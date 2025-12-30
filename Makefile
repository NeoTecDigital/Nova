VULKAN_SDK_PATH = /usr/include/vulkan
STB_PATH = /usr/include/stb
TINYOBJ_PATH = ./Core/components/extern/tinyobj/
VK_EXP_PATH = /etc/vulkan/explicit_layer.d

CFLAGS = -std=c++20 -I $(VULKAN_SDK_PATH) -I $(STB_PATH) -I $(TINYOBJ_PATH)
LDFLAGS = -L $(VULKAN_SDK_PATH) -lSDL2 -lvulkan -ldl -pthread
NOVA_SOURCES = ./Nova.cpp ./Core/core.cpp ./Core/components/genesis.cpp ./Core/components/logger.cpp \
               ./Core/modules/device.cpp ./Core/modules/management.cpp ./Core/modules/presentation.cpp \
               ./Core/modules/render.cpp ./Core/modules/atomic/atomic.cpp \
               ./Core/modules/camera/camera.cpp ./Core/modules/camera/perspective.cpp \
               ./Core/modules/pipeline/pipeline.cpp ./Core/modules/pipeline/scene.cpp

all: wavecube_compute

wavecube_compute: wavecube_compute.cpp
	@echo "Building WaveCube compute binary..."
	g++ $(CFLAGS) -o wavecube_compute wavecube_compute.cpp $(NOVA_SOURCES) $(LDFLAGS)
	@echo "✅ Built: wavecube_compute"

clean:
	rm -f wavecube_compute

.PHONY: all clean
