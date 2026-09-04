// Written by Richard Christopher, Copyright 2026 NeoTec Digital
//
// The refusals that keep a node scheduled for destruction out of the live tree.
//
// destroy() unlinks a subtree the instant it is called and frees it at the next
// drain, so for a whole frame a node is alive, out of the tree, and about to
// have its slot recycled. Every structural edit has to refuse it in that
// window: relinking one, creating under one, or restacking inside one leaves a
// live parent naming a slot that is about to be reissued, and every walk over
// that parent follows the link. These are the tests for that window.
//
// Split out of test_registry.cpp, which covers the store's mechanics - slot
// reuse, generations, reparenting, the world cache and drain itself.
//
// GPU-free: Splash::Registry touches no device at all and nothing here builds
// a scene.

#include "Splash/Registry.h"

#include <cmath>
#include <cstdio>
#include <iostream>

// Assertion harness. Deliberately NOT <cassert>: assert() is compiled out under
// -DNDEBUG, which would silently turn every check below into a no-op.
static int g_failures = 0;

#define CHECK(cond, msg)                                                       \
    do {                                                                       \
        if (!(cond)) {                                                         \
            fprintf(stderr, "  [FAIL] %s:%d: %s\n", __FILE__, __LINE__, msg);  \
            ++g_failures;                                                      \
        }                                                                      \
    } while (0)

#define CHECK_NEAR(actual, expected, tol, msg)                                 \
    do {                                                                       \
        const double actual_ = static_cast<double>(actual);                    \
        const double expected_ = static_cast<double>(expected);                \
        if (!(std::fabs(actual_ - expected_) <= (tol))) {                      \
            fprintf(stderr, "  [FAIL] %s:%d: %s (got %.9f, expected %.9f)\n",  \
                    __FILE__, __LINE__, msg, actual_, expected_);              \
            ++g_failures;                                                      \
        }                                                                      \
    } while (0)

namespace {

using Splash::INVALID_NODE;
using Splash::NodeId;
using Splash::Registry;
using Splash::SpatialNode;

// 6b. The two refusals the drain loop rests on, each pinned on its own.
//
// A destructor that names another node is the whole reason they exist: it runs
// from inside freeSlot, so the node it names may be one this same pass has
// already freed.
class DestroyingProbe : public SpatialNode {
public:
    DestroyingProbe(Registry& reg, NodeId self) : SpatialNode(reg, self), registry_(&reg) {}
    ~DestroyingProbe() override {
        if (target.valid()) registry_->destroy(target);
    }

    NodeId target;

private:
    Registry* registry_;
};

void testDoubleScheduleAndDeadTargetsAreRefused() {
    Registry reg;

    // (a) The same LIVE id destroyed twice. Without the already-scheduled
    //     refusal the subtree lands on the kill list twice and drain frees the
    //     slot twice: the live count underflows and the generation skips one,
    //     which leaves a handle naming a node that never existed.
    const NodeId twice = reg.createContainer(INVALID_NODE);
    const uint32_t slot = twice.index();
    const uint32_t generation = twice.generation();
    reg.destroy(twice);
    reg.destroy(twice);
    reg.drain();
    CHECK(reg.liveCount() == 0, "a second destroy of a scheduled node does not free its slot twice");

    const NodeId reissued = reg.createContainer(INVALID_NODE);
    CHECK(reissued.index() == slot && reissued.generation() == generation + 1,
          "nor bump its generation twice, which would skip a handle");
    reg.destroy(reissued);
    reg.drain();

    // (b) A destructor naming a node this same drain pass has already freed.
    //     destroy() is the only writer of the kill list, so its own liveness
    //     test is all that keeps a freed slot off it and out of a second free.
    const NodeId owner = reg.createContainer(INVALID_NODE);
    const NodeId earlier = reg.emplace<DestroyingProbe>(owner);
    const NodeId later = reg.emplace<DestroyingProbe>(owner);
    reg.as<DestroyingProbe>(later)->target = earlier;
    CHECK(reg.liveCount() == 3, "three nodes before the destroy");

    reg.destroy(owner);
    reg.drain();
    CHECK(reg.liveCount() == 0,
          "a destructor naming an already-freed sibling frees nothing a second time");
}

// 7. A node already scheduled for destruction cannot be linked back into the
//    live tree. destroy() unlinks it now and drain() frees it WITHOUT
//    unlinking, so a parent that acquired it in between would be left naming a
//    recycled slot -- and every walk over that parent would follow the link.
void testAScheduledNodeCannotRejoinTheTree() {
    Registry reg;

    const NodeId root = reg.createContainer(INVALID_NODE);
    reg[root].name = "Root";
    reg[root].size = glm::vec2(1.0f);

    const NodeId doomed = reg.createContainer(INVALID_NODE);
    reg[doomed].name = "Doomed";

    reg.destroy(doomed);
    CHECK(reg.alive(doomed) && !reg.attachable(doomed),
          "a scheduled node is alive until drain but is no longer attachable");

    reg.reparent(doomed, root);
    CHECK(reg.children(root).empty(), "reparent refuses to link a scheduled node into the tree");
    CHECK(!reg.parentOf(doomed).valid(), "and leaves it where destroy left it: unlinked");

    reg.reparentKeepingWorld(doomed, root);
    CHECK(reg.children(root).empty(), "the world-keeping variant refuses it too");

    CHECK(!reg.raiseChild(root, doomed, -1.0f, 1.0f), "and it cannot be raised among children it has left");

    reg.drain();
    CHECK(!reg.alive(doomed), "drain frees it");
    CHECK(reg.children(root).empty(), "and the parent it was refused names nothing");

    // The walks are the reason any of this matters: both follow first_child.
    const Nova::Math::Ray3D ray(glm::vec3(0.0f, 0.0f, 3.0f), glm::vec3(0.0f, 0.0f, -1.0f));
    Nova::Math::RayHit hit;
    NodeId struck;
    CHECK(Splash::hitTest(reg, root, ray, hit, struck) && struck == root,
          "hitTest walks the surviving tree and touches no freed slot");
    CHECK(reg.liveCount() == 1, "one node survives, and the freed one was freed exactly once");
}

// 7b. Nor can a node be created under one. Enrolling a new node in a subtree
//     that is already on its way out would hand the caller a handle that dies
//     at a drain it never asked for.
void testANodeCannotBeCreatedUnderAScheduledParent() {
    Registry reg;

    const NodeId parent = reg.createContainer(INVALID_NODE);
    reg.destroy(parent);

    const NodeId refused = reg.createContainer(parent);
    CHECK(!refused.valid(), "creation under a scheduled parent is refused, not silently doomed");
    CHECK(reg.liveCount() == 1, "and spends no slot on the node it refused");

    reg.drain();
    const NodeId dead_parent = parent;
    CHECK(!reg.alive(dead_parent), "the parent is gone");
    CHECK(!reg.createContainer(dead_parent).valid(), "and creation under a dead id is refused as well");
    CHECK(reg.liveCount() == 0, "leaving nothing behind either way");
}

// 8. Structural mutation is refused for the length of a render traversal: a
//    collect hook that reparents or destroys is editing the list the walk is
//    standing in.
void testStructuralMutationIsRefusedDuringATraversal() {
    Registry reg;

    const NodeId root = reg.createContainer(INVALID_NODE);
    const NodeId first = reg.createContainer(root);
    const NodeId second = reg.createContainer(root);
    reg.transform(second).position = glm::vec3(0.25f, 0.0f, 0.0f);
    reg.worldOf(second);
    CHECK(reg.worldIsCached(second), "the world cache is populated before the walk starts");

    {
        Registry::TraversalScope traversal(reg);
        reg.destroy(first);
        reg.reparent(second, INVALID_NODE);
        reg.reparentKeepingWorld(second, root);
        CHECK(!reg.createContainer(root).valid(), "no node may be created while the tree is walked");
        CHECK(!reg.raiseChild(root, second, -1.0f, 1.0f), "no sibling may be restacked");
        reg.drain();
    }

    CHECK(reg.children(root).size() == 2, "the walk ended with the tree it started with");
    CHECK(reg.parentOf(first) == root && reg.parentOf(second) == root, "both children kept their parent");
    CHECK(reg.alive(first) && reg.attachable(first), "nothing was scheduled or freed under the walk");
    CHECK(reg.liveCount() == 3, "and nothing was added");
    CHECK(reg.worldIsCached(second),
          "and no transform was written through, which would have invalidated the cache mid-walk");
}

// 9. A scheduled PARENT refuses the raise too. destroy() unlinks the subtree
//    root and nothing below it, so every node inside a doomed subtree still
//    names its parent and the membership test alone would let the raise
//    through - writing z to nodes that are on their way out.
void testRaiseChildRefusesAScheduledParent() {
    Registry reg;

    const NodeId parent = reg.createContainer(INVALID_NODE);
    const NodeId first = reg.createContainer(parent);
    const NodeId second = reg.createContainer(parent);
    reg.transform(first).position.z = 0.5f;
    reg.transform(second).position.z = 0.25f;

    reg.destroy(parent);
    CHECK(reg.parentOf(first) == parent,
          "destroy unlinks the subtree root and leaves the links inside it standing");

    CHECK(!reg.raiseChild(parent, first, -1.0f, 1.0f),
          "no sibling may be restacked inside a subtree already scheduled for destruction");
    CHECK_NEAR(reg[first].transform().position.z, 0.5f, 1e-9,
               "the refused raise wrote no front_z");
    CHECK_NEAR(reg[second].transform().position.z, 0.25f, 1e-9,
               "and no back_z to the siblings it would have pushed behind");
    CHECK(reg.lastChild(parent) == second, "and reordered nothing");
}

// 10. reparentKeepingWorld's own gate, in the one case the inner reparent
//     refusal and the membership early-out do not already cover: a scheduled
//     child asked to keep its world under the parent it is ALREADY under. The
//     relink is a no-op, so control would fall through to the transform()
//     write - a cache invalidation on a node that is on its way out.
void testWorldKeepingReparentDoesNotRewriteAScheduledNode() {
    Registry reg;

    const NodeId parent = reg.createContainer(INVALID_NODE);
    reg.transform(parent).position = glm::vec3(1.0f, 0.0f, 0.0f);
    const NodeId child = reg.createContainer(parent);
    reg.transform(child).position = glm::vec3(0.25f, 0.5f, 0.0f);

    reg.destroy(parent);
    CHECK(reg.parentOf(child) == parent && !reg.attachable(child),
          "the child is scheduled with the subtree, and still names its parent");

    // After the destroy, because destroy() dirties the subtree it takes: the
    // claim under test is that reparentKeepingWorld leaves the cache alone, so
    // the cache has to be warm at the moment it is called.
    reg.worldOf(child);
    CHECK(reg.worldIsCached(child), "the child's world is memoized before the call");

    reg.reparentKeepingWorld(child, parent);
    CHECK_NEAR(reg[child].transform().position.x, 0.25f, 1e-9,
               "a scheduled node's local transform is not rewritten");
    CHECK_NEAR(reg[child].transform().position.y, 0.5f, 1e-9, "on either axis");
    CHECK(reg.worldIsCached(child),
          "and no transform() write went through it, which would have dropped its cached world");

    reg.drain();
    CHECK(reg.liveCount() == 0, "and the subtree still goes at the next drain");
}

} // namespace

int main() {
    std::cout << "\n==========================================================================" << std::endl;
    std::cout << " [REGISTRY LIFETIME]: Refusals that keep a scheduled node out of the tree" << std::endl;
    std::cout << "==========================================================================" << std::endl;

    testDoubleScheduleAndDeadTargetsAreRefused();
    testAScheduledNodeCannotRejoinTheTree();
    testANodeCannotBeCreatedUnderAScheduledParent();
    testStructuralMutationIsRefusedDuringATraversal();
    testRaiseChildRefusesAScheduledParent();
    testWorldKeepingReparentDoesNotRewriteAScheduledNode();

    if (g_failures == 0) {
        std::cout << " [REGISTRY LIFETIME STATUS] Scheduled-node refusals PASSED with zero failures." << std::endl;
        return 0;
    }
    std::cout << " [REGISTRY LIFETIME STATUS] Scheduled-node refusals FAILED with "
              << g_failures << " failure(s)." << std::endl;
    return 1;
}
