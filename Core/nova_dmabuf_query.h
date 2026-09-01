// Written by Richard Christopher, Copyright 2026 NeoTec Digital
//
// DMA-BUF import (plan section D.2), interrogation half: what this device can
// import, and whether one specific request is importable. Nothing here creates
// an object - every function answers a question the creation half asks first.
#pragma once

#include "./nova_graphics.h"

#include <vector>

// Everything a wlroots output buffer or a client surface can be asked to do.
constexpr VkImageUsageFlags NOVA_IMPORT_USAGE =
    VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT |
    VK_IMAGE_USAGE_TRANSFER_SRC_BIT |
    VK_IMAGE_USAGE_TRANSFER_DST_BIT |
    VK_IMAGE_USAGE_SAMPLED_BIT;

// Aspect naming memory plane i of a DRM-format-modifier image.
VkImageAspectFlagBits novaMemoryPlaneAspect(int plane);

// Every modifier the device reports for `format`.
std::vector<VkDrmFormatModifierPropertiesEXT> novaQueryModifiers(VkPhysicalDevice device, VkFormat format);

// The modifier's own plane count; false when the device does not know it or it
// cannot be a colour attachment.
bool novaModifierPlaneCount(VkPhysicalDevice device, VkFormat format, uint64_t modifier, uint32_t& planes_out);

// Can this exact (format, modifier, usage) be imported as a dmabuf here?
bool novaImportableAsDmabuf(VkPhysicalDevice device, VkFormat format, uint64_t modifier);

// Do every plane fd name the same underlying DMA-BUF? See the definition.
bool novaPlanesShareOneBuffer(const NovaDmabufAttributes& attrs);

// Plane layouts, verbatim from the exporter.
void novaFillPlaneLayouts(const NovaDmabufAttributes& attrs, std::vector<VkSubresourceLayout>& layouts);

// Device-independent gates: shape of the request, then the format table.
bool novaValidateDmabufRequest(const NovaDmabufAttributes& attrs, VkFormat& format_out);

// First memory type index allowed by `allowed_bits`, or UINT32_MAX.
uint32_t novaFirstAllowedMemoryType(VkPhysicalDevice physical, uint32_t allowed_bits);

// vkGetMemoryFdPropertiesKHR for `device`, resolved once per device.
PFN_vkGetMemoryFdPropertiesKHR novaLoadGetMemoryFdProperties(VkDevice device);
