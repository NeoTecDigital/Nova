// Written by Richard Christopher, Copyright 2026 NeoTec Digital
#pragma once

#include "Nova/core_base.h"
#include "./spatial_vertex.h"
#include "Nova/math/quaternion_transform.h"
#include <array>
#include <string>
#include <vector>
#include <memory>

namespace Nova {

class Graphics;

struct MeshData {
    std::vector<SpatialVertex> vertices;
    std::vector<uint32_t> indices;
};

/**
 * MeshCache - Value-keyed hold for generated geometry.
 *
 * UI geometry is otherwise regenerated from scratch on every collectRender()
 * pass. The key is the exact parameter tuple that was handed to the generator,
 * so callers may mutate their public fields freely and the cache still
 * self-invalidates: there is no dirty flag that can be forgotten.
 *
 * Signature slots are generator-specific and compared for exact equality --
 * these are copies of the caller's own values, never recomputed results, so
 * bitwise float equality is the correct predicate.
 */
class MeshCache {
public:
    static constexpr size_t SIGNATURE_SLOTS = 12;
    using Signature = std::array<float, SIGNATURE_SLOTS>;

    bool isValidFor(const Signature& params) const {
        return valid_ && text_.empty() && params_ == params;
    }

    bool isValidFor(const Signature& params, const std::string& text) const {
        return valid_ && params_ == params && text_ == text;
    }

    void store(const Signature& params, MeshData mesh) {
        params_ = params;
        text_.clear();
        mesh_ = std::move(mesh);
        valid_ = true;
    }

    void store(const Signature& params, const std::string& text, MeshData mesh) {
        params_ = params;
        text_ = text;
        mesh_ = std::move(mesh);
        valid_ = true;
    }

    const MeshData& mesh() const { return mesh_; }
    void invalidate() { valid_ = false; }

private:
    Signature params_{};
    std::string text_;
    MeshData mesh_;
    bool valid_ = false;
};

class SpatialMeshGenerator {
public:
    // Generate an oriented 3D rectangular plane centered at local origin (0, 0, 0)
    // with UVs (0,0) top-left to (1,1) bottom-right
    static MeshData createPlanarQuad(const glm::vec2& size,
                                     const glm::vec4& color = glm::vec4(1.0f),
                                     float border_thickness = 0.0f,
                                     float corner_radius = 0.0f,
                                     float render_mode = 0.0f);

    // Generate a curved cylindrical arc quad in 3D space.
    // A radius at or below MIN_ARC_RADIUS has no arc to sweep and degenerates
    // to a planar quad rather than dividing by zero.
    static MeshData createCurvedArc(const glm::vec2& size,
                                    float radius,
                                    uint32_t segments = 32,
                                    const glm::vec4& color = glm::vec4(1.0f),
                                    float border_thickness = 0.0f,
                                    float corner_radius = 0.0f,
                                    float render_mode = 0.0f);

    // Generate a 3D reticle (annular circle ring + crosshair bars)
    static MeshData createReticle(const glm::vec4& circle_color,
                                  const glm::vec4& crosshair_color,
                                  float radius = 0.06f,
                                  float ring_thickness = 0.006f,
                                  float crosshair_length = 0.08f,
                                  float crosshair_thickness = 0.005f);

    static constexpr float MIN_ARC_RADIUS = 1e-5f;
};

/**
 * SpatialMeshBuffer - Per-frame-in-flight dynamic geometry ring.
 *
 * One vertex/index buffer pair exists per frame-in-flight slot. Recording frame
 * N writes only into slot N % MAX_FRAMES_IN_FLIGHT, whose in-flight fence the
 * caller has already waited on, so a frame never overwrites memory that an
 * earlier still-executing frame is reading.
 *
 * Growth is deferred, never immediate: a frame whose geometry exceeds the
 * current slot capacity is drawn truncated and schedules a resize that is
 * applied at the START of the next frame that lands on each slot -- after that
 * slot's fence wait. Buffers are never destroyed while a command buffer that
 * references them is being recorded or is in flight.
 *
 * Lifecycle per frame:
 *   beginFrame()  -> apply any pending growth for this slot, clear CPU staging
 *   append() ...  -> accumulate geometry, dropping whole meshes past capacity
 *   upload()      -> memcpy into this slot, schedule growth if it overflowed
 *   bind()/draw() -> record against this slot's buffers
 */
class SpatialMeshBuffer {
public:
    static constexpr size_t DEFAULT_VERTEX_CAPACITY = 65536;
    static constexpr size_t DEFAULT_INDEX_CAPACITY = 131072;

    SpatialMeshBuffer(Core* core,
                      size_t initial_vertex_count = DEFAULT_VERTEX_CAPACITY,
                      size_t initial_index_count = DEFAULT_INDEX_CAPACITY);
    ~SpatialMeshBuffer();

    SpatialMeshBuffer(const SpatialMeshBuffer&) = delete;
    SpatialMeshBuffer& operator=(const SpatialMeshBuffer&) = delete;

    // Begin the frame-in-flight slot the graphics core is currently recording.
    // Requires the core to be a Graphics; throws otherwise.
    void beginFrame();

    // Begin an explicit frame-in-flight slot. The caller guarantees the GPU has
    // finished with this slot (its in-flight fence has been waited on).
    void beginFrame(uint32_t frame_slot);

    void append(const MeshData& mesh, uint32_t& out_first_index, uint32_t& out_index_count);
    void upload();

    void bind(VkCommandBuffer cmd);
    void draw(VkCommandBuffer cmd, uint32_t first_index, uint32_t index_count);

    size_t getVertexCount() const { return vertices_.size(); }
    size_t getIndexCount() const { return indices_.size(); }

    uint32_t getFrameSlot() const { return current_slot_; }
    size_t getVertexCapacity(uint32_t frame_slot) const;
    size_t getIndexCapacity(uint32_t frame_slot) const;

    // Geometry offered this frame, including meshes dropped by truncation.
    size_t getDemandedVertexCount() const { return demanded_vertices_; }
    size_t getDemandedIndexCount() const { return demanded_indices_; }
    bool didOverflow() const { return overflowed_; }
    bool isGrowthPending(uint32_t frame_slot) const;

private:
    struct FrameSlot {
        Buffer_T vertex_buffer{};
        Buffer_T index_buffer{};
        size_t max_vertices = 0;
        size_t max_indices = 0;
        bool growth_pending = false;
    };

    Core* core_ = nullptr;
    Graphics* graphics_ = nullptr;
    VmaAllocator allocator_ = VK_NULL_HANDLE;

    std::vector<SpatialVertex> vertices_;
    std::vector<uint32_t> indices_;

    FrameSlot slots_[MAX_FRAMES_IN_FLIGHT];
    uint32_t current_slot_ = 0;

    size_t demanded_vertices_ = 0;
    size_t demanded_indices_ = 0;
    bool overflowed_ = false;

    // Largest capacity ever requested; a growth event is logged only when these
    // rise, which keeps the warning at once per event rather than once per frame.
    size_t target_vertices_ = 0;
    size_t target_indices_ = 0;

    Buffer_T createBuffer(size_t byte_size, VkBufferUsageFlags usage, const char* label);
    void allocateSlot(FrameSlot& slot, size_t vertex_count, size_t index_count, const char* reason);
    void destroySlot(FrameSlot& slot);
    void applyPendingGrowth(FrameSlot& slot, uint32_t slot_index);
    void scheduleGrowth();
    void copyToBuffer(const Buffer_T& buffer, const void* source, size_t byte_size);
};

} // namespace Nova
