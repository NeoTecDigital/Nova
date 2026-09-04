<!-- Written by Richard Christopher, Copyright 2026 NeoTec Digital -->

# Pass B — Splash Substrate Design (B2 facets + B3 Registry)

**Date:** 2026-09-03 · **Baseline:** `20329df` · **Parent plan:** `~/.claude/plans/splendid-crafting-blum.md`, `plans/phase-1_vazio-component-architecture.md` §0, §2, §S.5, §M
**Status:** adopted as the implementation spec. Landing order: **B3 → B2.0 → B2.a → B2.b → B2.c → B2.d → B2.e**, each independently green at both configs with `layer_purity` clean.

## 0. Re-measured inventory (vs the plan's numbers)

| Item | Plan said | Measured at 20329df |
|---|---|---|
| `SpatialNode` subclasses | 14 | **11 first-party + `ProbeNode`** — `Splash/Primitives.h:13,33,73,98`; `Clouds/ui/UIComponents.h:54,94`; `Clouds/ui/UIWindow.h:14`; `Clouds/ui/UIMenuBar.h:26,46`; `Clouds/ui/UIDockBar.h:21`; `Clouds/apps/SpatialFilesystem.h:19`; `tests/test_scene_input.cpp:71` |
| Virtuals | 8 | 8 (`Splash/SpatialNode.h:98-113`); **`onUpdate` is overridden by nobody** |
| Overrides | — | 27 across 11 classes |
| `shared_ptr<SpatialNode>` strict | 79 | **76**; any node type: **130** across 20 files |
| `addChild` / `removeChild` | 68 / 9 | **60** (21 test, 39 product) / **10** |
| `weak_ptr` | 3 | 2 declarations + 5 `.lock()` sites |
| `make_shared<node>` | — | 59 (22 test, 37 product) |
| direct `->children` reads outside SpatialNode | — | 21 (16 test, 3 `test_protocol_seat`, 2 product) |
| `getWorldTransform()` calls | — | 10 |
| Per-frame O(n) walks | 3 | 3 (`SpatialScene.cpp:427` cluster rebuild, `:432` `onUpdate` no-op, `:453` collect) + hit-test per motion |
| `test_scene_input` assertions | 48+ | **107** (88 `CHECK`, 19 `CHECK_NEAR`), 17 functions |

Dead state found (relevant to record layout): `phase_state` write-only tree-wide (`SpatialNode.h:27,91`; written `vazio_main.cpp:192`); `SpatialButton::hover_anim_` unused; `SpatialPillNode::is_expanded`, `children_pills`, `parent_pill` written at `SpatialFilesystem.cpp:239,244`, never read; `SpatialButton::onRayEnter/onRayLeave` (`Primitives.cpp:67-73`) pass-through; `physics_config.active_nodes` has no reader.

## 1. The facet shape

**Decision: a static per-kind vtable struct of plain function pointers, plus one `void* state` per node owned through a `destroy_state` slot.** Not `std::function`, not per-node function pointers.

- `std::function` is the *current* `SpatialSurfaceHost` pattern (`Primitives.h:108-112`, five of them): 32 bytes each on libstdc++, 160+ bytes per node even on labels/panels carrying no input. Vazio fills them with lambdas capturing `comp`/`surface` (`SpatialPopupHost.cpp:264-285`) and then must null all five in two places (`SpatialHosts.h:218-225`, `SpatialPopupHost.cpp:432-436`) because the captures dangle. A kind whose state *is* the host struct needs one `surface = nullptr` write.
- Per-node function pointers: there is no per-node variation in behaviour anywhere — all 11 classes dispatch identically per instance; only state varies. 56 bytes/node for nothing.
- Vtable struct: one `static const NodeKind` per presentable; per node 16 bytes (`kind*`, `state*`); dispatch = load kind, load slot, indirect call — the same three operations as a virtual call. The kind's `name` is B4's presenter key for free.

```cpp
// Splash/Facets.h
namespace Splash {
class Registry; struct NodeId;

struct ContentSource {   // emit THIS node's geometry; children are the traversal's job
    void (*collect)(void* state, Registry& reg, NodeId self, const glm::mat4& world,
                    Nova::SpatialMeshBuffer& mesh, std::vector<SpatialRenderCommand>& out) = nullptr;
};
struct InputSink {       // seat vocabulary, exactly the five hooks the scene routes today
    void (*enter) (void* state, Registry&, NodeId, const Nova::Math::RayHit&) = nullptr;
    void (*move)  (void* state, Registry&, NodeId, const Nova::Math::RayHit&) = nullptr;
    void (*leave) (void* state, Registry&, NodeId) = nullptr;
    void (*button)(void* state, Registry&, NodeId, const Nova::Math::RayHit&, uint32_t button, bool pressed) = nullptr;
    void (*key)   (void* state, Registry&, NodeId, uint32_t key, bool pressed) = nullptr;
};
struct PresentFeedback {
    void (*presented)(void* state, Registry&, NodeId, const struct timespec& when) = nullptr;
};
struct NodeKind {
    const char* name;                          // "splash.panel", "wayland.surface", "ui.button"
    ContentSource   content;
    InputSink       input;
    PresentFeedback present;
    void (*destroy_state)(void* state) = nullptr;   // null = state owned elsewhere (Vazio hosts)
};

// The base behaviour every override chains to today (SpatialNode.cpp:73-92):
namespace DefaultInput {
    void enter (void*, Registry&, NodeId, const RayHit&);   // is_hovered = true
    void leave (void*, Registry&, NodeId);                  // is_hovered = is_pressed = false
    void button(void*, Registry&, NodeId, const RayHit&, uint32_t, bool); // button 1: is_pressed; on press is_focused
}
}
```

`DefaultInput` exists because `UIButton::onRayButton` deliberately does **not** chain (`UIComponents.cpp:183-203`: a disabled button must not take `is_focused`; the click latch reads `is_pressed` before the base clobbers it). Composable defaults preserve that exactly.

`world` is passed in by the traversal from the cache (§2), so kinds stop calling `getWorldTransform()` — that is how the three O(depth) walks per node go to zero.

### `UIButton` as a kind literal

```cpp
// Clouds/ui/UIButton.cpp
struct ButtonState {
    const UITheme* theme; std::shared_ptr<Splash::SpatialFont> font;
    std::string label; ButtonVariant variant = ButtonVariant::PRIMARY; bool enabled = true;
    std::function<void()> on_click;
    Splash::NodeId caption;                  // child label; INVALID when fontless
    Splash::MeshCache box_cache;             // today's button regenerates its quad every frame (UIComponents.cpp:212)
};
static void buttonCollect(void* s, Splash::Registry& reg, Splash::NodeId id, const glm::mat4& world,
                          Nova::SpatialMeshBuffer& mesh, std::vector<Splash::SpatialRenderCommand>& out) {
    auto& st = *static_cast<ButtonState*>(s); auto& n = reg[id];
    const glm::vec4 fill = !st.enabled ? kDisabled : n.is_pressed ? st.theme->primary_active
                         : n.is_hovered ? hoverColor(st) : baseColor(st);          // UIComponents.cpp:143-173
    /* signature-keyed quad via st.box_cache; push {world, surface_dim(n.size), nullptr, first, count} */
}
static void buttonPress(void* s, Splash::Registry& reg, Splash::NodeId id, const Nova::Math::RayHit&, uint32_t b, bool down) {
    auto& st = *static_cast<ButtonState*>(s); auto& n = reg[id];
    if (!st.enabled || b != 1) return;
    if (down) n.is_pressed = true;
    else if (n.is_pressed) { n.is_pressed = false; if (st.on_click) st.on_click(); }
}
const Splash::NodeKind kUIButton = {
    .name = "ui.button",
    .content = { buttonCollect },
    .input = { .enter = Splash::DefaultInput::enter, .leave = Splash::DefaultInput::leave, .button = buttonPress },
    .destroy_state = [](void* s) { delete static_cast<ButtonState*>(s); },
};
Splash::NodeId makeUIButton(Splash::Registry& reg, Splash::NodeId parent, const UITheme& theme,
                            const std::string& label, glm::vec2 size, std::shared_ptr<Splash::SpatialFont> font,
                            std::function<void()> on_click = {}, ButtonVariant v = ButtonVariant::PRIMARY) {
    auto* st = new ButtonState{&theme, font, label, v, true, std::move(on_click)};
    const Splash::NodeId id = reg.create(parent, kUIButton, st);
    reg[id].size = size; reg[id].name = "UIButton: " + label; reg[id].captures_subtree_input = true;
    if (font) {                                                   // UIComponents.cpp:116-128
        st->caption = makeUILabel(reg, id, theme, label, font, TextRole::BODY, TextTone::PRIMARY, TextAlignment::CENTER);
        reg.transform(st->caption).position = {0, 0, 0.002f};
        reg[st->caption].claims_pointer_input = false;
    }
    return id;
}
```

The child is created *after* `id` exists. The `relinkChildren` bug class (`SpatialNode.cpp:10-16`) cannot recur: there is no moment where a node has children but no identity.

### `SpatialSurfaceHost` as Vazio constructs it

The seam splits into a Splash content half and a Vazio sink half; Vazio composes the kind (the direction the layer chain permits).

```cpp
// Splash/kinds/SurfaceQuad.h — the content half; also the canvas mode's content (plan §M)
struct SurfaceQuadState { std::shared_ptr<Nova::TextureHandle> texture; float corner_radius = 0.015f; Splash::MeshCache quad_cache; };
void surfaceQuadCollect(SurfaceQuadState&, Registry&, NodeId, const glm::mat4&, Nova::SpatialMeshBuffer&, std::vector<SpatialRenderCommand>&);
// = Primitives.cpp:258-290 verbatim, minus the trailing SpatialNode::collectRender

// Vazio/SpatialHosts.h — the sink half; state is EMBEDDED in the host, not heap-owned by the registry
struct HostedSurfaceState {
    Splash::SurfaceQuadState quad;
    SpatialCompositor* compositor = nullptr;
    struct wlr_surface* surface = nullptr;    // nulled by beginDestruction(); replaces clearInputRouting()'s five nulls
};
struct SpatialXdgWindow {
    WindowHandle handle; wlr_xdg_toplevel* toplevel; wlr_surface* surface;
    Splash::NodeId frame_panel, surface_host, title_label;     // were three shared_ptr (SpatialHosts.h:101-103)
    HostedSurfaceState surface_state;
    std::shared_ptr<Nova::TextureHandle> client_texture;       // unchanged; setTexture becomes surface_state.quad.texture = ...
    /* listeners unchanged */
};

// Vazio/SpatialSeatInput.cpp — one static kind replaces bindChildSurfaceInput's 5 lambdas x 3 host types
static void hostedEnter(void* s, Splash::Registry&, Splash::NodeId, const Nova::Math::RayHit& hit) {
    auto& st = *static_cast<HostedSurfaceState*>(s); if (!st.surface) return;
    st.compositor->notifySeatPointerEnter(st.surface, hit.uv.x * st.surface->current.width, hit.uv.y * st.surface->current.height);
}
/* hostedMotion, hostedLeave, hostedButton (focusSurface on press + sceneButtonToEvdev), hostedKey: SpatialPopupHost.cpp:266-285 */
static void hostedCollect(void* s, Splash::Registry& r, Splash::NodeId n, const glm::mat4& w, Nova::SpatialMeshBuffer& m, auto& out) {
    Splash::surfaceQuadCollect(static_cast<HostedSurfaceState*>(s)->quad, r, n, w, m, out);
}
static void hostedPresented(void* s, Splash::Registry&, Splash::NodeId, const struct timespec& when) {
    auto& st = *static_cast<HostedSurfaceState*>(s); if (st.surface) wlr_surface_send_frame_done(st.surface, &when);
}
const Splash::NodeKind kHostedSurface = {
    .name = "wayland.surface",
    .content = { hostedCollect },
    .input = { /* each hook: DefaultInput flag write, then forward — Primitives.cpp:223-256 order; explicit calls inside each hosted hook */ },
    .present = { hostedPresented },
    .destroy_state = nullptr,                                             // the host owns surface_state
};
```

**Minimal facet set:** `ContentSource` + `InputSink` covers all 27 overrides. `onUpdate` is overridden by nobody; per-frame animation is driven by owners iterating their own lists (`SpatialFilesystem::update` at `SpatialFilesystem.cpp:268-272`; `BootDesktop::update` at `vazio_main.cpp:202-214`). Delete the virtual and the walk at `SpatialScene.cpp:432`.

**No fourth facet.** Four kinds use `collectRender` as a per-frame pre-render hook emitting nothing — `UIWindow` (`UIWindow.cpp:191-196`), `UIMenuDropdown` (`UIMenuBar.cpp:62-67`), `UIMenuBar` (`:205-210`), `UIDockBar` (`UIDockBar.cpp:76-81`) — all `syncChromeToTheme()` pushing theme values onto child `SpatialPanel`s. A `Tick` facet in disguise. **Do not add it.** A `collect` that emits zero commands is already the container contract (eight such nodes exist). The fix is a `ThemedPanel` content variant with state `{const UITheme*, PanelRole}` resolving material at collect time, deleting the four sync hooks (B2.c). The plan's "Registry refuses a node with null `draw`" is **refuted** by the eight containers; the rule becomes **null `collect` = container**.

**`PresentFeedback`** is declared and populated by one kind, but the loop that calls it stays in the compositor: `onFramePresented` (`SpatialWindowHost.cpp:403-431`) iterates the host lists filtered on `mapped && !minimized`. A registry-wide walk would send `frame_done` to unmapped windows. Until a remote ContentSource exists (Phase 5) the compositor calls the slot for each mapped host. Do not invent a consumer.

## 2. What `SpatialNode` becomes

```cpp
struct NodeRecord {
    NodeId parent, first_child, last_child, next_sibling, prev_sibling;   // intrusive doubly-linked sibling list, no heap
    Nova::Math::QuatTransform local;          // was `transform` (SpatialNode.h:26)
    glm::vec2 size;
    Nova::Math::QuatTransform world_cache;    // memoized
    bool world_dirty = true;
    bool visible = true, interactable = true, is_hovered = false, is_pressed = false, is_focused = false,
         captures_subtree_input = false, claims_pointer_input = true;     // SpatialNode.h:31-50, names unchanged
    const NodeKind* kind = nullptr;           // null = plain container
    void* state = nullptr;
    std::string name;                         // kept: 7 test assertions match on it
};
```

~160 bytes, one allocation for the whole registry (sizes UNVERIFIED to the byte). Sibling list over `vector<NodeId>` because `raiseChild` is "move to end" (`SpatialNode.cpp:32` uses `std::rotate`) — O(1) on a list — and it kills the last per-node heap allocation.

**World transform: write-through accessor + subtree dirty, lazy validation on read.** `reg.transform(id)` returns a mutable ref and marks `id`'s subtree dirty; `reg.worldOf(id)` recomputes up to the nearest clean ancestor. Chosen over a per-frame top-down pass because **`test_scene_input` never calls `scene->update()`** — it aims, presses, and reads `window->transform.position` (`test:225-227`); a frame-pass-only cache fails 17 tests.

**`hitTest` becomes a free function over the Registry**, iterative, rules verbatim from `SpatialNode.cpp:97-135`: self on equal terms if `interactable`; children forward in sibling order; `if (child_hit.distance > best.distance) continue;` so an exact tie goes to the later sibling (`testRaiseChildDecidesTheHit`, `test:514-540`); `visible` prunes. `resolveInputTarget` walks `parent` ids; `projectRayOntoNodePlane` takes `(reg, id)`; `raiseChild(reg, parent, child, front_z, back_z)` unlinks/relinks last and writes z through `transform()`. Scoped capture, plane re-cast and the grab survive because none depended on the pointer type.

A traversal-time assertion (`in_traversal_`) forbids structural mutation inside `collect`.

## 3. Registry

```cpp
struct NodeId { uint32_t bits = 0;  // index:20 | generation:12  (plan S.5)
    uint32_t index() const { return bits & 0xFFFFF; } uint32_t generation() const { return bits >> 20; } };
class Registry {
public:
    NodeId create(NodeId parent, const NodeKind& kind, void* state);   // parent INVALID = detached
    NodeId createContainer(NodeId parent);                              // kind = null
    void   destroy(NodeId);      // unlinks the subtree NOW; frees at drain(); idempotent on dead ids
    void   drain();              // once per frame outside dispatch: destroy_state, free slots, bump generations
    bool   alive(NodeId) const;
    NodeRecord* get(NodeId);     // nullptr when dead; never a stale pointer
    NodeRecord& operator[](NodeId);          // asserts alive
    void   reparent(NodeId child, NodeId parent);            // keeps LOCAL (today's addChild semantics)
    void   reparentKeepingWorld(NodeId child, NodeId parent);
    bool   raiseChild(NodeId parent, NodeId child, float front_z, float back_z);
    Nova::Math::QuatTransform& transform(NodeId);           // write-through, dirties subtree
    const Nova::Math::QuatTransform& worldOf(NodeId);       // validates lazily
    NodeId parentOf(NodeId) const; NodeId firstChild(NodeId) const; NodeId nextSibling(NodeId) const; NodeId lastChild(NodeId) const;
    std::vector<NodeId> children(NodeId) const;             // snapshot; tests and DynamicSceneManager use it
    bool   isDescendant(NodeId node, NodeId ancestor) const;   // replaces nodeWithin (SpatialPopupHost.cpp:36-42)
    template<class T> T* stateAs(NodeId) const;             // reinterpret; caller asserts kind
private:
    std::vector<NodeRecord> slots_; std::vector<uint16_t> gen_; std::deque<uint32_t> free_;   // FIFO
    std::vector<NodeId> kill_; std::vector<uint32_t> quarantined_; bool in_traversal_ = false;
};
```

- **Ownership:** the Registry owns every record, flat; parent/child are links. `destroy(id)` destroys the subtree — what `shared_ptr children` already meant.
- **Deferred destruction vs the compositor's kill lists: keep parallel, chain them.** The five `pending_destroy_*` lists (`SpatialCompositor.h:475-488`) hold *hosts* (listeners, textures, protocol state). `drainDestroyedWindows` (`SpatialWindowHost.cpp:451-472`) calls `releaseNode`, then `reg.destroy(win.frame_panel)`; `iterateEventLoop` (`SpatialCompositor.cpp:264-268`) gains `registry_.drain()` as its last line.
- **Reparenting:** `reparent` = today's `addChild` (local kept); `reparentKeepingWorld` new, pinned by test.
- **Order:** sibling order is paint order; render walks forward; hit-test walks forward with the `>`-continue tie rule (identical outcome to current code). `raiseChild` O(1).
- **Generation wrap:** gen is 12 bits starting at 1 (zeroed `NodeId` is dead in both halves). At 4096 the slot is quarantined, never reissued. Free list is FIFO so churn spreads across the pool.
- **Nameable:** `NodeId` is a `uint32_t`; no pointer escapes. `WireHandle = SessionId ++ NodeId` is Phase 4 arithmetic on top.
- **Where it lives:** one `Splash::Registry` constructed in `main` (`Runtime`, `vazio_main.cpp:257-271`; `vazio_dev_main.cpp:166`) and by each test; `SpatialScene` takes `Registry&`, its `root` becomes a `NodeId`. No global.

## 4. Migration order — B3 first, with B2.0 bridging

Under B2-first, every kind builder returns `shared_ptr` and calls `addChild`, then B3 rewrites every builder again — 37 `make_shared` + 39 `addChild` sites touched twice. Under B3-first, subclass constructors become `(Registry&, NodeId self, …)` via `reg.emplace<T>(parent, args…)` (slot allocated *before* the constructor runs, so children attach under `self`); that signature *is* the kind-builder signature, so B2 changes bodies, not call sites. The ownership fix — the three-way pill holding (`SpatialFilesystem.cpp:242-244`), `relinkChildren`, `releaseNode`'s strong-reference dance (`SpatialScene.cpp:276-292`) — lands while every virtual body is byte-identical.

**B2.0 bridging:** after B3, `reg[id]` yields the legacy `SpatialNode&`. B2.0 moves the POD fields into `NodeRecord`, makes the legacy object behaviour-only (virtuals receive `Registry&, NodeId`), and the traversal dispatches `kind ? kind->content.collect(...) : object->collectRender(...)`. Dual dispatch is deleted in B2.e.

| Landing | Content | Gate notes |
|---|---|---|
| **B3** | `Splash/Registry.{h,cpp}`, `NodeId`; `SpatialNode` loses `enable_shared_from_this`/`parent`/`children`/`addChild`/`removeChild`/`relinkChildren`/`raiseChild`/`getWorldTransform`/`hitTest`; `SpatialScene` focus/grab/root → `NodeId`; cluster index retired (`SpatialScene.h:9,30`, `.cpp:191-207,359-364,424-428`); `onUpdate` walk deleted; every `shared_ptr<node>` → `NodeId` in Vazio/Clouds/mains; subclasses take `(Registry&, NodeId self, …)`; 11 `collectRender` bodies drop their trailing recursion | ~25 files. `test_scene_input` reworded (§6) + `tests/test_registry.cpp`; `test_protocol_seat.cpp:59-67` `findSurfaceHost` → kind/host lookup |
| B2.0 | fields → `NodeRecord`; legacy virtuals take `(Registry&, NodeId)`; dual dispatch | zero-semantic |
| B2.a | `kHostedSurface` in Vazio + `SurfaceQuad` in Splash; delete `SpatialSurfaceHost`, `bindChildSurfaceInput`, `clearInputRouting`, five nulls in `releaseChildHost` | `protocol_seat`, `disconnect_cycle`, `staged_boot_e2e` |
| B2.b | `SpatialPanel`, `SpatialLabel`, `SpatialButton` → kinds; `Active3DNode` → three `NodeId` | |
| B2.c | `UILabel`, `UIButton`, `UIWindow` → kinds; `UIMenuBar`/`UIDockBar`/`UIMenuDropdown` → builders + `ThemedPanel` (deletes four `syncChromeToTheme`) | flow-layout + theme tests |
| B2.d | `SpatialPillNode` → kind; delete `children_pills`/`parent_pill`/`is_expanded`; `selected_node_`/`on_select` → `NodeId` | `vazio-dev` builds |
| B2.e | delete the legacy virtual object; `ProbeNode` → probe kind; dual dispatch gone; decide `phase_state` (write-only) | |

## 5. The compositor seam

After B3, `SpatialXdgWindow`/`SpatialXdgPopup`/`SpatialSubsurface` hold `NodeId`s (`SpatialHosts.h:101-103,249,325`). `hostNodeForSurface` (`SpatialPopupHost.cpp:288-304`) returns `NodeId`; popups anchor with `reg.reparent(popup.surface_host, anchor)`; `nodeWithin` → `reg.isDescendant(focus, popup.surface_host)`; `placeChildOnParentQuad` reads `reg[anchor].size`, writes `reg.transform(child)`; `focusSurface` passes the id to `setKeyboardFocus`.

**`WindowHandle` stays its own space.** It names a host (three nodes + listeners + texture + protocol state), is monotonic and never recycled (`SpatialCompositor.h:493-495`), resolved by linear scan — no generation needed. Folding it into `NodeId` would make a window's identity one of its three nodes, which is wrong on unmap (nodes leave the tree, the window persists, `SpatialWindowHost.cpp:113-121`).

Parallel until Phase 4: `WindowHandle`; the five host kill lists (chained, not merged); `SessionId`/epochs/`WireHandle` (no code); `Registry` reached by reference from `Runtime`, promoted to Session-owned when Sessions exist.

## 6. What the tests need

**Wording (API) changes in `tests/test_scene_input.cpp`:** 22 `make_shared<…>` → builders (`makeUIWindow(reg, parent, …)`; B3 introduces them as `reg.emplace<T>` wrappers so B2 does not touch them again); 21 `addChild` → builder `parent` arg or `reg.reparent`; 16 `->children` reads → `reg.children(id)`/`reg.lastChild(id)`; `:192 parent.lock() == window` → `reg.parentOf(chrome) == window`; `:197 getWorldTransform()` → `reg.worldOf(chrome)`; 4 `hitTest(ray, hit, node)` → `Splash::hitTest(reg, root, ray, hit, node)`; 2 `raiseChild(b, …)` → `reg.raiseChild(root, b, …)`; 2 `dynamic_pointer_cast` → `reg[id].kind == &kUIButton` (B2.c); `ProbeNode` → `ProbeState` + `kProbe` (B2.e); field reads → `reg.transform(id)`, `reg[id].visible`, `reg[id].size`, `reg.stateAs<LabelState>(id)->text`.

**Semantic change: one, justified.** `:416 doomed.use_count() == 1` becomes: after `reg.destroy(doomed); reg.drain();`, `!reg.alive(doomed)` and the scene's three ids resolve to `INVALID`. The claim (the scene keeps nothing alive) is preserved; the inspected mechanism is gone. `:466` (address identity of `is_focused`) is vacuous once `UIWindow` is not a class (B2.c); its siblings `:470,475-480` survive against `reg[window].is_focused`. Everything else unchanged in meaning.

`tests/integration/test_protocol_seat.cpp`: `findSurfaceHost` (`:59-67`) → kind scan or compositor accessor; `s.host` a `NodeId`.

**New tests B3 owes** (`tests/test_registry.cpp`, registered beside `scene_input`): (1) slot reuse does not alias — destroy+drain A, create B in the same index, `A != B`, `get(A) == nullptr`; (2) generation-wrap quarantine — 4095 cycles on one slot, never reissued; (3) reparent keeps world consistent — both variants; (4) world-cache invalidation — move a grandparent after a grandchild's world was read; (5) deferred destroy inside a callback — a probe kind whose `button` hook calls `reg.destroy(self)`: no crash, unlinked immediately, `alive` until `drain()`, focus/grab ids dead after; (6) `destroy_state` runs exactly once per node on drain, never for null. **B2 owes:** (7) two-facet node — content+input receives input and renders; input-only is hit-testable and emits nothing; content-only is never delivered a hook; container draws nothing but its children are collected.

## 7. Honest cost

**B3 is atomic across three layers.** `addChild` is called from Splash (`DynamicSceneManager.cpp:106-108`), Vazio (`SpatialWindowHost.cpp:276,321,332`; `SpatialPopupHost.cpp:315,350`; `vazio_main.cpp:173-196,335,377`) and Clouds (four UI files, `SpatialFilesystem.cpp:52,147,183,242`); a tree half on `shared_ptr` links and half on `NodeId` links is two parent-link systems. Sweep: 130 `shared_ptr` sites, 60 `addChild`, 10 `removeChild`, 10 `getWorldTransform`, 21 direct `children` reads, 12 constructors, the `SpatialScene` API (26 occurrences), ~90 test lines — roughly 25 files. `SpatialFilesystem` gets smaller (three holdings → one `vector<NodeId>` index; two write-only members deleted).

**B2 per kind is deceptively small** — one header + one `.cpp`, builders' bodies only; B2.a is the largest at ~150 lines net removed.

**Smallest first landing worth committing: B3 alone.** It deletes `relinkChildren` and the constructor-ordering bug class, the three-way pill ownership, the per-frame cluster rebuild and the dead `onUpdate` walk, memoizes world transforms, gives the compositor `NodeId`s — pinned under 107 unchanged-in-meaning assertions plus six new ones, every virtual body intact.

**UNVERIFIED:** record sizes; that `layer_purity` stays green with `Vazio::kHostedSurface` naming `Splash::surfaceQuadCollect` (downward edge, should be fine); exact touched-line counts.
