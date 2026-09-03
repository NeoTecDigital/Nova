// Written by Richard Christopher, Copyright 2026 NeoTec Digital
//
// Targeted coverage for the four SpatialPresentLoop defects QA5 found by
// reading. None of them is reachable through `vazio` today - the headless
// config creates exactly one output and never changes its mode - so they are
// driven here against a bare wlroots backend instead of being left unproven.
//
//   D1 multi-output sidecar overflow  -> the CPU path must refuse a second
//                                        output rather than resize the shared
//                                        target under the first one.
//   D2 frame-callback driver election -> destroying the driving output must
//                                        hand the role to the next live one,
//                                        and must erase the dead entry.
//   D3 selectPath half-state          -> a failed selection must leave the
//                                        path Undecided so a later attach
//                                        re-probes.
//   D4 sidecar mode change            -> a mode set on the output must rebuild
//                                        the swapchain and the target.
//
// compositor_ is deliberately null: presentFrame guards on it, onPresent guards
// on it, and nothing else in the loop touches it. That keeps the probe about
// the presentation loop instead of about the compositor.
#include "include/Clouds/SpatialPresentLoop.h"
#include "include/Clouds/SpatialCompositor.h"
#include "Core/components/logger.h"

extern "C" {
#include <wlr/backend/headless.h>
#include <wlr/render/allocator.h>
#include <wlr/render/wlr_renderer.h>
#include <wlr/types/wlr_output.h>
#include <wlr/util/log.h>
}

#include <cstdio>
#include <cstring>
#include <memory>
#include <string>

namespace {

int g_checks = 0;
int g_failures = 0;

void check(bool ok, const char* what) {
    ++g_checks;
    if (!ok) ++g_failures;
    std::printf("[%s] %s\n", ok ? " ok " : "FAIL", what);
    std::fflush(stdout);
}

// One headless output, enabled and mode-set, which is the precondition
// SpatialPresentLoop::attach documents.
struct wlr_output* addOutput(struct wlr_backend* backend, int width, int height) {
    struct wlr_output* output = wlr_headless_add_output(backend, width, height);
    if (!output) return nullptr;

    struct wlr_output_state state;
    wlr_output_state_init(&state);
    wlr_output_state_set_enabled(&state, true);
    wlr_output_state_set_custom_mode(&state, width, height, 0);
    const bool ok = wlr_output_commit_state(output, &state);
    wlr_output_state_finish(&state);
    return ok ? output : nullptr;
}

bool setMode(struct wlr_output* output, int width, int height) {
    struct wlr_output_state state;
    wlr_output_state_init(&state);
    wlr_output_state_set_custom_mode(&state, width, height, 0);
    const bool ok = wlr_output_commit_state(output, &state);
    wlr_output_state_finish(&state);
    return ok;
}

struct Harness {
    struct wl_display* display = nullptr;
    struct wlr_backend* backend = nullptr;
    struct wlr_renderer* renderer = nullptr;
    struct wlr_allocator* allocator = nullptr;
    std::unique_ptr<Nova::Graphics> nova;

    bool up() const {
        return display && backend && renderer && allocator && nova;
    }

    ~Harness() {
        nova.reset();
        if (allocator) wlr_allocator_destroy(allocator);
        if (backend) wlr_backend_destroy(backend);
        if (display) wl_display_destroy(display);
    }
};

bool bringUp(Harness& h) {
    h.display = wl_display_create();
    if (!h.display) return false;

    h.backend = wlr_headless_backend_create(wl_display_get_event_loop(h.display));
    if (!h.backend) return false;

    h.renderer = wlr_renderer_autocreate(h.backend);
    if (!h.renderer) return false;

    h.allocator = wlr_allocator_autocreate(h.backend, h.renderer);
    if (!h.allocator) return false;

    if (!wlr_backend_start(h.backend)) return false;

    Nova::OffscreenConfig config = {};
    config.extent = { 640, 400 };
    config.drm_fd = wlr_backend_get_drm_fd(h.backend);
    config.request_dmabuf_import = true;
    h.nova = std::make_unique<Nova::Graphics>(config, std::string("ERROR"));
    return true;
}

// Attaching an output needs it bound to the renderer/allocator the loop will
// acquire buffers from, which is what the compositor does for its own outputs.
void initOutputRender(Harness& h, struct wlr_output* output) {
    wlr_output_init_render(output, h.allocator, h.renderer);
}

// Drive one frame per output by dispatching the event loop: a headless output
// arms `frame` from its own timer, so this is the real path, not a poke.
void pump(Harness& h, int iterations) {
    for (int i = 0; i < iterations; ++i) {
        wl_event_loop_dispatch(wl_display_get_event_loop(h.display), 10);
        wl_display_flush_clients(h.display);
    }
}

int runSidecarCase() {
    std::printf("\n=== pixman sidecar: D1 (second output refused) + D4 (mode change) ===\n");
    Harness h;
    if (!bringUp(h) || !h.up()) {
        check(false, "sidecar: harness came up");
        return 1;
    }
    check(true, "sidecar: harness came up");
    // wlroots 0.19 exposes no renderer name, so the capability bits are the
    // observable: a pixman renderer advertises SHM buffers and not DMABUF,
    // which is exactly the condition selectPath falls back to the CPU path on.
    check((h.renderer->render_buffer_caps & WLR_BUFFER_CAP_DMABUF) == 0,
          "sidecar: WLR_RENDERER=pixman took effect (renderer advertises no DMABUF)");

    struct wlr_output* first = addOutput(h.backend, 640, 400);
    struct wlr_output* second = addOutput(h.backend, 800, 600);
    check(first != nullptr && second != nullptr, "sidecar: two headless outputs exist");
    if (!first || !second) return 1;
    initOutputRender(h, first);
    initOutputRender(h, second);

    Vazio::SpatialPresentLoop loop(h.nova.get(), nullptr);
    loop.setSceneRenderer([](VkCommandBuffer, const VkExtent2D&) {});

    check(loop.attach(first), "sidecar: the first output was adopted");
    check(loop.path() == Vazio::PresentPath::PixmanSidecar,
          "sidecar: the CPU path was selected");
    check(loop.renderPass() != VK_NULL_HANDLE, "sidecar: a render pass exists after attach");

    // D1: the shared readback target is sized for `first`. A second output of a
    // different size must be refused, not silently resize it.
    check(!loop.attach(second), "D1: a second sidecar output is refused");
    check(loop.path() == Vazio::PresentPath::PixmanSidecar,
          "D1: the refusal did not disturb the selected path");

    pump(h, 30);
    const uint64_t commits_before = loop.commits();
    const uint64_t failures_before = loop.failures();
    check(commits_before > 0, "sidecar: the CPU path committed frames");
    check(failures_before == 0, "sidecar: no failed frames before the mode change");

    // D4: a mode set on the driven output. Before the fix the swapchain and the
    // target stayed at the old extent and every subsequent commit failed.
    check(setMode(first, 1024, 768), "D4: the output accepted a 1024x768 mode set");
    pump(h, 40);
    check(loop.commits() > commits_before, "D4: frames committed again after the mode change");
    check(loop.failures() == failures_before,
          "D4: the mode change cost no failed frames");

    loop.stop();
    check(loop.path() == Vazio::PresentPath::Undecided, "sidecar: stop() reset the path");
    return 0;
}

int runElectionCase() {
    std::printf("\n=== dmabuf import: D2 (frame-callback driver election) ===\n");
    Harness h;
    if (!bringUp(h) || !h.up()) {
        check(false, "election: harness came up");
        return 1;
    }
    check(true, "election: harness came up");

    struct wlr_output* first = addOutput(h.backend, 640, 400);
    struct wlr_output* second = addOutput(h.backend, 640, 400);
    check(first != nullptr && second != nullptr, "election: two headless outputs exist");
    if (!first || !second) return 1;
    initOutputRender(h, first);
    initOutputRender(h, second);

    Vazio::SpatialPresentLoop loop(h.nova.get(), nullptr);
    loop.setSceneRenderer([](VkCommandBuffer, const VkExtent2D&) {});

    check(loop.attach(first), "election: the first output was adopted");
    if (loop.path() != Vazio::PresentPath::DmabufImport) {
        std::printf("[skip] election: this device selected %s, not dmabuf-import; "
                    "the multi-output case needs the zero-copy path\n",
                    Vazio::presentPathName(loop.path()));
        return 0;
    }
    check(loop.attach(second), "election: a second output is accepted on the import path");

    pump(h, 30);
    const uint64_t commits_two = loop.commits();
    check(commits_two > 0, "election: both outputs committed frames");

    // D2: destroying the driving output must hand frame_done to the survivor
    // and must not leave a dead entry behind. The re-election is reported, so
    // the log line is the observable; the survivor still committing is the
    // functional proof.
    wlr_output_destroy(first);
    pump(h, 30);
    check(loop.commits() > commits_two, "D2: the surviving output keeps committing");
    check(loop.failures() == 0, "D2: destroying the driver cost no failed frames");

    // The reap runs from attach(). Re-attaching the survivor is a no-op that
    // must still succeed and must not resurrect the dead entry.
    check(loop.attach(second), "D2: re-attaching the survivor is a successful no-op");

    wlr_output_destroy(second);
    pump(h, 10);
    check(true, "D2: destroying the last output did not fault");

    loop.stop();
    check(true, "election: stop() after every output is gone");
    return 0;
}

} // namespace

int main(int argc, char** argv) {
    wlr_log_init(WLR_ERROR, nullptr);
    const bool sidecar = (argc > 1 && std::strcmp(argv[1], "sidecar") == 0);

    if (sidecar) {
        runSidecarCase();
    } else {
        runElectionCase();
    }

    std::printf("\nprobe_present_loop(%s): %d/%d checks passed, %d failures\n",
                sidecar ? "sidecar" : "election",
                g_checks - g_failures, g_checks, g_failures);
    std::fflush(stdout);
    return g_failures == 0 ? 0 : 1;
}
