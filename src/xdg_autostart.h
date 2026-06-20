// xdg_autostart.h
// XDG Desktop Entry parser and autostart runner for Stellar DE.
//
// Implements the freedesktop.org Desktop Entry Specification (exec/filtering)
// and the Desktop Application Autostart Specification (directory scanning,
// OnlyShowIn/NotShowIn, user-overrides-system dedup).
//
// Autostart entries are spawned once per session on screen 0's display.
// In a zaphod-heads setup there is no spec-level mechanism for assigning
// autostart apps to a specific screen, and session services (tray applets,
// polkit agents, keyrings, etc.) only need one instance.

#ifndef XDG_AUTOSTART_H
#define XDG_AUTOSTART_H

#include "stellar.h"

#define MAX_DE_VALUE     1024
#define MAX_DE_LIST       512
#define MAX_AUTOSTART     256

/* ---------- Desktop Entry ---------- */

typedef struct {
    /* Identity / metadata */
    char name[MAX_DE_VALUE];
    char type[64];                   /* must be "Application" for autostart */
    char filename[256];              /* basename, e.g. "nm-applet.desktop"  */
    char filepath[PATH_MAX];         /* full path for %k expansion & logs  */

    /* Execution */
    char exec[MAX_DE_VALUE];
    char try_exec[MAX_DE_VALUE];
    char path[MAX_DE_VALUE];         /* working directory                  */
    char icon[MAX_DE_VALUE];

    /* Filtering */
    char only_show_in[MAX_DE_LIST];  /* semicolon-separated desktop names  */
    char not_show_in[MAX_DE_LIST];
    bool hidden;
    bool terminal;
    bool dbus_activatable;           /* if true and no Exec, skip          */

    /* Internal flag - set by parse_desktop_entry on success */
    bool valid;
} DesktopEntry;


/* ---------- Public API ---------- */

// Parse a .desktop file into `entry`.  Returns true on success.
// Only the [Desktop Entry] group is read; everything else is ignored.
bool parse_desktop_entry(const char *filepath, DesktopEntry *entry);

// Fork+exec a single desktop entry.  `display_name` overrides DISPLAY in the
// child (pass NULL to inherit the parent's DISPLAY).  Returns the child PID,
// or -1 on failure.  The child is double-forked so Stellar doesn't need to
// track it - the intermediate process exits immediately and the grandchild is
// reparented to init/PID-1.
pid_t exec_desktop_entry(const DesktopEntry *entry, const char *display_name);

// Scan /etc/xdg/autostart + ~/.config/autostart, apply OnlyShowIn / NotShowIn
// / Hidden / TryExec filtering for the desktop name "Stellar" (read from
// XDG_CURRENT_DESKTOP), and launch every qualifying entry on screen 0.
// Call this once, after all Stellar daemons are running.
void run_xdg_autostart(StellarState *st);

#endif // XDG_AUTOSTART_H
