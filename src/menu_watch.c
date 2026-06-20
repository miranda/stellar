// menu_watch.c
// inotify watcher for XDG applications directories. See menu_watch.h.
//
// Overhead profile:
//   - One inotify instance, a handful of directory watches (one per existing
//     applications/ dir, typically 2-4). No recursion, no per-file watches.
//   - The fd costs nothing until the kernel makes it readable. No thread, no
//     polling: it rides the existing select() loop.
//   - A package install touching many .desktop files produces a burst of
//     events; the debounce collapses them into a single rebuild.

#include "stellar.h"
#include "menu_watch.h"
#include "xdg_menu.h"          // rebuild_and_broadcast_menu

#include <sys/inotify.h>

// Debounce: wait this long after the LAST observed change before rebuilding,
// so a multi-file install/upgrade triggers one rebuild rather than dozens.
#define MENU_WATCH_DEBOUNCE_MS 1000

// We care about events that add, remove, rename, or modify entries in a watched
// directory. IN_CLOSE_WRITE catches a file finished being written (the common
// "installed a .desktop" case); the move/create/delete events catch package
// managers that rename temp files into place or remove entries.
#define MENU_WATCH_MASK \
    (IN_CLOSE_WRITE | IN_CREATE | IN_DELETE | IN_MOVED_TO | IN_MOVED_FROM)

#define MENU_WATCH_MAX_DIRS 40

// inotify event buffer. Events are variable-length; this holds a healthy batch.
#define MENU_WATCH_BUFSZ (64 * (sizeof(struct inotify_event) + NAME_MAX + 1))

static int    s_ifd = -1;                 // inotify instance fd
static int    s_wds[MENU_WATCH_MAX_DIRS]; // watch descriptors (for cleanup)
static int    s_nwd = 0;

static bool      s_dirty = false;         // a change is pending a rebuild
static long long s_last_event_ms = 0;     // monotonic ms of last event

// Monotonic milliseconds. CLOCK_MONOTONIC so debounce timing is immune to wall
// clock changes (NTP steps, etc.).
static long long now_ms(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (long long)ts.tv_sec * 1000LL + ts.tv_nsec / 1000000LL;
}

// Add a watch on one directory if it exists. Silently skips non-existent dirs
// (many systems won't have, e.g., ~/.local/share/applications until something
// creates it - see the note in menu_watch_init about that gap).
static void add_watch(const char *dir) {
    if (s_nwd >= MENU_WATCH_MAX_DIRS) {
        return;
    }
    int wd = inotify_add_watch(s_ifd, dir, MENU_WATCH_MASK);
    if (wd < 0) {
        // ENOENT is expected and not worth warning about; other errors are.
        if (errno != ENOENT) {
            log_error("menu-watch: cannot watch %s: %s", dir, strerror(errno));
        }
        return;
    }
    s_wds[s_nwd++] = wd;
    log_info("menu-watch: watching %s (wd=%d)", dir, wd);
}

int menu_watch_init(StellarState *st) {
    (void)st;

    s_ifd = inotify_init1(IN_NONBLOCK | IN_CLOEXEC);
    if (s_ifd < 0) {
        log_error("menu-watch: inotify_init1 failed: %s", strerror(errno));
        return -1;
    }

    // Same directory set the menu builder scans: $XDG_DATA_HOME/applications
    // plus each $XDG_DATA_DIRS/applications. We don't need precedence order
    // here (we're only detecting "something changed"), just coverage.
    const char *data_home = getenv("XDG_DATA_HOME");
    if (data_home && data_home[0]) {
        char path[PATH_MAX];
        snprintf(path, sizeof(path), "%s/applications", data_home);
        // Ensure it exists so the first user-created .desktop is caught. If the
        // dir is absent we'd have nothing to watch and would miss that first
        // creation (the gap KDE closes by also watching parent dirs). Creating
        // it up front is simpler and harmless. mkdir failures are non-fatal.
        mkdir(path, 0755);
        add_watch(path);
    } else {
        const char *home = get_user_home_dir();
        char path[PATH_MAX];
        snprintf(path, sizeof(path), "%s/.local/share/applications", home);
        mkdir(path, 0755);   // see note above
        add_watch(path);
    }

    const char *data_dirs = getenv("XDG_DATA_DIRS");
    if (!data_dirs || !data_dirs[0]) {
        data_dirs = "/usr/local/share:/usr/share";
    }
    char buf[PATH_MAX * 8];
    snprintf(buf, sizeof(buf), "%s", data_dirs);
    char *saveptr = NULL;
    for (char *t = strtok_r(buf, ":", &saveptr); t; t = strtok_r(NULL, ":", &saveptr)) {
        char path[PATH_MAX];
        snprintf(path, sizeof(path), "%s/applications", t);
        add_watch(path);
    }

    if (s_nwd == 0) {
        log_error("menu-watch: no applications directories to watch; disabling");
        close(s_ifd);
        s_ifd = -1;
        return -1;
    }

    log_info("menu-watch: initialized with %d watch(es)", s_nwd);
    return s_ifd;
}

int menu_watch_fd(void) {
    return s_ifd;
}

void menu_watch_handle_readable(StellarState *st) {
    (void)st;
    if (s_ifd < 0) {
        return;
    }

    // Drain ALL pending events. We don't inspect them individually - any event
    // on a watched applications dir means "the menu might have changed", and
    // the rescan in rebuild_and_broadcast_menu re-derives the truth from disk.
    // Reading fully is important so the fd doesn't stay perpetually readable.
    char buf[MENU_WATCH_BUFSZ]
        __attribute__((aligned(__alignof__(struct inotify_event))));

    for (;;) {
        ssize_t n = read(s_ifd, buf, sizeof(buf));
        if (n <= 0) {
            if (n < 0 && errno != EAGAIN && errno != EWOULDBLOCK) {
                log_error("menu-watch: read failed: %s", strerror(errno));
            }
            break;   // EAGAIN: drained. 0: nothing. either way, stop.
        }
        // We could iterate the records here, but there's nothing per-record to
        // do - the whole point is "rescan on any change". Just keep draining.
    }

    s_dirty = true;
    s_last_event_ms = now_ms();
}

void menu_watch_tick(StellarState *st) {
    if (!s_dirty) {
        return;
    }
    if (now_ms() - s_last_event_ms < MENU_WATCH_DEBOUNCE_MS) {
        return;   // still within the quiet period; wait for the burst to settle
    }

    s_dirty = false;
    log_info("menu-watch: change settled, rebuilding application menu");
    rebuild_and_broadcast_menu(st);
}

void menu_watch_cleanup(void) {
    if (s_ifd < 0) {
        return;
    }
    for (int i = 0; i < s_nwd; i++) {
        inotify_rm_watch(s_ifd, s_wds[i]);
    }
    close(s_ifd);
    s_ifd = -1;
    s_nwd = 0;
    s_dirty = false;
}
