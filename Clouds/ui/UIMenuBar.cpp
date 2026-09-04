// Written by Richard Christopher, Copyright 2026 NeoTec Digital
#include "./UIMenuBar.h"
#include "Splash/Primitives.h"

namespace Clouds::UI {

// ---------------------------------------------------------------------------
// UIMenuDropdown Implementation
// ---------------------------------------------------------------------------
UIMenuDropdown::UIMenuDropdown(const UITheme& ui_theme,
                               const std::vector<MenuItem>& items,
                               float width,
                               std::shared_ptr<Splash::SpatialFont> font_ptr,
                               std::function<void()> on_item_selected)
    : theme_(&ui_theme) {
    name = "UIMenuDropdown";
    float item_h = 0.045f;
    float total_h = items.size() * item_h + 0.02f;
    size = glm::vec2(width, total_h);

    panel_ = std::make_shared<Splash::SpatialPanel>(
        size,
        theme_->surface_elevated
    );
    addChild(panel_);

    float start_y = total_h * 0.5f - 0.01f - item_h * 0.5f;

    for (size_t i = 0; i < items.size(); ++i) {
        const auto& it = items[i];
        std::string label_txt = it.shortcut.empty() ? it.title : (it.title + "   " + it.shortcut);
        auto act = it.action;

        auto btn = std::make_shared<UIButton>(
            *theme_,
            label_txt,
            glm::vec2(width - 0.02f, item_h - 0.005f),
            font_ptr,
            [act, on_item_selected]() {
                if (act) act();
                if (on_item_selected) on_item_selected();
            },
            ButtonVariant::GHOST
        );
        btn->transform.position = glm::vec3(0.0f, start_y - i * item_h, 0.003f);
        panel_->addChild(btn);
        item_buttons_.push_back(btn);
    }

    // A starting value, not a stored one; collectRender re-resolves every frame.
    syncChromeToTheme();
}

void UIMenuDropdown::syncChromeToTheme() {
    if (!panel_) return;
    panel_->background_color = theme_->surface_elevated;
    panel_->corner_radius = theme_->radius_panel;
    panel_->border_thickness = theme_->border_panel;
    panel_->border_color = theme_->window_border_active;
}

void UIMenuDropdown::collectRender(Nova::SpatialMeshBuffer* mesh_buf,
                                   std::vector<Splash::SpatialRenderCommand>& out_commands) {
    if (!visible) return;
    syncChromeToTheme();
    Splash::SpatialNode::collectRender(mesh_buf, out_commands);
}

// ---------------------------------------------------------------------------
// UIMenuBar Implementation
// ---------------------------------------------------------------------------
UIMenuBar::UIMenuBar(const UITheme& ui_theme,
                     float bar_width,
                     std::shared_ptr<Splash::SpatialFont> font_ptr)
    : theme_(&ui_theme), font_(font_ptr) {
    name = "UIMenuBar";
    // menubar_height is geometry: the bar's extent and every item position
    // packed against it are a layout, resolved once. Material follows the live
    // theme every frame, in syncChromeToTheme below.
    size = glm::vec2(bar_width, theme_->menubar_height);

    // Bar Panel Background
    bar_panel_ = std::make_shared<Splash::SpatialPanel>(
        size,
        theme_->bg_dark
    );
    addChild(bar_panel_);

    // Brand / Logo Label
    if (font_) {
        brand_label_ = std::make_shared<UILabel>(
            *theme_,
            "CLOUDS 3D",
            font_,
            TextRole::HEADER,
            TextTone::HIGHLIGHT,
            TextAlignment::LEFT
        );
        brand_label_->transform.position = glm::vec3(-bar_width * 0.5f + 0.035f, 0.0f, 0.003f);
        bar_panel_->addChild(brand_label_);

        // Seeded empty, not with a sample readout. A pre-filled string here is
        // a measurement nobody took: it renders as live telemetry from the
        // first frame and stays wrong until setTelemetryText() is called, if
        // it ever is. UILabel::collectRender skips empty text, so an unfed bar
        // shows nothing rather than showing a number it invented.
        telemetry_label_ = std::make_shared<UILabel>(
            *theme_,
            "",
            font_,
            TextRole::SMALL,
            TextTone::MUTED,
            TextAlignment::RIGHT
        );
        telemetry_label_->transform.position = glm::vec3(bar_width * 0.5f - 0.035f, 0.0f, 0.003f);
        bar_panel_->addChild(telemetry_label_);
    }

    current_x_offset_ = -bar_width * 0.5f + 0.22f;

    // A starting value, not a stored one; collectRender re-resolves every frame.
    syncChromeToTheme();
}

void UIMenuBar::addMenu(const std::string& category_name, const std::vector<MenuItem>& items) {
    if (!font_) return;

    int idx = static_cast<int>(categories_.size());
    MenuCategory cat;
    cat.name = category_name;
    cat.items = items;
    categories_.push_back(cat);

    glm::vec2 txt_dim = font_->measureText(category_name, theme_->scale_body);
    float btn_w = txt_dim.x + 0.045f;
    float btn_h = theme_->menubar_height - 0.015f;

    auto btn = std::make_shared<UIButton>(
        *theme_,
        category_name,
        glm::vec2(btn_w, btn_h),
        font_,
        [this, idx]() {
            toggleDropdown(idx);
        },
        ButtonVariant::GHOST
    );
    btn->transform.position = glm::vec3(current_x_offset_ + btn_w * 0.5f, 0.0f, 0.003f);
    bar_panel_->addChild(btn);
    menu_buttons_.push_back(btn);

    // Create dropdown menu below
    auto dropdown = std::make_shared<UIMenuDropdown>(
        *theme_,
        items,
        0.32f,
        font_,
        [this]() {
            closeAllDropdowns();
        }
    );
    dropdown->transform.position = glm::vec3(current_x_offset_ + 0.16f, -0.15f, 0.010f);
    dropdown->visible = false;
    addChild(dropdown);
    dropdowns_.push_back(dropdown);

    current_x_offset_ += btn_w + 0.01f;
}

void UIMenuBar::toggleDropdown(int index) {
    if (index < 0 || index >= static_cast<int>(dropdowns_.size())) return;

    if (open_dropdown_index_ == index) {
        closeAllDropdowns();
    } else {
        closeAllDropdowns();
        open_dropdown_index_ = index;
        dropdowns_[index]->visible = true;
        menu_buttons_[index]->setVariant(ButtonVariant::PRIMARY);
    }
}

void UIMenuBar::closeAllDropdowns() {
    for (size_t i = 0; i < dropdowns_.size(); ++i) {
        dropdowns_[i]->visible = false;
        menu_buttons_[i]->setVariant(ButtonVariant::GHOST);
    }
    open_dropdown_index_ = -1;
}

void UIMenuBar::setTelemetryText(const std::string& text) {
    if (telemetry_label_) {
        telemetry_label_->setText(text);
    }
}

void UIMenuBar::syncChromeToTheme() {
    if (!bar_panel_) return;
    bar_panel_->background_color = theme_->bg_dark;
    bar_panel_->corner_radius = theme_->radius_pill;
    bar_panel_->border_thickness = theme_->border_window;
    bar_panel_->border_color = theme_->window_border_inactive;
}

void UIMenuBar::collectRender(Nova::SpatialMeshBuffer* mesh_buf,
                              std::vector<Splash::SpatialRenderCommand>& out_commands) {
    if (!visible) return;
    syncChromeToTheme();
    Splash::SpatialNode::collectRender(mesh_buf, out_commands);
}

} // namespace Clouds::UI
