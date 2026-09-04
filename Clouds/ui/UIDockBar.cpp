// Written by Richard Christopher, Copyright 2026 NeoTec Digital
#include "./UIDockBar.h"
#include "Splash/Primitives.h"

namespace Clouds::UI {

UIDockBar::UIDockBar(const UITheme& ui_theme,
                     float dock_width,
                     std::shared_ptr<Splash::SpatialFont> font_ptr)
    : theme_(&ui_theme), font_(font_ptr), total_width_(dock_width) {
    name = "UIDockBar";
    // dockbar_height is geometry: resolved once, because the item run is packed
    // against it. Material follows the live theme in syncChromeToTheme below.
    size = glm::vec2(dock_width, theme_->dockbar_height);

    dock_panel_ = std::make_shared<Splash::SpatialPanel>(
        size,
        theme_->bg_dark
    );
    addChild(dock_panel_);

    current_x_ = -dock_width * 0.5f + 0.05f;

    // A starting value, not a stored one; collectRender re-resolves every frame.
    syncChromeToTheme();
}

void UIDockBar::addItem(const std::string& label, std::function<void()> click_handler) {
    if (!font_) return;

    glm::vec2 txt_dim = font_->measureText(label, theme_->scale_body);
    float btn_w = txt_dim.x + 0.040f;
    float btn_h = theme_->dockbar_height - 0.016f;

    auto btn = std::make_shared<UIButton>(
        *theme_,
        label,
        glm::vec2(btn_w, btn_h),
        font_,
        click_handler,
        ButtonVariant::SECONDARY
    );
    btn->transform.position = glm::vec3(current_x_ + btn_w * 0.5f, 0.0f, 0.003f);
    dock_panel_->addChild(btn);
    buttons_.push_back(btn);

    current_x_ += btn_w + 0.015f;
}

void UIDockBar::setStatusText(const std::string& status) {
    if (!font_) return;
    if (!status_label_) {
        status_label_ = std::make_shared<UILabel>(
            *theme_,
            status,
            font_,
            TextRole::SMALL,
            TextTone::MUTED,
            TextAlignment::RIGHT
        );
        status_label_->transform.position = glm::vec3(total_width_ * 0.5f - 0.04f, 0.0f, 0.003f);
        dock_panel_->addChild(status_label_);
    } else {
        status_label_->setText(status);
    }
}

void UIDockBar::syncChromeToTheme() {
    if (!dock_panel_) return;
    dock_panel_->background_color = theme_->bg_dark;
    dock_panel_->corner_radius = theme_->radius_pill;
    dock_panel_->border_thickness = theme_->border_window;
    dock_panel_->border_color = theme_->window_border_active;
}

void UIDockBar::collectRender(Nova::SpatialMeshBuffer* mesh_buf,
                              std::vector<Splash::SpatialRenderCommand>& out_commands) {
    if (!visible) return;
    syncChromeToTheme();
    Splash::SpatialNode::collectRender(mesh_buf, out_commands);
}

} // namespace Clouds::UI
