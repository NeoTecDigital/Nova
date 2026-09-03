// Written by Richard Christopher, Copyright 2026 NeoTec Digital
#include "./spatial_mesh.h"
#include "Core/nova_graphics.h"
#include "Core/components/logger.h"
#include <algorithm>
#include <cmath>
#include <cstring>
#include <stdexcept>

namespace Nova {

MeshData SpatialMeshGenerator::createPlanarQuad(const glm::vec2& size,
                                                const glm::vec4& color,
                                                float border_thickness,
                                                float corner_radius,
                                                float render_mode) {
    MeshData mesh;
    float half_w = size.x * 0.5f;
    float half_h = size.y * 0.5f;

    glm::vec4 params(border_thickness, corner_radius, render_mode, 0.0f);
    glm::vec3 normal(0.0f, 0.0f, 1.0f);

    // Top-left, Top-right, Bottom-right, Bottom-left
    mesh.vertices = {
        { Nova::Math::Hyper4(-half_w,  half_h, 0.0f, 1.0f), Nova::Math::Hyper4(color.r, color.g, color.b, color.a), normal, glm::vec2(0.0f, 0.0f), params },
        { Nova::Math::Hyper4( half_w,  half_h, 0.0f, 1.0f), Nova::Math::Hyper4(color.r, color.g, color.b, color.a), normal, glm::vec2(1.0f, 0.0f), params },
        { Nova::Math::Hyper4( half_w, -half_h, 0.0f, 1.0f), Nova::Math::Hyper4(color.r, color.g, color.b, color.a), normal, glm::vec2(1.0f, 1.0f), params },
        { Nova::Math::Hyper4(-half_w, -half_h, 0.0f, 1.0f), Nova::Math::Hyper4(color.r, color.g, color.b, color.a), normal, glm::vec2(0.0f, 1.0f), params },
    };

    mesh.indices = {
        0, 1, 2,
        2, 3, 0
    };

    return mesh;
}

namespace {

// Two triangles per segment across a top/bottom vertex pair strip.
void appendQuadStripIndices(MeshData& mesh, uint32_t segments) {
    for (uint32_t i = 0; i < segments; ++i) {
        const uint32_t tl = i * 2;
        const uint32_t bl = tl + 1;
        const uint32_t tr = tl + 2;
        const uint32_t br = tl + 3;

        mesh.indices.push_back(tl);
        mesh.indices.push_back(tr);
        mesh.indices.push_back(br);

        mesh.indices.push_back(br);
        mesh.indices.push_back(bl);
        mesh.indices.push_back(tl);
    }
}

} // namespace

MeshData SpatialMeshGenerator::createCurvedArc(const glm::vec2& size,
                                               float radius,
                                               uint32_t segments,
                                               const glm::vec4& color,
                                               float border_thickness,
                                               float corner_radius,
                                               float render_mode) {
    // A zero (or sub-epsilon) radius sweeps no arc; size.x / radius would be
    // inf or NaN and poison every vertex. An infinite-radius arc IS a flat
    // quad, so degenerate to one rather than emitting garbage geometry.
    if (!(std::fabs(radius) > SpatialMeshGenerator::MIN_ARC_RADIUS)) {
        return createPlanarQuad(size, color, border_thickness, corner_radius, render_mode);
    }

    MeshData mesh;
    if (segments < 2) segments = 2;

    float half_h = size.y * 0.5f;
    float arc_angle = size.x / radius; // Angle spanned by width
    float start_angle = -arc_angle * 0.5f;
    float step_angle = arc_angle / static_cast<float>(segments);

    glm::vec4 params(border_thickness, corner_radius, render_mode, 0.0f);

    for (uint32_t i = 0; i <= segments; ++i) {
        float theta = start_angle + static_cast<float>(i) * step_angle;
        float u = static_cast<float>(i) / static_cast<float>(segments);

        float x = radius * std::sin(theta);
        float z = radius * (1.0f - std::cos(theta)); // Curve inwards/backwards
        glm::vec3 normal(-std::sin(theta), 0.0f, std::cos(theta));

        // Top vertex (v=0)
        mesh.vertices.push_back({ Nova::Math::Hyper4(x,  half_h, z, 1.0f), Nova::Math::Hyper4(color.r, color.g, color.b, color.a), normal, glm::vec2(u, 0.0f), params });
        // Bottom vertex (v=1)
        mesh.vertices.push_back({ Nova::Math::Hyper4(x, -half_h, z, 1.0f), Nova::Math::Hyper4(color.r, color.g, color.b, color.a), normal, glm::vec2(u, 1.0f), params });
    }

    appendQuadStripIndices(mesh, segments);
    return mesh;
}

MeshData SpatialMeshGenerator::createReticle(const glm::vec4& circle_color,
                                             const glm::vec4& crosshair_color,
                                             float radius,
                                             float ring_thickness,
                                             float crosshair_length,
                                             float crosshair_thickness) {
    MeshData mesh;
    uint32_t segments = 36;
    float r_in = radius - ring_thickness * 0.5f;
    float r_out = radius + ring_thickness * 0.5f;
    glm::vec3 normal(0.0f, 0.0f, 1.0f);
    glm::vec4 circle_params(0.0f, 0.0f, 0.0f, 0.0f);

    // 1. Annular Circle Ring
    uint32_t ring_start_idx = static_cast<uint32_t>(mesh.vertices.size());
    for (uint32_t i = 0; i <= segments; ++i) {
        float theta = static_cast<float>(i) / static_cast<float>(segments) * glm::two_pi<float>();
        float cos_t = std::cos(theta);
        float sin_t = std::sin(theta);

        glm::vec3 pos_in(r_in * cos_t, r_in * sin_t, 0.0f);
        glm::vec3 pos_out(r_out * cos_t, r_out * sin_t, 0.0f);

        mesh.vertices.push_back({
            Nova::Math::Hyper4(pos_in.x, pos_in.y, pos_in.z, 1.0f),
            Nova::Math::Hyper4(circle_color.r, circle_color.g, circle_color.b, circle_color.a),
            normal,
            glm::vec2(0.0f),
            circle_params
        });

        mesh.vertices.push_back({
            Nova::Math::Hyper4(pos_out.x, pos_out.y, pos_out.z, 1.0f),
            Nova::Math::Hyper4(circle_color.r, circle_color.g, circle_color.b, circle_color.a),
            normal,
            glm::vec2(1.0f),
            circle_params
        });
    }

    for (uint32_t i = 0; i < segments; ++i) {
        uint32_t in0 = ring_start_idx + i * 2;
        uint32_t out0 = in0 + 1;
        uint32_t in1 = in0 + 2;
        uint32_t out1 = in0 + 3;

        mesh.indices.push_back(in0);
        mesh.indices.push_back(out0);
        mesh.indices.push_back(out1);

        mesh.indices.push_back(out1);
        mesh.indices.push_back(in1);
        mesh.indices.push_back(in0);
    }

    // 2. Horizontal Crosshair Bar
    float hx = crosshair_length * 0.5f;
    float hy = crosshair_thickness * 0.5f;
    uint32_t h_start = static_cast<uint32_t>(mesh.vertices.size());
    glm::vec4 ch_params(0.0f, 0.0f, 0.0f, 0.0f);

    mesh.vertices.push_back({ Nova::Math::Hyper4(-hx,  hy, 0.001f, 1.0f), Nova::Math::Hyper4(crosshair_color.r, crosshair_color.g, crosshair_color.b, crosshair_color.a), normal, glm::vec2(0.0f), ch_params });
    mesh.vertices.push_back({ Nova::Math::Hyper4( hx,  hy, 0.001f, 1.0f), Nova::Math::Hyper4(crosshair_color.r, crosshair_color.g, crosshair_color.b, crosshair_color.a), normal, glm::vec2(1.0f), ch_params });
    mesh.vertices.push_back({ Nova::Math::Hyper4( hx, -hy, 0.001f, 1.0f), Nova::Math::Hyper4(crosshair_color.r, crosshair_color.g, crosshair_color.b, crosshair_color.a), normal, glm::vec2(1.0f), ch_params });
    mesh.vertices.push_back({ Nova::Math::Hyper4(-hx, -hy, 0.001f, 1.0f), Nova::Math::Hyper4(crosshair_color.r, crosshair_color.g, crosshair_color.b, crosshair_color.a), normal, glm::vec2(0.0f), ch_params });

    mesh.indices.push_back(h_start + 0);
    mesh.indices.push_back(h_start + 1);
    mesh.indices.push_back(h_start + 2);
    mesh.indices.push_back(h_start + 2);
    mesh.indices.push_back(h_start + 3);
    mesh.indices.push_back(h_start + 0);

    // 3. Vertical Crosshair Bar
    float vx = crosshair_thickness * 0.5f;
    float vy = crosshair_length * 0.5f;
    uint32_t v_start = static_cast<uint32_t>(mesh.vertices.size());

    mesh.vertices.push_back({ Nova::Math::Hyper4(-vx,  vy, 0.001f, 1.0f), Nova::Math::Hyper4(crosshair_color.r, crosshair_color.g, crosshair_color.b, crosshair_color.a), normal, glm::vec2(0.0f), ch_params });
    mesh.vertices.push_back({ Nova::Math::Hyper4( vx,  vy, 0.001f, 1.0f), Nova::Math::Hyper4(crosshair_color.r, crosshair_color.g, crosshair_color.b, crosshair_color.a), normal, glm::vec2(1.0f), ch_params });
    mesh.vertices.push_back({ Nova::Math::Hyper4( vx, -vy, 0.001f, 1.0f), Nova::Math::Hyper4(crosshair_color.r, crosshair_color.g, crosshair_color.b, crosshair_color.a), normal, glm::vec2(1.0f), ch_params });
    mesh.vertices.push_back({ Nova::Math::Hyper4(-vx, -vy, 0.001f, 1.0f), Nova::Math::Hyper4(crosshair_color.r, crosshair_color.g, crosshair_color.b, crosshair_color.a), normal, glm::vec2(0.0f), ch_params });

    mesh.indices.push_back(v_start + 0);
    mesh.indices.push_back(v_start + 1);
    mesh.indices.push_back(v_start + 2);
    mesh.indices.push_back(v_start + 2);
    mesh.indices.push_back(v_start + 3);
    mesh.indices.push_back(v_start + 0);

    return mesh;
}

// ---------------------------------------------------------------------------
// SpatialMeshBuffer
// ---------------------------------------------------------------------------
namespace {

// Headroom applied when a growth event is scheduled, so a scene that keeps
// creeping upward does not re-trigger truncation on every added node.
constexpr size_t GROWTH_NUMERATOR = 2;

size_t grownCapacity(size_t demand) {
    return demand * GROWTH_NUMERATOR;
}

} // namespace

SpatialMeshBuffer::SpatialMeshBuffer(Core* core, size_t initial_vertex_count, size_t initial_index_count)
    : core_(core) {
    if (!core_) {
        throw std::runtime_error("SpatialMeshBuffer requires a valid NovaCore");
    }
    allocator_ = core_->getAllocator();
    if (allocator_ == VK_NULL_HANDLE) {
        throw std::runtime_error("SpatialMeshBuffer requires an initialized VMA allocator");
    }

    // Resolved once: the graphics core is the only honest source for the index
    // of the frame slot currently being recorded. A non-graphics core keeps
    // graphics_ null and must drive slots through beginFrame(frame_slot).
    graphics_ = dynamic_cast<Graphics*>(core_);

    target_vertices_ = initial_vertex_count;
    target_indices_ = initial_index_count;

    for (uint32_t i = 0; i < MAX_FRAMES_IN_FLIGHT; ++i) {
        allocateSlot(slots_[i], initial_vertex_count, initial_index_count, "initial");
    }

    vertices_.reserve(initial_vertex_count);
    indices_.reserve(initial_index_count);
}

SpatialMeshBuffer::~SpatialMeshBuffer() {
    // Command buffers recorded against these slots may not have retired yet;
    // freeing the allocations under an executing frame is a use-after-free.
    if (core_ != nullptr && core_->getDevice() != VK_NULL_HANDLE) {
        vkDeviceWaitIdle(core_->getDevice());
    }

    for (uint32_t i = 0; i < MAX_FRAMES_IN_FLIGHT; ++i) {
        destroySlot(slots_[i]);
    }
}

Buffer_T SpatialMeshBuffer::createBuffer(size_t byte_size, VkBufferUsageFlags usage, const char* label) {
    VkBufferCreateInfo buffer_info = {
        .sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
        .size = byte_size,
        .usage = usage,
        .sharingMode = VK_SHARING_MODE_EXCLUSIVE
    };

    VmaAllocationCreateInfo alloc_info = {};
    alloc_info.usage = VMA_MEMORY_USAGE_CPU_TO_GPU;

    Buffer_T buffer = {};
    const VkResult result = vmaCreateBuffer(allocator_, &buffer_info, &alloc_info,
                                            &buffer.buffer, &buffer.allocation, &buffer.info);

    if (result != VK_SUCCESS || buffer.buffer == VK_NULL_HANDLE || buffer.allocation == VK_NULL_HANDLE) {
        report(LOGGER::ERROR,
               "SpatialMeshBuffer - vmaCreateBuffer failed for %s buffer of %zu bytes (VkResult %d)",
               label, byte_size, static_cast<int>(result));
        throw std::runtime_error("SpatialMeshBuffer - GPU buffer allocation failed");
    }

    return buffer;
}

void SpatialMeshBuffer::allocateSlot(FrameSlot& slot, size_t vertex_count, size_t index_count, const char* reason) {
    // Allocate before publishing: if the index buffer throws, the slot is not
    // left half-updated with a stale capacity describing a freed vertex buffer.
    Buffer_T new_vertex = createBuffer(vertex_count * sizeof(SpatialVertex),
                                       VK_BUFFER_USAGE_VERTEX_BUFFER_BIT, reason);
    Buffer_T new_index = {};
    try {
        new_index = createBuffer(index_count * sizeof(uint32_t),
                                 VK_BUFFER_USAGE_INDEX_BUFFER_BIT, reason);
    } catch (...) {
        vmaDestroyBuffer(allocator_, new_vertex.buffer, new_vertex.allocation);
        throw;
    }

    slot.vertex_buffer = new_vertex;
    slot.index_buffer = new_index;
    slot.max_vertices = vertex_count;
    slot.max_indices = index_count;
}

void SpatialMeshBuffer::destroySlot(FrameSlot& slot) {
    if (allocator_ == VK_NULL_HANDLE) return;

    if (slot.vertex_buffer.buffer != VK_NULL_HANDLE) {
        vmaDestroyBuffer(allocator_, slot.vertex_buffer.buffer, slot.vertex_buffer.allocation);
    }
    if (slot.index_buffer.buffer != VK_NULL_HANDLE) {
        vmaDestroyBuffer(allocator_, slot.index_buffer.buffer, slot.index_buffer.allocation);
    }

    slot.vertex_buffer = {};
    slot.index_buffer = {};
    slot.max_vertices = 0;
    slot.max_indices = 0;
}

size_t SpatialMeshBuffer::getVertexCapacity(uint32_t frame_slot) const {
    return slots_[frame_slot % MAX_FRAMES_IN_FLIGHT].max_vertices;
}

size_t SpatialMeshBuffer::getIndexCapacity(uint32_t frame_slot) const {
    return slots_[frame_slot % MAX_FRAMES_IN_FLIGHT].max_indices;
}

bool SpatialMeshBuffer::isGrowthPending(uint32_t frame_slot) const {
    return slots_[frame_slot % MAX_FRAMES_IN_FLIGHT].growth_pending;
}

void SpatialMeshBuffer::beginFrame() {
    if (graphics_ == nullptr) {
        throw std::runtime_error(
            "SpatialMeshBuffer::beginFrame() needs a NovaGraphics core; callers that own "
            "their own frame pacing must call beginFrame(frame_slot)");
    }
    beginFrame(graphics_->getCurrentFrameIndex());
}

void SpatialMeshBuffer::beginFrame(uint32_t frame_slot) {
    current_slot_ = frame_slot % MAX_FRAMES_IN_FLIGHT;
    FrameSlot& slot = slots_[current_slot_];

    // The caller has waited on this slot's in-flight fence, so no GPU work can
    // still be reading it. This is the ONLY point at which reallocating these
    // buffers is legal -- doing it from upload() would free memory out from
    // under a command buffer recorded one or two frames ago.
    if (slot.growth_pending) {
        applyPendingGrowth(slot, current_slot_);
    }

    vertices_.clear();
    indices_.clear();
    demanded_vertices_ = 0;
    demanded_indices_ = 0;
    overflowed_ = false;
}

void SpatialMeshBuffer::applyPendingGrowth(FrameSlot& slot, uint32_t slot_index) {
    slot.growth_pending = false;

    const size_t new_vertices = std::max(slot.max_vertices, target_vertices_);
    const size_t new_indices = std::max(slot.max_indices, target_indices_);
    if (new_vertices == slot.max_vertices && new_indices == slot.max_indices) {
        return;
    }

    destroySlot(slot);
    allocateSlot(slot, new_vertices, new_indices, "growth");

    report(LOGGER::DEBUG,
           "SpatialMeshBuffer - slot %u grown to %zu vertices / %zu indices",
           slot_index, new_vertices, new_indices);
}

void SpatialMeshBuffer::scheduleGrowth() {
    const size_t wanted_vertices = grownCapacity(demanded_vertices_);
    const size_t wanted_indices = grownCapacity(demanded_indices_);

    // Only a rise in the high-water target counts as a new growth event; a
    // scene that overflows identically every frame logs exactly once.
    if (wanted_vertices <= target_vertices_ && wanted_indices <= target_indices_) {
        return;
    }

    report(LOGGER::ERROR,
           "SpatialMeshBuffer - geometry truncated: frame demanded %zu vertices / %zu indices, "
           "slot %u holds %zu / %zu. Growing to %zu / %zu on this slot's next frame.",
           demanded_vertices_, demanded_indices_, current_slot_,
           slots_[current_slot_].max_vertices, slots_[current_slot_].max_indices,
           std::max(target_vertices_, wanted_vertices),
           std::max(target_indices_, wanted_indices));

    target_vertices_ = std::max(target_vertices_, wanted_vertices);
    target_indices_ = std::max(target_indices_, wanted_indices);

    for (uint32_t i = 0; i < MAX_FRAMES_IN_FLIGHT; ++i) {
        slots_[i].growth_pending = true;
    }
}

void SpatialMeshBuffer::append(const MeshData& mesh, uint32_t& out_first_index, uint32_t& out_index_count) {
    out_first_index = 0;
    out_index_count = 0;

    if (mesh.vertices.empty() || mesh.indices.empty()) return;

    // Demand is counted whether or not the mesh fits: it is the capacity the
    // next frame on this slot has to cover.
    demanded_vertices_ += mesh.vertices.size();
    demanded_indices_ += mesh.indices.size();

    const FrameSlot& slot = slots_[current_slot_];
    if (demanded_vertices_ > slot.max_vertices || demanded_indices_ > slot.max_indices) {
        // Drop the mesh whole. A partial append would emit indices pointing at
        // vertices that were never uploaded. Testing running demand rather than
        // remaining room also makes truncation a deterministic prefix instead
        // of letting small meshes slip in behind a dropped large one.
        overflowed_ = true;
        return;
    }

    out_first_index = static_cast<uint32_t>(indices_.size());
    out_index_count = static_cast<uint32_t>(mesh.indices.size());

    const uint32_t base_vertex = static_cast<uint32_t>(vertices_.size());
    vertices_.insert(vertices_.end(), mesh.vertices.begin(), mesh.vertices.end());

    for (uint32_t idx : mesh.indices) {
        indices_.push_back(base_vertex + idx);
    }
}

void SpatialMeshBuffer::upload() {
    // Bookkeeping first: it must run even on a frame that produced no drawable
    // geometry, otherwise a scene that overflows on its very first mesh would
    // never schedule the growth that unblocks it.
    if (overflowed_) {
        scheduleGrowth();
    }

    if (vertices_.empty() || indices_.empty()) return;

    const FrameSlot& slot = slots_[current_slot_];
    copyToBuffer(slot.vertex_buffer, vertices_.data(), vertices_.size() * sizeof(SpatialVertex));
    copyToBuffer(slot.index_buffer, indices_.data(), indices_.size() * sizeof(uint32_t));
}

void SpatialMeshBuffer::copyToBuffer(const Buffer_T& buffer, const void* source, size_t byte_size) {
    void* mapped = nullptr;
    const VkResult map_result = vmaMapMemory(allocator_, buffer.allocation, &mapped);
    if (map_result != VK_SUCCESS || mapped == nullptr) {
        report(LOGGER::ERROR, "SpatialMeshBuffer - vmaMapMemory failed (VkResult %d); frame not uploaded",
               static_cast<int>(map_result));
        return;
    }

    std::memcpy(mapped, source, byte_size);

    // No-op on coherent memory, required on non-coherent HOST_VISIBLE heaps.
    vmaFlushAllocation(allocator_, buffer.allocation, 0, byte_size);
    vmaUnmapMemory(allocator_, buffer.allocation);
}

void SpatialMeshBuffer::bind(VkCommandBuffer cmd) {
    const FrameSlot& slot = slots_[current_slot_];
    if (slot.vertex_buffer.buffer == VK_NULL_HANDLE || slot.index_buffer.buffer == VK_NULL_HANDLE) {
        return;
    }

    VkBuffer v_bufs[] = { slot.vertex_buffer.buffer };
    VkDeviceSize offsets[] = { 0 };
    vkCmdBindVertexBuffers(cmd, 0, 1, v_bufs, offsets);
    vkCmdBindIndexBuffer(cmd, slot.index_buffer.buffer, 0, VK_INDEX_TYPE_UINT32);
}

void SpatialMeshBuffer::draw(VkCommandBuffer cmd, uint32_t first_index, uint32_t index_count) {
    if (index_count == 0) return;

    // A command issued for geometry that truncation dropped would read past the
    // range actually uploaded this frame.
    if (static_cast<size_t>(first_index) + index_count > indices_.size()) return;

    vkCmdDrawIndexed(cmd, index_count, 1, first_index, 0, 0);
}

} // namespace Nova
