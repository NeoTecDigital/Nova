// Written by Richard Christopher, Copyright 2026 NeoTec Digital
//
// Scene input routing: nearest-hit resolution, scoped subtree capture, the
// implicit pointer grab, and the pointer/keyboard focus split.
//
// GPU-free by construction. SpatialScene's constructor only builds a root
// node; every Vulkan object it owns is created in initialize() and used in
// render(), neither of which is called here, and its destructor's
// vkDeviceWaitIdle is guarded on a non-null core. So SpatialScene(nullptr,
// nullptr) is a complete, exercisable scene graph with no device behind it.
// UIWindow is likewise built with a null font, which is the supported path
// its own setupChrome() already guards for.

#include "include/Clouds/SpatialScene.h"
#include "include/Clouds/UI/UIWindow.h"
#include "include/Clouds/UI/UIComponents.h"
#include "include/Clouds/UI/UIDockBar.h"
#include "include/Clouds/UI/UIMenuBar.h"

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

using Splash::SpatialNode;
using Splash::SpatialScene;
using Clouds::UI::UIButton;
using Clouds::UI::UIDockBar;
using Clouds::UI::UILabel;
using Clouds::UI::UIMenuBar;
using Clouds::UI::UIWindow;
using Clouds::UI::g_Theme;

constexpr float kScreenW = 1600.0f;
constexpr float kScreenH = 1000.0f;
const glm::vec2 kScreen{kScreenW, kScreenH};

// Records which hooks fired, so a routing claim can be checked at the node the
// event was supposed to reach rather than only at the scene's bookkeeping.
class ProbeNode : public SpatialNode {
public:
    int enters = 0;
    int leaves = 0;
    int moves = 0;
    int presses = 0;
    int releases = 0;
    int keys = 0;

    void onRayEnter(const Nova::Math::RayHit& hit) override {
        ++enters;
        SpatialNode::onRayEnter(hit);
    }
    void onRayLeave() override {
        ++leaves;
        SpatialNode::onRayLeave();
    }
    void onRayMove(const Nova::Math::RayHit& hit) override {
        ++moves;
        SpatialNode::onRayMove(hit);
    }
    void onRayButton(const Nova::Math::RayHit& hit, uint32_t button, bool pressed) override {
        pressed ? ++presses : ++releases;
        SpatialNode::onRayButton(hit, button, pressed);
    }
    void onKey(uint32_t key, bool pressed) override {
        if (pressed) ++keys;
        SpatialNode::onKey(key, pressed);
    }
};

std::shared_ptr<SpatialNode> makeQuad(const std::string& name, const glm::vec3& pos, const glm::vec2& extent) {
    auto node = std::make_shared<SpatialNode>();
    node->name = name;
    node->size = extent;
    node->transform.position = pos;
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
std::unique_ptr<SpatialScene> makeScene() {
    auto scene = std::make_unique<SpatialScene>(nullptr, nullptr);
    scene->physics_config.dither_enabled = false;
    return scene;
}

const char* nodeName(const std::shared_ptr<SpatialNode>& node) {
    return node ? node->name.c_str() : "<none>";
}

// ---------------------------------------------------------------------------
// 1. Nearest hit, not first in list, and not "any descendant beats the parent".
// ---------------------------------------------------------------------------
void testNearestBeatsListOrder() {
    auto root = std::make_shared<SpatialNode>();

    // The NEAR quad is added first on purpose: reverse-list-order-first-wins
    // returns the far one here, so this fails loudly on a regression.
    auto near_quad = makeQuad("Near", glm::vec3(0.0f, 0.0f, 0.5f), glm::vec2(1.0f));
    auto far_quad = makeQuad("Far", glm::vec3(0.0f, 0.0f, -0.5f), glm::vec2(1.0f));
    root->addChild(near_quad);
    root->addChild(far_quad);

    const Nova::Math::Ray3D ray(glm::vec3(0.0f, 0.0f, 3.0f), glm::vec3(0.0f, 0.0f, -1.0f));
    Nova::Math::RayHit hit;
    std::shared_ptr<SpatialNode> node;

    CHECK(root->hitTest(ray, hit, node), "two overlapping quads must produce a hit");
    CHECK(node == near_quad, "the nearer quad wins regardless of child list order");
    CHECK_NEAR(hit.distance, 2.5f, 1e-4, "reported distance is the near quad's");
    fprintf(stdout, "  nearest-of-two -> %s at %.4f\n", nodeName(node), static_cast<double>(hit.distance));
}

void testParentIsNotShadowedByItsChild() {
    auto parent = makeQuad("Parent", glm::vec3(0.0f, 0.0f, 0.6f), glm::vec2(1.0f));
    auto child = makeQuad("Child", glm::vec3(0.0f, 0.0f, -0.4f), glm::vec2(1.0f)); // world z = 0.2
    parent->addChild(child);

    const Nova::Math::Ray3D ray(glm::vec3(0.0f, 0.0f, 3.0f), glm::vec3(0.0f, 0.0f, -1.0f));
    Nova::Math::RayHit hit;
    std::shared_ptr<SpatialNode> node;

    CHECK(parent->hitTest(ray, hit, node), "parent subtree must produce a hit");
    CHECK(node == parent, "a deeper node behind the parent must not shadow it");

    // ...and the child still wins when it is genuinely in front.
    child->transform.position.z = 0.4f; // world z = 1.0
    Nova::Math::RayHit front_hit;
    std::shared_ptr<SpatialNode> front_node;
    CHECK(parent->hitTest(ray, front_hit, front_node), "parent subtree must still hit");
    CHECK(front_node == child, "a child in front of its parent wins on distance");
}

// ---------------------------------------------------------------------------
// 2. Constructor-assembled subtrees are parented. Everything below depends on
//    it: an unparented child has a world transform that ignores its owner, so
//    it draws in the wrong place and is hit in the wrong place.
// ---------------------------------------------------------------------------
void testConstructorWiredChildrenAreParented() {
    auto scene = makeScene();
    auto window = std::make_shared<UIWindow>("Parented", glm::vec2(0.6f, 0.4f), nullptr);
    scene->root->addChild(window);

    CHECK(!window->children.empty(), "the window built its chrome");
    for (const std::shared_ptr<SpatialNode>& chrome : window->children) {
        CHECK(chrome->parent.lock() == window, "chrome built in the constructor is parented to the window");
    }

    window->transform.position = glm::vec3(1.0f, 0.0f, 0.0f);
    for (const std::shared_ptr<SpatialNode>& chrome : window->children) {
        CHECK_NEAR(chrome->getWorldTransform().position.x, 1.0f, 1e-4,
                   "chrome follows the window it belongs to");
    }
}

// ---------------------------------------------------------------------------
// 3. Scoped capture, both directions.
// ---------------------------------------------------------------------------
void testTitlebarDragReachesTheWindow() {
    auto scene = makeScene();
    auto window = std::make_shared<UIWindow>("Drag Me", glm::vec2(0.6f, 0.4f), nullptr);
    scene->root->addChild(window);

    const float tb_h = Clouds::UI::g_Theme.titlebar_height;
    const float tb_centre_y = window->window_size.y * 0.5f - tb_h * 0.5f;

    // Hover the titlebar: geometry lands on titlebar_panel_, routing must not.
    aimAt(*scene, glm::vec3(0.0f, tb_centre_y, 0.0f));
    CHECK(scene->getPointerFocus() == window, "a hit on window chrome routes to the window");
    fprintf(stdout, "  titlebar hover -> %s\n", nodeName(scene->getPointerFocus()));

    scene->processPointerButton(1, true);
    CHECK(scene->getPointerGrab() == window, "the press grabs the window");
    CHECK(scene->getKeyboardFocus() == window, "the press gives the window keyboard focus");

    // Drag 0.1 along +x. Both rays are aimed at the window's own plane, so the
    // expected delta is exact.
    aimAt(*scene, glm::vec3(0.1f, tb_centre_y, 0.0f));
    CHECK_NEAR(window->transform.position.x, 0.1f, 1e-3, "titlebar drag moves the window");
    CHECK_NEAR(window->transform.position.z, 0.0f, 1e-4, "an in-plane drag must not drift in depth");

    scene->processPointerButton(1, false);
    CHECK(!scene->getPointerGrab(), "the release drops the grab");

    // The release reached the window, so further motion no longer drags it.
    aimAt(*scene, glm::vec3(0.25f, tb_centre_y, 0.0f));
    CHECK_NEAR(window->transform.position.x, 0.1f, 1e-3, "motion after release must not keep dragging");
}

void testChildButtonStillGetsItsClick() {
    auto scene = makeScene();
    auto window = std::make_shared<UIWindow>("Closable", glm::vec2(0.6f, 0.4f), nullptr);
    scene->root->addChild(window);

    // Close button: titlebar-local (+w/2 - 0.045, 0, 0.003) under a titlebar at
    // (0, body_h/2, 0.003) in window space.
    const float body_h = window->window_size.y - Clouds::UI::g_Theme.titlebar_height;
    const glm::vec3 close_at(window->window_size.x * 0.5f - 0.045f, body_h * 0.5f, 0.006f);

    aimAt(*scene, close_at);
    CHECK(scene->getPointerFocus() != window, "a claiming child is not swallowed by the capture");
    CHECK(scene->getPointerFocus() && scene->getPointerFocus()->name == "UIButton: x",
          "the close button is the pointer target");
    fprintf(stdout, "  close-button hover -> %s\n", nodeName(scene->getPointerFocus()));

    scene->processPointerButton(1, true);
    scene->processPointerButton(1, false);
    CHECK(!window->visible, "press+release on the close button ran its handler");
}

void testCaptureYieldsToAClaimingChild() {
    auto scene = makeScene();

    auto button = std::make_shared<UIButton>("Ok", glm::vec2(0.30f, 0.16f), nullptr, nullptr);
    int clicks = 0;
    button->on_click = [&clicks]() { ++clicks; };
    scene->root->addChild(button);

    // Stand-in for the caption a font-backed button carries: a nearer child
    // covering the whole face. It is chrome, so it does not claim.
    auto caption = std::make_shared<UILabel>("Ok", nullptr);
    caption->name = "ButtonCaption";
    caption->transform.position = glm::vec3(0.0f, 0.0f, 0.002f);
    caption->claims_pointer_input = false;
    button->addChild(caption);

    aimAt(*scene, glm::vec3(0.0f, 0.0f, 0.0f));
    CHECK(scene->getPointerFocus() == button, "a non-claiming caption hands the hit to the button");
    scene->processPointerButton(1, true);
    scene->processPointerButton(1, false);
    CHECK(clicks == 1, "the button ran its click handler through its own caption");

    // Other direction: let the caption claim, and the capture must let it.
    caption->claims_pointer_input = true;
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
    auto scene = makeScene();
    auto window = std::make_shared<UIWindow>("Runaway", glm::vec2(0.6f, 0.4f), nullptr);
    scene->root->addChild(window);

    const float tb_centre_y = window->window_size.y * 0.5f - Clouds::UI::g_Theme.titlebar_height * 0.5f;
    aimAt(*scene, glm::vec3(0.0f, tb_centre_y, 0.0f));
    scene->processPointerButton(1, true);

    // Well clear of a 0.6 x 0.465 window: the ray now hits nothing at all.
    const glm::vec3 off_node(1.4f, tb_centre_y + 0.9f, 0.0f);
    {
        Nova::Math::RayHit probe;
        std::shared_ptr<SpatialNode> probe_node;
        const Nova::Math::Ray3D ray = Nova::Math::unprojectScreenRay(
            screenPixelFor(*scene, off_node), kScreen,
            glm::inverse(scene->getProjectionMatrix(kScreenW / kScreenH) * scene->getViewMatrix()));
        CHECK(!scene->root->hitTest(ray, probe, probe_node), "the off-node aim point must miss the scene");
    }

    aimAt(*scene, off_node);
    CHECK(scene->getPointerGrab() == window, "the grab survives the pointer leaving the node");
    CHECK(scene->getPointerFocus() == window, "hover does not move while the pointer is grabbed");
    CHECK_NEAR(window->transform.position.x, 1.4f, 1e-3, "the drag tracks the pointer off the quad");
    CHECK_NEAR(window->transform.position.y, 0.9f, 1e-3, "the drag tracks the pointer off the quad");

    // The release must land on the pressed node even though nothing is hovered.
    scene->processPointerButton(1, false);
    CHECK(!scene->getPointerGrab(), "the release ends the grab");

    const glm::vec3 after = window->transform.position;
    aimAt(*scene, glm::vec3(0.2f, 0.2f, 0.0f));
    CHECK_NEAR(window->transform.position.x, after.x, 1e-4, "the release stopped the drag");
    CHECK_NEAR(window->transform.position.y, after.y, 1e-4, "the release stopped the drag");
    fprintf(stdout, "  grabbed drag ended at (%.3f, %.3f)\n",
            static_cast<double>(after.x), static_cast<double>(after.y));
}

void testGrabHoldsAcrossASecondButton() {
    auto scene = makeScene();
    auto probe = std::make_shared<ProbeNode>();
    probe->name = "Probe";
    probe->size = glm::vec2(0.5f);
    scene->root->addChild(probe);

    aimAt(*scene, glm::vec3(0.0f, 0.0f, 0.0f));
    scene->processPointerButton(1, true);
    scene->processPointerButton(3, true);
    scene->processPointerButton(1, false);
    CHECK(scene->getPointerGrab() == probe, "one of two held buttons coming up keeps the grab");
    scene->processPointerButton(3, false);
    CHECK(!scene->getPointerGrab(), "the last button up ends the grab");
    CHECK(probe->presses == 2 && probe->releases == 2, "every button edge reached the grabbed node");
}

// ---------------------------------------------------------------------------
// 5b. Exactly one node carries is_focused, and a node leaving takes it with it.
// ---------------------------------------------------------------------------
void testFocusTransferClearsThePreviousHolder() {
    auto scene = makeScene();

    auto first = std::make_shared<ProbeNode>();
    first->name = "First";
    first->size = glm::vec2(0.4f);
    first->transform.position = glm::vec3(-0.5f, 0.0f, 0.0f);

    auto second = std::make_shared<ProbeNode>();
    second->name = "Second";
    second->size = glm::vec2(0.4f);
    second->transform.position = glm::vec3(0.5f, 0.0f, 0.0f);

    scene->root->addChild(first);
    scene->root->addChild(second);

    aimAt(*scene, first->transform.position);
    scene->processPointerButton(1, true);
    scene->processPointerButton(1, false);
    CHECK(first->is_focused, "the clicked node is focused");

    aimAt(*scene, second->transform.position);
    scene->processPointerButton(1, true);
    scene->processPointerButton(1, false);
    CHECK(second->is_focused, "the newly clicked node takes focus");
    CHECK(!first->is_focused, "the previous holder's is_focused was cleared by the transfer");
    CHECK(scene->getKeyboardFocus() == second, "keyboard focus names the same node");

    // A transfer driven by the compositor rather than by a click.
    scene->setKeyboardFocus(first);
    CHECK(first->is_focused && !second->is_focused,
          "setKeyboardFocus moves the flag, it does not add a second one");

    scene->setKeyboardFocus(nullptr);
    CHECK(!first->is_focused && !second->is_focused, "focus going nowhere leaves nothing flagged");
    CHECK(!scene->getKeyboardFocus(), "and the scene agrees it holds no focus");
}

// ---------------------------------------------------------------------------
// 5c. A node the scene still names, removed from the tree, is fully released.
// ---------------------------------------------------------------------------
void testReleaseNodeDropsEveryReference() {
    auto scene = makeScene();

    auto doomed = std::make_shared<ProbeNode>();
    doomed->name = "Doomed";
    doomed->size = glm::vec2(0.4f);
    scene->root->addChild(doomed);

    aimAt(*scene, doomed->transform.position);
    scene->processPointerButton(1, true);
    CHECK(scene->getPointerFocus() == doomed && scene->getKeyboardFocus() == doomed &&
          scene->getPointerGrab() == doomed, "the node is named by focus and by the grab");

    scene->root->removeChild(doomed);
    scene->releaseNode(doomed);

    CHECK(!scene->getPointerFocus(), "releaseNode drops pointer focus");
    CHECK(!scene->getKeyboardFocus(), "releaseNode drops keyboard focus");
    CHECK(!scene->getPointerGrab(), "releaseNode drops the implicit grab");
    CHECK(!doomed->is_focused, "and clears the flag it left behind");
    CHECK(doomed->leaves == 1, "the node still got its leave edge on the way out");
    CHECK(doomed.use_count() == 1, "no reference to the released node survives in the scene");
}

// ---------------------------------------------------------------------------
// 5. Pointer focus and keyboard focus are separate states.
// ---------------------------------------------------------------------------
void testKeyboardFocusSurvivesHoverMovingAway() {
    auto scene = makeScene();

    auto typed_at = std::make_shared<ProbeNode>();
    typed_at->name = "TypedAt";
    typed_at->size = glm::vec2(0.4f);
    typed_at->transform.position = glm::vec3(-0.5f, 0.0f, 0.0f);

    auto hovered = std::make_shared<ProbeNode>();
    hovered->name = "Hovered";
    hovered->size = glm::vec2(0.4f);
    hovered->transform.position = glm::vec3(0.5f, 0.0f, 0.0f);

    scene->root->addChild(typed_at);
    scene->root->addChild(hovered);

    aimAt(*scene, typed_at->transform.position);
    scene->processPointerButton(1, true);
    scene->processPointerButton(1, false);
    CHECK(scene->getKeyboardFocus() == typed_at, "activation sets keyboard focus");

    aimAt(*scene, hovered->transform.position);
    CHECK(scene->getPointerFocus() == hovered, "pointer focus follows the pointer");
    CHECK(scene->getKeyboardFocus() == typed_at, "keyboard focus does not follow the pointer");

    scene->processKey(65, true);
    scene->processKey(65, false);
    CHECK(typed_at->keys == 1, "keys go to the keyboard-focused node");
    CHECK(hovered->keys == 0, "keys do not go to the merely hovered node");
    CHECK(hovered->enters == 1 && typed_at->leaves == 1, "hover transitions still fire");
    fprintf(stdout, "  keyboard focus -> %s, pointer focus -> %s\n",
            nodeName(scene->getKeyboardFocus()), nodeName(scene->getPointerFocus()));
}

// ---------------------------------------------------------------------------
// 6. is_focused has exactly one storage location.
// ---------------------------------------------------------------------------
void testIsFocusedIsSingleSourced() {
    auto scene = makeScene();
    auto window = std::make_shared<UIWindow>("Focus", glm::vec2(0.6f, 0.4f), nullptr);
    scene->root->addChild(window);

    const SpatialNode* base = window.get();
    CHECK(static_cast<const void*>(&window->is_focused) == static_cast<const void*>(&base->is_focused),
          "UIWindow::is_focused must BE SpatialNode::is_focused, not shadow it");

    window->setFocused(false);
    CHECK(!base->is_focused, "setFocused(false) clears the base flag");

    // Written by SpatialNode::onRayButton, read back through the setter's member.
    aimAt(*scene, glm::vec3(0.0f, 0.0f, 0.0f));
    scene->processPointerButton(1, true);
    CHECK(window->is_focused, "onRayButton focuses the window");
    CHECK(base->is_focused, "and the base flag agrees, because there is only one");
    scene->processPointerButton(1, false);

    window->setFocused(false);
    CHECK(!base->is_focused && !window->is_focused, "the two spellings never disagree");
}

// ---------------------------------------------------------------------------
// 7. Sibling stacking. SpatialNode::raiseChild is the z/paint-order mechanism
//    mined out of WindowManager::bringToFront when that class was deleted in
//    A0 (its prior form is in commit 70673fb). Nothing else in the tree orders
//    siblings, so this is the whole coverage of it.
// ---------------------------------------------------------------------------
void testRaiseChildOrdersDepthAndPaintTogether() {
    auto root = std::make_shared<SpatialNode>();
    auto a = makeQuad("a", glm::vec3(0.0f), glm::vec2(1.0f));
    auto b = makeQuad("b", glm::vec3(0.0f), glm::vec2(1.0f));
    auto c = makeQuad("c", glm::vec3(0.0f), glm::vec2(1.0f));
    root->addChild(a);
    root->addChild(b);
    root->addChild(c);

    CHECK(root->raiseChild(b, -0.04f, -0.12f), "raising an actual child reports success");
    CHECK_NEAR(b->transform.position.z, -0.04f, 1e-6, "the raised child takes front_z");
    CHECK_NEAR(a->transform.position.z, -0.12f, 1e-6, "every other child takes back_z");
    CHECK_NEAR(c->transform.position.z, -0.12f, 1e-6, "including the one that was last");
    CHECK(root->children.back() == b, "and it moves last, which is painted on top");
    CHECK(root->children.size() == 3, "raising neither adds nor drops a child");
    CHECK(root->children[0] == a && root->children[1] == c,
          "the siblings it passed keep their relative order");

    CHECK(!root->raiseChild(makeQuad("stranger", glm::vec3(0.0f), glm::vec2(1.0f)), 9.0f, -9.0f),
          "a node that is not a child cannot be raised");
    CHECK(!root->raiseChild(nullptr, 9.0f, -9.0f), "nor can nothing");
    CHECK(root->children.back() == b, "a rejected raise reorders nothing");
    CHECK_NEAR(a->transform.position.z, -0.12f, 1e-6, "and moves nothing");
}

void testRaiseChildDecidesTheHit() {
    auto scene = makeScene();
    auto lower = makeQuad("lower", glm::vec3(0.0f), glm::vec2(1.0f));
    auto upper = makeQuad("upper", glm::vec3(0.0f), glm::vec2(1.0f));
    scene->root->addChild(lower);
    scene->root->addChild(upper);

    // Coplanar, in front of the root's own quad: distance ties between the two,
    // so only sibling order can settle it. This is the case the two halves of
    // raiseChild have to agree on, and the reason it writes both.
    CHECK(scene->root->raiseChild(lower, 0.5f, 0.5f), "coplanar raise succeeds");
    aimAt(*scene, glm::vec3(0.0f, 0.0f, 0.5f));
    CHECK(scene->getPointerFocus() == lower, "on an exact depth tie the raised node takes the hit");

    CHECK(scene->root->raiseChild(upper, 0.5f, 0.5f), "raising the other one");
    aimAt(*scene, glm::vec3(0.0f, 0.0f, 0.5f));
    CHECK(scene->getPointerFocus() == upper, "and the tie follows the raise");

    // Separated in depth: distance decides outright, and must land on the same
    // node paint order does.
    CHECK(scene->root->raiseChild(lower, 0.6f, 0.4f), "separated raise succeeds");
    aimAt(*scene, glm::vec3(0.0f, 0.0f, 0.6f));
    CHECK(scene->getPointerFocus() == lower, "the nearer node takes the hit");
    CHECK(scene->root->children.back() == lower, "and is also the one painted last");
    fprintf(stdout, "  raise -> hit %s, painted last %s\n",
            nodeName(scene->getPointerFocus()), nodeName(scene->root->children.back()));
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
std::vector<std::shared_ptr<UIButton>> buttonsUnder(const std::shared_ptr<SpatialNode>& parent) {
    std::vector<std::shared_ptr<UIButton>> found;
    for (const std::shared_ptr<SpatialNode>& child : parent->children) {
        if (auto button = std::dynamic_pointer_cast<UIButton>(child)) found.push_back(button);
    }
    return found;
}

void checkFlowRun(const std::vector<std::shared_ptr<UIButton>>& run,
                  float left_inset, float gap, const char* what) {
    CHECK(run.size() >= 2, what);
    if (run.size() < 2) return;

    CHECK_NEAR(run[0]->transform.position.x - run[0]->size.x * 0.5f, left_inset, 1e-5,
               "the run starts at the bar's left inset");

    for (size_t i = 1; i < run.size(); ++i) {
        const float prev_right = run[i - 1]->transform.position.x + run[i - 1]->size.x * 0.5f;
        const float this_left = run[i]->transform.position.x - run[i]->size.x * 0.5f;
        CHECK_NEAR(this_left - prev_right, gap, 1e-5,
                   "each item is one gap past the previous one, never overlapping it");
    }
}

void testDockBarFlowsFromItsLeftInset() {
    auto font = std::make_shared<Nova::SpatialFont>(nullptr, nullptr);
    const float width = 2.20f;
    auto dock = std::make_shared<UIDockBar>(width, font);

    int clicks = 0;
    dock->addItem("A", [&clicks]() { ++clicks; });
    dock->addItem("a much longer item", [&clicks]() { ++clicks; });
    dock->addItem("C", [&clicks]() { ++clicks; });

    CHECK(!dock->children.empty(), "the dock built its panel");
    const std::vector<std::shared_ptr<UIButton>> items = buttonsUnder(dock->children[0]);
    CHECK(items.size() == 3, "one button per added item");
    if (items.size() != 3) return;

    checkFlowRun(items, -width * 0.5f + 0.05f, 0.015f, "the dock laid out a run");
    CHECK(items[1]->size.x > items[0]->size.x,
          "a longer label measures to a wider button, which is what makes this a flow");

    items[2]->on_click();
    CHECK(clicks == 1, "the handler an item was added with is the one it carries");
}

void testMenuBarFlowsAndOpensOneDropdownAtATime() {
    auto font = std::make_shared<Nova::SpatialFont>(nullptr, nullptr);
    const float width = 2.60f;
    auto bar = std::make_shared<UIMenuBar>(width, font);

    int fired = 0;
    bar->addMenu("Windows", {{ "Reset", "R", [&fired]() { ++fired; } }});
    bar->addMenu("Physics with a long name", {{ "Toggle", "", [&fired]() { ++fired; } }});

    CHECK(!bar->children.empty(), "the bar built its panel");
    const std::vector<std::shared_ptr<UIButton>> menus = buttonsUnder(bar->children[0]);
    CHECK(menus.size() == 2, "one button per menu");
    if (menus.size() != 2) return;

    checkFlowRun(menus, -width * 0.5f + 0.22f, 0.01f, "the menu bar laid out a run");
    CHECK(menus[1]->size.x > menus[0]->size.x, "a longer menu title measures wider");

    // Dropdowns are the bar's own children, not the panel's, so they draw over
    // the buttons rather than under them.
    std::vector<std::shared_ptr<SpatialNode>> dropdowns;
    for (size_t i = 1; i < bar->children.size(); ++i) dropdowns.push_back(bar->children[i]);
    CHECK(dropdowns.size() == 2, "one dropdown per menu");
    if (dropdowns.size() != 2) return;

    CHECK(!dropdowns[0]->visible && !dropdowns[1]->visible, "a fresh bar has nothing open");

    menus[0]->on_click();
    CHECK(dropdowns[0]->visible && !dropdowns[1]->visible, "opening one opens exactly one");

    menus[1]->on_click();
    CHECK(!dropdowns[0]->visible && dropdowns[1]->visible, "opening another closes the first");

    menus[1]->on_click();
    CHECK(!dropdowns[0]->visible && !dropdowns[1]->visible, "clicking the open one closes it");

    bar->closeAllDropdowns();
    CHECK(!dropdowns[0]->visible && !dropdowns[1]->visible, "and closing all is idempotent");

    // The telemetry seed used to read "FPS: 60 | Mode: Quaternionic Spatial |
    // Sync: Delta" -- a measurement nobody took, drawn as though it were live.
    std::vector<std::shared_ptr<UILabel>> labels;
    for (const std::shared_ptr<SpatialNode>& child : bar->children[0]->children) {
        if (auto label = std::dynamic_pointer_cast<UILabel>(child)) labels.push_back(label);
    }
    CHECK(labels.size() == 2, "the bar carries a brand label and a telemetry label");
    if (labels.size() != 2) return;
    CHECK(labels[1]->text.empty(), "telemetry says nothing until something measures it");
    bar->setTelemetryText("tick 7");
    CHECK(labels[1]->text == "tick 7", "and says exactly what it is given");
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

    if (g_failures == 0) {
        std::cout << " [SCENE INPUT STATUS] Input routing PASSED with zero failures." << std::endl;
        return 0;
    }
    std::cout << " [SCENE INPUT STATUS] Input routing FAILED with " << g_failures << " failure(s)." << std::endl;
    return 1;
}
