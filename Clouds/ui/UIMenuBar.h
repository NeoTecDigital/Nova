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
    UIMenuDropdown(const UITheme& ui_theme,
                   const std::vector<MenuItem>& items,
                   float width,
                   std::shared_ptr<Splash::SpatialFont> font_ptr,
                   std::function<void()> on_item_selected = nullptr);

    const UITheme& theme() const { return *theme_; }

    void collectRender(Nova::SpatialMeshBuffer* mesh_buf, std::vector<Splash::SpatialRenderCommand>& out_commands) override;

private:
    const UITheme* theme_;
    std::shared_ptr<Splash::SpatialPanel> panel_;
    std::vector<std::shared_ptr<UIButton>> item_buttons_;

    void syncChromeToTheme();
};

class UIMenuBar : public Splash::SpatialNode {
public:
    UIMenuBar(const UITheme& ui_theme,
              float bar_width,
              std::shared_ptr<Splash::SpatialFont> font_ptr);

    void addMenu(const std::string& category_name, const std::vector<MenuItem>& items);
    void setTelemetryText(const std::string& text);
    void closeAllDropdowns();

    const UITheme& theme() const { return *theme_; }

    void collectRender(Nova::SpatialMeshBuffer* mesh_buf, std::vector<Splash::SpatialRenderCommand>& out_commands) override;

private:
    const UITheme* theme_;
    std::shared_ptr<Splash::SpatialFont> font_;
    std::shared_ptr<Splash::SpatialPanel> bar_panel_;
    std::shared_ptr<UILabel> brand_label_;
    std::shared_ptr<UILabel> telemetry_label_;

    std::vector<MenuCategory> categories_;
    std::vector<std::shared_ptr<UIButton>> menu_buttons_;
    std::vector<std::shared_ptr<UIMenuDropdown>> dropdowns_;

    int open_dropdown_index_ = -1;
    float current_x_offset_ = -1.25f;

    void toggleDropdown(int index);

    // Pushes the resolved material onto the bar's own panel, once per frame
    // from collectRender. See the seam contract at the foot of UITheme.h.
    void syncChromeToTheme();
};

} // namespace Clouds::UI
