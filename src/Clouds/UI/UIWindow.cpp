#include "include/Clouds/UI/UIWindow.h"
#include "include/Clouds/Primitives.h"

namespace Clouds::UI {

UIWindow::UIWindow(const std::string& window_title,
                   const glm::vec2& content_size,
                   std::shared_ptr<Nova::SpatialFont> font_ptr)
    : title(window_title), font_(font_ptr) {
    name = "UIWindow: " + title;
    window_size = glm::vec2(content_size.x, content_size.y + g_Theme.titlebar_height);
    size = window_size;

    // The window is the input target for its own chrome. Its titlebar and body
    // are panels it draws over itself; without this the pointer would land on
    // one of them and the window -- which owns the only drag in the tree, and
    // the focus click -- would never hear about it. Children that mean
    // something on their own opt back in by claiming, so the capture cannot
    // swallow a button press. See SpatialNode's input-routing block.
    captures_subtree_input = true;

    setupChrome();
}

void UIWindow::setupChrome() {
    const float tb_h = g_Theme.titlebar_height;
    const float body_h = window_size.y - tb_h;

    buildBody(body_h, tb_h);
    buildTitlebar(body_h, tb_h);
    buildWindowControls();
}

void UIWindow::buildBody(float body_h, float tb_h) {
    body_panel_ = std::make_shared<Splash::SpatialPanel>(
        glm::vec2(window_size.x, body_h),
        g_Theme.window_bg
    );
    body_panel_->corner_radius = g_Theme.radius_window;
    body_panel_->border_thickness = g_Theme.border_window;
    body_panel_->border_color = g_Theme.window_border_active;
    body_panel_->transform.position = glm::vec3(0.0f, -tb_h * 0.5f, 0.0f);
    body_panel_->claims_pointer_input = false;
    addChild(body_panel_);

    content_area = std::make_shared<Splash::SpatialNode>();
    content_area->name = "WindowContentArea";
    content_area->transform.position = glm::vec3(0.0f, 0.0f, 0.002f);
    // A container, not a surface: its default 1x1 extent describes nothing, so
    // it must not be hit-tested. Its children still are.
    content_area->interactable = false;
    body_panel_->addChild(content_area);
}

void UIWindow::buildTitlebar(float body_h, float tb_h) {
    titlebar_panel_ = std::make_shared<Splash::SpatialPanel>(
        glm::vec2(window_size.x, tb_h),
        g_Theme.window_titlebar_active
    );
    titlebar_panel_->corner_radius = g_Theme.radius_window;
    titlebar_panel_->border_thickness = g_Theme.border_window;
    titlebar_panel_->border_color = g_Theme.window_border_active;
    titlebar_panel_->transform.position = glm::vec3(0.0f, body_h * 0.5f, 0.003f);
    titlebar_panel_->claims_pointer_input = false;
    addChild(titlebar_panel_);

    if (!font_) return;

    title_label_ = std::make_shared<UILabel>(
        title,
        font_,
        g_Theme.scale_header,
        g_Theme.text_primary,
        TextAlignment::LEFT
    );
    title_label_->transform.position = glm::vec3(-window_size.x * 0.5f + 0.04f, 0.0f, 0.002f);
    title_label_->claims_pointer_input = false;   // text is not a grab handle
    titlebar_panel_->addChild(title_label_);
}

void UIWindow::buildWindowControls() {
    const glm::vec2 btn_size(0.040f, 0.032f);

    min_button_ = std::make_shared<UIButton>(
        "-", btn_size, font_, [this]() { toggleMinimize(); }, ButtonVariant::GHOST);
    min_button_->transform.position = glm::vec3(window_size.x * 0.5f - 0.095f, 0.0f, 0.003f);
    titlebar_panel_->addChild(min_button_);

    close_button_ = std::make_shared<UIButton>(
        "x", btn_size, font_, [this]() { close(); }, ButtonVariant::DANGER);
    close_button_->transform.position = glm::vec3(window_size.x * 0.5f - 0.045f, 0.0f, 0.003f);
    titlebar_panel_->addChild(close_button_);
}

void UIWindow::setTitle(const std::string& new_title) {
    title = new_title;
    name = "UIWindow: " + title;
    if (title_label_) {
        title_label_->setText(title);
    }
}

void UIWindow::setFocused(bool focused) {
    is_focused = focused;   // SpatialNode::is_focused
    if (titlebar_panel_) {
        titlebar_panel_->background_color = focused ?
            g_Theme.window_titlebar_active : g_Theme.window_titlebar_inactive;
        titlebar_panel_->border_color = focused ?
            g_Theme.window_border_active : g_Theme.window_border_inactive;
    }
    if (body_panel_) {
        body_panel_->border_color = focused ?
            g_Theme.window_border_active : g_Theme.window_border_inactive;
    }
}

void UIWindow::toggleMinimize() {
    is_minimized = !is_minimized;
    if (body_panel_) {
        body_panel_->visible = !is_minimized;
    }
}

void UIWindow::close() {
    visible = false;
    if (on_close) {
        on_close(this);
    }
}

void UIWindow::onRayMove(const Nova::Math::RayHit& hit) {
    if (is_dragging_) {
        glm::vec3 delta = hit.world_point - drag_start_hit_;
        transform.position = drag_start_pos_ + delta;
    }
    Splash::SpatialNode::onRayMove(hit);
}

void UIWindow::onRayButton(const Nova::Math::RayHit& hit, uint32_t button, bool pressed) {
    if (button == 1) { // Left click
        pressed ? onLeftPress(hit) : (void)(is_dragging_ = false);
    }
    Splash::SpatialNode::onRayButton(hit, button, pressed);
}

void UIWindow::onLeftPress(const Nova::Math::RayHit& hit) {
    if (on_focus_gained) {
        on_focus_gained(this);
    }

    // hit.local_point is in this window's frame even when the pointer landed on
    // the titlebar panel: the scene re-casts a captured hit onto the capturing
    // node's own plane, which is what makes this band test mean anything.
    const float tb_top = window_size.y * 0.5f;
    const float tb_bottom = tb_top - g_Theme.titlebar_height;
    if (hit.local_point.y < tb_bottom || hit.local_point.y > tb_top) return;

    is_dragging_ = true;
    drag_start_hit_ = hit.world_point;
    drag_start_pos_ = transform.position;
}

void UIWindow::collectRender(Nova::SpatialMeshBuffer* mesh_buf,
                             std::vector<Splash::SpatialRenderCommand>& out_commands) {
    if (!visible) return;
    Splash::SpatialNode::collectRender(mesh_buf, out_commands);
}

} // namespace Clouds::UI
