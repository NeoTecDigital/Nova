// Written by Richard Christopher, Copyright 2026 NeoTec Digital
//
// Subsurface half of the compositor: the SpatialSubsurface lifetime object and
// every SpatialCompositor member that creates, places or anchors one.
//
// It used to be split across SpatialCompositor.cpp and SpatialPopupHost.cpp for
// no reason but the 500-line cap on both, with a comment saying so; this is
// that TU. A subsurface follows the popup's discipline - detach and schedule,
// release outside dispatch - minus the configure and minus the positioner: the
// position is applied by the parent's commit and the size is whatever the child
// committed, so onCommit is import-and-place.
//
// What stays in SpatialPopupHost.cpp is the kill-list plumbing alone
// (mappedSubsurfaceCount, removeSubsurface, drainDestroyedSubsurfaces,
// releaseSubsurfaces): popups and subsurfaces are one hosted-child lifetime
// there, written once over the element type.
#include "./SpatialCompositor.h"
#include "Nova/components/logger.h"

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

// Every listener a hosted subsurface owns. Split out for the same reason
// bindWindowListeners and bindPopupListeners are: the binding is a fixed list
// and the creation path is about everything else.
void bindSubsurfaceListeners(SpatialSubsurface& child, struct wlr_subsurface* subsurface) {
    SpatialSubsurface* self = &child;
    struct wlr_surface* surface = child.surface;
    self->commit_listener.bind(self, &SpatialSubsurface::onCommit, &surface->events.commit);
    self->map_listener.bind(self, &SpatialSubsurface::onMap, &surface->events.map);
    self->unmap_listener.bind(self, &SpatialSubsurface::onUnmap, &surface->events.unmap);
    self->surface_destroy_listener.bind(self, &SpatialSubsurface::onSurfaceDestroy,
                                        &surface->events.destroy);
    self->destroy_listener.bind(self, &SpatialSubsurface::onDestroy, &subsurface->events.destroy);
    self->new_subsurface_listener.bind(self, &SpatialSubsurface::onNewSubsurface,
                                       &surface->events.new_subsurface);
}

} // namespace

// --- SpatialSubsurface ---

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

// --- SpatialCompositor - subsurface hosting ---

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
    // Detached until it maps, which is when it is reparented onto its anchor.
    Splash::Registry* reg = registry();
    if (!reg) return;   // no scene: there is no tree for a subsurface to sit in
    hosted->surface_host = reg->emplace<Splash::SpatialSurfaceHost>(
        Splash::INVALID_NODE,
        glm::vec2(kSubsurfaceFallbackExtent, kSubsurfaceFallbackExtent), fallback_texture);
    (*reg)[hosted->surface_host].name = "Subsurface";
    (*reg)[hosted->surface_host].claims_pointer_input = true;

    SpatialSubsurface* child = hosted.get();
    struct wlr_surface* surface = hosted->surface;
    bindSubsurfaceListeners(*child, subsurface);
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

void SpatialCompositor::attachSubsurfaceToParent(SpatialSubsurface& sub) {
    Splash::Registry* reg = registry();
    const Splash::NodeId anchor = hostNodeForSurface(sub.parent_surface);
    if (!reg || !reg->alive(anchor) || !reg->alive(sub.surface_host)) {
        report(LOGGER::ERROR, "SpatialCompositor - Subsurface %u has no hosted parent; not shown", sub.handle);
        return;
    }

    placeSubsurfaceOnParent(sub);
    reg->reparent(sub.surface_host, anchor);
    report(LOGGER::INFO, "SpatialCompositor - Subsurface %u mapped onto its parent surface node", sub.handle);
}

void SpatialCompositor::detachSubsurfaceFromParent(SpatialSubsurface& sub) {
    Splash::Registry* reg = registry();
    if (!reg || !reg->alive(sub.surface_host)) return;

    reg->reparent(sub.surface_host, Splash::INVALID_NODE);
    report(LOGGER::INFO, "SpatialCompositor - Subsurface %u unmapped from its parent node", sub.handle);
}

void SpatialCompositor::placeSubsurfaceOnParent(SpatialSubsurface& sub) {
    const Splash::NodeId anchor = hostNodeForSurface(sub.parent_surface);
    if (!anchor.valid() || !sub.surface_host.valid() || !sub.subsurface || !sub.surface ||
        !sub.parent_surface) return;

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
    placeChildOnParentQuad(sub.surface_host, anchor, *sub.parent_surface, box,
                           kSubsurfaceDepthBias * static_cast<float>(1 + paint_index));
}

void SpatialCompositor::importSubsurfaceBuffer(SpatialSubsurface& sub) {
    importSurfaceContent(sub.surface, sub.surface_host, sub.client_texture,
                         sub.unsupported_format, sub.handle);
}

} // namespace Vazio
