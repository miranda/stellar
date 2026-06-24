// stellar_settings_state.h
// Shared declarations for the split-up Stellar settings app.
//
// settings.c   - owns global state (defines the variables below), load/save,
//                IPC, font-preview lifecycle, main() + event loop.
// views.c      - all nuklear UI: process_frame(), per-view renderers, the
//                generalized per-screen target selector + override helpers.
// xorg_gen.c   - display/topology logic: screen grid, xorg.conf generation,
//                staging/validation, apply/remove/restore. No nuklear.
//
// This header is the contract between those translation units. The structs that
// were previously file-static in the monolith live here so every unit agrees on
// their layout; the globals are declared `extern` here and defined exactly once
// in settings.c.

#ifndef STELLAR_SETTINGS_STATE_H
#define STELLAR_SETTINGS_STATE_H

#include <stdbool.h>
#include <limits.h>

#include "stellar_config.h"
#include "stellar_hw.h"
#include "stellar_theme.h"
#include "stellar_font.h"

// nuklear is only needed by views.c; forward-declare so this header stays
// includable by the nuklear-free xorg_gen.c.
struct nk_context;

/* ===================================================================
 * UI-only state (never written to settings.json)
 * =================================================================== */

typedef struct {
    int is_active;
    char scale_buf[16];

    // Per-screen appearance buffers
    char font_name_buf[128];
    float vector_font_size;
    float bitmap_font_size;
    char font_unit[8];
    bool font_prefer_bitmap;
} ScreenUIState;

typedef struct {
    char font_name_buf[128];   // fontconfig family name (editable)
    float vector_font_size;    // Remembers the pt size for vector fonts
    float bitmap_font_size;
    char font_unit[8];
    bool font_prefer_bitmap;
    ScreenUIState screens[MAX_SCREENS];
} UIState;

/* ===================================================================
 * View + xorg-change enums
 * =================================================================== */

// The sidebar tabs. The per-screen pointer views (APPEARANCE, FONT, WALLPAPER)
// share the target selector; SYSTEM, DISPLAY and RULES do not.
typedef enum {
    VIEW_APPEARANCE,   // cursor theme + size (per-screen)
    VIEW_FONT,         // system font (per-screen)
    VIEW_WALLPAPER,    // wallpaper image (per-screen)
    VIEW_SYSTEM,       // stellar theme + terminal + shell (global/session)
    VIEW_DISPLAY,      // all-screens topology grid
    VIEW_RULES         // window rules (not per-screen)
} SettingsView;

// Classification of how a freshly-generated xorg.conf differs from the one
// currently installed. Drives the three save/exit behaviors:
//   NONE       -> nothing to write, close quietly
//   LIVE       -> rotation / preferred-mode / geometry only; write file on
//                 Save & Apply (so LightDM etc. are correct next boot) and apply
//                 live via xrandr, but DO NOT prompt for a restart
//   STRUCTURAL -> screen/device set changed (new monitor, probe, topology);
//                 write file and prompt for a restart
typedef enum {
    XORG_CHANGE_NONE = 0,
    XORG_CHANGE_LIVE,
    XORG_CHANGE_STRUCTURAL
} XorgChangeClass;

/* ===================================================================
 * Monitor info received from the DE over IPC (GET_SCREEN_INFO)
 * =================================================================== */

typedef struct {
    char monitor_name[64];
    char output_name[64];
    char display[32];
    char rotation[16];
    bool connected;
    int width;
    int height;
    int refresh_mhz;
    int phys_width_mm;
    int phys_height_mm;
    int dpi;

    // True when this monitor's output exposes a vrr_capable RandR property set
    // to 1 (FreeSync/Adaptive-Sync usable). Drives whether the settings app
    // offers the Variable Refresh checkbox for this screen.
    bool vrr_capable;

    char gpu_name[128];

    // Available modes
    struct {
        int width;
        int height;
        int refresh_mhz;
        char label[32];   // "2560x1440 @ 165 Hz" (for dropdown)
        char value[32];   // "2560x1440@165"       (for xorg.conf)
    } modes[MAX_MONITOR_MODES];
    int mode_count;
} MonitorInfo;

/* ===================================================================
 * Per-screen target selection (the generalized "pointer" mechanism)
 *
 * The per-screen views all answer the same question first: "am I editing the
 * Global default, or Screen N?" appearance_target encodes that (-1 = global).
 * Instead of one facet-specific accessor (the old get_active_appearance_ptrs),
 * each view asks for the AppearanceConfig* it should edit and the override
 * flag* that gates it, then reads its own fields off that.
 * =================================================================== */

// Which per-screen facet a pointer-view edits. Each maps to its own override
// flag in ScreenConfig, so a screen can override (say) its wallpaper without
// overriding its font. Adding a future per-screen view = add an enum value, a
// flag in ScreenConfig, and a row in the facet table in views.c.
typedef enum {
    FACET_CURSOR = 0,   // cursor theme + size  (legacy override_global_appearance)
    FACET_FONT,         // font family/size/unit (override_font)
    FACET_WALLPAPER,    // wallpaper path        (override_wallpaper)
    FACET_COUNT
} AppearanceFacet;

#endif // STELLAR_SETTINGS_STATE_H
