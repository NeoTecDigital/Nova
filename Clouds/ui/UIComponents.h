#pragma once

#include "Splash/SpatialNode.h"
#include "Nova/pipeline/spatial_font.h"
#include "./UITheme.h"
#include <functional>
#include <vector>
#include <string>

namespace Clouds::UI {

enum class TextAlignment {
    LEFT,
    CENTER,
    RIGHT
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
 */
class UILabel : public Splash::SpatialNode {
public:
    std::string text;
    glm::vec4 color{1.0f};
    float font_scale = 0.00045f;
    TextAlignment alignment = TextAlignment::LEFT;
    std::shared_ptr<Nova::SpatialFont> font;

    UILabel(const std::string& label_text,
            std::shared_ptr<Nova::SpatialFont> font_ptr,
            float scale = 0.00045f,
            const glm::vec4& text_color = g_Theme.text_primary,
            TextAlignment align = TextAlignment::LEFT);

    void setText(const std::string& new_text);
    glm::vec2 getDimensions() const;

    void collectRender(Nova::SpatialMeshBuffer* mesh_buf, std::vector<Splash::SpatialRenderCommand>& out_commands) override;
};

/**
 * UIButton - Styled Interactive Button with Hover/Press Transitions
 */
class UIButton : public Splash::SpatialNode {
public:
    std::string label;
    ButtonVariant variant = ButtonVariant::PRIMARY;
    std::function<void()> on_click;

    bool enabled = true;
    float corner_radius = g_Theme.radius_button;
    float border_thickness = g_Theme.border_button;

    UIButton(const std::string& btn_label,
             const glm::vec2& btn_size,
             std::shared_ptr<Nova::SpatialFont> font_ptr,
             std::function<void()> click_handler = nullptr,
             ButtonVariant btn_variant = ButtonVariant::PRIMARY);

    void setLabel(const std::string& new_label);
    void setVariant(ButtonVariant new_variant);

    void onRayEnter(const Nova::Math::RayHit& hit) override;
    void onRayLeave() override;
    void onRayButton(const Nova::Math::RayHit& hit, uint32_t button, bool pressed) override;

    void collectRender(Nova::SpatialMeshBuffer* mesh_buf, std::vector<Splash::SpatialRenderCommand>& out_commands) override;

private:
    std::shared_ptr<Nova::SpatialFont> font_;
    std::shared_ptr<UILabel> label_node_;
    float hover_factor_ = 0.0f;
    bool is_pressed_ = false;

    glm::vec4 getBaseColor() const;
    glm::vec4 getHoverColor() const;
    glm::vec4 getActiveColor() const;
};

} // namespace Clouds::UI
