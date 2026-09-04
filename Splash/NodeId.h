// Written by Richard Christopher, Copyright 2026 NeoTec Digital
#pragma once

#include <cstdint>

namespace Splash {

/**
 * NodeId - a generational handle into Splash::Registry.
 *
 * One 32-bit word: 20 bits of slot index, 12 bits of generation. Nothing about
 * a node's address is in it, so it is copyable, comparable, storable in a
 * protocol message and meaningless to dereference by hand - which is the whole
 * point. A handle that outlives the node it named resolves to nothing rather
 * than to whatever moved into the slot.
 *
 * Generations start at 1, so a zeroed NodeId is dead in BOTH halves: index 0
 * with generation 0 can never be issued, and a default-constructed handle
 * therefore never names a live node by accident.
 */
struct NodeId {
    static constexpr uint32_t INDEX_BITS = 20;
    static constexpr uint32_t INDEX_MASK = (1u << INDEX_BITS) - 1u;
    static constexpr uint32_t MAX_INDEX = INDEX_MASK;
    static constexpr uint32_t MAX_GENERATION = (1u << 12) - 1u;   // 4095
    static constexpr uint32_t FIRST_GENERATION = 1u;

    uint32_t bits = 0;

    constexpr uint32_t index() const { return bits & INDEX_MASK; }
    constexpr uint32_t generation() const { return bits >> INDEX_BITS; }
    constexpr bool valid() const { return bits != 0; }
    constexpr explicit operator bool() const { return bits != 0; }

    static constexpr NodeId make(uint32_t slot_index, uint32_t slot_generation) {
        return NodeId{((slot_generation & MAX_GENERATION) << INDEX_BITS) | (slot_index & INDEX_MASK)};
    }

    friend constexpr bool operator==(NodeId lhs, NodeId rhs) { return lhs.bits == rhs.bits; }
    friend constexpr bool operator!=(NodeId lhs, NodeId rhs) { return lhs.bits != rhs.bits; }
};

inline constexpr NodeId INVALID_NODE{};

} // namespace Splash
