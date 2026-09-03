// Written by Richard Christopher, Copyright 2026 NeoTec Digital
#pragma once

#include "./SpatialNode.h"
#include <functional>

namespace Splash {

/**
 * SpatialPanel - 3D Floating Glass/Acrylic UI Surface
 */
class SpatialPanel : public SpatialNode {
public:
    glm::vec4 background_color{0.10f, 0.12f, 0.18f, 0.85f};
    glm::vec4 border_color{0.25f, 0.30f, 0.45f, 0.90f};
    float corner_radius = 0.04f;
    float border_thickness = 0.005f;

    SpatialPanel(const glm::vec2& panel_size, const glm::vec4& bg_col = glm::vec4(0.10f, 0.12f, 0.18f, 0.85f));

    void collectRender(Nova::SpatialMeshBuffer* mesh_buf, std::vector<SpatialRenderCommand>& out_commands) override;

private:
    // Quad geometry is a pure function of the public fields above; the cache
    // keys on their values so mutating them invalidates it automatically.
    Nova::MeshCache quad_cache_;
};

/**
 * SpatialButton - Interactive 3D Button with Hover/Click Animations
 */
class SpatialButton : public SpatialNode {
public:
    std::string label;
    std::function<void()> on_click;

    glm::vec4 normal_color{0.18f, 0.22f, 0.35f, 0.95f};
    glm::vec4 hover_color{0.28f, 0.40f, 0.75f, 1.0f};
    glm::vec4 press_color{0.15f, 0.25f, 0.55f, 1.0f};
    glm::vec4 text_color{1.0f, 1.0f, 1.0f, 1.0f};

    float corner_radius = 0.02f;
    float border_thickness = 0.003f;
    std::shared_ptr<Nova::SpatialFont> font;

    SpatialButton(const std::string& btn_label,
                  const glm::vec2& btn_size,
                  std::shared_ptr<Nova::SpatialFont> spatial_font,
                  std::function<void()> click_handler = nullptr);

    void onRayEnter(const Nova::Math::RayHit& hit) override;
    void onRayLeave() override;
    void onRayButton(const Nova::Math::RayHit& hit, uint32_t button, bool pressed) override;

    void collectRender(Nova::SpatialMeshBuffer* mesh_buf, std::vector<SpatialRenderCommand>& out_commands) override;

private:
    float hover_anim_ = 0.0f;

    // Background quad and glyph run are cached independently: hovering recolours
    // the box every frame but leaves the far more expensive text mesh untouched.
    Nova::MeshCache box_cache_;
    Nova::MeshCache text_cache_;

    const Nova::MeshData& resolveBoxMesh(const glm::vec4& color);
    const Nova::MeshData& resolveTextMesh(float font_scale);
};

/**
 * SpatialLabel - 3D Text Display Element
 */
class SpatialLabel : public SpatialNode {
public:
    std::string text;
    glm::vec4 color{1.0f, 1.0f, 1.0f, 1.0f};
    float font_scale = 0.002f;
    bool center_aligned = true;
    std::shared_ptr<Nova::SpatialFont> font;

    SpatialLabel(const std::string& label_text,
                 std::shared_ptr<Nova::SpatialFont> spatial_font,
                 float scale = 0.002f,
                 const glm::vec4& col = glm::vec4(1.0f));

    void setText(const std::string& new_text);

    void collectRender(Nova::SpatialMeshBuffer* mesh_buf, std::vector<SpatialRenderCommand>& out_commands) override;

private:
    // Keyed on the glyph string as well as scale, colour and alignment.
    Nova::MeshCache text_cache_;
};

/**
 * SpatialSurfaceHost - Wayland Client Surface Host in 3D Space
 */
class SpatialSurfaceHost : public SpatialNode {
public:
    std::shared_ptr<Nova::TextureHandle> texture;
    float corner_radius = 0.015f;

    // Pointer focus is a seat-level fact, so the host reports both edges of it,
    // not just the motion in between. Enter carries the surface coordinate of
    // the sample that first landed here -- a press in that same sample is
    // routed by whatever the seat's focus is at that instant, so issuing enter
    // one motion sample later delivers the press to the previous surface.
    std::function<void(float u, float v)> on_surface_pointer_enter;
    std::function<void(float u, float v)> on_surface_pointer_motion;
    std::function<void()> on_surface_pointer_leave;
    std::function<void(uint32_t button, bool pressed)> on_surface_button;
    std::function<void(uint32_t key, bool pressed)> on_surface_key;

    SpatialSurfaceHost(const glm::vec2& surface_size, std::shared_ptr<Nova::TextureHandle> tex = nullptr);

    void setTexture(std::shared_ptr<Nova::TextureHandle> tex);

    void onRayEnter(const Nova::Math::RayHit& hit) override;
    void onRayMove(const Nova::Math::RayHit& hit) override;
    void onRayLeave() override;
    void onRayButton(const Nova::Math::RayHit& hit, uint32_t button, bool pressed) override;
    void onKey(uint32_t key, bool pressed) override;

    void collectRender(Nova::SpatialMeshBuffer* mesh_buf, std::vector<SpatialRenderCommand>& out_commands) override;

private:
    // The hosted texture changes every frame; the quad it is sampled onto does
    // not, and is only rebuilt when size or corner radius moves.
    Nova::MeshCache quad_cache_;
};

} // namespace Splash
