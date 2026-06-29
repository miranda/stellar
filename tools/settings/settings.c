// stellar_settings.c
// Settings GUI for the Stellar desktop environment.
// Uses GlobalConfig/ScreenConfig from stellar_config.h for all persistent config.
// UI-only state (font preview, is_active flags) lives in a separate struct.

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <sys/wait.h>
#include <unistd.h>
#include <limits.h>
#include <pwd.h>
#include <poll.h>
#include <time.h>
#include <math.h>
#include <glob.h>

#define NK_INCLUDE_FIXED_TYPES
#define NK_INCLUDE_STANDARD_IO
#define NK_INCLUDE_STANDARD_VARARGS
#define NK_INCLUDE_DEFAULT_ALLOCATOR
#define NK_IMPLEMENTATION
#include "nuklear.h"

#define NK_XCB_CAIRO_IMPLEMENTATION
#include "nuklear_xcb.h"

#include "cJSON.h"
#include "stellar_config.h"
#include "stellar_hw.h"
#include "stellar_theme.h"
#include "stellar_nk_theme.h"
#include "stellar_font.h"

#include "stellar_settings_state.h"

// ---------------------------------------------------------------------------
// Global state DEFINITIONS. These are declared `extern` in
// stellar_settings_state.h and referenced from views.c and xorg_gen.c.
// (Structs/enums themselves live in that header.)
// ---------------------------------------------------------------------------

GlobalConfig state = {0};
UIState ui = {0};
ThemeData theme_data;

int appearance_target = -1;            // -1 = Global, 0+ = screen index
SettingsView current_view = VIEW_APPEARANCE;

int stellar_num_screens;
int stellar_screen;                    // index of the screen THIS app runs on

// Display/topology + staging state (consumed by xorg_gen.c)
int screen_grid[MAX_SCREENS][MAX_SCREENS];
char staging_dir[PATH_MAX];
bool valid_installed_conf_exists = false;
bool valid_staging_conf_exists = false;
char active_installed_conf_path[PATH_MAX] = "";
int pending_xorg_change = XORG_CHANGE_NONE;
int hardware_mismatch_detected = 0;
bool requires_restart = false;

// Monitor info from the DE (GET_SCREEN_INFO)
MonitorInfo monitor_info[MAX_SCREENS];
bool monitor_info_loaded = false;

// Live active-output EDID for monitor-swap detection.
struct LiveOutput live_outputs[MAX_SCREENS];
int live_output_count = 0;

// Font preview lifecycle (driven by the event loop; read by views.c)
struct nk_user_font *preview_font = NULL;
int pending_preview_update = 0;
int pending_theme_apply = 0;
char **font_families = NULL;
int font_family_count = 0;
StellarFontInfo preview_info;
int preview_resolved = 0;
bool current_font_has_bitmap = false;
bool current_font_has_vector = false;
float *current_strikes = NULL;
char **current_strike_labels = NULL;
int current_strike_count = 0;
int force_bitmap_resolve = 0;          // signals the event loop to query in px

static cJSON *cached_window_rules = NULL;
static cJSON *cached_compositor_settings = NULL;

// --- Functions provided by views.c / xorg_gen.c that settings.c calls ---
void process_frame(struct nk_context *ctx, int os_win_width, int os_win_height);
void get_active_appearance_ptrs(char **font_buf, float **vec_size, float **bmp_size,
                                char **unit, bool **prefer_bitmap);
void init_staging_dir(void);
void validate_stellar_xorg_conf_files(void);
void convert_neighbors_to_grid(void);
void refresh_pending_xorg_change(void);
void remove_installed_display_config(void);
void restore_staging_display_config(void);
int  calc_auto_dpi(int native_width_px, int phys_width_mm);

// now_ms: monotonic millisecond clock used by the resize-settle logic in the
// event loop. Defined here (was below process_frame in the monolith) so it is
// visible before main().
static long long now_ms(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (long long)ts.tv_sec * 1000LL + ts.tv_nsec / 1000000LL;
}

// --- Helpers ---
int get_env_int(const char *var_name, int default_val) {
    const char *env_str = getenv(var_name);
    if (!env_str) return default_val;

    char *endptr;
    long val = strtol(env_str, &endptr, 10);

    if (endptr == env_str || *endptr != '\0') return default_val;
    if (val < INT_MIN || val > INT_MAX) return default_val;

    return (int)val;
}

#include <stdbool.h>

// Replicates the path-searching behavior of execlp()
bool is_executable_in_path(const char *prog_name) {
    if (!prog_name || prog_name[0] == '\0') {
        return false;
    }

    // If the program name contains a slash, execlp() treats it as a path
    // and bypasses the $PATH search. We do the same.
    if (strchr(prog_name, '/')) {
        return access(prog_name, X_OK) == 0;
    }

    const char *path_env = getenv("PATH");
    if (!path_env) {
        // POSIX standard fallback if PATH is entirely unset
        path_env = "/usr/bin:/bin";
    }

    // We must duplicate the PATH string because strtok modifies the string in-place
    char *path_copy = strdup(path_env);
    if (!path_copy) {
        return false; 
    }

    bool found = false;
    char full_path[1024];

    char *dir = strtok(path_copy, ":");
    while (dir != NULL) {
        // POSIX states an empty PATH element (e.g., "::") represents the current directory
        if (*dir == '\0') {
            dir = ".";
        }

        // Construct the full path: dir/prog_name
        int len = snprintf(full_path, sizeof(full_path), "%s/%s", dir, prog_name);
        
        // Ensure we didn't truncate the path
        if (len >= 0 && len < sizeof(full_path)) {
            // access() returns 0 if the user has execute permission
            if (access(full_path, X_OK) == 0) {
                found = true;
                break;
            }
        }
        dir = strtok(NULL, ":");
    }

    free(path_copy);
    return found;
}

const char* choose_program(const char *programs[], size_t count) {
	char *choice = NULL;
    for (size_t i = 0; i < count; ++i) {
        if (is_executable_in_path(programs[i])) {
            choice = programs[i];
			break;
        }
    }
    return choice; 
}

// Canonical list of known GUI terminal emulators, shared by the initial-pick
// logic and the UI dropdown. NULL-terminated so the UI can iterate without a
// separate count.
const char *known_terminals[] = {
    "wezterm",
    "alacritty",
    "kitty",
    "ghostty",
    "urxvt",
    "qterminal",
    "xterm",
    NULL
};

// Pick the first installed terminal emulator (bare name, e.g. "alacritty").
const char* get_initial_terminal(void) {
    size_t count = 0;
    while (known_terminals[count]) count++;

    const char *choice = choose_program(known_terminals, count);

    if (choice) {
        printf("Found terminal: %s\n", choice);
    } else {
        printf("Error: No suitable terminal emulator found in $PATH.\n");
    }

    return choice;
}

// Pick the initial login shell as a full path (e.g. "/bin/bash"). Prefers bash
// when present; otherwise falls back to the first usable entry in /etc/shells,
// then to /bin/sh. Returns NULL if nothing usable is found.
const char* get_initial_shell(void) {
    static char shell_buf[256];

    // Prefer bash. Resolve its full path via /etc/shells if we can, else the
    // conventional location.
    bool want_bash = is_executable_in_path("bash");

    FILE *f = fopen("/etc/shells", "r");
    if (f) {
        char line[256];
        char first_usable[256] = "";
        while (fgets(line, sizeof(line), f)) {
            char *s = line;
            while (*s == ' ' || *s == '\t') s++;          // skip leading space
            if (*s == '#' || *s == '\n' || *s == '\0') continue;  // comments/blanks
            s[strcspn(s, "\r\n")] = '\0';                 // strip newline
            if (access(s, X_OK) != 0) continue;           // must be executable

            if (first_usable[0] == '\0')
                snprintf(first_usable, sizeof(first_usable), "%s", s);

            // If we want bash and this entry is a bash, take it immediately.
            if (want_bash) {
                const char *base = strrchr(s, '/');
                base = base ? base + 1 : s;
                if (strcmp(base, "bash") == 0) {
                    snprintf(shell_buf, sizeof(shell_buf), "%s", s);
                    fclose(f);
                    return shell_buf;
                }
            }
        }
        fclose(f);

        if (first_usable[0] != '\0') {
            snprintf(shell_buf, sizeof(shell_buf), "%s", first_usable);
            return shell_buf;
        }
    }

    // Last-resort fallbacks if /etc/shells is missing or empty.
    if (want_bash && access("/bin/bash", X_OK) == 0) return "/bin/bash";
    if (access("/bin/sh", X_OK) == 0) return "/bin/sh";
    return NULL;
}

static void load_gpu_names_from_cache(void) {
    FILE *f = fopen("/var/cache/stellar/hardware.json", "r");
    if (!f) return;

    fseek(f, 0, SEEK_END);
    long length = ftell(f);
    if (length < 0) { fclose(f); return; }
    fseek(f, 0, SEEK_SET);
    char *data = malloc(length + 1);
    if (!data) { fclose(f); return; }
    size_t got = fread(data, 1, length, f);
    fclose(f);
    data[got] = '\0';

    cJSON *json = cJSON_Parse(data);
    free(data);
    if (!json) return;

    cJSON *cards = cJSON_GetObjectItemCaseSensitive(json, "cards");
    if (cards) {
        // Iterate through all possible cards
        for (int c = 0; c < 16; c++) {
            char key[16];
            snprintf(key, sizeof(key), "%d", c);
            cJSON *card = cJSON_GetObjectItemCaseSensitive(cards, key);
            if (!card) continue;

            // Get the newly added GPU name
            cJSON *gpu_name = cJSON_GetObjectItemCaseSensitive(card, "name");
            const char *gpu_name_str = (cJSON_IsString(gpu_name) && gpu_name->valuestring) 
                                        ? gpu_name->valuestring : "Unknown GPU";

            // Map this GPU name to its outputs
            cJSON *outputs = cJSON_GetObjectItemCaseSensitive(card, "outputs");
            if (cJSON_IsArray(outputs)) {
                cJSON *output = NULL;
                cJSON_ArrayForEach(output, outputs) {
                    if (!cJSON_IsObject(output)) continue;
                    cJSON *out_name = cJSON_GetObjectItemCaseSensitive(output, "name");
                    
                    if (cJSON_IsString(out_name) && out_name->valuestring) {
                        // Find which live screen is using this output name
                        for (int i = 0; i < MAX_SCREENS; i++) {
                            if (monitor_info[i].connected && 
                                strcmp(monitor_info[i].output_name, out_name->valuestring) == 0) {
                                snprintf(monitor_info[i].gpu_name, sizeof(monitor_info[i].gpu_name), "%s", gpu_name_str);

                                // VRR capability is recorded per-output by the
                                // privileged probe (it can read the property even
                                // for ports that are inactive in the live Zaphod
                                // server). Source it from the cache so the toggle
                                // survives a monitor being offline; the live IPC
                                // path never carries it.
                                cJSON *vrr = cJSON_GetObjectItemCaseSensitive(output, "vrr_capable");
                                if (cJSON_IsBool(vrr))
                                    monitor_info[i].vrr_capable = cJSON_IsTrue(vrr);
                            }
                        }
                    }
                }
            }
        }
    }
    cJSON_Delete(json);
}

// --- IPC ---
static void send_stellar_command(const char *cmd) {
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

void request_screen_info(void) {
    const char *socket_path = getenv("STELLAR_SOCKET");
    if (!socket_path) {
        printf("Error: STELLAR_SOCKET not set. Cannot request screen info.\n");
        return;
    }

    int sock = socket(AF_UNIX, SOCK_STREAM, 0);
    if (sock < 0) return;

    struct sockaddr_un addr = {0};
    addr.sun_family = AF_UNIX;
    strncpy(addr.sun_path, socket_path, sizeof(addr.sun_path) - 1);

    if (connect(sock, (struct sockaddr *)&addr, sizeof(addr)) != 0) {
        printf("Failed to connect to Stellar for screen info.\n");
        close(sock);
        return;
    }

    dprintf(sock, "GET_SCREEN_INFO\n");

    // Read the JSON response line (with a timeout)
    char buf[4096] = {0};
    size_t total = 0;

    struct pollfd pfd = { .fd = sock, .events = POLLIN };
    while (total < sizeof(buf) - 1) {
        int pr = poll(&pfd, 1, 2000);
        if (pr <= 0) break;

        ssize_t n = read(sock, buf + total, sizeof(buf) - 1 - total);
        if (n <= 0) break;
        total += n;

        if (memchr(buf, '\n', total)) break;
    }
    close(sock);

    if (total == 0) {
        printf("No response from Stellar for GET_SCREEN_INFO.\n");
        return;
    }

    // Parse the JSON
    cJSON *json = cJSON_Parse(buf);
    if (!json) {
        printf("Failed to parse screen info JSON.\n");
        return;
    }

    memset(monitor_info, 0, sizeof(monitor_info));
    memset(live_outputs, 0, sizeof(live_outputs));
    live_output_count = 0;

    cJSON *screens = cJSON_GetObjectItemCaseSensitive(json, "screens");
    if (screens && cJSON_IsArray(screens)) {
        cJSON *screen = NULL;
        cJSON_ArrayForEach(screen, screens) {
            cJSON *idx = cJSON_GetObjectItemCaseSensitive(screen, "index");
            if (!cJSON_IsNumber(idx)) continue;
            int i = idx->valueint;
            if (i < 0 || i >= MAX_SCREENS) continue;

            MonitorInfo *mi = &monitor_info[i];
            cJSON *item;

            item = cJSON_GetObjectItemCaseSensitive(screen, "monitor_name");
            if (cJSON_IsString(item) && item->valuestring)
                snprintf(mi->monitor_name, sizeof(mi->monitor_name), "%s", item->valuestring);

            item = cJSON_GetObjectItemCaseSensitive(screen, "output_name");
            if (cJSON_IsString(item) && item->valuestring)
                snprintf(mi->output_name, sizeof(mi->output_name), "%s", item->valuestring);

            // Capture this active output's live EDID for swap detection. Only
            // record entries that have both an output name and a non-empty EDID;
            // those are the active ZaphodHeads outputs check_monitor_mismatch
            // can meaningfully compare against the cache.
            {
                cJSON *edid_item = cJSON_GetObjectItemCaseSensitive(screen, "edid");
                if (live_output_count < MAX_SCREENS &&
                    mi->output_name[0] != '\0' &&
                    cJSON_IsString(edid_item) && edid_item->valuestring &&
                    edid_item->valuestring[0] != '\0') {
                    struct LiveOutput *lo = &live_outputs[live_output_count++];
                    snprintf(lo->output_name, sizeof(lo->output_name),
                             "%s", mi->output_name);
                    snprintf(lo->edid_hex, sizeof(lo->edid_hex),
                             "%s", edid_item->valuestring);
                }
            }

            item = cJSON_GetObjectItemCaseSensitive(screen, "display");
            if (cJSON_IsString(item) && item->valuestring)
                snprintf(mi->display, sizeof(mi->display), "%s", item->valuestring);

            item = cJSON_GetObjectItemCaseSensitive(screen, "rotation");
            if (cJSON_IsString(item) && item->valuestring)
                snprintf(mi->rotation, sizeof(mi->rotation), "%s", item->valuestring);

            item = cJSON_GetObjectItemCaseSensitive(screen, "connected");
            if (cJSON_IsBool(item)) mi->connected = cJSON_IsTrue(item);

            item = cJSON_GetObjectItemCaseSensitive(screen, "width");
            if (cJSON_IsNumber(item)) mi->width = item->valueint;

            item = cJSON_GetObjectItemCaseSensitive(screen, "height");
            if (cJSON_IsNumber(item)) mi->height = item->valueint;

            item = cJSON_GetObjectItemCaseSensitive(screen, "refresh_mhz");
            if (cJSON_IsNumber(item)) mi->refresh_mhz = item->valueint;

            item = cJSON_GetObjectItemCaseSensitive(screen, "phys_width_mm");
            if (cJSON_IsNumber(item)) mi->phys_width_mm = item->valueint;

            item = cJSON_GetObjectItemCaseSensitive(screen, "phys_height_mm");
            if (cJSON_IsNumber(item)) mi->phys_height_mm = item->valueint;

            item = cJSON_GetObjectItemCaseSensitive(screen, "dpi");
            if (cJSON_IsNumber(item)) mi->dpi = item->valueint;

            // Parse available modes
            cJSON *modes = cJSON_GetObjectItemCaseSensitive(screen, "modes");
            if (modes && cJSON_IsArray(modes)) {
                cJSON *mode_entry = NULL;
                mi->mode_count = 0;
                cJSON_ArrayForEach(mode_entry, modes) {
                    if (mi->mode_count >= MAX_MONITOR_MODES) break;
 
                    cJSON *mw = cJSON_GetObjectItemCaseSensitive(mode_entry, "w");
                    cJSON *mh = cJSON_GetObjectItemCaseSensitive(mode_entry, "h");
                    cJSON *mr = cJSON_GetObjectItemCaseSensitive(mode_entry, "r");
 
                    if (cJSON_IsNumber(mw) && cJSON_IsNumber(mh) && cJSON_IsNumber(mr)) {
                        int idx = mi->mode_count;
                        mi->modes[idx].width = mw->valueint;
                        mi->modes[idx].height = mh->valueint;
                        mi->modes[idx].refresh_mhz = mr->valueint;
 
                        int hz = (mr->valueint + 500) / 1000;
                        snprintf(mi->modes[idx].label, sizeof(mi->modes[idx].label),
                                 "%dx%d @ %d Hz", mw->valueint, mh->valueint, hz);
                        snprintf(mi->modes[idx].value, sizeof(mi->modes[idx].value),
                                 "%dx%d@%d", mw->valueint, mh->valueint, hz);
 
                        mi->mode_count++;
                    }
                }
            }
        }

        monitor_info_loaded = true;
        printf("Loaded monitor info for %d screens.\n", stellar_num_screens);

        // Map the static GPU names to the live IPC data
        load_gpu_names_from_cache();
    }

    cJSON_Delete(json);
}

// Combined re-probe decision: GPU identity (always) plus per-output EDID swap
// detection (when live monitor info has been loaded from the DE). Either
// triggering means the cached hardware.json no longer reflects reality.
// Call request_screen_info() first so live_outputs is populated; if it hasn't
// run, the monitor half is simply skipped (live_output_count == 0).
int check_full_mismatch(void) {
    if (check_hardware_mismatch() != 0)
        return 1;
    return check_monitor_mismatch(live_outputs, live_output_count);
}

static void init_default_settings(void) {
    memset(&state, 0, sizeof(GlobalConfig));
    memset(&ui, 0, sizeof(UIState));

    // UI-only defaults
    snprintf(ui.font_name_buf, sizeof(ui.font_name_buf), "sans-serif");
    ui.vector_font_size = 14;

    // Global appearance defaults
    snprintf(state.stellar_theme, sizeof(state.stellar_theme), "stellar-blue");
    snprintf(state.appearance.cursor_theme, sizeof(state.appearance.cursor_theme), "default");
    state.appearance.cursor_size = 16;
    snprintf(state.appearance.font_name, sizeof(state.appearance.font_name), "sans-serif");
    state.appearance.font_size = 14.0f;
    snprintf(state.appearance.font_unit, sizeof(state.appearance.font_unit), "pt");
    // NOTE: Keep the default wallpaper path and mode in sync with stellar_bridge.lua
    snprintf(state.appearance.wallpaper_path, sizeof(state.appearance.wallpaper_path), 
             "%s/wallpapers/pleiades-blue.jpg", STELLAR_SHARE_PATH);
    snprintf(state.appearance.wallpaper_mode, sizeof(state.appearance.wallpaper_mode), 
             "cropped");

    // Global power defaults
    state.saver_enabled = true;
    state.power.timeout_screensaver = 30;
    state.power.timeout_dpms = 60;

    // Terminal defaults: GUI emulator (bare name) and login shell (full path).
    {
        const char *emu = get_initial_terminal();
        if (emu)
            snprintf(state.term_gui, sizeof(state.term_gui), "%s", emu);

        const char *sh = get_initial_shell();
        if (sh)
            snprintf(state.term_shell, sizeof(state.term_shell), "%s", sh);
    }

    // Screen defaults
    state.screen_count = stellar_num_screens;
    for (int i = 0; i < MAX_SCREENS; i++) {
        ScreenConfig *sc = &state.screens[i];

        ui.screens[i].is_active = (i < stellar_num_screens);

        sc->phys_scale = 1.0f;
        sc->phys_offset = 0.0f;
        sc->picom_enabled = true;
        sc->tray_enabled = true;
        sc->tearfree_enabled = true;
        sc->vrr_enabled = false;
        snprintf(sc->rotation, sizeof(sc->rotation), "normal");

		// Init screen layout sequentially horizontally
        sc->neighbor_left = (i > 0) ? (i - 1) : -1;
        sc->neighbor_right = (i < (stellar_num_screens - 1)) ? (i + 1) : -1;
        sc->neighbor_up = -1;
        sc->neighbor_down = -1;

        snprintf(sc->appearance.cursor_theme, sizeof(sc->appearance.cursor_theme), "default");
        sc->appearance.cursor_size = 16;

        // Per-screen UI font defaults (used when override is first enabled)
        snprintf(ui.screens[i].font_name_buf, sizeof(ui.screens[i].font_name_buf), "sans-serif");
        ui.screens[i].vector_font_size = 14.0f;
        ui.screens[i].bitmap_font_size = 14.0f;
        snprintf(ui.screens[i].font_unit, sizeof(ui.screens[i].font_unit), "pt");

        sc->power.timeout_screensaver = 30;
        sc->power.timeout_dpms = 60;
    }
}


static void load_settings(void) {
    char config_path[PATH_MAX];
    snprintf(config_path, sizeof(config_path),
             "%s/.config/stellar/settings.json", get_user_home_dir());

    FILE *f = fopen(config_path, "r");
    if (!f) {
        printf("Settings file not found. Using UI defaults.\n");
        return;
    }

    fseek(f, 0, SEEK_END);
    long length = ftell(f);
    fseek(f, 0, SEEK_SET);
    char *data = malloc(length + 1);
    fread(data, 1, length, f);
    fclose(f);
    data[length] = '\0';

    cJSON *json = cJSON_Parse(data);
    free(data);
    if (!json) return;

    cJSON *appearance = cJSON_GetObjectItemCaseSensitive(json, "appearance");
    if (appearance) {
        cJSON *cursor_theme = cJSON_GetObjectItemCaseSensitive(appearance, "cursor_theme");
        if (cJSON_IsString(cursor_theme) && cursor_theme->valuestring)
            snprintf(state.appearance.cursor_theme,
                     sizeof(state.appearance.cursor_theme),
                     "%s", cursor_theme->valuestring);

        cJSON *cursor_size = cJSON_GetObjectItemCaseSensitive(appearance, "cursor_size");
        if (cJSON_IsNumber(cursor_size))
            state.appearance.cursor_size = cursor_size->valueint;

        cJSON *font_name = cJSON_GetObjectItemCaseSensitive(appearance, "font_name");
        if (cJSON_IsString(font_name) && font_name->valuestring)
            snprintf(state.appearance.font_name,
                     sizeof(state.appearance.font_name),
                     "%s", font_name->valuestring);

        cJSON *font_size = cJSON_GetObjectItemCaseSensitive(appearance, "font_size");
        if (cJSON_IsNumber(font_size))
            state.appearance.font_size = (float)font_size->valuedouble;

		cJSON *font_unit = cJSON_GetObjectItemCaseSensitive(appearance, "font_unit");
		if (cJSON_IsString(font_unit) && font_unit->valuestring) {
			snprintf(state.appearance.font_unit, sizeof(state.appearance.font_unit), "%s", font_unit->valuestring);
		} else {
			snprintf(state.appearance.font_unit, sizeof(state.appearance.font_unit), "pt");
		}

		cJSON *font_prefer_bmp = cJSON_GetObjectItemCaseSensitive(appearance, "font_prefer_bitmap");
		state.appearance.font_prefer_bitmap = cJSON_IsTrue(font_prefer_bmp);

		cJSON *wallpaper = cJSON_GetObjectItemCaseSensitive(appearance, "wallpaper_path");
		if (cJSON_IsString(wallpaper) && wallpaper->valuestring)
			snprintf(state.appearance.wallpaper_path, sizeof(state.appearance.wallpaper_path),
			         "%s", wallpaper->valuestring);

		cJSON *w_mode = cJSON_GetObjectItemCaseSensitive(appearance, "wallpaper_mode");
        if (cJSON_IsString(w_mode) && w_mode->valuestring)
            snprintf(state.appearance.wallpaper_mode, sizeof(state.appearance.wallpaper_mode),
                     "%s", w_mode->valuestring);

		cJSON *stheme = cJSON_GetObjectItemCaseSensitive(appearance, "stellar_theme");
		if (cJSON_IsString(stheme) && stheme->valuestring)
			snprintf(state.stellar_theme, sizeof(state.stellar_theme), "%s", stheme->valuestring);

        // Mirror the loaded font settings into the editable UI buffers.
        if (state.appearance.font_name[0] != '\0')
            snprintf(ui.font_name_buf, sizeof(ui.font_name_buf), "%s", state.appearance.font_name);

        ui.font_prefer_bitmap = state.appearance.font_prefer_bitmap;
        
        if (strcmp(state.appearance.font_unit, "pt") == 0 && state.appearance.font_size > 0) {
            ui.vector_font_size = state.appearance.font_size;
            ui.bitmap_font_size = 14.0f; 
        } else {
            ui.vector_font_size = 14.0f;
            ui.bitmap_font_size = state.appearance.font_size;
            force_bitmap_resolve = 1; // Force the initial preview to use px
        }
    }

    cJSON *power = cJSON_GetObjectItemCaseSensitive(json, "power");
    if (power) {
        cJSON *ss_enabled = cJSON_GetObjectItemCaseSensitive(power, "screensaver_enabled");
        if (cJSON_IsBool(ss_enabled))
            state.saver_enabled = cJSON_IsTrue(ss_enabled);

        cJSON *saver = cJSON_GetObjectItemCaseSensitive(power, "timeout_screensaver");
        if (cJSON_IsNumber(saver))
            state.power.timeout_screensaver = saver->valueint;

        cJSON *dpms = cJSON_GetObjectItemCaseSensitive(power, "timeout_dpms");
        if (cJSON_IsNumber(dpms))
            state.power.timeout_dpms = dpms->valueint;
    }

    cJSON *terminal = cJSON_GetObjectItemCaseSensitive(json, "terminal");
    if (terminal) {
        cJSON *shell = cJSON_GetObjectItemCaseSensitive(terminal, "shell");
        if (cJSON_IsString(shell) && shell->valuestring)
            snprintf(state.term_shell, sizeof(state.term_shell),
                     "%s", shell->valuestring);

        cJSON *term_gui = cJSON_GetObjectItemCaseSensitive(terminal, "terminal_gui");
        if (cJSON_IsString(term_gui) && term_gui->valuestring)
            snprintf(state.term_gui, sizeof(state.term_gui),
                     "%s", term_gui->valuestring);
    }

    if (cached_compositor_settings) {
        cJSON_Delete(cached_compositor_settings);
        cached_compositor_settings = NULL;
    }
    cJSON *compositor = cJSON_GetObjectItemCaseSensitive(json, "compositor");
    if (compositor && cJSON_IsObject(compositor)) {
        cached_compositor_settings = cJSON_Duplicate(compositor, 1);
    } else {
        cached_compositor_settings = cJSON_CreateObject();
    }
    cJSON *blur = cJSON_GetObjectItemCaseSensitive(compositor, "blur");
    if (!blur) {
        blur = cJSON_CreateObject();
        cJSON_AddItemToObject(compositor, "blur", blur);
    }

    // Screens and per-screen config
    cJSON *screens = cJSON_GetObjectItemCaseSensitive(json, "screens");
    if (screens) {
        ui.screens[0].is_active = 0;

        for (int i = 0; i < MAX_SCREENS; i++) {
            char key[8];
            snprintf(key, sizeof(key), "%d", i);
            cJSON *screen = cJSON_GetObjectItemCaseSensitive(screens, key);

            if (!screen) continue;

            ui.screens[i].is_active = 1;
            ScreenConfig *sc = &state.screens[i];

            cJSON *phys = cJSON_GetObjectItemCaseSensitive(screen, "physical");
            if (phys) {
                cJSON *scale = cJSON_GetObjectItemCaseSensitive(phys, "scale");
                if (cJSON_IsNumber(scale)) sc->phys_scale = scale->valuedouble;
                cJSON *offset = cJSON_GetObjectItemCaseSensitive(phys, "offset");
                if (cJSON_IsNumber(offset)) sc->phys_offset = offset->valuedouble;
            }

            cJSON *rot = cJSON_GetObjectItemCaseSensitive(screen, "rotation");
            if (cJSON_IsString(rot) && rot->valuestring)
                snprintf(sc->rotation, sizeof(sc->rotation), "%s", rot->valuestring);

            cJSON *pmode = cJSON_GetObjectItemCaseSensitive(screen, "preferred_mode");
            if (cJSON_IsString(pmode) && pmode->valuestring)
                snprintf(sc->preferred_mode, sizeof(sc->preferred_mode), "%s", pmode->valuestring);

            cJSON *dpi_ov = cJSON_GetObjectItemCaseSensitive(screen, "dpi_override");
            if (cJSON_IsNumber(dpi_ov))
                sc->dpi_override = dpi_ov->valueint;

            cJSON *neighbors = cJSON_GetObjectItemCaseSensitive(screen, "neighbors");
            if (neighbors) {
                cJSON *nb;
                nb = cJSON_GetObjectItemCaseSensitive(neighbors, "left");
                if (cJSON_IsNumber(nb)) sc->neighbor_left = nb->valueint;
                nb = cJSON_GetObjectItemCaseSensitive(neighbors, "right");
                if (cJSON_IsNumber(nb)) sc->neighbor_right = nb->valueint;
                nb = cJSON_GetObjectItemCaseSensitive(neighbors, "up");
                if (cJSON_IsNumber(nb)) sc->neighbor_up = nb->valueint;
                nb = cJSON_GetObjectItemCaseSensitive(neighbors, "down");
                if (cJSON_IsNumber(nb)) sc->neighbor_down = nb->valueint;
            }

            cJSON *comp = cJSON_GetObjectItemCaseSensitive(screen, "compositor");
            if (comp) {
                cJSON *enabled = cJSON_GetObjectItemCaseSensitive(comp, "enabled");
                if (cJSON_IsBool(enabled)) sc->picom_enabled = cJSON_IsTrue(enabled);
            }

            cJSON *tray = cJSON_GetObjectItemCaseSensitive(screen, "tray");
            if (tray) {
                cJSON *enabled = cJSON_GetObjectItemCaseSensitive(tray, "enabled");
                if (cJSON_IsBool(enabled)) sc->tray_enabled = cJSON_IsTrue(enabled);
            }

            cJSON *tearfree = cJSON_GetObjectItemCaseSensitive(screen, "tearfree");
            if (tearfree) {
                cJSON *enabled = cJSON_GetObjectItemCaseSensitive(tearfree, "enabled");
                if (cJSON_IsBool(enabled)) sc->tearfree_enabled = cJSON_IsTrue(enabled);
            }

            cJSON *vrr = cJSON_GetObjectItemCaseSensitive(screen, "vrr");
            if (vrr) {
                cJSON *enabled = cJSON_GetObjectItemCaseSensitive(vrr, "enabled");
                if (cJSON_IsBool(enabled)) sc->vrr_enabled = cJSON_IsTrue(enabled);
            }

            cJSON *sc_power = cJSON_GetObjectItemCaseSensitive(screen, "power");
            if (sc_power) {
                cJSON *indep = cJSON_GetObjectItemCaseSensitive(sc_power, "independent_dpms");
                if (cJSON_IsBool(indep)) sc->independent_dpms = cJSON_IsTrue(indep);

                cJSON *explwake = cJSON_GetObjectItemCaseSensitive(sc_power, "require_explicit_wake");
                if (cJSON_IsBool(explwake)) sc->require_explicit_wake = cJSON_IsTrue(explwake);

                cJSON *override_power = cJSON_GetObjectItemCaseSensitive(sc_power, "override_global");
                if (cJSON_IsBool(override_power)) sc->override_global_power = cJSON_IsTrue(override_power);

                cJSON *sc_saver = cJSON_GetObjectItemCaseSensitive(sc_power, "timeout_screensaver");
                if (cJSON_IsNumber(sc_saver)) sc->power.timeout_screensaver = sc_saver->valueint;

                cJSON *sc_dpms = cJSON_GetObjectItemCaseSensitive(sc_power, "timeout_dpms");
                if (cJSON_IsNumber(sc_dpms)) sc->power.timeout_dpms = sc_dpms->valueint;
            }

            cJSON *sc_appearance = cJSON_GetObjectItemCaseSensitive(screen, "appearance");
            if (sc_appearance) {
                cJSON *override_app = cJSON_GetObjectItemCaseSensitive(sc_appearance, "override_global");
                if (cJSON_IsBool(override_app)) sc->override_global_appearance = cJSON_IsTrue(override_app);

                // Per-facet override flags. For configs written before the split,
                // fall back to the legacy override_global_appearance so an
                // existing per-screen font keeps overriding after upgrade.
                cJSON *override_font_j = cJSON_GetObjectItemCaseSensitive(sc_appearance, "override_font");
                if (cJSON_IsBool(override_font_j))
                    sc->override_font = cJSON_IsTrue(override_font_j);
                else
                    sc->override_font = sc->override_global_appearance;

                cJSON *override_wp_j = cJSON_GetObjectItemCaseSensitive(sc_appearance, "override_wallpaper");
                if (cJSON_IsBool(override_wp_j))
                    sc->override_wallpaper = cJSON_IsTrue(override_wp_j);

                cJSON *sc_cursor_theme = cJSON_GetObjectItemCaseSensitive(sc_appearance, "cursor_theme");
                if (cJSON_IsString(sc_cursor_theme) && sc_cursor_theme->valuestring)
                    snprintf(sc->appearance.cursor_theme,
                             sizeof(sc->appearance.cursor_theme),
                             "%s", sc_cursor_theme->valuestring);

                cJSON *sc_cursor_size = cJSON_GetObjectItemCaseSensitive(sc_appearance, "cursor_size");
                if (cJSON_IsNumber(sc_cursor_size))
                    sc->appearance.cursor_size = sc_cursor_size->valueint;

                // Load the per-screen fonts
                cJSON *sf_name = cJSON_GetObjectItemCaseSensitive(sc_appearance, "font_name");
                if (cJSON_IsString(sf_name) && sf_name->valuestring)
                    snprintf(sc->appearance.font_name, sizeof(sc->appearance.font_name), "%s", sf_name->valuestring);

                cJSON *sf_size = cJSON_GetObjectItemCaseSensitive(sc_appearance, "font_size");
                if (cJSON_IsNumber(sf_size)) sc->appearance.font_size = (float)sf_size->valuedouble;

                cJSON *sf_unit = cJSON_GetObjectItemCaseSensitive(sc_appearance, "font_unit");
                if (cJSON_IsString(sf_unit) && sf_unit->valuestring) {
                    snprintf(sc->appearance.font_unit, sizeof(sc->appearance.font_unit), "%s", sf_unit->valuestring);
                } else {
                    snprintf(sc->appearance.font_unit, sizeof(sc->appearance.font_unit), "pt");
                }

                cJSON *sf_pref_bmp = cJSON_GetObjectItemCaseSensitive(sc_appearance, "font_prefer_bitmap");
                sc->appearance.font_prefer_bitmap = cJSON_IsTrue(sf_pref_bmp);

                cJSON *sc_wallpaper = cJSON_GetObjectItemCaseSensitive(sc_appearance, "wallpaper_path");
                if (cJSON_IsString(sc_wallpaper) && sc_wallpaper->valuestring)
                    snprintf(sc->appearance.wallpaper_path, sizeof(sc->appearance.wallpaper_path),
                             "%s", sc_wallpaper->valuestring);

                cJSON *sc_w_mode = cJSON_GetObjectItemCaseSensitive(sc_appearance, "wallpaper_mode");
                if (cJSON_IsString(sc_w_mode) && sc_w_mode->valuestring)
                    snprintf(sc->appearance.wallpaper_mode, sizeof(sc->appearance.wallpaper_mode),
                             "%s", sc_w_mode->valuestring);

                // Initialize the UI buffers for this screen
                if (sc->appearance.font_name[0] != '\0')
                    snprintf(ui.screens[i].font_name_buf, 128, "%s", sc->appearance.font_name);
                else
                    snprintf(ui.screens[i].font_name_buf, 128, "sans-serif");

                snprintf(ui.screens[i].font_unit, sizeof(ui.screens[i].font_unit), "%s",
                         sc->appearance.font_unit[0] ? sc->appearance.font_unit : "pt");
                ui.screens[i].font_prefer_bitmap = sc->appearance.font_prefer_bitmap;

                if (strcmp(sc->appearance.font_unit, "pt") == 0 && sc->appearance.font_size > 0) {
                    ui.screens[i].vector_font_size = sc->appearance.font_size;
                    ui.screens[i].bitmap_font_size = 14.0f;
                } else {
                    ui.screens[i].vector_font_size = 14.0f;
                    ui.screens[i].bitmap_font_size = sc->appearance.font_size > 0 ? sc->appearance.font_size : 14.0f;
                }
            }
        }
    }

    // Load window rules
    if (cached_window_rules) {
        cJSON_Delete(cached_window_rules);
        cached_window_rules = NULL;
    }
    cJSON *rules = cJSON_GetObjectItemCaseSensitive(json, "window_rules");
    if (rules && cJSON_IsArray(rules)) {
        cached_window_rules = cJSON_Duplicate(rules, 1);
    } else {
        cached_window_rules = cJSON_CreateArray();
    }

    cJSON_Delete(json);
}

void save_and_reload(void) {
    cJSON *json = cJSON_CreateObject();

    cJSON *appearance = cJSON_CreateObject();
    cJSON_AddStringToObject(appearance, "cursor_theme", state.appearance.cursor_theme);
    cJSON_AddNumberToObject(appearance, "cursor_size", state.appearance.cursor_size);
    cJSON_AddStringToObject(appearance, "font_name", state.appearance.font_name);
    cJSON_AddNumberToObject(appearance, "font_size", state.appearance.font_size);
    cJSON_AddStringToObject(appearance, "font_unit", state.appearance.font_unit);
    cJSON_AddBoolToObject(appearance, "font_prefer_bitmap", state.appearance.font_prefer_bitmap);
    cJSON_AddStringToObject(appearance, "wallpaper_path", state.appearance.wallpaper_path);
    cJSON_AddStringToObject(appearance, "stellar_theme", state.stellar_theme);
    cJSON_AddItemToObject(json, "appearance", appearance);

    cJSON *power = cJSON_CreateObject();
    cJSON_AddBoolToObject(power, "screensaver_enabled", state.saver_enabled);
    cJSON_AddNumberToObject(power, "timeout_screensaver", state.power.timeout_screensaver);
    cJSON_AddNumberToObject(power, "timeout_dpms", state.power.timeout_dpms);
    cJSON_AddItemToObject(json, "power", power);

    cJSON *terminal = cJSON_CreateObject();
    cJSON_AddStringToObject(terminal, "shell", state.term_shell);
    cJSON_AddStringToObject(terminal, "terminal_gui", state.term_gui);
    cJSON_AddItemToObject(json, "terminal", terminal);

    if (cached_compositor_settings) {
        cJSON_AddItemToObject(json, "compositor",
                              cJSON_Duplicate(cached_compositor_settings, 1));
    } else {
        cJSON_AddItemToObject(json, "compositor", cJSON_CreateObject());
    }

    // Screens and per-screen config
    cJSON *screens = cJSON_CreateObject();
    for (int i = 0; i < MAX_SCREENS; i++) {
        if (!ui.screens[i].is_active) continue;

        ScreenConfig *sc = &state.screens[i];
        char key[8];
        snprintf(key, sizeof(key), "%d", i);
        cJSON *screen = cJSON_CreateObject();

        cJSON *neighbors = cJSON_CreateObject();
        if (sc->neighbor_left >= 0)  cJSON_AddNumberToObject(neighbors, "left",  sc->neighbor_left);
        if (sc->neighbor_right >= 0) cJSON_AddNumberToObject(neighbors, "right", sc->neighbor_right);
        if (sc->neighbor_up >= 0)    cJSON_AddNumberToObject(neighbors, "up",    sc->neighbor_up);
        if (sc->neighbor_down >= 0)  cJSON_AddNumberToObject(neighbors, "down",  sc->neighbor_down);
        cJSON_AddItemToObject(screen, "neighbors", neighbors);

        cJSON *phys = cJSON_CreateObject();
        cJSON_AddNumberToObject(phys, "scale", sc->phys_scale);
        cJSON_AddNumberToObject(phys, "offset", sc->phys_offset);
        cJSON_AddItemToObject(screen, "physical", phys);

        if (sc->rotation[0] != '\0')
            cJSON_AddStringToObject(screen, "rotation", sc->rotation);
 
        if (sc->preferred_mode[0] != '\0')
            cJSON_AddStringToObject(screen, "preferred_mode", sc->preferred_mode);

		if (sc->dpi_override > 0)
            cJSON_AddNumberToObject(screen, "dpi_override", sc->dpi_override);

        cJSON *comp = cJSON_CreateObject();
        cJSON_AddBoolToObject(comp, "enabled", sc->picom_enabled);
        cJSON_AddItemToObject(screen, "compositor", comp);

        cJSON *tray = cJSON_CreateObject();
        cJSON_AddBoolToObject(tray, "enabled", sc->tray_enabled);
        cJSON_AddItemToObject(screen, "tray", tray);

        cJSON *tearfree = cJSON_CreateObject();
        cJSON_AddBoolToObject(tearfree, "enabled", sc->tearfree_enabled);
		fprintf(stderr, "[settings SAVE] screen %d tearfree=%d\n", i, sc->tearfree_enabled);
        cJSON_AddItemToObject(screen, "tearfree", tearfree);

        cJSON *vrr = cJSON_CreateObject();
        cJSON_AddBoolToObject(vrr, "enabled", sc->vrr_enabled);
        cJSON_AddItemToObject(screen, "vrr", vrr);

        cJSON *sc_power = cJSON_CreateObject();
        cJSON_AddBoolToObject(sc_power, "independent_dpms", sc->independent_dpms);
        cJSON_AddBoolToObject(sc_power, "require_explicit_wake", sc->require_explicit_wake);
        cJSON_AddBoolToObject(sc_power, "override_global", sc->override_global_power);
        cJSON_AddNumberToObject(sc_power, "timeout_screensaver", sc->power.timeout_screensaver);
        cJSON_AddNumberToObject(sc_power, "timeout_dpms", sc->power.timeout_dpms);
        cJSON_AddItemToObject(screen, "power", sc_power);

        cJSON *sc_appearance = cJSON_CreateObject();
        cJSON_AddBoolToObject(sc_appearance, "override_global", sc->override_global_appearance);
        cJSON_AddBoolToObject(sc_appearance, "override_font", sc->override_font);
        cJSON_AddBoolToObject(sc_appearance, "override_wallpaper", sc->override_wallpaper);
        cJSON_AddStringToObject(sc_appearance, "cursor_theme", sc->appearance.cursor_theme);
        cJSON_AddNumberToObject(sc_appearance, "cursor_size", sc->appearance.cursor_size);
        cJSON_AddStringToObject(sc_appearance, "font_name", sc->appearance.font_name);
        cJSON_AddNumberToObject(sc_appearance, "font_size", sc->appearance.font_size);
        cJSON_AddStringToObject(sc_appearance, "font_unit", sc->appearance.font_unit);
        cJSON_AddBoolToObject(sc_appearance, "font_prefer_bitmap", sc->appearance.font_prefer_bitmap);
        cJSON_AddStringToObject(sc_appearance, "wallpaper_path", sc->appearance.wallpaper_path);
        cJSON_AddItemToObject(screen, "appearance", sc_appearance);

        cJSON_AddItemToObject(screens, key, screen);
    }
    cJSON_AddItemToObject(json, "screens", screens);

    // Save window rules
    if (cached_window_rules) {
        cJSON_AddItemToObject(json, "window_rules",
                              cJSON_Duplicate(cached_window_rules, 1));
    } else {
        cJSON_AddItemToObject(json, "window_rules", cJSON_CreateArray());
    }

    // Write to settings file
    char config_path[512];
    snprintf(config_path, sizeof(config_path),
             "%s/.config/stellar/settings.json", get_user_home_dir());

    FILE *f = fopen(config_path, "w");
    if (f) {
        char *json_string = cJSON_Print(json);
        fprintf(f, "%s\n", json_string);
        fclose(f);
        free(json_string);
        send_stellar_command("RELOAD_CONFIG");
    }
    cJSON_Delete(json);
}

// apply_nk_theme() now lives in stellar_theme.c and is shared by all support
// apps.  This helper pushes the settings UI's current font selection into the
// ThemeData (resolving name -> file via fontconfig, with transparent
// bdf/pcf -> otb conversion) so the shared theme application uses it.
//
// IMPORTANT: this resolves the font for the screen the settings app is
// RUNNING ON (stellar_screen), not whatever scope the user is currently
// editing.  That way Save & Apply previews what THIS screen will look like
// after the DE reloads, rather than blindly applying a different screen's
// font to the local UI.
void sync_ui_font_into_theme_data(void) {
    StellarFontInfo fi;

    // Determine the effective font for the local screen.
    // If the local screen overrides global appearance, use its settings;
    // otherwise fall back to global.
    const char *eff_name;
    float eff_size;
    const char *eff_unit;
    bool eff_prefer_bitmap;

    if (stellar_screen >= 0 && stellar_screen < stellar_num_screens &&
        state.screens[stellar_screen].override_global_appearance) {
        eff_name = ui.screens[stellar_screen].font_name_buf;
        eff_unit = ui.screens[stellar_screen].font_unit;
        eff_prefer_bitmap = ui.screens[stellar_screen].font_prefer_bitmap;
        if (strcmp(eff_unit, "px") == 0)
            eff_size = ui.screens[stellar_screen].bitmap_font_size;
        else
            eff_size = ui.screens[stellar_screen].vector_font_size;
    } else {
        eff_name = ui.font_name_buf;
        eff_unit = ui.font_unit;
        eff_prefer_bitmap = ui.font_prefer_bitmap;
        if (strcmp(eff_unit, "px") == 0)
            eff_size = ui.bitmap_font_size;
        else
            eff_size = ui.vector_font_size;
    }

    // Fall back to sane defaults for empty/zero values
    if (!eff_name || eff_name[0] == '\0') eff_name = "sans-serif";
    if (!eff_unit || eff_unit[0] == '\0') eff_unit = "pt";
    if (eff_size <= 0) eff_size = 14.0f;

    if (stellar_font_resolve_ex(eff_name, eff_size, eff_unit,
                                theme_data.dpi, eff_prefer_bitmap, &fi) == 0) {
        snprintf(theme_data.font, sizeof(theme_data.font), "%s", fi.family);
        snprintf(theme_data.font_path, sizeof(theme_data.font_path), "%s", fi.path);
        theme_data.font_size = fi.size_px;
        theme_data.font_is_bitmap = fi.is_bitmap;
    } else {
        snprintf(theme_data.font, sizeof(theme_data.font), "%s", eff_name);
        theme_data.font_path[0] = '\0';
        theme_data.font_size = eff_size;
    }
}


// Two independent conditions:
//   hardware_mismatch_detected -> probe is stale/missing (offers a re-probe)
//   pending_xorg_change        -> current settings differ from installed
//                                 xorg.conf (offers Save & Apply; copy depends
//                                 on whether the change is live-applicable or
//                                 needs a restart)
// Called from the top of the content pane so banners show regardless of view.



// Blocks until the dialog is closed. Returns true if exit code was 0 (the primary button).
static bool prompt_user_dialog(const char *message, const char *btn_text, const char *cancel_text) {
    pid_t pid = fork();
    
    if (pid < 0) {
        perror("Failed to fork for dialog");
        return false; 
    }

    if (pid == 0) {
        // --- CHILD PROCESS ---
        char dialog_path[PATH_MAX];
        snprintf(dialog_path, sizeof(dialog_path), "%s/stellar-dialog", STELLAR_LIBEXEC_PATH);

        execl(dialog_path, "stellar-dialog",
              "--message", message,
              "--button", btn_text,
              "--cancel-button", cancel_text,
              (char *)NULL);

        // If execl succeeds, this line is never reached.
        // If it fails (e.g., file not found), we must exit immediately using _exit() 
        // to prevent a second copy of the main GUI loop from starting.
        perror("Failed to execute stellar-dialog");
        _exit(127); 
    } else {
        // --- PARENT PROCESS ---
        int status;
        
        // waitpid blocks the main thread until this specific child process exits
        if (waitpid(pid, &status, 0) == pid) {
            if (WIFEXITED(status)) {
                // Child exited normally, return true if the user clicked the primary button (exit 0)
                return (WEXITSTATUS(status) == 0);
            }
        }
        
        // Child crashed, was killed by a signal, or waitpid failed
        return false; 
    }
}

// Uses Polkit to securely remove the actively installed Xorg config
int main(int argc, char *argv[]) {
    init_staging_dir();
    bool run_main_ui = true;

    if (argc > 1 && strcmp(argv[1], "--autostart") == 0) {
        validate_stellar_xorg_conf_files();
        
        const char *desktop = getenv("XDG_CURRENT_DESKTOP");
        bool is_stellar = (desktop && strcmp(desktop, "Stellar") == 0);

        // Assume we don't need the UI unless one of the prompts below overrides it
        run_main_ui = false; 

		printf("stellar-settings is in autostart mode.  XDG_CURRENT_DESKTOP = %s", desktop);

        if (is_stellar) {
            if (valid_installed_conf_exists) {
                // Running on installed config
                if (check_hardware_mismatch() != 0) {
                    if (prompt_user_dialog("A hardware mismatch has been detected.\nWould you like to reconfigure Stellar now?", "Yes", "Ignore")) {
                        run_main_ui = true;
					}
                }
            } else {
                // No valid conf installed
                if (valid_staging_conf_exists) {
                    if (prompt_user_dialog("A previous Stellar X11 configuration has been detected.\nWould you like to restore it now?", "Restore", "Ignore")) {
                        restore_staging_display_config();
                        requires_restart = true;
                    }
                } else {
                    if (prompt_user_dialog("Stellar must configure X11 in order to function properly.\nWould you like to do that now?", "Yes", "Ignore")) {
                        run_main_ui = true;
                    }
                }
            }
        } else {
            // Non-Stellar environment
            if (valid_installed_conf_exists) {
                if (prompt_user_dialog("A Stellar X11 configuration is currently installed.\nThis may interfere with screen functionality in other X11 environments.\nWould you like to uninstall the Stellar configuration?\n(You will be able to restore it the next time you start Stellar.)", "Uninstall", "Ignore")) {
                    remove_installed_display_config();
                    requires_restart = true;
                }
            }
        }
    }

    // --- GUI INITIALIZATION ---
    if (run_main_ui) {
		struct nk_xcb_context *xcb_ctx;
		struct nk_cairo_context *cairo_ctx;
		struct nk_context *ctx;

		int window_width = 1000;
		int window_height = 800;

		stellar_num_screens = get_env_int("STELLAR_NUM_SCREENS", 1);
		const char *env = getenv("STELLAR_SCREEN");
		if (env) stellar_screen = atoi(env);

		// Open the per-screen views focused on the screen this app is running
		// on, not the global default. Falls back to global (-1) if the index is
		// somehow out of range.
		if (stellar_screen >= 0 && stellar_screen < stellar_num_screens)
			appearance_target = stellar_screen;
		else
			appearance_target = -1;

		init_default_settings();
		load_settings();
		convert_neighbors_to_grid();
		request_screen_info();

		stellar_font_init();
		if (stellar_font_list_families(&font_families, &font_family_count) != 0) {
			fprintf(stderr, "Warning: could not list font families\n");
		}

		if (request_theme_data(stellar_screen, &theme_data) != 0) {
			fprintf(stderr, "Failed to get theme data from Stellar\n");
			return 1;
		}
		hardware_mismatch_detected = check_full_mismatch();

		// Catch a dirty config on launch: if current settings (incl. any leftover
		// settings.json rotation/mode) would generate an xorg.conf different from
		// what's installed, flag it so the user is told to save - even though no
		// hardware mismatch exists. This is the "silent dirty state" guard.
		refresh_pending_xorg_change();

		xcb_ctx = nk_xcb_init("Stellar Settings", 0, 0, window_width, window_height);
		struct nk_color background =
			stellar_theme_color(theme_data.nk_color_window, nk_rgb(0, 0, 0));

		cairo_ctx = nk_cairo_init(
			&background, NULL, 0,
			nk_xcb_create_cairo_surface(xcb_ctx));

		ctx = malloc(sizeof(struct nk_context));
		if (!ctx) {
			fprintf(stderr, "Out of memory\n");
			return 1;
		}
		nk_init_default(ctx, nk_cairo_default_font(cairo_ctx));
		apply_nk_theme(ctx, cairo_ctx, &theme_data);

		int live_w = window_width;
		int live_h = window_height;
		int ui_w = window_width;
		int ui_h = window_height;

		int running = 1;
		int need_redraw = 1;
		int resizing = 0;
		long long last_resize_at = 0;
		const long long resize_settle_ms = 1;

		int is_first_frame = 1;
		pending_preview_update = 1;

		while (running) {
			int timeout_ms = -1;

			if (resizing) {
				long long elapsed = now_ms() - last_resize_at;
				long long remaining = resize_settle_ms - elapsed;
				timeout_ms = remaining > 0 ? (int)remaining : 0;
			}

			struct pollfd pfd;
			pfd.fd = xcb_get_file_descriptor(xcb_ctx->conn);
			pfd.events = POLLIN;
			pfd.revents = 0;

			int pr = poll(&pfd, 1, timeout_ms);

			if (pr < 0) {
				perror("poll");
				break;
			}

			if (pr > 0 && (pfd.revents & POLLIN)) {
				int events = nk_xcb_handle_event(xcb_ctx, ctx);

				if (events & NK_XCB_EVENT_STOP) break;

				need_redraw = 1;

				if (events & NK_XCB_EVENT_PAINT) {
					nk_cairo_damage(cairo_ctx);
					need_redraw = 1;
				}

				if (events & NK_XCB_EVENT_RESIZED) {
					live_w = xcb_ctx->width;
					live_h = xcb_ctx->height;

					nk_xcb_resize_cairo_surface(
						xcb_ctx, nk_cairo_surface(cairo_ctx));

					if (is_first_frame) {
						ui_w = live_w;
						ui_h = live_h;
					} else {
						resizing = 1;
						last_resize_at = now_ms();
					}

					nk_cairo_damage(cairo_ctx);
					need_redraw = 1;
				}
			} else {
				if (resizing && (now_ms() - last_resize_at) >= resize_settle_ms) {
					ui_w = live_w;
					ui_h = live_h;
					resizing = 0;
					nk_cairo_damage(cairo_ctx);
					need_redraw = 1;
				}
			}

			if (pending_preview_update) {
				char *p_name; float *p_vec; float *p_bmp; char *p_unit; bool *p_prefer_bmp;
				get_active_appearance_ptrs(&p_name, &p_vec, &p_bmp, &p_unit, &p_prefer_bmp);

				// Guard: a non-overriding screen target (or any not-yet-populated
				// buffer) leaves p_name empty. Resolving an empty family is
				// meaningless and just churns fontconfig; clear the flag, mark
				// the preview unresolved, and skip. This is also the defensive
				// stop for the spin observed when switching the target dropdown
				// to a screen that isn't overriding the font facet.
				if (!p_name || p_name[0] == '\0') {
					preview_resolved = 0;
					pending_preview_update = 0;
					need_redraw = 1;
					goto preview_done;
				}

				// 1. Determine if we are querying a vector point size, or a locked
				//    bitmap pixel size. Resolve in px when EITHER the user just
				//    picked a strike (force_bitmap_resolve) OR the active target is
				//    already a px/bitmap font (its loaded unit is "px"). Keying off
				//    the unit is what keeps a per-screen strike intact when you
				//    switch the appearance-target dropdown: without it, a plain
				//    refresh would resolve the bitmap font in "pt" mode against the
				//    default vector size and then clobber *p_bmp with whatever
				//    strike that pt request happened to snap to.
				int resolve_px = force_bitmap_resolve ||
				                 (p_unit && strcmp(p_unit, "px") == 0);
				float r_size = resolve_px ? *p_bmp : *p_vec;
				const char *r_unit = resolve_px ? "px" : "pt";
				force_bitmap_resolve = 0;

				// 2. Query Fontconfig (with bitmap preference when set).
				//    stellar_font_resolve_ex guards the preference: if the family
				//    has no bitmap variant, the preference is silently ignored.
				preview_resolved =
					(stellar_font_resolve_ex(p_name, r_size, r_unit, theme_data.dpi,
					                         *p_prefer_bmp, &preview_info) == 0);
				
				// Immediately store the resolved unit so "Save" knows what it is later
				if (preview_resolved) {
					snprintf(p_unit, 8, preview_info.is_bitmap ? "px" : "pt");

					// 2b. Detect whether both bitmap and vector variants exist
					//     for this family (drives the UI toggle checkbox).
					stellar_font_family_has_variants(preview_info.family,
					                                 &current_font_has_bitmap,
					                                 &current_font_has_vector);

					// 3. If it's a bitmap, fetch all available sizes for the dropdown
					if (preview_info.is_bitmap) {
						// Clean up old strikes
						if (current_strikes) free(current_strikes);
						if (current_strike_labels) {
							for (int i = 0; i < current_strike_count; i++) free(current_strike_labels[i]);
							free(current_strike_labels);
						}

						// Fetch new strikes and build the UI string labels
						stellar_font_list_strikes(preview_info.family, &current_strikes, &current_strike_count);
						if (current_strike_count > 0) {
							current_strike_labels = malloc(current_strike_count * sizeof(char*));
							for (int i = 0; i < current_strike_count; i++) {
								char buf[32];
								snprintf(buf, sizeof(buf), "%.0f px", current_strikes[i]);
								current_strike_labels[i] = strdup(buf);
							}
						}
						// Sync the UI state to whatever strike fontconfig actually gave us
						*p_bmp = preview_info.size_px;
					}

					// 4. Fallback font resolution (unchanged, strictly using "px" to match the primary font)
					StellarFontInfo fb[2];
					const char *fb_files[2] = { NULL, NULL };
					
					int fb_count = stellar_font_resolve_fallbacks(
						preview_info.path, preview_info.size_px, "px", theme_data.dpi,
						fb, 2);
						
					for (int f = 0; f < fb_count; f++) fb_files[f] = fb[f].path;

					struct nk_user_font *new_font =
						nk_cairo_font_create_with_fallbacks(
							cairo_ctx, preview_info.path, preview_info.size_px,
							fb_files, fb_count);

					if (new_font) {
						if (preview_font) nk_cairo_font_free(preview_font);
						preview_font = new_font;
						printf("Loaded preview font: %s -> %s (%.1fpx)\n",
							   p_name, preview_info.path,
							   (double)preview_info.size_px);
					} else {
						printf("Failed to load preview font file: %s\n",
							   preview_info.path);
					}
				} else {
					printf("Could not resolve font: %s\n", p_name);
				}
				pending_preview_update = 0;
				need_redraw = 1;
			}
			preview_done:;

			if (pending_theme_apply) {
				sync_ui_font_into_theme_data();
				apply_nk_theme(ctx, cairo_ctx, &theme_data);
				pending_theme_apply = 0;
				need_redraw = 1;
			}

			if (need_redraw) {
				process_frame(ctx, ui_w, ui_h);
				nk_cairo_render(cairo_ctx, ctx);
				nk_xcb_render(xcb_ctx);
				nk_clear(ctx);
				need_redraw = 0;
				is_first_frame = 0;
			}
		}

		if (preview_font) nk_cairo_font_free(preview_font);
		stellar_font_free_families(font_families, font_family_count);

		if (current_strikes) free(current_strikes);
		if (current_strike_labels) {
			for (int i = 0; i < current_strike_count; i++) free(current_strike_labels[i]);
			free(current_strike_labels);
		}

		nk_free(ctx);
		free(ctx);
		stellar_nk_theme_cleanup(cairo_ctx);
		nk_cairo_free(cairo_ctx);
		nk_xcb_free(xcb_ctx);
	}

	if (requires_restart) {
        const char *desktop = getenv("XDG_CURRENT_DESKTOP");
        bool is_stellar = (desktop && strcmp(desktop, "Stellar") == 0);

        pid_t pid = fork();
        if (pid == 0) {
            setsid(); 
            char dialog_path[PATH_MAX];
            snprintf(dialog_path, sizeof(dialog_path), "%s/stellar-dialog", STELLAR_LIBEXEC_PATH);
            
            if (is_stellar) {
                // We are in Stellar; use native IPC for a graceful restart
                execl(dialog_path, "stellar-dialog",
                      "--message", "Your X11 configuration has been changed.\nYou need to restart the X server to activate the new configuration.",
                      "--action-cmd", "RESTART_X11",
                      "--button", "Restart",
                      "--cancel-button", "Ignore",
                      NULL);
            } else {
                // We are in KDE/GNOME/etc; use systemd to log the user out
                char cmd[128];
                const char *session_id = getenv("XDG_SESSION_ID");

                if (session_id && session_id[0] != '\0') {
                    snprintf(cmd, sizeof(cmd), "loginctl terminate-session %s", session_id);
                } else {
                    // Fallback: terminate all graphical sessions for this user
                    const char *user = getenv("USER");
                    snprintf(cmd, sizeof(cmd), "loginctl terminate-user %s", user ? user : "");
                }

                execl(dialog_path, "stellar-dialog",
                      "--message", "Your Stellar display configuration has been removed.\nYou need to log out to restore your desktop's default screen behavior.",
                      "--action-method", "shell",
                      "--action-cmd", cmd,
                      "--button", "Log Out",
                      "--cancel-button", "Ignore",
                      NULL);
            }

            exit(1);
        }
    }

    return 0;
}
