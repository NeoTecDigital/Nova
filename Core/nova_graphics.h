#pragma once
#include "./core_base.h"
#include "./nova_offscreen.h"
#include "./modules/pipeline/pipeline.h"
#include "./modules/camera/camera.h"

#include <memory>
#include <vector>
namespace Nova {
/**
 * Graphics - Graphics rendering mode
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
class Graphics : public Core {
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

    Builder* graphics_pipeline = nullptr;
    DescriptorContext descriptor;

    // Offscreen mode (plan section D.2): no surface, no swapchain, no present.
    // renderFrame() is unusable in this mode and says so; renderToImage() is
    // the whole presentation path.
    bool offscreen_mode = false;
    std::unique_ptr<OffscreenTargets> offscreen_targets;

    // Imported dmabufs, tracked BY VALUE and matched on VkImage. Tracking the
    // caller's object by address would leave a dangling entry the moment a
    // caller let its ImportedImage go out of scope unreleased.
    std::vector<ImportedImage> imported_images;

    const VkClearValue CLEAR_COLOR = {{{0.0f, 0.0f, 0.0f, 1.0f}}};
    unsigned int frame_ct = 0;

    FrameData& current_frame() { return frames[frame_ct % MAX_FRAMES_IN_FLIGHT]; }

    /**
     * Everything a surface-backed instance needs once `surface` is live:
     * device selection with presentation, queues, swapchain, render pass,
     * framebuffers and frame sync.
     *
     * Shared by both surface constructors - the SDL one in Core/nova_sdl.cpp
     * and the external-VkSurfaceKHR one in Core/nova_graphics.cpp - so the two
     * paths cannot drift apart. Not called on the offscreen path.
     */
    void completeSurfaceBackedInit();

    // Private graphics initialization methods
    void createSwapchain();
    void createImageViews();
    void createFramebuffers();
    void createRenderPass();
    void createFrameSyncObjects();
    void recreateSwapchain();

    // Teardown halves of the surface-backed path, split out of ~Graphics so
    // the destructor stays readable. Both are safe in offscreen mode, where the
    // collections they walk are empty.
    void destroySwapchainResources();
    void destroyFrameSyncObjects();

    // Frame slots without the per-swapchain-image semaphores: an offscreen
    // submission has nothing to acquire from and nothing to present to, so the
    // in-flight fence is the whole of its synchronisation.
    void createOffscreenFrameSyncObjects();
    void registerOffscreenCleanup();

    /**
     * The registry key the offscreen cleanup entry is filed under.
     *
     * Core::resource_registry belongs to the BASE, so it runs its entries
     * from ~Core - after offscreen_targets and imported_images below have
     * already been destroyed. ~Graphics therefore runs and unregisters this
     * entry itself, by this key, while those members are still alive. The key
     * is named here because two translation units address it: nova_offscreen.cpp
     * registers, nova_graphics.cpp releases.
     */
    static constexpr const char* kOffscreenCleanupKey = "offscreen_targets";

    // Release the offscreen-mode GPU state this instance owns: every imported
    // image, then the render passes and framebuffers. Idempotent.
    void destroyOffscreenState();

    bool resolveOffscreenTarget(const RenderTarget& target,
                                VkRenderPass& pass_out,
                                VkFramebuffer& framebuffer_out);

    // DMA-BUF import internals.
    bool dmabufImportAccepted(const DmabufAttributes& attrs, VkFormat& format_out);
    uint32_t importMemoryTypeFor(int fd, const VkMemoryRequirements& requirements);
    VkDeviceMemory importPlaneMemory(VkImage image, int fd, const VkMemoryRequirements& requirements);
    VkDeviceMemory importOnePlane(ImportedImage& imported, const DmabufAttributes& attrs,
                                  int plane, bool disjoint);
    bool bindImportedMemory(ImportedImage& imported, const DmabufAttributes& attrs, bool disjoint);
    bool createImportedView(ImportedImage& imported);
    void trackImportedImage(const ImportedImage& imported);

    // Destroy the handles an import created. Assumes the device is already idle.
    void destroyImportedResources(ImportedImage& imported);

    void querySwapChainDetails();
    SwapChainSupportDetails querySwapChainSupport(VkPhysicalDevice device);
    void createSwapchainInfoKHR(VkSwapchainCreateInfoKHR* create_info, uint32_t image_count);

public:
    Camera player_camera;
    bool framebuffer_resized = false;

    /**
     * Constructor - Initialize graphics mode with SDL window.
     *
     * DEFINED IN Core/nova_sdl.cpp, not in nova_graphics.cpp. That translation
     * unit is the only one in Nova that calls libSDL2, and it is compiled into
     * the separate Nova::SDL archive so that windowless targets never resolve
     * against SDL at all. Declaring it here costs nothing: `struct SDL_Window`
     * is an incomplete type and needs no SDL header.
     *
     * @param extent Window extent (width, height)
     * @param debug_level Logging level
     * @param window SDL window handle
     */
    Graphics(VkExtent2D extent, const std::string& debug_level, struct SDL_Window* window);

    /**
     * Constructor - Initialize graphics mode against a surface Nova did not make.
     *
     * @param instance_extensions The instance-level WSI extensions the caller's
     *        platform requires. Passed as data rather than queried, because
     *        this translation unit has no window system to ask - the caller who
     *        owns the surface is the one that knows.
     */
    Graphics(VkExtent2D extent, const std::string& debug_level, VkSurfaceKHR surface,
                 const std::vector<const char*>& instance_extensions);

    /**
     * Constructor - offscreen mode: no SDL, no surface, no swapchain.
     *
     * Instance creation skips the surface extensions and device selection asks
     * for a graphics family without a present family, so this constructor is
     * usable on a bare TTY where no display server exists.
     */
    Graphics(const OffscreenConfig& config, const std::string& debug_level);

    /**
     * Destructor - Cleanup graphics resources
     */
    ~Graphics() override;

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
    VkFence renderToImage(const RenderTarget& target,
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
    bool importDmabufAsImage(const DmabufAttributes& attrs, ImportedImage& out);

    /**
     * Destroy an imported image and every object Nova created for it.
     *
     * Waits for the device to go idle first, like TextureBridge::releaseTexture
     * and for the same reason: a destroy cannot be ordered by a barrier, and a
     * command buffer still in flight would be left referencing nothing.
     */
    void releaseImportedImage(ImportedImage& image);

    /**
     * The render pass renderToImage() will use for this (format, layout) pair.
     *
     * Callers build their graphics pipelines against it. It is created on first
     * ask and cached, so the handle is stable for the life of this Graphics
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

} // namespace Nova
