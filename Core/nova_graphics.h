#pragma once
#include "./core_base.h"
#include "./nova_offscreen.h"
#include "./modules/pipeline/pipeline.h"
#include "./modules/camera/camera.h"

#include <memory>
#include <vector>

/**
 * NovaGraphics - Graphics rendering mode
 *
 * Traditional Vulkan rendering pipeline:
 * - Surface and swapchain
 * - Graphics and present queues
 * - Render pass
 * - Frame synchronization
 * - Graphics pipeline
 *
 * Use for:
 * - Traditional 3D rendering
 * - Window-based applications
 * - Game engines
 */
class NovaGraphics : public NovaCore {
private:
    // Graphics-specific resources
    VkQueue graphics_queue = VK_NULL_HANDLE;
    VkQueue present_queue = VK_NULL_HANDLE;
    VkSurfaceKHR surface = VK_NULL_HANDLE;

    // Value-initialised, not merely default-initialised: both aggregates hold
    // raw Vulkan handles with no in-class initialisers, and the destructor
    // tests every one of them. Offscreen mode never populates the swapchain, so
    // without this the teardown would read indeterminate handles.
    SwapChainContext swapchain = {};
    FrameData frames[MAX_FRAMES_IN_FLIGHT] = {};
    VkRenderPass render_pass = VK_NULL_HANDLE;

    Pipeline* graphics_pipeline = nullptr;
    DescriptorContext descriptor;

    // Offscreen mode (plan section D.2): no surface, no swapchain, no present.
    // renderFrame() is unusable in this mode and says so; renderToImage() is
    // the whole presentation path.
    bool offscreen_mode = false;
    std::unique_ptr<NovaOffscreenTargets> offscreen_targets;

    // Imported dmabufs, tracked BY VALUE and matched on VkImage. Tracking the
    // caller's object by address would leave a dangling entry the moment a
    // caller let its NovaImportedImage go out of scope unreleased.
    std::vector<NovaImportedImage> imported_images;

    const VkClearValue CLEAR_COLOR = {{{0.0f, 0.0f, 0.0f, 1.0f}}};
    unsigned int frame_ct = 0;

    FrameData& current_frame() { return frames[frame_ct % MAX_FRAMES_IN_FLIGHT]; }

    // Private graphics initialization methods
    void createSwapchain();
    void createImageViews();
    void createFramebuffers();
    void createRenderPass();
    void createFrameSyncObjects();
    void recreateSwapchain();

    // Frame slots without the per-swapchain-image semaphores: an offscreen
    // submission has nothing to acquire from and nothing to present to, so the
    // in-flight fence is the whole of its synchronisation.
    void createOffscreenFrameSyncObjects();
    void registerOffscreenCleanup();
    bool resolveOffscreenTarget(const NovaRenderTarget& target,
                                VkRenderPass& pass_out,
                                VkFramebuffer& framebuffer_out);

    // DMA-BUF import internals.
    bool dmabufImportAccepted(const NovaDmabufAttributes& attrs, VkFormat& format_out);
    uint32_t importMemoryTypeFor(int fd, const VkMemoryRequirements& requirements);
    VkDeviceMemory importPlaneMemory(VkImage image, int fd, const VkMemoryRequirements& requirements);
    VkDeviceMemory importOnePlane(NovaImportedImage& imported, const NovaDmabufAttributes& attrs,
                                  int plane, bool disjoint);
    bool bindImportedMemory(NovaImportedImage& imported, const NovaDmabufAttributes& attrs, bool disjoint);
    bool createImportedView(NovaImportedImage& imported);
    void trackImportedImage(const NovaImportedImage& imported);

    // Destroy the handles an import created. Assumes the device is already idle.
    void destroyImportedResources(NovaImportedImage& imported);

    void querySwapChainDetails();
    SwapChainSupportDetails querySwapChainSupport(VkPhysicalDevice device);
    void createSwapchainInfoKHR(VkSwapchainCreateInfoKHR* create_info, uint32_t image_count);

public:
    Camera player_camera;
    bool framebuffer_resized = false;

    /**
     * Constructor - Initialize graphics mode with SDL window
     * @param extent Window extent (width, height)
     * @param debug_level Logging level
     * @param window SDL window handle
     */
    NovaGraphics(VkExtent2D extent, const std::string& debug_level, struct SDL_Window* window);
    NovaGraphics(VkExtent2D extent, const std::string& debug_level, VkSurfaceKHR surface);

    /**
     * Constructor - offscreen mode: no SDL, no surface, no swapchain.
     *
     * Instance creation skips the surface extensions and device selection asks
     * for a graphics family without a present family, so this constructor is
     * usable on a bare TTY where no display server exists.
     */
    NovaGraphics(const NovaOffscreenConfig& config, const std::string& debug_level);

    /**
     * Destructor - Cleanup graphics resources
     */
    ~NovaGraphics() override;

    /**
     * Render a frame with custom rendering callback
     */
    void drawFrame();
    void renderFrame(std::function<void(VkCommandBuffer, uint32_t)>&& render_callback);

    /**
     * Render one frame into an image Nova does not own.
     *
     * Same render-pass semantics as the swapchain path: wait on this frame
     * slot's in-flight fence, clear, set a full-target viewport and scissor,
     * run the callback inside the pass, submit on the graphics queue. There is
     * no acquire and no present, so there are no semaphores - the returned
     * fence is the only completion signal.
     *
     * The callback receives the frame-in-flight slot index in the second
     * argument, where the swapchain path passes the swapchain image index.
     * Both are getCurrentFrameIndex() for offscreen work.
     *
     * Returns the fence that signals when the GPU is done, or VK_NULL_HANDLE
     * when the target was rejected. The caller MUST wait on it (waitForRender)
     * before handing the image to a consumer: bring-up sync is fence-and-wait,
     * per plan D.2; timeline semaphores come later.
     */
    VkFence renderToImage(const NovaRenderTarget& target,
                          std::function<void(VkCommandBuffer, uint32_t)>&& render_callback);

    // Block until a fence from renderToImage signals. False on timeout/error.
    bool waitForRender(VkFence fence, uint64_t timeout_ns = UINT64_MAX);

    /**
     * Import a DMA-BUF as a VkImage renderable by renderToImage.
     *
     * `attrs.planes[i].fd` is borrowed: Nova dups what it keeps and the caller
     * still owns its own fds. Returns false and leaves `out` untouched when the
     * format, the modifier or the plane count is not importable on this device,
     * or when the external-memory extensions are absent.
     */
    bool importDmabufAsImage(const NovaDmabufAttributes& attrs, NovaImportedImage& out);

    /**
     * Destroy an imported image and every object Nova created for it.
     *
     * Waits for the device to go idle first, like TextureBridge::releaseTexture
     * and for the same reason: a destroy cannot be ordered by a barrier, and a
     * command buffer still in flight would be left referencing nothing.
     */
    void releaseImportedImage(NovaImportedImage& image);

    /**
     * The render pass renderToImage() will use for this (format, layout) pair.
     *
     * Callers build their graphics pipelines against it. It is created on first
     * ask and cached, so the handle is stable for the life of this NovaGraphics
     * and a pipeline built against it stays compatible.
     *
     * VK_NULL_HANDLE if creation fails. getRenderPass() is the swapchain
     * equivalent and stays VK_NULL_HANDLE in offscreen mode.
     */
    VkRenderPass getOffscreenRenderPass(VkFormat format, VkImageLayout final_layout);

    /**
     * DRM format modifiers this device can import `drm_format` with.
     *
     * What a compositor advertises through linux-dmabuf-v1 and what it hands
     * to an allocator: a buffer allocated outside this list cannot be imported.
     * Empty when the format is unmapped or the extensions are absent.
     *
     * DRM_FORMAT_MOD_INVALID never appears. Vulkan has no implicit-modifier
     * import path, so a buffer allocated without explicit modifiers cannot be
     * imported at all - allocate with this list instead.
     */
    std::vector<uint64_t> supportedDmabufModifiers(uint32_t drm_format) const;

    // True when the external-memory trio was present and enabled.
    bool supportsDmabufImport() const;

    // True when constructed offscreen: no surface, no swapchain, no present.
    bool isOffscreen() const { return offscreen_mode; }

    /**
     * Update window extent (for resize)
     */
    void setWindowExtent(VkExtent2D extent);

    /**
     * Construct graphics pipeline
     * @param vert Vertex shader path
     * @param frag Fragment shader path
     */
    void constructGraphicsPipeline(const std::string& vert = "", const std::string& frag = "");

    /**
     * Index of the frame-in-flight slot currently being RECORDED.
     *
     * frame_ct is incremented at the very end of renderFrame() (after present)
     * and of renderToImage() (after submit), so during either render callback
     * this returns the slot whose in-flight fence was already waited on at the
     * top of the call. Per-frame resources indexed by this value are therefore
     * guaranteed free of GPU readers, in both modes.
     */
    uint32_t getCurrentFrameIndex() const { return frame_ct % MAX_FRAMES_IN_FLIGHT; }

    /**
     * Get swapchain context
     */
    SwapChainContext& getSwapchain() { return swapchain; }

    /**
     * Get render pass
     */
    VkRenderPass getRenderPass() const { return render_pass; }

    /**
     * Get window extent
     */
    VkExtent2D getWindowExtent() const { return window_extent; }

    /**
     * Get graphics queue
     */
    VkQueue getGraphicsQueue() const { return graphics_queue; }

    /**
     * Get present queue
     */
    VkQueue getPresentQueue() const { return present_queue; }

    /**
     * Get surface
     */
    VkSurfaceKHR getSurface() const { return surface; }
};
