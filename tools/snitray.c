/*
 * stellar-snitray
 *
 * A zaphod-aware C port of snixembed for the Stellar desktop environment.
 *
 * Bridges StatusNotifierItem (SNI / appindicator) tray icons to XEmbed
 * system tray icons, creating each icon window on the X screen that the
 * owning application's windows live on, so the icon lands in the correct
 * per-screen stalonetray instance.
 *
 * Dependencies: libsystemd (sd-bus), Xlib, cairo (cairo-xlib).
 * No GTK, no glib.
 *
 * Build:
 *   cc -O2 -Wall -o stellar-snitray stellar-snitray.c \
 *      $(pkg-config --cflags --libs libsystemd x11 cairo)
 *
 * Environment knobs:
 *   STELLAR_SNI_DEFAULT_SCREEN  fallback screen when pid->screen fails (default 0)
 *   STELLAR_SNI_TINT            none | gray | color   (icon filter, default none)
 *   STELLAR_SNI_TINT_COLOR      RRGGBB hex used when STELLAR_SNI_TINT=color
 *   STELLAR_SNI_ICON_DIR        override icon dir (default $XDG_CONFIG_HOME/stellar/sni-icons)
 *
 * Icon substitution: drop a PNG named "<Id>.png" (the SNI Id property, e.g.
 * "telegram.png") into the override icon dir and it is used instead of
 * whatever the app ships, before any tint filter runs.
 */

#define _GNU_SOURCE
#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <limits.h>
#include <math.h>
#include <poll.h>
#include <signal.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <sys/types.h>
#include <time.h>
#include <unistd.h>

#include <systemd/sd-bus.h>

#include <X11/Xlib.h>
#include <X11/Xatom.h>
#include <X11/Xutil.h>

#include <cairo.h>
#include <cairo-xlib.h>

/* Stellar theme/appearance (transport half only - no nuklear).  Link with
 * stellar_theme.c, stellar_font.c, stellar_config.c and cJSON.c. */
#include "stellar_theme.h"

/* --------------------------------------------------------------------
 * Configuration
 * -------------------------------------------------------------------- */

#define SNI_WATCHER_NAME   "org.kde.StatusNotifierWatcher"
#define SNI_WATCHER_OBJECT "/StatusNotifierWatcher"
#define SNI_ITEM_IFACE     "org.kde.StatusNotifierItem"
#define SNI_ITEM_OBJECT    "/StatusNotifierItem"
#define DBUSMENU_IFACE     "com.canonical.dbusmenu"

#define MAX_SNI_SCREENS    16
#define DEFAULT_ICON_SIZE  24
#define RESOLVE_RETRY_MS   2000     /* pid->screen retry interval        */
#define RESOLVE_MAX_TRIES  15       /* ~30s of retries after registering */
#define DOCK_RETRY_MS      2000     /* selection-owner poll while undocked */
#define TOOLTIP_DELAY_MS   600

#define SYSTEM_TRAY_REQUEST_DOCK    0
#define XEMBED_MAPPED               (1 << 0)

/* --------------------------------------------------------------------
 * Types
 * -------------------------------------------------------------------- */

typedef struct {
    int32_t  w, h;
    uint8_t *data;          /* ARGB32, network byte order, w*h*4 bytes */
} RawPixmap;

typedef enum {
    TINT_NONE = 0,
    TINT_GRAY,
    TINT_COLOR
} TintMode;

typedef struct MenuNode {
    int32_t  id;
    char     label[256];
    char     type[32];          /* "standard" | "separator"            */
    char     toggle_type[16];   /* "" | "checkmark" | "radio"          */
    int      toggle_state;      /* -1 none, 0 off, 1 on                */
    bool     enabled;
    bool     visible;
    bool     has_children;      /* children-display == "submenu"       */
    bool     expanded;          /* UI state: inline-expanded submenu   */
    struct MenuNode *children;
    struct MenuNode *next;
} MenuNode;

typedef struct Item {
    struct Item *next;

    /* D-Bus identity */
    char service[256];          /* bus name we talk to (unique or well-known) */
    char owner[256];            /* unique name of the owner (for lifetime)    */
    char path[256];             /* object path of the item                    */
    char registered_as[512];    /* string reported via the watcher property   */

    /* SNI properties (cached) */
    char id[128];
    char title[256];
    char status[32];
    char icon_name[256];
    char icon_theme_path[PATH_MAX];
    char menu_path[256];
    bool item_is_menu;
    char tooltip_title[256];
    char tooltip_text[1024];

    RawPixmap *pixmaps;
    int        npixmaps;

    /* pid -> screen resolution */
    pid_t pid;
    int   screen;
    bool  screen_resolved;
    int   resolve_tries;

    /* XEmbed window state */
    Window win;
    bool   docked;
    int    win_w, win_h;

    /* rendered icon, ready to paint (ARGB32 image surface) */
    cairo_surface_t *icon;

    /* async NewIcon refresh bookkeeping (mirrors snixembed handle_new_icon) */
    int icon_fetch_pending;
    int icon_fetch_changes;

    bool activate_unsupported;  /* Activate() returned an error once */

    sd_bus_slot *sig_slot;      /* match on the item's SNI signals */
} Item;

/* Tooltip (one global, re-created per screen as needed) */
typedef struct {
    Window win;
    int    screen;
    bool   visible;
    Item  *pending_item;        /* hovered, waiting for delay */
    long long due_ms;           /* when to show */
    int    root_x, root_y;
} Tooltip;

/* dbusmenu popup (one global) */
typedef struct {
    Item     *item;
    Window    win;
    int       screen;
    MenuNode *root;
    uint32_t  revision;
    int       width, height;
    int       hover_index;      /* index into flattened visible list */
    int       pos_x, pos_y;     /* root coords where to pop          */
    bool      visible;
    bool      pending;          /* layout requested, not yet shown   */
} MenuPopup;

/* --------------------------------------------------------------------
 * Globals
 * -------------------------------------------------------------------- */

static Display *dpy;
static int      nscreens;
static int      xfd;
static sd_bus  *bus;

static Item    *items;
static Tooltip  tooltip;
static MenuPopup menu;

static volatile sig_atomic_t g_running = 1;

/* --------------------------------------------------------------------
 * Stellar theme: colors + appearance font, fetched per screen (lazily,
 * cached).  When the DE is unreachable every value falls back to the
 * original hardcoded look.
 * -------------------------------------------------------------------- */

#define SNI_MAX_SCREENS 16

static ThemeData sni_theme[SNI_MAX_SCREENS];
static bool      sni_theme_ok[SNI_MAX_SCREENS];
static bool      sni_theme_tried[SNI_MAX_SCREENS];

static const ThemeData *theme_for_screen(int screen) {
    if (screen < 0 || screen >= SNI_MAX_SCREENS) screen = 0;
    if (!sni_theme_tried[screen]) {
        sni_theme_tried[screen] = true;
        sni_theme_ok[screen] =
            (request_theme_data(screen, &sni_theme[screen]) == 0);
        if (!sni_theme_ok[screen])
            fprintf(stderr, "snitray: no theme data for screen %d, "
                            "using default colors\n", screen);
    }
    return sni_theme_ok[screen] ? &sni_theme[screen] : NULL;
}

typedef struct { double r, g, b; } Rgb;

static bool hex_rgb(const char *hex, Rgb *out) {
    unsigned r, g, b;
    if (!hex || hex[0] != '#') return false;
    size_t len = strlen(hex);
    if (len != 7 && len != 9) return false;
    if (sscanf(hex + 1, "%02x%02x%02x", &r, &g, &b) != 3) return false;
    out->r = r / 255.0; out->g = g / 255.0; out->b = b / 255.0;
    return true;
}

static Rgb rgb_mix(Rgb a, Rgb b, double t) {
    Rgb c = { a.r + (b.r - a.r) * t,
              a.g + (b.g - a.g) * t,
              a.b + (b.b - a.b) * t };
    return c;
}

static unsigned long rgb_pixel(Rgb c) {
    return ((unsigned long)(c.r * 255.0 + 0.5) << 16) |
           ((unsigned long)(c.g * 255.0 + 0.5) << 8)  |
            (unsigned long)(c.b * 255.0 + 0.5);
}

/* Popup palette: four primaries from the theme, the rest derived shades.
 * Defaults reproduce the previous hardcoded grays exactly. */
typedef struct {
    Rgb bg, text, hover, border;
    Rgb disabled, separator, secondary;
} SniPalette;

static SniPalette palette_for_screen(int screen) {
    SniPalette p;
    p.bg     = (Rgb){0.16, 0.16, 0.16};
    p.text   = (Rgb){0.90, 0.90, 0.90};
    p.hover  = (Rgb){0.28, 0.28, 0.32};
    p.border = (Rgb){0.33, 0.33, 0.33};

    const ThemeData *td = theme_for_screen(screen);
    if (td) {
        hex_rgb(td->nk_color_window,       &p.bg);
        hex_rgb(td->nk_color_text,         &p.text);
        hex_rgb(td->nk_color_button_hover, &p.hover);
        hex_rgb(td->nk_color_border,       &p.border);
    }
    p.disabled  = rgb_mix(p.text, p.bg, 0.45);
    p.separator = rgb_mix(p.text, p.bg, 0.65);
    p.secondary = rgb_mix(p.text, p.bg, 0.25);
    return p;
}

static void set_rgb(cairo_t *cr, Rgb c) {
    cairo_set_source_rgb(cr, c.r, c.g, c.b);
}

/* Apply the configured appearance font.  The cairo toy API resolves the
 * family through fontconfig, so transparently converted bitmap fonts
 * (.otb) work here too - the size in ThemeData is already snapped to the
 * strike by the DE. */
static void theme_apply_font(cairo_t *cr, int screen, double fallback_size) {
    const ThemeData *td = theme_for_screen(screen);
    const char *family = "sans-serif";
    double size = fallback_size;
    if (td && td->font[0] && td->font[0] != '/')
        family = td->font;
    if (td && td->font_size > 0)
        size = td->font_size;
    cairo_select_font_face(cr, family,
                           CAIRO_FONT_SLANT_NORMAL, CAIRO_FONT_WEIGHT_NORMAL);
    cairo_set_font_size(cr, size);
}

/* Menu row metrics, recomputed from the font in menu_layout_and_show() so
 * rendering AND click hit-testing stay consistent at any font size. */
static int menu_item_h = 24;
static int menu_sep_h  = 9;

static void menu_update_metrics(cairo_t *cr_with_font) {
    cairo_font_extents_t fe;
    cairo_font_extents(cr_with_font, &fe);
    menu_item_h = (int)(fe.height + 0.5) + 9;
    if (menu_item_h < 20) menu_item_h = 20;
}

/* --------------------------------------------------------------------
 * SNI tooltip markup.  The StatusNotifierItem spec allows a small HTML
 * subset (Qt rich text) in the tooltip body - nm-tray really does send
 * "<pre>Connection <strong>...</strong></pre>".  We render plain cairo
 * text, so strip the tags and decode the common entities.
 * -------------------------------------------------------------------- */
static void strip_sni_markup(char *dst, size_t dst_size, const char *src) {
    size_t w = 0;
    if (dst_size == 0) return;
    if (!src) { dst[0] = '\0'; return; }

    while (*src && w + 1 < dst_size) {
        if (*src == '<') {
            /* block-ish tags become a space, everything else vanishes */
            bool brk = (strncasecmp(src, "<br", 3) == 0 ||
                        strncasecmp(src, "<p", 2)  == 0 ||
                        strncasecmp(src, "</p", 3) == 0 ||
                        strncasecmp(src, "<div", 4) == 0 ||
                        strncasecmp(src, "</div", 5) == 0);
            const char *end = strchr(src, '>');
            if (!end) break;                 /* unterminated tag */
            src = end + 1;
            if (brk && w > 0 && dst[w - 1] != ' ')
                dst[w++] = ' ';
        } else if (*src == '&') {
            static const struct { const char *ent; char ch; } ents[] = {
                { "&amp;",  '&'  }, { "&lt;",   '<' },
                { "&gt;",   '>'  }, { "&quot;", '"' },
                { "&apos;", '\'' }, { "&nbsp;", ' ' },
            };
            bool hit = false;
            for (size_t i = 0; i < sizeof(ents) / sizeof(ents[0]); i++) {
                size_t el = strlen(ents[i].ent);
                if (strncmp(src, ents[i].ent, el) == 0) {
                    dst[w++] = ents[i].ch;
                    src += el;
                    hit = true;
                    break;
                }
            }
            if (!hit) dst[w++] = *src++;
        } else {
            dst[w++] = *src++;
        }
    }
    while (w > 0 && dst[w - 1] == ' ') w--;  /* trim trailing space */
    dst[w] = '\0';
}

static int      default_screen_fallback = 0;
static TintMode tint_mode = TINT_NONE;
static double   tint_r = 1.0, tint_g = 1.0, tint_b = 1.0;
static char     override_icon_dir[PATH_MAX];

/* Atoms (display-global) */
static Atom a_xembed_info;
static Atom a_manager;
static Atom a_tray_opcode;
static Atom a_net_client_list;
static Atom a_net_wm_pid;
static Atom a_tray_selection[MAX_SNI_SCREENS]; /* _NET_SYSTEM_TRAY_S<n> */

/* Watcher state: names registered (for RegisteredStatusNotifierItems) */
static sd_bus_slot *name_owner_slot;

/* --------------------------------------------------------------------
 * Logging (same style as stellar.c, separate log file)
 * -------------------------------------------------------------------- */

#define SNITRAY_LOG_PATH "/tmp/stellar-snitray.log"

static FILE *log_file;

static void init_log_file(void) {
    if (log_file == NULL)
        log_file = fopen(SNITRAY_LOG_PATH, "a");
}

static void log_info(const char *fmt, ...) {
    va_list ap, ap2;
    va_start(ap, fmt);
    va_copy(ap2, ap);
    fprintf(stdout, "[snitray] ");
    vfprintf(stdout, fmt, ap);
    fprintf(stdout, "\n");
    fflush(stdout);
    init_log_file();
    if (log_file) {
        fprintf(log_file, "[snitray] ");
        vfprintf(log_file, fmt, ap2);
        fprintf(log_file, "\n");
        fflush(log_file);
    }
    va_end(ap2);
    va_end(ap);
}

static void log_error(const char *fmt, ...) {
    va_list ap, ap2;
    va_start(ap, fmt);
    va_copy(ap2, ap);
    fprintf(stderr, "[snitray] ERROR: ");
    vfprintf(stderr, fmt, ap);
    fprintf(stderr, "\n");
    fflush(stderr);
    init_log_file();
    if (log_file) {
        fprintf(log_file, "[snitray] ERROR: ");
        vfprintf(log_file, fmt, ap2);
        fprintf(log_file, "\n");
        fflush(log_file);
    }
    va_end(ap2);
    va_end(ap);
}

static long long now_ms(void) {
    struct timeval tv;
    gettimeofday(&tv, NULL);
    return ((long long)tv.tv_sec * 1000LL) + (tv.tv_usec / 1000);
}

/* --------------------------------------------------------------------
 * X error handling: tray docking races make BadWindow unavoidable
 * -------------------------------------------------------------------- */

static int x_error_handler(Display *d, XErrorEvent *e) {
    char buf[128];
    XGetErrorText(d, e->error_code, buf, sizeof(buf));
    log_info("X error (ignored): %s, request %d", buf, e->request_code);
    return 0;
}

/* --------------------------------------------------------------------
 * /proc helpers for pid ancestry
 * -------------------------------------------------------------------- */

static pid_t proc_ppid(pid_t pid) {
    char path[64], buf[1024];
    snprintf(path, sizeof(path), "/proc/%d/stat", (int)pid);
    FILE *f = fopen(path, "r");
    if (!f)
        return -1;
    size_t n = fread(buf, 1, sizeof(buf) - 1, f);
    fclose(f);
    if (n == 0)
        return -1;
    buf[n] = '\0';

    /* comm field can contain spaces/parens; parse after the LAST ')' */
    char *p = strrchr(buf, ')');
    if (!p)
        return -1;
    p++;
    int ppid = -1;
    char state;
    if (sscanf(p, " %c %d", &state, &ppid) != 2)
        return -1;
    return (pid_t)ppid;
}

/* Fill chain[] with pid, ppid, pppid, ... up to init. Returns count. */
static int proc_ancestor_chain(pid_t pid, pid_t *chain, int max) {
    int n = 0;
    while (pid > 1 && n < max) {
        chain[n++] = pid;
        pid = proc_ppid(pid);
        if (pid <= 0)
            break;
    }
    return n;
}

/*
 * Read DISPLAY from /proc/<pid>/environ. If it carries an explicit
 * screen suffix (":0.1"), return that screen number, else -1.
 * Only works for same-uid processes, which session apps are.
 */
static int screen_from_proc_environ(pid_t pid) {
    char path[64];
    snprintf(path, sizeof(path), "/proc/%d/environ", (int)pid);
    FILE *f = fopen(path, "r");
    if (!f)
        return -1;

    char buf[8192];
    size_t n = fread(buf, 1, sizeof(buf) - 1, f);
    fclose(f);
    if (n == 0)
        return -1;
    buf[n] = '\0';

    int result = -1;
    for (size_t i = 0; i < n; ) {
        char *entry = buf + i;
        size_t len = strnlen(entry, n - i);
        if (strncmp(entry, "DISPLAY=", 8) == 0) {
            const char *val = entry + 8;
            const char *colon = strrchr(val, ':');
            const char *dot = colon ? strchr(colon, '.') : NULL;
            if (dot) {
                int s = atoi(dot + 1);
                if (s >= 0 && s < nscreens)
                    result = s;
            }
            break;
        }
        i += len + 1;
    }
    return result;
}

/* --------------------------------------------------------------------
 * pid -> screen resolution
 *
 * Strategy, strongest signal first:
 *   1. A window on some screen has _NET_WM_PID == the item's pid.
 *   2. A window pid is an ancestor of the item pid (launcher owns the
 *      window, child registered the SNI item).
 *   3. The item pid is an ancestor of a window pid (item process forked
 *      the thing that owns windows -- common with wine/wrapper trees).
 *   4. The item process's own DISPLAY env names a screen explicitly.
 *
 * These are also the functions worth lifting into stellar.c if you want
 * pid->screen resolution available over the IPC socket later.
 * -------------------------------------------------------------------- */

typedef struct {
    pid_t pid;
    int   screen;
} WinPid;

static pid_t window_pid(Window w) {
    Atom actual;
    int fmt;
    unsigned long n, after;
    unsigned char *prop = NULL;
    pid_t pid = -1;

    if (XGetWindowProperty(dpy, w, a_net_wm_pid, 0, 1, False, XA_CARDINAL,
                           &actual, &fmt, &n, &after, &prop) == Success && prop) {
        if (fmt == 32 && n >= 1)
            pid = (pid_t)*(unsigned long *)prop;
        XFree(prop);
    }
    return pid;
}

/* Collect (pid, screen) pairs for all client windows on every screen. */
static int collect_window_pids(WinPid *out, int max) {
    int count = 0;

    for (int s = 0; s < nscreens && count < max; s++) {
        Window root = RootWindow(dpy, s);

        Atom actual;
        int fmt;
        unsigned long n = 0, after;
        unsigned char *prop = NULL;
        Window *list = NULL;
        bool from_client_list = false;

        /* Prefer _NET_CLIENT_LIST (awesome maintains it) */
        if (XGetWindowProperty(dpy, root, a_net_client_list, 0, 4096, False,
                               XA_WINDOW, &actual, &fmt, &n, &after,
                               &prop) == Success && prop && fmt == 32 && n > 0) {
            list = (Window *)prop;
            from_client_list = true;
        }

        if (list) {
            for (unsigned long i = 0; i < n && count < max; i++) {
                pid_t p = window_pid(list[i]);
                if (p > 0) {
                    out[count].pid = p;
                    out[count].screen = s;
                    count++;
                }
            }
            XFree(prop);
        }

        if (!from_client_list) {
            /* Fallback: scan direct children of the root */
            Window root_ret, parent_ret, *children = NULL;
            unsigned int nchildren = 0;
            if (XQueryTree(dpy, root, &root_ret, &parent_ret,
                           &children, &nchildren)) {
                for (unsigned int i = 0; i < nchildren && count < max; i++) {
                    pid_t p = window_pid(children[i]);
                    if (p > 0) {
                        out[count].pid = p;
                        out[count].screen = s;
                        count++;
                    }
                }
                if (children)
                    XFree(children);
            }
        }
    }
    return count;
}

/* Returns screen number, or -1 if it could not be determined. */
static int screen_for_pid(pid_t pid) {
    if (pid <= 0)
        return -1;

    WinPid wins[256];
    int nwins = collect_window_pids(wins, 256);

    /* 1. Direct match */
    for (int i = 0; i < nwins; i++)
        if (wins[i].pid == pid)
            return wins[i].screen;

    /* 2. A window pid is an ancestor of the item pid */
    pid_t chain[32];
    int clen = proc_ancestor_chain(pid, chain, 32);
    for (int c = 1; c < clen; c++)        /* skip chain[0] == pid itself */
        for (int i = 0; i < nwins; i++)
            if (wins[i].pid == chain[c])
                return wins[i].screen;

    /* 3. The item pid is an ancestor of a window pid */
    for (int i = 0; i < nwins; i++) {
        pid_t wchain[32];
        int wlen = proc_ancestor_chain(wins[i].pid, wchain, 32);
        for (int c = 1; c < wlen; c++)
            if (wchain[c] == pid)
                return wins[i].screen;
    }

    /* 4. The process's own DISPLAY env */
    int s = screen_from_proc_environ(pid);
    if (s >= 0)
        return s;

    return -1;
}

/* --------------------------------------------------------------------
 * Icon pixel filter & substitution
 *
 * Everything that becomes an icon -- SNI pixmaps, theme PNGs, absolute
 * paths -- funnels through finalize_icon_surface(). That is the single
 * choke point for the planned tinting/substitution feature:
 *
 *   - load_override_icon() implements substitution: a PNG named after
 *     the item's Id in $XDG_CONFIG_HOME/stellar/sni-icons/ wins over
 *     anything the app provides.
 *   - apply_icon_filter() runs on the raw premultiplied ARGB32 pixels
 *     of whatever surface was chosen. Desaturation and colorize are
 *     wired up now (env-controlled); per-app rules can slot in here
 *     later by keying off item->id.
 * -------------------------------------------------------------------- */

static void apply_icon_filter(cairo_surface_t *surf, const Item *it) {
    (void)it; /* future: per-id rules */

    if (tint_mode == TINT_NONE || !surf)
        return;
    if (cairo_image_surface_get_format(surf) != CAIRO_FORMAT_ARGB32)
        return;

    cairo_surface_flush(surf);
    int w = cairo_image_surface_get_width(surf);
    int h = cairo_image_surface_get_height(surf);
    int stride = cairo_image_surface_get_stride(surf);
    unsigned char *data = cairo_image_surface_get_data(surf);
    if (!data)
        return;

    for (int y = 0; y < h; y++) {
        uint32_t *row = (uint32_t *)(data + y * stride);
        for (int x = 0; x < w; x++) {
            uint32_t px = row[x];
            uint32_t a = (px >> 24) & 0xff;
            uint32_t r = (px >> 16) & 0xff;
            uint32_t g = (px >> 8) & 0xff;
            uint32_t b = px & 0xff;

            /* Rec. 601 luma on the premultiplied values; since all
             * channels share the same alpha, luma stays premultiplied. */
            uint32_t luma = (r * 299 + g * 587 + b * 114) / 1000;

            if (tint_mode == TINT_GRAY) {
                r = g = b = luma;
            } else { /* TINT_COLOR: luma modulated by the tint color */
                r = (uint32_t)(luma * tint_r);
                g = (uint32_t)(luma * tint_g);
                b = (uint32_t)(luma * tint_b);
            }
            /* keep premultiplied invariant: channel <= alpha */
            if (r > a) r = a;
            if (g > a) g = a;
            if (b > a) b = a;

            row[x] = (a << 24) | (r << 16) | (g << 8) | b;
        }
    }
    cairo_surface_mark_dirty(surf);
}

static cairo_surface_t *load_png_checked(const char *path) {
    cairo_surface_t *s = cairo_image_surface_create_from_png(path);
    if (cairo_surface_status(s) != CAIRO_STATUS_SUCCESS) {
        cairo_surface_destroy(s);
        return NULL;
    }
    /* Force ARGB32 so the filter can run on it */
    if (cairo_image_surface_get_format(s) != CAIRO_FORMAT_ARGB32) {
        int w = cairo_image_surface_get_width(s);
        int h = cairo_image_surface_get_height(s);
        cairo_surface_t *conv = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, w, h);
        cairo_t *cr = cairo_create(conv);
        cairo_set_source_surface(cr, s, 0, 0);
        cairo_paint(cr);
        cairo_destroy(cr);
        cairo_surface_destroy(s);
        s = conv;
    }
    return s;
}

static cairo_surface_t *load_override_icon(const Item *it) {
    if (it->id[0] == '\0' || override_icon_dir[0] == '\0')
        return NULL;
    char path[PATH_MAX];
    int n = snprintf(path, sizeof(path), "%s/%s.png", override_icon_dir, it->id);
    if (n < 0 || (size_t)n >= sizeof(path))
        return NULL;  /* would truncate -- treat as no override */
    if (access(path, R_OK) != 0)
        return NULL;
    return load_png_checked(path);
}

/* --------------------------------------------------------------------
 * Theme icon lookup, GTK-free
 *
 * We do not parse index.theme; we probe the well-known directory
 * layouts directly. PNG only -- if only an SVG exists we fall back to
 * the item's pixmap data, which apps shipping SVG icons always provide
 * in practice (and if not, rsvg-convert is probed as a last resort).
 * -------------------------------------------------------------------- */

static const int   probe_sizes[] = { 0 /* replaced by target */, 48, 32, 24, 22, 16, 64, 128, 256 };
static const char *probe_cats[]  = { "status", "apps", "panel", "devices", "actions", "categories" };

static bool try_path(char *out, size_t outsz, const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(out, outsz, fmt, ap);
    va_end(ap);
    return access(out, R_OK) == 0;
}

/* Search one icons base dir (e.g. /usr/share/icons) for theme/name. */
static bool probe_icons_dir(const char *base, const char *theme,
                            const char *name, int target,
                            char *out, size_t outsz) {
    int sizes[16];
    int nsizes = 0;
    sizes[nsizes++] = target;
    for (size_t i = 1; i < sizeof(probe_sizes) / sizeof(probe_sizes[0]); i++)
        if (probe_sizes[i] != target)
            sizes[nsizes++] = probe_sizes[i];

    for (int si = 0; si < nsizes; si++) {
        for (size_t ci = 0; ci < sizeof(probe_cats) / sizeof(probe_cats[0]); ci++) {
            if (try_path(out, outsz, "%s/%s/%dx%d/%s/%s.png",
                         base, theme, sizes[si], sizes[si], probe_cats[ci], name))
                return true;
        }
        if (try_path(out, outsz, "%s/%s/%dx%d/%s.png",
                     base, theme, sizes[si], sizes[si], name))
            return true;
    }
    return false;
}

/* Probe a flat directory (appindicator IconThemePath, /usr/share/pixmaps) */
static bool probe_flat_dir(const char *dir, const char *name, int target,
                           char *out, size_t outsz) {
    if (try_path(out, outsz, "%s/%s.png", dir, name))
        return true;
    /* appindicator theme paths sometimes use hicolor-style subdirs */
    if (probe_icons_dir(dir, "hicolor", name, target, out, outsz))
        return true;
    return false;
}

/* Last resort for svg-only icons: rasterize via rsvg-convert if present. */
static cairo_surface_t *try_rsvg(const char *svg_path, int target) {
    char png_path[PATH_MAX];
    snprintf(png_path, sizeof(png_path), "/tmp/stellar-snitray-%d-icon.png",
             (int)getpid());

    char cmd[PATH_MAX * 2 + 128];
    snprintf(cmd, sizeof(cmd),
             "rsvg-convert -w %d -h %d -o '%s' '%s' 2>/dev/null",
             target, target, png_path, svg_path);
    if (system(cmd) != 0)
        return NULL;
    cairo_surface_t *s = load_png_checked(png_path);
    unlink(png_path);
    return s;
}

static bool probe_svg(const char *base, const char *theme, const char *name,
                      char *out, size_t outsz) {
    for (size_t ci = 0; ci < sizeof(probe_cats) / sizeof(probe_cats[0]); ci++) {
        if (try_path(out, outsz, "%s/%s/scalable/%s/%s.svg",
                     base, theme, probe_cats[ci], name))
            return true;
    }
    return try_path(out, outsz, "%s/%s/scalable/%s.svg", base, theme, name);
}

static cairo_surface_t *load_theme_icon(const Item *it, int target) {
    const char *name = it->icon_name;
    if (name[0] == '\0')
        return NULL;

    /* Some apps wrongly set a path as the name (snixembed handles this too) */
    if (strchr(name, '/') && access(name, R_OK) == 0) {
        const char *ext = strrchr(name, '.');
        if (ext && strcasecmp(ext, ".svg") == 0)
            return try_rsvg(name, target);
        return load_png_checked(name);
    }

    char found[PATH_MAX];

    /* 1. appindicator-provided theme path */
    if (it->icon_theme_path[0] != '\0' &&
        probe_flat_dir(it->icon_theme_path, name, target, found, sizeof(found)))
        return load_png_checked(found);

    /* 2. standard icon dirs */
    char bases[8][PATH_MAX];
    int nbases = 0;

    const char *home = getenv("HOME");
    const char *xdg_data_home = getenv("XDG_DATA_HOME");
    if (xdg_data_home && *xdg_data_home)
        snprintf(bases[nbases++], PATH_MAX, "%s/icons", xdg_data_home);
    else if (home)
        snprintf(bases[nbases++], PATH_MAX, "%s/.local/share/icons", home);
    if (home)
        snprintf(bases[nbases++], PATH_MAX, "%s/.icons", home);

    const char *xdg_dirs = getenv("XDG_DATA_DIRS");
    if (!xdg_dirs || !*xdg_dirs)
        xdg_dirs = "/usr/local/share:/usr/share";
    char dirs_copy[PATH_MAX * 2];
    snprintf(dirs_copy, sizeof(dirs_copy), "%s", xdg_dirs);
    for (char *tok = strtok(dirs_copy, ":"); tok && nbases < 8;
         tok = strtok(NULL, ":"))
        snprintf(bases[nbases++], PATH_MAX, "%s/icons", tok);

    static const char *themes[] = { "hicolor", "Adwaita" };

    for (int b = 0; b < nbases; b++)
        for (size_t t = 0; t < sizeof(themes) / sizeof(themes[0]); t++)
            if (probe_icons_dir(bases[b], themes[t], name, target,
                                found, sizeof(found)))
                return load_png_checked(found);

    /* 3. pixmaps */
    if (try_path(found, sizeof(found), "/usr/share/pixmaps/%s.png", name))
        return load_png_checked(found);

    /* 4. svg-only icons, rasterized externally if rsvg-convert exists */
    for (int b = 0; b < nbases; b++)
        for (size_t t = 0; t < sizeof(themes) / sizeof(themes[0]); t++)
            if (probe_svg(bases[b], themes[t], name, found, sizeof(found)))
                return try_rsvg(found, target);

    return NULL;
}

/* --------------------------------------------------------------------
 * SNI pixmap -> cairo surface
 *
 * IconPixmap data is ARGB32 in network byte order, non-premultiplied.
 * Cairo wants native-endian premultiplied ARGB32.
 * -------------------------------------------------------------------- */

static const RawPixmap *choose_best_pixmap(const Item *it, int target) {
    if (it->npixmaps == 0)
        return NULL;
    const RawPixmap *best = &it->pixmaps[0];
    if (target <= 0)
        return best;

    /* smallest pixmap >= target, else the largest (same rule as snixembed) */
    for (int i = 1; i < it->npixmaps; i++) {
        const RawPixmap *pm = &it->pixmaps[i];
        if ((best->h < target && best->h < pm->h) ||
            (best->h > pm->h && pm->h >= target))
            best = pm;
    }
    return best;
}

static cairo_surface_t *surface_from_pixmap(const RawPixmap *pm) {
    if (!pm || pm->w <= 0 || pm->h <= 0 || !pm->data)
        return NULL;

    cairo_surface_t *s =
        cairo_image_surface_create(CAIRO_FORMAT_ARGB32, pm->w, pm->h);
    if (cairo_surface_status(s) != CAIRO_STATUS_SUCCESS) {
        cairo_surface_destroy(s);
        return NULL;
    }
    cairo_surface_flush(s);
    int stride = cairo_image_surface_get_stride(s);
    unsigned char *dst = cairo_image_surface_get_data(s);

    for (int y = 0; y < pm->h; y++) {
        uint32_t *row = (uint32_t *)(dst + y * stride);
        const uint8_t *src = pm->data + (size_t)y * pm->w * 4;
        for (int x = 0; x < pm->w; x++) {
            uint32_t a = src[x * 4 + 0];
            uint32_t r = src[x * 4 + 1];
            uint32_t g = src[x * 4 + 2];
            uint32_t b = src[x * 4 + 3];
            /* premultiply */
            r = r * a / 255;
            g = g * a / 255;
            b = b * a / 255;
            row[x] = (a << 24) | (r << 16) | (g << 8) | b;
        }
    }
    cairo_surface_mark_dirty(s);
    return s;
}

/* --------------------------------------------------------------------
 * Icon rendering: rebuild item->icon from the cached SNI properties
 * -------------------------------------------------------------------- */

static void item_redraw(Item *it);

static void item_rebuild_icon(Item *it) {
    int target = it->win_h > 0 ? it->win_h : DEFAULT_ICON_SIZE;
    cairo_surface_t *s = NULL;

    /* substitution feature: user-provided icon wins */
    s = load_override_icon(it);

    /* theme / file icon by name (snixembed precedence) */
    if (!s)
        s = load_theme_icon(it, target);

    /* pixmap data shipped over the bus */
    if (!s)
        s = surface_from_pixmap(choose_best_pixmap(it, target));

    if (s)
        apply_icon_filter(s, it);

    if (it->icon)
        cairo_surface_destroy(it->icon);
    it->icon = s;

    item_redraw(it);
}

static void item_redraw(Item *it) {
    if (it->win == None)
        return;

    /* ParentRelative background: clearing repaints the tray's backdrop,
     * then we composite the icon over it (pseudo-transparency). */
    XClearWindow(dpy, it->win);

    if (!it->icon)
        return;

    int w = it->win_w > 0 ? it->win_w : DEFAULT_ICON_SIZE;
    int h = it->win_h > 0 ? it->win_h : DEFAULT_ICON_SIZE;

    cairo_surface_t *xs = cairo_xlib_surface_create(
        dpy, it->win, DefaultVisual(dpy, it->screen), w, h);
    cairo_t *cr = cairo_create(xs);

    int iw = cairo_image_surface_get_width(it->icon);
    int ih = cairo_image_surface_get_height(it->icon);
    if (iw > 0 && ih > 0) {
        double scale = (double)h / ih;
        if (iw * scale > w)
            scale = (double)w / iw;
        double dx = (w - iw * scale) / 2.0;
        double dy = (h - ih * scale) / 2.0;

        cairo_translate(cr, dx, dy);
        cairo_scale(cr, scale, scale);
        cairo_set_source_surface(cr, it->icon, 0, 0);
        cairo_pattern_set_filter(cairo_get_source(cr), CAIRO_FILTER_GOOD);
        cairo_set_operator(cr, CAIRO_OPERATOR_OVER);
        cairo_paint(cr);
    }

    cairo_destroy(cr);
    cairo_surface_destroy(xs);
    XFlush(dpy);
}

/* --------------------------------------------------------------------
 * XEmbed window creation & docking
 * -------------------------------------------------------------------- */

static void item_set_xembed_info(Item *it, bool mapped) {
    long info[2] = { 0 /* version */, mapped ? XEMBED_MAPPED : 0 };
    XChangeProperty(dpy, it->win, a_xembed_info, a_xembed_info, 32,
                    PropModeReplace, (unsigned char *)info, 2);
}

static void item_create_window(Item *it) {
    Window root = RootWindow(dpy, it->screen);

    XSetWindowAttributes attrs;
    attrs.background_pixmap = ParentRelative;
    attrs.event_mask = ExposureMask | ButtonPressMask | ButtonReleaseMask |
                       StructureNotifyMask | EnterWindowMask | LeaveWindowMask;

    it->win = XCreateWindow(dpy, root, 0, 0,
                            DEFAULT_ICON_SIZE, DEFAULT_ICON_SIZE, 0,
                            CopyFromParent, InputOutput, CopyFromParent,
                            CWBackPixmap | CWEventMask, &attrs);
    it->win_w = it->win_h = DEFAULT_ICON_SIZE;
    it->docked = false;

    XClassHint hint = { (char *)"stellar-snitray", (char *)"stellar-snitray" };
    XSetClassHint(dpy, it->win, &hint);
    if (it->id[0] != '\0')
        XStoreName(dpy, it->win, it->id);

    // Start unmapped - the real Status isn't known until the async GetAll
    // reply arrives.  item_getall_cb will flip XEMBED_MAPPED once it has
    // the authoritative value, preventing flash-then-hide for Passive items.
    item_set_xembed_info(it, false);
}

static bool item_dock(Item *it) {
    if (it->win == None)
        item_create_window(it);
    if (it->docked)
        return true;

    Window manager = XGetSelectionOwner(dpy, a_tray_selection[it->screen]);
    if (manager == None)
        return false;   /* wait for MANAGER message / retry timer */

    XEvent ev;
    memset(&ev, 0, sizeof(ev));
    ev.xclient.type = ClientMessage;
    ev.xclient.window = manager;
    ev.xclient.message_type = a_tray_opcode;
    ev.xclient.format = 32;
    ev.xclient.data.l[0] = CurrentTime;
    ev.xclient.data.l[1] = SYSTEM_TRAY_REQUEST_DOCK;
    ev.xclient.data.l[2] = (long)it->win;

    XSendEvent(dpy, manager, False, NoEventMask, &ev);
    XFlush(dpy);
    it->docked = true;  /* optimistically; undone on reparent-to-root */
    log_info("%s docked on screen %d (tray window 0x%lx)",
             it->service, it->screen, (unsigned long)manager);
    return true;
}

static void item_undock_destroy(Item *it) {
    if (it->win != None) {
        XDestroyWindow(dpy, it->win);
        XFlush(dpy);
        it->win = None;
    }
    it->docked = false;
}

/* Move the icon to a (newly resolved) screen */
static void item_move_to_screen(Item *it, int screen) {
    if (screen == it->screen && it->win != None)
        return;
    log_info("%s: moving icon to screen %d", it->service, screen);
    item_undock_destroy(it);
    it->screen = screen;
    item_create_window(it);
    item_dock(it);
    item_rebuild_icon(it);
}

/* --------------------------------------------------------------------
 * Tooltip
 * -------------------------------------------------------------------- */

static void tooltip_hide(void) {
    if (tooltip.win != None) {
        XDestroyWindow(dpy, tooltip.win);
        XFlush(dpy);
        tooltip.win = None;
    }
    tooltip.visible = false;
    tooltip.pending_item = NULL;
}

static void tooltip_show(Item *it, int root_x, int root_y) {
    tooltip_hide();

    char line1[300] = "", line2[1100] = "";
    if (it->tooltip_title[0])
        snprintf(line1, sizeof(line1), "%s", it->tooltip_title);
    else if (it->title[0])
        snprintf(line1, sizeof(line1), "%s", it->title);
    else if (it->id[0])
        snprintf(line1, sizeof(line1), "%s", it->id);
    if (it->tooltip_text[0])
        snprintf(line2, sizeof(line2), "%s", it->tooltip_text);
    if (!line1[0] && !line2[0])
        return;

    int screen = it->screen;
    Window root = RootWindow(dpy, screen);
    SniPalette pal = palette_for_screen(screen);

    /* measure */
    cairo_surface_t *meas =
        cairo_image_surface_create(CAIRO_FORMAT_ARGB32, 1, 1);
    cairo_t *mcr = cairo_create(meas);
    theme_apply_font(mcr, screen, 12);
    cairo_font_extents_t fe;
    cairo_font_extents(mcr, &fe);
    cairo_text_extents_t e1 = {0}, e2 = {0};
    if (line1[0]) cairo_text_extents(mcr, line1, &e1);
    if (line2[0]) cairo_text_extents(mcr, line2, &e2);
    cairo_destroy(mcr);
    cairo_surface_destroy(meas);

    int pad = 6, lh = (int)(fe.height + 0.5) + 2;
    int w = (int)(e1.width > e2.width ? e1.width : e2.width) + pad * 2;
    int h = pad * 2 + lh * ((line1[0] ? 1 : 0) + (line2[0] ? 1 : 0));
    if (w < 24) w = 24;

    int sw = DisplayWidth(dpy, screen);
    int sh = DisplayHeight(dpy, screen);
    int x = root_x + 8, y = root_y + 16;
    if (x + w > sw) x = sw - w - 2;
    if (y + h > sh) y = root_y - h - 8;
    if (x < 0) x = 0;
    if (y < 0) y = 0;

    XSetWindowAttributes attrs;
    attrs.override_redirect = True;
    attrs.background_pixel = rgb_pixel(pal.bg);
    attrs.border_pixel = rgb_pixel(pal.border);
    attrs.event_mask = ExposureMask;
    tooltip.win = XCreateWindow(dpy, root, x, y, w, h, 1,
                                CopyFromParent, InputOutput, CopyFromParent,
                                CWOverrideRedirect | CWBackPixel |
                                CWBorderPixel | CWEventMask, &attrs);
    tooltip.screen = screen;
    XMapRaised(dpy, tooltip.win);

    cairo_surface_t *xs = cairo_xlib_surface_create(
        dpy, tooltip.win, DefaultVisual(dpy, screen), w, h);
    cairo_t *cr = cairo_create(xs);
    set_rgb(cr, pal.bg);
    cairo_paint(cr);
    theme_apply_font(cr, screen, 12);
    set_rgb(cr, pal.text);
    int ty = pad + (int)(fe.ascent + 0.5);
    if (line1[0]) { cairo_move_to(cr, pad, ty); cairo_show_text(cr, line1); ty += lh; }
    if (line2[0]) {
        set_rgb(cr, pal.secondary);
        cairo_move_to(cr, pad, ty);
        cairo_show_text(cr, line2);
    }
    cairo_destroy(cr);
    cairo_surface_destroy(xs);
    XFlush(dpy);

    tooltip.visible = true;
}

/* --------------------------------------------------------------------
 * dbusmenu (com.canonical.dbusmenu)
 *
 * snixembed leaned on DbusmenuGtk for this; here we fetch the layout
 * ourselves and render a small cairo popup. Submenus expand inline
 * (click to toggle), which keeps it to a single window.
 * -------------------------------------------------------------------- */

#define MENU_FONT_SIZE 13.0
#define MENU_ITEM_H    24
#define MENU_SEP_H     9
#define MENU_PAD_X     12
#define MENU_INDENT    16
#define MENU_MARK_W    18   /* gutter for check/radio marks */

/* Check/radio indicators and submenu arrows are drawn as cairo paths,
 * NOT text.  The toy text API (cairo_select_font_face) resolves to a
 * single face with no glyph fallback, so Unicode marks (U+2713, U+25CF,
 * U+25B8...) render as tofu boxes whenever fontconfig's "sans-serif"
 * maps to a font missing them (Liberation Sans, for one).  snixembed
 * dodges this only because Pango does cross-font fallback. */
static void menu_draw_toggle(cairo_t *cr, double x, double cy,
                             const MenuNode *node) {
    double s  = 9.0;                /* mark box size            */
    double by = cy - s / 2.0;       /* top of the mark box      */

    cairo_set_line_width(cr, 1.2);
    if (node->toggle_type[0] == 'r') {            /* radio */
        cairo_new_sub_path(cr);
        cairo_arc(cr, x + s / 2.0, cy, s / 2.0, 0, 2 * M_PI);
        cairo_stroke(cr);
        if (node->toggle_state == 1) {
            cairo_new_sub_path(cr);
            cairo_arc(cr, x + s / 2.0, cy, s / 2.0 - 2.5, 0, 2 * M_PI);
            cairo_fill(cr);
        }
    } else {                                       /* checkmark */
        cairo_rectangle(cr, x + 0.5, by + 0.5, s - 1.0, s - 1.0);
        cairo_stroke(cr);
        if (node->toggle_state == 1) {
            cairo_set_line_width(cr, 1.6);
            cairo_move_to(cr, x + 2.0,       by + s * 0.55);
            cairo_line_to(cr, x + s * 0.40,  by + s - 2.0);
            cairo_line_to(cr, x + s - 1.5,   by + 1.5);
            cairo_stroke(cr);
        }
    }
}

static void menu_draw_arrow(cairo_t *cr, double rx, double cy,
                            bool expanded) {
    if (expanded) {                  /* down triangle */
        cairo_move_to(cr, rx - 4.0, cy - 2.0);
        cairo_line_to(cr, rx + 4.0, cy - 2.0);
        cairo_line_to(cr, rx,       cy + 3.0);
    } else {                         /* right triangle */
        cairo_move_to(cr, rx - 2.0, cy - 4.0);
        cairo_line_to(cr, rx - 2.0, cy + 4.0);
        cairo_line_to(cr, rx + 3.0, cy);
    }
    cairo_close_path(cr);
    cairo_fill(cr);
}

static void menu_free_nodes(MenuNode *n) {
    while (n) {
        MenuNode *next = n->next;
        menu_free_nodes(n->children);
        free(n);
        n = next;
    }
}

static void menu_close(void) {
    if (menu.win != None) {
        XUngrabPointer(dpy, CurrentTime);
        XDestroyWindow(dpy, menu.win);
        XFlush(dpy);
        menu.win = None;
    }
    menu_free_nodes(menu.root);
    menu.root = NULL;
    menu.item = NULL;
    menu.visible = false;
    menu.pending = false;
    menu.hover_index = -1;
}

/* Parse one (ia{sv}av) layout node; m is positioned at the struct. */
static MenuNode *menu_parse_node(sd_bus_message *m) {
    int r = sd_bus_message_enter_container(m, 'r', "ia{sv}av");
    if (r <= 0)
        return NULL;

    MenuNode *node = calloc(1, sizeof(MenuNode));
    node->enabled = true;
    node->visible = true;
    node->toggle_state = -1;
    snprintf(node->type, sizeof(node->type), "standard");

    sd_bus_message_read(m, "i", &node->id);

    /* properties */
    sd_bus_message_enter_container(m, 'a', "{sv}");
    while (sd_bus_message_enter_container(m, 'e', "sv") > 0) {
        const char *key = NULL;
        sd_bus_message_read(m, "s", &key);

        const char *contents = NULL;
        char type;
        sd_bus_message_peek_type(m, &type, &contents);
        sd_bus_message_enter_container(m, 'v', contents);

        if (strcmp(key, "label") == 0 && strcmp(contents, "s") == 0) {
            const char *s = NULL;
            sd_bus_message_read(m, "s", &s);
            /* strip mnemonic underscores: "_File" -> "File" */
            size_t o = 0;
            for (size_t i = 0; s && s[i] && o < sizeof(node->label) - 1; i++) {
                if (s[i] == '_' && s[i + 1] != '_')
                    continue;
                if (s[i] == '_' && s[i + 1] == '_')
                    i++;
                node->label[o++] = s[i];
            }
            node->label[o] = '\0';
        } else if (strcmp(key, "type") == 0 && strcmp(contents, "s") == 0) {
            const char *s = NULL;
            sd_bus_message_read(m, "s", &s);
            snprintf(node->type, sizeof(node->type), "%s", s ? s : "standard");
        } else if (strcmp(key, "toggle-type") == 0 && strcmp(contents, "s") == 0) {
            const char *s = NULL;
            sd_bus_message_read(m, "s", &s);
            snprintf(node->toggle_type, sizeof(node->toggle_type), "%s", s ? s : "");
        } else if (strcmp(key, "toggle-state") == 0 && strcmp(contents, "i") == 0) {
            sd_bus_message_read(m, "i", &node->toggle_state);
        } else if (strcmp(key, "enabled") == 0 && strcmp(contents, "b") == 0) {
            int b; sd_bus_message_read(m, "b", &b); node->enabled = b;
        } else if (strcmp(key, "visible") == 0 && strcmp(contents, "b") == 0) {
            int b; sd_bus_message_read(m, "b", &b); node->visible = b;
        } else if (strcmp(key, "children-display") == 0 && strcmp(contents, "s") == 0) {
            const char *s = NULL;
            sd_bus_message_read(m, "s", &s);
            node->has_children = s && strcmp(s, "submenu") == 0;
        } else {
            sd_bus_message_skip(m, contents);
        }

        sd_bus_message_exit_container(m); /* v */
        sd_bus_message_exit_container(m); /* e */
    }
    sd_bus_message_exit_container(m); /* a{sv} */

    /* children: av, each v wraps (ia{sv}av) */
    sd_bus_message_enter_container(m, 'a', "v");
    MenuNode **tail = &node->children;
    while (sd_bus_message_enter_container(m, 'v', "(ia{sv}av)") > 0) {
        MenuNode *child = menu_parse_node(m);
        if (child) {
            *tail = child;
            tail = &child->next;
            node->has_children = true;
        }
        sd_bus_message_exit_container(m); /* v */
    }
    sd_bus_message_exit_container(m); /* av */

    sd_bus_message_exit_container(m); /* struct */
    return node;
}

/* Flatten visible nodes (with inline-expanded submenus) for layout/hit-test */
typedef struct {
    MenuNode *node;
    int depth;
} MenuRow;

static int menu_flatten_rec(MenuNode *n, int depth, MenuRow *rows, int max, int count) {
    for (; n && count < max; n = n->next) {
        if (!n->visible)
            continue;
        rows[count].node = n;
        rows[count].depth = depth;
        count++;
        if (n->has_children && n->expanded)
            count = menu_flatten_rec(n->children, depth + 1, rows, max, count);
    }
    return count;
}

static int menu_flatten(MenuRow *rows, int max) {
    if (!menu.root)
        return 0;
    return menu_flatten_rec(menu.root->children, 0, rows, max, 0);
}

static void menu_render(void) {
    if (menu.win == None)
        return;

    MenuRow rows[128];
    int n = menu_flatten(rows, 128);

    cairo_surface_t *xs = cairo_xlib_surface_create(
        dpy, menu.win, DefaultVisual(dpy, menu.screen),
        menu.width, menu.height);
    cairo_t *cr = cairo_create(xs);
    SniPalette pal = palette_for_screen(menu.screen);

    set_rgb(cr, pal.bg);
    cairo_paint(cr);
    theme_apply_font(cr, menu.screen, MENU_FONT_SIZE);
    cairo_font_extents_t fe;
    cairo_font_extents(cr, &fe);
    /* vertically center the text in the row */
    double baseline = (menu_item_h - fe.height) / 2.0 + fe.ascent;

    int y = 0;
    for (int i = 0; i < n; i++) {
        MenuNode *node = rows[i].node;
        int x = MENU_PAD_X + rows[i].depth * MENU_INDENT;

        if (strcmp(node->type, "separator") == 0) {
            set_rgb(cr, pal.separator);
            cairo_set_line_width(cr, 1);
            cairo_move_to(cr, 6, y + menu_sep_h / 2.0 + 0.5);
            cairo_line_to(cr, menu.width - 6, y + menu_sep_h / 2.0 + 0.5);
            cairo_stroke(cr);
            y += menu_sep_h;
            continue;
        }

        if (i == menu.hover_index && node->enabled) {
            set_rgb(cr, pal.hover);
            cairo_rectangle(cr, 0, y, menu.width, menu_item_h);
            cairo_fill(cr);
        }

        if (node->enabled)
            set_rgb(cr, pal.text);
        else
            set_rgb(cr, pal.disabled);

        int tx = x;
        if (node->toggle_type[0] != '\0') {
            menu_draw_toggle(cr, x, y + menu_item_h / 2.0, node);
            tx += MENU_MARK_W;
        }

        cairo_move_to(cr, tx, y + baseline);
        cairo_show_text(cr, node->label);

        if (node->has_children)
            menu_draw_arrow(cr, menu.width - 11,
                            y + menu_item_h / 2.0, node->expanded);
        y += menu_item_h;
    }

    cairo_destroy(cr);
    cairo_surface_destroy(xs);
    XFlush(dpy);
}

static void menu_layout_and_show(void) {
    MenuRow rows[128];
    int n = menu_flatten(rows, 128);
    if (n == 0) {
        menu_close();
        return;
    }

    /* measure */
    cairo_surface_t *meas =
        cairo_image_surface_create(CAIRO_FORMAT_ARGB32, 1, 1);
    cairo_t *mcr = cairo_create(meas);
    theme_apply_font(mcr, menu.screen, MENU_FONT_SIZE);
    menu_update_metrics(mcr);

    int w = 120, h = 0;
    for (int i = 0; i < n; i++) {
        MenuNode *node = rows[i].node;
        if (strcmp(node->type, "separator") == 0) {
            h += menu_sep_h;
            continue;
        }
        cairo_text_extents_t e;
        cairo_text_extents(mcr, node->label, &e);
        int iw = (int)e.width + MENU_PAD_X * 2 +
                 rows[i].depth * MENU_INDENT + 40 +
                 (node->toggle_type[0] != '\0' ? MENU_MARK_W : 0);
        if (iw > w)
            w = iw;
        h += menu_item_h;
    }
    cairo_destroy(mcr);
    cairo_surface_destroy(meas);

    int screen = menu.screen;
    int sw = DisplayWidth(dpy, screen);
    int sh = DisplayHeight(dpy, screen);
    int x = menu.pos_x, y = menu.pos_y;
    if (x + w > sw) x = sw - w - 2;
    if (y + h > sh) y = menu.pos_y - h;   /* trays usually at an edge */
    if (x < 0) x = 0;
    if (y < 0) y = 0;

    menu.width = w;
    menu.height = h;

    if (menu.win == None) {
        SniPalette pal = palette_for_screen(menu.screen);
        XSetWindowAttributes attrs;
        attrs.override_redirect = True;
        attrs.background_pixel = rgb_pixel(pal.bg);
        attrs.border_pixel = rgb_pixel(pal.border);
        attrs.event_mask = ExposureMask | ButtonPressMask | ButtonReleaseMask |
                           PointerMotionMask | LeaveWindowMask;
        attrs.save_under = True;
        menu.win = XCreateWindow(dpy, RootWindow(dpy, screen), x, y, w, h, 1,
                                 CopyFromParent, InputOutput, CopyFromParent,
                                 CWOverrideRedirect | CWBackPixel |
                                 CWBorderPixel | CWEventMask | CWSaveUnder,
                                 &attrs);
        XMapRaised(dpy, menu.win);
        XGrabPointer(dpy, menu.win, True,
                     ButtonPressMask | ButtonReleaseMask | PointerMotionMask,
                     GrabModeAsync, GrabModeAsync, None, None, CurrentTime);
    } else {
        XMoveResizeWindow(dpy, menu.win, x, y, w, h);
    }
    menu.visible = true;
    menu_render();
}

/* dbusmenu Event("clicked") */
static void menu_send_clicked(Item *it, int32_t id) {
    sd_bus_message *m = NULL;
    int r = sd_bus_message_new_method_call(bus, &m, it->service,
                                           it->menu_path, DBUSMENU_IFACE,
                                           "Event");
    if (r < 0)
        return;
    sd_bus_message_append(m, "is", id, "clicked");
    sd_bus_message_append(m, "v", "s", "");
    sd_bus_message_append(m, "u", (uint32_t)(now_ms() / 1000));
    sd_bus_call_async(bus, NULL, m, NULL, NULL, 0);
    sd_bus_message_unref(m);
}

static void menu_handle_click(int win_x, int win_y) {
    if (win_x < 0 || win_y < 0 || win_x >= menu.width || win_y >= menu.height) {
        menu_close();
        return;
    }

    MenuRow rows[128];
    int n = menu_flatten(rows, 128);

    int y = 0;
    for (int i = 0; i < n; i++) {
        MenuNode *node = rows[i].node;
        int rh = strcmp(node->type, "separator") == 0 ? menu_sep_h : menu_item_h;
        if (win_y >= y && win_y < y + rh) {
            if (strcmp(node->type, "separator") == 0)
                return;
            if (node->has_children) {
                node->expanded = !node->expanded;
                menu_layout_and_show();
                return;
            }
            if (!node->enabled)
                return;
            Item *it = menu.item;
            int32_t id = node->id;
            menu_close();
            if (it)
                menu_send_clicked(it, id);
            return;
        }
        y += rh;
    }
}

static int menu_hit_index(int win_y) {
    MenuRow rows[128];
    int n = menu_flatten(rows, 128);
    int y = 0;
    for (int i = 0; i < n; i++) {
        int rh = strcmp(rows[i].node->type, "separator") == 0
                     ? menu_sep_h : menu_item_h;
        if (win_y >= y && win_y < y + rh)
            return i;
        y += rh;
    }
    return -1;
}

/* GetLayout reply */
static int menu_layout_cb(sd_bus_message *m, void *userdata,
                          sd_bus_error *ret_error) {
    (void)userdata; (void)ret_error;

    if (!menu.pending)
        return 0;
    menu.pending = false;

    if (sd_bus_message_is_method_error(m, NULL)) {
        const sd_bus_error *e = sd_bus_message_get_error(m);
        log_info("GetLayout failed: %s", e && e->message ? e->message : "?");
        /* fall back to the item's ContextMenu method */
        if (menu.item)
            sd_bus_call_method_async(bus, NULL, menu.item->service,
                                     menu.item->path, SNI_ITEM_IFACE,
                                     "ContextMenu", NULL, NULL, "ii",
                                     menu.pos_x, menu.pos_y);
        menu.item = NULL;
        return 0;
    }

    sd_bus_message_read(m, "u", &menu.revision);
    menu_free_nodes(menu.root);
    menu.root = menu_parse_node(m);
    if (!menu.root || !menu.root->children) {
        menu_close();
        return 0;
    }
    menu_layout_and_show();
    return 0;
}

static void menu_popup(Item *it, int root_x, int root_y) {
    menu_close();
    tooltip_hide();

    menu.item = it;
    menu.screen = it->screen;
    menu.pos_x = root_x;
    menu.pos_y = root_y;
    menu.pending = true;
    menu.hover_index = -1;

    /* AboutToShow (fire and forget), then GetLayout */
    sd_bus_call_method_async(bus, NULL, it->service, it->menu_path,
                             DBUSMENU_IFACE, "AboutToShow", NULL, NULL,
                             "i", 0);

    static const char *props[] = {
        "label", "enabled", "visible", "type",
        "toggle-type", "toggle-state", "children-display", NULL
    };
    sd_bus_message *call = NULL;
    int r = sd_bus_message_new_method_call(bus, &call, it->service,
                                           it->menu_path, DBUSMENU_IFACE,
                                           "GetLayout");
    if (r < 0) {
        menu.pending = false;
        return;
    }
    sd_bus_message_append(call, "ii", 0, -1);
    sd_bus_message_append_strv(call, (char **)props);
    sd_bus_call_async(bus, NULL, call, menu_layout_cb, NULL, 0);
    sd_bus_message_unref(call);
}

/* --------------------------------------------------------------------
 * SNI item: property parsing
 * -------------------------------------------------------------------- */

static void item_free_pixmaps(Item *it) {
    for (int i = 0; i < it->npixmaps; i++)
        free(it->pixmaps[i].data);
    free(it->pixmaps);
    it->pixmaps = NULL;
    it->npixmaps = 0;
}

/* m positioned at an a(iiay); replaces it->pixmaps. Returns true if changed. */
static bool item_parse_pixmaps(Item *it, sd_bus_message *m) {
    RawPixmap pms[16];
    int n = 0;

    if (sd_bus_message_enter_container(m, 'a', "(iiay)") < 0)
        return false;
    while (sd_bus_message_enter_container(m, 'r', "iiay") > 0) {
        int32_t w = 0, h = 0;
        const void *bytes = NULL;
        size_t len = 0;
        sd_bus_message_read(m, "ii", &w, &h);
        sd_bus_message_read_array(m, 'y', &bytes, &len);
        sd_bus_message_exit_container(m);

        if (n < 16 && w > 0 && h > 0 && len >= (size_t)w * h * 4) {
            pms[n].w = w;
            pms[n].h = h;
            pms[n].data = malloc(len);
            memcpy(pms[n].data, bytes, len);
            n++;
        }
    }
    sd_bus_message_exit_container(m);

    /* cheap change detection */
    bool changed = n != it->npixmaps;
    if (!changed) {
        for (int i = 0; i < n; i++) {
            if (pms[i].w != it->pixmaps[i].w || pms[i].h != it->pixmaps[i].h ||
                memcmp(pms[i].data, it->pixmaps[i].data,
                       (size_t)pms[i].w * pms[i].h * 4) != 0) {
                changed = true;
                break;
            }
        }
    }

    if (!changed) {
        for (int i = 0; i < n; i++)
            free(pms[i].data);
        return false;
    }

    item_free_pixmaps(it);
    if (n > 0) {
        it->pixmaps = calloc(n, sizeof(RawPixmap));
        memcpy(it->pixmaps, pms, n * sizeof(RawPixmap));
        it->npixmaps = n;
    }
    return true;
}

/* m positioned at a (sa(iiay)ss) ToolTip struct */
static void item_parse_tooltip(Item *it, sd_bus_message *m) {
    if (sd_bus_message_enter_container(m, 'r', "sa(iiay)ss") < 0)
        return;
    const char *icon = NULL, *title = NULL, *text = NULL;
    sd_bus_message_read(m, "s", &icon);
    sd_bus_message_skip(m, "a(iiay)");
    sd_bus_message_read(m, "ss", &title, &text);
    strip_sni_markup(it->tooltip_title, sizeof(it->tooltip_title), title);
    strip_sni_markup(it->tooltip_text,  sizeof(it->tooltip_text),  text);
    sd_bus_message_exit_container(m);
}

/*
 * Consume one property value. `m` must be positioned at the variant.
 * Returns true if an icon-affecting property changed.
 */
static bool item_consume_property(Item *it, const char *key, sd_bus_message *m) {
    const char *contents = NULL;
    char type;
    bool icon_changed = false;

    if (sd_bus_message_peek_type(m, &type, &contents) < 0)
        return false;
    if (type != 'v') {
        sd_bus_message_skip(m, NULL);
        return false;
    }
    sd_bus_message_enter_container(m, 'v', contents);

    if (strcmp(key, "Id") == 0 && contents[0] == 's') {
        const char *s = NULL;
        sd_bus_message_read(m, "s", &s);
        snprintf(it->id, sizeof(it->id), "%s", s ? s : "");
        if (it->win != None && it->id[0])
            XStoreName(dpy, it->win, it->id);
    } else if (strcmp(key, "Title") == 0 && contents[0] == 's') {
        const char *s = NULL;
        sd_bus_message_read(m, "s", &s);
        snprintf(it->title, sizeof(it->title), "%s", s ? s : "");
    } else if (strcmp(key, "Status") == 0 && contents[0] == 's') {
        const char *s = NULL;
        sd_bus_message_read(m, "s", &s);
        snprintf(it->status, sizeof(it->status), "%s", s ? s : "");
        if (it->win != None)
            item_set_xembed_info(it, strcasecmp(it->status, "Passive") != 0);
    } else if (strcmp(key, "IconName") == 0 && contents[0] == 's') {
        const char *s = NULL;
        sd_bus_message_read(m, "s", &s);
        if (strcmp(it->icon_name, s ? s : "") != 0) {
            snprintf(it->icon_name, sizeof(it->icon_name), "%s", s ? s : "");
            icon_changed = true;
        }
    } else if (strcmp(key, "IconThemePath") == 0 && contents[0] == 's') {
        const char *s = NULL;
        sd_bus_message_read(m, "s", &s);
        if (strcmp(it->icon_theme_path, s ? s : "") != 0) {
            snprintf(it->icon_theme_path, sizeof(it->icon_theme_path),
                     "%s", s ? s : "");
            icon_changed = true;
        }
    } else if (strcmp(key, "IconPixmap") == 0 && strcmp(contents, "a(iiay)") == 0) {
        icon_changed = item_parse_pixmaps(it, m);
    } else if (strcmp(key, "ToolTip") == 0 && strcmp(contents, "(sa(iiay)ss)") == 0) {
        item_parse_tooltip(it, m);
    } else if (strcmp(key, "Menu") == 0 && contents[0] == 'o') {
        const char *s = NULL;
        sd_bus_message_read(m, "o", &s);
        snprintf(it->menu_path, sizeof(it->menu_path), "%s", s ? s : "");
    } else if (strcmp(key, "ItemIsMenu") == 0 && contents[0] == 'b') {
        int b = 0;
        sd_bus_message_read(m, "b", &b);
        it->item_is_menu = b;
    } else {
        sd_bus_message_skip(m, contents);
    }

    sd_bus_message_exit_container(m);
    return icon_changed;
}

/* --------------------------------------------------------------------
 * SNI item: async property fetching
 * -------------------------------------------------------------------- */

static Item *find_item(const char *service, const char *path) {
    for (Item *it = items; it; it = it->next)
        if (strcmp(it->service, service) == 0 &&
            (!path || strcmp(it->path, path) == 0))
            return it;
    return NULL;
}

/* Initial GetAll reply */
static int item_getall_cb(sd_bus_message *m, void *userdata,
                          sd_bus_error *ret_error) {
    (void)ret_error;
    Item *it = userdata;

    /* item might have been removed while the call was in flight */
    bool alive = false;
    for (Item *p = items; p; p = p->next)
        if (p == it) { alive = true; break; }
    if (!alive)
        return 0;

    if (sd_bus_message_is_method_error(m, NULL)) {
        const sd_bus_error *e = sd_bus_message_get_error(m);
        log_error("%s: GetAll failed: %s", it->service,
                  e && e->message ? e->message : "?");
        return 0;
    }

    sd_bus_message_enter_container(m, 'a', "{sv}");
    while (sd_bus_message_enter_container(m, 'e', "sv") > 0) {
        const char *key = NULL;
        sd_bus_message_read(m, "s", &key);
        item_consume_property(it, key, m);
        sd_bus_message_exit_container(m);
    }
    sd_bus_message_exit_container(m);

    log_info("%s: id='%s' menu='%s' pid=%d screen=%d%s",
             it->service, it->id, it->menu_path, (int)it->pid, it->screen,
             it->screen_resolved ? "" : " (unresolved, will retry)");

    // The window was created with XEMBED_MAPPED=0 to avoid flashing
    // Passive items.  Now that we have the authoritative status, apply it.
    // (Redundant if item_consume_property already handled a Status key
    // above - item_set_xembed_info is idempotent.)
    if (it->win != None)
        item_set_xembed_info(it, strcasecmp(it->status, "Passive") != 0);

    item_rebuild_icon(it);
    return 0;
}

static void item_fetch_all(Item *it) {
    sd_bus_call_method_async(bus, NULL, it->service, it->path,
                             "org.freedesktop.DBus.Properties", "GetAll",
                             item_getall_cb, it, "s", SNI_ITEM_IFACE);
}

/* NewIcon refresh: re-Get IconName, IconPixmap, IconThemePath
 * (same trio snixembed refreshes in handle_new_icon).  Each Get reply
 * carries its property name in an IconPropCtx. */
typedef struct {
    Item *item;
    char  key[32];
} IconPropCtx;

static int item_iconprop_reply(sd_bus_message *m, void *userdata,
                               sd_bus_error *ret_error) {
    (void)ret_error;
    IconPropCtx *ctx = userdata;
    Item *it = ctx->item;

    bool alive = false;
    for (Item *p = items; p; p = p->next)
        if (p == it) { alive = true; break; }

    if (alive) {
        if (!sd_bus_message_is_method_error(m, NULL)) {
            if (item_consume_property(it, ctx->key, m))
                it->icon_fetch_changes++;
        }
        if (--it->icon_fetch_pending == 0 && it->icon_fetch_changes > 0) {
            it->icon_fetch_changes = 0;
            item_rebuild_icon(it);
        }
    }
    free(ctx);
    return 0;
}

static void item_refresh_icon_props(Item *it) {
    static const char *keys[] = { "IconName", "IconPixmap", "IconThemePath" };

    if (it->icon_fetch_pending > 0)
        return;  /* a refresh is already in flight */

    it->icon_fetch_changes = 0;
    for (size_t i = 0; i < sizeof(keys) / sizeof(keys[0]); i++) {
        IconPropCtx *ctx = calloc(1, sizeof(*ctx));
        ctx->item = it;
        snprintf(ctx->key, sizeof(ctx->key), "%s", keys[i]);
        int r = sd_bus_call_method_async(
            bus, NULL, it->service, it->path,
            "org.freedesktop.DBus.Properties", "Get",
            item_iconprop_reply, ctx, "ss", SNI_ITEM_IFACE, keys[i]);
        if (r >= 0)
            it->icon_fetch_pending++;
        else
            free(ctx);
    }
}

typedef struct {
    Item *item;
    char  key[32];
} SinglePropCtx;

static int item_singleprop_reply(sd_bus_message *m, void *userdata,
                                 sd_bus_error *ret_error) {
    (void)ret_error;
    SinglePropCtx *ctx = userdata;
    Item *it = ctx->item;

    bool alive = false;
    for (Item *p = items; p; p = p->next)
        if (p == it) { alive = true; break; }

    if (alive && !sd_bus_message_is_method_error(m, NULL))
        item_consume_property(it, ctx->key, m);
    free(ctx);
    return 0;
}

static void item_refresh_prop(Item *it, const char *key) {
    SinglePropCtx *ctx = calloc(1, sizeof(*ctx));
    ctx->item = it;
    snprintf(ctx->key, sizeof(ctx->key), "%s", key);
    if (sd_bus_call_method_async(bus, NULL, it->service, it->path,
                                 "org.freedesktop.DBus.Properties", "Get",
                                 item_singleprop_reply, ctx, "ss",
                                 SNI_ITEM_IFACE, key) < 0)
        free(ctx);
}

/* SNI signal dispatcher (NewIcon / NewTitle / NewToolTip / NewStatus) */
static int item_signal_cb(sd_bus_message *m, void *userdata,
                          sd_bus_error *ret_error) {
    (void)ret_error;
    Item *it = userdata;

    bool alive = false;
    for (Item *p = items; p; p = p->next)
        if (p == it) { alive = true; break; }
    if (!alive)
        return 0;

    const char *member = sd_bus_message_get_member(m);
    if (!member)
        return 0;

    if (strcmp(member, "NewIcon") == 0 ||
        strcmp(member, "NewAttentionIcon") == 0)
        item_refresh_icon_props(it);
    else if (strcmp(member, "NewTitle") == 0)
        item_refresh_prop(it, "Title");
    else if (strcmp(member, "NewToolTip") == 0)
        item_refresh_prop(it, "ToolTip");
    else if (strcmp(member, "NewStatus") == 0) {
        /* NewStatus carries the status as an argument */
        const char *s = NULL;
        if (sd_bus_message_read(m, "s", &s) >= 0 && s) {
            snprintf(it->status, sizeof(it->status), "%s", s);
            if (it->win != None)
                item_set_xembed_info(it,
                                     strcasecmp(it->status, "Passive") != 0);
        }
    }
    return 0;
}

/* --------------------------------------------------------------------
 * SNI item lifecycle
 * -------------------------------------------------------------------- */

static void item_try_resolve_screen(Item *it) {
    int s = screen_for_pid(it->pid);
    it->resolve_tries++;
    if (s >= 0 && s < nscreens) {
        it->screen_resolved = true;
        if (s != it->screen) {
            item_move_to_screen(it, s);
        }
        log_info("%s: resolved to screen %d (pid %d, try %d)",
                 it->service, s, (int)it->pid, it->resolve_tries);
    }
}

static Item *item_add(const char *service, const char *path,
                      const char *owner, pid_t pid) {
    Item *it = calloc(1, sizeof(Item));
    snprintf(it->service, sizeof(it->service), "%s", service);
    snprintf(it->path, sizeof(it->path), "%s", path);
    snprintf(it->owner, sizeof(it->owner), "%s", owner ? owner : service);
    snprintf(it->registered_as, sizeof(it->registered_as), "%s%s",
             service, path);
    snprintf(it->status, sizeof(it->status), "Active");
    it->pid = pid;
    it->screen = default_screen_fallback;

    /* pid -> screen, with retries if the app hasn't mapped windows yet */
    int s = screen_for_pid(pid);
    if (s >= 0 && s < nscreens) {
        it->screen = s;
        it->screen_resolved = true;
    }
    it->resolve_tries = 1;

    /* subscribe to all SNI signals from this item */
    sd_bus_match_signal(bus, &it->sig_slot, it->service, it->path,
                        SNI_ITEM_IFACE, NULL, item_signal_cb, it);

    it->next = items;
    items = it;

    item_create_window(it);
    item_dock(it);
    item_fetch_all(it);

    log_info("%s appeared (path %s, pid %d, screen %d%s)",
             service, path, (int)pid, it->screen,
             it->screen_resolved ? "" : " [fallback]");
    return it;
}

static void item_remove(Item *it) {
    Item **pp = &items;
    while (*pp && *pp != it)
        pp = &(*pp)->next;
    if (!*pp)
        return;
    *pp = it->next;

    log_info("%s vanished", it->service);

    if (menu.item == it)
        menu_close();
    if (tooltip.pending_item == it)
        tooltip_hide();

    if (it->sig_slot)
        sd_bus_slot_unref(it->sig_slot);
    item_undock_destroy(it);
    if (it->icon)
        cairo_surface_destroy(it->icon);
    item_free_pixmaps(it);
    free(it);
}

/* --------------------------------------------------------------------
 * Mouse interaction on icon windows
 * -------------------------------------------------------------------- */

static int activate_err_cb(sd_bus_message *m, void *userdata,
                           sd_bus_error *ret_error) {
    (void)ret_error;
    Item *it = userdata;
    bool alive = false;
    for (Item *p = items; p; p = p->next)
        if (p == it) { alive = true; break; }
    if (!alive)
        return 0;

    if (sd_bus_message_is_method_error(m, NULL)) {
        /* Activate unsupported (common with appindicator-only apps):
         * remember that and treat left click as menu from now on. */
        it->activate_unsupported = true;
        if (it->menu_path[0] != '\0') {
            Window dummy_root, dummy_child;
            int rx = 0, ry = 0, wx, wy;
            unsigned int mask;
            XQueryPointer(dpy, RootWindow(dpy, it->screen), &dummy_root,
                          &dummy_child, &rx, &ry, &wx, &wy, &mask);
            menu_popup(it, rx, ry);
        }
    }
    return 0;
}

/* Press: only dismiss the tooltip and handle scroll.  Buttons 1-3 are
 * acted on at ButtonRelease instead -- between press and release the X
 * server holds an implicit pointer grab on OUR behalf, and apps like
 * nm-tray respond to Activate by popping a Qt::Popup menu that needs
 * its own pointer grab to auto-close.  Grabbing fails while our
 * implicit grab is live, leaving an unclosable menu behind on every
 * click (stock snixembed has the same bug: GtkStatusIcon fires
 * 'activate' on button-press).  By release time the implicit grab is
 * gone and the app's grab succeeds. */
static void item_button_press(Item *it, XButtonEvent *ev) {
    tooltip_hide();

    switch (ev->button) {
    case Button4:
        sd_bus_call_method_async(bus, NULL, it->service, it->path,
                                 SNI_ITEM_IFACE, "Scroll", NULL, NULL,
                                 "is", -1, "vertical");
        break;
    case Button5:
        sd_bus_call_method_async(bus, NULL, it->service, it->path,
                                 SNI_ITEM_IFACE, "Scroll", NULL, NULL,
                                 "is", 1, "vertical");
        break;
    case 6:
        sd_bus_call_method_async(bus, NULL, it->service, it->path,
                                 SNI_ITEM_IFACE, "Scroll", NULL, NULL,
                                 "is", -1, "horizontal");
        break;
    case 7:
        sd_bus_call_method_async(bus, NULL, it->service, it->path,
                                 SNI_ITEM_IFACE, "Scroll", NULL, NULL,
                                 "is", 1, "horizontal");
        break;
    }
}

static void item_button_release(Item *it, XButtonEvent *ev) {
    /* classic click semantics: ignore the release if the pointer was
     * dragged off the icon (the implicit grab still routes it to us) */
    if (ev->x < 0 || ev->y < 0 ||
        ev->x >= (int)it->win_w || ev->y >= (int)it->win_h)
        return;

    switch (ev->button) {
    case Button1:
        if (it->item_is_menu ||
            (it->activate_unsupported && it->menu_path[0] != '\0')) {
            menu_popup(it, ev->x_root, ev->y_root);
        } else {
            sd_bus_call_method_async(bus, NULL, it->service, it->path,
                                     SNI_ITEM_IFACE, "Activate",
                                     activate_err_cb, it, "ii",
                                     ev->x_root, ev->y_root);
        }
        break;
    case Button2:
        sd_bus_call_method_async(bus, NULL, it->service, it->path,
                                 SNI_ITEM_IFACE, "SecondaryActivate",
                                 NULL, NULL, "ii", ev->x_root, ev->y_root);
        break;
    case Button3:
        if (it->menu_path[0] != '\0')
            menu_popup(it, ev->x_root, ev->y_root);
        else
            sd_bus_call_method_async(bus, NULL, it->service, it->path,
                                     SNI_ITEM_IFACE, "ContextMenu",
                                     NULL, NULL, "ii",
                                     ev->x_root, ev->y_root);
        break;
    }
}

/* --------------------------------------------------------------------
 * StatusNotifierWatcher D-Bus service
 * -------------------------------------------------------------------- */

static bool looks_like_bus_name(const char *s) {
    if (!s || !*s)
        return false;
    if (s[0] == '/')
        return false;             /* object path: appindicator fallback */
    if (s[0] == ':')
        return true;              /* unique name */
    return strchr(s, '.') != NULL; /* well-known names contain a dot */
}

static int watcher_register_item(sd_bus_message *m, void *userdata,
                                 sd_bus_error *ret_error) {
    (void)userdata; (void)ret_error;

    const char *arg = NULL;
    int r = sd_bus_message_read(m, "s", &arg);
    if (r < 0)
        return r;

    const char *sender = sd_bus_message_get_sender(m);

    const char *service;
    const char *path;
    if (looks_like_bus_name(arg)) {
        service = arg;
        path = SNI_ITEM_OBJECT;
    } else {
        /* libappindicator passes an object path; the item lives on the
         * sender's connection */
        log_info("%s is not a service name, appindicator fallback on %s",
                 arg, sender ? sender : "?");
        service = sender;
        path = arg[0] == '/' ? arg : SNI_ITEM_OBJECT;
    }
    if (!service)
        return sd_bus_reply_method_return(m, NULL);

    if (find_item(service, path)) {
        log_info("%s (%s) already registered; ignoring", service, arg);
        return sd_bus_reply_method_return(m, NULL);
    }

    /* who is registering? -> pid -> screen */
    pid_t pid = -1;
    sd_bus_creds *creds = NULL;
    if (sd_bus_query_sender_creds(m, SD_BUS_CREDS_PID, &creds) >= 0) {
        sd_bus_creds_get_pid(creds, &pid);
        sd_bus_creds_unref(creds);
    }

    Item *it = item_add(service, path, sender, pid);

    sd_bus_emit_signal(bus, SNI_WATCHER_OBJECT, SNI_WATCHER_NAME,
                       "StatusNotifierItemRegistered", "s",
                       it->registered_as);
    sd_bus_emit_properties_changed(bus, SNI_WATCHER_OBJECT, SNI_WATCHER_NAME,
                                   "RegisteredStatusNotifierItems", NULL);

    return sd_bus_reply_method_return(m, NULL);
}

static int watcher_register_host(sd_bus_message *m, void *userdata,
                                 sd_bus_error *ret_error) {
    (void)userdata; (void)ret_error;
    /* we are the host; nothing to do (same as snixembed) */
    return sd_bus_reply_method_return(m, NULL);
}

static int watcher_prop_items(sd_bus *b, const char *p, const char *iface,
                              const char *prop, sd_bus_message *reply,
                              void *userdata, sd_bus_error *err) {
    (void)b; (void)p; (void)iface; (void)prop; (void)userdata; (void)err;
    int r = sd_bus_message_open_container(reply, 'a', "s");
    if (r < 0)
        return r;
    for (Item *it = items; it; it = it->next) {
        r = sd_bus_message_append(reply, "s", it->registered_as);
        if (r < 0)
            return r;
    }
    return sd_bus_message_close_container(reply);
}

static int watcher_prop_host_registered(sd_bus *b, const char *p,
                                        const char *iface, const char *prop,
                                        sd_bus_message *reply,
                                        void *userdata, sd_bus_error *err) {
    (void)b; (void)p; (void)iface; (void)prop; (void)userdata; (void)err;
    return sd_bus_message_append(reply, "b", 1);
}

static int watcher_prop_protocol(sd_bus *b, const char *p, const char *iface,
                                 const char *prop, sd_bus_message *reply,
                                 void *userdata, sd_bus_error *err) {
    (void)b; (void)p; (void)iface; (void)prop; (void)userdata; (void)err;
    return sd_bus_message_append(reply, "i", 0);
}

static const sd_bus_vtable watcher_vtable[] = {
    SD_BUS_VTABLE_START(0),
    SD_BUS_METHOD("RegisterStatusNotifierItem", "s", "",
                  watcher_register_item, SD_BUS_VTABLE_UNPRIVILEGED),
    SD_BUS_METHOD("RegisterStatusNotifierHost", "s", "",
                  watcher_register_host, SD_BUS_VTABLE_UNPRIVILEGED),
    SD_BUS_PROPERTY("RegisteredStatusNotifierItems", "as",
                    watcher_prop_items, 0,
                    SD_BUS_VTABLE_PROPERTY_EMITS_CHANGE),
    SD_BUS_PROPERTY("IsStatusNotifierHostRegistered", "b",
                    watcher_prop_host_registered, 0,
                    SD_BUS_VTABLE_PROPERTY_CONST),
    SD_BUS_PROPERTY("ProtocolVersion", "i",
                    watcher_prop_protocol, 0,
                    SD_BUS_VTABLE_PROPERTY_CONST),
    SD_BUS_SIGNAL("StatusNotifierItemRegistered", "s", 0),
    SD_BUS_SIGNAL("StatusNotifierItemUnregistered", "s", 0),
    SD_BUS_SIGNAL("StatusNotifierHostRegistered", NULL, 0),
    SD_BUS_VTABLE_END
};

/* Item lifetime: drop items whose bus connection went away */
static int name_owner_changed_cb(sd_bus_message *m, void *userdata,
                                 sd_bus_error *ret_error) {
    (void)userdata; (void)ret_error;

    const char *name = NULL, *old_owner = NULL, *new_owner = NULL;
    if (sd_bus_message_read(m, "sss", &name, &old_owner, &new_owner) < 0)
        return 0;
    if (new_owner && *new_owner)
        return 0;  /* name changed hands; only removals matter */

    Item *it = items;
    while (it) {
        Item *next = it->next;
        if (strcmp(it->service, name) == 0 || strcmp(it->owner, name) == 0) {
            char reg[512];
            snprintf(reg, sizeof(reg), "%s", it->registered_as);
            item_remove(it);
            sd_bus_emit_signal(bus, SNI_WATCHER_OBJECT, SNI_WATCHER_NAME,
                               "StatusNotifierItemUnregistered", "s", reg);
            sd_bus_emit_properties_changed(bus, SNI_WATCHER_OBJECT,
                                           SNI_WATCHER_NAME,
                                           "RegisteredStatusNotifierItems",
                                           NULL);
        }
        it = next;
    }
    return 0;
}

/* --------------------------------------------------------------------
 * X event handling
 * -------------------------------------------------------------------- */

static Item *item_by_window(Window w) {
    for (Item *it = items; it; it = it->next)
        if (it->win == w)
            return it;
    return NULL;
}

static void handle_x_event(XEvent *ev) {
    /* menu popup events first */
    if (menu.visible && menu.win != None) {
        switch (ev->type) {
        case Expose:
            if (ev->xexpose.window == menu.win && ev->xexpose.count == 0)
                menu_render();
            return;
        case MotionNotify:
            if (ev->xmotion.window == menu.win) {
                int idx = menu_hit_index(ev->xmotion.y);
                if (idx != menu.hover_index) {
                    menu.hover_index = idx;
                    menu_render();
                }
            }
            return;
        case ButtonPress: {
            /* with the pointer grab, all presses come to the menu win */
            int x = ev->xbutton.x, y = ev->xbutton.y;
            if (ev->xbutton.window != menu.win) {
                menu_close();
                return;
            }
            menu_handle_click(x, y);
            return;
        }
        case ButtonRelease:
            return;
        case LeaveNotify:
            if (ev->xcrossing.window == menu.win) {
                menu.hover_index = -1;
                menu_render();
            }
            return;
        }
    }

    switch (ev->type) {
    case Expose: {
        if (tooltip.win != None && ev->xexpose.window == tooltip.win)
            return; /* tooltip is painted at creation */
        Item *it = item_by_window(ev->xexpose.window);
        if (it && ev->xexpose.count == 0)
            item_redraw(it);
        break;
    }
    case ConfigureNotify: {
        Item *it = item_by_window(ev->xconfigure.window);
        if (it) {
            int oldh = it->win_h;
            it->win_w = ev->xconfigure.width;
            it->win_h = ev->xconfigure.height;
            if (it->win_h != oldh)
                item_rebuild_icon(it);   /* re-pick best pixmap/theme size */
            else
                item_redraw(it);
        }
        break;
    }
    case ReparentNotify: {
        Item *it = item_by_window(ev->xreparent.window);
        if (it) {
            if (ev->xreparent.parent == RootWindow(dpy, it->screen)) {
                /* tray went away and gave us back to the root: undock,
                 * wait for the new tray (stellar respawns stalonetray) */
                if (it->docked)
                    log_info("%s: tray on screen %d vanished, waiting",
                             it->service, it->screen);
                it->docked = false;
            } else {
                it->docked = true;
            }
        }
        break;
    }
    case DestroyNotify: {
        Item *it = item_by_window(ev->xdestroywindow.window);
        if (it) {
            /* tray destroyed our window: make a fresh one and re-dock
             * when a tray reappears */
            it->win = None;
            it->docked = false;
            item_create_window(it);
            item_dock(it);
        }
        break;
    }
    case ButtonPress: {
        Item *it = item_by_window(ev->xbutton.window);
        if (it)
            item_button_press(it, &ev->xbutton);
        break;
    }
    case ButtonRelease: {
        Item *it = item_by_window(ev->xbutton.window);
        if (it)
            item_button_release(it, &ev->xbutton);
        break;
    }
    case EnterNotify: {
        Item *it = item_by_window(ev->xcrossing.window);
        if (it) {
            tooltip.pending_item = it;
            tooltip.due_ms = now_ms() + TOOLTIP_DELAY_MS;
            tooltip.root_x = ev->xcrossing.x_root;
            tooltip.root_y = ev->xcrossing.y_root;
        }
        break;
    }
    case LeaveNotify: {
        Item *it = item_by_window(ev->xcrossing.window);
        if (it && (tooltip.pending_item == it || tooltip.visible))
            tooltip_hide();
        break;
    }
    case ClientMessage: {
        /* MANAGER announcements: a tray (re)appeared on some screen */
        if (ev->xclient.message_type == a_manager) {
            Atom sel = (Atom)ev->xclient.data.l[1];
            for (int s = 0; s < nscreens; s++) {
                if (sel == a_tray_selection[s]) {
                    log_info("tray manager appeared on screen %d", s);
                    for (Item *it = items; it; it = it->next)
                        if (it->screen == s && !it->docked)
                            item_dock(it);
                    break;
                }
            }
        }
        break;
    }
    }
}

/* --------------------------------------------------------------------
 * Periodic work: screen-resolution retries, dock retries, tooltips
 * -------------------------------------------------------------------- */

static long long next_periodic_ms;

static void periodic_work(void) {
    long long now = now_ms();

    /* tooltip delay */
    if (tooltip.pending_item && !tooltip.visible && now >= tooltip.due_ms) {
        Item *it = tooltip.pending_item;
        tooltip.pending_item = NULL;
        if (!menu.visible)
            tooltip_show(it, tooltip.root_x, tooltip.root_y);
    }

    if (now < next_periodic_ms)
        return;
    next_periodic_ms = now + RESOLVE_RETRY_MS;

    for (Item *it = items; it; it = it->next) {
        if (!it->screen_resolved && it->resolve_tries < RESOLVE_MAX_TRIES)
            item_try_resolve_screen(it);
        if (!it->docked)
            item_dock(it);   /* covers missed MANAGER messages */
    }
}

static bool work_pending(void) {
    if (tooltip.pending_item && !tooltip.visible)
        return true;
    for (Item *it = items; it; it = it->next)
        if (!it->docked ||
            (!it->screen_resolved && it->resolve_tries < RESOLVE_MAX_TRIES))
            return true;
    return false;
}

/* --------------------------------------------------------------------
 * Setup & main loop
 * -------------------------------------------------------------------- */

static void on_signal(int sig) {
    (void)sig;
    g_running = 0;
}

static void read_env_config(void) {
    const char *s;

    s = getenv("STELLAR_SNI_DEFAULT_SCREEN");
    if (s && *s) {
        int v = atoi(s);
        if (v >= 0)
            default_screen_fallback = v;
    }

    s = getenv("STELLAR_SNI_TINT");
    if (s) {
        if (strcasecmp(s, "gray") == 0 || strcasecmp(s, "grey") == 0 ||
            strcasecmp(s, "desaturate") == 0)
            tint_mode = TINT_GRAY;
        else if (strcasecmp(s, "color") == 0 || strcasecmp(s, "colorize") == 0)
            tint_mode = TINT_COLOR;
    }

    s = getenv("STELLAR_SNI_TINT_COLOR");
    if (s && strlen(s) == 6) {
        unsigned int rgb = (unsigned int)strtoul(s, NULL, 16);
        tint_r = ((rgb >> 16) & 0xff) / 255.0;
        tint_g = ((rgb >> 8) & 0xff) / 255.0;
        tint_b = (rgb & 0xff) / 255.0;
    }

    s = getenv("STELLAR_SNI_ICON_DIR");
    if (s && *s) {
        snprintf(override_icon_dir, sizeof(override_icon_dir), "%s", s);
    } else {
        const char *cfg = getenv("XDG_CONFIG_HOME");
        const char *home = getenv("HOME");
        if (cfg && *cfg)
            snprintf(override_icon_dir, sizeof(override_icon_dir),
                     "%s/stellar/sni-icons", cfg);
        else if (home)
            snprintf(override_icon_dir, sizeof(override_icon_dir),
                     "%s/.config/stellar/sni-icons", home);
    }
}

static int setup_x(void) {
    dpy = XOpenDisplay(NULL);
    if (!dpy) {
        log_error("cannot open display");
        return -1;
    }
    XSetErrorHandler(x_error_handler);

    nscreens = ScreenCount(dpy);
    if (nscreens > MAX_SNI_SCREENS)
        nscreens = MAX_SNI_SCREENS;
    xfd = ConnectionNumber(dpy);

    a_xembed_info     = XInternAtom(dpy, "_XEMBED_INFO", False);
    a_manager         = XInternAtom(dpy, "MANAGER", False);
    a_tray_opcode     = XInternAtom(dpy, "_NET_SYSTEM_TRAY_OPCODE", False);
    a_net_client_list = XInternAtom(dpy, "_NET_CLIENT_LIST", False);
    a_net_wm_pid      = XInternAtom(dpy, "_NET_WM_PID", False);

    for (int s = 0; s < nscreens; s++) {
        char name[64];
        snprintf(name, sizeof(name), "_NET_SYSTEM_TRAY_S%d", s);
        a_tray_selection[s] = XInternAtom(dpy, name, False);

        /* MANAGER client messages arrive via StructureNotify on roots */
        XSelectInput(dpy, RootWindow(dpy, s), StructureNotifyMask);
    }

    log_info("connected to %s, %d screen(s)",
             DisplayString(dpy), nscreens);
    return 0;
}

static int setup_bus(bool replace) {
    int r = sd_bus_open_user(&bus);
    if (r < 0) {
        log_error("cannot connect to session bus: %s", strerror(-r));
        return r;
    }

    r = sd_bus_add_object_vtable(bus, NULL, SNI_WATCHER_OBJECT,
                                 SNI_WATCHER_NAME, watcher_vtable, NULL);
    if (r < 0) {
        log_error("cannot register watcher object: %s", strerror(-r));
        return r;
    }

    uint64_t flags = replace
        ? (SD_BUS_NAME_ALLOW_REPLACEMENT | SD_BUS_NAME_REPLACE_EXISTING)
        : SD_BUS_NAME_ALLOW_REPLACEMENT;
    r = sd_bus_request_name(bus, SNI_WATCHER_NAME, flags);
    if (r < 0) {
        log_error("could not acquire %s: %s (another watcher running? "
                  "try --replace)", SNI_WATCHER_NAME, strerror(-r));
        return r;
    }

    sd_bus_match_signal(bus, &name_owner_slot,
                        "org.freedesktop.DBus", "/org/freedesktop/DBus",
                        "org.freedesktop.DBus", "NameOwnerChanged",
                        name_owner_changed_cb, NULL);

    /* announce ourselves as host so SNI apps register instead of
     * falling back to xembed on their own terms */
    sd_bus_emit_signal(bus, SNI_WATCHER_OBJECT, SNI_WATCHER_NAME,
                       "StatusNotifierHostRegistered", NULL);

    log_info("acquired %s", SNI_WATCHER_NAME);
    return 0;
}

int main(int argc, char **argv) {
    bool replace = false;
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "-v") == 0 || strcmp(argv[i], "--version") == 0) {
            printf("stellar-snitray 1.0\n");
            return 0;
        }
        if (strcmp(argv[i], "--replace") == 0)
            replace = true;
    }

    signal(SIGINT, on_signal);
    signal(SIGTERM, on_signal);
    signal(SIGPIPE, SIG_IGN);

    read_env_config();

    if (setup_x() < 0)
        return 1;
    if (setup_bus(replace) < 0)
        return 1;

    menu.hover_index = -1;

    while (g_running) {
        /* drain the bus */
        int r;
        while ((r = sd_bus_process(bus, NULL)) > 0)
            ;
        if (r < 0) {
            log_error("sd_bus_process: %s", strerror(-r));
            break;
        }

        /* drain X */
        while (XPending(dpy)) {
            XEvent ev;
            XNextEvent(dpy, &ev);
            handle_x_event(&ev);
        }

        periodic_work();

        /* wait for either fd */
        struct pollfd pfd[2];
        pfd[0].fd = xfd;
        pfd[0].events = POLLIN;
        pfd[0].revents = 0;
        pfd[1].fd = sd_bus_get_fd(bus);
        pfd[1].events = (short)sd_bus_get_events(bus);
        pfd[1].revents = 0;

        uint64_t bus_usec = UINT64_MAX;
        sd_bus_get_timeout(bus, &bus_usec);

        int timeout_ms;
        if (work_pending())
            timeout_ms = 200;
        else
            timeout_ms = 5000;
        if (bus_usec != UINT64_MAX) {
            /* sd-bus timeout is absolute CLOCK_MONOTONIC in usec */
            struct timespec ts;
            clock_gettime(CLOCK_MONOTONIC, &ts);
            uint64_t now_usec =
                (uint64_t)ts.tv_sec * 1000000ULL + ts.tv_nsec / 1000;
            int bus_ms = bus_usec > now_usec
                             ? (int)((bus_usec - now_usec) / 1000)
                             : 0;
            if (bus_ms < timeout_ms)
                timeout_ms = bus_ms;
        }

        poll(pfd, 2, timeout_ms);
    }

    log_info("shutting down");

    while (items)
        item_remove(items);
    menu_close();
    tooltip_hide();

    sd_bus_release_name(bus, SNI_WATCHER_NAME);
    if (name_owner_slot)
        sd_bus_slot_unref(name_owner_slot);
    sd_bus_unref(bus);
    XCloseDisplay(dpy);
    return 0;
}
