// Written by Richard Christopher, Copyright 2026 NeoTec Digital
//
// The per-object lifetime types a session hosts: xdg toplevels, xdg popups and
// physical input devices. Each owns the wl_listeners for exactly one wlroots
// object and follows one rule - a listener callback never destroys the object
// that owns the listener being dispatched, so every destroy path detaches and
// schedules, and SpatialCompositor releases outside signal dispatch.
#pragma once

#include "./WaylandListener.h"
#include "./Primitives.h"

#ifdef __cplusplus
extern "C" {
#endif
#include <wlr/types/wlr_compositor.h>
#include <wlr/types/wlr_input_device.h>
#include <wlr/types/wlr_keyboard.h>
#include <wlr/types/wlr_output.h>
#include <wlr/types/wlr_pointer.h>
#include <wlr/types/wlr_seat.h>
#include <wlr/types/wlr_xdg_shell.h>
#ifdef __cplusplus
}
#endif

#include <cstdint>
#include <memory>

namespace Clouds {

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
    std::shared_ptr<SpatialPanel> frame_panel;
    std::shared_ptr<SpatialSurfaceHost> surface_host;
    std::shared_ptr<SpatialLabel> title_label;

    // Live client pixels. Allocated on the first commit that carries a buffer
    // and reused in place until the client resizes; owned by the texture bridge.
    std::shared_ptr<NovaSpatial::TextureHandle> client_texture;

    WaylandListener<SpatialXdgWindow> commit_listener;
    WaylandListener<SpatialXdgWindow> map_listener;
    WaylandListener<SpatialXdgWindow> unmap_listener;
    WaylandListener<SpatialXdgWindow> surface_destroy_listener;
    WaylandListener<SpatialXdgWindow> destroy_listener;

    // True between wlr_surface.events.map and .unmap. Only mapped windows are in
    // the scene and only mapped windows are sent frame callbacks.
    bool mapped = false;

    // Last DRM fourcc this window committed that the import path cannot handle.
    // Held so the refusal is reported once per format, not once per commit.
    uint32_t unsupported_format = 0;

    // Set once the window has been handed to the compositor kill list, so the
    // xdg-surface and wlr_surface destroy paths cannot schedule it twice.
    bool destroy_scheduled = false;

    // Detaches before any member is torn down, so the listeners are off the
    // wl_signal lists by the time the scene-node shared_ptrs are released.
    ~SpatialXdgWindow() { detachListeners(); clearInputRouting(); }

    void onCommit(void* data);
    void applySurfaceGeometry(int width, int height);
    void onMap(void* data);
    void onUnmap(void* data);
    void onSurfaceDestroy(void* data);
    void onDestroy(void* data);

    // Detach every wl_listener this window owns. Idempotent. Must run while the
    // wlr_surface / wlr_xdg_surface that own the signals are still alive.
    void detachListeners() {
        commit_listener.unbind();
        map_listener.unbind();
        unmap_listener.unbind();
        surface_destroy_listener.unbind();
        destroy_listener.unbind();
    }

    // Drop the raw wlr_seat / wlr_surface pointers handed to the scene graph.
    void clearInputRouting() {
        if (!surface_host) return;
        surface_host->on_surface_pointer_enter = nullptr;
        surface_host->on_surface_pointer_motion = nullptr;
        surface_host->on_surface_pointer_leave = nullptr;
        surface_host->on_surface_button = nullptr;
        surface_host->on_surface_key = nullptr;
    }

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

    std::shared_ptr<SpatialSurfaceHost> surface_host;
    std::shared_ptr<NovaSpatial::TextureHandle> client_texture;

    WaylandListener<SpatialXdgPopup> commit_listener;
    WaylandListener<SpatialXdgPopup> map_listener;
    WaylandListener<SpatialXdgPopup> unmap_listener;
    WaylandListener<SpatialXdgPopup> surface_destroy_listener;
    WaylandListener<SpatialXdgPopup> destroy_listener;

    bool mapped = false;
    uint32_t unsupported_format = 0;
    bool destroy_scheduled = false;

    ~SpatialXdgPopup() { detachListeners(); }

    void onCommit(void* data);
    void onMap(void* data);
    void onUnmap(void* data);
    void onSurfaceDestroy(void* data);
    void onDestroy(void* data);

    void detachListeners() {
        commit_listener.unbind();
        map_listener.unbind();
        unmap_listener.unbind();
        surface_destroy_listener.unbind();
        destroy_listener.unbind();
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

    WaylandListener<SpatialSeatDevice> key_listener;
    WaylandListener<SpatialSeatDevice> modifiers_listener;
    WaylandListener<SpatialSeatDevice> motion_listener;
    WaylandListener<SpatialSeatDevice> motion_absolute_listener;
    WaylandListener<SpatialSeatDevice> button_listener;
    WaylandListener<SpatialSeatDevice> frame_listener;
    WaylandListener<SpatialSeatDevice> destroy_listener;

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

    WaylandListener<SpatialOutput> destroy_listener;

    bool destroy_scheduled = false;

    ~SpatialOutput() { detachListeners(); }

    void onDestroy(void* data);

    void detachListeners() { destroy_listener.unbind(); }

    void beginDestruction();
};

} // namespace Clouds
