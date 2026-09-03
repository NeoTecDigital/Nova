// Written by Richard Christopher, Copyright 2026 NeoTec Digital
#pragma once

#ifndef WLR_USE_UNSTABLE
#define WLR_USE_UNSTABLE
#endif

#ifdef __cplusplus
extern "C" {
#endif
#include <wayland-server-core.h>
#include <wlr/backend.h>
#include <wlr/render/wlr_renderer.h>
#include <wlr/render/allocator.h>
#include <wlr/types/wlr_buffer.h>
#include <wlr/types/wlr_compositor.h>
#include <wlr/types/wlr_subcompositor.h>
#include <wlr/types/wlr_data_device.h>
#include <wlr/types/wlr_primary_selection.h>
#include <wlr/types/wlr_primary_selection_v1.h>
#include <wlr/types/wlr_xdg_decoration_v1.h>
#include <wlr/types/wlr_xdg_shell.h>
#include <wlr/types/wlr_seat.h>
#include <wlr/types/wlr_output.h>
#include <wlr/types/wlr_cursor.h>
#include <wlr/types/wlr_keyboard.h>
#include <wlr/interfaces/wlr_keyboard.h>
#include <wlr/backend/headless.h>
#include <wlr/backend/multi.h>
#include <wlr/backend/session.h>
#include <wlr/types/wlr_input_device.h>
#include <wlr/types/wlr_pointer.h>
#include <wlr/util/box.h>
#include <wlr/util/log.h>
#include <xkbcommon/xkbcommon.h>
#ifdef __cplusplus
}
#endif

#include "./SpatialHosts.h"
#include "Splash/SpatialScene.h"
#include "Splash/Primitives.h"
#include <cstddef>
#include <cstdint>
#include <ctime>
#include <memory>
#include <string>
#include <type_traits>
#include <vector>

namespace Vazio {

class SpatialCompositor {
public:
    /**
     * @param portal_root Scene node mapped client surfaces are inserted under.
     *        Injected rather than assumed so a Desktop/Portal can be introduced
     *        later without rewiring a single listener. Falls back to the scene
     *        root when null, which is what a single-Desktop session wants.
     */
    SpatialCompositor(Nova::Core* core,
                      Nova::TextureBridge* texture_bridge,
                      std::shared_ptr<Splash::SpatialScene> scene,
                      std::shared_ptr<Splash::SpatialNode> portal_root = nullptr,
                      SpatialCompositorConfig config = {});
    ~SpatialCompositor();

    SpatialCompositor(const SpatialCompositor&) = delete;
    SpatialCompositor& operator=(const SpatialCompositor&) = delete;

    // Latent (SessionStage, plan S.6): display, backend, session, renderer,
    // allocator and outputs live and committing, zero sockets, zero globals.
    // Unreachable by construction - nothing is listening.
    bool startSubstrate();

    // latent -> open on the SAME display: socket, protocol globals, seat,
    // listeners, and a wl_output global per already-committed output.
    bool open(const std::string& socket_name = "wayland-clouds-0");

    // Substrate + open in one call: the nested entry point's whole startup.
    bool startServer(const std::string& socket_name = "wayland-clouds-0");

    SessionStage stage() const { return stage_; }

    // The presentation loop's output hook; replays over committed outputs.
    void setOutputReadyHandler(OutputReadyHandler handler);

    // Process Wayland event loop iteration
    void iterateEventLoop(int timeout_ms = 0);

    // Stop and cleanup. Idempotent: every resource is released and nulled.
    void stop();

    struct wl_display* getDisplay() const { return wl_display_; }
    const std::string& getSocketName() const { return socket_name_; }

    /**
     * Whether this session currently owns the seat's devices and outputs.
     *
     * False only between a VT switch away and the switch back: wlr_session
     * revokes the DRM master and the input fds for the duration, so committing
     * an output would fail and rendering into one would be wasted work. True
     * whenever there is no session at all - nested and headless backends are
     * never suspended, and a caller that gates rendering on this must not stall
     * because no one is managing VTs.
     */
    bool isSessionActive() const { return session_active_; }

    // Non-null only when the backend acquired a libseat session, i.e. on DRM.
    struct wlr_session* getSession() const { return session_; }

    // Extent of the output pointer positions are clamped to. Falls back to the
    // configured virtual extent until an output has committed a mode.
    const struct wlr_box& outputBox() const { return output_box_; }

    // Current pointer position in output pixels, after clamping. The reticle is
    // drawn from the raycast this feeds, not from here; this is the state a
    // Portal or a test needs to ask about without replaying the input.
    glm::vec2 pointerPosition() const {
        return glm::vec2(static_cast<float>(pointer_x_), static_cast<float>(pointer_y_));
    }

    // Outputs this session has enabled and committed.
    size_t outputCount() const { return outputs_seen_; }

    // Windows this session hosts, and the subset currently in the scene. The two
    // differ for every toplevel that exists but has not committed a buffer yet.
    size_t windowCount() const { return windows_.size(); }
    size_t mappedWindowCount() const;

    // Popups this session hosts, and the subset currently in the scene.
    size_t popupCount() const { return popups_.size(); }
    size_t mappedPopupCount() const;

    // Subsurfaces this session hosts, and the subset currently in the scene.
    size_t subsurfaceCount() const { return subsurfaces_.size(); }
    size_t mappedSubsurfaceCount() const;

    /**
     * Release the frame callbacks every mapped client is blocked on.
     *
     * Must be called once per presented frame and never from inside the commit
     * handler: a client that gets frame_done during its own commit is being told
     * to draw a frame the compositor has not shown yet.
     *
     * @param when Presentation time on CLOCK_MONOTONIC. The caller supplies it
     *        because the authoritative source moves from "just after the render
     *        submission returned" to the output present event under the DRM
     *        backend, and only the call site should have to change.
     */
    void onFramePresented(const struct timespec& when);

    // Input routing
    void processKey(uint32_t keycode, bool pressed, uint32_t time_ms = 0);
    void focusSurface(struct wlr_surface* surface);

    /**
     * The one pointer-position entry point, in output pixels.
     *
     * Two producers reach it: the interim SDL window, which already knows an
     * absolute position, and libinput pointers, which only ever report deltas.
     * Keeping the absolute position here rather than in either producer is what
     * lets the SDL half be deleted at Phase 3 without the DRM half noticing.
     */
    void processPointerMotionAbsolute(double x_px, double y_px);

    // Accumulate a relative delta onto the pointer position, clamped to the
    // output box. This is the only form libinput devices deliver.
    void processPointerMotionRelative(double dx_px, double dy_px);

    // Pointer buttons in evdev codes (BTN_LEFT and friends) - the currency the
    // seat and the clients both speak.
    void processPointerButton(uint32_t evdev_button, bool pressed);

    // Internal signal handlers
    void onNewXdgToplevel(void* data);
    void onNewXdgPopup(void* data);
    void onNewOutput(void* data);
    void onNewInput(void* data);
    void onNewToplevelDecoration(void* data);

    /**
     * Selection (clipboard) routing. Handing the source straight to
     * wlr_seat_set_selection is the whole policy: wlroots then offers it to
     * whichever client holds keyboard focus, which is the rule every desktop
     * already runs on. Drag and drop (request_start_drag) is NOT implemented
     * here - a drag is a spatial gesture in this session, and inventing its
     * geometry at the transport layer would be inventing policy.
     */
    void onRequestSetSelection(void* data);
    void onRequestSetPrimarySelection(void* data);

    // wl_pointer.set_cursor. Accepted and deliberately not drawn: the 3D
    // reticle IS the pointer here and there is no 2D cursor plane to composite
    // a client's image on. Bound so the policy is stated where the request
    // arrives rather than being an unbound signal nobody can find.
    void onRequestSetCursor(void* data);
    void onFallbackModifiers(void* data);
    void onSessionActive(void* data);
    void onSessionDestroy(void* data);

    // Hand a window to the kill list. Safe to call from inside a wl_listener
    // dispatch: the window is only released by drainDestroyedWindows().
    void removeWindow(WindowHandle handle);

    // Scene insertion / removal driven by wlr_surface map and unmap. Unmapping
    // takes the window out of the scene; it does not destroy it.
    void attachWindowToPortal(SpatialXdgWindow& win);
    void detachWindowFromPortal(SpatialXdgWindow& win);

    // Import the currently committed client buffer into the window's texture.
    void importSurfaceBuffer(SpatialXdgWindow& win);

    // Popup counterparts of the window paths above. Popups anchor to the node
    // their parent surface is drawn on rather than to the portal root.
    void attachPopupToParent(SpatialXdgPopup& popup);
    void detachPopupFromParent(SpatialXdgPopup& popup);
    void placePopupOnParent(SpatialXdgPopup& popup);
    void importPopupBuffer(SpatialXdgPopup& popup);
    void removePopup(WindowHandle handle);

    // Dismiss every grabbing popup the pointer is not inside. xdg-shell breaks
    // an explicit grab on input outside the popup; wlroots covers half of it -
    // its grab ends when a button arrives with no focused client, so a click on
    // empty space works while a click on the grabbing client's own toplevel
    // does not. That second case is the one a menu has to survive, and only the
    // scene knows which node the ray hit.
    void dismissPopupsOutsidePointer();

    // Subsurface counterparts of the popup paths above: no configure of its
    // own, same shape otherwise.
    void onNewSubsurface(struct wlr_surface* parent_surface, void* data);
    void attachSubsurfaceToParent(SpatialSubsurface& sub);
    void detachSubsurfaceFromParent(SpatialSubsurface& sub);
    void placeSubsurfaceOnParent(SpatialSubsurface& sub);
    void importSubsurfaceBuffer(SpatialSubsurface& sub);
    void removeSubsurface(WindowHandle handle);


    // Hand an input device to the kill list. Safe from inside a dispatch.
    void removeInputDevice(struct wlr_input_device* device);

    // Hand an output to the kill list. Safe from inside a dispatch.
    void removeOutput(struct wlr_output* output);

    // Forward a key and the modifier state that goes with it. Both halves are
    // required: a client that gets keys without modifiers never sees Shift.
    void notifySeatKey(const struct wlr_keyboard_key_event& event, struct wlr_keyboard* source);
    void notifySeatModifiers(struct wlr_keyboard* source);

    // Forward a key the scene routed to a focused surface. Separate from
    // notifySeatKey because that one carries a physical device's own event and
    // its keymap; this one carries a keycode the scene resolved and stamps the
    // time itself. Both speak raw evdev - the only keycode space a client
    // understands - and neither invents one.
    void notifySeatSurfaceKey(uint32_t evdev_keycode, bool pressed);

    /**
     * --- wl_pointer event groups ---
     *
     * GROUPING RULE: one wl_pointer.frame closes exactly one logical group,
     * and one logical group is one scene-level input sample. Every entry point
     * below emits the events of a single sample - a ray enter, a ray move, a
     * button transition, a ray leave - and terminates it with one frame. Never
     * one frame per event, never one frame per rendered frame.
     *
     * This is not optional bookkeeping: wl_pointer >= v5 defines frame as the
     * end of a group, this seat advertises v9, and every real toolkit buffers
     * enter/motion/button until the frame arrives. Without it clients receive
     * the events and act on none of them.
     *
     * Every group is closed from here unconditionally, including the two cases
     * where the frame is redundant or discarded - both measured against
     * wlroots 0.19.3 rather than assumed:
     *   - a pointer focus change is framed by wlroots itself, so the enter's
     *     own frame arrives before ours and ours closes an empty group;
     *   - after clear_focus there is no focused client left, and
     *     wlr_seat_pointer_notify_frame drops the frame entirely.
     * An empty group is a no-op every client already tolerates. Suppressing
     * these would make correctness depend on wlroots internals no header
     * documents, and a frame we failed to send is a class of bug this seat is
     * not going to have twice. The frames that carry the whole weight are the
     * motion and button groups - the ones a drag is made of.
     */
    void notifySeatPointerEnter(struct wlr_surface* surface, double sx, double sy);
    void notifySeatPointerMotion(struct wlr_surface* surface, double sx, double sy);
    void notifySeatPointerButton(uint32_t evdev_button, bool pressed);
    void notifySeatPointerLeave();

    // Close whatever group is still open, if any. Bound to a physical pointer's
    // events.frame: hardware reports its own group boundaries, and a producer
    // that emits seat pointer events without closing them is terminated here.
    // A no-op when the group was already closed, so no empty frame is ever sent.
    void notifySeatPointerFrame();

    // Node mapped surfaces are parented to. Never null once a scene exists.
    const std::shared_ptr<Splash::SpatialNode>& portalRoot() const;

private:
    bool initDisplay();
    bool initBackendStack();
    bool createBackend();
    bool initProtocols();

    // Latent-stage input: onNewInput() needs a seat, an open()-stage object, so
    // devices announced during wlr_backend_start() are tracked here instead.
    void onLatentInput(void* data);
    void adoptLatentInput();
    bool ensureVirtualOutput();
    bool initInput();
    bool initKeyboard();
    bool initSocket(const std::string& socket_name);
    void releaseWindows();
    void releasePopups();
    void releaseSubsurfaces();
    void releaseInput();
    void releaseBackendStack();

    // The headless backend hosting the virtual output, or null when real
    // connectors supply the outputs. Either the whole backend (explicit
    // headless), a child autocreate already built (WLR_BACKENDS=headless), or a
    // companion added to the multi-backend for the nested case.
    struct wlr_backend* resolveVirtualOutputHost();

    // Adopt one arriving device onto the seat. Split by type because the two
    // halves share nothing but the device list they land in.
    void attachKeyboardDevice(SpatialSeatDevice& seat_device);
    void attachPointerDevice(SpatialSeatDevice& seat_device);

    // Recompute WL_SEAT_CAPABILITY_* from the devices actually present.
    void refreshSeatCapabilities();

    // Re-point the seat at a live keyboard after the one it held went away.
    void restoreSeatKeyboard();
    bool hasAttachedKeyboard() const;

    // Copy one SHM client buffer into the window texture. Split out so the
    // pointer-access window stays as narrow as the wlroots contract requires.
    bool uploadClientPixels(struct wlr_buffer* source,
                            std::shared_ptr<Nova::TextureHandle>& texture,
                            uint32_t& unsupported_format,
                            WindowHandle handle);

    // Import the committed buffer of any hosted surface into its host node.
    // Toplevels and popups differ in placement, never in how pixels arrive.
    void importSurfaceContent(struct wlr_surface* surface,
                              const std::shared_ptr<Splash::SpatialSurfaceHost>& host,
                              std::shared_ptr<Nova::TextureHandle>& texture,
                              uint32_t& unsupported_format,
                              WindowHandle handle);

    // The scene node a hosted surface is drawn on, or null when the surface is
    // not one this session hosts. Popups anchor to the node this returns.
    std::shared_ptr<Splash::SpatialSurfaceHost> hostNodeForSurface(struct wlr_surface* surface) const;

    // Release everything on the kill lists. Must never be called from inside a
    // wl_listener callback.
    void drainDestroyedWindows();
    void drainDestroyedPopups();
    void drainDestroyedSubsurfaces();
    void drainRemovedInputDevices();
    void drainRemovedOutputs();

    // Release the scene-side half of a hosted surface: out of the tree, out of
    // every focus and grab that names it, texture returned to the bridge.
    void releaseChildHost(const std::shared_ptr<Splash::SpatialSurfaceHost>& host,
                          std::shared_ptr<Nova::TextureHandle>& texture);

    // Place a hosted child quad on its parent's quad from surface-local pixels.
    // The parent's quad spans its local [-w/2, w/2] x [-h/2, h/2] and maps onto
    // the parent surface's pixels, y down; this is that one change of basis.
    void placeChildOnParentQuad(Splash::SpatialSurfaceHost& child,
                                const Splash::SpatialSurfaceHost& anchor,
                                const struct wlr_surface& parent_surface,
                                const struct wlr_box& child_box,
                                float depth_bias);

    // Pick the mode to commit on an arriving output, and adopt the committed
    // size as the pointer clamp box.
    void selectOutputMode(struct wlr_output* output, struct wlr_output_state& state);
    void adoptOutputBox(struct wlr_output* output);

    // Track a committed output and hand it to the presentation loop. No
    // wl_output global here: a global is protocol surface area and the latent
    // stage has none. advertiseOutput() creates it, only once open.
    void trackOutput(struct wlr_output* output);
    void advertiseOutput(struct wlr_output* output);
    void releaseOutputs();

    // Send wl_pointer.frame and close the open group. Every notifySeatPointer*
    // entry point ends here, which is what makes the grouping rule one line
    // rather than a convention four call sites have to remember.
    void closePointerGroup();

    void buildWindowNodes(SpatialXdgWindow& win, struct wlr_xdg_toplevel* toplevel);
    void bindWindowListeners(SpatialXdgWindow& window, struct wlr_xdg_toplevel* toplevel,
                             struct wlr_surface* surface);
    void bindPopupListeners(SpatialXdgPopup& popup, struct wlr_surface* surface);

    // Input routing for a hosted child surface: popups and subsurfaces route
    // identically, so the wiring is written once.
    void bindChildSurfaceInput(const std::shared_ptr<Splash::SpatialSurfaceHost>& host,
                               struct wlr_surface* surface);

    // Popups and subsurfaces are one hosted-child lifetime with two placement
    // rules, so draining and release are written once over the element type.
    template <typename Child>
    void drainHostedChildren(std::vector<std::shared_ptr<Child>>& pending);
    template <typename Child>
    void releaseHostedChildren(std::vector<std::shared_ptr<Child>>& live,
                               std::vector<std::shared_ptr<Child>>& pending);

    Nova::Core* core_ = nullptr;
    Nova::TextureBridge* texture_bridge_ = nullptr;
    std::shared_ptr<Splash::SpatialScene> scene_;
    std::shared_ptr<Splash::SpatialNode> portal_root_;
    SpatialCompositorConfig config_;

    std::string socket_name_;
    struct wl_display* wl_display_ = nullptr;
    struct wl_event_loop* event_loop_ = nullptr;
    struct wlr_backend* backend_ = nullptr;
    struct wlr_renderer* renderer_ = nullptr;
    struct wlr_allocator* allocator_ = nullptr;
    struct wlr_compositor* compositor_ = nullptr;
    struct wlr_xdg_shell* xdg_shell_ = nullptr;
    struct wlr_seat* seat_ = nullptr;
    struct wlr_xdg_decoration_manager_v1* xdg_decoration_manager_ = nullptr;
    struct wlr_primary_selection_v1_device_manager* primary_selection_manager_ = nullptr;

    // Populated by wlr_backend_autocreate on bare metal; null when nested or
    // headless, which is also what makes the virtual output necessary.
    struct wlr_session* session_ = nullptr;
    bool session_active_ = true;

    // The headless backend the virtual output lives on, if any. Never owned
    // separately: it is either backend_ itself or a child of the multi-backend,
    // and both are released by wlr_backend_destroy(backend_).
    struct wlr_backend* virtual_output_host_ = nullptr;

    // Fallback keyboard, kept for the headless case where no device ever
    // arrives. A real keyboard supersedes it on the seat but does not free it.
    struct wlr_keyboard* keyboard_ = nullptr;
    struct xkb_context* xkb_context_ = nullptr;
    struct xkb_keymap* xkb_keymap_ = nullptr;

    // True between the first wl_pointer event of a group and the frame that
    // closes it. Never observed outside a single call chain - the seat is only
    // ever driven from the event loop thread - so a plain bool is the whole of
    // the state the grouping rule needs.
    bool pointer_group_open_ = false;

    // Absolute pointer position in output pixels, shared by every producer.
    double pointer_x_ = 0.0;
    double pointer_y_ = 0.0;
    struct wlr_box output_box_ = {0, 0, 0, 0};
    bool output_box_adopted_ = false;

    // Outputs this session has successfully committed. Zero after the backend
    // has started is what the virtual-output bridge keys on.
    size_t outputs_seen_ = 0;

    Splash::WaylandListener<SpatialCompositor> new_xdg_toplevel_listener_;
    Splash::WaylandListener<SpatialCompositor> new_xdg_popup_listener_;
    Splash::WaylandListener<SpatialCompositor> new_output_listener_;
    Splash::WaylandListener<SpatialCompositor> new_input_listener_;
    Splash::WaylandListener<SpatialCompositor> fallback_modifiers_listener_;
    Splash::WaylandListener<SpatialCompositor> session_active_listener_;
    Splash::WaylandListener<SpatialCompositor> session_destroy_listener_;
    Splash::WaylandListener<SpatialCompositor> new_toplevel_decoration_listener_;
    Splash::WaylandListener<SpatialCompositor> request_set_selection_listener_;
    Splash::WaylandListener<SpatialCompositor> request_set_primary_selection_listener_;
    Splash::WaylandListener<SpatialCompositor> request_set_cursor_listener_;

    std::vector<std::shared_ptr<SpatialXdgWindow>> windows_;
    std::vector<std::shared_ptr<SpatialXdgWindow>> pending_destroy_;

    std::vector<std::shared_ptr<SpatialXdgPopup>> popups_;
    std::vector<std::shared_ptr<SpatialXdgPopup>> pending_destroy_popups_;

    std::vector<std::shared_ptr<SpatialSubsurface>> subsurfaces_;
    std::vector<std::shared_ptr<SpatialSubsurface>> pending_destroy_subsurfaces_;

    std::vector<std::shared_ptr<SpatialSeatDevice>> input_devices_;
    std::vector<std::shared_ptr<SpatialSeatDevice>> pending_destroy_devices_;

    std::vector<std::shared_ptr<SpatialOutput>> outputs_;
    std::vector<std::shared_ptr<SpatialOutput>> pending_destroy_outputs_;

    SessionStage stage_ = SessionStage::Down;
    OutputReadyHandler output_ready_;

    // Monotonic per-session counter. Zero is reserved as the invalid handle, so
    // a default-constructed handle never names a live window.
    WindowHandle next_window_handle_ = 1;
};

} // namespace Vazio
