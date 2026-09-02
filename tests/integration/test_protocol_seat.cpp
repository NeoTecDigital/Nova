// Written by Richard Christopher, Copyright 2026 NeoTec Digital
//
// p1-class protocol and seat harness, now in-repo and under CTest.
//
// Server half: a real Clouds::SpatialCompositor on the wlroots headless
// backend, driven by this process's own event loop. Client half: a forked
// process running the raw wayland-client script in protocol_client_*.cpp.
// Neither half mocks the other - the only thing between them is a Wayland
// socket in a private XDG_RUNTIME_DIR that this harness creates and removes.
//
// No Vulkan: the compositor is constructed with a null NovaCore and a null
// TextureBridge, which is the configuration in which it hosts surfaces, routes
// the seat and answers the protocol without ever touching a GPU. The one thing
// that configuration cannot assert is the pixel-level upload into a texture -
// see the import phase in protocol_client_shell.cpp for what is asserted instead.

#include "protocol_client.h"

#include "Clouds/Primitives.h"
#include "Clouds/SpatialCompositor.h"
#include "Clouds/SpatialScene.h"

#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

#include <cstdlib>
#include <string>

namespace {

using VazioTest::CheckLog;
using VazioTest::PhaseChannel;
using VazioTest::PhasePipes;

constexpr uint32_t kOutputWidth = 1600;
constexpr uint32_t kOutputHeight = 1000;
constexpr uint32_t kBtnLeft = 0x110;

// Coarse enough to be fast, fine enough that a 1.2 x 0.75 world-unit quad
// cannot fall between two samples at this camera distance.
constexpr int kScanStep = 8;

struct Server {
    std::shared_ptr<Clouds::SpatialScene> scene;
    std::unique_ptr<Clouds::SpatialCompositor> compositor;
    std::string socket_name;

    // Frame callbacks are the compositor's obligation once per presented frame.
    // Nothing here renders, so the harness stands in for the presentation loop.
    void pump() {
        compositor->iterateEventLoop(0);
        struct timespec now = {};
        clock_gettime(CLOCK_MONOTONIC, &now);
        compositor->onFramePresented(now);
    }
};

std::shared_ptr<Clouds::SpatialSurfaceHost> findSurfaceHost(
    const std::shared_ptr<Clouds::SpatialNode>& node) {
    if (!node) return nullptr;
    if (auto host = std::dynamic_pointer_cast<Clouds::SpatialSurfaceHost>(node)) return host;
    for (const auto& child : node->children) {
        if (auto found = findSurfaceHost(child)) return found;
    }
    return nullptr;
}

/**
 * Find an output pixel whose raycast lands on `target`.
 *
 * Deliberately a search rather than a projection: the pixel a surface occupies
 * is a function of the camera, the window layout and the frame chrome, none of
 * which this test is about. Hard-coding a pixel would make an unrelated layout
 * change look like a seat regression.
 */
bool findPixelHitting(Server& server, const std::shared_ptr<Clouds::SpatialNode>& target,
                      double& out_x, double& out_y) {
    const struct wlr_box& box = server.compositor->outputBox();
    for (int y = kScanStep; y < box.height; y += kScanStep) {
        // Flush per row. The sweep produces an enter/leave pair every time it
        // crosses a surface; leaving thousands of them unflushed in the client's
        // output buffer is how a harness ends up with an event backlog that
        // outlives the phase that produced it.
        server.pump();
        for (int x = kScanStep; x < box.width; x += kScanStep) {
            server.compositor->processPointerMotionAbsolute(x, y);
            if (server.scene->getPointerFocus() == target) {
                out_x = x;
                out_y = y;
                return true;
            }
        }
    }
    return false;
}

// Drive the graded pointer sequence: enter, press, five-sample drag, release,
// leave. One scene-level sample per call into the compositor, which is what the
// frame-grouping rule is defined in terms of.
void drivePointerSequence(Server& server, double x, double y) {
    server.compositor->processPointerMotionAbsolute(x, y);
    server.pump();

    server.compositor->processPointerButton(kBtnLeft, true);
    server.pump();

    for (int step = 1; step <= 5; ++step) {
        server.compositor->processPointerMotionAbsolute(x + step, y + step);
        server.pump();
    }

    server.compositor->processPointerButton(kBtnLeft, false);
    server.pump();

    // Off the quad entirely: the leave edge is the QA3 F2 regression.
    server.compositor->processPointerMotionAbsolute(1.0, 1.0);
    server.pump();
}

void driveKeySequence(Server& server) {
    const uint32_t keys[] = { VazioTest::kEvdevKeyA, VazioTest::kEvdevKeyLeftShift,
                              VazioTest::kEvdevKeyEnter };
    for (uint32_t key : keys) {
        for (bool pressed : { true, false }) {
            // Both halves, exactly as the product's own key path does it: the
            // fallback keyboard carries xkb state and modifiers, the scene
            // routes the key to whatever surface holds keyboard focus.
            server.compositor->processKey(key, pressed, 0);
            server.scene->processKey(key, pressed);
            server.pump();
        }
    }
}

// Run the server's event loop until the client reports the barrier it is on.
bool expectToken(Server& server, PhaseChannel& channel, char wanted, CheckLog& log,
                 const char* phase) {
    const char token = channel.await([&] { server.pump(); }, 30000);
    if (token == wanted) return true;
    log.check(false, "script: expected '%c' from the client after the %s phase, got '%c'",
              wanted, phase, token ? token : '0');
    return false;
}

bool prepareRuntimeDir(CheckLog& log) {
    std::string error;
    if (VazioTest::prepareRuntimeDir(error)) return true;
    log.check(false, "setup: %s", error.c_str());
    return false;
}

/**
 * The server half's per-phase state. Passed by reference so each phase reads
 * what the previous one established (the surface host, above all) without any
 * of them owning the whole script.
 */
struct Script {
    Server& server;
    PhaseChannel& channel;
    CheckLog& log;
    std::shared_ptr<Clouds::SpatialSurfaceHost> host;
};

bool serverToplevelPhase(Script& s, size_t portal_children_before) {
    s.channel.send(VazioTest::Phase::kGoToplevel);
    if (!expectToken(s.server, s.channel, VazioTest::Phase::kToplevelMapped, s.log, "toplevel")) {
        return false;
    }

    auto& compositor = *s.server.compositor;
    s.log.check(compositor.windowCount() == 1, "server: one xdg_toplevel hosted (%zu)",
                compositor.windowCount());
    s.log.check(compositor.mappedWindowCount() == 1, "server: the toplevel is mapped (%zu)",
                compositor.mappedWindowCount());
    s.log.check(compositor.portalRoot()->children.size() == portal_children_before + 1,
                "server: exactly one node was inserted under the portal root");

    s.host = findSurfaceHost(compositor.portalRoot());
    s.log.check(s.host != nullptr, "server: a SpatialSurfaceHost exists in the scene for the client");
    if (!s.host) return true;

    // applySurfaceGeometry: 1.2 world units wide, height driven by the committed
    // buffer's aspect. This is the committed geometry reaching the scene, which
    // is the observable half of the import.
    const float expected_h = 1.2f / (static_cast<float>(VazioTest::kToplevelWidth) /
                                     static_cast<float>(VazioTest::kToplevelHeight));
    s.log.check(std::abs(s.host->size.x - 1.2f) < 1e-4f &&
                    std::abs(s.host->size.y - expected_h) < 1e-4f,
                "server: surface host sized from the committed buffer (%.4f x %.4f, expected %.4f x %.4f)",
                s.host->size.x, s.host->size.y, 1.2f, expected_h);
    return true;
}

bool serverPointerPhase(Script& s) {
    double hit_x = 0.0;
    double hit_y = 0.0;
    const bool found = findPixelHitting(s.server, s.host, hit_x, hit_y);
    s.log.check(found, "server: found an output pixel whose ray hits the client surface (%.0f, %.0f)",
                hit_x, hit_y);

    // Park the pointer off the surface so the graded sequence starts from a
    // known miss and the enter it produces is the enter under test.
    s.server.compositor->processPointerMotionAbsolute(1.0, 1.0);
    s.server.pump();

    s.channel.send(VazioTest::Phase::kGoPointerArm);
    if (!expectToken(s.server, s.channel, VazioTest::Phase::kPointerArmed, s.log, "pointer-arm")) {
        return false;
    }

    if (VazioTest::negativeControl("nopointer")) {
        s.log.note("NEG=nopointer: not driving the pointer; the client's seat phase must fail");
    } else if (found) {
        drivePointerSequence(s.server, hit_x, hit_y);
    }

    s.channel.send(VazioTest::Phase::kGoPointerGrade);
    if (!expectToken(s.server, s.channel, VazioTest::Phase::kPointerDone, s.log, "pointer")) {
        return false;
    }

    const glm::vec2 pointer = s.server.compositor->pointerPosition();
    s.log.check(pointer.x == 1.0f && pointer.y == 1.0f,
                "server: pointer position tracks the last driven sample (%.1f, %.1f)",
                pointer.x, pointer.y);
    return true;
}

bool serverKeyboardPhase(Script& s) {
    // Focus the surface explicitly: the press above moved scene focus onto the
    // host, and this states the precondition the phase depends on instead of
    // inheriting it.
    if (s.host) s.server.scene->setKeyboardFocus(s.host);
    driveKeySequence(s.server);
    s.channel.send(VazioTest::Phase::kGoKeyboard);
    return expectToken(s.server, s.channel, VazioTest::Phase::kKeyboardDone, s.log, "keyboard");
}

bool serverPopupPhase(Script& s) {
    s.channel.send(VazioTest::Phase::kGoPopup);
    if (!expectToken(s.server, s.channel, VazioTest::Phase::kPopupMapped, s.log, "popup")) return false;

    auto& compositor = *s.server.compositor;
    s.log.check(compositor.popupCount() == 1, "server: one xdg_popup hosted (%zu)",
                compositor.popupCount());
    s.log.check(compositor.mappedPopupCount() == 1, "server: the popup is mapped (%zu)",
                compositor.mappedPopupCount());

    // Click somewhere the popup is not. The popup took an explicit grab, so
    // xdg-shell obliges the compositor to dismiss it.
    double outside_x = 0.0;
    double outside_y = 0.0;
    const bool outside = findPixelHitting(s.server, s.host, outside_x, outside_y);
    s.log.check(outside, "server: found a pixel on the parent surface but outside the popup (%.0f, %.0f)",
                outside_x, outside_y);
    if (outside) {
        compositor.processPointerButton(kBtnLeft, true);
        s.server.pump();
        compositor.processPointerButton(kBtnLeft, false);
        s.server.pump();
    }

    s.channel.send(VazioTest::Phase::kGoPopupDismiss);
    if (!expectToken(s.server, s.channel, VazioTest::Phase::kPopupDismissed, s.log, "popup-grab")) {
        return false;
    }
    s.log.check(compositor.mappedPopupCount() == 0,
                "server: the grabbing popup is gone after the outside click (%zu mapped)",
                compositor.mappedPopupCount());
    return true;
}

// Subsurface, decoration and selection: three barriers, one shape.
bool serverChildSurfacePhases(Script& s) {
    auto& compositor = *s.server.compositor;

    s.channel.send(VazioTest::Phase::kGoSubsurface);
    if (!expectToken(s.server, s.channel, VazioTest::Phase::kSubsurfaceDone, s.log, "subsurface")) {
        return false;
    }
    s.log.check(compositor.subsurfaceCount() == 1, "server: one subsurface hosted (%zu)",
                compositor.subsurfaceCount());
    s.log.check(compositor.mappedSubsurfaceCount() == 1, "server: the subsurface is mapped (%zu)",
                compositor.mappedSubsurfaceCount());

    s.channel.send(VazioTest::Phase::kGoDecoration);
    if (!expectToken(s.server, s.channel, VazioTest::Phase::kDecorationDone, s.log, "decoration")) {
        return false;
    }
    s.log.check(compositor.windowCount() == 2,
                "server: the decorated second toplevel is hosted too (%zu)", compositor.windowCount());
    s.log.check(compositor.mappedWindowCount() == 2, "server: both toplevels are mapped (%zu)",
                compositor.mappedWindowCount());

    s.channel.send(VazioTest::Phase::kGoSelection);
    return expectToken(s.server, s.channel, VazioTest::Phase::kSelectionDone, s.log, "selection");
}

int runServer(Server& server, PhaseChannel& channel, CheckLog& log) {
    log.check(server.compositor->stage() == Clouds::SessionStage::Open,
              "server: session reached SessionStage::Open");
    log.check(server.compositor->outputCount() == 1,
              "server: exactly one output (%zu) - the two-output regression stays fixed",
              server.compositor->outputCount());

    if (!expectToken(server, channel, VazioTest::Phase::kClientBound, log, "bind")) return 1;

    Script script = { server, channel, log, nullptr };
    const size_t portal_children_before = server.compositor->portalRoot()->children.size();

    if (!serverToplevelPhase(script, portal_children_before)) return 1;
    if (!serverPointerPhase(script)) return 1;
    if (!serverKeyboardPhase(script)) return 1;
    if (!serverPopupPhase(script)) return 1;
    if (!serverChildSurfacePhases(script)) return 1;

    channel.send(VazioTest::Phase::kFinish);
    return 0;
}

}  // namespace

namespace {

// Build the session layer this harness drives: a scene with no renderer and a
// compositor with no GPU, on the headless backend, opened on its own socket.
bool buildServer(Server& server, CheckLog& log) {
    server.socket_name = "wayland-vzt-" + std::to_string(getpid());
    server.scene = std::make_shared<Clouds::SpatialScene>(nullptr, nullptr);

    const Clouds::SpatialCompositorConfig config = {
        .headless = true, .virtual_width = kOutputWidth, .virtual_height = kOutputHeight
    };
    server.compositor = std::make_unique<Clouds::SpatialCompositor>(
        nullptr, nullptr, server.scene, server.scene->root, config);

    return log.check(server.compositor->startServer(server.socket_name),
                     "server: startServer('%s') succeeded", server.socket_name.c_str());
}

void gradeClientExit(pid_t child, CheckLog& log) {
    int status = 0;
    waitpid(child, &status, 0);
    const bool exited = WIFEXITED(status);
    log.check(exited, "client: exited normally rather than on a signal");
    log.check(exited && WEXITSTATUS(status) == 0,
              "client: every client-side assertion held (exit %d)",
              exited ? WEXITSTATUS(status) : -1);
}

void tearDownServer(Server& server, CheckLog& log) {
    // Teardown, and the idempotency the lifetime harness proves at length:
    // calling it twice must be a no-op, not a double free.
    server.compositor->stop();
    server.compositor->stop();
    log.check(server.compositor->stage() == Clouds::SessionStage::Down,
              "server: stop() is idempotent and leaves the session Down");

    server.compositor.reset();
    server.scene.reset();
}

}  // namespace

int main() {
    CheckLog log("protocol_seat/server");
    log.note("negative control: %s", VazioTest::negativeControlName());

    if (!prepareRuntimeDir(log)) return log.report();
    wlr_log_init(WLR_ERROR, nullptr);

    PhasePipes pipes;
    if (!pipes.create()) {
        log.check(false, "setup: could not create the phase pipes");
        return log.report();
    }

    Server server;
    if (!buildServer(server, log)) return log.report();

    const pid_t child = fork();
    if (child < 0) {
        log.check(false, "setup: fork failed");
        return log.report();
    }

    if (child == 0) {
        PhaseChannel client_channel;
        pipes.adoptClient(client_channel);
        // The child must not run the parent's teardown: the compositor's fds and
        // wl_display are the parent's, and stopping them here would tear the
        // socket out from under the very test that is running.
        _exit(VazioTest::runProtocolClient(server.socket_name.c_str(), client_channel));
    }

    PhaseChannel server_channel;
    pipes.adoptServer(server_channel);
    const int script_result = runServer(server, server_channel, log);
    server_channel.close();

    gradeClientExit(child, log);
    tearDownServer(server, log);

    const int failures = log.report();
    return (failures == 0 && script_result == 0) ? 0 : 1;
}
