// Written by Richard Christopher, Copyright 2026 NeoTec Digital
#pragma once

#include "Splash/Registry.h"
#include "Splash/SpatialNode.h"
#include "Splash/content/spatial_font.h"
#include "./UITheme.h"
#include <functional>
#include <string>
#include <vector>

namespace Clouds::UI {

enum class TextAlignment {
    LEFT,
    CENTER,
    RIGHT
};

// A label carries a ROLE and a TONE, not a resolved scale and colour. The two
// enumerations are exactly the typography scales and text colours UITheme
// defines; naming one is naming a slot in the live theme, which is what lets a
// theme edit reach a label that already exists. A label that stored the float
// and the vec4 would be storing a copy of the theme from the instant it was
// built -- the defect this whole seam removes.
enum class TextRole {
    TITLE,
    HEADER,
    BODY,
    SMALL,
    MONO
};

enum class TextTone {
    PRIMARY,
    SECONDARY,
    MUTED,
    HIGHLIGHT
};

enum class ButtonVariant {
    PRIMARY,
    SECONDARY,
    SUCCESS,
    DANGER,
    GHOST
};

/**
 * UILabel - High Precision Text Label with Alignment & Dynamic Metrics
 *
 * Holds a non-owning pointer to the theme it was constructed with; see the
 * seam contract at the foot of UITheme.h for the lifetime rule.
 */
class UILabel : public Splash::SpatialNode {
public:
    std::string text;
    TextRole role = TextRole::BODY;
    TextTone tone = TextTone::PRIMARY;
    TextAlignment alignment = TextAlignment::LEFT;
    std::shared_ptr<Splash::SpatialFont> font;

    UILabel(Splash::Registry& reg, Splash::NodeId self,
            const UITheme& ui_theme,
            const std::string& label_text,
            std::shared_ptr<Splash::SpatialFont> font_ptr,
            TextRole text_role = TextRole::BODY,
            TextTone text_tone = TextTone::PRIMARY,
            TextAlignment align = TextAlignment::LEFT);

    void setText(const std::string& new_text);
    glm::vec2 getDimensions() const;

    // Read from the live theme on every call. collectRender uses these and
    // nothing else, so what a caller reads here is what the frame draws.
    glm::vec4 resolvedColor() const;
    float resolvedScale() const;

    const UITheme& theme() const { return *theme_; }

    void collectRender(Splash::Registry& reg, Splash::NodeId self,
                       Nova::SpatialMeshBuffer* mesh_buf,
                       std::vector<Splash::SpatialRenderCommand>& out_commands) override;

private:
    const UITheme* theme_;
};

/**
 * UIButton - Styled Interactive Button with Hover/Press Transitions
 *
 * Hover and press are SpatialNode::is_hovered and SpatialNode::is_pressed. A
 * derived hover float and a derived press bool used to sit beside them, second
 * copies of base state written by handlers that never called the base, so the
 * two spellings could disagree in silence -- the same defect already removed
 * from UIWindow::is_focused.
 */
class UIButton : public Splash::SpatialNode {
public:
    std::string label;
    ButtonVariant variant = ButtonVariant::PRIMARY;
    std::function<void()> on_click;

    bool enabled = true;

    UIButton(Splash::Registry& reg, Splash::NodeId self,
             const UITheme& ui_theme,
             const std::string& btn_label,
             const glm::vec2& btn_size,
             std::shared_ptr<Splash::SpatialFont> font_ptr,
             std::function<void()> click_handler = nullptr,
             ButtonVariant btn_variant = ButtonVariant::PRIMARY);

    void setLabel(Splash::Registry& reg, const std::string& new_label);
    void setVariant(ButtonVariant new_variant);

    // Resolved from the live theme and the live interaction state on every
    // call; collectRender draws with exactly these three and nothing else.
    glm::vec4 resolvedFillColor() const;
    float resolvedCornerRadius() const;
    float resolvedBorderThickness() const;

    const UITheme& theme() const { return *theme_; }

    // onRayEnter/onRayLeave are NOT overridden: SpatialNode already maintains
    // is_hovered and clears is_pressed on leave, and an override that only
    // forwarded to the base is where the two shadow members used to live.
    void onRayButton(Splash::Registry& reg, Splash::NodeId self, const Nova::Math::RayHit& hit,
                     uint32_t button, bool pressed) override;

    void collectRender(Splash::Registry& reg, Splash::NodeId self,
                       Nova::SpatialMeshBuffer* mesh_buf,
                       std::vector<Splash::SpatialRenderCommand>& out_commands) override;

private:
    const UITheme* theme_;
    std::shared_ptr<Splash::SpatialFont> font_;
    Splash::NodeId label_node_;

    glm::vec4 getBaseColor() const;
    glm::vec4 getHoverColor() const;
    glm::vec4 getActiveColor() const;
};

// --- Builders --------------------------------------------------------------
//
// The one spelling every caller uses. B2.c replaces the classes above with
// kinds and rewrites these bodies; nothing that calls them changes.

inline Splash::NodeId makeUILabel(Splash::Registry& reg, Splash::NodeId parent,
                                  const UITheme& theme, const std::string& text,
                                  std::shared_ptr<Splash::SpatialFont> font,
                                  TextRole role = TextRole::BODY,
                                  TextTone tone = TextTone::PRIMARY,
                                  TextAlignment align = TextAlignment::LEFT) {
    return reg.emplace<UILabel>(parent, theme, text, std::move(font), role, tone, align);
}

inline Splash::NodeId makeUIButton(Splash::Registry& reg, Splash::NodeId parent,
                                   const UITheme& theme, const std::string& label,
                                   const glm::vec2& size,
                                   std::shared_ptr<Splash::SpatialFont> font,
                                   std::function<void()> on_click = nullptr,
                                   ButtonVariant variant = ButtonVariant::PRIMARY) {
    return reg.emplace<UIButton>(parent, theme, label, size, std::move(font),
                                 std::move(on_click), variant);
}

} // namespace Clouds::UI
