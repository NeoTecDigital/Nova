#pragma once

#include "./SpatialNode.h"
#include "Core/modules/spatial_pipeline/spatial_pipeline.h"
#include "Core/modules/spatial_pipeline/spatial_mesh.h"
#include "Core/modules/spatial_pipeline/spatial_font.h"
#include "Core/modules/spatial_pipeline/texture_bridge.h"
#include "Core/math/engine_physics.h"
#include "Core/math/spatial_cluster.h"
#include "Core/math/input_filter.h"
#include <memory>
#include <vector>

namespace Clouds {

class SpatialScene {
public:
    std::shared_ptr<SpatialNode> root;
    std::shared_ptr<NovaSpatial::SpatialFont> font;

    glm::vec3 camera_pos{0.0f, 0.0f, 2.5f};
    glm::vec3 camera_target{0.0f, 0.0f, 0.0f};
    glm::vec3 camera_up{0.0f, 1.0f, 0.0f};
    float fov_radians = glm::radians(60.0f);
    float near_plane = 0.01f;
    float far_plane = 100.0f;

    NovaMath::EnginePhysicsConfig physics_config;
    NovaMath::InputRayFilter input_filter;
    NovaMath::SpatialClusterIndex spatial_cluster_index{1.0f, 4};

    SpatialScene(NovaCore* core, NovaSpatial::TextureBridge* texture_bridge);
    ~SpatialScene();

    void initialize(const std::string& font_path = "/usr/share/fonts/TTF/SpaceMonoNerdFont-Regular.ttf");

    // Process 2D Screen input (translates pixel coords into 3D Quaternionic raycasts through filter)
    void processPointerMotion(const glm::vec2& screen_pixel, const glm::vec2& screen_size);
    void processPointerButton(uint32_t button, bool pressed);
    void processKey(uint32_t key, bool pressed);

    // Update scene dynamics, cluster spatial index, and phase physics
    void update(float dt);

    // Render 3D spatial scene to Vulkan command buffer
    void render(NovaSpatial::SpatialPipeline* pipeline, VkCommandBuffer cmd, const glm::vec2& screen_size);

    glm::vec3 cursor_3d_pos{0.0f, 0.0f, 0.0f};
    bool show_lookat_reticle = true;
    bool show_cursor_reticle = true;

    glm::mat4 getViewMatrix() const;
    glm::mat4 getProjectionMatrix(float aspect_ratio) const;

    // --- Focus ---
    //
    // Pointer focus follows the pointer; keyboard focus follows activation.
    // Conflating them means moving the pointer off a window steals its typing,
    // which is neither what a seat does nor what anyone expects.
    void setPointerFocus(std::shared_ptr<SpatialNode> node, const NovaMath::RayHit& enter_hit);
    void setPointerFocus(std::shared_ptr<SpatialNode> node) { setPointerFocus(std::move(node), last_hit_); }
    void setKeyboardFocus(std::shared_ptr<SpatialNode> node);

    // Drop every reference this scene holds to a node that is going away.
    // Focus and the implicit grab are the only strong references it keeps
    // outside the tree: a node removed from the tree while one of them names
    // it would go on receiving motion, and go on existing, indefinitely.
    void releaseNode(const std::shared_ptr<SpatialNode>& node);

    // --- Implicit pointer grab ---
    //
    // Held from the first button press to the last release: motion and the
    // release itself go to the pressed node however far the pointer wanders,
    // so a drag that leaves the quad still gets its release. Public because a
    // gesture recogniser or a menu may need to take the pointer explicitly.
    void grabPointer(std::shared_ptr<SpatialNode> node);
    void releasePointer();

    std::shared_ptr<SpatialNode> getPointerFocus() const { return pointer_focus_; }
    std::shared_ptr<SpatialNode> getKeyboardFocus() const { return keyboard_focus_; }
    std::shared_ptr<SpatialNode> getPointerGrab() const { return pointer_grab_; }

    // Retained spelling of getKeyboardFocus(): this always returned the node
    // set on press, which is keyboard focus.
    std::shared_ptr<SpatialNode> getFocusedNode() const { return keyboard_focus_; }
    const NovaMath::RayHit& getLastHit() const { return last_hit_; }

private:
    NovaCore* core_ = nullptr;
    NovaSpatial::TextureBridge* texture_bridge_ = nullptr;
    std::unique_ptr<NovaSpatial::SpatialMeshBuffer> mesh_buffer_;

    NovaSpatial::MeshData lookat_reticle_mesh_;
    NovaSpatial::MeshData cursor_reticle_mesh_;

    std::shared_ptr<SpatialNode> pointer_focus_;
    std::shared_ptr<SpatialNode> keyboard_focus_;
    std::shared_ptr<SpatialNode> pointer_grab_;

    // One bit per button index. The grab lasts until every button is up, so
    // releasing one of two held buttons does not drop the drag.
    uint32_t pressed_button_mask_ = 0;

    NovaMath::RayHit last_hit_;
    NovaMath::Ray3D last_pointer_ray_;
    bool has_pointer_sample_ = false;
    glm::vec2 last_screen_pixel_{0.0f};
    glm::vec2 last_screen_size_{1.0f};

    uint32_t frame_index_ = 0;
    float fps_accumulator_ = 0.0f;
    uint32_t fps_frames_ = 0;

    void rebuildSpatialIndex(std::shared_ptr<SpatialNode> node, uint32_t& out_node_count);

    NovaMath::Ray3D buildPointerRay(const glm::vec2& screen_pixel, const glm::vec2& screen_size);
    bool castPointerRay(const NovaMath::Ray3D& world_ray,
                        NovaMath::RayHit& out_hit,
                        std::shared_ptr<SpatialNode>& out_target);
    void updateHover(const NovaMath::Ray3D& world_ray);
    void deliverGrabMotion(const NovaMath::Ray3D& world_ray);
    void placeCursorOnMissedRay(const NovaMath::Ray3D& world_ray);
};

} // namespace Clouds
