#include "./UIComponents.h"
#include "Splash/content/mesh_generators.h"
#include <algorithm>

namespace Clouds::UI {

// ---------------------------------------------------------------------------
// UILabel Implementation
// ---------------------------------------------------------------------------
UILabel::UILabel(const std::string& label_text,
                 std::shared_ptr<Splash::SpatialFont> font_ptr,
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
                   std::shared_ptr<Splash::SpatialFont> font_ptr,
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

    auto quad_mesh = Splash::SpatialMeshGenerator::createPlanarQuad(
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

} // namespace Clouds::UI
