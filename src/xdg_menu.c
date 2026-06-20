// xdg_menu.c
// XDG application-menu builder for Stellar DE. See xdg_menu.h for the API.
//
// Pipeline:
//   1. Collect Type=Application entries from every applications/ directory,
//      with user-overrides-system dedup keyed on the desktop-file ID.
//   2. Filter each entry for menu visibility (NoDisplay / Hidden / OnlyShowIn /
//      NotShowIn / TryExec).
//   3. Assign each visible entry to exactly one top-level menu category using
//      the freedesktop Menu Spec "Main Categories" table (first match wins,
//      falling back to "Other").
//   4. Emit a single JSON document, escaping every string, and cache it.
//
// The categorization table and visibility rules live here in C, next to the
// .desktop parsing, so the Lua side only has to json.decode and walk the tree.

#include "stellar.h"
#include "xdg_menu.h"
#include "ipc_lua.h"		// broadcast_line
#include "xdg_util.h"		// xdg_in_semicolon_list, xdg_command_exists,
							// xdg_strip_field_codes, XDG_DE_VALUE/LIST
#include <dirent.h>

/* ------------------------------------------------------------------ */
/*  Menu category model                                                */
/* ------------------------------------------------------------------ */

// Top-level menu sections, in display order. This is the Stellar projection of
// the Menu Spec's registered Main Categories. "Other" is the catch-all and is
// always last. Order here is the order sections appear in the emitted JSON.
typedef enum {
    CAT_ACCESSORIES = 0,   // Utility
    CAT_DEVELOPMENT,       // Development
    CAT_EDUCATION,         // Education
    CAT_GAMES,             // Game
    CAT_GRAPHICS,          // Graphics
    CAT_INTERNET,          // Network
    CAT_MULTIMEDIA,        // AudioVideo / Audio / Video
    CAT_OFFICE,            // Office
    CAT_SCIENCE,           // Science
    CAT_SETTINGS,          // Settings
    CAT_SYSTEM,            // System
    CAT_OTHER,             // catch-all
    CAT_COUNT
} MenuCategory;

// Human-readable section labels, parallel to MenuCategory.
static const char *const k_category_labels[CAT_COUNT] = {
    "Accessories",
    "Development",
    "Education",
    "Games",
    "Graphics",
    "Internet",
    "Multimedia",
    "Office",
    "Science",
    "Settings",
    "System",
    "Other",
};

// Placement priority per section, used to resolve entries that list several
// Main Categories. The freedesktop spec treats Categories= as an UNORDERED set,
// so we must not let token order decide placement (Steam ships
// "Network;FileTransfer;Game" but is obviously a Game, not Internet). Instead we
// pick the highest-priority section among all matched tokens.
//
// Higher number = stronger/more-specific classifier = wins. The ranking encodes
// real-world specificity: "Game" is almost never incidental, while "Network",
// "System", and "Utility" attach to many apps that are primarily something else.
// CAT_OTHER is 0 so any real match beats the catch-all.
static const int k_category_priority[CAT_COUNT] = {
    [CAT_ACCESSORIES] = 1,   // Utility - weakest, near-catch-all
    [CAT_SYSTEM]      = 2,   // System - broad
    [CAT_SETTINGS]    = 3,
    [CAT_INTERNET]    = 4,   // Network - weak; many apps are incidentally online
    [CAT_EDUCATION]   = 6,
    [CAT_SCIENCE]     = 7,
    [CAT_MULTIMEDIA]  = 8,
    [CAT_DEVELOPMENT] = 9,
    [CAT_OFFICE]      = 10,
    [CAT_GRAPHICS]    = 11,
    [CAT_GAMES]       = 12,   // very strong - almost never incidental
    [CAT_OTHER]       = 0,    // catch-all; loses to every real match
};

// Maps a freedesktop Categories= token to a Stellar section. Covers the
// registered Main Categories plus the common Additional Categories that clearly
// belong to one section, so more apps land somewhere sensible instead of
// "Other". An entry may match several of these; the one whose section has the
// highest k_category_priority wins (see apply in parse_menu_entry). Tokens not
// listed (purely technical hints like "Qt", "GTK", "ConsoleOnly") are ignored.
struct cat_map {
    const char *token;
    MenuCategory cat;
};

static const struct cat_map k_cat_map[] = {
    // --- Main Categories ---
    { "Utility",          CAT_ACCESSORIES },
    { "Development",      CAT_DEVELOPMENT },
    { "Education",        CAT_EDUCATION },
    { "Game",            CAT_GAMES },
    { "Graphics",        CAT_GRAPHICS },
    { "Network",         CAT_INTERNET },
    { "AudioVideo",      CAT_MULTIMEDIA },
    { "Audio",           CAT_MULTIMEDIA },
    { "Video",           CAT_MULTIMEDIA },
    { "Office",          CAT_OFFICE },
    { "Science",         CAT_SCIENCE },
    { "Settings",        CAT_SETTINGS },
    { "System",          CAT_SYSTEM },

    // --- Additional Categories folded into the sections above ---
    // Accessories (Utility-ish)
    { "Accessibility",   CAT_ACCESSORIES },
    { "Archiving",       CAT_ACCESSORIES },
    { "Compression",     CAT_ACCESSORIES },
    { "Calculator",      CAT_ACCESSORIES },
    { "Clock",           CAT_ACCESSORIES },
    { "TextEditor",      CAT_ACCESSORIES },
    { "TextTools",       CAT_ACCESSORIES },
    // Development
    { "IDE",             CAT_DEVELOPMENT },
    { "Building",        CAT_DEVELOPMENT },
    { "Debugger",        CAT_DEVELOPMENT },
    { "GUIDesigner",     CAT_DEVELOPMENT },
    { "Profiling",       CAT_DEVELOPMENT },
    { "RevisionControl", CAT_DEVELOPMENT },
    { "WebDevelopment",  CAT_DEVELOPMENT },
    // Education / Science (Education tokens kept in Education; hard-science
    // tokens to Science)
    { "Languages",       CAT_EDUCATION },
    { "Math",            CAT_SCIENCE },
    { "NumericalAnalysis", CAT_SCIENCE },
    { "Physics",         CAT_SCIENCE },
    { "Chemistry",       CAT_SCIENCE },
    { "Biology",         CAT_SCIENCE },
    { "Astronomy",       CAT_SCIENCE },
    { "Electronics",     CAT_SCIENCE },
    { "Engineering",     CAT_SCIENCE },
    // Games
    { "ActionGame",      CAT_GAMES },
    { "ArcadeGame",      CAT_GAMES },
    { "BoardGame",       CAT_GAMES },
    { "BlocksGame",      CAT_GAMES },
    { "CardGame",        CAT_GAMES },
    { "KidsGame",        CAT_GAMES },
    { "LogicGame",       CAT_GAMES },
    { "RolePlaying",     CAT_GAMES },
    { "Shooter",        CAT_GAMES },
    { "Simulation",      CAT_GAMES },
    { "SportsGame",      CAT_GAMES },
    { "StrategyGame",    CAT_GAMES },
    { "Emulator",        CAT_GAMES },
    // Graphics
    { "2DGraphics",      CAT_GRAPHICS },
    { "3DGraphics",      CAT_GRAPHICS },
    { "VectorGraphics",  CAT_GRAPHICS },
    { "RasterGraphics",  CAT_GRAPHICS },
    { "Photography",     CAT_GRAPHICS },
    { "Viewer",          CAT_GRAPHICS },
    { "Scanning",        CAT_GRAPHICS },
    { "OCR",             CAT_GRAPHICS },
    { "Publishing",      CAT_GRAPHICS },
    // Internet (Network sub-tokens)
    { "Email",           CAT_INTERNET },
    { "WebBrowser",      CAT_INTERNET },
    { "Chat",            CAT_INTERNET },
    { "InstantMessaging", CAT_INTERNET },
    { "IRCClient",       CAT_INTERNET },
    { "Feed",            CAT_INTERNET },
    { "News",            CAT_INTERNET },
    { "P2P",             CAT_INTERNET },
    { "RemoteAccess",    CAT_INTERNET },
    { "Telephony",       CAT_INTERNET },
    { "VideoConference", CAT_INTERNET },
    { "FileTransfer",    CAT_INTERNET },
    // Multimedia
    { "Player",          CAT_MULTIMEDIA },
    { "Recorder",        CAT_MULTIMEDIA },
    { "Music",           CAT_MULTIMEDIA },
    { "Mixer",           CAT_MULTIMEDIA },
    { "Sequencer",       CAT_MULTIMEDIA },
    { "Tuner",           CAT_MULTIMEDIA },
    { "TV",              CAT_MULTIMEDIA },
    { "AudioVideoEditing", CAT_MULTIMEDIA },
    { "DiscBurning",     CAT_MULTIMEDIA },
    // Office
    { "WordProcessor",   CAT_OFFICE },
    { "Spreadsheet",     CAT_OFFICE },
    { "Presentation",    CAT_OFFICE },
    { "Database",        CAT_OFFICE },
    { "Calendar",        CAT_OFFICE },
    { "ContactManagement", CAT_OFFICE },
    { "Dictionary",      CAT_OFFICE },
    { "Chart",           CAT_OFFICE },
    { "Finance",         CAT_OFFICE },
    { "FlowChart",       CAT_OFFICE },
    { "PDA",             CAT_OFFICE },
    { "ProjectManagement", CAT_OFFICE },
    // Settings
    { "HardwareSettings", CAT_SETTINGS },
    { "Printing",        CAT_SETTINGS },
    { "PackageManager",  CAT_SETTINGS },
    { "Security",        CAT_SETTINGS },
    { "DesktopSettings", CAT_SETTINGS },
    // System
    { "Monitor",         CAT_SYSTEM },
    { "FileManager",     CAT_SYSTEM },
    { "FileTools",       CAT_SYSTEM },
    { "TerminalEmulator", CAT_SYSTEM },
    { "Filesystem",      CAT_SYSTEM },
};

/* ------------------------------------------------------------------ */
/*  A single menu item                                                 */
/* ------------------------------------------------------------------ */

typedef struct {
    char id[256];          // desktop-file ID, e.g. "org.gnome.gedit" - dedup key
    char name[XDG_DE_VALUE];
    char exec[XDG_DE_VALUE];   // field codes stripped, ready to spawn
    char icon[XDG_DE_VALUE];   // raw Icon= value (name or path); may be empty
    bool terminal;             // Terminal=true - Awesome wraps in a term emulator
    MenuCategory cat;
} MenuItem;

/* ------------------------------------------------------------------ */
/*  Module state: the cached JSON document                             */
/* ------------------------------------------------------------------ */

static char *g_menu_json = NULL;       // heap-allocated, owned here
static size_t g_menu_json_len = 0;

/* ------------------------------------------------------------------ */
/*  Small string helpers                                               */
/* ------------------------------------------------------------------ */
//
// The semicolon-list test, PATH resolver, and Exec field-code stripper used to
// live here. They are now in xdg_util.{c,h}, shared verbatim with
// xdg_autostart.c (xdg_in_semicolon_list / xdg_command_exists /
// xdg_strip_field_codes). Only the genuinely menu-specific helpers remain
// below.

// Derive the desktop-file ID from a path under an applications/ root, per the
// Desktop Entry Spec: the path relative to the applications/ dir with '/'
// turned into '-' and the ".desktop" suffix removed. We don't have the root
// handy at call sites that matter for dedup correctness across nested dirs, so
// we approximate with the basename (sans suffix), which is correct for the
// flat layout used by virtually all real entries and still dedups
// system-vs-user copies of the same file. Nested vendor subdirs are rare and
// at worst produce a duplicate, never a wrong launch.
static void derive_desktop_id(const char *filename, char *out, size_t out_sz) {
    snprintf(out, out_sz, "%s", filename);
    size_t len = strlen(out);
    if (len >= 8 && strcmp(out + len - 8, ".desktop") == 0) {
        out[len - 8] = '\0';
    }
}

/* ------------------------------------------------------------------ */
/*  JSON string escaping                                               */
/* ------------------------------------------------------------------ */

// Append `src` to the buffer (cursor in *pos, capacity cap) as a JSON string
// BODY (no surrounding quotes), escaping per RFC 8259. Truncates safely if the
// buffer fills. This is the one piece hand-rolled JSON generation must get
// right: names like  Andy's "Editor"  or Windows-style paths with backslashes
// would otherwise corrupt the whole document.
static void json_escape_into(char *buf, size_t cap, size_t *pos, const char *src) {
    if (!src) src = "";
    for (const unsigned char *p = (const unsigned char *)src; *p; p++) {
        // need at most 6 bytes for \uXXXX plus room for the trailing quote etc.
        if (*pos + 6 >= cap) {
            break;
        }
        unsigned char c = *p;
        switch (c) {
            case '"':  buf[(*pos)++] = '\\'; buf[(*pos)++] = '"';  break;
            case '\\': buf[(*pos)++] = '\\'; buf[(*pos)++] = '\\'; break;
            case '\b': buf[(*pos)++] = '\\'; buf[(*pos)++] = 'b';  break;
            case '\f': buf[(*pos)++] = '\\'; buf[(*pos)++] = 'f';  break;
            case '\n': buf[(*pos)++] = '\\'; buf[(*pos)++] = 'n';  break;
            case '\r': buf[(*pos)++] = '\\'; buf[(*pos)++] = 'r';  break;
            case '\t': buf[(*pos)++] = '\\'; buf[(*pos)++] = 't';  break;
            default:
                if (c < 0x20) {
                    // other control chars -> \u00XX
                    int n = snprintf(buf + *pos, cap - *pos, "\\u%04x", c);
                    if (n > 0) *pos += (size_t)n;
                } else {
                    // UTF-8 bytes >= 0x20 pass through verbatim; valid UTF-8 in
                    // the source stays valid UTF-8 in the output.
                    buf[(*pos)++] = (char)c;
                }
                break;
        }
    }
}

// Append a raw (already-safe) literal, truncating on overflow.
static void json_append_raw(char *buf, size_t cap, size_t *pos, const char *lit) {
    while (*lit && *pos + 1 < cap) {
        buf[(*pos)++] = *lit++;
    }
}

/* ------------------------------------------------------------------ */
/*  Desktop entry parsing (menu-specific field set)                    */
/* ------------------------------------------------------------------ */

// Parse just the fields the menu needs from one .desktop file into `item`.
// Returns true if the entry is a valid, visible Type=Application launcher for
// the given desktop name; false if it should not appear in the menu (or failed
// to parse). On true, item->cat is assigned and item->exec is field-code-clean.
static bool parse_menu_entry(const char *filepath, const char *filename,
                             const char *desktop_name, MenuItem *item) {
    FILE *fp = fopen(filepath, "r");
    if (!fp) {
        return false;
    }

    memset(item, 0, sizeof(*item));
    item->cat = CAT_OTHER;

    char type[64]        = {0};
    char categories[XDG_DE_LIST] = {0};
    char only_show_in[XDG_DE_LIST] = {0};
    char not_show_in[XDG_DE_LIST]  = {0};
    char try_exec[XDG_DE_VALUE]    = {0};
    bool no_display = false;
    bool hidden     = false;

    char line[2048];
    bool in_group = false;

    while (fgets(line, sizeof(line), fp)) {
        // trim trailing newline / CR
        size_t len = strlen(line);
        while (len > 0 && (line[len - 1] == '\n' || line[len - 1] == '\r')) {
            line[--len] = '\0';
        }

        if (line[0] == '[') {
            // Only the [Desktop Entry] group matters; stop honoring keys once
            // we cross into any action group like [Desktop Action new-window].
            in_group = (strcmp(line, "[Desktop Entry]") == 0);
            continue;
        }
        if (!in_group || line[0] == '#' || line[0] == '\0') {
            continue;
        }

        char *eq = strchr(line, '=');
        if (!eq) {
            continue;
        }
        *eq = '\0';
        char *key = line;
        char *val = eq + 1;

        size_t klen = strlen(key);
        while (klen > 0 && key[klen - 1] == ' ') key[--klen] = '\0';
        while (*val == ' ') val++;

        // Skip localized keys (e.g. "Name[de]"): take only the unlocalized one.
        if (strchr(key, '[')) {
            continue;
        }

        if (strcmp(key, "Type") == 0) {
            snprintf(type, sizeof(type), "%s", val);
        } else if (strcmp(key, "Name") == 0) {
            snprintf(item->name, sizeof(item->name), "%s", val);
        } else if (strcmp(key, "Exec") == 0) {
            snprintf(item->exec, sizeof(item->exec), "%s", val);
        } else if (strcmp(key, "Icon") == 0) {
            snprintf(item->icon, sizeof(item->icon), "%s", val);
        } else if (strcmp(key, "Categories") == 0) {
            snprintf(categories, sizeof(categories), "%s", val);
        } else if (strcmp(key, "OnlyShowIn") == 0) {
            snprintf(only_show_in, sizeof(only_show_in), "%s", val);
        } else if (strcmp(key, "NotShowIn") == 0) {
            snprintf(not_show_in, sizeof(not_show_in), "%s", val);
        } else if (strcmp(key, "TryExec") == 0) {
            snprintf(try_exec, sizeof(try_exec), "%s", val);
        } else if (strcmp(key, "NoDisplay") == 0) {
            no_display = (strcasecmp(val, "true") == 0);
        } else if (strcmp(key, "Hidden") == 0) {
            hidden = (strcasecmp(val, "true") == 0);
        } else if (strcmp(key, "Terminal") == 0) {
            item->terminal = (strcasecmp(val, "true") == 0);
        }
    }
    fclose(fp);

    // --- visibility filtering ---

    // Must be an application with something to run.
    if (strcmp(type, "Application") != 0 || item->name[0] == '\0' ||
        item->exec[0] == '\0') {
        return false;
    }

    // Hidden means "deleted, treat as absent". NoDisplay means "valid but not
    // shown in menus" (often a MIME handler). Both exclude from the menu.
    if (hidden || no_display) {
        return false;
    }

    if (only_show_in[0] && !xdg_in_semicolon_list(only_show_in, desktop_name)) {
        return false;
    }
    if (not_show_in[0] && xdg_in_semicolon_list(not_show_in, desktop_name)) {
        return false;
    }
    if (try_exec[0] && !xdg_command_exists(try_exec)) {
        return false;
    }

    // --- finalize exec + id + category ---

    xdg_strip_field_codes(item->exec);
    if (item->exec[0] == '\0') {
        return false;
    }

    derive_desktop_id(filename, item->id, sizeof(item->id));

    // Resolve the section by scanning ALL category tokens and keeping the one
    // whose section has the highest k_category_priority. Categories= is an
    // unordered set per spec, so we must not let token order decide (Steam's
    // "Network;FileTransfer;Game" must resolve to Games, not Internet). Falls
    // back to CAT_OTHER (already set) when no token maps.
    {
        char cats_buf[XDG_DE_LIST];
        snprintf(cats_buf, sizeof(cats_buf), "%s", categories);
        int best_priority = -1;   // -1 so even CAT_OTHER(0) can't pre-empt a real match
        char *saveptr = NULL;
        for (char *tok = strtok_r(cats_buf, ";", &saveptr);
             tok; tok = strtok_r(NULL, ";", &saveptr)) {
            for (size_t i = 0; i < sizeof(k_cat_map) / sizeof(k_cat_map[0]); i++) {
                if (strcmp(tok, k_cat_map[i].token) == 0) {
                    MenuCategory c = k_cat_map[i].cat;
                    int pr = k_category_priority[c];
                    if (pr > best_priority) {
                        best_priority = pr;
                        item->cat = c;
                    }
                    break;   // token matched; move to next token
                }
            }
        }
    }

    return true;
}

/* ------------------------------------------------------------------ */
/*  Directory scanning with dedup                                      */
/* ------------------------------------------------------------------ */

// Scan one applications/ directory. Entries whose ID already exists are
// replaced (user-overrides-system), so callers must scan lowest-priority dirs
// first and the user dir last.
static void scan_applications_dir(const char *dir_path, const char *desktop_name,
                                  MenuItem *items, int *count, int max_items) {
    DIR *d = opendir(dir_path);
    if (!d) {
        return;
    }

    struct dirent *de;
    while ((de = readdir(d)) != NULL) {
        size_t namelen = strlen(de->d_name);
        if (namelen < 9 || strcmp(de->d_name + namelen - 8, ".desktop") != 0) {
            continue;
        }

        char filepath[PATH_MAX];
        snprintf(filepath, sizeof(filepath), "%s/%s", dir_path, de->d_name);

        MenuItem tmp;
        if (!parse_menu_entry(filepath, de->d_name, desktop_name, &tmp)) {
            continue;
        }

        // Dedup on desktop ID.
        int slot = -1;
        for (int i = 0; i < *count; i++) {
            if (strcmp(items[i].id, tmp.id) == 0) {
                slot = i;
                break;
            }
        }

        if (slot >= 0) {
            items[slot] = tmp;     // higher-priority dir overrides
        } else if (*count < max_items) {
            items[(*count)++] = tmp;
        } else {
            log_error("xdg-menu: hit entry limit (%d), skipping %s",
                      max_items, de->d_name);
        }
    }

    closedir(d);
}

// Collect every applications/ directory in XDG precedence order and scan them
// so the user dir wins. $XDG_DATA_HOME (default ~/.local/share) is highest
// priority; $XDG_DATA_DIRS (default /usr/local/share:/usr/share) follow, with
// earlier entries higher priority. Because scan_applications_dir overrides on
// collision, we scan in REVERSE precedence: lowest first, user dir last.
static int collect_all_entries(const char *desktop_name, MenuItem *items,
                               int max_items) {
    int count = 0;

    // Build an ordered low->high list of applications dirs.
    char dirs[40][PATH_MAX];
    int ndirs = 0;

    const char *data_dirs = getenv("XDG_DATA_DIRS");
    if (!data_dirs || !data_dirs[0]) {
        data_dirs = "/usr/local/share:/usr/share";
    }

    // XDG_DATA_DIRS: earlier = higher priority, so push them in reverse so the
    // final list is strictly low->high.
    {
        char buf[PATH_MAX * 8];
        snprintf(buf, sizeof(buf), "%s", data_dirs);

        char *toks[40];
        int nt = 0;
        char *saveptr = NULL;
        for (char *t = strtok_r(buf, ":", &saveptr);
             t && nt < 40; t = strtok_r(NULL, ":", &saveptr)) {
            toks[nt++] = t;
        }
        for (int i = nt - 1; i >= 0 && ndirs < 40; i--) {
            snprintf(dirs[ndirs++], PATH_MAX, "%s/applications", toks[i]);
        }
    }

    // XDG_DATA_HOME (or ~/.local/share) - highest priority, scanned last.
    if (ndirs < 40) {
        const char *data_home = getenv("XDG_DATA_HOME");
        if (data_home && data_home[0]) {
            snprintf(dirs[ndirs++], PATH_MAX, "%s/applications", data_home);
        } else {
            const char *home = get_user_home_dir();
            snprintf(dirs[ndirs++], PATH_MAX, "%s/.local/share/applications", home);
        }
    }

    for (int i = 0; i < ndirs; i++) {
        log_info("xdg-menu: scanning %s", dirs[i]);
        scan_applications_dir(dirs[i], desktop_name, items, &count, max_items);
    }

    return count;
}

/* ------------------------------------------------------------------ */
/*  Sorting                                                            */
/* ------------------------------------------------------------------ */

// Sort by category (display order), then by name (case-insensitive) within a
// category, so the emitted JSON is already in render order and the Lua side
// can walk it linearly.
static int item_cmp(const void *a, const void *b) {
    const MenuItem *ia = a, *ib = b;
    if (ia->cat != ib->cat) {
        return (int)ia->cat - (int)ib->cat;
    }
    return strcasecmp(ia->name, ib->name);
}

/* ------------------------------------------------------------------ */
/*  JSON document emission                                             */
/* ------------------------------------------------------------------ */

// Shape:
// {
//   "categories": [
//     { "name": "Internet",
//       "items": [ {"name":..,"exec":..,"icon":..,"terminal":bool}, ... ] },
//     ...
//   ]
// }
// Empty categories are omitted so the menu has no dead submenus.
static char *emit_menu_json(MenuItem *items, int count, size_t *out_len) {
    // Generous fixed buffer: ~256 bytes/item worst case + structural overhead.
    // For the 2048-item ceiling that's ~512KB; we size to the actual count.
    size_t cap = (size_t)count * 512 + 4096;
    if (cap < 8192) cap = 8192;

    char *buf = malloc(cap);
    if (!buf) {
        log_error("xdg-menu: malloc(%zu) failed", cap);
        return NULL;
    }

    size_t pos = 0;
    json_append_raw(buf, cap, &pos, "{\"categories\":[");

    bool first_cat = true;
    for (int cat = 0; cat < CAT_COUNT; cat++) {
        // Find the contiguous run of items in this category (items are sorted).
        int start = -1, end = -1;
        for (int i = 0; i < count; i++) {
            if (items[i].cat == (MenuCategory)cat) {
                if (start < 0) start = i;
                end = i;
            } else if (start >= 0) {
                break;   // run ended
            }
        }
        if (start < 0) {
            continue;    // no items -> omit the section entirely
        }

        if (!first_cat) {
            json_append_raw(buf, cap, &pos, ",");
        }
        first_cat = false;

        json_append_raw(buf, cap, &pos, "{\"name\":\"");
        json_escape_into(buf, cap, &pos, k_category_labels[cat]);
        json_append_raw(buf, cap, &pos, "\",\"items\":[");

        for (int i = start; i <= end; i++) {
            if (i > start) {
                json_append_raw(buf, cap, &pos, ",");
            }
            json_append_raw(buf, cap, &pos, "{\"name\":\"");
            json_escape_into(buf, cap, &pos, items[i].name);
            json_append_raw(buf, cap, &pos, "\",\"exec\":\"");
            json_escape_into(buf, cap, &pos, items[i].exec);
            json_append_raw(buf, cap, &pos, "\",\"icon\":\"");
            json_escape_into(buf, cap, &pos, items[i].icon);
            json_append_raw(buf, cap, &pos, "\",\"terminal\":");
            json_append_raw(buf, cap, &pos, items[i].terminal ? "true" : "false");
            json_append_raw(buf, cap, &pos, "}");
        }

        json_append_raw(buf, cap, &pos, "]}");
    }

    json_append_raw(buf, cap, &pos, "]}");
    buf[pos < cap ? pos : cap - 1] = '\0';

    if (out_len) *out_len = pos;
    return buf;
}

/* ------------------------------------------------------------------ */
/*  Cache write                                                        */
/* ------------------------------------------------------------------ */

static int write_menu_cache(const char *json, size_t len) {
    const char *home = get_user_home_dir();
    char path[PATH_MAX];
    snprintf(path, sizeof(path), "%s/.cache/stellar/menu.json", home);

    // Write to a temp file then rename, so a concurrent reader never sees a
    // half-written document (atomic replace on the same filesystem).
    char tmp[PATH_MAX];
    int need = snprintf(tmp, sizeof(tmp), "%s/.cache/stellar/menu.json.tmp", home);
    if (need < 0 || (size_t)need >= sizeof(tmp)) {
        log_error("xdg-menu: cache temp path too long");
        return -1;
    }

    FILE *f = fopen(tmp, "w");
    if (!f) {
        log_error("xdg-menu: cannot write %s: %s", tmp, strerror(errno));
        return -1;
    }
    fwrite(json, 1, len, f);
    fputc('\n', f);
    fclose(f);

    if (rename(tmp, path) != 0) {
        log_error("xdg-menu: rename %s -> %s failed: %s",
                  tmp, path, strerror(errno));
        unlink(tmp);
        return -1;
    }

    log_info("xdg-menu: cached menu at %s (%zu bytes)", path, len);
    return 0;
}

/* ------------------------------------------------------------------ */
/*  Public API                                                         */
/* ------------------------------------------------------------------ */

int build_application_menu(StellarState *st) {
    (void)st;

    const char *desktop = getenv("XDG_CURRENT_DESKTOP");
    if (!desktop || !desktop[0]) {
        desktop = "Stellar";
    }

    log_info("xdg-menu: building application menu (desktop=%s)", desktop);

    MenuItem *items = calloc(MAX_MENU_ENTRIES, sizeof(MenuItem));
    if (!items) {
        log_error("xdg-menu: calloc for %d items failed", MAX_MENU_ENTRIES);
        return -1;
    }

    int count = collect_all_entries(desktop, items, MAX_MENU_ENTRIES);
    log_info("xdg-menu: collected %d visible application entries", count);

    qsort(items, (size_t)count, sizeof(MenuItem), item_cmp);

    size_t len = 0;
    char *json = emit_menu_json(items, count, &len);
    free(items);

    if (!json) {
        return -1;
    }

    // Swap into the module cache.
    free(g_menu_json);
    g_menu_json = json;
    g_menu_json_len = len;

    return write_menu_cache(json, len);
}

const char *get_cached_menu_json(void) {
    return g_menu_json;
}

bool push_menu_to_fd(int fd) {
    if (!g_menu_json || fd < 0) {
        return false;
    }

    // One line: "MENU_DATA <json>\n". The Lua bridge splits on the first space.
    static const char prefix[] = "MENU_DATA ";
    ssize_t w1 = write(fd, prefix, sizeof(prefix) - 1);
    ssize_t w2 = write(fd, g_menu_json, g_menu_json_len);
    ssize_t w3 = write(fd, "\n", 1);
    (void)w1; (void)w2; (void)w3;

    log_info("xdg-menu: pushed menu to fd=%d (%zu bytes)", fd, g_menu_json_len);
    return true;
}

void rebuild_and_broadcast_menu(StellarState *st) {
    if (build_application_menu(st) != 0) {
        log_error("xdg-menu: rebuild failed; keeping previous menu");
        // If a previous menu exists we still broadcast it below; if not, bail.
        if (!g_menu_json) return;
    }

    // Build the full "MENU_DATA <json>" line once and broadcast it to all
    // connected clients (broadcast_line appends its own newline).
    size_t line_len = g_menu_json_len + 10 + 1;  // "MENU_DATA " + json + NUL
    char *line = malloc(line_len);
    if (!line) {
        log_error("xdg-menu: malloc for broadcast line failed");
        return;
    }
    snprintf(line, line_len, "MENU_DATA %s", g_menu_json);
    broadcast_line(st, line);
    free(line);

    log_info("xdg-menu: rebuilt and broadcast menu to all clients");
}
