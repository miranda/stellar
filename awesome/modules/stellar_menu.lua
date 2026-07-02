-- stellar_menu.lua
-- AwesomeWM-side application menu for Stellar.
--
-- Receives the categorized menu document the DE builds in C (pushed over IPC
-- as a "MENU_DATA <json>" line and decoded by stellar_bridge.lua), and turns
-- it into an awful.menu. The DE owns the data - it scans the .desktop files -
-- so this module is purely a renderer: it caches the latest document and
-- rebuilds the awful.menu whenever fresh data arrives.
--
-- Data shape (already sorted, empty categories omitted):
--   { categories = {
--       { name = "Internet", items = {
--           { name="Firefox", exec="firefox", icon="firefox", terminal=false },
--           ...
--       } },
--       ...
--   } }
--
-- Icons are passed as raw Icon= names/paths; we resolve them to files here at
-- build time using AwesomeWM's icon lookup, which is the icon-theme-aware layer.
--
-- Placement: drop this in the awesome `modules/` directory and require it as
-- `require("modules.stellar_menu")` from BOTH stellar_bridge.lua (the MENU_DATA
-- handler) and rc.lua (bind_main_menu). Lua caches the module, so both share
-- one instance - that shared instance is what carries data from the bridge's
-- IPC handler to rc.lua's menu binding.
--
-- Load-order safe: the bridge's process_command (and thus set_menu_data) is
-- installed BEFORE rc.lua runs, and C may push MENU_DATA before rc.lua finishes
-- loading. set_menu_data always caches but only rebuilds once a binding exists;
-- bind_main_menu builds once using whatever data already arrived. So both
-- data-before-bind and bind-before-data converge correctly.

local awful     = require("awful")
local menubar   = require("menubar")
local wibox     = require("wibox")
local gears     = require("gears")
local beautiful = require("beautiful")
local stellar_ui = require("modules.stellar_ui")
local behavior  = require("modules.stellar_menu_behavior")

local M = {}

-- Latest decoded menu document (or nil before the first MENU_DATA push).
local _menu_data = nil

-- Caller-provided hooks, installed via bind_main_menu():
--   _header_fn()      -> array of static menu items to place ABOVE the apps
--   _on_rebuild(menu) -> receives the freshly built awful.menu object
local _header_fn  = nil
local _on_rebuild = nil

-- State trackers for the launcher icon
local _current_menu = nil
local _launcher_widget = nil
local _is_menu_open = false
local _is_hovered = false

-- Evaluates state and updates the widget's image
local function update_launcher_icon()
    if not _launcher_widget then return end
    
    local mode = "normal"
    if _is_menu_open or _is_hovered then
        mode = "selected"
    end
    
    _launcher_widget.image = stellar_ui.get_image_path("sys", "stellar_logo", mode)
end

-- ------------------------------------------------------------------------
-- Separator items
-- ------------------------------------------------------------------------
-- awful.menu has no built-in divider: every item is a clickable entry. But an
-- item may carry its own constructor via a `new` field, which awful.menu calls
-- as new(parent, args) instead of the default awful.menu.entry. That
-- constructor must return a table with a `widget` field (rendered into the
-- menu) and may set `cmd` (the action). We return a horizontal-rule widget and
-- leave `cmd` nil, so the row draws a line and does nothing on click. We also
-- attach no mouse handlers, so it never highlights on hover.
--
-- Use it by placing M.separator() wherever a menu item would go, at the top
-- level or inside any submenu:
--     { "hotkeys", ... },
--     stellar_menu.separator(),
--     { "quit", ... },
local function make_separator_widget(args)
    -- The real line.
    local line = wibox.widget {
        orientation = "horizontal",
        forced_height = 1,
        span_ratio    = 0.95,
        color = (args and args.theme and args.theme.fg_normal)
                or beautiful.menu_fg_normal
                or beautiful.fg_normal
                or "#888888",
        widget = wibox.widget.separator,
    }

    local item_h = (args and args.theme and args.theme.height)
                   or beautiful.menu_height or 16

    -- CRITICAL: a bare separator's preferred width is effectively unbounded (a
    -- horizontal line "wants" infinite width). awful.menu sizes each menu to
    -- its widest item's preferred width, so an unconstrained separator blows
    -- the whole menu out to the maximum. We pin the widget's preferred width to
    -- the menu's own width (args.theme.width, computed by compute_width) so the
    -- separator never drives sizing - it just fills the row the text defined.
    local menu_w = (args and args.theme and args.theme.width)
                   or beautiful.menu_width or 100

    return wibox.widget {
        {
            line,
            top    = math.floor(item_h / 2),
            bottom = math.floor(item_h / 2),
            widget = wibox.container.margin,
        },
        forced_height = item_h,
        forced_width  = menu_w,   -- finite: stops the separator from widening the menu
        widget = wibox.container.background,
    }
end

-- Per-item constructor matching awful.menu's `new(parent, args)` contract.
-- Returns the menu-item table awful.menu expects: a `widget` to render and no
-- `cmd`, so the row is inert.
local function separator_entry(parent, args) -- luacheck: no unused args
    return {
        widget = make_separator_widget(args),
        cmd    = nil,   -- no action: clicking does nothing
        akey   = nil,   -- no access key
    }
end

-- Public: returns a menu item that renders as a divider line. Drop it anywhere
-- an item table would go (top level or inside a submenu).
function M.separator()
    return { new = separator_entry }
end

-- ------------------------------------------------------------------------
-- Menu width
-- ------------------------------------------------------------------------
-- awful.menu uses ONE fixed width for every item in a menu (from theme.width,
-- falling back to beautiful.menu_width); it does not auto-fit. Long names get
-- ellipsized. To make each menu just wide enough for its longest label (up to a
-- cap), we measure every label in the menu's font and pass the result as the
-- menu's theme.width.
--
-- Measurement uses an offscreen textbox's :get_preferred_size - the width the
-- text would take with infinite space - in the exact menu font, so it accounts
-- for proportional fonts correctly (unlike counting characters).

-- Tunables.
local MENU_WIDTH_MIN  = 140    -- never narrower than this (px)
local MENU_WIDTH_MAX  = 420    -- cap before ellipsizing kicks in (px)
local MENU_TEXT_PAD   = 0      -- extra px added to measured text (set below)

-- A single reusable measuring textbox.
local _measure_tb = wibox.widget.textbox()

-- Per-item horizontal chrome that is NOT the label text but still consumes
-- width: left icon column + submenu arrow + internal padding/margins. awful.menu
-- doesn't expose these precisely, so we approximate from theme values. Erring
-- slightly high just means a touch more right-margin, which is harmless; erring
-- low would re-introduce truncation, so we round up.
local function item_chrome_width()
    local h = beautiful.menu_height or 16          -- icon column ~ item height
    local arrow = 20                                -- submenu "▶" + its margin
    local pad = 24                                  -- left+right text padding
    return h + arrow + pad
end

-- Measure one string's pixel width in the menu font.
local function text_px(label_text)
    _measure_tb.font = beautiful.menu_font or beautiful.font
    _measure_tb.text = label_text or ""
    -- get_preferred_size(screen) -> (w, h); screen 1 is fine, menu font/DPI
    -- are screen-independent here for our purposes.
    local w = select(1, _measure_tb:get_preferred_size(1))
    return w or 0
end

-- Given a list of label strings, return the menu width to use: the widest label
-- plus per-item chrome, clamped to [MIN, MAX].
local function compute_width(labels)
    local widest = 0
    for _, t in ipairs(labels) do
        local w = text_px(t)
        if w > widest then widest = w end
    end
    local total = math.ceil(widest) + item_chrome_width() + MENU_TEXT_PAD
    if total < MENU_WIDTH_MIN then total = MENU_WIDTH_MIN end
    if total > MENU_WIDTH_MAX then total = MENU_WIDTH_MAX end
    return total
end

-- Resolve an Icon= value to a file path Awesome can render, or nil.
-- A value containing '/' is treated as a literal path; otherwise it is looked
-- up against the configured icon theme. Resolution failure is non-fatal - the
-- menu entry simply renders without an icon.
local function resolve_icon(icon)
    if not icon or icon == "" then
        return nil
    end
    if icon:find("/", 1, true) then
        -- Literal path. Trust it if it exists; gears would also accept it.
        return icon
    end
    -- menubar.utils.lookup_icon returns a path or nil/false.
    local ok, path = pcall(menubar.utils.lookup_icon, icon)
    if ok and path then
        return path
    end
    return nil
end

-- Build the spawn callback for one entry. Terminal=true apps are launched
-- inside the configured terminal emulator; everything else spawns directly.
-- awful.spawn with a table argument avoids shell re-parsing of the (already
-- field-code-stripped) Exec string.
local function make_launch(item)
    local exec = item.exec
    local needs_term = item.terminal

    return function()
        if needs_term then
            -- `terminal` is the global from rc.lua; fall back to xterm.
            local term = _G.terminal or "xterm"
            awful.spawn.with_shell(term .. " -e " .. exec)
        else
            awful.spawn(exec, false)
        end
    end
end

-- Turn the cached document into the array-of-items form awful.menu expects.
-- Each category becomes a submenu: { "Internet", { {entry}, {entry}, ... } }.
-- Side effect: sets _max_submenu_width to the widest width any category needs,
-- which rebuild() applies globally via beautiful.menu_width.
local _max_submenu_width = 0

local function build_app_items()
    local items = {}
    _max_submenu_width = 0
    if not _menu_data or type(_menu_data.categories) ~= "table" then
        return items
    end

    for _, cat in ipairs(_menu_data.categories) do
        if type(cat) == "table" and type(cat.items) == "table"
           and #cat.items > 0 then
            local sub = {}
            local labels = {}
            for _, entry in ipairs(cat.items) do
                if type(entry) == "table" and entry.name and entry.exec then
                    -- awful.menu item form: { label, action, icon_path }
                    sub[#sub + 1] = {
                        entry.name,
                        make_launch(entry),
                        resolve_icon(entry.icon),
                    }
                    labels[#labels + 1] = entry.name
                end
            end
            if #sub > 0 then
                -- Submenu is a RAW ITEMS ARRAY (a pre-built awful.menu object
                -- renders empty on this version). We embed a `theme` table on
                -- the array itself; awful.menu reads the submenu's own theme when
                -- it builds the child menu, letting each category carry its own
                -- width independent of the root. (If this version ignores the
                -- embedded theme, USE_PER_SUBMENU_WIDTH below falls back to a
                -- single unified width.)
                local w = compute_width(labels)
                if w > _max_submenu_width then _max_submenu_width = w end
                sub.theme = { width = w }
                items[#items + 1] = { cat.name, sub }
            end
        end
    end

    return items
end

-- Construct a fresh awful.menu from header items + application categories.
local function rebuild()
    local items = {}

    if _header_fn then
        local header = _header_fn()
        if type(header) == "table" then
            for _, it in ipairs(header) do
                items[#items + 1] = it
            end
        end
    end

    local apps = build_app_items()   -- also computes _max_submenu_width
    for _, it in ipairs(apps) do
        items[#items + 1] = it
    end

    -- Width handling. Two strategies depending on what this awful.menu version
    -- supports (decided by the probe; flip USE_PER_SUBMENU_WIDTH accordingly):
    --
    --   true  -> each category submenu carries its own embedded theme.width
    --            (set in build_app_items). The ROOT then gets its own narrow
    --            width sized to just its top-level labels. This is the ideal:
    --            narrow root, each submenu sized to its own contents.
    --
    --   false -> embedded submenu theme is ignored by this version, so submenus
    --            inherit the root's theme.width. To avoid truncation we must
    --            make the root (and thus all submenus) as wide as the widest
    --            label anywhere. Root ends up wider than its own labels need.
    local USE_PER_SUBMENU_WIDTH = true

    local top_labels = {}
    for _, it in ipairs(items) do
        if type(it) == "table" and type(it[1]) == "string" then
            top_labels[#top_labels + 1] = it[1]
        end
    end
    local root_label_w = compute_width(top_labels)

    local root_width
    if USE_PER_SUBMENU_WIDTH then
        root_width = root_label_w                          -- narrow root
    else
        root_width = math.max(root_label_w, _max_submenu_width)  -- unified
    end

    local menu = awful.menu({
        items = items,
        theme = {
			width		 = root_width,
			border_width = 2,
        	border_color = "#3300cc"
		},
    })

    -- Intercept 'show' to highlight the icon
    local original_show = menu.show
    menu.show = function(self, ...)
        _is_menu_open = true
        update_launcher_icon()
        if original_show then return original_show(self, ...) end
    end

    -- Intercept 'hide' to remove the highlight
    local original_hide = menu.hide
    menu.hide = function(self, ...)
        _is_menu_open = false
        update_launcher_icon()
        if original_hide then return original_hide(self, ...) end
    end

    -- Store the live menu so the launcher can toggle it
    _current_menu = menu

    -- Layer on corner-crossing tolerance + auto-hide. Done AFTER the show/hide
    -- interceptors above so autohide's own show/hide wrappers sit outside them:
    -- call chain becomes autohide -> icon-highlight -> original. Re-applied on
    -- every rebuild because each rebuild produces a brand-new menu object.
    behavior.apply(menu)

    if _on_rebuild then
        _on_rebuild(menu)
    end

    return menu
end

-- Called by stellar_bridge.lua when a MENU_DATA line is decoded. Stores the
-- new document and rebuilds the menu if a binding is installed.
function M.set_menu_data(data)
    _menu_data = data
    if _header_fn or _on_rebuild then
        rebuild()
    end
end

-- Install the header-items provider and the rebuild sink, then build once with
-- whatever data has arrived so far (the menu is usable immediately even before
-- the first MENU_DATA push - it just shows only the header items until then).
--
--   header_fn   : function() -> array of static awful.menu items (above apps)
--   on_rebuild  : function(menu) -> receives each freshly built awful.menu
function M.bind_main_menu(header_fn, on_rebuild)
    _header_fn  = header_fn
    _on_rebuild = on_rebuild
    return rebuild()
end

-- Expose the current document (handy for debugging / a future search prompt).
function M.get_data()
    return _menu_data
end

function M.create_launcher()
    _launcher_widget = awful.widget.button({ 
        image = M.stellar_icon() 
    })

    -- Attach hover listeners
    _launcher_widget:connect_signal("mouse::enter", function()
        _is_hovered = true
        update_launcher_icon()
    end)

    _launcher_widget:connect_signal("mouse::leave", function()
        _is_hovered = false
        update_launcher_icon()
    end)

    -- Click handler to toggle the dynamically updating menu
    _launcher_widget:buttons(gears.table.join(
        awful.button({}, 1, function() 
            if _current_menu then
                _current_menu:toggle() 
            end
        end)
    ))

    return _launcher_widget
end

function M.stellar_icon(selected)
	local mode = "normal"
	if selected then
		mode = "selected"
	end
	return stellar_ui.get_image_path("sys", "stellar_logo", mode)
end

return M
