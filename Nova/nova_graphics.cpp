#include "./nova_graphics.h"
#include "./nova_dmabuf_query.h"
namespace Nova {
// No window-system header belongs in this translation unit. The SDL-window
// constructor lives in Nova/nova_sdl.cpp, which is the one place in Nova that
// links libSDL2; everything here is surface-agnostic.

Graphics::Graphics(VkExtent2D extent, const std::string& debug_level, VkSurfaceKHR surf,
                           const std::vector<const char*>& instance_extensions)
    : Core(debug_level), surface(surf)
{
    report(LOGGER::INFO, "NovaGraphics - Initializing graphics mode ..");

    setWindowExtent(extent);

    createVulkanInstance(instance_extensions);
    completeSurfaceBackedInit();

    report(LOGGER::INFO, "NovaGraphics - Initialized successfully");
}

// Second half of every surface-backed bring-up, from device selection through
// frame sync. Both surface constructors call it with `surface` already live, so
// the SDL path and the external-surface path stay one code path.
void Graphics::completeSurfaceBackedInit()
{
    createPhysicalDevice(true, surface);   // Need presentation support
    createLogicalDevice(true);             // Need swapchain extension
    resolveDmabufEntryPoints();
    createSharedCommandPools();
    createImmediateContext();

    // Get graphics and present queues
    vkGetDeviceQueue(logical_device, queues.indices.graphics_family.value(), 0, &graphics_queue);
    vkGetDeviceQueue(logical_device, queues.indices.present_family.value(), 0, &present_queue);

    // Initialize graphics-specific resources
    createSwapchain();
    createImageViews();
    createRenderPass();
    createFramebuffers();
    createFrameSyncObjects();
}

/**
 * Bind this instance's device-level dmabuf entry points to this instance.
 *
 * One lookup, at the only moment when the answer is both knowable and final:
 * the logical device exists and its enabled extension set is fixed. Nothing is
 * cached across instances because there is nothing shared to cache - the thunk
 * belongs to the VkDevice, and a second Graphics has a second device.
 *
 * Silent when the extensions are absent: that is not a failure, it is the
 * capability supportsDmabufImport() already reports, and every import path
 * checks the pointer before it uses it.
 */
void Graphics::resolveDmabufEntryPoints()
{
    if (!supportsDmabufImport()) {
        return;
    }

    get_memory_fd_properties = loadGetMemoryFdProperties(logical_device);
    if (get_memory_fd_properties == nullptr) {
        report(LOGGER::WARN,
               "NovaGraphics - VK_KHR_external_memory_fd is enabled but "
               "vkGetMemoryFdPropertiesKHR did not resolve; imports will be refused");
        return;
    }

    report(LOGGER::VERBOSE, "NovaGraphics - dmabuf entry points resolved for this device");
}

Graphics::~Graphics()
{
    report(LOGGER::INFO, "NovaGraphics - Destroying");
    vkDeviceWaitIdle(logical_device);

    // offscreen_targets and imported_images are Graphics members, so their
    // release belongs HERE, in the derived destructor body, before any member
    // of this object has been destroyed. Core::resource_registry cannot do
    // it: the base destructor drains the registry after both members are gone,
    // which is a use-after-free that -O0 survives by accident and -O2 turns
    // into `double free or corruption`. release() rather than run_and_release()
    // because the registered entry is a tripwire, not a copy of this teardown
    // (see registerOffscreenCleanup). Both calls are idempotent and both are
    // no-ops on the surface-backed path, which registers no entry.
    destroyOffscreenState();
    resource_registry.release(kOffscreenCleanupKey);

    if (graphics_pipeline) {
        delete graphics_pipeline;
        graphics_pipeline = nullptr;
    }

    if (render_pass != VK_NULL_HANDLE) {
        vkDestroyRenderPass(logical_device, render_pass, nullptr);
    }

    destroySwapchainResources();
    destroyFrameSyncObjects();

    // The surface is destroyed by whoever created it (nova_sdl.cpp for the SDL
    // path, the caller for the external-surface path), never here.
}

// Framebuffers, image views and the swapchain itself. All three collections are
// empty in offscreen mode, which is what makes this callable unconditionally.
void Graphics::destroySwapchainResources()
{
    for (auto framebuffer : swapchain.framebuffers) {
        if (framebuffer != VK_NULL_HANDLE) {
            vkDestroyFramebuffer(logical_device, framebuffer, nullptr);
        }
    }

    for (auto imageView : swapchain.image_views) {
        if (imageView != VK_NULL_HANDLE) {
            vkDestroyImageView(logical_device, imageView, nullptr);
        }
    }

    if (swapchain.instance != VK_NULL_HANDLE) {
        vkDestroySwapchainKHR(logical_device, swapchain.instance, nullptr);
    }
}

// The per-frame-in-flight slots. Offscreen mode populates the fence and the
// command pool but leaves both semaphore vectors empty, so the same walk covers
// either construction path.
void Graphics::destroyFrameSyncObjects()
{
    for (size_t i = 0; i < MAX_FRAMES_IN_FLIGHT; i++) {
        for (auto semaphore : frames[i].image_available) {
            if (semaphore != VK_NULL_HANDLE) {
                vkDestroySemaphore(logical_device, semaphore, nullptr);
            }
        }
        for (auto semaphore : frames[i].render_finished) {
            if (semaphore != VK_NULL_HANDLE) {
                vkDestroySemaphore(logical_device, semaphore, nullptr);
            }
        }
        if (frames[i].in_flight != VK_NULL_HANDLE) {
            vkDestroyFence(logical_device, frames[i].in_flight, nullptr);
        }
        if (frames[i].cmd.pool != VK_NULL_HANDLE) {
            vkDestroyCommandPool(logical_device, frames[i].cmd.pool, nullptr);
        }
    }
}

void Graphics::createRenderPass()
{
    report(LOGGER::VLINE, "\t .. Creating Render Pass ..");

    VkAttachmentDescription color_attachment = {
        .format = swapchain.format,
        .samples = VK_SAMPLE_COUNT_1_BIT,
        .loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR,
        .storeOp = VK_ATTACHMENT_STORE_OP_STORE,
        .stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE,
        .stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE,
        .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED,
        .finalLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR
    };

    VkAttachmentReference color_attachment_ref = {
        .attachment = 0,
        .layout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL
    };

    VkSubpassDescription subpass = {
        .pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS,
        .colorAttachmentCount = 1,
        .pColorAttachments = &color_attachment_ref
    };

    VkRenderPassCreateInfo render_pass_info = {
        .sType = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO,
        .attachmentCount = 1,
        .pAttachments = &color_attachment,
        .subpassCount = 1,
        .pSubpasses = &subpass
    };

    VK_TRY(vkCreateRenderPass(logical_device, &render_pass_info, nullptr, &render_pass));

    report(LOGGER::INFO, "Render pass created");
}

void Graphics::createFrameSyncObjects()
{
    report(LOGGER::VLINE, "\t .. Creating Frame Sync Objects ..");

    VkSemaphoreCreateInfo semaphore_info = {
        .sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO
    };

    VkFenceCreateInfo fence_info = {
        .sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO,
        .flags = VK_FENCE_CREATE_SIGNALED_BIT
    };

    for (size_t i = 0; i < MAX_FRAMES_IN_FLIGHT; i++) {
        // Create command pool for this frame
        VkCommandPoolCreateInfo pool_info = {
            .sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO,
            .flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT,
            .queueFamilyIndex = queues.indices.graphics_family.value()
        };
        VK_TRY(vkCreateCommandPool(logical_device, &pool_info, nullptr, &frames[i].cmd.pool));

        // Allocate command buffer
        VkCommandBufferAllocateInfo cmd_alloc_info = {
            .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,
            .commandPool = frames[i].cmd.pool,
            .level = VK_COMMAND_BUFFER_LEVEL_PRIMARY,
            .commandBufferCount = 1
        };
        VK_TRY(vkAllocateCommandBuffers(logical_device, &cmd_alloc_info, &frames[i].cmd.buffer));

        // Create semaphores for each swapchain image
        frames[i].image_available.resize(swapchain.images.size());
        frames[i].render_finished.resize(swapchain.images.size());

        for (size_t j = 0; j < swapchain.images.size(); j++) {
            VK_TRY(vkCreateSemaphore(logical_device, &semaphore_info, nullptr, &frames[i].image_available[j]));
            VK_TRY(vkCreateSemaphore(logical_device, &semaphore_info, nullptr, &frames[i].render_finished[j]));
        }

        // Create fence
        VK_TRY(vkCreateFence(logical_device, &fence_info, nullptr, &frames[i].in_flight));
    }

    report(LOGGER::INFO, "Frame sync objects created");
}

void Graphics::drawFrame()
{
    renderFrame([](VkCommandBuffer, uint32_t){});
}

void Graphics::renderFrame(std::function<void(VkCommandBuffer, uint32_t)>&& render_callback)
{
    // Offscreen mode has no swapchain to acquire from; renderToImage() is its
    // presentation path. Saying so beats dereferencing a null swapchain.
    if (offscreen_mode) {
        report(LOGGER::ERROR, "renderFrame: unavailable in offscreen mode - use renderToImage()");
        return;
    }

    FrameData& frame = current_frame();

    VK_TRY(vkWaitForFences(logical_device, 1, &frame.in_flight, VK_TRUE, UINT64_MAX));

    uint32_t image_index = 0;
    VkResult result = vkAcquireNextImageKHR(
        logical_device,
        swapchain.instance,
        UINT64_MAX,
        frame.image_available[0],
        VK_NULL_HANDLE,
        &image_index
    );

    if (result == VK_ERROR_OUT_OF_DATE_KHR) {
        recreateSwapchain();
        return;
    } else if (result != VK_SUCCESS && result != VK_SUBOPTIMAL_KHR) {
        report(LOGGER::ERROR, "Failed to acquire swapchain image!");
        return;
    }

    VK_TRY(vkResetFences(logical_device, 1, &frame.in_flight));

    VkCommandBuffer cmd = frame.cmd.buffer;
    VK_TRY(vkResetCommandBuffer(cmd, 0));

    VkCommandBufferBeginInfo begin_info = {
        .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
        .flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT
    };
    VK_TRY(vkBeginCommandBuffer(cmd, &begin_info));

    VkClearValue clear_values[1];
    clear_values[0].color = {{ 0.04f, 0.05f, 0.08f, 1.0f }};

    VkRenderPassBeginInfo render_pass_info = {
        .sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO,
        .renderPass = render_pass,
        .framebuffer = swapchain.framebuffers[image_index],
        .renderArea = {
            .offset = {0, 0},
            .extent = swapchain.extent
        },
        .clearValueCount = 1,
        .pClearValues = clear_values
    };

    vkCmdBeginRenderPass(cmd, &render_pass_info, VK_SUBPASS_CONTENTS_INLINE);

    VkViewport viewport = {
        .x = 0.0f,
        .y = 0.0f,
        .width = static_cast<float>(swapchain.extent.width),
        .height = static_cast<float>(swapchain.extent.height),
        .minDepth = 0.0f,
        .maxDepth = 1.0f
    };
    vkCmdSetViewport(cmd, 0, 1, &viewport);

    VkRect2D scissor = {
        .offset = {0, 0},
        .extent = swapchain.extent
    };
    vkCmdSetScissor(cmd, 0, 1, &scissor);

    // Execute user render callback
    if (render_callback) {
        render_callback(cmd, image_index);
    }

    vkCmdEndRenderPass(cmd);
    VK_TRY(vkEndCommandBuffer(cmd));

    VkSemaphore wait_semaphores[] = { frame.image_available[0] };
    VkPipelineStageFlags wait_stages[] = { VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT };
    VkSemaphore signal_semaphores[] = { frame.render_finished[0] };

    VkSubmitInfo submit_info = {
        .sType = VK_STRUCTURE_TYPE_SUBMIT_INFO,
        .waitSemaphoreCount = 1,
        .pWaitSemaphores = wait_semaphores,
        .pWaitDstStageMask = wait_stages,
        .commandBufferCount = 1,
        .pCommandBuffers = &cmd,
        .signalSemaphoreCount = 1,
        .pSignalSemaphores = signal_semaphores
    };

    VK_TRY(vkQueueSubmit(graphics_queue, 1, &submit_info, frame.in_flight));

    VkPresentInfoKHR present_info = {
        .sType = VK_STRUCTURE_TYPE_PRESENT_INFO_KHR,
        .waitSemaphoreCount = 1,
        .pWaitSemaphores = signal_semaphores,
        .swapchainCount = 1,
        .pSwapchains = &swapchain.instance,
        .pImageIndices = &image_index
    };

    result = vkQueuePresentKHR(present_queue, &present_info);
    if (result == VK_ERROR_OUT_OF_DATE_KHR || result == VK_SUBOPTIMAL_KHR || framebuffer_resized) {
        framebuffer_resized = false;
        recreateSwapchain();
    }

    frame_ct++;
}

void Graphics::setWindowExtent(VkExtent2D extent)
{
    window_extent = extent;
}

void Graphics::constructGraphicsPipeline(const std::string& vert, const std::string& frag)
{
    report(LOGGER::DEBUG, "NovaGraphics::constructGraphicsPipeline() - not yet implemented");
    // Placeholder - will migrate from existing code
}

} // namespace Nova
