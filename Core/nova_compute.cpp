#include "./nova_compute.h"

NovaCompute::NovaCompute(const std::string& debug_level)
    : NovaCore(debug_level)
{
    report(LOGGER::INFO, "NovaCompute - Initializing compute-only mode ..");

    // Initialize base resources (no surface)
    createVulkanInstance(false);  // No surface extensions
    createPhysicalDevice(false);  // No presentation support needed
    createLogicalDevice(false);   // No swapchain extension
    createSharedCommandPools();
    createImmediateContext();

    // Allocate compute command buffer
    VkCommandBufferAllocateInfo cmd_info = {
        .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,
        .commandPool = compute_pool,
        .level = VK_COMMAND_BUFFER_LEVEL_PRIMARY,
        .commandBufferCount = 1
    };
    VK_TRY(vkAllocateCommandBuffers(logical_device, &cmd_info, &compute_cmd));

    // Create compute fence
    VkFenceCreateInfo fence_info = {
        .sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO,
        .flags = VK_FENCE_CREATE_SIGNALED_BIT
    };
    VK_TRY(vkCreateFence(logical_device, &fence_info, nullptr, &compute_fence));

    // Register for cleanup
    resource_registry.register_resource("compute_resources", [this]() {
        if (compute_fence != VK_NULL_HANDLE) {
            vkDestroyFence(logical_device, compute_fence, nullptr);
            compute_fence = VK_NULL_HANDLE;
        }
    });

    report(LOGGER::INFO, "NovaCompute - Initialized successfully");
}

NovaCompute::~NovaCompute()
{
    report(LOGGER::INFO, "NovaCompute - Destroying");
    vkDeviceWaitIdle(logical_device);
}

void NovaCompute::executeCompute(std::function<void(VkCommandBuffer)>&& func)
{
    // Reset fence and command buffer
    VK_TRY(vkResetFences(logical_device, 1, &compute_fence));
    VK_TRY(vkResetCommandBuffer(compute_cmd, 0));

    // Begin command buffer
    VkCommandBufferBeginInfo begin_info = {
        .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
        .flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT
    };

    VK_TRY(vkBeginCommandBuffer(compute_cmd, &begin_info));

    // Execute user-provided commands
    func(compute_cmd);

    VK_TRY(vkEndCommandBuffer(compute_cmd));

    // Submit to compute queue
    VkSubmitInfo submit_info = {
        .sType = VK_STRUCTURE_TYPE_SUBMIT_INFO,
        .commandBufferCount = 1,
        .pCommandBuffers = &compute_cmd
    };

    VK_TRY(vkQueueSubmit(queues.compute, 1, &submit_info, compute_fence));

    // Wait for completion
    VK_TRY(vkWaitForFences(logical_device, 1, &compute_fence, VK_TRUE, UINT64_MAX));
}

void NovaCompute::waitIdle()
{
    vkDeviceWaitIdle(logical_device);
}
