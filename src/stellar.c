// stellar.c
#include "stellar.h"
#include "stellar_font.h"
#include "xdnd.h"
#include "power.h"
#include "ipc_lua.h"
#include "xresources.h"
#include "monitor.h"
#include "xdg_autostart.h"
#include "xdg_menu.h"
#include "menu_watch.h"

static volatile sig_atomic_t g_running = 1;
static volatile sig_atomic_t g_sigchld = 0;

static PendingWindow pending_windows[MAX_PENDING_WINDOWS];
static TrackedWindow tracked_windows[MAX_TRACKED_WINDOWS];
static int pending_count = 0;
static int tracked_count = 0;

StellarState *g_state = NULL;

/* ---------- Utility ---------- */

FILE *log_file = NULL;

void init_log_file(void) {
    if (log_file == NULL) {
        rename(STELLAR_LOG_PATH, STELLAR_LOG_OLD_PATH);
        log_file = fopen("/tmp/stellar.log", "a");
    }
}

void log_info(const char *fmt, ...) {
    va_list ap;
    va_list ap_copy;

    va_start(ap, fmt);
    va_copy(ap_copy, ap);

    fprintf(stdout, "[stellar] ");
    vfprintf(stdout, fmt, ap);
    fprintf(stdout, "\n");
    fflush(stdout);

    init_log_file();
    if (log_file != NULL) {
        fprintf(log_file, "[stellar] ");
        vfprintf(log_file, fmt, ap_copy);
        fprintf(log_file, "\n");
        fflush(log_file);
    }

    va_end(ap_copy);
    va_end(ap);
}

void log_error(const char *fmt, ...) {
    va_list ap;
    va_list ap_copy;

    va_start(ap, fmt);
    va_copy(ap_copy, ap);

    fprintf(stderr, "[stellar] ERROR: ");
    vfprintf(stderr, fmt, ap);
    fprintf(stderr, "\n");
    fflush(stderr);

    init_log_file();
    if (log_file != NULL) {
        fprintf(log_file, "[stellar] ERROR: ");
        vfprintf(log_file, fmt, ap_copy);
        fprintf(log_file, "\n");
        fflush(log_file);
    }

    va_end(ap_copy);
    va_end(ap);
}

const char* get_cursor_theme_for_screen(StellarState *st, int screen_num) {
    ScreenState *sc = &st->screens[screen_num];
    if (sc->config.override_global_appearance && sc->config.appearance.cursor_theme[0] != '\0') {
        return sc->config.appearance.cursor_theme;
    }
    return st->config.appearance.cursor_theme;
}

int get_cursor_size_for_screen(StellarState *st, int screen_num) {
    ScreenState *sc = &st->screens[screen_num];
    if (sc->config.override_global_appearance && sc->config.appearance.cursor_size > 0) {
        return sc->config.appearance.cursor_size;
    }
    return st->config.appearance.cursor_size;
}

const char* get_font_name_for_screen(StellarState *st, int screen_num) {
    ScreenState *sc = &st->screens[screen_num];
    if (sc->config.override_global_appearance && sc->config.appearance.font_name[0] != '\0') {
        return sc->config.appearance.font_name;
    }
    return st->config.appearance.font_name;
}

float get_font_size_for_screen(StellarState *st, int screen_num) {
    ScreenState *sc = &st->screens[screen_num];
    if (sc->config.override_global_appearance && sc->config.appearance.font_size > 0) {
        return sc->config.appearance.font_size;
    }
    return st->config.appearance.font_size;
}

const char *get_font_unit_for_screen(StellarState *st, int screen_num) {
    ScreenState *sc = &st->screens[screen_num];
    
    // Check if the screen overrides the global settings AND actually has a unit defined
    if (sc->config.override_global_appearance && sc->config.appearance.font_unit[0] != '\0') {
        return sc->config.appearance.font_unit;
    }
    
    // Fall back to the global unit
    return st->config.appearance.font_unit;
}

// Minimal JSON string escaping (quotes and backslashes) for IPC replies.
static void json_escape_into(char *dst, size_t dst_size, const char *src) {
    size_t w = 0;
    for (const char *p = src; *p && w + 2 < dst_size; p++) {
        if (*p == '"' || *p == '\\') dst[w++] = '\\';
        dst[w++] = *p;
    }
    dst[w] = '\0';
}

void ipc_handle_get_appearance(StellarState *st, int fd, int screen_num) {
    if (screen_num < 0 || screen_num >= st->config.screen_count) {
        screen_num = 0;
    }

    const char *name = get_font_name_for_screen(st, screen_num);
    float size = get_font_size_for_screen(st, screen_num);
    
    // Fetch the configured unit, defaulting to pt for safety
    const char *unit = get_font_unit_for_screen(st, screen_num);
    if (!unit || unit[0] == '\0') unit = "pt";

    int dpi = st->screens[screen_num].dpi > 0 ? st->screens[screen_num].dpi : 96;

    StellarFontInfo fi;
    memset(&fi, 0, sizeof(fi));
    
    // Pass the unit into the newly updated resolver
    if (stellar_font_resolve(name, size, unit, dpi, &fi) != 0) {
        log_error("GET_APPEARANCE: could not resolve font '%s'", name);
        // Still answer with the raw name so the client can try itself.
        snprintf(fi.family, sizeof(fi.family), "%s", name ? name : "");
        fi.size_px = size;
    }

    char fam_esc[256], path_esc[PATH_MAX * 2], cur_esc[256], pango_esc[320];
    json_escape_into(fam_esc, sizeof(fam_esc), fi.family);
    json_escape_into(path_esc, sizeof(path_esc), fi.path);
    json_escape_into(pango_esc, sizeof(pango_esc), fi.pango_desc);
    json_escape_into(cur_esc, sizeof(cur_esc),
                     get_cursor_theme_for_screen(st, screen_num));

    // Determine what size and unit to broadcast to the C clients.
    float out_size = size;
    const char *out_unit = unit;

    if (fi.is_bitmap) {
        // Fontconfig snapped this to a specific physical strike.
        // Override the output so downstream Cairo apps don't attempt to DPI-scale it.
        out_size = fi.size_px;
        out_unit = "px";
    }

    dprintf(fd,
        "{\"screen\":%d,"
        "\"font_name\":\"%s\","
        "\"font_size\":%g,"
        "\"font_unit\":\"%s\","
        "\"font_path\":\"%s\","
        "\"font_is_bitmap\":%s,"
        "\"pango_desc\":\"%s\","
        "\"dpi\":%d,"
        "\"cursor_theme\":\"%s\","
        "\"cursor_size\":%d}\n",
        screen_num,
        fam_esc,
        (double)out_size,
        out_unit,
        path_esc,
        fi.is_bitmap ? "true" : "false",
        pango_esc,
        dpi,
        cur_esc,
        get_cursor_size_for_screen(st, screen_num));
}

int get_screensaver_timeout_for_screen(StellarState *st, int screen_num) {
    ScreenState *sc = &st->screens[screen_num];
    if (sc->config.override_global_power && sc->config.power.timeout_screensaver > 0) {
        return sc->config.power.timeout_screensaver;
    }
    return st->config.power.timeout_screensaver;
}

int get_dpms_timeout_for_screen(StellarState *st, int screen_num) {
    ScreenState *sc = &st->screens[screen_num];
    if (sc->config.override_global_power && sc->config.power.timeout_dpms > 0) {
        return sc->config.power.timeout_dpms;
    }
    return st->config.power.timeout_dpms;
}

static long long now_ms(void) {
    struct timeval tv;
    gettimeofday(&tv, NULL);
    return ((long long)tv.tv_sec * 1000LL) + (tv.tv_usec / 1000);
}

static void trim_newlines(char *s) {
    size_t n = strlen(s);
    while (n > 0 && (s[n - 1] == '\n' || s[n - 1] == '\r')) {
        s[n - 1] = '\0';
        n--;
    }
}

static bool file_exists(const char *path) {
    return path && access(path, F_OK) == 0;
}

static void strip_shell_value(char *s) {
    if (!s) {
        return;
    }

    trim_newlines(s);

    char *semicolon = strchr(s, ';');
    if (semicolon) {
        *semicolon = '\0';
    }

    while (*s && isspace((unsigned char)*s)) {
        memmove(s, s + 1, strlen(s));
    }

    size_t len = strlen(s);
    while (len > 0 && isspace((unsigned char)s[len - 1])) {
        s[len - 1] = '\0';
        len--;
    }

    if (len >= 2 && s[0] == '\'' && s[len - 1] == '\'') {
        memmove(s, s + 1, len - 2);
        s[len - 2] = '\0';
        return;
    }

    if (len >= 2 && s[0] == '"' && s[len - 1] == '"') {
        memmove(s, s + 1, len - 2);
        s[len - 2] = '\0';
    }
}

static void init_stellar_cache_dirs(StellarState *st) {
    const char *home = get_user_home_dir();
    char path[PATH_MAX];

    snprintf(path, sizeof(path), "%s/.cache/stellar", home);
    if (mkdir(path, 0755) != 0 && errno != EEXIST) {
        log_error("Failed to create %s: %s", path, strerror(errno));
    }

    snprintf(path, sizeof(path), "%s/.cache/stellar/xsettingsd", home);
    if (mkdir(path, 0755) != 0 && errno != EEXIST) {
        log_error("Failed to create %s: %s", path, strerror(errno));
    }

    snprintf(path, sizeof(path), "%s/.cache/stellar/picom", home);
    if (mkdir(path, 0755) != 0 && errno != EEXIST) {
        log_error("Failed to create %s: %s", path, strerror(errno));
    }
}

static int calc_dpi(Display *dpy, int screen_num) {
    double px = (double)DisplayWidth(dpy, screen_num);
    double mm = (double)DisplayWidthMM(dpy, screen_num);
	int dpi = 96;

    if (DisplayHeight(dpy, screen_num) <= 480) {
		dpi = 48;
	} else if (mm > 48) {
    	// Calculate, round to nearest whole number, and cast to int
		dpi = (int)round((px * 25.4) / mm);
	}

	log_info("DPI calculated: px=%f mm=%f dpi=%d", px, mm, dpi);
	return dpi;
}

// Apply DPI overrides from config, or auto-calculate from EDID dimensions.
// Must be called after both parse_settings_from_json and monitor_update_all_screens
// so that the config and EDID data are both available.
static void apply_dpi_settings(StellarState *st) {
    for (int i = 0; i < st->config.screen_count; i++) {
        ScreenState *sc = &st->screens[i];
        int old_dpi = sc->dpi;

        if (sc->config.dpi_override > 0) {
            // Explicit override from settings
            sc->dpi = sc->config.dpi_override;
        } else if (sc->monitor_phys_width_mm > 0 && sc->monitor_width > 0) {
            // Auto: use native resolution and EDID physical width, snap down
            int raw = (int)(sc->monitor_width * 25.4 / sc->monitor_phys_width_mm);
            sc->dpi = snap_dpi_down(raw);
        }
        // else: keep the X-calculated value from calc_dpi (96 fallback)

        sc->scale = (double)sc->dpi / 96.0;

        if (sc->dpi != old_dpi) {
            log_info("Screen %d: DPI adjusted %d -> %d (scale=%.2f, override=%d)",
                     i, old_dpi, sc->dpi, sc->scale, sc->config.dpi_override);
        }
    }
}

/* ---------- DBUS ---------- */

static bool adopt_runtime_dbus(StellarState *st) {
    (void)st;

    const char *addr = getenv("DBUS_SESSION_BUS_ADDRESS");
    if (addr && addr[0] != '\0') {
        log_info("using existing D-Bus session bus: %s", addr);
        return true;
    }

    const char *xdg_runtime = getenv("XDG_RUNTIME_DIR");
    if (xdg_runtime && xdg_runtime[0] != '\0') {
        char bus_path[PATH_MAX];
        char addr_buf[PATH_MAX + 32];

        snprintf(bus_path, sizeof(bus_path), "%s/bus", xdg_runtime);
        if (file_exists(bus_path)) {
            snprintf(
                addr_buf,
                sizeof(addr_buf),
                "unix:path=%s",
                bus_path
            );
            setenv("DBUS_SESSION_BUS_ADDRESS", addr_buf, 1);
            log_info("adopted runtime D-Bus session bus: %s", addr_buf);
            return true;
        }
    }

    {
        uid_t uid = getuid();
        char bus_path[PATH_MAX];
        char addr_buf[PATH_MAX + 32];

        snprintf(bus_path, sizeof(bus_path), "/run/user/%u/bus", (unsigned)uid);
        if (file_exists(bus_path)) {
            snprintf(
                addr_buf,
                sizeof(addr_buf),
                "unix:path=%s",
                bus_path
            );
            setenv("DBUS_SESSION_BUS_ADDRESS", addr_buf, 1);
            log_info("adopted fallback D-Bus session bus: %s", addr_buf);
            return true;
        }
    }

    return false;
}

static bool start_private_dbus(StellarState *st) {
    FILE *fp = popen("dbus-launch --sh-syntax", "r");
    if (!fp) {
        log_error("failed to run dbus-launch: %s", strerror(errno));
        return false;
    }

    char line[512];
    char address[512] = {0};
    pid_t pid = 0;

    while (fgets(line, sizeof(line), fp)) {
        trim_newlines(line);

        if (strncmp(line, "DBUS_SESSION_BUS_ADDRESS=", 25) == 0) {
            snprintf(address, sizeof(address), "%s", line + 25);
            strip_shell_value(address);
        } else if (strncmp(line, "DBUS_SESSION_BUS_PID=", 21) == 0) {
            char pidbuf[64];
            snprintf(pidbuf, sizeof(pidbuf), "%s", line + 21);
            strip_shell_value(pidbuf);
            pid = (pid_t)atoi(pidbuf);
        }
    }

    int rc = pclose(fp);
    if (rc == -1) {
        log_error("pclose(dbus-launch) failed: %s", strerror(errno));
    }

    if (address[0] == '\0') {
        log_error("dbus-launch did not return DBUS_SESSION_BUS_ADDRESS");
        return false;
    }

    setenv("DBUS_SESSION_BUS_ADDRESS", address, 1);

    if (pid > 0) {
        char pid_str[64];
        snprintf(pid_str, sizeof(pid_str), "%d", (int)pid);
        setenv("DBUS_SESSION_BUS_PID", pid_str, 1);
        st->dbus_session_pid = pid;
        st->dbus_started_by_stellar = true;
        log_info("started private D-Bus session bus pid=%d", (int)pid);
    } else {
        log_info("started private D-Bus session bus (pid unknown)");
    }

    log_info("DBUS_SESSION_BUS_ADDRESS=%s", address);
    return true;
}

static bool ensure_dbus_session(StellarState *st) {
    if (adopt_runtime_dbus(st)) {
        return true;
    }

    log_info("no existing D-Bus session bus found; starting one");
    return start_private_dbus(st);
}

static void sync_dbus_environment(void) {
    pid_t pid = fork();
    if (pid < 0) {
        log_error("fork failed for dbus-update-activation-environment: %s", strerror(errno));
        return;
    }

    if (pid == 0) {
        // Child: Push all current environment variables to systemd and D-Bus
        execlp(
            "dbus-update-activation-environment",
            "dbus-update-activation-environment",
            "--systemd",
            "--all",
            (char *)NULL
        );
        _exit(127);
    }

    // Wait for the environment synchronization to finish completely
    int status;
    while (waitpid(pid, &status, 0) < 0 && errno == EINTR);

    log_info("Synchronized environment variables with D-Bus and systemd");

    // Force xdg-desktop-portal to restart so it inherits the fresh environment
    pid_t restart_pid = fork();
    if (restart_pid == 0) {
        execlp("systemctl", "systemctl", "--user", "restart", "xdg-desktop-portal.service", (char *)NULL);
        _exit(127);
    }
    while (waitpid(restart_pid, &status, 0) < 0 && errno == EINTR);
    log_info("Signaled xdg-desktop-portal to cycle and reload session backends");
}

/* ---------- Signal Handling ---------- */

static void on_sigchld(int sig) {
    (void)sig;
    g_sigchld = 1;
}

static void on_terminate(int sig) {
    (void)sig;
    g_running = 0;
}

/* ---------- IPC Helpers ---------- */

int ensure_runtime_dir_and_socket_path(
    StellarState *st
) {
    const char *runtime = getenv("XDG_RUNTIME_DIR");
    if (runtime && runtime[0] != '\0') {
        char dir[PATH_MAX];
        snprintf(dir, sizeof(dir), "%s/stellar", runtime);

        if (mkdir(dir, 0700) == -1 && errno != EEXIST) {
            log_error("mkdir(%s) failed: %s", dir, strerror(errno));
            return -1;
        }

        snprintf(
            st->socket_path,
            sizeof(st->socket_path),
            "%s/bridge.sock",
            dir
        );
        return 0;
    }

    uid_t uid = getuid();
    snprintf(
        st->socket_path,
        sizeof(st->socket_path),
        "/tmp/stellar-%u.sock",
        (unsigned)uid
    );
    return 0;
}

/* ------------ System Tray --------- */

static void start_tray(StellarState *st, int screen_idx) {
	log_info("start_tray called for screen %d. ", screen_idx);
    ScreenState *sc = &st->screens[screen_idx];

    if (!sc->config.tray_enabled) {
		log_info("System tray is not enabled on screen %d. ", screen_idx);
		return;
	}

    if (sc->tray_pid > 0) {
		log_info("System tray is already running with PID %d on screen %d. ", sc->tray_pid, screen_idx);
		return;
	}

    time_t now = time(NULL);

    if (now - sc->tray_respawn_start > 10) {
        sc->tray_respawn_start = now;
        sc->tray_respawn_count = 0;
    }

    sc->tray_respawn_count++;

    if (sc->tray_respawn_count > 3) {
        log_error(
            "CRITICAL: System tray crash loop detected on screen %d. "
            "Disabling tray to prevent lockup.", 
            screen_idx
        );
        sc->config.tray_enabled = false; 
        return; 
    }

    pid_t pid = fork();
    if (pid == 0) {
		int icon_size = (int)(20.0f * sc->scale);
		int slot_size = (int)(24.0f * sc->scale);
        char icon_size_str[8];
        char slot_size_str[8];
		snprintf(icon_size_str, sizeof(icon_size_str), "%d", icon_size);
		snprintf(slot_size_str, sizeof(slot_size_str), "%d", slot_size);
        execlp(
            "stalonetray", 
            "stalonetray", 
			"-display", sc->display_name,
			"-bg", "black",
			"-i", icon_size_str,
			"-s", slot_size_str,
			"--kludges", "force_icons_size",
            NULL
        );
        exit(1);
    } else if (pid > 0) {
        sc->tray_pid = pid;
        log_info("System tray started on screen %d (attempt %d, PID %d)", 
                 screen_idx, sc->tray_respawn_count, pid);
    }
}

static void stop_tray(StellarState *st, int screen_idx) {
    ScreenState *sc = &st->screens[screen_idx];
    if (sc->tray_pid > 0) {
        log_info("Stopping System tray on screen %d, PID %d)", 
                 screen_idx, sc->tray_pid);
        kill(sc->tray_pid, SIGTERM);
    }
}

/* ------------ SNI tray bridge --------- */

static void start_snitray(StellarState *st) {
    log_info("start_snitray called.");

    if (st->snitray_pid > 0) {
        log_info("SNI tray bridge is already running with PID %d.",
                 st->snitray_pid);
        return;
    }

    time_t now = time(NULL);

    if (now - st->snitray_respawn_start > 10) {
        st->snitray_respawn_start = now;
        st->snitray_respawn_count = 0;
    }

    st->snitray_respawn_count++;

    if (st->snitray_respawn_count > 3) {
        log_error(
            "CRITICAL: SNI tray bridge crash loop detected. "
            "Not respawning to prevent lockup."
        );
        return;
    }

    pid_t pid = fork();
    if (pid == 0) {
    	char snitray_path[PATH_MAX];
    	snprintf(snitray_path, sizeof(snitray_path), "%s/stellar-snitray", STELLAR_LIBEXEC_PATH);
        execl(
			snitray_path,
			"stellar-snitray",
            "--replace",
			NULL
		);
        log_error("Failed to execute %s: %s", snitray_path, strerror(errno));
        exit(1);
    } else if (pid > 0) {
        st->snitray_pid = pid;
        log_info("SNI tray bridge started (attempt %d, PID %d)",
                 st->snitray_respawn_count, pid);
    } else {
        log_error("Failed to fork for stellar-snitray: %s", strerror(errno));
    }
}

static void stop_snitray(StellarState *st) {
    if (st->snitray_pid > 0) {
        log_info("Stopping SNI tray bridge, PID %d", st->snitray_pid);
        kill(st->snitray_pid, SIGTERM);
    }
}

/* ---------- Compositor ---------- */

static void start_compositor(StellarState *st, int screen_idx) {
    ScreenState *sc = &st->screens[screen_idx];

    if (!sc->config.picom_enabled) {
		log_info("Picom is not enabled on screen %d. ", screen_idx);
		return;
	}

    if (sc->picom_pid > 0) {
		log_info("Picom is already running with PID %d on screen %d. ", sc->picom_pid, screen_idx);
		return;
	}

    time_t now = time(NULL);

    if (now - sc->picom_respawn_start > 10) {
        sc->picom_respawn_start = now;
        sc->picom_respawn_count = 0;
    }

    sc->picom_respawn_count++;

    if (sc->picom_respawn_count > 3) {
        log_error(
            "CRITICAL: Picom crash loop detected on screen %d. "
            "Disabling compositor to prevent lockup.", 
            screen_idx
        );
        sc->config.picom_enabled = false; 
        return; 
    }

    pid_t pid = fork();
    if (pid == 0) {
        // Force picom to bind to this specific screen (e.g., ":0.1")
        char display_env[64];
        snprintf(display_env, sizeof(display_env), "DISPLAY=%s", sc->display_name);
        putenv(display_env);

		char conf_path[PATH_MAX];
		snprintf(conf_path, sizeof(conf_path), "%s/.cache/stellar/picom/screen_%d.conf", get_user_home_dir(), screen_idx);

        execlp(
            "picom", 
            "picom", 
            "--unredir-if-possible",
            "--config", conf_path,
            NULL
        );
        exit(1);
    } else if (pid > 0) {
        sc->picom_pid = pid;
        log_info("Picom started on screen %d (attempt %d, PID %d)", 
                 screen_idx, sc->picom_respawn_count, pid);
    }
}

static void stop_compositor(StellarState *st, int screen_idx) {
    ScreenState *sc = &st->screens[screen_idx];
    if (sc->picom_pid > 0) {
        log_info("Stopping Picom compositor...");
        log_info("Stopping Picom compositor on screen %d, PID %d)", 
                 screen_idx, sc->picom_pid);
        kill(sc->picom_pid, SIGTERM);
    }
}

/* ---------- X11 ---------- */

static int x11_error_handler(Display *dpy, XErrorEvent *err) {
    // Silently ignore BadWindow
    if (err->error_code == BadWindow) {
        return 0; 
    }
    
    // For other errors, log them
    char msg[256];
    XGetErrorText(dpy, err->error_code, msg, sizeof(msg));
    fprintf(stderr, "[stellar] Ignored non-fatal X11 Error: %s (opcode: %d)\n", msg, err->request_code);
    
    return 0; // Returning 0 prevents the default exit(1) behavior
}

static void add_pending(Window w, const char *class_name) {
    // Don't double-add
    for (int i = 0; i < pending_count; i++)
        if (pending_windows[i].win == w) return;
    if (pending_count >= MAX_PENDING_WINDOWS) return;
    PendingWindow *p = &pending_windows[pending_count++];
    p->win = w;
    snprintf(p->class_name, sizeof(p->class_name), "%s", class_name);
}

static PendingWindow *find_pending(Window w) {
    for (int i = 0; i < pending_count; i++) {
        if (pending_windows[i].win == w) return &pending_windows[i];
    }
    return NULL;
}

static void remove_pending(Window w) {
    for (int i = 0; i < pending_count; i++) {
        if (pending_windows[i].win == w) {
            pending_windows[i] = pending_windows[--pending_count];
            return;
        }
    }
}

static void add_tracked(Window w, const char *class_name, const char *win_name, Atom target_type) {
    // Don't double-add
    for (int i = 0; i < tracked_count; i++)
        if (tracked_windows[i].win == w) return;
    if (tracked_count >= MAX_TRACKED_WINDOWS) return;
    TrackedWindow *t = &tracked_windows[tracked_count++];
    t->win = w;
    t->target_type = target_type;
    t->remanaged = false;
    snprintf(t->class_name, sizeof(t->class_name), "%s", class_name);
    snprintf(t->win_name, sizeof(t->win_name), "%s", win_name);
}

static TrackedWindow *find_tracked(Window w) {
    for (int i = 0; i < tracked_count; i++)
        if (tracked_windows[i].win == w) return &tracked_windows[i];
    return NULL;
}

static void remove_tracked(Window w) {
    for (int i = 0; i < tracked_count; i++) {
        if (tracked_windows[i].win == w) {
            tracked_windows[i] = tracked_windows[--tracked_count];
            return;
        }
    }
}

static void make_screen_display_name(
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



static void init_cursor(StellarState *st, int screen_idx) {
	Window root = RootWindow(st->dpy, screen_idx);
	const char *theme = get_cursor_theme_for_screen(st, screen_idx);
	int size = get_cursor_size_for_screen(st, screen_idx);

    if (theme[0] != '\0') {
        XcursorSetTheme(st->dpy, theme);
    }

    if (size > 0) {
        XcursorSetDefaultSize(st->dpy, size);
    }

    Cursor cur = XcursorLibraryLoadCursor(st->dpy, "left_ptr");
    if (cur == None) {
        log_error(
            "failed to load themed cursor left_ptr for screen %d "
            "(theme=%s size=%d), falling back to core cursor",
            screen_idx,
            theme[0] != '\0' ? theme : "(empty)",
            size
        );
        cur = XCreateFontCursor(st->dpy, XC_left_ptr);
    } else {
        log_info(
            "applied root cursor on screen %d theme=%s size=%d",
            screen_idx,
            theme[0] != '\0' ? theme : "(empty)",
            size
        );
    }

    XDefineCursor(st->dpy, root, cur);
    XFlush(st->dpy);
    XFreeCursor(st->dpy, cur);
}

static void init_all_cursors(StellarState *st) {
    for (int idx = 0; idx < st->config.screen_count; idx++) {
        init_cursor(st, idx);
    }
}

// Warping the pointer between protocol screens while the SOURCE root carries an
// animated cursor (e.g. the startup-notification spinner) faults Xorg in its
// sprite handoff.  It must be  called immediately before XWarpPointer
void reset_cursor_sprite(StellarState *st, ScreenState *sc, int screen_idx) {
    const char *theme = get_cursor_theme_for_screen(st, screen_idx);
    int size = get_cursor_size_for_screen(st, screen_idx);

    if (theme && theme[0] != '\0') XcursorSetTheme(st->dpy, theme);
    if (size > 0) XcursorSetDefaultSize(st->dpy, size);

    Cursor cur = XcursorLibraryLoadCursor(st->dpy, "left_ptr");
    if (cur == None) cur = XCreateFontCursor(st->dpy, XC_left_ptr);
    if (cur != None) {
        XDefineCursor(st->dpy, sc->root, cur);
        XFreeCursor(st->dpy, cur);
    }

    XSync(st->dpy, False);
}

// Helper to find the application client window inside the AwesomeWM frame.
// Standard X11 practice is to search the children for the WM_STATE property.
static Window find_client_window(Display *dpy, Window frame) {
    Window root, parent, *children;
    unsigned int num_children;
    Window client = None;

    if (!XQueryTree(dpy, frame, &root, &parent, &children, &num_children)) {
        return None;
    }

    Atom wm_state = XInternAtom(dpy, "WM_STATE", True);

    for (unsigned int i = 0; i < num_children; i++) {
        Atom actual_type;
        int actual_format;
        unsigned long nitems, bytes_after;
        unsigned char *prop = NULL;

        // Check if this child has WM_STATE
        if (XGetWindowProperty(dpy, children[i], wm_state, 0, 0, False, AnyPropertyType,
                               &actual_type, &actual_format, &nitems, &bytes_after, &prop) == Success) {
            if (prop) XFree(prop);
            if (actual_type != None) {
                client = children[i];
                break;
            }
        }
    }
    
    if (children) XFree(children);
    return client;
}

// Helper to check if a window has custom locked property
static bool is_window_locked(Display *dpy, Window client_win) {
    Atom locked_atom = XInternAtom(dpy, "_STELLAR_LOCKED", True);
    // If the atom doesn't exist in the X server yet, no windows are locked.
    if (locked_atom == None) {
        return false;
    }

    Atom actual_type;
    int actual_format;
    unsigned long nitems, bytes_after;
    unsigned char *prop = NULL;
    bool is_locked = false;

    // Fetch the property. We expect it to be a 32-bit integer (standard for simple flags).
    if (XGetWindowProperty(dpy, client_win, locked_atom, 0, 1, False, AnyPropertyType,
                           &actual_type, &actual_format, &nitems, &bytes_after, &prop) == Success) {
        if (prop) {
            if (actual_type != None && nitems > 0) {
                // If the value is greater than 0, it is locked.
                is_locked = (*((long*)prop) != 0);
            }
            XFree(prop);
        }
    }
    
    return is_locked;
}

// Helper to check if pointer is inside the custom tab bar rectangle
static bool is_in_tab_rect(Display *dpy, Window client_win, int client_x, int client_y, unsigned int client_w, unsigned int client_h) {
    Atom tab_rect_atom = XInternAtom(dpy, "_STELLAR_TAB_BAR_RECT", True);
    if (tab_rect_atom == None) {
        return false;
    }

    Atom actual_type;
    int actual_format;
    unsigned long nitems, bytes_after;
    unsigned char *prop = NULL;
    bool in_rect = false;

    // Fetch the property
    if (XGetWindowProperty(dpy, client_win, tab_rect_atom, 0, 64, False, AnyPropertyType,
                           &actual_type, &actual_format, &nitems, &bytes_after, &prop) == Success) {
        
        // Ensure format is 8 (string) and we actually got data
        if (prop && actual_type != None && actual_format == 8 && nitems > 0) {
            char buffer[256] = {0};
            // Safely copy to ensure null-termination, preventing buffer over-reads
            snprintf(buffer, sizeof(buffer), "%.*s", (int)nitems, (char *)prop);

            char anchor[16];
            int x_off, y_off;
            unsigned int w, h;

            // Parse format: "anchor:x_off,y_off,w,h" (e.g., "bl:0,0,362,20")
            if (sscanf(buffer, "%15[^:]:%d,%d,%u,%u", anchor, &x_off, &y_off, &w, &h) == 5) {
                int rect_x = 0;
                int rect_y = 0;

                // -------------------------------------------------------------
                // Calculate absolute coordinates based on client_win's (0,0) origin.
                // Titlebars are OUTSIDE the client geometry.
                // -------------------------------------------------------------
                
                // Top Bars (Y is negative, above the client)
                if (strcmp(anchor, "tl") == 0) {
                    rect_x = x_off;
                    rect_y = -(int)h - y_off;
                } else if (strcmp(anchor, "tr") == 0) {
                    rect_x = (int)client_w - (int)w - x_off;
                    rect_y = -(int)h - y_off;
                } 
                // Bottom Bars (Y starts at client_h, extending downwards)
                else if (strcmp(anchor, "bl") == 0) {
                    rect_x = x_off;
                    rect_y = (int)client_h + y_off;
                } else if (strcmp(anchor, "br") == 0) {
                    rect_x = (int)client_w - (int)w - x_off;
                    rect_y = (int)client_h + y_off;
                }
                // Left Bars (X is negative, left of the client)
                else if (strcmp(anchor, "lt") == 0) {
                    rect_x = -(int)w - x_off;
                    rect_y = y_off;
                } else if (strcmp(anchor, "lb") == 0) {
                    rect_x = -(int)w - x_off;
                    rect_y = (int)client_h - (int)h - y_off;
                }
                // Right Bars (X starts at client_w, extending rightwards)
                else if (strcmp(anchor, "rt") == 0) {
                    rect_x = (int)client_w + x_off;
                    rect_y = y_off;
                } else if (strcmp(anchor, "rb") == 0) {
                    rect_x = (int)client_w + x_off;
                    rect_y = (int)client_h - (int)h - y_off;
                }

                // Check collision
                if (client_x >= rect_x && client_x < rect_x + (int)w &&
                    client_y >= rect_y && client_y < rect_y + (int)h) {
                    in_rect = true;
                }
            }
            XFree(prop);
        }
    }
    
    return in_rect;
}

static void process_hover_cursor(StellarState *st, Window child_ret, int root_x, int root_y) {
    static Window current_cursor_frame = None;
    static Cursor crosshair = None;

    if (crosshair == None) {
        int size = get_cursor_size_for_screen(st, st->pointer_screen);
        if (size > 0) XcursorSetDefaultSize(st->dpy, size);
        crosshair = XcursorLibraryLoadCursor(st->dpy, "crosshair");
    }

    // If child_ret is None, we are hovering on the desktop background.
    if (child_ret == None) {
        if (current_cursor_frame != None) {
            XDefineCursor(st->dpy, current_cursor_frame, None);
            current_cursor_frame = None;
        }
        return;
    }

    // Find the actual application client window inside the WM frame
    Window client_win = find_client_window(st->dpy, child_ret);

    if (client_win == None) {
        // Not a standard managed window (could be a dock, polybar, or unmapped).
        if (current_cursor_frame != None) {
            XDefineCursor(st->dpy, current_cursor_frame, None);
            current_cursor_frame = None;
        }
        return;
    }

    // Check if the window is locked via custom X property
    bool locked = is_window_locked(st->dpy, client_win);

    // Translate pointer coordinates relative to the client window
    int client_x, client_y;
    Window junk_child;
    XTranslateCoordinates(st->dpy, st->screens[st->pointer_screen].root, client_win, 
                          root_x, root_y, &client_x, &client_y, &junk_child);

    // Get client geometry
    unsigned int w, h, bw, depth;
    int jx, jy;
    Window jroot;
    XGetGeometry(st->dpy, client_win, &jroot, &jx, &jy, &w, &h, &bw, &depth);

    // Check if pointer is strictly inside the application content.
    // If the window is locked, we force this to true to bypass the crosshair frame logic.
    bool in_client = locked ||
        (client_x >= -(int)bw && client_y >= -(int)bw &&
         client_x < (int)(w + bw) && client_y < (int)(h + bw));

    // 5. Custom Exclusion Zone: If we are not in the client, check if we are in the tab bar.
    if (!in_client) {
        if (is_in_tab_rect(st->dpy, client_win, client_x, client_y, w, h)) {
            in_client = true; // Pretend we are in the client to preserve the standard cursor
        }
    }

    if (!in_client) {
        // We are inside the frame (child_ret) but OUTSIDE the client AND tab rect.
        // Therefore, we are hovering on the AwesomeWM titlebars/borders.
        if (current_cursor_frame != child_ret) {
            if (current_cursor_frame != None) {
                // Clear the old frame if we jumped directly to a new frame
                XDefineCursor(st->dpy, current_cursor_frame, None);
            }
            XDefineCursor(st->dpy, child_ret, crosshair);
            current_cursor_frame = child_ret;
            XFlush(st->dpy);
        }
    } else {
        // We are hovering over the actual application's internal geometry or the custom tab rect.
        // Strip the crosshair and let the application/system dictate the cursor.
        if (current_cursor_frame != None) {
            XDefineCursor(st->dpy, current_cursor_frame, None);
            current_cursor_frame = None;
            XFlush(st->dpy);
        }
    }
}

void cleanup_phantom_awesome_drawins(StellarState *st, int screen_num) {
	Window root = st->screens[screen_num].root;
    Window root_return, parent_return;
	Window *children = NULL;
    unsigned int num_children = 0;

    if (XQueryTree(st->dpy, root, &root_return, &parent_return, &children, &num_children) == 0) {
        return;
    }

    for (unsigned int i = 0; i < num_children; i++) {
        Window w = children[i];
        XWindowAttributes attrs;
        
        // Check if override_redirect is True
        if (XGetWindowAttributes(st->dpy, w, &attrs) == 0) continue;
        if (!attrs.override_redirect) continue;

        // Check the WM_CLASS
        XClassHint class_hint;
        bool is_awesome_class = false;
        if (XGetClassHint(st->dpy, w, &class_hint)) {
            if (class_hint.res_name && strcmp(class_hint.res_name, "awesome") == 0 &&
                class_hint.res_class && strcmp(class_hint.res_class, "awesome") == 0) {
                is_awesome_class = true;
            }
            if (class_hint.res_name) XFree(class_hint.res_name);
            if (class_hint.res_class) XFree(class_hint.res_class);
        }
        if (!is_awesome_class) continue;

        // Check the WM_NAME
        char *window_name = NULL;
        bool is_phantom_drawin = false;
        if (XFetchName(st->dpy, w, &window_name)) {
            if (window_name && strcmp(window_name, "Awesome drawin") == 0) {
                is_phantom_drawin = true;
            }
            XFree(window_name);
        }
        if (!is_phantom_drawin) continue;

        XDestroyWindow(st->dpy, w);
    }

    if (children) {
        XFree(children);
    }
    
    XSync(st->dpy, False); 
}

static bool init_x(StellarState *st) {
    st->dpy = XOpenDisplay(NULL);
    if (!st->dpy) {
        log_error("XOpenDisplay failed");
        return false;
    }

	XSetErrorHandler(x11_error_handler);

	log_info("connected to X display %s", DisplayString(st->dpy));

    st->xfd = ConnectionNumber(st->dpy);
    st->config.screen_count = ScreenCount(st->dpy);

    if (st->config.screen_count > MAX_SCREENS) {
        st->config.screen_count = MAX_SCREENS;
    }
	
	st->config.power.timeout_screensaver = 30;
	st->config.power.timeout_dpms = 60;
	st->config.saver_enabled = true;

    for (int i = 0; i < st->config.screen_count; i++) {
        ScreenState *sc = &st->screens[i];
        memset(sc, 0, sizeof(*sc));

        sc->screen_num = i;
        sc->root = RootWindow(st->dpy, i);
        sc->awesome_fd = -1;
		sc->pending_theme_request_fd = -1;
        sc->dpi = calc_dpi(st->dpy, i);
        sc->scale = (double)sc->dpi / 96.0;
        sc->respawn_window_start = 0;
        sc->respawn_count = 0;
        sc->config.neighbor_left = -1;
        sc->config.neighbor_right = -1;
        sc->config.neighbor_up = -1;
        sc->config.neighbor_down = -1;
        sc->config.phys_scale = 1.0f;
        sc->config.phys_offset = 1.0f;
		sc->config.picom_enabled = true;

		clock_gettime(CLOCK_MONOTONIC, &sc->last_activity);

        make_screen_display_name(
            DisplayString(st->dpy),
            i,
            sc->display_name,
            sizeof(sc->display_name)
        );

        XSelectInput(
            st->dpy,
            sc->root,
            SubstructureNotifyMask | EnterWindowMask | LeaveWindowMask |
                PropertyChangeMask
        );

        log_info(
            "screen=%d root=0x%lx dpi=%d scale=%.2f display=%s",
            i,
            sc->root,
            sc->dpi,
            sc->scale,
            sc->display_name
        );
    }

	init_xdnd_atoms(st);
	create_xdnd_proxy_windows(st);

	// --- XInput2 Setup ---
    int event, error;
    if (!XQueryExtension(st->dpy, "XInputExtension", &st->xi_opcode, &event, &error)) {
        log_error("XInput extension not available");
        return false;
    }

    int major = 2, minor = 0;
    if (XIQueryVersion(st->dpy, &major, &minor) == BadRequest) {
        log_error("XI2 not available");
        return false;
    }

    // Subscribe to raw motion from all master devices
    XIEventMask evmask;
    unsigned char mask[XIMaskLen(XI_RawMotion)] = { 0 };
    evmask.deviceid = XIAllMasterDevices;
    evmask.mask_len = sizeof(mask);
    evmask.mask = mask;
    XISetMask(mask, XI_RawMotion);
    XISetMask(mask, XI_RawKeyPress);

	for (int i = 0; i < st->config.screen_count; i++) {
		XISelectEvents(st->dpy, st->screens[i].root, &evmask, 1);
	}

    // --- RandR Setup ---
    if (!XRRQueryExtension(st->dpy, &st->rr_event_base, &st->rr_error_base)) {
        log_error("RandR extension not available for hotplug detection");
        st->rr_event_base = 0;
    } else {
        int rr_major = 0, rr_minor = 0;
        XRRQueryVersion(st->dpy, &rr_major, &rr_minor);
        log_info("RandR %d.%d available (event_base=%d)", rr_major, rr_minor, st->rr_event_base);

        for (int i = 0; i < st->config.screen_count; i++) {
            XRRSelectInput(st->dpy, st->screens[i].root,
                RRScreenChangeNotifyMask | RROutputChangeNotifyMask |
                RRCrtcChangeNotifyMask | RROutputPropertyNotifyMask);
        }
    }

    XSync(st->dpy, False);
    return true;
}

static int detect_pointer_screen(StellarState *st) {
    for (int i = 0; i < st->config.screen_count; i++) {
        Window root_ret, child_ret;
        int root_x, root_y, win_x, win_y;
        unsigned int mask;

        if (
            XQueryPointer(
                st->dpy,
                st->screens[i].root,
                &root_ret,
                &child_ret,
                &root_x,
                &root_y,
                &win_x,
                &win_y,
                &mask
            )
        ) {
            return i;
        }
    }

    return -1;
}

static int screen_from_root(StellarState *st, Window root) {
    for (int i = 0; i < st->config.screen_count; i++) {
        if (st->screens[i].root == root) {
            return i;
        }
    }
    return -1;
}

static void handle_pointer_tick(StellarState *st) {
	// XDND proxy: detect button release even without motion events
	if (st->xdnd_proxy.state == XDND_PROXY_DRAGGING) {
	    ScreenState *psc = &st->screens[st->xdnd_proxy.target_screen];
	    Window pr, cr;
	    int rx, ry, wx, wy;
	    unsigned int pmask;
	    if (XQueryPointer(st->dpy, psc->root, &pr, &cr,
	  					&rx, &ry, &wx, &wy, &pmask)) {
	  	  if (!(pmask & (Button1Mask | Button2Mask | Button3Mask))) {
	  		  xdnd_proxy_drop(st, CurrentTime);
	  	  } else {
	  		  xdnd_proxy_motion(st, rx, ry, CurrentTime);
	  	  }
	    }
	    return;  // Don't run normal tick logic while proxying
	}

	// XDND proxy: timeout safety for DROPPED state.
	// Browsers legitimately delay XdndFinished until the page has finished
	// processing the drop (e.g. an upload), so this is generous - cleanup
	// is now harmless to a target that's still working (no XdndLeave after
	// drop, selection released only if we still own it).
	{
	    static int drop_ticks = 0;
	    if (st->xdnd_proxy.state == XDND_PROXY_DROPPED) {
	        drop_ticks++;
	        if (drop_ticks > 200) {  // 200 * 50ms = 10 seconds
	            log_error("xdnd: XdndFinished timeout, cleaning up");
	            xdnd_proxy_cleanup(st);
	            drop_ticks = 0;
	        }
	        return;
	    }
	    // Reset whenever we're not in DROPPED, so a session that finished
	    // normally doesn't bequeath its tick count to the next one.
	    drop_ticks = 0;
	}

    // Static tracking for coordinates to prevent X server spam
    static int last_root_x = -1;
    static int last_root_y = -1;

    // 1. Quick check: Is the pointer still on the screen we left it on?
    if (st->pointer_screen >= 0 && st->pointer_screen < st->config.screen_count) {
        Window root_ret, child_ret;
        int root_x, root_y, win_x, win_y;
        unsigned int mask;
        
        if (XQueryPointer(st->dpy, st->screens[st->pointer_screen].root, 
                          &root_ret, &child_ret, &root_x, &root_y, 
                          &win_x, &win_y, &mask)) {
            
            // Only evaluate hover if the mouse physically moved
            if (root_x != last_root_x || root_y != last_root_y) {
                process_hover_cursor(st, child_ret, root_x, root_y);
                last_root_x = root_x;
                last_root_y = root_y;
            }

            // XDND edge detection: raw motion events are suppressed on master
            // devices during the active grab that XDND creates, so we detect
            // edge contact here via polling instead. No momentum required -
            // if you're dragging something to the edge, you mean it.
            if ((mask & (Button1Mask | Button2Mask | Button3Mask)) &&
                st->xdnd_proxy.state == XDND_PROXY_IDLE) {
                Window owner = XGetSelectionOwner(st->dpy, st->xdnd_selection);
                if (owner != None) {
                    ScreenState *sc = &st->screens[st->pointer_screen];
                    XWindowAttributes wa;
                    XGetWindowAttributes(st->dpy, sc->root, &wa);

                    int target_screen = -1;
                    if (root_x <= 0 && sc->config.neighbor_left != -1)
                        target_screen = sc->config.neighbor_left;
                    else if (root_x >= wa.width - 1 && sc->config.neighbor_right != -1)
                        target_screen = sc->config.neighbor_right;
                    else if (root_y <= 0 && sc->config.neighbor_up != -1)
                        target_screen = sc->config.neighbor_up;
                    else if (root_y >= wa.height - 1 && sc->config.neighbor_down != -1)
                        target_screen = sc->config.neighbor_down;

                    if (target_screen != -1) {
                        ScreenState *tgt_sc = &st->screens[target_screen];
                        XWindowAttributes tgt_wa;
                        XGetWindowAttributes(st->dpy, tgt_sc->root, &tgt_wa);

                        int warp_x = root_x;
                        int warp_y = root_y;

                        if (target_screen == sc->config.neighbor_left || target_screen == sc->config.neighbor_right) {
                            float n_src = (float)root_y / wa.height;
                            float p = (n_src - 0.5f) * sc->config.phys_scale + sc->config.phys_offset;
                            float n_tgt = (p - tgt_sc->config.phys_offset) / tgt_sc->config.phys_scale + 0.5f;
                            warp_y = (int)(n_tgt * tgt_wa.height);
                            warp_x = (target_screen == sc->config.neighbor_left) ? (tgt_wa.width - 2) : 1;

                            if (warp_y < 0 || warp_y >= tgt_wa.height) {
                                return;
                            }
                        }

                        if (target_screen == sc->config.neighbor_up || target_screen == sc->config.neighbor_down) {
                            float n_src = (float)root_x / wa.width;
                            float p = (n_src - 0.5f) * sc->config.phys_scale + sc->config.phys_offset;
                            float n_tgt = (p - tgt_sc->config.phys_offset) / tgt_sc->config.phys_scale + 0.5f;
                            warp_x = (int)(n_tgt * tgt_wa.width);
                            warp_y = (target_screen == sc->config.neighbor_up) ? (tgt_wa.height - 2) : 1;

                            if (warp_x < 0 || warp_x >= tgt_wa.width) {
                                return;
                            }
                        }

                        xdnd_proxy_start(st, st->pointer_screen, target_screen,
                                         warp_x, warp_y, CurrentTime);
                        return;
                    }
                }
            }

            // Yes, it's still here. Do nothing.
            return; 
        }
    }

    // 2. If XQueryPointer failed, the pointer escaped! 
    // This happens because handle_raw_motion teleported it, or a keyboard shortcut moved it.
    int current = detect_pointer_screen(st);
    if (current < 0 || current == st->pointer_screen) {
        return; // Safety check
    }

    // 3. Update state
    int old = st->pointer_screen;
    st->pointer_screen = current;

    // 4. Safety Net Focus: Ensure hardware focus follows the pointer.
    // (If handle_raw_motion did the warp, this is redundant but totally safe. 
    // If a Lua keyboard shortcut did the warp, this fixes the focus.
    XSetInputFocus(st->dpy, st->screens[current].root, RevertToPointerRoot, CurrentTime);
    XFlush(st->dpy);

    // 5. Broadcast to AwesomeWM
    log_info("pointer screen changed: %d -> %d", old, current);

    char line[128];
    snprintf(line, sizeof(line), "POINTER_SCREEN old=%d new=%d", old, current);
    broadcast_line(st, line);
    lua_on_pointer_screen_change(st, old, current);
}

static void get_window_class(
    Display *dpy,
    Window w,
    char *buf,
    size_t buf_sz
) {
    if (buf_sz == 0) {
        return;
    }

    XClassHint ch;
    if (XGetClassHint(dpy, w, &ch)) {
        if (ch.res_class) {
            snprintf(buf, buf_sz, "%s", ch.res_class);
        } else {
            snprintf(buf, buf_sz, "");
        }

        if (ch.res_name) {
            XFree(ch.res_name);
        }
        if (ch.res_class) {
            XFree(ch.res_class);
        }
    } else {
        snprintf(buf, buf_sz, "");
    }
}

static void get_window_name(
    Display *dpy,
    Window w,
    char *buf,
    size_t buf_sz
) {
    if (buf_sz == 0) return;
    buf[0] = '\0';

    // Try _NET_WM_NAME first (UTF-8, modern apps)
    Atom net_wm_name = XInternAtom(dpy, "_NET_WM_NAME", False);
    Atom utf8 = XInternAtom(dpy, "UTF8_STRING", False);
    Atom actual_type;
    int actual_format;
    unsigned long nitems, bytes_after;
    unsigned char *prop = NULL;

    if (XGetWindowProperty(dpy, w, net_wm_name, 0, 1024, False, utf8,
                           &actual_type, &actual_format, &nitems,
                           &bytes_after, &prop) == Success) {
        // XGetWindowProperty returns Success even when the property is absent
        // (actual_type == None) and hands back a non-NULL zero-length buffer.
        // Only treat it as a real name if the type matches and it's non-empty;
        // otherwise fall through to the WM_NAME fallback.
        if (prop && actual_type == utf8 && nitems > 0 && prop[0] != '\0') {
            snprintf(buf, buf_sz, "%s", (char *)prop);
            XFree(prop);
            return;
        }
        if (prop) XFree(prop);
    }

    // Fall back to WM_NAME (Latin-1, older apps)
    char *name = NULL;
    if (XFetchName(dpy, w, &name) && name) {
        snprintf(buf, buf_sz, "%s", name);
        XFree(name);
    }
}

static void handle_raw_motion(StellarState *st, double dx, double dy, Time event_time) {
    // Raw motion events are suppressed on master devices during active pointer
    // grabs (including XDND drags), so this function only handles normal
    // (non-grab) pointer movement. XDND edge detection is handled in
    // handle_pointer_tick via XQueryPointer polling instead.

    if (st->pointer_screen < 0 || st->pointer_screen >= st->config.screen_count) return;

    int current = st->pointer_screen;
    ScreenState *sc = &st->screens[current];

    Window root_ret, child_ret;
    int root_x, root_y, win_x, win_y;
    unsigned int mask;

    if (!XQueryPointer(st->dpy, sc->root, &root_ret, &child_ret,
                       &root_x, &root_y, &win_x, &win_y, &mask)) return;

    XWindowAttributes wa;
    XGetWindowAttributes(st->dpy, sc->root, &wa);

    // Accumulate Force (momentum toward screen edge)
    if (win_x <= 0 && dx < 0 && sc->config.neighbor_left != -1) {
        st->edge_force_x += dx;
    } else if (win_x >= wa.width - 1 && dx > 0 && sc->config.neighbor_right != -1) {
        st->edge_force_x += dx;
    } else {
        st->edge_force_x = 0;
    }

    if (win_y <= 0 && dy < 0 && sc->config.neighbor_up != -1) {
        st->edge_force_y += dy;
    } else if (win_y >= wa.height - 1 && dy > 0 && sc->config.neighbor_down != -1) {
        st->edge_force_y += dy;
    } else {
        st->edge_force_y = 0;
    }

    // Threshold check
    int target_screen = -1;
    int warp_x = win_x;
    int warp_y = win_y;

    if (st->edge_force_x <= -EDGE_FORCE_THRESHOLD) target_screen = sc->config.neighbor_left;
    else if (st->edge_force_x >= EDGE_FORCE_THRESHOLD) target_screen = sc->config.neighbor_right;
    else if (st->edge_force_y <= -EDGE_FORCE_THRESHOLD) target_screen = sc->config.neighbor_up;
    else if (st->edge_force_y >= EDGE_FORCE_THRESHOLD) target_screen = sc->config.neighbor_down;

    // Warp pointer to next display
    if (target_screen != -1) {
        ScreenState *tgt_sc = &st->screens[target_screen];
        XWindowAttributes tgt_wa;
        XGetWindowAttributes(st->dpy, tgt_sc->root, &tgt_wa);

        if (target_screen == sc->config.neighbor_left || target_screen == sc->config.neighbor_right) {
            float n_src = (float)win_y / wa.height;
            float p = (n_src - 0.5f) * sc->config.phys_scale + sc->config.phys_offset;
            float n_tgt = (p - tgt_sc->config.phys_offset) / tgt_sc->config.phys_scale + 0.5f;
            warp_y = (int)(n_tgt * tgt_wa.height);

            warp_x = (target_screen == sc->config.neighbor_left) ? (tgt_wa.width - 2) : 1;

            if (warp_y < 0 || warp_y >= tgt_wa.height) {
                st->edge_force_x = 0;
                return;
            }
        }

        // De-animate this screen's cursor (theme + size preserved) before the
        // cross-screen warp, to avoid Xorg animated-cursor sprite-handoff crash.
		reset_cursor_sprite(st, sc, current);

        XWarpPointer(st->dpy, None, tgt_sc->root, 0, 0, 0, 0, warp_x, warp_y);
        XSetInputFocus(st->dpy, tgt_sc->root, RevertToPointerRoot, event_time);
        XFlush(st->dpy);

        st->edge_force_x = 0;
        st->edge_force_y = 0;
    }
}

void send_directed_key_event(StellarState *st, Window target_win, int keycode, bool is_press) {
    XKeyEvent ev;
    memset(&ev, 0, sizeof(ev));

    ev.type = is_press ? KeyPress : KeyRelease;
    ev.display = st->dpy;
    ev.window = target_win;
    ev.root = DefaultRootWindow(st->dpy); // Or the specific screen's root if strictly needed
    ev.subwindow = None;
    ev.time = CurrentTime; 
    
    // Arbitrary coordinates, usually ignored by non-mouse handlers, 
    // but good practice to put something within the window bounds.
    ev.x = 1;
    ev.y = 1;
    ev.x_root = 1;
    ev.y_root = 1;
    ev.same_screen = True;
    
    ev.keycode = keycode;
    ev.state = 0; // TODO: add modifier masks here (e.g., ShiftMask, ControlMask) for global hotkeys being directed to windows (PTT)

    // Send the event directly to the target window
    long event_mask = is_press ? KeyPressMask : KeyReleaseMask;
    
    // The 'True' here is the propagate flag. 
    XSendEvent(st->dpy, target_win, True, event_mask, (XEvent *)&ev);
    XFlush(st->dpy);
}

void force_remanage_with_type(StellarState *st, Window client, Atom type_value) {
    Display *dpy = st->dpy;
    Window root = DefaultRootWindow(dpy);

    // Confirm Awesome is currently managing this window (it's reparented into
    // a frame). If it's already a direct child of root, Awesome hasn't managed
    // it yet and apply_window_rules' own XChangeProperty will be read at the
    // upcoming manage - no re-manage needed.
    Window rroot, parent, *children = NULL;
    unsigned int nchildren = 0;
    if (XQueryTree(dpy, client, &rroot, &parent, &children, &nchildren)) {
        if (children) XFree(children);
        if (parent == root) {
            log_info("force_remanage: 0x%lx not yet managed, setting type in place", client);
            Atom type_atom = XInternAtom(dpy, "_NET_WM_WINDOW_TYPE", False);
            XChangeProperty(dpy, client, type_atom, XA_ATOM, 32,
                            PropModeReplace, (unsigned char *)&type_value, 1);
            XFlush(dpy);
            return;
        }
    }

    log_info("force_remanage: detaching 0x%lx to apply window type", client);

    // Preserve the client across the reparent/unmap so it isn't destroyed if
    // the connection that owns it goes away mid-cycle.
    XAddToSaveSet(dpy, client);

    // Step 1: Reparent to root -> Awesome sees the UnmapNotify on its frame
    // and unmanages the client. Now there is no SubstructureRedirect between
    // us and the window, so our property change below is authoritative.
    XReparentWindow(dpy, client, root, 0, 0);
    XFlush(dpy);
    usleep(100000);  // let Awesome process the unmanage

    // Step 2: Set the window type while unmanaged. This is the value Awesome
    // will read when it re-manages on the next MapRequest.
    Atom type_atom = XInternAtom(dpy, "_NET_WM_WINDOW_TYPE", False);
    XChangeProperty(dpy, client, type_atom, XA_ATOM, 32,
                    PropModeReplace, (unsigned char *)&type_value, 1);
    XFlush(dpy);

    // Step 3: Remap. Awesome intercepts the MapRequest, builds a fresh frame,
    // and manages the client from scratch -- reading the now-correct type at
    // the one moment it ever reads it.
    XMapWindow(dpy, client);
    XFlush(dpy);

    XRemoveFromSaveSet(dpy, client);
}

void reapply_window_rules_to_existing(StellarState *st) {
    Display *dpy = st->dpy;
    Atom wm_state = XInternAtom(dpy, "WM_STATE", True);

    log_info("reapply_window_rules_to_existing: sweeping live windows");
    int total = 0;

    for (int s = 0; s < st->config.screen_count; s++) {
        Window root = st->screens[s].root;
        Window rroot, parent, *children = NULL;
        unsigned int nchildren = 0;

        if (!XQueryTree(dpy, root, &rroot, &parent, &children, &nchildren))
            continue;

        for (unsigned int i = 0; i < nchildren; i++) {
            // The real client may be this child directly (rare under Awesome)
            // or the WM_STATE-bearing window inside an Awesome frame. Resolve
            // to the actual client window either way.
            Window client = None;

            if (wm_state != None) {
                Atom at; int af; unsigned long ni, ba; unsigned char *p = NULL;
                if (XGetWindowProperty(dpy, children[i], wm_state, 0, 0, False,
                                       AnyPropertyType, &at, &af, &ni, &ba, &p) == Success) {
                    if (p) XFree(p);
                    if (at != None) client = children[i];
                }
            }
            if (client == None)
                client = find_client_window(dpy, children[i]);
            if (client == None)
                continue;

            char class_name[256] = {0};
            char win_name[256] = {0};
            get_window_class(dpy, client, class_name, sizeof(class_name));
            get_window_name(dpy, client, win_name, sizeof(win_name));
            if (class_name[0] == '\0' && win_name[0] == '\0')
                continue;

            apply_window_rules_authoritative(st, client, class_name, win_name);
            total++;
        }

        if (children) XFree(children);
    }

    XFlush(dpy);
    log_info("reapply_window_rules_to_existing: updated %d window(s)", total);
}

static void drain_x_events(StellarState *st) {
    while (XPending(st->dpy) > 0) {
        XEvent ev;
        XNextEvent(st->dpy, &ev);

		// --- Handle XInput2 Raw Events ---
        if (ev.type == GenericEvent) {
            XGenericEventCookie *cookie = &ev.xcookie;
            if (XGetEventData(st->dpy, cookie)) {
                if (cookie->extension == st->xi_opcode) {
                    
                    // ANY raw activity resets the timer for the active screen
                    if (cookie->evtype == XI_RawMotion || cookie->evtype == XI_RawKeyPress) {
                        reset_idle_timer(st);
                    }

                    // Specific raw motion handling (mouse warps, edge forces)
                    if (cookie->evtype == XI_RawMotion) {
                        XIRawEvent *re = (XIRawEvent *)cookie->data;

                        double dx = 0.0, dy = 0.0;
                        Time event_time = re->time;

                        double *val = re->valuators.values;
                        if (XIMaskIsSet(re->valuators.mask, 0)) dx = *val++; 
                        if (XIMaskIsSet(re->valuators.mask, 1)) dy = *val++; 
                        
                        handle_raw_motion(st, dx, dy, event_time);
                    }

					if (cookie->evtype == XI_RawKeyPress || cookie->evtype == XI_RawKeyRelease) {
						XIRawEvent *re = (XIRawEvent *)cookie->data;
						int keycode = re->detail;
						bool is_press = (cookie->evtype == XI_RawKeyPress);

						// WIP: global hotkey window rules
						// 1. Check if 'keycode' is in C-side whitelist.
						// 2. If it is, fetch the associated target Window ID.
						// 3. Call send_directed_key_event(st, target_window, keycode, is_press);
					}
                }
                XFreeEventData(st->dpy, cookie);
            }
            continue; // Move to next event
        }

        // --- Handle RandR events (dynamic event numbers) ---
        if (st->rr_event_base > 0) {
            if (ev.type == st->rr_event_base + RRScreenChangeNotify) {
                XRRScreenChangeNotifyEvent *rr = (XRRScreenChangeNotifyEvent *)&ev;
                XRRUpdateConfiguration(&ev);

                int screen = screen_from_root(st, rr->root);
                if (screen >= 0) {
                    log_info("RandR: screen change on screen %d (%dx%d)",
                             screen, rr->width, rr->height);
                    monitor_update_screen_info(st, screen);

                    char line[128];
                    snprintf(line, sizeof(line), "MONITOR_CHANGED screen=%d", screen);
                    broadcast_line(st, line);
                }
                continue;
            }

            if (ev.type == st->rr_event_base + RRNotify) {
                XRRNotifyEvent *rr = (XRRNotifyEvent *)&ev;
                int screen = screen_from_root(st, rr->window);

                if (screen >= 0 && rr->subtype == RRNotify_OutputChange) {
                    XRROutputChangeNotifyEvent *oc = (XRROutputChangeNotifyEvent *)&ev;
                    log_info("RandR: output change on screen %d (output=0x%lx)",
                             screen, oc->output);
                    monitor_update_screen_info(st, screen);

                    char line[128];
                    snprintf(line, sizeof(line), "MONITOR_CHANGED screen=%d", screen);
                    broadcast_line(st, line);
                }
                continue;
            }
        }

        switch (ev.type) {
            case EnterNotify:
            case LeaveNotify:
                handle_pointer_tick(st);
                break;

            case CreateNotify: {
				XCreateWindowEvent *e = &ev.xcreatewindow;
				if (e->override_redirect) break;
				int screen = screen_from_root(st, e->parent);
				if (screen < 0) break;

				char class_name[256] = {0};
				char win_name[256] = {0};
				get_window_class(st->dpy, e->window, class_name, sizeof(class_name));
				get_window_name(st->dpy, e->window, win_name, sizeof(win_name));

				log_info("CreateNotify: win=0x%lx class='%s' name='%s'",
						 e->window, class_name, win_name);

				if (class_name[0]) {
					// Always keep watching until the window maps: some apps
					// (e.g. NoMachine/nxplayer) set an early placeholder name
					// at create time and only later replace it with the real
					// title via _NET_WM_NAME. If we matched once here and gave
					// up, a name-keyed rule would never fire. So we register
					// the window as pending, re-evaluating the rule on every
					// name change (see PropertyNotify), and stop at MapNotify.
					add_pending(e->window, class_name);
					XSelectInput(st->dpy, e->window, PropertyChangeMask);

					// Still attempt an immediate match so windows that already
					// have their final name at create time are handled at once.
					if (win_name[0]) {
						RuleResult rr = apply_window_rules(st, e->window, class_name, win_name);
						if (rr.set_type) {
							add_tracked(e->window, class_name, win_name, rr.type_value);
						}
					} else {
						log_info("Deferring rule match for 0x%lx (no name yet)", e->window);
					}
				}
	
				char line[256];
				snprintf(
					line,
					sizeof(line),
					"XEVENT type=create screen=%d win=0x%lx",
					screen,
					e->window
				);
				broadcast_line(st, line);
				break;
            }

			case PropertyNotify: {
				XPropertyEvent *e = &ev.xproperty;
				Atom net_wm_name = XInternAtom(st->dpy, "_NET_WM_NAME", False);
				Atom net_wm_type = XInternAtom(st->dpy, "_NET_WM_WINDOW_TYPE", False);

				// --- Tracked: Wine overwrote our type? ---
				TrackedWindow *tw = find_tracked(e->window);
				if (tw && e->atom == net_wm_type) {
					// Check if it's already correct (avoid infinite loop)
					Atom actual_type;
					int actual_format;
					unsigned long nitems, bytes_after;
					unsigned char *prop = NULL;

					if (XGetWindowProperty(st->dpy, e->window, net_wm_type, 0, 1, False,
										   XA_ATOM, &actual_type, &actual_format, &nitems,
										   &bytes_after, &prop) == Success && prop && nitems > 0) {
						Atom current = *(Atom *)prop;
						XFree(prop);
						if (current == tw->target_type) break;  // our own change
					} else {
						if (prop) XFree(prop);
					}

					log_info("Type overwritten on 0x%lx, re-applying", e->window);
					Atom type_atom = XInternAtom(st->dpy, "_NET_WM_WINDOW_TYPE", False);
					XChangeProperty(st->dpy, e->window, type_atom, XA_ATOM, 32,
									PropModeReplace, (unsigned char *)&tw->target_type, 1);
					XFlush(st->dpy);
					break;
				}

				// --- Name change: (re)evaluate the rule ---
				// Apps may set their matchable title at several points:
				//  - before Awesome manages (placeholder -> real title), or
				//  - AFTER Awesome has already managed the window (NoMachine).
				// We must handle both. A window is a candidate as long as it's
				// either still pending (not yet managed) or already tracked
				// (rule matched once, type being defended).
				if (e->atom != XA_WM_NAME && e->atom != net_wm_name) break;

				PendingWindow *pw = find_pending(e->window);
				TrackedWindow *twn = find_tracked(e->window);
				if (!pw && !twn) break;

				char win_name[256] = {0};
				get_window_name(st->dpy, e->window, win_name, sizeof(win_name));
				if (win_name[0] == '\0') break;

				const char *cls = pw ? pw->class_name : twn->class_name;
				log_info("Name changed for 0x%lx: '%s' (re-evaluating)", e->window, win_name);
				RuleResult rr = apply_window_rules(st, e->window, cls, win_name);

				if (rr.set_type) {
					// Ensure there's a tracked entry (idempotent) so we can
					// (a) defend the type against later overwrites and
					// (b) remember whether we've already forced a re-manage.
					add_tracked(e->window, cls, win_name, rr.type_value);
					TrackedWindow *t = find_tracked(e->window);

					// If the type only just became correct but Awesome has
					// already managed this window, the manage-time read saw the
					// old type. Force a single re-manage so Awesome re-reads the
					// correct type. force_remanage_with_type is a no-op (just
					// sets the property in place) if the window isn't yet
					// managed, so calling it is always safe.
					if (t && !t->remanaged) {
						t->remanaged = true;
						force_remanage_with_type(st, e->window, rr.type_value);
						// The re-manage triggers a fresh manage cycle; the new
						// tracked entry persists across it (we keyed on the
						// client window, which is unchanged by reparenting).
					}
				}
				// Do NOT remove_pending or drop the event mask here: a later
				// name change might newly match a rule. Cleanup happens when
				// the window is destroyed.
				break;
			}

            case DestroyNotify: {
				// Clean up pending list if window dies
				remove_pending(ev.xdestroywindow.window);
				remove_tracked(ev.xdestroywindow.window);

                XDestroyWindowEvent *e = &ev.xdestroywindow;
                int screen = screen_from_root(st, e->event);
                if (screen >= 0) {
                    char line[256];
                    snprintf(
                        line,
                        sizeof(line),
                        "XEVENT type=destroy screen=%d win=0x%lx",
                        screen,
                        e->window
                    );
                    broadcast_line(st, line);
                }
                break;
            }

            case MapNotify: {
                XMapEvent *e = &ev.xmap;
                // FILTER: Ignore internal/transient windows
                if (e->override_redirect) break; 

				// Clean up pending (gave up waiting for name)
				remove_pending(e->window);

				// Tracked windows: normally we stop defending the type once the
				// window maps. But a window we had to RE-MANAGE (NoMachine-style:
				// matchable title arrived after Awesome's first manage) must keep
				// its tracking past this map -- this MapNotify is the re-manage's
				// own map. Tearing down here would (a) drop the type-overwrite
				// defense and (b) lose the one-shot re-manage guard, risking a
				// re-manage loop. Keep it; DestroyNotify does the final cleanup.
				TrackedWindow *tw = find_tracked(e->window);
				if (tw && !tw->remanaged) {
					XSelectInput(st->dpy, e->window, NoEventMask);
					remove_tracked(e->window);
				}

                int screen = screen_from_root(st, e->event);
                if (screen >= 0) {
                    char class_name[256];
                    get_window_class(
                        st->dpy,
                        e->window,
                        class_name,
                        sizeof(class_name)
                    );

                    char line[512];
                    snprintf(
                        line,
                        sizeof(line),
                        "XEVENT type=map screen=%d win=0x%lx class=%s",
                        screen,
                        e->window,
                        class_name[0] ? class_name : "unknown"
                    );
                    broadcast_line(st, line);
                }
                break;
            }

            case UnmapNotify: {
                XUnmapEvent *e = &ev.xunmap;
                int screen = screen_from_root(st, e->event);
                if (screen >= 0) {
                    char line[256];
                    snprintf(
                        line,
                        sizeof(line),
                        "XEVENT type=unmap screen=%d win=0x%lx",
                        screen,
                        e->window
                    );
                    broadcast_line(st, line);
                }
                break;
            }

			case SelectionRequest: {
				// Always route: the handler replies with a refusal when it
				// has nothing to serve, which keeps requestors from hanging
				// on a conversion that will never be answered.
				xdnd_proxy_handle_selection_request(st, &ev.xselectionrequest);
				break;
			}

			case ClientMessage: {
				// Always route: drop-shield traffic from the original drag
				// source (XdndPosition/XdndDrop addressed to a shield window)
				// can arrive after the proxy state has returned to IDLE, and
				// it must still be answered or the source hangs waiting for
				// XdndFinished.
				xdnd_proxy_handle_client_message(st, &ev.xclient);
				break;
			}

			case SelectionNotify:
				// Consumed by fetch_xdnd_data's sync loop; ignore stray ones
				break;

            default:
                break;
        }
    }
}

/* ---------- Settings ---------- */

static void pre_parse_shell_setting(StellarState *st) {
    char path[PATH_MAX];
    snprintf(path, sizeof(path), "%s/.config/stellar/settings.json",
             getenv("HOME"));

    FILE *f = fopen(path, "r");
    if (!f) goto fallback;

    char line[1024];
    bool in_terminal = false;
    while (fgets(line, sizeof(line), f)) {
        if (strstr(line, "\"terminal\"")) in_terminal = true;
        if (in_terminal && strstr(line, "\"shell\"")) {
            // extract value: "shell": "/bin/zsh"
            char *colon = strchr(line, ':');
            if (colon) {
                char *q1 = strchr(colon, '"');
                if (q1) {
                    q1++;
                    char *q2 = strchr(q1, '"');
                    if (q2) {
                        size_t len = q2 - q1;
                        if (len < sizeof(st->config.term_shell)) {
                            memcpy(st->config.term_shell, q1, len);
                            st->config.term_shell[len] = '\0';
                            fclose(f);
                            log_info("pre-parsed shell: %s", st->config.term_shell);
                            return;
                        }
                    }
                }
            }
        }
    }
    fclose(f);

fallback: {
        const char *env_shell = getenv("SHELL");
        if (env_shell && env_shell[0] != '\0') {
            snprintf(st->config.term_shell, sizeof(st->config.term_shell), "%s", env_shell);
        } else {
            snprintf(st->config.term_shell, sizeof(st->config.term_shell), "/bin/bash");
        }
    }
}

static void apply_settings_defaults(StellarState *st) {
    snprintf(st->config.stellar_theme, sizeof(st->config.stellar_theme), "stellar-blue");

	st->config.saver_enabled = true;

    snprintf(st->config.appearance.font_name,
             sizeof(st->config.appearance.font_name), "sans-serif");
	st->config.appearance.font_size = 11.0f;
	snprintf(st->config.appearance.font_unit,
			 sizeof(st->config.appearance.font_unit), "pt");

	for (int i = 0; i < st->config.screen_count; i++) {
		ScreenState *sc = &st->screens[i];

		sc->config.picom_enabled = true;
		sc->config.tray_enabled = true;
	}
}

void generate_stellar_xresources(StellarState *st) {
	char file_path[PATH_MAX];
	snprintf(file_path, sizeof(file_path), "%s/.cache/stellar/Xresources", get_user_home_dir());

	FILE *f = fopen(file_path, "w");
	if (f) {
		fprintf(f, "! Automatically generated by Stellar\n");
		fprintf(f, "! Do not edit - changes will be lost\n");

		for (int idx = 0; idx < st->config.screen_count; idx++) {
			ScreenState *sc = &st->screens[idx];
				fprintf(f, "\n");
				fprintf(f, "#if SCREEN_NUM == %d\n", idx);
				fprintf(f, "Xcursor.theme: %s\n", get_cursor_theme_for_screen(st, idx));
				fprintf(f, "Xcursor.size: %d\n", get_cursor_size_for_screen(st, idx));
				fprintf(f, "Xft.dpi: %d\n", sc->dpi);
				fprintf(f, "#endif\n");
		}
		fclose(f);
		log_info("Generated cached Xresources", file_path);
	} else {
		log_error("Failed to write generated Xresources: %s", strerror(errno));
	}
}

void apply_stellar_xsettings(StellarState *st) {
	char dir_path[PATH_MAX];
	snprintf(dir_path, sizeof(dir_path), "%s/.cache/stellar/xsettingsd", get_user_home_dir());

    for (int idx = 0; idx < st->config.screen_count; idx++) {
    	ScreenState *sc = &st->screens[idx];
		
		char file_path[PATH_MAX];
		snprintf(file_path, sizeof(file_path), "%s/screen_%d.conf", dir_path, idx);

		FILE *f = fopen(file_path, "w");
		if (f) {
			fprintf(f, "# Automatically generated by Stellar\n");
			fprintf(f, "# Do not edit - changes will be lost\n");
			fprintf(f, "\n");
			fprintf(f, "Gtk/CursorThemeName \"%s\"\n", get_cursor_theme_for_screen(st, idx));
			fprintf(f, "Gtk/CursorThemeSize %d\n", get_cursor_size_for_screen(st, idx));
			fprintf(f, "Net/ThemeName \"Adwaita\"\n"); 
			fprintf(f, "Net/IconThemeName \"hicolor\"\n");
			
			fclose(f);
			log_info("Generated isolated xsettingsd conf at %s", file_path);
		} else {
			log_error("Failed to write stellar xsettingsd conf: %s", strerror(errno));
		}

		if (sc->xsettingsd_pid > 0) {
			kill(sc->xsettingsd_pid, SIGHUP);
            log_info("Sent SIGHUP to xsettingsd on screen %d for live reload", idx);
		} 
	}
}

static void apply_compositor_settings(StellarState *st) {
	char dir_path[PATH_MAX];
	snprintf(dir_path, sizeof(dir_path), "%s/.cache/stellar/picom", get_user_home_dir());

    for (int idx = 0; idx < st->config.screen_count; idx++) {
    	ScreenState *sc = &st->screens[idx];
		
		char file_path[PATH_MAX];
		snprintf(file_path, sizeof(file_path), "%s/screen_%d.conf", dir_path, idx);

		FILE *f = fopen(file_path, "w");
		if (f) {
			fprintf(f, "# Automatically generated by Stellar\n");
			fprintf(f, "# Do not edit - changes will be lost\n");
			fprintf(f, "\n");
			fprintf(f, "Gtk/CursorThemeName \"%s\"\n", get_cursor_theme_for_screen(st, idx));
			fprintf(f, "Gtk/CursorThemeSize %d\n", get_cursor_size_for_screen(st, idx));
			fprintf(f, "Net/ThemeName \"Adwaita\"\n"); 
			fprintf(f, "Net/IconThemeName \"hicolor\"\n");
			
			fclose(f);
			log_info("Generated isolated xsettingsd conf at %s", file_path);
		} else {
			log_error("Failed to write stellar xsettingsd conf: %s", strerror(errno));
		}

		if (sc->xsettingsd_pid > 0) {
			kill(sc->xsettingsd_pid, SIGHUP);
            log_info("Sent SIGHUP to xsettingsd on screen %d for live reload", idx);
		} 
	}
}

/* ---------- Child Process Management ---------- */

static bool can_respawn_screen(ScreenState *sc) {
    time_t now = time(NULL);

    if (
        sc->respawn_window_start == 0 ||
        now - sc->respawn_window_start > AWESOME_RESPAWN_WINDOW_SEC
    ) {
        sc->respawn_window_start = now;
        sc->respawn_count = 0;
    }

    if (sc->respawn_count >= AWESOME_RESPAWN_LIMIT) {
        return false;
    }

    sc->respawn_count++;
    return true;
}

static pid_t spawn_xsettingsd_for_screen(StellarState *st, int screen_num) {
    ScreenState *sc = &st->screens[screen_num];

    pid_t pid = fork();
    if (pid < 0) {
        log_error("fork failed for xsettingsd screen=%d: %s", screen_num, strerror(errno));
        return -1;
    }

    if (pid == 0) {
        setenv("DISPLAY", sc->display_name, 1);
		char screen_str[8];
		snprintf(screen_str, sizeof(screen_str), "%d", screen_num);

        char conf_path[PATH_MAX];
        snprintf(conf_path, sizeof(conf_path), "%s/.cache/stellar/xsettingsd/screen_%d.conf", get_user_home_dir(), screen_num);

		execlp("xsettingsd", "xsettingsd", "-s", screen_str, "-c", conf_path, (char *)NULL); 
        
        fprintf(stderr, "[stellar] ERROR: exec xsettingsd (screen %d) failed: %s\n", screen_num, strerror(errno));
        _exit(127);
    }

    sc->xsettingsd_pid = pid;

    log_info(
        "spawned xsettingsd screen=%d pid=%d display=%s",
        screen_num,
        pid,
        sc->display_name
    );

    return pid;
}

static void spawn_all_xsettingsd(StellarState *st) {
    for (int idx = 0; idx < st->config.screen_count; idx++) {
        spawn_xsettingsd_for_screen(st, idx);
    }
}

static pid_t spawn_awesome_for_screen(
    StellarState *st,
    int screen_num
) {
    ScreenState *sc = &st->screens[screen_num];

    pid_t pid = fork();
    if (pid < 0) {
        log_error("fork failed for awesome screen=%d: %s",
            screen_num,
            strerror(errno));
        return -1;
    }

    if (pid == 0) {
        char dpi_str[32];
        char scale_str[32];
        char screen_str[16];
        char cursor_size_str[16];
		char num_screens_str[16];

		snprintf(screen_str, sizeof(screen_str), "%d", screen_num);
		snprintf(num_screens_str, sizeof(num_screens_str), "%d", st->config.screen_count);

        setenv("DISPLAY", sc->display_name, 1);
        setenv("STELLAR_SCREEN", screen_str, 1);
		setenv("STELLAR_NUM_SCREENS", num_screens_str, 1);

        snprintf(scale_str, sizeof(scale_str), "%.2f", sc->scale);
        setenv("QT_SCALE_FACTOR", scale_str, 1);
        setenv("GDK_DPI_SCALE", scale_str, 1);
        setenv("GDK_SCALE", "1", 1);

        snprintf(dpi_str, sizeof(dpi_str), "%d", sc->dpi);
        setenv("STELLAR_DPI", dpi_str, 1);
        setenv("STELLAR_UI_SCALE", scale_str, 1);

        execlp("awesome", "awesome", "-c", st->awesome_conf_path, (char *)NULL);

        fprintf(
            stderr,
            "[stellar] ERROR: exec awesome failed for screen %d: %s\n",
            screen_num,
            strerror(errno)
        );
        _exit(127);
    }

    sc->awesome_pid = pid;
    sc->awesome_fd = -1;

    log_info(
        "launched awesome screen=%d pid=%d display=%s scale=%.2f",
        screen_num,
        pid,
        sc->display_name,
        sc->scale
    );

    return pid;
}

static void launch_all_awesome(StellarState *st) {
    for (int i = 0; i < st->config.screen_count; i++) {
        spawn_awesome_for_screen(st, i);
    }
}

static void restart_awesome(StellarState *st, int screen_num) {
    ScreenState *sc = &st->screens[screen_num];

    if (!can_respawn_screen(sc)) {
        log_error(
            "awesome screen=%d crashed too often; not restarting right now",
            screen_num
        );
        return;
    }

    sleep(1);
    spawn_awesome_for_screen(st, screen_num);
}

// Block until stalonetray has claimed _NET_SYSTEM_TRAY_S<n> on every
// enabled screen, or until timeout_ms elapses.  Without this, autostart
// apps race against the tray: they fall back from SNI to XEmbed, find no
// selection owner, and map their icon windows on the root at (0,0).
static void wait_for_tray_ready(StellarState *st, int timeout_ms) {
    bool need[MAX_SCREENS] = {false};
    int remaining = 0;

    for (int i = 0; i < st->config.screen_count; i++) {
        if (st->screens[i].config.tray_enabled) {
            need[i] = true;
            remaining++;
        }
    }

    if (remaining == 0)
        return;

    // Intern the selection atoms once
    Atom sel[MAX_SCREENS];
    for (int i = 0; i < st->config.screen_count; i++) {
        if (!need[i]) continue;
        char name[64];
        snprintf(name, sizeof(name), "_NET_SYSTEM_TRAY_S%d", i);
        sel[i] = XInternAtom(st->dpy, name, False);
    }

    int elapsed = 0;
    int poll_interval_us = 25000;  // 25 ms

    while (remaining > 0 && elapsed < timeout_ms * 1000) {
        for (int i = 0; i < st->config.screen_count; i++) {
            if (!need[i]) continue;
            Window owner = XGetSelectionOwner(st->dpy, sel[i]);
            if (owner != None) {
                log_info("tray selection _NET_SYSTEM_TRAY_S%d claimed "
                         "(waited ~%d ms)", i, elapsed / 1000);
                need[i] = false;
                remaining--;
            }
        }
        if (remaining > 0) {
            usleep(poll_interval_us);
            elapsed += poll_interval_us;
        }
    }

    if (remaining > 0) {
        for (int i = 0; i < st->config.screen_count; i++) {
            if (need[i])
                log_error("tray selection _NET_SYSTEM_TRAY_S%d not claimed "
                          "after %d ms - autostart may race", i, timeout_ms);
        }
    }
}

void apply_system_daemons(StellarState *st) {
    // Update Compositor and Tray State
    for (int i = 0; i < st->config.screen_count; i++) {
        ScreenState *sc = &st->screens[i];

        if (sc->config.picom_enabled) {
            start_compositor(st, i);
        } else {
            stop_compositor(st, i);
        }

		if (sc->config.tray_enabled) {
			start_tray(st, i);
		} else {
			stop_tray(st, i);
		} 
    }
    
	// Update SNI tray bridge state
    start_snitray(st);

    // Update Stellar-saver State
    if (st->config.saver_enabled) {
        start_stellar_saver(st);
    } else {
        stop_stellar_saver(st);
    } 
}

static void reap_children(StellarState *st) {
    int status = 0;
    pid_t pid;

    while ((pid = waitpid(-1, &status, WNOHANG)) > 0) {
        bool matched = false;

        for (int i = 0; i < st->config.screen_count; i++) {
            ScreenState *sc = &st->screens[i];
            
            // --- Check AwesomeWM ---
            if (sc->awesome_pid == pid) {
                matched = true;
                sc->awesome_pid = 0; 
                sc->awesome_fd = -1;

                if (WIFEXITED(status)) {
                    log_error("awesome screen=%d exited pid=%d code=%d", i, pid, WEXITSTATUS(status));
                } else if (WIFSIGNALED(status)) {
                    log_error("awesome screen=%d died pid=%d signal=%d", i, pid, WTERMSIG(status));
                } else {
                    log_error("awesome screen=%d ended pid=%d", i, pid);
                }

                // ONLY restart if we aren't shutting down
                if (g_running && !st->stellar_shutdown) {
                    restart_awesome(st, i);
                }
                break;
            }

            // --- Check Compositor ---
            if (sc->picom_pid == pid) {
                matched = true;
                sc->picom_pid = -1;
                log_info("Picom on screen %d exited (PID %d)", i, pid);
                
                if (sc->config.picom_enabled && g_running && !st->stellar_shutdown) {
                    start_compositor(st, i);
                }
                break;
            }

            // --- Check Tray ---
            if (sc->tray_pid == pid) {
                matched = true;
                sc->tray_pid = -1;
                log_info("Stalonetray on screen %d exited (PID %d)", i, pid);
                
                if (sc->config.tray_enabled && g_running && !st->stellar_shutdown) {
                    start_tray(st, i);
                }
                break;
            }

            // --- Check xsettingsd ---
            if (sc->xsettingsd_pid == pid) {
                matched = true;
                sc->xsettingsd_pid = -1;
                log_info("Xsettingsd on screen %d exited (PID %d)", i, pid);
                break;
            }
        }
        
        // --- Check Stellar-saver ---
        if (st->saver_pid == pid) {
            matched = true;
            st->saver_pid = -1;
            log_info("Stellar-saver exited (PID %d)", pid);
            
            if (st->config.saver_enabled && g_running && !st->stellar_shutdown) {
                start_stellar_saver(st);
            }
            continue;
        }

		// --- Check SNI tray bridge ---
		if (st->snitray_pid == pid) {
			matched = true;
			st->snitray_pid = -1;
			log_info("SNI tray bridge exited (PID %d)", pid);

			if (g_running && !st->stellar_shutdown) {
				start_snitray(st);
			}
			continue;
		}

        if (!matched) {
            log_info("reaped unrelated child pid=%d", pid);
        }
    }
}

/* ---------- Session Helpers ---------- */

static void setup_session_env(StellarState *st) {
    setenv("XDG_CURRENT_DESKTOP", "Stellar", 1);
    setenv("DESKTOP_SESSION", "stellar", 1);
    unsetenv("XCURSOR_THEME");
    unsetenv("XCURSOR_SIZE");

	char share_path[PATH_MAX];
	snprintf(share_path, sizeof(share_path), "%s", STELLAR_SHARE_PATH);

	setenv("STELLAR_SOCKET", st->socket_path, 1);
	setenv("STELLAR_SHARE_PATH", share_path, 1);
	setenv("STELLAR_AWESOME_BASE", st->awesome_base_conf_path, 1);
	setenv("STELLAR_SHELL", st->config.term_shell, 1);

    char share_base[PATH_MAX];
    snprintf(share_base, sizeof(share_base), "%s", STELLAR_SHARE_PATH);
    char *last_slash = strrchr(share_base, '/');
    if (last_slash && strcmp(last_slash, "/stellar") == 0) {
        *last_slash = '\0'; // Extract "/usr/local/share" from "/usr/local/share/stellar"
    }

    const char *data_dirs = getenv("XDG_DATA_DIRS");
    char new_dirs[4096];
    if (!data_dirs || data_dirs[0] == '\0') {
        snprintf(new_dirs, sizeof(new_dirs), "%s:/usr/share", share_base);
        setenv("XDG_DATA_DIRS", new_dirs, 1);
    } else if (!strstr(data_dirs, share_base)) {
        snprintf(new_dirs, sizeof(new_dirs), "%s:%s", share_base, data_dirs);
        setenv("XDG_DATA_DIRS", new_dirs, 1);
    }
}

static void generate_portal_config(void) {
    const char *home = get_user_home_dir();
    if (!home) return;

    char dir_path[PATH_MAX];
    snprintf(dir_path, sizeof(dir_path), "%s/.config/xdg-desktop-portal", home);
    
    // Ensure the directory exists
    if (mkdir(dir_path, 0755) != 0 && errno != EEXIST) {
        log_error("Failed to create %s: %s", dir_path, strerror(errno));
    }

    char conf_path[PATH_MAX];
    char tmp_path[PATH_MAX];
    snprintf(conf_path, sizeof(conf_path), "%s/stellar-portals.conf", dir_path);
    snprintf(tmp_path, sizeof(tmp_path), "%s/stellar-portals.conf.tmp", dir_path);

    // 1. Write to a temporary file first
    FILE *f = fopen(tmp_path, "w");
    if (f) {
        fprintf(f, "# Automatically generated by Stellar\n");
        fprintf(f, "[preferred]\n");
        fprintf(f, "default=stellar;gtk;\n");
        
        // 2. Force the standard library to flush to the OS
        fflush(f);
        
        // 3. Force the OS to flush to the physical disk
        int fd = fileno(f);
        if (fd >= 0) fsync(fd);
        
        fclose(f);

        // 4. Atomically swap the temp file over the real file
        if (rename(tmp_path, conf_path) == 0) {
            log_info("Generated XDG portal routing config at %s", conf_path);
        } else {
            log_error("Failed to rename portal config into place: %s", strerror(errno));
        }
    } else {
        log_error("Failed to write temporary portal config: %s", strerror(errno));
    }
}

static bool resolve_config(char *out_path, size_t out_size, const char *rel_path, const char *name) {
    const char *home = get_user_home_dir();

    // Safety check in case home is completely unresolvable
    if (!home || home[0] == '\0') {
        log_error("could not determine home directory for %s config", name);
        return false; 
    }

    char candidate[PATH_MAX];

    // 1. User stellar config: ~/.config/stellar/{rel_path}
    snprintf(candidate, sizeof(candidate), "%s/.config/stellar/%s", home, rel_path);
    if (access(candidate, R_OK) == 0) {
        snprintf(out_path, out_size, "%s", candidate);
        log_info("using user stellar %s config: %s", name, out_path);
        return true;
    }

    // 2. System stellar config: {STELLAR_SHARE_PATH}/{rel_path}
    snprintf(candidate, sizeof(candidate), "%s/%s", STELLAR_SHARE_PATH, rel_path);
    if (access(candidate, R_OK) == 0) {
        snprintf(out_path, out_size, "%s", candidate);
        log_info("using system stellar %s config: %s", name, out_path);
        return true;
    }

    // 3. User standard config: ~/.config/{rel_path}
    snprintf(candidate, sizeof(candidate), "%s/.config/%s", home, rel_path);
    if (access(candidate, R_OK) == 0) {
        snprintf(out_path, out_size, "%s", candidate);
        log_info("using user %s config: %s", name, out_path);
        return true;
    }

    log_error("could not find a %s config", name);
    return false;
}

static bool resolve_awesome_base_conf(StellarState *st) {
    return resolve_config(
        st->awesome_base_conf_path,
        sizeof(st->awesome_base_conf_path),
        "awesome/rc.lua",
        "awesome"
    );
}

static bool resolve_awesome_wrapper(StellarState *st) {
    snprintf(
        st->awesome_conf_path,
        sizeof(st->awesome_conf_path),
        "%s/awesome/stellar_bridge.lua",
        STELLAR_SHARE_PATH
    );

    if (access(st->awesome_conf_path, R_OK) != 0) {
        log_error(
            "stellar bridge config not readable: %s",
            st->awesome_conf_path
        );
        return false;
    }

    log_info("using stellar awesome bridge: %s", st->awesome_conf_path);
    log_info("base awesome config: %s", st->awesome_base_conf_path);

    return true;
}

/* ---------- Cleanup ---------- */

static void shutdown_awesome(StellarState *st) {
    for (int i = 0; i < st->config.screen_count; i++) {
        if (st->screens[i].awesome_pid > 0) {
            kill(st->screens[i].awesome_pid, SIGTERM);
        }
    }

    for (int i = 0; i < st->config.screen_count; i++) {
        if (st->screens[i].awesome_pid > 0) {
            waitpid(st->screens[i].awesome_pid, NULL, 0);
            st->screens[i].awesome_pid = 0;
        }
    }
}

static void cleanup(StellarState *st) {
    if (!st) {
        return;
    }

	menu_watch_cleanup();
    shutdown_awesome(st);

    for (int i = st->client_count - 1; i >= 0; i--) {
        remove_client(st, i);
    }

    if (st->server_fd >= 0) {
        close(st->server_fd);
        st->server_fd = -1;
    }

    if (st->socket_path[0]) {
        unlink(st->socket_path);
    }

    if (st->snitray_pid > 0) {
        kill(st->snitray_pid, SIGTERM);
        st->snitray_pid = 0;
    }

    if (st->dpy) {
        XCloseDisplay(st->dpy);
        st->dpy = NULL;
    }

    if (st->L) {
        lua_close(st->L);
        st->L = NULL;
    }

	if (st->dbus_started_by_stellar && st->dbus_session_pid > 0) {
		kill(st->dbus_session_pid, SIGTERM);
		st->dbus_session_pid = 0;
	}
}

/* ---------- Main ---------- */

int main(void) {
    StellarState st;
    memset(&st, 0, sizeof(st));
    st.server_fd = -1;
    st.xfd = -1;
    st.pointer_screen = -1;

    g_state = &st;

    setvbuf(stdout, NULL, _IONBF, 0);
    setvbuf(stderr, NULL, _IONBF, 0);

    log_info("starting Stellar");

    signal(SIGCHLD, on_sigchld);
    signal(SIGINT, on_terminate);
    signal(SIGTERM, on_terminate);
    signal(SIGHUP, on_terminate);
	signal(SIGPIPE, SIG_IGN); 
    
	if (!ensure_dbus_session(&st)) {
		log_error("could not establish a D-Bus session bus; continuing");
	}

    if (!resolve_awesome_base_conf(&st)) {
        cleanup(&st);
        return 1;
    }

    if (!resolve_awesome_wrapper(&st)) {
        cleanup(&st);
        return 1;
    }

    if (!check_lua_module_available("socket.unix")) {
        log_error(
            "Stellar requires lua-socket support for 'socket.unix'. "
            "Please install lua-socket and try again."
        );
        cleanup(&st);
        return 1;
    }

    if (!init_lua(&st)) {
        cleanup(&st);
        return 1;
    }

    st.server_fd = make_server_socket(&st);
    if (st.server_fd < 0) {
        cleanup(&st);
        return 1;
    }

    pre_parse_shell_setting(&st);
	setup_session_env(&st);

    if (!init_x(&st)) {
        cleanup(&st);
        return 1;
    }

	init_stellar_cache_dirs(&st);
	generate_portal_config();	
	sync_dbus_environment();

    lua_call_noargs(&st, "init");

	apply_settings_defaults(&st);
	parse_settings_from_json(&st);

	// Transparent bitmap font support: make sure every .bdf/.pcf on the
	// system has an up-to-date .otb installed before anything (AwesomeWM,
	// support apps) tries to match fonts by name.  No-op when fresh.
	if (stellar_font_sync_bitmap_cache(false) < 0) {
		log_error("bitmap font cache sync failed (is fonttosfnt installed?)");
	}

	// Build the application menu (scan .desktop files, categorize, cache to
	// ~/.cache/stellar/menu.json). Done here so first-login cost is visible
	// under the session splash; subsequent logins reuse the on-disk cache via
	// the same code path. The menu is pushed to each Awesome instance during
	// its ready-for-sync handshake (see register_awesome_client), so it does
	// NOT need to be broadcast here - Awesome may not be connected yet.
	if (build_application_menu(&st) != 0) {
		log_error("application menu build failed");
	}

	monitor_update_all_screens(&st);
	monitor_apply_all_rotations(&st);
	apply_dpi_settings(&st);

	generate_stellar_xresources(&st);
    merge_session_xresources();
	publish_screen_resource_managers(&st);

	apply_stellar_xsettings(&st);
    spawn_all_xsettingsd(&st);

	load_window_rules(&st);
    launch_all_awesome(&st);

	update_stellar_picom_config(&st);
	apply_system_daemons(&st);

	// Wait for the tray selection to be claimed before launching autostart apps
	wait_for_tray_ready(&st, 3000);
	run_xdg_autostart(&st);

	// Start watching the XDG applications directories so the menu updates live
	// when packages are installed/removed or a .desktop file is added by hand.
	// Non-fatal if it fails - the menu just won't auto-refresh.
	menu_watch_init(&st);

	log_info("running with %d screens", st.config.screen_count);

    while (g_running && !st.stellar_shutdown) {
        fd_set rfds;
        FD_ZERO(&rfds);

        int maxfd = -1;

        FD_SET(st.server_fd, &rfds);
        if (st.server_fd > maxfd) {
            maxfd = st.server_fd;
        }

        FD_SET(st.xfd, &rfds);
        if (st.xfd > maxfd) {
            maxfd = st.xfd;
        }

        int mwfd = menu_watch_fd();
        if (mwfd >= 0) {
            FD_SET(mwfd, &rfds);
            if (mwfd > maxfd) {
                maxfd = mwfd;
            }
        }

        for (int i = 0; i < st.client_count; i++) {
            FD_SET(st.clients[i].fd, &rfds);
            if (st.clients[i].fd > maxfd) {
                maxfd = st.clients[i].fd;
            }
        }

        struct timeval tv;
        tv.tv_sec = 0;
        tv.tv_usec = POLL_US;

        int rv = select(maxfd + 1, &rfds, NULL, NULL, &tv);
        if (rv < 0) {
            if (errno == EINTR) {
                goto after_select;
            }

            log_error("select failed: %s", strerror(errno));
            break;
        }

        if (FD_ISSET(st.server_fd, &rfds)) {
            accept_client(&st);
        }

        if (FD_ISSET(st.xfd, &rfds)) {
            drain_x_events(&st);
        }

        if (mwfd >= 0 && FD_ISSET(mwfd, &rfds)) {
            menu_watch_handle_readable(&st);
        }

        for (int i = 0; i < st.client_count; i++) {
            if (FD_ISSET(st.clients[i].fd, &rfds)) {
                consume_client_data(&st, i);
                if (i >= st.client_count) {
                    break;
                }
            }
        }

    after_select:
        if (g_sigchld) {
            g_sigchld = 0;
            reap_children(&st);
        }

        handle_pointer_tick(&st);
        check_idle_screens(&st);
		menu_watch_tick(&st);
    }

    log_info("shutting down");
    cleanup(&st);
    return 0;
}
