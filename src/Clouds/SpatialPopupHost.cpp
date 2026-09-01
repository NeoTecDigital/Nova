// Written by Richard Christopher, Copyright 2026 NeoTec Digital
//
// xdg_popup hosting: the SpatialXdgPopup lifetime object and the compositor
// members that place a popup on the node its parent surface is drawn on.
//
// SCOPE, stated plainly. A popup is anchored where its positioner says and
// nothing more. Constraint solving - the flip/slide/resize policy an xdg
// positioner may request when the popup would leave the visible area - is NOT
// implemented here, and wlr_xdg_popup_unconstrain_from_box is deliberately not
// called. In a 3D scene the constraint area is not a screen rectangle, so the
// policy is a Desktop/Portal decision rather than a transport one, and it lands
// with them. Until then a menu that would overflow simply overflows, which is
// visible and honest rather than silently mispositioned.
#include "../../include/Clouds/SpatialCompositor.h"
#include "../../Core/components/logger.h"

#include <algorithm>

namespace Clouds {

namespace {

// Popups sit just in front of the quad they are anchored to, along its normal.
// Small enough that the popup reads as attached to its parent rather than
// floating away from it, large enough to win the depth test at any camera
// angle the parent is legible from.
constexpr float kPopupDepthBias = 0.004f;

// Extent of a popup whose parent has not committed a buffer yet, in world
// units. Only used for the single frame between map and the parent's first
// geometry, and replaced as soon as real pixel dimensions exist.
constexpr float kPopupFallbackExtent = 0.3f;

} // namespace

// --- SpatialXdgPopup ---
// Identical destruction discipline to SpatialXdgWindow: handlers detach and
// schedule, and the shared_ptr is released outside signal dispatch.

void SpatialXdgPopup::beginDestruction() {
    detachListeners();
    popup = nullptr;
    surface = nullptr;
    parent_surface = nullptr;

    mapped = false;
    if (destroy_scheduled) return;
    destroy_scheduled = true;
    if (compositor) {
        compositor->removePopup(handle);
    }
}

void SpatialXdgPopup::onCommit(void*) {
    if (!popup || !popup->base || !popup->base->surface) return;
    struct wlr_xdg_surface* base = popup->base;

    if (base->initial_commit) {
        // Popups have no set_size equivalent: xdg_popup.configure carries the
        // geometry the positioner already asked for, so the compositor's job on
        // the initial commit is to acknowledge that geometry and unblock the
        // client. wlr_xdg_surface_schedule_configure is what emits it - the
        // toplevel path's wlr_xdg_toplevel_set_size does not apply to a role
        // that never negotiates its own size.
        uint32_t serial = wlr_xdg_surface_schedule_configure(base);
        report(LOGGER::INFO, "SpatialCompositor - Popup %u initial configure serial=%u",
               handle, serial);
        return;
    }

    struct wlr_surface* commit_surface = base->surface;
    if (!wlr_surface_has_buffer(commit_surface)) return;

    if (compositor) {
        compositor->importPopupBuffer(*this);
        // Re-anchor on every commit: a reactive popup moves without remapping,
        // and the parent's own size may have changed under it.
        compositor->placePopupOnParent(*this);
    }
}

void SpatialXdgPopup::onMap(void*) {
    if (mapped) return;
    mapped = true;
    if (compositor) {
        compositor->attachPopupToParent(*this);
    }
}

void SpatialXdgPopup::onUnmap(void*) {
    if (!mapped) return;
    mapped = false;
    if (compositor) {
        compositor->detachPopupFromParent(*this);
    }
}

void SpatialXdgPopup::onSurfaceDestroy(void*) {
    beginDestruction();
}

void SpatialXdgPopup::onDestroy(void*) {
    beginDestruction();
}

// --- SpatialCompositor - popup hosting ---

void SpatialCompositor::onNewXdgPopup(void* data) {
    auto popup = static_cast<struct wlr_xdg_popup*>(data);
    if (!popup || !popup->base || !popup->base->surface) {
        report(LOGGER::ERROR, "SpatialCompositor - new_popup signal delivered no backing surface");
        return;
    }

    struct wlr_surface* surface = popup->base->surface;

    auto hosted = std::make_shared<SpatialXdgPopup>();
    hosted->compositor = this;
    hosted->handle = next_window_handle_++;
    hosted->popup = popup;
    hosted->surface = surface;
    hosted->parent_surface = popup->parent;

    std::shared_ptr<NovaSpatial::TextureHandle> fallback_texture;
    if (texture_bridge_) {
        fallback_texture = texture_bridge_->getFallbackTexture();
    }

    // A popup is its own pixels: no frame, no title, no chrome. Whatever a menu
    // wants around its edges, it draws.
    hosted->surface_host = std::make_shared<SpatialSurfaceHost>(
        glm::vec2(kPopupFallbackExtent, kPopupFallbackExtent), fallback_texture);
    hosted->surface_host->name = "XdgPopup";

    bindPopupListeners(*hosted, surface);
    bindPopupInput(*hosted, surface);

    report(LOGGER::INFO, "SpatialCompositor - New XDG Popup %u attached to parent surface %p",
           hosted->handle, static_cast<const void*>(popup->parent));

    popups_.push_back(std::move(hosted));
}

void SpatialCompositor::bindPopupListeners(SpatialXdgPopup& popup, struct wlr_surface* surface) {
    popup.commit_listener.bind(&popup, &SpatialXdgPopup::onCommit, &surface->events.commit);
    popup.map_listener.bind(&popup, &SpatialXdgPopup::onMap, &surface->events.map);
    popup.unmap_listener.bind(&popup, &SpatialXdgPopup::onUnmap, &surface->events.unmap);
    popup.surface_destroy_listener.bind(&popup, &SpatialXdgPopup::onSurfaceDestroy,
                                        &surface->events.destroy);
    popup.destroy_listener.bind(&popup, &SpatialXdgPopup::onDestroy,
                                &popup.popup->base->events.destroy);
}

void SpatialCompositor::bindPopupInput(SpatialXdgPopup& popup, struct wlr_surface* surface) {
    if (!popup.surface_host) return;

    // The captured raw pointers are cleared the moment either destroy signal
    // fires, so they cannot outlive the seat or the surface.
    SpatialCompositor* comp = this;

    // A popup claims its own input and sits at +kPopupDepthBias in its parent's
    // local space, so nearest-hit gives it the pointer wherever it overlaps the
    // surface it is anchored to - and the parent keeps everything outside it.
    popup.surface_host->claims_pointer_input = true;

    popup.surface_host->on_surface_pointer_enter = [comp, surface](float u, float v) {
        if (!surface) return;
        comp->notifySeatPointerEnter(surface, u * surface->current.width,
                                     v * surface->current.height);
    };

    popup.surface_host->on_surface_pointer_motion = [comp, surface](float u, float v) {
        if (!surface) return;
        comp->notifySeatPointerMotion(surface, u * surface->current.width,
                                      v * surface->current.height);
    };

    popup.surface_host->on_surface_pointer_leave = [comp]() {
        comp->notifySeatPointerLeave();
    };

    popup.surface_host->on_surface_button = [comp](uint32_t button, bool down) {
        comp->notifySeatPointerButton(sceneButtonToEvdev(button), down);
    };
}

std::shared_ptr<SpatialSurfaceHost> SpatialCompositor::hostNodeForSurface(
        struct wlr_surface* surface) const {
    if (!surface) return nullptr;

    for (const auto& win : windows_) {
        if (win && win->surface == surface) return win->surface_host;
    }
    // Nested menus anchor to another popup's node, so popups are searched too.
    for (const auto& popup : popups_) {
        if (popup && popup->surface == surface) return popup->surface_host;
    }
    return nullptr;
}

void SpatialCompositor::attachPopupToParent(SpatialXdgPopup& popup) {
    std::shared_ptr<SpatialSurfaceHost> anchor = hostNodeForSurface(popup.parent_surface);
    if (!anchor || !popup.surface_host) {
        report(LOGGER::ERROR, "SpatialCompositor - Popup %u has no hosted parent node; not shown",
               popup.handle);
        return;
    }

    placePopupOnParent(popup);
    anchor->addChild(popup.surface_host);
    report(LOGGER::INFO, "SpatialCompositor - Popup %u mapped onto its parent surface node",
           popup.handle);
}

void SpatialCompositor::detachPopupFromParent(SpatialXdgPopup& popup) {
    std::shared_ptr<SpatialSurfaceHost> anchor = hostNodeForSurface(popup.parent_surface);
    if (!anchor || !popup.surface_host) return;

    anchor->removeChild(popup.surface_host);
    report(LOGGER::INFO, "SpatialCompositor - Popup %u unmapped from its parent surface node",
           popup.handle);
}

void SpatialCompositor::placePopupOnParent(SpatialXdgPopup& popup) {
    std::shared_ptr<SpatialSurfaceHost> anchor = hostNodeForSurface(popup.parent_surface);
    if (!anchor || !popup.surface_host || !popup.popup || !popup.surface) return;
    if (!popup.parent_surface) return;

    const int parent_w = popup.parent_surface->current.width;
    const int parent_h = popup.parent_surface->current.height;
    if (parent_w <= 0 || parent_h <= 0) return;

    // The parent's quad spans its own local [-w/2, w/2] x [-h/2, h/2] and maps
    // onto the parent surface's pixels, y down. Everything below is that one
    // change of basis: surface pixels to the anchor node's local space.
    const float px_to_x = anchor->size.x / static_cast<float>(parent_w);
    const float px_to_y = anchor->size.y / static_cast<float>(parent_h);

    double popup_sx = 0.0;
    double popup_sy = 0.0;
    wlr_xdg_popup_get_position(popup.popup, &popup_sx, &popup_sy);

    const struct wlr_box& geometry = popup.popup->scheduled.geometry;
    const int popup_w = geometry.width > 0 ? geometry.width : popup.surface->current.width;
    const int popup_h = geometry.height > 0 ? geometry.height : popup.surface->current.height;
    if (popup_w <= 0 || popup_h <= 0) return;

    popup.surface_host->size = glm::vec2(static_cast<float>(popup_w) * px_to_x,
                                         static_cast<float>(popup_h) * px_to_y);

    // Anchor point is the popup's top-left in parent surface pixels; the node
    // is positioned by its centre, hence the half-extent shift.
    const float centre_px = static_cast<float>(popup_sx) + static_cast<float>(popup_w) * 0.5f;
    const float centre_py = static_cast<float>(popup_sy) + static_cast<float>(popup_h) * 0.5f;
    popup.surface_host->transform.position = glm::vec3(
        (centre_px / static_cast<float>(parent_w) - 0.5f) * anchor->size.x,
        (0.5f - centre_py / static_cast<float>(parent_h)) * anchor->size.y,
        kPopupDepthBias);
}

void SpatialCompositor::importPopupBuffer(SpatialXdgPopup& popup) {
    importSurfaceContent(popup.surface, popup.surface_host, popup.client_texture,
                         popup.unsupported_format, popup.handle);
}

size_t SpatialCompositor::mappedPopupCount() const {
    return static_cast<size_t>(std::count_if(popups_.begin(), popups_.end(),
        [](const std::shared_ptr<SpatialXdgPopup>& p) { return p && p->mapped; }));
}

void SpatialCompositor::removePopup(WindowHandle handle) {
    if (handle == INVALID_WINDOW_HANDLE) return;

    auto it = std::find_if(popups_.begin(), popups_.end(),
        [handle](const std::shared_ptr<SpatialXdgPopup>& p) { return p && p->handle == handle; });
    if (it == popups_.end()) return;

    pending_destroy_popups_.push_back(std::move(*it));
    popups_.erase(it);
}

void SpatialCompositor::drainDestroyedPopups() {
    if (pending_destroy_popups_.empty()) return;

    std::vector<std::shared_ptr<SpatialXdgPopup>> doomed;
    doomed.swap(pending_destroy_popups_);

    for (auto& popup : doomed) {
        if (!popup) continue;
        popup->detachListeners();
        if (popup->surface_host) {
            // The parent node may already be gone - a client that destroys its
            // toplevel takes the popups with it - so detach from whatever
            // parent the node still records rather than looking the surface up.
            if (auto parent = popup->surface_host->parent.lock()) {
                parent->removeChild(popup->surface_host);
            }
            if (scene_) scene_->releaseNode(popup->surface_host);
            popup->surface_host->on_surface_pointer_enter = nullptr;
            popup->surface_host->on_surface_pointer_motion = nullptr;
            popup->surface_host->on_surface_pointer_leave = nullptr;
            popup->surface_host->on_surface_button = nullptr;
            popup->surface_host->on_surface_key = nullptr;
            popup->surface_host->setTexture(nullptr);
        }
        if (texture_bridge_ && popup->client_texture) {
            texture_bridge_->releaseTexture(popup->client_texture);
        }
    }
}

void SpatialCompositor::releasePopups() {
    for (auto& popup : popups_) {
        if (!popup) continue;
        popup->detachListeners();
        pending_destroy_popups_.push_back(std::move(popup));
    }
    popups_.clear();
    drainDestroyedPopups();
}

} // namespace Clouds
