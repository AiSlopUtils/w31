#define _POSIX_C_SOURCE 200809L

#include "applications.h"
#include "app_icons.h"
#include "auto_lock.h"
#include "desktop_state.h"
#include "icon_assets.h"
#include "session_actions.h"
#include "settings.h"
#include "task_manager_data.h"
#include "wifi_backend.h"

#include <X11/Xatom.h>
#include <X11/XKBlib.h>
#include <X11/Xlib.h>
#include <X11/Xutil.h>
#include <X11/cursorfont.h>
#include <X11/extensions/Xrandr.h>
#include <X11/extensions/Xinerama.h>
#include <X11/keysym.h>

#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <signal.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/select.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#define WIN31X_NAME "Win31 X"
#define FRAME_LEFT 3
#define FRAME_RIGHT 3
#define FRAME_TOP 25
#define FRAME_BOTTOM 3
#define TITLE_Y 3
#define TITLE_HEIGHT 20
#define TITLE_BUTTON 17
#define MIN_CLIENT_WIDTH 96
#define MIN_CLIENT_HEIGHT 48
#define ICON_WIDTH 112
#define ICON_HEIGHT 80
#define ICON_GAP 8
#define ICON_MARGIN 12
#define LAUNCHER_DEFAULT_WIDTH 680
#define LAUNCHER_DEFAULT_HEIGHT 460
#define LAUNCHER_CELL_WIDTH 124
#define LAUNCHER_CELL_HEIGHT 82
#define CONTROL_PANEL_DEFAULT_WIDTH 680
#define CONTROL_PANEL_DEFAULT_HEIGHT 460
#define CONTROL_PANEL_NAV_WIDTH 148
#define CONTROL_PANEL_NAV_TOP 34
#define CONTROL_PANEL_NAV_ITEM_HEIGHT 88
#define CONTROL_PANEL_NAV_GAP 6
#define CONTROL_PANEL_PASSWORD_CAPACITY 128
#define RUN_DIALOG_DEFAULT_WIDTH 470
#define RUN_DIALOG_DEFAULT_HEIGHT 176
#define RUN_COMMAND_CAPACITY 512
#define TASK_MANAGER_DEFAULT_WIDTH 720
#define TASK_MANAGER_DEFAULT_HEIGHT 520
#define TASK_MANAGER_REFRESH_MS UINT64_C(1000)
#define TASK_MANAGER_CONFIRM_MS UINT64_C(5000)
#define TASK_MANAGER_FORCE_DELAY_MS UINT64_C(3000)
#define TASK_MANAGER_HISTORY_LENGTH 120U
#define TASK_MANAGER_ROW_HEIGHT 20
#define TASK_MANAGER_TAB_HEIGHT 25
#define TASK_MANAGER_STATUS_CAPACITY 192
#define DESKTOP_MENU_WIDTH 196
#define DESKTOP_MENU_HEIGHT 119
#define DESKTOP_MENU_ITEM_HEIGHT 26
#define SESSION_CONFIRM_DEFAULT_WIDTH 420
#define SESSION_CONFIRM_DEFAULT_HEIGHT 176
#define SESSION_CONFIRM_BUTTON_WIDTH 78
#define SESSION_CONFIRM_BUTTON_HEIGHT 26
#define SESSION_CONFIRM_BUTTON_GAP 8
#define SESSION_CONFIRM_BUTTON_MARGIN 13
#define DOUBLE_CLICK_MS 450
#define SNAP_THRESHOLD 8
#define DRAG_OUTLINE_THICKNESS 3
#define DRAG_OUTLINE_WINDOW_COUNT 4
#define CLIENT_ICON_PROPERTY_LIMIT (1024UL * 1024UL)
#define MAX_MONITORS 32
#define DESKTOP_STATE_SAVE_DELAY_MS UINT64_C(750)
#define ICON_DRAG_SLOP 5
#define STATE_IDENTITY_INSTANCE_RESERVE 24U

typedef struct Client Client;
typedef struct DesktopIcon DesktopIcon;

typedef struct {
    Atom name;
    int x;
    int y;
    int width;
    int height;
    bool primary;
} MonitorGeometry;

typedef struct {
    Atom name;
    int x;
    int y;
    bool valid;
} MonitorAnchor;

typedef struct {
    Pixmap color;
    Pixmap mask;
    unsigned int width;
    unsigned int height;
} RenderedIcon;

typedef enum {
    CLIENT_LAYOUT_NORMAL,
    CLIENT_LAYOUT_MAXIMIZED,
    CLIENT_LAYOUT_SNAP_LEFT,
    CLIENT_LAYOUT_SNAP_RIGHT
} ClientLayout;

typedef enum {
    INTERNAL_FOCUS_NONE,
    INTERNAL_FOCUS_APPLICATIONS,
    INTERNAL_FOCUS_CONTROL_PANEL,
    INTERNAL_FOCUS_RUN,
    INTERNAL_FOCUS_TASK_MANAGER
} InternalFocus;

typedef enum {
    TITLE_GLYPH_MINIMIZE,
    TITLE_GLYPH_MAXIMIZE,
    TITLE_GLYPH_RESTORE,
    TITLE_GLYPH_CLOSE
} TitleGlyph;

typedef enum {
    ICON_APPLICATIONS,
    ICON_CONTROL_PANEL,
    ICON_MINIMIZED
} IconKind;

struct DesktopIcon {
    Window window;
    IconKind kind;
    Client *client;
    MonitorAnchor monitor;
    int x;
    int y;
    bool manual_position;
    Win31xDesktopPlacement preferred;
    DesktopIcon *next;
};

struct Client {
    Window window;
    Window frame;
    Window focus_overlay;
    Window transient_for;
    Window minimized_with_owner;
    char *title;
    char *class_name;
    char *state_base_identity;
    char *state_identity;
    unsigned int state_instance;
    bool state_persistable;
    bool state_restored;
    bool state_monitor_fallback;
    bool initial_maximize_precedence;
    /* Root-relative coordinates of the client's inside (drawable) corner. */
    int x;
    int y;
    int width;
    int height;
    int win_gravity;
    unsigned int saved_border;
    unsigned int expected_unmaps;
    bool minimized;
    ClientLayout layout;
    ClientLayout layout_before_maximize;
    bool restore_valid;
    int restore_x;
    int restore_y;
    int restore_width;
    int restore_height;
    MonitorAnchor layout_monitor;
    DesktopIcon *icon;
    RenderedIcon actual_icon_small;
    RenderedIcon actual_icon_large;
    Client *next;
};

typedef struct {
    unsigned long black;
    unsigned long white;
    unsigned long silver;
    unsigned long dark_gray;
    unsigned long graph_green;
    unsigned long graph_grid;
    unsigned long active_title;
    unsigned long desktop;
    unsigned long scheme_desktop[WIN31X_COLOR_SCHEME_COUNT];
    unsigned long scheme_active_title[WIN31X_COLOR_SCHEME_COUNT];
} Theme;

typedef enum {
    APPLICATION_ICON_UNTRIED,
    APPLICATION_ICON_READY,
    APPLICATION_ICON_FAILED
} ApplicationIconState;

typedef struct {
    RenderedIcon rendered;
    ApplicationIconState state;
} ApplicationRenderedIcon;

typedef struct {
    Atom wm_protocols;
    Atom wm_delete_window;
    Atom wm_take_focus;
    Atom wm_change_state;
    Atom wm_state;
    Atom utf8_string;
    Atom net_supported;
    Atom net_supporting_wm_check;
    Atom net_wm_name;
    Atom net_wm_icon_name;
    Atom net_wm_icon;
    Atom net_client_list;
    Atom net_active_window;
    Atom net_close_window;
    Atom net_frame_extents;
    Atom net_number_of_desktops;
    Atom net_current_desktop;
    Atom net_workarea;
    Atom net_wm_state;
    Atom net_wm_state_hidden;
    Atom net_wm_state_maximized_horz;
    Atom net_wm_state_maximized_vert;
    Atom net_wm_desktop_file;
    Atom gtk_application_id;
    Atom wm_window_role;
    Atom win31x_role;
    Atom win31x_client;
    Atom win31x_task_manager_tab;
    Atom win31x_task_manager_sample_serial;
    Atom win31x_task_manager_cpu_tenths;
    Atom win31x_task_manager_memory_tenths;
    Atom win31x_task_manager_process_count;
    Atom win31x_task_manager_selected_pid;
} Atoms;

typedef enum {
    DRAG_NONE,
    DRAG_MOVE_CLIENT,
    DRAG_RESIZE_CLIENT,
    DRAG_MOVE_LAUNCHER,
    DRAG_MOVE_CONTROL_PANEL,
    DRAG_MOVE_RUN,
    DRAG_MOVE_TASK_MANAGER,
    DRAG_MOVE_ICON
} DragKind;

typedef enum {
    CONTROL_SECTION_WIFI,
    CONTROL_SECTION_COLORS,
    CONTROL_SECTION_AUTO_LOCK,
    CONTROL_SECTION_COUNT
} ControlSection;

typedef struct {
    Window window;
    int x;
    int y;
    int width;
    int height;
    bool visible;
    ClientLayout layout;
    ClientLayout layout_before_maximize;
    bool restore_valid;
    int restore_x;
    int restore_y;
    int restore_width;
    int restore_height;
    MonitorAnchor layout_monitor;
    ControlSection section;
    int wifi_selected;
    int wifi_scroll;
    bool password_active;
    char password[CONTROL_PANEL_PASSWORD_CAPACITY];
    size_t password_length;
    char settings_status[160];
} ControlPanel;

typedef struct {
    Window window;
    int x;
    int y;
    int width;
    int height;
    bool visible;
    bool positioned;
    char command[RUN_COMMAND_CAPACITY];
    size_t command_length;
    char status[160];
    InternalFocus return_internal_focus;
    Window return_client;
    MonitorAnchor monitor;
} RunDialog;

typedef enum {
    TASK_MANAGER_TAB_APPLICATIONS,
    TASK_MANAGER_TAB_PROCESSES,
    TASK_MANAGER_TAB_PERFORMANCE,
    TASK_MANAGER_TAB_SYSTEM,
    TASK_MANAGER_TAB_COUNT
} TaskManagerTab;

typedef struct {
    Window window;
    int x;
    int y;
    int width;
    int height;
    bool visible;
    bool positioned;
    ClientLayout layout;
    ClientLayout layout_before_maximize;
    bool restore_valid;
    int restore_x;
    int restore_y;
    int restore_width;
    int restore_height;
    MonitorAnchor layout_monitor;
    InternalFocus return_internal_focus;
    Window return_client;
    Pixmap backing;
    int backing_width;
    int backing_height;
    TaskManagerTab tab;
    Win31xTaskManagerData data;
    bool data_available;
    uint64_t next_refresh_ms;
    unsigned long sample_serial;
    double cpu_history[TASK_MANAGER_HISTORY_LENGTH];
    double memory_history[TASK_MANAGER_HISTORY_LENGTH];
    size_t history_count;
    Window selected_application;
    Window closing_application;
    Window application_last_click;
    Time application_last_click_time;
    int application_scroll;
    pid_t selected_pid;
    uint64_t selected_start_time;
    int process_scroll;
    pid_t confirm_pid;
    uint64_t confirm_start_time;
    uint64_t confirm_until_ms;
    pid_t terminating_pid;
    uint64_t terminating_start_time;
    uint64_t force_due_ms;
    bool force_ready;
    bool process_delete_down;
    bool status_is_refresh_error;
    bool status_is_closing_application;
    char status[TASK_MANAGER_STATUS_CAPACITY];
} TaskManager;

typedef enum {
    DESKTOP_MENU_LOCK,
    DESKTOP_MENU_LOG_OUT,
    DESKTOP_MENU_RESTART,
    DESKTOP_MENU_SHUT_DOWN,
    DESKTOP_MENU_ITEM_COUNT,
    DESKTOP_MENU_NONE = -1
} DesktopMenuItem;

typedef struct {
    Window window;
    int x;
    int y;
    int width;
    int height;
    bool visible;
    bool pointer_grabbed;
    bool keyboard_grabbed;
    DesktopMenuItem selected;
    DesktopMenuItem armed;
    unsigned int pressed_button;
    bool ignore_open_release;
    MonitorAnchor monitor;
} DesktopMenu;

typedef enum {
    SESSION_CONFIRM_YES,
    SESSION_CONFIRM_NO,
    SESSION_CONFIRM_CLOSE,
    SESSION_CONFIRM_NONE = -1
} SessionConfirmButton;

typedef struct {
    Window shield;
    Window window;
    int x;
    int y;
    int width;
    int height;
    bool visible;
    DesktopMenuItem action;
    SessionConfirmButton selected;
    SessionConfirmButton armed;
    unsigned int pressed_button;
    InternalFocus return_internal_focus;
    Window return_client;
    MonitorAnchor monitor;
} SessionConfirmation;

typedef struct {
    bool compact;
    int toggle_x;
    int toggle_y;
    int toggle_width;
    int toggle_height;
    int timeout_minus_x;
    int timeout_value_x;
    int timeout_plus_x;
    int timeout_y;
    int timeout_button_width;
    int timeout_value_width;
    int timeout_height;
    int lock_x;
    int lock_y;
    int lock_width;
    int lock_height;
} ControlAutoLayout;

enum {
    EDGE_LEFT = 1,
    EDGE_RIGHT = 2,
    EDGE_TOP = 4,
    EDGE_BOTTOM = 8
};

typedef struct {
    DragKind kind;
    Client *client;
    DesktopIcon *icon;
    int edges;
    int start_root_x;
    int start_root_y;
    int start_x;
    int start_y;
    int start_width;
    int start_height;
    int pending_x;
    int pending_y;
    int pending_width;
    int pending_height;
    bool moved;
    bool arranged_restore_prepared;
    bool outline_visible;
    bool keyboard_grabbed;
} DragState;

typedef struct {
    Display *display;
    int screen;
    Window root;
    Window support_window;
    Window launcher;
    int screen_width;
    int screen_height;
    int launcher_x;
    int launcher_y;
    int launcher_width;
    int launcher_height;
    bool launcher_visible;
    ClientLayout launcher_layout;
    ClientLayout launcher_layout_before_maximize;
    bool launcher_restore_valid;
    int launcher_restore_x;
    int launcher_restore_y;
    int launcher_restore_width;
    int launcher_restore_height;
    MonitorAnchor launcher_layout_monitor;
    int launcher_scroll_row;
    int launcher_selected;
    int launcher_last_click;
    Time launcher_last_click_time;
    Client *clients;
    Client *active;
    DesktopIcon *icons;
    DesktopIcon *applications_icon;
    DesktopIcon *control_panel_icon;
    AppList applications;
    ApplicationRenderedIcon *application_icons;
    size_t application_icon_count;
    MonitorGeometry monitors[MAX_MONITORS];
    size_t monitor_count;
    MonitorAnchor active_monitor;
    bool randr_available;
    int randr_event_base;
    int randr_error_base;
    int randr_major;
    int randr_minor;
    GC gc;
    Window drag_outline[DRAG_OUTLINE_WINDOW_COUNT];
    XFontStruct *font;
    Cursor arrow_cursor;
    Cursor move_cursor;
    Cursor resize_cursor;
    Theme theme;
    IconAssets icon_assets;
    RenderedIcon rendered_icons[ICON_CATEGORY_COUNT][ICON_SIZE_COUNT];
    Win31xSettings settings;
    Win31xDesktopState desktop_state;
    bool desktop_state_dirty;
    uint64_t desktop_state_due_ms;
    bool launcher_positioned;
    bool control_panel_positioned;
    Win31xAutoLock auto_lock;
    Win31xSessionActions session_actions;
    WifiBackend wifi;
    ControlPanel control_panel;
    RunDialog run_dialog;
    TaskManager task_manager;
    DesktopMenu desktop_menu;
    SessionConfirmation session_confirmation;
    InternalFocus internal_focus;
    unsigned int super_masks[8];
    size_t super_mask_count;
    unsigned int ignored_lock_mask;
    Atoms atoms;
    DragState drag;
} WindowManager;

static volatile sig_atomic_t keep_running = 1;
static volatile sig_atomic_t child_changed = 0;
static int startup_bad_access = 0;

static void dismiss_launcher(WindowManager *wm);
static void dismiss_control_panel(WindowManager *wm);
static void draw_launcher(WindowManager *wm);
static void draw_control_panel(WindowManager *wm);
static void draw_run_dialog(WindowManager *wm);
static void hide_run_dialog(WindowManager *wm);
static void draw_task_manager(WindowManager *wm);
static void hide_task_manager(WindowManager *wm);
static void show_task_manager(WindowManager *wm);
static void task_manager_select_first_application(WindowManager *wm);
static void dismiss_desktop_menu(WindowManager *wm);
static void dismiss_session_confirmation(WindowManager *wm, bool restore_focus,
                                         Time time);
static void draw_session_confirmation(WindowManager *wm);
static void refocus_session_confirmation(WindowManager *wm, Time time);
static void draw_frame(WindowManager *wm, Client *client);
static void draw_desktop_icon(WindowManager *wm, DesktopIcon *icon);
static void update_focus_overlays(WindowManager *wm);
static void activate_internal_window(WindowManager *wm, InternalFocus focus,
                                     Time time);
static void raise_focused_internal_window(WindowManager *wm);
static void apply_client_geometry(WindowManager *wm, Client *client);
static void send_configure_notify(WindowManager *wm, Client *client);
static void reapply_client_layout(WindowManager *wm, Client *client);
static void cancel_drag(WindowManager *wm, Time time);
static void clamp_geometry_to_monitor(const MonitorGeometry *monitor, int *x,
                                      int *y, int *width, int *height);
static const MonitorGeometry *monitor_at(const WindowManager *wm,
                                         size_t index);

static void handle_signal(int signal_number)
{
    (void)signal_number;
    keep_running = 0;
}

static void handle_child_signal(int signal_number)
{
    (void)signal_number;
    child_changed = 1;
}

static int startup_error_handler(Display *display, XErrorEvent *event)
{
    (void)display;
    if (event->error_code == BadAccess)
        startup_bad_access = 1;
    return 0;
}

static int runtime_error_handler(Display *display, XErrorEvent *event)
{
    char message[256];

    if (event->error_code == BadWindow || event->error_code == BadDrawable ||
        event->error_code == BadMatch)
        return 0;
    XGetErrorText(display, event->error_code, message, sizeof(message));
    fprintf(stderr, "win31x: X11 error: %s (request %u.%u, resource 0x%lx)\n",
            message, event->request_code, event->minor_code, event->resourceid);
    return 0;
}

static bool monitor_geometry_equal(const MonitorGeometry *left,
                                   const MonitorGeometry *right)
{
    return left->name == right->name && left->x == right->x &&
           left->y == right->y && left->width == right->width &&
           left->height == right->height && left->primary == right->primary;
}

static bool monitor_sort_before(const MonitorGeometry *left,
                                const MonitorGeometry *right)
{
    if (left->primary != right->primary)
        return left->primary;
    if (left->y != right->y)
        return left->y < right->y;
    if (left->x != right->x)
        return left->x < right->x;
    if (left->height != right->height)
        return left->height < right->height;
    if (left->width != right->width)
        return left->width < right->width;
    return left->name < right->name;
}

static void sort_monitors(MonitorGeometry *monitors, size_t count)
{
    size_t index;

    for (index = 1U; index < count; ++index) {
        MonitorGeometry current = monitors[index];
        size_t position = index;

        while (position > 0U &&
               monitor_sort_before(&current, &monitors[position - 1U])) {
            monitors[position] = monitors[position - 1U];
            --position;
        }
        monitors[position] = current;
    }
}

static void add_monitor_geometry(WindowManager *wm, MonitorGeometry *monitors,
                                 size_t *count, Atom name, int x, int y,
                                 int width, int height, bool primary)
{
    long long right = (long long)x + width;
    long long bottom = (long long)y + height;
    size_t index;

    if (width <= 0 || height <= 0 || wm->screen_width <= 0 ||
        wm->screen_height <= 0)
        return;
    if (x < 0)
        x = 0;
    if (y < 0)
        y = 0;
    if (right > wm->screen_width)
        right = wm->screen_width;
    if (bottom > wm->screen_height)
        bottom = wm->screen_height;
    if (right <= x || bottom <= y)
        return;
    width = (int)(right - x);
    height = (int)(bottom - y);
    for (index = 0U; index < *count; ++index) {
        MonitorGeometry *existing = &monitors[index];

        if (existing->x == x && existing->y == y &&
            existing->width == width && existing->height == height) {
            if (primary) {
                existing->primary = true;
                existing->name = name;
            }
            return;
        }
    }
    if (*count >= MAX_MONITORS)
        return;
    monitors[*count] = (MonitorGeometry){
        name, x, y, width, height, primary
    };
    ++*count;
}

static size_t monitor_override_layout(WindowManager *wm,
                                      MonitorGeometry *monitors)
{
    const char *layout = getenv("WIN31X_TEST_MONITORS");
    const char *cursor = layout;
    size_t count = 0U;

    if (layout == NULL || layout[0] == '\0')
        return 0U;
    while (*cursor != '\0' && count < MAX_MONITORS) {
        char name[64];
        int width;
        int height;
        int x;
        int y;
        int consumed = 0;

        if (sscanf(cursor, "%dx%d%d%d%n", &width, &height, &x, &y,
                   &consumed) != 4 || consumed <= 0)
            return 0U;
        snprintf(name, sizeof(name), "_WIN31X_MONITOR_%zu", count);
        add_monitor_geometry(wm, monitors, &count,
                             XInternAtom(wm->display, name, False), x, y,
                             width, height, count == 0U);
        cursor += consumed;
        if (*cursor == ',')
            ++cursor;
        else if (*cursor != '\0')
            return 0U;
    }
    return count;
}

static bool refresh_monitor_layout(WindowManager *wm)
{
    MonitorGeometry detected[MAX_MONITORS];
    size_t count = monitor_override_layout(wm, detected);
    bool override_layout = count > 0U;
    bool randr_has_explicit_monitor = false;
    size_t index;

    if (count == 0U && wm->randr_available) {
        int monitor_count = 0;
        XRRMonitorInfo *information =
            XRRGetMonitors(wm->display, wm->root, True, &monitor_count);
        int monitor_index;

        for (monitor_index = 0;
             information != NULL && monitor_index < monitor_count;
             ++monitor_index) {
            XRRMonitorInfo *monitor = &information[monitor_index];
            size_t previous_count = count;

            add_monitor_geometry(wm, detected, &count, monitor->name,
                                 monitor->x, monitor->y,
                                 monitor->width, monitor->height,
                                 monitor->primary != False);
            if (count > previous_count && monitor->automatic == False)
                randr_has_explicit_monitor = true;
        }
        if (information != NULL)
            XRRFreeMonitors(information);
    }
    if (!override_layout && count <= 1U &&
        !randr_has_explicit_monitor && XineramaIsActive(wm->display)) {
        int screen_count = 0;
        XineramaScreenInfo *screens =
            XineramaQueryScreens(wm->display, &screen_count);

        if (screens != NULL && screen_count > 1) {
            int screen_index;

            count = 0U;
            for (screen_index = 0; screen_index < screen_count;
                 ++screen_index) {
                char name[64];

                snprintf(name, sizeof(name), "_WIN31X_XINERAMA_%d",
                         screens[screen_index].screen_number);
                add_monitor_geometry(
                    wm, detected, &count,
                    XInternAtom(wm->display, name, False),
                    screens[screen_index].x_org,
                    screens[screen_index].y_org,
                    screens[screen_index].width,
                    screens[screen_index].height, screen_index == 0);
            }
        }
        if (screens != NULL)
            XFree(screens);
    }
    if (count == 0U) {
        add_monitor_geometry(wm, detected, &count, None, 0, 0,
                             wm->screen_width, wm->screen_height, true);
    }
    sort_monitors(detected, count);
    if (count == wm->monitor_count) {
        for (index = 0U; index < count; ++index) {
            if (!monitor_geometry_equal(&detected[index],
                                        &wm->monitors[index]))
                break;
        }
        if (index == count)
            return false;
    }
    memcpy(wm->monitors, detected, count * sizeof(detected[0]));
    wm->monitor_count = count;
    return true;
}

static long long distance_to_monitor(const MonitorGeometry *monitor,
                                     int x, int y)
{
    long long right = (long long)monitor->x + monitor->width - 1;
    long long bottom = (long long)monitor->y + monitor->height - 1;
    long long dx = 0;
    long long dy = 0;

    if (x < monitor->x)
        dx = (long long)monitor->x - x;
    else if ((long long)x > right)
        dx = (long long)x - right;
    if (y < monitor->y)
        dy = (long long)monitor->y - y;
    else if ((long long)y > bottom)
        dy = (long long)y - bottom;
    return dx + dy;
}

static size_t monitor_index_for_point(const WindowManager *wm, int x, int y)
{
    size_t best = 0U;
    long long best_distance = LLONG_MAX;
    size_t index;

    for (index = 0U; index < wm->monitor_count; ++index) {
        long long distance = distance_to_monitor(&wm->monitors[index], x, y);

        if (distance < best_distance) {
            best = index;
            best_distance = distance;
        }
    }
    return best;
}

static size_t monitor_index_for_rectangle(const WindowManager *wm, int x,
                                          int y, int width, int height)
{
    size_t center_monitor = monitor_index_for_point(
        wm, x + (width > 0 ? width / 2 : 0),
        y + (height > 0 ? height / 2 : 0));
    size_t best = center_monitor;
    long long best_area = 0;
    size_t index;

    for (index = 0U; index < wm->monitor_count; ++index) {
        const MonitorGeometry *monitor = &wm->monitors[index];
        long long left = x > monitor->x ? x : monitor->x;
        long long top = y > monitor->y ? y : monitor->y;
        long long right = (long long)x + (width > 0 ? width : 1);
        long long bottom = (long long)y + (height > 0 ? height : 1);
        long long monitor_right = (long long)monitor->x + monitor->width;
        long long monitor_bottom = (long long)monitor->y + monitor->height;
        long long area;

        if (right > monitor_right)
            right = monitor_right;
        if (bottom > monitor_bottom)
            bottom = monitor_bottom;
        area = right > left && bottom > top
                   ? (right - left) * (bottom - top)
                   : 0;
        if (area > best_area ||
            (area == best_area && index == center_monitor)) {
            best = index;
            best_area = area;
        }
    }
    return best;
}

static size_t monitor_index_for_anchor(const WindowManager *wm,
                                       const MonitorAnchor *anchor)
{
    size_t index;

    if (anchor != NULL && anchor->valid && anchor->name != None) {
        for (index = 0U; index < wm->monitor_count; ++index) {
            if (wm->monitors[index].name == anchor->name)
                return index;
        }
    }
    if (anchor != NULL && anchor->valid)
        return monitor_index_for_point(wm, anchor->x, anchor->y);
    return 0U;
}

static void set_monitor_anchor(MonitorAnchor *anchor,
                               const MonitorGeometry *monitor)
{
    anchor->name = monitor->name;
    anchor->x = monitor->x + monitor->width / 2;
    anchor->y = monitor->y + monitor->height / 2;
    anchor->valid = true;
}

static uint64_t monotonic_milliseconds(void)
{
    struct timespec now;

    if (clock_gettime(CLOCK_MONOTONIC, &now) < 0)
        return 0U;
    return (uint64_t)now.tv_sec * UINT64_C(1000) +
           (uint64_t)now.tv_nsec / UINT64_C(1000000);
}

static void mark_desktop_state_dirty(WindowManager *wm)
{
    if (!wm->desktop_state.write_enabled)
        return;
    wm->desktop_state_dirty = true;
    wm->desktop_state_due_ms =
        monotonic_milliseconds() + DESKTOP_STATE_SAVE_DELAY_MS;
}

static void flush_desktop_state(WindowManager *wm, bool force)
{
    uint64_t now;

    if (!wm->desktop_state_dirty)
        return;
    if (!wm->desktop_state.write_enabled) {
        wm->desktop_state_dirty = false;
        return;
    }
    now = monotonic_milliseconds();
    if (!force && now != 0U && now < wm->desktop_state_due_ms)
        return;
    if (win31x_desktop_state_save(&wm->desktop_state) < 0) {
        fprintf(stderr, "win31x: could not save desktop layout: %s\n",
                strerror(errno));
        wm->desktop_state_due_ms = now + DESKTOP_STATE_SAVE_DELAY_MS;
        return;
    }
    wm->desktop_state_dirty = false;
}

static void monitor_name(WindowManager *wm, const MonitorGeometry *monitor,
                         char *name, size_t capacity)
{
    char *atom_name;

    if (capacity == 0U)
        return;
    name[0] = '\0';
    if (monitor == NULL || monitor->name == None)
        return;
    atom_name = XGetAtomName(wm->display, monitor->name);
    if (atom_name == NULL)
        return;
    snprintf(name, capacity, "%s", atom_name);
    XFree(atom_name);
}

static bool monitor_has_saved_name(WindowManager *wm,
                                   const MonitorGeometry *monitor,
                                   const char *saved_name)
{
    char current[WIN31X_DESKTOP_MONITOR_NAME_MAX + 1U];

    if (saved_name == NULL || saved_name[0] == '\0')
        return monitor != NULL && monitor->name == None;
    monitor_name(wm, monitor, current, sizeof(current));
    return strcmp(current, saved_name) == 0;
}

static bool placement_monitor_is_available(
    WindowManager *wm, const Win31xDesktopPlacement *placement)
{
    size_t index;

    if (placement == NULL || !placement->valid)
        return false;
    if (placement->monitor_name[0] == '\0')
        return true;
    for (index = 0U; index < wm->monitor_count; ++index) {
        if (monitor_has_saved_name(wm, &wm->monitors[index],
                                   placement->monitor_name))
            return true;
    }
    return false;
}

static size_t monitor_index_for_placement(
    WindowManager *wm, const Win31xDesktopPlacement *placement)
{
    size_t index;

    if (placement != NULL && placement->valid &&
        placement->monitor_name[0] != '\0') {
        for (index = 0U; index < wm->monitor_count; ++index) {
            if (monitor_has_saved_name(wm, &wm->monitors[index],
                                       placement->monitor_name))
                return index;
        }
    }
    if (placement != NULL && placement->valid)
        return monitor_index_for_point(wm, placement->monitor_center_x,
                                       placement->monitor_center_y);
    return 0U;
}

/* Leave enough headroom for frame decorations and the largest placement
 * dimension before these coordinates are used by Xlib geometry helpers. */
static int safe_placement_coordinate(long long value)
{
    const long long margin =
        (long long)WIN31X_DESKTOP_DIMENSION_MAX + FRAME_TOP + FRAME_BOTTOM + 1;
    const long long minimum = (long long)INT_MIN + margin;
    const long long maximum = (long long)INT_MAX - margin;

    if (value < minimum)
        return (int)minimum;
    if (value > maximum)
        return (int)maximum;
    return (int)value;
}

static void placement_from_geometry(
    WindowManager *wm, Win31xDesktopPlacement *placement, size_t monitor_index,
    int x, int y, int width, int height, ClientLayout layout)
{
    const MonitorGeometry *monitor = monitor_at(wm, monitor_index);

    win31x_desktop_placement_defaults(placement);
    if (monitor == NULL || width < 1 || height < 1)
        return;
    monitor_name(wm, monitor, placement->monitor_name,
                 sizeof(placement->monitor_name));
    placement->monitor_center_x = safe_placement_coordinate(
        (long long)monitor->x + monitor->width / 2);
    placement->monitor_center_y = safe_placement_coordinate(
        (long long)monitor->y + monitor->height / 2);
    placement->relative_x = safe_placement_coordinate(
        (long long)x - monitor->x);
    placement->relative_y = safe_placement_coordinate(
        (long long)y - monitor->y);
    placement->width = width;
    placement->height = height;
    placement->layout = (Win31xDesktopLayout)layout;
    placement->valid = true;
    placement->valid = win31x_desktop_placement_is_valid(placement);
}

static size_t geometry_from_placement(
    WindowManager *wm, const Win31xDesktopPlacement *placement, int *x, int *y,
    int *width, int *height, bool clamp)
{
    size_t monitor_index = monitor_index_for_placement(wm, placement);
    const MonitorGeometry *monitor = monitor_at(wm, monitor_index);

    if (monitor == NULL || placement == NULL || !placement->valid)
        return monitor_index;
    *x = safe_placement_coordinate((long long)monitor->x +
                                   placement->relative_x);
    *y = safe_placement_coordinate((long long)monitor->y +
                                   placement->relative_y);
    *width = placement->width;
    *height = placement->height;
    if (clamp)
        clamp_geometry_to_monitor(monitor, x, y, width, height);
    return monitor_index;
}

static void clamp_geometry_to_monitor(const MonitorGeometry *monitor, int *x,
                                      int *y, int *width, int *height)
{
    if (*width < 1)
        *width = 1;
    if (*height < 1)
        *height = 1;
    if (*width > monitor->width)
        *width = monitor->width;
    if (*height > monitor->height)
        *height = monitor->height;
    if (*x < monitor->x)
        *x = monitor->x;
    if (*y < monitor->y)
        *y = monitor->y;
    if (*x > monitor->x + monitor->width - *width)
        *x = monitor->x + monitor->width - *width;
    if (*y > monitor->y + monitor->height - *height)
        *y = monitor->y + monitor->height - *height;
}

static unsigned long allocate_color(WindowManager *wm, const char *name,
                                    unsigned long fallback)
{
    XColor exact;
    XColor color;

    if (XAllocNamedColor(wm->display, DefaultColormap(wm->display, wm->screen),
                         name, &color, &exact))
        return color.pixel;
    return fallback;
}

static void initialize_theme(WindowManager *wm)
{
    size_t index;

    wm->theme.black = BlackPixel(wm->display, wm->screen);
    wm->theme.white = WhitePixel(wm->display, wm->screen);
    wm->theme.silver = allocate_color(wm, "#c0c0c0", wm->theme.white);
    wm->theme.dark_gray = allocate_color(wm, "#606060", wm->theme.black);
    wm->theme.graph_green = allocate_color(wm, "#00ff00", wm->theme.white);
    wm->theme.graph_grid = allocate_color(wm, "#005000", wm->theme.dark_gray);
    for (index = 0U; index < WIN31X_COLOR_SCHEME_COUNT; ++index) {
        const Win31xColorScheme *scheme = win31x_color_scheme(index);

        wm->theme.scheme_desktop[index] = allocate_color(
            wm, scheme != NULL ? scheme->desktop_hex : "#008080",
            wm->theme.black);
        wm->theme.scheme_active_title[index] = allocate_color(
            wm, scheme != NULL ? scheme->active_title_hex : "#000080",
            wm->theme.black);
    }
    if (wm->settings.color_scheme >= WIN31X_COLOR_SCHEME_COUNT)
        wm->settings.color_scheme = 0U;
    wm->theme.desktop = wm->theme.scheme_desktop[wm->settings.color_scheme];
    wm->theme.active_title =
        wm->theme.scheme_active_title[wm->settings.color_scheme];
}

static unsigned long icon_channel_pixel(unsigned char channel,
                                        unsigned long mask)
{
    unsigned int shift = 0;
    unsigned long maximum;
    uint64_t scaled;

    if (mask == 0)
        return 0;
    while (((mask >> shift) & 1u) == 0u)
        ++shift;
    maximum = mask >> shift;
    scaled = ((uint64_t)channel * maximum + 127u) / 255u;
    return ((unsigned long)scaled << shift) & mask;
}

static unsigned long icon_rgb_pixel(WindowManager *wm, unsigned char red,
                                    unsigned char green, unsigned char blue)
{
    Visual *visual = DefaultVisual(wm->display, wm->screen);

    return icon_channel_pixel(red, visual->red_mask) |
           icon_channel_pixel(green, visual->green_mask) |
           icon_channel_pixel(blue, visual->blue_mask);
}

static int create_rendered_icon(WindowManager *wm, const IconImage *source,
                                RenderedIcon *rendered)
{
    Visual *visual = DefaultVisual(wm->display, wm->screen);
    unsigned int depth = (unsigned int)DefaultDepth(wm->display, wm->screen);
    XImage *image = NULL;
    unsigned char *mask_data = NULL;
    size_t mask_stride;
    size_t mask_bytes;
    unsigned int x;
    unsigned int y;

    if (source == NULL || source->rgba == NULL || source->width == 0 ||
        source->height == 0 || rendered == NULL || visual->class != TrueColor ||
        visual->red_mask == 0 || visual->green_mask == 0 ||
        visual->blue_mask == 0) {
        errno = EINVAL;
        return -1;
    }
    rendered->color = XCreatePixmap(wm->display, wm->root, source->width,
                                    source->height, depth);
    if (rendered->color == None)
        return -1;
    image = XCreateImage(wm->display, visual, depth, ZPixmap, 0, NULL,
                         source->width, source->height, BitmapPad(wm->display),
                         0);
    if (image == NULL)
        goto fail;
    image->data = calloc((size_t)image->bytes_per_line, source->height);
    if (image->data == NULL)
        goto fail;
    mask_stride = (source->width + 7u) / 8u;
    if (source->height > SIZE_MAX / mask_stride)
        goto fail;
    mask_bytes = mask_stride * source->height;
    mask_data = calloc(mask_bytes, 1);
    if (mask_data == NULL)
        goto fail;
    for (y = 0; y < source->height; ++y) {
        for (x = 0; x < source->width; ++x) {
            size_t offset = ((size_t)y * source->width + x) * 4u;
            unsigned char alpha = source->rgba[offset + 3];

            XPutPixel(image, (int)x, (int)y,
                      icon_rgb_pixel(wm, source->rgba[offset],
                                     source->rgba[offset + 1],
                                     source->rgba[offset + 2]));
            if (alpha >= 128)
                mask_data[(size_t)y * mask_stride + x / 8u] |=
                    (unsigned char)(1u << (x & 7u));
        }
    }
    XPutImage(wm->display, rendered->color, wm->gc, image, 0, 0, 0, 0,
              source->width, source->height);
    rendered->mask = XCreateBitmapFromData(
        wm->display, wm->root, (const char *)mask_data, source->width,
        source->height);
    if (rendered->mask == None)
        goto fail;
    rendered->width = source->width;
    rendered->height = source->height;
    free(mask_data);
    XDestroyImage(image);
    return 0;

fail:
    free(mask_data);
    if (image != NULL)
        XDestroyImage(image);
    if (rendered->color != None)
        XFreePixmap(wm->display, rendered->color);
    memset(rendered, 0, sizeof(*rendered));
    errno = ENOMEM;
    return -1;
}

static void free_rendered_icon(WindowManager *wm, RenderedIcon *icon)
{
    if (icon == NULL)
        return;
    if (icon->color != None)
        XFreePixmap(wm->display, icon->color);
    if (icon->mask != None)
        XFreePixmap(wm->display, icon->mask);
    memset(icon, 0, sizeof(*icon));
}

static void free_rendered_icons(WindowManager *wm)
{
    int category;
    int size;

    for (category = 0; category < ICON_CATEGORY_COUNT; ++category) {
        for (size = 0; size < ICON_SIZE_COUNT; ++size) {
            RenderedIcon *icon = &wm->rendered_icons[category][size];

            free_rendered_icon(wm, icon);
        }
    }
}

static int initialize_rendered_icons(WindowManager *wm)
{
    Visual *visual = DefaultVisual(wm->display, wm->screen);
    int category;
    int size;

    if (visual->class != TrueColor || visual->red_mask == 0 ||
        visual->green_mask == 0 || visual->blue_mask == 0) {
        fprintf(stderr,
                "win31x: supplied icons require a TrueColor X visual\n");
        errno = ENOTSUP;
        return -1;
    }
    if (icon_assets_load(&wm->icon_assets) < 0)
        return -1;
    for (category = 0; category < ICON_CATEGORY_COUNT; ++category) {
        for (size = 0; size < ICON_SIZE_COUNT; ++size) {
            const IconImage *source = icon_assets_get(
                &wm->icon_assets, (IconCategory)category, (IconSize)size);

            if (create_rendered_icon(wm, source,
                                     &wm->rendered_icons[category][size]) < 0) {
                const IconAssetDescriptor *descriptor = icon_assets_descriptor(
                    (IconCategory)category, (IconSize)size);

                fprintf(stderr, "win31x: could not prepare supplied icon %s\n",
                        descriptor != NULL ? descriptor->filename : "(unknown)");
                free_rendered_icons(wm);
                icon_assets_free(&wm->icon_assets);
                return -1;
            }
        }
    }
    return 0;
}

static void copy_rendered_icon(WindowManager *wm, Drawable drawable,
                               const RenderedIcon *icon, int source_x,
                               int source_y, unsigned int width,
                               unsigned int height, int destination_x,
                               int destination_y)
{
    XSetClipMask(wm->display, wm->gc, icon->mask);
    XSetClipOrigin(wm->display, wm->gc, destination_x - source_x,
                   destination_y - source_y);
    XCopyArea(wm->display, icon->color, drawable, wm->gc, source_x, source_y,
              width, height, destination_x, destination_y);
    XSetClipMask(wm->display, wm->gc, None);
    XSetClipOrigin(wm->display, wm->gc, 0, 0);
}

static void draw_supplied_icon(WindowManager *wm, Drawable drawable,
                               IconCategory category, IconSize size, int x,
                               int y)
{
    const RenderedIcon *icon;

    if (category < 0 || category >= ICON_CATEGORY_COUNT || size < 0 ||
        size >= ICON_SIZE_COUNT)
        return;
    icon = &wm->rendered_icons[category][size];
    if (icon->color == None || icon->mask == None)
        return;
    copy_rendered_icon(wm, drawable, icon, 0, 0, icon->width, icon->height, x,
                       y);
}

static void draw_supplied_icon_centered(WindowManager *wm, Drawable drawable,
                                        IconCategory category, IconSize size,
                                        int box_x, int box_y, int box_width,
                                        int box_height)
{
    const RenderedIcon *icon;
    unsigned int copy_width;
    unsigned int copy_height;
    int source_x;
    int source_y;
    int destination_x;
    int destination_y;

    if (category < 0 || category >= ICON_CATEGORY_COUNT || size < 0 ||
        size >= ICON_SIZE_COUNT)
        return;
    icon = &wm->rendered_icons[category][size];
    if (icon->color == None || icon->mask == None || box_width <= 0 ||
        box_height <= 0)
        return;
    copy_width = icon->width < (unsigned int)box_width
                     ? icon->width
                     : (unsigned int)box_width;
    copy_height = icon->height < (unsigned int)box_height
                      ? icon->height
                      : (unsigned int)box_height;
    source_x = ((int)icon->width - (int)copy_width) / 2;
    source_y = ((int)icon->height - (int)copy_height) / 2;
    destination_x = box_x + (box_width - (int)copy_width) / 2;
    destination_y = box_y + (box_height - (int)copy_height) / 2;
    copy_rendered_icon(wm, drawable, icon, source_x, source_y, copy_width,
                       copy_height, destination_x, destination_y);
}

static void draw_rendered_icon_centered(WindowManager *wm, Drawable drawable,
                                        const RenderedIcon *icon, int box_x,
                                        int box_y, int box_width,
                                        int box_height)
{
    int destination_x;
    int destination_y;

    if (icon == NULL || icon->color == None || icon->mask == None ||
        icon->width == 0U || icon->height == 0U)
        return;
    destination_x = box_x + (box_width - (int)icon->width) / 2;
    destination_y = box_y + (box_height - (int)icon->height) / 2;
    copy_rendered_icon(wm, drawable, icon, 0, 0, icon->width, icon->height,
                       destination_x, destination_y);
}

typedef struct {
    const unsigned long *pixels;
    unsigned int width;
    unsigned int height;
} NetWmIconChoice;

static bool net_wm_icon_is_better(unsigned int candidate_width,
                                  unsigned int candidate_height,
                                  const NetWmIconChoice *current,
                                  unsigned int target)
{
    unsigned int candidate_size = candidate_width > candidate_height
                                      ? candidate_width
                                      : candidate_height;
    unsigned int current_size = current->width > current->height
                                    ? current->width
                                    : current->height;
    bool candidate_large = candidate_size >= target;
    bool current_large = current_size >= target;

    if (current->pixels == NULL)
        return true;
    if (candidate_large != current_large)
        return candidate_large;
    if (candidate_large)
        return candidate_size < current_size;
    return candidate_size > current_size;
}

static int rendered_net_wm_icon(WindowManager *wm, const unsigned long *data,
                                unsigned long count, unsigned int target,
                                RenderedIcon *rendered)
{
    NetWmIconChoice choice = {0};
    unsigned long offset = 0UL;
    IconImage source = {0};
    IconImage scaled = {0};
    size_t pixel_count;
    size_t index;
    bool any_alpha = false;
    bool any_color = false;
    bool any_visible = false;
    int result = -1;

    while (count - offset >= 2UL) {
        unsigned long raw_width = data[offset++];
        unsigned long raw_height = data[offset++];
        uint64_t raw_pixels;

        if (raw_width == 0UL || raw_height == 0UL || raw_width > 1024UL ||
            raw_height > 1024UL)
            break;
        raw_pixels = (uint64_t)raw_width * raw_height;
        if (raw_pixels > count - offset)
            break;
        if (net_wm_icon_is_better((unsigned int)raw_width,
                                  (unsigned int)raw_height, &choice, target)) {
            choice.pixels = data + offset;
            choice.width = (unsigned int)raw_width;
            choice.height = (unsigned int)raw_height;
        }
        offset += (unsigned long)raw_pixels;
    }
    if (choice.pixels == NULL) {
        errno = ENOENT;
        return -1;
    }
    pixel_count = (size_t)choice.width * choice.height;
    source.rgba = malloc(pixel_count * 4U);
    if (source.rgba == NULL)
        return -1;
    source.width = choice.width;
    source.height = choice.height;
    for (index = 0U; index < pixel_count; ++index) {
        unsigned long argb = choice.pixels[index];
        unsigned char alpha = (unsigned char)((argb >> 24) & 0xffUL);
        unsigned char red = (unsigned char)((argb >> 16) & 0xffUL);
        unsigned char green = (unsigned char)((argb >> 8) & 0xffUL);
        unsigned char blue = (unsigned char)(argb & 0xffUL);

        source.rgba[index * 4U] = red;
        source.rgba[index * 4U + 1U] = green;
        source.rgba[index * 4U + 2U] = blue;
        source.rgba[index * 4U + 3U] = alpha;
        any_alpha = any_alpha || alpha != 0U;
        any_visible = any_visible || alpha >= 128U;
        any_color = any_color || red != 0U || green != 0U || blue != 0U;
    }
    /* A few older toolkits accidentally publish RGB with a zero alpha byte. */
    if (!any_alpha && any_color) {
        for (index = 0U; index < pixel_count; ++index)
            source.rgba[index * 4U + 3U] = 255U;
        any_visible = true;
    }
    if (!any_visible) {
        errno = EINVAL;
        goto done;
    }
    if (icon_image_scale_fit(&source, target, target, &scaled) < 0)
        goto done;
    if (create_rendered_icon(wm, &scaled, rendered) < 0)
        goto done;
    result = 0;

done:
    icon_image_free(&scaled);
    icon_image_free(&source);
    return result;
}

static void refresh_client_icon(WindowManager *wm, Client *client)
{
    Atom actual_type;
    int actual_format;
    unsigned long count;
    unsigned long bytes_after;
    unsigned char *value = NULL;
    RenderedIcon small = {0};
    RenderedIcon large = {0};

    if (XGetWindowProperty(wm->display, client->window, wm->atoms.net_wm_icon,
                           0, CLIENT_ICON_PROPERTY_LIMIT, False, XA_CARDINAL,
                           &actual_type, &actual_format, &count, &bytes_after,
                           &value) == Success &&
        actual_type == XA_CARDINAL && actual_format == 32 && value != NULL &&
        bytes_after == 0UL) {
        const unsigned long *items = (const unsigned long *)value;

        (void)rendered_net_wm_icon(wm, items, count, 16U, &small);
        (void)rendered_net_wm_icon(wm, items, count, 48U, &large);
    }
    if (value != NULL)
        XFree(value);
    free_rendered_icon(wm, &client->actual_icon_small);
    free_rendered_icon(wm, &client->actual_icon_large);
    client->actual_icon_small = small;
    client->actual_icon_large = large;
    draw_frame(wm, client);
    if (client->icon != NULL)
        draw_desktop_icon(wm, client->icon);
}

static Atom intern(Display *display, const char *name)
{
    return XInternAtom(display, name, False);
}

static void initialize_atoms(WindowManager *wm)
{
    Atoms *a = &wm->atoms;
    Display *d = wm->display;

    a->wm_protocols = intern(d, "WM_PROTOCOLS");
    a->wm_delete_window = intern(d, "WM_DELETE_WINDOW");
    a->wm_take_focus = intern(d, "WM_TAKE_FOCUS");
    a->wm_change_state = intern(d, "WM_CHANGE_STATE");
    a->wm_state = intern(d, "WM_STATE");
    a->utf8_string = intern(d, "UTF8_STRING");
    a->net_supported = intern(d, "_NET_SUPPORTED");
    a->net_supporting_wm_check = intern(d, "_NET_SUPPORTING_WM_CHECK");
    a->net_wm_name = intern(d, "_NET_WM_NAME");
    a->net_wm_icon_name = intern(d, "_NET_WM_ICON_NAME");
    a->net_wm_icon = intern(d, "_NET_WM_ICON");
    a->net_client_list = intern(d, "_NET_CLIENT_LIST");
    a->net_active_window = intern(d, "_NET_ACTIVE_WINDOW");
    a->net_close_window = intern(d, "_NET_CLOSE_WINDOW");
    a->net_frame_extents = intern(d, "_NET_FRAME_EXTENTS");
    a->net_number_of_desktops = intern(d, "_NET_NUMBER_OF_DESKTOPS");
    a->net_current_desktop = intern(d, "_NET_CURRENT_DESKTOP");
    a->net_workarea = intern(d, "_NET_WORKAREA");
    a->net_wm_state = intern(d, "_NET_WM_STATE");
    a->net_wm_state_hidden = intern(d, "_NET_WM_STATE_HIDDEN");
    a->net_wm_state_maximized_horz =
        intern(d, "_NET_WM_STATE_MAXIMIZED_HORZ");
    a->net_wm_state_maximized_vert =
        intern(d, "_NET_WM_STATE_MAXIMIZED_VERT");
    a->net_wm_desktop_file = intern(d, "_NET_WM_DESKTOP_FILE");
    a->gtk_application_id = intern(d, "_GTK_APPLICATION_ID");
    a->wm_window_role = intern(d, "WM_WINDOW_ROLE");
    a->win31x_role = intern(d, "_WIN31X_ROLE");
    a->win31x_client = intern(d, "_WIN31X_CLIENT");
    a->win31x_task_manager_tab = intern(d, "_WIN31X_TASK_MANAGER_TAB");
    a->win31x_task_manager_sample_serial =
        intern(d, "_WIN31X_TASK_MANAGER_SAMPLE_SERIAL");
    a->win31x_task_manager_cpu_tenths =
        intern(d, "_WIN31X_TASK_MANAGER_CPU_TENTHS");
    a->win31x_task_manager_memory_tenths =
        intern(d, "_WIN31X_TASK_MANAGER_MEMORY_TENTHS");
    a->win31x_task_manager_process_count =
        intern(d, "_WIN31X_TASK_MANAGER_PROCESS_COUNT");
    a->win31x_task_manager_selected_pid =
        intern(d, "_WIN31X_TASK_MANAGER_SELECTED_PID");
}

static void set_utf8_property(WindowManager *wm, Window window, Atom property,
                              const char *value)
{
    XChangeProperty(wm->display, window, property, wm->atoms.utf8_string, 8,
                    PropModeReplace, (const unsigned char *)value,
                    (int)strlen(value));
}

static void set_internal_role(WindowManager *wm, Window window, const char *role)
{
    set_utf8_property(wm, window, wm->atoms.win31x_role, role);
}

static void initialize_drag_outline(WindowManager *wm)
{
    XSetWindowAttributes attributes;
    size_t index;

    attributes.override_redirect = True;
    attributes.save_under = True;
    attributes.background_pixel = wm->theme.black;
    attributes.event_mask = NoEventMask;
    for (index = 0U; index < DRAG_OUTLINE_WINDOW_COUNT; ++index) {
        wm->drag_outline[index] = XCreateWindow(
            wm->display, wm->root, 0, 0, 1, 1, 0, CopyFromParent,
            InputOutput, CopyFromParent,
            CWOverrideRedirect | CWSaveUnder | CWBackPixel | CWEventMask,
            &attributes);
        set_internal_role(wm, wm->drag_outline[index], "drag-outline");
    }
}

static void raise_drag_outline(WindowManager *wm)
{
    size_t index;

    if (!wm->drag.outline_visible)
        return;
    for (index = 0U; index < DRAG_OUTLINE_WINDOW_COUNT; ++index)
        XRaiseWindow(wm->display, wm->drag_outline[index]);
}

static void show_drag_outline(WindowManager *wm, int x, int y, int width,
                              int height)
{
    int horizontal_thickness = DRAG_OUTLINE_THICKNESS;
    int vertical_thickness = DRAG_OUTLINE_THICKNESS;
    int middle_height;
    int side_y;
    size_t index;

    if (width < 1 || height < 1)
        return;
    if (horizontal_thickness > height)
        horizontal_thickness = height;
    if (vertical_thickness > width)
        vertical_thickness = width;
    middle_height = height - horizontal_thickness * 2;
    if (middle_height < 1)
        middle_height = 1;
    side_y = y + (height - middle_height) / 2;

    XMoveResizeWindow(wm->display, wm->drag_outline[0], x, y,
                      (unsigned)width, (unsigned)horizontal_thickness);
    XMoveResizeWindow(wm->display, wm->drag_outline[1], x,
                      y + height - horizontal_thickness, (unsigned)width,
                      (unsigned)horizontal_thickness);
    XMoveResizeWindow(wm->display, wm->drag_outline[2], x, side_y,
                      (unsigned)vertical_thickness, (unsigned)middle_height);
    XMoveResizeWindow(wm->display, wm->drag_outline[3],
                      x + width - vertical_thickness, side_y,
                      (unsigned)vertical_thickness, (unsigned)middle_height);
    if (!wm->drag.outline_visible) {
        for (index = 0U; index < DRAG_OUTLINE_WINDOW_COUNT; ++index)
            XMapWindow(wm->display, wm->drag_outline[index]);
        wm->drag.outline_visible = true;
    }
    raise_drag_outline(wm);
}

static void hide_drag_outline(WindowManager *wm)
{
    size_t index;

    if (!wm->drag.outline_visible)
        return;
    wm->drag.outline_visible = false;
    for (index = 0U; index < DRAG_OUTLINE_WINDOW_COUNT; ++index)
        XUnmapWindow(wm->display, wm->drag_outline[index]);
}

static void cancel_drag(WindowManager *wm, Time time)
{
    bool was_dragging = wm->drag.kind != DRAG_NONE;
    bool keyboard_grabbed = wm->drag.keyboard_grabbed;

    hide_drag_outline(wm);
    memset(&wm->drag, 0, sizeof(wm->drag));
    if (was_dragging)
        XUngrabPointer(wm->display, time);
    if (keyboard_grabbed)
        XUngrabKeyboard(wm->display, time);
}

static void cancel_drag_and_restore(WindowManager *wm, Time time)
{
    DragState drag = wm->drag;

    cancel_drag(wm, time);
    if (drag.kind == DRAG_RESIZE_CLIENT && drag.client != NULL) {
        drag.client->x = drag.start_x;
        drag.client->y = drag.start_y;
        drag.client->width = drag.start_width;
        drag.client->height = drag.start_height;
        apply_client_geometry(wm, drag.client);
        send_configure_notify(wm, drag.client);
    }
}

static char *copy_bytes(const unsigned char *data, unsigned long length)
{
    char *copy = malloc(length + 1);

    if (copy == NULL)
        return NULL;
    memcpy(copy, data, length);
    copy[length] = '\0';
    return copy;
}

static char *window_title(WindowManager *wm, Window window)
{
    Atom actual_type;
    int actual_format;
    unsigned long item_count;
    unsigned long bytes_after;
    unsigned char *value = NULL;
    char *title = NULL;
    char *legacy = NULL;

    if (XGetWindowProperty(wm->display, window, wm->atoms.net_wm_name, 0, 1024,
                           False, wm->atoms.utf8_string, &actual_type,
                           &actual_format, &item_count, &bytes_after, &value) ==
            Success &&
        actual_type == wm->atoms.utf8_string && actual_format == 8 && value != NULL)
        title = copy_bytes(value, item_count);
    if (value != NULL)
        XFree(value);
    if (title == NULL && XFetchName(wm->display, window, &legacy) && legacy != NULL) {
        title = strdup(legacy);
        XFree(legacy);
    }
    if (title == NULL || title[0] == '\0') {
        free(title);
        title = strdup("Application");
    }
    return title;
}

static char *window_class(WindowManager *wm, Window window)
{
    XClassHint hint = {0};
    const char *name;
    const char *class_name;
    char *combined;
    size_t size;

    if (!XGetClassHint(wm->display, window, &hint))
        return strdup("");
    name = hint.res_name != NULL ? hint.res_name : "";
    class_name = hint.res_class != NULL ? hint.res_class : "";
    size = strlen(name) + strlen(class_name) + 2;
    combined = malloc(size);
    if (combined != NULL)
        snprintf(combined, size, "%s %s", name, class_name);
    if (hint.res_name != NULL)
        XFree(hint.res_name);
    if (hint.res_class != NULL)
        XFree(hint.res_class);
    return combined;
}

static char *window_identity_property(WindowManager *wm, Window window,
                                      Atom property)
{
    Atom actual_type;
    int actual_format;
    unsigned long item_count;
    unsigned long bytes_after;
    unsigned char *value = NULL;
    char *copy = NULL;

    if (XGetWindowProperty(wm->display, window, property, 0, 256, False,
                           AnyPropertyType, &actual_type, &actual_format,
                           &item_count, &bytes_after, &value) == Success &&
        actual_type != None && actual_format == 8 && value != NULL &&
        bytes_after == 0UL && item_count > 0UL && item_count <= 192UL &&
        memchr(value, '\0', item_count) == NULL)
        copy = copy_bytes(value, item_count);
    if (value != NULL)
        XFree(value);
    return copy;
}

static bool append_identity_component(char *identity, size_t capacity,
                                      size_t *used, const char *label,
                                      const char *value)
{
    size_t value_length = value != NULL ? strlen(value) : 0U;
    int prefix_length;

    prefix_length = snprintf(identity + *used, capacity - *used, "%s%zu:",
                             label, value_length);
    if (prefix_length < 0 || (size_t)prefix_length >= capacity - *used ||
        value_length >= capacity - *used - (size_t)prefix_length)
        return false;
    *used += (size_t)prefix_length;
    memcpy(identity + *used, value, value_length);
    *used += value_length;
    identity[*used] = '\0';
    return true;
}

static char *window_state_identity(WindowManager *wm, Window window)
{
    XClassHint hint = {0};
    char *application_id = window_identity_property(
        wm, window, wm->atoms.gtk_application_id);
    char *desktop_file = NULL;
    char *role = window_identity_property(wm, window, wm->atoms.wm_window_role);
    const char *name = "";
    const char *class_name = "";
    char identity[WIN31X_DESKTOP_IDENTITY_MAX + 1U -
                  STATE_IDENTITY_INSTANCE_RESERVE];
    size_t used = 0U;
    bool valid = true;

    if (application_id == NULL)
        desktop_file = window_identity_property(
            wm, window, wm->atoms.net_wm_desktop_file);
    (void)XGetClassHint(wm->display, window, &hint);
    if (hint.res_name != NULL)
        name = hint.res_name;
    if (hint.res_class != NULL)
        class_name = hint.res_class;

    identity[0] = '\0';
    if (application_id != NULL) {
        valid = append_identity_component(identity, sizeof(identity), &used,
                                          "app", application_id);
    } else if (desktop_file != NULL) {
        valid = append_identity_component(identity, sizeof(identity), &used,
                                          "desktop", desktop_file);
    } else if (name[0] != '\0' || class_name[0] != '\0') {
        valid = append_identity_component(identity, sizeof(identity), &used,
                                          "class", class_name) &&
                append_identity_component(identity, sizeof(identity), &used,
                                          "name", name);
    } else {
        valid = false;
    }
    if (valid && role != NULL && role[0] != '\0')
        valid = append_identity_component(identity, sizeof(identity), &used,
                                          "role", role);

    if (hint.res_name != NULL)
        XFree(hint.res_name);
    if (hint.res_class != NULL)
        XFree(hint.res_class);
    free(application_id);
    free(desktop_file);
    free(role);
    return strdup(valid ? identity : "");
}

static bool state_instance_in_use(const WindowManager *wm,
                                  const Client *ignored,
                                  const char *base_identity,
                                  unsigned int instance)
{
    const Client *client;

    for (client = wm->clients; client != NULL; client = client->next) {
        if (client != ignored && client->state_base_identity != NULL &&
            strcmp(client->state_base_identity, base_identity) == 0 &&
            client->state_instance == instance)
            return true;
    }
    return false;
}

static char *client_state_identity(WindowManager *wm, Client *client,
                                   const char *base_identity,
                                   unsigned int *instance_out)
{
    char identity[WIN31X_DESKTOP_IDENTITY_MAX + 1U];
    unsigned int instance = 1U;
    int written;

    if (base_identity == NULL || base_identity[0] == '\0') {
        *instance_out = 0U;
        return strdup("");
    }
    while (state_instance_in_use(wm, client, base_identity, instance)) {
        if (instance == UINT_MAX) {
            errno = EOVERFLOW;
            return NULL;
        }
        ++instance;
    }
    *instance_out = instance;
    if (instance == 1U)
        return strdup(base_identity);
    written = snprintf(identity, sizeof(identity), "instance=%u;%s",
                       instance, base_identity);
    if (written < 0 || (size_t)written >= sizeof(identity)) {
        errno = ENAMETOOLONG;
        return NULL;
    }
    return strdup(identity);
}

static Client *client_for_window(WindowManager *wm, Window window)
{
    Client *client;

    for (client = wm->clients; client != NULL; client = client->next) {
        if (client->window == window || client->frame == window ||
            client->focus_overlay == window)
            return client;
    }
    return NULL;
}

static Client *client_for_client_window(WindowManager *wm, Window window)
{
    Client *client;

    for (client = wm->clients; client != NULL; client = client->next) {
        if (client->window == window)
            return client;
    }
    return NULL;
}

static DesktopIcon *icon_for_window(WindowManager *wm, Window window)
{
    DesktopIcon *icon;

    for (icon = wm->icons; icon != NULL; icon = icon->next) {
        if (icon->window == window)
            return icon;
    }
    return NULL;
}

static void draw_bevel(WindowManager *wm, Window window, int x, int y,
                       int width, int height, bool sunken)
{
    unsigned long top_left = sunken ? wm->theme.dark_gray : wm->theme.white;
    unsigned long bottom_right = sunken ? wm->theme.white : wm->theme.dark_gray;

    XSetForeground(wm->display, wm->gc, top_left);
    XDrawLine(wm->display, window, wm->gc, x, y, x + width - 1, y);
    XDrawLine(wm->display, window, wm->gc, x, y, x, y + height - 1);
    XSetForeground(wm->display, wm->gc, bottom_right);
    XDrawLine(wm->display, window, wm->gc, x, y + height - 1,
              x + width - 1, y + height - 1);
    XDrawLine(wm->display, window, wm->gc, x + width - 1, y,
              x + width - 1, y + height - 1);
}

static void fitted_text(WindowManager *wm, const char *source, int max_width,
                        char *destination, size_t destination_size)
{
    size_t length;

    if (destination_size == 0)
        return;
    snprintf(destination, destination_size, "%s", source == NULL ? "" : source);
    length = strlen(destination);
    while (length > 0 && XTextWidth(wm->font, destination, (int)length) > max_width) {
        --length;
        destination[length] = '\0';
    }
    if (length >= 4 && source != NULL && strlen(source) > length) {
        destination[length - 3] = '.';
        destination[length - 2] = '.';
        destination[length - 1] = '.';
    }
}

static void draw_centered_text(WindowManager *wm, Window window, const char *text,
                               int center_x, int baseline, unsigned long color,
                               int max_width)
{
    char fitted[256];
    int width;

    fitted_text(wm, text, max_width, fitted, sizeof(fitted));
    width = XTextWidth(wm->font, fitted, (int)strlen(fitted));
    XSetForeground(wm->display, wm->gc, color);
    XDrawString(wm->display, window, wm->gc, center_x - width / 2, baseline,
                fitted, (int)strlen(fitted));
}

static int frame_width(const Client *client)
{
    return client->width + FRAME_LEFT + FRAME_RIGHT;
}

static int frame_height(const Client *client)
{
    return client->height + FRAME_TOP + FRAME_BOTTOM;
}

static size_t monitor_index_for_client(const WindowManager *wm,
                                       const Client *client)
{
    return monitor_index_for_rectangle(
        wm, client->x - FRAME_LEFT, client->y - FRAME_TOP,
        frame_width(client), frame_height(client));
}

static size_t pointer_monitor_index(WindowManager *wm)
{
    Window root_return;
    Window child_return;
    int root_x;
    int root_y;
    int window_x;
    int window_y;
    unsigned int mask;

    if (XQueryPointer(wm->display, wm->root, &root_return, &child_return,
                      &root_x, &root_y, &window_x, &window_y, &mask))
        return monitor_index_for_point(wm, root_x, root_y);
    return monitor_index_for_anchor(wm, &wm->active_monitor);
}

static size_t active_monitor_index(WindowManager *wm)
{
    if (wm->internal_focus == INTERNAL_FOCUS_APPLICATIONS &&
        wm->launcher_visible) {
        return monitor_index_for_rectangle(
            wm, wm->launcher_x, wm->launcher_y,
            wm->launcher_width, wm->launcher_height);
    }
    if (wm->internal_focus == INTERNAL_FOCUS_CONTROL_PANEL &&
        wm->control_panel.visible) {
        return monitor_index_for_rectangle(
            wm, wm->control_panel.x, wm->control_panel.y,
            wm->control_panel.width, wm->control_panel.height);
    }
    if (wm->internal_focus == INTERNAL_FOCUS_RUN && wm->run_dialog.visible) {
        return monitor_index_for_rectangle(
            wm, wm->run_dialog.x, wm->run_dialog.y,
            wm->run_dialog.width, wm->run_dialog.height);
    }
    if (wm->internal_focus == INTERNAL_FOCUS_TASK_MANAGER &&
        wm->task_manager.visible) {
        return monitor_index_for_rectangle(
            wm, wm->task_manager.x, wm->task_manager.y,
            wm->task_manager.width, wm->task_manager.height);
    }
    if (wm->active != NULL && !wm->active->minimized)
        return monitor_index_for_client(wm, wm->active);
    if (wm->active_monitor.valid)
        return monitor_index_for_anchor(wm, &wm->active_monitor);
    return pointer_monitor_index(wm);
}

static const MonitorGeometry *monitor_at(const WindowManager *wm,
                                         size_t index)
{
    if (wm->monitor_count == 0U)
        return NULL;
    if (index >= wm->monitor_count)
        index = 0U;
    return &wm->monitors[index];
}

static void set_active_monitor(WindowManager *wm, size_t index)
{
    const MonitorGeometry *monitor = monitor_at(wm, index);

    if (monitor != NULL)
        set_monitor_anchor(&wm->active_monitor, monitor);
}

static void set_active_monitor_from_point(WindowManager *wm, int x, int y)
{
    set_active_monitor(wm, monitor_index_for_point(wm, x, y));
}

static void set_active_monitor_from_client(WindowManager *wm,
                                           const Client *client)
{
    if (client != NULL)
        set_active_monitor(wm, monitor_index_for_client(wm, client));
}

static int close_button_x(const Client *client)
{
    return frame_width(client) - FRAME_RIGHT - TITLE_BUTTON - 2;
}

static int maximize_button_x(const Client *client)
{
    return close_button_x(client) - TITLE_BUTTON - 3;
}

static int minimize_button_x(const Client *client)
{
    return maximize_button_x(client) - TITLE_BUTTON - 3;
}

static int internal_close_button_x(int width)
{
    return width - TITLE_BUTTON - 6;
}

static int internal_maximize_button_x(int width)
{
    return internal_close_button_x(width) - TITLE_BUTTON - 3;
}

static IconCategory client_icon_category(const Client *client)
{
    if (client == NULL)
        return ICON_CATEGORY_EXECUTABLE;
    return icon_assets_classify(client->title, client->class_name, NULL);
}

static void draw_title_button(WindowManager *wm, Window window, int x, int y,
                              TitleGlyph glyph)
{
    XSetForeground(wm->display, wm->gc, wm->theme.silver);
    XFillRectangle(wm->display, window, wm->gc, x, y, TITLE_BUTTON, TITLE_BUTTON);
    draw_bevel(wm, window, x, y, TITLE_BUTTON, TITLE_BUTTON, false);
    XSetForeground(wm->display, wm->gc, wm->theme.black);
    if (glyph == TITLE_GLYPH_CLOSE) {
        XDrawLine(wm->display, window, wm->gc, x + 5, y + 5, x + 11, y + 11);
        XDrawLine(wm->display, window, wm->gc, x + 11, y + 5, x + 5, y + 11);
        XDrawLine(wm->display, window, wm->gc, x + 6, y + 5, x + 12, y + 11);
        XDrawLine(wm->display, window, wm->gc, x + 12, y + 5, x + 6, y + 11);
    } else if (glyph == TITLE_GLYPH_MINIMIZE) {
        XFillRectangle(wm->display, window, wm->gc, x + 4, y + 11, 9, 2);
    } else if (glyph == TITLE_GLYPH_MAXIMIZE) {
        XDrawRectangle(wm->display, window, wm->gc, x + 4, y + 4, 8, 8);
        XDrawLine(wm->display, window, wm->gc, x + 5, y + 5, x + 11, y + 5);
    } else {
        XDrawRectangle(wm->display, window, wm->gc, x + 3, y + 6, 7, 7);
        XDrawRectangle(wm->display, window, wm->gc, x + 6, y + 3, 7, 7);
        XDrawLine(wm->display, window, wm->gc, x + 7, y + 4, x + 12, y + 4);
    }
}

static void draw_text(WindowManager *wm, Window window, int x, int baseline,
                      unsigned long color, const char *text)
{
    if (text == NULL)
        return;
    XSetForeground(wm->display, wm->gc, color);
    XDrawString(wm->display, window, wm->gc, x, baseline, text,
                (int)strlen(text));
}

static void draw_button(WindowManager *wm, Window window, int x, int y,
                        int width, int height, const char *label, bool pressed,
                        bool enabled)
{
    unsigned long text_color = enabled ? wm->theme.black : wm->theme.dark_gray;

    if (width < 2 || height < 2)
        return;
    XSetForeground(wm->display, wm->gc, wm->theme.silver);
    XFillRectangle(wm->display, window, wm->gc, x, y, (unsigned)width,
                   (unsigned)height);
    draw_bevel(wm, window, x, y, width, height, pressed);
    draw_centered_text(wm, window, label, x + width / 2,
                       y + height / 2 + 5, text_color, width - 8);
}

static bool point_in_rectangle(int point_x, int point_y, int x, int y,
                               int width, int height)
{
    return point_x >= x && point_y >= y && point_x < x + width &&
           point_y < y + height;
}

static void clear_sensitive_bytes(void *memory, size_t length)
{
    volatile unsigned char *cursor = memory;
    size_t index;

    for (index = 0U; index < length; ++index)
        cursor[index] = 0U;
}

static void clear_control_password(ControlPanel *panel)
{
    clear_sensitive_bytes(panel->password, sizeof(panel->password));
    panel->password_length = 0U;
    panel->password_active = false;
}

static void draw_frame(WindowManager *wm, Client *client)
{
    int width = frame_width(client);
    int title_end = minimize_button_x(client) - 3;
    char title[512];

    XSetForeground(wm->display, wm->gc, wm->theme.silver);
    XFillRectangle(wm->display, client->frame, wm->gc, 0, 0,
                   (unsigned)width, (unsigned)frame_height(client));
    draw_bevel(wm, client->frame, 0, 0, width, frame_height(client), false);
    XSetForeground(
        wm->display, wm->gc,
        client == wm->active && wm->internal_focus == INTERNAL_FOCUS_NONE
            ? wm->theme.active_title
            : wm->theme.dark_gray);
    XFillRectangle(wm->display, client->frame, wm->gc, FRAME_LEFT, TITLE_Y,
                   (unsigned)(width - FRAME_LEFT - FRAME_RIGHT), TITLE_HEIGHT);

    if (client->actual_icon_small.color != None)
        draw_rendered_icon_centered(wm, client->frame,
                                    &client->actual_icon_small,
                                    FRAME_LEFT + 2, TITLE_Y + 2, 16, 16);
    else
        draw_supplied_icon(wm, client->frame, client_icon_category(client),
                           ICON_SIZE_SMALL, FRAME_LEFT + 2, TITLE_Y + 2);

    fitted_text(wm, client->title, title_end - 25, title, sizeof(title));
    XSetForeground(wm->display, wm->gc, wm->theme.white);
    XDrawString(wm->display, client->frame, wm->gc, FRAME_LEFT + 22,
                TITLE_Y + 14, title, (int)strlen(title));
    draw_title_button(wm, client->frame, minimize_button_x(client), TITLE_Y + 1,
                      TITLE_GLYPH_MINIMIZE);
    draw_title_button(
        wm, client->frame, maximize_button_x(client), TITLE_Y + 1,
        client->layout == CLIENT_LAYOUT_MAXIMIZED ? TITLE_GLYPH_RESTORE
                                                  : TITLE_GLYPH_MAXIMIZE);
    draw_title_button(wm, client->frame, close_button_x(client), TITLE_Y + 1,
                      TITLE_GLYPH_CLOSE);
}

static void draw_desktop_icon(WindowManager *wm, DesktopIcon *icon)
{
    const char *label;
    IconCategory category;

    if (icon->kind == ICON_APPLICATIONS) {
        label = "Applications";
        category = ICON_CATEGORY_APPLICATIONS;
    } else if (icon->kind == ICON_CONTROL_PANEL) {
        label = "Control Panel";
        category = ICON_CATEGORY_SETTINGS;
    } else {
        label = icon->client != NULL ? icon->client->title : "Application";
        category = client_icon_category(icon->client);
    }

    XSetForeground(wm->display, wm->gc, wm->theme.desktop);
    XFillRectangle(wm->display, icon->window, wm->gc, 0, 0, ICON_WIDTH, ICON_HEIGHT);
    if (icon->kind == ICON_MINIMIZED && icon->client != NULL &&
        icon->client->actual_icon_large.color != None)
        draw_rendered_icon_centered(wm, icon->window,
                                    &icon->client->actual_icon_large,
                                    0, 5, ICON_WIDTH, 48);
    else
        draw_supplied_icon_centered(wm, icon->window, category,
                                    ICON_SIZE_LARGE, 0, 5, ICON_WIDTH, 48);
    draw_centered_text(wm, icon->window, label, ICON_WIDTH / 2 + 1, 70,
                       wm->theme.black, ICON_WIDTH - 4);
    draw_centered_text(wm, icon->window, label, ICON_WIDTH / 2, 69,
                       wm->theme.white, ICON_WIDTH - 4);
}

static void save_settings(WindowManager *wm)
{
    if (win31x_settings_save(&wm->settings) < 0) {
        snprintf(wm->control_panel.settings_status,
                 sizeof(wm->control_panel.settings_status),
                 "Could not save settings: %s", strerror(errno));
    } else {
        snprintf(wm->control_panel.settings_status,
                 sizeof(wm->control_panel.settings_status),
                 "Settings saved");
    }
}

static void apply_color_scheme(WindowManager *wm, size_t scheme_index,
                               bool persist)
{
    Client *client;
    DesktopIcon *icon;

    if (scheme_index >= WIN31X_COLOR_SCHEME_COUNT)
        return;
    wm->settings.color_scheme = scheme_index;
    wm->theme.desktop = wm->theme.scheme_desktop[scheme_index];
    wm->theme.active_title = wm->theme.scheme_active_title[scheme_index];
    XSetWindowBackground(wm->display, wm->root, wm->theme.desktop);
    XClearWindow(wm->display, wm->root);
    for (icon = wm->icons; icon != NULL; icon = icon->next) {
        XSetWindowBackground(wm->display, icon->window, wm->theme.desktop);
        draw_desktop_icon(wm, icon);
    }
    for (client = wm->clients; client != NULL; client = client->next)
        draw_frame(wm, client);
    if (persist)
        save_settings(wm);
    if (wm->launcher_visible)
        draw_launcher(wm);
    if (wm->control_panel.visible)
        draw_control_panel(wm);
    if (wm->run_dialog.visible)
        draw_run_dialog(wm);
    if (wm->task_manager.visible)
        draw_task_manager(wm);
    if (wm->session_confirmation.visible)
        draw_session_confirmation(wm);
}

static void set_wm_state(WindowManager *wm, Client *client, long state)
{
    long values[2];

    values[0] = state;
    values[1] = client->icon != NULL && client->minimized_with_owner == None
                    ? (long)client->icon->window
                    : None;
    XChangeProperty(wm->display, client->window, wm->atoms.wm_state,
                    wm->atoms.wm_state, 32, PropModeReplace,
                    (unsigned char *)values, 2);
}

static long read_wm_state(WindowManager *wm, Window window)
{
    Atom actual_type;
    int actual_format;
    unsigned long count;
    unsigned long after;
    unsigned char *data = NULL;
    long state = WithdrawnState;

    if (XGetWindowProperty(wm->display, window, wm->atoms.wm_state, 0, 2, False,
                           wm->atoms.wm_state, &actual_type, &actual_format,
                           &count, &after, &data) == Success &&
        actual_type == wm->atoms.wm_state && actual_format == 32 && count >= 1 &&
        data != NULL)
        state = ((long *)data)[0];
    if (data != NULL)
        XFree(data);
    return state;
}

static bool net_wm_state_contains(WindowManager *wm, Window window, Atom state)
{
    Atom actual_type;
    int actual_format;
    unsigned long count;
    unsigned long after;
    unsigned char *data = NULL;
    Atom *states;
    unsigned long index;
    bool found = false;

    if (XGetWindowProperty(wm->display, window, wm->atoms.net_wm_state, 0, 128,
                           False, XA_ATOM, &actual_type, &actual_format, &count,
                           &after, &data) != Success ||
        actual_type != XA_ATOM || actual_format != 32 || data == NULL)
        count = 0;
    states = (Atom *)data;
    for (index = 0; index < count; ++index) {
        if (states[index] == state) {
            found = true;
            break;
        }
    }
    if (data != NULL)
        XFree(data);
    return found;
}

static void set_net_wm_state_atom(WindowManager *wm, Client *client, Atom state,
                                  bool enabled)
{
    Atom actual_type;
    int actual_format;
    unsigned long count;
    unsigned long after;
    unsigned char *data = NULL;
    Atom *states = NULL;
    Atom *replacement;
    unsigned long index;
    unsigned long new_count = 0;
    bool already_present = false;

    if (XGetWindowProperty(wm->display, client->window, wm->atoms.net_wm_state,
                           0, 128, False, XA_ATOM, &actual_type, &actual_format,
                           &count, &after, &data) == Success &&
        actual_type == XA_ATOM && actual_format == 32)
        states = (Atom *)data;
    else
        count = 0;
    replacement = calloc(count + 1, sizeof(*replacement));
    if (replacement == NULL) {
        if (data != NULL)
            XFree(data);
        return;
    }
    for (index = 0; index < count; ++index) {
        if (states[index] == state) {
            already_present = true;
            if (!enabled)
                continue;
        }
        replacement[new_count++] = states[index];
    }
    if (enabled && !already_present)
        replacement[new_count++] = state;
    if (new_count == 0)
        XDeleteProperty(wm->display, client->window, wm->atoms.net_wm_state);
    else
        XChangeProperty(wm->display, client->window, wm->atoms.net_wm_state,
                        XA_ATOM, 32, PropModeReplace,
                        (unsigned char *)replacement, (int)new_count);
    free(replacement);
    if (data != NULL)
        XFree(data);
}

static void set_hidden_state(WindowManager *wm, Client *client, bool hidden)
{
    set_net_wm_state_atom(wm, client, wm->atoms.net_wm_state_hidden, hidden);
}

static void set_maximized_state(WindowManager *wm, Client *client,
                                bool maximized)
{
    set_net_wm_state_atom(wm, client, wm->atoms.net_wm_state_maximized_horz,
                          maximized);
    set_net_wm_state_atom(wm, client, wm->atoms.net_wm_state_maximized_vert,
                          maximized);
}

static void update_client_list(WindowManager *wm)
{
    Client *client;
    Window *windows;
    size_t count = 0;
    size_t index = 0;

    for (client = wm->clients; client != NULL; client = client->next)
        ++count;
    windows = count == 0 ? NULL : calloc(count, sizeof(*windows));
    if (count != 0 && windows == NULL)
        return;
    index = count;
    for (client = wm->clients; client != NULL; client = client->next)
        windows[--index] = client->window;
    XChangeProperty(wm->display, wm->root, wm->atoms.net_client_list, XA_WINDOW,
                    32, PropModeReplace, (unsigned char *)windows, (int)count);
    free(windows);
}

static void publish_active_client(WindowManager *wm, Client *client)
{
    if (client == NULL) {
        XDeleteProperty(wm->display, wm->root, wm->atoms.net_active_window);
        return;
    }
    XChangeProperty(wm->display, wm->root, wm->atoms.net_active_window, XA_WINDOW,
                    32, PropModeReplace, (unsigned char *)&client->window, 1);
}

static size_t managed_client_count(const WindowManager *wm)
{
    const Client *client;
    size_t count = 0;

    for (client = wm->clients; client != NULL; client = client->next)
        ++count;
    return count;
}

static bool client_is_in_transient_subtree(WindowManager *wm,
                                           Client *client,
                                           Client *ancestor)
{
    size_t remaining = managed_client_count(wm) + 1;

    while (client != NULL && remaining-- > 0) {
        if (client == ancestor)
            return true;
        if (client->transient_for == None)
            return false;
        client = client_for_client_window(wm, client->transient_for);
    }
    return false;
}

static void raise_transient_subtree(WindowManager *wm, Client *client,
                                    Client *preferred, size_t remaining)
{
    Client *child;

    if (client == NULL || client->minimized || remaining == 0)
        return;
    XRaiseWindow(wm->display, client->frame);
    for (child = wm->clients; child != NULL; child = child->next) {
        if (child->transient_for == client->window && !child->minimized &&
            !client_is_in_transient_subtree(wm, preferred, child))
            raise_transient_subtree(wm, child, preferred, remaining - 1);
    }
    for (child = wm->clients; child != NULL; child = child->next) {
        if (child->transient_for == client->window && !child->minimized &&
            client_is_in_transient_subtree(wm, preferred, child))
            raise_transient_subtree(wm, child, preferred, remaining - 1);
    }
}

static Client *transient_family_root(WindowManager *wm, Client *client)
{
    Client *root = client;
    size_t remaining = managed_client_count(wm) + 1;

    if (client == NULL || client->minimized)
        return NULL;
    while (root->transient_for != None && remaining-- > 0) {
        Client *owner = client_for_client_window(wm, root->transient_for);

        if (owner == NULL || owner->minimized || owner == root ||
            client_is_in_transient_subtree(wm, owner, root))
            break;
        root = owner;
    }
    return root;
}

static void raise_client_family(WindowManager *wm, Client *client)
{
    Client *root = transient_family_root(wm, client);

    if (root == NULL)
        return;
    raise_transient_subtree(wm, root, client, managed_client_count(wm) + 1);
}

static bool active_has_foreign_frame_above(WindowManager *wm)
{
    Window root_return;
    Window parent_return;
    Window *children = NULL;
    unsigned int child_count = 0;
    unsigned int index;
    Client *active_root;
    bool active_seen = false;
    bool foreign_above = false;

    if (wm->active == NULL || wm->active->minimized)
        return false;
    active_root = transient_family_root(wm, wm->active);
    if (active_root == NULL ||
        !XQueryTree(wm->display, wm->root, &root_return, &parent_return,
                    &children, &child_count))
        return false;
    for (index = 0; index < child_count; ++index) {
        Client *above;

        if (children[index] == wm->active->frame) {
            active_seen = true;
            continue;
        }
        if (!active_seen)
            continue;
        above = client_for_window(wm, children[index]);
        if (above != NULL && children[index] == above->frame &&
            !above->minimized && transient_family_root(wm, above) != active_root) {
            foreign_above = true;
            break;
        }
    }
    if (children != NULL)
        XFree(children);
    return foreign_above;
}

static void restack_transient_children(WindowManager *wm, Client *owner,
                                       Client *preferred, size_t remaining)
{
    Client *child;
    int preferred_pass;

    if (owner == NULL || owner->minimized || remaining == 0)
        return;
    /* A child placed above its owner first remains above siblings inserted
     * later at the same position, so visit the preferred branch first. */
    for (preferred_pass = 1; preferred_pass >= 0; --preferred_pass) {
        for (child = wm->clients; child != NULL; child = child->next) {
            XWindowChanges changes;
            bool is_preferred;

            if (child->transient_for != owner->window || child->minimized)
                continue;
            is_preferred = client_is_in_transient_subtree(wm, preferred, child);
            if (is_preferred != (preferred_pass != 0))
                continue;
            changes.sibling = owner->frame;
            changes.stack_mode = Above;
            XConfigureWindow(wm->display, child->frame,
                             CWSibling | CWStackMode, &changes);
            restack_transient_children(wm, child, preferred, remaining - 1);
        }
    }
}

static void refresh_client_transient_for(WindowManager *wm, Client *client)
{
    Window owner = None;

    if (!XGetTransientForHint(wm->display, client->window, &owner) ||
        owner == client->window)
        owner = None;
    client->transient_for = owner;
    if (!client->minimized) {
        Client *preferred = client;

        if (wm->active != NULL &&
            client_is_in_transient_subtree(wm, wm->active, client))
            preferred = wm->active;
        raise_client_family(wm, preferred);
        update_focus_overlays(wm);
        raise_focused_internal_window(wm);
    }
}

static void update_focus_overlays(WindowManager *wm)
{
    Client *client;
    bool active_needs_raise = wm->internal_focus == INTERNAL_FOCUS_NONE &&
                              active_has_foreign_frame_above(wm);

    for (client = wm->clients; client != NULL; client = client->next) {
        bool intercept = !client->minimized &&
                         (client != wm->active ||
                          wm->internal_focus != INTERNAL_FOCUS_NONE ||
                          wm->session_confirmation.visible ||
                          active_needs_raise);

        if (client->focus_overlay == None)
            continue;
        if (intercept)
            XMapRaised(wm->display, client->focus_overlay);
        else
            XUnmapWindow(wm->display, client->focus_overlay);
    }
}

static void change_active_client(WindowManager *wm, Client *client,
                                 bool raise)
{
    Client *old_active = wm->active;

    if (client != NULL && client->minimized)
        return;
    wm->active = client;
    publish_active_client(wm, client);
    if (raise)
        raise_client_family(wm, client);
    update_focus_overlays(wm);
    if (old_active != NULL && old_active != client)
        draw_frame(wm, old_active);
    if (client != NULL)
        draw_frame(wm, client);
    raise_drag_outline(wm);
}

static void send_configure_notify(WindowManager *wm, Client *client)
{
    XEvent event;
    int requested_border = (int)client->saved_border;

    memset(&event, 0, sizeof(event));
    event.xconfigure.type = ConfigureNotify;
    event.xconfigure.display = wm->display;
    event.xconfigure.event = client->window;
    event.xconfigure.window = client->window;
    /* Synthetic coordinates describe the root-relative outer corner the
     * client would have with its requested border restored. */
    event.xconfigure.x = client->x - requested_border;
    event.xconfigure.y = client->y - requested_border;
    event.xconfigure.width = client->width;
    event.xconfigure.height = client->height;
    event.xconfigure.border_width = requested_border;
    event.xconfigure.above = None;
    event.xconfigure.override_redirect = False;
    XSendEvent(wm->display, client->window, False, StructureNotifyMask, &event);
}

static int constrain_dimension(int requested, int hard_minimum,
                               bool has_hint_minimum, int hint_minimum,
                               bool has_hint_maximum, int hint_maximum,
                               int base, bool has_increment, int increment)
{
    long long minimum = hard_minimum;
    long long maximum = INT_MAX;
    long long value = requested;

    if (has_hint_minimum && hint_minimum > minimum)
        minimum = hint_minimum;
    if (has_hint_maximum && hint_maximum > 0)
        maximum = hint_maximum;
    if (maximum < minimum)
        maximum = minimum;
    if (value < minimum)
        value = minimum;
    if (value > maximum)
        value = maximum;
    if (has_increment && increment > 0) {
        long long normalized_base = base < 0 ? 0 : base;
        long long smallest;

        if (normalized_base >= minimum) {
            smallest = normalized_base;
        } else {
            long long distance = minimum - normalized_base;

            smallest = normalized_base +
                       ((distance + increment - 1) / increment) * increment;
        }
        if (smallest <= maximum) {
            if (value <= smallest) {
                value = smallest;
            } else {
                value = normalized_base +
                        ((value - normalized_base) / increment) * increment;
                if (value < smallest)
                    value = smallest;
            }
        }
    }
    if (value < minimum)
        value = minimum;
    if (value > maximum)
        value = maximum;
    if (value < 1)
        value = 1;
    return (int)value;
}

static int normalized_win_gravity(int gravity)
{
    switch (gravity) {
    case NorthWestGravity:
    case NorthGravity:
    case NorthEastGravity:
    case WestGravity:
    case CenterGravity:
    case EastGravity:
    case SouthWestGravity:
    case SouthGravity:
    case SouthEastGravity:
    case StaticGravity:
        return gravity;
    default:
        return NorthWestGravity;
    }
}

static void update_client_win_gravity(Client *client, const XSizeHints *hints,
                                      bool have_hints)
{
    if (have_hints && (hints->flags & PWinGravity))
        client->win_gravity = normalized_win_gravity(hints->win_gravity);
    else
        client->win_gravity = NorthWestGravity;
}

static void constrain_client_size(WindowManager *wm, Client *client,
                                  int *width, int *height)
{
    XSizeHints hints;
    long supplied;
    int base_width = 0;
    int base_height = 0;

    memset(&hints, 0, sizeof(hints));
    if (!XGetWMNormalHints(wm->display, client->window, &hints, &supplied)) {
        update_client_win_gravity(client, NULL, false);
        *width = constrain_dimension(*width, MIN_CLIENT_WIDTH, false, 0,
                                     false, 0, 0, false, 0);
        *height = constrain_dimension(*height, MIN_CLIENT_HEIGHT, false, 0,
                                      false, 0, 0, false, 0);
        return;
    }
    update_client_win_gravity(client, &hints, true);
    if (hints.flags & PBaseSize) {
        base_width = hints.base_width;
        base_height = hints.base_height;
    } else if (hints.flags & PMinSize) {
        base_width = hints.min_width;
        base_height = hints.min_height;
    }
    *width = constrain_dimension(
        *width, MIN_CLIENT_WIDTH, (hints.flags & PMinSize) != 0,
        hints.min_width, (hints.flags & PMaxSize) != 0, hints.max_width,
        base_width, (hints.flags & PResizeInc) != 0, hints.width_inc);
    *height = constrain_dimension(
        *height, MIN_CLIENT_HEIGHT, (hints.flags & PMinSize) != 0,
        hints.min_height, (hints.flags & PMaxSize) != 0, hints.max_height,
        base_height, (hints.flags & PResizeInc) != 0, hints.height_inc);
}

static int horizontal_gravity_factor(int gravity)
{
    switch (gravity) {
    case NorthGravity:
    case CenterGravity:
    case SouthGravity:
        return 1;
    case NorthEastGravity:
    case EastGravity:
    case SouthEastGravity:
        return 2;
    default:
        return 0;
    }
}

static int vertical_gravity_factor(int gravity)
{
    switch (gravity) {
    case WestGravity:
    case CenterGravity:
    case EastGravity:
        return 1;
    case SouthWestGravity:
    case SouthGravity:
    case SouthEastGravity:
        return 2;
    default:
        return 0;
    }
}

static int inner_coordinate_after_gravity_resize(int coordinate, int old_size,
                                                 int new_size, int factor,
                                                 int leading_frame_extent)
{
    long long size_change = (long long)new_size - old_size;
    long long adjusted = (long long)coordinate - factor * size_change / 2;

    if (adjusted < (long long)INT_MIN + leading_frame_extent)
        return INT_MIN + leading_frame_extent;
    if (adjusted > INT_MAX)
        return INT_MAX;
    return (int)adjusted;
}

static int inner_x_from_requested_outer(const Client *client, int requested_x)
{
    long long frame_x;

    if (client->win_gravity == StaticGravity) {
        frame_x = (long long)requested_x + client->saved_border - FRAME_LEFT;
    } else {
        long long client_outer_width =
            (long long)client->width + 2LL * client->saved_border;
        long long difference = client_outer_width - frame_width(client);

        frame_x = (long long)requested_x +
                  horizontal_gravity_factor(client->win_gravity) * difference / 2;
    }
    if (frame_x < INT_MIN)
        return INT_MIN + FRAME_LEFT;
    if (frame_x > INT_MAX - FRAME_LEFT)
        return INT_MAX;
    return (int)frame_x + FRAME_LEFT;
}

static int inner_y_from_requested_outer(const Client *client, int requested_y)
{
    long long frame_y;

    if (client->win_gravity == StaticGravity) {
        frame_y = (long long)requested_y + client->saved_border - FRAME_TOP;
    } else {
        long long client_outer_height =
            (long long)client->height + 2LL * client->saved_border;
        long long difference = client_outer_height - frame_height(client);

        frame_y = (long long)requested_y +
                  vertical_gravity_factor(client->win_gravity) * difference / 2;
    }
    if (frame_y < INT_MIN)
        return INT_MIN + FRAME_TOP;
    if (frame_y > INT_MAX - FRAME_TOP)
        return INT_MAX;
    return (int)frame_y + FRAME_TOP;
}

static void apply_client_geometry(WindowManager *wm, Client *client)
{
    XMoveResizeWindow(wm->display, client->frame,
                      client->x - FRAME_LEFT, client->y - FRAME_TOP,
                      (unsigned)frame_width(client), (unsigned)frame_height(client));
    XMoveResizeWindow(wm->display, client->window, FRAME_LEFT, FRAME_TOP,
                      (unsigned)client->width, (unsigned)client->height);
    if (client->focus_overlay != None)
        XMoveResizeWindow(wm->display, client->focus_overlay, FRAME_LEFT,
                          FRAME_TOP, (unsigned)client->width,
                          (unsigned)client->height);
    draw_frame(wm, client);
}

static void reposition_icons(WindowManager *wm)
{
    DesktopIcon *icon;
    int minimized_counts[MAX_MONITORS] = {0};

    for (icon = wm->icons; icon != NULL; icon = icon->next) {
        size_t monitor_index;
        const MonitorGeometry *monitor;
        int x;
        int y;

        if (icon->manual_position && icon->preferred.valid) {
            int width;
            int height;

            monitor_index = geometry_from_placement(
                wm, &icon->preferred, &x, &y, &width, &height, false);
            monitor = monitor_at(wm, monitor_index);
            if (monitor == NULL)
                continue;
            width = ICON_WIDTH;
            height = ICON_HEIGHT;
            clamp_geometry_to_monitor(monitor, &x, &y, &width, &height);
            goto position_icon;
        }
        monitor_index = icon->kind == ICON_APPLICATIONS ||
                                icon->kind == ICON_CONTROL_PANEL
                            ? 0U
                            : monitor_index_for_anchor(wm, &icon->monitor);
        monitor = monitor_at(wm, monitor_index);
        if (monitor == NULL)
            continue;
        if (icon->kind == ICON_APPLICATIONS) {
            x = monitor->x + ICON_MARGIN;
            y = monitor->y + ICON_MARGIN;
        } else if (icon->kind == ICON_CONTROL_PANEL) {
            x = monitor->x + ICON_MARGIN;
            y = monitor->y + ICON_MARGIN + ICON_HEIGHT + ICON_GAP;
        } else {
            int columns;
            int rows;
            int slot;
            int column;
            int row;

            if (icon->client == NULL || !icon->client->minimized)
                continue;
            columns = (monitor->width - ICON_MARGIN * 2 + ICON_GAP) /
                      (ICON_WIDTH + ICON_GAP);
            rows = (monitor->height - ICON_MARGIN * 2 + ICON_GAP) /
                   (ICON_HEIGHT + ICON_GAP);
            if (columns < 1)
                columns = 1;
            if (rows < 1)
                rows = 1;
            slot = minimized_counts[monitor_index]++;
            column = slot % columns;
            row = slot / columns;
            if (row >= rows)
                row = rows - 1;
            x = monitor->x + ICON_MARGIN +
                column * (ICON_WIDTH + ICON_GAP);
            y = monitor->y + monitor->height - ICON_MARGIN - ICON_HEIGHT -
                row * (ICON_HEIGHT + ICON_GAP);
        }
        if (x + ICON_WIDTH > monitor->x + monitor->width)
            x = monitor->x + monitor->width - ICON_WIDTH;
        if (y + ICON_HEIGHT > monitor->y + monitor->height)
            y = monitor->y + monitor->height - ICON_HEIGHT;
        if (x < monitor->x)
            x = monitor->x;
        if (y < monitor->y)
            y = monitor->y;
position_icon:
        icon->x = x;
        icon->y = y;
        set_monitor_anchor(&icon->monitor, monitor);
        XMoveWindow(wm->display, icon->window, x, y);
        XLowerWindow(wm->display, icon->window);
    }
}

static void remember_desktop_icon_position(WindowManager *wm,
                                           DesktopIcon *icon)
{
    size_t monitor_index;
    Win31xDesktopPlacement *saved = NULL;

    if (icon == NULL)
        return;
    monitor_index = monitor_index_for_rectangle(
        wm, icon->x, icon->y, ICON_WIDTH, ICON_HEIGHT);
    placement_from_geometry(wm, &icon->preferred, monitor_index, icon->x,
                            icon->y, ICON_WIDTH, ICON_HEIGHT,
                            CLIENT_LAYOUT_NORMAL);
    if (!icon->preferred.valid)
        return;
    icon->manual_position = true;
    set_monitor_anchor(&icon->monitor, monitor_at(wm, monitor_index));
    if (icon->kind == ICON_APPLICATIONS)
        saved = &wm->desktop_state.applications_icon;
    else if (icon->kind == ICON_CONTROL_PANEL)
        saved = &wm->desktop_state.control_panel_icon;
    if (saved != NULL) {
        *saved = icon->preferred;
        mark_desktop_state_dirty(wm);
    }
}

static DesktopIcon *create_desktop_icon(WindowManager *wm, IconKind kind,
                                        Client *client)
{
    XSetWindowAttributes attributes;
    DesktopIcon *icon = calloc(1, sizeof(*icon));
    long client_window;

    if (icon == NULL)
        return NULL;
    attributes.override_redirect = True;
    attributes.background_pixel = wm->theme.desktop;
    attributes.event_mask = ExposureMask | ButtonPressMask |
                            ButtonReleaseMask | PointerMotionMask;
    attributes.cursor = wm->arrow_cursor;
    icon->window = XCreateWindow(wm->display, wm->root, 0, 0, ICON_WIDTH,
                                 ICON_HEIGHT, 0, CopyFromParent, InputOutput,
                                 CopyFromParent, CWOverrideRedirect | CWBackPixel |
                                 CWEventMask | CWCursor, &attributes);
    icon->kind = kind;
    icon->client = client;
    if (kind == ICON_APPLICATIONS &&
        wm->desktop_state.applications_icon.valid) {
        icon->preferred = wm->desktop_state.applications_icon;
        icon->manual_position = true;
    } else if (kind == ICON_CONTROL_PANEL &&
               wm->desktop_state.control_panel_icon.valid) {
        icon->preferred = wm->desktop_state.control_panel_icon;
        icon->manual_position = true;
    }
    if (client != NULL)
        set_monitor_anchor(&icon->monitor,
                           monitor_at(wm, monitor_index_for_client(wm,
                                                                  client)));
    else
        set_monitor_anchor(&icon->monitor, monitor_at(wm, 0U));
    icon->next = wm->icons;
    wm->icons = icon;
    if (kind == ICON_APPLICATIONS)
        set_internal_role(wm, icon->window, "applications-icon");
    else if (kind == ICON_CONTROL_PANEL)
        set_internal_role(wm, icon->window, "control-panel-icon");
    else
        set_internal_role(wm, icon->window, "minimized-icon");
    if (client != NULL) {
        client_window = (long)client->window;
        XChangeProperty(wm->display, icon->window, wm->atoms.win31x_client,
                        XA_WINDOW, 32, PropModeReplace,
                        (unsigned char *)&client_window, 1);
    }
    XMapWindow(wm->display, icon->window);
    reposition_icons(wm);
    return icon;
}

static void destroy_desktop_icon(WindowManager *wm, DesktopIcon *target)
{
    DesktopIcon **cursor;

    if (target == NULL)
        return;
    if (wm->drag.kind == DRAG_MOVE_ICON && wm->drag.icon == target)
        cancel_drag(wm, CurrentTime);
    for (cursor = &wm->icons; *cursor != NULL; cursor = &(*cursor)->next) {
        if (*cursor == target) {
            *cursor = target->next;
            break;
        }
    }
    if (wm->applications_icon == target)
        wm->applications_icon = NULL;
    if (wm->control_panel_icon == target)
        wm->control_panel_icon = NULL;
    XDestroyWindow(wm->display, target->window);
    free(target);
    reposition_icons(wm);
}

static bool client_supports_protocol(WindowManager *wm, Client *client, Atom protocol)
{
    Atom *protocols = NULL;
    int count = 0;
    int index;
    bool supported = false;

    if (XGetWMProtocols(wm->display, client->window, &protocols, &count)) {
        for (index = 0; index < count; ++index) {
            if (protocols[index] == protocol) {
                supported = true;
                break;
            }
        }
    }
    if (protocols != NULL)
        XFree(protocols);
    return supported;
}

static void focus_client(WindowManager *wm, Client *client, Time time)
{
    XWMHints *hints;
    bool accepts_input = true;
    bool take_focus;
    bool leaving_internal;

    if (client == NULL || client->minimized)
        return;
    if (wm->session_confirmation.visible) {
        refocus_session_confirmation(wm, time);
        return;
    }
    set_active_monitor_from_client(wm, client);
    leaving_internal = wm->internal_focus != INTERNAL_FOCUS_NONE;
    wm->internal_focus = INTERNAL_FOCUS_NONE;
    change_active_client(wm, client, true);
    hints = XGetWMHints(wm->display, client->window);
    if (hints != NULL && (hints->flags & InputHint))
        accepts_input = hints->input != False;
    take_focus = client_supports_protocol(wm, client, wm->atoms.wm_take_focus);
    if (accepts_input)
        XSetInputFocus(wm->display, client->window, RevertToPointerRoot, time);
    else if (leaving_internal)
        XSetInputFocus(wm->display, wm->root, RevertToPointerRoot, time);
    if (take_focus) {
        XEvent event;

        memset(&event, 0, sizeof(event));
        event.xclient.type = ClientMessage;
        event.xclient.window = client->window;
        event.xclient.message_type = wm->atoms.wm_protocols;
        event.xclient.format = 32;
        event.xclient.data.l[0] = (long)wm->atoms.wm_take_focus;
        event.xclient.data.l[1] = (long)time;
        XSendEvent(wm->display, client->window, False, NoEventMask, &event);
    }
    if (hints != NULL)
        XFree(hints);
    if (leaving_internal) {
        if (wm->launcher_visible)
            draw_launcher(wm);
        if (wm->control_panel.visible)
            draw_control_panel(wm);
        if (wm->run_dialog.visible)
            draw_run_dialog(wm);
        if (wm->task_manager.visible)
            draw_task_manager(wm);
    }
}

static void activate_internal_window(WindowManager *wm, InternalFocus focus,
                                     Time time)
{
    Window window;

    if (wm->session_confirmation.visible) {
        refocus_session_confirmation(wm, time);
        return;
    }
    if (focus == INTERNAL_FOCUS_APPLICATIONS && wm->launcher_visible) {
        window = wm->launcher;
    } else if (focus == INTERNAL_FOCUS_CONTROL_PANEL &&
               wm->control_panel.visible) {
        window = wm->control_panel.window;
    } else if (focus == INTERNAL_FOCUS_RUN && wm->run_dialog.visible) {
        window = wm->run_dialog.window;
    } else if (focus == INTERNAL_FOCUS_TASK_MANAGER &&
               wm->task_manager.visible) {
        window = wm->task_manager.window;
    } else {
        return;
    }
    wm->internal_focus = focus;
    if (focus == INTERNAL_FOCUS_APPLICATIONS) {
        set_active_monitor(
            wm, monitor_index_for_rectangle(
                    wm, wm->launcher_x, wm->launcher_y,
                    wm->launcher_width, wm->launcher_height));
    } else if (focus == INTERNAL_FOCUS_CONTROL_PANEL) {
        set_active_monitor(
            wm, monitor_index_for_rectangle(
                    wm, wm->control_panel.x, wm->control_panel.y,
                    wm->control_panel.width, wm->control_panel.height));
    } else if (focus == INTERNAL_FOCUS_RUN) {
        set_active_monitor(
            wm, monitor_index_for_rectangle(
                    wm, wm->run_dialog.x, wm->run_dialog.y,
                    wm->run_dialog.width, wm->run_dialog.height));
    } else {
        set_active_monitor(
            wm, monitor_index_for_rectangle(
                    wm, wm->task_manager.x, wm->task_manager.y,
                    wm->task_manager.width, wm->task_manager.height));
    }
    publish_active_client(wm, NULL);
    update_focus_overlays(wm);
    if (wm->active != NULL)
        draw_frame(wm, wm->active);
    if (wm->launcher_visible)
        draw_launcher(wm);
    if (wm->control_panel.visible)
        draw_control_panel(wm);
    if (wm->run_dialog.visible)
        draw_run_dialog(wm);
    if (wm->task_manager.visible)
        draw_task_manager(wm);
    XRaiseWindow(wm->display, window);
    XSetInputFocus(wm->display, window, RevertToPointerRoot, time);
}

static bool window_is_descendant_of(WindowManager *wm, Window window,
                                    Window ancestor)
{
    unsigned int remaining = 256;

    while (window != None && window != PointerRoot && remaining-- > 0) {
        Window root_return;
        Window parent = None;
        Window *children = NULL;
        unsigned int child_count = 0;

        if (window == ancestor)
            return true;
        if (window == wm->root ||
            !XQueryTree(wm->display, window, &root_return, &parent,
                        &children, &child_count)) {
            if (children != NULL)
                XFree(children);
            return false;
        }
        if (children != NULL)
            XFree(children);
        if (parent == window)
            return false;
        window = parent;
    }
    return false;
}

static bool client_currently_has_focus(WindowManager *wm, Client *client)
{
    Window focused = None;
    int revert_to;

    XGetInputFocus(wm->display, &focused, &revert_to);
    return window_is_descendant_of(wm, focused, client->window);
}

static Client *next_visible_client(WindowManager *wm, Client *after)
{
    Client *client;

    if (wm->clients == NULL)
        return NULL;
    client = after != NULL && after->next != NULL ? after->next : wm->clients;
    while (client != after) {
        if (!client->minimized)
            return client;
        client = client->next != NULL ? client->next : wm->clients;
        if (after == NULL && client == wm->clients)
            break;
    }
    if (after != NULL && !after->minimized)
        return after;
    return NULL;
}

static void focus_next(WindowManager *wm, Time time)
{
    Client *next = next_visible_client(wm, wm->active);

    if (next != NULL)
        focus_client(wm, next, time);
    else if (wm->internal_focus == INTERNAL_FOCUS_NONE) {
        change_active_client(wm, NULL, false);
        XSetInputFocus(wm->display, wm->root, RevertToPointerRoot, time);
    }
}

static void close_client(WindowManager *wm, Client *client, Time time)
{
    if (client_supports_protocol(wm, client, wm->atoms.wm_delete_window)) {
        XEvent event;

        memset(&event, 0, sizeof(event));
        event.xclient.type = ClientMessage;
        event.xclient.window = client->window;
        event.xclient.message_type = wm->atoms.wm_protocols;
        event.xclient.format = 32;
        event.xclient.data.l[0] = (long)wm->atoms.wm_delete_window;
        event.xclient.data.l[1] = (long)time;
        XSendEvent(wm->display, client->window, False, NoEventMask, &event);
    } else {
        XKillClient(wm->display, client->window);
    }
}

static void cancel_client_drag(WindowManager *wm, Client *client)
{
    if (wm->drag.client != client)
        return;
    cancel_drag(wm, CurrentTime);
}

static void minimize_client_window(WindowManager *wm, Client *client,
                                   bool show_desktop_icon, Window owner)
{
    cancel_client_drag(wm, client);
    client->minimized = true;
    client->minimized_with_owner = owner;
    if (show_desktop_icon) {
        if (client->icon == NULL)
            client->icon = create_desktop_icon(wm, ICON_MINIMIZED, client);
        else {
            set_monitor_anchor(
                &client->icon->monitor,
                monitor_at(wm, monitor_index_for_client(wm, client)));
            XMapWindow(wm->display, client->icon->window);
        }
    } else if (client->icon != NULL) {
        XUnmapWindow(wm->display, client->icon->window);
    }
    set_wm_state(wm, client, IconicState);
    set_hidden_state(wm, client, true);
    ++client->expected_unmaps;
    XUnmapWindow(wm->display, client->focus_overlay);
    XUnmapWindow(wm->display, client->window);
    XUnmapWindow(wm->display, client->frame);
}

static void minimize_client(WindowManager *wm, Client *client)
{
    Client *dependent;

    if (client == NULL || client->minimized)
        return;
    for (dependent = wm->clients; dependent != NULL;
         dependent = dependent->next) {
        if (dependent != client && !dependent->minimized &&
            client_is_in_transient_subtree(wm, dependent, client))
            minimize_client_window(wm, dependent, false, client->window);
    }
    minimize_client_window(wm, client, true, None);
    if (wm->internal_focus == INTERNAL_FOCUS_NONE &&
        wm->active != NULL && wm->active->minimized)
        focus_next(wm, CurrentTime);
    reposition_icons(wm);
    if (wm->task_manager.visible)
        draw_task_manager(wm);
}

static void restore_client_window(WindowManager *wm, Client *client)
{
    client->minimized = false;
    client->minimized_with_owner = None;
    if (client->icon != NULL)
        XUnmapWindow(wm->display, client->icon->window);
    set_wm_state(wm, client, NormalState);
    set_hidden_state(wm, client, false);
    XMapWindow(wm->display, client->window);
    XMapWindow(wm->display, client->frame);
    send_configure_notify(wm, client);
}

static void restore_client(WindowManager *wm, Client *client, Time time)
{
    Client *requested = client;
    Client *focus_target;
    Client *dependent;
    size_t remaining;
    Window owner_window;

    if (client == NULL)
        return;
    if (!client->minimized) {
        focus_client(wm, client, time);
        return;
    }
    remaining = managed_client_count(wm) + 1;
    while (client->minimized_with_owner != None && remaining-- > 0) {
        Client *owner = client_for_client_window(
            wm, client->minimized_with_owner);

        if (owner == NULL || !owner->minimized || owner == client)
            break;
        client = owner;
    }
    owner_window = client->window;
    focus_target = requested;
    restore_client_window(wm, client);
    for (dependent = wm->clients; dependent != NULL;
         dependent = dependent->next) {
        if (dependent->minimized &&
            dependent->minimized_with_owner == owner_window) {
            restore_client_window(wm, dependent);
            if (requested == client && focus_target == requested)
                focus_target = dependent;
        }
    }
    if (focus_target->minimized)
        focus_target = client;
    reposition_icons(wm);
    focus_client(wm, focus_target, time);
    if (wm->task_manager.visible)
        draw_task_manager(wm);
}

static void expose_orphaned_transients(WindowManager *wm, Window owner)
{
    Client *client;

    for (client = wm->clients; client != NULL; client = client->next) {
        if (!client->minimized || client->minimized_with_owner != owner)
            continue;
        client->minimized_with_owner = None;
        if (client->icon == NULL)
            client->icon = create_desktop_icon(wm, ICON_MINIMIZED, client);
        else {
            set_monitor_anchor(
                &client->icon->monitor,
                monitor_at(wm, monitor_index_for_client(wm, client)));
            XMapWindow(wm->display, client->icon->window);
        }
        set_wm_state(wm, client, IconicState);
    }
    reposition_icons(wm);
}

static void remove_client(WindowManager *wm, Client *client, bool destroyed,
                          bool remap, bool already_reparented)
{
    Client **cursor;
    bool was_active;

    if (client == NULL)
        return;
    cancel_client_drag(wm, client);
    for (cursor = &wm->clients; *cursor != NULL; cursor = &(*cursor)->next) {
        if (*cursor == client) {
            *cursor = client->next;
            break;
        }
    }
    if (wm->task_manager.closing_application == client->window) {
        wm->task_manager.closing_application = None;
        if (wm->task_manager.status_is_closing_application) {
            wm->task_manager.status[0] = '\0';
            wm->task_manager.status_is_closing_application = false;
            wm->task_manager.status_is_refresh_error = false;
        }
    }
    if (!remap)
        expose_orphaned_transients(wm, client->window);
    was_active = wm->active == client;
    if (was_active)
        change_active_client(wm, NULL, false);
    if (client->icon != NULL) {
        destroy_desktop_icon(wm, client->icon);
        client->icon = NULL;
    }
    if (!destroyed) {
        XGrabServer(wm->display);
        XUnmapWindow(wm->display, client->frame);
        XUngrabButton(wm->display, AnyButton, AnyModifier,
                      client->focus_overlay);
        if (!already_reparented)
            XReparentWindow(wm->display, client->window, wm->root,
                            client->x - (int)client->saved_border,
                            client->y - (int)client->saved_border);
        XSetWindowBorderWidth(wm->display, client->window, client->saved_border);
        XRemoveFromSaveSet(wm->display, client->window);
        XDeleteProperty(wm->display, client->window, wm->atoms.net_frame_extents);
        if (remap) {
            set_wm_state(wm, client, NormalState);
            set_hidden_state(wm, client, false);
            XMapWindow(wm->display, client->window);
        } else {
            XDeleteProperty(wm->display, client->window, wm->atoms.wm_state);
            set_hidden_state(wm, client, false);
        }
        XUngrabServer(wm->display);
    }
    if (client->focus_overlay != None) {
        XDestroyWindow(wm->display, client->focus_overlay);
        client->focus_overlay = None;
    }
    XDestroyWindow(wm->display, client->frame);
    free_rendered_icon(wm, &client->actual_icon_small);
    free_rendered_icon(wm, &client->actual_icon_large);
    free(client->title);
    free(client->class_name);
    free(client->state_base_identity);
    free(client->state_identity);
    free(client);
    update_client_list(wm);
    if (wm->task_manager.visible) {
        task_manager_select_first_application(wm);
        draw_task_manager(wm);
    }
    if (was_active && wm->internal_focus == INTERNAL_FOCUS_NONE)
        focus_next(wm, CurrentTime);
}

static void set_frame_extents(WindowManager *wm, Client *client)
{
    long extents[4] = {FRAME_LEFT, FRAME_RIGHT, FRAME_TOP, FRAME_BOTTOM};

    XChangeProperty(wm->display, client->window, wm->atoms.net_frame_extents,
                    XA_CARDINAL, 32, PropModeReplace,
                    (unsigned char *)extents, 4);
}

static void keep_client_on_screen(WindowManager *wm, Client *client)
{
    const MonitorGeometry *monitor = monitor_at(
        wm, monitor_index_for_client(wm, client));
    int outer_width = frame_width(client);
    int frame_x = client->x - FRAME_LEFT;
    int frame_y = client->y - FRAME_TOP;

    if (monitor == NULL)
        return;
    if (frame_y < monitor->y)
        client->y = monitor->y + FRAME_TOP;
    if (frame_x > monitor->x + monitor->width - 48)
        client->x = monitor->x + monitor->width - 48 + FRAME_LEFT;
    if (frame_x + outer_width < monitor->x + 48)
        client->x = monitor->x + 48 - outer_width + FRAME_LEFT;
    if (frame_y > monitor->y + monitor->height - TITLE_HEIGHT)
        client->y = monitor->y + monitor->height - TITLE_HEIGHT + FRAME_TOP;
}

static void remember_client_geometry(Client *client)
{
    client->restore_x = client->x;
    client->restore_y = client->y;
    client->restore_width = client->width;
    client->restore_height = client->height;
    client->restore_valid = true;
}

static void capture_client_placement(WindowManager *wm, Client *client,
                                     bool replace_monitor_fallback)
{
    Win31xDesktopPlacement placement;
    size_t monitor_index;
    int x;
    int y;
    int width;
    int height;

    if (client == NULL || !client->state_persistable ||
        client->state_identity == NULL || client->state_identity[0] == '\0')
        return;
    if (client->state_monitor_fallback && !replace_monitor_fallback)
        return;
    if (replace_monitor_fallback)
        client->state_monitor_fallback = false;
    if (client->layout == CLIENT_LAYOUT_NORMAL) {
        x = client->x;
        y = client->y;
        width = client->width;
        height = client->height;
        monitor_index = monitor_index_for_rectangle(
            wm, x - FRAME_LEFT, y - FRAME_TOP, frame_width(client),
            frame_height(client));
    } else if (client->restore_valid) {
        x = client->restore_x;
        y = client->restore_y;
        width = client->restore_width;
        height = client->restore_height;
        monitor_index = client->layout_monitor.valid
                            ? monitor_index_for_anchor(wm,
                                                       &client->layout_monitor)
                            : monitor_index_for_rectangle(
                                  wm, x - FRAME_LEFT, y - FRAME_TOP,
                                  width + FRAME_LEFT + FRAME_RIGHT,
                                  height + FRAME_TOP + FRAME_BOTTOM);
    } else {
        return;
    }
    placement_from_geometry(wm, &placement, monitor_index, x, y, width,
                            height, client->layout);
    if (!placement.valid)
        return;
    placement.layout_before_maximize =
        (Win31xDesktopLayout)client->layout_before_maximize;
    if (win31x_desktop_state_upsert_client(
            &wm->desktop_state, client->state_identity, &placement) < 0) {
        if (errno != ENOSPC)
            fprintf(stderr, "win31x: could not remember window layout: %s\n",
                    strerror(errno));
        return;
    }
    mark_desktop_state_dirty(wm);
}

static ClientLayout restore_client_placement(WindowManager *wm, Client *client,
                                              size_t *layout_monitor)
{
    const Win31xDesktopClientRecord *record;
    const MonitorGeometry *monitor;
    int x;
    int y;
    int width;
    int height;

    *layout_monitor = 0U;
    if (!client->state_persistable || client->state_identity == NULL ||
        client->state_identity[0] == '\0')
        return CLIENT_LAYOUT_NORMAL;
    record = win31x_desktop_state_find_client(&wm->desktop_state,
                                               client->state_identity);
    if (record == NULL || !record->placement.valid)
        return CLIENT_LAYOUT_NORMAL;
    client->layout_before_maximize =
        (ClientLayout)record->placement.layout_before_maximize;
    client->state_monitor_fallback =
        !placement_monitor_is_available(wm, &record->placement);
    *layout_monitor = geometry_from_placement(
        wm, &record->placement, &x, &y, &width, &height, false);
    monitor = monitor_at(wm, *layout_monitor);
    if (monitor == NULL)
        return CLIENT_LAYOUT_NORMAL;
    if (width > monitor->width - FRAME_LEFT - FRAME_RIGHT)
        width = monitor->width - FRAME_LEFT - FRAME_RIGHT;
    if (height > monitor->height - FRAME_TOP - FRAME_BOTTOM)
        height = monitor->height - FRAME_TOP - FRAME_BOTTOM;
    if (width < 1)
        width = 1;
    if (height < 1)
        height = 1;
    client->x = x;
    client->y = y;
    client->width = width;
    client->height = height;
    constrain_client_size(wm, client, &client->width, &client->height);
    keep_client_on_screen(wm, client);
    client->state_restored = true;
    return (ClientLayout)record->placement.layout;
}

static bool reflow_client_to_saved_placement(WindowManager *wm, Client *client)
{
    const Win31xDesktopClientRecord *record;
    const MonitorGeometry *monitor;
    size_t monitor_index;
    int x;
    int y;
    int width;
    int height;

    if (!client->state_persistable || client->state_identity == NULL)
        return false;
    record = win31x_desktop_state_find_client(&wm->desktop_state,
                                               client->state_identity);
    if (record == NULL || !record->placement.valid ||
        (ClientLayout)record->placement.layout != client->layout)
        return false;
    client->layout_before_maximize =
        (ClientLayout)record->placement.layout_before_maximize;
    client->state_monitor_fallback =
        !placement_monitor_is_available(wm, &record->placement);
    monitor_index = geometry_from_placement(
        wm, &record->placement, &x, &y, &width, &height, false);
    monitor = monitor_at(wm, monitor_index);
    if (monitor == NULL)
        return false;
    if (width > monitor->width - FRAME_LEFT - FRAME_RIGHT)
        width = monitor->width - FRAME_LEFT - FRAME_RIGHT;
    if (height > monitor->height - FRAME_TOP - FRAME_BOTTOM)
        height = monitor->height - FRAME_TOP - FRAME_BOTTOM;
    if (width < 1)
        width = 1;
    if (height < 1)
        height = 1;
    constrain_client_size(wm, client, &width, &height);
    if (client->layout == CLIENT_LAYOUT_NORMAL) {
        client->x = x;
        client->y = y;
        client->width = width;
        client->height = height;
        keep_client_on_screen(wm, client);
        apply_client_geometry(wm, client);
        send_configure_notify(wm, client);
    } else {
        client->restore_x = x;
        client->restore_y = y;
        client->restore_width = width;
        client->restore_height = height;
        client->restore_valid = true;
        set_monitor_anchor(&client->layout_monitor, monitor);
        reapply_client_layout(wm, client);
    }
    return true;
}

static void apply_client_layout_geometry(WindowManager *wm, Client *client)
{
    size_t monitor_index = client->layout_monitor.valid
                               ? monitor_index_for_anchor(
                                     wm, &client->layout_monitor)
                               : monitor_index_for_client(wm, client);
    const MonitorGeometry *monitor = monitor_at(wm, monitor_index);
    int outer_x;
    int outer_width;
    int width;
    int height;

    if (monitor == NULL)
        return;
    set_monitor_anchor(&client->layout_monitor, monitor);
    outer_x = monitor->x;
    outer_width = monitor->width;
    height = monitor->height - FRAME_TOP - FRAME_BOTTOM;

    if (client->layout == CLIENT_LAYOUT_SNAP_LEFT) {
        outer_width = monitor->width / 2;
    } else if (client->layout == CLIENT_LAYOUT_SNAP_RIGHT) {
        outer_x = monitor->x + monitor->width / 2;
        outer_width = monitor->width - monitor->width / 2;
    }
    width = outer_width - FRAME_LEFT - FRAME_RIGHT;
    if (width < 1)
        width = 1;
    if (height < 1)
        height = 1;
    constrain_client_size(wm, client, &width, &height);
    client->width = width;
    client->height = height;
    client->y = monitor->y + FRAME_TOP;
    if (client->layout == CLIENT_LAYOUT_SNAP_RIGHT) {
        client->x = outer_x + outer_width - frame_width(client) + FRAME_LEFT;
    } else {
        client->x = outer_x + FRAME_LEFT;
    }
}

static void reapply_client_layout(WindowManager *wm, Client *client)
{
    if (client->layout == CLIENT_LAYOUT_NORMAL)
        return;
    apply_client_layout_geometry(wm, client);
    set_maximized_state(wm, client,
                        client->layout == CLIENT_LAYOUT_MAXIMIZED);
    apply_client_geometry(wm, client);
    send_configure_notify(wm, client);
}

static void set_client_layout(WindowManager *wm, Client *client,
                              ClientLayout layout)
{
    if (layout == CLIENT_LAYOUT_NORMAL &&
        client->layout == CLIENT_LAYOUT_MAXIMIZED &&
        client->layout_before_maximize != CLIENT_LAYOUT_NORMAL)
        layout = client->layout_before_maximize;
    if (layout == CLIENT_LAYOUT_MAXIMIZED &&
        client->layout != CLIENT_LAYOUT_MAXIMIZED)
        client->layout_before_maximize = client->layout;
    if (layout == CLIENT_LAYOUT_NORMAL) {
        if (client->layout == CLIENT_LAYOUT_NORMAL)
            return;
        client->layout = CLIENT_LAYOUT_NORMAL;
        if (client->restore_valid) {
            client->x = client->restore_x;
            client->y = client->restore_y;
            client->width = client->restore_width;
            client->height = client->restore_height;
        }
        client->restore_valid = false;
        client->layout_monitor.valid = false;
        constrain_client_size(wm, client, &client->width, &client->height);
        keep_client_on_screen(wm, client);
    } else {
        if (!client->layout_monitor.valid) {
            set_monitor_anchor(
                &client->layout_monitor,
                monitor_at(wm, monitor_index_for_client(wm, client)));
        }
        if (client->layout == CLIENT_LAYOUT_NORMAL)
            remember_client_geometry(client);
        client->layout = layout;
        apply_client_layout_geometry(wm, client);
    }
    set_maximized_state(wm, client,
                        client->layout == CLIENT_LAYOUT_MAXIMIZED);
    apply_client_geometry(wm, client);
    send_configure_notify(wm, client);
}

static void toggle_client_maximize(WindowManager *wm, Client *client)
{
    set_client_layout(wm, client,
                      client->layout == CLIENT_LAYOUT_MAXIMIZED
                          ? CLIENT_LAYOUT_NORMAL
                          : CLIENT_LAYOUT_MAXIMIZED);
    capture_client_placement(wm, client, true);
}

static void normalize_client_layout(WindowManager *wm, Client *client)
{
    if (client->layout == CLIENT_LAYOUT_NORMAL)
        return;
    client->layout = CLIENT_LAYOUT_NORMAL;
    client->layout_before_maximize = CLIENT_LAYOUT_NORMAL;
    client->restore_valid = false;
    client->layout_monitor.valid = false;
    set_maximized_state(wm, client, false);
    draw_frame(wm, client);
}

static void handle_normal_hints_change(WindowManager *wm, Client *client)
{
    int width = client->width;
    int height = client->height;

    if (client->layout != CLIENT_LAYOUT_NORMAL) {
        reapply_client_layout(wm, client);
        return;
    }
    /* A gravity change affects subsequent client position requests; it does
     * not by itself move a window that is already managed.  New size limits,
     * on the other hand, are applied immediately. */
    constrain_client_size(wm, client, &width, &height);
    if (width == client->width && height == client->height)
        return;
    client->x = inner_coordinate_after_gravity_resize(
        client->x, client->width, width,
        horizontal_gravity_factor(client->win_gravity), FRAME_LEFT);
    client->y = inner_coordinate_after_gravity_resize(
        client->y, client->height, height,
        vertical_gravity_factor(client->win_gravity), FRAME_TOP);
    client->width = width;
    client->height = height;
    keep_client_on_screen(wm, client);
    apply_client_geometry(wm, client);
    send_configure_notify(wm, client);
    capture_client_placement(wm, client, false);
}

static Client *manage_window(WindowManager *wm, Window window, bool startup)
{
    XWindowAttributes attributes;
    XSetWindowAttributes frame_attributes;
    XSetWindowAttributes overlay_attributes;
    XWMHints *hints;
    Client *client;
    ClientLayout saved_layout = CLIENT_LAYOUT_NORMAL;
    ClientLayout saved_layout_before_maximize = CLIENT_LAYOUT_NORMAL;
    size_t saved_layout_monitor = 0U;
    Window state_transient = None;
    bool initially_iconic = false;
    bool initially_maximized;

    if (client_for_window(wm, window) != NULL ||
        !XGetWindowAttributes(wm->display, window, &attributes) ||
        attributes.override_redirect || attributes.class == InputOnly)
        return NULL;
    client = calloc(1, sizeof(*client));
    if (client == NULL)
        return NULL;
    client->window = window;
    initially_maximized =
        net_wm_state_contains(wm, window,
                              wm->atoms.net_wm_state_maximized_horz) ||
        net_wm_state_contains(wm, window,
                              wm->atoms.net_wm_state_maximized_vert);
    client->initial_maximize_precedence = initially_maximized;
    client->title = window_title(wm, window);
    client->class_name = window_class(wm, window);
    client->state_base_identity = window_state_identity(wm, window);
    client->state_identity = client_state_identity(
        wm, client, client->state_base_identity, &client->state_instance);
    client->state_persistable =
        client->state_identity != NULL && client->state_identity[0] != '\0' &&
        (!XGetTransientForHint(wm->display, window, &state_transient) ||
         state_transient == None);
    client->width = attributes.width;
    client->height = attributes.height;
    client->saved_border = (unsigned)attributes.border_width;
    constrain_client_size(wm, client, &client->width, &client->height);
    client->x = inner_x_from_requested_outer(client, attributes.x);
    client->y = inner_y_from_requested_outer(client, attributes.y);
    hints = XGetWMHints(wm->display, window);
    if (hints != NULL && (hints->flags & StateHint) &&
        hints->initial_state == IconicState)
        initially_iconic = true;
    if (startup && read_wm_state(wm, window) == IconicState)
        initially_iconic = true;
    if (hints != NULL)
        XFree(hints);
    saved_layout = restore_client_placement(wm, client,
                                            &saved_layout_monitor);
    saved_layout_before_maximize = client->layout_before_maximize;
    keep_client_on_screen(wm, client);

    frame_attributes.override_redirect = False;
    frame_attributes.background_pixel = wm->theme.silver;
    frame_attributes.event_mask = ExposureMask | ButtonPressMask |
                                  ButtonReleaseMask | PointerMotionMask |
                                  SubstructureRedirectMask;
    frame_attributes.cursor = wm->arrow_cursor;
    client->frame = XCreateWindow(wm->display, wm->root,
                                  client->x - FRAME_LEFT,
                                  client->y - FRAME_TOP,
                                  (unsigned)frame_width(client),
                                  (unsigned)frame_height(client), 0,
                                  CopyFromParent, InputOutput, CopyFromParent,
                                  CWOverrideRedirect | CWBackPixel | CWEventMask |
                                  CWCursor, &frame_attributes);
    set_internal_role(wm, client->frame, "client-frame");
    overlay_attributes.event_mask = ButtonPressMask;
    overlay_attributes.cursor = wm->arrow_cursor;
    client->focus_overlay = XCreateWindow(
        wm->display, client->frame, FRAME_LEFT, FRAME_TOP,
        (unsigned)client->width, (unsigned)client->height, 0, 0, InputOnly,
        CopyFromParent, CWEventMask | CWCursor, &overlay_attributes);
    set_internal_role(wm, client->focus_overlay, "focus-overlay");
    XSelectInput(wm->display, window,
                 PropertyChangeMask | StructureNotifyMask | FocusChangeMask);
    XGrabButton(wm->display, AnyButton, AnyModifier, client->focus_overlay, False,
                ButtonPressMask, GrabModeSync, GrabModeAsync, None, None);
    XAddToSaveSet(wm->display, window);
    XSetWindowBorderWidth(wm->display, window, 0);
    if (attributes.map_state != IsUnmapped) {
        ++client->expected_unmaps;
        XUnmapWindow(wm->display, window);
    }
    XReparentWindow(wm->display, window, client->frame, FRAME_LEFT, FRAME_TOP);
    XResizeWindow(wm->display, window, (unsigned)client->width,
                  (unsigned)client->height);
    client->next = wm->clients;
    wm->clients = client;
    refresh_client_transient_for(wm, client);
    if (client->transient_for != None)
        client->state_persistable = false;
    set_frame_extents(wm, client);
    update_client_list(wm);
    refresh_client_icon(wm, client);
    /* An explicit initial maximize wins the saved mode, while the saved mode
     * and monitor remain the target restored by the maximize button. */
    if (initially_maximized) {
        if (client->state_restored) {
            set_monitor_anchor(&client->layout_monitor,
                               monitor_at(wm, saved_layout_monitor));
        }
        set_client_layout(wm, client, CLIENT_LAYOUT_MAXIMIZED);
        if (client->state_restored) {
            client->layout_before_maximize =
                saved_layout == CLIENT_LAYOUT_MAXIMIZED
                    ? saved_layout_before_maximize
                    : saved_layout;
        }
    } else if (saved_layout != CLIENT_LAYOUT_NORMAL) {
        set_monitor_anchor(&client->layout_monitor,
                           monitor_at(wm, saved_layout_monitor));
        set_client_layout(wm, client, saved_layout);
        if (saved_layout == CLIENT_LAYOUT_MAXIMIZED)
            client->layout_before_maximize = saved_layout_before_maximize;
    }
    if (!client->state_restored)
        capture_client_placement(wm, client, false);

    if (initially_iconic) {
        client->minimized = true;
        client->icon = create_desktop_icon(wm, ICON_MINIMIZED, client);
        set_wm_state(wm, client, IconicState);
        set_hidden_state(wm, client, true);
    } else {
        set_wm_state(wm, client, NormalState);
        set_hidden_state(wm, client, false);
        XMapWindow(wm->display, window);
        XMapRaised(wm->display, client->focus_overlay);
        XMapWindow(wm->display, client->frame);
        if (!initially_maximized)
            send_configure_notify(wm, client);
        focus_client(wm, client, CurrentTime);
    }
    if (wm->task_manager.visible) {
        task_manager_select_first_application(wm);
        draw_task_manager(wm);
    }
    return client;
}

static int launcher_columns(WindowManager *wm)
{
    int columns = (wm->launcher_width - 20) / LAUNCHER_CELL_WIDTH;
    return columns < 1 ? 1 : columns;
}

static int launcher_visible_rows(WindowManager *wm)
{
    int rows = (wm->launcher_height - 82) / LAUNCHER_CELL_HEIGHT;
    return rows < 1 ? 1 : rows;
}

static int launcher_total_rows(WindowManager *wm)
{
    int columns = launcher_columns(wm);
    return ((int)wm->applications.len + columns - 1) / columns;
}

static void clamp_launcher_scroll(WindowManager *wm)
{
    int maximum = launcher_total_rows(wm) - launcher_visible_rows(wm);

    if (maximum < 0)
        maximum = 0;
    if (wm->launcher_scroll_row < 0)
        wm->launcher_scroll_row = 0;
    if (wm->launcher_scroll_row > maximum)
        wm->launcher_scroll_row = maximum;
}

static IconCategory application_icon_category(const AppEntry *entry)
{
    IconCategory category;

    if (entry == NULL)
        return ICON_CATEGORY_EXECUTABLE;
    category = icon_assets_classify(NULL, NULL, entry->categories);
    if (category == ICON_CATEGORY_EXECUTABLE)
        category = icon_assets_classify(entry->name, entry->icon, entry->exec);
    if (category == ICON_CATEGORY_EXECUTABLE && entry->terminal)
        category = ICON_CATEGORY_TERMINAL;
    return category;
}

static void free_application_icons(WindowManager *wm)
{
    size_t index;

    for (index = 0U; index < wm->application_icon_count; ++index)
        free_rendered_icon(wm, &wm->application_icons[index].rendered);
    free(wm->application_icons);
    wm->application_icons = NULL;
    wm->application_icon_count = 0U;
}

static const RenderedIcon *application_rendered_icon(WindowManager *wm,
                                                     size_t index)
{
    ApplicationRenderedIcon *cached;
    const AppEntry *entry;
    char *path = NULL;
    IconImage source = {0};
    IconImage scaled = {0};

    if (index >= wm->applications.len ||
        index >= wm->application_icon_count || wm->application_icons == NULL)
        return NULL;
    cached = &wm->application_icons[index];
    if (cached->state == APPLICATION_ICON_READY)
        return &cached->rendered;
    if (cached->state == APPLICATION_ICON_FAILED)
        return NULL;
    cached->state = APPLICATION_ICON_FAILED;
    entry = &wm->applications.entries[index];
    if (entry->icon == NULL || entry->icon[0] == '\0' ||
        app_icon_resolve(entry->icon, 48U, &path) < 0)
        goto done;
    if (icon_image_load_file(path, &source) < 0 ||
        icon_image_scale_fit(&source, 48U, 48U, &scaled) < 0 ||
        create_rendered_icon(wm, &scaled, &cached->rendered) < 0)
        goto done;
    cached->state = APPLICATION_ICON_READY;

done:
    free(path);
    icon_image_free(&scaled);
    icon_image_free(&source);
    return cached->state == APPLICATION_ICON_READY ? &cached->rendered : NULL;
}

static void draw_launcher(WindowManager *wm)
{
    int width = wm->launcher_width;
    int height = wm->launcher_height;
    int columns = launcher_columns(wm);
    int visible_rows = launcher_visible_rows(wm);
    int first = wm->launcher_scroll_row * columns;
    int last = first + columns * visible_rows;
    int index;
    char footer[128];

    XSetForeground(wm->display, wm->gc, wm->theme.silver);
    XFillRectangle(wm->display, wm->launcher, wm->gc, 0, 0,
                   (unsigned)width, (unsigned)height);
    draw_bevel(wm, wm->launcher, 0, 0, width, height, false);
    XSetForeground(
        wm->display, wm->gc,
        wm->internal_focus == INTERNAL_FOCUS_APPLICATIONS
            ? wm->theme.active_title
            : wm->theme.dark_gray);
    XFillRectangle(wm->display, wm->launcher, wm->gc, 3, 3,
                   (unsigned)(width - 6), TITLE_HEIGHT);
    draw_supplied_icon(wm, wm->launcher, ICON_CATEGORY_APPLICATIONS,
                       ICON_SIZE_SMALL, 7, 5);
    XSetForeground(wm->display, wm->gc, wm->theme.white);
    XDrawString(wm->display, wm->launcher, wm->gc, 28, 17,
                "Applications", 12);
    draw_title_button(
        wm, wm->launcher, internal_maximize_button_x(width), 4,
        wm->launcher_layout == CLIENT_LAYOUT_MAXIMIZED ? TITLE_GLYPH_RESTORE
                                                       : TITLE_GLYPH_MAXIMIZE);
    draw_title_button(wm, wm->launcher, internal_close_button_x(width), 4,
                      TITLE_GLYPH_CLOSE);

    XSetForeground(wm->display, wm->gc, wm->theme.white);
    XFillRectangle(wm->display, wm->launcher, wm->gc, 7, 29,
                   (unsigned)(width - 14), (unsigned)(height - 58));
    draw_bevel(wm, wm->launcher, 6, 28, width - 12, height - 56, true);
    if (last > (int)wm->applications.len)
        last = (int)wm->applications.len;
    for (index = first; index < last; ++index) {
        int relative = index - first;
        int column = relative % columns;
        int row = relative / columns;
        int cell_x = 10 + column * LAUNCHER_CELL_WIDTH;
        int cell_y = 34 + row * LAUNCHER_CELL_HEIGHT;
        const AppEntry *entry = &wm->applications.entries[index];
        const RenderedIcon *actual_icon =
            application_rendered_icon(wm, (size_t)index);

        if (index == wm->launcher_selected) {
            XSetForeground(wm->display, wm->gc, wm->theme.silver);
            XFillRectangle(wm->display, wm->launcher, wm->gc,
                           cell_x + 2, cell_y + 1,
                           LAUNCHER_CELL_WIDTH - 5, LAUNCHER_CELL_HEIGHT - 4);
            XSetForeground(wm->display, wm->gc, wm->theme.black);
            XDrawRectangle(wm->display, wm->launcher, wm->gc,
                           cell_x + 2, cell_y + 1,
                           LAUNCHER_CELL_WIDTH - 6, LAUNCHER_CELL_HEIGHT - 5);
        }
        if (actual_icon != NULL)
            draw_rendered_icon_centered(wm, wm->launcher, actual_icon,
                                        cell_x, cell_y + 5,
                                        LAUNCHER_CELL_WIDTH, 48);
        else
            draw_supplied_icon_centered(wm, wm->launcher,
                                        application_icon_category(entry),
                                        ICON_SIZE_LARGE, cell_x, cell_y + 5,
                                        LAUNCHER_CELL_WIDTH, 48);
        draw_centered_text(wm, wm->launcher,
                           entry->name,
                           cell_x + LAUNCHER_CELL_WIDTH / 2, cell_y + 68,
                           wm->theme.black, LAUNCHER_CELL_WIDTH - 10);
    }
    if (wm->applications.len == 0)
        draw_centered_text(wm, wm->launcher, "No applications were found",
                           width / 2, height / 2, wm->theme.dark_gray, width - 40);
    snprintf(footer, sizeof(footer),
             "%zu application%s  -  Double-click to open",
             wm->applications.len, wm->applications.len == 1 ? "" : "s");
    XSetForeground(wm->display, wm->gc, wm->theme.black);
    XDrawString(wm->display, wm->launcher, wm->gc, 9, height - 11,
                footer, (int)strlen(footer));
}

static void screen_layout_geometry(const MonitorGeometry *monitor,
                                   ClientLayout layout, int *x, int *y,
                                   int *width, int *height)
{
    *x = monitor->x;
    *y = monitor->y;
    *width = monitor->width;
    *height = monitor->height;
    if (layout == CLIENT_LAYOUT_SNAP_LEFT) {
        *width = monitor->width / 2;
    } else if (layout == CLIENT_LAYOUT_SNAP_RIGHT) {
        *x = monitor->x + monitor->width / 2;
        *width = monitor->width - monitor->width / 2;
    }
    if (*width < 1)
        *width = 1;
    if (*height < 1)
        *height = 1;
}

static void clamp_internal_geometry(const WindowManager *wm, int *x, int *y,
                                    int *width, int *height)
{
    const MonitorGeometry *monitor = monitor_at(
        wm, monitor_index_for_rectangle(wm, *x, *y, *width, *height));

    if (monitor != NULL)
        clamp_geometry_to_monitor(monitor, x, y, width, height);
}

static void restore_launcher_placement(WindowManager *wm)
{
    const Win31xDesktopPlacement *saved = &wm->desktop_state.launcher;
    size_t monitor_index;
    int x;
    int y;
    int width;
    int height;

    if (!saved->valid)
        return;
    monitor_index = geometry_from_placement(wm, saved, &x, &y, &width,
                                            &height, true);
    wm->launcher_x = x;
    wm->launcher_y = y;
    wm->launcher_width = width;
    wm->launcher_height = height;
    wm->launcher_layout = (ClientLayout)saved->layout;
    wm->launcher_layout_before_maximize =
        (ClientLayout)saved->layout_before_maximize;
    if (wm->launcher_layout != CLIENT_LAYOUT_NORMAL) {
        wm->launcher_restore_x = x;
        wm->launcher_restore_y = y;
        wm->launcher_restore_width = width;
        wm->launcher_restore_height = height;
        wm->launcher_restore_valid = true;
        set_monitor_anchor(&wm->launcher_layout_monitor,
                           monitor_at(wm, monitor_index));
        screen_layout_geometry(monitor_at(wm, monitor_index),
                               wm->launcher_layout, &wm->launcher_x,
                               &wm->launcher_y, &wm->launcher_width,
                               &wm->launcher_height);
    }
    wm->launcher_positioned = true;
}

static void remember_launcher_placement(WindowManager *wm)
{
    size_t monitor_index;
    int x = wm->launcher_x;
    int y = wm->launcher_y;
    int width = wm->launcher_width;
    int height = wm->launcher_height;

    if (wm->launcher_layout != CLIENT_LAYOUT_NORMAL &&
        wm->launcher_restore_valid) {
        x = wm->launcher_restore_x;
        y = wm->launcher_restore_y;
        width = wm->launcher_restore_width;
        height = wm->launcher_restore_height;
    }
    monitor_index = wm->launcher_layout != CLIENT_LAYOUT_NORMAL &&
                            wm->launcher_layout_monitor.valid
                        ? monitor_index_for_anchor(
                              wm, &wm->launcher_layout_monitor)
                        : monitor_index_for_rectangle(wm, x, y, width,
                                                      height);
    placement_from_geometry(wm, &wm->desktop_state.launcher, monitor_index,
                            x, y, width, height, wm->launcher_layout);
    if (wm->desktop_state.launcher.valid) {
        wm->desktop_state.launcher.layout_before_maximize =
            (Win31xDesktopLayout)wm->launcher_layout_before_maximize;
        mark_desktop_state_dirty(wm);
    }
}

static void restore_control_panel_placement(WindowManager *wm)
{
    ControlPanel *panel = &wm->control_panel;
    const Win31xDesktopPlacement *saved = &wm->desktop_state.control_panel;
    size_t monitor_index;
    int x;
    int y;
    int width;
    int height;

    if (!saved->valid)
        return;
    monitor_index = geometry_from_placement(wm, saved, &x, &y, &width,
                                            &height, true);
    panel->x = x;
    panel->y = y;
    panel->width = width;
    panel->height = height;
    panel->layout = (ClientLayout)saved->layout;
    panel->layout_before_maximize =
        (ClientLayout)saved->layout_before_maximize;
    if (panel->layout != CLIENT_LAYOUT_NORMAL) {
        panel->restore_x = x;
        panel->restore_y = y;
        panel->restore_width = width;
        panel->restore_height = height;
        panel->restore_valid = true;
        set_monitor_anchor(&panel->layout_monitor,
                           monitor_at(wm, monitor_index));
        screen_layout_geometry(monitor_at(wm, monitor_index), panel->layout,
                               &panel->x, &panel->y, &panel->width,
                               &panel->height);
    }
    wm->control_panel_positioned = true;
}

static void remember_control_panel_placement(WindowManager *wm)
{
    ControlPanel *panel = &wm->control_panel;
    size_t monitor_index;
    int x = panel->x;
    int y = panel->y;
    int width = panel->width;
    int height = panel->height;

    if (panel->layout != CLIENT_LAYOUT_NORMAL && panel->restore_valid) {
        x = panel->restore_x;
        y = panel->restore_y;
        width = panel->restore_width;
        height = panel->restore_height;
    }
    monitor_index = panel->layout != CLIENT_LAYOUT_NORMAL &&
                            panel->layout_monitor.valid
                        ? monitor_index_for_anchor(wm,
                                                   &panel->layout_monitor)
                        : monitor_index_for_rectangle(wm, x, y, width,
                                                      height);
    placement_from_geometry(wm, &wm->desktop_state.control_panel,
                            monitor_index, x, y, width, height, panel->layout);
    if (wm->desktop_state.control_panel.valid) {
        wm->desktop_state.control_panel.layout_before_maximize =
            (Win31xDesktopLayout)panel->layout_before_maximize;
        mark_desktop_state_dirty(wm);
    }
}

static void apply_launcher_layout(WindowManager *wm, ClientLayout layout)
{
    if (layout == CLIENT_LAYOUT_NORMAL &&
        wm->launcher_layout == CLIENT_LAYOUT_MAXIMIZED &&
        wm->launcher_layout_before_maximize != CLIENT_LAYOUT_NORMAL)
        layout = wm->launcher_layout_before_maximize;
    if (layout == CLIENT_LAYOUT_MAXIMIZED &&
        wm->launcher_layout != CLIENT_LAYOUT_MAXIMIZED)
        wm->launcher_layout_before_maximize = wm->launcher_layout;
    if (layout == CLIENT_LAYOUT_NORMAL) {
        if (wm->launcher_layout == CLIENT_LAYOUT_NORMAL)
            return;
        wm->launcher_layout = CLIENT_LAYOUT_NORMAL;
        if (wm->launcher_restore_valid) {
            wm->launcher_x = wm->launcher_restore_x;
            wm->launcher_y = wm->launcher_restore_y;
            wm->launcher_width = wm->launcher_restore_width;
            wm->launcher_height = wm->launcher_restore_height;
        }
        wm->launcher_restore_valid = false;
        wm->launcher_layout_monitor.valid = false;
        clamp_internal_geometry(wm, &wm->launcher_x, &wm->launcher_y,
                                &wm->launcher_width, &wm->launcher_height);
    } else {
        const MonitorGeometry *monitor;

        if (!wm->launcher_layout_monitor.valid) {
            set_monitor_anchor(
                &wm->launcher_layout_monitor,
                monitor_at(wm, monitor_index_for_rectangle(
                                   wm, wm->launcher_x, wm->launcher_y,
                                   wm->launcher_width,
                                   wm->launcher_height)));
        }
        if (wm->launcher_layout == CLIENT_LAYOUT_NORMAL) {
            wm->launcher_restore_x = wm->launcher_x;
            wm->launcher_restore_y = wm->launcher_y;
            wm->launcher_restore_width = wm->launcher_width;
            wm->launcher_restore_height = wm->launcher_height;
            wm->launcher_restore_valid = true;
        }
        wm->launcher_layout = layout;
        monitor = monitor_at(
            wm, monitor_index_for_anchor(wm,
                                         &wm->launcher_layout_monitor));
        set_monitor_anchor(&wm->launcher_layout_monitor, monitor);
        screen_layout_geometry(monitor, layout, &wm->launcher_x,
                               &wm->launcher_y, &wm->launcher_width,
                               &wm->launcher_height);
    }
    clamp_launcher_scroll(wm);
    XMoveResizeWindow(wm->display, wm->launcher, wm->launcher_x, wm->launcher_y,
                      (unsigned)wm->launcher_width,
                      (unsigned)wm->launcher_height);
    draw_launcher(wm);
}

static void toggle_launcher_maximize(WindowManager *wm)
{
    apply_launcher_layout(
        wm, wm->launcher_layout == CLIENT_LAYOUT_MAXIMIZED
                ? CLIENT_LAYOUT_NORMAL
                : CLIENT_LAYOUT_MAXIMIZED);
    remember_launcher_placement(wm);
}

static void position_launcher_on_monitor(WindowManager *wm,
                                         size_t monitor_index)
{
    const MonitorGeometry *monitor = monitor_at(wm, monitor_index);
    int available_width = monitor->width - 24;
    int available_height = monitor->height - 24;

    wm->launcher_layout = CLIENT_LAYOUT_NORMAL;
    wm->launcher_layout_before_maximize = CLIENT_LAYOUT_NORMAL;
    wm->launcher_restore_valid = false;
    wm->launcher_layout_monitor.valid = false;
    wm->launcher_width = available_width >= 260
                             ? (available_width < LAUNCHER_DEFAULT_WIDTH
                                    ? available_width
                                    : LAUNCHER_DEFAULT_WIDTH)
                             : monitor->width;
    wm->launcher_height = available_height >= 180
                              ? (available_height < LAUNCHER_DEFAULT_HEIGHT
                                     ? available_height
                                     : LAUNCHER_DEFAULT_HEIGHT)
                              : monitor->height;
    if (wm->launcher_width < 1)
        wm->launcher_width = 1;
    if (wm->launcher_height < 1)
        wm->launcher_height = 1;
    clamp_launcher_scroll(wm);
    wm->launcher_x = monitor->x +
                     (monitor->width - wm->launcher_width) / 2;
    wm->launcher_y = monitor->y +
                     (monitor->height - wm->launcher_height) / 2;
    wm->launcher_positioned = true;
    XMoveResizeWindow(wm->display, wm->launcher, wm->launcher_x, wm->launcher_y,
                      (unsigned)wm->launcher_width,
                      (unsigned)wm->launcher_height);
}

static void dismiss_launcher(WindowManager *wm)
{
    if (!wm->launcher_visible)
        return;
    wm->launcher_visible = false;
    if (wm->internal_focus == INTERNAL_FOCUS_APPLICATIONS)
        wm->internal_focus = INTERNAL_FOCUS_NONE;
    if (wm->drag.kind == DRAG_MOVE_LAUNCHER)
        cancel_drag(wm, CurrentTime);
    XUnmapWindow(wm->display, wm->launcher);
    update_focus_overlays(wm);
}

static void focus_after_internal_close(WindowManager *wm, bool was_focused,
                                       Time time)
{
    Client *next;

    if (!was_focused)
        return;
    /* Internal windows that track an explicit return target restore it before
     * reaching this fallback.  For Launcher and Control Panel, the managed
     * client that was active before they took focus is the closest equivalent
     * to the next window in the real root stacking order.  Do not let an
     * unrelated, visible internal window jump above that client merely because
     * it is still mapped in the background. */
    if (wm->active != NULL && !wm->active->minimized) {
        focus_client(wm, wm->active, time);
        return;
    }
    if (wm->run_dialog.visible) {
        activate_internal_window(wm, INTERNAL_FOCUS_RUN, time);
        return;
    }
    if (wm->task_manager.visible) {
        activate_internal_window(wm, INTERNAL_FOCUS_TASK_MANAGER, time);
        return;
    }
    if (wm->launcher_visible) {
        activate_internal_window(wm, INTERNAL_FOCUS_APPLICATIONS, time);
        return;
    }
    if (wm->control_panel.visible) {
        activate_internal_window(wm, INTERNAL_FOCUS_CONTROL_PANEL, time);
        return;
    }
    wm->internal_focus = INTERNAL_FOCUS_NONE;
    next = next_visible_client(wm, NULL);
    if (next != NULL)
        focus_client(wm, next, time);
    else
        XSetInputFocus(wm->display, wm->root, RevertToPointerRoot, time);
}

static void hide_launcher(WindowManager *wm)
{
    bool was_focused;

    if (!wm->launcher_visible)
        return;
    was_focused = wm->internal_focus == INTERNAL_FOCUS_APPLICATIONS;
    dismiss_launcher(wm);
    focus_after_internal_close(wm, was_focused, CurrentTime);
}

static void show_launcher_on_monitor(WindowManager *wm, size_t monitor_index)
{
    if (wm->launcher_visible) {
        activate_internal_window(wm, INTERNAL_FOCUS_APPLICATIONS, CurrentTime);
        return;
    }
    free_application_icons(wm);
    apps_free(&wm->applications);
    if (apps_load(&wm->applications) < 0)
        fprintf(stderr, "win31x: could not load application entries: %s\n",
                strerror(errno));
    if (wm->applications.len > 0U) {
        wm->application_icons =
            calloc(wm->applications.len, sizeof(*wm->application_icons));
        if (wm->application_icons != NULL)
            wm->application_icon_count = wm->applications.len;
    }
    wm->launcher_scroll_row = 0;
    wm->launcher_selected = wm->applications.len == 0 ? -1 : 0;
    wm->launcher_last_click = -1;
    wm->launcher_visible = true;
    if (!wm->launcher_positioned) {
        position_launcher_on_monitor(wm, monitor_index);
        remember_launcher_placement(wm);
    }
    XMapWindow(wm->display, wm->launcher);
    activate_internal_window(wm, INTERNAL_FOCUS_APPLICATIONS, CurrentTime);
}

static void show_launcher(WindowManager *wm)
{
    show_launcher_on_monitor(wm, active_monitor_index(wm));
}

static void launch_selected_application(WindowManager *wm)
{
    if (wm->launcher_selected < 0 ||
        wm->launcher_selected >= (int)wm->applications.len)
        return;
    if (app_launch(&wm->applications.entries[wm->launcher_selected]) < 0)
        fprintf(stderr, "win31x: could not launch %s: %s\n",
                wm->applications.entries[wm->launcher_selected].name,
                strerror(errno));
}

static void initialize_launcher(WindowManager *wm)
{
    XSetWindowAttributes attributes;

    wm->launcher_width = LAUNCHER_DEFAULT_WIDTH;
    wm->launcher_height = LAUNCHER_DEFAULT_HEIGHT;
    wm->launcher_selected = -1;
    wm->launcher_last_click = -1;
    restore_launcher_placement(wm);
    attributes.override_redirect = True;
    attributes.background_pixel = wm->theme.silver;
    attributes.event_mask = ExposureMask | ButtonPressMask | ButtonReleaseMask |
                            PointerMotionMask | KeyPressMask;
    attributes.cursor = wm->arrow_cursor;
    wm->launcher = XCreateWindow(wm->display, wm->root, wm->launcher_x,
                                 wm->launcher_y,
                                 (unsigned)wm->launcher_width,
                                 (unsigned)wm->launcher_height, 0,
                                 CopyFromParent, InputOutput, CopyFromParent,
                                 CWOverrideRedirect | CWBackPixel | CWEventMask |
                                 CWCursor, &attributes);
    set_internal_role(wm, wm->launcher, "applications-window");
    set_utf8_property(wm, wm->launcher, wm->atoms.net_wm_name, "Applications");
}

static int control_content_x(void)
{
    return CONTROL_PANEL_NAV_WIDTH + 14;
}

static int control_content_width(const ControlPanel *panel)
{
    int width = panel->width - control_content_x() - 7;

    return width > 0 ? width : 0;
}

static int control_nav_item_height(const ControlPanel *panel)
{
    int available = panel->height - CONTROL_PANEL_NAV_TOP - 7 -
                    (CONTROL_SECTION_COUNT - 1) * CONTROL_PANEL_NAV_GAP;
    int height = available / CONTROL_SECTION_COUNT;

    if (height > CONTROL_PANEL_NAV_ITEM_HEIGHT)
        height = CONTROL_PANEL_NAV_ITEM_HEIGHT;
    return height > 0 ? height : 1;
}

static int control_nav_item_y(const ControlPanel *panel, int section)
{
    return CONTROL_PANEL_NAV_TOP +
           section * (control_nav_item_height(panel) + CONTROL_PANEL_NAV_GAP);
}

static bool control_wifi_layout_available(const ControlPanel *panel)
{
    return control_content_width(panel) >= 240 && panel->height >= 260;
}

static int control_color_row_pitch(const ControlPanel *panel)
{
    int available = panel->height - 116;
    int pitch = available / (int)WIN31X_COLOR_SCHEME_COUNT;

    if (pitch > 58)
        pitch = 58;
    return pitch > 0 ? pitch : 1;
}

static int control_color_row_height(const ControlPanel *panel)
{
    int pitch = control_color_row_pitch(panel);
    int height = pitch >= 49 ? 46 : pitch - 3;

    return height > 1 ? height : 2;
}

static int control_color_row_y(const ControlPanel *panel, size_t index)
{
    return 92 + (int)index * control_color_row_pitch(panel);
}

static void control_auto_layout(const ControlPanel *panel,
                                ControlAutoLayout *layout)
{
    int content_x = control_content_x();
    int inner_width = control_content_width(panel) - 30;
    int horizontal_gap = 3;

    memset(layout, 0, sizeof(*layout));
    if (inner_width < 0)
        inner_width = 0;
    layout->compact = panel->height < 360;
    layout->toggle_x = content_x + 15;
    layout->lock_x = content_x + 15;
    layout->toggle_width = inner_width < 170 ? inner_width : 170;
    layout->lock_width = layout->toggle_width;
    if (!layout->compact) {
        layout->toggle_y = 125;
        layout->toggle_height = 36;
        layout->timeout_y = 207;
        layout->timeout_height = 34;
        layout->lock_y = 278;
        layout->lock_height = 38;
    } else {
        int available = panel->height - 96;
        int row_height = available / 3;

        if (row_height > 38)
            row_height = 38;
        if (row_height < 2)
            row_height = 2;
        layout->toggle_y = 78;
        layout->toggle_height = row_height;
        layout->timeout_y = layout->toggle_y + row_height + 5;
        layout->timeout_height = row_height;
        layout->lock_y = layout->timeout_y + row_height + 5;
        layout->lock_height = row_height;
    }
    layout->timeout_minus_x = content_x + 15;
    if (inner_width >= 253) {
        layout->timeout_button_width = 42;
        layout->timeout_value_x = content_x + 63;
        layout->timeout_value_width = 142;
        layout->timeout_plus_x = content_x + 211;
    } else {
        int button_width = inner_width / 4;
        int value_width;

        if (button_width > 32)
            button_width = 32;
        if (button_width < 1)
            button_width = 1;
        value_width = inner_width - button_width * 2 - horizontal_gap * 2;
        if (value_width < 1) {
            horizontal_gap = 0;
            button_width = inner_width / 3;
            if (button_width < 1)
                button_width = 1;
            value_width = inner_width - button_width * 2;
        }
        if (value_width < 0)
            value_width = 0;
        layout->timeout_button_width = button_width;
        layout->timeout_value_x = layout->timeout_minus_x + button_width +
                                  horizontal_gap;
        layout->timeout_value_width = value_width;
        layout->timeout_plus_x = layout->timeout_value_x + value_width +
                                 horizontal_gap;
    }
}

static void draw_control_navigation(WindowManager *wm)
{
    static const char *const labels[CONTROL_SECTION_COUNT] = {
        "Wi-Fi", "Colors", "Auto Lock"
    };
    static const IconCategory categories[CONTROL_SECTION_COUNT] = {
        ICON_CATEGORY_NETWORK, ICON_CATEGORY_GRAPHICS, ICON_CATEGORY_CLOCK
    };
    ControlPanel *panel = &wm->control_panel;
    int item_height = control_nav_item_height(panel);
    int section;

    for (section = 0; section < CONTROL_SECTION_COUNT; ++section) {
        int y = control_nav_item_y(panel, section);
        bool selected = panel->section == (ControlSection)section;

        XSetForeground(wm->display, wm->gc, wm->theme.silver);
        XFillRectangle(wm->display, panel->window, wm->gc, 7, y,
                       CONTROL_PANEL_NAV_WIDTH - 7,
                       (unsigned)item_height);
        draw_bevel(wm, panel->window, 7, y, CONTROL_PANEL_NAV_WIDTH - 7,
                   item_height, selected);
        if (item_height >= 70) {
            draw_supplied_icon_centered(
                wm, panel->window, categories[section], ICON_SIZE_LARGE, 11,
                y + 5, CONTROL_PANEL_NAV_WIDTH - 15, 50);
            draw_centered_text(wm, panel->window, labels[section],
                               CONTROL_PANEL_NAV_WIDTH / 2 + 3,
                               y + item_height - 16, wm->theme.black,
                               CONTROL_PANEL_NAV_WIDTH - 18);
        } else if (item_height >= 20) {
            draw_supplied_icon(wm, panel->window, categories[section],
                               ICON_SIZE_SMALL, 13,
                               y + (item_height - 16) / 2);
            draw_centered_text(wm, panel->window, labels[section], 91,
                               y + item_height / 2 + 5, wm->theme.black,
                               CONTROL_PANEL_NAV_WIDTH - 48);
        }
    }
}

static int wifi_list_y(void)
{
    return 90;
}

static int wifi_action_y(const ControlPanel *panel)
{
    return panel->height - 58;
}

static int wifi_password_y(const ControlPanel *panel)
{
    return wifi_action_y(panel) - 48;
}

static int wifi_visible_rows(const ControlPanel *panel)
{
    int height = wifi_password_y(panel) - wifi_list_y() - 23;
    int rows = height / 31;

    return rows > 0 ? rows : 1;
}

static void clamp_wifi_selection(WindowManager *wm)
{
    ControlPanel *panel = &wm->control_panel;
    int count = (int)wifi_backend_network_count(&wm->wifi);
    int rows = wifi_visible_rows(panel);
    int maximum_scroll = count - rows;

    if (maximum_scroll < 0)
        maximum_scroll = 0;
    if (panel->wifi_scroll < 0)
        panel->wifi_scroll = 0;
    if (panel->wifi_scroll > maximum_scroll)
        panel->wifi_scroll = maximum_scroll;
    if (panel->wifi_selected >= count)
        panel->wifi_selected = -1;
    if (panel->wifi_selected >= 0) {
        if (panel->wifi_selected < panel->wifi_scroll)
            panel->wifi_scroll = panel->wifi_selected;
        if (panel->wifi_selected >= panel->wifi_scroll + rows)
            panel->wifi_scroll = panel->wifi_selected - rows + 1;
    }
}

static const WifiNetwork *selected_wifi_network(WindowManager *wm)
{
    if (wm->control_panel.wifi_selected < 0)
        return NULL;
    return wifi_backend_network_at(
        &wm->wifi, (size_t)wm->control_panel.wifi_selected);
}

static bool wifi_network_needs_password(const WifiNetwork *network)
{
    return network != NULL &&
           (network->security == WIFI_SECURITY_WPA_PSK ||
            network->security == WIFI_SECURITY_SAE);
}

static void draw_control_wifi(WindowManager *wm)
{
    ControlPanel *panel = &wm->control_panel;
    int content_x = control_content_x();
    int content_width = control_content_width(panel);
    int list_y = wifi_list_y();
    int password_y = wifi_password_y(panel);
    int action_y = wifi_action_y(panel);
    int list_width = content_width - 30;
    int rows = wifi_visible_rows(panel);
    int list_height = rows * 31 + 2;
    int row;
    char text[256];
    char masked[CONTROL_PANEL_PASSWORD_CAPACITY];
    const WifiNetwork *selected;
    bool busy = wifi_backend_busy(&wm->wifi);

    if (!control_wifi_layout_available(panel)) {
        draw_text(wm, panel->window, content_x + 8, 57, wm->theme.black,
                  "Wi-Fi");
        fitted_text(wm, "Enlarge the screen to view networks.",
                    content_width - 16, text, sizeof(text));
        draw_text(wm, panel->window, content_x + 8, 80,
                  wm->theme.dark_gray, text);
        return;
    }
    clamp_wifi_selection(wm);
    selected = selected_wifi_network(wm);
    draw_text(wm, panel->window, content_x + 15, 57, wm->theme.black,
              "Wi-Fi Networks");
    draw_button(wm, panel->window, content_x + content_width - 100, 39,
                82, 30, busy ? "Working..." : "Refresh", false, !busy);
    fitted_text(wm, wifi_backend_status_text(&wm->wifi), content_width - 125,
                text, sizeof(text));
    draw_text(wm, panel->window, content_x + 15, 79,
              wifi_backend_status(&wm->wifi) == WIFI_STATUS_FAILED ||
                      wifi_backend_status(&wm->wifi) == WIFI_STATUS_UNAVAILABLE ||
                      wifi_backend_status(&wm->wifi) == WIFI_STATUS_INVALID_DATA
                  ? wm->theme.black
                  : wm->theme.dark_gray,
              text);

    if (list_width > 20 && list_height > 2) {
        XSetForeground(wm->display, wm->gc, wm->theme.white);
        XFillRectangle(wm->display, panel->window, wm->gc, content_x + 15,
                       list_y, (unsigned)list_width, (unsigned)list_height);
        draw_bevel(wm, panel->window, content_x + 14, list_y - 1,
                   list_width + 2, list_height + 2, true);
    }
    for (row = 0; row < rows; ++row) {
        int index = panel->wifi_scroll + row;
        const WifiNetwork *network = wifi_backend_network_at(
            &wm->wifi, (size_t)index);
        int y = list_y + 2 + row * 31;
        char signal_text[24];
        char name[160];
        char security[64];

        if (network == NULL)
            break;
        if (index == panel->wifi_selected) {
            XSetForeground(wm->display, wm->gc, wm->theme.silver);
            XFillRectangle(wm->display, panel->window, wm->gc,
                           content_x + 17, y, (unsigned)(list_width - 4), 29U);
            XSetForeground(wm->display, wm->gc, wm->theme.black);
            XDrawRectangle(wm->display, panel->window, wm->gc,
                           content_x + 17, y, (unsigned)(list_width - 5), 28U);
        }
        snprintf(name, sizeof(name), "%s%s", network->active ? "* " : "",
                 network->display_name);
        fitted_text(wm, name, list_width - 205, text, sizeof(text));
        draw_text(wm, panel->window, content_x + 24, y + 19,
                  wm->theme.black, text);
        snprintf(security, sizeof(security), "%s", network->security_name);
        fitted_text(wm, security, 104, text, sizeof(text));
        draw_text(wm, panel->window, content_x + list_width - 165, y + 19,
                  wifi_backend_network_supported(network)
                      ? wm->theme.dark_gray
                      : wm->theme.black,
                  text);
        snprintf(signal_text, sizeof(signal_text), "%u%%", network->signal);
        draw_text(wm, panel->window, content_x + list_width - 50, y + 19,
                  wm->theme.dark_gray, signal_text);
    }

    if (selected != NULL && !wifi_backend_network_supported(selected)) {
        draw_text(wm, panel->window, content_x + 15, password_y - 8,
                  wm->theme.black,
                  "This network needs enterprise or legacy Wi-Fi setup.");
    } else if (selected != NULL && wifi_network_needs_password(selected) &&
               !selected->active) {
        draw_text(wm, panel->window, content_x + 15, password_y - 8,
                  wm->theme.black, "Network password");
        XSetForeground(wm->display, wm->gc, wm->theme.white);
        XFillRectangle(wm->display, panel->window, wm->gc, content_x + 15,
                       password_y, (unsigned)(list_width - 2), 30U);
        draw_bevel(wm, panel->window, content_x + 14, password_y - 1,
                   list_width, 32, true);
        memset(masked, '*', panel->password_length);
        masked[panel->password_length] = '\0';
        fitted_text(wm, masked, list_width - 14, text, sizeof(text));
        draw_text(wm, panel->window, content_x + 22, password_y + 20,
                  wm->theme.black, text);
        if (panel->password_active) {
            int cursor_x = content_x + 22 +
                           XTextWidth(wm->font, text, (int)strlen(text));
            XDrawLine(wm->display, panel->window, wm->gc, cursor_x,
                      password_y + 7, cursor_x, password_y + 23);
        }
    } else {
        draw_text(wm, panel->window, content_x + 15, password_y + 20,
                  wm->theme.dark_gray,
                  selected == NULL ? "Select a network."
                                   : (selected->active
                                          ? "This network is connected."
                                          : "No password is required."));
    }

    draw_button(
        wm, panel->window, content_x + 15, action_y, 145, 34,
        selected != NULL && selected->active ? "Disconnect" : "Connect",
        false,
        selected != NULL && !busy &&
            (selected->active || wifi_backend_network_supported(selected)));
}

static void draw_control_colors(WindowManager *wm)
{
    ControlPanel *panel = &wm->control_panel;
    int content_x = control_content_x();
    int content_width = control_content_width(panel);
    size_t index;

    draw_text(wm, panel->window, content_x + 15, 57, wm->theme.black,
              "Color scheme");
    draw_text(wm, panel->window, content_x + 15, 78, wm->theme.dark_gray,
              "Choose desktop and active-title colors.");
    for (index = 0U; index < WIN31X_COLOR_SCHEME_COUNT; ++index) {
        const Win31xColorScheme *scheme = win31x_color_scheme(index);
        int y = control_color_row_y(panel, index);
        int row_height = control_color_row_height(panel);
        int row_width = content_width - 30;
        bool selected = wm->settings.color_scheme == index;

        if (row_width < 80 || scheme == NULL)
            continue;
        draw_button(wm, panel->window, content_x + 15, y, row_width,
                    row_height,
                    scheme->name, selected, true);
        if (row_height >= 32) {
            int sample_y = y + (row_height - 26) / 2;

            XSetForeground(wm->display, wm->gc,
                           wm->theme.scheme_desktop[index]);
            XFillRectangle(wm->display, panel->window, wm->gc,
                           content_x + row_width - 73, sample_y, 34U, 26U);
            XSetForeground(wm->display, wm->gc,
                           wm->theme.scheme_active_title[index]);
            XFillRectangle(wm->display, panel->window, wm->gc,
                           content_x + row_width - 37, sample_y, 34U, 26U);
            XSetForeground(wm->display, wm->gc, wm->theme.black);
            XDrawRectangle(wm->display, panel->window, wm->gc,
                           content_x + row_width - 73, sample_y, 68U, 26U);
        }
    }
    if (panel->settings_status[0] != '\0')
        draw_text(wm, panel->window, content_x + 15, panel->height - 18,
                  wm->theme.dark_gray, panel->settings_status);
}

static void draw_control_auto_lock(WindowManager *wm)
{
    ControlPanel *panel = &wm->control_panel;
    Win31xAutoLock *lock = &wm->auto_lock;
    int content_x = control_content_x();
    int content_width = control_content_width(panel);
    ControlAutoLayout layout;
    char timeout_label[64];
    char provider_text[256];
    char status[256];

    control_auto_layout(panel, &layout);
    draw_text(wm, panel->window, content_x + 15,
              layout.compact ? 48 : 57, wm->theme.black,
              "Auto Lock");
    snprintf(status, sizeof(status), "Screen locker: %s", lock->provider);
    fitted_text(wm, status, content_width - 30, provider_text,
                sizeof(provider_text));
    draw_text(wm, panel->window, content_x + 15,
              layout.compact ? 68 : 80,
              lock->locker_available ? wm->theme.dark_gray : wm->theme.black,
              provider_text);
    if (!layout.compact)
        draw_text(wm, panel->window, content_x + 15, 112, wm->theme.black,
                  "Lock the screen after inactivity");
    draw_button(wm, panel->window, layout.toggle_x, layout.toggle_y,
                layout.toggle_width, layout.toggle_height,
                wm->settings.auto_lock_enabled ? "Auto Lock: On"
                                               : "Auto Lock: Off",
                wm->settings.auto_lock_enabled, lock->available);

    if (!layout.compact)
        draw_text(wm, panel->window, content_x + 15, 194, wm->theme.black,
                  "Idle timeout");
    draw_button(wm, panel->window, layout.timeout_minus_x, layout.timeout_y,
                layout.timeout_button_width, layout.timeout_height, "-", false,
                lock->available);
    snprintf(timeout_label, sizeof(timeout_label), "%u minute%s",
             wm->settings.auto_lock_minutes,
             wm->settings.auto_lock_minutes == 1U ? "" : "s");
    draw_button(wm, panel->window, layout.timeout_value_x, layout.timeout_y,
                layout.timeout_value_width, layout.timeout_height,
                timeout_label, true, lock->available);
    draw_button(wm, panel->window, layout.timeout_plus_x, layout.timeout_y,
                layout.timeout_button_width, layout.timeout_height, "+", false,
                lock->available);

    draw_button(wm, panel->window, layout.lock_x, layout.lock_y,
                layout.lock_width, layout.lock_height,
                lock->direct_pid > 0 ? "Locking..." : "Lock Now", false,
                lock->locker_available && lock->direct_pid <= 0);
    if (!layout.compact && !lock->available) {
        draw_text(wm, panel->window, content_x + 15, 347, wm->theme.black,
                  "Install xss-lock and xsecurelock to enable auto lock.");
    }
    if (!layout.compact && content_width > 40) {
        fitted_text(wm, lock->status, content_width - 30, status,
                    sizeof(status));
        draw_text(wm, panel->window, content_x + 15, panel->height - 39,
                  wm->theme.dark_gray, status);
    }
    if (!layout.compact && panel->settings_status[0] != '\0')
        draw_text(wm, panel->window, content_x + 15, panel->height - 18,
                  wm->theme.dark_gray, panel->settings_status);
}

static void draw_control_panel(WindowManager *wm)
{
    ControlPanel *panel = &wm->control_panel;
    int content_x = control_content_x();
    int content_width = control_content_width(panel);

    XSetForeground(wm->display, wm->gc, wm->theme.silver);
    XFillRectangle(wm->display, panel->window, wm->gc, 0, 0,
                   (unsigned)panel->width, (unsigned)panel->height);
    draw_bevel(wm, panel->window, 0, 0, panel->width, panel->height, false);
    if (panel->width < 48 || panel->height < 30)
        return;
    XSetForeground(
        wm->display, wm->gc,
        wm->internal_focus == INTERNAL_FOCUS_CONTROL_PANEL
            ? wm->theme.active_title
            : wm->theme.dark_gray);
    XFillRectangle(wm->display, panel->window, wm->gc, 3, 3,
                   (unsigned)(panel->width - 6), TITLE_HEIGHT);
    draw_supplied_icon(wm, panel->window, ICON_CATEGORY_SETTINGS,
                       ICON_SIZE_SMALL, 7, 5);
    draw_text(wm, panel->window, 28, 17, wm->theme.white, "Control Panel");
    draw_title_button(
        wm, panel->window, internal_maximize_button_x(panel->width), 4,
        panel->layout == CLIENT_LAYOUT_MAXIMIZED ? TITLE_GLYPH_RESTORE
                                                 : TITLE_GLYPH_MAXIMIZE);
    draw_title_button(wm, panel->window,
                      internal_close_button_x(panel->width), 4,
                      TITLE_GLYPH_CLOSE);
    draw_control_navigation(wm);
    if (content_width > 0 && panel->height > 36) {
        XSetForeground(wm->display, wm->gc, wm->theme.white);
        XFillRectangle(wm->display, panel->window, wm->gc, content_x, 30,
                       (unsigned)content_width,
                       (unsigned)(panel->height - 37));
        draw_bevel(wm, panel->window, content_x - 1, 29,
                   content_width + 2, panel->height - 35, true);
        if (panel->section == CONTROL_SECTION_COLORS)
            draw_control_colors(wm);
        else if (panel->section == CONTROL_SECTION_WIFI)
            draw_control_wifi(wm);
        else
            draw_control_auto_lock(wm);
    }
}

static void apply_control_panel_layout(WindowManager *wm, ClientLayout layout)
{
    ControlPanel *panel = &wm->control_panel;

    if (layout == CLIENT_LAYOUT_NORMAL &&
        panel->layout == CLIENT_LAYOUT_MAXIMIZED &&
        panel->layout_before_maximize != CLIENT_LAYOUT_NORMAL)
        layout = panel->layout_before_maximize;
    if (layout == CLIENT_LAYOUT_MAXIMIZED &&
        panel->layout != CLIENT_LAYOUT_MAXIMIZED)
        panel->layout_before_maximize = panel->layout;
    if (layout == CLIENT_LAYOUT_NORMAL) {
        if (panel->layout == CLIENT_LAYOUT_NORMAL)
            return;
        panel->layout = CLIENT_LAYOUT_NORMAL;
        if (panel->restore_valid) {
            panel->x = panel->restore_x;
            panel->y = panel->restore_y;
            panel->width = panel->restore_width;
            panel->height = panel->restore_height;
        }
        panel->restore_valid = false;
        panel->layout_monitor.valid = false;
        clamp_internal_geometry(wm, &panel->x, &panel->y, &panel->width,
                                &panel->height);
    } else {
        const MonitorGeometry *monitor;

        if (!panel->layout_monitor.valid) {
            set_monitor_anchor(
                &panel->layout_monitor,
                monitor_at(wm, monitor_index_for_rectangle(
                                   wm, panel->x, panel->y, panel->width,
                                   panel->height)));
        }
        if (panel->layout == CLIENT_LAYOUT_NORMAL) {
            panel->restore_x = panel->x;
            panel->restore_y = panel->y;
            panel->restore_width = panel->width;
            panel->restore_height = panel->height;
            panel->restore_valid = true;
        }
        panel->layout = layout;
        monitor = monitor_at(
            wm, monitor_index_for_anchor(wm, &panel->layout_monitor));
        set_monitor_anchor(&panel->layout_monitor, monitor);
        screen_layout_geometry(monitor, layout, &panel->x, &panel->y,
                               &panel->width, &panel->height);
    }
    if (!control_wifi_layout_available(panel))
        clear_control_password(panel);
    XMoveResizeWindow(wm->display, panel->window, panel->x, panel->y,
                      (unsigned)panel->width, (unsigned)panel->height);
    draw_control_panel(wm);
}

static void toggle_control_panel_maximize(WindowManager *wm)
{
    apply_control_panel_layout(
        wm, wm->control_panel.layout == CLIENT_LAYOUT_MAXIMIZED
                ? CLIENT_LAYOUT_NORMAL
                : CLIENT_LAYOUT_MAXIMIZED);
    remember_control_panel_placement(wm);
}

static void position_control_panel_on_monitor(WindowManager *wm,
                                              size_t monitor_index)
{
    ControlPanel *panel = &wm->control_panel;
    const MonitorGeometry *monitor = monitor_at(wm, monitor_index);
    int available_width = monitor->width - 24;
    int available_height = monitor->height - 24;

    panel->layout = CLIENT_LAYOUT_NORMAL;
    panel->layout_before_maximize = CLIENT_LAYOUT_NORMAL;
    panel->restore_valid = false;
    panel->layout_monitor.valid = false;
    panel->width = available_width >= 320
                       ? (available_width < CONTROL_PANEL_DEFAULT_WIDTH
                              ? available_width
                              : CONTROL_PANEL_DEFAULT_WIDTH)
                       : monitor->width;
    panel->height = available_height >= 300
                        ? (available_height < CONTROL_PANEL_DEFAULT_HEIGHT
                               ? available_height
                               : CONTROL_PANEL_DEFAULT_HEIGHT)
                        : monitor->height;
    if (panel->width < 1)
        panel->width = 1;
    if (panel->height < 1)
        panel->height = 1;
    if (!control_wifi_layout_available(panel))
        clear_control_password(panel);
    panel->x = monitor->x + (monitor->width - panel->width) / 2;
    panel->y = monitor->y + (monitor->height - panel->height) / 2;
    wm->control_panel_positioned = true;
    XMoveResizeWindow(wm->display, panel->window, panel->x, panel->y,
                      (unsigned)panel->width, (unsigned)panel->height);
}

static void dismiss_control_panel(WindowManager *wm)
{
    ControlPanel *panel = &wm->control_panel;

    if (!panel->visible)
        return;
    panel->visible = false;
    clear_control_password(panel);
    if (wm->internal_focus == INTERNAL_FOCUS_CONTROL_PANEL)
        wm->internal_focus = INTERNAL_FOCUS_NONE;
    if (wm->drag.kind == DRAG_MOVE_CONTROL_PANEL)
        cancel_drag(wm, CurrentTime);
    XUnmapWindow(wm->display, panel->window);
    update_focus_overlays(wm);
}

static void hide_control_panel(WindowManager *wm)
{
    bool was_focused;

    if (!wm->control_panel.visible)
        return;
    was_focused = wm->internal_focus == INTERNAL_FOCUS_CONTROL_PANEL;
    dismiss_control_panel(wm);
    focus_after_internal_close(wm, was_focused, CurrentTime);
}

static void show_control_panel_on_monitor(WindowManager *wm,
                                          size_t monitor_index)
{
    ControlPanel *panel = &wm->control_panel;

    if (panel->visible) {
        activate_internal_window(wm, INTERNAL_FOCUS_CONTROL_PANEL, CurrentTime);
        return;
    }
    clear_control_password(panel);
    panel->visible = true;
    if (!wm->control_panel_positioned) {
        position_control_panel_on_monitor(wm, monitor_index);
        remember_control_panel_placement(wm);
    }
    XMapWindow(wm->display, panel->window);
    activate_internal_window(wm, INTERNAL_FOCUS_CONTROL_PANEL, CurrentTime);
    if (panel->section == CONTROL_SECTION_WIFI &&
        !wifi_backend_busy(&wm->wifi))
        (void)wifi_backend_start_scan(&wm->wifi);
    draw_control_panel(wm);
}

static void initialize_control_panel(WindowManager *wm)
{
    ControlPanel *panel = &wm->control_panel;
    XSetWindowAttributes attributes;

    panel->width = CONTROL_PANEL_DEFAULT_WIDTH;
    panel->height = CONTROL_PANEL_DEFAULT_HEIGHT;
    panel->section = (ControlSection)wm->settings.control_panel_section;
    panel->wifi_selected = -1;
    restore_control_panel_placement(wm);
    attributes.override_redirect = True;
    attributes.background_pixel = wm->theme.silver;
    attributes.event_mask = ExposureMask | ButtonPressMask | ButtonReleaseMask |
                            PointerMotionMask | KeyPressMask;
    attributes.cursor = wm->arrow_cursor;
    panel->window = XCreateWindow(
        wm->display, wm->root, panel->x, panel->y, (unsigned)panel->width,
        (unsigned)panel->height, 0, CopyFromParent, InputOutput, CopyFromParent,
        CWOverrideRedirect | CWBackPixel | CWEventMask | CWCursor, &attributes);
    set_internal_role(wm, panel->window, "control-panel-window");
    set_utf8_property(wm, panel->window, wm->atoms.net_wm_name,
                      "Control Panel");
}

static int run_button_y(const RunDialog *dialog)
{
    return dialog->height - 39;
}

static int run_cancel_button_x(const RunDialog *dialog)
{
    return dialog->width - 91;
}

static int run_open_button_x(const RunDialog *dialog)
{
    return run_cancel_button_x(dialog) - 86;
}

static void draw_run_dialog(WindowManager *wm)
{
    RunDialog *dialog = &wm->run_dialog;
    bool compact = dialog->width < 260 || dialog->height < 140;
    int edit_x = compact ? 8 : 78;
    int edit_y = compact ? 35 : 62;
    int edit_width = dialog->width - edit_x - (compact ? 8 : 16);
    int edit_height = dialog->height - edit_y - 4;
    const char *visible;
    int text_width;

    if (dialog->window == None)
        return;
    XSetForeground(wm->display, wm->gc, wm->theme.silver);
    XFillRectangle(wm->display, dialog->window, wm->gc, 0, 0,
                   (unsigned)dialog->width, (unsigned)dialog->height);
    draw_bevel(wm, dialog->window, 0, 0, dialog->width, dialog->height, false);
    if (dialog->width > 6) {
        XSetForeground(wm->display, wm->gc,
                       wm->internal_focus == INTERNAL_FOCUS_RUN
                           ? wm->theme.active_title
                           : wm->theme.dark_gray);
        XFillRectangle(wm->display, dialog->window, wm->gc, 3, 3,
                       (unsigned)(dialog->width - 6), TITLE_HEIGHT);
    }
    if (dialog->width >= 48) {
        draw_supplied_icon(wm, dialog->window, ICON_CATEGORY_EXECUTABLE,
                           ICON_SIZE_SMALL, 7, 5);
        draw_text(wm, dialog->window, 28, 17, wm->theme.white, "Run");
    }
    if (dialog->width >= TITLE_BUTTON + 12)
        draw_title_button(wm, dialog->window,
                          internal_close_button_x(dialog->width), 4,
                          TITLE_GLYPH_CLOSE);

    if (!compact) {
        draw_supplied_icon_centered(wm, dialog->window,
                                    ICON_CATEGORY_EXECUTABLE,
                                    ICON_SIZE_LARGE, 13, 43, 52, 52);
        draw_text(wm, dialog->window, edit_x, 49, wm->theme.black,
                  "Type the name of a program to open:");
    }
    if (edit_width < 1)
        edit_width = 1;
    if (edit_height > 25)
        edit_height = 25;
    if (edit_height < 3)
        return;
    XSetForeground(wm->display, wm->gc, wm->theme.white);
    XFillRectangle(wm->display, dialog->window, wm->gc, edit_x, edit_y,
                   (unsigned)edit_width, (unsigned)edit_height);
    draw_bevel(wm, dialog->window, edit_x - 1, edit_y - 1,
               edit_width + 2, edit_height + 2, true);
    visible = dialog->command;
    while (*visible != '\0' &&
           XTextWidth(wm->font, visible, (int)strlen(visible)) >
               edit_width - 10)
        ++visible;
    draw_text(wm, dialog->window, edit_x + 5,
              edit_y + (edit_height > 18 ? 17 : edit_height - 2),
              wm->theme.black, visible);
    if (wm->internal_focus == INTERNAL_FOCUS_RUN && edit_height > 6) {
        text_width = XTextWidth(wm->font, visible, (int)strlen(visible));
        XSetForeground(wm->display, wm->gc, wm->theme.black);
        XDrawLine(wm->display, dialog->window, wm->gc,
                  edit_x + 5 + text_width, edit_y + 3,
                  edit_x + 5 + text_width, edit_y + edit_height - 3);
    }
    if (!compact && dialog->status[0] != '\0')
        draw_text(wm, dialog->window, edit_x, 108, wm->theme.dark_gray,
                  dialog->status);
    if (!compact) {
        draw_button(wm, dialog->window, run_open_button_x(dialog),
                    run_button_y(dialog), 78, 26, "Run", false,
                    dialog->command_length > 0U);
        draw_button(wm, dialog->window, run_cancel_button_x(dialog),
                    run_button_y(dialog), 78, 26, "Cancel", false, true);
    }
}

static void position_run_dialog(WindowManager *wm)
{
    RunDialog *dialog = &wm->run_dialog;
    size_t monitor_index = dialog->monitor.valid
                               ? monitor_index_for_anchor(wm,
                                                          &dialog->monitor)
                               : active_monitor_index(wm);
    const MonitorGeometry *monitor = monitor_at(wm, monitor_index);

    set_monitor_anchor(&dialog->monitor, monitor);
    dialog->width = monitor->width - 24 < RUN_DIALOG_DEFAULT_WIDTH
                        ? monitor->width - 24
                        : RUN_DIALOG_DEFAULT_WIDTH;
    dialog->height = monitor->height - 24 < RUN_DIALOG_DEFAULT_HEIGHT
                         ? monitor->height - 24
                         : RUN_DIALOG_DEFAULT_HEIGHT;
    if (dialog->width < 260)
        dialog->width = monitor->width > 0 ? monitor->width : 1;
    if (dialog->height < 130)
        dialog->height = monitor->height > 0 ? monitor->height : 1;
    dialog->x = monitor->x + (monitor->width - dialog->width) / 2;
    dialog->y = monitor->y + (monitor->height - dialog->height) / 2;
    clamp_geometry_to_monitor(monitor, &dialog->x, &dialog->y,
                              &dialog->width, &dialog->height);
    dialog->positioned = true;
    XMoveResizeWindow(wm->display, dialog->window, dialog->x, dialog->y,
                      (unsigned)dialog->width, (unsigned)dialog->height);
}

static void restore_run_dialog_placement(WindowManager *wm)
{
    RunDialog *dialog = &wm->run_dialog;
    const Win31xDesktopPlacement *saved = &wm->desktop_state.run_dialog;
    size_t monitor_index;

    if (!saved->valid)
        return;
    monitor_index = geometry_from_placement(
        wm, saved, &dialog->x, &dialog->y, &dialog->width, &dialog->height,
        true);
    set_monitor_anchor(&dialog->monitor, monitor_at(wm, monitor_index));
    dialog->positioned = true;
}

static void remember_run_dialog_placement(WindowManager *wm)
{
    RunDialog *dialog = &wm->run_dialog;
    size_t monitor_index;

    if (!dialog->positioned)
        return;
    monitor_index = monitor_index_for_rectangle(
        wm, dialog->x, dialog->y, dialog->width, dialog->height);
    placement_from_geometry(wm, &wm->desktop_state.run_dialog,
                            monitor_index, dialog->x, dialog->y,
                            dialog->width, dialog->height,
                            CLIENT_LAYOUT_NORMAL);
    if (wm->desktop_state.run_dialog.valid)
        mark_desktop_state_dirty(wm);
}

static void dismiss_run_dialog(WindowManager *wm)
{
    RunDialog *dialog = &wm->run_dialog;

    if (!dialog->visible)
        return;
    dialog->visible = false;
    if (wm->internal_focus == INTERNAL_FOCUS_RUN)
        wm->internal_focus = INTERNAL_FOCUS_NONE;
    if (wm->drag.kind == DRAG_MOVE_RUN)
        cancel_drag(wm, CurrentTime);
    XUnmapWindow(wm->display, dialog->window);
    update_focus_overlays(wm);
}

static void remember_run_return_focus(WindowManager *wm)
{
    RunDialog *dialog = &wm->run_dialog;

    if (wm->internal_focus == INTERNAL_FOCUS_RUN)
        return;
    dialog->return_internal_focus = wm->internal_focus;
    dialog->return_client =
        wm->internal_focus == INTERNAL_FOCUS_NONE && wm->active != NULL
            ? wm->active->window
            : None;
}

static void restore_run_return_focus(WindowManager *wm, bool was_focused,
                                     Time time)
{
    RunDialog *dialog = &wm->run_dialog;
    InternalFocus internal = dialog->return_internal_focus;
    Window client_window = dialog->return_client;
    Client *client = client_window != None
                         ? client_for_client_window(wm, client_window)
                         : NULL;

    dialog->return_internal_focus = INTERNAL_FOCUS_NONE;
    dialog->return_client = None;
    if (!was_focused)
        return;
    if (internal == INTERNAL_FOCUS_APPLICATIONS && wm->launcher_visible) {
        activate_internal_window(wm, INTERNAL_FOCUS_APPLICATIONS, time);
        return;
    }
    if (internal == INTERNAL_FOCUS_CONTROL_PANEL &&
        wm->control_panel.visible) {
        activate_internal_window(wm, INTERNAL_FOCUS_CONTROL_PANEL, time);
        return;
    }
    if (internal == INTERNAL_FOCUS_TASK_MANAGER &&
        wm->task_manager.visible) {
        activate_internal_window(wm, INTERNAL_FOCUS_TASK_MANAGER, time);
        return;
    }
    if (client != NULL && !client->minimized) {
        focus_client(wm, client, time);
        return;
    }
    focus_after_internal_close(wm, true, time);
}

static void hide_run_dialog(WindowManager *wm)
{
    bool was_focused;

    if (!wm->run_dialog.visible)
        return;
    was_focused = wm->internal_focus == INTERNAL_FOCUS_RUN;
    dismiss_run_dialog(wm);
    restore_run_return_focus(wm, was_focused, CurrentTime);
}

static void show_run_dialog(WindowManager *wm)
{
    RunDialog *dialog = &wm->run_dialog;

    remember_run_return_focus(wm);
    if (dialog->visible) {
        activate_internal_window(wm, INTERNAL_FOCUS_RUN, CurrentTime);
        return;
    }
    dialog->status[0] = '\0';
    dialog->visible = true;
    if (!dialog->positioned) {
        set_monitor_anchor(&dialog->monitor,
                           monitor_at(wm, active_monitor_index(wm)));
        position_run_dialog(wm);
        remember_run_dialog_placement(wm);
    } else {
        XMoveResizeWindow(wm->display, dialog->window, dialog->x, dialog->y,
                          (unsigned)dialog->width,
                          (unsigned)dialog->height);
    }
    XMapWindow(wm->display, dialog->window);
    activate_internal_window(wm, INTERNAL_FOCUS_RUN, CurrentTime);
    draw_run_dialog(wm);
}

static void execute_run_command(WindowManager *wm)
{
    RunDialog *dialog = &wm->run_dialog;

    if (dialog->command_length == 0U) {
        snprintf(dialog->status, sizeof(dialog->status),
                 "Enter a program name or path.");
        draw_run_dialog(wm);
        return;
    }
    if (app_launch_command(dialog->command) < 0) {
        snprintf(dialog->status, sizeof(dialog->status), "Could not run: %s",
                 strerror(errno));
        draw_run_dialog(wm);
        return;
    }
    memset(dialog->command, 0, sizeof(dialog->command));
    dialog->command_length = 0U;
    dialog->status[0] = '\0';
    hide_run_dialog(wm);
}

static void initialize_run_dialog(WindowManager *wm)
{
    RunDialog *dialog = &wm->run_dialog;
    XSetWindowAttributes attributes;

    dialog->width = RUN_DIALOG_DEFAULT_WIDTH;
    dialog->height = RUN_DIALOG_DEFAULT_HEIGHT;
    restore_run_dialog_placement(wm);
    attributes.override_redirect = True;
    attributes.background_pixel = wm->theme.silver;
    attributes.event_mask = ExposureMask | ButtonPressMask | ButtonReleaseMask |
                            PointerMotionMask | KeyPressMask;
    attributes.cursor = wm->arrow_cursor;
    dialog->window = XCreateWindow(
        wm->display, wm->root, dialog->x, dialog->y, (unsigned)dialog->width,
        (unsigned)dialog->height, 0, CopyFromParent, InputOutput,
        CopyFromParent, CWOverrideRedirect | CWBackPixel | CWEventMask |
                            CWCursor,
        &attributes);
    set_internal_role(wm, dialog->window, "run-window");
    set_utf8_property(wm, dialog->window, wm->atoms.net_wm_name, "Run");
}

static size_t task_manager_application_count(const WindowManager *wm)
{
    const Client *client;
    size_t count = 0U;

    for (client = wm->clients; client != NULL; client = client->next) {
        if (client->transient_for == None)
            ++count;
    }
    return count;
}

static Client *task_manager_application_at(WindowManager *wm, size_t index)
{
    Client *client;

    for (client = wm->clients; client != NULL; client = client->next) {
        if (client->transient_for != None)
            continue;
        if (index == 0U)
            return client;
        --index;
    }
    return NULL;
}

static Client *task_manager_selected_application(WindowManager *wm)
{
    Client *client = wm->task_manager.selected_application != None
                         ? client_for_client_window(
                               wm, wm->task_manager.selected_application)
                         : NULL;

    if (client != NULL && client->transient_for == None)
        return client;
    wm->task_manager.selected_application = None;
    return NULL;
}

static void task_manager_select_first_application(WindowManager *wm)
{
    Client *client;

    if (task_manager_selected_application(wm) != NULL)
        return;
    client = task_manager_application_at(wm, 0U);
    if (client != NULL)
        wm->task_manager.selected_application = client->window;
}

static bool task_manager_application_family_contains(WindowManager *wm,
                                                     Client *application,
                                                     Client *candidate)
{
    return application != NULL && candidate != NULL &&
           client_is_in_transient_subtree(wm, candidate, application);
}

static Client *task_manager_application_focus_target(WindowManager *wm,
                                                     Client *application)
{
    Client *candidate;
    Client *target = application;

    if (application == NULL || application->minimized)
        return application;
    if (wm->active != NULL && !wm->active->minimized &&
        task_manager_application_family_contains(wm, application,
                                                 wm->active))
        return wm->active;

    /* Prefer a visible descendant over its owner so a modal dialog never
     * remains raised while keyboard focus is sent behind it.  The managed
     * client list is newest-first; retain that ordering for sibling dialogs
     * while still walking down to a nested descendant when one exists. */
    for (candidate = wm->clients; candidate != NULL;
         candidate = candidate->next) {
        if (candidate->minimized || candidate == application ||
            !task_manager_application_family_contains(
                wm, application, candidate))
            continue;
        if (target == application ||
            client_is_in_transient_subtree(wm, candidate, target))
            target = candidate;
    }
    return target;
}

static const Win31xTaskManagerProcess *task_manager_process_by_identity(
    const TaskManager *manager, pid_t pid, uint64_t start_time)
{
    const Win31xTaskManagerSnapshot *snapshot =
        win31x_task_manager_data_snapshot(&manager->data);
    size_t index;

    if (snapshot == NULL || pid <= 0 || start_time == 0U)
        return NULL;
    for (index = 0U; index < snapshot->process_count; ++index) {
        const Win31xTaskManagerProcess *process = &snapshot->processes[index];

        if (process->pid == pid && process->start_time_ticks == start_time)
            return process;
    }
    return NULL;
}

static const Win31xTaskManagerProcess *task_manager_selected_process(
    const TaskManager *manager)
{
    return task_manager_process_by_identity(
        manager, manager->selected_pid, manager->selected_start_time);
}

static double task_manager_memory_percent(
    const Win31xTaskManagerSystem *system)
{
    uint64_t used;

    if (system == NULL || system->memory_total_bytes == 0U)
        return 0.0;
    used = system->memory_available_bytes < system->memory_total_bytes
               ? system->memory_total_bytes - system->memory_available_bytes
               : 0U;
    return (double)used * 100.0 / (double)system->memory_total_bytes;
}

static void task_manager_append_history(TaskManager *manager, double cpu,
                                        double memory)
{
    if (cpu < 0.0)
        cpu = 0.0;
    if (cpu > 100.0)
        cpu = 100.0;
    if (memory < 0.0)
        memory = 0.0;
    if (memory > 100.0)
        memory = 100.0;
    if (manager->history_count < TASK_MANAGER_HISTORY_LENGTH) {
        manager->cpu_history[manager->history_count] = cpu;
        manager->memory_history[manager->history_count] = memory;
        ++manager->history_count;
    } else {
        memmove(&manager->cpu_history[0], &manager->cpu_history[1],
                (TASK_MANAGER_HISTORY_LENGTH - 1U) *
                    sizeof(manager->cpu_history[0]));
        memmove(&manager->memory_history[0], &manager->memory_history[1],
                (TASK_MANAGER_HISTORY_LENGTH - 1U) *
                    sizeof(manager->memory_history[0]));
        manager->cpu_history[TASK_MANAGER_HISTORY_LENGTH - 1U] = cpu;
        manager->memory_history[TASK_MANAGER_HISTORY_LENGTH - 1U] = memory;
    }
}

static void task_manager_publish_cardinal(WindowManager *wm, Atom property,
                                          unsigned long value)
{
    XChangeProperty(wm->display, wm->task_manager.window, property, XA_CARDINAL,
                    32, PropModeReplace, (unsigned char *)&value, 1);
}

static void task_manager_publish_state(WindowManager *wm)
{
    TaskManager *manager = &wm->task_manager;
    const Win31xTaskManagerSnapshot *snapshot =
        win31x_task_manager_data_snapshot(&manager->data);
    unsigned long cpu_tenths = 0U;
    unsigned long memory_tenths = 0U;
    unsigned long process_count = 0U;
    unsigned long selected_pid = manager->selected_pid > 0
                                     ? (unsigned long)manager->selected_pid
                                     : 0U;

    if (snapshot != NULL) {
        double cpu = snapshot->system.cpu_percent_valid
                         ? snapshot->system.cpu_percent
                         : 0.0;
        double memory = task_manager_memory_percent(&snapshot->system);

        if (cpu > 100.0)
            cpu = 100.0;
        if (memory > 100.0)
            memory = 100.0;
        cpu_tenths = (unsigned long)(cpu * 10.0 + 0.5);
        memory_tenths = (unsigned long)(memory * 10.0 + 0.5);
        process_count = snapshot->process_count > (size_t)ULONG_MAX
                            ? ULONG_MAX
                            : (unsigned long)snapshot->process_count;
    }
    task_manager_publish_cardinal(
        wm, wm->atoms.win31x_task_manager_tab,
        (unsigned long)manager->tab);
    task_manager_publish_cardinal(
        wm, wm->atoms.win31x_task_manager_sample_serial,
        manager->sample_serial);
    task_manager_publish_cardinal(
        wm, wm->atoms.win31x_task_manager_cpu_tenths, cpu_tenths);
    task_manager_publish_cardinal(
        wm, wm->atoms.win31x_task_manager_memory_tenths, memory_tenths);
    task_manager_publish_cardinal(
        wm, wm->atoms.win31x_task_manager_process_count, process_count);
    task_manager_publish_cardinal(
        wm, wm->atoms.win31x_task_manager_selected_pid, selected_pid);
}

static void task_manager_reconcile_process_state(WindowManager *wm,
                                                 uint64_t now)
{
    TaskManager *manager = &wm->task_manager;
    const Win31xTaskManagerSnapshot *snapshot =
        win31x_task_manager_data_snapshot(&manager->data);

    if (manager->selected_pid > 0 &&
        task_manager_selected_process(manager) == NULL) {
        manager->selected_pid = 0;
        manager->selected_start_time = 0U;
        manager->confirm_pid = 0;
        manager->confirm_start_time = 0U;
        manager->confirm_until_ms = 0U;
    }
    if (manager->confirm_pid > 0 && now > manager->confirm_until_ms) {
        manager->confirm_pid = 0;
        manager->confirm_start_time = 0U;
        manager->confirm_until_ms = 0U;
        snprintf(manager->status, sizeof(manager->status),
                 "End Process confirmation expired.");
        manager->status_is_refresh_error = false;
        manager->status_is_closing_application = false;
    }
    if (manager->terminating_pid > 0) {
        const Win31xTaskManagerProcess *process =
            task_manager_process_by_identity(
                manager, manager->terminating_pid,
                manager->terminating_start_time);

        if (process == NULL) {
            snprintf(manager->status, sizeof(manager->status),
                     "The process ended.");
            manager->status_is_refresh_error = false;
            manager->status_is_closing_application = false;
            manager->terminating_pid = 0;
            manager->terminating_start_time = 0U;
            manager->force_due_ms = 0U;
            manager->force_ready = false;
        } else if (now >= manager->force_due_ms) {
            manager->force_ready = true;
            snprintf(manager->status, sizeof(manager->status),
                     "%s (PID %ld) did not exit; Force End is available.",
                     process->name, (long)process->pid);
            manager->status_is_refresh_error = false;
            manager->status_is_closing_application = false;
        }
    }
    if (manager->selected_pid <= 0 && snapshot != NULL &&
        snapshot->process_count > 0U) {
        size_t index;
        size_t selected = 0U;

        for (index = 0U; index < snapshot->process_count; ++index) {
            const Win31xTaskManagerProcess *process =
                &snapshot->processes[index];

            if (process->owned_by_user && process->pid > 1 &&
                process->pid != getpid()) {
                selected = index;
                break;
            }
        }
        manager->selected_pid = snapshot->processes[selected].pid;
        manager->selected_start_time =
            snapshot->processes[selected].start_time_ticks;
        manager->process_scroll =
            selected <= (size_t)INT_MAX ? (int)selected : 0;
    }
}

static void refresh_task_manager(WindowManager *wm, bool force)
{
    TaskManager *manager = &wm->task_manager;
    const Win31xTaskManagerSnapshot *snapshot;
    uint64_t now = monotonic_milliseconds();
    uint64_t completed;
    double cpu;
    double memory;

    if (!manager->data_available || (!manager->visible && !force))
        return;
    if (!force && now != 0U && now < manager->next_refresh_ms)
        return;
    if (win31x_task_manager_data_refresh(&manager->data) < 0) {
        snprintf(manager->status, sizeof(manager->status),
                 "Could not refresh system information: %s", strerror(errno));
        manager->status_is_refresh_error = true;
        manager->status_is_closing_application = false;
        completed = monotonic_milliseconds();
        if (completed == 0U)
            completed = now;
        manager->next_refresh_ms = completed + TASK_MANAGER_REFRESH_MS;
        if (manager->visible)
            draw_task_manager(wm);
        return;
    }
    if (manager->status_is_refresh_error) {
        manager->status[0] = '\0';
        manager->status_is_refresh_error = false;
    }
    snapshot = win31x_task_manager_data_snapshot(&manager->data);
    cpu = snapshot != NULL && snapshot->system.cpu_percent_valid
              ? snapshot->system.cpu_percent
              : (manager->history_count > 0U
                     ? manager->cpu_history[manager->history_count - 1U]
                     : 0.0);
    memory = snapshot != NULL
                 ? task_manager_memory_percent(&snapshot->system)
                 : 0.0;
    task_manager_append_history(manager, cpu, memory);
    if (manager->sample_serial < ULONG_MAX)
        ++manager->sample_serial;
    completed = monotonic_milliseconds();
    if (completed == 0U)
        completed = now;
    manager->next_refresh_ms = completed + TASK_MANAGER_REFRESH_MS;
    task_manager_reconcile_process_state(wm, completed);
    task_manager_select_first_application(wm);
    task_manager_publish_state(wm);
    if (manager->visible)
        draw_task_manager(wm);
}

static void format_task_manager_bytes(uint64_t bytes, char *text,
                                      size_t capacity)
{
    static const char *const units[] = {"B", "KB", "MB", "GB", "TB"};
    double value = (double)bytes;
    size_t unit = 0U;

    while (value >= 1024.0 && unit + 1U < sizeof(units) / sizeof(units[0])) {
        value /= 1024.0;
        ++unit;
    }
    if (unit == 0U)
        snprintf(text, capacity, "%llu %s", (unsigned long long)bytes,
                 units[unit]);
    else
        snprintf(text, capacity, "%.1f %s", value, units[unit]);
}

static void format_task_manager_uptime(double seconds, char *text,
                                       size_t capacity)
{
    unsigned long long total = seconds > 0.0
                                   ? (unsigned long long)seconds
                                   : 0ULL;
    unsigned long long days = total / 86400ULL;
    unsigned long long hours = total / 3600ULL % 24ULL;
    unsigned long long minutes = total / 60ULL % 60ULL;
    unsigned long long remaining = total % 60ULL;

    snprintf(text, capacity, "%llu day%s, %02llu:%02llu:%02llu", days,
             days == 1ULL ? "" : "s", hours, minutes, remaining);
}

static void task_manager_fill_rectangle(WindowManager *wm, Drawable drawable,
                                        int x, int y, int width, int height)
{
    if (width <= 0 || height <= 0)
        return;
    XFillRectangle(wm->display, drawable, wm->gc, x, y, (unsigned)width,
                   (unsigned)height);
}

static void task_manager_draw_bevel(WindowManager *wm, Drawable drawable,
                                    int x, int y, int width, int height,
                                    bool sunken)
{
    if (width < 2 || height < 2)
        return;
    draw_bevel(wm, drawable, x, y, width, height, sunken);
}

static int task_manager_tab_x(const TaskManager *manager, TaskManagerTab tab)
{
    static const int positions[TASK_MANAGER_TAB_COUNT] = {8, 112, 211, 326};
    int available;

    if (tab < 0 || tab >= TASK_MANAGER_TAB_COUNT)
        return 8;
    if (manager == NULL || manager->width >= 416)
        return positions[tab];
    available = manager->width - 16;
    if (available < 0)
        available = 0;
    return 8 + available * (int)tab / TASK_MANAGER_TAB_COUNT;
}

static int task_manager_tab_width(const TaskManager *manager,
                                  TaskManagerTab tab)
{
    static const int widths[TASK_MANAGER_TAB_COUNT] = {105, 100, 116, 82};
    int available;
    int left;
    int right;

    if (tab < 0 || tab >= TASK_MANAGER_TAB_COUNT)
        return 0;
    if (manager == NULL || manager->width >= 416)
        return widths[tab];
    available = manager->width - 16;
    if (available <= 0)
        return 0;
    left = available * (int)tab / TASK_MANAGER_TAB_COUNT;
    right = available * ((int)tab + 1) / TASK_MANAGER_TAB_COUNT;
    return right - left;
}

static int task_manager_content_top(void)
{
    return 73;
}

static int task_manager_status_y(const TaskManager *manager)
{
    return manager->height - 24;
}

static int task_manager_action_y(const TaskManager *manager)
{
    return manager->height - 58;
}

static int task_manager_list_visible_rows(const TaskManager *manager)
{
    int height = task_manager_action_y(manager) -
                 (task_manager_content_top() + 32) - 7;
    int rows = height / TASK_MANAGER_ROW_HEIGHT;

    return rows > 0 ? rows : 0;
}

static bool task_manager_actions_visible(const TaskManager *manager)
{
    return manager != NULL &&
           task_manager_action_y(manager) >= task_manager_content_top() + 5 &&
           task_manager_action_y(manager) + 26 <=
               task_manager_status_y(manager);
}

static bool task_manager_status_visible(const TaskManager *manager)
{
    return manager != NULL && manager->width >= 12 &&
           task_manager_status_y(manager) >= task_manager_content_top();
}

static int task_manager_content_bottom(const TaskManager *manager)
{
    return task_manager_status_visible(manager)
               ? task_manager_status_y(manager) - 5
               : manager->height - 5;
}

static void task_manager_application_button_geometry(
    const TaskManager *manager, int button, int *x, int *width)
{
    int available;
    int gap = 4;
    int each;

    *x = 0;
    *width = 0;
    if (manager == NULL || button < 0 || button > 2)
        return;
    if (manager->width >= 314) {
        *x = button == 0 ? 8 : (button == 1 ? 108 : manager->width - 106);
        *width = button == 2 ? 98 : 92;
        return;
    }
    available = manager->width - 16;
    if (available <= gap * 2)
        return;
    each = (available - gap * 2) / 3;
    if (each < 2)
        return;
    *x = 8 + button * (each + gap);
    *width = button == 2 ? available - 2 * (each + gap) : each;
}

static void task_manager_process_button_geometry(const TaskManager *manager,
                                                 int button, int *x,
                                                 int *width)
{
    int available;
    int gap = 6;
    int first;

    *x = 0;
    *width = 0;
    if (manager == NULL || button < 0 || button > 1)
        return;
    if (manager->width >= 226) {
        *x = button == 0 ? 8 : manager->width - 118;
        *width = button == 0 ? 92 : 110;
        return;
    }
    available = manager->width - 16;
    if (available <= gap)
        return;
    first = (available - gap) / 2;
    if (first < 2)
        return;
    *x = button == 0 ? 8 : 8 + first + gap;
    *width = button == 0 ? first : available - first - gap;
}

static Drawable task_manager_drawable(WindowManager *wm)
{
    TaskManager *manager = &wm->task_manager;

    if (manager->backing != None &&
        (manager->backing_width != manager->width ||
         manager->backing_height != manager->height)) {
        XFreePixmap(wm->display, manager->backing);
        manager->backing = None;
    }
    if (manager->backing == None && manager->width > 0 &&
        manager->height > 0) {
        manager->backing = XCreatePixmap(
            wm->display, manager->window, (unsigned)manager->width,
            (unsigned)manager->height,
            (unsigned)DefaultDepth(wm->display, wm->screen));
        if (manager->backing != None) {
            manager->backing_width = manager->width;
            manager->backing_height = manager->height;
        }
    }
    return manager->backing != None ? manager->backing : manager->window;
}

static void draw_fitted_text(WindowManager *wm, Drawable drawable, int x,
                             int baseline, unsigned long color,
                             const char *value, int maximum_width)
{
    char fitted[512];

    if (maximum_width <= 0)
        return;
    fitted_text(wm, value != NULL ? value : "", maximum_width, fitted,
                sizeof(fitted));
    draw_text(wm, drawable, x, baseline, color, fitted);
}

static void task_manager_draw_chrome(WindowManager *wm, Drawable drawable)
{
    static const char *const labels[TASK_MANAGER_TAB_COUNT] = {
        "Applications", "Processes", "Performance", "System"};
    TaskManager *manager = &wm->task_manager;
    int width = manager->width;
    int height = manager->height;
    int tab;

    XSetForeground(wm->display, wm->gc, wm->theme.silver);
    task_manager_fill_rectangle(wm, drawable, 0, 0, width, height);
    task_manager_draw_bevel(wm, drawable, 0, 0, width, height, false);
    if (width > 6) {
        XSetForeground(wm->display, wm->gc,
                       wm->internal_focus == INTERNAL_FOCUS_TASK_MANAGER
                           ? wm->theme.active_title
                           : wm->theme.dark_gray);
        task_manager_fill_rectangle(wm, drawable, 3, 3, width - 6,
                                    TITLE_HEIGHT);
    }
    if (width >= 90) {
        draw_supplied_icon(wm, drawable, ICON_CATEGORY_TASK_MANAGER,
                           ICON_SIZE_SMALL, 7, 5);
        draw_fitted_text(wm, drawable, 28, 17, wm->theme.white,
                         "Windows 98 Task Manager",
                         internal_maximize_button_x(width) - 34);
    }
    if (width >= TITLE_BUTTON * 2 + 18) {
        draw_title_button(
            wm, drawable, internal_maximize_button_x(width), 4,
            manager->layout == CLIENT_LAYOUT_MAXIMIZED
                ? TITLE_GLYPH_RESTORE
                : TITLE_GLYPH_MAXIMIZE);
        draw_title_button(wm, drawable, internal_close_button_x(width), 4,
                          TITLE_GLYPH_CLOSE);
    }

    if (width >= 35)
        draw_text(wm, drawable, 9, 42, wm->theme.black, "File");
    if (width >= 96)
        draw_text(wm, drawable, 47, 42, wm->theme.black, "Options");
    if (width >= 140)
        draw_text(wm, drawable, 105, 42, wm->theme.black, "View");
    if (width >= 180)
        draw_text(wm, drawable, 145, 42, wm->theme.black, "Help");
    if (width > 9) {
        XSetForeground(wm->display, wm->gc, wm->theme.dark_gray);
        XDrawLine(wm->display, drawable, wm->gc, 4, 47, width - 5, 47);
        XSetForeground(wm->display, wm->gc, wm->theme.white);
        XDrawLine(wm->display, drawable, wm->gc, 4, 48, width - 5, 48);
    }

    for (tab = 0; tab < TASK_MANAGER_TAB_COUNT; ++tab) {
        int x = task_manager_tab_x(manager, (TaskManagerTab)tab);
        int tab_width = task_manager_tab_width(manager, (TaskManagerTab)tab);
        bool selected = manager->tab == (TaskManagerTab)tab;
        int y = selected ? 50 : 53;
        int tab_height = selected ? TASK_MANAGER_TAB_HEIGHT :
                                    TASK_MANAGER_TAB_HEIGHT - 3;

        if (x >= width - 7)
            continue;
        if (x + tab_width > width - 7)
            tab_width = width - 7 - x;
        if (tab_width < 12)
            continue;
        XSetForeground(wm->display, wm->gc, wm->theme.silver);
        task_manager_fill_rectangle(wm, drawable, x, y, tab_width,
                                    tab_height);
        task_manager_draw_bevel(wm, drawable, x, y, tab_width, tab_height,
                                false);
        draw_centered_text(wm, drawable, labels[tab], x + tab_width / 2,
                           y + 16, wm->theme.black, tab_width - 8);
    }
}

static void task_manager_draw_list_box(WindowManager *wm, Drawable drawable,
                                       int *list_x, int *list_y,
                                       int *list_width)
{
    TaskManager *manager = &wm->task_manager;
    int x = 8;
    int y = task_manager_content_top();
    int width = manager->width - 16;
    int height = task_manager_action_y(manager) - y - 5;

    XSetForeground(wm->display, wm->gc, wm->theme.white);
    task_manager_fill_rectangle(wm, drawable, x, y, width, height);
    task_manager_draw_bevel(wm, drawable, x, y, width, height, true);
    if (height > 28 && width > 4) {
        XSetForeground(wm->display, wm->gc, wm->theme.silver);
        task_manager_fill_rectangle(wm, drawable, x + 2, y + 2, width - 4,
                                    25);
        task_manager_draw_bevel(wm, drawable, x + 2, y + 2, width - 4, 25,
                                false);
    }
    *list_x = x + 3;
    *list_y = y + 29;
    *list_width = width > 6 ? width - 6 : 0;
}

static void task_manager_clamp_application_scroll(WindowManager *wm)
{
    TaskManager *manager = &wm->task_manager;
    int maximum = (int)task_manager_application_count(wm) -
                  task_manager_list_visible_rows(manager);

    if (maximum < 0)
        maximum = 0;
    if (manager->application_scroll < 0)
        manager->application_scroll = 0;
    if (manager->application_scroll > maximum)
        manager->application_scroll = maximum;
}

static void task_manager_clamp_process_scroll(WindowManager *wm)
{
    TaskManager *manager = &wm->task_manager;
    const Win31xTaskManagerSnapshot *snapshot =
        win31x_task_manager_data_snapshot(&manager->data);
    int count = snapshot == NULL
                    ? 0
                    : (snapshot->process_count <= (size_t)INT_MAX
                           ? (int)snapshot->process_count
                           : INT_MAX);
    int maximum = count - task_manager_list_visible_rows(manager);

    if (maximum < 0)
        maximum = 0;
    if (manager->process_scroll < 0)
        manager->process_scroll = 0;
    if (manager->process_scroll > maximum)
        manager->process_scroll = maximum;
}

static void task_manager_draw_applications(WindowManager *wm,
                                           Drawable drawable)
{
    TaskManager *manager = &wm->task_manager;
    Client *selected = task_manager_selected_application(wm);
    int x;
    int y;
    int width;
    int rows = task_manager_list_visible_rows(manager);
    int index;
    int status_x;
    bool selection_available;
    bool show_status;
    int button_x;
    int button_width;

    task_manager_clamp_application_scroll(wm);
    task_manager_draw_list_box(wm, drawable, &x, &y, &width);
    status_x = x + width * 3 / 4;
    show_status = width >= 160;
    if (width > 12)
        draw_text(wm, drawable, x + 5, y - 11, wm->theme.black, "Task");
    if (show_status)
        draw_text(wm, drawable, status_x, y - 11, wm->theme.black, "Status");
    for (index = 0; index < rows; ++index) {
        int application_index = manager->application_scroll + index;
        Client *client = task_manager_application_at(
            wm, (size_t)application_index);
        int row_y = y + index * TASK_MANAGER_ROW_HEIGHT;
        bool is_selected;
        bool family_is_active;
        unsigned long color;
        const char *status;

        if (client == NULL)
            break;
        is_selected = client->window == manager->selected_application;
        if (is_selected) {
            XSetForeground(wm->display, wm->gc, wm->theme.active_title);
            task_manager_fill_rectangle(wm, drawable, x + 1, row_y,
                                        width - 2,
                                        TASK_MANAGER_ROW_HEIGHT);
        }
        color = is_selected ? wm->theme.white : wm->theme.black;
        if (client->actual_icon_small.color != None)
            draw_rendered_icon_centered(wm, drawable,
                                        &client->actual_icon_small, x + 3,
                                        row_y + 2, 16, 16);
        else
            draw_supplied_icon(wm, drawable, client_icon_category(client),
                               ICON_SIZE_SMALL, x + 3, row_y + 2);
        draw_fitted_text(wm, drawable, x + 23, row_y + 15, color,
                         client->title,
                         (show_status ? status_x : x + width) - x - 27);
        family_is_active = wm->active != NULL && !wm->active->minimized &&
                           task_manager_application_family_contains(
                               wm, client, wm->active);
        status = client->minimized
                     ? "Minimized"
                     : (family_is_active ? "Active" : "Running");
        if (show_status)
            draw_fitted_text(wm, drawable, status_x, row_y + 15, color,
                             status, x + width - status_x - 5);
    }
    if (rows > 0 && width > 12 && task_manager_application_count(wm) == 0U)
        draw_centered_text(wm, drawable, "No applications are running",
                           manager->width / 2,
                           y + TASK_MANAGER_ROW_HEIGHT + 8,
                           wm->theme.dark_gray, manager->width - 40);
    selection_available = selected != NULL;
    if (!task_manager_actions_visible(manager))
        return;
    task_manager_application_button_geometry(manager, 0, &button_x,
                                             &button_width);
    draw_button(wm, drawable, button_x, task_manager_action_y(manager),
                button_width, 26, "New Task...", false, true);
    task_manager_application_button_geometry(manager, 1, &button_x,
                                             &button_width);
    draw_button(wm, drawable, button_x, task_manager_action_y(manager),
                button_width, 26, "Switch To", false, selection_available);
    task_manager_application_button_geometry(manager, 2, &button_x,
                                             &button_width);
    draw_button(wm, drawable, button_x, task_manager_action_y(manager),
                button_width, 26, "End Task", false, selection_available);
}

static void task_manager_draw_processes(WindowManager *wm, Drawable drawable)
{
    TaskManager *manager = &wm->task_manager;
    const Win31xTaskManagerSnapshot *snapshot =
        win31x_task_manager_data_snapshot(&manager->data);
    const Win31xTaskManagerProcess *selected =
        task_manager_selected_process(manager);
    int x;
    int y;
    int width;
    int rows = task_manager_list_visible_rows(manager);
    int name_x;
    int pid_x;
    int cpu_x;
    int memory_x;
    int user_x;
    int row;
    int button_x;
    int button_width;
    bool show_pid;
    bool show_cpu;
    bool show_memory;
    bool show_user;
    const char *action_label = "End Process";
    bool action_enabled = selected != NULL && selected->owned_by_user &&
                          selected->pid > 1 && selected->pid != getpid();
    bool selected_is_terminating =
        selected != NULL && selected->pid == manager->terminating_pid &&
        selected->start_time_ticks == manager->terminating_start_time;

    task_manager_clamp_process_scroll(wm);
    task_manager_draw_list_box(wm, drawable, &x, &y, &width);
    name_x = x + 5;
    show_pid = width >= 120;
    show_cpu = width >= 390;
    show_memory = width >= 240;
    show_user = width >= 390;
    if (show_cpu) {
        pid_x = x + width * 43 / 100;
        cpu_x = x + width * 54 / 100;
        memory_x = x + width * 65 / 100;
        user_x = x + width * 84 / 100;
    } else if (show_memory) {
        pid_x = x + width * 60 / 100;
        cpu_x = pid_x;
        memory_x = x + width * 76 / 100;
        user_x = x + width;
    } else {
        pid_x = x + width * 75 / 100;
        cpu_x = pid_x;
        memory_x = x + width;
        user_x = x + width;
    }
    if (!show_pid)
        pid_x = x + width;
    if (width > 12)
        draw_text(wm, drawable, name_x, y - 11, wm->theme.black,
                  "Image Name");
    if (show_pid)
        draw_text(wm, drawable, pid_x, y - 11, wm->theme.black, "PID");
    if (show_cpu)
        draw_text(wm, drawable, cpu_x, y - 11, wm->theme.black, "CPU");
    if (show_memory)
        draw_text(wm, drawable, memory_x, y - 11, wm->theme.black, "Memory");
    if (show_user)
        draw_text(wm, drawable, user_x, y - 11, wm->theme.black, "User");
    for (row = 0; row < rows; ++row) {
        size_t process_index = (size_t)(manager->process_scroll + row);
        const Win31xTaskManagerProcess *process;
        int row_y = y + row * TASK_MANAGER_ROW_HEIGHT;
        bool is_selected;
        unsigned long color;
        char pid_text[32];
        char cpu_text[32];
        char memory_text[32];
        char user_text[32];

        if (snapshot == NULL || process_index >= snapshot->process_count)
            break;
        process = &snapshot->processes[process_index];
        is_selected = process->pid == manager->selected_pid &&
                      process->start_time_ticks == manager->selected_start_time;
        if (is_selected) {
            XSetForeground(wm->display, wm->gc, wm->theme.active_title);
            task_manager_fill_rectangle(wm, drawable, x + 1, row_y,
                                        width - 2,
                                        TASK_MANAGER_ROW_HEIGHT);
        }
        color = is_selected ? wm->theme.white : wm->theme.black;
        snprintf(pid_text, sizeof(pid_text), "%ld", (long)process->pid);
        if (process->cpu_percent_valid)
            snprintf(cpu_text, sizeof(cpu_text), "%.1f%%",
                     process->cpu_percent);
        else
            snprintf(cpu_text, sizeof(cpu_text), "--");
        format_task_manager_bytes(process->resident_bytes, memory_text,
                                  sizeof(memory_text));
        if (process->owned_by_user)
            snprintf(user_text, sizeof(user_text), "You");
        else if (process->uid == 0)
            snprintf(user_text, sizeof(user_text), "root");
        else
            snprintf(user_text, sizeof(user_text), "UID %lu",
                     (unsigned long)process->uid);
        draw_fitted_text(wm, drawable, name_x, row_y + 15, color,
                         process->name, pid_x - name_x - 5);
        if (show_pid)
            draw_fitted_text(wm, drawable, pid_x, row_y + 15, color,
                             pid_text,
                             (show_cpu ? cpu_x : memory_x) - pid_x - 4);
        if (show_cpu)
            draw_fitted_text(wm, drawable, cpu_x, row_y + 15, color,
                             cpu_text, memory_x - cpu_x - 4);
        if (show_memory)
            draw_fitted_text(wm, drawable, memory_x, row_y + 15, color,
                             memory_text, user_x - memory_x - 4);
        if (show_user)
            draw_fitted_text(wm, drawable, user_x, row_y + 15, color,
                             user_text, x + width - user_x - 3);
    }
    if (rows > 0 && width > 12 &&
        (snapshot == NULL || snapshot->process_count == 0U))
        draw_centered_text(wm, drawable, "No process information is available",
                           manager->width / 2,
                           y + TASK_MANAGER_ROW_HEIGHT + 8,
                           wm->theme.dark_gray, manager->width - 40);
    if (selected_is_terminating) {
        if (manager->force_ready) {
            action_label = "Force End";
        } else {
            action_label = "Ending...";
            action_enabled = false;
        }
    } else if (selected != NULL && manager->confirm_pid == selected->pid &&
               manager->confirm_start_time == selected->start_time_ticks &&
               monotonic_milliseconds() <= manager->confirm_until_ms) {
        action_label = "Confirm End";
    }
    if (!task_manager_actions_visible(manager))
        return;
    task_manager_process_button_geometry(manager, 0, &button_x,
                                         &button_width);
    draw_button(wm, drawable, button_x, task_manager_action_y(manager),
                button_width, 26, "Refresh", false,
                manager->data_available);
    task_manager_process_button_geometry(manager, 1, &button_x,
                                         &button_width);
    draw_button(wm, drawable, button_x, task_manager_action_y(manager),
                button_width, 26, action_label, false, action_enabled);
}

static void task_manager_draw_graph(WindowManager *wm, Drawable drawable,
                                    int x, int y, int width, int height,
                                    const double *history, size_t count,
                                    const char *label, double current)
{
    XPoint points[TASK_MANAGER_HISTORY_LENGTH];
    size_t shown;
    size_t first;
    size_t index;
    char value[64];
    int inner_width;
    int inner_height;

    if (width < 30 || height < 30)
        return;
    XSetForeground(wm->display, wm->gc, wm->theme.black);
    task_manager_fill_rectangle(wm, drawable, x, y, width, height);
    task_manager_draw_bevel(wm, drawable, x - 1, y - 1, width + 2,
                            height + 2, true);
    XSetForeground(wm->display, wm->gc, wm->theme.graph_grid);
    for (index = 1U; index < 4U; ++index) {
        int grid_x = x + (int)((size_t)width * index / 4U);
        int grid_y = y + (int)((size_t)height * index / 4U);

        XDrawLine(wm->display, drawable, wm->gc, grid_x, y, grid_x,
                  y + height - 1);
        XDrawLine(wm->display, drawable, wm->gc, x, grid_y, x + width - 1,
                  grid_y);
    }
    inner_width = width - 2;
    inner_height = height - 3;
    shown = count;
    if (shown > (size_t)inner_width)
        shown = (size_t)inner_width;
    first = count - shown;
    for (index = 0U; index < shown; ++index) {
        double value_at = history[first + index];

        if (value_at < 0.0)
            value_at = 0.0;
        if (value_at > 100.0)
            value_at = 100.0;
        points[index].x = (short)(x + 1 +
            (shown > 1U ? (int)(index * (size_t)(inner_width - 1) /
                                     (shown - 1U))
                        : inner_width - 1));
        points[index].y = (short)(y + height - 2 -
                                  (int)(value_at * inner_height / 100.0));
    }
    if (shown >= 2U) {
        XSetForeground(wm->display, wm->gc, wm->theme.graph_green);
        XDrawLines(wm->display, drawable, wm->gc, points, (int)shown,
                   CoordModeOrigin);
    }
    draw_text(wm, drawable, x + 5, y + 15, wm->theme.graph_green, label);
    snprintf(value, sizeof(value), "%.1f%%", current);
    draw_fitted_text(wm, drawable, x + width - 70, y + 15,
                     wm->theme.graph_green, value, 65);
}

static void task_manager_draw_performance(WindowManager *wm,
                                          Drawable drawable)
{
    TaskManager *manager = &wm->task_manager;
    const Win31xTaskManagerSnapshot *snapshot =
        win31x_task_manager_data_snapshot(&manager->data);
    double cpu = snapshot != NULL && snapshot->system.cpu_percent_valid
                     ? snapshot->system.cpu_percent
                     : 0.0;
    double memory = snapshot != NULL
                        ? task_manager_memory_percent(&snapshot->system)
                        : 0.0;
    int x = 12;
    int y = task_manager_content_top() + 10;
    int width = manager->width - 24;
    int available_height = task_manager_content_bottom(manager) - y - 7;

    if (width >= 560) {
        int graph_width = (width - 12) / 2;

        task_manager_draw_graph(wm, drawable, x, y, graph_width,
                                available_height, manager->cpu_history,
                                manager->history_count, "CPU Usage History",
                                cpu);
        task_manager_draw_graph(wm, drawable, x + graph_width + 12, y,
                                width - graph_width - 12, available_height,
                                manager->memory_history,
                                manager->history_count,
                                "Memory Usage History", memory);
    } else {
        int graph_height = (available_height - 10) / 2;

        task_manager_draw_graph(wm, drawable, x, y, width, graph_height,
                                manager->cpu_history, manager->history_count,
                                "CPU Usage History", cpu);
        task_manager_draw_graph(wm, drawable, x, y + graph_height + 10,
                                width, available_height - graph_height - 10,
                                manager->memory_history,
                                manager->history_count,
                                "Memory Usage History", memory);
    }
}

static void task_manager_draw_system_row(WindowManager *wm, Drawable drawable,
                                         int x, int y, int label_width,
                                         int value_width, const char *label,
                                         const char *value)
{
    draw_fitted_text(wm, drawable, x, y, wm->theme.dark_gray, label,
                     label_width - 8);
    draw_fitted_text(wm, drawable, x + label_width, y, wm->theme.black, value,
                     value_width);
}

static void task_manager_draw_system(WindowManager *wm, Drawable drawable)
{
    TaskManager *manager = &wm->task_manager;
    const Win31xTaskManagerSnapshot *snapshot =
        win31x_task_manager_data_snapshot(&manager->data);
    int x = 16;
    int y = task_manager_content_top() + 20;
    int width = manager->width - 32;
    int label_width = width > 400 ? 145 : width / 3;
    char memory[128];
    char uptime[96];
    char cores[32];
    char load[96];
    char processes[64];
    char memory_total[48];
    char memory_used[48];
    uint64_t used = 0U;
    int content_height = task_manager_content_bottom(manager) -
                         task_manager_content_top();
    const char *labels[9];
    const char *values[9];
    size_t row;

    XSetForeground(wm->display, wm->gc, wm->theme.white);
    task_manager_fill_rectangle(wm, drawable, 8, task_manager_content_top(),
                                manager->width - 16, content_height);
    task_manager_draw_bevel(wm, drawable, 8, task_manager_content_top(),
                            manager->width - 16, content_height, true);
    if (content_height <= 8 || width <= 0)
        return;
    draw_supplied_icon(wm, drawable, ICON_CATEGORY_TASK_MANAGER,
                       ICON_SIZE_LARGE, x, y - 11);
    if (width > 50)
        draw_fitted_text(wm, drawable, x + 42, y + 2, wm->theme.black,
                         "System Information", width - 42);
    y += 42;
    if (snapshot == NULL) {
        draw_text(wm, drawable, x, y, wm->theme.dark_gray,
                  "System information is unavailable.");
        return;
    }
    if (snapshot->system.memory_total_bytes >
        snapshot->system.memory_available_bytes)
        used = snapshot->system.memory_total_bytes -
               snapshot->system.memory_available_bytes;
    format_task_manager_bytes(snapshot->system.memory_total_bytes,
                              memory_total, sizeof(memory_total));
    format_task_manager_bytes(used, memory_used, sizeof(memory_used));
    snprintf(memory, sizeof(memory), "%s used / %s total", memory_used,
             memory_total);
    format_task_manager_uptime(snapshot->system.uptime_seconds, uptime,
                               sizeof(uptime));
    snprintf(cores, sizeof(cores), "%u logical processor%s",
             snapshot->system.cpu_core_count,
             snapshot->system.cpu_core_count == 1U ? "" : "s");
    snprintf(load, sizeof(load), "%.2f, %.2f, %.2f",
             snapshot->system.load_average[0],
             snapshot->system.load_average[1],
             snapshot->system.load_average[2]);
    snprintf(processes, sizeof(processes), "%zu%s", snapshot->process_count,
             snapshot->process_list_truncated ? " (list truncated)" : "");
    labels[0] = "Operating system:";
    labels[1] = "Computer name:";
    labels[2] = "Kernel:";
    labels[3] = "Processor:";
    labels[4] = "CPU configuration:";
    labels[5] = "Physical memory:";
    labels[6] = "System uptime:";
    labels[7] = "Load averages:";
    labels[8] = "Processes:";
    values[0] = snapshot->system.operating_system;
    values[1] = snapshot->system.hostname;
    values[2] = snapshot->system.kernel;
    values[3] = snapshot->system.cpu_model;
    values[4] = cores;
    values[5] = memory;
    values[6] = uptime;
    values[7] = load;
    values[8] = processes;
    for (row = 0U; row < sizeof(labels) / sizeof(labels[0]); ++row) {
        if (y + 5 > task_manager_content_bottom(manager))
            break;
        task_manager_draw_system_row(wm, drawable, x, y, label_width,
                                     width - label_width, labels[row],
                                     values[row]);
        y += 26;
    }
}

static void task_manager_draw_status(WindowManager *wm, Drawable drawable)
{
    TaskManager *manager = &wm->task_manager;
    const Win31xTaskManagerSnapshot *snapshot =
        win31x_task_manager_data_snapshot(&manager->data);
    char status[256];

    if (!task_manager_status_visible(manager))
        return;

    XSetForeground(wm->display, wm->gc, wm->theme.silver);
    task_manager_fill_rectangle(wm, drawable, 5,
                                task_manager_status_y(manager),
                                manager->width - 10, 19);
    task_manager_draw_bevel(wm, drawable, 5,
                            task_manager_status_y(manager),
                            manager->width - 10, 19, true);
    if (manager->status[0] != '\0') {
        snprintf(status, sizeof(status), "%s", manager->status);
    } else if (snapshot != NULL) {
        double cpu = snapshot->system.cpu_percent_valid
                         ? snapshot->system.cpu_percent
                         : 0.0;
        double memory = task_manager_memory_percent(&snapshot->system);

        snprintf(status, sizeof(status),
                 "Processes: %zu   CPU Usage: %.1f%%   Memory Usage: %.1f%%",
                 snapshot->process_count, cpu, memory);
    } else {
        snprintf(status, sizeof(status), "System information unavailable");
    }
    draw_fitted_text(wm, drawable, 10, task_manager_status_y(manager) + 14,
                     wm->theme.black, status, manager->width - 20);
}

static void draw_task_manager(WindowManager *wm)
{
    TaskManager *manager = &wm->task_manager;
    Drawable drawable;

    if (manager->window == None || manager->width <= 0 ||
        manager->height <= 0)
        return;
    drawable = task_manager_drawable(wm);
    task_manager_draw_chrome(wm, drawable);
    switch (manager->tab) {
    case TASK_MANAGER_TAB_APPLICATIONS:
        task_manager_draw_applications(wm, drawable);
        break;
    case TASK_MANAGER_TAB_PROCESSES:
        task_manager_draw_processes(wm, drawable);
        break;
    case TASK_MANAGER_TAB_PERFORMANCE:
        task_manager_draw_performance(wm, drawable);
        break;
    case TASK_MANAGER_TAB_SYSTEM:
        task_manager_draw_system(wm, drawable);
        break;
    default:
        manager->tab = TASK_MANAGER_TAB_APPLICATIONS;
        task_manager_draw_applications(wm, drawable);
        break;
    }
    task_manager_draw_status(wm, drawable);
    if (manager->backing != None)
        XCopyArea(wm->display, manager->backing, manager->window, wm->gc,
                  0, 0, (unsigned)manager->width,
                  (unsigned)manager->height, 0, 0);
}

static void restore_task_manager_placement(WindowManager *wm)
{
    TaskManager *manager = &wm->task_manager;
    const Win31xDesktopPlacement *saved = &wm->desktop_state.task_manager;
    size_t monitor_index;
    int x;
    int y;
    int width;
    int height;

    if (!saved->valid)
        return;
    monitor_index = geometry_from_placement(wm, saved, &x, &y, &width,
                                            &height, true);
    manager->x = x;
    manager->y = y;
    manager->width = width;
    manager->height = height;
    manager->layout = (ClientLayout)saved->layout;
    manager->layout_before_maximize =
        (ClientLayout)saved->layout_before_maximize;
    if (manager->layout != CLIENT_LAYOUT_NORMAL) {
        manager->restore_x = x;
        manager->restore_y = y;
        manager->restore_width = width;
        manager->restore_height = height;
        manager->restore_valid = true;
        set_monitor_anchor(&manager->layout_monitor,
                           monitor_at(wm, monitor_index));
        screen_layout_geometry(monitor_at(wm, monitor_index), manager->layout,
                               &manager->x, &manager->y, &manager->width,
                               &manager->height);
    }
    manager->positioned = true;
}

static void remember_task_manager_placement(WindowManager *wm)
{
    TaskManager *manager = &wm->task_manager;
    size_t monitor_index;
    int x = manager->x;
    int y = manager->y;
    int width = manager->width;
    int height = manager->height;

    if (manager->layout != CLIENT_LAYOUT_NORMAL && manager->restore_valid) {
        x = manager->restore_x;
        y = manager->restore_y;
        width = manager->restore_width;
        height = manager->restore_height;
    }
    monitor_index = manager->layout != CLIENT_LAYOUT_NORMAL &&
                            manager->layout_monitor.valid
                        ? monitor_index_for_anchor(wm,
                                                   &manager->layout_monitor)
                        : monitor_index_for_rectangle(wm, x, y, width,
                                                      height);
    placement_from_geometry(wm, &wm->desktop_state.task_manager,
                            monitor_index, x, y, width, height,
                            manager->layout);
    if (wm->desktop_state.task_manager.valid) {
        wm->desktop_state.task_manager.layout_before_maximize =
            (Win31xDesktopLayout)manager->layout_before_maximize;
        mark_desktop_state_dirty(wm);
    }
}

static void apply_task_manager_layout(WindowManager *wm, ClientLayout layout)
{
    TaskManager *manager = &wm->task_manager;

    if (layout == CLIENT_LAYOUT_NORMAL &&
        manager->layout == CLIENT_LAYOUT_MAXIMIZED &&
        manager->layout_before_maximize != CLIENT_LAYOUT_NORMAL)
        layout = manager->layout_before_maximize;
    if (layout == CLIENT_LAYOUT_MAXIMIZED &&
        manager->layout != CLIENT_LAYOUT_MAXIMIZED)
        manager->layout_before_maximize = manager->layout;
    if (layout == CLIENT_LAYOUT_NORMAL) {
        if (manager->layout == CLIENT_LAYOUT_NORMAL)
            return;
        manager->layout = CLIENT_LAYOUT_NORMAL;
        if (manager->restore_valid) {
            manager->x = manager->restore_x;
            manager->y = manager->restore_y;
            manager->width = manager->restore_width;
            manager->height = manager->restore_height;
        }
        manager->restore_valid = false;
        manager->layout_monitor.valid = false;
        clamp_internal_geometry(wm, &manager->x, &manager->y,
                                &manager->width, &manager->height);
    } else {
        const MonitorGeometry *monitor;

        if (!manager->layout_monitor.valid) {
            set_monitor_anchor(
                &manager->layout_monitor,
                monitor_at(wm, monitor_index_for_rectangle(
                                   wm, manager->x, manager->y,
                                   manager->width, manager->height)));
        }
        if (manager->layout == CLIENT_LAYOUT_NORMAL) {
            manager->restore_x = manager->x;
            manager->restore_y = manager->y;
            manager->restore_width = manager->width;
            manager->restore_height = manager->height;
            manager->restore_valid = true;
        }
        manager->layout = layout;
        monitor = monitor_at(
            wm, monitor_index_for_anchor(wm, &manager->layout_monitor));
        set_monitor_anchor(&manager->layout_monitor, monitor);
        screen_layout_geometry(monitor, layout, &manager->x, &manager->y,
                               &manager->width, &manager->height);
    }
    task_manager_clamp_application_scroll(wm);
    task_manager_clamp_process_scroll(wm);
    XMoveResizeWindow(wm->display, manager->window, manager->x, manager->y,
                      (unsigned)manager->width, (unsigned)manager->height);
    draw_task_manager(wm);
}

static void toggle_task_manager_maximize(WindowManager *wm)
{
    apply_task_manager_layout(
        wm, wm->task_manager.layout == CLIENT_LAYOUT_MAXIMIZED
                ? CLIENT_LAYOUT_NORMAL
                : CLIENT_LAYOUT_MAXIMIZED);
    remember_task_manager_placement(wm);
}

static void position_task_manager_on_monitor(WindowManager *wm,
                                             size_t monitor_index)
{
    TaskManager *manager = &wm->task_manager;
    const MonitorGeometry *monitor = monitor_at(wm, monitor_index);
    int available_width = monitor->width - 24;
    int available_height = monitor->height - 24;

    manager->layout = CLIENT_LAYOUT_NORMAL;
    manager->layout_before_maximize = CLIENT_LAYOUT_NORMAL;
    manager->restore_valid = false;
    manager->layout_monitor.valid = false;
    manager->width = available_width >= 420
                         ? (available_width < TASK_MANAGER_DEFAULT_WIDTH
                                ? available_width
                                : TASK_MANAGER_DEFAULT_WIDTH)
                         : monitor->width;
    manager->height = available_height >= 340
                          ? (available_height < TASK_MANAGER_DEFAULT_HEIGHT
                                 ? available_height
                                 : TASK_MANAGER_DEFAULT_HEIGHT)
                          : monitor->height;
    if (manager->width < 1)
        manager->width = 1;
    if (manager->height < 1)
        manager->height = 1;
    manager->x = monitor->x + (monitor->width - manager->width) / 2;
    manager->y = monitor->y + (monitor->height - manager->height) / 2;
    manager->positioned = true;
    XMoveResizeWindow(wm->display, manager->window, manager->x, manager->y,
                      (unsigned)manager->width, (unsigned)manager->height);
}

static void dismiss_task_manager(WindowManager *wm)
{
    TaskManager *manager = &wm->task_manager;

    if (!manager->visible)
        return;
    manager->visible = false;
    manager->confirm_pid = 0;
    manager->confirm_start_time = 0U;
    manager->confirm_until_ms = 0U;
    manager->process_delete_down = false;
    if (wm->internal_focus == INTERNAL_FOCUS_TASK_MANAGER)
        wm->internal_focus = INTERNAL_FOCUS_NONE;
    if (wm->drag.kind == DRAG_MOVE_TASK_MANAGER)
        cancel_drag(wm, CurrentTime);
    XUnmapWindow(wm->display, manager->window);
    update_focus_overlays(wm);
}

static void remember_task_manager_return_focus(WindowManager *wm)
{
    TaskManager *manager = &wm->task_manager;

    if (wm->internal_focus == INTERNAL_FOCUS_TASK_MANAGER)
        return;
    manager->return_internal_focus = wm->internal_focus;
    manager->return_client =
        wm->internal_focus == INTERNAL_FOCUS_NONE && wm->active != NULL
            ? wm->active->window
            : None;
}

static void restore_task_manager_return_focus(WindowManager *wm,
                                              bool was_focused, Time time)
{
    TaskManager *manager = &wm->task_manager;
    InternalFocus internal = manager->return_internal_focus;
    Window client_window = manager->return_client;
    Client *client = client_window != None
                         ? client_for_client_window(wm, client_window)
                         : NULL;

    manager->return_internal_focus = INTERNAL_FOCUS_NONE;
    manager->return_client = None;
    if (!was_focused)
        return;
    if (internal == INTERNAL_FOCUS_APPLICATIONS && wm->launcher_visible) {
        activate_internal_window(wm, INTERNAL_FOCUS_APPLICATIONS, time);
        return;
    }
    if (internal == INTERNAL_FOCUS_CONTROL_PANEL &&
        wm->control_panel.visible) {
        activate_internal_window(wm, INTERNAL_FOCUS_CONTROL_PANEL, time);
        return;
    }
    if (internal == INTERNAL_FOCUS_RUN && wm->run_dialog.visible) {
        activate_internal_window(wm, INTERNAL_FOCUS_RUN, time);
        return;
    }
    if (client != NULL && !client->minimized) {
        focus_client(wm, client, time);
        return;
    }
    focus_after_internal_close(wm, true, time);
}

static void hide_task_manager(WindowManager *wm)
{
    bool was_focused;

    if (!wm->task_manager.visible)
        return;
    was_focused = wm->internal_focus == INTERNAL_FOCUS_TASK_MANAGER;
    dismiss_task_manager(wm);
    restore_task_manager_return_focus(wm, was_focused, CurrentTime);
}

static void show_task_manager(WindowManager *wm)
{
    TaskManager *manager = &wm->task_manager;

    dismiss_desktop_menu(wm);
    remember_task_manager_return_focus(wm);
    if (manager->visible) {
        activate_internal_window(wm, INTERNAL_FOCUS_TASK_MANAGER,
                                 CurrentTime);
        XFlush(wm->display);
        refresh_task_manager(wm, false);
        return;
    }
    manager->visible = true;
    if (manager->data_available) {
        manager->status[0] = '\0';
        manager->status_is_refresh_error = false;
        manager->status_is_closing_application = false;
    } else {
        snprintf(manager->status, sizeof(manager->status),
                 "Task information is unavailable on this system.");
        manager->status_is_refresh_error = false;
        manager->status_is_closing_application = false;
    }
    if (!manager->positioned) {
        position_task_manager_on_monitor(wm, active_monitor_index(wm));
        remember_task_manager_placement(wm);
    } else {
        XMoveResizeWindow(wm->display, manager->window, manager->x,
                          manager->y, (unsigned)manager->width,
                          (unsigned)manager->height);
    }
    XMapWindow(wm->display, manager->window);
    activate_internal_window(wm, INTERNAL_FOCUS_TASK_MANAGER, CurrentTime);
    draw_task_manager(wm);
    XFlush(wm->display);
    refresh_task_manager(wm, true);
}

static void initialize_task_manager(WindowManager *wm)
{
    TaskManager *manager = &wm->task_manager;
    XSetWindowAttributes attributes;

    manager->width = TASK_MANAGER_DEFAULT_WIDTH;
    manager->height = TASK_MANAGER_DEFAULT_HEIGHT;
    manager->tab = TASK_MANAGER_TAB_APPLICATIONS;
    restore_task_manager_placement(wm);
    attributes.override_redirect = True;
    attributes.background_pixel = wm->theme.silver;
    attributes.event_mask = ExposureMask | ButtonPressMask |
                            ButtonReleaseMask | PointerMotionMask |
                            KeyPressMask | KeyReleaseMask;
    attributes.cursor = wm->arrow_cursor;
    manager->window = XCreateWindow(
        wm->display, wm->root, manager->x, manager->y,
        (unsigned)manager->width, (unsigned)manager->height, 0,
        CopyFromParent, InputOutput, CopyFromParent,
        CWOverrideRedirect | CWBackPixel | CWEventMask | CWCursor,
        &attributes);
    set_internal_role(wm, manager->window, "task-manager-window");
    set_utf8_property(wm, manager->window, wm->atoms.net_wm_name,
                      "Windows 98 Task Manager");
    if (win31x_task_manager_data_init(&manager->data) < 0) {
        manager->data_available = false;
        snprintf(manager->status, sizeof(manager->status),
                 "Task information is unavailable: %s", strerror(errno));
        manager->status_is_refresh_error = false;
        manager->status_is_closing_application = false;
    } else {
        manager->data_available = true;
    }
    task_manager_publish_state(wm);
}

static void task_manager_switch_to_application(WindowManager *wm, Time time)
{
    Client *client = task_manager_selected_application(wm);
    Client *target;

    if (client == NULL)
        return;
    if (client->minimized)
        restore_client(wm, client, time);
    else {
        target = task_manager_application_focus_target(wm, client);
        focus_client(wm, target, time);
    }
}

static void task_manager_end_application(WindowManager *wm, Time time)
{
    Client *client = task_manager_selected_application(wm);

    if (client == NULL)
        return;
    snprintf(wm->task_manager.status, sizeof(wm->task_manager.status),
             "Closing %s...", client->title);
    wm->task_manager.closing_application = client->window;
    wm->task_manager.status_is_refresh_error = false;
    wm->task_manager.status_is_closing_application = true;
    close_client(wm, client, time);
    draw_task_manager(wm);
}

static void task_manager_request_end_process(WindowManager *wm)
{
    TaskManager *manager = &wm->task_manager;
    const Win31xTaskManagerProcess *process =
        task_manager_selected_process(manager);
    uint64_t now = monotonic_milliseconds();
    char error[WIN31X_TASK_MANAGER_ERROR_CAPACITY];

    if (process == NULL)
        return;
    manager->status_is_closing_application = false;
    if (!process->owned_by_user || process->pid <= 1 ||
        process->pid == getpid()) {
        snprintf(manager->status, sizeof(manager->status),
                 "This process cannot be ended by Task Manager.");
        manager->status_is_refresh_error = false;
        XBell(wm->display, 0);
        draw_task_manager(wm);
        return;
    }
    if (!manager->force_ready && process->pid == manager->terminating_pid &&
        process->start_time_ticks == manager->terminating_start_time) {
        snprintf(manager->status, sizeof(manager->status),
                 "Waiting for %s (PID %ld) to exit...", process->name,
                 (long)process->pid);
        manager->status_is_refresh_error = false;
        draw_task_manager(wm);
        return;
    }
    if (manager->force_ready && process->pid == manager->terminating_pid &&
        process->start_time_ticks == manager->terminating_start_time) {
        if (win31x_task_manager_data_force_terminate(
                &manager->data, process->pid, process->start_time_ticks,
                error, sizeof(error)) < 0) {
            snprintf(manager->status, sizeof(manager->status), "%s", error);
            manager->status_is_refresh_error = false;
            XBell(wm->display, 0);
        } else {
            manager->force_ready = false;
            snprintf(manager->status, sizeof(manager->status),
                     "Force-ending %s (PID %ld)...", process->name,
                     (long)process->pid);
            manager->status_is_refresh_error = false;
        }
        manager->next_refresh_ms = 0U;
        draw_task_manager(wm);
        return;
    }
    if (manager->confirm_pid != process->pid ||
        manager->confirm_start_time != process->start_time_ticks ||
        now > manager->confirm_until_ms) {
        manager->confirm_pid = process->pid;
        manager->confirm_start_time = process->start_time_ticks;
        manager->confirm_until_ms = now + TASK_MANAGER_CONFIRM_MS;
        snprintf(manager->status, sizeof(manager->status),
                 "Warning: ending a process can lose data. Click End Process again to confirm PID %ld.",
                 (long)process->pid);
        manager->status_is_refresh_error = false;
        XBell(wm->display, 0);
        draw_task_manager(wm);
        return;
    }
    manager->confirm_pid = 0;
    manager->confirm_start_time = 0U;
    manager->confirm_until_ms = 0U;
    if (win31x_task_manager_data_terminate(
            &manager->data, process->pid, process->start_time_ticks,
            error, sizeof(error)) < 0) {
        snprintf(manager->status, sizeof(manager->status), "%s", error);
        manager->status_is_refresh_error = false;
        XBell(wm->display, 0);
    } else {
        manager->terminating_pid = process->pid;
        manager->terminating_start_time = process->start_time_ticks;
        manager->force_due_ms = now + TASK_MANAGER_FORCE_DELAY_MS;
        manager->force_ready = false;
        snprintf(manager->status, sizeof(manager->status),
                 "End signal sent to %s (PID %ld).", process->name,
                 (long)process->pid);
        manager->status_is_refresh_error = false;
    }
    manager->next_refresh_ms = 0U;
    draw_task_manager(wm);
}

static int task_manager_selected_process_index(const TaskManager *manager)
{
    const Win31xTaskManagerSnapshot *snapshot =
        win31x_task_manager_data_snapshot(&manager->data);
    size_t index;

    if (snapshot == NULL)
        return -1;
    for (index = 0U; index < snapshot->process_count; ++index) {
        if (snapshot->processes[index].pid == manager->selected_pid &&
            snapshot->processes[index].start_time_ticks ==
                manager->selected_start_time)
            return index <= (size_t)INT_MAX ? (int)index : -1;
    }
    return -1;
}

static void task_manager_select_process_index(WindowManager *wm, int index)
{
    TaskManager *manager = &wm->task_manager;
    const Win31xTaskManagerSnapshot *snapshot =
        win31x_task_manager_data_snapshot(&manager->data);

    if (snapshot == NULL || snapshot->process_count == 0U) {
        manager->selected_pid = 0;
        manager->selected_start_time = 0U;
        return;
    }
    if (index < 0)
        index = 0;
    if ((size_t)index >= snapshot->process_count)
        index = snapshot->process_count > (size_t)INT_MAX
                    ? INT_MAX
                    : (int)snapshot->process_count - 1;
    manager->selected_pid = snapshot->processes[index].pid;
    manager->selected_start_time =
        snapshot->processes[index].start_time_ticks;
    manager->confirm_pid = 0;
    manager->confirm_start_time = 0U;
    manager->confirm_until_ms = 0U;
    if (index < manager->process_scroll)
        manager->process_scroll = index;
    if (index >= manager->process_scroll +
                     task_manager_list_visible_rows(manager))
        manager->process_scroll =
            index - task_manager_list_visible_rows(manager) + 1;
    task_manager_clamp_process_scroll(wm);
    task_manager_publish_state(wm);
}

static int desktop_menu_item_y(DesktopMenuItem item)
{
    switch (item) {
    case DESKTOP_MENU_LOCK:
        return 4;
    case DESKTOP_MENU_LOG_OUT:
        return 37;
    case DESKTOP_MENU_RESTART:
        return 63;
    case DESKTOP_MENU_SHUT_DOWN:
        return 89;
    default:
        return -1;
    }
}

static const char *desktop_menu_item_label(DesktopMenuItem item)
{
    switch (item) {
    case DESKTOP_MENU_LOCK:
        return "Lock";
    case DESKTOP_MENU_LOG_OUT:
        return "Log Out";
    case DESKTOP_MENU_RESTART:
        return "Restart";
    case DESKTOP_MENU_SHUT_DOWN:
        return "Shut Down";
    default:
        return "";
    }
}

static bool desktop_menu_item_enabled(const WindowManager *wm,
                                      DesktopMenuItem item)
{
    if (item == DESKTOP_MENU_LOCK)
        return wm->auto_lock.locker_available &&
               wm->auto_lock.direct_pid <= 0;
    if (item == DESKTOP_MENU_LOG_OUT)
        return true;
    if (item == DESKTOP_MENU_RESTART || item == DESKTOP_MENU_SHUT_DOWN)
        return wm->session_actions.available &&
               wm->session_actions.child_pid <= 0;
    return false;
}

static DesktopMenuItem desktop_menu_item_at(const WindowManager *wm,
                                            int root_x, int root_y)
{
    const DesktopMenu *menu = &wm->desktop_menu;
    int local_x = root_x - menu->x;
    int local_y = root_y - menu->y;
    int item;

    if (!menu->visible || local_x < 4 || local_x >= menu->width - 4)
        return DESKTOP_MENU_NONE;
    for (item = 0; item < DESKTOP_MENU_ITEM_COUNT; ++item) {
        int y = desktop_menu_item_y((DesktopMenuItem)item);

        if (local_y >= y && local_y < y + DESKTOP_MENU_ITEM_HEIGHT)
            return (DesktopMenuItem)item;
    }
    return DESKTOP_MENU_NONE;
}

static void draw_desktop_menu(WindowManager *wm)
{
    DesktopMenu *menu = &wm->desktop_menu;
    int item;

    if (menu->window == None)
        return;
    XSetForeground(wm->display, wm->gc, wm->theme.silver);
    XFillRectangle(wm->display, menu->window, wm->gc, 0, 0,
                   (unsigned)menu->width, (unsigned)menu->height);
    draw_bevel(wm, menu->window, 0, 0, menu->width, menu->height, false);
    if (menu->width > 15 && menu->height > 34) {
        XSetForeground(wm->display, wm->gc, wm->theme.dark_gray);
        XDrawLine(wm->display, menu->window, wm->gc, 7, 33,
                  menu->width - 8, 33);
        XSetForeground(wm->display, wm->gc, wm->theme.white);
        XDrawLine(wm->display, menu->window, wm->gc, 7, 34,
                  menu->width - 8, 34);
    }
    for (item = 0; item < DESKTOP_MENU_ITEM_COUNT; ++item) {
        DesktopMenuItem current = (DesktopMenuItem)item;
        bool enabled = desktop_menu_item_enabled(wm, current);
        bool selected = enabled && menu->selected == current;
        int y = desktop_menu_item_y(current);
        int row_height = DESKTOP_MENU_ITEM_HEIGHT;
        unsigned long text_color;

        if (menu->width <= 8 || y >= menu->height)
            continue;
        if (y + row_height > menu->height)
            row_height = menu->height - y;
        XSetForeground(wm->display, wm->gc,
                       selected ? wm->theme.active_title : wm->theme.silver);
        XFillRectangle(wm->display, menu->window, wm->gc, 4, y,
                       (unsigned)(menu->width - 8),
                       (unsigned)row_height);
        if (selected)
            text_color = wm->theme.white;
        else if (enabled)
            text_color = wm->theme.black;
        else
            text_color = wm->theme.dark_gray;
        if (menu->width > 22 && y + 18 < menu->height)
            draw_text(wm, menu->window, 15, y + 18, text_color,
                      desktop_menu_item_label(current));
    }
}

static void dismiss_desktop_menu(WindowManager *wm)
{
    DesktopMenu *menu = &wm->desktop_menu;

    if (menu->keyboard_grabbed) {
        XUngrabKeyboard(wm->display, CurrentTime);
        menu->keyboard_grabbed = false;
    }
    if (menu->pointer_grabbed) {
        XUngrabPointer(wm->display, CurrentTime);
        menu->pointer_grabbed = false;
    }
    if (menu->visible) {
        menu->visible = false;
        XUnmapWindow(wm->display, menu->window);
    }
    menu->selected = DESKTOP_MENU_NONE;
    menu->armed = DESKTOP_MENU_NONE;
    menu->pressed_button = 0U;
    menu->ignore_open_release = false;
}

static void show_desktop_menu(WindowManager *wm, int root_x, int root_y,
                              Time time)
{
    DesktopMenu *menu = &wm->desktop_menu;
    size_t monitor_index = monitor_index_for_point(wm, root_x, root_y);
    const MonitorGeometry *monitor = monitor_at(wm, monitor_index);
    int pointer_result;

    if (wm->session_confirmation.visible) {
        refocus_session_confirmation(wm, time);
        return;
    }
    set_active_monitor_from_point(wm, root_x, root_y);
    set_monitor_anchor(&menu->monitor, monitor);
    dismiss_desktop_menu(wm);
    menu->width = monitor->width < DESKTOP_MENU_WIDTH
                      ? monitor->width
                      : DESKTOP_MENU_WIDTH;
    menu->height = monitor->height < DESKTOP_MENU_HEIGHT
                       ? monitor->height
                       : DESKTOP_MENU_HEIGHT;
    if (menu->width < 1)
        menu->width = 1;
    if (menu->height < 1)
        menu->height = 1;
    menu->x = root_x;
    menu->y = root_y;
    if (menu->x + menu->width > monitor->x + monitor->width)
        menu->x = monitor->x + monitor->width - menu->width;
    if (menu->y + menu->height > monitor->y + monitor->height)
        menu->y = monitor->y + monitor->height - menu->height;
    if (menu->x < monitor->x)
        menu->x = monitor->x;
    if (menu->y < monitor->y)
        menu->y = monitor->y;
    menu->selected = DESKTOP_MENU_NONE;
    menu->armed = DESKTOP_MENU_NONE;
    menu->pressed_button = 0U;
    menu->ignore_open_release = true;
    XMoveResizeWindow(wm->display, menu->window, menu->x, menu->y,
                      (unsigned)menu->width, (unsigned)menu->height);
    XMapRaised(wm->display, menu->window);
    menu->visible = true;
    draw_desktop_menu(wm);
    pointer_result = XGrabPointer(
        wm->display, menu->window, False,
        ButtonPressMask | ButtonReleaseMask | PointerMotionMask,
        GrabModeAsync, GrabModeAsync, None, wm->arrow_cursor, CurrentTime);
    if (pointer_result != GrabSuccess) {
        dismiss_desktop_menu(wm);
        return;
    }
    menu->pointer_grabbed = true;
    if (XGrabKeyboard(wm->display, menu->window, False, GrabModeAsync,
                      GrabModeAsync, CurrentTime) == GrabSuccess)
        menu->keyboard_grabbed = true;
}

static const char *session_confirmation_title(DesktopMenuItem action)
{
    switch (action) {
    case DESKTOP_MENU_LOG_OUT:
        return "Confirm Log Out";
    case DESKTOP_MENU_RESTART:
        return "Confirm Restart";
    case DESKTOP_MENU_SHUT_DOWN:
        return "Confirm Shut Down";
    default:
        return "Confirm Session Action";
    }
}

static const char *session_confirmation_prompt(DesktopMenuItem action)
{
    switch (action) {
    case DESKTOP_MENU_LOG_OUT:
        return "Log out of this session now?";
    case DESKTOP_MENU_RESTART:
        return "Restart the computer now?";
    case DESKTOP_MENU_SHUT_DOWN:
        return "Shut down the computer now?";
    default:
        return "Continue with this session action?";
    }
}

static int session_confirmation_horizontal_margin(
    const SessionConfirmation *dialog)
{
    int full_width = SESSION_CONFIRM_BUTTON_WIDTH * 2 +
                     SESSION_CONFIRM_BUTTON_GAP +
                     SESSION_CONFIRM_BUTTON_MARGIN * 2;

    return dialog->width >= full_width ? SESSION_CONFIRM_BUTTON_MARGIN : 4;
}

static int session_confirmation_button_gap(const SessionConfirmation *dialog)
{
    return dialog->width >= 32 ? SESSION_CONFIRM_BUTTON_GAP : 2;
}

static int session_confirmation_button_width(
    const SessionConfirmation *dialog)
{
    int margin = session_confirmation_horizontal_margin(dialog);
    int available = dialog->width - margin * 2 -
                    session_confirmation_button_gap(dialog);
    int width = available / 2;

    if (width > SESSION_CONFIRM_BUTTON_WIDTH)
        width = SESSION_CONFIRM_BUTTON_WIDTH;
    return width > 0 ? width : 0;
}

static int session_confirmation_vertical_margin(
    const SessionConfirmation *dialog)
{
    int full_height = TITLE_HEIGHT + 8 + SESSION_CONFIRM_BUTTON_HEIGHT +
                      SESSION_CONFIRM_BUTTON_MARGIN;

    return dialog->height >= full_height ? SESSION_CONFIRM_BUTTON_MARGIN : 4;
}

static int session_confirmation_button_height(
    const SessionConfirmation *dialog)
{
    int margin = session_confirmation_vertical_margin(dialog);
    int height = dialog->height - margin - (TITLE_HEIGHT + 8);

    if (height > SESSION_CONFIRM_BUTTON_HEIGHT)
        height = SESSION_CONFIRM_BUTTON_HEIGHT;
    return height > 0 ? height : 0;
}

static int session_confirmation_button_y(const SessionConfirmation *dialog)
{
    int height = session_confirmation_button_height(dialog);
    int margin = session_confirmation_vertical_margin(dialog);

    return dialog->height - height - margin;
}

static int session_confirmation_no_button_x(const SessionConfirmation *dialog)
{
    return dialog->width -
           session_confirmation_horizontal_margin(dialog) -
           session_confirmation_button_width(dialog);
}

static int session_confirmation_yes_button_x(
    const SessionConfirmation *dialog)
{
    return session_confirmation_no_button_x(dialog) -
           session_confirmation_button_gap(dialog) -
           session_confirmation_button_width(dialog);
}

static bool session_confirmation_buttons_visible(
    const SessionConfirmation *dialog)
{
    int button_y = session_confirmation_button_y(dialog);
    int yes_x = session_confirmation_yes_button_x(dialog);
    int no_x = session_confirmation_no_button_x(dialog);
    int button_width = session_confirmation_button_width(dialog);
    int button_height = session_confirmation_button_height(dialog);

    return button_width >= 8 && button_height >= 8 && yes_x >= 0 &&
           no_x > yes_x && button_y >= TITLE_HEIGHT + 8;
}

static bool session_confirmation_close_visible(
    const SessionConfirmation *dialog)
{
    return dialog->width >= TITLE_BUTTON + 12 &&
           dialog->height >= 4 + TITLE_BUTTON;
}

static bool session_confirmation_close_at(const SessionConfirmation *dialog,
                                          int root_x, int root_y)
{
    int x = root_x - dialog->x;
    int y = root_y - dialog->y;

    return dialog->visible && session_confirmation_close_visible(dialog) &&
           point_in_rectangle(x, y,
                              internal_close_button_x(dialog->width), 4,
                              TITLE_BUTTON, TITLE_BUTTON);
}

static SessionConfirmButton session_confirmation_button_at(
    const SessionConfirmation *dialog, int root_x, int root_y)
{
    int x = root_x - dialog->x;
    int y = root_y - dialog->y;
    int button_y = session_confirmation_button_y(dialog);
    int button_width = session_confirmation_button_width(dialog);
    int button_height = session_confirmation_button_height(dialog);

    if (!dialog->visible || !session_confirmation_buttons_visible(dialog))
        return SESSION_CONFIRM_NONE;
    if (point_in_rectangle(x, y,
                           session_confirmation_yes_button_x(dialog),
                           button_y, button_width, button_height))
        return SESSION_CONFIRM_YES;
    if (point_in_rectangle(x, y,
                           session_confirmation_no_button_x(dialog),
                           button_y, button_width, button_height))
        return SESSION_CONFIRM_NO;
    return SESSION_CONFIRM_NONE;
}

static void position_session_confirmation(WindowManager *wm)
{
    SessionConfirmation *dialog = &wm->session_confirmation;
    size_t monitor_index = dialog->monitor.valid
                               ? monitor_index_for_anchor(wm,
                                                          &dialog->monitor)
                               : active_monitor_index(wm);
    const MonitorGeometry *monitor = monitor_at(wm, monitor_index);
    int available_width = monitor->width - 24;
    int available_height = monitor->height - 24;

    set_monitor_anchor(&dialog->monitor, monitor);
    dialog->width = available_width < SESSION_CONFIRM_DEFAULT_WIDTH
                        ? available_width
                        : SESSION_CONFIRM_DEFAULT_WIDTH;
    dialog->height = available_height < SESSION_CONFIRM_DEFAULT_HEIGHT
                         ? available_height
                         : SESSION_CONFIRM_DEFAULT_HEIGHT;
    if (dialog->width < 230)
        dialog->width = monitor->width > 0 ? monitor->width : 1;
    if (dialog->height < 120)
        dialog->height = monitor->height > 0 ? monitor->height : 1;
    dialog->x = monitor->x + (monitor->width - dialog->width) / 2;
    dialog->y = monitor->y + (monitor->height - dialog->height) / 2;
    clamp_geometry_to_monitor(monitor, &dialog->x, &dialog->y,
                              &dialog->width, &dialog->height);
    XMoveResizeWindow(wm->display, dialog->shield, 0, 0,
                      (unsigned)(wm->screen_width > 0 ? wm->screen_width : 1),
                      (unsigned)(wm->screen_height > 0 ? wm->screen_height
                                                       : 1));
    XMoveResizeWindow(wm->display, dialog->window, dialog->x, dialog->y,
                      (unsigned)dialog->width, (unsigned)dialog->height);
}

static void draw_session_confirmation(WindowManager *wm)
{
    SessionConfirmation *dialog = &wm->session_confirmation;
    const char *title = session_confirmation_title(dialog->action);
    const char *prompt = session_confirmation_prompt(dialog->action);
    int button_y = session_confirmation_button_y(dialog);
    int yes_x = session_confirmation_yes_button_x(dialog);
    int no_x = session_confirmation_no_button_x(dialog);
    int button_width = session_confirmation_button_width(dialog);
    int button_height = session_confirmation_button_height(dialog);
    char fitted[160];

    if (dialog->window == None)
        return;
    XSetForeground(wm->display, wm->gc, wm->theme.silver);
    XFillRectangle(wm->display, dialog->window, wm->gc, 0, 0,
                   (unsigned)dialog->width, (unsigned)dialog->height);
    draw_bevel(wm, dialog->window, 0, 0, dialog->width, dialog->height, false);
    if (dialog->width > 6) {
        XSetForeground(wm->display, wm->gc, wm->theme.active_title);
        XFillRectangle(wm->display, dialog->window, wm->gc, 3, 3,
                       (unsigned)(dialog->width - 6), TITLE_HEIGHT);
    }
    if (dialog->width >= 48) {
        draw_supplied_icon(wm, dialog->window, ICON_CATEGORY_HELP,
                           ICON_SIZE_SMALL, 7, 5);
        fitted_text(wm, title, dialog->width - 54, fitted, sizeof(fitted));
        draw_text(wm, dialog->window, 28, 17, wm->theme.white, fitted);
    }
    if (session_confirmation_close_visible(dialog))
        draw_title_button(wm, dialog->window,
                          internal_close_button_x(dialog->width), 4,
                          TITLE_GLYPH_CLOSE);

    if (dialog->width >= 280 && dialog->height >= 130) {
        draw_supplied_icon_centered(wm, dialog->window, ICON_CATEGORY_HELP,
                                    ICON_SIZE_LARGE, 17, 43, 42, 48);
        fitted_text(wm, prompt, dialog->width - 82, fitted, sizeof(fitted));
        draw_text(wm, dialog->window, 70, 64, wm->theme.black, fitted);
        fitted_text(wm, "Unsaved work may be lost.", dialog->width - 82,
                    fitted, sizeof(fitted));
        draw_text(wm, dialog->window, 70, 88, wm->theme.dark_gray, fitted);
    } else {
        int prompt_baseline = button_y - 8;

        if (prompt_baseline > 58)
            prompt_baseline = 58;
        if (prompt_baseline >= TITLE_HEIGHT + 8) {
            fitted_text(wm, prompt, dialog->width - 20, fitted,
                        sizeof(fitted));
            draw_centered_text(wm, dialog->window, fitted,
                               dialog->width / 2, prompt_baseline,
                               wm->theme.black, dialog->width - 20);
        }
    }

    if (session_confirmation_buttons_visible(dialog)) {
        bool yes_pressed = dialog->pressed_button == Button1 &&
                           dialog->armed == SESSION_CONFIRM_YES;
        bool no_pressed = dialog->pressed_button == Button1 &&
                          dialog->armed == SESSION_CONFIRM_NO;

        draw_button(wm, dialog->window, yes_x, button_y,
                    button_width, button_height, "Yes", yes_pressed, true);
        draw_button(wm, dialog->window, no_x, button_y,
                    button_width, button_height, "No", no_pressed, true);
        XSetForeground(wm->display, wm->gc, wm->theme.black);
        if (dialog->selected == SESSION_CONFIRM_YES)
            XDrawRectangle(wm->display, dialog->window, wm->gc, yes_x + 3,
                           button_y + 3, (unsigned)(button_width - 7),
                           (unsigned)(button_height - 7));
        else if (dialog->selected == SESSION_CONFIRM_NO)
            XDrawRectangle(wm->display, dialog->window, wm->gc, no_x + 3,
                           button_y + 3, (unsigned)(button_width - 7),
                           (unsigned)(button_height - 7));
    }
}

static void restore_session_confirmation_focus(WindowManager *wm,
                                               InternalFocus saved_internal,
                                               Window saved_client, Time time)
{
    Client *client;

    if (saved_internal == INTERNAL_FOCUS_APPLICATIONS &&
        wm->launcher_visible) {
        activate_internal_window(wm, INTERNAL_FOCUS_APPLICATIONS, time);
        return;
    }
    if (saved_internal == INTERNAL_FOCUS_CONTROL_PANEL &&
        wm->control_panel.visible) {
        activate_internal_window(wm, INTERNAL_FOCUS_CONTROL_PANEL, time);
        return;
    }
    if (saved_internal == INTERNAL_FOCUS_RUN && wm->run_dialog.visible) {
        activate_internal_window(wm, INTERNAL_FOCUS_RUN, time);
        return;
    }
    if (saved_internal == INTERNAL_FOCUS_TASK_MANAGER &&
        wm->task_manager.visible) {
        activate_internal_window(wm, INTERNAL_FOCUS_TASK_MANAGER, time);
        return;
    }
    client = client_for_client_window(wm, saved_client);
    if (client != NULL && !client->minimized) {
        focus_client(wm, client, time);
        return;
    }
    client = next_visible_client(wm, NULL);
    if (saved_client != None && client != NULL) {
        focus_client(wm, client, time);
        return;
    }
    wm->internal_focus = INTERNAL_FOCUS_NONE;
    change_active_client(wm, NULL, false);
    XSetInputFocus(wm->display, wm->root, RevertToPointerRoot, time);
}

static void dismiss_session_confirmation(WindowManager *wm, bool restore_focus,
                                         Time time)
{
    SessionConfirmation *dialog = &wm->session_confirmation;
    InternalFocus saved_internal = dialog->return_internal_focus;
    Window saved_client = dialog->return_client;

    if (dialog->visible) {
        dialog->visible = false;
        XUnmapWindow(wm->display, dialog->window);
        XUnmapWindow(wm->display, dialog->shield);
    }
    dialog->pressed_button = 0U;
    dialog->armed = SESSION_CONFIRM_NONE;
    dialog->selected = SESSION_CONFIRM_NO;
    dialog->action = DESKTOP_MENU_NONE;
    dialog->return_internal_focus = INTERNAL_FOCUS_NONE;
    dialog->return_client = None;
    dialog->monitor.valid = false;
    if (restore_focus)
        restore_session_confirmation_focus(wm, saved_internal, saved_client,
                                           time);
}

static void refocus_session_confirmation(WindowManager *wm, Time time)
{
    SessionConfirmation *dialog = &wm->session_confirmation;

    if (!dialog->visible)
        return;
    XRaiseWindow(wm->display, dialog->shield);
    XRaiseWindow(wm->display, dialog->window);
    XSetInputFocus(wm->display, dialog->window, RevertToPointerRoot, time);
}

static bool session_confirmation_should_yield_to_window(
    WindowManager *wm, Window window)
{
    SessionConfirmation *dialog = &wm->session_confirmation;
    const MonitorGeometry *monitor;
    XWindowAttributes attributes;
    long long right;
    long long bottom;

    if (window == None || window == dialog->window ||
        window == dialog->shield || window == wm->launcher ||
        window == wm->control_panel.window ||
        window == wm->run_dialog.window ||
        window == wm->task_manager.window ||
        window == wm->desktop_menu.window || window == wm->support_window ||
        !XGetWindowAttributes(wm->display, window, &attributes) ||
        !attributes.override_redirect || attributes.class != InputOutput ||
        attributes.map_state == IsUnmapped)
        return false;
    monitor = monitor_at(
        wm, monitor_index_for_anchor(wm, &dialog->monitor));
    if (monitor == NULL)
        return false;
    right = (long long)attributes.x + attributes.width +
            attributes.border_width * 2LL;
    bottom = (long long)attributes.y + attributes.height +
             attributes.border_width * 2LL;
    return attributes.x <= monitor->x && attributes.y <= monitor->y &&
           right >= monitor->x + monitor->width &&
           bottom >= monitor->y + monitor->height;
}

static void confirm_session_action(WindowManager *wm, Time time)
{
    DesktopMenuItem action = wm->session_confirmation.action;
    Win31xSessionAction system_action;

    if (!wm->session_confirmation.visible)
        return;
    if (!desktop_menu_item_enabled(wm, action)) {
        XBell(wm->display, 0);
        dismiss_session_confirmation(wm, true, time);
        return;
    }
    flush_desktop_state(wm, true);
    dismiss_session_confirmation(wm, action != DESKTOP_MENU_LOG_OUT, time);
    if (action == DESKTOP_MENU_LOG_OUT) {
        keep_running = 0;
        return;
    }
    system_action = action == DESKTOP_MENU_RESTART
                        ? WIN31X_SESSION_ACTION_RESTART
                        : WIN31X_SESSION_ACTION_SHUT_DOWN;
    if (win31x_session_action_start(&wm->session_actions, system_action) < 0)
        fprintf(stderr, "win31x: could not request the system action: %s\n",
                strerror(errno));
}

static void show_session_confirmation(WindowManager *wm,
                                      DesktopMenuItem action, Time time)
{
    SessionConfirmation *dialog = &wm->session_confirmation;

    if (action != DESKTOP_MENU_LOG_OUT && action != DESKTOP_MENU_RESTART &&
        action != DESKTOP_MENU_SHUT_DOWN)
        return;
    if (!desktop_menu_item_enabled(wm, action)) {
        XBell(wm->display, 0);
        return;
    }
    dismiss_session_confirmation(wm, false, time);
    if (wm->desktop_menu.monitor.valid)
        dialog->monitor = wm->desktop_menu.monitor;
    else
        set_monitor_anchor(&dialog->monitor,
                           monitor_at(wm, active_monitor_index(wm)));
    dialog->return_internal_focus = wm->internal_focus;
    dialog->return_client = wm->internal_focus == INTERNAL_FOCUS_NONE &&
                                    wm->active != NULL
                                ? wm->active->window
                                : None;
    dialog->action = action;
    dialog->selected = SESSION_CONFIRM_NO;
    dialog->armed = SESSION_CONFIRM_NONE;
    dialog->pressed_button = 0U;
    position_session_confirmation(wm);
    set_utf8_property(wm, dialog->window, wm->atoms.net_wm_name,
                      session_confirmation_title(action));
    XMapRaised(wm->display, dialog->shield);
    XMapRaised(wm->display, dialog->window);
    dialog->visible = true;
    draw_session_confirmation(wm);
    refocus_session_confirmation(wm, time);
}

static void handle_session_confirmation_button(WindowManager *wm,
                                               XButtonEvent *event)
{
    SessionConfirmation *dialog = &wm->session_confirmation;
    SessionConfirmButton button;

    if (event->button != Button1) {
        XBell(wm->display, 0);
        return;
    }
    button = session_confirmation_close_at(dialog, event->x_root,
                                           event->y_root)
                 ? SESSION_CONFIRM_CLOSE
                 : session_confirmation_button_at(dialog, event->x_root,
                                                  event->y_root);
    dialog->pressed_button = event->button;
    dialog->armed = button;
    if (button == SESSION_CONFIRM_YES || button == SESSION_CONFIRM_NO)
        dialog->selected = button;
    else if (button == SESSION_CONFIRM_NONE)
        XBell(wm->display, 0);
    draw_session_confirmation(wm);
}

static void handle_session_confirmation_button_release(WindowManager *wm,
                                                       XButtonEvent *event)
{
    SessionConfirmation *dialog = &wm->session_confirmation;
    SessionConfirmButton armed;
    SessionConfirmButton released;

    if (dialog->pressed_button == 0U ||
        event->button != dialog->pressed_button)
        return;
    armed = dialog->armed;
    released = session_confirmation_close_at(dialog, event->x_root,
                                             event->y_root)
                   ? SESSION_CONFIRM_CLOSE
                   : session_confirmation_button_at(
                         dialog, event->x_root, event->y_root);
    dialog->pressed_button = 0U;
    dialog->armed = SESSION_CONFIRM_NONE;
    if (armed == released && armed == SESSION_CONFIRM_YES) {
        confirm_session_action(wm, event->time);
        return;
    }
    if (armed == released && armed == SESSION_CONFIRM_NO) {
        dismiss_session_confirmation(wm, true, event->time);
        return;
    }
    if (armed == released && armed == SESSION_CONFIRM_CLOSE) {
        dismiss_session_confirmation(wm, true, event->time);
        return;
    }
    draw_session_confirmation(wm);
}

static void handle_session_confirmation_motion(WindowManager *wm,
                                               XMotionEvent *event)
{
    SessionConfirmation *dialog = &wm->session_confirmation;
    SessionConfirmButton button;

    if (dialog->pressed_button != 0U)
        return;
    button = session_confirmation_button_at(dialog, event->x_root,
                                            event->y_root);
    if (button != SESSION_CONFIRM_NONE && dialog->selected != button) {
        dialog->selected = button;
        draw_session_confirmation(wm);
    }
}

static bool handle_session_confirmation_key(WindowManager *wm, KeySym key,
                                            unsigned int state, Time time)
{
    SessionConfirmation *dialog = &wm->session_confirmation;

    if (!dialog->visible)
        return false;
    if (key == XK_Escape || key == XK_n || key == XK_N ||
        (key == XK_F4 && (state & Mod1Mask) != 0U)) {
        dismiss_session_confirmation(wm, true, time);
        return true;
    }
    if (key == XK_y || key == XK_Y) {
        confirm_session_action(wm, time);
        return true;
    }
    if (key == XK_Left || key == XK_Right || key == XK_Tab) {
        dialog->selected = dialog->selected == SESSION_CONFIRM_YES
                               ? SESSION_CONFIRM_NO
                               : SESSION_CONFIRM_YES;
        draw_session_confirmation(wm);
        return true;
    }
    if (key == XK_Return || key == XK_KP_Enter) {
        if (dialog->selected == SESSION_CONFIRM_YES)
            confirm_session_action(wm, time);
        else
            dismiss_session_confirmation(wm, true, time);
        return true;
    }
    XBell(wm->display, 0);
    return true;
}

static void initialize_session_confirmation(WindowManager *wm)
{
    SessionConfirmation *dialog = &wm->session_confirmation;
    XSetWindowAttributes attributes;
    XSetWindowAttributes shield_attributes;

    dialog->width = SESSION_CONFIRM_DEFAULT_WIDTH;
    dialog->height = SESSION_CONFIRM_DEFAULT_HEIGHT;
    dialog->action = DESKTOP_MENU_NONE;
    dialog->selected = SESSION_CONFIRM_NO;
    dialog->armed = SESSION_CONFIRM_NONE;
    shield_attributes.override_redirect = True;
    shield_attributes.event_mask = ButtonPressMask | ButtonReleaseMask |
                                   PointerMotionMask;
    shield_attributes.cursor = wm->arrow_cursor;
    dialog->shield = XCreateWindow(
        wm->display, wm->root, 0, 0,
        (unsigned)(wm->screen_width > 0 ? wm->screen_width : 1),
        (unsigned)(wm->screen_height > 0 ? wm->screen_height : 1), 0, 0,
        InputOnly, CopyFromParent,
        CWOverrideRedirect | CWEventMask | CWCursor, &shield_attributes);
    set_internal_role(wm, dialog->shield, "session-confirmation-shield");
    attributes.override_redirect = True;
    attributes.background_pixel = wm->theme.silver;
    attributes.event_mask = ExposureMask | ButtonPressMask |
                            ButtonReleaseMask | PointerMotionMask |
                            KeyPressMask;
    attributes.cursor = wm->arrow_cursor;
    dialog->window = XCreateWindow(
        wm->display, wm->root, 0, 0, (unsigned)dialog->width,
        (unsigned)dialog->height, 0, CopyFromParent, InputOutput,
        CopyFromParent, CWOverrideRedirect | CWBackPixel | CWEventMask |
                            CWCursor,
        &attributes);
    set_internal_role(wm, dialog->window, "session-confirmation");
    set_utf8_property(wm, dialog->window, wm->atoms.net_wm_name,
                      "Confirm Session Action");
    position_session_confirmation(wm);
}

static void execute_desktop_menu_item(WindowManager *wm,
                                      DesktopMenuItem item, Time time)
{
    if (!desktop_menu_item_enabled(wm, item)) {
        XBell(wm->display, 0);
        return;
    }
    dismiss_desktop_menu(wm);
    if (item == DESKTOP_MENU_LOCK) {
        if (win31x_auto_lock_lock_now(&wm->auto_lock) < 0)
            fprintf(stderr, "win31x: could not lock the session: %s\n",
                    strerror(errno));
        return;
    }
    show_session_confirmation(wm, item, time);
}

static void handle_desktop_menu_button(WindowManager *wm,
                                       XButtonEvent *event)
{
    DesktopMenuItem item;

    wm->desktop_menu.ignore_open_release = false;
    wm->desktop_menu.pressed_button = event->button;
    wm->desktop_menu.armed = DESKTOP_MENU_NONE;
    if (event->button != Button1)
        return;
    item = desktop_menu_item_at(wm, event->x_root, event->y_root);
    if (item != DESKTOP_MENU_NONE &&
        desktop_menu_item_enabled(wm, item)) {
        wm->desktop_menu.armed = item;
        wm->desktop_menu.selected = item;
        draw_desktop_menu(wm);
    }
}

static void handle_desktop_menu_button_release(WindowManager *wm,
                                               XButtonEvent *event)
{
    DesktopMenu *menu = &wm->desktop_menu;
    DesktopMenuItem released_item;
    DesktopMenuItem armed;

    if (event->button == Button3 && menu->ignore_open_release) {
        menu->ignore_open_release = false;
        return;
    }
    if (menu->pressed_button == 0U ||
        event->button != menu->pressed_button)
        return;
    armed = menu->armed;
    menu->pressed_button = 0U;
    menu->armed = DESKTOP_MENU_NONE;
    released_item = desktop_menu_item_at(wm, event->x_root, event->y_root);
    if (event->button == Button1 && armed != DESKTOP_MENU_NONE &&
        released_item == armed && desktop_menu_item_enabled(wm, armed)) {
        execute_desktop_menu_item(wm, armed, event->time);
        return;
    }
    dismiss_desktop_menu(wm);
}

static void handle_desktop_menu_motion(WindowManager *wm,
                                       XMotionEvent *event)
{
    DesktopMenuItem item = desktop_menu_item_at(
        wm, event->x_root, event->y_root);

    if (item != DESKTOP_MENU_NONE &&
        !desktop_menu_item_enabled(wm, item))
        item = DESKTOP_MENU_NONE;
    if (wm->desktop_menu.selected != item) {
        wm->desktop_menu.selected = item;
        draw_desktop_menu(wm);
    }
}

static DesktopMenuItem next_desktop_menu_item(const WindowManager *wm,
                                              int direction)
{
    int item = wm->desktop_menu.selected;
    int attempts;

    for (attempts = 0; attempts < DESKTOP_MENU_ITEM_COUNT; ++attempts) {
        if (item < 0)
            item = direction > 0 ? 0 : DESKTOP_MENU_ITEM_COUNT - 1;
        else
            item = (item + direction + DESKTOP_MENU_ITEM_COUNT) %
                   DESKTOP_MENU_ITEM_COUNT;
        if (desktop_menu_item_enabled(wm, (DesktopMenuItem)item))
            return (DesktopMenuItem)item;
    }
    return DESKTOP_MENU_NONE;
}

static bool handle_desktop_menu_key(WindowManager *wm, KeySym key, Time time)
{
    DesktopMenu *menu = &wm->desktop_menu;

    if (menu->pressed_button != 0U || menu->ignore_open_release) {
        menu->armed = DESKTOP_MENU_NONE;
        return true;
    }
    if (key == XK_Escape) {
        dismiss_desktop_menu(wm);
        return true;
    }
    if (key == XK_Up || key == XK_Down) {
        menu->selected = next_desktop_menu_item(
            wm, key == XK_Up ? -1 : 1);
        draw_desktop_menu(wm);
        return true;
    }
    if ((key == XK_Return || key == XK_KP_Enter) &&
        menu->selected != DESKTOP_MENU_NONE) {
        execute_desktop_menu_item(wm, menu->selected, time);
        return true;
    }
    dismiss_desktop_menu(wm);
    return false;
}

static void initialize_desktop_menu(WindowManager *wm)
{
    DesktopMenu *menu = &wm->desktop_menu;
    XSetWindowAttributes attributes;

    menu->width = DESKTOP_MENU_WIDTH;
    menu->height = DESKTOP_MENU_HEIGHT;
    menu->selected = DESKTOP_MENU_NONE;
    menu->armed = DESKTOP_MENU_NONE;
    attributes.override_redirect = True;
    attributes.background_pixel = wm->theme.silver;
    attributes.event_mask = ExposureMask | ButtonPressMask |
                            ButtonReleaseMask | PointerMotionMask |
                            KeyPressMask;
    attributes.cursor = wm->arrow_cursor;
    menu->window = XCreateWindow(
        wm->display, wm->root, 0, 0, (unsigned)menu->width,
        (unsigned)menu->height, 0, CopyFromParent, InputOutput,
        CopyFromParent, CWOverrideRedirect | CWBackPixel | CWEventMask |
                            CWCursor,
        &attributes);
    set_internal_role(wm, menu->window, "desktop-menu");
    set_utf8_property(wm, menu->window, wm->atoms.net_wm_name,
                      "Desktop Menu");
}

static void raise_focused_internal_window(WindowManager *wm)
{
    if (wm->internal_focus == INTERNAL_FOCUS_APPLICATIONS &&
        wm->launcher_visible)
        XRaiseWindow(wm->display, wm->launcher);
    else if (wm->internal_focus == INTERNAL_FOCUS_CONTROL_PANEL &&
             wm->control_panel.visible)
        XRaiseWindow(wm->display, wm->control_panel.window);
    else if (wm->internal_focus == INTERNAL_FOCUS_RUN &&
             wm->run_dialog.visible)
        XRaiseWindow(wm->display, wm->run_dialog.window);
    else if (wm->internal_focus == INTERNAL_FOCUS_TASK_MANAGER &&
             wm->task_manager.visible)
        XRaiseWindow(wm->display, wm->task_manager.window);
    if (wm->desktop_menu.visible)
        XRaiseWindow(wm->display, wm->desktop_menu.window);
    raise_drag_outline(wm);
    if (wm->session_confirmation.visible) {
        XRaiseWindow(wm->display, wm->session_confirmation.shield);
        XRaiseWindow(wm->display, wm->session_confirmation.window);
    }
}

static void initialize_ewmh(WindowManager *wm)
{
    Atom supported[] = {
        wm->atoms.net_supporting_wm_check,
        wm->atoms.net_wm_name,
        wm->atoms.net_wm_icon,
        wm->atoms.net_client_list,
        wm->atoms.net_active_window,
        wm->atoms.net_close_window,
        wm->atoms.net_frame_extents,
        wm->atoms.net_number_of_desktops,
        wm->atoms.net_current_desktop,
        wm->atoms.net_workarea,
        wm->atoms.net_wm_state,
        wm->atoms.net_wm_state_hidden,
        wm->atoms.net_wm_state_maximized_horz,
        wm->atoms.net_wm_state_maximized_vert
    };
    long support = (long)wm->support_window;
    long one = 1;
    long zero = 0;
    long workarea[4] = {0, 0, wm->screen_width, wm->screen_height};

    XChangeProperty(wm->display, wm->root, wm->atoms.net_supporting_wm_check,
                    XA_WINDOW, 32, PropModeReplace,
                    (unsigned char *)&support, 1);
    XChangeProperty(wm->display, wm->support_window,
                    wm->atoms.net_supporting_wm_check, XA_WINDOW, 32,
                    PropModeReplace, (unsigned char *)&support, 1);
    set_utf8_property(wm, wm->support_window, wm->atoms.net_wm_name, WIN31X_NAME);
    XChangeProperty(wm->display, wm->root, wm->atoms.net_supported, XA_ATOM, 32,
                    PropModeReplace, (unsigned char *)supported,
                    (int)(sizeof(supported) / sizeof(supported[0])));
    XChangeProperty(wm->display, wm->root, wm->atoms.net_number_of_desktops,
                    XA_CARDINAL, 32, PropModeReplace,
                    (unsigned char *)&one, 1);
    XChangeProperty(wm->display, wm->root, wm->atoms.net_current_desktop,
                    XA_CARDINAL, 32, PropModeReplace,
                    (unsigned char *)&zero, 1);
    XChangeProperty(wm->display, wm->root, wm->atoms.net_workarea, XA_CARDINAL,
                    32, PropModeReplace, (unsigned char *)workarea, 4);
    update_client_list(wm);
}

static void grab_shortcuts(WindowManager *wm)
{
    KeyCode tab = XKeysymToKeycode(wm->display, XK_Tab);
    KeyCode f2 = XKeysymToKeycode(wm->display, XK_F2);
    KeyCode f4 = XKeysymToKeycode(wm->display, XK_F4);
    KeyCode r = XKeysymToKeycode(wm->display, XK_r);
    KeyCode escape = XKeysymToKeycode(wm->display, XK_Escape);
    KeyCode num_lock = XKeysymToKeycode(wm->display, XK_Num_Lock);
    KeyCode scroll_lock = XKeysymToKeycode(wm->display, XK_Scroll_Lock);
    XModifierKeymap *mapping = XGetModifierMapping(wm->display);
    unsigned num_lock_mask = 0;
    unsigned scroll_lock_mask = 0;
    unsigned lock_combinations[8];
    size_t lock_combination_count = 0U;
    size_t index;
    int modifier;
    int key;

    wm->super_mask_count = 0U;
    if (mapping != NULL) {
        for (modifier = 0; modifier < 8; ++modifier) {
            for (key = 0; key < mapping->max_keypermod; ++key) {
                KeyCode code = mapping->modifiermap[
                    modifier * mapping->max_keypermod + key];
                KeySym symbol;
                unsigned int mask = 1U << modifier;
                size_t mask_index;

                if (num_lock != 0 && code == num_lock)
                    num_lock_mask = 1U << modifier;
                if (scroll_lock != 0 && code == scroll_lock)
                    scroll_lock_mask = 1U << modifier;
                if (code == 0)
                    continue;
                symbol = XkbKeycodeToKeysym(wm->display, code, 0, 0);
                if (symbol != XK_Super_L && symbol != XK_Super_R)
                    continue;
                for (mask_index = 0U; mask_index < wm->super_mask_count;
                     ++mask_index) {
                    if (wm->super_masks[mask_index] == mask)
                        break;
                }
                if (mask_index == wm->super_mask_count &&
                    wm->super_mask_count < sizeof(wm->super_masks) /
                                                   sizeof(wm->super_masks[0]))
                    wm->super_masks[wm->super_mask_count++] = mask;
            }
        }
        XFreeModifiermap(mapping);
    }
    if (wm->super_mask_count == 0U)
        wm->super_masks[wm->super_mask_count++] = Mod4Mask;
    wm->ignored_lock_mask = LockMask | num_lock_mask | scroll_lock_mask;
    {
        unsigned int bits;

        for (bits = 0U; bits < 8U; ++bits) {
            unsigned int combination =
                ((bits & 1U) != 0U ? LockMask : 0U) |
                ((bits & 2U) != 0U ? num_lock_mask : 0U) |
                ((bits & 4U) != 0U ? scroll_lock_mask : 0U);
            size_t existing;

            for (existing = 0U; existing < lock_combination_count;
                 ++existing) {
                if (lock_combinations[existing] == combination)
                    break;
            }
            if (existing == lock_combination_count)
                lock_combinations[lock_combination_count++] = combination;
        }
    }

    XUngrabKey(wm->display, AnyKey, AnyModifier, wm->root);
    for (index = 0; index < lock_combination_count; ++index) {
        unsigned modifiers = Mod1Mask | lock_combinations[index];
        size_t super_index;

        if (tab != 0)
            XGrabKey(wm->display, tab, modifiers, wm->root, True,
                     GrabModeAsync, GrabModeAsync);
        if (f2 != 0)
            XGrabKey(wm->display, f2, modifiers, wm->root, True,
                     GrabModeAsync, GrabModeAsync);
        if (f4 != 0)
            XGrabKey(wm->display, f4, modifiers, wm->root, True,
                     GrabModeAsync, GrabModeAsync);
        if (escape != 0)
            XGrabKey(wm->display, escape,
                     ControlMask | ShiftMask | lock_combinations[index],
                     wm->root, True, GrabModeAsync, GrabModeAsync);
        if (r == 0)
            continue;
        for (super_index = 0U; super_index < wm->super_mask_count;
             ++super_index) {
            XGrabKey(wm->display, r,
                     wm->super_masks[super_index] | lock_combinations[index],
                     wm->root, True, GrabModeAsync, GrabModeAsync);
        }
    }
}

static bool key_event_has_super(const WindowManager *wm,
                                const XKeyEvent *event)
{
    size_t index;
    unsigned int state = event->state & ~wm->ignored_lock_mask;

    for (index = 0U; index < wm->super_mask_count; ++index) {
        if (state == wm->super_masks[index])
            return true;
    }
    return false;
}

static void adopt_existing_windows(WindowManager *wm)
{
    Window root_return;
    Window parent_return;
    Window *children = NULL;
    unsigned int child_count = 0;
    unsigned int index;

    XGrabServer(wm->display);
    if (XQueryTree(wm->display, wm->root, &root_return, &parent_return,
                   &children, &child_count)) {
        for (index = 0; index < child_count; ++index) {
            XWindowAttributes attributes;
            long state;

            if (children[index] == wm->support_window ||
                !XGetWindowAttributes(wm->display, children[index], &attributes) ||
                attributes.override_redirect || attributes.class == InputOnly)
                continue;
            state = read_wm_state(wm, children[index]);
            if (attributes.map_state == IsViewable || state == NormalState ||
                state == IconicState)
                manage_window(wm, children[index], true);
        }
    }
    if (children != NULL)
        XFree(children);
    XUngrabServer(wm->display);
    XSync(wm->display, False);
}

static void handle_configure_request(WindowManager *wm, XConfigureRequestEvent *event)
{
    Client *client = client_for_client_window(wm, event->window);
    int old_width;
    int old_height;
    bool arranged;

    if (client == NULL) {
        XWindowChanges changes;

        changes.x = event->x;
        changes.y = event->y;
        changes.width = event->width;
        changes.height = event->height;
        changes.border_width = event->border_width;
        changes.sibling = event->above;
        changes.stack_mode = event->detail;
        XConfigureWindow(wm->display, event->window,
                         (unsigned)event->value_mask, &changes);
        return;
    }
    old_width = client->width;
    old_height = client->height;
    arranged = client->layout != CLIENT_LAYOUT_NORMAL;
    if (event->value_mask & CWBorderWidth)
        client->saved_border = (unsigned)event->border_width;
    if (!arranged) {
        if (event->value_mask & CWWidth)
            client->width = event->width;
        if (event->value_mask & CWHeight)
            client->height = event->height;
        constrain_client_size(wm, client, &client->width, &client->height);
        if (event->value_mask & CWX)
            client->x = inner_x_from_requested_outer(client, event->x);
        else
            client->x = inner_coordinate_after_gravity_resize(
                client->x, old_width, client->width,
                horizontal_gravity_factor(client->win_gravity), FRAME_LEFT);
        if (event->value_mask & CWY)
            client->y = inner_y_from_requested_outer(client, event->y);
        else
            client->y = inner_coordinate_after_gravity_resize(
                client->y, old_height, client->height,
                vertical_gravity_factor(client->win_gravity), FRAME_TOP);
        keep_client_on_screen(wm, client);
    } else {
        apply_client_layout_geometry(wm, client);
    }
    apply_client_geometry(wm, client);
    if (event->value_mask & (CWSibling | CWStackMode)) {
        XWindowChanges changes;
        unsigned mask = event->value_mask & (CWSibling | CWStackMode);
        Client *sibling = (mask & CWSibling) != 0
                              ? client_for_client_window(wm, event->above)
                              : NULL;
        Client *root = transient_family_root(wm, client);
        Client *preferred = client;

        memset(&changes, 0, sizeof(changes));
        if (sibling != NULL && root != NULL &&
            transient_family_root(wm, sibling) == root) {
            mask &= ~(unsigned)CWSibling;
        } else {
            changes.sibling = sibling != NULL ? sibling->frame : event->above;
        }
        changes.stack_mode = event->detail;
        if (root != NULL && mask != 0)
            XConfigureWindow(wm->display, root->frame, mask, &changes);
        if (root != NULL) {
            if (wm->active != NULL &&
                client_is_in_transient_subtree(wm, wm->active, root))
                preferred = wm->active;
            restack_transient_children(
                wm, root, preferred, managed_client_count(wm) + 1);
        }
    }
    if (wm->active == client)
        set_active_monitor_from_client(wm, client);
    raise_focused_internal_window(wm);
    if (event->value_mask & (CWSibling | CWStackMode))
        update_focus_overlays(wm);
    send_configure_notify(wm, client);
    if (!arranged &&
        (event->value_mask & (CWX | CWY | CWWidth | CWHeight)) != 0U)
        capture_client_placement(wm, client, false);
}

static int frame_resize_edges(Client *client, int x, int y)
{
    int edges = 0;
    int width = frame_width(client);
    int height = frame_height(client);

    if (x <= FRAME_LEFT + 1)
        edges |= EDGE_LEFT;
    if (x >= width - FRAME_RIGHT - 2)
        edges |= EDGE_RIGHT;
    if (y <= 2)
        edges |= EDGE_TOP;
    if (y >= height - FRAME_BOTTOM - 2)
        edges |= EDGE_BOTTOM;
    return edges;
}

static void clamp_drag_position(const WindowManager *wm, int width, int *x,
                                int *y)
{
    if (*x > wm->screen_width - 48)
        *x = wm->screen_width - 48;
    if (*x + width < 48)
        *x = 48 - width;
    if (*y < 0)
        *y = 0;
    if (*y > wm->screen_height - TITLE_HEIGHT)
        *y = wm->screen_height - TITLE_HEIGHT;
}

static void prepare_launcher_drag_restore(WindowManager *wm, int pointer_x,
                                          int pointer_y)
{
    int old_x = wm->launcher_x;
    int old_y = wm->launcher_y;
    int old_width = wm->launcher_width;
    int offset_x = pointer_x - old_x;
    int offset_y = pointer_y - old_y;
    int width = wm->launcher_restore_valid ? wm->launcher_restore_width
                                           : wm->launcher_width;
    int height = wm->launcher_restore_valid ? wm->launcher_restore_height
                                            : wm->launcher_height;
    int x;
    int y;

    if (offset_x < 0)
        offset_x = 0;
    if (offset_x > old_width)
        offset_x = old_width;
    if (offset_y < 0)
        offset_y = 0;
    if (offset_y >= TITLE_Y + TITLE_HEIGHT)
        offset_y = TITLE_Y + TITLE_HEIGHT - 1;
    x = pointer_x -
        (int)((long long)offset_x * width / (old_width > 0 ? old_width : 1));
    y = pointer_y - offset_y;
    clamp_internal_geometry(wm, &x, &y, &width, &height);
    wm->drag.start_root_x = pointer_x;
    wm->drag.start_root_y = pointer_y;
    wm->drag.start_x = x;
    wm->drag.start_y = y;
    wm->drag.start_width = width;
    wm->drag.start_height = height;
    wm->drag.pending_x = x;
    wm->drag.pending_y = y;
    wm->drag.pending_width = width;
    wm->drag.pending_height = height;
    wm->drag.arranged_restore_prepared = true;
}

static void prepare_control_panel_drag_restore(WindowManager *wm,
                                               int pointer_x, int pointer_y)
{
    ControlPanel *panel = &wm->control_panel;
    int old_x = panel->x;
    int old_y = panel->y;
    int old_width = panel->width;
    int offset_x = pointer_x - old_x;
    int offset_y = pointer_y - old_y;
    int width = panel->restore_valid ? panel->restore_width : panel->width;
    int height = panel->restore_valid ? panel->restore_height : panel->height;
    int x;
    int y;

    if (offset_x < 0)
        offset_x = 0;
    if (offset_x > old_width)
        offset_x = old_width;
    if (offset_y < 0)
        offset_y = 0;
    if (offset_y >= TITLE_Y + TITLE_HEIGHT)
        offset_y = TITLE_Y + TITLE_HEIGHT - 1;
    x = pointer_x -
        (int)((long long)offset_x * width / (old_width > 0 ? old_width : 1));
    y = pointer_y - offset_y;
    clamp_internal_geometry(wm, &x, &y, &width, &height);
    wm->drag.start_root_x = pointer_x;
    wm->drag.start_root_y = pointer_y;
    wm->drag.start_x = x;
    wm->drag.start_y = y;
    wm->drag.start_width = width;
    wm->drag.start_height = height;
    wm->drag.pending_x = x;
    wm->drag.pending_y = y;
    wm->drag.pending_width = width;
    wm->drag.pending_height = height;
    wm->drag.arranged_restore_prepared = true;
}

static void prepare_task_manager_drag_restore(WindowManager *wm,
                                              int pointer_x, int pointer_y)
{
    TaskManager *manager = &wm->task_manager;
    int old_x = manager->x;
    int old_y = manager->y;
    int old_width = manager->width;
    int offset_x = pointer_x - old_x;
    int offset_y = pointer_y - old_y;
    int width = manager->restore_valid ? manager->restore_width :
                                         manager->width;
    int height = manager->restore_valid ? manager->restore_height :
                                          manager->height;
    int x;
    int y;

    if (offset_x < 0)
        offset_x = 0;
    if (offset_x > old_width)
        offset_x = old_width;
    if (offset_y < 0)
        offset_y = 0;
    if (offset_y >= TITLE_Y + TITLE_HEIGHT)
        offset_y = TITLE_Y + TITLE_HEIGHT - 1;
    x = pointer_x -
        (int)((long long)offset_x * width / (old_width > 0 ? old_width : 1));
    y = pointer_y - offset_y;
    clamp_internal_geometry(wm, &x, &y, &width, &height);
    wm->drag.start_root_x = pointer_x;
    wm->drag.start_root_y = pointer_y;
    wm->drag.start_x = x;
    wm->drag.start_y = y;
    wm->drag.start_width = width;
    wm->drag.start_height = height;
    wm->drag.pending_x = x;
    wm->drag.pending_y = y;
    wm->drag.pending_width = width;
    wm->drag.pending_height = height;
    wm->drag.arranged_restore_prepared = true;
}

static void prepare_client_drag_restore(WindowManager *wm, Client *client,
                                        int pointer_x, int pointer_y)
{
    int old_frame_x = client->x - FRAME_LEFT;
    int old_frame_y = client->y - FRAME_TOP;
    int old_frame_width = frame_width(client);
    int offset_x = pointer_x - old_frame_x;
    int offset_y = pointer_y - old_frame_y;
    int width = client->restore_valid ? client->restore_width : client->width;
    int height = client->restore_valid ? client->restore_height : client->height;
    int new_frame_width;
    int frame_x;
    int frame_y;

    if (offset_x < 0)
        offset_x = 0;
    if (offset_x > old_frame_width)
        offset_x = old_frame_width;
    if (offset_y < 0)
        offset_y = 0;
    if (offset_y >= TITLE_Y + TITLE_HEIGHT)
        offset_y = TITLE_Y + TITLE_HEIGHT - 1;
    constrain_client_size(wm, client, &width, &height);
    new_frame_width = width + FRAME_LEFT + FRAME_RIGHT;
    frame_x = pointer_x -
              (int)((long long)offset_x * new_frame_width /
                    (old_frame_width > 0 ? old_frame_width : 1));
    frame_y = pointer_y - offset_y;
    clamp_drag_position(wm, new_frame_width, &frame_x, &frame_y);
    wm->drag.start_root_x = pointer_x;
    wm->drag.start_root_y = pointer_y;
    wm->drag.start_x = frame_x + FRAME_LEFT;
    wm->drag.start_y = frame_y + FRAME_TOP;
    wm->drag.start_width = width;
    wm->drag.start_height = height;
    wm->drag.pending_x = wm->drag.start_x;
    wm->drag.pending_y = wm->drag.start_y;
    wm->drag.pending_width = width;
    wm->drag.pending_height = height;
    wm->drag.arranged_restore_prepared = true;
}

static void start_drag(WindowManager *wm, DragKind kind, Client *client,
                       DesktopIcon *icon, int edges, int root_x, int root_y)
{
    if (wm->drag.kind != DRAG_NONE)
        cancel_drag(wm, CurrentTime);
    wm->drag.kind = kind;
    wm->drag.client = client;
    wm->drag.icon = icon;
    wm->drag.edges = edges;
    wm->drag.start_root_x = root_x;
    wm->drag.start_root_y = root_y;
    if (client != NULL) {
        wm->drag.start_x = client->x;
        wm->drag.start_y = client->y;
        wm->drag.start_width = client->width;
        wm->drag.start_height = client->height;
    } else if (kind == DRAG_MOVE_ICON && icon != NULL) {
        wm->drag.start_x = icon->x;
        wm->drag.start_y = icon->y;
        wm->drag.start_width = ICON_WIDTH;
        wm->drag.start_height = ICON_HEIGHT;
    } else if (kind == DRAG_MOVE_CONTROL_PANEL) {
        wm->drag.start_x = wm->control_panel.x;
        wm->drag.start_y = wm->control_panel.y;
        wm->drag.start_width = wm->control_panel.width;
        wm->drag.start_height = wm->control_panel.height;
    } else if (kind == DRAG_MOVE_RUN) {
        wm->drag.start_x = wm->run_dialog.x;
        wm->drag.start_y = wm->run_dialog.y;
        wm->drag.start_width = wm->run_dialog.width;
        wm->drag.start_height = wm->run_dialog.height;
    } else if (kind == DRAG_MOVE_TASK_MANAGER) {
        wm->drag.start_x = wm->task_manager.x;
        wm->drag.start_y = wm->task_manager.y;
        wm->drag.start_width = wm->task_manager.width;
        wm->drag.start_height = wm->task_manager.height;
    } else {
        wm->drag.start_x = wm->launcher_x;
        wm->drag.start_y = wm->launcher_y;
        wm->drag.start_width = wm->launcher_width;
        wm->drag.start_height = wm->launcher_height;
    }
    wm->drag.pending_x = wm->drag.start_x;
    wm->drag.pending_y = wm->drag.start_y;
    wm->drag.pending_width = wm->drag.start_width;
    wm->drag.pending_height = wm->drag.start_height;
    if (XGrabPointer(wm->display, wm->root, False,
                     PointerMotionMask | ButtonReleaseMask, GrabModeAsync,
                     GrabModeAsync, None,
                     kind == DRAG_RESIZE_CLIENT ? wm->resize_cursor : wm->move_cursor,
                     CurrentTime) != GrabSuccess) {
        memset(&wm->drag, 0, sizeof(wm->drag));
        return;
    }
    wm->drag.keyboard_grabbed =
        XGrabKeyboard(wm->display, wm->root, False, GrabModeAsync,
                      GrabModeAsync, CurrentTime) == GrabSuccess;
}

static void handle_frame_button(WindowManager *wm, Client *client,
                                XButtonEvent *event)
{
    int edges;

    if (event->button != Button1)
        return;
    focus_client(wm, client, event->time);
    if (event->y >= TITLE_Y && event->y < TITLE_Y + TITLE_HEIGHT) {
        if (event->x >= close_button_x(client)) {
            close_client(wm, client, event->time);
            return;
        }
        if (event->x >= maximize_button_x(client) &&
            event->x < maximize_button_x(client) + TITLE_BUTTON) {
            toggle_client_maximize(wm, client);
            return;
        }
        if (event->x >= minimize_button_x(client) &&
            event->x < minimize_button_x(client) + TITLE_BUTTON) {
            minimize_client(wm, client);
            return;
        }
        start_drag(wm, DRAG_MOVE_CLIENT, client, NULL, 0,
                   event->x_root, event->y_root);
        return;
    }
    edges = frame_resize_edges(client, event->x, event->y);
    if (edges != 0) {
        normalize_client_layout(wm, client);
        start_drag(wm, DRAG_RESIZE_CLIENT, client, NULL, edges,
                   event->x_root, event->y_root);
    }
}

static void handle_launcher_button(WindowManager *wm, XButtonEvent *event)
{
    int columns = launcher_columns(wm);

    if (event->button == Button4 || event->button == Button5) {
        wm->launcher_scroll_row += event->button == Button4 ? -1 : 1;
        clamp_launcher_scroll(wm);
        draw_launcher(wm);
        return;
    }
    if (event->button != Button1)
        return;
    if (event->y < 25 &&
        event->x >= internal_close_button_x(wm->launcher_width)) {
        hide_launcher(wm);
        return;
    }
    if (event->y < 25 &&
        event->x >= internal_maximize_button_x(wm->launcher_width) &&
        event->x < internal_maximize_button_x(wm->launcher_width) +
                       TITLE_BUTTON) {
        toggle_launcher_maximize(wm);
        return;
    }
    if (event->y < 25) {
        start_drag(wm, DRAG_MOVE_LAUNCHER, NULL, NULL, 0,
                   event->x_root, event->y_root);
        return;
    }
    if (event->x >= 10 && event->y >= 34 &&
        event->y < wm->launcher_height - 29) {
        int column = (event->x - 10) / LAUNCHER_CELL_WIDTH;
        int row = (event->y - 34) / LAUNCHER_CELL_HEIGHT;
        int index;

        if (column < 0 || column >= columns || row < 0 ||
            row >= launcher_visible_rows(wm))
            return;
        index = (wm->launcher_scroll_row + row) * columns + column;
        if (index < 0 || index >= (int)wm->applications.len)
            return;
        wm->launcher_selected = index;
        draw_launcher(wm);
        if (wm->launcher_last_click == index &&
            event->time - wm->launcher_last_click_time <= DOUBLE_CLICK_MS) {
            launch_selected_application(wm);
            return;
        }
        wm->launcher_last_click = index;
        wm->launcher_last_click_time = event->time;
    }
}

static unsigned int adjacent_lock_timeout(unsigned int current, int direction)
{
    static const unsigned int choices[] = {1U, 5U, 10U, 15U, 30U, 60U, 120U};
    size_t index;

    if (direction < 0) {
        for (index = sizeof(choices) / sizeof(choices[0]); index > 0U;
             --index) {
            if (choices[index - 1U] < current)
                return choices[index - 1U];
        }
        return choices[0];
    }
    for (index = 0U; index < sizeof(choices) / sizeof(choices[0]); ++index) {
        if (choices[index] > current)
            return choices[index];
    }
    return choices[sizeof(choices) / sizeof(choices[0]) - 1U];
}

static void start_wifi_scan(WindowManager *wm)
{
    if (wifi_backend_busy(&wm->wifi))
        return;
    clear_control_password(&wm->control_panel);
    wm->control_panel.wifi_selected = -1;
    wm->control_panel.wifi_scroll = 0;
    (void)wifi_backend_start_scan(&wm->wifi);
    draw_control_panel(wm);
}

static void activate_selected_wifi(WindowManager *wm)
{
    ControlPanel *panel = &wm->control_panel;
    const WifiNetwork *network = selected_wifi_network(wm);

    if (network == NULL || wifi_backend_busy(&wm->wifi))
        return;
    if (network->active) {
        (void)wifi_backend_start_disconnect(&wm->wifi, network->device);
    } else if (wifi_backend_network_supported(network)) {
        (void)wifi_backend_start_connect(
            &wm->wifi, network,
            (const unsigned char *)panel->password,
            panel->password_length);
    }
    clear_control_password(panel);
    draw_control_panel(wm);
}

static void update_auto_lock_settings(WindowManager *wm, bool enabled,
                                      unsigned int minutes)
{
    wm->settings.auto_lock_enabled = enabled;
    wm->settings.auto_lock_minutes = minutes;
    (void)win31x_auto_lock_configure(&wm->auto_lock, enabled, minutes);
    save_settings(wm);
    draw_control_panel(wm);
}

static void handle_control_panel_button(WindowManager *wm,
                                        XButtonEvent *event)
{
    ControlPanel *panel = &wm->control_panel;
    int section;

    if (panel->section == CONTROL_SECTION_WIFI &&
        (event->button == Button4 || event->button == Button5)) {
        if (!control_wifi_layout_available(panel)) {
            clear_control_password(panel);
            return;
        }
        panel->wifi_scroll += event->button == Button4 ? -1 : 1;
        clamp_wifi_selection(wm);
        draw_control_panel(wm);
        return;
    }
    if (event->button != Button1)
        return;
    if (event->y < 25 &&
        event->x >= internal_close_button_x(panel->width)) {
        hide_control_panel(wm);
        return;
    }
    if (event->y < 25 &&
        event->x >= internal_maximize_button_x(panel->width) &&
        event->x < internal_maximize_button_x(panel->width) + TITLE_BUTTON) {
        toggle_control_panel_maximize(wm);
        return;
    }
    if (event->y < 25) {
        start_drag(wm, DRAG_MOVE_CONTROL_PANEL, NULL, NULL, 0,
                   event->x_root, event->y_root);
        return;
    }
    for (section = 0; section < CONTROL_SECTION_COUNT; ++section) {
        if (point_in_rectangle(event->x, event->y, 7,
                               control_nav_item_y(panel, section),
                               CONTROL_PANEL_NAV_WIDTH - 7,
                               control_nav_item_height(panel))) {
            panel->section = (ControlSection)section;
            wm->settings.control_panel_section =
                (Win31xControlPanelSection)panel->section;
            save_settings(wm);
            clear_control_password(panel);
            if (panel->section == CONTROL_SECTION_WIFI)
                start_wifi_scan(wm);
            draw_control_panel(wm);
            return;
        }
    }
    if (panel->section == CONTROL_SECTION_COLORS) {
        int content_x = control_content_x();
        int row_width = control_content_width(panel) - 30;
        size_t index;

        for (index = 0U; index < WIN31X_COLOR_SCHEME_COUNT; ++index) {
            int y = control_color_row_y(panel, index);

            if (point_in_rectangle(event->x, event->y, content_x + 15, y,
                                   row_width,
                                   control_color_row_height(panel))) {
                apply_color_scheme(wm, index, true);
                return;
            }
        }
    } else if (panel->section == CONTROL_SECTION_WIFI) {
        int content_x = control_content_x();
        int content_width = control_content_width(panel);
        int rows = wifi_visible_rows(panel);
        int list_y = wifi_list_y();
        int password_y = wifi_password_y(panel);
        int action_y = wifi_action_y(panel);
        const WifiNetwork *selected;

        if (!control_wifi_layout_available(panel)) {
            clear_control_password(panel);
            return;
        }
        if (!wifi_backend_busy(&wm->wifi) &&
            point_in_rectangle(event->x, event->y,
                               content_x + content_width - 100, 39, 82, 30)) {
            start_wifi_scan(wm);
            return;
        }
        if (point_in_rectangle(event->x, event->y, content_x + 15, list_y,
                               content_width - 30, rows * 31 + 2)) {
            int row = (event->y - list_y - 2) / 31;
            int index = panel->wifi_scroll + row;

            if (row >= 0 && row < rows &&
                wifi_backend_network_at(&wm->wifi, (size_t)index) != NULL) {
                clear_control_password(panel);
                panel->wifi_selected = index;
                draw_control_panel(wm);
            }
            return;
        }
        selected = selected_wifi_network(wm);
        if (wifi_network_needs_password(selected) && !selected->active &&
            point_in_rectangle(event->x, event->y, content_x + 15,
                               password_y, content_width - 32, 30)) {
            panel->password_active = true;
            draw_control_panel(wm);
            return;
        }
        if (point_in_rectangle(event->x, event->y, content_x + 15,
                               action_y, 145, 34)) {
            activate_selected_wifi(wm);
            return;
        }
        panel->password_active = false;
        draw_control_panel(wm);
    } else if (panel->section == CONTROL_SECTION_AUTO_LOCK) {
        ControlAutoLayout layout;

        control_auto_layout(panel, &layout);
        if (wm->auto_lock.available &&
            point_in_rectangle(event->x, event->y, layout.toggle_x,
                               layout.toggle_y, layout.toggle_width,
                               layout.toggle_height)) {
            update_auto_lock_settings(
                wm, !wm->settings.auto_lock_enabled,
                wm->settings.auto_lock_minutes);
            return;
        }
        if (wm->auto_lock.available &&
            point_in_rectangle(event->x, event->y, layout.timeout_minus_x,
                               layout.timeout_y,
                               layout.timeout_button_width,
                               layout.timeout_height)) {
            update_auto_lock_settings(
                wm, wm->settings.auto_lock_enabled,
                adjacent_lock_timeout(wm->settings.auto_lock_minutes, -1));
            return;
        }
        if (wm->auto_lock.available &&
            point_in_rectangle(event->x, event->y, layout.timeout_plus_x,
                               layout.timeout_y,
                               layout.timeout_button_width,
                               layout.timeout_height)) {
            update_auto_lock_settings(
                wm, wm->settings.auto_lock_enabled,
                adjacent_lock_timeout(wm->settings.auto_lock_minutes, 1));
            return;
        }
        if (wm->auto_lock.locker_available &&
            wm->auto_lock.direct_pid <= 0 &&
            point_in_rectangle(event->x, event->y, layout.lock_x,
                               layout.lock_y, layout.lock_width,
                               layout.lock_height)) {
            (void)win31x_auto_lock_lock_now(&wm->auto_lock);
            draw_control_panel(wm);
            return;
        }
    }
}

static void handle_run_button(WindowManager *wm, XButtonEvent *event)
{
    RunDialog *dialog = &wm->run_dialog;

    if (event->button != Button1)
        return;
    if (dialog->width >= TITLE_BUTTON + 12 && event->y < 25 &&
        event->x >= internal_close_button_x(dialog->width)) {
        hide_run_dialog(wm);
        return;
    }
    if (event->y < 25) {
        start_drag(wm, DRAG_MOVE_RUN, NULL, NULL, 0, event->x_root,
                   event->y_root);
        return;
    }
    if (dialog->width >= 260 && dialog->height >= 140 &&
        point_in_rectangle(event->x, event->y, run_open_button_x(dialog),
                           run_button_y(dialog), 78, 26)) {
        execute_run_command(wm);
        return;
    }
    if (dialog->width >= 260 && dialog->height >= 140 &&
        point_in_rectangle(event->x, event->y, run_cancel_button_x(dialog),
                           run_button_y(dialog), 78, 26))
        hide_run_dialog(wm);
}

static void task_manager_set_tab(WindowManager *wm, TaskManagerTab tab)
{
    TaskManager *manager = &wm->task_manager;

    if (tab < 0 || tab >= TASK_MANAGER_TAB_COUNT)
        return;
    manager->tab = tab;
    manager->status[0] = '\0';
    manager->status_is_refresh_error = false;
    manager->status_is_closing_application = false;
    manager->confirm_pid = 0;
    manager->confirm_start_time = 0U;
    manager->confirm_until_ms = 0U;
    manager->process_delete_down = false;
    if (tab == TASK_MANAGER_TAB_APPLICATIONS)
        task_manager_select_first_application(wm);
    if (tab == TASK_MANAGER_TAB_PROCESSES &&
        task_manager_selected_process(manager) == NULL)
        task_manager_select_process_index(wm, 0);
    task_manager_publish_state(wm);
    draw_task_manager(wm);
}

static void handle_task_manager_button(WindowManager *wm,
                                       XButtonEvent *event)
{
    TaskManager *manager = &wm->task_manager;
    int tab;

    if (event->button == Button4 || event->button == Button5) {
        int direction = event->button == Button4 ? -1 : 1;

        if (manager->tab == TASK_MANAGER_TAB_APPLICATIONS) {
            manager->application_scroll += direction;
            task_manager_clamp_application_scroll(wm);
            draw_task_manager(wm);
        } else if (manager->tab == TASK_MANAGER_TAB_PROCESSES) {
            manager->process_scroll += direction * 3;
            task_manager_clamp_process_scroll(wm);
            draw_task_manager(wm);
        }
        return;
    }
    if (event->button != Button1)
        return;
    if (event->y < 25 &&
        event->x >= internal_close_button_x(manager->width)) {
        hide_task_manager(wm);
        return;
    }
    if (event->y < 25 &&
        event->x >= internal_maximize_button_x(manager->width) &&
        event->x < internal_maximize_button_x(manager->width) +
                       TITLE_BUTTON) {
        toggle_task_manager_maximize(wm);
        return;
    }
    if (event->y < 25) {
        start_drag(wm, DRAG_MOVE_TASK_MANAGER, NULL, NULL, 0,
                   event->x_root, event->y_root);
        return;
    }
    for (tab = 0; tab < TASK_MANAGER_TAB_COUNT; ++tab) {
        if (point_in_rectangle(
                event->x, event->y,
                task_manager_tab_x(manager, (TaskManagerTab)tab), 49,
                task_manager_tab_width(manager, (TaskManagerTab)tab),
                TASK_MANAGER_TAB_HEIGHT + 2)) {
            task_manager_set_tab(wm, (TaskManagerTab)tab);
            return;
        }
    }
    if (manager->tab == TASK_MANAGER_TAB_APPLICATIONS) {
        int list_y = task_manager_content_top() + 29;
        int rows = task_manager_list_visible_rows(manager);
        int button_x;
        int button_width;

        if (point_in_rectangle(event->x, event->y, 11, list_y,
                               manager->width - 22,
                               rows * TASK_MANAGER_ROW_HEIGHT)) {
            int row = (event->y - list_y) / TASK_MANAGER_ROW_HEIGHT;
            Client *client = task_manager_application_at(
                wm, (size_t)(manager->application_scroll + row));

            if (client != NULL) {
                manager->selected_application = client->window;
                manager->status[0] = '\0';
                manager->status_is_refresh_error = false;
                manager->status_is_closing_application = false;
                draw_task_manager(wm);
                if (manager->application_last_click == client->window &&
                    event->time - manager->application_last_click_time <=
                        DOUBLE_CLICK_MS) {
                    task_manager_switch_to_application(wm, event->time);
                    return;
                }
                manager->application_last_click = client->window;
                manager->application_last_click_time = event->time;
            }
            return;
        }
        if (!task_manager_actions_visible(manager))
            return;
        task_manager_application_button_geometry(manager, 0, &button_x,
                                                 &button_width);
        if (point_in_rectangle(event->x, event->y, button_x,
                               task_manager_action_y(manager), button_width,
                               26)) {
            show_run_dialog(wm);
            return;
        }
        task_manager_application_button_geometry(manager, 1, &button_x,
                                                 &button_width);
        if (point_in_rectangle(event->x, event->y, button_x,
                               task_manager_action_y(manager), button_width,
                               26)) {
            task_manager_switch_to_application(wm, event->time);
            return;
        }
        task_manager_application_button_geometry(manager, 2, &button_x,
                                                 &button_width);
        if (point_in_rectangle(event->x, event->y, button_x,
                               task_manager_action_y(manager), button_width,
                               26)) {
            task_manager_end_application(wm, event->time);
            return;
        }
    } else if (manager->tab == TASK_MANAGER_TAB_PROCESSES) {
        const Win31xTaskManagerSnapshot *snapshot =
            win31x_task_manager_data_snapshot(&manager->data);
        int list_y = task_manager_content_top() + 29;
        int rows = task_manager_list_visible_rows(manager);
        int button_x;
        int button_width;

        if (point_in_rectangle(event->x, event->y, 11, list_y,
                               manager->width - 22,
                               rows * TASK_MANAGER_ROW_HEIGHT)) {
            int row = (event->y - list_y) / TASK_MANAGER_ROW_HEIGHT;
            size_t process_index =
                (size_t)(manager->process_scroll + row);

            if (snapshot == NULL || process_index >= snapshot->process_count) {
                return;
            }
            task_manager_select_process_index(wm, (int)process_index);
            manager->status[0] = '\0';
            manager->status_is_refresh_error = false;
            manager->status_is_closing_application = false;
            draw_task_manager(wm);
            return;
        }
        if (!task_manager_actions_visible(manager))
            return;
        task_manager_process_button_geometry(manager, 0, &button_x,
                                             &button_width);
        if (point_in_rectangle(event->x, event->y, button_x,
                               task_manager_action_y(manager), button_width,
                               26)) {
            manager->status[0] = '\0';
            manager->status_is_refresh_error = false;
            manager->status_is_closing_application = false;
            manager->confirm_pid = 0;
            manager->confirm_start_time = 0U;
            manager->confirm_until_ms = 0U;
            manager->process_delete_down = false;
            manager->next_refresh_ms = 0U;
            refresh_task_manager(wm, true);
            return;
        }
        task_manager_process_button_geometry(manager, 1, &button_x,
                                             &button_width);
        if (point_in_rectangle(event->x, event->y, button_x,
                               task_manager_action_y(manager), button_width,
                               26)) {
            task_manager_request_end_process(wm);
            return;
        }
    }
}

static void activate_desktop_icon(WindowManager *wm, DesktopIcon *icon,
                                  Time time)
{
    size_t monitor_index;

    if (icon == NULL)
        return;
    monitor_index = monitor_index_for_anchor(wm, &icon->monitor);
    set_active_monitor(wm, monitor_index);
    if (icon->kind == ICON_APPLICATIONS)
        show_launcher_on_monitor(wm, monitor_index);
    else if (icon->kind == ICON_CONTROL_PANEL)
        show_control_panel_on_monitor(wm, monitor_index);
    else
        restore_client(wm, icon->client, time);
}

static void handle_button_press(WindowManager *wm, XButtonEvent *event)
{
    DesktopIcon *icon;
    Client *client;

    if (wm->session_confirmation.visible) {
        if (event->send_event) {
            refocus_session_confirmation(wm, event->time);
            return;
        }
        handle_session_confirmation_button(wm, event);
        return;
    }
    if (wm->desktop_menu.visible) {
        handle_desktop_menu_button(wm, event);
        return;
    }
    if (event->window == wm->root && event->subwindow == None &&
        event->button == Button3) {
        show_desktop_menu(wm, event->x_root, event->y_root, event->time);
        return;
    }
    icon = icon_for_window(wm, event->window);
    if (icon != NULL && event->button == Button1) {
        if (event->send_event) {
            activate_desktop_icon(wm, icon, event->time);
            return;
        }
        start_drag(wm, DRAG_MOVE_ICON, NULL, icon, 0, event->x_root,
                   event->y_root);
        return;
    }
    if (event->window == wm->launcher) {
        activate_internal_window(wm, INTERNAL_FOCUS_APPLICATIONS, event->time);
        handle_launcher_button(wm, event);
        return;
    }
    if (event->window == wm->control_panel.window) {
        activate_internal_window(wm, INTERNAL_FOCUS_CONTROL_PANEL, event->time);
        handle_control_panel_button(wm, event);
        return;
    }
    if (event->window == wm->run_dialog.window) {
        remember_run_return_focus(wm);
        activate_internal_window(wm, INTERNAL_FOCUS_RUN, event->time);
        handle_run_button(wm, event);
        return;
    }
    if (event->window == wm->task_manager.window) {
        remember_task_manager_return_focus(wm);
        activate_internal_window(wm, INTERNAL_FOCUS_TASK_MANAGER,
                                 event->time);
        handle_task_manager_button(wm, event);
        return;
    }
    client = client_for_window(wm, event->window);
    if (client == NULL)
        return;
    if (event->window == client->focus_overlay) {
        /* Keep the passive grab alive until ReplayPointer is processed.  The
         * overlay must no longer be the pointer target, though, so lower it
         * behind the client before replaying the original press. */
        XLowerWindow(wm->display, client->focus_overlay);
        XAllowEvents(wm->display, ReplayPointer, event->time);
        focus_client(wm, client, event->time);
    } else if (event->window == client->frame) {
        handle_frame_button(wm, client, event);
    }
}

static void handle_motion(WindowManager *wm, XMotionEvent *event)
{
    int dx;
    int dy;

    if (wm->session_confirmation.visible) {
        if (event->send_event) {
            refocus_session_confirmation(wm, event->time);
            return;
        }
        handle_session_confirmation_motion(wm, event);
        return;
    }
    if (wm->desktop_menu.visible) {
        handle_desktop_menu_motion(wm, event);
        return;
    }
    if (wm->drag.kind == DRAG_NONE)
        return;
    if (wm->drag.kind == DRAG_MOVE_LAUNCHER &&
        wm->launcher_layout != CLIENT_LAYOUT_NORMAL &&
        !wm->drag.arranged_restore_prepared) {
        prepare_launcher_drag_restore(wm, event->x_root, event->y_root);
    } else if (wm->drag.kind == DRAG_MOVE_CONTROL_PANEL &&
               wm->control_panel.layout != CLIENT_LAYOUT_NORMAL &&
               !wm->drag.arranged_restore_prepared) {
        prepare_control_panel_drag_restore(wm, event->x_root, event->y_root);
    } else if (wm->drag.kind == DRAG_MOVE_TASK_MANAGER &&
               wm->task_manager.layout != CLIENT_LAYOUT_NORMAL &&
               !wm->drag.arranged_restore_prepared) {
        prepare_task_manager_drag_restore(wm, event->x_root, event->y_root);
    } else if (wm->drag.kind == DRAG_MOVE_CLIENT && wm->drag.client != NULL &&
               wm->drag.client->layout != CLIENT_LAYOUT_NORMAL &&
               !wm->drag.arranged_restore_prepared) {
        prepare_client_drag_restore(wm, wm->drag.client, event->x_root,
                                    event->y_root);
    }
    dx = event->x_root - wm->drag.start_root_x;
    dy = event->y_root - wm->drag.start_root_y;
    if (wm->drag.kind == DRAG_MOVE_ICON) {
        const MonitorGeometry *monitor;
        int x;
        int y;
        int width = ICON_WIDTH;
        int height = ICON_HEIGHT;

        if (!wm->drag.moved && abs(dx) < ICON_DRAG_SLOP &&
            abs(dy) < ICON_DRAG_SLOP)
            return;
        wm->drag.moved = true;
        x = wm->drag.start_x + dx;
        y = wm->drag.start_y + dy;
        monitor = monitor_at(
            wm, monitor_index_for_point(wm, event->x_root, event->y_root));
        if (monitor != NULL)
            clamp_geometry_to_monitor(monitor, &x, &y, &width, &height);
        wm->drag.pending_x = x;
        wm->drag.pending_y = y;
        show_drag_outline(wm, x, y, ICON_WIDTH, ICON_HEIGHT);
        return;
    }
    wm->drag.moved = true;
    if (wm->drag.kind == DRAG_MOVE_LAUNCHER) {
        int x = wm->drag.start_x + dx;
        int y = wm->drag.start_y + dy;

        clamp_drag_position(wm, wm->drag.start_width, &x, &y);
        wm->drag.pending_x = x;
        wm->drag.pending_y = y;
        show_drag_outline(wm, x, y, wm->drag.pending_width,
                          wm->drag.pending_height);
        return;
    }
    if (wm->drag.kind == DRAG_MOVE_CONTROL_PANEL) {
        int x = wm->drag.start_x + dx;
        int y = wm->drag.start_y + dy;

        clamp_drag_position(wm, wm->drag.start_width, &x, &y);
        wm->drag.pending_x = x;
        wm->drag.pending_y = y;
        show_drag_outline(wm, x, y, wm->drag.pending_width,
                          wm->drag.pending_height);
        return;
    }
    if (wm->drag.kind == DRAG_MOVE_RUN) {
        int x = wm->drag.start_x + dx;
        int y = wm->drag.start_y + dy;

        clamp_drag_position(wm, wm->drag.start_width, &x, &y);
        wm->drag.pending_x = x;
        wm->drag.pending_y = y;
        show_drag_outline(wm, x, y, wm->drag.pending_width,
                          wm->drag.pending_height);
        return;
    }
    if (wm->drag.kind == DRAG_MOVE_TASK_MANAGER) {
        int x = wm->drag.start_x + dx;
        int y = wm->drag.start_y + dy;

        clamp_drag_position(wm, wm->drag.start_width, &x, &y);
        wm->drag.pending_x = x;
        wm->drag.pending_y = y;
        show_drag_outline(wm, x, y, wm->drag.pending_width,
                          wm->drag.pending_height);
        return;
    }
    if (wm->drag.client == NULL)
        return;
    if (wm->drag.kind == DRAG_MOVE_CLIENT) {
        int frame_x = wm->drag.start_x + dx - FRAME_LEFT;
        int frame_y = wm->drag.start_y + dy - FRAME_TOP;
        int outer_width = wm->drag.start_width + FRAME_LEFT + FRAME_RIGHT;
        int outer_height = wm->drag.start_height + FRAME_TOP + FRAME_BOTTOM;

        clamp_drag_position(wm, outer_width, &frame_x, &frame_y);
        wm->drag.pending_x = frame_x + FRAME_LEFT;
        wm->drag.pending_y = frame_y + FRAME_TOP;
        show_drag_outline(wm, frame_x, frame_y, outer_width, outer_height);
        return;
    } else if (wm->drag.kind == DRAG_RESIZE_CLIENT) {
        Client *client = wm->drag.client;
        int new_x = wm->drag.start_x;
        int new_y = wm->drag.start_y;
        int new_width = wm->drag.start_width;
        int new_height = wm->drag.start_height;

        if (wm->drag.edges & EDGE_RIGHT)
            new_width += dx;
        if (wm->drag.edges & EDGE_BOTTOM)
            new_height += dy;
        if (wm->drag.edges & EDGE_LEFT) {
            new_x += dx;
            new_width -= dx;
        }
        if (wm->drag.edges & EDGE_TOP) {
            new_y += dy;
            new_height -= dy;
        }
        constrain_client_size(wm, client, &new_width, &new_height);
        if (wm->drag.edges & EDGE_LEFT)
            new_x = wm->drag.start_x + wm->drag.start_width - new_width;
        if (wm->drag.edges & EDGE_TOP)
            new_y = wm->drag.start_y + wm->drag.start_height - new_height;
        client->x = new_x;
        client->y = new_y;
        client->width = new_width;
        client->height = new_height;
    }
    apply_client_geometry(wm, wm->drag.client);
    send_configure_notify(wm, wm->drag.client);
}

static ClientLayout snap_layout_for_release(const WindowManager *wm,
                                            int root_x, int root_y,
                                            size_t *monitor_index)
{
    const MonitorGeometry *monitor;
    int right;

    *monitor_index = monitor_index_for_point(wm, root_x, root_y);
    monitor = monitor_at(wm, *monitor_index);
    if (monitor == NULL)
        return CLIENT_LAYOUT_NORMAL;
    if (root_y < monitor->y ||
        root_y >= monitor->y + monitor->height)
        return CLIENT_LAYOUT_NORMAL;
    if (root_x >= monitor->x - SNAP_THRESHOLD &&
        root_x <= monitor->x + SNAP_THRESHOLD)
        return CLIENT_LAYOUT_SNAP_LEFT;
    right = monitor->x + monitor->width - 1;
    if (root_x >= right - SNAP_THRESHOLD &&
        root_x <= right + SNAP_THRESHOLD)
        return CLIENT_LAYOUT_SNAP_RIGHT;
    return CLIENT_LAYOUT_NORMAL;
}

static void finish_drag(WindowManager *wm, XButtonEvent *event)
{
    DragState drag;
    size_t snap_monitor;
    ClientLayout snap = snap_layout_for_release(
        wm, event->x_root, event->y_root, &snap_monitor);

    if (event->button != Button1)
        return;
    drag = wm->drag;
    cancel_drag(wm, event->time);
    if (drag.kind == DRAG_MOVE_ICON && drag.icon != NULL) {
        int release_dx = event->x_root - drag.start_root_x;
        int release_dy = event->y_root - drag.start_root_y;
        bool moved = drag.moved || abs(release_dx) >= ICON_DRAG_SLOP ||
                     abs(release_dy) >= ICON_DRAG_SLOP;

        if (!moved) {
            activate_desktop_icon(wm, drag.icon, event->time);
        } else {
            const MonitorGeometry *monitor = monitor_at(wm, snap_monitor);
            int width = ICON_WIDTH;
            int height = ICON_HEIGHT;

            drag.icon->x = drag.start_x + release_dx;
            drag.icon->y = drag.start_y + release_dy;
            if (monitor != NULL)
                clamp_geometry_to_monitor(monitor, &drag.icon->x,
                                          &drag.icon->y, &width, &height);
            set_active_monitor(wm, snap_monitor);
            XMoveWindow(wm->display, drag.icon->window, drag.icon->x,
                        drag.icon->y);
            XLowerWindow(wm->display, drag.icon->window);
            remember_desktop_icon_position(wm, drag.icon);
        }
        return;
    }
    if (drag.kind == DRAG_RESIZE_CLIENT) {
        if (drag.moved && drag.client != NULL)
            capture_client_placement(wm, drag.client, true);
        return;
    }
    if (!drag.moved)
        return;
    set_active_monitor(wm, snap_monitor);

    if (drag.kind == DRAG_MOVE_CLIENT && drag.client != NULL) {
        Client *client = drag.client;

        client->layout = CLIENT_LAYOUT_NORMAL;
        client->layout_before_maximize = CLIENT_LAYOUT_NORMAL;
        client->restore_valid = false;
        client->layout_monitor.valid = false;
        client->x = drag.pending_x;
        client->y = drag.pending_y;
        if (drag.arranged_restore_prepared) {
            client->width = drag.pending_width;
            client->height = drag.pending_height;
        }
        if (snap != CLIENT_LAYOUT_NORMAL) {
            set_monitor_anchor(&client->layout_monitor,
                               monitor_at(wm, snap_monitor));
            set_client_layout(wm, client, snap);
        } else {
            set_maximized_state(wm, client, false);
            keep_client_on_screen(wm, client);
            apply_client_geometry(wm, client);
            send_configure_notify(wm, client);
        }
        capture_client_placement(wm, client, true);
    } else if (drag.kind == DRAG_MOVE_LAUNCHER) {
        wm->launcher_layout = CLIENT_LAYOUT_NORMAL;
        wm->launcher_layout_before_maximize = CLIENT_LAYOUT_NORMAL;
        wm->launcher_restore_valid = false;
        wm->launcher_layout_monitor.valid = false;
        wm->launcher_x = drag.pending_x;
        wm->launcher_y = drag.pending_y;
        wm->launcher_width = drag.pending_width;
        wm->launcher_height = drag.pending_height;
        if (snap != CLIENT_LAYOUT_NORMAL) {
            set_monitor_anchor(&wm->launcher_layout_monitor,
                               monitor_at(wm, snap_monitor));
            apply_launcher_layout(wm, snap);
        } else {
            clamp_internal_geometry(wm, &wm->launcher_x, &wm->launcher_y,
                                    &wm->launcher_width,
                                    &wm->launcher_height);
            clamp_launcher_scroll(wm);
            XMoveResizeWindow(wm->display, wm->launcher, wm->launcher_x,
                              wm->launcher_y, (unsigned)wm->launcher_width,
                              (unsigned)wm->launcher_height);
            draw_launcher(wm);
        }
        remember_launcher_placement(wm);
    } else if (drag.kind == DRAG_MOVE_CONTROL_PANEL) {
        ControlPanel *panel = &wm->control_panel;

        panel->layout = CLIENT_LAYOUT_NORMAL;
        panel->layout_before_maximize = CLIENT_LAYOUT_NORMAL;
        panel->restore_valid = false;
        panel->layout_monitor.valid = false;
        panel->x = drag.pending_x;
        panel->y = drag.pending_y;
        panel->width = drag.pending_width;
        panel->height = drag.pending_height;
        if (snap != CLIENT_LAYOUT_NORMAL) {
            set_monitor_anchor(&panel->layout_monitor,
                               monitor_at(wm, snap_monitor));
            apply_control_panel_layout(wm, snap);
        } else {
            clamp_internal_geometry(wm, &panel->x, &panel->y,
                                    &panel->width, &panel->height);
            if (!control_wifi_layout_available(panel))
                clear_control_password(panel);
            XMoveResizeWindow(wm->display, panel->window, panel->x, panel->y,
                              (unsigned)panel->width,
                              (unsigned)panel->height);
            draw_control_panel(wm);
        }
        remember_control_panel_placement(wm);
    } else if (drag.kind == DRAG_MOVE_TASK_MANAGER) {
        TaskManager *manager = &wm->task_manager;

        manager->layout = CLIENT_LAYOUT_NORMAL;
        manager->layout_before_maximize = CLIENT_LAYOUT_NORMAL;
        manager->restore_valid = false;
        manager->layout_monitor.valid = false;
        manager->x = drag.pending_x;
        manager->y = drag.pending_y;
        manager->width = drag.pending_width;
        manager->height = drag.pending_height;
        if (snap != CLIENT_LAYOUT_NORMAL) {
            set_monitor_anchor(&manager->layout_monitor,
                               monitor_at(wm, snap_monitor));
            apply_task_manager_layout(wm, snap);
        } else {
            clamp_internal_geometry(wm, &manager->x, &manager->y,
                                    &manager->width, &manager->height);
            XMoveResizeWindow(wm->display, manager->window, manager->x,
                              manager->y, (unsigned)manager->width,
                              (unsigned)manager->height);
            draw_task_manager(wm);
        }
        remember_task_manager_placement(wm);
    } else if (drag.kind == DRAG_MOVE_RUN) {
        wm->run_dialog.x = drag.pending_x;
        wm->run_dialog.y = drag.pending_y;
        clamp_internal_geometry(wm, &wm->run_dialog.x, &wm->run_dialog.y,
                                &wm->run_dialog.width,
                                &wm->run_dialog.height);
        set_monitor_anchor(
            &wm->run_dialog.monitor,
            monitor_at(wm, monitor_index_for_rectangle(
                               wm, wm->run_dialog.x, wm->run_dialog.y,
                               wm->run_dialog.width,
                               wm->run_dialog.height)));
        XMoveResizeWindow(wm->display, wm->run_dialog.window,
                          wm->run_dialog.x, wm->run_dialog.y,
                          (unsigned)wm->run_dialog.width,
                          (unsigned)wm->run_dialog.height);
        wm->run_dialog.positioned = true;
        remember_run_dialog_placement(wm);
        draw_run_dialog(wm);
    }
}

static void handle_control_wifi_key(WindowManager *wm, XKeyEvent *event,
                                    KeySym key)
{
    ControlPanel *panel = &wm->control_panel;
    int count = (int)wifi_backend_network_count(&wm->wifi);

    if (!control_wifi_layout_available(panel)) {
        clear_control_password(panel);
        return;
    }
    if (key == XK_Up || key == XK_Down) {
        int selected = panel->wifi_selected;

        if (count == 0)
            return;
        if (selected < 0)
            selected = 0;
        else
            selected += key == XK_Up ? -1 : 1;
        if (selected < 0)
            selected = 0;
        if (selected >= count)
            selected = count - 1;
        clear_control_password(panel);
        panel->wifi_selected = selected;
        clamp_wifi_selection(wm);
        draw_control_panel(wm);
        return;
    }
    if (key == XK_Return || key == XK_KP_Enter) {
        activate_selected_wifi(wm);
        return;
    }
    if (key == XK_Tab) {
        const WifiNetwork *selected = selected_wifi_network(wm);

        panel->password_active = wifi_network_needs_password(selected) &&
                                 selected != NULL && !selected->active;
        draw_control_panel(wm);
        return;
    }
    if (panel->password_active && key == XK_BackSpace) {
        if (panel->password_length > 0U) {
            --panel->password_length;
            panel->password[panel->password_length] = '\0';
        }
        draw_control_panel(wm);
        return;
    }
    if (panel->password_active) {
        char input[16];
        KeySym translated;
        int length = XLookupString(event, input, (int)sizeof(input),
                                   &translated, NULL);
        int index;

        for (index = 0; index < length; ++index) {
            unsigned char character = (unsigned char)input[index];

            if (character < 32U || character == 127U ||
                panel->password_length + 1U >= sizeof(panel->password))
                continue;
            panel->password[panel->password_length++] = (char)character;
            panel->password[panel->password_length] = '\0';
        }
        clear_sensitive_bytes(input, sizeof(input));
        if (length > 0)
            draw_control_panel(wm);
    }
}

static void handle_run_key(WindowManager *wm, XKeyEvent *event, KeySym key)
{
    RunDialog *dialog = &wm->run_dialog;

    if ((event->state & Mod1Mask) != 0U) {
        if (key == XK_Tab)
            focus_next(wm, event->time);
        else if (key == XK_F2)
            show_launcher(wm);
        else if (key == XK_F4)
            hide_run_dialog(wm);
        return;
    }
    if (key == XK_Escape) {
        hide_run_dialog(wm);
        return;
    }
    if (key == XK_Return || key == XK_KP_Enter) {
        execute_run_command(wm);
        return;
    }
    if (key == XK_BackSpace) {
        if (dialog->command_length > 0U) {
            --dialog->command_length;
            dialog->command[dialog->command_length] = '\0';
        }
        dialog->status[0] = '\0';
        draw_run_dialog(wm);
        return;
    }
    {
        char input[32];
        KeySym translated;
        int length = XLookupString(event, input, (int)sizeof(input),
                                   &translated, NULL);
        int index;

        for (index = 0; index < length; ++index) {
            unsigned char character = (unsigned char)input[index];

            if (character < 32U || character == 127U ||
                dialog->command_length + 1U >= sizeof(dialog->command))
                continue;
            dialog->command[dialog->command_length++] = (char)character;
            dialog->command[dialog->command_length] = '\0';
        }
        if (length > 0) {
            dialog->status[0] = '\0';
            draw_run_dialog(wm);
        }
    }
}

static int task_manager_selected_application_index(WindowManager *wm)
{
    size_t count = task_manager_application_count(wm);
    size_t index;

    for (index = 0U; index < count; ++index) {
        Client *client = task_manager_application_at(wm, index);

        if (client != NULL &&
            client->window == wm->task_manager.selected_application)
            return index <= (size_t)INT_MAX ? (int)index : -1;
    }
    return -1;
}

static void task_manager_select_application_index(WindowManager *wm,
                                                  int index)
{
    TaskManager *manager = &wm->task_manager;
    int count = task_manager_application_count(wm) <= (size_t)INT_MAX
                    ? (int)task_manager_application_count(wm)
                    : INT_MAX;
    Client *client;

    if (count <= 0) {
        manager->selected_application = None;
        return;
    }
    if (index < 0)
        index = 0;
    if (index >= count)
        index = count - 1;
    client = task_manager_application_at(wm, (size_t)index);
    if (client != NULL)
        manager->selected_application = client->window;
    if (index < manager->application_scroll)
        manager->application_scroll = index;
    if (index >= manager->application_scroll +
                     task_manager_list_visible_rows(manager))
        manager->application_scroll =
            index - task_manager_list_visible_rows(manager) + 1;
    task_manager_clamp_application_scroll(wm);
}

static void handle_task_manager_key(WindowManager *wm, XKeyEvent *event,
                                    KeySym key)
{
    TaskManager *manager = &wm->task_manager;
    unsigned int state = event->state & ~wm->ignored_lock_mask;

    if ((state & Mod1Mask) != 0U) {
        if (key == XK_F4)
            hide_task_manager(wm);
        else if (key == XK_Tab)
            focus_next(wm, event->time);
        else if (key == XK_F2)
            show_launcher(wm);
        return;
    }
    if (key == XK_Delete) {
        if (manager->process_delete_down)
            return;
        manager->process_delete_down = true;
    }
    if (key == XK_Tab && (state & ControlMask) != 0U) {
        int direction = (state & ShiftMask) != 0U ? -1 : 1;
        int tab = ((int)manager->tab + direction + TASK_MANAGER_TAB_COUNT) %
                  TASK_MANAGER_TAB_COUNT;

        task_manager_set_tab(wm, (TaskManagerTab)tab);
        return;
    }
    if ((key == XK_Left || key == XK_Right) &&
        (state & (ControlMask | ShiftMask)) == 0U) {
        int direction = key == XK_Left ? -1 : 1;
        int tab = ((int)manager->tab + direction + TASK_MANAGER_TAB_COUNT) %
                  TASK_MANAGER_TAB_COUNT;

        task_manager_set_tab(wm, (TaskManagerTab)tab);
        return;
    }
    if (key == XK_F5) {
        manager->confirm_pid = 0;
        manager->confirm_start_time = 0U;
        manager->confirm_until_ms = 0U;
        manager->process_delete_down = false;
        manager->status[0] = '\0';
        manager->status_is_refresh_error = false;
        manager->status_is_closing_application = false;
        manager->next_refresh_ms = 0U;
        refresh_task_manager(wm, true);
        return;
    }
    if (key == XK_Escape) {
        manager->confirm_pid = 0;
        manager->confirm_start_time = 0U;
        manager->confirm_until_ms = 0U;
        manager->process_delete_down = false;
        manager->status[0] = '\0';
        manager->status_is_refresh_error = false;
        manager->status_is_closing_application = false;
        draw_task_manager(wm);
        return;
    }
    if (manager->tab == TASK_MANAGER_TAB_APPLICATIONS) {
        int selected = task_manager_selected_application_index(wm);
        int page = task_manager_list_visible_rows(manager);

        if (key == XK_Return || key == XK_KP_Enter) {
            task_manager_switch_to_application(wm, event->time);
            return;
        }
        if (key == XK_Delete) {
            task_manager_end_application(wm, event->time);
            return;
        }
        if (key == XK_Up)
            --selected;
        else if (key == XK_Down)
            ++selected;
        else if (key == XK_Home)
            selected = 0;
        else if (key == XK_End)
            selected = INT_MAX;
        else if (key == XK_Page_Up)
            selected -= page;
        else if (key == XK_Page_Down)
            selected += page;
        else
            return;
        task_manager_select_application_index(wm, selected);
        manager->status[0] = '\0';
        manager->status_is_refresh_error = false;
        manager->status_is_closing_application = false;
        draw_task_manager(wm);
        return;
    }
    if (manager->tab == TASK_MANAGER_TAB_PROCESSES) {
        int selected = task_manager_selected_process_index(manager);
        int page = task_manager_list_visible_rows(manager);

        if (key == XK_Delete) {
            task_manager_request_end_process(wm);
            return;
        }
        if (key == XK_Up)
            --selected;
        else if (key == XK_Down)
            ++selected;
        else if (key == XK_Home)
            selected = 0;
        else if (key == XK_End)
            selected = INT_MAX;
        else if (key == XK_Page_Up)
            selected -= page;
        else if (key == XK_Page_Down)
            selected += page;
        else
            return;
        task_manager_select_process_index(wm, selected);
        manager->status[0] = '\0';
        manager->status_is_refresh_error = false;
        manager->status_is_closing_application = false;
        draw_task_manager(wm);
    }
}

static bool task_manager_key_release_is_auto_repeat(
    WindowManager *wm, const XKeyEvent *event)
{
    XEvent next;

    if (XPending(wm->display) <= 0)
        return false;
    XPeekEvent(wm->display, &next);
    return next.type == KeyPress &&
           next.xkey.window == event->window &&
           next.xkey.keycode == event->keycode &&
           next.xkey.time == event->time;
}

static void handle_key_release(WindowManager *wm, XKeyEvent *event)
{
    KeySym key;

    if (event->window != wm->task_manager.window)
        return;
    key = XLookupKeysym(event, 0);
    if (key != XK_Delete ||
        task_manager_key_release_is_auto_repeat(wm, event))
        return;
    wm->task_manager.process_delete_down = false;
}

static void handle_key_press(WindowManager *wm, XKeyEvent *event)
{
    KeySym key = XLookupKeysym(event, 0);

    if (wm->session_confirmation.visible) {
        if (event->send_event) {
            refocus_session_confirmation(wm, event->time);
            return;
        }
        if (handle_session_confirmation_key(wm, key, event->state,
                                            event->time))
            return;
    }
    if (wm->drag.kind != DRAG_NONE) {
        if (key == XK_Escape)
            cancel_drag_and_restore(wm, event->time);
        return;
    }
    if (key == XK_Escape &&
        (event->state & ~wm->ignored_lock_mask) ==
            (ControlMask | ShiftMask)) {
        dismiss_desktop_menu(wm);
        show_task_manager(wm);
        return;
    }
    if (wm->desktop_menu.visible &&
        handle_desktop_menu_key(wm, key, event->time))
        return;
    if (key == XK_r && key_event_has_super(wm, event)) {
        show_run_dialog(wm);
        return;
    }

    if (event->window == wm->run_dialog.window) {
        handle_run_key(wm, event, key);
        return;
    }

    if (event->window == wm->task_manager.window) {
        handle_task_manager_key(wm, event, key);
        return;
    }

    if (event->window == wm->control_panel.window) {
        if ((event->state & Mod1Mask) != 0U && key == XK_F2) {
            show_launcher(wm);
            return;
        }
        if ((event->state & Mod1Mask) != 0U && key == XK_Tab) {
            focus_next(wm, event->time);
            return;
        }
        if (key == XK_Escape) {
            clear_control_password(&wm->control_panel);
            draw_control_panel(wm);
            return;
        }
        if (key == XK_F4 && (event->state & Mod1Mask) != 0U) {
            hide_control_panel(wm);
            return;
        }
        if (key == XK_Left && wm->control_panel.section > 0) {
            --wm->control_panel.section;
            wm->settings.control_panel_section =
                (Win31xControlPanelSection)wm->control_panel.section;
            save_settings(wm);
            clear_control_password(&wm->control_panel);
            if (wm->control_panel.section == CONTROL_SECTION_WIFI)
                start_wifi_scan(wm);
            else
                draw_control_panel(wm);
            return;
        } else if (key == XK_Right &&
                   wm->control_panel.section + 1 < CONTROL_SECTION_COUNT) {
            ++wm->control_panel.section;
            wm->settings.control_panel_section =
                (Win31xControlPanelSection)wm->control_panel.section;
            save_settings(wm);
            clear_control_password(&wm->control_panel);
            draw_control_panel(wm);
            return;
        }
        if (wm->control_panel.section == CONTROL_SECTION_WIFI) {
            handle_control_wifi_key(wm, event, key);
            return;
        }
        return;
    }
    if (event->window != wm->launcher && (event->state & Mod1Mask)) {
        if (key == XK_Tab)
            focus_next(wm, event->time);
        else if (key == XK_F2)
            show_launcher(wm);
        else if (key == XK_F4 && wm->active != NULL)
            close_client(wm, wm->active, event->time);
        return;
    }
    if (event->window != wm->launcher)
        return;
    if (key == XK_Tab && (event->state & Mod1Mask) != 0U) {
        focus_next(wm, event->time);
        return;
    }
    if (key == XK_F4 && (event->state & Mod1Mask) != 0U) {
        hide_launcher(wm);
        return;
    }
    if (key == XK_Escape)
        return;
    if (key == XK_Return || key == XK_KP_Enter) {
        launch_selected_application(wm);
        return;
    }
    if (wm->applications.len == 0)
        return;
    if (wm->launcher_selected < 0)
        wm->launcher_selected = 0;
    if (key == XK_Left)
        --wm->launcher_selected;
    else if (key == XK_Right)
        ++wm->launcher_selected;
    else if (key == XK_Up)
        wm->launcher_selected -= launcher_columns(wm);
    else if (key == XK_Down)
        wm->launcher_selected += launcher_columns(wm);
    else if (key == XK_Home)
        wm->launcher_selected = 0;
    else if (key == XK_End)
        wm->launcher_selected = (int)wm->applications.len - 1;
    else if (key == XK_Page_Up)
        wm->launcher_selected -= launcher_columns(wm) * launcher_visible_rows(wm);
    else if (key == XK_Page_Down)
        wm->launcher_selected += launcher_columns(wm) * launcher_visible_rows(wm);
    else
        return;
    if (wm->launcher_selected < 0)
        wm->launcher_selected = 0;
    if (wm->launcher_selected >= (int)wm->applications.len)
        wm->launcher_selected = (int)wm->applications.len - 1;
    {
        int selected_row = wm->launcher_selected / launcher_columns(wm);
        if (selected_row < wm->launcher_scroll_row)
            wm->launcher_scroll_row = selected_row;
        if (selected_row >= wm->launcher_scroll_row + launcher_visible_rows(wm))
            wm->launcher_scroll_row = selected_row - launcher_visible_rows(wm) + 1;
        clamp_launcher_scroll(wm);
    }
    draw_launcher(wm);
}

static void update_client_title(WindowManager *wm, Client *client)
{
    char *title = window_title(wm, client->window);

    if (title == NULL)
        return;
    free(client->title);
    client->title = title;
    draw_frame(wm, client);
    if (client->icon != NULL)
        draw_desktop_icon(wm, client->icon);
}

static bool restore_late_client_identity(WindowManager *wm, Client *client,
                                         bool preserve_initial_maximize)
{
    const Win31xDesktopClientRecord *record;
    ClientLayout saved_layout;
    ClientLayout saved_layout_before_maximize;
    size_t monitor_index = 0U;

    if (!client->state_persistable || client->state_identity == NULL)
        return false;
    record = win31x_desktop_state_find_client(&wm->desktop_state,
                                               client->state_identity);
    if (record == NULL || !record->placement.valid)
        return false;

    if (preserve_initial_maximize &&
        client->layout == CLIENT_LAYOUT_MAXIMIZED) {
        int current_x = client->x;
        int current_y = client->y;
        int current_width = client->width;
        int current_height = client->height;
        MonitorAnchor current_monitor = client->layout_monitor;

        client->layout = CLIENT_LAYOUT_NORMAL;
        client->layout_monitor.valid = false;
        client->state_restored = false;
        saved_layout = restore_client_placement(wm, client, &monitor_index);
        saved_layout_before_maximize = client->layout_before_maximize;
        if (!client->state_restored) {
            client->x = current_x;
            client->y = current_y;
            client->width = current_width;
            client->height = current_height;
            client->layout = CLIENT_LAYOUT_MAXIMIZED;
            client->layout_monitor = current_monitor;
            return false;
        }
        client->restore_x = client->x;
        client->restore_y = client->y;
        client->restore_width = client->width;
        client->restore_height = client->height;
        client->restore_valid = true;
        /* Match manage_window(): keep the application's maximize request, but
         * anchor it to the now-known saved monitor and restore target. */
        client->layout = CLIENT_LAYOUT_MAXIMIZED;
        client->layout_before_maximize =
            saved_layout == CLIENT_LAYOUT_MAXIMIZED
                ? saved_layout_before_maximize
                : saved_layout;
        set_monitor_anchor(&client->layout_monitor,
                           monitor_at(wm, monitor_index));
        reapply_client_layout(wm, client);
        return true;
    }

    normalize_client_layout(wm, client);
    client->layout_before_maximize = CLIENT_LAYOUT_NORMAL;
    client->restore_valid = false;
    client->layout_monitor.valid = false;
    client->state_restored = false;
    saved_layout = restore_client_placement(wm, client, &monitor_index);
    saved_layout_before_maximize = client->layout_before_maximize;
    if (!client->state_restored)
        return false;
    if (saved_layout != CLIENT_LAYOUT_NORMAL) {
        set_monitor_anchor(&client->layout_monitor,
                           monitor_at(wm, monitor_index));
        set_client_layout(wm, client, saved_layout);
        if (saved_layout == CLIENT_LAYOUT_MAXIMIZED)
            client->layout_before_maximize = saved_layout_before_maximize;
    } else {
        set_maximized_state(wm, client, false);
        apply_client_geometry(wm, client);
        send_configure_notify(wm, client);
    }
    return true;
}

static void update_client_class(WindowManager *wm, Client *client)
{
    char *class_name = window_class(wm, client->window);
    char *base_identity = window_state_identity(wm, client->window);

    if (class_name == NULL) {
        free(base_identity);
        return;
    }
    free(client->class_name);
    client->class_name = class_name;
    if (base_identity != NULL) {
        bool identity_changed = client->state_base_identity == NULL ||
                                strcmp(client->state_base_identity,
                                       base_identity) != 0;
        bool identity_ready = !identity_changed;

        if (identity_changed) {
            char *identity;
            unsigned int instance = 0U;

            identity = client_state_identity(wm, client, base_identity,
                                             &instance);
            if (identity != NULL) {
                free(client->state_base_identity);
                free(client->state_identity);
                client->state_base_identity = base_identity;
                client->state_identity = identity;
                client->state_instance = instance;
                base_identity = NULL;
                identity_ready = true;
            }
        }
        free(base_identity);
        client->state_persistable = identity_ready &&
                                    client->transient_for == None &&
                                    client->state_identity != NULL &&
                                    client->state_identity[0] != '\0';
        client->state_restored = false;
        if (identity_ready &&
            !(identity_changed &&
              restore_late_client_identity(
                  wm, client, client->initial_maximize_precedence))) {
            if (identity_changed)
                client->state_monitor_fallback = false;
            capture_client_placement(wm, client, false);
        }
        if (identity_changed)
            client->initial_maximize_precedence = false;
    }
    draw_frame(wm, client);
    if (client->icon != NULL)
        draw_desktop_icon(wm, client->icon);
}

static void handle_client_message(WindowManager *wm, XClientMessageEvent *event)
{
    Client *client = client_for_client_window(wm, event->window);

    if (client == NULL)
        return;
    if (event->message_type == wm->atoms.wm_change_state &&
        event->format == 32 && event->data.l[0] == IconicState) {
        minimize_client(wm, client);
    } else if (event->message_type == wm->atoms.net_active_window &&
               event->format == 32) {
        restore_client(wm, client, (Time)event->data.l[1]);
    } else if (event->message_type == wm->atoms.net_close_window &&
               event->format == 32) {
        close_client(wm, client, (Time)event->data.l[0]);
    } else if (event->message_type == wm->atoms.net_wm_state &&
               event->format == 32) {
        Atom first = (Atom)event->data.l[1];
        Atom second = (Atom)event->data.l[2];
        bool mentions_hidden = first == wm->atoms.net_wm_state_hidden ||
                               second == wm->atoms.net_wm_state_hidden;
        bool mentions_maximized =
            first == wm->atoms.net_wm_state_maximized_horz ||
            first == wm->atoms.net_wm_state_maximized_vert ||
            second == wm->atoms.net_wm_state_maximized_horz ||
            second == wm->atoms.net_wm_state_maximized_vert;
        long action = event->data.l[0];

        if (mentions_hidden) {
            if ((action == 1 || action == 2) && !client->minimized)
                minimize_client(wm, client);
            else if ((action == 0 || action == 2) && client->minimized)
                restore_client(wm, client, CurrentTime);
        }
        if (mentions_maximized) {
            bool maximized = client->layout == CLIENT_LAYOUT_MAXIMIZED;

            if ((action == 1 || action == 2) && !maximized)
                set_client_layout(wm, client, CLIENT_LAYOUT_MAXIMIZED);
            else if ((action == 0 || action == 2) && maximized)
                set_client_layout(wm, client, CLIENT_LAYOUT_NORMAL);
            capture_client_placement(wm, client, true);
        }
    }
}

static void publish_root_workarea(WindowManager *wm)
{
    long workarea[4] = {0, 0, wm->screen_width, wm->screen_height};

    XChangeProperty(wm->display, wm->root, wm->atoms.net_workarea, XA_CARDINAL,
                    32, PropModeReplace, (unsigned char *)workarea, 4);
}

static void reflow_monitor_layout(WindowManager *wm)
{
    Client *client;

    cancel_drag(wm, CurrentTime);
    dismiss_desktop_menu(wm);
    if (wm->active_monitor.valid)
        set_active_monitor(
            wm, monitor_index_for_anchor(wm, &wm->active_monitor));
    else
        set_active_monitor(wm, pointer_monitor_index(wm));
    for (client = wm->clients; client != NULL; client = client->next) {
        int old_x = client->x;
        int old_y = client->y;

        if (reflow_client_to_saved_placement(wm, client))
            continue;
        if (client->layout != CLIENT_LAYOUT_NORMAL) {
            reapply_client_layout(wm, client);
            continue;
        }
        keep_client_on_screen(wm, client);
        if (client->x != old_x || client->y != old_y) {
            apply_client_geometry(wm, client);
            send_configure_notify(wm, client);
        }
    }
    reposition_icons(wm);
    if (wm->desktop_state.launcher.valid &&
        (ClientLayout)wm->desktop_state.launcher.layout ==
            wm->launcher_layout)
        restore_launcher_placement(wm);
    if (wm->launcher_visible) {
        if (wm->launcher_layout == CLIENT_LAYOUT_NORMAL) {
            clamp_internal_geometry(wm, &wm->launcher_x, &wm->launcher_y,
                                    &wm->launcher_width,
                                    &wm->launcher_height);
            clamp_launcher_scroll(wm);
            XMoveResizeWindow(wm->display, wm->launcher, wm->launcher_x,
                              wm->launcher_y, (unsigned)wm->launcher_width,
                              (unsigned)wm->launcher_height);
            draw_launcher(wm);
        } else {
            apply_launcher_layout(wm, wm->launcher_layout);
        }
    }
    if (wm->desktop_state.control_panel.valid &&
        (ClientLayout)wm->desktop_state.control_panel.layout ==
            wm->control_panel.layout)
        restore_control_panel_placement(wm);
    if (wm->control_panel.visible) {
        if (wm->control_panel.layout == CLIENT_LAYOUT_NORMAL) {
            ControlPanel *panel = &wm->control_panel;

            clamp_internal_geometry(wm, &panel->x, &panel->y, &panel->width,
                                    &panel->height);
            if (!control_wifi_layout_available(panel))
                clear_control_password(panel);
            XMoveResizeWindow(wm->display, panel->window, panel->x, panel->y,
                              (unsigned)panel->width,
                              (unsigned)panel->height);
            draw_control_panel(wm);
        } else {
            apply_control_panel_layout(wm, wm->control_panel.layout);
        }
    }
    if (wm->desktop_state.run_dialog.valid)
        restore_run_dialog_placement(wm);
    if (wm->run_dialog.visible) {
        if (!wm->desktop_state.run_dialog.valid)
            position_run_dialog(wm);
        else
            XMoveResizeWindow(wm->display, wm->run_dialog.window,
                              wm->run_dialog.x, wm->run_dialog.y,
                              (unsigned)wm->run_dialog.width,
                              (unsigned)wm->run_dialog.height);
        draw_run_dialog(wm);
    }
    if (wm->desktop_state.task_manager.valid &&
        (ClientLayout)wm->desktop_state.task_manager.layout ==
            wm->task_manager.layout)
        restore_task_manager_placement(wm);
    if (wm->task_manager.visible) {
        TaskManager *manager = &wm->task_manager;

        if (manager->layout == CLIENT_LAYOUT_NORMAL) {
            clamp_internal_geometry(wm, &manager->x, &manager->y,
                                    &manager->width, &manager->height);
            task_manager_clamp_application_scroll(wm);
            task_manager_clamp_process_scroll(wm);
            XMoveResizeWindow(wm->display, manager->window, manager->x,
                              manager->y, (unsigned)manager->width,
                              (unsigned)manager->height);
            draw_task_manager(wm);
        } else {
            apply_task_manager_layout(wm, manager->layout);
        }
    }
    if (wm->session_confirmation.visible) {
        position_session_confirmation(wm);
        draw_session_confirmation(wm);
        refocus_session_confirmation(wm, CurrentTime);
    }
}

static void handle_root_layout_change(WindowManager *wm, int width, int height)
{
    bool size_changed = false;
    bool monitors_changed;

    if (width > 0 && height > 0 &&
        (width != wm->screen_width || height != wm->screen_height)) {
        wm->screen_width = width;
        wm->screen_height = height;
        size_changed = true;
        publish_root_workarea(wm);
    }
    monitors_changed = refresh_monitor_layout(wm);
    if (size_changed || monitors_changed)
        reflow_monitor_layout(wm);
}

static void handle_randr_event(WindowManager *wm, XEvent *event)
{
    XWindowAttributes attributes;

    if (!wm->randr_available)
        return;
    if (event->type == wm->randr_event_base + RRScreenChangeNotify)
        (void)XRRUpdateConfiguration(event);
    if (XGetWindowAttributes(wm->display, wm->root, &attributes))
        handle_root_layout_change(wm, attributes.width, attributes.height);
    else
        handle_root_layout_change(wm, wm->screen_width, wm->screen_height);
}

static void dispatch_event(WindowManager *wm, XEvent *event)
{
    Client *client;
    DesktopIcon *icon;

    if (wm->randr_available &&
        (event->type == wm->randr_event_base + RRScreenChangeNotify ||
         event->type == wm->randr_event_base + RRNotify)) {
        handle_randr_event(wm, event);
        return;
    }

    switch (event->type) {
    case MapRequest:
        dismiss_desktop_menu(wm);
        XGrabServer(wm->display);
        client = client_for_client_window(wm, event->xmaprequest.window);
        if (client == NULL) {
            Client *managed = manage_window(wm, event->xmaprequest.window,
                                            false);

            if (managed == NULL) {
                XWindowAttributes attributes;

                if (XGetWindowAttributes(wm->display,
                                         event->xmaprequest.window,
                                         &attributes) &&
                    attributes.class == InputOnly)
                    XMapWindow(wm->display, event->xmaprequest.window);
            }
        } else if (client->minimized) {
            restore_client(wm, client, CurrentTime);
        } else {
            XMapWindow(wm->display, client->window);
            XMapWindow(wm->display, client->frame);
            focus_client(wm, client, CurrentTime);
        }
        XUngrabServer(wm->display);
        XSync(wm->display, False);
        break;
    case ConfigureRequest:
        handle_configure_request(wm, &event->xconfigurerequest);
        break;
    case UnmapNotify:
        client = client_for_client_window(wm, event->xunmap.window);
        if (client != NULL) {
            if (event->xunmap.event == client->window) {
                if (client->expected_unmaps > 0)
                    --client->expected_unmaps;
                else
                    remove_client(wm, client, false, false, false);
            } else if (event->xunmap.send_event &&
                       event->xunmap.event == wm->root) {
                remove_client(wm, client, false, false, false);
            }
        }
        break;
    case DestroyNotify:
        client = client_for_client_window(wm, event->xdestroywindow.window);
        if (client != NULL)
            remove_client(wm, client, true, false, false);
        break;
    case ReparentNotify:
        client = client_for_client_window(wm, event->xreparent.window);
        if (client != NULL && event->xreparent.parent != client->frame)
            remove_client(wm, client, false, false, true);
        break;
    case Expose:
        if (event->xexpose.count != 0)
            break;
        client = client_for_window(wm, event->xexpose.window);
        if (client != NULL && event->xexpose.window == client->frame)
            draw_frame(wm, client);
        else if (event->xexpose.window == wm->launcher)
            draw_launcher(wm);
        else if (event->xexpose.window == wm->control_panel.window)
            draw_control_panel(wm);
        else if (event->xexpose.window == wm->run_dialog.window)
            draw_run_dialog(wm);
        else if (event->xexpose.window == wm->task_manager.window)
            draw_task_manager(wm);
        else if (event->xexpose.window == wm->desktop_menu.window)
            draw_desktop_menu(wm);
        else if (event->xexpose.window ==
                 wm->session_confirmation.window)
            draw_session_confirmation(wm);
        else {
            icon = icon_for_window(wm, event->xexpose.window);
            if (icon != NULL)
                draw_desktop_icon(wm, icon);
        }
        break;
    case ButtonPress:
        handle_button_press(wm, &event->xbutton);
        break;
    case ButtonRelease:
        if (wm->session_confirmation.visible) {
            if (event->xbutton.send_event)
                refocus_session_confirmation(wm, event->xbutton.time);
            else
                handle_session_confirmation_button_release(
                    wm, &event->xbutton);
            break;
        }
        if (wm->desktop_menu.visible) {
            handle_desktop_menu_button_release(wm, &event->xbutton);
            break;
        }
        if (wm->drag.kind != DRAG_NONE)
            finish_drag(wm, &event->xbutton);
        break;
    case MotionNotify:
        while (XCheckTypedWindowEvent(wm->display, event->xmotion.window,
                                      MotionNotify, event))
            ;
        handle_motion(wm, &event->xmotion);
        break;
    case KeyPress:
        handle_key_press(wm, &event->xkey);
        break;
    case KeyRelease:
        handle_key_release(wm, &event->xkey);
        break;
    case PropertyNotify:
        client = client_for_client_window(wm, event->xproperty.window);
        if (client != NULL &&
            (event->xproperty.atom == XA_WM_NAME ||
             event->xproperty.atom == wm->atoms.net_wm_name ||
             event->xproperty.atom == wm->atoms.net_wm_icon_name))
            update_client_title(wm, client);
        else if (client != NULL &&
                 (event->xproperty.atom == XA_WM_CLASS ||
                  event->xproperty.atom == wm->atoms.gtk_application_id ||
                  event->xproperty.atom == wm->atoms.net_wm_desktop_file ||
                  event->xproperty.atom == wm->atoms.wm_window_role))
            update_client_class(wm, client);
        else if (client != NULL &&
                 event->xproperty.atom == wm->atoms.net_wm_icon)
            refresh_client_icon(wm, client);
        else if (client != NULL &&
                 event->xproperty.atom == XA_WM_NORMAL_HINTS)
            handle_normal_hints_change(wm, client);
        else if (client != NULL &&
                 event->xproperty.atom == XA_WM_TRANSIENT_FOR) {
            bool was_persistable = client->state_persistable;

            refresh_client_transient_for(wm, client);
            client->state_persistable =
                client->transient_for == None &&
                client->state_identity != NULL &&
                client->state_identity[0] != '\0';
            if (!was_persistable && client->state_persistable) {
                client->state_monitor_fallback = false;
                capture_client_placement(wm, client, false);
            }
        }
        break;
    case FocusIn:
        if (wm->session_confirmation.visible) {
            if (event->xfocus.window != wm->session_confirmation.window)
                refocus_session_confirmation(wm, CurrentTime);
            break;
        }
        client = client_for_client_window(wm, event->xfocus.window);
        if (client != NULL && !client->minimized &&
            !event->xfocus.send_event && event->xfocus.mode != NotifyGrab &&
            event->xfocus.mode != NotifyUngrab &&
            (event->xfocus.detail == NotifyAncestor ||
             event->xfocus.detail == NotifyVirtual ||
             event->xfocus.detail == NotifyNonlinear ||
             event->xfocus.detail == NotifyNonlinearVirtual) &&
            client_currently_has_focus(wm, client)) {
            bool leaving_internal =
                wm->internal_focus != INTERNAL_FOCUS_NONE;

            wm->internal_focus = INTERNAL_FOCUS_NONE;
            set_active_monitor_from_client(wm, client);
            change_active_client(wm, client, true);
            if (leaving_internal) {
                if (wm->launcher_visible)
                    draw_launcher(wm);
                if (wm->control_panel.visible)
                    draw_control_panel(wm);
                if (wm->run_dialog.visible)
                    draw_run_dialog(wm);
                if (wm->task_manager.visible)
                    draw_task_manager(wm);
            }
        }
        break;
    case ClientMessage:
        handle_client_message(wm, &event->xclient);
        break;
    case ConfigureNotify:
        if (event->xconfigure.window == wm->root)
            handle_root_layout_change(wm, event->xconfigure.width,
                                      event->xconfigure.height);
        else if (wm->session_confirmation.visible &&
                 event->xconfigure.event == wm->root &&
                 event->xconfigure.window !=
                     wm->session_confirmation.window &&
                 event->xconfigure.window !=
                     wm->session_confirmation.shield) {
            if (session_confirmation_should_yield_to_window(
                    wm, event->xconfigure.window))
                dismiss_session_confirmation(wm, false, CurrentTime);
            else
                refocus_session_confirmation(wm, CurrentTime);
        }
        break;
    case MapNotify:
        if (wm->session_confirmation.visible &&
            event->xmap.event == wm->root &&
            event->xmap.window != wm->session_confirmation.window &&
            event->xmap.window != wm->session_confirmation.shield) {
            if (session_confirmation_should_yield_to_window(
                    wm, event->xmap.window))
                dismiss_session_confirmation(wm, false, CurrentTime);
            else
                refocus_session_confirmation(wm, CurrentTime);
        }
        break;
    case MappingNotify:
        XRefreshKeyboardMapping(&event->xmapping);
        grab_shortcuts(wm);
        break;
    default:
        break;
    }
}

static int initialize_window_manager(WindowManager *wm, const char *display_name)
{
    XSetWindowAttributes support_attributes;
    struct sigaction action;
    int connection_flags;

    memset(wm, 0, sizeof(*wm));
    wm->display = XOpenDisplay(display_name);
    if (wm->display == NULL) {
        fprintf(stderr, "win31x: cannot open X display %s\n",
                display_name != NULL ? display_name : "(from DISPLAY)");
        return -1;
    }
    if (ConnectionNumber(wm->display) < 0 ||
        ConnectionNumber(wm->display) >= FD_SETSIZE) {
        fprintf(stderr,
                "win31x: X connection descriptor is outside select() range\n");
        XCloseDisplay(wm->display);
        wm->display = NULL;
        errno = EMFILE;
        return -1;
    }
    wm->screen = DefaultScreen(wm->display);
    wm->root = RootWindow(wm->display, wm->screen);
    wm->screen_width = DisplayWidth(wm->display, wm->screen);
    wm->screen_height = DisplayHeight(wm->display, wm->screen);
    wm->randr_major = 1;
    wm->randr_minor = 5;
    if (XRRQueryExtension(wm->display, &wm->randr_event_base,
                          &wm->randr_error_base) &&
        XRRQueryVersion(wm->display, &wm->randr_major,
                        &wm->randr_minor) &&
        (wm->randr_major > 1 ||
         (wm->randr_major == 1 && wm->randr_minor >= 5))) {
        wm->randr_available = true;
        XRRSelectInput(wm->display, wm->root,
                       RRScreenChangeNotifyMask | RRCrtcChangeNotifyMask |
                           RROutputChangeNotifyMask |
                           RRProviderChangeNotifyMask |
                           RRResourceChangeNotifyMask);
    }
    (void)refresh_monitor_layout(wm);
    set_active_monitor(wm, pointer_monitor_index(wm));
    initialize_atoms(wm);
    if (win31x_settings_load(&wm->settings) < 0) {
        fprintf(stderr, "win31x: could not load settings: %s\n",
                strerror(errno));
        win31x_settings_defaults(&wm->settings);
    }
    if (win31x_desktop_state_load(&wm->desktop_state) < 0) {
        fprintf(stderr, "win31x: could not load desktop layout: %s\n",
                strerror(errno));
        win31x_desktop_state_defaults(&wm->desktop_state);
    }
    initialize_theme(wm);
    wm->font = XLoadQueryFont(wm->display, "-misc-fixed-*-*-*-*-13-*-*-*-*-*-*-*");
    if (wm->font == NULL)
        wm->font = XLoadQueryFont(wm->display, "fixed");
    if (wm->font == NULL) {
        fprintf(stderr, "win31x: the X server has no usable core font\n");
        XCloseDisplay(wm->display);
        return -1;
    }
    wm->gc = XCreateGC(wm->display, wm->root, 0, NULL);
    XSetFont(wm->display, wm->gc, wm->font->fid);
    XSetGraphicsExposures(wm->display, wm->gc, False);
    wm->arrow_cursor = XCreateFontCursor(wm->display, XC_left_ptr);
    wm->move_cursor = XCreateFontCursor(wm->display, XC_fleur);
    wm->resize_cursor = XCreateFontCursor(wm->display, XC_bottom_right_corner);

    XSetErrorHandler(startup_error_handler);
    XSelectInput(wm->display, wm->root,
                 SubstructureRedirectMask | SubstructureNotifyMask |
                 PropertyChangeMask | StructureNotifyMask | KeyPressMask |
                 ButtonPressMask);
    XSync(wm->display, False);
    if (startup_bad_access) {
        fprintf(stderr, "win31x: another window manager is already running\n");
        XCloseDisplay(wm->display);
        return -1;
    }
    XSetErrorHandler(runtime_error_handler);
    if (initialize_rendered_icons(wm) < 0) {
        icon_assets_free(&wm->icon_assets);
        XFreeCursor(wm->display, wm->arrow_cursor);
        XFreeCursor(wm->display, wm->move_cursor);
        XFreeCursor(wm->display, wm->resize_cursor);
        XFreeGC(wm->display, wm->gc);
        XFreeFont(wm->display, wm->font);
        XCloseDisplay(wm->display);
        return -1;
    }
    XSetWindowBackground(wm->display, wm->root, wm->theme.desktop);
    XClearWindow(wm->display, wm->root);
    XDefineCursor(wm->display, wm->root, wm->arrow_cursor);

    support_attributes.override_redirect = True;
    support_attributes.event_mask = NoEventMask;
    wm->support_window = XCreateWindow(wm->display, wm->root, -10, -10, 1, 1, 0,
                                       CopyFromParent, InputOutput, CopyFromParent,
                                       CWOverrideRedirect | CWEventMask,
                                       &support_attributes);
    set_internal_role(wm, wm->support_window, "wm-check");
    initialize_ewmh(wm);
    grab_shortcuts(wm);
    connection_flags = fcntl(ConnectionNumber(wm->display), F_GETFD);
    if (connection_flags >= 0)
        fcntl(ConnectionNumber(wm->display), F_SETFD, connection_flags | FD_CLOEXEC);

    memset(&action, 0, sizeof(action));
    action.sa_handler = handle_signal;
    sigemptyset(&action.sa_mask);
    sigaction(SIGINT, &action, NULL);
    sigaction(SIGTERM, &action, NULL);
    action.sa_handler = handle_child_signal;
    action.sa_flags = SA_RESTART | SA_NOCLDSTOP;
    sigaction(SIGCHLD, &action, NULL);
    action.sa_handler = SIG_IGN;
    action.sa_flags = 0;
    sigaction(SIGPIPE, &action, NULL);

    if (wifi_backend_init(&wm->wifi) < 0)
        fprintf(stderr, "win31x: could not initialize Wi-Fi controls: %s\n",
                strerror(errno));
    if (win31x_auto_lock_init(&wm->auto_lock, wm->display,
                              wm->settings.auto_lock_enabled,
                              wm->settings.auto_lock_minutes) < 0) {
        fprintf(stderr, "win31x: auto lock is unavailable: %s\n",
                wm->auto_lock.status);
    }
    if (win31x_session_actions_init(&wm->session_actions) < 0) {
        fprintf(stderr, "win31x: restart and shut down are unavailable: %s\n",
                wm->session_actions.status);
    }

    adopt_existing_windows(wm);
    initialize_launcher(wm);
    initialize_control_panel(wm);
    initialize_run_dialog(wm);
    initialize_task_manager(wm);
    initialize_desktop_menu(wm);
    initialize_session_confirmation(wm);
    initialize_drag_outline(wm);
    wm->applications_icon = create_desktop_icon(wm, ICON_APPLICATIONS, NULL);
    wm->control_panel_icon =
        create_desktop_icon(wm, ICON_CONTROL_PANEL, NULL);
    XSync(wm->display, False);
    return 0;
}

static void reap_children(WindowManager *wm)
{
    pid_t pid;
    int wait_status;

    child_changed = 0;
    for (;;) {
        pid = waitpid((pid_t)-1, &wait_status, WNOHANG);
        if (pid > 0) {
            bool wifi_handled = wifi_backend_handle_child_exit(
                &wm->wifi, pid, wait_status);
            bool lock_handled = win31x_auto_lock_handle_child_exit(
                &wm->auto_lock, pid, wait_status);
            bool session_handled = win31x_session_actions_handle_child_exit(
                &wm->session_actions, pid, wait_status);

            if (wifi_handled && wm->control_panel.visible &&
                wm->control_panel.section == CONTROL_SECTION_WIFI) {
                clamp_wifi_selection(wm);
                draw_control_panel(wm);
            }
            if (lock_handled && wm->control_panel.visible &&
                wm->control_panel.section == CONTROL_SECTION_AUTO_LOCK)
                draw_control_panel(wm);
            if ((lock_handled || session_handled) &&
                wm->desktop_menu.visible)
                draw_desktop_menu(wm);
            if (session_handled &&
                (!WIFEXITED(wait_status) || WEXITSTATUS(wait_status) != 0))
                fprintf(stderr, "win31x: %s\n",
                        wm->session_actions.status);
            continue;
        }
        if (pid < 0 && errno == EINTR)
            continue;
        break;
    }
}

static void service_wifi_backend(WindowManager *wm, const fd_set *read_set)
{
    WifiStatus old_status = wifi_backend_status(&wm->wifi);
    bool old_busy = wifi_backend_busy(&wm->wifi);
    size_t old_count = wifi_backend_network_count(&wm->wifi);
    char old_text[192];

    snprintf(old_text, sizeof(old_text), "%s",
             wifi_backend_status_text(&wm->wifi));
    if (read_set != NULL)
        wifi_backend_dispatch_fds(&wm->wifi, read_set);
    else
        wifi_backend_tick(&wm->wifi);
    if (wm->control_panel.visible &&
        wm->control_panel.section == CONTROL_SECTION_WIFI &&
        (old_status != wifi_backend_status(&wm->wifi) ||
         old_busy != wifi_backend_busy(&wm->wifi) ||
         old_count != wifi_backend_network_count(&wm->wifi) ||
         strcmp(old_text, wifi_backend_status_text(&wm->wifi)) != 0)) {
        clamp_wifi_selection(wm);
        draw_control_panel(wm);
    }
}

static void run_event_loop(WindowManager *wm)
{
    int connection = ConnectionNumber(wm->display);

    while (keep_running) {
        flush_desktop_state(wm, false);
        refresh_task_manager(wm, false);
        if (child_changed)
            reap_children(wm);
        while (keep_running && XPending(wm->display) > 0) {
            XEvent event;

            if (child_changed)
                reap_children(wm);
            XNextEvent(wm->display, &event);
            dispatch_event(wm, &event);
        }
        if (child_changed)
            reap_children(wm);
        if (keep_running) {
            fd_set read_set;
            struct timeval timeout;
            int maximum_fd = connection;
            int selected;

            FD_ZERO(&read_set);
            FD_SET(connection, &read_set);
            wifi_backend_add_select_fds(&wm->wifi, &read_set, &maximum_fd);
            timeout.tv_sec = 0;
            timeout.tv_usec = 250000;
            selected = select(maximum_fd + 1, &read_set, NULL, NULL, &timeout);
            if (selected < 0 && errno != EINTR)
                break;
            service_wifi_backend(wm, selected >= 0 ? &read_set : NULL);
            refresh_task_manager(wm, false);
            flush_desktop_state(wm, false);
        }
    }
}

static void shut_down(WindowManager *wm)
{
    size_t index;

    flush_desktop_state(wm, true);
    cancel_drag(wm, CurrentTime);
    dismiss_session_confirmation(wm, false, CurrentTime);
    dismiss_desktop_menu(wm);
    wifi_backend_destroy(&wm->wifi);
    win31x_auto_lock_shutdown(&wm->auto_lock);
    while (wm->clients != NULL)
        remove_client(wm, wm->clients, false, true, false);
    while (wm->icons != NULL)
        destroy_desktop_icon(wm, wm->icons);
    free_application_icons(wm);
    apps_free(&wm->applications);
    clear_control_password(&wm->control_panel);
    if (wm->control_panel.window != None)
        XDestroyWindow(wm->display, wm->control_panel.window);
    if (wm->run_dialog.window != None)
        XDestroyWindow(wm->display, wm->run_dialog.window);
    win31x_task_manager_data_destroy(&wm->task_manager.data);
    if (wm->task_manager.backing != None)
        XFreePixmap(wm->display, wm->task_manager.backing);
    if (wm->task_manager.window != None)
        XDestroyWindow(wm->display, wm->task_manager.window);
    if (wm->desktop_menu.window != None)
        XDestroyWindow(wm->display, wm->desktop_menu.window);
    if (wm->session_confirmation.window != None)
        XDestroyWindow(wm->display, wm->session_confirmation.window);
    if (wm->session_confirmation.shield != None)
        XDestroyWindow(wm->display, wm->session_confirmation.shield);
    for (index = 0U; index < DRAG_OUTLINE_WINDOW_COUNT; ++index) {
        if (wm->drag_outline[index] != None)
            XDestroyWindow(wm->display, wm->drag_outline[index]);
    }
    if (wm->launcher != None)
        XDestroyWindow(wm->display, wm->launcher);
    if (wm->support_window != None)
        XDestroyWindow(wm->display, wm->support_window);
    XDeleteProperty(wm->display, wm->root, wm->atoms.net_supporting_wm_check);
    XDeleteProperty(wm->display, wm->root, wm->atoms.net_supported);
    XDeleteProperty(wm->display, wm->root, wm->atoms.net_client_list);
    XDeleteProperty(wm->display, wm->root, wm->atoms.net_active_window);
    XDeleteProperty(wm->display, wm->root, wm->atoms.net_workarea);
    XDeleteProperty(wm->display, wm->root,
                    wm->atoms.net_number_of_desktops);
    XDeleteProperty(wm->display, wm->root, wm->atoms.net_current_desktop);
    XSetInputFocus(wm->display, PointerRoot, RevertToPointerRoot, CurrentTime);
    XSync(wm->display, False);
    free_rendered_icons(wm);
    icon_assets_free(&wm->icon_assets);
    XFreeCursor(wm->display, wm->arrow_cursor);
    XFreeCursor(wm->display, wm->move_cursor);
    XFreeCursor(wm->display, wm->resize_cursor);
    XFreeGC(wm->display, wm->gc);
    XFreeFont(wm->display, wm->font);
    XCloseDisplay(wm->display);
}

static void print_usage(FILE *stream)
{
    fprintf(stream,
            "Usage: win31x [--display DISPLAY] [--help] [--version]\n"
            "\n"
            "A minimal Windows 3.1-inspired window manager for X11.\n");
}

int main(int argc, char **argv)
{
    WindowManager wm;
    const char *display_name = NULL;
    int index;

    for (index = 1; index < argc; ++index) {
        if (strcmp(argv[index], "--help") == 0 || strcmp(argv[index], "-h") == 0) {
            print_usage(stdout);
            return 0;
        }
        if (strcmp(argv[index], "--version") == 0 || strcmp(argv[index], "-v") == 0) {
            puts("win31x 0.1.0");
            return 0;
        }
        if (strcmp(argv[index], "--display") == 0 && index + 1 < argc) {
            display_name = argv[++index];
            continue;
        }
        fprintf(stderr, "win31x: unknown option: %s\n", argv[index]);
        print_usage(stderr);
        return 2;
    }
    if (initialize_window_manager(&wm, display_name) < 0)
        return 1;
    run_event_loop(&wm);
    shut_down(&wm);
    return 0;
}
