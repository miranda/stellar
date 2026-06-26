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
    stellar_log("ERROR: " .. tostring(err))
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

-- MRU
local mru_stack = {}

stellar_api.mru_cycle = function(modkey)
	-- Bail out if there aren't enough windows to cycle through
	if #mru_stack < 2 then return end

	-- Create a static snapshot of valid clients for the current screen
	local cycle_list = {}
	for _, c in ipairs(mru_stack) do
		-- Filter out minimized windows and windows on other monitors
		if c.screen == awful.screen.focused() and not c.minimized then
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

local function is_fullscreen_desktop(c)
	return c:get_xproperty("_STELLAR_FULLSCREEN_DESKTOP")
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

    awful.spawn.easy_async_with_shell(scrot_cmd, function()
        flash_screen()
        play_system_sound(soundfile)
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

                -- Cancel the run prompt if it is active
                if _G.cancel_run_prompt then
                    _G.cancel_run_prompt()
                end

				-- Cancel the desktop context menu if it is active
				if _G.cancel_desktop_menu then
					_G.cancel_desktop_menu()
				end
			end

            -- We do absolutely nothing when leaving. AwesomeWM keeps its memory naturally!
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



    client.connect_signal("manage", function(c)
-- TODO: change this to set as active client only if normal window w/ titlebars
		stellar_api.set_active_client(c)

		if c.type == "desktop" and is_fullscreen_desktop(c) then
			fit_to_screen(c)
			c._stellar_show_in_tasklist = true
		end

		if c.class and c.class:match("^term_%d+$") then
			-- We apply a custom property to mark it for a post-startup fix
			c._needs_mux_placement_fix = true
		end

		send_line(
            "EVENT type=client_manage screen="
			.. tostring(stellar.screen_num) .. " win=" .. tostring(c.window) .. " class=" .. tostring(c.class or "")
        )
    end)

    client.connect_signal("unmanage", function(c)
		if stellar_api._active_client and stellar_api._active_client == c then
			stellar_api._active_client = nil
		end

		local previous = awful.client.focus.history.previous()
		if previous and previous.valid then
			stellar_api._active_client = previous
		end

		-- Clean up windows when they are closed so the stack doesn't contain dead clients
		for i, v in ipairs(mru_stack) do
			if v == c then
				table.remove(mru_stack, i)
				break
			end
		end

		send_line(
            "EVENT type=client_unmanage screen="
			.. tostring(stellar.screen_num) .. " win=" .. tostring(c.window) .. " class=" .. tostring(c.class or "")
        )
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

	awesome.connect_signal("exit", function(reason_restart)
		if reason_restart then
			stellar_log("AwesomeWM is restarting. Sending RESTARTING event to DE...")
			send_line("EVENT type=awesome_restarting screen=" .. tostring(stellar.screen_num))
		else
			stellar_log("AwesomeWM is quitting. Sending QUITTING event to DE...")
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

