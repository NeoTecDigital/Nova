// Written by Richard Christopher, Copyright 2026 NeoTec Digital
//
// Client half, seat phases plus the script driver. The shell phases live in
// protocol_client_shell.cpp.

#include "protocol_client.h"

#include <csignal>

#include <xkbcommon/xkbcommon.h>

namespace VazioTest {
namespace {

struct KeycaseExpectation {
    uint32_t evdev;
    xkb_keysym_t keysym;
    const char* label;
};

// The exact three cases QA3's keycode proof used. An evdev code becomes an XKB
// keycode by adding 8; a client that is handed an SDL scancode or an X11 keysym
// instead resolves every one of these to something else, which is the failure
// this phase exists to catch.
constexpr KeycaseExpectation kKeycases[] = {
    { kEvdevKeyA,         XKB_KEY_a,       "KEY_A -> 'a'" },
    { kEvdevKeyLeftShift, XKB_KEY_Shift_L, "KEY_LEFTSHIFT -> Shift_L" },
    { kEvdevKeyEnter,     XKB_KEY_Return,  "KEY_ENTER -> Return" },
};

int countKind(const std::vector<PointerEvent>& events, PointerEvent::Kind kind) {
    int total = 0;
    for (const PointerEvent& event : events) {
        if (event.kind == kind) ++total;
    }
    return total;
}

// Index of the first event of a kind, or -1.
int firstIndexOf(const std::vector<PointerEvent>& events, PointerEvent::Kind kind) {
    for (size_t i = 0; i < events.size(); ++i) {
        if (events[i].kind == kind) return static_cast<int>(i);
    }
    return -1;
}

bool keymapResolves(const std::string& keymap_text, CheckLog& log) {
    struct xkb_context* context = xkb_context_new(XKB_CONTEXT_NO_FLAGS);
    if (!context) {
        log.check(false, "keyboard: xkb_context_new failed");
        return false;
    }

    struct xkb_keymap* keymap = xkb_keymap_new_from_string(
        context, keymap_text.c_str(), XKB_KEYMAP_FORMAT_TEXT_V1, XKB_KEYMAP_COMPILE_NO_FLAGS);
    if (!keymap) {
        log.check(false, "keyboard: the compositor's keymap text did not compile");
        xkb_context_unref(context);
        return false;
    }

    struct xkb_state* state = xkb_state_new(keymap);
    bool all_ok = true;
    for (const KeycaseExpectation& expected : kKeycases) {
        const xkb_keysym_t actual = xkb_state_key_get_one_sym(state, expected.evdev + 8);
        char name[64] = {};
        xkb_keysym_get_name(actual, name, sizeof(name));
        const bool ok = actual == expected.keysym;
        all_ok = log.check(ok, "keyboard: %s through the compositor's own keymap (got '%s')",
                           expected.label, name) && all_ok;
    }

    xkb_state_unref(state);
    xkb_keymap_unref(keymap);
    xkb_context_unref(context);
    return all_ok;
}

}  // namespace

bool clientPointerArmPhase(ClientSurfaces& c) {
    // The server sweeps the output to find the pixel that hits this surface,
    // and that sweep generates thousands of enter/leave events of its own.
    // Roundtrip FIRST so every one of them has been dispatched, then clear:
    // clearing while some are still on the wire leaves them to be read into the
    // supposedly-empty log, and the grading below then reads a stale enter and a
    // stale leave instead of the driven sequence. That is a race, and it is how
    // this harness failed intermittently before the barrier was made explicit.
    roundtripClient(c.wl);
    c.wl.pointer_events.clear();
    c.pointer_log_base = 0;
    c.log->check(c.wl.pointer != nullptr, "pointer: seat advertised WL_SEAT_CAPABILITY_POINTER");
    return c.wl.pointer != nullptr;
}

namespace {

// Enter, drag, press, release, leave - the shape of one graded sequence.
void gradePointerSequence(const std::vector<PointerEvent>& events, CheckLog& log) {
    const int enters = countKind(events, PointerEvent::Kind::Enter);
    const int motions = countKind(events, PointerEvent::Kind::Motion);
    const int buttons = countKind(events, PointerEvent::Kind::Button);
    const int leaves = countKind(events, PointerEvent::Kind::Leave);

    log.check(enters >= 1, "pointer: wl_pointer.enter delivered (%d)", enters);
    log.check(leaves >= 1, "pointer: wl_pointer.leave delivered - hover is not sticky (%d)", leaves);
    log.check(motions >= 5, "pointer: the 5-sample drag arrived as motion events (%d)", motions);
    log.check(buttons >= 2, "pointer: press and release both delivered (%d button events)", buttons);

    const int enter_index = firstIndexOf(events, PointerEvent::Kind::Enter);
    const int button_index = firstIndexOf(events, PointerEvent::Kind::Button);
    log.check(enter_index >= 0 && button_index > enter_index,
              "pointer: the press landed AFTER the enter, on this surface (enter@%d press@%d)",
              enter_index, button_index);

    if (enter_index >= 0) {
        const PointerEvent& enter = events[static_cast<size_t>(enter_index)];
        const bool sane = enter.sx >= 0.0 && enter.sx <= kToplevelWidth &&
                          enter.sy >= 0.0 && enter.sy <= kToplevelHeight;
        log.check(sane, "pointer: enter carried surface-local coordinates inside the surface (%.2f, %.2f)",
                  enter.sx, enter.sy);
    }

    if (button_index >= 0) {
        const PointerEvent& press = events[static_cast<size_t>(button_index)];
        log.check(press.button == 0x110 /* BTN_LEFT */,
                  "pointer: the button arrived in evdev space (0x%x)", press.button);
        log.check(press.state == WL_POINTER_BUTTON_STATE_PRESSED,
                  "pointer: the first button event is the press");
    }
}

// The QA3 F1 regression: one wl_pointer.frame per scene-level input sample.
bool gradeFrameGrouping(const std::vector<PointerEvent>& events, CheckLog& log) {
    const FrameGrouping grouping = groupPointerEvents(events);
    log.note("pointer: frames=%d groups=%d empty=%d biggest_group=%d",
             grouping.frames, grouping.groups, grouping.empty_frames, grouping.biggest_group);

    log.check(grouping.frames > 0, "pointer: wl_pointer.frame is sent at all (%d)", grouping.frames);
    log.check(grouping.groups >= 3,
              "pointer: enter, drag and button each closed their own group (%d non-empty groups)",
              grouping.groups);

    // The failure mode this exists for: without per-sample frames every event
    // batches behind wlroots' own focus-change frame, and the client sees one
    // enormous group instead of a stream of samples.
    log.check(grouping.biggest_group <= 3,
              "pointer: no group batched more than 3 events (biggest=%d) - events are not queued "
              "behind a single frame", grouping.biggest_group);

    const bool terminated = !events.empty() && events.back().kind == PointerEvent::Kind::Frame;
    log.check(terminated, "pointer: the log ends on a frame - no group left open");
    return grouping.frames > 0;
}

}  // namespace

bool clientPointerGradePhase(ClientSurfaces& c) {
    CheckLog& log = *c.log;

    // The whole driven sequence was queued before the server sent this phase's
    // token, so one roundtrip dispatches all of it and nothing after it. Waiting
    // for a particular event instead would stop at the first match and grade a
    // partial log.
    roundtripClient(c.wl);

    const std::vector<PointerEvent>& events = c.wl.pointer_events;
    gradePointerSequence(events, log);
    const bool framed = gradeFrameGrouping(events, log);

    return countKind(events, PointerEvent::Kind::Enter) >= 1 &&
           countKind(events, PointerEvent::Kind::Leave) >= 1 && framed;
}

bool clientKeyboardPhase(ClientSurfaces& c) {
    CheckLog& log = *c.log;

    const size_t expected_events = sizeof(kKeycases) / sizeof(kKeycases[0]) * 2;
    roundtripClient(c.wl);

    log.check(c.wl.keyboard != nullptr, "keyboard: seat advertised WL_SEAT_CAPABILITY_KEYBOARD");
    log.check(c.wl.keymap_format == WL_KEYBOARD_KEYMAP_FORMAT_XKB_V1,
              "keyboard: keymap arrived in XKB_V1 format (%u)", c.wl.keymap_format);
    log.check(!c.wl.keymap_text.empty(), "keyboard: keymap text is non-empty (%zu bytes)",
              c.wl.keymap_text.size());

    log.check(c.wl.key_events.size() >= expected_events,
              "keyboard: %zu key events delivered (expected >= %zu: press+release per case)",
              c.wl.key_events.size(), expected_events);

    // Keycodes must arrive exactly as sent. This is the assertion that fails if
    // anything on the path re-maps into SDL scancode or X11 keysym space.
    size_t matched = 0;
    for (size_t i = 0; i < sizeof(kKeycases) / sizeof(kKeycases[0]); ++i) {
        const uint32_t wanted = kKeycases[i].evdev;
        bool pressed_seen = false;
        bool released_seen = false;
        for (const KeyEvent& event : c.wl.key_events) {
            if (event.keycode != wanted) continue;
            if (event.state == WL_KEYBOARD_KEY_STATE_PRESSED) pressed_seen = true;
            if (event.state == WL_KEYBOARD_KEY_STATE_RELEASED) released_seen = true;
        }
        if (log.check(pressed_seen && released_seen,
                      "keyboard: evdev code %u arrived unmodified, press and release (%s)",
                      wanted, kKeycases[i].label)) {
            ++matched;
        }
    }

    const bool keymap_ok = c.wl.keymap_text.empty() ? false : keymapResolves(c.wl.keymap_text, log);
    return matched == sizeof(kKeycases) / sizeof(kKeycases[0]) && keymap_ok;
}

// --- the script --------------------------------------------------------------

namespace {

struct ScriptStep {
    char expect;
    char reply;
    bool (*phase)(ClientSurfaces&);
    const char* name;
};

// No phase to run: the barrier itself is the whole step.
bool noPhase(ClientSurfaces&) { return true; }

constexpr ScriptStep kScript[] = {
    { Phase::kGoToplevel,     Phase::kToplevelMapped,   clientToplevelPhase,       "toplevel"    },
    { Phase::kGoPointerArm,   Phase::kPointerArmed,     clientPointerArmPhase,     "pointer-arm" },
    { Phase::kGoPointerGrade, Phase::kPointerDone,      clientPointerGradePhase,   "pointer"     },
    { Phase::kGoKeyboard,     Phase::kKeyboardDone,     clientKeyboardPhase,       "keyboard"    },
    { Phase::kGoPopup,        Phase::kPopupMapped,      clientPopupPhase,          "popup"       },
    { Phase::kGoPopupDismiss, Phase::kPopupDismissed,   clientPopupDismissPhase,   "popup-grab"  },
    { Phase::kGoSubsurface,   Phase::kSubsurfaceDone,   clientSubsurfacePhase,     "subsurface"  },
    { Phase::kGoDecoration,   Phase::kDecorationDone,   clientDecorationPhase,     "decoration"  },
    { Phase::kGoSelection,    Phase::kSelectionDone,    clientSelectionPhase,      "selection"   },
    { Phase::kFinish,         '\0',                     noPhase,                   "finish"      },
};

}  // namespace

namespace {

// What the registry must have offered before any phase can mean anything.
void gradeRegistry(const ClientSurfaces& c, CheckLog& log) {
    const RegistryGlobals& g = c.wl.globals;
    log.check(true, "bind: %zu globals advertised", g.names.size());
    log.check(g.saw_output, "bind: wl_output global exists (v%u, %d output(s)) - toolkits "
              "enumerate monitors from it", g.output_version, g.output_count);
    log.check(g.seat_version >= 5,
              "bind: wl_seat advertised at v%u (>= 5, so wl_pointer.frame is defined)",
              g.seat_version);
    log.check(g.wm_base_version >= 2, "bind: xdg_wm_base advertised at v%u", g.wm_base_version);
    log.check(c.wl.pointer != nullptr && c.wl.keyboard != nullptr,
              "bind: seat capabilities yielded both a pointer and a keyboard");
}

// Walk the token script. False means a barrier did not arrive, which is fatal:
// carrying on would deadlock against a server that is waiting for a reply.
bool runScript(ClientSurfaces& c, PhaseChannel& channel, CheckLog& log) {
    auto pump = [&] { pumpClient(c.wl); };

    for (const ScriptStep& step : kScript) {
        const char token = channel.await(pump, 30000);
        if (token != step.expect) {
            log.check(false, "script: expected '%c' before the %s phase, got '%c'",
                      step.expect, step.name, token ? token : '0');
            channel.send(Phase::kAborted);
            return false;
        }
        if (step.reply == '\0') break;

        step.phase(c);
        channel.send(step.reply);
    }
    return true;
}

}  // namespace

int runProtocolClient(const char* socket_name, PhaseChannel& channel) {
    // A half-closed phase pipe must surface as a failed send, not as a signal:
    // the parent closes its end as soon as the script ends.
    signal(SIGPIPE, SIG_IGN);

    CheckLog log("client");
    ClientSurfaces c;
    c.channel = &channel;
    c.log = &log;

    if (!connectClient(c.wl, socket_name)) {
        log.check(false, "bind: could not connect and bind every required global on '%s'",
                  socket_name);
        channel.send(Phase::kAborted);
        return log.report();
    }

    gradeRegistry(c, log);
    channel.send(Phase::kClientBound);
    runScript(c, channel, log);

    // Flush anything the last phase queued before the socket goes away.
    for (int i = 0; i < 5; ++i) pumpClient(c.wl);

    // Buffers BEFORE the display. Every ShmBuffer owns a wl_buffer proxy, and a
    // proxy outliving wl_display_disconnect is a use-after-free in libwayland -
    // which shows up as the client dying on a signal after reporting every
    // assertion green, i.e. as the most confusing possible pass.
    c.toplevel_buffer.reset();
    c.toplevel_buffer_next.reset();
    c.popup_buffer.reset();
    c.sub_buffer.reset();
    c.second_buffer.reset();

    disconnectClient(c.wl);
    return log.report();
}

}  // namespace VazioTest
