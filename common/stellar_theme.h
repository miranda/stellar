// stellar_theme.h
//
// Theme + appearance data shared by all Stellar support apps (settings,
// fileselect, dialogs...).
//
// Terminology note: colors/bitmaps come from the *theme* (relayed from the
// AwesomeWM Lua side via GET_THEME_DATA), while the font is an *appearance*
// setting owned by Stellar's settings.json (GET_APPEARANCE, answered by
// Stellar directly).  request_theme_data() performs both requests and merges
// the result, so apps still make a single call.

#ifndef STELLAR_THEME_H
#define STELLAR_THEME_H

#include <limits.h>

// Forward declarations so this header doesn't force nuklear on everyone.
struct nk_context;
struct nk_cairo_context;
struct nk_color;

// --- Theme + appearance data received from Stellar DE via IPC ---
typedef struct {
    char assets_path[PATH_MAX];
    char theme_dir[PATH_MAX];
    int dpi;
    int screen;

    // Font (appearance setting, authoritative source: Stellar settings.json).
    // `font` is a fontconfig family name; `font_path` is the concrete file
    // resolved by the DE (already redirected to a converted .otb for bitmap
    // fonts); `font_size` is in pixels, snapped to a strike for bitmap fonts.
    char  font[128];
    float font_size;
	char  font_unit[8];
    char  font_path[PATH_MAX];
    int   font_is_bitmap;

    // Preference for bitmap rendering when both bitmap and vector variants
    // of the same family exist (from settings.json).
    int   font_prefer_bitmap;

    // Colors ("#rrggbb")
    char bg_normal[16];
    char bg_focus[16];
    char bg_urgent[16];
    char fg_normal[16];
    char fg_focus[16];
    char fg_urgent[16];
    char border_normal[16];
    char border_focus[16];
	char nk_color_window[16];
	char nk_color_text[16];
	char nk_color_button[16];
	char nk_color_button_hover[16];
	char nk_color_button_active[16];
	char nk_color_border[16];

    // Sizes
    int border_width;
    int useless_gap;
} ThemeData;

// Resolve an X display string (e.g. ":0.1") to the Stellar screen index that
// owns it, by asking the DE (which holds the canonical display<->screen map).
// Returns the screen index (>= 0), or -1 if unknown / DE unreachable.  Useful
// for global helpers (polkit agent) that have no STELLAR_SCREEN of their own.
int stellar_screen_for_display(const char *display);

// Fetch theme colors (relayed from Awesome) AND appearance/font settings
// (answered by Stellar) for a screen, merged into one struct.
// Returns 0 on success, -1 on failure. Caller owns the struct.
int request_theme_data(int screen_num, ThemeData *out);

// Fetch only the appearance (font) settings from Stellar and merge them into
// an existing ThemeData.  Called automatically by request_theme_data(); only
// needed standalone if you want to refresh the font without re-fetching the
// theme.  Returns 0 on success.
int request_appearance(int screen_num, ThemeData *out);

// Parse a "#rrggbb" / "#rrggbbaa" string, returning `fallback` on bad input.
// Implemented in stellar_nk_theme.c - link that object ONLY into nuklear
// apps (programs whose main .c defines NK_IMPLEMENTATION); the prototypes
// below are harmless in non-nuklear tools as long as they are never called.
struct nk_color stellar_theme_color(const char *hex, struct nk_color fallback);

// Wrapped label that honors '\n': one wrapped text block per line, each
// sized from the current font and the available row width (plain
// nk_label_wrap treats newlines as ordinary glyphs).  Implemented in
// stellar_nk_theme.c - nuklear apps only.
void nk_label_wrap_multiline(struct nk_context *ctx, const char *text);

// Apply theme colors + the configured font to a nuklear context.
// Frees the previously applied font (it keeps one static slot), so it is safe
// to call repeatedly (e.g. after a live RELOAD).  Pass the ThemeData from
// request_theme_data().  Returns 0 on success (theme colors are applied even
// if the font failed to load).
int apply_nk_theme(struct nk_context *ctx, struct nk_cairo_context *cairo_ctx,
                   const ThemeData *td);

#endif // STELLAR_THEME_H
