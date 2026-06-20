#define _GNU_SOURCE

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <poll.h>
#include <time.h>

#define NK_INCLUDE_FIXED_TYPES
#define NK_INCLUDE_STANDARD_IO
#define NK_INCLUDE_STANDARD_VARARGS
#define NK_INCLUDE_DEFAULT_ALLOCATOR
#define NK_IMPLEMENTATION
#include "nuklear.h"

#define NK_XCB_CAIRO_IMPLEMENTATION
#include "nuklear_xcb.h"
#include <xcb/xcb.h>

// Shared Stellar theming: colors come from the active theme (relayed from
// AwesomeWM), the font comes from the appearance settings.  Link with
// stellar_theme.c, stellar_font.c, stellar_config.c and cJSON.c.
#include "stellar_theme.h"
#include "stellar_nk_theme.h"

/* Lightweight file logging to diagnose theme/screen resolution when the
 * dialog is spawned non-interactively (forked at settings-app exit), where
 * stdout/stderr normally go nowhere visible.  Override path with
 * STELLAR_DIALOG_LOG. */
#include <stdarg.h>
#include <fcntl.h>

static const char *dialog_log_path(void) {
    const char *path = getenv("STELLAR_DIALOG_LOG");
    if (!path || !path[0]) path = "/tmp/stellar-dialog.log";
    return path;
}

static void dlog(const char *fmt, ...) {
    FILE *f = fopen(dialog_log_path(), "a");
    if (!f) return;
    va_list ap; va_start(ap, fmt); vfprintf(f, fmt, ap); va_end(ap);
    fputc('\n', f); fclose(f);
}

/* Redirect stdout+stderr into the same log file so the existing printf()
 * diagnostics inside request_theme_data()/request_appearance() (which name
 * exactly which failure exit fired) are captured for the forked dialog. */
static void dialog_capture_stdio(void) {
    int fd = open(dialog_log_path(), O_WRONLY | O_CREAT | O_APPEND, 0644);
    if (fd < 0) return;
    dup2(fd, STDOUT_FILENO);
    dup2(fd, STDERR_FILENO);
    if (fd > STDERR_FILENO) close(fd);
    setvbuf(stdout, NULL, _IOLBF, 0);
    setvbuf(stderr, NULL, _IOLBF, 0);
}

// --- IPC ---
static void send_stellar_command(const char *cmd) {
    if (!cmd || strlen(cmd) == 0) return;

    const char *socket_path = getenv("STELLAR_SOCKET");
    if (!socket_path) {
        printf("Error: STELLAR_SOCKET not set. Cannot send IPC.\n");
        return;
    }
    int sock = socket(AF_UNIX, SOCK_STREAM, 0);
    if (sock < 0) return;

    struct sockaddr_un addr = {0};
    addr.sun_family = AF_UNIX;
    strncpy(addr.sun_path, socket_path, sizeof(addr.sun_path) - 1);

    if (connect(sock, (struct sockaddr *)&addr, sizeof(addr)) == 0) {
        dprintf(sock, "%s\n", cmd);
        printf("Sent IPC to Stellar: %s\n", cmd);
    } else {
        printf("Failed to connect to Stellar IPC socket.\n");
    }
    close(sock);
}

static float calculate_dialog_height(struct nk_context *ctx, const char *text, int window_width) {
    const struct nk_user_font *f = ctx->style.font;
    float line_h = f->height + 4.0f;
    float avail = (float)window_width - 2.0f * ctx->style.window.padding.x - 8.0f;
    if (avail < 32.0f) avail = 32.0f;

    float total_text_h = 0;
    int num_text_rows = 0;
    const char *p = text;

    if (p) {
        while (*p) {
            const char *nl = strchr(p, '\n');
            int len = nl ? (int)(nl - p) : (int)strlen(p);

            if (len == 0) {
                total_text_h += line_h * 0.5f;
            } else {
                float tw = f->width(f->userdata, f->height, p, len);
                int rows = (int)(tw / avail) + 1;
                total_text_h += line_h * (float)rows;
            }
            num_text_rows++; // Track every generated layout row

            if (!nl) break;
            p = nl + 1;
        }
    }

	// Tally up all layout rows: 
    // top spacer(1) + text lines + middle spacer(1) + buttons(1) + bottom spacer(1)
    int total_rows = 1 + num_text_rows + 1 + 1 + 1;

    // Sum up the explicit element heights (+20.0f at the end for the bottom margin)
    float content_h = 20.0f + total_text_h + 20.0f + 30.0f + 20.0f;
    
    // Add Nuklear's internal layout paddings
    content_h += ctx->style.window.padding.y * 2.0f;              // Top & bottom window padding
    content_h += ctx->style.window.spacing.y * (float)total_rows; // Spacing injected between every row
    content_h += ctx->style.window.border * 2.0f;                 // Top & bottom window borders

    // Return with a tiny 2px safety buffer to absorb floating point truncation
    return content_h + 2.0f;
}

// --- Main ---
int main(int argc, char *argv[]) {
    // Default arguments
    const char *window_title = "Stellar System Prompt";
    const char *dialog_message = "An action is required.";
    const char *action_method = "ipc";
    const char *action_cmd = "";
    const char *button_text = "Ok";
    const char *cancel_text = "Cancel";
	int choices = 2;

    // Parse command-line arguments
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--message") == 0 && i + 1 < argc) {
            dialog_message = argv[++i];
        } else if (strcmp(argv[i], "--action-method") == 0 && i + 1 < argc) {
            action_method = argv[++i];
        } else if (strcmp(argv[i], "--action-cmd") == 0 && i + 1 < argc) {
            action_cmd = argv[++i];
        } else if (strcmp(argv[i], "--button") == 0 && i + 1 < argc) {
            button_text = argv[++i];
        } else if (strcmp(argv[i], "--cancel-button") == 0 && i + 1 < argc) {
            cancel_text = argv[++i];
        } else if (strcmp(argv[i], "--single-choice") == 0 && i + 1 < argc) {
            choices = 1;
        }
    }

    int window_width = 450;
    int window_height = 200;

    /* Resolve which screen's theme to request from the DISPLAY this dialog
     * will actually render on -- NOT from STELLAR_SCREEN.  When the dialog is
     * forked at the settings app's exit, STELLAR_SCREEN may be unset or may
     * not match the screen AwesomeWM maps the window onto, which would pull
     * the wrong screen's appearance (e.g. a 96-dpi font onto a 120-dpi
     * screen -> wrong font).  The DISPLAY we inherit is the one the window
     * opens on, so asking the DE which screen owns it is authoritative.
     * Fall back to STELLAR_SCREEN, then 0. */
    const char *screen_env = getenv("STELLAR_SCREEN");
    const char *disp = getenv("DISPLAY");
    int stellar_screen = 0;
    int resolved = stellar_screen_for_display(disp);
    if (resolved >= 0) {
        stellar_screen = resolved;
    } else if (screen_env) {
        stellar_screen = atoi(screen_env);
    }

    dlog("==== stellar-dialog starting pid=%d ====", (int)getpid());
    dlog("  DISPLAY=%s  STELLAR_SCREEN=%s  DE-resolved=%d -> using screen=%d",
         disp ? disp : "(unset)", screen_env ? screen_env : "(unset)",
         resolved, stellar_screen);
    dlog("  STELLAR_SOCKET=%s",
         getenv("STELLAR_SOCKET") ? getenv("STELLAR_SOCKET") : "(unset)");

    /* Capture stdout/stderr into the log so request_theme_data()'s internal
     * printf diagnostics (which name exactly which of its three failure exits
     * fired -- IPC request, JSON parse, or DE error field) are visible for
     * this non-interactive forked process. */
    dialog_capture_stdio();

    ThemeData theme_data;
    int have_theme = (request_theme_data(stellar_screen, &theme_data) == 0);
    if (!have_theme) {
        dlog("  request_theme_data FAILED -> hardcoded defaults (wrong font). "
             "See the printf line above for which exit fired.");
        fprintf(stderr, "dialog: no theme data from Stellar, using defaults\n");
        memset(&theme_data, 0, sizeof(theme_data));

        // Inject a sensible fallback font so Nuklear doesn't use the tiny default
        strncpy(theme_data.font, "sans-serif", sizeof(theme_data.font) - 1);
        theme_data.font_size = 12.0f; // Sane default point size
        strncpy(theme_data.font_unit, "pt", sizeof(theme_data.font_unit) - 1);
        theme_data.dpi = 96;
    } else {
        dlog("  request_theme_data OK: dpi=%d font='%s' size=%.1f path='%s'",
             theme_data.dpi, theme_data.font, (double)theme_data.font_size,
             theme_data.font_path);
        dlog("  nk_color_window='%s' nk_color_text='%s'",
             theme_data.nk_color_window, theme_data.nk_color_text);
    }

    struct nk_xcb_context *xcb_ctx;
    struct nk_cairo_context *cairo_ctx;
    struct nk_context *ctx;

    /* X11 / Cairo.  The window type must be set BEFORE the window is
     * mapped: AwesomeWM reads _NET_WM_WINDOW_TYPE when it starts managing
     * the window and applies its floating/placement rules then. */
    xcb_ctx = nk_xcb_init_unmapped(
        window_title,
        0,
        0,
        window_width,
        window_height);
    nk_xcb_set_window_type(xcb_ctx, "_NET_WM_WINDOW_TYPE_DIALOG");

    struct nk_color background =
        stellar_theme_color(theme_data.nk_color_window, nk_rgb(30, 30, 30));

    cairo_ctx = nk_cairo_init(
        &background,
        NULL,
        0,
        nk_xcb_create_cairo_surface(xcb_ctx));

    /* Nuklear */
    ctx = malloc(sizeof(struct nk_context));
    if (!ctx) {
        fprintf(stderr, "Out of memory\n");
        return 1;
    }
    nk_init_default(ctx, nk_cairo_default_font(cairo_ctx));

    // Same look as the settings app: theme colors + the configured system
    // font (bitmap fonts arrive here as transparently converted .otb files).
    dlog("  -> apply_nk_theme with font='%s' path='%s' size=%.1f is_bitmap=%d",
         theme_data.font, theme_data.font_path, (double)theme_data.font_size,
         theme_data.font_is_bitmap);
    int theme_rc = apply_nk_theme(ctx, cairo_ctx, &theme_data);
    dlog("  apply_nk_theme returned %d (0=ok, -1=font failed to load -> "
         "nuklear default face = completely wrong font)", theme_rc);

    // --- Dynamic Height Calculation ---
    // Now that the font is applied, calculate the required height
    int calc_height = (int)calculate_dialog_height(ctx, dialog_message, window_width);
    if (calc_height < 150) calc_height = 150; // Sane minimum
    if (calc_height > 800) calc_height = 800; // Sane maximum
    window_height = calc_height;

    // Resize the unmapped X11 window
    uint32_t values[] = { (uint32_t)window_width, (uint32_t)window_height };
    xcb_configure_window(xcb_ctx->conn, xcb_ctx->window, XCB_CONFIG_WINDOW_WIDTH | XCB_CONFIG_WINDOW_HEIGHT, values);
    
    // Update Nuklear/Cairo structures with new boundaries
    xcb_ctx->width = window_width;
    xcb_ctx->height = window_height;
    nk_xcb_resize_cairo_surface(xcb_ctx, nk_cairo_surface(cairo_ctx));

    // Finally map the correctly-sized window
    nk_xcb_map(xcb_ctx);
    xcb_flush(xcb_ctx->conn);
    // ----------------------------------

	// Main loop
    int running = 1;
	int exit_status = 1; // Default to 1 (Cancel/Closed) for safety

    while (running) {
        /* nk_xcb_handle_event() is non-blocking (it polls), so we do the
         * sleeping ourselves on the X socket.  Key/button/motion events do
         * NOT set any NK_XCB_EVENT_* bit, so the handler returns 0 for them;
         * in immediate mode the captured input is only consumed when the
         * widget frame runs AFTER nk_input_end and BEFORE the next
         * nk_input_begin.  So we render a frame on every iteration that
         * handled events -- otherwise clicks/hover-motion get wiped by the
         * next nk_input_begin and the buttons never react. */
        struct pollfd pfd;
        pfd.fd = xcb_get_file_descriptor(xcb_ctx->conn);
        pfd.events = POLLIN;
        pfd.revents = 0;

        int pr = poll(&pfd, 1, -1);   /* block until X socket activity */
        if (pr < 0) {
            perror("poll");
            break;
        }
        if (!(pfd.revents & POLLIN))
            continue;

        int events = nk_xcb_handle_event(xcb_ctx, ctx);

        if (events & NK_XCB_EVENT_STOP) {
			exit_status = 1; // Window closed via WM
            running = 0;
            break;
        }

        /* Expose/map: window contents are gone (no backing store) but the
         * nuklear command buffer is unchanged, so force the repaint. */
        if (events & NK_XCB_EVENT_PAINT) {
            nk_cairo_damage(cairo_ctx);
        }

        if (events & NK_XCB_EVENT_RESIZED) {
            nk_xcb_resize_cairo_surface(xcb_ctx, nk_cairo_surface(cairo_ctx));
            nk_cairo_damage(cairo_ctx);
        }

        /* Always render after handling so input captured this cycle is
         * consumed before the next nk_input_begin. */
        {
            if (nk_begin(ctx, "Dialog", nk_rect(0, 0, xcb_ctx->width, xcb_ctx->height), NK_WINDOW_NO_SCROLLBAR)) {
                
                // Top spacing
                nk_layout_space_begin(ctx, NK_STATIC, 20, 1);
                nk_layout_space_end(ctx);

                // Message text: wrapped, '\n' starts a new line
                nk_label_wrap_multiline(ctx, dialog_message);

                // Bottom spacing to push buttons down
                nk_layout_space_begin(ctx, NK_STATIC, 20, 1);
                nk_layout_space_end(ctx);

                // Buttons
                nk_layout_row_dynamic(ctx, 30, choices);
				if (choices > 1 && nk_button_label(ctx, cancel_text)) {
                    exit_status = 1;
                    running = 0; // Exit without sending IPC
                }
                if (nk_button_label(ctx, button_text)) {
					if (strcmp(action_method, "shell") == 0) {
						system(action_cmd);
					} else {
						send_stellar_command(action_cmd);
					}
					exit_status = 0;
					running = 0;
				}
			    
				// Bottom padding so buttons aren't flush with the window edge
                nk_layout_space_begin(ctx, NK_STATIC, 20, 1);
                nk_layout_space_end(ctx);
			}
			nk_end(ctx);

			nk_cairo_render(cairo_ctx, ctx);
			nk_xcb_render(xcb_ctx);
			nk_clear(ctx);
        }
    }

    nk_free(ctx);
    free(ctx);
	stellar_nk_theme_cleanup(cairo_ctx);
    nk_cairo_free(cairo_ctx);
    nk_xcb_free(xcb_ctx);

    return exit_status;
}
