#pragma once
#include "./core_base.h"

/**
 * NovaCompute - Compute-only mode
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
class NovaCompute : public NovaCore {
private:
    // Compute-specific resources
    VkCommandBuffer compute_cmd = VK_NULL_HANDLE;
    VkFence compute_fence = VK_NULL_HANDLE;

public:
    /**
     * Constructor - Initialize compute-only mode
     * @param debug_level Logging level (INFO, DEBUG, VERBOSE, etc.)
     */
    NovaCompute(const std::string& debug_level);

    /**
     * Destructor - Cleanup compute resources
     */
    ~NovaCompute() override;

    /**
     * Execute compute commands and wait for completion
     * @param func Lambda receiving VkCommandBuffer for recording commands
     */
    void executeCompute(std::function<void(VkCommandBuffer)>&& func);

    /**
     * Wait for all device operations to complete
     */
    void waitIdle();

    /**
     * Get the compute command buffer (for manual recording)
     */
    VkCommandBuffer getComputeCommandBuffer() const { return compute_cmd; }
};
