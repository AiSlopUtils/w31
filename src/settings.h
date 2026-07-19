#ifndef WIN31X_SETTINGS_H
#define WIN31X_SETTINGS_H

#include <stdbool.h>
#include <stddef.h>

#define WIN31X_COLOR_SCHEME_COUNT 5U
#define WIN31X_AUTO_LOCK_MINUTES_MIN 1U
#define WIN31X_AUTO_LOCK_MINUTES_MAX 120U

typedef enum {
    WIN31X_CONTROL_PANEL_SECTION_WIFI = 0,
    WIN31X_CONTROL_PANEL_SECTION_COLORS,
    WIN31X_CONTROL_PANEL_SECTION_AUTO_LOCK,
    WIN31X_CONTROL_PANEL_SECTION_COUNT
} Win31xControlPanelSection;

typedef struct {
    const char *id;
    const char *name;
    const char *desktop_hex;
    const char *active_title_hex;
} Win31xColorScheme;

typedef struct {
    size_t color_scheme;
    bool auto_lock_enabled;
    unsigned int auto_lock_minutes;
    Win31xControlPanelSection control_panel_section;
} Win31xSettings;

extern const Win31xColorScheme
    win31x_color_schemes[WIN31X_COLOR_SCHEME_COUNT];

const Win31xColorScheme *win31x_color_scheme(size_t index);
void win31x_settings_defaults(Win31xSettings *settings);

/*
 * Load settings from the XDG user configuration directory. Missing files and
 * malformed individual values leave their defaults in place. I/O and security
 * errors are reported with -1 and errno.
 */
int win31x_settings_load(Win31xSettings *settings);

/* Save settings atomically with a private file and directory mode. */
int win31x_settings_save(const Win31xSettings *settings);

#endif
