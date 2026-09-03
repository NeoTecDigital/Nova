// Written by Richard Christopher, Copyright 2026 NeoTec Digital
#pragma once

#include "Nova/pipeline/spatial_vertex.h"
#include <array>
#include <string>

namespace Splash {

/**
 * MeshCache - Value-keyed hold for generated geometry.
 *
 * UI geometry is otherwise regenerated from scratch on every collectRender()
 * pass. The key is the exact parameter tuple that was handed to the generator,
 * so callers may mutate their public fields freely and the cache still
 * self-invalidates: there is no dirty flag that can be forgotten.
 *
 * Signature slots are generator-specific and compared for exact equality --
 * these are copies of the caller's own values, never recomputed results, so
 * bitwise float equality is the correct predicate.
 */
class MeshCache {
public:
    static constexpr size_t SIGNATURE_SLOTS = 12;
    using Signature = std::array<float, SIGNATURE_SLOTS>;

    bool isValidFor(const Signature& params) const {
        return valid_ && text_.empty() && params_ == params;
    }

    bool isValidFor(const Signature& params, const std::string& text) const {
        return valid_ && params_ == params && text_ == text;
    }

    void store(const Signature& params, Nova::MeshData mesh) {
        params_ = params;
        text_.clear();
        mesh_ = std::move(mesh);
        valid_ = true;
    }

    void store(const Signature& params, const std::string& text, Nova::MeshData mesh) {
        params_ = params;
        text_ = text;
        mesh_ = std::move(mesh);
        valid_ = true;
    }

    const Nova::MeshData& mesh() const { return mesh_; }
    void invalidate() { valid_ = false; }

private:
    Signature params_{};
    std::string text_;
    Nova::MeshData mesh_;
    bool valid_ = false;
};

} // namespace Splash
