#pragma once

#include "./spatial_mesh.h"
#include "./texture_bridge.h"
#include <ft2build.h>
#include FT_FREETYPE_H
#include <unordered_map>
#include <string>
#include <memory>

namespace NovaSpatial {

struct GlyphMetric {
    float advance_x = 0.0f;
    float width = 0.0f;
    float height = 0.0f;
    float bearing_x = 0.0f;
    float bearing_y = 0.0f;
    glm::vec2 uv_min{0.0f};
    glm::vec2 uv_max{0.0f};
};

class SpatialFont {
public:
    SpatialFont(NovaCore* core, TextureBridge* texture_bridge);
    ~SpatialFont();

    // Load font from TTF file path at specified pixel height
    bool loadFromFile(const std::string& ttf_path, uint32_t pixel_height = 48);

    // Generate 3D quad mesh for a text string positioned in local coordinates
    MeshData createTextMesh(const std::string& text,
                            float font_scale = 0.0015f,
                            const glm::vec4& color = glm::vec4(1.0f),
                            bool center_aligned = true);

    // Measure text dimensions (width, height in local units)
    glm::vec2 measureText(const std::string& text, float font_scale = 0.0015f);

    std::shared_ptr<TextureHandle> getAtlasTexture() const { return atlas_texture_; }

private:
    NovaCore* core_ = nullptr;
    TextureBridge* texture_bridge_ = nullptr;

    FT_Library ft_library_ = nullptr;
    FT_Face ft_face_ = nullptr;

    uint32_t atlas_width_ = 1024;
    uint32_t atlas_height_ = 1024;
    std::unordered_map<char, GlyphMetric> glyphs_;
    std::shared_ptr<TextureHandle> atlas_texture_;

    void buildAsciiAtlas(uint32_t pixel_height);
    void buildFallbackAtlas();
};

} // namespace NovaSpatial
