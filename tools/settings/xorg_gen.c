// xorg_gen.c
// Display-configuration + screen-topology logic for the Stellar settings app.
// Pure C (cJSON, libc); NO nuklear. Split out of the settings monolith so the
// topology/xorg.conf machinery is isolated from the UI.
//
// Owns: the screen-arrangement grid (neighbor<->grid conversion, contiguity,
// drag-to-move), DPI math, xorg.conf generation into the staging dir, change
// classification (none/live/structural), validation, and apply/remove/restore
// of the installed config via the privileged helper.
//
// State (state, ui, screen_grid, staging_dir, monitor_info, ...) is defined in
// settings.c and shared via stellar_settings_state.h.

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>
#include <limits.h>
#include <math.h>
#include <glob.h>

#include "cJSON.h"
#include "stellar_settings_state.h"
#include "stellar_config.h"

/* ---- Shared globals (defined in settings.c) ---- */
extern GlobalConfig state;
extern UIState ui;
extern int screen_grid[MAX_SCREENS][MAX_SCREENS];
extern int stellar_num_screens;
extern char staging_dir[PATH_MAX];
extern MonitorInfo monitor_info[MAX_SCREENS];
extern bool monitor_info_loaded;
extern int pending_xorg_change;
extern bool valid_installed_conf_exists;
extern bool valid_staging_conf_exists;
extern char active_installed_conf_path[PATH_MAX];

 
// Calculate auto DPI from native (pre-rotation) resolution and EDID physical width
int calc_auto_dpi(int native_width_px, int phys_width_mm) {
    if (native_width_px <= 0 || phys_width_mm <= 0) return 96;
    int raw = (int)(native_width_px * 25.4 / phys_width_mm);
    return snap_dpi_down(raw);
}
 
// Get the effective DPI for a screen (override or auto)
static int get_effective_dpi(int screen_idx) {
    ScreenConfig *sc = &state.screens[screen_idx];
    if (sc->dpi_override > 0) return sc->dpi_override;
    if (!monitor_info_loaded) return 96;
    MonitorInfo *mi = &monitor_info[screen_idx];
    return calc_auto_dpi(mi->width, mi->phys_width_mm);
}

// Helper function for the recursion
static void dfs_map_screen(int id, int vx, int vy, int *v_x_arr, int *v_y_arr, int *visited, int *min_x, int *min_y) {
    if (id == -1 || visited[id]) return; // Stop if no neighbor or already mapped

    visited[id] = 1;
    v_x_arr[id] = vx;
    v_y_arr[id] = vy;

    // Track the extreme top-left boundaries
    if (vx < *min_x) *min_x = vx;
    if (vy < *min_y) *min_y = vy;

    ScreenConfig *sc = &state.screens[id];

    // Recurse to all valid neighbors
    dfs_map_screen(sc->neighbor_up,    vx,     vy - 1, v_x_arr, v_y_arr, visited, min_x, min_y);
    dfs_map_screen(sc->neighbor_down,  vx,     vy + 1, v_x_arr, v_y_arr, visited, min_x, min_y);
    dfs_map_screen(sc->neighbor_left,  vx - 1, vy,     v_x_arr, v_y_arr, visited, min_x, min_y);
    dfs_map_screen(sc->neighbor_right, vx + 1, vy,     v_x_arr, v_y_arr, visited, min_x, min_y);
}

void convert_neighbors_to_grid(void) {
    // Initialize grid to empty (-1)
    for (int y = 0; y < MAX_SCREENS; y++) {
        for (int x = 0; x < MAX_SCREENS; x++) {
            screen_grid[y][x] = -1;
        }
    }

    if (stellar_num_screens <= 0) return;

    int v_x[MAX_SCREENS] = {0};
    int v_y[MAX_SCREENS] = {0};
    int visited[MAX_SCREENS] = {0};
    int min_x = 0;
    int min_y = 0;
    int current_offset_x = 0;

    // Map all screens to virtual coordinates
    // We loop in case the user's config has disjoint/disconnected screens
    for (int i = 0; i < stellar_num_screens; i++) {
        if (!visited[i]) {
            dfs_map_screen(i, current_offset_x, 0, v_x, v_y, visited, &min_x, &min_y);
            // If there's a disconnected monitor, place it safely to the right of everything else in the next loop
            current_offset_x += MAX_SCREENS; 
        }
    }

    // Shift virtual coordinates to positive indices and place in actual grid
    for (int i = 0; i < stellar_num_screens; i++) {
        int final_x = v_x[i] - min_x;
        int final_y = v_y[i] - min_y;
        
        // Safety bounds check
        if (final_x >= 0 && final_x < MAX_SCREENS && final_y >= 0 && final_y < MAX_SCREENS) {
            screen_grid[final_y][final_x] = i;
        }
    }
}

void shift_grid_to_origin(void) {
    int min_r = MAX_SCREENS;
    int min_c = MAX_SCREENS;

    // Find the top-most row and left-most column that actually contain a screen
    for (int r = 0; r < MAX_SCREENS; r++) {
        for (int c = 0; c < MAX_SCREENS; c++) {
            if (screen_grid[r][c] != -1) {
                if (r < min_r) min_r = r;
                if (c < min_c) min_c = c;
            }
        }
    }

    // If grid is empty or already at origin, do nothing
    if (min_r == MAX_SCREENS || (min_r == 0 && min_c == 0)) return;

    // Shift everything towards the top-left
    for (int r = 0; r < MAX_SCREENS; r++) {
        for (int c = 0; c < MAX_SCREENS; c++) {
            int src_r = r + min_r;
            int src_c = c + min_c;

            if (src_r < MAX_SCREENS && src_c < MAX_SCREENS) {
                screen_grid[r][c] = screen_grid[src_r][src_c];
            } else {
                screen_grid[r][c] = -1; // Clear old positions
            }
        }
    }
}

void convert_grid_to_neighbors(void) {
    // Reset all neighbors to -1
    for (int i = 0; i < stellar_num_screens; i++) {
        state.screens[i].neighbor_up    = -1;
        state.screens[i].neighbor_down  = -1;
        state.screens[i].neighbor_left  = -1;
        state.screens[i].neighbor_right = -1;
    }

    // Rebuild based on grid adjacency
    for (int y = 0; y < MAX_SCREENS; y++) {
        for (int x = 0; x < MAX_SCREENS; x++) {
            int id = screen_grid[y][x];
            
            if (id == -1) continue; // Empty grid slot

            ScreenConfig *sc = &state.screens[id];

            if (y > 0) {
                sc->neighbor_up = screen_grid[y - 1][x];
            }
            if (y < MAX_SCREENS - 1) {
                sc->neighbor_down = screen_grid[y + 1][x];
            }
            if (x > 0) {
                sc->neighbor_left = screen_grid[y][x - 1];
            }
            if (x < MAX_SCREENS - 1) {
                sc->neighbor_right = screen_grid[y][x + 1];
            }
        }
    }
}

// Returns 1 if all screens are connected, 0 if there are islands
static int check_contiguity(int test_grid[MAX_SCREENS][MAX_SCREENS]) {
    int visited[MAX_SCREENS][MAX_SCREENS] = {0};
    int start_x = -1, start_y = -1;
    int total_screens = 0;

    // Count screens and find a starting point
    for (int y = 0; y < MAX_SCREENS; y++) {
        for (int x = 0; x < MAX_SCREENS; x++) {
            if (test_grid[y][x] != -1) {
                total_screens++;
                if (start_x == -1) { start_x = x; start_y = y; }
            }
        }
    }
    
    if (total_screens <= 1) return 1;

    // Flood Fill (BFS)
    int qx[MAX_SCREENS * MAX_SCREENS];
    int qy[MAX_SCREENS * MAX_SCREENS];
    int head = 0, tail = 0, connected_count = 0;

    qx[tail] = start_x; 
    qy[tail++] = start_y;
    visited[start_y][start_x] = 1;

    int dx[] = {0, 0, -1, 1};
    int dy[] = {-1, 1, 0, 0};

    while (head < tail) {
        int cx = qx[head]; 
        int cy = qy[head++];
        connected_count++;

        for (int i = 0; i < 4; i++) {
            int nx = cx + dx[i];
            int ny = cy + dy[i];

            if (nx >= 0 && nx < MAX_SCREENS && ny >= 0 && ny < MAX_SCREENS) {
                if (test_grid[ny][nx] != -1 && !visited[ny][nx]) {
                    visited[ny][nx] = 1;
                    qx[tail] = nx; 
                    qy[tail++] = ny;
                }
            }
        }
    }

    return (connected_count == total_screens);
}

static void close_gap(int temp[MAX_SCREENS][MAX_SCREENS], int y, int x, int is_vertical_move) {
    if (is_vertical_move) {
        // If we left a hole in a row, shift the right side leftwards
        if ((x > 0 && temp[y][x-1] != -1) || (x < MAX_SCREENS - 1 && temp[y][x+1] != -1)) {
            for (int c = x; c < MAX_SCREENS - 1; c++) {
                temp[y][c] = temp[y][c + 1];
            }
            temp[y][MAX_SCREENS - 1] = -1;
        }
    } else {
        // If we left a hole in a column, shift the bottom side upwards
        if ((y > 0 && temp[y-1][x] != -1) || (y < MAX_SCREENS - 1 && temp[y+1][x] != -1)) {
            for (int r = y; r < MAX_SCREENS - 1; r++) {
                temp[r][x] = temp[r + 1][x];
            }
            temp[MAX_SCREENS - 1][x] = -1;
        }
    }
}

void try_move_screen(int y, int x, int dy, int dx) {
    int temp[MAX_SCREENS][MAX_SCREENS];
    memcpy(temp, screen_grid, sizeof(temp));

    int ty = y + dy;
    int tx = x + dx;

    if (ty < 0 || ty >= MAX_SCREENS || tx < 0 || tx >= MAX_SCREENS) return;

    int id = temp[y][x];
    int target_id = temp[ty][tx];

    // SCENARIO 1 & 2: Moving into an occupied slot
    if (target_id != -1) {
        int target_in_row = (tx > 0 && temp[ty][tx-1] != -1) || (tx < MAX_SCREENS-1 && temp[ty][tx+1] != -1);
        int target_in_col = (ty > 0 && temp[ty-1][tx] != -1) || (ty < MAX_SCREENS-1 && temp[ty+1][tx] != -1);

        if (dx != 0) { // Moving Horizontally
            if (target_in_col && !target_in_row) {
                // SPLICE: Perpendicular move into a column. Push the column right.
                temp[y][x] = -1;
                for (int c = MAX_SCREENS - 1; c > tx; c--) temp[ty][c] = temp[ty][c - 1];
                temp[ty][tx] = id;
                close_gap(temp, y, x, 0); 
            } else {
                // SWAP: Moving along a row (or into a dense cluster)
                temp[ty][tx] = id;
                temp[y][x] = target_id;
            }
        } else { // dy != 0 -> Moving Vertically
            if (target_in_row && !target_in_col) {
                // SPLICE: Perpendicular move into a row. Push the row down.
                temp[y][x] = -1;
                for (int r = MAX_SCREENS - 1; r > ty; r--) temp[r][tx] = temp[r - 1][tx];
                temp[ty][tx] = id;
                close_gap(temp, y, x, 1);
            } else {
                // SWAP: Moving along a column (or into a dense cluster)
                temp[ty][tx] = id;
                temp[y][x] = target_id;
            }
        }
    } 
    // SCENARIO 3: Moving into empty space
    else {
        temp[y][x] = -1;
        close_gap(temp, y, x, dy != 0);
        temp[ty][tx] = id;
        
        // SNAP LOGIC: If the move orphaned the screen, slide it along the axis until it touches
        if (!check_contiguity(temp)) {
            temp[ty][tx] = -1; // Pick it back up
            int snapped = 0;
            
            if (dy != 0) { // Moved vertically, slide horizontally to find connection
                for (int offset = 1; offset < MAX_SCREENS; offset++) {
                    if (tx - offset >= 0) {
                        temp[ty][tx - offset] = id;
                        if (check_contiguity(temp)) { snapped = 1; break; }
                        temp[ty][tx - offset] = -1;
                    }
                    if (tx + offset < MAX_SCREENS) {
                        temp[ty][tx + offset] = id;
                        if (check_contiguity(temp)) { snapped = 1; break; }
                        temp[ty][tx + offset] = -1;
                    }
                }
            } else { // Moved horizontally, slide vertically to find connection
                for (int offset = 1; offset < MAX_SCREENS; offset++) {
                    if (ty - offset >= 0) {
                        temp[ty - offset][tx] = id;
                        if (check_contiguity(temp)) { snapped = 1; break; }
                        temp[ty - offset][tx] = -1;
                    }
                    if (ty + offset < MAX_SCREENS) {
                        temp[ty + offset][tx] = id;
                        if (check_contiguity(temp)) { snapped = 1; break; }
                        temp[ty + offset][tx] = -1;
                    }
                }
            }
            if (!snapped) return; // Move is impossible, abort
        }
    }

    // Final Validation
    if (check_contiguity(temp)) {
        memcpy(screen_grid, temp, sizeof(temp));
        shift_grid_to_origin(); 
    }
}

static bool files_are_identical(const char *path1, const char *path2) {
    FILE *f1 = fopen(path1, "rb");
    FILE *f2 = fopen(path2, "rb");

    // If one file doesn't exist (e.g., first time running), they aren't identical
    if (!f1 || !f2) {
        if (f1) fclose(f1);
        if (f2) fclose(f2);
        return false;
    }

    // Check file sizes first for a quick out
    fseek(f1, 0, SEEK_END);
    fseek(f2, 0, SEEK_END);
    long size1 = ftell(f1);
    long size2 = ftell(f2);
    
    if (size1 != size2) {
        fclose(f1);
        fclose(f2);
        return false;
    }

    // Compare contents chunk by chunk
    fseek(f1, 0, SEEK_SET);
    fseek(f2, 0, SEEK_SET);
    
    char buf1[4096];
    char buf2[4096];
    size_t bytes_read;
    bool identical = true;

    while ((bytes_read = fread(buf1, 1, sizeof(buf1), f1)) > 0) {
        fread(buf2, 1, bytes_read, f2);
        if (memcmp(buf1, buf2, bytes_read) != 0) {
            identical = false;
            break;
        }
    }

    fclose(f1);
    fclose(f2);
    return identical;
}

// Map a cache output name to a live screen index, if that output is currently
// an active ZaphodHeads screen. Returns -1 when the monitor is connected but
// inactive (the bootstrap case) - callers then fall back to cache data and
// per-screen defaults rather than indexing monitor_info[]/state.screens[].
static int live_screen_index_for_output(const char *out_name) {
    if (!monitor_info_loaded) return -1;
    for (int s = 0; s < stellar_num_screens && s < MAX_SCREENS; s++) {
        if (monitor_info[s].connected &&
            strcmp(monitor_info[s].output_name, out_name) == 0) {
            return s;
        }
    }
    return -1;
}

// Extract the "structural skeleton" of an xorg.conf: the lines that define the
// SET of screens and their device bindings, ignoring anything a live xrandr
// call can change (Rotate, PreferredMode, DisplaySize, and the per-screen
// layout Y offsets that shift when rotation changes the stacking height).
// Two configs with identical skeletons differ only in live-applicable ways.
// Returns the number of skeleton lines, written into `out` (each NUL-terminated,
// up to max_lines entries of skel_line_len).
#define SKEL_MAX_LINES 256
#define SKEL_LINE_LEN  512
static int extract_xorg_skeleton(const char *path,
                                 char out[SKEL_MAX_LINES][SKEL_LINE_LEN]) {
    FILE *f = fopen(path, "r");
    if (!f) return -1;

    int n = 0;
    char line[512];
    while (fgets(line, sizeof(line), f) && n < SKEL_MAX_LINES) {
        // Trim leading whitespace.
        char *p = line;
        while (*p == ' ' || *p == '\t') p++;
        // Trim trailing whitespace/newline.
        size_t len = strlen(p);
        while (len > 0 && (p[len-1] == '\n' || p[len-1] == '\r' ||
                           p[len-1] == ' '  || p[len-1] == '\t')) {
            p[--len] = '\0';
        }
        if (len == 0) continue;

        // Skip lines that represent live-applicable (non-structural) state.
        if (strstr(p, "Option") && strstr(p, "Rotate"))        continue;
        if (strstr(p, "Option") && strstr(p, "PreferredMode")) continue;
        if (strncmp(p, "DisplaySize", 11) == 0)                continue;
        // The ServerLayout per-screen line carries a Y offset that moves with
        // rotation; keep the screen identity but drop the coordinates.
        if (strncmp(p, "Screen", 6) == 0 && strstr(p, "\"Screen")) {
            // Normalize "Screen N "ScreenN" X Y" -> "Screen "ScreenN"" so a pure
            // offset shift doesn't read as structural, but adding/removing a
            // screen still does.
            char *q = strstr(p, "\"Screen");
            char norm[SKEL_LINE_LEN];
            char ident[64] = "";
            // Pull the quoted identifier.
            char *start = q + 1;
            char *endq = strchr(start, '"');
            if (endq) {
                size_t idlen = (size_t)(endq - start);
                if (idlen >= sizeof(ident)) idlen = sizeof(ident) - 1;
                memcpy(ident, start, idlen);
                ident[idlen] = '\0';
            }
            snprintf(norm, sizeof(norm), "ScreenRef \"%s\"", ident);
            snprintf(out[n], SKEL_LINE_LEN, "%s", norm);
            n++;
            continue;
        }

        snprintf(out[n], SKEL_LINE_LEN, "%s", p);
        n++;
    }
    fclose(f);
    return n;
}

void init_staging_dir(void) {
    char base_dir[PATH_MAX];
    const char *home = get_user_home_dir();

    // Ensure ~/.cache/stellar exists
    snprintf(base_dir, sizeof(base_dir), "%s/.cache/stellar", home);
    struct stat st = {0};
    if (stat(base_dir, &st) == -1) {
        mkdir(base_dir, 0755);
    }

    // Set the global staging_dir and ensure the xorg dir exists
    snprintf(staging_dir, sizeof(staging_dir), "%s/xorg", base_dir);
    if (stat(staging_dir, &st) == -1) {
        mkdir(staging_dir, 0755);
    }
}

// Classify how `staging` differs from `installed`.
static XorgChangeClass classify_xorg_changes(const char *staging,
                                             const char *installed) {
    // Byte-identical => no change at all.
    if (files_are_identical(staging, installed)) return XORG_CHANGE_NONE;

    static char skel_a[SKEL_MAX_LINES][SKEL_LINE_LEN];
    static char skel_b[SKEL_MAX_LINES][SKEL_LINE_LEN];

    int na = extract_xorg_skeleton(staging, skel_a);
    int nb = extract_xorg_skeleton(installed, skel_b);

    // If we can't read one of them (e.g. no installed file yet), treat as
    // structural - first-time install needs a restart.
    if (na < 0 || nb < 0) return XORG_CHANGE_STRUCTURAL;

    if (na != nb) return XORG_CHANGE_STRUCTURAL;
    for (int i = 0; i < na; i++) {
        if (strcmp(skel_a[i], skel_b[i]) != 0)
            return XORG_CHANGE_STRUCTURAL;
    }

    // Files differ but skeletons match => only Rotate/PreferredMode/DisplaySize/
    // offsets changed => live-applicable.
    return XORG_CHANGE_LIVE;
}

static void generate_xorg_conf(void) {
    // Source of truth is the hardware cache, not the live DE screen list: that
    // is what lets connected-but-inactive monitors (invisible to a ZaphodHeads
    // server until they're assigned a screen) get laid out at all. Live monitor
    // info, when present, is used only as an overlay for current resolution.
    char *data = NULL;
    {
        FILE *f = fopen("/var/cache/stellar/hardware.json", "r");
        if (!f) {
            printf("Error: hardware.json not found. Run hardware probe first.\n");
            return;
        }
        fseek(f, 0, SEEK_END);
        long length = ftell(f);
        if (length < 0) { fclose(f); return; }
        fseek(f, 0, SEEK_SET);
        data = malloc(length + 1);
        if (!data) { fclose(f); return; }
        size_t got = fread(data, 1, length, f);
        fclose(f);
        data[got] = '\0';
    }

    cJSON *json = cJSON_Parse(data);
    free(data);
    if (!json) return;

    cJSON *cards = cJSON_GetObjectItemCaseSensitive(json, "cards");
    if (!cards) {
        cJSON_Delete(json);
        return;
    }

    char conf_path[512];
    snprintf(conf_path, sizeof(conf_path), "%s/10-stellar-video.conf", staging_dir);
    FILE *out = fopen(conf_path, "w");
    if (!out) {
        printf("Error: Could not open %s for writing\n", conf_path);
        cJSON_Delete(json);
        return;
    }

    // Buffers to hold the different sections before we write them
    char server_layout[8192] = "Section \"ServerLayout\"\n    Identifier  \"StellarLayout\"\n";
    char monitor_sections[8192] = "";
    char device_sections[8192] = "";
    char screen_sections[8192] = "";

    int global_screen_idx = 0;
    int current_y_offset = 0;   // vertical stacking; screens isolated with a gap
                                // so the DE controls all crossing via pointer warps

    // Iterate through up to 16 potential GPUs
    for (int card_id = 0; card_id < 16; card_id++) {
        char key[16];
        snprintf(key, sizeof(key), "%d", card_id);
        cJSON *card = cJSON_GetObjectItemCaseSensitive(cards, key);
        if (!card) continue;

        cJSON *driver = cJSON_GetObjectItemCaseSensitive(card, "driver");
        cJSON *bus_id = cJSON_GetObjectItemCaseSensitive(card, "bus_id");
        cJSON *outputs = cJSON_GetObjectItemCaseSensitive(card, "outputs");

        if (!cJSON_IsString(driver) || !cJSON_IsString(bus_id) || !cJSON_IsArray(outputs)) continue;

        int head_idx = 0;
        cJSON *output = NULL;

        // Iterate through all physical ports on this GPU
        cJSON_ArrayForEach(output, outputs) {
            // New schema: each output is an object {name, connected, monitor_name,
            // phys_width_mm, phys_height_mm, edid, modes, preferred_mode}.
            if (!cJSON_IsObject(output)) continue;

            cJSON *j_name      = cJSON_GetObjectItemCaseSensitive(output, "name");
            cJSON *j_connected = cJSON_GetObjectItemCaseSensitive(output, "connected");
            if (!cJSON_IsString(j_name)) continue;

            // Only lay out ports with a monitor actually connected. This is read
            // from the cache (probed in full non-Zaphod mode), so it correctly
            // includes monitors that are currently inactive in the live server.
            if (!cJSON_IsBool(j_connected) || !cJSON_IsTrue(j_connected)) continue;

            const char *out_name = j_name->valuestring;

            // Cache-derived monitor facts (available even when the monitor is
            // inactive and thus absent from monitor_info[]).
            cJSON *j_mon_name = cJSON_GetObjectItemCaseSensitive(output, "monitor_name");
            cJSON *j_pref     = cJSON_GetObjectItemCaseSensitive(output, "preferred_mode");

            const char *monitor_id =
                (cJSON_IsString(j_mon_name) && j_mon_name->valuestring[0])
                    ? j_mon_name->valuestring : out_name;

            // Resolution: prefer the cache's preferred_mode; fall back to the
            // live active resolution if this output is a current screen; else a
            // safe default so layout math still works.
            int res_w = 0, res_h = 0;
            if (cJSON_IsObject(j_pref)) {
                cJSON *pw = cJSON_GetObjectItemCaseSensitive(j_pref, "w");
                cJSON *ph = cJSON_GetObjectItemCaseSensitive(j_pref, "h");
                if (cJSON_IsNumber(pw) && cJSON_IsNumber(ph)) {
                    res_w = pw->valueint;
                    res_h = ph->valueint;
                }
            }

            // Is this output currently a live screen? If so we can use its
            // user-configured settings and live resolution.
            int live_idx = live_screen_index_for_output(out_name);

            if ((res_w <= 0 || res_h <= 0) && live_idx >= 0) {
                res_w = monitor_info[live_idx].width;
                res_h = monitor_info[live_idx].height;
            }
            if (res_w <= 0 || res_h <= 0) { res_w = 1920; res_h = 1080; }

            // Per-screen user settings: only meaningful for a live screen index.
            // Inactive monitors get defaults; the user edits them after the
            // layout is generated and the screen becomes active on restart.
            const char *rot = "";
            const char *pref_mode_setting = "";
            int dpi = 96;
            int phys_w_mm = 0;

            cJSON *j_phys_w = cJSON_GetObjectItemCaseSensitive(output, "phys_width_mm");
            if (cJSON_IsNumber(j_phys_w)) phys_w_mm = j_phys_w->valueint;

            if (live_idx >= 0) {
                rot = state.screens[live_idx].rotation;
                pref_mode_setting = state.screens[live_idx].preferred_mode;
                dpi = get_effective_dpi(live_idx);
            } else {
                // DPI from cache phys size + preferred resolution.
                if (state.screens[global_screen_idx % MAX_SCREENS].dpi_override > 0)
                    dpi = state.screens[global_screen_idx % MAX_SCREENS].dpi_override;
                else
                    dpi = calc_auto_dpi(res_w, phys_w_mm);
            }

            // --- ServerLayout Section (vertical stacking) ---
            char layout_buf[128];
            snprintf(layout_buf, sizeof(layout_buf), "    Screen      %d \"Screen%d\" 0 %d\n",
                     global_screen_idx, global_screen_idx, current_y_offset);
            strcat(server_layout, layout_buf);

            // Advance y by this monitor's (post-rotation) height plus a gap. The
            // gap keeps screens non-adjacent so the bare X server never warps the
            // pointer across on its own - the DE owns all screen crossing.
            bool swapped_layout = (rot[0] != '\0' &&
                (strcmp(rot, "left") == 0 || strcmp(rot, "right") == 0));
            current_y_offset += (swapped_layout ? res_w : res_h) + 40;

            // --- Monitor Section ---
            char mon_buf[1024];
            char mon_opts[512] = "";
            int opt_pos = 0;

            if (rot[0] != '\0' && strcmp(rot, "normal") != 0) {
                opt_pos += snprintf(mon_opts + opt_pos, sizeof(mon_opts) - opt_pos,
                    "    Option      \"Rotate\" \"%s\"\n", rot);
            }
            if (pref_mode_setting[0] != '\0') {
                opt_pos += snprintf(mon_opts + opt_pos, sizeof(mon_opts) - opt_pos,
                    "    Option      \"PreferredMode\" \"%s\"\n", pref_mode_setting);
            }

            // DisplaySize to force the desired DPI. X computes
            // DPI = post_rotation_pixels / (DisplaySize_mm / 25.4) and does NOT
            // swap DisplaySize for rotation, so use post-rotation resolution.
            {
                int eff_w = swapped_layout ? res_h : res_w;
                int eff_h = swapped_layout ? res_w : res_h;
                if (eff_w > 0 && eff_h > 0 && dpi > 0) {
                    int ds_w = (int)round(eff_w * 25.4 / dpi);
                    int ds_h = (int)round(eff_h * 25.4 / dpi);
                    opt_pos += snprintf(mon_opts + opt_pos, sizeof(mon_opts) - opt_pos,
                        "    DisplaySize %d %d\n", ds_w, ds_h);
                }
            }

            snprintf(mon_buf, sizeof(mon_buf),
                "Section \"Monitor\"\n"
                "    Identifier  \"%s\"\n"
                "%s"
                "EndSection\n\n", monitor_id, mon_opts);
            strcat(monitor_sections, mon_buf);

            // --- Device Section (ZaphodHeads mapping) ---
            char dev_buf[512];
            snprintf(dev_buf, sizeof(dev_buf),
                "Section \"Device\"\n"
                "    Identifier  \"Card%d-Head%d\"\n"
                "    Driver      \"%s\"\n"
                "    BusID       \"%s\"\n"
                "    Screen      %d\n"
                "    Option      \"ZaphodHeads\" \"%s\"\n"
                "    Option      \"Xinerama\" \"FALSE\"\n"
                "    Option      \"AsyncFlipSecondaries\" \"True\"\n"
                "EndSection\n\n",
                card_id, head_idx, driver->valuestring, bus_id->valuestring, head_idx, out_name);
            strcat(device_sections, dev_buf);

            // --- Screen Section ---
            char scr_buf[512];
            snprintf(scr_buf, sizeof(scr_buf),
                "Section \"Screen\"\n"
                "    Identifier \"Screen%d\"\n"
                "    Device     \"Card%d-Head%d\"\n"
                "    Monitor    \"%s\"\n"
                "    SubSection \"Display\"\n"
                "        Depth     24\n"
                "    EndSubSection\n"
                "EndSection\n\n",
                global_screen_idx, card_id, head_idx, monitor_id);
            strcat(screen_sections, scr_buf);

            head_idx++;
            global_screen_idx++;
        }
    }
    strcat(server_layout, "EndSection\n\n");

    // Print all assembled blocks to the file
    fprintf(out, "%s", server_layout);
    fprintf(out, "%s", monitor_sections);
    fprintf(out, "%s", device_sections);
    fprintf(out, "%s", screen_sections);

    fclose(out);
    cJSON_Delete(json);

    printf("Generated Xorg layout written to: %s (%d screen%s)\n",
           conf_path, global_screen_idx, global_screen_idx == 1 ? "" : "s");
}

// Helper: Opens a file and verifies it contains the structural minimums for Stellar
static bool validate_stellar_conf_file(const char *filepath) {
    FILE *f = fopen(filepath, "r");
    if (!f) return false;

    bool has_server_layout = false;
    int monitor_count = 0;
    int device_count = 0;

    char line[512];
    while (fgets(line, sizeof(line), f)) {
        // Trim leading whitespace to reliably catch the section declarations
        char *p = line;
        while (*p == ' ' || *p == '\t') p++;

        if (strncmp(p, "Section", 7) == 0) {
            if (strstr(p, "\"ServerLayout\"")) has_server_layout = true;
            else if (strstr(p, "\"Monitor\"")) monitor_count++;
            else if (strstr(p, "\"Device\"")) device_count++;
        }

        // Fast exit if we've already satisfied the requirements
        if (has_server_layout && monitor_count > 0 && device_count > 0) {
            fclose(f);
            return true;
        }
    }

    fclose(f);
    return false; // Reached EOF without satisfying all requirements
}

// Main function to discover and validate configurations
void validate_stellar_xorg_conf_files(void) {
    valid_installed_conf_exists = false;
    valid_staging_conf_exists = false;
    active_installed_conf_path[0] = '\0';

    glob_t glob_result;

    // 1. Search Standard X11 System Locations
    // This allows the admin to install the file in either the user-override or package-default directory.
    const char *system_search_patterns[] = {
        "/etc/X11/xorg.conf.d/*-stellar-video.conf",
        "/usr/share/X11/xorg.conf.d/*-stellar-video.conf"
    };

    for (size_t i = 0; i < sizeof(system_search_patterns)/sizeof(system_search_patterns[0]); i++) {
        if (glob(system_search_patterns[i], GLOB_NOSORT, NULL, &glob_result) == 0) {
            for (size_t j = 0; j < glob_result.gl_pathc; j++) {
                if (validate_stellar_conf_file(glob_result.gl_pathv[j])) {
                    valid_installed_conf_exists = true;
                    snprintf(active_installed_conf_path, sizeof(active_installed_conf_path), 
                             "%s", glob_result.gl_pathv[j]);
                    break; // Stop searching system paths once a valid one is found
                }
            }
        }
        globfree(&glob_result);
        if (valid_installed_conf_exists) break;
    }

    // 2. Search Dynamic Staging Location
    // Uses staging_dir variable.
    char staging_pattern[PATH_MAX + 32];
    snprintf(staging_pattern, sizeof(staging_pattern), "%s/*-stellar-video.conf", staging_dir);

    if (glob(staging_pattern, GLOB_NOSORT, NULL, &glob_result) == 0) {
        for (size_t j = 0; j < glob_result.gl_pathc; j++) {
            if (validate_stellar_conf_file(glob_result.gl_pathv[j])) {
                valid_staging_conf_exists = true;
                break; // Stop searching staging once a valid one is found
            }
        }
    }
    globfree(&glob_result);
}

// Regenerate the prospective xorg.conf into staging and classify how it differs
// from what's installed, updating pending_xorg_change. Safe to call any time;
// it only writes to the staging path, never the live system file. Call at
// startup (to catch a dirty leftover-settings state) and after any edit that
// can affect the generated config (rotation, preferred mode, topology, probe).
void refresh_pending_xorg_change(void) {
    convert_grid_to_neighbors();   // make sure neighbor topology is current
    generate_xorg_conf();          // writes staging file from current settings

    char staging_file[PATH_MAX];
    snprintf(staging_file, sizeof(staging_file), "%s/10-stellar-video.conf", staging_dir);
    
    const char *system_file  = "/etc/X11/xorg.conf.d/10-stellar-video.conf";

    pending_xorg_change = classify_xorg_changes(staging_file, system_file);
}

// --- UI Layout ---
// Writes the staging xorg.conf to the system path via the privileged helper,
// but only when it actually differs from what's installed. Classifies the
// change first so the caller knows whether a restart is needed:
//   - STRUCTURAL change -> *out_needs_restart = true (new screen set/topology)
//   - LIVE change       -> file written for next boot (LightDM etc.), but no
//                          restart; the live session keeps working and the DE
//                          can apply rotation/mode via xrandr immediately
//   - NONE              -> nothing written
// Always refreshes pending_xorg_change afterward so the banner clears on success.
void apply_display_config(bool *out_needs_restart) {
    if (out_needs_restart) *out_needs_restart = false;

    convert_grid_to_neighbors();
    generate_xorg_conf();

    char staging_file[PATH_MAX];
    snprintf(staging_file, sizeof(staging_file), "%s/10-stellar-video.conf", staging_dir);
    
    const char *system_file  = "/etc/X11/xorg.conf.d/10-stellar-video.conf";

    XorgChangeClass cls = classify_xorg_changes(staging_file, system_file);

    if (cls == XORG_CHANGE_NONE) {
        printf("No changes to Xorg config detected. Skipping Polkit elevation.\n");
        pending_xorg_change = XORG_CHANGE_NONE;
        return;
    }

    char helper_cmd[PATH_MAX * 2 + 128]; // Ensure buffer is large enough for both paths
    snprintf(helper_cmd, sizeof(helper_cmd),
             "pkexec %s/stellar-admin-helper --apply-display %s %s",
             STELLAR_LIBEXEC_PATH, staging_file, system_file);

    int ret = system(helper_cmd);
    if (ret != 0) {
        printf("Polkit elevation failed or was canceled.\n");
        // Leave pending_xorg_change as-is so the banner persists.
        return;
    }

    printf("Display configuration applied successfully.\n");

    if (cls == XORG_CHANGE_STRUCTURAL) {
        if (out_needs_restart) *out_needs_restart = true;
    }
    // LIVE changes: file is written for next boot; the live rotation/mode is
    // already in effect (applied via the DE's xrandr path), so no restart.

    // Recompute against the now-updated system file; should be NONE on success.
    refresh_pending_xorg_change();
}

void remove_installed_display_config(void) {
    if (active_installed_conf_path[0] == '\0') return;

    char cmd[PATH_MAX + 128];
    snprintf(cmd, sizeof(cmd), "pkexec rm -f %s", active_installed_conf_path);
    
    if (system(cmd) == 0) {
        printf("Successfully removed installed X11 configuration.\n");
    } else {
        printf("Polkit elevation failed or was canceled during removal.\n");
    }
}

// Uses Polkit to copy the staging file into the live system
void restore_staging_display_config(void) {
    char staging_file[PATH_MAX];
    snprintf(staging_file, sizeof(staging_file), "%s/10-stellar-video.conf", staging_dir);
    
    const char *system_file = "/etc/X11/xorg.conf.d/10-stellar-video.conf";

    char cmd[PATH_MAX * 2 + 128];
    snprintf(cmd, sizeof(cmd), "pkexec %s/stellar-admin-helper --apply-display %s %s",
             STELLAR_LIBEXEC_PATH, staging_file, system_file);
             
    if (system(cmd) == 0) {
        printf("Successfully restored staging X11 configuration.\n");
    } else {
        printf("Polkit elevation failed or was canceled during restore.\n");
    }
}

