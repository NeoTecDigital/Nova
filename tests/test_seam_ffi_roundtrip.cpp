// Written by Richard Christopher, Copyright 2026 NeoTec Digital
#include "extern/OATS-ffi/include/oats_ffi.h"

#include <iostream>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <set>
#include <string>
#include <vector>

// ---------------------------------------------------------------------------
// Assertion harness. Deliberately NOT <cassert>: assert() is compiled out under
// -DNDEBUG, which would silently turn every check below into a no-op.
// ---------------------------------------------------------------------------
static int g_failures = 0;

#define CHECK(cond, msg)                                                       \
    do {                                                                       \
        if (!(cond)) {                                                         \
            fprintf(stderr, "  [FAIL] %s:%d: %s\n", __FILE__, __LINE__, msg);  \
            ++g_failures;                                                      \
        }                                                                      \
    } while (0)

namespace {

/// Owns a char* handed across the FFI boundary and returns it to Rust exactly
/// once. Move-only so a buffer can never be double-freed by this test.
class OwnedFfiString {
public:
    explicit OwnedFfiString(char* raw) : raw_(raw) {}
    OwnedFfiString(const OwnedFfiString&) = delete;
    OwnedFfiString& operator=(const OwnedFfiString&) = delete;
    OwnedFfiString(OwnedFfiString&& other) noexcept : raw_(other.raw_) { other.raw_ = nullptr; }
    OwnedFfiString& operator=(OwnedFfiString&& other) noexcept {
        if (this != &other) {
            reset();
            raw_ = other.raw_;
            other.raw_ = nullptr;
        }
        return *this;
    }
    ~OwnedFfiString() { reset(); }

    bool valid() const { return raw_ != nullptr; }
    const char* c_str() const { return raw_ != nullptr ? raw_ : ""; }
    std::string str() const { return raw_ != nullptr ? std::string(raw_) : std::string(); }

private:
    void reset() {
        if (raw_ != nullptr) {
            oats_runtime_free_string(raw_);
            raw_ = nullptr;
        }
    }
    char* raw_ = nullptr;
};

bool contains(const std::string& haystack, const std::string& needle) {
    return haystack.find(needle) != std::string::npos;
}

// ---------------------------------------------------------------------------
// Error paths: a null runtime handle must be rejected by every entry point
// without dereferencing it.
// ---------------------------------------------------------------------------
void testNullRuntimeHandle() {
    CHECK(oats_runtime_step(nullptr, 0.016) == nullptr, "step(null) must return null");
    CHECK(oats_runtime_register_fs_entity(nullptr, "/tmp/x", false, 0, "txt") == nullptr,
          "register_fs_entity(null) must return null");
    CHECK(oats_runtime_register_spatial_pill(nullptr, "n", "/p", 0, 0, 0, 1, 0, 0, 0, 1, 1) == nullptr,
          "register_spatial_pill(null) must return null");
    CHECK(oats_runtime_register_hypergraph_node(nullptr, "id", "ns", "[]") == nullptr,
          "register_hypergraph_node(null) must return null");
    CHECK(oats_runtime_get_types_json(nullptr) == nullptr, "get_types_json(null) must return null");
    CHECK(oats_runtime_get_entities_json(nullptr, nullptr) == nullptr,
          "get_entities_json(null) must return null");

    // Void-returning entry points must simply no-op rather than fault.
    oats_runtime_update_spatial_pose(nullptr, "id", 0, 0, 0, 1, 0, 0, 0);
    oats_runtime_destroy(nullptr);
    oats_runtime_free_string(nullptr);
    oats_runtime_free_string(nullptr); // repeated null free is defined as a no-op
}

// ---------------------------------------------------------------------------
// Error paths: null string arguments. Required arguments reject, optional ones
// substitute a documented default.
// ---------------------------------------------------------------------------
void testNullStringArguments(OatsRuntime* runtime) {
    CHECK(oats_runtime_register_fs_entity(runtime, nullptr, false, 0, "txt") == nullptr,
          "register_fs_entity with null path must return null");
    CHECK(oats_runtime_register_spatial_pill(runtime, nullptr, "/p", 0, 0, 0, 1, 0, 0, 0, 1, 1) == nullptr,
          "register_spatial_pill with null name must return null");
    CHECK(oats_runtime_register_spatial_pill(runtime, "n", nullptr, 0, 0, 0, 1, 0, 0, 0, 1, 1) == nullptr,
          "register_spatial_pill with null path must return null");
    CHECK(oats_runtime_register_hypergraph_node(runtime, nullptr, "ns", "[]") == nullptr,
          "register_hypergraph_node with null node_id must return null");

    // ext, namespace and parents_json are optional: null is accepted and the
    // runtime substitutes "" / "" / "{}" respectively.
    OwnedFfiString fs_id(oats_runtime_register_fs_entity(runtime, "/tmp/null_ext", false, 7, nullptr));
    CHECK(fs_id.valid() && !fs_id.str().empty(), "null ext must be tolerated and still yield an id");

    OwnedFfiString node_id(oats_runtime_register_hypergraph_node(runtime, "optional_args", nullptr, nullptr));
    CHECK(node_id.valid() && !node_id.str().empty(),
          "null namespace and parents_json must be tolerated and still yield an id");

    // Null id on a pose update is a no-op, as is a well-formed but unknown id.
    oats_runtime_update_spatial_pose(runtime, nullptr, 1, 2, 3, 1, 0, 0, 0);
    oats_runtime_update_spatial_pose(runtime, "00000000-0000-0000-0000-000000000000", 1, 2, 3, 1, 0, 0, 0);
}

// ---------------------------------------------------------------------------
// Malformed input. The runtime stores parents_json verbatim without parsing,
// and invalid UTF-8 degrades to an empty string instead of aborting.
// ---------------------------------------------------------------------------
void testMalformedInput(OatsRuntime* runtime) {
    OwnedFfiString bad_json(oats_runtime_register_hypergraph_node(runtime, "malformed_parents", "ns", "{not valid json"));
    CHECK(bad_json.valid() && !bad_json.str().empty(),
          "malformed parents_json is stored verbatim and must still yield an id");

    const char invalid_utf8[] = {'\x66', '\xff', '\xfe', '\0'};
    OwnedFfiString bad_utf8(oats_runtime_register_fs_entity(runtime, invalid_utf8, false, 0, invalid_utf8));
    CHECK(bad_utf8.valid() && !bad_utf8.str().empty(),
          "invalid UTF-8 path must degrade to an empty string, not fault");

    OwnedFfiString empty_path(oats_runtime_register_fs_entity(runtime, "", true, 0, ""));
    CHECK(empty_path.valid() && !empty_path.str().empty(),
          "empty path is accepted and still yields a unique id");

    // An unknown type filter is a valid query that yields an empty JSON array.
    OwnedFfiString none(oats_runtime_get_entities_json(runtime, "NoSuchTypeAtAll"));
    CHECK(none.valid(), "get_entities_json with unknown type must return a string, not null");
    CHECK(none.str() == "[]", "unknown type filter must yield an empty JSON array");

    // An empty filter is treated as "no filter" and returns every object.
    OwnedFfiString all_empty_filter(oats_runtime_get_entities_json(runtime, ""));
    OwnedFfiString all_null_filter(oats_runtime_get_entities_json(runtime, nullptr));
    CHECK(all_empty_filter.valid() && all_null_filter.valid(),
          "empty and null type filters must both return a string");
    CHECK(all_empty_filter.str().size() == all_null_filter.str().size(),
          "empty type filter must behave identically to a null filter");
}

void testTypeRegistry(OatsRuntime* runtime) {
    OwnedFfiString types(oats_runtime_get_types_json(runtime));
    CHECK(types.valid(), "get_types_json must return a string for a live runtime");
    const std::string json = types.str();
    CHECK(json.size() > 2, "type registry must not be empty");
    CHECK(contains(json, "FileSystemEntity"), "built-in FileSystemEntity type must be registered");
    CHECK(contains(json, "SpatialPill"), "built-in SpatialPill type must be registered");
}

// ---------------------------------------------------------------------------
// Benchmarks with correctness layered on top.
// ---------------------------------------------------------------------------
std::vector<std::string> benchmarkSpawn(OatsRuntime* runtime, int num_nodes) {
    std::cout << " [INFO] Spawning " << num_nodes << " spatial pill nodes via FFI..." << std::endl;

    std::vector<std::string> spawned_ids;
    spawned_ids.reserve(num_nodes);

    const auto start = std::chrono::high_resolution_clock::now();
    for (int i = 0; i < num_nodes; ++i) {
        const std::string name = "PerfNode_" + std::to_string(i);
        OwnedFfiString id(oats_runtime_register_spatial_pill(
            runtime, name.c_str(), name.c_str(),
            i * 0.01f, i * 0.02f, -0.5f,
            1.0f, 0.0f, 0.0f, 0.0f,
            0.25f, 0.75f));
        if (!id.valid() || id.str().empty()) {
            CHECK(false, "register_spatial_pill must return a non-empty id");
            continue;
        }
        spawned_ids.push_back(id.str());
    }
    const auto end = std::chrono::high_resolution_clock::now();
    const double spawn_sec = std::chrono::duration<double>(end - start).count();
    std::cout << " [SEAM 5 RESULT] Spawned and indexed " << num_nodes << " dynamic nodes across C++/Rust FFI boundary in "
              << spawn_sec * 1000.0 << " ms (" << static_cast<double>(num_nodes) / spawn_sec << " nodes/sec)" << std::endl;

    CHECK(spawned_ids.size() == static_cast<size_t>(num_nodes), "every spawn must produce an id");
    const std::set<std::string> unique_ids(spawned_ids.begin(), spawned_ids.end());
    CHECK(unique_ids.size() == spawned_ids.size(), "every spawned node must receive a unique id");

    OwnedFfiString pills(oats_runtime_get_entities_json(runtime, "SpatialPill"));
    CHECK(pills.valid(), "SpatialPill query must return a string");
    const std::string pills_json = pills.str();
    CHECK(contains(pills_json, spawned_ids.front()), "first spawned id must be queryable by type");
    CHECK(contains(pills_json, spawned_ids.back()), "last spawned id must be queryable by type");
    return spawned_ids;
}

void benchmarkPoseUpdates(OatsRuntime* runtime, const std::vector<std::string>& ids, int num_updates) {
    CHECK(!ids.empty(), "pose update benchmark requires at least one spawned node");
    if (ids.empty()) {
        return;
    }

    const auto start = std::chrono::high_resolution_clock::now();
    for (int i = 0; i < num_updates; ++i) {
        const std::string& target_id = ids[static_cast<size_t>(i) % ids.size()];
        oats_runtime_update_spatial_pose(runtime, target_id.c_str(),
                                         static_cast<float>(i) * 0.001f, 0.5f, -0.5f,
                                         1.0f, 0.0f, 0.0f, 0.0f);
    }
    const auto end = std::chrono::high_resolution_clock::now();
    const double update_sec = std::chrono::duration<double>(end - start).count();
    std::cout << " [SEAM 5 RESULT] Executed " << num_updates << " full round-trip pose updates in "
              << update_sec * 1000.0 << " ms (" << static_cast<double>(num_updates) / update_sec << " roundtrips/sec)" << std::endl;

    // The pose writes must surface as trait deltas on the very next tick.
    OwnedFfiString delta(oats_runtime_step(runtime, 0.016));
    CHECK(delta.valid(), "step must return a delta string");
    const std::string delta_json = delta.str();
    CHECK(contains(delta_json, "updated_traits"), "delta must carry an updated_traits field");
    CHECK(contains(delta_json, ids.front()), "pose update must appear in the emitted trait delta");
    CHECK(contains(delta_json, "pos_x"), "pose update must emit a pos_x trait delta");
}

void benchmarkStepLoop(OatsRuntime* runtime, int num_steps, int num_nodes) {
    const auto start = std::chrono::high_resolution_clock::now();
    int valid_deltas = 0;
    for (int i = 0; i < num_steps; ++i) {
        OwnedFfiString delta(oats_runtime_step(runtime, 0.016));
        if (delta.valid() && contains(delta.str(), "\"tick\"")) {
            ++valid_deltas;
        }
    }
    const auto end = std::chrono::high_resolution_clock::now();
    const double step_sec = std::chrono::duration<double>(end - start).count();
    std::cout << " [SEAM 5 RESULT] Stepped " << num_steps << " full ECS ticks + JSON Delta reconciliations ("
              << num_nodes << " nodes) in " << step_sec * 1000.0 << " ms ("
              << static_cast<double>(num_steps) / step_sec << " FPS / Delta loops/sec)" << std::endl;

    CHECK(valid_deltas == num_steps, "every step must return a well-formed delta carrying a tick counter");
}

} // namespace

int main() {
    std::cout << "\n==========================================================================" << std::endl;
    std::cout << " [SEAM 5 BENCHMARK]: C++ <-> Rust FFI Delta Sync & Dynamic Node Limits" << std::endl;
    std::cout << "==========================================================================" << std::endl;

    testNullRuntimeHandle();

    OatsRuntime* runtime = oats_runtime_create();
    CHECK(runtime != nullptr, "oats_runtime_create must return a live handle");
    if (runtime == nullptr) {
        std::cout << " [SEAM 5 STATUS] C++ <-> Rust FFI Integration FAILED with " << g_failures << " failure(s)." << std::endl;
        return 1;
    }

    testNullStringArguments(runtime);
    testMalformedInput(runtime);
    testTypeRegistry(runtime);

    const int num_nodes = 200;
    const std::vector<std::string> spawned_ids = benchmarkSpawn(runtime, num_nodes);
    benchmarkPoseUpdates(runtime, spawned_ids, 500);
    benchmarkStepLoop(runtime, 1000, num_nodes);

    oats_runtime_destroy(runtime);
    runtime = nullptr;
    oats_runtime_destroy(runtime); // destroying a null handle again must be a no-op

    if (g_failures == 0) {
        std::cout << " [SEAM 5 STATUS] C++ <-> Rust FFI Integration PASSED with zero failures." << std::endl;
        return 0;
    }
    std::cout << " [SEAM 5 STATUS] C++ <-> Rust FFI Integration FAILED with " << g_failures << " failure(s)." << std::endl;
    return 1;
}
