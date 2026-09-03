// Written by Richard Christopher, Copyright 2026 NeoTec Digital
#pragma once

#include "./UIWindow.h"
#include "./UIMenuBar.h"
#include "./UIDockBar.h"
#include "include/Clouds/OatsBridge.h"
#include "Core/math/engine_physics.h"
#include <vector>
#include <memory>

namespace Clouds::UI {

class WindowManager {
public:
    WindowManager(std::shared_ptr<Splash::SpatialNode> scene_root,
                  std::shared_ptr<Nova::SpatialFont> font_ptr,
                  std::shared_ptr<Splash::OatsBridge> oats_bridge,
                  Nova::Math::EnginePhysicsConfig* physics_config);
    ~WindowManager() = default;

    void initialize();
    void update(float dt);

    void addWindow(std::shared_ptr<UIWindow> window);
    void bringToFront(UIWindow* window);
    void toggleWindow(const std::string& window_title);

    void resetLayout();

    std::shared_ptr<UIMenuBar> getMenuBar() const { return menubar_; }
    std::shared_ptr<UIDockBar> getDockBar() const { return dockbar_; }

private:
    std::shared_ptr<Splash::SpatialNode> root_;
    std::shared_ptr<Nova::SpatialFont> font_;
    std::shared_ptr<Splash::OatsBridge> oats_bridge_;
    Nova::Math::EnginePhysicsConfig* physics_config_ = nullptr;

    std::shared_ptr<Splash::SpatialNode> window_root_;
    std::shared_ptr<UIMenuBar> menubar_;
    std::shared_ptr<UIDockBar> dockbar_;

    std::vector<std::shared_ptr<UIWindow>> windows_;
    UIWindow* active_window_ = nullptr;

    // Specific Built-in Windows
    std::shared_ptr<UIWindow> physics_window_;
    std::shared_ptr<UIWindow> hypergraph_window_;

    // Physics UI references
    std::shared_ptr<UILabel> physics_stats_label_;
    std::shared_ptr<UIButton> laser_focus_btn_;
    std::shared_ptr<UIButton> dither_btn_;

    void createMenuBar();
    void createDockBar();
    void createPhysicsWindow();
    void createHypergraphWindow();

    void refreshWindowContents();
};

} // namespace Clouds::UI
