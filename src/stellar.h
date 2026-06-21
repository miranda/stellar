// stellar.h
// DE-only header. Includes stellar_config.h for the shared configuration structs,
// then defines runtime state structs that embed them.
#ifndef STELLAR_H
#define STELLAR_H

#include <errno.h>
#include <limits.h>
#include <ctype.h>
#include <lua.h>
#include <lauxlib.h>
#include <lualib.h>
#include <pwd.h>
#include <signal.h>
#include <spawn.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <stdarg.h>
#include <time.h>
#include <math.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <sys/types.h>
#include <sys/un.h>
#include <sys/wait.h>
#include <unistd.h>
#include <X11/Xlib.h>
#include <X11/Xatom.h>
#include <X11/Xutil.h>
#include <X11/Xcursor/Xcursor.h>
#include <X11/cursorfont.h>
#include <X11/extensions/XInput2.h>
#include <X11/extensions/Xrandr.h>
#include <X11/extensions/dpms.h>

#include "stellar_config.h"

#define MAX_CLIENTS 32
#define POLL_US 50000
#define AWESOME_RESPAWN_LIMIT 5
#define AWESOME_RESPAWN_WINDOW_SEC 30
#define MAX_PENDING_WINDOWS 32
#define MAX_TRACKED_WINDOWS 16

#define EDGE_FORCE_THRESHOLD 30.0

#define STELLAR_LOG_PATH "/tmp/stellar.log"
#define STELLAR_LOG_OLD_PATH "/tmp/stellar.log.old"

extern char **environ;

/* ---------- DE-Only Structs & Enums ---------- */

typedef struct {
    int fd;
    bool is_awesome;
    int screen_num;
    pid_t pid;
    char input_buf[4096];
    size_t input_len;
} IpcClient;

typedef struct {
    char **items;
    size_t count;
    size_t cap;
} StringSet;

typedef struct {
    Window win;
    char class_name[256];
} PendingWindow;

typedef struct {
    Window win;
    char class_name[256];
    char win_name[256];
    Atom target_type;
    bool remanaged;   // true once we've forced a re-manage to apply the type
} TrackedWindow;

typedef struct {
    bool set_type;
    Atom type_value;
} RuleResult;

typedef enum {
    POWER_STATE_ON = 0,
    POWER_STATE_SAVER,
    POWER_STATE_OFF
} ScreenPowerState;

typedef enum {
    XDND_PROXY_IDLE,
    XDND_PROXY_FETCHING,
    XDND_PROXY_DRAGGING,
    XDND_PROXY_DROPPED
} XdndProxyState;

typedef struct {
    XdndProxyState state;
    int            target_screen;
    int            source_screen;
    int            shield_screen;   // screen whose drop-shield is mapped, -1 = none
    Window         current_target;
    unsigned char *uri_data;
    unsigned long  uri_len;
    Time           drag_time;
    Window         source_owner;
} XdndProxy;

/* ---------- Per-Screen Runtime State ---------- */
// Embeds ScreenConfig for the user-facing settings.
// Everything else here is transient DE runtime state.

typedef struct {
    ScreenConfig config;              // <-- shared config

    // Identity
    int screen_num;
    Window root;
    char display_name[32];
    int dpi;
    double scale;

    // AwesomeWM management
    pid_t awesome_pid;
    int awesome_fd;
    int respawn_count;
    time_t respawn_window_start;

    // Power & idle tracking (runtime)
    struct timespec last_activity;
    ScreenPowerState power_state;
    pid_t screensaver_pid;

    // Xrandr saved state (for restore on exit)
    RRCrtc saved_crtc;
    RRMode saved_mode;
    Rotation saved_rotation;
    int saved_x;
    int saved_y;
    RROutput saved_outputs[8];
    int saved_noutput;

    // Xsettings daemon
    pid_t xsettingsd_pid;

    // Compositor runtime
    pid_t picom_pid;
    int picom_respawn_count;
    time_t picom_respawn_start;

    // System tray runtime
    pid_t tray_pid;
    int tray_respawn_count;
    time_t tray_respawn_start;

    // Monitor / EDID info (populated by RandR queries)
    char monitor_name[64];
    char output_name[64];
    unsigned char edid[512];
    size_t edid_len;
    int monitor_width;
    int monitor_height;
    int monitor_refresh_mhz;
    int monitor_phys_width_mm;
    int monitor_phys_height_mm;
    bool monitor_connected;
    Rotation monitor_rotation;       // Current rotation from CRTC

    // Available display modes (populated from XRandR)
    struct {
        int width;
        int height;
        int refresh_mhz;
    } modes[MAX_MONITOR_MODES];
    int mode_count;

	int pending_theme_request_fd;
} ScreenState;

/* ---------- Global DE Runtime State ---------- */
// Embeds GlobalConfig for user-facing settings.
// Everything else is transient DE runtime state.

typedef struct {
    GlobalConfig config;              // Shared config
	bool stellar_shutdown;

    // X11
    Display *dpy;
    int xfd;
    int server_fd;
    lua_State *L;

    int pointer_screen;
    int xi_opcode;
    double edge_force_x;
    double edge_force_y;

    // RandR
    int rr_event_base;
    int rr_error_base;

    // XDND atoms
    Atom xdnd_selection;
    Atom xdnd_aware;
    Atom xdnd_enter;
    Atom xdnd_position;
    Atom xdnd_status;
    Atom xdnd_leave;
    Atom xdnd_drop;
    Atom xdnd_finished;
    Atom xdnd_action_copy;
    Atom xdnd_type_list;
    Atom text_uri_list;
    Atom xdnd_proxy_prop;

    // Proxy windows (one per screen, hidden, XdndAware)
    Window xdnd_proxy_win[MAX_SCREENS];

    // Drop-shield windows (one per screen, full-screen InputOnly, XdndAware).
    // Mapped on the SOURCE screen only while a proxy drag is active, so the
    // original drag source's own (still-running) XDND session targets us
    // instead of whatever app happens to sit under its stale coordinates.
    Window xdnd_shield_win[MAX_SCREENS];

    // Proxy drag state
    XdndProxy xdnd_proxy;

    // Runtime screen state (each embeds its own ScreenConfig)
    ScreenState screens[MAX_SCREENS];

    // IPC
    IpcClient clients[MAX_CLIENTS];
    int client_count;

    // Paths
    char awesome_base_conf_path[PATH_MAX];
    char socket_path[PATH_MAX];
    char awesome_conf_path[PATH_MAX];

    // D-Bus
    pid_t dbus_session_pid;
    bool dbus_started_by_stellar;

    // SNI tray bridge runtime
    pid_t  snitray_pid;
    int    snitray_respawn_count;
    time_t snitray_respawn_start;

    // Screensaver daemon runtime
    pid_t saver_pid;
    int saver_respawn_count;
    time_t saver_respawn_start;
} StellarState;

/* ---------- Global State ---------- */
extern StellarState *g_state;
extern FILE *log_file;

/* ---------- Public Utility Functions ---------- */
void log_info(const char *fmt, ...);
void log_error(const char *fmt, ...);
const char* get_cursor_theme_for_screen(StellarState *st, int screen_num);
int get_cursor_size_for_screen(StellarState *st, int screen_num);
const char* get_font_name_for_screen(StellarState *st, int screen_num);
float get_font_size_for_screen(StellarState *st, int screen_num);
const char *get_font_unit_for_screen(StellarState *st, int screen_num);
// Responds to the "GET_APPEARANCE screen=N" IPC command on fd with one line
// of JSON: {"font_name":..,"font_size":..,"font_path":..,"font_is_bitmap":..,
// "dpi":..,"cursor_theme":..,"cursor_size":..}.  Call from the IPC command
// dispatcher in ipc_lua.h.
void ipc_handle_get_appearance(StellarState *st, int fd, int screen_num);
int get_screensaver_timeout_for_screen(StellarState *st, int screen_num);
int get_dpms_timeout_for_screen(StellarState *st, int screen_num);
int ensure_runtime_dir_and_socket_path(StellarState *st);
void generate_stellar_xresources(StellarState *st);
void apply_stellar_xsettings(StellarState *st);
void apply_system_daemons(StellarState *st);
void cleanup_phantom_awesome_drawins(StellarState *st, int screen_num);
void reset_cursor_sprite(StellarState *st, ScreenState *sc, int screen_idx);
// Detaches `client` from Awesome (reparent to root), sets _NET_WM_WINDOW_TYPE
// to `type_value` while unmanaged, then remaps so Awesome re-manages from
// scratch and reads the new type at manage time. This is the only way to make
// a window-type rule stick for apps (e.g. NoMachine) that set their matchable
// title only AFTER Awesome has already managed the window, since Awesome
// treats _NET_WM_WINDOW_TYPE as read-only post-manage.
void force_remanage_with_type(StellarState *st, Window client, Atom type_value);
// Re-applies window rules authoritatively to all currently-existing managed
// windows. Call after reloading settings.json so changes to titlebar/fullscreen
// flags (and removal of rules) take effect live. Does not re-manage windows or
// change their type; only updates the _STELLAR_* flag xproperties.
void reapply_window_rules_to_existing(StellarState *st);
// Authoritative single-window rule application (defined in ipc_lua.c). Used by
// the sweep above. Sets the flags a matching rule specifies and strips those it
// doesn't; strips all _STELLAR_* flags when no rule matches.
RuleResult apply_window_rules_authoritative(StellarState *st, Window w,
                                            const char *class_name,
                                            const char *win_name);

#endif // STELLAR_H
