#pragma once
#include "./modules/pipeline/pipeline.h"
#include "./modules/camera/camera.h"
#include "./components/lexicon.h"
#include <span>


class NovaCore {
    public:
        VkInstance instance;
        VkPhysicalDevice physical_device;
        VkDevice logical_device;
        Queues queues;
        SwapChainContext swapchain;
        VkSurfaceKHR surface;
        Camera player_camera;

        bool framebuffer_resized = false;

        NovaCore(VkExtent2D, std::string&);
        ~NovaCore();

        void log();

        void setWindowExtent(VkExtent2D);

        SwapChainSupportDetails querySwapChainSupport(VkPhysicalDevice);
        void querySwapChainDetails();
        void createPhysicalDevice();
        void createLogicalDevice(); 
        void constructSwapChain();
        void constructImageViews();
        void createFrameBuffers();
        void createRenderPass();
        void createDescriptorSetLayout();
        void createCommandPool();
        void createTextureImage();
        void constructVertexBuffer();
        void constructIndexBuffer();
        void constructUniformBuffer();
        void constructDescriptorPool();
        void createDescriptorSets();
        void createCommandBuffers();
        void createSyncObjects();
        void constructGraphicsPipeline();
        void constructComputePipeline();
        MeshBuffer createMeshBuffer(std::span<uint32_t>, std::span<Vertex_T>);
        void drawFrame();

    private:
        FrameData frames[MAX_FRAMES_IN_FLIGHT];
        VkRenderPass render_pass;
        QueuePresentContext present;
        DescriptorContext descriptor;
        Pipeline *graphics_pipeline;
        Pipeline *compute_pipeline;
        BufferContext vertex;
        BufferContext index;
        std::vector<BufferContext> uniform;
        std::vector<void*> uniform_data;
        const VkClearValue CLEAR_COLOR = {{{0.0f, 0.0f, 0.0f, 1.0f}}};
        FrameData& current_frame();
        int _frame_ct = 0;
        VmaAllocator allocator;

        void logQueues();
        void logSwapChain();
        void logFrameData();

        void _blankContext();
        void createVulkanInstance();

        bool checkValidationLayerSupport();
        void getQueueFamilies(VkPhysicalDevice);
        VkDeviceQueueCreateInfo getQueueCreateInfo(uint32_t);
        void setQueueFamilyProperties(unsigned int);
        void immediateSubmit(std::function<void(VkCommandBuffer)>&&);

        VkMemoryAllocateInfo getMemoryAllocateInfo(VkMemoryRequirements, VkMemoryPropertyFlags);

        void createSwapchainInfoKHR(VkSwapchainCreateInfoKHR*, uint32_t);
        VkImageViewCreateInfo createImageViewInfo(size_t);
        void transitionImage();
        void recreateSwapChain();

        VkRenderPassBeginInfo getRenderPassInfo(size_t);
        VkAttachmentDescription colorAttachment();

        void constructPipeline(Pipeline*);

        VkCommandBufferBeginInfo createBeginInfo();
        VkCommandBufferAllocateInfo createCommandBuffersInfo(VkCommandPool&, char*);
        VkCommandBuffer createEphemeralCommand(VkCommandPool&);
        void flushCommandBuffer(VkCommandBuffer&, char*);

        VkBufferCreateInfo getBufferInfo(VkDeviceSize, VkBufferUsageFlags);
        void createBuffer(VkDeviceSize, VkBufferUsageFlags, VkMemoryPropertyFlags, BufferContext*);
        void copyBuffer(VkBuffer, VkBuffer, VkDeviceSize);
        void recordCommandBuffers(VkCommandBuffer&, uint32_t); 
        void resetCommandBuffers();
        void updateUniformBuffer(uint32_t);
        Buffer_T createEphemeralBuffer(size_t, VkBufferUsageFlags, VmaMemoryUsage);

        void transitionImageLayout(VkImage, VkFormat, VkImageLayout, VkImageLayout);
        void copyBufferToImage(VkBuffer&, VkImage&, uint32_t, uint32_t);

        bool deviceProvisioned(VkPhysicalDevice);
        void destroySwapChain();
        void destroyBuffer(BufferContext*);
        void destroyCommandContext();
        void destroyVertexContext();
        void destroyIndexContext();
        void destroyUniformContext();
        void destroyPipeline(Pipeline*);
};

