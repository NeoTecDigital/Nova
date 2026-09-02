// Written by Richard Christopher, Copyright 2026 NeoTec Digital
//
// Client-side scaffolding shared by the in-repo protocol harnesses. See
// wl_client_kit.h for why this speaks raw wayland-client.

#include "wl_client_kit.h"

#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fcntl.h>
#include <poll.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

namespace VazioTest {
namespace {

// --- CRC32 (IEEE 802.3), computed rather than tabulated: the buffers are small
// and a 1 KiB static table is more code than the loop it saves. ---
uint32_t crc32Update(uint32_t crc, const uint8_t* data, size_t length) {
    crc = ~crc;
    for (size_t i = 0; i < length; ++i) {
        crc ^= data[i];
        for (int bit = 0; bit < 8; ++bit) {
            crc = (crc >> 1) ^ (0xEDB88320u & (0u - (crc & 1u)));
        }
    }
    return ~crc;
}

// --- wl_buffer ---------------------------------------------------------------

void onBufferRelease(void* data, struct wl_buffer*) {
    static_cast<ShmBuffer*>(data)->released = true;
}

const struct wl_buffer_listener kBufferListener = { .release = onBufferRelease };

// --- wl_callback (frame) -----------------------------------------------------

void onFrameDone(void* data, struct wl_callback* callback, uint32_t) {
    ++static_cast<ClientState*>(data)->frame_done_count;
    wl_callback_destroy(callback);
}

const struct wl_callback_listener kFrameListener = { .done = onFrameDone };

// --- xdg_wm_base -------------------------------------------------------------

void onWmBasePing(void*, struct xdg_wm_base* base, uint32_t serial) {
    xdg_wm_base_pong(base, serial);
}

// --- wl_pointer --------------------------------------------------------------

void onPointerEnter(void* data, struct wl_pointer*, uint32_t serial, struct wl_surface*,
                    wl_fixed_t sx, wl_fixed_t sy) {
    auto* state = static_cast<ClientState*>(data);
    state->last_input_serial = serial;
    state->pointer_events.push_back({ PointerEvent::Kind::Enter, wl_fixed_to_double(sx),
                                      wl_fixed_to_double(sy), 0, 0, serial });
}

void onPointerLeave(void* data, struct wl_pointer*, uint32_t serial, struct wl_surface*) {
    auto* state = static_cast<ClientState*>(data);
    state->last_input_serial = serial;
    state->pointer_events.push_back({ PointerEvent::Kind::Leave, 0, 0, 0, 0, serial });
}

void onPointerMotion(void* data, struct wl_pointer*, uint32_t, wl_fixed_t sx, wl_fixed_t sy) {
    auto* state = static_cast<ClientState*>(data);
    state->pointer_events.push_back({ PointerEvent::Kind::Motion, wl_fixed_to_double(sx),
                                      wl_fixed_to_double(sy), 0, 0, 0 });
}

void onPointerButton(void* data, struct wl_pointer*, uint32_t serial, uint32_t,
                     uint32_t button, uint32_t button_state) {
    auto* state = static_cast<ClientState*>(data);
    state->last_input_serial = serial;
    state->pointer_events.push_back({ PointerEvent::Kind::Button, 0, 0, button, button_state, serial });
}

void onPointerFrame(void* data, struct wl_pointer*) {
    auto* state = static_cast<ClientState*>(data);
    state->pointer_events.push_back({ PointerEvent::Kind::Frame, 0, 0, 0, 0, 0 });
}

void onPointerAxis(void*, struct wl_pointer*, uint32_t, uint32_t, wl_fixed_t) {}
void onPointerAxisSource(void*, struct wl_pointer*, uint32_t) {}
void onPointerAxisStop(void*, struct wl_pointer*, uint32_t, uint32_t) {}
void onPointerAxisDiscrete(void*, struct wl_pointer*, uint32_t, int32_t) {}
void onPointerAxisValue120(void*, struct wl_pointer*, uint32_t, int32_t) {}
void onPointerAxisDirection(void*, struct wl_pointer*, uint32_t, uint32_t) {}

const struct wl_pointer_listener kPointerListener = {
    .enter = onPointerEnter,
    .leave = onPointerLeave,
    .motion = onPointerMotion,
    .button = onPointerButton,
    .axis = onPointerAxis,
    .frame = onPointerFrame,
    .axis_source = onPointerAxisSource,
    .axis_stop = onPointerAxisStop,
    .axis_discrete = onPointerAxisDiscrete,
    .axis_value120 = onPointerAxisValue120,
    .axis_relative_direction = onPointerAxisDirection,
};

// --- wl_keyboard -------------------------------------------------------------

void onKeyboardKeymap(void* data, struct wl_keyboard*, uint32_t format, int32_t fd, uint32_t size) {
    auto* state = static_cast<ClientState*>(data);
    state->keymap_format = format;
    void* mapped = mmap(nullptr, size, PROT_READ, MAP_PRIVATE, fd, 0);
    if (mapped != MAP_FAILED) {
        state->keymap_text.assign(static_cast<const char*>(mapped), strnlen(static_cast<const char*>(mapped), size));
        munmap(mapped, size);
    }
    close(fd);
}

void onKeyboardEnter(void* data, struct wl_keyboard*, uint32_t serial, struct wl_surface*,
                     struct wl_array*) {
    static_cast<ClientState*>(data)->last_input_serial = serial;
}

void onKeyboardLeave(void*, struct wl_keyboard*, uint32_t, struct wl_surface*) {}

void onKeyboardKey(void* data, struct wl_keyboard*, uint32_t serial, uint32_t,
                   uint32_t key, uint32_t key_state) {
    auto* state = static_cast<ClientState*>(data);
    state->last_input_serial = serial;
    state->key_events.push_back({ key, key_state, serial });
}

void onKeyboardModifiers(void*, struct wl_keyboard*, uint32_t, uint32_t, uint32_t, uint32_t, uint32_t) {}
void onKeyboardRepeatInfo(void*, struct wl_keyboard*, int32_t, int32_t) {}

const struct wl_keyboard_listener kKeyboardListener = {
    .keymap = onKeyboardKeymap,
    .enter = onKeyboardEnter,
    .leave = onKeyboardLeave,
    .key = onKeyboardKey,
    .modifiers = onKeyboardModifiers,
    .repeat_info = onKeyboardRepeatInfo,
};

// --- wl_seat -----------------------------------------------------------------

void onSeatCapabilities(void* data, struct wl_seat* seat, uint32_t capabilities) {
    auto* state = static_cast<ClientState*>(data);
    state->seat_capabilities = capabilities;

    if ((capabilities & WL_SEAT_CAPABILITY_POINTER) && !state->pointer) {
        state->pointer = wl_seat_get_pointer(seat);
        wl_pointer_add_listener(state->pointer, &kPointerListener, state);
    }
    if ((capabilities & WL_SEAT_CAPABILITY_KEYBOARD) && !state->keyboard) {
        state->keyboard = wl_seat_get_keyboard(seat);
        wl_keyboard_add_listener(state->keyboard, &kKeyboardListener, state);
    }
}

void onSeatName(void*, struct wl_seat*, const char*) {}

const struct wl_seat_listener kSeatListener = {
    .capabilities = onSeatCapabilities,
    .name = onSeatName,
};

// --- wl_registry -------------------------------------------------------------

// Bound versions are pinned deliberately. Binding "whatever the server offers"
// makes the harness's own listener tables version-dependent, and a listener that
// is short one function pointer is a null call, not a warning.
constexpr uint32_t kSeatBindVersion = 5;   // wl_pointer.frame exists from v5
constexpr uint32_t kShmBindVersion = 1;
constexpr uint32_t kCompositorBindVersion = 4;
constexpr uint32_t kWmBaseBindVersion = 3;
constexpr uint32_t kDataDeviceBindVersion = 3;

template <typename T>
T* bindGlobal(struct wl_registry* registry, uint32_t name, const struct wl_interface* interface,
              uint32_t offered, uint32_t wanted) {
    const uint32_t version = offered < wanted ? offered : wanted;
    return static_cast<T*>(wl_registry_bind(registry, name, interface, version));
}

void onRegistryGlobal(void* data, struct wl_registry* registry, uint32_t name,
                      const char* interface, uint32_t version) {
    auto* state = static_cast<ClientState*>(data);
    RegistryGlobals& g = state->globals;
    g.names.emplace_back(interface);

    if (strcmp(interface, wl_compositor_interface.name) == 0) {
        g.compositor = bindGlobal<struct wl_compositor>(registry, name, &wl_compositor_interface,
                                                        version, kCompositorBindVersion);
    } else if (strcmp(interface, wl_subcompositor_interface.name) == 0) {
        g.subcompositor = bindGlobal<struct wl_subcompositor>(registry, name, &wl_subcompositor_interface,
                                                              version, 1);
    } else if (strcmp(interface, wl_shm_interface.name) == 0) {
        g.shm = bindGlobal<struct wl_shm>(registry, name, &wl_shm_interface, version, kShmBindVersion);
    } else if (strcmp(interface, wl_seat_interface.name) == 0) {
        g.seat_version = version;
        g.seat = bindGlobal<struct wl_seat>(registry, name, &wl_seat_interface, version, kSeatBindVersion);
        wl_seat_add_listener(g.seat, &kSeatListener, state);
    } else if (strcmp(interface, xdg_wm_base_interface.name) == 0) {
        g.wm_base_version = version;
        g.wm_base = bindGlobal<struct xdg_wm_base>(registry, name, &xdg_wm_base_interface,
                                                   version, kWmBaseBindVersion);
        xdg_wm_base_add_listener(g.wm_base, &kWmBaseListener, state);
    } else if (strcmp(interface, wl_data_device_manager_interface.name) == 0) {
        g.data_device_manager = bindGlobal<struct wl_data_device_manager>(
            registry, name, &wl_data_device_manager_interface, version, kDataDeviceBindVersion);
    } else if (strcmp(interface, zxdg_decoration_manager_v1_interface.name) == 0) {
        g.decoration_manager = bindGlobal<struct zxdg_decoration_manager_v1>(
            registry, name, &zxdg_decoration_manager_v1_interface, version, 1);
    } else if (strcmp(interface, zwp_primary_selection_device_manager_v1_interface.name) == 0) {
        g.primary_selection_manager = bindGlobal<struct zwp_primary_selection_device_manager_v1>(
            registry, name, &zwp_primary_selection_device_manager_v1_interface, version, 1);
    } else if (strcmp(interface, wl_output_interface.name) == 0) {
        g.saw_output = true;
        g.output_version = version;
        ++g.output_count;
    }
}

void onRegistryGlobalRemove(void*, struct wl_registry*, uint32_t) {}

const struct wl_registry_listener kRegistryListener = {
    .global = onRegistryGlobal,
    .global_remove = onRegistryGlobalRemove,
};

int createAnonymousFile(size_t size) {
    int fd = memfd_create("vazio-harness-shm", MFD_CLOEXEC | MFD_ALLOW_SEALING);
    if (fd < 0) return -1;
    if (ftruncate(fd, static_cast<off_t>(size)) != 0) {
        close(fd);
        return -1;
    }
    // Shrink-sealed only: the compositor must be able to trust the mapping's
    // length, and the client still needs to write into it.
    fcntl(fd, F_ADD_SEALS, F_SEAL_SHRINK);
    return fd;
}

}  // namespace

const struct xdg_wm_base_listener kWmBaseListener = { .ping = onWmBasePing };

// --- ShmBuffer ---------------------------------------------------------------

ShmBuffer::~ShmBuffer() {
    if (buffer_) wl_buffer_destroy(buffer_);
    if (pixels_) munmap(pixels_, size_);
}

bool ShmBuffer::create(struct wl_shm* shm, int32_t width, int32_t height, uint32_t format) {
    width_ = width;
    height_ = height;
    stride_ = width * 4;
    size_ = static_cast<size_t>(stride_) * static_cast<size_t>(height);

    int fd = createAnonymousFile(size_);
    if (fd < 0) return false;

    void* mapped = mmap(nullptr, size_, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    if (mapped == MAP_FAILED) {
        close(fd);
        return false;
    }
    pixels_ = static_cast<uint8_t*>(mapped);

    struct wl_shm_pool* pool = wl_shm_create_pool(shm, fd, static_cast<int32_t>(size_));
    buffer_ = wl_shm_pool_create_buffer(pool, 0, width_, height_, stride_, format);
    wl_shm_pool_destroy(pool);
    close(fd);

    if (!buffer_) return false;
    wl_buffer_add_listener(buffer_, &kBufferListener, this);
    return true;
}

void ShmBuffer::fillPattern(uint32_t seed) {
    for (int32_t y = 0; y < height_; ++y) {
        auto* row = reinterpret_cast<uint32_t*>(pixels_ + static_cast<size_t>(y) * stride_);
        for (int32_t x = 0; x < width_; ++x) {
            const uint32_t r = static_cast<uint32_t>((x * 7 + seed) & 0xFF);
            const uint32_t g = static_cast<uint32_t>((y * 11 + seed) & 0xFF);
            const uint32_t b = static_cast<uint32_t>(((x ^ y) * 13 + seed) & 0xFF);
            row[x] = 0xFF000000u | (r << 16) | (g << 8) | b;
        }
    }
}

uint32_t ShmBuffer::checksum() const {
    return pixels_ ? crc32Update(0, pixels_, size_) : 0;
}

bool prepareRuntimeDir(std::string& error_out) {
    const char* runtime_dir = getenv("XDG_RUNTIME_DIR");
    if (!runtime_dir || runtime_dir[0] == '\0') {
        error_out = "XDG_RUNTIME_DIR is unset; the test properties must provide one";
        return false;
    }

    const std::string path = runtime_dir;
    if (path.size() > 80) {
        error_out = "XDG_RUNTIME_DIR '" + path + "' leaves no room for a socket name in sun_path";
        return false;
    }

    for (size_t i = 1; i <= path.size(); ++i) {
        if (i != path.size() && path[i] != '/') continue;
        const std::string component = path.substr(0, i);
        if (mkdir(component.c_str(), 0700) != 0 && errno != EEXIST) {
            error_out = "could not create '" + component + "': " + strerror(errno);
            return false;
        }
    }
    return true;
}

// --- connection --------------------------------------------------------------

bool connectClient(ClientState& state, const char* socket_name) {
    state.display = wl_display_connect(socket_name);
    if (!state.display) {
        fprintf(stderr, "wl_client_kit: wl_display_connect(%s) failed: %s\n",
                socket_name ? socket_name : "(default)", strerror(errno));
        return false;
    }

    state.registry = wl_display_get_registry(state.display);
    wl_registry_add_listener(state.registry, &kRegistryListener, &state);

    // Two roundtrips: the first delivers the globals, the second delivers the
    // events the binds in the first one triggered (wl_seat.capabilities above
    // all - without it there is no pointer and no keyboard to listen on).
    if (wl_display_roundtrip(state.display) < 0) return false;
    if (wl_display_roundtrip(state.display) < 0) return false;

    return state.globals.complete();
}

void disconnectClient(ClientState& state) {
    if (state.keyboard) { wl_keyboard_release(state.keyboard); state.keyboard = nullptr; }
    if (state.pointer) { wl_pointer_release(state.pointer); state.pointer = nullptr; }
    if (state.registry) { wl_registry_destroy(state.registry); state.registry = nullptr; }
    if (state.display) { wl_display_disconnect(state.display); state.display = nullptr; }
}

bool pumpClient(ClientState& state) {
    if (!state.display) return false;

    while (wl_display_prepare_read(state.display) != 0) {
        if (wl_display_dispatch_pending(state.display) < 0) return false;
    }
    if (wl_display_flush(state.display) < 0 && errno != EAGAIN) {
        wl_display_cancel_read(state.display);
        return false;
    }

    struct pollfd pfd = { wl_display_get_fd(state.display), POLLIN, 0 };
    const int ready = poll(&pfd, 1, 10);
    if (ready > 0) {
        if (wl_display_read_events(state.display) < 0) return false;
    } else {
        wl_display_cancel_read(state.display);
        if (ready < 0 && errno != EINTR) return false;
    }

    return wl_display_dispatch_pending(state.display) >= 0;
}

void requestFrameCallback(ClientState& state, struct wl_surface* surface) {
    struct wl_callback* callback = wl_surface_frame(surface);
    wl_callback_add_listener(callback, &kFrameListener, &state);
}

bool roundtripClient(ClientState& state) {
    return state.display != nullptr && wl_display_roundtrip(state.display) >= 0;
}

FrameGrouping groupPointerEvents(const std::vector<PointerEvent>& events) {
    FrameGrouping grouping;
    int pending = 0;
    for (const PointerEvent& event : events) {
        if (event.kind != PointerEvent::Kind::Frame) {
            ++pending;
            continue;
        }
        ++grouping.frames;
        if (pending == 0) {
            ++grouping.empty_frames;
        } else {
            ++grouping.groups;
            if (pending > grouping.biggest_group) grouping.biggest_group = pending;
        }
        pending = 0;
    }
    return grouping;
}

}  // namespace VazioTest
