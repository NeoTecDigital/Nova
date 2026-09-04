// Written by Richard Christopher, Copyright 2026 NeoTec Digital
//
// The node store itself: slot reuse under generations, the wrap quarantine,
// both reparenting variants, world-cache invalidation, destruction deferred out
// of a live callback, the one-destructor-per-node guarantee drain owes, and the
// refusals that keep a scheduled node out of the live tree.
//
// GPU-free by construction. Splash::Registry touches no device at all, and a
// SpatialScene(registry, nullptr, nullptr) allocates a root node and nothing
// else: every Vulkan object it owns is built in initialize(), never called here.

#include "Splash/Registry.h"
#include "Splash/SpatialScene.h"

#include <cmath>
#include <cstdio>
#include <iostream>
#include <set>
#include <string>
#include <vector>

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
using Splash::SpatialScene;

constexpr float kScreenW = 1600.0f;
constexpr float kScreenH = 1000.0f;
const glm::vec2 kScreen{kScreenW, kScreenH};

// 1. A reused slot is a different node, and the handle that named the old one
//    resolves to nothing rather than to whatever moved in.
void testSlotReuseDoesNotAlias() {
    Registry reg;

    const NodeId first = reg.createContainer(INVALID_NODE);
    reg[first].name = "First";
    const uint32_t slot = first.index();

    reg.destroy(first);
    CHECK(reg.alive(first), "destroy alone does not free: the node lives until drain");
    reg.drain();
    CHECK(!reg.alive(first), "drain frees it");
    CHECK(reg.get(first) == nullptr, "and the stale handle resolves to nothing");

    const NodeId second = reg.createContainer(INVALID_NODE);
    reg[second].name = "Second";
    CHECK(second.index() == slot, "the freed slot is the one the next node takes");
    CHECK(second != first, "but the handle naming it is a different handle");
    CHECK(second.generation() == first.generation() + 1, "the generation is what makes it different");
    CHECK(reg.get(first) == nullptr, "the old handle still resolves to nothing, not to its successor");
    CHECK(reg.get(second) != nullptr && reg[second].name == "Second", "the new node is reachable");
    fprintf(stdout, "  slot %u reissued as generation %u\n", slot, second.generation());
}

// 2. A slot whose generations run out is retired, never wrapped: reissuing
//    generation 1 would make the oldest surviving handle to that index live.
void testGenerationWrapQuarantinesTheSlot() {
    Registry reg;

    NodeId current = reg.createContainer(INVALID_NODE);
    const uint32_t slot = current.index();

    std::set<uint32_t> issued;
    issued.insert(current.bits);

    bool same_slot_every_cycle = true;
    bool every_handle_unique = true;

    // Generation 1 is already spent; cycle the rest of them onto this one slot.
    for (uint32_t cycle = NodeId::FIRST_GENERATION; cycle < NodeId::MAX_GENERATION; ++cycle) {
        reg.destroy(current);
        reg.drain();
        current = reg.createContainer(INVALID_NODE);
        if (current.index() != slot) same_slot_every_cycle = false;
        if (!issued.insert(current.bits).second) every_handle_unique = false;
    }

    CHECK(same_slot_every_cycle, "a FIFO free list of one hands the same slot back every time");
    CHECK(every_handle_unique, "and no handle is ever issued twice");
    CHECK(current.generation() == NodeId::MAX_GENERATION, "the last usable generation is reached");
    CHECK(issued.size() == NodeId::MAX_GENERATION, "one node per generation, 4095 of them");

    reg.destroy(current);
    reg.drain();
    CHECK(reg.quarantinedCount() == 1, "the exhausted slot is quarantined");

    const NodeId after_wrap = reg.createContainer(INVALID_NODE);
    CHECK(after_wrap.index() != slot, "and is never reissued");
    CHECK(after_wrap.generation() == NodeId::FIRST_GENERATION, "the fresh slot starts at generation 1");
    fprintf(stdout, "  slot %u retired after %u generations\n", slot, NodeId::MAX_GENERATION);
}

// 3. Both reparenting variants, against a parent that rotates and scales, so
//    the world solve is exercised rather than a translation cancelling out.
void testReparentKeepsTheWorldItPromises() {
    Registry reg;

    const NodeId left = reg.createContainer(INVALID_NODE);
    reg.transform(left).position = glm::vec3(1.0f, 0.0f, 0.0f);

    const NodeId right = reg.createContainer(INVALID_NODE);
    reg.transform(right).position = glm::vec3(0.0f, 2.0f, 0.0f);
    reg.transform(right).orientation = glm::angleAxis(glm::radians(90.0f), glm::vec3(0.0f, 0.0f, 1.0f));
    reg.transform(right).scale = glm::vec3(2.0f);

    const NodeId child = reg.createContainer(left);
    reg.transform(child).position = glm::vec3(0.5f, 0.0f, 0.0f);
    CHECK_NEAR(reg.worldOf(child).position.x, 1.5f, 1e-5, "the child starts where its parent puts it");

    // reparent(): the LOCAL survives, so the node moves in the world.
    reg.reparent(child, right);
    CHECK(reg.parentOf(child) == right, "reparent relinks the child");
    CHECK(reg.children(left).empty(), "and unlinks it from the parent it left");
    CHECK_NEAR(reg[child].transform().position.x, 0.5f, 1e-6, "reparent keeps the local pose");
    CHECK_NEAR(reg.worldOf(child).position.x, 0.0f, 1e-5, "so the world pose follows the new parent");
    CHECK_NEAR(reg.worldOf(child).position.y, 3.0f, 1e-5, "through its rotation and its scale");

    // reparentKeepingWorld(): the WORLD survives, so the local is re-solved.
    const Nova::Math::QuatTransform before = reg.worldOf(child);
    reg.reparentKeepingWorld(child, left);
    CHECK(reg.parentOf(child) == left, "reparentKeepingWorld relinks the child too");
    const Nova::Math::QuatTransform after = reg.worldOf(child);
    CHECK_NEAR(after.position.x, before.position.x, 1e-5, "the world position is untouched");
    CHECK_NEAR(after.position.y, before.position.y, 1e-5, "on every axis");
    CHECK_NEAR(after.position.z, before.position.z, 1e-5, "including depth");
    CHECK_NEAR(after.scale.x, before.scale.x, 1e-5, "and the world scale with it");
    CHECK_NEAR(reg[child].transform().position.x, -1.0f, 1e-5, "which the local had to move to hold");
    CHECK_NEAR(reg[child].transform().position.y, 3.0f, 1e-5, "on both axes");

    // Detaching keeps nothing but the node itself.
    reg.reparentKeepingWorld(child, INVALID_NODE);
    CHECK(!reg.parentOf(child).valid(), "a detached child has no parent");
    CHECK_NEAR(reg.worldOf(child).position.y, before.position.y, 1e-5,
               "and a detached child that kept its world did not move");
}

// 4. A cached world is invalidated by an ancestor moving, however far above.
void testWorldCacheFollowsAGrandparent() {
    Registry reg;

    const NodeId grandparent = reg.createContainer(INVALID_NODE);
    const NodeId parent = reg.createContainer(grandparent);
    const NodeId child = reg.createContainer(parent);

    reg.transform(grandparent).position = glm::vec3(1.0f, 0.0f, 0.0f);
    reg.transform(parent).position = glm::vec3(0.0f, 1.0f, 0.0f);
    reg.transform(child).position = glm::vec3(0.0f, 0.0f, 1.0f);

    // Read it first: an invalidation test that never populated the cache is
    // testing nothing.
    CHECK_NEAR(reg.worldOf(child).position.x, 1.0f, 1e-6, "the grandchild's world accumulates the chain");
    CHECK_NEAR(reg.worldOf(child).position.y, 1.0f, 1e-6, "on every axis");
    CHECK_NEAR(reg.worldOf(child).position.z, 1.0f, 1e-6, "including its own");

    reg.transform(grandparent).position = glm::vec3(5.0f, 0.0f, 0.0f);
    CHECK_NEAR(reg.worldOf(child).position.x, 5.0f, 1e-6,
               "moving the grandparent invalidates the grandchild's cached world");
    CHECK_NEAR(reg.worldOf(parent).position.x, 5.0f, 1e-6, "and the parent's");

    // A sibling subtree that was never read must not be left holding a stale
    // clean flag either.
    const NodeId sibling = reg.createContainer(parent);
    reg.transform(sibling).position = glm::vec3(0.0f, 0.0f, -1.0f);
    reg.transform(grandparent).position = glm::vec3(7.0f, 0.0f, 0.0f);
    CHECK_NEAR(reg.worldOf(sibling).position.x, 7.0f, 1e-6, "a sibling reads the same fresh chain");
    CHECK_NEAR(reg.worldOf(sibling).position.z, -1.0f, 1e-6, "with its own local intact");
}

// 5. A node that destroys itself from inside its own input handler. The handler
//    must return into memory that is still its own, and the node must be out of
//    the tree from that instant.
class SelfDestroyingProbe : public SpatialNode {
public:
    SelfDestroyingProbe(Registry& reg, NodeId self) : SpatialNode(reg, self) {}

    int presses = 0;
    bool alive_after_own_destroy = false;

    void onRayButton(Registry& reg, NodeId self, const Nova::Math::RayHit& hit,
                     uint32_t button, bool pressed) override {
        ++presses;
        SpatialNode::onRayButton(reg, self, hit, button, pressed);
        if (!pressed) return;

        reg.destroy(self);
        // Written AFTER asking for its own destruction. A registry that freed
        // here would make this a write through a dangling `this`, which is the
        // whole reason destruction is deferred.
        alive_after_own_destroy = reg.alive(self);
    }
};

glm::vec2 screenPixelFor(const SpatialScene& scene, const glm::vec3& world_point) {
    const glm::mat4 view_proj = scene.getProjectionMatrix(kScreenW / kScreenH) * scene.getViewMatrix();
    const glm::vec4 clip = view_proj * glm::vec4(world_point, 1.0f);
    const glm::vec3 ndc = glm::vec3(clip) / clip.w;
    return glm::vec2((ndc.x * 0.5f + 0.5f) * kScreenW, (ndc.y * 0.5f + 0.5f) * kScreenH);
}

void aimAt(SpatialScene& scene, const glm::vec3& world_point) {
    scene.processPointerMotion(screenPixelFor(scene, world_point), kScreen);
}

void testDestroyInsideACallbackIsDeferred() {
    Registry reg;
    SpatialScene scene(reg, nullptr, nullptr);
    scene.physics_config.dither_enabled = false;

    const NodeId probe = reg.emplace<SelfDestroyingProbe>(scene.root);
    reg[probe].name = "SelfDestroyer";
    reg[probe].size = glm::vec2(0.5f);
    // A container, not a surface: its default 1x1 extent describes nothing, so
    // it must not be hit-tested. It is here to prove the whole subtree goes.
    const NodeId caption = reg.createContainer(probe);
    reg[caption].interactable = false;

    aimAt(scene, glm::vec3(0.0f, 0.0f, 0.0f));
    CHECK(scene.getPointerFocus() == probe, "the probe is the pointer target before the press");

    scene.processPointerButton(1, true);
    const SelfDestroyingProbe* probe_object = reg.as<SelfDestroyingProbe>(probe);
    CHECK(probe_object != nullptr && probe_object->presses == 1, "the handler ran exactly once");
    CHECK(probe_object != nullptr && probe_object->alive_after_own_destroy,
          "and its object was still its own to write to after it asked to be destroyed");
    CHECK(reg.alive(probe) && reg.alive(caption), "the subtree lives until the frame drains");
    CHECK(!reg.parentOf(probe).valid(), "but it is unlinked from its parent immediately");
    CHECK(reg.children(scene.root).empty(), "so the scene root no longer lists it");

    Nova::Math::RayHit hit;
    NodeId struck;
    const Nova::Math::Ray3D ray(glm::vec3(0.0f, 0.0f, 3.0f), glm::vec3(0.0f, 0.0f, -1.0f));
    CHECK(Splash::hitTest(reg, scene.root, ray, hit, struck) && struck == scene.root,
          "and the ray that used to land on it now reaches the scene root behind it");

    reg.drain();
    CHECK(!reg.alive(probe), "drain frees the node");
    CHECK(!reg.alive(caption), "and its whole subtree with it");
    CHECK(!reg.alive(scene.getPointerGrab()), "the grab the press took now names a dead node");
    CHECK(!reg.alive(scene.getKeyboardFocus()), "as does keyboard focus");
    CHECK(!reg.alive(scene.getPointerFocus()), "and pointer focus");

    // The scene has to keep running over the handles it is still holding.
    aimAt(scene, glm::vec3(0.0f, 0.0f, 0.0f));
    CHECK(scene.getPointerFocus() == scene.root,
          "the next sample drops the dead reference and re-resolves onto what is still in the tree");
    CHECK(!scene.getPointerGrab().valid(), "grab included");
    fprintf(stdout, "  self-destroying probe: %d press, freed at drain\n", 1);
}

// 6. drain() runs each node's destructor exactly once, and never runs one for a
//    slot it has already freed.
int g_probe_destructions = 0;

class CountingProbe : public SpatialNode {
public:
    CountingProbe(Registry& reg, NodeId self) : SpatialNode(reg, self) {}
    ~CountingProbe() override { ++g_probe_destructions; }
};

void testDrainDestroysEachNodeExactlyOnce() {
    Registry reg;
    g_probe_destructions = 0;

    const NodeId root = reg.emplace<CountingProbe>(INVALID_NODE);
    const NodeId branch = reg.emplace<CountingProbe>(root);
    reg.emplace<CountingProbe>(branch);
    reg.emplace<CountingProbe>(branch);
    // A plain container in the same subtree: it carries no counting destructor,
    // so the count proves the walk visited the counted nodes and only those.
    const NodeId plain = reg.createContainer(root);

    CHECK(reg.liveCount() == 5, "five nodes are live before the destroy");
    CHECK(g_probe_destructions == 0, "and none has been destroyed");

    reg.destroy(root);
    CHECK(g_probe_destructions == 0, "scheduling destroys nothing");

    reg.drain();
    CHECK(g_probe_destructions == 4, "drain ran one destructor per counted node");
    CHECK(!reg.alive(plain), "the uncounted container was freed by the same walk");
    CHECK(reg.liveCount() == 0, "and the registry holds nothing live");

    reg.drain();
    CHECK(g_probe_destructions == 4, "a second drain has nothing left to run");

    reg.destroy(root);
    reg.destroy(branch);
    reg.drain();
    CHECK(g_probe_destructions == 4, "destroying a dead id is a no-op, not a second free");
    CHECK(reg.liveCount() == 0, "and leaves the live count where it was");

    // Nested destroys of the same subtree must not schedule it twice either.
    g_probe_destructions = 0;
    const NodeId again = reg.emplace<CountingProbe>(INVALID_NODE);
    const NodeId inner = reg.emplace<CountingProbe>(again);
    reg.destroy(again);
    reg.destroy(inner);
    reg.drain();
    CHECK(g_probe_destructions == 2, "a subtree scheduled through two ids is still freed once");
    fprintf(stdout, "  drain destructions: %d\n", g_probe_destructions);
}

} // namespace

int main() {
    std::cout << "\n==========================================================================" << std::endl;
    std::cout << " [REGISTRY]: Slot reuse, generation wrap, reparenting, world cache, drain" << std::endl;
    std::cout << "==========================================================================" << std::endl;

    testSlotReuseDoesNotAlias();
    testGenerationWrapQuarantinesTheSlot();
    testReparentKeepsTheWorldItPromises();
    testWorldCacheFollowsAGrandparent();
    testDestroyInsideACallbackIsDeferred();
    testDrainDestroysEachNodeExactlyOnce();

    if (g_failures == 0) {
        std::cout << " [REGISTRY STATUS] Node store PASSED with zero failures." << std::endl;
        return 0;
    }
    std::cout << " [REGISTRY STATUS] Node store FAILED with " << g_failures << " failure(s)." << std::endl;
    return 1;
}
