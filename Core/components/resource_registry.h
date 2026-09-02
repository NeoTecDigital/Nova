// Written by Richard Christopher, Copyright 2026 NeoTec Digital
#pragma once
#include <functional>
#include <utility>
#include <vector>
#include <string>

namespace NovaRAII {

/**
 * Resource Registry for Dynamic Cleanup Injection
 *
 * Principle: "Register cleanup at creation time"
 *
 * This solves the fundamental problem:
 * - Resources created conditionally (based on mode, config, etc.)
 * - Must be destroyed in reverse order of creation
 * - Only if they were actually created
 *
 * Usage:
 *   registry.register_resource("swapchain", [=]() {
 *       vkDestroySwapchainKHR(device, swapchain, nullptr);
 *   });
 *
 * On destruction, all registered cleanup functions are called in reverse order.
 *
 * OWNERSHIP INVARIANT (the reason run_and_release exists)
 * ------------------------------------------------------
 * A registry embedded in a base class runs its entries from the BASE
 * destructor, which is the LAST thing to execute: every derived destructor
 * body has already returned and every derived data member has already been
 * destroyed by then. So a cleanup function may only touch state owned by the
 * class that owns the registry.
 *
 * A derived class that needs registry-driven cleanup must therefore run and
 * unregister its own entry in its OWN destructor, while its members are still
 * alive:
 *
 *   ~Derived() { registry.run_and_release("derived_thing"); ... }
 *
 * Doing it in the destructor BODY rather than through a member token is
 * deliberate: the body runs before any member of Derived is destroyed, so it
 * carries no declaration-order dependency. Ignoring this reads freed memory -
 * survivable at -O0 by accident, a double free at -O2.
 */
class ResourceRegistry {
public:
    ResourceRegistry() = default;
    ~ResourceRegistry() { cleanup_all(); }

    // No copy, only move
    ResourceRegistry(const ResourceRegistry&) = delete;
    ResourceRegistry& operator=(const ResourceRegistry&) = delete;
    ResourceRegistry(ResourceRegistry&&) = default;
    ResourceRegistry& operator=(ResourceRegistry&&) = default;

    /**
     * Register a resource for cleanup
     * @param name Key for the resource. Also the handle release()/
     *             run_and_release() address it by, so it must be stable.
     * @param cleanup_fn Function to call to clean up this resource
     */
    void register_resource(const std::string& name, std::function<void()> cleanup_fn) {
        resources.push_back({name, std::move(cleanup_fn)});
    }

    /**
     * Clean up all registered resources in reverse order (LIFO)
     * This ensures proper destruction order (last created, first destroyed)
     */
    void cleanup_all() {
        // Detached before the walk: a cleanup function that registers or
        // releases would otherwise reallocate the vector being iterated.
        std::vector<Resource> pending;
        pending.swap(resources);
        for (auto it = pending.rbegin(); it != pending.rend(); ++it) {
            invoke(it->cleanup);
        }
    }

    /**
     * Run every entry filed under `name`, newest first, and unregister them.
     *
     * The entries are detached before any of them runs, so cleanup_all() can
     * never see a half-run entry and a cleanup function is free to touch the
     * registry. A name that was never registered is not an error: it is how a
     * conditional resource reports "never created".
     *
     * @return how many entries ran.
     */
    size_t run_and_release(const std::string& name) {
        std::vector<std::function<void()>> matched = detach(name);
        for (auto it = matched.rbegin(); it != matched.rend(); ++it) {
            invoke(*it);
        }
        return matched.size();
    }

    /**
     * Unregister every entry filed under `name` WITHOUT running it. For state
     * whose owner has already torn it down by another route.
     *
     * @return how many entries were dropped.
     */
    size_t release(const std::string& name) { return detach(name).size(); }

    /**
     * Whether any entry is filed under `name`.
     */
    bool contains(const std::string& name) const {
        for (const Resource& held : resources) {
            if (held.name == name) return true;
        }
        return false;
    }

    /**
     * Get count of registered resources (for debugging)
     */
    size_t count() const { return resources.size(); }

    /**
     * Check if any resources are registered
     */
    bool empty() const { return resources.empty(); }

private:
    struct Resource {
        std::string name;
        std::function<void()> cleanup;
    };

    // Swallow exceptions during cleanup so one failure cannot strand the rest.
    static void invoke(const std::function<void()>& cleanup) {
        if (!cleanup) return;
        try {
            cleanup();
        } catch (...) {
            // Nothing actionable at teardown; the remaining entries still run.
        }
    }

    // Remove every entry named `name`, preserving registration order in the
    // returned vector.
    std::vector<std::function<void()>> detach(const std::string& name) {
        std::vector<std::function<void()>> matched;
        std::vector<Resource> kept;
        kept.reserve(resources.size());
        for (Resource& held : resources) {
            if (held.name == name) {
                matched.push_back(std::move(held.cleanup));
            } else {
                kept.push_back(std::move(held));
            }
        }
        resources.swap(kept);
        return matched;
    }

    std::vector<Resource> resources;
};

} // namespace NovaRAII
