// Written by Richard Christopher, Copyright 2026 NeoTec Digital
#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include <wayland-client.h>

#include "primary-selection-unstable-v1-client-protocol.h"
#include "xdg-decoration-unstable-v1-client-protocol.h"
#include "xdg-shell-client-protocol.h"

/**
 * The client half of the in-repo protocol harnesses: a real libwayland client,
 * not a mock.
 *
 * This deliberately speaks raw wayland-client rather than linking a toolkit.
 * The whole point of the protocol harness is to assert what goes over the wire -
 * configure/ack ordering, wl_buffer.release, pointer frame grouping, keymap
 * contents - and a toolkit both hides those and adds its own opinions about
 * them. Every event the harness cares about is recorded here, in order, so the
 * assertions can be about sequence and not just about counts.
 */
namespace VazioTest {

// One recorded pointer event. `serial` and `time` are kept because a group's
// identity is what the assertions are about, not the individual event.
struct PointerEvent {
    enum class Kind { Enter, Leave, Motion, Button, Frame };
    Kind kind = Kind::Frame;
    double sx = 0.0;
    double sy = 0.0;
    uint32_t button = 0;
    uint32_t state = 0;
    uint32_t serial = 0;
};

struct KeyEvent {
    uint32_t keycode = 0;   // as sent on the wire: evdev code, NOT +8
    uint32_t state = 0;
    uint32_t serial = 0;
};

/**
 * Globals the compositor advertises, with the versions it advertised them at.
 * Version is recorded rather than assumed: "wl_seat v9" is an assertion the
 * frame-grouping phase depends on, because frame only exists from v5.
 */
struct RegistryGlobals {
    struct wl_compositor* compositor = nullptr;
    struct wl_subcompositor* subcompositor = nullptr;
    struct wl_shm* shm = nullptr;
    struct wl_seat* seat = nullptr;
    struct xdg_wm_base* wm_base = nullptr;
    struct wl_data_device_manager* data_device_manager = nullptr;
    struct zxdg_decoration_manager_v1* decoration_manager = nullptr;
    struct zwp_primary_selection_device_manager_v1* primary_selection_manager = nullptr;

    uint32_t seat_version = 0;
    uint32_t wm_base_version = 0;
    uint32_t output_version = 0;
    bool saw_output = false;
    int output_count = 0;
    std::vector<std::string> names;

    bool complete() const {
        return compositor && subcompositor && shm && seat && wm_base &&
               data_device_manager && decoration_manager && primary_selection_manager;
    }
};

/**
 * A wl_shm buffer backed by an anonymous file, filled with a deterministic
 * pattern whose CRC32 the client keeps.
 *
 * The checksum is load-bearing, not decorative: it is recomputed after the
 * compositor has read and released the buffer, which is how the harness proves
 * the import path is read-only with respect to client memory. A compositor that
 * scribbles into a client's SHM pool corrupts every toolkit that reuses one.
 */
class ShmBuffer {
public:
    ShmBuffer() = default;
    ~ShmBuffer();

    ShmBuffer(const ShmBuffer&) = delete;
    ShmBuffer& operator=(const ShmBuffer&) = delete;

    bool create(struct wl_shm* shm, int32_t width, int32_t height, uint32_t format);

    // Deterministic, position-dependent ARGB fill. `seed` distinguishes the
    // surfaces of one run from each other.
    void fillPattern(uint32_t seed);

    uint32_t checksum() const;

    struct wl_buffer* buffer() const { return buffer_; }
    int32_t width() const { return width_; }
    int32_t height() const { return height_; }

    // Set by the wl_buffer.release listener installed in create().
    bool released = false;

private:
    struct wl_buffer* buffer_ = nullptr;
    uint8_t* pixels_ = nullptr;
    size_t size_ = 0;
    int32_t width_ = 0;
    int32_t height_ = 0;
    int32_t stride_ = 0;
};

/**
 * Everything one connected client observes. Passed as the user-data pointer of
 * every listener, so a single object carries the whole recorded history.
 */
struct ClientState {
    struct wl_display* display = nullptr;
    struct wl_registry* registry = nullptr;
    RegistryGlobals globals;

    struct wl_pointer* pointer = nullptr;
    struct wl_keyboard* keyboard = nullptr;
    uint32_t seat_capabilities = 0;

    std::vector<PointerEvent> pointer_events;
    std::vector<KeyEvent> key_events;

    // The keymap the compositor sent, parsed far enough to answer "does evdev
    // code N mean the keysym the compositor thinks it means".
    std::string keymap_text;
    uint32_t keymap_format = 0;

    // Set by the xdg_surface.configure listener; the client acks explicitly so
    // the negative control can decline to.
    uint32_t last_configure_serial = 0;
    int configure_count = 0;
    int toplevel_configure_count = 0;
    int32_t last_toplevel_width = 0;
    int32_t last_toplevel_height = 0;

    int popup_configure_count = 0;
    int popup_done_count = 0;
    int32_t popup_x = 0;
    int32_t popup_y = 0;

    int frame_done_count = 0;

    uint32_t decoration_mode = 0;
    int decoration_configure_count = 0;

    int selection_offer_count = 0;
    int primary_selection_offer_count = 0;
    std::vector<std::string> offered_mime_types;

    // Serial of the last input event, needed by xdg_popup.grab and by
    // wl_data_device.set_selection.
    uint32_t last_input_serial = 0;
};

/**
 * Create $XDG_RUNTIME_DIR, parents included, and verify it can hold a socket.
 *
 * CTest can set an environment variable but cannot create a directory, and the
 * private runtime dir these harnesses use lives under the build tree, which may
 * not exist yet on a fresh configure. The sun_path check is not paranoia: a
 * unix socket path over 107 bytes fails inside bind() with a message that names
 * neither the path nor the limit.
 *
 * @param error_out set to a human-readable reason when this returns false.
 */
bool prepareRuntimeDir(std::string& error_out);

// Bind everything the harness needs and block until the registry has settled.
// Returns false if the compositor failed to advertise a required global.
bool connectClient(ClientState& state, const char* socket_name);

void disconnectClient(ClientState& state);

// Non-blocking service: flush, then read and dispatch whatever is queued.
// Returns false when the connection has failed.
bool pumpClient(ClientState& state);

/**
 * Dispatch everything the compositor had already queued, and nothing after it.
 *
 * The only race-free barrier in the protocol: wl_display_roundtrip returns when
 * the server has answered a wl_display.sync issued now, and the server answers
 * requests in order, so every event queued before the sync has been dispatched
 * by the time it returns. A "pump until I see event X" loop is NOT equivalent -
 * it stops at the first X, which may be a stale one from an earlier phase, and
 * leaves the rest of the burst unread. That mistake produced a harness that
 * passed or failed depending on scheduling.
 */
bool roundtripClient(ClientState& state);

/**
 * Request one wl_surface.frame callback and count it into
 * ClientState::frame_done_count when it fires.
 *
 * This is the map signal every harness here uses. SpatialCompositor only sends
 * frame_done to a surface it has MAPPED, so a callback arriving is the
 * client-visible proof that the surface reached the scene. wl_buffer.release is
 * NOT that proof: wlroots holds a committed buffer until the surface commits the
 * next one, so a single-buffered client never sees one.
 */
void requestFrameCallback(ClientState& state, struct wl_surface* surface);

// Dispatch until `predicate` is true or the timeout expires.
template <typename Predicate>
bool pumpUntil(ClientState& state, Predicate predicate, int timeout_ms = 5000);

// Pointer-event bookkeeping used by the frame-grouping assertions: every
// non-frame event belongs to the group closed by the next frame.
struct FrameGrouping {
    int frames = 0;
    int groups = 0;         // groups that carried at least one event
    int empty_frames = 0;
    int biggest_group = 0;
};

FrameGrouping groupPointerEvents(const std::vector<PointerEvent>& events);

// xdg_wm_base ping/pong must be answered or the compositor may consider the
// client unresponsive. Installed by connectClient.
extern const struct xdg_wm_base_listener kWmBaseListener;

}  // namespace VazioTest

#include "wl_client_kit_inl.h"
