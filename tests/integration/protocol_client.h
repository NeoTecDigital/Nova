// Written by Richard Christopher, Copyright 2026 NeoTec Digital
#pragma once

#include "harness_check.h"
#include "phase_channel.h"
#include "wl_client_kit.h"

#include <memory>

namespace VazioTest {

/**
 * The token script the two halves of the protocol harness run.
 *
 * One linear sequence, one token per barrier, uppercase from the client and
 * digits/lowercase from the server. Written out as named constants rather than
 * inline literals because a mis-typed barrier is a deadlock, and a deadlock in a
 * forked test is the least debuggable failure there is.
 */
namespace Phase {
constexpr char kClientBound      = 'R';   // C: registry bound, globals complete
constexpr char kGoToplevel       = '1';   // S: create, configure, map a toplevel
constexpr char kToplevelMapped   = 'C';   // C: mapped, buffer released, checksum held
constexpr char kGoPointerArm     = '2';   // S: clear the pointer log, I am about to drive
constexpr char kPointerArmed     = 'a';   // C: log cleared
constexpr char kGoPointerGrade   = '3';   // S: sequence driven, grade it
constexpr char kPointerDone      = 'P';
constexpr char kGoKeyboard       = '4';   // S: keys driven, grade them
constexpr char kKeyboardDone     = 'K';
constexpr char kGoPopup          = '5';   // C: create a grabbing popup
constexpr char kPopupMapped      = 'U';
constexpr char kGoPopupDismiss   = '6';   // S: clicked outside the grab, grade it
constexpr char kPopupDismissed   = 'u';
constexpr char kGoSubsurface     = '7';
constexpr char kSubsurfaceDone   = 'B';
constexpr char kGoDecoration     = '8';
constexpr char kDecorationDone   = 'D';
constexpr char kGoSelection      = '9';
constexpr char kSelectionDone    = 'L';
constexpr char kFinish           = 'X';
constexpr char kAborted          = '!';   // either side: give up, do not deadlock
}  // namespace Phase

// Geometry the two halves agree on. The server derives the expected surface-host
// extent from these, so a change here is a change to both assertions at once.
constexpr int32_t kToplevelWidth  = 160;
constexpr int32_t kToplevelHeight = 100;
constexpr int32_t kPopupWidth     = 40;
constexpr int32_t kPopupHeight    = 30;
constexpr int32_t kSubsurfaceSize = 32;
constexpr int32_t kSecondToplevelWidth  = 96;
constexpr int32_t kSecondToplevelHeight = 64;

// The three keycode cases QA3 proved the compositor used to get wrong. Sent as
// raw evdev codes, which is the only space a wl_keyboard client understands.
constexpr uint32_t kEvdevKeyA         = 30;
constexpr uint32_t kEvdevKeyLeftShift = 42;
constexpr uint32_t kEvdevKeyEnter     = 28;

/**
 * Everything the client half builds, in one object.
 *
 * Buffers are held by pointer because ShmBuffer owns an mmap and a wl_buffer and
 * is deliberately neither copyable nor movable - a buffer that can be moved is a
 * buffer whose wl_buffer listener user-data can go stale.
 */
struct ClientSurfaces {
    ClientState wl;
    PhaseChannel* channel = nullptr;
    CheckLog* log = nullptr;

    struct wl_surface* toplevel_surface = nullptr;
    struct xdg_surface* toplevel_xdg = nullptr;
    struct xdg_toplevel* toplevel = nullptr;
    // TWO buffers, because wlroots holds a committed buffer until the surface
    // commits the next one: release is observable only on a client that double
    // buffers, which is what every real toolkit does.
    std::unique_ptr<ShmBuffer> toplevel_buffer;
    std::unique_ptr<ShmBuffer> toplevel_buffer_next;
    uint32_t toplevel_checksum = 0;

    struct wl_surface* popup_surface = nullptr;
    struct xdg_surface* popup_xdg = nullptr;
    struct xdg_popup* popup = nullptr;
    struct xdg_positioner* positioner = nullptr;
    std::unique_ptr<ShmBuffer> popup_buffer;

    struct wl_surface* sub_surface = nullptr;
    struct wl_subsurface* subsurface = nullptr;
    std::unique_ptr<ShmBuffer> sub_buffer;

    struct wl_surface* second_surface = nullptr;
    struct xdg_surface* second_xdg = nullptr;
    struct xdg_toplevel* second_toplevel = nullptr;
    struct zxdg_toplevel_decoration_v1* decoration = nullptr;
    std::unique_ptr<ShmBuffer> second_buffer;

    struct wl_data_device* data_device = nullptr;
    struct wl_data_source* data_source = nullptr;
    struct zwp_primary_selection_device_v1* primary_device = nullptr;
    struct zwp_primary_selection_source_v1* primary_source = nullptr;

    // Pointer-log high-water marks, captured at the arm barrier so the graded
    // sequence is measured against a known-empty log.
    size_t pointer_log_base = 0;
};

/**
 * The forked client half. Runs the whole script and returns a process exit code
 * (0 = every assertion held). Never returns to the caller's main().
 */
int runProtocolClient(const char* socket_name, PhaseChannel& channel);

// --- the two halves of the client script, split so neither file carries both --

// Shell: toplevel, buffer import, popup, subsurface, decoration, selection.
bool clientToplevelPhase(ClientSurfaces& c);
bool clientPopupPhase(ClientSurfaces& c);
bool clientPopupDismissPhase(ClientSurfaces& c);
bool clientSubsurfacePhase(ClientSurfaces& c);
bool clientDecorationPhase(ClientSurfaces& c);
bool clientSelectionPhase(ClientSurfaces& c);

// Seat: pointer grouping and keyboard keycode space.
bool clientPointerArmPhase(ClientSurfaces& c);
bool clientPointerGradePhase(ClientSurfaces& c);
bool clientKeyboardPhase(ClientSurfaces& c);

}  // namespace VazioTest
