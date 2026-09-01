// Written by Richard Christopher, Copyright 2026 NeoTec Digital
//
// The backend half of the compositor: which wlroots backend this session runs
// on, the libseat session that comes with it on bare metal, and the outputs it
// presents through.
//
// Autocreate is primary. On a TTY that means DRM plus libinput against a real
// wlr_session; nested it means the wayland or x11 backend; and headless is only
// ever chosen when something asked for it. The virtual output below is the
// bridge that keeps the interim SDL window fed while Vazio still presents
// through SDL rather than into a wlr_output, and it goes when that changes.
#include "../../include/Clouds/SpatialCompositor.h"
#include "../../Core/components/logger.h"

#include <algorithm>

namespace Clouds {

namespace {

// wlr_multi_for_each_backend has no early exit, so the search records the first
// headless child it sees and lets the walk finish.
void recordHeadlessChild(struct wlr_backend* child, void* data) {
    auto found = static_cast<struct wlr_backend**>(data);
    if (*found == nullptr && wlr_backend_is_headless(child)) {
        *found = child;
    }
}

} // namespace

bool SpatialCompositor::createBackend() {
    // Autocreate is the boot target: on a TTY it acquires the libseat session
    // and builds DRM + libinput, and when nested it picks the wayland or x11
    // backend. Headless is never chosen implicitly - a compositor that quietly
    // renders to nothing on a machine with a screen is the wrong default.
    if (!config_.headless) {
        backend_ = wlr_backend_autocreate(event_loop_, &session_);
        if (backend_) return true;
        report(LOGGER::ERROR, "SpatialCompositor - wlr_backend_autocreate found no usable backend");
        return false;
    }

    backend_ = wlr_headless_backend_create(event_loop_);
    if (!backend_) {
        report(LOGGER::ERROR, "SpatialCompositor - Failed to create the requested headless backend");
        return false;
    }
    report(LOGGER::INFO, "SpatialCompositor - Headless backend created on explicit request");
    return true;
}

struct wlr_backend* SpatialCompositor::resolveVirtualOutputHost() {
    // A session means real connectors: outputs arrive on new_output and a
    // virtual one would be a second, fictional screen.
    if (session_) return nullptr;
    if (wlr_backend_is_headless(backend_)) return backend_;
    if (!wlr_backend_is_multi(backend_)) return nullptr;

    // WLR_BACKENDS=headless already put one inside the multi-backend; verified
    // against wlroots 0.19.3, which honours the variable in autocreate. Reuse
    // it rather than stacking a second headless backend beside it.
    struct wlr_backend* existing = nullptr;
    wlr_multi_for_each_backend(backend_, recordHeadlessChild, &existing);
    if (existing) return existing;

    // Nested: the wayland/x11 backend is created with no outputs, and Vazio
    // still presents through the interim SDL window. A headless companion
    // carries the output that window stands for. Scaffolding - it goes when
    // Vazio renders into a real wlr_output.
    struct wlr_backend* companion = wlr_headless_backend_create(event_loop_);
    if (!companion) {
        report(LOGGER::ERROR, "SpatialCompositor - Failed to create the headless companion backend");
        return nullptr;
    }
    if (!wlr_multi_backend_add(backend_, companion)) {
        report(LOGGER::ERROR, "SpatialCompositor - Failed to add the headless companion to the multi-backend");
        wlr_backend_destroy(companion);
        return nullptr;
    }
    report(LOGGER::INFO, "SpatialCompositor - Headless companion backend hosting the %ux%u virtual output",
           config_.virtual_width, config_.virtual_height);
    return companion;
}

bool SpatialCompositor::initBackendStack() {
    if (!createBackend()) return false;

    // Must precede wlr_backend_start: a backend added afterwards is never
    // started, so its outputs would exist and never produce a frame.
    virtual_output_host_ = resolveVirtualOutputHost();

    if (session_) {
        session_active_ = session_->active;
        session_active_listener_.bind(this, &SpatialCompositor::onSessionActive, &session_->events.active);
        session_destroy_listener_.bind(this, &SpatialCompositor::onSessionDestroy, &session_->events.destroy);
        report(LOGGER::INFO, "SpatialCompositor - Acquired session on seat '%s' (active=%s)",
               session_->seat, session_active_ ? "yes" : "no");
    }

    renderer_ = wlr_renderer_autocreate(backend_);
    if (!renderer_) {
        report(LOGGER::ERROR, "SpatialCompositor - Failed to autocreate wlroots renderer");
        return false;
    }

    if (!wlr_renderer_init_wl_display(renderer_, wl_display_)) {
        report(LOGGER::ERROR, "SpatialCompositor - Failed to bind renderer buffer protocols to the Wayland display");
        return false;
    }

    allocator_ = wlr_allocator_autocreate(backend_, renderer_);
    if (!allocator_) {
        report(LOGGER::ERROR, "SpatialCompositor - Failed to autocreate wlroots allocator");
        return false;
    }
    return true;
}

void SpatialCompositor::onNewOutput(void* data) {
    auto output = static_cast<struct wlr_output*>(data);
    if (!output) {
        report(LOGGER::ERROR, "SpatialCompositor - new_output signal delivered no output");
        return;
    }

    const char* output_name = output->name ? output->name : "unnamed";
    report(LOGGER::INFO, "SpatialCompositor - New output detected: %s", output_name);

    if (!allocator_ || !renderer_ || !wlr_output_init_render(output, allocator_, renderer_)) {
        report(LOGGER::ERROR, "SpatialCompositor - Failed to initialize render for output '%s'", output_name);
        return;
    }

    struct wlr_output_state state;
    wlr_output_state_init(&state);
    wlr_output_state_set_enabled(&state, true);
    selectOutputMode(output, state);

    if (!wlr_output_commit_state(output, &state)) {
        report(LOGGER::ERROR, "SpatialCompositor - Failed to commit initial state for output '%s'", output_name);
        wlr_output_state_finish(&state);
        return;
    }
    wlr_output_state_finish(&state);
    ++outputs_seen_;
    adoptOutputBox(output);
    publishOutput(output);
}

void SpatialCompositor::publishOutput(struct wlr_output* output) {
    // Strictly after the commit that enabled the output and set its mode: the
    // global carries the geometry and mode a client enumerates monitors from,
    // and advertising it before the commit publishes a screen with no size.
    // Clients that never see a wl_output see no monitors at all, which is
    // where GTK and Qt stop.
    auto tracked = std::make_shared<SpatialOutput>();
    tracked->compositor = this;
    tracked->output = output;
    tracked->destroy_listener.bind(tracked.get(), &SpatialOutput::onDestroy, &output->events.destroy);

    wlr_output_create_global(output, wl_display_);
    outputs_.push_back(std::move(tracked));

    report(LOGGER::INFO, "SpatialCompositor - Output '%s' advertised as a wl_output global (%dx%d)",
           output->name ? output->name : "unnamed", output->width, output->height);
}

void SpatialCompositor::selectOutputMode(struct wlr_output* output, struct wlr_output_state& state) {
    // A cold DRM connector has no current mode, and a custom one it does not
    // advertise will fail to commit - so ask the connector what it prefers
    // before inventing anything. Only a modeless output falls through to the
    // configured extent, which is what headless and nested outputs are.
    if (struct wlr_output_mode* preferred = wlr_output_preferred_mode(output)) {
        wlr_output_state_set_mode(&state, preferred);
        return;
    }
    if (output->current_mode) {
        wlr_output_state_set_mode(&state, output->current_mode);
        return;
    }
    wlr_output_state_set_custom_mode(&state,
                                     static_cast<int32_t>(config_.virtual_width),
                                     static_cast<int32_t>(config_.virtual_height),
                                     0);
}

void SpatialCompositor::adoptOutputBox(struct wlr_output* output) {
    // Single-output scope: the first output to commit a usable size defines the
    // box pointer positions are clamped to, and later outputs do not move it -
    // a second screen appearing must not silently retarget the pointer.
    // wlr_output_layout takes this over when multi-output placement is real.
    if (output_box_adopted_ || output->width <= 0 || output->height <= 0) return;

    output_box_adopted_ = true;
    output_box_.width = output->width;
    output_box_.height = output->height;
    pointer_x_ = std::min(pointer_x_, static_cast<double>(output_box_.width - 1));
    pointer_y_ = std::min(pointer_y_, static_cast<double>(output_box_.height - 1));
    report(LOGGER::INFO, "SpatialCompositor - Output box is now %dx%d",
           output_box_.width, output_box_.height);
}

void SpatialCompositor::onSessionActive(void*) {
    if (!session_) return;
    session_active_ = session_->active;
    // A VT switch revokes the DRM master and the input fds. Nothing is torn
    // down: the caller stops rendering and keeps dispatching, so the switch
    // back finds the same windows, listeners and textures it left.
    report(LOGGER::INFO, "SpatialCompositor - Session became %s",
           session_active_ ? "active" : "inactive");
}

void SpatialCompositor::onSessionDestroy(void*) {
    // Detach while the signal owner is still alive, then behave as a session
    // that never existed: nothing is managing VTs any more, so nothing should
    // be waiting for one to hand the seat back.
    session_active_listener_.unbind();
    session_destroy_listener_.unbind();
    session_ = nullptr;
    session_active_ = true;
}

} // namespace Clouds
