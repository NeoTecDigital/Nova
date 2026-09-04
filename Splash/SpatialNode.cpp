// Written by Richard Christopher, Copyright 2026 NeoTec Digital
#include "./SpatialNode.h"
#include "./Registry.h"

namespace Splash {

void SpatialNode::onRayEnter(Registry&, NodeId, const Nova::Math::RayHit&) {
    is_hovered = true;
}

void SpatialNode::onRayMove(Registry&, NodeId, const Nova::Math::RayHit&) {
}

void SpatialNode::onRayLeave(Registry&, NodeId) {
    is_hovered = false;
    is_pressed = false;
}

void SpatialNode::onRayButton(Registry&, NodeId, const Nova::Math::RayHit&, uint32_t button, bool pressed) {
    if (button == 1) { // Left mouse button
        is_pressed = pressed;
        if (pressed) {
            is_focused = true;
        }
    }
}

void SpatialNode::onKey(Registry&, NodeId, uint32_t, bool) {
}

void SpatialNode::collectRender(Registry&, NodeId, Nova::SpatialMeshBuffer*,
                                std::vector<SpatialRenderCommand>&) {
    // A plain node is a container: it has no surface of its own to draw, and
    // its children are collected by the traversal below.
}

namespace {

// Push a node's children so a stack pops them in sibling order: last child
// first, which makes the pop order first-to-last.
void pushChildrenReversed(Registry& reg, NodeId parent, std::vector<NodeId>& stack) {
    for (NodeId child = reg.lastChild(parent); child.valid(); child = reg.prevSibling(child)) {
        stack.push_back(child);
    }
}

} // namespace

bool hitTest(Registry& reg, NodeId root, const Nova::Math::Ray3D& world_ray,
             Nova::Math::RayHit& out_hit, NodeId& out_node) {
    if (!reg.alive(root)) return false;

    // Seeded at infinity, so the first real intersection always wins.
    Nova::Math::RayHit best;
    NodeId best_node;

    std::vector<NodeId> stack;
    stack.push_back(root);

    while (!stack.empty()) {
        const NodeId id = stack.back();
        stack.pop_back();

        const SpatialNode& node = reg[id];
        if (!node.visible) continue;   // an invisible node prunes its whole subtree

        // Every node on equal terms, none shadowed by its own ancestor: testing
        // a parent after its children is what let a descendant behind it win.
        // A non-interactable node still hosts its children; it just has no
        // surface of its own to be hit.
        //
        // Nearest wins, and on an exact tie the LATER node in paint order takes
        // it -- which is why the comparison replaces on equality and the walk
        // runs in the same order collectSubtree paints in.
        if (node.interactable) {
            Nova::Math::RayHit hit;
            if (Nova::Math::intersectOrientedQuad(world_ray, reg.worldOf(id), node.size, hit) &&
                hit.distance <= best.distance) {
                best = hit;
                best_node = id;
            }
        }
        pushChildrenReversed(reg, id, stack);
    }

    if (!best_node.valid()) return false;
    out_hit = best;
    out_node = best_node;
    return true;
}

void collectSubtree(Registry& reg, NodeId root, Nova::SpatialMeshBuffer* mesh_buf,
                    std::vector<SpatialRenderCommand>& out_commands) {
    if (!reg.alive(root) || mesh_buf == nullptr) return;

    // Structural mutation from inside a collect hook would be editing the list
    // this walk is standing in; the registry refuses it for the walk's duration.
    Registry::TraversalScope traversal(reg);

    std::vector<NodeId> stack;
    stack.push_back(root);

    while (!stack.empty()) {
        const NodeId id = stack.back();
        stack.pop_back();

        SpatialNode& node = reg[id];
        if (!node.visible) continue;

        node.collectRender(reg, id, mesh_buf, out_commands);
        pushChildrenReversed(reg, id, stack);
    }
}

} // namespace Splash
