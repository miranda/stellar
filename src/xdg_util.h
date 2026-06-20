// xdg_desktop_common.h
// Shared primitives for parsing freedesktop Desktop Entry (.desktop) files.
//
// These three helpers are pure functions over strings with no dependency on any
// particular entry struct, and both the autostart runner (xdg_autostart.c) and
// the application-menu builder (xdg_menu.c) need identical behavior from them.
// They live here so there is exactly one implementation of each.
//
// Deliberately NOT shared (see the modules): the .desktop line parser and the
// visibility filter. Those look similar across the two modules but encode
// different intent - the parser reads a different field set on each side, and
// the filter's rules diverge (autostart rejects Terminal=true and ignores
// NoDisplay; the menu keeps Terminal=true and must honor NoDisplay). Forcing
// those into shared code couples the two modules exactly where they disagree.

#ifndef XDG_DESKTOP_COMMON_H
#define XDG_DESKTOP_COMMON_H

#include <stdbool.h>

// Field-size constants for Desktop Entry values. Shared so both modules size
// their buffers identically. A single value (Name, Exec, Icon, ...) uses
// XDG_DE_VALUE; a semicolon-separated list value (Categories, OnlyShowIn, ...)
// uses XDG_DE_LIST.
#define XDG_DE_VALUE   1024
#define XDG_DE_LIST     512

// Case-sensitive membership test in a ';'-separated list, per the Desktop Entry
// Spec (desktop names in OnlyShowIn / NotShowIn are matched exactly). Returns
// false if either argument is NULL or empty.
bool xdg_in_semicolon_list(const char *haystack, const char *needle);

// Resolve a command against PATH. If `name` contains a '/', it is treated as a
// literal path and checked directly. Returns true iff an executable is found.
// Used for TryExec validation.
bool xdg_command_exists(const char *name);

// Strip XDG Exec field codes (%f %F %u %U %d %D %n %N %i %c %k %v %m) in place.
// %% collapses to a literal %. Unknown %-sequences are preserved verbatim (the
// spec-conservative choice). Neither autostart nor the menu substitutes real
// file/URL arguments, so recognized codes are simply removed and the leftover
// whitespace is collapsed. Operates on `exec` in place.
void xdg_strip_field_codes(char *exec);

#endif // XDG_DESKTOP_COMMON_H
