// Written by Richard Christopher, Copyright 2026 NeoTec Digital
#pragma once

#include <vector>

/**
 * Nova::SDL - the ONLY window-system boundary in the Nova engine.
 *
 * Every call into libSDL2 that Nova makes lives in Core/nova_sdl.cpp, and that
 * translation unit is compiled into its own archive (target `Nova::SDL`) rather
 * than into libNova.a. The split is a link-time fact, not a style preference:
 * `vazio` is an early-userspace boot binary that opens no window, and while the
 * SDL call sites sat in core_base.cpp and nova_graphics.cpp - two archive
 * members every consumer pulls - every Nova consumer resolved against libSDL2
 * and carried it as a runtime dependency.
 *
 * Link `Nova::SDL` from a target only if that target opens an SDL window.
 *
 * This header intentionally pulls in no SDL header: `struct SDL_Window` stays
 * incomplete for consumers, so including it costs nothing.
 */
namespace Nova::SDL {

/**
 * The instance-level Vulkan extensions SDL's video driver requires.
 *
 * Returns the list SDL_Vulkan_GetInstanceExtensions reports (VK_KHR_surface
 * plus the platform surface extension for the active driver). The strings are
 * owned by SDL and are valid for the life of the process; the vector is the
 * caller's.
 *
 * SDL_Init(SDL_INIT_VIDEO) must already have succeeded. Returns an empty vector
 * and reports at ERROR if SDL declines the query - passing that empty list on
 * to Core::createVulkanInstance produces an instance with no WSI support,
 * which then fails loudly at surface creation rather than silently rendering
 * nowhere.
 */
std::vector<const char*> vulkanInstanceExtensions();

}  // namespace Nova::SDL
