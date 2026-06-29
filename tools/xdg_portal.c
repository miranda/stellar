/*
 * xdg-desktop-portal-stellar
 *
 * Portal backend for the Stellar desktop environment.
 * Currently implements: org.freedesktop.impl.portal.FileChooser
 *
 * Uses sd-bus for D-Bus and Xlib to resolve parent windows to screens.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <signal.h>
#include <stdarg.h>
#include <time.h>
#include <sys/wait.h>
#include <limits.h>

#include <systemd/sd-bus.h>
#include <X11/Xlib.h>

#include "stellar_theme.h"   /* stellar_screen_for_window(), make_screen_display_name() */

/* --------------------------------------------------------------------
 * Configuration
 * -------------------------------------------------------------------- */

#ifndef STELLAR_LIBEXEC_PATH
#define STELLAR_LIBEXEC_PATH "/usr/local/libexec/stellar"
#endif

#ifndef FILECHOOSER_BIN
#define FILECHOOSER_BIN STELLAR_LIBEXEC_PATH "/stellar-fileselect"
#endif

/* --------------------------------------------------------------------
 * Debug file logging
 * --------------------------------------------------------------------
 * The portal is D-Bus-activated, so its stderr usually goes nowhere visible.
 * Route diagnostics to a file instead (override with STELLAR_PORTAL_LOG).
 * Logging is OFF unless a log path is configured OR STELLAR_PORTAL_DEBUG=1,
 * so production runs stay silent.  Mirrors the polkit agent's approach. */

static void portal_log(const char *fmt, ...)
{
    const char *path = getenv("STELLAR_PORTAL_LOG");
    if (!path || !path[0]) {
        if (getenv("STELLAR_PORTAL_DEBUG"))
            path = "/tmp/stellar-portal.log";
        else
            return;   /* logging disabled */
    }

    FILE *f = fopen(path, "a");
    if (!f) return;

    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    struct tm tm;
    localtime_r(&ts.tv_sec, &tm);
    char when[32];
    strftime(when, sizeof(when), "%H:%M:%S", &tm);
    fprintf(f, "[%s.%03ld pid=%d] ", when, ts.tv_nsec / 1000000L, (int)getpid());

    va_list ap;
    va_start(ap, fmt);
    vfprintf(f, fmt, ap);
    va_end(ap);

    fputc('\n', f);
    fclose(f);
}

/* Dump the environment variables that matter for screen/theme resolution, so
 * we can see whether the D-Bus-activated portal inherited them at all. */
static void portal_log_env(const char *context)
{
    static const char *keys[] = {
        "STELLAR_SOCKET", "STELLAR_SCREEN", "DISPLAY",
        "XDG_RUNTIME_DIR", "XDG_CURRENT_DESKTOP",
    };
    portal_log("---- environment (%s) ----", context);
    for (size_t i = 0; i < sizeof(keys) / sizeof(keys[0]); i++) {
        const char *v = getenv(keys[i]);
        portal_log("    %s=%s", keys[i], v ? v : "(unset)");
    }
}

/* --------------------------------------------------------------------
 * Screen resolution
 * -------------------------------------------------------------------- */

/*
 * Parse the portal parent_window string ("x11:0x1a2b3c") and ask the DE which
 * Stellar screen owns that window.  Returns the screen index (>= 0), or -1 if
 * the parent is unknown / not an X11 window / the DE is unreachable.
 *
 * We deliberately do NOT reconstruct a per-screen DISPLAY string here from the
 * X server: the old code did, and it broke on non-:0 servers (Xephyr :3.x) and
 * assumed the X screen index equals the Stellar screen index.  The DE is the
 * single source of truth for the window->screen mapping (it built the table in
 * init_x()), and its answer is correct even for windows AwesomeWM reparented.
 */
static int resolve_parent_screen(const char *parent_window)
{
    if (!parent_window || !*parent_window) {
        portal_log("resolve_parent_screen: empty parent_window -> -1");
        return -1;
    }

    if (strncmp(parent_window, "x11:", 4) == 0) {
        unsigned long wid = strtoul(parent_window + 4, NULL, 16);
        portal_log("resolve_parent_screen: parent='%s' wid=0x%lx", parent_window, wid);
        portal_log_env("before stellar_screen_for_window");
        if (wid == 0) {
            portal_log("resolve_parent_screen: wid==0 -> -1");
            return -1;
        }
        int s = stellar_screen_for_window(wid);
        portal_log("resolve_parent_screen: stellar_screen_for_window(0x%lx) -> %d", wid, s);
        return s;
    }

    /* Wayland or unknown - ignore for now */
    portal_log("resolve_parent_screen: non-x11 parent '%s' -> -1", parent_window);
    return -1;
}

/* --------------------------------------------------------------------
 * File chooser subprocess
 * -------------------------------------------------------------------- */

struct chooser_result {
    int  cancelled;      /* 1 if user cancelled */
    char **uris;         /* NULL-terminated array of file:// URIs */
    int  num_uris;
};

static void free_chooser_result(struct chooser_result *r)
{
    if (!r) return;
    for (int i = 0; i < r->num_uris; i++)
        free(r->uris[i]);
    free(r->uris);
}

/*
 * Build the argv for the file chooser and run it.
 *
 * The chooser is expected to:
 *   - Print selected paths to stdout, one per line
 *   - Exit 0 on success, 1 on cancel
 *
 * `screen` is the Stellar screen index the chooser should appear and theme
 * itself on, as resolved from the request's parent window via the DE.  Pass -1
 * to leave the inherited DISPLAY untouched (chooser falls back to screen 0 /
 * its own defaults); used when there is no usable X11 parent.
 */
static int run_filechooser(const char *mode, const char *app_id, const char *title,
                           int multiple, int directory,
                           const char *current_name,
                           int screen,
                           struct chooser_result *out)
{
    int pipefd[2];
    pid_t pid;

    memset(out, 0, sizeof(*out));

    /* Log what we're about to hand the child, computed the same way the child
     * will, so the log shows the actual DISPLAY/STELLAR_SCREEN it gets. */
    if (screen >= 0) {
        const char *base = getenv("DISPLAY");
        if (!base || !base[0]) base = ":0";
        char preview[64];
        make_screen_display_name(base, screen, preview, sizeof(preview));
        portal_log("run_filechooser: mode=%s screen=%d base_DISPLAY=%s "
                   "child_DISPLAY=%s child_STELLAR_SCREEN=%d",
                   mode, screen, base, preview, screen);
    } else {
        portal_log("run_filechooser: mode=%s screen=%d (UNRESOLVED) -- child "
                   "inherits portal's DISPLAY=%s, no STELLAR_SCREEN set",
                   mode, screen, getenv("DISPLAY") ? getenv("DISPLAY") : "(unset)");
    }

    if (pipe(pipefd) < 0)
        return -1;

    pid = fork();
    if (pid < 0) {
        close(pipefd[0]);
        close(pipefd[1]);
        return -1;
    }

    if (pid == 0) {
        /* Child */
        close(pipefd[0]);
        dup2(pipefd[1], STDOUT_FILENO);
        close(pipefd[1]);

        if (screen >= 0) {
            /* Place the chooser on the resolved screen: build that screen's
             * canonical DISPLAY using the SAME helper the DE used in init_x(),
             * so the string matches exactly (correct on non-:0 / Xephyr).  Also
             * export STELLAR_SCREEN so the chooser fetches the right screen's
             * theme + font -- without this it defaults to screen 0 and shows
             * the wrong font, since the portal is a single global process that
             * never inherits a per-screen STELLAR_SCREEN. */
            const char *base = getenv("DISPLAY");
            if (!base || !base[0]) base = ":0";

            char dispbuf[64];
            make_screen_display_name(base, screen, dispbuf, sizeof(dispbuf));
            setenv("DISPLAY", dispbuf, 1);

            char scrbuf[16];
            snprintf(scrbuf, sizeof(scrbuf), "%d", screen);
            setenv("STELLAR_SCREEN", scrbuf, 1);
        }

        const char *argv[20];
        int argc = 0;

        argv[argc++] = FILECHOOSER_BIN;
        argv[argc++] = "--mode";
        argv[argc++] = mode;  /* "open", "save", or "open-directory" */

        if (app_id && app_id[0] != '\0') {
            argv[argc++] = "--app-id";
            argv[argc++] = app_id;
        }
        if (title) {
            argv[argc++] = "--title";
            argv[argc++] = title;
        }
        if (current_name) {
            argv[argc++] = "--current-name";
            argv[argc++] = current_name;
        }
        if (multiple)
            argv[argc++] = "--multiple";
        if (directory)
            argv[argc++] = "--directory";

        argv[argc] = NULL;

        execvp(argv[0], (char * const *)argv);
        portal_log("execvp(%s) FAILED: %s", argv[0], strerror(errno));
        _exit(127);
    }

    /* Parent - read stdout from child */
    close(pipefd[1]);

    char buf[PATH_MAX * 64];
    ssize_t total = 0;
    ssize_t n;

    while ((n = read(pipefd[0], buf + total,
                     sizeof(buf) - total - 1)) > 0) {
        total += n;
    }
    close(pipefd[0]);
    buf[total] = '\0';

    int status;
    waitpid(pid, &status, 0);

    if (!WIFEXITED(status) || WEXITSTATUS(status) != 0) {
        out->cancelled = 1;
        return 0;
    }

    /* Parse lines into URIs */
    int capacity = 8;
    out->uris = calloc(capacity, sizeof(char *));
    out->num_uris = 0;

    char *line = strtok(buf, "\n");
    while (line) {
        /* Filter: Skip empty lines or random library warnings.
           A valid file path from the filechooser MUST be absolute. */
        if (line[0] != '/') {
            line = strtok(NULL, "\n");
            continue;
        }

        if (out->num_uris >= capacity - 1) {
            capacity *= 2;
            out->uris = realloc(out->uris, capacity * sizeof(char *));
        }

        /* Encode: XDP strictly requires valid URIs. 
           We must safely URL-encode spaces and special characters.
           Worst case size is 3x the string length plus the prefix. */
        size_t max_encoded_len = strlen("file://") + (strlen(line) * 3) + 1;
        char *uri = malloc(max_encoded_len);
        
        char *p = uri;
        p += sprintf(p, "file://");
        
        for (const char *c = line; *c != '\0'; c++) {
            // Encode spaces, percentages, and standard URI reserved chars
            if (*c <= 0x20 || *c == '%' || *c == '#' || *c == '?' || 
                *c == '"'  || *c == '<' || *c == '>') {
                p += sprintf(p, "%%%02X", (unsigned char)*c);
            } else {
                *p++ = *c;
            }
        }
        *p = '\0';

        out->uris[out->num_uris++] = uri;
        line = strtok(NULL, "\n");
    }

    out->uris[out->num_uris] = NULL;

    return 0;
}

/* --------------------------------------------------------------------
 * Portal D-Bus method handlers
 * -------------------------------------------------------------------- */

/*
 * Extract common options from the portal request's options dict.
 */
static void parse_options(sd_bus_message *msg, int *multiple, int *directory, char **current_name)
{
    *multiple = 0;
    *directory = 0;
    if (current_name) *current_name = NULL;

    /* Enter the a{sv} options dict */
    if (sd_bus_message_enter_container(msg, 'a', "{sv}") < 0)
        return;

    while (sd_bus_message_enter_container(msg, 'e', "sv") >= 0) {
        const char *key;
        if (sd_bus_message_read(msg, "s", &key) >= 0) {
            if (strcmp(key, "multiple") == 0) {
                int val;
                if (sd_bus_message_enter_container(msg, 'v', "b") >= 0) {
                    sd_bus_message_read(msg, "b", &val);
                    *multiple = val;
                    sd_bus_message_exit_container(msg);
                }
            } else if (strcmp(key, "directory") == 0) {
                int val;
                if (sd_bus_message_enter_container(msg, 'v', "b") >= 0) {
                    sd_bus_message_read(msg, "b", &val);
                    *directory = val;
                    sd_bus_message_exit_container(msg);
                }
            } else if (current_name && strcmp(key, "current_name") == 0) {
                const char *val;
                if (sd_bus_message_enter_container(msg, 'v', "s") >= 0) {
                    sd_bus_message_read(msg, "s", &val);
                    *current_name = strdup(val);
                    sd_bus_message_exit_container(msg);
                }
            } else {
                sd_bus_message_skip(msg, "v");
            }
        }
        sd_bus_message_exit_container(msg);
    }
    sd_bus_message_exit_container(msg);
}

/*
 * Build the D-Bus response with a results dict containing "uris".
 */
static int reply_with_uris(sd_bus_message *reply, uint32_t response,
                           struct chooser_result *result)
{
    int r;

    r = sd_bus_message_append(reply, "u", response);
    if (r < 0) return r;

    /* Open results dict: a{sv} */
    r = sd_bus_message_open_container(reply, 'a', "{sv}");
    if (r < 0) return r;

    if (response == 0 && result->num_uris > 0) {
        /* "uris" => as */
        r = sd_bus_message_open_container(reply, 'e', "sv");
        if (r < 0) return r;

        r = sd_bus_message_append(reply, "s", "uris");
        if (r < 0) return r;

        r = sd_bus_message_open_container(reply, 'v', "as");
        if (r < 0) return r;

        r = sd_bus_message_open_container(reply, 'a', "s");
        if (r < 0) return r;

        for (int i = 0; i < result->num_uris; i++) {
            r = sd_bus_message_append(reply, "s", result->uris[i]);
            if (r < 0) return r;
        }

        sd_bus_message_close_container(reply); /* a */
        sd_bus_message_close_container(reply); /* v */
        sd_bus_message_close_container(reply); /* e */
    }

    sd_bus_message_close_container(reply); /* a{sv} */

    return 0;
}

static int handle_open_file(sd_bus_message *msg, void *userdata,
                            sd_bus_error *error)
{
    (void)userdata;
    const char *handle, *app_id, *parent_window, *title;
    int multiple, directory;
    int r;

    r = sd_bus_message_read(msg, "osss", &handle, &app_id,
                            &parent_window, &title);
    if (r < 0)
        return r;

    parse_options(msg, &multiple, &directory, NULL);

    int screen = resolve_parent_screen(parent_window);

    const char *mode = directory ? "open-directory" : "open";

    struct chooser_result result;
    r = run_filechooser(mode, app_id, title, multiple, directory, NULL, screen, &result);

    if (r < 0) {
        return sd_bus_error_set(error,
            "org.freedesktop.portal.Error.Failed",
            "Failed to launch file chooser");
    }

    sd_bus_message *reply = NULL;
    r = sd_bus_message_new_method_return(msg, &reply);
    if (r < 0) goto out;

    uint32_t response = result.cancelled ? 1 : 0;
    r = reply_with_uris(reply, response, &result);
    if (r < 0) goto out;

    r = sd_bus_send(NULL, reply, NULL);

out:
    sd_bus_message_unref(reply);
    free_chooser_result(&result);
    return r;
}

static int handle_save_file(sd_bus_message *msg, void *userdata,
                            sd_bus_error *error)
{
    (void)userdata;
    const char *handle, *app_id, *parent_window, *title;
    int r;

    r = sd_bus_message_read(msg, "osss", &handle, &app_id,
                            &parent_window, &title);
    if (r < 0)
        return r;

    /* SaveFile doesn't use multiple/directory, but we still need
     * to consume the options dict from the message */
    int dummy1, dummy2;
	char *current_name = NULL;
    parse_options(msg, &dummy1, &dummy2, &current_name);

    int screen = resolve_parent_screen(parent_window);

    struct chooser_result result;
    r = run_filechooser("save", app_id, title, 0, 0, current_name, screen, &result);

    if (r < 0) {
        return sd_bus_error_set(error,
            "org.freedesktop.portal.Error.Failed",
            "Failed to launch file chooser");
    }

    sd_bus_message *reply = NULL;
    r = sd_bus_message_new_method_return(msg, &reply);
    if (r < 0) goto out;

    uint32_t response = result.cancelled ? 1 : 0;
    r = reply_with_uris(reply, response, &result);
    if (r < 0) goto out;

    r = sd_bus_send(NULL, reply, NULL);

out:
    sd_bus_message_unref(reply);
    free_chooser_result(&result);
    return r;
}

static int handle_save_files(sd_bus_message *msg, void *userdata,
                             sd_bus_error *error)
{
    (void)userdata;
    /* SaveFiles is for saving multiple files to a chosen directory.
     * Treat it as an open-directory call. */
    const char *handle, *app_id, *parent_window, *title;
    int r;

    r = sd_bus_message_read(msg, "osss", &handle, &app_id,
                            &parent_window, &title);
    if (r < 0)
        return r;

    int dummy1, dummy2;
    parse_options(msg, &dummy1, &dummy2, NULL);

    int screen = resolve_parent_screen(parent_window);

    struct chooser_result result;
    r = run_filechooser("open-directory", app_id,
                        title ? title : "Select Destination",
                        0, 1, NULL, screen, &result);

    if (r < 0) {
        return sd_bus_error_set(error,
            "org.freedesktop.portal.Error.Failed",
            "Failed to launch file chooser");
    }

    sd_bus_message *reply = NULL;
    r = sd_bus_message_new_method_return(msg, &reply);
    if (r < 0) goto out;

    uint32_t response = result.cancelled ? 1 : 0;
    r = reply_with_uris(reply, response, &result);
    if (r < 0) goto out;

    r = sd_bus_send(NULL, reply, NULL);

out:
    sd_bus_message_unref(reply);
    free_chooser_result(&result);
    return r;
}

/* --------------------------------------------------------------------
 * D-Bus interface vtable
 * -------------------------------------------------------------------- */

/*
 * Portal method signatures from the spec:
 *   OpenFile  (o handle, s app_id, s parent_window, s title, a{sv} options)
 *             -> (u response, a{sv} results)
 *   SaveFile  - same
 *   SaveFiles - same
 *
 * sd-bus signature strings:
 *   input:  "osssa{sv}"
 *   output: "ua{sv}"
 */

static const sd_bus_vtable filechooser_vtable[] = {
    SD_BUS_VTABLE_START(0),

    SD_BUS_METHOD("OpenFile",  "osssa{sv}", "ua{sv}",
                  handle_open_file,  SD_BUS_VTABLE_UNPRIVILEGED),
    SD_BUS_METHOD("SaveFile",  "osssa{sv}", "ua{sv}",
                  handle_save_file,  SD_BUS_VTABLE_UNPRIVILEGED),
    SD_BUS_METHOD("SaveFiles", "osssa{sv}", "ua{sv}",
                  handle_save_files, SD_BUS_VTABLE_UNPRIVILEGED),

    SD_BUS_VTABLE_END
};

/* --------------------------------------------------------------------
 * Main
 * -------------------------------------------------------------------- */

static volatile sig_atomic_t running = 1;

static void handle_signal(int sig)
{
    (void)sig;
    running = 0;
}

int main(int argc, char *argv[])
{
    sd_bus *bus = NULL;
    sd_bus_slot *slot = NULL;
    int r;

    (void)argc;
    (void)argv;

    portal_log("==== xdg-desktop-portal-stellar starting ====");
    portal_log_env("at startup (main)");

    signal(SIGINT,  handle_signal);
    signal(SIGTERM, handle_signal);

    r = sd_bus_open_user(&bus);
    if (r < 0) {
        fprintf(stderr, "Failed to connect to session bus: %s\n",
                strerror(-r));
        return 1;
    }

    r = sd_bus_add_object_vtable(bus, &slot,
        "/org/freedesktop/portal/desktop",
        "org.freedesktop.impl.portal.FileChooser",
        filechooser_vtable,
        NULL);
    if (r < 0) {
        fprintf(stderr, "Failed to register vtable: %s\n", strerror(-r));
        goto finish;
    }

    r = sd_bus_request_name(bus,
        "org.freedesktop.impl.portal.desktop.stellar",
        0);
    if (r < 0) {
        fprintf(stderr, "Failed to acquire bus name: %s\n", strerror(-r));
        goto finish;
    }

    fprintf(stderr, "xdg-desktop-portal-stellar running\n");

    while (running) {
        r = sd_bus_process(bus, NULL);
        if (r < 0) {
            fprintf(stderr, "Bus processing error: %s\n", strerror(-r));
            break;
        }
        if (r > 0)
            continue;  /* More work queued, process again */

        r = sd_bus_wait(bus, (uint64_t)-1);
        if (r < 0 && r != -EINTR) {
            fprintf(stderr, "Bus wait error: %s\n", strerror(-r));
            break;
        }
    }

    fprintf(stderr, "xdg-desktop-portal-stellar exiting\n");

finish:
    sd_bus_slot_unref(slot);
    sd_bus_unref(bus);
    return r < 0 ? 1 : 0;
}
