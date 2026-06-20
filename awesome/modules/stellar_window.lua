local awful = require("awful")
local gears = require("gears")
local surface = require("gears.surface")
local wibox = require("wibox")
local beautiful = require("beautiful")
local xresources = require("beautiful.xresources")
local dpi = xresources.apply_dpi
local json = require("dkjson")
local stellar_ui = require("modules.stellar_ui")
local conflux = require("modules.conflux")
local M = {}

local grab_outline = {}
local active_outline_client = nil
local tab_popup_offset = -(dpi(8))

local function is_tiled_client(c)
    local is_tiled = false

    local t = c.first_tag or (c.screen and c.screen.selected_tag)
    local is_floating_layout = t and (t.layout.name == "floating")

    -- If the layout isn't floating, and the client isn't floating
	if not is_floating_layout and not c.floating then
		is_tiled = true
	end

	return is_tiled
end

local function is_tabbed_client(c)
    -- Fallback to empty string if nil to avoid pattern matching errors
    local instance_name = c.instance or ""
    local class_name = c.class or ""
    return instance_name:match("^stellar_conflux_") or class_name:match("^stellar_conflux_")
end

local original_mousegrabber_run = mousegrabber.run
mousegrabber.run = function(func, cursor)
    stellar_api.log("mousegrabber started")

    -- Warp-induced mouse::enter events are processed this tick
    -- Allow them again on next idle
    gears.timer.delayed_call(function()
        if stellar_api._grab then
            stellar_api._grab.settled = true
        end
    end)

    local started_with_button = false

    local wrapped_func = function(mouse_args)
        local has_buttons = false
        for _, pressed in pairs(mouse_args.buttons) do
            if pressed then
                has_buttons = true
                started_with_button = true
                break
            end
        end

        -- Workarounds for Awesome's mousegrabber quirks and lack of callbacks in tiled mode
        if started_with_button and not has_buttons then
            -- Determine if this is a problematic tiled operation
            local grab = stellar_api._grab
			local is_tiled_problematic = grab
				and (grab.action == "mouse_resize" or grab.action == "mouse_move")
				and grab.client.valid
				and is_tiled_client(grab.client)

            if is_tiled_problematic then
                -- Force stop without calling func (the double-stop case)
                mousegrabber.stop()
                return false
            else
                -- Let func process the release (snap-to-edge, floating, etc.)
                local result = func(mouse_args)
                if result == false then
                	mousegrabber.stop()
                    return false
                end
                -- If func didn't stop itself, force it
                mousegrabber.stop()
                return false
            end
        end

        local result = func(mouse_args)
        if result == false then
            mousegrabber.stop() 
        end
        return result
    end

    return original_mousegrabber_run(wrapped_func, cursor)
end

local original_mousegrabber_stop = mousegrabber.stop
mousegrabber.stop = function(...)
    local grabbed_c = active_outline_client
    if grabbed_c then
        hide_outline(grabbed_c)
    end

	local function focus_under_cursor()
		local under_cursor = mouse.current_client
		if under_cursor and under_cursor.valid and client.focus ~= under_cursor then
			under_cursor:emit_signal("request::activate", "mouse_enter", {raise = false})
		end
	end

    local grab = stellar_api._grab

    if grab then
        stellar_api.log("*** MOUSEGRABBER STOPPED *** grab client=" .. tostring(grab.client))

		local ok, err = pcall(stellar_api._restore_z_stack, grab.z_stack)
		if not ok then
			stellar_api.log("z_stack restore failed: " .. tostring(err))
		end

        -- Nudge pointer for floating resize
        if grab.client.valid and grab.action == "mouse_resize" and not is_tiled_client(grab.client) then
            local g = grab.client:geometry()
            local coords = mouse.coords()
            local margin = 4
            local nx, ny = coords.x, coords.y

            if coords.x >= g.x + g.width then nx = g.x + g.width - margin end
            if coords.y >= g.y + g.height then ny = g.y + g.height - margin end

            if nx ~= coords.x or ny ~= coords.y then
                mouse.coords({ x = nx, y = ny }, true)
            end
        end

        -- Focus correction only if grab has settled (past the warp phase)
        if grab.settled then
            stellar_api._grab = nil
            gears.timer.delayed_call(focus_under_cursor)
        else
            -- First stop (pre-warp phase) - don't clear grab, don't focus
        end
    else
        stellar_api.log("*** MOUSEGRABBER STOPPED (no grab client) ***")

		gears.timer.delayed_call(focus_under_cursor)
    end

    return original_mousegrabber_stop(...)
end

local snap_module = require("awful.mouse.snap")
local resize = require("awful.mouse.resize")

-- Disable edge snapping by default - we'll enable it dynamically
snap_module.edge_enabled = false

local drag_start = nil
local drag_threshold = 10  -- pixels

resize.add_move_callback(function(c, geo, args)
    if not drag_start then
        local coords = mouse.coords()
        drag_start = { x = coords.x, y = coords.y }
    end

    local coords = mouse.coords()
    local dx = coords.x - drag_start.x
    local dy = coords.y - drag_start.y

    if dx * dx + dy * dy >= drag_threshold * drag_threshold then
        snap_module.edge_enabled = true
    end
end, "mouse.move")

resize.add_leave_callback(function(c, _, args)
    drag_start = nil
    snap_module.edge_enabled = false

	-- Nudge windows back in from screen edge to correct accidental overshoots
    if c and c.valid then
        local wa = c.screen.workarea
        local g = c:geometry()
        local bw2 = 2 * c.border_width
        local threshold = dpi(2)

        -- Left edge
        if g.x < wa.x and (wa.x - g.x) <= threshold then
            g.x = wa.x
        end

        -- Top edge
        if g.y < wa.y and (wa.y - g.y) <= threshold then
            g.y = wa.y
        end

        -- Right edge
        local right_overshoot = (g.x + g.width + bw2) - (wa.x + wa.width)
        if right_overshoot > 0 and right_overshoot <= threshold then
            g.x = g.x - right_overshoot
        end

        -- Bottom edge
        local bottom_overshoot = (g.y + g.height + bw2) - (wa.y + wa.height)
        if bottom_overshoot > 0 and bottom_overshoot <= threshold then
            g.y = g.y - bottom_overshoot
        end

        c:geometry(g)
    end
end, "mouse.move")

-- Evaluator and Mode Getters

local function get_program_name(c)
    if not c then
        return ""
    end

    local source = c.stellar_title_source

    if source == "program" or source == nil then
        if c.pid then
            -- Read the binary name directly from the Linux procfs
            local f = io.open("/proc/" .. tostring(c.pid) .. "/comm", "r")

            if f then
                local name = f:read("*l")
                f:close()

                if name and name ~= "" then
                    return name
                end
            end
        end
    elseif source == "class" or source == "instance" or source == "name" then
        local value = c[source]

        if value and value ~= "" then
            return value
        end
    end

    -- Fallback
    return c.name or c.class or c.instance or ""
end

local function evaluate_state(c)
    if not c or not c.valid then return end

    local is_locked = false
    local is_tiled = is_tiled_client(c) -- false -- Our new state

    -- 1. Determine States
    if c.maximized or c.fullscreen then
        is_locked = true
        is_tiled = false -- Maximized usually supersedes tiling
    elseif is_tiled then
		-- Check for the "only one tiled window" condition
		local tiled_clients = 0
		for _, cl in ipairs(c.screen.clients) do
			if not cl.floating and not cl.minimized then
				tiled_clients = tiled_clients + 1
			end
		end
		if tiled_clients <= 1 then
			is_locked = true
		end
    end

    -- 2. Apply Locked State
    if c.stellar_locked ~= is_locked then
        c.stellar_locked = is_locked
        -- Passing nil physically removes the property from X11
        c:set_xproperty("_STELLAR_LOCKED", is_locked or nil) 
        c:emit_signal("property::stellar_locked")

        if is_locked then
            hide_outline(c)
        end
    end

    -- 3. Apply Tiled State
    if c.stellar_tiled ~= is_tiled then
        c.stellar_tiled = is_tiled
        -- Passing nil removes it when not tiled
        c:set_xproperty("_STELLAR_TILED", is_tiled or nil)
        c:emit_signal("property::stellar_tiled")
    end
end

-- Outline Management
local function make_line(color)
    local wb = wibox({ visible = false, ontop = true, bg = color })
    pcall(function() wb.input_passthrough = true end)
    return wb
end

local function get_outline(c)
    if grab_outline[c] then return grab_outline[c] end

    local color = "#ffffff"
    local outline = {
        top = make_line(color),
        bottom = make_line(color),
        left = make_line(color),
        right = make_line(color),
        width = beautiful.grab_border_width or 1,
        color = color,
    }

    grab_outline[c] = outline

    c:connect_signal("unmanage", function()
        local o = grab_outline[c]
        if o then
            o.top.visible = false
            o.bottom.visible = false
            o.left.visible = false
            o.right.visible = false
            grab_outline[c] = nil
        end
        if active_outline_client == c then
            active_outline_client = nil
        end
    end)

    return outline
end

local function update_outline(c)
    local o = grab_outline[c]
    if not o then return end

    local g = c:geometry()
    local bw = o.width or 1

    o.top.x = g.x - bw
    o.top.y = g.y - bw
    o.top.width = g.width + (bw * 2)
    o.top.height = bw

    o.bottom.x = g.x - bw
    o.bottom.y = g.y + g.height
    o.bottom.width = g.width + (bw * 2)
    o.bottom.height = bw

    o.left.x = g.x - bw
    o.left.y = g.y - bw
    o.left.width = bw
    o.left.height = g.height + (bw * 2)

    o.right.x = g.x + g.width
    o.right.y = g.y - bw
    o.right.width = bw
    o.right.height = g.height + (bw * 2)
end

-- Decouple the visual outline from the raw property::geometry signal.
-- property::geometry fires once per microscopic mouse movement; synchronizing
-- four large wiboxes on every tick floods the single-threaded XCB queue
-- (catastrophic on the 60Hz/2880px DualUp). Instead we mark the active client
-- dirty and flush at most ~60fps via this timer, collapsing thousands of X11
-- move requests per second down to ~60.
local outline_throttle_pending = nil
local outline_throttle_timer = gears.timer {
    timeout = 1 / 60,
    callback = function()
        local c = outline_throttle_pending
        if c and c.valid then
            local o = grab_outline[c]
            if o and o.top.visible then
                update_outline(c)
            end
        end
        -- Nothing left to draw - stop the timer so it isn't spinning idle.
        outline_throttle_pending = nil
        outline_throttle_timer:stop()
    end
}

-- Request a throttled outline refresh for client `c`. Cheap to call on every
-- geometry tick: it only stores the client and ensures the timer is running.
local function request_outline_update(c)
    outline_throttle_pending = c
    if not outline_throttle_timer.started then
        outline_throttle_timer:start()
    end
end

local function show_outline(c, color, width)
	if c.stellar_locked then return end

    local o = get_outline(c)

    o.color = color or beautiful.grab_border_color or "#ffffff"
    o.width = width or beautiful.grab_border_width or 1

    o.top.bg = o.color
    o.bottom.bg = o.color
    o.left.bg = o.color
    o.right.bg = o.color

    update_outline(c)

    o.top.visible = true
    o.bottom.visible = true
    o.left.visible = true
    o.right.visible = true
end

function hide_outline(c)
    local o = grab_outline[c]
    if not o then return end

    -- Cancel any queued throttled redraw for this client so the timer
    -- doesn't re-show the outline one frame after we hide it.
    if outline_throttle_pending == c then
        outline_throttle_pending = nil
    end

    o.top.visible = false
    o.bottom.visible = false
    o.left.visible = false
    o.right.visible = false
    
    if active_outline_client == c then
        active_outline_client = nil
    end
end

local function begin_grab(c, action)
-- TODO: revisit locked state handling
--    if c.stellar_locked then
--        c:activate { context = "titlebar", action = "mouse_click" }
--        return
--    end

    if active_outline_client and active_outline_client ~= c then
        hide_outline(active_outline_client)
    end
    
    active_outline_client = c

    show_outline(
        c,
        beautiful.grab_border_color or "#ffffff",
        beautiful.grab_border_width or 1
    )

	stellar_api.mouse_action(c, "titlebar", action)
end

local function update_tab_bar(c, slice_data, frame_buttons)
    if not c or not c.valid then return end

    local current_workspace = conflux.workspaces[c.instance]
    if not current_workspace then return end

    local tabs = conflux.get_tab_order(current_workspace)
    if #tabs == 0 then return end

    local tab_height = slice_data["tab"]["lower_left"].h

    -- ========================================
    -- INITIALIZE TITLEBAR + STRIP EXACTLY ONCE
    -- ========================================
    if not c._stellar_tab_strip then

        -- Create the tab strip widget via stellar_ui
        c._stellar_tab_strip = stellar_ui.tab_strip({
            slice_data    = slice_data,
            category      = "tab",
            max_tab_width = 250,
            min_tab_width = 80,
            min_margin    = 4,

            signal_source   = c,
            refresh_signals = stellar_ui.default_refresh_signals,

            new_button = {
                on_click = function()
                    conflux.spawn_new(current_workspace)
                    if active_outline_client then
                        hide_outline(active_outline_client)
                    end
                end,
            },

            get_slice_image = function(category, name, variation)
                return stellar_ui.get_slice_image(c, category, name, variation)
            end,

            -- Sync X property whenever the strip's pixel width changes
			on_fit = function(w, h)
				if not c.valid then return end
				if c._stellar_tab_total_width ~= w then
					c._stellar_tab_total_width = w
					gears.timer.delayed_call(function()
						if c.valid then
							local rect_string = string.format("bl:0,0,%d,%d", w, tab_height)
							c:set_xproperty("_STELLAR_TAB_BAR_RECT", rect_string)
						end
					end)
				end
			end,
        })

        -- Generate the X11 titlebar surface (once)
        local bottom_titlebar = awful.titlebar(c, { position = "bottom", size = tab_height })
        bottom_titlebar:setup {
            c._stellar_tab_strip,
            stellar_ui.create_slice(c, "tab", "lower_flex",  true,  slice_data),
            stellar_ui.create_slice(c, "tab", "lower_right", false, slice_data),
            layout  = wibox.layout.align.horizontal,
            buttons = frame_buttons,
        }

        -- Create a single reusable popup for tab context menus.
        -- Content is rebuilt on each hover so closures reference the correct tab.
        c._stellar_tab_hover = stellar_ui.hover_popup({
            bg           = beautiful.bg_normal,
            border_width = 2,
            border_color = "#3300cc",
            timeout      = 0.3,
        })

        local popup = c._stellar_tab_hover.popup

        -- Reposition when the popup resizes so the bottom-left anchor stays put
        local function reanchor_tab_popup()
            if c.valid and popup.visible and c._stellar_tab_popup_anchor_y then
                popup.x = c._stellar_tab_popup_anchor_x + tab_popup_offset
                popup.y = c._stellar_tab_popup_anchor_y - popup.height
            end
        end
        popup:connect_signal("property::width",  reanchor_tab_popup)
        popup:connect_signal("property::height", reanchor_tab_popup)

        -- Dismiss when the window loses focus
        c:connect_signal("unfocus", function()
            if c.valid and c._stellar_tab_hover then
                c._stellar_tab_hover:hide()
            end
        end)

        -- Clean up on client destruction
        c:connect_signal("unmanage", function()
            if c._stellar_tab_hover then
                c._stellar_tab_hover:destroy()
                c._stellar_tab_hover = nil
            end
            if c._stellar_tab_strip then
                c._stellar_tab_strip:disconnect_all()
            end
        end)
    end

    -- ============================
    -- BUILD DESCRIPTORS AND UPDATE
    -- ============================
    local N     = #tabs
    local popup = c._stellar_tab_hover.popup

    local descriptors = {}

    for i, instance_name in ipairs(tabs) do
        local is_active  = (instance_name == c.instance)
        local tab_client = conflux.find_client_for_workspace(instance_name)
        local text_color = is_active and "#FFFFFF" or "#AAAAAA"

        -- Build the label widget
        local label
        if tab_client then
            local t_widget = awful.titlebar.widget.titlewidget(tab_client)
            t_widget.halign = "center"
            t_widget.valign = "center"

            label = wibox.widget {
				t_widget,
                fg     = text_color,
                widget = wibox.container.background,
            }
        else
            local safe_title = gears.string.xml_escape(current_workspace)
            label = wibox.widget {
                markup = "<span foreground='" .. text_color .. "'> " .. safe_title .. " </span>",
                valign = "center",
                halign = "center",
                widget = wibox.widget.textbox,
            }
        end

        table.insert(descriptors, {
            id         = instance_name,
            is_active  = is_active,
            label      = label,
            text_color = text_color,

            on_click = function()
                conflux.activate_tab(instance_name)
            end,

            on_context_enter = function(img_right)
                if not c.valid then return end
                if not c._stellar_tab_hover then return end

                c._stellar_tab_hover.timer:stop()

                -- If already visible, just keep it alive - don't rebuild
                if popup.visible then return end

                -- Build fresh menu content for THIS tab
                local menu_layout = wibox.layout.fixed.vertical()

                local function add_item(text, callback)
                    menu_layout:add(stellar_ui.menu_item({
                        text     = text,
                        callback = function()
                            c._stellar_tab_hover:hide()
                            callback()
                        end,
                    }))
                end

                if i > 1 then
                    add_item("Move Left",  function() conflux.move_tab(instance_name, -1) end)
                end
                if i < N then
                    add_item("Move Right", function() conflux.move_tab(instance_name,  1) end)
                end
                if N > 1 then
                    add_item("Detach",     function() conflux.detach_tab(instance_name) end)
                end
                add_item("Terminate",      function() conflux.terminate_tab(instance_name) end)

                popup:setup {
                    menu_layout,
                    margins = 6,
                    widget  = wibox.container.margin,
                }

                -- Anchor: client left edge + widget offset, top of bottom titlebar
                local geo = c:geometry()
                c._stellar_tab_popup_anchor_x = geo.x + (img_right._tab_strip_drawable_x or 0)
                c._stellar_tab_popup_anchor_y = geo.y + geo.height - tab_height

                popup.x = c._stellar_tab_popup_anchor_x + tab_popup_offset
                popup.y = c._stellar_tab_popup_anchor_y - (popup.height or 0)
                popup.visible = true
            end,

            on_context_leave = function()
                if c.valid and c._stellar_tab_hover then
                    c._stellar_tab_hover:schedule_hide()
                end
            end,
        })
    end

    c._stellar_tab_strip:update(descriptors)
end

-- Setup and Global Bindings

function M.setup()
    local slice_data = stellar_ui.load_slice_data(stellar_api.theme_assets_path)
	local floating_geometries = {}

    -- Per-client pending updates. Most events only need to refresh the client they fire on.
    -- pending_full_sweep is set for events with cross-client side effects (layout change,
    -- tagging changes) where evaluate_state's tiled_clients count can flip on other windows.
    local pending_clients = {}
    local pending_full_sweep = false

    local function process_client(c)
        if not c.valid then return end
        evaluate_state(c)

        if c ~= client.focus and grab_outline[c] and grab_outline[c].top.visible then
            hide_outline(c)
        end

        stellar_ui.update_decorations(c)
    end

    -- Debouncer for state evaluation.
    -- 0.2s (was 0.05s): a tight 50ms window could fire between erratic mouse
    -- polls mid-drag, triggering process_client -> update_decorations ->
    -- stellar::refresh_slices, which reloads every titlebar's Cairo surface on
    -- the CPU and freezes the drag. 0.2s comfortably clears any realistic
    -- inter-poll gap so the reload only happens once motion has settled.
    local global_state_timer = gears.timer {
        timeout = 0.2,
        single_shot = true,
        callback = function()
            if pending_full_sweep then
                for _, c in ipairs(client.get()) do
                    process_client(c)
                end
            else
                for c, _ in pairs(pending_clients) do
                    process_client(c)
                end
            end
            pending_clients = {}
            pending_full_sweep = false
        end
    }

    -- Queue an update for a single client. Pass nil/non-client to request a full sweep.
    local function queue_update(c)
        -- Use pcall because tag objects might error on .window access
        local is_client = false
        if c then
            local ok, w = pcall(function() return c.window end)
            is_client = ok and w ~= nil
        end

        if is_client and c.valid then
            pending_clients[c] = true
        else
            pending_full_sweep = true
        end
        global_state_timer:again()
    end

    local function queue_sweep()
        pending_full_sweep = true
        global_state_timer:again()
    end

    local function evaluate_screen(s)
        if not s then return end
        for _, c in ipairs(s.clients) do
            evaluate_state(c)
        end
    end

    client.connect_signal("property::geometry", function(c)
	    if not c.valid then return end

        queue_update(c)
        local o = grab_outline[c]
        if o and o.top.visible then
            -- Throttled to ~60fps instead of running on every raw geometry tick.
            request_outline_update(c)
        end

		local layout = awful.layout.get(c.screen)
		-- If the layout is floating, but the window technically isn't, track its real position
		if layout == awful.layout.suit.floating and not c.floating then
			floating_geometries[c] = c:geometry()
		end
    end)

    client.connect_signal("request::activate", function(c, context)
        if context == "conflux_tab_switch" then
            if active_outline_client and active_outline_client.valid and active_outline_client ~= c then
                -- The user is holding the mouse down on a tab button to switch and drag.
                -- If we let Conflux unmap the old tab while it's grabbed, the C-backend 
                -- silently kills the grab, bypassing our outline cleanup completely.
                
                -- Identify the current action (move vs resize)
                local action = "mouse_move"
                if stellar_api._grab and stellar_api._grab.action then
                    action = stellar_api._grab.action
                end

                -- Force-stop the grab on the old tab natively via Lua. 
                -- This guarantees the mousegrabber.stop() override and hide_outline() run.
                mousegrabber.stop()

                -- Restart the grab and outline on the NEW incoming tab.
                -- Because the physical mouse button is still held down, 
                -- AwesomeWM will seamlessly pick up the drag on the new window!
                begin_grab(c, action)
            end
        end
    end)

    client.connect_signal("property::screen", function(c)
        queue_update(c)
        local o = grab_outline[c]
        if o and o.top.visible then
            update_outline(c)
        end
    end)

    client.connect_signal("manage", function(c)
        evaluate_state(c)
        queue_update(c)
    end)
    
    client.connect_signal("unmanage", function(c)
		queue_update(c)
		floating_geometries[c] = nil
	end)

	client.connect_signal("property::stellar_active", function(c)
		stellar_api.log("property::stellar_active fired for " .. tostring(c) .. " active=" .. tostring(c.stellar_active))
		if c.valid then
			stellar_ui.update_decorations(c)
		end
	end)

	-- Layout changes affect every client on the tag (tiled_clients count flips can
	-- ripple across windows), so do a full sweep.
	tag.connect_signal("property::layout", queue_sweep)
    client.connect_signal("property::maximized", evaluate_state)
    client.connect_signal("property::fullscreen", evaluate_state)
    client.connect_signal("property::ontop", queue_update)
    client.connect_signal("property::sticky", queue_update)
    client.connect_signal("property::floating", function(c)
	    if not c.valid then return end
		evaluate_state(c)
		stellar_ui.update_decorations(c)

		local layout = awful.layout.get(c.screen)
		-- If the window is toggled to floating while in a floating layout
		if layout == awful.layout.suit.floating and c.floating then
			local geom = floating_geometries[c]
			if geom then
				-- Use delayed_call to apply our saved geometry *after* AwesomeWM applies the stale one
				gears.timer.delayed_call(function()
					if c.valid then
						c:geometry(geom)
					end
				end)
			end
		end
	end)
    
    client.connect_signal("tagged", function(c, t) evaluate_screen(t.screen) end)
    client.connect_signal("untagged", function(c, t) evaluate_screen(t.screen) end)

	client.connect_signal("mouse::enter", evaluate_state)
	client.connect_signal("mouse::leave", evaluate_state)

    awful.mouse.resize.add_leave_callback(function(c)
        hide_outline(c)
    end, "mouse.move")

    awful.mouse.resize.add_leave_callback(function(c)
        hide_outline(c)
        gears.timer.delayed_call(function()
            if c.valid then
				stellar_ui.update_decorations(c)
            end
        end)
    end, "mouse.resize")
    
    client.connect_signal("request::titlebars", function(c, context, hints)
		local no_titlebars = c:get_xproperty("_STELLAR_NO_TITLEBARS")
		local fullscreen_desktop = c:get_xproperty("_STELLAR_FULLSCREEN_DESKTOP")
	    if no_titlebars or fullscreen_desktop then
			c.titlebars_enabled = false
			return
		end
		
	    if c.stellar_titlebars_built then return end
	    c.stellar_titlebars_built = true

		local popup_margin = 6
		local popup_spacing = 2
		local popup_border_width = 2
		local popup_border_color = "#3300cc"

		local titlebar_buttons = {
            awful.button({}, 1, function() begin_grab(c, "mouse_move") end),
            awful.button({}, 2, function() c.minimized = true end),
            awful.button({}, 3, function() begin_grab(c, "mouse_resize") end),
        }

		local frame_buttons = {
            awful.button({}, 1, function() begin_grab(c, "mouse_move") end),
            awful.button({}, 3, function() begin_grab(c, "mouse_resize") end),
        }

		-- Forwards to the shared UI factory, injecting the client + slice_data
		-- that the call sites below rely on from this scope. These titlebar menu
		-- buttons signal state through icon PNGs (image_active/image_inactive),
		-- not a hover background, so hover_highlight is disabled to match the
		-- original flat appearance.
		local function popup_icon_button(opts)
			opts.client          = c
			opts.slice_data      = slice_data
			opts.hover_highlight = false
			return stellar_ui.popup_icon_button(opts)
		end

		-- Forward-declare the popup so callbacks can reference it to close it
		local popup_buttons 

		-- === Confirmation Configuration & Logic ===
        -- Options: "always", "never", "smart" (only if active processes)
        local terminal_confirm_mode = "smart" 
        local conflux_workspace = c:get_xproperty("_STELLAR_CONFLUX_WORKSPACE")

        local function execute_kill()
            local conflux_workspace = c:get_xproperty("_STELLAR_CONFLUX_WORKSPACE")
			if conflux_workspace and conflux_workspace ~= "" then
                conflux.kill_workspace(conflux_workspace, true)
            else
                c:kill()
            end
            if popup_buttons then popup_buttons.visible = false end
        end

        local function create_text_btn(text, bg_color, cb)
            local btn = wibox.widget {
                {
                    { text = text, align = "center", widget = wibox.widget.textbox },
                    margins = 4, widget = wibox.container.margin
                },
                bg = bg_color, shape = gears.shape.rounded_rect, widget = wibox.container.background
            }
            btn:buttons({ awful.button({}, 1, cb) })
            return btn
        end

        -- The secondary layout for the Yes/No prompt
		local confirm_layout = wibox.widget {
			{
				markup = "<span foreground='#ffffff'>Active sessions running.\nKill all processes?</span>",
				align = "center",
				widget = wibox.widget.textbox
			},
			{
				create_text_btn("Yes", "#cc0000", execute_kill),
				create_text_btn("No", "#555555", function() 
					if popup_buttons then popup_buttons.visible = false end
				end),
				spacing = 12, -- Spacing between the Yes/No buttons
				layout = wibox.layout.flex.horizontal
			},
			spacing = 24, -- Increased padding between the text and the buttons
			layout = wibox.layout.fixed.vertical
		}

		-- Wrap it to center horizontally and vertically within whatever space it's given
		local confirm_wrapper = wibox.widget {
			confirm_layout,
			halign = "center",
			valign = "center",
			widget = wibox.container.place
		}
		
		-- Define the existing right-hand column (Window Actions)
        local window_actions_col = wibox.widget {
            layout  = wibox.layout.fixed.vertical,
            spacing = popup_spacing,
            popup_icon_button {
                image_inactive = stellar_ui.get_image_path("win", "icon_terminate", nil, nil),
                text_label     = "Terminate",
                slice_name     = "icon_terminate",
                callback       = function()
                    local conflux_workspace = c:get_xproperty("_STELLAR_CONFLUX_WORKSPACE")
					local is_conflux = (conflux_workspace and conflux_workspace ~= "")
                    
                    -- If it's not a conflux window, or we never confirm, kill instantly
                    if not is_conflux or terminal_confirm_mode == "never" then
                        execute_kill()
                        return
                    end

                    local function show_prompt()
						-- Capture current auto-sized dimensions of the main menu
						local geo = popup_buttons:geometry()
						
						-- Lock the popup so it cannot shrink when the content changes
						popup_buttons.minimum_width = geo.width
						popup_buttons.minimum_height = geo.height
						
						-- Swap to the centered confirmation wrapper
						popup_buttons.widget.widget = confirm_wrapper
                    end

                    if terminal_confirm_mode == "always" then
                        show_prompt()
                    elseif terminal_confirm_mode == "smart" then
                        -- Check for active processes asynchronously
						conflux.has_active_processes(conflux_workspace, function(has_active)
                            -- Ensure the popup wasn't closed by the user while we were checking
                            if not popup_buttons.visible then return end

                            if has_active then
                                show_prompt()
                            else
                                execute_kill()
                            end
                        end)
                    end
                end,
            },
			popup_icon_button {
				image_inactive = stellar_ui.get_image_path("win", "icon_floating", "inactive"),
				image_active   = stellar_ui.get_image_path("win", "icon_floating", "active"),
				property       = "floating",
				callback       = function() c.floating = not c.floating end,
				text_label     = "Floating",
				slice_name     = "icon_floating",
			},
			popup_icon_button {
				image_inactive = stellar_ui.get_image_path("win", "icon_maximized", "inactive"),
				image_active   = stellar_ui.get_image_path("win", "icon_maximized", "active"),
				property       = "maximized",
				callback       = function() c.maximized = not c.maximized; c:raise() end,
				text_label     = "Maximized",
				slice_name     = "icon_maximized",
			},
			popup_icon_button {
				image_inactive = stellar_ui.get_image_path("win", "icon_ontop", "inactive"),
				image_active   = stellar_ui.get_image_path("win", "icon_ontop", "active"),
				property       = "ontop",
				callback       = function() c.ontop = not c.ontop end,
				text_label     = "Always On Top",
				slice_name     = "icon_ontop",
			},
			popup_icon_button {
				image_inactive = stellar_ui.get_image_path("win", "icon_sticky", "inactive"),
				image_active   = stellar_ui.get_image_path("win", "icon_sticky", "active"),
				property       = "sticky",
				callback       = function() c.sticky = not c.sticky end,
				text_label     = "Sticky",
				slice_name     = "icon_sticky",
			},
		}

		-- Define the main outer horizontal layout
		local main_popup_layout = wibox.widget {
			layout  = wibox.layout.fixed.horizontal,
			spacing = popup_spacing,
		}

		-- Dynamically build the left-hand column if multiple screens exist
		if stellar_api._num_screens > 1 then
			local screen_actions_col = wibox.widget {
				layout  = wibox.layout.fixed.vertical,
				spacing = popup_spacing,
			}

			-- Iterate through all available screens
			for s = 0, stellar_api._num_screens - 1, 1 do
				if s ~= stellar_api._this_screen then
					screen_actions_col:add(popup_icon_button {
						image_inactive = stellar_ui.get_image_path("win", "icon_sticky", nil, nil),
						callback       = function() 
							if popup_buttons then popup_buttons.visible = false end 
    						if c:get_xproperty("_STELLAR_CONFLUX_WORKSPACE") then
								conflux.relocate_workspace(c, s)
							end
						end,
						text_label     = "Move to Display [" .. tostring(s) .. "]",
						slice_name     = "icon_sticky",
					})
				end
			end

			-- Add the screen column to the LEFT side of the horizontal layout
			main_popup_layout:add(screen_actions_col)
		end

		-- Add the standard window actions to the right (or only) side
		main_popup_layout:add(window_actions_col)

		-- Initialize the awful.popup with the dynamically constructed widget
		popup_buttons = awful.popup {
			type         = "combo",
			ontop        = true,
			visible      = false,
			bg           = beautiful.bg_normal,
			border_width = popup_border_width,
			border_color = popup_border_color, 
			shape        = gears.shape.rect,
			widget       = {
				main_popup_layout,
				margins = popup_margin,
				widget  = wibox.container.margin,
			}
		}

        -- === Reset the layout when the popup hides ===
        popup_buttons:connect_signal("property::visible", function()
			if not popup_buttons.visible then
				-- Revert back to the main menu for the next time it opens
				popup_buttons.widget.widget = main_popup_layout
				
				-- Release the locked dimensions so it can auto-size normally again
				popup_buttons.minimum_width = nil
				popup_buttons.minimum_height = nil
			end
        end)

		local function place_popup()
            -- Anchors the top right of the popup to the top right of the client window,
            -- offset to the left by the exact width of the upper_right slice.
            awful.placement.top_right(popup_buttons, {
                parent = c,
                margins = {
					top = -(popup_border_width + popup_margin + 2),
					right = slice_data["win"]["upper_right"].w
							+ math.floor(slice_data["win"]["button"].w / 2)
							- math.floor(slice_data["win"]["icon_terminate"].w / 2)
							- (popup_border_width + popup_margin)
				}
            })
        end

        popup_buttons:connect_signal("property::width", function()
            if popup_buttons.visible then
                place_popup()
            end
        end)

		local hide_timer = gears.timer {
            timeout = 0.3,
            single_shot = true,
            callback = function() popup_buttons.visible = false end,
        }

        popup_buttons:connect_signal("mouse::enter", function() hide_timer:stop() end)
        popup_buttons:connect_signal("mouse::leave", function() hide_timer:again() end)

        local hover_area = wibox.widget {
            stellar_ui.create_slice(c, "win", "button", false, slice_data),
            widget = wibox.container.background,
        }

        hover_area:connect_signal("mouse::enter", function()
            hide_timer:stop()
            place_popup()
            popup_buttons.visible = true
        end)
        hover_area:connect_signal("mouse::leave", function() hide_timer:again() end)

        local is_tabbed = is_tabbed_client(c)

			stellar_api.log(
				"TITLEBAR for class="
					.. tostring(c.class)
					.. " instance="
					.. tostring(c.instance)
					.. " name="
					.. tostring(c.name)
					.. " source="
					.. tostring(c.stellar_title_source)
			)

			-- Always create a mutable textbox so we can update it reactively.
			-- stellar_title_source and c.instance may not be available yet
			-- during restart (they arrive after request::titlebars fires).
			local title_widget = wibox.widget {
				text   = "",
				valign = "center",
				halign = "left",
				widget = wibox.widget.textbox
			}

			local function refresh_title()
				if is_tabbed or c.stellar_title_source then
					title_widget.text = get_program_name(c)
				else
					title_widget.text = c.name or ""
				end
			end

			refresh_title()

			-- React to the property being set after titlebar creation
			c:connect_signal("property::stellar_title_source", refresh_title)
			-- Also update if instance/class populate late during restart
			c:connect_signal("property::instance", refresh_title)
			-- For the default (no title_source) case, track name changes
			c:connect_signal("property::name", refresh_title)

        local top_text_stack = wibox.widget {
				stellar_ui.create_slice(c, "win", "title_flex", true, slice_data),
				{
					{ align  = "left", widget = title_widget },
					halign = "left", widget = wibox.container.place,
				},
				layout = wibox.layout.stack
			}

        awful.titlebar(c, { position = "top", size = slice_data["win"]["upper_left"].h }) : setup {
            { stellar_ui.create_slice(c, "win", "upper_left", false, slice_data), top_text_stack, stellar_ui.create_slice(c, "win", "title_trans", false, slice_data), layout = wibox.layout.fixed.horizontal },
            stellar_ui.create_slice(c, "win", "bar_flex", true, slice_data),
            { hover_area, stellar_ui.create_slice(c, "win", "upper_right", false, slice_data), layout = wibox.layout.fixed.horizontal },
            layout  = wibox.layout.align.horizontal,
            buttons = titlebar_buttons
        }

		local lower_category

        -- 1. Bottom Titlebar
        local bottom_size = is_tabbed and slice_data["tab"]["lower_left"].h or slice_data["win"]["lower_left"].h
        local bottom_titlebar = awful.titlebar(c, { position = "bottom", size = bottom_size })

        if is_tabbed then
		    -- Set up a temporary placeholder so the titlebar isn't nil
			bottom_titlebar : setup {
				wibox.widget.imagebox(stellar_ui.get_image_path("tab", "lower_left", "normal", "focused")),
				wibox.widget.imagebox(stellar_ui.get_image_path("tab", "lower_flex", nil, "focused")),
				wibox.widget.imagebox(stellar_ui.get_image_path("tab", "lower_right", nil, "focused")),
				layout  = wibox.layout.align.horizontal,
				buttons = frame_buttons,
			}
			-- Then kick off the async update which will re-setup the titlebar
			update_tab_bar(c, slice_data, frame_buttons)
			lower_category = "tab"
        else
            -- Standard window lower bar
            bottom_titlebar : setup {
                stellar_ui.create_slice(c, "win", "lower_left", false, slice_data),
                stellar_ui.create_slice(c, "win", "lower_flex", true, slice_data),
                stellar_ui.create_slice(c, "win", "lower_right", false, slice_data),
                layout  = wibox.layout.align.horizontal,
                buttons = frame_buttons,
            }
			lower_category = "win"
        end

        awful.titlebar(c, { position = "left", size = slice_data["win"]["left_upper"].w }) : setup {
            stellar_ui.create_slice(c, "win", "left_upper", false, slice_data),
            stellar_ui.create_slice(c, "win", "left_flex", true, slice_data, nil, "vertical"),
            stellar_ui.create_slice(c, lower_category, "left_lower", false, slice_data),
            layout  = wibox.layout.align.vertical,
            buttons = frame_buttons,
        }

        awful.titlebar(c, { position = "right", size = slice_data["win"]["right_upper"].w }) : setup {
            stellar_ui.create_slice(c, "win", "right_upper", false, slice_data),
            stellar_ui.create_slice(c, "win", "right_flex", true, slice_data, nil, "vertical"),
            stellar_ui.create_slice(c, lower_category, "right_lower", false, slice_data),
            layout  = wibox.layout.align.vertical,
            buttons = frame_buttons,
        }

		local function cursor_inside_popup()
			local coords = mouse.coords()
			local geo = popup_buttons:geometry()
			return coords.x >= geo.x
			   and coords.x <= geo.x + geo.width
			   and coords.y >= geo.y
			   and coords.y <= geo.y + geo.height
		end
		
		c:connect_signal("property::_STELLAR_CONFLUX_WORKSPACE", function()
    		update_tab_bar(c, slice_data, frame_buttons)
		end)

		c:connect_signal("property::geometry", function()
			if popup_buttons.visible then
				place_popup()
				-- Give placement a tick to settle, then check
				gears.timer.delayed_call(function()
					if popup_buttons.visible and not cursor_inside_popup() then
						popup_buttons.visible = false
					end
				end)
			end
		end)

        c:connect_signal("unfocus", function()
			if client.focus ~= c then
            	popup_buttons.visible = false
			end
        end)

		c._stellar_popup_buttons = popup_buttons

		c:connect_signal("unmanage", function()
			if c._stellar_popup_buttons then
				c._stellar_popup_buttons.visible = false
				c._stellar_popup_buttons = nil
			end
		end)
    end)
end

stellar_api.log("STELLAR WINDOW MODULE INITIALIZED")

return M
