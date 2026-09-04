// Written by Richard Christopher, Copyright 2026 NeoTec Digital
#pragma once

#include <glm/glm.hpp>
#include <string>

namespace Clouds::UI {

struct UITheme {
    // -------------------------------------------------------------
    // Surfaces & Backgrounds (Modern Nordic Obsidian Dark Theme)
    // -------------------------------------------------------------
    glm::vec4 bg_dark{0.05f, 0.07f, 0.10f, 0.95f};
    glm::vec4 surface_base{0.09f, 0.11f, 0.16f, 0.92f};
    glm::vec4 surface_elevated{0.12f, 0.15f, 0.22f, 0.95f};
    glm::vec4 surface_card{0.10f, 0.13f, 0.19f, 0.90f};
    
    // Window specific
    glm::vec4 window_bg{0.08f, 0.10f, 0.15f, 0.94f};
    glm::vec4 window_titlebar_active{0.14f, 0.18f, 0.26f, 0.98f};
    glm::vec4 window_titlebar_inactive{0.10f, 0.12f, 0.18f, 0.92f};
    glm::vec4 window_border_active{0.25f, 0.45f, 0.80f, 0.90f};
    glm::vec4 window_border_inactive{0.18f, 0.22f, 0.32f, 0.60f};

    // -------------------------------------------------------------
    // Accents & Interactive States
    // -------------------------------------------------------------
    glm::vec4 primary{0.20f, 0.45f, 0.85f, 0.95f};
    glm::vec4 primary_hover{0.28f, 0.55f, 0.95f, 1.00f};
    glm::vec4 primary_active{0.15f, 0.35f, 0.70f, 1.00f};

    glm::vec4 secondary{0.16f, 0.20f, 0.28f, 0.90f};
    glm::vec4 secondary_hover{0.22f, 0.28f, 0.40f, 1.00f};

    glm::vec4 accent_cyan{0.15f, 0.75f, 0.90f, 0.95f};
    glm::vec4 accent_success{0.18f, 0.72f, 0.42f, 0.95f};
    glm::vec4 accent_warning{0.92f, 0.60f, 0.15f, 0.95f};
    glm::vec4 accent_danger{0.88f, 0.25f, 0.32f, 0.95f};
    glm::vec4 accent_purple{0.65f, 0.35f, 0.85f, 0.95f};

    // -------------------------------------------------------------
    // Typography Colors
    // -------------------------------------------------------------
    glm::vec4 text_primary{0.94f, 0.96f, 1.00f, 1.00f};
    glm::vec4 text_secondary{0.68f, 0.75f, 0.88f, 0.90f};
    glm::vec4 text_muted{0.45f, 0.52f, 0.65f, 0.75f};
    glm::vec4 text_highlight{0.35f, 0.75f, 1.00f, 1.00f};

    // -------------------------------------------------------------
    // Typography Scales (Local Spatial Units)
    // -------------------------------------------------------------
    float scale_title = 0.00065f;
    float scale_header = 0.00055f;
    float scale_body = 0.00045f;
    float scale_small = 0.00038f;
    float scale_mono = 0.00035f;

    // -------------------------------------------------------------
    // Geometry & Radii
    // -------------------------------------------------------------
    float radius_window = 0.020f;
    float radius_panel = 0.012f;
    float radius_button = 0.008f;
    float radius_pill = 0.016f;

    float border_window = 0.0025f;
    float border_panel = 0.0015f;
    float border_button = 0.0012f;

    // Titlebar metrics
    float titlebar_height = 0.065f;
    float menubar_height = 0.055f;
    float dockbar_height = 0.065f;
};

// --- The theme seam ---------------------------------------------------------
//
// There is no global theme and no default theme. A UITheme is owned by whoever
// composes a UI and is handed to every widget it builds, by const reference, at
// construction; the widget keeps a NON-OWNING pointer to it and reads it when
// it resolves a colour or a material, never copying a field out. That is what
// makes an edit to a live theme -- a YAML reload, a per-Desktop swap -- reach
// the widgets that already exist. The single mutable `inline UITheme` global
// that used to live at the foot of this header was read in five in-class
// initialisers, so every widget snapshotted the theme at construction and a
// live edit moved nothing that was already on screen.
//
// Why a reference parameter and not a pointer with a fallback: a reference has
// no null, so "this widget was never given a theme" is a compile error rather
// than a runtime branch, and there is no static default instance for a theme to
// silently drift back to. A fallback theme is a mutable global wearing a hat.
//
// Why non-owning and not shared_ptr<const UITheme>: the owner mutates the theme
// in place and every widget reads through it, so one write updates the whole
// tree with no traversal. Shared ownership of a *const* theme would make a
// hot-swap mean re-pointing every widget -- the same per-node walk this change
// exists to delete, and torn if it were ever partial. Lifetime is safe by
// construction: whoever owns the scene owns the theme, and the widgets are in
// the scene.
//
// Where this goes next: Phase 4 (B3) puts the theme on the Desktop. The Desktop
// will hand its own theme in through this same construction parameter, and no
// widget will change. This is that seam.
//
// What resolves when: MATERIAL (colours, radii, border thickness) resolves per
// frame from the live theme. GEOMETRY (titlebar height, bar heights, the
// text-measured widths a flow layout packs against) is resolved once at
// construction, because changing it means re-running a layout, which is the
// anchored-Layout work in Phase 1 and not this seam's job.

} // namespace Clouds::UI
