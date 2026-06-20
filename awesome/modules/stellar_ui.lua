---------------------------------------------------------------------------
--- stellar_ui - shared UI primitives for the Stellar desktop environment.
--
-- Centralises image resolution, slice widget factories, layout helpers,
-- popup utilities, and the tab-strip composite widget so that every
-- module (window decorations, wibar tasklist, future panels) draws from
-- the same library.
--
-- @usage
--     local sui = require("modules.stellar_ui")
--
--     -- Load theme data once at startup
--     local slice_data = sui.load_slice_data(stellar_api.theme_assets_path)
--
--     -- Create a static titlebar slice
--     local corner = sui.create_slice(c, "win", "upper_left", false, slice_data)
--
--     -- Create a tab strip for a wibar tasklist
--     local strip = sui.tab_strip(config)
--     strip:update(descriptors)
---------------------------------------------------------------------------

local awful     = require("awful")
local gears     = require("gears")
local wibox     = require("wibox")
local beautiful = require("beautiful")

local sui = {}

---------------------------------------------------------------------------
--
--  1. THEME / IMAGE RESOLUTION
--
---------------------------------------------------------------------------

--- Slice names that respond to the "active" mode.
-- If a slice name is in this set and the client is focused + active + pointer
-- on screen, it renders as "active" instead of "focused".
sui.active_slices = {
    upper_left      = true, title_trans  = true, bar_flex = true,
    button_trans    = true, upper_right  = true,
    left_upper      = true, right_upper  = true,
    left_lower      = true, right_lower  = true,
    lower_left      = true, lower_right  = true,
    -- Tabbed-client slices
    tab_left_lower  = true, tab_right_lower     = true,
    tab_lower_left_normal   = true,
    tab_lower_left_selected = true,
    tab_lower_right         = true,
}

--- Build a filesystem path for a themed slice image.
--
-- @tparam string category    e.g. "win"
-- @tparam string sprite_name e.g. "upper_left"
-- @tparam string|nil mode    e.g. "focused", "active", nil
-- @treturn string            Absolute PNG path.
function sui.get_image_path(category, sprite_name, variation, focus_mode)
    if focus_mode and not variation then
        return string.format("%s/%s_%s_%s.png",
            stellar_api.theme_assets_path, category, sprite_name, focus_mode)
    elseif variation and not focus_mode then
        return string.format("%s/%s_%s_%s.png",
            stellar_api.theme_assets_path, category, sprite_name, variation)
    elseif variation and focus_mode then
        return string.format("%s/%s_%s_%s_%s.png",
            stellar_api.theme_assets_path, category, sprite_name, variation, focus_mode)
    else
        return string.format("%s/%s_%s.png",
            stellar_api.theme_assets_path, category, sprite_name)
    end
end

--- Determine the button-overlay mode string for a client.
--
-- @tparam client  c
-- @tparam boolean is_focused
-- @treturn string e.g. "maximized_focused", "tiled_unfocused"
function sui.get_button_focus_mode(c, is_focused)
    local focus = is_focused and "focused" or "unfocused"

    if c.maximized then
        return "maximized_" .. focus
    elseif c.sticky then
        return "sticky_" .. focus
    elseif c.ontop then
        return "ontop_" .. focus
    elseif c.floating then
        return "floating_" .. focus
    end

    return "tiled_" .. focus
end

--- Resolve the full image path for a given client + slice, accounting for
-- focus, active glow, and the special "button" mode logic.
--
-- @tparam client c
-- @tparam string category    e.g. "win"
-- @tparam string slice_name  e.g. "upper_left", "button"
-- @treturn string             Absolute PNG path.
function sui.get_slice_image(c, category, slice_name, variation)
    local is_focused = client.focus == c

    local focus_mode = nil
	if category == "win" or category == "tab" then
		if slice_name == "button" then
			focus_mode = sui.get_button_focus_mode(c, is_focused)
		elseif is_focused then
			if  sui.active_slices[slice_name]
				and not c.stellar_locked
				and c.stellar_active
				and stellar_api._pointer_on_screen
			then
				focus_mode = "active"
			else
				focus_mode = "focused"
			end
		else
			focus_mode = "unfocused"
		end
	end

    return sui.get_image_path(category, slice_name, variation, focus_mode)
end

--- Load the slice dimension / metadata table from a theme's asset directory.
--
-- Expects `<asset_dir>/slice_data.lua` to return a nested table like
-- `{ win = { upper_left = { w = 12, h = 34 }, ... }, ... }`.
--
-- @tparam string asset_dir
-- @treturn table
function sui.load_slice_data(asset_dir)
    local data_path = asset_dir .. "/slice_data.lua"
    local chunk, err = loadfile(data_path)
    if not chunk then
        require("gears.debug").print_warning(
            "stellar_ui: could not load slice data: " .. tostring(err))
        return {}
    end
    return chunk()
end

---------------------------------------------------------------------------
--
--  2. SLICE WIDGET FACTORIES
--
---------------------------------------------------------------------------

--- Default set of client signals that should trigger a slice redraw.
sui.default_refresh_signals = {
    "focus",
    "unfocus",
    "property::stellar_locked",
    "property::stellar_active",
    "stellar::refresh_slices",
}

--- Create a static slice imagebox bound to a client.
--
-- The image auto-updates on focus / unfocus / active-state changes.
-- This is the workhorse for every fixed-name titlebar segment (corners,
-- edges, flex fills).
--
-- @tparam client  c
-- @tparam string  category    e.g. "win"
-- @tparam string  slice_name  e.g. "upper_left"
-- @tparam boolean stretch     true → image repeats along one axis
-- @tparam table   slice_data  theme slice metadata
-- @tparam string  direction   "horizontal" (default) or "vertical"; only
--                              matters when stretch is true
-- @treturn widget             An imagebox that refreshes automatically.
function sui.create_slice(c, category, slice_name, stretch, slice_data, variation, direction)
    local category_data = slice_data[category] or {}
    local slice_info    = category_data[slice_name]

    local h_policy, v_policy
    local do_resize = false

    if stretch then
        do_resize = true
        if direction == "vertical" then
            h_policy = "fit"
            v_policy = "repeat"
        else
            h_policy = "repeat"
            v_policy = "fit"
        end
    end

    local img_widget = wibox.widget {
        image                 = gears.surface.load(sui.get_slice_image(c, category, slice_name, variation)),
        resize                = do_resize,
        horizontal_fit_policy = h_policy,
        vertical_fit_policy   = v_policy,
        forced_height         = slice_info and slice_info.h or nil,
        forced_width          = slice_info and slice_info.w or nil,
        widget                = wibox.widget.imagebox,
    }

    local function update_surface()
        img_widget.image = gears.surface.load(sui.get_slice_image(c, category, slice_name, variation))
    end

    for _, sig in ipairs(sui.default_refresh_signals) do
        c:connect_signal(sig, update_surface)
    end

    return img_widget
end

--- Create a dynamic-name slice imagebox with tracked signal connections.
--
-- Unlike create_slice, the slice name is computed by a callback each time
-- the image refreshes - essential for tab-strip segments whose name
-- includes a hover/selection state suffix.
--
-- Signal connections are appended to `connections` so the caller can
-- disconnect them on rebuild to prevent leaked closures.
--
-- @tparam table    config      Strip-level config (see tab_strip.new docs).
-- @tparam table    connections Mutable list; {signal, fn} pairs are appended.
-- @tparam function name_fn     `function() → string` returning the current slice key.
-- @tparam boolean  stretch
-- @tparam string   direction   "horizontal" or "vertical"
-- @treturn widget              An imagebox with :update_surface().
function sui.create_dynamic_slice(config, connections, name, variation_fn, stretch, direction)
    local category      = config.category or "tab"
    local slice_data    = config.slice_data
    local category_data = slice_data[category] or {}

	local initial_variation = variation_fn()
    local slice_info		= category_data[name]

    local h_policy, v_policy
    local do_resize = false

    if stretch then
        do_resize = true
        if direction == "vertical" then
            h_policy = "fit"
            v_policy = "repeat"
        else
            h_policy = "repeat"
            v_policy = "fit"
        end
    end

    local img_widget = wibox.widget {
        image                 = gears.surface.load(config.get_slice_image(category, name, initial_variation)),
        resize                = do_resize,
        horizontal_fit_policy = h_policy,
        vertical_fit_policy   = v_policy,
        forced_height         = slice_info and slice_info.h or nil,
        forced_width          = slice_info and slice_info.w or nil,
        widget                = wibox.widget.imagebox,
    }

    function img_widget:update_surface()
		local current_variation = variation_fn()
        self.image = gears.surface.load(config.get_slice_image(category, name, current_variation))

        local info = category_data[name]
        if info then
            self.forced_width  = info.w
            self.forced_height = info.h
        end
    end

    -- Auto-refresh on signals from the config's signal_source
    if config.signal_source and config.refresh_signals then
        for _, signal in ipairs(config.refresh_signals) do
            local fn = function() img_widget:update_surface() end
            config.signal_source:connect_signal(signal, fn)
            table.insert(connections, { signal, fn })
        end
    end

    return img_widget
end

--- Trigger a decoration redraw on a client without firing real focus events.
--
-- Emits "stellar::refresh_slices" which all create_slice widgets listen to,
-- avoiding the side-effects of focus/unfocus (picom transitions, popup
-- dismissal, awful's focus tracking, etc.).
--
-- @tparam client c
function sui.update_decorations(c)
    if not c or not c.valid then return end
    c:emit_signal("stellar::refresh_slices")
end

---------------------------------------------------------------------------
--
--  3. LAYOUT HELPERS
--
---------------------------------------------------------------------------

--- Wrap a child widget in an adaptive margin that centres narrow content
-- within a guaranteed minimum width.
--
-- When the child is narrower than `min_width` the margin grows
-- symmetrically to fill the gap; when wider it stays at `min_margin`.
--
-- @tparam widget child
-- @tparam table  args  Optional: `min_width` (default 80), `min_margin` (default 4).
-- @treturn widget
function sui.adaptive_min_width_margin(child, args)
    args = args or {}

    local min_width  = args.min_width  or 80
    local min_margin = args.min_margin or 4

    local margin = wibox.container.margin(child, min_margin, min_margin)

    local container = wibox.widget {
        margin,
        strategy = "min",
        width    = min_width,
        widget   = wibox.container.constraint,
    }

    function container:fit(context, width, height)
        local child_width = 0

        if child.fit then
            child_width = select(1, child:fit(context, width, height))
        else
            child_width = min_width
        end

        local extra          = min_width - child_width
        local dynamic_margin = min_margin

        if extra > min_margin * 2 then
            dynamic_margin = math.floor(extra / 2)
        end
        if dynamic_margin < min_margin then
            dynamic_margin = min_margin
        end

        margin.left  = dynamic_margin
        margin.right = dynamic_margin

        return wibox.container.constraint.fit(self, context, width, height)
    end

    return container
end

---------------------------------------------------------------------------
--
--  4. POPUP UTILITIES
--
---------------------------------------------------------------------------

--- Create a reusable hover popup with a show/hide timer.
--
-- Returns a table with `.popup`, `.timer`, and helper methods.  The popup
-- auto-hides after `timeout` seconds once the cursor leaves, and cancels
-- the hide if the cursor re-enters.
--
-- This pattern is used by both the tab context menu and the window-button
-- popup, so factoring it here avoids duplicating the timer/signal wiring.
--
-- @tparam table args
--   * bg           (string)  Background colour (default beautiful.bg_normal)
--   * border_width (number)  (default 2)
--   * border_color (string)  (default "#3300cc")
--   * timeout      (number)  Hide delay in seconds (default 0.3)
--   * shape        (function) (default gears.shape.rect)
-- @treturn table  `{ popup, timer, show(), hide(), destroy() }`
function sui.hover_popup(args)
    args = args or {}

    local popup = awful.popup {
        type         = "combo",
        ontop        = true,
        visible      = false,
        bg           = args.bg           or beautiful.bg_normal,
        border_width = args.border_width or 2,
        border_color = args.border_color or "#3300cc",
        shape        = args.shape        or gears.shape.rect,
        widget       = wibox.widget.textbox(""),
    }

    local hide_timer = gears.timer {
        timeout     = args.timeout or 0.3,
        single_shot = true,
        callback    = function()
            popup.visible = false
        end,
    }

    popup:connect_signal("mouse::enter", function() hide_timer:stop() end)
    popup:connect_signal("mouse::leave", function() hide_timer:again() end)

    local obj = {
        popup = popup,
        timer = hide_timer,
    }

    function obj:show()
        hide_timer:stop()
        popup.visible = true
    end

    function obj:hide()
        hide_timer:stop()
        popup.visible = false
    end

    function obj:schedule_hide()
        hide_timer:again()
    end

    function obj:destroy()
        popup.visible = false
        hide_timer:stop()
    end

    return obj
end

--- Create a popup menu item with icon + label, hover highlighting, and
-- a click callback.
--
-- This is the building block for both the window-button popup and the tab
-- context menu.  It is a pure widget factory with no closures over client
-- state - pass everything explicitly.
--
-- @tparam table opts
--   * image_inactive (string)         Path to the default icon image.
--   * image_active   (string|nil)     Path to the active-state icon, or nil.
--   * property       (string|nil)     Client property to track for active state.
--   * client         (client|nil)     Client to observe (required if property is set).
--   * callback       (function)       Left-click action.
--   * text_label     (string)         Display text.
--   * slice_data     (table)          Theme slice metadata (for dimensions).
--   * category       (string)         Slice category, default "win".
--   * slice_name     (string)         Key into slice_data for forced dimensions.
--   * bg_normal      (string)         Normal background  (default beautiful.bg_normal).
--   * fg_normal      (string)         Normal foreground   (default "#aaaaff").
--   * bg_hover       (string)         Hover background    (default "#3300cc").
--   * fg_hover       (string)         Hover foreground    (default "#ffffff").
--   * hover_highlight(boolean)        Paint bg_hover/fg_hover on hover (default true).
--                                     Pass false for icon-PNG-only buttons.
-- @treturn widget
function sui.popup_icon_button(opts)
    local image_inactive = opts.image_inactive
    local image_active   = opts.image_active
    local property       = opts.property
    local c              = opts.client
    local callback       = opts.callback
    local text_label     = opts.text_label
    local slice_name     = opts.slice_name
    local category       = opts.category or "win"
    local bg_normal      = opts.bg_normal or beautiful.bg_normal
    local fg_normal      = opts.fg_normal or "#aaaaff"
    local bg_hover       = opts.bg_hover  or "#3300cc"
    local fg_hover       = opts.fg_hover  or "#ffffff"
    -- When true (default), the whole button paints bg_hover/fg_hover on hover.
    -- Callers whose buttons signal state purely through icon PNGs (e.g. the
    -- window titlebar menu) pass false to keep the button flat on bg_normal.
    local hover_highlight = opts.hover_highlight ~= false

    local category_data = (opts.slice_data and opts.slice_data[category]) or {}
    local slice_info    = category_data[slice_name]

    local icon = wibox.widget {
        image  = image_inactive,
        resize = false,
        widget = wibox.widget.imagebox,
    }

    local label = wibox.widget {
        markup = text_label,
        valign = "center",
        halign = "right",
        widget = wibox.widget.textbox,
    }

    local function update()
        local is_active = property and c and c.valid and c[property] or false

        if property and image_active then
            icon.image = is_active and image_active or image_inactive
        end

        local text_color = is_active and "#FFFFFF" or fg_normal
        label.markup = "<span foreground='" .. text_color .. "'>"
            .. text_label .. "</span>"
    end

    if property and c then
        c:connect_signal("property::" .. property, update)
    end
    update()

    local button = wibox.widget {
        {
            {
                nil,
                label,
                {
                    {
                        {
                            icon,
                            halign = "center",
                            valign = "center",
                            widget = wibox.container.place,
                        },
                        forced_width = slice_info and slice_info.w or nil,
                        strategy     = "exact",
                        widget       = wibox.container.constraint,
                    },
                    left   = 8,
                    widget = wibox.container.margin,
                },
                layout = wibox.layout.align.horizontal,
            },
            forced_height = slice_info and slice_info.h or nil,
            strategy      = "exact",
            widget        = wibox.container.constraint,
        },
        bg     = bg_normal,
        widget = wibox.container.background,
    }

    if hover_highlight then
        button:connect_signal("mouse::enter", function()
            button.bg = bg_hover
            button.fg = fg_hover
        end)
        button:connect_signal("mouse::leave", function()
            button.bg = bg_normal
            button.fg = fg_normal
        end)
    end

    button:buttons({ awful.button({}, 1, callback) })
    return button
end

--- Create a simple text menu item with hover highlighting.
--
-- Used inside tab context menus and similar vertical menu popups.
--
-- @tparam table opts
--   * text      (string)   Display text.
--   * callback  (function) Left-click action.
--   * bg_normal (string)   Default beautiful.bg_normal.
--   * fg_normal (string)   Default "#aaaaff".
--   * bg_hover  (string)   Default "#3300cc".
--   * fg_hover  (string)   Default "#ffffff".
-- @treturn widget
function sui.menu_item(opts)
    local bg_normal = opts.bg_normal or beautiful.bg_normal
    local fg_normal = opts.fg_normal or "#aaaaff"
    local bg_hover  = opts.bg_hover  or "#3300cc"
    local fg_hover  = opts.fg_hover  or "#ffffff"

    local item = wibox.widget {
        {
            {
                text   = opts.text,
                valign = "center",
                widget = wibox.widget.textbox,
            },
            margins = { left = 10, right = 16, top = 5, bottom = 5 },
            widget  = wibox.container.margin,
        },
        bg     = bg_normal,
        fg     = fg_normal,
        widget = wibox.container.background,
    }

    item:connect_signal("mouse::enter", function()
        item.bg = bg_hover
        item.fg = fg_hover
    end)
    item:connect_signal("mouse::leave", function()
        item.bg = bg_normal
        item.fg = fg_normal
    end)

    if opts.callback then
        item:buttons({ awful.button({}, 1, nil, opts.callback) })
    end

    return item
end

---------------------------------------------------------------------------
--
--  5. TAB STRIP - composite widget
--
---------------------------------------------------------------------------

--- Create a new tab-strip widget.
--
-- The returned object is a `wibox.layout.fixed.horizontal` with `:update()`
-- and `:disconnect_all()` methods.  It renders a row of sliced-image tabs
-- with hover states, an optional (+) button, and optional signal-driven
-- refresh.  Usable in titlebars, wibars, or standalone popups.
--
-- @tparam table config  Configuration:
--
--   REQUIRED
--   * slice_data        (table)    Theme slice dimension metadata.
--   * get_slice_image   (function) `function(category, name) → path_or_surface`
--
--   OPTIONAL
--   * category          (string)   Slice category key (default "win").
--   * max_tab_width     (number)   Pixel cap per tab (default 250).
--   * min_tab_width     (number)   Minimum label width (default 80).
--   * min_margin        (number)   Minimum label side margin (default 4).
--   * signal_source     (object)   Any object with connect_signal / disconnect_signal.
--   * refresh_signals   (table)    Signal names that trigger visual refresh.
--   * new_button        (table)    `{ on_click = fn }` or nil.
--   * on_fit            (function) `function(w, h)` called after every layout fit.
--
-- @treturn widget
function sui.tab_strip(config)
    assert(config.slice_data,      "tab_strip: slice_data is required")
    assert(config.get_slice_image, "tab_strip: get_slice_image is required")

    config.category      = config.category      or "win"
    config.max_tab_width = config.max_tab_width or 250
    config.min_tab_width = config.min_tab_width or 80
    config.min_margin    = config.min_margin    or 4

    local connections    = {}
    local smart_widgets  = {}
    local element_states = {}

    local layout = wibox.layout.fixed.horizontal()

    -- Optional fit callback for external synchronisation (X properties, etc.)
    if config.on_fit then
        local original_fit = layout.fit
        layout.fit = function(self, context, width, height)
            local w, h = original_fit(self, context, width, height)
            config.on_fit(w, h)
            return w, h
        end
    end

    local function refresh_all()
        for _, w in ipairs(smart_widgets) do
            w:update_surface()
        end
    end

    --- Rebuild the strip from a list of tab descriptors.
    --
    -- Each descriptor is a table:
    --   REQUIRED
    --   * id          (any)      Unique key (opaque to the strip).
    --   * is_active   (boolean)  Whether this tab is "selected" at rest.
    --   * label       (widget)   Content shown over the centre flex.
    --
    --   OPTIONAL
    --   * text_color        (string)   Label foreground (default "#FFFFFF").
    --   * on_click          (function) Left-click callback.
    --   * on_context_enter  (function) `fn(right_joint_widget, index, descriptor)`
    --   * on_context_leave  (function) `fn(right_joint_widget, index, descriptor)`
    function layout:update(descriptors)
        -- Tear down previous signals
        if config.signal_source then
            for _, conn in ipairs(connections) do
                config.signal_source:disconnect_signal(conn[1], conn[2])
            end
        end
        connections    = {}
        smart_widgets  = {}
        element_states = {}
        self:reset()

        local N           = #descriptors
        local new_btn_idx = N + 1

        for i, desc in ipairs(descriptors) do
            element_states[i] = desc.is_active and "selected" or "normal"
        end
        element_states[new_btn_idx] = "normal"

        -------------------------------------------------------------------
        -- Build each tab
        -------------------------------------------------------------------
        for i, desc in ipairs(descriptors) do

            -- LEFT JOINT
			local left_name = (i == 1) and "lower_left" or "join_right"
            local img_left = sui.create_dynamic_slice(config, connections, left_name, function()
                if i == 1 then
                    return element_states[i]
                else
                    return element_states[i - 1] .. "_" .. element_states[i]
                end
            end, false)

            -- CENTRE FLEX
            local img_flex = sui.create_dynamic_slice(config, connections, "title_flex", function() 
				return element_states[i]
            end, true, "horizontal")

            -- RIGHT JOINT
            local img_right = sui.create_dynamic_slice(config, connections, "join_left", function()
				local next_state = element_states[i + 1]
				return element_states[i] .. "_" .. next_state
            end, false)

            -- Cache drawable X offset via cairo matrix (for popup anchoring)
            local orig_draw = img_right.draw
            function img_right:draw(ctx, cr, w, h)
                self._tab_strip_drawable_x = cr:get_matrix().x0
                return orig_draw(self, ctx, cr, w, h)
            end

            -- Context-menu hooks on the right joint
            if desc.on_context_enter then
                img_right:connect_signal("mouse::enter", function()
                    desc.on_context_enter(img_right, i, desc)
                end)
            end
            if desc.on_context_leave then
                img_right:connect_signal("mouse::leave", function()
                    desc.on_context_leave(img_right, i, desc)
                end)
            end

            -- Label wrapped in adaptive margin + coloured background
            local label_widget
            if desc.label then
                local inner  = desc.label
                inner.halign = inner.halign or "center"
                inner.valign = inner.valign or "center"

                label_widget = wibox.widget {
                    sui.adaptive_min_width_margin(inner, {
                        min_width  = config.min_tab_width,
                        min_margin = config.min_margin,
                    }),
                    fg     = desc.text_color or "#FFFFFF",
                    widget = wibox.container.background,
                }
            end

            table.insert(smart_widgets, img_left)
            table.insert(smart_widgets, img_flex)
            table.insert(smart_widgets, img_right)

            -- Stack flex background + label, clamped to max width
            local stack_children = { img_flex }
            if label_widget then
                table.insert(stack_children, label_widget)
            end

            local flex_stack = wibox.widget {
                {
                    layout = wibox.layout.stack,
                    table.unpack(stack_children),
                },
                strategy = "max",
                width    = config.max_tab_width,
                widget   = wibox.container.constraint,
            }

            local tab_wrapper = wibox.widget {
                img_left, flex_stack, img_right,
                layout = wibox.layout.fixed.horizontal,
            }

            -- Hover
            tab_wrapper:connect_signal("mouse::enter", function()
                element_states[i] = "selected"
                refresh_all()
            end)
            tab_wrapper:connect_signal("mouse::leave", function()
                if not desc.is_active then
                    element_states[i] = "normal"
                end
                refresh_all()
            end)

            -- Click
            if desc.on_click then
                tab_wrapper:buttons({
                    awful.button({}, 1, function() desc.on_click() end),
                })
            end

            self:add(tab_wrapper)
        end

        -------------------------------------------------------------------
        -- (+) New-tab button
        -------------------------------------------------------------------
        if config.new_button then
            local img_new = sui.create_dynamic_slice(config, connections, "join_new", function()
				return element_states[N] .. "_" .. element_states[new_btn_idx]
            end, false)

            local img_end = sui.create_dynamic_slice(config, connections, "join_end", function()
				return element_states[new_btn_idx]
            end, false)

            table.insert(smart_widgets, img_new)
            table.insert(smart_widgets, img_end)

            local new_wrapper = wibox.widget {
                img_new, img_end,
                layout = wibox.layout.fixed.horizontal,
            }

            new_wrapper:connect_signal("mouse::enter", function()
                element_states[new_btn_idx] = "selected"
                refresh_all()
            end)
            new_wrapper:connect_signal("mouse::leave", function()
                element_states[new_btn_idx] = "normal"
                refresh_all()
            end)

            if config.new_button.on_click then
                new_wrapper:buttons({
                    awful.button({}, 1, function() config.new_button.on_click() end),
                })
            end

            self:add(new_wrapper)
        end
    end

    --- Disconnect all tracked signal connections.
    -- Call when the strip is permanently destroyed (e.g. client unmanage).
    function layout:disconnect_all()
        if config.signal_source then
            for _, conn in ipairs(connections) do
                config.signal_source:disconnect_signal(conn[1], conn[2])
            end
        end
        connections   = {}
        smart_widgets = {}
    end

    return layout
end

return sui
