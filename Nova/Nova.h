#pragma once
// Use new modular architecture (core_base, nova_compute, nova_graphics)
#include "./core_base.h"
#include "./nova_compute.h"
#include "./nova_graphics.h"
#include "./config.h"

// Backward compatibility: still include old core.h for legacy code
// TODO: Remove this once all code migrated to new architecture
#include "./core.h"

#include <string>
#include <future>
namespace Nova {
// The goal of this layer of abstraction is to create a friendly user implementation for creating a graphics engine, for future projects.

// Forward declaration
class SMFTEngine;

// TODO: Cross Platform Support
class App {
    // Allow SMFTEngine to access private _architect for compute operations
    friend class SMFTEngine;

    public:
        bool initialized = false;

        App(Config);
        ~App();

        // TODO: Determine Default Initializers

        void illuminate();
        //void illuminate(fnManifest);

        // Mode detection
        enum class Mode { Compute, Graphics };
        Mode getMode() const { return _mode; }

        // Get appropriate interface
        Core* getCore();  // Base interface (compute or graphics)
        Compute* getCompute();  // Compute-only interface (nullptr if graphics mode)
        Graphics* getGraphics();  // Graphics interface (nullptr if compute mode)
        struct SDL_Window* getWindow() const { return _window; }

    private:
        Config _config;
        bool _suspended = false;
        struct SDL_Window* _window = nullptr;

        // Mode-specific instances
        Mode _mode;
        Compute* _architect_compute = nullptr;
        Graphics* _architect_graphics = nullptr;

        // Legacy compatibility
        Core* _architect = nullptr;  // Points to either _architect_compute or _architect_graphics

        VkDebugUtilsMessengerEXT _debug_messenger;

        void _initFramework();
        void _initSwapChain(std::promise<void>&, std::future<void>&, std::promise<void>&);
        void _initPipeline(std::future<void>&, std::promise<void>&);
        void _initBuffers();
        void _initSyncStructures();
        void _resizeWindow();
        void _initGraphicsPipeline();
};

} // namespace Nova
