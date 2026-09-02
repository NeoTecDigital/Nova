// Written by Richard Christopher, Copyright 2026 NeoTec Digital
#pragma once

// Template half of wl_client_kit.h. Split out rather than inlined into the
// header so neither file carries two jobs; included from the bottom of
// wl_client_kit.h and never on its own.

#include <ctime>

namespace VazioTest {

inline int64_t monotonicMs() {
    struct timespec ts = {};
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return static_cast<int64_t>(ts.tv_sec) * 1000 + ts.tv_nsec / 1000000;
}

template <typename Predicate>
bool pumpUntil(ClientState& state, Predicate predicate, int timeout_ms) {
    const int64_t deadline = monotonicMs() + timeout_ms;
    while (!predicate()) {
        if (monotonicMs() > deadline) return false;
        if (!pumpClient(state)) return false;
    }
    return true;
}

}  // namespace VazioTest
