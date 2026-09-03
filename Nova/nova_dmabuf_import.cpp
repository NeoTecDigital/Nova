// Written by Richard Christopher, Copyright 2026 NeoTec Digital
//
// DMA-BUF import (plan section D.2), creation half: image, memory, binding,
// view, teardown. The queries that decide WHETHER a buffer can be imported
// live in nova_dmabuf_query.cpp.

#include "./nova_dmabuf_query.h"

#include <unistd.h>
#include <vector>
namespace Nova {

/**
 * The image itself.
 *
 * initialLayout UNDEFINED is mandatory, not a choice: VUID-VkImageCreateInfo-
 * pNext-01443 requires it for any image whose pNext chain carries
 * VkExternalMemoryImageCreateInfo with a non-zero handleTypes. (PREINITIALIZED
 * was tried first and the validation layer rejected it.)
 *
 * It costs nothing here. With VK_IMAGE_TILING_DRM_FORMAT_MODIFIER_EXT the
 * modifier fully determines the memory layout, so an UNDEFINED -> X transition
 * has no alternative tiling to move from and cannot discard the exporter's
 * bytes. A consumer reading an imported buffer therefore barriers FROM
 * UNDEFINED - the same idiom wlroots' Vulkan renderer uses for dmabuf textures.
 *
 * `disjoint` is about how many BUFFERS back the image, never how many planes
 * the modifier declares. See planesShareOneBuffer().
 */
static VkImage createImportImage(VkDevice device,
                                 const DmabufAttributes& attrs,
                                 VkFormat format,
                                 bool disjoint)
{
    std::vector<VkSubresourceLayout> layouts;
    fillPlaneLayouts(attrs, layouts);

    VkImageDrmFormatModifierExplicitCreateInfoEXT modifier_info = {
        .sType = VK_STRUCTURE_TYPE_IMAGE_DRM_FORMAT_MODIFIER_EXPLICIT_CREATE_INFO_EXT,
        .drmFormatModifier = attrs.modifier,
        .drmFormatModifierPlaneCount = static_cast<uint32_t>(attrs.plane_count),
        .pPlaneLayouts = layouts.data()
    };

    VkExternalMemoryImageCreateInfo external_info = {
        .sType = VK_STRUCTURE_TYPE_EXTERNAL_MEMORY_IMAGE_CREATE_INFO,
        .pNext = &modifier_info,
        .handleTypes = VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT
    };

    VkImageCreateInfo info = {
        .sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
        .pNext = &external_info,
        .flags = static_cast<VkImageCreateFlags>(disjoint ? VK_IMAGE_CREATE_DISJOINT_BIT : 0),
        .imageType = VK_IMAGE_TYPE_2D,
        .format = format,
        .extent = { attrs.width, attrs.height, 1 },
        .mipLevels = 1,
        .arrayLayers = 1,
        .samples = VK_SAMPLE_COUNT_1_BIT,
        .tiling = VK_IMAGE_TILING_DRM_FORMAT_MODIFIER_EXT,
        .usage = NOVA_IMPORT_USAGE,
        .sharingMode = VK_SHARING_MODE_EXCLUSIVE,
        .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED
    };

    VkImage image = VK_NULL_HANDLE;
    const VkResult result = vkCreateImage(device, &info, nullptr, &image);
    if (result != VK_SUCCESS) {
        report(LOGGER::ERROR, "dmabuf import: vkCreateImage failed (%d)", static_cast<int>(result));
        return VK_NULL_HANDLE;
    }

    return image;
}

// Requirements for the whole image, or for one memory plane when disjoint.
static VkMemoryRequirements planeRequirements(VkDevice device, VkImage image, int plane, bool disjoint)
{
    VkImagePlaneMemoryRequirementsInfo plane_info = {
        .sType = VK_STRUCTURE_TYPE_IMAGE_PLANE_MEMORY_REQUIREMENTS_INFO,
        .planeAspect = memoryPlaneAspect(plane)
    };

    VkImageMemoryRequirementsInfo2 info = {
        .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_REQUIREMENTS_INFO_2,
        .pNext = disjoint ? &plane_info : nullptr,
        .image = image
    };

    VkMemoryRequirements2 requirements = { .sType = VK_STRUCTURE_TYPE_MEMORY_REQUIREMENTS_2 };
    vkGetImageMemoryRequirements2(device, &info, &requirements);

    return requirements.memoryRequirements;
}

// One VkDeviceMemory per fd. The fd is dup'ed because a successful
// vkAllocateMemory takes ownership of the descriptor it was handed.
// A memory type the image and the fd both accept, or UINT32_MAX.
uint32_t Graphics::importMemoryTypeFor(int fd, const VkMemoryRequirements& requirements)
{
    PFN_vkGetMemoryFdPropertiesKHR get_fd_properties = loadGetMemoryFdProperties(logical_device);
    if (get_fd_properties == nullptr) {
        report(LOGGER::ERROR, "dmabuf import: vkGetMemoryFdPropertiesKHR unavailable");
        return UINT32_MAX;
    }

    VkMemoryFdPropertiesKHR fd_properties = { .sType = VK_STRUCTURE_TYPE_MEMORY_FD_PROPERTIES_KHR };
    if (get_fd_properties(logical_device, VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT,
                          fd, &fd_properties) != VK_SUCCESS) {
        report(LOGGER::ERROR, "dmabuf import: fd is not importable as a dmabuf");
        return UINT32_MAX;
    }

    const uint32_t type_index = firstAllowedMemoryType(
        physical_device, requirements.memoryTypeBits & fd_properties.memoryTypeBits);
    if (type_index == UINT32_MAX) {
        report(LOGGER::ERROR, "dmabuf import: no memory type satisfies both image and fd");
    }

    return type_index;
}

VkDeviceMemory Graphics::importPlaneMemory(VkImage image, int fd, const VkMemoryRequirements& requirements)
{
    const uint32_t type_index = importMemoryTypeFor(fd, requirements);
    if (type_index == UINT32_MAX) {
        return VK_NULL_HANDLE;
    }

    const int owned_fd = dup(fd);
    if (owned_fd < 0) {
        report(LOGGER::ERROR, "dmabuf import: dup() of the plane fd failed");
        return VK_NULL_HANDLE;
    }

    VkImportMemoryFdInfoKHR import_info = {
        .sType = VK_STRUCTURE_TYPE_IMPORT_MEMORY_FD_INFO_KHR,
        .handleType = VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT,
        .fd = owned_fd
    };

    // Dedicated allocation: the fd IS this image's storage, not a suballocation.
    VkMemoryDedicatedAllocateInfo dedicated = {
        .sType = VK_STRUCTURE_TYPE_MEMORY_DEDICATED_ALLOCATE_INFO,
        .pNext = &import_info,
        .image = image
    };

    VkMemoryAllocateInfo alloc_info = {
        .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
        .pNext = &dedicated,
        .allocationSize = requirements.size,
        .memoryTypeIndex = type_index
    };

    VkDeviceMemory memory = VK_NULL_HANDLE;
    const VkResult result = vkAllocateMemory(logical_device, &alloc_info, nullptr, &memory);
    if (result != VK_SUCCESS) {
        close(owned_fd);   // ownership only transfers on success
        report(LOGGER::ERROR, "dmabuf import: vkAllocateMemory failed (%d)", static_cast<int>(result));
        return VK_NULL_HANDLE;
    }

    return memory;
}

// One plane's VkDeviceMemory, recorded on the image so teardown frees it even
// if a later plane fails.
VkDeviceMemory Graphics::importOnePlane(ImportedImage& imported,
                                            const DmabufAttributes& attrs,
                                            int plane,
                                            bool disjoint)
{
    const VkMemoryRequirements requirements =
        planeRequirements(logical_device, imported.image, plane, disjoint);

    if (requirements.size == 0) {
        report(LOGGER::ERROR,
               "dmabuf import: driver wants no separate allocation for memory plane %d - "
               "this modifier is not importable disjoint on this device", plane);
        return VK_NULL_HANDLE;
    }

    VkDeviceMemory memory = importPlaneMemory(imported.image, attrs.planes[plane].fd, requirements);
    if (memory != VK_NULL_HANDLE) {
        imported.memory[imported.memory_count++] = memory;
    }

    return memory;
}

bool Graphics::bindImportedMemory(ImportedImage& imported,
                                      const DmabufAttributes& attrs,
                                      bool disjoint)
{
    std::vector<VkBindImageMemoryInfo> binds;
    std::vector<VkBindImagePlaneMemoryInfo> plane_binds;
    binds.reserve(static_cast<size_t>(attrs.plane_count));
    plane_binds.reserve(static_cast<size_t>(attrs.plane_count));

    for (int plane = 0; plane < attrs.plane_count; plane++) {
        VkDeviceMemory memory = importOnePlane(imported, attrs, plane, disjoint);
        if (memory == VK_NULL_HANDLE) {
            return false;
        }

        plane_binds.push_back(VkBindImagePlaneMemoryInfo{
            .sType = VK_STRUCTURE_TYPE_BIND_IMAGE_PLANE_MEMORY_INFO,
            .planeAspect = memoryPlaneAspect(plane)
        });

        binds.push_back(VkBindImageMemoryInfo{
            .sType = VK_STRUCTURE_TYPE_BIND_IMAGE_MEMORY_INFO,
            .pNext = disjoint ? &plane_binds.back() : nullptr,
            .image = imported.image,
            .memory = memory,
            .memoryOffset = 0
        });

        if (!disjoint) {
            break;   // one memory object backs every plane of a joint image
        }
    }

    const VkResult result = vkBindImageMemory2(logical_device,
                                              static_cast<uint32_t>(binds.size()), binds.data());
    if (result != VK_SUCCESS) {
        report(LOGGER::ERROR, "dmabuf import: vkBindImageMemory2 failed (%d)", static_cast<int>(result));
        return false;
    }

    return true;
}

// Every gate an import must clear before an object is created.
bool Graphics::dmabufImportAccepted(const DmabufAttributes& attrs, VkFormat& format_out)
{
    if (!supportsDmabufImport()) {
        report(LOGGER::ERROR, "dmabuf import: external-memory extensions are not enabled on this device");
        return false;
    }

    if (!validateDmabufRequest(attrs, format_out)) {
        return false;
    }

    uint32_t modifier_planes = 0;
    if (!modifierPlaneCount(physical_device, format_out, attrs.modifier, modifier_planes)) {
        report(LOGGER::ERROR, "dmabuf import: %s + modifier 0x%llx unsupported on this device",
               drmFormatName(attrs.drm_format),
               static_cast<unsigned long long>(attrs.modifier));
        return false;
    }

    if (modifier_planes != static_cast<uint32_t>(attrs.plane_count)) {
        report(LOGGER::ERROR, "dmabuf import: modifier wants %u planes, attributes carry %d",
               modifier_planes, attrs.plane_count);
        return false;
    }

    if (!importableAsDmabuf(physical_device, format_out, attrs.modifier)) {
        report(LOGGER::ERROR, "dmabuf import: %s is not importable as a colour attachment here",
               drmFormatName(attrs.drm_format));
        return false;
    }

    return true;
}

bool Graphics::importDmabufAsImage(const DmabufAttributes& attrs, ImportedImage& out)
{
    VkFormat format = VK_FORMAT_UNDEFINED;
    if (!dmabufImportAccepted(attrs, format)) {
        return false;
    }

    ImportedImage imported = {};
    imported.format = format;
    imported.extent = { attrs.width, attrs.height };
    imported.modifier = attrs.modifier;
    imported.drm_format = attrs.drm_format;

    // DISJOINT is about how many BUFFERS back the image, not how many planes
    // the modifier has. Several planes inside one dmabuf are one allocation.
    const bool disjoint = attrs.plane_count > 1 && !planesShareOneBuffer(attrs);

    imported.image = createImportImage(logical_device, attrs, format, disjoint);
    if (imported.image == VK_NULL_HANDLE) {
        return false;
    }

    if (!bindImportedMemory(imported, attrs, disjoint) || !createImportedView(imported)) {
        vkDeviceWaitIdle(logical_device);
        destroyImportedResources(imported);   // never tracked, so nothing to untrack
        return false;
    }

    out = imported;
    trackImportedImage(out);

    report(LOGGER::INFO, "dmabuf imported: %ux%u %s modifier 0x%llx, %d plane(s)%s",
           attrs.width, attrs.height, drmFormatName(attrs.drm_format),
           static_cast<unsigned long long>(attrs.modifier), attrs.plane_count,
           disjoint ? ", disjoint" : "");

    return true;
}

bool Graphics::createImportedView(ImportedImage& imported)
{
    VkImageViewCreateInfo info = {
        .sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,
        .image = imported.image,
        .viewType = VK_IMAGE_VIEW_TYPE_2D,
        .format = imported.format,
        .components = {
            .r = VK_COMPONENT_SWIZZLE_IDENTITY,
            .g = VK_COMPONENT_SWIZZLE_IDENTITY,
            .b = VK_COMPONENT_SWIZZLE_IDENTITY,
            .a = VK_COMPONENT_SWIZZLE_IDENTITY
        },
        .subresourceRange = {
            .aspectMask = VK_IMAGE_ASPECT_COLOR_BIT,
            .baseMipLevel = 0,
            .levelCount = 1,
            .baseArrayLayer = 0,
            .layerCount = 1
        }
    };

    const VkResult result = vkCreateImageView(logical_device, &info, nullptr, &imported.view);
    if (result != VK_SUCCESS) {
        report(LOGGER::ERROR, "dmabuf import: image view creation failed (%d)", static_cast<int>(result));
        imported.view = VK_NULL_HANDLE;
        return false;
    }

    return true;
}

void Graphics::trackImportedImage(const ImportedImage& imported)
{
    imported_images.push_back(imported);
}

void Graphics::destroyImportedResources(ImportedImage& imported)
{
    if (imported.view != VK_NULL_HANDLE) {
        if (offscreen_targets) {
            offscreen_targets->invalidate(imported.view);
        }
        vkDestroyImageView(logical_device, imported.view, nullptr);
        imported.view = VK_NULL_HANDLE;
    }

    if (imported.image != VK_NULL_HANDLE) {
        vkDestroyImage(logical_device, imported.image, nullptr);
        imported.image = VK_NULL_HANDLE;
    }

    for (uint32_t plane = 0; plane < imported.memory_count; plane++) {
        if (imported.memory[plane] != VK_NULL_HANDLE) {
            vkFreeMemory(logical_device, imported.memory[plane], nullptr);
            imported.memory[plane] = VK_NULL_HANDLE;
        }
    }
    imported.memory_count = 0;
}

void Graphics::releaseImportedImage(ImportedImage& imported)
{
    // Same rule as TextureBridge::releaseTexture: a destroy cannot be ordered
    // by a barrier, so everything referencing it must already have retired.
    if (logical_device == VK_NULL_HANDLE || imported.image == VK_NULL_HANDLE) {
        return;
    }

    vkDeviceWaitIdle(logical_device);

    const VkImage released = imported.image;
    destroyImportedResources(imported);

    for (auto it = imported_images.begin(); it != imported_images.end(); ++it) {
        if (it->image == released) {
            imported_images.erase(it);
            break;
        }
    }
}

} // namespace Nova
