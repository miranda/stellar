local wibox = require("wibox")
local gears = require("gears")
local awful = require("awful")
local ruled = require("ruled")

local stalonetray = {}

-- Store references to registered wibars per screen
local registered_wibars = {}

-- Re-entrancy guard for geometry sync
local syncing = false

-- Walk the wibar's internal widget hierarchy tree to find the
-- device-coordinate position of a specific widget.  This is the
-- correct way to locate a widget - find_widgets(x,y) expects
-- screen coordinates, NOT a widget reference.
local function find_widget_geo(wibar, target)
    local drawable = wibar._drawable or wibar.drawable
    if not drawable then return nil end

    local root = drawable._widget_hierarchy
    if not root then return nil end

    local function search(node)
        if node:get_widget() == target then
            local mtx = node:get_matrix_to_device()
            local x, y = mtx:transform_point(0, 0)
            local w, h = node:get_size()
            return { x = x, y = y, width = w, height = h }
        end
        for _, child in ipairs(node:get_children()) do
            local hit = search(child)
            if hit then return hit end
        end
        return nil
    end

    return search(root)
end

-- The main container widget
stalonetray.widget = wibox.widget {
    layout       = wibox.layout.fixed.horizontal,
    forced_width = 0, -- Starts collapsed
}

-- The Setup Function: Call this from rc.lua to link a screen's wibar to the module
function stalonetray.register_wibar(screen, wibar_instance)
    registered_wibars[screen] = wibar_instance
end

ruled.client.connect_signal("request::rules", function()
    ruled.client.append_rule {
        id = "stellar_stalonetray",
        rule = { class = "stalonetray" },
        properties = {
            floating          = true,
            titlebars_enabled = false,
            focusable         = false,
            sticky            = true,
            skip_taskbar      = true,
            below             = true,
            placement         = false,
        },
        callback = function(c)
            -- The properties table above gets overridden by rc.lua's
            -- "global" and "titlebars" rules (which are appended later
            -- in load order). But callbacks run AFTER all merged
            -- properties are applied, so these direct assignments win.
            c.floating          = true
            c.focusable         = false
            c.sticky            = true
            c.skip_taskbar      = true
            c.below             = true

            -- The "titlebars" rule already set titlebars_enabled = true
            -- and request::titlebars already fired, so a titlebar exists.
            -- Shrink it to nothing.
            awful.titlebar(c, { size = 0, position = "top" })
            awful.titlebar(c, { size = 0, position = "bottom" })
            awful.titlebar(c, { size = 0, position = "left" })
            awful.titlebar(c, { size = 0, position = "right" })

            if stellar_api and stellar_api.log then
                stellar_api.log("*** stalonetray managed inside module ***")
            end

            local function sync_width(client_obj)
                if not client_obj.valid then return end
                stalonetray.widget.forced_width = client_obj:geometry().width
            end

            local function sync_position(client_obj)
                if not client_obj.valid then return end

                local target_wibar = registered_wibars[client_obj.screen]
                if not target_wibar then return end

                local widget_geo = find_widget_geo(target_wibar, stalonetray.widget)
                if not widget_geo then return end

                local wibox_geo = target_wibar:geometry()

                -- Guard: setting geometry fires property::geometry,
                -- which would call us again → infinite recursion.
                syncing = true
                client_obj:geometry({
                    x = wibox_geo.x + widget_geo.x,
                    y = wibox_geo.y + widget_geo.y,
                })
                syncing = false
            end

            sync_width(c)

            gears.timer.start_new(0.1, function()
                sync_position(c)
                return false
            end)

            c:connect_signal("property::geometry", function(client_obj)
                if syncing then return end
                sync_width(client_obj)
                -- Defer: sync_width just changed forced_width, but the
                -- wibar hasn't re-laid-out yet so the hierarchy is stale.
                -- delayed_call runs after pending layout updates.
                gears.timer.delayed_call(function()
                    if client_obj.valid then
                        sync_position(client_obj)
                    end
                end)
            end)

            c:connect_signal("unmanage", function()
                stalonetray.widget.forced_width = 0
                if stellar_api and stellar_api.log then
                    stellar_api.log("*** stalonetray unmanaged, collapsed widget ***")
                end
            end)
        end
    }
end)

return stalonetray
