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

# The integration harnesses under tests/integration/ are real Wayland clients.
pkg_check_modules(WAYLAND_CLIENT REQUIRED wayland-client)
pkg_check_modules(LIBDRM REQUIRED libdrm)
pkg_check_modules(WAYLAND_PROTOCOLS REQUIRED wayland-protocols)
pkg_check_modules(XKBCOMMON REQUIRED xkbcommon)

# wlr/render/pixman.h is included by include/Clouds/SpatialPresentLoop.h for the
# CPU-copy presentation path, and it includes <pixman.h> directly.
pkg_check_modules(PIXMAN REQUIRED pixman-1)

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

# --- Client-side protocol code, for the in-repo integration harnesses ---------
#
# The harnesses under tests/integration/ are real Wayland CLIENTS driven against
# a real SpatialCompositor, so they need the client-header form of the same XML
# the server rule above consumes, plus the two unstable protocols the compositor
# advertises and the protocol phases assert on. Mirrors the server rule exactly;
# only the wayland-scanner mode and the output name differ.
set(NOVA_CLIENT_PROTOCOL_MAP
    "xdg-shell|${WAYLAND_PROTOCOLS_DIR}/stable/xdg-shell/xdg-shell.xml"
    "xdg-decoration-unstable-v1|${WAYLAND_PROTOCOLS_DIR}/unstable/xdg-decoration/xdg-decoration-unstable-v1.xml"
    "primary-selection-unstable-v1|${WAYLAND_PROTOCOLS_DIR}/unstable/primary-selection/primary-selection-unstable-v1.xml"
)

set(NOVA_CLIENT_PROTOCOL_SOURCES "")
set(NOVA_CLIENT_PROTOCOL_HEADERS "")
foreach(_entry IN LISTS NOVA_CLIENT_PROTOCOL_MAP)
    string(REPLACE "|" ";" _pair "${_entry}")
    list(GET _pair 0 _proto_name)
    list(GET _pair 1 _proto_xml)
    if(NOT EXISTS "${_proto_xml}")
        message(FATAL_ERROR
            "Missing Wayland protocol XML: ${_proto_xml}\n"
            "tests/integration needs it to generate client bindings. Install a "
            "complete wayland-protocols package.")
    endif()

    set(_client_header "${PROTOCOL_GEN_DIR}/${_proto_name}-client-protocol.h")
    set(_client_code   "${PROTOCOL_GEN_DIR}/${_proto_name}-client-protocol.c")

    add_custom_command(
        OUTPUT "${_client_header}"
        COMMAND ${WAYLAND_SCANNER} client-header "${_proto_xml}" "${_client_header}"
        DEPENDS "${_proto_xml}"
        COMMENT "wayland-scanner: ${_proto_name} client header"
        VERBATIM
    )
    add_custom_command(
        OUTPUT "${_client_code}"
        COMMAND ${WAYLAND_SCANNER} private-code "${_proto_xml}" "${_client_code}"
        DEPENDS "${_proto_xml}"
        COMMENT "wayland-scanner: ${_proto_name} private code"
        VERBATIM
    )
    list(APPEND NOVA_CLIENT_PROTOCOL_HEADERS "${_client_header}")
    list(APPEND NOVA_CLIENT_PROTOCOL_SOURCES "${_client_code}")
endforeach()

# Generated C: not ours to fix.
set_source_files_properties(${NOVA_CLIENT_PROTOCOL_SOURCES} PROPERTIES COMPILE_OPTIONS "-w")

# ONE target owns the generation, and everything else depends on that target.
#
# Listing a custom-command OUTPUT as a source of several targets makes the
# Makefile generator emit the rule once per target, and a parallel build then
# races two wayland-scanner invocations writing the same header - which surfaces
# as "zwp_primary_selection_device_manager_v1_interface was not declared",
# i.e. a compile against a half-written file. A library with a single owner
# removes the race by construction, and target-level dependencies give the
# ordering the header needs.
add_custom_target(nova_client_protocol_headers DEPENDS ${NOVA_CLIENT_PROTOCOL_HEADERS})

add_library(nova_client_protocols STATIC ${NOVA_CLIENT_PROTOCOL_SOURCES})
add_dependencies(nova_client_protocols nova_client_protocol_headers)
target_include_directories(nova_client_protocols PUBLIC
    ${PROTOCOL_GEN_DIR}
    ${WAYLAND_CLIENT_INCLUDE_DIRS}
)
target_link_libraries(nova_client_protocols PUBLIC ${WAYLAND_CLIENT_LIBRARIES})

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
# Nova.cpp is deliberately absent: `class Nova` IS the SDL application wrapper
# (SDL_Init, SDL_CreateWindow, SDL_PollEvent, SDL_Vulkan_GetDrawableSize), so it
# belongs to the NovaSDL target below, not to the windowless engine archive.
set(NOVA_SOURCES
    "${CMAKE_CURRENT_SOURCE_DIR}/Core/core_base.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/Core/core_device_select.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/Core/core.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/Core/nova_compute.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/Core/nova_graphics.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/Core/nova_swapchain.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/Core/nova_offscreen.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/Core/nova_dmabuf.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/Core/nova_dmabuf_query.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/Core/nova_dmabuf_import.cpp"
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
    ${FREETYPE_INCLUDE_DIRS}
)

# SDL2 headers - and ONLY the headers - are a compile requirement of any consumer
# that includes Core/nova_graphics.h: that header pulls in
# Core/modules/camera/camera.h, which includes <SDL2/SDL_events.h> for
# Camera::processEvents(SDL_Event&). `vazio` is such a consumer
# (examples/vazio_main.cpp constructs an offscreen NovaGraphics), so the include
# path is genuinely PUBLIC. An include path is not a link edge: `vazio` compiles
# against SDL2's headers and links none of its code. Narrowing this further means
# taking SDL out of Camera's public interface, not out of here.
target_include_directories(Nova PUBLIC ${SDL2_INCLUDE_DIRS})

target_link_libraries(Nova PUBLIC
    glm::glm
    ${Vulkan_LIBRARIES}
    ${FREETYPE_LIBRARIES}
    ${CMAKE_DL_LIBS}
)

# Nova links NO SDL, in any scope. A PRIVATE dependency on a STATIC library is
# still forwarded to consumers as $<LINK_ONLY:>, so as long as any archive member
# a consumer pulls carried an undefined SDL symbol, every consumer - `vazio`
# included - resolved against libSDL2 and carried it at runtime. The two members
# that did (core_base.cpp.o: SDL_Vulkan_GetInstanceExtensions,
# nova_graphics.cpp.o: SDL_Vulkan_CreateSurface) no longer call SDL at all; both
# call sites now live in Core/nova_sdl.cpp, in the separate NovaSDL archive below.

target_compile_definitions(Nova PUBLIC
    NOVA_COMPUTE_ENABLED
)

# ==========================================
# NovaSDL - the window-system half of the engine
#
# One translation unit: Core/nova_sdl.cpp, holding NovaSDL::vulkanInstanceExtensions
# and the SDL_Window constructor of NovaGraphics. Kept OUT of libNova.a so that a
# target which never opens a window cannot accidentally acquire libSDL2 through
# the archive. Link this only from targets that do.
#
# Two translation units live here: Core/nova_sdl.cpp (the extension query and the
# SDL_Window constructor of NovaGraphics) and Nova.cpp (`class Nova`, the SDL
# application wrapper that owns the window and the event pump). Both are
# SDL-calling by nature; the dependency direction is strictly NovaSDL -> Nova,
# never back, which is what keeps the two static archives acyclic on the link line.
#
# ONE residual SDL reference remains inside libNova.a: core.cpp.o, the deprecated
# NovaCoreLegacy, calls SDL_Vulkan_GetInstanceExtensions. It is NOT moved because
# NovaCoreLegacy's methods are spread across five translation units and splitting
# the class across two archives would reintroduce exactly the ordering problem
# this arrangement removes. Nothing reachable from `vazio` pulls that member, so
# it costs nothing today; a target that does start using NovaCoreLegacy fails
# loudly at link time and must link NovaSDL. Enforced by the `nova_link_hygiene`
# test at the bottom of this file.
# ==========================================
add_library(NovaSDL STATIC
    "${CMAKE_CURRENT_SOURCE_DIR}/Core/nova_sdl.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/Nova.cpp"
)
target_compile_options(NovaSDL PRIVATE ${NOVA_FIRST_PARTY_WARNINGS})
target_link_libraries(NovaSDL PUBLIC Nova ${SDL2_LIBRARIES})
target_include_directories(NovaSDL PUBLIC ${SDL2_INCLUDE_DIRS})

# ==========================================
# Clouds display server & spatial UI
# ==========================================
# The session layer both entry points are built from: scene, compositor,
# presentation and the spatial UI. Nothing here touches SDL or ImGui, which is
# what lets the staged boot binary exist at all (plan B.3).
set(CLOUDS_SESSION_SOURCES
    "${CMAKE_CURRENT_SOURCE_DIR}/src/Clouds/SpatialNode.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/src/Clouds/Primitives.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/src/Clouds/SpatialScene.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/src/Clouds/SpatialCompositor.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/src/Clouds/SpatialBackendStack.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/src/Clouds/SpatialPresentLoop.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/src/Clouds/SpatialPresentSidecar.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/src/Clouds/SpatialWindowHost.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/src/Clouds/SpatialPopupHost.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/src/Clouds/SpatialSeatInput.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/src/Clouds/OatsBridge.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/src/Clouds/SpatialFilesystem.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/src/Clouds/CrmWorkspace.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/src/Clouds/DynamicSceneManager.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/src/Clouds/UI/UIComponents.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/src/Clouds/UI/UIDockBar.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/src/Clouds/UI/UIMenuBar.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/src/Clouds/UI/UIWindow.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/src/Clouds/UI/WindowManager.cpp"
)

set(CLOUDS_FIRST_PARTY_SOURCES
    ${CLOUDS_SESSION_SOURCES}
    "${CMAKE_CURRENT_SOURCE_DIR}/src/Clouds/ImGuiEngineOverlay.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/src/Clouds/EngineHUD.cpp"
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
    ${PIXMAN_INCLUDE_DIRS}
)

target_link_libraries(Clouds PRIVATE
    NovaSDL
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
# vazio - the staged boot entry (plan section B)
#
# Same session layer as Clouds, different composition: no ImGui, no window, and
# no source of its own that calls SDL. It presents into wlroots' own output
# buffers (plan D.2) and holds the session latent until a readiness token
# arrives. Deliberately a second TARGET and not a second copy of anything: every
# line it shares with Clouds is shared through CLOUDS_SESSION_SOURCES.
#
# It links no libSDL2: verified by `ldd vazio` and enforced by construction -
# Nova carries no SDL link edge in any scope, and the only SDL-calling
# translation unit lives in the separate NovaSDL archive, which this target does
# not link. It still COMPILES against SDL2's headers, because Core/nova_graphics.h
# reaches Core/modules/camera/camera.h and that declares
# Camera::processEvents(SDL_Event&). An include path is not a runtime dependency.
# This distinction matters here and nowhere else: an early-userspace boot binary
# carrying an unnecessary shared object is a boot-time fact, not a cosmetic one.
# ==========================================
add_executable(vazio
    ${CLOUDS_SESSION_SOURCES}
    "${CMAKE_CURRENT_SOURCE_DIR}/examples/vazio_main.cpp"
    "${PROTOCOL_GEN_DIR}/xdg-shell-protocol.h"
)
add_dependencies(vazio nova_shaders)

target_compile_options(vazio PRIVATE ${NOVA_FIRST_PARTY_WARNINGS})
target_compile_definitions(vazio PRIVATE WLR_USE_UNSTABLE)

target_include_directories(vazio PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${CMAKE_CURRENT_SOURCE_DIR}/include
    ${CMAKE_CURRENT_SOURCE_DIR}/Core
    ${PROTOCOL_GEN_DIR}
    ${Vulkan_INCLUDE_DIRS}
    ${FREETYPE_INCLUDE_DIRS}
    ${WLROOTS_INCLUDE_DIRS}
    ${WAYLAND_SERVER_INCLUDE_DIRS}
    ${LIBDRM_INCLUDE_DIRS}
    ${XKBCOMMON_INCLUDE_DIRS}
    ${PIXMAN_INCLUDE_DIRS}
)

target_link_libraries(vazio PRIVATE
    Nova
    oats_ffi
    nlohmann_json::nlohmann_json
    ${WLROOTS_LIBRARIES}
    ${WAYLAND_SERVER_LIBRARIES}
    ${LIBDRM_LIBRARIES}
    ${XKBCOMMON_LIBRARIES}
    ${Vulkan_LIBRARIES}
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
# Links NovaSDL, not Nova: wavecube_compute.cpp drives `class Nova`, whose
# translation unit calls SDL_Init/SDL_CreateWindow unconditionally. Compute-only
# at runtime, SDL-referencing at link time - stating that here rather than
# hiding it behind a transitive dependency is the whole point of the split.
add_executable(wavecube_compute "${CMAKE_CURRENT_SOURCE_DIR}/wavecube_compute.cpp")
target_compile_options(wavecube_compute PRIVATE ${NOVA_FIRST_PARTY_WARNINGS})
target_link_libraries(wavecube_compute PRIVATE NovaSDL)

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

# ==========================================
# Integration harnesses (tests/integration)
#
# These are the harnesses that used to live in scratch directories and were lost
# with them. They are in the repository and under CTest now, which is the whole
# point: a proof that only exists in /tmp is a proof that expires.
#
# Every one of them runs against a PRIVATE XDG_RUNTIME_DIR under the build tree
# and forces WLR_BACKENDS=headless. None of them can touch a live session:
# a compositor here binds its own socket, in its own runtime dir, with a
# pid-suffixed name. The runtime dir is kept SHORT on purpose - sockaddr_un's
# sun_path is 108 bytes and the failure when it overflows names neither the path
# nor the limit.
# ==========================================
set(NOVA_TEST_RUNTIME_ROOT "${CMAKE_CURRENT_BINARY_DIR}/trt")

# The session layer these harnesses drive. Same sources as `vazio`, compiled
# once more for the tests rather than linked out of an executable - identical
# defines and warning flags, so no ODR hazard, and the tests stay independent of
# how the product binaries happen to be composed.
# The generated protocol code is NOT listed here: it lives in the
# nova_client_protocols library above, which every harness links.
set(NOVA_INTEGRATION_SESSION_SOURCES
    ${CLOUDS_SESSION_SOURCES}
)

set(NOVA_INTEGRATION_INCLUDES
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${CMAKE_CURRENT_SOURCE_DIR}/include
    ${CMAKE_CURRENT_SOURCE_DIR}/Core
    ${CMAKE_CURRENT_SOURCE_DIR}/tests/integration
    ${PROTOCOL_GEN_DIR}
    ${Vulkan_INCLUDE_DIRS}
    ${FREETYPE_INCLUDE_DIRS}
    ${WLROOTS_INCLUDE_DIRS}
    ${WAYLAND_SERVER_INCLUDE_DIRS}
    ${WAYLAND_CLIENT_INCLUDE_DIRS}
    ${LIBDRM_INCLUDE_DIRS}
    ${XKBCOMMON_INCLUDE_DIRS}
    ${PIXMAN_INCLUDE_DIRS}
)

set(NOVA_INTEGRATION_LIBS
    Nova
    nova_client_protocols
    oats_ffi
    nlohmann_json::nlohmann_json
    ${WLROOTS_LIBRARIES}
    ${WAYLAND_SERVER_LIBRARIES}
    ${WAYLAND_CLIENT_LIBRARIES}
    ${LIBDRM_LIBRARIES}
    ${XKBCOMMON_LIBRARIES}
    ${Vulkan_LIBRARIES}
    ${FREETYPE_LIBRARIES}
    ${CMAKE_DL_LIBS}
)

# p1-class: protocol completeness and seat routing, server + forked real client.
add_executable(test_protocol_seat
    "${CMAKE_CURRENT_SOURCE_DIR}/tests/integration/test_protocol_seat.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/tests/integration/protocol_client_shell.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/tests/integration/protocol_client_seat.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/tests/integration/wl_client_kit.cpp"
    ${NOVA_INTEGRATION_SESSION_SOURCES}
)
add_dependencies(test_protocol_seat nova_shaders nova_client_protocol_headers)
target_compile_options(test_protocol_seat PRIVATE ${NOVA_FIRST_PARTY_WARNINGS})
target_compile_definitions(test_protocol_seat PRIVATE WLR_USE_UNSTABLE)
target_include_directories(test_protocol_seat PRIVATE ${NOVA_INTEGRATION_INCLUDES})
target_link_libraries(test_protocol_seat PRIVATE ${NOVA_INTEGRATION_LIBS})

# lt-class: three connect/disconnect cycles against one compositor, then a
# double stop().
add_executable(test_disconnect_cycle
    "${CMAKE_CURRENT_SOURCE_DIR}/tests/integration/test_disconnect_cycle.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/tests/integration/wl_client_kit.cpp"
    ${NOVA_INTEGRATION_SESSION_SOURCES}
)
add_dependencies(test_disconnect_cycle nova_shaders nova_client_protocol_headers)
target_compile_options(test_disconnect_cycle PRIVATE ${NOVA_FIRST_PARTY_WARNINGS})
target_compile_definitions(test_disconnect_cycle PRIVATE WLR_USE_UNSTABLE)
target_include_directories(test_disconnect_cycle PRIVATE ${NOVA_INTEGRATION_INCLUDES})
target_link_libraries(test_disconnect_cycle PRIVATE ${NOVA_INTEGRATION_LIBS})

# Nova teardown probes. probe_offscreen is the standing guard on the P0
# registry use-after-free: it constructs and destroys an offscreen NovaGraphics
# and dies at -O2 if a base-class registry ever again runs a cleanup that
# touches derived members.
add_executable(probe_offscreen "${CMAKE_CURRENT_SOURCE_DIR}/tests/integration/probe_offscreen.cpp")
target_compile_options(probe_offscreen PRIVATE ${NOVA_FIRST_PARTY_WARNINGS})
target_include_directories(probe_offscreen PRIVATE ${NOVA_INTEGRATION_INCLUDES})
target_link_libraries(probe_offscreen PRIVATE Nova ${CMAKE_DL_LIBS})

add_executable(probe_font_leak "${CMAKE_CURRENT_SOURCE_DIR}/tests/integration/probe_font_leak.cpp")
target_compile_options(probe_font_leak PRIVATE ${NOVA_FIRST_PARTY_WARNINGS})
target_include_directories(probe_font_leak PRIVATE ${NOVA_INTEGRATION_INCLUDES})
target_link_libraries(probe_font_leak PRIVATE Nova ${CMAKE_DL_LIBS})

# Present-loop defects D1-D4: multi-output sidecar bounds, frame-callback driver
# re-election, half-decided path selection, sidecar mode change. Drives the loop
# against a bare wlroots headless backend, because none of the four is reachable
# through a product binary that creates exactly one output and never changes mode.
add_executable(probe_present_loop
    "${CMAKE_CURRENT_SOURCE_DIR}/tests/integration/probe_present_loop.cpp"
    ${NOVA_INTEGRATION_SESSION_SOURCES}
)
add_dependencies(probe_present_loop nova_shaders nova_client_protocol_headers)
target_compile_options(probe_present_loop PRIVATE ${NOVA_FIRST_PARTY_WARNINGS})
target_compile_definitions(probe_present_loop PRIVATE WLR_USE_UNSTABLE)
target_include_directories(probe_present_loop PRIVATE ${NOVA_INTEGRATION_INCLUDES})
target_link_libraries(probe_present_loop PRIVATE ${NOVA_INTEGRATION_LIBS})

add_test(NAME protocol_seat COMMAND test_protocol_seat)
add_test(NAME disconnect_cycle COMMAND test_disconnect_cycle)
add_test(NAME nova_offscreen_teardown COMMAND probe_offscreen)
add_test(NAME nova_font_atlas_release COMMAND probe_font_leak)
add_test(NAME nova_font_atlas_release_explicit COMMAND probe_font_leak release)
add_test(NAME present_loop_election COMMAND probe_present_loop election)
add_test(NAME present_loop_sidecar COMMAND probe_present_loop sidecar)

# The staged-boot milestone drives the real `vazio` binary. $<TARGET_FILE:vazio>
# rather than a guessed path, so it follows whatever the generator decides.
add_test(NAME staged_boot_e2e
    COMMAND "${CMAKE_CURRENT_SOURCE_DIR}/tests/integration/staged_boot_e2e.sh"
            "$<TARGET_FILE:vazio>" "${NOVA_TEST_RUNTIME_ROOT}/e2e"
)

set_tests_properties(
    protocol_seat disconnect_cycle nova_offscreen_teardown nova_font_atlas_release
    nova_font_atlas_release_explicit present_loop_election present_loop_sidecar
    staged_boot_e2e
    PROPERTIES
        TIMEOUT 300
        WORKING_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}"
        ENVIRONMENT "WLR_BACKENDS=headless;WLR_RENDERER=pixman;LIBGL_ALWAYS_SOFTWARE=1"
)

# present_loop_election is the ONE test that must NOT force the pixman renderer.
# D2 - frame-callback driver re-election across two outputs - is only reachable
# on the zero-copy dmabuf-import path, and the probe correctly SKIPS itself when
# the sidecar path was selected. Forcing pixman here would leave a green test
# that had quietly stopped testing the thing it is named after (3 checks instead
# of 10). Its sibling present_loop_sidecar keeps pixman, because the CPU-copy
# path is exactly what it is for.
set_tests_properties(present_loop_election PROPERTIES
    ENVIRONMENT "WLR_BACKENDS=headless;LIBGL_ALWAYS_SOFTWARE=1"
)

# Each Wayland-serving harness gets its OWN runtime dir. Sharing one would let a
# stale socket from a previous run satisfy the next run's connect, which is the
# race that made the old out-of-tree lt harness flaky.
set_tests_properties(protocol_seat PROPERTIES
    ENVIRONMENT "WLR_BACKENDS=headless;WLR_RENDERER=pixman;LIBGL_ALWAYS_SOFTWARE=1;XDG_RUNTIME_DIR=${NOVA_TEST_RUNTIME_ROOT}/ps"
)
set_tests_properties(disconnect_cycle PROPERTIES
    ENVIRONMENT "WLR_BACKENDS=headless;WLR_RENDERER=pixman;LIBGL_ALWAYS_SOFTWARE=1;XDG_RUNTIME_DIR=${NOVA_TEST_RUNTIME_ROOT}/dc"
)

# --------------------------------------------------------------------------
# NEGATIVE CONTROLS
#
# Every harness carries modes that deliberately break one input so the run MUST
# fail. A test that has never been seen to fail is a test that has never been
# seen to work. Select one with VAZIO_TEST_NEG in the environment; ctest passes
# it through, so the whole thing is:
#
#     VAZIO_TEST_NEG=<mode> ctest --test-dir build -R <test>
#
#   protocol_seat
#     send        client never attaches a buffer -> nothing maps (28 failures)
#     configure   client never acks the initial configure -> wlroots refuses the
#                 map (33 failures)
#     nopointer   server never drives the pointer -> the whole seat phase is
#                 unfed (9 failures)
#   disconnect_cycle
#     earlystop   stop() before the last cycle -> cycle 3 cannot connect
#     nodispatch  stop servicing the event loop for one cycle -> that client hangs
#   staged_boot_e2e
#     abortlog    inject a `double free or corruption` line into the splash log ->
#                 the abort-signature gate must fire
#     noready     write the readiness token to a path vazio was not given -> the
#                 latent -> open flip never happens
#
# All seven were verified failing, and the unmodified run verified green, in
# both configurations.
# --------------------------------------------------------------------------

# --------------------------------------------------------------------------
# NOT wired into ctest, and deliberately so. Both need a rebuild with different
# flags or a different loader environment, and a ctest entry that silently does
# neither would be a test that reports on something other than what it names.
#
#   AddressSanitizer:
#     cmake -S . -B build-asan -DCMAKE_BUILD_TYPE=RelWithDebInfo \
#           -DCMAKE_CXX_FLAGS="-fsanitize=address -fno-omit-frame-pointer" \
#           -DCMAKE_EXE_LINKER_FLAGS="-fsanitize=address"
#     cmake --build build-asan -j && ctest --test-dir build-asan
#     Known environmental record: one 256-byte allocation under pthread_once
#     inside a dlopen'd Vulkan ICD. Reproduced by a Vulkan-only probe that links
#     no first-party compositor code; it is not ours.
#
#   Vulkan validation layers:
#     VK_LOADER_LAYERS_ENABLE=VK_LAYER_KHRONOS_validation \
#     VK_INSTANCE_LAYERS=VK_LAYER_KHRONOS_validation ctest --test-dir build -R nova_
#     Verify the layer actually loaded (VK_LOADER_DEBUG=layer) before believing
#     a zero-VUID result: a layer that failed to load is also silent.
# --------------------------------------------------------------------------

# Link hygiene: the boot binary must never acquire an SDL runtime dependency.
# Referenced by the NovaSDL rationale comment above the NovaSDL target.
add_test(NAME nova_link_hygiene
    COMMAND sh -c "! ldd $<TARGET_FILE:vazio> | grep -q libSDL2")
