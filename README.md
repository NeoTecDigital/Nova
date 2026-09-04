# Nova
The Nova Graphics Engine, built on Vulkan, streamlines GPU compute and rendering, focusing on AMD and non-CUDA GPUs.

Nova simplifies Vulkan, reducing boilerplate, while offering a flexible graphics library comparable to engines like Pygame. Its goal is to enable seamless creation of machine learning projects or MVP prototypes without sacrificing quality, usability, or ease of use.

This library addresses the complexity of GPU programming, making it accessible for efficient prototyping and development.

# TODOs
 - [ ] Object Loading
 - [ ] Camera Implementation
 - [ ] Shader Abstraction

# Example
You can find a Makefile (that may require some modifications to your system). You may be required to install the required dependencies to run this library. Currently we are running Vulkan with SDL, but there may be opportunity in the future to grow support for DX and Metal. You can run `make` to build, and `./wavecube_compute` to run the compute driver.

`Nova/wavecube_compute.cpp` is the worked example of driving `Nova::App`. The
older `Example/` tree (`main.cpp`, `NovaExample.*`) was removed in B6: it was
never in the CMake build, its own Makefile still pointed at `../Core/`, and it
was written against the pre-namespace API (global `Nova` and `NovaConfig`, now
`Nova::App` and `Nova::Config`), so it had not been buildable for some time. It
is recoverable at commit 0f40d65.
