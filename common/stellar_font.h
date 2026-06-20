// stellar_font.h
// Shared font handling for the Stellar DE, settings app, and support apps.
//
// Goals:
//   1. Fonts are selected by *family name* + size (fontconfig), not by path.
//   2. Legacy bitmap fonts (.bdf / .pcf / .pcf.gz) are transparently converted
//      to .otb (via fonttosfnt) and installed into the user's fontconfig font
//      path (~/.local/share/fonts/stellar-otb).  Once installed there:
//        - Pango (and therefore AwesomeWM) can match them BY NAME, with
//          automatic per-glyph fallback for missing glyphs (nerdfont symbols
//          on an 8x8 Atari ST font, etc).
//        - Our cairo/nuklear apps get a concrete file path back from
//          stellar_font_resolve() and load the .otb directly with FreeType.
//   3. Bitmap fonts only exist at fixed pixel strikes, so the requested pixel
//      size is snapped to the nearest available strike.
//
// Sizes are in PIXELS everywhere in Stellar.  Pixel sizes are DPI-independent
// in Pango too ("Family 16px"), which sidesteps the pt->px rounding problem
// that breaks 1:1 bitmap rendering.  Use StellarFontInfo.pango_desc for
// anything Pango-based (Awesome themes).
//
// Build: link against fontconfig (pkg-config --cflags --libs fontconfig).
// Runtime dependency for conversion: the `fonttosfnt` binary (xorg).

#ifndef STELLAR_FONT_H
#define STELLAR_FONT_H

#include <stdbool.h>
#include <limits.h>

typedef struct {
    char  family[128];      // resolved family name (what fontconfig matched)
    char  path[PATH_MAX];   // concrete file to load (redirected to .otb for bitmap sources)
    float size_px;          // requested pixel size, snapped to a strike for bitmap fonts
    float size_pt;          // equivalent point size at the dpi given to resolve()
    bool  is_bitmap;        // true if the matched face is a (converted) bitmap font
    char  pango_desc[160];  // ready-to-use Pango description, e.g. "AtariST8x16 16px"
} StellarFontInfo;

// Initialize fontconfig (idempotent, called automatically by the functions
// below, but you can call it up front).  Returns 0 on success.
int  stellar_font_init(void);
void stellar_font_shutdown(void);

// Scan the standard font directories (~/.fonts, ~/.local/share/fonts,
// /usr/local/share/fonts, /usr/share/fonts) for .bdf/.pcf(.gz) files and make
// sure each one has an up-to-date converted .otb installed in
// ~/.local/share/fonts/stellar-otb.  Runs `fc-cache` on that directory and
// refreshes this process's fontconfig state when anything changed.
// This is what makes bitmap fonts appear "natively supported" everywhere.
// If `force` is true, everything is reconverted regardless of timestamps.
// Returns the number of fonts (re)converted, or -1 on hard failure.
int  stellar_font_sync_bitmap_cache(bool force);

// Resolve a font by family name (or, for backward compatibility, an absolute
// file path) plus a pixel size, into a concrete file + snapped size.
//   family   - fontconfig family ("DejaVu Sans", "monospace", "AtariST8x16"...)
//              or an absolute path to a font file (legacy configs).
//   size  - desired size (e.g., 14) <= 0 falls back to 14.
//   unit  - "pt" for points (scaled by dpi), "px" for absolute pixels.
//   dpi      - used only to compute size_pt (<= 0 falls back to 96).
// Returns 0 on success, -1 if nothing could be matched at all.
int  stellar_font_resolve(const char *family, float size, const char *unit, int dpi,
                          StellarFontInfo *out);

// Extended resolve with an explicit bitmap-variant preference.  When
// prefer_bitmap is true AND the family exists in both scalable and bitmap
// variants, bias fontconfig toward the bitmap (.otb).  If the family has NO
// bitmap variant, the preference is silently ignored (the vector font is
// returned as usual, NOT a random bitmap from a different family).
int  stellar_font_resolve_ex(const char *family, float size, const char *unit, int dpi,
                             bool prefer_bitmap, StellarFontInfo *out);

// List all font family names known to fontconfig (sorted, de-duplicated).
// Caller frees with stellar_font_free_families().  Returns 0 on success.
int  stellar_font_list_families(char ***out_names, int *out_count);
void stellar_font_free_families(char **names, int count);

// List all available pixel strikes for a given family (sorted & deduplicated).
// Caller frees *out_sizes with free(). Returns 0 on success.
int stellar_font_list_strikes(const char *family, float **out_sizes, int *out_count);

// Check whether a font family exists in multiple format variants (bitmap AND
// vector).  When both *has_bitmap and *has_vector are true, the settings UI
// can offer a toggle between pixel-perfect bitmap rendering and smooth vector
// scaling.  Returns 0 on success.
int stellar_font_family_has_variants(const char *family,
                                     bool *has_bitmap, bool *has_vector);

// Pick fallback fonts for per-glyph fallback in the cairo/nuklear apps
// (AwesomeWM gets fallback for free from Pango/fontconfig):
//   1. A symbol font, found by *charset* match on Nerd Font / Powerline
//      private-use codepoints (e.g. "Symbols Nerd Font" or any patched font).
//   2. The system default sans face, for ordinary missing Unicode.
// Files equal to `primary_path` or each other are skipped.  Fills up to
// `max_out` entries of `out` and returns the count (0 if nothing suitable).
int  stellar_font_resolve_fallbacks(const char *primary_path, float size, const char *unit,
                                    int dpi, StellarFontInfo *out, int max_out);

#endif // STELLAR_FONT_H
