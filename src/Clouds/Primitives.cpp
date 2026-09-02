// Written by Richard Christopher, Copyright 2026 NeoTec Digital
#include "include/Clouds/Primitives.h"

namespace Clouds {
namespace {

// Render modes consumed by the spatial fragment shader.
constexpr float PROCEDURAL_SDF_MODE = 0.0f;
constexpr float TEXTURED_QUAD_MODE = 1.0f;

// Glyph atlas is rasterised at 48px; button text occupies 42% of button height.
constexpr float FONT_PIXEL_HEIGHT = 48.0f;
constexpr float BUTTON_TEXT_HEIGHT_RATIO = 0.42f;

} // namespace

SpatialPanel::SpatialPanel(const glm::vec2& panel_size, const glm::vec4& bg_col) {
    size = panel_size;
    background_color = bg_col;
    name = "SpatialPanel";
}

void SpatialPanel::collectRender(NovaSpatial::SpatialMeshBuffer* mesh_buf,
                                 std::vector<SpatialRenderCommand>& out_commands) {
    if (!visible) return;

    NovaMath::QuatTransform world_xf = getWorldTransform();
    glm::mat4 model = world_xf.toMatrix();

    // Procedural panel quad (mode 0.0), rebuilt only when its inputs move.
    const NovaSpatial::MeshCache::Signature signature = {{
        size.x, size.y,
        background_color.r, background_color.g, background_color.b, background_color.a,
        border_thickness, corner_radius, PROCEDURAL_SDF_MODE
    }};

    if (!quad_cache_.isValidFor(signature)) {
        quad_cache_.store(signature, NovaSpatial::SpatialMeshGenerator::createPlanarQuad(
            size, background_color, border_thickness, corner_radius, PROCEDURAL_SDF_MODE));
    }

    uint32_t first_idx, idx_count;
    mesh_buf->append(quad_cache_.mesh(), first_idx, idx_count);

    out_commands.push_back({
        .model = model,
        .surface_dim = glm::vec4(size.x, size.y, size.x / std::max(size.y, 0.001f), 0.0f),
        .texture = nullptr,
        .first_index = first_idx,
        .index_count = idx_count
    });

    // Collect children on top
    SpatialNode::collectRender(mesh_buf, out_commands);
}

SpatialButton::SpatialButton(const std::string& btn_label,
                             const glm::vec2& btn_size,
                             std::shared_ptr<NovaSpatial::SpatialFont> spatial_font,
                             std::function<void()> click_handler)
    : label(btn_label), on_click(click_handler), font(spatial_font) {
    size = btn_size;
    name = "SpatialButton: " + label;
}

void SpatialButton::onRayEnter(const NovaMath::RayHit& hit) {
    SpatialNode::onRayEnter(hit);
}

void SpatialButton::onRayLeave() {
    SpatialNode::onRayLeave();
}

void SpatialButton::onRayButton(const NovaMath::RayHit& hit, uint32_t button, bool pressed) {
    SpatialNode::onRayButton(hit, button, pressed);
    if (button == 1 && !pressed && is_hovered) { // Button release
        if (on_click) {
            on_click();
        }
    }
}

void SpatialButton::collectRender(NovaSpatial::SpatialMeshBuffer* mesh_buf,
                                  std::vector<SpatialRenderCommand>& out_commands) {
    if (!visible) return;

    NovaMath::QuatTransform world_xf = getWorldTransform();
    glm::mat4 model = world_xf.toMatrix();

    // Determine current visual color based on interaction state
    glm::vec4 current_color = normal_color;
    if (is_pressed) {
        current_color = press_color;
    } else if (is_hovered) {
        current_color = hover_color;
    }

    // 1. Button Background Box
    uint32_t box_first_idx, box_idx_count;
    mesh_buf->append(resolveBoxMesh(current_color), box_first_idx, box_idx_count);

    out_commands.push_back({
        .model = model,
        .surface_dim = glm::vec4(size.x, size.y, size.x / std::max(size.y, 0.001f), 0.0f),
        .texture = nullptr,
        .first_index = box_first_idx,
        .index_count = box_idx_count
    });

    // 2. Button Text Label
    if (font && !label.empty()) {
        const float font_scale = (size.y * BUTTON_TEXT_HEIGHT_RATIO) / FONT_PIXEL_HEIGHT;

        uint32_t text_first_idx, text_idx_count;
        mesh_buf->append(resolveTextMesh(font_scale), text_first_idx, text_idx_count);

        out_commands.push_back({
            .model = model,
            .surface_dim = glm::vec4(size.x, size.y, 1.0f, 0.0f),
            .texture = font->getAtlasTexture(),
            .first_index = text_first_idx,
            .index_count = text_idx_count
        });
    }

    // Collect children
    SpatialNode::collectRender(mesh_buf, out_commands);
}

const NovaSpatial::MeshData& SpatialButton::resolveBoxMesh(const glm::vec4& color) {
    // Only the colour moves under interaction, so the cache turns over on state
    // change rather than every frame.
    const NovaSpatial::MeshCache::Signature signature = {{
        size.x, size.y,
        color.r, color.g, color.b, color.a,
        border_thickness, corner_radius, PROCEDURAL_SDF_MODE
    }};

    if (!box_cache_.isValidFor(signature)) {
        box_cache_.store(signature, NovaSpatial::SpatialMeshGenerator::createPlanarQuad(
            size, color, border_thickness, corner_radius, PROCEDURAL_SDF_MODE));
    }

    return box_cache_.mesh();
}

const NovaSpatial::MeshData& SpatialButton::resolveTextMesh(float font_scale) {
    const NovaSpatial::MeshCache::Signature signature = {{
        font_scale,
        text_color.r, text_color.g, text_color.b, text_color.a,
        1.0f // centre-aligned
    }};

    if (!text_cache_.isValidFor(signature, label)) {
        text_cache_.store(signature, label,
                          font->createTextMesh(label, font_scale, text_color, true));
    }

    return text_cache_.mesh();
}

SpatialLabel::SpatialLabel(const std::string& label_text,
                           std::shared_ptr<NovaSpatial::SpatialFont> spatial_font,
                           float scale,
                           const glm::vec4& col)
    : text(label_text), color(col), font_scale(scale), font(spatial_font) {
    name = "SpatialLabel: " + text;
    if (font) {
        size = font->measureText(text, font_scale);
    }
}

void SpatialLabel::setText(const std::string& new_text) {
    text = new_text;
    if (font) {
        size = font->measureText(text, font_scale);
    }
}

void SpatialLabel::collectRender(NovaSpatial::SpatialMeshBuffer* mesh_buf,
                                 std::vector<SpatialRenderCommand>& out_commands) {
    if (!visible || !font || text.empty()) return;

    NovaMath::QuatTransform world_xf = getWorldTransform();
    glm::mat4 model = world_xf.toMatrix();

    const NovaSpatial::MeshCache::Signature signature = {{
        font_scale,
        color.r, color.g, color.b, color.a,
        center_aligned ? 1.0f : 0.0f
    }};

    if (!text_cache_.isValidFor(signature, text)) {
        text_cache_.store(signature, text,
                          font->createTextMesh(text, font_scale, color, center_aligned));
    }

    uint32_t first_idx, idx_count;
    mesh_buf->append(text_cache_.mesh(), first_idx, idx_count);

    out_commands.push_back({
        .model = model,
        .surface_dim = glm::vec4(size.x, size.y, 1.0f, 0.0f),
        .texture = font->getAtlasTexture(),
        .first_index = first_idx,
        .index_count = idx_count
    });

    SpatialNode::collectRender(mesh_buf, out_commands);
}

SpatialSurfaceHost::SpatialSurfaceHost(const glm::vec2& surface_size, std::shared_ptr<NovaSpatial::TextureHandle> tex)
    : texture(tex) {
    size = surface_size;
    name = "SpatialSurfaceHost";
}

void SpatialSurfaceHost::setTexture(std::shared_ptr<NovaSpatial::TextureHandle> tex) {
    texture = tex;
}

void SpatialSurfaceHost::onRayEnter(const NovaMath::RayHit& hit) {
    SpatialNode::onRayEnter(hit);
    if (on_surface_pointer_enter) {
        on_surface_pointer_enter(hit.uv.x, hit.uv.y);
    }
}

void SpatialSurfaceHost::onRayMove(const NovaMath::RayHit& hit) {
    SpatialNode::onRayMove(hit);
    if (on_surface_pointer_motion) {
        on_surface_pointer_motion(hit.uv.x, hit.uv.y);
    }
}

void SpatialSurfaceHost::onRayLeave() {
    SpatialNode::onRayLeave();
    if (on_surface_pointer_leave) {
        on_surface_pointer_leave();
    }
}

void SpatialSurfaceHost::onRayButton(const NovaMath::RayHit& hit, uint32_t button, bool pressed) {
    SpatialNode::onRayButton(hit, button, pressed);
    if (on_surface_button) {
        on_surface_button(button, pressed);
    }
}

void SpatialSurfaceHost::onKey(uint32_t key, bool pressed) {
    SpatialNode::onKey(key, pressed);
    if (on_surface_key) {
        on_surface_key(key, pressed);
    }
}

void SpatialSurfaceHost::collectRender(NovaSpatial::SpatialMeshBuffer* mesh_buf,
                                       std::vector<SpatialRenderCommand>& out_commands) {
    if (!visible) return;

    NovaMath::QuatTransform world_xf = getWorldTransform();
    glm::mat4 model = world_xf.toMatrix();

    // Geometry depends only on size and corner radius; the texture it samples is
    // swapped through the render command, not the vertex data.
    const NovaSpatial::MeshCache::Signature signature = {{
        size.x, size.y,
        1.0f, 1.0f, 1.0f, 1.0f,
        0.0f, corner_radius, TEXTURED_QUAD_MODE
    }};

    if (!quad_cache_.isValidFor(signature)) {
        quad_cache_.store(signature, NovaSpatial::SpatialMeshGenerator::createPlanarQuad(
            size, glm::vec4(1.0f), 0.0f, corner_radius, TEXTURED_QUAD_MODE));
    }

    uint32_t first_idx, idx_count;
    mesh_buf->append(quad_cache_.mesh(), first_idx, idx_count);

    out_commands.push_back({
        .model = model,
        .surface_dim = glm::vec4(size.x, size.y, size.x / std::max(size.y, 0.001f), 0.0f),
        .texture = texture,
        .first_index = first_idx,
        .index_count = idx_count
    });

    SpatialNode::collectRender(mesh_buf, out_commands);
}

} // namespace Clouds
