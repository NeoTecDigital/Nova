// Written by Richard Christopher, Copyright 2026 NeoTec Digital
#include "../../include/Clouds/SpatialFilesystem.h"
#include "../../include/Clouds/Primitives.h"
#include "../../Core/components/logger.h"
#include <cmath>
#include <iostream>
#include <algorithm>

namespace Clouds {

// ---------------------------------------------------------------------------
// 3D Pill Mesh Generator Implementation
// ---------------------------------------------------------------------------
NovaSpatial::MeshData PillMeshGenerator::createPill(
    float radius,
    float cylinder_height,
    uint32_t radial_segments,
    uint32_t cap_rings,
    const glm::vec4& color,
    float render_mode
) {
    NovaSpatial::MeshData mesh;
    float half_h = cylinder_height * 0.5f;

    // 1. Top Hemisphere Vertices (Z from +half_h to +half_h + radius)
    for (uint32_t ring = 0; ring <= cap_rings; ++ring) {
        float phi = (float(ring) / float(cap_rings)) * (glm::pi<float>() * 0.5f); // 0 to pi/2
        float cos_phi = std::cos(phi);
        float sin_phi = std::sin(phi);

        for (uint32_t seg = 0; seg <= radial_segments; ++seg) {
            float theta = (float(seg) / float(radial_segments)) * glm::two_pi<float>();
            float cos_theta = std::cos(theta);
            float sin_theta = std::sin(theta);

            glm::vec3 normal(sin_phi * cos_theta, sin_phi * sin_theta, cos_phi);
            glm::vec3 pos = normal * radius + glm::vec3(0.0f, 0.0f, half_h);

            NovaSpatial::SpatialVertex v;
            v.state_primary = NovaMath::Hyper4(pos.x, pos.y, pos.z, 1.0f);
            v.state_dual = NovaMath::Hyper4(color.r, color.g, color.b, color.a);
            v.normal = normal;
            v.uv = glm::vec2(float(seg) / float(radial_segments), float(ring) / float(cap_rings * 2 + 1));
            v.params = glm::vec4(0.0f, 0.0f, render_mode, 1.0f);
            mesh.vertices.push_back(v);
        }
    }

    // 2. Bottom Hemisphere Vertices (Z from -half_h to -half_h - radius)
    for (uint32_t ring = 0; ring <= cap_rings; ++ring) {
        float phi = (glm::pi<float>() * 0.5f) + (float(ring) / float(cap_rings)) * (glm::pi<float>() * 0.5f); // pi/2 to pi
        float cos_phi = std::cos(phi);
        float sin_phi = std::sin(phi);

        for (uint32_t seg = 0; seg <= radial_segments; ++seg) {
            float theta = (float(seg) / float(radial_segments)) * glm::two_pi<float>();
            float cos_theta = std::cos(theta);
            float sin_theta = std::sin(theta);

            glm::vec3 normal(sin_phi * cos_theta, sin_phi * sin_theta, cos_phi);
            glm::vec3 pos = normal * radius - glm::vec3(0.0f, 0.0f, half_h);

            NovaSpatial::SpatialVertex v;
            v.state_primary = NovaMath::Hyper4(pos.x, pos.y, pos.z, 1.0f);
            v.state_dual = NovaMath::Hyper4(color.r, color.g, color.b, color.a);
            v.normal = normal;
            v.uv = glm::vec2(float(seg) / float(radial_segments), 0.5f + float(ring) / float(cap_rings * 2 + 1));
            v.params = glm::vec4(0.0f, 0.0f, render_mode, 1.0f);
            mesh.vertices.push_back(v);
        }
    }

    // Generate Triangles
    uint32_t ring_vertex_count = radial_segments + 1;
    uint32_t total_rings = (cap_rings + 1) * 2;

    for (uint32_t r = 0; r < total_rings - 1; ++r) {
        for (uint32_t s = 0; s < radial_segments; ++s) {
            uint32_t current = r * ring_vertex_count + s;
            uint32_t next = current + ring_vertex_count;

            mesh.indices.push_back(current);
            mesh.indices.push_back(next);
            mesh.indices.push_back(current + 1);

            mesh.indices.push_back(current + 1);
            mesh.indices.push_back(next);
            mesh.indices.push_back(next + 1);
        }
    }

    return mesh;
}

// ---------------------------------------------------------------------------
// SpatialPillNode Implementation
// ---------------------------------------------------------------------------
SpatialPillNode::SpatialPillNode(const std::string& name,
                                 const std::string& path,
                                 bool is_dir,
                                 uintmax_t size_bytes,
                                 std::shared_ptr<NovaSpatial::SpatialFont> font_ptr)
    : item_name(name), full_path(path), is_directory(is_dir), file_size(size_bytes), font_(font_ptr) {
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
        auto label_node = std::make_shared<SpatialLabel>(
            item_name,
            font_,
            0.00045f,
            glm::vec4(0.95f, 0.98f, 1.0f, 1.0f)
        );
        label_node->transform.position = glm::vec3(0.0f, -pill_height * 0.5f - pill_radius - 0.035f, 0.002f);
        addChild(label_node);
    }
}

void SpatialPillNode::setSelected(bool sel) {
    is_selected = sel;
}

void SpatialPillNode::setHovered(bool hov) {
    is_hovered = hov;
}

void SpatialPillNode::onRayEnter(const NovaMath::RayHit&) {
    setHovered(true);
}

void SpatialPillNode::onRayLeave() {
    setHovered(false);
}

void SpatialPillNode::onRayButton(const NovaMath::RayHit&, uint32_t button, bool pressed) {
    if (button == 1 && pressed) {
        setSelected(true);
        if (on_select) {
            on_select(this);
        }
    }
}

void SpatialPillNode::update(float dt) {
    phase_angle_ += dt * 1.5f;
    pulse_anim_ = 0.5f + 0.5f * std::sin(phase_angle_);

    // Subtle idle orientation oscillation
    glm::quat idle_rot = glm::angleAxis(std::sin(phase_angle_ * 0.5f) * 0.15f, glm::vec3(0.0f, 1.0f, 0.0f));
    transform.orientation = idle_rot;
}

void SpatialPillNode::collectRender(NovaSpatial::SpatialMeshBuffer* mesh_buf,
                                   std::vector<SpatialRenderCommand>& out_commands) {
    if (!visible) return;

    glm::vec4 active_color = base_color;
    if (is_selected) {
        active_color = glm::vec4(1.0f, 0.85f, 0.20f, 1.0f); // Glowing Gold
    } else if (is_hovered) {
        active_color = base_color + glm::vec4(0.2f, 0.2f, 0.2f, 0.0f);
    }

    NovaMath::QuatTransform world_xf = getWorldTransform();
    glm::mat4 model = world_xf.toMatrix();

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

    SpatialNode::collectRender(mesh_buf, out_commands);
}

const NovaSpatial::MeshData& SpatialPillNode::resolveMesh(const glm::vec4& color, float render_mode) {
    // Regenerated only when a value that actually feeds createPill() moves.
    // Every field in the key is public, so keying on the values rather than a
    // dirty flag keeps the cache correct under direct mutation.
    const NovaSpatial::MeshCache::Signature signature = {{
        pill_radius, pill_height,
        color.r, color.g, color.b, color.a,
        render_mode,
        static_cast<float>(RADIAL_SEGMENTS), static_cast<float>(CAP_RINGS)
    }};

    if (!mesh_cache_.isValidFor(signature)) {
        mesh_cache_.store(signature, PillMeshGenerator::createPill(
            pill_radius, pill_height, RADIAL_SEGMENTS, CAP_RINGS, color, render_mode));
    }

    return mesh_cache_.mesh();
}

// ---------------------------------------------------------------------------
// SpatialFilesystem Implementation
// ---------------------------------------------------------------------------
SpatialFilesystem::SpatialFilesystem(std::shared_ptr<SpatialNode> root_scene_node,
                                     std::shared_ptr<NovaSpatial::SpatialFont> font_ptr)
    : scene_root_(root_scene_node), font_(font_ptr) {
    filesystem_3d_root_ = std::make_shared<SpatialNode>();
    filesystem_3d_root_->name = "SpatialFilesystem_Root";
    scene_root_->addChild(filesystem_3d_root_);
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
    filesystem_3d_root_->children.clear();

    std::filesystem::path rpath(root_path);
    if (!std::filesystem::exists(rpath)) {
        report(LOGGER::ERROR, "SpatialFilesystem - Path does not exist: %s", root_path.c_str());
        return;
    }

    scan_root_ = root_path;
    scan_depth_ = max_depth;

    report(LOGGER::INFO, "SpatialFilesystem - Building 3D Pill Hierarchy for: %s", root_path.c_str());

    root_pill_ = std::make_shared<SpatialPillNode>(
        rpath.filename().string().empty() ? rpath.string() : rpath.filename().string(),
        root_path,
        true,
        0,
        font_
    );
    root_pill_->transform.position = glm::vec3(0.0f, 0.0f, -0.3f);
    root_pill_->on_select = [this](SpatialPillNode* n) { selectNode(n); };

    filesystem_3d_root_->addChild(root_pill_);
    all_nodes_.push_back(root_pill_);

    buildSubTree(rpath, root_pill_, 1, max_depth, root_pill_->transform.position, 1.15f);

    report(LOGGER::INFO, "SpatialFilesystem - Created %zu interactive 3D pills in Quaternionic space.", all_nodes_.size());
}

void SpatialFilesystem::buildSubTree(const std::filesystem::path& dir_path,
                                    std::shared_ptr<SpatialPillNode> parent_node,
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

        auto pill = std::make_shared<SpatialPillNode>(
            entry.path().filename().string(),
            entry.path().string(),
            is_dir,
            sz,
            font_
        );
        pill->transform.position = pill_pos;
        pill->parent_pill = parent_node;
        pill->on_select = [this](SpatialPillNode* n) { selectNode(n); };

        filesystem_3d_root_->addChild(pill);
        all_nodes_.push_back(pill);
        parent_node->children_pills.push_back(pill);

        // Recurse into top directories
        if (is_dir && current_depth < max_depth) {
            buildSubTree(entry.path(), pill, current_depth + 1, max_depth, pill_pos, orbit_radius * 0.45f);
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
    for (auto& node : all_nodes_) {
        node->update(dt);
    }
}

} // namespace Clouds
