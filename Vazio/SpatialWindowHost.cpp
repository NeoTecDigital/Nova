// Written by Richard Christopher, Copyright 2026 NeoTec Digital
//
// Per-window half of the compositor: the SpatialXdgWindow lifetime object and
// every SpatialCompositor member that operates on the window list - creation,
// map/unmap into the portal root, client buffer import, frame pacing and the
// deferred destruction path. The session substrate (bring-up, sockets, seat,
// teardown) lives in SpatialCompositor.cpp.
#include "./SpatialCompositor.h"
#include "Nova/components/logger.h"
#include "Nova/pipeline/texture_bridge.h"
#include <drm_fourcc.h>
#include <algorithm>

namespace Vazio {

namespace {

// wl_shm's two mandatory formats. Both are little-endian 32-bit words with the
// alpha/ignored byte highest, so their in-memory byte order is B,G,R,A - which
// is what Nova::PixelLayout names. Anything else is refused rather than
// silently drawn with swapped channels.
bool drmFormatToPixelLayout(uint32_t drm_format, Nova::PixelLayout& out) {
    switch (drm_format) {
        case DRM_FORMAT_ARGB8888: out = Nova::PixelLayout::BGRA8; return true;
        case DRM_FORMAT_XRGB8888: out = Nova::PixelLayout::BGRX8; return true;
        default: return false;
    }
}

// What the frame's label says: title, else app_id, else a name honest about
// knowing neither. Shared by node construction and by the live
// set_title/set_app_id path so the label cannot drift from the client.
std::string resolveWindowTitle(const struct wlr_xdg_toplevel* toplevel) {
    if (!toplevel) return "Wayland Window";
    if (toplevel->title && toplevel->title[0]) return toplevel->title;
    if (toplevel->app_id && toplevel->app_id[0]) return toplevel->app_id;
    return "Wayland Window";
}

} // namespace

// --- SpatialXdgWindow ---
// INVARIANT: no wl_listener callback may destroy the object that owns the
// listener being dispatched. The destroy paths below only detach and schedule;
// the shared_ptr is released by SpatialCompositor::drainDestroyedWindows(),
// which runs once per frame strictly outside signal dispatch.

void SpatialXdgWindow::beginDestruction() {
    // Both signal owners (wlr_surface, wlr_xdg_surface) are still alive on the
    // first entry, which is the only point at which wl_list_remove is legal.
    detachListeners();
    clearInputRouting();
    toplevel = nullptr;
    surface = nullptr;

    mapped = false;
    if (destroy_scheduled) return;
    destroy_scheduled = true;
    if (compositor) {
        compositor->removeWindow(handle);
    }
}

void SpatialXdgWindow::onCommit(void*) {
    if (!toplevel || !toplevel->base || !toplevel->base->surface) return;
    struct wlr_xdg_surface* base = toplevel->base;

    if (base->initial_commit) {
        // The client is blocked until it receives a configure, and by protocol
        // its initial commit carries no buffer. A 0x0 size means "you choose",
        // which is the right answer while placement policy lives elsewhere; the
        // load-bearing part is that a configure is sent at all. It goes through
        // answerStateRequest() so state asked for before the first commit -
        // toolkits set_maximized and then commit - rides this very configure,
        // there being nothing to schedule one on until the surface is live.
        uint32_t serial = answerStateRequest();
        if (decoration_answer_pending) answerDecorationMode();
        report(LOGGER::INFO, "SpatialCompositor - Window %u initial configure serial=%u",
               handle, serial);
        return;
    }

    struct wlr_surface* commit_surface = base->surface;
    if (!wlr_surface_has_buffer(commit_surface)) return;

    if (compositor) {
        compositor->importSurfaceBuffer(*this);
    }
    applySurfaceGeometry(commit_surface->current.width, commit_surface->current.height);
}

void SpatialXdgWindow::applySurfaceGeometry(int width, int height) {
    if (width <= 0 || height <= 0 || !surface_host) return;

    float aspect = static_cast<float>(width) / static_cast<float>(height);
    surface_host->size = glm::vec2(1.2f, 1.2f / aspect);
    if (!frame_panel) return;

    frame_panel->size = glm::vec2(surface_host->size.x + 0.06f, surface_host->size.y + 0.12f);
    if (title_label) {
        title_label->transform.position = glm::vec3(0.0f, frame_panel->size.y * 0.5f - 0.04f, 0.003f);
    }
}

void SpatialXdgWindow::onMap(void*) {
    if (mapped) return;
    mapped = true;
    if (compositor && !minimized) {
        compositor->attachWindowToPortal(*this);
    }
}

void SpatialXdgWindow::onUnmap(void*) {
    if (!mapped) return;
    mapped = false;
    // Unmapping is not destruction: the client may commit a buffer again and be
    // remapped with the same nodes, listeners and texture.
    if (compositor) {
        compositor->detachWindowFromPortal(*this);
    }
}

uint32_t SpatialXdgWindow::answerStateRequest() {
    if (!toplevel || !toplevel->base || !toplevel->base->initialized) return 0;

    // A spatial window manager has no screen rectangle to fill: a window is a
    // quad in a 3D scene and "maximized" has no extent to mean here. So the
    // request is answered the only honest way the protocol allows - asked state
    // acked, size echoed back unchanged - and nothing pretends a monitor was
    // involved. What maximize and fullscreen should *do* spatially (fill the
    // Portal's frame, take the Precipitation surface) is a Desktop/Portal
    // decision. One configure carries all three: schedule_configure coalesces
    // within an event-loop iteration, so the client sees one atomic state.
    const int32_t width = surface ? surface->current.width : 0;
    const int32_t height = surface ? surface->current.height : 0;
    wlr_xdg_toplevel_set_size(toplevel, width, height);
    wlr_xdg_toplevel_set_maximized(toplevel, toplevel->requested.maximized);
    return wlr_xdg_toplevel_set_fullscreen(toplevel, toplevel->requested.fullscreen);
}

void SpatialXdgWindow::onRequestMinimize(void*) {
    if (!toplevel) return;
    const bool wanted = toplevel->requested.minimized;
    if (wanted == minimized) return;
    minimized = wanted;

    report(LOGGER::INFO, "SpatialCompositor - Window %u minimize request: minimized=%d",
           handle, static_cast<int>(minimized));

    // xdg-shell defines no configure and no state for minimisation, so the
    // answer is entirely what the compositor does with the node: out of the
    // scene and off the frame-callback list, which is as close to unmapped as a
    // still-mapped surface gets. Buffer, texture and listeners stay.
    if (!mapped || !compositor) return;
    if (minimized) {
        compositor->detachWindowFromPortal(*this);
    } else {
        compositor->attachWindowToPortal(*this);
    }
}

void SpatialXdgWindow::adoptDecoration(struct wlr_xdg_toplevel_decoration_v1* dec) {
    if (!dec) return;
    decoration = dec;
    decoration_mode_listener.bind(this, &SpatialXdgWindow::onDecorationMode, &dec->events.request_mode);
    decoration_destroy_listener.bind(this, &SpatialXdgWindow::onDecorationDestroy, &dec->events.destroy);
    answerDecorationMode();
}

void SpatialXdgWindow::answerDecorationMode() {
    if (!decoration || !toplevel || !toplevel->base) return;
    if (!toplevel->base->initialized) { decoration_answer_pending = true; return; }
    decoration_answer_pending = false;

    // Server-side unless the client insists. This compositor already draws a
    // frame panel around every hosted surface and two frames on one window is
    // worse than either; a client that asks for CLIENT_SIDE gets it, because
    // the protocol lets it ask. NOT yet honoured visually - the frame panel is
    // drawn either way, so a client-side window is double-framed until the
    // Portal owns chrome policy. Named rather than hidden behind the mode.
    const enum wlr_xdg_toplevel_decoration_v1_mode mode =
        decoration->requested_mode == WLR_XDG_TOPLEVEL_DECORATION_V1_MODE_CLIENT_SIDE
            ? WLR_XDG_TOPLEVEL_DECORATION_V1_MODE_CLIENT_SIDE
            : WLR_XDG_TOPLEVEL_DECORATION_V1_MODE_SERVER_SIDE;
    report(LOGGER::INFO, "SpatialCompositor - Window %u decoration mode=%d serial=%u", handle,
           static_cast<int>(mode), wlr_xdg_toplevel_decoration_v1_set_mode(decoration, mode));
}

void SpatialXdgWindow::applyTitle() {
    if (!title_label) return;
    title_label->setText(resolveWindowTitle(toplevel));
}

void SpatialXdgWindow::onNewSubsurface(void* data) {
    if (compositor) compositor->onNewSubsurface(surface, data);
}

void SpatialXdgWindow::onSurfaceDestroy(void*) {
    beginDestruction();
}

void SpatialXdgWindow::onDestroy(void*) {
    beginDestruction();
}

void SpatialCompositor::onNewXdgToplevel(void* data) {
    auto toplevel = static_cast<struct wlr_xdg_toplevel*>(data);
    if (!toplevel || !toplevel->base || !toplevel->base->surface) {
        report(LOGGER::ERROR, "SpatialCompositor - new_toplevel signal delivered no backing surface");
        return;
    }
    if (!portalRoot()) {
        report(LOGGER::ERROR, "SpatialCompositor - Portal root unavailable; cannot host XDG toplevel");
        return;
    }

    report(LOGGER::INFO, "SpatialCompositor - New XDG Toplevel client attached: title='%s', app_id='%s'",
           toplevel->title ? toplevel->title : "Untitled",
           toplevel->app_id ? toplevel->app_id : "Unknown");

    struct wlr_surface* surface = toplevel->base->surface;

    auto win = std::make_shared<SpatialXdgWindow>();
    win->compositor = this;
    win->handle = next_window_handle_++;
    win->toplevel = toplevel;
    win->surface = surface;

    buildWindowNodes(*win, toplevel);
    // The same routing a popup or a subsurface gets: the client's surface is
    // its own input target whatever role it plays.
    bindChildSurfaceInput(win->surface_host, surface);
    bindWindowListeners(*win, toplevel, surface);

    // Scene insertion happens on map, not here: a toplevel exists long before it
    // has a buffer, and a window with nothing to show does not belong in a scene.
    windows_.push_back(win);
}

void SpatialCompositor::bindWindowListeners(SpatialXdgWindow& window,
                                            struct wlr_xdg_toplevel* toplevel,
                                            struct wlr_surface* surface) {
    SpatialXdgWindow* win = &window;

    // The surface destroy listener exists because a wlr_surface may outlive, or
    // be torn down ahead of, its xdg_surface. Either signal detaches both.
    win->commit_listener.bind(win, &SpatialXdgWindow::onCommit, &surface->events.commit);
    win->map_listener.bind(win, &SpatialXdgWindow::onMap, &surface->events.map);
    win->unmap_listener.bind(win, &SpatialXdgWindow::onUnmap, &surface->events.unmap);
    win->surface_destroy_listener.bind(win, &SpatialXdgWindow::onSurfaceDestroy, &surface->events.destroy);
    // The ROLE object's destroy, not the xdg_surface's: wlroots tears the
    // toplevel down first and asserts that its own request_* signals have no
    // listeners left by the time it does (wlr_xdg_toplevel.c:530). A listener
    // on the surface's destroy is one step too late to satisfy that.
    win->destroy_listener.bind(win, &SpatialXdgWindow::onDestroy, &toplevel->events.destroy);
    win->new_subsurface_listener.bind(win, &SpatialXdgWindow::onNewSubsurface,
                                      &surface->events.new_subsurface);

    // Mandatory, not decorative: the protocol requires a configure in answer to
    // the state requests, and set_title/set_app_id are the only notice that the
    // label already drawn has gone stale.
    win->request_maximize_listener.bind(win, &SpatialXdgWindow::onRequestMaximize,
                                        &toplevel->events.request_maximize);
    win->request_fullscreen_listener.bind(win, &SpatialXdgWindow::onRequestFullscreen,
                                          &toplevel->events.request_fullscreen);
    win->request_minimize_listener.bind(win, &SpatialXdgWindow::onRequestMinimize,
                                        &toplevel->events.request_minimize);
    win->set_title_listener.bind(win, &SpatialXdgWindow::onSetTitle, &toplevel->events.set_title);
    win->set_app_id_listener.bind(win, &SpatialXdgWindow::onSetAppId, &toplevel->events.set_app_id);
}

void SpatialCompositor::attachWindowToPortal(SpatialXdgWindow& win) {
    const std::shared_ptr<Splash::SpatialNode>& root = portalRoot();
    if (!root || !win.frame_panel) return;

    root->addChild(win.frame_panel);
    report(LOGGER::INFO, "SpatialCompositor - Window %u mapped into the portal root", win.handle);
    focusSurface(win.surface);
}

void SpatialCompositor::detachWindowFromPortal(SpatialXdgWindow& win) {
    const std::shared_ptr<Splash::SpatialNode>& root = portalRoot();
    if (!root || !win.frame_panel) return;

    root->removeChild(win.frame_panel);
    report(LOGGER::INFO, "SpatialCompositor - Window %u unmapped from the portal root", win.handle);
}

void SpatialCompositor::buildWindowNodes(SpatialXdgWindow& win, struct wlr_xdg_toplevel* toplevel) {
    win.frame_panel = std::make_shared<Splash::SpatialPanel>(
        glm::vec2(1.26f, 0.92f),
        glm::vec4(0.08f, 0.10f, 0.16f, 0.92f)
    );

    // Compositor-side chrome follows the same rule the UI toolkit's windows do:
    // the frame is the input target for the decoration it draws, so a click on
    // the titlebar text reaches the frame rather than dying on a label with no
    // callbacks. The client's own surface claims below and stays its own
    // target, which is the whole point - a capture that swallowed the client's
    // input would make the window undrivable.
    win.frame_panel->captures_subtree_input = true;

    // Position the window in 3D Quaternionic space with a staggered layout
    float offset_x = (static_cast<float>(windows_.size() % 3) - 1.0f) * 0.4f;
    float offset_y = (static_cast<float>(windows_.size() % 2) - 0.5f) * 0.3f;
    float offset_z = -0.08f * static_cast<float>(windows_.size());
    win.frame_panel->transform.position = glm::vec3(offset_x, offset_y, offset_z);

    std::shared_ptr<Nova::TextureHandle> fallback_texture;
    if (texture_bridge_) {
        fallback_texture = texture_bridge_->getFallbackTexture();
    } else {
        report(LOGGER::ERROR, "SpatialCompositor - Texture bridge unavailable; surface hosted without fallback texture");
    }

    win.surface_host = std::make_shared<Splash::SpatialSurfaceHost>(glm::vec2(1.20f, 0.80f), fallback_texture);
    win.surface_host->transform.position = glm::vec3(0.0f, -0.04f, 0.002f);
    // Stated rather than inherited: this is the node the frame's capture must
    // not take, and the default that makes it so is one edit away from anyone.
    win.surface_host->claims_pointer_input = true;
    win.frame_panel->addChild(win.surface_host);

    if (!scene_ || !scene_->font) {
        report(LOGGER::ERROR, "SpatialCompositor - Spatial font unavailable; XDG toplevel hosted without a title label");
        return;
    }

    win.title_label = std::make_shared<Splash::SpatialLabel>(resolveWindowTitle(toplevel), scene_->font,
                                                     0.0011f, glm::vec4(0.85f, 0.92f, 1.0f, 1.0f));
    win.title_label->transform.position = glm::vec3(0.0f, 0.42f, 0.003f);
    win.title_label->claims_pointer_input = false;   // text is not a grab handle
    win.frame_panel->addChild(win.title_label);
}

// --- SpatialCompositor - client buffer import (SHM only this phase) ---

void SpatialCompositor::importSurfaceBuffer(SpatialXdgWindow& win) {
    importSurfaceContent(win.surface, win.surface_host, win.client_texture,
                         win.unsupported_format, win.handle);
}

void SpatialCompositor::importSurfaceContent(struct wlr_surface* surface,
                                             const std::shared_ptr<Splash::SpatialSurfaceHost>& host,
                                             std::shared_ptr<Nova::TextureHandle>& texture,
                                             uint32_t& unsupported_format,
                                             WindowHandle handle) {
    if (!texture_bridge_ || !surface || !host) return;

    struct wlr_client_buffer* client_buffer = surface->buffer;
    if (!client_buffer) return;

    // The raw client buffer, not the wlr_client_buffer wrapper: the wrapper's
    // texture is the wlroots renderer's copy, which Vazio never draws from.
    struct wlr_buffer* source = client_buffer->source;
    if (!source) return;   // client destroyed the buffer before it was released

    if (!uploadClientPixels(source, texture, unsupported_format, handle)) return;

    host->setTexture(texture);
}

bool SpatialCompositor::uploadClientPixels(struct wlr_buffer* source,
                                           std::shared_ptr<Nova::TextureHandle>& texture,
                                           uint32_t& unsupported_format,
                                           WindowHandle handle) {
    void* data = nullptr;
    uint32_t drm_format = 0;
    size_t stride = 0;

    if (!wlr_buffer_begin_data_ptr_access(source, WLR_BUFFER_DATA_PTR_ACCESS_READ,
                                          &data, &drm_format, &stride)) {
        // No CPU-visible storage: a dmabuf. Zero-copy import is a later phase,
        // and guessing here would draw garbage.
        return false;
    }

    Nova::PixelLayout layout = Nova::PixelLayout::BGRA8;
    bool supported = drmFormatToPixelLayout(drm_format, layout);
    bool uploaded = false;

    if (supported && data && source->width > 0 && source->height > 0) {
        uploaded = texture_bridge_->updateTextureFromRGBA(
            texture,
            static_cast<const uint8_t*>(data),
            static_cast<uint32_t>(stride),
            static_cast<uint32_t>(source->width),
            static_cast<uint32_t>(source->height),
            layout);
    }

    wlr_buffer_end_data_ptr_access(source);

    if (!supported && unsupported_format != drm_format) {
        unsupported_format = drm_format;
        report(LOGGER::ERROR, "SpatialCompositor - Surface %u committed unsupported DRM format 0x%08x",
               handle, drm_format);
    }
    return uploaded;
}

// --- SpatialCompositor - frame pacing ---

void SpatialCompositor::onFramePresented(const struct timespec& when) {
    if (!wl_display_) return;

    // Throttled clients render exactly one frame and then block on the callback
    // they requested. Releasing it here, once per presented frame, is what turns
    // a single static buffer into a live surface.
    for (auto& win : windows_) {
        // A minimised window is out of the scene: a frame callback would be
        // telling a client to draw what nobody is going to look at.
        if (win && win->mapped && !win->minimized && win->surface) {
            wlr_surface_send_frame_done(win->surface, &when);
        }
    }
    // Subsurfaces carry their own frame callbacks: the parent's does not reach
    // them, and a video pane that never gets one renders exactly one frame.
    for (auto& sub : subsurfaces_) {
        if (sub && sub->mapped && sub->surface) {
            wlr_surface_send_frame_done(sub->surface, &when);
        }
    }
    // Popups throttle on frame callbacks exactly as toplevels do; a menu that
    // never gets one animates once and then freezes.
    for (auto& popup : popups_) {
        if (popup && popup->mapped && popup->surface) {
            wlr_surface_send_frame_done(popup->surface, &when);
        }
    }
    wl_display_flush_clients(wl_display_);
}

size_t SpatialCompositor::mappedWindowCount() const {
    return static_cast<size_t>(std::count_if(windows_.begin(), windows_.end(),
        [](const std::shared_ptr<SpatialXdgWindow>& w) { return w && w->mapped; }));
}

void SpatialCompositor::removeWindow(WindowHandle handle) {
    if (handle == INVALID_WINDOW_HANDLE) return;

    auto it = std::find_if(windows_.begin(), windows_.end(),
        [handle](const std::shared_ptr<SpatialXdgWindow>& w) { return w && w->handle == handle; });
    if (it == windows_.end()) return;

    // Move, never erase-and-free: this runs inside a wl_listener dispatch and the
    // window owns the listener being dispatched.
    pending_destroy_.push_back(std::move(*it));
    windows_.erase(it);
}

void SpatialCompositor::drainDestroyedWindows() {
    if (pending_destroy_.empty()) return;

    // Swap out first so a nested schedule during teardown cannot invalidate the
    // iteration, then release outside every wl_listener dispatch frame.
    std::vector<std::shared_ptr<SpatialXdgWindow>> doomed;
    doomed.swap(pending_destroy_);

    for (auto& win : doomed) {
        if (!win) continue;
        win->detachListeners();
        if (portalRoot() && win->frame_panel) {
            portalRoot()->removeChild(win->frame_panel);
        }
        // Out of the scene first, texture second: releaseTexture waits for the
        // device to idle, so nothing can still be sampling it by then. Removal
        // from the tree is not enough - focus and the implicit grab are strong
        // references held outside it, and the frame panel can hold both.
        if (scene_) scene_->releaseNode(win->frame_panel);
        releaseChildHost(win->surface_host, win->client_texture);
    }
}

void SpatialCompositor::releaseWindows() {
    for (auto& win : windows_) {
        if (!win) continue;
        win->detachListeners();
        pending_destroy_.push_back(std::move(win));
    }
    windows_.clear();
    drainDestroyedWindows();
}

} // namespace Vazio
