#ifndef WIN31X_DESKTOP_STATE_H
#define WIN31X_DESKTOP_STATE_H

#include <stdbool.h>
#include <stddef.h>

#define WIN31X_DESKTOP_STATE_VERSION 2U
#define WIN31X_DESKTOP_CLIENT_MAX 128U
#define WIN31X_DESKTOP_ID_CAPACITY 256U
#define WIN31X_DESKTOP_MONITOR_CAPACITY 128U
#define WIN31X_DESKTOP_IDENTITY_MAX (WIN31X_DESKTOP_ID_CAPACITY - 1U)
#define WIN31X_DESKTOP_MONITOR_NAME_MAX \
    (WIN31X_DESKTOP_MONITOR_CAPACITY - 1U)
#define WIN31X_DESKTOP_DIMENSION_MAX 65535

/*
 * Values deliberately match the window manager's normal/maximized/left/right
 * layout values. Unknown values read from disk are normalized to NORMAL.
 */
typedef enum {
    WIN31X_DESKTOP_LAYOUT_NORMAL = 0,
    WIN31X_DESKTOP_LAYOUT_MAXIMIZED = 1,
    WIN31X_DESKTOP_LAYOUT_SNAPPED_LEFT = 2,
    WIN31X_DESKTOP_LAYOUT_SNAPPED_RIGHT = 3
} Win31xDesktopLayout;

/*
 * A placement is monitor-relative so it survives monitor rearrangement.
 * monitor_center_x/y record the monitor's former absolute center and provide a
 * fallback when its name is no longer present. Dimensions are X11-sized and
 * must be in the inclusive range 1..WIN31X_DESKTOP_DIMENSION_MAX whenever
 * valid is true. layout_before_maximize remembers whether a maximized window
 * came from a normal, left-snapped, or right-snapped layout; MAXIMIZED and
 * unknown values are normalized to NORMAL when stored.
 */
typedef struct {
    bool valid;
    char monitor_name[WIN31X_DESKTOP_MONITOR_CAPACITY];
    int monitor_center_x;
    int monitor_center_y;
    int relative_x;
    int relative_y;
    int width;
    int height;
    Win31xDesktopLayout layout;
    Win31xDesktopLayout layout_before_maximize;
} Win31xDesktopPlacement;

typedef struct {
    char identity[WIN31X_DESKTOP_ID_CAPACITY];
    Win31xDesktopPlacement placement;
} Win31xDesktopClientRecord;

typedef struct {
    Win31xDesktopPlacement applications_icon;
    Win31xDesktopPlacement control_panel_icon;
    Win31xDesktopPlacement launcher;
    Win31xDesktopPlacement control_panel;
    Win31xDesktopPlacement run_dialog;
    Win31xDesktopClientRecord clients[WIN31X_DESKTOP_CLIENT_MAX];
    size_t client_count;
    /* False when an unsupported on-disk schema was loaded and preserved. */
    bool write_enabled;
} Win31xDesktopState;

void win31x_desktop_placement_defaults(Win31xDesktopPlacement *placement);
void win31x_desktop_state_defaults(Win31xDesktopState *state);

/*
 * Returns true only for a usable placement (including a bounded monitor name
 * and positive dimensions). An unknown layout is usable and is normalized to
 * NORMAL by the upsert/save path. layout_before_maximize is independently
 * normalized to NORMAL, SNAPPED_LEFT, or SNAPPED_RIGHT.
 */
bool win31x_desktop_placement_is_valid(
    const Win31xDesktopPlacement *placement);

/*
 * Load ~/.config/win31x/layout.conf, or the equivalent absolute
 * $XDG_CONFIG_HOME path. Missing files produce defaults. Malformed placement
 * and client lines are ignored independently; an absent or unsupported
 * version leaves the entire state at defaults. A syntactically valid
 * unsupported version also clears state->write_enabled so a subsequent
 * save cannot replace data this version does not understand. I/O and
 * security failures are reported as -1 with errno set.
 *
 * Version 2 is a whitespace-delimited, one-record-per-line format:
 *
 *   version 2
 *   applications_icon VALID MONITOR_HEX CX CY X Y WIDTH HEIGHT LAYOUT PREV
 *   control_panel_icon VALID MONITOR_HEX CX CY X Y WIDTH HEIGHT LAYOUT PREV
 *   launcher VALID MONITOR_HEX CX CY X Y WIDTH HEIGHT LAYOUT PREV
 *   control_panel VALID MONITOR_HEX CX CY X Y WIDTH HEIGHT LAYOUT PREV
 *   run_dialog VALID MONITOR_HEX CX CY X Y WIDTH HEIGHT LAYOUT PREV
 *   client IDENTITY_HEX VALID MONITOR_HEX CX CY X Y WIDTH HEIGHT LAYOUT PREV
 *
 * Strings are bytewise hexadecimal; "-" represents an empty monitor name.
 * VALID is 0 or 1 and LAYOUT uses Win31xDesktopLayout's numeric values. The
 * loader also accepts legacy version 1 records, where PREV was absent and
 * defaults to NORMAL. A PREV field in version 1 is tolerated for migration;
 * saves always write version 2 and include PREV.
 */
int win31x_desktop_state_load(Win31xDesktopState *state);

/*
 * Save atomically with a private 0600 file inside a private 0700 directory.
 * Returns -1/ENOTSUP without touching disk when write_enabled is false.
 */
int win31x_desktop_state_save(const Win31xDesktopState *state);

/* Find a client by its exact, non-empty, bounded identity. */
const Win31xDesktopClientRecord *win31x_desktop_state_find_client(
    const Win31xDesktopState *state, const char *identity);
Win31xDesktopClientRecord *win31x_desktop_state_find_client_mutable(
    Win31xDesktopState *state, const char *identity);

/*
 * Insert or replace a client placement. The placement must be valid. Records
 * are kept oldest-to-newest: replacing one refreshes it to the newest slot,
 * and inserting into a full state evicts the oldest record deterministically.
 * Returns -1/EINVAL for invalid input.
 */
int win31x_desktop_state_upsert_client(
    Win31xDesktopState *state, const char *identity,
    const Win31xDesktopPlacement *placement);

#endif
