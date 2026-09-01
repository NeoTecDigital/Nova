// Written by Richard Christopher, Copyright 2026 NeoTec Digital
//
// Offscreen presentation mode (plan §D.2). NovaGraphics renders into an image
// it was handed instead of a swapchain image it acquired, which is what lets
// Vazio draw into a wlroots output buffer, a boot-splash target, or a probe's
// readback image with no SDL window anywhere in the process.
#pragma once

#include "./nova_dmabuf.h"

#include <vulkan/vulkan.h>
#include <unordered_map>

/**
 * What an offscreen NovaGraphics needs to come up without a surface.
 *
 * No window, no VkSurfaceKHR, no swapchain: instance creation skips the SDL
 * surface extensions entirely and device selection asks for a graphics family
 * without asking for a present family.
 */
struct NovaOffscreenConfig {
    // Default render extent. Individual targets carry their own; this is what
    // window_extent reports and what a caller-supplied camera is sized against.
    VkExtent2D extent = { 0, 0 };

    /**
     * Borrowed DRM node fd, used only as the device-selection tie-break.
     *
     * Nova never reads, ioctls or closes it. Pass the fd whose allocator will
     * produce the buffers to be imported (wlr_backend_get_drm_fd) so import
     * cannot silently land on a second GPU.
     */
    int drm_fd = -1;

    // Ask for the external-memory extension trio. Absence is not fatal: the
    // owned-image path still works and importDmabufAsImage reports unsupported.
    bool request_dmabuf_import = true;
};

/**
 * An image Nova does not own, described well enough to render into.
 *
 * `view` must be a 2D colour view of `image` covering mip 0 / layer 0 in
 * `format`. Nova creates no view for a caller-supplied target and destroys
 * none; imported dmabufs are the exception and carry their own.
 */
struct NovaRenderTarget {
    VkImage image = VK_NULL_HANDLE;
    VkImageView view = VK_NULL_HANDLE;
    VkExtent2D extent = { 0, 0 };
    VkFormat format = VK_FORMAT_UNDEFINED;

    /**
     * Layout the image is left in when the render pass ends.
     *
     * The render pass always starts from UNDEFINED and clears, so the previous
     * contents and the previous layout are both irrelevant on entry; only the
     * exit layout is the consumer's business. TRANSFER_SRC suits a readback,
     * GENERAL suits a hand-off to an external consumer such as KMS scanout.
     */
    VkImageLayout final_layout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;

    // Same clear the swapchain path uses, so both modes start identical.
    VkClearValue clear = {{{ 0.04f, 0.05f, 0.08f, 1.0f }}};

    /**
     * Will something outside this VkDevice read the result?
     *
     * KMS scanout, another process, another API. When set, renderToImage()
     * releases queue-family ownership of the image to VK_QUEUE_FAMILY_FOREIGN_EXT
     * after the pass, which is what makes a driver flush any private
     * representation - on radv a DCC-compressed modifier image read back
     * WITHOUT this release comes out wrong, measured. Set automatically by
     * NovaImportedImage::asRenderTarget(); an owned image needs it only if its
     * memory is exported.
     *
     * Requires VK_EXT_queue_family_foreign. Without it the release is skipped
     * and reported, because there is no legal way to express it.
     */
    bool external_consumer = false;

    bool valid() const {
        return image != VK_NULL_HANDLE && view != VK_NULL_HANDLE &&
               format != VK_FORMAT_UNDEFINED && extent.width > 0 && extent.height > 0;
    }
};

/**
 * A dmabuf imported as a VkImage, owned by Nova until released.
 *
 * `memory_count` is 1 for the common case and equals the modifier's plane count
 * when the image had to be created disjoint. Every fd Nova holds here is a dup
 * of the caller's; releasing the image closes Nova's copies only.
 *
 * The image is created with initialLayout UNDEFINED (required for external
 * memory, VUID-VkImageCreateInfo-pNext-01443). A consumer that wants to READ
 * bytes an external writer put in the buffer barriers from UNDEFINED: with a
 * DRM format modifier the layout is fixed by the modifier, so the transition
 * has nothing to discard. renderToImage() needs no such barrier - its render
 * pass already starts from UNDEFINED and clears.
 */
struct NovaImportedImage {
    VkImage image = VK_NULL_HANDLE;
    VkImageView view = VK_NULL_HANDLE;
    VkDeviceMemory memory[NOVA_DMABUF_MAX_PLANES] = {};
    uint32_t memory_count = 0;

    VkFormat format = VK_FORMAT_UNDEFINED;
    VkExtent2D extent = { 0, 0 };
    uint64_t modifier = NOVA_DRM_FORMAT_MOD_INVALID;
    uint32_t drm_format = 0;

    bool valid() const { return image != VK_NULL_HANDLE && view != VK_NULL_HANDLE; }

    // Ready to hand straight to renderToImage, with external_consumer set: an
    // imported buffer exists precisely to be read by someone outside Vulkan.
    NovaRenderTarget asRenderTarget(VkImageLayout final_layout) const;
};

/**
 * Render passes and framebuffers keyed by what actually distinguishes them.
 *
 * A render pass depends only on (format, final layout) and a framebuffer only
 * on (pass, view, extent), so both are cached rather than rebuilt per frame -
 * an imported output buffer is re-rendered every frame for the life of the
 * output and must not churn objects at that rate.
 */
class NovaOffscreenTargets {
public:
    explicit NovaOffscreenTargets(VkDevice device) : device_(device) {}
    ~NovaOffscreenTargets();

    NovaOffscreenTargets(const NovaOffscreenTargets&) = delete;
    NovaOffscreenTargets& operator=(const NovaOffscreenTargets&) = delete;

    // VK_NULL_HANDLE when the pass could not be created.
    VkRenderPass renderPass(VkFormat format, VkImageLayout final_layout);

    // VK_NULL_HANDLE when the framebuffer could not be created.
    VkFramebuffer framebuffer(VkRenderPass pass, const NovaRenderTarget& target);

    /**
     * Drop the framebuffer built over `view`.
     *
     * Mandatory before the view is destroyed: a cached framebuffer outliving
     * its attachment is a dangling handle the next frame would submit.
     */
    void invalidate(VkImageView view);

    void destroy();

private:
    struct FramebufferEntry {
        VkFramebuffer framebuffer = VK_NULL_HANDLE;
        VkRenderPass pass = VK_NULL_HANDLE;
        VkExtent2D extent = { 0, 0 };
    };

    VkDevice device_ = VK_NULL_HANDLE;
    std::unordered_map<uint64_t, VkRenderPass> passes_;
    std::unordered_map<VkImageView, FramebufferEntry> framebuffers_;
};
