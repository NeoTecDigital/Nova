// Written by Richard Christopher, Copyright 2026 NeoTec Digital
//
// The VMA_IMPLEMENTATION carrier, and nothing else.
//
// AMD's VulkanMemoryAllocator is a single-header library: every consumer gets
// the declarations from Nova/components/vk_memory.h, and exactly ONE
// translation unit must define VMA_IMPLEMENTATION to emit the ~19,800 lines of
// definitions. Which TU that is, is a link-graph decision, not a stylistic one.
//
// It used to be Nova/modules/management.cpp - a NovaCoreLegacy member file.
// Because that object defined vmaAllocateMemory and friends, every target that
// allocated GPU memory forced the linker to extract it from libNova.a, which
// dragged in the 172 NovaCoreLegacy symbols sharing the TU, whose two
// unresolved members (CoreLegacy::getBufferInfo, CoreLegacy::createBeginInfo)
// then dragged modules/presentation.cpp and its 16 more. 188 symbols of a
// deprecated class landed in the boot binary as collateral of a #define, none
// of them ever referenced by a live call.
//
// Isolating the definition here makes the extraction cost exactly what it says
// it is: a target that allocates memory gets VMA, and gets nothing else. Keep
// this file's contents to the two lines below - anything else added here
// acquires the same unconditional reach into every consumer.
#define VMA_IMPLEMENTATION
#include "Nova/components/vk_memory.h"
