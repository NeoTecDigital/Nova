#include "Core/core.h"
#include "Core/components/vk_memory.h"

#include <set>
#include <stdexcept>
#include <string>


    //////////////////////
    // VALIDATION LAYER //
    //////////////////////

bool NovaCoreLegacy::checkValidationLayerSupport() 
    {
        uint32_t layerCount;
        vkEnumerateInstanceLayerProperties(&layerCount, nullptr);

        std::vector<VkLayerProperties> availableLayers(layerCount);
        vkEnumerateInstanceLayerProperties(&layerCount, availableLayers.data());
        report(LOGGER::VERBOSE, "Checking for validation layers ..");
        report(LOGGER::VLINE, "Vulkan: %d layers supported:", layerCount);
        for (const auto& layer : availableLayers) 
            { report(LOGGER::VLINE, "\t%s", layer.layerName); }

        return false;
    }


    ////////////////////
    //  Device Queues //
    ////////////////////

void NovaCoreLegacy::setQueueFamilyProperties(unsigned int i) {
    VkQueueFamilyProperties* queue_family = &queues.families[i];
    std::string queue_name = "";

    if (queue_family->queueFlags & VK_QUEUE_GRAPHICS_BIT) 
        { 
            queue_name += "{ Graphics } "; 
            queues.indices.graphics_family = i;
            queues.priorities.push_back(std::vector<float>(queue_family->queueCount, 1.0f));
            report(LOGGER::VLINE, "\t\tGraphics Family Set.");
        }

    if (queue_family->queueFlags & VK_QUEUE_COMPUTE_BIT) 
        { 
            queue_name += "{ Compute } "; 

            // A disengaged graphics index compares unequal, so an unseen graphics
            // family also counts as "dedicated".
            if (queues.indices.graphics_family != i) 
                {
                    queues.indices.compute_family = i;
                    queues.priorities.push_back(std::vector<float>(queue_family->queueCount, 1.0f));
                    report(LOGGER::VLINE, "\t\tCompute Family Set.");
                }
        }

    if (queue_family->queueFlags & VK_QUEUE_TRANSFER_BIT) 
        { 
            queue_name += "{ Transfer } "; 

            if (queues.indices.graphics_family != i) 
                {
                    queues.indices.transfer_family = i;
                    queues.priorities.push_back(std::vector<float>(queue_family->queueCount, 1.0f));
                    report(LOGGER::VLINE, "\t\tTransfer Family Set.");
                }
        }

    if (queue_family->queueFlags & VK_QUEUE_SPARSE_BINDING_BIT) 
        { queue_name += "{ Sparse Binding } "; }

    if (queue_name.empty()) 
        { queue_name = "~ Unknown ~"; }


    report(LOGGER::VLINE, "\t\t\tQueue Count: %d", queue_family->queueCount);
    report(LOGGER::VLINE, "\t\t\t %s", queue_name.c_str());
}

// Vulkan guarantees a graphics family also supports compute and transfer, so it backs
// any family that has no dedicated candidate; a compute family backs transfer.
static void backfillQueueFamilies(QueueFamilyIndices& indices)
    {
        if (indices.graphics_family.has_value()) 
            { 
                if (!indices.compute_family.has_value()) 
                    {
                        report(LOGGER::VLINE, "\t\tNo Dedicated Compute Family. Falling Back to Graphics Family.");
                        indices.compute_family = indices.graphics_family;
                    }

                if (!indices.transfer_family.has_value()) 
                    {
                        report(LOGGER::VLINE, "\t\tNo Dedicated Transfer Family. Falling Back to Graphics Family.");
                        indices.transfer_family = indices.graphics_family;
                    }
            }
        else if (indices.compute_family.has_value() && !indices.transfer_family.has_value()) 
            {
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
                                    indices.compute_family,
                                    indices.transfer_family })
            { if (family.has_value()) { families.insert(family.value()); } }

        return families;
    }

void NovaCoreLegacy::getQueueFamilies(VkPhysicalDevice scanned_device) 
    {
        report(LOGGER::VLINE, "\t .. Acquiring Queue Families ..");

        // Each candidate device is scanned from scratch; leftovers from a rejected
        // device would otherwise make the next one look provisioned.
        queues.indices.reset();
        queues.priorities.clear();

        uint32_t _queue_family_count = 0;
        vkGetPhysicalDeviceQueueFamilyProperties(scanned_device, &_queue_family_count, nullptr);
        queues.families.resize(_queue_family_count);
        vkGetPhysicalDeviceQueueFamilyProperties(scanned_device, &_queue_family_count, queues.families.data());

        for (int i = 0; i < queues.families.size(); i++)
            {
                report(LOGGER::VLINE, "\t\tQueue Family %d", i);

                // Presentation is only meaningful against a surface; compute-only devices
                // leave present_family disengaged.
                if (!compute_only && surface != VK_NULL_HANDLE && !queues.indices.present_family.has_value()) 
                    {
                        VkBool32 _present_support = false;
                        vkGetPhysicalDeviceSurfaceSupportKHR(scanned_device, i, surface, &_present_support);

                        if (_present_support)
                            {
                                queues.indices.present_family = i;
                                report(LOGGER::VLINE, "\t\tPresent Family Set.");
                            }
                    }

                setQueueFamilyProperties(i);
            }

        backfillQueueFamilies(queues.indices);
    }

static bool checkDeviceExtensionSupport(VkPhysicalDevice device) 
    {
        uint32_t extensionCount;
        vkEnumerateDeviceExtensionProperties(device, nullptr, &extensionCount, nullptr);

        std::vector<VkExtensionProperties> availableExtensions(extensionCount);
        vkEnumerateDeviceExtensionProperties(device, nullptr, &extensionCount, availableExtensions.data());

        std::set<std::string> requiredExtensions(DEVICE_EXTENSIONS.begin(), DEVICE_EXTENSIONS.end());

        for (const auto& extension : availableExtensions) 
            { requiredExtensions.erase(extension.extensionName); }

        return requiredExtensions.empty();
    }

    ////////////////////////
    //  DEVICE PROVISION  //
    ////////////////////////

bool NovaCoreLegacy::deviceProvisioned(VkPhysicalDevice scanned_device)
    {
        getQueueFamilies(scanned_device);

        // In compute-only mode, we don't need extensions or swapchain support
        if (compute_only) {
            return queues.indices.isComplete(false);
        }

        // Graphics mode: check extensions and swapchain
        bool extensions_supported = checkDeviceExtensionSupport(scanned_device);

        bool swap_chain_adequate = false;
        if (extensions_supported)
            {
                SwapChainSupportDetails swap_chain_support = querySwapChainSupport(scanned_device);
                swap_chain_adequate = !swap_chain_support.formats.empty() && !swap_chain_support.present_modes.empty();
            }

        return queues.indices.isComplete(true) && extensions_supported && swap_chain_adequate;
    }


    //////////////////////////
    // PHYSICAL DEVICE INFO //
    //////////////////////////

void NovaCoreLegacy::createPhysicalDevice() 
    {
        report(LOGGER::VLINE, "\t .. Scanning for Physical Devices ..");

        uint32_t device_count = 0;
        VK_TRY(vkEnumeratePhysicalDevices(instance, &device_count, nullptr));

        if (device_count == 0) 
            { 
                report(LOGGER::ERROR, "Vulkan: No GPUs with Vulkan support found"); 
                return;    
            }

        std::vector<VkPhysicalDevice> devices(device_count);
        VK_TRY(vkEnumeratePhysicalDevices(instance, &device_count, devices.data()));

        for (const auto& device : devices) 
            {
                VkPhysicalDeviceProperties device_properties;
                vkGetPhysicalDeviceProperties(device, &device_properties);
                report(LOGGER::VLINE, "\tScanning Device: %p - %s", device, device_properties.deviceName);
                if (device == VK_NULL_HANDLE) 
                    { continue; }

                if (deviceProvisioned(device))
                    { 
                        report(LOGGER::VLINE, "\tUsing Device: %s", device_properties.deviceName);

  
                        physical_device = device;
                        break; 
                    }
            }


        if (physical_device == VK_NULL_HANDLE) 
            { report(LOGGER::ERROR, "Vulkan: Failed to find a suitable GPU"); }

        return;
    }


    /////////////////////////
    // LOGICAL DEVICE INFO //
    /////////////////////////

VkDeviceQueueCreateInfo NovaCoreLegacy::getQueueCreateInfo(uint32_t queue_family)
    {
        return {
                sType: VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO,
                queueFamilyIndex: queue_family,
                queueCount: queues.families[queue_family].queueCount,
                pQueuePriorities: queues.priorities[queue_family].data()
            };
    }

void NovaCoreLegacy::createLogicalDevice()
    {
        report(LOGGER::VLINE, "\t .. Creating Logical Device ..");
        VkPhysicalDeviceFeatures _device_features = {};
        _device_features.shaderFloat64 = VK_TRUE;  // Enable FP64 for shaders that request it

        // Enable Vulkan 1.2 features (timeline semaphore required for vkWaitSemaphores)
        VkPhysicalDeviceVulkan12Features vulkan12Features{};
        vulkan12Features.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES;
        vulkan12Features.timelineSemaphore = VK_TRUE;
        vulkan12Features.pNext = nullptr;

        std::vector<VkDeviceQueueCreateInfo> _queue_create_infos;
        const std::set<uint32_t> _unique_queue_families = collectQueueFamilies(queues.indices);

        if (_unique_queue_families.empty()) 
            { throw std::runtime_error("No queue families resolved for the selected device"); }

        report(LOGGER::VLINE, "\t .. Creating Queue Family ..");
        for (uint32_t _queue_family : _unique_queue_families)
            {
                report(LOGGER::VLINE, "\t\tQueue Family: %d", _queue_family);
                report(LOGGER::VLINE, "\t\t\tQueue Count: %d", queues.families[_queue_family].queueCount);

                VkDeviceQueueCreateInfo _queue_info = getQueueCreateInfo(_queue_family);

                _queue_create_infos.push_back(_queue_info);
            }

        VkDeviceCreateInfo create_info = {
                sType: VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO,
                pNext: &vulkan12Features,  // Chain Vulkan 1.2 features
                queueCreateInfoCount: static_cast<uint32_t>(_queue_create_infos.size()),
                pQueueCreateInfos: _queue_create_infos.data(),
                pEnabledFeatures: &_device_features,
            };

        // Only enable swapchain extension in graphics mode
        if (!compute_only) {
            create_info.enabledExtensionCount = static_cast<uint32_t>(DEVICE_EXTENSIONS.size());
            create_info.ppEnabledExtensionNames = DEVICE_EXTENSIONS.data();
        } else {
            create_info.enabledExtensionCount = 0;
            create_info.ppEnabledExtensionNames = nullptr;
        }

        VK_TRY(vkCreateDevice(physical_device, &create_info, nullptr, &logical_device));

        if (queues.indices.graphics_family.has_value()) 
            { vkGetDeviceQueue(logical_device, queues.indices.graphics_family.value(), 0, &queues.graphics); }

        if (queues.indices.present_family.has_value()) 
            { vkGetDeviceQueue(logical_device, queues.indices.present_family.value(), 0, &queues.present); }

        vkGetDeviceQueue(logical_device, queues.indices.compute_family.value(), 0, &queues.compute);
        vkGetDeviceQueue(logical_device, queues.indices.transfer_family.value(), 0, &queues.transfer);

        // Register logical device for cleanup (must be destroyed AFTER all other resources)
        resource_registry.register_resource("logical_device", [this]() {
            if (logical_device != VK_NULL_HANDLE) {
                report(LOGGER::INFO, "Destroying logical device");
                vkDestroyDevice(logical_device, nullptr);
                logical_device = VK_NULL_HANDLE;
            }
        });

        // Initialize VMA allocator after logical device creation
        VmaAllocatorCreateInfo allocatorInfo = {};
        allocatorInfo.vulkanApiVersion = VK_API_VERSION_1_2;
        allocatorInfo.physicalDevice = physical_device;
        allocatorInfo.device = logical_device;
        allocatorInfo.instance = instance;

        VK_TRY(vmaCreateAllocator(&allocatorInfo, &allocator));

        // Register VMA allocator for cleanup (must be destroyed BEFORE logical device)
        resource_registry.register_resource("vma_allocator", [this]() {
            if (allocator != VK_NULL_HANDLE) {
                report(LOGGER::INFO, "Destroying VMA allocator");
                vmaDestroyAllocator(allocator);
                allocator = VK_NULL_HANDLE;
            }
        });

        // Create immediate context for immediateSubmit() operations
        createImmediateContext();

        //log();
    }