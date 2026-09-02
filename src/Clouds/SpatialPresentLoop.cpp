// Written by Richard Christopher, Copyright 2026 NeoTec Digital
//
// The Option-B presentation loop (plan D.2). See SpatialPresentLoop.h for the
// per-frame sequence and for why the path is chosen once rather than retried.
//
// Buffer-lock discipline, measured against wlroots 0.19.3 rather than assumed
// (scratchpad probe_locks.c): wlr_swapchain_acquire returns the buffer with
// n_locks=1 and that lock is the caller's (swapchain.h:36-42);
// wlr_output_state_set_buffer takes a second, independent lock (n_locks=2);
// wlr_output_commit_state takes none of its own; wlr_output_state_finish drops
// the state's lock (back to 1) and wlr_buffer_unlock drops ours (0, at which
// point the swapchain slot is released). So the order below - set_buffer,
// commit, state_finish, unlock - keeps our own reference alive across the whole
// of the frame's use of the buffer, which is the point of holding one.
#include "include/Clouds/SpatialPresentLoop.h"
#include "include/Clouds/SpatialCompositor.h"
#include "Core/components/logger.h"

#include <algorithm>
#include <cstring>
#include <ctime>

namespace Clouds {

namespace {

// Translate wlroots' DMA-BUF description into Nova's. The fds stay borrowed:
// Nova dups what it keeps, and these belong to the wlr_buffer.
NovaDmabufAttributes toNovaAttributes(const struct wlr_dmabuf_attributes& src) {
    NovaDmabufAttributes out = {};
    out.width = static_cast<uint32_t>(src.width);
    out.height = static_cast<uint32_t>(src.height);
    out.drm_format = src.format;
    out.modifier = src.modifier;
    out.plane_count = std::min(src.n_planes, NOVA_DMABUF_MAX_PLANES);
    for (int plane = 0; plane < out.plane_count; ++plane) {
        out.planes[plane].fd = src.fd[plane];
        out.planes[plane].offset = src.offset[plane];
        out.planes[plane].stride = src.stride[plane];
    }
    return out;
}

} // namespace

const char* presentPathName(PresentPath path) {
    switch (path) {
        case PresentPath::DmabufImport:  return "dmabuf-import";
        case PresentPath::PixmanSidecar: return "pixman-sidecar";
        case PresentPath::Undecided:     break;
    }
    return "undecided";
}

// --- Output bindings ---------------------------------------------------------

SpatialPresentLoop::Output::~Output() {
    frame_listener.unbind();
    present_listener.unbind();
    commit_listener.unbind();
    destroy_listener.unbind();

    // Owned by this binding, not by the output: the sidecar swapchain belongs
    // to the loop's own allocator, which stop() destroys strictly after this.
    if (sidecar_swapchain) {
        wlr_swapchain_destroy(sidecar_swapchain);
        sidecar_swapchain = nullptr;
    }
}

void SpatialPresentLoop::Output::onFrame(void*) {
    if (loop) loop->presentFrame(*this);
}

void SpatialPresentLoop::Output::onPresent(void* data) {
    auto event = static_cast<struct wlr_output_event_present*>(data);
    if (!drives_frame_callbacks || !loop || !loop->compositor_ || !event || !event->presented) return;

    // The authoritative presentation time, which is exactly what plan D.2 said
    // this would become once presentation stopped being "the render submission
    // returned". Only one output drives it: frame_done is per client, not per
    // screen, and two outputs would release every callback twice.
    loop->compositor_->onFramePresented(event->when);
}

void SpatialPresentLoop::Output::onCommit(void* data) {
    auto event = static_cast<struct wlr_output_event_commit*>(data);
    if (!loop || !event || !event->state) return;
    loop->noteOutputCommit(*this, *event->state);
}

void SpatialPresentLoop::Output::onDestroy(void*) {
    // The signals are being torn down around us, so detach first. The entry
    // itself is only marked dead: deleting the object whose listener is being
    // dispatched is what WaylandListener forbids, so reapDeadOutputs() erases
    // it at the next attach() or frame instead.
    frame_listener.unbind();
    present_listener.unbind();
    commit_listener.unbind();
    destroy_listener.unbind();
    output = nullptr;

    // Handing frame_done on is NOT deferrable: nothing schedules a frame on a
    // screen that is gone, so a loop that waited for the reap would leave every
    // throttled client stalled on a callback no output is left to release.
    // Re-election only writes other entries' flags, so it is safe here.
    const bool was_driver = drives_frame_callbacks;
    drives_frame_callbacks = false;
    if (loop && was_driver) loop->electFrameCallbackDriver();
}

// --- Construction / teardown -------------------------------------------------

SpatialPresentLoop::SpatialPresentLoop(NovaGraphics* graphics, SpatialCompositor* compositor)
    : graphics_(graphics), compositor_(compositor) {}

SpatialPresentLoop::~SpatialPresentLoop() {
    stop();
}

void SpatialPresentLoop::stop() {
    outputs_.clear();

    while (!imports_.empty()) {
        releaseImport(imports_.back().get());
        imports_.pop_back();
    }

    destroySidecarTarget();
    if (sidecar_allocator_) {
        wlr_allocator_destroy(sidecar_allocator_);
        sidecar_allocator_ = nullptr;
    }
    if (sidecar_renderer_) {
        wlr_renderer_destroy(sidecar_renderer_);
        sidecar_renderer_ = nullptr;
    }

    path_ = PresentPath::Undecided;
    render_pass_ = VK_NULL_HANDLE;
    target_format_ = VK_FORMAT_UNDEFINED;
}

// --- Output bookkeeping ------------------------------------------------------

bool SpatialPresentLoop::hasFrameCallbackDriver() const {
    for (const auto& bound : outputs_) {
        if (bound && bound->live() && bound->drives_frame_callbacks) return true;
    }
    return false;
}

void SpatialPresentLoop::electFrameCallbackDriver() {
    if (hasFrameCallbackDriver()) return;

    for (const auto& bound : outputs_) {
        if (!bound || !bound->live()) continue;
        bound->drives_frame_callbacks = true;
        report(LOGGER::INFO, "SpatialPresentLoop - Output '%s' now drives frame callbacks",
               bound->output->name ? bound->output->name : "unnamed");
        return;
    }

    // No live output left, so no presentation is happening and there is no
    // honest presentation time to hand out. Clients stop being throttled by
    // this loop rather than being throttled against a screen that is gone;
    // whichever output is plugged in next takes the role at attach().
    report(LOGGER::WARN, "SpatialPresentLoop - No live output drives frame callbacks; "
                         "clients are no longer throttled by presentation");
}

void SpatialPresentLoop::reapDeadOutputs() {
    // Called outside listener dispatch only: erasing runs ~Output, which
    // unbinds listeners and destroys the sidecar swapchain.
    outputs_.erase(std::remove_if(outputs_.begin(), outputs_.end(),
                                  [](const std::unique_ptr<Output>& bound) {
                                      return !bound || !bound->live();
                                  }),
                   outputs_.end());
}

void SpatialPresentLoop::noteOutputCommit(Output& bound, const struct wlr_output_state& state) {
    if (path_ != PresentPath::PixmanSidecar || !bound.output) return;

    // This loop's own per-frame commits carry BUFFER and DAMAGE only, so they
    // never land here; only a mode set or an enable can move the extent, and by
    // `commit` (emitted right after) the output already reports the new one.
    constexpr uint32_t kGeometryFields = WLR_OUTPUT_STATE_MODE | WLR_OUTPUT_STATE_ENABLED;
    if ((state.committed & kGeometryFields) == 0) return;

    const VkExtent2D extent = { static_cast<uint32_t>(bound.output->width),
                                static_cast<uint32_t>(bound.output->height) };
    if (extent.width == bound.sidecar_extent.width && extent.height == bound.sidecar_extent.height) {
        return;
    }

    bound.sidecar_needs_rebuild = true;
    report(LOGGER::INFO, "SpatialPresentLoop - Output '%s' changed mode to %ux%u; sidecar rebuild queued",
           bound.output->name ? bound.output->name : "unnamed", extent.width, extent.height);

    // `frame` only re-arms on a commit this loop made, and the commit that just
    // happened was not one of ours, so without this kick the rebuild would wait
    // for a frame that never comes.
    wlr_output_schedule_frame(bound.output);
}

void SpatialPresentLoop::releaseImport(ImportedBuffer* entry) {
    if (!entry) return;
    entry->destroy_listener.unbind();
    if (graphics_ && entry->image.valid()) {
        graphics_->releaseImportedImage(entry->image);
    }
}

void SpatialPresentLoop::ImportedBuffer::onDestroy(void*) {
    // The buffer is going away, so the VkImage built on its fds goes with it.
    // The entry itself is only marked dead, never freed here: this runs inside
    // the buffer's own destroy dispatch and deleting the object that owns the
    // listener being dispatched is exactly what WaylandListener forbids.
    // importedImageFor() reaps it on the next lookup.
    if (loop) loop->releaseImport(this);
    buffer = nullptr;
}

// --- Path selection ----------------------------------------------------------

bool SpatialPresentLoop::probeDmabufImport(struct wlr_output* output) {
    // Decisive rather than inferred: allocate the real primary swapchain, take
    // a real buffer out of it and try the real import. Caps alone cannot say
    // whether this device can import THIS modifier.
    struct wlr_output_state state;
    wlr_output_state_init(&state);
    const bool configured =
        wlr_output_configure_primary_swapchain(output, &state, &output->swapchain);
    wlr_output_state_finish(&state);
    if (!configured) return false;

    struct wlr_buffer* buffer = wlr_swapchain_acquire(output->swapchain);
    if (!buffer) return false;

    const bool ok = importedImageFor(buffer) != nullptr;
    wlr_buffer_unlock(buffer);
    return ok;
}

// Decided once, and only ever COMMITTED once: every candidate value stays in a
// local until the render pass - the last thing that can fail - exists. A run
// that failed half way used to leave path_ decided and render_pass_ null, which
// the `path_ != Undecided` short-circuit then accepted, and attach() went on to
// bind listeners for a loop that could never render.
bool SpatialPresentLoop::selectPath(struct wlr_output* output) {
    if (path_ != PresentPath::Undecided) return render_pass_ != VK_NULL_HANDLE;

    const uint32_t drm_format = output->render_format ? output->render_format : kPresentFallbackDrmFormat;
    const bool caps_allow = output->renderer != nullptr &&
                            (output->renderer->render_buffer_caps & WLR_BUFFER_CAP_DMABUF) != 0;

    PresentPath chosen = PresentPath::PixmanSidecar;
    VkFormat format = novaVulkanFormatFromDrm(kPresentFallbackDrmFormat);
    VkImageLayout layout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
    if (graphics_ && graphics_->supportsDmabufImport() && caps_allow && probeDmabufImport(output)) {
        chosen = PresentPath::DmabufImport;
        format = novaVulkanFormatFromDrm(drm_format);
        layout = VK_IMAGE_LAYOUT_GENERAL;
    }

    VkRenderPass pass = graphics_ ? graphics_->getOffscreenRenderPass(format, layout) : VK_NULL_HANDLE;
    if (pass == VK_NULL_HANDLE) {
        report(LOGGER::ERROR,
               "SpatialPresentLoop - No render pass for %s over %s; leaving the path undecided so a "
               "later attach re-probes",
               presentPathName(chosen), novaDrmFormatName(drm_format));
        return false;
    }

    path_ = chosen;
    target_format_ = format;
    target_layout_ = layout;
    render_pass_ = pass;
    report(LOGGER::INFO,
           "SpatialPresentLoop - Presentation path: %s (output format %s, dmabuf import %s, "
           "renderer dmabuf caps %s)",
           presentPathName(path_), novaDrmFormatName(drm_format),
           (graphics_ && graphics_->supportsDmabufImport()) ? "available" : "unavailable",
           caps_allow ? "yes" : "no");
    return true;
}

// --- Attach ------------------------------------------------------------------

// The CPU path owns exactly ONE loop-wide render target and readback buffer, so
// a second output of a different size would resize the buffer the first one's
// per-frame row copy reads from - a heap overflow into a scanout buffer, not a
// wrong picture. Refusing is the honest Phase-3a answer: the sidecar is the
// fallback for hardware that cannot import, not a multi-head presentation path.
// Per-output sidecar targets are the fix if multi-head on non-importable
// hardware ever becomes a requirement.
bool SpatialPresentLoop::sidecarSlotFree(struct wlr_output* candidate) const {
    if (path_ != PresentPath::PixmanSidecar || outputs_.empty()) return true;

    const struct wlr_output* holder = outputs_.front()->output;
    report(LOGGER::ERROR,
           "SpatialPresentLoop - Output '%s' refused: the pixman sidecar drives one output only, "
           "and '%s' already holds it",
           candidate && candidate->name ? candidate->name : "unnamed",
           holder && holder->name ? holder->name : "unnamed");
    return false;
}

bool SpatialPresentLoop::attach(struct wlr_output* output) {
    if (!output || !graphics_) return false;

    // Unplugged screens are erased here rather than accumulating across
    // hotplug, and doing it first is what lets the CPU path accept a
    // replacement output after its only one went away.
    reapDeadOutputs();

    const auto already = std::find_if(outputs_.begin(), outputs_.end(),
                                      [output](const std::unique_ptr<Output>& bound) {
                                          return bound->output == output;
                                      });
    if (already != outputs_.end()) return true;

    const PresentPath before = path_;
    if (!selectPath(output)) {
        report(LOGGER::ERROR, "SpatialPresentLoop - No usable presentation path for output '%s'",
               output->name ? output->name : "unnamed");
        return false;
    }
    if (before != PresentPath::Undecided && before != path_) {
        report(LOGGER::ERROR, "SpatialPresentLoop - Output '%s' needs a different path; refused",
               output->name ? output->name : "unnamed");
        return false;
    }

    if (!sidecarSlotFree(output)) return false;

    auto bound = std::make_unique<Output>();
    bound->loop = this;
    bound->output = output;
    bound->drives_frame_callbacks = !hasFrameCallbackDriver();

    if (path_ == PresentPath::PixmanSidecar && !ensureSidecar(*bound, output)) return false;

    bound->frame_listener.bind(bound.get(), &Output::onFrame, &output->events.frame);
    bound->present_listener.bind(bound.get(), &Output::onPresent, &output->events.present);
    bound->commit_listener.bind(bound.get(), &Output::onCommit, &output->events.commit);
    bound->destroy_listener.bind(bound.get(), &Output::onDestroy, &output->events.destroy);
    outputs_.push_back(std::move(bound));

    // A `frame` event only re-arms on commit, so an output that has never been
    // committed by this loop would sit silent forever without a first kick.
    wlr_output_schedule_frame(output);
    report(LOGGER::INFO, "SpatialPresentLoop - Driving output '%s' (%dx%d) over %s",
           output->name ? output->name : "unnamed", output->width, output->height,
           presentPathName(path_));
    return true;
}

// --- The frame ---------------------------------------------------------------

void SpatialPresentLoop::presentFrame(Output& bound) {
    struct wlr_output* output = bound.output;
    if (!output || !scene_renderer_) return;

    // A VT switch away revokes the DRM master: committing would fail and
    // rendering into a buffer nobody will scan out is wasted work. Reschedule
    // nothing - the session coming back schedules the next frame.
    if (compositor_ && !compositor_->isSessionActive()) return;

    // A mode change the output committed for itself is acted on here rather
    // than in the commit dispatch that saw it, so the swapchain being replaced
    // is never the one that commit is still standing on.
    if (bound.sidecar_needs_rebuild && !rebuildSidecar(bound)) {
        ++failures_;
        return;
    }

    struct wlr_output_state state;
    wlr_output_state_init(&state);

    struct wlr_swapchain** swapchain =
        (path_ == PresentPath::PixmanSidecar) ? &bound.sidecar_swapchain : &output->swapchain;
    if (path_ != PresentPath::PixmanSidecar &&
        !wlr_output_configure_primary_swapchain(output, &state, swapchain)) {
        wlr_output_state_finish(&state);
        ++failures_;
        return;
    }

    struct wlr_buffer* buffer = *swapchain ? wlr_swapchain_acquire(*swapchain) : nullptr;
    if (!buffer) {
        wlr_output_state_finish(&state);
        ++failures_;
        return;
    }

    const bool drawn = (path_ == PresentPath::DmabufImport) ? presentImported(bound, buffer)
                                                            : presentSidecar(bound, buffer);
    if (drawn) {
        wlr_output_state_set_buffer(&state, buffer);
        if (wlr_output_commit_state(output, &state)) ++commits_;
        else ++failures_;
    } else {
        ++failures_;
    }

    wlr_output_state_finish(&state);
    wlr_buffer_unlock(buffer);
}

// --- DMA-BUF import path -----------------------------------------------------

const NovaImportedImage* SpatialPresentLoop::importedImageFor(struct wlr_buffer* buffer) {
    // Reap the entries whose buffers were destroyed since the last lookup.
    imports_.erase(std::remove_if(imports_.begin(), imports_.end(),
                                  [](const std::unique_ptr<ImportedBuffer>& held) {
                                      return held->buffer == nullptr;
                                  }),
                   imports_.end());

    for (const auto& held : imports_) {
        if (held->buffer == buffer) return held->image.valid() ? &held->image : nullptr;
    }

    struct wlr_dmabuf_attributes attrs = {};
    if (!wlr_buffer_get_dmabuf(buffer, &attrs)) return nullptr;

    auto entry = std::make_unique<ImportedBuffer>();
    entry->loop = this;
    entry->buffer = buffer;
    const NovaDmabufAttributes nova_attrs = toNovaAttributes(attrs);
    if (!graphics_->importDmabufAsImage(nova_attrs, entry->image)) {
        report(LOGGER::ERROR, "SpatialPresentLoop - Cannot import output buffer (%s, modifier 0x%llx)",
               novaDrmFormatName(attrs.format), static_cast<unsigned long long>(attrs.modifier));
        return nullptr;
    }

    entry->destroy_listener.bind(entry.get(), &ImportedBuffer::onDestroy, &buffer->events.destroy);
    const NovaImportedImage* image = &entry->image;
    imports_.push_back(std::move(entry));
    report(LOGGER::INFO, "SpatialPresentLoop - Imported output buffer %p (%ux%u %s), %zu cached",
           static_cast<const void*>(buffer), image->extent.width, image->extent.height,
           novaDrmFormatName(image->drm_format), imports_.size());
    return image;
}

bool SpatialPresentLoop::presentImported(Output&, struct wlr_buffer* buffer) {
    const NovaImportedImage* image = importedImageFor(buffer);
    if (!image) return false;

    // asRenderTarget sets external_consumer, which is what releases the image
    // to VK_QUEUE_FAMILY_FOREIGN_EXT after the pass. Required, not defensive:
    // KMS reading a DCC-compressed radv image without it gets the compressed
    // representation, measured.
    const NovaRenderTarget target = image->asRenderTarget(target_layout_);
    const VkExtent2D extent = target.extent;
    VkFence fence = graphics_->renderToImage(target, [this, extent](VkCommandBuffer cmd, uint32_t) {
        scene_renderer_(cmd, extent);
    });
    if (fence == VK_NULL_HANDLE) return false;

    // Bring-up synchronisation is fence-and-wait (plan D.2): the buffer is
    // handed to a consumer outside this VkDevice the moment it is committed.
    return graphics_->waitForRender(fence);
}

} // namespace Clouds
