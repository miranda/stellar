// xdg_autostart.c
// XDG Desktop Entry parser and autostart runner for Stellar DE.
// See xdg_autostart.h for the public API.

#include "stellar.h"
#include "xdg_autostart.h"
#include "xdg_util.h"   // xdg_in_semicolon_list, xdg_command_exists, xdg_strip_field_codes
#include <dirent.h>

/* ------------------------------------------------------------------ */
/*  Internal helpers                                                   */
/* ------------------------------------------------------------------ */

// Check whether `needle` appears in a semicolon-separated list `haystack`.
// Both the spec's OnlyShowIn and NotShowIn use this format:
//   OnlyShowIn=GNOME;Stellar;
// Comparison is case-sensitive per the spec.
// Resolve a bare command name against PATH.  Returns true if found and
// executable.  If `name` contains a slash it's treated as a literal path.
// Strip XDG Exec field codes (%f %F %u %U %d %D %n %N %i %c %k %v %m)
// from a command string.  %% becomes a literal %.  For autostart there are
// no file/URL arguments to substitute, so the codes are simply removed.
// Operates in-place.
// Read a line from `fp`, stripping the trailing newline.  Returns the line
// buffer on success, NULL on EOF.
static char *read_line(FILE *fp, char *buf, size_t bufsz) {
    if (!fgets(buf, (int)bufsz, fp)) {
        return NULL;
    }
    size_t len = strlen(buf);
    while (len > 0 && (buf[len - 1] == '\n' || buf[len - 1] == '\r')) {
        buf[--len] = '\0';
    }
    return buf;
}

/* ------------------------------------------------------------------ */
/*  Desktop Entry parser                                               */
/* ------------------------------------------------------------------ */

bool parse_desktop_entry(const char *filepath, DesktopEntry *entry) {
    memset(entry, 0, sizeof(*entry));

    FILE *fp = fopen(filepath, "r");
    if (!fp) {
        return false;
    }

    // Record paths for later use
    snprintf(entry->filepath, sizeof(entry->filepath), "%s", filepath);

    const char *slash = strrchr(filepath, '/');
    snprintf(entry->filename, sizeof(entry->filename), "%s",
             slash ? slash + 1 : filepath);

    char line[2048];
    bool in_entry_group = false;

    while (read_line(fp, line, sizeof(line))) {
        // Group header
        if (line[0] == '[') {
            in_entry_group = (strcmp(line, "[Desktop Entry]") == 0);
            continue;
        }

        if (!in_entry_group) {
            continue;
        }

        // Skip comments and blank lines
        if (line[0] == '#' || line[0] == '\0') {
            continue;
        }

        char *eq = strchr(line, '=');
        if (!eq) {
            continue;
        }

        *eq = '\0';
        char *key = line;
        char *val = eq + 1;

        // Trim key (spec says no spaces around '=' but be tolerant)
        size_t klen = strlen(key);
        while (klen > 0 && key[klen - 1] == ' ') {
            key[--klen] = '\0';
        }
        while (*val == ' ') {
            val++;
        }

        // Map known keys
        if (strcmp(key, "Type") == 0) {
            snprintf(entry->type, sizeof(entry->type), "%s", val);
        } else if (strcmp(key, "Name") == 0) {
            // Only take the unlocalized Name (no brackets in key)
            snprintf(entry->name, sizeof(entry->name), "%s", val);
        } else if (strcmp(key, "Exec") == 0) {
            snprintf(entry->exec, sizeof(entry->exec), "%s", val);
        } else if (strcmp(key, "TryExec") == 0) {
            snprintf(entry->try_exec, sizeof(entry->try_exec), "%s", val);
        } else if (strcmp(key, "Path") == 0) {
            snprintf(entry->path, sizeof(entry->path), "%s", val);
        } else if (strcmp(key, "Icon") == 0) {
            snprintf(entry->icon, sizeof(entry->icon), "%s", val);
        } else if (strcmp(key, "OnlyShowIn") == 0) {
            snprintf(entry->only_show_in, sizeof(entry->only_show_in), "%s", val);
        } else if (strcmp(key, "NotShowIn") == 0) {
            snprintf(entry->not_show_in, sizeof(entry->not_show_in), "%s", val);
        } else if (strcmp(key, "Hidden") == 0) {
            entry->hidden = (strcasecmp(val, "true") == 0);
        } else if (strcmp(key, "Terminal") == 0) {
            entry->terminal = (strcasecmp(val, "true") == 0);
        } else if (strcmp(key, "DBusActivatable") == 0) {
            entry->dbus_activatable = (strcasecmp(val, "true") == 0);
        }
    }

    fclose(fp);

    entry->valid = (entry->type[0] != '\0' &&
                    (entry->exec[0] != '\0' || entry->dbus_activatable));
    return entry->valid;
}

/* ------------------------------------------------------------------ */
/*  Filtering                                                          */
/* ------------------------------------------------------------------ */

// Determine whether a parsed entry should be launched for the given desktop.
// `desktop_name` is compared against OnlyShowIn / NotShowIn.
// Also checks Hidden, Type, TryExec, and the presence of an Exec line.
static bool should_run_entry(const DesktopEntry *entry, const char *desktop_name) {
    if (!entry->valid) {
        return false;
    }

    if (entry->hidden) {
        return false;
    }

    // Must be Type=Application
    if (strcmp(entry->type, "Application") != 0) {
        return false;
    }

    // OnlyShowIn: if set, desktop_name must be in the list
    if (entry->only_show_in[0]) {
        if (!xdg_in_semicolon_list(entry->only_show_in, desktop_name)) {
            return false;
        }
    }

    // NotShowIn: if desktop_name is in the list, skip
    if (entry->not_show_in[0]) {
        if (xdg_in_semicolon_list(entry->not_show_in, desktop_name)) {
            return false;
        }
    }

    // Need an Exec line (DBusActivatable-only entries with no Exec are
    // activated by the bus, not by us - skip them).
    if (entry->exec[0] == '\0') {
        return false;
    }

    // TryExec: if specified, the binary must exist in PATH
    if (entry->try_exec[0]) {
        if (!xdg_command_exists(entry->try_exec)) {
            return false;
        }
    }

    // Terminal=true entries need a terminal emulator wrapper.
    // Autostart services should virtually never set this, so we skip them
    // rather than pulling in a terminal dependency.
    if (entry->terminal) {
        log_info("xdg-autostart: skipping Terminal=true entry: %s", entry->name);
        return false;
    }

    return true;
}

/* ------------------------------------------------------------------ */
/*  Execution                                                          */
/* ------------------------------------------------------------------ */

pid_t exec_desktop_entry(const DesktopEntry *entry, const char *display_name) {
    if (!entry || entry->exec[0] == '\0') {
        return -1;
    }

    // Build the cleaned command line
    char cmdline[MAX_DE_VALUE];
    snprintf(cmdline, sizeof(cmdline), "%s", entry->exec);
    xdg_strip_field_codes(cmdline);

    if (cmdline[0] == '\0') {
        return -1;
    }

    // Double-fork so the grandchild is reparented to init.
    // This way Stellar doesn't accumulate autostart PIDs to track.
    pid_t pid = fork();
    if (pid < 0) {
        log_error("xdg-autostart: fork failed for %s: %s",
                  entry->name, strerror(errno));
        return -1;
    }

    if (pid == 0) {
        // --- Intermediate child ---
        // Fork again immediately.  The grandchild does the real exec.
        pid_t grandchild = fork();
        if (grandchild < 0) {
            _exit(127);
        }

        if (grandchild > 0) {
            // Intermediate exits right away so Stellar can reap it
            // quickly, and the grandchild is inherited by init.
            _exit(0);
        }

        // --- Grandchild (session-detached) ---
        setsid();

        if (display_name && display_name[0]) {
            setenv("DISPLAY", display_name, 1);
        }

        if (entry->path[0]) {
            if (chdir(entry->path) != 0) {
                fprintf(stderr, "[stellar] xdg-autostart: chdir(%s) failed: %s\n",
                        entry->path, strerror(errno));
            }
        }

        // Execute through the shell so that complex Exec lines
        // (arguments, quoting) are handled correctly.
        execl("/bin/sh", "sh", "-c", cmdline, (char *)NULL);
        _exit(127);
    }

    // Parent: wait for the intermediate child (it exits immediately).
    // Retry on EINTR since our SIGCHLD handler can interrupt this.
    int status;
    while (waitpid(pid, &status, 0) < 0 && errno == EINTR)
        ;

    return pid;
}

/* ------------------------------------------------------------------ */
/*  Directory scanning & autostart                                     */
/* ------------------------------------------------------------------ */

// Scan a single autostart directory and add entries to `entries`.
// If an entry with the same filename already exists (from an earlier dir),
// it is overwritten - this implements user-overrides-system semantics.
static void scan_autostart_dir(
    const char *dir_path,
    DesktopEntry *entries,
    int *count,
    int max_entries
) {
    DIR *d = opendir(dir_path);
    if (!d) {
        return;
    }

    struct dirent *de;
    while ((de = readdir(d)) != NULL) {
        // Only .desktop files
        size_t namelen = strlen(de->d_name);
        if (namelen < 9 || strcmp(de->d_name + namelen - 8, ".desktop") != 0) {
            continue;
        }

        char filepath[PATH_MAX];
        snprintf(filepath, sizeof(filepath), "%s/%s", dir_path, de->d_name);

        DesktopEntry tmp;
        if (!parse_desktop_entry(filepath, &tmp)) {
            continue;
        }

        // Check for existing entry with the same filename (dedup)
        int slot = -1;
        for (int i = 0; i < *count; i++) {
            if (strcmp(entries[i].filename, tmp.filename) == 0) {
                slot = i;
                break;
            }
        }

        if (slot >= 0) {
            // Higher-priority entry replaces the earlier one
            entries[slot] = tmp;
        } else if (*count < max_entries) {
            entries[(*count)++] = tmp;
        } else {
            log_error("xdg-autostart: hit entry limit (%d), skipping %s",
                      max_entries, de->d_name);
        }
    }

    closedir(d);
}

// Double-fork and exec a plain command (script or binary).
// Same orphan pattern as exec_desktop_entry.
static pid_t exec_plain(const char *filepath, const char *display_name) {
    pid_t pid = fork();
    if (pid < 0) {
        log_error("stellar-autostart: fork failed for %s: %s",
                  filepath, strerror(errno));
        return -1;
    }

    if (pid == 0) {
        pid_t grandchild = fork();
        if (grandchild < 0) {
            _exit(127);
        }
        if (grandchild > 0) {
            _exit(0);
        }

        setsid();

        if (display_name && display_name[0]) {
            setenv("DISPLAY", display_name, 1);
        }

        // If it starts with #! or is a binary, execl handles it.
        // Wrap in sh -c so scripts without +x on the shebang still work
        // and so that PATH-relative helpers inside the script resolve.
        execl("/bin/sh", "sh", "-c", filepath, (char *)NULL);
        _exit(127);
    }

    int status;
    while (waitpid(pid, &status, 0) < 0 && errno == EINTR)
        ;
    return pid;
}

// Scan ~/.config/stellar/autostart/ for plain executables and .desktop files.
// Plain executables (regular file + x bit) are run directly.
// .desktop files are parsed and executed without OnlyShowIn/NotShowIn
// filtering, since their presence in this directory is an explicit choice.
static void run_stellar_autostart(StellarState *st, const char *display_name) {
    const char *home = get_user_home_dir();
    char dir_path[PATH_MAX];
    snprintf(dir_path, sizeof(dir_path), "%s/.config/stellar/autostart", home);

    DIR *d = opendir(dir_path);
    if (!d) {
        // Directory doesn't exist yet - that's fine, not an error
        log_info("stellar-autostart: %s not found, skipping", dir_path);
        return;
    }

    log_info("stellar-autostart: scanning %s", dir_path);
    int launched = 0;

    struct dirent *de;
    while ((de = readdir(d)) != NULL) {
        // Skip . and ..
        if (de->d_name[0] == '.') {
            continue;
        }

        char filepath[PATH_MAX];
        snprintf(filepath, sizeof(filepath), "%s/%s", dir_path, de->d_name);

        struct stat sb;
        if (stat(filepath, &sb) != 0 || !S_ISREG(sb.st_mode)) {
            continue;
        }

        size_t namelen = strlen(de->d_name);
        bool is_desktop = (namelen >= 9 &&
                           strcmp(de->d_name + namelen - 8, ".desktop") == 0);

        if (is_desktop) {
            // Parse and run without OnlyShowIn filtering
            DesktopEntry entry;
            if (!parse_desktop_entry(filepath, &entry)) {
                log_error("stellar-autostart: failed to parse %s", de->d_name);
                continue;
            }
            if (entry.hidden || entry.exec[0] == '\0') {
                continue;
            }
            log_info("stellar-autostart: launching %s (%s) -> %s",
                     de->d_name, entry.name, entry.exec);
            exec_desktop_entry(&entry, display_name);
            launched++;
        } else {
            // Plain executable - must have +x
            if (!(sb.st_mode & S_IXUSR)) {
                log_info("stellar-autostart: skip %s (not executable)", de->d_name);
                continue;
            }
            log_info("stellar-autostart: launching %s", filepath);
            exec_plain(filepath, display_name);
            launched++;
        }
    }

    closedir(d);
    log_info("stellar-autostart: launched %d entries", launched);
}

void run_xdg_autostart(StellarState *st) {
    // Determine the desktop name for filtering.
    // setup_session_env() already set XDG_CURRENT_DESKTOP=Stellar.
    const char *desktop = getenv("XDG_CURRENT_DESKTOP");
    if (!desktop || desktop[0] == '\0') {
        desktop = "Stellar";
    }

    // The display for autostart children - screen 0 in the zaphod setup.
    // Session-level services only need one instance and their windows are
    // managed by the AwesomeWM on screen 0.
    const char *display_name = st->screens[0].display_name;

    log_info("xdg-autostart: scanning (desktop=%s display=%s)", desktop, display_name);

    // ------------------------------------------------------------------
    // 1. Collect entries from system dirs, then overlay user dir.
    //    Per the autostart spec, $XDG_CONFIG_DIRS defaults to /etc/xdg
    //    and $XDG_CONFIG_HOME defaults to ~/.config.
    //    Entries scanned later with the same filename overwrite earlier
    //    ones, so system dirs go first and the user dir last.
    // ------------------------------------------------------------------

    DesktopEntry entries[MAX_AUTOSTART];
    int entry_count = 0;

    // System autostart directories
    const char *config_dirs = getenv("XDG_CONFIG_DIRS");
    if (!config_dirs || config_dirs[0] == '\0') {
        config_dirs = "/etc/xdg";
    }

    // Parse the colon-separated list.  Per the XDG Base Directory spec,
    // earlier entries in $XDG_CONFIG_DIRS have higher precedence.  Since
    // scan_autostart_dir overwrites on filename collision, we scan in
    // REVERSE order so that the highest-priority system dir's entries
    // survive as the final values.  The user dir is scanned last and
    // always wins.
    {
        char dirs_buf[PATH_MAX * 4];
        snprintf(dirs_buf, sizeof(dirs_buf), "%s", config_dirs);

        char *sys_dirs[32];
        int sys_dir_count = 0;

        char *saveptr = NULL;
        for (char *tok = strtok_r(dirs_buf, ":", &saveptr);
             tok && sys_dir_count < 32;
             tok = strtok_r(NULL, ":", &saveptr))
        {
            sys_dirs[sys_dir_count++] = tok;
        }

        for (int i = sys_dir_count - 1; i >= 0; i--) {
            char autostart_path[PATH_MAX];
            snprintf(autostart_path, sizeof(autostart_path),
                     "%s/autostart", sys_dirs[i]);
            scan_autostart_dir(autostart_path, entries, &entry_count, MAX_AUTOSTART);
        }
    }

    // User autostart directory (always wins over system)
    {
        const char *config_home = getenv("XDG_CONFIG_HOME");
        char user_dir[PATH_MAX];

        if (config_home && config_home[0]) {
            snprintf(user_dir, sizeof(user_dir), "%s/autostart", config_home);
        } else {
            const char *home = get_user_home_dir();
            snprintf(user_dir, sizeof(user_dir), "%s/.config/autostart", home);
        }

        scan_autostart_dir(user_dir, entries, &entry_count, MAX_AUTOSTART);
    }

    log_info("xdg-autostart: found %d desktop entries", entry_count);

    // ------------------------------------------------------------------
    // 2. Filter and launch.
    // ------------------------------------------------------------------
    int launched = 0;

    for (int i = 0; i < entry_count; i++) {
        DesktopEntry *e = &entries[i];

        if (!should_run_entry(e, desktop)) {
            log_info("xdg-autostart: skip %s (%s)",
                     e->filename, e->name[0] ? e->name : "?");
            continue;
        }

        log_info("xdg-autostart: launching %s (%s) -> %s",
                 e->filename, e->name, e->exec);

        pid_t pid = exec_desktop_entry(e, display_name);
        if (pid >= 0) {
            launched++;
        }
    }

    log_info("xdg-autostart: launched %d/%d entries", launched, entry_count);

    // ------------------------------------------------------------------
    // 3. Stellar-specific autostart: plain executables.
    //    ~/.config/stellar/autostart/ holds scripts and binaries that are
    //    specific to Stellar.  Any regular file with +x is executed.
    //    .desktop files here are also honored (parsed & run without
    //    OnlyShowIn filtering, since placement in this dir is explicit).
    // ------------------------------------------------------------------
    run_stellar_autostart(st, display_name);
}
