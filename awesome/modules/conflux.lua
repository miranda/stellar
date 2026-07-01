local awful = require("awful")
local wibox = require("wibox")
local gears = require("gears")
local ruled = require("ruled")
local json = require("dkjson")

local conflux = {}

-- Internal state
conflux.counter = 0
conflux.workspaces = {}
conflux.buttons = {}
conflux.pending_spawns = {}
conflux.pending_relocations = {}
conflux.tab_orders = {}
conflux.pending_restores = {}

-- The main container widget
conflux.widget = wibox.widget {
    layout  = wibox.layout.fixed.horizontal,
    spacing = 4,
}

local function get_term_cmd_prefix(instance_name)
	local term_app = stellar_api.stellar_settings.terminal_gui
	if term_app == "xterm" then
		return {
			"xterm",
			"-name", instance_name,
			"-e"
		}
	else
		return {
			"wezterm",
			"start",
			"--always-new-process",
			"--class", instance_name,
			"--"
		}
	end
end

local function get_user_shell()
    return os.getenv("STELLAR_SHELL")
        or os.getenv("SHELL")
        or "/bin/bash"
end

local function spawn_terminal_with_session(instance_name, session_name)
    awful.spawn(
		gears.table.join(
			get_term_cmd_prefix(instance_name),
			{ "atch", "-q", session_name, get_user_shell() }
		),
		false
	)
end

-- Helper function to create uniform buttons
local function create_button(text, bg_color, callback)
    local btn = wibox.widget {
        {
            {
                text   = text,
                valign = "center",
                align  = "center",
                widget = wibox.widget.textbox
            },
            margins = 4,
            widget  = wibox.container.margin
        },
        bg     = bg_color,
        fg     = "#FFFFFF",
        shape  = gears.shape.rounded_rect,
        widget = wibox.container.background
    }

    btn:connect_signal("button::press", function(_, _, _, button)
        if button == 1 then
            callback()
        end
    end)

    btn:connect_signal("mouse::enter", function(c)
        c:set_bg("#555555")
    end)

    btn:connect_signal("mouse::leave", function(c)
        c:set_bg(bg_color)
    end)

    return btn
end

-- @param workspace_name string The ID of the workspace group
-- @param callback function A function to run with the boolean result
function conflux.has_active_processes(workspace_name, callback)
    if not workspace_name or workspace_name == "" then
        callback(false)
        return
    end

    -- Collect ALL instance names belonging to this workspace,
    -- then derive the atch session name from each one.
    local session_names = {}
    for instance_name, ws_name in pairs(conflux.workspaces) do
        if ws_name == workspace_name then
            local uid = instance_name:match("^stellar_conflux_(.+)$")
            if uid then
                table.insert(session_names, "stellar_conflux_session_" .. uid)
            end
        end
    end

    if #session_names == 0 then
        callback(false)
        return
    end

    local shell_basename = get_user_shell():match("([^/]+)$") or "sh"

    local checks = {}
    for _, sname in ipairs(session_names) do
        local safe = sname:gsub("'", "'\\''")
		table.insert(checks, string.format([[
			ATCH_PID=""
			for PID in $(pgrep -x atch 2>/dev/null); do
				if grep -q '%s' /proc/$PID/cmdline 2>/dev/null; then
					ATCH_PID=$PID
					break
				fi
			done
			if [ -z "$ATCH_PID" ]; then
				echo 1; exit 0
			fi
			CURSOR=$ATCH_PID
			while true; do
				CHILD=$(pgrep -P "$CURSOR" 2>/dev/null | head -1)
				[ -z "$CHILD" ] && break
				CURSOR=$CHILD
				COMM=$(cat /proc/$CHILD/comm 2>/dev/null)
				[ "$COMM" != "atch" ] && break
			done
			COMM=$(cat /proc/$CURSOR/comm 2>/dev/null)
			if [ "$COMM" != '%s' ]; then
				echo 1; exit 0
			fi
			if pgrep -P "$CURSOR" >/dev/null 2>&1; then
				echo 1; exit 0
			fi
		]], safe, shell_basename))
    end

    local script = table.concat(checks, "\n") .. "\necho 0\n"

    awful.spawn.easy_async_with_shell(script, function(stdout)
        local result = tonumber((stdout or ""):match("%d+")) or 1
        callback(result > 0)
    end)
end

-- Helper function to find the next available sequential term_XX name
local function get_next_workspace_name(atch_output)
    local used_numbers = {}

    -- 1. Check internal table values (which store the workspace groups like "term_01")
    for _, workspace_name in pairs(conflux.workspaces) do
        local num_str = workspace_name:match("term_(%d+)")
        if num_str then
            used_numbers[tonumber(num_str)] = true
        end
    end

    -- 2. Check the atch list output for the same pattern
    if atch_output then
        for num_str in atch_output:gmatch("term_(%d+)") do
            local num = tonumber(num_str)
            if num then
                used_numbers[num] = true
            end
        end
    end

    -- 3. Find the lowest available hole
    local next_num = 1
    while used_numbers[next_num] do
        next_num = next_num + 1
    end

    return string.format("term_%02d", next_num)
end

function conflux.find_client_for_workspace(name)
    for _, c in ipairs(client.get()) do
        if c.instance == name then
            return c
        end
    end

    return nil
end

-- Returns the ordered list of instance names for a workspace.
-- On first call for a given workspace (or after the cached order is cleared),
-- builds the list from conflux.workspaces and sorts alphabetically as default.
function conflux.get_tab_order(workspace_name)
    if not conflux.tab_orders[workspace_name] then
        local order = {}
        for instance_name, ws in pairs(conflux.workspaces) do
            if ws == workspace_name then
                table.insert(order, instance_name)
            end
        end
        table.sort(order)
        conflux.tab_orders[workspace_name] = order
    end
    return conflux.tab_orders[workspace_name]
end

-- Toggles all four titlebar sides on a tabbed client. We use this to keep hidden
-- tabs from contributing wibox surfaces to compositor damage cycles. awful.titlebar.hide
-- sets the side's size to 0 (and remembers the prior size) so awful.titlebar.show
-- restores it without us tracking original dimensions.
local titlebar_positions = { "top", "bottom", "left", "right" }
function conflux.set_decorations_visible(c, visible)
    if not c or not c.valid then return end
    for _, pos in ipairs(titlebar_positions) do
        if visible then
            awful.titlebar.show(c, pos)
        else
            awful.titlebar.hide(c, pos)
        end
    end
end

function conflux.toggle_workspace(workspace_name)
    local active_c = nil
    local fallback_c = nil

    -- Search the group for the window that is currently "in front" (not hidden)
    for instance_name, ws_name in pairs(conflux.workspaces) do
        if ws_name == workspace_name then
            local c = conflux.find_client_for_workspace(instance_name)
            if c then
                fallback_c = c -- Keep any valid window as a fallback
                if not c.hidden then
                    active_c = c
                    break
                end
            end
        end
    end

    local target_c = active_c or fallback_c
    if not target_c then return end

    -- Standard minimize/restore logic on the physical X11 client
    if target_c == client.focus and not target_c.minimized then
        -- It's focused and visible, so minimize it
        target_c.minimized = true
    else
        -- It's either minimized or hiding behind another app, so raise it
        target_c.minimized = false
        target_c:emit_signal("request::activate", "conflux_toggle", { raise = true })
    end
end

function conflux.add_workspace(workspace_name, instance_name)
    -- Always register the new instance to the workspace group
    conflux.workspaces[instance_name] = workspace_name

    -- Maintain explicit tab order if one exists for this group.
    -- If none exists yet, get_tab_order will build it on demand.
    local order = conflux.tab_orders[workspace_name]
    if order then
        local found = false
        for _, name in ipairs(order) do
            if name == instance_name then found = true; break end
        end
        if not found then
            table.insert(order, instance_name)
        end
    end

    -- If the group already has a UI button, abort drawing a new one!
    if conflux.buttons[workspace_name] then
        return
    end

    local ws_btn = create_button(" " .. workspace_name .. " ", "#444b59", function()
        conflux.toggle_workspace(workspace_name)
    end)

    -- Track the button by the workspace group name, not the instance
    conflux.buttons[workspace_name] = ws_btn
    conflux.widget:add(ws_btn)
end

function conflux.remove_workspace(instance_name)
    local workspace_name = conflux.workspaces[instance_name]
    if not workspace_name then return end

    -- 1. Unregister this specific instance
    conflux.workspaces[instance_name] = nil

    -- 1b. Remove from explicit tab order
    local order = conflux.tab_orders[workspace_name]
    if order then
        for i, name in ipairs(order) do
            if name == instance_name then
                table.remove(order, i)
                break
            end
        end
        if #order == 0 then
            conflux.tab_orders[workspace_name] = nil
        end
    end

    -- 2. Check if any other tabs are still alive in this group
    local group_still_active = false
    for _, ws_name in pairs(conflux.workspaces) do
        if ws_name == workspace_name then
            group_still_active = true
            break
        end
    end

    -- 3. Only delete the UI button if the entire group is empty
    if not group_still_active then
        local btn = conflux.buttons[workspace_name]
        if btn then
            conflux.widget:remove_widgets(btn)
            conflux.buttons[workspace_name] = nil
        end
    end
end

-- Kills all clients in a given workspace and cleans up the UI.
-- @param workspace_name string The ID of the workspace group
-- @param nuke_sessions boolean If true, also terminates the underlying atch sessions
-- @return table The instance names that were terminated
function conflux.kill_workspace(workspace_name, active_tab_client, nuke_sessions)
    if not workspace_name and workspace_name ~= "" then return {} end

    local instancees = {}
    for instance_name, ws_name in pairs(conflux.workspaces) do
        if ws_name == workspace_name then
            table.insert(instancees, instance_name)
        end
    end

    if #instancees == 0 then return {} end

    -- Clear the cached tab order so it doesn't resurrect as zombie
    -- entries if this workspace name gets reused on the next spawn.
    conflux.tab_orders[workspace_name] = nil
    conflux.pending_restores[workspace_name] = nil

    local clients_to_kill = {}
    for _, instance_name in ipairs(instancees) do
        local c = conflux.find_client_for_workspace(instance_name)

        -- Save the geometry for the client that was clicked on BEFORE we
        -- unregister it below. get_geometry_state_file() resolves the target
        -- filename from conflux.workspaces[c.instance]; if we niled that first,
        -- the save would fall through to a per-instance junk filename (or fail
        -- to resolve), which is why conflux geometry stopped persisting to the
        -- shared stellar_conflux_<workspace> file.
        if c and c.valid and c == active_tab_client then
            stellar_api.save_client_geometry(c, true)
        end

        -- Unregister from the internal table
        conflux.workspaces[instance_name] = nil

        -- NEW: Kill the underlying atch session if the flag is true
        if nuke_sessions then
            local uid = instance_name:match("^stellar_conflux_(.+)$")
            if uid then
                local session_name = "stellar_conflux_session_" .. uid
                awful.spawn({ "atch", "kill", session_name }, false)
                gears.timer.start_new(1.0, function()
                    awful.spawn({ "atch", "rm", session_name }, false)
                    return false
                end)
            end
        end

        if c and c.valid then
            -- Normalize mapped state
            conflux.set_decorations_visible(c, true)
            c.hidden = false
            c.minimized = false

            -- Pull from focus history
            if awful.client.focus.history and awful.client.focus.history.delete then
                awful.client.focus.history.delete(c)
            end
            if client.focus == c then
                client.focus = nil
            end

            table.insert(clients_to_kill, c)
        end
    end

    -- Synchronous Hard Kill for the X11 clients
    for _, c in ipairs(clients_to_kill) do
        if c.valid then
            if c.pid then
                os.execute("kill -9 " .. tostring(c.pid))
            else
                os.execute("xkill -id " .. tostring(c.window))
            end
        end
    end

    -- Clean up the UI button
    local btn = conflux.buttons[workspace_name]
    if btn then
        conflux.widget:remove_widgets(btn)
        conflux.buttons[workspace_name] = nil
    end

    return instancees
end

-- Initiates the transfer from the source screen
function conflux.relocate_workspace(target_client, target_screen)
    local workspace_name = target_client:get_xproperty("_STELLAR_CONFLUX_WORKSPACE")
    stellar_api.log("conflux: relocate function called, workspace=" .. workspace_name)
    if not workspace_name and workspace_name ~= "" then return end

    -- Capture tab order and active tab BEFORE kill_workspace clears the state.
    local tab_order = conflux.get_tab_order(workspace_name)
    -- Shallow copy - kill_workspace will nil the original
    local saved_order = {}
    for _, name in ipairs(tab_order) do table.insert(saved_order, name) end

    local active_tab = nil
    for _, inst in ipairs(saved_order) do
        local tc = conflux.find_client_for_workspace(inst)
        if tc and tc.valid and not tc.hidden then
            active_tab = inst
            break
        end
    end

    -- Delegate the teardown to kill_workspace, which returns the affected instancees
    local instancees = conflux.kill_workspace(workspace_name)
    
    -- If nothing was killed (e.g. invalid workspace), abort the relocation
    if not instancees or #instancees == 0 then return end

    local tx_id = tostring(os.time()) .. "_" .. workspace_name

    local payload = {
        transaction_id = tx_id,
        source_screen = stellar_api._this_screen,
        target_screen = target_screen,
        workspace = workspace_name,
        instancees = instancees,
        tab_order = saved_order,
        active_tab = active_tab
    }

    -- Step 3: Store rollback data and start the failsafe timer
	conflux.pending_relocations[tx_id] = {
        payload = payload,
        timer = gears.timer {
            timeout = 3.0,
            autostart = true,
            single_shot = true,
            callback = function()
                stellar_api.log("Relocation timeout for " .. tx_id .. ". Rolling back to source screen!")
                conflux.restore_workspace(payload)
                conflux.pending_relocations[tx_id] = nil
            end
        }
    }

    -- Step 4: Fire the data to the DE
    if stellar_api and stellar_api.send_ipc then
        stellar_api.send_ipc("RELOCATE_CONFLUX " .. json.encode(payload))
    end
end

-- Reconstructs the workspace on the target screen
function conflux.restore_workspace(data)
    local target_workspace = data.workspace
    
    -- Check for namespace collisions on the new screen.
    -- If a workspace with the same name already exists, assign a new hole.
    local collision = false
    for _, ws_name in pairs(conflux.workspaces) do
        if ws_name == target_workspace then 
            collision = true 
            break 
        end
    end
    
    if collision then
        target_workspace = get_next_workspace_name(nil)
    end

    -- Pre-seed the tab order so the manage handler preserves it.
    -- Falls back to instancees (alphabetical) if the payload predates this field.
    local order = data.tab_order or data.instancees
    conflux.tab_orders[target_workspace] = {}
    for _, name in ipairs(order) do
        table.insert(conflux.tab_orders[target_workspace], name)
    end

    -- Track how many tabs we expect so the manage handler can activate
    -- the correct tab after the last one arrives, regardless of order.
    if data.active_tab then
        conflux.pending_restores[target_workspace] = {
            expected   = #order,
            arrived    = 0,
            active_tab = data.active_tab,
        }
    end

    -- Spawn the X11 clients. The existing 'manage' signal will pick these
    -- up via pending_spawns. Arrival order is non-deterministic, so we
    -- don't try to control which tab lands last - pending_restores handles it.
    for _, instance_name in ipairs(order) do
        local uid = instance_name:match("^stellar_conflux_(.+)$")
        if uid then
            local session_name = "stellar_conflux_session_" .. uid
            
            conflux.pending_spawns[instance_name] = target_workspace

			spawn_terminal_with_session(instance_name, session_name)
        end
    end

    -- If this was a remote relocation, send a confirmation ACK back to the source
	if data.transaction_id and data.source_screen and stellar_api and stellar_api.send_ipc then
        local ack_payload = {
            transaction_id = data.transaction_id,
            target_screen = data.source_screen -- We route the ACK *back* to the source
        }
        stellar_api.send_ipc("RELOCATE_ACK " .. json.encode(ack_payload))
    end
end

function conflux.activate_tab(target_instance)
    local target_client = conflux.find_client_for_workspace(target_instance)
    if not target_client or target_client == client.focus then return end

    local group_id = target_client:get_xproperty("_STELLAR_CONFLUX_WORKSPACE")
    if not group_id then return end

    local old_c = nil

    for instance_name, ws_name in pairs(conflux.workspaces) do
        if ws_name == group_id and instance_name ~= target_instance then
            local c = conflux.find_client_for_workspace(instance_name)
            if c and not c.hidden then
                old_c = c
                break
            end
        end
    end

    -- Brand picom anti-fade BEFORE any visibility change
    target_client:set_xproperty("_STELLAR_NO_FADE", true)
    if old_c then
        old_c:set_xproperty("_STELLAR_NO_FADE", true)
    end

    -- IMPORTANT: Restore decorations on the incoming tab BEFORE copying geometry.
    -- awesome preserves the inner content area when titlebar sizes change, so if we
    -- copy geometry while the incoming tab has size-0 titlebars and *then* show them,
    -- awesome grows the frame to add titlebar space - causing the window to creep
    -- taller on every swap.
    conflux.set_decorations_visible(target_client, true)

    -- Now match geometry on the still-hidden incoming tab. Both clients have full
    -- titlebars at this point, so the frame sizes align cleanly.
    if old_c then
        target_client.floating = old_c.floating
        target_client:geometry(old_c:geometry())
    end

    -- Swap clients
	if target_client.valid then
		target_client.hidden = false
		if old_c then target_client:swap(old_c) end
		target_client:emit_signal("request::activate", "conflux_tab_switch", { raise = true })
		target_client:emit_signal("property::_STELLAR_CONFLUX_WORKSPACE") 
	end

	if old_c and old_c.valid then
		conflux.set_decorations_visible(old_c, false)
		old_c.hidden = true
	end
	
	-- Re-raise after old_c's hide triggers a layout recalculation,
	-- which can steal focus to another tiled client.
	if target_client.valid then
	   target_client:raise()
	   client.focus = target_client
	end

	-- Deferred so focus from request::activate has fully settled before
	-- we flip stellar_active - update_surface checks client.focus == c,
	-- which won't be true until the activation has propagated.
	gears.timer.delayed_call(function()
		if target_client.valid then
			stellar_api.set_active_client(target_client)
		end
	end)

	-- Clean up the picom brand after a safe margin
	gears.timer.start_new(0.10, function()
		if target_client.valid then target_client:set_xproperty("_STELLAR_NO_FADE", false) end
		if old_c and old_c.valid then old_c:set_xproperty("_STELLAR_NO_FADE", false) end
		return false
	end)
end

-- Swaps a tab one position left (direction = -1) or right (direction = 1)
-- within its workspace group and rebuilds all tab bars in the group.
function conflux.move_tab(instance_name, direction)
    local workspace_name = conflux.workspaces[instance_name]
    if not workspace_name then return end

    local order = conflux.get_tab_order(workspace_name)
    local idx
    for i, name in ipairs(order) do
        if name == instance_name then idx = i; break end
    end
    if not idx then return end

    local target = idx + direction
    if target < 1 or target > #order then return end

    order[idx], order[target] = order[target], order[idx]

    -- Rebuild tab bars for every client in the group
    for _, inst in ipairs(order) do
        local gc = conflux.find_client_for_workspace(inst)
        if gc and gc.valid then
            gc:emit_signal("property::_STELLAR_CONFLUX_WORKSPACE")
        end
    end
end

-- Pulls a single tab out of its workspace group into a new solo workspace.
-- If the tab is the only member of its group, this is a no-op.
function conflux.detach_tab(instance_name)
    local old_workspace = conflux.workspaces[instance_name]
    if not old_workspace then return end

    -- Don't detach the only tab
    local count = 0
    for _, ws in pairs(conflux.workspaces) do
        if ws == old_workspace then count = count + 1 end
    end
    if count <= 1 then return end

    local target_c = conflux.find_client_for_workspace(instance_name)
    if not target_c or not target_c.valid then return end

    local was_hidden = target_c.hidden

    -- Remove from old workspace's order
    local order = conflux.tab_orders[old_workspace]
    if order then
        for i, name in ipairs(order) do
            if name == instance_name then table.remove(order, i); break end
        end
        if #order == 0 then
            conflux.tab_orders[old_workspace] = nil
        end
    end

    -- Assign a new workspace name (synchronous, based on current state)
    local new_workspace = get_next_workspace_name(nil)

    -- Update mappings
    conflux.workspaces[instance_name] = new_workspace
    target_c:set_xproperty("_STELLAR_CONFLUX_WORKSPACE", new_workspace)
    conflux.tab_orders[new_workspace] = { instance_name }

    -- Create taskbar button for the new solo workspace
    if not conflux.buttons[new_workspace] then
        local ws_btn = create_button(" " .. new_workspace .. " ", "#444b59", function()
            conflux.toggle_workspace(new_workspace)
        end)
        conflux.buttons[new_workspace] = ws_btn
        conflux.widget:add(ws_btn)
    end

    if was_hidden then
        -- This was a background tab - unhide it in its own workspace
        target_c:set_xproperty("_STELLAR_NO_FADE", true)
        conflux.set_decorations_visible(target_c, true)
        target_c.hidden = false
        target_c.minimized = false
        target_c:emit_signal("request::activate", "conflux_detach", { raise = true })

        gears.timer.start_new(0.10, function()
            if target_c.valid then target_c:set_xproperty("_STELLAR_NO_FADE", false) end
            return false
        end)
    else
        -- This was the visible/active tab. Promote a hidden sibling in the old group.
        for inst, ws in pairs(conflux.workspaces) do
            if ws == old_workspace then
                local sibling = conflux.find_client_for_workspace(inst)
                if sibling and sibling.valid and sibling.hidden then
                    sibling:set_xproperty("_STELLAR_NO_FADE", true)
                    conflux.set_decorations_visible(sibling, true)
                    sibling.floating = target_c.floating
                    sibling:geometry(target_c:geometry())
                    sibling.hidden = false
                    sibling.minimized = false
                    sibling:emit_signal("request::activate", "conflux_detach_promote", { raise = true })

                    gears.timer.start_new(0.10, function()
                        if sibling.valid then sibling:set_xproperty("_STELLAR_NO_FADE", false) end
                        return false
                    end)
                    break
                end
            end
        end
    end

    -- Rebuild tab bars for both old and new workspaces
    for inst, ws in pairs(conflux.workspaces) do
        if ws == old_workspace or ws == new_workspace then
            local gc = conflux.find_client_for_workspace(inst)
            if gc and gc.valid then
                gc:emit_signal("property::_STELLAR_CONFLUX_WORKSPACE")
            end
        end
    end

    -- Attempt to load saved geometry for the new workspace.
    -- If none exists, fall back to natural placement.
    if target_c.valid then
        local loaded = false
        if stellar_api and stellar_api.load_client_geometry then
            loaded = stellar_api.load_client_geometry(target_c)
        end
        
        if not loaded then
            awful.placement.no_overlap(target_c)
            awful.placement.no_offscreen(target_c)
        end
    end
end

-- Kills a single tab's client. The existing unmanage handler promotes a sibling
-- and cleans up the workspace button if the group becomes empty.
function conflux.terminate_tab(instance_name)
    local c = conflux.find_client_for_workspace(instance_name)
    if not c or not c.valid then return end

    -- If this is the last tab in its workspace, terminating it closes the whole
    -- window, so persist its geometry (same as the whole-window close path).
    local workspace_name = conflux.workspaces[instance_name]
        or c:get_xproperty("_STELLAR_CONFLUX_WORKSPACE")
    if workspace_name and workspace_name ~= "" then
        local sibling_count = 0
        for inst, ws in pairs(conflux.workspaces) do
            if ws == workspace_name and inst ~= instance_name then
                sibling_count = sibling_count + 1
            end
        end
        if sibling_count == 0 then
            stellar_api.save_client_geometry(c, true)
        end
    end

	conflux.activate_tab(instance_name)
    c:kill()
end

conflux.sub_counter = 0

function conflux.spawn_new(target_workspace, source_client)
    awful.spawn.easy_async({ "atch", "list" }, function(stdout)
        local workspace_name = target_workspace or get_next_workspace_name(stdout)
    	stellar_api.log("conflux: spawn_new(), workspace=" .. workspace_name)
        local uid = string.format("%x%x%d", os.time(), conflux.sub_counter, tonumber(stellar_api._this_screen))
        conflux.sub_counter = conflux.sub_counter + 1
        
        local instance_name	= string.format("stellar_conflux_%s", uid)
        local session_name	= string.format("stellar_conflux_session_%s", uid)

        -- Register the intent for the manage signal
        conflux.pending_spawns[instance_name] = workspace_name

		spawn_terminal_with_session(instance_name, session_name)
    end)
end

local add_btn = create_button(" + ", "#282c34", function()
    conflux.spawn_new(nil)
end)

conflux.widget:add(add_btn)

-- Intercept window rules before mapping to guarantee Picom and Taskbar compliance
client.connect_signal("request::rules", function(c)
    if c.instance and c.instance:match("^stellar_conflux_") then
    	stellar_api.log("conflux: request::rules triggered, class=" .. c.class .. " instance=" .. c.instance)
        
        local target_workspace = conflux.pending_spawns[c.instance]
        local existing_workspace = c:get_xproperty("_STELLAR_CONFLUX_WORKSPACE")
        local ws = target_workspace or existing_workspace

        if ws then
            local has_siblings = false
            
            for instance_name, ws_name in pairs(conflux.workspaces) do
                if ws_name == ws and instance_name ~= c.instance then
                    has_siblings = true
                    
                    local other_c = conflux.find_client_for_workspace(instance_name)
                    if other_c and not other_c.hidden then
                        -- A visible tab already exists. Brand the new window before it maps!
                        c:set_xproperty("_STELLAR_NO_FADE", true)
                    end
                end
            end

            -- If the workspace already has other tabs, we will copy their geometry 
            -- dynamically in the `manage` signal. Tell stellar_bridge not to load from disk.
            if has_siblings then
                c._stellar_placement_handled = true
            end
        end
    end
end)

client.connect_signal("manage", function(c)
    local instance = c.instance

    if instance and instance:match("^stellar_conflux_") then
    	stellar_api.log("conflux: manage signal triggered, instance=" .. c.instance)
        -- 1. Standard tasklist skip: Conflux owns these windows now.
        c.skip_taskbar = true 

        local target_workspace = conflux.pending_spawns[instance]

        if target_workspace then
            c:set_xproperty("_STELLAR_CONFLUX_WORKSPACE", target_workspace)
            conflux.add_workspace(target_workspace, instance)
            conflux.pending_spawns[instance] = nil

            local old_c = nil

            -- Find the active tab in this group that we are spawning on top of
            for instance_name, ws_name in pairs(conflux.workspaces) do
                if ws_name == target_workspace and instance_name ~= instance then
                    local other_c = conflux.find_client_for_workspace(instance_name)
                    if other_c and not other_c.hidden then
                        old_c = other_c
                        break
                    end
                end
            end

			-- TODO: fix visual glitch from disabling fading, or remove this custom xproperty entirely
            if old_c then
                c:set_xproperty("_STELLAR_NO_FADE", true)
                old_c:set_xproperty("_STELLAR_NO_FADE", true)

                c.floating = old_c.floating
                c:geometry(old_c:geometry())

				c._stellar_placement_handled = true

                -- The new client `c` is already mapped at this point (awesome maps
                -- it before firing manage). So the only swap we need is hiding old_c.
                -- We delay it one tick so awesome can finish its current manage flow
                -- (fully drawing the new client + decorations) before we unmap old_c -
                -- minimizing the compositor window where both are visible.
				
				-- Inherit old_c's tiling slot so the new tab doesn't
				-- land in master and shift every other tiled window.
				c:swap(old_c)

				if old_c and old_c.valid then
					conflux.set_decorations_visible(old_c, false)
					old_c.hidden = true
				end

				gears.timer.start_new(0.10, function()
					if c.valid then c:set_xproperty("_STELLAR_NO_FADE", false) end
					if old_c and old_c.valid then old_c:set_xproperty("_STELLAR_NO_FADE", false) end
					return false
				end)
            end

            -- Force UI redraw
            for instance_name, ws_name in pairs(conflux.workspaces) do
                if ws_name == target_workspace then
                    local group_client = conflux.find_client_for_workspace(instance_name)
                    if group_client then
                        group_client:emit_signal("property::_STELLAR_CONFLUX_WORKSPACE") 
                    end
                end
            end

            -- If this tab is part of a relocation restore, track arrival.
            -- Once all expected tabs have checked in, activate the correct one
            -- regardless of what order they arrived in.
            local pr = conflux.pending_restores[target_workspace]
            if pr then
                pr.arrived = pr.arrived + 1
                if pr.arrived >= pr.expected then
                    local active_inst = pr.active_tab
                    conflux.pending_restores[target_workspace] = nil
                    gears.timer.delayed_call(function()
                        if active_inst then
                            conflux.activate_tab(active_inst)
                        end
                    end)
                end
            end
		else
		    -- ==============================================
			-- ORPHAN ADOPTION (AWESOMEWM RESTART)
			-- ==============================================
			local existing_workspace = c:get_xproperty("_STELLAR_CONFLUX_WORKSPACE")

			if existing_workspace and existing_workspace ~= "" then
				-- Reconstruct the internal Lua state first so the sibling search
				-- below can see prior orphans that already went through this path.
				conflux.add_workspace(existing_workspace, instance)

				-- Look for a sibling that has already been promoted to visible
				local visible_sibling = nil
				for instance_name, ws_name in pairs(conflux.workspaces) do
					if ws_name == existing_workspace and instance_name ~= instance then
						local other_c = conflux.find_client_for_workspace(instance_name)
						if other_c and other_c.valid and not other_c.hidden then
							visible_sibling = other_c
							break
						end
					end
				end

				if visible_sibling then
					-- Someone else already won the promotion race - stack under them
					c.floating = visible_sibling.floating
					c:geometry(visible_sibling:geometry())
					conflux.set_decorations_visible(c, false)
					c.hidden = true
				end
				-- else: first orphan in this group, leave it visible as the active tab

				-- Broadcast redraw to every member of the group so tab bars rebuild
				-- with the full roster, not just whatever was registered when each
				-- individual manage fired.
				for instance_name, ws_name in pairs(conflux.workspaces) do
					if ws_name == existing_workspace then
						local group_client = conflux.find_client_for_workspace(instance_name)
						if group_client then
							group_client:emit_signal("property::_STELLAR_CONFLUX_WORKSPACE")
						end
					end
				end
			end
        end
    end
end)

-- Auto-remove workspace buttons when session is gone
client.connect_signal("unmanage", function(c)
    local instance = c.instance

    if not instance or not conflux.workspaces[instance] then
        return
    end

    stellar_api.log("conflux: unmanage fired, instance=" .. instance)

    local workspace_name = conflux.workspaces[instance]

    -- Unregister this instance immediately so sibling lookups skip it
    conflux.workspaces[instance] = nil

    -- Remove from explicit tab order
    local order = conflux.tab_orders[workspace_name]
    if order then
        for i, name in ipairs(order) do
            if name == instance then table.remove(order, i); break end
        end
        if #order == 0 then
            conflux.tab_orders[workspace_name] = nil
        end
    end

    -- Clean up the button immediately if this was the last tab in the group.
    -- The delayed atch session check below can't be relied on for this because
    -- the session may still be alive when the timer fires.
    local group_still_active = false
    for _, ws_name in pairs(conflux.workspaces) do
        if ws_name == workspace_name then
            group_still_active = true
            break
        end
    end
    if not group_still_active then
        local btn = conflux.buttons[workspace_name]
        if btn then
            conflux.widget:remove_widgets(btn)
            conflux.buttons[workspace_name] = nil
        end
    end

    -- If the closing tab was the visible one, revive a sibling
    if not c.hidden then
        local sibling = nil
        for other_instance, ws_name in pairs(conflux.workspaces) do
            if ws_name == workspace_name then
                local oc = conflux.find_client_for_workspace(other_instance)
                if oc and oc.valid then
                    sibling = oc
                    break
                end
            end
        end

        if sibling then
            -- Match geometry from the dying client before it's gone
            local geom = c:geometry()
            local floating = c.floating

            sibling:set_xproperty("_STELLAR_NO_FADE", true)
            conflux.set_decorations_visible(sibling, true)
            sibling.floating = floating
            sibling:geometry(geom)

			sibling.hidden = false
            sibling.minimized = false
            sibling:emit_signal("request::activate", "conflux_tab_revive", { raise = true })
            sibling:emit_signal("property::_STELLAR_CONFLUX_WORKSPACE")

            gears.timer.start_new(0.10, function()
                if sibling.valid then
                    sibling:set_xproperty("_STELLAR_NO_FADE", false)
                end
                return false
            end)
        end
    end

    -- Extract the unique ID from the instance name
    local uid = instance:match("^stellar_conflux_(.+)$")
    if not uid then return end

    local session_name = "stellar_conflux_session_" .. uid

    gears.timer.start_new(0.5, function()
        awful.spawn.easy_async({ "atch", "list" }, function(stdout)
            if not stdout or not stdout:find(session_name, 1, true) then
                stellar_api.log("conflux: session " .. session_name .. " is gone")
                -- Session left atch list but may still be settling into atch's
                -- history buffer - delay rm so the entry exists when we clear it.
                gears.timer.start_new(1.0, function()
                    awful.spawn({ "atch", "rm", session_name }, false)
                    return false
                end)
            else
                -- Client is unmanaged but the atch session survived (WezTerm
                -- disconnected without killing the shell).  Clean it up
                -- gracefully so it doesn't linger as an orphan - and don't
                -- re-register the dead instance, which would block button
                -- cleanup for the workspace group.
                stellar_api.log("conflux: session " .. session_name .. " orphaned after client exit, killing")
                awful.spawn({ "atch", "kill", session_name }, false)
                -- atch leaves killed sessions in a history buffer; rm clears
                -- them but no-ops on still-alive sessions, so we wait briefly
                -- for the kill to settle before cleaning up.
                gears.timer.start_new(1.0, function()
                    awful.spawn({ "atch", "rm", session_name }, false)
                    return false
                end)
            end

            -- Final button sweep: catches cases where the immediate cleanup
            -- in unmanage was blocked by zombie re-registrations from earlier
            -- tab kills whose timers were still in flight.
            local group_still_active = false
            for _, ws_name in pairs(conflux.workspaces) do
                if ws_name == workspace_name then
                    group_still_active = true
                    break
                end
            end
            if not group_still_active then
                local btn = conflux.buttons[workspace_name]
                if btn then
                    conflux.widget:remove_widgets(btn)
                    conflux.buttons[workspace_name] = nil
                end
            end
        end)
        return false
    end)
end)

awesome.connect_signal("stellar::conflux_restore", function(data)
    conflux.restore_workspace(data)
end)

awesome.connect_signal("stellar::conflux_ack", function(tx_id)
    local pending = conflux.pending_relocations[tx_id]
    if pending then
        pending.timer:stop()
        conflux.pending_relocations[tx_id] = nil
        stellar_api.log("Relocation " .. tx_id .. " acknowledged successfully. Rollback cancelled.")
    end
end)

stellar_api.log("STELLAR CONFLUX MODULE INITIALIZED")
print("conflux: stellar_api=" .. tostring(stellar_api))
if stellar_api then
    for k, v in pairs(stellar_api) do
        print("  stellar_api." .. k .. " = " .. tostring(v))
    end
end

return conflux
