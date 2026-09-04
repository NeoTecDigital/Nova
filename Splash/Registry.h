// Written by Richard Christopher, Copyright 2026 NeoTec Digital
#pragma once

#include "./NodeId.h"
#include "./SpatialNode.h"
#include "Nova/math/quaternion_transform.h"

#include <cstddef>
#include <cstdint>
#include <deque>
#include <memory>
#include <utility>
#include <vector>

namespace Splash {

/**
 * Registry - the one owner of every node, flat.
 *
 * Parent and child are LINKS, not ownership: the hierarchy is an intrusive
 * doubly-linked sibling list stored beside the node, so raising a child to the
 * front is O(1) and a node costs no heap allocation of its own for its
 * children. destroy(id) destroys the subtree - which is what a tree of
 * shared_ptr children already meant - but defers the free to drain(), so a node
 * may be destroyed from inside its own event handler without the handler
 * returning into freed memory.
 *
 * World transforms are memoized. transform(id) is the only write path and it
 * dirties the subtree; worldOf(id) validates lazily up to the nearest clean
 * ancestor. A per-frame top-down pass was rejected because a caller may aim,
 * press and read a world transform without a frame ever having been drawn.
 */
class Registry {
public:
    Registry() = default;
    ~Registry();

    Registry(const Registry&) = delete;
    Registry& operator=(const Registry&) = delete;

    /**
     * Construct a node of type T under `parent` (INVALID parent = detached).
     *
     * The slot is allocated and linked BEFORE T's constructor runs, and T
     * receives (Registry&, NodeId self, args...) - so a constructor that
     * assembles children attaches them under `self` and they are parented from
     * the instant they exist. The constructor-ordering bug class that the old
     * after-the-fact parent-link repair pass existed to paper over cannot recur
     * here: there is no moment at which a node has children but no identity.
     */
    template <class T, class... Args>
    NodeId emplace(NodeId parent, Args&&... args) {
        const NodeId self = allocate(parent);
        if (!self.valid()) return self;
        auto object = std::make_unique<T>(*this, self, std::forward<Args>(args)...);
        slots_[self.index()].object = std::move(object);
        return self;
    }

    // A node with no behaviour of its own: draws nothing, hosts children.
    NodeId createContainer(NodeId parent) { return emplace<SpatialNode>(parent); }

    // Unlinks the subtree NOW; frees it at drain(). Idempotent on dead ids and
    // on a subtree already scheduled.
    void destroy(NodeId id);

    // Release everything destroy() scheduled. Once per frame, strictly outside
    // event dispatch and outside a render traversal.
    void drain();

    bool alive(NodeId id) const;

    // Alive, and not already scheduled for destruction. This - not alive() - is
    // what every structural edit gates on: destroy() unlinks immediately and
    // frees at the next drain, so a node stays alive for a whole frame after it
    // has left the tree, and linking one back in would hand a live parent a
    // slot that is about to be recycled.
    bool attachable(NodeId id) const;

    // Whether this node's world transform is memoized right now. The only
    // observable the cache has: it says the walk that follows will not have to
    // recompute, which is the whole claim worldOf() makes.
    bool worldIsCached(NodeId id) const;

    // Null when dead - never a stale pointer.
    SpatialNode* get(NodeId id);
    const SpatialNode* get(NodeId id) const;

    // Asserts liveness by construction: only ever called where the caller has
    // already established it. get() is the checked form.
    SpatialNode& operator[](NodeId id) { return *slots_[id.index()].object; }
    const SpatialNode& operator[](NodeId id) const { return *slots_[id.index()].object; }

    // The legacy object as its concrete type, or null. B2 replaces this with a
    // NodeKind comparison; until the kinds exist, the type IS the kind.
    template <class T>
    T* as(NodeId id) { return dynamic_cast<T*>(get(id)); }
    template <class T>
    const T* as(NodeId id) const { return dynamic_cast<const T*>(get(id)); }

    // Keeps the child's LOCAL pose (today's addChild semantics). An INVALID
    // parent detaches. Refuses a cycle rather than building one.
    void reparent(NodeId child, NodeId parent);

    // Keeps the child's WORLD pose: the local is re-solved against the new
    // parent so nothing appears to move.
    void reparentKeepingWorld(NodeId child, NodeId parent);

    /**
     * Raise `child` above its siblings: it moves to the end of the sibling list
     * and takes `front_z`, every other child takes `back_z`. False if `child` is
     * not one of `parent`'s children.
     *
     * Depth and paint order are written together on purpose. hitTest resolves by
     * distance and falls back to sibling order only on an exact tie, while
     * collectSubtree paints in sibling order outright -- so setting one without
     * the other leaves what the pointer hits and what is drawn on top free to
     * disagree. Both z values are the caller's: how far apart a shell separates
     * its top-levels is that shell's layout, and a node has no basis for a
     * default. is_focused is deliberately untouched; SpatialScene owns it.
     */
    bool raiseChild(NodeId parent, NodeId child, float front_z, float back_z);

    // Write-through: dirties this node's whole subtree's world cache.
    Nova::Math::QuatTransform& transform(NodeId id);

    // Cumulative world transform, validated lazily.
    const Nova::Math::QuatTransform& worldOf(NodeId id);

    NodeId parentOf(NodeId id) const;
    NodeId firstChild(NodeId id) const;
    NodeId lastChild(NodeId id) const;
    NodeId nextSibling(NodeId id) const;
    NodeId prevSibling(NodeId id) const;

    // Snapshot, so a caller may mutate the tree while walking it.
    std::vector<NodeId> children(NodeId id) const;

    // Is `node` `ancestor`, or anywhere inside it? Inclusive, because every
    // containment question this answers ("is the pointer inside that popup")
    // means the subtree including its root.
    bool isDescendant(NodeId node, NodeId ancestor) const;

    size_t liveCount() const { return live_count_; }

    // Slots burnt by generation wrap, never reissued. Reported so a session
    // that is churning nodes hard enough to retire slots can say so.
    size_t quarantinedCount() const { return quarantined_.size(); }

    /**
     * Structural mutation is forbidden while a render traversal is walking the
     * tree: a collect hook that reparents or destroys is editing the list the
     * walk is standing in. Held by collectSubtree for the length of the walk.
     */
    class TraversalScope {
    public:
        explicit TraversalScope(Registry& registry) : registry_(registry) {
            registry_.in_traversal_ = true;
        }
        ~TraversalScope() { registry_.in_traversal_ = false; }
        TraversalScope(const TraversalScope&) = delete;
        TraversalScope& operator=(const TraversalScope&) = delete;

    private:
        Registry& registry_;
    };

private:
    struct Slot {
        std::unique_ptr<SpatialNode> object;

        NodeId parent;
        NodeId first_child;
        NodeId last_child;
        NodeId next_sibling;
        NodeId prev_sibling;

        Nova::Math::QuatTransform world_cache;
        bool world_dirty = true;
        bool in_use = false;
        bool pending_destroy = false;
    };

    NodeId allocate(NodeId parent);
    NodeId claimSlot();

    void appendChild(NodeId parent, NodeId child);
    void unlinkFromParent(NodeId child);
    void dirtySubtree(NodeId root);
    void collectSubtreeIds(NodeId root, std::vector<NodeId>& out) const;
    void freeSlot(uint32_t slot_index);
    bool refuseDuringTraversal(const char* what) const;

    // Reference stability across nested creation is not a convenience here: a
    // constructor that builds children allocates slots WHILE its own slot is
    // being written, and a vector that reallocated under that would invalidate
    // every SpatialNode& a caller was holding. std::deque never moves an
    // element that has been pushed, and slots are reused rather than erased.
    std::deque<Slot> slots_;
    std::vector<uint16_t> gen_;

    // FIFO, so churn spreads across the pool rather than burning one slot's
    // generations first.
    std::deque<uint32_t> free_;

    std::vector<NodeId> kill_;
    std::vector<uint32_t> quarantined_;

    // Scratch, reused: the walks below run per pointer sample and per frame.
    std::vector<NodeId> walk_scratch_;

    size_t live_count_ = 0;
    bool in_traversal_ = false;
};

} // namespace Splash
