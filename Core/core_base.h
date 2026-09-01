#pragma once
#include "./modules/atomic/atomic.h"
#include "./core_device_select.h"
#include "./components/resource_registry.h"
#include "./components/vk_memory.h"
#include "./components/lexicon.h"
#include <functional>

/**
 * NovaCore - Base class for all Nova modes (Compute and Graphics)
 *
 * Provides shared Vulkan resources:
 * - Instance, physical device, logical device
 * - VMA allocator
 * - Queue infrastructure (compute, transfer)
 * - Immediate submission context
 * - Resource registry for cleanup
 *
 * Derived classes:
 * - NovaCompute: Compute-only mode (no surface/swapchain)
 * - NovaGraphics: Traditional rendering pipeline
 */
class NovaCore {
protected:
    // Core Vulkan resources (shared by all modes)
    VkInstance instance = VK_NULL_HANDLE;
    VkPhysicalDevice physical_device = VK_NULL_HANDLE;
    VkDevice logical_device = VK_NULL_HANDLE;
    VmaAllocator allocator = VK_NULL_HANDLE;

    // Queue infrastructure
    struct {
        VkQueue compute = VK_NULL_HANDLE;
        VkQueue transfer = VK_NULL_HANDLE;
        // Engaged only when the selected device exposes a graphics family; the
        // same handle NovaGraphics submits its frames on (family, index 0).
        VkQueue graphics = VK_NULL_HANDLE;
        QueueFamilyIndices indices;
        std::vector<VkQueueFamilyProperties> families;
        std::vector<std::vector<float>> priorities;
        DeletionQueue deletion;  // Legacy deletion queue
    } queues;

    /**
     * One-shot command recording bound to a single queue family.
     *
     * The family is what makes a context usable, not an implementation detail:
     * vkCmdPipelineBarrier rejects any pipeline stage the recording family cannot
     * execute, and an EXCLUSIVE resource written on one family then read on
     * another needs an explicit ownership transfer. Callers therefore pick the
     * context whose family matches the work instead of sharing a single one.
     */
    struct ImmediateContext {
        VkFence fence = VK_NULL_HANDLE;
        VkCommandBuffer cmd = VK_NULL_HANDLE;
        VkCommandPool pool = VK_NULL_HANDLE;
        VkQueue queue = VK_NULL_HANDLE;

        bool engaged() const { return cmd != VK_NULL_HANDLE && queue != VK_NULL_HANDLE; }
    };

    // Transfer-family context: staging copies that name no graphics stage.
    ImmediateContext immediate;

    // Graphics-family context: uploads whose barriers name graphics stages, or
    // that touch resources the render submissions also read. Left disengaged on
    // devices that expose no graphics family, which compute-only mode accepts.
    ImmediateContext graphics_immediate;

    // Command pools (shared)
    VkCommandPool compute_pool = VK_NULL_HANDLE;
    VkCommandPool transfer_pool = VK_NULL_HANDLE;

    // Resource cleanup registry
    NovaRAII::ResourceRegistry resource_registry;

    // Window extent (may be unused in compute mode)
    VkExtent2D window_extent;

    // Optional device extensions the winning device actually reported, resolved
    // once at selection time and enabled verbatim on the logical device.
    std::vector<std::string> enabled_device_extensions;

    // Protected constructor (only derived classes can instantiate)
    NovaCore(const std::string& debug_level);

    // Virtual destructor (required for polymorphism)
    virtual ~NovaCore();

    // Shared initialization methods
    void createVulkanInstance(bool need_surface_extensions);
    void createPhysicalDevice(bool need_presentation, VkSurfaceKHR surface = VK_NULL_HANDLE);

    /**
     * Score every candidate that clears the request's gates and keep the best.
     *
     * DISCRETE > VIRTUAL > INTEGRATED > CPU, tie-broken by a DRM-node match
     * against request.drm_fd. Every candidate is reported at INFO.
     */
    void createPhysicalDevice(const NovaDeviceRequest& request);
    void createLogicalDevice(bool need_swapchain_extension);
    void createImmediateContext();
    void createSharedCommandPools();
    void createDeviceHandle(const std::vector<VkDeviceQueueCreateInfo>& queue_infos,
                            bool need_swapchain_extension);
    void acquireQueueHandles();
    void createMemoryAllocator();

    // Allocate/release the pool, command buffer and fence of one context.
    void buildImmediateContext(ImmediateContext& context, uint32_t queue_family, VkQueue queue);
    void destroyImmediateContext(ImmediateContext& context);
    void submitImmediate(ImmediateContext& context, const std::function<void(VkCommandBuffer)>& func);

    // Helper methods
    bool checkValidationLayerSupport();
    void getQueueFamilies(VkPhysicalDevice device, VkSurfaceKHR surface, bool need_presentation);
    bool deviceProvisioned(VkPhysicalDevice device, VkSurfaceKHR surface, bool need_swapchain);
    bool deviceProvisioned(VkPhysicalDevice device, const NovaDeviceRequest& request);

    // Re-scan the winner, resolve its optional extensions, emit the D.3 report.
    void finalizeDeviceSelection(const NovaDeviceRequest& request);
    std::vector<std::string> resolveOptionalExtensions(VkPhysicalDevice device,
                                                       const std::vector<const char*>& wanted);
    /**
     * Record what family `index` can do on the device currently being scanned.
     *
     * Takes no surface and no presentation flag: presentation is a property of
     * a (family, surface) pair that getQueueFamilies() resolves itself, and
     * carrying them here only made two parameters that were never read.
     */
    void setQueueFamilyProperties(uint32_t index);

    void _blankContext();
    void setWindowExtent(VkExtent2D extent);

public:
    // Public API (shared by all modes)

    /**
     * Record and submit on the TRANSFER family, then wait for it to retire.
     *
     * Valid for staging copies and layout transitions expressed purely in
     * transfer stages. A transfer family carries no graphics or compute
     * capability, so barriers naming those stages belong on another context.
     */
    void immediateSubmit(std::function<void(VkCommandBuffer)>&& func);

    /**
     * Record and submit on the GRAPHICS family, then wait for it to retire.
     *
     * Use for work whose barriers name graphics pipeline stages, or that writes
     * an EXCLUSIVE resource the frame submissions later read: sharing the render
     * queue's family removes the ownership transfer and makes submission order
     * against the frames a real ordering guarantee.
     *
     * Falls back to the transfer context on devices with no graphics family;
     * hasGraphicsImmediate() reports which one a caller will get.
     */
    void immediateSubmitGraphics(std::function<void(VkCommandBuffer)>&& func);

    // True when immediateSubmitGraphics() runs on a graphics-capable family.
    bool hasGraphicsImmediate() const { return graphics_immediate.engaged(); }

    /**
     * Create buffer using VMA (ephemeral or persistent)
     */
    Buffer_T createEphemeralBuffer(size_t size, VkBufferUsageFlags flags, VmaMemoryUsage usage);

    // Resource access getters
    VmaAllocator getAllocator() const { return allocator; }
    VkQueue getComputeQueue() const { return queues.compute; }
    VkQueue getTransferQueue() const { return queues.transfer; }
    VkDevice getDevice() const { return logical_device; }
    VkInstance getInstance() const { return instance; }
    VkPhysicalDevice getPhysicalDevice() const { return physical_device; }

    // Family every render submission and every graphics immediate uses. What a
    // caller needs to name the other side of a queue-family ownership transfer.
    // UINT32_MAX on a device that exposes no graphics family.
    uint32_t getGraphicsFamilyIndex() const {
        return queues.indices.graphics_family.value_or(UINT32_MAX);
    }

    // True when `name` was resolved and enabled on the logical device.
    bool hasDeviceExtension(const char* name) const;
};
