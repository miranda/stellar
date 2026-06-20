// xresources.c

#include "stellar.h"
#include "xresources.h"

/* ---------- Session Helpers ---------- */

static void export_xrdb_cursor_env(void) {
    FILE *fp = popen("xrdb -query", "r");
    if (!fp) {
        log_error("Failed to run xrdb -query");
        return;
    }

    char line[256];
    while (fgets(line, sizeof(line), fp)) {
        if (strncmp(line, "Xcursor.theme:", 14) == 0) {
            char *val = line + 14;
            /* Strip leading whitespace */
            while (*val == ' ' || *val == '\t') val++;
            /* Strip trailing newlines */
            char *end = val + strlen(val) - 1;
            while (end > val && (*end == '\n' || *end == '\r')) *end-- = '\0';
            
            setenv("XCURSOR_THEME", val, 1);
            log_info("XRES: Exported XCURSOR_THEME=%s", val);
        }
        else if (strncmp(line, "Xcursor.size:", 13) == 0) {
            char *val = line + 13;
            while (*val == ' ' || *val == '\t') val++;
            char *end = val + strlen(val) - 1;
            while (end > val && (*end == '\n' || *end == '\r')) *end-- = '\0';
            
            setenv("XCURSOR_SIZE", val, 1);
            log_info("XRES: Exported XCURSOR_SIZE=%s", val);
        }
    }
    pclose(fp);
}

static int run_xrdb_merge_file(const char *path) {
    if (!path) {
        errno = EINVAL;
        return -1;
    }

    pid_t pid;
    char *const argv[] = {
        "xrdb",
		"-all",
        "-merge",
        (char *)path,
        NULL,
    };

    int spawn_rc = posix_spawnp(&pid, "xrdb", NULL, NULL, argv, environ);
    if (spawn_rc != 0) {
        errno = spawn_rc;
        return -1;
    }

    int status = 0;
    if (waitpid(pid, &status, 0) < 0) {
        return -1;
    }

    if (WIFEXITED(status)) {
        return WEXITSTATUS(status);
    }

    if (WIFSIGNALED(status)) {
        errno = EINTR;
        return 128 + WTERMSIG(status);
    }

    errno = ECHILD;
    return -1;
}

static void maybe_merge_one_xresources_file(
    const char *label,
    const char *path
) {
    if (!path || !*path) {
        return;
    }

    if (access(path, R_OK) != 0) {
        if (errno != ENOENT) {
            log_error(
                "XRES: %s exists check failed for %s: %s",
                label,
                path,
                strerror(errno)
            );
        } else {
            log_info("XRES: %s not present: %s", label, path);
        }
        return;
    }

    log_info("XRES: Merging %s from %s", label, path);

    int rc = run_xrdb_merge_file(path);
    if (rc == 0) {
        log_info("XRES: Successfully merged %s", label);
    } else {
        log_error(
            "XRES: xrdb failed for %s (rc=%d, errno=%d: %s)",
            label,
            rc,
            errno,
            strerror(errno)
        );
    }
}

void merge_session_xresources(void) {
    const char *home_dir = get_user_home_dir();
    if (!home_dir) {
        log_error("XRES: No home dir found");
        return;
    }

    const char *display = getenv("DISPLAY");
    if (!display || !*display) {
        log_error("XRES: DISPLAY is not set");
        return;
    }

    log_info("XRES: DISPLAY=%s", display);

    char vendor_xr[PATH_MAX];
    snprintf(vendor_xr, sizeof(vendor_xr), "%s/Xresources", STELLAR_SHARE_PATH);
    char generated_xr[PATH_MAX];
    snprintf(generated_xr, sizeof(generated_xr), "%s/.cache/stellar/Xresources", home_dir);
    char user_xr[PATH_MAX];
    snprintf(user_xr, sizeof(user_xr), "%s/.Xresources", home_dir);

    /* DE/vendor defaults */
    maybe_merge_one_xresources_file("vendor defaults", vendor_xr);
    /* Generated cached */
    maybe_merge_one_xresources_file("generated resources", generated_xr);
    /* User overrides */
    maybe_merge_one_xresources_file("user resources", user_xr);
}

static char *xstrdup_range(const char *s, size_t n) {
    char *out = malloc(n + 1);
    if (!out) {
        return NULL;
    }
    memcpy(out, s, n);
    out[n] = '\0';
    return out;
}

static char *get_root_text_property(Display *dpy, int screen_num,
                                    const char *prop_name) {
    Atom prop = XInternAtom(dpy, prop_name, False);
    Window root = RootWindow(dpy, screen_num);

    Atom actual_type = None;
    int actual_format = 0;
    unsigned long nitems = 0;
    unsigned long bytes_after = 0;
    unsigned char *data = NULL;

    int rc = XGetWindowProperty(dpy, root, prop, 0, 1L << 20, False,
                                XA_STRING, &actual_type, &actual_format,
                                &nitems, &bytes_after, &data);

    if (rc != Success || actual_type != XA_STRING || actual_format != 8 ||
        !data) {
        if (data) {
            XFree(data);
        }
        return strdup("");
    }

    char *out = xstrdup_range((const char *)data, nitems);
    XFree(data);
    return out;
}

static bool set_root_text_property(Display *dpy, int screen_num,
                                   const char *prop_name,
                                   const char *text) {
    Atom prop = XInternAtom(dpy, prop_name, False);
    Window root = RootWindow(dpy, screen_num);

    XChangeProperty(dpy, root, prop, XA_STRING, 8, PropModeReplace,
                    (const unsigned char *)text, (int)strlen(text));
    return true;
}

static void string_set_free(StringSet *set) {
    if (!set) {
        return;
    }

    for (size_t i = 0; i < set->count; i++) {
        free(set->items[i]);
    }
    free(set->items);
    set->items = NULL;
    set->count = 0;
    set->cap = 0;
}

static bool string_set_contains(const StringSet *set, const char *s,
                                size_t len) {
    for (size_t i = 0; i < set->count; i++) {
        if (strlen(set->items[i]) == len &&
            strncmp(set->items[i], s, len) == 0) {
            return true;
        }
    }
    return false;
}

static bool string_set_add_range(StringSet *set, const char *s, size_t len) {
    if (!set || !s || len == 0) {
        return false;
    }

    if (string_set_contains(set, s, len)) {
        return true;
    }

    if (set->count == set->cap) {
        size_t new_cap = set->cap ? set->cap * 2 : 16;
        char **new_items =
            realloc(set->items, new_cap * sizeof(*new_items));
        if (!new_items) {
            return false;
        }
        set->items = new_items;
        set->cap = new_cap;
    }

    set->items[set->count] = xstrdup_range(s, len);
    if (!set->items[set->count]) {
        return false;
    }

    set->count++;
    return true;
}

static bool extract_resource_key(const char *line, size_t len,
                                 const char **key_start, size_t *key_len) {
    if (!line || !key_start || !key_len) {
        return false;
    }

    while (len > 0 && (*line == ' ' || *line == '\t')) {
        line++;
        len--;
    }

    if (len == 0 || *line == '!' || *line == '#') {
        return false;
    }

    const char *colon = memchr(line, ':', len);
    if (!colon) {
        return false;
    }

    const char *end = colon;
    while (end > line && (end[-1] == ' ' || end[-1] == '\t')) {
        end--;
    }

    if (end == line) {
        return false;
    }

    *key_start = line;
    *key_len = (size_t)(end - line);
    return true;
}

static bool collect_resource_keys(const char *text, StringSet *keys) {
    if (!text || !keys) {
        return false;
    }

    const char *p = text;

    while (*p) {
        const char *line_end = strchr(p, '\n');
        if (!line_end) {
            line_end = p + strlen(p);
        }

        size_t line_len = (size_t)(line_end - p);
        const char *key_start = NULL;
        size_t key_len = 0;

        if (extract_resource_key(p, line_len, &key_start, &key_len)) {
            if (!string_set_add_range(keys, key_start, key_len)) {
                return false;
            }
        }

        p = *line_end ? line_end + 1 : line_end;
    }

    return true;
}

static bool key_is_in_set(const StringSet *set, const char *line,
                          size_t line_len) {
    const char *key_start = NULL;
    size_t key_len = 0;

    if (!extract_resource_key(line, line_len, &key_start, &key_len)) {
        return false;
    }

    return string_set_contains(set, key_start, key_len);
}

static char *filter_resource_text(const char *base, const StringSet *drop_keys) {
    if (!base) {
        base = "";
    }

    size_t cap = strlen(base) + 1;
    char *out = malloc(cap);
    if (!out) {
        return NULL;
    }

    size_t out_len = 0;
    const char *p = base;

    while (*p) {
        const char *line_end = strchr(p, '\n');
        size_t line_len;

        if (line_end) {
            line_len = (size_t)(line_end + 1 - p);
        } else {
            line_len = strlen(p);
        }

        if (!key_is_in_set(drop_keys, p, line_len)) {
            if (out_len + line_len + 1 > cap) {
                size_t new_cap = (cap * 2 > out_len + line_len + 1)
                                     ? cap * 2
                                     : out_len + line_len + 1;
                char *new_out = realloc(out, new_cap);
                if (!new_out) {
                    free(out);
                    return NULL;
                }
                out = new_out;
                cap = new_cap;
            }

            memcpy(out + out_len, p, line_len);
            out_len += line_len;
        }

        p = line_end ? line_end + 1 : p + line_len;
    }

    out[out_len] = '\0';
    return out;
}

static char *concat_resource_text(const char *a, const char *b) {
    if (!a) {
        a = "";
    }
    if (!b) {
        b = "";
    }

    size_t a_len = strlen(a);
    size_t b_len = strlen(b);

    bool need_sep = false;
    if (a_len > 0 && b_len > 0 && a[a_len - 1] != '\n') {
        need_sep = true;
    }

    size_t total = a_len + (need_sep ? 1 : 0) + b_len + 1;
    char *out = malloc(total);
    if (!out) {
        return NULL;
    }

    size_t pos = 0;

    memcpy(out + pos, a, a_len);
    pos += a_len;

    if (need_sep) {
        out[pos++] = '\n';
    }

    memcpy(out + pos, b, b_len);
    pos += b_len;

    out[pos] = '\0';
    return out;
}

void publish_screen_resource_managers(StellarState *st) {
    if (!st || !st->dpy || st->config.screen_count <= 0) {
        return;
    }

    char *global_rm = get_root_text_property(st->dpy, 0, "RESOURCE_MANAGER");
    if (!global_rm) {
        global_rm = strdup("");
    }

    char **screen_res = calloc((size_t)st->config.screen_count, sizeof(*screen_res));
    if (!screen_res) {
        free(global_rm);
        return;
    }

    StringSet screen_keys = {0};

    for (int i = 0; i < st->config.screen_count; i++) {
        screen_res[i] = get_root_text_property(st->dpy, i, "SCREEN_RESOURCES");
        if (!screen_res[i]) {
            screen_res[i] = strdup("");
        }

        if (!collect_resource_keys(screen_res[i], &screen_keys)) {
            for (int j = 0; j <= i; j++) {
                free(screen_res[j]);
            }
            free(screen_res);
            free(global_rm);
            string_set_free(&screen_keys);
            return;
        }
    }

    char *filtered_global = filter_resource_text(global_rm, &screen_keys);
    if (!filtered_global) {
        for (int i = 0; i < st->config.screen_count; i++) {
            free(screen_res[i]);
        }
        free(screen_res);
        free(global_rm);
        string_set_free(&screen_keys);
        return;
    }

    for (int i = 0; i < st->config.screen_count; i++) {
        char *merged =
            concat_resource_text(filtered_global, screen_res[i]);
        if (!merged) {
            continue;
        }

        set_root_text_property(st->dpy, i, "RESOURCE_MANAGER", merged);
        free(merged);
    }

    XFlush(st->dpy);

    free(filtered_global);
    for (int i = 0; i < st->config.screen_count; i++) {
        free(screen_res[i]);
    }
    free(screen_res);
    free(global_rm);
    string_set_free(&screen_keys);
}
