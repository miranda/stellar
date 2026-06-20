#define _GNU_SOURCE

#include "stellar_hw.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdbool.h>
#include <unistd.h>
#include <fcntl.h>
#include <dirent.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <sys/stat.h>
#include <time.h>
#include <signal.h>
#include <sched.h>
#include <sys/mount.h>

#include <xf86drm.h>
#include <pciaccess.h>
#include <X11/Xlib.h>
#include <X11/Xatom.h>
#include <X11/extensions/Xrandr.h>

#define CACHE_DIR "/var/cache/stellar"
#define JSON_OUTPUT_FILE "/var/cache/stellar/hardware.json"
#define LOG_FILE "/tmp/stellar-hw-probe.log"
#define TEMP_CONF "/tmp/stellar-hw-probe-xorg.conf"

// Keep in sync with stellar_config.h. Defined defensively in case this helper
// is built without that header on the include path.
#ifndef MAX_MONITOR_MODES
#define MAX_MONITOR_MODES 48
#endif

struct GpuInfo gpus[16];
int gpu_count = 0;

// --- Helper Functions ---
void ensure_root(int argc, char *argv[]) {
    if (geteuid() != 0) {
        printf("Elevating privileges via Polkit...\n");
        char **pkexec_argv = malloc((argc + 2) * sizeof(char *));
        pkexec_argv[0] = "pkexec";
        for (int i = 0; i < argc; i++) pkexec_argv[i + 1] = argv[i];
        pkexec_argv[argc + 1] = NULL;
        
        execvp("pkexec", pkexec_argv);
        perror("Failed to execute pkexec");
        exit(EXIT_FAILURE);
    }
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

// Dynamically creates a minimal config so Xorg loads the correct DDX drivers
void write_temp_xorg_conf() {
    FILE *f = fopen(TEMP_CONF, "w");
    if (!f) return;

    for (int i = 0; i < gpu_count; i++) {
        fprintf(f, "Section \"Device\"\n");
        fprintf(f, "    Identifier \"Card%d\"\n", i);
        fprintf(f, "    Driver     \"%s\"\n", gpus[i].driver);
        fprintf(f, "    BusID      \"%s\"\n", gpus[i].bus_id);
        fprintf(f, "EndSection\n\n");

        fprintf(f, "Section \"Screen\"\n");
        fprintf(f, "    Identifier \"Screen%d\"\n", i);
        fprintf(f, "    Device     \"Card%d\"\n", i);
        fprintf(f, "EndSection\n\n");
    }

    fprintf(f, "Section \"ServerLayout\"\n");
    fprintf(f, "    Identifier \"ProbeLayout\"\n");
    for (int i = 0; i < gpu_count; i++) {
        fprintf(f, "    Screen     \"Screen%d\"\n", i);
    }
    fprintf(f, "EndSection\n");

    fclose(f);
}

// Spawns isolated X server using the temporary config
pid_t start_x_server(int *display_num) {
    int pfd[2];
    if (pipe(pfd) == -1) exit(EXIT_FAILURE);

    pid_t pid = fork();
    if (pid == 0) {
		close(pfd[0]);
		
		// Isolate from system xorg configs
		if (unshare(CLONE_NEWNS) == 0) {
			mount(NULL, "/", NULL, MS_REC | MS_PRIVATE, NULL);
			mount("tmpfs", "/etc/X11/xorg.conf.d", "tmpfs", 0, NULL);
			mount("tmpfs", "/usr/share/X11/xorg.conf.d", "tmpfs", 0, NULL);
		}
		
		char fd_str[16];
		snprintf(fd_str, sizeof(fd_str), "%d", pfd[1]);
		execlp("Xorg", "Xorg", "-displayfd", fd_str, "-logfile", LOG_FILE,
			   "-config", TEMP_CONF, "vt9", NULL);
		exit(EXIT_FAILURE);
    }

    close(pfd[1]);
    char disp_buf[16] = {0};
    ssize_t bytes = read(pfd[0], disp_buf, sizeof(disp_buf) - 1);
    close(pfd[0]);

    if (bytes <= 0) {
        kill(pid, SIGTERM);
        exit(EXIT_FAILURE);
    }

    *display_num = atoi(disp_buf);
    return pid;
}

// Emit a JSON-safe version of `s`. EDID monitor-name descriptors are free-form
// and can legally contain " or \, which would otherwise produce invalid JSON.
static void json_escape_fputs(const char *s, FILE *out) {
    for (const unsigned char *p = (const unsigned char *)s; *p; p++) {
        switch (*p) {
            case '"':  fputs("\\\"", out); break;
            case '\\': fputs("\\\\", out); break;
            case '\n': fputs("\\n", out);  break;
            case '\r': fputs("\\r", out);  break;
            case '\t': fputs("\\t", out);  break;
            default:
                if (*p < 0x20) fprintf(out, "\\u%04x", *p);
                else           fputc(*p, out);
        }
    }
}

// EDID is binary with embedded nulls; store it as a lowercase hex string so it
// survives JSON intact and can be hashed later for swap-aware mismatch checks.
static void edid_to_hex(const unsigned char *edid, size_t len, char *out, size_t out_size) {
    static const char hex[] = "0123456789abcdef";
    size_t o = 0;
    for (size_t i = 0; i < len && o + 2 < out_size; i++) {
        out[o++] = hex[edid[i] >> 4];
        out[o++] = hex[edid[i] & 0x0F];
    }
    out[o] = '\0';
}

// Physical size in mm lives at EDID bytes 21 (horiz) and 22 (vert), in cm.
// Returns false if the EDID is too short or reports 0 (some projectors do).
static bool edid_phys_size_mm(const unsigned char *edid, size_t len,
                              int *w_mm, int *h_mm) {
    if (!edid || len < 23) return false;
    int w_cm = edid[21], h_cm = edid[22];
    if (w_cm == 0 && h_cm == 0) return false;
    *w_mm = w_cm * 10;
    *h_mm = h_cm * 10;
    return true;
}

// Refresh in mHz from a RandR mode, matching monitor.c's rounding exactly so
// readers see identical numbers regardless of which side computed them.
static int mode_refresh_mhz(const XRRModeInfo *mode) {
    if (mode->hTotal > 0 && mode->vTotal > 0) {
        return (int)((double)mode->dotClock /
                     ((double)mode->hTotal * (double)mode->vTotal)
                     * 1000.0 + 0.5);
    }
    return 0;
}

void probe_xrandr_and_write(int display_num) {
    char display_str[32];
    snprintf(display_str, sizeof(display_str), ":%d", display_num);

    Display *dpy = XOpenDisplay(display_str);
    if (!dpy) return;

    struct stat st = {0};
    if (stat(CACHE_DIR, &st) == -1) mkdir(CACHE_DIR, 0755);

    FILE *out = fopen(JSON_OUTPUT_FILE, "w");
    if (!out) {
        fprintf(stderr, "Failed to open %s for writing\n", JSON_OUTPUT_FILE);
        XCloseDisplay(dpy);
        return;
    }

	Atom edid_atom = XInternAtom(dpy, "EDID", False);

    // Start JSON Object
    fprintf(out, "{\n");
    fprintf(out, "  \"cards\": {\n");
    
    // We iterate up to gpu_count. Because of the temporary xorg.conf, 
    // Screen i is guaranteed to correspond to gpus[i].
    for (int i = 0; i < gpu_count; i++) {
        fprintf(out, "    \"%d\": {\n", i);
        fprintf(out, "      \"driver\": \"%s\",\n", gpus[i].driver);
        fprintf(out, "      \"bus_id\": \"%s\",\n", gpus[i].bus_id);
        fprintf(out, "      \"name\": \"%s\",\n", gpus[i].name);
        fprintf(out, "      \"outputs\": [\n");

        Window root = RootWindow(dpy, i);
        XRRScreenResources *res = XRRGetScreenResources(dpy, root);

        if (res) {
            for (int j = 0; j < res->noutput; j++) {
                XRROutputInfo *info = XRRGetOutputInfo(dpy, res, res->outputs[j]);
                if (!info) continue;

                bool connected = (info->connection == RR_Connected);

                fprintf(out, "        {\n");
                fprintf(out, "          \"name\": \"");
                json_escape_fputs(info->name, out);
                fprintf(out, "\",\n");
                fprintf(out, "          \"connected\": %s",
                        connected ? "true" : "false");

                if (connected) {
                    // --- EDID (this is what only the dummy full-X probe can
                    // see for inactive outputs; the running DE cannot) ---
                    unsigned char edid_buf[512];
                    size_t edid_len = 0;
                    char monitor_name[64] = "";
                    int phys_w = 0, phys_h = 0;

                    Atom actual_type;
                    int actual_format;
                    unsigned long nitems = 0, bytes_after = 0;
                    unsigned char *prop = NULL;

                    if (edid_atom != None &&
                        XRRGetOutputProperty(dpy, res->outputs[j], edid_atom,
                                             0, 512, False, False,
                                             AnyPropertyType, &actual_type,
                                             &actual_format, &nitems,
                                             &bytes_after, &prop) == Success &&
                        prop) {
                        edid_len = nitems;
                        if (edid_len > sizeof(edid_buf)) edid_len = sizeof(edid_buf);
                        memcpy(edid_buf, prop, edid_len);
                        extract_monitor_name(edid_buf, edid_len,
                                             monitor_name, sizeof(monitor_name));
                        edid_phys_size_mm(edid_buf, edid_len, &phys_w, &phys_h);
                        XFree(prop);
                    }

                    // Fall back to RandR's mm fields, then to the output name.
                    if (phys_w == 0 && phys_h == 0) {
                        phys_w = (int)info->mm_width;
                        phys_h = (int)info->mm_height;
                    }
                    if (monitor_name[0] == '\0')
                        snprintf(monitor_name, sizeof(monitor_name), "%s", info->name);

                    fprintf(out, ",\n          \"monitor_name\": \"");
                    json_escape_fputs(monitor_name, out);
                    fprintf(out, "\",\n");
                    fprintf(out, "          \"phys_width_mm\": %d,\n", phys_w);
                    fprintf(out, "          \"phys_height_mm\": %d,\n", phys_h);

                    if (edid_len > 0) {
                        char *edid_hex = malloc(edid_len * 2 + 1);
                        if (edid_hex) {
                            edid_to_hex(edid_buf, edid_len, edid_hex, edid_len * 2 + 1);
                            fprintf(out, "          \"edid\": \"%s\",\n", edid_hex);
                            free(edid_hex);
                        } else {
                            fprintf(out, "          \"edid\": \"\",\n");
                        }
                    } else {
                        fprintf(out, "          \"edid\": \"\",\n");
                    }

                    // --- Mode list (dedup + preferred), same math as monitor.c ---
                    // RandR marks the first info->npreferred entries of
                    // info->modes as preferred; index 0 is the best one.
                    RRMode preferred_id = (info->npreferred > 0 && info->nmode > 0)
                                          ? info->modes[0] : None;
                    int pref_w = 0, pref_h = 0, pref_r = 0;

                    fprintf(out, "          \"modes\": [");
                    int emitted = 0;
                    // Track emitted WxH@Hz for dedup.
                    struct { int w, h, hz; } seen[MAX_MONITOR_MODES];

                    for (int m = 0; m < info->nmode && emitted < MAX_MONITOR_MODES; m++) {
                        for (int r = 0; r < res->nmode; r++) {
                            if (res->modes[r].id != info->modes[m]) continue;
                            XRRModeInfo *mode = &res->modes[r];
                            int w = (int)mode->width;
                            int h = (int)mode->height;
                            int rmhz = mode_refresh_mhz(mode);
                            int rhz = (rmhz + 500) / 1000;

                            if (info->modes[m] == preferred_id) {
                                pref_w = w; pref_h = h; pref_r = rmhz;
                            }

                            bool dup = false;
                            for (int d = 0; d < emitted; d++) {
                                if (seen[d].w == w && seen[d].h == h && seen[d].hz == rhz) {
                                    dup = true; break;
                                }
                            }
                            if (!dup) {
                                fprintf(out, "%s{\"w\":%d,\"h\":%d,\"r\":%d}",
                                        emitted ? "," : "", w, h, rmhz);
                                seen[emitted].w = w;
                                seen[emitted].h = h;
                                seen[emitted].hz = rhz;
                                emitted++;
                            }
                            break;
                        }
                    }
                    fprintf(out, "],\n");
                    fprintf(out, "          \"preferred_mode\": ");
                    if (pref_w > 0)
                        fprintf(out, "{\"w\":%d,\"h\":%d,\"r\":%d}\n", pref_w, pref_h, pref_r);
                    else
                        fprintf(out, "null\n");
                } else {
                    fprintf(out, "\n");
                }

                fprintf(out, "        }%s\n", (j < res->noutput - 1) ? "," : "");
                XRRFreeOutputInfo(info);
            }
            XRRFreeScreenResources(res);
        }

        fprintf(out, "      ]\n");

        // Handle trailing comma for the card object closing brace
        fprintf(out, "    }%s\n", (i < gpu_count - 1) ? "," : "");
    }
    
    fprintf(out, "  }\n"); // Close cards object
    fprintf(out, "}\n");   // Close root object

    fflush(out); // Ensure the stream is fully pushed out
	fclose(out);
    XCloseDisplay(dpy);
    
    printf("Hardware probe complete: Wrote %s\n", JSON_OUTPUT_FILE);
}

int apply_config(const char *src, const char *dest) {
    // Security constraints: generalized file copying but sandboxed to the X11 config directory
    const char *allowed_dir = "/etc/X11/xorg.conf.d/";
    if (strncmp(dest, allowed_dir, strlen(allowed_dir)) != 0) {
        fprintf(stderr, "Security violation: Destination must be within %s\n", allowed_dir);
        return 1;
    }
    if (strstr(dest, "..")) {
        fprintf(stderr, "Security violation: Directory traversal detected.\n");
        return 1;
    }

    int fd_src = open(src, O_RDONLY);
    if (fd_src < 0) {
        perror("Failed to open staging file");
        return 1;
    }

    // Write file with root ownership and global read access
    int fd_dest = open(dest, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (fd_dest < 0) {
        perror("Failed to open target configuration file");
        close(fd_src);
        return 1;
    }

    char buf[4096];
    ssize_t bytes_read;
    while ((bytes_read = read(fd_src, buf, sizeof(buf))) > 0) {
        if (write(fd_dest, buf, bytes_read) != bytes_read) {
            perror("Failed writing configuration block");
            close(fd_src);
            close(fd_dest);
            return 1;
        }
    }

    close(fd_src);
    close(fd_dest);
    printf("Success: Applied configuration to %s\n", dest);
    return 0;
}

void print_usage(const char *prog_name) {
    printf("Stellar DE Privileged Administration Helper\n\n");
    printf("Usage:\n");
    printf("  %s --probe\n", prog_name);
    printf("  %s --apply-display <staging_path> <target_path>\n", prog_name);
}

int main(int argc, char *argv[]) {
    if (argc < 2) {
        print_usage(argv[0]);
        return EXIT_FAILURE;
    }

    // Expect the caller (e.g., pkexec) to have elevated privileges before invocation.
    if (geteuid() != 0) {
        fprintf(stderr, "Error: Insufficient privileges. Must be run as root.\n");
        return EXIT_FAILURE;
    }

    if (strcmp(argv[1], "--probe") == 0) {
		probe_hardware(gpus, &gpu_count);
        write_temp_xorg_conf();
        
        int display_num = -1;
        pid_t xorg_pid = start_x_server(&display_num);

        if (display_num >= 0) {
            probe_xrandr_and_write(display_num);
        }

        kill(xorg_pid, SIGTERM);
        waitpid(xorg_pid, NULL, 0);
        unlink(TEMP_CONF); 
        return EXIT_SUCCESS;

    } else if (strcmp(argv[1], "--apply-display") == 0) {
        if (argc != 4) {
            fprintf(stderr, "Error: --apply-display requires staging file and target file paths.\n");
            return EXIT_FAILURE;
        }
        return apply_config(argv[2], argv[3]) == 0 ? EXIT_SUCCESS : EXIT_FAILURE;

    } else {
        fprintf(stderr, "Unknown action: %s\n", argv[1]);
        print_usage(argv[0]);
        return EXIT_FAILURE;
    }
}
