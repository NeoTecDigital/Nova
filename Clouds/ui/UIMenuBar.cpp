// Written by Richard Christopher, Copyright 2026 NeoTec Digital
#include "./UIMenuBar.h"
#include "Splash/Primitives.h"

namespace Clouds::UI {

// ---------------------------------------------------------------------------
// UIMenuDropdown Implementation
// ---------------------------------------------------------------------------
UIMenuDropdown::UIMenuDropdown(Splash::Registry& reg, Splash::NodeId self,
                               const UITheme& ui_theme,
                               const std::vector<MenuItem>& items,
                               float width,
                               std::shared_ptr<Splash::SpatialFont> font_ptr,
                               std::function<void()> on_item_selected)
    : SpatialNode(reg, self), theme_(&ui_theme) {
    name = "UIMenuDropdown";
    float item_h = 0.045f;
    float total_h = items.size() * item_h + 0.02f;
    size = glm::vec2(width, total_h);

    panel_ = reg.emplace<Splash::SpatialPanel>(self, size, theme_->surface_elevated);

    float start_y = total_h * 0.5f - 0.01f - item_h * 0.5f;

    for (size_t i = 0; i < items.size(); ++i) {
        const auto& it = items[i];
        std::string label_txt = it.shortcut.empty() ? it.title : (it.title + "   " + it.shortcut);
        auto act = it.action;

        const Splash::NodeId btn = makeUIButton(
            reg, panel_, *theme_, label_txt,
            glm::vec2(width - 0.02f, item_h - 0.005f), font_ptr,
            [act, on_item_selected]() {
                if (act) act();
                if (on_item_selected) on_item_selected();
            },
            ButtonVariant::GHOST);
        reg.transform(btn).position = glm::vec3(0.0f, start_y - i * item_h, 0.003f);
        item_buttons_.push_back(btn);
    }

    // A starting value, not a stored one; collectRender re-resolves every frame.
    syncChromeToTheme(reg);
}

void UIMenuDropdown::syncChromeToTheme(Splash::Registry& reg) {
    Splash::SpatialPanel* panel = reg.as<Splash::SpatialPanel>(panel_);
    if (!panel) return;
    panel->background_color = theme_->surface_elevated;
    panel->corner_radius = theme_->radius_panel;
    panel->border_thickness = theme_->border_panel;
    panel->border_color = theme_->window_border_active;
}

void UIMenuDropdown::collectRender(Splash::Registry& reg, Splash::NodeId,
                                   Nova::SpatialMeshBuffer*,
                                   std::vector<Splash::SpatialRenderCommand>&) {
    syncChromeToTheme(reg);
}

// ---------------------------------------------------------------------------
// UIMenuBar Implementation
// ---------------------------------------------------------------------------
UIMenuBar::UIMenuBar(Splash::Registry& reg, Splash::NodeId self,
                     const UITheme& ui_theme,
                     float bar_width,
                     std::shared_ptr<Splash::SpatialFont> font_ptr)
    : SpatialNode(reg, self), theme_(&ui_theme), font_(font_ptr) {
    name = "UIMenuBar";
    // menubar_height is geometry: the bar's extent and every item position
    // packed against it are a layout, resolved once. Material follows the live
    // theme every frame, in syncChromeToTheme below.
    size = glm::vec2(bar_width, theme_->menubar_height);

    // Bar Panel Background
    bar_panel_ = reg.emplace<Splash::SpatialPanel>(self, size, theme_->bg_dark);

    // Brand / Logo Label
    if (font_) {
        brand_label_ = makeUILabel(reg, bar_panel_, *theme_, "CLOUDS 3D", font_,
                                   TextRole::HEADER, TextTone::HIGHLIGHT, TextAlignment::LEFT);
        reg.transform(brand_label_).position = glm::vec3(-bar_width * 0.5f + 0.035f, 0.0f, 0.003f);

        // Seeded empty, not with a sample readout. A pre-filled string here is
        // a measurement nobody took: it renders as live telemetry from the
        // first frame and stays wrong until setTelemetryText() is called, if
        // it ever is. UILabel::collectRender skips empty text, so an unfed bar
        // shows nothing rather than showing a number it invented.
        telemetry_label_ = makeUILabel(reg, bar_panel_, *theme_, "", font_,
                                       TextRole::SMALL, TextTone::MUTED, TextAlignment::RIGHT);
        reg.transform(telemetry_label_).position = glm::vec3(bar_width * 0.5f - 0.035f, 0.0f, 0.003f);
    }

    current_x_offset_ = -bar_width * 0.5f + 0.22f;

    // A starting value, not a stored one; collectRender re-resolves every frame.
    syncChromeToTheme(reg);
}

void UIMenuBar::addMenu(Splash::Registry& reg, Splash::NodeId self,
                        const std::string& category_name, const std::vector<MenuItem>& items) {
    if (!font_) return;

    int idx = static_cast<int>(categories_.size());
    MenuCategory cat;
    cat.name = category_name;
    cat.items = items;
    categories_.push_back(cat);

    glm::vec2 txt_dim = font_->measureText(category_name, theme_->scale_body);
    float btn_w = txt_dim.x + 0.045f;
    float btn_h = theme_->menubar_height - 0.015f;

    const Splash::NodeId btn = makeUIButton(
        reg, bar_panel_, *theme_, category_name, glm::vec2(btn_w, btn_h), font_,
        [&reg, self, idx]() {
            if (UIMenuBar* bar = reg.as<UIMenuBar>(self)) bar->toggleDropdown(reg, idx);
        },
        ButtonVariant::GHOST);
    reg.transform(btn).position = glm::vec3(current_x_offset_ + btn_w * 0.5f, 0.0f, 0.003f);
    menu_buttons_.push_back(btn);

    // Create dropdown menu below
    const Splash::NodeId dropdown = reg.emplace<UIMenuDropdown>(
        self, *theme_, items, 0.32f, font_,
        std::function<void()>([&reg, self]() {
            if (UIMenuBar* bar = reg.as<UIMenuBar>(self)) bar->closeAllDropdowns(reg);
        }));
    reg.transform(dropdown).position = glm::vec3(current_x_offset_ + 0.16f, -0.15f, 0.010f);
    reg[dropdown].visible = false;
    dropdowns_.push_back(dropdown);

    current_x_offset_ += btn_w + 0.01f;
}

void UIMenuBar::toggleDropdown(Splash::Registry& reg, int index) {
    if (index < 0 || index >= static_cast<int>(dropdowns_.size())) return;

    if (open_dropdown_index_ == index) {
        closeAllDropdowns(reg);
    } else {
        closeAllDropdowns(reg);
        open_dropdown_index_ = index;
        reg[dropdowns_[index]].visible = true;
        if (UIButton* button = reg.as<UIButton>(menu_buttons_[index])) {
            button->setVariant(ButtonVariant::PRIMARY);
        }
    }
}

void UIMenuBar::closeAllDropdowns(Splash::Registry& reg) {
    for (size_t i = 0; i < dropdowns_.size(); ++i) {
        reg[dropdowns_[i]].visible = false;
        if (UIButton* button = reg.as<UIButton>(menu_buttons_[i])) {
            button->setVariant(ButtonVariant::GHOST);
        }
    }
    open_dropdown_index_ = -1;
}

void UIMenuBar::setTelemetryText(Splash::Registry& reg, const std::string& text) {
    if (UILabel* label = reg.as<UILabel>(telemetry_label_)) {
        label->setText(text);
    }
}

void UIMenuBar::syncChromeToTheme(Splash::Registry& reg) {
    Splash::SpatialPanel* panel = reg.as<Splash::SpatialPanel>(bar_panel_);
    if (!panel) return;
    panel->background_color = theme_->bg_dark;
    panel->corner_radius = theme_->radius_pill;
    panel->border_thickness = theme_->border_window;
    panel->border_color = theme_->window_border_inactive;
}

void UIMenuBar::collectRender(Splash::Registry& reg, Splash::NodeId,
                              Nova::SpatialMeshBuffer*,
                              std::vector<Splash::SpatialRenderCommand>&) {
    syncChromeToTheme(reg);
}

} // namespace Clouds::UI
