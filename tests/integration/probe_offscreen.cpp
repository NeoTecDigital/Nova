// Written by Richard Christopher, Copyright 2026 NeoTec Digital
//
// Offscreen Graphics acceptance probe, and the direct reproducer for the
// teardown use-after-free QA5 found: an optimized build used to render 18/18
// checks correctly and then die inside ~Core, because the cleanup lambda
// Graphics registered in the BASE registry touched DERIVED members that had
// already been destroyed.
//
// The teardown is therefore the last and most important assertion in the file.
// It is deliberately NOT wrapped in anything: the process exit code is the
// check. rc 0 = the registry entry ran once, at the right level, on live state.
#include "Core/nova_graphics.h"
#include "Core/components/logger.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <string>
#include <vector>

namespace {

int g_checks = 0;
int g_failures = 0;

void check(bool ok, const char* what) {
    ++g_checks;
    if (!ok) ++g_failures;
    std::printf("[%s] %s\n", ok ? " ok " : "FAIL", what);
    std::fflush(stdout);
}

struct OwnedImage {
    VkImage image = VK_NULL_HANDLE;
    VmaAllocation allocation = VK_NULL_HANDLE;
    VkImageView view = VK_NULL_HANDLE;
};

bool createOwnedImage(Nova::Graphics& nova, VkExtent2D extent, VkFormat format, OwnedImage& out) {
    VkImageCreateInfo image_info = {
        .sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
        .imageType = VK_IMAGE_TYPE_2D,
        .format = format,
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
    if (vmaCreateImage(nova.getAllocator(), &image_info, &alloc_info, &out.image,
                       &out.allocation, nullptr) != VK_SUCCESS) {
        return false;
    }

    VkImageViewCreateInfo view_info = {
        .sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,
        .image = out.image,
        .viewType = VK_IMAGE_VIEW_TYPE_2D,
        .format = format,
        .subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 }
    };
    return vkCreateImageView(nova.getDevice(), &view_info, nullptr, &out.view) == VK_SUCCESS;
}

void destroyOwnedImage(Nova::Graphics& nova, OwnedImage& owned) {
    if (owned.view != VK_NULL_HANDLE) {
        vkDestroyImageView(nova.getDevice(), owned.view, nullptr);
        owned.view = VK_NULL_HANDLE;
    }
    if (owned.image != VK_NULL_HANDLE) {
        vmaDestroyImage(nova.getAllocator(), owned.image, owned.allocation);
        owned.image = VK_NULL_HANDLE;
        owned.allocation = VK_NULL_HANDLE;
    }
}

// One render + readback, so the checks cover a real submission rather than only
// object construction. Returns the clear colour the GPU actually wrote.
bool renderAndReadBack(Nova::Graphics& nova, const Nova::RenderTarget& target, uint32_t& pixel_out) {
    VkFence fence = nova.renderToImage(target, [](VkCommandBuffer, uint32_t) {});
    if (fence == VK_NULL_HANDLE || !nova.waitForRender(fence)) return false;

    const size_t bytes = static_cast<size_t>(target.extent.width) * target.extent.height * 4;
    Nova::Buffer_T readback = nova.createEphemeralBuffer(bytes, VK_BUFFER_USAGE_TRANSFER_DST_BIT,
                                                   VMA_MEMORY_USAGE_CPU_ONLY);
    if (readback.allocation == VK_NULL_HANDLE) return false;

    VkBufferImageCopy region = {
        .bufferOffset = 0, .bufferRowLength = 0, .bufferImageHeight = 0,
        .imageSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 },
        .imageOffset = {0, 0, 0},
        .imageExtent = { target.extent.width, target.extent.height, 1 }
    };
    VkImage image = target.image;
    VkBuffer buffer = readback.buffer;
    nova.immediateSubmitGraphics([image, buffer, region](VkCommandBuffer cmd) {
        vkCmdCopyImageToBuffer(cmd, image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, buffer, 1, &region);
    });

    void* mapped = nullptr;
    bool ok = false;
    if (vmaMapMemory(nova.getAllocator(), readback.allocation, &mapped) == VK_SUCCESS && mapped) {
        vmaInvalidateAllocation(nova.getAllocator(), readback.allocation, 0, VK_WHOLE_SIZE);
        std::memcpy(&pixel_out, mapped, sizeof(pixel_out));
        vmaUnmapMemory(nova.getAllocator(), readback.allocation);
        ok = true;
    }
    vmaDestroyBuffer(nova.getAllocator(), readback.buffer, readback.allocation);
    return ok;
}

} // namespace

int main() {
    const std::string debug_level = "ERROR";

    Nova::OffscreenConfig config = {};
    config.extent = { 640, 360 };
    config.request_dmabuf_import = true;

    // Scoped so the destructor runs BEFORE the summary is printed: a teardown
    // that aborts must not be able to print a passing summary first.
    {
        auto nova = std::make_unique<Nova::Graphics>(config, debug_level);

        check(nova->getDevice() != VK_NULL_HANDLE, "offscreen instance has a logical device");
        check(nova->getAllocator() != VK_NULL_HANDLE, "offscreen instance has a VMA allocator");
        check(nova->hasGraphicsImmediate(), "graphics-family immediate context is engaged");

        const VkFormat format = VK_FORMAT_B8G8R8A8_UNORM;
        VkRenderPass pass = nova->getOffscreenRenderPass(format, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
        check(pass != VK_NULL_HANDLE, "offscreen render pass created for BGRA/TRANSFER_SRC");

        VkRenderPass again = nova->getOffscreenRenderPass(format, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
        check(again == pass, "render pass is cached, not rebuilt per call");

        VkRenderPass general = nova->getOffscreenRenderPass(format, VK_IMAGE_LAYOUT_GENERAL);
        check(general != VK_NULL_HANDLE, "second render pass created for BGRA/GENERAL");
        check(general != pass, "final layout is part of the render pass cache key");

        OwnedImage owned = {};
        check(createOwnedImage(*nova, config.extent, format, owned), "owned colour target allocated");
        check(owned.view != VK_NULL_HANDLE, "owned colour target has an image view");

        Nova::RenderTarget target = {};
        target.image = owned.image;
        target.view = owned.view;
        target.extent = config.extent;
        target.format = format;
        target.final_layout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
        target.clear = {{{ 0.0f, 0.5f, 1.0f, 1.0f }}};
        check(target.valid(), "render target passes its own validity check");

        uint32_t pixel = 0;
        check(renderAndReadBack(*nova, target, pixel), "renderToImage + readback completed");
        // BGRA8 UNORM: B=0xFF, G=0x80, R=0x00 for (0.0, 0.5, 1.0). Little-endian
        // load makes that 0xFF80xx00 with A=0xFF.
        check((pixel & 0x000000FFu) == 0x000000FFu, "blue channel cleared to 1.0");
        check((pixel & 0x00FF0000u) == 0x00000000u, "red channel cleared to 0.0");

        // Cached framebuffer reuse: a second frame through the same view must
        // not churn objects, which is the whole reason the cache exists.
        uint32_t pixel_again = 0;
        check(renderAndReadBack(*nova, target, pixel_again), "second frame through the same view");
        check(pixel_again == pixel, "second frame produced identical pixels");

        Nova::RenderTarget invalid = {};
        check(nova->renderToImage(invalid, [](VkCommandBuffer, uint32_t) {}) == VK_NULL_HANDLE,
              "renderToImage rejects an invalid target");

        // Imports are the other half of what the offscreen cleanup entry owns.
        // Reported either way: absence is not a failure, it is a capability.
        std::printf("[info] dmabuf import: %s\n", nova->supportsDmabufImport() ? "available" : "unavailable");
        check(true, "dmabuf import capability reported");

        check(nova->isOffscreen(), "instance reports offscreen mode");
        check(nova->getRenderPass() == VK_NULL_HANDLE, "swapchain render pass stays null offscreen");
        check(nova->getWindowExtent().width == config.extent.width,
              "window extent reports the configured offscreen extent");

        // Nothing may be in flight when the attachment goes: the cached
        // framebuffer built over this view outlives it, which is legal only
        // because no submission still references either.
        vkDeviceWaitIdle(nova->getDevice());
        destroyOwnedImage(*nova, owned);
        check(true, "owned colour target released after the device went idle");

        std::printf("[info] tearing down NovaGraphics (the P0 reproducer)\n");
        std::fflush(stdout);
        nova.reset();
        check(true, "NovaGraphics destroyed without aborting");
    }

    std::printf("\nprobe_offscreen: %d/%d checks passed, %d failures\n",
                g_checks - g_failures, g_checks, g_failures);
    std::fflush(stdout);
    return g_failures == 0 ? 0 : 1;
}
