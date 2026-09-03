#pragma once

#include <vulkan/vulkan.h>
#include <glm/glm.hpp>
#include "Core/math/hyper_math.h"
#include <array>

namespace Nova {

/**
 * SpatialVertex - Aligned with dual [a,b,c,w] Hyper4 primordial structures
 */
struct SpatialVertex {
    Nova::Math::Hyper4 state_primary{0.0f, 0.0f, 0.0f, 1.0f}; // [a, b, c, w]_0 : Spatial / (x, y, z, w) / (re0, im0)
    Nova::Math::Hyper4 state_dual{1.0f, 1.0f, 1.0f, 1.0f};    // [a, b, c, w]_1 : Chromatic / (r, g, b, a) / Phase Field
    glm::vec3 normal{0.0f, 0.0f, 1.0f};
    glm::vec2 uv{0.0f};
    glm::vec4 params{0.0f}; // [border_thickness, corner_radius, render_mode, phase_factor]

    static VkVertexInputBindingDescription getBindingDescription() {
        VkVertexInputBindingDescription binding = {};
        binding.binding = 0;
        binding.stride = sizeof(SpatialVertex);
        binding.inputRate = VK_VERTEX_INPUT_RATE_VERTEX;
        return binding;
    }

    static std::array<VkVertexInputAttributeDescription, 5> getAttributeDescriptions() {
        std::array<VkVertexInputAttributeDescription, 5> attrs = {};

        // location 0: vec4 state_primary [a, b, c, w]_0
        attrs[0].binding = 0;
        attrs[0].location = 0;
        attrs[0].format = VK_FORMAT_R32G32B32A32_SFLOAT;
        attrs[0].offset = offsetof(SpatialVertex, state_primary);

        // location 1: vec4 state_dual [a, b, c, w]_1
        attrs[1].binding = 0;
        attrs[1].location = 1;
        attrs[1].format = VK_FORMAT_R32G32B32A32_SFLOAT;
        attrs[1].offset = offsetof(SpatialVertex, state_dual);

        // location 2: vec3 normal
        attrs[2].binding = 0;
        attrs[2].location = 2;
        attrs[2].format = VK_FORMAT_R32G32B32_SFLOAT;
        attrs[2].offset = offsetof(SpatialVertex, normal);

        // location 3: vec2 uv
        attrs[3].binding = 0;
        attrs[3].location = 3;
        attrs[3].format = VK_FORMAT_R32G32_SFLOAT;
        attrs[3].offset = offsetof(SpatialVertex, uv);

        // location 4: vec4 params
        attrs[4].binding = 0;
        attrs[4].location = 4;
        attrs[4].format = VK_FORMAT_R32G32B32A32_SFLOAT;
        attrs[4].offset = offsetof(SpatialVertex, params);

        return attrs;
    }
};

struct SpatialPushConstants {
    glm::mat4 view_proj;
    glm::mat4 model;
    glm::vec4 camera_pos;
    glm::vec4 surface_dim; // [width, height, aspect, time]
};

} // namespace Nova
