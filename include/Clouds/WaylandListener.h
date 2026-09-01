// Written by Richard Christopher, Copyright 2026 NeoTec Digital
#pragma once

#ifndef WLR_USE_UNSTABLE
#define WLR_USE_UNSTABLE
#endif

#ifdef __cplusplus
extern "C" {
#endif
#include <wayland-server-core.h>
#ifdef __cplusplus
}
#endif

#include <cstddef>
#include <type_traits>

namespace Clouds {

/**
 * RAII binding between a wl_signal and a C++ member function.
 *
 * Lifetime contract (all three clauses are load bearing):
 *   1. The embedded wl_listener link is initialised at construction, so unbind()
 *      is well defined even when bind() was never called.
 *   2. unbind() must run while the signal's owner is still alive. Detaching a
 *      link writes through link->prev and link->next; if the wl_signal has
 *      already been freed those writes land in released memory.
 *   3. No listener callback may destroy the object that owns the listener
 *      currently being dispatched. Destruction must be deferred to a point
 *      outside signal dispatch (see SpatialCompositor::drainDestroyedWindows).
 */
template<typename T>
struct WaylandListener {
    struct wl_listener listener{};
    T* target = nullptr;
    void (T::*callback)(void*) = nullptr;
    bool bound = false;

    WaylandListener() {
        listener.notify = &WaylandListener<T>::notify;
        wl_list_init(&listener.link);
    }

    ~WaylandListener() { unbind(); }

    WaylandListener(const WaylandListener&) = delete;
    WaylandListener& operator=(const WaylandListener&) = delete;
    WaylandListener(WaylandListener&&) = delete;
    WaylandListener& operator=(WaylandListener&&) = delete;

    static void notify(struct wl_listener* l, void* data) {
        static_assert(std::is_standard_layout<WaylandListener<T>>::value,
                      "WaylandListener must be standard layout: notify() recovers the "
                      "wrapper from the wl_listener address via reinterpret_cast.");
        static_assert(offsetof(WaylandListener<T>, listener) == 0,
                      "wl_listener must be the first member of WaylandListener: notify() "
                      "recovers the wrapper from the wl_listener address via reinterpret_cast.");
        auto self = reinterpret_cast<WaylandListener<T>*>(l);
        if (self && self->target && self->callback) {
            (self->target->*(self->callback))(data);
        }
    }

    // Attach to a signal. Rebinding detaches the previous attachment first.
    void bind(T* tgt, void (T::*cb)(void*), struct wl_signal* signal) {
        if (!tgt || !cb || !signal) return;
        unbind();
        target = tgt;
        callback = cb;
        listener.notify = &WaylandListener<T>::notify;
        wl_signal_add(signal, &listener);
        bound = true;
    }

    // Detach from the signal. Idempotent, and safe when never bound.
    void unbind() {
        if (bound) {
            wl_list_remove(&listener.link);
            wl_list_init(&listener.link);
            bound = false;
        }
        target = nullptr;
        callback = nullptr;
    }

    bool isBound() const { return bound; }
};

} // namespace Clouds
