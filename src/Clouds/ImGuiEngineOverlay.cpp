// Written by Richard Christopher, Copyright 2026 NeoTec Digital
#include "include/Clouds/ImGuiEngineOverlay.h"
#include "Core/components/logger.h"
#include <imgui.h>
#include <backends/imgui_impl_sdl2.h>
#include <backends/imgui_impl_vulkan.h>
#include <iostream>
#include <numeric>

namespace Clouds {

static void check_vk_result(VkResult err) {
    if (err == 0) return;
    report(LOGGER::ERROR, "[ImGui Vulkan Error] VkResult = %d", err);
}

ImGuiEngineOverlay::ImGuiEngineOverlay(Nova::Graphics* graphics,
                                       SDL_Window* window,
                                       Nova::Math::EnginePhysicsConfig* physics_config,
                                       std::shared_ptr<Splash::OatsBridge> oats_bridge,
                                       std::shared_ptr<SpatialFilesystem> filesystem_3d)
    : graphics_(graphics), window_(window), physics_config_(physics_config),
      oats_bridge_(oats_bridge), filesystem_3d_(filesystem_3d) {
    fps_history_.resize(max_history_size_, 60.0f);
}

ImGuiEngineOverlay::~ImGuiEngineOverlay() {
    if (initialized_) {
        VkDevice device = graphics_->getDevice();
        vkDeviceWaitIdle(device);

        ImGui_ImplVulkan_Shutdown();
        ImGui_ImplSDL2_Shutdown();
        ImGui::DestroyContext();

        if (descriptor_pool_ != VK_NULL_HANDLE) {
            vkDestroyDescriptorPool(device, descriptor_pool_, nullptr);
            descriptor_pool_ = VK_NULL_HANDLE;
        }
    }
}

bool ImGuiEngineOverlay::initialize() {
    if (!graphics_ || !window_) return false;

    report(LOGGER::INFO, "ImGuiEngineOverlay - Initializing Dear ImGui Vulkan Overlay...");

    // 1. Create Descriptor Pool for ImGui
    VkDescriptorPoolSize pool_sizes[] = {
        { VK_DESCRIPTOR_TYPE_SAMPLER, 1000 },
        { VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1000 },
        { VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, 1000 },
        { VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 1000 },
        { VK_DESCRIPTOR_TYPE_UNIFORM_TEXEL_BUFFER, 1000 },
        { VK_DESCRIPTOR_TYPE_STORAGE_TEXEL_BUFFER, 1000 },
        { VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 1000 },
        { VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1000 },
        { VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC, 1000 },
        { VK_DESCRIPTOR_TYPE_STORAGE_BUFFER_DYNAMIC, 1000 },
        { VK_DESCRIPTOR_TYPE_INPUT_ATTACHMENT, 1000 }
    };

    VkDescriptorPoolCreateInfo pool_info = {};
    pool_info.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
    pool_info.flags = VK_DESCRIPTOR_POOL_CREATE_FREE_DESCRIPTOR_SET_BIT;
    pool_info.maxSets = 1000 * IM_ARRAYSIZE(pool_sizes);
    pool_info.poolSizeCount = (uint32_t)IM_ARRAYSIZE(pool_sizes);
    pool_info.pPoolSizes = pool_sizes;

    VkResult res = vkCreateDescriptorPool(graphics_->getDevice(), &pool_info, nullptr, &descriptor_pool_);
    if (res != VK_SUCCESS) {
        report(LOGGER::ERROR, "ImGuiEngineOverlay - Failed to create descriptor pool");
        return false;
    }

    // 2. Setup Dear ImGui Context
    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;

    applyDarkTheme();

    // 3. Setup Platform / Renderer Backends
    ImGui_ImplSDL2_InitForVulkan(window_);

    ImGui_ImplVulkan_InitInfo init_info = {};
    init_info.Instance = graphics_->getInstance();
    init_info.PhysicalDevice = graphics_->getPhysicalDevice();
    init_info.Device = graphics_->getDevice();
    init_info.QueueFamily = 0;
    init_info.Queue = graphics_->getGraphicsQueue();
    init_info.PipelineCache = VK_NULL_HANDLE;
    init_info.DescriptorPool = descriptor_pool_;
    init_info.MinImageCount = 2;
    init_info.ImageCount = static_cast<uint32_t>(graphics_->getSwapchain().images.size());
    init_info.CheckVkResultFn = check_vk_result;
    init_info.PipelineInfoMain.RenderPass = graphics_->getRenderPass();
    init_info.PipelineInfoMain.Subpass = 0;
    init_info.PipelineInfoMain.MSAASamples = VK_SAMPLE_COUNT_1_BIT;

    ImGui_ImplVulkan_Init(&init_info);

    initialized_ = true;
    report(LOGGER::INFO, "ImGuiEngineOverlay - Dear ImGui active with Vulkan backend.");
    return true;
}

void ImGuiEngineOverlay::applyDarkTheme() {
    ImGui::StyleColorsDark();
    ImGuiStyle& style = ImGui::GetStyle();

    style.WindowRounding = 6.0f;
    style.FrameRounding = 4.0f;
    style.PopupRounding = 6.0f;
    style.ScrollbarRounding = 6.0f;
    style.GrabRounding = 4.0f;
    style.TabRounding = 4.0f;

    style.WindowBorderSize = 1.0f;
    style.FrameBorderSize = 0.0f;
    style.PopupBorderSize = 1.0f;

    ImVec4* colors = style.Colors;
    colors[ImGuiCol_WindowBg]             = ImVec4(0.08f, 0.10f, 0.14f, 0.94f);
    colors[ImGuiCol_Header]               = ImVec4(0.18f, 0.24f, 0.35f, 0.80f);
    colors[ImGuiCol_HeaderHovered]        = ImVec4(0.25f, 0.38f, 0.60f, 0.90f);
    colors[ImGuiCol_HeaderActive]         = ImVec4(0.20f, 0.32f, 0.50f, 1.00f);
    colors[ImGuiCol_Button]               = ImVec4(0.18f, 0.28f, 0.48f, 0.85f);
    colors[ImGuiCol_ButtonHovered]        = ImVec4(0.25f, 0.40f, 0.70f, 1.00f);
    colors[ImGuiCol_ButtonActive]         = ImVec4(0.15f, 0.22f, 0.38f, 1.00f);
    colors[ImGuiCol_FrameBg]              = ImVec4(0.12f, 0.15f, 0.22f, 0.85f);
    colors[ImGuiCol_FrameBgHovered]       = ImVec4(0.18f, 0.24f, 0.35f, 0.85f);
    colors[ImGuiCol_FrameBgActive]        = ImVec4(0.22f, 0.30f, 0.45f, 1.00f);
    colors[ImGuiCol_TitleBg]              = ImVec4(0.10f, 0.12f, 0.18f, 1.00f);
    colors[ImGuiCol_TitleBgActive]        = ImVec4(0.15f, 0.20f, 0.32f, 1.00f);
    colors[ImGuiCol_CheckMark]            = ImVec4(0.35f, 0.75f, 1.00f, 1.00f);
    colors[ImGuiCol_SliderGrab]           = ImVec4(0.30f, 0.60f, 0.95f, 1.00f);
    colors[ImGuiCol_SliderGrabActive]     = ImVec4(0.40f, 0.75f, 1.00f, 1.00f);
    colors[ImGuiCol_Border]               = ImVec4(0.25f, 0.35f, 0.50f, 0.60f);
    colors[ImGuiCol_Separator]            = ImVec4(0.20f, 0.28f, 0.40f, 0.60f);
}

void ImGuiEngineOverlay::processEvent(const SDL_Event& event) {
    if (!initialized_) return;
    ImGui_ImplSDL2_ProcessEvent(&event);
}

void ImGuiEngineOverlay::newFrame() {
    if (!initialized_) return;

    ImGui_ImplVulkan_NewFrame();
    ImGui_ImplSDL2_NewFrame();
    ImGui::NewFrame();

    // Update rolling FPS history
    if (physics_config_) {
        fps_history_.erase(fps_history_.begin());
        fps_history_.push_back(physics_config_->current_fps);
    }
}

void ImGuiEngineOverlay::renderUI() {
    if (!initialized_) return;

    renderMainMenuBar();

    if (show_metrics_window_) renderMetricsWindow();
    if (show_filesystem_window_) renderFilesystemWindow();
    if (show_oats_window_) renderOatsWindow();
    if (show_hypergraph_window_) renderHypergraphWindow();
    if (show_demo_window_) ImGui::ShowDemoWindow(&show_demo_window_);
}

void ImGuiEngineOverlay::renderMainMenuBar() {
    if (ImGui::BeginMainMenuBar()) {
        if (ImGui::BeginMenu("File")) {
            if (ImGui::MenuItem("Rescan Filesystem", "F5")) {
                if (filesystem_3d_) filesystem_3d_->rescan();
            }
            ImGui::Separator();
            if (ImGui::MenuItem("Exit", "Esc")) {
                SDL_Event quit_ev;
                quit_ev.type = SDL_QUIT;
                SDL_PushEvent(&quit_ev);
            }
            ImGui::EndMenu();
        }

        if (ImGui::BeginMenu("Physics")) {
            if (physics_config_) {
                bool laser = (physics_config_->accel_mode == Nova::Math::AccelerationMode::LaserFocus);
                if (ImGui::MenuItem("LaserFocus Acceleration", nullptr, laser)) {
                    physics_config_->accel_mode = laser ?
                        Nova::Math::AccelerationMode::ClusteredDither : Nova::Math::AccelerationMode::LaserFocus;
                }
                if (ImGui::MenuItem("Temporal Sub-pixel Dithering", nullptr, physics_config_->dither_enabled)) {
                    physics_config_->dither_enabled = !physics_config_->dither_enabled;
                }
                ImGui::Separator();
                if (ImGui::MenuItem("Reset Physics Defaults")) {
                    physics_config_->phase_coupling_strength = 1.0f;
                    physics_config_->phase_velocity = 2.0f;
                    physics_config_->laser_precision = 1.0f;
                    physics_config_->dither_enabled = true;
                }
            }
            ImGui::EndMenu();
        }

        if (ImGui::BeginMenu("Windows")) {
            ImGui::MenuItem("Metrics & Engine Controller", "F1", &show_metrics_window_);
            ImGui::MenuItem("3D Filesystem & Pill Inspector", "F2", &show_filesystem_window_);
            ImGui::MenuItem("OATS-rs ECS & State Machine", "F3", &show_oats_window_);
            ImGui::MenuItem("Lumberjack Hypergraph DAG", "F4", &show_hypergraph_window_);
            ImGui::Separator();
            ImGui::MenuItem("ImGui Demo Showcase", nullptr, &show_demo_window_);
            ImGui::EndMenu();
        }

        // Right-aligned status indicators
        float avail = ImGui::GetContentRegionAvail().x;
        float text_width = 300.0f;
        if (avail > text_width) {
            ImGui::SameLine(ImGui::GetWindowWidth() - text_width);
            if (physics_config_) {
                ImGui::TextColored(ImVec4(0.3f, 0.85f, 0.4f, 1.0f), "%.1f FPS | 3D Pills: %zu",
                                   physics_config_->current_fps, filesystem_3d_ ? filesystem_3d_->getNodeCount() : 0);
            }
        }

        ImGui::EndMainMenuBar();
    }
}

void ImGuiEngineOverlay::renderMetricsWindow() {
    ImGui::SetNextWindowSize(ImVec2(440, 480), ImGuiCond_FirstUseEver);
    ImGui::SetNextWindowPos(ImVec2(20, 40), ImGuiCond_FirstUseEver);

    if (ImGui::Begin("Engine Physics & Telemetry (F1)", &show_metrics_window_)) {
        if (physics_config_) {
            ImGui::TextColored(ImVec4(0.3f, 0.8f, 1.0f, 1.0f), ":: Mathematical Substrate & Real-Time Profiler ::");
            ImGui::Separator();

            // FPS Plot
            char fps_label[32];
            snprintf(fps_label, sizeof(fps_label), "%.1f FPS (%.2f ms)", physics_config_->current_fps, 1000.0f / std::max(physics_config_->current_fps, 1.0f));
            ImGui::PlotLines("Frame Rate", fps_history_.data(), (int)fps_history_.size(), 0, fps_label, 0.0f, 1200.0f, ImVec2(0, 65));

            ImGui::Spacing();
            ImGui::Text("Active 3D Entities: %zu", filesystem_3d_ ? filesystem_3d_->getNodeCount() : 0);
            ImGui::Text("Raycast Tests/Frame: %u", physics_config_->cluster_tests_per_frame);

            ImGui::Spacing();
            ImGui::SeparatorText("Complex Phase & Quaternionic Dynamics");
            ImGui::SliderFloat("Coupling (lambda)", &physics_config_->phase_coupling_strength, 0.0f, 5.0f, "%.2f");
            ImGui::SliderFloat("Velocity (omega)", &physics_config_->phase_velocity, -10.0f, 10.0f, "%.2f rad/s");
            ImGui::SliderFloat("Harmonic Decay", &physics_config_->phase_decay, 0.80f, 1.0f, "%.3f");

            ImGui::Spacing();
            ImGui::SeparatorText("Spatial Index & Acceleration");
            int mode_idx = static_cast<int>(physics_config_->accel_mode);
            const char* modes[] = { "LaserFocus (Pinpoint)", "ClusteredDither (Hierarchical)", "LazyUniform (Grid)" };
            if (ImGui::Combo("Acceleration Mode", &mode_idx, modes, IM_ARRAYSIZE(modes))) {
                physics_config_->accel_mode = static_cast<Nova::Math::AccelerationMode>(mode_idx);
            }

            ImGui::SliderFloat("Laser Precision", &physics_config_->laser_precision, 0.1f, 2.0f, "%.2f");
            ImGui::SliderInt("Cluster Depth", &physics_config_->cluster_depth, 1, 8);
            ImGui::Checkbox("Temporal Dithering", &physics_config_->dither_enabled);
            if (physics_config_->dither_enabled) {
                ImGui::SliderFloat("Dither Amplitude", &physics_config_->dither_amplitude, 0.0f, 2.0f, "%.2f px");
            }
        }
    }
    ImGui::End();
}

void ImGuiEngineOverlay::renderFilesystemWindow() {
    ImGui::SetNextWindowSize(ImVec2(480, 520), ImGuiCond_FirstUseEver);
    ImGui::SetNextWindowPos(ImVec2(ImGui::GetIO().DisplaySize.x - 500, 40), ImGuiCond_FirstUseEver);

    if (ImGui::Begin("3D Filesystem & Pill Inspector (F2)", &show_filesystem_window_)) {
        ImGui::TextColored(ImVec4(0.4f, 0.9f, 0.6f, 1.0f), ":: Interactive 3D Quaternionic Space ::");
        ImGui::Separator();

        SpatialPillNode* selected = filesystem_3d_ ? filesystem_3d_->getSelectedNode() : nullptr;

        if (selected) {
            ImGui::TextColored(ImVec4(1.0f, 0.85f, 0.2f, 1.0f), "Selected 3D Pill: %s", selected->item_name.c_str());
            ImGui::Text("Type: %s", selected->is_directory ? "Directory (Branch Hub)" : "File (Leaf Entity)");
            ImGui::Text("Path: %s", selected->full_path.c_str());
            if (!selected->is_directory) {
                ImGui::Text("Size: %ju bytes (%.2f KB)", selected->file_size, float(selected->file_size) / 1024.0f);
                ImGui::Text("Extension: %s", selected->file_extension.c_str());
            }

            ImGui::Spacing();
            ImGui::SeparatorText("3D Spatial Pose");
            glm::vec3 pos = selected->transform.position;
            glm::quat rot = selected->transform.orientation;
            ImGui::Text("Position: (%.2f, %.2f, %.2f)", pos.x, pos.y, pos.z);
            ImGui::Text("Orientation: [w=%.2f, x=%.2f, y=%.2f, z=%.2f]", rot.w, rot.x, rot.y, rot.z);
            ImGui::Text("Pill Dimensions: R=%.3f, H=%.3f", selected->pill_radius, selected->pill_height);
            ImGui::ColorEdit4("Base Color", &selected->base_color.x, ImGuiColorEditFlags_NoInputs);
        } else {
            ImGui::TextDisabled("No 3D pill selected. Click any 3D pill in the viewport to inspect.");
        }

        ImGui::Spacing();
        ImGui::SeparatorText("Directory Tree Navigation");
        if (filesystem_3d_) {
            std::string rescan_label = "Rescan " + filesystem_3d_->getScanRoot();
            if (ImGui::Button(rescan_label.c_str())) {
                filesystem_3d_->rescan();
            }
        }

        if (filesystem_3d_) {
            const auto& all_pills = filesystem_3d_->getAllNodes();
            ImGui::BeginChild("PillList", ImVec2(0, 200), true);
            for (const auto& pill : all_pills) {
                bool is_cur = (pill.get() == selected);
                std::string label = (pill->is_directory ? "[DIR] " : "      ") + pill->item_name;
                if (ImGui::Selectable(label.c_str(), is_cur)) {
                    filesystem_3d_->selectNode(pill.get());
                }
            }
            ImGui::EndChild();
        }
    }
    ImGui::End();
}

void ImGuiEngineOverlay::renderOatsWindow() {
    ImGui::SetNextWindowSize(ImVec2(460, 420), ImGuiCond_FirstUseEver);
    ImGui::SetNextWindowPos(ImVec2(20, ImGui::GetIO().DisplaySize.y - 450), ImGuiCond_FirstUseEver);

    if (ImGui::Begin("OATS-rs Runtime & State Deltas (F3)", &show_oats_window_)) {
        if (oats_bridge_) {
            renderOatsRuntimeStatus();
            renderOatsEntityTables();
            renderOatsEventLog();
        } else {
            ImGui::TextColored(ImVec4(0.9f, 0.3f, 0.3f, 1.0f), "OATS bridge unavailable.");
        }
    }
    ImGui::End();
}

void ImGuiEngineOverlay::renderOatsRuntimeStatus() {
    ImGui::TextColored(ImVec4(0.9f, 0.4f, 0.2f, 1.0f), ":: Rust ECS Runtime ::");
    ImGui::Text("Tick: %ju | Objects: %zu | Registered types: %zu",
                (uintmax_t)oats_bridge_->getTick(),
                oats_bridge_->getObjectCount(),
                oats_bridge_->getRegisteredTypeNames().size());

    if (oats_bridge_->isHealthy()) {
        ImGui::TextColored(ImVec4(0.4f, 0.8f, 0.4f, 1.0f), "Bridge healthy.");
    } else {
        ImGui::TextColored(ImVec4(0.95f, 0.35f, 0.30f, 1.0f), "Errors: %ju | Last: %s",
                           (uintmax_t)oats_bridge_->getErrorCount(),
                           oats_bridge_->getLastError().c_str());
        ImGui::SameLine();
        if (ImGui::Button("Clear")) {
            oats_bridge_->clearError();
        }
    }

    if (ImGui::Button("Reload snapshot from runtime")) {
        oats_bridge_->reloadFromRuntime();
    }
    ImGui::Separator();
}

void ImGuiEngineOverlay::renderOatsEntityTables() {
    ImGui::SeparatorText("Spatial Pills");
    for (const auto& [id, pill] : oats_bridge_->getSpatialPills()) {
        ImGui::Text("%s  |  (%.2f, %.2f, %.2f)  |  r %.3f  h %.3f",
                    pill.name.c_str(), pill.position.x, pill.position.y, pill.position.z,
                    pill.radius, pill.height);
    }

    ImGui::SeparatorText("Filesystem Entities");
    for (const auto& [id, entity] : oats_bridge_->getFilesystemEntities()) {
        ImGui::Text("%s%s  |  %ju bytes  |  %s",
                    entity.is_directory ? "[DIR] " : "      ", entity.path.c_str(),
                    (uintmax_t)entity.size_bytes, entity.extension.c_str());
    }

    ImGui::SeparatorText("Hypergraph DAG Nodes");
    for (const auto& [id, node] : oats_bridge_->getHypergraphNodes()) {
        ImGui::Text("%s  @ %s", node.node_id.c_str(), node.name_space.c_str());
    }
}

void ImGuiEngineOverlay::renderOatsEventLog() {
    ImGui::Spacing();
    ImGui::SeparatorText("Recent ECS Events");
    const auto& events = oats_bridge_->getRecentEvents();
    ImGui::BeginChild("EventLog", ImVec2(0, 100), true);
    for (auto it = events.rbegin(); it != events.rend(); ++it) {
        ImGui::TextColored(ImVec4(0.4f, 0.8f, 0.4f, 1.0f), "[%s] %s",
                           it->type.c_str(), it->message.c_str());
    }
    ImGui::EndChild();
}

void ImGuiEngineOverlay::renderHypergraphWindow() {
    ImGui::SetNextWindowSize(ImVec2(440, 320), ImGuiCond_FirstUseEver);
    ImGui::SetNextWindowPos(ImVec2(ImGui::GetIO().DisplaySize.x - 460, ImGui::GetIO().DisplaySize.y - 340), ImGuiCond_FirstUseEver);

    if (ImGui::Begin("Lumberjack Hypergraph DAG (F4)", &show_hypergraph_window_)) {
        renderHypergraphNodeList();
    }
    ImGui::End();
}

void ImGuiEngineOverlay::renderHypergraphNodeList() {
    ImGui::TextColored(ImVec4(0.2f, 0.7f, 0.9f, 1.0f), ":: Multi-Parent Forest & JIT Storage ::");
    ImGui::Separator();

    if (!oats_bridge_) {
        ImGui::TextColored(ImVec4(0.9f, 0.3f, 0.3f, 1.0f), "OATS bridge unavailable.");
        return;
    }

    const auto& nodes = oats_bridge_->getHypergraphNodes();
    ImGui::Text("Registered DAG nodes: %zu", nodes.size());
    ImGui::Spacing();
    ImGui::BeginChild("DagNodes", ImVec2(0, 190), true);
    for (const auto& [id, node] : nodes) {
        ImGui::Text("%s", node.node_id.c_str());
        ImGui::TextDisabled("  ns: %s  parents: %s",
                            node.name_space.c_str(), node.parents_json.c_str());
    }
    ImGui::EndChild();
}

void ImGuiEngineOverlay::renderDrawData(VkCommandBuffer cmd) {
    if (!initialized_) return;

    ImGui::Render();
    ImDrawData* draw_data = ImGui::GetDrawData();
    if (draw_data) {
        ImGui_ImplVulkan_RenderDrawData(draw_data, cmd);
    }
}

} // namespace Clouds
