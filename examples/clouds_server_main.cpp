// Written by Richard Christopher, Copyright 2026 NeoTec Digital
#include "Nova.h"
#include "include/Clouds/SpatialScene.h"
#include "include/Clouds/SpatialCompositor.h"
#include "include/Clouds/OatsBridge.h"
#include "include/Clouds/SpatialFilesystem.h"
#include "include/Clouds/ImGuiEngineOverlay.h"
#include "Core/components/logger.h"
#include "./sdl_evdev_scancodes.h"

#include <SDL2/SDL.h>
#include <SDL2/SDL_vulkan.h>
#include <linux/input-event-codes.h>
#include <imgui.h>
#include <iostream>
#include <memory>
#include <chrono>
#include <ctime>
#include <filesystem>
#include <string>

namespace {

// How long the main loop blocks in the display's own poll while this session
// is not the active VT. Bounded rather than infinite so the worst-case resume
// latency is a number rather than a hope: a VT switch back arrives as a libseat
// event on the session fd, which lives in this same event loop, so the normal
// case wakes immediately and this only bounds the pathological one.
constexpr int kInactiveDispatchMs = 50;

/**
 * Dispatch the display once, and say whether this session still owns the seat.
 *
 * The timeout is the whole of the VT-away fix. iterateEventLoop(0) polls with a
 * zero timeout and returns at once, which is a busy wait at a full core for as
 * long as the user is on another VT. A bounded timeout blocks inside the event
 * loop's own epoll_wait - the fd wl_event_loop_get_fd() names,
 * wayland-server-core.h:176 - and returns the moment anything lands on it: a
 * client, a device, or the session coming back. Measured on this compositor:
 * 0.997 cores at timeout 0, 0.0001 cores at timeout 50, and a dispatch that
 * would have waited a second returns in 301ms when a client connects at 300ms.
 */
bool dispatchDisplay(Vazio::SpatialCompositor& compositor) {
    const bool active = compositor.isSessionActive();
    compositor.iterateEventLoop(active ? 0 : kInactiveDispatchMs);
    return active;
}

struct CommandLine {
    // Scan root is the first non-flag argument; defaults to the working directory.
    std::string scan_root;
    // --headless forces the wlroots headless backend. Without it the backend is
    // autocreated, which is DRM + libinput on a TTY and the wayland or x11
    // backend when nested. Only affects the display server: Nova still opens its
    // interim SDL window either way.
    bool headless = false;
};

std::string canonicalize(const std::filesystem::path& root) {
    std::error_code ec;
    std::filesystem::path canonical = std::filesystem::weakly_canonical(root, ec);
    return ec ? root.string() : canonical.string();
}

CommandLine parseCommandLine(int argc, char** argv) {
    CommandLine cli;
    std::filesystem::path root;
    bool have_root = false;

    for (int i = 1; i < argc; ++i) {
        const std::string arg = argv[i];
        if (arg == "--headless") {
            cli.headless = true;
        } else if (!have_root) {
            root = arg;
            have_root = true;
        } else {
            report(LOGGER::ERROR, "Ignoring unexpected argument '%s'", arg.c_str());
        }
    }

    if (!have_root) {
        std::error_code ec;
        root = std::filesystem::current_path(ec);
        if (ec) {
            report(LOGGER::ERROR, "Unable to resolve current working directory: %s", ec.message().c_str());
            cli.scan_root = ".";
            return cli;
        }
    }
    cli.scan_root = canonicalize(root);
    return cli;
}

/**
 * The one keycode translation in the process, at the one boundary that needs
 * it. SDL is the only producer that does not already speak evdev; both
 * consumers below - the compositor's fallback keyboard, which derives modifier
 * state, and the scene route, which reaches the focused client surface - are
 * handed the same evdev code and neither knows SDL was involved.
 *
 * Unmapped keys are dropped, not guessed: a wrong keycode types the wrong
 * character into a client, which is worse than typing nothing.
 */
void forwardKeyToCompositor(Vazio::SpatialCompositor& compositor,
                            Splash::SpatialScene& scene,
                            const SDL_KeyboardEvent& key,
                            bool pressed) {
    const uint32_t evdev = CloudsInterim::sdlScancodeToEvdev(key.keysym.scancode);
    if (evdev == 0) {
        report(LOGGER::DEBUG, "Dropping unmapped SDL scancode %u (no evdev equivalent)",
               static_cast<unsigned>(key.keysym.scancode));
        return;
    }
    compositor.processKey(evdev, pressed, key.timestamp);
    scene.processKey(evdev, pressed);
}

} // namespace

int main(int argc, char** argv) {
    // Process-global sink: wlroots writes through one static logger, so this is
    // the entry point's call to make, not any individual session's.
    wlr_log_init(WLR_INFO, nullptr);

    const CommandLine cli = parseCommandLine(argc, argv);

    report(LOGGER::INFO, "=========================================================");
    report(LOGGER::INFO, "  CLOUDS 3D - Quaternionic Spatial Engine & Filesystem   ");
    report(LOGGER::INFO, "  Dear ImGui Engine Controller & 3D Pill Tree Explorer   ");
    report(LOGGER::INFO, "=========================================================");

    Nova::Config config = {
        .name = "Clouds 3D Spatial Engine",
        .screen = { 1600, 1000 },
        .debug_level = "INFO",
        .dimensions = "3D",
        .camera_type = "orbit",
        .compute = false
    };

    // Initialize Nova Vulkan Graphics Engine
    auto nova = std::make_unique<Nova::App>(config);
    if (!nova->initialized) {
        report(LOGGER::ERROR, "Failed to initialize Nova Vulkan Engine");
        return 1;
    }

    Nova::Graphics* graphics = nova->getGraphics();
    if (!graphics) {
        report(LOGGER::ERROR, "Graphics context unavailable");
        return 1;
    }

    SDL_Window* sdl_window = nova->getWindow();

    // Initialize Nova Spatial Texture & Builder Bridge
    auto texture_bridge = std::make_unique<Nova::TextureBridge>(graphics);
    texture_bridge->initialize();

    // Construct 3D Quaternionic Vulkan Graphics Builder
    auto pipeline = std::make_unique<Nova::SpatialPipeline>(graphics, graphics->getRenderPass(), texture_bridge.get());
    pipeline->build("shaders/spatial/spatial_ui_vert.spv", "shaders/spatial/spatial_ui_frag.spv");

    // Initialize 3D Spatial Scene
    auto scene = std::make_shared<Splash::SpatialScene>(graphics, texture_bridge.get());
    scene->initialize();

    // Initialize Rust OATS-rs FFI Delta Bridge
    auto oats_bridge = std::make_shared<Splash::OatsBridge>();
    if (!oats_bridge->initialize()) {
        report(LOGGER::ERROR, "Failed to initialize OATS-rs FFI bridge - runtime unavailable");
        return 1;
    }

    // Initialize 3D Filesystem Explorer with Parametric 3D Pills
    report(LOGGER::INFO, "Filesystem scan root: %s", cli.scan_root.c_str());
    auto filesystem_3d = std::make_shared<Clouds::SpatialFilesystem>(scene->root, scene->font);
    filesystem_3d->scanAndBuild3DTree(cli.scan_root, 2);

    // Initialize Dear ImGui Engine Overlay
    auto imgui_overlay = std::make_unique<Clouds::ImGuiEngineOverlay>(
        graphics,
        sdl_window,
        &scene->physics_config,
        oats_bridge,
        filesystem_3d
    );
    imgui_overlay->initialize();

    // Initialize wlroots Wayland Display Server. The scene root doubles as the
    // portal root while this session hosts exactly one Desktop; a real Portal
    // node substitutes here without the compositor knowing the difference.
    const Vazio::SpatialCompositorConfig compositor_config = {
        .headless = cli.headless,
        .virtual_width = static_cast<uint32_t>(config.screen.width),
        .virtual_height = static_cast<uint32_t>(config.screen.height)
    };
    auto compositor = std::make_unique<Vazio::SpatialCompositor>(
        graphics, texture_bridge.get(), scene, scene->root, compositor_config);
    if (!compositor->startServer("wayland-clouds-0")) {
        report(LOGGER::ERROR, "Failed to start the Wayland display server - no socket to host clients on");
        return 1;
    }

    report(LOGGER::INFO, "Clouds Display Server & 3D Filesystem Engine Running.");

    bool quit = false;
    SDL_Event e;
    auto last_time = std::chrono::high_resolution_clock::now();

    // Camera Navigation State
    bool is_left_orbiting = false;
    bool is_panning = false;
    int last_mouse_x = 0;
    int last_mouse_y = 0;
    int left_start_x = 0;
    int left_start_y = 0;
    float camera_distance = 3.2f;
    float camera_yaw = 0.0f;
    float camera_pitch = 0.2f;

    auto updateCamera = [&]() {
        float cx = camera_distance * std::sin(camera_yaw) * std::cos(camera_pitch);
        float cy = camera_distance * std::sin(camera_pitch);
        float cz = camera_distance * std::cos(camera_yaw) * std::cos(camera_pitch);
        scene->camera_pos = scene->camera_target + glm::vec3(cx, cy, cz);
    };

    updateCamera();

    while (!quit) {
        auto current_time = std::chrono::high_resolution_clock::now();
        float dt = std::chrono::duration<float>(current_time - last_time).count();
        last_time = current_time;

        scene->physics_config.current_fps = (dt > 0.0001f) ? (1.0f / dt) : 60.0f;

        while (SDL_PollEvent(&e)) {
            // Forward event to Dear ImGui
            imgui_overlay->processEvent(e);

            ImGuiIO& io = ImGui::GetIO();
            bool imgui_wants_mouse = io.WantCaptureMouse;

            if (e.type == SDL_QUIT) {
                quit = true;
            } else if (e.type == SDL_KEYDOWN) {
                if (e.key.keysym.sym == SDLK_ESCAPE) {
                    quit = true;
                } else if (e.key.keysym.sym == SDLK_F1) {
                    imgui_overlay->toggleMetricsWindow();
                } else if (e.key.keysym.sym == SDLK_F2) {
                    imgui_overlay->toggleFilesystemWindow();
                } else if (e.key.keysym.sym == SDLK_F3) {
                    imgui_overlay->toggleOatsWindow();
                } else if (e.key.keysym.sym == SDLK_F4) {
                    imgui_overlay->toggleHypergraphWindow();
                } else if (e.key.keysym.sym == SDLK_r) {
                    camera_yaw = 0.0f;
                    camera_pitch = 0.2f;
                    camera_distance = 3.2f;
                    scene->camera_target = glm::vec3(0.0f, 0.0f, 0.0f);
                    updateCamera();
                }
                forwardKeyToCompositor(*compositor, *scene, e.key, true);
            } else if (e.type == SDL_KEYUP) {
                forwardKeyToCompositor(*compositor, *scene, e.key, false);
            } else if (e.type == SDL_MOUSEMOTION) {
                glm::vec2 screen_px(static_cast<float>(e.motion.x), static_cast<float>(e.motion.y));

                float dx = static_cast<float>(e.motion.x - last_mouse_x);
                float dy = static_cast<float>(e.motion.y - last_mouse_y);

                if (is_left_orbiting) {
                    // Left-click drag orbits camera around lookat target
                    camera_yaw -= dx * 0.005f;
                    camera_pitch += dy * 0.005f;
                    camera_pitch = std::clamp(camera_pitch, -glm::radians(85.0f), glm::radians(85.0f));
                    updateCamera();
                } else if (is_panning) {
                    // Right-click drag pans camera target in view plane
                    glm::vec3 cam_dir = glm::normalize(scene->camera_pos - scene->camera_target);
                    glm::vec3 cam_right = glm::normalize(glm::cross(scene->camera_up, cam_dir));
                    glm::vec3 cam_actual_up = glm::cross(cam_dir, cam_right);
                    glm::vec3 pan_delta = (-cam_right * dx + cam_actual_up * dy) * 0.003f;
                    scene->camera_target += pan_delta;
                    updateCamera();
                }

                // Update 3D Raycast & Cursor 3D position. Routed through the
                // compositor, not straight into the scene: it owns the one
                // pointer position that libinput devices also write through,
                // so both producers agree on where the pointer is.
                compositor->processPointerMotionAbsolute(screen_px.x, screen_px.y);

                last_mouse_x = e.motion.x;
                last_mouse_y = e.motion.y;
            } else if (e.type == SDL_MOUSEBUTTONDOWN) {
                if (!imgui_wants_mouse) {
                    if (e.button.button == SDL_BUTTON_LEFT) {
                        is_left_orbiting = true;
                        last_mouse_x = e.button.x;
                        last_mouse_y = e.button.y;
                        left_start_x = e.button.x;
                        left_start_y = e.button.y;
                    } else if (e.button.button == SDL_BUTTON_RIGHT || e.button.button == SDL_BUTTON_MIDDLE) {
                        is_panning = true;
                        last_mouse_x = e.button.x;
                        last_mouse_y = e.button.y;
                    }
                }
            } else if (e.type == SDL_MOUSEBUTTONUP) {
                if (e.button.button == SDL_BUTTON_LEFT) {
                    if (is_left_orbiting) {
                        is_left_orbiting = false;
                        // If user performed a click without dragging, send select event to 3D scene
                        int drag_dist = std::abs(e.button.x - left_start_x) + std::abs(e.button.y - left_start_y);
                        if (drag_dist <= 4 && !imgui_wants_mouse) {
                            compositor->processPointerButton(BTN_LEFT, true);
                            compositor->processPointerButton(BTN_LEFT, false);
                        }
                    }
                } else if (e.button.button == SDL_BUTTON_RIGHT || e.button.button == SDL_BUTTON_MIDDLE) {
                    is_panning = false;
                }
            } else if (e.type == SDL_MOUSEWHEEL) {
                if (!imgui_wants_mouse) {
                    camera_distance -= static_cast<float>(e.wheel.y) * 0.20f;
                    camera_distance = std::clamp(camera_distance, 0.6f, 15.0f);
                    updateCamera();
                }
            }
        }

        // Process Wayland event loop iteration. Blocks briefly while the
        // session is away rather than spinning; see dispatchDisplay().
        const bool session_active = dispatchDisplay(*compositor);

        // Step OATS-rs ECS runtime
        oats_bridge->step(dt);

        // Update 3D Scene and 3D Filesystem Pills
        filesystem_3d->update(dt);
        scene->update(dt);

        // A VT switch away takes the DRM master and the input devices with it.
        // Dispatching continues - clients keep talking, windows keep arriving -
        // but nothing is drawn and no frame callback is released, because no
        // frame was presented. Rendering resumes where it left off on the way
        // back; nothing about the session is torn down in between.
        if (!session_active) {
            continue;   // the waiting already happened, inside the dispatch
        }

        // Build Dear ImGui Frame
        imgui_overlay->newFrame();
        imgui_overlay->renderUI();

        // Render Frame via Nova Vulkan pipeline
        graphics->renderFrame([&](VkCommandBuffer cmd, uint32_t) {
            glm::vec2 screen_size(static_cast<float>(config.screen.width), static_cast<float>(config.screen.height));

            // 1. Render 3D Spatial Scene (Pills, Filesystem, Reticles)
            scene->render(pipeline.get(), cmd, screen_size);

            // 2. Render Dear ImGui Engine Controller & Metrics Overlay
            imgui_overlay->renderDrawData(cmd);
        });

        // Release the frame callbacks the clients are blocked on. Interim
        // presentation time: the render submission has returned. Under the DRM
        // backend this moves to the output present event and only this line
        // changes - the compositor already takes the timestamp as a parameter.
        struct timespec presented = {};
        clock_gettime(CLOCK_MONOTONIC, &presented);
        compositor->onFramePresented(presented);
    }

    report(LOGGER::INFO, "Display Server shutting down...");
    compositor->stop();
    return 0;
}
