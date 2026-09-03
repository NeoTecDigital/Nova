// Written by Richard Christopher, Copyright 2026 NeoTec Digital
//
// DMA-BUF import (plan section D.2), interrogation half. See nova_dmabuf_query.h.

#include "./nova_dmabuf_query.h"

#include <sys/stat.h>
namespace Nova {
VkImageAspectFlagBits memoryPlaneAspect(int plane)
{
    static constexpr VkImageAspectFlagBits ASPECTS[NOVA_DMABUF_MAX_PLANES] = {
        VK_IMAGE_ASPECT_MEMORY_PLANE_0_BIT_EXT,
        VK_IMAGE_ASPECT_MEMORY_PLANE_1_BIT_EXT,
        VK_IMAGE_ASPECT_MEMORY_PLANE_2_BIT_EXT,
        VK_IMAGE_ASPECT_MEMORY_PLANE_3_BIT_EXT
    };

    return ASPECTS[plane];
}

// vkGetMemoryFdPropertiesKHR ships with VK_KHR_external_memory_fd and is not in
// the loader's exported set, so it is resolved per device on first use.
PFN_vkGetMemoryFdPropertiesKHR loadGetMemoryFdProperties(VkDevice device)
{
    static PFN_vkGetMemoryFdPropertiesKHR cached = nullptr;
    static VkDevice cached_for = VK_NULL_HANDLE;

    if (cached_for != device) {
        cached = reinterpret_cast<PFN_vkGetMemoryFdPropertiesKHR>(
            vkGetDeviceProcAddr(device, "vkGetMemoryFdPropertiesKHR"));
        cached_for = device;
    }

    return cached;
}

// Every modifier the device reports for `format`. Two-call idiom: count, then fill.
std::vector<VkDrmFormatModifierPropertiesEXT> queryModifiers(VkPhysicalDevice device, VkFormat format)
{
    VkDrmFormatModifierPropertiesListEXT list = {
        .sType = VK_STRUCTURE_TYPE_DRM_FORMAT_MODIFIER_PROPERTIES_LIST_EXT
    };
    VkFormatProperties2 props = { .sType = VK_STRUCTURE_TYPE_FORMAT_PROPERTIES_2, .pNext = &list };

    vkGetPhysicalDeviceFormatProperties2(device, format, &props);
    if (list.drmFormatModifierCount == 0) {
        return {};
    }

    std::vector<VkDrmFormatModifierPropertiesEXT> entries(list.drmFormatModifierCount);
    list.pDrmFormatModifierProperties = entries.data();
    vkGetPhysicalDeviceFormatProperties2(device, format, &props);

    return entries;
}

// The modifier's own plane count, and whether the device knows the modifier at
// all. `format` is the Vulkan format the fourcc mapped to.
bool modifierPlaneCount(VkPhysicalDevice device, VkFormat format, uint64_t modifier, uint32_t& planes_out)
{
    for (const auto& entry : queryModifiers(device, format)) {
        if (entry.drmFormatModifier != modifier) {
            continue;
        }

        if ((entry.drmFormatModifierTilingFeatures & VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BIT) == 0) {
            report(LOGGER::ERROR, "dmabuf import: modifier 0x%llx cannot be a colour attachment",
                   static_cast<unsigned long long>(modifier));
            return false;
        }

        planes_out = entry.drmFormatModifierPlaneCount;
        return true;
    }

    return false;
}

// The device has to say it can import this exact (format, modifier, usage) as a
// dmabuf before an image is created, or vkCreateImage failure is the first news.
bool importableAsDmabuf(VkPhysicalDevice device, VkFormat format, uint64_t modifier)
{
    VkPhysicalDeviceImageDrmFormatModifierInfoEXT modifier_info = {
        .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_IMAGE_DRM_FORMAT_MODIFIER_INFO_EXT,
        .drmFormatModifier = modifier,
        .sharingMode = VK_SHARING_MODE_EXCLUSIVE
    };

    VkPhysicalDeviceExternalImageFormatInfo external_info = {
        .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_EXTERNAL_IMAGE_FORMAT_INFO,
        .pNext = &modifier_info,
        .handleType = VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT
    };

    VkPhysicalDeviceImageFormatInfo2 info = {
        .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_IMAGE_FORMAT_INFO_2,
        .pNext = &external_info,
        .format = format,
        .type = VK_IMAGE_TYPE_2D,
        .tiling = VK_IMAGE_TILING_DRM_FORMAT_MODIFIER_EXT,
        .usage = NOVA_IMPORT_USAGE,
        .flags = 0
    };

    VkExternalImageFormatProperties external_props = {
        .sType = VK_STRUCTURE_TYPE_EXTERNAL_IMAGE_FORMAT_PROPERTIES
    };
    VkImageFormatProperties2 props = {
        .sType = VK_STRUCTURE_TYPE_IMAGE_FORMAT_PROPERTIES_2,
        .pNext = &external_props
    };

    if (vkGetPhysicalDeviceImageFormatProperties2(device, &info, &props) != VK_SUCCESS) {
        return false;
    }

    return (external_props.externalMemoryProperties.externalMemoryFeatures &
            VK_EXTERNAL_MEMORY_FEATURE_IMPORTABLE_BIT) != 0;
}

/**
 * Do every plane fd name the same underlying DMA-BUF?
 *
 * A modifier with several MEMORY planes (AMD's DCC codes, for instance) is
 * normally one allocation carrying every plane at its own offset - the
 * exporter hands out one fd per plane, but they are all exports of the same
 * buffer, which dmabuf gives a single inode. That case takes ONE
 * VkDeviceMemory and describes the planes through pPlaneLayouts; only planes
 * living in genuinely separate buffers need VK_IMAGE_CREATE_DISJOINT_BIT.
 *
 * Verified on radv: for XR24 + 0x020000002096bb03 the driver reports
 * memory-plane 1 as requiring zero bytes, because plane 0's allocation already
 * contains it. Treating that as disjoint asks for a zero-sized allocation and
 * fails. When identity cannot be established the shared case is assumed - it
 * is what every allocator on this path produces.
 */
bool planesShareOneBuffer(const DmabufAttributes& attrs)
{
    struct stat first = {};
    if (fstat(attrs.planes[0].fd, &first) != 0) {
        report(LOGGER::WARN, "dmabuf import: cannot stat plane 0 fd - assuming one shared buffer");
        return true;
    }

    for (int plane = 1; plane < attrs.plane_count; plane++) {
        struct stat other = {};
        if (fstat(attrs.planes[plane].fd, &other) != 0) {
            report(LOGGER::WARN, "dmabuf import: cannot stat plane %d fd - assuming one shared buffer", plane);
            return true;
        }

        if (other.st_dev != first.st_dev || other.st_ino != first.st_ino) {
            return false;
        }
    }

    return true;
}

// Plane layouts, verbatim from the exporter. size/arrayPitch/depthPitch must be
// zero for a modifier import: the driver derives them from the modifier.
void fillPlaneLayouts(const DmabufAttributes& attrs, std::vector<VkSubresourceLayout>& layouts)
{
    layouts.resize(static_cast<size_t>(attrs.plane_count));

    for (int plane = 0; plane < attrs.plane_count; plane++) {
        layouts[plane] = VkSubresourceLayout{
            .offset = attrs.planes[plane].offset,
            .size = 0,
            .rowPitch = attrs.planes[plane].stride,
            .arrayPitch = 0,
            .depthPitch = 0
        };
    }
}


std::vector<uint64_t> Graphics::supportedDmabufModifiers(uint32_t drm_format) const
{
    std::vector<uint64_t> modifiers;

    const VkFormat format = vulkanFormatFromDrm(drm_format);
    if (!supportsDmabufImport() || format == VK_FORMAT_UNDEFINED) {
        return modifiers;
    }

    for (const auto& entry : queryModifiers(physical_device, format)) {
        const bool renderable =
            (entry.drmFormatModifierTilingFeatures & VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BIT) != 0;
        if (renderable && importableAsDmabuf(physical_device, format, entry.drmFormatModifier)) {
            modifiers.push_back(entry.drmFormatModifier);
        }
    }

    return modifiers;
}

bool Graphics::supportsDmabufImport() const
{
    return hasDeviceExtension(VK_KHR_EXTERNAL_MEMORY_FD_EXTENSION_NAME) &&
           hasDeviceExtension(VK_EXT_EXTERNAL_MEMORY_DMA_BUF_EXTENSION_NAME) &&
           hasDeviceExtension(VK_EXT_IMAGE_DRM_FORMAT_MODIFIER_EXTENSION_NAME);
}

// Gates that do not need the device: shape of the request, then the table.
bool validateDmabufRequest(const DmabufAttributes& attrs, VkFormat& format_out)
{
    if (attrs.width == 0 || attrs.height == 0) {
        report(LOGGER::ERROR, "dmabuf import: zero extent");
        return false;
    }

    if (attrs.plane_count <= 0 || attrs.plane_count > NOVA_DMABUF_MAX_PLANES) {
        report(LOGGER::ERROR, "dmabuf import: plane count %d out of range", attrs.plane_count);
        return false;
    }

    for (int plane = 0; plane < attrs.plane_count; plane++) {
        if (attrs.planes[plane].fd < 0) {
            report(LOGGER::ERROR, "dmabuf import: plane %d has no fd", plane);
            return false;
        }
    }

    if (attrs.modifier == NOVA_DRM_FORMAT_MOD_INVALID) {
        report(LOGGER::ERROR,
               "dmabuf import: implicit modifier (INVALID) cannot be imported - "
               "VK_EXT_image_drm_format_modifier needs an explicit one; "
               "allocate from supportedDmabufModifiers()");
        return false;
    }

    format_out = vulkanFormatFromDrm(attrs.drm_format);
    if (format_out == VK_FORMAT_UNDEFINED) {
        report(LOGGER::ERROR, "dmabuf import: DRM format %s is not in Nova's format table",
               drmFormatName(attrs.drm_format));
        return false;
    }

    return true;
}

uint32_t firstAllowedMemoryType(VkPhysicalDevice physical, uint32_t allowed_bits)
{
    VkPhysicalDeviceMemoryProperties memory = {};
    vkGetPhysicalDeviceMemoryProperties(physical, &memory);

    for (uint32_t index = 0; index < memory.memoryTypeCount; index++) {
        if ((allowed_bits & (1u << index)) != 0) {
            return index;
        }
    }

    return UINT32_MAX;
}


} // namespace Nova
