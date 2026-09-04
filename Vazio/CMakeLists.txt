# Written by Richard Christopher, Copyright 2026 NeoTec Digital
# --- Vazio: the display server / graphical runtime environment. ---------------
# Depends on: Splash (and Nova through it). Does NOT depend on Clouds - see the
# THE LAYER CHAIN note in the root CMakeLists.txt: the two main() files are the
# only upward edges and they are not members of this archive.
#
# It owns the generated xdg-shell SERVER header, now through a target-level
# dependency on nova_xdg_shell_server_header rather than by listing the
# custom-command OUTPUT as a source. The reason is mechanical, not stylistic: an
# add_custom_command OUTPUT only grows a build rule for targets in the SAME
# directory, so with the rule in the root and this library here the header would
# simply never be generated - CMake configures clean and the compile fails on a
# missing include. The single-owner property that mattered is unchanged: exactly
# one target still drives that wayland-scanner invocation, so the two-owner race
# that produced half-written headers cannot come back. Every consumer picks the
# ordering up through its link edge. Same shape as nova_client_protocol_headers.
add_library(Vazio STATIC
    "${CMAKE_CURRENT_SOURCE_DIR}/SpatialCompositor.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/SpatialBackendStack.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/SpatialPresentLoop.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/SpatialPresentSidecar.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/SpatialWindowHost.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/SpatialPopupHost.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/SpatialSubsurfaceHost.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/SpatialSeatInput.cpp"
)
add_dependencies(Vazio nova_xdg_shell_server_header)
target_compile_options(Vazio PRIVATE ${NOVA_FIRST_PARTY_WARNINGS})

# PUBLIC, not PRIVATE: wlroots' headers are #error guards until WLR_USE_UNSTABLE
# is defined, and Vazio/SpatialCompositor.h and Vazio/SpatialPresentLoop.h are
# both consumed outside this archive (both main()s, three integration harnesses).
target_compile_definitions(Vazio PUBLIC WLR_USE_UNSTABLE)
target_include_directories(Vazio PUBLIC
    ${PROTOCOL_GEN_DIR}
    ${WLROOTS_INCLUDE_DIRS}
    ${WAYLAND_SERVER_INCLUDE_DIRS}
    ${LIBDRM_INCLUDE_DIRS}
    ${XKBCOMMON_INCLUDE_DIRS}
    ${PIXMAN_INCLUDE_DIRS}
)
target_link_libraries(Vazio PUBLIC
    Splash
    ${WLROOTS_LIBRARIES}
    ${WAYLAND_SERVER_LIBRARIES}
    ${LIBDRM_LIBRARIES}
    ${XKBCOMMON_LIBRARIES}
)
