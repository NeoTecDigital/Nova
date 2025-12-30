#pragma once
#include <unordered_map>
#include <filesystem>
#include <optional>
#include <vector>
#include "../core.h"

std::optional<std::vector<std::shared_ptr<MeshType>>> loadGLTFMeshes(NovaCore* engine, std::filesystem::path path);
std::optional<std::vector<std::shared_ptr<MeshType>>> loadOBJMeshes(NovaCore* engine, std::filesystem::path path);
std::optional<std::vector<std::shared_ptr<MeshType>>> loadMeshes(NovaCore* engine, std::filesystem::path path);