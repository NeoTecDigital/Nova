#pragma once
#include "./core_base.h"
#include "./modules/pipeline/pipeline.h"
#include "./modules/camera/camera.h"

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

    SwapChainContext swapchain;
    FrameData frames[MAX_FRAMES_IN_FLIGHT];
    VkRenderPass render_pass = VK_NULL_HANDLE;

    Pipeline* graphics_pipeline = nullptr;
    DescriptorContext descriptor;

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
     * Destructor - Cleanup graphics resources
     */
    ~NovaGraphics() override;

    /**
     * Render a frame with custom rendering callback
     */
    void drawFrame();
    void renderFrame(std::function<void(VkCommandBuffer, uint32_t)>&& render_callback);

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
     * frame_ct is incremented at the very end of renderFrame(), after present,
     * so during the render callback this returns the slot whose in-flight fence
     * was already waited on at the top of renderFrame(). Per-frame resources
     * indexed by this value are therefore guaranteed free of GPU readers.
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
