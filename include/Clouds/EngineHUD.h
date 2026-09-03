#pragma once

#include "./SpatialNode.h"
#include "./Primitives.h"
#include "Core/math/engine_physics.h"
#include <memory>
#include <functional>
#include <string>

namespace Clouds {

class EngineHUD {
public:
    EngineHUD(std::shared_ptr<Splash::SpatialNode> root_node,
              std::shared_ptr<Nova::SpatialFont> spatial_font,
              Nova::Math::EnginePhysicsConfig* physics_config);
    ~EngineHUD() = default;

    void initialize();
    void update(float dt);

    void toggleMenu();
    void setMenuVisible(bool visible);
    bool isMenuVisible() const { return menu_visible_; }

    void onPhysicsChanged();

private:
    std::shared_ptr<Splash::SpatialNode> root_;
    std::shared_ptr<Nova::SpatialFont> font_;
    Nova::Math::EnginePhysicsConfig* config_ = nullptr;

    bool menu_visible_ = false;

    // Bottom HUD Pill
    std::shared_ptr<Splash::SpatialPanel> pill_panel_;
    std::shared_ptr<Splash::SpatialButton> pill_button_;
    std::shared_ptr<Splash::SpatialLabel> pill_label_;

    // Config & Monitoring Menu Panel
    std::shared_ptr<Splash::SpatialPanel> menu_panel_;
    std::shared_ptr<Splash::SpatialLabel> menu_title_;
    std::shared_ptr<Splash::SpatialLabel> telemetry_label_;

    // Interactive Physics Controls
    std::shared_ptr<Splash::SpatialLabel> lambda_label_;
    std::shared_ptr<Splash::SpatialButton> lambda_dec_btn_;
    std::shared_ptr<Splash::SpatialButton> lambda_inc_btn_;

    std::shared_ptr<Splash::SpatialLabel> omega_label_;
    std::shared_ptr<Splash::SpatialButton> omega_dec_btn_;
    std::shared_ptr<Splash::SpatialButton> omega_inc_btn_;

    std::shared_ptr<Splash::SpatialButton> mode_toggle_btn_;
    std::shared_ptr<Splash::SpatialButton> dither_toggle_btn_;
    std::shared_ptr<Splash::SpatialButton> reset_btn_;

    void updateTelemetry();
    void updateControlLabels();
};

} // namespace Clouds
