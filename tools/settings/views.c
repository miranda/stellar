// views.c
// All nuklear UI for the Stellar settings app: the sidebar, the per-view
// renderers, and the generalized per-screen target selector that the
// pointer-views (Appearance/Font/Wallpaper) share.
//
// Split out of the settings monolith. The big idea here is that the old
// font-only "get_active_appearance_ptrs" has been generalized: a per-screen
// view first resolves a *target* (Global default vs Screen N) and a *facet*
// (cursor / font / wallpaper), and from those gets the AppearanceConfig* to
// edit plus the override-flag* that gates it. Adding a new per-screen view is
// then: add a SettingsView, add an AppearanceFacet + ScreenConfig flag, add a
// row to facet_table[], and write a render_*_view().
//
// LINK RULE: like stellar_nk_theme.c, this unit references nk_* symbols defined
// in the app's main translation unit (settings.c, which defines
// NK_IMPLEMENTATION). The NK_INCLUDE_* feature defines below MUST match.

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <unistd.h>
#include <sys/wait.h>
#include <math.h>

#define NK_INCLUDE_FIXED_TYPES
#define NK_INCLUDE_STANDARD_IO
#define NK_INCLUDE_STANDARD_VARARGS
#define NK_INCLUDE_DEFAULT_ALLOCATOR
#include "nuklear.h"
#include "nuklear_xcb.h"

#include "stellar_settings_state.h"
#include "stellar_config.h"
#include "stellar_theme.h"
#include "stellar_nk_theme.h"
#include "stellar_font.h"

/* ===================================================================
 * Shared globals (defined in settings.c)
 * =================================================================== */
extern GlobalConfig state;
extern UIState ui;
extern ThemeData theme_data;

extern int stellar_num_screens;
extern int stellar_screen;
extern int screen_grid[MAX_SCREENS][MAX_SCREENS];

extern MonitorInfo monitor_info[MAX_SCREENS];
extern bool monitor_info_loaded;

extern SettingsView current_view;
extern int appearance_target;          // -1 = Global, 0+ = screen index

extern int hardware_mismatch_detected;
extern bool requires_restart;
extern int pending_xorg_change;

// Font preview lifecycle state (owned by settings.c's event loop)
extern struct nk_user_font *preview_font;
extern int pending_preview_update;
extern int pending_theme_apply;
extern StellarFontInfo preview_info;
extern int preview_resolved;
extern bool current_font_has_bitmap;
extern bool current_font_has_vector;
extern char **font_families;
extern int font_family_count;
extern float *current_strikes;
extern char **current_strike_labels;
extern int current_strike_count;
extern int force_bitmap_resolve;

// Stellar theme (global/session setting, edited in the System view)
// stored in state.stellar_theme.

/* ---- Cross-unit functions ---- */
// settings.c
void save_and_reload(void);
void sync_ui_font_into_theme_data(void);
void request_screen_info(void);
// xorg_gen.c
int  calc_auto_dpi(int native_width_px, int phys_width_mm);
void refresh_pending_xorg_change(void);
void apply_display_config(bool *out_needs_restart);
void try_move_screen(int y, int x, int dy, int dx);
// terminal/shell pickers (settings.c)
bool is_executable_in_path(const char *prog_name);
extern const char *known_terminals[];

/* ---- Rotation helpers (small, UI-local) ---- */
static const char *rotation_labels[] = { "Normal", "Left (90\xc2\xb0)", "Right (90\xc2\xb0)", "Inverted (180\xc2\xb0)" };
static const char *rotation_values[] = { "normal", "left", "right", "inverted" };

static int rotation_string_to_index(const char *s) {
    if (!s || strcmp(s, "normal") == 0) return 0;
    if (strcmp(s, "left") == 0)         return 1;
    if (strcmp(s, "right") == 0)        return 2;
    if (strcmp(s, "inverted") == 0)     return 3;
    return 0;
}

/* ---- Bool<->int helper for nuklear checkboxes ---- */
static void nk_checkbox_bool(struct nk_context *ctx, const char *label, bool *val) {
    int tmp = *val ? 1 : 0;
    nk_checkbox_label(ctx, label, &tmp);
    *val = (tmp != 0);
}

/* ---- View titles (single source of truth for both the sidebar and the
 *      centered content-pane heading). Indexed by SettingsView. ---- */
static const char *view_title(SettingsView v) {
    switch (v) {
        case VIEW_APPEARANCE: return "Appearance";
        case VIEW_FONT:       return "Font";
        case VIEW_WALLPAPER:  return "Wallpaper";
        case VIEW_SYSTEM:     return "System";
        case VIEW_DISPLAY:    return "Display";
        case VIEW_RULES:      return "Window Rules";
        default:              return "";
    }
}

// Centered heading drawn at the top of every content pane, above the banner's
// payload and the view body. One call in process_frame keeps all views
// consistent (no per-renderer duplication).
static void render_view_title(struct nk_context *ctx) {
    nk_layout_row_dynamic(ctx, 30, 1);
    nk_label(ctx, view_title(current_view), NK_TEXT_CENTERED);
    nk_layout_row_dynamic(ctx, 8, 1);
    nk_rule_horizontal(ctx, nk_rgb(100, 100, 100), nk_true);
}

// A sidebar tab button that shows selected state. When `view` is the current
// view, the button is painted with the "active" accent in all three states so
// it reads as pressed/selected; otherwise it behaves as a normal button.
// Returns nonzero when clicked.
static int sidebar_tab(struct nk_context *ctx, const char *label, SettingsView view) {
    int selected = (current_view == view);
    if (!selected)
        return nk_button_label(ctx, label);

    // Push the active-button color into normal/hover so the selected tab stays
    // highlighted even when not under the cursor.
    struct nk_style_button *b = &ctx->style.button;
    struct nk_color normal = b->normal.data.color;
    struct nk_color hover  = b->hover.data.color;
    struct nk_color txt     = b->text_normal;

    b->normal = nk_style_item_color(b->active.data.color);
    b->hover  = nk_style_item_color(b->active.data.color);
    b->text_normal = b->text_active;

    int clicked = nk_button_label(ctx, label);

    b->normal = nk_style_item_color(normal);
    b->hover  = nk_style_item_color(hover);
    b->text_normal = txt;
    return clicked;
}

/* ===================================================================
 * Generalized per-screen target + facet machinery
 *
 * A pointer-view edits one facet of one target. The target is the global
 * appearance_target (-1 global, else screen index). The facet says which
 * override flag gates the per-screen copy and which AppearanceConfig fields the
 * view will touch. facet_override_ptr() returns the bool* that the override
 * checkbox toggles; active_appearance() returns the AppearanceConfig* the view
 * should read/write (global config when target == -1 or override is off).
 * =================================================================== */

// Per-facet descriptor: human label + the offset of the gating override flag
// within ScreenConfig. Cursor still rides the legacy override_global_appearance
// flag; font and wallpaper have their own. (Offsets keep this table tiny and
// make "add a facet" a one-line change.)
typedef struct {
    const char *label;
    size_t      override_offset;   // offsetof(ScreenConfig, <flag>)
} FacetDesc;

static const FacetDesc facet_table[FACET_COUNT] = {
    [FACET_CURSOR]    = { "Appearance", offsetof(ScreenConfig, override_global_appearance) },
    [FACET_FONT]      = { "Font",       offsetof(ScreenConfig, override_font) },
    [FACET_WALLPAPER] = { "Wallpaper",  offsetof(ScreenConfig, override_wallpaper) },
};

// Map the current view to the facet it edits (only valid for pointer-views).
static AppearanceFacet facet_for_view(SettingsView v) {
    switch (v) {
        case VIEW_FONT:      return FACET_FONT;
        case VIEW_WALLPAPER: return FACET_WALLPAPER;
        case VIEW_APPEARANCE:
        default:             return FACET_CURSOR;
    }
}

// The override flag* for (screen, facet). NULL when target is global.
static bool *facet_override_ptr(int screen_idx, AppearanceFacet facet) {
    if (screen_idx < 0) return NULL;
    char *base = (char *)&state.screens[screen_idx];
    return (bool *)(base + facet_table[facet].override_offset);
}

// The AppearanceConfig the active target+facet should edit. When editing a
// screen that overrides this facet, that's the screen's own appearance; in
// every other case (global target, or screen not overriding) it's the global
// appearance. The override checkbox is rendered separately by the caller.
// Currently the selector inlines this logic; kept for views that may want the
// config pointer without drawing the selector.
__attribute__((unused))
static AppearanceConfig *active_appearance(int screen_idx, AppearanceFacet facet) {
    if (screen_idx < 0) return &state.appearance;
    bool *ov = facet_override_ptr(screen_idx, facet);
    if (ov && *ov) return &state.screens[screen_idx].appearance;
    return &state.appearance;
}

/* ---- Font UI buffers: a non-overriding screen edits the GLOBAL buffers, so
 *      selecting such a screen behaves identically to the global default. Only
 *      a screen that overrides the font facet edits its own per-screen buffers. */
static void get_active_font_ptrs(char **font_buf, float **vec_size, float **bmp_size,
                                 char **unit, bool **prefer_bitmap) {
    int t = appearance_target;
    if (t >= 0) {
        bool *fov = facet_override_ptr(t, FACET_FONT);
        if (!fov || !*fov) t = -1;   // not overriding -> fall through to global
    }

    if (t == -1) {
        if (font_buf) *font_buf = ui.font_name_buf;
        if (vec_size) *vec_size = &ui.vector_font_size;
        if (bmp_size) *bmp_size = &ui.bitmap_font_size;
        if (unit)     *unit     = ui.font_unit;
        if (prefer_bitmap) *prefer_bitmap = &ui.font_prefer_bitmap;
    } else {
        if (font_buf) *font_buf = ui.screens[t].font_name_buf;
        if (vec_size) *vec_size = &ui.screens[t].vector_font_size;
        if (bmp_size) *bmp_size = &ui.screens[t].bitmap_font_size;
        if (unit)     *unit     = ui.screens[t].font_unit;
        if (prefer_bitmap) *prefer_bitmap = &ui.screens[t].font_prefer_bitmap;
    }
}

// (used by settings.c's preview loop - keep the old name available)
void get_active_appearance_ptrs(char **font_buf, float **vec_size, float **bmp_size,
                                char **unit, bool **prefer_bitmap) {
    get_active_font_ptrs(font_buf, vec_size, bmp_size, unit, prefer_bitmap);
}

/* ===================================================================
 * Copy-on-override: when a screen's facet override flips ON, seed the screen's
 * per-facet config from the global default so the user starts from a copy of
 * what they were already seeing. One function per facet keeps the per-screen
 * copy minimal (we don't clobber the other facets' per-screen values).
 * =================================================================== */

static void copy_global_cursor_to_screen(int s) {
    ScreenConfig *sc = &state.screens[s];
    snprintf(sc->appearance.cursor_theme, sizeof(sc->appearance.cursor_theme),
             "%s", state.appearance.cursor_theme);
    sc->appearance.cursor_size = state.appearance.cursor_size;
}

static void copy_global_font_to_screen(int s) {
    ScreenConfig *sc = &state.screens[s];
    snprintf(sc->appearance.font_name, sizeof(sc->appearance.font_name),
             "%s", state.appearance.font_name);
    sc->appearance.font_size = state.appearance.font_size;
    snprintf(sc->appearance.font_unit, sizeof(sc->appearance.font_unit),
             "%s", state.appearance.font_unit);
    sc->appearance.font_prefer_bitmap = state.appearance.font_prefer_bitmap;

    // Mirror the editable UI font buffers too.
    snprintf(ui.screens[s].font_name_buf, sizeof(ui.screens[s].font_name_buf),
             "%s", ui.font_name_buf);
    snprintf(ui.screens[s].font_unit, sizeof(ui.screens[s].font_unit),
             "%s", ui.font_unit);
    ui.screens[s].vector_font_size  = ui.vector_font_size;
    ui.screens[s].bitmap_font_size  = ui.bitmap_font_size;
    ui.screens[s].font_prefer_bitmap = ui.font_prefer_bitmap;
}

static void copy_global_wallpaper_to_screen(int s) {
    ScreenConfig *sc = &state.screens[s];
    snprintf(sc->appearance.wallpaper_path, sizeof(sc->appearance.wallpaper_path),
             "%s", state.appearance.wallpaper_path);
}

static void copy_global_facet_to_screen(int s, AppearanceFacet facet) {
    switch (facet) {
        case FACET_CURSOR:    copy_global_cursor_to_screen(s);    break;
        case FACET_FONT:      copy_global_font_to_screen(s);      break;
        case FACET_WALLPAPER: copy_global_wallpaper_to_screen(s); break;
        default: break;
    }
}

/* ===================================================================
 * Shared target selector for pointer-views.
 *
 * Renders "Configure For: [Global default | Screen N]" plus, when a screen is
 * selected, the per-facet "Override Global <Facet>" checkbox (seeding on the
 * rising edge). Returns the AppearanceConfig* the view should edit, or NULL if
 * the screen is NOT overriding this facet (in which case the caller should draw
 * the "using global defaults" note and render nothing else).
 *
 * out_is_global (optional) is set true when editing the global default.
 * =================================================================== */
static AppearanceConfig *render_target_selector(struct nk_context *ctx,
                                                AppearanceFacet facet,
                                                bool *out_is_global) {
    // 1. Scope dropdown: Global + active screens, each labelled with its
    //    monitor name ("Screen 1 - LG SDQHD") to match the Display view.
    nk_layout_row_dynamic(ctx, 25, 2);
    nk_label(ctx, "Configure For:", NK_TEXT_LEFT);

    int target_map[MAX_SCREENS + 1];
    const char *target_labels[MAX_SCREENS + 1];
    static char screen_labels[MAX_SCREENS][96];

    target_map[0] = -1;
    target_labels[0] = "Global Default";
    int target_count = 1;

    for (int i = 0; i < stellar_num_screens; i++) {
        if (ui.screens[i].is_active) {
            const char *mon = monitor_info[i].monitor_name;
            if (mon && mon[0] != '\0')
                snprintf(screen_labels[i], sizeof(screen_labels[i]), "Screen %d - %s", i, mon);
            else
                snprintf(screen_labels[i], sizeof(screen_labels[i]), "Screen %d", i);
            target_map[target_count] = i;
            target_labels[target_count] = screen_labels[i];
            target_count++;
        }
    }

    int sel_idx = 0;
    for (int i = 0; i < target_count; i++)
        if (target_map[i] == appearance_target) sel_idx = i;

    int new_sel = nk_combo(ctx, target_labels, target_count, sel_idx, 25, nk_vec2(260, 220));
    if (new_sel != sel_idx) {
        appearance_target = target_map[new_sel];
        // Refresh the font preview for any new target. A non-overriding screen
        // now edits the global buffers (see get_active_font_ptrs), so the global
        // font is the right thing to resolve and the preview is always valid.
        pending_preview_update = 1;
    }

    if (out_is_global) *out_is_global = (appearance_target == -1);

    // 2. Global target. Below the combo, show which screens this facet's global
    //    value actually reaches (i.e. those NOT overriding it).
    if (appearance_target == -1) {
        char used[256];
        int used_pos = 0;
        int n_used = 0, n_active = 0;
        for (int i = 0; i < stellar_num_screens; i++) {
            if (!ui.screens[i].is_active) continue;
            n_active++;
            bool *ov = facet_override_ptr(i, facet);
            if (ov && *ov) continue;   // screen overrides this facet -> not using global
            n_used++;
            int w = snprintf(used + used_pos, sizeof(used) - used_pos,
                             "%s%d", used_pos ? ", " : "", i);
            if (w > 0 && (size_t)(used_pos + w) < sizeof(used)) used_pos += w;
        }

        nk_layout_row_dynamic(ctx, 22, 1);
        char usage[320];
        if (n_used == 0)
            snprintf(usage, sizeof(usage), "Not used by any screen (all override %s).",
                     facet_table[facet].label);
        else if (n_used == n_active)
            snprintf(usage, sizeof(usage), "Used by all screens.");
        else
            snprintf(usage, sizeof(usage), "Used by screen%s %s.",
                     n_used == 1 ? "" : "s", used);
        nk_label_colored(ctx, usage, NK_TEXT_LEFT, nk_rgb(150, 150, 150));

        return &state.appearance;
    }

    // 3. Screen target: per-facet override checkbox, with rising-edge seeding.
    bool *ov = facet_override_ptr(appearance_target, facet);
    nk_layout_row_dynamic(ctx, 25, 1);

    char ov_label[64];
    snprintf(ov_label, sizeof(ov_label), "Override Global %s", facet_table[facet].label);

    bool was = *ov;
    nk_checkbox_bool(ctx, ov_label, ov);
    if (!was != !*ov) {
        // Override toggled: seed the screen's copy from global on enable, and
        // either way the active config/buffers change, so refresh the preview.
        if (!was && *ov) copy_global_facet_to_screen(appearance_target, facet);
        if (facet == FACET_FONT) pending_preview_update = 1;
    }

    // 4. Not overriding -> behaves exactly like the global view: same inputs,
    //    editing the global config. Make that explicit so the user knows their
    //    edits here are global, not screen-local.
    if (!*ov) {
        nk_layout_row_dynamic(ctx, 22, 1);
        nk_label_colored(ctx, "Editing global appearance (shared with other screens).",
                         NK_TEXT_LEFT, nk_rgb(150, 150, 150));
        return &state.appearance;
    }

    return &state.screens[appearance_target].appearance;
}

/* ===================================================================
 * Appearance view: cursor theme + size (per-screen, FACET_CURSOR).
 * The stellar theme moved to the System view; this view is now purely cursor.
 * =================================================================== */
static void render_appearance_view(struct nk_context *ctx) {
    bool is_global;
    AppearanceConfig *ap = render_target_selector(ctx, FACET_CURSOR, &is_global);
    if (!ap) return;   // safety net; selector no longer returns NULL

    nk_layout_row_dynamic(ctx, 10, 1);
    nk_rule_horizontal(ctx, nk_rgb(100, 100, 100), nk_true);

    nk_layout_row_dynamic(ctx, 25, 1);
    nk_label(ctx, "Cursor Settings:", NK_TEXT_LEFT);

    nk_layout_row_dynamic(ctx, 25, 2);
    nk_label(ctx, "Theme Name:", NK_TEXT_LEFT);
    nk_edit_string_zero_terminated(ctx, NK_EDIT_FIELD, ap->cursor_theme,
                                   sizeof(ap->cursor_theme) - 1, nk_filter_default);
    nk_label(ctx, "Cursor Size:", NK_TEXT_LEFT);
    nk_property_int(ctx, "Size", 8, &ap->cursor_size, 128, 1, 1);

    // Cursor preview placeholder (wire actual cursor render later).
    nk_layout_row_dynamic(ctx, 10, 1);
    nk_rule_horizontal(ctx, nk_rgb(100, 100, 100), nk_true);
    nk_layout_row_dynamic(ctx, 25, 1);
    nk_label(ctx, "Preview:", NK_TEXT_LEFT);
    nk_layout_row_dynamic(ctx, 40, 1);
    nk_label_colored(ctx, "(cursor preview coming soon)", NK_TEXT_CENTERED,
                     nk_rgb(150, 150, 150));
}

/* ===================================================================
 * Font view: the full font-resolution UI (per-screen, FACET_FONT).
 * Reads/writes the per-target UI font buffers (vector/bitmap sizes) and drives
 * the preview via pending_preview_update / force_bitmap_resolve, exactly as the
 * old Appearance tab did.
 * =================================================================== */
static void render_font_view(struct nk_context *ctx) {
    bool is_global;
    AppearanceConfig *ap = render_target_selector(ctx, FACET_FONT, &is_global);
    if (!ap) return;   // safety net; selector no longer returns NULL
    (void)ap;          // font edits go through the UI buffers below, not ap directly

    char *active_font_buf;
    float *active_vec_size;
    float *active_bmp_size;
    char *active_unit;
    bool *active_prefer_bitmap;
    get_active_font_ptrs(&active_font_buf, &active_vec_size, &active_bmp_size,
                         &active_unit, &active_prefer_bitmap);

    nk_layout_row_dynamic(ctx, 10, 1);
    nk_rule_horizontal(ctx, nk_rgb(100, 100, 100), nk_true);

    nk_layout_row_dynamic(ctx, 25, 1);
    nk_label(ctx, "System Font:", NK_TEXT_LEFT);

    // Predictive partial match against installed families.
    int sel = -1;
    size_t active_len = strlen(active_font_buf);
    if (font_families && font_family_count > 0 && active_len > 0) {
        for (int f = 0; f < font_family_count; f++) {
            if (strncasecmp(font_families[f], active_font_buf, active_len) == 0) {
                sel = f;
                break;
            }
        }
    }

    const char *combo_label = "Browse Installed Fonts...";
    if (sel >= 0)               combo_label = font_families[sel];
    else if (active_len > 0)    combo_label = active_font_buf;

    if (font_families && font_family_count > 0) {
        static int combo_was_open = 0;
        int combo_is_open = nk_combo_begin_label(ctx, combo_label, nk_vec2(350, 400));
        if (combo_is_open) {
            if (!combo_was_open && sel >= 0) {
                float row_h = 25.0f + ctx->style.window.spacing.y;
                float target_y = (sel * row_h) - (400.0f / 2.0f) + (row_h / 2.0f);
                nk_window_set_scroll(ctx, 0, target_y > 0.0f ? (nk_uint)target_y : 0);
            }
            nk_layout_row_dynamic(ctx, 25, 1);
            for (int f = 0; f < font_family_count; f++) {
                if (nk_combo_item_label(ctx, font_families[f], NK_TEXT_LEFT)) {
                    snprintf(active_font_buf, 128, "%s", font_families[f]);
                    pending_preview_update = 1;
                }
            }
            nk_combo_end(ctx);
        }
        combo_was_open = combo_is_open;
    }

    // Free-form field for aliases ("monospace") or anything the dropdown missed.
    static char last_edit_font[128] = "";
    if (pending_preview_update)
        snprintf(last_edit_font, sizeof(last_edit_font), "%s", active_font_buf);

    int edit_flags = nk_edit_string_zero_terminated(ctx, NK_EDIT_FIELD,
        active_font_buf, 128 - 1, nk_filter_default);
    if (edit_flags & (NK_EDIT_COMMITED | NK_EDIT_DEACTIVATED)) {
        if (strcmp(last_edit_font, active_font_buf) != 0) {
            snprintf(last_edit_font, sizeof(last_edit_font), "%s", active_font_buf);
            pending_preview_update = 1;
        }
    }

    if (preview_resolved) {
        const char *variant_note = "";
        if (current_font_has_bitmap && current_font_has_vector) {
            variant_note = preview_info.is_bitmap
                ? " - vector also available"
                : " - bitmap also available";
        }

        char label[128];
        snprintf(label, sizeof(label), preview_info.is_bitmap
            ? "Font Size: (native px)%s"
            : "Font Size: (pt)%s",
            variant_note);

        nk_label(ctx, label, NK_TEXT_LEFT);
        nk_layout_row_dynamic(ctx, 25, 2);

        if (preview_info.is_bitmap) {
            if (current_strike_count > 1 && current_strike_labels) {
                int ssel = 0;
                for (int i = 0; i < current_strike_count; i++) {
                    if (fabs(current_strikes[i] - preview_info.size_px) < 0.1f) ssel = i;
                }
                int new_sel = nk_combo(ctx, (const char **)current_strike_labels,
                                       current_strike_count, ssel, 25, nk_vec2(200, 200));
                if (new_sel != ssel) {
                    *active_bmp_size = current_strikes[new_sel];
                    force_bitmap_resolve = 1;
                    pending_preview_update = 1;
                }
            } else {
                char locked_label[64];
                snprintf(locked_label, sizeof(locked_label), "%.0f px (Locked)",
                         preview_info.size_px);
                nk_label(ctx, locked_label, NK_TEXT_LEFT);
            }
        } else {
            float old_size = *active_vec_size;
            nk_property_float(ctx, "Font Size", 6, active_vec_size, 64, 1, 1);
            if (*active_vec_size != old_size)
                pending_preview_update = 1;
        }
    }

    // Bitmap/vector toggle, only when both variants exist.
    if (current_font_has_bitmap && current_font_has_vector) {
        nk_layout_row_dynamic(ctx, 25, 1);
        int pref = *active_prefer_bitmap ? 1 : 0;
        int old_pref = pref;
        nk_checkbox_label(ctx, "Prefer bitmap rendering (pixel-perfect)", &pref);
        if (pref != old_pref) {
            *active_prefer_bitmap = (pref != 0);
            if (pref) force_bitmap_resolve = 1;
            else      snprintf(active_unit, 8, "pt");
            pending_preview_update = 1;
        }
    }

    // Preview.
    nk_layout_row_dynamic(ctx, 20, 1);
    nk_label(ctx, " ", NK_TEXT_LEFT);
    nk_layout_row_dynamic(ctx, 25, 1);
    nk_label(ctx, "Preview:", NK_TEXT_LEFT);
    nk_layout_row_dynamic(ctx, 60, 1);
    if (preview_font) {
        nk_style_push_font(ctx, preview_font);
        nk_label(ctx, "Quick vermilion dragons vex furious cyborg wizards beyond the Jovian starport.",
                 NK_TEXT_CENTERED);
        nk_style_pop_font(ctx);
    }
}

/* ===================================================================
 * Wallpaper view (per-screen, FACET_WALLPAPER).
 * NOTE: the old monolith wrote the chosen path into state.term_gui by mistake;
 * here it correctly writes ap->wallpaper_path for the active target.
 * =================================================================== */
static void render_wallpaper_view(struct nk_context *ctx) {
    bool is_global;
    AppearanceConfig *ap = render_target_selector(ctx, FACET_WALLPAPER, &is_global);
    if (!ap) return;   // safety net; selector no longer returns NULL

    nk_layout_row_dynamic(ctx, 10, 1);
    nk_rule_horizontal(ctx, nk_rgb(100, 100, 100), nk_true);

    nk_layout_row_dynamic(ctx, 25, 1);
    nk_label(ctx, "Wallpaper image:", NK_TEXT_LEFT);

    nk_layout_row_dynamic(ctx, 25, 2);
    nk_edit_string_zero_terminated(ctx, NK_EDIT_FIELD,
        ap->wallpaper_path, sizeof(ap->wallpaper_path) - 1, nk_filter_default);

    if (nk_button_label(ctx, "Select from filesystem")) {
        char fileselect_path[PATH_MAX];
        snprintf(fileselect_path, sizeof(fileselect_path),
                 "%s/stellar-fileselect", STELLAR_LIBEXEC_PATH);

        char cmd[2048];
        snprintf(cmd, sizeof(cmd),
                 "%s --title \"Select wallpaper image\" --current-name \"%s\"",
                 fileselect_path, ap->wallpaper_path);

        FILE *fp = popen(cmd, "r");
        if (fp != NULL) {
            char temp_buffer[1024];
            char final_path[1024] = {0};
            while (fgets(temp_buffer, sizeof(temp_buffer), fp) != NULL) {
                strncpy(final_path, temp_buffer, sizeof(final_path) - 1);
                final_path[sizeof(final_path) - 1] = '\0';
            }
            int status = pclose(fp);
            if (WIFEXITED(status) && WEXITSTATUS(status) == 0) {
                size_t len = strlen(final_path);
                if (len > 0 && final_path[len - 1] == '\n')
                    final_path[len - 1] = '\0';
                if (strlen(final_path) > 0) {
                    strncpy(ap->wallpaper_path, final_path,
                            sizeof(ap->wallpaper_path) - 1);
                    ap->wallpaper_path[sizeof(ap->wallpaper_path) - 1] = '\0';
                }
            }
        }
    }

    // Wallpaper preview placeholder (render the image thumbnail later).
    nk_layout_row_dynamic(ctx, 10, 1);
    nk_rule_horizontal(ctx, nk_rgb(100, 100, 100), nk_true);
    nk_layout_row_dynamic(ctx, 25, 1);
    nk_label(ctx, "Preview:", NK_TEXT_LEFT);
    nk_layout_row_dynamic(ctx, 60, 1);
    nk_label_colored(ctx, "(wallpaper preview coming soon)", NK_TEXT_CENTERED,
                     nk_rgb(150, 150, 150));
}

/* ===================================================================
 * System view: session-identity settings that are NOT per-screen.
 * Terminal emulator + login shell (unchanged from the old Terminal view),
 * then the Stellar theme selector + a stubbed theme preview.
 * =================================================================== */
static void render_system_view(struct nk_context *ctx) {
    // ---- Terminal GUI app ----
    nk_layout_row_dynamic(ctx, 25, 1);
    nk_label(ctx, "Terminal Emulator:", NK_TEXT_LEFT);

    static const char *avail_terms[16];
    static int avail_term_count = -1;
    if (avail_term_count < 0) {
        avail_term_count = 0;
        for (int i = 0; known_terminals[i] &&
                        avail_term_count < (int)(sizeof(avail_terms)/sizeof(avail_terms[0])); i++) {
            if (is_executable_in_path(known_terminals[i]))
                avail_terms[avail_term_count++] = known_terminals[i];
        }
    }

    int term_sel = -1;
    for (int i = 0; i < avail_term_count; i++)
        if (strcmp(state.term_gui, avail_terms[i]) == 0) { term_sel = i; break; }

    const char *term_label;
    if (term_sel >= 0)                  term_label = avail_terms[term_sel];
    else if (state.term_gui[0] != '\0') term_label = state.term_gui;
    else                                term_label = "Select terminal...";

    if (avail_term_count > 0) {
        int popup_h = (avail_term_count + 1) * 30;
        if (popup_h > 300) popup_h = 300;
        if (nk_combo_begin_label(ctx, term_label, nk_vec2(300, popup_h))) {
            nk_layout_row_dynamic(ctx, 25, 1);
            for (int i = 0; i < avail_term_count; i++) {
                if (nk_combo_item_label(ctx, avail_terms[i], NK_TEXT_LEFT))
                    snprintf(state.term_gui, sizeof(state.term_gui), "%s", avail_terms[i]);
            }
            nk_combo_end(ctx);
        }
    } else {
        nk_label(ctx, "(No known terminals found in $PATH)", NK_TEXT_LEFT);
    }

    nk_layout_row_dynamic(ctx, 25, 1);
    nk_label(ctx, "Or enter a terminal command manually:", NK_TEXT_LEFT);
    nk_edit_string_zero_terminated(ctx, NK_EDIT_FIELD,
        state.term_gui, sizeof(state.term_gui) - 1, nk_filter_default);

    nk_layout_row_dynamic(ctx, 10, 1);
    nk_label(ctx, "", NK_TEXT_LEFT);

    // ---- Login shell (full path) ----
    nk_layout_row_dynamic(ctx, 25, 1);
    nk_label(ctx, "Shell:", NK_TEXT_LEFT);

    static char avail_shells[24][256];
    static int avail_shell_count = -1;
    if (avail_shell_count < 0) {
        avail_shell_count = 0;
        FILE *sf = fopen("/etc/shells", "r");
        if (sf) {
            char line[256];
            while (fgets(line, sizeof(line), sf) &&
                   avail_shell_count < (int)(sizeof(avail_shells)/sizeof(avail_shells[0]))) {
                char *s = line;
                while (*s == ' ' || *s == '\t') s++;
                if (*s == '#' || *s == '\n' || *s == '\0') continue;
                s[strcspn(s, "\r\n")] = '\0';
                if (access(s, X_OK) != 0) continue;
                int dup = 0;
                for (int i = 0; i < avail_shell_count; i++)
                    if (strcmp(avail_shells[i], s) == 0) { dup = 1; break; }
                if (!dup)
                    snprintf(avail_shells[avail_shell_count++], 256, "%s", s);
            }
            fclose(sf);
        }
    }

    int shell_sel = -1;
    for (int i = 0; i < avail_shell_count; i++)
        if (strcmp(state.term_shell, avail_shells[i]) == 0) { shell_sel = i; break; }

    const char *shell_label;
    if (shell_sel >= 0)                   shell_label = avail_shells[shell_sel];
    else if (state.term_shell[0] != '\0') shell_label = state.term_shell;
    else                                  shell_label = "Select shell...";

    if (avail_shell_count > 0) {
        int popup_h = (avail_shell_count + 1) * 30;
        if (popup_h > 300) popup_h = 300;
        if (nk_combo_begin_label(ctx, shell_label, nk_vec2(300, popup_h))) {
            nk_layout_row_dynamic(ctx, 25, 1);
            for (int i = 0; i < avail_shell_count; i++) {
                if (nk_combo_item_label(ctx, avail_shells[i], NK_TEXT_LEFT))
                    snprintf(state.term_shell, sizeof(state.term_shell), "%s", avail_shells[i]);
            }
            nk_combo_end(ctx);
        }
    } else {
        nk_label(ctx, "(/etc/shells unavailable)", NK_TEXT_LEFT);
    }

    nk_layout_row_dynamic(ctx, 25, 1);
    nk_label(ctx, "Or enter a shell path manually:", NK_TEXT_LEFT);
    nk_edit_string_zero_terminated(ctx, NK_EDIT_FIELD,
        state.term_shell, sizeof(state.term_shell) - 1, nk_filter_default);

    // ---- Stellar theme (session-level identity, not per-screen) ----
    nk_layout_row_dynamic(ctx, 10, 1);
    nk_rule_horizontal(ctx, nk_rgb(100, 100, 100), nk_true);

    nk_layout_row_dynamic(ctx, 25, 1);
    nk_label(ctx, "Stellar Theme:", NK_TEXT_LEFT);
    nk_layout_row_dynamic(ctx, 25, 1);
    nk_edit_string_zero_terminated(ctx, NK_EDIT_FIELD,
        state.stellar_theme, sizeof(state.stellar_theme) - 1, nk_filter_default);

    // Theme preview placeholder: will show the theme's window + button PNGs.
    nk_layout_row_dynamic(ctx, 25, 1);
    nk_label(ctx, "Preview:", NK_TEXT_LEFT);
    nk_layout_row_dynamic(ctx, 60, 1);
    nk_label_colored(ctx, "(theme preview coming soon)", NK_TEXT_CENTERED,
                     nk_rgb(150, 150, 150));
}

/* ===================================================================
 * Display view: the all-screens topology grid + per-screen display config +
 * global power management. Unchanged from the monolith; this is the one view
 * that is intentionally NOT a per-screen pointer-view (it must show every
 * screen at once to edit their spatial relationship).
 * =================================================================== */
static void render_display_view(struct nk_context *ctx) {
                nk_layout_row_dynamic(ctx, 25, 2);
                nk_label(ctx, "Screen Arrangement (Dynamic Topology):", NK_TEXT_LEFT);
                if (nk_button_label(ctx, "Refresh Monitor Info")) {
                    request_screen_info();
                }

                // Calculate the active grid bounds
                int max_r = 0;
                int max_c = 0;
                for (int r = 0; r < MAX_SCREENS; r++) {
                    for (int c = 0; c < MAX_SCREENS; c++) {
                        if (screen_grid[r][c] != -1) {
                            if (r > max_r) max_r = r;
                            if (c > max_c) max_c = c;
                        }
                    }
                }

                int active_rows = max_r + 1;
                int active_cols = max_c + 1;

                // Calculate dynamic height to keep the total diagram space constant
                const float TOTAL_DIAGRAM_HEIGHT = 330.0f; 
                float dynamic_row_height = TOTAL_DIAGRAM_HEIGHT / (float)active_rows;

                // Ensure the row height doesn't shrink so small that the buttons become unclickable
                if (dynamic_row_height < 80.0f) dynamic_row_height = 80.0f; 

                // Draw only the active bounds
                for (int y = 0; y < active_rows; y++) {
                    // Nuklear automatically divides the window width by active_cols
                    nk_layout_row_dynamic(ctx, dynamic_row_height, active_cols); 
                    
                    for (int x = 0; x < active_cols; x++) {
                        int screen_id = screen_grid[y][x];
                        char box_id[32];
                        snprintf(box_id, sizeof(box_id), "grid_%d_%d", y, x);

                        if (nk_group_begin(ctx, box_id, NK_WINDOW_BORDER | NK_WINDOW_NO_SCROLLBAR)) {
                            
                            if (screen_id == -1) {
                                // Empty slot within the active bounds (e.g., an L-shape layout)
                                // We just draw a blank space
                                nk_layout_space_begin(ctx, NK_STATIC, 10, 1);
                                nk_layout_space_end(ctx); 
                            } else {
                                char label[16];
                                snprintf(label, sizeof(label), "Scr %d", screen_id);

                                // Scale button sizes slightly based on available height
                                float btn_h = (dynamic_row_height > 100.0f) ? 22.0f : 16.0f;

                                // Top Arrow Row
                                nk_layout_row_dynamic(ctx, btn_h, 3);
                                nk_spacing(ctx, 1);
                                if (y > 0 && nk_button_label(ctx, "^")) {
									try_move_screen(y, x, -1, 0); // Move Up
                                } else { nk_spacing(ctx, 1); }
                                nk_spacing(ctx, 1);

                                // Middle Row
                                nk_layout_row_dynamic(ctx, btn_h, 3);
                                if (x > 0 && nk_button_label(ctx, "<")) {
									try_move_screen(y, x, 0, -1); // Move Left
                                } else { nk_spacing(ctx, 1); }
                                
                                nk_label(ctx, label, NK_TEXT_CENTERED);
                                
                                // Expand Right (Allows pushing past the current active bound)
                                if (x < MAX_SCREENS - 1 && nk_button_label(ctx, ">")) {
									try_move_screen(y, x, 0, 1);  // Move Right
                                } else { nk_spacing(ctx, 1); }

                                // Bottom Arrow Row (Allows pushing past the current active bound)
                                nk_layout_row_dynamic(ctx, btn_h, 3);
                                nk_spacing(ctx, 1);
                                if (y < MAX_SCREENS - 1 && nk_button_label(ctx, "v")) {
									try_move_screen(y, x, 1, 0);  // Move Down
                                } else { nk_spacing(ctx, 1); }
                                nk_spacing(ctx, 1);
                            }
                            nk_group_end(ctx);
                        }
                    }
                }

                // --- PER-SCREEN CONFIGURATION ---
                nk_layout_row_dynamic(ctx, 25, 1);
                nk_label(ctx, " ", NK_TEXT_LEFT); // Spacer
                nk_label(ctx, "Per-Screen Configuration:", NK_TEXT_LEFT);

                for (int i = 0; i < stellar_num_screens; i++) {
                    ScreenConfig *sc = &state.screens[i];
                    char label[96];
                    if (monitor_info_loaded && monitor_info[i].connected) {
                        snprintf(label, sizeof(label), "Screen %d - %s", i, monitor_info[i].monitor_name);
                    } else {
                        snprintf(label, sizeof(label), "Screen %d Settings", i);
                    }

                    if (nk_tree_push(ctx, NK_TREE_TAB, label, NK_MINIMIZED)) {
                        // --- Monitor Info (read-only, from IPC) ---
                        if (monitor_info_loaded) {
                            MonitorInfo *mi = &monitor_info[i];
                            char info_line[128];

                            nk_layout_row_dynamic(ctx, 20, 1);

                            if (mi->connected) {
                                snprintf(info_line, sizeof(info_line),
                                    "Monitor: %s (%s)", mi->monitor_name, mi->output_name);
                                nk_label(ctx, info_line, NK_TEXT_LEFT);

                                if (mi->gpu_name[0] != '\0') {
                                    snprintf(info_line, sizeof(info_line), "GPU: %s", mi->gpu_name);
                                    nk_label(ctx, info_line, NK_TEXT_LEFT);
                                }

                                snprintf(info_line, sizeof(info_line),
                                    "Resolution: %dx%d @ %.1f Hz  |  Rotation: %s",
                                    mi->width, mi->height,
                                    mi->refresh_mhz / 1000.0,
                                    mi->rotation[0] ? mi->rotation : "normal");
                                nk_label(ctx, info_line, NK_TEXT_LEFT);

                                snprintf(info_line, sizeof(info_line),
                                    "Physical: %dx%d mm  |  %d DPI",
                                    mi->phys_width_mm, mi->phys_height_mm, mi->dpi);
                                nk_label(ctx, info_line, NK_TEXT_LEFT);
                            } else {
                                nk_label(ctx, "Monitor: Not Connected", NK_TEXT_LEFT);
                            }

                            nk_label(ctx, " ", NK_TEXT_LEFT);
                        }

                        nk_layout_row_dynamic(ctx, 25, 2);

                        nk_label(ctx, "Physical Scale:", NK_TEXT_LEFT);
                        nk_property_float(ctx, "Physical Scale", 0.1f, &sc->phys_scale, 1.0f, 0.01f, 0.1f);
                        
                        nk_label(ctx, "Physical Offset:", NK_TEXT_LEFT);
                        nk_property_float(ctx, "Physical Offset", -0.5f, &sc->phys_offset, 0.5f, 0.01f, 0.1f);

                        nk_label(ctx, "Rotation:", NK_TEXT_LEFT);
                        {
                            int rot_idx = rotation_string_to_index(sc->rotation);
                            int new_idx = nk_combo(ctx, rotation_labels, 4, rot_idx, 25, nk_vec2(200, 120));
                            if (new_idx != rot_idx) {
                                snprintf(sc->rotation, sizeof(sc->rotation), "%s", rotation_values[new_idx]);
                                // Rotation feeds the generated xorg.conf; update
                                // the dirty-state banner immediately.
                                refresh_pending_xorg_change();
                            }
                        }

                        nk_label(ctx, "Preferred Mode:", NK_TEXT_LEFT);
                        if (monitor_info_loaded && monitor_info[i].mode_count > 0) {
                            MonitorInfo *mi_modes = &monitor_info[i];
 
                            // Build label array: "Auto" + each available mode
                            const char *mode_labels[MAX_MONITOR_MODES + 1];
                            mode_labels[0] = "Auto (native)";
                            for (int m = 0; m < mi_modes->mode_count; m++) {
                                mode_labels[m + 1] = mi_modes->modes[m].label;
                            }
 
                            // Find current selection
                            int sel = 0;
                            for (int m = 0; m < mi_modes->mode_count; m++) {
                                if (strcmp(sc->preferred_mode, mi_modes->modes[m].value) == 0) {
                                    sel = m + 1;
                                    break;
                                }
                            }
 
                            int new_sel = nk_combo(ctx, mode_labels, mi_modes->mode_count + 1,
                                                   sel, 25, nk_vec2(250, 200));
                            if (new_sel != sel) {
                                if (new_sel == 0) {
                                    sc->preferred_mode[0] = '\0';
                                } else {
                                    snprintf(sc->preferred_mode, sizeof(sc->preferred_mode),
                                             "%s", mi_modes->modes[new_sel - 1].value);
                                }
                                // Preferred mode feeds the generated xorg.conf.
                                refresh_pending_xorg_change();
                            }
                        } else {
                            nk_label(ctx, "(no mode data)", NK_TEXT_LEFT);
                        }

                        nk_label(ctx, "DPI:", NK_TEXT_LEFT);
                        {
                            int auto_dpi = 96;
                            if (monitor_info_loaded && monitor_info[i].connected) {
                                auto_dpi = calc_auto_dpi(monitor_info[i].width, monitor_info[i].phys_width_mm);
                            }
 
                            // Build label array: "Auto (N DPI)" + each step
                            char auto_label[32];
                            snprintf(auto_label, sizeof(auto_label), "Auto (%d DPI)", auto_dpi);
 
                            const char *dpi_labels[NUM_DPI_STEPS + 1];
                            char dpi_step_labels[NUM_DPI_STEPS][16];
                            dpi_labels[0] = auto_label;
                            for (int d = 0; d < NUM_DPI_STEPS; d++) {
                                snprintf(dpi_step_labels[d], sizeof(dpi_step_labels[d]), "%d DPI", dpi_steps[d]);
                                dpi_labels[d + 1] = dpi_step_labels[d];
                            }
 
                            // Find current selection
                            int sel = 0;
                            if (sc->dpi_override > 0) {
                                for (int d = 0; d < NUM_DPI_STEPS; d++) {
                                    if (dpi_steps[d] == sc->dpi_override) {
                                        sel = d + 1;
                                        break;
                                    }
                                }
                            }
 
                            int new_sel = nk_combo(ctx, dpi_labels, NUM_DPI_STEPS + 1,
                                                   sel, 25, nk_vec2(200, 200));
                            if (new_sel == 0) {
                                sc->dpi_override = 0;
                            } else {
                                sc->dpi_override = dpi_steps[new_sel - 1];
                            }
                        }

                        nk_layout_row_dynamic(ctx, 25, 2);
                        nk_checkbox_bool(ctx, "Enable Compositor", &sc->picom_enabled);
                        nk_checkbox_bool(ctx, "Enable System Tray", &sc->tray_enabled);
                        nk_checkbox_bool(ctx, "Independent DPMS", &sc->independent_dpms);
                        nk_checkbox_bool(ctx, "Require Explicit Wake", &sc->require_explicit_wake);

                        nk_tree_pop(ctx);
                    }
                }

                // --- GLOBAL POWER MANAGEMENT ---
                nk_layout_row_dynamic(ctx, 25, 1);
                nk_label(ctx, " ", NK_TEXT_LEFT); // Spacer
                nk_label(ctx, "Global Power Management:", NK_TEXT_LEFT);

                nk_checkbox_bool(ctx, "Enable Screensaver", &state.saver_enabled);

                nk_layout_row_dynamic(ctx, 25, 2);
                nk_label(ctx, "Screensaver Timeout (minutes):", NK_TEXT_LEFT);
                nk_property_int(ctx, "Screensaver Timeout", 0, &state.power.timeout_screensaver, 300, 1, 1);

                nk_label(ctx, "DPMS Timeout (minutes):", NK_TEXT_LEFT);
                nk_property_int(ctx, "DPMS Timeout", 0, &state.power.timeout_dpms, 300, 1, 1);
}

/* ===================================================================
 * Window Rules view (stub). Not per-screen; per-rule screen options will live
 * inside individual rules when this is built out.
 * =================================================================== */
static void render_rules_view(struct nk_context *ctx) {
    nk_layout_row_dynamic(ctx, 25, 1);
    nk_label(ctx, "Window Rules will go here.", NK_TEXT_LEFT);
}

/* ===================================================================
 * commit_font: fold the UI font buffers into persistent AppearanceConfig
 * on Save. Uses the live preview resolution for the actively-previewed
 * target so a snapped bitmap strike is written exactly.
 * =================================================================== */
static void commit_font(char *src_name, float vec_sz, float bmp_sz, char *src_unit,
                        bool src_prefer_bitmap,
                        char *dst_name, float *dst_sz, char *dst_unit,
                        bool *dst_prefer_bitmap, bool is_active_preview) {
    snprintf(dst_name, 128, "%s", src_name);
    *dst_prefer_bitmap = src_prefer_bitmap;
    // If this is the font currently in the preview window, use the exact preview resolution
    if (is_active_preview && preview_resolved && preview_info.is_bitmap) {
        *dst_sz = preview_info.size_px;
        snprintf(dst_unit, 8, "px");
        snprintf(src_unit, 8, "px");
    } else {
        // Otherwise, rely on the cached unit (defaults to pt if never resolved as a bitmap)
        if (strcmp(src_unit, "px") == 0) {
            *dst_sz = bmp_sz;
            snprintf(dst_unit, 8, "px");
        } else {
            *dst_sz = vec_sz;
            snprintf(dst_unit, 8, "pt");
        }
    }
}

/* ---- extra cross-unit fn used by the banner ---- */
int check_full_mismatch(void);

/* ===================================================================
 * Hardware / unsaved-display banner. Shown on every view.
 * =================================================================== */
static void render_hardware_banner(struct nk_context *ctx) {
    bool any = hardware_mismatch_detected || (pending_xorg_change != XORG_CHANGE_NONE);
    if (!any) return;

    if (hardware_mismatch_detected) {
        nk_layout_row_dynamic(ctx, 30, 1);
        nk_label_colored(ctx, "Warning: display hardware has changed or probe data is missing.",
                         NK_TEXT_CENTERED, nk_rgb(255, 100, 100));

        nk_layout_row_dynamic(ctx, 30, 2);
        if (nk_button_label(ctx, "Run Hardware Probe")) {
            char helper_cmd[PATH_MAX];
            snprintf(helper_cmd, sizeof(helper_cmd),
                     "pkexec %s/stellar-admin-helper --probe",
                     STELLAR_LIBEXEC_PATH);

            int ret = system(helper_cmd);

            // If the user didn't cancel the Polkit prompt, refresh live monitor
            // info from the just-updated cache, then re-evaluate. Refreshing
            // first lets the per-output EDID swap check run against current data.
            if (ret == 0) {
                request_screen_info();
                hardware_mismatch_detected = check_full_mismatch();
                // A fresh probe rewrites the cache, which changes what
                // generate_xorg_conf() produces - re-evaluate the dirty state.
                refresh_pending_xorg_change();
                // Jump to the display arrangement so the user can lay out any
                // monitors the fresh probe surfaced.
                current_view = VIEW_DISPLAY;
            }
        }
        if (nk_button_label(ctx, "Ignore")) {
            hardware_mismatch_detected = 0;
        }
    }

    if (pending_xorg_change != XORG_CHANGE_NONE) {
        nk_layout_row_dynamic(ctx, 30, 1);
        if (pending_xorg_change == XORG_CHANGE_STRUCTURAL) {
            nk_label_colored(ctx,
                "Unsaved display changes: the screen layout differs from the active configuration (restart required).",
                NK_TEXT_CENTERED, nk_rgb(255, 180, 80));
        } else {
            nk_label_colored(ctx,
                "Unsaved display changes: rotation/mode settings aren't yet written to the display configuration.",
                NK_TEXT_CENTERED, nk_rgb(255, 210, 110));
        }

        nk_layout_row_dynamic(ctx, 30, 1);
        if (nk_button_label(ctx, "Save Display Configuration")) {
            bool needs_restart = false;
            apply_display_config(&needs_restart);
            if (needs_restart) requires_restart = true;
        }
    }

    nk_layout_row_dynamic(ctx, 10, 1);
    nk_rule_horizontal(ctx, nk_rgb(100, 100, 100), nk_true);
}

/* ===================================================================
 * Top-level frame: sidebar (tab buttons + Save & Apply) and the content pane
 * that dispatches to the active view's renderer.
 * =================================================================== */
void process_frame(struct nk_context *ctx, int os_win_width, int os_win_height) {
    const float SIDEBAR_PCT = 0.25f;
    const int SIDEBAR_MIN_WIDTH = 150;
    const int SIDEBAR_MAX_WIDTH = 300;

    int sidebar_width = (int)(os_win_width * SIDEBAR_PCT);
    if (sidebar_width < SIDEBAR_MIN_WIDTH) sidebar_width = SIDEBAR_MIN_WIDTH;
    if (sidebar_width > SIDEBAR_MAX_WIDTH) sidebar_width = SIDEBAR_MAX_WIDTH;

    int content_width = os_win_width - sidebar_width;

    // Sidebar
    if (nk_begin(ctx, "Sidebar", nk_rect(0, 0, sidebar_width, os_win_height), NK_WINDOW_BORDER)) {
        nk_layout_row_dynamic(ctx, 30, 1);
        if (sidebar_tab(ctx, "Appearance",   VIEW_APPEARANCE)) current_view = VIEW_APPEARANCE;
        if (sidebar_tab(ctx, "Font",          VIEW_FONT))       current_view = VIEW_FONT;
        if (sidebar_tab(ctx, "Wallpaper",     VIEW_WALLPAPER))  current_view = VIEW_WALLPAPER;
        if (sidebar_tab(ctx, "System",        VIEW_SYSTEM))     current_view = VIEW_SYSTEM;
        if (sidebar_tab(ctx, "Display",       VIEW_DISPLAY))    current_view = VIEW_DISPLAY;
        if (sidebar_tab(ctx, "Window Rules",  VIEW_RULES))      current_view = VIEW_RULES;

        nk_layout_space_begin(ctx, NK_STATIC, os_win_height - 260, 1);
        nk_layout_space_end(ctx);

        nk_layout_row_dynamic(ctx, 30, 1);
        if (nk_button_label(ctx, "Save & Apply")) {
            // Keep the legacy single flag in sync with the font facet so the DE
            // (which still reads override_global_appearance for font) doesn't
            // regress until its accessors are migrated to override_font.
            for (int i = 0; i < stellar_num_screens; i++) {
                if (!ui.screens[i].is_active) continue;
                if (state.screens[i].override_font)
                    state.screens[i].override_global_appearance = true;
            }

            // Commit Global font
            commit_font(ui.font_name_buf, ui.vector_font_size, ui.bitmap_font_size, ui.font_unit,
                        ui.font_prefer_bitmap,
                        state.appearance.font_name, &state.appearance.font_size, state.appearance.font_unit,
                        &state.appearance.font_prefer_bitmap,
                        appearance_target == -1);

            // Commit all Screens' fonts
            for (int i = 0; i < stellar_num_screens; i++) {
                if (!ui.screens[i].is_active) continue;
                commit_font(ui.screens[i].font_name_buf, ui.screens[i].vector_font_size,
                            ui.screens[i].bitmap_font_size, ui.screens[i].font_unit,
                            ui.screens[i].font_prefer_bitmap,
                            state.screens[i].appearance.font_name, &state.screens[i].appearance.font_size,
                            state.screens[i].appearance.font_unit,
                            &state.screens[i].appearance.font_prefer_bitmap,
                            appearance_target == i);
            }

            bool needs_restart = false;
            apply_display_config(&needs_restart);
            if (needs_restart) requires_restart = true;

            save_and_reload();
            pending_theme_apply = 1;
        }
    }
    nk_end(ctx);

    // Content pane
    if (nk_begin(ctx, "Content",
                 nk_rect(sidebar_width, 0, content_width, os_win_height),
                 NK_WINDOW_BORDER)) {
        // Mismatch banner shows on every view, not just Display.
        render_hardware_banner(ctx);

        // Centered view title at the top of every content pane.
        render_view_title(ctx);

        switch (current_view) {
            case VIEW_APPEARANCE: render_appearance_view(ctx); break;
            case VIEW_FONT:       render_font_view(ctx);       break;
            case VIEW_WALLPAPER:  render_wallpaper_view(ctx);  break;
            case VIEW_SYSTEM:     render_system_view(ctx);     break;
            case VIEW_DISPLAY:    render_display_view(ctx);    break;
            case VIEW_RULES:      render_rules_view(ctx);      break;
        }
    }
    nk_end(ctx);
}
