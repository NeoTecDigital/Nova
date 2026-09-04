// Written by Richard Christopher, Copyright 2026 NeoTec Digital
#pragma once

#include "Splash/SpatialNode.h"
#include "Splash/Primitives.h"
#include "./UIComponents.h"
#include "./UITheme.h"
#include <memory>
#include <string>
#include <functional>

namespace Clouds::UI {

class UIWindow : public Splash::SpatialNode {
public:
    std::string title;
    glm::vec2 window_size; // Total window size including titlebar
    bool is_minimized = false;

    std::function<void(UIWindow*)> on_focus_gained;
    std::function<void(UIWindow*)> on_close;

    std::shared_ptr<Splash::SpatialNode> content_area;

    UIWindow(const UITheme& ui_theme,
             const std::string& window_title,
             const glm::vec2& content_size,
             std::shared_ptr<Splash::SpatialFont> font_ptr);
    virtual ~UIWindow() = default;

    void setTitle(const std::string& new_title);

    // Writes SpatialNode::is_focused -- the base member, not a second copy of
    // it. A derived shadow used to sit alongside it, so this setter and
    // SpatialNode::onRayButton wrote different bools and disagreed in silence.
    //
    // It writes the flag and nothing else. The chrome colours that follow from
    // focus are resolved from the theme every frame by the two accessors below,
    // so there is no second place for the focused look to be decided from and
    // no way for a focus change that arrived through the base to be missed.
    void setFocused(bool focused);
    void toggleMinimize();
    void close();

    // Resolved from the live theme and the live focus flag on every call.
    // syncChromeToTheme() -- the only thing that writes the panels' material --
    // uses exactly these, so what a caller reads here is what the frame draws.
    glm::vec4 resolvedTitlebarColor() const;
    glm::vec4 resolvedBorderColor() const;
    glm::vec4 resolvedBodyColor() const;

    const UITheme& theme() const { return *theme_; }

    // Interaction overrides
    void onRayMove(const Nova::Math::RayHit& hit) override;
    void onRayButton(const Nova::Math::RayHit& hit, uint32_t button, bool pressed) override;

    void collectRender(Nova::SpatialMeshBuffer* mesh_buf, std::vector<Splash::SpatialRenderCommand>& out_commands) override;

private:
    const UITheme* theme_;
    std::shared_ptr<Splash::SpatialFont> font_;

    // Window chrome
    std::shared_ptr<Splash::SpatialPanel> titlebar_panel_;
    std::shared_ptr<UILabel> title_label_;
    std::shared_ptr<UIButton> min_button_;
    std::shared_ptr<UIButton> close_button_;
    std::shared_ptr<Splash::SpatialPanel> body_panel_;

    // Dragging state
    bool is_dragging_ = false;
    glm::vec3 drag_start_hit_{0.0f};
    glm::vec3 drag_start_pos_{0.0f};

    void setupChrome();
    void buildBody(float body_h, float tb_h);
    void buildTitlebar(float body_h, float tb_h);
    void buildWindowControls();
    void onLeftPress(const Nova::Math::RayHit& hit);

    // Pushes the resolved material onto the SpatialPanels the window draws
    // itself with. Called from collectRender, so it runs once per frame against
    // whatever the theme says at that moment. SpatialPanel keys its mesh cache
    // on these fields' values, so re-writing the same numbers costs nothing.
    void syncChromeToTheme();
};

} // namespace Clouds::UI
