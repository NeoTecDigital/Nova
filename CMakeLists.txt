# Written by Richard Christopher, Copyright 2026 NeoTec Digital

cmake_minimum_required(VERSION 3.20)
project(Nova LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

enable_testing()

# ==========================================
# Submodule preflight
#
# extern/imgui, extern/lumberjack and extern/OATS-rs are git submodules. A fresh
# clone without `--recursive` leaves them empty; failing here with a usable
# instruction beats failing 40 targets deep with "no such file".
# ==========================================
set(NOVA_SUBMODULE_SENTINELS
    "extern/imgui/imgui.cpp"
    "extern/OATS-rs/Cargo.toml"
)
foreach(_sentinel IN LISTS NOVA_SUBMODULE_SENTINELS)
    if(NOT EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/${_sentinel}")
        message(FATAL_ERROR
            "Missing submodule content: ${_sentinel}\n"
            "Git submodules are not initialised. Run:\n"
            "    git submodule update --init --recursive\n"
            "from ${CMAKE_CURRENT_SOURCE_DIR} and re-run CMake.")
    endif()
endforeach()

# ==========================================
# Dependencies
# ==========================================
find_package(Vulkan REQUIRED)
find_package(SDL2 REQUIRED)

# glm and nlohmann_json are included by first-party headers
# (include/Clouds/OatsBridge.h, src/Clouds/OatsBridge.cpp). They previously
# resolved only because they happen to live in /usr/include.
find_package(glm QUIET)
if(NOT glm_FOUND)
    message(FATAL_ERROR
        "glm not found. It is a hard dependency of include/Clouds/OatsBridge.h.\n"
        "Install the GLM development package (Arch: glm, Debian: libglm-dev) or\n"
        "pass -Dglm_DIR=<path containing glmConfig.cmake>.")
endif()

find_package(nlohmann_json QUIET)
if(NOT nlohmann_json_FOUND)
    message(FATAL_ERROR
        "nlohmann_json not found. It is a hard dependency of src/Clouds/OatsBridge.cpp.\n"
        "Install it (Arch: nlohmann-json, Debian: nlohmann-json3-dev) or pass\n"
        "-Dnlohmann_json_DIR=<path containing nlohmann_jsonConfig.cmake>.")
endif()

find_package(PkgConfig REQUIRED)
pkg_check_modules(FREETYPE REQUIRED freetype2)
pkg_check_modules(WAYLAND_SERVER REQUIRED wayland-server)
pkg_check_modules(LIBDRM REQUIRED libdrm)
pkg_check_modules(WAYLAND_PROTOCOLS REQUIRED wayland-protocols)
pkg_check_modules(XKBCOMMON REQUIRED xkbcommon)

# wlroots major.minor is baked into the pkg-config name.
pkg_check_modules(WLROOTS wlroots-0.19)
if(NOT WLROOTS_FOUND)
    pkg_check_modules(WLROOTS wlroots-0.20)
endif()
if(NOT WLROOTS_FOUND)
    pkg_check_modules(WLROOTS REQUIRED wlroots)
endif()

find_program(WAYLAND_SCANNER wayland-scanner REQUIRED)

# ==========================================
# Warning policy
#
# `-w` used to be applied to the whole Nova target, which is why a VK_TRY format
# bug and six -Wreorder member-init bugs went unseen. Warnings are now on for
# first-party code; vendored code is suppressed per translation unit below.
# -Werror is deliberately NOT set: this phase is about visibility.
#
# -Wmissing-field-initializers is excluded globally: it fires 401 times on the
# idiomatic C++20 designated-initializer form of Vulkan's Vk*CreateInfo structs,
# where the omitted trailing members are value-initialised by the standard. It
# is 88% of all diagnostics and carries no signal for this codebase.
# ==========================================
set(NOVA_FIRST_PARTY_WARNINGS
    $<$<COMPILE_LANG_AND_ID:CXX,GNU,Clang>:-Wall>
    $<$<COMPILE_LANG_AND_ID:CXX,GNU,Clang>:-Wextra>
    $<$<COMPILE_LANG_AND_ID:CXX,GNU,Clang>:-Wno-missing-field-initializers>
)

# Warning categories emitted by the vendored amalgamations themselves. Applied
# only to the two first-party TUs that carry them, so every other diagnostic
# (-Wformat, -Wsign-compare, ...) stays live in those files.
set(NOVA_VMA_CARRIER_SUPPRESSIONS      "-Wno-unused-parameter;-Wno-unused-variable")
set(NOVA_FASTGLTF_CARRIER_SUPPRESSIONS "-Wno-unused-parameter;-Wno-redundant-move")

# ==========================================
# Wayland protocol generation
# ==========================================
pkg_get_variable(WAYLAND_PROTOCOLS_DIR wayland-protocols pkgdatadir)
set(XDG_SHELL_XML "${WAYLAND_PROTOCOLS_DIR}/stable/xdg-shell/xdg-shell.xml")
set(PROTOCOL_GEN_DIR "${CMAKE_CURRENT_BINARY_DIR}/protocol")
file(MAKE_DIRECTORY ${PROTOCOL_GEN_DIR})

add_custom_command(
    OUTPUT "${PROTOCOL_GEN_DIR}/xdg-shell-protocol.h"
    COMMAND ${WAYLAND_SCANNER} server-header ${XDG_SHELL_XML} "${PROTOCOL_GEN_DIR}/xdg-shell-protocol.h"
    DEPENDS ${XDG_SHELL_XML}
    COMMENT "wayland-scanner: xdg-shell server header"
)

add_custom_command(
    OUTPUT "${PROTOCOL_GEN_DIR}/xdg-shell-protocol.c"
    COMMAND ${WAYLAND_SCANNER} private-code ${XDG_SHELL_XML} "${PROTOCOL_GEN_DIR}/xdg-shell-protocol.c"
    DEPENDS ${XDG_SHELL_XML}
    COMMENT "wayland-scanner: xdg-shell private code"
)

# ==========================================
# GLSL -> SPIR-V
#
# The .spv files were previously committed as build *inputs* with no rule to
# regenerate them. Sources reference them CWD-relative -- see the default
# arguments of SpatialPipeline::build in
# Core/modules/spatial_pipeline/spatial_pipeline.h and the pipeline->build call
# in examples/clouds_server_main.cpp -- so output goes next to the binaries.
#
# Output basenames are irregular in the existing tree (_v/_f vs _vert/_frag), so
# the mapping is explicit rather than derived from a naming convention.
# ==========================================
find_program(GLSL_COMPILER NAMES glslc glslangValidator
    HINTS "$ENV{VULKAN_SDK}/bin" ${Vulkan_GLSLC_EXECUTABLE})
if(NOT GLSL_COMPILER)
    message(FATAL_ERROR
        "No GLSL compiler found. Install shaderc (glslc) or glslang "
        "(glslangValidator), or set VULKAN_SDK.")
endif()

set(NOVA_SHADER_MAP
    "shaders/spatial/spatial_ui.vert|shaders/spatial/spatial_ui_vert.spv"
    "shaders/spatial/spatial_ui.frag|shaders/spatial/spatial_ui_frag.spv"
    "shaders/Clouds/ui.vert|shaders/Clouds/ui_vert.spv"
    "shaders/Clouds/ui.frag|shaders/Clouds/ui_frag.spv"
    "Core/components/shaders/sq1.vert|Core/components/shaders/sq1_v.spv"
    "Core/components/shaders/sq1.frag|Core/components/shaders/sq1_f.spv"
    "Core/components/shaders/tri.vert|Core/components/shaders/tri_vert.spv"
    "Core/components/shaders/tri.frag|Core/components/shaders/tri_frag.spv"
)

set(NOVA_SPIRV_OUTPUTS "")
foreach(_entry IN LISTS NOVA_SHADER_MAP)
    string(REPLACE "|" ";" _pair "${_entry}")
    list(GET _pair 0 _glsl_rel)
    list(GET _pair 1 _spv_rel)
    set(_glsl "${CMAKE_CURRENT_SOURCE_DIR}/${_glsl_rel}")
    set(_spv "${CMAKE_CURRENT_BINARY_DIR}/${_spv_rel}")
    get_filename_component(_spv_dir "${_spv}" DIRECTORY)

    if(GLSL_COMPILER MATCHES "glslangValidator")
        set(_glsl_cmd ${GLSL_COMPILER} -V "${_glsl}" -o "${_spv}")
    else()
        set(_glsl_cmd ${GLSL_COMPILER} "${_glsl}" -o "${_spv}")
    endif()

    add_custom_command(
        OUTPUT "${_spv}"
        COMMAND ${CMAKE_COMMAND} -E make_directory "${_spv_dir}"
        COMMAND ${_glsl_cmd}
        DEPENDS "${_glsl}"
        COMMENT "GLSL -> SPIR-V: ${_spv_rel}"
        VERBATIM
    )
    list(APPEND NOVA_SPIRV_OUTPUTS "${_spv}")
endforeach()

add_custom_target(nova_shaders ALL DEPENDS ${NOVA_SPIRV_OUTPUTS})

# ==========================================
# OATS FFI (Rust cdylib)
#
# extern/OATS-ffi/lib/ is gitignored, so a fresh clone has no .so. Build it.
# ==========================================
find_program(CARGO_EXECUTABLE cargo HINTS "$ENV{HOME}/.cargo/bin")
if(NOT CARGO_EXECUTABLE)
    message(FATAL_ERROR
        "cargo not found. extern/OATS-ffi is a Rust crate and must be built to "
        "produce liboats_ffi.so. Install a Rust toolchain (https://rustup.rs) or "
        "pass -DCARGO_EXECUTABLE=<path/to/cargo>.")
endif()

set(OATS_FFI_DIR "${CMAKE_CURRENT_SOURCE_DIR}/extern/OATS-ffi")
set(OATS_FFI_TARGET_DIR "${CMAKE_CURRENT_BINARY_DIR}/oats-ffi-target")
set(OATS_FFI_LIB "${OATS_FFI_TARGET_DIR}/release/liboats_ffi.so")

file(GLOB_RECURSE OATS_FFI_RUST_SOURCES CONFIGURE_DEPENDS
    "${OATS_FFI_DIR}/src/*.rs"
    "${CMAKE_CURRENT_SOURCE_DIR}/extern/OATS-rs/src/*.rs"
)

add_custom_command(
    OUTPUT "${OATS_FFI_LIB}"
    COMMAND ${CARGO_EXECUTABLE} build --release
            --manifest-path "${OATS_FFI_DIR}/Cargo.toml"
            --target-dir "${OATS_FFI_TARGET_DIR}"
    DEPENDS ${OATS_FFI_RUST_SOURCES}
            "${OATS_FFI_DIR}/Cargo.toml"
            "${OATS_FFI_DIR}/Cargo.lock"
    COMMENT "cargo build --release: oats_ffi"
    VERBATIM
)

add_custom_target(oats_ffi_build DEPENDS "${OATS_FFI_LIB}")

add_library(oats_ffi SHARED IMPORTED GLOBAL)
# A Rust cdylib carries no SONAME. Without IMPORTED_NO_SONAME, CMake puts the
# library's path on the link line and ld copies that path verbatim into
# DT_NEEDED; because it contains a slash the loader resolves it against the
# process CWD and ignores RUNPATH entirely. NO_SONAME makes CMake emit
# -L<dir> -loats_ffi, so DT_NEEDED is a bare name and RUNPATH applies.
set_target_properties(oats_ffi PROPERTIES
    IMPORTED_LOCATION "${OATS_FFI_LIB}"
    IMPORTED_NO_SONAME TRUE
    INTERFACE_INCLUDE_DIRECTORIES "${OATS_FFI_DIR}/include"
)
add_dependencies(oats_ffi oats_ffi_build)

# ==========================================
# Nova engine library
# ==========================================
set(NOVA_SOURCES
    "${CMAKE_CURRENT_SOURCE_DIR}/Nova.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/Core/core_base.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/Core/core.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/Core/nova_compute.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/Core/nova_graphics.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/Core/modules/device.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/Core/modules/management.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/Core/modules/presentation.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/Core/modules/render.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/Core/modules/atomic/atomic.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/Core/modules/pipeline/pipeline.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/Core/modules/pipeline/scene.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/Core/modules/camera/camera.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/Core/modules/camera/perspective.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/Core/components/genesis.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/Core/components/logger.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/Core/components/obj_loader.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/Core/modules/spatial_pipeline/texture_bridge.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/Core/modules/spatial_pipeline/spatial_pipeline.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/Core/modules/spatial_pipeline/spatial_mesh.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/Core/modules/spatial_pipeline/spatial_font.cpp"
)

add_library(Nova STATIC ${NOVA_SOURCES})
add_dependencies(Nova nova_shaders)

target_compile_options(Nova PRIVATE ${NOVA_FIRST_PARTY_WARNINGS})

# management.cpp is the VMA_IMPLEMENTATION carrier: it instantiates all 19,786
# lines of AMD's VulkanMemoryAllocator. obj_loader.cpp instantiates fastgltf.
set_source_files_properties(
    "${CMAKE_CURRENT_SOURCE_DIR}/Core/modules/management.cpp"
    PROPERTIES COMPILE_OPTIONS "${NOVA_VMA_CARRIER_SUPPRESSIONS}"
)
set_source_files_properties(
    "${CMAKE_CURRENT_SOURCE_DIR}/Core/components/obj_loader.cpp"
    PROPERTIES COMPILE_OPTIONS "${NOVA_FASTGLTF_CARRIER_SUPPRESSIONS}"
)

target_include_directories(Nova PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${CMAKE_CURRENT_SOURCE_DIR}/Core
    ${Vulkan_INCLUDE_DIRS}
    ${SDL2_INCLUDE_DIRS}
    ${FREETYPE_INCLUDE_DIRS}
)

target_link_libraries(Nova PUBLIC
    glm::glm
    ${Vulkan_LIBRARIES}
    ${SDL2_LIBRARIES}
    ${FREETYPE_LIBRARIES}
    ${CMAKE_DL_LIBS}
)

target_compile_definitions(Nova PUBLIC
    NOVA_COMPUTE_ENABLED
)

# ==========================================
# Clouds display server & spatial UI
# ==========================================
set(CLOUDS_FIRST_PARTY_SOURCES
    "${CMAKE_CURRENT_SOURCE_DIR}/src/Clouds/SpatialNode.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/src/Clouds/Primitives.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/src/Clouds/SpatialScene.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/src/Clouds/SpatialCompositor.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/src/Clouds/SpatialBackendStack.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/src/Clouds/SpatialWindowHost.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/src/Clouds/SpatialPopupHost.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/src/Clouds/SpatialSeatInput.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/src/Clouds/OatsBridge.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/src/Clouds/SpatialFilesystem.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/src/Clouds/ImGuiEngineOverlay.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/src/Clouds/CrmWorkspace.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/src/Clouds/DynamicSceneManager.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/src/Clouds/EngineHUD.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/src/Clouds/UI/UIComponents.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/src/Clouds/UI/UIDockBar.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/src/Clouds/UI/UIMenuBar.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/src/Clouds/UI/UIWindow.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/src/Clouds/UI/WindowManager.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/examples/clouds_server_main.cpp"
)

set(CLOUDS_VENDOR_SOURCES
    "${CMAKE_CURRENT_SOURCE_DIR}/extern/imgui/imgui.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/extern/imgui/imgui_demo.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/extern/imgui/imgui_draw.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/extern/imgui/imgui_tables.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/extern/imgui/imgui_widgets.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/extern/imgui/backends/imgui_impl_sdl2.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/extern/imgui/backends/imgui_impl_vulkan.cpp"
)

# Wholly third-party translation units: nothing in them is ours to fix.
set_source_files_properties(${CLOUDS_VENDOR_SOURCES} PROPERTIES COMPILE_OPTIONS "-w")
set_source_files_properties("${PROTOCOL_GEN_DIR}/xdg-shell-protocol.c" PROPERTIES COMPILE_OPTIONS "-w")

add_executable(Clouds
    ${CLOUDS_FIRST_PARTY_SOURCES}
    ${CLOUDS_VENDOR_SOURCES}
    "${PROTOCOL_GEN_DIR}/xdg-shell-protocol.c"
    "${PROTOCOL_GEN_DIR}/xdg-shell-protocol.h"
)
add_dependencies(Clouds nova_shaders)

target_compile_options(Clouds PRIVATE ${NOVA_FIRST_PARTY_WARNINGS})

target_compile_definitions(Clouds PRIVATE
    WLR_USE_UNSTABLE
)

target_include_directories(Clouds PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${CMAKE_CURRENT_SOURCE_DIR}/include
    ${CMAKE_CURRENT_SOURCE_DIR}/Core
    ${CMAKE_CURRENT_SOURCE_DIR}/extern/imgui
    ${CMAKE_CURRENT_SOURCE_DIR}/extern/imgui/backends
    ${PROTOCOL_GEN_DIR}
    ${Vulkan_INCLUDE_DIRS}
    ${SDL2_INCLUDE_DIRS}
    ${FREETYPE_INCLUDE_DIRS}
    ${WLROOTS_INCLUDE_DIRS}
    ${WAYLAND_SERVER_INCLUDE_DIRS}
    ${LIBDRM_INCLUDE_DIRS}
    ${XKBCOMMON_INCLUDE_DIRS}
)

target_link_libraries(Clouds PRIVATE
    Nova
    oats_ffi
    nlohmann_json::nlohmann_json
    ${WLROOTS_LIBRARIES}
    ${WAYLAND_SERVER_LIBRARIES}
    ${LIBDRM_LIBRARIES}
    ${XKBCOMMON_LIBRARIES}
    ${Vulkan_LIBRARIES}
    ${SDL2_LIBRARIES}
    ${FREETYPE_LIBRARIES}
    ${CMAKE_DL_LIBS}
)

# ==========================================
# WaveCube compute driver
#
# Previously built only by the root Makefile, which carried a divergent copy of
# NOVA_SOURCES. That divergence is how atomic.cpp and pipeline/scene.cpp were
# lost from the CMake build. The Makefile now delegates here; this is the single
# definition of the Nova source set.
# ==========================================
add_executable(wavecube_compute "${CMAKE_CURRENT_SOURCE_DIR}/wavecube_compute.cpp")
target_compile_options(wavecube_compute PRIVATE ${NOVA_FIRST_PARTY_WARNINGS})
target_link_libraries(wavecube_compute PRIVATE Nova)

# ==========================================
# Seam benchmarks & limit integration tests
# ==========================================
add_executable(test_seam_math_raycast
    "${CMAKE_CURRENT_SOURCE_DIR}/tests/test_seam_math_raycast.cpp"
)
target_compile_options(test_seam_math_raycast PRIVATE ${NOVA_FIRST_PARTY_WARNINGS})
target_include_directories(test_seam_math_raycast PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${CMAKE_CURRENT_SOURCE_DIR}/Core
)
target_link_libraries(test_seam_math_raycast PRIVATE
    Nova
    ${CMAKE_DL_LIBS}
)

# Talks to the C ABI in extern/OATS-ffi/include/oats_ffi.h directly; it does not
# link the C++ OatsBridge wrapper, which keeps the seam under test isolated.
add_executable(test_seam_ffi_roundtrip
    "${CMAKE_CURRENT_SOURCE_DIR}/tests/test_seam_ffi_roundtrip.cpp"
)
target_compile_options(test_seam_ffi_roundtrip PRIVATE ${NOVA_FIRST_PARTY_WARNINGS})
target_include_directories(test_seam_ffi_roundtrip PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${CMAKE_CURRENT_SOURCE_DIR}/include
    ${CMAKE_CURRENT_SOURCE_DIR}/Core
)
target_link_libraries(test_seam_ffi_roundtrip PRIVATE
    Nova
    oats_ffi
    ${CMAKE_DL_LIBS}
)

# Scene-graph input routing. Builds the Clouds scene sources it exercises
# directly rather than linking the Clouds executable: the test needs no Vulkan
# device, no wlroots and no compositor, and saying so in the target is how that
# stays true.
add_executable(test_scene_input
    "${CMAKE_CURRENT_SOURCE_DIR}/tests/test_scene_input.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/src/Clouds/SpatialNode.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/src/Clouds/SpatialScene.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/src/Clouds/Primitives.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/src/Clouds/UI/UIComponents.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/src/Clouds/UI/UIWindow.cpp"
)
target_compile_options(test_scene_input PRIVATE ${NOVA_FIRST_PARTY_WARNINGS})
target_include_directories(test_scene_input PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${CMAKE_CURRENT_SOURCE_DIR}/include
    ${CMAKE_CURRENT_SOURCE_DIR}/Core
)
target_link_libraries(test_scene_input PRIVATE
    Nova
    ${CMAKE_DL_LIBS}
)

add_test(NAME seam_math_raycast COMMAND test_seam_math_raycast)
add_test(NAME seam_ffi_roundtrip COMMAND test_seam_ffi_roundtrip)
add_test(NAME scene_input COMMAND test_scene_input)

set_tests_properties(seam_math_raycast seam_ffi_roundtrip scene_input PROPERTIES
    TIMEOUT 120
    WORKING_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}"
)
