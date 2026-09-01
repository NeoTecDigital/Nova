#include "./core_base.h"
#include "./components/vk_memory.h"
#include <SDL2/SDL_vulkan.h>
#include <set>
#include <string>
#include <stdexcept>

// Base class constructor
NovaCore::NovaCore(const std::string& debug_level)
{
    report(LOGGER::INFO, "NovaCore - Base initialization");
    _blankContext();
}

// Base class destructor
NovaCore::~NovaCore()
{
    report(LOGGER::INFO, "NovaCore - Base cleanup");

    // Flush legacy deletion queue
    queues.deletion.flush();

    // Resource registry will clean up automatically (RAII)
    // Cleans up in reverse order (LIFO)
}

void NovaCore::_blankContext()
{
    instance = VK_NULL_HANDLE;
    physical_device = VK_NULL_HANDLE;
    logical_device = VK_NULL_HANDLE;
    allocator = VK_NULL_HANDLE;

    queues.compute = VK_NULL_HANDLE;
    queues.transfer = VK_NULL_HANDLE;
    queues.graphics = VK_NULL_HANDLE;

    immediate = ImmediateContext{};
    graphics_immediate = ImmediateContext{};

    compute_pool = VK_NULL_HANDLE;
    transfer_pool = VK_NULL_HANDLE;

    window_extent = {0, 0};
}

void NovaCore::setWindowExtent(VkExtent2D extent)
{
    window_extent = extent;
}

// Vulkan instance creation
void NovaCore::createVulkanInstance(bool need_surface_extensions)
{
    report(LOGGER::VLINE, "\t .. Instantiating Vulkan Instance ..");

    VkApplicationInfo app_info = {
        .sType = VK_STRUCTURE_TYPE_APPLICATION_INFO,
        .pApplicationName = "Nova Engine",
        .applicationVersion = VK_MAKE_VERSION(1, 0, 0),
        .pEngineName = "Nova Engine",
        .engineVersion = VK_MAKE_VERSION(1, 0, 0),
        .apiVersion = VK_API_VERSION_1_2,
    };

    VkInstanceCreateInfo create_info = {
        .sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO,
        .pNext = nullptr,
        .pApplicationInfo = &app_info,
    };

    // Handle Extensions
    report(LOGGER::VERBOSE, "Vulkan: Checking for extensions ..");

    uint32_t extension_count = 0;
    std::vector<const char*> extensions;

    if (need_surface_extensions) {
        // Graphics mode: need SDL surface extensions
        SDL_Vulkan_GetInstanceExtensions(nullptr, &extension_count, nullptr);
        extensions.resize(extension_count);
        SDL_Vulkan_GetInstanceExtensions(nullptr, &extension_count, extensions.data());
    }

    report(LOGGER::VLINE, "\t .. %d extensions found", extension_count);
    for (const auto& ext : extensions) {
        report(LOGGER::VLINE, "\t\t%s", ext);
    }

    create_info.enabledExtensionCount = static_cast<uint32_t>(extensions.size());
    create_info.ppEnabledExtensionNames = extensions.data();

    // Validation layers (optional)
    const std::vector<const char*> VALIDATION_LAYERS = {};
    create_info.enabledLayerCount = static_cast<uint32_t>(VALIDATION_LAYERS.size());
    create_info.ppEnabledLayerNames = VALIDATION_LAYERS.data();

    VK_TRY(vkCreateInstance(&create_info, nullptr, &instance));

    // Register for cleanup
    resource_registry.register_resource("vulkan_instance", [this]() {
        if (instance != VK_NULL_HANDLE) {
            vkDestroyInstance(instance, nullptr);
            instance = VK_NULL_HANDLE;
        }
    });

    report(LOGGER::INFO, "Vulkan instance created");
}

// Validation layer support check
bool NovaCore::checkValidationLayerSupport()
{
    uint32_t layerCount;
    vkEnumerateInstanceLayerProperties(&layerCount, nullptr);

    std::vector<VkLayerProperties> availableLayers(layerCount);
    vkEnumerateInstanceLayerProperties(&layerCount, availableLayers.data());

    report(LOGGER::VERBOSE, "Checking for validation layers ..");
    report(LOGGER::VLINE, "Vulkan: %d layers supported:", layerCount);

    for (const auto& layer : availableLayers) {
        report(LOGGER::VLINE, "\t%s", layer.layerName);
    }

    return false;
}

// Queue family property setting
void NovaCore::setQueueFamilyProperties(unsigned int i, VkSurfaceKHR surface, bool need_presentation)
{
    VkQueueFamilyProperties* queue_family = &queues.families[i];
    std::string queue_name = "";

    if (queue_family->queueFlags & VK_QUEUE_GRAPHICS_BIT) {
        queue_name += "{ Graphics } ";
        queues.indices.graphics_family = i;
        queues.priorities.push_back(std::vector<float>(queue_family->queueCount, 1.0f));
        report(LOGGER::VLINE, "\t\tGraphics Family Set.");
    }

    if (queue_family->queueFlags & VK_QUEUE_COMPUTE_BIT) {
        queue_name += "{ Compute } ";
        // A disengaged graphics index compares unequal, so an unseen graphics
        // family also counts as "dedicated".
        if (queues.indices.graphics_family != i) {
            queues.indices.compute_family = i;
            queues.priorities.push_back(std::vector<float>(queue_family->queueCount, 1.0f));
            report(LOGGER::VLINE, "\t\tCompute Family Set.");
        }
    }

    if (queue_family->queueFlags & VK_QUEUE_TRANSFER_BIT) {
        queue_name += "{ Transfer } ";
        if (queues.indices.graphics_family != i) {
            queues.indices.transfer_family = i;
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

// Collects the distinct families that were actually found on the selected device.
static std::set<uint32_t> collectQueueFamilies(const QueueFamilyIndices& indices)
{
    std::set<uint32_t> families;

    for (const auto& family : { indices.graphics_family,
                                indices.present_family,
                                indices.transfer_family,
                                indices.compute_family }) {
        if (family.has_value()) {
            families.insert(family.value());
        }
    }

    return families;
}

// Get queue families
void NovaCore::getQueueFamilies(VkPhysicalDevice scanned_device, VkSurfaceKHR surface, bool need_presentation)
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

    for (int i = 0; i < queues.families.size(); i++) {
        report(LOGGER::VLINE, "\t\tQueue Family %d", i);

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

        setQueueFamilyProperties(i, surface, need_presentation);
    }

    backfillQueueFamilies(queues.indices);
}

// Device extension support check
static bool checkDeviceExtensionSupport(VkPhysicalDevice device, const std::vector<const char*>& required_extensions)
{
    uint32_t extensionCount;
    vkEnumerateDeviceExtensionProperties(device, nullptr, &extensionCount, nullptr);

    std::vector<VkExtensionProperties> availableExtensions(extensionCount);
    vkEnumerateDeviceExtensionProperties(device, nullptr, &extensionCount, availableExtensions.data());

    std::set<std::string> requiredExtensions(required_extensions.begin(), required_extensions.end());

    for (const auto& extension : availableExtensions) {
        requiredExtensions.erase(extension.extensionName);
    }

    return requiredExtensions.empty();
}

// Device provision check
bool NovaCore::deviceProvisioned(VkPhysicalDevice scanned_device, VkSurfaceKHR surface, bool need_swapchain)
{
    getQueueFamilies(scanned_device, surface, need_swapchain && surface != VK_NULL_HANDLE);

    // In compute-only mode, we don't need extensions or swapchain support
    if (!need_swapchain) {
        return queues.indices.isComplete(false);
    }

    // Graphics mode: check device extensions
    const std::vector<const char*> DEVICE_EXTENSIONS = {
        VK_KHR_SWAPCHAIN_EXTENSION_NAME
    };

    bool extensionsSupported = checkDeviceExtensionSupport(scanned_device, DEVICE_EXTENSIONS);
    return queues.indices.isComplete(true) && extensionsSupported;
}

static const char* deviceTypeName(VkPhysicalDeviceType type)
{
    switch (type) {
        case VK_PHYSICAL_DEVICE_TYPE_INTEGRATED_GPU: return "Integrated GPU";
        case VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU:   return "Discrete GPU";
        case VK_PHYSICAL_DEVICE_TYPE_VIRTUAL_GPU:    return "Virtual GPU";
        case VK_PHYSICAL_DEVICE_TYPE_CPU:            return "CPU";
        default:                                     return "Other";
    }
}

// Physical device creation
void NovaCore::createPhysicalDevice(bool need_presentation, VkSurfaceKHR surface)
{
    report(LOGGER::VLINE, "\t .. Selecting Physical Device ..");

    uint32_t deviceCount = 0;
    vkEnumeratePhysicalDevices(instance, &deviceCount, nullptr);

    if (deviceCount == 0) {
        report(LOGGER::ERROR, "Failed to find GPUs with Vulkan support!");
        throw std::runtime_error("No Vulkan-capable GPU found");
    }

    std::vector<VkPhysicalDevice> devices(deviceCount);
    vkEnumeratePhysicalDevices(instance, &deviceCount, devices.data());

    for (const auto& device : devices) {
        if (deviceProvisioned(device, surface, need_presentation)) {
            physical_device = device;
            break;
        }
    }

    if (physical_device == VK_NULL_HANDLE) {
        report(LOGGER::ERROR, "Failed to find a suitable GPU!");
        throw std::runtime_error("No suitable GPU found");
    }

    VkPhysicalDeviceProperties props;
    vkGetPhysicalDeviceProperties(physical_device, &props);
    report(LOGGER::INFO, "Selected GPU: %s", props.deviceName);
    report(LOGGER::INFO, "Selected GPU Type: %s", deviceTypeName(props.deviceType));

    if (props.deviceType == VK_PHYSICAL_DEVICE_TYPE_CPU) {
        report(LOGGER::WARN, "Selected GPU is a CPU software rasterizer - expect severely degraded throughput");
    }
}

// One queue per distinct family. `priority` is borrowed, not copied: it has to
// outlive the VkDeviceCreateInfo these infos are handed to.
static std::vector<VkDeviceQueueCreateInfo> buildQueueCreateInfos(const std::set<uint32_t>& families,
                                                                  const float& priority)
{
    std::vector<VkDeviceQueueCreateInfo> infos;
    infos.reserve(families.size());

    for (uint32_t family : families) {
        infos.push_back(VkDeviceQueueCreateInfo{
            .sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO,
            .queueFamilyIndex = family,
            .queueCount = 1,
            .pQueuePriorities = &priority
        });
    }

    return infos;
}

// Logical device creation
void NovaCore::createLogicalDevice(bool need_swapchain_extension)
{
    report(LOGGER::VLINE, "\t .. Creating Logical Device ..");

    // Only request families that were actually found (compute-only devices have no graphics family)
    const std::set<uint32_t> uniqueQueueFamilies = collectQueueFamilies(queues.indices);

    if (uniqueQueueFamilies.empty()) {
        report(LOGGER::ERROR, "No queue families resolved for the selected device!");
        throw std::runtime_error("No queue families resolved for the selected device");
    }

    const float queuePriority = 1.0f;
    createDeviceHandle(buildQueueCreateInfos(uniqueQueueFamilies, queuePriority),
                       need_swapchain_extension);

    acquireQueueHandles();

    // ResourceRegistry tears down LIFO: registration order is the INVERSE of
    // destruction order. vmaDestroyAllocator calls vkFreeMemory, so the device must
    // outlive the allocator -- register the device FIRST to have it destroyed LAST.
    resource_registry.register_resource("logical_device", [this]() {
        if (logical_device != VK_NULL_HANDLE) {
            vkDestroyDevice(logical_device, nullptr);
            logical_device = VK_NULL_HANDLE;
        }
    });

    createMemoryAllocator();

    report(LOGGER::INFO, "Logical device created");
}

void NovaCore::createDeviceHandle(const std::vector<VkDeviceQueueCreateInfo>& queue_infos,
                                  bool need_swapchain_extension)
{
    VkPhysicalDeviceFeatures deviceFeatures{};
    deviceFeatures.samplerAnisotropy = VK_TRUE;

    std::vector<const char*> deviceExtensions;
    if (need_swapchain_extension) {
        deviceExtensions.push_back(VK_KHR_SWAPCHAIN_EXTENSION_NAME);
    }

    VkDeviceCreateInfo createInfo = {
        .sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO,
        .queueCreateInfoCount = static_cast<uint32_t>(queue_infos.size()),
        .pQueueCreateInfos = queue_infos.data(),
        .enabledExtensionCount = static_cast<uint32_t>(deviceExtensions.size()),
        .ppEnabledExtensionNames = deviceExtensions.data(),
        .pEnabledFeatures = &deviceFeatures
    };

    VK_TRY(vkCreateDevice(physical_device, &createInfo, nullptr, &logical_device));
}

// One queue per resolved family, index 0. collectQueueFamilies() already asked
// createLogicalDevice for each of these, so a handle exists wherever an index does.
void NovaCore::acquireQueueHandles()
{
    vkGetDeviceQueue(logical_device, queues.indices.compute_family.value(), 0, &queues.compute);
    vkGetDeviceQueue(logical_device, queues.indices.transfer_family.value(), 0, &queues.transfer);

    // Index 0 of the graphics family is the same handle NovaGraphics renders on,
    // which is what lets an immediate submission be ordered against the frames.
    if (queues.indices.graphics_family.has_value()) {
        vkGetDeviceQueue(logical_device, queues.indices.graphics_family.value(), 0, &queues.graphics);
    }
}

void NovaCore::createMemoryAllocator()
{
    VmaAllocatorCreateInfo allocatorInfo = {
        .physicalDevice = physical_device,
        .device = logical_device,
        .instance = instance
    };
    VK_TRY(vmaCreateAllocator(&allocatorInfo, &allocator));

    // Registered after the device => destroyed before it.
    resource_registry.register_resource("vma_allocator", [this]() {
        if (allocator != VK_NULL_HANDLE) {
            vmaDestroyAllocator(allocator);
            allocator = VK_NULL_HANDLE;
        }
    });
}

// One immediate context: pool bound to `queue_family`, one primary buffer, one fence.
void NovaCore::buildImmediateContext(ImmediateContext& context, uint32_t queue_family, VkQueue queue)
{
    VkCommandPoolCreateInfo pool_info = {
        .sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO,
        .flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT,
        .queueFamilyIndex = queue_family
    };
    VK_TRY(vkCreateCommandPool(logical_device, &pool_info, nullptr, &context.pool));

    VkCommandBufferAllocateInfo cmd_info = {
        .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,
        .commandPool = context.pool,
        .level = VK_COMMAND_BUFFER_LEVEL_PRIMARY,
        .commandBufferCount = 1
    };
    VK_TRY(vkAllocateCommandBuffers(logical_device, &cmd_info, &context.cmd));

    VkFenceCreateInfo fence_info = {
        .sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO,
        .flags = VK_FENCE_CREATE_SIGNALED_BIT
    };
    VK_TRY(vkCreateFence(logical_device, &fence_info, nullptr, &context.fence));

    context.queue = queue;
}

void NovaCore::destroyImmediateContext(ImmediateContext& context)
{
    if (context.fence != VK_NULL_HANDLE) {
        vkDestroyFence(logical_device, context.fence, nullptr);
    }
    if (context.pool != VK_NULL_HANDLE) {
        vkDestroyCommandPool(logical_device, context.pool, nullptr);
    }
    context = ImmediateContext{};
}

// Immediate context creation (Phase 1 fix)
void NovaCore::createImmediateContext()
{
    report(LOGGER::VLINE, "\t .. Creating Immediate Context ..");

    buildImmediateContext(immediate, queues.indices.transfer_family.value(), queues.transfer);

    // A graphics family is optional: compute-only devices may expose none, and
    // isComplete(false) does not ask for one. Where it exists, the context is
    // built eagerly - it is the only legal home for barriers naming graphics
    // stages, and for writes to EXCLUSIVE images the frame submissions sample.
    if (queues.graphics != VK_NULL_HANDLE) {
        buildImmediateContext(graphics_immediate, queues.indices.graphics_family.value(), queues.graphics);
    } else {
        report(LOGGER::WARN, "No graphics family on this device - immediate graphics submissions fall back to transfer");
    }

    // Register for cleanup
    resource_registry.register_resource("immediate_context", [this]() {
        destroyImmediateContext(graphics_immediate);
        destroyImmediateContext(immediate);
    });

    report(LOGGER::INFO, "Immediate context created");
}

// Shared command pools creation
void NovaCore::createSharedCommandPools()
{
    report(LOGGER::VLINE, "\t .. Creating Shared Command Pools ..");

    // Compute pool
    VkCommandPoolCreateInfo compute_pool_info = {
        .sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO,
        .flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT,
        .queueFamilyIndex = queues.indices.compute_family.value()
    };
    VK_TRY(vkCreateCommandPool(logical_device, &compute_pool_info, nullptr, &compute_pool));

    // Transfer pool
    VkCommandPoolCreateInfo transfer_pool_info = {
        .sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO,
        .flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT,
        .queueFamilyIndex = queues.indices.transfer_family.value()
    };
    VK_TRY(vkCreateCommandPool(logical_device, &transfer_pool_info, nullptr, &transfer_pool));

    // Register for cleanup
    resource_registry.register_resource("shared_command_pools", [this]() {
        if (compute_pool != VK_NULL_HANDLE) {
            vkDestroyCommandPool(logical_device, compute_pool, nullptr);
            compute_pool = VK_NULL_HANDLE;
        }
        if (transfer_pool != VK_NULL_HANDLE) {
            vkDestroyCommandPool(logical_device, transfer_pool, nullptr);
            transfer_pool = VK_NULL_HANDLE;
        }
    });

    report(LOGGER::INFO, "Shared command pools created");
}

// Immediate submit implementation
void NovaCore::submitImmediate(ImmediateContext& context, const std::function<void(VkCommandBuffer)>& func)
{
    VK_TRY(vkResetFences(logical_device, 1, &context.fence));
    VK_TRY(vkResetCommandBuffer(context.cmd, 0));

    VkCommandBufferBeginInfo begin_info = {
        .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
        .flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT
    };

    VK_TRY(vkBeginCommandBuffer(context.cmd, &begin_info));
    func(context.cmd);
    VK_TRY(vkEndCommandBuffer(context.cmd));

    VkSubmitInfo submit_info = {
        .sType = VK_STRUCTURE_TYPE_SUBMIT_INFO,
        .commandBufferCount = 1,
        .pCommandBuffers = &context.cmd
    };

    VK_TRY(vkQueueSubmit(context.queue, 1, &submit_info, context.fence));
    VK_TRY(vkWaitForFences(logical_device, 1, &context.fence, VK_TRUE, UINT64_MAX));
}

void NovaCore::immediateSubmit(std::function<void(VkCommandBuffer)>&& func)
{
    submitImmediate(immediate, func);
}

void NovaCore::immediateSubmitGraphics(std::function<void(VkCommandBuffer)>&& func)
{
    submitImmediate(hasGraphicsImmediate() ? graphics_immediate : immediate, func);
}

// Buffer creation
Buffer_T NovaCore::createEphemeralBuffer(size_t size, VkBufferUsageFlags flags, VmaMemoryUsage usage)
{
    VkBufferCreateInfo buffer_info = {
        .sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
        .size = size,
        .usage = flags,
        .sharingMode = VK_SHARING_MODE_EXCLUSIVE
    };

    VmaAllocationCreateInfo alloc_info = {
        .usage = usage
    };

    // pAllocationInfo must be supplied: Buffer_T::info is what callers read to get
    // the mapped pointer, offset and real size. Passing nullptr left it uninitialized.
    Buffer_T buffer{};
    VK_TRY(vmaCreateBuffer(allocator, &buffer_info, &alloc_info, &buffer.buffer, &buffer.allocation, &buffer.info));

    return buffer;
}
