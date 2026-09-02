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
//
// It also carries the session lifecycle (plan S.6): startSubstrate() brings the
// substrate up latent - display, backend, outputs, no socket and no global -
// and open() flips the same object to reachable. startServer() is both.
#include "include/Clouds/SpatialCompositor.h"
#include "Core/components/logger.h"

#include <algorithm>
#include <utility>

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

    // wlr_renderer_init_wl_display is NOT called here: it is what creates the
    // wl_shm and linux-dmabuf globals, and a latent session has none. It runs
    // in initProtocols(), with the rest of the protocol surface area.
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
    trackOutput(output);
}

void SpatialCompositor::trackOutput(struct wlr_output* output) {
    auto tracked = std::make_shared<SpatialOutput>();
    tracked->compositor = this;
    tracked->output = output;
    tracked->destroy_listener.bind(tracked.get(), &SpatialOutput::onDestroy, &output->events.destroy);
    outputs_.push_back(std::move(tracked));

    // An output that arrives while the session is already open is advertised at
    // once; one that arrives latent is advertised by open(), with the rest.
    if (stage_ == SessionStage::Open) advertiseOutput(output);

    // Last, and outside the advertise branch: the presentation loop is the one
    // consumer that exists in both stages, and it is what draws the splash.
    if (output_ready_) output_ready_(output);
}

void SpatialCompositor::advertiseOutput(struct wlr_output* output) {
    // Strictly after the commit that enabled the output and set its mode: the
    // global carries the geometry and mode a client enumerates monitors from,
    // and advertising it before the commit publishes a screen with no size.
    // Clients that never see a wl_output see no monitors at all, which is
    // where GTK and Qt stop.
    wlr_output_create_global(output, wl_display_);
    report(LOGGER::INFO, "SpatialCompositor - Output '%s' advertised as a wl_output global (%dx%d)",
           output->name ? output->name : "unnamed", output->width, output->height);
}

void SpatialCompositor::setOutputReadyHandler(OutputReadyHandler handler) {
    output_ready_ = std::move(handler);
    if (!output_ready_) return;

    // Replay: an output that committed before the loop existed is not a
    // different kind of output, and making the caller order these two calls
    // correctly is a rule that would be broken exactly once.
    for (const auto& tracked : outputs_) {
        if (tracked && tracked->output) output_ready_(tracked->output);
    }
}

// --- Session lifecycle (plan S.6): latent -> open -----------------------------

bool SpatialCompositor::initDisplay() {
    if (wl_display_) return true;

    wl_display_ = wl_display_create();
    if (!wl_display_) {
        report(LOGGER::ERROR, "SpatialCompositor - Failed to create Wayland display");
        return false;
    }
    event_loop_ = wl_display_get_event_loop(wl_display_);
    if (!event_loop_) {
        report(LOGGER::ERROR, "SpatialCompositor - Failed to obtain the Wayland event loop");
        return false;
    }
    return true;
}

bool SpatialCompositor::startSubstrate() {
    if (stage_ != SessionStage::Down) {
        report(LOGGER::ERROR, "SpatialCompositor - startSubstrate on a session that is already up");
        return false;
    }
    // wlr_log_init is deliberately absent: it configures a process-global sink,
    // which is the entry point's business, not a session's. See main().
    report(LOGGER::INFO, "SpatialCompositor - Bringing the substrate up latent (no socket, no globals)");

    if (!initDisplay() || !initBackendStack()) return false;

    // Bound before wlr_backend_start, because that is when a backend announces
    // what it has. new_output must be live or the connectors a DRM backend
    // finds at boot are never adopted; new_input is parked on the latent
    // handler until a seat exists to route the devices onto.
    new_output_listener_.bind(this, &SpatialCompositor::onNewOutput, &backend_->events.new_output);
    new_input_listener_.bind(this, &SpatialCompositor::onLatentInput, &backend_->events.new_input);

    if (!wlr_backend_start(backend_)) {
        report(LOGGER::ERROR, "SpatialCompositor - Failed to start wlroots backend");
        return false;
    }
    if (!ensureVirtualOutput()) return false;

    stage_ = SessionStage::Latent;
    report(LOGGER::INFO, "SpatialCompositor - Substrate latent: %zu output(s), 0 sockets, 0 globals",
           outputs_.size());
    return true;
}

bool SpatialCompositor::open(const std::string& socket_name) {
    if (stage_ != SessionStage::Latent) {
        report(LOGGER::ERROR, "SpatialCompositor - open() requires a latent session (stage=%d)",
               static_cast<int>(stage_));
        return false;
    }
    report(LOGGER::INFO, "SpatialCompositor - Opening the session on socket '%s'...", socket_name.c_str());

    if (!initProtocols() || !initInput()) return false;
    adoptLatentInput();
    if (!initSocket(socket_name)) return false;

    // The globals the latent stage withheld. After the socket exists is safe:
    // wl_display_add_socket only starts listening, and no client can be served
    // until the event loop is dispatched, which happens after this returns.
    for (const auto& tracked : outputs_) {
        if (tracked && tracked->output) advertiseOutput(tracked->output);
    }

    stage_ = SessionStage::Open;
    printf("\n======================================================\n"
           "  CLOUDS DISPLAY SERVER ACTIVE\n  Connect clients with:\n"
           "    WAYLAND_DISPLAY=%s <app>\n"
           "======================================================\n\n", socket_name_.c_str());
    fflush(stdout);
    report(LOGGER::INFO, "SpatialCompositor - Wayland display server active on WAYLAND_DISPLAY=%s",
           socket_name_.c_str());
    return true;
}

void SpatialCompositor::onLatentInput(void* data) {
    auto device = static_cast<struct wlr_input_device*>(data);
    if (!device) return;

    // Tracked, not routed: attaching a keyboard needs a wlr_seat, and the seat
    // is what open() adds. The destroy listener is bound now so a device that
    // disappears before open() is removed rather than left dangling.
    auto seat_device = std::make_shared<SpatialSeatDevice>();
    seat_device->compositor = this;
    seat_device->device = device;
    seat_device->destroy_listener.bind(seat_device.get(), &SpatialSeatDevice::onDestroy,
                                       &device->events.destroy);
    input_devices_.push_back(std::move(seat_device));

    report(LOGGER::INFO, "SpatialCompositor - Latent-stage input device held for the seat: %s",
           device->name ? device->name : "unnamed");
}

void SpatialCompositor::adoptLatentInput() {
    // Only the devices onLatentInput parked have neither half bound; a device
    // that arrived through onNewInput was routed when it arrived.
    for (const auto& seat_device : input_devices_) {
        if (!seat_device || !seat_device->device) continue;
        if (seat_device->keyboard || seat_device->pointer) continue;

        if (seat_device->device->type == WLR_INPUT_DEVICE_KEYBOARD) {
            attachKeyboardDevice(*seat_device);
        } else if (seat_device->device->type == WLR_INPUT_DEVICE_POINTER) {
            attachPointerDevice(*seat_device);
        }
    }
    refreshSeatCapabilities();
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
    if (!session_active_) return;

    // A wlr_output only re-arms its frame event on commit, and no frame was
    // committed while the session was away - so without this kick the
    // presentation loop would never be asked to draw again after the switch
    // back. Scheduling one frame per output restarts the cadence; it cannot
    // spin, because it only runs on the inactive -> active edge.
    for (const auto& tracked : outputs_) {
        if (tracked && tracked->output) wlr_output_schedule_frame(tracked->output);
    }
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
