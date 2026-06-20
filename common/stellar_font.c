// stellar_font.c
// See stellar_font.h for the overview.

#include "stellar_font.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <dirent.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>
#include <errno.h>
#include <fcntl.h>
#include <math.h>

#include <fontconfig/fontconfig.h>
#include <fontconfig/fcfreetype.h>

// get_user_home_dir() lives in stellar_config.c, which is already compiled
// into every Stellar program.
#include "stellar_config.h"

#define OTB_SUBDIR ".local/share/fonts/stellar-otb"
#define DEFAULT_FONT_PX 14.0f

static bool fc_ready = false;

/* ---------- Small helpers ---------- */

int stellar_font_init(void) {
    if (fc_ready) return 0;
    if (!FcInit()) return -1;
    fc_ready = true;
    return 0;
}

void stellar_font_shutdown(void) {
    if (!fc_ready) return;
    FcFini();
    fc_ready = false;
}

// Where converted .otb files are installed.  Inside the user's default
// fontconfig path so Pango/Awesome pick them up by name automatically.
static int otb_dir(char *out, size_t out_size) {
    const char *home = get_user_home_dir();
    if (!home) return -1;
    snprintf(out, out_size, "%s/" OTB_SUBDIR, home);
    return 0;
}

static int mkdir_p(const char *path) {
    char tmp[PATH_MAX];
    snprintf(tmp, sizeof(tmp), "%s", path);
    for (char *p = tmp + 1; *p; p++) {
        if (*p == '/') {
            *p = '\0';
            if (mkdir(tmp, 0755) == -1 && errno != EEXIST) return -1;
            *p = '/';
        }
    }
    if (mkdir(tmp, 0755) == -1 && errno != EEXIST) return -1;
    return 0;
}

static bool ends_with_nocase(const char *s, const char *suffix) {
    size_t sl = strlen(s), xl = strlen(suffix);
    if (xl > sl) return false;
    return strcasecmp(s + sl - xl, suffix) == 0;
}

// Is this a bitmap source format that Pango can't use but fonttosfnt can read?
static bool is_bitmap_source(const char *path) {
    return ends_with_nocase(path, ".bdf")    ||
           ends_with_nocase(path, ".bdf.gz") ||
           ends_with_nocase(path, ".pcf")    ||
           ends_with_nocase(path, ".pcf.gz");
}

// FNV-1a hash of the full source path, so two fonts with the same basename in
// different directories don't collide in the cache.
static unsigned fnv1a(const char *s) {
    unsigned h = 2166136261u;
    while (*s) { h ^= (unsigned char)*s++; h *= 16777619u; }
    return h;
}

// Build the cache .otb path for a given bitmap source file.
static int otb_path_for(const char *src, char *out, size_t out_size) {
    char dir[PATH_MAX];
    if (otb_dir(dir, sizeof(dir)) != 0) return -1;

    const char *base = strrchr(src, '/');
    base = base ? base + 1 : src;

    // strip extension(s) and sanitize
    char name[128];
    snprintf(name, sizeof(name), "%s", base);
    char *dot = strchr(name, '.');
    if (dot) *dot = '\0';
    for (char *p = name; *p; p++) {
        if (*p == ' ' || *p == '/' || *p == '\\') *p = '_';
    }

    snprintf(out, out_size, "%s/%s-%08x.otb", dir, name, fnv1a(src));
    return 0;
}

/* ---------- Conversion ---------- */

// Run: fonttosfnt -b -g 2 -m 2 -o <dst> <src>
static int run_fonttosfnt(const char *src, const char *dst) {
    pid_t pid = fork();
    if (pid < 0) return -1;

    if (pid == 0) {
        // Quiet child: fonttosfnt is chatty about every undefined glyph.
        int devnull = open("/dev/null", O_WRONLY);
        if (devnull >= 0) { dup2(devnull, 1); dup2(devnull, 2); close(devnull); }
        execlp("fonttosfnt", "fonttosfnt",
               "-b", "-g", "2", "-m", "2",
               "-o", dst, src, (char *)NULL);
        _exit(127);
    }

    int status = 0;
    if (waitpid(pid, &status, 0) < 0) return -1;
    if (!WIFEXITED(status) || WEXITSTATUS(status) != 0) {
        unlink(dst);  // don't leave a truncated/garbage file in the cache
        return -1;
    }
    return 0;
}

// Make sure an up-to-date .otb exists for `src`.  Writes the otb path to
// `out`.  Returns 1 if a conversion was performed, 0 if the cache was already
// fresh, -1 on failure.
static int ensure_otb(const char *src, bool force, char *out, size_t out_size) {
    struct stat src_st;
    if (stat(src, &src_st) != 0) return -1;

    if (otb_path_for(src, out, out_size) != 0) return -1;

    // Negative cache: failures leave a `.failed` marker so known-bad sources
    // are not retried on every sync (until the source changes or force).
    char marker[PATH_MAX + 8];
    snprintf(marker, sizeof(marker), "%s.failed", out);

    struct stat dst_st;
    if (!force && stat(out, &dst_st) == 0 &&
        dst_st.st_mtime >= src_st.st_mtime && dst_st.st_size > 0) {
        return 0;  // cache hit
    }
    if (!force && stat(marker, &dst_st) == 0 &&
        dst_st.st_mtime >= src_st.st_mtime) {
        return -1;  // known-bad source, skip quietly
    }

    char dir[PATH_MAX];
    if (otb_dir(dir, sizeof(dir)) != 0 || mkdir_p(dir) != 0) return -1;

    if (run_fonttosfnt(src, out) != 0) {
        fprintf(stderr, "[stellar_font] fonttosfnt failed for %s\n", src);
        FILE *m = fopen(marker, "w");
        if (m) fclose(m);
        return -1;
    }
    unlink(marker);
    fprintf(stderr, "[stellar_font] converted %s -> %s\n", src, out);
    return 1;
}

// Some distros ship 70-no-bitmaps-except-emoji.conf, which makes fontconfig
// reject every non-scalable font - including our converted .otb files - so
// they would be invisible to Pango/Awesome and to name matching.  An
// acceptfont *pattern* overrides reject patterns, so install a user-level
// config that re-accepts sfnt-wrapped bitmap fonts (fontformat=TrueType,
// scalable=false).  Raw .pcf/.bdf stay rejected, which is exactly right:
// it also guarantees name matches resolve to the .otb instead of the raw
// bitmap file on those distros.
static void ensure_fontconfig_accept_conf(void) {
    const char *home = get_user_home_dir();
    if (!home) return;

    char dir[PATH_MAX];
    const char *xdg = getenv("XDG_CONFIG_HOME");
    if (xdg && xdg[0] == '/') {
        snprintf(dir, sizeof(dir), "%s/fontconfig/conf.d", xdg);
    } else {
        snprintf(dir, sizeof(dir), "%s/.config/fontconfig/conf.d", home);
    }

    char path[PATH_MAX + 64];
    snprintf(path, sizeof(path), "%s/05-stellar-bitmap-fonts.conf", dir);
    if (access(path, R_OK) == 0) return;  // already installed

    if (mkdir_p(dir) != 0) return;

    FILE *f = fopen(path, "w");
    if (!f) return;
    fputs(
        "<?xml version=\"1.0\"?>\n"
        "<!DOCTYPE fontconfig SYSTEM \"fonts.dtd\">\n"
        "<fontconfig>\n"
        "  <!-- Generated by Stellar - do not edit -->\n"
        "  <description>Accept sfnt-wrapped bitmap fonts (.otb) converted by Stellar</description>\n"
        "  <selectfont>\n"
        "    <acceptfont>\n"
        "      <pattern>\n"
        "        <patelt name=\"fontformat\"><string>TrueType</string></patelt>\n"
        "        <patelt name=\"scalable\"><bool>false</bool></patelt>\n"
        "      </pattern>\n"
        "    </acceptfont>\n"
        "  </selectfont>\n"
        "</fontconfig>\n",
        f);
    fclose(f);
    fprintf(stderr, "[stellar_font] installed fontconfig accept rule: %s\n", path);
}

// Refresh fontconfig's view of the otb dir after conversions.
static void refresh_fontconfig(void) {
    char dir[PATH_MAX];
    if (otb_dir(dir, sizeof(dir)) != 0) return;

    pid_t pid = fork();
    if (pid == 0) {
        execlp("fc-cache", "fc-cache", "-f", dir, (char *)NULL);
        _exit(127);
    }
    if (pid > 0) waitpid(pid, NULL, 0);

    // Make the *current process* see the new fonts too.
    FcInitReinitialize();
}

/* ---------- Directory scan ---------- */

static int scan_dir_recursive(const char *dir, const char *skip_dir,
                              bool force, int depth) {
    if (depth > 6) return 0;
    if (skip_dir && strcmp(dir, skip_dir) == 0) return 0;

    DIR *d = opendir(dir);
    if (!d) return 0;

    int converted = 0;
    struct dirent *e;
    while ((e = readdir(d)) != NULL) {
        if (e->d_name[0] == '.') continue;

        char path[PATH_MAX];
        snprintf(path, sizeof(path), "%s/%s", dir, e->d_name);

        struct stat st;
        if (stat(path, &st) != 0) continue;

        if (S_ISDIR(st.st_mode)) {
            converted += scan_dir_recursive(path, skip_dir, force, depth + 1);
        } else if (S_ISREG(st.st_mode) && is_bitmap_source(path)) {
            char otb[PATH_MAX];
            if (ensure_otb(path, force, otb, sizeof(otb)) == 1) {
                converted++;
            }
        }
    }
    closedir(d);
    return converted;
}

int stellar_font_sync_bitmap_cache(bool force) {
    if (stellar_font_init() != 0) return -1;

    ensure_fontconfig_accept_conf();

    const char *home = get_user_home_dir();
    char skip[PATH_MAX] = {0};
    otb_dir(skip, sizeof(skip));

    int converted = 0;
    char dir[PATH_MAX];

    if (home) {
        snprintf(dir, sizeof(dir), "%s/.fonts", home);
        converted += scan_dir_recursive(dir, skip, force, 0);
        snprintf(dir, sizeof(dir), "%s/.local/share/fonts", home);
        converted += scan_dir_recursive(dir, skip, force, 0);
    }
    converted += scan_dir_recursive("/usr/local/share/fonts", skip, force, 0);
    converted += scan_dir_recursive("/usr/share/fonts", skip, force, 0);

    if (converted > 0) {
        refresh_fontconfig();
        fprintf(stderr, "[stellar_font] bitmap cache sync: %d font(s) converted\n",
                converted);
    }
    return converted;
}

/* ---------- Strike snapping ---------- */

// For bitmap fonts: find the available pixel strike closest to `want_px`
// within the given file.  Returns the snapped size, or `want_px` if no strike
// information is available.
static float snap_to_strike(const char *file, const char *family, float want_px) {
    FcPattern *pat = FcPatternCreate();
    FcObjectSet *os = FcObjectSetBuild(FC_PIXEL_SIZE, FC_FILE, (char *)NULL);
    if (!pat || !os) {
        if (pat) FcPatternDestroy(pat);
        if (os) FcObjectSetDestroy(os);
        return want_px;
    }
    FcPatternAddString(pat, FC_FAMILY, (const FcChar8 *)family);

    FcFontSet *set = FcFontList(NULL, pat, os);
    float best = want_px;
    double best_dist = -1.0;

    if (set) {
        for (int i = 0; i < set->nfont; i++) {
            FcChar8 *f = NULL;
            if (FcPatternGetString(set->fonts[i], FC_FILE, 0, &f) != FcResultMatch)
                continue;
            // Prefer strikes from the matched file; if the family only exists
            // in that file this matches everything anyway.
            if (file && file[0] && strcmp((const char *)f, file) != 0)
                continue;

            double px;
            for (int j = 0;
                 FcPatternGetDouble(set->fonts[i], FC_PIXEL_SIZE, j, &px) == FcResultMatch;
                 j++) {
                double dist = fabs(px - (double)want_px);
                if (best_dist < 0 || dist < best_dist) {
                    best_dist = dist;
                    best = (float)px;
                }
            }
        }
        FcFontSetDestroy(set);
    }

    FcPatternDestroy(pat);
    FcObjectSetDestroy(os);
    return best;
}

/* ---------- Resolution ---------- */

// Forward declaration - full implementation is below stellar_font_list_strikes.
static int has_variants_internal(const char *family, bool *has_bitmap, bool *has_vector);

static void fill_sizes(StellarFontInfo *out, float final_px, float orig_size, const char *orig_unit, int dpi) {
    if (final_px <= 0) final_px = DEFAULT_FONT_PX;
    if (dpi <= 0) dpi = 96;
    out->size_px = final_px;
    out->size_pt = final_px * 72.0f / (float)dpi;

    // "Npx" is an absolute size in Pango: DPI-independent, exact pixel grid.
    // Use %g so 16.0 prints as "16".

    const char *u = (orig_unit && orig_unit[0]) ? orig_unit : "px";
    
    // Generate the exact string AwesomeWM needs (e.g., "Fira Sans 10pt" or "Spleen 13px")
    snprintf(out->pango_desc, sizeof(out->pango_desc), "%s %g%s",
             out->family, (double)orig_size, u);
}

// Resolve when the config still contains an absolute file path (legacy).
static int resolve_path(const char *path, float size, const char *unit, int dpi,
                        StellarFontInfo *out) {
    char use_path[PATH_MAX];
    snprintf(use_path, sizeof(use_path), "%s", path);

    out->is_bitmap = false;
    if (is_bitmap_source(path)) {
        // Convert on demand so the rest of the system can use it by name too.
        // If conversion fails (e.g. exotic encoding fonttosfnt can't map),
        // fall back to the raw file: the cairo/FreeType apps can still load
        // .pcf/.bdf directly - only Pango needs the .otb.
        if (ensure_otb(path, false, use_path, sizeof(use_path)) >= 0) {
            refresh_fontconfig();
        } else {
            snprintf(use_path, sizeof(use_path), "%s", path);
        }
        out->is_bitmap = true;
    } else if (ends_with_nocase(path, ".otb")) {
        out->is_bitmap = true;
    }

    if (access(use_path, R_OK) != 0) return -1;

    // Ask fontconfig/freetype for the real family name inside the file.
    int count = 0;
    FcPattern *q = FcFreeTypeQuery((const FcChar8 *)use_path, 0, NULL, &count);
    if (q) {
        FcChar8 *fam = NULL;
        if (FcPatternGetString(q, FC_FAMILY, 0, &fam) == FcResultMatch) {
            snprintf(out->family, sizeof(out->family), "%s", (const char *)fam);
        }
        FcPatternDestroy(q);
    }
    if (out->family[0] == '\0') {
        const char *base = strrchr(use_path, '/');
        snprintf(out->family, sizeof(out->family), "%.127s",
                 base ? base + 1 : use_path);
    }

    snprintf(out->path, sizeof(out->path), "%s", use_path);

    float px_req = size;
    if (unit && strcmp(unit, "pt") == 0 && dpi > 0) {
        px_req = size * ((float)dpi / 72.0f);
    }

    if (out->is_bitmap) {
        px_req = snap_to_strike(out->path, out->family,
                                px_req > 0 ? px_req : DEFAULT_FONT_PX);
    }
    fill_sizes(out, px_req, size, unit, dpi);
    return 0;
}

int stellar_font_resolve_ex(const char *family, float size, const char *unit, int dpi,
                            bool prefer_bitmap, StellarFontInfo *out) {
    if (!out) return -1;
    memset(out, 0, sizeof(*out));
    if (stellar_font_init() != 0) return -1;

    if (!family || family[0] == '\0') family = "sans-serif";
    if (size <= 0) size = DEFAULT_FONT_PX;
    if (!unit || unit[0] == '\0') unit = "px";

    // Unified math: convert everything to a pixel target for fontconfig/cairo
    float px_req = size;
    if (strcmp(unit, "pt") == 0 && dpi > 0) {
        px_req = size * ((float)dpi / 72.0f);
    }

    // Legacy escape hatch: configs that still hold a path keep working.
    if (family[0] == '/') {
        return resolve_path(family, size, unit, dpi, out);
    }

    // Safety check: only apply the bitmap preference when the family
    // actually has a non-scalable variant.  Without this, fontconfig may
    // fall through to an unrelated bitmap font from a different family
    // (e.g. asking for "JetBrains Mono" with SCALABLE=false returns
    // "Sony Fixed" if JetBrains Mono only ships as a .ttf).
    bool apply_bitmap_bias = false;
    if (prefer_bitmap) {
        bool has_bmp = false, has_vec = false;
        if (has_variants_internal(family, &has_bmp, &has_vec) == 0 && has_bmp) {
            apply_bitmap_bias = true;
        }
    }

    FcPattern *pat = FcPatternCreate();
    if (!pat) return -1;
    FcPatternAddString(pat, FC_FAMILY, (const FcChar8 *)family);
    FcPatternAddDouble(pat, FC_PIXEL_SIZE, (double)px_req);

    if (apply_bitmap_bias) {
        FcPatternAddBool(pat, FC_SCALABLE, FcFalse);
    }

    FcConfigSubstitute(NULL, pat, FcMatchPattern);
    FcDefaultSubstitute(pat);

    FcResult result;
    FcPattern *match = FcFontMatch(NULL, pat, &result);
    FcPatternDestroy(pat);
    if (!match) return -1;

    FcChar8 *file = NULL;
    FcChar8 *fam = NULL;
    FcBool scalable = FcTrue;
    if (FcPatternGetString(match, FC_FILE, 0, &file) != FcResultMatch || !file) {
        FcPatternDestroy(match);
        return -1;
    }
    FcPatternGetString(match, FC_FAMILY, 0, &fam);
    FcPatternGetBool(match, FC_SCALABLE, 0, &scalable);

    snprintf(out->family, sizeof(out->family), "%s",
             fam ? (const char *)fam : family);

    const char *fpath = (const char *)file;
    if (is_bitmap_source(fpath)) {
        // fontconfig matched the raw .bdf/.pcf (some distros still index
        // them).  Pango can't render those, and we want everything on the
        // same file - redirect to the converted .otb.
        char otb[PATH_MAX];
        if (ensure_otb(fpath, false, otb, sizeof(otb)) >= 0) {
            refresh_fontconfig();
            snprintf(out->path, sizeof(out->path), "%s", otb);
        } else {
            snprintf(out->path, sizeof(out->path), "%s", fpath);
        }
        out->is_bitmap = true;
    } else {
        snprintf(out->path, sizeof(out->path), "%s", fpath);
        out->is_bitmap = (!scalable) || ends_with_nocase(fpath, ".otb");
    }
    FcPatternDestroy(match);

    if (out->is_bitmap) {
        px_req = snap_to_strike(out->path, out->family, px_req);
    }
    fill_sizes(out, px_req, size, unit, dpi);
    return 0;
}

// Backward-compatible wrapper: resolves with no bitmap preference.
int stellar_font_resolve(const char *family, float size, const char *unit, int dpi,
                         StellarFontInfo *out) {
    return stellar_font_resolve_ex(family, size, unit, dpi, false, out);
}

/* ---------- Family listing ---------- */

static int cmp_str(const void *a, const void *b) {
    return strcasecmp(*(const char *const *)a, *(const char *const *)b);
}

int stellar_font_list_families(char ***out_names, int *out_count) {
    if (!out_names || !out_count) return -1;
    *out_names = NULL;
    *out_count = 0;
    if (stellar_font_init() != 0) return -1;

    FcPattern *pat = FcPatternCreate();
    FcObjectSet *os = FcObjectSetBuild(FC_FAMILY, (char *)NULL);
    if (!pat || !os) {
        if (pat) FcPatternDestroy(pat);
        if (os) FcObjectSetDestroy(os);
        return -1;
    }

    FcFontSet *set = FcFontList(NULL, pat, os);
    FcPatternDestroy(pat);
    FcObjectSetDestroy(os);
    if (!set) return -1;

    size_t cap = 256, n = 0;
    char **names = malloc(cap * sizeof(char *));
    if (!names) { FcFontSetDestroy(set); return -1; }

    for (int i = 0; i < set->nfont; i++) {
        FcChar8 *fam = NULL;
        if (FcPatternGetString(set->fonts[i], FC_FAMILY, 0, &fam) != FcResultMatch)
            continue;
        if (n >= cap) {
            cap *= 2;
            char **grown = realloc(names, cap * sizeof(char *));
            if (!grown) break;
            names = grown;
        }
        names[n] = strdup((const char *)fam);
        if (names[n]) n++;
    }
    FcFontSetDestroy(set);

    if (n == 0) { free(names); return -1; }

    qsort(names, n, sizeof(char *), cmp_str);

    // de-duplicate in place
    size_t w = 1;
    for (size_t i = 1; i < n; i++) {
        if (strcasecmp(names[i], names[w - 1]) == 0) {
            free(names[i]);
        } else {
            names[w++] = names[i];
        }
    }

    *out_names = names;
    *out_count = (int)w;
    return 0;
}

void stellar_font_free_families(char **names, int count) {
    if (!names) return;
    for (int i = 0; i < count; i++) free(names[i]);
    free(names);
}

int stellar_font_list_strikes(const char *family, float **out_sizes, int *out_count) {
    if (!out_sizes || !out_count) return -1;
    *out_sizes = NULL;
    *out_count = 0;
    if (stellar_font_init() != 0) return -1;

    FcPattern *pat = FcPatternCreate();
    FcObjectSet *os = FcObjectSetBuild(FC_PIXEL_SIZE, (char *)NULL);
    if (!pat || !os) {
        if (pat) FcPatternDestroy(pat);
        if (os) FcObjectSetDestroy(os);
        return -1;
    }

    FcPatternAddString(pat, FC_FAMILY, (const FcChar8 *)family);

    FcFontSet *set = FcFontList(NULL, pat, os);
    FcPatternDestroy(pat);
    FcObjectSetDestroy(os);
    if (!set) return -1;

    size_t cap = 16, n = 0;
    float *sizes = malloc(cap * sizeof(float));
    if (!sizes) { FcFontSetDestroy(set); return -1; }

    for (int i = 0; i < set->nfont; i++) {
        double px;
        // A single font file might define multiple pixel sizes in an array
        for (int j = 0; FcPatternGetDouble(set->fonts[i], FC_PIXEL_SIZE, j, &px) == FcResultMatch; j++) {
            if (n >= cap) {
                cap *= 2;
                float *grown = realloc(sizes, cap * sizeof(float));
                if (!grown) break;
                sizes = grown;
            }
            sizes[n++] = (float)px;
        }
    }
    FcFontSetDestroy(set);

    if (n == 0) {
        free(sizes);
        return 0;
    }

    // Sort ascending
    for (size_t i = 0; i < n - 1; i++) {
        for (size_t j = i + 1; j < n; j++) {
            if (sizes[i] > sizes[j]) {
                float temp = sizes[i];
                sizes[i] = sizes[j];
                sizes[j] = temp;
            }
        }
    }

    // De-duplicate (using a tiny epsilon for float comparison)
    size_t w = 1;
    for (size_t i = 1; i < n; i++) {
        if (fabs(sizes[i] - sizes[w - 1]) > 0.1f) {
            sizes[w++] = sizes[i];
        }
    }

    *out_sizes = sizes;
    *out_count = (int)w;
    return 0;
}

/* ---------- Variant detection ---------- */

// Internal implementation shared by the forward-declared static and the public API.
static int has_variants_internal(const char *family, bool *has_bitmap, bool *has_vector) {
    if (!has_bitmap || !has_vector) return -1;
    *has_bitmap = false;
    *has_vector = false;
    if (!family || family[0] == '\0') return -1;
    if (stellar_font_init() != 0) return -1;

    FcPattern *pat = FcPatternCreate();
    FcObjectSet *os = FcObjectSetBuild(FC_SCALABLE, FC_FILE, (char *)NULL);
    if (!pat || !os) {
        if (pat) FcPatternDestroy(pat);
        if (os) FcObjectSetDestroy(os);
        return -1;
    }

    FcPatternAddString(pat, FC_FAMILY, (const FcChar8 *)family);

    FcFontSet *set = FcFontList(NULL, pat, os);
    FcPatternDestroy(pat);
    FcObjectSetDestroy(os);
    if (!set) return -1;

    for (int i = 0; i < set->nfont; i++) {
        FcBool scalable = FcTrue;
        if (FcPatternGetBool(set->fonts[i], FC_SCALABLE, 0, &scalable) == FcResultMatch) {
            if (scalable) *has_vector = true;
            else          *has_bitmap = true;
        }
    }

    FcFontSetDestroy(set);
    return 0;
}

int stellar_font_family_has_variants(const char *family,
                                     bool *has_bitmap, bool *has_vector) {
    return has_variants_internal(family, has_bitmap, has_vector);
}

/* ---------- Fallback resolution (for the cairo/nuklear apps) ---------- */

// Codepoints used to find a Nerd Font / Powerline symbol face by charset.
static const FcChar32 nerd_probe[] = {
    0xE0B0,   // powerline right-pointing triangle
    0xF015,   // nf-fa-home
    0xE702,   // nf-dev-git_branch
};
#define NERD_PROBE_COUNT (sizeof(nerd_probe) / sizeof(nerd_probe[0]))

// Match a font whose charset actually contains the probe codepoints.
// Returns 0 and fills `out` on success.
static int resolve_by_charset(const FcChar32 *probes, size_t n_probes,
                              float size, const char *unit, int dpi, StellarFontInfo *out) {
    // Unified math: convert everything to a pixel target for fontconfig
    float px_req = size;
    if (unit && strcmp(unit, "pt") == 0 && dpi > 0) {
        px_req = size * ((float)dpi / 72.0f);
    }

    FcPattern *pat = FcPatternCreate();
    FcCharSet *cs = FcCharSetCreate();
    if (!pat || !cs) {
        if (pat) FcPatternDestroy(pat);
        if (cs) FcCharSetDestroy(cs);
        return -1;
    }
    for (size_t i = 0; i < n_probes; i++) FcCharSetAddChar(cs, probes[i]);
    FcPatternAddCharSet(pat, FC_CHARSET, cs);
    
    // Search fontconfig using the exact calculated pixels
    FcPatternAddDouble(pat, FC_PIXEL_SIZE, (double)px_req);
    
    // Family preferences: dedicated symbol fonts first.  These are hints -
    // if none is installed, fontconfig still falls back to whichever
    // installed font covers the probe charset.
    FcPatternAddString(pat, FC_FAMILY, (const FcChar8 *)"Symbols Nerd Font");
    FcPatternAddString(pat, FC_FAMILY, (const FcChar8 *)"Symbols Nerd Font Mono");
    FcPatternAddString(pat, FC_FAMILY, (const FcChar8 *)"PowerlineSymbols");

    FcConfigSubstitute(NULL, pat, FcMatchPattern);
    FcDefaultSubstitute(pat);

    FcResult result;
    FcPattern *match = FcFontMatch(NULL, pat, &result);
    FcPatternDestroy(pat);
    FcCharSetDestroy(cs);
    if (!match) return -1;

    // FcFontMatch always returns *something*; verify the match really has
    // the probe glyphs, otherwise there is no suitable font installed.
    FcCharSet *mcs = NULL;
    bool covered = false;
    if (FcPatternGetCharSet(match, FC_CHARSET, 0, &mcs) == FcResultMatch && mcs) {
        covered = true;
        for (size_t i = 0; i < n_probes; i++) {
            if (!FcCharSetHasChar(mcs, probes[i])) { covered = false; break; }
        }
    }

    FcChar8 *file = NULL;
    FcChar8 *fam = NULL;
    FcBool scalable = FcTrue;
    if (!covered ||
        FcPatternGetString(match, FC_FILE, 0, &file) != FcResultMatch || !file) {
        FcPatternDestroy(match);
        return -1;
    }
    FcPatternGetString(match, FC_FAMILY, 0, &fam);
    FcPatternGetBool(match, FC_SCALABLE, 0, &scalable);

    memset(out, 0, sizeof(*out));
    snprintf(out->family, sizeof(out->family), "%s", fam ? (const char *)fam : "");
    snprintf(out->path, sizeof(out->path), "%s", (const char *)file);
    out->is_bitmap = !scalable;
    FcPatternDestroy(match);

    // If the matched symbol font happens to be a bitmap font, snap it to the grid!
    if (out->is_bitmap) {
        px_req = snap_to_strike(out->path, out->family, px_req);
    }

    // Call fill_sizes with both the physical pixels AND the requested unit/size
    fill_sizes(out, px_req, size, unit, dpi);
    return 0;
}

int stellar_font_resolve_fallbacks(const char *primary_path, float size,
                                   const char *unit, int dpi, 
                                   StellarFontInfo *out, int max_out) {
    if (!out || max_out <= 0) return 0;
    if (stellar_font_init() != 0) return 0;
    if (size <= 0) size = DEFAULT_FONT_PX;
    if (!unit || unit[0] == '\0') unit = "px";

    int n = 0;

    // 1. Symbol / Nerd Font face by charset.
    StellarFontInfo fi;
    if (n < max_out &&
        resolve_by_charset(nerd_probe, NERD_PROBE_COUNT, size, unit, dpi, &fi) == 0 &&
        (!primary_path || strcmp(fi.path, primary_path) != 0)) {
        out[n++] = fi;
    }

    // 2. The default sans face, for ordinary missing Unicode
    if (n < max_out &&
        stellar_font_resolve("sans-serif", size, unit, dpi, &fi) == 0 &&
        !fi.is_bitmap &&
        (!primary_path || strcmp(fi.path, primary_path) != 0) &&
        (n == 0 || strcmp(fi.path, out[0].path) != 0)) {
        out[n++] = fi;
    }

    return n;
}
