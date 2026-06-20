#include "stellar_config.h" 
#include <stdlib.h>
#include <unistd.h>
#include <pwd.h>
#include <stdio.h>
#include <limits.h>

const char *get_user_home_dir(void) {
    // 1. Try the HOME environment variable first (standard for shells and containers)
    const char *home = getenv("HOME");
    if (home && home[0] != '\0') {
        return home;
    }

    // 2. Fallback to querying the password database via UID
    struct passwd *pw = getpwuid(getuid());
    if (pw && pw->pw_dir && pw->pw_dir[0] != '\0') {
        return pw->pw_dir;
    }

    // 3. Failed to find it
    return NULL;
}

// Round raw DPI down to the nearest supported step
int snap_dpi_down(int raw_dpi) {
    for (int i = NUM_DPI_STEPS - 1; i >= 0; i--) {
        if (raw_dpi >= dpi_steps[i]) return dpi_steps[i];
    }
    return 48;
}

const char* get_stellar_theme_dir(char *out_path, size_t out_size, const char *theme_name) {
    const char *home = get_user_home_dir();
    char dir_candidate[PATH_MAX];
    char file_candidate[PATH_MAX];

    // Define the base search paths in order of priority
    const char *base_paths[3] = {0};
    char user_local[PATH_MAX] = {0};

    // 1. User local data (~/.local/share/stellar/themes)
    if (home) {
        snprintf(user_local, sizeof(user_local), "%s/.local/share/stellar", home);
        base_paths[0] = user_local;
    }

    // 2. System local administrator overrides
    base_paths[1] = "/usr/local/share/stellar";

    // 3. System-wide package manager installations
    // (Assumes STELLAR_SHARE_PATH is defined at compile time, e.g., "/usr/share/stellar")
    base_paths[2] = STELLAR_SHARE_PATH;

    // Iterate through the hierarchy
    for (int i = 0; i < 3; i++) {
        if (!base_paths[i] || base_paths[i][0] == '\0') continue;

        // Construct the directory path (e.g., /usr/share/stellar/themes/stellar-blue)
        snprintf(dir_candidate, sizeof(dir_candidate), "%s/themes/%s", base_paths[i], theme_name);

        // Construct the full file path to test (e.g., /usr/share/stellar/themes/stellar-blue/theme_data.lua)
        snprintf(file_candidate, sizeof(file_candidate), "%s/theme_data.lua", dir_candidate);

        // Verify the data file actually exists and is readable
        if (access(file_candidate, R_OK) == 0) {
            // It's valid! Copy ONLY the directory path to the output buffer
            snprintf(out_path, out_size, "%s", dir_candidate);
            fprintf(stderr, "[Stellar Config] Found valid theme '%s' at: %s\n", theme_name, out_path);
            return out_path;
        }
    }

    fprintf(stderr, "[Stellar Config] Error: Could not find a valid '%s' theme in any standard directory.\n", theme_name);
    return NULL;
}
