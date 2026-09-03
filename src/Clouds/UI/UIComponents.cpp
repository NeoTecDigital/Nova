#include "include/Clouds/UI/UIComponents.h"
#include <algorithm>

namespace Clouds::UI {

// ---------------------------------------------------------------------------
// UILabel Implementation
// ---------------------------------------------------------------------------
UILabel::UILabel(const std::string& label_text,
                 std::shared_ptr<Nova::SpatialFont> font_ptr,
                 float scale,
                 const glm::vec4& text_color,
                 TextAlignment align)
    : text(label_text), color(text_color), font_scale(scale), alignment(align), font(font_ptr) {
    name = "UILabel: " + text;
    if (font) {
        size = font->measureText(text, font_scale);
    }
}

void UILabel::setText(const std::string& new_text) {
    text = new_text;
    name = "UILabel: " + text;
    if (font) {
        size = font->measureText(text, font_scale);
    }
}

glm::vec2 UILabel::getDimensions() const {
    if (!font) return glm::vec2(0.0f);
    return font->measureText(text, font_scale);
}

void UILabel::collectRender(Nova::SpatialMeshBuffer* mesh_buf,
                            std::vector<Splash::SpatialRenderCommand>& out_commands) {
    if (!visible || text.empty() || !font) return;

    Nova::Math::QuatTransform world_xf = getWorldTransform();
    
    // Offset world position based on alignment
    if (alignment == TextAlignment::LEFT) {
        glm::vec2 dims = font->measureText(text, font_scale);
        world_xf.position += world_xf.orientation * glm::vec3(dims.x * 0.5f, 0.0f, 0.0f);
    } else if (alignment == TextAlignment::RIGHT) {
        glm::vec2 dims = font->measureText(text, font_scale);
        world_xf.position -= world_xf.orientation * glm::vec3(dims.x * 0.5f, 0.0f, 0.0f);
    }

    glm::mat4 model = world_xf.toMatrix();
    auto text_mesh = font->createTextMesh(text, font_scale, color, true);

    uint32_t first_idx, idx_count;
    mesh_buf->append(text_mesh, first_idx, idx_count);

    out_commands.push_back({
        .model = model,
        .surface_dim = glm::vec4(1.0f),
        .texture = font->getAtlasTexture(),
        .first_index = first_idx,
        .index_count = idx_count
    });

    Splash::SpatialNode::collectRender(mesh_buf, out_commands);
}

// ---------------------------------------------------------------------------
// UIButton Implementation
// ---------------------------------------------------------------------------
UIButton::UIButton(const std::string& btn_label,
                   const glm::vec2& btn_size,
                   std::shared_ptr<Nova::SpatialFont> font_ptr,
                   std::function<void()> click_handler,
                   ButtonVariant btn_variant)
    : label(btn_label), variant(btn_variant), on_click(click_handler), font_(font_ptr) {
    size = btn_size;
    name = "UIButton: " + label;

    // The caption sits in front of the box, so a hit on the glyph run lands on
    // the label, not the button. Capturing the subtree hands those hits back:
    // clicking the middle of a button is clicking the button.
    captures_subtree_input = true;

    if (font_) {
        label_node_ = std::make_shared<UILabel>(
            label,
            font_,
            g_Theme.scale_body,
            g_Theme.text_primary,
            TextAlignment::CENTER
        );
        label_node_->transform.position = glm::vec3(0.0f, 0.0f, 0.002f);
        label_node_->claims_pointer_input = false;
        addChild(label_node_);
    }
}

void UIButton::setLabel(const std::string& new_label) {
    label = new_label;
    name = "UIButton: " + label;
    if (label_node_) {
        label_node_->setText(label);
    }
}

void UIButton::setVariant(ButtonVariant new_variant) {
    variant = new_variant;
}

glm::vec4 UIButton::getBaseColor() const {
    switch (variant) {
        case ButtonVariant::PRIMARY:   return g_Theme.primary;
        case ButtonVariant::SECONDARY: return g_Theme.secondary;
        case ButtonVariant::SUCCESS:   return g_Theme.accent_success;
        case ButtonVariant::DANGER:    return g_Theme.accent_danger;
        case ButtonVariant::GHOST:     return glm::vec4(0.12f, 0.16f, 0.24f, 0.30f);
    }
    return g_Theme.primary;
}

glm::vec4 UIButton::getHoverColor() const {
    switch (variant) {
        case ButtonVariant::PRIMARY:   return g_Theme.primary_hover;
        case ButtonVariant::SECONDARY: return g_Theme.secondary_hover;
        case ButtonVariant::SUCCESS:   return glm::vec4(0.24f, 0.85f, 0.50f, 1.0f);
        case ButtonVariant::DANGER:    return glm::vec4(0.95f, 0.35f, 0.40f, 1.0f);
        case ButtonVariant::GHOST:     return glm::vec4(0.20f, 0.28f, 0.42f, 0.70f);
    }
    return g_Theme.primary_hover;
}

glm::vec4 UIButton::getActiveColor() const {
    return g_Theme.primary_active;
}

void UIButton::onRayEnter(const Nova::Math::RayHit&) {
    hover_factor_ = 1.0f;
}

void UIButton::onRayLeave() {
    hover_factor_ = 0.0f;
    is_pressed_ = false;
}

void UIButton::onRayButton(const Nova::Math::RayHit&, uint32_t button, bool pressed) {
    if (!enabled) return;

    if (button == 1) { // Left click
        if (pressed) {
            is_pressed_ = true;
        } else if (is_pressed_) {
            is_pressed_ = false;
            if (on_click) {
                on_click();
            }
        }
    }
}

void UIButton::collectRender(Nova::SpatialMeshBuffer* mesh_buf,
                             std::vector<Splash::SpatialRenderCommand>& out_commands) {
    if (!visible) return;

    glm::vec4 current_color = is_pressed_ ? getActiveColor() :
                              (hover_factor_ > 0.5f ? getHoverColor() : getBaseColor());
    
    if (!enabled) {
        current_color = glm::vec4(0.15f, 0.18f, 0.24f, 0.50f);
    }

    Nova::Math::QuatTransform world_xf = getWorldTransform();
    glm::mat4 model = world_xf.toMatrix();

    auto quad_mesh = Nova::SpatialMeshGenerator::createPlanarQuad(
        size,
        current_color,
        border_thickness,
        corner_radius,
        0.0f
    );

    uint32_t first_idx, idx_count;
    mesh_buf->append(quad_mesh, first_idx, idx_count);

    out_commands.push_back({
        .model = model,
        .surface_dim = glm::vec4(size.x, size.y, size.x / std::max(size.y, 0.001f), 0.0f),
        .texture = nullptr,
        .first_index = first_idx,
        .index_count = idx_count
    });

    Splash::SpatialNode::collectRender(mesh_buf, out_commands);
}

// ---------------------------------------------------------------------------
// UIBadge Implementation
// ---------------------------------------------------------------------------
UIBadge::UIBadge(const std::string& badge_text,
                 const glm::vec4& color,
                 std::shared_ptr<Nova::SpatialFont> font_ptr)
    : text(badge_text), bg_color(color), font_(font_ptr) {
    name = "UIBadge: " + text;
    
    glm::vec2 text_dim = font_ ? font_->measureText(text, g_Theme.scale_small) : glm::vec2(0.1f, 0.04f);
    size = glm::vec2(text_dim.x + 0.035f, 0.045f);

    if (font_) {
        label_node_ = std::make_shared<UILabel>(
            text,
            font_,
            g_Theme.scale_small,
            text_color,
            TextAlignment::CENTER
        );
        label_node_->transform.position = glm::vec3(0.0f, 0.0f, 0.002f);
        addChild(label_node_);
    }
}

void UIBadge::setText(const std::string& new_text, const glm::vec4& color) {
    text = new_text;
    bg_color = color;
    name = "UIBadge: " + text;
    if (label_node_) {
        label_node_->setText(text);
    }
    if (font_) {
        glm::vec2 text_dim = font_->measureText(text, g_Theme.scale_small);
        size = glm::vec2(text_dim.x + 0.035f, 0.045f);
    }
}

void UIBadge::collectRender(Nova::SpatialMeshBuffer* mesh_buf,
                            std::vector<Splash::SpatialRenderCommand>& out_commands) {
    if (!visible) return;

    Nova::Math::QuatTransform world_xf = getWorldTransform();
    glm::mat4 model = world_xf.toMatrix();

    auto quad_mesh = Nova::SpatialMeshGenerator::createPlanarQuad(
        size,
        bg_color,
        g_Theme.border_button,
        0.008f,
        0.0f
    );

    uint32_t first_idx, idx_count;
    mesh_buf->append(quad_mesh, first_idx, idx_count);

    out_commands.push_back({
        .model = model,
        .surface_dim = glm::vec4(size.x, size.y, size.x / std::max(size.y, 0.001f), 0.0f),
        .texture = nullptr,
        .first_index = first_idx,
        .index_count = idx_count
    });

    Splash::SpatialNode::collectRender(mesh_buf, out_commands);
}

// ---------------------------------------------------------------------------
// UIProgressBar Implementation
// ---------------------------------------------------------------------------
UIProgressBar::UIProgressBar(const glm::vec2& bar_size,
                             float initial_progress,
                             const glm::vec4& bar_color)
    : progress(std::clamp(initial_progress, 0.0f, 1.0f)), fill_color(bar_color) {
    size = bar_size;
    name = "UIProgressBar";
}

void UIProgressBar::setProgress(float val) {
    progress = std::clamp(val, 0.0f, 1.0f);
}

void UIProgressBar::collectRender(Nova::SpatialMeshBuffer* mesh_buf,
                                  std::vector<Splash::SpatialRenderCommand>& out_commands) {
    if (!visible) return;

    Nova::Math::QuatTransform world_xf = getWorldTransform();
    
    // 1. Background trough
    auto bg_mesh = Nova::SpatialMeshGenerator::createPlanarQuad(
        size,
        bg_color,
        0.001f,
        0.006f,
        0.0f
    );
    uint32_t bg_first, bg_count;
    mesh_buf->append(bg_mesh, bg_first, bg_count);

    out_commands.push_back({
        .model = world_xf.toMatrix(),
        .surface_dim = glm::vec4(size.x, size.y, size.x / std::max(size.y, 0.001f), 0.0f),
        .texture = nullptr,
        .first_index = bg_first,
        .index_count = bg_count
    });

    // 2. Active fill
    if (progress > 0.01f) {
        float fill_w = size.x * progress;
        glm::vec2 fill_size(fill_w, size.y - 0.004f);
        
        Nova::Math::QuatTransform fill_xf = world_xf;
        float offset_x = (fill_w - size.x) * 0.5f;
        fill_xf.position += fill_xf.orientation * glm::vec3(offset_x, 0.0f, 0.001f);

        auto fill_mesh = Nova::SpatialMeshGenerator::createPlanarQuad(
            fill_size,
            fill_color,
            0.0f,
            0.004f,
            0.0f
        );
        uint32_t fill_first, fill_count;
        mesh_buf->append(fill_mesh, fill_first, fill_count);

        out_commands.push_back({
            .model = fill_xf.toMatrix(),
            .surface_dim = glm::vec4(fill_size.x, fill_size.y, fill_size.x / std::max(fill_size.y, 0.001f), 0.0f),
            .texture = nullptr,
            .first_index = fill_first,
            .index_count = fill_count
        });
    }

    Splash::SpatialNode::collectRender(mesh_buf, out_commands);
}

// ---------------------------------------------------------------------------
// UITabControl Implementation
// ---------------------------------------------------------------------------
UITabControl::UITabControl(const std::vector<std::string>& tab_names,
                           float width,
                           std::shared_ptr<Nova::SpatialFont> font_ptr,
                           std::function<void(int)> tab_callback)
    : on_tab_changed(tab_callback), total_width_(width) {
    name = "UITabControl";
    size = glm::vec2(width, 0.055f);

    if (tab_names.empty()) return;

    float tab_w = (width - 0.01f * (tab_names.size() - 1)) / tab_names.size();
    float start_x = -width * 0.5f + tab_w * 0.5f;

    for (size_t i = 0; i < tab_names.size(); ++i) {
        int idx = static_cast<int>(i);
        auto btn = std::make_shared<UIButton>(
            tab_names[i],
            glm::vec2(tab_w, 0.045f),
            font_ptr,
            [this, idx]() {
                setActiveTab(idx);
            },
            (idx == 0) ? ButtonVariant::PRIMARY : ButtonVariant::GHOST
        );
        btn->transform.position = glm::vec3(start_x + i * (tab_w + 0.01f), 0.0f, 0.002f);
        addChild(btn);
        tab_buttons_.push_back(btn);
    }
}

void UITabControl::setActiveTab(int index) {
    if (index < 0 || index >= static_cast<int>(tab_buttons_.size())) return;
    active_tab_index = index;

    for (size_t i = 0; i < tab_buttons_.size(); ++i) {
        tab_buttons_[i]->setVariant((static_cast<int>(i) == active_tab_index) ?
            ButtonVariant::PRIMARY : ButtonVariant::GHOST);
    }

    if (on_tab_changed) {
        on_tab_changed(active_tab_index);
    }
}

void UITabControl::collectRender(Nova::SpatialMeshBuffer* mesh_buf,
                                 std::vector<Splash::SpatialRenderCommand>& out_commands) {
    if (!visible) return;
    Splash::SpatialNode::collectRender(mesh_buf, out_commands);
}

} // namespace Clouds::UI
