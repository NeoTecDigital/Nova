#pragma once

#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct OatsRuntime OatsRuntime;

OatsRuntime* oats_runtime_create(void);
void oats_runtime_destroy(OatsRuntime* runtime);

char* oats_runtime_step(OatsRuntime* runtime, double delta_time_secs);

char* oats_runtime_register_fs_entity(
    OatsRuntime* runtime,
    const char* path,
    bool is_dir,
    uint64_t size_bytes,
    const char* ext
);

char* oats_runtime_register_spatial_pill(
    OatsRuntime* runtime,
    const char* name,
    const char* path,
    float pos_x,
    float pos_y,
    float pos_z,
    float rot_w,
    float rot_x,
    float rot_y,
    float rot_z,
    float radius,
    float height
);

char* oats_runtime_register_hypergraph_node(
    OatsRuntime* runtime,
    const char* node_id,
    const char* namespace_str,
    const char* parents_json
);

void oats_runtime_update_spatial_pose(
    OatsRuntime* runtime,
    const char* id,
    float pos_x,
    float pos_y,
    float pos_z,
    float rot_w,
    float rot_x,
    float rot_y,
    float rot_z
);

char* oats_runtime_get_types_json(OatsRuntime* runtime);
char* oats_runtime_get_entities_json(OatsRuntime* runtime, const char* obj_type);

void oats_runtime_free_string(char* s);

#ifdef __cplusplus
}
#endif
