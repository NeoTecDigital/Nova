// Written by Richard Christopher, Copyright 2026 NeoTec Digital
#include "./mesh_buffer.h"
#include "Nova/nova_graphics.h"
#include "Nova/components/logger.h"
#include <algorithm>
#include <cstring>
#include <stdexcept>

namespace Nova {

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
