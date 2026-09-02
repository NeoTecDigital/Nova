// Written by Richard Christopher, Copyright 2026 NeoTec Digital
#pragma once

#include "./SpatialNode.h"
#include "./Primitives.h"
#include "./OatsBridge.h"
#include "Core/modules/spatial_pipeline/spatial_font.h"
#include <memory>
#include <unordered_map>
#include <string>
#include <functional>

namespace Clouds {

struct Active3DNode {
    std::string id;
    std::shared_ptr<SpatialPanel> panel;
    std::shared_ptr<SpatialButton> button;
    std::shared_ptr<SpatialLabel> label;
};

/**
 * DynamicSceneManager - mirrors the OATS SpatialPill population into 3D scene nodes.
 *
 * Additions and pose changes arrive through the bridge caches; removals arrive through
 * OatsBridge::drainRemovedObjectIds so deleted runtime objects tear down their 3D
 * representation instead of leaking it.
 */
class DynamicSceneManager {
public:
    DynamicSceneManager(std::shared_ptr<SpatialNode> root_node,
                        std::shared_ptr<NovaSpatial::SpatialFont> font,
                        std::shared_ptr<OatsBridge> oats_bridge);
    ~DynamicSceneManager() = default;

    void initialize();
    void update(float dt);

    // Registers a SpatialPill with the runtime and returns its object id (empty on failure).
    std::string spawnPill(const std::string& name, const glm::vec3& position);

    // Invoked with the OATS object id when a pill's 3D button is triggered. The OATS FFI
    // exposes no interaction endpoint, so activation is dispatched to the host application.
    std::function<void(const std::string&)> on_node_activated;

private:
    std::shared_ptr<SpatialNode> root_;
    std::shared_ptr<NovaSpatial::SpatialFont> font_;
    std::shared_ptr<OatsBridge> oats_bridge_;

    std::shared_ptr<SpatialNode> dynamic_root_;
    std::unordered_map<std::string, Active3DNode> active_nodes_;


    void syncNodesFromState();
    void createNodeFor(const OatsSpatialPill& pill);
    void updateNodeFor(Active3DNode& node, const OatsSpatialPill& pill);
    void removeStaleNodes();
    void destroyNode(const std::string& id);
};

} // namespace Clouds
