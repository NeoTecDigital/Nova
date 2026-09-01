// Written by Richard Christopher, Copyright 2026 NeoTec Digital
//
// Swapchain lifecycle for the surface-backed (SDL / external VkSurfaceKHR)
// path. Split out of nova_graphics.cpp when offscreen mode landed: that file
// now carries the frame loop shared by both modes, this one everything that
// only exists when a surface does.

#include "./nova_graphics.h"
#include <set>

// Swapchain creation (placeholder - will migrate from existing code)
void NovaGraphics::createSwapchain()
{
    report(LOGGER::VLINE, "\t .. Creating Swapchain ..");

    swapchain.support = querySwapChainSupport(physical_device);
    querySwapChainDetails();

    swapchain.extent = window_extent;
    swapchain.format = swapchain.details.surface_format.format;

    uint32_t image_count = swapchain.support.capabilities.minImageCount + 1;
    if (swapchain.support.capabilities.maxImageCount > 0 && image_count > swapchain.support.capabilities.maxImageCount) {
        image_count = swapchain.support.capabilities.maxImageCount;
    }

    VkSwapchainCreateInfoKHR create_info = {};
    createSwapchainInfoKHR(&create_info, image_count);

    VK_TRY(vkCreateSwapchainKHR(logical_device, &create_info, nullptr, &swapchain.instance));

    // Get swapchain images
    vkGetSwapchainImagesKHR(logical_device, swapchain.instance, &image_count, nullptr);
    swapchain.images.resize(image_count);
    vkGetSwapchainImagesKHR(logical_device, swapchain.instance, &image_count, swapchain.images.data());

    report(LOGGER::INFO, "Swapchain created with %d images", image_count);
}

SwapChainSupportDetails NovaGraphics::querySwapChainSupport(VkPhysicalDevice device)
{
    SwapChainSupportDetails details = {};
    vkGetPhysicalDeviceSurfaceCapabilitiesKHR(device, surface, &details.capabilities);

    uint32_t format_count;
    vkGetPhysicalDeviceSurfaceFormatsKHR(device, surface, &format_count, nullptr);
    if (format_count != 0) {
        details.formats.resize(format_count);
        vkGetPhysicalDeviceSurfaceFormatsKHR(device, surface, &format_count, details.formats.data());
    }

    uint32_t present_mode_count;
    vkGetPhysicalDeviceSurfacePresentModesKHR(device, surface, &present_mode_count, nullptr);
    if (present_mode_count != 0) {
        details.present_modes.resize(present_mode_count);
        vkGetPhysicalDeviceSurfacePresentModesKHR(device, surface, &present_mode_count, details.present_modes.data());
    }

    return details;
}

void NovaGraphics::querySwapChainDetails()
{
    report(LOGGER::VLINE, "\t .. Querying SwapChain Details ..");

    if (swapchain.support.formats.empty() || swapchain.support.present_modes.empty()) {
        report(LOGGER::ERROR, "Vulkan: SwapChain support is not available.");
        return;
    }

    // Select surface format (prefer B8G8R8A8_SRGB)
    swapchain.details.surface_format = swapchain.support.formats.front();
    for (const auto& format : swapchain.support.formats) {
        if (format.format == VK_FORMAT_B8G8R8A8_SRGB &&
            format.colorSpace == VK_COLOR_SPACE_SRGB_NONLINEAR_KHR) {
            swapchain.details.surface_format = format;
            break;
        }
    }

    // Select present mode (prefer MAILBOX, fallback to FIFO)
    swapchain.details.present_mode = VK_PRESENT_MODE_FIFO_KHR;
    for (const auto& mode : swapchain.support.present_modes) {
        if (mode == VK_PRESENT_MODE_MAILBOX_KHR) {
            swapchain.details.present_mode = mode;
            break;
        }
    }

    // Select extent
    swapchain.details.extent = window_extent;
    if (swapchain.support.capabilities.currentExtent.width != UINT32_MAX) {
        swapchain.details.extent = swapchain.support.capabilities.currentExtent;
    } else {
        swapchain.details.extent.width = std::max(
            swapchain.support.capabilities.minImageExtent.width,
            std::min(swapchain.support.capabilities.maxImageExtent.width, window_extent.width)
        );
        swapchain.details.extent.height = std::max(
            swapchain.support.capabilities.minImageExtent.height,
            std::min(swapchain.support.capabilities.maxImageExtent.height, window_extent.height)
        );
    }
}

void NovaGraphics::createSwapchainInfoKHR(VkSwapchainCreateInfoKHR* create_info, uint32_t image_count)
{
    std::set<uint32_t> unique_queue_families = {
        queues.indices.graphics_family.value(),
        queues.indices.present_family.value()
    };
    std::vector<uint32_t> queue_families(unique_queue_families.begin(), unique_queue_families.end());

    *create_info = {
        .sType = VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR,
        .surface = surface,
        .minImageCount = image_count,
        .imageFormat = swapchain.details.surface_format.format,
        .imageColorSpace = swapchain.details.surface_format.colorSpace,
        .imageExtent = swapchain.extent,
        .imageArrayLayers = 1,
        .imageUsage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT,
        .preTransform = swapchain.support.capabilities.currentTransform,
        .compositeAlpha = VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR,
        .presentMode = swapchain.details.present_mode,
        .clipped = VK_TRUE,
        .oldSwapchain = VK_NULL_HANDLE
    };

    if (queue_families.size() > 1) {
        create_info->imageSharingMode = VK_SHARING_MODE_CONCURRENT;
        create_info->queueFamilyIndexCount = static_cast<uint32_t>(queue_families.size());
        create_info->pQueueFamilyIndices = queue_families.data();
    } else {
        create_info->imageSharingMode = VK_SHARING_MODE_EXCLUSIVE;
        create_info->queueFamilyIndexCount = 0;
        create_info->pQueueFamilyIndices = nullptr;
    }
}

void NovaGraphics::createImageViews()
{
    report(LOGGER::VLINE, "\t .. Creating Image Views ..");

    swapchain.image_views.resize(swapchain.images.size());

    for (size_t i = 0; i < swapchain.images.size(); i++) {
        VkImageViewCreateInfo create_info = {
            .sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,
            .image = swapchain.images[i],
            .viewType = VK_IMAGE_VIEW_TYPE_2D,
            .format = swapchain.format,
            .components = {
                .r = VK_COMPONENT_SWIZZLE_IDENTITY,
                .g = VK_COMPONENT_SWIZZLE_IDENTITY,
                .b = VK_COMPONENT_SWIZZLE_IDENTITY,
                .a = VK_COMPONENT_SWIZZLE_IDENTITY
            },
            .subresourceRange = {
                .aspectMask = VK_IMAGE_ASPECT_COLOR_BIT,
                .baseMipLevel = 0,
                .levelCount = 1,
                .baseArrayLayer = 0,
                .layerCount = 1
            }
        };

        VK_TRY(vkCreateImageView(logical_device, &create_info, nullptr, &swapchain.image_views[i]));
    }

    report(LOGGER::INFO, "Created %zu image views", swapchain.image_views.size());
}

void NovaGraphics::createFramebuffers()
{
    report(LOGGER::VLINE, "\t .. Creating Framebuffers ..");

    swapchain.framebuffers.resize(swapchain.image_views.size());

    for (size_t i = 0; i < swapchain.image_views.size(); i++) {
        VkImageView attachments[] = { swapchain.image_views[i] };

        VkFramebufferCreateInfo framebuffer_info = {
            .sType = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO,
            .renderPass = render_pass,
            .attachmentCount = 1,
            .pAttachments = attachments,
            .width = swapchain.extent.width,
            .height = swapchain.extent.height,
            .layers = 1
        };

        VK_TRY(vkCreateFramebuffer(logical_device, &framebuffer_info, nullptr, &swapchain.framebuffers[i]));
    }

    report(LOGGER::INFO, "Created %zu framebuffers", swapchain.framebuffers.size());
}

void NovaGraphics::recreateSwapchain()
{
    report(LOGGER::DEBUG, "NovaGraphics::recreateSwapchain() - not yet implemented");
    // Placeholder - will migrate from existing code
}
