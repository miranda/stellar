#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <sys/types.h>
#include <dirent.h>
#include <pwd.h>
#include <unistd.h>
#include <poll.h>
#include <time.h>

#define NK_INCLUDE_FIXED_TYPES
#define NK_INCLUDE_STANDARD_IO
#define NK_INCLUDE_STANDARD_VARARGS
#define NK_INCLUDE_DEFAULT_ALLOCATOR
#define NK_IMPLEMENTATION
#include "nuklear.h"

#define NK_XCB_CAIRO_IMPLEMENTATION
#include "nuklear_xcb.h"

// Shared Stellar theming: colors come from the active theme (relayed from
// AwesomeWM), the font comes from the appearance settings.  Link with
// stellar_theme.c, stellar_font.c, stellar_config.c and cJSON.c.
#include "stellar_theme.h"
#include "stellar_nk_theme.h"

struct icons {
    struct nk_image desktop;
    struct nk_image home;
    struct nk_image computer;
    struct nk_image directory;

    struct nk_image default_file;
    struct nk_image text_file;
    struct nk_image music_file;
    struct nk_image font_file;
    struct nk_image img_file;
    struct nk_image movie_file;
};

enum file_groups {
    FILE_GROUP_DEFAULT,
    FILE_GROUP_TEXT,
    FILE_GROUP_MUSIC,
    FILE_GROUP_FONT,
    FILE_GROUP_IMAGE,
    FILE_GROUP_MOVIE,
    FILE_GROUP_MAX
};

enum file_types {
    FILE_DEFAULT,
    FILE_TEXT,
    FILE_C_SOURCE,
    FILE_CPP_SOURCE,
    FILE_HEADER,
    FILE_CPP_HEADER,
    FILE_MP3,
    FILE_WAV,
    FILE_OGG,
    FILE_TTF,
    FILE_BMP,
    FILE_PNG,
    FILE_JPEG,
    FILE_PCX,
    FILE_TGA,
    FILE_GIF,
    FILE_MAX
};

struct file_group {
    enum file_groups group;
    const char *name;
    struct nk_image *icon;
};

struct file {
    enum file_types type;
    const char *suffix;
    enum file_groups group;
};

struct media {
    int font;
    int icon_sheet;
    struct icons icons;
    struct file_group group[FILE_GROUP_MAX];
    struct file files[FILE_MAX];
};

#define MAX_PATH_LEN 512
struct file_browser {
    /* path */
    char file[MAX_PATH_LEN];
    char home[MAX_PATH_LEN];
    char desktop[MAX_PATH_LEN];
    char directory[MAX_PATH_LEN];

    char input_name[MAX_PATH_LEN];
	int selected_item;

	/* directory content */
    char **files;
    char **directories;
    size_t file_count;
    size_t dir_count;
    struct media *media;
};

static void die(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    vfprintf(stderr, fmt, ap);
    va_end(ap);
    fputs("\n", stderr);
    exit(EXIT_FAILURE);
}

static char* str_duplicate(const char *src)
{
    char *ret;
    size_t len = strlen(src);
    if (!len) return 0;
    ret = (char*)malloc(len+1);
    if (!ret) return 0;
    memcpy(ret, src, len);
    ret[len] = '\0';
    return ret;
}

static void dir_free_list(char **list, size_t size)
{
    size_t i;
    for (i = 0; i < size; ++i)
        free(list[i]);
    free(list);
}

static char** dir_list(const char *dir, int return_subdirs, size_t *count)
{
    size_t n = 0;
    char buffer[MAX_PATH_LEN];
    char **results = NULL;
    const DIR *none = NULL;
    size_t capacity = 32;
    size_t size;
    DIR *z;

    assert(dir);
    assert(count);
    strncpy(buffer, dir, MAX_PATH_LEN);
    buffer[MAX_PATH_LEN - 1] = 0;
    n = strlen(buffer);

    if (n > 0 && (buffer[n-1] != '/'))
        buffer[n++] = '/';

    size = 0;

    z = opendir(dir);
    if (z != none) {
        int nonempty = 1;
        struct dirent *data = readdir(z);
        nonempty = (data != NULL);
        if (!nonempty) return NULL;

        do {
            DIR *y;
            char *p;
            int is_subdir;
            if (data->d_name[0] == '.')
                continue;

            strncpy(buffer + n, data->d_name, MAX_PATH_LEN-n);
            y = opendir(buffer);
            is_subdir = (y != NULL);
            if (y != NULL) closedir(y);

            if ((return_subdirs && is_subdir) || (!is_subdir && !return_subdirs)){
                if (!size) {
                    results = (char**)calloc(capacity, sizeof(char*));
                } else if (size >= capacity) {
                    void *old = results;
                    capacity = capacity * 2;
                    results = (char**)realloc(results, capacity * sizeof(char*));
                    assert(results);
                    if (!results) free(old);
                }
                p = str_duplicate(data->d_name);
                results[size++] = p;
            }
        } while ((data = readdir(z)) != NULL);
    }

    if (z) closedir(z);
    *count = size;
    return results;
}

static struct file_group FILE_GROUP(enum file_groups group, const char *name, struct nk_image *icon)
{
    struct file_group fg;
    fg.group = group;
    fg.name = name;
    fg.icon = icon;
    return fg;
}

static struct file FILE_DEF(enum file_types type, const char *suffix, enum file_groups group)
{
    struct file fd;
    fd.type = type;
    fd.suffix = suffix;
    fd.group = group;
    return fd;
}

static struct nk_image* media_icon_for_file(struct media *media, const char *file)
{
    int i = 0;
    const char *s = file;
    char suffix[4];
    int found = 0;
    memset(suffix, 0, sizeof(suffix));

    /* extract suffix .xxx from file */
    while (*s++ != '\0') {
        if (found && i < 3)
            suffix[i++] = *s;

        if (*s == '.') {
            if (found){
                found = 0;
                break;
            }
            found = 1;
        }
    }

    /* check for all file definition of all groups for fitting suffix*/
    for (i = 0; i < FILE_MAX && found; ++i) {
        struct file *d = &media->files[i];
        {
            const char *f = d->suffix;
            s = suffix;
            while (f && *f && *s && *s == *f) {
                s++; f++;
            }

            /* found correct file definition so */
            if (f && *s == '\0' && *f == '\0')
                return media->group[d->group].icon;
        }
    }
    return &media->icons.default_file;
}

static void media_init(struct media *media)
{
    /* file groups */
    struct icons *icons = &media->icons;
    media->group[FILE_GROUP_DEFAULT] = FILE_GROUP(FILE_GROUP_DEFAULT,"default",&icons->default_file);
    media->group[FILE_GROUP_TEXT] = FILE_GROUP(FILE_GROUP_TEXT, "textual", &icons->text_file);
    media->group[FILE_GROUP_MUSIC] = FILE_GROUP(FILE_GROUP_MUSIC, "music", &icons->music_file);
    media->group[FILE_GROUP_FONT] = FILE_GROUP(FILE_GROUP_FONT, "font", &icons->font_file);
    media->group[FILE_GROUP_IMAGE] = FILE_GROUP(FILE_GROUP_IMAGE, "image", &icons->img_file);
    media->group[FILE_GROUP_MOVIE] = FILE_GROUP(FILE_GROUP_MOVIE, "movie", &icons->movie_file);

    /* files */
    media->files[FILE_DEFAULT] = FILE_DEF(FILE_DEFAULT, NULL, FILE_GROUP_DEFAULT);
    media->files[FILE_TEXT] = FILE_DEF(FILE_TEXT, "txt", FILE_GROUP_TEXT);
    media->files[FILE_C_SOURCE] = FILE_DEF(FILE_C_SOURCE, "c", FILE_GROUP_TEXT);
    media->files[FILE_CPP_SOURCE] = FILE_DEF(FILE_CPP_SOURCE, "cpp", FILE_GROUP_TEXT);
    media->files[FILE_HEADER] = FILE_DEF(FILE_HEADER, "h", FILE_GROUP_TEXT);
    media->files[FILE_CPP_HEADER] = FILE_DEF(FILE_HEADER, "hpp", FILE_GROUP_TEXT);
    media->files[FILE_MP3] = FILE_DEF(FILE_MP3, "mp3", FILE_GROUP_MUSIC);
    media->files[FILE_WAV] = FILE_DEF(FILE_WAV, "wav", FILE_GROUP_MUSIC);
    media->files[FILE_OGG] = FILE_DEF(FILE_OGG, "ogg", FILE_GROUP_MUSIC);
    media->files[FILE_TTF] = FILE_DEF(FILE_TTF, "ttf", FILE_GROUP_FONT);
    media->files[FILE_BMP] = FILE_DEF(FILE_BMP, "bmp", FILE_GROUP_IMAGE);
    media->files[FILE_PNG] = FILE_DEF(FILE_PNG, "png", FILE_GROUP_IMAGE);
    media->files[FILE_JPEG] = FILE_DEF(FILE_JPEG, "jpg", FILE_GROUP_IMAGE);
    media->files[FILE_PCX] = FILE_DEF(FILE_PCX, "pcx", FILE_GROUP_IMAGE);
    media->files[FILE_TGA] = FILE_DEF(FILE_TGA, "tga", FILE_GROUP_IMAGE);
    media->files[FILE_GIF] = FILE_DEF(FILE_GIF, "gif", FILE_GROUP_IMAGE);
}

int cmp_fn(const void *str1, const void *str2)
{
    const char *str1_ret = *(const char **)str1;
    const char *str2_ret = *(const char **)str2;
    return nk_stricmp(str1_ret, str2_ret);
}
 
static void file_browser_reload_directory_content(struct file_browser *browser, const char *path)
{
    strncpy(browser->directory, path, MAX_PATH_LEN);
    browser->directory[MAX_PATH_LEN - 1] = 0;

    // Clear selection state
    browser->selected_item = -1;

    dir_free_list(browser->files, browser->file_count);
    dir_free_list(browser->directories, browser->dir_count);
    
    browser->files = dir_list(path, 0, &browser->file_count);
    browser->directories = dir_list(path, 1, &browser->dir_count);

    // Sort the arrays exactly once after loading them
    if (browser->file_count > 0) {
        qsort(browser->files, browser->file_count, sizeof(char *), cmp_fn);
    }
    if (browser->dir_count > 0) {
        qsort(browser->directories, browser->dir_count, sizeof(char *), cmp_fn);
    }
}

static void file_browser_init(struct file_browser *browser, struct media *media)
{
    const char *home;
    size_t l;

    memset(browser, 0, sizeof(*browser));
    browser->media = media;

    /* load files and sub-directory list */
    home = getenv("HOME");
    if (!home) home = getpwuid(getuid())->pw_dir;

    strncpy(browser->home, home, MAX_PATH_LEN);
    browser->home[MAX_PATH_LEN - 1] = 0;
    l = strlen(browser->home);
    strcpy(browser->home + l, "/");
    strcpy(browser->directory, browser->home);

    strcpy(browser->desktop, browser->home);
    l = strlen(browser->desktop);
    strcpy(browser->desktop + l, "desktop/");

    browser->files = dir_list(browser->directory, 0, &browser->file_count);
    browser->directories = dir_list(browser->directory, 1, &browser->dir_count);
}

static void file_browser_free(struct file_browser *browser)
{
    if (browser->files)
        dir_free_list(browser->files, browser->file_count);
    if (browser->directories)
        dir_free_list(browser->directories, browser->dir_count);
    browser->files = NULL;
    browser->directories = NULL;
    memset(browser, 0, sizeof(*browser));
}

static int file_browser_run(struct file_browser *browser, struct nk_context *ctx, int os_win_width, int os_win_height, int is_save_mode, int is_dir_mode) {
    int ret = 0;
    struct nk_rect total_space;
    static nk_bool file_browser_is_open = nk_true;

    if (!file_browser_is_open) return 0;

    if (nk_begin(ctx, "FileBrowser", nk_rect(0, 0, os_win_width, os_win_height), NK_WINDOW_NO_SCROLLBAR | NK_WINDOW_BACKGROUND)) {
        static float ratio[] = {0.25f, 0.75f};
        float spacing_x = ctx->style.window.spacing.x;

        /* Menubar Navigation */
        ctx->style.window.spacing.x = 0;
        nk_menubar_begin(ctx);
        {
            char *d = browser->directory;
            char *begin = d + 1;
            nk_layout_row_dynamic(ctx, 25, 6);
            while (*d++) {
                if (*d == '/') {
                    *d = '\0';
                    if (nk_button_label(ctx, begin)) {
                        *d++ = '/'; *d = '\0';
                        file_browser_reload_directory_content(browser, browser->directory);
                        break;
                    }
                    *d = '/';
                    begin = d + 1;
                }
            }
        }
        nk_menubar_end(ctx);
        ctx->style.window.spacing.x = spacing_x;

        /* Main Window Split */
        total_space = nk_window_get_content_region(ctx);
        float content_height = total_space.h - 85.0f; // Always leave room for the bottom bar
        if (content_height < 100.0f) content_height = 100.0f; 

        nk_layout_row(ctx, NK_DYNAMIC, content_height, 2, ratio);

        if (nk_group_begin(ctx, "Special", NK_WINDOW_NO_SCROLLBAR)) {
            nk_layout_row_dynamic(ctx, 40, 1);
            if (nk_button_label(ctx, "Home")) file_browser_reload_directory_content(browser, browser->home);
            if (nk_button_label(ctx, "Desktop")) file_browser_reload_directory_content(browser, browser->desktop);
            if (nk_button_label(ctx, "Computer")) file_browser_reload_directory_content(browser, "/");
            nk_group_end(ctx);
        }

        if (nk_group_begin(ctx, "Content", NK_WINDOW_BORDER)) {
            int index = -1;
            static float ratio2[] = {0.15f, 0.85f};
            
            // Draw Directories
            for (size_t j = 0; j < browser->dir_count; ++j) {
                int is_clicked = 0; 
                nk_layout_row(ctx, NK_DYNAMIC, 30, 2, ratio2);
                nk_label(ctx, "[DIR]", NK_TEXT_LEFT);
                if (nk_selectable_label(ctx, browser->directories[j], NK_TEXT_LEFT, &is_clicked)) {
                    index = (int)j;
                }
            }

            // Draw Files
            if (!is_dir_mode) {
                for (size_t j = 0; j < browser->file_count; ++j) {
                    // Check if this specific file is our currently selected item
                    int is_selected = (browser->selected_item == (int)j); 
                    
                    nk_layout_row(ctx, NK_DYNAMIC, 30, 2, ratio2);
                    nk_label(ctx, "[FILE]", NK_TEXT_LEFT);
                    
                    // If clicked, update the selection tracker and text box
                    if (nk_selectable_label(ctx, browser->files[j], NK_TEXT_LEFT, &is_selected)) {
                        browser->selected_item = (int)j;
                        strncpy(browser->input_name, browser->files[j], MAX_PATH_LEN - 1);
                    }
                }
            }

            // Execute Directory Change
            if (index != -1) {
                size_t n = strlen(browser->directory);
                strncpy(browser->directory + n, browser->directories[index], MAX_PATH_LEN - n);
                n = strlen(browser->directory);
                if (n < MAX_PATH_LEN - 1) {
                    browser->directory[n] = '/';
                    browser->directory[n+1] = '\0';
                }
                file_browser_reload_directory_content(browser, browser->directory);
            }
            nk_group_end(ctx);
        }

        // --- Always visible bottom bar ---
        nk_layout_row_dynamic(ctx, 30, 2);
        nk_label(ctx, "File Name:", NK_TEXT_LEFT);
        nk_edit_string_zero_terminated(ctx, NK_EDIT_FIELD, browser->input_name, MAX_PATH_LEN, nk_filter_default);

        nk_layout_row_dynamic(ctx, 30, 4);
        nk_label(ctx, "", NK_TEXT_LEFT); 
        nk_label(ctx, "", NK_TEXT_LEFT); 
        
        if (nk_button_label(ctx, "Cancel")) {
            ret = -1;
        }

        const char *action_label = is_save_mode ? "Save" : (is_dir_mode ? "Select Directory" : "Open");
        if (nk_button_label(ctx, action_label)) {
            if (is_dir_mode) {
                strncpy(browser->file, browser->directory, MAX_PATH_LEN);
                ret = 1;
            } else {
                if (strlen(browser->input_name) > 0) {
                    snprintf(browser->file, MAX_PATH_LEN, "%s%s", browser->directory, browser->input_name);
                    ret = 1;
                }
            }
        }
    }
    nk_end(ctx);
    return ret;
}

static long long now_ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (long long)ts.tv_sec * 1000LL + ts.tv_nsec / 1000000LL;
}

// --- Main event loop ---
int main(int argc, char *argv[])
{
    int window_width = 800;
    int window_height = 600;

	// Default states
    int is_save_mode = 0;
    int is_dir_mode = 0;
    int allow_multiple = 0;
    char window_title[256] = "File Select";
    char initial_name[MAX_PATH_LEN] = {0};

    // Parse the arguments sent by xdg_portal.c
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--mode") == 0 && i + 1 < argc) {
            if (strcmp(argv[i+1], "save") == 0) is_save_mode = 1;
            if (strcmp(argv[i+1], "open-directory") == 0) is_dir_mode = 1;
            i++;
        } else if (strcmp(argv[i], "--title") == 0 && i + 1 < argc) {
            strncpy(window_title, argv[i+1], sizeof(window_title) - 1);
            i++;
        } else if (strcmp(argv[i], "--current-name") == 0 && i + 1 < argc) { // <--- NEW
            strncpy(initial_name, argv[i+1], sizeof(initial_name) - 1);
            i++;
        } else if (strcmp(argv[i], "--multiple") == 0) {
            allow_multiple = 1;
        } else if (strcmp(argv[i], "--directory") == 0) {
            is_dir_mode = 1;
        }
    }

// Fetch theme colors + font from the Stellar DE.  Falls back to the old
// hardcoded look if the DE isn't reachable (e.g. running standalone).
    int stellar_screen = 0;
    const char *screen_env = getenv("STELLAR_SCREEN");
    if (screen_env) stellar_screen = atoi(screen_env);

    ThemeData theme_data;
    int have_theme = (request_theme_data(stellar_screen, &theme_data) == 0);
    if (!have_theme) {
        fprintf(stderr, "fileselect: no theme data from Stellar, using defaults\n");
        memset(&theme_data, 0, sizeof(theme_data));
    }

    struct nk_xcb_context *xcb_ctx;
    struct nk_cairo_context *cairo_ctx;
    struct nk_context *ctx;

    /* X11 / Cairo */
    xcb_ctx = nk_xcb_init(
        window_title,
        0,
        0,
        window_width,
        window_height);
    struct nk_color background =
        stellar_theme_color(theme_data.nk_color_window, nk_rgb(30, 30, 30));

    cairo_ctx = nk_cairo_init(
        &background,
        NULL,
        0,
        nk_xcb_create_cairo_surface(xcb_ctx));

    /* Nuklear */
    ctx = malloc(sizeof(struct nk_context));
    if (!ctx) {
        fprintf(stderr, "Out of memory\n");
        return 1;
    }

    nk_init_default(ctx, nk_cairo_default_font(cairo_ctx));
    apply_nk_theme(ctx, cairo_ctx, &theme_data);

    // Initialize File Browser State
    struct file_browser browser;
    struct media media;
    memset(&media, 0, sizeof(media)); // Since we stripped icons, just zero it out safely
    file_browser_init(&browser, &media);

    // Inject initial name
    if (initial_name[0] != '\0') {
        strncpy(browser.input_name, initial_name, MAX_PATH_LEN - 1);
    }

    int live_w = window_width;
    int live_h = window_height;
    int ui_w = window_width;
    int ui_h = window_height;

    int running = 1;
    int need_redraw = 1;
    int resizing = 0;
    long long last_resize_at = 0;

    // X11 window size settle time 
    const long long resize_settle_ms = 1;

    char selected_filepath[MAX_PATH_LEN] = {0};

    while (running) {
        int timeout_ms = -1;

        if (resizing) {
            long long elapsed = now_ms() - last_resize_at;
            long long remaining = resize_settle_ms - elapsed;
            timeout_ms = remaining > 0 ? (int)remaining : 0;
        }

        struct pollfd pfd;
        pfd.fd = xcb_get_file_descriptor(xcb_ctx->conn);
        pfd.events = POLLIN;
        pfd.revents = 0;

        int pr = poll(&pfd, 1, timeout_ms);

        if (pr < 0) {
            perror("poll");
            break;
        }

        if (pr > 0 && (pfd.revents & POLLIN)) {
            int events = nk_xcb_handle_event(xcb_ctx, ctx);

            if (events & NK_XCB_EVENT_STOP) {
                break;
            }

			// Redraw on any UI interaction
			need_redraw = 1;

            if (events & NK_XCB_EVENT_PAINT) {
                nk_cairo_damage(cairo_ctx);
                need_redraw = 1;
            }

            if (events & NK_XCB_EVENT_RESIZED) {
                /*
                  Keep the actual Cairo/X11 surface in sync immediately
                  so the whole real window can be repainted.
                */
                live_w = xcb_ctx->width;
                live_h = xcb_ctx->height;

                nk_xcb_resize_cairo_surface(
                    xcb_ctx,
                    nk_cairo_surface(cairo_ctx));

                /*
                  Do not resize the giant Nuklear root yet.
                  Just mark that resize is active and keep drawing
                  the old settled ui_w/ui_h for now.
                */
                resizing = 1;
                last_resize_at = now_ms();

                nk_cairo_damage(cairo_ctx);
                need_redraw = 1;
            }
        } else {
            /*
              poll() timed out: if we were resizing and no more configure
              events arrived in the settle interval, commit the new size
              to the Nuklear root window now.
            */
            if (resizing &&
                (now_ms() - last_resize_at) >= resize_settle_ms) {
                ui_w = live_w;
                ui_h = live_h;
                resizing = 0;

                nk_cairo_damage(cairo_ctx);
                need_redraw = 1;
            }
        }

        if (need_redraw) {
            /*
              process_frame gets the settled UI size, not the live X11 size.
              That keeps the giant root from relayouting every resize tick.
            */

            int ui_status = file_browser_run(&browser, ctx, ui_w, ui_h, is_save_mode, is_dir_mode);
            
            if (ui_status == 1) {
                // A file or directory was successfully selected
                strncpy(selected_filepath, browser.file, MAX_PATH_LEN - 1);
                running = 0; 
            } else if (ui_status == -1) {
                // The user explicitly clicked Cancel
                running = 0; 
            }

            nk_cairo_render(cairo_ctx, ctx);
            nk_xcb_render(xcb_ctx);
            nk_clear(ctx);

            need_redraw = 0;
        }
    }

	// Cleanup
    file_browser_free(&browser);
    nk_free(ctx);
    free(ctx);
	stellar_nk_theme_cleanup(cairo_ctx);
    nk_cairo_free(cairo_ctx);
    nk_xcb_free(xcb_ctx);

    // Output standard result for the DE to consume
    if (strlen(selected_filepath) > 0) {
        printf("%s\n", selected_filepath);
		fflush(stdout);
        return 0;
    }

    return 1; // User closed window without selecting anything
}
