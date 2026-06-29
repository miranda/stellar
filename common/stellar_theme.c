// stellar_theme.c
// Theme/appearance transport: IPC requests + JSON parsing.  Pure C (sockets,
// cJSON, fontconfig) with NO nuklear dependency, so it can be linked into
// non-nuklear tools (snitray, etc).  The nuklear presentation half
// (stellar_theme_color, apply_nk_theme) lives in stellar_nk_theme.c and is
// linked only into nuklear apps.  Compile both into every support app that
// draws with nuklear, together with stellar_font.c and stellar_config.c.

#include "stellar_theme.h"
#include "stellar_font.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>
#include <poll.h>
#include <time.h>
#include "cJSON.h"

/* ---------- IPC plumbing ---------- */

// Connect to Stellar, send one command line, read one '\n'-terminated reply.
// Returns bytes read (>0) on success, -1 on failure.
static int stellar_ipc_request(const char *cmd, char *buf, size_t buf_size,
                               int timeout_ms) {
    const char *socket_path = getenv("STELLAR_SOCKET");
    if (!socket_path) {
        fprintf(stderr, "Error: STELLAR_SOCKET not set. Cannot reach Stellar.\n");
        return -1;
    }

    int sock = socket(AF_UNIX, SOCK_STREAM, 0);
    if (sock < 0) return -1;

    struct sockaddr_un addr = {0};
    addr.sun_family = AF_UNIX;
    strncpy(addr.sun_path, socket_path, sizeof(addr.sun_path) - 1);

    if (connect(sock, (struct sockaddr *)&addr, sizeof(addr)) != 0) {
        fprintf(stderr, "Failed to connect to Stellar IPC socket.\n");
        close(sock);
        return -1;
    }

    dprintf(sock, "%s\n", cmd);

    /* Read line-oriented, skipping asynchronous EVENT broadcasts.
     *
     * The DE broadcasts events (EVENT type=client_manage / unmap / destroy /
     * focus ...) to ALL connected clients via broadcast_line().  A client that
     * has just connected to issue a request is in that broadcast set, so if
     * the DE happens to broadcast while our request is in flight, the first
     * line we read can be an unrelated EVENT rather than our reply.  This bit
     * the restart dialog specifically: it connects right as the settings
     * window is being torn down, so it received "EVENT type=unmap/destroy"
     * lines and cJSON_Parse() choked on them.
     *
     * Replies to GET_* requests are either JSON (start '{') or a short scalar
     * (e.g. GET_SCREEN_FOR_DISPLAY -> "1"); none of them begin with "EVENT".
     * So we skip any line that is an event broadcast and return the first line
     * that is an actual reply.  We keep a rolling line buffer; bytes after a
     * skipped line's newline are retained for the next line. */
    struct pollfd pfd = { .fd = sock, .events = POLLIN };
    char acc[8192];
    size_t acc_len = 0;
    int got_reply = 0;
    size_t reply_len = 0;

    while (!got_reply && acc_len < sizeof(acc) - 1) {
        /* Process any complete lines already buffered before reading more. */
        char *nl;
        while ((nl = memchr(acc, '\n', acc_len)) != NULL) {
            size_t this_len = (size_t)(nl - acc);   /* length excluding '\n' */

            /* Is this line an asynchronous event broadcast?  If so, drop it
             * and shift the remainder down.  Tolerate a stray leading byte
             * before "EVENT" (we have observed "XEVENT") by checking for the
             * "EVENT type=" marker near the start of the line. */
            int is_event = 0;
            if (this_len >= 5 && memcmp(acc, "EVENT", 5) == 0) is_event = 1;
            else if (this_len >= 6 && memcmp(acc + 1, "EVENT", 5) == 0) is_event = 1;

            if (is_event) {
                size_t consumed = this_len + 1;     /* include the '\n' */
                memmove(acc, acc + consumed, acc_len - consumed);
                acc_len -= consumed;
                continue;                            /* check next buffered line */
            }

            /* Not an event: this is our reply.  Copy the line (without '\n')
             * into the caller's buffer. */
            reply_len = this_len < buf_size - 1 ? this_len : buf_size - 1;
            memcpy(buf, acc, reply_len);
            buf[reply_len] = '\0';
            got_reply = 1;
            break;
        }
        if (got_reply) break;

        int pr = poll(&pfd, 1, timeout_ms);
        if (pr <= 0) break;

        ssize_t n = read(sock, acc + acc_len, sizeof(acc) - 1 - acc_len);
        if (n <= 0) break;
        acc_len += (size_t)n;
    }
    close(sock);

    return got_reply ? (int)reply_len : -1;
}

static void json_copy_string(cJSON *obj, const char *key,
                             char *dst, size_t dst_size) {
    cJSON *item = cJSON_GetObjectItemCaseSensitive(obj, key);
    if (cJSON_IsString(item) && item->valuestring)
        snprintf(dst, dst_size, "%s", item->valuestring);
}

/* ---------- Display -> screen resolution ---------- */

// Ask the DE which Stellar screen owns a given X display string (e.g. ":0.1").
// Used by global helpers (the polkit agent) that have no STELLAR_SCREEN of
// their own and must resolve the screen per request from the requesting app's
// DISPLAY.  The DE matches against its own canonical display_name strings, so
// this is correct on non-:0 servers (Xephyr :3.0-:3.2 etc.) where parsing the
// numeric suffix client-side would be unreliable.  Returns the screen index
// (>= 0) on success, or -1 if the display is unknown or the DE is unreachable.
int stellar_screen_for_display(const char *display) {
    if (!display || display[0] == '\0') return -1;

    char cmd[128];
    snprintf(cmd, sizeof(cmd), "GET_SCREEN_FOR_DISPLAY %s", display);

    char buf[64];
    if (stellar_ipc_request(cmd, buf, sizeof(buf), 1000) < 0)
        return -1;

    // Reply is a single line: the screen index, or -1.
    int screen = -1;
    if (sscanf(buf, "%d", &screen) != 1) return -1;
    return screen;
}

// Ask the DE which Stellar screen owns a given X window id.  Like
// stellar_screen_for_display(), but resolves from the window itself, which is
// authoritative even when AwesomeWM has reparented the window (the common case
// for a portal file-chooser parent, where a per-screen DISPLAY suffix would be
// unavailable or unreliable).  The DE matches the window's screen root against
// its own ScreenState table.  Returns the screen index (>= 0) on success, or
// -1 if the window is unknown or the DE is unreachable.
int stellar_screen_for_window(unsigned long window_id) {
    if (window_id == 0) return -1;

    char cmd[64];
    snprintf(cmd, sizeof(cmd), "GET_SCREEN_FOR_WINDOW %lu", window_id);

    char buf[64];
    if (stellar_ipc_request(cmd, buf, sizeof(buf), 1000) < 0)
        return -1;

    // Reply is a single line: the screen index, or -1.
    int screen = -1;
    if (sscanf(buf, "%d", &screen) != 1) return -1;
    return screen;
}

void make_screen_display_name(
    const char *display_str,
    int screen_num,
    char *out,
    size_t out_sz
) {
    if (!display_str || !out || out_sz == 0) {
        return;
    }

    /*
     * X display strings are generally:
     *   hostname:display
     *   hostname:display.screen
     *   :display
     *   :display.screen
     *
     * We want to preserve hostname/display, but replace the screen suffix.
     */
    char base[256];
    snprintf(base, sizeof(base), "%s", display_str);

    char *last_colon = strrchr(base, ':');
    if (!last_colon) {
        /* Fallback: if format is weird, just use as-is and append screen */
        snprintf(out, out_sz, "%s.%d", base, screen_num);
        return;
    }

    char *dot_after_colon = strchr(last_colon, '.');
    if (dot_after_colon) {
        *dot_after_colon = '\0';
    }

    snprintf(out, out_sz, "%s.%d", base, screen_num);
}

/* ---------- Appearance (font) request ---------- */

int request_appearance(int screen_num, ThemeData *out) {
    char cmd[64];
    snprintf(cmd, sizeof(cmd), "GET_APPEARANCE screen=%d", screen_num);

    char buf[4096];
    if (stellar_ipc_request(cmd, buf, sizeof(buf), 2000) < 0) {
        fprintf(stderr, "No response from Stellar for GET_APPEARANCE screen=%d.\n",
               screen_num);
        return -1;
    }

    cJSON *json = cJSON_Parse(buf);
    if (!json) {
        fprintf(stderr, "Failed to parse appearance JSON for screen %d.\n", screen_num);
        return -1;
    }

    cJSON *err = cJSON_GetObjectItemCaseSensitive(json, "error");
    if (err && cJSON_IsString(err)) {
        fprintf(stderr, "Appearance error for screen %d: %s\n",
               screen_num, err->valuestring);
        cJSON_Delete(json);
        return -1;
    }

    cJSON *item;

    json_copy_string(json, "font_name", out->font, sizeof(out->font));
    json_copy_string(json, "font_path", out->font_path, sizeof(out->font_path));

    item = cJSON_GetObjectItemCaseSensitive(json, "font_size");
    if (cJSON_IsNumber(item)) out->font_size = (float)item->valuedouble;

    char font_unit[16] = "pt";
    item = cJSON_GetObjectItemCaseSensitive(json, "font_unit");
    if (cJSON_IsString(item) && item->valuestring) {
        snprintf(font_unit, sizeof(font_unit), "%s", item->valuestring);
    }
    snprintf(out->font_unit, sizeof(out->font_unit), "%s", font_unit);

    item = cJSON_GetObjectItemCaseSensitive(json, "font_is_bitmap");
    if (cJSON_IsBool(item)) out->font_is_bitmap = cJSON_IsTrue(item);

    item = cJSON_GetObjectItemCaseSensitive(json, "dpi");
    if (cJSON_IsNumber(item) && out->dpi <= 0) out->dpi = item->valueint;

    cJSON_Delete(json);

    // If the DE didn't resolve a path for us (older DE build), resolve
    // client-side from the family name.
    if (out->font_path[0] == '\0' && out->font[0] != '\0') {
        StellarFontInfo fi;
        // Clean call! The API now handles the pt/px math internally.
        if (stellar_font_resolve(out->font, out->font_size, out->font_unit, out->dpi, &fi) == 0) {
            snprintf(out->font_path, sizeof(out->font_path), "%s", fi.path);
            out->font_size = fi.size_px;
            snprintf(out->font_unit, sizeof(out->font_unit), "px"); // Lock to px for Cairo
            out->font_is_bitmap = fi.is_bitmap;
        }
    } else {
        // If path was provided by DE, we STILL must convert pt to px for Cairo
        if (strcmp(out->font_unit, "pt") == 0 && out->dpi > 0) {
            out->font_size = out->font_size * ((float)out->dpi / 72.0f);
            snprintf(out->font_unit, sizeof(out->font_unit), "px"); // Lock to px for Cairo
        }
    }

    return 0;
}

/* ---------- Theme request (colors from the Awesome relay) ---------- */

int request_theme_data(int screen_num, ThemeData *out) {
    memset(out, 0, sizeof(*out));
    out->screen = screen_num;

    char cmd[64];
    snprintf(cmd, sizeof(cmd), "GET_THEME_DATA screen=%d", screen_num);

    cJSON *json = NULL;
    char buf[8192];
    const int max_retries = 5;

    struct timespec sleep_time = {0};
    sleep_time.tv_sec = 0;
    sleep_time.tv_nsec = 400000000L; // 400ms

	// Autostart race condition guard: AwesomeWM and the IPC bridge take a moment
    // to handshake during a fresh login. We retry for up to 2 seconds.
    for (int attempt = 1; attempt <= max_retries; attempt++) {
        // Longer timeout since this is relayed through AwesomeWM.
        if (stellar_ipc_request(cmd, buf, sizeof(buf), 3000) < 0) {
            if (attempt == max_retries) {
                fprintf(stderr, "No response from Stellar for GET_THEME_DATA screen=%d.\n", screen_num);
                return -1;
            }
			nanosleep(&sleep_time, NULL);
            continue;
        }

        json = cJSON_Parse(buf);
        if (!json) {
            if (attempt == max_retries) {
                fprintf(stderr, "Failed to parse theme data JSON for screen %d.\n", screen_num);
                /* Dump exactly what we received so the failure is diagnosable */
                const char *eptr = cJSON_GetErrorPtr();
                size_t blen = strlen(buf);
                fprintf(stderr, "  raw reply: %zu bytes; parse error at offset %ld\n",
                       blen, eptr ? (long)(eptr - buf) : -1L);
                fputs("  reply[0:512]=<<", stdout);
                for (size_t i = 0; i < blen && i < 512; i++) {
                    unsigned char ch = (unsigned char)buf[i];
                    if (ch == '\n') fputs("\\n", stdout);
                    else if (ch == '\r') fputs("\\r", stdout);
                    else if (ch == '\t') fputs("\\t", stdout);
                    else if (ch >= 32 && ch < 127) putchar((int)ch);
                    else printf("\\x%02x", ch);
                }
                fputs(">>\n", stdout);
                return -1;
            }
			nanosleep(&sleep_time, NULL);
            continue;
        }

        cJSON *err = cJSON_GetObjectItemCaseSensitive(json, "error");
        if (err && cJSON_IsString(err)) {
            if (attempt == max_retries) {
                fprintf(stderr, "Theme data error for screen %d: %s\n", screen_num, err->valuestring);
                cJSON_Delete(json);
                return -1;
            }
            // Bridge is up but WM isn't ready. Discard and retry.
            cJSON_Delete(json);
            json = NULL;
			nanosleep(&sleep_time, NULL);
            continue;
        }

        // Success! We have a valid JSON payload.
        break;
    }

    cJSON *item;

    json_copy_string(json, "assets_path", out->assets_path, sizeof(out->assets_path));
    json_copy_string(json, "theme_dir", out->theme_dir, sizeof(out->theme_dir));

    item = cJSON_GetObjectItemCaseSensitive(json, "dpi");
    if (cJSON_IsNumber(item)) out->dpi = item->valueint;

    // The theme may still carry a font - kept as a fallback, but the
    // appearance setting below overrides it.
    json_copy_string(json, "font", out->font, sizeof(out->font));

    // Parse colors
    cJSON *colors = cJSON_GetObjectItemCaseSensitive(json, "colors");
    if (colors && cJSON_IsObject(colors)) {
        json_copy_string(colors, "bg_normal",     out->bg_normal,     sizeof(out->bg_normal));
        json_copy_string(colors, "bg_focus",      out->bg_focus,      sizeof(out->bg_focus));
        json_copy_string(colors, "bg_urgent",     out->bg_urgent,     sizeof(out->bg_urgent));
        json_copy_string(colors, "fg_normal",     out->fg_normal,     sizeof(out->fg_normal));
        json_copy_string(colors, "fg_focus",      out->fg_focus,      sizeof(out->fg_focus));
        json_copy_string(colors, "fg_urgent",     out->fg_urgent,     sizeof(out->fg_urgent));
        json_copy_string(colors, "border_normal", out->border_normal, sizeof(out->border_normal));
        json_copy_string(colors, "border_focus",  out->border_focus,  sizeof(out->border_focus));
        json_copy_string(colors, "nk_color_window",        out->nk_color_window,        sizeof(out->nk_color_window));
        json_copy_string(colors, "nk_color_text",          out->nk_color_text,          sizeof(out->nk_color_text));
        json_copy_string(colors, "nk_color_button",        out->nk_color_button,        sizeof(out->nk_color_button));
        json_copy_string(colors, "nk_color_button_hover",  out->nk_color_button_hover,  sizeof(out->nk_color_button_hover));
        json_copy_string(colors, "nk_color_button_active", out->nk_color_button_active, sizeof(out->nk_color_button_active));
        json_copy_string(colors, "nk_color_border",        out->nk_color_border,        sizeof(out->nk_color_border));
    }

    // Parse sizes
    cJSON *sizes = cJSON_GetObjectItemCaseSensitive(json, "sizes");
    if (sizes && cJSON_IsObject(sizes)) {
        item = cJSON_GetObjectItemCaseSensitive(sizes, "border_width");
        if (cJSON_IsNumber(item)) out->border_width = item->valueint;

        item = cJSON_GetObjectItemCaseSensitive(sizes, "useless_gap");
        if (cJSON_IsNumber(item)) out->useless_gap = item->valueint;
    }

    cJSON_Delete(json);

    // Fonts are an appearance setting, not part of the theme: fetch them from
    // Stellar's settings and override whatever the theme suggested.  A failure
    // here is non-fatal - we fall back to the theme font (resolved by name).
    if (request_appearance(screen_num, out) != 0 &&
        out->font_path[0] == '\0' && out->font[0] != '\0') {
        StellarFontInfo fi;

        // Safely determine the unit, defaulting to "pt" if nothing is set
        const char *unit = (out->font_unit[0] != '\0') ? out->font_unit : "pt";

        // Pass the unit parameter to the updated resolver
        if (stellar_font_resolve(out->font, out->font_size, unit, out->dpi, &fi) == 0) {
            snprintf(out->font_path, sizeof(out->font_path), "%s", fi.path);
            out->font_size = fi.size_px;
            snprintf(out->font_unit, sizeof(out->font_unit), "px"); // Lock to px for Cairo
            out->font_is_bitmap = fi.is_bitmap;
        }
    }

    fprintf(stderr, "Loaded theme data for screen %d: assets=%s dpi=%d font=%s (%.1fpx) path=%s\n",
           screen_num, out->assets_path, out->dpi,
           out->font, (double)out->font_size, out->font_path);

    return 0;
}
