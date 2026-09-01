// Written by Richard Christopher, Copyright 2026 NeoTec Digital

#include "../../include/Clouds/DynamicSceneManager.h"
#include "../../Core/components/logger.h"
#include <vector>

namespace Clouds {
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

DynamicSceneManager::DynamicSceneManager(std::shared_ptr<SpatialNode> root_node,
                                         std::shared_ptr<NovaSpatial::SpatialFont> font,
                                         std::shared_ptr<OatsBridge> oats_bridge)
    : root_(root_node), font_(font), oats_bridge_(oats_bridge) {
}

void DynamicSceneManager::initialize() {
    dynamic_root_ = std::make_shared<SpatialNode>();
    dynamic_root_->name = "OatsSpatialPillRoot";
    root_->addChild(dynamic_root_);
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
    if (!oats_bridge_ || !dynamic_root_) {
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
    node3d.panel = std::make_shared<SpatialPanel>(size, kPanelColor);
    node3d.panel->corner_radius = kPanelCornerRadius;
    node3d.panel->border_thickness = kPanelBorderThickness;
    node3d.panel->transform.position = pill.position;

    const std::string activated_id = pill.id;
    node3d.button = std::make_shared<SpatialButton>(
        pill.name,
        glm::vec2(size.x - 0.04f, size.y - 0.04f),
        font_,
        [this, activated_id]() {
            if (on_node_activated) {
                on_node_activated(activated_id);
            }
        }
    );
    node3d.button->normal_color = kButtonNormal;
    node3d.button->hover_color = kButtonHover;
    node3d.button->press_color = kButtonPress;

    node3d.label = std::make_shared<SpatialLabel>(pill.name, font_, kLabelScale, kLabelColor);
    node3d.label->transform.position = glm::vec3(0.0f, 0.0f, 0.003f);

    node3d.button->addChild(node3d.label);
    node3d.panel->addChild(node3d.button);
    dynamic_root_->addChild(node3d.panel);

    active_nodes_[pill.id] = std::move(node3d);
}

void DynamicSceneManager::updateNodeFor(Active3DNode& node, const OatsSpatialPill& pill) {
    if (node.panel) {
        node.panel->transform.position = pill.position;
        node.panel->size = panelSizeFor(pill);
    }
    if (node.label) {
        node.label->setText(pill.name);
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
    if (dynamic_root_ && it->second.panel) {
        dynamic_root_->removeChild(it->second.panel);
    }
    active_nodes_.erase(it);
}

} // namespace Clouds
