#pragma once

#include "Core/core_base.h"
#include "./spatial_vertex.h"
#include "./texture_bridge.h"
#include <vulkan/vulkan.h>
#include <string>
#include <vector>
#include <memory>

namespace Nova {

class SpatialPipeline {
public:
    SpatialPipeline(Core* core, VkRenderPass render_pass, TextureBridge* texture_bridge);
    ~SpatialPipeline();

    // Construct the Vulkan graphics pipeline
    void build(const std::string& vert_spv_path = "shaders/spatial/spatial_ui_vert.spv",
               const std::string& frag_spv_path = "shaders/spatial/spatial_ui_frag.spv");

    // Bind pipeline and push spatial constants
    void bind(VkCommandBuffer cmd);
    void pushConstants(VkCommandBuffer cmd, const SpatialPushConstants& constants);
    void bindTexture(VkCommandBuffer cmd, VkDescriptorSet descriptor_set);

    VkPipeline getPipeline() const { return pipeline_; }
    VkPipelineLayout getLayout() const { return pipeline_layout_; }

private:
    Core* core_ = nullptr;
    VkDevice device_ = VK_NULL_HANDLE;
    VkRenderPass render_pass_ = VK_NULL_HANDLE;
    TextureBridge* texture_bridge_ = nullptr;

    VkPipelineLayout pipeline_layout_ = VK_NULL_HANDLE;
    VkPipeline pipeline_ = VK_NULL_HANDLE;

    VkShaderModule createShaderModule(const std::vector<char>& code);
    std::vector<char> readFile(const std::string& filename);
};

} // namespace Nova
