// Written by Richard Christopher, Copyright 2026 NeoTec Digital
//
// The seat half of the compositor: physical input devices arriving on
// backend.events.new_input, the keyboard and pointer routing they feed, and the
// single absolute pointer position every producer writes through.
//
// Under DRM this is the only source of input there is - libinput sits inside
// the autocreated backend and speaks evdev directly. Under the interim SDL
// window the same entry points are driven by SDL. Neither producer knows about
// the other, which is the whole point: the SDL half can be deleted without the
// DRM half noticing.
#include "./SpatialCompositor.h"
#include "Nova/components/logger.h"

#include <linux/input-event-codes.h>
#include <algorithm>
#include <ctime>

namespace Vazio {

namespace {

// Key repeat matching the X11/libinput defaults. Clients that draw their own
// repeat need these numbers; without them a held key repeats at whatever the
// client guesses, or not at all.
constexpr int32_t kRepeatRateHz = 25;
constexpr int32_t kRepeatDelayMs = 600;

// Event timestamps in the currency libinput and every client use: milliseconds
// on CLOCK_MONOTONIC, truncated to 32 bits. Synthesised events need one too -
// a constant zero makes every double-click a double-click and every drag
// instantaneous, because the client's own timing logic reads this field.
uint32_t monotonicTimeMs() {
    struct timespec now = {};
    clock_gettime(CLOCK_MONOTONIC, &now);
    return static_cast<uint32_t>(now.tv_sec * 1000 + now.tv_nsec / 1000000);
}

const char* describeDeviceType(enum wlr_input_device_type type) {
    switch (type) {
        case WLR_INPUT_DEVICE_KEYBOARD:   return "keyboard";
        case WLR_INPUT_DEVICE_POINTER:    return "pointer";
        case WLR_INPUT_DEVICE_TOUCH:      return "touch";
        case WLR_INPUT_DEVICE_TABLET:     return "tablet";
        case WLR_INPUT_DEVICE_TABLET_PAD: return "tablet-pad";
        case WLR_INPUT_DEVICE_SWITCH:     return "switch";
    }
    return "unknown";
}

} // namespace

// --- Button currency ---
// The scene graph indexes pointer buttons 1/2/3 because that is what its only
// producer used to be. The seat and every client speak evdev BTN_* codes. One
// place owns the correspondence so the two directions cannot drift apart.

uint32_t sceneButtonToEvdev(uint32_t scene_button) {
    switch (scene_button) {
        case 2: return BTN_MIDDLE;
        case 3: return BTN_RIGHT;
        default: return BTN_LEFT;
    }
}

uint32_t evdevToSceneButton(uint32_t evdev_button) {
    switch (evdev_button) {
        case BTN_MIDDLE: return 2;
        case BTN_RIGHT:  return 3;
        default:         return 1;
    }
}

// --- SpatialSeatDevice ---
// Same discipline as SpatialXdgWindow: the destroy signal fires from inside the
// backend's dispatch, so the handler only detaches and schedules.

void SpatialSeatDevice::beginDestruction() {
    detachListeners();
    keyboard = nullptr;
    pointer = nullptr;
    if (destroy_scheduled) return;
    destroy_scheduled = true;

    // Nulled AFTER removal, not before: removeInputDevice finds the entry by
    // this very pointer, so clearing it first left the dead device in the list
    // and its removal unlogged. The device is still alive for the duration of
    // its own destroy signal, which is the only thing the search needs.
    struct wlr_input_device* dying = device;
    if (compositor) {
        compositor->removeInputDevice(dying);
    }
    device = nullptr;
}

void SpatialSeatDevice::onKey(void* data) {
    auto event = static_cast<struct wlr_keyboard_key_event*>(data);
    if (!compositor || !event || !keyboard) return;
    compositor->notifySeatKey(*event, keyboard);
}

void SpatialSeatDevice::onModifiers(void*) {
    if (!compositor || !keyboard) return;
    compositor->notifySeatModifiers(keyboard);
}

void SpatialSeatDevice::onMotion(void* data) {
    auto event = static_cast<struct wlr_pointer_motion_event*>(data);
    if (!compositor || !event) return;
    compositor->processPointerMotionRelative(event->delta_x, event->delta_y);
}

void SpatialSeatDevice::onMotionAbsolute(void* data) {
    // Nested backends report 0..1 across the output rather than a delta; the
    // pointer position is the same state either way.
    auto event = static_cast<struct wlr_pointer_motion_absolute_event*>(data);
    if (!compositor || !event) return;
    const struct wlr_box& box = compositor->outputBox();
    compositor->processPointerMotionAbsolute(event->x * box.width, event->y * box.height);
}

void SpatialSeatDevice::onButton(void* data) {
    auto event = static_cast<struct wlr_pointer_button_event*>(data);
    if (!compositor || !event) return;
    compositor->processPointerButton(event->button,
                                     event->state == WL_POINTER_BUTTON_STATE_PRESSED);
}

void SpatialSeatDevice::onFrame(void*) {
    // The hardware's own group boundary. The scene path closes each group as it
    // produces it, so this is normally inert; it exists so a producer that ever
    // emits seat pointer events without terminating them is still correct.
    if (!compositor) return;
    compositor->notifySeatPointerFrame();
}

void SpatialSeatDevice::onDestroy(void*) {
    beginDestruction();
}

// --- SpatialOutput ---
// Same discipline again: the destroy signal fires from inside the backend's
// dispatch, so the handler withdraws the global and schedules.

void SpatialOutput::beginDestruction() {
    detachListeners();
    if (output) {
        // While the wlr_output is still alive, which is the only point at which
        // withdrawing its global is legal.
        wlr_output_destroy_global(output);
    }
    if (destroy_scheduled) return;
    destroy_scheduled = true;

    // Same ordering as SpatialSeatDevice: removeOutput finds the entry by this
    // pointer, and the wlr_output is alive for its own destroy signal.
    struct wlr_output* dying = output;
    if (compositor) {
        compositor->removeOutput(dying);
    }
    output = nullptr;
}

void SpatialOutput::onDestroy(void*) {
    beginDestruction();
}

// --- SpatialCompositor - device adoption ---

void SpatialCompositor::onNewInput(void* data) {
    auto device = static_cast<struct wlr_input_device*>(data);
    if (!device) return;

    // INVARIANT: a seat exists here. open() runs initProtocols() - which rebinds
    // new_input from onLatentInput to this handler - then initInput(), which
    // creates seat_, with no event-loop dispatch between them. Said out loud
    // rather than swallowed: if that order changes, the device dies silently.
    if (!seat_) {
        report(LOGGER::ERROR, "SpatialCompositor - Input device '%s' announced before the seat "
                              "exists; dropped. initProtocols() must not bind new_input first",
               device->name ? device->name : "unnamed");
        return;
    }

    report(LOGGER::INFO, "SpatialCompositor - New input device: %s (%s)",
           device->name ? device->name : "unnamed", describeDeviceType(device->type));

    auto seat_device = std::make_shared<SpatialSeatDevice>();
    seat_device->compositor = this;
    seat_device->device = device;
    seat_device->destroy_listener.bind(seat_device.get(), &SpatialSeatDevice::onDestroy,
                                       &device->events.destroy);

    if (device->type == WLR_INPUT_DEVICE_KEYBOARD) {
        attachKeyboardDevice(*seat_device);
    } else if (device->type == WLR_INPUT_DEVICE_POINTER) {
        attachPointerDevice(*seat_device);
    }

    // Unrouted device types are still tracked: the destroy listener has to come
    // off the signal at teardown whether or not the device ever fed anything.
    input_devices_.push_back(std::move(seat_device));
    refreshSeatCapabilities();
}

void SpatialCompositor::attachKeyboardDevice(SpatialSeatDevice& seat_device) {
    struct wlr_keyboard* keyboard = wlr_keyboard_from_input_device(seat_device.device);
    if (!keyboard) return;

    if (!xkb_keymap_) {
        report(LOGGER::ERROR, "SpatialCompositor - No XKB keymap; keyboard '%s' left unrouted",
               seat_device.device->name ? seat_device.device->name : "unnamed");
        return;
    }

    seat_device.keyboard = keyboard;
    wlr_keyboard_set_keymap(keyboard, xkb_keymap_);
    wlr_keyboard_set_repeat_info(keyboard, kRepeatRateHz, kRepeatDelayMs);

    seat_device.key_listener.bind(&seat_device, &SpatialSeatDevice::onKey, &keyboard->events.key);
    seat_device.modifiers_listener.bind(&seat_device, &SpatialSeatDevice::onModifiers,
                                        &keyboard->events.modifiers);

    // A real keyboard supersedes the hand-allocated fallback on the seat, but
    // does not free it: the headless path has no devices at all and still has
    // to hand clients a keymap.
    wlr_seat_set_keyboard(seat_, keyboard);
}

void SpatialCompositor::attachPointerDevice(SpatialSeatDevice& seat_device) {
    struct wlr_pointer* pointer = wlr_pointer_from_input_device(seat_device.device);
    if (!pointer) return;

    seat_device.pointer = pointer;
    seat_device.motion_listener.bind(&seat_device, &SpatialSeatDevice::onMotion,
                                     &pointer->events.motion);
    seat_device.motion_absolute_listener.bind(&seat_device, &SpatialSeatDevice::onMotionAbsolute,
                                              &pointer->events.motion_absolute);
    seat_device.button_listener.bind(&seat_device, &SpatialSeatDevice::onButton,
                                     &pointer->events.button);
    seat_device.frame_listener.bind(&seat_device, &SpatialSeatDevice::onFrame,
                                    &pointer->events.frame);
}

void SpatialCompositor::removeInputDevice(struct wlr_input_device* device) {
    if (!device) return;

    auto it = std::find_if(input_devices_.begin(), input_devices_.end(),
        [device](const std::shared_ptr<SpatialSeatDevice>& d) { return d && d->device == device; });
    if (it == input_devices_.end()) return;

    report(LOGGER::INFO, "SpatialCompositor - Input device removed: %s",
           device->name ? device->name : "unnamed");

    // Move, never erase-and-free: this runs inside the device's own destroy
    // dispatch and the entry owns the listener being dispatched.
    pending_destroy_devices_.push_back(std::move(*it));
    input_devices_.erase(it);
}

void SpatialCompositor::removeOutput(struct wlr_output* output) {
    if (!output) return;

    auto it = std::find_if(outputs_.begin(), outputs_.end(),
        [output](const std::shared_ptr<SpatialOutput>& o) { return o && o->output == output; });
    if (it == outputs_.end()) return;

    report(LOGGER::INFO, "SpatialCompositor - Output removed: %s",
           output->name ? output->name : "unnamed");

    // Move, never erase-and-free: this runs inside the output's own destroy
    // dispatch and the entry owns the listener being dispatched.
    pending_destroy_outputs_.push_back(std::move(*it));
    outputs_.erase(it);
    if (outputs_seen_ > 0) --outputs_seen_;
}

void SpatialCompositor::drainRemovedOutputs() {
    if (pending_destroy_outputs_.empty()) return;

    std::vector<std::shared_ptr<SpatialOutput>> doomed;
    doomed.swap(pending_destroy_outputs_);
    for (auto& output : doomed) {
        if (output) output->detachListeners();
    }
}

void SpatialCompositor::releaseOutputs() {
    for (auto& output : outputs_) {
        if (!output) continue;
        output->detachListeners();
        pending_destroy_outputs_.push_back(std::move(output));
    }
    outputs_.clear();
    drainRemovedOutputs();
}

void SpatialCompositor::drainRemovedInputDevices() {
    if (pending_destroy_devices_.empty()) return;

    std::vector<std::shared_ptr<SpatialSeatDevice>> doomed;
    doomed.swap(pending_destroy_devices_);
    for (auto& device : doomed) {
        if (device) device->detachListeners();
    }

    restoreSeatKeyboard();
    refreshSeatCapabilities();
}

void SpatialCompositor::restoreSeatKeyboard() {
    // The seat clears its own keyboard when that keyboard is destroyed, so by
    // the time this runs it may be pointing at nothing. Prefer another live
    // device; fall back to the virtual keyboard, which always has a keymap.
    if (!seat_ || wlr_seat_get_keyboard(seat_)) return;

    for (const auto& device : input_devices_) {
        if (device && device->keyboard) {
            wlr_seat_set_keyboard(seat_, device->keyboard);
            return;
        }
    }
    if (keyboard_) {
        wlr_seat_set_keyboard(seat_, keyboard_);
    }
}

void SpatialCompositor::refreshSeatCapabilities() {
    if (!seat_) return;

    // The pointer capability is not device-derived and should not pretend to
    // be: Vazio's pointer is the 3D reticle, which exists whether the position
    // comes from libinput, from the interim SDL window, or from a script. The
    // keyboard capability is device-derived, with the virtual keyboard standing
    // in when no physical one is attached.
    uint32_t capabilities = WL_SEAT_CAPABILITY_POINTER;
    if (keyboard_ || wlr_seat_get_keyboard(seat_) || hasAttachedKeyboard()) {
        capabilities |= WL_SEAT_CAPABILITY_KEYBOARD;
    }
    wlr_seat_set_capabilities(seat_, capabilities);
}

bool SpatialCompositor::hasAttachedKeyboard() const {
    return std::any_of(input_devices_.begin(), input_devices_.end(),
        [](const std::shared_ptr<SpatialSeatDevice>& d) { return d && d->keyboard; });
}

// --- SpatialCompositor - keyboard forwarding ---

void SpatialCompositor::notifySeatKey(const struct wlr_keyboard_key_event& event,
                                      struct wlr_keyboard* source) {
    if (!seat_ || !source) return;

    // The seat must be looking at the keyboard the event came from before the
    // key is forwarded: it is where the keymap and the modifier state that the
    // client will interpret this keycode against are read from.
    wlr_seat_set_keyboard(seat_, source);
    wlr_seat_keyboard_notify_key(seat_, event.time_msec, event.keycode, event.state);
}

void SpatialCompositor::notifySeatModifiers(struct wlr_keyboard* source) {
    if (!seat_ || !source) return;
    wlr_seat_set_keyboard(seat_, source);
    wlr_seat_keyboard_notify_modifiers(seat_, &source->modifiers);
}

void SpatialCompositor::notifySeatSurfaceKey(uint32_t evdev_keycode, bool pressed) {
    if (!seat_) return;

    // The seat needs a keyboard for the client to have been sent a keymap to
    // interpret this keycode against. It normally has one; restore it rather
    // than forwarding a key into a seat that would drop it silently.
    if (!wlr_seat_get_keyboard(seat_)) restoreSeatKeyboard();

    wlr_seat_keyboard_notify_key(seat_, monotonicTimeMs(), evdev_keycode,
                                 pressed ? WL_KEYBOARD_KEY_STATE_PRESSED
                                         : WL_KEYBOARD_KEY_STATE_RELEASED);
}

// --- SpatialCompositor - pointer event groups ---
// One frame per logical group, one group per scene input sample. See the
// grouping rule in SpatialCompositor.h.

void SpatialCompositor::closePointerGroup() {
    if (!seat_ || !pointer_group_open_) return;
    pointer_group_open_ = false;
    wlr_seat_pointer_notify_frame(seat_);
}

void SpatialCompositor::notifySeatPointerEnter(struct wlr_surface* surface, double sx, double sy) {
    if (!seat_ || !surface) return;
    wlr_seat_pointer_notify_enter(seat_, surface, sx, sy);
    pointer_group_open_ = true;
    closePointerGroup();
}

void SpatialCompositor::notifySeatPointerMotion(struct wlr_surface* surface, double sx, double sy) {
    if (!seat_ || !surface) return;

    // Enter is idempotent in wlroots when the surface already holds focus, so
    // this is the recovery path for a motion that arrives without a preceding
    // ray enter - a grab dragging back over its own surface, for instance.
    wlr_seat_pointer_notify_enter(seat_, surface, sx, sy);
    wlr_seat_pointer_notify_motion(seat_, monotonicTimeMs(), sx, sy);
    pointer_group_open_ = true;
    closePointerGroup();
}

void SpatialCompositor::notifySeatPointerButton(uint32_t evdev_button, bool pressed) {
    if (!seat_) return;
    wlr_seat_pointer_notify_button(seat_, monotonicTimeMs(), evdev_button,
                                   pressed ? WL_POINTER_BUTTON_STATE_PRESSED
                                           : WL_POINTER_BUTTON_STATE_RELEASED);
    pointer_group_open_ = true;
    closePointerGroup();
}

void SpatialCompositor::notifySeatPointerLeave() {
    if (!seat_) return;
    wlr_seat_pointer_notify_clear_focus(seat_);
    pointer_group_open_ = true;
    closePointerGroup();
}

void SpatialCompositor::notifySeatPointerFrame() {
    closePointerGroup();
}

// --- SpatialCompositor - selection and cursor policy ---

void SpatialCompositor::onRequestSetSelection(void* data) {
    auto event = static_cast<struct wlr_seat_request_set_selection_event*>(data);
    if (!seat_ || !event) return;

    // wlroots asks before it acts because only the compositor knows whether the
    // client may own the clipboard. The rule here is the one every desktop runs
    // on - the client that asked owns it - and from that point wlroots offers
    // the source to whichever client holds keyboard focus, which is why nothing
    // below has to know anything about clients or offers.
    wlr_seat_set_selection(seat_, event->source, event->serial);
}

void SpatialCompositor::onRequestSetPrimarySelection(void* data) {
    auto event = static_cast<struct wlr_seat_request_set_primary_selection_event*>(data);
    if (!seat_ || !event) return;
    wlr_seat_set_primary_selection(seat_, event->source, event->serial);
}

void SpatialCompositor::onRequestSetCursor(void* data) {
    auto event = static_cast<struct wlr_seat_pointer_request_set_cursor_event*>(data);
    if (!event) return;

    // Accepted and deliberately not drawn. The pointer in this session is the
    // 3D reticle the scene's raycast places; there is no 2D cursor plane to
    // composite a client's cursor image onto, and lifting one into world space
    // would be inventing an appearance nobody asked for. The cursor surface
    // itself is safe by construction: it carries the cursor role, never an xdg
    // one, so it is never hosted, its commits reach no listener this compositor
    // bound, and it cannot enter the toplevel path. Refusing instead would be
    // worse - the protocol offers no refusal, and a client whose set_cursor is
    // a protocol error is a client that dies on hover.
    report(LOGGER::DEBUG, "SpatialCompositor - Client set a cursor surface (%p); the reticle is the pointer",
           static_cast<const void*>(event->surface));
}

// --- SpatialCompositor - pointer position ---

void SpatialCompositor::processPointerMotionAbsolute(double x_px, double y_px) {
    const double max_x = std::max(0.0, static_cast<double>(output_box_.width) - 1.0);
    const double max_y = std::max(0.0, static_cast<double>(output_box_.height) - 1.0);
    pointer_x_ = std::clamp(x_px, 0.0, max_x);
    pointer_y_ = std::clamp(y_px, 0.0, max_y);

    if (!scene_) return;
    scene_->processPointerMotion(
        glm::vec2(static_cast<float>(pointer_x_), static_cast<float>(pointer_y_)),
        glm::vec2(static_cast<float>(output_box_.width), static_cast<float>(output_box_.height)));
}

void SpatialCompositor::processPointerMotionRelative(double dx_px, double dy_px) {
    // libinput only ever reports deltas, so the absolute position lives here
    // rather than in any one device. Clamping happens in the absolute path so
    // both producers are clamped by the same rule.
    processPointerMotionAbsolute(pointer_x_ + dx_px, pointer_y_ + dy_px);
}

void SpatialCompositor::processPointerButton(uint32_t evdev_button, bool pressed) {
    if (!scene_) return;

    // A press outside a grabbing popup takes the menu down before the click is
    // routed, which is the order xdg-shell describes: the input breaks the
    // grab, and the surface it landed on still receives it.
    if (pressed) dismissPopupsOutsidePointer();

    scene_->processPointerButton(evdevToSceneButton(evdev_button), pressed);
}

} // namespace Vazio
