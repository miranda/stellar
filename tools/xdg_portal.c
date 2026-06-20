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
#include <sys/wait.h>
#include <limits.h>

#include <systemd/sd-bus.h>
#include <X11/Xlib.h>

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
 * X11 helpers
 * -------------------------------------------------------------------- */

/*
 * Given an X11 window ID (from the portal's parent_window string),
 * figure out which screen it's on and return the DISPLAY string
 * for that screen (e.g. ":0.1").
 *
 * Returns a malloc'd string, or NULL on failure.
 */
static char *display_for_x11_window(unsigned long window_id)
{
    Display *dpy;
    char *current_display;
    char *result = NULL;

    current_display = getenv("DISPLAY");
    if (!current_display)
        current_display = ":0";

    dpy = XOpenDisplay(current_display);
    if (!dpy)
        return NULL;

    int num_screens = ScreenCount(dpy);

    for (int i = 0; i < num_screens; i++) {
        Screen *scr = ScreenOfDisplay(dpy, i);
        Window root = RootWindowOfScreen(scr);

        /* Check if the window is in this screen's tree */
        Window root_ret, parent_ret;
        Window *children = NULL;
        unsigned int nchildren = 0;

        /*
         * XQueryTree will succeed if the window belongs to this root.
         * We verify root_ret matches this screen's root.
         */
        if (XQueryTree(dpy, (Window)window_id, &root_ret, &parent_ret,
                        &children, &nchildren)) {
            if (children)
                XFree(children);
            if (root_ret == root) {
				// TODO: This isn't reliable parsing this way.
                char base[256];
                snprintf(base, sizeof(base), "%s", current_display);

                /* Remove .N suffix if present */
                char *dot = strrchr(base, '.');
                char *colon = strrchr(base, ':');
                if (dot && colon && dot > colon)
                    *dot = '\0';

                size_t len = strlen(base) + 16;
                result = malloc(len);
                if (result)
                    snprintf(result, len, "%s.%d", base, i);
                break;
            }
        }
    }

    XCloseDisplay(dpy);
    return result;
}

/*
 * Parse the portal parent_window string.
 * Format for X11: "x11:0x1a2b3c"
 * Returns the display string for that window's screen, or NULL.
 */
static char *resolve_parent_display(const char *parent_window)
{
    if (!parent_window || !*parent_window)
        return NULL;

    if (strncmp(parent_window, "x11:", 4) == 0) {
        unsigned long wid = strtoul(parent_window + 4, NULL, 16);
        if (wid == 0)
            return NULL;
        return display_for_x11_window(wid);
    }

    /* Wayland or unknown - ignore for now */
    return NULL;
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
 * `display` may be NULL (use inherited DISPLAY).
 */
static int run_filechooser(const char *mode, const char *title,
                           int multiple, int directory,
                           const char *current_name,
                           const char *display,
                           struct chooser_result *out)
{
    int pipefd[2];
    pid_t pid;

    memset(out, 0, sizeof(*out));

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

        if (display)
            setenv("DISPLAY", display, 1);

        const char *argv[16];
        int argc = 0;

        argv[argc++] = FILECHOOSER_BIN;
        argv[argc++] = "--mode";
        argv[argc++] = mode;  /* "open", "save", or "open-directory" */

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

    char *display = resolve_parent_display(parent_window);

    const char *mode = directory ? "open-directory" : "open";

    struct chooser_result result;
    r = run_filechooser(mode, title, multiple, directory, NULL, display, &result);
    free(display);

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

    char *display = resolve_parent_display(parent_window);

    struct chooser_result result;
    r = run_filechooser("save", title, 0, 0, current_name, display, &result);
    free(display);

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

    char *display = resolve_parent_display(parent_window);

    struct chooser_result result;
    r = run_filechooser("open-directory",
                        title ? title : "Select Destination",
                        0, 1, NULL, display, &result);
    free(display);

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
