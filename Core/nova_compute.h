#pragma once
#include "./core_base.h"
#include <mutex>
namespace Nova {
/**
 * Compute - Compute-only mode
 *
 * Lightweight Vulkan context for GPU compute operations:
 * - No surface/swapchain
 * - No graphics pipeline
 * - Minimal overhead
 *
 * Use for:
 * - FFT computations (Logos encoding)
 * - Triplanar projections
 * - General GPU compute tasks
 */
class Compute : public Core {
private:
    // Serializes all command buffer access — held from submit through wait.
    // submitCompute() acquires, waitCompute() releases.
    // executeCompute() holds for full duration.
    std::mutex compute_mutex_;
    std::unique_lock<std::mutex> submit_lock_;  // Held between submit and wait

    // Compute-specific resources
    VkCommandBuffer compute_cmd = VK_NULL_HANDLE;
    VkFence compute_fence = VK_NULL_HANDLE;

public:
    /**
     * Constructor - Initialize compute-only mode
     * @param debug_level Logging level (INFO, DEBUG, VERBOSE, etc.)
     */
    Compute(const std::string& debug_level);

    /**
     * Destructor - Cleanup compute resources
     */
    ~Compute() override;

    /**
     * Execute compute commands and wait for completion
     * @param func Lambda receiving VkCommandBuffer for recording commands
     */
    void executeCompute(std::function<void(VkCommandBuffer)>&& func);

    /**
     * Submit compute commands without waiting (async)
     * @param func Lambda receiving VkCommandBuffer for recording commands
     */
    void submitCompute(std::function<void(VkCommandBuffer)>&& func);

    /**
     * Non-blocking check if submitted compute work is complete
     */
    bool isComputeComplete() const;

    /**
     * Block until submitted compute work completes
     */
    void waitCompute();

    /**
     * Wait for all device operations to complete
     */
    void waitIdle();

    /**
     * Get the compute command buffer (for manual recording)
     */
    VkCommandBuffer getComputeCommandBuffer() const { return compute_cmd; }
};

} // namespace Nova
