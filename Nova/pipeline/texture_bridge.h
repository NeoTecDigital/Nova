// Written by Richard Christopher, Copyright 2026 NeoTec Digital
#pragma once

#include "Nova/core_base.h"
#include <vulkan/vulkan.h>
#include <vector>
#include <memory>
#include <unordered_map>

namespace Nova {

/**
 * Byte order of a source pixel row, named in MEMORY order (byte 0 first).
 *
 * wl_shm mandates DRM_FORMAT_ARGB8888 and DRM_FORMAT_XRGB8888. Both are
 * little-endian 32-bit words with A/X in the high byte, so their in-memory byte
 * order is B,G,R,A and B,G,R,X respectively - i.e. VK_FORMAT_B8G8R8A8_UNORM,
 * whose Vulkan component names are also written in byte order. No CPU channel
 * swizzle is needed for either; XRGB only differs in that its fourth byte is
 * undefined, which the image view neutralises with an A -> ONE swizzle.
 */
enum class PixelLayout : uint8_t {
    RGBA8,  // byte0=R byte1=G byte2=B byte3=A -> VK_FORMAT_R8G8B8A8_UNORM
    BGRA8,  // byte0=B byte1=G byte2=R byte3=A -> VK_FORMAT_B8G8R8A8_UNORM
    BGRX8   // byte0=B byte1=G byte2=R byte3=X -> VK_FORMAT_B8G8R8A8_UNORM, alpha forced to 1
};

struct TextureHandle {
    VkImage image = VK_NULL_HANDLE;
    VkImageView image_view = VK_NULL_HANDLE;
    VkSampler sampler = VK_NULL_HANDLE;
    VmaAllocation allocation = VK_NULL_HANDLE;
    VkDescriptorSet descriptor_set = VK_NULL_HANDLE;
    uint32_t width = 0;
    uint32_t height = 0;
    bool owns_image = true;

    // Persistent upload staging, retained only by textures fed from a live
    // client surface. One-shot textures release it right after creation.
    VkBuffer staging_buffer = VK_NULL_HANDLE;
    VmaAllocation staging_allocation = VK_NULL_HANDLE;
    VkDeviceSize staging_size = 0;

    PixelLayout layout = PixelLayout::RGBA8;

    bool matches(uint32_t w, uint32_t h, PixelLayout l) const {
        return width == w && height == h && layout == l;
    }
};

class TextureBridge {
public:
    TextureBridge(Core* core);
    ~TextureBridge();

    TextureBridge(const TextureBridge&) = delete;
    TextureBridge& operator=(const TextureBridge&) = delete;

    // Initialize descriptor pool and default white fallback texture
    void initialize();

    // Create a texture from raw RGBA8 pixel data (e.g. font atlas, icon, image).
    // One-shot: the staging buffer is released before returning.
    std::shared_ptr<TextureHandle> createTextureFromRGBA(const uint8_t* pixels, uint32_t width, uint32_t height);

    /**
     * Upload a client pixel rectangle into a persistent, reused image.
     *
     * The image, its view, its descriptor set and its staging buffer are all
     * retained across calls; only a change of extent or pixel layout forces a
     * reallocation. `stride` is the source row pitch in BYTES and may exceed
     * width * 4.
     *
     * Returns false and leaves the handle untouched when the upload could not
     * be performed. On success `handle` always references a live texture.
     */
    bool updateTextureFromRGBA(std::shared_ptr<TextureHandle>& handle,
                               const uint8_t* pixels,
                               uint32_t stride,
                               uint32_t width,
                               uint32_t height,
                               PixelLayout layout);

    /**
     * Destroy every GPU object the handle owns and reset it to null.
     *
     * Waits for the device to go idle first. Unlike an overwrite, a destroy
     * cannot be ordered by a barrier: the descriptor set and the image are freed
     * outright, and any command buffer still in flight that references them
     * would be left pointing at nothing.
     */
    void releaseTexture(std::shared_ptr<TextureHandle>& handle);

    // Register an external VkImageView (e.g. from wlroots wlr_texture, DMA-BUF, or another Vulkan process)
    std::shared_ptr<TextureHandle> registerExternalImageView(VkImageView image_view, VkSampler sampler = VK_NULL_HANDLE);

    // Get the shared descriptor set layout (binding 0: combined image sampler)
    VkDescriptorSetLayout getDescriptorSetLayout() const { return descriptor_set_layout; }

    // Get the fallback white texture (for untextured / procedural SDF surfaces)
    std::shared_ptr<TextureHandle> getFallbackTexture() const { return fallback_texture; }

    static VkFormat vulkanFormat(PixelLayout layout);
    static VkComponentMapping componentMapping(PixelLayout layout);

private:
    Core* core_ = nullptr;
    VkDevice device_ = VK_NULL_HANDLE;
    VmaAllocator allocator_ = VK_NULL_HANDLE;

    VkDescriptorSetLayout descriptor_set_layout = VK_NULL_HANDLE;
    VkDescriptorPool descriptor_pool = VK_NULL_HANDLE;
    VkSampler default_sampler = VK_NULL_HANDLE;

    // Stage the upload barriers name as the reader of these images. Fixed at
    // construction from the upload queue's family: FRAGMENT_SHADER is what
    // actually samples them, but a device with no graphics family cannot name it
    // on any queue it has, and falls back to the always-supported ALL_COMMANDS.
    VkPipelineStageFlags sample_stage_ = VK_PIPELINE_STAGE_ALL_COMMANDS_BIT;

    std::shared_ptr<TextureHandle> fallback_texture;

    void createDescriptorSetLayout();
    void createDescriptorPool();
    void createDefaultSampler();
    void createFallbackTexture();

    VkDescriptorSet allocateDescriptorSet(VkImageView image_view, VkSampler sampler);

    // Allocate image + view + descriptor set. Returns null on failure.
    std::shared_ptr<TextureHandle> allocateTexture(uint32_t width, uint32_t height, PixelLayout layout);
    bool createTextureView(TextureHandle& handle, VkFormat format);

    // Grow the handle's staging buffer to at least `required` bytes.
    bool ensureStagingBuffer(TextureHandle& handle, VkDeviceSize required);
    void releaseStagingBuffer(TextureHandle& handle);

    // Copy a strided source rectangle into the handle's staging buffer, packed.
    bool writeStagingPixels(TextureHandle& handle, const uint8_t* pixels, uint32_t stride);

    /**
     * Record the staging -> image transfer on the graphics queue and wait for it.
     *
     * The graphics family is the one the frames are submitted on, so the image
     * needs no queue-family ownership transfer and the barriers may legally name
     * the fragment shader stage.
     *
     * Both ends are guarded in the command buffer itself: the entry barrier
     * waits on every earlier-submitted read of this image before the copy may
     * write it, and the exit barrier makes the copy visible to every later
     * sample. Same queue, so submission order carries both.
     */
    void uploadStagingToImage(TextureHandle& handle);
};

} // namespace Nova
