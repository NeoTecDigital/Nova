// Written by Richard Christopher, Copyright 2026 NeoTec Digital
#pragma once

#include "./Registry.h"
#include "./SpatialNode.h"
#include "Nova/pipeline/spatial_pipeline.h"
#include "Nova/pipeline/mesh_buffer.h"
#include "Splash/content/spatial_font.h"
#include "Nova/pipeline/texture_bridge.h"
#include "Nova/math/engine_physics.h"
#include "Nova/math/input_filter.h"
#include <memory>
#include <string>
#include <vector>

namespace Splash {

class SpatialScene {
public:
    NodeId root;
    std::shared_ptr<Splash::SpatialFont> font;

    glm::vec3 camera_pos{0.0f, 0.0f, 2.5f};
    glm::vec3 camera_target{0.0f, 0.0f, 0.0f};
    glm::vec3 camera_up{0.0f, 1.0f, 0.0f};
    float fov_radians = glm::radians(60.0f);
    float near_plane = 0.01f;
    float far_plane = 100.0f;

    Nova::Math::EnginePhysicsConfig physics_config;
    Nova::Math::InputRayFilter input_filter;

    /**
     * @param registry The node store this scene's tree lives in. Held by
     *        reference and NOT owned: one registry serves a whole session, and
     *        it must outlive every scene built against it.
     */
    SpatialScene(Registry& registry, Nova::Core* core, Nova::TextureBridge* texture_bridge);
    ~SpatialScene();

    SpatialScene(const SpatialScene&) = delete;
    SpatialScene& operator=(const SpatialScene&) = delete;

    Registry& registry() const { return registry_; }

    void initialize(const std::string& font_path = "/usr/share/fonts/TTF/SpaceMonoNerdFont-Regular.ttf");

    // Process 2D Screen input (translates pixel coords into 3D Quaternionic raycasts through filter)
    void processPointerMotion(const glm::vec2& screen_pixel, const glm::vec2& screen_size);
    void processPointerButton(uint32_t button, bool pressed);
    void processKey(uint32_t key, bool pressed);

    // Update scene dynamics and phase physics
    void update(float dt);

    // Render 3D spatial scene to Vulkan command buffer
    void render(Nova::SpatialPipeline* pipeline, VkCommandBuffer cmd, const glm::vec2& screen_size);

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
    void setPointerFocus(NodeId node, const Nova::Math::RayHit& enter_hit);
    void setPointerFocus(NodeId node) { setPointerFocus(node, last_hit_); }
    void setKeyboardFocus(NodeId node);

    // Drop every reference this scene holds to a node that is going away.
    // Focus and the implicit grab are the only references it keeps outside the
    // tree: a node removed from the tree while one of them named it would go on
    // receiving motion. Safe on an id that is already dead.
    void releaseNode(NodeId node);

    // --- Implicit pointer grab ---
    //
    // Held from the first button press to the last release: motion and the
    // release itself go to the pressed node however far the pointer wanders,
    // so a drag that leaves the quad still gets its release. Public because a
    // gesture recogniser or a menu may need to take the pointer explicitly.
    void grabPointer(NodeId node);
    void releasePointer();

    NodeId getPointerFocus() const { return pointer_focus_; }
    NodeId getKeyboardFocus() const { return keyboard_focus_; }
    NodeId getPointerGrab() const { return pointer_grab_; }

    // Retained spelling of getKeyboardFocus(): this always returned the node
    // set on press, which is keyboard focus.
    NodeId getFocusedNode() const { return keyboard_focus_; }
    const Nova::Math::RayHit& getLastHit() const { return last_hit_; }

private:
    Registry& registry_;
    Nova::Core* core_ = nullptr;
    Nova::TextureBridge* texture_bridge_ = nullptr;
    std::unique_ptr<Nova::SpatialMeshBuffer> mesh_buffer_;

    Nova::MeshData lookat_reticle_mesh_;
    Nova::MeshData cursor_reticle_mesh_;

    NodeId pointer_focus_;
    NodeId keyboard_focus_;
    NodeId pointer_grab_;

    // One bit per button index. The grab lasts until every button is up, so
    // releasing one of two held buttons does not drop the drag.
    uint32_t pressed_button_mask_ = 0;

    Nova::Math::RayHit last_hit_;
    Nova::Math::Ray3D last_pointer_ray_;
    bool has_pointer_sample_ = false;
    glm::vec2 last_screen_pixel_{0.0f};
    glm::vec2 last_screen_size_{1.0f};

    uint32_t frame_index_ = 0;
    float fps_accumulator_ = 0.0f;
    uint32_t fps_frames_ = 0;

    // Focus and the grab name nodes they do not own, and a node may be
    // destroyed by anything that can reach the registry -- including its own
    // input handler. Every entry point below starts here rather than assuming
    // the ids it is holding still resolve.
    void dropDeadReferences();

    Nova::Math::Ray3D buildPointerRay(const glm::vec2& screen_pixel, const glm::vec2& screen_size);
    bool castPointerRay(const Nova::Math::Ray3D& world_ray,
                        Nova::Math::RayHit& out_hit,
                        NodeId& out_target);
    void updateHover(const Nova::Math::Ray3D& world_ray);
    void deliverGrabMotion(const Nova::Math::Ray3D& world_ray);
    void placeCursorOnMissedRay(const Nova::Math::Ray3D& world_ray);
};

} // namespace Splash
