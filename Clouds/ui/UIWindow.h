// Written by Richard Christopher, Copyright 2026 NeoTec Digital
#pragma once

#include "Splash/Registry.h"
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

    Splash::NodeId content_area;

    UIWindow(Splash::Registry& reg, Splash::NodeId self,
             const UITheme& ui_theme,
             const std::string& window_title,
             const glm::vec2& content_size,
             std::shared_ptr<Splash::SpatialFont> font_ptr);
    virtual ~UIWindow() = default;

    void setTitle(Splash::Registry& reg, const std::string& new_title);

    // Writes SpatialNode::is_focused -- the base member, not a second copy of
    // it. A derived shadow used to sit alongside it, so this setter and
    // SpatialNode::onRayButton wrote different bools and disagreed in silence.
    //
    // It writes the flag and nothing else. The chrome colours that follow from
    // focus are resolved from the theme every frame by the two accessors below,
    // so there is no second place for the focused look to be decided from and
    // no way for a focus change that arrived through the base to be missed.
    void setFocused(bool focused);
    void toggleMinimize(Splash::Registry& reg);
    void close(Splash::Registry& reg);

    // Resolved from the live theme and the live focus flag on every call.
    // syncChromeToTheme() -- the only thing that writes the panels' material --
    // uses exactly these, so what a caller reads here is what the frame draws.
    glm::vec4 resolvedTitlebarColor() const;
    glm::vec4 resolvedBorderColor() const;
    glm::vec4 resolvedBodyColor() const;

    const UITheme& theme() const { return *theme_; }

    // Interaction overrides
    void onRayMove(Splash::Registry& reg, Splash::NodeId self, const Nova::Math::RayHit& hit) override;
    void onRayButton(Splash::Registry& reg, Splash::NodeId self, const Nova::Math::RayHit& hit,
                     uint32_t button, bool pressed) override;

    void collectRender(Splash::Registry& reg, Splash::NodeId self,
                       Nova::SpatialMeshBuffer* mesh_buf,
                       std::vector<Splash::SpatialRenderCommand>& out_commands) override;

private:
    const UITheme* theme_;
    std::shared_ptr<Splash::SpatialFont> font_;

    // Window chrome
    Splash::NodeId titlebar_panel_;
    Splash::NodeId title_label_;
    Splash::NodeId min_button_;
    Splash::NodeId close_button_;
    Splash::NodeId body_panel_;

    // Dragging state
    bool is_dragging_ = false;
    glm::vec3 drag_start_hit_{0.0f};
    glm::vec3 drag_start_pos_{0.0f};

    void setupChrome(Splash::Registry& reg, Splash::NodeId self);
    void buildBody(Splash::Registry& reg, Splash::NodeId self, float body_h, float tb_h);
    void buildTitlebar(Splash::Registry& reg, Splash::NodeId self, float body_h, float tb_h);
    void buildWindowControls(Splash::Registry& reg, Splash::NodeId self);
    void onLeftPress(Splash::Registry& reg, Splash::NodeId self, const Nova::Math::RayHit& hit);

    // Pushes the resolved material onto the SpatialPanels the window draws
    // itself with. Called from collectRender, so it runs once per frame against
    // whatever the theme says at that moment. SpatialPanel keys its mesh cache
    // on these fields' values, so re-writing the same numbers costs nothing.
    void syncChromeToTheme(Splash::Registry& reg);
};

inline Splash::NodeId makeUIWindow(Splash::Registry& reg, Splash::NodeId parent,
                                   const UITheme& theme, const std::string& title,
                                   const glm::vec2& content_size,
                                   std::shared_ptr<Splash::SpatialFont> font) {
    return reg.emplace<UIWindow>(parent, theme, title, content_size, std::move(font));
}

} // namespace Clouds::UI
