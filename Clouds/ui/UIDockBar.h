#pragma once

#include "Splash/SpatialNode.h"
#include "Splash/Primitives.h"
#include "./UIComponents.h"
#include "./UITheme.h"
#include <memory>
#include <vector>
#include <string>
#include <functional>

namespace Clouds::UI {

struct DockItem {
    std::string icon_label;
    std::string tooltip;
    std::function<void()> on_click;
};

class UIDockBar : public Splash::SpatialNode {
public:
    UIDockBar(float dock_width,
              std::shared_ptr<Splash::SpatialFont> font_ptr);

    void addItem(const std::string& label, std::function<void()> click_handler);
    void setStatusText(const std::string& status);

    void collectRender(Nova::SpatialMeshBuffer* mesh_buf, std::vector<Splash::SpatialRenderCommand>& out_commands) override;

private:
    std::shared_ptr<Splash::SpatialFont> font_;
    std::shared_ptr<Splash::SpatialPanel> dock_panel_;
    std::shared_ptr<UILabel> status_label_;
    std::vector<std::shared_ptr<UIButton>> buttons_;

    float total_width_ = 1.6f;
    float current_x_ = -0.6f;
};

} // namespace Clouds::UI
