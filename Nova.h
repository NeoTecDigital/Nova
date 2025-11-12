#pragma once
#include "./Core/core.h"
#include "./Core/config.h"


#include <string>
#include <future>

// The goal of this layer of abstraction is to create a friendly user implementation for creating a graphics engine, for future projects.

// TODO: Cross Platform Support
class Nova {
    public:
        bool initialized = false;

        Nova(NovaConfig);
        ~Nova();

        // TODO: Determine Default Initializers 
        

        void illuminate();
        //void illuminate(fnManifest);

    private:
        NovaConfig _config;
        bool _suspended = false;
        struct SDL_Window* _window = nullptr;
        NovaCore* _architect;

        VkDebugUtilsMessengerEXT _debug_messenger;

        void _initFramework();
        void _initSwapChain(std::promise<void>&, std::future<void>&, std::promise<void>&);
        void _initPipeline(std::future<void>&, std::promise<void>&);
        void _initBuffers();
        void _initSyncStructures();
        void _resizeWindow();
};
