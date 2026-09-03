// Written by Richard Christopher, Copyright 2026 NeoTec Digital
#pragma once

#include <vulkan/vulkan.h>
#include <vector>
namespace Nova {
/**
 * What a mode needs from a physical device, and what it would like.
 *
 * The three booleans are gates: a candidate that fails one is not scored at all.
 * `drm_fd` and `optional_extensions` are preferences: they steer the choice
 * between candidates that already passed, and never reject one.
 */
struct DeviceRequest {
    // Requires a present family against `surface` and VK_KHR_swapchain.
    bool need_presentation = false;

    // Requires a graphics family. Implied by need_presentation; stated
    // separately because offscreen rendering needs graphics without present.
    bool need_graphics = false;

    // Only meaningful when need_presentation is set.
    VkSurfaceKHR surface = VK_NULL_HANDLE;

    /**
     * Borrowed DRM node fd used as the tie-break, never closed here.
     *
     * A dmabuf allocated on one node and imported on another either fails or
     * silently copies, so when the caller already holds the node its buffers
     * come from, the device that owns that node wins ties.
     */
    int drm_fd = -1;

    // Enabled on the logical device when the winner reports them; absence is
    // reported at INFO and degrades the feature that asked, not the boot.
    std::vector<const char*> optional_extensions;
};

// "Discrete GPU", "Virtual GPU", ... - stable spelling for the INFO report.
const char* deviceTypeName(VkPhysicalDeviceType type);

/**
 * Preference order of the D.3 report: DISCRETE > VIRTUAL > INTEGRATED > CPU.
 *
 * Spaced so a DRM-node match can only break a tie between equal types and can
 * never promote a CPU rasterizer over a real GPU.
 */
uint32_t deviceTypeScore(VkPhysicalDeviceType type);

// Major/minor of the node behind `drm_fd`, or false when it is not a device fd.
bool drmNodeOf(int drm_fd, int64_t& major, int64_t& minor);

// True when `device` reports VK_EXT_physical_device_drm nodes matching major/minor.
bool deviceOwnsDrmNode(VkPhysicalDevice device, int64_t major, int64_t minor);

} // namespace Nova
