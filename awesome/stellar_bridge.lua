_G.stellar_api = {}

local stellar_log_path = "/tmp/stellar.log"
local stellar_screen = tonumber(os.getenv("STELLAR_SCREEN") or "-1")
local log_prefix = "[awesome:" .. tostring(stellar_screen) .. "]"

local log_file = io.open(stellar_log_path, "a")

if log_file then
    -- Set buffering to line-by-line to prevent mangled concurrent writes
    log_file:setvbuf("line")
end

stellar_api.log = function(msg)
    if log_file then
        log_file:write(
            os.date("%H:%M:%S")
                .. " "
                .. log_prefix
                .. " "
                .. tostring(msg)
                .. "\n"
        )
    end
end
local stellar_log = stellar_api.log

local old_print = print
_G.print = function(...)
    local args = {}
    for i = 1, select("#", ...) do
        args[#args + 1] = tostring(select(i, ...))
    end
    local str = table.concat(args, "\t")
    stellar_log(str)
    old_print(...)
end

local gears = require("gears")
local config_dir = gears.filesystem.get_configuration_dir()
-- Load patched tasklist before requiring 'awful'
package.loaded["awful.widget.tasklist"] = dofile(config_dir .. "modules/tasklist.lua")

local wibox = require("wibox")
local awful = require("awful")
local ruled = require("ruled")
local beautiful = require("beautiful")

-- Hide stalonetray from Awesome's floating-window snap targets.
-- stalonetray is still managed/visible, but other clients should not
-- magnetize to it while moving/resizing.
--
-- snap.snap() runs on every mouse tick while the mousegrabber is active.
-- The previous implementation built a brand-new closure AND a brand-new
-- filtered table on every one of those ticks, generating heavy GC pressure
-- exactly when the drag is already saturating the X11 queue. Instead we:
--   1. tag the stalonetray client once with c.stellar_ignore_snap = true, and
--   2. install a single, reused visible-filter closure that simply skips any
--      client carrying that flag and short-circuits (zero allocation) when no
--      such client exists.
local snap_module = require("awful.mouse.snap")
local aclient = require("awful.client")
local original_snap = snap_module.snap
local original_visible = aclient.visible

-- Tag stalonetray exactly once, when it appears.
local function tag_if_ignored(c)
	if c and c.valid and (
		c.class == "stalonetray"
		or c.instance == "stalonetray"
		or c.name == "stalonetray"
	) then
		c.stellar_ignore_snap = true
	end
end

client.connect_signal("manage", tag_if_ignored)
-- Catch a stalonetray that is already managed (e.g. on config reload).
for _, c in ipairs(client.get()) do
	tag_if_ignored(c)
end

-- A single filter closure, created once and reused across every snap tick.
local function snap_visible_filter(...)
	local clients = original_visible(...)

	-- Fast path: scan for an ignored client. The overwhelming majority of
	-- ticks have none, so we return the original table untouched and allocate
	-- nothing.
	local has_ignored = false
	for _, candidate in ipairs(clients) do
		if candidate.stellar_ignore_snap then
			has_ignored = true
			break
		end
	end

	if not has_ignored then
		return clients
	end

	-- Slow path (only when stalonetray is actually present): build the
	-- filtered list. This is the rare case, so the allocation is acceptable.
	local filtered = {}
	for _, candidate in ipairs(clients) do
		if not candidate.stellar_ignore_snap then
			filtered[#filtered + 1] = candidate
		end
	end
	return filtered
end

snap_module.snap = function(c, ...)
	aclient.visible = snap_visible_filter
	local ok, result = pcall(original_snap, c, ...)
	aclient.visible = original_visible

	if not ok then
		error(result)
	end

	return result
end

-- Register custom window xproperties
awesome.register_xproperty("_STELLAR_LOCKED", "boolean")
awesome.register_xproperty("_STELLAR_TILED", "boolean")
awesome.register_xproperty("_STELLAR_CONFLUX_WORKSPACE", "string")
awesome.register_xproperty("_STELLAR_NO_FADE", "boolean")
awesome.register_xproperty("_STELLAR_TAB_BAR_RECT", "string")
awesome.register_xproperty("_STELLAR_NO_TITLEBARS", "boolean")
awesome.register_xproperty("_STELLAR_SIMPLE_DECORATIONS", "boolean")
awesome.register_xproperty("_STELLAR_FULLSCREEN_DESKTOP", "boolean")

awesome.connect_signal("debug::error", function(err)
	-- err may be a string OR a table/object; normalise it
	local msg = type(err) == "table" and (err.message or err.msg) or tostring(err)
	stellar_log("ERROR: " .. tostring(msg))
	stellar_log("TRACEBACK:\n" .. debug.traceback("", 2))
end)

awesome.connect_signal("debug::deprecation", function(msg)
    stellar_log("DEPRECATION: " .. tostring(msg))
end)

local function fail(msg)
    io.stderr:write("stellar bridge error: " .. msg .. "\n")
    error(msg, 0)
end

local function require_socket_unix_or_fail()
    local ok, mod = pcall(require, "socket.unix")
    if not ok then
        fail(
            "missing required Lua module 'socket.unix' "
                .. "(usually provided by lua-socket): "
                .. tostring(mod)
        )
    end

    return mod
end

local function require_json_or_fail()
    local ok, mod = pcall(require, "dkjson")
    if not ok then
        fail(
            "missing required Lua module 'dkjson' "
                .. "(usually provided by lua-dkjson): "
                .. tostring(mod)
        )
    end

    return mod
end

local socket_unix = require_socket_unix_or_fail()
local json = require_json_or_fail()
stellar_api._this_screen = stellar_screen
stellar_api._num_screens = tonumber(os.getenv("STELLAR_NUM_SCREENS") or "1")

local function is_fullscreen_desktop(c)
	if not c or not c.valid then return false end
    return c:get_xproperty("_STELLAR_FULLSCREEN_DESKTOP") == true
end

-- MRU
local mru_stack = {}

stellar_api.mru_cycle = function(modkey)
	local cl = client.focus
	if is_fullscreen_desktop(cl) then
		if cl.fullscreen then return end
		client.focus = nil
		stellar_api.log("Released fullscreen desktop client focus")
	elseif #mru_stack < 2 then
		return
	end

	-- Create a static snapshot of valid clients for the current screen
	local cycle_list = {}
	for _, c in ipairs(mru_stack) do
		-- Filter out minimized, hidden, and dead clients
		if c and c.valid and not c.minimized and not c.hidden then
			table.insert(cycle_list, c)
		end
	end

	if #cycle_list < 2 then return end

	-- Focus the second window in the snapshot (the previously focused one)
	local current_index = 2
	local c = cycle_list[current_index]
	if c then
		client.focus = c
		c:raise()
		stellar_api.set_active_client(c)
	end

	-- Map the AwesomeWM modifier name to the X11 release keysyms it can produce.
    local release_keys = {
        Mod4 = { Super_L = true, Super_R = true },
        Mod1 = { Alt_L = true, Alt_R = true },
        Control = { Control_L = true, Control_R = true },
        Shift = { Shift_L = true, Shift_R = true },
    }
    local stop_on = release_keys[modkey] or { Super_L = true, Super_R = true }

	-- Start the keygrabber to handle holding Mod and repeatedly pressing Tab
	awful.keygrabber.run(function(mod, key, event)
		stellar_api.log("grab event=" .. event .. " key=" .. tostring(key)
			.. " mod=[" .. table.concat(mod, ",") .. "]")
		-- Stop cycling when the modifier key is released
		if event == "release" then
			if stop_on[key] then
				awful.keygrabber.stop()
				-- Re-assert focus after the grab tears down, so the grabber's
				-- own focus restoration can't stomp the selection.
				local final = cycle_list[current_index]
				if final and final.valid then
					gears.timer.delayed_call(function()
						if final.valid then
							final:raise()
							client.focus = final
							stellar_api.focus_window(final.window)
							stellar_api.set_active_client(final)
						end
					end)
				end
			end
			return
		end

		-- Advance the cycle on subsequent Tab presses
		if event == "press" and key == "Tab" then
			current_index = current_index + 1

			-- Wrap around to the beginning if we hit the end of the list
			if current_index > #cycle_list then
				current_index = 1
			end

			local next_c = cycle_list[current_index]
			if next_c then
				client.focus = next_c
				next_c:raise()
				stellar_api.set_active_client(next_c)
			end
		end
	end)
end

-- Mouse focus settings
local focus_mode = "sloppy"
local focus_cooldown = 0.25
local focus_guard_active = false
local pending_focus_client = nil
local focus_timer = gears.timer {
	timeout = focus_cooldown,
	single_shot = true
}
focus_timer:connect_signal("timeout", function()
    focus_guard_active = false
    -- If a client was queued during the guard period, focus it now
    if pending_focus_client and pending_focus_client.valid
       and client.focus ~= pending_focus_client then
        pending_focus_client:emit_signal("request::activate", "mouse_enter", {raise = false})
        pending_focus_client = nil
        -- That was another focus change, so start a new guard
        focus_guard_active = true
        focus_timer:again()
    else
        pending_focus_client = nil
    end
end)

local function load_stellar_settings()
    local file = io.open(os.getenv("HOME") .. "/.config/stellar/settings.json", "r")
    local obj = nil

    if file then
        local content = file:read("*all")
        file:close()

        local parsed, pos, err = json.decode(content, 1, nil)
        if err then
            print("stellar: Error parsing settings: " .. err)
        else
            obj = parsed
        end
    end

    -- Ensure the base structure exists even if the file is completely missing
    obj = obj or {}
    obj.appearance = obj.appearance or {}
    obj.screens = obj.screens or {}
    obj.terminal = obj.terminal or {}
    obj.mouse = obj.mouse or {}

    -- Inject Defaults (Strictly checking for 'nil')
    -- If the value is "", it means the user explicitly cleared it in the GUI.
    if obj.default_placement == nil then
        obj.default_placement = { "no_overlap", "no_offscreen" }
    end

    if obj.appearance.wallpaper_path == nil then
        local share_path = os.getenv("STELLAR_SHARE_PATH") or "/usr/local/share/stellar"
        obj.appearance.wallpaper_path = share_path .. "/wallpapers/pleiades-blue.jpg"
    end

    if obj.appearance.wallpaper_mode == nil then
        obj.appearance.wallpaper_mode = "cropped"
    end

    if obj.appearance.font_name == nil then
        obj.appearance.font_name = "sans-serif"
    end
    if obj.appearance.font_size == nil then
        obj.appearance.font_size = 11.0
    end
    if obj.appearance.font_unit == nil then
        obj.appearance.font_unit = "pt"
    end

    if obj.terminal.terminal_gui == nil then
        obj.terminal.terminal_gui = "xterm"
    end

    if obj.terminal.shell == nil then
        obj.terminal.shell = os.getenv("SHELL") or "/bin/bash"
    end

    return obj
end

-- Font application now lives in modules.stellar_theme (the theme authority).
-- This thin delegate preserves the old call sites (live settings reload, etc.)
-- while keeping a single implementation.
local function stellar_apply_font(settings)
    require("modules.stellar_theme").apply_font(settings)
end

local function make_unix_socket(socket_unix)
    if type(socket_unix) == "function" then
        return socket_unix()
    end

    if type(socket_unix) == "table" then
        if type(socket_unix.stream) == "function" then
            return socket_unix.stream()
        end

        if type(socket_unix) == "table" and getmetatable(socket_unix) then
            local mt = getmetatable(socket_unix)
            if mt and type(mt.__call) == "function" then
                return socket_unix()
            end
        end
    end

    fail(
        "module 'socket.unix' loaded, but no supported socket constructor "
            .. "was found"
    )
end

local function get_required_env(name)
    local value = os.getenv(name)
    if not value or value == "" then
        fail(name .. " not set")
    end
    return value
end

local function load_base_config(base_path)
    print("stellar: loading base awesome config: " .. base_path)
    dofile(base_path)
end

local function flush_stellar_rules()
    local ruled = require("ruled")
    local rules = ruled.client.rules

    -- Iterate backward to safely remove elements without messing up the index
    for i = #rules, 1, -1 do
        if rules[i].stellar_id then
            table.remove(rules, i)
        end
    end
end

-- Helper to check if stellar_screen is in the target string/number
local function screen_matches(target, current_screen)
    if target == nil or target == "all" then return true end
    if type(target) == "number" then return target == current_screen end
    if type(target) == "string" then
        for s in string.gmatch(target, "%d+") do
            if tonumber(s) == current_screen then return true end
        end
    end
    return false
end

-- Converts strings like "awful.placement.centered" into the real function
local function resolve_function_strings(k, v)
    local awful = require("awful")
    if type(v) == "string" and v:match("^awful%.placement%.") then
        local placement_name = v:gsub("awful%.placement%.", "")
        if awful.placement[placement_name] then
            return awful.placement[placement_name]
        end
    end
    return v
end

local function fit_to_screen(c)
	local geo
	if is_fullscreen_desktop(c) then
		geo = c.screen.geometry
	else
		geo = c.screen.workspace
	end

	c:geometry({x = geo.x, y = geo.y, width = geo.width, height = geo.height})
	stellar_log("###### fit_to_screen() called on " .. tostring(c.name) .. " --- applying fullscreen geometry")
end

local function apply_window_rules(settings)
    if not settings or not settings.window_rules then
        return
    end

    flush_stellar_rules()
    stellar_log("Parsing window rules for stellar_screen=" .. tostring(stellar_screen))

    for _, rule_def in ipairs(settings.window_rules) do
        local props = rule_def.properties or {}
        local target_screen = props.screen
        local rule_class = (rule_def.rule and rule_def.rule.class) or "unknown"

        -- Conditional logic for loading rules per screen
        if screen_matches(target_screen, stellar_screen) then
            stellar_log("LOADING rule for " .. rule_class)

            local new_rule = {
                stellar_id = rule_def.stellar_id,
                rule = rule_def.rule,
                properties = {},
            }

			-- Custom properties handled in callback only
			local should_maximize = false
			local should_fullscreen = false
			local title_source = nil
			local prevent_fullscreen = false

			-- Strip internal/custom keys before passing properties to Awesome
			for k, v in pairs(props) do
				if k == "maximized" then
					should_maximize = v
				elseif k == "fullscreen" then
					should_fullscreen = v
				elseif k == "prevent_fullscreen" and v then
					prevent_fullscreen = true
				elseif k == "title_source" then
					title_source = v
				elseif k == "protect_transient_focus" then
                    protect_transient_focus = v
				elseif k ~= "screen" and
					   k ~= "event_keys" and
					   k ~= "titlebars_enabled" and
					   k ~= "simple_decorations" then
					new_rule.properties[k] = resolve_function_strings(k, v)
				end
			end

			-- Log exactly when this rule hits a window
			new_rule.callback = function(c)
				stellar_log(
					">>> RULE EXECUTED! Caught window: "
						.. tostring(c.class)
						.. " ("
						.. tostring(c.name)
						.. ") titlebars_enabled=" .. tostring(titlebars_enabled)
				)

				-- Set title_source directly and notify the titlebar
				if title_source and c.stellar_title_source ~= title_source then
					c.stellar_title_source = title_source
					c:emit_signal("property::stellar_title_source")
				end

				c.prevent_fullscreen = prevent_fullscreen
				c.should_maximize = should_maximize
				c.protect_transient_focus = protect_transient_focus
				c.maximized = false
				if c.should_maximize then
					stellar_log("setting should_maximize=true for " .. tostring(c.name))
				end

				gears.timer.delayed_call(function()
					if not c.valid then
						return
					end

					if new_rule.properties.placement == awful.placement.centered then
						c.floating = true
						awful.placement.centered(c, {
							honor_workarea = true,
						})
					end

					if not prevent_fullscreen then
						if should_fullscreen then
							c.fullscreen = false
							c.fullscreen = true
						end
					end

					if c.should_maximize then
						c.maximized = true
						if is_fullscreen_desktop(c) then
							fit_to_screen(c)
						end
					end
				end)
			end

            ruled.client.append_rule(new_rule)
        else
            stellar_log(
                "IGNORING rule for "
                    .. rule_class
                    .. " (Condition: "
                    .. tostring(target_screen)
                    .. ")"
            )
        end
    end
end

local function flash_screen()
    local s = awful.screen.focused()

    -- Create the flash wibox
    local flash = wibox({
        x = s.geometry.x,
        y = s.geometry.y,
        width = s.geometry.width,
        height = s.geometry.height,
        bg = "#ffffff",
        ontop = true,
        visible = true,
        type = "splash" -- ensures it floats over everything
    })

    -- Destroy the wibox after 0.15 seconds
    gears.timer.start_new(0.15, function()
        flash.visible = false
        flash = nil
        return false -- return false so the timer doesn't repeat
    end)
end

stellar_api.tasklist_source = function(s)
    local cls = {}
    for _, c in ipairs(client.get()) do
        if c.screen == s then
            table.insert(cls, c)
--[[
			if c.type == "desktop" then
			    stellar_log("tasklist_source: " .. tostring(c.name)
						.. " skip_taskbar=" .. tostring(c.skip_taskbar)
						.. " hidden=" .. tostring(c.hidden)
						.. " sticky=" .. tostring(c.sticky)
						.. " tags=" .. tostring(#c:tags()))
            end
]]--
		end
    end
---    stellar_log("tasklist_source: returning " .. #cls .. " clients")
    return cls
end

stellar_api.tasklist_filter = function(c, screen)
	local awful = require("awful")
    stellar_log("tasklist_filter: " .. tostring(c.name) .. " type=" .. c.type)
	local fullscreen_desktop = c:get_xproperty("_STELLAR_FULLSCREEN_DESKTOP")
	if c.type == "desktop" then
        stellar_log("tasklist_filter: " .. tostring(c.name)
            .. " type=desktop fullscreen_desktop=" .. tostring(fullscreen_desktop))
    end
	if fullscreen_desktop then return true end
	return awful.widget.tasklist.filter.currenttags(c, screen)
end

stellar_api.toggle_fullscreen = function(c)
    stellar_log("toggle_fullscreen: " .. tostring(c.name)
        .. " prevent_fullscreen=" .. tostring(c.prevent_fullscreen)
        .. " fullscreen=" .. tostring(c.fullscreen)
        .. " maximized=" .. tostring(c.maximized))

    if c.fullscreen then
        c.fullscreen = false
        -- When we drop fullscreen, Wine will panic and grab Maximized.
        -- Let it. It's an invisible property in the desktop layer.
        if is_fullscreen_desktop(c) then
            fit_to_screen(c)
        end
    elseif not c.prevent_fullscreen then
        -- Strip maximized immediately before asserting fullscreen
        if c.maximized then
            c.maximized = false
        end
        c.fullscreen = true
    end
end

local function play_system_sound(filepath)
    if filepath and filepath ~= "" then
        -- The `false` argument prevents starting a shell and detaches the process
        awful.spawn({"paplay", filepath}, false)
    end
end

stellar_api.take_screenshot = function(mode)
    local awful = require("awful")
    local screenshot_filename = "\"$HOME/Pictures/Screenshots/Screenshot %Y-%m-%d at %H.%M.%S \\$wx\\$h.png\""
    local soundfile = "/usr/share/sounds/freedesktop/stereo/camera-shutter.oga"

    local scrot_cmd
    if mode == "selection" then
        stellar_log("took screenshot (selection area)")
        -- Added sleep to release the X11 key grab, and >/dev/null 2>&1 to detach xclip
        scrot_cmd = "sleep 0.2 && scrot -s -f " .. screenshot_filename .. " -e 'xclip -selection clipboard -target image/png -i \"$f\" >/dev/null 2>&1'"
    else
        stellar_log("took screenshot (entire screen)")
        -- Added >/dev/null 2>&1 to detach xclip
        scrot_cmd = "scrot " .. screenshot_filename .. " -e 'xclip -selection clipboard -target image/png -i \"$f\" >/dev/null 2>&1'"
    end

    awful.spawn.easy_async_with_shell(scrot_cmd, function(stdout, stderr, exitreason, exitcode)
        -- Only flash and play sound if scrot exited successfully
        if exitcode == 0 then
            flash_screen()
            play_system_sound(soundfile)
        else
            stellar_log("screenshot canceled or failed (exit code: " .. tostring(exitcode) .. ")")
        end
    end)
end

-- (wallpaper application now lives in modules.stellar_wallpaper)

-- (theme_data.lua loading now lives in modules.stellar_theme)


local function apply_stellar_settings(new_settings)
	-- Re-apply the appearance font (newly converted bitmap fonts
	-- are already installed by the DE before this broadcast).
	stellar_apply_font(new_settings)

	-- apply new mouse focus mode
	if new_settings.mouse and new_settings.mouse.focus_mode then
		focus_mode = new_settings.mouse.focus_mode
		stellar_log("stellar: focus_mode set to " .. focus_mode)
	end
	if new_settings.mouse and new_settings.mouse.focus_cooldown then
		focus_cooldown = new_settings.mouse.focus_cooldown
		focus_timer.timeout = focus_cooldown
		stellar_log("stellar: focus_cooldown set to " .. focus_cooldown)
	end

	-- Swap out the window rules safely
	apply_window_rules(new_settings)

	-- Re-apply rules to existing windows instantly
	for _, c in ipairs(client.get()) do
		awful.rules.apply(c)
	end
end

-- Helper to ensure paths don't contain slashes or weird characters
local function sanitize_name(str)
    if not str or str == "" then return "unknown" end
    -- Replace spaces, slashes, and colons with underscores
    return str:gsub("[%s/\\:]", "_")
end

local function get_geometry_state_file(c, monitor_edid_name)
    -- Fail closed on an unresolved monitor name. Previously an unknown name
    -- silently produced a valid-looking path under "Unknown_Monitor", so a
    -- window managed before the name was known would save/restore geometry to
    -- the wrong directory forever. Returning nil makes load_client_geometry
    -- fall through to default placement (acceptable, self-heals on the next
    -- settled manage) and makes save_client_geometry a no-op (no bogus dir).
    if not monitor_edid_name
       or monitor_edid_name == ""
       or monitor_edid_name == "Unknown_Monitor"
       or monitor_edid_name == "Unknown"
       or monitor_edid_name == "No Monitor" then
        stellar_log("geometry: monitor name unresolved ('"
            .. tostring(monitor_edid_name) .. "'), skipping state file")
        return nil
    end

    local state_home = os.getenv("XDG_STATE_HOME")
    if not state_home or state_home == "" then
        local home = os.getenv("HOME") or "/tmp"
        state_home = home .. "/.local/state"
    end

    local safe_monitor = sanitize_name(monitor_edid_name)

    local identifier

    -- Check if it's a Conflux-managed window by checking its instance name
    if c.instance and c.instance:match("^stellar_conflux_") then
        local conflux = require("modules.conflux")

        -- 1. Try to find it in the active workspaces
        local ws_name = conflux.workspaces[c.instance]

        -- 2. Fallback to pending_spawns if it's just now spawning
        if not ws_name then
            ws_name = conflux.pending_spawns[c.instance]
        end

        if ws_name then
            identifier = sanitize_name("stellar_conflux_" .. ws_name)
			stellar_log("conflux: geometry state identifier = " .. identifier)
        end
    end

    -- If not a Conflux window (or resolution failed), default to class/role
    if not identifier then
        local instance = c.instance or "unknown"
        local role = c.role or "default"
        identifier = sanitize_name(instance .. "_" .. role)
		stellar_log("final geometry state identifier = " .. identifier)
    end

	if identifier ~= "unknown" then
		-- Build: ~/.local/state/stellar/awesome/geometry/screen1_MAG275C_QD_E2/
		local target_dir = string.format("%s/stellar/awesome/geometry/screen%d_%s",
										 state_home, stellar_screen, safe_monitor)

		gears.filesystem.make_directories(target_dir)
		stellar_log("final geometry state file path = " .. target_dir .. "/" .. identifier)
		return target_dir .. "/" .. identifier
	end
	return nil
end

local function install_stellar_hooks(socket_unix)
    local gears = require("gears")
    local awful = require("awful")

    local client = client
    local screen = screen
    local tag = tag
	local z_stack_snapshot = {}
	stellar_api._grab = nil
	stellar_api._active_client = nil
	stellar_api._pointer_on_screen = false
	-- Reconcile state initialized early (before connect()/socket reads) so a
	-- replayed RESTORE_STATE_* line can never be processed before these exist.
	-- The apply functions are defined further down but are only invoked from
	-- timer callbacks that fire after all synchronous init completes.
	stellar_api._reconcile_active = false
	stellar_api._reconcile_windows = nil
	stellar_api._reconcile_tags = nil

	local stellar = {
        sock = nil,
        screen_num = stellar_screen,
        socket_path = get_required_env("STELLAR_SOCKET"),
        reconnect_timer = nil,
    }

    local function send_line(line)
        if not stellar.sock then
            return
        end

        local ok, err = stellar.sock:send(line .. "\n")
        if not ok then
            print("stellar: send failed: " .. tostring(err))
            if err == "closed" then
                pcall(function()
                    stellar.sock:close()
                end)
                stellar.sock = nil
            end
        end
    end
	stellar_api.send_ipc = send_line

	stellar_api.focus_window = function(win)
		send_line("FOCUS_WINDOW win=" .. tostring(win or 0))
	end

	stellar_api.unmaximize_window = function(win)
		send_line("UNMAXIMIZE win=" .. tostring(win or 0))
	end

	stellar_api.set_active_client = function(c)
		if is_fullscreen_desktop(c) then return end

		local prev = stellar_api._active_client
		if prev == c then return end
		stellar_api.log("set_active_client: " .. tostring(c) .. " (prev=" .. tostring(prev) .. ")")

		if prev and prev.valid then
			prev.stellar_active = false
			prev:emit_signal("property::stellar_active")
		end

		stellar_api._active_client = c

		if c and c.valid then
			c.stellar_active = true
			c:emit_signal("property::stellar_active")
		end
	end

	local minimize_others = function(c)
		if not c then end
		local screen = c.screen
		if not screen then return nil end

		for _, cl in ipairs(screen.clients) do
			if c ~= cl and cl.class ~= "stalonetray" then
				if cl.type == "desktop" and is_fullscreen_desktop(cl) then
					cl.hidden = true
				else
					cl.minimized = true
				end
			    stellar_log("*** MINIMIZED CLIENT: " .. tostring(cl.name)
						.. " type=" .. tostring(cl.type)
						.. " class=" .. tostring(cl.class)
						.. " minimized=" .. tostring(cl.minimized)
						.. " hidden=" .. tostring(cl.hidden)
				)
			end
		end
	end

	local is_only_visible_client = function(c)
		if not c then end
		local screen = c.screen
		if not screen then return nil end

		local visible = 0
		for _, cl in ipairs(screen.clients) do
			if not cl.minimized and not cl.hidden and cl.class ~= "stalonetray" then
				visible = visible + 1
			end
		end
		return (visible == 1)
	end

	local save_z_stack = function(screen)
--		if stellar_api._grab then return nil end
		if not screen then return nil end
		local snapshot = {}
		for _, cl in ipairs(screen.clients) do
			table.insert(snapshot, cl)
		end
		z_stack_snapshot = snapshot
		return snapshot
	end

	stellar_api._restore_z_stack = function(snapshot)
		if not snapshot then return end
		for i = #snapshot, 1, -1 do
			local cl = snapshot[i]
			if cl.valid then
				cl:raise()
			end
		end
	end

	stellar_api.mouse_action = function(c, context, action)
		stellar_log("Mouse action *** context=" .. context .. " action=" .. (action or "nil"))

		if context == "mouse_enter" then
			if stellar_api._grab and not stellar_api._grab.settled then return end
			-- Skip hover focus for desktop fullscreen windows
			if is_fullscreen_desktop(c) and not c.fullscreen then return end

			-- "click" mode: focus only changes on a real click, not on hover
			if focus_mode == "click" then return end

			-- Prevent stealing focus from active dialogs/transients within the same application
			local f = client.focus
			if f and f.protect_transient_focus and f.class == c.class then
				if f.transient_for == c or f.type == "dialog" or f.type == "utility" then
					return
				end
			end

			-- Guard pattern: focus instantly unless a focus change just happened.
			-- This prevents flicker at overlapping window edges without adding
			-- perceptible latency to normal mouse movement.
			if not focus_guard_active then
				-- No recent focus change - act immediately
				c:activate { context = "mouse_enter", raise = false }
				pending_focus_client = nil
				focus_guard_active = true
				focus_timer:again()
			else
				-- Inside the guard window - queue this client for when it expires
				pending_focus_client = c
			end
			return
		end

		local t = c.first_tag or (c.screen and c.screen.selected_tag)
		local is_floating_layout = t and (t.layout.name == "floating")

		local needs_z_restore = (action == "mouse_resize")
			or (action == "mouse_move" and not is_floating_layout and not c.floating)

		if needs_z_restore then
			stellar_api._grab = {
				client = c,
				action = action,
				z_stack = save_z_stack(c.screen),
			}

			c:activate { context = context, action = action }

			if not mousegrabber.isrunning() then
				stellar_api._grab = nil
			end

			gears.timer.delayed_call(function()
				if stellar_api._grab then
					stellar_api._restore_z_stack(stellar_api._grab.z_stack)
				end
			end)
		else
			local hide_not_minimize = c.type == "desktop" and c._stellar_show_in_tasklist

			if action == "toggle_minimization" and hide_not_minimize then
				-- Special handling for fullscreen_desktop windows
				if c.hidden or not is_only_visible_client(c) then
					stellar_log("### restoring desktop fullscreen window")
					c.hidden = false
					minimize_others(c)
					c:activate { context = "tasklist" }
				else
					stellar_log("### hiding the only fullscreen desktop window")
					c.hidden = true
				end
			else
				c:activate { context = context, action = action }
			end

			if action == "toggle_minimization" then
				save_z_stack(c.screen)
			elseif context == "mouse_click" then
				stellar_api.set_active_client(c)
			end
		end
	end

	-- DPI configuration and DPI-resolved sprite directory are now established
	-- by modules.stellar_theme (called from the bootstrap before the base
	-- config loads). stellar_api.theme_assets_path is mirrored there for the
	-- slice modules (stellar_ui / stellar_window) that read it directly.

    local function connect()
        if stellar.sock then return true end

        local sock = make_unix_socket(socket_unix)
        if not sock then return false end

        if sock.settimeout then sock:settimeout(0) end

        local ok, err = sock:connect(stellar.socket_path)
        if not ok and err ~= "timeout" and err ~= "already connected" then
            pcall(function() sock:close() end)
            return false
        end

        stellar.sock = sock
        send_line("HELLO role=awesome screen=" .. tostring(stellar.screen_num) .. " pid=0 status=ready_for_sync")
        print("stellar: connected to DE socket for screen " .. tostring(stellar.screen_num))
        return true
    end

    local function disconnect()
        if stellar.sock then
            pcall(function() stellar.sock:close() end)
            stellar.sock = nil
        end
    end

    local function focus_tag_by_name(tag_name)
        for s in screen do
            for _, t in ipairs(s.tags) do
                if t.name == tag_name then
                    t:view_only()
                    return true
                end
            end
        end
        return false
    end

    local function process_command(line)
--   	print("stellar cmd: " .. line)

		-- Update window focus when pointer changes screens
        local old_s, new_s = line:match("^POINTER_SCREEN old=(%-?%d+) new=(%d+)$")
        if old_s and new_s then
            old_s = tonumber(old_s)
            new_s = tonumber(new_s)

            if new_s == stellar.screen_num then
                stellar_api._pointer_on_screen = true
				if stellar_api._active_client and stellar_api._active_client.valid then
        			stellar_api._active_client:emit_signal("property::stellar_active")
    			end

				local awful = require("awful")
                -- Target is either the window directly under the mouse,
                -- or the last focused window from AwesomeWM's native history!
                local target = mouse.current_client or awful.client.focus.history.get(mouse.screen, 0)

                if target and target.valid then
                    client.focus = target                       -- Keep AwesomeWM's UI state correct
                    _G.stellar_api.focus_window(target.window)  -- Force C to set the hardware focus
                else
                    _G.stellar_api.focus_window(0)              -- Force C to focus the empty desktop
                end

			elseif old_s == stellar.screen_num then
			    -- The pointer just left THIS screen
                stellar_api._pointer_on_screen = false
				if stellar_api._active_client and stellar_api._active_client.valid then
					stellar_api._active_client:emit_signal("property::stellar_active")
    			end

                -- The pointer left this screen entirely (e.g. swiped onto
                -- another monitor in zaphodheads). That is NOT a widget
                -- mouse::leave, so any transient popup keyed off mouse::leave
                -- would stick open. Broadcast a process-wide signal so every
                -- such popup (run prompt, desktop menu, layout chooser, window
                -- titlebar popups, ...) can dismiss itself the same way it
                -- would on mouse::leave. Modules subscribe via
                --   awesome.connect_signal("stellar::pointer_left_screen", fn)
                -- and receive (old_screen, new_screen).
                awesome.emit_signal("stellar::pointer_left_screen", old_s, new_s)
			end

            -- We do absolutely nothing when leaving. AwesomeWM keeps its memory naturally!
            return
        end

        -- Initial handshake monitor name
        local mon_name = line:match("^MONITOR_NAME%s+(.+)$")
        if mon_name then
            stellar_api.monitor_name = mon_name
            stellar_api.log("Cached monitor name: " .. mon_name)
            -- Name just resolved: place any windows parked during the reload
            -- race window (see park-and-drain near the manage handler).
            if stellar_api._drain_pending_placement then
                stellar_api._drain_pending_placement(false)
            end
            return
        end

        -- ===== Restart state hand-off receiver =====
        -- The DE replays the RESTORE_STATE_* lines we sent before restarting.
        -- Receiving them means this startup is a restart with held state (never
        -- a cold boot), so we arm reconcile mode: the manage handler suppresses
        -- file-based geometry restore, and after the manage storm settles we
        -- apply the captured state (3b-1: z-stack; 3b-2: geometry/flags/tags).
        if line:match("^RESTORE_STATE_BEGIN") then
            stellar_api._restore_incoming = { windows = {}, tags = nil }
            stellar_api._reconcile_active = true
            stellar_api._reconcile_windows = nil
            stellar_api._reconcile_tags = nil
            stellar_api.log("restore: BEGIN receiving state blob (reconcile armed)")
            -- Cover: hide everything already managed so the reconcile happens
            -- out of sight. Windows remanage BEFORE this line arrives over the
            -- socket, so this mass-hide (not the manage-time hide) is what
            -- covers the bulk of them. Revealed in final state at settle.
            if stellar_api._reconcile_cover then
                stellar_api._reconcile_cover()
            end
            return
        end
        local win_json = line:match("^RESTORE_STATE_WIN%s+screen=%d+%s+(.+)$")
        if win_json then
            local ok, obj = pcall(json.decode, win_json, 1, nil)
            if ok and obj then
                if stellar_api._restore_incoming then
                    table.insert(stellar_api._restore_incoming.windows, obj)
                end
                stellar_api.log(string.format(
                    "restore WIN: wid=%s class=%s floating=%s stack=%s "
                    .. "min=%s max=%s fs=%s ontop=%s sticky=%s hidden=%s fsd=%s geo=%s tags=%s",
                    tostring(obj.wid), tostring(obj.class), tostring(obj.floating),
                    tostring(obj.stack), tostring(obj.minimized), tostring(obj.maximized),
                    tostring(obj.fullscreen), tostring(obj.ontop), tostring(obj.sticky),
                    tostring(obj.hidden), tostring(obj.fullscreen_desktop),
                    obj.geometry and string.format("%d,%d,%d,%d", obj.geometry.x,
                        obj.geometry.y, obj.geometry.width, obj.geometry.height) or "nil",
                    obj.tags and (#obj.tags .. " tag(s)") or "nil"))
            else
                stellar_api.log("restore WIN: json.decode failed: " .. tostring(win_json))
            end
            return
        end
        local tags_json = line:match("^RESTORE_STATE_TAGS%s+screen=%d+%s+(.+)$")
        if tags_json then
            local ok, obj = pcall(json.decode, tags_json, 1, nil)
            if ok and obj then
                if stellar_api._restore_incoming then
                    stellar_api._restore_incoming.tags = obj
                end
                stellar_api.log("restore TAGS: selected=" .. tostring(obj.selected)
                    .. " layouts=" .. tostring(obj.layouts and #obj.layouts or 0))
            end
            return
        end
        -- Conflux workspace tab order + active tab. Seed it IMMEDIATELY (not
        -- deferred to reconcile) so conflux.tab_orders is populated before the
        -- orphaned tabs hit the manage handler's orphan-adoption path -- that is
        -- what preserves manual order and restores the active tab instead of
        -- falling back to alphabetical + first-orphan-visible.
        local conflux_json = line:match("^RESTORE_CONFLUX%s+screen=%d+%s+(.+)$")
        if conflux_json then
            local ok, obj = pcall(json.decode, conflux_json, 1, nil)
            if ok and obj then
                if stellar_api._restore_incoming then
                    stellar_api._restore_incoming.conflux = obj
                end
                local ok_cx, conflux = pcall(require, "modules.conflux")
                if ok_cx and conflux and conflux.seed_restart_restore then
                    conflux.seed_restart_restore(obj)
                    stellar_api.log("restore CONFLUX: seeded "
                        .. tostring(#obj) .. " workspace(s)")
                else
                    stellar_api.log("restore CONFLUX: module/seed unavailable")
                end
            else
                stellar_api.log("restore CONFLUX: json.decode failed")
            end
            return
        end
        if line:match("^RESTORE_STATE_END") then
            local incoming = stellar_api._restore_incoming
            local n = incoming and #incoming.windows or 0
            stellar_api.log("restore: END received " .. tostring(n)
                .. " window record(s) -- triggering reconcile (z-stack)")

            -- Hand the window records to reconcile and start the settle-then-
            -- apply. If for some reason nothing was collected, disarm so we
            -- don't leave placement suppressed.
            if incoming and n > 0 then
                stellar_api._reconcile_windows = incoming.windows
                stellar_api._reconcile_tags = incoming.tags
                if stellar_api._run_reconcile then
                    stellar_api._run_reconcile()
                end
            else
                stellar_api._reconcile_active = false
                stellar_api._reconcile_windows = nil
                stellar_api._reconcile_tags = nil
            end
            return
        end

        -- Monitor hotplugs/resolution changes
        local changed_screen, new_name = line:match("^MONITOR_CHANGED%s+screen=(%d+)%s+name=(.+)$")
        if changed_screen and new_name then
            if tonumber(changed_screen) == stellar.screen_num then
                stellar_api.monitor_name = new_name
                stellar_api.log("Updated monitor name after hotplug: " .. new_name)
                -- In case any window was parked awaiting a name, place it now.
                if stellar_api._drain_pending_placement then
                    stellar_api._drain_pending_placement(false)
                end
            end
            return
        end

        local tag_name = line:match("^FOCUS_TAG%s+(.+)$")
        if tag_name then
            local ok, err = pcall(focus_tag_by_name, tag_name)
            if not ok then
                print("stellar: FOCUS_TAG failed: " .. tostring(err))
            end
            return
        end

        local cmd = line:match("^SPAWN%s+(.+)$")
        if cmd then
            local ok, err = pcall(function()
                awful.spawn(cmd, false)
            end)
            if not ok then
                print("stellar: SPAWN failed: " .. tostring(err))
            end
            return
        end

		local cmd = line:match("^SETTINGS_RELOADED")
		if cmd then
			print("stellar: Reloading settings from disk...")
			local new_settings = load_stellar_settings()

			if new_settings then
				apply_stellar_settings(new_settings)
				stellar_api.stellar_settings = new_settings

				print("stellar: Settings successfully applied live.")
			end
			return
		end

		local rest_cmd = line:match("^CONFLUX_RESTORE%s+(.+)$")
        if rest_cmd then
            local ok, obj = pcall(json.decode, rest_cmd, 1, nil)
            if ok and type(obj) == "table" then
                awesome.emit_signal("stellar::conflux_restore", obj)
            end
            return
        end

        local ack_cmd = line:match("^RELOCATE_ACK_RECV%s+(.+)$")
        if ack_cmd then
            local ok, obj = pcall(json.decode, ack_cmd, 1, nil)
            if ok and type(obj) == "table" and obj.transaction_id then
                awesome.emit_signal("stellar::conflux_ack", obj.transaction_id)
            end
            return
        end

		local cmd = line:match("^GET_THEME_DATA$")
		if cmd then
			stellar_log("GET_THEME_DATA request received, gathering theme info...")

			local stellar_theme = require("modules.stellar_theme")
			local theme_info     = stellar_theme.theme_info()
			local theme_base_data = stellar_theme.base_data()

			-- DIAGNOSTIC: did theme_data.lua actually load, and what keys does
			-- its nk_colors table really use?  Empty nk_colors here (or wrongly
			-- named keys) is why the C apps fall back to hardcoded colors.
			stellar_log("THEME_DATA: theme_dir=" .. tostring(theme_info.theme_dir))
			stellar_log("THEME_DATA: theme_base_data is "
				.. (next(theme_base_data) == nil and "EMPTY (load failed?)" or "populated"))
			if theme_base_data.nk_colors then
				for k, v in pairs(theme_base_data.nk_colors) do
					stellar_log("THEME_DATA: nk_colors['" .. tostring(k)
						.. "'] -> merged as 'nk_color_" .. tostring(k)
						.. "' = " .. tostring(v))
				end
			else
				stellar_log("THEME_DATA: theme_base_data.nk_colors is NIL "
					.. "(no nuklear colors will be sent)")
			end

			local response_json = json.encode(theme_info)
			send_line("THEME_DATA_RESPONSE " .. response_json)
			stellar_log("Sent THEME_DATA_RESPONSE (" .. #response_json .. " bytes)")
			return
		end

		local menu_json = line:match("^MENU_DATA%s+(.+)$")
        if menu_json then
            local ok, obj = pcall(json.decode, menu_json, 1, nil)
            if ok and type(obj) == "table" then
                require("modules.stellar_menu").set_menu_data(obj)
                stellar_log("MENU_DATA received: "
                    .. tostring(obj.categories and #obj.categories or 0)
                    .. " categories")
            else
                stellar_log("MENU_DATA decode failed")
            end
            return
        end
    end

	local function poll_socket()
        if not stellar.sock then return end

        while true do
            local chunk, err, partial = stellar.sock:receive("*l")
            local line = chunk or partial

            if line and #line > 0 then process_command(line) end

            if err == "timeout" then break end
            if err == "closed" or (err and err ~= "timeout") then
                disconnect()
                break
            end
            if not chunk and not partial then break end
        end
    end

	connect()

    stellar.reconnect_timer = gears.timer({
        timeout = 0.1,
        autostart = true,
        call_now = false,
        callback = function()
            if not stellar.sock then
                connect()
			else
            	poll_socket()
			end
        end,
    })

	awesome.connect_signal("startup", function()
        send_line("EVENT type=awesome_startup screen=" .. tostring(stellar.screen_num))
    end)

-- TODO: change or get rid of locked custom window state
	local function update_locked_state(c)
		local is_locked = false
--[[
		if c.maximized or c.fullscreen or not c.focusable then
			is_locked = true
		end

		c:set_xproperty("_STELLAR_LOCKED", is_locked)
]]--
	end

	client.connect_signal("property::class", function(c)
		if c.class and c.class ~= "" then
            stellar_log(
                "applying window rules to window id="
                .. tostring(c.window) .. " class=" .. tostring(c.class or "")
            )
			awful.rules.apply(c)
		end
	end)

	stellar_api.load_client_geometry = function(c)
		if not c or not c.valid or is_fullscreen_desktop(c) then return false end

		local edid_name = stellar_api.monitor_name  -- nil when unresolved; get_geometry_state_file fails closed
		local state_path = get_geometry_state_file(c, edid_name)

		if state_path and (c.floating or awful.layout.get(c.screen).name == "floating") then
			local file = io.open(state_path, "r")
			if file then
				local content = file:read("*all")
				file:close()
				local x, y, w, h = content:match("(%-?%d+),(%-?%d+),(%d+),(%d+)")
				if x and y and w and h then
					local saved_geo = {
						x = tonumber(x), y = tonumber(y),
						width = tonumber(w), height = tonumber(h)
					}
					c:geometry(saved_geo)
					return saved_geo
				end
			end
		end
		return false
	end

	-- ============================================================
	-- Park-and-drain placement (reload race fix)
	-- ------------------------------------------------------------
	-- Geometry restore keys on the monitor name. On cold boot the DE's settle
	-- barrier guarantees the name is known before AwesomeWM spawns, so at
	-- 'manage' time the name is already present and placement runs immediately.
	--
	-- On a Lua-side awesome.restart() (menu "restart awesome") the DE barrier
	-- does NOT run -- AwesomeWM relearns its name asynchronously via the
	-- MONITOR_NAME socket push during startup. Window 'manage' races that push:
	-- a window managed before the name arrives would key on nil, fail restore,
	-- and land in the default corner placement. That race is per-window and
	-- nondeterministic (the "reload N times, sometimes windows scatter" bug).
	--
	-- Fix: if the name isn't resolved when a window is managed, PARK it instead
	-- of placing it. When the name arrives (MONITOR_NAME / MONITOR_CHANGED
	-- handlers) we DRAIN the parked list and place each one then. A bounded
	-- safety timer drains anyway after a timeout so nothing is stuck if the
	-- name never comes (worst case: corner fallback, i.e. old behavior, but
	-- deterministically). The event loop keeps running throughout, so the very
	-- socket message we're waiting on is free to arrive -- no blocking.
	local pending_placement = {}

	local function monitor_name_resolved()
		local n = stellar_api.monitor_name
		return n ~= nil and n ~= ""
			and n ~= "Unknown_Monitor" and n ~= "Unknown" and n ~= "No Monitor"
	end

	-- The actual placement decision, factored out of the manage handler so it
	-- can run either immediately (name known) or deferred (on drain).
	local function apply_placement(c)
		if not c or not c.valid then return end
		if c._stellar_placement_handled then return end

		local loaded_saved = false

		-- Try to load saved geometry (only applies for floating windows;
		-- load_client_geometry enforces that and the fail-closed name check).
		local saved_geo = stellar_api.load_client_geometry(c)
		if saved_geo then
			loaded_saved = true

			-- Check if this is a Conflux window
			local is_conflux = c.instance and c.instance:match("^stellar_conflux_")

			-- Enforce-once guard. Some clients (e.g. Lutris) re-assert their
			-- own remembered position shortly after mapping, via a path that
			-- does NOT come through request::geometry -- so the move only shows
			-- up as property::geometry, which fires AFTER the position already
			-- changed. We watch for the first such deviation, snap it back to
			-- saved_geo once, then disarm and let the window move freely.
			--
			-- Only clients that actually restored a saved geometry arm this.
			-- Brand-new windows never do, so their default placement is
			-- untouched.
			if not is_conflux then
				c._stellar_geom_guard = saved_geo

				local function enforce_once(cl)
					if not cl._stellar_geom_guard then return end
					local g = cl._stellar_geom_guard
					-- Disarm first so our own :geometry() call below
					-- (which re-fires property::geometry) doesn't recurse.
					cl._stellar_geom_guard = nil
					cl:disconnect_signal("property::geometry", enforce_once)

					local cur = cl:geometry()
					if cur.x ~= g.x or cur.y ~= g.y
						or cur.width ~= g.width or cur.height ~= g.height then
						cl:geometry(g)
						stellar_log("geom guard: overrode self-reposition of "
							.. tostring(cl.class) .. " back to saved "
							.. string.format("%d,%d,%d,%d", g.x, g.y, g.width, g.height))
					end
				end

				c:connect_signal("property::geometry", enforce_once)

				-- Safety net: if the client never repositions itself, drop the
				-- guard so a much later legitimate move isn't caught. Disarming
				-- is idempotent with enforce_once.
				gears.timer.start_new(4.0, function()
					if c.valid and c._stellar_geom_guard then
						c._stellar_geom_guard = nil
						c:disconnect_signal("property::geometry", enforce_once)
					end
					return false
				end)
			end
		end

		-- Fallback to settings.json defaults for brand new windows
		if not loaded_saved and not c.fullscreen and not c.maximized then
			local placement_rules = stellar_api.stellar_settings.default_placement
			local placement_engine = require("awful.placement")

			for _, rule_name in ipairs(placement_rules) do
				if placement_engine[rule_name] then
					placement_engine[rule_name](c, { honor_workarea = true })
				end
			end
		end
	end

	-- Place any windows that were parked waiting for the monitor name.
	-- force=true means the safety timer expired: place them regardless (they
	-- take corner fallback if the name is still unresolved).
	stellar_api._drain_pending_placement = function(force)
		if #pending_placement == 0 then return end
		if not force and not monitor_name_resolved() then return end

		-- If a restart reconcile is active, the snapshot is the authority on
		-- these windows' placement -- do NOT apply file placement to them, or
		-- they snap back to stale file geometry. Reconcile handles them (z-stack
		-- now; full geometry/flags in 3b-2). Just clear the queue.
		--
		-- This closes the path that made snap-back 100%: on a restart the name
		-- is unresolved when windows manage, so they PARK rather than hit the
		-- manage-handler reconcile gate; the name then arrives and drains them
		-- through apply_placement, bypassing that gate. Gating here too covers
		-- the park-drain path.
		if stellar_api._reconcile_active then
			stellar_log("draining " .. tostring(#pending_placement)
				.. " parked window(s): reconcile active, discarding file "
				.. "placement (snapshot owns geometry)")
			pending_placement = {}
			return
		end

		local drained = pending_placement
		pending_placement = {}
		stellar_log("draining " .. tostring(#drained)
			.. " parked window(s) for placement (force=" .. tostring(force)
			.. ", name=" .. tostring(stellar_api.monitor_name) .. ")")
		for _, c in ipairs(drained) do
			apply_placement(c)
		end
	end

	-- Safety net: if the name never resolves after a reload, drain anyway so
	-- parked windows aren't stuck unplaced. 2s mirrors the DE-side barrier.
	gears.timer.start_new(2.0, function()
		if #pending_placement > 0 then
			stellar_log("placement drain timeout: name still unresolved, "
				.. "placing parked windows with fallback")
			stellar_api._drain_pending_placement(true)
		end
		return false  -- one-shot
	end)

	-- ============================================================
	-- Reconcile mode (restart restore, 3b-1: gate + z-stack)
	-- ------------------------------------------------------------
	-- Armed when RESTORE_STATE records arrive on the sync handshake (i.e. this
	-- startup is a restart with held state, not a cold boot). While armed:
	--   * the manage handler SUPPRESSES its file-based geometry restore, because
	--     the snapshot is the authority on the reload path (this is what stops
	--     windows snapping back to stale file geometry).
	--   * after the manage storm settles, we apply the captured z-stack order.
	-- ============================================================
	-- Reconcile visibility cover ("hide, reconcile in the dark, reveal")
	-- ------------------------------------------------------------
	-- Without this, restarted windows are visible wherever Awesome's remanage
	-- drops them and then visibly shuffle when reconcile applies states/z-stack.
	-- With it: everything is hidden the moment we know this is a restart
	-- (RESTORE_STATE_BEGIN), reconciled while unmapped, and revealed in final
	-- state -- KDE-plasmashell-style "disappear, reappear settled".
	--
	-- Two cover hooks are needed because windows remanage BEFORE the BEGIN line
	-- arrives over the socket (manage is local startup; the socket poll comes
	-- later): a mass-hide at BEGIN for everything already managed, plus a
	-- hide-at-manage for late arrivals while reconcile is armed.
	--
	-- The reveal restores each record's INTENDED hidden state (captured in the
	-- snapshot) rather than blanket-unhiding, so fullscreen_desktop and hidden
	-- Conflux siblings come back correctly. Unmatched covered windows (e.g. a
	-- brand-new window that appeared mid-reload) are simply unhidden.
	--
	-- stalonetray is Stellar-owned and never touched.
	local function is_tray(c)
		return c.class == "stalonetray"
			or c.instance == "stalonetray"
			or c.name == "stalonetray"
	end

	stellar_api._reconcile_cover = function()
		local s = screen[stellar.screen_num] or awful.screen.focused()
		if not s then return end
		local covered = 0
		for _, c in ipairs(s.all_clients) do
			if c.valid and not is_tray(c) and not c.hidden then
				c.hidden = true
				covered = covered + 1
			end
		end
		stellar_log("reconcile: cover -- hid " .. covered .. " window(s)")

		-- Failsafe: if reconcile never completes (e.g. RESTORE_STATE_END never
		-- arrives, so the settle never starts), the cover would leave the
		-- desktop hidden forever. If reconcile is still armed well past the
		-- settle bound (4s), force-reveal everything and disarm. In a healthy
		-- run reconcile has disarmed long before this fires, making it a no-op.
		gears.timer.start_new(6.0, function()
			if stellar_api._reconcile_active then
				stellar_log("reconcile: FAILSAFE -- still armed 6s after cover; "
					.. "force-revealing all windows and disarming")
				local s2 = screen[stellar.screen_num] or awful.screen.focused()
				if s2 then
					for _, cl in ipairs(s2.all_clients) do
						if cl.valid and not is_tray(cl) then cl.hidden = false end
					end
				end
				stellar_api._reconcile_active = false
				stellar_api._reconcile_windows = nil
				stellar_api._reconcile_tags = nil
			end
			return false  -- one-shot
		end)
	end

	-- Restore intended visibility from the records; unhide anything covered
	-- that has no record. Applies rec.hidden uniformly (including Conflux and
	-- fullscreen_desktop records: for Conflux this agrees with -- and backstops
	-- -- force_active_tab, whose passes all run before the settle).
	local function reconcile_reveal(records)
		local s = screen[stellar.screen_num] or awful.screen.focused()
		if not s then return end

		local intended = {}          -- client -> intended hidden (bool)
		if records then
			local by_wid, by_inst = {}, {}
			for _, c in ipairs(s.all_clients) do
				if c.valid then
					by_wid[c.window] = c
					if c.instance then by_inst[c.instance] = c end
				end
			end
			for _, rec in ipairs(records) do
				local c = (rec.wid and by_wid[rec.wid])
					or (rec.instance and by_inst[rec.instance])
				if c and c.valid then
					intended[c] = rec.hidden and true or false
				end
			end
		end

		local shown, kept_hidden = 0, 0
		for _, c in ipairs(s.all_clients) do
			if c.valid and not is_tray(c) then
				-- Claim this window from the startup-cover failsafe: reveal is
				-- now the authority on its visibility.
				c._stellar_startup_covered = nil
				local want_hidden = intended[c]
				if want_hidden == nil then want_hidden = false end
				if c.hidden ~= want_hidden then
					c.hidden = want_hidden
				end
				if want_hidden then kept_hidden = kept_hidden + 1
				else shown = shown + 1 end
			end
		end
		stellar_log("reconcile: reveal -- " .. shown .. " shown, "
			.. kept_hidden .. " intentionally hidden")
	end

	-- Geometry/flags/tags apply (3b-2). Runs at reconcile settle, before the
	-- z-stack raise. Matches records to live clients by wid (normal) or instance
	-- (Conflux). For each: restore tag membership, floating, geometry, then the
	-- inflating flags (maximize/fullscreen) LAST so Awesome derives the inflated
	-- display from the real geometry underneath, then ontop/sticky/minimized.
	local function apply_window_states(records)
		if not records then return end
		local s = screen[stellar.screen_num] or awful.screen.focused()
		if not s then return end

		stellar_log("reconcile: applying window states ("
			.. tostring(#records) .. " record(s))...")

		-- wid -> client and instance -> client maps.
		local by_wid, by_inst = {}, {}
		for _, c in ipairs(s.all_clients) do
			if c.valid then
				by_wid[c.window] = c
				if c.instance then by_inst[c.instance] = c end
			end
		end

		local applied, failed = 0, 0
		for _, rec in ipairs(records) do
			-- fullscreen_desktop windows manage their own geometry/visibility
			-- (fit_to_screen / hidden), so skip them here.
			--
			-- Conflux tab windows are owned by the Conflux restart restore (tab
			-- order + active tab via seed_restart_restore/activate_tab, which
			-- handles their geometry and hidden state). Applying generic window
			-- states to them would fight that, so skip them too -- identified by
			-- the stellar_conflux_ instance prefix.
			local is_conflux = rec.instance
				and tostring(rec.instance):match("^stellar_conflux_")
			if not rec.fullscreen_desktop and not is_conflux then
				local c = (rec.wid and by_wid[rec.wid])
					or (rec.instance and by_inst[rec.instance])
				if c and c.valid then
					-- Per-record protection: one malformed record or one client
					-- that objects to a property write must not abort the whole
					-- reconcile (this runs inside a timer callback -- an
					-- unhandled error here would silently kill the z-stack and
					-- focus restore that follow).
					local ok, err = pcall(function()
						-- 1. Tag membership (by saved tag indices).
						if rec.tags and #rec.tags > 0 then
							local tags = {}
							for _, te in ipairs(rec.tags) do
								local t = s.tags[te.tag]
								if t then table.insert(tags, t) end
							end
							if #tags > 0 then c:tags(tags) end
						end

						-- 2. Floating state (before geometry).
						c.floating = rec.floating and true or false

						-- 3+4. Geometry and inflating flags, together, because the
						-- order is what makes un-fullscreen/un-maximize work.
						--
						-- On restart, an inflated window is managed at its
						-- inflated size (the fullscreen/maximize atoms persist as
						-- xproperties), so AWESOME memorizes the inflated rect as
						-- its internal "restore to" geometry. Merely setting our
						-- _saved_floating_geom doesn't help -- Awesome's
						-- un-fullscreen path uses its own memorized rect, which
						-- is why exiting fullscreen after a reload left a giant
						-- window.
						--
						-- Fix: the toggle pattern (already used by the rules code
						-- for fullscreen). CLEAR the inflating flags, apply the
						-- real geometry while un-inflated, then set the flags
						-- back. At the moment a flag goes true, Awesome records
						-- the current -- now real -- geometry as the restore
						-- target, so leaving the state later returns correctly.
						local was_inflated = rec.fullscreen or rec.maximized
							or rec.maximized_horizontal or rec.maximized_vertical

						if was_inflated then
							c.fullscreen = false
							c.maximized = false
							c.maximized_horizontal = false
							c.maximized_vertical = false
						end

						if rec.geometry then
							c._saved_floating_geom = {
								x = rec.geometry.x, y = rec.geometry.y,
								width = rec.geometry.width, height = rec.geometry.height,
							}
							c:geometry(rec.geometry)
						end

						if was_inflated then
							-- Re-inflate from the real geometry just applied.
							if rec.maximized_horizontal then c.maximized_horizontal = true end
							if rec.maximized_vertical   then c.maximized_vertical   = true end
							if rec.maximized  then c.maximized  = true end
							if rec.fullscreen then c.fullscreen = true end
						end

						-- 5. Remaining flags.
						c.ontop  = rec.ontop and true or false
						c.sticky = rec.sticky and true or false
						c.minimized = rec.minimized and true or false
					end)
					if ok then
						applied = applied + 1
					else
						failed = failed + 1
						stellar_log("reconcile: window-state apply FAILED for "
							.. tostring(rec.class) .. " (" .. tostring(rec.wid)
							.. "): " .. tostring(err))
					end
				end
			end
		end
		stellar_log("reconcile: applied window states -- "
			.. applied .. " ok, " .. failed .. " failed, "
			.. tostring(#records) .. " record(s) total")
	end

	-- Raise clients into the recorded stack order. Mirrors the proven
	-- _restore_z_stack convention exactly: screen.clients[1] is the TOP, so we
	-- raise from the highest saved stack index down to 1, and each raise() puts
	-- that client above all previously-raised ones.
	local function apply_z_stack(records)
		if not records then return end

		-- Build a WID -> live client map for this screen.
		local by_wid = {}
		local s = screen[stellar.screen_num] or awful.screen.focused()
		if not s then return end
		for _, c in ipairs(s.all_clients) do
			if c.valid then by_wid[c.window] = c end
		end

		-- Order records by saved stack index ascending (1 = top), then raise
		-- from the bottom (highest index) up to the top (index 1).
		--
		-- Skip windows that were hidden or are fullscreen_desktop: hidden
		-- clients aren't part of the visible stack (raising them is inert and
		-- would only race Conflux's own active-tab raise), and fullscreen_desktop
		-- windows manage their own layering.
		local ordered = {}
		for _, rec in ipairs(records) do
			if rec.wid and rec.stack
			   and not rec.hidden and not rec.fullscreen_desktop then
				table.insert(ordered, rec)
			end
		end
		table.sort(ordered, function(a, b) return a.stack < b.stack end)

		local raised = 0
		for i = #ordered, 1, -1 do
			local rec = ordered[i]
			local c = by_wid[rec.wid]
			if c and c.valid then
				c:raise()
				raised = raised + 1
			end
		end
		stellar_log("reconcile: applied z-stack, raised " .. raised
			.. "/" .. #ordered .. " matched window(s)")
	end

	-- Resolve a saved {wid, instance} identity to a live client. Prefer wid
	-- (stable for normal windows); fall back to instance (Conflux tabs, whose
	-- wid churns across restart).
	local function resolve_ident(ident)
		if not ident then return nil end
		local s = screen[stellar.screen_num] or awful.screen.focused()
		if not s then return nil end
		if ident.wid then
			for _, c in ipairs(s.all_clients) do
				if c.valid and c.window == ident.wid then return c end
			end
		end
		if ident.instance then
			for _, c in ipairs(s.all_clients) do
				if c.valid and c.instance == ident.instance then return c end
			end
		end
		return nil
	end

	-- Restore Awesome focus and Stellar active-client from the tags record.
	-- These are INDEPENDENT (FFM/sloppy focus), so restore each separately:
	-- active-client via set_active_client (keeps the stellar_active property /
	-- signals consistent), focus via client.focus directly. Skip hidden/invalid
	-- targets (focusing a hidden window is meaningless).
	local function apply_focus_active(tags)
		if not tags then return end

		if tags.active then
			local ac = resolve_ident(tags.active)
			if ac and ac.valid and not ac.hidden then
				stellar_api.set_active_client(ac)
				stellar_log("reconcile: restored active client -> "
					.. tostring(ac.class) .. " / " .. tostring(ac.instance))
			end
		end

		if tags.focused then
			local fc = resolve_ident(tags.focused)
			if fc and fc.valid and not fc.hidden then
				client.focus = fc
				stellar_log("reconcile: restored focus -> "
					.. tostring(fc.class) .. " / " .. tostring(fc.instance))
			end
		end
	end

	-- Restore each window's recorded urgency. Runs LAST (after focus restore,
	-- which itself clears urgency on the focused client): activation requests
	-- that land while a window is covered make Awesome's ewmh handler set
	-- urgent=true (_NET_WM_STATE_DEMANDS_ATTENTION) instead of focusing, and
	-- that spurious flag survives the reveal. Overwriting with the recorded
	-- pre-restart value clears the spurious cases and preserves genuine ones.
	local function apply_urgency(records)
		if not records then return end
		local set, cleared = 0, 0
		for _, rec in ipairs(records) do
			local c = resolve_ident(rec)   -- rec carries wid + instance directly
			if c and c.valid then
				local want = rec.urgent and true or false
				if c.urgent ~= want then
					c.urgent = want
					if want then set = set + 1 else cleared = cleared + 1 end
				end
			end
		end
		if set > 0 or cleared > 0 then
			stellar_log("reconcile: urgency restored (" .. set
				.. " set, " .. cleared .. " cleared)")
		end
	end

	-- Called after the restore blob is fully received (RESTORE_STATE_END) to
	-- run the settle-then-apply. We wait for the manage storm to quiesce rather
	-- than a fixed sleep: poll the client count until it is stable across two
	-- checks, then apply. Bounded so it always completes.
	stellar_api._run_reconcile = function()
		if not stellar_api._reconcile_windows then return end

		local last_count = -1
		local stable_ticks = 0
		local elapsed = 0
		local interval = 0.15
		local max_wait = 4.0

		local function tick()
			local s = screen[stellar.screen_num] or awful.screen.focused()
			local count = s and #s.all_clients or 0

			if count == last_count and count > 0 then
				stable_ticks = stable_ticks + 1
			else
				stable_ticks = 0
			end
			last_count = count
			elapsed = elapsed + interval

			-- Apply once the count has held steady for two consecutive ticks,
			-- or the bound is hit.
			if stable_ticks >= 2 or elapsed >= max_wait then
				stellar_log("reconcile: manage storm settled (count=" .. count
					.. ", elapsed=" .. string.format("%.2f", elapsed) .. "s); applying")
				-- Order matters and is all synchronous (one visual update):
				--   1. window states (geometry/floating/flags/tags) applied
				--      while everything is still covered/hidden;
				--   2. reveal to intended visibility -- BEFORE the z-stack,
				--      because raising unmapped (hidden) windows may not
				--      survive the remap;
				--   3. z-stack raise on the now-mapped windows;
				--   4. focus/active (deferred one tick past the raises).
				-- The states pass is pcall-protected as a whole: if it throws,
				-- the reveal below must STILL run -- a covered desktop that
				-- never reveals is a black screen, which is worse than any
				-- mis-applied state.
				local ok_states, err_states =
					pcall(apply_window_states, stellar_api._reconcile_windows)
				if not ok_states then
					stellar_log("reconcile: window-states pass FAILED: "
						.. tostring(err_states))
				end
				local ok_rev, err_rev =
					pcall(reconcile_reveal, stellar_api._reconcile_windows)
				if not ok_rev then
					stellar_log("reconcile: reveal FAILED (" .. tostring(err_rev)
						.. "); emergency unhide of all windows")
					local s2 = screen[stellar.screen_num] or awful.screen.focused()
					if s2 then
						for _, cl in ipairs(s2.all_clients) do
							if cl.valid and not is_tray(cl) then cl.hidden = false end
						end
					end
				end
				apply_z_stack(stellar_api._reconcile_windows)

				-- Restore focus + active client AFTER z-stack, so the targets
				-- are placed and raised. Deferred one tick so any focus churn
				-- from the raises settles first (raising can emit focus events).
				-- Urgency restore follows focus in the same tick: focusing
				-- clears urgency on the focused client, so urgency must be
				-- written after it, per record.
				local tags = stellar_api._reconcile_tags
				local wins = stellar_api._reconcile_windows
				gears.timer.delayed_call(function()
					apply_focus_active(tags)
					apply_urgency(wins)
				end)

				-- Reconcile finished: disarm so normal placement resumes for
				-- any later windows.
				stellar_api._reconcile_active = false
				stellar_api._reconcile_windows = nil
				stellar_api._reconcile_tags = nil
				return false  -- stop timer
			end
			return true  -- keep polling
		end

		gears.timer.start_new(interval, tick)
	end

	-- One-shot failsafe for the startup cover below (armed on first use).
	local startup_cover_failsafe_armed = false

    client.connect_signal("manage", function(c)
-- TODO: change this to set as active client only if normal window w/ titlebars
		stellar_api.set_active_client(c)

		-- Reconcile cover, at the earliest possible moment. Waiting for
		-- RESTORE_STATE_BEGIN to hide (a socket round-trip away) lets the
		-- remanaged windows PAINT first -- the visible flash. awesome.startup
		-- is true exactly while Awesome remanages pre-existing windows, which
		-- is the restart storm, so hide right here at manage. The BEGIN
		-- mass-hide remains as a second net for anything missed.
		--
		-- Marker + failsafe: if this is actually a cold boot with pre-existing
		-- windows (e.g. the whole DE restarted -- no restore blob will ever
		-- arrive, no reveal will run), a one-shot timer unhides exactly the
		-- windows we covered. reconcile_reveal clears the marker on every
		-- window it settles, so after a normal restart the failsafe is a no-op.
		if not is_tray(c) and (stellar_api._reconcile_active or awesome.startup) then
			c.hidden = true
			c._stellar_startup_covered = true
			if not startup_cover_failsafe_armed then
				startup_cover_failsafe_armed = true
				gears.timer.start_new(2.5, function()
					-- If reconcile is mid-flight, leave it alone -- its own
					-- settle/failsafe owns the reveal.
					if not stellar_api._reconcile_active then
						local shown = 0
						local s2 = screen[stellar.screen_num] or awful.screen.focused()
						if s2 then
							for _, cl in ipairs(s2.all_clients) do
								if cl.valid and cl._stellar_startup_covered then
									cl._stellar_startup_covered = nil
									if cl.hidden then
										cl.hidden = false
										shown = shown + 1
									end
								end
							end
						end
						if shown > 0 then
							stellar_log("startup cover failsafe: no reconcile "
								.. "arrived; revealed " .. shown .. " window(s)")
						end
					end
					return false  -- one-shot
				end)
			end
		end

        if c.type == "desktop" and is_fullscreen_desktop(c) then
            fit_to_screen(c)
            c._stellar_show_in_tasklist = true
        else
			-- During restart reconcile, the snapshot is the authority on the
			-- reload path -- suppress the manage-time file geometry restore so
			-- windows don't snap back to stale file geometry. Reconcile applies
			-- z-stack now (3b-1) and full geometry/flags later (3b-2). Brand-new
			-- windows that appear after reconcile disarms use normal placement.
			if stellar_api._reconcile_active then
				stellar_log("reconcile active: skipping file placement for "
					.. tostring(c.class) .. " / " .. tostring(c.instance))
			-- Placement decision. If the monitor name is already resolved
			-- (always true on cold boot thanks to the DE settle barrier), place
			-- immediately. If not (the reload race window, before the async
			-- MONITOR_NAME push arrives), PARK this window and place it when the
			-- name lands in the MONITOR_NAME/MONITOR_CHANGED handler, or when the
			-- safety timer fires. This keeps geometry restore from keying on a
			-- nil name and falling to corner placement.
			elseif monitor_name_resolved() then
				apply_placement(c)
			else
				table.insert(pending_placement, c)
				stellar_log("parking window for placement (name unresolved): "
					.. tostring(c.class) .. " / " .. tostring(c.instance))
			end
		end

        send_line("EVENT type=client_manage screen=" .. tostring(stellar.screen_num) .. " win=" .. tostring(c.window))
    end)

	stellar_api.save_client_geometry = function(c, force_conflux)
		-- Conflux geometry is normally saved explicitly from
		-- conflux.kill_workspace(), which passes force_conflux=true. The
		-- automatic unmanage path must NOT save conflux windows (the workspace
		-- mapping is already being torn down and geometry resolution is
		-- unreliable there), so it calls without the flag and we bail here.
		if c.instance and c.instance:match("^stellar_conflux_") and not force_conflux then
			return
		end

        if (c.floating or awful.layout.get(c.screen).name == "floating")
		and not is_fullscreen_desktop(c) then
            local edid_name = stellar_api.monitor_name  -- nil when unresolved; get_geometry_state_file fails closed
            local state_path = get_geometry_state_file(c, edid_name)
			if state_path then
				local geom = c:geometry()
				local data = string.format("%d,%d,%d,%d", geom.x, geom.y, geom.width, geom.height)

				local file = io.open(state_path, "w")
				if file then
					file:write(data)
					file:close()
					stellar_log("writing " .. tostring(c.name) .. "type=" .. c.type .. " fullscreen_desktop=" .. tostring(is_fullscreen_desktop(c)) .. " data=" .. data)
				end
			end
        end
	end

    client.connect_signal("unmanage", function(c)
		-- Clean up windows when they are closed so the stack doesn't contain dead clients
		for i, v in ipairs(mru_stack) do
			if v == c then
				table.remove(mru_stack, i)
				break
			end
		end

		-- If the window closing was the active one, fallback securely
        if stellar_api._active_client == c then
            local fallback = nil

            -- Scan the MRU stack for the next valid, unminimized window
            for _, v in ipairs(mru_stack) do
                if v and v.valid and not v.minimized then
                    fallback = v
                    break
                end
            end

            -- This cleanly sets the new active client and fires the UI signals.
            -- If fallback is nil (you closed the last window), it safely clears the state.
            stellar_api.set_active_client(fallback)
        end

		stellar_api.save_client_geometry(c)

        send_line("EVENT type=client_unmanage screen=" .. tostring(stellar.screen_num) .. " win=" .. tostring(c.window))
    end)

	client.connect_signal("mouse::enter", function(c)
		stellar_api.log("mouse::enter on " .. tostring(c)
			.. " suppress=" .. tostring(stellar_api._suppress_mouse_enter)
			.. " locked=" .. tostring(stellar_api._z_stack_locked)
			.. " grabber=" .. tostring(mousegrabber.isrunning()))
	end)

	client.connect_signal("mouse::leave", function(c)
		-- If this client was queued during the guard period, cancel it -
		-- the mouse has already left.  The guard timer keeps running so the
		-- cooldown still protects against rapid bouncing.
		if pending_focus_client == c then
			pending_focus_client = nil
		end
	end)

    client.connect_signal("focus", function(c)
	    -- Find and remove the client from its current position in the stack
		for i, v in ipairs(mru_stack) do
			if v == c then
				table.remove(mru_stack, i)
				break
			end
		end
		-- Push the newly focused client to the very top (index 1)
		table.insert(mru_stack, 1, c)

        send_line(
            "EVENT type=client_focus screen="
			.. tostring(stellar.screen_num) .. " win=" .. tostring(c.window) .. " class=" .. tostring(c.class or "")
        )
	    stellar_api.log("FOCUS changed to " .. tostring(c)
			.. " suppress=" .. tostring(stellar_api._suppress_mouse_enter)
			.. " locked=" .. tostring(stellar_api._z_stack_locked)
			.. " grabber=" .. tostring(mousegrabber.isrunning()))
    end)

--[[
    client.connect_signal("focus", function(c)
        send_line(
            "EVENT type=client_focus screen="
			.. tostring(stellar.screen_num) .. " win=" .. tostring(c.window) .. " class=" .. tostring(c.class or "")
        )
    end)

    client.connect_signal("unfocus", function(c)
        send_line(
            "EVENT type=client_unfocus screen="
			.. tostring(stellar.screen_num) .. " win=" .. tostring(c.window) .. " class=" .. tostring(c.class or "")
        )
    end)

    tag.connect_signal("property::selected", function(t)
        if t.selected then
            send_line(
                "EVENT type=tag_selected screen="
				.. tostring(stellar.screen_num) .. " tag=" .. tostring(t.name)
            )
        end
    end)
]]--

	client.connect_signal("property::maximized", function(c)
		if c.maximized and c.prevent_fullscreen then
        	c.maximized = false
    	end
		update_locked_state(c)
	end)

	client.connect_signal("property::fullscreen", function(c)
		if c.fullscreen and c.prevent_fullscreen then
			stellar_log("fullscreen BLOCKED for " .. tostring(c.name))
			c.fullscreen = false
			return
		end

		stellar_log("*** property::fullscreen has been triggered!!! ***")
		update_locked_state(c)

		local s = c.screen
		if c.fullscreen then
			stellar_log("entering fullscreen for " .. tostring(c.name) .. "type=" .. c.type)
			c:raise()
			client.focus = c
		else
			if is_fullscreen_desktop(c) then
				stellar_log("exiting fullscreen for " .. tostring(c.name) .. "type=" .. c.type .. " (fullscreen_desktop window)")
				if c.should_maximize then
					fit_to_screen(c)
				end
			else
				stellar_log("exiting fullscreen for " .. tostring(c.name) .. "type=" .. c.type)
			end
		end

        send_line(
            "EVENT type=client_fullscreen screen="
			.. tostring(stellar.screen_num) .. " win=" .. tostring(c.window) .. " fullscreen=" .. tostring(c.class or "")
        )
	end)

--[[
	client.connect_signal("property::geometry", function(c)
		-- If this is a flagged conflux terminal window that just received its real size
		if c._needs_mux_placement_fix then
			-- Immediately remove the flag so we don't interfere with manual user resizes later
			c._needs_mux_placement_fix = false

			-- Now that it has its true dimensions, re-apply the standard placement rules
			-- TODO: use global settings set in rc.lua
			awful.placement.no_overlap(c)
			awful.placement.no_offscreen(c)
		end
	end)
]]--

	-- Serialize full per-window state and stream it to the DE, one window per
	-- line (RESTORE_STATE_WIN), bracketed by BEGIN/END, plus one TAGS line for
	-- per-tag layout and the selected tag. The DE holds these opaquely and
	-- replays them after the restart. One window per line keeps every line well
	-- under the DE's 4096-byte IPC line buffer.
	--
	-- Excludes stalonetray (Stellar-owned, not affected by an Awesome restart).
	-- fullscreen_desktop windows are marked so the (future) reconcile routes
	-- them to the hidden/fit path instead of geometry restore.
	--
	-- NOTE (transport milestone): this only SERIALIZES and SENDS. The restored
	-- Lua currently just parses and logs what comes back; reconciliation is a
	-- later step.
	local function is_stalonetray(c)
		return c.class == "stalonetray"
			or c.instance == "stalonetray"
			or c.name == "stalonetray"
	end

	local function tag_indices(c)
		-- Record which of this screen's tags the client is on, by tag index
		-- (stable, layout-independent). Also record its position within each
		-- tag's client list (tile-slot order) for tiled reconcile.
		local out = {}
		local s = c.screen
		if not s then return out end
		for ti, t in ipairs(s.tags) do
			local clients = t:clients()
			for ci, cc in ipairs(clients) do
				if cc == c then
					table.insert(out, { tag = ti, index = ci })
					break
				end
			end
		end
		return out
	end

	local function serialize_window(c, stack_index)
		local fsd = is_fullscreen_desktop(c)

		-- Resolve ONE trusted geometry value for the payload.
		--
		-- While a window has an inflating atom set (fullscreen, or horizontal/
		-- vertical maximize), c:geometry() returns the fake inflated rect, not
		-- where the window really lives. c._saved_floating_geom holds the real
		-- underlying rect. It is a Lua property so it dies with the Lua state on
		-- restart -- but we read it HERE, at exit, before the re-exec, and bake
		-- the resolved result into the payload's single geometry value. Nothing
		-- separate is transmitted; the geometry field already IS the real rect.
		--
		-- Rule: if inflated, substitute _saved_floating_geom; if that is missing,
		-- omit geometry (restore falls back to default placement -- shouldn't
		-- happen in practice). If not inflated, use live geometry (or sfg if the
		-- live value isn't set).
		local sfg = c._saved_floating_geom

		local inflated = c.fullscreen or c.maximized
			or c.maximized_horizontal or c.maximized_vertical
		local geo
		if inflated then
			geo = sfg   -- may be nil; handled by the (geo and ...) guard below
		else
			geo = sfg or c:geometry()
		end

		return {
			wid        = c.window,
			class      = c.class,
			instance   = c.instance,
			name       = c.name,
			tags       = tag_indices(c),
			stack      = stack_index,          -- position in screen.clients (z-order)
			floating   = c.floating and true or false,
			-- geometry: the trusted uninflated rect. For an inflated window
			-- (fullscreen/maximize) this is the substituted saved_floating_geom;
			-- for a normal window it's the live geometry. Omitted for
			-- fullscreen_desktop, or for an inflated window with no saved
			-- geometry to substitute (geo is nil then -> default placement on
			-- restore; in practice this shouldn't happen).
			geometry   = (not fsd and geo)
				and { x = geo.x, y = geo.y, width = geo.width, height = geo.height } or nil,
			minimized  = c.minimized and true or false,
			-- Maximize is captured as the full flag plus both axes, since a
			-- single-axis maximize has c.maximized == false but still inflates
			-- one dimension. Restore sets the axes so the display matches.
			maximized  = c.maximized and true or false,
			maximized_horizontal = c.maximized_horizontal and true or false,
			maximized_vertical   = c.maximized_vertical and true or false,
			fullscreen = c.fullscreen and true or false,
			ontop      = c.ontop and true or false,
			sticky     = c.sticky and true or false,
			hidden     = c.hidden and true or false,   -- intended hidden state (pre any cover)
			-- Urgency is captured so reconcile can restore the pre-restart
			-- truth: activation requests that land while a window is COVERED
			-- (hidden) make Awesome's ewmh handler set urgent=true instead of
			-- focusing (that is _NET_WM_STATE_DEMANDS_ATTENTION), and that
			-- spurious flag would otherwise survive the reveal.
			urgent     = c.urgent and true or false,
			fullscreen_desktop = fsd and true or false,
		}
	end

	local function serialize_tags()
		local s = screen[stellar.screen_num] or awful.screen.focused()
		local out = { layouts = {}, selected = nil, focused = nil, active = nil }
		if not s then return out end
		for ti, t in ipairs(s.tags) do
			-- Per-tag layout name (each tag can have its own layout).
			out.layouts[ti] = t.layout and t.layout.name or nil
			if t.selected then out.selected = ti end
		end

		-- Focused window (Awesome input focus) and active client (Stellar's
		-- committed/clicked window) are INDEPENDENT under FFM/sloppy focus, so
		-- record both. Identify each by wid (normal windows) and instance
		-- (Conflux tabs, whose wid churns across restart) so restore can match
		-- either way.
		local function ident(c)
			if not c or not c.valid then return nil end
			-- Only record if this client belongs to this screen.
			if c.screen ~= s then return nil end
			return { wid = c.window, instance = c.instance }
		end

		out.focused = ident(client.focus)
		out.active  = ident(stellar_api._active_client)

		return out
	end

	local function send_restart_snapshot()
		local s = screen[stellar.screen_num] or awful.screen.focused()
		send_line("RESTORE_STATE_BEGIN screen=" .. tostring(stellar.screen_num))

		if s then
			-- Pass 1: s.clients is the VISIBLE stacking order (index 1 = top);
			-- stack_index captures z-order directly.
			local idx = 0
			local seen = {}
			for _, c in ipairs(s.clients) do
				if c.valid and not is_stalonetray(c) then
					idx = idx + 1
					seen[c] = true
					local rec = serialize_window(c, idx)
					local ok, encoded = pcall(json.encode, rec)
					if ok and encoded then
						send_line("RESTORE_STATE_WIN screen=" .. tostring(stellar.screen_num)
							.. " " .. encoded)
					else
						stellar_log("restore snapshot: json.encode failed for "
							.. tostring(c.class))
					end
				end
			end

			-- Pass 2: windows NOT in the visible stack -- hidden (e.g. Conflux
			-- siblings) and minimized ones. s.clients excludes them, so without
			-- this pass they'd have no record at all: the reveal's default-
			-- unhide would expose hidden Conflux siblings, and minimized
			-- windows would silently lose their state. stack=nil (not part of
			-- the visible z-order; the z-stack apply skips them).
			for _, c in ipairs(s.all_clients) do
				if c.valid and not seen[c] and not is_stalonetray(c) then
					local rec = serialize_window(c, nil)
					local ok, encoded = pcall(json.encode, rec)
					if ok and encoded then
						send_line("RESTORE_STATE_WIN screen=" .. tostring(stellar.screen_num)
							.. " " .. encoded)
					else
						stellar_log("restore snapshot: json.encode failed for "
							.. tostring(c.class))
					end
				end
			end
		end

		local ok, tags_encoded = pcall(json.encode, serialize_tags())
		if ok and tags_encoded then
			send_line("RESTORE_STATE_TAGS screen=" .. tostring(stellar.screen_num)
				.. " " .. tags_encoded)
		end

		-- Conflux tab order + active tab per workspace, kept as a SEPARATE line
		-- (workspace-scoped, not window-scoped). Reuses the same data shape as
		-- relocate_workspace. Restored via conflux.seed_restart_restore() before
		-- the orphaned tabs remanage, which fixes tab order/active-tab scramble
		-- on restart.
		do
			local ok_cx, conflux = pcall(require, "modules.conflux")
			if ok_cx and conflux and conflux.serialize_all_workspaces then
				local ws_list = conflux.serialize_all_workspaces()
				if ws_list and #ws_list > 0 then
					local ok_enc, cx_encoded = pcall(json.encode, ws_list)
					if ok_enc and cx_encoded then
						send_line("RESTORE_CONFLUX screen=" .. tostring(stellar.screen_num)
							.. " " .. cx_encoded)
						stellar_log("restore snapshot: sent conflux state for "
							.. #ws_list .. " workspace(s)")
					end
				end
			end
		end

		send_line("RESTORE_STATE_END screen=" .. tostring(stellar.screen_num))
		stellar_log("restore snapshot sent for screen " .. tostring(stellar.screen_num))
	end

	awesome.connect_signal("exit", function(reason_restart)
		if reason_restart then
			stellar_log("AwesomeWM is restarting. Sending RESTARTING event to DE...")

			-- Serialize and hand the full window state to the DE, which holds
			-- it across the restart and replays it on the sync handshake. This
			-- captures live c:geometry() directly into the snapshot, so it is
			-- the single source of truth for the reload path -- it does NOT go
			-- through the geometry state files at all.
			--
			-- (The previous per-client batch save into the state files was
			-- removed: it duplicated the snapshot, could write stale data, and
			-- the manage-time file restore it fed will be suppressed on the
			-- reload path once reconcile applies the snapshot. The state files
			-- remain the mechanism for cold boot / individual close+reopen, fed
			-- by the unmanage handler.)
			send_restart_snapshot()

			send_line("EVENT type=awesome_restarting screen=" .. tostring(stellar.screen_num))
		else
			stellar_log("AwesomeWM is quitting. Sending QUITTING event to DE...")
			-- Include screen= so the DE clears only this screen's held blob.
			send_line("EVENT type=awesome_quitting screen=" .. tostring(stellar.screen_num))
		end

		-- Give the socket a brief moment to flush the buffer before we sever it
        if stellar.sock and stellar.sock.settimeout then stellar.sock:settimeout(1) end
		disconnect()
	end)
end

local base_path = get_required_env("STELLAR_AWESOME_BASE")
local settings = load_stellar_settings()
stellar_api.stellar_settings = settings

-- Apply the initial mouse focus mode from settings.json.
-- (apply_stellar_settings only runs on live-reload via IPC, so the
-- first session needs this explicit initialization.)
if settings and settings.mouse and settings.mouse.focus_mode then
	focus_mode = settings.mouse.focus_mode
	stellar_api.log("stellar: initial focus_mode = " .. focus_mode)
end
if settings and settings.mouse and settings.mouse.focus_cooldown then
	focus_cooldown = settings.mouse.focus_cooldown
	focus_timer.timeout = focus_cooldown
	stellar_api.log("stellar: initial focus_cooldown = " .. focus_cooldown)
end

install_stellar_hooks(socket_unix)

-- Establish the theme BEFORE loading the base config. stellar_theme.init:
--   * configures beautiful DPI,
--   * resolves the DPI-appropriate sprite directory,
--   * loads theme_data.lua,
--   * assembles + installs the full beautiful table (beautiful.init),
--   * applies the appearance font from settings.json.
-- Doing this first means every wibar/titlebar built by rc.lua picks up
-- correct colours, font, gaps, and border_width on the first build - no
-- "init then patch" afterwards.
local stellar_theme = require("modules.stellar_theme")
stellar_theme.init {
	settings = settings,
	log      = stellar_api.log,
	screen   = stellar_screen,
}

-- The slice modules (stellar_ui / stellar_window) read this directly, so
-- mirror the resolved sprite directory onto the shared api table.
stellar_api.theme_assets_path = stellar_theme.assets_path()

-- Wallpaper is settings-driven and per-screen; it lives in its own module.
-- rc.lua's request::wallpaper handler calls stellar_wallpaper.apply(s).
local stellar_wallpaper = require("modules.stellar_wallpaper")
stellar_wallpaper.init {
	screen = stellar_screen,
	log    = stellar_api.log,
}

load_base_config(base_path)

apply_window_rules(settings)

