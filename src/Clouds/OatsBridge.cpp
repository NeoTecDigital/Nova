// Written by Richard Christopher, Copyright 2026 NeoTec Digital

#include "include/Clouds/OatsBridge.h"
#include "include/Clouds/OatsTraitCodec.h"
#include "Core/components/logger.h"
#include "extern/OATS-ffi/include/oats_ffi.h"

#include <nlohmann/json.hpp>
#include <exception>
#include <string>
#include <utility>

using json = nlohmann::json;

namespace Clouds {
namespace {

constexpr size_t kMaxRecentEvents = 64;

// Takes ownership of an FFI-allocated string, copies it and releases the Rust allocation.
std::string takeFfiString(char* raw) {
    if (!raw) {
        return {};
    }
    std::string value(raw);
    oats_runtime_free_string(raw);
    return value;
}

// Builds a typed object from its traits payload and upserts it into `store` by id.
template <typename T, typename ParseFn>
bool ingestTyped(std::unordered_map<std::string, T>& store, const std::string& id,
                 const std::string& name, const json& traits, bool debug_encoded,
                 ParseFn parse, std::string& error) {
    T value;
    value.id = id;
    value.name = name;
    if (!parse(traits, debug_encoded, value, error)) {
        return false;
    }
    store[id] = std::move(value);
    return true;
}

} // namespace

// ---------------------------------------------------------------------------
// Lifecycle and diagnostics
// ---------------------------------------------------------------------------

OatsBridge::OatsBridge() = default;

OatsBridge::~OatsBridge() {
    if (runtime_) {
        oats_runtime_destroy(runtime_);
        runtime_ = nullptr;
    }
}

void OatsBridge::recordFailure(const char* operation, const std::string& detail) const {
    healthy_ = false;
    ++error_count_;
    last_error_ = std::string(operation) + ": " + detail;
    report(LOGGER::ERROR, "OatsBridge - %s", last_error_.c_str());
}

bool OatsBridge::requireRuntime(const char* operation) const {
    if (runtime_) {
        return true;
    }
    recordFailure(operation, "OATS runtime is not initialised");
    return false;
}

void OatsBridge::clearError() {
    healthy_ = true;
    last_error_.clear();
}

bool OatsBridge::initialize() {
    if (runtime_) {
        recordFailure("initialize", "runtime already initialised");
        return false;
    }
    report(LOGGER::INFO, "OatsBridge - creating OATS-ffi runtime");
    runtime_ = oats_runtime_create();
    if (!runtime_) {
        recordFailure("initialize", "oats_runtime_create returned null");
        return false;
    }
    if (!loadRegisteredTypes() || !reloadFromRuntime()) {
        return false;
    }
    report(LOGGER::INFO, "OatsBridge - runtime ready (%zu registered types, %zu live objects)",
           registered_type_names_.size(), getObjectCount());
    return true;
}

bool OatsBridge::loadRegisteredTypes() {
    registered_type_names_.clear();
    const std::string raw = getTypesJson();
    if (raw.empty()) {
        return false;
    }
    try {
        const json types = json::parse(raw);
        if (!types.is_array()) {
            recordFailure("loadRegisteredTypes", "get_types_json did not return an array");
            return false;
        }
        for (const auto& entry : types) {
            registered_type_names_.push_back(entry.value("type_name", std::string()));
        }
    } catch (const std::exception& e) {
        recordFailure("loadRegisteredTypes", std::string("type registry parse failed: ") + e.what());
        return false;
    }
    return true;
}

// ---------------------------------------------------------------------------
// Raw FFI queries
// ---------------------------------------------------------------------------

std::string OatsBridge::getTypesJson() const {
    if (!requireRuntime("getTypesJson")) {
        return {};
    }
    std::string payload = takeFfiString(oats_runtime_get_types_json(runtime_));
    if (payload.empty()) {
        recordFailure("getTypesJson", "oats_runtime_get_types_json returned no payload");
    }
    return payload;
}

std::string OatsBridge::getEntitiesJson(const std::string& object_type) const {
    if (!requireRuntime("getEntitiesJson")) {
        return {};
    }
    const char* filter = object_type.empty() ? nullptr : object_type.c_str();
    std::string payload = takeFfiString(oats_runtime_get_entities_json(runtime_, filter));
    if (payload.empty()) {
        recordFailure("getEntitiesJson", "oats_runtime_get_entities_json returned no payload");
    }
    return payload;
}

// ---------------------------------------------------------------------------
// Stepping: the incremental synchronisation channel
// ---------------------------------------------------------------------------

bool OatsBridge::step(float dt_secs) {
    if (!requireRuntime("step")) {
        return false;
    }
    char* raw = oats_runtime_step(runtime_, static_cast<double>(dt_secs));
    if (!raw) {
        recordFailure("step", "oats_runtime_step returned a null delta");
        return false;
    }
    return applyDeltaJson(takeFfiString(raw));
}

bool OatsBridge::applyDeltaJson(const std::string& delta_json) {
    json delta;
    try {
        delta = json::parse(delta_json);
    } catch (const std::exception& e) {
        recordFailure("step", std::string("delta JSON parse failed: ") + e.what());
        return false;
    }
    if (!delta.is_object()) {
        recordFailure("step", "delta payload is not a JSON object");
        return false;
    }
    tick_ = delta.value("tick", tick_);
    applyRemovalDeltas(delta);
    applyAdditionDeltas(delta);
    applyTraitDeltas(delta);
    applyEventDeltas(delta);
    return true;
}

void OatsBridge::applyRemovalDeltas(const json& delta) {
    const auto it = delta.find("removed_object_ids");
    if (it == delta.end() || !it->is_array()) {
        return;
    }
    for (const auto& entry : *it) {
        if (!entry.is_string()) {
            continue;
        }
        std::string id = entry.get<std::string>();
        eraseObject(id);
        pending_removed_ids_.push_back(std::move(id));
    }
}

void OatsBridge::applyAdditionDeltas(const json& delta) {
    const auto it = delta.find("added_objects");
    if (it == delta.end() || !it->is_array()) {
        return;
    }
    for (const auto& object : *it) {
        ingestObject(object, false);
    }
}

void OatsBridge::applyTraitDeltas(const json& delta) {
    const auto it = delta.find("updated_traits");
    if (it == delta.end() || !it->is_array()) {
        return;
    }
    for (const auto& update : *it) {
        const std::string id = update.value("object_id", std::string());
        const std::string trait_name = update.value("trait_name", std::string());
        if (id.empty() || trait_name.empty() || !update.contains("data")) {
            continue;
        }
        applySingleTrait(id, trait_name, update.at("data"));
    }
}

void OatsBridge::applyEventDeltas(const json& delta) {
    const auto it = delta.find("events");
    if (it == delta.end() || !it->is_array()) {
        return;
    }
    for (const auto& entry : *it) {
        OatsSystemEvent event;
        event.type = entry.value("event_type", std::string("INFO"));
        event.message = entry.value("message", std::string());
        event.timestamp = entry.value("timestamp_utc", std::string());
        pushEvent(std::move(event));
    }
}

void OatsBridge::applySingleTrait(const std::string& id, const std::string& trait_name,
                                  const json& data) {
    const auto pill = spatial_pills_.find(id);
    if (pill != spatial_pills_.end()) {
        applyOatsPillTrait(pill->second, trait_name, data);
        return;
    }
    const auto entity = filesystem_entities_.find(id);
    if (entity != filesystem_entities_.end()) {
        applyOatsEntityTrait(entity->second, trait_name, data);
        return;
    }
    const auto node = hypergraph_nodes_.find(id);
    if (node != hypergraph_nodes_.end()) {
        applyOatsNodeTrait(node->second, trait_name, data);
    }
}

// ---------------------------------------------------------------------------
// Cache maintenance
// ---------------------------------------------------------------------------

bool OatsBridge::ingestObject(const json& object, bool debug_encoded_traits) {
    if (!object.is_object()) {
        recordFailure("ingestObject", "object entry is not a JSON object");
        return false;
    }
    const std::string id = object.value("id", std::string());
    const std::string type = object.value("type", std::string());
    if (id.empty() || type.empty()) {
        recordFailure("ingestObject", "object entry is missing 'id' or 'type'");
        return false;
    }
    static const json kNoTraits = json::object();
    const json& traits = object.contains("traits") ? object.at("traits") : kNoTraits;
    const std::string name = object.value("name", std::string());
    const bool debug = debug_encoded_traits;
    std::string error;
    bool parsed = true;

    if (type == kOatsTypeSpatialPill) {
        parsed = ingestTyped(spatial_pills_, id, name, traits, debug, &parseOatsSpatialPill, error);
    } else if (type == kOatsTypeFileSystemEntity) {
        parsed = ingestTyped(filesystem_entities_, id, name, traits, debug,
                             &parseOatsFileSystemEntity, error);
    } else if (type == kOatsTypeHypergraphNode) {
        parsed = ingestTyped(hypergraph_nodes_, id, name, traits, debug,
                             &parseOatsHypergraphNode, error);
    } else {
        // A type the bridge does not model yet is an expected condition as the Rust
        // TypeRegistry grows; it is not a failure of this synchronisation pass.
        report(LOGGER::DEBUG, "OatsBridge - ignoring unmodelled object type '%s'", type.c_str());
        return true;
    }

    if (!parsed) {
        recordFailure("ingestObject", error);
        return false;
    }
    return true;
}

void OatsBridge::eraseObject(const std::string& id) {
    spatial_pills_.erase(id);
    filesystem_entities_.erase(id);
    hypergraph_nodes_.erase(id);
}

void OatsBridge::pushEvent(OatsSystemEvent&& event) {
    if (recent_events_.size() >= kMaxRecentEvents) {
        recent_events_.erase(recent_events_.begin());
    }
    recent_events_.push_back(std::move(event));
}

std::vector<std::string> OatsBridge::drainRemovedObjectIds() {
    std::vector<std::string> drained;
    drained.swap(pending_removed_ids_);
    return drained;
}

bool OatsBridge::reloadFromRuntime() {
    if (!requireRuntime("reloadFromRuntime")) {
        return false;
    }
    const std::string raw = getEntitiesJson();
    if (raw.empty()) {
        return false;
    }
    json entities;
    try {
        entities = json::parse(raw);
    } catch (const std::exception& e) {
        recordFailure("reloadFromRuntime", std::string("entity snapshot parse failed: ") + e.what());
        return false;
    }
    if (!entities.is_array()) {
        recordFailure("reloadFromRuntime", "get_entities_json did not return an array");
        return false;
    }
    spatial_pills_.clear();
    filesystem_entities_.clear();
    hypergraph_nodes_.clear();

    bool complete = true;
    for (const auto& object : entities) {
        complete = ingestObject(object, true) && complete;
    }
    return complete;
}

// ---------------------------------------------------------------------------
// Registration and mutation
// ---------------------------------------------------------------------------

std::string OatsBridge::registerFilesystemEntity(const std::string& path, bool is_directory,
                                                 uint64_t size_bytes,
                                                 const std::string& extension) {
    if (!requireRuntime("registerFilesystemEntity")) {
        return {};
    }
    if (path.empty()) {
        recordFailure("registerFilesystemEntity", "path must not be empty");
        return {};
    }
    std::string id = takeFfiString(oats_runtime_register_fs_entity(
        runtime_, path.c_str(), is_directory, size_bytes, extension.c_str()));
    if (id.empty()) {
        recordFailure("registerFilesystemEntity", "runtime rejected path '" + path + "'");
        return {};
    }
    OatsFileSystemEntity entity;
    entity.id = id;
    entity.name = path; // runtime.rs:57 names the object after its path
    entity.path = path;
    entity.extension = extension;
    entity.is_directory = is_directory;
    entity.size_bytes = size_bytes;
    filesystem_entities_[id] = std::move(entity);
    return id;
}

std::string OatsBridge::registerSpatialPill(const std::string& name, const std::string& path,
                                            const glm::vec3& position,
                                            const glm::vec4& orientation,
                                            float radius, float height) {
    if (!requireRuntime("registerSpatialPill")) {
        return {};
    }
    if (name.empty()) {
        recordFailure("registerSpatialPill", "name must not be empty");
        return {};
    }
    std::string id = takeFfiString(oats_runtime_register_spatial_pill(
        runtime_, name.c_str(), path.c_str(), position.x, position.y, position.z,
        orientation.w, orientation.x, orientation.y, orientation.z, radius, height));
    if (id.empty()) {
        recordFailure("registerSpatialPill", "runtime rejected pill '" + name + "'");
        return {};
    }
    OatsSpatialPill pill;
    pill.id = id;
    pill.name = name;
    pill.path = path;
    pill.position = position;
    pill.orientation = orientation;
    pill.radius = radius;
    pill.height = height;
    spatial_pills_[id] = std::move(pill);
    return id;
}

std::string OatsBridge::registerHypergraphNode(const std::string& node_id,
                                               const std::string& name_space,
                                               const std::string& parents_json) {
    if (!requireRuntime("registerHypergraphNode")) {
        return {};
    }
    if (node_id.empty()) {
        recordFailure("registerHypergraphNode", "node_id must not be empty");
        return {};
    }
    std::string id = takeFfiString(oats_runtime_register_hypergraph_node(
        runtime_, node_id.c_str(), name_space.c_str(), parents_json.c_str()));
    if (id.empty()) {
        recordFailure("registerHypergraphNode", "runtime rejected node '" + node_id + "'");
        return {};
    }
    OatsHypergraphNode node;
    node.id = id;
    node.name = node_id; // runtime.rs:90 names the object after its node id
    node.node_id = node_id;
    node.name_space = name_space;
    node.parents_json = parents_json;
    hypergraph_nodes_[id] = std::move(node);
    return id;
}

bool OatsBridge::updateSpatialPose(const std::string& id, const glm::vec3& position,
                                   const glm::vec4& orientation) {
    if (!requireRuntime("updateSpatialPose")) {
        return false;
    }
    const auto it = spatial_pills_.find(id);
    if (it == spatial_pills_.end()) {
        recordFailure("updateSpatialPose", "no spatial pill cached for id '" + id + "'");
        return false;
    }
    oats_runtime_update_spatial_pose(runtime_, id.c_str(), position.x, position.y, position.z,
                                     orientation.w, orientation.x, orientation.y, orientation.z);
    it->second.position = position;
    it->second.orientation = orientation;
    return true;
}

} // namespace Clouds
