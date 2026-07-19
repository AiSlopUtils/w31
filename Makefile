CC ?= cc
PKG_CONFIG ?= pkg-config
BUILD_DIR ?= build

override WIN31X_REQUIRED_PACKAGES := x11 xrandr xinerama libpng
override WIN31X_XTST_PACKAGE := xtst

prefix ?= /usr/local
bindir ?= $(prefix)/bin
datadir ?= $(prefix)/share
mandir ?= $(datadir)/man
icondir ?= $(datadir)/win31x/icons
appicon16dir ?= $(datadir)/icons/hicolor/16x16/apps
appicon48dir ?= $(datadir)/icons/hicolor/48x48/apps

CFLAGS ?= -O2 -g

override WIN31X_CPPFLAGS = -Isrc -DWIN31X_ICON_DIR='"$(icondir)"' \
	$(shell $(PKG_CONFIG) --cflags $(WIN31X_REQUIRED_PACKAGES))
override WIN31X_CFLAGS = -std=c11 -Wall -Wextra -Wpedantic -Wshadow
override WIN31X_LDFLAGS =
override WIN31X_LDLIBS = \
	$(shell $(PKG_CONFIG) --libs $(WIN31X_REQUIRED_PACKAGES))
override WIN31X_XTST_CPPFLAGS = \
	$(shell $(PKG_CONFIG) --cflags $(WIN31X_XTST_PACKAGE))
override WIN31X_XTST_LDLIBS = \
	$(shell $(PKG_CONFIG) --libs $(WIN31X_XTST_PACKAGE))

ifneq ($(SANITIZE),)
override WIN31X_CFLAGS += -O1 -fno-omit-frame-pointer \
	-fsanitize=address,undefined
override WIN31X_LDFLAGS += -fsanitize=address,undefined
endif

WM_SOURCES = src/win31x.c src/applications.c src/app_icons.c src/auto_lock.c \
	src/desktop_state.c src/icon_assets.c src/session_actions.c src/settings.c \
	src/task_manager_data.c src/wifi_backend.c
WM_HEADERS = src/applications.h src/app_icons.h src/auto_lock.h \
	src/desktop_state.h src/icon_assets.h src/session_actions.h src/settings.h \
	src/task_manager_data.h src/wifi_backend.h
ICON_FILES = $(notdir $(wildcard assets/icons/*.png))
ICON_DIR_STAMP = $(BUILD_DIR)/.icon-dir

.PHONY: all check check-build-deps check-xtst-deps check-xvfb-deps smoke \
	smoke-xvfb smoke-multimon-xvfb smoke-persistence-xvfb install uninstall \
	clean FORCE

all: $(BUILD_DIR)/win31x

check-build-deps:
	@if ! $(PKG_CONFIG) --version >/dev/null 2>&1; then \
		echo "win31x: pkg-config is required to locate X11, XRandR, Xinerama, and libpng." >&2; \
		echo "Debian/Ubuntu: sudo apt install build-essential pkg-config libx11-dev libxrandr-dev libxinerama-dev libpng-dev" >&2; \
		exit 2; \
	fi
	@if ! $(PKG_CONFIG) --exists $(WIN31X_REQUIRED_PACKAGES); then \
		$(PKG_CONFIG) --print-errors --exists \
			$(WIN31X_REQUIRED_PACKAGES) 2>&1 || true; \
		echo "win31x: required pkg-config modules are unavailable: $(WIN31X_REQUIRED_PACKAGES)" >&2; \
		echo "Debian/Ubuntu: sudo apt install build-essential pkg-config libx11-dev libxrandr-dev libxinerama-dev libpng-dev" >&2; \
		exit 2; \
	fi

check-xtst-deps: check-build-deps
	@if ! $(PKG_CONFIG) --exists $(WIN31X_XTST_PACKAGE); then \
		$(PKG_CONFIG) --print-errors --exists \
			$(WIN31X_XTST_PACKAGE) 2>&1 || true; \
		echo "win31x: XTEST development files are required for X11 smoke tests." >&2; \
		echo "Debian/Ubuntu: sudo apt install libxtst-dev" >&2; \
		exit 2; \
	fi

check-xvfb-deps: check-xtst-deps
	@for program in xvfb-run xauth; do \
		if ! command -v "$$program" >/dev/null 2>&1; then \
			echo "win31x: $$program is required for headless X11 smoke tests." >&2; \
			echo "Debian/Ubuntu: sudo apt install xvfb xauth xfonts-base" >&2; \
			exit 2; \
		fi; \
	done

$(BUILD_DIR):
	mkdir -p $(BUILD_DIR)

FORCE:

$(ICON_DIR_STAMP): FORCE | $(BUILD_DIR)
	@tmp="$@.tmp"; printf '%s\n' "$(icondir)" >"$$tmp"; \
	if ! cmp -s "$$tmp" "$@"; then mv "$$tmp" "$@"; else rm -f "$$tmp"; fi

$(BUILD_DIR)/win31x: $(WM_SOURCES) $(WM_HEADERS) $(ICON_DIR_STAMP) | \
	$(BUILD_DIR) check-build-deps
	$(CC) $(CPPFLAGS) $(WIN31X_CPPFLAGS) $(CFLAGS) $(WIN31X_CFLAGS) \
		$(LDFLAGS) $(WIN31X_LDFLAGS) $(WM_SOURCES) -o $@ \
		$(LDLIBS) $(WIN31X_LDLIBS)

$(BUILD_DIR)/test-applications: tests/test_applications.c src/applications.c $(WM_HEADERS) | $(BUILD_DIR)
	$(CC) $(CPPFLAGS) -Isrc $(CFLAGS) $(WIN31X_CFLAGS) $(LDFLAGS) \
		$(WIN31X_LDFLAGS) tests/test_applications.c src/applications.c -o $@

$(BUILD_DIR)/test-icon-assets: tests/test_icon_assets.c src/icon_assets.c src/icon_assets.h | \
	$(BUILD_DIR) check-build-deps
	$(CC) $(CPPFLAGS) $(WIN31X_CPPFLAGS) $(CFLAGS) $(WIN31X_CFLAGS) \
		$(LDFLAGS) $(WIN31X_LDFLAGS) tests/test_icon_assets.c \
		src/icon_assets.c -o $@ $(LDLIBS) $(WIN31X_LDLIBS)

$(BUILD_DIR)/test-app-icons: tests/test_app_icons.c src/app_icons.c src/app_icons.h | \
		$(BUILD_DIR)
	$(CC) $(CPPFLAGS) -Isrc $(CFLAGS) $(WIN31X_CFLAGS) $(LDFLAGS) \
		$(WIN31X_LDFLAGS) tests/test_app_icons.c src/app_icons.c -o $@

$(BUILD_DIR)/test-settings: tests/test_settings.c src/settings.c src/settings.h | $(BUILD_DIR)
	$(CC) $(CPPFLAGS) -Isrc $(CFLAGS) $(WIN31X_CFLAGS) $(LDFLAGS) \
		$(WIN31X_LDFLAGS) tests/test_settings.c src/settings.c -o $@

$(BUILD_DIR)/test-desktop-state: tests/test_desktop_state.c \
		src/desktop_state.c src/desktop_state.h | $(BUILD_DIR)
	$(CC) $(CPPFLAGS) -Isrc $(CFLAGS) $(WIN31X_CFLAGS) $(LDFLAGS) \
		$(WIN31X_LDFLAGS) tests/test_desktop_state.c src/desktop_state.c -o $@

$(BUILD_DIR)/test-auto-lock: tests/test_auto_lock.c src/auto_lock.c src/auto_lock.h | \
	$(BUILD_DIR) check-build-deps
	$(CC) $(CPPFLAGS) $(WIN31X_CPPFLAGS) $(CFLAGS) $(WIN31X_CFLAGS) \
		$(LDFLAGS) $(WIN31X_LDFLAGS) tests/test_auto_lock.c \
		src/auto_lock.c -o $@ $(LDLIBS) $(WIN31X_LDLIBS)

$(BUILD_DIR)/test-session-actions: tests/test_session_actions.c \
		src/session_actions.c src/session_actions.h src/applications.c \
		src/applications.h | $(BUILD_DIR)
	$(CC) $(CPPFLAGS) -Isrc $(CFLAGS) $(WIN31X_CFLAGS) $(LDFLAGS) \
		$(WIN31X_LDFLAGS) tests/test_session_actions.c \
		src/session_actions.c src/applications.c -o $@

$(BUILD_DIR)/test-task-manager-data: tests/test_task_manager_data.c \
		src/task_manager_data.c src/task_manager_data.h | $(BUILD_DIR)
	$(CC) $(CPPFLAGS) -Isrc $(CFLAGS) $(WIN31X_CFLAGS) $(LDFLAGS) \
		$(WIN31X_LDFLAGS) tests/test_task_manager_data.c \
		src/task_manager_data.c -o $@

$(BUILD_DIR)/test-wifi-backend: tests/test_wifi_backend.c src/wifi_backend.c \
		src/wifi_backend.h | $(BUILD_DIR)
	$(CC) $(CPPFLAGS) -Isrc $(CFLAGS) $(WIN31X_CFLAGS) $(LDFLAGS) \
		$(WIN31X_LDFLAGS) tests/test_wifi_backend.c src/wifi_backend.c -o $@

$(BUILD_DIR)/wm-probe: tests/wm_probe.c | $(BUILD_DIR) check-xtst-deps
	$(CC) $(CPPFLAGS) $(WIN31X_CPPFLAGS) $(WIN31X_XTST_CPPFLAGS) \
		$(CFLAGS) $(WIN31X_CFLAGS) $(LDFLAGS) $(WIN31X_LDFLAGS) \
		tests/wm_probe.c -o $@ $(LDLIBS) $(WIN31X_LDLIBS) \
		$(WIN31X_XTST_LDLIBS)

$(BUILD_DIR)/persistence-probe: tests/persistence_probe.c | \
		$(BUILD_DIR) check-xtst-deps
	$(CC) $(CPPFLAGS) $(WIN31X_CPPFLAGS) $(WIN31X_XTST_CPPFLAGS) \
		$(CFLAGS) $(WIN31X_CFLAGS) $(LDFLAGS) $(WIN31X_LDFLAGS) \
		tests/persistence_probe.c -o $@ $(LDLIBS) $(WIN31X_LDLIBS) \
		$(WIN31X_XTST_LDLIBS)

$(BUILD_DIR)/preexisting-client: tests/preexisting_client.c | \
	$(BUILD_DIR) check-build-deps
	$(CC) $(CPPFLAGS) $(WIN31X_CPPFLAGS) $(CFLAGS) $(WIN31X_CFLAGS) \
		$(LDFLAGS) $(WIN31X_LDFLAGS) tests/preexisting_client.c \
		-o $@ $(LDLIBS) $(WIN31X_LDLIBS)

check: $(BUILD_DIR)/test-applications $(BUILD_DIR)/test-app-icons \
	$(BUILD_DIR)/test-icon-assets \
	$(BUILD_DIR)/test-settings $(BUILD_DIR)/test-desktop-state \
	$(BUILD_DIR)/test-auto-lock \
	$(BUILD_DIR)/test-session-actions \
	$(BUILD_DIR)/test-task-manager-data \
	$(BUILD_DIR)/test-wifi-backend
	$(BUILD_DIR)/test-applications
	$(BUILD_DIR)/test-app-icons
	WIN31X_ICON_DIR=assets/icons $(BUILD_DIR)/test-icon-assets
	$(BUILD_DIR)/test-settings
	$(BUILD_DIR)/test-desktop-state
	$(BUILD_DIR)/test-auto-lock
	$(BUILD_DIR)/test-session-actions
	$(BUILD_DIR)/test-task-manager-data
	$(BUILD_DIR)/test-wifi-backend
	./tests/check-icon-provenance.sh

smoke: all $(BUILD_DIR)/wm-probe $(BUILD_DIR)/preexisting-client \
	$(BUILD_DIR)/test-auto-lock $(BUILD_DIR)/test-session-actions \
	$(BUILD_DIR)/test-task-manager-data $(BUILD_DIR)/test-wifi-backend
	./tests/smoke-x11.sh

smoke-xvfb: all $(BUILD_DIR)/wm-probe $(BUILD_DIR)/preexisting-client \
	$(BUILD_DIR)/persistence-probe \
	$(BUILD_DIR)/test-auto-lock $(BUILD_DIR)/test-session-actions \
	$(BUILD_DIR)/test-task-manager-data $(BUILD_DIR)/test-wifi-backend \
	check-xvfb-deps
	xvfb-run -a -s "-screen 0 1024x768x24" ./tests/smoke-x11.sh
	WIN31X_TEST_MONITORS='800x600+0+0,800x600+800+100' \
		xvfb-run -a -s "-screen 0 1600x700x24" ./tests/smoke-x11.sh
	./tests/persistence-x11.sh

smoke-multimon-xvfb: all $(BUILD_DIR)/wm-probe \
	$(BUILD_DIR)/test-auto-lock $(BUILD_DIR)/test-session-actions \
	$(BUILD_DIR)/test-task-manager-data $(BUILD_DIR)/test-wifi-backend \
	check-xvfb-deps
	WIN31X_TEST_MONITORS='800x600+0+0,800x600+800+100' \
		xvfb-run -a -s "-screen 0 1600x700x24" ./tests/smoke-x11.sh

smoke-persistence-xvfb: all $(BUILD_DIR)/persistence-probe check-xvfb-deps
	./tests/persistence-x11.sh

install: all
	install -d "$(DESTDIR)$(bindir)"
	install -m 0755 $(BUILD_DIR)/win31x "$(DESTDIR)$(bindir)/win31x"
	install -m 0755 scripts/win31x-session "$(DESTDIR)$(bindir)/win31x-session"
	install -d "$(DESTDIR)$(datadir)/xsessions"
	install -m 0644 data/win31x.desktop "$(DESTDIR)$(datadir)/xsessions/win31x.desktop"
	install -d "$(DESTDIR)$(icondir)"
	install -m 0644 assets/icons/*.png "$(DESTDIR)$(icondir)/"
	install -d "$(DESTDIR)$(appicon16dir)"
	install -m 0644 assets/icons/applications-16.png \
		"$(DESTDIR)$(appicon16dir)/win31x.png"
	install -d "$(DESTDIR)$(appicon48dir)"
	install -m 0644 assets/icons/applications-48.png \
		"$(DESTDIR)$(appicon48dir)/win31x.png"
	@if [ -z "$(DESTDIR)" ] && command -v gtk-update-icon-cache >/dev/null 2>&1; then \
		gtk-update-icon-cache -q "$(datadir)/icons/hicolor" || true; \
	fi
	install -d "$(DESTDIR)$(mandir)/man1"
	install -m 0644 man/win31x.1 "$(DESTDIR)$(mandir)/man1/win31x.1"
	install -m 0644 man/win31x-session.1 "$(DESTDIR)$(mandir)/man1/win31x-session.1"

uninstall:
	rm -f "$(DESTDIR)$(bindir)/win31x"
	rm -f "$(DESTDIR)$(bindir)/win31x-session"
	rm -f "$(DESTDIR)$(datadir)/xsessions/win31x.desktop"
	@for icon in $(ICON_FILES); do \
		rm -f "$(DESTDIR)$(icondir)/$$icon"; \
	done
	rmdir "$(DESTDIR)$(icondir)" 2>/dev/null || true
	rm -f "$(DESTDIR)$(appicon16dir)/win31x.png"
	rm -f "$(DESTDIR)$(appicon48dir)/win31x.png"
	@if [ -z "$(DESTDIR)" ] && command -v gtk-update-icon-cache >/dev/null 2>&1; then \
		gtk-update-icon-cache -q "$(datadir)/icons/hicolor" || true; \
	fi
	rm -f "$(DESTDIR)$(mandir)/man1/win31x.1"
	rm -f "$(DESTDIR)$(mandir)/man1/win31x-session.1"

clean:
	rm -rf "$(BUILD_DIR)"
