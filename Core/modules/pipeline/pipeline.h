#pragma once
#include "Core/modules/atomic/atomic.h"

#include <vector>
namespace Nova {
/*
    The Builder class is a builder that allows for the creation of a graphics pipeline
    and is responsible for the creation of the pipeline layout, shaders, and other pipeline related
    configurations.
*/

// TODO: Assert Singleton
class Builder {
    public:
	    Builder();
        ~Builder();

        VkPipeline instance;
        VkPipelineLayout layout;
        std::vector<Vertex> vertices = {};
        std::vector<uint32_t> indices = {};
        VkImage texture_image;
        VkDeviceMemory texture_image_memory;

        Builder& shaders(VkDevice*, const std::string& vert_path = "", const std::string& frag_path = "");
        Builder& vertexInput();
        Builder& inputAssembly();
        Builder& viewportState();
        Builder& rasterizer();
        Builder& multisampling();
        Builder& depthStencil();
        Builder& colorBlending();
        Builder& dynamicState();
        Builder& createLayout(VkDevice*, VkDescriptorSetLayout*);
        Builder& pipe(VkRenderPass*);
        Builder& create(VkDevice*);
        void clear();


    private:
        VkGraphicsPipelineCreateInfo _pipeline_info;
        VkPipelineLayoutCreateInfo _pipeline_layout_info;
        VkPipelineViewportStateCreateInfo _viewport_state;
        VkPipelineVertexInputStateCreateInfo _vertex_input_state;
        VkPipelineInputAssemblyStateCreateInfo _input_assembly;
        VkPipelineDynamicStateCreateInfo _dynamic_state;
        std::vector<VkShaderModule> _shader_modules;
        std::vector<VkPipelineShaderStageCreateInfo> _shader_stages;
        VkRenderingInfo _render_info;
        VkPipelineRasterizationStateCreateInfo _rasterizer;
        VkPipelineMultisampleStateCreateInfo _multisampling;
        VkPipelineDepthStencilStateCreateInfo _depth_stencil;
        VkPipelineColorBlendAttachmentState _color_blend_attachment;
        VkPipelineColorBlendStateCreateInfo _color_blending;
        std::vector<VkDynamicState> _dynamic_states = { VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR };
        VkVertexInputBindingDescription _binding_description;
        std::array<VkVertexInputAttributeDescription, 2UL> _attribute_descriptions;
       
        void addShaderStage(VkShaderModule, VkShaderStageFlagBits);
};

} // namespace Nova
