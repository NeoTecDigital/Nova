#pragma once

#include "../SpatialNode.h"
#include "../Primitives.h"
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

class UIMenuDropdown : public SpatialNode {
public:
    UIMenuDropdown(const std::vector<MenuItem>& items,
                   float width,
                   std::shared_ptr<NovaSpatial::SpatialFont> font_ptr,
                   std::function<void()> on_item_selected = nullptr);

    void collectRender(NovaSpatial::SpatialMeshBuffer* mesh_buf, std::vector<SpatialRenderCommand>& out_commands) override;

private:
    std::shared_ptr<SpatialPanel> panel_;
    std::vector<std::shared_ptr<UIButton>> item_buttons_;
};

class UIMenuBar : public SpatialNode {
public:
    UIMenuBar(float bar_width,
              std::shared_ptr<NovaSpatial::SpatialFont> font_ptr);

    void addMenu(const std::string& category_name, const std::vector<MenuItem>& items);
    void setTelemetryText(const std::string& text);
    void closeAllDropdowns();

    void collectRender(NovaSpatial::SpatialMeshBuffer* mesh_buf, std::vector<SpatialRenderCommand>& out_commands) override;

private:
    std::shared_ptr<NovaSpatial::SpatialFont> font_;
    std::shared_ptr<SpatialPanel> bar_panel_;
    std::shared_ptr<UILabel> brand_label_;
    std::shared_ptr<UILabel> telemetry_label_;

    std::vector<MenuCategory> categories_;
    std::vector<std::shared_ptr<UIButton>> menu_buttons_;
    std::vector<std::shared_ptr<UIMenuDropdown>> dropdowns_;

    int open_dropdown_index_ = -1;
    float current_x_offset_ = -1.25f;

    void toggleDropdown(int index);
};

} // namespace Clouds::UI
