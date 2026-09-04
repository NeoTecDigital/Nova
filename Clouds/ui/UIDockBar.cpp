// Written by Richard Christopher, Copyright 2026 NeoTec Digital
#include "./UIDockBar.h"
#include "Splash/Primitives.h"

namespace Clouds::UI {

UIDockBar::UIDockBar(Splash::Registry& reg, Splash::NodeId self,
                     const UITheme& ui_theme,
                     float dock_width,
                     std::shared_ptr<Splash::SpatialFont> font_ptr)
    : SpatialNode(reg, self), theme_(&ui_theme), font_(font_ptr), total_width_(dock_width) {
    name = "UIDockBar";
    // dockbar_height is geometry: resolved once, because the item run is packed
    // against it. Material follows the live theme in syncChromeToTheme below.
    size = glm::vec2(dock_width, theme_->dockbar_height);

    dock_panel_ = reg.emplace<Splash::SpatialPanel>(self, size, theme_->bg_dark);

    current_x_ = -dock_width * 0.5f + 0.05f;

    // A starting value, not a stored one; collectRender re-resolves every frame.
    syncChromeToTheme(reg);
}

void UIDockBar::addItem(Splash::Registry& reg, const std::string& label,
                        std::function<void()> click_handler) {
    if (!font_) return;

    glm::vec2 txt_dim = font_->measureText(label, theme_->scale_body);
    float btn_w = txt_dim.x + 0.040f;
    float btn_h = theme_->dockbar_height - 0.016f;

    const Splash::NodeId btn = makeUIButton(reg, dock_panel_, *theme_, label,
                                            glm::vec2(btn_w, btn_h), font_,
                                            std::move(click_handler), ButtonVariant::SECONDARY);
    reg.transform(btn).position = glm::vec3(current_x_ + btn_w * 0.5f, 0.0f, 0.003f);
    buttons_.push_back(btn);

    current_x_ += btn_w + 0.015f;
}

void UIDockBar::setStatusText(Splash::Registry& reg, const std::string& status) {
    if (!font_) return;
    if (UILabel* label = reg.as<UILabel>(status_label_)) {
        label->setText(status);
        return;
    }
    status_label_ = makeUILabel(reg, dock_panel_, *theme_, status, font_,
                                TextRole::SMALL, TextTone::MUTED, TextAlignment::RIGHT);
    reg.transform(status_label_).position = glm::vec3(total_width_ * 0.5f - 0.04f, 0.0f, 0.003f);
}

void UIDockBar::syncChromeToTheme(Splash::Registry& reg) {
    Splash::SpatialPanel* panel = reg.as<Splash::SpatialPanel>(dock_panel_);
    if (!panel) return;
    panel->background_color = theme_->bg_dark;
    panel->corner_radius = theme_->radius_pill;
    panel->border_thickness = theme_->border_window;
    panel->border_color = theme_->window_border_active;
}

void UIDockBar::collectRender(Splash::Registry& reg, Splash::NodeId,
                              Nova::SpatialMeshBuffer*,
                              std::vector<Splash::SpatialRenderCommand>&) {
    syncChromeToTheme(reg);
}

} // namespace Clouds::UI
