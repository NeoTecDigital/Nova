// Written by Richard Christopher, Copyright 2026 NeoTec Digital
#pragma once

#include "Core/nova_graphics.h"
#include "Core/math/engine_physics.h"
#include "./OatsBridge.h"
#include "./SpatialFilesystem.h"
#include <SDL2/SDL.h>
#include <vector>
#include <memory>
#include <string>

namespace Clouds {

class ImGuiEngineOverlay {
public:
    ImGuiEngineOverlay(NovaGraphics* graphics,
                       SDL_Window* window,
                       NovaMath::EnginePhysicsConfig* physics_config,
                       std::shared_ptr<OatsBridge> oats_bridge,
                       std::shared_ptr<SpatialFilesystem> filesystem_3d);
    ~ImGuiEngineOverlay();

    bool initialize();
    void processEvent(const SDL_Event& event);
    void newFrame();
    void renderUI();
    void renderDrawData(VkCommandBuffer cmd);

    // Window toggles
    void toggleMetricsWindow() { show_metrics_window_ = !show_metrics_window_; }
    void toggleFilesystemWindow() { show_filesystem_window_ = !show_filesystem_window_; }
    void toggleOatsWindow() { show_oats_window_ = !show_oats_window_; }
    void toggleHypergraphWindow() { show_hypergraph_window_ = !show_hypergraph_window_; }

private:
    NovaGraphics* graphics_ = nullptr;
    SDL_Window* window_ = nullptr;
    NovaMath::EnginePhysicsConfig* physics_config_ = nullptr;
    std::shared_ptr<OatsBridge> oats_bridge_;
    std::shared_ptr<SpatialFilesystem> filesystem_3d_;

    VkDescriptorPool descriptor_pool_ = VK_NULL_HANDLE;
    bool initialized_ = false;

    // Window Visibility Flags
    bool show_metrics_window_ = true;
    bool show_filesystem_window_ = true;
    bool show_oats_window_ = false;
    bool show_hypergraph_window_ = false;
    bool show_demo_window_ = false;

    // Rolling metrics history
    std::vector<float> fps_history_;
    size_t max_history_size_ = 100;

    void applyDarkTheme();
    void renderMainMenuBar();
    void renderMetricsWindow();
    void renderFilesystemWindow();
    void renderOatsWindow();
    void renderOatsRuntimeStatus();
    void renderOatsEntityTables();
    void renderOatsEventLog();
    void renderHypergraphWindow();
    void renderHypergraphNodeList();
};

} // namespace Clouds
