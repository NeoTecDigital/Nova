// Written by Richard Christopher, Copyright 2026 NeoTec Digital
#pragma once

#include "Splash/Registry.h"
#include "Splash/SpatialNode.h"
#include "Nova/pipeline/mesh_buffer.h"
#include "Splash/content/mesh_cache.h"
#include "Splash/content/spatial_font.h"
#include <string>
#include <vector>
#include <memory>
#include <filesystem>
#include <functional>

namespace Clouds {

/**
 * SpatialPillNode - Interactive 3D Pill representing an entity in the 3D Filesystem
 */
class SpatialPillNode : public Splash::SpatialNode {
public:
    std::string item_name;
    std::string full_path;
    bool is_directory = false;
    uintmax_t file_size = 0;
    std::string file_extension;

    bool is_selected = false;
    // NOTE: is_hovered is inherited from SpatialNode - do not redeclare (base-member shadowing).

    float pill_radius = 0.07f;
    float pill_height = 0.22f;
    glm::vec4 base_color{0.2f, 0.5f, 0.85f, 0.95f};

    std::function<void(SpatialPillNode*)> on_select;

    // Tessellation is fixed for every pill: the mesh cache keys on it so the
    // two can never drift apart.
    static constexpr uint32_t RADIAL_SEGMENTS = 24;
    static constexpr uint32_t CAP_RINGS = 8;

    SpatialPillNode(Splash::Registry& reg, Splash::NodeId self,
                    const std::string& name,
                    const std::string& path,
                    bool is_dir,
                    uintmax_t size_bytes,
                    std::shared_ptr<Splash::SpatialFont> font_ptr);

    void setSelected(bool sel);
    void setHovered(bool hov);

    void onRayEnter(Splash::Registry& reg, Splash::NodeId self, const Nova::Math::RayHit& hit) override;
    void onRayLeave(Splash::Registry& reg, Splash::NodeId self) override;
    void onRayButton(Splash::Registry& reg, Splash::NodeId self, const Nova::Math::RayHit& hit,
                     uint32_t button, bool pressed) override;

    void update(Splash::Registry& reg, Splash::NodeId self, float dt);
    void collectRender(Splash::Registry& reg, Splash::NodeId self,
                       Nova::SpatialMeshBuffer* mesh_buf,
                       std::vector<Splash::SpatialRenderCommand>& out_commands) override;

private:
    std::shared_ptr<Splash::SpatialFont> font_;
    float phase_angle_ = 0.0f;
    float pulse_anim_ = 0.0f;

    // Pill geometry is static per (radius, height, colour, render mode). The
    // per-frame idle rotation and pulse ride on the model matrix and push
    // constants respectively, so neither touches vertex data.
    Splash::MeshCache mesh_cache_;

    const Nova::MeshData& resolveMesh(const glm::vec4& color, float render_mode);
};

/**
 * SpatialFilesystem - Constructs and manages the interactive 3D Hypergraph Filesystem
 */
class SpatialFilesystem {
public:
    SpatialFilesystem(Splash::Registry& registry,
                      Splash::NodeId root_scene_node,
                      std::shared_ptr<Splash::SpatialFont> font_ptr);
    ~SpatialFilesystem() = default;

    void scanAndBuild3DTree(const std::string& root_path, int max_depth = 2);
    void rescan();
    void update(float dt);

    Splash::Registry& registry() const { return registry_; }

    const std::string& getScanRoot() const { return scan_root_; }
    int getScanDepth() const { return scan_depth_; }

    SpatialPillNode* getSelectedNode() const { return selected_node_; }
    void selectNode(SpatialPillNode* node);

    // ONE index, in creation order. The pills used to be held three ways at
    // once - by this list, by a root_pill_ member and by each parent's own
    // children_pills - so "which of the three is authoritative" had no answer.
    // The tree is the registry's; this is only the iteration order the per-frame
    // animation walks.
    const std::vector<Splash::NodeId>& getAllNodes() const { return all_nodes_; }
    size_t getNodeCount() const { return all_nodes_.size(); }

    SpatialPillNode* resolve(Splash::NodeId id) const { return registry_.as<SpatialPillNode>(id); }

    std::function<void(SpatialPillNode*)> on_node_selected;

private:
    Splash::Registry& registry_;
    Splash::NodeId scene_root_;
    std::shared_ptr<Splash::SpatialFont> font_;
    Splash::NodeId filesystem_3d_root_;

    std::vector<Splash::NodeId> all_nodes_;
    SpatialPillNode* selected_node_ = nullptr;

    std::string scan_root_;
    int scan_depth_ = 2;

    void buildSubTree(const std::filesystem::path& dir_path,
                      int current_depth,
                      int max_depth,
                      glm::vec3 center_pos,
                      float orbit_radius);
};

} // namespace Clouds
