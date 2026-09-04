// Written by Richard Christopher, Copyright 2026 NeoTec Digital
#include "./UIComponents.h"
#include "Splash/content/mesh_generators.h"
#include <algorithm>

namespace Clouds::UI {

// ---------------------------------------------------------------------------
// UILabel Implementation
// ---------------------------------------------------------------------------
UILabel::UILabel(const UITheme& ui_theme,
                 const std::string& label_text,
                 std::shared_ptr<Splash::SpatialFont> font_ptr,
                 TextRole text_role,
                 TextTone text_tone,
                 TextAlignment align)
    : text(label_text), role(text_role), tone(text_tone), alignment(align),
      font(font_ptr), theme_(&ui_theme) {
    name = "UILabel: " + text;
    if (font) {
        size = font->measureText(text, resolvedScale());
    }
}

float UILabel::resolvedScale() const {
    switch (role) {
        case TextRole::TITLE:  return theme_->scale_title;
        case TextRole::HEADER: return theme_->scale_header;
        case TextRole::BODY:   return theme_->scale_body;
        case TextRole::SMALL:  return theme_->scale_small;
        case TextRole::MONO:   return theme_->scale_mono;
    }
    return theme_->scale_body;
}

glm::vec4 UILabel::resolvedColor() const {
    switch (tone) {
        case TextTone::PRIMARY:   return theme_->text_primary;
        case TextTone::SECONDARY: return theme_->text_secondary;
        case TextTone::MUTED:     return theme_->text_muted;
        case TextTone::HIGHLIGHT: return theme_->text_highlight;
    }
    return theme_->text_primary;
}

void UILabel::setText(const std::string& new_text) {
    text = new_text;
    name = "UILabel: " + text;
    if (font) {
        size = font->measureText(text, resolvedScale());
    }
}

glm::vec2 UILabel::getDimensions() const {
    if (!font) return glm::vec2(0.0f);
    return font->measureText(text, resolvedScale());
}

void UILabel::collectRender(Nova::SpatialMeshBuffer* mesh_buf,
                            std::vector<Splash::SpatialRenderCommand>& out_commands) {
    if (!visible || text.empty() || !font) return;

    const float scale = resolvedScale();

    // Re-measured, not remembered: the scale is the theme's and the theme can
    // change under a label that is already on screen. The extent the pointer is
    // tested against has to be the extent that was just drawn.
    const glm::vec2 dims = font->measureText(text, scale);
    size = dims;

    Nova::Math::QuatTransform world_xf = getWorldTransform();

    // Offset world position based on alignment
    if (alignment == TextAlignment::LEFT) {
        world_xf.position += world_xf.orientation * glm::vec3(dims.x * 0.5f, 0.0f, 0.0f);
    } else if (alignment == TextAlignment::RIGHT) {
        world_xf.position -= world_xf.orientation * glm::vec3(dims.x * 0.5f, 0.0f, 0.0f);
    }

    glm::mat4 model = world_xf.toMatrix();
    auto text_mesh = font->createTextMesh(text, scale, resolvedColor(), true);

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
UIButton::UIButton(const UITheme& ui_theme,
                   const std::string& btn_label,
                   const glm::vec2& btn_size,
                   std::shared_ptr<Splash::SpatialFont> font_ptr,
                   std::function<void()> click_handler,
                   ButtonVariant btn_variant)
    : label(btn_label), variant(btn_variant), on_click(click_handler),
      theme_(&ui_theme), font_(font_ptr) {
    size = btn_size;
    name = "UIButton: " + label;

    // The caption sits in front of the box, so a hit on the glyph run lands on
    // the label, not the button. Capturing the subtree hands those hits back:
    // clicking the middle of a button is clicking the button.
    captures_subtree_input = true;

    if (font_) {
        label_node_ = std::make_shared<UILabel>(
            *theme_,
            label,
            font_,
            TextRole::BODY,
            TextTone::PRIMARY,
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
        case ButtonVariant::PRIMARY:   return theme_->primary;
        case ButtonVariant::SECONDARY: return theme_->secondary;
        case ButtonVariant::SUCCESS:   return theme_->accent_success;
        case ButtonVariant::DANGER:    return theme_->accent_danger;
        case ButtonVariant::GHOST:     return glm::vec4(0.12f, 0.16f, 0.24f, 0.30f);
    }
    return theme_->primary;
}

glm::vec4 UIButton::getHoverColor() const {
    switch (variant) {
        case ButtonVariant::PRIMARY:   return theme_->primary_hover;
        case ButtonVariant::SECONDARY: return theme_->secondary_hover;
        case ButtonVariant::SUCCESS:   return glm::vec4(0.24f, 0.85f, 0.50f, 1.0f);
        case ButtonVariant::DANGER:    return glm::vec4(0.95f, 0.35f, 0.40f, 1.0f);
        case ButtonVariant::GHOST:     return glm::vec4(0.20f, 0.28f, 0.42f, 0.70f);
    }
    return theme_->primary_hover;
}

glm::vec4 UIButton::getActiveColor() const {
    return theme_->primary_active;
}

glm::vec4 UIButton::resolvedFillColor() const {
    if (!enabled) return glm::vec4(0.15f, 0.18f, 0.24f, 0.50f);
    if (is_pressed) return getActiveColor();
    return is_hovered ? getHoverColor() : getBaseColor();
}

float UIButton::resolvedCornerRadius() const {
    return theme_->radius_button;
}

float UIButton::resolvedBorderThickness() const {
    return theme_->border_button;
}

// Deliberately does not chain to SpatialNode::onRayButton. The base would
// write is_pressed unconditionally, and the click latch below needs to read the
// value the press left before the release overwrites it; the base also takes
// is_focused on press, which a disabled button must not do. Hover and leave are
// not overridden at all -- the base owns is_hovered and clears is_pressed when
// the pointer leaves, which is exactly what a held button that is walked away
// from should do.
void UIButton::onRayButton(const Nova::Math::RayHit&, uint32_t button, bool pressed) {
    if (!enabled) return;

    if (button == 1) { // Left click
        if (pressed) {
            is_pressed = true;
        } else if (is_pressed) {
            is_pressed = false;
            if (on_click) {
                on_click();
            }
        }
    }
}

void UIButton::collectRender(Nova::SpatialMeshBuffer* mesh_buf,
                             std::vector<Splash::SpatialRenderCommand>& out_commands) {
    if (!visible) return;

    Nova::Math::QuatTransform world_xf = getWorldTransform();
    glm::mat4 model = world_xf.toMatrix();

    auto quad_mesh = Splash::SpatialMeshGenerator::createPlanarQuad(
        size,
        resolvedFillColor(),
        resolvedBorderThickness(),
        resolvedCornerRadius(),
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
