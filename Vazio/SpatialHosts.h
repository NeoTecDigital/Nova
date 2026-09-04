// Written by Richard Christopher, Copyright 2026 NeoTec Digital
//
// The per-object lifetime types a session hosts: xdg toplevels, xdg popups and
// physical input devices. Each owns the wl_listeners for exactly one wlroots
// object and follows one rule - a listener callback never destroys the object
// that owns the listener being dispatched, so every destroy path detaches and
// schedules, and SpatialCompositor releases outside signal dispatch.
#pragma once

#include "Splash/WaylandListener.h"
#include "Splash/Primitives.h"
#include "Splash/Registry.h"

#ifdef __cplusplus
extern "C" {
#endif
#include <wlr/types/wlr_compositor.h>
#include <wlr/types/wlr_input_device.h>
#include <wlr/types/wlr_keyboard.h>
#include <wlr/types/wlr_output.h>
#include <wlr/types/wlr_pointer.h>
#include <wlr/types/wlr_seat.h>
#include <wlr/types/wlr_subcompositor.h>
#include <wlr/types/wlr_xdg_decoration_v1.h>
#include <wlr/types/wlr_xdg_shell.h>
#ifdef __cplusplus
}
#endif

#include <cstdint>
#include <functional>
#include <memory>

namespace Vazio {

class SpatialCompositor;

/**
 * Bring-up options that are known before the server starts and never change
 * afterwards, so they are passed once by value rather than read from the
 * environment at the point of use.
 */
struct SpatialCompositorConfig {
    // Force the headless backend instead of letting wlr_backend_autocreate
    // choose. Autocreate is the boot-target path: it yields DRM + libinput on a
    // TTY and the wayland/x11 backend when nested. Headless is the CI/test path
    // and is never selected implicitly.
    bool headless = false;

    // Extent of the virtual output hosted on the headless companion backend.
    // Matches the interim SDL window so the two agree on the output box that
    // pointer positions are clamped to.
    uint32_t virtual_width = 1600;
    uint32_t virtual_height = 1000;
};

/**
 * Session lifecycle, plan section S.6.
 *
 * `Latent` is the boot-splash substrate: the display exists, the backend, the
 * renderer, the allocator and the outputs are live and committing frames, and
 * the session is unreachable BY CONSTRUCTION - no socket has been added, so
 * there is nothing for a client to connect to and no protocol global for it to
 * bind. `Open` adds the socket, the protocol globals and the seat to the SAME
 * display: a state change on one object, never a handoff to a second one, which
 * is what keeps DRM master acquired exactly once across the whole boot.
 */
enum class SessionStage {
    Down,    // nothing created, or stop() has run
    Latent,  // substrate up, zero sockets, zero globals, no seat
    Open     // socket added, globals created, seat live
};

/**
 * Notified once for every output that has been enabled, mode-set and committed.
 *
 * The presentation loop is the consumer: it is the point at which an output can
 * be handed to wlr_output_configure_primary_swapchain. Delivered in both the
 * latent and the open stage, because the splash presents through exactly the
 * same outputs the session later does.
 */
using OutputReadyHandler = std::function<void(struct wlr_output*)>;

// Identifies a window across the compositor boundary. Minted from a per-session
// counter, never derived from an address: a raw pointer is only meaningful while
// the object lives, and the whole destruction discipline here exists because
// that lifetime is not the caller's to assume.
using WindowHandle = uint32_t;
constexpr WindowHandle INVALID_WINDOW_HANDLE = 0;

// The scene graph indexes pointer buttons 1/2/3; the seat and its clients speak
// evdev BTN_* codes. Declared here so the two directions have exactly one
// definition between them (src/Clouds/SpatialSeatInput.cpp).
uint32_t sceneButtonToEvdev(uint32_t scene_button);
uint32_t evdevToSceneButton(uint32_t evdev_button);

struct SpatialXdgWindow {
    SpatialCompositor* compositor = nullptr;
    WindowHandle handle = INVALID_WINDOW_HANDLE;
    struct wlr_xdg_toplevel* toplevel = nullptr;
    struct wlr_surface* surface = nullptr;
    // The three scene nodes this window is drawn as. Ids, not owners: the
    // registry owns every node, and a window that has unmapped keeps naming
    // nodes that are simply not in the tree at the moment.
    Splash::NodeId frame_panel;
    Splash::NodeId surface_host;
    Splash::NodeId title_label;

    // Live client pixels. Allocated on the first commit that carries a buffer
    // and reused in place until the client resizes; owned by the texture bridge.
    std::shared_ptr<Nova::TextureHandle> client_texture;

    Splash::WaylandListener<SpatialXdgWindow> commit_listener;
    Splash::WaylandListener<SpatialXdgWindow> map_listener;
    Splash::WaylandListener<SpatialXdgWindow> unmap_listener;
    Splash::WaylandListener<SpatialXdgWindow> surface_destroy_listener;
    Splash::WaylandListener<SpatialXdgWindow> destroy_listener;
    Splash::WaylandListener<SpatialXdgWindow> new_subsurface_listener;

    /**
     * zxdg_toplevel_decoration_v1 for this toplevel, when the client asked who
     * draws the frame. Held here rather than in a lifetime object of its own
     * because that is what it is: a property of one toplevel, created after it
     * and destroyed with it. Null until the client asks, and again if it drops
     * the decoration while keeping the window.
     */
    struct wlr_xdg_toplevel_decoration_v1* decoration = nullptr;
    Splash::WaylandListener<SpatialXdgWindow> decoration_mode_listener;
    Splash::WaylandListener<SpatialXdgWindow> decoration_destroy_listener;

    // A mode requested before the xdg_surface was initialised cannot be
    // answered yet - scheduling a configure on one is a wlroots assertion, and
    // toolkits routinely ask before their first commit - so the initial commit
    // flushes it.
    bool decoration_answer_pending = false;

    // xdg_toplevel state requests. Bound because the protocol requires every
    // one of them to be answered with a configure - even when the answer is
    // "nothing changed" - and because a title that is only read once at
    // creation is a label that lies for the rest of the window's life.
    Splash::WaylandListener<SpatialXdgWindow> request_maximize_listener;
    Splash::WaylandListener<SpatialXdgWindow> request_fullscreen_listener;
    Splash::WaylandListener<SpatialXdgWindow> request_minimize_listener;
    Splash::WaylandListener<SpatialXdgWindow> set_title_listener;
    Splash::WaylandListener<SpatialXdgWindow> set_app_id_listener;

    // True between wlr_surface.events.map and .unmap. Only mapped windows are in
    // the scene and only mapped windows are sent frame callbacks.
    bool mapped = false;

    // Last DRM fourcc this window committed that the import path cannot handle.
    // Held so the refusal is reported once per format, not once per commit.
    uint32_t unsupported_format = 0;

    // Set once the window has been handed to the compositor kill list, so the
    // xdg-surface and wlr_surface destroy paths cannot schedule it twice.
    bool destroy_scheduled = false;

    // Honoured xdg_toplevel.set_minimized. The window stays mapped in the
    // protocol sense - the client keeps its buffer and its surface - but its
    // node is out of the scene and it is paced no frames, which is the closest
    // honest analogue of a minimised window a portal-less session has.
    bool minimized = false;

    // Detaches before any member is torn down, so the listeners are off the
    // wl_signal lists by the time the scene-node shared_ptrs are released.
    ~SpatialXdgWindow() { detachListeners(); clearInputRouting(); }

    void onCommit(void* data);
    void applySurfaceGeometry(int width, int height);
    void onMap(void* data);
    void onUnmap(void* data);
    void onSurfaceDestroy(void* data);
    void onDestroy(void* data);
    void onNewSubsurface(void* data);

    // Answer a maximize/fullscreen request with a configure. Both land in the
    // same place because both are answered the same way and the difference is
    // one field of the same scheduled state. Returns the configure serial, or
    // zero when the surface cannot take a configure yet.
    void onRequestMaximize(void*) { answerStateRequest(); }
    void onRequestFullscreen(void*) { answerStateRequest(); }
    uint32_t answerStateRequest();
    void onRequestMinimize(void* data);

    // Adopt a decoration object and answer it. Both are the compositor's call:
    // it is what knows the frame is drawn server-side.
    void adoptDecoration(struct wlr_xdg_toplevel_decoration_v1* decoration);
    void answerDecorationMode();
    void onDecorationMode(void*) { answerDecorationMode(); }
    void onDecorationDestroy(void*) {
        decoration_mode_listener.unbind();
        decoration_destroy_listener.unbind();
        decoration = nullptr;
        decoration_answer_pending = false;
    }

    // Re-read the toplevel's title/app_id onto the label the frame draws.
    void onSetTitle(void*) { applyTitle(); }
    void onSetAppId(void*) { applyTitle(); }
    void applyTitle();

    // Detach every wl_listener this window owns. Idempotent. Must run while the
    // wlr_surface / wlr_xdg_surface that own the signals are still alive.
    void detachListeners() {
        commit_listener.unbind();
        map_listener.unbind();
        unmap_listener.unbind();
        surface_destroy_listener.unbind();
        destroy_listener.unbind();
        new_subsurface_listener.unbind();
        request_maximize_listener.unbind();
        request_fullscreen_listener.unbind();
        request_minimize_listener.unbind();
        set_title_listener.unbind();
        set_app_id_listener.unbind();
        decoration_mode_listener.unbind();
        decoration_destroy_listener.unbind();
    }

    // Drop the raw wlr_seat / wlr_surface pointers handed to the scene graph.
    // Reaches the node through the compositor's registry, so it is a no-op once
    // the compositor is gone or the node has been destroyed.
    void clearInputRouting();

    // Sever every outbound reference and hand the window to the kill list.
    // Idempotent; safe to call from inside a wl_listener dispatch.
    void beginDestruction();
};

/**
 * An xdg_popup hosted as a child of the node its parent surface is drawn on.
 *
 * Popups follow the same lifetime discipline as toplevels - configure on the
 * initial commit, scene insertion on map, deferred destruction - but carry no
 * chrome: a menu is its own pixels and nothing else.
 */
struct SpatialXdgPopup {
    SpatialCompositor* compositor = nullptr;
    WindowHandle handle = INVALID_WINDOW_HANDLE;
    struct wlr_xdg_popup* popup = nullptr;
    struct wlr_surface* surface = nullptr;

    // The surface this popup is anchored to. May be a toplevel's surface or
    // another popup's, which is how nested menus stack.
    struct wlr_surface* parent_surface = nullptr;

    Splash::NodeId surface_host;
    std::shared_ptr<Nova::TextureHandle> client_texture;

    Splash::WaylandListener<SpatialXdgPopup> commit_listener;
    Splash::WaylandListener<SpatialXdgPopup> map_listener;
    Splash::WaylandListener<SpatialXdgPopup> unmap_listener;
    Splash::WaylandListener<SpatialXdgPopup> surface_destroy_listener;
    Splash::WaylandListener<SpatialXdgPopup> destroy_listener;
    Splash::WaylandListener<SpatialXdgPopup> reposition_listener;
    Splash::WaylandListener<SpatialXdgPopup> new_subsurface_listener;

    bool mapped = false;
    uint32_t unsupported_format = 0;
    bool destroy_scheduled = false;

    ~SpatialXdgPopup() { detachListeners(); }

    void onCommit(void* data);
    void onMap(void* data);
    void onUnmap(void* data);
    void onSurfaceDestroy(void* data);
    void onDestroy(void* data);
    void onReposition(void* data);
    void onNewSubsurface(void* data);

    /**
     * Whether this popup took an explicit grab (xdg_popup.grab).
     *
     * wlroots 0.19 exposes no grab signal, so the state is read off the object:
     * wlr_xdg_popup.seat (wlr_xdg_shell.h:101) is null until a grab request
     * names a seat, and nothing else writes it. grab_link is NOT usable for
     * this - it is only initialised when the popup is linked into a grab, so
     * wl_list_empty() on an ungrabbed popup reads uninitialised memory and
     * answers "grabbed" (measured: every popup looked grabbed, and ungrabbed
     * menus were dismissed by clicks that should not have touched them).
     *
     * Only a grabbing popup is dismissed by a click outside it. A popup that
     * never grabbed is the client's business, and taking it down would be the
     * compositor deciding something no one asked it to decide.
     */
    bool grabbed() const { return popup && popup->seat != nullptr; }

    void detachListeners() {
        commit_listener.unbind();
        map_listener.unbind();
        unmap_listener.unbind();
        surface_destroy_listener.unbind();
        destroy_listener.unbind();
        reposition_listener.unbind();
        new_subsurface_listener.unbind();
    }

    void beginDestruction();
};

/**
 * One wl_subsurface hosted as a child node of its parent surface's node.
 *
 * A subsurface is a second buffer the client positions in its parent's own
 * coordinates - a video pane inside a player, a GL canvas inside a toolkit
 * window. It has no role object of its own to configure and no say in its own
 * lifetime: it maps and unmaps with its parent, and the parent's commit is
 * what applies its position. So the discipline is the popup's, minus the
 * configure, and the placement is the popup's change of basis, minus the
 * positioner.
 */
struct SpatialSubsurface {
    SpatialCompositor* compositor = nullptr;
    WindowHandle handle = INVALID_WINDOW_HANDLE;
    struct wlr_subsurface* subsurface = nullptr;
    struct wlr_surface* surface = nullptr;

    // The surface this subsurface is positioned in. Always a surface this
    // session hosts - a toplevel, a popup, or another subsurface.
    struct wlr_surface* parent_surface = nullptr;

    Splash::NodeId surface_host;
    std::shared_ptr<Nova::TextureHandle> client_texture;

    Splash::WaylandListener<SpatialSubsurface> commit_listener;
    Splash::WaylandListener<SpatialSubsurface> map_listener;
    Splash::WaylandListener<SpatialSubsurface> unmap_listener;
    Splash::WaylandListener<SpatialSubsurface> surface_destroy_listener;
    Splash::WaylandListener<SpatialSubsurface> destroy_listener;
    Splash::WaylandListener<SpatialSubsurface> new_subsurface_listener;

    bool mapped = false;
    uint32_t unsupported_format = 0;
    bool destroy_scheduled = false;

    ~SpatialSubsurface() { detachListeners(); }

    void onCommit(void* data);
    void onMap(void* data);
    void onUnmap(void* data);
    void onSurfaceDestroy(void* data);
    void onDestroy(void* data);
    void onNewSubsurface(void* data);

    void detachListeners() {
        commit_listener.unbind();
        map_listener.unbind();
        unmap_listener.unbind();
        surface_destroy_listener.unbind();
        destroy_listener.unbind();
        new_subsurface_listener.unbind();
    }

    void beginDestruction();
};

/**
 * One physical input device attached to the seat.
 *
 * Devices arrive on backend.events.new_input and leave on their own destroy
 * signal, which fires from inside libinput's dispatch - so removal schedules
 * rather than frees, exactly as window destruction does.
 */
struct SpatialSeatDevice {
    SpatialCompositor* compositor = nullptr;
    struct wlr_input_device* device = nullptr;

    // Exactly one of these is set, chosen by device->type.
    struct wlr_keyboard* keyboard = nullptr;
    struct wlr_pointer* pointer = nullptr;

    Splash::WaylandListener<SpatialSeatDevice> key_listener;
    Splash::WaylandListener<SpatialSeatDevice> modifiers_listener;
    Splash::WaylandListener<SpatialSeatDevice> motion_listener;
    Splash::WaylandListener<SpatialSeatDevice> motion_absolute_listener;
    Splash::WaylandListener<SpatialSeatDevice> button_listener;
    Splash::WaylandListener<SpatialSeatDevice> frame_listener;
    Splash::WaylandListener<SpatialSeatDevice> destroy_listener;

    bool destroy_scheduled = false;

    ~SpatialSeatDevice() { detachListeners(); }

    void onKey(void* data);
    void onModifiers(void* data);
    void onMotion(void* data);
    void onMotionAbsolute(void* data);
    void onButton(void* data);
    void onFrame(void* data);
    void onDestroy(void* data);

    void detachListeners() {
        key_listener.unbind();
        modifiers_listener.unbind();
        motion_listener.unbind();
        motion_absolute_listener.unbind();
        button_listener.unbind();
        frame_listener.unbind();
        destroy_listener.unbind();
    }

    void beginDestruction();
};

/**
 * One wlr_output this session has enabled, committed and advertised.
 *
 * The wl_output global is what a client enumerates monitors from; a session
 * that never creates one hands GTK/Qt an empty screen list. Creating it is the
 * whole of this object's job, and the destroy listener exists so the global is
 * withdrawn while the wlr_output that backs it is still alive - the same
 * detach-and-schedule discipline every other hosted lifetime follows.
 */
struct SpatialOutput {
    SpatialCompositor* compositor = nullptr;
    struct wlr_output* output = nullptr;

    Splash::WaylandListener<SpatialOutput> destroy_listener;

    bool destroy_scheduled = false;

    ~SpatialOutput() { detachListeners(); }

    void onDestroy(void* data);

    void detachListeners() { destroy_listener.unbind(); }

    void beginDestruction();
};

} // namespace Vazio
