// stellar_nk_theme.c
// Nuklear presentation half of the Stellar theming code: applies ThemeData
// (colors + font) to a nuklear context.  Split out of stellar_theme.c so the
// IPC/transport half stays nuklear-free and can be linked into non-nuklear
// tools.
//
// LINK RULE: only link this object into programs whose main .c defines
// NK_IMPLEMENTATION and NK_XCB_CAIRO_IMPLEMENTATION (settings, fileselect,
// dialog, polkit-agent...).  The nk_* symbols referenced here are defined in
// that translation unit.  The NK_INCLUDE_* feature defines below MUST match
// the ones used in the app's main .c, otherwise the nk struct layouts will
// differ between translation units.

#include <stdio.h>
#include <string.h>
#include <cairo/cairo.h>

#define NK_INCLUDE_FIXED_TYPES
#define NK_INCLUDE_STANDARD_IO
#define NK_INCLUDE_STANDARD_VARARGS
#define NK_INCLUDE_DEFAULT_ALLOCATOR
#include "nuklear.h"
#include "nuklear_xcb.h"

#include "stellar_nk_theme.h"
#include "stellar_theme.h"
#include "stellar_font.h"

/* The nuklear_xcb backend implements this but forgot to expose it in the header API */
extern cairo_surface_t *nk_cairo_surface(struct nk_cairo_context *cairo_ctx);

/* ---------- Nuklear theming (shared by all support apps) ---------- */

struct nk_color stellar_theme_color(const char *hex, struct nk_color fallback) {
    if (!hex || hex[0] != '#') return fallback;

    unsigned r, g, b, a = 255;
    size_t len = strlen(hex);

    if (len == 7) {
        if (sscanf(hex + 1, "%02x%02x%02x", &r, &g, &b) != 3) return fallback;
    } else if (len == 9) {
        if (sscanf(hex + 1, "%02x%02x%02x%02x", &r, &g, &b, &a) != 4) return fallback;
    } else {
        return fallback;
    }
    return nk_rgba((int)r, (int)g, (int)b, (int)a);
}

// One static slot so re-applying the theme frees the previous font.
static struct nk_user_font *theme_font = NULL;

// Linear blend between two colors (t = 0 -> a, t = 1 -> b).
static struct nk_color nk_color_mix(struct nk_color a, struct nk_color b, float t) {
    struct nk_color c;
    c.r = (nk_byte)((1.0f - t) * a.r + t * b.r);
    c.g = (nk_byte)((1.0f - t) * a.g + t * b.g);
    c.b = (nk_byte)((1.0f - t) * a.b + t * b.b);
    c.a = (nk_byte)((1.0f - t) * a.a + t * b.a);
    return c;
}

int apply_nk_theme(struct nk_context *ctx, struct nk_cairo_context *cairo_ctx,
                   const ThemeData *td) {
    int ok = 0;

    // --- Font ---
    const char *path = td->font_path;
    float size = td->font_size > 0 ? td->font_size : 14.0f;

    StellarFontInfo fi;
    if ((!path || path[0] == '\0') && td->font[0] != '\0') {
        // No resolved path in the theme data: resolve by name now.
        if (stellar_font_resolve(td->font, size, td->font_unit, td->dpi, &fi) == 0) {
            path = fi.path;
            size = fi.size_px;
        }
    }

    if (path && path[0] != '\0') {
        // Per-glyph fallback: a symbol/Nerd font (if installed) plus the
        // system sans face, so bitmap fonts still render nerdfont symbols
        // and ordinary missing Unicode.
        StellarFontInfo fb[2];
        const char *fb_files[2] = { NULL, NULL };
        int fb_count = stellar_font_resolve_fallbacks(path, size, td->font_unit, td->dpi,
                                                      fb, 2);
        for (int i = 0; i < fb_count; i++) fb_files[i] = fb[i].path;

        struct nk_user_font *new_font =
            nk_cairo_font_create_with_fallbacks(cairo_ctx, path, size,
                                                fb_files, fb_count);
        if (new_font) {
            nk_style_set_font(ctx, new_font);
            if (theme_font) nk_cairo_font_free(theme_font);
            theme_font = new_font;
            printf("Theme applied: font %s (%.1fpx) from %s (%d fallback%s)\n",
                   td->font[0] ? td->font : "(unnamed)", (double)size, path,
                   fb_count, fb_count == 1 ? "" : "s");
        } else {
            printf("Failed to load theme font: %s\n", path);
            ok = -1;
        }
    }

    // --- Colors ---
    // Six primaries come from the theme (defaults preserve the original
    // hardcoded Stellar look).  Every other widget color is derived from
    // them, so the whole table is consistent with the theme - this also
    // avoids depending on nuklear's nk_default_color_style, which is only
    // visible in the NK_IMPLEMENTATION translation unit.
    struct nk_color window =
        stellar_theme_color(td->nk_color_window, nk_rgb(0, 0, 0));
    struct nk_color text =
        stellar_theme_color(td->nk_color_text, nk_rgb(255, 255, 255));
    struct nk_color button =
        stellar_theme_color(td->nk_color_button, nk_rgb(0, 0, 0));
    struct nk_color button_hover =
        stellar_theme_color(td->nk_color_button_hover, nk_rgb(0, 0, 80));
    struct nk_color button_active =
        stellar_theme_color(td->nk_color_button_active, nk_rgb(0, 0, 255));
    struct nk_color border =
        stellar_theme_color(td->nk_color_border, nk_rgb(0, 0, 255));

    // Derived shades
    struct nk_color field        = nk_color_mix(window, text, 0.08f);  // edits, sliders...
    struct nk_color field_hi     = nk_color_mix(window, text, 0.16f);
    struct nk_color cursor       = nk_color_mix(text, window, 0.30f);
    struct nk_color cursor_hover = nk_color_mix(text, window, 0.15f);

    struct nk_color table[NK_COLOR_COUNT];
    // Future-proof default for any entries this nuklear version adds that we
    // don't know by name.
    for (int i = 0; i < NK_COLOR_COUNT; i++) table[i] = field;

    table[NK_COLOR_TEXT]                    = text;
    table[NK_COLOR_WINDOW]                  = window;
    table[NK_COLOR_HEADER]                  = button_hover;
    table[NK_COLOR_BORDER]                  = border;
    table[NK_COLOR_BUTTON]                  = button;
    table[NK_COLOR_BUTTON_HOVER]            = button_hover;
    table[NK_COLOR_BUTTON_ACTIVE]           = button_active;
    table[NK_COLOR_TOGGLE]                  = field;
    table[NK_COLOR_TOGGLE_HOVER]            = field_hi;
    table[NK_COLOR_TOGGLE_CURSOR]           = button_active;
    table[NK_COLOR_SELECT]                  = field;
    table[NK_COLOR_SELECT_ACTIVE]           = button_active;
    table[NK_COLOR_SLIDER]                  = field;
    table[NK_COLOR_SLIDER_CURSOR]           = cursor;
    table[NK_COLOR_SLIDER_CURSOR_HOVER]     = cursor_hover;
    table[NK_COLOR_SLIDER_CURSOR_ACTIVE]    = text;
    table[NK_COLOR_PROPERTY]                = field;
    table[NK_COLOR_EDIT]                    = field;
    table[NK_COLOR_EDIT_CURSOR]             = text;
    table[NK_COLOR_COMBO]                   = field;
    table[NK_COLOR_CHART]                   = field;
    table[NK_COLOR_CHART_COLOR]             = button_active;
    table[NK_COLOR_CHART_COLOR_HIGHLIGHT]   = border;
    table[NK_COLOR_SCROLLBAR]               = window;
    table[NK_COLOR_SCROLLBAR_CURSOR]        = cursor;
    table[NK_COLOR_SCROLLBAR_CURSOR_HOVER]  = cursor_hover;
    table[NK_COLOR_SCROLLBAR_CURSOR_ACTIVE] = text;
    table[NK_COLOR_TAB_HEADER]              = button_hover;

    nk_style_from_table(ctx, table);
    return ok;
}

void stellar_nk_theme_cleanup(struct nk_cairo_context *cairo_ctx) {
    // Free the server-side XRender glyphs before the connection dies
    if (theme_font) {
        nk_cairo_font_free(theme_font);
        theme_font = NULL;
    }

    // Force Cairo to drop its XCB connection cache
    if (cairo_ctx) {
        cairo_surface_t *surf = nk_cairo_surface(cairo_ctx);
        if (surf) {
            cairo_device_t *dev = cairo_surface_get_device(surf);
            cairo_device_finish(dev);
        }
    }
}

/* --------------------------------------------------------------------
 * Multiline wrapped label.  nuklear's nk_label_wrap only wraps on width
 * and renders '\n' as a glyph; this splits on newlines and emits one
 * wrapped text block per line, sizing each row from the font and the
 * measured text width so wrapped lines are not clipped.
 * -------------------------------------------------------------------- */
void nk_label_wrap_multiline(struct nk_context *ctx, const char *text)
{
    const struct nk_user_font *f = ctx->style.font;
    float line_h = f->height + 4.0f;
    float avail = nk_window_get_content_region(ctx).w
                  - 2.0f * ctx->style.window.padding.x - 8.0f;
    const char *p = text;

    if (!p) return;
    if (avail < 32.0f) avail = 32.0f;

    while (*p) {
        const char *nl = strchr(p, '\n');
        int len = nl ? (int)(nl - p) : (int)strlen(p);

        if (len == 0) {
            /* blank line: half-height spacer */
            nk_layout_row_dynamic(ctx, line_h * 0.5f, 1);
            nk_spacing(ctx, 1);
        } else {
            float tw = f->width(f->userdata, f->height, p, len);
            int rows = (int)(tw / avail) + 1;
            nk_layout_row_dynamic(ctx, line_h * (float)rows, 1);
            nk_text_wrap(ctx, p, len);
        }

        if (!nl) break;
        p = nl + 1;
    }
}
