// monitor.c

#include "stellar.h"
#include "monitor.h"

/* ---------- Rotation Helpers ---------- */

static Rotation rotation_from_config(const char *s) {
    if (!s || strcmp(s, "normal") == 0)   return RR_Rotate_0;
    if (strcmp(s, "left") == 0)           return RR_Rotate_90;
    if (strcmp(s, "inverted") == 0)       return RR_Rotate_180;
    if (strcmp(s, "right") == 0)          return RR_Rotate_270;
    return RR_Rotate_0;
}

static const char *rotation_to_string(Rotation r) {
    switch (r & 0x0F) {
        case RR_Rotate_90:  return "left";
        case RR_Rotate_180: return "inverted";
        case RR_Rotate_270: return "right";
        default:            return "normal";
    }
}

// Sort modes: largest resolution first, then highest refresh first
static int compare_modes(const void *a, const void *b) {
    const struct { int width; int height; int refresh_mhz; } *ma = a, *mb = b;
    int area_a = ma->width * ma->height;
    int area_b = mb->width * mb->height;
    if (area_b != area_a) return area_b - area_a;
    if (mb->width != ma->width) return mb->width - ma->width;
    return mb->refresh_mhz - ma->refresh_mhz;
}

void extract_monitor_name(const unsigned char *edid, size_t length, char *out_name, size_t max_len) {
    snprintf(out_name, max_len, "Generic-Monitor");
    if (!edid || length < 128) return;

    for (int i = 0; i < 4; i++) {
        int offset = 54 + (i * 18);
        if (edid[offset] == 0x00 && edid[offset+1] == 0x00 && edid[offset+2] == 0x00) {
            if (edid[offset+3] == 0xFC) {
                int j;
                for (j = 0; j < 13; j++) {
                    if (edid[offset + 5 + j] == 0x0A) break;
                    out_name[j] = edid[offset + 5 + j];
                }
                out_name[j] = '\0';
                return;
            }
        }
    }
}

bool monitor_disable_crtc(StellarState *st, int screen_idx) {
    ScreenState *sc = &st->screens[screen_idx];

	// Get the hardware resources for this specific root window
    XRRScreenResources *res = XRRGetScreenResources(st->dpy, sc->root);
    if (!res) {
        log_error("Screen %d: Failed to get RandR resources", screen_idx);
        return false;
    }

    // Focus safety net (save)
    Window focused_win;
    int revert_to;
    XGetInputFocus(st->dpy, &focused_win, &revert_to);
    
	bool succeeded = false;

    // Find the active CRTC driving this screen
    for (int i = 0; i < res->ncrtc; i++) {
        XRRCrtcInfo *crtc_info = XRRGetCrtcInfo(st->dpy, res, res->crtcs[i]);
        if (!crtc_info) continue;

        // If it has a mode and outputs, it's our active CRTC
        if (crtc_info->mode != None && crtc_info->noutput > 0) {
            
            // Cache the state
            sc->saved_crtc = res->crtcs[i];
            sc->saved_mode = crtc_info->mode;
            sc->saved_rotation = crtc_info->rotation;
            sc->saved_x = crtc_info->x;
            sc->saved_y = crtc_info->y;
            sc->saved_noutput = crtc_info->noutput;
            
            for (int j = 0; j < crtc_info->noutput && j < 8; j++) {
                sc->saved_outputs[j] = crtc_info->outputs[j];
            }

            // Kill the output
            // Pushing 'None' as the mode forces the monitor into DPMS standby
            Status s = XRRSetCrtcConfig(st->dpy, res, sc->saved_crtc, CurrentTime,
                                        0, 0, None, RR_Rotate_0, NULL, 0);
            
            if (s == Success) {
                log_info("Screen %d: CRTC disabled (Standby Mode)", screen_idx);
				succeeded = true;
            } else {
                log_error("Screen %d: Failed to disable CRTC", screen_idx);
            }

            XRRFreeCrtcInfo(crtc_info);
            break; // We found and killed the active CRTC, stop looping
        }
        XRRFreeCrtcInfo(crtc_info);
    }
    XRRFreeScreenResources(res);

    // Focus safety net (restore)
    if (focused_win != None) {
        XSetInputFocus(st->dpy, focused_win, revert_to, CurrentTime);
        XFlush(st->dpy);
    }

	return succeeded;
}

bool monitor_restore_crtc(StellarState *st, int screen_idx) {
    ScreenState *sc = &st->screens[screen_idx];

    // Safety check: ensure we actually have a saved state to restore
    if (sc->saved_mode == None || sc->saved_crtc == None) {
        log_error("Screen %d: No saved CRTC state to restore!", screen_idx);
        return false; 
    }

    // We use 'Current' here because we don't need to poll the hardware 
    // for new monitors, we just need the X11 resource handles.
    XRRScreenResources *res = XRRGetScreenResourcesCurrent(st->dpy, sc->root);
    if (!res) return false;

    // Focus safety net (save)
    Window focused_win;
    int revert_to;
    XGetInputFocus(st->dpy, &focused_win, &revert_to);

	bool succeeded = false;    

    // Blast the cache back to hardware
    Status s = XRRSetCrtcConfig(st->dpy, res, sc->saved_crtc, CurrentTime,
                                sc->saved_x, sc->saved_y, 
                                sc->saved_mode, sc->saved_rotation, 
                                sc->saved_outputs, sc->saved_noutput);

    if (s == Success) {
        log_info("Screen %d: CRTC restored successfully", screen_idx);
		succeeded = true;
    } else {
        log_error("Screen %d: Failed to restore CRTC config!", screen_idx);
    }

    XRRFreeScreenResources(res);

    // Focus safety net (restore)
    if (focused_win != None) {
        XSetInputFocus(st->dpy, focused_win, revert_to, CurrentTime);
        XFlush(st->dpy);
    }

	return succeeded;
}

/* ---------- Monitor Info Queries ---------- */

void monitor_update_screen_info(StellarState *st, int screen_idx) {
    if (screen_idx < 0 || screen_idx >= st->config.screen_count) return;

    ScreenState *sc = &st->screens[screen_idx];

    // Reset all monitor fields
    sc->monitor_name[0] = '\0';
    sc->output_name[0] = '\0';
    sc->edid_len = 0;
    sc->monitor_width = 0;
    sc->monitor_height = 0;
    sc->monitor_refresh_mhz = 0;
    sc->monitor_phys_width_mm = 0;
    sc->monitor_phys_height_mm = 0;
    sc->monitor_connected = false;
    sc->monitor_rotation = RR_Rotate_0;
    sc->mode_count = 0;

    // Full hardware poll - we want fresh data after a hotplug
    XRRScreenResources *res = XRRGetScreenResources(st->dpy, sc->root);
    if (!res) {
        log_error("Screen %d: failed to get RandR resources for monitor info", screen_idx);
        snprintf(sc->monitor_name, sizeof(sc->monitor_name), "Unknown");
        return;
    }

    // In Zaphod-head mode each X screen typically has one active output.
    // Walk all outputs: prefer the first connected output that has an active
    // CRTC; fall back to the first connected output without one.
    int best_output = -1;

    for (int i = 0; i < res->noutput; i++) {
        XRROutputInfo *out = XRRGetOutputInfo(st->dpy, res, res->outputs[i]);
        if (!out) continue;

        if (out->connection == RR_Connected) {
            if (best_output < 0) best_output = i;
            if (out->crtc != None) {
                best_output = i;
                XRRFreeOutputInfo(out);
                break;
            }
        }
        XRRFreeOutputInfo(out);
    }

    if (best_output < 0) {
        XRRFreeScreenResources(res);
        snprintf(sc->monitor_name, sizeof(sc->monitor_name), "No Monitor");
        log_info("Screen %d: no connected output found", screen_idx);
        return;
    }

    XRROutputInfo *out = XRRGetOutputInfo(st->dpy, res, res->outputs[best_output]);
    if (!out) { XRRFreeScreenResources(res); return; }

    sc->monitor_connected = true;
    snprintf(sc->output_name, sizeof(sc->output_name), "%s", out->name);
    sc->monitor_phys_width_mm  = (int)out->mm_width;
    sc->monitor_phys_height_mm = (int)out->mm_height;

    // --- Resolution & refresh from the active CRTC ---
    if (out->crtc != None) {
        XRRCrtcInfo *crtc = XRRGetCrtcInfo(st->dpy, res, out->crtc);
        if (crtc) {
            for (int m = 0; m < res->nmode; m++) {
                if (res->modes[m].id == crtc->mode) {
                    XRRModeInfo *mode = &res->modes[m];
                    sc->monitor_width  = (int)mode->width;
                    sc->monitor_height = (int)mode->height;

                    if (mode->hTotal > 0 && mode->vTotal > 0) {
                        sc->monitor_refresh_mhz = (int)(
                            (double)mode->dotClock /
                            ((double)mode->hTotal * (double)mode->vTotal)
                            * 1000.0 + 0.5
                        );
                    }
                    break;
                }
            }
            sc->monitor_rotation = crtc->rotation;
            XRRFreeCrtcInfo(crtc);
        }
    }

    // --- Read EDID property ---
    Atom edid_atom = XInternAtom(st->dpy, "EDID", True);
    if (edid_atom != None) {
        Atom actual_type;
        int actual_format;
        unsigned long nitems, bytes_after;
        unsigned char *prop = NULL;

        if (XRRGetOutputProperty(st->dpy, res->outputs[best_output],
                                  edid_atom, 0, 512, False, False,
                                  AnyPropertyType, &actual_type,
                                  &actual_format, &nitems,
                                  &bytes_after, &prop) == Success && prop) {
            size_t len = nitems;
            if (len > sizeof(sc->edid)) len = sizeof(sc->edid);
            memcpy(sc->edid, prop, len);
            sc->edid_len = len;

            extract_monitor_name(sc->edid, sc->edid_len,
                                 sc->monitor_name, sizeof(sc->monitor_name));
            XFree(prop);
        }
    }

    // --- Collect available modes for this output ---
    for (int m = 0; m < out->nmode && sc->mode_count < MAX_MONITOR_MODES; m++) {
        // Look up the mode info in the screen resources
        for (int r = 0; r < res->nmode; r++) {
            if (res->modes[r].id != out->modes[m]) continue;

            XRRModeInfo *mode = &res->modes[r];
            int w = (int)mode->width;
            int h = (int)mode->height;
            int rmhz = 0;

            if (mode->hTotal > 0 && mode->vTotal > 0) {
                rmhz = (int)(
                    (double)mode->dotClock /
                    ((double)mode->hTotal * (double)mode->vTotal)
                    * 1000.0 + 0.5
                );
            }

            // Deduplicate: skip if same WxH @ same rounded Hz already exists
            int rhz = (rmhz + 500) / 1000;
            bool dup = false;
            for (int d = 0; d < sc->mode_count; d++) {
                int ehz = (sc->modes[d].refresh_mhz + 500) / 1000;
                if (sc->modes[d].width == w && sc->modes[d].height == h && ehz == rhz) {
                    dup = true;
                    break;
                }
            }
            if (!dup) {
                sc->modes[sc->mode_count].width = w;
                sc->modes[sc->mode_count].height = h;
                sc->modes[sc->mode_count].refresh_mhz = rmhz;
                sc->mode_count++;
            }
            break;
        }
    }

    // Sort: largest resolution first, then highest refresh first
    if (sc->mode_count > 1) {
        qsort(sc->modes, sc->mode_count, sizeof(sc->modes[0]), compare_modes);
    }

    // Fallback name: use the output name if EDID didn't give us one
    if (sc->monitor_name[0] == '\0') {
        snprintf(sc->monitor_name, sizeof(sc->monitor_name), "%s", sc->output_name);
    }

    XRRFreeOutputInfo(out);
    XRRFreeScreenResources(res);

    log_info("Screen %d: monitor='%s' output='%s' %dx%d @%dmHz rot=%s phys=%dx%dmm modes=%d",
             screen_idx, sc->monitor_name, sc->output_name,
             sc->monitor_width, sc->monitor_height, sc->monitor_refresh_mhz,
             rotation_to_string(sc->monitor_rotation),
             sc->monitor_phys_width_mm, sc->monitor_phys_height_mm,
             sc->mode_count);
}

void monitor_update_all_screens(StellarState *st) {
    for (int i = 0; i < st->config.screen_count; i++) {
        monitor_update_screen_info(st, i);
    }
}

void monitor_build_screen_info_json(StellarState *st, char *buf, size_t buf_size) {
    int pos = 0;

    pos += snprintf(buf + pos, buf_size - pos,
        "{\"screen_count\":%d,\"screens\":[", st->config.screen_count);

    for (int i = 0; i < st->config.screen_count; i++) {
        ScreenState *sc = &st->screens[i];

        if (i > 0 && (size_t)pos < buf_size)
            buf[pos++] = ',';

        pos += snprintf(buf + pos, buf_size - pos,
            "{\"index\":%d,"
            "\"display\":\"%s\","
            "\"monitor_name\":\"%s\","
            "\"output_name\":\"%s\","
            "\"connected\":%s,"
            "\"width\":%d,"
            "\"height\":%d,"
            "\"refresh_mhz\":%d,"
            "\"rotation\":\"%s\","
            "\"phys_width_mm\":%d,"
            "\"phys_height_mm\":%d,"
            "\"dpi\":%d,"
            "\"edid\":\"",
            i,
            sc->display_name,
            sc->monitor_name,
            sc->output_name,
            sc->monitor_connected ? "true" : "false",
            sc->monitor_width,
            sc->monitor_height,
            sc->monitor_refresh_mhz,
            rotation_to_string(sc->monitor_rotation),
            sc->monitor_phys_width_mm,
            sc->monitor_phys_height_mm,
            sc->dpi);

        // Emit the live EDID as lowercase hex so the settings app can pass it to
        // check_monitor_mismatch() for swap detection. Bounds-checked against
        // remaining buffer (2 hex chars per byte).
        {
            static const char hexd[] = "0123456789abcdef";
            for (size_t e = 0; e < sc->edid_len && (size_t)pos + 2 < buf_size; e++) {
                buf[pos++] = hexd[sc->edid[e] >> 4];
                buf[pos++] = hexd[sc->edid[e] & 0x0F];
            }
        }

        pos += snprintf(buf + pos, buf_size - pos, "\",\"modes\":[");

        for (int m = 0; m < sc->mode_count && (size_t)pos < buf_size; m++) {
            if (m > 0) buf[pos++] = ',';
            pos += snprintf(buf + pos, buf_size - pos,
                "{\"w\":%d,\"h\":%d,\"r\":%d}",
                sc->modes[m].width, sc->modes[m].height, sc->modes[m].refresh_mhz);
        }

        pos += snprintf(buf + pos, buf_size - pos, "]}");
    }

    snprintf(buf + pos, buf_size - pos, "]}");
}

void monitor_apply_rotation(StellarState *st, int screen_idx) {
    if (screen_idx < 0 || screen_idx >= st->config.screen_count) return;

    ScreenState *sc = &st->screens[screen_idx];

    // If no rotation is explicitly configured, leave the current state alone.
    // This respects whatever xorg.conf or the X server set up.
    if (sc->config.rotation[0] == '\0') return;

    Rotation desired = rotation_from_config(sc->config.rotation);

    XRRScreenResources *res = XRRGetScreenResources(st->dpy, sc->root);
    if (!res) {
        log_error("Screen %d: failed to get RandR resources for rotation apply", screen_idx);
        return;
    }

    // Find the active CRTC (one with a mode and outputs assigned)
    for (int i = 0; i < res->ncrtc; i++) {
        XRRCrtcInfo *crtc = XRRGetCrtcInfo(st->dpy, res, res->crtcs[i]);
        if (!crtc) continue;

        if (crtc->mode == None || crtc->noutput == 0) {
            XRRFreeCrtcInfo(crtc);
            continue;
        }

        // Preserve any existing reflection bits, replace only rotation
        Rotation current_rot = crtc->rotation & 0x0F;
        Rotation current_ref = crtc->rotation & ~0x0F;
        Rotation combined = desired | current_ref;

        if (current_rot == desired) {
            // Already at the requested rotation
            XRRFreeCrtcInfo(crtc);
            break;
        }

        log_info("Screen %d: rotating %s -> %s",
                 screen_idx, rotation_to_string(current_rot),
                 rotation_to_string(desired));

        // Focus safety net (save)
        Window focused_win;
        int revert_to;
        XGetInputFocus(st->dpy, &focused_win, &revert_to);

        Status s = XRRSetCrtcConfig(st->dpy, res, res->crtcs[i], CurrentTime,
                                    crtc->x, crtc->y,
                                    crtc->mode, combined,
                                    crtc->outputs, crtc->noutput);

        if (s == Success) {
            log_info("Screen %d: rotation applied successfully", screen_idx);
            // Refresh cached monitor info to reflect the new state
            XRRFreeCrtcInfo(crtc);
            XRRFreeScreenResources(res);

            // Focus safety net (restore)
            if (focused_win != None) {
                XSetInputFocus(st->dpy, focused_win, revert_to, CurrentTime);
                XFlush(st->dpy);
            }

            monitor_update_screen_info(st, screen_idx);
            return;
        } else {
            log_error("Screen %d: XRRSetCrtcConfig failed for rotation", screen_idx);
        }

        // Focus safety net (restore)
        if (focused_win != None) {
            XSetInputFocus(st->dpy, focused_win, revert_to, CurrentTime);
            XFlush(st->dpy);
        }

        XRRFreeCrtcInfo(crtc);
        break;
    }

    XRRFreeScreenResources(res);
}

bool monitor_apply_tearfree(StellarState *st, int screen_idx) {
    ScreenState *sc = &st->screens[screen_idx];
    Display *dpy = st->dpy;

    Atom prop = XInternAtom(dpy, "TearFree", True);   // True = only-if-exists
    if (prop == None) {
        log_info("TearFree: no driver on this server exposes the property");
        return false;
    }

    XRRScreenResources *res = XRRGetScreenResources(dpy, sc->root);
    if (!res) return false;

    const char *val_str = sc->config.tearfree_enabled ? "on" : "off";
    Atom val_atom = XInternAtom(dpy, val_str, False);
    bool applied = false;

    for (int o = 0; o < res->noutput; o++) {
        XRROutputInfo *oi = XRRGetOutputInfo(dpy, res, res->outputs[o]);
        if (!oi) continue;

        // Only this screen's output, and only if connected & active.
        bool is_ours = (sc->output_name[0] != '\0' && oi->name &&
                        strcmp(oi->name, sc->output_name) == 0);

        if (is_ours && oi->connection == RR_Connected && oi->crtc != None) {
            // Confirm THIS output's driver actually has the property.
            int nprop = 0;
            Atom *props = XRRListOutputProperties(dpy, res->outputs[o], &nprop);
            bool has_it = false;
            for (int p = 0; p < nprop; p++)
                if (props[p] == prop) { has_it = true; break; }
            if (props) XFree(props);

            if (has_it) {
                XRRChangeOutputProperty(dpy, res->outputs[o], prop,
                                        XA_ATOM, 32, PropModeReplace,
                                        (unsigned char *)&val_atom, 1);
                applied = true;
                log_info("TearFree %s on output %s (screen %d)",
                         val_str, oi->name, screen_idx);
            } else {
                log_info("TearFree: output %s driver lacks the property",
                         oi->name);
            }
        }
        XRRFreeOutputInfo(oi);
    }

    XRRFreeScreenResources(res);
    XFlush(dpy);
    return applied;
}

void monitor_apply_all_tearfree(StellarState *st) {
    for (int i = 0; i < st->config.screen_count; i++)
        monitor_apply_tearfree(st, i);
}

void monitor_apply_all_rotations(StellarState *st) {
    for (int i = 0; i < st->config.screen_count; i++) {
        monitor_apply_rotation(st, i);
    }
}
