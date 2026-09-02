#include "include/Clouds/EngineHUD.h"
#include "Core/components/logger.h"
#include <iomanip>
#include <sstream>

namespace Clouds {

EngineHUD::EngineHUD(std::shared_ptr<SpatialNode> root_node,
                     std::shared_ptr<NovaSpatial::SpatialFont> spatial_font,
                     NovaMath::EnginePhysicsConfig* physics_config)
    : root_(root_node), font_(spatial_font), config_(physics_config) {
}

void EngineHUD::initialize() {
    if (!root_ || !font_ || !config_) return;

    // -------------------------------------------------------------
    // 1. Bottom Telemetry & Control Pill Button
    // -------------------------------------------------------------
    pill_panel_ = std::make_shared<SpatialPanel>(
        glm::vec2(0.95f, 0.10f),
        glm::vec4(0.06f, 0.08f, 0.14f, 0.90f)
    );
    pill_panel_->corner_radius = 0.045f;
    pill_panel_->border_thickness = 0.003f;
    pill_panel_->border_color = glm::vec4(0.25f, 0.45f, 0.75f, 0.90f);
    pill_panel_->transform.position = glm::vec3(0.0f, -0.78f, 0.15f);

    pill_button_ = std::make_shared<SpatialButton>(
        "Config",
        glm::vec2(0.92f, 0.08f),
        font_,
        [this]() {
            toggleMenu();
        }
    );
    pill_button_->normal_color = glm::vec4(0.09f, 0.12f, 0.20f, 0.40f);
    pill_button_->hover_color = glm::vec4(0.18f, 0.28f, 0.50f, 0.85f);
    pill_button_->press_color = glm::vec4(0.12f, 0.20f, 0.40f, 1.0f);
    pill_button_->corner_radius = 0.035f;

    pill_label_ = std::make_shared<SpatialLabel>(
        "⚡ 60 FPS | λ=1.0 | LaserFocus",
        font_,
        0.00095f,
        glm::vec4(0.75f, 0.90f, 1.0f, 1.0f)
    );
    pill_label_->transform.position = glm::vec3(0.0f, 0.0f, 0.002f);

    pill_panel_->addChild(pill_button_);
    pill_button_->addChild(pill_label_);
    root_->addChild(pill_panel_);

    // -------------------------------------------------------------
    // 2. Interactive Physics & Monitoring Config Menu
    // -------------------------------------------------------------
    menu_panel_ = std::make_shared<SpatialPanel>(
        glm::vec2(1.20f, 0.95f),
        glm::vec4(0.05f, 0.07f, 0.12f, 0.95f)
    );
    menu_panel_->corner_radius = 0.04f;
    menu_panel_->border_thickness = 0.004f;
    menu_panel_->border_color = glm::vec4(0.35f, 0.55f, 0.95f, 0.95f);
    menu_panel_->transform.position = glm::vec3(0.0f, 0.0f, 0.25f);
    menu_panel_->visible = false;

    menu_title_ = std::make_shared<SpatialLabel>(
        ":: ENGINE PHYSICS & INPUT ACCELERATOR ::",
        font_,
        0.0012f,
        glm::vec4(0.4f, 0.75f, 1.0f, 1.0f)
    );
    menu_title_->transform.position = glm::vec3(0.0f, 0.40f, 0.002f);
    menu_panel_->addChild(menu_title_);

    telemetry_label_ = std::make_shared<SpatialLabel>(
        "Metrics: FPS=60.0 | Nodes=0 | Tests=0 | Dither=ON",
        font_,
        0.0009f,
        glm::vec4(0.6f, 0.8f, 0.95f, 0.9f)
    );
    telemetry_label_->transform.position = glm::vec3(0.0f, 0.32f, 0.002f);
    menu_panel_->addChild(telemetry_label_);

    // Non-linear Phase Coupling Lambda controls
    lambda_label_ = std::make_shared<SpatialLabel>(
        "Phase Coupling (λ): 1.00",
        font_,
        0.0010f,
        glm::vec4(0.9f, 0.95f, 1.0f, 1.0f)
    );
    lambda_label_->transform.position = glm::vec3(-0.15f, 0.20f, 0.002f);
    menu_panel_->addChild(lambda_label_);

    lambda_dec_btn_ = std::make_shared<SpatialButton>(
        " - λ ",
        glm::vec2(0.18f, 0.07f),
        font_,
        [this]() {
            config_->phase_coupling_strength = std::max(0.0f, config_->phase_coupling_strength - 0.2f);
            onPhysicsChanged();
        }
    );
    lambda_dec_btn_->transform.position = glm::vec3(0.22f, 0.20f, 0.005f);
    menu_panel_->addChild(lambda_dec_btn_);

    lambda_inc_btn_ = std::make_shared<SpatialButton>(
        " + λ ",
        glm::vec2(0.18f, 0.07f),
        font_,
        [this]() {
            config_->phase_coupling_strength = std::min(5.0f, config_->phase_coupling_strength + 0.2f);
            onPhysicsChanged();
        }
    );
    lambda_inc_btn_->transform.position = glm::vec3(0.44f, 0.20f, 0.005f);
    menu_panel_->addChild(lambda_inc_btn_);

    // Phase Velocity Omega controls
    omega_label_ = std::make_shared<SpatialLabel>(
        "Phase Velocity (ω): 2.00 rad/s",
        font_,
        0.0010f,
        glm::vec4(0.9f, 0.95f, 1.0f, 1.0f)
    );
    omega_label_->transform.position = glm::vec3(-0.15f, 0.08f, 0.002f);
    menu_panel_->addChild(omega_label_);

    omega_dec_btn_ = std::make_shared<SpatialButton>(
        " - ω ",
        glm::vec2(0.18f, 0.07f),
        font_,
        [this]() {
            config_->phase_velocity -= 0.5f;
            onPhysicsChanged();
        }
    );
    omega_dec_btn_->transform.position = glm::vec3(0.22f, 0.08f, 0.005f);
    menu_panel_->addChild(omega_dec_btn_);

    omega_inc_btn_ = std::make_shared<SpatialButton>(
        " + ω ",
        glm::vec2(0.18f, 0.07f),
        font_,
        [this]() {
            config_->phase_velocity += 0.5f;
            onPhysicsChanged();
        }
    );
    omega_inc_btn_->transform.position = glm::vec3(0.44f, 0.08f, 0.005f);
    menu_panel_->addChild(omega_inc_btn_);

    // Acceleration Mode Selector
    mode_toggle_btn_ = std::make_shared<SpatialButton>(
        "Mode: LaserFocus",
        glm::vec2(0.50f, 0.08f),
        font_,
        [this]() {
            int current = static_cast<int>(config_->accel_mode);
            config_->accel_mode = static_cast<NovaMath::AccelerationMode>((current + 1) % 3);
            onPhysicsChanged();
        }
    );
    mode_toggle_btn_->transform.position = glm::vec3(-0.25f, -0.06f, 0.005f);
    menu_panel_->addChild(mode_toggle_btn_);

    // Sub-pixel Dither Toggle
    dither_toggle_btn_ = std::make_shared<SpatialButton>(
        "Dither: ON",
        glm::vec2(0.40f, 0.08f),
        font_,
        [this]() {
            config_->dither_enabled = !config_->dither_enabled;
            onPhysicsChanged();
        }
    );
    dither_toggle_btn_->transform.position = glm::vec3(0.26f, -0.06f, 0.005f);
    menu_panel_->addChild(dither_toggle_btn_);

    // Reset Defaults
    reset_btn_ = std::make_shared<SpatialButton>(
        "Reset Defaults",
        glm::vec2(0.42f, 0.08f),
        font_,
        [this]() {
            config_->resetDefaults();
            onPhysicsChanged();
        }
    );
    reset_btn_->normal_color = glm::vec4(0.28f, 0.15f, 0.20f, 0.90f);
    reset_btn_->transform.position = glm::vec3(0.0f, -0.22f, 0.005f);
    menu_panel_->addChild(reset_btn_);

    root_->addChild(menu_panel_);
    updateControlLabels();
}

void EngineHUD::toggleMenu() {
    setMenuVisible(!menu_visible_);
}

void EngineHUD::setMenuVisible(bool visible) {
    menu_visible_ = visible;
    if (menu_panel_) {
        menu_panel_->visible = menu_visible_;
    }
}

void EngineHUD::update(float) {
    updateTelemetry();
}

void EngineHUD::onPhysicsChanged() {
    updateControlLabels();
    updateTelemetry();
}

void EngineHUD::updateControlLabels() {
    if (!config_) return;

    if (lambda_label_) {
        std::ostringstream ss;
        ss << std::fixed << std::setprecision(2) << "Phase Coupling (λ): " << config_->phase_coupling_strength;
        lambda_label_->setText(ss.str());
    }

    if (omega_label_) {
        std::ostringstream ss;
        ss << std::fixed << std::setprecision(2) << "Phase Velocity (ω): " << config_->phase_velocity << " rad/s";
        omega_label_->setText(ss.str());
    }

    if (mode_toggle_btn_) {
        mode_toggle_btn_->label = std::string("Mode: ") + config_->getModeName();
    }

    if (dither_toggle_btn_) {
        dither_toggle_btn_->label = config_->dither_enabled ? "Dither: ON" : "Dither: OFF";
    }
}

void EngineHUD::updateTelemetry() {
    if (!config_) return;

    // Update bottom pill label
    if (pill_label_) {
        std::ostringstream ss;
        ss << std::fixed << std::setprecision(0)
           << "⚡ " << config_->current_fps << " FPS | "
           << config_->active_nodes << " Nodes | "
           << std::setprecision(1) << "λ=" << config_->phase_coupling_strength << " | "
           << config_->getModeName();
        pill_label_->setText(ss.str());
    }

    // Update menu telemetry label
    if (telemetry_label_ && menu_visible_) {
        std::ostringstream ss;
        ss << std::fixed << std::setprecision(1)
           << "FPS=" << config_->current_fps
           << " | Active Nodes=" << config_->active_nodes
           << " | Tests/Frame=" << config_->cluster_tests_per_frame
           << " | Dither=" << (config_->dither_enabled ? "ON" : "OFF");
        telemetry_label_->setText(ss.str());
    }
}

} // namespace Clouds
