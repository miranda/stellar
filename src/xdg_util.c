// xdg_util.c
// Shared Desktop Entry parsing primitives. See xdg_util.h.
//
// Implementations are taken from the original xdg_autostart.c versions (the
// spec-conservative field-code handling that preserves unknown %-sequences),
// so consolidating here does not change autostart behavior. The menu module
// previously dropped unknown codes; adopting these shared versions aligns it
// with the spec-correct behavior.

#include "xdg_util.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <limits.h>

bool xdg_in_semicolon_list(const char *haystack, const char *needle) {
    if (!haystack || !haystack[0] || !needle || !needle[0]) {
        return false;
    }

    size_t needle_len = strlen(needle);
    const char *p = haystack;

    while (*p) {
        const char *semi = strchr(p, ';');
        size_t span = semi ? (size_t)(semi - p) : strlen(p);

        if (span == needle_len && strncmp(p, needle, needle_len) == 0) {
            return true;
        }

        if (!semi) {
            break;
        }
        p = semi + 1;
    }

    return false;
}

bool xdg_command_exists(const char *name) {
    if (!name || !name[0]) {
        return false;
    }

    // Absolute or relative path - check directly.
    if (strchr(name, '/')) {
        return access(name, X_OK) == 0;
    }

    const char *path_env = getenv("PATH");
    if (!path_env) {
        path_env = "/usr/local/bin:/usr/bin:/bin";
    }

    char pathbuf[PATH_MAX];
    snprintf(pathbuf, sizeof(pathbuf), "%s", path_env);

    char *saveptr = NULL;
    for (char *dir = strtok_r(pathbuf, ":", &saveptr);
         dir != NULL;
         dir = strtok_r(NULL, ":", &saveptr))
    {
        char candidate[PATH_MAX];
        snprintf(candidate, sizeof(candidate), "%s/%s", dir, name);
        if (access(candidate, X_OK) == 0) {
            return true;
        }
    }

    return false;
}

void xdg_strip_field_codes(char *exec) {
    char buf[XDG_DE_VALUE];
    size_t out = 0;

    for (size_t i = 0; exec[i] && out < sizeof(buf) - 1; ) {
        if (exec[i] == '%' && exec[i + 1]) {
            char code = exec[i + 1];
            switch (code) {
                case '%':
                    buf[out++] = '%';
                    break;
                case 'f': case 'F':
                case 'u': case 'U':
                case 'd': case 'D':
                case 'n': case 'N':
                case 'v': case 'm':
                case 'i': case 'c': case 'k':
                    // Drop the field code entirely. %i could expand to
                    // --icon NAME, but neither autostart nor a menu launch
                    // supplies a file/URL, so this is never meaningful here.
                    break;
                default:
                    // Unknown %-sequence - keep it as-is.
                    buf[out++] = exec[i];
                    buf[out++] = exec[i + 1];
                    break;
            }
            i += 2;
        } else {
            buf[out++] = exec[i++];
        }
    }
    buf[out] = '\0';

    // Collapse runs of spaces left behind by removed codes.
    size_t dst = 0;
    bool prev_space = false;
    for (size_t s = 0; buf[s]; s++) {
        if (buf[s] == ' ') {
            if (!prev_space) {
                exec[dst++] = ' ';
            }
            prev_space = true;
        } else {
            exec[dst++] = buf[s];
            prev_space = false;
        }
    }
    exec[dst] = '\0';

    // Trim trailing space.
    while (dst > 0 && exec[dst - 1] == ' ') {
        exec[--dst] = '\0';
    }
}
