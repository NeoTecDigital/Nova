// Written by Richard Christopher, Copyright 2026 NeoTec Digital
//
// Hosted child surfaces: xdg_popups and wl_subsurfaces. Both are a client's
// second buffer placed in its parent's own coordinates, both map and unmap with
// the parent and both are torn down by the same deferred path - so the lifetime
// is written once and only the rule that decides their rectangle differs.
//
// SCOPE, stated plainly. A popup is anchored where its positioner says and
// nothing more. Constraint solving - the flip/slide/resize policy an xdg
// positioner may request when the popup would leave the visible area - is NOT
// implemented here, and wlr_xdg_popup_unconstrain_from_box is deliberately not
// called: in a 3D scene the constraint area is not a screen rectangle, so the
// policy is a Desktop/Portal decision rather than a transport one. Until then a
// menu that would overflow simply overflows, which is visible and honest.
#include "include/Clouds/SpatialCompositor.h"
#include "Core/components/logger.h"

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

// Is `node` the subtree rooted at `root`, or anywhere inside it?
bool nodeWithin(const std::shared_ptr<SpatialNode>& node, const std::shared_ptr<SpatialNode>& root) {
    if (!node || !root) return false;
    for (std::shared_ptr<SpatialNode> walk = node; walk; walk = walk->parent.lock()) {
        if (walk == root) return true;
    }
    return false;
}

template <typename Child>
size_t countMappedChildren(const std::vector<std::shared_ptr<Child>>& list) {
    return static_cast<size_t>(std::count_if(list.begin(), list.end(),
        [](const std::shared_ptr<Child>& c) { return c && c->mapped; }));
}

// Move, never erase-and-free: removal runs inside a wl_listener dispatch and
// the child owns the listener being dispatched.
template <typename Child>
void scheduleChildRemoval(std::vector<std::shared_ptr<Child>>& live,
                          std::vector<std::shared_ptr<Child>>& pending,
                          WindowHandle handle) {
    if (handle == INVALID_WINDOW_HANDLE) return;
    auto it = std::find_if(live.begin(), live.end(),
        [handle](const std::shared_ptr<Child>& c) { return c && c->handle == handle; });
    if (it == live.end()) return;
    pending.push_back(std::move(*it));
    live.erase(it);
}

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

void SpatialXdgPopup::onReposition(void*) {
    // reposition replaces the positioner and expects the popup to be placed
    // again by the rules that placed it first. There is one set of those rules
    // and this is it; the client's ack and re-commit run the same path again.
    if (compositor) compositor->placePopupOnParent(*this);
}

void SpatialXdgPopup::onNewSubsurface(void* data) {
    if (compositor) compositor->onNewSubsurface(surface, data);
}

// --- SpatialSubsurface ---
// Same discipline again, with no configure to answer and no positioner to
// consult: the position is applied by the parent's commit and the size is
// whatever the child committed, so onCommit is import-and-place.

void SpatialSubsurface::beginDestruction() {
    detachListeners();
    subsurface = nullptr;
    surface = nullptr;
    parent_surface = nullptr;

    mapped = false;
    if (destroy_scheduled) return;
    destroy_scheduled = true;
    if (compositor) {
        compositor->removeSubsurface(handle);
    }
}

void SpatialSubsurface::onCommit(void*) {
    if (!surface || !compositor || !wlr_surface_has_buffer(surface)) return;
    compositor->importSubsurfaceBuffer(*this);
    // Re-place every commit: offset is parent state, extent is the child's own,
    // and either can change without anything remapping.
    compositor->placeSubsurfaceOnParent(*this);
}

void SpatialSubsurface::onMap(void*) {
    if (mapped) return;
    mapped = true;
    if (!compositor) return;
    compositor->attachSubsurfaceToParent(*this);
    // A subsurface's first commit is applied by its PARENT's commit - the same
    // one that puts it in the parent's current state - so that signal can fire
    // before this object exists to hear it. Map is the first moment a buffer is
    // guaranteed. Measured: without this the node drew the fallback forever.
    onCommit(nullptr);
}

void SpatialSubsurface::onUnmap(void*) {
    if (!mapped) return;
    mapped = false;
    if (compositor) compositor->detachSubsurfaceFromParent(*this);
}

void SpatialSubsurface::onSurfaceDestroy(void*) { beginDestruction(); }

void SpatialSubsurface::onDestroy(void*) { beginDestruction(); }

void SpatialSubsurface::onNewSubsurface(void* data) {
    if (compositor) compositor->onNewSubsurface(surface, data);
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

    hosted->surface_host->claims_pointer_input = true;
    bindPopupListeners(*hosted, surface);
    bindChildSurfaceInput(hosted->surface_host, surface);

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
    // The role object's own destroy, for the same reason the toplevel uses it:
    // wlroots destroys the popup before its xdg_surface and requires its
    // reposition signal to be listener-free by then.
    popup.destroy_listener.bind(&popup, &SpatialXdgPopup::onDestroy,
                                &popup.popup->events.destroy);
    popup.reposition_listener.bind(&popup, &SpatialXdgPopup::onReposition,
                                   &popup.popup->events.reposition);
    popup.new_subsurface_listener.bind(&popup, &SpatialXdgPopup::onNewSubsurface,
                                       &surface->events.new_subsurface);
}

void SpatialCompositor::bindChildSurfaceInput(const std::shared_ptr<SpatialSurfaceHost>& host,
                                              struct wlr_surface* surface) {
    if (!host || !surface) return;

    // The captured raw pointers are cleared the moment either destroy signal
    // fires (releaseChildHost), so they cannot outlive the seat or the surface.
    // A child claims its own input and sits in front of its parent, so
    // nearest-hit gives it the pointer wherever the two overlap.
    SpatialCompositor* comp = this;

    host->on_surface_pointer_enter = [comp, surface](float u, float v) {
        comp->notifySeatPointerEnter(surface, u * surface->current.width,
                                     v * surface->current.height);
    };

    host->on_surface_pointer_motion = [comp, surface](float u, float v) {
        comp->notifySeatPointerMotion(surface, u * surface->current.width,
                                      v * surface->current.height);
    };

    host->on_surface_pointer_leave = [comp]() { comp->notifySeatPointerLeave(); };

    host->on_surface_button = [comp, surface](uint32_t button, bool down) {
        if (down) comp->focusSurface(surface);
        comp->notifySeatPointerButton(sceneButtonToEvdev(button), down);
    };

    host->on_surface_key = [comp](uint32_t key, bool pressed) {
        comp->notifySeatSurfaceKey(key, pressed);
    };
}

std::shared_ptr<SpatialSurfaceHost> SpatialCompositor::hostNodeForSurface(
        struct wlr_surface* surface) const {
    if (!surface) return nullptr;

    for (const auto& win : windows_) {
        if (win && win->surface == surface) return win->surface_host;
    }
    // Nested menus anchor to another popup's node, and a subsurface may host
    // both, so every hosted surface kind is searched.
    for (const auto& popup : popups_) {
        if (popup && popup->surface == surface) return popup->surface_host;
    }
    for (const auto& sub : subsurfaces_) {
        if (sub && sub->surface == surface) return sub->surface_host;
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
    // A grabbing popup is what typing is about while it is up. This names it to
    // the seat and to the scene; wlroots' own xdg keyboard grab may decline to
    // move the seat's focus off the surface the grab started on, which is why
    // the outcome is logged rather than assumed (kb=1 means the seat moved).
    if (popup.grabbed()) focusSurface(popup.surface);
    report(LOGGER::INFO, "SpatialCompositor - Popup %u mapped onto its parent node (grab=%d kb=%d)",
           popup.handle, static_cast<int>(popup.grabbed()),
           static_cast<int>(seat_ && seat_->keyboard_state.focused_surface == popup.surface));
}

void SpatialCompositor::detachPopupFromParent(SpatialXdgPopup& popup) {
    std::shared_ptr<SpatialSurfaceHost> anchor = hostNodeForSurface(popup.parent_surface);
    if (!anchor || !popup.surface_host) return;

    anchor->removeChild(popup.surface_host);
    report(LOGGER::INFO, "SpatialCompositor - Popup %u unmapped from its parent surface node",
           popup.handle);

    // The menu is going; typing goes back to what it covered. Asked of the seat
    // rather than the popup, which may already have been stripped of its grab.
    if (seat_ && popup.parent_surface &&
        seat_->keyboard_state.focused_surface == popup.surface) {
        focusSurface(popup.parent_surface);
    }
}

void SpatialCompositor::attachSubsurfaceToParent(SpatialSubsurface& sub) {
    std::shared_ptr<SpatialSurfaceHost> anchor = hostNodeForSurface(sub.parent_surface);
    if (!anchor || !sub.surface_host) {
        report(LOGGER::ERROR, "SpatialCompositor - Subsurface %u has no hosted parent; not shown", sub.handle);
        return;
    }

    placeSubsurfaceOnParent(sub);
    anchor->addChild(sub.surface_host);
    report(LOGGER::INFO, "SpatialCompositor - Subsurface %u mapped onto its parent surface node", sub.handle);
}

void SpatialCompositor::detachSubsurfaceFromParent(SpatialSubsurface& sub) {
    std::shared_ptr<SpatialSurfaceHost> anchor = hostNodeForSurface(sub.parent_surface);
    if (!anchor || !sub.surface_host) return;

    anchor->removeChild(sub.surface_host);
    report(LOGGER::INFO, "SpatialCompositor - Subsurface %u unmapped from its parent node", sub.handle);
}

void SpatialCompositor::placeChildOnParentQuad(SpatialSurfaceHost& child,
                                               const SpatialSurfaceHost& anchor,
                                               const struct wlr_surface& parent_surface,
                                               const struct wlr_box& child_box,
                                               float depth_bias) {
    const int parent_w = parent_surface.current.width;
    const int parent_h = parent_surface.current.height;
    if (parent_w <= 0 || parent_h <= 0 || child_box.width <= 0 || child_box.height <= 0) return;

    // Surface pixels to the anchor node's local space, on both axes.
    const float px_to_x = anchor.size.x / static_cast<float>(parent_w);
    const float px_to_y = anchor.size.y / static_cast<float>(parent_h);
    child.size = glm::vec2(static_cast<float>(child_box.width) * px_to_x,
                           static_cast<float>(child_box.height) * px_to_y);

    // The box is top-left anchored in parent surface pixels; the node is
    // positioned by its centre, hence the half-extent shift.
    const float centre_px = static_cast<float>(child_box.x) + static_cast<float>(child_box.width) * 0.5f;
    const float centre_py = static_cast<float>(child_box.y) + static_cast<float>(child_box.height) * 0.5f;
    child.transform.position = glm::vec3(
        (centre_px / static_cast<float>(parent_w) - 0.5f) * anchor.size.x,
        (0.5f - centre_py / static_cast<float>(parent_h)) * anchor.size.y,
        depth_bias);
}

void SpatialCompositor::placePopupOnParent(SpatialXdgPopup& popup) {
    std::shared_ptr<SpatialSurfaceHost> anchor = hostNodeForSurface(popup.parent_surface);
    if (!anchor || !popup.surface_host || !popup.popup || !popup.surface) return;
    if (!popup.parent_surface) return;

    // Where the positioner put it, in the parent surface's own pixels. The
    // scheduled geometry is the size the client was configured with; the
    // surface's own extent stands in until it has acked one.
    double popup_sx = 0.0;
    double popup_sy = 0.0;
    wlr_xdg_popup_get_position(popup.popup, &popup_sx, &popup_sy);
    const struct wlr_box& geometry = popup.popup->scheduled.geometry;

    const struct wlr_box box = {
        static_cast<int>(popup_sx), static_cast<int>(popup_sy),
        geometry.width > 0 ? geometry.width : popup.surface->current.width,
        geometry.height > 0 ? geometry.height : popup.surface->current.height
    };
    placeChildOnParentQuad(*popup.surface_host, *anchor, *popup.parent_surface, box, kPopupDepthBias);
}

void SpatialCompositor::importPopupBuffer(SpatialXdgPopup& popup) {
    importSurfaceContent(popup.surface, popup.surface_host, popup.client_texture,
                         popup.unsupported_format, popup.handle);
}

size_t SpatialCompositor::mappedSubsurfaceCount() const { return countMappedChildren(subsurfaces_); }

void SpatialCompositor::removeSubsurface(WindowHandle handle) {
    scheduleChildRemoval(subsurfaces_, pending_destroy_subsurfaces_, handle);
}

void SpatialCompositor::drainDestroyedSubsurfaces() { drainHostedChildren(pending_destroy_subsurfaces_); }

void SpatialCompositor::releaseSubsurfaces() { releaseHostedChildren(subsurfaces_, pending_destroy_subsurfaces_); }

void SpatialCompositor::releaseChildHost(const std::shared_ptr<SpatialSurfaceHost>& host,
                                        std::shared_ptr<NovaSpatial::TextureHandle>& texture) {
    if (host) {
        // The parent node may already be gone - a client that destroys its
        // toplevel takes its children with it - so detach from whatever parent
        // the node still records rather than looking it up. Removal from the
        // tree is not enough either: focus and the grab are references too.
        if (auto parent = host->parent.lock()) parent->removeChild(host);
        if (scene_) scene_->releaseNode(host);
        host->on_surface_pointer_enter = nullptr;
        host->on_surface_pointer_motion = nullptr;
        host->on_surface_pointer_leave = nullptr;
        host->on_surface_button = nullptr;
        host->on_surface_key = nullptr;
        host->setTexture(nullptr);
    }
    if (texture_bridge_ && texture) texture_bridge_->releaseTexture(texture);
}

template <typename Child>
void SpatialCompositor::drainHostedChildren(std::vector<std::shared_ptr<Child>>& pending) {
    if (pending.empty()) return;

    // Swap out first so a nested schedule during teardown cannot invalidate the
    // iteration, then release outside every wl_listener dispatch frame.
    std::vector<std::shared_ptr<Child>> doomed;
    doomed.swap(pending);
    for (auto& child : doomed) {
        if (!child) continue;
        child->detachListeners();
        releaseChildHost(child->surface_host, child->client_texture);
    }
}

template <typename Child>
void SpatialCompositor::releaseHostedChildren(std::vector<std::shared_ptr<Child>>& live,
                                              std::vector<std::shared_ptr<Child>>& pending) {
    for (auto& child : live) {
        if (!child) continue;
        child->detachListeners();
        pending.push_back(std::move(child));
    }
    live.clear();
    drainHostedChildren(pending);
}

void SpatialCompositor::dismissPopupsOutsidePointer() {
    if (popups_.empty() || !scene_) return;
    const std::shared_ptr<SpatialNode> focus = scene_->getPointerFocus();

    // Copied first: wlr_xdg_popup_destroy runs the popup's own destroy path,
    // which mutates popups_ underneath the iteration. Nested menus fall out of
    // the containment test - a child popup's node is a descendant of its
    // parent's - so a click inside the child leaves the chain standing.
    std::vector<std::shared_ptr<SpatialXdgPopup>> candidates = popups_;
    for (const auto& popup : candidates) {
        if (!popup || !popup->mapped || !popup->popup || !popup->grabbed()) continue;
        if (nodeWithin(focus, popup->surface_host)) continue;
        report(LOGGER::INFO, "SpatialCompositor - Popup %u dismissed: input landed outside its grab",
               popup->handle);
        wlr_xdg_popup_destroy(popup->popup);   // sends xdg_popup.popup_done
    }
}

size_t SpatialCompositor::mappedPopupCount() const { return countMappedChildren(popups_); }

void SpatialCompositor::removePopup(WindowHandle handle) {
    scheduleChildRemoval(popups_, pending_destroy_popups_, handle);
}

void SpatialCompositor::drainDestroyedPopups() { drainHostedChildren(pending_destroy_popups_); }

void SpatialCompositor::releasePopups() { releaseHostedChildren(popups_, pending_destroy_popups_); }

} // namespace Clouds
