// Written by Richard Christopher, Copyright 2026 NeoTec Digital
//
// Physical-device selection (plan section D.3): queue-family discovery, the
// suitability gates, the type scoring, the DRM-node tie-break and the
// optional-extension resolution. Split out of core_base.cpp, which now carries
// only the instance/device/allocator lifecycle - the two answer different
// questions ("which device?" versus "how is it set up?") and neither file
// stays under its size limit holding both.

#include "./core_base.h"

#include <set>
#include <string>
#include <stdexcept>
#include <sys/stat.h>
#include <sys/sysmacros.h>
namespace Nova {
void Core::setQueueFamilyProperties(uint32_t index)
{
    const VkQueueFamilyProperties* queue_family = &queues.families[index];
    std::string queue_name = "";

    if (queue_family->queueFlags & VK_QUEUE_GRAPHICS_BIT) {
        queue_name += "{ Graphics } ";
        queues.indices.graphics_family = index;
        queues.priorities.push_back(std::vector<float>(queue_family->queueCount, 1.0f));
        report(LOGGER::VLINE, "\t\tGraphics Family Set.");
    }

    if (queue_family->queueFlags & VK_QUEUE_COMPUTE_BIT) {
        queue_name += "{ Compute } ";
        // A disengaged graphics index compares unequal, so an unseen graphics
        // family also counts as "dedicated".
        if (queues.indices.graphics_family != index) {
            queues.indices.compute_family = index;
            queues.priorities.push_back(std::vector<float>(queue_family->queueCount, 1.0f));
            report(LOGGER::VLINE, "\t\tCompute Family Set.");
        }
    }

    if (queue_family->queueFlags & VK_QUEUE_TRANSFER_BIT) {
        queue_name += "{ Transfer } ";
        if (queues.indices.graphics_family != index) {
            queues.indices.transfer_family = index;
            queues.priorities.push_back(std::vector<float>(queue_family->queueCount, 1.0f));
            report(LOGGER::VLINE, "\t\tTransfer Family Set.");
        }
    }

    if (queue_family->queueFlags & VK_QUEUE_SPARSE_BINDING_BIT) {
        queue_name += "{ Sparse Binding } ";
    }

    if (queue_name.empty()) {
        queue_name = "~ Unknown ~";
    }

    report(LOGGER::VLINE, "\t\t\tQueue Count: %d", queue_family->queueCount);
    report(LOGGER::VLINE, "\t\t\t %s", queue_name.c_str());
}

// Vulkan guarantees a graphics family also supports compute and transfer, so it backs
// any family that has no dedicated candidate; a compute family backs transfer.
static void backfillQueueFamilies(QueueFamilyIndices& indices)
{
    if (indices.graphics_family.has_value()) {
        if (!indices.compute_family.has_value()) {
            report(LOGGER::VLINE, "\t\tNo Dedicated Compute Family. Falling Back to Graphics Family.");
            indices.compute_family = indices.graphics_family;
        }

        if (!indices.transfer_family.has_value()) {
            report(LOGGER::VLINE, "\t\tNo Dedicated Transfer Family. Falling Back to Graphics Family.");
            indices.transfer_family = indices.graphics_family;
        }
    } else if (indices.compute_family.has_value() && !indices.transfer_family.has_value()) {
        report(LOGGER::VLINE, "\t\tNo Dedicated Transfer Family. Falling Back to Compute Family.");
        indices.transfer_family = indices.compute_family;
    }
}

// Get queue families
void Core::getQueueFamilies(VkPhysicalDevice scanned_device, VkSurfaceKHR surface, bool need_presentation)
{
    report(LOGGER::VLINE, "\t .. Acquiring Queue Families ..");

    // Each candidate device is scanned from scratch; leftovers from a rejected
    // device would otherwise make the next one look provisioned.
    queues.indices.reset();
    queues.priorities.clear();

    uint32_t queue_family_count = 0;
    vkGetPhysicalDeviceQueueFamilyProperties(scanned_device, &queue_family_count, nullptr);
    queues.families.resize(queue_family_count);
    vkGetPhysicalDeviceQueueFamilyProperties(scanned_device, &queue_family_count, queues.families.data());

    for (uint32_t i = 0; i < queue_family_count; i++) {
        report(LOGGER::VLINE, "\t\tQueue Family %u", i);

        // Presentation is only meaningful against a surface; compute-only devices
        // leave present_family disengaged.
        if (need_presentation && surface != VK_NULL_HANDLE && !queues.indices.present_family.has_value()) {
            VkBool32 present_support = false;
            vkGetPhysicalDeviceSurfaceSupportKHR(scanned_device, i, surface, &present_support);

            if (present_support) {
                queues.indices.present_family = i;
                report(LOGGER::VLINE, "\t\tPresent Family Set.");
            }
        }

        setQueueFamilyProperties(i);
    }

    backfillQueueFamilies(queues.indices);
}

const char* deviceTypeName(VkPhysicalDeviceType type)
{
    switch (type) {
        case VK_PHYSICAL_DEVICE_TYPE_INTEGRATED_GPU: return "Integrated GPU";
        case VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU:   return "Discrete GPU";
        case VK_PHYSICAL_DEVICE_TYPE_VIRTUAL_GPU:    return "Virtual GPU";
        case VK_PHYSICAL_DEVICE_TYPE_CPU:            return "CPU";
        default:                                     return "Other";
    }
}

uint32_t deviceTypeScore(VkPhysicalDeviceType type)
{
    switch (type) {
        case VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU:   return 400;
        case VK_PHYSICAL_DEVICE_TYPE_VIRTUAL_GPU:    return 300;
        case VK_PHYSICAL_DEVICE_TYPE_INTEGRATED_GPU: return 200;
        case VK_PHYSICAL_DEVICE_TYPE_CPU:            return 100;
        default:                                     return 50;
    }
}

// Worth less than one type step, so it only ever separates equals.
static constexpr uint32_t DRM_MATCH_BONUS = 50;

static bool deviceHasExtension(VkPhysicalDevice device, const char* name)
{
    uint32_t count = 0;
    vkEnumerateDeviceExtensionProperties(device, nullptr, &count, nullptr);

    std::vector<VkExtensionProperties> available(count);
    vkEnumerateDeviceExtensionProperties(device, nullptr, &count, available.data());

    for (const auto& extension : available) {
        if (std::string(extension.extensionName) == name) {
            return true;
        }
    }

    return false;
}

bool drmNodeOf(int drm_fd, int64_t& major_out, int64_t& minor_out)
{
    if (drm_fd < 0) {
        return false;
    }

    struct stat node = {};
    if (fstat(drm_fd, &node) != 0 || !S_ISCHR(node.st_mode)) {
        return false;
    }

    major_out = static_cast<int64_t>(major(node.st_rdev));
    minor_out = static_cast<int64_t>(minor(node.st_rdev));
    return true;
}

bool deviceOwnsDrmNode(VkPhysicalDevice device, int64_t node_major, int64_t node_minor)
{
    if (!deviceHasExtension(device, VK_EXT_PHYSICAL_DEVICE_DRM_EXTENSION_NAME)) {
        return false;
    }

    VkPhysicalDeviceDrmPropertiesEXT drm = { .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DRM_PROPERTIES_EXT };
    VkPhysicalDeviceProperties2 props = {
        .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2,
        .pNext = &drm
    };
    vkGetPhysicalDeviceProperties2(device, &props);

    const bool primary_match = drm.hasPrimary &&
                               drm.primaryMajor == node_major && drm.primaryMinor == node_minor;
    const bool render_match = drm.hasRender &&
                              drm.renderMajor == node_major && drm.renderMinor == node_minor;

    return primary_match || render_match;
}

// deviceName + deviceType + driver, unconditionally at INFO (§D.3).
static void reportCandidate(VkPhysicalDevice device, uint32_t score, bool drm_match)
{
    VkPhysicalDeviceProperties base = {};
    vkGetPhysicalDeviceProperties(device, &base);

    const char* driver = "unreported";
    VkPhysicalDeviceDriverProperties driver_props = {
        .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DRIVER_PROPERTIES
    };

    // VkPhysicalDeviceDriverProperties is core in 1.2 only; do not chain it at
    // a device that never promised to read it.
    if (base.apiVersion >= VK_API_VERSION_1_2) {
        VkPhysicalDeviceProperties2 props = {
            .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2,
            .pNext = &driver_props
        };
        vkGetPhysicalDeviceProperties2(device, &props);
        driver = driver_props.driverName;
    }

    report(LOGGER::INFO, "GPU candidate: %s | %s | driver %s | score %u%s",
           base.deviceName, deviceTypeName(base.deviceType), driver, score,
           drm_match ? " | DRM node match" : "");
}

std::vector<std::string> Core::resolveOptionalExtensions(VkPhysicalDevice device,
                                                             const std::vector<const char*>& wanted)
{
    std::vector<std::string> resolved;

    for (const char* name : wanted) {
        if (deviceHasExtension(device, name)) {
            resolved.emplace_back(name);
        } else {
            report(LOGGER::WARN, "Device extension unavailable: %s - dependent features stay disabled", name);
        }
    }

    return resolved;
}

bool Core::hasDeviceExtension(const char* name) const
{
    for (const auto& enabled : enabled_device_extensions) {
        if (enabled == name) {
            return true;
        }
    }

    return false;
}

bool Core::deviceProvisioned(VkPhysicalDevice scanned_device, const DeviceRequest& request)
{
    const bool wants_present = request.need_presentation && request.surface != VK_NULL_HANDLE;
    getQueueFamilies(scanned_device, request.surface, wants_present);

    if (request.need_presentation) {
        return queues.indices.isComplete(true) &&
               deviceHasExtension(scanned_device, VK_KHR_SWAPCHAIN_EXTENSION_NAME);
    }

    // Offscreen rendering needs a graphics family without ever needing a
    // present family; compute-only needs neither.
    return queues.indices.isComplete(false) &&
           (!request.need_graphics || queues.indices.graphics_family.has_value());
}

bool Core::deviceProvisioned(VkPhysicalDevice scanned_device, VkSurfaceKHR surface, bool need_swapchain)
{
    return deviceProvisioned(scanned_device, DeviceRequest{
        .need_presentation = need_swapchain,
        .need_graphics = need_swapchain,
        .surface = surface
    });
}

// Every candidate that passes the gates, scored and reported.
static VkPhysicalDevice highestScoring(const std::vector<VkPhysicalDevice>& candidates,
                                       const std::vector<uint32_t>& scores)
{
    VkPhysicalDevice winner = VK_NULL_HANDLE;
    uint32_t best = 0;

    for (size_t i = 0; i < candidates.size(); i++) {
        if (winner == VK_NULL_HANDLE || scores[i] > best) {
            winner = candidates[i];
            best = scores[i];
        }
    }

    return winner;
}

void Core::createPhysicalDevice(const DeviceRequest& request)
{
    report(LOGGER::VLINE, "\t .. Selecting Physical Device ..");

    uint32_t device_count = 0;
    vkEnumeratePhysicalDevices(instance, &device_count, nullptr);

    if (device_count == 0) {
        report(LOGGER::ERROR, "Failed to find GPUs with Vulkan support!");
        throw std::runtime_error("No Vulkan-capable GPU found");
    }

    std::vector<VkPhysicalDevice> devices(device_count);
    vkEnumeratePhysicalDevices(instance, &device_count, devices.data());

    int64_t node_major = 0;
    int64_t node_minor = 0;
    const bool has_node_hint = drmNodeOf(request.drm_fd, node_major, node_minor);

    std::vector<VkPhysicalDevice> passed;
    std::vector<uint32_t> scores;

    for (VkPhysicalDevice device : devices) {
        if (!deviceProvisioned(device, request)) {
            continue;
        }

        VkPhysicalDeviceProperties props = {};
        vkGetPhysicalDeviceProperties(device, &props);

        const bool drm_match = has_node_hint && deviceOwnsDrmNode(device, node_major, node_minor);
        const uint32_t score = deviceTypeScore(props.deviceType) + (drm_match ? DRM_MATCH_BONUS : 0);

        reportCandidate(device, score, drm_match);
        passed.push_back(device);
        scores.push_back(score);
    }

    physical_device = highestScoring(passed, scores);
    finalizeDeviceSelection(request);
}

// The scoring loop left queues.indices describing whichever candidate it scanned
// last, so the winner is re-scanned before anything reads them.
void Core::finalizeDeviceSelection(const DeviceRequest& request)
{
    if (physical_device == VK_NULL_HANDLE) {
        report(LOGGER::ERROR, "Failed to find a suitable GPU!");
        throw std::runtime_error("No suitable GPU found");
    }

    if (!deviceProvisioned(physical_device, request)) {
        report(LOGGER::ERROR, "Selected GPU stopped satisfying its own requirements on rescan!");
        throw std::runtime_error("Physical device rescan disagreed with selection");
    }

    enabled_device_extensions = resolveOptionalExtensions(physical_device, request.optional_extensions);

    VkPhysicalDeviceProperties props = {};
    vkGetPhysicalDeviceProperties(physical_device, &props);
    report(LOGGER::INFO, "Selected GPU: %s", props.deviceName);
    report(LOGGER::INFO, "Selected GPU Type: %s", deviceTypeName(props.deviceType));

    if (props.deviceType == VK_PHYSICAL_DEVICE_TYPE_CPU) {
        report(LOGGER::WARN, "Selected GPU is a CPU software rasterizer - expect severely degraded throughput");
    }
}

void Core::createPhysicalDevice(bool need_presentation, VkSurfaceKHR surface)
{
    createPhysicalDevice(DeviceRequest{
        .need_presentation = need_presentation,
        .need_graphics = need_presentation,
        .surface = surface
    });
}

} // namespace Nova
