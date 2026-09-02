#include "./core_base.h"
#include "./components/vk_memory.h"
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

// Vulkan instance creation.
//
// Extensions arrive as data. This translation unit deliberately links no window
// system: the SDL query that used to live here is now
// NovaSDL::vulkanInstanceExtensions (Core/nova_sdl.cpp), so `vazio` and every
// other windowless consumer of libNova.a stops carrying libSDL2.
void NovaCore::createVulkanInstance(const std::vector<const char*>& instance_extensions)
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

    report(LOGGER::VLINE, "\t .. %zu extensions requested", instance_extensions.size());
    for (const auto& ext : instance_extensions) {
        report(LOGGER::VLINE, "\t\t%s", ext);
    }

    create_info.enabledExtensionCount = static_cast<uint32_t>(instance_extensions.size());
    create_info.ppEnabledExtensionNames = instance_extensions.data();

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

    // Resolved at selection time against what the winning device reports, so
    // every name here is already known to exist on `physical_device`.
    for (const auto& optional : enabled_device_extensions) {
        deviceExtensions.push_back(optional.c_str());
        report(LOGGER::VLINE, "\t\t.. Enabling device extension %s", optional.c_str());
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

    // pAllocationInfo must be supplied so Buffer_T::info describes the real
    // allocation - memory, offset, size - instead of holding indeterminate bytes.
    //
    // info.pMappedData is NOT one of those fields: VMA only fills it for an
    // allocation created with VMA_ALLOCATION_CREATE_MAPPED_BIT, which this
    // function deliberately does not request, so it is always null here. A
    // caller that needs the memory mapped owns that mapping itself, via
    // vmaMapMemory / vmaUnmapMemory (SpatialPresentLoop's sidecar readback is
    // the one such caller today).
    Buffer_T buffer{};
    VK_TRY(vmaCreateBuffer(allocator, &buffer_info, &alloc_info, &buffer.buffer, &buffer.allocation, &buffer.info));

    return buffer;
}
