// Written by Richard Christopher, Copyright 2026 NeoTec Digital
//
// SCAFFOLDING - DIES AT PHASE 3b.
//
// Vazio's boot target is autocreate: DRM plus libinput on a TTY, where every
// key already arrives as a raw evdev code and nothing here is reachable. This
// file exists only for the interim SDL window, which is the one producer that
// speaks a different keycode space. When the SDL half is deleted at Phase 3b
// this header and its single call site go with it; nothing else includes it,
// and nothing in src/ or include/ ever will.
//
// WHY IT HAS TO EXIST AT ALL. A wl_keyboard.key event carries a raw evdev
// keycode - that is the only currency a client can interpret, because the
// keymap the compositor handed it is indexed by evdev+8. SDL reports two other
// things: a keysym (X11-flavoured, layout-dependent) and a scancode (a USB HID
// usage ID, layout-independent). Neither is evdev. Sending a keysym delivers
// 'Control_R' for 'a' and 'equal' for Enter; sending a scancode delivers '3'
// for 'a' and 'XF86MonBrightnessUp' for Shift. Only the physical-key space
// translates, so the scancode is the input and evdev is the output.
//
// ONE TRANSLATION, AT THE BOUNDARY. Both consumers downstream - the fallback
// wlr_keyboard that produces modifier state, and the scene route that reaches
// the focused surface - receive the evdev code this produces. Neither of them
// knows SDL exists.
#pragma once

#include <SDL2/SDL_scancode.h>
#include <linux/input-event-codes.h>

#include <algorithm>
#include <cstdint>
#include <iterator>

namespace CloudsInterim {

struct ScancodeMapping {
    uint16_t sdl_scancode;
    uint16_t evdev_keycode;
};

// Sorted by sdl_scancode, which is also the order SDL assigns them, so the
// table reads as the HID usage page it is. Binary searched below: O(log n) and
// no runtime construction, against a switch's O(1) but 200 lines of control
// flow. Coverage is the common set a keyboard-driven client needs - letters,
// digits, the printing punctuation, F1-F24, the editing and navigation cluster,
// the keypad and every modifier. Anything outside it is dropped rather than
// guessed at, because a wrong keycode is worse than a missing one.
inline constexpr ScancodeMapping kSdlScancodeToEvdev[] = {
    // Letters. Note evdev orders these by QWERTY position, not alphabetically.
    { SDL_SCANCODE_A, KEY_A }, { SDL_SCANCODE_B, KEY_B }, { SDL_SCANCODE_C, KEY_C },
    { SDL_SCANCODE_D, KEY_D }, { SDL_SCANCODE_E, KEY_E }, { SDL_SCANCODE_F, KEY_F },
    { SDL_SCANCODE_G, KEY_G }, { SDL_SCANCODE_H, KEY_H }, { SDL_SCANCODE_I, KEY_I },
    { SDL_SCANCODE_J, KEY_J }, { SDL_SCANCODE_K, KEY_K }, { SDL_SCANCODE_L, KEY_L },
    { SDL_SCANCODE_M, KEY_M }, { SDL_SCANCODE_N, KEY_N }, { SDL_SCANCODE_O, KEY_O },
    { SDL_SCANCODE_P, KEY_P }, { SDL_SCANCODE_Q, KEY_Q }, { SDL_SCANCODE_R, KEY_R },
    { SDL_SCANCODE_S, KEY_S }, { SDL_SCANCODE_T, KEY_T }, { SDL_SCANCODE_U, KEY_U },
    { SDL_SCANCODE_V, KEY_V }, { SDL_SCANCODE_W, KEY_W }, { SDL_SCANCODE_X, KEY_X },
    { SDL_SCANCODE_Y, KEY_Y }, { SDL_SCANCODE_Z, KEY_Z },

    // Digit row. SDL runs 1..9 then 0; evdev does the same.
    { SDL_SCANCODE_1, KEY_1 }, { SDL_SCANCODE_2, KEY_2 }, { SDL_SCANCODE_3, KEY_3 },
    { SDL_SCANCODE_4, KEY_4 }, { SDL_SCANCODE_5, KEY_5 }, { SDL_SCANCODE_6, KEY_6 },
    { SDL_SCANCODE_7, KEY_7 }, { SDL_SCANCODE_8, KEY_8 }, { SDL_SCANCODE_9, KEY_9 },
    { SDL_SCANCODE_0, KEY_0 },

    // Whitespace and editing.
    { SDL_SCANCODE_RETURN, KEY_ENTER },       { SDL_SCANCODE_ESCAPE, KEY_ESC },
    { SDL_SCANCODE_BACKSPACE, KEY_BACKSPACE },{ SDL_SCANCODE_TAB, KEY_TAB },
    { SDL_SCANCODE_SPACE, KEY_SPACE },

    // Printing punctuation.
    { SDL_SCANCODE_MINUS, KEY_MINUS },              { SDL_SCANCODE_EQUALS, KEY_EQUAL },
    { SDL_SCANCODE_LEFTBRACKET, KEY_LEFTBRACE },    { SDL_SCANCODE_RIGHTBRACKET, KEY_RIGHTBRACE },
    { SDL_SCANCODE_BACKSLASH, KEY_BACKSLASH },      { SDL_SCANCODE_NONUSHASH, KEY_BACKSLASH },
    { SDL_SCANCODE_SEMICOLON, KEY_SEMICOLON },      { SDL_SCANCODE_APOSTROPHE, KEY_APOSTROPHE },
    { SDL_SCANCODE_GRAVE, KEY_GRAVE },              { SDL_SCANCODE_COMMA, KEY_COMMA },
    { SDL_SCANCODE_PERIOD, KEY_DOT },               { SDL_SCANCODE_SLASH, KEY_SLASH },
    { SDL_SCANCODE_CAPSLOCK, KEY_CAPSLOCK },

    // Function row. evdev breaks its own run at F11.
    { SDL_SCANCODE_F1, KEY_F1 },   { SDL_SCANCODE_F2, KEY_F2 },   { SDL_SCANCODE_F3, KEY_F3 },
    { SDL_SCANCODE_F4, KEY_F4 },   { SDL_SCANCODE_F5, KEY_F5 },   { SDL_SCANCODE_F6, KEY_F6 },
    { SDL_SCANCODE_F7, KEY_F7 },   { SDL_SCANCODE_F8, KEY_F8 },   { SDL_SCANCODE_F9, KEY_F9 },
    { SDL_SCANCODE_F10, KEY_F10 }, { SDL_SCANCODE_F11, KEY_F11 }, { SDL_SCANCODE_F12, KEY_F12 },

    // Editing and navigation cluster.
    { SDL_SCANCODE_PRINTSCREEN, KEY_SYSRQ },   { SDL_SCANCODE_SCROLLLOCK, KEY_SCROLLLOCK },
    { SDL_SCANCODE_PAUSE, KEY_PAUSE },         { SDL_SCANCODE_INSERT, KEY_INSERT },
    { SDL_SCANCODE_HOME, KEY_HOME },           { SDL_SCANCODE_PAGEUP, KEY_PAGEUP },
    { SDL_SCANCODE_DELETE, KEY_DELETE },       { SDL_SCANCODE_END, KEY_END },
    { SDL_SCANCODE_PAGEDOWN, KEY_PAGEDOWN },
    { SDL_SCANCODE_RIGHT, KEY_RIGHT },         { SDL_SCANCODE_LEFT, KEY_LEFT },
    { SDL_SCANCODE_DOWN, KEY_DOWN },           { SDL_SCANCODE_UP, KEY_UP },

    // Keypad.
    { SDL_SCANCODE_NUMLOCKCLEAR, KEY_NUMLOCK }, { SDL_SCANCODE_KP_DIVIDE, KEY_KPSLASH },
    { SDL_SCANCODE_KP_MULTIPLY, KEY_KPASTERISK },{ SDL_SCANCODE_KP_MINUS, KEY_KPMINUS },
    { SDL_SCANCODE_KP_PLUS, KEY_KPPLUS },       { SDL_SCANCODE_KP_ENTER, KEY_KPENTER },
    { SDL_SCANCODE_KP_1, KEY_KP1 }, { SDL_SCANCODE_KP_2, KEY_KP2 }, { SDL_SCANCODE_KP_3, KEY_KP3 },
    { SDL_SCANCODE_KP_4, KEY_KP4 }, { SDL_SCANCODE_KP_5, KEY_KP5 }, { SDL_SCANCODE_KP_6, KEY_KP6 },
    { SDL_SCANCODE_KP_7, KEY_KP7 }, { SDL_SCANCODE_KP_8, KEY_KP8 }, { SDL_SCANCODE_KP_9, KEY_KP9 },
    { SDL_SCANCODE_KP_0, KEY_KP0 }, { SDL_SCANCODE_KP_PERIOD, KEY_KPDOT },

    { SDL_SCANCODE_NONUSBACKSLASH, KEY_102ND }, { SDL_SCANCODE_APPLICATION, KEY_COMPOSE },

    // Extended function row.
    { SDL_SCANCODE_F13, KEY_F13 }, { SDL_SCANCODE_F14, KEY_F14 }, { SDL_SCANCODE_F15, KEY_F15 },
    { SDL_SCANCODE_F16, KEY_F16 }, { SDL_SCANCODE_F17, KEY_F17 }, { SDL_SCANCODE_F18, KEY_F18 },
    { SDL_SCANCODE_F19, KEY_F19 }, { SDL_SCANCODE_F20, KEY_F20 }, { SDL_SCANCODE_F21, KEY_F21 },
    { SDL_SCANCODE_F22, KEY_F22 }, { SDL_SCANCODE_F23, KEY_F23 }, { SDL_SCANCODE_F24, KEY_F24 },

    // Modifiers. Without these a client sees an unshifted, unctrl'd keyboard.
    { SDL_SCANCODE_LCTRL, KEY_LEFTCTRL },   { SDL_SCANCODE_LSHIFT, KEY_LEFTSHIFT },
    { SDL_SCANCODE_LALT, KEY_LEFTALT },     { SDL_SCANCODE_LGUI, KEY_LEFTMETA },
    { SDL_SCANCODE_RCTRL, KEY_RIGHTCTRL },  { SDL_SCANCODE_RSHIFT, KEY_RIGHTSHIFT },
    { SDL_SCANCODE_RALT, KEY_RIGHTALT },    { SDL_SCANCODE_RGUI, KEY_RIGHTMETA },
};

// Sortedness is the binary search's precondition, and a table this long is
// edited by hand. Prove it at compile time rather than trusting the ordering
// to survive the next insertion.
constexpr bool scancodeTableIsSorted() {
    for (std::size_t i = 1; i < std::size(kSdlScancodeToEvdev); ++i) {
        if (kSdlScancodeToEvdev[i - 1].sdl_scancode >= kSdlScancodeToEvdev[i].sdl_scancode) {
            return false;
        }
    }
    return true;
}
static_assert(scancodeTableIsSorted(), "kSdlScancodeToEvdev must be sorted by sdl_scancode");

// Raw evdev keycode for an SDL scancode, or 0 when the key has no mapping.
// Zero is not a valid evdev keycode (KEY_RESERVED), so it is an unambiguous
// "drop this" and never a key a client could act on.
inline uint32_t sdlScancodeToEvdev(uint32_t sdl_scancode) {
    const auto* end = std::end(kSdlScancodeToEvdev);
    const auto* found = std::lower_bound(
        std::begin(kSdlScancodeToEvdev), end, sdl_scancode,
        [](const ScancodeMapping& entry, uint32_t key) { return entry.sdl_scancode < key; });

    if (found == end || found->sdl_scancode != sdl_scancode) return 0;
    return found->evdev_keycode;
}

} // namespace CloudsInterim
