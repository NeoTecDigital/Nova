// Written by Richard Christopher, Copyright 2026 NeoTec Digital
#pragma once

#include "./SpatialNode.h"
#include "./Primitives.h"
#include "./OatsBridge.h"
#include "Core/modules/spatial_pipeline/spatial_font.h"
#include <memory>
#include <vector>

namespace Clouds {

/**
 * CrmWorkspace - 3D workspace banner for the OATS runtime.
 *
 * The lead/deal/campaign panels were removed with the CRM FFI surface: OATS-ffi exposes
 * no such objects. What remains is bound to data the Rust runtime actually emits
 * (runtime tick, object population, system events). This file is not currently part of
 * the Clouds build and is pending re-evaluation.
 */
class CrmWorkspace {
public:
    CrmWorkspace(std::shared_ptr<SpatialNode> root_node,
                 std::shared_ptr<NovaSpatial::SpatialFont> font,
                 std::shared_ptr<OatsBridge> oats_bridge);
    ~CrmWorkspace() = default;

    void initialize();
    void update(float dt);

    void setVisible(bool visible);
    bool isVisible() const { return visible_; }
    void toggleVisible() { setVisible(!visible_); }

private:
    std::shared_ptr<SpatialNode> root_;
    std::shared_ptr<NovaSpatial::SpatialFont> font_;
    std::shared_ptr<OatsBridge> oats_bridge_;

    bool visible_ = true;

    std::shared_ptr<SpatialPanel> main_container_;
    std::shared_ptr<SpatialLabel> header_title_;
    std::shared_ptr<SpatialLabel> orchestrator_status_;

    // Event Log Banner
    std::shared_ptr<SpatialPanel> log_panel_;
    std::shared_ptr<SpatialLabel> log_label_;

    void buildEventLogPanel();
    void refreshDisplay();
};

} // namespace Clouds
