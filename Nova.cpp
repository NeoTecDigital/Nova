#include "./Nova.h"

#include <thread>
#include <chrono>

#include <SDL2/SDL.h>
#include <SDL2/SDL_vulkan.h>

    ///////////////////
    // INSTANTIATION //
    ///////////////////


Nova::Nova(NovaConfig config)
    {
        report(LOGGER::INFO, "Nova - Instantiating ..");

        setLogLevel(config.debug_level.c_str());
        
        // Initialize SDL and create a window
        SDL_Init(SDL_INIT_VIDEO);
        SDL_WindowFlags window_flags = (SDL_WindowFlags)(SDL_WINDOW_VULKAN | SDL_WINDOW_RESIZABLE | SDL_WINDOW_ALLOW_HIGHDPI);

        _window = SDL_CreateWindow(
            config.name.c_str(),
            SDL_WINDOWPOS_UNDEFINED,
            SDL_WINDOWPOS_UNDEFINED,
            config.screen.width,
            config.screen.height,
            window_flags
        );

        if (_window == nullptr) 
            {
                report(LOGGER::ERROR, "Nova - Failed to create SDL window ..");
                return;
            }


        // TODO: Wrap all of this into a subset of init functions 
        //       that use proper fences and semaphores with the following order:
        // Device()
        // Presentation()
        // Management()
        // Scene()
        // Render()

        _architect = new NovaCore(config.screen, config.debug_level);
        _initFramework(); // Do we want to handle this in the Nova?

        _architect->swapchain.support = _architect->querySwapChainSupport(_architect->physical_device);
        _architect->querySwapChainDetails();

        // Now we can construct the Swapchain
        std::promise<void> waitForSwapchain;
        std::future<void> waitingForFrameBuffer = waitForSwapchain.get_future();
        std::promise<void> startPipeline;
        std::future<void> startingPipeline = startPipeline.get_future();
        std::promise<void> waitForPipeline; 
        std::future<void> waitingForPipeline = waitForPipeline.get_future();

        std::thread _swapchain_thread(&Nova::_initSwapChain, this, std::ref(startPipeline), std::ref(waitingForPipeline), std::ref(waitForSwapchain));
        std::thread _pipeline_thread(&Nova::_initPipeline, this, std::ref(startingPipeline), std::ref(waitForPipeline));

        _pipeline_thread.join();
        _swapchain_thread.join();
        waitingForFrameBuffer.wait();


        // Now we can construct the Command Buffers 
        _initBuffers();
        _initSyncStructures();
        report(LOGGER::INFO, "Nova - Initialized ..");
    }

Nova::~Nova() 
    {
        report(LOGGER::INFO, "Nova - Deconstructing ..");
        vkDeviceWaitIdle(_architect->logical_device);

        if (USE_VALIDATION_LAYERS) 
            { 
                report(LOGGER::VLINE, "\t .. Destroying Debug Messenger ..");
                destroyDebugUtilsMessengerEXT(_architect->instance, _debug_messenger, nullptr); 
            }

        if (initialized) 
            {         
                report(LOGGER::VLINE, "\t .. Destroying Nova ..");
                delete _architect;

                report(LOGGER::VLINE, "\t .. Destroying Window ..");
                SDL_DestroyWindow(_window); 

                SDL_Quit();
            }
        
        report(LOGGER::INFO, "Nova - Destroyed ..");
    }


    /////////////////////////
    // TOP LEVEL FUNCTIONS //
    /////////////////////////

void Nova::illuminate()
//void Nova::illuminate(fnManifest fnManifest)
    {
        report(LOGGER::INFO, "Nova - Illuminating ..");

        SDL_Event _e;
        bool _quit = false;

        while (!_quit) {
            while (SDL_PollEvent(&_e)) 
                {
                    if (_e.type == SDL_QUIT) { _quit = !_quit; }
                    
                    if (_e.type == SDL_KEYDOWN) 
                        { if (_e.key.keysym.sym == SDLK_ESCAPE) { _quit = !_quit; } }

                    if (_e.type == SDL_WINDOWEVENT) 
                        { 
                            switch (_e.window.event) 
                                {
                                    case SDL_WINDOWEVENT_MINIMIZED: _suspended = true; break;
                                    case SDL_WINDOWEVENT_RESTORED: _suspended = false; break;
                                    case SDL_WINDOWEVENT_RESIZED: _resizeWindow(); break;
                                }
                        }

                    _architect->player_camera.processEvents(_e);
                }
            
            if (_suspended) 
                {
                    std::this_thread::sleep_for(std::chrono::milliseconds(100));
                    continue;
                }
            else 
                { _architect->drawFrame(); }

            //fnManifest();
        }

        return;
    }


    //////////////////
    // INITIALIZERS //
    //////////////////

void Nova::_initFramework() 
    {
        report(LOGGER::INFO, "Nova - Initializing Frameworks ..");

        SDL_Vulkan_CreateSurface(_window, _architect->instance, &_architect->surface);
        createDebugMessenger(&_architect->instance, &_debug_messenger);
        _architect->createPhysicalDevice();
        _architect->createLogicalDevice();

        return;
    }

void Nova::_initSwapChain(std::promise<void>& startPipeline, std::future<void>& waitingForPipeline, std::promise<void>& waitForFrameBuffer) 
    {
        report(LOGGER::INFO, "Nova - Initializing SwapChain Buffers ..");

        _architect->constructSwapChain();
        _architect->constructImageViews();
        startPipeline.set_value();
        waitingForPipeline.wait();
        _architect->createFrameBuffers();
        waitForFrameBuffer.set_value();
     
        return;
    }

void Nova::_initPipeline(std::future<void>& startingPipeline, std::promise<void>& waitForPipeline) 
    {
        report(LOGGER::INFO, "Nova - Initializing Graphics Pipeline ..");

        // abstract both of these as part of the Nova and rename _architect to Nova

        startingPipeline.wait();
        _architect->createRenderPass();
        _architect->createDescriptorSetLayout();
        _architect->constructGraphicsPipeline();
        //_architect->constructComputePipeline();
        waitForPipeline.set_value();
     
        return;
    }

void Nova::_initBuffers() 
    {
        report(LOGGER::INFO, "Nova - Initializing Command Operator ..");

        _architect->createCommandPool(); 
        _architect->createTextureImage(); 
        _architect->constructVertexBuffer();
        _architect->constructIndexBuffer(); 
        _architect->constructUniformBuffer(); 
        // TODO: multithread Command Buffer to init as part of the Management Phase
        _architect->constructDescriptorPool();
        _architect->createDescriptorSets();
        _architect->createCommandBuffers();
     
        return;
    }

void Nova::_initSyncStructures()
    {
        report(LOGGER::INFO, "Nova - Initializing Synchronization Structures ..");

        _architect->createSyncObjects();

        return;
    }


    ///////////////////
    // RESIZE WINDOW //
    ///////////////////

inline void Nova::_resizeWindow()
    {
        report(LOGGER::VERBOSE, "Nova - Resizing Window ..");

        int w, h;
        SDL_Vulkan_GetDrawableSize(_window, &w, &h);
        
        _config.screen.width = w;
        _config.screen.height = h;

        _architect->setWindowExtent(_config.screen);
        _architect->framebuffer_resized = true;
        return;
    }   

