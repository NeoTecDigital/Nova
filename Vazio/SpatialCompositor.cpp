// Written by Richard Christopher, Copyright 2026 NeoTec Digital
#include "./SpatialCompositor.h"
#include "Nova/components/logger.h"
#include <algorithm>
#include <cstdlib>

namespace Vazio {

namespace {

// Spacing between a parent quad and the subsurfaces stacked in front of it.
// Smaller than the popup bias so a menu opened over a subsurface still wins.
constexpr float kSubsurfaceDepthBias = 0.0008f;

// Extent of a subsurface whose parent has not committed a buffer yet. Replaced
// by real pixel dimensions on the first commit that carries any.
constexpr float kSubsurfaceFallbackExtent = 0.3f;

// Paint order within the parent's committed stack. wlroots keeps the protocol's
// two lists - below the parent and above it - in paint order and reorders them
// in place, so reading them here is what makes restacking take effect.
int subsurfacePaintIndex(struct wlr_surface* parent, const struct wlr_subsurface* target) {
    int index = 0;
    struct wl_list* stacks[] = { &parent->current.subsurfaces_below,
                                 &parent->current.subsurfaces_above };
    for (struct wl_list* stack : stacks) {
        struct wlr_subsurface_parent_state* state = nullptr;
        wl_list_for_each(state, stack, link) {
            struct wlr_subsurface* sibling = wl_container_of(state, sibling, current);
            if (sibling == target) return index;
            ++index;
        }
    }
    return index;
}

} // namespace

SpatialCompositor::SpatialCompositor(Nova::Core* core,
                                     Nova::TextureBridge* texture_bridge,
                                     std::shared_ptr<Splash::SpatialScene> scene,
                                     std::shared_ptr<Splash::SpatialNode> portal_root,
                                     SpatialCompositorConfig config)
    : core_(core), texture_bridge_(texture_bridge), scene_(scene),
      portal_root_(std::move(portal_root)), config_(config) {
    // The output box is authoritative for pointer clamping from the first
    // motion event onward, which may precede the first output commit.
    output_box_.width = static_cast<int>(config_.virtual_width);
    output_box_.height = static_cast<int>(config_.virtual_height);
    pointer_x_ = output_box_.width * 0.5;
    pointer_y_ = output_box_.height * 0.5;
}

const std::shared_ptr<Splash::SpatialNode>& SpatialCompositor::portalRoot() const {
    // A null portal root is the single-Desktop default, not an error: the scene
    // root is exactly the Portal a session with one Desktop would inject.
    static const std::shared_ptr<Splash::SpatialNode> none;
    if (portal_root_) return portal_root_;
    return scene_ ? scene_->root : none;
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
        if (std::shared_ptr<Splash::SpatialSurfaceHost> host = hostNodeForSurface(surface)) {
            scene_->setKeyboardFocus(std::move(host));
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

// --- SpatialCompositor - subsurface hosting ---
//
// Creation and placement live here, next to the loop that drains the kill list;
// the SpatialSubsurface lifetime object and the attach/detach it shares with
// popups live in SpatialPopupHost.cpp. Split only because both files are at the
// 500-line cap - the whole of it belongs in one TU, and the next hand on this
// code should make that src/Clouds/SpatialSubsurfaceHost.cpp.

void SpatialCompositor::onNewSubsurface(struct wlr_surface* parent_surface, void* data) {
    auto subsurface = static_cast<struct wlr_subsurface*>(data);
    if (!subsurface || !subsurface->surface || !parent_surface) {
        report(LOGGER::ERROR, "SpatialCompositor - new_subsurface signal delivered no backing surface");
        return;
    }

    auto hosted = std::make_shared<SpatialSubsurface>();
    hosted->compositor = this;
    hosted->handle = next_window_handle_++;
    hosted->subsurface = subsurface;
    hosted->surface = subsurface->surface;
    hosted->parent_surface = parent_surface;

    std::shared_ptr<Nova::TextureHandle> fallback_texture;
    if (texture_bridge_) fallback_texture = texture_bridge_->getFallbackTexture();

    // Its own pixels and its own input, exactly like a popup: a subsurface is
    // a second buffer the client draws, not chrome this compositor owns.
    hosted->surface_host = std::make_shared<Splash::SpatialSurfaceHost>(
        glm::vec2(kSubsurfaceFallbackExtent, kSubsurfaceFallbackExtent), fallback_texture);
    hosted->surface_host->name = "Subsurface";
    hosted->surface_host->claims_pointer_input = true;

    SpatialSubsurface* child = hosted.get();
    struct wlr_surface* surface = hosted->surface;
    child->commit_listener.bind(child, &SpatialSubsurface::onCommit, &surface->events.commit);
    child->map_listener.bind(child, &SpatialSubsurface::onMap, &surface->events.map);
    child->unmap_listener.bind(child, &SpatialSubsurface::onUnmap, &surface->events.unmap);
    child->surface_destroy_listener.bind(child, &SpatialSubsurface::onSurfaceDestroy, &surface->events.destroy);
    child->destroy_listener.bind(child, &SpatialSubsurface::onDestroy, &subsurface->events.destroy);
    child->new_subsurface_listener.bind(child, &SpatialSubsurface::onNewSubsurface,
                                        &surface->events.new_subsurface);
    bindChildSurfaceInput(hosted->surface_host, surface);

    report(LOGGER::INFO, "SpatialCompositor - New subsurface %u on parent surface %p",
           child->handle, static_cast<const void*>(parent_surface));
    subsurfaces_.push_back(std::move(hosted));

    // new_subsurface fires when the subsurface enters the parent's CURRENT
    // state (wlr_compositor.h:226), which for a client that committed the child
    // first is after that child has already mapped. Waiting for a map signal
    // that has been and gone would leave the node out of the scene forever, so
    // the current state is read once instead of assumed pristine.
    if (surface->mapped) {
        child->onMap(nullptr);
        child->onCommit(nullptr);
    }
}

void SpatialCompositor::placeSubsurfaceOnParent(SpatialSubsurface& sub) {
    std::shared_ptr<Splash::SpatialSurfaceHost> anchor = hostNodeForSurface(sub.parent_surface);
    if (!anchor || !sub.surface_host || !sub.subsurface || !sub.surface || !sub.parent_surface) return;

    // Offset is parent-relative surface-local pixels, applied by the parent's
    // commit; extent is the child's own committed size. The popup path's change
    // of basis carries both because they are in the same pixel space.
    const struct wlr_box box = { sub.subsurface->current.x, sub.subsurface->current.y,
                                 sub.surface->current.width, sub.surface->current.height };

    // Paint order, biased in front of the parent quad. wl_subsurface.place_below
    // can put a sibling behind its parent, which two coplanar quads cannot
    // express, so every subsurface is drawn in front and only the relative order
    // is honoured - below-parent siblings sitting closest to it. Stated rather
    // than hidden: it is the one place this departs from the protocol.
    const int paint_index = subsurfacePaintIndex(sub.parent_surface, sub.subsurface);
    placeChildOnParentQuad(*sub.surface_host, *anchor, *sub.parent_surface, box,
                           kSubsurfaceDepthBias * static_cast<float>(1 + paint_index));
}

void SpatialCompositor::importSubsurfaceBuffer(SpatialSubsurface& sub) {
    importSurfaceContent(sub.surface, sub.surface_host, sub.client_texture,
                         sub.unsupported_format, sub.handle);
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
