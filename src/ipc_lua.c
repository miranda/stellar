// ipc_lua.c

#include "stellar.h"
#include "stellar_font.h"
#include "ipc_lua.h"
#include "monitor.h"
#include "xdg_menu.h"

static void trim_newlines(char *s) {
    size_t n = strlen(s);
    while (n > 0 && (s[n - 1] == '\n' || s[n - 1] == '\r')) {
        s[n - 1] = '\0';
        n--;
    }
}

int make_server_socket(StellarState *st) {
    if (ensure_runtime_dir_and_socket_path(st) != 0) {
        return -1;
    }

    unlink(st->socket_path);

    int fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (fd < 0) {
        log_error("socket failed: %s", strerror(errno));
        return -1;
    }

    struct sockaddr_un addr;
    memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;

    if (strlen(st->socket_path) >= sizeof(addr.sun_path)) {
        log_error("socket path too long: %s", st->socket_path);
        close(fd);
        return -1;
    }

    strncpy(addr.sun_path, st->socket_path, sizeof(addr.sun_path) - 1);

    if (bind(fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        log_error("bind(%s) failed: %s", st->socket_path, strerror(errno));
        close(fd);
        return -1;
    }

    if (listen(fd, 16) < 0) {
        log_error("listen failed: %s", strerror(errno));
        close(fd);
        unlink(st->socket_path);
        return -1;
    }

    log_info("IPC socket listening at %s", st->socket_path);
    return fd;
}

void accept_client(StellarState *st) {
    int fd = accept(st->server_fd, NULL, NULL);
    if (fd < 0) {
        log_error("accept failed: %s", strerror(errno));
        return;
    }

    if (st->client_count >= MAX_CLIENTS) {
        log_error("too many IPC clients; rejecting fd=%d", fd);
        close(fd);
        return;
    }

    IpcClient *c = &st->clients[st->client_count++];
    memset(c, 0, sizeof(*c));
    c->fd = fd;
    c->screen_num = -1;
    c->pid = 0;
    c->input_len = 0;

    log_info("IPC client connected fd=%d", fd);
}

void remove_client(StellarState *st, int idx) {
    if (idx < 0 || idx >= st->client_count) {
        return;
    }

    int fd = st->clients[idx].fd;
    bool was_awesome = st->clients[idx].is_awesome;
    int screen_num = st->clients[idx].screen_num;

    close(fd);

    if (was_awesome && screen_num >= 0 && screen_num < st->config.screen_count) {
        if (st->screens[screen_num].awesome_fd == fd) {
            st->screens[screen_num].awesome_fd = -1;
        }
    }

    // Clear any pending theme request targeting this fd
    for (int i = 0; i < st->config.screen_count; i++) {
        if (st->screens[i].pending_theme_request_fd == fd) {
            st->screens[i].pending_theme_request_fd = -1;
            log_info("Cleared pending theme request for screen %d (fd=%d disconnected)", i, fd);
        }
    }

    for (int i = idx; i < st->client_count - 1; i++) {
        st->clients[i] = st->clients[i + 1];
    }
    st->client_count--;

    log_info("IPC client fd=%d disconnected", fd);
}

void broadcast_line(StellarState *st, const char *line) {
    size_t len = strlen(line);

    for (int i = 0; i < st->client_count; i++) {
        ssize_t wr1 = write(st->clients[i].fd, line, len);
        ssize_t wr2 = write(st->clients[i].fd, "\n", 1);
        (void)wr1;
        (void)wr2;
    }
}

static void send_line_to_screen(
    StellarState *st,
    int screen_num,
    const char *line
) {
    if (screen_num < 0 || screen_num >= st->config.screen_count) {
        return;
    }

    int fd = st->screens[screen_num].awesome_fd;
    if (fd < 0) {
        return;
    }

    ssize_t wr1 = write(fd, line, strlen(line));
    ssize_t wr2 = write(fd, "\n", 1);
    (void)wr1;
    (void)wr2;
}

/* ---------- Lua Bridge ---------- */

static int l_send_to_screen(lua_State *L) {
    int screen_num = (int)luaL_checkinteger(L, 1);
    const char *line = luaL_checkstring(L, 2);

    if (!g_state) {
        return 0;
    }

    send_line_to_screen(g_state, screen_num, line);
    return 0;
}

static int l_broadcast(lua_State *L) {
    const char *line = luaL_checkstring(L, 1);

    if (!g_state) {
        return 0;
    }

    broadcast_line(g_state, line);
    return 0;
}

static int l_get_screen_count(lua_State *L) {
    if (!g_state) {
        lua_pushinteger(L, 0);
        return 1;
    }

    lua_pushinteger(L, g_state->config.screen_count);
    return 1;
}

static int l_log(lua_State *L) {
    const char *msg = luaL_checkstring(L, 1);
    log_info("lua: %s", msg);
    return 0;
}

bool check_lua_module_available(const char *module_name) {
    lua_State *L = luaL_newstate();
    if (!L) {
        log_error("luaL_newstate failed during dependency check");
        return false;
    }

    luaL_openlibs(L);

    lua_getglobal(L, "require");
    lua_pushstring(L, module_name);

    if (lua_pcall(L, 1, 1, 0) != LUA_OK) {
        const char *err = lua_tostring(L, -1);
        log_error(
            "required Lua module '%s' is unavailable: %s",
            module_name,
            err ? err : "unknown error"
        );
        lua_pop(L, 1);
        lua_close(L);
        return false;
    }

    lua_pop(L, 1);
    lua_close(L);
    return true;
}

bool init_lua(StellarState *st) {
    st->L = luaL_newstate();
    if (!st->L) {
        log_error("luaL_newstate failed");
        return false;
    }

    luaL_openlibs(st->L);

    lua_newtable(st->L);

    lua_pushcfunction(st->L, l_send_to_screen);
    lua_setfield(st->L, -2, "send_to_screen");

    lua_pushcfunction(st->L, l_broadcast);
    lua_setfield(st->L, -2, "broadcast");

    lua_pushcfunction(st->L, l_get_screen_count);
    lua_setfield(st->L, -2, "screen_count");

    lua_pushcfunction(st->L, l_log);
    lua_setfield(st->L, -2, "log");

    lua_setglobal(st->L, "Stellar");

    char lua_path[PATH_MAX];
    snprintf(
        lua_path,
        sizeof(lua_path),
        "%s/lua/stellar.lua",
        STELLAR_SHARE_PATH
    );

    if (luaL_dofile(st->L, lua_path) != LUA_OK) {
        log_error(
            "failed to load Lua file %s: %s",
            lua_path,
            lua_tostring(st->L, -1)
        );
        lua_pop(st->L, 1);
        return false;
    }

    log_info("Lua loaded: %s", lua_path);
    return true;
}

void lua_call_noargs(StellarState *st, const char *func) {
    lua_getglobal(st->L, func);
    if (!lua_isfunction(st->L, -1)) {
        lua_pop(st->L, 1);
        return;
    }

    if (lua_pcall(st->L, 0, 0, 0) != LUA_OK) {
        log_error("Lua error in %s: %s", func, lua_tostring(st->L, -1));
        lua_pop(st->L, 1);
    }
}

void lua_on_pointer_screen_change(
    StellarState *st,
    int old_screen,
    int new_screen
) {
    lua_getglobal(st->L, "on_pointer_screen_change");
    if (!lua_isfunction(st->L, -1)) {
        lua_pop(st->L, 1);
        return;
    }

    lua_pushinteger(st->L, old_screen);
    lua_pushinteger(st->L, new_screen);

    if (lua_pcall(st->L, 2, 0, 0) != LUA_OK) {
        log_error(
            "Lua error in on_pointer_screen_change: %s",
            lua_tostring(st->L, -1)
        );
        lua_pop(st->L, 1);
    }
}

static void lua_handle_ipc_line(
    StellarState *st,
    int client_fd,
    const char *line
) {
    lua_getglobal(st->L, "handle_ipc_line");
    if (!lua_isfunction(st->L, -1)) {
        lua_pop(st->L, 1);
        return;
    }

    lua_pushinteger(st->L, client_fd);
    lua_pushstring(st->L, line);

    if (lua_pcall(st->L, 2, 0, 0) != LUA_OK) {
        log_error("Lua error in handle_ipc_line: %s", lua_tostring(st->L, -1));
        lua_pop(st->L, 1);
    }
}

void parse_settings_from_json(StellarState *st) {
    char config_path[PATH_MAX];
    snprintf(config_path, sizeof(config_path), "%s/.config/stellar/settings.json", getenv("HOME"));

    // --- Call the JSON parsing function in stellar.lua ---
    lua_getglobal(st->L, "parse_json_file");
    lua_pushstring(st->L, config_path);

    if (lua_pcall(st->L, 1, 1, 0) != LUA_OK) {
        log_error("Lua config error: %s", lua_tostring(st->L, -1));
        lua_pop(st->L, 1);
        return;
    }

    if (lua_isnil(st->L, -1)) {
        lua_pop(st->L, 1);
        return;
    }

    // --- Global Appearance Settings ---
	lua_getfield(st->L, -1, "appearance");
	if (lua_istable(st->L, -1)) {
		lua_getfield(st->L, -1, "stellar_theme");
		if (lua_isstring(st->L, -1)) {
			const char *theme = lua_tostring(st->L, -1);
			snprintf(
				st->config.stellar_theme,
				sizeof(st->config.stellar_theme),
				"%s",
				theme
			);
		}
		lua_pop(st->L, 1);

		lua_getfield(st->L, -1, "cursor_theme");
		if (lua_isstring(st->L, -1)) {
			const char *theme = lua_tostring(st->L, -1);
			snprintf(
				st->config.appearance.cursor_theme,
				sizeof(st->config.appearance.cursor_theme),
				"%s",
				theme
			);
		}
		lua_pop(st->L, 1);

		lua_getfield(st->L, -1, "cursor_size");
		if (lua_isnumber(st->L, -1)) {
			st->config.appearance.cursor_size = lua_tointeger(st->L, -1);
			if (st->config.appearance.cursor_size < 1) {
				st->config.appearance.cursor_size = 16;
			}
		}
		lua_pop(st->L, 1);

		lua_getfield(st->L, -1, "font_name");
		if (lua_isstring(st->L, -1)) {
			const char *fname = lua_tostring(st->L, -1);
			snprintf(
				st->config.appearance.font_name,
				sizeof(st->config.appearance.font_name),
				"%s",
				fname
			);
		}
		lua_pop(st->L, 1);

		lua_getfield(st->L, -1, "font_size");
		if (lua_isnumber(st->L, -1)) {
			st->config.appearance.font_size = (float)lua_tonumber(st->L, -1);
			if (st->config.appearance.font_size <= 0) {
				st->config.appearance.font_size = 14.0f;
			}
		}
		lua_pop(st->L, 1);

		lua_getfield(st->L, -1, "font_unit");
		if (lua_isstring(st->L, -1)) {
			const char *unit = lua_tostring(st->L, -1);
			snprintf(
				st->config.appearance.font_unit,
				sizeof(st->config.appearance.font_unit),
				"%s",
				unit
			);
		}
		lua_pop(st->L, 1);
	}
	lua_pop(st->L, 1);

    // --- Extract Global Power Settings ---
    lua_getfield(st->L, -1, "power");
    if (lua_istable(st->L, -1)) {
        lua_getfield(st->L, -1, "screensaver_enabled");
        if (lua_isboolean(st->L, -1)) st->config.saver_enabled = lua_toboolean(st->L, -1);
        lua_pop(st->L, 1);

        lua_getfield(st->L, -1, "timeout_screensaver");
        if (lua_isnumber(st->L, -1)) st->config.power.timeout_screensaver = lua_tointeger(st->L, -1);
        lua_pop(st->L, 1);

        lua_getfield(st->L, -1, "timeout_dpms");
        if (lua_isnumber(st->L, -1)) st->config.power.timeout_dpms = lua_tointeger(st->L, -1);
        lua_pop(st->L, 1);
    }
    lua_pop(st->L, 1);

    // --- Extract Terminal Settings ---
	lua_getfield(st->L, -1, "terminal");
	if (lua_istable(st->L, -1)) {
		lua_getfield(st->L, -1, "shell");
		if (lua_isstring(st->L, -1)) {
			const char *shell = lua_tostring(st->L, -1);
			snprintf(
				st->config.term_shell,
				sizeof(st->config.term_shell),
				"%s",
				shell
			);
		}
		lua_pop(st->L, 1);

		lua_getfield(st->L, -1, "terminal_gui");
		if (lua_isstring(st->L, -1)) {
			const char *term_gui = lua_tostring(st->L, -1);
			snprintf(
				st->config.term_gui,
				sizeof(st->config.term_gui),
				"%s",
				term_gui
			);
		}
		lua_pop(st->L, 1);
    }
    lua_pop(st->L, 1);

    // --- Extract Per-Screen Settings ---
    lua_getfield(st->L, -1, "screens");
    if (lua_istable(st->L, -1)) {
        for (int i = 0; i < st->config.screen_count; i++) {
            ScreenState *sc = &st->screens[i];

            sc->config.neighbor_left = -1;
            sc->config.neighbor_right = -1;
            sc->config.neighbor_up = -1;
            sc->config.neighbor_down = -1;

            char screen_key[16];
            snprintf(screen_key, sizeof(screen_key), "%d", i);

            lua_getfield(st->L, -1, screen_key);
            if (lua_istable(st->L, -1)) {

                lua_getfield(st->L, -1, "neighbors");
                if (lua_istable(st->L, -1)) {
                    lua_getfield(st->L, -1, "left");
                    if (lua_isnumber(st->L, -1)) sc->config.neighbor_left = lua_tointeger(st->L, -1);
                    lua_pop(st->L, 1);

                    lua_getfield(st->L, -1, "right");
                    if (lua_isnumber(st->L, -1)) sc->config.neighbor_right = lua_tointeger(st->L, -1);
                    lua_pop(st->L, 1);

                    lua_getfield(st->L, -1, "up");
                    if (lua_isnumber(st->L, -1)) sc->config.neighbor_up = lua_tointeger(st->L, -1);
                    lua_pop(st->L, 1);

                    lua_getfield(st->L, -1, "down");
                    if (lua_isnumber(st->L, -1)) sc->config.neighbor_down = lua_tointeger(st->L, -1);
                    lua_pop(st->L, 1);
                }
                lua_pop(st->L, 1);

                // Extract Physical Layout
                lua_getfield(st->L, -1, "physical");
                if (lua_istable(st->L, -1)) {
                    lua_getfield(st->L, -1, "scale");
                    if (lua_isnumber(st->L, -1)) sc->config.phys_scale = lua_tonumber(st->L, -1);
                    lua_pop(st->L, 1);

                    lua_getfield(st->L, -1, "offset");
                    if (lua_isnumber(st->L, -1)) sc->config.phys_offset = lua_tonumber(st->L, -1);
                    lua_pop(st->L, 1);
                }
                lua_pop(st->L, 1);

                // Extract Rotation
                lua_getfield(st->L, -1, "rotation");
                if (lua_isstring(st->L, -1)) {
                    const char *rot = lua_tostring(st->L, -1);
                    snprintf(sc->config.rotation, sizeof(sc->config.rotation), "%s", rot);
                }
                lua_pop(st->L, 1);

                // Extract Preferred Mode
                lua_getfield(st->L, -1, "preferred_mode");
                if (lua_isstring(st->L, -1)) {
                    const char *pm = lua_tostring(st->L, -1);
                    snprintf(sc->config.preferred_mode, sizeof(sc->config.preferred_mode), "%s", pm);
                }
                lua_pop(st->L, 1);

                // Extract DPI Override
                lua_getfield(st->L, -1, "dpi_override");
                if (lua_isnumber(st->L, -1)) {
                    sc->config.dpi_override = lua_tointeger(st->L, -1);
                }
                lua_pop(st->L, 1);

                lua_getfield(st->L, -1, "compositor");
                if (lua_istable(st->L, -1)) {
                    lua_getfield(st->L, -1, "enabled");
                    if (lua_isboolean(st->L, -1)) sc->config.picom_enabled = lua_toboolean(st->L, -1);
                    lua_pop(st->L, 1);
                }
                lua_pop(st->L, 1);

                lua_getfield(st->L, -1, "tray");
                if (lua_istable(st->L, -1)) {
                    lua_getfield(st->L, -1, "enabled");
                    if (lua_isboolean(st->L, -1)) sc->config.tray_enabled = lua_toboolean(st->L, -1);
                    lua_pop(st->L, 1);
                }
                lua_pop(st->L, 1);

                lua_getfield(st->L, -1, "tearfree");
                if (lua_istable(st->L, -1)) {
                    lua_getfield(st->L, -1, "enabled");
                    if (lua_isboolean(st->L, -1)) {
						sc->config.tearfree_enabled = lua_toboolean(st->L, -1);
						log_info("[parse] screen tearfree=%d", sc->config.tearfree_enabled);
					}
                    lua_pop(st->L, 1);
                }
                lua_pop(st->L, 1);

                // Extract Screen Power Settings
                lua_getfield(st->L, -1, "power");
                if (lua_istable(st->L, -1)) {
                    lua_getfield(st->L, -1, "independent_dpms");
                    if (lua_isboolean(st->L, -1)) sc->config.independent_dpms = lua_toboolean(st->L, -1);
                    lua_pop(st->L, 1);

                    lua_getfield(st->L, -1, "require_explicit_wake");
                    if (lua_isboolean(st->L, -1)) sc->config.require_explicit_wake = lua_toboolean(st->L, -1);
                    lua_pop(st->L, 1);

					lua_getfield(st->L, -1, "override_global");
					if (lua_isboolean(st->L, -1)) sc->config.override_global_power = lua_toboolean(st->L, -1);
					lua_pop(st->L, 1);

					lua_getfield(st->L, -1, "timeout_screensaver");
					if (lua_isnumber(st->L, -1)) sc->config.power.timeout_screensaver = lua_tointeger(st->L, -1);
					lua_pop(st->L, 1);

					lua_getfield(st->L, -1, "timeout_dpms");
					if (lua_isnumber(st->L, -1)) sc->config.power.timeout_dpms = lua_tointeger(st->L, -1);
					lua_pop(st->L, 1);
                }
                lua_pop(st->L, 1);

				// --- Screen Appearance Settings ---
				lua_getfield(st->L, -1, "appearance");
				if (lua_istable(st->L, -1)) {
					lua_getfield(st->L, -1, "override_global");
					if (lua_isboolean(st->L, -1)) sc->config.override_global_appearance = lua_toboolean(st->L, -1);
					lua_pop(st->L, 1);

					lua_getfield(st->L, -1, "cursor_theme");
					if (lua_isstring(st->L, -1)) {
						const char *theme = lua_tostring(st->L, -1);
						snprintf(
							sc->config.appearance.cursor_theme,
							sizeof(sc->config.appearance.cursor_theme),
							"%s",
							theme
						);
					}
					lua_pop(st->L, 1);

					lua_getfield(st->L, -1, "cursor_size");
					if (lua_isnumber(st->L, -1)) {
						sc->config.appearance.cursor_size = lua_tointeger(st->L, -1);
						if (sc->config.appearance.cursor_size < 1) {
							sc->config.appearance.cursor_size = 16;
						}
					}
					lua_pop(st->L, 1);

					lua_getfield(st->L, -1, "font_name");
					if (lua_isstring(st->L, -1)) {
						const char *fname = lua_tostring(st->L, -1);
						snprintf(
							sc->config.appearance.font_name,
							sizeof(sc->config.appearance.font_name),
							"%s",
							fname
						);
					}
					lua_pop(st->L, 1);

					lua_getfield(st->L, -1, "font_size");
					if (lua_isnumber(st->L, -1)) {
						sc->config.appearance.font_size = (float)lua_tonumber(st->L, -1);
					}
					lua_pop(st->L, 1);

					lua_getfield(st->L, -1, "font_unit");
					if (lua_isstring(st->L, -1)) {
						const char *unit = lua_tostring(st->L, -1);
						snprintf(
							sc->config.appearance.font_unit,
							sizeof(sc->config.appearance.font_unit),
							"%s",
							unit
						);
					}
					lua_pop(st->L, 1);
				}
				lua_pop(st->L, 1);
            }
            lua_pop(st->L, 1); // pop this specific screen table
        }
    }
    lua_pop(st->L, 1); // pop "screens" table

    // Clean up the main returned object from the stack
    lua_pop(st->L, 1);

    log_info("Configuration successfully loaded into C core.");
}


/* ---------- Awesome restart state hand-off (transport only) ---------- */
// The DE stores restore lines opaquely and replays them verbatim; it never
// parses the JSON. See the StellarState comment for the invariants.

static void restore_state_clear(StellarState *st, int screen) {
    if (screen < 0 || screen >= MAX_SCREENS) return;
    for (int i = 0; i < st->restore_line_count[screen]; i++) {
        free(st->restore_lines[screen][i]);
        st->restore_lines[screen][i] = NULL;
    }
    st->restore_line_count[screen] = 0;
    st->restore_accumulating[screen] = false;
    st->restore_ready[screen] = false;
}

// Begin a fresh accumulation for a screen. Any previously held (unconsumed)
// blob for that screen is discarded first, so we always hold the most recent
// restart's state, never a stale one.
static void restore_state_begin(StellarState *st, int screen) {
    if (screen < 0 || screen >= MAX_SCREENS) return;
    restore_state_clear(st, screen);
    st->restore_accumulating[screen] = true;
    log_info("restore-state: begin accumulation for screen %d", screen);
}

// Store one opaque line (the full "RESTORE_STATE_WIN ..." line as received,
// including the prefix, so replay is a verbatim echo).
static void restore_state_add_line(StellarState *st, int screen, const char *line) {
    if (screen < 0 || screen >= MAX_SCREENS) return;
    if (!st->restore_accumulating[screen]) {
        log_error("restore-state: WIN line for screen %d outside BEGIN/END; dropping",
                  screen);
        return;
    }
    if (st->restore_line_count[screen] >= MAX_TRACKED_RESTORE_WINDOWS) {
        log_error("restore-state: screen %d exceeded %d lines; dropping extra",
                  screen, MAX_TRACKED_RESTORE_WINDOWS);
        return;
    }
    char *dup = strdup(line);
    if (!dup) {
        log_error("restore-state: strdup failed for screen %d line", screen);
        return;
    }
    st->restore_lines[screen][st->restore_line_count[screen]++] = dup;
}

// Mark accumulation complete and hold for the next ready_for_sync.
static void restore_state_end(StellarState *st, int screen) {
    if (screen < 0 || screen >= MAX_SCREENS) return;
    if (!st->restore_accumulating[screen]) {
        log_error("restore-state: END for screen %d without BEGIN; ignoring", screen);
        return;
    }
    st->restore_accumulating[screen] = false;
    st->restore_ready[screen] = true;
    log_info("restore-state: held %d line(s) for screen %d, ready for handoff",
             st->restore_line_count[screen], screen);
}

// Replay held lines verbatim to a freshly-registered Awesome, then clear
// (consume-once). No-op if nothing is held for this screen (cold boot / quit).
static void restore_state_replay_to_fd(StellarState *st, int screen, int fd) {
    if (screen < 0 || screen >= MAX_SCREENS) return;
    if (!st->restore_ready[screen] || st->restore_line_count[screen] == 0) {
        return;
    }
    log_info("restore-state: replaying %d line(s) to screen %d fd=%d",
             st->restore_line_count[screen], screen, fd);

    // BEGIN and END are control lines: they are consumed on receive (to bracket
    // accumulation) and NOT stored, so we synthesize them here to bracket the
    // replayed content. Without these, the restarted Awesome never sees BEGIN
    // (reconcile never arms) or END (reconcile never triggers) -- it would only
    // receive the bare WIN/TAGS/CONFLUX lines.
    char ctrl[64];
    int m = snprintf(ctrl, sizeof(ctrl), "RESTORE_STATE_BEGIN screen=%d\n", screen);
    if (m > 0) write(fd, ctrl, (size_t)m);

    for (int i = 0; i < st->restore_line_count[screen]; i++) {
        const char *l = st->restore_lines[screen][i];
        write(fd, l, strlen(l));
        write(fd, "\n", 1);
    }

    m = snprintf(ctrl, sizeof(ctrl), "RESTORE_STATE_END screen=%d\n", screen);
    if (m > 0) write(fd, ctrl, (size_t)m);

    restore_state_clear(st, screen);  // consumed
}


static void register_awesome_client(
    StellarState *st,
    IpcClient *c,
    int screen_num,
    pid_t pid
) {
    if (screen_num < 0 || screen_num >= st->config.screen_count) {
        log_error("awesome HELLO has invalid screen=%d", screen_num);
        return;
    }

    c->is_awesome = true;
    c->screen_num = screen_num;
    c->pid = pid;

    st->screens[screen_num].awesome_fd = c->fd;

    log_info(
        "awesome IPC registered screen=%d fd=%d pid=%d",
        screen_num,
        c->fd,
        pid
    );

    // The moment the client is registered, tell it where the mouse currently is.
    // We send old=-1 so the Lua bridge knows this is a startup sync.
    if (st->pointer_screen != -1) {
        char reply[128];
        snprintf(reply, sizeof(reply), "POINTER_SCREEN old=-1 new=%d\n", st->pointer_screen);
        write(c->fd, reply, strlen(reply));
    }
    
	// Push the monitor name so AwesomeWM can cache it.
    char mon_reply[256];
    snprintf(mon_reply, sizeof(mon_reply), "MONITOR_NAME %s\n", st->screens[screen_num].monitor_name);
    write(c->fd, mon_reply, strlen(mon_reply));

    // Push the cached application menu to this newly-registered Awesome. This
    // is the menu analog of the SETTINGS_RELOADED/picom sync in the HELLO
    // handler: the DE owns the menu data (it lives on disk as .desktop files),
    // so we push it the moment the screen's socket is ready. Covers both fresh
    // logins and Awesome restarts (amnesia), since both re-run this path.
    push_menu_to_fd(c->fd);
}

static void handle_ipc_line(
    StellarState *st,
    int client_index,
    char *line
) {
    trim_newlines(line);

    if (line[0] == '\0') {
        return;
    }

    IpcClient *c = &st->clients[client_index];

    if (strncmp(line, "HELLO ", 6) == 0) {
        char role[64] = {0};
        int screen_num = -1;
        int pid = -1;

        int n = sscanf(
            line,
            "HELLO role=%63s screen=%d pid=%d",
            role,
            &screen_num,
            &pid
        );

        if (n >= 2 && strcmp(role, "awesome") == 0) {
            bool wants_sync = (strstr(line, "status=ready_for_sync") != NULL);

            // On a restart sync, replay the held window-state blob FIRST --
            // before register_awesome_client pushes MONITOR_NAME. Ordering
            // matters: Awesome processes socket lines in order, and receiving
            // MONITOR_NAME triggers its park-drain (placing parked windows). If
            // that drain runs before RESTORE_STATE_BEGIN has armed reconcile on
            // the Lua side, the parked windows get file-placed (geometry snaps
            // back) before reconcile can claim them. Sending BEGIN...END ahead
            // of MONITOR_NAME guarantees reconcile is armed before the drain.
            // Cold boot holds nothing, so this is a no-op there.
            if (wants_sync) {
                restore_state_replay_to_fd(st, screen_num, c->fd);
            }

            // Register the new socket. (This also sends POINTER_SCREEN and
            // MONITOR_NAME.)
            register_awesome_client(st, c, screen_num, (pid_t)pid);

            // Check if AwesomeWM woke up with amnesia and needs a sync
            if (wants_sync) {
                log_info("AwesomeWM (screen=%d) requested a state sync. Pushing state...", screen_num);

				//allow picom live-update
				update_stellar_picom_config(st);

                // Force the new Awesome instance to immediately load window rules
                char *sync_msg = "SETTINGS_RELOADED\n";
                write(c->fd, sync_msg, strlen(sync_msg));
            }
            return;
        }
    }

	if (strncmp(line, "FOCUS_WINDOW ", 13) == 0) {
        unsigned long w = 0;
        if (sscanf(line, "FOCUS_WINDOW win=%lu", &w) == 1) {
            // If win=0, fallback to the current screen's root window
            Window target = (w == 0) ? st->screens[st->pointer_screen].root : (Window)w;

            XSetInputFocus(st->dpy, target, RevertToPointerRoot, CurrentTime);
            XFlush(st->dpy);
        }
        return;
    }

/* TODO: Reimplement forced umaximized failsafe function.
	if (strncmp(line, "UNMAXIMIZE ", 11) == 0) {
        unsigned long w = 0;
        if (sscanf(line, "UNMAXIMIZE win=%lu", &w) == 1) {
            Window target = (Window)w;
			force_unmaximize(st, target);
        }
        return;
    }
*/

	if (strncmp(line, "RESTART_X11", 11) == 0) {
		log_info("Received RESTART_X11 from a client.");
		st->stellar_shutdown = true;
        return;
    }

	if (strncmp(line, "RELOAD_CONFIG", 13) == 0) {
		log_info("Received RELOAD_CONFIG from a client. Reloading settings.json...");

		parse_settings_from_json(st);

		// Pick up any newly installed .bdf/.pcf fonts (no-op when fresh)
		if (stellar_font_sync_bitmap_cache(false) < 0) {
			log_error("bitmap font cache sync failed (is fonttosfnt installed?)");
		}

		apply_stellar_xsettings(st);

		load_window_rules(st);

		// Push the updated rules onto existing windows' _STELLAR_* flags now,
		// before telling the Awesome side to re-apply. This restores titlebars
		// on windows whose "titlebars_enabled: false" rule was removed, and
		// updates fullscreen flags live. (Window type is left untouched on live
		// windows; it only applies at create time.)
		reapply_window_rules_to_existing(st);

		update_stellar_picom_config(st);
        apply_system_daemons(st);
		monitor_apply_all_preferred_modes(st);
		monitor_apply_all_rotations(st);
		monitor_apply_all_tearfree(st);

		broadcast_line(st, "SETTINGS_RELOADED");
	}

	if (strncmp(line, "GET_APPEARANCE", 14) == 0) {
		int s = 0;
		// Missing/invalid screen= falls back to 0 inside the handler.
		sscanf(line, "GET_APPEARANCE screen=%d", &s);
		ipc_handle_get_appearance(st, c->fd, s);
		log_info("Sent appearance JSON to fd=%d (screen=%d)", c->fd, s);
		return;
	}

	if (strncmp(line, "GET_SCREEN_INFO", 15) == 0) {
		// Large enough for MAX_SCREENS worth of mode lists plus per-screen EDID
		// hex (up to ~1KB each). Previously 4096, which truncated once EDID was
		// added to the payload.
		char json_buf[32768];
		monitor_build_screen_info_json(st, json_buf, sizeof(json_buf));

		ssize_t wr = write(c->fd, json_buf, strlen(json_buf));
		ssize_t wr2 = write(c->fd, "\n", 1);
		(void)wr; (void)wr2;

		log_info("Sent screen info JSON to fd=%d (%zu bytes)", c->fd, strlen(json_buf));
		return;
	}

	if (strncmp(line, "GET_SCREEN_FOR_DISPLAY ", 23) == 0) {
		// Map an X display string (e.g. ":0.1") to a Stellar screen index.
		// Global helpers like the polkit agent have no STELLAR_SCREEN of
		// their own (it is only set per-Awesome-child), so they resolve the
		// screen per-request from the requesting app's DISPLAY.  We match
		// against the canonical display_name strings the DE generated in
		// init_x() via make_screen_display_name(), so this is robust to
		// non-:0 servers (e.g. Xephyr :3.0-:3.2) where a naive suffix parse
		// would be wrong.  Reply is one line: the matching index, or -1.
		const char *want = line + 23;
		while (*want == ' ') want++;   // tolerate extra spaces

		int found = -1;
		for (int i = 0; i < st->config.screen_count; i++) {
			if (strcmp(st->screens[i].display_name, want) == 0) {
				found = i;
				break;
			}
		}

		char reply[32];
		int rn = snprintf(reply, sizeof(reply), "%d\n", found);
		ssize_t wr = write(c->fd, reply, (size_t)rn);
		(void)wr;

		log_info("GET_SCREEN_FOR_DISPLAY '%s' -> screen %d (fd=%d)",
				 want, found, c->fd);
		return;
	}

	if (strncmp(line, "GET_SCREEN_FOR_WINDOW ", 22) == 0) {
		// Map an X window id to the Stellar screen index that owns it.
		//
		// The xdg portal (and any other global helper) is handed an X window
		// id as the "parent" of a file-chooser request and needs to know which
		// screen to theme + place the chooser on.  Resolving this here, against
		// the DE's own ScreenState[].root table from init_x(), is the
		// authoritative answer: it avoids the client re-deriving the screen by
		// parsing DISPLAY strings (wrong on non-:0 / Xephyr servers) or by
		// assuming the X screen index equals the Stellar screen index.
		//
		// The parent handed to a portal is frequently a CLIENT window that
		// AwesomeWM has already reparented under a frame, so its immediate
		// parent is not one of our screen roots.  XQueryTree fills root_ret
		// with the window's *screen root* regardless of reparenting, so a
		// single query resolves it.  A stale / already-destroyed wid makes
		// XQueryTree trip BadWindow, which x11_error_handler() swallows; the
		// call then returns 0 and we reply -1.  Reply is one line: index or -1.
		const char *want = line + 22;
		while (*want == ' ') want++;   // tolerate extra spaces

		unsigned long wid = 0;
		int found = -1;
		if (sscanf(want, "%lu", &wid) == 1 && wid != 0) {
			Window root_ret = 0, parent_ret = 0, *children = NULL;
			unsigned int nchildren = 0;

			if (XQueryTree(st->dpy, (Window)wid, &root_ret, &parent_ret,
			               &children, &nchildren)) {
				if (children) XFree(children);
				for (int i = 0; i < st->config.screen_count; i++) {
					if (st->screens[i].root == root_ret) {
						found = i;
						break;
					}
				}
			}
		}

		char reply[32];
		int rn = snprintf(reply, sizeof(reply), "%d\n", found);
		ssize_t wr = write(c->fd, reply, (size_t)rn);
		(void)wr;

		log_info("GET_SCREEN_FOR_WINDOW '%s' (wid=%lu) -> screen %d (fd=%d)",
		         want, wid, found, c->fd);
		return;
	}

	if (strncmp(line, "GET_THEME_DATA", 14) == 0) {
		int s = -1;
		if (sscanf(line, "GET_THEME_DATA screen=%d", &s) != 1
			|| s < 0 || s >= st->config.screen_count) {
			// Bad or missing screen param - send error JSON back to requester
			const char *err = "{\"error\":\"invalid screen\"}\n";
			write(c->fd, err, strlen(err));
			log_error("GET_THEME_DATA: invalid screen=%d", s);
			return;
		}

		ScreenState *sc = &st->screens[s];

		// Check that the awesome instance for this screen is connected
		if (sc->awesome_fd < 0) {
			const char *err = "{\"error\":\"awesome not connected for this screen\"}\n";
			write(c->fd, err, strlen(err));
			log_error("GET_THEME_DATA: awesome not connected for screen %d", s);
			return;
		}

		// Store the requester's fd so we can relay the response back
		sc->pending_theme_request_fd = c->fd;

		// Forward the request to the awesome instance for this screen
		send_line_to_screen(st, s, "GET_THEME_DATA");

		log_info("GET_THEME_DATA: forwarded to screen %d, requester fd=%d", s, c->fd);
		// NOTE: do NOT close the client connection here - we need it open for the response
		return;
	}

	if (strncmp(line, "THEME_DATA_RESPONSE ", 20) == 0) {
		// This comes FROM an awesome instance. The payload is JSON.
		// Find which screen this awesome client belongs to.
		if (!c->is_awesome || c->screen_num < 0
			|| c->screen_num >= st->config.screen_count) {
			log_error("THEME_DATA_RESPONSE from non-awesome or invalid client");
			return;
		}

		ScreenState *sc = &st->screens[c->screen_num];
		int dest_fd = sc->pending_theme_request_fd;

		if (dest_fd < 0) {
			log_error("THEME_DATA_RESPONSE for screen %d but no pending request",
					  c->screen_num);
			return;
		}

		// Clear the pending request
		sc->pending_theme_request_fd = -1;

		// The JSON payload starts after "THEME_DATA_RESPONSE "
		const char *json_payload = line + 20;

		ssize_t wr = write(dest_fd, json_payload, strlen(json_payload));
		ssize_t wr2 = write(dest_fd, "\n", 1);
		(void)wr; (void)wr2;

		log_info("Relayed theme data for screen %d to fd=%d (%zu bytes)",
				 c->screen_num, dest_fd, strlen(json_payload));
		return;
	}

    // --- Awesome restart state hand-off (transport only) ---
    // Awesome sends these just before a restart re-execs. We store the WIN/TAGS
    // lines verbatim and replay them on the next ready_for_sync. The DE never
    // parses the JSON payload - it's opaque transport.
    if (strncmp(line, "RESTORE_STATE_BEGIN", 19) == 0) {
        int screen_idx = -1;
        if (sscanf(line, "RESTORE_STATE_BEGIN screen=%d", &screen_idx) == 1) {
            restore_state_begin(st, screen_idx);
        }
        return;
    }
    if (strncmp(line, "RESTORE_STATE_WIN", 17) == 0 ||
        strncmp(line, "RESTORE_STATE_TAGS", 18) == 0 ||
        strncmp(line, "RESTORE_CONFLUX", 15) == 0) {
        int screen_idx = -1;
        // All carry "screen=N" right after the keyword.
        const char *sp = strstr(line, "screen=");
        if (sp && sscanf(sp, "screen=%d", &screen_idx) == 1) {
            restore_state_add_line(st, screen_idx, line);  // store verbatim
        }
        return;
    }
    if (strncmp(line, "RESTORE_STATE_END", 17) == 0) {
        int screen_idx = -1;
        if (sscanf(line, "RESTORE_STATE_END screen=%d", &screen_idx) == 1) {
            restore_state_end(st, screen_idx);
        }
        return;
    }

    if (strncmp(line, "EVENT type=awesome_startup", 26) == 0) {
        int screen_idx = -1;
        if (sscanf(line, "EVENT type=awesome_startup screen=%d", &screen_idx) == 1) {
        	log_info("Received startup signal from AwesomeWM running on screen %d.", screen_idx);
        }
    }

    if (strncmp(line, "EVENT type=awesome_restarting", 29) == 0) {
        int screen_idx = -1;
        if (sscanf(line, "EVENT type=awesome_restarting screen=%d", &screen_idx) == 1) {
			log_info("Received RESTART signal from AwesomeWM. Cleaning up orphaned windows on screen %d.", screen_idx);
			cleanup_phantom_awesome_drawins(st, screen_idx);
		}
    }

    if (strncmp(line, "EVENT type=awesome_quitting", 27) == 0) {
        log_info("Received QUIT signal from AwesomeWM.");
        // Quit, not restart: discard any held restore blob for this screen so
        // it can never be replayed into a later start. (screen= is present on
        // the event; if parseable, clear just that screen, else clear all.)
        int screen_idx = -1;
        if (sscanf(line, "EVENT type=awesome_quitting screen=%d", &screen_idx) == 1
            && screen_idx >= 0 && screen_idx < MAX_SCREENS) {
            restore_state_clear(st, screen_idx);
        } else {
            for (int s = 0; s < MAX_SCREENS; s++) restore_state_clear(st, s);
        }
		st->stellar_shutdown = true;
    }

	log_info("ipc fd=%d: %s", c->fd, line);
    lua_handle_ipc_line(st, c->fd, line);
}

void consume_client_data(StellarState *st, int idx) {
    IpcClient *c = &st->clients[idx];

    char buf[1024];
    ssize_t n = read(c->fd, buf, sizeof(buf));

    if (n <= 0) {
        remove_client(st, idx);
        return;
    }

    if (c->input_len + (size_t)n >= sizeof(c->input_buf)) {
        log_error("IPC input overflow on fd=%d", c->fd);
        remove_client(st, idx);
        return;
    }

    memcpy(c->input_buf + c->input_len, buf, (size_t)n);
    c->input_len += (size_t)n;
    c->input_buf[c->input_len] = '\0';

    char *start = c->input_buf;
    while (1) {
        char *nl = strchr(start, '\n');
        if (!nl) {
            break;
        }

        *nl = '\0';
        handle_ipc_line(st, idx, start);
        start = nl + 1;
    }

    size_t remaining = c->input_buf + c->input_len - start;
    memmove(c->input_buf, start, remaining);
    c->input_len = remaining;
    c->input_buf[c->input_len] = '\0';
}

void load_window_rules(StellarState *st) {
    const char *home = get_user_home_dir();

    char settings_path[PATH_MAX];
    snprintf(
        settings_path,
        sizeof(settings_path),
        "%s/.config/stellar/settings.json",
        home
    );

    lua_getglobal(st->L, "load_window_rules");
    lua_pushstring(st->L, settings_path);

    if (lua_pcall(st->L, 1, 0, 0) != LUA_OK) {
        log_error("Window rules load failed: %s", lua_tostring(st->L, -1));
        lua_pop(st->L, 1);
        return;
    }
}

// Set or strip the _STELLAR_* flag xproperties on window `w` from the match
// result table at the top of the Lua stack (the table is left in place).
//
// `authoritative` controls behavior when a flag is NOT mentioned by the rule:
//   - false (per-window calls from CreateNotify / name-change): leave the
//     existing flag untouched. A rule that's silent about titlebars must not
//     clobber a flag some other rule owns, and re-evaluating a window's name
//     should not strip flags every time. This is what stops the spurious
//     "Removed _STELLAR_NO_TITLEBARS" churn on windows like NoMachine.
//   - true  (reload sweep over existing windows): make the window's flags
//     exactly reflect the current rules, stripping any flag the rule no longer
//     calls for. This is what lets deleting a "titlebars_enabled: false" rule
//     restore titlebars live on reload.
//
// Returns the window-type result (set_type / type_value) via *out_rr.
static void apply_rule_props_from_stack(StellarState *st, Window w,
                                        bool authoritative, RuleResult *out_rr) {
    // --- fullscreen_desktop xproperty ---
    lua_getfield(st->L, -1, "fullscreen_desktop");
    if (lua_isboolean(st->L, -1) && lua_toboolean(st->L, -1)) {
        Atom prop = XInternAtom(st->dpy, "_STELLAR_FULLSCREEN_DESKTOP", False);
        Atom cardinal = XInternAtom(st->dpy, "CARDINAL", False);
        unsigned long val = 1;
        XChangeProperty(st->dpy, w, prop, cardinal, 32,
                        PropModeReplace, (unsigned char *)&val, 1);
        log_info("Set _STELLAR_FULLSCREEN_DESKTOP for 0x%lx", w);
    } else if (authoritative) {
        Atom prop = XInternAtom(st->dpy, "_STELLAR_FULLSCREEN_DESKTOP", True);
        if (prop != None) {
            XDeleteProperty(st->dpy, w, prop);
            log_info("Removed _STELLAR_FULLSCREEN_DESKTOP from 0x%lx", w);
        }
    }
    lua_pop(st->L, 1);

    // --- window type ---
    lua_getfield(st->L, -1, "window_type");
    if (lua_isstring(st->L, -1)) {
        const char *wtype = lua_tostring(st->L, -1);
        Atom type_atom = XInternAtom(st->dpy, "_NET_WM_WINDOW_TYPE", False);
        Atom value;

        if (strcmp(wtype, "desktop") == 0)
            value = XInternAtom(st->dpy, "_NET_WM_WINDOW_TYPE_DESKTOP", False);
        else if (strcmp(wtype, "dock") == 0)
            value = XInternAtom(st->dpy, "_NET_WM_WINDOW_TYPE_DOCK", False);
        else
            value = XInternAtom(st->dpy, "_NET_WM_WINDOW_TYPE_NORMAL", False);

        XChangeProperty(st->dpy, w, type_atom, XA_ATOM, 32,
                        PropModeReplace, (unsigned char *)&value, 1);
        log_info("Set window type to %s for 0x%lx", wtype, w);

        if (out_rr) {
            out_rr->set_type = true;
            out_rr->type_value = value;
        }
    }
    lua_pop(st->L, 1);

    // --- titlebars ---
    lua_getfield(st->L, -1, "titlebars_enabled");
    if (lua_isboolean(st->L, -1) && !lua_toboolean(st->L, -1)) {
        Atom prop = XInternAtom(st->dpy, "_STELLAR_NO_TITLEBARS", False);
        Atom cardinal = XInternAtom(st->dpy, "CARDINAL", False);
        unsigned long val = 1;
        XChangeProperty(st->dpy, w, prop, cardinal, 32,
                        PropModeReplace, (unsigned char *)&val, 1);
        log_info("Set _STELLAR_NO_TITLEBARS for 0x%lx", w);
    } else if (authoritative) {
        Atom prop = XInternAtom(st->dpy, "_STELLAR_NO_TITLEBARS", True);
        if (prop != None) {
            Atom actual_type;
            int actual_format;
            unsigned long nitems, bytes_after;
            unsigned char *prop_data = NULL;

            // Actually check if the window has the property before logging/deleting
            if (XGetWindowProperty(st->dpy, w, prop, 0, 0, False, AnyPropertyType,
                                   &actual_type, &actual_format, &nitems, &bytes_after, &prop_data) == Success) {
                if (prop_data) XFree(prop_data);
                if (actual_type != None) {
                    XDeleteProperty(st->dpy, w, prop);
                    log_info("Removed _STELLAR_NO_TITLEBARS from 0x%lx", w);
                }
            }
        }
    }
    lua_pop(st->L, 1);

    XFlush(st->dpy);
}

RuleResult apply_window_rules(StellarState *st, Window w, const char *class_name, const char *win_name) {
    RuleResult rr = { false, 0 };
    log_info("apply_window_rules: class='%s' name='%s' win=0x%lx",
             class_name, win_name, w);

    lua_getglobal(st->L, "match_window_rule");
    lua_pushstring(st->L, class_name);
    lua_pushstring(st->L, win_name);

    if (lua_pcall(st->L, 2, 1, 0) != LUA_OK) {
        log_error("match_window_rule error: %s", lua_tostring(st->L, -1));
        lua_pop(st->L, 1);
        return rr;
    }

    if (!lua_istable(st->L, -1)) {
        lua_pop(st->L, 1);
        return rr;
    }

    // Per-window call: only SET flags the rule explicitly specifies; never
    // strip on absence. Stripping/restoration of stale flags is the job of the
    // authoritative reload sweep (reapply_window_rules_to_existing).
    apply_rule_props_from_stack(st, w, /*authoritative=*/false, &rr);

    lua_pop(st->L, 1); // pop result table
    return rr;
}

// Authoritative single-window application, used by the reload sweep over
// existing windows. Unlike apply_window_rules (per-window, additive), this
// makes the window's _STELLAR_* flags exactly reflect the current rules:
//   - if a rule matches, set the flags it specifies and strip the ones it
//     doesn't (authoritative=true);
//   - if NO rule matches, strip all _STELLAR_* flags, so a window that used to
//     match a "titlebars_enabled: false" rule gets its titlebars back.
// It intentionally does NOT act on window_type for re-manage purposes: type is
// applied at window-create time and we don't want a rule edit to reparent/flash
// live windows. (The caller ignores the returned set_type.)
RuleResult apply_window_rules_authoritative(StellarState *st, Window w,
                                            const char *class_name,
                                            const char *win_name) {
    RuleResult rr = { false, 0 };

    lua_getglobal(st->L, "match_window_rule");
    lua_pushstring(st->L, class_name);
    lua_pushstring(st->L, win_name);

    if (lua_pcall(st->L, 2, 1, 0) != LUA_OK) {
        log_error("match_window_rule error: %s", lua_tostring(st->L, -1));
        lua_pop(st->L, 1);
        return rr;
    }

    if (!lua_istable(st->L, -1)) {
        // No matching rule: strip every _STELLAR_* flag this window might carry.
        lua_pop(st->L, 1);

        Atom ft = XInternAtom(st->dpy, "_STELLAR_FULLSCREEN_DESKTOP", True);
        if (ft != None) XDeleteProperty(st->dpy, w, ft);
        Atom nt = XInternAtom(st->dpy, "_STELLAR_NO_TITLEBARS", True);
        if (nt != None) XDeleteProperty(st->dpy, w, nt);
        XFlush(st->dpy);
        return rr;
    }

    apply_rule_props_from_stack(st, w, /*authoritative=*/true, &rr);

    lua_pop(st->L, 1); // pop result table
    return rr;
}

void update_stellar_picom_config(StellarState *st) {
    const char *home = get_user_home_dir();

    char defaults_path[PATH_MAX];
    snprintf(
        defaults_path,
        sizeof(defaults_path),
        "%s/picom/defaults.json",
        STELLAR_SHARE_PATH
    );

    char settings_path[PATH_MAX];
    snprintf(
        settings_path,
        sizeof(settings_path),
        "%s/.config/stellar/settings.json",
        home
    );

    // Load and cache the merged settings (no useful return value)
    lua_getglobal(st->L, "load_picom_settings");
    lua_pushstring(st->L, defaults_path);
    lua_pushstring(st->L, settings_path);

    if (lua_pcall(st->L, 2, 0, 0) != LUA_OK) {
        log_error("Picom settings load failed: %s", lua_tostring(st->L, -1));
        lua_pop(st->L, 1);
        return;
    }

    char theme_dir[PATH_MAX] = {0};
    if (get_stellar_theme_dir(theme_dir, sizeof(theme_dir), st->config.stellar_theme) == NULL) {
        log_error("Failed to resolve theme: %s", st->config.stellar_theme);
        return;
    }

    for (int idx = 0; idx < st->config.screen_count; idx++) {
        ScreenState *sc = &st->screens[idx];

        lua_getglobal(st->L, "generate_picom_config");
        lua_pushstring(st->L, theme_dir);
        lua_pushnumber(st->L, sc->scale);

        if (lua_pcall(st->L, 2, 1, 0) != LUA_OK) {
            log_error("Picom config gen failed for screen %d: %s",
                idx, lua_tostring(st->L, -1));
            lua_pop(st->L, 1);
            continue;  // continue, not return - try other screens
        }

        const char *config_str = lua_tostring(st->L, -1);
        if (!config_str) {
            lua_pop(st->L, 1);
            continue;
        }

        char file_path[PATH_MAX];
        snprintf(
            file_path,
            sizeof(file_path),
            "%s/.cache/stellar/picom/screen_%d.conf",
            home,
            idx
        );

        FILE *f = fopen(file_path, "w");
        if (f) {
            fputs(config_str, f);
            fclose(f);
            log_info("Generated picom conf at %s", file_path);
        } else {
            log_error("Failed to write picom conf: %s", strerror(errno));
        }

        lua_pop(st->L, 1);  // pop the config string
    }
}
