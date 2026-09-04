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

struct MenuItem {
    std::string title;
    std::string shortcut;
    std::function<void()> action;
};

struct MenuCategory {
    std::string name;
    std::vector<MenuItem> items;
};

class UIMenuDropdown : public Splash::SpatialNode {
public:
    UIMenuDropdown(Splash::Registry& reg, Splash::NodeId self,
                   const UITheme& ui_theme,
                   const std::vector<MenuItem>& items,
                   float width,
                   std::shared_ptr<Splash::SpatialFont> font_ptr,
                   std::function<void()> on_item_selected = nullptr);

    const UITheme& theme() const { return *theme_; }

    void collectRender(Splash::Registry& reg, Splash::NodeId self,
                       Nova::SpatialMeshBuffer* mesh_buf,
                       std::vector<Splash::SpatialRenderCommand>& out_commands) override;

private:
    const UITheme* theme_;
    Splash::NodeId panel_;
    std::vector<Splash::NodeId> item_buttons_;

    void syncChromeToTheme(Splash::Registry& reg);
};

class UIMenuBar : public Splash::SpatialNode {
public:
    UIMenuBar(Splash::Registry& reg, Splash::NodeId self,
              const UITheme& ui_theme,
              float bar_width,
              std::shared_ptr<Splash::SpatialFont> font_ptr);

    // `self` is this bar's own id: the dropdowns hang off the bar rather than
    // off its panel, so that they draw over the buttons instead of under them.
    void addMenu(Splash::Registry& reg, Splash::NodeId self,
                 const std::string& category_name, const std::vector<MenuItem>& items);
    void setTelemetryText(Splash::Registry& reg, const std::string& text);
    void closeAllDropdowns(Splash::Registry& reg);

    const UITheme& theme() const { return *theme_; }

    void collectRender(Splash::Registry& reg, Splash::NodeId self,
                       Nova::SpatialMeshBuffer* mesh_buf,
                       std::vector<Splash::SpatialRenderCommand>& out_commands) override;

private:
    const UITheme* theme_;
    std::shared_ptr<Splash::SpatialFont> font_;
    Splash::NodeId bar_panel_;
    Splash::NodeId brand_label_;
    Splash::NodeId telemetry_label_;

    std::vector<MenuCategory> categories_;
    std::vector<Splash::NodeId> menu_buttons_;
    std::vector<Splash::NodeId> dropdowns_;

    int open_dropdown_index_ = -1;
    float current_x_offset_ = -1.25f;

    void toggleDropdown(Splash::Registry& reg, int index);

    // Pushes the resolved material onto the bar's own panel, once per frame
    // from collectRender. See the seam contract at the foot of UITheme.h.
    void syncChromeToTheme(Splash::Registry& reg);
};

inline Splash::NodeId makeUIMenuBar(Splash::Registry& reg, Splash::NodeId parent,
                                    const UITheme& theme, float bar_width,
                                    std::shared_ptr<Splash::SpatialFont> font) {
    return reg.emplace<UIMenuBar>(parent, theme, bar_width, std::move(font));
}

} // namespace Clouds::UI
