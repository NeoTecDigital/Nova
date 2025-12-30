cmake_minimum_required(VERSION 3.20)
project(Nova LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Find Vulkan
find_package(Vulkan REQUIRED)

# Find SDL2
find_package(SDL2 REQUIRED)

# Collect Nova source files (excluding problematic external dependencies)
file(GLOB_RECURSE NOVA_SOURCES
    "Nova.cpp"
    "Core/*.cpp"
    "Core/modules/**/*.cpp"
    "Core/components/*.cpp"
)

# Exclude external dependencies that have missing headers
list(FILTER NOVA_SOURCES EXCLUDE REGEX ".*extern/.*")
list(FILTER NOVA_SOURCES EXCLUDE REGEX ".*fastgltf.*")
list(FILTER NOVA_SOURCES EXCLUDE REGEX ".*test.*")
list(FILTER NOVA_SOURCES EXCLUDE REGEX ".*example.*")

file(GLOB NOVA_HEADERS
    "Nova.h"
    "Core/*.h"
    "Core/modules/pipeline/*.h"
    "Core/modules/camera/*.h"
    "Core/components/*.h"
)

# Create Nova library
add_library(Nova STATIC ${NOVA_SOURCES})

# Disable warnings-as-errors for Nova (external library)
target_compile_options(Nova PRIVATE
    $<$<CXX_COMPILER_ID:GNU,Clang>:-w>  # Suppress all warnings for Nova
)

# Include directories
target_include_directories(Nova PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${CMAKE_CURRENT_SOURCE_DIR}/Core
    ${Vulkan_INCLUDE_DIRS}
    ${SDL2_INCLUDE_DIRS}
)

# Link libraries
target_link_libraries(Nova PUBLIC
    ${Vulkan_LIBRARIES}
    ${SDL2_LIBRARIES}
    ${CMAKE_DL_LIBS}
)

# Compile definitions
target_compile_definitions(Nova PUBLIC
    NOVA_COMPUTE_ENABLED
)

# Export for parent project
set(NOVA_INCLUDE_DIRS ${CMAKE_CURRENT_SOURCE_DIR} PARENT_SCOPE)
set(NOVA_LIBRARIES Nova PARENT_SCOPE)