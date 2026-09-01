#include "../../../include/Clouds/UI/UIMenuBar.h"
#include "../../../include/Clouds/Primitives.h"

namespace Clouds::UI {

// ---------------------------------------------------------------------------
// UIMenuDropdown Implementation
// ---------------------------------------------------------------------------
UIMenuDropdown::UIMenuDropdown(const std::vector<MenuItem>& items,
                               float width,
                               std::shared_ptr<NovaSpatial::SpatialFont> font_ptr,
                               std::function<void()> on_item_selected) {
    name = "UIMenuDropdown";
    float item_h = 0.045f;
    float total_h = items.size() * item_h + 0.02f;
    size = glm::vec2(width, total_h);

    panel_ = std::make_shared<SpatialPanel>(
        size,
        g_Theme.surface_elevated
    );
    panel_->corner_radius = g_Theme.radius_panel;
    panel_->border_thickness = g_Theme.border_panel;
    panel_->border_color = g_Theme.window_border_active;
    addChild(panel_);

    float start_y = total_h * 0.5f - 0.01f - item_h * 0.5f;

    for (size_t i = 0; i < items.size(); ++i) {
        const auto& it = items[i];
        std::string label_txt = it.shortcut.empty() ? it.title : (it.title + "   " + it.shortcut);
        auto act = it.action;

        auto btn = std::make_shared<UIButton>(
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
}

void UIMenuDropdown::collectRender(NovaSpatial::SpatialMeshBuffer* mesh_buf,
                                   std::vector<SpatialRenderCommand>& out_commands) {
    if (!visible) return;
    SpatialNode::collectRender(mesh_buf, out_commands);
}

// ---------------------------------------------------------------------------
// UIMenuBar Implementation
// ---------------------------------------------------------------------------
UIMenuBar::UIMenuBar(float bar_width,
                     std::shared_ptr<NovaSpatial::SpatialFont> font_ptr)
    : font_(font_ptr) {
    name = "UIMenuBar";
    size = glm::vec2(bar_width, g_Theme.menubar_height);

    // Bar Panel Background
    bar_panel_ = std::make_shared<SpatialPanel>(
        size,
        g_Theme.bg_dark
    );
    bar_panel_->corner_radius = g_Theme.radius_pill;
    bar_panel_->border_thickness = g_Theme.border_window;
    bar_panel_->border_color = g_Theme.window_border_inactive;
    addChild(bar_panel_);

    // Brand / Logo Label
    if (font_) {
        brand_label_ = std::make_shared<UILabel>(
            "CLOUDS 3D",
            font_,
            g_Theme.scale_header,
            g_Theme.text_highlight,
            TextAlignment::LEFT
        );
        brand_label_->transform.position = glm::vec3(-bar_width * 0.5f + 0.035f, 0.0f, 0.003f);
        bar_panel_->addChild(brand_label_);

        telemetry_label_ = std::make_shared<UILabel>(
            "FPS: 60 | Mode: Quaternionic Spatial | Sync: Delta",
            font_,
            g_Theme.scale_small,
            g_Theme.text_muted,
            TextAlignment::RIGHT
        );
        telemetry_label_->transform.position = glm::vec3(bar_width * 0.5f - 0.035f, 0.0f, 0.003f);
        bar_panel_->addChild(telemetry_label_);
    }

    current_x_offset_ = -bar_width * 0.5f + 0.22f;
}

void UIMenuBar::addMenu(const std::string& category_name, const std::vector<MenuItem>& items) {
    if (!font_) return;

    int idx = static_cast<int>(categories_.size());
    MenuCategory cat;
    cat.name = category_name;
    cat.items = items;
    categories_.push_back(cat);

    glm::vec2 txt_dim = font_->measureText(category_name, g_Theme.scale_body);
    float btn_w = txt_dim.x + 0.045f;
    float btn_h = g_Theme.menubar_height - 0.015f;

    auto btn = std::make_shared<UIButton>(
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

void UIMenuBar::collectRender(NovaSpatial::SpatialMeshBuffer* mesh_buf,
                              std::vector<SpatialRenderCommand>& out_commands) {
    if (!visible) return;
    SpatialNode::collectRender(mesh_buf, out_commands);
}

} // namespace Clouds::UI
