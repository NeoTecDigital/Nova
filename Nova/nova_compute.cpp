#include "./nova_compute.h"
namespace Nova {
Compute::Compute(const std::string& debug_level)
    : Core(debug_level)
{
    report(LOGGER::INFO, "NovaCompute - Initializing compute-only mode ..");

    // Initialize base resources (no surface)
    createVulkanInstance({});     // No window system: no surface extensions
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

Compute::~Compute()
{
    report(LOGGER::INFO, "NovaCompute - Destroying");
    vkDeviceWaitIdle(logical_device);

    // compute_fence is a Compute member; resource_registry belongs to the
    // base. Draining the entry from ~Core would run it after this object's
    // members are gone. VkFence is a trivially destructible handle so the bytes
    // happened to survive, but the pattern is the teardown UAF that ~Graphics
    // already had to fix - so run and unregister it HERE, while the member is
    // still alive. Idempotent: run_and_release is a no-op if the key is absent.
    resource_registry.run_and_release("compute_resources");
}

void Compute::submitCompute(std::function<void(VkCommandBuffer)>&& func)
{
    // Acquire mutex and HOLD it until waitCompute() releases.
    // This prevents another thread from resetting the command buffer
    // while GPU work is in flight.
    submit_lock_ = std::unique_lock<std::mutex>(compute_mutex_);

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

    // Submit to compute queue (does NOT wait)
    VkSubmitInfo submit_info = {
        .sType = VK_STRUCTURE_TYPE_SUBMIT_INFO,
        .commandBufferCount = 1,
        .pCommandBuffers = &compute_cmd
    };

    VK_TRY(vkQueueSubmit(queues.compute, 1, &submit_info, compute_fence));
    // Lock remains held — released by waitCompute()
}

bool Compute::isComputeComplete() const
{
    // Fence query is atomic in Vulkan — safe without mutex.
    // The caller must already own the submit_lock_ (they called submitCompute).
    return vkGetFenceStatus(logical_device, compute_fence) == VK_SUCCESS;
}

void Compute::waitCompute()
{
    // Wait for GPU work to finish, then release the lock so other
    // threads can submit.
    VK_TRY(vkWaitForFences(logical_device, 1, &compute_fence, VK_TRUE, UINT64_MAX));
    if (submit_lock_.owns_lock()) {
        submit_lock_.unlock();
    }
}

void Compute::executeCompute(std::function<void(VkCommandBuffer)>&& func)
{
    // Synchronous: acquire lock, record+submit, wait, release lock.
    // Uses a local lock — does NOT go through submitCompute/waitCompute
    // to avoid the member unique_lock dance.
    std::lock_guard<std::mutex> lock(compute_mutex_);

    VK_TRY(vkResetFences(logical_device, 1, &compute_fence));
    VK_TRY(vkResetCommandBuffer(compute_cmd, 0));

    VkCommandBufferBeginInfo begin_info = {
        .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
        .flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT
    };

    VK_TRY(vkBeginCommandBuffer(compute_cmd, &begin_info));
    func(compute_cmd);
    VK_TRY(vkEndCommandBuffer(compute_cmd));

    VkSubmitInfo submit_info = {
        .sType = VK_STRUCTURE_TYPE_SUBMIT_INFO,
        .commandBufferCount = 1,
        .pCommandBuffers = &compute_cmd
    };

    VK_TRY(vkQueueSubmit(queues.compute, 1, &submit_info, compute_fence));
    VK_TRY(vkWaitForFences(logical_device, 1, &compute_fence, VK_TRUE, UINT64_MAX));
}

void Compute::waitIdle()
{
    std::lock_guard<std::mutex> lock(compute_mutex_);
    vkDeviceWaitIdle(logical_device);
}

} // namespace Nova
