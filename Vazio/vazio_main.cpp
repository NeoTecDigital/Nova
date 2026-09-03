// Written by Richard Christopher, Copyright 2026 NeoTec Digital
//
// The staged boot entry (plan section B): one process, one DRM master
// acquisition, splash morphing into session in-engine.
//
//   Splash  - offscreen Graphics (no SDL anywhere in this binary), texture
//             bridge, scene, and a compositor substrate held LATENT: outputs
//             live and committing frames, zero sockets, zero protocol globals.
//   Session - a readiness token arrives on the channel Core provided, the SAME
//             compositor opens its socket, and the boot Desktop is swapped for
//             the session Desktop.
//
// examples/clouds_server_main.cpp remains the nested development entry and is
// untouched: it wants SDL, ImGui and a socket from the first instant, which is
// exactly what this binary must not have.
#include "./SpatialCompositor.h"
#include "./SpatialPresentLoop.h"
#include "Splash/SpatialScene.h"
#include "Clouds/apps/SpatialFilesystem.h"
#include "Splash/oats/OatsBridge.h"
#include "Splash/Primitives.h"
#include "Nova/nova_graphics.h"
#include "Nova/components/logger.h"

#include <fcntl.h>
#include <signal.h>
#include <unistd.h>

#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <memory>
#include <string>
#include <vector>

namespace {

// Exit codes, plan B.4. Core's rc script acts on these, so they are a contract:
// a crash-looping splash must be distinguishable from one that asked to be
// replaced. Only OK and FATAL are ever produced today - the two policies that
// would produce the others are the user's to set, not this binary's to invent.
constexpr int kExitOk = 0;
constexpr int kExitFatal = 1;
constexpr int kExitCleanFallback = 2;     // reserved: "hand the console back"
constexpr int kExitReadinessTimeout = 3;  // reserved: plan B.2 option (iii)
static_assert(kExitOk != kExitFatal && kExitCleanFallback != kExitFatal &&
              kExitReadinessTimeout != kExitCleanFallback,
              "plan B.4 requires four distinct exit codes");

// Bounded dispatch, so an idle splash costs no core. Frame events arrive from
// the output's own timer inside this same loop, so the cadence is the output's;
// this only bounds how long a readiness poll can be late by.
constexpr int kActiveDispatchMs = 8;
constexpr int kInactiveDispatchMs = 50;

volatile sig_atomic_t g_terminate = 0;
void onTerminate(int) { g_terminate = 1; }

struct CommandLine {
    // Readiness channel, plan B.2. Exactly one is used; neither is required.
    int ready_fd = -1;
    std::string ready_path;

    // Borrowed DRM node, used ONLY as Nova's device-selection tie-break (plan
    // D.3). The backend's own fd would be the better source but is not knowable
    // before Nova exists, so the provisioner names it - which is the same place
    // that already knows which node this session was granted.
    std::string drm_node;

    std::string session_root;
    bool headless = false;

    // CI hook: stop after this many committed frames. Zero means run until a
    // signal. Not a timeout policy - it reports nothing and decides nothing.
    uint64_t max_frames = 0;
};

void reportUsage() {
    report(LOGGER::INFO, "vazio [--ready-fd N | --ready-path P] [--drm-node PATH] "
                         "[--session-root DIR] [--headless] [--max-frames N]");
}

CommandLine parseCommandLine(int argc, char** argv) {
    CommandLine cli;
    for (int i = 1; i < argc; ++i) {
        const std::string arg = argv[i];
        const bool has_value = (i + 1) < argc;
        if (arg == "--ready-fd" && has_value)          cli.ready_fd = std::atoi(argv[++i]);
        else if (arg == "--ready-path" && has_value)   cli.ready_path = argv[++i];
        else if (arg == "--drm-node" && has_value)     cli.drm_node = argv[++i];
        else if (arg == "--session-root" && has_value) cli.session_root = argv[++i];
        else if (arg == "--max-frames" && has_value)   cli.max_frames = std::strtoull(argv[++i], nullptr, 10);
        else if (arg == "--headless")                  cli.headless = true;
        else {
            report(LOGGER::ERROR, "vazio - Unrecognised argument '%s'", arg.c_str());
            reportUsage();
        }
    }
    if (cli.session_root.empty()) {
        std::error_code ec;
        const std::filesystem::path cwd = std::filesystem::current_path(ec);
        cli.session_root = ec ? "." : cwd.string();
    }
    return cli;
}

/**
 * The readiness contract, Vazio's half only (plan B.2).
 *
 * One readable token means ready. What the token is, who writes it and whether
 * a timeout exists are all Core's rc-script domain; this only reports whether
 * one has arrived.
 */
class ReadinessChannel {
public:
    ReadinessChannel(int fd, std::string path) : fd_(fd), path_(std::move(path)) {
        if (fd_ >= 0) {
            const int flags = fcntl(fd_, F_GETFL, 0);
            if (flags >= 0) fcntl(fd_, F_SETFL, flags | O_NONBLOCK);
        }
    }

    bool provided() const { return fd_ >= 0 || !path_.empty(); }

    const char* describe() const {
        if (fd_ >= 0) return "inherited fd";
        if (!path_.empty()) return "filesystem path";
        return "none provided";
    }

    // True exactly once, on the token that arrives first.
    bool signalled() {
        if (fired_) return false;
        if (fd_ >= 0) {
            char token = 0;
            if (read(fd_, &token, 1) == 1) fired_ = true;
        }
        if (!fired_ && !path_.empty()) {
            std::error_code ec;
            if (std::filesystem::exists(path_, ec) && !ec) fired_ = true;
        }
        return fired_;
    }

private:
    int fd_ = -1;
    std::string path_;
    bool fired_ = false;
};

/**
 * The boot Desktop: an ordinary Desktop with local nodes under it (plan S.6).
 *
 * Honest by construction. There is no measured total to report progress
 * against, so it reports none: a title, the elapsed time, and the one stage
 * fact this process actually knows.
 */
struct BootDesktop {
    std::shared_ptr<Splash::SpatialNode> root;
    std::shared_ptr<Splash::SpatialLabel> status;
    std::vector<std::shared_ptr<Splash::SpatialPanel>> orbiters;
    float elapsed = 0.0f;

    void build(const std::shared_ptr<Splash::SpatialFont>& font, const char* channel) {
        root = std::make_shared<Splash::SpatialNode>();
        root->name = "BootDesktop";

        auto title = std::make_shared<Splash::SpatialLabel>("VAZIO", font, 0.006f);
        title->transform.position = glm::vec3(0.0f, 0.55f, 0.0f);
        root->addChild(title);

        status = std::make_shared<Splash::SpatialLabel>("", font, 0.0022f);
        status->transform.position = glm::vec3(0.0f, -0.55f, 0.0f);
        root->addChild(status);

        auto channel_line = std::make_shared<Splash::SpatialLabel>(
            std::string("readiness channel: ") + channel, font, 0.0016f,
            glm::vec4(0.55f, 0.62f, 0.78f, 1.0f));
        channel_line->transform.position = glm::vec3(0.0f, -0.68f, 0.0f);
        root->addChild(channel_line);

        for (int i = 0; i < 5; ++i) {
            auto orbiter = std::make_shared<Splash::SpatialPanel>(
                glm::vec2(0.14f, 0.14f), glm::vec4(0.16f, 0.34f, 0.62f, 0.9f));
            orbiter->name = "BootOrbiter";
            orbiter->interactable = false;
            // Each orbiter starts at a different point on the phase circle, so
            // evolvePhase moves them independently rather than in lockstep.
            orbiter->phase_state = Nova::Math::PhaseState8::fromComplexPhase(
                std::cos(static_cast<float>(i) * 1.2566371f),
                std::sin(static_cast<float>(i) * 1.2566371f),
                static_cast<float>(i) * 0.7f);
            root->addChild(orbiter);
            orbiters.push_back(orbiter);
        }
        root->relinkChildren();
    }

    void update(float dt) {
        elapsed += dt;
        const int index_count = static_cast<int>(orbiters.size());
        for (int i = 0; i < index_count; ++i) {
            const float phase = elapsed * 1.1f + static_cast<float>(i) * 6.2831853f / index_count;
            orbiters[i]->transform.position =
                glm::vec3(std::cos(phase) * 0.55f, std::sin(phase) * 0.28f, std::sin(phase * 0.5f) * 0.2f);
            orbiters[i]->evolvePhase(dt, 1.0f);
        }
        char line[96];
        std::snprintf(line, sizeof(line), "waiting for readiness signal - %.1fs elapsed", elapsed);
        status->setText(line);
    }
};

/**
 * Resolve a build-tree-relative asset, preferring the executable's directory.
 *
 * Plan B.3.7: shader paths default to CWD-relative, and early userspace has no
 * useful working directory. The executable's own directory is the one location
 * this process can always name, so it is tried first and the CWD-relative form
 * is kept as the development fallback.
 */
std::string resolveAssetPath(const std::string& relative) {
    char exe[4096] = {0};
    const ssize_t length = readlink("/proc/self/exe", exe, sizeof(exe) - 1);
    if (length > 0) {
        const std::filesystem::path candidate =
            std::filesystem::path(exe).parent_path() / relative;
        std::error_code ec;
        if (std::filesystem::exists(candidate, ec) && !ec) return candidate.string();
    }
    return relative;
}

// Nova's device-selection hint. Borrowed and never used for anything else: it
// is not ioctl'd, not read, and closed by the caller at exit.
int openDrmHint(const std::string& node) {
    if (node.empty()) {
        report(LOGGER::INFO, "vazio - No --drm-node hint; device selection falls back to type score");
        return -1;
    }
    const int fd = open(node.c_str(), O_RDWR | O_CLOEXEC);
    if (fd < 0) {
        report(LOGGER::ERROR, "vazio - Cannot open DRM node '%s' for the selection hint", node.c_str());
    }
    return fd;
}

} // namespace

// --- Composition -------------------------------------------------------------

namespace {

struct Runtime {
    std::unique_ptr<Nova::Graphics> graphics;
    std::unique_ptr<Nova::TextureBridge> bridge;
    std::unique_ptr<Nova::SpatialPipeline> pipeline;
    std::shared_ptr<Splash::SpatialScene> scene;
    std::unique_ptr<Vazio::SpatialCompositor> compositor;
    std::unique_ptr<Vazio::SpatialPresentLoop> present;

    // Session stage only; absent for the whole of the splash.
    std::shared_ptr<Splash::OatsBridge> oats;
    std::shared_ptr<Clouds::SpatialFilesystem> filesystem;
    std::shared_ptr<Splash::SpatialNode> session_root;

    BootDesktop boot;
};

bool buildGraphics(Runtime& rt, const CommandLine& cli, int drm_fd) {
    Nova::OffscreenConfig config;
    config.extent = { cli.headless ? 1600u : 1920u, cli.headless ? 1000u : 1080u };
    config.drm_fd = drm_fd;
    config.request_dmabuf_import = true;

    rt.graphics = std::make_unique<Nova::Graphics>(config, "INFO");
    rt.bridge = std::make_unique<Nova::TextureBridge>(rt.graphics.get());
    rt.bridge->initialize();

    // No font file: SpatialFont falls back to its built-in atlas, which is what
    // makes the splash independent of anything on disk (plan B.3.7).
    rt.scene = std::make_shared<Splash::SpatialScene>(rt.graphics.get(), rt.bridge.get());
    rt.scene->initialize("");
    rt.scene->show_lookat_reticle = false;
    rt.scene->show_cursor_reticle = false;
    return true;
}

// Bind the presentation loop to the outputs the substrate brought up, and build
// the scene pipeline against the render pass the chosen path settled on.
bool bindPresentation(Runtime& rt) {
    rt.present = std::make_unique<Vazio::SpatialPresentLoop>(rt.graphics.get(), rt.compositor.get());
    rt.present->setSceneRenderer([&rt](VkCommandBuffer cmd, const VkExtent2D& extent) {
        if (!rt.pipeline) return;
        rt.scene->render(rt.pipeline.get(), cmd,
                         glm::vec2(static_cast<float>(extent.width), static_cast<float>(extent.height)));
    });

    // Captures only the runtime, which outlives the compositor holding this
    // handler. A hotplugged output lands here too, and finds the pipeline built.
    rt.compositor->setOutputReadyHandler([&rt](struct wlr_output* output) {
        if (!rt.present->attach(output) || rt.pipeline) return;
        rt.pipeline = std::make_unique<Nova::SpatialPipeline>(
            rt.graphics.get(), rt.present->renderPass(), rt.bridge.get());
        rt.pipeline->build(resolveAssetPath("shaders/spatial/spatial_ui_vert.spv"),
                           resolveAssetPath("shaders/spatial/spatial_ui_frag.spv"));
    });

    if (!rt.pipeline) {
        report(LOGGER::ERROR, "vazio - No output could be driven; nothing to present through");
        return false;
    }
    return true;
}

/**
 * The splash -> session morph (plan S.6): swap the presented Desktop.
 *
 * Mechanism, not policy. Vazio supports both swapping Desktops and rebinding
 * Portals into the existing one; which of the two a boot uses, and what it
 * looks like in between, is theme/Precipitation's decision and is deliberately
 * not made here.
 */
void enterSessionStage(Runtime& rt, const CommandLine& cli) {
    rt.scene->root->removeChild(rt.boot.root);
    rt.boot.root.reset();
    rt.boot.status.reset();
    rt.boot.orbiters.clear();

    rt.session_root = std::make_shared<Splash::SpatialNode>();
    rt.session_root->name = "SessionDesktop";
    rt.scene->root->addChild(rt.session_root);

    // OATS degrades, never aborts (plan B.3.2): a runtime that will not start
    // costs this session its presenters, not its display.
    rt.oats = std::make_shared<Splash::OatsBridge>();
    if (!rt.oats->initialize()) {
        report(LOGGER::ERROR, "vazio - OATS runtime unavailable; continuing without its presenters");
        rt.oats.reset();
    }

    rt.filesystem = std::make_shared<Clouds::SpatialFilesystem>(rt.session_root, rt.scene->font);
    rt.filesystem->scanAndBuild3DTree(cli.session_root, 2);
    report(LOGGER::INFO, "vazio - Session Desktop built from '%s' (%zu nodes)",
           cli.session_root.c_str(), rt.filesystem->getNodeCount());
}

void stepSession(Runtime& rt, float dt) {
    if (rt.oats) rt.oats->step(dt);
    if (rt.filesystem) rt.filesystem->update(dt);
}

// The splash stage, top to bottom: graphics, the latent substrate, the
// presentation loop bound to its outputs, and the boot Desktop on screen.
bool enterSplashStage(Runtime& rt, const CommandLine& cli, const ReadinessChannel& readiness,
                      int drm_fd) {
    if (!buildGraphics(rt, cli, drm_fd)) return false;

    const Vazio::SpatialCompositorConfig compositor_config = {
        .headless = cli.headless,
        .virtual_width = rt.graphics->getWindowExtent().width,
        .virtual_height = rt.graphics->getWindowExtent().height
    };
    rt.compositor = std::make_unique<Vazio::SpatialCompositor>(
        rt.graphics.get(), rt.bridge.get(), rt.scene, rt.scene->root, compositor_config);

    if (!rt.compositor->startSubstrate()) {
        report(LOGGER::ERROR, "vazio - Substrate failed; nothing to present through");
        return false;
    }
    if (!bindPresentation(rt)) return false;

    rt.boot.build(rt.scene->font, readiness.describe());
    rt.scene->root->addChild(rt.boot.root);
    report(LOGGER::INFO, "vazio - Splash stage on %zu output(s) over %s; readiness channel: %s",
           rt.compositor->outputCount(), Vazio::presentPathName(rt.present->path()),
           readiness.describe());

    // No readiness source is plan B.2 option (i) by default: stay on the splash
    // with honest status forever. Options (ii) timeout-open and (iii)
    // timeout-exit are policies, and their existence and value are the user's
    // decision - this binary reserves the exit codes and implements neither.
    if (!readiness.provided()) {
        report(LOGGER::INFO, "vazio - No readiness channel; the splash is the whole of this run");
    }
    return true;
}

// The one-way flip, latent -> open. True means "nothing to do yet, or done";
// false means the flip was attempted on a live substrate and refused, which is
// fatal - there is no second substrate to fall back to.
bool openSessionIfReady(Runtime& rt, const CommandLine& cli, ReadinessChannel& readiness) {
    if (rt.compositor->stage() != Vazio::SessionStage::Latent) return true;
    if (!readiness.signalled()) return true;

    report(LOGGER::INFO, "vazio - Readiness token received after %.2fs; opening the session",
           rt.boot.elapsed);
    if (!rt.compositor->open("wayland-vazio-0")) {
        report(LOGGER::ERROR, "vazio - Session refused to open on a live substrate");
        return false;
    }
    enterSessionStage(rt, cli);
    return true;
}

/**
 * The whole run: splash until the token arrives, session after it.
 *
 * Rendering is NOT driven from here. Frames are committed from the output's own
 * `frame` event inside iterateEventLoop, so this loop's job is to advance the
 * scene the next frame will draw and to watch the readiness channel.
 */
int runStagedLoop(Runtime& rt, const CommandLine& cli, ReadinessChannel& readiness) {
    auto last = std::chrono::steady_clock::now();
    while (!g_terminate) {
        const auto now = std::chrono::steady_clock::now();
        const float dt = std::chrono::duration<float>(now - last).count();
        last = now;

        const bool active = rt.compositor->isSessionActive();
        rt.compositor->iterateEventLoop(active ? kActiveDispatchMs : kInactiveDispatchMs);

        // A VT switch away took the DRM master with it. Dispatch continues so
        // the switch back is seen; nothing is drawn and nothing is stepped.
        if (!active) continue;

        if (rt.boot.root) rt.boot.update(dt);
        rt.scene->update(dt);
        stepSession(rt, dt);

        if (!openSessionIfReady(rt, cli, readiness)) return kExitFatal;
        if (cli.max_frames > 0 && rt.present->commits() >= cli.max_frames) break;
    }
    return kExitOk;
}

} // namespace

int main(int argc, char** argv) {
    // Process-global sink: wlroots writes through one static logger, so this is
    // the entry point's call to make, not any individual session's.
    wlr_log_init(WLR_INFO, nullptr);
    signal(SIGTERM, onTerminate);
    signal(SIGINT, onTerminate);

    const CommandLine cli = parseCommandLine(argc, argv);
    ReadinessChannel readiness(cli.ready_fd, cli.ready_path);
    const int drm_fd = openDrmHint(cli.drm_node);

    Runtime rt;
    if (!enterSplashStage(rt, cli, readiness, drm_fd)) return kExitFatal;

    const int loop_result = runStagedLoop(rt, cli, readiness);

    report(LOGGER::INFO, "vazio - Shutting down: %llu frames committed, %llu failed, stage %s",
           static_cast<unsigned long long>(rt.present->commits()),
           static_cast<unsigned long long>(rt.present->failures()),
           rt.compositor->stage() == Vazio::SessionStage::Open ? "open" : "latent");

    // Order is load bearing: the loop's listeners live on wlr_output signals,
    // so they come off before the compositor destroys the display that owns
    // them, and the scene's Vulkan objects go last of all.
    rt.present->stop();
    rt.compositor->stop();
    if (drm_fd >= 0) close(drm_fd);
    return loop_result;
}
