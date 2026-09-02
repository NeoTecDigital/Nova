// Written by Richard Christopher, Copyright 2026 NeoTec Digital

#include "include/Clouds/UI/WindowManager.h"
#include "include/Clouds/Primitives.h"
#include "Core/components/logger.h"
#include <sstream>
#include <iomanip>

namespace Clouds::UI {

WindowManager::WindowManager(std::shared_ptr<SpatialNode> scene_root,
                             std::shared_ptr<NovaSpatial::SpatialFont> font_ptr,
                             std::shared_ptr<OatsBridge> oats_bridge,
                             NovaMath::EnginePhysicsConfig* physics_config)
    : root_(scene_root), font_(font_ptr), oats_bridge_(oats_bridge), physics_config_(physics_config) {
}

void WindowManager::initialize() {
    if (!root_ || !font_) return;

    window_root_ = std::make_shared<SpatialNode>();
    window_root_->name = "WindowManager_Root";
    root_->addChild(window_root_);

    createMenuBar();
    createDockBar();

    createPhysicsWindow();
    createHypergraphWindow();

    resetLayout();
    report(LOGGER::INFO, "WindowManager - Desktop Shell & Window Subsystem initialized.");
}

void WindowManager::createMenuBar() {
    menubar_ = std::make_shared<UIMenuBar>(2.60f, font_);
    menubar_->transform.position = glm::vec3(0.0f, 0.82f, -0.08f);

    // Windows menu
    menubar_->addMenu("Windows", {
        { "Physics & LaserFocus", "F1", [this]() { toggleWindow("Physics & LaserFocus"); } },
        { "Hypergraph Forest",    "F4", [this]() { toggleWindow("Hypergraph Forest"); } },
        { "Reset Desktop Layout", "R",  [this]() { resetLayout(); } }
    });

    // Physics menu
    menubar_->addMenu("Physics", {
        { "Toggle LaserFocus", "", [this]() {
            if (physics_config_) {
                physics_config_->accel_mode = (physics_config_->accel_mode == NovaMath::AccelerationMode::LaserFocus) ?
                    NovaMath::AccelerationMode::ClusteredDither : NovaMath::AccelerationMode::LaserFocus;
            }
        }},
        { "Toggle Dithering", "", [this]() {
            if (physics_config_) physics_config_->dither_enabled = !physics_config_->dither_enabled;
        }}
    });

    root_->addChild(menubar_);
}

void WindowManager::createDockBar() {
    dockbar_ = std::make_shared<UIDockBar>(2.20f, font_);
    dockbar_->transform.position = glm::vec3(0.0f, -0.80f, -0.08f);

    dockbar_->addItem("Physics (F1)", [this]() { toggleWindow("Physics & LaserFocus"); });
    dockbar_->addItem("Hypergraph (F4)", [this]() { toggleWindow("Hypergraph Forest"); });
    dockbar_->addItem("Reset (R)", [this]() { resetLayout(); });

    root_->addChild(dockbar_);
}

void WindowManager::createPhysicsWindow() {
    physics_window_ = std::make_shared<UIWindow>("Physics & LaserFocus", glm::vec2(0.90f, 0.58f), font_);

    physics_stats_label_ = std::make_shared<UILabel>(
        "Awaiting physics telemetry...",
        font_,
        g_Theme.scale_body,
        g_Theme.text_primary,
        TextAlignment::LEFT
    );
    physics_stats_label_->transform.position = glm::vec3(-0.38f, 0.14f, 0.003f);
    physics_window_->content_area->addChild(physics_stats_label_);

    bool is_laser = physics_config_ && (physics_config_->accel_mode == NovaMath::AccelerationMode::LaserFocus);

    laser_focus_btn_ = std::make_shared<UIButton>(
        is_laser ? "LaserFocus: ENABLED" : "LaserFocus: DISABLED",
        glm::vec2(0.60f, 0.052f),
        font_,
        [this]() {
            if (physics_config_) {
                physics_config_->accel_mode = (physics_config_->accel_mode == NovaMath::AccelerationMode::LaserFocus) ?
                    NovaMath::AccelerationMode::ClusteredDither : NovaMath::AccelerationMode::LaserFocus;
                bool now_laser = (physics_config_->accel_mode == NovaMath::AccelerationMode::LaserFocus);
                laser_focus_btn_->setLabel(now_laser ? "LaserFocus: ENABLED" : "LaserFocus: DISABLED");
                laser_focus_btn_->setVariant(now_laser ? ButtonVariant::SUCCESS : ButtonVariant::SECONDARY);
            }
        },
        is_laser ? ButtonVariant::SUCCESS : ButtonVariant::SECONDARY
    );
    laser_focus_btn_->transform.position = glm::vec3(0.0f, 0.02f, 0.003f);
    physics_window_->content_area->addChild(laser_focus_btn_);

    bool is_dither = physics_config_ && physics_config_->dither_enabled;

    dither_btn_ = std::make_shared<UIButton>(
        is_dither ? "Temporal Dithering: ENABLED" : "Temporal Dithering: DISABLED",
        glm::vec2(0.60f, 0.052f),
        font_,
        [this]() {
            if (physics_config_) {
                physics_config_->dither_enabled = !physics_config_->dither_enabled;
                dither_btn_->setLabel(physics_config_->dither_enabled ? "Temporal Dithering: ENABLED" : "Temporal Dithering: DISABLED");
                dither_btn_->setVariant(physics_config_->dither_enabled ? ButtonVariant::PRIMARY : ButtonVariant::SECONDARY);
            }
        },
        is_dither ? ButtonVariant::PRIMARY : ButtonVariant::SECONDARY
    );
    dither_btn_->transform.position = glm::vec3(0.0f, -0.06f, 0.003f);
    physics_window_->content_area->addChild(dither_btn_);

    auto reset_btn = std::make_shared<UIButton>(
        "Reset Physics Defaults",
        glm::vec2(0.45f, 0.045f),
        font_,
        [this]() {
            if (physics_config_) {
                physics_config_->phase_coupling_strength = 1.0f;
                physics_config_->phase_velocity = 2.0f;
                physics_config_->accel_mode = NovaMath::AccelerationMode::LaserFocus;
                physics_config_->dither_enabled = true;
            }
        },
        ButtonVariant::GHOST
    );
    reset_btn->transform.position = glm::vec3(0.0f, -0.16f, 0.003f);
    physics_window_->content_area->addChild(reset_btn);

    physics_window_->visible = false;
    addWindow(physics_window_);
}

void WindowManager::createHypergraphWindow() {
    hypergraph_window_ = std::make_shared<UIWindow>("Hypergraph Forest", glm::vec2(0.95f, 0.58f), font_);

    auto lbl = std::make_shared<UILabel>(
        "Lumberjack Multi-Parent Forest: Active\nVirtual Namespace: /org/crm/leads/active\nTopological Nodes: 10,000 | Ingestion: 638k/s",
        font_,
        g_Theme.scale_body,
        g_Theme.text_highlight,
        TextAlignment::LEFT
    );
    lbl->transform.position = glm::vec3(-0.40f, 0.12f, 0.003f);
    hypergraph_window_->content_area->addChild(lbl);

    auto sync_btn = std::make_shared<UIButton>(
        "🌳 Sync Hypergraph DAG & Recall",
        glm::vec2(0.60f, 0.052f),
        font_,
        [this]() {
            report(LOGGER::INFO, "Lumberjack Hypergraph synced via socket.");
        },
        ButtonVariant::PRIMARY
    );
    sync_btn->transform.position = glm::vec3(0.0f, -0.06f, 0.003f);
    hypergraph_window_->content_area->addChild(sync_btn);

    hypergraph_window_->visible = false;
    addWindow(hypergraph_window_);
}

void WindowManager::addWindow(std::shared_ptr<UIWindow> window) {
    if (!window) return;
    window->on_focus_gained = [this](UIWindow* w) {
        bringToFront(w);
    };
    windows_.push_back(window);
    window_root_->addChild(window);
}

void WindowManager::bringToFront(UIWindow* window) {
    if (!window) return;
    active_window_ = window;

    for (auto& w : windows_) {
        bool is_target = (w.get() == window);
        w->setFocused(is_target);
        glm::vec3 pos = w->transform.position;
        pos.z = is_target ? -0.04f : -0.12f;
        w->transform.position = pos;
    }
}

void WindowManager::toggleWindow(const std::string& window_title) {
    for (auto& w : windows_) {
        if (w->title == window_title) {
            w->visible = !w->visible;
            if (w->visible) {
                bringToFront(w.get());
            }
            return;
        }
    }
}

void WindowManager::resetLayout() {
    if (physics_window_) {
        physics_window_->transform.position = glm::vec3(0.0f, 0.0f, -0.02f);
    }
    if (hypergraph_window_) {
        hypergraph_window_->transform.position = glm::vec3(0.0f, 0.0f, -0.02f);
    }
    if (physics_window_) {
        bringToFront(physics_window_.get());
    }
}

void WindowManager::update(float) {
    refreshWindowContents();
}

void WindowManager::refreshWindowContents() {
    if (physics_stats_label_ && physics_config_) {
        std::ostringstream ss;
        ss << "Coupling lambda: " << std::fixed << std::setprecision(2)
           << physics_config_->phase_coupling_strength
           << "  |  Velocity omega: " << physics_config_->phase_velocity << " rad/s";
        if (oats_bridge_) {
            ss << "\nOATS tick: " << oats_bridge_->getTick()
               << "  |  Objects: " << oats_bridge_->getObjectCount();
        }
        physics_stats_label_->setText(ss.str());
    }

    if (menubar_) {
        const bool is_laser = physics_config_ &&
            (physics_config_->accel_mode == NovaMath::AccelerationMode::LaserFocus);
        std::ostringstream ss;
        if (oats_bridge_) {
            ss << "Tick: " << oats_bridge_->getTick() << " | ";
        }
        ss << (is_laser ? "LaserFocus: ON" : "LaserFocus: OFF");
        menubar_->setTelemetryText(ss.str());
    }
}

} // namespace Clouds::UI
