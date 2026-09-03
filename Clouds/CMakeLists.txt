# Written by Richard Christopher, Copyright 2026 NeoTec Digital
# --- Clouds: the things running in the environment. Transitional. -------------
# Depends on: Splash (and Nova through it). NOT on Vazio: a tenant programs
# against the SDK, never against the compositor's internals. Sibling of Vazio.
#
# Clouds/apps/ImGuiEngineOverlay.cpp is the one SDL/ImGui-calling member of this
# archive, and this target deliberately links NEITHER. The archive member simply
# carries undefined ImGui_*/SDL_* symbols; a target that pulls it must supply
# them (link NovaSDL and compile the ImGui vendor blob) and fails loudly at link
# time otherwise, while a target that never references the overlay - `vazio`,
# test_scene_input - never pulls the member and never acquires SDL. Same
# construction as the NovaCoreLegacy member inside libNova.a, and it is what
# keeps `ldd vazio` SDL-free by construction rather than by luck.
add_library(Clouds STATIC
    "${CMAKE_CURRENT_SOURCE_DIR}/apps/SpatialFilesystem.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/apps/ImGuiEngineOverlay.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/ui/UIComponents.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/ui/UIDockBar.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/ui/UIMenuBar.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/ui/UIWindow.cpp"
)
target_compile_options(Clouds PRIVATE ${NOVA_FIRST_PARTY_WARNINGS})

# PRIVATE: only ImGuiEngineOverlay.cpp includes <imgui.h> and the backends.
# ImGuiEngineOverlay.h exposes SDL_Window*/SDL_Event, whose header comes through
# Nova's PUBLIC ${SDL2_INCLUDE_DIRS} - an include path, not a link edge.
target_include_directories(Clouds PRIVATE
    ${PROJECT_SOURCE_DIR}/extern/imgui
    ${PROJECT_SOURCE_DIR}/extern/imgui/backends
)
target_link_libraries(Clouds PUBLIC Splash)
