// Written by Richard Christopher, Copyright 2026 NeoTec Digital
//
// The DRM-fourcc <-> VkFormat table. Pure data plus two lookups: no Vulkan
// device is touched here, so the table can be consulted before a device exists
// (format advertisement) and unit-tested without a GPU.

#include "./nova_dmabuf.h"
namespace Nova {
namespace {

/**
 * DRM word order vs Vulkan memory order, spelled out once.
 *
 * ARGB8888 is the 32-bit little-endian word (A<<24)|(R<<16)|(G<<8)|B, so byte 0
 * of memory holds B and byte 3 holds A: that is exactly VK_FORMAT_B8G8R8A8,
 * whose component names are written in ascending address order. The X variants
 * share the Vulkan format and differ only in that their fourth byte carries no
 * defined alpha - a fact the consumer handles with a component swizzle, not a
 * different format.
 *
 * The PACK32/PACK16 Vulkan formats are the opposite convention - they name the
 * word from most to least significant bit - which is why the 2101010 and 565
 * rows read as direct transliterations of their DRM names.
 *
 * Deliberately absent: RGBA8888/BGRA8888/RGBX8888/BGRX8888 (byte orders
 * A,B,G,R and A,R,G,B, which Vulkan has no format for) and the planar YUV
 * codes (multi-plane FORMATS, distinct from the multi-plane MODIFIERS handled
 * in nova_dmabuf_import.cpp). Adding a row is the whole cost of extending this.
 */
constexpr FormatMapping FORMAT_TABLE[] = {
    { NOVA_DRM_FORMAT_ARGB8888,      VK_FORMAT_B8G8R8A8_UNORM,             true,  "AR24" },
    { NOVA_DRM_FORMAT_XRGB8888,      VK_FORMAT_B8G8R8A8_UNORM,             false, "XR24" },
    { NOVA_DRM_FORMAT_ABGR8888,      VK_FORMAT_R8G8B8A8_UNORM,             true,  "AB24" },
    { NOVA_DRM_FORMAT_XBGR8888,      VK_FORMAT_R8G8B8A8_UNORM,             false, "XB24" },
    { NOVA_DRM_FORMAT_ARGB2101010,   VK_FORMAT_A2R10G10B10_UNORM_PACK32,   true,  "AR30" },
    { NOVA_DRM_FORMAT_XRGB2101010,   VK_FORMAT_A2R10G10B10_UNORM_PACK32,   false, "XR30" },
    { NOVA_DRM_FORMAT_ABGR2101010,   VK_FORMAT_A2B10G10R10_UNORM_PACK32,   true,  "AB30" },
    { NOVA_DRM_FORMAT_XBGR2101010,   VK_FORMAT_A2B10G10R10_UNORM_PACK32,   false, "XB30" },
    { NOVA_DRM_FORMAT_ABGR16161616F, VK_FORMAT_R16G16B16A16_SFLOAT,        true,  "AB4H" },
    { NOVA_DRM_FORMAT_XBGR16161616F, VK_FORMAT_R16G16B16A16_SFLOAT,        false, "XB4H" },
    { NOVA_DRM_FORMAT_RGB565,        VK_FORMAT_R5G6B5_UNORM_PACK16,        false, "RG16" },
    { NOVA_DRM_FORMAT_BGR565,        VK_FORMAT_B5G6R5_UNORM_PACK16,        false, "BG16" },
    { NOVA_DRM_FORMAT_GR88,          VK_FORMAT_R8G8_UNORM,                 false, "GR88" },
    { NOVA_DRM_FORMAT_R8,            VK_FORMAT_R8_UNORM,                   false, "R8  " }
};

constexpr uint32_t FORMAT_TABLE_SIZE = sizeof(FORMAT_TABLE) / sizeof(FORMAT_TABLE[0]);

const FormatMapping* findByDrm(uint32_t drm_format)
{
    for (uint32_t i = 0; i < FORMAT_TABLE_SIZE; i++) {
        if (FORMAT_TABLE[i].drm_format == drm_format) {
            return &FORMAT_TABLE[i];
        }
    }

    return nullptr;
}

} // namespace

const FormatMapping* formatTable(uint32_t& count_out)
{
    count_out = FORMAT_TABLE_SIZE;
    return FORMAT_TABLE;
}

VkFormat vulkanFormatFromDrm(uint32_t drm_format)
{
    const FormatMapping* mapping = findByDrm(drm_format);
    return mapping != nullptr ? mapping->vk_format : VK_FORMAT_UNDEFINED;
}

uint32_t drmFormatFromVulkan(VkFormat format)
{
    // The table is ordered alpha-variant-first per Vulkan format, so the first
    // hit is already the preferred answer.
    for (uint32_t i = 0; i < FORMAT_TABLE_SIZE; i++) {
        if (FORMAT_TABLE[i].vk_format == format) {
            return FORMAT_TABLE[i].drm_format;
        }
    }

    return 0;
}

bool drmFormatHasAlpha(uint32_t drm_format)
{
    const FormatMapping* mapping = findByDrm(drm_format);
    return mapping != nullptr && mapping->has_alpha;
}

const char* drmFormatName(uint32_t drm_format)
{
    const FormatMapping* mapping = findByDrm(drm_format);
    if (mapping != nullptr) {
        return mapping->name;
    }

    // Unknown codes still have to print as something a bug report can carry.
    // thread_local so two threads logging at once cannot interleave into one
    // buffer; the returned pointer stays valid until this thread's next call.
    static thread_local char scratch[5] = {};
    for (int byte = 0; byte < 4; byte++) {
        const char ch = static_cast<char>((drm_format >> (byte * 8)) & 0xFF);
        scratch[byte] = (ch >= 0x20 && ch < 0x7F) ? ch : '?';
    }
    scratch[4] = '\0';

    return scratch;
}

} // namespace Nova
