// Written by Richard Christopher, Copyright 2026 NeoTec Digital
#pragma once

#include "Splash/Registry.h"
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
    UIDockBar(Splash::Registry& reg, Splash::NodeId self,
              const UITheme& ui_theme,
              float dock_width,
              std::shared_ptr<Splash::SpatialFont> font_ptr);

    void addItem(Splash::Registry& reg, const std::string& label, std::function<void()> click_handler);
    void setStatusText(Splash::Registry& reg, const std::string& status);

    const UITheme& theme() const { return *theme_; }

    void collectRender(Splash::Registry& reg, Splash::NodeId self,
                       Nova::SpatialMeshBuffer* mesh_buf,
                       std::vector<Splash::SpatialRenderCommand>& out_commands) override;

private:
    const UITheme* theme_;
    std::shared_ptr<Splash::SpatialFont> font_;
    Splash::NodeId dock_panel_;
    Splash::NodeId status_label_;
    std::vector<Splash::NodeId> buttons_;

    float total_width_ = 1.6f;
    float current_x_ = -0.6f;

    // Pushes the resolved material onto the dock's own panel, once per frame
    // from collectRender. See the seam contract at the foot of UITheme.h.
    void syncChromeToTheme(Splash::Registry& reg);
};

inline Splash::NodeId makeUIDockBar(Splash::Registry& reg, Splash::NodeId parent,
                                    const UITheme& theme, float dock_width,
                                    std::shared_ptr<Splash::SpatialFont> font) {
    return reg.emplace<UIDockBar>(parent, theme, dock_width, std::move(font));
}

} // namespace Clouds::UI
