---------------------------------------------------------------------------
--- stellar_wallpaper - per-screen wallpaper application for the Stellar DE.
--
-- Wallpaper is settings-driven (settings.json appearance + per-screen
-- overrides) and not part of the theme identity, so it lives in its own
-- module rather than in stellar_theme or cluttering the bridge.
--
-- The actual settings table is read from stellar_api.stellar_settings at
-- apply time (not cached), so a live SETTINGS_RELOADED that swaps the table
-- is picked up automatically on the next request::wallpaper.
--
-- @usage
--     local stellar_wallpaper = require("modules.stellar_wallpaper")
--     stellar_wallpaper.init{ screen = stellar_screen, log = stellar_api.log }
--
--     screen.connect_signal("request::wallpaper", function(s)
--         stellar_wallpaper.apply(s)
--     end)
---------------------------------------------------------------------------

local gears     = require("gears")
local wibox     = require("wibox")
local awful     = require("awful")

local stellar_wallpaper = {}

-- Module-local state set by init().
local S = {
    screen = -1,
    log    = function() end,
}

---------------------------------------------------------------------------
--  Init
---------------------------------------------------------------------------

-- @tparam table args
--   * screen (number)   This screen index (for per-screen overrides).
--   * log    (function) Logger, e.g. stellar_api.log. Optional.
function stellar_wallpaper.init(args)
    args = args or {}
    if args.log then S.log = args.log end
    S.screen = args.screen
        or tonumber(os.getenv("STELLAR_SCREEN") or "-1")
end

---------------------------------------------------------------------------
--  Resolve the effective path + mode for this screen
---------------------------------------------------------------------------

-- Returns target_path, target_mode (or nil if nothing is configured).
local function resolve(settings)
    if not settings or not settings.appearance then
        S.log("apply_wallpaper: Missing settings or appearance block.")
        return nil
    end

    -- 1. Global fallback defaults.
    local target_path = settings.appearance.wallpaper_path
    local target_mode = settings.appearance.wallpaper_mode or "cropped"

    -- 2. Per-screen overrides.
    local scr_key      = tostring(S.screen)
    local scr_settings = settings.screens and settings.screens[scr_key]

    if scr_settings and scr_settings.appearance
        and scr_settings.appearance.override_wallpaper then
        local scr_app = scr_settings.appearance

        if type(scr_app.wallpaper_path) == "string" and scr_app.wallpaper_path ~= "" then
            target_path = scr_app.wallpaper_path
        end

        if type(scr_app.wallpaper_mode) == "string" and scr_app.wallpaper_mode ~= "" then
            target_mode = scr_app.wallpaper_mode
        end
    end

    -- 3. Abort if no valid path in either location.
    if not target_path or target_path == "" then
        S.log("apply_wallpaper: No wallpaper path defined for screen " .. scr_key)
        return nil
    end

    return target_path, target_mode
end

---------------------------------------------------------------------------
--  Apply
---------------------------------------------------------------------------

-- Apply the configured wallpaper to screen `s`. Reads the current settings
-- from stellar_api.stellar_settings so live reloads are honoured.
function stellar_wallpaper.apply(s)
    local settings = stellar_api.stellar_settings
    local target_path, target_mode = resolve(settings)
    if not target_path then return end

    S.log(string.format(
        "apply_wallpaper: Applying %s (mode: %s) to stellar_screen=%s, s.index=%s",
        target_path, target_mode, tostring(S.screen), tostring(s.index)))

    if target_mode == "cropped" then
        awful.wallpaper {
            screen = s,
            widget = {
                image  = gears.surface.crop_surface {
                    surface = gears.surface.load_uncached(target_path),
                    ratio   = s.geometry.width / s.geometry.height,
                },
                widget = wibox.widget.imagebox,
            },
        }
    elseif target_mode == "centered" then
        awful.wallpaper {
            screen = s,
            widget = {
                {
                    image  = target_path,
                    widget = wibox.widget.imagebox,
                },
                valign = "center",
                halign = "center",
                tiled  = false,
                widget = wibox.container.tile,
            }
        }
    elseif target_mode == "tiled" then
        awful.wallpaper {
            screen = s,
            widget = {
                {
                    image  = target_path,
                    widget = wibox.widget.imagebox,
                },
                valign = "center",
                halign = "center",
                tiled  = true,
                widget = wibox.container.tile,
            }
        }
    else
        -- "scaled" or "maximized": standard fallback stretching the image.
        awful.wallpaper {
            screen = s,
            widget = {
                image     = target_path,
                upscale   = true,
                downscale = true,
                widget    = wibox.widget.imagebox,
            }
        }
    end
end

return stellar_wallpaper

-- vim: filetype=lua:expandtab:shiftwidth=4:tabstop=8:softtabstop=4:textwidth=80
