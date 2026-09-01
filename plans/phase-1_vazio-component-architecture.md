<!-- Written by Richard Christopher, Copyright 2026 NeoTec Digital -->

# Phase 1 — Vazio Component Architecture (v2)

**Date:** 2026-08-31 · **Supersedes:** v1
**Roadmap:** `roadmap_1788146769919_rijqx7s9o` · **Phase:** `phase_1788146783280_9yzo9915v`
**Baseline analysis:** notepad `87a9cc49-cb13-4352-9293-38e4beec0600`
**Verification basis:** working tree at branch `Clouds` (HEAD `eb2f312`; `src/`, `include/`, `examples/`, `tests/` are untracked — the working tree is what is cited). wlroots headers: `/usr/include/wlroots-0.19/` (pkg-config: 0.19.3). Every `file:line` below was re-read against the current tree during this revision; v1 citations that drifted are corrected in place and noted. Anything not checkable from this machine is marked **UNVERIFIED** with what would verify it.
**Adversarial audit result (2026-08-31):** 128 citations CONFIRMED (every wlroots/wayland/Vulkan header cite exact), 2 minor REFUTED (QEMU host-facts wording, corrected below), 24 DRIFTED — the Phase 0 fixes landed in the working tree *while the audit ran*: frame-indexed mesh slots + deferred growth + `MeshCache` (`spatial_mesh.*`), `getCurrentFrameIndex()` (`nova_graphics.h:96`), and the `isComplete()` sentinel fix (`atomic.h`). Citations into `spatial_mesh.*`, `SpatialScene.cpp`, `Primitives.*`, `SpatialFilesystem.cpp` therefore drift by ~9–19 lines; the defects described were real at time of writing and are precisely the ones now fixed. Re-read those five files before executing against their line numbers.

## Layering (settled — not relitigated)

`macOS : Darwin : Quartz ∷ Clouds : Core : Vazio`

- **Vazio** — this repo. The WindowServer: owns surfaces, composites, routes events. Nothing more.
- **wlroots** — client transport (Wayland protocol) only.
- **Nova** — Vulkan graphics+compute drawing layer inside Vazio.
- **Core** — separate project (mfsBSD base). All controller/coordinator logic.
- **Precipitation** — separate engine proxying Core into Vazio. YAML at that boundary.
- **OATS** — object registry + type system only.

Settled by direction: Session objects plural, not a singleton; `focus()` + `setTransform` with depth-placement policy at Precipitation — no opinionated `raise()`; boot splash is the **staged single-process model** (Vazio takes DRM master once, splash morphs into session in-engine — §B), not the Plymouth model.

---

## 0. Stance

Vazio presents and organizes; it does not decide. Every design choice is tested against one question: *is this a fact about geometry and pixels, or a decision about meaning?* Facts stay. Decisions move to Precipitation or Core.

The unifying primitive: **everything Vazio manages is a node with a geometry, a content source, and an interaction sink.** v2 amends this with a third content-side facet forced by the remote-session requirement: a **presentation-feedback channel** (§S.4). A node is: geometry (owned by the scene) + `{ContentSource, InputSink, PresentFeedback}` (owned by whoever supplies content). A Wayland toplevel, a calculator keypad, an OATS object, a remote desktop tile — and a boot-splash animation node — are the same shape; they differ only in which facets they carry.

---

## S. Session model (new — all definitions are **proposals for ratification**)

### S.1 Vocabulary — proposed definitions

The user, verbatim: *"we might be connected to multiple virtual clouds sessions .. we might have multiple 'Portals' open or even 'multiple Desktops' running (some local and some remote and most likely some are hybrid)."*

| Concept | Definition (proposed) | Owns | Cardinality |
|---|---|---|---|
| **Session** | A *client-attachment context*: one content transport plus one identity epoch. A **local** Session owns the display substrate (one `wl_display` + backend attachment) and, once **open**, its socket + protocol globals + one `wlr_seat`. A **remote** Session owns one Precipitation content channel and no `wl_display`. Local Sessions have a lifecycle state: `latent` (substrate up, no socket/globals — §S.6) → `open`. | Transport state, surface bookkeeping, the mint for its half of wire handles | N per process |
| **Desktop** | A *scene*: one spatial scene graph root + camera(s) + theme. The thing you look at. | Nodes, focus order, theme (`g_Theme` retargets here — v1 said "onto the Session"; corrected) | N per process |
| **Portal** | The *binding* projecting a Session's content into a Desktop sub-tree: which surfaces appear, under which root node, with which placement-policy hooks and input routing. | The Session↔Desktop mapping, per-binding placement fallback, the sub-tree root | N per (Session×Desktop); "multiple Portals open" = multiple live bindings |
| **Hybrid Desktop** | A Desktop whose Portals reference Sessions of differing origin. Not a new mechanism — §S.4. | nothing new | emergent |

Rationale: v1's single "Session object" conflated transport (wl_display+seat), scene (nodes+theme), and identity (handle table). Plural heterogeneous sessions make that untenable: a remote session has no `wl_display`; two desktops share no focus order; one handle table must serve all. So v1's Session becomes **Registry** (identity, §2) + **Session** (transport) + **Desktop** (scene) + **Portal** (binding). The current `SpatialCompositor` (`include/Clouds/SpatialCompositor.h:75-124`) is already the skeleton of a local Session — a plain object constructed in `main` (`examples/clouds_server_main.cpp:101`), not a singleton, holding exactly the local-session state (display :107, backend :109, renderer :110, allocator :111, xdg_shell :113, seat :114). Keep it that shape; rename when the substrate lands.

### S.2 Local sessions and the wlroots layer — what "multiple in one process" actually means

**Verified from headers — the API is instance-based, not process-global:**
- `wl_display_create(void)` returns a fresh instance (`/usr/include/wayland-server-core.h:190-191`); each display owns its own event loop (accessor used at `src/Clouds/SpatialCompositor.cpp:52`). Sockets are added explicitly per display: `wl_display_add_socket` / `_auto` / `_fd` (`wayland-server-core.h:200-206`) — a display with zero sockets is unreachable by clients (load-bearing for §S.6). Two displays with two socket names coexist by construction.
- wlroots backends bind to an explicit `wl_event_loop`, not a global: `wlr_backend_autocreate(struct wl_event_loop *loop, struct wlr_session **session_ptr)` (`/usr/include/wlroots-0.19/wlr/backend.h:64-65`), `wlr_headless_backend_create(loop)` (`wlr/backend/headless.h:19`), `wlr_multi_backend_create(loop)` (`wlr/backend/multi.h:18`), `wlr_wl_backend_create(loop, remote_display)` (`wlr/backend/wayland.h:19-20`).
- `wlr_seat` is per-display (`wlr_seat_create(wl_display_, "seat0")` at `SpatialCompositor.cpp:100`), so seat names cannot collide across sessions.

**Verified process-global state (the honest constraints):**
1. **Logging**: `wlr_log_init` (`wlr/util/log.h:44`) and `wl_log_set_handler_server` (`wayland-server-core.h:701`) are global — process policy, set once in `main`. (Today `wlr_log_init` is inside `startServer`, `SpatialCompositor.cpp:44` — must move.)
2. **Environment**: `wlr_backend_autocreate` selects "the most suitable backend given the environment" (`backend.h:54`); env vars are process-global, so two *autocreated* backends would read identical configuration. The variable list (`WLR_BACKENDS`, …) is not in the installed headers — **UNVERIFIED** (verify: wlroots 0.19.3 `docs/env_vars.md`).
3. **The physical seat**: a `wlr_session` "manages access to physical devices," is "only required when running on bare metal," and creation is "taking control of the current virtual terminal. This should not be called if another program is already in control" (`wlr/backend/session.h:25-37, 91-98`). One VT, one controlling session ⇒ **at most one Session per process owns the physical seat**; any number may be headless-local or remote.
4. **Threading**: wlroots is conventionally single-threaded per event loop — community-documented, not header-verifiable, **UNVERIFIED**. Design assumption: all sessions' loops dispatch from the one render thread, as `iterateEventLoop` does today (`SpatialCompositor.cpp:274-279`, called at `clouds_server_main.cpp:235`) — round-robin `timeout=0`, or poll each loop's fd (`wl_event_loop_get_fd`, `wayland-server-core.h:176`).

**Conclusion:** multiple `wl_display`s per process is supported by the API and is the design basis; what is singular is physical-seat ownership. **Session[phys]** (0 or 1: autocreate backend + `wlr_session` + real input/outputs) and **Session[virt]** (0..n: headless backend + named socket — client isolation free, since clients of display A cannot name objects on display B). Residual risk of hidden global state in transitive libraries — **UNVERIFIED**; the two-session unit test in Phase 4 is the verification.

### S.3 Remote sessions — the interface contract Vazio offers

Vazio does not invent the transport (Precipitation/Core's job). Vazio publishes what a remote content source must supply for indistinguishable presentation — exactly the three facets of §0, made explicit:

**(a) Frames (ContentSource).** Per attachment: DRM-fourcc format + extent + stride; delivery as CPU memory (mandatory baseline — same import path as `wl_shm`) or dmabuf fd (optional; only meaningful when the proxy is a local process; negotiated, never assumed). Frames carry a monotonic `frame_id` and an optional damage-rect list (absent = full frame). Lands on the same texture-update path as local SHM (§1.1 step 3) — the importer cannot tell origins apart, which is the point.

**(b) Input (InputSink).** Vazio → source, seat vocabulary only: `enter(u,v) / motion(u,v,t) / button(evdev,state,t) / axis / leave`, `key(evdev,state,t)` + modifier state, `focus_gained/lost`. Coordinates are normalized surface-UV — the raycast already produces exactly this (`hit.uv`, consumed at `src/Clouds/Primitives.cpp:175-179`). Wayland serials never cross the wire (per-`wl_display` artifacts).

**(c) Presentation feedback + pacing (PresentFeedback).** Vazio reports `presented(frame_id, time)` after actual presentation — the same fact wlroots surfaces via the output `present` event (`wlr/types/wlr_output.h:205-207`). Source declares pacing at attach: `throttled` (next frame after `presented` — the frame-callback discipline; default) or `free-run` (Vazio keeps latest, drops stale, never blocks its render loop on a remote source). Vazio guarantees only "presented when composited" — latency bounds are transport policy.

**(d) Lifecycle.** `attach(desc) → handle`; resize as `proposal(extent, serial)` outward / `ack(serial)+frame` inward (mirroring xdg configure/ack, §1.1); `detach`; transport death ⇒ unmap (not destroy) + fact reported outward. Reconnect mints a **new session epoch** (§S.5): stale handles fail, never alias.

**(e) Refused:** codec/compression, encryption, authentication, reconnection policy — transport concerns, Precipitation's. If they leak into this contract, the boundary was drawn wrong.

### S.4 Hybrid — confirm or refute "it's the ContentSource axis"

**Confirmed, with one rigorous amendment.** For *pixels*, hybrid is exactly v1's ContentSource axis: `SpatialSurfaceHost` draws whatever `texture` it holds (`src/Clouds/Primitives.cpp:196-223`; setter at `:171-173` — still zero call sites, re-verified; declaration `include/Clouds/Primitives.h:90`), and traversal/hit-test/render consult only geometry and flags (`src/Clouds/SpatialNode.cpp:58-88`) — provably origin-blind.

The amendment: ContentSource alone is **not** sufficient, because two origin-dependent behaviors sit outside it — (1) *pacing*: local surfaces throttle via `wlr_surface_send_frame_done` (zero occurrences tree-wide today, grep-verified — §1.1), remote via S.3(c); (2) *resize negotiation*: local via xdg serials (`wlr_xdg_toplevel_set_size` returns the serial, `/usr/include/wlroots-0.19/wlr/types/wlr_xdg_shell.h:392-397`), remote via S.3(d). Same *shape* (propose→ack→commit), different plumbing — so the node-facing abstraction is the trio `{ContentSource, InputSink, PresentFeedback}` with per-origin implementations. v1's two-facet model refuted in the narrow sense; the axis mechanism confirmed.

### S.5 Location transparency — the handle scheme (proposal)

Requirements: `NodeId` nameable across a wire; stale handles fail rather than alias; reconnects never resurrect names.

- **In-process:** one **Registry** per process (not per session — hybrid Desktops span sessions). `NodeId` = `u32` generational slot-map handle, `index:20 | generation:12` (1,048,575 live nodes; 4,096 generations/slot). Mismatch = dead handle, error, never a pointer. On generation wrap, quarantine the slot.
- **On the wire:** `u64 = SessionId(u32) ++ NodeId(u32)`, `SessionId = slot:8 | epoch:24`. Every session (re)connect — and a local Session's `latent→open` transition (§S.6) — mints/keeps epochs such that any handle from a dead epoch answers `NOT_FOUND`. Locally-created nodes (shell, Lua, splash) carry reserved `SessionId 0` (process epoch, bumped per process start).
- Handles are opaque to peers; the bit-split is Vazio-internal (peers round-trip the `u64`).
- OATS interop: OATS-ffi ids are heap strings (`char*` returns, `extern/OATS-ffi/include/oats_ffi.h:15-43`; `std::string` throughout `include/Clouds/OatsBridge.h:23,32,44`). The Registry maps OATS string-id ↔ NodeId at the presenter boundary; OATS strings never become scene handles.

Marked **proposal**: the 20/12 and 8/24 splits, quarantine rule, and `SessionId 0` reservation need ratification (they shape the Precipitation schema).

### S.6 The boot splash in the session model — latent Session, boot Desktop

Question posed: is the splash a Session (degenerate local, no clients) or a pre-session state? **Answer: neither a new object nor a bare pre-state — the splash stage is the boot Desktop plus Session[phys] in `latent` state.**

- The staged model's invariant is *substrate continuity*: one DRM master acquisition, no handoff (§B). The object holding `wl_display`+backend+outputs at splash must be *the same object* that serves clients later. Modeling splash as a separate "pre-session thing" would reintroduce an ownership handoff in code — exactly what the staged model exists to eliminate. A lifecycle state on one object models a morph; a second object models a handoff. So: **`latent`** = display created, **zero sockets** (unreachable by construction — `wl_display_add_socket` is explicit, `wayland-server-core.h:200-206`), backend/outputs live, no protocol globals, no seat. **`open`** = socket added, `wlr_compositor`/`wlr_subcompositor`/`wlr_data_device_manager`/`wlr_xdg_shell`/`wlr_seat` created, listeners bound. **Update 2026-08-31:** `startServer` has since been decomposed (uncommitted) into `initBackendStack()` / `initProtocols()` / `initInput()` / `initSocket()` — the split this section prescribes is now mechanical: `initBackendStack()` is the substrate/`latent` half; `initProtocols()+initInput()+initSocket()` become the `open()` half.
- The splash *scene* is an ordinary **Desktop** (scene root + camera + theme — it needs nothing a Desktop doesn't have), populated by local nodes under `SessionId 0`. The splash→session morph is then scene-graph mechanics: either rebind Portals into the same Desktop (morph) or switch the presented Desktop (swap). Vazio provides both mechanisms; *which* to use, and the transition animation, is theme/Precipitation policy — flagged, not decided here.
- Epochs: `latent` mints no client-facing handles (no clients exist), so `latent→open` needs no epoch invalidation; the epoch is stamped at `open`.

---

## 1. Component design

### 1.1 Window / surface lifecycle

Primitive set unchanged from v1 except `raise()` is deleted per the settled pushback:

| Primitive | Why it earns its place |
|---|---|
| `createSurface(SurfaceDesc) → NodeId` | Desc: extent-px, content source, input sink, parent. Nothing semantic. |
| `configure(NodeId, extent) → Serial` | Vazio *proposes*. The missing link blocking every client today. |
| `ack(NodeId, Serial)` | Until acked, old geometry is authoritative (xdg discipline). |
| `map(NodeId)` / `unmap(NodeId)` | Separate from create/destroy. `SpatialCompositor.cpp:254` inserts `frame_panel` into the scene at *toplevel-create* time, before any buffer exists. |
| `destroy(NodeId)` — deferred | Kill list drained once per frame outside listener dispatch. |
| `focus(NodeId)` | Focus order is real and per-Desktop. No `raise()`. |
| `frameDone(NodeId, t)` | `wlr_surface_send_frame_done`: zero occurrences tree-wide (grep-verified) — throttled clients render once and block forever. |

**Excluded:** placement policy — the staggered-modulo layout at `SpatialCompositor.cpp:204-207` is policy in the wrong repo; keep only as the labelled fallback when no Portal placement is supplied. Also excluded: app launch, permissions, "a Terminal should be 80×24."

**wlroots work, in dependency order (all header refs re-verified):**

1. On `xdg_surface->surface->events.commit`, when `xdg_surface->initial_commit` (`/usr/include/wlroots-0.19/wlr/types/wlr_xdg_shell.h:300`) call `wlr_xdg_toplevel_set_size(toplevel, 0, 0)` (`wlr_xdg_shell.h:396-397`). **This single missing call is why nothing displays.** Today's commit handler (`SpatialCompositor.cpp:7-25`) only resizes panels; it never configures.
2. Bind `surface->events.map` / `events.unmap` — on `wlr_surface` in 0.19: `map` at `/usr/include/wlroots-0.19/wlr/types/wlr_compositor.h:214`, `unmap` at `:221` (v1 cited 213; that is the doc comment — corrected). Scene insertion moves from toplevel-create (`:254`) to map.
3. On commit-with-buffer, import. `wlr_client_buffer` at `/usr/include/wlroots-0.19/wlr/types/wlr_buffer.h:157-176`, raw client buffer in `->source` (`:168`). Dispatch: `wlr_buffer_get_dmabuf` (`wlr_buffer.h:92`) for zero-copy later; `wlr_buffer_begin_data_ptr_access`/`end` (`:144-152`) for the SHM path now. Feed `SpatialSurfaceHost::setTexture` (`Primitives.cpp:171-173`, zero call sites).
4. `wlr_surface_send_frame_done` after present — monotonic timespec, mapped surfaces only, never in the commit handler. Interim "after present" = after `graphics->renderFrame(...)` returns (`clouds_server_main.cpp:249-257`); final = the output `present` event (`wlr/types/wlr_output.h:205-207`) once §D lands. Route through one hook so the call site moves once (§3, P1.e).
5. Deferred destroy — the fix for `onDestroy` → `removeWindow` (`SpatialCompositor.cpp:27-31` → `:261-272`) erasing the owning `shared_ptr` (and its embedded `wl_listener`s, `SpatialCompositor.h:68-69`) during signal dispatch.

**Renderer note (carried, re-verified):** Vazio never imports through the wlroots renderer for drawing; `wlr_renderer` stays because `wlr_renderer_init_wl_display` "initializes wl_shm, linux-dmabuf and other buffer factory protocols" (`/usr/include/wlroots-0.19/wlr/render/wlr_renderer.h:80-86`; called at `SpatialCompositor.cpp:69`). Renderer-less `wlr_shm_create` exists (`wlr/types/wlr_shm.h:36-40`) but drops linux-dmabuf; not worth it. Under §D the renderer also feeds `wlr_output_init_render` (already at `SpatialCompositor.cpp:173`).

**Texture update path:** `TextureBridge::createTextureFromRGBA` (`Core/modules/spatial_pipeline/texture_bridge.cpp:137`) does a full `vmaCreateImage` (`:176`) + blocking `immediateSubmit` (`:179`) per call; no update-in-place sibling exists (grep-verified). Needs `updateTextureFromRGBA(handle, pixels, stride, damage_rects)` reusing a persistent image + staging buffer, reallocating only on resize. The damage parameter is how S.3(a) stays cheap.

### 1.2 Input & interaction routing — revised for the backend flip

**Primitives:** unchanged — `pointerMove(ray)`, `pointerButton`, `scroll`, `key(evdev, pressed)`, `modifiers(...)`; `HitResult{NodeId, uv, world, distance}`; separate `setPointerFocus`/`setKeyboardFocus` (Wayland requires the split; both notify paths exist at `SpatialCompositor.cpp:218-225`, `:159-168`); `grabPointer/releasePointer`. The grab earns its place because `processPointerButton` routes to `hovered_node_` (`src/Clouds/SpatialScene.cpp:138-147`), which can change between press and release.

**Three defects (re-verified):**

- `SpatialNode::hitTest` (`src/Clouds/SpatialNode.cpp:58-78`): children in reverse order, first-hit-wins early return (`:62-66`), no distance comparison; self tested only after (`:68-75`) so descendants always shadow the parent. **This is why `UIWindow`'s titlebar drag never fires** — `body_panel_`/`titlebar_panel_` are children (`src/Clouds/UI/UIWindow.cpp:31,48`) and always win; the drag logic itself (`:128-134` motion, `:136-154` button, band test `:142-148`) is correct. Fix: nearest hit across self+descendants + `captures_subtree_input`.
- `include/Clouds/UI/UIWindow.h:17` `bool is_focused = true` **still shadows** `SpatialNode::is_focused` (`include/Clouds/SpatialNode.h:35`). Base written by `SpatialNode::onRayButton` (`SpatialNode.cpp:50` — v1 cited :51; corrected), derived by `UIWindow::setFocused` (`UIWindow.cpp:101`). Delete the derived member. (v1-era `is_hovered` shadowing is gone — no such member in current `UIWindow.h`.)
- SDL key translation (`examples/clouds_server_main.cpp:164-168` — v1 cited :140-144; file grew): `:164/:167` send SDL *scancodes* (≠ evdev) to the compositor; `:165/:168` send *keysyms* to the scene — two consumers, two wrong currencies; `wlr_seat_keyboard_notify_modifiers` appears nowhere (grep-verified), so Shift/Ctrl are dropped. **Revision:** under the flipped backend (§D), keyboards arrive as `wlr_keyboard`s already speaking evdev via `backend.events.new_input` (`/usr/include/wlroots-0.19/wlr/backend.h:47`) on nested *and* DRM — the SDL table is scaffolding that dies with the SDL window. Write the minimal table only if Phase 2A typing demos precede 2B; label disposable.

**v1's "headless moots `new_input`" is dead — revised:** under DRM, `new_input` is the *only* input source (libinput backend inside autocreate against the `wlr_session`; standalone form `wlr_libinput_backend_create(session)`, `/usr/include/wlroots-0.19/wlr/backend/libinput.h:19`). Bind it; real keymaps onto the seat; delete the hand-`calloc`'d keyboard (`SpatialCompositor.cpp:104-116`).

**The pointer under DRM:** remains the 3D raycast reticle — still no `wlr_cursor` for drawing. New piece DRM forces: libinput pointers deliver *relative* motion and SDL's absolute position disappears — add a per-seat accumulator (clamped to the output box) feeding the existing raycast entry (`SpatialScene::processPointerMotion`, `SpatialScene.cpp:65-136`). `wlr_output_layout` (header present) is optional at single-output scope; adopt for multi-output. Seat capabilities (`SpatialCompositor.cpp:101`, POINTER|KEYBOARD unconditional) become dynamic from devices present.

**Excluded:** keybinding semantics, device-config policy, gestures-as-commands. Exception stands: the compositor-reserved key list — data the Desktop holds, Precipitation populates.

### 1.3 Scene graph & spatial organization

Primitives unchanged (`create/reparent/destroy/setTransform/setExtent/setVisible/setInteractable/worldTransform/query(Ray)/query(Frustum)/forEachVisible`). Extents on the node (`SpatialNode.h:28`) — already true.

**Storage: flat slot map** replacing the `shared_ptr` tree (`SpatialNode.h:37-38`), addressed by the S.5 `NodeId`. Dividends: deferred destroy, safe script/wire handles, cache-friendly traversal. `getWorldTransform()` (`SpatialNode.cpp:19-24`) walks to the root per call from `collectRender` (`Primitives.cpp:15,72,146,200`), `hitTest` (`SpatialNode.cpp:71`) and `rebuildSpatialIndex` (`SpatialScene.cpp:51`) — three O(depth) walks per node per frame. Memoize with a dirty bit.

**Retire the spatial index (re-verified):** `SpatialScene.cpp:84-86` calls `queryRay` and discards the candidate list; only `tests_performed` survives into `physics_config.cluster_tests_per_frame` (`:86`), consumed solely as display telemetry (`src/Clouds/EngineHUD.cpp:262`, `src/Clouds/ImGuiEngineOverlay.cpp:252`). `Core/math/spatial_cluster.h:98-114` is an O(n) scan over a flat hash map (`:102`) at a single depth; the "Hierarchical" of its doc (`:43-48`) is unimplemented; full rebuild every frame (`SpatialScene.cpp:168-171`). Replace with traversal + AABB early-out; BVH only when a profile demands. `tests/test_seam_math_raycast.cpp` includes this header (`:2`, CHECK harness `:19-26`) — retarget, don't drop.

**The mesh-buffer hazard (re-verified, citations refined):** `SpatialScene.cpp:239` uploads while the current frame's command buffer is being recorded (the render lambda runs inside `vkBeginCommandBuffer`/`vkEndCommandBuffer`, `Core/nova_graphics.cpp:435-476`, invoked from `clouds_server_main.cpp:249-257`) into a single non-frame-indexed `CPU_TO_GPU` buffer (`spatial_mesh.cpp:209-219`) with `MAX_FRAMES_IN_FLIGHT = 2` (`Core/modules/atomic/atomic.h:55`). On overflow, `upload` calls `allocateGpuBuffers(×2)` (`spatial_mesh.cpp:242-244`), which `vmaDestroyBuffer`s the old buffers immediately (`:202-207`) while in-flight command buffers may reference them. Capacity 65,536 verts (`SpatialScene.cpp:17`); every panel/button/label/pill regenerates its mesh per frame (`Primitives.cpp:19,84,106,149,203`; `SpatialFilesystem.cpp:186-193` — pill count is scan-root-dependent now that the root is CLI-configurable, `clouds_server_main.cpp:21-31,85-88`; v1's "157 pills / 70,650 verts" were run-specific measurements, not tree facts). Fix unchanged: (1) frame-index the buffer; (2) defer growth past `vkWaitForFences`, draw truncated on the overflow frame; (3) cache static geometry per node. Frame-indexing needs the frame index, which `SpatialMeshBuffer` cannot see — `frame_ct` is private with no accessor (`Core/nova_graphics.h:36` — v1 cited :38, which is `current_frame()`; corrected).

### 1.4 Object/entity presentation — the Precipitation seam

Unchanged in substance; re-verified. `DynamicSceneManager::createNodeFor` (`src/Clouds/DynamicSceneManager.cpp:78-111`) hardcodes pill→panel+button+label; `SpatialFilesystem` hardcodes entry→capsule and runs `std::filesystem` scanning inside the window server (`SpatialFilesystem.cpp:232-233`, `directory_iterator` `:271`).

```
struct PresentationRequest { string kind; WireHandle parent; Transform xf; vec2 extent; PropertyBag props; }
PropertyBag = flat map<string, variant<double,string,bool,vec4>>
registerPresenter(kind, PresenterFn) / present / update / retract / describeCapabilities
```

Ships presenters only for `wayland.toplevel`, `text`, `panel`, `image`. Unknown kind → error and **nothing appears**. `PropertyBag` stays flat (survives YAML without schema). Multi-session addition: `present` requests arrive per Portal, parent under that Portal's root; `WireHandle` is the S.5 u64.

**What crosses, and which way** — unchanged from v1 (inward: requests/updates/retractions/placement/theme/key reservations/catalog; outward: structural facts + measured timing only; never: calling into Core, launching processes, caching decisions). Additions: session lifecycle facts flow outward (`session attached/lost`, epoch changes) — and the boot-readiness signal flows *inward* (§B.2).

**OATS's role shrinks (re-verified):** three concrete domain types at `include/Clouds/OatsBridge.h:16-18` (structs `:22-49`) plus the 395-line `include/Clouds/OatsTraitCodec.h` (wc-verified) collapse to `(id, type_name, traits) → PresentationRequest{kind=type_name, props=traits}`. A deletion, not a rewrite. Salvage: `removeStaleNodes` (`DynamicSceneManager.cpp:123-140`) is authoritative-removal+safety-net — the reconciliation a presenter registry runs, and under S.5 also the cleanup for session-epoch death; `syncNodesFromState` (`:62-76`) survives near-verbatim.

**Encoding caution (carried):** yaml-cpp 0.9.0 installed (pkg-config-verified; not in the build). YAML for the declarative channel; line-oriented framing for the event channel; if YAML is mandated for both, keep it off the frame path. Remote-session frames (S.3a) are never YAML — binary buffers by definition.

### 1.5 Customizability, scripting, base OS applications

**(a) Visual sandbox.** `UITheme` (`include/Clouds/UI/UITheme.h:8-73`) is the right shape — flat POD. `g_Theme` is an `inline` global at `:75`; it moves onto the **Desktop** (per-Desktop theming is the point of plural desktops). Anchored `Layout` regions replace the menubar/dock magic constants (`src/Clouds/UI/WindowManager.cpp:37,64`). Excluded: CSS-like cascade.

**(b) Scriptable interface.** The scripting surface = Registry/Desktop/Portal primitive set (~25 functions; now includes `listSessions`/`listPortals`). System Lua is 5.5.0, duktape also present (pkg-config-verified; neither linked today). Take Lua; restricted `_ENV`; errors report-and-continue; transactional node creation. Excluded from scripts: Vulkan, wlroots, spawn, filesystem.

**(c) Base OS applications.** Calculator built completely (~300 lines, deterministic, unit-testable in the existing CHECK harness); messenger with honest empty state (salvage the pattern at `CrmWorkspace.cpp:37,62`); settings bound to real knobs (`Core/math/engine_physics.h:18-64`, wired via `WindowManager.cpp:225-246` and the ImGui overlay); monitor showing only measured values.

**Fabrication ledger — revised, because the tree improved:** FPS is now genuinely measured twice — per-frame (`clouds_server_main.cpp:135`) and 0.5 s-windowed (`SpatialScene.cpp:158-165`). Remaining dishonesty, narrower than v1: (1) the initializer `current_fps = 60.0f` (`engine_physics.h:37`) and the `dt≤0.0001 → 60.0` fallback (`clouds_server_main.cpp:135`) assert 60 without measurement — seed 0, report "not yet measured"; (2) literal seed strings asserting metrics before the first update tick: `EngineHUD.cpp:43`, `:77`, `UIMenuBar.cpp:88` (updaters do feed real values — `EngineHUD.cpp:242-263`, `WindowManager.cpp:237-246`); (3) `WindowManager.cpp:149` — one literal carrying three fabricated claims; (4) `WindowManager.cpp:158-164` — a button whose entire action is a log line (`:163`). The emoji at `WindowManager.cpp:159` and `EngineHUD.cpp:43,249` cannot render: atlas is ASCII 32–126 (`spatial_font.cpp:58`), keyed on `char` (`spatial_font.h:51`), no UTF-8 decode, no `\n`; unknown bytes advance 20 units (`spatial_font.cpp:170-174`).

---

## 2. Agnosticism mechanism — revised for plural sessions

**v1's answer stands, strengthened: the dictionary is right; the singleton is wrong — and the "one Session object" is now also wrong, split per §S.1.**

The identity authority is one **Registry** per process (slot map, S.5). Sessions, Desktops, Portals are plain objects constructed in `main`, passed by reference; a test constructs two of anything; nothing is global. (Globals to unwind: `g_Theme`, `UITheme.h:75`; `wlr_log_init` placement, `SpatialCompositor.cpp:44`.)

Three techniques, costs as in v1:
1. **Handle indirection** — kills the `removeWindow`-during-dispatch use-after-free class (`SpatialCompositor.cpp:27-31`,`:261-272`); ids nameable from YAML/Lua/wire. Cost: generation check per lookup; lose `shared_ptr` ergonomics.
2. **Capability records over inheritance** — `{ContentSource, InputSink, PresentFeedback}` as type-erased structs; a new presentable is a struct literal, not the subclass+header+cpp+CMake-line each UI file costs today. Cost: no override checking; null slots are runtime errors; mitigation: Registry refuses a node with null `draw`, and says so.
3. **String→factory presenter registry** — runtime errors, dumpable capabilities; registration at startup.

Honest summary unchanged: static type safety is traded exactly where pluggability is gained.

---

## B. Staged boot / splash (new)

Accepted direction: **staged single-process model.** `vazio` starts in early userspace as soon as the DRM device exists, takes DRM master **once**, renders a minimal splash scene, and when Core/Precipitation signal readiness the same process swaps splash for session in-engine. One master acquisition, no handoff flicker, no VT dance; the loading screen morphs into the desktop.

### B.1 Stages

| Stage | What runs | What must NOT run |
|---|---|---|
| **Pre-kernel** | Bootloader/kernel framebuffer | Out of scope for Vazio |
| **Splash** | `wl_display` (zero sockets) + autocreate backend (`wlr_session` + DRM outputs) + renderer/allocator + Nova (§D.2 offscreen mode) + one boot **Desktop** with animated local nodes. Session[phys] in `latent` state (§S.6). VT-active handling (`session.events.active`, `wlr/backend/session.h:59-63`) — pause rendering while switched away. | Wayland socket, protocol globals, seat, OATS, Precipitation channel, filesystem scan, ImGui, SDL |
| **Session** | Everything: socket added, globals created, listeners bound (`SpatialCompositor.cpp:77-128` becomes `open()`), clients attach, Portals bind | — |

### B.2 The transition — Vazio's side of the readiness contract (only)

- Vazio accepts a readiness channel handle at launch (a CLI-passed fd or path — the *mechanism and provisioning* are Core's rc-script domain; Vazio only documents what it accepts). One readable token = "ready" ⇒ Session[phys] `latent→open` + Desktop morph/swap. This is the first message of the Precipitation inward channel or a Core-provided sentinel — either satisfies the contract; Vazio does not care which.
- During splash, Vazio publishes *outward* only honest status if asked to render it: elapsed time, and stage facts it actually knows ("waiting for readiness signal"). It fabricates no progress percentages — there is no measured total to report against (§1.5's fabrication rule applies to the splash too).
- **If readiness never arrives: policy question — flagged for the user, not decided here.** Options with consequences: (i) stay on splash indefinitely with honest status text (machine appears hung but is truthful); (ii) after a timeout, open the session anyway onto an empty desktop (recoverable, but presents an OS that isn't ready); (iii) after a timeout, exit with a distinct code so Core's rc fallback takes over (§B.4). The timeout value itself, if any, is Core/Precipitation policy data, not a Vazio constant.

### B.3 Modularity forcing function — what has to be decoupled (cited)

The splash requires Nova + scene graph with zero compositor/OATS/Precipitation dependencies. The *libraries* already permit this — `SpatialScene`'s includes are Nova/math only (`include/Clouds/SpatialScene.h:1-12`), and the scene layer never touches wlroots. The *composition* does not. Entanglements in the current init sequence, in order:

1. `examples/clouds_server_main.cpp:51-55` — `Nova(config)` in graphics mode hard-creates the SDL window (`Nova.cpp:33-46`: `SDL_Init` + `SDL_CreateWindow` + `NovaGraphics(..., _window)`). On bare DRM there is no display server for SDL. The compute branch (`Nova.cpp:21-30`) proves the SDL-free construction pattern but yields `NovaCompute`, not graphics. **Decouple:** the §D.2 offscreen `NovaGraphics` mode, constructed directly (not via `Nova`).
2. `:79-82` — OATS is load-bearing: `if (!oats_bridge->initialize()) return 1;` aborts the entire server. **Decouple:** OATS constructed only at session stage; failure degrades (no OATS presenters), never aborts.
3. `:85-88` — filesystem scan at startup (`SpatialFilesystem` + `scanAndBuild3DTree`). Session-stage-only (and per §1.4, eventually not Vazio's at all).
4. `:91-98` — ImGui overlay (SDL-coupled, `imgui_impl_sdl2`). Dev-nested-only; absent at splash and under DRM (§D.2).
5. `:101-102` — compositor + `startServer` (socket immediately). Splits per §S.6 into substrate-up (splash) / `open()` (session).
6. `:137-232` — the loop is SDL-event-driven; `:249-257` — presentation is swapchain-coupled. At splash the loop is `wl_event_loop` + output `frame` events (§D.2); no SDL anywhere.
7. **Asset fragilities for early userspace:** shaders load CWD-relative by default (`Core/modules/spatial_pipeline/spatial_pipeline.h:19-20`; the CMake comment confirms CWD-relative referencing, `CMakeLists.txt:126-128`) — must become exe-relative or embedded SPIR-V for the splash binary. The font defaults to an absolute host path (`include/Clouds/SpatialScene.h:35`) but **degrades gracefully, verified**: `SpatialFont::loadFromFile` falls back to a built-in atlas when FreeType or the file is missing (`spatial_font.cpp:28-41`), so the splash needs no font file on disk.

Deliverable: a `SplashMain` composition (offscreen NovaGraphics + TextureBridge + SpatialPipeline + SpatialScene + splash nodes) that is also the permanent process entry — the session stage is *added to* it, never a different binary. This is the modularity dividend: if `SplashMain` cannot be built, the layering is wrong somewhere, and the splash finds it early.

### B.4 Boot-path honesty — the crash-loop cost

Holding DRM master from early boot puts Vazio on the critical boot path: a splash crash loop blocks the machine. **Mitigation is a Core/rc-script concern with Vazio's cooperation — Vazio must not implement watchdog policy alone:**
- *Core/rc owns:* restart limits, fallback-to-console/getty after N failures, whether a watchdog timeout exists and its value.
- *Vazio cooperates by:* (1) distinct exit codes — clean-fallback-requested vs. fatal-error vs. readiness-timeout (B.2 option iii); (2) emitting a liveness heartbeat on a Core-provided channel if Core asks for one; (3) never trapping fatal Vulkan/DRM errors into a silent retry loop — fail fast and loudly so rc can act; (4) releasing cleanly on VT switch (the `wlr_session` already drops DRM master when inactive — `session.h:113-116`).

---

## D. DRM/QEMU target

### D.1 Backend flip: autocreate-primary

**Current state (re-verified):** headless is created *unconditionally* with autocreate as fallback (`src/Clouds/SpatialCompositor.cpp:54-61`), discarding the session out-param (`wlr_backend_autocreate(event_loop_, nullptr)` at `:56`); a virtual 1600×1000 output is added when headless (`:96-98`; `wlr_headless_add_output` decl `/usr/include/wlroots-0.19/wlr/backend/headless.h:25-26`). Backwards versus the boot target.

**Target:**
```
wlr_session *session = NULL;
backend = wlr_backend_autocreate(loop, &session);   // backend.h:64-65; "always returns a multi-backend" (:55);
                                                    // session populated "if any" (:57-59) — non-NULL on DRM, NULL nested
if (!backend && explicitly_requested_headless) backend = wlr_headless_backend_create(loop);   // headless.h:19
```
- Nested (dev): autocreate under `WAYLAND_DISPLAY`/X11 picks the wayland/x11 backend — the automatic dev mode. Env-var overrides: **VERIFIED empirically 2026-08-31** — `WLR_BACKENDS=headless` is honored by `wlr_backend_autocreate` in 0.19.3 (`backend.c:342 "Loading user-specified backends due to WLR_BACKENDS"`), returning a multi-backend with `session=NULL`, identical in effect to calling `wlr_headless_backend_create` directly. The harnesses' env-based safety mechanism is sound.
- Headless: demoted to explicit `--headless` (CI/tests).
- DRM (TTY): autocreate creates the `wlr_session` (libseat underneath) + DRM + libinput backends. **No new link dependencies**: wlroots' .pc has `have_drm_backend=true have_session=true have_libinput_backend=true` with `libseat >= 0.2.0` in `Requires.private`, and `libwlroots-0.19.so` links `libseat.so.1`/`libinput.so.10`/`libudev.so.1` internally (ldd-verified). CMakeLists needs nothing added.
- Keep the session pointer: VT switches arrive on `session.events.active` (`session.h:59-63`); minimum handling = pause rendering while inactive (required at splash already, §B.1).

**Outputs across backends:** DRM outputs arrive via `backend.events.new_output` from connectors; nested outputs must be created (`wlr_wl_output_create`, `wayland.h:38`; backend "created with no outputs", `wayland.h:13-14`); headless via `wlr_headless_add_output`. Existing `onNewOutput` (`SpatialCompositor.cpp:170-185`) already does `wlr_output_init_render` + enable + commit, but its mode logic is wrong for DRM: `output->current_mode` else custom 1600×1000 (`:178-182`) — on a cold connector use `wlr_output_preferred_mode` (`/usr/include/wlroots-0.19/wlr/types/wlr_output.h:318`); an unsupported custom mode will fail to commit.

**Interim bridge (keeps SDL alive until D.2):** autocreate provides real input but zero outputs on nested; client tolerance of a zero-`wl_output` world is client-dependent — **UNVERIFIED**. Bridge: add a headless *companion* backend into the returned multi-backend (`wlr_multi_backend_add`, `multi.h:23-24`, before `wlr_backend_start`) hosting the virtual output matching the SDL window. Scaffolding, deleted at D.2.

### D.2 The presentation question — now under dual duty (splash + session)

**The conflict:** Nova presents through SDL — `SDL_Vulkan_CreateSurface` (`Core/nova_graphics.cpp:16`) → swapchain → `vkQueuePresentKHR` (`:504`); the window comes from `Nova` (`Nova.cpp:36-46`). On a TTY, `SDL_Init(SDL_INIT_VIDEO)` has no display server; the constructor path dies before wlroots is reached. wlroots' DRM backend owns modesetting. §B adds the selection criterion: **whichever path is chosen must serve both the splash stage (zero clients, earliest boot) and the session stage, on real DRM and in QEMU-without-venus (lavapipe).**

**Option A — Nova presents directly (`VK_KHR_display` or hand-rolled KMS).** Fails dual duty three ways: (1) lavapipe is a CPU rasterizer with no display hardware — `VK_KHR_display` there is structurally implausible (**UNVERIFIED** formally; verify `vulkaninfo` in the guest — but do not bet the boot path on it), so QEMU-without-venus needs a *second* mechanism; (2) no nested analogue — dev mode needs a third path; (3) Vazio reimplements modesetting/hotplug/VT — machinery wlroots has (`session.h:32-37`) and the splash needs on day one. Rejected.

**Option B — Vazio renders into wlroots' output via its allocator/swapchain (chosen; dual duty is the second independent argument).** Per frame, on the output `frame` event (`wlr_output.h:193-194`): `wlr_output_configure_primary_swapchain(output, NULL, &swapchain)` (`wlr_output.h:603-613`) → `wlr_swapchain_acquire` (`/usr/include/wlroots-0.19/wlr/render/swapchain.h:42`; slots are `wlr_buffer`s, `:10-30`) → `wlr_buffer_get_dmabuf` (`wlr_buffer.h:92`; attrs `render/dmabuf.h:34-46`: per-plane fd/stride/offset + modifier) → import into Nova as `VkImage` (`VK_KHR_external_memory_fd` / `VK_EXT_external_memory_dma_buf` / `VK_EXT_image_drm_format_modifier`, present in `/usr/include/vulkan/vulkan_core.h:10518,15944,16741`) → render → sync (bring-up: `vkWaitForFences`; later: explicit sync via syncobj timelines — backend advertises `features.timeline`, `backend.h:38-41`) → `wlr_output_state_set_buffer` (`wlr_output.h:522-529`) → `wlr_output_commit_state` (`:364`) → clients' `frame_done` after the `present` event (`:205-207`; the splash has no clients — the same loop simply has no frame_done consumers).
*Dual-duty check:* outputs exist with zero clients, so the splash uses this path unmodified; nested uses it identically against a `wlr_wl` output — meaning **the splash scene is testable in a window during development**.
*Costs, honestly:* (1) **NovaGraphics offscreen mode** — the largest single Nova change: `renderFrame` is hardwired to acquire/present (`nova_graphics.cpp:402-511`), the render pass to the swapchain format (`:286-323`); needs render-to-external-image, a DRM-fourcc→VkFormat table, and instance creation without SDL extensions (the `need_surface_extensions` split exists, `core_base.cpp:53,78-82`; an external-`VkSurfaceKHR` ctor exists, `nova_graphics.cpp:40-66` — precedent, not sufficiency). (2) **Device match**: imported dmabufs must come from Nova's device — match `wlr_backend_get_drm_fd` (`backend.h:82`) against `VkPhysicalDeviceDrmPropertiesEXT` (`vulkan_core.h:20445-20454`). (3) **Fallback when import fails** (lavapipe/venus quirks): CPU copy into a mappable output buffer — **spike-verified 2026-08-31 (see Least-sure item 2): NOT viable on gbm buffers (structural), fully viable via a public-API pixman sidecar renderer whose shm swapchain the output accepts.** The fallback is therefore an explicit up-front renderer/allocator choice, not a runtime probe. On real DRM (backend rejects SHM) the mappable analogue is the non-public dumb allocator — resolve at the QEMU/TTY run. Adopting wlroots' Vulkan renderer's device (`wlr_vk_renderer_get_device`, `render/vulkan.h:21-25`) is noted and rejected: wrapping Nova around a foreign `VkDevice` is more invasive than importing buffers.
**Consequence: SDL leaves the server path entirely at D.2** — window, input (§1.2), and the ImGui overlay (dev scaffolding; disable under DRM or port later — user decision).

### D.3 Nova device selection — preference + report

Current (re-verified): first-suitable-device wins, zero preference logic (`Core/core_base.cpp:271-276`); the chosen *name* is reported (`:283-285`) but not the type. The suitability gate is weaker than it looks: `isComplete()` (`Core/modules/atomic/atomic.h:100-102`) compares `std::optional<unsigned int>` fields against 0, and every field is *initialized engaged* with `-1` wrapped to `UINT_MAX` (`atomic.h:95-98`), so each `field >= 0` was unconditionally true. **Fixed in the working tree 2026-08-31 (uncommitted):** sentinels removed, `isComplete(bool needs_present)` with honest `has_value()` gates (audit-current lines: `core_base.cpp:273/:282`, `device.cpp:185/:198` — the queue files are still moving), per-candidate `reset()` so rejected devices no longer leak partial state into the next candidate, and the logical-device family set now includes `present_family` — previously `vkGetDeviceQueue` on the present family (`nova_graphics.cpp:28`) was invalid whenever present ≠ graphics; latent bug, also fixed. (Graphics-mode extension check `:246-252` did real work throughout; compute-mode suitability was the vacuous half.)
Required: score devices `DISCRETE_GPU > VIRTUAL_GPU > INTEGRATED_GPU > CPU` (venus enumerates VIRTUAL; lavapipe CPU), tie-break by the D.2 DRM-node match; log `deviceName + deviceType + driver` unconditionally at INFO, loud warning when the winner is CPU (a splash silently on lavapipe when venus exists is a misconfiguration the log must surface). Fix `isComplete()` to `has_value()` semantics after removing the `= -1` sentinel engagement.

### D.4 QEMU bring-up checklist (first boot = the splash, not a client)

Host facts (verified on this machine): `qemu-system-x86_64` 11.0.2 at `/usr/bin/`; **no virtio-gpu device installed** — `-device virtio-gpu-pci`/`virtio-gpu-gl-pci` both report "not found"; the hw-display module set contains only `hw-display-qxl.so` (other `/usr/lib/qemu/` modules are UI/bridge helpers); the built-in display-device list (ati/bochs/cirrus/qxl/vmware/VGA variants) includes no virtio-gpu device. Host Vulkan ICDs: `radeon_icd.json` only.

0. **Host:** install QEMU virtio-gpu modules (Arch: `qemu-hw-display-virtio-gpu`, `-gl`); re-probe `-device virtio-gpu-gl-pci,help` for `venus`/`blob`/`hostmem` properties — flag syntax on this build **UNVERIFIED** until the module exists.
1. **Stepping stone available today:** `-device bochs-display` (verified present) is driven by the kernel's bochs KMS driver — exercises the DRM backend + lavapipe/CPU-copy path with zero new host packages. De-risk D.2 and the splash here first.
2. **Guest image** (Linux guest; the mfsBSD/FreeBSD base is Core's project — nothing below verified for FreeBSD, **UNVERIFIED**): kernel with `CONFIG_DRM_VIRTIO_GPU` (or `CONFIG_DRM_BOCHS` for step 1); no `nomodeset`; mesa `vulkan-swrast` (lavapipe) minimum, `vulkan-virtio` (venus) when enabled; `seatd` (host has `/usr/bin/seatd`, service disabled; systemd-logind also present — either satisfies libseat) with seat-group membership; `XDG_RUNTIME_DIR` set; **Vazio's splash assets reachable at its early-userspace CWD or embedded (§B.3.7)**.
3. **Verify in order, in the guest:** (a) `/dev/dri/card0` exists; (b) `vulkaninfo --summary` — which ICDs, which `deviceType`; (c) run `vazio` from the TTY: log shows session acquired + DRM output on `new_output` + D.3 device report; (d) **the splash milestone: animated scene on the connector, zero clients** (Phase 3a exit); (e) readiness token → in-engine morph to session stage; (f) `WAYLAND_DISPLAY=… foot` from the opened socket shows live pixels.
4. **Sync caveat under venus:** implicit-sync assumptions that hold on radv may not hold on venus; if corruption appears, force `vkWaitForFences`-before-commit (D.2) — correctness first. **UNVERIFIED** until run.

---

## 3. Sequencing — revised: the session-shaped cut, then the splash-shaped reorder

**Tension 1 (multi-session vs. client-first), resolved:** land Phase 1 *inside a session boundary that already structurally exists* — `SpatialCompositor` is instance-shaped today (§S.1) — and make only decisions P1.a–e below early. Everything else about sessions defers to Phase 4 without rework, because Phase 1's new code touches one session and communicates through the P1 seams.

**Tension 2 (splash vs. client-first), resolved:** the splash milestone — *a scene on bare DRM with zero clients* — is strictly simpler than a client on DRM and derisks exactly the Nova-presents-without-SDL question, so on the **DRM track it precedes first-client**. It does not displace nested-track Phase 1, which remains the highest-information milestone for the *protocol* axis (configure/map/import are untouched by presentation work). Two tracks, different risk axes, explicitly parallelizable.

**Pre-Phase-1 session decisions (P1.a–e):**
- **a.** `SpatialCompositor` *is* the LocalSession-to-be: no new globals; `wlr_log_init` moves to `main` (one line).
- **b.** Mapped surfaces insert under an injected *portal root node*, not `scene_->root` (today `:254`) — one ctor parameter now, Desktop/Portal later without listener rewiring.
- **c.** New cross-boundary bookkeeping keyed by per-session `u32` counter handles, never raw pointers (internal `shared_ptr` may stay until Phase 4).
- **d.** Socket naming from one place (already flows `main` → `startServer` with auto fallback, `SpatialCompositor.cpp:118-128`); `startServer` **splits into `startSubstrate()` / `open()`** per §S.6 — this is also the splash enabler, so it is a P1 decision, not a Phase 4 one.
- **e.** `frameDone`/presented-time through one `Session::onFramePresented(t)` hook (call site moves once at D.2). Ratify S.5 on paper now; implement Phase 4.

Deferred with justification: the slot map (still benefits from observed behavior), type erasure, Desktops/Portals as code, second-session runtime, remote transport (blocked on Precipitation's existence — no Precipitation code in tree, grep-verified).

**Phases:**
- **Phase 0 — stability floor.** 0.1 deferred destroy (`SpatialCompositor.cpp:27-31`/`:261-272`) — gates everything. 0.2 frame-indexed mesh buffer + deferred growth (`SpatialScene.cpp:239`; `spatial_mesh.cpp:202-207,242-244`). 0.3 static-mesh caching. 0.4 resize honesty: `recreateSwapchain()` is a log-only no-op (`nova_graphics.cpp:524-528`) yet sole handler for `VK_ERROR_OUT_OF_DATE_KHR` (`:418-420`) and present-time resize (`:505-508`); the window is `SDL_WINDOW_RESIZABLE` (`Nova.cpp:37`) while the Clouds loop handles no `SDL_WINDOWEVENT` (`clouds_server_main.cpp:137-232`) and passes fixed `config.screen` as extent (`:171,:250`) — resize today = frozen loop. Implement or make non-resizable; don't ship the silent path. Adjacent: per-image semaphore arrays allocated (`nova_graphics.cpp:382-388`) but only `[0]` used (`:413,478,480`).
- **Phase 1 — one client visible (nested track)** + P1.a–e. 1.1 initial_commit→set_size; 1.2 map/unmap + insertion-on-map under the portal root; 1.3 SHM import → persistent-texture update → `setTexture`; 1.4 frame_done via P1.e. SHM first; dmabuf behind a probe. *Done when:* `WAYLAND_DISPLAY=wayland-clouds-0 foot` shows live pixels (socket default `SpatialCompositor.h:81`).
- **Phase 2A — usable interaction (transport-independent).** Nearest-hit `hitTest` + `captures_subtree_input`; delete the `is_focused` shadow; pointer grab; split foci; `xdg_popup` (signal exists unbound — `wlr_xdg_shell.h:28`; only `new_toplevel` bound, `SpatialCompositor.cpp:92`); the disposable SDL scancode table only if typing demos precede 2B. *Done when:* click into a client, type, drag by titlebar.
- **Phase 2B — backend flip (D.1).** Autocreate-primary + session out-param + VT-active handling; headless companion for the interim SDL window; `new_input` (real keymaps; delete calloc'd keyboard `SpatialCompositor.cpp:104-116`; pointer accumulator); dynamic seat caps; `wlr_output_preferred_mode` in `onNewOutput`.
- **Phase 3a — splash stage (DRM track; §B).** `SplashMain`: offscreen NovaGraphics + Option-B present path + boot Desktop + latent Session[phys]; asset decoupling (§B.3.7); B.4 exit-code/heartbeat cooperation; bochs-display QEMU run (D.4.1). *Done when:* D.4.3(d) — animated scene on the connector, zero clients, in QEMU and on the dev machine's TTY. **Deliberately the smallest possible exercise of D.2 — presentation minus clients minus input.**
- **Phase 3b — session stage on DRM.** `open()` on readiness token (B.2); clients on the flipped stack; SDL and the ImGui overlay exit the server path; full D.4 checklist through (f); virtio-gpu/venus after bochs.
- **Phase 4 — session substrate.** Slot map + `NodeId`/`WireHandle`; Registry/Desktop/Portal real; `g_Theme` → Desktop; type-erased facets; collapse `Primitives.h`'s four classes (`include/Clouds/Primitives.h:11,26,58,79`) into presenters; retire `SpatialClusterIndex` + retarget the seam test; **two-sessions-in-one-process unit test** (S.2's verification).
- **Phase 5 — Precipitation seam.** PropertyBag/registerPresenter/present/update/retract; YAML both ways; `describeCapabilities`; remote-session contract (S.3) against Precipitation's first transport; readiness channel formalized as that channel's first message (B.2); hybrid Desktop demo (one local + one remote Portal, one scene).
- **Phase 6 — shell & apps.** Theme/Layout from YAML; Lua over the Registry surface; calculator complete; monitor (measured only); messenger (honest empty state); settings (bound to `EnginePhysicsConfig`).

---

## 4. Disposition of the 1,563 LOC

Line counts re-verified (`wc -l`): UIComponents 380, UIWindow 162, UIMenuBar 177, UIDockBar 68, WindowManager 249, EngineHUD 268, DynamicSceneManager 153, CrmWorkspace 106. Tree-wide fact strengthening every verdict: **all are compiled into `Clouds` but `WindowManager`, `EngineHUD`, `CrmWorkspace`, `DynamicSceneManager` are never instantiated anywhere** (no constructor call sites in `src/`+`examples/`, grep-verified) — dead weight in the running binary today.

| File | LOC | Verdict (v2) | Reasoning — deltas from v1 only |
|---|---|---|---|
| `UI/UIComponents.cpp` (+`.h`) | 380 | **KEEP → mine** | Widget vocabulary (`UIComponents.h:29,52,91,112,129`); variant→color map `:103-128` re-verified. Theme refs go per-Desktop in Phase 4. Splash note: available to the splash scene but not required by it. |
| `UI/UIWindow.cpp` (+`.h`) | 162 | **KEEP → fix two bugs** | Only 3D drag in tree (`:128-154`), correct, blocked solely by `hitTest`. Delete `is_focused` shadow (`UIWindow.h:17`); `updateChromeLayout()` declared (`UIWindow.h:57`), never defined — remove or write. |
| `UI/UIMenuBar.cpp` | 177 | **KEEP → mine** | Dropdown anchoring/category layout `:101-142`. Delete seed `:88`; `setTelemetryText` (`:165-169`) stays, fed real values. |
| `UI/UIDockBar.cpp` | 68 | **KEEP** | `addItem` cursor-advance layout `:24-43` — the anchored-layout primitive. |
| `UI/WindowManager.cpp` | 249 | **MINE → drop the shell** | `addWindow/bringToFront/toggleWindow` (`:174-206`) extracts into **Portal/Desktop** (per-Desktop focus order), not a process Session as v1 said. The two hardcoded depths (`-0.04f/-0.12f`, `:191`) confirm the settled pushback. Delete `createPhysicsWindow` (`:73`) / `createHypergraphWindow` (`:145`) — which-windows-exist is shell policy; the hypergraph one is fabricated (`:149`,`:163`). |
| `EngineHUD.cpp` | 268 | **DROP** | Physics window written twice, raw `SpatialButton`s (`:29,95,107,129`…), own constants, seeds `:43,:77`, unrenderable emoji `:43,:249`. Preserve the λ/ω/mode/dither control set *as data*. |
| `DynamicSceneManager.cpp` | 153 | **MINE hard** | `removeStaleNodes` (`:123-140`) = authoritative-removal+safety-net — the presenter-registry reconciliation *and* the session-epoch-death cleanup (S.5). `syncNodesFromState` (`:62-76`) survives near-verbatim. `createNodeFor` (`:78-111`) → `spatial.pill` presenter. |
| `CrmWorkspace.cpp` | 106 | **DROP** | Honest (`:85-104`: real counts, surfaces `isHealthy()` failures) but domain-named and redundant with `ImGuiEngineOverlay::renderOatsWindow` (`ImGuiEngineOverlay.cpp:335`). Salvage empty-state strings `:37`,`:62` for the messenger. |

Net unchanged: ~1,000 LOC survives as material; ~374 deleted outright; ~200 deleted as misplaced policy.

---

## 5. Honest cost

**Genuinely large:**
- **Nova offscreen/presentation mode (D.2, exercised first by the splash)** — №1. Touches instance/device creation (`core_base.cpp:53-91,256-331`), render-pass/format handling (`nova_graphics.cpp:286-323`), the whole frame loop (`:402-511`), dmabuf import, fourcc→VkFormat table. The SDL exit strands the ImGui overlay — decide disable-vs-port before 3b.
- **Slot-map/handle refactor (Phase 4)** — every `addChild`/`shared_ptr<SpatialNode>`/`weak_ptr parent` across `SpatialNode`, `Primitives`, `SpatialScene`, `SpatialCompositor`, `SpatialFilesystem`, `DynamicSceneManager`, five UI files.
- **DMA-BUF client import** — machinery LANDED 2026-09-01 (Track A): external-memory extensions, modifier negotiation, multi-plane import, device scoring. **Two corrections from measurement:** the "trio" is a **quartet** — `VK_EXT_queue_family_foreign` is required, not polish (DCC-modifier dmabufs read back wrong pixels without a release to `QUEUE_FAMILY_FOREIGN_EXT`; proven with a DCC-fails/LINEAR-passes negative control); and `DISJOINT` keys on **buffer count, not plane count** (AMD DCC = 2 planes, 1 dmabuf; fds compared by st_dev/st_ino). `DRM_FORMAT_MOD_INVALID` has no Vulkan import path — refused explicitly. Client-side wiring (commit path → import) remains for 3b.
- **Being on the critical boot path (B.4)** — not a code cost but an operational one: Vazio inherits boot-reliability obligations (exit-code discipline, heartbeat cooperation, fail-fast norms) a mid-session compositor never carries. Named so it is budgeted, with policy owned by Core/rc.
- **Lua bindings + rollback discipline; multi-window correctness** (subsurfaces, popups, clipboard/DnD beyond `wlr_data_device_manager_create` at `SpatialCompositor.cpp:84`).

**Deceptively small:**
- `frame_done` and initial-configure: one call each; placement is the whole game (§1.1).
- Frame-indexing the mesh buffer: LANDED — `getCurrentFrameIndex()` now public at `nova_graphics.h:96`; the privacy-boundary concern is moot.
- The pointer accumulator (D.1): ~10 lines, but it replaces SDL as the source of truth for cursor position — every camera binding in `clouds_server_main.cpp:169-231` moves with it at 3b.
- The splash *scene itself* is small (nodes + animation on existing machinery — `evolvePhase` already animates, `SpatialNode.h:47-49`); the splash *stage* is not (it drags D.2 in). Do not confuse the two when estimating.
- `startServer` split (§S.6): LANDED as a decomposition (`initBackendStack`/`initProtocols`/`initInput`/`initSocket`, uncommitted); the remaining work is exposing `latent`/`open` as the two-phase public API over it. The risk is ordering assumptions between the halves, not volume.
- Newlines/UTF-8 in `spatial_font`: `\n` ~10 lines; UTF-8 = rekey `glyphs_` (`spatial_font.h:51`) to `char32_t` + decoder + on-demand rasterization — a day, not an hour.
- Session epochs (S.5): a counter and a compare now; a protocol migration if retrofitted after Precipitation ships — why the scheme is decided in this document.

---

## M. Modes (added 2026-09-01 — user-defined, OATS-designed)

Ratified by the user, with the correction that this mapping was **designed into OATS from the start** — its three registered types are the mode taxonomy, not incidental demo data. Presenters still claim types generically by name (§1.4 mechanics unchanged), but the taxonomy is product structure: do not genericize the three types away.

| Mode | What it is | Substrate (exists) | New to build |
|---|---|---|---|
| **explorer** | Filesystem navigation | `SpatialFilesystem` tree + OATS `FileSystemEntity`; scan moves behind Precipitation per §1.4 | Presenter-ization only |
| **sandbox** | Primitive 3D modeler/sculpter | Pill/box/plane generators + `MeshCache`; drag-on-plane (grab + re-cast) is a gizmo's core; objects persist as OATS `SpatialPill` traits via Precipitation | Tool controller: spawn/scale/rotate gizmos, snapping, undo |
| **canvas** | Window-as-canvas: notes + graphs | `updateTextureFromRGBA` is a canvas rasterizer pointed the other way (draw strokes/text into RGBA, push through the client-pixel path); graphs = OATS `HypergraphDAGNode`; `children_pills` edges exist unread | Stroke/text tools, edge rendering; **pulls UTF-8/newline font work forward** |
| **portal** | Remote sessions to other Clouds OS portals | IS the architecture: §S.3 contract + the Portal binding | Precipitation transport — but the **first portal is loopback**: a second local session (§S.2) bound into the Desktop, proving the mechanism with zero remote code. The Phase-4 two-session test, promoted to a feature. |

**Mode model (proposal awaiting ratification):** a mode = a Desktop plus a bound toolset — same scene substrate, different input interpretation and presenters; switch by Desktop swap or coexist as regions; hybrid free (a portal open inside explorer).

**Sequencing impact:** Phase 4's substrate is *also* the mode framework. After Phase 4: canvas (maximal reuse, forces UTF-8) → explorer (mostly exists) → sandbox (gizmos over existing generators) → portal (loopback in Phase 4/5, remote at Phase 5 proper). The windowing-completeness pack (clipboard/DnD, subsurfaces, xdg-toplevel requests, decorations, popup grab, protocol grab-bag) parallelizes alongside Phase 3.

**Decisions added to the list:** (9) XWayland in or out; (10) ratify the mode model above.

---

## 6. Carried pushbacks — now settled

v1 §6 (z-order ill-defined in 3D; expose `focus()`+`setTransform`, no `raise()`) — **accepted; executed** in §1.1 and §4. The singleton pushback — superseded by §S.1's four-way split. The splash direction (staged single-process, not Plymouth) — **accepted; executed** in §B/§S.6/§3.

---

## Least sure about, ranked

1. **Venus/dmabuf behavior in the QEMU guest** (D.2 sync, D.4.4) — nothing verifiable from this host (only `radeon_icd.json`; no virtio-gpu QEMU device until modules installed — verified absent). Verify: D.4.0-3. Guaranteed interim path: bochs-display + CPU copy.
2. **CPU-copy fallback — SHARPENED by spike 2026-08-31 (notepad 58bfa0df), partially resolved.** Measured on this machine: `wlr_buffer_begin_data_ptr_access(WRITE)` is a **structural hard-no on any gbm-allocated buffer** (the gbm allocator's caps simply lack `DATA_PTR`; fails 120/120 under both gles2 and vulkan renderers, no errno). It **succeeds 120/120 on the shm path** (pixman renderer → shm allocator, XR24, stride exactly width×4, gradient written and readback-verified in scanout storage). Allocator *selection order* (gbm → shm → drm-dumb → udmabuf, driven by `render_buffer_caps ∩ backend buffer_caps`) is **not public API** — the concrete allocator constructors are exported symbols but unheaderered. **The public-API escape, verified:** a **pixman sidecar renderer** (`wlr_pixman_renderer_create` IS public) fed to `wlr_allocator_autocreate` yields a mappable shm swapchain that `wlr_output_test_state`/`commit_state` ACCEPT even on an output initialised with gles2. Design consequence: Phase 3a makes an explicit renderer/allocator *choice* at splash time — the outcome is fully determined by `render_buffer_caps` and knowable before the first frame — rather than attempting `begin_data_ptr_access` at runtime with a fallback. Bonus fact: `wlr_renderer_autocreate` falls back to pixman automatically when no DRM render node exists, no env needed. **Still open, needs the QEMU/TTY run:** a DRM backend advertises DMABUF only (rejects SHM), so the DRM-side analogue is the non-public dumb allocator — whether 3a uses that (ABI risk) or another route on real DRM is the remaining decision.
3. ~~The socketless-display splash substrate~~ **RESOLVED by spike 2026-08-31 (notepad 58bfa0df): PASS.** 240/240 output commits on a socketless `wl_display`, zero error logs, deterministic across 3 runs. Un-reachability proven two ways: empty `XDG_RUNTIME_DIR` AND a `/proc/self/fd` walk showing **zero socket descriptors of any kind** during the latent phase. The pre-compositor registry holds **zero globals** — backend+renderer+allocator+output bring-up creates nothing on the display; `wl_shm`/`linux-dmabuf` arrive only with `wlr_renderer_init_wl_display`, `wl_output` only with `wlr_output_create_global`. The latent→open flip works on one continuous display: socket added mid-process, exactly four globals appear (`wl_compositor v6`, `wl_subcompositor v1`, `xdg_wm_base v3`, `wl_seat v9`), a client connects and receives `xdg_surface.configure`, and 120 further frames commit on the same output. §S.6 stands confirmed by measurement.
4. **Hidden process-global state under two live `wl_display`s** — API-verified clean (S.2); only the Phase 4 two-session test proves it. Also **UNVERIFIED**: autocreate env-var semantics; wlroots thread-affinity rules (docs, not headers).
5. **Client tolerance of zero `wl_output`s** in interim nested mode (D.1) — client-dependent; the headless-companion bridge ducks it. Verify empirically if the bridge is dropped.
6. **Cross-device DMA-BUF client import** (v1's №1, still live): `wlr_renderer_autocreate` (`SpatialCompositor.cpp:63`) and Nova pick devices independently today; D.3 matching reduces, does not eliminate. SHM ships regardless.
7. **Flat `PropertyBag` expressiveness** (carried) — expect pressure within three presenters; and **remote-session input latency** over Precipitation (S.3b) — slow round-trips will tempt local hover echo, which flirts with Vazio deciding things.

## Decisions that need the user (vs. settled)

**Settled (by direction, not revisited):** layering; Vazio-as-WindowServer; wlroots as transport; Core/Precipitation split; YAML at the boundary; OATS = registry+types; plural Sessions; `focus()`+`setTransform`, no `raise()`; staged single-process splash with one DRM master acquisition.

**Needs the user:**
1. **Ratify S.1 vocabulary** — Session/Desktop/Portal definitions and the `latent→open` Session lifecycle (they become Precipitation schema).
2. **Ratify S.5 handle scheme** — u64 wire handle, epoch rules, `SessionId 0` reservation.
3. **Confirm Option B** (render into wlr_output; SDL exits the server path at Phase 3b) and the ImGui overlay's fate (disable under DRM vs port).
4. **Splash readiness-failure policy (B.2)** — (i) stay on splash with honest status, (ii) timeout→open empty session, (iii) timeout→exit for Core's rc fallback; and whether a timeout exists at all.
5. **Splash→session visual policy** — morph Portals within one Desktop vs swap Desktops; transition animation ownership (theme/Precipitation). Mechanism supports both (§S.6).
6. **Guest OS for first QEMU boot** — Linux guest per D.4, mfsBSD/FreeBSD deferred to Core: confirm.
7. **YAML mandate scope** — declarative-only (recommended) vs both channels (§1.4).
8. **Phase 2A's disposable SDL scancode table** — write it (typing demos sooner) or skip to 2B (less throwaway).

---

### Critical files for implementation
- `src/Clouds/SpatialCompositor.cpp`
- `Core/nova_graphics.cpp`
- `examples/clouds_server_main.cpp`
- `src/Clouds/SpatialScene.cpp`
- `Core/modules/spatial_pipeline/texture_bridge.cpp`
