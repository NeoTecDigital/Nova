// Written by Richard Christopher, Copyright 2026 NeoTec Digital
#pragma once

#include <string>
#include <vector>
#include <unordered_map>
#include <cstdint>
#include <glm/glm.hpp>
#include <nlohmann/json_fwd.hpp>

struct OatsRuntime;

namespace Splash {

// Object type names emitted by the Rust runtime (extern/OATS-ffi/src/runtime.rs:57,71,90).
inline constexpr const char* kOatsTypeFileSystemEntity = "FileSystemEntity";
inline constexpr const char* kOatsTypeSpatialPill = "SpatialPill";
inline constexpr const char* kOatsTypeHypergraphNode = "HypergraphDAGNode";

// Trait payload decoding for both FFI encodings lives in Clouds/OatsTraitCodec.h.

struct OatsFileSystemEntity {
    std::string id;
    std::string name;
    std::string path;
    std::string extension;
    bool is_directory = false;
    uint64_t size_bytes = 0;
};

struct OatsSpatialPill {
    std::string id;
    std::string name;
    std::string path;
    glm::vec3 position{0.0f};
    // glm component order (x, y, z, w); w is the quaternion scalar. The FFI argument order
    // is (rot_w, rot_x, rot_y, rot_z) and is reordered at the call boundary.
    glm::vec4 orientation{0.0f, 0.0f, 0.0f, 1.0f};
    float radius = 0.0f;
    float height = 0.0f;
};

struct OatsHypergraphNode {
    std::string id;
    std::string name;
    std::string node_id;
    std::string name_space;
    std::string parents_json;
};

struct OatsSystemEvent {
    std::string type;
    std::string message;
    std::string timestamp;
};

/**
 * OatsBridge - C++ facade over the OATS-ffi Rust runtime.
 *
 * The step delta (organizer.rs:20-28) is the incremental synchronisation channel: object
 * additions, removals, trait updates and system events all arrive there with native JSON
 * payloads. A full reload via oats_runtime_get_entities_json is only performed on demand.
 */
class OatsBridge {
public:
    OatsBridge();
    ~OatsBridge();

    OatsBridge(const OatsBridge&) = delete;
    OatsBridge& operator=(const OatsBridge&) = delete;

    bool initialize();

    // Advances the runtime and applies the returned delta. False means the tick failed.
    bool step(float dt_secs);

    // Registration. Returns the runtime-assigned object id, or an empty string on failure.
    std::string registerFilesystemEntity(const std::string& path,
                                         bool is_directory,
                                         uint64_t size_bytes,
                                         const std::string& extension);

    std::string registerSpatialPill(const std::string& name,
                                    const std::string& path,
                                    const glm::vec3& position,
                                    const glm::vec4& orientation,
                                    float radius,
                                    float height);

    std::string registerHypergraphNode(const std::string& node_id,
                                       const std::string& name_space,
                                       const std::string& parents_json);

    // Per-frame pose channel. Cheap: no JSON round-trip, no full reload.
    bool updateSpatialPose(const std::string& id,
                           const glm::vec3& position,
                           const glm::vec4& orientation);

    // Full reconciliation against the runtime. Expensive; call only when the incremental
    // channel is known to be stale. False means the caches were left untouched.
    bool reloadFromRuntime();

    const std::unordered_map<std::string, OatsFileSystemEntity>& getFilesystemEntities() const {
        return filesystem_entities_;
    }
    const std::unordered_map<std::string, OatsSpatialPill>& getSpatialPills() const {
        return spatial_pills_;
    }
    const std::unordered_map<std::string, OatsHypergraphNode>& getHypergraphNodes() const {
        return hypergraph_nodes_;
    }
    const std::vector<OatsSystemEvent>& getRecentEvents() const { return recent_events_; }
    const std::vector<std::string>& getRegisteredTypeNames() const { return registered_type_names_; }

    // Ids removed by the runtime since the last drain. Callers own teardown of their views.
    std::vector<std::string> drainRemovedObjectIds();

    uint64_t getTick() const { return tick_; }
    size_t getObjectCount() const {
        return filesystem_entities_.size() + spatial_pills_.size() + hypergraph_nodes_.size();
    }

    bool isRunning() const { return runtime_ != nullptr; }
    bool isHealthy() const { return healthy_; }
    const std::string& getLastError() const { return last_error_; }
    uint64_t getErrorCount() const { return error_count_; }
    void clearError();

    std::string getTypesJson() const;
    std::string getEntitiesJson(const std::string& object_type = std::string()) const;

private:
    OatsRuntime* runtime_ = nullptr;

    std::unordered_map<std::string, OatsFileSystemEntity> filesystem_entities_;
    std::unordered_map<std::string, OatsSpatialPill> spatial_pills_;
    std::unordered_map<std::string, OatsHypergraphNode> hypergraph_nodes_;

    std::vector<OatsSystemEvent> recent_events_;
    std::vector<std::string> registered_type_names_;
    std::vector<std::string> pending_removed_ids_;

    uint64_t tick_ = 0;

    // Diagnostics channel: mutable so const query methods can still surface FFI failures
    // instead of silently returning empty payloads.
    mutable uint64_t error_count_ = 0;
    mutable bool healthy_ = true;
    mutable std::string last_error_;

    void recordFailure(const char* operation, const std::string& detail) const;
    bool requireRuntime(const char* operation) const;

    bool loadRegisteredTypes();
    bool applyDeltaJson(const std::string& delta_json);
    void applyEventDeltas(const nlohmann::json& delta);
    void applyRemovalDeltas(const nlohmann::json& delta);
    void applyAdditionDeltas(const nlohmann::json& delta);
    void applyTraitDeltas(const nlohmann::json& delta);

    bool ingestObject(const nlohmann::json& object, bool debug_encoded_traits);
    void applySingleTrait(const std::string& id,
                          const std::string& trait_name,
                          const nlohmann::json& data);
    void eraseObject(const std::string& id);
    void pushEvent(OatsSystemEvent&& event);
};

} // namespace Splash
