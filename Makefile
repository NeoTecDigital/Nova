# Written by Richard Christopher, Copyright 2026 NeoTec Digital
#
# This Makefile no longer defines its own source list. It used to carry a private
# copy of NOVA_SOURCES that diverged from CMakeLists.txt (it had
# Core/modules/atomic/atomic.cpp and Core/modules/pipeline/scene.cpp; CMake did
# not), which left NovaCoreLegacy::updateUniformBuffer unresolved in libNova.a.
#
# CMakeLists.txt is now the single source of truth for the Nova source set.
# Everything here delegates to it so the two can never diverge again.

BUILD_DIR ?= build
CMAKE     ?= cmake
JOBS      ?= $(shell nproc 2>/dev/null || echo 4)

.PHONY: all configure wavecube_compute vazio vazio-dev test clean distclean

all: configure
	$(CMAKE) --build $(BUILD_DIR) -j$(JOBS)

configure: $(BUILD_DIR)/CMakeCache.txt

$(BUILD_DIR)/CMakeCache.txt:
	$(CMAKE) -S . -B $(BUILD_DIR)

wavecube_compute: configure
	$(CMAKE) --build $(BUILD_DIR) -j$(JOBS) --target wavecube_compute

vazio: configure
	$(CMAKE) --build $(BUILD_DIR) -j$(JOBS) --target vazio

vazio-dev: configure
	$(CMAKE) --build $(BUILD_DIR) -j$(JOBS) --target vazio-dev

test: all
	cd $(BUILD_DIR) && ctest --output-on-failure

clean:
	@if [ -d $(BUILD_DIR) ]; then $(CMAKE) --build $(BUILD_DIR) --target clean; fi

distclean:
	rm -rf $(BUILD_DIR)
