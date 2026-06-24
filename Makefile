CC = gcc
# Use += so any CFLAGS exported by the environment (e.g. makepkg's hardening
# and optimization flags on Arch) are preserved rather than overwritten. On a
# plain local build where CFLAGS is unset, this starts empty and yields exactly
# these flags.
CFLAGS += -Wall -Wextra -g

DEPFLAGS = -MMD -MP

PREFIX ?= /usr/local
LIBEXECDIR ?= $(PREFIX)/libexec
DATADIR ?= $(PREFIX)/share
DESTDIR ?=

STELLAR_LIBEXEC_PATH = $(LIBEXECDIR)/stellar
STELLAR_SHARE_PATH = $(DATADIR)/stellar

DEFINES = -DSTELLAR_LIBEXEC_PATH=\"$(STELLAR_LIBEXEC_PATH)\" -DSTELLAR_SHARE_PATH=\"$(STELLAR_SHARE_PATH)\"
CFLAGS += $(DEFINES)

DEST_BINDIR = $(DESTDIR)$(PREFIX)/bin
DEST_LIBEXECDIR = $(DESTDIR)$(STELLAR_LIBEXEC_PATH)
DEST_SHAREDIR_BASE = $(DESTDIR)$(DATADIR)
DEST_SHAREDIR = $(DESTDIR)$(STELLAR_SHARE_PATH)
DEST_PORTALDIR = $(DESTDIR)$(DATADIR)/xdg-desktop-portal/portals
DEST_DBUSSERVICEDIR = $(DESTDIR)$(DATADIR)/dbus-1/services

AWESOME_SRC = awesome
AWESOME_DST = $(DEST_SHAREDIR)/awesome
THEME_SRC = themes
THEME_DST = $(DEST_SHAREDIR)/themes
WALLPAPER_SRC = wallpapers
WALLPAPER_DST = $(DEST_SHAREDIR)/wallpapers

BUILDDIR = build
BINDIR_LOCAL = bin

# --- pkg-config groups ---
LUA_CFLAGS	= $(shell pkg-config --cflags lua5.4)
LUA_LIBS	= -llua5.4

FONT_CFLAGS	= $(shell pkg-config --cflags fontconfig)
FONT_LIBS	= $(shell pkg-config --libs fontconfig)

NUKLEAR_PKGS	= xcb xcb-util xcb-keysyms xkbcommon xkbcommon-x11 cairo freetype2 fontconfig
NUKLEAR_CFLAGS	= $(shell pkg-config --cflags $(NUKLEAR_PKGS)) -std=gnu99 \
				  -D_POSIX_C_SOURCE=200809L
NUKLEAR_LIBS	= $(shell pkg-config --libs $(NUKLEAR_PKGS)) -lm

ADMIN_HELPER_PKGS	= libdrm pciaccess x11 xrandr
ADMIN_HELPER_CFLAGS	= $(shell pkg-config --cflags $(ADMIN_HELPER_PKGS))
ADMIN_HELPER_LIBS	= $(shell pkg-config --libs $(ADMIN_HELPER_PKGS)) -lm
X11_LIBS			= -lX11

POLKIT_PKGS     = polkit-agent-1 polkit-gobject-1 glib-2.0
POLKIT_CFLAGS   = $(shell pkg-config --cflags $(POLKIT_PKGS)) -DPOLKIT_AGENT_I_KNOW_API_IS_SUBJECT_TO_CHANGE
POLKIT_LIBS     = $(shell pkg-config --libs $(POLKIT_PKGS)) -lpthread

SYSTEMD_BUS		   := $(shell pkg-config --exists libsystemd && echo libsystemd || echo libelogind)
XDG_PORTAL_PKGS		= $(SYSTEMD_BUS) x11
XDG_PORTAL_CFLAGS	= $(shell pkg-config --cflags $(XDG_PORTAL_PKGS))
XDG_PORTAL_LIBS		= $(shell pkg-config --libs $(XDG_PORTAL_PKGS)) -lm

SNITRAY_PKGS	= $(SYSTEMD_BUS) x11 cairo fontconfig
SNITRAY_CFLAGS	= $(shell pkg-config --cflags $(SNITRAY_PKGS))
SNITRAY_LIBS	= $(shell pkg-config --libs $(SNITRAY_PKGS)) -lm

# --- common objects (shared code) ---
VENDOR_CJSON_OBJ	= $(BUILDDIR)/vendor/cJSON.o
COMMON_CONFIG_OBJ	= $(BUILDDIR)/common/stellar_config.o
COMMON_HW_OBJ		= $(BUILDDIR)/common/stellar_hw.o
COMMON_THEME_OBJ	= $(BUILDDIR)/common/stellar_theme.o
COMMON_NK_THEME_OBJ	= $(BUILDDIR)/common/stellar_nk_theme.o
COMMON_FONT_OBJ		= $(BUILDDIR)/common/stellar_font.o

# Shared by every support app: IPC + fontconfig, pure C, no nuklear symbols.
SUPPORT_CORE_OBJS = $(COMMON_FONT_OBJ) $(COMMON_THEME_OBJ) $(COMMON_CONFIG_OBJ) $(VENDOR_CJSON_OBJ)

# Nuklear apps additionally link the nk theme-application code.  Its nk_*
# references are satisfied by the app's own NK_IMPLEMENTATION translation
# unit, so this object must never be linked into a non-nuklear tool.
SUPPORT_APP_OBJS = $(SUPPORT_CORE_OBJS) $(COMMON_NK_THEME_OBJ)

# --- stellar (main DE) ---
STELLAR_SRCS = src/stellar.c src/xdnd.c src/power.c src/monitor.c src/ipc_lua.c src/xresources.c \
			   src/xdg_autostart.c src/xdg_menu.c src/menu_watch.c src/xdg_util.c
STELLAR_OBJS = $(STELLAR_SRCS:%.c=$(BUILDDIR)/%.o)
STELLAR_LIBS = -lX11 -lXcursor -lXi -lXrandr -lXext $(LUA_LIBS) -lm -ldl

# --- tools ---
SETTINGS_SRCS = tools/settings/settings.c tools/settings/views.c tools/settings/xorg_gen.c
SETTINGS_OBJS = $(SETTINGS_SRCS:%.c=$(BUILDDIR)/%.o)

SAVER_SRCS = tools/saver.c
SAVER_OBJS = $(SAVER_SRCS:%.c=$(BUILDDIR)/%.o)

ADMIN_HELPER_SRCS = tools/admin_helper.c
ADMIN_HELPER_OBJS = $(ADMIN_HELPER_SRCS:%.c=$(BUILDDIR)/%.o)

FILESELECT_SRCS = tools/fileselect.c
FILESELECT_OBJS = $(FILESELECT_SRCS:%.c=$(BUILDDIR)/%.o)

DIALOG_SRCS = tools/dialog.c
DIALOG_OBJS = $(DIALOG_SRCS:%.c=$(BUILDDIR)/%.o)

POLKIT_AGENT_SRCS = tools/polkit_agent.c
POLKIT_AGENT_OBJS = $(POLKIT_AGENT_SRCS:%.c=$(BUILDDIR)/%.o)

XDG_PORTAL_SRCS = tools/xdg_portal.c
XDG_PORTAL_OBJS = $(XDG_PORTAL_SRCS:%.c=$(BUILDDIR)/%.o)

SNITRAY_SRCS = tools/snitray.c
SNITRAY_OBJS = $(SNITRAY_SRCS:%.c=$(BUILDDIR)/%.o)

# ==========================================
.PHONY: all clean install uninstall dirs

all: dirs $(BINDIR_LOCAL)/stellar $(BINDIR_LOCAL)/stellar-saver \
    $(BINDIR_LOCAL)/stellar-settings $(BINDIR_LOCAL)/stellar-admin-helper \
    $(BINDIR_LOCAL)/stellar-fileselect $(BINDIR_LOCAL)/stellar-dialog \
    $(BINDIR_LOCAL)/stellar-polkit-agent $(BINDIR_LOCAL)/xdg-desktop-portal-stellar \
    $(BINDIR_LOCAL)/stellar-snitray

dirs:
	@mkdir -p $(BUILDDIR)/src $(BUILDDIR)/common $(BUILDDIR)/tools \
              $(BUILDDIR)/tools/settings \
              $(BUILDDIR)/vendor $(BINDIR_LOCAL)

# --- link rules ---
$(BINDIR_LOCAL)/stellar: $(STELLAR_OBJS) $(COMMON_CONFIG_OBJ) $(COMMON_FONT_OBJ)
	$(CC) $(CFLAGS) $(LDFLAGS) $^ -o $@ $(STELLAR_LIBS) $(ADMIN_HELPER_LIBS) $(FONT_LIBS)

$(BINDIR_LOCAL)/stellar-saver: $(SAVER_OBJS)
	$(CC) $(CFLAGS) $(LDFLAGS) $^ -o $@ $(X11_LIBS)

$(BINDIR_LOCAL)/stellar-settings: $(SETTINGS_OBJS) $(SUPPORT_APP_OBJS) $(COMMON_HW_OBJ)
	$(CC) $(CFLAGS) $(LDFLAGS) $^ -o $@ $(NUKLEAR_LIBS) $(ADMIN_HELPER_LIBS)

$(BINDIR_LOCAL)/stellar-admin-helper: $(ADMIN_HELPER_OBJS) $(COMMON_HW_OBJ) $(VENDOR_CJSON_OBJ)
	$(CC) $(CFLAGS) $(LDFLAGS) $^ -o $@ $(ADMIN_HELPER_LIBS)

$(BINDIR_LOCAL)/stellar-fileselect: $(FILESELECT_OBJS) $(SUPPORT_APP_OBJS)
	$(CC) $(CFLAGS) $(LDFLAGS) $^ -o $@ $(NUKLEAR_LIBS)

$(BINDIR_LOCAL)/stellar-dialog: $(DIALOG_OBJS) $(SUPPORT_APP_OBJS)
	$(CC) $(CFLAGS) $(LDFLAGS) $^ -o $@ $(NUKLEAR_LIBS)

$(BINDIR_LOCAL)/stellar-polkit-agent: $(POLKIT_AGENT_OBJS) $(SUPPORT_APP_OBJS)
	$(CC) $(CFLAGS) $(LDFLAGS) $^ -o $@ $(NUKLEAR_LIBS) $(POLKIT_LIBS)

$(BINDIR_LOCAL)/xdg-desktop-portal-stellar: $(XDG_PORTAL_OBJS)
	$(CC) $(CFLAGS) $(LDFLAGS) $^ -o $@ $(XDG_PORTAL_LIBS)

$(BINDIR_LOCAL)/stellar-snitray: $(SNITRAY_OBJS) $(SUPPORT_CORE_OBJS)
	$(CC) $(CFLAGS) $(LDFLAGS) $^ -o $@ $(SNITRAY_LIBS)

# --- compile rules with per-directory flags ---
$(BUILDDIR)/src/%.o: src/%.c
	$(CC) $(CFLAGS) $(DEPFLAGS) $(LUA_CFLAGS) $(FONT_CFLAGS) -Icommon -Ivendor -c $< -o $@

# Common objects: stellar_nk_theme.c needs the nuklear/cairo headers
# (-Itools for nuklear.h/nuklear_xcb.h); the rest only need fontconfig.
$(BUILDDIR)/common/%.o: common/%.c
	$(CC) $(CFLAGS) $(DEPFLAGS) $(ADMIN_HELPER_CFLAGS) $(NUKLEAR_CFLAGS) $(FONT_CFLAGS) -Icommon -Itools -Ivendor -c $< -o $@

$(BUILDDIR)/tools/settings/%.o: tools/settings/%.c
	$(CC) $(CFLAGS) $(DEPFLAGS) $(NUKLEAR_CFLAGS) $(ADMIN_HELPER_CFLAGS) -Icommon -Itools -Itools/settings -Ivendor -c $< -o $@

$(BUILDDIR)/tools/admin_helper.o: tools/admin_helper.c
	$(CC) $(CFLAGS) $(DEPFLAGS) $(ADMIN_HELPER_CFLAGS) -Icommon -Ivendor -c $< -o $@

$(BUILDDIR)/tools/fileselect.o: tools/fileselect.c
	$(CC) $(CFLAGS) $(DEPFLAGS) $(NUKLEAR_CFLAGS) -Icommon -Ivendor -c $< -o $@

$(BUILDDIR)/tools/dialog.o: tools/dialog.c
	$(CC) $(CFLAGS) $(DEPFLAGS) $(NUKLEAR_CFLAGS) -Icommon -Ivendor -c $< -o $@

$(BUILDDIR)/tools/polkit_agent.o: tools/polkit_agent.c
	$(CC) $(CFLAGS) $(DEPFLAGS) $(NUKLEAR_CFLAGS) $(POLKIT_CFLAGS) -Icommon -Ivendor -c $< -o $@

$(BUILDDIR)/tools/xdg_portal.o: tools/xdg_portal.c
	$(CC) $(CFLAGS) $(DEPFLAGS) $(XDG_PORTAL_CFLAGS) -Icommon -Ivendor -c $< -o $@

$(BUILDDIR)/tools/snitray.o: tools/snitray.c
	$(CC) $(CFLAGS) $(DEPFLAGS) $(SNITRAY_CFLAGS) -Icommon -Ivendor -c $< -o $@

$(BUILDDIR)/tools/saver.o: tools/saver.c
	$(CC) $(CFLAGS) $(DEPFLAGS) -c $< -o $@

$(BUILDDIR)/vendor/%.o: vendor/%.c
	$(CC) $(CFLAGS) $(DEPFLAGS) -c $< -o $@

clean:
	rm -rf $(BUILDDIR) $(BINDIR_LOCAL)

# Pull in auto-generated header dependencies (.d files emitted by -MMD).
-include $(shell find $(BUILDDIR) -name '*.d' 2>/dev/null)

# ==========================================
# INSTALL RULES
# ==========================================
install:
	install -Dm755 $(BINDIR_LOCAL)/stellar $(DEST_BINDIR)/stellar
	install -Dm755 $(BINDIR_LOCAL)/stellar-settings $(DEST_BINDIR)/stellar-settings
	install -Dm755 scripts/stellar_run.sh $(DEST_BINDIR)/stellar-run
	
	install -Dm755 $(BINDIR_LOCAL)/stellar-polkit-agent $(DEST_BINDIR)/stellar-polkit-agent
	install -Dm644 xdg/autostart/stellar-polkit-agent.desktop $(DESTDIR)/etc/xdg/autostart/stellar-polkit-agent.desktop
	install -Dm644 xdg/autostart/stellar-settings.desktop $(DESTDIR)/etc/xdg/autostart/stellar-settings.desktop
	install -Dm644 xdg/applications/stellar-settings.desktop $(DEST_SHAREDIR_BASE)/applications/stellar-settings.desktop
	
	install -Dm644 xdg/stellar.desktop $(DESTDIR)/usr/share/xsessions/stellar.desktop
	
	mkdir -p $(DEST_SHAREDIR)/lua
	install -Dm644 lua/stellar.lua $(DEST_SHAREDIR)/lua/stellar.lua
	
	mkdir -p $(DEST_SHAREDIR)/picom
	install -Dm644 picom_defaults.json $(DEST_SHAREDIR)/picom/defaults.json
	
	mkdir -p $(DEST_LIBEXECDIR)
	install -Dm755 $(BINDIR_LOCAL)/stellar-admin-helper $(DEST_LIBEXECDIR)/stellar-admin-helper
	install -Dm755 $(BINDIR_LOCAL)/stellar-saver $(DEST_LIBEXECDIR)/stellar-saver
	install -Dm755 $(BINDIR_LOCAL)/stellar-fileselect $(DEST_LIBEXECDIR)/stellar-fileselect
	install -Dm755 $(BINDIR_LOCAL)/stellar-dialog $(DEST_LIBEXECDIR)/stellar-dialog
	install -Dm755 $(BINDIR_LOCAL)/stellar-snitray $(DEST_LIBEXECDIR)/stellar-snitray
	
	install -Dm755 $(BINDIR_LOCAL)/xdg-desktop-portal-stellar $(DEST_LIBEXECDIR)/xdg-desktop-portal-stellar
	install -Dm644 xdg/portals/stellar.portal $(DEST_PORTALDIR)/stellar.portal
	install -d $(DEST_DBUSSERVICEDIR)
	
	# Notice we substitute the clean path, not the DESTDIR staging path
	sed 's|@LIBEXECDIR@|$(STELLAR_LIBEXEC_PATH)|g' \
        xdg/portals/org.freedesktop.impl.portal.desktop.stellar.service.in \
        > $(DEST_DBUSSERVICEDIR)/org.freedesktop.impl.portal.desktop.stellar.service
	chmod 644 $(DEST_DBUSSERVICEDIR)/org.freedesktop.impl.portal.desktop.stellar.service
	
	mkdir -p $(DEST_SHAREDIR)/awesome/modules
	cd $(AWESOME_SRC) && find . -type d -exec install -d -m 755 "$(abspath $(AWESOME_DST))/{}" \;
	cd $(AWESOME_SRC) && find . -type f -exec sh -c 'install -m 644 "$$1" "$(abspath $(AWESOME_DST))/$$1"' _ {} \;
	
	mkdir -p $(DEST_SHAREDIR)/themes
	cd $(THEME_SRC) && find . -type d -exec install -d -m 755 "$(abspath $(THEME_DST))/{}" \;
	cd $(THEME_SRC) && find . -type f -exec sh -c 'install -m 644 "$$1" "$(abspath $(THEME_DST))/$$1"' _ {} \;
	
	mkdir -p $(DEST_SHAREDIR)/wallpapers
	cd $(WALLPAPER_SRC) && find . -type d -exec install -d -m 755 "$(abspath $(WALLPAPER_DST))/{}" \;
	cd $(WALLPAPER_SRC) && find . -type f -exec sh -c 'install -m 644 "$$1" "$(abspath $(WALLPAPER_DST))/$$1"' _ {} \;

uninstall:
	rm -f $(DEST_BINDIR)/stellar
	rm -f $(DEST_BINDIR)/stellar-run
	rm -f $(DEST_BINDIR)/stellar-settings
	rm -f $(DEST_BINDIR)/stellar-polkit-agent
	rm -f $(DESTDIR)/etc/xdg/autostart/stellar-polkit-agent.desktop
	rm -rf $(DEST_LIBEXECDIR)
	rm -rf $(DEST_SHAREDIR)
	rm -f $(DEST_PORTALDIR)/stellar.portal
	rm -f $(DEST_DBUSSERVICEDIR)/org.freedesktop.impl.portal.desktop.stellar.service
