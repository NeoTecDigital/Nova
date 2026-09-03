// Written by Richard Christopher, Copyright 2026 NeoTec Digital
#include "./spatial_font.h"
#include "Nova/components/logger.h"
#include <vector>
#include <cstring>
#include <algorithm>

namespace Splash {

SpatialFont::SpatialFont(Nova::Core* core, Nova::TextureBridge* texture_bridge)
    : core_(core), texture_bridge_(texture_bridge) {
    if (FT_Init_FreeType(&ft_library_) != 0) {
        report(LOGGER::ERROR, "SpatialFont - Failed to initialize FreeType library");
        ft_library_ = nullptr;
    }
}

SpatialFont::~SpatialFont() {
    // The atlas is this font's allocation, so it is this font's to return.
    // Freeing only the FreeType side left the VMA allocator holding a live
    // image at teardown, which aborts the process on an otherwise clean exit.
    if (atlas_texture_ && texture_bridge_) {
        texture_bridge_->releaseTexture(atlas_texture_);
    }
    atlas_texture_.reset();

    if (ft_face_) {
        FT_Done_Face(ft_face_);
        ft_face_ = nullptr;
    }
    if (ft_library_) {
        FT_Done_FreeType(ft_library_);
        ft_library_ = nullptr;
    }
}

bool SpatialFont::loadFromFile(const std::string& ttf_path, uint32_t pixel_height) {
    if (!ft_library_) {
        report(LOGGER::INFO, "SpatialFont - FreeType unavailable, using fallback atlas");
        buildFallbackAtlas();
        return false;
    }

    if (FT_New_Face(ft_library_, ttf_path.c_str(), 0, &ft_face_) != 0) {
        report(LOGGER::INFO, "SpatialFont - Failed to load font '%s', trying fallback", ttf_path.c_str());
        buildFallbackAtlas();
        return false;
    }

    FT_Set_Pixel_Sizes(ft_face_, 0, pixel_height);
    buildAsciiAtlas(pixel_height);
    report(LOGGER::INFO, "SpatialFont - Successfully loaded font '%s' (height: %u)", ttf_path.c_str(), pixel_height);
    return true;
}

void SpatialFont::buildAsciiAtlas(uint32_t pixel_height) {
    glyphs_.clear();
    atlas_width_ = 1024;
    atlas_height_ = 1024;

    std::vector<uint8_t> alpha_buffer(atlas_width_ * atlas_height_, 0);

    uint32_t current_x = 2;
    uint32_t current_y = 2;
    uint32_t max_row_height = 0;

    for (unsigned char c = 32; c < 127; ++c) {
        if (FT_Load_Char(ft_face_, c, FT_LOAD_RENDER) != 0) {
            continue;
        }

        FT_GlyphSlot slot = ft_face_->glyph;
        uint32_t glyph_w = slot->bitmap.width;
        uint32_t glyph_h = slot->bitmap.rows;

        if (current_x + glyph_w + 2 >= atlas_width_) {
            current_x = 2;
            current_y += max_row_height + 2;
            max_row_height = 0;
        }

        if (current_y + glyph_h + 2 >= atlas_height_) {
            break; // Atlas full
        }

        // Copy glyph bitmap to alpha atlas
        for (uint32_t r = 0; r < glyph_h; ++r) {
            for (uint32_t col = 0; col < glyph_w; ++col) {
                uint32_t dst_idx = (current_y + r) * atlas_width_ + (current_x + col);
                uint32_t src_idx = r * slot->bitmap.pitch + col;
                alpha_buffer[dst_idx] = slot->bitmap.buffer[src_idx];
            }
        }

        GlyphMetric metric;
        metric.advance_x = static_cast<float>(slot->advance.x >> 6);
        metric.width = static_cast<float>(glyph_w);
        metric.height = static_cast<float>(glyph_h);
        metric.bearing_x = static_cast<float>(slot->bitmap_left);
        metric.bearing_y = static_cast<float>(slot->bitmap_top);
        metric.uv_min = glm::vec2(
            static_cast<float>(current_x) / static_cast<float>(atlas_width_),
            static_cast<float>(current_y) / static_cast<float>(atlas_height_)
        );
        metric.uv_max = glm::vec2(
            static_cast<float>(current_x + glyph_w) / static_cast<float>(atlas_width_),
            static_cast<float>(current_y + glyph_h) / static_cast<float>(atlas_height_)
        );

        glyphs_[c] = metric;

        current_x += glyph_w + 2;
        max_row_height = std::max(max_row_height, glyph_h);
    }

    // Convert single-channel alpha to 4-channel RGBA (with red channel storing alpha for font shader)
    std::vector<uint8_t> rgba_buffer(atlas_width_ * atlas_height_ * 4, 255);
    for (size_t i = 0; i < alpha_buffer.size(); ++i) {
        rgba_buffer[i * 4 + 0] = alpha_buffer[i]; // R = alpha mask
        rgba_buffer[i * 4 + 1] = alpha_buffer[i];
        rgba_buffer[i * 4 + 2] = alpha_buffer[i];
        rgba_buffer[i * 4 + 3] = alpha_buffer[i];
    }

    atlas_texture_ = texture_bridge_->createTextureFromRGBA(rgba_buffer.data(), atlas_width_, atlas_height_);
}

void SpatialFont::buildFallbackAtlas() {
    atlas_width_ = 256;
    atlas_height_ = 256;
    std::vector<uint8_t> rgba_buffer(atlas_width_ * atlas_height_ * 4, 255);

    // Provide basic default metrics for ASCII characters
    for (char c = 32; c < 127; ++c) {
        GlyphMetric metric;
        metric.advance_x = 24.0f;
        metric.width = 20.0f;
        metric.height = 28.0f;
        metric.bearing_x = 2.0f;
        metric.bearing_y = 24.0f;
        metric.uv_min = glm::vec2(0.0f, 0.0f);
        metric.uv_max = glm::vec2(0.1f, 0.1f);
        glyphs_[c] = metric;
    }

    atlas_texture_ = texture_bridge_->createTextureFromRGBA(rgba_buffer.data(), atlas_width_, atlas_height_);
}

glm::vec2 SpatialFont::measureText(const std::string& text, float font_scale) {
    float width = 0.0f;
    float max_height = 0.0f;

    for (char c : text) {
        auto it = glyphs_.find(c);
        if (it != glyphs_.end()) {
            width += it->second.advance_x * font_scale;
            max_height = std::max(max_height, it->second.height * font_scale);
        } else {
            width += 20.0f * font_scale;
        }
    }

    return glm::vec2(width, max_height);
}

Nova::MeshData SpatialFont::createTextMesh(const std::string& text,
                                           float font_scale,
                                           const glm::vec4& color,
                                           bool center_aligned) {
    Nova::MeshData mesh;
    glm::vec2 total_size = measureText(text, font_scale);

    float pen_x = center_aligned ? (-total_size.x * 0.5f) : 0.0f;
    float pen_y = center_aligned ? (-total_size.y * 0.5f) : 0.0f;

    glm::vec4 params(0.0f, 0.0f, 2.0f, 0.0f); // mode 2.0 = font glyph alpha
    glm::vec3 normal(0.0f, 0.0f, 1.0f);

    for (char c : text) {
        auto it = glyphs_.find(c);
        if (it == glyphs_.end()) {
            pen_x += 20.0f * font_scale;
            continue;
        }

        const GlyphMetric& g = it->second;

        float x0 = pen_x + g.bearing_x * font_scale;
        float y0 = pen_y + (g.bearing_y - g.height) * font_scale;
        float x1 = x0 + g.width * font_scale;
        float y1 = y0 + g.height * font_scale;

        uint32_t base_v = static_cast<uint32_t>(mesh.vertices.size());

        // Top-left, Top-right, Bottom-right, Bottom-left (inverting Y for correct glyph orientation)
        mesh.vertices.push_back({ Nova::Math::Hyper4(x0, y1, 0.001f, 1.0f), Nova::Math::Hyper4(color.r, color.g, color.b, color.a), normal, glm::vec2(g.uv_min.x, g.uv_min.y), params });
        mesh.vertices.push_back({ Nova::Math::Hyper4(x1, y1, 0.001f, 1.0f), Nova::Math::Hyper4(color.r, color.g, color.b, color.a), normal, glm::vec2(g.uv_max.x, g.uv_min.y), params });
        mesh.vertices.push_back({ Nova::Math::Hyper4(x1, y0, 0.001f, 1.0f), Nova::Math::Hyper4(color.r, color.g, color.b, color.a), normal, glm::vec2(g.uv_max.x, g.uv_max.y), params });
        mesh.vertices.push_back({ Nova::Math::Hyper4(x0, y0, 0.001f, 1.0f), Nova::Math::Hyper4(color.r, color.g, color.b, color.a), normal, glm::vec2(g.uv_min.x, g.uv_max.y), params });

        mesh.indices.push_back(base_v + 0);
        mesh.indices.push_back(base_v + 1);
        mesh.indices.push_back(base_v + 2);
        mesh.indices.push_back(base_v + 2);
        mesh.indices.push_back(base_v + 3);
        mesh.indices.push_back(base_v + 0);

        pen_x += g.advance_x * font_scale;
    }

    return mesh;
}

} // namespace Splash
