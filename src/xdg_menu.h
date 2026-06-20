// xdg_menu.h
// XDG application-menu builder for Stellar DE.
//
// Scans the standard application directories ($XDG_DATA_DIRS/applications and
// $XDG_DATA_HOME/applications) for Type=Application desktop entries, applies
// the same visibility filtering as autostart (Hidden / NoDisplay / OnlyShowIn
// / NotShowIn / TryExec), groups them into the freedesktop Menu Spec's main
// categories, and emits the result as a single JSON document.
//
// The JSON is built as text in C (no C-side JSON library; the Lua side already
// has dkjson to parse it) and cached on disk. It is pushed to each AwesomeWM
// instance over IPC as one "MENU_DATA <json>" line, so every screen renders an
// identical menu from a single filesystem scan.
//
// This is the launcher-side complement to xdg_autostart.c: both read .desktop
// files via parse_desktop_entry(), but this one categorizes for display rather
// than executing for session startup.

#ifndef XDG_MENU_H
#define XDG_MENU_H

#include "stellar.h"

// Hard ceiling on entries collected across all application directories.
// Real systems land in the 200-600 range; 2048 is comfortable headroom.
#define MAX_MENU_ENTRIES 2048

// Build the application menu by scanning all application directories, then
// cache the resulting JSON to ~/.cache/stellar/menu.json.
//
// Call once during session init (after init_stellar_cache_dirs, around where
// the bitmap-font cache is synced). Safe to call again to rebuild (e.g. on a
// REBUILD_MENU IPC command after a package install). Returns 0 on success,
// -1 on a hard failure (the cache write failed); a scan that simply finds no
// entries still returns 0 and writes a valid empty menu.
int build_application_menu(StellarState *st);

// Return a pointer to the cached menu JSON built by build_application_menu().
// The returned string is owned by the menu subsystem and remains valid until
// the next build_application_menu() call; callers must not free it. Returns
// NULL if no menu has been built yet. Used by the IPC layer to push the menu
// to an AwesomeWM instance during its ready-for-sync handshake.
const char *get_cached_menu_json(void);

// Push the cached menu to a single client fd as one "MENU_DATA <json>\n" line.
// No-op (returns false) if no menu has been built yet or fd is invalid.
// Called from register_awesome_client() so each Awesome gets the menu exactly
// when its socket is ready, and from the REBUILD_MENU broadcast path.
bool push_menu_to_fd(int fd);

// Rebuild the menu and broadcast it to every connected AwesomeWM instance.
// Convenience wrapper used by the REBUILD_MENU IPC command.
void rebuild_and_broadcast_menu(StellarState *st);

#endif // XDG_MENU_H
