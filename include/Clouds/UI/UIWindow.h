#pragma once

#include "../SpatialNode.h"
#include "../Primitives.h"
#include "./UIComponents.h"
#include "./UITheme.h"
#include <memory>
#include <string>
#include <functional>

namespace Clouds::UI {

class UIWindow : public SpatialNode {
public:
    std::string title;
    glm::vec2 window_size; // Total window size including titlebar
    bool is_minimized = false;

    std::function<void(UIWindow*)> on_focus_gained;
    std::function<void(UIWindow*)> on_close;

    std::shared_ptr<SpatialNode> content_area;

    UIWindow(const std::string& window_title,
             const glm::vec2& content_size,
             std::shared_ptr<NovaSpatial::SpatialFont> font_ptr);
    virtual ~UIWindow() = default;

    void setTitle(const std::string& new_title);

    // Writes SpatialNode::is_focused -- the base member, not a second copy of
    // it. A derived shadow used to sit alongside it, so this setter and
    // SpatialNode::onRayButton wrote different bools and disagreed in silence.
    void setFocused(bool focused);
    void toggleMinimize();
    void close();

    // Interaction overrides
    void onRayMove(const NovaMath::RayHit& hit) override;
    void onRayButton(const NovaMath::RayHit& hit, uint32_t button, bool pressed) override;

    void collectRender(NovaSpatial::SpatialMeshBuffer* mesh_buf, std::vector<SpatialRenderCommand>& out_commands) override;

private:
    std::shared_ptr<NovaSpatial::SpatialFont> font_;

    // Window chrome
    std::shared_ptr<SpatialPanel> titlebar_panel_;
    std::shared_ptr<UILabel> title_label_;
    std::shared_ptr<UIButton> min_button_;
    std::shared_ptr<UIButton> close_button_;
    std::shared_ptr<SpatialPanel> body_panel_;

    // Dragging state
    bool is_dragging_ = false;
    glm::vec3 drag_start_hit_{0.0f};
    glm::vec3 drag_start_pos_{0.0f};

    void setupChrome();
    void buildBody(float body_h, float tb_h);
    void buildTitlebar(float body_h, float tb_h);
    void buildWindowControls();
    void onLeftPress(const NovaMath::RayHit& hit);
};

} // namespace Clouds::UI
