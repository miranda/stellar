// menu_watch.h
// Filesystem watcher that triggers a menu rebuild when .desktop files change.
//
// Mirrors how KDE keeps its menu live: it watches the XDG applications
// directories with inotify (KDE uses KDirWatch, which is inotify on Linux) and
// rebuilds when something changes, rather than rescanning on a timer. A short
// debounce coalesces the burst of events a single package install produces into
// one rebuild.
//
// Integration is minimal and threadless: one inotify fd is added to the main
// select() set, and a debounce deadline is checked once per loop tick (the loop
// already wakes every POLL_US). When the deadline passes after the last event,
// rebuild_and_broadcast_menu() runs and re-pushes to every AwesomeWM instance.

#ifndef MENU_WATCH_H
#define MENU_WATCH_H

#include "stellar.h"

// Initialize the watcher: create the inotify instance and add a watch on each
// existing XDG applications directory ($XDG_DATA_HOME/applications and each
// $XDG_DATA_DIRS/applications). Returns the inotify fd (>= 0) on success, or -1
// on failure (in which case the menu simply won't auto-update; not fatal).
// Call once during init, after build_application_menu().
int menu_watch_init(StellarState *st);

// The inotify fd to add to the main select() read set, or -1 if init failed.
// Add it alongside server_fd/xfd in the loop's FD_SET section.
int menu_watch_fd(void);

// Called when the inotify fd is readable (FD_ISSET true). Drains all pending
// inotify events and arms the debounce timer; does NOT rebuild immediately.
// Cheap - just reads and discards event records and updates a timestamp.
void menu_watch_handle_readable(StellarState *st);

// Called once per main-loop tick (in the after_select section). If a change was
// seen and the debounce interval has elapsed since the last event, this runs
// rebuild_and_broadcast_menu(st) exactly once and clears the dirty flag. No-op
// otherwise, so it's safe and near-free to call every tick.
void menu_watch_tick(StellarState *st);

// Tear down: close watches and the inotify fd. Call from cleanup().
void menu_watch_cleanup(void);

#endif // MENU_WATCH_H
