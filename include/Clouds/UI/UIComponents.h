#pragma once

#include "../SpatialNode.h"
#include "../../../Core/modules/spatial_pipeline/spatial_font.h"
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
class UILabel : public SpatialNode {
public:
    std::string text;
    glm::vec4 color{1.0f};
    float font_scale = 0.00045f;
    TextAlignment alignment = TextAlignment::LEFT;
    std::shared_ptr<NovaSpatial::SpatialFont> font;

    UILabel(const std::string& label_text,
            std::shared_ptr<NovaSpatial::SpatialFont> font_ptr,
            float scale = 0.00045f,
            const glm::vec4& text_color = g_Theme.text_primary,
            TextAlignment align = TextAlignment::LEFT);

    void setText(const std::string& new_text);
    glm::vec2 getDimensions() const;

    void collectRender(NovaSpatial::SpatialMeshBuffer* mesh_buf, std::vector<SpatialRenderCommand>& out_commands) override;
};

/**
 * UIButton - Styled Interactive Button with Hover/Press Transitions
 */
class UIButton : public SpatialNode {
public:
    std::string label;
    ButtonVariant variant = ButtonVariant::PRIMARY;
    std::function<void()> on_click;

    bool enabled = true;
    float corner_radius = g_Theme.radius_button;
    float border_thickness = g_Theme.border_button;

    UIButton(const std::string& btn_label,
             const glm::vec2& btn_size,
             std::shared_ptr<NovaSpatial::SpatialFont> font_ptr,
             std::function<void()> click_handler = nullptr,
             ButtonVariant btn_variant = ButtonVariant::PRIMARY);

    void setLabel(const std::string& new_label);
    void setVariant(ButtonVariant new_variant);

    void onRayEnter(const NovaMath::RayHit& hit) override;
    void onRayLeave() override;
    void onRayButton(const NovaMath::RayHit& hit, uint32_t button, bool pressed) override;

    void collectRender(NovaSpatial::SpatialMeshBuffer* mesh_buf, std::vector<SpatialRenderCommand>& out_commands) override;

private:
    std::shared_ptr<NovaSpatial::SpatialFont> font_;
    std::shared_ptr<UILabel> label_node_;
    float hover_factor_ = 0.0f;
    bool is_pressed_ = false;

    glm::vec4 getBaseColor() const;
    glm::vec4 getHoverColor() const;
    glm::vec4 getActiveColor() const;
};

/**
 * UIBadge - Status Tag Pill (e.g. "ACTIVE", "WON", "PROPOSAL")
 */
class UIBadge : public SpatialNode {
public:
    std::string text;
    glm::vec4 bg_color;
    glm::vec4 text_color{1.0f};

    UIBadge(const std::string& badge_text,
            const glm::vec4& color,
            std::shared_ptr<NovaSpatial::SpatialFont> font_ptr);

    void setText(const std::string& new_text, const glm::vec4& color);
    void collectRender(NovaSpatial::SpatialMeshBuffer* mesh_buf, std::vector<SpatialRenderCommand>& out_commands) override;

private:
    std::shared_ptr<NovaSpatial::SpatialFont> font_;
    std::shared_ptr<UILabel> label_node_;
};

/**
 * UIProgressBar - Visual Percentage Metric Bar
 */
class UIProgressBar : public SpatialNode {
public:
    float progress = 0.0f; // 0.0 to 1.0
    glm::vec4 fill_color = g_Theme.primary;
    glm::vec4 bg_color = g_Theme.surface_base;

    UIProgressBar(const glm::vec2& bar_size,
                  float initial_progress = 0.0f,
                  const glm::vec4& bar_color = g_Theme.primary);

    void setProgress(float val);
    void collectRender(NovaSpatial::SpatialMeshBuffer* mesh_buf, std::vector<SpatialRenderCommand>& out_commands) override;
};

/**
 * UITabControl - Multi-Tab Switcher
 */
class UITabControl : public SpatialNode {
public:
    int active_tab_index = 0;
    std::function<void(int new_index)> on_tab_changed;

    UITabControl(const std::vector<std::string>& tab_names,
                 float width,
                 std::shared_ptr<NovaSpatial::SpatialFont> font_ptr,
                 std::function<void(int)> tab_callback = nullptr);

    void setActiveTab(int index);
    void collectRender(NovaSpatial::SpatialMeshBuffer* mesh_buf, std::vector<SpatialRenderCommand>& out_commands) override;

private:
    std::vector<std::shared_ptr<UIButton>> tab_buttons_;
    float total_width_ = 1.0f;
};

} // namespace Clouds::UI
