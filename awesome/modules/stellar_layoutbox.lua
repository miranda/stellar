---------------------------------------------------------------------------
--- stellar_layoutbox - a drop-in replacement for awful.widget.layoutbox.
--
-- Shows the current layout's icon in the wibar exactly like the stock
-- layoutbox. On mouse-over it raises a popup, anchored next to the widget,
-- containing EVERY available layout arranged in a grid: each cell is the
-- layout's icon with its name printed underneath. Click a cell to switch the
-- tag to that layout. No more click/scroll-cycling through layouts one at a
-- time - every option is visible at once.
--
-- Consistency with the rest of Stellar:
--   * Popup chrome (bg, border colour/width, margins, hover colours) matches
--     the tab context menu and window-button popup in stellar_window.lua.
--   * Hover lifecycle uses stellar_ui.hover_popup() - the same show/hide-timer
--     helper those popups use, so re-entering the popup cancels the hide.
--   * Icon resolution goes through resolve_layout_icon(), which PREFERS a
--     themed Stellar PNG (via stellar_ui.get_image_path) and falls back to the
--     beautiful.layout_<name> icons the theme already ships. Today no layout
--     PNGs are authored, so it transparently uses the beautiful icons; author
--     `layout_tile.png`, `layout_floating.png`, ... in the asset dir later and
--     they light up with zero code changes here.
--
-- @usage
--     local stellar_layoutbox = require("modules.stellar_layoutbox")
--     -- in your wibar setup, in place of `s.mylayoutbox = awful.widget.layoutbox(s)`:
--     s.mylayoutbox = stellar_layoutbox(s)
--     -- ... then add s.mylayoutbox to the wibar as usual.
---------------------------------------------------------------------------

local awful      = require("awful")
local gears      = require("gears")
local wibox      = require("wibox")
local beautiful  = require("beautiful")
local xresources = require("beautiful.xresources")
local dpi        = xresources.apply_dpi

local stellar_ui = require("modules.stellar_ui")

-- `mouse`, `screen`, `client` and `stellar_api` are AwesomeWM globals (no
-- require needed); referenced directly below, as the rest of the Stellar
-- modules do.

---------------------------------------------------------------------------
--  Popup styling - kept in lockstep with stellar_window's popups.
---------------------------------------------------------------------------

local popup_margin       = 6
local popup_border_width = 2
local popup_border_color = "#3300cc"
local popup_bg           = beautiful.bg_normal
local popup_hover_bg     = "#3300cc"
local popup_fg           = "#aaaaff"
local popup_hover_fg     = "#ffffff"

-- Grid geometry.
local grid_columns       = 4		-- cells per row before wrapping
local cell_spacing       = dpi(2)	-- gap between cells (matches popup_spacing feel)
local cell_inner_pad     = dpi(6)	-- padding inside each cell around icon+label
local icon_size          = dpi(24)	-- forced icon box; stock layout PNGs are small
local popup_offset       = 0		-- gap between wibar widget and popup edge

-- Slice category the themed layout PNGs would live under, once authored.
-- get_image_path builds "<assets>/layout_<name>.png".
local LAYOUT_CATEGORY    = "layout"

---------------------------------------------------------------------------
--  Icon resolution: themed Stellar PNG first, beautiful fallback second.
---------------------------------------------------------------------------

-- AwesomeWM layout objects expose a `.name` like "tile", "floating",
-- "fairh", "tileleft", etc. beautiful already carries layout_<name> icon
-- paths (see stellar_theme.build_beautiful_table). We try a themed Stellar
-- sprite by the same key, then fall back to the beautiful icon.
local function resolve_layout_icon(layout)
    local name = layout and layout.name or "floating"

    -- 1. Themed Stellar PNG (not authored yet, but wired for the future).
    --    stellar_ui.get_image_path returns a path string regardless of whether
    --    the file exists, so we must check readability before trusting it.
    if stellar_api and stellar_api.theme_assets_path then
        local themed = stellar_ui.get_image_path(LAYOUT_CATEGORY, name, nil, nil)
        if gears.filesystem.file_readable(themed) then
            return themed
        end
    end

    -- 2. Fall back to the beautiful.layout_<name> icon the theme ships.
    local fallback = beautiful["layout_" .. name]
    return fallback
end

-- Human-readable label for a layout. AwesomeWM layout names are terse
-- ("tileleft", "fairh"); prettify the common ones, otherwise just capitalise.
local PRETTY_NAMES = {
    floating    = "Floating",
    tile        = "Tile",
    tileleft    = "Tile Left",
    tilebottom  = "Tile Bottom",
    tiletop     = "Tile Top",
    fairh       = "Fair (H)",
    fairv       = "Fair (V)",
    magnifier   = "Magnifier",
    max         = "Maximized",
    fullscreen  = "Fullscreen",
    spiral      = "Spiral",
    dwindle     = "Dwindle",
    cornernw    = "Corner NW",
    cornerne    = "Corner NE",
    cornersw    = "Corner SW",
    cornerse    = "Corner SE",
}

local function pretty_name(layout)
    local name = layout and layout.name or "unknown"
    return PRETTY_NAMES[name] or (name:sub(1, 1):upper() .. name:sub(2))
end

---------------------------------------------------------------------------
--  Grid cell factory: icon on top, label underneath, hover highlight.
---------------------------------------------------------------------------

-- Builds one selectable cell for the layout grid. Mirrors the look of
-- stellar_ui.popup_icon_button but stacked vertically (icon over label) and
-- carrying its own hover highlight, since the grid reads better with each
-- option boxed.
--
-- @tparam table opts
--   * layout     (layout) The AwesomeWM layout object this cell represents.
--   * is_current (boolean) Whether this is the tag's active layout.
--   * on_select  (function) Called with the layout when the cell is clicked.
local function make_layout_cell(opts)
    local layout     = opts.layout
    local is_current = opts.is_current
    local on_select  = opts.on_select

    local icon_path  = resolve_layout_icon(layout)
    local label_text = pretty_name(layout)

    local icon = wibox.widget {
        {
            image         = icon_path and gears.surface.load(icon_path) or nil,
            resize        = true,
            forced_width  = icon_size,
            forced_height = icon_size,
            halign        = "center",
            valign        = "center",
            widget        = wibox.widget.imagebox,
        },
        halign = "center",
        valign = "center",
        widget = wibox.container.place,
    }

    -- Active layout gets the hover-foreground straight away so the current
    -- selection is obvious at a glance.
    local rest_fg = is_current and popup_hover_fg or popup_fg
    local rest_bg = is_current and popup_hover_bg or popup_bg

    local label = wibox.widget {
        markup = "<span foreground='" .. rest_fg .. "'>" ..
                 gears.string.xml_escape(label_text) .. "</span>",
        halign = "center",
        valign = "center",
        widget = wibox.widget.textbox,
    }

    local cell = wibox.widget {
        {
            {
                icon,
                label,
                spacing = dpi(2),
                layout  = wibox.layout.fixed.vertical,
            },
            margins = cell_inner_pad,
            widget  = wibox.container.margin,
        },
        bg     = rest_bg,
        fg     = rest_fg,
        shape  = gears.shape.rect,
        widget = wibox.container.background,
    }

    cell:connect_signal("mouse::enter", function()
        cell.bg     = popup_hover_bg
        cell.fg     = popup_hover_fg
        label.markup = "<span foreground='" .. popup_hover_fg .. "'>" ..
                       gears.string.xml_escape(label_text) .. "</span>"
    end)
    cell:connect_signal("mouse::leave", function()
        cell.bg     = rest_bg
        cell.fg     = rest_fg
        label.markup = "<span foreground='" .. rest_fg .. "'>" ..
                       gears.string.xml_escape(label_text) .. "</span>"
    end)

    cell:buttons({
        awful.button({}, 1, function()
            if on_select then on_select(layout) end
        end),
    })

    return cell
end

---------------------------------------------------------------------------
--  The widget itself.
---------------------------------------------------------------------------

-- Read the layouts available to a tag. awful.layout.layouts is the global
-- list; a tag may carry its own subset via t.layouts. Prefer the tag's.
local function layouts_for_tag(t)
    if t and t.layouts and #t.layouts > 0 then
        return t.layouts
    end
    return awful.layout.layouts or {}
end

-- Rebuild the popup grid for the given screen's selected tag.
local function build_grid(s, hover, current_layout, on_select)
    local t        = s.selected_tag
    local layouts  = layouts_for_tag(t)

    local grid = wibox.widget {
        homogeneous   = true,
        expand        = false,
        spacing       = cell_spacing,
        forced_num_cols = grid_columns,
        layout        = wibox.layout.grid,
    }

    for _, layout in ipairs(layouts) do
        grid:add(make_layout_cell {
            layout     = layout,
            is_current = (layout == current_layout),
            on_select  = on_select,
        })
    end

    hover.popup:setup {
        {
            grid,
            margins = popup_margin,
            widget  = wibox.container.margin,
        },
        widget = wibox.container.background,
    }
end

--- Construct a Stellar layoutbox for a screen.
--
-- Returns a widget you can drop into the wibar in place of
-- `awful.widget.layoutbox(s)`. The widget shows the active layout's icon and,
-- on hover, opens the full-grid chooser popup.
--
-- @tparam screen s
-- @treturn widget
local function stellar_layoutbox(s)
    -- The in-wibar icon for the currently active layout.
    local current_icon = wibox.widget {
        resize        = true,
        forced_width  = icon_size,
        forced_height = icon_size,
        widget        = wibox.widget.imagebox,
    }

    local function current_layout()
        return awful.layout.get(s)
    end

    local function refresh_current_icon()
        local path = resolve_layout_icon(current_layout())
        current_icon.image = path and gears.surface.load(path) or nil
    end
    refresh_current_icon()

    -- Wrap so we have a sensible hover/click target with padding.
    local widget = wibox.widget {
        {
            current_icon,
            halign = "center",
            valign = "center",
            widget = wibox.container.place,
        },
        margins = dpi(2),
        widget  = wibox.container.margin,
    }

    -- One reusable hover popup, styled like the rest of Stellar.
    local hover = stellar_ui.hover_popup {
        bg           = popup_bg,
        border_width = popup_border_width,
        border_color = popup_border_color,
        shape        = gears.shape.rect,
        timeout      = 0.3,
    }

    local function on_select(layout)
        local t = s.selected_tag
        if t then
            t.layout = layout
        end
        hover:hide()
        refresh_current_icon()
    end

    -- The widget's on-screen geometry, captured when the popup opens. We hold
    -- onto it so we can RE-anchor after the popup measures itself (see below).
    local anchor_geo = nil

    -- Place the popup against the captured anchor geometry using explicit
    -- coordinate math rather than awful.placement.next_to. next_to with an
    -- explicit `geometry` table is unreliable for wibar widgets (it tends to
    -- ignore preferred_positions and drift toward the screen's left/top - see
    -- awesomeWM issue #3391), and we need deterministic behaviour at the far
    -- right edge. Geometry tables are in absolute screen coordinates.
    --
    -- Strategy: right-align the popup's right edge to the widget's right edge
    -- so a far-right widget opens a popup that extends LEFTwards (the first-
    -- open off-screen bug), drop it just below the wibar, then clamp the whole
    -- rect into the screen workarea as a final guard.
    local function place_popup()
		if not anchor_geo then return end

		local pw = hover.popup.width  or 0
		local ph = hover.popup.height or 0

		local wa = s.geometry
		local margin = dpi(4)

		-- Right edges aligned: popup.x so its right edge meets the widget's.
		local x = anchor_geo.x + anchor_geo.width - pw
		-- Just below the widget (wibar), plus the small offset.
		local y = anchor_geo.y + anchor_geo.height + popup_offset

		-- If dropping below would run off the bottom, flip to above the widget.
		if y + ph > wa.y + wa.height then
		   y = anchor_geo.y - ph - popup_offset
		end

		-- Clamp horizontally into the workarea.
		if x + pw > wa.x + wa.width - margin then x = wa.x + wa.width - margin - pw end
		if x < wa.x + margin then x = wa.x + margin end
		-- Clamp vertically too, as a last resort.
		if y + ph > wa.y + wa.height then y = wa.y + wa.height - ph end
		if y < wa.y                then y = wa.y                end

		hover.popup.x = math.floor(x)
		hover.popup.y = math.floor(y)
    end

    -- THE REANCHOR DANCE (matches stellar_window's popups): the first time the
    -- popup is shown its width/height aren't known yet, so an immediate
    -- placement uses stale geometry and lands wrong. We place once now, then
    -- re-place every time the popup's measured size changes - by the time it's
    -- fully rendered, property::width/height have fired and corrected it.
    hover.popup:connect_signal("property::width",  function()
        if hover.popup.visible then place_popup() end
    end)
    hover.popup:connect_signal("property::height", function()
        if hover.popup.visible then place_popup() end
    end)

    ---------------------------------------------------------------------
    --  Layer / input guard
    ---------------------------------------------------------------------
    -- Problem: a window whose titlebar sits directly beneath the popup will
    -- still receive mouse::enter on its titlebar menu button, because the
    -- popup being painted on top does not consume the underlying widget's
    -- pointer-crossing events.
    --
    -- The fix is a shared flag, stellar_api._layout_popup_open, in the same
    -- spirit as stellar_api._grab: it is true exactly while the grid is up.
    -- stellar_window's titlebar hover handler checks it and bails (one-line
    -- edit, see the integration note at the bottom of this file). This is the
    -- same "coordinate via a stellar_api flag" pattern the grab system already
    -- uses, and unlike a mousegrabber it does NOT swallow the clicks we need
    -- for selecting a layout cell.
    --
    -- (A mousegrabber was considered to make this work with zero changes to
    -- stellar_window, but a grabber owns ALL pointer events for its lifetime,
    -- so it would eat the cell clicks - making the grid unusable. The flag is
    -- the right tool here.)

    -- Build, anchor, and show the popup. The reanchor dance above corrects the
    -- position once the popup has measured itself.
    local function show_popup()
        hover.timer:stop()
        if hover.popup.visible then return end

        build_grid(s, hover, current_layout(), on_select)

        -- Capture the hovered widget's absolute geometry NOW, for anchoring.
        anchor_geo = mouse.current_widget_geometry or anchor_geo

        hover.popup.visible            = true
        stellar_api._layout_popup_open = true

        place_popup()      -- first pass (stale size); property::* will correct.
    end

    -- Clear the guard flag whenever the popup hides, however it was dismissed
    -- (timeout, selection, focus change).
    hover.popup:connect_signal("property::visible", function()
        if not hover.popup.visible then
            stellar_api._layout_popup_open = false
        end
    end)

    -- Hover lifecycle on the wibar widget. The popup helper's own
    -- mouse::enter/leave (wired by stellar_ui.hover_popup) keeps it alive when
    -- the pointer moves onto the popup, and starts the hide timer when it
    -- leaves the popup - so moving widget -> popup -> a cell stays open, and
    -- moving away dismisses after the timeout.
    widget:connect_signal("mouse::enter", show_popup)
    widget:connect_signal("mouse::leave", function()
        hover:schedule_hide()
    end)

    -- Cross-screen teardown (zaphodheads) is handled inside stellar_ui's
    -- hover_popup: it subscribes to "stellar::pointer_left_screen" and hides
    -- itself when the pointer swipes onto another monitor. Nothing to do here.

    -- Also support the stock interactions so muscle memory still works:
    -- left-click cycles forward, right-click backward, scroll cycles.
    widget:buttons(gears.table.join(
        awful.button({}, 1, function() awful.layout.inc( 1, s) end),
        awful.button({}, 3, function() awful.layout.inc(-1, s) end),
        awful.button({}, 4, function() awful.layout.inc( 1, s) end),
        awful.button({}, 5, function() awful.layout.inc(-1, s) end)
    ))

    -- Keep the wibar icon in sync when the layout changes by any means.
    local function on_layout_change(t)
        if t and t.screen == s then
            refresh_current_icon()
        end
    end
    awful.tag.attached_connect_signal(s, "property::selected",        function() refresh_current_icon() end)
    awful.tag.attached_connect_signal(s, "property::layout",          on_layout_change)
    awful.tag.attached_connect_signal(s, "property::activated",       function() refresh_current_icon() end)
    -- Tag switches change which tag's layout we display.
    s:connect_signal("tag::history::update", refresh_current_icon)

    -- Hand back the widget; stash the popup so callers could destroy it.
    widget._stellar_layout_hover = hover
    return widget
end

return stellar_layoutbox

---------------------------------------------------------------------------
--  INTEGRATION NOTE - optional one-line hardening in stellar_window.lua
---------------------------------------------------------------------------
--
-- This module sets stellar_api._layout_popup_open = true exactly while the
-- layout grid is visible (false otherwise), mirroring the stellar_api._grab
-- convention. For the titlebar menu button NOT to activate when it sits
-- directly beneath the open grid, stellar_window's titlebar hover handler
-- should consult that flag and bail. In stellar_window.lua, the hover_area
-- enter handler (~line 1155) becomes:
--
--     hover_area:connect_signal("mouse::enter", function()
--         if stellar_api._layout_popup_open then return end   -- <-- add this
--         hide_timer:stop()
--         place_popup()
--         popup_buttons.visible = true
--     end)
--
-- That single guard is the layering fix. (The tab context-menu hover in the
-- bottom titlebar can take the same guard if you find it activating under the
-- grid too.)
---------------------------------------------------------------------------
