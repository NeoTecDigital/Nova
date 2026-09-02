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
    EngineHUD(std::shared_ptr<SpatialNode> root_node,
              std::shared_ptr<NovaSpatial::SpatialFont> spatial_font,
              NovaMath::EnginePhysicsConfig* physics_config);
    ~EngineHUD() = default;

    void initialize();
    void update(float dt);

    void toggleMenu();
    void setMenuVisible(bool visible);
    bool isMenuVisible() const { return menu_visible_; }

    void onPhysicsChanged();

private:
    std::shared_ptr<SpatialNode> root_;
    std::shared_ptr<NovaSpatial::SpatialFont> font_;
    NovaMath::EnginePhysicsConfig* config_ = nullptr;

    bool menu_visible_ = false;

    // Bottom HUD Pill
    std::shared_ptr<SpatialPanel> pill_panel_;
    std::shared_ptr<SpatialButton> pill_button_;
    std::shared_ptr<SpatialLabel> pill_label_;

    // Config & Monitoring Menu Panel
    std::shared_ptr<SpatialPanel> menu_panel_;
    std::shared_ptr<SpatialLabel> menu_title_;
    std::shared_ptr<SpatialLabel> telemetry_label_;

    // Interactive Physics Controls
    std::shared_ptr<SpatialLabel> lambda_label_;
    std::shared_ptr<SpatialButton> lambda_dec_btn_;
    std::shared_ptr<SpatialButton> lambda_inc_btn_;

    std::shared_ptr<SpatialLabel> omega_label_;
    std::shared_ptr<SpatialButton> omega_dec_btn_;
    std::shared_ptr<SpatialButton> omega_inc_btn_;

    std::shared_ptr<SpatialButton> mode_toggle_btn_;
    std::shared_ptr<SpatialButton> dither_toggle_btn_;
    std::shared_ptr<SpatialButton> reset_btn_;

    void updateTelemetry();
    void updateControlLabels();
};

} // namespace Clouds
