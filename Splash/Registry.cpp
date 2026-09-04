// Written by Richard Christopher, Copyright 2026 NeoTec Digital
#include "./Registry.h"
#include "Nova/components/logger.h"

namespace Splash {

Registry::~Registry() {
    // Nothing is walking the tree in a destructor, and the objects hold
    // shared_ptr copies of fonts and textures that their owners expect to be
    // released here rather than leaked.
    in_traversal_ = false;
    kill_.clear();
    for (Slot& slot : slots_) {
        slot.object.reset();
    }
}

bool Registry::refuseDuringTraversal(const char* what) const {
    if (!in_traversal_) return false;
    report(LOGGER::ERROR, "Splash::Registry - %s refused: the tree is being traversed", what);
    return true;
}

// --- Slot allocation -------------------------------------------------------

NodeId Registry::claimSlot() {
    if (!free_.empty()) {
        const uint32_t reused = free_.front();
        free_.pop_front();
        return NodeId::make(reused, gen_[reused]);
    }

    if (slots_.size() > NodeId::MAX_INDEX) {
        report(LOGGER::ERROR, "Splash::Registry - slot space exhausted at %zu nodes", slots_.size());
        return INVALID_NODE;
    }

    const uint32_t fresh = static_cast<uint32_t>(slots_.size());
    slots_.emplace_back();
    gen_.push_back(static_cast<uint16_t>(NodeId::FIRST_GENERATION));
    return NodeId::make(fresh, NodeId::FIRST_GENERATION);
}

NodeId Registry::allocate(NodeId parent) {
    if (refuseDuringTraversal("node creation")) return INVALID_NODE;

    // A named parent must be one a child can actually be attached to. An id
    // that is dead, or alive only until the next drain, is neither: creating
    // under it would silently enrol the new node in a subtree already on its
    // way out, and the caller would be holding a handle that dies at a drain it
    // never asked for. INVALID means detached; doomed does not mean doomed.
    if (parent.valid() && !attachable(parent)) {
        report(LOGGER::ERROR,
               "Splash::Registry - creation refused: parent %u is %s",
               parent.index(), alive(parent) ? "already scheduled for destruction" : "dead");
        return INVALID_NODE;
    }

    const NodeId self = claimSlot();
    if (!self.valid()) return self;

    Slot& slot = slots_[self.index()];
    slot.parent = INVALID_NODE;
    slot.first_child = INVALID_NODE;
    slot.last_child = INVALID_NODE;
    slot.next_sibling = INVALID_NODE;
    slot.prev_sibling = INVALID_NODE;
    slot.world_cache = Nova::Math::QuatTransform();
    slot.world_dirty = true;
    slot.in_use = true;
    slot.pending_destroy = false;
    ++live_count_;

    // Linked before the constructor runs, which is the whole reason emplace
    // hands the constructor its own id.
    if (parent.valid()) appendChild(parent, self);
    return self;
}

// --- Liveness --------------------------------------------------------------

bool Registry::alive(NodeId id) const {
    if (!id.valid()) return false;
    const uint32_t slot_index = id.index();
    if (slot_index >= slots_.size()) return false;
    return gen_[slot_index] == id.generation() && slots_[slot_index].in_use;
}

// Alive AND not already on the kill list. Every structural edit asks this
// rather than alive(): destroy() unlinks a subtree the instant it is called but
// frees it at the next drain, so a node stays alive for a whole frame after it
// has stopped being part of the tree. Linking one back in is how a freed slot
// ends up named by a live parent's first_child.
bool Registry::attachable(NodeId id) const {
    return alive(id) && !slots_[id.index()].pending_destroy;
}

bool Registry::worldIsCached(NodeId id) const {
    return alive(id) && !slots_[id.index()].world_dirty;
}

SpatialNode* Registry::get(NodeId id) {
    return alive(id) ? slots_[id.index()].object.get() : nullptr;
}

const SpatialNode* Registry::get(NodeId id) const {
    return alive(id) ? slots_[id.index()].object.get() : nullptr;
}

// --- Hierarchy links -------------------------------------------------------

NodeId Registry::parentOf(NodeId id) const { return alive(id) ? slots_[id.index()].parent : INVALID_NODE; }
NodeId Registry::firstChild(NodeId id) const { return alive(id) ? slots_[id.index()].first_child : INVALID_NODE; }
NodeId Registry::lastChild(NodeId id) const { return alive(id) ? slots_[id.index()].last_child : INVALID_NODE; }
NodeId Registry::nextSibling(NodeId id) const { return alive(id) ? slots_[id.index()].next_sibling : INVALID_NODE; }
NodeId Registry::prevSibling(NodeId id) const { return alive(id) ? slots_[id.index()].prev_sibling : INVALID_NODE; }

std::vector<NodeId> Registry::children(NodeId id) const {
    std::vector<NodeId> found;
    if (!alive(id)) return found;
    for (NodeId child = slots_[id.index()].first_child; child.valid();
         child = slots_[child.index()].next_sibling) {
        found.push_back(child);
    }
    return found;
}

bool Registry::isDescendant(NodeId node, NodeId ancestor) const {
    if (!alive(node) || !alive(ancestor)) return false;
    for (NodeId walk = node; walk.valid(); walk = slots_[walk.index()].parent) {
        if (walk == ancestor) return true;
    }
    return false;
}

void Registry::appendChild(NodeId parent, NodeId child) {
    Slot& parent_slot = slots_[parent.index()];
    Slot& child_slot = slots_[child.index()];

    child_slot.parent = parent;
    child_slot.next_sibling = INVALID_NODE;
    child_slot.prev_sibling = parent_slot.last_child;

    if (parent_slot.last_child.valid()) {
        slots_[parent_slot.last_child.index()].next_sibling = child;
    } else {
        parent_slot.first_child = child;
    }
    parent_slot.last_child = child;
}

void Registry::unlinkFromParent(NodeId child) {
    Slot& child_slot = slots_[child.index()];
    const NodeId parent = child_slot.parent;

    if (parent.valid()) {
        Slot& parent_slot = slots_[parent.index()];
        if (parent_slot.first_child == child) parent_slot.first_child = child_slot.next_sibling;
        if (parent_slot.last_child == child) parent_slot.last_child = child_slot.prev_sibling;
    }
    if (child_slot.prev_sibling.valid()) {
        slots_[child_slot.prev_sibling.index()].next_sibling = child_slot.next_sibling;
    }
    if (child_slot.next_sibling.valid()) {
        slots_[child_slot.next_sibling.index()].prev_sibling = child_slot.prev_sibling;
    }

    child_slot.parent = INVALID_NODE;
    child_slot.next_sibling = INVALID_NODE;
    child_slot.prev_sibling = INVALID_NODE;
}

// --- Reparenting -----------------------------------------------------------

void Registry::reparent(NodeId child, NodeId parent) {
    if (!alive(child)) return;
    if (refuseDuringTraversal("reparenting")) return;

    // alive() is not the question here; attachable() is. A child already
    // scheduled for destruction has been unlinked and will be freed at the next
    // drain, and drain frees it without unlinking - so relinking it now would
    // leave `parent` naming a slot that is about to be recycled.
    if (!attachable(child)) {
        report(LOGGER::ERROR, "Splash::Registry - reparent refused: node %u is scheduled for destruction",
               child.index());
        return;
    }
    if (parent.valid() && !attachable(parent)) return;

    // A node cannot be moved inside its own subtree: the result is a cycle, and
    // every walk over it is an infinite loop rather than a wrong answer.
    if (isDescendant(parent, child)) {
        report(LOGGER::ERROR, "Splash::Registry - reparent refused: '%s' would contain its own ancestor",
               slots_[child.index()].object ? slots_[child.index()].object->name.c_str() : "<unbuilt>");
        return;
    }

    unlinkFromParent(child);
    if (parent.valid()) appendChild(parent, child);
    dirtySubtree(child);
}

void Registry::reparentKeepingWorld(NodeId child, NodeId parent) {
    // Guarded here as well as inside reparent(): when the child is already
    // under `parent` the relink is a no-op and control falls through to the
    // transform() write below, which is a structural edit's worth of cache
    // invalidation in the middle of somebody's walk.
    if (refuseDuringTraversal("reparenting")) return;
    if (!attachable(child)) return;
    if (parent.valid() && !attachable(parent)) return;

    const Nova::Math::QuatTransform world = worldOf(child);
    reparent(child, parent);
    if (parentOf(child) != parent) return;   // refused; the pose it kept is still correct

    Nova::Math::QuatTransform& local = transform(child);
    if (!parent.valid()) {
        local = world;
        return;
    }

    // Solve parent_world.combine(local) == world, which combine() defines
    // componentwise: position through the parent's inverse, orientation and
    // scale by cancelling the parent's own.
    const Nova::Math::QuatTransform& parent_world = worldOf(parent);
    local.position = parent_world.inverseTransformPoint(world.position);
    local.orientation = glm::conjugate(parent_world.orientation) * world.orientation;
    local.scale = world.scale / parent_world.scale;
}

bool Registry::raiseChild(NodeId parent, NodeId child, float front_z, float back_z) {
    if (!alive(parent) || !alive(child)) return false;

    // The parent needs its own test. destroy() unlinks the subtree ROOT but
    // leaves the links inside that subtree standing, so every node below a
    // scheduled parent still names it - and restacking siblings inside a
    // subtree that is on its way out is z written to nodes nobody will draw.
    // The child needs none: a scheduled node has been unlinked, so its parent
    // is INVALID and the membership test below is what refuses it.
    if (!attachable(parent)) return false;
    if (slots_[child.index()].parent != parent) return false;
    if (refuseDuringTraversal("sibling raise")) return false;

    for (NodeId sibling = slots_[parent.index()].first_child; sibling.valid();
         sibling = slots_[sibling.index()].next_sibling) {
        transform(sibling).position.z = back_z;
    }
    transform(child).position.z = front_z;

    // Last in the sibling list is last painted and takes hitTest's tie-break,
    // so the raised node is moved there rather than merely marked as raised.
    unlinkFromParent(child);
    appendChild(parent, child);
    return true;
}

// --- World transform cache -------------------------------------------------

void Registry::dirtySubtree(NodeId root) {
    walk_scratch_.clear();
    walk_scratch_.push_back(root);

    while (!walk_scratch_.empty()) {
        const NodeId id = walk_scratch_.back();
        walk_scratch_.pop_back();

        Slot& slot = slots_[id.index()];
        // INVARIANT: a dirty node's descendants are all dirty, so a node that is
        // already dirty has nothing below it left to mark.
        if (slot.world_dirty) continue;
        slot.world_dirty = true;

        for (NodeId child = slot.first_child; child.valid(); child = slots_[child.index()].next_sibling) {
            walk_scratch_.push_back(child);
        }
    }
}

Nova::Math::QuatTransform& Registry::transform(NodeId id) {
    dirtySubtree(id);
    return slots_[id.index()].object->local_;
}

const Nova::Math::QuatTransform& Registry::worldOf(NodeId id) {
    Slot& slot = slots_[id.index()];
    if (!slot.world_dirty) return slot.world_cache;

    // Up to the nearest clean ancestor, then back down: the chain is the only
    // part of the tree whose world is unknown.
    walk_scratch_.clear();
    for (NodeId walk = id; walk.valid();) {
        Slot& walk_slot = slots_[walk.index()];
        if (!walk_slot.world_dirty) break;
        walk_scratch_.push_back(walk);
        walk = walk_slot.parent;
    }

    for (auto it = walk_scratch_.rbegin(); it != walk_scratch_.rend(); ++it) {
        Slot& chain_slot = slots_[it->index()];
        const NodeId parent = chain_slot.parent;
        chain_slot.world_cache = parent.valid()
            ? slots_[parent.index()].world_cache.combine(chain_slot.object->local_)
            : chain_slot.object->local_;
        chain_slot.world_dirty = false;
    }
    return slot.world_cache;
}

// --- Destruction -----------------------------------------------------------

void Registry::collectSubtreeIds(NodeId root, std::vector<NodeId>& out) const {
    const size_t first = out.size();
    out.push_back(root);
    for (size_t read = first; read < out.size(); ++read) {
        for (NodeId child = slots_[out[read].index()].first_child; child.valid();
             child = slots_[child.index()].next_sibling) {
            out.push_back(child);
        }
    }
}

void Registry::destroy(NodeId id) {
    if (!alive(id)) return;
    if (slots_[id.index()].pending_destroy) return;
    if (refuseDuringTraversal("node destruction")) return;

    unlinkFromParent(id);
    dirtySubtree(id);

    // Marked NOW, freed at drain: the subtree is out of the tree from this
    // instant, so nothing renders or hit-tests it, while a handler that is
    // still running on one of these nodes returns into memory that is still its
    // own. Every node is marked so a later destroy() of a descendant, or of an
    // ancestor, cannot schedule the same subtree twice.
    std::vector<NodeId> doomed;
    collectSubtreeIds(id, doomed);
    for (NodeId node : doomed) {
        slots_[node.index()].pending_destroy = true;
    }
    kill_.push_back(id);
}

void Registry::freeSlot(uint32_t slot_index) {
    Slot& slot = slots_[slot_index];
    slot.object.reset();
    slot.parent = INVALID_NODE;
    slot.first_child = INVALID_NODE;
    slot.last_child = INVALID_NODE;
    slot.next_sibling = INVALID_NODE;
    slot.prev_sibling = INVALID_NODE;
    slot.world_dirty = true;
    slot.in_use = false;
    slot.pending_destroy = false;
    --live_count_;

    // The generation is what makes a reused slot a different node. When it runs
    // out the slot is retired rather than wrapped: reissuing generation 1 would
    // make the oldest surviving handle to this index live again.
    const uint32_t next_generation = static_cast<uint32_t>(gen_[slot_index]) + 1u;
    gen_[slot_index] = static_cast<uint16_t>(next_generation);
    if (next_generation > NodeId::MAX_GENERATION) {
        quarantined_.push_back(slot_index);
        return;
    }
    free_.push_back(slot_index);
}

void Registry::drain() {
    if (kill_.empty()) return;
    if (refuseDuringTraversal("deferred destruction")) return;

    // Swapped out first so a destructor that schedules more destruction cannot
    // invalidate the iteration; the loop picks that up on its next pass.
    //
    // Every id in kill_ is alive and every subtree in it is disjoint, and that
    // is a property of destroy() rather than something re-checked here:
    // destroy() refuses a dead id, refuses one already scheduled, and unlinks
    // the subtree it takes - while attachable() stops anything being created or
    // relinked under a node already on this list. A second liveness test here
    // would be a branch no sequence of calls can reach, and a branch that
    // cannot be reached is a branch that cannot be trusted.
    std::vector<NodeId> roots;
    std::vector<NodeId> doomed;
    while (!kill_.empty()) {
        roots.clear();
        roots.swap(kill_);
        doomed.clear();
        for (NodeId root : roots) {
            collectSubtreeIds(root, doomed);
        }
        for (NodeId node : doomed) {
            freeSlot(node.index());
        }
    }
}

} // namespace Splash
