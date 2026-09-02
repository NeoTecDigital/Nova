// Written by Richard Christopher, Copyright 2026 NeoTec Digital

#include "include/Clouds/CrmWorkspace.h"
#include "Core/components/logger.h"
#include <sstream>

namespace Clouds {

CrmWorkspace::CrmWorkspace(std::shared_ptr<SpatialNode> root_node,
                           std::shared_ptr<NovaSpatial::SpatialFont> font,
                           std::shared_ptr<OatsBridge> oats_bridge)
    : root_(root_node), font_(font), oats_bridge_(oats_bridge) {
}

void CrmWorkspace::initialize() {
    if (!root_ || !font_ || !oats_bridge_) return;

    main_container_ = std::make_shared<SpatialPanel>(
        glm::vec2(2.8f, 0.55f),
        glm::vec4(0.04f, 0.06f, 0.10f, 0.75f)
    );
    main_container_->corner_radius = 0.05f;
    main_container_->border_thickness = 0.004f;
    main_container_->border_color = glm::vec4(0.20f, 0.35f, 0.60f, 0.85f);
    main_container_->transform.position = glm::vec3(0.0f, 0.10f, -0.20f);

    header_title_ = std::make_shared<SpatialLabel>(
        ":: OATS-rs RUNTIME ::",
        font_,
        0.0013f,
        glm::vec4(0.40f, 0.75f, 1.0f, 1.0f)
    );
    header_title_->transform.position = glm::vec3(0.0f, 0.18f, 0.003f);
    main_container_->addChild(header_title_);

    orchestrator_status_ = std::make_shared<SpatialLabel>(
        "Awaiting first runtime tick...",
        font_,
        0.00095f,
        glm::vec4(0.65f, 0.80f, 0.95f, 0.90f)
    );
    orchestrator_status_->transform.position = glm::vec3(0.0f, 0.10f, 0.003f);
    main_container_->addChild(orchestrator_status_);

    buildEventLogPanel();
    main_container_->addChild(log_panel_);
    root_->addChild(main_container_);

    refreshDisplay();
}

void CrmWorkspace::buildEventLogPanel() {
    log_panel_ = std::make_shared<SpatialPanel>(
        glm::vec2(2.65f, 0.14f),
        glm::vec4(0.03f, 0.05f, 0.08f, 0.92f)
    );
    log_panel_->corner_radius = 0.025f;
    log_panel_->border_thickness = 0.002f;
    log_panel_->transform.position = glm::vec3(0.0f, -0.12f, 0.005f);

    log_label_ = std::make_shared<SpatialLabel>(
        "No runtime events yet.",
        font_,
        0.00085f,
        glm::vec4(0.70f, 0.85f, 0.70f, 0.95f)
    );
    log_label_->transform.position = glm::vec3(0.0f, 0.0f, 0.002f);
    log_panel_->addChild(log_label_);
}

void CrmWorkspace::setVisible(bool visible) {
    visible_ = visible;
    if (main_container_) {
        main_container_->visible = visible_;
    }
}

void CrmWorkspace::update(float dt) {
    if (!visible_ || !oats_bridge_) return;

    oats_bridge_->step(dt);
    refreshDisplay();
}

void CrmWorkspace::refreshDisplay() {
    if (!oats_bridge_) return;

    if (orchestrator_status_) {
        std::ostringstream ss;
        ss << "Tick: " << oats_bridge_->getTick()
           << " | Objects: " << oats_bridge_->getObjectCount()
           << " | Types: " << oats_bridge_->getRegisteredTypeNames().size();
        if (!oats_bridge_->isHealthy()) {
            ss << " | ERROR: " << oats_bridge_->getLastError();
        }
        orchestrator_status_->setText(ss.str());
    }

    const auto& events = oats_bridge_->getRecentEvents();
    if (!events.empty() && log_label_) {
        const auto& latest = events.back();
        log_label_->setText("[" + latest.type + "] " + latest.message);
    }
}

} // namespace Clouds
