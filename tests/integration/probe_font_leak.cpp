// Written by Richard Christopher, Copyright 2026 NeoTec Digital
//
// The font-atlas ownership probe. Two variants, one binary:
//
//   (no argument)  the atlas is released implicitly, by ~SpatialFont, and the
//                  VMA allocator must then tear down holding nothing. Before
//                  the ownership fix this aborted inside vmaDestroyAllocator.
//   release        the caller returns the atlas EXPLICITLY first, so
//                  ~SpatialFont sees a handle it has already given back. The
//                  release must be idempotent - a second free here is the bug
//                  this variant exists to catch.
//
// Both variants must exit 0. The whole assertion is the teardown, so the exit
// code is the result and the checks only establish that there was something
// real to tear down.
#include "Core/nova_graphics.h"
#include "Core/modules/spatial_pipeline/spatial_font.h"
#include "Core/modules/spatial_pipeline/texture_bridge.h"
#include "Core/components/logger.h"

#include <cstdio>
#include <cstring>
#include <memory>
#include <string>

namespace {

int g_checks = 0;
int g_failures = 0;

void check(bool ok, const char* what) {
    ++g_checks;
    if (!ok) ++g_failures;
    std::printf("[%s] %s\n", ok ? " ok " : "FAIL", what);
    std::fflush(stdout);
}

} // namespace

int main(int argc, char** argv) {
    const bool explicit_release = (argc > 1 && std::strcmp(argv[1], "release") == 0);
    std::printf("probe_font_leak: variant = %s\n", explicit_release ? "release" : "implicit");

    Nova::OffscreenConfig config = {};
    config.extent = { 320, 200 };
    config.request_dmabuf_import = false;

    {
        auto nova = std::make_unique<Nova::Graphics>(config, std::string("ERROR"));
        check(nova->getAllocator() != VK_NULL_HANDLE, "offscreen Nova has an allocator");

        // Declaration order IS the contract: the bridge must outlive the font,
        // because the font returns its atlas to the bridge in ~SpatialFont.
        // Reversing these two lines is the bug the header warns about.
        auto bridge = std::make_unique<Nova::TextureBridge>(nova.get());
        bridge->initialize();
        check(bridge->getDescriptorSetLayout() != VK_NULL_HANDLE,
              "TextureBridge initialized its descriptor pool and layout");

        auto font = std::make_unique<Nova::SpatialFont>(nova.get(), bridge.get());

        // No TTF path is given on purpose: the fallback atlas is a real VMA
        // image allocation, which is all this probe needs, and it removes any
        // dependency on a font being installed.
        font->loadFromFile("/nonexistent/vazio-probe-font.ttf", 32);

        std::shared_ptr<Nova::TextureHandle> atlas = font->getAtlasTexture();
        check(atlas != nullptr, "fallback atlas was built");
        check(atlas && atlas->image != VK_NULL_HANDLE, "atlas holds a live VkImage");

        // Taken BEFORE any release: this is the scene-node case, a copy of the
        // shared_ptr that outlives both the font and the release. releaseTexture
        // must zero the handle in place so this copy points at a null
        // TextureHandle rather than at destroyed Vulkan objects.
        std::shared_ptr<Nova::TextureHandle> survivor = atlas;

        if (explicit_release) {
            // releaseTexture takes the caller's shared_ptr by reference and
            // resets it, so `atlas` is null afterwards by design.
            bridge->releaseTexture(atlas);
            check(atlas == nullptr, "explicit releaseTexture reset the caller's own reference");
            check(survivor && survivor->image == VK_NULL_HANDLE,
                  "explicit releaseTexture zeroed the shared handle in place");
        }

        std::printf("[info] tearing down font -> bridge -> Nova\n");
        std::fflush(stdout);
        font.reset();
        check(survivor && survivor->image == VK_NULL_HANDLE,
              "the surviving handle copy was zeroed, not dangling");

        bridge.reset();
        check(true, "TextureBridge destroyed after the font returned its atlas");

        nova.reset();
        check(true, "VMA allocator torn down holding no live allocation");
    }

    std::printf("\nprobe_font_leak(%s): %d/%d checks passed, %d failures\n",
                explicit_release ? "release" : "implicit",
                g_checks - g_failures, g_checks, g_failures);
    std::fflush(stdout);
    return g_failures == 0 ? 0 : 1;
}
