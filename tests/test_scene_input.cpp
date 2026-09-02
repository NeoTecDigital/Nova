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

#include <cmath>
#include <cstdio>
#include <iostream>
#include <memory>
#include <string>

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

using Clouds::SpatialNode;
using Clouds::SpatialScene;
using Clouds::UI::UIButton;
using Clouds::UI::UILabel;
using Clouds::UI::UIWindow;

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

    void onRayEnter(const NovaMath::RayHit& hit) override {
        ++enters;
        SpatialNode::onRayEnter(hit);
    }
    void onRayLeave() override {
        ++leaves;
        SpatialNode::onRayLeave();
    }
    void onRayMove(const NovaMath::RayHit& hit) override {
        ++moves;
        SpatialNode::onRayMove(hit);
    }
    void onRayButton(const NovaMath::RayHit& hit, uint32_t button, bool pressed) override {
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

    const NovaMath::Ray3D ray(glm::vec3(0.0f, 0.0f, 3.0f), glm::vec3(0.0f, 0.0f, -1.0f));
    NovaMath::RayHit hit;
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

    const NovaMath::Ray3D ray(glm::vec3(0.0f, 0.0f, 3.0f), glm::vec3(0.0f, 0.0f, -1.0f));
    NovaMath::RayHit hit;
    std::shared_ptr<SpatialNode> node;

    CHECK(parent->hitTest(ray, hit, node), "parent subtree must produce a hit");
    CHECK(node == parent, "a deeper node behind the parent must not shadow it");

    // ...and the child still wins when it is genuinely in front.
    child->transform.position.z = 0.4f; // world z = 1.0
    NovaMath::RayHit front_hit;
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
        NovaMath::RayHit probe;
        std::shared_ptr<SpatialNode> probe_node;
        const NovaMath::Ray3D ray = NovaMath::unprojectScreenRay(
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

    if (g_failures == 0) {
        std::cout << " [SCENE INPUT STATUS] Input routing PASSED with zero failures." << std::endl;
        return 0;
    }
    std::cout << " [SCENE INPUT STATUS] Input routing FAILED with " << g_failures << " failure(s)." << std::endl;
    return 1;
}
