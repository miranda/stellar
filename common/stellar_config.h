// stellar_config.h
// Shared configuration structs used by both the DE and the settings app.
// Contains ONLY user-facing settings - no PIDs, file descriptors, X11 handles,
// respawn counters, or any other runtime/transient state.
#ifndef STELLAR_CONFIG_H
#define STELLAR_CONFIG_H

#include <stdbool.h>
#include <limits.h>
#include <stddef.h>

#ifndef MAX_SCREENS
#define MAX_SCREENS 8
#endif

#ifndef MAX_MONITOR_MODES
#define MAX_MONITOR_MODES 48
#endif

#ifndef STELLAR_SHARE_PATH
#define STELLAR_SHARE_PATH "/usr/local/share/stellar"
#endif

#ifndef STELLAR_LIBEXEC_PATH
#define STELLAR_LIBEXEC_PATH "/usr/local/libexec/stellar"
#endif

#define NUM_DPI_STEPS 7
static const int dpi_steps[] = {48, 72, 96, 120, 144, 168, 192};

/* ---------- Leaf Config Structs ---------- */

typedef struct {
    char cursor_theme[128];
    int  cursor_size;
    char gtk_theme[128];
    char icon_theme[128];

    // Fontconfig family name (e.g. "DejaVu Sans", "AtariST8x16"), NOT a path.
    // Legacy absolute paths are still accepted by stellar_font_resolve().
    char  font_name[128];
    // Font size in PIXELS (px is DPI-independent in Pango too, and is the
    // only size that makes sense for bitmap strikes).
    float font_size;
	char font_unit[8];

    // When the same family name exists as both a bitmap (.otb) and a vector
    // (.ttf/.otf), prefer the bitmap variant.  Has no effect when only one
    // format exists.
    bool font_prefer_bitmap;

	char wallpaper_path[256];
	char wallpaper_mode[32];
} AppearanceConfig;

typedef struct {
    int timeout_screensaver;
    int timeout_dpms;
} PowerConfig;

/* ---------- Per-Screen Configuration ---------- */

typedef struct {
    // Neighbor topology (screen indices, -1 = none)
    int neighbor_left;
    int neighbor_right;
    int neighbor_up;
    int neighbor_down;

    // Physical layout
    float phys_scale;
    float phys_offset;

    // Rotation: "normal", "left", "right", "inverted"
    char rotation[16];

	// Preferred mode: "2560x1440@165", empty = auto
    char preferred_mode[32];

    // DPI override: 0 = auto (round down from EDID), else forced value
    // Supported steps: 48, 72, 96, 120, 144, 168, 192
    int dpi_override;

    // Feature toggles
    bool picom_enabled;
    bool tray_enabled;
	bool tearfree_enabled;

    // Power behavior
    bool independent_dpms;
    bool require_explicit_wake;
    bool keep_awake;

    // Per-screen overrides.
    //
    // override_global_appearance is the legacy single flag. It currently still
    // gates the CURSOR facet (cursor_theme / cursor_size) on both the settings
    // and DE sides. The font and wallpaper facets were split out into their own
    // per-facet flags below so a screen can, e.g., keep the global font while
    // overriding only its wallpaper.
    //
    // DE MIGRATION NOTE: the DE's get_*_for_screen() accessors still read
    // override_global_appearance for font. They can be migrated to read
    // override_font instead, one accessor at a time, independently of the
    // settings app - these flags are written additively and the old flag is
    // preserved. Until then, the settings app keeps override_global_appearance
    // in sync with override_font on save so nothing regresses.
    bool override_global_appearance;   // -> cursor facet (and legacy font on DE)
    bool override_font;                 // font facet
    bool override_wallpaper;            // wallpaper facet
    bool override_global_power;
    AppearanceConfig appearance;
    PowerConfig power;
} ScreenConfig;

/* ---------- Global Configuration ---------- */

typedef struct {
    // Global appearance & power (used when screen doesn't override)
    AppearanceConfig appearance;
    PowerConfig power;

	char stellar_theme[128];

    // Screensaver master toggle
    bool saver_enabled;

    // Terminal
    char term_shell[256];
    char term_gui[256];

    // Screens
    int screen_count;
    ScreenConfig screens[MAX_SCREENS];
} GlobalConfig;

/* ---------- Shared Utility Functions ---------- */
// These are implemented in a shared .c file compiled into both programs.
const char* get_user_home_dir(void);
int snap_dpi_down(int raw_dpi);
const char* get_stellar_theme_dir(char *out_path, size_t out_size, const char *theme_name);

#endif // STELLAR_CONFIG_H
