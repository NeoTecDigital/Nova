// Written by Richard Christopher, Copyright 2026 NeoTec Digital
//
// lt-class lifetime harness, now in-repo and under CTest.
//
// Three full connect / map / disconnect cycles against ONE long-lived
// SpatialCompositor, then a double stop(). What it is looking for is the class
// of bug a single-cycle test cannot see: listeners left bound to a dead client,
// window entries never reaped, an event loop that survives one disconnect and
// not two, and a teardown that is only safe the first time it runs.
//
// No Vulkan, no renderer of our own: headless wlroots and a null Core.

#include "harness_check.h"
#include "wl_client_kit.h"

#include "Vazio/SpatialCompositor.h"
#include "Splash/SpatialScene.h"

#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

#include <cstdlib>
#include <cstring>
#include <string>

namespace {

using VazioTest::CheckLog;
using VazioTest::ClientState;
using VazioTest::ShmBuffer;

constexpr int kCycles = 3;
constexpr int32_t kClientWidth = 64;
constexpr int32_t kClientHeight = 48;

void onXdgSurfaceConfigure(void* data, struct xdg_surface* surface, uint32_t serial) {
    auto* state = static_cast<ClientState*>(data);
    state->last_configure_serial = serial;
    ++state->configure_count;
    xdg_surface_ack_configure(surface, serial);
}

const struct xdg_surface_listener kXdgSurfaceListener = { .configure = onXdgSurfaceConfigure };

void onToplevelConfigure(void* data, struct xdg_toplevel*, int32_t, int32_t, struct wl_array*) {
    ++static_cast<ClientState*>(data)->toplevel_configure_count;
}
void onToplevelClose(void*, struct xdg_toplevel*) {}
void onToplevelBounds(void*, struct xdg_toplevel*, int32_t, int32_t) {}
void onToplevelCaps(void*, struct xdg_toplevel*, struct wl_array*) {}

const struct xdg_toplevel_listener kToplevelListener = {
    .configure = onToplevelConfigure,
    .close = onToplevelClose,
    .configure_bounds = onToplevelBounds,
    .wm_capabilities = onToplevelCaps,
};

/**
 * The body of one cycle's client: connect, map a real surface, wait for the
 * compositor to pace it, then tear the protocol objects down in order.
 *
 * Split from runCycleClient so that every wl_ proxy this function owns - the
 * ShmBuffer's wl_buffer above all - is destroyed BEFORE wl_display_disconnect
 * runs. A proxy that outlives its display is a use-after-free inside
 * libwayland, and it shows up as a child that dies on a signal after doing
 * everything right.
 */
int runCycleClientBody(ClientState& state, const char* socket_name) {
    if (!VazioTest::connectClient(state, socket_name)) {
        fprintf(stderr, "cycle client: connect/bind failed on '%s'\n", socket_name);
        return 2;
    }

    struct wl_surface* surface = wl_compositor_create_surface(state.globals.compositor);
    struct xdg_surface* xdg = xdg_wm_base_get_xdg_surface(state.globals.wm_base, surface);
    xdg_surface_add_listener(xdg, &kXdgSurfaceListener, &state);
    struct xdg_toplevel* toplevel = xdg_surface_get_toplevel(xdg);
    xdg_toplevel_add_listener(toplevel, &kToplevelListener, &state);
    xdg_toplevel_set_title(toplevel, "vazio-lifetime-cycle");
    wl_surface_commit(surface);

    if (!VazioTest::pumpUntil(state, [&] { return state.configure_count > 0; }, 8000)) {
        fprintf(stderr, "cycle client: no initial configure\n");
        return 3;
    }

    ShmBuffer buffer;
    if (!buffer.create(state.globals.shm, kClientWidth, kClientHeight, WL_SHM_FORMAT_ARGB8888)) {
        fprintf(stderr, "cycle client: shm buffer allocation failed\n");
        return 4;
    }
    buffer.fillPattern(0x11);
    wl_surface_attach(surface, buffer.buffer(), 0, 0);
    wl_surface_damage_buffer(surface, 0, 0, kClientWidth, kClientHeight);
    VazioTest::requestFrameCallback(state, surface);
    wl_surface_commit(surface);

    if (!VazioTest::pumpUntil(state, [&] { return state.frame_done_count > 0; }, 10000)) {
        fprintf(stderr, "cycle client: no frame callback - the surface did not map\n");
        return 5;
    }

    // Tear down in protocol order rather than dropping the socket: a compositor
    // that only survives an abrupt disconnect has not been tested on the path
    // every real client actually takes.
    xdg_toplevel_destroy(toplevel);
    xdg_surface_destroy(xdg);
    wl_surface_destroy(surface);
    wl_display_flush(state.display);
    return 0;
}

int runCycleClient(const char* socket_name) {
    ClientState state;
    const int result = runCycleClientBody(state, socket_name);
    VazioTest::disconnectClient(state);
    return result;
}

struct Server {
    // Declared first, destroyed last: the scene hands its subtree back through
    // it on the way out.
    Splash::Registry registry;
    std::shared_ptr<Splash::SpatialScene> scene;
    std::unique_ptr<Vazio::SpatialCompositor> compositor;
    std::string socket_name;

    void pump() {
        compositor->iterateEventLoop(0);
        struct timespec now = {};
        clock_gettime(CLOCK_MONOTONIC, &now);
        compositor->onFramePresented(now);
    }
};

// Peak occupancy the compositor reached while one cycle's client was connected.
struct CyclePeak {
    size_t windows = 0;
    size_t mapped = 0;
    int status = 0;
    bool reaped = false;
};

// Pump the server until the forked client exits, sampling what the compositor
// held while it was connected.
CyclePeak awaitCycleClient(Server& server, pid_t child, bool dispatch) {
    CyclePeak peak;
    const int64_t deadline = VazioTest::monotonicMs() + 30000;

    while (VazioTest::monotonicMs() < deadline) {
        if (dispatch) server.pump();
        peak.windows = std::max(peak.windows, server.compositor->windowCount());
        peak.mapped = std::max(peak.mapped, server.compositor->mappedWindowCount());

        const pid_t reaped = waitpid(child, &peak.status, WNOHANG);
        if (reaped == child) { peak.reaped = true; break; }
        if (reaped < 0) break;
        if (!dispatch) usleep(2000);
    }

    if (!peak.reaped) {
        kill(child, SIGKILL);
        waitpid(child, &peak.status, 0);
    }
    return peak;
}

// Everything the compositor must have let go of once the client has left.
void gradeCycleTeardown(Server& server, int index, CheckLog& log) {
    // The disconnect must be REAPED, not merely survived: a window entry left
    // behind holds listeners bound to a destroyed wl_resource.
    const int64_t drain_until = VazioTest::monotonicMs() + 2000;
    while (server.compositor->windowCount() > 0 && VazioTest::monotonicMs() < drain_until) {
        server.pump();
    }
    log.check(server.compositor->windowCount() == 0,
              "cycle %d: the window was reaped after the client left (%zu remain)",
              index, server.compositor->windowCount());
    log.check(server.compositor->outputCount() == 1,
              "cycle %d: still exactly one output - no output leaked across the cycle (%zu)",
              index, server.compositor->outputCount());
}

bool runCycle(Server& server, int index, CheckLog& log, bool dispatch) {
    const pid_t child = fork();
    if (child < 0) {
        log.check(false, "cycle %d: fork failed", index);
        return false;
    }
    if (child == 0) {
        _exit(runCycleClient(server.socket_name.c_str()));
    }

    const CyclePeak peak = awaitCycleClient(server, child, dispatch);
    if (!peak.reaped) {
        log.check(false, "cycle %d: client did not finish within the timeout", index);
        return false;
    }

    const bool exited = WIFEXITED(peak.status);
    log.check(exited, "cycle %d: client exited normally rather than on a signal", index);
    log.check(exited && WEXITSTATUS(peak.status) == 0,
              "cycle %d: client connected, mapped and disconnected cleanly (exit %d)",
              index, exited ? WEXITSTATUS(peak.status) : -1);
    log.check(peak.windows == 1, "cycle %d: the compositor hosted exactly one toplevel (peak %zu)",
              index, peak.windows);
    log.check(peak.mapped == 1, "cycle %d: that toplevel mapped (peak %zu)", index, peak.mapped);

    gradeCycleTeardown(server, index, log);
    return exited && WEXITSTATUS(peak.status) == 0;
}

bool prepareRuntimeDir(CheckLog& log) {
    std::string error;
    if (VazioTest::prepareRuntimeDir(error)) return true;
    log.check(false, "setup: %s", error.c_str());
    return false;
}

}  // namespace

namespace {

bool buildServer(Server& server, CheckLog& log) {
    server.socket_name = "wayland-vzl-" + std::to_string(getpid());
    server.scene = std::make_shared<Splash::SpatialScene>(server.registry, nullptr, nullptr);

    const Vazio::SpatialCompositorConfig config = { .headless = true,
                                                     .virtual_width = 800,
                                                     .virtual_height = 600 };
    server.compositor = std::make_unique<Vazio::SpatialCompositor>(
        nullptr, nullptr, server.scene, server.scene->root, config);

    return log.check(server.compositor->startServer(server.socket_name),
                     "server: startServer('%s') succeeded", server.socket_name.c_str());
}

// stop() is documented idempotent. Proving it means calling it more than once
// and then asking the object what it thinks its state is - a double free here is
// the whole reason the claim is worth a test.
void gradeTeardown(Server& server, CheckLog& log) {
    server.compositor->stop();
    log.check(server.compositor->stage() == Vazio::SessionStage::Down,
              "teardown: stop() left the session Down");
    server.compositor->stop();
    server.compositor->stop();
    log.check(server.compositor->stage() == Vazio::SessionStage::Down,
              "teardown: two further stop() calls are no-ops");
    log.check(server.compositor->windowCount() == 0 && server.compositor->popupCount() == 0,
              "teardown: no windows or popups survive the stop");

    server.compositor.reset();
    server.scene.reset();
}

}  // namespace

int main() {
    CheckLog log("disconnect_cycle");
    log.note("negative control: %s", VazioTest::negativeControlName());

    if (!prepareRuntimeDir(log)) return log.report();
    wlr_log_init(WLR_ERROR, nullptr);

    Server server;
    if (!buildServer(server, log)) return log.report();
    log.check(server.compositor->stage() == Vazio::SessionStage::Open,
              "server: session is Open before the first cycle");

    for (int cycle = 1; cycle <= kCycles; ++cycle) {
        // NEG=earlystop: tear the session down before the last cycle. Cycle 3
        // must then fail to connect, which is exactly what the assertions above
        // are supposed to notice.
        if (cycle == kCycles && VazioTest::negativeControl("earlystop")) {
            log.note("NEG=earlystop: stopping the session before cycle %d", cycle);
            server.compositor->stop();
        }

        // NEG=nodispatch: stop servicing the Wayland event loop for one cycle.
        // The client then blocks on its first roundtrip and the cycle fails.
        const bool dispatch = !(cycle == 2 && VazioTest::negativeControl("nodispatch"));
        if (!dispatch) log.note("NEG=nodispatch: not dispatching during cycle %d", cycle);

        runCycle(server, cycle, log, dispatch);
    }

    gradeTeardown(server, log);
    return log.report();
}
