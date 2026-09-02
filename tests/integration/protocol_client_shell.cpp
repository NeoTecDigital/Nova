// Written by Richard Christopher, Copyright 2026 NeoTec Digital
//
// Client half, shell phases: toplevel bring-up, buffer import, popup grab,
// subsurface, decoration and selection. The seat phases live in
// protocol_client_seat.cpp so neither translation unit carries both jobs.

#include "protocol_client.h"

#include <cstring>

namespace VazioTest {
namespace {

// --- xdg_surface / xdg_toplevel / xdg_popup listeners -------------------------

void onXdgSurfaceConfigure(void* data, struct xdg_surface*, uint32_t serial) {
    auto* state = static_cast<ClientState*>(data);
    state->last_configure_serial = serial;
    ++state->configure_count;
}

const struct xdg_surface_listener kXdgSurfaceListener = { .configure = onXdgSurfaceConfigure };

void onToplevelConfigure(void* data, struct xdg_toplevel*, int32_t width, int32_t height,
                         struct wl_array*) {
    auto* state = static_cast<ClientState*>(data);
    ++state->toplevel_configure_count;
    state->last_toplevel_width = width;
    state->last_toplevel_height = height;
}

void onToplevelClose(void*, struct xdg_toplevel*) {}
void onToplevelConfigureBounds(void*, struct xdg_toplevel*, int32_t, int32_t) {}
void onToplevelCapabilities(void*, struct xdg_toplevel*, struct wl_array*) {}

const struct xdg_toplevel_listener kToplevelListener = {
    .configure = onToplevelConfigure,
    .close = onToplevelClose,
    .configure_bounds = onToplevelConfigureBounds,
    .wm_capabilities = onToplevelCapabilities,
};

void onPopupConfigure(void* data, struct xdg_popup*, int32_t x, int32_t y, int32_t, int32_t) {
    auto* state = static_cast<ClientState*>(data);
    ++state->popup_configure_count;
    state->popup_x = x;
    state->popup_y = y;
}

void onPopupDone(void* data, struct xdg_popup*) {
    ++static_cast<ClientState*>(data)->popup_done_count;
}

void onPopupRepositioned(void*, struct xdg_popup*, uint32_t) {}

const struct xdg_popup_listener kPopupListener = {
    .configure = onPopupConfigure,
    .popup_done = onPopupDone,
    .repositioned = onPopupRepositioned,
};

// --- zxdg_toplevel_decoration_v1 --------------------------------------------

void onDecorationConfigure(void* data, struct zxdg_toplevel_decoration_v1*, uint32_t mode) {
    auto* state = static_cast<ClientState*>(data);
    ++state->decoration_configure_count;
    state->decoration_mode = mode;
}

const struct zxdg_toplevel_decoration_v1_listener kDecorationListener = {
    .configure = onDecorationConfigure,
};

// --- selection ---------------------------------------------------------------

void onOfferMime(void* data, struct wl_data_offer*, const char* mime_type) {
    static_cast<ClientState*>(data)->offered_mime_types.emplace_back(mime_type);
}
void onOfferSourceActions(void*, struct wl_data_offer*, uint32_t) {}
void onOfferAction(void*, struct wl_data_offer*, uint32_t) {}

const struct wl_data_offer_listener kOfferListener = {
    .offer = onOfferMime,
    .source_actions = onOfferSourceActions,
    .action = onOfferAction,
};

void onDataOffer(void* data, struct wl_data_device*, struct wl_data_offer* offer) {
    wl_data_offer_add_listener(offer, &kOfferListener, data);
}
void onDataEnter(void*, struct wl_data_device*, uint32_t, struct wl_surface*, wl_fixed_t,
                 wl_fixed_t, struct wl_data_offer*) {}
void onDataLeave(void*, struct wl_data_device*) {}
void onDataMotion(void*, struct wl_data_device*, uint32_t, wl_fixed_t, wl_fixed_t) {}
void onDataDrop(void*, struct wl_data_device*) {}

void onDataSelection(void* data, struct wl_data_device*, struct wl_data_offer* offer) {
    if (offer) ++static_cast<ClientState*>(data)->selection_offer_count;
}

const struct wl_data_device_listener kDataDeviceListener = {
    .data_offer = onDataOffer,
    .enter = onDataEnter,
    .leave = onDataLeave,
    .motion = onDataMotion,
    .drop = onDataDrop,
    .selection = onDataSelection,
};

void onDataSourceTarget(void*, struct wl_data_source*, const char*) {}
void onDataSourceSend(void*, struct wl_data_source*, const char*, int32_t fd) { close(fd); }
void onDataSourceCancelled(void*, struct wl_data_source*) {}
void onDataSourceDndDrop(void*, struct wl_data_source*) {}
void onDataSourceDndFinished(void*, struct wl_data_source*) {}
void onDataSourceAction(void*, struct wl_data_source*, uint32_t) {}

const struct wl_data_source_listener kDataSourceListener = {
    .target = onDataSourceTarget,
    .send = onDataSourceSend,
    .cancelled = onDataSourceCancelled,
    .dnd_drop_performed = onDataSourceDndDrop,
    .dnd_finished = onDataSourceDndFinished,
    .action = onDataSourceAction,
};

void onPrimaryOfferMime(void*, struct zwp_primary_selection_offer_v1*, const char*) {}
const struct zwp_primary_selection_offer_v1_listener kPrimaryOfferListener = {
    .offer = onPrimaryOfferMime,
};

void onPrimaryDataOffer(void*, struct zwp_primary_selection_device_v1*,
                        struct zwp_primary_selection_offer_v1* offer) {
    zwp_primary_selection_offer_v1_add_listener(offer, &kPrimaryOfferListener, nullptr);
}

void onPrimarySelection(void* data, struct zwp_primary_selection_device_v1*,
                        struct zwp_primary_selection_offer_v1* offer) {
    if (offer) ++static_cast<ClientState*>(data)->primary_selection_offer_count;
}

const struct zwp_primary_selection_device_v1_listener kPrimaryDeviceListener = {
    .data_offer = onPrimaryDataOffer,
    .selection = onPrimarySelection,
};

void onPrimarySourceSend(void*, struct zwp_primary_selection_source_v1*, const char*, int32_t fd) {
    close(fd);
}
void onPrimarySourceCancelled(void*, struct zwp_primary_selection_source_v1*) {}

const struct zwp_primary_selection_source_v1_listener kPrimarySourceListener = {
    .send = onPrimarySourceSend,
    .cancelled = onPrimarySourceCancelled,
};

// --- helpers -----------------------------------------------------------------

// Attach, damage the whole surface, request a frame callback and commit.
void presentBuffer(ClientSurfaces& c, struct wl_surface* surface, ShmBuffer& buffer) {
    if (!negativeControl("send")) {
        wl_surface_attach(surface, buffer.buffer(), 0, 0);
        wl_surface_damage_buffer(surface, 0, 0, buffer.width(), buffer.height());
    }
    requestFrameCallback(c.wl, surface);
    wl_surface_commit(surface);
}

}  // namespace

// --- phases ------------------------------------------------------------------

namespace {

// Role-object handshake: create, commit empty, wait for the initial configure,
// ack it. Returns false when no configure arrived at all.
bool negotiateToplevel(ClientSurfaces& c) {
    CheckLog& log = *c.log;

    const bool configured = pumpUntil(c.wl, [&] { return c.wl.configure_count > 0; });
    log.check(configured, "toplevel: initial xdg_surface.configure arrived (count=%d)",
              c.wl.configure_count);
    log.check(c.wl.toplevel_configure_count > 0,
              "toplevel: xdg_toplevel.configure arrived alongside it (count=%d)",
              c.wl.toplevel_configure_count);
    if (!configured) return false;

    // NEG=configure declines to ack. wlroots then refuses the map, and every
    // server-side assertion about a mapped window must fail.
    if (!negativeControl("configure")) {
        xdg_surface_ack_configure(c.toplevel_xdg, c.wl.last_configure_serial);
    }
    return true;
}

// Release is only observable on the SECOND commit: wlroots holds a committed
// buffer until the surface commits the next one. A single-buffered client never
// sees a release, which is exactly why asserting it here needs two.
bool proveBufferRelease(ClientSurfaces& c) {
    CheckLog& log = *c.log;

    c.toplevel_buffer_next = std::make_unique<ShmBuffer>();
    if (!c.toplevel_buffer_next->create(c.wl.globals.shm, kToplevelWidth, kToplevelHeight,
                                        WL_SHM_FORMAT_ARGB8888)) {
        log.check(false, "toplevel: second wl_shm buffer allocation failed");
        return false;
    }
    c.toplevel_buffer_next->fillPattern(0x77);
    presentBuffer(c, c.toplevel_surface, *c.toplevel_buffer_next);

    const bool released = pumpUntil(c.wl, [&] { return c.toplevel_buffer->released; }, 10000);
    log.check(released, "toplevel: the first wl_buffer was released once the second was committed");

    // The checksum is the point of the fill: the import path reads client memory
    // and must not write to it. A compositor that scribbles into a wl_shm pool
    // corrupts every client that reuses one.
    log.check(c.toplevel_buffer->checksum() == c.toplevel_checksum,
              "toplevel: buffer content unchanged after import (CRC32 still 0x%08x)",
              c.toplevel_checksum);
    return released;
}

// Build the popup, take an explicit grab and commit it. Returns the
// xdg_surface.configure count from before the commit, which is what the caller
// compares against to prove the popup got its own configure.
int createGrabbingPopup(ClientSurfaces& c) {
    c.popup_surface = wl_compositor_create_surface(c.wl.globals.compositor);
    c.popup_xdg = xdg_wm_base_get_xdg_surface(c.wl.globals.wm_base, c.popup_surface);
    xdg_surface_add_listener(c.popup_xdg, &kXdgSurfaceListener, &c.wl);

    // Anchored to the parent's top-left corner so the parent's bottom-right
    // quadrant is guaranteed to be popup-free - which is what the dismiss phase
    // needs somewhere to click.
    c.positioner = xdg_wm_base_create_positioner(c.wl.globals.wm_base);
    xdg_positioner_set_size(c.positioner, kPopupWidth, kPopupHeight);
    xdg_positioner_set_anchor_rect(c.positioner, 0, 0, 8, 8);
    xdg_positioner_set_anchor(c.positioner, XDG_POSITIONER_ANCHOR_TOP_LEFT);
    xdg_positioner_set_gravity(c.positioner, XDG_POSITIONER_GRAVITY_BOTTOM_RIGHT);

    const int configures_before = c.wl.configure_count;
    c.popup = xdg_surface_get_popup(c.popup_xdg, c.toplevel_xdg, c.positioner);
    xdg_popup_add_listener(c.popup, &kPopupListener, &c.wl);

    // An explicit grab is what makes click-outside-to-dismiss the compositor's
    // obligation rather than the client's. It needs a real input serial.
    xdg_popup_grab(c.popup, c.wl.globals.seat, c.wl.last_input_serial);
    wl_surface_commit(c.popup_surface);
    return configures_before;
}

}  // namespace

bool clientToplevelPhase(ClientSurfaces& c) {
    CheckLog& log = *c.log;

    c.toplevel_surface = wl_compositor_create_surface(c.wl.globals.compositor);
    c.toplevel_xdg = xdg_wm_base_get_xdg_surface(c.wl.globals.wm_base, c.toplevel_surface);
    xdg_surface_add_listener(c.toplevel_xdg, &kXdgSurfaceListener, &c.wl);
    c.toplevel = xdg_surface_get_toplevel(c.toplevel_xdg);
    xdg_toplevel_add_listener(c.toplevel, &kToplevelListener, &c.wl);
    xdg_toplevel_set_title(c.toplevel, "vazio-protocol-harness");
    xdg_toplevel_set_app_id(c.toplevel, "dev.neotec.vazio.harness");

    // The role-object handshake: an empty commit asks for the initial configure,
    // and no buffer may be attached before it has been acked.
    wl_surface_commit(c.toplevel_surface);
    if (!negotiateToplevel(c)) return false;

    c.toplevel_buffer = std::make_unique<ShmBuffer>();
    const bool made = c.toplevel_buffer->create(c.wl.globals.shm, kToplevelWidth, kToplevelHeight,
                                                WL_SHM_FORMAT_ARGB8888);
    log.check(made, "toplevel: %dx%d ARGB8888 wl_shm buffer allocated", kToplevelWidth, kToplevelHeight);
    if (!made) return false;

    c.toplevel_buffer->fillPattern(0x5A);
    c.toplevel_checksum = c.toplevel_buffer->checksum();
    log.check(c.toplevel_checksum != 0, "toplevel: pattern CRC32 = 0x%08x (non-degenerate)",
              c.toplevel_checksum);

    const int frames_before = c.wl.frame_done_count;
    presentBuffer(c, c.toplevel_surface, *c.toplevel_buffer);

    // The frame callback is the map signal: SpatialCompositor::onFramePresented
    // only sends frame_done to surfaces it has mapped, so receiving one is the
    // client-visible proof that this surface reached the scene.
    const bool framed = pumpUntil(c.wl, [&] { return c.wl.frame_done_count > frames_before; }, 10000);
    log.check(framed, "toplevel: wl_surface.frame callback fired - the surface is mapped (count=%d)",
              c.wl.frame_done_count);

    return proveBufferRelease(c) && framed;
}

bool clientPopupPhase(ClientSurfaces& c) {
    CheckLog& log = *c.log;

    const int configures_before = createGrabbingPopup(c);

    const bool configured = pumpUntil(c.wl, [&] {
        return c.wl.popup_configure_count > 0 && c.wl.configure_count > configures_before;
    });
    log.check(configured, "popup: xdg_popup.configure + xdg_surface.configure arrived");
    log.check(c.wl.popup_x == 0 && c.wl.popup_y == 0,
              "popup: placed at the anchor the positioner asked for (%d,%d)",
              c.wl.popup_x, c.wl.popup_y);
    if (!configured) return false;

    xdg_surface_ack_configure(c.popup_xdg, c.wl.last_configure_serial);

    c.popup_buffer = std::make_unique<ShmBuffer>();
    if (!c.popup_buffer->create(c.wl.globals.shm, kPopupWidth, kPopupHeight, WL_SHM_FORMAT_ARGB8888)) {
        log.check(false, "popup: wl_shm buffer allocation failed");
        return false;
    }
    c.popup_buffer->fillPattern(0xA5);
    const int frames_before = c.wl.frame_done_count;
    presentBuffer(c, c.popup_surface, *c.popup_buffer);

    // Popups throttle on frame callbacks exactly as toplevels do, and the
    // compositor only sends them to a MAPPED popup - so this is the map proof.
    const bool mapped = pumpUntil(c.wl, [&] { return c.wl.frame_done_count > frames_before; }, 10000);
    log.check(mapped, "popup: frame callback fired - the popup is mapped onto its parent");
    return mapped;
}

bool clientPopupDismissPhase(ClientSurfaces& c) {
    CheckLog& log = *c.log;
    roundtripClient(c.wl);
    const bool dismissed = pumpUntil(c.wl, [&] { return c.wl.popup_done_count > 0; }, 8000);
    log.check(dismissed, "popup: xdg_popup.popup_done after a click outside the grab (count=%d)",
              c.wl.popup_done_count);
    return dismissed;
}

bool clientSubsurfacePhase(ClientSurfaces& c) {
    CheckLog& log = *c.log;

    c.sub_surface = wl_compositor_create_surface(c.wl.globals.compositor);
    c.subsurface = wl_subcompositor_get_subsurface(c.wl.globals.subcompositor, c.sub_surface,
                                                   c.toplevel_surface);
    log.check(c.subsurface != nullptr, "subsurface: wl_subcompositor.get_subsurface succeeded");
    if (!c.subsurface) return false;

    wl_subsurface_set_position(c.subsurface, 20, 20);
    wl_subsurface_set_desync(c.subsurface);

    c.sub_buffer = std::make_unique<ShmBuffer>();
    if (!c.sub_buffer->create(c.wl.globals.shm, kSubsurfaceSize, kSubsurfaceSize,
                              WL_SHM_FORMAT_ARGB8888)) {
        log.check(false, "subsurface: wl_shm buffer allocation failed");
        return false;
    }
    c.sub_buffer->fillPattern(0x33);
    const int frames_before = c.wl.frame_done_count;
    presentBuffer(c, c.sub_surface, *c.sub_buffer);

    // A desync subsurface still needs the parent commit to take effect on the
    // parent's state; committing both is what a real client does.
    wl_surface_commit(c.toplevel_surface);

    const bool mapped = pumpUntil(c.wl, [&] { return c.wl.frame_done_count > frames_before; }, 10000);
    log.check(mapped, "subsurface: frame callback fired - the subsurface is mapped");
    return mapped;
}

bool clientDecorationPhase(ClientSurfaces& c) {
    CheckLog& log = *c.log;

    // A SECOND toplevel, deliberately: zxdg_toplevel_decoration_v1 forbids
    // attaching a buffer before the decoration has been configured, so the
    // decoration has to exist before the first commit of the surface it decorates.
    c.second_surface = wl_compositor_create_surface(c.wl.globals.compositor);
    c.second_xdg = xdg_wm_base_get_xdg_surface(c.wl.globals.wm_base, c.second_surface);
    xdg_surface_add_listener(c.second_xdg, &kXdgSurfaceListener, &c.wl);
    c.second_toplevel = xdg_surface_get_toplevel(c.second_xdg);
    xdg_toplevel_add_listener(c.second_toplevel, &kToplevelListener, &c.wl);
    xdg_toplevel_set_title(c.second_toplevel, "vazio-decorated");

    c.decoration = zxdg_decoration_manager_v1_get_toplevel_decoration(
        c.wl.globals.decoration_manager, c.second_toplevel);
    zxdg_toplevel_decoration_v1_add_listener(c.decoration, &kDecorationListener, &c.wl);
    zxdg_toplevel_decoration_v1_set_mode(c.decoration,
                                         ZXDG_TOPLEVEL_DECORATION_V1_MODE_SERVER_SIDE);

    const int configures_before = c.wl.configure_count;
    wl_surface_commit(c.second_surface);

    const bool answered = pumpUntil(c.wl, [&] {
        return c.wl.decoration_configure_count > 0 && c.wl.configure_count > configures_before;
    });
    log.check(answered, "decoration: zxdg_toplevel_decoration_v1.configure arrived (count=%d)",
              c.wl.decoration_configure_count);
    log.check(c.wl.decoration_mode == ZXDG_TOPLEVEL_DECORATION_V1_MODE_SERVER_SIDE,
              "decoration: mode is SERVER_SIDE (%u) - the compositor draws the frame",
              c.wl.decoration_mode);
    if (!answered) return false;

    xdg_surface_ack_configure(c.second_xdg, c.wl.last_configure_serial);

    c.second_buffer = std::make_unique<ShmBuffer>();
    if (!c.second_buffer->create(c.wl.globals.shm, kSecondToplevelWidth, kSecondToplevelHeight,
                                 WL_SHM_FORMAT_ARGB8888)) {
        log.check(false, "decoration: wl_shm buffer allocation failed");
        return false;
    }
    c.second_buffer->fillPattern(0x0F);
    const int frames_before = c.wl.frame_done_count;
    presentBuffer(c, c.second_surface, *c.second_buffer);

    const bool mapped = pumpUntil(c.wl, [&] { return c.wl.frame_done_count > frames_before; }, 10000);
    log.check(mapped, "decoration: the decorated second toplevel mapped");
    return mapped;
}

bool clientSelectionPhase(ClientSurfaces& c) {
    CheckLog& log = *c.log;
    static const char* kMime = "text/plain;charset=utf-8";

    c.data_device = wl_data_device_manager_get_data_device(c.wl.globals.data_device_manager,
                                                           c.wl.globals.seat);
    wl_data_device_add_listener(c.data_device, &kDataDeviceListener, &c.wl);

    c.data_source = wl_data_device_manager_create_data_source(c.wl.globals.data_device_manager);
    wl_data_source_add_listener(c.data_source, &kDataSourceListener, &c.wl);
    wl_data_source_offer(c.data_source, kMime);
    wl_data_device_set_selection(c.data_device, c.data_source, c.wl.last_input_serial);

    const bool offered = pumpUntil(c.wl, [&] { return c.wl.selection_offer_count > 0; }, 8000);
    log.check(offered, "selection: wl_data_device.selection came back to the focused client (count=%d)",
              c.wl.selection_offer_count);

    bool mime_seen = false;
    for (const std::string& mime : c.wl.offered_mime_types) {
        if (mime == kMime) mime_seen = true;
    }
    log.check(mime_seen, "selection: the offered mime type survived the round trip ('%s')", kMime);

    c.primary_device = zwp_primary_selection_device_manager_v1_get_device(
        c.wl.globals.primary_selection_manager, c.wl.globals.seat);
    zwp_primary_selection_device_v1_add_listener(c.primary_device, &kPrimaryDeviceListener, &c.wl);

    c.primary_source = zwp_primary_selection_device_manager_v1_create_source(
        c.wl.globals.primary_selection_manager);
    zwp_primary_selection_source_v1_add_listener(c.primary_source, &kPrimarySourceListener, &c.wl);
    zwp_primary_selection_source_v1_offer(c.primary_source, kMime);
    zwp_primary_selection_device_v1_set_selection(c.primary_device, c.primary_source,
                                                  c.wl.last_input_serial);

    const bool primary = pumpUntil(c.wl, [&] { return c.wl.primary_selection_offer_count > 0; }, 8000);
    log.check(primary, "selection: zwp_primary_selection_device_v1.selection delivered (count=%d)",
              c.wl.primary_selection_offer_count);

    return offered && mime_seen && primary;
}

}  // namespace VazioTest
