#include "include/Clouds/SpatialNode.h"
#include <algorithm>

namespace Splash {

void SpatialNode::addChild(std::shared_ptr<SpatialNode> child) {
    if (!child) return;
    child->parent = weak_from_this();
    children.push_back(child);

    // A node that assembled its own children inside its constructor could not
    // link them: enable_shared_from_this installs its weak reference in the
    // shared_ptr constructor, which has not run yet, so weak_from_this() was
    // empty and every one of those links was silently dropped. Attaching the
    // node is the first moment a shared owner exists, so repair them here.
    child->relinkChildren();
}

void SpatialNode::relinkChildren() {
    const std::weak_ptr<SpatialNode> self = weak_from_this();

    for (const std::shared_ptr<SpatialNode>& child : children) {
        if (!child) continue;
        child->parent = self;      // membership of children IS the parent claim
        child->relinkChildren();
    }
}

void SpatialNode::removeChild(std::shared_ptr<SpatialNode> child) {
    auto it = std::remove(children.begin(), children.end(), child);
    if (it == children.end()) return;

    // The parent link is cleared through `child`, never through *it: everything
    // from `it` to end() is moved-from after std::remove, so *it is a null
    // shared_ptr whenever the removed node was not the last one - which is
    // every removal from a node with more than one child.
    if (child) child->parent.reset();
    children.erase(it, children.end());
}

Nova::Math::QuatTransform SpatialNode::getWorldTransform() const {
    if (auto p = parent.lock()) {
        return p->getWorldTransform().combine(transform);
    }
    return transform;
}

void SpatialNode::onUpdate(float dt) {
    for (auto& child : children) {
        if (child) {
            child->onUpdate(dt);
        }
    }
}

void SpatialNode::onRayEnter(const Nova::Math::RayHit&) {
    is_hovered = true;
}

void SpatialNode::onRayMove(const Nova::Math::RayHit&) {
}

void SpatialNode::onRayLeave() {
    is_hovered = false;
    is_pressed = false;
}

void SpatialNode::onRayButton(const Nova::Math::RayHit&, uint32_t button, bool pressed) {
    if (button == 1) { // Left mouse button
        is_pressed = pressed;
        if (pressed) {
            is_focused = true;
        }
    }
}

void SpatialNode::onKey(uint32_t, bool) {
}

bool SpatialNode::hitTest(const Nova::Math::Ray3D& world_ray, Nova::Math::RayHit& out_hit, std::shared_ptr<SpatialNode>& out_node) {
    if (!visible) return false;

    // Seeded at infinity, so the first real intersection always wins.
    Nova::Math::RayHit best;
    std::shared_ptr<SpatialNode> best_node;

    // Self first, and on equal terms with the children rather than after them:
    // testing it last is what let a descendant shadow its own parent whatever
    // the depths were. A non-interactable node still hosts its children; it
    // just has no surface of its own to be hit.
    if (interactable) {
        Nova::Math::RayHit self_hit;
        if (Nova::Math::intersectOrientedQuad(world_ray, getWorldTransform(), size, self_hit)) {
            best = self_hit;
            best_node = shared_from_this();
        }
    }

    // Nearest wins. On an exact tie the later sibling takes it, matching the
    // order collectRender paints them in: last drawn is on top.
    for (const std::shared_ptr<SpatialNode>& child : children) {
        if (!child) continue;

        Nova::Math::RayHit child_hit;
        std::shared_ptr<SpatialNode> child_node;
        if (!child->hitTest(world_ray, child_hit, child_node)) continue;
        if (child_hit.distance > best.distance) continue;

        best = child_hit;
        best_node = child_node;
    }

    if (!best_node) return false;

    out_hit = best;
    out_node = best_node;
    return true;
}

void SpatialNode::collectRender(Nova::SpatialMeshBuffer* mesh_buf, std::vector<SpatialRenderCommand>& out_commands) {
    if (!visible) return;

    for (auto& child : children) {
        if (child) {
            child->collectRender(mesh_buf, out_commands);
        }
    }
}

} // namespace Splash
