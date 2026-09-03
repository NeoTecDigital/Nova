// Written by Richard Christopher, Copyright 2026 NeoTec Digital
//
// Offscreen Graphics acceptance probe, and the direct reproducer for the
// teardown use-after-free QA5 found: an optimized build used to render 18/18
// checks correctly and then die inside ~Core, because the cleanup lambda
// Graphics registered in the BASE registry touched DERIVED members that had
// already been destroyed.
//
// The teardown is therefore the last and most important assertion in the file.
// It is deliberately NOT wrapped in anything: the process exit code is the
// check. rc 0 = the registry entry ran once, at the right level, on live state.
#include "Nova/nova_graphics.h"
#include "Nova/nova_dmabuf_query.h"
#include "Nova/components/logger.h"

#include <unistd.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <string>
#include <vector>

namespace {

int g_checks = 0;
int g_failures = 0;

void check(bool ok, const char* what) {
    ++g_checks;
    if (!ok) ++g_failures;
    std::printf("[%s] %s\n", ok ? " ok " : "FAIL", what);
    std::fflush(stdout);
}

struct OwnedImage {
    VkImage image = VK_NULL_HANDLE;
    VmaAllocation allocation = VK_NULL_HANDLE;
    VkImageView view = VK_NULL_HANDLE;
};

bool createOwnedImage(Nova::Graphics& nova, VkExtent2D extent, VkFormat format, OwnedImage& out) {
    VkImageCreateInfo image_info = {
        .sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
        .imageType = VK_IMAGE_TYPE_2D,
        .format = format,
        .extent = { extent.width, extent.height, 1 },
        .mipLevels = 1,
        .arrayLayers = 1,
        .samples = VK_SAMPLE_COUNT_1_BIT,
        .tiling = VK_IMAGE_TILING_OPTIMAL,
        .usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT,
        .sharingMode = VK_SHARING_MODE_EXCLUSIVE,
        .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED
    };
    VmaAllocationCreateInfo alloc_info = {};
    alloc_info.usage = VMA_MEMORY_USAGE_GPU_ONLY;
    if (vmaCreateImage(nova.getAllocator(), &image_info, &alloc_info, &out.image,
                       &out.allocation, nullptr) != VK_SUCCESS) {
        return false;
    }

    VkImageViewCreateInfo view_info = {
        .sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,
        .image = out.image,
        .viewType = VK_IMAGE_VIEW_TYPE_2D,
        .format = format,
        .subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 }
    };
    return vkCreateImageView(nova.getDevice(), &view_info, nullptr, &out.view) == VK_SUCCESS;
}

void destroyOwnedImage(Nova::Graphics& nova, OwnedImage& owned) {
    if (owned.view != VK_NULL_HANDLE) {
        vkDestroyImageView(nova.getDevice(), owned.view, nullptr);
        owned.view = VK_NULL_HANDLE;
    }
    if (owned.image != VK_NULL_HANDLE) {
        vmaDestroyImage(nova.getAllocator(), owned.image, owned.allocation);
        owned.image = VK_NULL_HANDLE;
        owned.allocation = VK_NULL_HANDLE;
    }
}

// One render + readback, so the checks cover a real submission rather than only
// object construction. Returns the clear colour the GPU actually wrote.
bool renderAndReadBack(Nova::Graphics& nova, const Nova::RenderTarget& target, uint32_t& pixel_out) {
    VkFence fence = nova.renderToImage(target, [](VkCommandBuffer, uint32_t) {});
    if (fence == VK_NULL_HANDLE || !nova.waitForRender(fence)) return false;

    const size_t bytes = static_cast<size_t>(target.extent.width) * target.extent.height * 4;
    Nova::Buffer_T readback = nova.createEphemeralBuffer(bytes, VK_BUFFER_USAGE_TRANSFER_DST_BIT,
                                                   VMA_MEMORY_USAGE_CPU_ONLY);
    if (readback.allocation == VK_NULL_HANDLE) return false;

    VkBufferImageCopy region = {
        .bufferOffset = 0, .bufferRowLength = 0, .bufferImageHeight = 0,
        .imageSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 },
        .imageOffset = {0, 0, 0},
        .imageExtent = { target.extent.width, target.extent.height, 1 }
    };
    VkImage image = target.image;
    VkBuffer buffer = readback.buffer;
    nova.immediateSubmitGraphics([image, buffer, region](VkCommandBuffer cmd) {
        vkCmdCopyImageToBuffer(cmd, image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, buffer, 1, &region);
    });

    void* mapped = nullptr;
    bool ok = false;
    if (vmaMapMemory(nova.getAllocator(), readback.allocation, &mapped) == VK_SUCCESS && mapped) {
        vmaInvalidateAllocation(nova.getAllocator(), readback.allocation, 0, VK_WHOLE_SIZE);
        std::memcpy(&pixel_out, mapped, sizeof(pixel_out));
        vmaUnmapMemory(nova.getAllocator(), readback.allocation);
        ok = true;
    }
    vmaDestroyBuffer(nova.getAllocator(), readback.buffer, readback.allocation);
    return ok;
}


// ---------------------------------------------------------------------------
// TWO DEVICES IN ONE PROCESS
//
// The standing guard on Pass B item B5. vkGetMemoryFdPropertiesKHR used to be
// held in a process-wide `static` pair keyed on the last VkDevice asked about
// (nova_dmabuf_query.cpp:25-26). One device hid the problem completely; two
// live devices made the "cache" a one-entry thrash whose correctness rested
// entirely on the loader handing out the same thunk for every device - which a
// second ICD or a layer stack does not do. It is now per-instance state on
// Nova::Graphics, and this case is what says so out loud.
//
// Two logical devices on the one physical GPU is the real shape of the
// two-sessions-in-one-process test the architecture plan schedules.
// ---------------------------------------------------------------------------

// A dmabuf this process exported itself, so the two-device case can import a
// real buffer rather than only ask what could be imported.
struct ExportedBuffer {
    VkImage image = VK_NULL_HANDLE;
    VkDeviceMemory memory = VK_NULL_HANDLE;
    int fd = -1;
    Nova::DmabufAttributes attrs = {};
};

// Single-plane, explicitly-modified, exportable as a dmabuf. The modifier LIST
// form is used rather than the EXPLICIT form because the plane layouts are what
// we are trying to learn, not what we already have.
VkImage createExportImage(VkDevice device, VkExtent2D extent, VkFormat format, uint64_t modifier) {
    VkImageDrmFormatModifierListCreateInfoEXT modifier_list = {
        .sType = VK_STRUCTURE_TYPE_IMAGE_DRM_FORMAT_MODIFIER_LIST_CREATE_INFO_EXT,
        .drmFormatModifierCount = 1,
        .pDrmFormatModifiers = &modifier
    };
    VkExternalMemoryImageCreateInfo external = {
        .sType = VK_STRUCTURE_TYPE_EXTERNAL_MEMORY_IMAGE_CREATE_INFO,
        .pNext = &modifier_list,
        .handleTypes = VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT
    };
    VkImageCreateInfo info = {
        .sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
        .pNext = &external,
        .imageType = VK_IMAGE_TYPE_2D,
        .format = format,
        .extent = { extent.width, extent.height, 1 },
        .mipLevels = 1,
        .arrayLayers = 1,
        .samples = VK_SAMPLE_COUNT_1_BIT,
        .tiling = VK_IMAGE_TILING_DRM_FORMAT_MODIFIER_EXT,
        .usage = Nova::NOVA_IMPORT_USAGE,
        .sharingMode = VK_SHARING_MODE_EXCLUSIVE,
        .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED
    };

    VkImage image = VK_NULL_HANDLE;
    return vkCreateImage(device, &info, nullptr, &image) == VK_SUCCESS ? image : VK_NULL_HANDLE;
}

// Dedicated, exportable memory for that image, already bound.
VkDeviceMemory bindExportMemory(Nova::Graphics& nova, VkImage image) {
    VkMemoryRequirements requirements = {};
    vkGetImageMemoryRequirements(nova.getDevice(), image, &requirements);

    const uint32_t type_index =
        Nova::firstAllowedMemoryType(nova.getPhysicalDevice(), requirements.memoryTypeBits);
    if (type_index == UINT32_MAX) return VK_NULL_HANDLE;

    VkExportMemoryAllocateInfo exportable = {
        .sType = VK_STRUCTURE_TYPE_EXPORT_MEMORY_ALLOCATE_INFO,
        .handleTypes = VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT
    };
    VkMemoryDedicatedAllocateInfo dedicated = {
        .sType = VK_STRUCTURE_TYPE_MEMORY_DEDICATED_ALLOCATE_INFO,
        .pNext = &exportable,
        .image = image
    };
    VkMemoryAllocateInfo alloc = {
        .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
        .pNext = &dedicated,
        .allocationSize = requirements.size,
        .memoryTypeIndex = type_index
    };

    VkDeviceMemory memory = VK_NULL_HANDLE;
    if (vkAllocateMemory(nova.getDevice(), &alloc, nullptr, &memory) != VK_SUCCESS) {
        return VK_NULL_HANDLE;
    }
    if (vkBindImageMemory(nova.getDevice(), image, memory, 0) != VK_SUCCESS) {
        vkFreeMemory(nova.getDevice(), memory, nullptr);
        return VK_NULL_HANDLE;
    }

    return memory;
}

// The fd, plus the plane geometry an importer needs. vkGetMemoryFdKHR is
// resolved HERE, against this device, for the same reason Nova resolves its own:
// the entry point belongs to the device, not to the process.
bool describeExport(Nova::Graphics& nova, uint32_t drm_format, uint64_t modifier,
                    VkExtent2D extent, ExportedBuffer& out) {
    auto get_memory_fd = reinterpret_cast<PFN_vkGetMemoryFdKHR>(
        vkGetDeviceProcAddr(nova.getDevice(), "vkGetMemoryFdKHR"));
    if (get_memory_fd == nullptr) return false;

    VkMemoryGetFdInfoKHR fd_info = {
        .sType = VK_STRUCTURE_TYPE_MEMORY_GET_FD_INFO_KHR,
        .memory = out.memory,
        .handleType = VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT
    };
    if (get_memory_fd(nova.getDevice(), &fd_info, &out.fd) != VK_SUCCESS || out.fd < 0) {
        return false;
    }

    VkImageSubresource plane0 = { VK_IMAGE_ASPECT_MEMORY_PLANE_0_BIT_EXT, 0, 0 };
    VkSubresourceLayout layout = {};
    vkGetImageSubresourceLayout(nova.getDevice(), out.image, &plane0, &layout);

    out.attrs.width = extent.width;
    out.attrs.height = extent.height;
    out.attrs.drm_format = drm_format;
    out.attrs.modifier = modifier;
    out.attrs.plane_count = 1;
    out.attrs.planes[0] = { out.fd, static_cast<uint32_t>(layout.offset),
                            static_cast<uint32_t>(layout.rowPitch) };
    return true;
}

// A single-plane modifier this device can both export and import, or absent.
bool pickSinglePlaneModifier(Nova::Graphics& nova, uint32_t drm_format, uint64_t& out) {
    const VkFormat format = Nova::vulkanFormatFromDrm(drm_format);
    for (uint64_t modifier : nova.supportedDmabufModifiers(drm_format)) {
        uint32_t planes = 0;
        if (Nova::modifierPlaneCount(nova.getPhysicalDevice(), format, modifier, planes) &&
            planes == 1) {
            out = modifier;
            return true;
        }
    }
    return false;
}

bool exportDmabuf(Nova::Graphics& nova, uint32_t drm_format, VkExtent2D extent, ExportedBuffer& out) {
    uint64_t modifier = Nova::NOVA_DRM_FORMAT_MOD_INVALID;
    if (!pickSinglePlaneModifier(nova, drm_format, modifier)) return false;

    out.image = createExportImage(nova.getDevice(), extent,
                                  Nova::vulkanFormatFromDrm(drm_format), modifier);
    if (out.image == VK_NULL_HANDLE) return false;

    out.memory = bindExportMemory(nova, out.image);
    if (out.memory == VK_NULL_HANDLE) return false;

    return describeExport(nova, drm_format, modifier, extent, out);
}

void destroyExport(Nova::Graphics& nova, ExportedBuffer& owned) {
    if (owned.fd >= 0) { close(owned.fd); owned.fd = -1; }
    if (owned.image != VK_NULL_HANDLE) {
        vkDestroyImage(nova.getDevice(), owned.image, nullptr);
        owned.image = VK_NULL_HANDLE;
    }
    if (owned.memory != VK_NULL_HANDLE) {
        vkFreeMemory(nova.getDevice(), owned.memory, nullptr);
        owned.memory = VK_NULL_HANDLE;
    }
}

// Both instances answer the same capability questions, alternating between them
// so a one-entry per-process cache would have to re-resolve on every call.
void checkInterleavedQueries(Nova::Graphics& a, Nova::Graphics& b, uint32_t drm_format) {
    const std::vector<uint64_t> a_first = a.supportedDmabufModifiers(drm_format);
    const std::vector<uint64_t> b_first = b.supportedDmabufModifiers(drm_format);
    const std::vector<uint64_t> a_again = a.supportedDmabufModifiers(drm_format);
    const std::vector<uint64_t> b_again = b.supportedDmabufModifiers(drm_format);

    check(a.supportsDmabufImport() == b.supportsDmabufImport(),
          "two devices agree on dmabuf import capability");
    check(a_first == a_again && b_first == b_again,
          "interleaved A/B/A/B modifier queries are stable per instance");
    check(a_first == b_first,
          "both devices report the same XR24 modifier set");
    std::printf("[info] XR24 modifiers: A=%zu B=%zu\n", a_first.size(), b_first.size());
}

// The import half: one dmabuf, exported by A, imported by A and B while both
// devices are live. Skipped, reported, and not failed when the host cannot
// export - absence of a modifier path is a capability, not a defect.
void checkConcurrentImport(Nova::Graphics& a, Nova::Graphics& b, uint32_t drm_format) {
    const VkExtent2D extent = { 256, 128 };
    ExportedBuffer owned = {};

    if (!exportDmabuf(a, drm_format, extent, owned)) {
        destroyExport(a, owned);
        std::printf("[info] dmabuf export unavailable on this host; import half skipped\n");
        check(true, "two-device import half reported its availability");
        return;
    }
    check(true, "instance A exported a single-plane dmabuf");

    Nova::ImportedImage into_a = {};
    Nova::ImportedImage into_b = {};
    check(a.importDmabufAsImage(owned.attrs, into_a), "instance A imported the dmabuf");
    check(b.importDmabufAsImage(owned.attrs, into_b), "instance B imported the SAME dmabuf");
    check(into_a.valid() && into_b.valid(), "both imports are live at the same time");
    check(into_a.image != into_b.image, "the two imports are distinct device objects");

    // Back to A while B still holds its import: the alternation that made the
    // old process-static cache thrash, and that a wrong thunk would break.
    Nova::ImportedImage into_a_again = {};
    check(a.importDmabufAsImage(owned.attrs, into_a_again),
          "instance A imported again with instance B's import still live");

    a.releaseImportedImage(into_a_again);
    b.releaseImportedImage(into_b);
    a.releaseImportedImage(into_a);
    destroyExport(a, owned);
    check(true, "every import released with both devices still up");
}

void runTwoDeviceCase() {
    std::printf("\n[info] two-device case (B5: no process-static PFN)\n");
    std::fflush(stdout);

    Nova::OffscreenConfig config = {};
    config.extent = { 320, 200 };
    config.request_dmabuf_import = true;

    auto a = std::make_unique<Nova::Graphics>(config, "ERROR");
    auto b = std::make_unique<Nova::Graphics>(config, "ERROR");

    check(a->getDevice() != VK_NULL_HANDLE && b->getDevice() != VK_NULL_HANDLE,
          "two offscreen instances are live in one process");
    check(a->getDevice() != b->getDevice(),
          "the two instances hold DISTINCT logical devices");
    check(a->getInstance() != b->getInstance(),
          "the two instances hold distinct VkInstances");

    checkInterleavedQueries(*a, *b, Nova::NOVA_DRM_FORMAT_XRGB8888);
    if (a->supportsDmabufImport() && b->supportsDmabufImport()) {
        checkConcurrentImport(*a, *b, Nova::NOVA_DRM_FORMAT_XRGB8888);
    } else {
        std::printf("[info] dmabuf import unavailable on this host; import half skipped\n");
        check(true, "two-device import half reported its availability");
    }

    b.reset();
    check(a->getDevice() != VK_NULL_HANDLE, "instance A survives instance B's teardown");
    a.reset();
    check(true, "both instances destroyed without aborting");
}

} // namespace

int main() {
    const std::string debug_level = "ERROR";

    Nova::OffscreenConfig config = {};
    config.extent = { 640, 360 };
    config.request_dmabuf_import = true;

    // Scoped so the destructor runs BEFORE the summary is printed: a teardown
    // that aborts must not be able to print a passing summary first.
    {
        auto nova = std::make_unique<Nova::Graphics>(config, debug_level);

        check(nova->getDevice() != VK_NULL_HANDLE, "offscreen instance has a logical device");
        check(nova->getAllocator() != VK_NULL_HANDLE, "offscreen instance has a VMA allocator");
        check(nova->hasGraphicsImmediate(), "graphics-family immediate context is engaged");

        const VkFormat format = VK_FORMAT_B8G8R8A8_UNORM;
        VkRenderPass pass = nova->getOffscreenRenderPass(format, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
        check(pass != VK_NULL_HANDLE, "offscreen render pass created for BGRA/TRANSFER_SRC");

        VkRenderPass again = nova->getOffscreenRenderPass(format, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
        check(again == pass, "render pass is cached, not rebuilt per call");

        VkRenderPass general = nova->getOffscreenRenderPass(format, VK_IMAGE_LAYOUT_GENERAL);
        check(general != VK_NULL_HANDLE, "second render pass created for BGRA/GENERAL");
        check(general != pass, "final layout is part of the render pass cache key");

        OwnedImage owned = {};
        check(createOwnedImage(*nova, config.extent, format, owned), "owned colour target allocated");
        check(owned.view != VK_NULL_HANDLE, "owned colour target has an image view");

        Nova::RenderTarget target = {};
        target.image = owned.image;
        target.view = owned.view;
        target.extent = config.extent;
        target.format = format;
        target.final_layout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
        target.clear = {{{ 0.0f, 0.5f, 1.0f, 1.0f }}};
        check(target.valid(), "render target passes its own validity check");

        uint32_t pixel = 0;
        check(renderAndReadBack(*nova, target, pixel), "renderToImage + readback completed");
        // BGRA8 UNORM: B=0xFF, G=0x80, R=0x00 for (0.0, 0.5, 1.0). Little-endian
        // load makes that 0xFF80xx00 with A=0xFF.
        check((pixel & 0x000000FFu) == 0x000000FFu, "blue channel cleared to 1.0");
        check((pixel & 0x00FF0000u) == 0x00000000u, "red channel cleared to 0.0");

        // Cached framebuffer reuse: a second frame through the same view must
        // not churn objects, which is the whole reason the cache exists.
        uint32_t pixel_again = 0;
        check(renderAndReadBack(*nova, target, pixel_again), "second frame through the same view");
        check(pixel_again == pixel, "second frame produced identical pixels");

        Nova::RenderTarget invalid = {};
        check(nova->renderToImage(invalid, [](VkCommandBuffer, uint32_t) {}) == VK_NULL_HANDLE,
              "renderToImage rejects an invalid target");

        // Imports are the other half of what the offscreen cleanup entry owns.
        // Reported either way: absence is not a failure, it is a capability.
        std::printf("[info] dmabuf import: %s\n", nova->supportsDmabufImport() ? "available" : "unavailable");
        check(true, "dmabuf import capability reported");

        check(nova->isOffscreen(), "instance reports offscreen mode");
        check(nova->getRenderPass() == VK_NULL_HANDLE, "swapchain render pass stays null offscreen");
        check(nova->getWindowExtent().width == config.extent.width,
              "window extent reports the configured offscreen extent");

        // Nothing may be in flight when the attachment goes: the cached
        // framebuffer built over this view outlives it, which is legal only
        // because no submission still references either.
        vkDeviceWaitIdle(nova->getDevice());
        destroyOwnedImage(*nova, owned);
        check(true, "owned colour target released after the device went idle");

        std::printf("[info] tearing down NovaGraphics (the P0 reproducer)\n");
        std::fflush(stdout);
        nova.reset();
        check(true, "NovaGraphics destroyed without aborting");
    }

    runTwoDeviceCase();   // B5: two live devices, after the teardown above

    std::printf("\nprobe_offscreen: %d/%d checks passed, %d failures\n",
                g_checks - g_failures, g_checks, g_failures);
    std::fflush(stdout);
    return g_failures == 0 ? 0 : 1;
}
