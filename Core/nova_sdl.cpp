// Written by Richard Christopher, Copyright 2026 NeoTec Digital
//
// The SDL-facing half of NovaGraphics. This is the one translation unit in the
// engine that references libSDL2, and it is built into the `NovaSDL` archive
// rather than libNova.a so that windowless targets (`vazio`, `wavecube_compute`,
// the test binaries) never pull an SDL symbol and therefore never link the
// library. See Core/nova_sdl.h for the rationale.

#include "./nova_sdl.h"
#include "./nova_graphics.h"

#include <SDL2/SDL_vulkan.h>

namespace NovaSDL {

std::vector<const char*> vulkanInstanceExtensions()
{
    unsigned int count = 0;
    if (SDL_Vulkan_GetInstanceExtensions(nullptr, &count, nullptr) != SDL_TRUE) {
        report(LOGGER::ERROR, "NovaSDL - SDL_Vulkan_GetInstanceExtensions(count) failed: %s",
               SDL_GetError());
        return {};
    }

    std::vector<const char*> extensions(count);
    if (SDL_Vulkan_GetInstanceExtensions(nullptr, &count, extensions.data()) != SDL_TRUE) {
        report(LOGGER::ERROR, "NovaSDL - SDL_Vulkan_GetInstanceExtensions(names) failed: %s",
               SDL_GetError());
        return {};
    }

    return extensions;
}

}  // namespace NovaSDL

// Moved here verbatim from Core/nova_graphics.cpp, minus the inline extension
// query (now NovaSDL::vulkanInstanceExtensions) and the duplicated bring-up
// tail (now NovaGraphics::completeSurfaceBackedInit, shared with the
// external-surface constructor).
NovaGraphics::NovaGraphics(VkExtent2D extent, const std::string& debug_level, struct SDL_Window* window)
    : NovaCore(debug_level)
{
    report(LOGGER::INFO, "NovaGraphics - Initializing graphics mode with SDL window ..");

    setWindowExtent(extent);

    // Initialize base resources WITH the surface extensions SDL asks for.
    createVulkanInstance(NovaSDL::vulkanInstanceExtensions());

    // Create Vulkan surface from SDL window using the valid VkInstance
    if (!SDL_Vulkan_CreateSurface(window, instance, &surface)) {
        report(LOGGER::ERROR, "NovaGraphics - Failed to create Vulkan surface from SDL window: %s",
               SDL_GetError());
        return;
    }

    completeSurfaceBackedInit();

    report(LOGGER::INFO, "NovaGraphics - Initialized successfully");
}
