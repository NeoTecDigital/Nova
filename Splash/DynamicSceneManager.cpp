// Written by Richard Christopher, Copyright 2026 NeoTec Digital

#include "./DynamicSceneManager.h"
#include "Nova/components/logger.h"
#include <vector>

namespace Splash {
namespace {

// Presentation constants. These describe how a pill is drawn, not what it contains.
constexpr float kPanelCornerRadius = 0.035f;
constexpr float kPanelBorderThickness = 0.003f;
constexpr float kLabelScale = 0.0010f;
const glm::vec4 kPanelColor(0.10f, 0.16f, 0.28f, 0.90f);
const glm::vec4 kButtonNormal(0.08f, 0.12f, 0.20f, 0.40f);
const glm::vec4 kButtonHover(0.20f, 0.35f, 0.65f, 0.85f);
const glm::vec4 kButtonPress(0.35f, 0.55f, 0.90f, 1.00f);
const glm::vec4 kLabelColor(0.95f, 0.98f, 1.00f, 1.00f);

// A pill's panel footprint follows its parametric capsule dimensions.
glm::vec2 panelSizeFor(const OatsSpatialPill& pill) {
    return glm::vec2(pill.radius * 2.0f, pill.height);
}

} // namespace

DynamicSceneManager::DynamicSceneManager(Registry& registry,
                                         NodeId root_node,
                                         std::shared_ptr<Splash::SpatialFont> font,
                                         std::shared_ptr<OatsBridge> oats_bridge)
    : registry_(registry), root_(root_node), font_(font), oats_bridge_(oats_bridge) {
}

void DynamicSceneManager::initialize() {
    dynamic_root_ = registry_.createContainer(root_);
    registry_[dynamic_root_].name = "OatsSpatialPillRoot";
    syncNodesFromState();
}

void DynamicSceneManager::update(float dt) {
    (void)dt;
    syncNodesFromState();
}

std::string DynamicSceneManager::spawnPill(const std::string& name, const glm::vec3& position) {
    if (!oats_bridge_) {
        report(LOGGER::ERROR, "DynamicSceneManager - cannot spawn '%s', no OATS bridge", name.c_str());
        return {};
    }
    const std::string id = oats_bridge_->registerSpatialPill(
        name, name, position, glm::vec4(0.0f, 0.0f, 0.0f, 1.0f), 0.07f, 0.22f);
    if (id.empty()) {
        report(LOGGER::ERROR, "DynamicSceneManager - runtime rejected pill '%s': %s",
               name.c_str(), oats_bridge_->getLastError().c_str());
        return {};
    }
    report(LOGGER::INFO, "DynamicSceneManager - registered SpatialPill '%s' (id %s)",
           name.c_str(), id.c_str());
    return id;
}

void DynamicSceneManager::syncNodesFromState() {
    if (!oats_bridge_ || !registry_.alive(dynamic_root_)) {
        return;
    }
    removeStaleNodes();

    for (const auto& [id, pill] : oats_bridge_->getSpatialPills()) {
        const auto it = active_nodes_.find(id);
        if (it == active_nodes_.end()) {
            createNodeFor(pill);
        } else {
            updateNodeFor(it->second, pill);
        }
    }
}

void DynamicSceneManager::createNodeFor(const OatsSpatialPill& pill) {
    const glm::vec2 size = panelSizeFor(pill);

    Active3DNode node3d;
    node3d.id = pill.id;
    node3d.panel = registry_.emplace<SpatialPanel>(dynamic_root_, size, kPanelColor);

    SpatialPanel& panel = static_cast<SpatialPanel&>(registry_[node3d.panel]);
    panel.corner_radius = kPanelCornerRadius;
    panel.border_thickness = kPanelBorderThickness;
    registry_.transform(node3d.panel).position = pill.position;

    const std::string activated_id = pill.id;
    node3d.button = registry_.emplace<SpatialButton>(
        node3d.panel,
        pill.name,
        glm::vec2(size.x - 0.04f, size.y - 0.04f),
        font_,
        std::function<void()>([this, activated_id]() {
            if (on_node_activated) {
                on_node_activated(activated_id);
            }
        })
    );
    SpatialButton& button = static_cast<SpatialButton&>(registry_[node3d.button]);
    button.normal_color = kButtonNormal;
    button.hover_color = kButtonHover;
    button.press_color = kButtonPress;

    node3d.label = registry_.emplace<SpatialLabel>(node3d.button, pill.name, font_,
                                                   kLabelScale, kLabelColor);
    registry_.transform(node3d.label).position = glm::vec3(0.0f, 0.0f, 0.003f);

    active_nodes_[pill.id] = std::move(node3d);
}

void DynamicSceneManager::updateNodeFor(Active3DNode& node, const OatsSpatialPill& pill) {
    if (registry_.alive(node.panel)) {
        registry_.transform(node.panel).position = pill.position;
        registry_[node.panel].size = panelSizeFor(pill);
    }
    if (SpatialLabel* label = registry_.as<SpatialLabel>(node.label)) {
        label->setText(pill.name);
    }
}

void DynamicSceneManager::removeStaleNodes() {
    // Authoritative removal channel: ids the runtime reported as deleted (organizer.rs:96).
    for (const std::string& removed_id : oats_bridge_->drainRemovedObjectIds()) {
        destroyNode(removed_id);
    }

    // Safety net for full snapshot reloads, which replace the caches wholesale.
    const auto& pills = oats_bridge_->getSpatialPills();
    std::vector<std::string> orphaned;
    for (const auto& [id, node] : active_nodes_) {
        if (pills.find(id) == pills.end()) {
            orphaned.push_back(id);
        }
    }
    for (const std::string& id : orphaned) {
        destroyNode(id);
    }
}

void DynamicSceneManager::destroyNode(const std::string& id) {
    const auto it = active_nodes_.find(id);
    if (it == active_nodes_.end()) {
        return;
    }
    // The panel owns the button which owns the label, so destroying the panel
    // takes the whole representation with it.
    registry_.destroy(it->second.panel);
    active_nodes_.erase(it);
}

} // namespace Splash
