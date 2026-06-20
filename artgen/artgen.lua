-- artgen.lua

local input_image = "STEL_UI.GIF"
local palette_file = "master_palette.gif"
local output_dir = arg[1] or "./output"

local function fail(msg)
    io.stderr:write("Artgen error: " .. msg .. "\n")
    error(msg, 0)
end

-- Slice table configuration.
--
-- Each entry describes a group of sprites in the source image.
--   category   - namespace prefix for output files and slice_data keys
--   name       - base name of the sprite group
--   w, h       - base dimensions (before size scaling)
--   scan       - direction to advance between scan_modes ("right" or "down")
--   next       - direction to advance after finishing all scan_modes in a
--                variation (or the whole entry if no variations):
--                  "right"  - snap X to max_size_x, reset Y to size_start_y
--                  "down"   - keep X, drop Y below this group
--                  "return" - carriage return: X=1, Y below everything so far
--                  "done"   - last entry, stop
--   scan_modes - list of mode suffixes; each gets its own output image
--   variations - (optional) list of variation prefixes; each variation
--                traverses all scan_modes and then advances per `next`,
--                exactly as if it were a separate slice entry.
--                Output filename: cat_name_variation_mode.png
--                Slice data records only the base name (dimensions are the
--                same across all variations).

local slices = {
	-- Windows
    { category = "win", name = "upper_left",		w = 20, h = 16, scan = "down",	next = "right", scan_modes = { "active", "focused",	"unfocused" } },
    { category = "win", name = "title_trans",		w = 24, h = 16, scan = "down",	next = "right", scan_modes = { "active", "focused",	"unfocused" } },
    { category = "win", name = "button_trans",		w = 8,	h = 16, scan = "down",	next = "right", scan_modes = { "active", "focused",	"unfocused" } },
    { category = "win", name = "upper_right",		w = 20,	h = 16, scan = "down",	next = "right", scan_modes = { "active", "focused",	"unfocused" } },
    { category = "win", name = "left_upper",		w = 4,	h = 4,	scan = "right",	next = "down",	scan_modes = { "active", "focused",	"unfocused" } },
    { category = "win", name = "right_upper",		w = 4,	h = 4,	scan = "right",	next = "down",	scan_modes = { "active", "focused",	"unfocused" } },
	{ category = "win", name = "left_lower",		w = 4,	h = 16,	scan = "right",	next = "down",	scan_modes = { "active", "focused",	"unfocused" } },
    { category = "win", name = "right_lower",		w = 4,	h = 16,	scan = "right",	next = "right", scan_modes = { "active", "focused",	"unfocused" } },
	{ category = "win", name = "lower_left",		w = 20,	h = 4,	scan = "down",	next = "down",	scan_modes = { "active", "focused",	"unfocused" } },
    { category = "win", name = "lower_right",		w = 20,	h = 4,	scan = "down",	next = "right",	scan_modes = { "active", "focused",	"unfocused" } },
    { category = "win", name = "title_flex",		w = 1,	h = 16, scan = "right", next = "right", scan_modes = { "focused", "unfocused" } },
    { category = "win", name = "bar_flex",			w = 1,	h = 16, scan = "right", next = "right", scan_modes = { "active", "focused", "unfocused" } },
    { category = "win", name = "left_flex",			w = 4,	h = 1,	scan = "down",	next = "down",	scan_modes = { "focused", "unfocused" } },
    { category = "win", name = "right_flex",		w = 4,	h = 1,	scan = "down",	next = "right", scan_modes = { "focused", "unfocused" } },
    { category = "win", name = "lower_flex",		w = 1,	h = 4,	scan = "right",	next = "right",	scan_modes = { "focused", "unfocused" } },

	-- Tabs
    { category = "tab", name = "lower_left",
		variations = { "normal", "selected" },
		w = 20, h = 16, scan = "down",	next = "right", scan_modes = { "active", "focused", "unfocused" } },

    { category = "tab", name = "join_left",
		variations = { "normal_normal", "selected_normal", "selected_selected", "normal_selected" },
		w = 16, h = 16, scan = "down",	next = "right", scan_modes = { "focused", "unfocused" } },

    { category = "tab", name = "join_right",
		variations = { "normal_normal", "selected_normal", "selected_selected", "normal_selected" },
		w = 8, h = 16, scan = "down", next = "right", scan_modes = { "focused", "unfocused" } },

    { category = "tab", name = "join_new",
		variations = { "normal_normal", "selected_normal", "selected_selected", "normal_selected" },
		w = 24, h = 16, scan = "down", next = "right", scan_modes = { "focused", "unfocused" } },

    { category = "tab", name = "join_end",
		variations = { "normal", "selected" },
		w = 8, h = 16, scan = "down", next = "right", scan_modes = { "focused", "unfocused" } },

    { category = "tab", name = "lower_right",		w = 20,	h = 16, scan = "down",	next = "right", scan_modes = { "active", "focused", "unfocused" } },

    { category = "tab", name = "title_flex",
		variations = { "normal", "selected" },
		w = 1, h = 16, scan = "right", next = "right", scan_modes = { "focused", "unfocused" } },

    { category = "tab", name = "lower_flex",		w = 1,	h = 16, scan = "right", next = "right", scan_modes = { "focused", "unfocused" } },
    { category = "tab", name = "left_lower",		w = 4,	h = 4,	scan = "right",	next = "down",	scan_modes = { "active", "focused", "unfocused" } },
    { category = "tab", name = "right_lower",		w = 4,	h = 4,	scan = "right",	next = "return",scan_modes = { "active", "focused", "unfocused" } },

	-- Window buttons (6 types x 2 focus states, laid out as 6 columns of 2)
    { category = "win", name = "button",
		variations = { "tiled", "floating", "maximized", "ontop", "sticky", "attention" },
		w = 28, h = 16, scan = "down", next = "right", scan_modes = { "focused", "unfocused" } },

	-- Window icons
    { category = "win", name = "icon_terminate",	w = 24,	h = 24,	scan = "down",	next = "right",	scan_modes = { "none" } },
    { category = "win", name = "icon_floating",		w = 24,	h = 24,	scan = "down",	next = "right",	scan_modes = { "active", "inactive" } },
    { category = "win", name = "icon_maximized",	w = 24,	h = 24,	scan = "down",	next = "right",	scan_modes = { "active", "inactive" } },
    { category = "win", name = "icon_ontop",		w = 24,	h = 24,	scan = "down",	next = "right",	scan_modes = { "active", "inactive" } },
    { category = "win", name = "icon_sticky",		w = 24,	h = 24,	scan = "down",	next = "return",scan_modes = { "active", "inactive" } },

	-- Wibar / Tasklist
    { category = "sys", 	name = "stellar_logo",	w = 24,	h = 24,	scan = "right",	next = "right",	scan_modes = { "normal", "selected" } },
    { category = "conflux", name = "start",			w = 32,	h = 24,	scan = "right",	next = "right",	scan_modes = { "normal", "selected" } },

    { category = "conflux", name = "join_left",		w = 24,	h = 24,	scan = "right",	next = "return",scan_modes = { "normal_none",
																												   "selected_none",
																												   "normal_normal",
																												   "selected_normal",
																												   "selected_selected",
																												   "normal_selected" } },

    { category = "task", name = "start",			w = 12,	h = 24,	scan = "right",	next = "right",	scan_modes = { "normal", "selected" } },

    { category = "task", name = "join_left",		w = 24,	h = 24,	scan = "right",	next = "right",	scan_modes = { "normal_normal", "selected_normal",
					    																					  "selected_selected", "normal_selected" } },

    { category = "task", name = "join_right",		w = 12,	h = 24,	scan = "right",	next = "right",	scan_modes = { "normal_normal", "selected_normal",
					    																					  "selected_selected", "normal_selected" } },

	{ category = "task", name = "join_end",				w = 12,	h = 24,	scan = "right",	next = "right",	scan_modes = { "normal", "selected" } },
    { category = "task", name = "title_flex",		w = 1,	h = 24,	scan = "right",	next = "done",	scan_modes = { "normal", "selected" } },
}

local sizes = {
--	{ name = "mimimal",		scale = 0.5 },
--	{ name = "compact",		scale = 0.75 },
	{ name = "standard",	scale = 1.0 },
	{ name = "comfortable",	scale = 1.25 },
--	{ name = "large",		scale = 1.5 },
--	{ name = "expanded",	scale = 1.75 },
--	{ name = "maximum",		scale = 2.0 },
}

local themes = {
    {
        name                    = "stellar-blue",
        pretty_name             = "Stellar Blue",
        -- Picom shadow colors
        compositor = {
            focused_shadow_color    = "#0055FF",
            unfocused_shadow_color  = "#0000FF",
        },
        -- Nuklear UI colors
        nk_colors = {
            window          = "#000000",
            text            = "#FFFFFF",
            button          = "#000000",
            button_hover    = "#000050",
            button_active   = "#000096",
            border          = "#0000FF",
        }
    }
}

print("Extracting master palette to prevent index scrambling...")
local extract_cmd = string.format("magick %s -unique-colors %s", input_image, palette_file)
os.execute(extract_cmd)

print("Slicing images...")

-- Global tracker for the absolute top edge of where we are in the image.
-- Starts at 1 to account for the top-left 1px red grid border.
local global_y = 1 

for _, theme in ipairs(themes) do
	local theme_dir = string.format("%s/%s", output_dir, theme.name)
	os.execute(string.format("mkdir -p %s", theme_dir))
	local def_base_filename = string.format("%s/%s", theme_dir, "theme_data.lua")
	local base_file, err = io.open(def_base_filename, "w") 
	if not base_file then
		fail("artgen failed to open file for write - error: " .. tostring(err))
	end
	print("base filename = " .. def_base_filename)

	base_file:write("-- Stellar theme base data\n")
	base_file:write(string.format("-- Name: %s\n", theme.name))
	base_file:write("return {\n")
	base_file:write(string.format('\ttheme_name = "%s",\n', theme.pretty_name))

	-- Picom sub-table
	base_file:write("\tcompositor = {\n")
	for k, v in pairs(theme.compositor) do
		base_file:write(string.format('\t\t%s = "%s",\n', k, v))
	end
	base_file:write("\t},\n")

	-- Nuklear colors sub-table
	base_file:write("\tnk_colors = {\n")
	for k, v in pairs(theme.nk_colors) do
		base_file:write(string.format('\t\t%s = "%s",\n', k, v))
	end
	base_file:write("\t},\n")

	base_file:write("}\n")
	base_file:close()

	for _, size in ipairs(sizes) do
		local size_dir = string.format("%s/%s", theme_dir, size.name)
		os.execute(string.format("mkdir -p %s", size_dir))
		local def_slice_filename = string.format("%s/%s", size_dir, "slice_data.lua")
		local slice_file, err = io.open(def_slice_filename, "w") 
		if not slice_file then
			fail("artgen failed to open file for write - error: " .. tostring(err))
		end
		print("slice filename = " .. def_slice_filename)
		
		slice_file:write("-- Stellar theme slice data\n")
		slice_file:write(string.format("-- Name: %s\n", theme.name))
		slice_file:write(string.format("-- Size: %s\n", size.name))

		-- Track the absolute top of this entire size block
		local size_start_y = global_y

		local current_x = 1
		local current_y = size_start_y

		local output_data = {}
		
		-- Track the absolute boundaries for the entire size block
		local max_size_x = 1
		local max_size_y = size_start_y

		for _, slice in ipairs(slices) do
			local target_w, target_h
			if slice.w == 1 then
				target_w = 1
				target_h = math.floor(slice.h * size.scale)
			elseif slice.h == 1 then
				target_w = math.floor(slice.w * size.scale)
				target_h = 1
			else
				target_w = math.floor(slice.w * size.scale)
				target_h = math.floor(slice.h * size.scale)
			end

			-- Dynamically create the category namespace
			local cat = slice.category or "uncategorized"
			output_data[cat] = output_data[cat] or {}

			-- Store the clean data directly (once per base name, shared across variations)
			output_data[cat][slice.name] = { w = target_w, h = target_h }

			-- If no variations defined, use a single nil entry (no variation prefix)
			local variations = slice.variations or { false }

			for _, variation in ipairs(variations) do
				local var_start_x = current_x
				local var_start_y = current_y

				local max_var_next_x = var_start_x
				local max_var_next_y = var_start_y

				for _, mode in ipairs(slice.scan_modes) do
					-- Build the output filename: cat_name[_variation][_mode].png
					local name_parts = { cat, slice.name }
					if variation then name_parts[#name_parts + 1] = variation end
					if mode ~= "none" then name_parts[#name_parts + 1] = mode end
					local output_file = string.format("%s/%s.png", size_dir, table.concat(name_parts, "_"))

					local cmd = string.format(
						"magick %s -crop %dx%d+%d+%d +repage +dither -remap %s PNG8:%s",
						input_image, target_w, target_h, current_x, current_y, palette_file, output_file
					)
					local cmd = string.format(
						"magick %s -crop %dx%d+%d+%d +repage +dither -remap %s -define png:exclude-chunk=date,time PNG8:%s",
						input_image, target_w, target_h, current_x, current_y, palette_file, output_file
					)

					print(string.format(" -> Writing %s", output_file))
					os.execute(cmd)

					local next_x = current_x + target_w + 1
					local next_y = current_y + target_h + 1

					if next_x > max_var_next_x then max_var_next_x = next_x end
					if next_y > max_var_next_y then max_var_next_y = next_y end

					if slice.scan == "right" then
						current_x = next_x
					elseif slice.scan == "down" then
						current_y = next_y
					end
				end

				-- Update the master boundaries for the whole size block
				if max_var_next_x > max_size_x then max_size_x = max_var_next_x end
				if max_var_next_y > max_size_y then max_size_y = max_var_next_y end

				-- Advance cursors for the next variation (or next slice if this is the last one).
				-- Each variation traverses the image exactly like a separate slice entry would.
				if slice.next == "right" then
					current_x = max_size_x
					current_y = size_start_y
				elseif slice.next == "down" then
					current_x = var_start_x
					current_y = max_var_next_y
				elseif slice.next == "return" then
					current_x = 1
					current_y = max_size_y
					size_start_y = max_size_y
					max_size_x = 1
				end
			end
		end
		
		-- All slices for this size scale are done. 
		global_y = max_size_y

		-- Serialize the nested table to the file
		slice_file:write("return {\n")
		for category_name, category_data in pairs(output_data) do
			slice_file:write(string.format("\t%s = {\n", category_name))
			
			for key, dims in pairs(category_data) do
				slice_file:write(string.format("\t\t%s = { w = %d, h = %d },\n", key, dims.w, dims.h))
			end
			
			slice_file:write("\t},\n")
		end

		slice_file:write("}\n")
		slice_file:close()	
	end
end

os.remove(palette_file)
print("Done!")
