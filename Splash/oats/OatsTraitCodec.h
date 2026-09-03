// Written by Richard Christopher, Copyright 2026 NeoTec Digital
#pragma once

#include "./OatsBridge.h"

#include <nlohmann/json.hpp>
#include <cstdint>
#include <cstring>
#include <exception>
#include <string>
#include <utility>

namespace Splash {

/**
 * OATS-ffi exposes trait payloads through two different encodings.
 *
 *  - The step delta (organizer.rs:66-80 for added_objects, :106-113 for updated_traits)
 *    emits native JSON values: numbers are numbers, strings are strings.
 *  - The snapshot query oats_runtime_get_entities_json (runtime.rs:139) emits
 *    `format!("{:?}", trait.data())`, i.e. Rust Debug text such as `Number(3.0)`,
 *    `String("foo")` or `Boolean(true)`, wrapped in a JSON string.
 *
 * OatsTraitDebug decodes the second form. Every decoder is total: it either produces the
 * typed value or returns false. Nothing is defaulted on failure.
 */
namespace OatsTraitDebug {
namespace detail {

inline bool splitVariant(const std::string& repr, const char* variant, std::string& inner) {
    const size_t name_len = std::strlen(variant);
    if (repr.size() < name_len + 2 || repr.compare(0, name_len, variant) != 0) {
        return false;
    }
    if (repr[name_len] != '(' || repr.back() != ')') {
        return false;
    }
    inner.assign(repr, name_len + 1, repr.size() - name_len - 2);
    return true;
}

inline void appendUtf8(uint32_t code_point, std::string& out) {
    if (code_point < 0x80u) {
        out.push_back(static_cast<char>(code_point));
        return;
    }
    if (code_point < 0x800u) {
        out.push_back(static_cast<char>(0xC0u | (code_point >> 6)));
        out.push_back(static_cast<char>(0x80u | (code_point & 0x3Fu)));
        return;
    }
    if (code_point < 0x10000u) {
        out.push_back(static_cast<char>(0xE0u | (code_point >> 12)));
        out.push_back(static_cast<char>(0x80u | ((code_point >> 6) & 0x3Fu)));
        out.push_back(static_cast<char>(0x80u | (code_point & 0x3Fu)));
        return;
    }
    out.push_back(static_cast<char>(0xF0u | (code_point >> 18)));
    out.push_back(static_cast<char>(0x80u | ((code_point >> 12) & 0x3Fu)));
    out.push_back(static_cast<char>(0x80u | ((code_point >> 6) & 0x3Fu)));
    out.push_back(static_cast<char>(0x80u | (code_point & 0x3Fu)));
}

inline int hexDigit(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

// Decodes Rust's `\u{HEX}` escape. `cursor` points at the 'u' and is advanced to the '}'.
inline bool decodeUnicodeEscape(const std::string& in, size_t& cursor, std::string& out) {
    if (cursor + 1 >= in.size() || in[cursor + 1] != '{') {
        return false;
    }
    const size_t close = in.find('}', cursor + 2);
    if (close == std::string::npos || close - cursor - 2 == 0 || close - cursor - 2 > 6) {
        return false;
    }
    uint32_t code_point = 0;
    for (size_t i = cursor + 2; i < close; ++i) {
        const int digit = hexDigit(in[i]);
        if (digit < 0) {
            return false;
        }
        code_point = (code_point << 4) | static_cast<uint32_t>(digit);
    }
    appendUtf8(code_point, out);
    cursor = close;
    return true;
}

inline bool decodeEscape(const std::string& in, size_t& cursor, std::string& out) {
    if (cursor >= in.size()) {
        return false;
    }
    switch (in[cursor]) {
        case 'n': out.push_back('\n'); return true;
        case 'r': out.push_back('\r'); return true;
        case 't': out.push_back('\t'); return true;
        case '0': out.push_back('\0'); return true;
        case '\\': out.push_back('\\'); return true;
        case '"': out.push_back('"'); return true;
        case '\'': out.push_back('\''); return true;
        case 'u': return decodeUnicodeEscape(in, cursor, out);
        default: return false;
    }
}

inline bool unescape(const std::string& in, std::string& out) {
    out.clear();
    out.reserve(in.size());
    for (size_t i = 0; i < in.size(); ++i) {
        if (in[i] != '\\') {
            out.push_back(in[i]);
            continue;
        }
        ++i;
        if (!decodeEscape(in, i, out)) {
            return false;
        }
    }
    return true;
}

} // namespace detail

inline bool decodeNumber(const std::string& repr, double& out) {
    std::string inner;
    if (!detail::splitVariant(repr, "Number", inner) || inner.empty()) {
        return false;
    }
    size_t consumed = 0;
    double value = 0.0;
    try {
        value = std::stod(inner, &consumed);
    } catch (const std::exception&) {
        return false;
    }
    if (consumed != inner.size()) {
        return false;
    }
    out = value;
    return true;
}

inline bool decodeString(const std::string& repr, std::string& out) {
    std::string inner;
    if (!detail::splitVariant(repr, "String", inner)) {
        return false;
    }
    if (inner.size() < 2 || inner.front() != '"' || inner.back() != '"') {
        return false;
    }
    return detail::unescape(inner.substr(1, inner.size() - 2), out);
}

inline bool decodeBoolean(const std::string& repr, bool& out) {
    std::string inner;
    if (!detail::splitVariant(repr, "Boolean", inner)) {
        return false;
    }
    if (inner == "true" || inner == "false") {
        out = (inner == "true");
        return true;
    }
    return false;
}

} // namespace OatsTraitDebug

/**
 * Reads typed trait values from either encoding. `debug_encoded` selects which one is
 * expected. The first mismatch is captured in error() and every subsequent read is a
 * no-op, so a caller only needs a single ok() check per object.
 */
class OatsTraitReader {
public:
    OatsTraitReader(const nlohmann::json& traits, bool debug_encoded, std::string context)
        : traits_(traits), debug_encoded_(debug_encoded), context_(std::move(context)) {}

    bool number(const char* key, double& out);
    bool text(const char* key, std::string& out);
    bool boolean(const char* key, bool& out);

    bool ok() const { return error_.empty(); }
    const std::string& error() const { return error_; }

private:
    const nlohmann::json* find(const char* key);
    void fail(const char* key, const std::string& why);

    const nlohmann::json& traits_;
    bool debug_encoded_ = false;
    std::string context_;
    std::string error_;
};

inline void OatsTraitReader::fail(const char* key, const std::string& why) {
    if (error_.empty()) {
        error_ = context_ + ": trait '" + key + "' " + why;
    }
}

inline const nlohmann::json* OatsTraitReader::find(const char* key) {
    if (!traits_.is_object()) {
        fail(key, "unavailable, traits payload is not an object");
        return nullptr;
    }
    const auto it = traits_.find(key);
    if (it == traits_.end()) {
        fail(key, "is absent");
        return nullptr;
    }
    return &(*it);
}

inline bool OatsTraitReader::number(const char* key, double& out) {
    const nlohmann::json* value = find(key);
    if (!value) {
        return false;
    }
    if (!debug_encoded_) {
        if (!value->is_number()) {
            fail(key, "is not a JSON number");
            return false;
        }
        out = value->get<double>();
        return true;
    }
    const std::string repr = value->is_string() ? value->get<std::string>() : value->dump();
    if (!OatsTraitDebug::decodeNumber(repr, out)) {
        fail(key, "is not a decodable Number(..) payload: " + repr);
        return false;
    }
    return true;
}

inline bool OatsTraitReader::text(const char* key, std::string& out) {
    const nlohmann::json* value = find(key);
    if (!value) {
        return false;
    }
    if (!value->is_string()) {
        fail(key, "is not a JSON string");
        return false;
    }
    const std::string repr = value->get<std::string>();
    if (!debug_encoded_) {
        out = repr;
        return true;
    }
    if (!OatsTraitDebug::decodeString(repr, out)) {
        fail(key, "is not a decodable String(..) payload: " + repr);
        return false;
    }
    return true;
}

inline bool OatsTraitReader::boolean(const char* key, bool& out) {
    const nlohmann::json* value = find(key);
    if (!value) {
        return false;
    }
    if (!debug_encoded_) {
        if (!value->is_boolean()) {
            fail(key, "is not a JSON boolean");
            return false;
        }
        out = value->get<bool>();
        return true;
    }
    const std::string repr = value->is_string() ? value->get<std::string>() : value->dump();
    if (!OatsTraitDebug::decodeBoolean(repr, out)) {
        fail(key, "is not a decodable Boolean(..) payload: " + repr);
        return false;
    }
    return true;
}

// ---------------------------------------------------------------------------
// Whole-object parsers (traits laid out by runtime.rs:56-99)
// ---------------------------------------------------------------------------

inline bool parseOatsSpatialPill(const nlohmann::json& traits, bool debug_encoded,
                                 OatsSpatialPill& pill, std::string& error) {
    OatsTraitReader reader(traits, debug_encoded, "SpatialPill '" + pill.name + "'");
    double px = 0.0, py = 0.0, pz = 0.0;
    double rw = 1.0, rx = 0.0, ry = 0.0, rz = 0.0;
    double radius = 0.0, height = 0.0;

    reader.text("path", pill.path);
    reader.number("pos_x", px);
    reader.number("pos_y", py);
    reader.number("pos_z", pz);
    reader.number("rot_w", rw);
    reader.number("rot_x", rx);
    reader.number("rot_y", ry);
    reader.number("rot_z", rz);
    reader.number("radius", radius);
    reader.number("height", height);

    if (!reader.ok()) {
        error = reader.error();
        return false;
    }
    pill.position = glm::vec3(static_cast<float>(px), static_cast<float>(py), static_cast<float>(pz));
    pill.orientation = glm::vec4(static_cast<float>(rx), static_cast<float>(ry),
                                 static_cast<float>(rz), static_cast<float>(rw));
    pill.radius = static_cast<float>(radius);
    pill.height = static_cast<float>(height);
    return true;
}

inline bool parseOatsFileSystemEntity(const nlohmann::json& traits, bool debug_encoded,
                                      OatsFileSystemEntity& entity, std::string& error) {
    OatsTraitReader reader(traits, debug_encoded, "FileSystemEntity '" + entity.name + "'");
    double size_bytes = 0.0;

    reader.text("path", entity.path);
    reader.text("extension", entity.extension);
    reader.boolean("is_directory", entity.is_directory);
    reader.number("size_bytes", size_bytes);

    if (!reader.ok()) {
        error = reader.error();
        return false;
    }
    entity.size_bytes = size_bytes > 0.0 ? static_cast<uint64_t>(size_bytes) : 0u;
    return true;
}

inline bool parseOatsHypergraphNode(const nlohmann::json& traits, bool debug_encoded,
                                    OatsHypergraphNode& node, std::string& error) {
    OatsTraitReader reader(traits, debug_encoded, "HypergraphDAGNode '" + node.name + "'");

    reader.text("node_id", node.node_id);
    reader.text("namespace", node.name_space);
    reader.text("parents", node.parents_json);

    if (!reader.ok()) {
        error = reader.error();
        return false;
    }
    return true;
}

// ---------------------------------------------------------------------------
// Single-trait application for the native-JSON delta channel
// ---------------------------------------------------------------------------

inline float* oatsPillNumericField(OatsSpatialPill& pill, const std::string& name) {
    if (name == "pos_x") return &pill.position.x;
    if (name == "pos_y") return &pill.position.y;
    if (name == "pos_z") return &pill.position.z;
    if (name == "rot_w") return &pill.orientation.w;
    if (name == "rot_x") return &pill.orientation.x;
    if (name == "rot_y") return &pill.orientation.y;
    if (name == "rot_z") return &pill.orientation.z;
    if (name == "radius") return &pill.radius;
    if (name == "height") return &pill.height;
    return nullptr;
}

inline void applyOatsPillTrait(OatsSpatialPill& pill, const std::string& name,
                               const nlohmann::json& data) {
    if (name == "path" && data.is_string()) {
        pill.path = data.get<std::string>();
        return;
    }
    float* field = oatsPillNumericField(pill, name);
    if (field && data.is_number()) {
        *field = data.get<float>();
    }
}

inline void applyOatsEntityTrait(OatsFileSystemEntity& entity, const std::string& name,
                                 const nlohmann::json& data) {
    if (name == "path" && data.is_string()) entity.path = data.get<std::string>();
    else if (name == "extension" && data.is_string()) entity.extension = data.get<std::string>();
    else if (name == "is_directory" && data.is_boolean()) entity.is_directory = data.get<bool>();
    else if (name == "size_bytes" && data.is_number()) entity.size_bytes = data.get<uint64_t>();
}

inline void applyOatsNodeTrait(OatsHypergraphNode& node, const std::string& name,
                               const nlohmann::json& data) {
    if (!data.is_string()) {
        return;
    }
    if (name == "node_id") node.node_id = data.get<std::string>();
    else if (name == "namespace") node.name_space = data.get<std::string>();
    else if (name == "parents") node.parents_json = data.get<std::string>();
}

} // namespace Splash
