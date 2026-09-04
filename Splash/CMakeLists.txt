# Written by Richard Christopher, Copyright 2026 NeoTec Digital
# --- Splash: the SDK. Primitives over Nova + OATS + wlroots' listener idiom. ---
# Depends on: Nova. Knows nothing of the compositor (Vazio) or its tenants
# (Clouds); those are its consumers, and PUBLIC Nova is the only edge it has.
add_library(Splash STATIC
    "${CMAKE_CURRENT_SOURCE_DIR}/Registry.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/SpatialNode.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/SpatialScene.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/Primitives.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/DynamicSceneManager.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/oats/OatsBridge.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/content/mesh_generators.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/content/spatial_font.cpp"
)
target_compile_options(Splash PRIVATE ${NOVA_FIRST_PARTY_WARNINGS})

# Splash/WaylandListener.h includes <wayland-server-core.h> and is a public
# header of this layer, so wayland-server is a compile requirement of every
# consumer - stated here rather than re-stated on each of them. OATS is a Splash
# link edge because Splash/oats/OatsBridge.cpp is the one TU that calls the C ABI.
target_include_directories(Splash PUBLIC ${WAYLAND_SERVER_INCLUDE_DIRS})
target_link_libraries(Splash PUBLIC
    Nova
    oats_ffi
    nlohmann_json::nlohmann_json
    ${WAYLAND_SERVER_LIBRARIES}
)
