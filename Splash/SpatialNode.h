// Written by Richard Christopher, Copyright 2026 NeoTec Digital
#pragma once

#include "Nova/math/quaternion_transform.h"
#include "Nova/math/raycast.h"
#include "Nova/math/hyper_math.h"
#include "Nova/pipeline/spatial_pipeline.h"
#include "Nova/pipeline/mesh_buffer.h"
#include "Splash/content/spatial_font.h"
#include "./NodeId.h"
#include <memory>
#include <string>
#include <vector>

namespace Splash {

class Registry;

struct SpatialRenderCommand {
    glm::mat4 model;
    glm::vec4 surface_dim; // [width, height, aspect, time]
    std::shared_ptr<Nova::TextureHandle> texture;
    uint32_t first_index;
    uint32_t index_count;
};

/**
 * SpatialNode - the behaviour half of a node.
 *
 * Identity, ownership and the hierarchy live in Splash::Registry; this object
 * is what a NodeId resolves to. It holds no parent pointer and no child list:
 * a tree whose links live in two places is a tree that can disagree with
 * itself, and the after-the-fact repair pass that used to re-point every
 * descendant's parent link is what that disagreement cost.
 */
class SpatialNode {
public:
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

    // Every node is constructed by Registry::emplace, which allocates and links
    // the slot BEFORE this runs -- so a constructor that assembles children
    // attaches them under `self` and there is no moment at which a node has
    // children but no identity. Both parameters are unused by the base and
    // named in the signature only because every subclass needs them.
    SpatialNode(Registry&, NodeId) {}
    virtual ~SpatialNode() = default;

    SpatialNode(const SpatialNode&) = delete;
    SpatialNode& operator=(const SpatialNode&) = delete;

    // Read-only view of this node's pose in its parent's frame.
    //
    // Registry::transform(id) is the ONLY write path, because a write has to
    // invalidate the world-transform cache of this node's whole subtree and a
    // public mutable field is a write that does not. Making the field private
    // is what turns that from a convention into a fact.
    const Nova::Math::QuatTransform& transform() const { return local_; }

    // Evolve complex phase distribution non-linearly
    void evolvePhase(float dt, float coupling = 1.0f) {
        phase_state = phase_state.coupleNonLinear(dt, coupling);
    }

    // --- Interactive event hooks ---
    //
    // Every hook takes the registry and its own id, because that is the only
    // thing a node has to reach the rest of the tree with -- and because it is
    // the signature the facet vtables of B2 dispatch through, so the bodies
    // move to those without their call sites moving.
    virtual void onRayEnter(Registry& reg, NodeId self, const Nova::Math::RayHit& hit);
    virtual void onRayMove(Registry& reg, NodeId self, const Nova::Math::RayHit& hit);
    virtual void onRayLeave(Registry& reg, NodeId self);
    virtual void onRayButton(Registry& reg, NodeId self, const Nova::Math::RayHit& hit,
                             uint32_t button, bool pressed);
    virtual void onKey(Registry& reg, NodeId self, uint32_t key, bool pressed);

    // Emit THIS node's geometry. Children are the traversal's job (see
    // collectSubtree), so a node that draws nothing is a container and the base
    // implementation is empty rather than a recursion every override chained to.
    virtual void collectRender(Registry& reg, NodeId self, Nova::SpatialMeshBuffer* mesh_buf,
                               std::vector<SpatialRenderCommand>& out_commands);

private:
    friend class Registry;
    Nova::Math::QuatTransform local_;
};

/**
 * Raycast the subtree rooted at `root`. Reports the NEAREST intersection in the
 * subtree -- every node on equal terms, none shadowed by its own parent -- so
 * depth decides what the pointer is on. Ties go to the later sibling, which is
 * the one painted on top. Answers geometry only; which node the event is routed
 * to is SpatialScene's decision.
 *
 * Iterative: a scene graph's depth is a client's to choose, and the stack is
 * not.
 */
bool hitTest(Registry& reg, NodeId root, const Nova::Math::Ray3D& world_ray,
             Nova::Math::RayHit& out_hit, NodeId& out_node);

/**
 * Depth-first render collection over the subtree rooted at `root`.
 *
 * Pre-order, siblings forward: a node is painted before its children and a
 * later sibling over an earlier one, which is the order hitTest breaks its ties
 * in. An invisible node prunes its whole subtree, exactly as it always did.
 */
void collectSubtree(Registry& reg, NodeId root, Nova::SpatialMeshBuffer* mesh_buf,
                    std::vector<SpatialRenderCommand>& out_commands);

} // namespace Splash
