#pragma once
#include <unordered_map>
#include <filesystem>
#include <optional>
#include <vector>
#include "Core/core.h"
namespace Nova {
std::optional<std::vector<std::shared_ptr<MeshType>>> loadGLTFMeshes(CoreLegacy* engine, std::filesystem::path path);
std::optional<std::vector<std::shared_ptr<MeshType>>> loadOBJMeshes(CoreLegacy* engine, std::filesystem::path path);
std::optional<std::vector<std::shared_ptr<MeshType>>> loadMeshes(CoreLegacy* engine, std::filesystem::path path);

} // namespace Nova
