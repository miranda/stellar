// power.c

#include "stellar.h"
#include "power.h"
#include "ipc_lua.h"
#include "monitor.h"

static void enforce_x11_power_settings(StellarState *st) {
    // --- Crush the Core X11 Screensaver ---
    // xset s off
    int timeout, interval, prefer_blanking, allow_exposures;
    XGetScreenSaver(st->dpy, &timeout, &interval, &prefer_blanking, &allow_exposures);
    
    if (timeout != 0) {
        log_info("Rogue program enabled X11 core screensaver. Crushing it.");
        XSetScreenSaver(st->dpy, 0, 0, DontPreferBlanking, DefaultExposures);
    }

    // --- Crush DPMS ---
    // xset -dpms
    int dummy;
    if (DPMSQueryExtension(st->dpy, &dummy, &dummy)) {
        BOOL dpms_state;
        CARD16 power_level;
        
        if (DPMSInfo(st->dpy, &power_level, &dpms_state)) {
            if (dpms_state) {
                log_info("Rogue program enabled DPMS. Crushing it.");
                DPMSDisable(st->dpy);
            }
        }
    }
}

static void kill_screensaver(StellarState *st, int screen_idx) {
    ScreenState *sc = &st->screens[screen_idx];
    
    // Only send the stop command if it was actually running
    if (sc->power_state == POWER_STATE_SAVER) {
        char line[128];
        snprintf(line, sizeof(line), "SAVER_STOP screen=%d", screen_idx);
        broadcast_line(st, line);
    }
}

static void trigger_screensaver(StellarState *st, int screen_idx) {
    ScreenState *sc = &st->screens[screen_idx];
    
    if (sc->power_state == POWER_STATE_ON) {
        log_info("Screen %d: Sending SAVER_START to daemon", screen_idx);
        
        char line[128];
        snprintf(line, sizeof(line), "SAVER_START screen=%d", screen_idx);
        broadcast_line(st, line);
        
        sc->power_state = POWER_STATE_SAVER;
    }
}

static void wake_single_screen(StellarState *st, int screen_idx) {
    ScreenState *sc = &st->screens[screen_idx];

    // If it's already on, there's nothing to do.
    if (sc->power_state == POWER_STATE_ON) {
        return;
    }

    // Waking from a deep sleep (CRTC disabled)
    if (sc->power_state == POWER_STATE_OFF) {
        // Only update the state IF the hardware actually woke up
        if (monitor_restore_crtc(st, screen_idx)) {
            sc->power_state = POWER_STATE_ON;
            clock_gettime(CLOCK_MONOTONIC, &sc->last_activity); 
            log_info("Screen %d: CRTC restored, state is now ON", screen_idx);
        } else {
            log_error("Screen %d: Hardware failed to wake CRTC. Keeping state OFF.", screen_idx);
            // We intentionally leave it OFF so the next mouse movement can try again
        }
    } 
    
    // Waking from the screensaver
    else if (sc->power_state == POWER_STATE_SAVER) {
        kill_screensaver(st, screen_idx); 
        sc->power_state = POWER_STATE_ON;
        clock_gettime(CLOCK_MONOTONIC, &sc->last_activity); 
        log_info("Screen %d: Screensaver killed, state is now ON", screen_idx);
    }
}

void check_idle_screens(StellarState *st) {
    struct timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now);

    // Throttle the X11 State Enforcement
    static time_t last_enforcement = 0;
    if (now.tv_sec - last_enforcement >= 5) {
        enforce_x11_power_settings(st);
        last_enforcement = now.tv_sec;
    }

    // The lowest elapsed time among all screens is our global idle time.
    double global_idle = 99999999.0; 
    for (int i = 0; i < st->config.screen_count; i++) {
        double elapsed = (now.tv_sec - st->screens[i].last_activity.tv_sec) + 
                         (now.tv_nsec - st->screens[i].last_activity.tv_nsec) / 1e9;
        if (elapsed < global_idle) {
            global_idle = elapsed;
        }
    }

	for (int i = 0; i < st->config.screen_count; i++) {
        ScreenState *sc = &st->screens[i];

        if (sc->power_state == POWER_STATE_OFF) continue; // Already dead

        double elapsed = (now.tv_sec - sc->last_activity.tv_sec) + 
                         (now.tv_nsec - sc->last_activity.tv_nsec) / 1e9;

		int timeout_dpms_sec = get_dpms_timeout_for_screen(st, i) * 60;
		int timeout_screensaver_sec = get_screensaver_timeout_for_screen(st, i) * 60;

        // Stage 2: Display sleep (CRTC off)
        bool global_dpms_triggered = (global_idle >= timeout_dpms_sec);
        bool local_dpms_triggered  = (sc->config.independent_dpms && elapsed >= timeout_dpms_sec);

        if (timeout_dpms_sec > 0 &&
			(global_dpms_triggered || local_dpms_triggered)) {
            log_info("Screen %d: DPMS timeout reached. Powering off.", i);
            kill_screensaver(st, i);    
            
			if (monitor_disable_crtc(st, i)) {
                sc->power_state = POWER_STATE_OFF;
            } else {
                log_error("Screen %d: Failed to suspend CRTC, keeping state ON", i);
            }
        } 
        // Stage 1: Screensaver
        else if (st->config.saver_enabled && 
				 timeout_screensaver_sec > 0 &&
                 elapsed >= timeout_screensaver_sec && 
                 sc->power_state == POWER_STATE_ON) {
            
            log_info("Screen %d: Screensaver timeout reached.", i);
            trigger_screensaver(st, i);
            
            // Change the state so we don't spam the IPC socket on the next tick
            sc->power_state = POWER_STATE_SAVER;
        }
    }
}

void reset_idle_timer(StellarState *st) {
    if (st->pointer_screen < 0 || st->pointer_screen >= st->config.screen_count) return;
    
    int active_idx = st->pointer_screen;
    ScreenState *active_sc = &st->screens[active_idx];

    // Always update the timer for the screen the pointer is on
    clock_gettime(CLOCK_MONOTONIC, &active_sc->last_activity);

    // Count how many screens are currently ON
    int screens_awake = 0;
    for (int i = 0; i < st->config.screen_count; i++) {
        if (st->screens[i].power_state == POWER_STATE_ON) {
            screens_awake++;
        }
    }

    if (screens_awake == 0) {
        // --- GLOBAL WAKE SCENARIO ---
        // Everything was asleep. Wake them all, respecting the override flag.
        for (int i = 0; i < st->config.screen_count; i++) {
            ScreenState *sc = &st->screens[i];
            
            // Wake it if it's the screen we just touched, OR if it allows global wakes
            if (i == active_idx || !sc->config.require_explicit_wake) {
                wake_single_screen(st, i);
            }
        }
    } else {
        // --- LOCAL WAKE SCENARIO ---
        // At least one screen is in use. Only wake the specific screen the pointer is on.
        wake_single_screen(st, active_idx);
    }
}

void start_stellar_saver(StellarState *st) {
    if (!st->config.saver_enabled) {
        log_info("Stellar-saver is not enabled.");
        return;
    }

    if (st->saver_pid > 0) {
        log_info("Stellar-saver is already running with PID %d.", st->saver_pid);
        return;
    }

    time_t now = time(NULL);

    if (now - st->saver_respawn_start > 10) {
        st->saver_respawn_start = now;
        st->saver_respawn_count = 0;
    }

    st->saver_respawn_count++;

    if (st->saver_respawn_count > 3) {
        log_error(
            "CRITICAL: stellar-saver crash loop detected. "
            "Disabling stellar-saver globally to prevent lockup."
        );
        st->config.saver_enabled = false; 
        return; 
    }

    pid_t pid = fork();
    if (pid == 0) {
        // --- CHILD PROCESS ---
        
        // 1. Inject the IPC socket path
        setenv("STELLAR_SOCKET", st->socket_path, 1);
        
        // 2. Dynamically grab the exact display string the DE is currently using
        if (st->dpy) {
            setenv("DISPLAY", DisplayString(st->dpy), 1);
        }

        // 3. Launch the binary
    	char saver_path[PATH_MAX];
    	snprintf(saver_path, sizeof(saver_path), "%s/stellar-saver", STELLAR_LIBEXEC_PATH);
        execl(saver_path, "stellar-saver", NULL);
        
        // 4. If it fails, log it
        log_error("Failed to execute %s: %s", saver_path, strerror(errno));
        exit(1);
    } else if (pid > 0) {
        // Parent Process
        st->saver_pid = pid;
        log_info("Stellar-saver started (attempt %d, PID %d)", 
                 st->saver_respawn_count, pid);
    } else {
        // Fork Failed
        log_error("Failed to fork for stellar-saver: %s", strerror(errno));
    }
}

void stop_stellar_saver(StellarState *st) {
    if (st->saver_pid > 0) {
        log_info("Stopping stellar-saver...");
        log_info("Stopping stellar-saver, PID %d)", 
                 st->saver_pid);
        kill(st->saver_pid, SIGTERM);
    }
}
