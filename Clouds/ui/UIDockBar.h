// Written by Richard Christopher, Copyright 2026 NeoTec Digital
#pragma once

#include "Splash/SpatialNode.h"
#include "Splash/Primitives.h"
#include "./UIComponents.h"
#include "./UITheme.h"
#include <memory>
#include <vector>
#include <string>
#include <functional>

namespace Clouds::UI {

struct DockItem {
    std::string icon_label;
    std::string tooltip;
    std::function<void()> on_click;
};

class UIDockBar : public Splash::SpatialNode {
public:
    UIDockBar(const UITheme& ui_theme,
              float dock_width,
              std::shared_ptr<Splash::SpatialFont> font_ptr);

    void addItem(const std::string& label, std::function<void()> click_handler);
    void setStatusText(const std::string& status);

    const UITheme& theme() const { return *theme_; }

    void collectRender(Nova::SpatialMeshBuffer* mesh_buf, std::vector<Splash::SpatialRenderCommand>& out_commands) override;

private:
    const UITheme* theme_;
    std::shared_ptr<Splash::SpatialFont> font_;
    std::shared_ptr<Splash::SpatialPanel> dock_panel_;
    std::shared_ptr<UILabel> status_label_;
    std::vector<std::shared_ptr<UIButton>> buttons_;

    float total_width_ = 1.6f;
    float current_x_ = -0.6f;

    // Pushes the resolved material onto the dock's own panel, once per frame
    // from collectRender. See the seam contract at the foot of UITheme.h.
    void syncChromeToTheme();
};

} // namespace Clouds::UI
