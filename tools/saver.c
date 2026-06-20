#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <signal.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <sys/wait.h>
#include <X11/Xlib.h>
#include <X11/Xatom.h>

#define MAX_SCREENS 16
#define SAVER_CMD "/usr/lib/xscreensaver/galaxy"

typedef struct {
    Window win;
    pid_t pid;
} ScreenSaverState;

ScreenSaverState screens[MAX_SCREENS];
Display *dpy;
volatile sig_atomic_t g_sigchld = 0;

static void handle_sigchld(int sig) {
    (void)sig;
    g_sigchld = 1;
}

static void reap_children() {
    pid_t pid;
    int status;
    
    // WNOHANG ensures we don't block waiting for active children
    while ((pid = waitpid(-1, &status, WNOHANG)) > 0) {
        for (int i = 0; i < ScreenCount(dpy); i++) {
            if (screens[i].pid == pid) {
                printf("Saver on screen %d (PID %d) exited.\n", i, pid);
                screens[i].pid = 0;
                
                // If the hack crashed, clean up the black window
                if (screens[i].win) {
                    XDestroyWindow(dpy, screens[i].win);
                    XFlush(dpy);
                    screens[i].win = 0;
                }
                break;
            }
        }
    }
}

static void start_saver(int screen_num) {
    if (screen_num < 0 || screen_num >= ScreenCount(dpy)) return;
    if (screens[screen_num].pid != 0) return; // Already running

    // 1. Create the pitch-black, unmanaged window
    Window root = RootWindow(dpy, screen_num);
    int width = DisplayWidth(dpy, screen_num);
    int height = DisplayHeight(dpy, screen_num);

    XSetWindowAttributes wa;
    wa.override_redirect = True;
    wa.background_pixel = BlackPixel(dpy, screen_num);

    Window win = XCreateWindow(
        dpy, root, 0, 0, width, height, 0,
        CopyFromParent, InputOutput, CopyFromParent,
        CWOverrideRedirect | CWBackPixel, &wa
    );

    XMapRaised(dpy, win);

	char empty_data[] = { 0 };
	Pixmap blank_pixmap = XCreateBitmapFromData(dpy, win, empty_data, 1, 1);
	XColor dummy_color = {0};
	Cursor invisible_cursor = XCreatePixmapCursor(dpy, blank_pixmap, blank_pixmap, &dummy_color, &dummy_color, 0, 0);
	XDefineCursor(dpy, win, invisible_cursor);
	XFreePixmap(dpy, blank_pixmap);
	XFreeCursor(dpy, invisible_cursor);

	Atom saver_atom = XInternAtom(dpy, "_STELLAR_SCREENSAVER", False);
	long val = 1;
	XChangeProperty(dpy, win, saver_atom, XA_CARDINAL, 32,
					PropModeReplace, (unsigned char *)&val, 1);

    XSync(dpy, False); // Ensure the window exists before passing its ID to the hack

    // 2. Fork and launch the xscreensaver hack
    pid_t pid = fork();
    if (pid == 0) {
        // Construct the hex Window ID string
        char wid_str[32];
        snprintf(wid_str, sizeof(wid_str), "0x%lx", win);

        // --- THE FIX: Safely parse and reconstruct the DISPLAY string ---
        char base_disp[64];
        strncpy(base_disp, DisplayString(dpy), sizeof(base_disp) - 1);
        base_disp[sizeof(base_disp) - 1] = '\0';
        
        // Find the colon, then look for a dot *after* the colon
        char *colon = strchr(base_disp, ':');
        if (colon) {
            char *dot = strchr(colon, '.');
            if (dot) {
                *dot = '\0'; // Truncate the string to strip the old screen number
            }
        }

        char display_env[128];
        snprintf(display_env, sizeof(display_env), "DISPLAY=%s.%d", base_disp, screen_num);
        putenv(display_env);
        // ----------------------------------------------------------------

        execl(SAVER_CMD, "galaxy", "-window-id", wid_str, NULL);
        
        // If execl fails, print why and bail out
        perror("execl failed");
        exit(1);
    } else if (pid > 0) {
        printf("Started saver on screen %d (PID %d, Win 0x%lx)\n", screen_num, pid, win);
        screens[screen_num].pid = pid;
        screens[screen_num].win = win;
    } else {
        perror("fork failed");
        XDestroyWindow(dpy, win);
    }
}

static void stop_saver(int screen_num) {
    if (screen_num < 0 || screen_num >= ScreenCount(dpy)) return;

    if (screens[screen_num].pid > 0) {
        printf("Stopping saver on screen %d\n", screen_num);
        kill(screens[screen_num].pid, SIGTERM);
        
        // Block explicitly here to ensure it dies before we yank the window
        waitpid(screens[screen_num].pid, NULL, 0); 
    }

    if (screens[screen_num].win) {
        XDestroyWindow(dpy, screens[screen_num].win);
        XFlush(dpy);
    }

    screens[screen_num].pid = 0;
    screens[screen_num].win = 0;
}

int main(void) {
    setlinebuf(stdout);

    // 1. Connect to X11
    dpy = XOpenDisplay(NULL);
    if (!dpy) {
        fprintf(stderr, "stellar-saver: Failed to open X display\n");
        return 1;
    }

    memset(screens, 0, sizeof(screens));

    // 2. Set up SIGCHLD handler for rogue crashes
    struct sigaction sa;
    sa.sa_handler = handle_sigchld;
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = SA_RESTART | SA_NOCLDSTOP;
    sigaction(SIGCHLD, &sa, NULL);

    // 3. Connect to the Stellar Unix Socket
    const char *socket_path = getenv("STELLAR_SOCKET");
    if (!socket_path) {
        fprintf(stderr, "stellar-saver: STELLAR_SOCKET environment variable not set\n");
        return 1;
    }

    int sock = socket(AF_UNIX, SOCK_STREAM, 0);
    if (sock < 0) {
        perror("socket");
        return 1;
    }

    struct sockaddr_un addr;
    memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;
    strncpy(addr.sun_path, socket_path, sizeof(addr.sun_path) - 1);

    if (connect(sock, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        perror("connect");
        return 1;
    }

    printf("stellar-saver: Connected to Stellar DE\n");
    dprintf(sock, "HELLO role=saver pid=%d\n", getpid());

    // 4. The IPC listening loop
    FILE *f = fdopen(sock, "r");
    if (!f) {
        perror("fdopen");
        return 1;
    }

    char line[256];
    while (fgets(line, sizeof(line), f)) {
        if (g_sigchld) {
            g_sigchld = 0;
            reap_children();
        }

        int s_num;
        if (sscanf(line, "SAVER_START screen=%d", &s_num) == 1) {
            start_saver(s_num);
        } else if (sscanf(line, "SAVER_STOP screen=%d", &s_num) == 1) {
            stop_saver(s_num);
        }
    }

    printf("stellar-saver: Socket closed, exiting.\n");
    XCloseDisplay(dpy);
    return 0;
}
