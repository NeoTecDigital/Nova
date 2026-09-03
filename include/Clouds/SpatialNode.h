#pragma once

#include "Core/math/quaternion_transform.h"
#include "Core/math/raycast.h"
#include "Core/math/hyper_math.h"
#include "Core/modules/spatial_pipeline/spatial_pipeline.h"
#include "Core/modules/spatial_pipeline/spatial_mesh.h"
#include "Core/modules/spatial_pipeline/spatial_font.h"
#include <vector>
#include <memory>
#include <functional>
#include <string>

namespace Splash {

struct SpatialRenderCommand {
    glm::mat4 model;
    glm::vec4 surface_dim; // [width, height, aspect, time]
    std::shared_ptr<Nova::TextureHandle> texture;
    uint32_t first_index;
    uint32_t index_count;
};

class SpatialNode : public std::enable_shared_from_this<SpatialNode> {
public:
    Nova::Math::QuatTransform transform;
    Nova::Math::PhaseState8 phase_state; // Dual [a, b, c, w] primordial state: Spatial/Real + Chromatic/Phase
    glm::vec2 size{1.0f, 1.0f}; // Dimensions in 3D world units (meters)
    std::string name = "SpatialNode";

    bool visible = true;
    bool interactable = true;
    bool is_hovered = false;
    bool is_pressed = false;
    bool is_focused = false;

    // --- Input routing (resolved by SpatialScene, see resolveInputTarget) ---
    //
    // A node that captures its subtree is the input target for any hit inside
    // it that no descendant claims. Chrome -- a titlebar panel, a background,
    // a button's own glyph run -- is decoration its owner draws; a drag or a
    // focus click on it is about the owner, not the decoration. Widgets that
    // mean something on their own keep the claim and stay reachable, so the
    // capture never swallows a click meant for a child.
    //
    // claims_pointer_input defaults to true: every node is its own target
    // until whoever built it declares otherwise, which is the only place that
    // knows whether it is chrome.
    bool captures_subtree_input = false;
    bool claims_pointer_input = true;

    std::weak_ptr<SpatialNode> parent;
    std::vector<std::shared_ptr<SpatialNode>> children;

    SpatialNode() = default;
    virtual ~SpatialNode() = default;

    void addChild(std::shared_ptr<SpatialNode> child);
    void removeChild(std::shared_ptr<SpatialNode> child);

    // --- Sibling stacking ---
    //
    // Raise `child` above its siblings: it moves to the end of `children` and
    // takes `front_z`, every other child takes `back_z`. False if `child` is
    // not one of this node's children.
    //
    // Depth and paint order are written together on purpose. hitTest resolves
    // by distance and falls back to sibling order only on an exact tie, while
    // collectRender paints in sibling order outright -- so setting one without
    // the other leaves what the pointer hits and what is drawn on top free to
    // disagree.
    //
    // Both z values are the caller's. How far apart a shell separates its
    // top-levels is that shell's layout; a node has no basis for a default.
    //
    // is_focused is deliberately untouched. SpatialScene already owns it and
    // keeps it exclusive (setKeyboardFocus clears the previous holder), so a
    // second writer here would race it. This is the z half alone -- the half
    // the tree had nowhere else to put.
    bool raiseChild(const std::shared_ptr<SpatialNode>& child, float front_z, float back_z);

    // Re-points every descendant's parent link at its actual owner. addChild
    // runs this on whatever it attaches, which is what makes a subtree built
    // inside a constructor -- where weak_from_this() is still empty and the
    // links it writes are dropped on the floor -- correct once it is attached.
    // Public so a node used detached can put its own tree right.
    void relinkChildren();

    // Evolve complex phase distribution non-linearly
    void evolvePhase(float dt, float coupling = 1.0f) {
        phase_state = phase_state.coupleNonLinear(dt, coupling);
    }

    // Compute cumulative world transform
    Nova::Math::QuatTransform getWorldTransform() const;

    // Interactive event hooks
    virtual void onUpdate(float dt);
    virtual void onRayEnter(const Nova::Math::RayHit& hit);
    virtual void onRayMove(const Nova::Math::RayHit& hit);
    virtual void onRayLeave();
    virtual void onRayButton(const Nova::Math::RayHit& hit, uint32_t button, bool pressed);
    virtual void onKey(uint32_t key, bool pressed);

    // Raycast hit test against this node and its children. Reports the NEAREST
    // intersection in the subtree -- self included, not shadowed by it -- so
    // depth decides what the pointer is on. Ties go to the later sibling, which
    // is the one painted on top. Answers geometry only; which node the event is
    // routed to is SpatialScene's decision.
    virtual bool hitTest(const Nova::Math::Ray3D& world_ray, Nova::Math::RayHit& out_hit, std::shared_ptr<SpatialNode>& out_node);

    // Batch render collector: appends geometry to mesh_buf and adds draw calls to out_commands
    virtual void collectRender(Nova::SpatialMeshBuffer* mesh_buf, std::vector<SpatialRenderCommand>& out_commands);
};

} // namespace Splash
