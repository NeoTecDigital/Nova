// Written by Richard Christopher, Copyright 2026 NeoTec Digital
#include "./texture_bridge.h"
#include "Nova/components/logger.h"
#include <cstring>
#include <stdexcept>

namespace Nova {

namespace {

// Client-supplied extents are untrusted. wl_shm buffers are bounded only by the
// pool size the client mapped, so a hostile or broken client could otherwise ask
// for an allocation large enough to take the server down.
constexpr uint32_t MAX_TEXTURE_DIMENSION = 16384;
constexpr uint32_t BYTES_PER_PIXEL = 4;

bool extentIsSane(uint32_t width, uint32_t height) {
    return width > 0 && height > 0 &&
           width <= MAX_TEXTURE_DIMENSION && height <= MAX_TEXTURE_DIMENSION;
}

VkImageSubresourceRange colorSubresource() {
    return VkImageSubresourceRange{
        .aspectMask = VK_IMAGE_ASPECT_COLOR_BIT,
        .baseMipLevel = 0,
        .levelCount = 1,
        .baseArrayLayer = 0,
        .layerCount = 1
    };
}

// Whole-image copy from a tightly packed source: writeStagingPixels() packs the
// rows, so bufferRowLength/bufferImageHeight of 0 (== image extent) is correct.
VkBufferImageCopy wholeImageCopy(uint32_t width, uint32_t height) {
    return VkBufferImageCopy{
        .bufferOffset = 0,
        .bufferRowLength = 0,
        .bufferImageHeight = 0,
        .imageSubresource = {
            .aspectMask = VK_IMAGE_ASPECT_COLOR_BIT,
            .mipLevel = 0,
            .baseArrayLayer = 0,
            .layerCount = 1
        },
        .imageOffset = {0, 0, 0},
        .imageExtent = { width, height, 1 }
    };
}

} // namespace

TextureBridge::TextureBridge(Core* core)
    : core_(core) {
    if (!core_) {
        throw std::runtime_error("TextureBridge requires a valid NovaCore instance");
    }
    device_ = core_->getDevice();
    allocator_ = core_->getAllocator();

    // Uploads go through the graphics family (see uploadStagingToImage), so the
    // fragment shader stage is nameable there. Without a graphics family nothing
    // can sample these images anyway; ALL_COMMANDS is supported by every queue
    // type and keeps the barriers legal on the transfer fallback.
    sample_stage_ = core_->hasGraphicsImmediate() ? VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT
                                                  : VK_PIPELINE_STAGE_ALL_COMMANDS_BIT;
}

TextureBridge::~TextureBridge() {
    if (device_ == VK_NULL_HANDLE) return;

    releaseTexture(fallback_texture);

    if (default_sampler != VK_NULL_HANDLE) {
        vkDestroySampler(device_, default_sampler, nullptr);
        default_sampler = VK_NULL_HANDLE;
    }
    if (descriptor_pool != VK_NULL_HANDLE) {
        vkDestroyDescriptorPool(device_, descriptor_pool, nullptr);
        descriptor_pool = VK_NULL_HANDLE;
    }
    if (descriptor_set_layout != VK_NULL_HANDLE) {
        vkDestroyDescriptorSetLayout(device_, descriptor_set_layout, nullptr);
        descriptor_set_layout = VK_NULL_HANDLE;
    }
}

void TextureBridge::initialize() {
    report(LOGGER::INFO, "TextureBridge - Initializing Vulkan texture bridging subsystem...");
    createDescriptorSetLayout();
    createDescriptorPool();
    createDefaultSampler();
    createFallbackTexture();
    report(LOGGER::INFO, "TextureBridge - Initialized successfully");
}

void TextureBridge::createDescriptorSetLayout() {
    VkDescriptorSetLayoutBinding sampler_binding = {
        .binding = 0,
        .descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
        .descriptorCount = 1,
        .stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT,
        .pImmutableSamplers = nullptr
    };

    VkDescriptorSetLayoutCreateInfo layout_info = {
        .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO,
        .bindingCount = 1,
        .pBindings = &sampler_binding
    };

    VK_TRY(vkCreateDescriptorSetLayout(device_, &layout_info, nullptr, &descriptor_set_layout));
}

void TextureBridge::createDescriptorPool() {
    VkDescriptorPoolSize pool_sizes[] = {
        { VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1024 }
    };

    VkDescriptorPoolCreateInfo pool_info = {
        .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO,
        .flags = VK_DESCRIPTOR_POOL_CREATE_FREE_DESCRIPTOR_SET_BIT,
        .maxSets = 1024,
        .poolSizeCount = 1,
        .pPoolSizes = pool_sizes
    };

    VK_TRY(vkCreateDescriptorPool(device_, &pool_info, nullptr, &descriptor_pool));
}

void TextureBridge::createDefaultSampler() {
    VkSamplerCreateInfo sampler_info = {
        .sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO,
        .magFilter = VK_FILTER_LINEAR,
        .minFilter = VK_FILTER_LINEAR,
        .mipmapMode = VK_SAMPLER_MIPMAP_MODE_LINEAR,
        .addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,
        .addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,
        .addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,
        .mipLodBias = 0.0f,
        .anisotropyEnable = VK_FALSE,
        .maxAnisotropy = 1.0f,
        .compareEnable = VK_FALSE,
        .minLod = 0.0f,
        .maxLod = 0.0f,
        .borderColor = VK_BORDER_COLOR_FLOAT_OPAQUE_WHITE,
        .unnormalizedCoordinates = VK_FALSE
    };

    VK_TRY(vkCreateSampler(device_, &sampler_info, nullptr, &default_sampler));
}

VkDescriptorSet TextureBridge::allocateDescriptorSet(VkImageView image_view, VkSampler sampler) {
    VkDescriptorSetAllocateInfo alloc_info = {
        .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO,
        .descriptorPool = descriptor_pool,
        .descriptorSetCount = 1,
        .pSetLayouts = &descriptor_set_layout
    };

    VkDescriptorSet descriptor_set = VK_NULL_HANDLE;
    if (vkAllocateDescriptorSets(device_, &alloc_info, &descriptor_set) != VK_SUCCESS) {
        report(LOGGER::ERROR, "TextureBridge - Descriptor set pool exhausted");
        return VK_NULL_HANDLE;
    }

    VkDescriptorImageInfo image_info = {
        .sampler = sampler ? sampler : default_sampler,
        .imageView = image_view,
        .imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL
    };

    VkWriteDescriptorSet descriptor_write = {
        .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
        .dstSet = descriptor_set,
        .dstBinding = 0,
        .dstArrayElement = 0,
        .descriptorCount = 1,
        .descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
        .pImageInfo = &image_info
    };

    vkUpdateDescriptorSets(device_, 1, &descriptor_write, 0, nullptr);
    return descriptor_set;
}

void TextureBridge::createFallbackTexture() {
    uint32_t white_pixel = 0xFFFFFFFF;
    fallback_texture = createTextureFromRGBA(reinterpret_cast<const uint8_t*>(&white_pixel), 1, 1);
    if (!fallback_texture) {
        report(LOGGER::ERROR, "TextureBridge - Failed to create the fallback texture");
    }
}

VkFormat TextureBridge::vulkanFormat(PixelLayout layout) {
    // Vulkan component names are written in byte order for non-packed formats,
    // which is the same convention PixelLayout uses, so this is a direct map.
    return (layout == PixelLayout::RGBA8) ? VK_FORMAT_R8G8B8A8_UNORM
                                          : VK_FORMAT_B8G8R8A8_UNORM;
}

VkComponentMapping TextureBridge::componentMapping(PixelLayout layout) {
    // XRGB8888's fourth byte is undefined: force alpha to 1 in the view rather
    // than paying a per-pixel CPU fixup on every client commit.
    VkComponentSwizzle alpha = (layout == PixelLayout::BGRX8) ? VK_COMPONENT_SWIZZLE_ONE
                                                              : VK_COMPONENT_SWIZZLE_IDENTITY;
    return VkComponentMapping{
        .r = VK_COMPONENT_SWIZZLE_IDENTITY,
        .g = VK_COMPONENT_SWIZZLE_IDENTITY,
        .b = VK_COMPONENT_SWIZZLE_IDENTITY,
        .a = alpha
    };
}

std::shared_ptr<TextureHandle> TextureBridge::allocateTexture(uint32_t width, uint32_t height, PixelLayout layout) {
    if (!extentIsSane(width, height)) {
        report(LOGGER::ERROR, "TextureBridge - Rejected texture extent %ux%u", width, height);
        return nullptr;
    }

    VkImageCreateInfo image_info = {
        .sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
        .imageType = VK_IMAGE_TYPE_2D,
        .format = vulkanFormat(layout),
        .extent = { width, height, 1 },
        .mipLevels = 1,
        .arrayLayers = 1,
        .samples = VK_SAMPLE_COUNT_1_BIT,
        .tiling = VK_IMAGE_TILING_OPTIMAL,
        .usage = VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_SAMPLED_BIT,
        // EXCLUSIVE with no ownership transfer anywhere in this file, which holds
        // only because upload and sampling happen on the same graphics family.
        .sharingMode = VK_SHARING_MODE_EXCLUSIVE,
        .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED
    };

    VmaAllocationCreateInfo alloc_info = { .usage = VMA_MEMORY_USAGE_GPU_ONLY };

    auto handle = std::make_shared<TextureHandle>();
    handle->width = width;
    handle->height = height;
    handle->layout = layout;
    handle->owns_image = true;

    if (vmaCreateImage(allocator_, &image_info, &alloc_info, &handle->image, &handle->allocation, nullptr) != VK_SUCCESS) {
        report(LOGGER::ERROR, "TextureBridge - Failed to allocate a %ux%u image", width, height);
        return nullptr;
    }

    if (!createTextureView(*handle, image_info.format)) {
        vmaDestroyImage(allocator_, handle->image, handle->allocation);
        handle->image = VK_NULL_HANDLE;
        return nullptr;
    }

    handle->sampler = default_sampler;
    handle->descriptor_set = allocateDescriptorSet(handle->image_view, handle->sampler);
    return handle;
}

bool TextureBridge::createTextureView(TextureHandle& handle, VkFormat format) {
    VkImageViewCreateInfo view_info = {
        .sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,
        .image = handle.image,
        .viewType = VK_IMAGE_VIEW_TYPE_2D,
        .format = format,
        .components = componentMapping(handle.layout),
        .subresourceRange = colorSubresource()
    };

    if (vkCreateImageView(device_, &view_info, nullptr, &handle.image_view) != VK_SUCCESS) {
        report(LOGGER::ERROR, "TextureBridge - Failed to create an image view");
        return false;
    }
    return true;
}

bool TextureBridge::ensureStagingBuffer(TextureHandle& handle, VkDeviceSize required) {
    if (handle.staging_buffer != VK_NULL_HANDLE && handle.staging_size >= required) {
        return true;
    }
    releaseStagingBuffer(handle);

    Buffer_T staging = core_->createEphemeralBuffer(
        static_cast<size_t>(required),
        VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
        VMA_MEMORY_USAGE_CPU_ONLY
    );
    if (staging.buffer == VK_NULL_HANDLE || staging.allocation == VK_NULL_HANDLE) {
        report(LOGGER::ERROR, "TextureBridge - Failed to allocate a %llu byte staging buffer",
               static_cast<unsigned long long>(required));
        return false;
    }

    handle.staging_buffer = staging.buffer;
    handle.staging_allocation = staging.allocation;
    handle.staging_size = required;
    return true;
}

void TextureBridge::releaseStagingBuffer(TextureHandle& handle) {
    if (handle.staging_buffer == VK_NULL_HANDLE) return;
    vmaDestroyBuffer(allocator_, handle.staging_buffer, handle.staging_allocation);
    handle.staging_buffer = VK_NULL_HANDLE;
    handle.staging_allocation = VK_NULL_HANDLE;
    handle.staging_size = 0;
}

bool TextureBridge::writeStagingPixels(TextureHandle& handle, const uint8_t* pixels, uint32_t stride) {
    const size_t packed_row = static_cast<size_t>(handle.width) * BYTES_PER_PIXEL;

    void* mapped = nullptr;
    if (vmaMapMemory(allocator_, handle.staging_allocation, &mapped) != VK_SUCCESS || !mapped) {
        report(LOGGER::ERROR, "TextureBridge - Failed to map the staging buffer");
        return false;
    }

    if (stride == packed_row) {
        std::memcpy(mapped, pixels, packed_row * handle.height);
    } else {
        // Strided source: copy row by row so bytes past the visible width are
        // never read, and the destination stays tightly packed for the copy.
        auto dst = static_cast<uint8_t*>(mapped);
        for (uint32_t row = 0; row < handle.height; ++row) {
            std::memcpy(dst + row * packed_row, pixels + static_cast<size_t>(row) * stride, packed_row);
        }
    }

    vmaUnmapMemory(allocator_, handle.staging_allocation);
    return true;
}

void TextureBridge::uploadStagingToImage(TextureHandle& handle) {
    const uint32_t width = handle.width;
    const uint32_t height = handle.height;
    const VkImage image = handle.image;
    const VkBuffer staging = handle.staging_buffer;
    const VkPipelineStageFlags sample_stage = sample_stage_;

    core_->immediateSubmitGraphics([image, staging, width, height, sample_stage](VkCommandBuffer cmd) {
        // Entry barrier, two jobs. Layout: the whole image is overwritten, so
        // discarding from UNDEFINED is legal and cheaper than preserving.
        // Ordering: an image already published to the scene may still be read by
        // frames submitted earlier on this queue, and the copy must not start
        // until those reads retire. Read-then-write needs an execution
        // dependency only, hence srcAccessMask = 0.
        VkImageMemoryBarrier barrier = {
            .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
            .srcAccessMask = 0,
            .dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT,
            .oldLayout = VK_IMAGE_LAYOUT_UNDEFINED,
            .newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
            .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
            .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
            .image = image,
            .subresourceRange = colorSubresource()
        };
        vkCmdPipelineBarrier(cmd, sample_stage, VK_PIPELINE_STAGE_TRANSFER_BIT,
                             0, 0, nullptr, 0, nullptr, 1, &barrier);

        VkBufferImageCopy copy_region = wholeImageCopy(width, height);
        vkCmdCopyBufferToImage(cmd, staging, image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &copy_region);

        barrier.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
        barrier.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        barrier.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
        barrier.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
        vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT, sample_stage,
                             0, 0, nullptr, 0, nullptr, 1, &barrier);
    });
}

std::shared_ptr<TextureHandle> TextureBridge::createTextureFromRGBA(const uint8_t* pixels, uint32_t width, uint32_t height) {
    if (!pixels) return nullptr;

    auto handle = allocateTexture(width, height, PixelLayout::RGBA8);
    if (!handle) return nullptr;

    const VkDeviceSize image_size = static_cast<VkDeviceSize>(width) * height * BYTES_PER_PIXEL;
    if (!ensureStagingBuffer(*handle, image_size) ||
        !writeStagingPixels(*handle, pixels, width * BYTES_PER_PIXEL)) {
        releaseTexture(handle);
        return nullptr;
    }

    uploadStagingToImage(*handle);

    // One-shot texture: nothing will feed it again, so the staging memory goes
    // back immediately. Only client surfaces keep their staging buffer.
    releaseStagingBuffer(*handle);
    return handle;
}

bool TextureBridge::updateTextureFromRGBA(std::shared_ptr<TextureHandle>& handle,
                                          const uint8_t* pixels,
                                          uint32_t stride,
                                          uint32_t width,
                                          uint32_t height,
                                          PixelLayout layout) {
    if (!pixels || !extentIsSane(width, height)) return false;
    if (stride < static_cast<uint32_t>(width) * BYTES_PER_PIXEL) {
        report(LOGGER::ERROR, "TextureBridge - Source stride %u is short for width %u", stride, width);
        return false;
    }

    if (handle && !handle->matches(width, height, layout)) {
        // Extent or layout changed: the old image cannot be reused, and freeing
        // it is a destroy, not an overwrite - no barrier can order a command
        // buffer against memory that has ceased to exist. releaseTexture() idles
        // the device for exactly that reason. Resizes are rare, so this is the
        // only place the stall is paid; a steady-state commit never reaches it.
        releaseTexture(handle);
    }

    if (!handle) {
        handle = allocateTexture(width, height, layout);
        if (!handle) return false;
    }

    const VkDeviceSize image_size = static_cast<VkDeviceSize>(width) * height * BYTES_PER_PIXEL;
    if (!ensureStagingBuffer(*handle, image_size) || !writeStagingPixels(*handle, pixels, stride)) {
        return false;
    }

    uploadStagingToImage(*handle);
    return true;
}

void TextureBridge::releaseTexture(std::shared_ptr<TextureHandle>& handle) {
    if (!handle) return;

    // The descriptor set and image may still be bound by command buffers that
    // have not retired. Nothing here is safe until the device is idle.
    if (device_ != VK_NULL_HANDLE) {
        vkDeviceWaitIdle(device_);
    }

    releaseStagingBuffer(*handle);

    if (handle->descriptor_set != VK_NULL_HANDLE && descriptor_pool != VK_NULL_HANDLE) {
        vkFreeDescriptorSets(device_, descriptor_pool, 1, &handle->descriptor_set);
        handle->descriptor_set = VK_NULL_HANDLE;
    }
    if (handle->owns_image && handle->image_view != VK_NULL_HANDLE) {
        vkDestroyImageView(device_, handle->image_view, nullptr);
        handle->image_view = VK_NULL_HANDLE;
    }
    if (handle->owns_image && handle->image != VK_NULL_HANDLE) {
        vmaDestroyImage(allocator_, handle->image, handle->allocation);
        handle->image = VK_NULL_HANDLE;
        handle->allocation = VK_NULL_HANDLE;
    }

    handle.reset();
}

std::shared_ptr<TextureHandle> TextureBridge::registerExternalImageView(VkImageView image_view, VkSampler sampler) {
    auto handle = std::make_shared<TextureHandle>();
    handle->image = VK_NULL_HANDLE;
    handle->image_view = image_view;
    handle->sampler = sampler ? sampler : default_sampler;
    handle->allocation = VK_NULL_HANDLE;
    handle->owns_image = false;
    handle->descriptor_set = allocateDescriptorSet(image_view, handle->sampler);
    return handle;
}

} // namespace Nova
