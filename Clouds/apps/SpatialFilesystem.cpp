// Written by Richard Christopher, Copyright 2026 NeoTec Digital
#include "./SpatialFilesystem.h"
#include "Splash/Primitives.h"
#include "Splash/content/mesh_generators.h"
#include "Nova/components/logger.h"
#include <cmath>
#include <iostream>
#include <algorithm>

namespace Clouds {

// ---------------------------------------------------------------------------
// SpatialPillNode Implementation
// ---------------------------------------------------------------------------
SpatialPillNode::SpatialPillNode(Splash::Registry& reg, Splash::NodeId self,
                                 const std::string& name,
                                 const std::string& path,
                                 bool is_dir,
                                 uintmax_t size_bytes,
                                 std::shared_ptr<Splash::SpatialFont> font_ptr)
    : SpatialNode(reg, self), item_name(name), full_path(path), is_directory(is_dir),
      file_size(size_bytes), font_(font_ptr) {
    this->name = "3D Pill: " + item_name;

    if (is_directory) {
        pill_radius = 0.09f;
        pill_height = 0.28f;
        base_color = glm::vec4(0.15f, 0.55f, 0.90f, 0.95f); // Vibrant Blue/Cyan
    } else {
        pill_radius = 0.06f;
        pill_height = 0.18f;

        // Color by extension
        std::string ext = std::filesystem::path(path).extension().string();
        file_extension = ext;
        if (ext == ".cpp" || ext == ".c") base_color = glm::vec4(0.20f, 0.75f, 0.50f, 0.95f); // Green
        else if (ext == ".h" || ext == ".hpp") base_color = glm::vec4(0.85f, 0.65f, 0.20f, 0.95f); // Gold/Amber
        else if (ext == ".rs") base_color = glm::vec4(0.90f, 0.40f, 0.15f, 0.95f); // Rust Orange
        else if (ext == ".go") base_color = glm::vec4(0.20f, 0.70f, 0.85f, 0.95f); // Go Cyan
        else if (ext == ".spv" || ext == ".vert" || ext == ".frag") base_color = glm::vec4(0.75f, 0.30f, 0.85f, 0.95f); // Magenta
        else base_color = glm::vec4(0.60f, 0.65f, 0.75f, 0.90f); // Slate
    }

    size = glm::vec2(pill_radius * 2.0f, pill_height + pill_radius * 2.0f);

    if (font_) {
        const Splash::NodeId label = reg.emplace<Splash::SpatialLabel>(
            self, item_name, font_, 0.00045f, glm::vec4(0.95f, 0.98f, 1.0f, 1.0f));
        reg.transform(label).position =
            glm::vec3(0.0f, -pill_height * 0.5f - pill_radius - 0.035f, 0.002f);
    }
}

void SpatialPillNode::setSelected(bool sel) {
    is_selected = sel;
}

void SpatialPillNode::setHovered(bool hov) {
    is_hovered = hov;
}

void SpatialPillNode::onRayEnter(Splash::Registry&, Splash::NodeId, const Nova::Math::RayHit&) {
    setHovered(true);
}

void SpatialPillNode::onRayLeave(Splash::Registry&, Splash::NodeId) {
    setHovered(false);
}

void SpatialPillNode::onRayButton(Splash::Registry&, Splash::NodeId, const Nova::Math::RayHit&,
                                  uint32_t button, bool pressed) {
    if (button == 1 && pressed) {
        setSelected(true);
        if (on_select) {
            on_select(this);
        }
    }
}

void SpatialPillNode::update(Splash::Registry& reg, Splash::NodeId self, float dt) {
    phase_angle_ += dt * 1.5f;
    pulse_anim_ = 0.5f + 0.5f * std::sin(phase_angle_);

    // Subtle idle orientation oscillation
    glm::quat idle_rot = glm::angleAxis(std::sin(phase_angle_ * 0.5f) * 0.15f, glm::vec3(0.0f, 1.0f, 0.0f));
    reg.transform(self).orientation = idle_rot;
}

void SpatialPillNode::collectRender(Splash::Registry& reg, Splash::NodeId self,
                                    Nova::SpatialMeshBuffer* mesh_buf,
                                    std::vector<Splash::SpatialRenderCommand>& out_commands) {
    glm::vec4 active_color = base_color;
    if (is_selected) {
        active_color = glm::vec4(1.0f, 0.85f, 0.20f, 1.0f); // Glowing Gold
    } else if (is_hovered) {
        active_color = base_color + glm::vec4(0.2f, 0.2f, 0.2f, 0.0f);
    }

    glm::mat4 model = reg.worldOf(self).toMatrix();

    const float render_mode = is_selected ? 2.0f : (is_hovered ? 1.0f : 0.0f);

    uint32_t first_idx, idx_count;
    mesh_buf->append(resolveMesh(active_color, render_mode), first_idx, idx_count);

    out_commands.push_back({
        .model = model,
        .surface_dim = glm::vec4(size.x, size.y, pill_radius, pulse_anim_),
        .texture = nullptr,
        .first_index = first_idx,
        .index_count = idx_count
    });
}

const Nova::MeshData& SpatialPillNode::resolveMesh(const glm::vec4& color, float render_mode) {
    // Regenerated only when a value that actually feeds createPill() moves.
    // Every field in the key is public, so keying on the values rather than a
    // dirty flag keeps the cache correct under direct mutation.
    const Splash::MeshCache::Signature signature = {{
        pill_radius, pill_height,
        color.r, color.g, color.b, color.a,
        render_mode,
        static_cast<float>(RADIAL_SEGMENTS), static_cast<float>(CAP_RINGS)
    }};

    if (!mesh_cache_.isValidFor(signature)) {
        mesh_cache_.store(signature, Splash::PillMeshGenerator::createPill(
            pill_radius, pill_height, RADIAL_SEGMENTS, CAP_RINGS, color, render_mode));
    }

    return mesh_cache_.mesh();
}

// ---------------------------------------------------------------------------
// SpatialFilesystem Implementation
// ---------------------------------------------------------------------------
SpatialFilesystem::SpatialFilesystem(Splash::Registry& registry,
                                     Splash::NodeId root_scene_node,
                                     std::shared_ptr<Splash::SpatialFont> font_ptr)
    : registry_(registry), scene_root_(root_scene_node), font_(font_ptr) {
    filesystem_3d_root_ = registry_.createContainer(scene_root_);
    registry_[filesystem_3d_root_].name = "SpatialFilesystem_Root";
}

void SpatialFilesystem::rescan() {
    if (scan_root_.empty()) {
        report(LOGGER::ERROR, "SpatialFilesystem - Rescan requested before any scan root was configured");
        return;
    }
    scanAndBuild3DTree(scan_root_, scan_depth_);
}

void SpatialFilesystem::scanAndBuild3DTree(const std::string& root_path, int max_depth) {
    all_nodes_.clear();

    // The selection names a pill that is about to be destroyed. Clearing it here
    // is not tidiness: getSelectedNode() is read by the inspector every frame,
    // and a rescan that left it pointing into a freed pill would be handing that
    // reader a dangling pointer.
    selected_node_ = nullptr;
    for (const Splash::NodeId child : registry_.children(filesystem_3d_root_)) {
        registry_.destroy(child);
    }

    std::filesystem::path rpath(root_path);
    if (!std::filesystem::exists(rpath)) {
        report(LOGGER::ERROR, "SpatialFilesystem - Path does not exist: %s", root_path.c_str());
        return;
    }

    scan_root_ = root_path;
    scan_depth_ = max_depth;

    report(LOGGER::INFO, "SpatialFilesystem - Building 3D Pill Hierarchy for: %s", root_path.c_str());

    const Splash::NodeId root_pill = registry_.emplace<SpatialPillNode>(
        filesystem_3d_root_,
        rpath.filename().string().empty() ? rpath.string() : rpath.filename().string(),
        root_path, true, uintmax_t{0}, font_);

    const glm::vec3 root_position(0.0f, 0.0f, -0.3f);
    registry_.transform(root_pill).position = root_position;
    registry_.as<SpatialPillNode>(root_pill)->on_select =
        [this](SpatialPillNode* n) { selectNode(n); };

    all_nodes_.push_back(root_pill);

    buildSubTree(rpath, 1, max_depth, root_position, 1.15f);

    report(LOGGER::INFO, "SpatialFilesystem - Created %zu interactive 3D pills in Quaternionic space.", all_nodes_.size());
}

void SpatialFilesystem::buildSubTree(const std::filesystem::path& dir_path,
                                     int current_depth,
                                     int max_depth,
                                     glm::vec3 center_pos,
                                     float orbit_radius) {
    if (current_depth > max_depth) return;

    std::vector<std::filesystem::directory_entry> entries;
    try {
        for (const auto& entry : std::filesystem::directory_iterator(dir_path)) {
            std::string name = entry.path().filename().string();
            // Skip dotfiles and git
            if (!name.empty() && name[0] == '.') continue;
            if (name == "build" || name == "target" || name == ".git") continue;
            entries.push_back(entry);
        }
    } catch (const std::exception& e) {
        return;
    }

    if (entries.empty()) return;

    // Limit visible pills per ring to prevent clutter
    size_t count = std::min(entries.size(), size_t(12));
    float angle_step = glm::two_pi<float>() / float(count);

    for (size_t i = 0; i < count; ++i) {
        const auto& entry = entries[i];
        bool is_dir = entry.is_directory();
        uintmax_t sz = is_dir ? 0 : (entry.is_regular_file() ? entry.file_size() : 0);

        float angle = float(i) * angle_step;
        float height_offset = ((float(i % 3) - 1.0f) * 0.25f);
        glm::vec3 pill_pos = center_pos + glm::vec3(
            std::cos(angle) * orbit_radius,
            std::sin(angle) * orbit_radius * 0.75f + height_offset,
            -0.15f + float(current_depth) * 0.10f
        );

        // Every pill hangs off the one filesystem root, flat: the directory
        // structure is expressed by where a pill sits in space, not by the
        // scene graph, and the pill-to-pill links that used to be kept as well
        // were never read by anything.
        const Splash::NodeId pill = registry_.emplace<SpatialPillNode>(
            filesystem_3d_root_,
            entry.path().filename().string(), entry.path().string(), is_dir, sz, font_);
        registry_.transform(pill).position = pill_pos;
        registry_.as<SpatialPillNode>(pill)->on_select =
            [this](SpatialPillNode* n) { selectNode(n); };

        all_nodes_.push_back(pill);

        // Recurse into top directories
        if (is_dir && current_depth < max_depth) {
            buildSubTree(entry.path(), current_depth + 1, max_depth, pill_pos, orbit_radius * 0.45f);
        }
    }
}

void SpatialFilesystem::selectNode(SpatialPillNode* node) {
    if (selected_node_) {
        selected_node_->setSelected(false);
    }
    selected_node_ = node;
    if (selected_node_) {
        selected_node_->setSelected(true);
        report(LOGGER::INFO, "3D Filesystem Pill Selected: %s (Path: %s, Size: %ju bytes)",
               selected_node_->item_name.c_str(), selected_node_->full_path.c_str(), selected_node_->file_size);
    }
    if (on_node_selected) {
        on_node_selected(selected_node_);
    }
}

void SpatialFilesystem::update(float dt) {
    for (const Splash::NodeId id : all_nodes_) {
        if (SpatialPillNode* pill = registry_.as<SpatialPillNode>(id)) {
            pill->update(registry_, id, dt);
        }
    }
}

} // namespace Clouds
