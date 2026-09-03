#pragma once

#include <string>
#include <vector>
#include "./vertex.h"

// Shader paths removed - should be passed via Config
// Applications must provide shader paths when graphics pipeline is needed

namespace genesis {
    std::vector<char> loadFile(const std::string&);
    void createObjects(std::vector<Nova::Vertex>*, std::vector<uint32_t>*);
}