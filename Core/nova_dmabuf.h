// Written by Richard Christopher, Copyright 2026 NeoTec Digital
//
// Nova's DMA-BUF vocabulary. Deliberately free of wlroots and libdrm headers:
// Core is the drawing layer and must not depend on the client transport, so the
// Vazio layer translates wlr_dmabuf_attributes into DmabufAttributes.
// The fourcc codes below are ABI, not API - their values are fixed by the
// Linux DRM format definitions and cannot drift.
#pragma once

#include <vulkan/vulkan.h>
#include <cstdint>
namespace Nova {
// Little-endian packing of a DRM fourcc, identical to drm_fourcc.h's fourcc_code.
#define NOVA_FOURCC(a, b, c, d)                                      \
    (static_cast<uint32_t>(a) | (static_cast<uint32_t>(b) << 8) |    \
     (static_cast<uint32_t>(c) << 16) | (static_cast<uint32_t>(d) << 24))

// DRM_FORMAT_MOD_INVALID / _LINEAR, spelled here so <drm_fourcc.h> stays out.
static constexpr uint64_t NOVA_DRM_FORMAT_MOD_INVALID = 0x00FFFFFFFFFFFFFFULL;
static constexpr uint64_t NOVA_DRM_FORMAT_MOD_LINEAR = 0ULL;

// A DRM format names channels in the order of a little-endian machine WORD; a
// Vulkan non-packed format names them in increasing MEMORY address. That is why
// XRGB8888 (word x,R,G,B) maps to B8G8R8A8 (bytes B,G,R,X) and not the reverse.
enum DrmFormat : uint32_t {
    NOVA_DRM_FORMAT_XRGB8888     = NOVA_FOURCC('X', 'R', '2', '4'),
    NOVA_DRM_FORMAT_ARGB8888     = NOVA_FOURCC('A', 'R', '2', '4'),
    NOVA_DRM_FORMAT_XBGR8888     = NOVA_FOURCC('X', 'B', '2', '4'),
    NOVA_DRM_FORMAT_ABGR8888     = NOVA_FOURCC('A', 'B', '2', '4'),
    NOVA_DRM_FORMAT_XRGB2101010  = NOVA_FOURCC('X', 'R', '3', '0'),
    NOVA_DRM_FORMAT_ARGB2101010  = NOVA_FOURCC('A', 'R', '3', '0'),
    NOVA_DRM_FORMAT_XBGR2101010  = NOVA_FOURCC('X', 'B', '3', '0'),
    NOVA_DRM_FORMAT_ABGR2101010  = NOVA_FOURCC('A', 'B', '3', '0'),
    NOVA_DRM_FORMAT_RGB565       = NOVA_FOURCC('R', 'G', '1', '6'),
    NOVA_DRM_FORMAT_BGR565       = NOVA_FOURCC('B', 'G', '1', '6'),
    NOVA_DRM_FORMAT_XBGR16161616F = NOVA_FOURCC('X', 'B', '4', 'H'),
    NOVA_DRM_FORMAT_ABGR16161616F = NOVA_FOURCC('A', 'B', '4', 'H'),
    NOVA_DRM_FORMAT_R8           = NOVA_FOURCC('R', '8', ' ', ' '),
    NOVA_DRM_FORMAT_GR88         = NOVA_FOURCC('G', 'R', '8', '8')
};

static constexpr int NOVA_DMABUF_MAX_PLANES = 4;

struct DmabufPlane {
    // Borrowed for the duration of the import call only: Nova dups every fd it
    // keeps, so the caller still owns and must close the fds it passed in.
    int fd = -1;
    uint32_t offset = 0;
    uint32_t stride = 0;
};

/**
 * Nova-side mirror of wlr_dmabuf_attributes (wlr/render/dmabuf.h:34-43).
 *
 * `modifier` follows the same convention: INVALID means an implicit
 * vendor-defined layout, LINEAR means linear, and any other value must be
 * honoured exactly.
 */
struct DmabufAttributes {
    uint32_t width = 0;
    uint32_t height = 0;
    uint32_t drm_format = 0;
    uint64_t modifier = NOVA_DRM_FORMAT_MOD_INVALID;
    int plane_count = 0;
    DmabufPlane planes[NOVA_DMABUF_MAX_PLANES] = {};
};

/**
 * One row of the DRM-fourcc <-> VkFormat table.
 *
 * `has_alpha` is what disambiguates the reverse direction: XRGB8888 and
 * ARGB8888 share VK_FORMAT_B8G8R8A8_UNORM, so the reverse lookup has to pick,
 * and it picks the alpha-bearing code.
 */
struct FormatMapping {
    uint32_t drm_format;
    VkFormat vk_format;
    bool has_alpha;
    const char* name;
};

// The table itself, so callers can advertise exactly what Nova can import.
const FormatMapping* formatTable(uint32_t& count_out);

// DRM fourcc -> VkFormat. VK_FORMAT_UNDEFINED when the code is not in the table.
VkFormat vulkanFormatFromDrm(uint32_t drm_format);

// VkFormat -> DRM fourcc, preferring the alpha-bearing code. 0 when unmapped.
uint32_t drmFormatFromVulkan(VkFormat format);

// True when the fourcc carries a defined alpha channel (ARGB yes, XRGB no).
bool drmFormatHasAlpha(uint32_t drm_format);

// "XR24"-style spelling of any fourcc, table member or not. Unknown codes are
// rendered into a thread_local scratch buffer valid until this thread's next
// call, so a log line must be formatted before the next lookup.
const char* drmFormatName(uint32_t drm_format);

} // namespace Nova
