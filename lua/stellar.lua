local json = require("dkjson")

local function starts_with(s, prefix)
  return s:sub(1, #prefix) == prefix
end

function parse_json_file(file_path)
  local file = io.open(file_path, "r")
  if not file then 
    Stellar.log("Failed to open settings file: " .. tostring(file_path))
    return nil 
  end
  
  local content = file:read("*all")
  file:close()
  
  local obj, pos, err = json.decode(content, 1, nil)
  if err then 
    Stellar.log("Error parsing settings JSON: " .. err)
    return nil 
  end
  
  return obj
end

function init()
  Stellar.log("stellar.lua init")
  Stellar.log("screen_count=" .. tostring(Stellar.screen_count()))
end

function on_pointer_screen_change(old_screen, new_screen)
  Stellar.log(
    "pointer moved from screen " .. tostring(old_screen) ..
      " to " .. tostring(new_screen)
  )
end

function handle_ipc_line(client_fd, line)
  Stellar.log("ipc from fd=" .. tostring(client_fd) .. ": " .. line)

  if starts_with(line, "REQUEST_FOCUS_TAG ") then
    -- Example command shape:
    -- REQUEST_FOCUS_TAG screen=1 tag=3
    local screen = line:match("screen=(%d+)")
    local tag = line:match("tag=([^%s]+)")
    if screen and tag then
      Stellar.send_to_screen(tonumber(screen), "FOCUS_TAG " .. tag)
    end
    return
  end

  if starts_with(line, "RELOCATE_CONFLUX ") then
    local payload = line:sub(18)
    local obj, pos, err = json.decode(payload, 1, nil)
    
    if obj and obj.target_screen then
      Stellar.log("Routing workspace relocation to screen: " .. tostring(obj.target_screen))
      Stellar.send_to_screen(tonumber(obj.target_screen), "CONFLUX_RESTORE " .. payload)
    end
    return
  end

  if starts_with(line, "RELOCATE_ACK ") then
    local payload = line:sub(14)
    local obj, pos, err = json.decode(payload, 1, nil)
    
    if obj and obj.target_screen then
      Stellar.log("Routing ACK back to source screen: " .. tostring(obj.target_screen))
      Stellar.send_to_screen(tonumber(obj.target_screen), "RELOCATE_ACK_RECV " .. payload)
    end
    return
  end
end

local _window_rules = {}

function load_window_rules(settings_path)
    local settings = parse_json_file(settings_path)
    if not settings or not settings.window_rules then
        _window_rules = {}
        return
    end
    _window_rules = settings.window_rules
end

-- C calls this with the class/name from a new window.
-- Returns a table of DE-level properties, or nil if no match.
function match_window_rule(win_class, win_name)
    for _, rule_def in ipairs(_window_rules) do
        local rule = rule_def.rule or {}
        local props = rule_def.properties or {}

        local class_match = not rule.class or rule.class == win_class
        local name_match  = not rule.name  or rule.name == win_name

        if class_match and name_match then
            -- Only return properties the DE cares about.
            -- Awesome-only props (floating, placement, etc.) stay out.
            local result = {}

            -- Window type override
            if props.window_type == "fullscreen_desktop" then
                result.window_type = "desktop"
				result.fullscreen_desktop = true
			elseif props.window_type then
                result.window_type = props.window_type
            end

			if props.titlebars_enabled  then
				result.titlebars_enabled = props.titlebars_enabled 
			end

            -- Compositor overrides for picom rules
            if props.compositor then
                result.compositor = props.compositor
            end

            if next(result) then
                return result
            end
        end
    end
    return nil
end

-- Deep merge: user values override defaults
local function deep_merge(defaults, overrides)
    local result = {}
    for k, v in pairs(defaults) do
        result[k] = v
    end
    for k, v in pairs(overrides) do
        if type(v) == "table" and type(result[k]) == "table"
           and not v[1] then  -- don't merge arrays, replace them
            result[k] = deep_merge(result[k], v)
        else
            result[k] = v
        end
    end
    return result
end

local function format_value(v)
    if type(v) == "boolean" then
        return tostring(v)
    elseif type(v) == "number" then
        -- picom wants "5.0" not "5" for some float fields,
        -- but integers are fine as integers
        if v == math.floor(v) then return tostring(v)
        else return string.format("%.6g", v) end
    elseif type(v) == "string" then
        return '"' .. v .. '"'
    end
    return tostring(v)
end

local function resolve_match(m)
    if type(m) == "table" then
        return table.concat(m, " || ")
    end
    return m
end

local function emit_rules(rules)
    local lines = {}
    table.insert(lines, "rules = (")
    for i, rule in ipairs(rules) do
        local parts = {}
        -- match first
        if rule.match then
            table.insert(parts, string.format('  match = "%s"', resolve_match(rule.match)))
        end
        for k, v in pairs(rule) do
            if k ~= "match" then
                table.insert(parts, string.format("  %s = %s", k, format_value(v)))
            end
        end
        local sep = (i < #rules) and "}," or "}"
        table.insert(lines, "{\n" .. table.concat(parts, ";\n") .. ";\n" .. sep)
    end
    table.insert(lines, ");")
    return table.concat(lines, "\n")
end

local function emit_section(key, tbl)
    -- For nested objects like "blur"
    local lines = {}
    table.insert(lines, key .. " = {")
    for k, v in pairs(tbl) do
        table.insert(lines, string.format("  %s = %s;", k, format_value(v)))
    end
    table.insert(lines, "};")
    return table.concat(lines, "\n")
end

local _cached_merged = nil

function load_picom_settings(defaults_path, settings_path)
    local defaults = parse_json_file(defaults_path)
    local settings = parse_json_file(settings_path)
    _cached_merged = deep_merge(defaults or {}, settings.compositor or {})
end

function generate_picom_config(theme_data_path, ui_scale)
    if not _cached_merged then return nil end

	local theme_data = dofile(theme_data_path .. "/theme_data.lua")
	
    -- Copy so we don't mutate the cached version
    local merged = deep_merge(_cached_merged, {})

	-- Apply DPI scaling
	local scaled_keys = { "shadow-radius", "shadow-offset-x", "shadow-offset-y" }
	for _, key in ipairs(scaled_keys) do
		if merged[key] then
			merged[key] = math.floor(merged[key] * ui_scale + 0.5)
		end
	end

    -- Apply theme_data values
    if merged["shadow-color"] == "$shadow_color_focused" then
    	merged["shadow-color"] = theme_data.compositor.focused_shadow_color
    end
    if merged.rules then
        for _, rule in ipairs(merged.rules) do
            if rule["shadow-color"] == "$shadow_color_unfocused" then
                rule["shadow-color"] = theme_data.compositor.unfocused_shadow_color
            end
        end
    end

    local lines = {}
    table.insert(lines, "# Automatically generated by Stellar")
    table.insert(lines, "# Do not edit - changes will be lost")
    table.insert(lines, string.format("# UI scale: %.4f", ui_scale))
    table.insert(lines, "")

    for k, v in pairs(merged) do
        if k ~= "rules" and type(v) ~= "table" then
            table.insert(lines, string.format("%s = %s;", k, format_value(v)))
        end
    end

    table.insert(lines, "")

    for k, v in pairs(merged) do
        if type(v) == "table" and k ~= "rules" then
            table.insert(lines, emit_section(k, v))
            table.insert(lines, "")
        end
    end

    if merged.rules then
        table.insert(lines, emit_rules(merged.rules))
    end

    return table.concat(lines, "\n")
end
