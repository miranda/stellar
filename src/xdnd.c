// xdnd.c

#include "stellar.h"
#include "xdnd.h"

// ---- XDND ----

void init_xdnd_atoms(StellarState *st) {
    st->xdnd_selection  = XInternAtom(st->dpy, "XdndSelection",  False);
    st->xdnd_aware      = XInternAtom(st->dpy, "XdndAware",      False);
    st->xdnd_enter      = XInternAtom(st->dpy, "XdndEnter",      False);
    st->xdnd_position   = XInternAtom(st->dpy, "XdndPosition",   False);
    st->xdnd_status     = XInternAtom(st->dpy, "XdndStatus",     False);
    st->xdnd_leave      = XInternAtom(st->dpy, "XdndLeave",      False);
    st->xdnd_drop       = XInternAtom(st->dpy, "XdndDrop",       False);
    st->xdnd_finished   = XInternAtom(st->dpy, "XdndFinished",   False);
    st->xdnd_action_copy= XInternAtom(st->dpy, "XdndActionCopy", False);
    st->xdnd_type_list  = XInternAtom(st->dpy, "XdndTypeList",   False);
    st->text_uri_list   = XInternAtom(st->dpy, "text/uri-list",  False);
    st->xdnd_proxy_prop = XInternAtom(st->dpy, "STELLAR_XDND_DATA", False);

    st->xdnd_proxy.state = XDND_PROXY_IDLE;
    st->xdnd_proxy.uri_data = NULL;
    st->xdnd_proxy.uri_len = 0;
    st->xdnd_proxy.current_target = None;
    st->xdnd_proxy.shield_screen = -1;
}

void create_xdnd_proxy_windows(StellarState *st) {
    Atom xdnd_version = 5;

    for (int i = 0; i < st->config.screen_count; i++) {
        Window root = st->screens[i].root;

        XSetWindowAttributes attrs;
        attrs.override_redirect = True;

        st->xdnd_proxy_win[i] = XCreateWindow(
            st->dpy, root,
            0, 0, 1, 1, 0,
            CopyFromParent, InputOnly, CopyFromParent,
            CWOverrideRedirect, &attrs
        );

        XChangeProperty(st->dpy, st->xdnd_proxy_win[i],
                        st->xdnd_aware, XA_ATOM, 32,
                        PropModeReplace,
                        (unsigned char *)&xdnd_version, 1);

        XSelectInput(st->dpy, st->xdnd_proxy_win[i], PropertyChangeMask);
        XMapWindow(st->dpy, st->xdnd_proxy_win[i]);

        // Drop-shield: a screen-covering InputOnly window, created now but
        // mapped only while a proxy drag is active (and only on the source
        // screen).  It advertises XdndAware so that the original drag
        // source - which is still running its own XDND session against its
        // own root window - resolves *us* as the drop target instead of an
        // innocent bystander (terminal, its own file pane, ...).  We answer
        // its session with XdndStatus/XdndFinished and discard the drop.
        // Oversized so RandR mode changes can never leave a gap.
        st->xdnd_shield_win[i] = XCreateWindow(
            st->dpy, root,
            0, 0, 32767, 32767, 0,
            CopyFromParent, InputOnly, CopyFromParent,
            CWOverrideRedirect, &attrs
        );
        XChangeProperty(st->dpy, st->xdnd_shield_win[i],
                        st->xdnd_aware, XA_ATOM, 32,
                        PropModeReplace,
                        (unsigned char *)&xdnd_version, 1);

        log_info("xdnd: proxy window 0x%lx, shield window 0x%lx on screen %d",
                 st->xdnd_proxy_win[i], st->xdnd_shield_win[i], i);
    }
    XFlush(st->dpy);
}

// Map the drop-shield over the source screen for the duration of a proxy
// drag.  Raised above everything so the source's XdndAware tree-walk hits it
// first at any coordinate.
static void xdnd_shield_map(StellarState *st, int screen_idx) {
    XdndProxy *px = &st->xdnd_proxy;
    XMapRaised(st->dpy, st->xdnd_shield_win[screen_idx]);
    px->shield_screen = screen_idx;
    log_info("xdnd: shield mapped on screen %d", screen_idx);
}

// Unmap the shield as soon as the physical button release happens (or the
// drag aborts).  This must not wait for XdndFinished from the *real* target:
// a browser can hold the session open for many seconds while it processes an
// upload, and an InputOnly window left mapped would eat every click on the
// source screen.  The shield window itself stays alive forever, so a late
// XdndDrop ClientMessage from the source still reaches us after unmapping.
static void xdnd_shield_unmap(StellarState *st) {
    XdndProxy *px = &st->xdnd_proxy;
    if (px->shield_screen >= 0) {
        XUnmapWindow(st->dpy, st->xdnd_shield_win[px->shield_screen]);
        XFlush(st->dpy);
        log_info("xdnd: shield unmapped on screen %d", px->shield_screen);
        px->shield_screen = -1;
    }
}

// Walk the window tree to find the deepest XdndAware window under (root_x, root_y).
static Window find_xdnd_window(StellarState *st, Window root, int root_x, int root_y) {
    Window child = None;
    int dest_x, dest_y;

    // Get the direct child of root at this point
    if (!XTranslateCoordinates(st->dpy, root, root,
                               root_x, root_y, &dest_x, &dest_y, &child)) {
        return None;
    }
    if (child == None) return None;

    // Walk down, checking each level for XdndAware
    Window current = child;
    Window best = None;

    for (int depth = 0; depth < 32; depth++) {
        Atom type = None;
        int format;
        unsigned long nitems, bytes_after;
        unsigned char *data = NULL;

        if (XGetWindowProperty(st->dpy, current, st->xdnd_aware,
                               0, 1, False, XA_ATOM,
                               &type, &format, &nitems, &bytes_after,
                               &data) == Success) {
            if (data) XFree(data);
            if (type == XA_ATOM && nitems > 0) {
                best = current;
                // XDND spec: if a parent has XdndAware, it handles drops
                // for all children. Don't go deeper.
                return best;
            }
        }

        // Go one level deeper
        Window next = None;
        if (!XTranslateCoordinates(st->dpy, root, current,
                                   root_x, root_y, &dest_x, &dest_y, &next)) {
            break;
        }
        if (next == None || next == current) break;
        current = next;
    }

    return best;  // None if nothing was XdndAware
}

// Synchronously fetch XdndSelection data as text/uri-list.
// Returns malloc'd buffer in *out_data, length in *out_len.  
// Returns true on success.
static bool fetch_xdnd_data(StellarState *st, int screen_idx,
                            unsigned char **out_data, unsigned long *out_len) {
    Window proxy = st->xdnd_proxy_win[screen_idx];

    // Delete any stale property
    XDeleteProperty(st->dpy, proxy, st->xdnd_proxy_prop);

    // Ask the current XdndSelection owner to convert to text/uri-list
    XConvertSelection(st->dpy, st->xdnd_selection,
                      st->text_uri_list,
                      st->xdnd_proxy_prop,
                      proxy, CurrentTime);
    XFlush(st->dpy);

    // Spin-wait for SelectionNotify (up to 500ms)
    for (int i = 0; i < 50; i++) {
        XEvent ev;
        if (XCheckTypedWindowEvent(st->dpy, proxy, SelectionNotify, &ev)) {
            if (ev.xselection.property == None) {
                log_error("xdnd: selection conversion refused");
                return false;
            }

            // Read the property
            Atom type;
            int format;
            unsigned long nitems, bytes_after;
            unsigned char *data = NULL;

            XGetWindowProperty(st->dpy, proxy, st->xdnd_proxy_prop,
                               0, 1024 * 1024, True,  // delete after read
                               AnyPropertyType,
                               &type, &format, &nitems, &bytes_after, &data);

            if (!data || nitems == 0) {
                if (data) XFree(data);
                log_error("xdnd: selection property empty");
                return false;
            }

            // Copy out (XFree the X-allocated buffer)
            unsigned long total = nitems * (format / 8);
            *out_data = malloc(total + 1);
            if (!*out_data) {
                XFree(data);
                return false;
            }
            memcpy(*out_data, data, total);
            (*out_data)[total] = '\0';
            *out_len = total;
            XFree(data);

            log_info("xdnd: fetched %lu bytes of uri data", total);
            return true;
        }

        // Process other events so we don't block everything
        while (XPending(st->dpy) > 0) {
            XEvent other;
            XNextEvent(st->dpy, &other);
            // Re-check in case this was our SelectionNotify on a different path
            if (other.type == SelectionNotify &&
                other.xselection.requestor == proxy) {
                if (other.xselection.property == None) {
                    log_error("xdnd: selection conversion refused (alt path)");
                    return false;
                }
                // Same read logic
                Atom type;
                int format;
                unsigned long nitems, bytes_after;
                unsigned char *data = NULL;
                XGetWindowProperty(st->dpy, proxy, st->xdnd_proxy_prop,
                                   0, 1024 * 1024, True,
                                   AnyPropertyType,
                                   &type, &format, &nitems, &bytes_after, &data);
                if (!data || nitems == 0) {
                    if (data) XFree(data);
                    return false;
                }
                unsigned long total = nitems * (format / 8);
                *out_data = malloc(total + 1);
                if (!*out_data) { XFree(data); return false; }
                memcpy(*out_data, data, total);
                (*out_data)[total] = '\0';
                *out_len = total;
                XFree(data);
                log_info("xdnd: fetched %lu bytes (alt path)", total);
                return true;
            }
            // Keep serving selection requests during this sync wait.  A
            // previous drop target (e.g. a browser still processing an
            // upload) may convert XdndSelection right now; silently dropping
            // its SelectionRequest would leave it hanging.
            if (other.type == SelectionRequest) {
                xdnd_proxy_handle_selection_request(st, &other.xselectionrequest);
                continue;
            }
            // Silently drop other events during this sync wait.
            // In practice this is <500ms so nothing important is lost.
        }

        usleep(10000);  // 10ms
    }

    log_error("xdnd: timed out waiting for selection data");
    return false;
}

// Send XdndEnter to a target window.
static void xdnd_send_enter(StellarState *st, Window target) {
    XdndProxy *px = &st->xdnd_proxy;
    Window source = st->xdnd_proxy_win[px->target_screen];

    XEvent ev;
    memset(&ev, 0, sizeof(ev));
    ev.xclient.type         = ClientMessage;
    ev.xclient.window       = target;
    ev.xclient.message_type = st->xdnd_enter;
    ev.xclient.format       = 32;
    ev.xclient.data.l[0]    = source;                  // Source window
    ev.xclient.data.l[1]    = (5L << 24);              // Version 5, <3 types so bit0=0
    ev.xclient.data.l[2]    = st->text_uri_list;       // Type 1
    ev.xclient.data.l[3]    = None;                    // Type 2
    ev.xclient.data.l[4]    = None;                    // Type 3

    XSendEvent(st->dpy, target, False, NoEventMask, &ev);
}

// Send XdndPosition to a target window.
static void xdnd_send_position(StellarState *st, Window target,
                               int root_x, int root_y, Time timestamp) {
    XdndProxy *px = &st->xdnd_proxy;
    Window source = st->xdnd_proxy_win[px->target_screen];

    XEvent ev;
    memset(&ev, 0, sizeof(ev));
    ev.xclient.type         = ClientMessage;
    ev.xclient.window       = target;
    ev.xclient.message_type = st->xdnd_position;
    ev.xclient.format       = 32;
    ev.xclient.data.l[0]    = source;
    ev.xclient.data.l[1]    = 0;                                   // Reserved
    ev.xclient.data.l[2]    = (root_x << 16) | (root_y & 0xFFFF); // Coords
    ev.xclient.data.l[3]    = timestamp;
    ev.xclient.data.l[4]    = st->xdnd_action_copy;

    XSendEvent(st->dpy, target, False, NoEventMask, &ev);
}

// Send XdndLeave to a target window.
static void xdnd_send_leave(StellarState *st, Window target) {
    XdndProxy *px = &st->xdnd_proxy;
    Window source = st->xdnd_proxy_win[px->target_screen];

    XEvent ev;
    memset(&ev, 0, sizeof(ev));
    ev.xclient.type         = ClientMessage;
    ev.xclient.window       = target;
    ev.xclient.message_type = st->xdnd_leave;
    ev.xclient.format       = 32;
    ev.xclient.data.l[0]    = source;

    XSendEvent(st->dpy, target, False, NoEventMask, &ev);
}

// Send XdndDrop to a target window.
// (XdndSelection ownership was already claimed in xdnd_proxy_start.)
static void xdnd_send_drop(StellarState *st, Window target, Time timestamp) {
    XdndProxy *px = &st->xdnd_proxy;
    Window source = st->xdnd_proxy_win[px->target_screen];

    XEvent ev;
    memset(&ev, 0, sizeof(ev));
    ev.xclient.type         = ClientMessage;
    ev.xclient.window       = target;
    ev.xclient.message_type = st->xdnd_drop;
    ev.xclient.format       = 32;
    ev.xclient.data.l[0]    = source;
    ev.xclient.data.l[1]    = 0;           // Reserved
    ev.xclient.data.l[2]    = timestamp;

    XSendEvent(st->dpy, target, False, NoEventMask, &ev);
    XFlush(st->dpy);
}

// Clean up proxy state.
void xdnd_proxy_cleanup(StellarState *st) {
    XdndProxy *px = &st->xdnd_proxy;

    // Covers abort paths where xdnd_proxy_drop never ran (idempotent).
    xdnd_shield_unmap(st);

    // Only send XdndLeave if the session ended *before* the drop.
    // Per the XDND spec, once XdndDrop has been sent the session is
    // terminated by the target's XdndFinished - sending XdndLeave after a
    // drop is a protocol violation and can confuse targets that are still
    // processing the drop (e.g. a browser mid-upload).
    if (px->state == XDND_PROXY_DRAGGING && px->current_target != None) {
        xdnd_send_leave(st, px->current_target);
    }

    // Relinquish XdndSelection if we still own it.  Without this the proxy
    // window owns XdndSelection forever after a drop, which makes any
    // "is a drag in progress?" check based on XGetSelectionOwner() misfire
    // on every later screen crossing.  Guard on ownership so we never steal
    // the selection from a new drag that has already started.
    if (px->state != XDND_PROXY_IDLE &&
        XGetSelectionOwner(st->dpy, st->xdnd_selection) ==
            st->xdnd_proxy_win[px->target_screen]) {
        XSetSelectionOwner(st->dpy, st->xdnd_selection, None, CurrentTime);
    }

    if (px->uri_data) {
        free(px->uri_data);
        px->uri_data = NULL;
    }
    px->uri_len = 0;
    px->current_target = None;
    px->state = XDND_PROXY_IDLE;

    log_info("xdnd: proxy cleaned up");
}

// Called from handle_raw_motion when an XDND drag crosses the screen boundary.
// Fetches the drag data and enters proxy mode.
bool xdnd_proxy_start(StellarState *st, int source_screen, int target_screen,
                             int warp_x, int warp_y, Time event_time) {
    XdndProxy *px = &st->xdnd_proxy;

    // A previous drop can legitimately linger in DROPPED for many seconds:
    // browsers like Chrome only send XdndFinished once the page has finished
    // processing the drop (i.e. after the upload).  If the user has already
    // started a *new* drag, the old session is dead weight - finalize it and
    // proceed rather than refusing until the safety timeout fires.
    if (px->state == XDND_PROXY_DROPPED) {
        log_info("xdnd: new proxy drag while previous drop unfinished, finalizing old session");
        xdnd_proxy_cleanup(st);
    }

    // Already proxying a live drag?
    if (px->state != XDND_PROXY_IDLE) {
        log_error("xdnd: proxy_start called while state=%d", px->state);
        return false;
    }

    // Remember who owns XdndSelection right now
    px->source_owner = XGetSelectionOwner(st->dpy, st->xdnd_selection);
    if (px->source_owner == None) {
        log_error("xdnd: no XdndSelection owner at crossing");
        return false;
    }

    log_info("xdnd: initiating proxy drag from screen %d to %d (source owner 0x%lx)",
             source_screen, target_screen, px->source_owner);

    // Fetch the drag data from the source while it still owns the selection
    unsigned char *data = NULL;
    unsigned long len = 0;
    if (!fetch_xdnd_data(st, source_screen, &data, &len)) {
        log_error("xdnd: failed to fetch drag data, aborting proxy");
        return false;
    }

    // Stash it
    px->uri_data = data;
    px->uri_len = len;
    px->target_screen = target_screen;
    px->source_screen = source_screen;
    px->current_target = None;
    px->drag_time = event_time;

    // Take over XdndSelection NOW, not at drop time.
    //
    // This delivers a SelectionClear to the real drag source (the file
    // manager).  Well-behaved toolkits (GTK/Qt) treat losing XdndSelection
    // mid-drag as "my drag was taken over": they cancel their drag, release
    // their active pointer grab, and stop running a competing XDND session.
    //
    // Previously ownership was only claimed inside xdnd_send_drop(), which
    // left the source's drag alive for the whole proxy phase.  The source
    // still held its pointer grab and still saw the button release (grabs
    // are display-wide in Zaphod mode), so it ran its own confused
    // end-of-drag logic against a root window it didn't expect, and then
    // sat waiting on its own internal drop/reply timeout - which is the
    // few-second "can't click anything in the file browser" freeze.
    XSetSelectionOwner(st->dpy, st->xdnd_selection,
                       st->xdnd_proxy_win[target_screen], event_time);
    if (XGetSelectionOwner(st->dpy, st->xdnd_selection) !=
            st->xdnd_proxy_win[target_screen]) {
        log_error("xdnd: failed to acquire XdndSelection ownership");
        // Not fatal: we already hold a private copy of the data, so we can
        // still serve the drop.  The source just won't get its cancel cue.
    }

    // Shield the source screen.  Toolkits that don't honor the SelectionClear
    // cancel cue (FOX/xfe, other older toolkits) keep their own drag session
    // running against their own root window with stale coordinates.  Without
    // the shield, their phantom drop lands on whatever sits there: a terminal
    // prints the URI, xfe pops its copy/move/link menu, etc.  With the shield
    // mapped, their session targets us and we swallow it cleanly.
    xdnd_shield_map(st, source_screen);

    // De-animate the source root's cursor before the cross-screen warp,
    // to prevent the Xorg animated-cursor sprite-handoff crash.
	reset_cursor_sprite(st, &st->screens[source_screen], source_screen);

    // Now warp the pointer to the target screen
    ScreenState *tgt_sc = &st->screens[target_screen];
    XWarpPointer(st->dpy, None, tgt_sc->root, 0, 0, 0, 0, warp_x, warp_y);
    XSetInputFocus(st->dpy, tgt_sc->root, RevertToPointerRoot, event_time);
    XFlush(st->dpy);

    px->state = XDND_PROXY_DRAGGING;

    log_info("xdnd: proxy drag active, data='%.*s'",
             (int)(len > 200 ? 200 : len), data);

    return true;
}

// Called every tick / motion while proxy drag is active.
// Updates XdndEnter/Position/Leave for the window under the cursor.
void xdnd_proxy_motion(StellarState *st, int root_x, int root_y, Time timestamp) {
    XdndProxy *px = &st->xdnd_proxy;
    ScreenState *sc = &st->screens[px->target_screen];

    Window target = find_xdnd_window(st, sc->root, root_x, root_y);

    // Filter out our own proxy window
    if (target == st->xdnd_proxy_win[px->target_screen]) {
        target = None;
    }

    if (target != px->current_target) {
        // Left old target
        if (px->current_target != None) {
            xdnd_send_leave(st, px->current_target);
        }
        // Entered new target
        px->current_target = target;
        if (target != None) {
            xdnd_send_enter(st, target);
        }
    }

    if (target != None) {
        xdnd_send_position(st, target, root_x, root_y, timestamp);
    }

    XFlush(st->dpy);
}

// Called when the button is released during proxy drag.
void xdnd_proxy_drop(StellarState *st, Time timestamp) {
    XdndProxy *px = &st->xdnd_proxy;

    // The physical button is up - the shield's input-blocking job is done.
    // (Its ClientMessage handling keeps working after unmap; see helper.)
    xdnd_shield_unmap(st);

    if (px->current_target == None) {
        log_info("xdnd: drop with no target, canceling");
        xdnd_proxy_cleanup(st);
        return;
    }

    log_info("xdnd: dropping onto 0x%lx", px->current_target);
    xdnd_send_drop(st, px->current_target, timestamp);
    px->state = XDND_PROXY_DROPPED;

    // Set a safety timeout - if XdndFinished never comes, clean up after 3s.
    // (Handled in handle_pointer_tick by counting ticks, or you can use a
    // timestamp check.  Simple approach: store the drop time.)
    px->drag_time = timestamp;
}

// Handle SelectionRequest: the drop target asks us for the data.
// Called from drain_x_events when we get a SelectionRequest event.
void xdnd_proxy_handle_selection_request(StellarState *st, XSelectionRequestEvent *req) {
    XdndProxy *px = &st->xdnd_proxy;

    XEvent reply;
    memset(&reply, 0, sizeof(reply));
    reply.xselection.type      = SelectionNotify;
    reply.xselection.requestor = req->requestor;
    reply.xselection.selection = req->selection;
    reply.xselection.target    = req->target;
    reply.xselection.time      = req->time;
    reply.xselection.property  = None;  // Assume failure

    if (req->selection == st->xdnd_selection && px->uri_data && px->uri_len > 0) {
        if (req->target == st->text_uri_list) {
            // Serve our stashed URI data
            XChangeProperty(st->dpy, req->requestor,
                            req->property, st->text_uri_list, 8,
                            PropModeReplace,
                            px->uri_data, px->uri_len);
            reply.xselection.property = req->property;
            log_info("xdnd: served %lu bytes to requestor 0x%lx",
                     px->uri_len, req->requestor);
        } else {
            // The target might ask for TARGETS first
            Atom targets_atom = XInternAtom(st->dpy, "TARGETS", False);
            if (req->target == targets_atom) {
                Atom supported[] = { st->text_uri_list, targets_atom };
                XChangeProperty(st->dpy, req->requestor,
                                req->property, XA_ATOM, 32,
                                PropModeReplace,
                                (unsigned char *)supported, 2);
                reply.xselection.property = req->property;
                log_info("xdnd: served TARGETS to requestor 0x%lx", req->requestor);
            } else {
                char *name = XGetAtomName(st->dpy, req->target);
                log_info("xdnd: unsupported target type requested: %s",
                         name ? name : "(null)");
                if (name) XFree(name);
            }
        }
    }

    XSendEvent(st->dpy, req->requestor, False, NoEventMask, &reply);
    XFlush(st->dpy);
}

// Handle XdndFinished or XdndStatus ClientMessages.
// Called from drain_x_events.
// Returns true if the event was consumed.
bool xdnd_proxy_handle_client_message(StellarState *st, XClientMessageEvent *cm) {
    XdndProxy *px = &st->xdnd_proxy;

    // --- Messages addressed to a drop-shield window ---
    // These come from the ORIGINAL drag source's still-running session
    // (e.g. xfe/FOX, which ignores the SelectionClear cancel cue).  We play
    // a polite, bottomless drop target: accept everything, request nothing,
    // finish immediately.  This terminates the source's drag cleanly with
    // no phantom drop, no context menu, no text pasted into terminals.
    // Note: these can arrive after px->state has returned to IDLE, so the
    // ClientMessage dispatcher must call us unconditionally.
    int shield_idx = -1;
    for (int i = 0; i < st->config.screen_count; i++) {
        if (cm->window == st->xdnd_shield_win[i]) {
            shield_idx = i;
            break;
        }
    }
    if (shield_idx >= 0) {
        Window src = (Window)cm->data.l[0];

        if (cm->message_type == st->xdnd_enter ||
            cm->message_type == st->xdnd_leave) {
            return true;  // Nothing to do, just consume.
        }

        if (cm->message_type == st->xdnd_position) {
            XEvent reply;
            memset(&reply, 0, sizeof(reply));
            reply.xclient.type         = ClientMessage;
            reply.xclient.window       = src;
            reply.xclient.message_type = st->xdnd_status;
            reply.xclient.format       = 32;
            reply.xclient.data.l[0]    = st->xdnd_shield_win[shield_idx];
            reply.xclient.data.l[1]    = 1;   // bit0: will accept the drop
            reply.xclient.data.l[2]    = 0;   // empty rectangle
            reply.xclient.data.l[3]    = 0;
            reply.xclient.data.l[4]    = st->xdnd_action_copy;
            XSendEvent(st->dpy, src, False, NoEventMask, &reply);
            XFlush(st->dpy);
            return true;
        }

        if (cm->message_type == st->xdnd_drop) {
            XEvent reply;
            memset(&reply, 0, sizeof(reply));
            reply.xclient.type         = ClientMessage;
            reply.xclient.window       = src;
            reply.xclient.message_type = st->xdnd_finished;
            reply.xclient.format       = 32;
            reply.xclient.data.l[0]    = st->xdnd_shield_win[shield_idx];
            reply.xclient.data.l[1]    = 1;   // bit0: drop was accepted
            reply.xclient.data.l[2]    = st->xdnd_action_copy;
            XSendEvent(st->dpy, src, False, NoEventMask, &reply);
            XFlush(st->dpy);
            log_info("xdnd: shield swallowed phantom drop from 0x%lx", src);
            return true;
        }

        return false;
    }

    // --- Messages addressed to a proxy window (our outgoing session) ---
    if (cm->message_type == st->xdnd_finished) {
        if (px->state == XDND_PROXY_DROPPED) {
            bool accepted = (cm->data.l[1] & 1);
            log_info("xdnd: finished, accepted=%d", accepted);
            xdnd_proxy_cleanup(st);
            return true;
        }
    }

    if (cm->message_type == st->xdnd_status) {
        // The target tells us whether it will accept.
        // data.l[1] bit 0 = will accept drop
        // We don't need to do much with this - the cursor feedback
        // is already handled by the target window itself.
        return true;
    }

    return false;
}
