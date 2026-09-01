// Written by Richard Christopher, Copyright 2026 NeoTec Digital
//
// Offscreen presentation mode (plan section D.2): NovaGraphics without SDL,
// without a surface and without a swapchain. Kept out of nova_graphics.cpp so
// neither file passes its size limit and so the SDL path stays readable as one
// thing.

#include "./nova_graphics.h"

// initialLayout is UNDEFINED because loadOp is CLEAR: nothing in the image
// survives the frame, so neither the previous contents nor the previous layout
// can matter, and an imported buffer needs no acquire barrier to be legal here.
// finalLayout is the caller's, because only the consumer knows what comes next.
static VkAttachmentDescription offscreenColorAttachment(VkFormat format, VkImageLayout final_layout)
{
    return VkAttachmentDescription{
        .format = format,
        .samples = VK_SAMPLE_COUNT_1_BIT,
        .loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR,
        .storeOp = VK_ATTACHMENT_STORE_OP_STORE,
        .stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE,
        .stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE,
        .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED,
        .finalLayout = final_layout
    };
}

// The pass a caller-supplied target is rendered through.
static VkRenderPass buildOffscreenPass(VkDevice device, VkFormat format, VkImageLayout final_layout)
{
    VkAttachmentDescription color = offscreenColorAttachment(format, final_layout);

    VkAttachmentReference color_ref = {
        .attachment = 0,
        .layout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL
    };

    VkSubpassDescription subpass = {
        .pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS,
        .colorAttachmentCount = 1,
        .pColorAttachments = &color_ref
    };

    // The external dependency is what makes the final layout transition visible
    // to whoever reads the image next; the swapchain path gets the equivalent
    // for free from the present engine.
    VkSubpassDependency exit = {
        .srcSubpass = 0,
        .dstSubpass = VK_SUBPASS_EXTERNAL,
        .srcStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
        .dstStageMask = VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT,
        .srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT,
        .dstAccessMask = 0
    };

    VkRenderPassCreateInfo info = {
        .sType = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO,
        .attachmentCount = 1,
        .pAttachments = &color,
        .subpassCount = 1,
        .pSubpasses = &subpass,
        .dependencyCount = 1,
        .pDependencies = &exit
    };

    VkRenderPass pass = VK_NULL_HANDLE;
    if (vkCreateRenderPass(device, &info, nullptr, &pass) != VK_SUCCESS) {
        return VK_NULL_HANDLE;
    }

    return pass;
}

NovaRenderTarget NovaImportedImage::asRenderTarget(VkImageLayout target_layout) const
{
    NovaRenderTarget target = {};
    target.image = image;
    target.view = view;
    target.extent = extent;
    target.format = format;
    target.final_layout = target_layout;
    target.external_consumer = true;

    return target;
}

    ///////////////////////////////
    // RENDER PASS / FB CACHING  //
    ///////////////////////////////

NovaOffscreenTargets::~NovaOffscreenTargets()
{
    destroy();
}

VkRenderPass NovaOffscreenTargets::renderPass(VkFormat format, VkImageLayout final_layout)
{
    const uint64_t key = (static_cast<uint64_t>(format) << 32) | static_cast<uint32_t>(final_layout);

    auto found = passes_.find(key);
    if (found != passes_.end()) {
        return found->second;
    }

    VkRenderPass pass = buildOffscreenPass(device_, format, final_layout);
    if (pass == VK_NULL_HANDLE) {
        report(LOGGER::ERROR, "Offscreen: no render pass for format %d / layout %d",
               static_cast<int>(format), static_cast<int>(final_layout));
        return VK_NULL_HANDLE;
    }

    passes_.emplace(key, pass);
    report(LOGGER::VLINE, "\t .. Offscreen render pass cached (format %d, final layout %d)",
           static_cast<int>(format), static_cast<int>(final_layout));

    return pass;
}

VkFramebuffer NovaOffscreenTargets::framebuffer(VkRenderPass pass, const NovaRenderTarget& target)
{
    auto found = framebuffers_.find(target.view);
    if (found != framebuffers_.end()) {
        const FramebufferEntry& entry = found->second;
        const bool same = entry.pass == pass &&
                          entry.extent.width == target.extent.width &&
                          entry.extent.height == target.extent.height;
        if (same) {
            return entry.framebuffer;
        }

        vkDestroyFramebuffer(device_, entry.framebuffer, nullptr);
        framebuffers_.erase(found);
    }

    VkImageView attachments[] = { target.view };
    VkFramebufferCreateInfo info = {
        .sType = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO,
        .renderPass = pass,
        .attachmentCount = 1,
        .pAttachments = attachments,
        .width = target.extent.width,
        .height = target.extent.height,
        .layers = 1
    };

    VkFramebuffer framebuffer = VK_NULL_HANDLE;
    if (vkCreateFramebuffer(device_, &info, nullptr, &framebuffer) != VK_SUCCESS) {
        report(LOGGER::ERROR, "Offscreen: framebuffer creation failed");
        return VK_NULL_HANDLE;
    }

    framebuffers_.emplace(target.view, FramebufferEntry{ framebuffer, pass, target.extent });
    return framebuffer;
}

void NovaOffscreenTargets::invalidate(VkImageView view)
{
    auto found = framebuffers_.find(view);
    if (found == framebuffers_.end()) {
        return;
    }

    vkDestroyFramebuffer(device_, found->second.framebuffer, nullptr);
    framebuffers_.erase(found);
}

void NovaOffscreenTargets::destroy()
{
    for (auto& entry : framebuffers_) {
        vkDestroyFramebuffer(device_, entry.second.framebuffer, nullptr);
    }
    framebuffers_.clear();

    for (auto& entry : passes_) {
        vkDestroyRenderPass(device_, entry.second, nullptr);
    }
    passes_.clear();
}

    ///////////////////////////////
    // OFFSCREEN CONSTRUCTION    //
    ///////////////////////////////

NovaGraphics::NovaGraphics(const NovaOffscreenConfig& config, const std::string& debug_level)
    : NovaCore(debug_level), offscreen_mode(true)
{
    report(LOGGER::INFO, "NovaGraphics - Initializing offscreen mode (no surface) ..");

    setWindowExtent(config.extent);

    // No SDL, so no VK_KHR_surface / VK_KHR_*_surface at instance level.
    createVulkanInstance(false);

    NovaDeviceRequest request = {};
    request.need_presentation = false;
    request.need_graphics = true;
    request.drm_fd = config.drm_fd;

    if (config.request_dmabuf_import) {
        request.optional_extensions = {
            VK_KHR_EXTERNAL_MEMORY_FD_EXTENSION_NAME,
            VK_EXT_EXTERNAL_MEMORY_DMA_BUF_EXTENSION_NAME,
            VK_EXT_IMAGE_DRM_FORMAT_MODIFIER_EXTENSION_NAME,
            // Not part of the import trio: needed to hand a rendered buffer
            // back to a consumer outside this device (NovaRenderTarget::
            // external_consumer). Import works without it; hand-off may not.
            VK_EXT_QUEUE_FAMILY_FOREIGN_EXTENSION_NAME
        };
    }

    createPhysicalDevice(request);
    createLogicalDevice(false);   // no VK_KHR_swapchain: there is no surface
    createSharedCommandPools();
    createImmediateContext();

    // A present family is meaningless without a surface; the graphics family is
    // the one every offscreen submission uses.
    vkGetDeviceQueue(logical_device, queues.indices.graphics_family.value(), 0, &graphics_queue);

    offscreen_targets = std::make_unique<NovaOffscreenTargets>(logical_device);
    createOffscreenFrameSyncObjects();
    registerOffscreenCleanup();

    report(LOGGER::INFO, "NovaGraphics - Offscreen mode initialized (dmabuf import: %s)",
           supportsDmabufImport() ? "available" : "unavailable");
}

void NovaGraphics::createOffscreenFrameSyncObjects()
{
    report(LOGGER::VLINE, "\t .. Creating Offscreen Frame Sync Objects ..");

    VkFenceCreateInfo fence_info = {
        .sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO,
        .flags = VK_FENCE_CREATE_SIGNALED_BIT
    };

    for (size_t i = 0; i < MAX_FRAMES_IN_FLIGHT; i++) {
        VkCommandPoolCreateInfo pool_info = {
            .sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO,
            .flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT,
            .queueFamilyIndex = queues.indices.graphics_family.value()
        };
        VK_TRY(vkCreateCommandPool(logical_device, &pool_info, nullptr, &frames[i].cmd.pool));

        VkCommandBufferAllocateInfo cmd_alloc_info = {
            .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,
            .commandPool = frames[i].cmd.pool,
            .level = VK_COMMAND_BUFFER_LEVEL_PRIMARY,
            .commandBufferCount = 1
        };
        VK_TRY(vkAllocateCommandBuffers(logical_device, &cmd_alloc_info, &frames[i].cmd.buffer));

        VK_TRY(vkCreateFence(logical_device, &fence_info, nullptr, &frames[i].in_flight));

        // The destructor iterates these; leaving them uninitialised would make
        // teardown walk whatever the stack happened to hold.
        frames[i].image_available.clear();
        frames[i].render_finished.clear();
        frames[i].transfer_finished = VK_NULL_HANDLE;
        frames[i].compute_finished = VK_NULL_HANDLE;
    }

    report(LOGGER::INFO, "Offscreen frame sync objects created");
}

// Registered last, so the registry's LIFO teardown runs it first - before the
// allocator and long before the device the objects belong to.
void NovaGraphics::registerOffscreenCleanup()
{
    resource_registry.register_resource("offscreen_targets", [this]() {
        // One idle wait covers every outstanding import; releaseImportedImage()
        // would also mutate the vector being walked.
        if (logical_device != VK_NULL_HANDLE && !imported_images.empty()) {
            vkDeviceWaitIdle(logical_device);
        }

        for (NovaImportedImage& imported : imported_images) {
            destroyImportedResources(imported);
        }
        imported_images.clear();

        if (offscreen_targets) {
            offscreen_targets->destroy();
            offscreen_targets.reset();
        }
    });
}

    ///////////////////////////////
    // OFFSCREEN FRAME           //
    ///////////////////////////////

VkRenderPass NovaGraphics::getOffscreenRenderPass(VkFormat format, VkImageLayout final_layout)
{
    if (!offscreen_targets) {
        report(LOGGER::ERROR, "getOffscreenRenderPass: offscreen targets unavailable on this instance");
        return VK_NULL_HANDLE;
    }

    return offscreen_targets->renderPass(format, final_layout);
}

bool NovaGraphics::waitForRender(VkFence fence, uint64_t timeout_ns)
{
    if (fence == VK_NULL_HANDLE) {
        return false;
    }

    return vkWaitForFences(logical_device, 1, &fence, VK_TRUE, timeout_ns) == VK_SUCCESS;
}

// Clear + full-target viewport/scissor + callback, exactly as the swapchain
// path records them, so a caller's render callback cannot tell the modes apart.
static void recordOffscreenPass(VkCommandBuffer cmd,
                                VkRenderPass pass,
                                VkFramebuffer framebuffer,
                                const NovaRenderTarget& target,
                                uint32_t slot,
                                const std::function<void(VkCommandBuffer, uint32_t)>& render_callback)
{
    VkClearValue clear_values[1] = { target.clear };

    VkRenderPassBeginInfo pass_info = {
        .sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO,
        .renderPass = pass,
        .framebuffer = framebuffer,
        .renderArea = { .offset = {0, 0}, .extent = target.extent },
        .clearValueCount = 1,
        .pClearValues = clear_values
    };

    vkCmdBeginRenderPass(cmd, &pass_info, VK_SUBPASS_CONTENTS_INLINE);

    VkViewport viewport = {
        .x = 0.0f,
        .y = 0.0f,
        .width = static_cast<float>(target.extent.width),
        .height = static_cast<float>(target.extent.height),
        .minDepth = 0.0f,
        .maxDepth = 1.0f
    };
    vkCmdSetViewport(cmd, 0, 1, &viewport);

    VkRect2D scissor = { .offset = {0, 0}, .extent = target.extent };
    vkCmdSetScissor(cmd, 0, 1, &scissor);

    if (render_callback) {
        render_callback(cmd, slot);
    }

    vkCmdEndRenderPass(cmd);
}

/**
 * Hand the finished image to a consumer outside this VkDevice.
 *
 * A queue-family ownership RELEASE to VK_QUEUE_FAMILY_FOREIGN_EXT. The layout
 * does not change - the render pass already reached final_layout - but the
 * release is what tells the driver the next reader is not it, which is when a
 * private representation (radv's DCC on a compressed modifier) gets resolved.
 * Measured: without this, a DCC-modifier dmabuf reads back wrong.
 */
static void releaseToForeignConsumer(VkCommandBuffer cmd,
                                     const NovaRenderTarget& target,
                                     uint32_t graphics_family)
{
    VkImageMemoryBarrier release = {
        .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
        .srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT,
        .dstAccessMask = 0,
        .oldLayout = target.final_layout,
        .newLayout = target.final_layout,
        .srcQueueFamilyIndex = graphics_family,
        .dstQueueFamilyIndex = VK_QUEUE_FAMILY_FOREIGN_EXT,
        .image = target.image,
        .subresourceRange = {
            .aspectMask = VK_IMAGE_ASPECT_COLOR_BIT,
            .baseMipLevel = 0,
            .levelCount = 1,
            .baseArrayLayer = 0,
            .layerCount = 1
        }
    };

    vkCmdPipelineBarrier(cmd,
                         VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
                         VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT,
                         0, 0, nullptr, 0, nullptr, 1, &release);
}

// The pass and framebuffer this target needs, both from the cache. False when
// the target is unusable or either object could not be produced.
bool NovaGraphics::resolveOffscreenTarget(const NovaRenderTarget& target,
                                          VkRenderPass& pass_out,
                                          VkFramebuffer& framebuffer_out)
{
    if (!target.valid()) {
        report(LOGGER::ERROR, "renderToImage: target is incomplete (image/view/format/extent)");
        return false;
    }

    if (!offscreen_targets) {
        report(LOGGER::ERROR, "renderToImage: offscreen targets unavailable on this instance");
        return false;
    }

    pass_out = offscreen_targets->renderPass(target.format, target.final_layout);
    if (pass_out == VK_NULL_HANDLE) {
        return false;
    }

    framebuffer_out = offscreen_targets->framebuffer(pass_out, target);
    return framebuffer_out != VK_NULL_HANDLE;
}

// Reset this slot's command buffer and open it for one-time recording.
static VkCommandBuffer beginOffscreenRecording(FrameData& frame)
{
    VkCommandBuffer cmd = frame.cmd.buffer;
    VK_TRY(vkResetCommandBuffer(cmd, 0));

    VkCommandBufferBeginInfo begin_info = {
        .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
        .flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT
    };
    VK_TRY(vkBeginCommandBuffer(cmd, &begin_info));

    return cmd;
}

VkFence NovaGraphics::renderToImage(const NovaRenderTarget& target,
                                    std::function<void(VkCommandBuffer, uint32_t)>&& render_callback)
{
    VkRenderPass pass = VK_NULL_HANDLE;
    VkFramebuffer framebuffer = VK_NULL_HANDLE;
    if (!resolveOffscreenTarget(target, pass, framebuffer)) {
        return VK_NULL_HANDLE;
    }

    // Same frame-in-flight discipline as renderFrame: this slot's fence is
    // waited on here, so getCurrentFrameIndex() names a slot with no GPU
    // readers for the whole of the callback.
    FrameData& frame = current_frame();
    const uint32_t slot = getCurrentFrameIndex();

    VK_TRY(vkWaitForFences(logical_device, 1, &frame.in_flight, VK_TRUE, UINT64_MAX));
    VK_TRY(vkResetFences(logical_device, 1, &frame.in_flight));

    VkCommandBuffer cmd = beginOffscreenRecording(frame);

    recordOffscreenPass(cmd, pass, framebuffer, target, slot, render_callback);

    if (target.external_consumer) {
        if (hasDeviceExtension(VK_EXT_QUEUE_FAMILY_FOREIGN_EXTENSION_NAME)) {
            releaseToForeignConsumer(cmd, target, queues.indices.graphics_family.value());
        } else {
            report(LOGGER::WARN, "renderToImage: VK_EXT_queue_family_foreign absent - "
                                 "external consumers may read a private representation");
        }
    }

    VK_TRY(vkEndCommandBuffer(cmd));

    // No acquire to wait on and no present to signal: the fence is the whole
    // of the synchronisation at bring-up (plan D.2).
    VkSubmitInfo submit_info = {
        .sType = VK_STRUCTURE_TYPE_SUBMIT_INFO,
        .commandBufferCount = 1,
        .pCommandBuffers = &cmd
    };
    VK_TRY(vkQueueSubmit(graphics_queue, 1, &submit_info, frame.in_flight));

    frame_ct++;
    return frame.in_flight;
}
