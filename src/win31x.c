#define _POSIX_C_SOURCE 200809L

#include "applications.h"
#include "auto_lock.h"
#include "icon_assets.h"
#include "settings.h"
#include "wifi_backend.h"

#include <X11/Xatom.h>
#include <X11/Xlib.h>
#include <X11/Xutil.h>
#include <X11/cursorfont.h>
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
#define DOUBLE_CLICK_MS 450

typedef struct Client Client;
typedef struct DesktopIcon DesktopIcon;

typedef enum {
    ICON_APPLICATIONS,
    ICON_CONTROL_PANEL,
    ICON_MINIMIZED
} IconKind;

struct DesktopIcon {
    Window window;
    IconKind kind;
    Client *client;
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
    /* Root-relative coordinates of the client's inside (drawable) corner. */
    int x;
    int y;
    int width;
    int height;
    int win_gravity;
    unsigned int saved_border;
    unsigned int expected_unmaps;
    bool minimized;
    DesktopIcon *icon;
    Client *next;
};

typedef struct {
    unsigned long black;
    unsigned long white;
    unsigned long silver;
    unsigned long dark_gray;
    unsigned long active_title;
    unsigned long desktop;
    unsigned long scheme_desktop[WIN31X_COLOR_SCHEME_COUNT];
    unsigned long scheme_active_title[WIN31X_COLOR_SCHEME_COUNT];
} Theme;

typedef struct {
    Pixmap color;
    Pixmap mask;
    unsigned int width;
    unsigned int height;
} RenderedIcon;

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
    Atom net_client_list;
    Atom net_active_window;
    Atom net_close_window;
    Atom net_frame_extents;
    Atom net_number_of_desktops;
    Atom net_current_desktop;
    Atom net_workarea;
    Atom net_wm_state;
    Atom net_wm_state_hidden;
    Atom win31x_role;
    Atom win31x_client;
} Atoms;

typedef enum {
    DRAG_NONE,
    DRAG_MOVE_CLIENT,
    DRAG_RESIZE_CLIENT,
    DRAG_MOVE_LAUNCHER,
    DRAG_MOVE_CONTROL_PANEL
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
    ControlSection section;
    int wifi_selected;
    int wifi_scroll;
    bool password_active;
    char password[CONTROL_PANEL_PASSWORD_CAPACITY];
    size_t password_length;
    char settings_status[160];
} ControlPanel;

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
    int edges;
    int start_root_x;
    int start_root_y;
    int start_x;
    int start_y;
    int start_width;
    int start_height;
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
    GC gc;
    XFontStruct *font;
    Cursor arrow_cursor;
    Cursor move_cursor;
    Cursor resize_cursor;
    Theme theme;
    IconAssets icon_assets;
    RenderedIcon rendered_icons[ICON_CATEGORY_COUNT][ICON_SIZE_COUNT];
    Win31xSettings settings;
    Win31xAutoLock auto_lock;
    WifiBackend wifi;
    ControlPanel control_panel;
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
static void update_focus_overlays(WindowManager *wm);

static bool internal_window_visible(const WindowManager *wm)
{
    return wm->launcher_visible || wm->control_panel.visible;
}

static void dismiss_internal_windows(WindowManager *wm)
{
    dismiss_launcher(wm);
    dismiss_control_panel(wm);
}

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

static void free_rendered_icons(WindowManager *wm)
{
    int category;
    int size;

    for (category = 0; category < ICON_CATEGORY_COUNT; ++category) {
        for (size = 0; size < ICON_SIZE_COUNT; ++size) {
            RenderedIcon *icon = &wm->rendered_icons[category][size];

            if (icon->color != None)
                XFreePixmap(wm->display, icon->color);
            if (icon->mask != None)
                XFreePixmap(wm->display, icon->mask);
            memset(icon, 0, sizeof(*icon));
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
    a->net_client_list = intern(d, "_NET_CLIENT_LIST");
    a->net_active_window = intern(d, "_NET_ACTIVE_WINDOW");
    a->net_close_window = intern(d, "_NET_CLOSE_WINDOW");
    a->net_frame_extents = intern(d, "_NET_FRAME_EXTENTS");
    a->net_number_of_desktops = intern(d, "_NET_NUMBER_OF_DESKTOPS");
    a->net_current_desktop = intern(d, "_NET_CURRENT_DESKTOP");
    a->net_workarea = intern(d, "_NET_WORKAREA");
    a->net_wm_state = intern(d, "_NET_WM_STATE");
    a->net_wm_state_hidden = intern(d, "_NET_WM_STATE_HIDDEN");
    a->win31x_role = intern(d, "_WIN31X_ROLE");
    a->win31x_client = intern(d, "_WIN31X_CLIENT");
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

static int close_button_x(const Client *client)
{
    return frame_width(client) - FRAME_RIGHT - TITLE_BUTTON - 2;
}

static int minimize_button_x(const Client *client)
{
    return close_button_x(client) - TITLE_BUTTON - 3;
}

static IconCategory client_icon_category(const Client *client)
{
    if (client == NULL)
        return ICON_CATEGORY_EXECUTABLE;
    return icon_assets_classify(client->title, client->class_name, NULL);
}

static void draw_title_button(WindowManager *wm, Window window, int x, int y,
                              bool close_button)
{
    XSetForeground(wm->display, wm->gc, wm->theme.silver);
    XFillRectangle(wm->display, window, wm->gc, x, y, TITLE_BUTTON, TITLE_BUTTON);
    draw_bevel(wm, window, x, y, TITLE_BUTTON, TITLE_BUTTON, false);
    XSetForeground(wm->display, wm->gc, wm->theme.black);
    if (close_button) {
        XDrawLine(wm->display, window, wm->gc, x + 5, y + 5, x + 11, y + 11);
        XDrawLine(wm->display, window, wm->gc, x + 11, y + 5, x + 5, y + 11);
        XDrawLine(wm->display, window, wm->gc, x + 6, y + 5, x + 12, y + 11);
        XDrawLine(wm->display, window, wm->gc, x + 12, y + 5, x + 6, y + 11);
    } else {
        XFillRectangle(wm->display, window, wm->gc, x + 4, y + 11, 9, 2);
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
    XSetForeground(wm->display, wm->gc,
                   client == wm->active ? wm->theme.active_title
                                        : wm->theme.dark_gray);
    XFillRectangle(wm->display, client->frame, wm->gc, FRAME_LEFT, TITLE_Y,
                   (unsigned)(width - FRAME_LEFT - FRAME_RIGHT), TITLE_HEIGHT);

    draw_supplied_icon(wm, client->frame, client_icon_category(client),
                       ICON_SIZE_SMALL, FRAME_LEFT + 2, TITLE_Y + 2);

    fitted_text(wm, client->title, title_end - 25, title, sizeof(title));
    XSetForeground(wm->display, wm->gc, wm->theme.white);
    XDrawString(wm->display, client->frame, wm->gc, FRAME_LEFT + 22,
                TITLE_Y + 14, title, (int)strlen(title));
    draw_title_button(wm, client->frame, minimize_button_x(client), TITLE_Y + 1,
                      false);
    draw_title_button(wm, client->frame, close_button_x(client), TITLE_Y + 1,
                      true);
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
    draw_supplied_icon_centered(wm, icon->window, category, ICON_SIZE_LARGE,
                                0, 5, ICON_WIDTH, 48);
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

static void set_hidden_state(WindowManager *wm, Client *client, bool hidden)
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
        if (states[index] == wm->atoms.net_wm_state_hidden) {
            already_present = true;
            if (!hidden)
                continue;
        }
        replacement[new_count++] = states[index];
    }
    if (hidden && !already_present)
        replacement[new_count++] = wm->atoms.net_wm_state_hidden;
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
    }
}

static void update_focus_overlays(WindowManager *wm)
{
    Client *client;
    bool active_needs_raise = !internal_window_visible(wm) &&
                              active_has_foreign_frame_above(wm);

    for (client = wm->clients; client != NULL; client = client->next) {
        bool intercept = !client->minimized &&
                         (client != wm->active || internal_window_visible(wm) ||
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
                                 bool raise, bool close_internal)
{
    Client *old_active = wm->active;

    if (client != NULL && client->minimized)
        return;
    if (close_internal && client != NULL && internal_window_visible(wm))
        dismiss_internal_windows(wm);
    wm->active = client;
    publish_active_client(wm, client);
    if (raise)
        raise_client_family(wm, client);
    update_focus_overlays(wm);
    if (old_active != NULL && old_active != client)
        draw_frame(wm, old_active);
    if (client != NULL)
        draw_frame(wm, client);
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
    int index = 0;
    int columns = (wm->screen_width - ICON_MARGIN * 2 + ICON_GAP) /
                  (ICON_WIDTH + ICON_GAP);

    if (columns < 1)
        columns = 1;
    for (icon = wm->icons; icon != NULL; icon = icon->next) {
        int x;
        int y;

        if (icon->kind == ICON_APPLICATIONS) {
            x = ICON_MARGIN;
            y = ICON_MARGIN;
        } else if (icon->kind == ICON_CONTROL_PANEL) {
            x = ICON_MARGIN;
            y = ICON_MARGIN + ICON_HEIGHT + ICON_GAP;
        } else {
            if (icon->client == NULL || !icon->client->minimized)
                continue;
            int column = index % columns;
            int row = index / columns;
            x = ICON_MARGIN + column * (ICON_WIDTH + ICON_GAP);
            y = wm->screen_height - ICON_MARGIN - ICON_HEIGHT -
                row * (ICON_HEIGHT + ICON_GAP);
            ++index;
        }
        XMoveWindow(wm->display, icon->window, x, y);
        XLowerWindow(wm->display, icon->window);
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
    attributes.event_mask = ExposureMask | ButtonPressMask;
    attributes.cursor = wm->arrow_cursor;
    icon->window = XCreateWindow(wm->display, wm->root, 0, 0, ICON_WIDTH,
                                 ICON_HEIGHT, 0, CopyFromParent, InputOutput,
                                 CopyFromParent, CWOverrideRedirect | CWBackPixel |
                                 CWEventMask | CWCursor, &attributes);
    icon->kind = kind;
    icon->client = client;
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

    if (client == NULL || client->minimized)
        return;
    change_active_client(wm, client, true, true);
    hints = XGetWMHints(wm->display, client->window);
    if (hints != NULL && (hints->flags & InputHint))
        accepts_input = hints->input != False;
    take_focus = client_supports_protocol(wm, client, wm->atoms.wm_take_focus);
    if (accepts_input)
        XSetInputFocus(wm->display, client->window, RevertToPointerRoot, time);
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
    else {
        if (internal_window_visible(wm))
            dismiss_internal_windows(wm);
        change_active_client(wm, NULL, false, false);
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
    wm->drag.kind = DRAG_NONE;
    wm->drag.client = NULL;
    XUngrabPointer(wm->display, CurrentTime);
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
        else
            XMapWindow(wm->display, client->icon->window);
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
    if (wm->active != NULL && wm->active->minimized)
        focus_next(wm, CurrentTime);
    reposition_icons(wm);
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
        else
            XMapWindow(wm->display, client->icon->window);
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
    if (!remap)
        expose_orphaned_transients(wm, client->window);
    was_active = wm->active == client;
    if (was_active)
        change_active_client(wm, NULL, false, false);
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
    free(client->title);
    free(client->class_name);
    free(client);
    update_client_list(wm);
    if (was_active)
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
    int outer_width = frame_width(client);
    int outer_height = frame_height(client);
    int frame_x = client->x - FRAME_LEFT;
    int frame_y = client->y - FRAME_TOP;

    if (frame_y < 0)
        client->y = FRAME_TOP;
    if (frame_x > wm->screen_width - 48)
        client->x = wm->screen_width - 48 + FRAME_LEFT;
    if (frame_x + outer_width < 48)
        client->x = 48 - outer_width + FRAME_LEFT;
    if (frame_y > wm->screen_height - TITLE_HEIGHT)
        client->y = wm->screen_height - TITLE_HEIGHT + FRAME_TOP;
    (void)outer_height;
}

static void handle_normal_hints_change(WindowManager *wm, Client *client)
{
    int width = client->width;
    int height = client->height;

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
}

static Client *manage_window(WindowManager *wm, Window window, bool startup)
{
    XWindowAttributes attributes;
    XSetWindowAttributes frame_attributes;
    XSetWindowAttributes overlay_attributes;
    XWMHints *hints;
    Client *client;
    bool initially_iconic = false;

    if (client_for_window(wm, window) != NULL ||
        !XGetWindowAttributes(wm->display, window, &attributes) ||
        attributes.override_redirect || attributes.class == InputOnly)
        return NULL;
    client = calloc(1, sizeof(*client));
    if (client == NULL)
        return NULL;
    client->window = window;
    client->title = window_title(wm, window);
    client->class_name = window_class(wm, window);
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
    set_frame_extents(wm, client);
    update_client_list(wm);

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
        send_configure_notify(wm, client);
        focus_client(wm, client, CurrentTime);
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
    XSetForeground(wm->display, wm->gc, wm->theme.active_title);
    XFillRectangle(wm->display, wm->launcher, wm->gc, 3, 3,
                   (unsigned)(width - 6), TITLE_HEIGHT);
    draw_supplied_icon(wm, wm->launcher, ICON_CATEGORY_APPLICATIONS,
                       ICON_SIZE_SMALL, 7, 5);
    XSetForeground(wm->display, wm->gc, wm->theme.white);
    XDrawString(wm->display, wm->launcher, wm->gc, 28, 17,
                "Applications", 12);
    draw_title_button(wm, wm->launcher, width - TITLE_BUTTON - 6, 4, true);

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

static void position_launcher(WindowManager *wm)
{
    int available_width = wm->screen_width - 24;
    int available_height = wm->screen_height - 24;

    wm->launcher_width = available_width >= 260
                             ? (available_width < LAUNCHER_DEFAULT_WIDTH
                                    ? available_width
                                    : LAUNCHER_DEFAULT_WIDTH)
                             : wm->screen_width;
    wm->launcher_height = available_height >= 180
                              ? (available_height < LAUNCHER_DEFAULT_HEIGHT
                                     ? available_height
                                     : LAUNCHER_DEFAULT_HEIGHT)
                              : wm->screen_height;
    if (wm->launcher_width < 1)
        wm->launcher_width = 1;
    if (wm->launcher_height < 1)
        wm->launcher_height = 1;
    clamp_launcher_scroll(wm);
    wm->launcher_x = (wm->screen_width - wm->launcher_width) / 2;
    wm->launcher_y = (wm->screen_height - wm->launcher_height) / 2;
    XMoveResizeWindow(wm->display, wm->launcher, wm->launcher_x, wm->launcher_y,
                      (unsigned)wm->launcher_width,
                      (unsigned)wm->launcher_height);
}

static void dismiss_launcher(WindowManager *wm)
{
    if (!wm->launcher_visible)
        return;
    wm->launcher_visible = false;
    wm->drag.kind = DRAG_NONE;
    wm->drag.client = NULL;
    XUngrabPointer(wm->display, CurrentTime);
    XUnmapWindow(wm->display, wm->launcher);
    update_focus_overlays(wm);
}

static void hide_launcher(WindowManager *wm)
{
    if (!wm->launcher_visible)
        return;
    dismiss_launcher(wm);
    if (wm->active != NULL)
        focus_client(wm, wm->active, CurrentTime);
    else
        XSetInputFocus(wm->display, wm->root, RevertToPointerRoot, CurrentTime);
}

static void show_launcher(WindowManager *wm)
{
    dismiss_control_panel(wm);
    apps_free(&wm->applications);
    if (apps_load(&wm->applications) < 0)
        fprintf(stderr, "win31x: could not load application entries: %s\n",
                strerror(errno));
    wm->launcher_scroll_row = 0;
    wm->launcher_selected = wm->applications.len == 0 ? -1 : 0;
    wm->launcher_last_click = -1;
    wm->launcher_visible = true;
    update_focus_overlays(wm);
    position_launcher(wm);
    XMapRaised(wm->display, wm->launcher);
    XSetInputFocus(wm->display, wm->launcher, RevertToPointerRoot, CurrentTime);
    draw_launcher(wm);
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
    hide_launcher(wm);
}

static void initialize_launcher(WindowManager *wm)
{
    XSetWindowAttributes attributes;

    wm->launcher_width = LAUNCHER_DEFAULT_WIDTH;
    wm->launcher_height = LAUNCHER_DEFAULT_HEIGHT;
    wm->launcher_selected = -1;
    wm->launcher_last_click = -1;
    attributes.override_redirect = True;
    attributes.background_pixel = wm->theme.silver;
    attributes.event_mask = ExposureMask | ButtonPressMask | ButtonReleaseMask |
                            PointerMotionMask | KeyPressMask;
    attributes.cursor = wm->arrow_cursor;
    wm->launcher = XCreateWindow(wm->display, wm->root, 0, 0,
                                 (unsigned)wm->launcher_width,
                                 (unsigned)wm->launcher_height, 0,
                                 CopyFromParent, InputOutput, CopyFromParent,
                                 CWOverrideRedirect | CWBackPixel | CWEventMask |
                                 CWCursor, &attributes);
    set_internal_role(wm, wm->launcher, "applications-window");
    set_utf8_property(wm, wm->launcher, wm->atoms.net_wm_name, "Applications");
    position_launcher(wm);
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
    XSetForeground(wm->display, wm->gc, wm->theme.active_title);
    XFillRectangle(wm->display, panel->window, wm->gc, 3, 3,
                   (unsigned)(panel->width - 6), TITLE_HEIGHT);
    draw_supplied_icon(wm, panel->window, ICON_CATEGORY_SETTINGS,
                       ICON_SIZE_SMALL, 7, 5);
    draw_text(wm, panel->window, 28, 17, wm->theme.white, "Control Panel");
    draw_title_button(wm, panel->window,
                      panel->width - TITLE_BUTTON - 6, 4, true);
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

static void position_control_panel(WindowManager *wm)
{
    ControlPanel *panel = &wm->control_panel;
    int available_width = wm->screen_width - 24;
    int available_height = wm->screen_height - 24;

    panel->width = available_width >= 320
                       ? (available_width < CONTROL_PANEL_DEFAULT_WIDTH
                              ? available_width
                              : CONTROL_PANEL_DEFAULT_WIDTH)
                       : wm->screen_width;
    panel->height = available_height >= 300
                        ? (available_height < CONTROL_PANEL_DEFAULT_HEIGHT
                               ? available_height
                               : CONTROL_PANEL_DEFAULT_HEIGHT)
                        : wm->screen_height;
    if (panel->width < 1)
        panel->width = 1;
    if (panel->height < 1)
        panel->height = 1;
    if (!control_wifi_layout_available(panel))
        clear_control_password(panel);
    panel->x = (wm->screen_width - panel->width) / 2;
    panel->y = (wm->screen_height - panel->height) / 2;
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
    wm->drag.kind = DRAG_NONE;
    wm->drag.client = NULL;
    XUngrabPointer(wm->display, CurrentTime);
    XUnmapWindow(wm->display, panel->window);
    update_focus_overlays(wm);
}

static void hide_control_panel(WindowManager *wm)
{
    if (!wm->control_panel.visible)
        return;
    dismiss_control_panel(wm);
    if (wm->active != NULL)
        focus_client(wm, wm->active, CurrentTime);
    else
        XSetInputFocus(wm->display, wm->root, RevertToPointerRoot,
                       CurrentTime);
}

static void show_control_panel(WindowManager *wm)
{
    ControlPanel *panel = &wm->control_panel;

    dismiss_launcher(wm);
    clear_control_password(panel);
    panel->visible = true;
    update_focus_overlays(wm);
    position_control_panel(wm);
    XMapRaised(wm->display, panel->window);
    XSetInputFocus(wm->display, panel->window, RevertToPointerRoot,
                   CurrentTime);
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
    panel->section = CONTROL_SECTION_WIFI;
    panel->wifi_selected = -1;
    attributes.override_redirect = True;
    attributes.background_pixel = wm->theme.silver;
    attributes.event_mask = ExposureMask | ButtonPressMask | ButtonReleaseMask |
                            PointerMotionMask | KeyPressMask;
    attributes.cursor = wm->arrow_cursor;
    panel->window = XCreateWindow(
        wm->display, wm->root, 0, 0, (unsigned)panel->width,
        (unsigned)panel->height, 0, CopyFromParent, InputOutput, CopyFromParent,
        CWOverrideRedirect | CWBackPixel | CWEventMask | CWCursor, &attributes);
    set_internal_role(wm, panel->window, "control-panel-window");
    set_utf8_property(wm, panel->window, wm->atoms.net_wm_name,
                      "Control Panel");
    position_control_panel(wm);
}

static void raise_visible_internal_window(WindowManager *wm)
{
    if (wm->launcher_visible)
        XRaiseWindow(wm->display, wm->launcher);
    else if (wm->control_panel.visible)
        XRaiseWindow(wm->display, wm->control_panel.window);
}

static void initialize_ewmh(WindowManager *wm)
{
    Atom supported[] = {
        wm->atoms.net_supporting_wm_check,
        wm->atoms.net_wm_name,
        wm->atoms.net_client_list,
        wm->atoms.net_active_window,
        wm->atoms.net_close_window,
        wm->atoms.net_frame_extents,
        wm->atoms.net_number_of_desktops,
        wm->atoms.net_current_desktop,
        wm->atoms.net_workarea,
        wm->atoms.net_wm_state,
        wm->atoms.net_wm_state_hidden
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
    KeyCode num_lock = XKeysymToKeycode(wm->display, XK_Num_Lock);
    XModifierKeymap *mapping = XGetModifierMapping(wm->display);
    unsigned num_lock_mask = 0;
    unsigned lock_combinations[4];
    size_t index;
    int modifier;
    int key;

    if (mapping != NULL) {
        for (modifier = 0; modifier < 8; ++modifier) {
            for (key = 0; key < mapping->max_keypermod; ++key) {
                if (num_lock != 0 &&
                    mapping->modifiermap[modifier * mapping->max_keypermod + key] ==
                        num_lock)
                    num_lock_mask = 1U << modifier;
            }
        }
        XFreeModifiermap(mapping);
    }
    lock_combinations[0] = 0;
    lock_combinations[1] = LockMask;
    lock_combinations[2] = num_lock_mask;
    lock_combinations[3] = LockMask | num_lock_mask;

    XUngrabKey(wm->display, AnyKey, AnyModifier, wm->root);
    for (index = 0; index < sizeof(lock_combinations) /
                                  sizeof(lock_combinations[0]); ++index) {
        unsigned modifiers = Mod1Mask | lock_combinations[index];
        XGrabKey(wm->display, tab, modifiers, wm->root, True,
                 GrabModeAsync, GrabModeAsync);
        XGrabKey(wm->display, f2, modifiers, wm->root, True,
                 GrabModeAsync, GrabModeAsync);
        XGrabKey(wm->display, f4, modifiers, wm->root, True,
                 GrabModeAsync, GrabModeAsync);
    }
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
    if (event->value_mask & CWWidth)
        client->width = event->width;
    if (event->value_mask & CWHeight)
        client->height = event->height;
    if (event->value_mask & CWBorderWidth)
        client->saved_border = (unsigned)event->border_width;
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
    raise_visible_internal_window(wm);
    if (event->value_mask & (CWSibling | CWStackMode))
        update_focus_overlays(wm);
    send_configure_notify(wm, client);
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

static void start_drag(WindowManager *wm, DragKind kind, Client *client, int edges,
                       int root_x, int root_y)
{
    wm->drag.kind = kind;
    wm->drag.client = client;
    wm->drag.edges = edges;
    wm->drag.start_root_x = root_x;
    wm->drag.start_root_y = root_y;
    if (client != NULL) {
        wm->drag.start_x = client->x;
        wm->drag.start_y = client->y;
        wm->drag.start_width = client->width;
        wm->drag.start_height = client->height;
    } else if (kind == DRAG_MOVE_CONTROL_PANEL) {
        wm->drag.start_x = wm->control_panel.x;
        wm->drag.start_y = wm->control_panel.y;
    } else {
        wm->drag.start_x = wm->launcher_x;
        wm->drag.start_y = wm->launcher_y;
    }
    if (XGrabPointer(wm->display, wm->root, False,
                     PointerMotionMask | ButtonReleaseMask, GrabModeAsync,
                     GrabModeAsync, None,
                     kind == DRAG_RESIZE_CLIENT ? wm->resize_cursor : wm->move_cursor,
                     CurrentTime) != GrabSuccess) {
        wm->drag.kind = DRAG_NONE;
        wm->drag.client = NULL;
    }
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
        if (event->x >= minimize_button_x(client) &&
            event->x < minimize_button_x(client) + TITLE_BUTTON) {
            minimize_client(wm, client);
            return;
        }
        start_drag(wm, DRAG_MOVE_CLIENT, client, 0,
                   event->x_root, event->y_root);
        return;
    }
    edges = frame_resize_edges(client, event->x, event->y);
    if (edges != 0)
        start_drag(wm, DRAG_RESIZE_CLIENT, client, edges,
                   event->x_root, event->y_root);
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
    if (event->y < 25 && event->x >= wm->launcher_width - TITLE_BUTTON - 8) {
        hide_launcher(wm);
        return;
    }
    if (event->y < 25) {
        start_drag(wm, DRAG_MOVE_LAUNCHER, NULL, 0,
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
        event->x >= panel->width - TITLE_BUTTON - 8) {
        hide_control_panel(wm);
        return;
    }
    if (event->y < 25) {
        start_drag(wm, DRAG_MOVE_CONTROL_PANEL, NULL, 0,
                   event->x_root, event->y_root);
        return;
    }
    for (section = 0; section < CONTROL_SECTION_COUNT; ++section) {
        if (point_in_rectangle(event->x, event->y, 7,
                               control_nav_item_y(panel, section),
                               CONTROL_PANEL_NAV_WIDTH - 7,
                               control_nav_item_height(panel))) {
            panel->section = (ControlSection)section;
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

static void handle_button_press(WindowManager *wm, XButtonEvent *event)
{
    DesktopIcon *icon = icon_for_window(wm, event->window);
    Client *client;

    if (icon != NULL && event->button == Button1) {
        if (icon->kind == ICON_APPLICATIONS)
            show_launcher(wm);
        else if (icon->kind == ICON_CONTROL_PANEL)
            show_control_panel(wm);
        else
            restore_client(wm, icon->client, event->time);
        return;
    }
    if (event->window == wm->launcher) {
        handle_launcher_button(wm, event);
        return;
    }
    if (event->window == wm->control_panel.window) {
        handle_control_panel_button(wm, event);
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

    if (wm->drag.kind == DRAG_NONE)
        return;
    dx = event->x_root - wm->drag.start_root_x;
    dy = event->y_root - wm->drag.start_root_y;
    if (wm->drag.kind == DRAG_MOVE_LAUNCHER) {
        wm->launcher_x = wm->drag.start_x + dx;
        wm->launcher_y = wm->drag.start_y + dy;
        if (wm->launcher_x > wm->screen_width - 48)
            wm->launcher_x = wm->screen_width - 48;
        if (wm->launcher_x + wm->launcher_width < 48)
            wm->launcher_x = 48 - wm->launcher_width;
        if (wm->launcher_y < 0)
            wm->launcher_y = 0;
        if (wm->launcher_y > wm->screen_height - TITLE_HEIGHT)
            wm->launcher_y = wm->screen_height - TITLE_HEIGHT;
        XMoveWindow(wm->display, wm->launcher, wm->launcher_x, wm->launcher_y);
        return;
    }
    if (wm->drag.kind == DRAG_MOVE_CONTROL_PANEL) {
        ControlPanel *panel = &wm->control_panel;

        panel->x = wm->drag.start_x + dx;
        panel->y = wm->drag.start_y + dy;
        if (panel->x > wm->screen_width - 48)
            panel->x = wm->screen_width - 48;
        if (panel->x + panel->width < 48)
            panel->x = 48 - panel->width;
        if (panel->y < 0)
            panel->y = 0;
        if (panel->y > wm->screen_height - TITLE_HEIGHT)
            panel->y = wm->screen_height - TITLE_HEIGHT;
        XMoveWindow(wm->display, panel->window, panel->x, panel->y);
        return;
    }
    if (wm->drag.client == NULL)
        return;
    if (wm->drag.kind == DRAG_MOVE_CLIENT) {
        wm->drag.client->x = wm->drag.start_x + dx;
        wm->drag.client->y = wm->drag.start_y + dy;
        keep_client_on_screen(wm, wm->drag.client);
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

static void handle_key_press(WindowManager *wm, XKeyEvent *event)
{
    KeySym key = XLookupKeysym(event, 0);

    if (event->window == wm->control_panel.window) {
        if ((event->state & Mod1Mask) != 0U && key == XK_F2) {
            show_launcher(wm);
            return;
        }
        if ((event->state & Mod1Mask) != 0U && key == XK_Tab) {
            focus_next(wm, event->time);
            return;
        }
        if (key == XK_Escape ||
            (key == XK_F4 && (event->state & Mod1Mask) != 0U)) {
            hide_control_panel(wm);
            return;
        }
        if (key == XK_Left && wm->control_panel.section > 0) {
            --wm->control_panel.section;
            clear_control_password(&wm->control_panel);
            if (wm->control_panel.section == CONTROL_SECTION_WIFI)
                start_wifi_scan(wm);
            else
                draw_control_panel(wm);
            return;
        } else if (key == XK_Right &&
                   wm->control_panel.section + 1 < CONTROL_SECTION_COUNT) {
            ++wm->control_panel.section;
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
    if (key == XK_Escape) {
        hide_launcher(wm);
        return;
    }
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

static void update_client_class(WindowManager *wm, Client *client)
{
    char *class_name = window_class(wm, client->window);

    if (class_name == NULL)
        return;
    free(client->class_name);
    client->class_name = class_name;
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
    } else if (event->message_type == wm->atoms.net_wm_state && event->format == 32) {
        bool mentions_hidden = (Atom)event->data.l[1] == wm->atoms.net_wm_state_hidden ||
                               (Atom)event->data.l[2] == wm->atoms.net_wm_state_hidden;
        long action = event->data.l[0];

        if (mentions_hidden) {
            if (action == 1 || (action == 2 && !client->minimized))
                minimize_client(wm, client);
            else if (action == 0 || (action == 2 && client->minimized))
                restore_client(wm, client, CurrentTime);
        }
    }
}

static void handle_root_resize(WindowManager *wm, int width, int height)
{
    long workarea[4];
    Client *client;

    wm->screen_width = width;
    wm->screen_height = height;
    workarea[0] = 0;
    workarea[1] = 0;
    workarea[2] = wm->screen_width;
    workarea[3] = wm->screen_height;
    XChangeProperty(wm->display, wm->root, wm->atoms.net_workarea, XA_CARDINAL,
                    32, PropModeReplace, (unsigned char *)workarea, 4);
    for (client = wm->clients; client != NULL; client = client->next) {
        int old_x = client->x;
        int old_y = client->y;

        keep_client_on_screen(wm, client);
        if (client->x != old_x || client->y != old_y) {
            apply_client_geometry(wm, client);
            send_configure_notify(wm, client);
        }
    }
    reposition_icons(wm);
    if (wm->launcher_visible) {
        position_launcher(wm);
        draw_launcher(wm);
    }
    if (wm->control_panel.visible) {
        position_control_panel(wm);
        draw_control_panel(wm);
    }
}

static void dispatch_event(WindowManager *wm, XEvent *event)
{
    Client *client;
    DesktopIcon *icon;

    switch (event->type) {
    case MapRequest:
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
        if (wm->drag.kind != DRAG_NONE) {
            wm->drag.kind = DRAG_NONE;
            wm->drag.client = NULL;
            XUngrabPointer(wm->display, event->xbutton.time);
        }
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
    case PropertyNotify:
        client = client_for_client_window(wm, event->xproperty.window);
        if (client != NULL &&
            (event->xproperty.atom == XA_WM_NAME ||
             event->xproperty.atom == wm->atoms.net_wm_name ||
             event->xproperty.atom == wm->atoms.net_wm_icon_name))
            update_client_title(wm, client);
        else if (client != NULL && event->xproperty.atom == XA_WM_CLASS)
            update_client_class(wm, client);
        else if (client != NULL &&
                 event->xproperty.atom == XA_WM_NORMAL_HINTS)
            handle_normal_hints_change(wm, client);
        else if (client != NULL &&
                 event->xproperty.atom == XA_WM_TRANSIENT_FOR)
            refresh_client_transient_for(wm, client);
        break;
    case FocusIn:
        client = client_for_client_window(wm, event->xfocus.window);
        if (client != NULL && !client->minimized &&
            !event->xfocus.send_event && event->xfocus.mode != NotifyGrab &&
            event->xfocus.mode != NotifyUngrab &&
            (event->xfocus.detail == NotifyAncestor ||
             event->xfocus.detail == NotifyVirtual ||
             event->xfocus.detail == NotifyNonlinear ||
             event->xfocus.detail == NotifyNonlinearVirtual) &&
            client_currently_has_focus(wm, client))
            change_active_client(wm, client, true, true);
        break;
    case ClientMessage:
        handle_client_message(wm, &event->xclient);
        break;
    case ConfigureNotify:
        if (event->xconfigure.window == wm->root &&
            (event->xconfigure.width != wm->screen_width ||
             event->xconfigure.height != wm->screen_height))
            handle_root_resize(wm, event->xconfigure.width,
                                event->xconfigure.height);
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
    initialize_atoms(wm);
    if (win31x_settings_load(&wm->settings) < 0) {
        fprintf(stderr, "win31x: could not load settings: %s\n",
                strerror(errno));
        win31x_settings_defaults(&wm->settings);
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
                 PropertyChangeMask | StructureNotifyMask | KeyPressMask);
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

    adopt_existing_windows(wm);
    initialize_launcher(wm);
    initialize_control_panel(wm);
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

            if (wifi_handled && wm->control_panel.visible &&
                wm->control_panel.section == CONTROL_SECTION_WIFI) {
                clamp_wifi_selection(wm);
                draw_control_panel(wm);
            }
            if (lock_handled && wm->control_panel.visible &&
                wm->control_panel.section == CONTROL_SECTION_AUTO_LOCK)
                draw_control_panel(wm);
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
        while (keep_running && XPending(wm->display) > 0) {
            XEvent event;
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
        }
    }
}

static void shut_down(WindowManager *wm)
{
    wifi_backend_destroy(&wm->wifi);
    win31x_auto_lock_shutdown(&wm->auto_lock);
    while (wm->clients != NULL)
        remove_client(wm, wm->clients, false, true, false);
    while (wm->icons != NULL)
        destroy_desktop_icon(wm, wm->icons);
    apps_free(&wm->applications);
    clear_control_password(&wm->control_panel);
    if (wm->control_panel.window != None)
        XDestroyWindow(wm->display, wm->control_panel.window);
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
