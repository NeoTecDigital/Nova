// Written by Richard Christopher, Copyright 2026 NeoTec Digital
//
// Scene input routing: nearest-hit resolution, scoped subtree capture, the
// implicit pointer grab, and the pointer/keyboard focus split.
//
// GPU-free by construction. SpatialScene's constructor only allocates a root
// node in the registry it is handed; every Vulkan object it owns is created in
// initialize() and used in render(), neither of which is called here, and its
// destructor's vkDeviceWaitIdle is guarded on a non-null core. So
// SpatialScene(registry, nullptr, nullptr) is a complete, exercisable scene
// graph with no device behind it. UIWindow is likewise built with a null font,
// which is the supported path its own setupChrome() already guards for.

#include "Splash/Registry.h"
#include "Splash/SpatialScene.h"
#include "Clouds/ui/UIWindow.h"
#include "Clouds/ui/UIComponents.h"
#include "Clouds/ui/UIDockBar.h"
#include "Clouds/ui/UIMenuBar.h"

#include <cmath>
#include <cstdio>
#include <iostream>
#include <memory>
#include <string>
#include <vector>

// ---------------------------------------------------------------------------
// Assertion harness. Deliberately NOT <cassert>: assert() is compiled out under
// -DNDEBUG, which would silently turn every check below into a no-op.
// ---------------------------------------------------------------------------
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
using Clouds::UI::makeUIButton;
using Clouds::UI::makeUIDockBar;
using Clouds::UI::makeUILabel;
using Clouds::UI::makeUIMenuBar;
using Clouds::UI::makeUIWindow;
using Clouds::UI::UIButton;
using Clouds::UI::UIDockBar;
using Clouds::UI::UILabel;
using Clouds::UI::UIMenuBar;
using Clouds::UI::UITheme;
using Clouds::UI::UIWindow;
using Clouds::UI::TextRole;
using Clouds::UI::TextTone;

constexpr float kScreenW = 1600.0f;
constexpr float kScreenH = 1000.0f;
const glm::vec2 kScreen{kScreenW, kScreenH};

// Records which hooks fired, so a routing claim can be checked at the node the
// event was supposed to reach rather than only at the scene's bookkeeping.
class ProbeNode : public SpatialNode {
public:
    ProbeNode(Registry& reg, NodeId self) : SpatialNode(reg, self) {}

    int enters = 0;
    int leaves = 0;
    int moves = 0;
    int presses = 0;
    int releases = 0;
    int keys = 0;

    void onRayEnter(Registry& reg, NodeId self, const Nova::Math::RayHit& hit) override {
        ++enters;
        SpatialNode::onRayEnter(reg, self, hit);
    }
    void onRayLeave(Registry& reg, NodeId self) override {
        ++leaves;
        SpatialNode::onRayLeave(reg, self);
    }
    void onRayMove(Registry& reg, NodeId self, const Nova::Math::RayHit& hit) override {
        ++moves;
        SpatialNode::onRayMove(reg, self, hit);
    }
    void onRayButton(Registry& reg, NodeId self, const Nova::Math::RayHit& hit,
                     uint32_t button, bool pressed) override {
        pressed ? ++presses : ++releases;
        SpatialNode::onRayButton(reg, self, hit, button, pressed);
    }
    void onKey(Registry& reg, NodeId self, uint32_t key, bool pressed) override {
        if (pressed) ++keys;
        SpatialNode::onKey(reg, self, key, pressed);
    }
};

NodeId makeQuad(Registry& reg, NodeId parent, const std::string& name,
                const glm::vec3& pos, const glm::vec2& extent) {
    const NodeId node = reg.createContainer(parent);
    reg[node].name = name;
    reg[node].size = extent;
    reg.transform(node).position = pos;
    return node;
}

NodeId makeProbe(Registry& reg, NodeId parent, const std::string& name,
                 const glm::vec3& pos, const glm::vec2& extent) {
    const NodeId node = reg.emplace<ProbeNode>(parent);
    reg[node].name = name;
    reg[node].size = extent;
    reg.transform(node).position = pos;
    return node;
}

// Forward map of the scene's own camera, so a test can aim at a world point
// instead of guessing pixels. Exact inverse of unprojectScreenRay's pixel->NDC
// step, which is why the round trip does not depend on the Vulkan Y flip.
glm::vec2 screenPixelFor(const SpatialScene& scene, const glm::vec3& world_point) {
    const glm::mat4 view_proj = scene.getProjectionMatrix(kScreenW / kScreenH) * scene.getViewMatrix();
    const glm::vec4 clip = view_proj * glm::vec4(world_point, 1.0f);
    const glm::vec3 ndc = glm::vec3(clip) / clip.w;
    return glm::vec2((ndc.x * 0.5f + 0.5f) * kScreenW, (ndc.y * 0.5f + 0.5f) * kScreenH);
}

void aimAt(SpatialScene& scene, const glm::vec3& world_point) {
    scene.processPointerMotion(screenPixelFor(scene, world_point), kScreen);
}

// Sub-pixel Halton jitter is correct for rendering and useless for a test that
// needs to know exactly where it aimed.
std::unique_ptr<SpatialScene> makeScene(Registry& reg) {
    auto scene = std::make_unique<SpatialScene>(reg, nullptr, nullptr);
    scene->physics_config.dither_enabled = false;
    return scene;
}

const char* nodeName(Registry& reg, NodeId node) {
    const SpatialNode* object = reg.get(node);
    return object ? object->name.c_str() : "<none>";
}

// ---------------------------------------------------------------------------
// 1. Nearest hit, not first in list, and not "any descendant beats the parent".
// ---------------------------------------------------------------------------
void testNearestBeatsListOrder() {
    Registry reg;
    const NodeId root = reg.createContainer(INVALID_NODE);

    // The NEAR quad is added first on purpose: reverse-list-order-first-wins
    // returns the far one here, so this fails loudly on a regression.
    const NodeId near_quad = makeQuad(reg, root, "Near", glm::vec3(0.0f, 0.0f, 0.5f), glm::vec2(1.0f));
    makeQuad(reg, root, "Far", glm::vec3(0.0f, 0.0f, -0.5f), glm::vec2(1.0f));

    const Nova::Math::Ray3D ray(glm::vec3(0.0f, 0.0f, 3.0f), glm::vec3(0.0f, 0.0f, -1.0f));
    Nova::Math::RayHit hit;
    NodeId node;

    CHECK(Splash::hitTest(reg, root, ray, hit, node), "two overlapping quads must produce a hit");
    CHECK(node == near_quad, "the nearer quad wins regardless of child list order");
    CHECK_NEAR(hit.distance, 2.5f, 1e-4, "reported distance is the near quad's");
    fprintf(stdout, "  nearest-of-two -> %s at %.4f\n", nodeName(reg, node), static_cast<double>(hit.distance));
}

void testParentIsNotShadowedByItsChild() {
    Registry reg;
    const NodeId parent = makeQuad(reg, INVALID_NODE, "Parent", glm::vec3(0.0f, 0.0f, 0.6f), glm::vec2(1.0f));
    const NodeId child = makeQuad(reg, parent, "Child", glm::vec3(0.0f, 0.0f, -0.4f), glm::vec2(1.0f)); // world z = 0.2

    const Nova::Math::Ray3D ray(glm::vec3(0.0f, 0.0f, 3.0f), glm::vec3(0.0f, 0.0f, -1.0f));
    Nova::Math::RayHit hit;
    NodeId node;

    CHECK(Splash::hitTest(reg, parent, ray, hit, node), "parent subtree must produce a hit");
    CHECK(node == parent, "a deeper node behind the parent must not shadow it");

    // ...and the child still wins when it is genuinely in front.
    reg.transform(child).position.z = 0.4f; // world z = 1.0
    Nova::Math::RayHit front_hit;
    NodeId front_node;
    CHECK(Splash::hitTest(reg, parent, ray, front_hit, front_node), "parent subtree must still hit");
    CHECK(front_node == child, "a child in front of its parent wins on distance");
}

// ---------------------------------------------------------------------------
// 2. Constructor-assembled subtrees are parented. Everything below depends on
//    it: an unparented child has a world transform that ignores its owner, so
//    it draws in the wrong place and is hit in the wrong place.
// ---------------------------------------------------------------------------
void testConstructorWiredChildrenAreParented() {
    Registry reg;
    auto scene = makeScene(reg);
    const UITheme theme;
    const NodeId window = makeUIWindow(reg, scene->root, theme, "Parented", glm::vec2(0.6f, 0.4f), nullptr);

    const std::vector<NodeId> chrome = reg.children(window);
    CHECK(!chrome.empty(), "the window built its chrome");
    for (const NodeId piece : chrome) {
        CHECK(reg.parentOf(piece) == window, "chrome built in the constructor is parented to the window");
    }

    reg.transform(window).position = glm::vec3(1.0f, 0.0f, 0.0f);
    for (const NodeId piece : chrome) {
        CHECK_NEAR(reg.worldOf(piece).position.x, 1.0f, 1e-4,
                   "chrome follows the window it belongs to");
    }
}

// ---------------------------------------------------------------------------
// 3. Scoped capture, both directions.
// ---------------------------------------------------------------------------
void testTitlebarDragReachesTheWindow() {
    Registry reg;
    auto scene = makeScene(reg);
    const UITheme theme;
    const NodeId window = makeUIWindow(reg, scene->root, theme, "Drag Me", glm::vec2(0.6f, 0.4f), nullptr);

    const float tb_h = theme.titlebar_height;
    const float tb_centre_y = reg.as<UIWindow>(window)->window_size.y * 0.5f - tb_h * 0.5f;

    // Hover the titlebar: geometry lands on the titlebar panel, routing must not.
    aimAt(*scene, glm::vec3(0.0f, tb_centre_y, 0.0f));
    CHECK(scene->getPointerFocus() == window, "a hit on window chrome routes to the window");
    fprintf(stdout, "  titlebar hover -> %s\n", nodeName(reg, scene->getPointerFocus()));

    scene->processPointerButton(1, true);
    CHECK(scene->getPointerGrab() == window, "the press grabs the window");
    CHECK(scene->getKeyboardFocus() == window, "the press gives the window keyboard focus");

    // Drag 0.1 along +x. Both rays are aimed at the window's own plane, so the
    // expected delta is exact.
    aimAt(*scene, glm::vec3(0.1f, tb_centre_y, 0.0f));
    CHECK_NEAR(reg[window].transform().position.x, 0.1f, 1e-3, "titlebar drag moves the window");
    CHECK_NEAR(reg[window].transform().position.z, 0.0f, 1e-4, "an in-plane drag must not drift in depth");

    scene->processPointerButton(1, false);
    CHECK(!scene->getPointerGrab().valid(), "the release drops the grab");

    // The release reached the window, so further motion no longer drags it.
    aimAt(*scene, glm::vec3(0.25f, tb_centre_y, 0.0f));
    CHECK_NEAR(reg[window].transform().position.x, 0.1f, 1e-3, "motion after release must not keep dragging");
}

void testChildButtonStillGetsItsClick() {
    Registry reg;
    auto scene = makeScene(reg);
    const UITheme theme;
    const NodeId window = makeUIWindow(reg, scene->root, theme, "Closable", glm::vec2(0.6f, 0.4f), nullptr);
    const glm::vec2 window_size = reg.as<UIWindow>(window)->window_size;

    // Close button: titlebar-local (+w/2 - 0.045, 0, 0.003) under a titlebar at
    // (0, body_h/2, 0.003) in window space.
    const float body_h = window_size.y - theme.titlebar_height;
    const glm::vec3 close_at(window_size.x * 0.5f - 0.045f, body_h * 0.5f, 0.006f);

    aimAt(*scene, close_at);
    CHECK(scene->getPointerFocus() != window, "a claiming child is not swallowed by the capture");
    CHECK(scene->getPointerFocus().valid() && reg[scene->getPointerFocus()].name == "UIButton: x",
          "the close button is the pointer target");
    fprintf(stdout, "  close-button hover -> %s\n", nodeName(reg, scene->getPointerFocus()));

    scene->processPointerButton(1, true);
    scene->processPointerButton(1, false);
    CHECK(!reg[window].visible, "press+release on the close button ran its handler");
}

void testCaptureYieldsToAClaimingChild() {
    Registry reg;
    auto scene = makeScene(reg);

    const UITheme theme;
    const NodeId button = makeUIButton(reg, scene->root, theme, "Ok", glm::vec2(0.30f, 0.16f), nullptr, nullptr);
    int clicks = 0;
    reg.as<UIButton>(button)->on_click = [&clicks]() { ++clicks; };

    // Stand-in for the caption a font-backed button carries: a nearer child
    // covering the whole face. It is chrome, so it does not claim.
    const NodeId caption = makeUILabel(reg, button, theme, "Ok", nullptr);
    reg[caption].name = "ButtonCaption";
    reg.transform(caption).position = glm::vec3(0.0f, 0.0f, 0.002f);
    reg[caption].claims_pointer_input = false;

    aimAt(*scene, glm::vec3(0.0f, 0.0f, 0.0f));
    CHECK(scene->getPointerFocus() == button, "a non-claiming caption hands the hit to the button");
    scene->processPointerButton(1, true);
    scene->processPointerButton(1, false);
    CHECK(clicks == 1, "the button ran its click handler through its own caption");

    // Other direction: let the caption claim, and the capture must let it.
    reg[caption].claims_pointer_input = true;
    aimAt(*scene, glm::vec3(0.0f, 0.0f, 0.0f));
    CHECK(scene->getPointerFocus() == caption, "a claiming child keeps its own hits");
    scene->processPointerButton(1, true);
    scene->processPointerButton(1, false);
    CHECK(clicks == 1, "a claimed hit must not reach the capturing parent");
    fprintf(stdout, "  capture/claim both directions: clicks=%d\n", clicks);
}

// ---------------------------------------------------------------------------
// 4. Implicit grab.
// ---------------------------------------------------------------------------
void testGrabHoldsADragThatLeavesTheNode() {
    Registry reg;
    auto scene = makeScene(reg);
    const UITheme theme;
    const NodeId window = makeUIWindow(reg, scene->root, theme, "Runaway", glm::vec2(0.6f, 0.4f), nullptr);

    const float tb_centre_y =
        reg.as<UIWindow>(window)->window_size.y * 0.5f - theme.titlebar_height * 0.5f;
    aimAt(*scene, glm::vec3(0.0f, tb_centre_y, 0.0f));
    scene->processPointerButton(1, true);

    // Well clear of a 0.6 x 0.465 window: the ray now hits nothing at all.
    const glm::vec3 off_node(1.4f, tb_centre_y + 0.9f, 0.0f);
    {
        Nova::Math::RayHit probe;
        NodeId probe_node;
        const Nova::Math::Ray3D ray = Nova::Math::unprojectScreenRay(
            screenPixelFor(*scene, off_node), kScreen,
            glm::inverse(scene->getProjectionMatrix(kScreenW / kScreenH) * scene->getViewMatrix()));
        CHECK(!Splash::hitTest(reg, scene->root, ray, probe, probe_node),
              "the off-node aim point must miss the scene");
    }

    aimAt(*scene, off_node);
    CHECK(scene->getPointerGrab() == window, "the grab survives the pointer leaving the node");
    CHECK(scene->getPointerFocus() == window, "hover does not move while the pointer is grabbed");
    CHECK_NEAR(reg[window].transform().position.x, 1.4f, 1e-3, "the drag tracks the pointer off the quad");
    CHECK_NEAR(reg[window].transform().position.y, 0.9f, 1e-3, "the drag tracks the pointer off the quad");

    // The release must land on the pressed node even though nothing is hovered.
    scene->processPointerButton(1, false);
    CHECK(!scene->getPointerGrab().valid(), "the release ends the grab");

    const glm::vec3 after = reg[window].transform().position;
    aimAt(*scene, glm::vec3(0.2f, 0.2f, 0.0f));
    CHECK_NEAR(reg[window].transform().position.x, after.x, 1e-4, "the release stopped the drag");
    CHECK_NEAR(reg[window].transform().position.y, after.y, 1e-4, "the release stopped the drag");
    fprintf(stdout, "  grabbed drag ended at (%.3f, %.3f)\n",
            static_cast<double>(after.x), static_cast<double>(after.y));
}

void testGrabHoldsAcrossASecondButton() {
    Registry reg;
    auto scene = makeScene(reg);
    const NodeId probe = makeProbe(reg, scene->root, "Probe", glm::vec3(0.0f), glm::vec2(0.5f));

    aimAt(*scene, glm::vec3(0.0f, 0.0f, 0.0f));
    scene->processPointerButton(1, true);
    scene->processPointerButton(3, true);
    scene->processPointerButton(1, false);
    CHECK(scene->getPointerGrab() == probe, "one of two held buttons coming up keeps the grab");
    scene->processPointerButton(3, false);
    CHECK(!scene->getPointerGrab().valid(), "the last button up ends the grab");

    const ProbeNode* object = reg.as<ProbeNode>(probe);
    CHECK(object->presses == 2 && object->releases == 2, "every button edge reached the grabbed node");
}

// ---------------------------------------------------------------------------
// 5b. Exactly one node carries is_focused, and a node leaving takes it with it.
// ---------------------------------------------------------------------------
void testFocusTransferClearsThePreviousHolder() {
    Registry reg;
    auto scene = makeScene(reg);

    const NodeId first = makeProbe(reg, scene->root, "First", glm::vec3(-0.5f, 0.0f, 0.0f), glm::vec2(0.4f));
    const NodeId second = makeProbe(reg, scene->root, "Second", glm::vec3(0.5f, 0.0f, 0.0f), glm::vec2(0.4f));

    aimAt(*scene, reg[first].transform().position);
    scene->processPointerButton(1, true);
    scene->processPointerButton(1, false);
    CHECK(reg[first].is_focused, "the clicked node is focused");

    aimAt(*scene, reg[second].transform().position);
    scene->processPointerButton(1, true);
    scene->processPointerButton(1, false);
    CHECK(reg[second].is_focused, "the newly clicked node takes focus");
    CHECK(!reg[first].is_focused, "the previous holder's is_focused was cleared by the transfer");
    CHECK(scene->getKeyboardFocus() == second, "keyboard focus names the same node");

    // A transfer driven by the compositor rather than by a click.
    scene->setKeyboardFocus(first);
    CHECK(reg[first].is_focused && !reg[second].is_focused,
          "setKeyboardFocus moves the flag, it does not add a second one");

    scene->setKeyboardFocus(INVALID_NODE);
    CHECK(!reg[first].is_focused && !reg[second].is_focused, "focus going nowhere leaves nothing flagged");
    CHECK(!scene->getKeyboardFocus().valid(), "and the scene agrees it holds no focus");
}

// ---------------------------------------------------------------------------
// 5c. A node the scene still names, removed from the tree, is fully released.
// ---------------------------------------------------------------------------
void testReleaseNodeDropsEveryReference() {
    Registry reg;
    auto scene = makeScene(reg);

    const NodeId doomed = makeProbe(reg, scene->root, "Doomed", glm::vec3(0.0f), glm::vec2(0.4f));

    aimAt(*scene, reg[doomed].transform().position);
    scene->processPointerButton(1, true);
    CHECK(scene->getPointerFocus() == doomed && scene->getKeyboardFocus() == doomed &&
          scene->getPointerGrab() == doomed, "the node is named by focus and by the grab");

    reg.reparent(doomed, INVALID_NODE);
    scene->releaseNode(doomed);

    CHECK(!scene->getPointerFocus().valid(), "releaseNode drops pointer focus");
    CHECK(!scene->getKeyboardFocus().valid(), "releaseNode drops keyboard focus");
    CHECK(!scene->getPointerGrab().valid(), "releaseNode drops the implicit grab");
    CHECK(!reg[doomed].is_focused, "and clears the flag it left behind");
    CHECK(reg.as<ProbeNode>(doomed)->leaves == 1, "the node still got its leave edge on the way out");

    // The claim the retired use_count() check made -- the scene keeps nothing
    // alive -- now reads off the registry: destroying the node frees it outright
    // and no id the scene is holding survives it.
    reg.destroy(doomed);
    reg.drain();
    CHECK(!reg.alive(doomed) && !scene->getPointerFocus().valid() &&
          !scene->getKeyboardFocus().valid() && !scene->getPointerGrab().valid(),
          "no reference to the released node survives in the scene");
}

// ---------------------------------------------------------------------------
// 5. Pointer focus and keyboard focus are separate states.
// ---------------------------------------------------------------------------
void testKeyboardFocusSurvivesHoverMovingAway() {
    Registry reg;
    auto scene = makeScene(reg);

    const NodeId typed_at = makeProbe(reg, scene->root, "TypedAt", glm::vec3(-0.5f, 0.0f, 0.0f), glm::vec2(0.4f));
    const NodeId hovered = makeProbe(reg, scene->root, "Hovered", glm::vec3(0.5f, 0.0f, 0.0f), glm::vec2(0.4f));

    aimAt(*scene, reg[typed_at].transform().position);
    scene->processPointerButton(1, true);
    scene->processPointerButton(1, false);
    CHECK(scene->getKeyboardFocus() == typed_at, "activation sets keyboard focus");

    aimAt(*scene, reg[hovered].transform().position);
    CHECK(scene->getPointerFocus() == hovered, "pointer focus follows the pointer");
    CHECK(scene->getKeyboardFocus() == typed_at, "keyboard focus does not follow the pointer");

    scene->processKey(65, true);
    scene->processKey(65, false);
    const ProbeNode* typed_object = reg.as<ProbeNode>(typed_at);
    const ProbeNode* hovered_object = reg.as<ProbeNode>(hovered);
    CHECK(typed_object->keys == 1, "keys go to the keyboard-focused node");
    CHECK(hovered_object->keys == 0, "keys do not go to the merely hovered node");
    CHECK(hovered_object->enters == 1 && typed_object->leaves == 1, "hover transitions still fire");
    fprintf(stdout, "  keyboard focus -> %s, pointer focus -> %s\n",
            nodeName(reg, scene->getKeyboardFocus()), nodeName(reg, scene->getPointerFocus()));
}

// ---------------------------------------------------------------------------
// 6. is_focused has exactly one storage location.
// ---------------------------------------------------------------------------
void testIsFocusedIsSingleSourced() {
    Registry reg;
    auto scene = makeScene(reg);
    const UITheme theme;
    const NodeId window = makeUIWindow(reg, scene->root, theme, "Focus", glm::vec2(0.6f, 0.4f), nullptr);

    UIWindow* derived = reg.as<UIWindow>(window);
    const SpatialNode* base = derived;
    CHECK(static_cast<const void*>(&derived->is_focused) == static_cast<const void*>(&base->is_focused),
          "UIWindow::is_focused must BE SpatialNode::is_focused, not shadow it");

    derived->setFocused(false);
    CHECK(!base->is_focused, "setFocused(false) clears the base flag");

    // Written by SpatialNode::onRayButton, read back through the setter's member.
    aimAt(*scene, glm::vec3(0.0f, 0.0f, 0.0f));
    scene->processPointerButton(1, true);
    CHECK(derived->is_focused, "onRayButton focuses the window");
    CHECK(base->is_focused, "and the base flag agrees, because there is only one");
    scene->processPointerButton(1, false);

    derived->setFocused(false);
    CHECK(!base->is_focused && !derived->is_focused, "the two spellings never disagree");
}

// ---------------------------------------------------------------------------
// 7. Sibling stacking. Registry::raiseChild is the z/paint-order mechanism
//    mined out of WindowManager::bringToFront when that class was deleted in
//    A0 (its prior form is in commit 70673fb). Nothing else in the tree orders
//    siblings, so this is the whole coverage of it.
// ---------------------------------------------------------------------------
void testRaiseChildOrdersDepthAndPaintTogether() {
    Registry reg;
    const NodeId root = reg.createContainer(INVALID_NODE);
    const NodeId a = makeQuad(reg, root, "a", glm::vec3(0.0f), glm::vec2(1.0f));
    const NodeId b = makeQuad(reg, root, "b", glm::vec3(0.0f), glm::vec2(1.0f));
    const NodeId c = makeQuad(reg, root, "c", glm::vec3(0.0f), glm::vec2(1.0f));

    CHECK(reg.raiseChild(root, b, -0.04f, -0.12f), "raising an actual child reports success");
    CHECK_NEAR(reg[b].transform().position.z, -0.04f, 1e-6, "the raised child takes front_z");
    CHECK_NEAR(reg[a].transform().position.z, -0.12f, 1e-6, "every other child takes back_z");
    CHECK_NEAR(reg[c].transform().position.z, -0.12f, 1e-6, "including the one that was last");
    CHECK(reg.lastChild(root) == b, "and it moves last, which is painted on top");
    CHECK(reg.children(root).size() == 3, "raising neither adds nor drops a child");
    CHECK(reg.children(root)[0] == a && reg.children(root)[1] == c,
          "the siblings it passed keep their relative order");

    const NodeId stranger = makeQuad(reg, INVALID_NODE, "stranger", glm::vec3(0.0f), glm::vec2(1.0f));
    CHECK(!reg.raiseChild(root, stranger, 9.0f, -9.0f), "a node that is not a child cannot be raised");
    CHECK(!reg.raiseChild(root, INVALID_NODE, 9.0f, -9.0f), "nor can nothing");
    CHECK(reg.lastChild(root) == b, "a rejected raise reorders nothing");
    CHECK_NEAR(reg[a].transform().position.z, -0.12f, 1e-6, "and moves nothing");
}

void testRaiseChildDecidesTheHit() {
    Registry reg;
    auto scene = makeScene(reg);
    const NodeId lower = makeQuad(reg, scene->root, "lower", glm::vec3(0.0f), glm::vec2(1.0f));
    const NodeId upper = makeQuad(reg, scene->root, "upper", glm::vec3(0.0f), glm::vec2(1.0f));

    // Coplanar, in front of the root's own quad: distance ties between the two,
    // so only sibling order can settle it. This is the case the two halves of
    // raiseChild have to agree on, and the reason it writes both.
    CHECK(reg.raiseChild(scene->root, lower, 0.5f, 0.5f), "coplanar raise succeeds");
    aimAt(*scene, glm::vec3(0.0f, 0.0f, 0.5f));
    CHECK(scene->getPointerFocus() == lower, "on an exact depth tie the raised node takes the hit");

    CHECK(reg.raiseChild(scene->root, upper, 0.5f, 0.5f), "raising the other one");
    aimAt(*scene, glm::vec3(0.0f, 0.0f, 0.5f));
    CHECK(scene->getPointerFocus() == upper, "and the tie follows the raise");

    // Separated in depth: distance decides outright, and must land on the same
    // node paint order does.
    CHECK(reg.raiseChild(scene->root, lower, 0.6f, 0.4f), "separated raise succeeds");
    aimAt(*scene, glm::vec3(0.0f, 0.0f, 0.6f));
    CHECK(scene->getPointerFocus() == lower, "the nearer node takes the hit");
    CHECK(reg.lastChild(scene->root) == lower, "and is also the one painted last");
    fprintf(stdout, "  raise -> hit %s, painted last %s\n",
            nodeName(reg, scene->getPointerFocus()), nodeName(reg, reg.lastChild(scene->root)));
}

// ---------------------------------------------------------------------------
// 8. Anchored flow layout. UIMenuBar and UIDockBar are the only code in the
//    tree that packs widgets left-to-right at text-measured widths, which is
//    the whole reason A0 kept them; nothing constructs them yet, so this is
//    what holds them up.
//
//    GPU-free: SpatialFont's constructor only starts FreeType, and a font that
//    was never loaded has an empty glyph table, so measureText falls back to a
//    fixed advance per character. Widths are therefore exact and the layout
//    invariants below are checked against the run itself, not against numbers
//    copied out of the implementation.
// ---------------------------------------------------------------------------
std::vector<NodeId> buttonsUnder(Registry& reg, NodeId parent) {
    std::vector<NodeId> found;
    for (const NodeId child : reg.children(parent)) {
        if (reg.as<UIButton>(child)) found.push_back(child);
    }
    return found;
}

void checkFlowRun(Registry& reg, const std::vector<NodeId>& run,
                  float left_inset, float gap, const char* what) {
    CHECK(run.size() >= 2, what);
    if (run.size() < 2) return;

    CHECK_NEAR(reg[run[0]].transform().position.x - reg[run[0]].size.x * 0.5f, left_inset, 1e-5,
               "the run starts at the bar's left inset");

    for (size_t i = 1; i < run.size(); ++i) {
        const float prev_right = reg[run[i - 1]].transform().position.x + reg[run[i - 1]].size.x * 0.5f;
        const float this_left = reg[run[i]].transform().position.x - reg[run[i]].size.x * 0.5f;
        CHECK_NEAR(this_left - prev_right, gap, 1e-5,
                   "each item is one gap past the previous one, never overlapping it");
    }
}

void testDockBarFlowsFromItsLeftInset() {
    Registry reg;
    auto font = std::make_shared<Splash::SpatialFont>(nullptr, nullptr);
    const float width = 2.20f;
    const UITheme theme;
    const NodeId dock = makeUIDockBar(reg, INVALID_NODE, theme, width, font);
    UIDockBar* dock_object = reg.as<UIDockBar>(dock);

    int clicks = 0;
    dock_object->addItem(reg, "A", [&clicks]() { ++clicks; });
    dock_object->addItem(reg, "a much longer item", [&clicks]() { ++clicks; });
    dock_object->addItem(reg, "C", [&clicks]() { ++clicks; });

    CHECK(!reg.children(dock).empty(), "the dock built its panel");
    const std::vector<NodeId> items = buttonsUnder(reg, reg.firstChild(dock));
    CHECK(items.size() == 3, "one button per added item");
    if (items.size() != 3) return;

    checkFlowRun(reg, items, -width * 0.5f + 0.05f, 0.015f, "the dock laid out a run");
    CHECK(reg[items[1]].size.x > reg[items[0]].size.x,
          "a longer label measures to a wider button, which is what makes this a flow");

    reg.as<UIButton>(items[2])->on_click();
    CHECK(clicks == 1, "the handler an item was added with is the one it carries");
}

// The bar carries a brand label and a telemetry label; the telemetry seed used
// to read "FPS: 60 | Mode: Quaternionic Spatial | Sync: Delta" -- a measurement
// nobody took, drawn as though it were live.
void checkMenuBarLabels(Registry& reg, UIMenuBar& bar, NodeId panel) {
    std::vector<UILabel*> labels;
    for (const NodeId child : reg.children(panel)) {
        if (UILabel* label = reg.as<UILabel>(child)) labels.push_back(label);
    }
    CHECK(labels.size() == 2, "the bar carries a brand label and a telemetry label");
    if (labels.size() != 2) return;
    CHECK(labels[1]->text.empty(), "telemetry says nothing until something measures it");
    bar.setTelemetryText(reg, "tick 7");
    CHECK(labels[1]->text == "tick 7", "and says exactly what it is given");
}

void testMenuBarFlowsAndOpensOneDropdownAtATime() {
    Registry reg;
    auto font = std::make_shared<Splash::SpatialFont>(nullptr, nullptr);
    const float width = 2.60f;
    const UITheme theme;
    const NodeId bar = makeUIMenuBar(reg, INVALID_NODE, theme, width, font);
    UIMenuBar* bar_object = reg.as<UIMenuBar>(bar);

    int fired = 0;
    bar_object->addMenu(reg, bar, "Windows", {{ "Reset", "R", [&fired]() { ++fired; } }});
    bar_object->addMenu(reg, bar, "Physics with a long name", {{ "Toggle", "", [&fired]() { ++fired; } }});

    CHECK(!reg.children(bar).empty(), "the bar built its panel");
    const NodeId panel = reg.firstChild(bar);
    const std::vector<NodeId> menus = buttonsUnder(reg, panel);
    CHECK(menus.size() == 2, "one button per menu");
    if (menus.size() != 2) return;

    checkFlowRun(reg, menus, -width * 0.5f + 0.22f, 0.01f, "the menu bar laid out a run");
    CHECK(reg[menus[1]].size.x > reg[menus[0]].size.x, "a longer menu title measures wider");

    // Dropdowns are the bar's own children, not the panel's, so they draw over
    // the buttons rather than under them.
    const std::vector<NodeId> bar_children = reg.children(bar);
    std::vector<NodeId> dropdowns(bar_children.begin() + 1, bar_children.end());
    CHECK(dropdowns.size() == 2, "one dropdown per menu");
    if (dropdowns.size() != 2) return;

    CHECK(!reg[dropdowns[0]].visible && !reg[dropdowns[1]].visible, "a fresh bar has nothing open");

    reg.as<UIButton>(menus[0])->on_click();
    CHECK(reg[dropdowns[0]].visible && !reg[dropdowns[1]].visible, "opening one opens exactly one");

    reg.as<UIButton>(menus[1])->on_click();
    CHECK(!reg[dropdowns[0]].visible && reg[dropdowns[1]].visible, "opening another closes the first");

    reg.as<UIButton>(menus[1])->on_click();
    CHECK(!reg[dropdowns[0]].visible && !reg[dropdowns[1]].visible, "clicking the open one closes it");

    bar_object->closeAllDropdowns(reg);
    CHECK(!reg[dropdowns[0]].visible && !reg[dropdowns[1]].visible, "and closing all is idempotent");

    checkMenuBarLabels(reg, *bar_object, panel);
}

// ---------------------------------------------------------------------------
// 9. The theme is read, never remembered.
//
//    A widget resolves colour, material and typography from the theme it was
//    handed, at the moment it is asked -- so an edit to a live theme reaches
//    widgets that are already in the scene. That used to be false: UIButton's
//    corner radius and border thickness were in-class initialisers reading a
//    global theme, and every label's colour and scale were constructor
//    arguments copied out of the same global, so a theme edit moved only the
//    nodes built after it. Five snapshot sites, and this is what holds the
//    ground they used to occupy.
//
//    Every accessor checked here is the one collectRender itself calls, so a
//    green run says the frame would be drawn with these values -- not that a
//    parallel path in the test agrees with itself. GPU-free for the same
//    reason the rest of this file is: nothing here records a frame.
// ---------------------------------------------------------------------------
// Two themes in one process, which is the point of per-Desktop theming: a
// widget built against the second must not see the first theme's edits.
void checkSecondThemeIsIndependent(Registry& reg, std::shared_ptr<Splash::SpatialFont> font,
                                   const UIButton& edited) {
    const UITheme untouched;
    UIButton* other = reg.as<UIButton>(
        makeUIButton(reg, INVALID_NODE, untouched, "Other", glm::vec2(0.30f, 0.10f), std::move(font)));
    CHECK_NEAR(other->resolvedCornerRadius(), untouched.radius_button, 1e-9,
               "a widget built against a second theme reads that one and only that one");
    CHECK(other->resolvedCornerRadius() != edited.resolvedCornerRadius(),
          "which is a claim with content only because the two themes now differ");
    fprintf(stdout, "  theme edit -> radius %.5f vs sibling theme %.5f\n",
            static_cast<double>(edited.resolvedCornerRadius()),
            static_cast<double>(other->resolvedCornerRadius()));
}

void testThemeEditsReachWidgetsThatAlreadyExist() {
    Registry reg;
    UITheme theme;
    auto font = std::make_shared<Splash::SpatialFont>(nullptr, nullptr);

    UIButton* button = reg.as<UIButton>(
        makeUIButton(reg, INVALID_NODE, theme, "Live", glm::vec2(0.30f, 0.10f), font));
    UILabel* label = reg.as<UILabel>(
        makeUILabel(reg, INVALID_NODE, theme, "Live", font, TextRole::SMALL, TextTone::MUTED));
    UIWindow* window = reg.as<UIWindow>(
        makeUIWindow(reg, INVALID_NODE, theme, "Live", glm::vec2(0.6f, 0.4f), font));
    window->setFocused(true);

    const float width_before = label->getDimensions().x;
    CHECK(width_before > 0.0f, "a fontless font still measures a fixed advance per character");

    // Every widget above already exists. Now move the theme under it.
    theme.radius_button = 0.077f;
    theme.border_button = 0.0099f;
    theme.primary = glm::vec4(0.11f, 0.22f, 0.33f, 0.44f);
    theme.text_muted = glm::vec4(0.55f, 0.66f, 0.77f, 0.88f);
    theme.scale_small = 0.00099f;
    theme.window_titlebar_active = glm::vec4(0.12f, 0.34f, 0.56f, 0.78f);

    CHECK_NEAR(button->resolvedCornerRadius(), theme.radius_button, 1e-9,
               "a live theme edit reaches the corner radius of a button that already exists");
    CHECK_NEAR(button->resolvedBorderThickness(), theme.border_button, 1e-9,
               "and its border thickness");
    CHECK(button->resolvedFillColor() == theme.primary,
          "and the fill its variant names");
    CHECK(label->resolvedColor() == theme.text_muted,
          "a label resolves the tone it carries, not a colour it was handed once");
    CHECK_NEAR(label->resolvedScale(), theme.scale_small, 1e-9,
               "and the scale its role names");
    CHECK(label->getDimensions().x > width_before,
          "so the metric the layout measures moves with the theme too");
    CHECK(window->resolvedTitlebarColor() == theme.window_titlebar_active,
          "and window chrome follows, focus state and all");

    checkSecondThemeIsIndependent(reg, font, *button);
}

} // namespace

int main() {
    std::cout << "\n==========================================================================" << std::endl;
    std::cout << " [SCENE INPUT]: Hit resolution, subtree capture, pointer grab, focus split" << std::endl;
    std::cout << "==========================================================================" << std::endl;

    testNearestBeatsListOrder();
    testParentIsNotShadowedByItsChild();
    testConstructorWiredChildrenAreParented();
    testTitlebarDragReachesTheWindow();
    testChildButtonStillGetsItsClick();
    testCaptureYieldsToAClaimingChild();
    testGrabHoldsADragThatLeavesTheNode();
    testGrabHoldsAcrossASecondButton();
    testKeyboardFocusSurvivesHoverMovingAway();
    testFocusTransferClearsThePreviousHolder();
    testReleaseNodeDropsEveryReference();
    testIsFocusedIsSingleSourced();
    testRaiseChildOrdersDepthAndPaintTogether();
    testRaiseChildDecidesTheHit();
    testDockBarFlowsFromItsLeftInset();
    testMenuBarFlowsAndOpensOneDropdownAtATime();
    testThemeEditsReachWidgetsThatAlreadyExist();

    if (g_failures == 0) {
        std::cout << " [SCENE INPUT STATUS] Input routing PASSED with zero failures." << std::endl;
        return 0;
    }
    std::cout << " [SCENE INPUT STATUS] Input routing FAILED with " << g_failures << " failure(s)." << std::endl;
    return 1;
}
