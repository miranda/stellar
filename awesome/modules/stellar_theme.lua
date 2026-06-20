---------------------------------------------------------------------------
--- stellar_theme - the single theme authority for the Stellar DE.
--
-- This module is the PRODUCER of the theme. Everything that wants a colour,
-- a font, the DPI-resolved sprite directory, or the Nuklear palette asks
-- this module rather than reaching into `beautiful` ad hoc.
--
-- Responsibilities:
--   1. Resolve the DPI-appropriate sprite asset directory (with directional
--      fallback) and expose it as `theme_assets_path`.
--   2. Configure beautiful's DPI before any widget is built.
--   3. Load `theme_data.lua` (ships with the theme: nk_colors, compositor).
--   4. Assemble a fully-populated `beautiful` table BEFORE the base config
--      (rc.lua) runs, so every wibar/titlebar picks up correct values first
--      time - no "init then patch" dance.
--   5. Apply the user's appearance font from settings.json, with the
--      documented precedence: theme_data defaults < settings.json overrides.
--   6. Provide `theme_info()` so the IPC GET_THEME_DATA handler can hand the
--      C apps a single, already-assembled view instead of re-harvesting
--      beautiful itself.
--
-- Load order (driven by stellar_bridge):
--     local stellar_theme = require("modules.stellar_theme")
--     stellar_theme.init{ settings = settings, log = stellar_api.log }
--     -- ... only now load_base_config(rc.lua) ...
--
-- @usage
--     local stellar_theme = require("modules.stellar_theme")
--     stellar_theme.init{ settings = settings, log = stellar_api.log }
--     local path = stellar_theme.assets_path()        -- DPI-resolved sprites
--     stellar_theme.apply_font(new_settings)           -- on live reload
--     local info = stellar_theme.theme_info()          -- for IPC response
---------------------------------------------------------------------------

local beautiful  = require("beautiful")
local gears      = require("gears")
local xresources = require("beautiful.xresources")

local stellar_theme = {}

---------------------------------------------------------------------------
--  Internal state
---------------------------------------------------------------------------

-- Populated by init(). Kept module-local so callers go through the API
-- rather than poking at internals.
local S = {
    initialized   = false,
    log           = function() end,  -- replaced in init()
    share_path    = nil,
    theme_name    = nil,             -- directory name, e.g. "stellar-blue"
    theme_dir     = nil,             -- absolute path to the theme dir
    assets_path   = nil,             -- DPI-resolved sprite subdir
    dpi           = 96,
    screen        = -1,
    base_data     = {},              -- contents of theme_data.lua
}

---------------------------------------------------------------------------
--  DPI-resolved asset directory
---------------------------------------------------------------------------

-- Pixel-size buckets shipped by each theme. The key is the DPI the bucket
-- targets; the value is the on-disk subdirectory name.
local THEME_SIZES = {
    [48]  = "minimal",
    [72]  = "compact",
    [96]  = "standard",
    [120] = "comfortable",
    [144] = "large",
    [168] = "expanded",
    [192] = "maximum",
}

-- Strictly ordered list for directional fallback math.
local SORTED_DPIS = { 48, 72, 96, 120, 144, 168, 192 }

-- Given a theme's base sprite dir and a target DPI, find the best existing
-- size bucket: prefer the largest bucket <= target, stepping DOWN if missing,
-- then UP, then a hard standard fallback.
local function resolve_assets_path(base_dir, target_dpi)
    target_dpi = target_dpi or 96

    local ideal_index = 1
    for i, dpi_val in ipairs(SORTED_DPIS) do
        if target_dpi >= dpi_val then
            ideal_index = i
        else
            break
        end
    end

    local function check_path(index)
        local dpi_val   = SORTED_DPIS[index]
        local size_name = THEME_SIZES[dpi_val]
        local path      = base_dir .. "/" .. size_name
        if gears.filesystem.dir_readable(path) then
            return path
        end
        return nil
    end

    -- Search downwards from ideal.
    for i = ideal_index, 1, -1 do
        local p = check_path(i)
        if p then return p end
    end

    -- Then upwards.
    for i = ideal_index + 1, #SORTED_DPIS do
        local p = check_path(i)
        if p then return p end
    end

    -- Failsafe.
    return base_dir .. "/" .. THEME_SIZES[96]
end

---------------------------------------------------------------------------
--  theme_data.lua loader (ships with the theme: nk_colors, compositor)
---------------------------------------------------------------------------

local function load_base_data(theme_dir)
    local data_path = theme_dir .. "/theme_data.lua"
    local chunk, err = loadfile(data_path)
    if not chunk then
        S.log("stellar_theme: could not load theme data: " .. tostring(err))
        return {}
    end
    local ok, data = pcall(chunk)
    if not ok or type(data) ~= "table" then
        S.log("stellar_theme: theme_data.lua did not return a table: " .. tostring(data))
        return {}
    end
    return data
end

---------------------------------------------------------------------------
--  Font (settings.json appearance -> beautiful.font)
---------------------------------------------------------------------------

-- Resolve the effective appearance block, honouring a per-screen override.
local function effective_appearance(settings)
    if not settings then return nil end
    local app     = settings.appearance
    local screens = settings.screens
    local scr = screens and (screens[tostring(S.screen)] or screens[S.screen])

    if scr and scr.appearance and scr.appearance.override_global
        and scr.appearance.font_name and scr.appearance.font_name ~= "" then
        app = scr.appearance
    end
    return app
end

-- Apply the configured appearance font to beautiful.font.
-- The size uses Pango's absolute "px" suffix when font_unit == "px":
-- DPI-independent, so bitmap fonts (converted .otb) land exactly on their
-- native pixel grid no matter what Xft.dpi / beautiful DPI says. Otherwise
-- the unit is omitted and X11 Xft.dpi scaling applies (good for vectors).
function stellar_theme.apply_font(settings)
    local app = effective_appearance(settings)

    if not app or type(app.font_name) ~= "string" or app.font_name == "" then
        return
    end

    -- Legacy configs may still hold a file path; Pango needs a family name,
    -- so skip those (the DE resolves paths for the C apps).
    if app.font_name:sub(1, 1) == "/" then
        S.log("stellar_theme: font_name is a path, not applying to beautiful.font: "
            .. app.font_name)
        return
    end

    local size = tonumber(app.font_size) or 14
    if size <= 0 then size = 14 end

    local unit = ""
    if app.font_unit == "px" then
        unit = "px"
    end

    beautiful.font = string.format("%s %g%s", app.font_name, size, unit)
    S.log("stellar_theme: applied appearance font: " .. beautiful.font)
end

---------------------------------------------------------------------------
--  Beautiful table assembly
---------------------------------------------------------------------------

-- Build the core beautiful values. This absorbs the live parts of the old
-- theme.lua. Anything the slice-based decoration system supplies its own
-- art for (titlebar button PNGs, etc.) is intentionally omitted - those were
-- dead under Stellar's renderer.
--
-- Precedence: hardcoded Stellar defaults provide the base; theme_data.lua
-- (compositor/nk_colors) is layered for the C apps; settings.json font wins
-- last via apply_font().
local function build_beautiful_table()
    local dpi = xresources.apply_dpi

    local theme = {}

    -- Base palette (Stellar Blue identity defaults).
    theme.font        = "sans 8"

    theme.bg_normal   = "#000000"
    theme.bg_focus    = "#111133"
    theme.bg_urgent   = "#ff0000"
    theme.bg_minimize = "#444444"
    theme.bg_systray  = "#000000"

    theme.fg_normal   = "#aaaaaa"
    theme.fg_focus    = "#ffffff"
    theme.fg_urgent   = "#ffffff"
    theme.fg_minimize = "#ffffff"

    -- Stellar draws its own borders via the slice system; real borders off.
    theme.useless_gap  = dpi(0)
    theme.border_width = 0

    -- Grab outline (used by stellar_window's outline wiboxes).
    theme.grab_border_width = dpi(1)
    theme.grab_border_color = "#ccccff"

    -- Snap overlay.
    theme.snap_bg           = "#9900ff"
    theme.snap_border_width = dpi(2)
    theme.snap_shape = function(cr, w, h)
        local lw = theme.snap_border_width
        cr:translate(lw, lw)
        cr:rectangle(0, 0, w - 2 * lw, h - 2 * lw)
    end

    -- Border colours still referenced by GET_THEME_DATA harvesting.
    theme.border_color_normal = "#000088"
    theme.border_color_active = "#7799ff"
    theme.border_color_marked = "#910000"

    -- Menu chrome (used by stellar_menu / the awesome submenu).
    theme.menu_height = dpi(15)
    theme.menu_width  = dpi(100)

    -- Layout icons: TEMPORARY. These point at awesome's stock "default" theme
    -- PNGs so the layoutbox renders icons instead of falling back to the bare
    -- layout name ("floating", "tile", ...). Replace with themed PNGs from the
    -- Stellar asset directory once they're authored, then delete this block's
    -- "stock" sourcing.
    local stock_layouts = gears.filesystem.get_themes_dir() .. "default/layouts/"
    theme.layout_fairh      = stock_layouts .. "fairhw.png"
    theme.layout_fairv      = stock_layouts .. "fairvw.png"
    theme.layout_floating   = stock_layouts .. "floatingw.png"
    theme.layout_magnifier  = stock_layouts .. "magnifierw.png"
    theme.layout_max        = stock_layouts .. "maxw.png"
    theme.layout_fullscreen = stock_layouts .. "fullscreenw.png"
    theme.layout_tilebottom = stock_layouts .. "tilebottomw.png"
    theme.layout_tileleft   = stock_layouts .. "tileleftw.png"
    theme.layout_tile       = stock_layouts .. "tilew.png"
    theme.layout_tiletop    = stock_layouts .. "tiletopw.png"
    theme.layout_spiral     = stock_layouts .. "spiralw.png"
    theme.layout_dwindle    = stock_layouts .. "dwindlew.png"
    theme.layout_cornernw   = stock_layouts .. "cornernww.png"
    theme.layout_cornerne   = stock_layouts .. "cornernew.png"
    theme.layout_cornersw   = stock_layouts .. "cornersww.png"
    theme.layout_cornerse   = stock_layouts .. "cornersew.png"

    -- Layer compositor data from theme_data.lua if present, so anything that
    -- checks beautiful for it has a consistent view.
    if S.base_data.compositor then
        theme.stellar_compositor = S.base_data.compositor
    end

    return theme
end

---------------------------------------------------------------------------
--  Public: init
---------------------------------------------------------------------------

-- @tparam table args
--   * settings (table)    Parsed settings.json (for the appearance font).
--   * log      (function) Logger, e.g. stellar_api.log. Optional.
--   * screen   (number)   This screen index. Optional (defaults to env).
function stellar_theme.init(args)
    args = args or {}
    if args.log then S.log = args.log end

    S.share_path = os.getenv("STELLAR_SHARE_PATH") or "/usr/local/share/stellar"
    S.theme_name = os.getenv("STELLAR_THEME") or "stellar-blue"
    S.theme_dir  = S.share_path .. "/themes/" .. S.theme_name
    S.screen     = args.screen
        or tonumber(os.getenv("STELLAR_SCREEN") or "-1")
    S.dpi        = tonumber(os.getenv("STELLAR_DPI")) or 96

    -- 1. Configure beautiful DPI before any widget exists.
    local stellar_dpi = tonumber(os.getenv("STELLAR_DPI"))
    if stellar_dpi and stellar_dpi > 0 then
        local awful_screen = require("awful.screen")
        xresources.set_dpi(stellar_dpi)
        awful_screen.set_auto_dpi_enabled(false)

        screen.connect_signal("request::desktop_decoration", function(s)
            s.dpi = stellar_dpi
        end)

        S.log("stellar_theme: apply_dpi multiplier="
            .. tostring(xresources.apply_dpi(100) * 0.01))
    end

    -- 2. Resolve the DPI-appropriate sprite directory.
    local base_theme_dir = S.theme_dir
    S.assets_path = resolve_assets_path(base_theme_dir, stellar_dpi)
    S.log("stellar_theme: assets_path=" .. tostring(S.assets_path))

    -- 3. Load theme_data.lua (nk_colors, compositor, theme_name).
    S.base_data = load_base_data(S.theme_dir)
    if S.base_data.theme_name then
        S.log("stellar_theme: loaded theme '" .. tostring(S.base_data.theme_name) .. "'")
    else
        S.log("stellar_theme: theme_data.lua missing or empty for " .. S.theme_dir)
    end

    -- 4. Assemble and install the beautiful table. beautiful.init accepts a
    --    table directly, so we never touch the old on-disk theme.lua path.
    beautiful.init(build_beautiful_table())

    -- 5. Apply the user's appearance font last (overrides the base default).
    stellar_theme.apply_font(args.settings)

    S.initialized = true
end

---------------------------------------------------------------------------
--  Public: accessors
---------------------------------------------------------------------------

-- DPI-resolved sprite directory. The slice modules read this.
function stellar_theme.assets_path()
    return S.assets_path
end

function stellar_theme.theme_dir()
    return S.theme_dir
end

function stellar_theme.base_data()
    return S.base_data
end

---------------------------------------------------------------------------
--  Public: theme_info - the single assembled view for the IPC C apps
---------------------------------------------------------------------------

-- Builds the payload the GET_THEME_DATA handler sends to the Nuklear apps.
-- Colours come from beautiful (already populated by init); the Nuklear
-- palette is merged from theme_data.lua's nk_colors with the "nk_color_"
-- prefix the C clients expect (see stellar_theme.c).
function stellar_theme.theme_info()
    local colors = {
        bg_normal     = beautiful.bg_normal,
        bg_focus      = beautiful.bg_focus,
        bg_urgent     = beautiful.bg_urgent,
        bg_minimize   = beautiful.bg_minimize,
        fg_normal     = beautiful.fg_normal,
        fg_focus      = beautiful.fg_focus,
        fg_urgent     = beautiful.fg_urgent,
        fg_minimize   = beautiful.fg_minimize,
        border_normal = beautiful.border_normal,
        border_focus  = beautiful.border_focus,
        border_marked = beautiful.border_marked,
    }

    if S.base_data.nk_colors then
        for k, v in pairs(S.base_data.nk_colors) do
            colors["nk_color_" .. k] = v
        end
    end

    return {
        assets_path = S.assets_path or "",
        theme_dir   = S.theme_dir,
        theme_name  = S.base_data.theme_name,
        dpi         = S.dpi,
        screen      = S.screen,
        colors      = colors,
        sizes = {
            border_width = beautiful.border_width,
            useless_gap  = beautiful.useless_gap,
        },
        font = beautiful.font,
    }
end

return stellar_theme
