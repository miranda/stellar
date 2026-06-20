#define _GNU_SOURCE

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <pthread.h>
#include <sys/eventfd.h>
#include <poll.h>
#include <fcntl.h>

/* GLib / polkit */
#include <glib.h>
#include <glib-object.h>
#include <polkit/polkit.h>
#include <polkitagent/polkitagent.h>

/* GLib defines bare MIN/MAX/CLAMP/ABS as macros. Nuklear guards its own
 * with #ifndef, so glib's leak in and nuklear silently uses glib's CLAMP,
 * whose argument order (value,low,high) differs from nuklear's (low,value,
 * high) -- corrupting every color-channel clamp so white text clamps to
 * black. Undef them so nuklear's NK_IMPLEMENTATION here matches the codegen
 * in stellar_nk_theme.o (which never sees glib). */
#undef MIN
#undef MAX
#undef CLAMP
#undef ABS

/* Nuklear / XCB */
#define NK_INCLUDE_FIXED_TYPES
#define NK_INCLUDE_STANDARD_IO
#define NK_INCLUDE_STANDARD_VARARGS
#define NK_INCLUDE_DEFAULT_ALLOCATOR
#define NK_IMPLEMENTATION
#include "nuklear.h"

#define NK_XCB_CAIRO_IMPLEMENTATION
#include "nuklear_xcb.h"
#include "stellar_theme.h"
#include "stellar_nk_theme.h"

/* =========================================================
 * Debug file logging
 * =========================================================
 * The agent's stderr usually goes nowhere visible (it's spawned by the DE),
 * so route diagnostics to a file. Override the path with STELLAR_AGENT_LOG.
 * Every line is timestamped and carries the pid+tid so the GLib thread and
 * the XCB thread can be told apart. */

#include <stdarg.h>
#include <time.h>
#include <sys/syscall.h>

static void agent_log(const char *fmt, ...) {
    const char *path = getenv("STELLAR_AGENT_LOG");
    if (!path || !path[0]) path = "/tmp/stellar-polkit-agent.log";

    FILE *f = fopen(path, "a");
    if (!f) return;

    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    struct tm tm;
    localtime_r(&ts.tv_sec, &tm);
    char when[32];
    strftime(when, sizeof(when), "%H:%M:%S", &tm);

    long tid = (long)syscall(SYS_gettid);
    fprintf(f, "[%s.%03ld pid=%d tid=%ld] ",
            when, ts.tv_nsec / 1000000L, (int)getpid(), tid);

    va_list ap;
    va_start(ap, fmt);
    vfprintf(f, fmt, ap);
    va_end(ap);

    fputc('\n', f);
    fclose(f);
}

/* Dump the environment variables that matter for theme/appearance retrieval,
 * so we can see whether the agent inherited them at all. */
static void agent_log_env(const char *context) {
    static const char *keys[] = {
        "STELLAR_SOCKET", "STELLAR_SCREEN", "DISPLAY",
        "XDG_RUNTIME_DIR", "XDG_CURRENT_DESKTOP",
        "FONTCONFIG_PATH", "FONTCONFIG_FILE", "HOME", "XAUTHORITY",
    };
    agent_log("---- environment (%s) ----", context);
    for (size_t i = 0; i < sizeof(keys) / sizeof(keys[0]); i++) {
        const char *v = getenv(keys[i]);
        agent_log("    %s=%s", keys[i], v ? v : "(unset)");
    }
}

/* =========================================================
 * Shared state between GLib thread and XCB render thread
 * ========================================================= */

#define MAX_PASSWORD_LEN 256
#define MAX_MESSAGE_LEN  512
#define MAX_IDENTITY_LEN 256

typedef enum {
    AUTH_STATE_IDLE,
    AUTH_STATE_REQUESTED,   /* GLib thread filled request, XCB thread should show dialog */
    AUTH_STATE_RESPONDED,   /* XCB thread filled response, GLib thread should submit      */
    AUTH_STATE_CANCELLED,   /* polkit cancelled, XCB thread should close dialog           */
} AuthState;

typedef struct {
    pthread_mutex_t  lock;

    /* eventfd: GLib thread writes 1 to wake the dispatcher (xcb_event_loop)
     * to START a dialog. One token == one new authentication to display. */
    int              request_efd;
    /* eventfd: XCB thread writes 1 to wake the blocked GLib on_session_request
     * once the user has submitted or cancelled. */
    int              response_efd;
    /* eventfd: GLib thread writes 1 to tell the CURRENTLY-RUNNING dialog that
     * polkit cancelled it. Kept separate from request_efd so the dispatcher
     * loop and the in-dialog cancel watch never steal each other's tokens. */
    int              cancel_efd;

    /* TRUE between the start of an accepted authentication and its completion.
     * The agent serves exactly one prompt at a time; a second
     * initiate_authentication while busy is rejected so polkit re-drives it
     * later instead of us clobbering the in-flight g_auth slot. */
    gboolean         busy;

    AuthState        state;

    /* filled by GLib thread on BeginAuthentication */
    char             message[MAX_MESSAGE_LEN];
    char             identity[MAX_IDENTITY_LEN]; /* "Authenticate as: <identity>" */
    gboolean         password_ok;                /* filled by GLib after pam verify */

    /* filled by XCB thread after user types password */
    char             password[MAX_PASSWORD_LEN];
    gboolean         cancelled;                  /* TRUE if user clicked Cancel */

    /* polkit cookie - needed to call agent_session functions */
    char            *cookie;
    PolkitAgentSession *session;

	char display[64];
} SharedAuth;

static SharedAuth g_auth;

/* =========================================================
 * XCB / Nuklear auth dialog
 * ========================================================= */

static char *get_display_for_pid(pid_t pid) {
    char path[64];
    snprintf(path, sizeof(path), "/proc/%d/environ", pid);
    
    FILE *f = fopen(path, "r");
    if (!f) return NULL;
    
    char buf[4096];
    size_t n = fread(buf, 1, sizeof(buf) - 1, f);
    fclose(f);
    buf[n] = '\0';
    
    /* environ is null-separated key=value pairs */
    char *p = buf;
    while (p < buf + n) {
        if (strncmp(p, "DISPLAY=", 8) == 0)
            return strdup(p + 8);
        p += strlen(p) + 1;
    }
    return NULL;
}

/* Read the parent pid of `pid` from /proc/<pid>/stat.  Returns 0 on failure.
 * Needed because the polkit subject for org.freedesktop.policykit.exec is
 * often pkexec itself, which is setuid-root: a non-root agent cannot read a
 * setuid process's /proc/<pid>/environ, so get_display_for_pid() fails on it.
 * The parent (the app or shell that invoked pkexec) is owned by us and DOES
 * carry DISPLAY, so we walk up one level. */
static pid_t get_ppid(pid_t pid) {
    char path[64];
    snprintf(path, sizeof(path), "/proc/%d/stat", pid);

    FILE *f = fopen(path, "r");
    if (!f) return 0;

    /* stat format: pid (comm) state ppid ...  comm may contain spaces and
     * parens, so scan to the LAST ')' and parse from there. */
    char buf[512];
    size_t n = fread(buf, 1, sizeof(buf) - 1, f);
    fclose(f);
    if (n == 0) return 0;
    buf[n] = '\0';

    char *rparen = strrchr(buf, ')');
    if (!rparen) return 0;

    /* After ")" comes " <state> <ppid> ..." */
    char state;
    int ppid = 0;
    if (sscanf(rparen + 1, " %c %d", &state, &ppid) != 2) return 0;
    return (pid_t)ppid;
}

/* Resolve DISPLAY for a requesting process, transparently walking up to the
 * parent if the process itself has no readable DISPLAY (the setuid-pkexec
 * case).  Tries up to a few levels so pkexec->sh->app chains resolve. */
static char *resolve_requester_display(pid_t pid) {
    for (int depth = 0; pid > 1 && depth < 4; depth++) {
        char *d = get_display_for_pid(pid);
        if (d && d[0] != '\0') return d;
        if (d) free(d);
        pid = get_ppid(pid);
    }
    return NULL;
}

/* Setup failed before the dialog could run: report a cancel through the
 * normal channel so the GLib thread doesn't block forever on response_efd. */
static void auth_dialog_abort(void) {
    pthread_mutex_lock(&g_auth.lock);
    g_auth.cancelled = TRUE;
    g_auth.state = AUTH_STATE_RESPONDED;
    pthread_mutex_unlock(&g_auth.lock);

    uint64_t v = 1;
    (void)write(g_auth.response_efd, &v, sizeof(v));
}

/*
 * Blocks until the user submits or cancels.
 * Reads message/identity from g_auth, writes password/cancelled back.
 * Called from the main (XCB) thread.
 */
static void run_auth_dialog(const char *display) {
    agent_log("run_auth_dialog: entered (display arg=%s)",
              display ? display : "(null)");

    /* Clear any stale cancel token from a previous request so this fresh
     * dialog doesn't immediately tear itself down on entry. */
    {
        int fl = fcntl(g_auth.cancel_efd, F_GETFL, 0);
        if (fl != -1) {
            fcntl(g_auth.cancel_efd, F_SETFL, fl | O_NONBLOCK);
            uint64_t drain;
            while (read(g_auth.cancel_efd, &drain, sizeof(drain)) > 0) { }
            fcntl(g_auth.cancel_efd, F_SETFL, fl);
        }
    }

    if (display)
        setenv("DISPLAY", display, 1);

    int window_width  = 600;
    int window_height = 300;
    const char *window_title = "Stellar Authentication";

    int stellar_screen = 0;

    /* The agent is spawned by Stellar (via xdg-autostart) without
     * STELLAR_SCREEN in its environment -- that variable is only ever set in
     * the per-screen AwesomeWM children, never in Stellar's master env that we
     * inherit.  The agent is a single global process serving auth prompts on
     * every screen, so there is no single correct STELLAR_SCREEN for it; the
     * screen is a property of each individual request.  We already resolved
     * the requesting app's DISPLAY (e.g. ":0.1") from its pid, so ask the DE
     * which screen owns that display.  The DE matches against its own
     * canonical display_name strings, which is correct even on non-:0 servers
     * (Xephyr :3.0-:3.2) where parsing the suffix here would be unreliable.
     * Fall back to STELLAR_SCREEN (if somehow set), then 0. */
    int resolved = stellar_screen_for_display(display);
    if (resolved >= 0) {
        stellar_screen = resolved;
    } else {
        const char *screen_env = getenv("STELLAR_SCREEN");
        if (screen_env) stellar_screen = atoi(screen_env);
    }
    agent_log("screen selection: display=%s DE-resolved=%d STELLAR_SCREEN=%s -> using screen=%d",
              display ? display : "(null)", resolved,
              getenv("STELLAR_SCREEN") ? getenv("STELLAR_SCREEN") : "(unset)",
              stellar_screen);

    /* Log the environment as it stands AT THE MOMENT of the theme request.
     * This is the whole point of the exercise: if STELLAR_SOCKET is unset
     * here, request_theme_data() can never reach the DE and we fall back to
     * the wrong default theme (black-on-black / wrong font). */
    agent_log_env("before request_theme_data");
    agent_log("requesting theme for screen=%d", stellar_screen);

    ThemeData theme_data;
    int rc = request_theme_data(stellar_screen, &theme_data);
    int have_theme = (rc == 0);

    agent_log("request_theme_data returned %d (have_theme=%d)", rc, have_theme);
    if (have_theme) {
        agent_log("  theme: dpi=%d", theme_data.dpi);
        agent_log("  theme: nk_color_window='%s' nk_color_text='%s'",
                  theme_data.nk_color_window, theme_data.nk_color_text);
        agent_log("  theme: nk_color_button='%s' nk_color_border='%s'",
                  theme_data.nk_color_button, theme_data.nk_color_border);
        agent_log("  font: name='%s' size=%.1f path='%s' is_bitmap=%d",
                  theme_data.font, (double)theme_data.font_size,
                  theme_data.font_path, theme_data.font_is_bitmap);
        agent_log("  theme_dir='%s' assets_path='%s'",
                  theme_data.theme_dir, theme_data.assets_path);
    }

    if (!have_theme) {
        agent_log("NO THEME DATA -- falling back to hardcoded defaults "
                  "(this is the bug: dialog will be black-on-black / wrong font)");
        fprintf(stderr, "polkit-agent: no theme data from Stellar, using defaults\n");
        memset(&theme_data, 0, sizeof(theme_data));
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
    if (!xcb_ctx) {
        fprintf(stderr, "polkit-agent: cannot open X display\n");
        auth_dialog_abort();
        return;
    }
    nk_xcb_set_window_type(xcb_ctx, "_NET_WM_WINDOW_TYPE_DIALOG");
    nk_xcb_map(xcb_ctx);
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
        fprintf(stderr, "polkit-agent: out of memory\n");
        nk_cairo_free(cairo_ctx);
        nk_xcb_free(xcb_ctx);
        auth_dialog_abort();
        return;
    }
    nk_init_default(ctx, nk_cairo_default_font(cairo_ctx));

    // Same look as the settings app: theme colors + the configured system
    // font (bitmap fonts arrive here as transparently converted .otb files).
    int theme_rc = apply_nk_theme(ctx, cairo_ctx, &theme_data);
    agent_log("apply_nk_theme returned %d (0=ok, -1=font failed to load)",
              theme_rc);

    /* local copies so we don't hold the lock while rendering */
    char message[MAX_MESSAGE_LEN];
    char identity[MAX_IDENTITY_LEN];

    pthread_mutex_lock(&g_auth.lock);
    strncpy(message,  g_auth.message,  sizeof(message)  - 1);
    strncpy(identity, g_auth.identity, sizeof(identity) - 1);
    pthread_mutex_unlock(&g_auth.lock);

    char password_buf[MAX_PASSWORD_LEN] = {0};
    int  password_len = 0;
    int  running      = 1;
    gboolean cancelled = FALSE;

    while (running) {
        /* nk_xcb_handle_event() runs nk_input_begin()..translate..
         * nk_input_end() internally.  Key and button events do NOT set any
         * NK_XCB_EVENT_* bit, so the handler returns 0 for them -- the same
         * value as "nothing happened".  In immediate mode, input captured
         * between begin/end is only consumed when the widget frame
         * (nk_begin..nk_edit_string/nk_button..nk_end) runs AFTER nk_input_end
         * and BEFORE the next nk_input_begin.  So we must render a frame every
         * time the handler runs, or the next iteration's nk_input_begin wipes
         * the click/keystroke and the dialog ignores all input (the bug:
         * can't type, can't click).
         *
         * We do the sleeping ourselves on BOTH the X socket and the polkit
         * cancel fd, so a cancel is acted on instantly even when idle.
         * nk_xcb_handle_event() no longer blocks (it polls), so it is safe to
         * call it only when the X fd is readable. */
        struct pollfd pfds[2];
        pfds[0].fd = xcb_get_file_descriptor(xcb_ctx->conn);
        pfds[0].events = POLLIN; pfds[0].revents = 0;
        pfds[1].fd = g_auth.cancel_efd;
        pfds[1].events = POLLIN; pfds[1].revents = 0;

        int pr = poll(pfds, 2, -1);
        if (pr < 0) break;

        /* polkit cancelled while we were waiting */
        if (pfds[1].revents & POLLIN) {
            uint64_t v;
            (void)read(g_auth.cancel_efd, &v, sizeof(v));
            cancelled = TRUE;
            running = 0;
            break;
        }

        if (!(pfds[0].revents & POLLIN))
            continue;   /* only the cancel fd woke us; nothing to draw */

        int events = nk_xcb_handle_event(xcb_ctx, ctx);

        if (events & NK_XCB_EVENT_STOP) {
            cancelled = TRUE;
            running = 0;
            break;
        }

        /* Expose/map: window contents are gone but the command buffer is
         * unchanged, so force the repaint. */
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
            if (nk_begin(ctx, "Auth", nk_rect(0, 0, xcb_ctx->width, xcb_ctx->height), 0)) {

                nk_layout_space_begin(ctx, NK_STATIC, 16, 1);
                nk_layout_space_end(ctx);

                /* identity line */
                nk_layout_row_dynamic(ctx, 20, 1);
                nk_label(ctx, identity, NK_TEXT_LEFT);

                nk_layout_space_begin(ctx, NK_STATIC, 6, 1);
                nk_layout_space_end(ctx);

                /* message: wrapped, '\n' starts a new line */
                nk_label_wrap_multiline(ctx, message);

                nk_layout_space_begin(ctx, NK_STATIC, 10, 1);
                nk_layout_space_end(ctx);

                /* password field - masked */
                nk_layout_row_dynamic(ctx, 30, 1);
                nk_edit_string(ctx,
                    NK_EDIT_FIELD | NK_EDIT_SIG_ENTER | NK_EDIT_PASSWORD,
                    password_buf, &password_len, MAX_PASSWORD_LEN - 1,
                    nk_filter_default);

                nk_layout_space_begin(ctx, NK_STATIC, 8, 1);
                nk_layout_space_end(ctx);

                /* buttons */
                nk_layout_row_dynamic(ctx, 30, 2);
                if (nk_button_label(ctx, "Cancel")) {
                    cancelled = TRUE;
                    running = 0;
                }
                if (nk_button_label(ctx, "Authenticate")) {
                    password_buf[password_len] = '\0';
                    running = 0;
                }
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

    /* write result back to shared state and wake GLib thread */
    pthread_mutex_lock(&g_auth.lock);
    g_auth.cancelled = cancelled;
    if (!cancelled) {
        password_buf[password_len] = '\0';
        strncpy(g_auth.password, password_buf, MAX_PASSWORD_LEN - 1);
    }
    g_auth.state = AUTH_STATE_RESPONDED;
    pthread_mutex_unlock(&g_auth.lock);

    uint64_t v = 1;
    (void)write(g_auth.response_efd, &v, sizeof(v));
}

/* =========================================================
 * Polkit agent implementation
 * ========================================================= */

#define STELLAR_TYPE_AGENT (stellar_agent_get_type())
G_DECLARE_FINAL_TYPE(StellarAgent, stellar_agent, STELLAR, AGENT, PolkitAgentListener)

struct _StellarAgent {
    PolkitAgentListener parent;
};

G_DEFINE_TYPE(StellarAgent, stellar_agent, POLKIT_AGENT_TYPE_LISTENER)

/* Forward declarations */
static void stellar_agent_initiate_authentication(
    PolkitAgentListener  *listener,
    const gchar          *action_id,
    const gchar          *message,
    const gchar          *icon_name,
    PolkitDetails        *details,
    const gchar          *cookie,
    GList                *identities,
    GCancellable         *cancellable,
    GAsyncReadyCallback   callback,
    gpointer              user_data);

static gboolean stellar_agent_initiate_authentication_finish(
    PolkitAgentListener  *listener,
    GAsyncResult         *res,
    GError              **error);

static void stellar_agent_class_init(StellarAgentClass *klass) {
    PolkitAgentListenerClass *listener_class = POLKIT_AGENT_LISTENER_CLASS(klass);
    listener_class->initiate_authentication        = stellar_agent_initiate_authentication;
    listener_class->initiate_authentication_finish = stellar_agent_initiate_authentication_finish;
}

static void stellar_agent_init(StellarAgent *self) {
    (void)self;
}

static StellarAgent *stellar_agent_new(void) {
    return g_object_new(STELLAR_TYPE_AGENT, NULL);
}

/* ---- session callbacks ---- */

typedef struct {
    GTask              *task;
    PolkitAgentSession *session;
    GCancellable       *cancellable;   /* not owned; borrowed from the task    */
    gulong              cancel_id;      /* handler id for on_polkit_cancelled    */
} SessionData;

static void on_session_completed(PolkitAgentSession *session,
                                  gboolean            gained_authorization,
                                  gpointer            user_data) {
    SessionData *sd = user_data;
    (void)session;

    pthread_mutex_lock(&g_auth.lock);
    g_auth.password_ok = gained_authorization;
    /* This authentication is finished (success or failure); release the slot
     * so the next initiate_authentication is accepted instead of rejected. */
    g_auth.busy = FALSE;
    g_auth.state = AUTH_STATE_IDLE;
    pthread_mutex_unlock(&g_auth.lock);

    /* Disconnect the cancellable BEFORE completing the task.
     * Completing the task tells Polkit we are done, which causes Polkit 
     * to drop its reference to the cancellable. Unreffing the task drops 
     * our reference. If we disconnect after that, we are corrupting memory! */
    if (sd->cancellable && sd->cancel_id != 0)
        g_cancellable_disconnect(sd->cancellable, sd->cancel_id);

    if (gained_authorization) {
        g_task_return_boolean(sd->task, TRUE);
    } else {
        g_task_return_new_error(sd->task,
            POLKIT_ERROR, POLKIT_ERROR_FAILED,
            "Authentication failed");
    }

    g_object_unref(sd->task);
    
    /* Drop our reference on the per-request session created in
     * initiate_authentication; otherwise one PolkitAgentSession leaks per
     * prompt. */
    if (sd->session)
        g_object_unref(sd->session);
    g_free(sd);
}

static void on_session_request(PolkitAgentSession *session,
                                const gchar        *request,
                                gboolean            echo_on,
                                gpointer            user_data) {
    (void)echo_on;
    (void)user_data;
    (void)request;

    /* The session is asking for a password.
       Wake the XCB thread to show the dialog, then wait for response. */
    uint64_t v = 1;

    /* Drain any stale token left on response_efd. A cancel path may write
     * response_efd to unblock us; if a previous cycle already consumed the
     * real response and the cancel token arrived late, it would otherwise make
     * this read return immediately with no dialog shown. eventfd is a counter,
     * so a non-blocking drain clears it. (request_efd carries the dialog-show
     * signal; the dialog itself writes response_efd when the user acts.) */
    {
        int fl = fcntl(g_auth.response_efd, F_GETFL, 0);
        if (fl != -1) {
            fcntl(g_auth.response_efd, F_SETFL, fl | O_NONBLOCK);
            uint64_t drain;
            while (read(g_auth.response_efd, &drain, sizeof(drain)) > 0) { }
            fcntl(g_auth.response_efd, F_SETFL, fl);
        }
    }

	/* Reset state so the XCB loop knows to launch the dialog again 
	 * (critical if PAM rejects the password and asks for it again) */
	pthread_mutex_lock(&g_auth.lock);
	g_auth.state = AUTH_STATE_REQUESTED;
	g_auth.cancelled = FALSE;
	memset(g_auth.password, 0, MAX_PASSWORD_LEN);
	pthread_mutex_unlock(&g_auth.lock);

    (void)write(g_auth.request_efd, &v, sizeof(v));

    /* Block this GLib callback until XCB thread responds.
       We spin on the response eventfd with a blocking read. */
    (void)read(g_auth.response_efd, &v, sizeof(v));

    pthread_mutex_lock(&g_auth.lock);
    gboolean cancelled = g_auth.cancelled;
    char password[MAX_PASSWORD_LEN];
    strncpy(password, g_auth.password, MAX_PASSWORD_LEN - 1);
    pthread_mutex_unlock(&g_auth.lock);

    if (cancelled) {
        polkit_agent_session_cancel(session);
    } else {
        polkit_agent_session_response(session, password);
        /* scrub from memory */
        memset(password, 0, sizeof(password));
    }
}

static void on_session_show_error(PolkitAgentSession *session,
                                   const gchar        *text,
                                   gpointer            user_data) {
    (void)session; (void)user_data;
    fprintf(stderr, "stellar-polkit-agent: auth error: %s\n", text);
}

static void on_session_show_info(PolkitAgentSession *session,
                                  const gchar        *text,
                                  gpointer            user_data) {
    (void)session; (void)user_data;
    fprintf(stderr, "stellar-polkit-agent: auth info: %s\n", text);
}

/* Fired (on the GLib thread) when polkit cancels the authentication via its
 * GCancellable -- e.g. the requesting process died, the action was superseded,
 * or another agent handled it. Previously nothing was connected to the
 * cancellable, so a cancel left the dialog up forever and the GLib thread stuck
 * in read(response_efd); polkit's D-Bus side then timed out and tore down the
 * registration, leaving a wedged process behind. We now:
 *   1) mark the request cancelled,
 *   2) poke cancel_efd so a dialog blocked in poll() tears down immediately,
 *   3) poke response_efd so on_session_request, if it is mid-wait, unblocks
 *      and takes the cancel branch instead of submitting a stale password. */
static void on_polkit_cancelled(GCancellable *cancellable,
                                 gpointer      user_data) {
    (void)cancellable; (void)user_data;

    agent_log("on_polkit_cancelled: polkit cancelled the in-flight request");

    pthread_mutex_lock(&g_auth.lock);
    g_auth.cancelled = TRUE;
    g_auth.state     = AUTH_STATE_CANCELLED;
    pthread_mutex_unlock(&g_auth.lock);

    uint64_t v = 1;
    (void)write(g_auth.cancel_efd, &v, sizeof(v));
    (void)write(g_auth.response_efd, &v, sizeof(v));
}

/* ---- BeginAuthentication ---- */

static void stellar_agent_initiate_authentication(
    PolkitAgentListener  *listener,
    const gchar          *action_id,
    const gchar          *message,
    const gchar          *icon_name,
    PolkitDetails        *details,
    const gchar          *cookie,
    GList                *identities,
    GCancellable         *cancellable,
    GAsyncReadyCallback   callback,
    gpointer              user_data)
{
    (void)listener; (void)icon_name; (void)details; (void)cancellable;

    GTask *task = g_task_new(listener, cancellable, callback, user_data);

    /* Serialize: the agent shows exactly one dialog at a time and keeps a
     * single global g_auth slot. If a prompt is already in flight, reject this
     * one cleanly. polkit treats a failed registrant request as "this agent
     * couldn't handle it now" and re-drives the authentication, so the user
     * still gets prompted -- after the current dialog finishes -- without us
     * clobbering the in-flight display/cookie/session and deadlocking the two
     * eventfd handshakes against each other. */
    pthread_mutex_lock(&g_auth.lock);
    if (g_auth.busy) {
        pthread_mutex_unlock(&g_auth.lock);
        agent_log("initiate_authentication: REJECTED (agent busy with an "
                  "in-flight prompt); polkit will re-drive this request");
        g_task_return_new_error(task, POLKIT_ERROR, POLKIT_ERROR_FAILED,
                                "Stellar polkit agent busy");
        g_object_unref(task);
        return;
    }
    g_auth.busy = TRUE;
    pthread_mutex_unlock(&g_auth.lock);

    /* Pick the first identity (usually the current user) */
    PolkitIdentity *identity = NULL;
    if (identities)
        identity = POLKIT_IDENTITY(identities->data);

    gchar *identity_str = identity
        ? polkit_identity_to_string(identity)
        : g_strdup("unknown");

	const gchar *pid_str = polkit_details_lookup(details, "polkit.subject-pid");
	pid_t pid = pid_str ? (pid_t)atoi(pid_str) : 0;
	char *display = pid ? resolve_requester_display(pid) : NULL;

	/* The requesting process may have no readable DISPLAY -- e.g. when the
	 * action is raised by pkexec (setuid root), whose /proc/<pid>/environ we
	 * cannot read as a normal user.  resolve_requester_display() already walks
	 * up the parent chain to find a readable DISPLAY, but if even that fails
	 * we must NOT try to render on an empty display string (the dialog would
	 * never appear, and pkexec then denies with "Not authorized").  Fall back
	 * to the agent's own DISPLAY (it was autostarted on screen 0), then ":0"
	 * as a last resort, so the dialog still shows -- on the primary screen if
	 * we cannot determine the requester's. */
	const char *display_source = "requester (or parent) environ";
	if (!display || display[0] == '\0') {
		if (display) { free(display); display = NULL; }
		const char *own = getenv("DISPLAY");
		if (own && own[0] != '\0') {
			display = strdup(own);
			display_source = "agent's own DISPLAY (requester unreadable)";
		} else {
			display = strdup(":0");
			display_source = "hardcoded :0 (no DISPLAY anywhere)";
		}
	}

	agent_log("initiate_authentication: action='%s' subject-pid=%s resolved DISPLAY=%s [via %s]",
	          action_id, pid_str ? pid_str : "(none)",
	          display ? display : "(null)", display_source);

    /* Fill shared auth state */
    pthread_mutex_lock(&g_auth.lock);

	g_auth.display[0] = '\0';
	if (display) {
		strncpy(g_auth.display, display, sizeof(g_auth.display) - 1);
		free(display);
	}

    snprintf(g_auth.message,  MAX_MESSAGE_LEN,  "%s", message);
    snprintf(g_auth.identity, MAX_IDENTITY_LEN, "Authenticate as: %s", identity_str);
    g_auth.state    = AUTH_STATE_REQUESTED;
    g_auth.cancelled = FALSE;
    memset(g_auth.password, 0, MAX_PASSWORD_LEN);
    pthread_mutex_unlock(&g_auth.lock);

    g_free(identity_str);

    fprintf(stderr, "stellar-polkit-agent: auth requested for action: %s\n", action_id);

    /* Create a polkit session for the chosen identity */
    SessionData *sd = g_new0(SessionData, 1);
    sd->task = task;

    PolkitAgentSession *session = polkit_agent_session_new(identity, cookie);
    sd->session = session;

    g_signal_connect(session, "completed",   G_CALLBACK(on_session_completed),   sd);
    g_signal_connect(session, "request",     G_CALLBACK(on_session_request),     sd);
    g_signal_connect(session, "show-error",  G_CALLBACK(on_session_show_error),  sd);
    g_signal_connect(session, "show-info",   G_CALLBACK(on_session_show_info),   sd);

    /* Wire polkit's cancellation to our dialog teardown. Keep the handler id so
     * on_session_completed can disconnect it before the task/cancellable go
     * away. */
    sd->cancellable = cancellable;
    sd->cancel_id   = 0;
    if (cancellable) {
        sd->cancel_id = g_cancellable_connect(
            cancellable, G_CALLBACK(on_polkit_cancelled), sd, NULL);
    }

    polkit_agent_session_initiate(session);
}

static gboolean stellar_agent_initiate_authentication_finish(
    PolkitAgentListener  *listener,
    GAsyncResult         *res,
    GError              **error)
{
    (void)listener;
    return g_task_propagate_boolean(G_TASK(res), error);
}

/* =========================================================
 * GLib main loop thread
 * ========================================================= */

static GMainLoop *g_mainloop = NULL;

static void *glib_thread_func(void *arg) {
    (void)arg;
    g_mainloop = g_main_loop_new(NULL, FALSE);
    g_main_loop_run(g_mainloop);
    return NULL;
}

/* =========================================================
 * XCB event loop - waits for auth requests and shows dialogs
 * ========================================================= */

static void xcb_event_loop(void) {
    struct pollfd pfd;
    pfd.fd     = g_auth.request_efd;
    pfd.events = POLLIN;

    while (1) {
        pfd.revents = 0;
        int pr = poll(&pfd, 1, -1);
        if (pr < 0) {
            perror("poll");
            break;
        }

        if (pfd.revents & POLLIN) {
            uint64_t v;
            (void)read(g_auth.request_efd, &v, sizeof(v));

            pthread_mutex_lock(&g_auth.lock);
            AuthState state = g_auth.state;
            pthread_mutex_unlock(&g_auth.lock);

            if (state == AUTH_STATE_REQUESTED) {
                run_auth_dialog(g_auth.display);
            }
            /* AUTH_STATE_CANCELLED is handled inside run_auth_dialog */
        }
    }
}

/* =========================================================
 * main
 * ========================================================= */

int main(void) {
    agent_log("==== stellar-polkit-agent starting ====");
    agent_log_env("at startup (main)");

    /* Init GLib type system */
    g_type_init_with_debug_flags(0);

    /* Init shared state */
    pthread_mutex_init(&g_auth.lock, NULL);
    g_auth.request_efd  = eventfd(0, EFD_CLOEXEC);
    g_auth.response_efd = eventfd(0, EFD_CLOEXEC);
    g_auth.cancel_efd   = eventfd(0, EFD_CLOEXEC);
    g_auth.state        = AUTH_STATE_IDLE;
    g_auth.busy         = FALSE;

    if (g_auth.request_efd < 0 || g_auth.response_efd < 0 ||
        g_auth.cancel_efd < 0) {
        perror("eventfd");
        return 1;
    }

    /* Start GLib thread */
    pthread_t glib_thread;
    pthread_create(&glib_thread, NULL, glib_thread_func, NULL);

    /* Give GLib a moment to start its loop */
    g_usleep(100 * 1000);

    /* Register as polkit agent */
    GError *error = NULL;
    PolkitSubject *subject = polkit_unix_session_new_for_process_sync(
        getpid(), NULL, &error);

    if (!subject) {
        fprintf(stderr, "stellar-polkit-agent: failed to get session: %s\n",
                error ? error->message : "unknown");
        return 1;
    }

    StellarAgent *agent = stellar_agent_new();
    gpointer registered = polkit_agent_listener_register(
        POLKIT_AGENT_LISTENER(agent),
        POLKIT_AGENT_REGISTER_FLAGS_NONE,
        subject,
        NULL,   /* object path, NULL = default */
        NULL,   /* cancellable */
        &error);

    g_object_unref(subject);

    if (!registered) {
        fprintf(stderr, "stellar-polkit-agent: failed to register agent: %s\n",
                error ? error->message : "unknown");
        return 1;
    }

    fprintf(stderr, "stellar-polkit-agent: registered, waiting for auth requests\n");

    /* Run the XCB event loop on the main thread */
    xcb_event_loop();

    /* Cleanup (unreachable in normal operation) */
    polkit_agent_listener_unregister(registered);
    g_object_unref(agent);
    g_main_loop_quit(g_mainloop);
    pthread_join(glib_thread, NULL);
    pthread_mutex_destroy(&g_auth.lock);
    close(g_auth.request_efd);
    close(g_auth.response_efd);
    close(g_auth.cancel_efd);

    return 0;
}
