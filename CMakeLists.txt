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
# (Splash/oats/OatsBridge.h, Splash/oats/OatsBridge.cpp). They previously
# resolved only because they happen to live in /usr/include.
find_package(glm QUIET)
if(NOT glm_FOUND)
    message(FATAL_ERROR
        "glm not found. It is a hard dependency of Splash/oats/OatsBridge.h.\n"
        "Install the GLM development package (Arch: glm, Debian: libglm-dev) or\n"
        "pass -Dglm_DIR=<path containing glmConfig.cmake>.")
endif()

find_package(nlohmann_json QUIET)
if(NOT nlohmann_json_FOUND)
    message(FATAL_ERROR
        "nlohmann_json not found. It is a hard dependency of Splash/oats/OatsBridge.cpp.\n"
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

# wlr/render/pixman.h is included by Vazio/SpatialPresentLoop.h for the
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

# ONE target owns the server header, for the same reason nova_client_protocols
# owns the client ones. The library that needs it - Vazio - now lives in its own
# directory, and an add_custom_command OUTPUT only grows a build rule for targets
# declared in the SAME directory as the command. Listing the OUTPUT as a source
# from Vazio/CMakeLists.txt would configure without complaint and then fail to
# compile against a header nothing ever generated. A custom target crosses the
# directory boundary and keeps the single owner.
#
# The private code (.c) needs no such target: its one consumer, vazio-dev, is
# still declared here in the root alongside the rule.
add_custom_target(nova_xdg_shell_server_header
    DEPENDS "${PROTOCOL_GEN_DIR}/xdg-shell-protocol.h")

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
# Nova/pipeline/spatial_pipeline.h and the pipeline->build call
# in Vazio/vazio_dev_main.cpp -- so output goes next to the binaries.
#
# Output basenames are irregular in the existing tree (_v/_f vs _vert/_frag), so
# the mapping is explicit rather than derived from a naming convention.
#
# The two halves are rewritten INDEPENDENTLY, and the asymmetry below is
# deliberate. The left column is a source path and followed Core/ -> Nova/ in the
# layer move. The right column is the RUNTIME layout: it is what the loader
# opens - exe-relative through resolveAssetPath in Vazio/vazio_main.cpp,
# CWD-relative from the dev entry, and as the default arguments of
# SpatialPipeline::build in Nova/pipeline/spatial_pipeline.h. Renaming an output
# is an interface change for those three call sites, so the
# Core/components/shaders/* outputs keep their names even though their sources
# moved. Those four have no code consumer today; when one appears, rename the
# output and its consumers together, never the output alone.
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
    "Nova/components/shaders/sq1.vert|Core/components/shaders/sq1_v.spv"
    "Nova/components/shaders/sq1.frag|Core/components/shaders/sq1_f.spv"
    "Nova/components/shaders/tri.vert|Core/components/shaders/tri_vert.spv"
    "Nova/components/shaders/tri.frag|Core/components/shaders/tri_frag.spv"
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
# THE LAYER CHAIN: Nova <- Splash <- {Vazio, Clouds}
#
# Every layer is its OWN static archive and every archive names the layer
# directly beneath it with `target_link_libraries(<lib> PUBLIC <lower>)`. The
# point is not tidiness: PUBLIC is what makes the direction load-bearing. A TU
# in Splash that reaches up into Clouds now has no archive to resolve against
# and the build stops with an undefined reference, so a layering violation is a
# link error rather than something a reviewer has to notice.
#
# The chain is a DAG, not a stack. Vazio and Clouds are SIBLINGS - both build on
# Splash, neither knows the other exists - which is exactly the claim the plan
# makes about the runtime environment versus the things running in it.
#
# The two main() files are the ONLY code in the tree that spans that fork:
# Vazio/vazio_main.cpp and Vazio/vazio_dev_main.cpp each include
# Clouds/apps/*.h. They are composition roots and they sit ABOVE both siblings
# in the link order even though they live in Vazio/. They are therefore compiled
# into their executables and are deliberately NOT members of libVazio.a -
# globbing them into the library would invert the chain and hand Vazio a
# dependency on Clouds that nothing else in the layer has.
#
# Each layer declares its own build in its own directory. The root keeps what is
# genuinely shared - dependency discovery, the generators, the composition-root
# binaries and the test registrations - and stops restating what belongs to a
# layer. add_subdirectory order follows the chain, and every subdirectory needs
# the generators and the warning policy above it to already exist.
# ==========================================
add_subdirectory(Nova)
add_subdirectory(Splash)
add_subdirectory(Vazio)
add_subdirectory(Clouds)


# ==========================================
# vazio-dev - the nested SDL/ImGui development harness
#
# Renamed from the executable `Clouds`, which collided with the source directory
# of the same name now that Clouds/ is a real layer with its own archive.
# ==========================================
set(VAZIO_DEV_VENDOR_SOURCES
    "${CMAKE_CURRENT_SOURCE_DIR}/extern/imgui/imgui.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/extern/imgui/imgui_demo.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/extern/imgui/imgui_draw.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/extern/imgui/imgui_tables.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/extern/imgui/imgui_widgets.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/extern/imgui/backends/imgui_impl_sdl2.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/extern/imgui/backends/imgui_impl_vulkan.cpp"
)

# Wholly third-party translation units: nothing in them is ours to fix.
set_source_files_properties(${VAZIO_DEV_VENDOR_SOURCES} PROPERTIES COMPILE_OPTIONS "-w")
set_source_files_properties("${PROTOCOL_GEN_DIR}/xdg-shell-protocol.c" PROPERTIES COMPILE_OPTIONS "-w")

# vazio_dev_main.cpp is a composition root, not a library member: it is one of
# the two files in the tree that reaches across the Vazio/Clouds fork. It is
# compiled straight into the executable, which is what puts it above both
# siblings on the link line. The ImGui vendor blob and the xdg-shell private
# code are likewise this binary's own sources - the only binary that needs them.
add_executable(vazio-dev
    "${CMAKE_CURRENT_SOURCE_DIR}/Vazio/vazio_dev_main.cpp"
    ${VAZIO_DEV_VENDOR_SOURCES}
    "${PROTOCOL_GEN_DIR}/xdg-shell-protocol.c"
)
add_dependencies(vazio-dev nova_shaders)

target_compile_options(vazio-dev PRIVATE ${NOVA_FIRST_PARTY_WARNINGS})

target_include_directories(vazio-dev PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/extern/imgui
    ${CMAKE_CURRENT_SOURCE_DIR}/extern/imgui/backends
)

# Vazio and Clouds are the two layer archives; NovaSDL is what supplies the
# SDL_* symbols that the ImGuiEngineOverlay member of libClouds.a leaves
# undefined. Everything else - Splash, Nova, wlroots, Vulkan, freetype, OATS -
# arrives through the PUBLIC edges the layers declare, which is the whole point
# of declaring them.
target_link_libraries(vazio-dev PRIVATE
    Vazio
    Clouds
    NovaSDL
)

# ==========================================
# vazio - the staged boot entry (plan section B)
#
# Same layers as vazio-dev, different composition: no ImGui, no window, and no
# source of its own that calls SDL. It presents into wlroots' own output buffers
# (plan D.2) and holds the session latent until a readiness token arrives. Not a
# second copy of anything - every line it shares with vazio-dev is shared through
# libVazio.a, libClouds.a and libSplash.a.
#
# It links no libSDL2, and now by two independent constructions. Nova carries no
# SDL link edge in any scope and the SDL-calling engine TUs live in the separate
# NovaSDL archive, which this target does not link; and the one SDL-calling
# member of libClouds.a - the ImGui overlay - is not referenced from
# vazio_main.cpp, so the linker never extracts it. Adding a reference to it here
# would fail to link rather than quietly grow a DT_NEEDED.
#
# It still COMPILES against SDL2's headers, because Nova/nova_graphics.h reaches
# Nova/modules/camera/camera.h and that declares Camera::processEvents(SDL_Event&).
# An include path is not a runtime dependency. This distinction matters here and
# nowhere else: an early-userspace boot binary carrying an unnecessary shared
# object is a boot-time fact, not a cosmetic one. Enforced by nova_link_hygiene.
#
# vazio_main.cpp is the second composition root and, like its sibling, is
# compiled into the executable rather than into libVazio.a.
# ==========================================
add_executable(vazio
    "${CMAKE_CURRENT_SOURCE_DIR}/Vazio/vazio_main.cpp"
)
add_dependencies(vazio nova_shaders)

target_compile_options(vazio PRIVATE ${NOVA_FIRST_PARTY_WARNINGS})

# The layer chain, named in dependency order. Nova is listed even though Vazio
# and Clouds both reach it PUBLIC-ly through Splash: this binary is the
# architecture's statement of what it is built out of, and the engine is part of
# that statement.
target_link_libraries(vazio PRIVATE
    Vazio
    Clouds
    Nova
)

# ==========================================
# WaveCube compute driver
#
# Previously built only by the root Makefile, which carried a divergent copy of
# NOVA_SOURCES. That divergence is how modules/atomic/atomic.cpp and
# modules/pipeline/scene.cpp were lost from the CMake build - scene.cpp has since
# gone with NovaCoreLegacy in B6, atomic.cpp has not. The Makefile now delegates
# here; this is the single definition of the Nova source set.
# ==========================================
# Links NovaSDL, not Nova: Nova/wavecube_compute.cpp drives `class Nova`, whose
# translation unit calls SDL_Init/SDL_CreateWindow unconditionally. Compute-only
# at runtime, SDL-referencing at link time - stating that here rather than
# hiding it behind a transitive dependency is the whole point of the split.
add_executable(wavecube_compute "${CMAKE_CURRENT_SOURCE_DIR}/Nova/wavecube_compute.cpp")
target_compile_options(wavecube_compute PRIVATE ${NOVA_FIRST_PARTY_WARNINGS})
target_link_libraries(wavecube_compute PRIVATE NovaSDL)

# ==========================================
# Seam benchmarks & limit integration tests
# ==========================================
add_executable(test_seam_math_raycast
    "${CMAKE_CURRENT_SOURCE_DIR}/tests/test_seam_math_raycast.cpp"
)
target_compile_options(test_seam_math_raycast PRIVATE ${NOVA_FIRST_PARTY_WARNINGS})
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
target_link_libraries(test_seam_ffi_roundtrip PRIVATE
    Nova
    oats_ffi
    ${CMAKE_DL_LIBS}
)

# Scene-graph input routing. It LINKS Splash and Clouds rather than carrying a
# hand-copied list of their sources: the copy had to be kept in step by hand and
# said nothing about why those seven files and not others. The claim the target
# makes now is a layering claim - this test needs the SDK and its tenants and
# nothing above them, so it does not link Vazio and therefore has no compositor
# and no wlroots. That claim is enforced: adding a compositor call here stops
# resolving. It also never pulls the ImGui overlay member out of libClouds.a, so
# it acquires no SDL.
add_executable(test_scene_input
    "${CMAKE_CURRENT_SOURCE_DIR}/tests/test_scene_input.cpp"
)
target_compile_options(test_scene_input PRIVATE ${NOVA_FIRST_PARTY_WARNINGS})
target_link_libraries(test_scene_input PRIVATE
    Splash
    Clouds
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

# The harnesses LINK the layer archives rather than recompiling them. Each one
# used to list the whole upper-layer source blob, so the same eighteen
# translation units were built five times over and every harness carried its own
# copy of the compositor. They now drive the same libVazio.a/libClouds.a the
# product binaries do, which is the stronger claim: a harness that passes has
# exercised the shipped object code, not a private rebuild of it that happens to
# use the same flags. The generated CLIENT protocol code is not listed here
# either - it lives in nova_client_protocols, which every harness links.
#
# The harnesses are clients as well as servers, so they add the client-side
# wayland include dir and their own tests/integration root on top of what the
# layers already export PUBLIC-ly.
set(NOVA_INTEGRATION_INCLUDES
    ${CMAKE_CURRENT_SOURCE_DIR}/tests/integration
    ${WAYLAND_CLIENT_INCLUDE_DIRS}
)

set(NOVA_INTEGRATION_LIBS
    Vazio
    Clouds
    nova_client_protocols
    ${WAYLAND_CLIENT_LIBRARIES}
)

# p1-class: protocol completeness and seat routing, server + forked real client.
add_executable(test_protocol_seat
    "${CMAKE_CURRENT_SOURCE_DIR}/tests/integration/test_protocol_seat.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/tests/integration/protocol_client_shell.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/tests/integration/protocol_client_seat.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/tests/integration/wl_client_kit.cpp"
)
add_dependencies(test_protocol_seat nova_shaders nova_client_protocol_headers)
target_compile_options(test_protocol_seat PRIVATE ${NOVA_FIRST_PARTY_WARNINGS})
target_include_directories(test_protocol_seat PRIVATE ${NOVA_INTEGRATION_INCLUDES})
target_link_libraries(test_protocol_seat PRIVATE ${NOVA_INTEGRATION_LIBS})

# lt-class: three connect/disconnect cycles against one compositor, then a
# double stop().
add_executable(test_disconnect_cycle
    "${CMAKE_CURRENT_SOURCE_DIR}/tests/integration/test_disconnect_cycle.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/tests/integration/wl_client_kit.cpp"
)
add_dependencies(test_disconnect_cycle nova_shaders nova_client_protocol_headers)
target_compile_options(test_disconnect_cycle PRIVATE ${NOVA_FIRST_PARTY_WARNINGS})
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

# Links Splash, not Nova: B1 moved SpatialFont out of the engine and into the
# SDK's content layer, so the atlas-lifetime seam this probe drives now lives in
# libSplash.a. Nova still arrives - PUBLIC-ly, through Splash - and it has to,
# because the probe constructs a NovaGraphics and a TextureBridge directly. What
# changed is which archive OWNS the class under test, and naming Nova alone here
# would fail to link rather than quietly test nothing.
add_executable(probe_font_leak "${CMAKE_CURRENT_SOURCE_DIR}/tests/integration/probe_font_leak.cpp")
target_compile_options(probe_font_leak PRIVATE ${NOVA_FIRST_PARTY_WARNINGS})
target_include_directories(probe_font_leak PRIVATE ${NOVA_INTEGRATION_INCLUDES})
target_link_libraries(probe_font_leak PRIVATE Splash ${CMAKE_DL_LIBS})

# Present-loop defects D1-D4: multi-output sidecar bounds, frame-callback driver
# re-election, half-decided path selection, sidecar mode change. Drives the loop
# against a bare wlroots headless backend, because none of the four is reachable
# through a product binary that creates exactly one output and never changes mode.
add_executable(probe_present_loop
    "${CMAKE_CURRENT_SOURCE_DIR}/tests/integration/probe_present_loop.cpp"
)
add_dependencies(probe_present_loop nova_shaders nova_client_protocol_headers)
target_compile_options(probe_present_loop PRIVATE ${NOVA_FIRST_PARTY_WARNINGS})
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
#   layer_purity
#     demangler   point the demangler at /bin/cat -> the demangler self-test
#                 must reject it instead of attributing nothing
#     nodetect    blind both classifiers -> the detector self-test must fire
#                 before a single archive is opened
#     nomangle    disable the mangled-name classifier -> the two independent
#                 classifiers must stop agreeing
#     noedge      assert a Splash -> Clouds edge, which the invariant forbids ->
#                 the expected-edge gate must report it zero and fail
#
# All eleven were verified failing, and the unmodified run verified green, in
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

# Link hygiene: TWO invariants about what the boot binary is allowed to contain.
# Still named for the binary it guards - `vazio` - which the vazio-dev rename did
# not touch; $<TARGET_FILE:vazio> resolves to the same staged-boot entry it
# always did.
#
# (1) No SDL runtime dependency. Two constructions have to hold for this, and
#     this test is what notices if either stops: Nova carries no SDL link edge in
#     any scope (see the NovaSDL rationale in Nova/CMakeLists.txt), and the one
#     SDL-calling member of libClouds.a is never referenced from vazio_main.cpp
#     (see the Clouds library).
#
# (2) No NovaCoreLegacy. This half is new, and it is here because the comment
#     next to (1) used to IMPLY it while the binary quietly carried 188 legacy
#     symbols. Nothing referenced them: modules/management.cpp was the
#     VMA_IMPLEMENTATION carrier, so allocating memory extracted that object and
#     the 172 CoreLegacy members sharing its TU, whose two unresolved calls
#     (getBufferInfo, createBeginInfo) then extracted modules/presentation.cpp
#     and 16 more. B6 moved the #define to Nova/components/vma_impl.cpp - which
#     took the count to zero - and then deleted the class. The count is asserted
#     rather than described, because "no live code calls it" was true the whole
#     time the symbols were in the binary. Matching on the mangled text needs no
#     demangler: `CoreLegacy` appears literally in every mangling of a member of
#     Nova::CoreLegacy, so grep over raw `nm` output cannot under-count.
#
# A test that only ever passed by construction is worth keeping precisely
# because the construction can be broken by a one-line link change - or, as (2)
# records, by a #define in the wrong translation unit.
add_test(NAME nova_link_hygiene
    COMMAND sh -c "! ldd $<TARGET_FILE:vazio> | grep -q libSDL2 && [ $(nm $<TARGET_FILE:vazio> | grep -c CoreLegacy) -eq 0 ]")

# ==========================================
# Layer purity
#
# The layer chain above is enforced at link time only PER TARGET: static-archive
# scan order decides whether an upward reference is caught, and A4 measured two
# targets - `vazio` and `test_scene_input` - building straight through a
# deliberately seeded violation because their own objects had already extracted
# the Clouds members before libSplash.a was scanned. An invariant that holds
# only for the targets that happen to scan in the right order is not enforced,
# it is lucky.
#
# This test asserts the invariant on the object code itself, against the
# archives this build just produced: no archive may reference or define a symbol
# belonging to a layer above it, and Vazio and Clouds - siblings - may not name
# each other. $<TARGET_FILE:> rather than guessed paths, so it follows the
# generator and cannot drift from what was actually compiled.
#
# LLVM_CXXFILT is resolved here rather than left to the script's PATH so the
# configured toolchain decides. If it is absent the test FAILS with an install
# instruction; it does not skip, and it does not fall back to GCC's c++filt,
# which cannot read C++20 requires-clause manglings and would leave symbols
# silently unattributed. The script's own gates (demangler self-test, detector
# self-test, dual-classifier agreement, non-zero downward edges) are what stop a
# broken analysis from reporting a clean sheet; see its header comment.
# ==========================================
find_program(LLVM_CXXFILT llvm-cxxfilt)
if(NOT LLVM_CXXFILT)
    message(STATUS
        "llvm-cxxfilt not found: the layer_purity test will FAIL until it is "
        "installed (Arch: llvm, Debian: llvm). It is not optional - GCC's "
        "c++filt cannot demangle C++20 requires-clause names and is not a "
        "substitute.")
endif()

add_test(NAME layer_purity
    COMMAND "${CMAKE_CURRENT_SOURCE_DIR}/tests/integration/layer_purity.sh"
            Nova      Nova   "$<TARGET_FILE:Nova>"
            NovaSDL   Nova   "$<TARGET_FILE:NovaSDL>"
            Splash    Splash "$<TARGET_FILE:Splash>"
            Vazio     Vazio  "$<TARGET_FILE:Vazio>"
            Clouds    Clouds "$<TARGET_FILE:Clouds>"
            protocols none   "$<TARGET_FILE:nova_client_protocols>"
)
set_tests_properties(layer_purity PROPERTIES
    TIMEOUT 120
    WORKING_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}"
)
if(LLVM_CXXFILT)
    set_tests_properties(layer_purity PROPERTIES
        ENVIRONMENT "LLVM_CXXFILT=${LLVM_CXXFILT}")
endif()
