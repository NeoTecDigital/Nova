// Written by Richard Christopher, Copyright 2026 NeoTec Digital
//
// The CPU-copy half of SpatialPresentLoop: what runs when the output's buffers
// cannot be imported into Nova (plan D.2, fallback). A pixman renderer of this
// loop's own feeds wlr_allocator_autocreate, which yields a MAPPABLE shm
// swapchain the output accepts even though it was initialised with a GPU
// renderer - spike-verified, and the only public-API route to a mappable
// buffer, since wlroots' allocator selection order is not public.
//
// One copy per frame: Nova renders into an image it owns, the result is read
// back into a host-visible buffer, and the rows are written into the mapped
// scanout buffer. Separate translation unit from the loop proper purely for the
// 500-line file cap; it is the same class and the same object.
#include "../../include/Clouds/SpatialPresentLoop.h"
#include "../../Core/components/logger.h"

#include <cstring>

namespace Clouds {


bool SpatialPresentLoop::ensureSidecar(Output& bound, struct wlr_output* output) {
    if (!sidecar_renderer_) {
        sidecar_renderer_ = wlr_pixman_renderer_create();
        if (!sidecar_renderer_) {
            report(LOGGER::ERROR, "SpatialPresentLoop - Failed to create the pixman sidecar renderer");
            return false;
        }
        sidecar_allocator_ = wlr_allocator_autocreate(output->backend, sidecar_renderer_);
        if (!sidecar_allocator_) {
            // A DRM backend advertises DMABUF only and rejects SHM, so this is
            // where a real connector without an importable buffer lands. Say so
            // plainly instead of failing three frames later.
            report(LOGGER::ERROR, "SpatialPresentLoop - Backend rejects a mappable allocator: "
                                  "no CPU-copy path exists for this output");
            return false;
        }
    }

    return createSidecarSwapchain(bound);
}

// The swapchain and the loop's single render target, both sized from the
// output's CURRENT extent. Replacing an existing swapchain is the mode-change
// path; creating the first one is attach().
bool SpatialPresentLoop::createSidecarSwapchain(Output& bound) {
    if (!bound.output || !sidecar_allocator_) return false;

    if (bound.sidecar_swapchain) {
        wlr_swapchain_destroy(bound.sidecar_swapchain);
        bound.sidecar_swapchain = nullptr;
    }
    bound.sidecar_extent = {0, 0};

    struct wlr_drm_format format = { .format = kPresentFallbackDrmFormat, .len = 0, .capacity = 0,
                                     .modifiers = nullptr };
    bound.sidecar_swapchain = wlr_swapchain_create(sidecar_allocator_, bound.output->width,
                                                   bound.output->height, &format);
    if (!bound.sidecar_swapchain) {
        report(LOGGER::ERROR, "SpatialPresentLoop - Failed to create the mappable sidecar swapchain");
        return false;
    }

    const VkExtent2D extent = { static_cast<uint32_t>(bound.output->width),
                                static_cast<uint32_t>(bound.output->height) };
    if (!ensureSidecarTarget(extent)) return false;
    bound.sidecar_extent = extent;
    return true;
}

// A mode change invalidated both the swapchain and the render target. The
// render pass is untouched - the format never changes - so every pipeline built
// against it stays valid across the rebuild.
bool SpatialPresentLoop::rebuildSidecar(Output& bound) {
    bound.sidecar_needs_rebuild = false;
    if (path_ != PresentPath::PixmanSidecar) return true;

    if (!createSidecarSwapchain(bound)) {
        report(LOGGER::ERROR, "SpatialPresentLoop - Sidecar rebuild failed for output '%s' at %dx%d",
               bound.output && bound.output->name ? bound.output->name : "unnamed",
               bound.output ? bound.output->width : 0, bound.output ? bound.output->height : 0);
        return false;
    }
    report(LOGGER::INFO, "SpatialPresentLoop - Sidecar rebuilt at %ux%u",
           bound.sidecar_extent.width, bound.sidecar_extent.height);
    return true;
}

bool SpatialPresentLoop::ensureSidecarTarget(VkExtent2D extent) {
    if (sidecar_target_.valid() && sidecar_target_.extent.width == extent.width &&
        sidecar_target_.extent.height == extent.height) {
        return true;
    }
    destroySidecarTarget();
    if (!createSidecarImage(extent)) return false;

    sidecar_target_.readback = graphics_->createEphemeralBuffer(
        static_cast<size_t>(extent.width) * extent.height * 4,
        VK_BUFFER_USAGE_TRANSFER_DST_BIT, VMA_MEMORY_USAGE_CPU_ONLY);
    sidecar_target_.extent = extent;
    if (sidecar_target_.readback.allocation == VK_NULL_HANDLE ||
        vmaMapMemory(graphics_->getAllocator(), sidecar_target_.readback.allocation,
                     &sidecar_target_.readback_mapped) != VK_SUCCESS) {
        report(LOGGER::ERROR, "SpatialPresentLoop - Sidecar readback buffer could not be mapped");
        return false;
    }
    report(LOGGER::INFO, "SpatialPresentLoop - Sidecar target ready at %ux%u",
           extent.width, extent.height);
    return true;
}

// Device-local colour attachment Nova renders the frame into, before the copy
// out. OPTIMAL tiling deliberately: a linear image that is also a colour
// attachment is not something every driver supports, and the copy is needed
// either way to reach the shm buffer.
bool SpatialPresentLoop::createSidecarImage(VkExtent2D extent) {
    VkImageCreateInfo image_info = {
        .sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
        .imageType = VK_IMAGE_TYPE_2D,
        .format = target_format_,
        .extent = { extent.width, extent.height, 1 },
        .mipLevels = 1,
        .arrayLayers = 1,
        .samples = VK_SAMPLE_COUNT_1_BIT,
        .tiling = VK_IMAGE_TILING_OPTIMAL,
        .usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT,
        .sharingMode = VK_SHARING_MODE_EXCLUSIVE,
        .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED
    };
    VmaAllocationCreateInfo alloc_info = {};
    alloc_info.usage = VMA_MEMORY_USAGE_GPU_ONLY;
    if (vmaCreateImage(graphics_->getAllocator(), &image_info, &alloc_info, &sidecar_target_.image,
                       &sidecar_target_.allocation, nullptr) != VK_SUCCESS) {
        report(LOGGER::ERROR, "SpatialPresentLoop - Failed to allocate the sidecar render target");
        return false;
    }

    VkImageViewCreateInfo view_info = {
        .sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,
        .image = sidecar_target_.image,
        .viewType = VK_IMAGE_VIEW_TYPE_2D,
        .format = target_format_,
        .subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 }
    };
    if (vkCreateImageView(graphics_->getDevice(), &view_info, nullptr, &sidecar_target_.view) != VK_SUCCESS) {
        report(LOGGER::ERROR, "SpatialPresentLoop - Failed to create the sidecar target view");
        return false;
    }
    return true;
}

void SpatialPresentLoop::destroySidecarTarget() {
    if (!graphics_) return;
    if (sidecar_target_.image != VK_NULL_HANDLE || sidecar_target_.view != VK_NULL_HANDLE) {
        vkDeviceWaitIdle(graphics_->getDevice());
    }
    if (sidecar_target_.view != VK_NULL_HANDLE) {
        vkDestroyImageView(graphics_->getDevice(), sidecar_target_.view, nullptr);
        sidecar_target_.view = VK_NULL_HANDLE;
    }
    if (sidecar_target_.image != VK_NULL_HANDLE) {
        vmaDestroyImage(graphics_->getAllocator(), sidecar_target_.image, sidecar_target_.allocation);
        sidecar_target_.image = VK_NULL_HANDLE;
        sidecar_target_.allocation = VK_NULL_HANDLE;
    }
    if (sidecar_target_.readback_mapped) {
        vmaUnmapMemory(graphics_->getAllocator(), sidecar_target_.readback.allocation);
        sidecar_target_.readback_mapped = nullptr;
    }
    if (sidecar_target_.readback.buffer != VK_NULL_HANDLE) {
        vmaDestroyBuffer(graphics_->getAllocator(), sidecar_target_.readback.buffer,
                         sidecar_target_.readback.allocation);
        sidecar_target_.readback = {};
    }
    sidecar_target_.extent = {0, 0};
}

bool SpatialPresentLoop::renderIntoSidecarTarget() {
    NovaRenderTarget target = {};
    target.image = sidecar_target_.image;
    target.view = sidecar_target_.view;
    target.extent = sidecar_target_.extent;
    target.format = target_format_;
    target.final_layout = target_layout_;

    const VkExtent2D extent = target.extent;
    VkFence fence = graphics_->renderToImage(target, [this, extent](VkCommandBuffer cmd, uint32_t) {
        scene_renderer_(cmd, extent);
    });
    if (fence == VK_NULL_HANDLE || !graphics_->waitForRender(fence)) return false;

    // The pass left the image in TRANSFER_SRC_OPTIMAL, so the copy needs no
    // barrier of its own; the submission is on the graphics family, the same
    // one the render used, so there is no ownership transfer either.
    VkBufferImageCopy region = {
        .bufferOffset = 0, .bufferRowLength = 0, .bufferImageHeight = 0,
        .imageSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 },
        .imageOffset = {0, 0, 0},
        .imageExtent = { extent.width, extent.height, 1 }
    };
    VkImage image = sidecar_target_.image;
    VkBuffer readback = sidecar_target_.readback.buffer;
    graphics_->immediateSubmitGraphics([image, readback, region](VkCommandBuffer cmd) {
        vkCmdCopyImageToBuffer(cmd, image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, readback, 1, &region);
    });

    // The submit has already retired, but the mapping may be non-coherent; the
    // invalidate is what makes the copy visible to this CPU read.
    vmaInvalidateAllocation(graphics_->getAllocator(), sidecar_target_.readback.allocation,
                            0, VK_WHOLE_SIZE);
    return true;
}

bool SpatialPresentLoop::presentSidecar(Output&, struct wlr_buffer* buffer) {
    if (!sidecar_target_.valid() || !renderIntoSidecarTarget()) return false;

    void* pixels = nullptr;
    uint32_t format = 0;
    size_t stride = 0;
    if (!wlr_buffer_begin_data_ptr_access(buffer, WLR_BUFFER_DATA_PTR_ACCESS_WRITE,
                                          &pixels, &format, &stride)) {
        report(LOGGER::ERROR, "SpatialPresentLoop - Sidecar buffer refused a write mapping");
        return false;
    }

    // Both ends are 32-bit BGRA-order rows; only the row pitch may differ.
    const uint32_t width = sidecar_target_.extent.width;
    const uint32_t height = sidecar_target_.extent.height;
    const size_t src_stride = static_cast<size_t>(width) * 4;

    // The destination is sized by the wlr_buffer, the source by this loop's
    // one shared target. attach() refuses a second sidecar output and
    // rebuildSidecar() resizes both together, so these agree - and this is what
    // makes that a checked fact rather than an assumption, because the cost of
    // being wrong is a heap overflow inside a scanout buffer.
    if (buffer->width < 0 || buffer->height < 0 ||
        static_cast<uint32_t>(buffer->width) < width ||
        static_cast<uint32_t>(buffer->height) < height || stride < src_stride) {
        report(LOGGER::ERROR,
               "SpatialPresentLoop - Sidecar buffer %dx%d (stride %zu) is smaller than the %ux%u "
               "target; refusing the copy",
               buffer->width, buffer->height, stride, width, height);
        wlr_buffer_end_data_ptr_access(buffer);
        return false;
    }

    const auto* src = static_cast<const uint8_t*>(sidecar_target_.readback_mapped);
    auto* dst = static_cast<uint8_t*>(pixels);
    for (uint32_t row = 0; row < height; ++row) {
        std::memcpy(dst + static_cast<size_t>(row) * stride, src + row * src_stride, src_stride);
    }
    wlr_buffer_end_data_ptr_access(buffer);
    return true;
}

} // namespace Clouds
