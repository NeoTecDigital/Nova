# Written by Richard Christopher, Copyright 2026 NeoTec Digital
#
# The Nova layer: the windowless Vulkan engine, and the SDL half kept out of it.
# ==========================================
# Nova engine library
# ==========================================
# Nova/Nova.cpp is deliberately absent: `class Nova` IS the SDL application wrapper
# (SDL_Init, SDL_CreateWindow, SDL_PollEvent, SDL_Vulkan_GetDrawableSize), so it
# belongs to the NovaSDL target below, not to the windowless engine archive.
set(NOVA_SOURCES
    "${CMAKE_CURRENT_SOURCE_DIR}/core_base.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/core_device_select.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/nova_compute.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/nova_graphics.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/nova_swapchain.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/nova_offscreen.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/nova_dmabuf.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/nova_dmabuf_query.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/nova_dmabuf_import.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/modules/atomic/atomic.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/modules/pipeline/pipeline.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/modules/camera/camera.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/modules/camera/perspective.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/components/genesis.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/components/vma_impl.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/components/logger.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/pipeline/texture_bridge.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/pipeline/spatial_pipeline.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/pipeline/mesh_buffer.cpp"
)

add_library(Nova STATIC ${NOVA_SOURCES})
add_dependencies(Nova nova_shaders)

target_compile_options(Nova PRIVATE ${NOVA_FIRST_PARTY_WARNINGS})

# vma_impl.cpp is the VMA_IMPLEMENTATION carrier: it instantiates all 19,786
# lines of AMD's VulkanMemoryAllocator and holds nothing else, so a target that
# needs GPU allocation extracts that and only that. The carrier used to be
# modules/management.cpp, whose other 172 members were NovaCoreLegacy; see the
# comment at the head of components/vma_impl.cpp for what that cost.
#
# NOVA_FASTGLTF_CARRIER_SUPPRESSIONS has no consumer here any more:
# components/obj_loader.cpp was the only fastgltf carrier, and it went with
# NovaCoreLegacy, whose type its whole API took. The vendored fastgltf tree
# under components/extern/ is untouched - it is simply no longer compiled by us.
set_source_files_properties(
    "${CMAKE_CURRENT_SOURCE_DIR}/components/vma_impl.cpp"
    PROPERTIES COMPILE_OPTIONS "${NOVA_VMA_CARRIER_SUPPRESSIONS}"
)

# The repository root is the ONLY first-party search root. Every quoted include
# in the tree is either root-relative ("Nova/...", "Splash/...", "Vazio/...",
# "Clouds/...") or a same-directory sibling ("./core_base.h"), and a
# same-directory sibling resolves against the including file, not against a -I.
# The old -I<root>/Nova - the faithful rename of -I<root>/Core - had no dependent
# include left once A3 made the last one explicit, so it is gone: a second search
# root is a second way to spell the same header, and that is how a layer boundary
# gets crossed without anyone naming it.
# ${PROJECT_SOURCE_DIR}, not ${CMAKE_CURRENT_SOURCE_DIR}: this file now lives in
# Nova/, and the search root being exported is the REPOSITORY root. Spelling it
# the other way here would re-export -I<root>/Nova - the second spelling of every
# Nova header that A3 removed - and nothing would fail to build.
target_include_directories(Nova PUBLIC
    ${PROJECT_SOURCE_DIR}
    ${Vulkan_INCLUDE_DIRS}
    ${FREETYPE_INCLUDE_DIRS}
)

# SDL2 headers - and ONLY the headers - are a compile requirement of any consumer
# that includes Nova/nova_graphics.h: that header pulls in
# Nova/modules/camera/camera.h, which includes <SDL2/SDL_events.h> for
# Camera::processEvents(SDL_Event&). `vazio` is such a consumer
# (Vazio/vazio_main.cpp constructs an offscreen NovaGraphics), so the include
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
# call sites now live in Nova/nova_sdl.cpp, in the separate NovaSDL archive below.

target_compile_definitions(Nova PUBLIC
    NOVA_COMPUTE_ENABLED
)

# ==========================================
# NovaSDL - the window-system half of the engine
#
# One translation unit: Nova/nova_sdl.cpp, holding NovaSDL::vulkanInstanceExtensions
# and the SDL_Window constructor of NovaGraphics. Kept OUT of libNova.a so that a
# target which never opens a window cannot accidentally acquire libSDL2 through
# the archive. Link this only from targets that do.
#
# Two translation units live here: Nova/nova_sdl.cpp (the extension query and the
# SDL_Window constructor of NovaGraphics) and Nova/Nova.cpp (`class Nova`, the SDL
# application wrapper that owns the window and the event pump). Both are
# SDL-calling by nature; the dependency direction is strictly NovaSDL -> Nova,
# never back, which is what keeps the two static archives acyclic on the link line.
#
# libNova.a now carries NO SDL reference at all. The last one was core.cpp.o -
# the deprecated NovaCoreLegacy calling SDL_Vulkan_GetInstanceExtensions - and B6
# deleted the class outright rather than splitting it across two archives: it was
# spread over six translation units, reached by no live call site, and landed in
# `vazio` only as collateral of modules/management.cpp being the VMA carrier.
# Recoverable at 0f40d65 should a use for it ever appear. The absence is asserted
# by the `nova_link_hygiene` test at the bottom of the root CMakeLists.
# ==========================================
add_library(NovaSDL STATIC
    "${CMAKE_CURRENT_SOURCE_DIR}/nova_sdl.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/Nova.cpp"
)
target_compile_options(NovaSDL PRIVATE ${NOVA_FIRST_PARTY_WARNINGS})
target_link_libraries(NovaSDL PUBLIC Nova ${SDL2_LIBRARIES})
target_include_directories(NovaSDL PUBLIC ${SDL2_INCLUDE_DIRS})
