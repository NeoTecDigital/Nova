// Written by Richard Christopher, Copyright 2026 NeoTec Digital
#include "./UIWindow.h"
#include "Splash/Primitives.h"

namespace Clouds::UI {

UIWindow::UIWindow(Splash::Registry& reg, Splash::NodeId self,
                   const UITheme& ui_theme,
                   const std::string& window_title,
                   const glm::vec2& content_size,
                   std::shared_ptr<Splash::SpatialFont> font_ptr)
    : SpatialNode(reg, self), title(window_title), theme_(&ui_theme), font_(font_ptr) {
    name = "UIWindow: " + title;

    // Geometry, resolved once: the titlebar band and every chrome position
    // derived from it are a layout, and re-deciding a layout mid-frame means
    // moving nodes the pointer is already being tested against. Material --
    // colours, radii, border widths -- is resolved per frame instead; see the
    // seam contract at the foot of UITheme.h.
    window_size = glm::vec2(content_size.x, content_size.y + theme_->titlebar_height);
    size = window_size;

    // The window is the input target for its own chrome. Its titlebar and body
    // are panels it draws over itself; without this the pointer would land on
    // one of them and the window -- which owns the only drag in the tree, and
    // the focus click -- would never hear about it. Children that mean
    // something on their own opt back in by claiming, so the capture cannot
    // swallow a button press. See SpatialNode's input-routing block.
    captures_subtree_input = true;

    setupChrome(reg, self);

    // Resolve once here as well as every frame, so a window is never briefly
    // wearing SpatialPanel's own defaults between construction and its first
    // render. This is a starting value, not a stored one: collectRender
    // re-resolves regardless, which is what keeps a later theme edit live.
    syncChromeToTheme(reg);
}

void UIWindow::setupChrome(Splash::Registry& reg, Splash::NodeId self) {
    const float tb_h = theme_->titlebar_height;
    const float body_h = window_size.y - tb_h;

    buildBody(reg, self, body_h, tb_h);
    buildTitlebar(reg, self, body_h, tb_h);
    buildWindowControls(reg, self);
}

void UIWindow::buildBody(Splash::Registry& reg, Splash::NodeId self, float body_h, float tb_h) {
    body_panel_ = reg.emplace<Splash::SpatialPanel>(
        self, glm::vec2(window_size.x, body_h), resolvedBodyColor());
    reg.transform(body_panel_).position = glm::vec3(0.0f, -tb_h * 0.5f, 0.0f);
    reg[body_panel_].claims_pointer_input = false;

    content_area = reg.createContainer(body_panel_);
    reg[content_area].name = "WindowContentArea";
    reg.transform(content_area).position = glm::vec3(0.0f, 0.0f, 0.002f);
    // A container, not a surface: its default 1x1 extent describes nothing, so
    // it must not be hit-tested. Its children still are.
    reg[content_area].interactable = false;
}

void UIWindow::buildTitlebar(Splash::Registry& reg, Splash::NodeId self, float body_h, float tb_h) {
    titlebar_panel_ = reg.emplace<Splash::SpatialPanel>(
        self, glm::vec2(window_size.x, tb_h), resolvedTitlebarColor());
    reg.transform(titlebar_panel_).position = glm::vec3(0.0f, body_h * 0.5f, 0.003f);
    reg[titlebar_panel_].claims_pointer_input = false;

    if (!font_) return;

    title_label_ = makeUILabel(reg, titlebar_panel_, *theme_, title, font_,
                               TextRole::HEADER, TextTone::PRIMARY, TextAlignment::LEFT);
    reg.transform(title_label_).position = glm::vec3(-window_size.x * 0.5f + 0.04f, 0.0f, 0.002f);
    reg[title_label_].claims_pointer_input = false;   // text is not a grab handle
}

void UIWindow::buildWindowControls(Splash::Registry& reg, Splash::NodeId self) {
    const glm::vec2 btn_size(0.040f, 0.032f);

    // The handlers name the window by id, not by `this`: a click arrives long
    // after construction, and an id that no longer resolves is a no-op rather
    // than a call through a dangling pointer.
    min_button_ = makeUIButton(reg, titlebar_panel_, *theme_, "-", btn_size, font_,
        [&reg, self]() {
            if (UIWindow* window = reg.as<UIWindow>(self)) window->toggleMinimize(reg);
        },
        ButtonVariant::GHOST);
    reg.transform(min_button_).position = glm::vec3(window_size.x * 0.5f - 0.095f, 0.0f, 0.003f);

    close_button_ = makeUIButton(reg, titlebar_panel_, *theme_, "x", btn_size, font_,
        [&reg, self]() {
            if (UIWindow* window = reg.as<UIWindow>(self)) window->close(reg);
        },
        ButtonVariant::DANGER);
    reg.transform(close_button_).position = glm::vec3(window_size.x * 0.5f - 0.045f, 0.0f, 0.003f);
}

glm::vec4 UIWindow::resolvedTitlebarColor() const {
    return is_focused ? theme_->window_titlebar_active : theme_->window_titlebar_inactive;
}

glm::vec4 UIWindow::resolvedBorderColor() const {
    return is_focused ? theme_->window_border_active : theme_->window_border_inactive;
}

glm::vec4 UIWindow::resolvedBodyColor() const {
    return theme_->window_bg;
}

void UIWindow::syncChromeToTheme(Splash::Registry& reg) {
    const glm::vec4 border = resolvedBorderColor();

    if (Splash::SpatialPanel* titlebar = reg.as<Splash::SpatialPanel>(titlebar_panel_)) {
        titlebar->background_color = resolvedTitlebarColor();
        titlebar->border_color = border;
        titlebar->corner_radius = theme_->radius_window;
        titlebar->border_thickness = theme_->border_window;
    }
    if (Splash::SpatialPanel* body = reg.as<Splash::SpatialPanel>(body_panel_)) {
        body->background_color = resolvedBodyColor();
        body->border_color = border;
        body->corner_radius = theme_->radius_window;
        body->border_thickness = theme_->border_window;
    }
}

void UIWindow::setTitle(Splash::Registry& reg, const std::string& new_title) {
    title = new_title;
    name = "UIWindow: " + title;
    if (UILabel* label = reg.as<UILabel>(title_label_)) {
        label->setText(title);
    }
}

void UIWindow::setFocused(bool focused) {
    is_focused = focused;   // SpatialNode::is_focused
}

void UIWindow::toggleMinimize(Splash::Registry& reg) {
    is_minimized = !is_minimized;
    if (Splash::SpatialNode* body = reg.get(body_panel_)) {
        body->visible = !is_minimized;
    }
}

void UIWindow::close(Splash::Registry&) {
    visible = false;
    if (on_close) {
        on_close(this);
    }
}

void UIWindow::onRayMove(Splash::Registry& reg, Splash::NodeId self, const Nova::Math::RayHit& hit) {
    if (is_dragging_) {
        glm::vec3 delta = hit.world_point - drag_start_hit_;
        reg.transform(self).position = drag_start_pos_ + delta;
    }
    Splash::SpatialNode::onRayMove(reg, self, hit);
}

void UIWindow::onRayButton(Splash::Registry& reg, Splash::NodeId self, const Nova::Math::RayHit& hit,
                           uint32_t button, bool pressed) {
    if (button == 1) { // Left click
        pressed ? onLeftPress(reg, self, hit) : (void)(is_dragging_ = false);
    }
    Splash::SpatialNode::onRayButton(reg, self, hit, button, pressed);
}

void UIWindow::onLeftPress(Splash::Registry& reg, Splash::NodeId self, const Nova::Math::RayHit& hit) {
    if (on_focus_gained) {
        on_focus_gained(this);
    }

    // hit.local_point is in this window's frame even when the pointer landed on
    // the titlebar panel: the scene re-casts a captured hit onto the capturing
    // node's own plane, which is what makes this band test mean anything.
    const float tb_top = window_size.y * 0.5f;
    const float tb_bottom = tb_top - theme_->titlebar_height;
    if (hit.local_point.y < tb_bottom || hit.local_point.y > tb_top) return;

    is_dragging_ = true;
    drag_start_hit_ = hit.world_point;
    drag_start_pos_ = reg[self].transform().position;
}

void UIWindow::collectRender(Splash::Registry& reg, Splash::NodeId,
                             Nova::SpatialMeshBuffer*,
                             std::vector<Splash::SpatialRenderCommand>&) {
    // The window itself draws nothing: its chrome panels do, and this is where
    // their material is refreshed from the live theme once per frame.
    syncChromeToTheme(reg);
}

} // namespace Clouds::UI
