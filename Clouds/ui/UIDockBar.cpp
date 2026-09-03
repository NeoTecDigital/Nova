#include "./UIDockBar.h"
#include "Splash/Primitives.h"

namespace Clouds::UI {

UIDockBar::UIDockBar(float dock_width,
                     std::shared_ptr<Splash::SpatialFont> font_ptr)
    : font_(font_ptr), total_width_(dock_width) {
    name = "UIDockBar";
    size = glm::vec2(dock_width, g_Theme.dockbar_height);

    dock_panel_ = std::make_shared<Splash::SpatialPanel>(
        size,
        g_Theme.bg_dark
    );
    dock_panel_->corner_radius = g_Theme.radius_pill;
    dock_panel_->border_thickness = g_Theme.border_window;
    dock_panel_->border_color = g_Theme.window_border_active;
    addChild(dock_panel_);

    current_x_ = -dock_width * 0.5f + 0.05f;
}

void UIDockBar::addItem(const std::string& label, std::function<void()> click_handler) {
    if (!font_) return;

    glm::vec2 txt_dim = font_->measureText(label, g_Theme.scale_body);
    float btn_w = txt_dim.x + 0.040f;
    float btn_h = g_Theme.dockbar_height - 0.016f;

    auto btn = std::make_shared<UIButton>(
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
            status,
            font_,
            g_Theme.scale_small,
            g_Theme.text_muted,
            TextAlignment::RIGHT
        );
        status_label_->transform.position = glm::vec3(total_width_ * 0.5f - 0.04f, 0.0f, 0.003f);
        dock_panel_->addChild(status_label_);
    } else {
        status_label_->setText(status);
    }
}

void UIDockBar::collectRender(Nova::SpatialMeshBuffer* mesh_buf,
                              std::vector<Splash::SpatialRenderCommand>& out_commands) {
    if (!visible) return;
    Splash::SpatialNode::collectRender(mesh_buf, out_commands);
}

} // namespace Clouds::UI
