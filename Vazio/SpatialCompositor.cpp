// Written by Richard Christopher, Copyright 2026 NeoTec Digital
#include "./SpatialCompositor.h"
#include "Nova/components/logger.h"
#include <algorithm>
#include <cstdlib>

namespace Vazio {

SpatialCompositor::SpatialCompositor(Nova::Core* core,
                                     Nova::TextureBridge* texture_bridge,
                                     std::shared_ptr<Splash::SpatialScene> scene,
                                     Splash::NodeId portal_root,
                                     SpatialCompositorConfig config)
    : core_(core), texture_bridge_(texture_bridge), scene_(scene),
      portal_root_(portal_root), config_(config) {
    // The output box is authoritative for pointer clamping from the first
    // motion event onward, which may precede the first output commit.
    output_box_.width = static_cast<int>(config_.virtual_width);
    output_box_.height = static_cast<int>(config_.virtual_height);
    pointer_x_ = output_box_.width * 0.5;
    pointer_y_ = output_box_.height * 0.5;
}

Splash::NodeId SpatialCompositor::portalRoot() const {
    // An unset portal root is the single-Desktop default, not an error: the
    // scene root is exactly the Portal a session with one Desktop would inject.
    if (portal_root_.valid()) return portal_root_;
    return scene_ ? scene_->root : Splash::INVALID_NODE;
}

SpatialCompositor::~SpatialCompositor() {
    stop();
}

bool SpatialCompositor::startServer(const std::string& socket_name) {
    // The nested development entry point's startup, in one call. Both halves
    // are in SpatialBackendStack.cpp with the rest of the lifecycle; a session
    // that is reachable from the moment it exists is simply one that does not
    // linger in the latent stage.
    return startSubstrate() && open(socket_name);
}

bool SpatialCompositor::initProtocols() {
    // The buffer protocols come first: wl_shm and linux-dmabuf are globals the
    // latent stage deliberately withheld, and wlr_compositor hands clients
    // buffers that only exist because of them.
    if (!wlr_renderer_init_wl_display(renderer_, wl_display_)) {
        report(LOGGER::ERROR, "SpatialCompositor - Failed to bind renderer buffer protocols to the Wayland display");
        return false;
    }

    compositor_ = wlr_compositor_create(wl_display_, 6, renderer_);
    if (!compositor_) {
        report(LOGGER::ERROR, "SpatialCompositor - Failed to create wlr_compositor");
        return false;
    }

    if (!wlr_subcompositor_create(wl_display_) || !wlr_data_device_manager_create(wl_display_)) {
        report(LOGGER::ERROR, "SpatialCompositor - Failed to create wlr_subcompositor or wlr_data_device_manager");
        return false;
    }

    // Advertised because a client that finds no decoration manager assumes it
    // must draw its own frame, and this session draws one for it.
    xdg_decoration_manager_ = wlr_xdg_decoration_manager_v1_create(wl_display_);
    primary_selection_manager_ = wlr_primary_selection_v1_device_manager_create(wl_display_);
    if (!xdg_decoration_manager_ || !primary_selection_manager_) {
        report(LOGGER::ERROR, "SpatialCompositor - Failed to create the decoration or primary-selection manager");
        return false;
    }

    xdg_shell_ = wlr_xdg_shell_create(wl_display_, 3);
    if (!xdg_shell_) {
        report(LOGGER::ERROR, "SpatialCompositor - Failed to create wlr_xdg_shell");
        return false;
    }

    new_xdg_toplevel_listener_.bind(this, &SpatialCompositor::onNewXdgToplevel, &xdg_shell_->events.new_toplevel);
    new_xdg_popup_listener_.bind(this, &SpatialCompositor::onNewXdgPopup, &xdg_shell_->events.new_popup);

    // new_output is already bound - it has to be live before wlr_backend_start,
    // which happens at substrate. new_input moves off the latent handler now
    // that a seat is about to exist to route devices onto.
    new_input_listener_.bind(this, &SpatialCompositor::onNewInput, &backend_->events.new_input);
    new_toplevel_decoration_listener_.bind(this, &SpatialCompositor::onNewToplevelDecoration,
                                           &xdg_decoration_manager_->events.new_toplevel_decoration);
    return true;
}

bool SpatialCompositor::ensureVirtualOutput() {
    // Strictly after wlr_backend_start, because that is when a backend declares
    // what it actually has: DRM emits its connectors, and autocreate's headless
    // path emits the WLR_HEADLESS_OUTPUTS it made for itself (one by default -
    // verified empirically against 0.19.3). Adding a virtual output before that
    // point produces a second, fictional screen on exactly those backends.
    if (outputs_seen_ > 0) return true;

    if (!virtual_output_host_) {
        // Real hardware with nothing plugged in. A headless output here would
        // claim a screen exists, so say so instead and carry on: connectors
        // still arrive on new_output when one is plugged in.
        report(LOGGER::ERROR, "SpatialCompositor - Backend started with no outputs and no virtual host");
        return true;
    }

    if (!wlr_headless_add_output(virtual_output_host_, config_.virtual_width, config_.virtual_height)) {
        report(LOGGER::ERROR, "SpatialCompositor - Failed to create the %ux%u virtual output",
               config_.virtual_width, config_.virtual_height);
        return false;
    }
    report(LOGGER::INFO, "SpatialCompositor - Virtual output created at %ux%u",
           config_.virtual_width, config_.virtual_height);
    return true;
}

bool SpatialCompositor::initInput() {
    seat_ = wlr_seat_create(wl_display_, "seat0");
    if (!seat_) {
        report(LOGGER::ERROR, "SpatialCompositor - Failed to create wlr_seat");
        return false;
    }

    // Selection and cursor policy are seat decisions the clients ask for; the
    // answers live with the rest of the seat (SpatialSeatInput.cpp).
    request_set_selection_listener_.bind(this, &SpatialCompositor::onRequestSetSelection,
                                         &seat_->events.request_set_selection);
    request_set_primary_selection_listener_.bind(this, &SpatialCompositor::onRequestSetPrimarySelection,
                                                 &seat_->events.request_set_primary_selection);
    request_set_cursor_listener_.bind(this, &SpatialCompositor::onRequestSetCursor,
                                      &seat_->events.request_set_cursor);

    // The fallback keyboard is best effort: physical keyboards arriving on
    // new_input supersede it, and capabilities are recomputed either way.
    initKeyboard();
    refreshSeatCapabilities();
    return true;
}

bool SpatialCompositor::initKeyboard() {
    keyboard_ = static_cast<struct wlr_keyboard*>(std::calloc(1, sizeof(struct wlr_keyboard)));
    if (!keyboard_) {
        report(LOGGER::ERROR, "SpatialCompositor - Failed to allocate virtual keyboard; keyboard capability withheld");
        return false;
    }
    wlr_keyboard_init(keyboard_, nullptr, "spatial-keyboard");

    xkb_context_ = xkb_context_new(XKB_CONTEXT_NO_FLAGS);
    if (xkb_context_) {
        struct xkb_rule_names rules = {};
        xkb_keymap_ = xkb_keymap_new_from_names(xkb_context_, &rules, XKB_KEYMAP_COMPILE_NO_FLAGS);
    }

    if (!xkb_keymap_) {
        report(LOGGER::ERROR, "SpatialCompositor - Failed to compile an XKB keymap; keyboard capability withheld");
        releaseInput();
        return false;
    }

    wlr_keyboard_set_keymap(keyboard_, xkb_keymap_);
    wlr_seat_set_keyboard(seat_, keyboard_);

    // processKey() drives this keyboard with update_state, so its xkb state
    // tracks held modifiers; without this the resulting modifier event has no
    // listener and every client sees an unshifted, unctrl'd keyboard.
    fallback_modifiers_listener_.bind(this, &SpatialCompositor::onFallbackModifiers,
                                      &keyboard_->events.modifiers);
    return true;
}

void SpatialCompositor::onFallbackModifiers(void*) {
    notifySeatModifiers(keyboard_);
}

bool SpatialCompositor::initSocket(const std::string& socket_name) {
    // Try the explicit named socket first, then fall back to auto-assigned.
    if (wl_display_add_socket(wl_display_, socket_name.c_str()) == 0) {
        socket_name_ = socket_name;
        return true;
    }

    const char* auto_socket = wl_display_add_socket_auto(wl_display_);
    if (!auto_socket) {
        report(LOGGER::ERROR, "SpatialCompositor - Failed to add socket to Wayland display");
        return false;
    }
    socket_name_ = auto_socket;
    return true;
}

void SpatialCompositor::processKey(uint32_t keycode, bool pressed, uint32_t time_ms) {
    if (!keyboard_) return;

    struct wlr_keyboard_key_event event = {
        .time_msec = time_ms,
        .keycode = keycode,
        .update_state = true,
        .state = pressed ? WL_KEYBOARD_KEY_STATE_PRESSED : WL_KEYBOARD_KEY_STATE_RELEASED
    };
    wlr_keyboard_notify_key(keyboard_, &event);
}

void SpatialCompositor::focusSurface(struct wlr_surface* surface) {
    if (!seat_ || !surface) return;

    struct wlr_keyboard* kb = wlr_seat_get_keyboard(seat_);
    if (kb) {
        wlr_seat_keyboard_notify_enter(seat_, surface, kb->keycodes, kb->num_keycodes, &kb->modifiers);
    } else {
        wlr_seat_keyboard_notify_enter(seat_, surface, nullptr, 0, nullptr);
    }

    // Seat focus and scene focus are one decision, not two. Leaving them apart
    // is what let a surface hold the seat's keyboard while the scene still
    // routed keys - and, before setKeyboardFocus cleared it, left every node
    // ever focused still flagged as focused.
    // A surface this session does not host has no node to focus; clearing the
    // scene's focus on its behalf would be inventing a decision nobody made.
    if (scene_) {
        const Splash::NodeId host = hostNodeForSurface(surface);
        if (host.valid()) {
            scene_->setKeyboardFocus(host);
        }
    }
}

void SpatialCompositor::iterateEventLoop(int timeout_ms) {
    if (wl_display_ && event_loop_) {
        wl_event_loop_dispatch(event_loop_, timeout_ms);
        wl_display_flush_clients(wl_display_);
    }
    // Strictly after dispatch returns, never from inside a listener callback.
    // Children before their parents: a subsurface's node hangs off the popup or
    // toplevel node released next.
    drainDestroyedSubsurfaces();
    drainDestroyedPopups();
    drainDestroyedWindows();
    drainRemovedInputDevices();
    drainRemovedOutputs();

    // Last, after every list that schedules node destruction: this is what turns
    // "unlinked now" into "the slot is reusable", and it is safe outside
    // dispatch for exactly the reason the lists above are.
    if (scene_) scene_->registry().drain();
}

// --- SpatialCompositor - teardown ---
// Ownership verified against wlroots 0.19 headers. wl_display_destroy releases
// everything holding a display_destroy listener: wlr_compositor (wlr_compositor.h:284),
// wlr_subcompositor (wlr_subcompositor.h:66), wlr_data_device_manager (wlr_data_device.h:26),
// wlr_xdg_shell (wlr_xdg_shell.h:35) and wlr_seat (wlr_seat.h:308) - none may be freed here.
// wlr_backend is destroyed with the wl_event_loop (backend.h:73-75), i.e. inside
// wl_display_destroy and therefore after the renderer/allocator its outputs use;
// wlr_renderer and wlr_allocator have no display hook at all. All three are freed below.

void SpatialCompositor::stop() {
    // Every wl_listener is detached while the object owning its wl_signal is
    // still alive; wl_list_remove writes through link->prev and link->next.
    releaseSubsurfaces();
    releasePopups();
    releaseWindows();
    releaseOutputs();

    if (wl_display_) {
        report(LOGGER::INFO, "SpatialCompositor - Stopping Wayland display server");
        wl_display_destroy_clients(wl_display_);
    }
    releaseInput();

    new_xdg_toplevel_listener_.unbind();  // owned by xdg_shell_->events.new_toplevel
    new_xdg_popup_listener_.unbind();     // owned by xdg_shell_->events.new_popup
    new_output_listener_.unbind();        // owned by backend_->events.new_output
    new_input_listener_.unbind();         // owned by backend_->events.new_input
    session_active_listener_.unbind();    // owned by session_->events.active
    session_destroy_listener_.unbind();   // owned by session_->events.destroy
    new_toplevel_decoration_listener_.unbind();       // owned by xdg_decoration_manager_
    request_set_selection_listener_.unbind();         // owned by seat_->events
    request_set_primary_selection_listener_.unbind(); // owned by seat_->events
    request_set_cursor_listener_.unbind();            // owned by seat_->events

    releaseBackendStack();

    if (wl_display_) {
        wl_display_destroy(wl_display_);
        wl_display_ = nullptr;
    }

    // Display-owned handles are dead now; null them so a second stop() is inert.
    // The session is destroyed with the event loop (wlr_session holds an
    // event_loop_destroy listener), so it is gone by this point too.
    event_loop_ = nullptr;
    compositor_ = nullptr;
    xdg_shell_ = nullptr;
    seat_ = nullptr;
    xdg_decoration_manager_ = nullptr;
    primary_selection_manager_ = nullptr;
    session_ = nullptr;
    session_active_ = true;
    virtual_output_host_ = nullptr;
    output_ready_ = nullptr;
    stage_ = SessionStage::Down;
}

// --- SpatialCompositor - decoration negotiation ---

void SpatialCompositor::onNewToplevelDecoration(void* data) {
    auto decoration = static_cast<struct wlr_xdg_toplevel_decoration_v1*>(data);
    if (!decoration || !decoration->toplevel) {
        report(LOGGER::ERROR, "SpatialCompositor - new_toplevel_decoration delivered no toplevel");
        return;
    }

    // The decoration belongs to exactly one toplevel and dies with it, so the
    // window that already owns that toplevel's listeners owns this one too.
    for (const auto& win : windows_) {
        if (win && win->toplevel == decoration->toplevel) {
            win->adoptDecoration(decoration);
            return;
        }
    }
    report(LOGGER::ERROR, "SpatialCompositor - Decoration arrived for a toplevel this session does not host");
}

void SpatialCompositor::releaseInput() {
    // Devices first: each holds listeners on signals the seat and the backend
    // own, and their destroy signals fire while both are still alive.
    for (auto& device : input_devices_) {
        if (device) device->detachListeners();
    }
    input_devices_.clear();
    for (auto& device : pending_destroy_devices_) {
        if (device) device->detachListeners();
    }
    pending_destroy_devices_.clear();

    if (keyboard_) {
        // wlr_keyboard_finish emits wlr_input_device.events.destroy, which the
        // seat listens on (wlr_seat.h:224) to clear its keyboard state. The seat
        // must therefore still be alive here.
        fallback_modifiers_listener_.unbind();  // owned by keyboard_->events.modifiers
        wlr_keyboard_finish(keyboard_);
        std::free(keyboard_);
        keyboard_ = nullptr;
    }
    if (xkb_keymap_) {
        xkb_keymap_unref(xkb_keymap_);
        xkb_keymap_ = nullptr;
    }
    if (xkb_context_) {
        xkb_context_unref(xkb_context_);
        xkb_context_ = nullptr;
    }
}

void SpatialCompositor::releaseBackendStack() {
    // Backend first: it destroys every wlr_output, and their swapchains hold
    // buffers produced by the allocator and textures owned by the renderer.
    if (backend_) {
        wlr_backend_destroy(backend_);
        backend_ = nullptr;
    }
    if (allocator_) {
        wlr_allocator_destroy(allocator_);
        allocator_ = nullptr;
    }
    if (renderer_) {
        wlr_renderer_destroy(renderer_);
        renderer_ = nullptr;
    }
}

} // namespace Vazio
