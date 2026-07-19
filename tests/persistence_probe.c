#define _POSIX_C_SOURCE 200809L

#include <X11/Xatom.h>
#include <X11/Xlib.h>
#include <X11/Xutil.h>
#include <X11/extensions/XTest.h>
#include <X11/keysym.h>

#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

enum {
    ICON_WIDTH = 112,
    ICON_HEIGHT = 80,
    FRAME_LEFT = 3,
    FRAME_RIGHT = 3,
    FRAME_TOP = 25,
    FRAME_BOTTOM = 3,
    TITLE_BUTTON = 17,
    TITLE_BUTTON_GAP = 3,
    TITLE_BUTTON_RIGHT_INSET = 3,
    TITLE_BUTTON_Y = 4,
    WAIT_ATTEMPTS = 250
};

typedef struct {
    int x;
    int y;
    int width;
    int height;
} Geometry;

static const Geometry dual_bounds = {0, 0, 1600, 700};
static const Geometry single_bounds = {0, 0, 640, 480};
static const Geometry applications_icon_dual = {
    1040, 260, ICON_WIDTH, ICON_HEIGHT
};
static const Geometry control_icon_dual = {
    1488, 620, ICON_WIDTH, ICON_HEIGHT
};
static const Geometry launcher_dual = {800, 100, 400, 600};
static const Geometry panel_dual = {1200, 100, 400, 600};
static const Geometry run_dual = {950, 180, 470, 176};
static const Geometry client_dual = {
    1030, 330, 310 + FRAME_LEFT + FRAME_RIGHT,
    170 + FRAME_TOP + FRAME_BOTTOM
};
static const Geometry applications_icon_single = {
    240, 160, ICON_WIDTH, ICON_HEIGHT
};
static const Geometry control_icon_single = {
    640 - ICON_WIDTH, 480 - ICON_HEIGHT, ICON_WIDTH, ICON_HEIGHT
};
static const Geometry launcher_single = {0, 0, 320, 480};
static const Geometry panel_single = {320, 0, 320, 480};
static const Geometry run_single = {150, 80, 470, 176};
static const Geometry client_single = {
    230, 230, 310 + FRAME_LEFT + FRAME_RIGHT,
    170 + FRAME_TOP + FRAME_BOTTOM
};
static const Geometry duplicate_first_dual = {
    850, 150, 220 + FRAME_LEFT + FRAME_RIGHT,
    100 + FRAME_TOP + FRAME_BOTTOM
};
static const Geometry duplicate_second_dual = {
    1180, 400, 240 + FRAME_LEFT + FRAME_RIGHT,
    120 + FRAME_TOP + FRAME_BOTTOM
};
static const Geometry duplicate_first_single = {
    50, 50, 220 + FRAME_LEFT + FRAME_RIGHT,
    100 + FRAME_TOP + FRAME_BOTTOM
};
static const Geometry duplicate_second_single = {
    380, 300, 240 + FRAME_LEFT + FRAME_RIGHT,
    120 + FRAME_TOP + FRAME_BOTTOM
};
static const Geometry late_identity_dual = {
    1070, 220, 280 + FRAME_LEFT + FRAME_RIGHT,
    130 + FRAME_TOP + FRAME_BOTTOM
};
static const Geometry late_identity_single = {
    270, 120, 280 + FRAME_LEFT + FRAME_RIGHT,
    130 + FRAME_TOP + FRAME_BOTTOM
};
static const Geometry primary_full_dual = {0, 0, 800, 600};
static const Geometry max_snap_full_dual = {800, 100, 800, 600};
static const Geometry max_snap_left_dual = {800, 100, 400, 600};
static const Geometry max_snap_full_single = {0, 0, 640, 480};
static const Geometry max_snap_left_single = {0, 0, 320, 480};

static Display *display;
static Window root;
static Atom role_atom;
static Atom supporting_atom;

static void wait_a_bit(void)
{
    struct timespec delay = {0, 10000000};

    (void)nanosleep(&delay, NULL);
}

static int geometry_equal(const Geometry *left, const Geometry *right)
{
    return left->x == right->x && left->y == right->y &&
           left->width == right->width && left->height == right->height;
}

static int geometry_inside(const Geometry *item, const Geometry *bounds)
{
    long long right;
    long long bottom;

    if (item->width < 1 || item->height < 1)
        return 0;
    right = (long long)item->x + item->width;
    bottom = (long long)item->y + item->height;
    return item->x >= bounds->x && item->y >= bounds->y &&
           right <= (long long)bounds->x + bounds->width &&
           bottom <= (long long)bounds->y + bounds->height;
}

static int get_geometry(Window window, Geometry *geometry)
{
    XWindowAttributes attributes;

    if (geometry == NULL || !XGetWindowAttributes(display, window,
                                                   &attributes))
        return -1;
    geometry->x = attributes.x;
    geometry->y = attributes.y;
    geometry->width = attributes.width;
    geometry->height = attributes.height;
    return 0;
}

static void report_geometry(const char *message, Window window,
                            const Geometry *expected)
{
    Geometry actual = {0, 0, 0, 0};

    if (get_geometry(window, &actual) < 0) {
        fprintf(stderr, "persistence-probe: %s (window unavailable)\n",
                message);
        return;
    }
    fprintf(stderr,
            "persistence-probe: %s (got %d,%d %dx%d; expected %d,%d %dx%d)\n",
            message, actual.x, actual.y, actual.width, actual.height,
            expected->x, expected->y, expected->width, expected->height);
}

static int wait_for_geometry(Window window, const Geometry *expected)
{
    int attempt;

    for (attempt = 0; attempt < WAIT_ATTEMPTS; ++attempt) {
        Geometry actual;

        XSync(display, False);
        if (get_geometry(window, &actual) == 0 &&
            geometry_equal(&actual, expected))
            return 0;
        wait_a_bit();
    }
    return -1;
}

static int role_matches(Window window, const char *role)
{
    Atom type;
    int format;
    unsigned long count;
    unsigned long after;
    unsigned char *value = NULL;
    int matches = 0;

    if (XGetWindowProperty(display, window, role_atom, 0, 128, False,
                           AnyPropertyType, &type, &format, &count, &after,
                           &value) == Success &&
        format == 8 && value != NULL && strlen(role) == count &&
        memcmp(value, role, count) == 0)
        matches = 1;
    if (value != NULL)
        XFree(value);
    return matches;
}

static Window find_role(const char *role, bool viewable)
{
    Window root_return;
    Window parent_return;
    Window *children = NULL;
    unsigned int count = 0U;
    unsigned int index;
    Window result = None;

    if (!XQueryTree(display, root, &root_return, &parent_return, &children,
                    &count))
        return None;
    for (index = 0U; index < count; ++index) {
        XWindowAttributes attributes;

        if (!role_matches(children[index], role))
            continue;
        if (viewable &&
            (!XGetWindowAttributes(display, children[index], &attributes) ||
             attributes.map_state != IsViewable))
            continue;
        result = children[index];
        break;
    }
    if (children != NULL)
        XFree(children);
    return result;
}

static Window wait_for_role(const char *role, bool viewable)
{
    int attempt;

    for (attempt = 0; attempt < WAIT_ATTEMPTS; ++attempt) {
        Window window = find_role(role, viewable);

        if (window != None)
            return window;
        wait_a_bit();
    }
    return None;
}

static int role_remains_hidden(const char *role)
{
    int attempt;

    for (attempt = 0; attempt < 30; ++attempt) {
        XSync(display, False);
        if (find_role(role, true) != None)
            return 0;
        wait_a_bit();
    }
    return 1;
}

static int wait_for_manager(void)
{
    int attempt;

    for (attempt = 0; attempt < WAIT_ATTEMPTS; ++attempt) {
        Atom type;
        int format;
        unsigned long count;
        unsigned long after;
        unsigned char *value = NULL;
        int ready;

        ready = XGetWindowProperty(display, root, supporting_atom, 0, 1,
                                   False, XA_WINDOW, &type, &format, &count,
                                   &after, &value) == Success &&
                type == XA_WINDOW && format == 32 && count == 1U &&
                value != NULL;
        if (value != NULL)
            XFree(value);
        if (ready)
            return 0;
        wait_a_bit();
    }
    return -1;
}

static int xtest_ready(void)
{
    int event_base;
    int error_base;
    int major;
    int minor;

    return XTestQueryExtension(display, &event_base, &error_base, &major,
                               &minor);
}

static int fake_click(int root_x, int root_y)
{
    if (!XTestFakeMotionEvent(display, DefaultScreen(display), root_x, root_y,
                              CurrentTime))
        return -1;
    XSync(display, False);
    wait_a_bit();
    if (!XTestFakeButtonEvent(display, Button1, True, CurrentTime))
        return -1;
    XSync(display, False);
    wait_a_bit();
    if (!XTestFakeButtonEvent(display, Button1, False, CurrentTime))
        return -1;
    XSync(display, False);
    return 0;
}

static int fake_key_chord(KeySym modifier, KeySym key)
{
    KeyCode modifier_code = XKeysymToKeycode(display, modifier);
    KeyCode key_code = XKeysymToKeycode(display, key);

    if (modifier_code == 0 || key_code == 0 ||
        !XTestFakeKeyEvent(display, modifier_code, True, CurrentTime) ||
        !XTestFakeKeyEvent(display, key_code, True, CurrentTime) ||
        !XTestFakeKeyEvent(display, key_code, False, CurrentTime) ||
        !XTestFakeKeyEvent(display, modifier_code, False, CurrentTime))
        return -1;
    XSync(display, False);
    return 0;
}

static int click_window(Window window)
{
    Geometry geometry;

    if (get_geometry(window, &geometry) < 0)
        return -1;
    return fake_click(geometry.x + geometry.width / 2,
                      geometry.y + geometry.height / 2);
}

static int click_frame_maximize(Window frame)
{
    Geometry geometry;
    int close_x;
    int maximize_x;

    if (get_geometry(frame, &geometry) < 0)
        return -1;
    close_x = geometry.width - FRAME_RIGHT - TITLE_BUTTON -
              TITLE_BUTTON_RIGHT_INSET;
    maximize_x = close_x - TITLE_BUTTON - TITLE_BUTTON_GAP;
    return fake_click(geometry.x + maximize_x + TITLE_BUTTON / 2,
                      geometry.y + TITLE_BUTTON_Y + TITLE_BUTTON / 2);
}

static int fake_drag(int start_x, int start_y, int end_x, int end_y)
{
    int intermediate_x = start_x;
    int intermediate_y = start_y;

    if (!XTestFakeMotionEvent(display, DefaultScreen(display), start_x,
                              start_y, CurrentTime))
        return -1;
    XSync(display, False);
    wait_a_bit();
    if (!XTestFakeButtonEvent(display, Button1, True, CurrentTime))
        return -1;
    XSync(display, False);
    wait_a_bit();
    if (end_x != start_x)
        intermediate_x += end_x > start_x ? 8 : -8;
    else
        intermediate_y += end_y > start_y ? 8 : -8;
    if (!XTestFakeMotionEvent(display, DefaultScreen(display), intermediate_x,
                              intermediate_y, CurrentTime)) {
        (void)XTestFakeButtonEvent(display, Button1, False, CurrentTime);
        return -1;
    }
    XSync(display, False);
    wait_a_bit();
    if (!XTestFakeMotionEvent(display, DefaultScreen(display), end_x, end_y,
                              CurrentTime)) {
        (void)XTestFakeButtonEvent(display, Button1, False, CurrentTime);
        return -1;
    }
    XSync(display, False);
    wait_a_bit();
    if (!XTestFakeButtonEvent(display, Button1, False, CurrentTime))
        return -1;
    XSync(display, False);
    return 0;
}

static int drag_window_to(Window window, const Geometry *target)
{
    Geometry current;

    if (get_geometry(window, &current) < 0)
        return -1;
    return fake_drag(current.x + current.width / 2,
                     current.y + current.height / 2,
                     target->x + target->width / 2,
                     target->y + target->height / 2);
}

static int drag_title_to(Window window, int root_x, int root_y)
{
    Geometry geometry;

    if (get_geometry(window, &geometry) < 0)
        return -1;
    return fake_drag(geometry.x + geometry.width / 2, geometry.y + 12,
                     root_x, root_y);
}

static int drag_title_to_geometry(Window window, const Geometry *target)
{
    return drag_title_to(window, target->x + target->width / 2,
                         target->y + 12);
}

static unsigned long named_color_pixel(const char *name)
{
    XColor screen_color;
    XColor exact_color;

    if (!XAllocNamedColor(display,
                          DefaultColormap(display, DefaultScreen(display)),
                          name, &screen_color, &exact_color))
        return ~0UL;
    return screen_color.pixel;
}

static unsigned long root_pixel_at(int x, int y)
{
    XImage *image = XGetImage(display, root, x, y, 1U, 1U, AllPlanes,
                              ZPixmap);
    unsigned long pixel;

    if (image == NULL)
        return ~0UL;
    pixel = XGetPixel(image, 0, 0);
    XDestroyImage(image);
    return pixel;
}

static int wait_for_root_color(const char *name, int x, int y)
{
    unsigned long expected = named_color_pixel(name);
    int attempt;

    if (expected == ~0UL)
        return -1;
    for (attempt = 0; attempt < WAIT_ATTEMPTS; ++attempt) {
        XSync(display, False);
        if (root_pixel_at(x, y) == expected)
            return 0;
        wait_a_bit();
    }
    return -1;
}

static Window client_frame(Window client)
{
    Window root_return;
    Window parent = None;
    Window *children = NULL;
    unsigned int count = 0U;

    if (!XQueryTree(display, client, &root_return, &parent, &children,
                    &count))
        parent = None;
    if (children != NULL)
        XFree(children);
    return parent;
}

static Window wait_for_client_frame(Window client)
{
    int attempt;

    for (attempt = 0; attempt < WAIT_ATTEMPTS; ++attempt) {
        Window frame;

        XSync(display, False);
        frame = client_frame(client);
        if (frame != None && frame != root &&
            role_matches(frame, "client-frame"))
            return frame;
        wait_a_bit();
    }
    return None;
}

static Window create_class_client(int x, int y, int width, int height,
                                  const char *resource_name,
                                  const char *resource_class)
{
    int screen = DefaultScreen(display);
    XClassHint class_hint;
    Window client = XCreateSimpleWindow(
        display, root, x, y, (unsigned)width, (unsigned)height, 0,
        BlackPixel(display, screen), WhitePixel(display, screen));

    class_hint.res_name = (char *)resource_name;
    class_hint.res_class = (char *)resource_class;
    XSetClassHint(display, client, &class_hint);
    XStoreName(display, client, "Win31 X persistence probe client");
    XMapWindow(display, client);
    XSync(display, False);
    return client;
}

static void set_initial_maximized_state(Window client)
{
    Atom net_wm_state = XInternAtom(display, "_NET_WM_STATE", False);
    Atom maximized_horz =
        XInternAtom(display, "_NET_WM_STATE_MAXIMIZED_HORZ", False);
    Atom maximized_vert =
        XInternAtom(display, "_NET_WM_STATE_MAXIMIZED_VERT", False);
    Atom states[2] = {maximized_horz, maximized_vert};

    XChangeProperty(display, client, net_wm_state, XA_ATOM, 32,
                    PropModeReplace, (const unsigned char *)states, 2);
}

static Window create_initially_maximized_class_client(
    int x, int y, int width, int height, const char *resource_name,
    const char *resource_class)
{
    int screen = DefaultScreen(display);
    XClassHint class_hint;
    Window client = XCreateSimpleWindow(
        display, root, x, y, (unsigned)width, (unsigned)height, 0,
        BlackPixel(display, screen), WhitePixel(display, screen));

    class_hint.res_name = (char *)resource_name;
    class_hint.res_class = (char *)resource_class;
    XSetClassHint(display, client, &class_hint);
    XStoreName(display, client, "Win31 X persistence probe client");
    set_initial_maximized_state(client);
    XMapWindow(display, client);
    XSync(display, False);
    return client;
}

static Window create_test_client(int x, int y, int width, int height)
{
    return create_class_client(x, y, width, height,
                               "win31x-persistence-probe",
                               "Win31xPersistenceProbe");
}

static Window create_unidentified_client(int x, int y, int width, int height)
{
    int screen = DefaultScreen(display);
    Window client = XCreateSimpleWindow(
        display, root, x, y, (unsigned)width, (unsigned)height, 0,
        BlackPixel(display, screen), WhitePixel(display, screen));

    XStoreName(display, client, "Win31 X late identity probe client");
    XMapWindow(display, client);
    XSync(display, False);
    return client;
}

static Window create_initially_maximized_unidentified_client(
    int x, int y, int width, int height)
{
    int screen = DefaultScreen(display);
    Window client = XCreateSimpleWindow(
        display, root, x, y, (unsigned)width, (unsigned)height, 0,
        BlackPixel(display, screen), WhitePixel(display, screen));

    XStoreName(display, client, "Win31 X late identity maximize probe");
    set_initial_maximized_state(client);
    XMapWindow(display, client);
    XSync(display, False);
    return client;
}

static int request_client_geometry(Window client, Window frame,
                                   const Geometry *expected)
{
    unsigned int client_width =
        (unsigned)(expected->width - FRAME_LEFT - FRAME_RIGHT);
    unsigned int client_height =
        (unsigned)(expected->height - FRAME_TOP - FRAME_BOTTOM);

    XMoveResizeWindow(display, client, expected->x, expected->y,
                      client_width, client_height);
    XSync(display, False);
    if (wait_for_geometry(frame, expected) < 0) {
        report_geometry("client geometry request was not applied", frame,
                        expected);
        return -1;
    }
    return 0;
}

static int exercise_duplicate_clients(const Geometry *first_expected,
                                      const Geometry *second_expected,
                                      bool seed)
{
    Window first = create_class_client(30, 40, 150, 70,
                                       "win31x-duplicate-probe",
                                       "Win31xDuplicateProbe");
    Window first_frame = wait_for_client_frame(first);
    Window second;
    Window second_frame;
    int result = 0;

    if (first_frame == None) {
        fprintf(stderr,
                "persistence-probe: first duplicate client was not managed\n");
        XDestroyWindow(display, first);
        return -1;
    }
    second = create_class_client(60, 70, 160, 80,
                                 "win31x-duplicate-probe",
                                 "Win31xDuplicateProbe");
    second_frame = wait_for_client_frame(second);
    if (second_frame == None) {
        fprintf(stderr,
                "persistence-probe: second duplicate client was not managed\n");
        result = -1;
    } else if (seed) {
        if (request_client_geometry(first, first_frame, first_expected) < 0 ||
            request_client_geometry(second, second_frame,
                                    second_expected) < 0)
            result = -1;
    } else if (wait_for_geometry(first_frame, first_expected) < 0 ||
               wait_for_geometry(second_frame, second_expected) < 0) {
        report_geometry("first duplicate geometry was not restored",
                        first_frame, first_expected);
        report_geometry("second duplicate geometry was not restored",
                        second_frame, second_expected);
        result = -1;
    }
    XDestroyWindow(display, first);
    XDestroyWindow(display, second);
    XSync(display, False);
    return result;
}

static int exercise_late_identity(const Geometry *expected, bool seed,
                                  bool request_same_geometry)
{
    static const char application_id[] = "org.win31x.LateIdentityProbe";
    Window client = create_unidentified_client(45, 55, 160, 80);
    Window frame = wait_for_client_frame(client);
    Atom property = XInternAtom(display, "_GTK_APPLICATION_ID", False);
    Atom utf8 = XInternAtom(display, "UTF8_STRING", False);
    int result = 0;

    if (frame == None) {
        XDestroyWindow(display, client);
        return -1;
    }
    XChangeProperty(display, client, property, utf8, 8, PropModeReplace,
                    (const unsigned char *)application_id,
                    (int)strlen(application_id));
    XSync(display, False);
    if (seed) {
        if (request_client_geometry(client, frame, expected) < 0)
            result = -1;
    } else if (wait_for_geometry(frame, expected) < 0) {
        report_geometry("late application identity was not restored", frame,
                        expected);
        result = -1;
    }
    if (result == 0 && !seed && request_same_geometry &&
        request_client_geometry(client, frame, expected) < 0)
        result = -1;
    XDestroyWindow(display, client);
    XSync(display, False);
    return result;
}

static int exercise_late_identity_initial_maximize(
    const Geometry *saved_normal, const Geometry *initial_maximized,
    const Geometry *saved_monitor_maximized)
{
    static const char application_id[] = "org.win31x.LateIdentityProbe";
    Window client = create_initially_maximized_unidentified_client(
        initial_maximized->x + 80, initial_maximized->y + 90, 180, 90);
    Atom property = XInternAtom(display, "_GTK_APPLICATION_ID", False);
    Atom utf8 = XInternAtom(display, "UTF8_STRING", False);
    Window frame;
    int result = 0;

    frame = wait_for_client_frame(client);
    if (frame == None ||
        wait_for_geometry(frame, initial_maximized) < 0) {
        result = -1;
        goto done;
    }
    XChangeProperty(display, client, property, utf8, 8, PropModeReplace,
                    (const unsigned char *)application_id,
                    (int)strlen(application_id));
    XSync(display, False);
    if (wait_for_geometry(frame, saved_monitor_maximized) < 0 ||
        click_frame_maximize(frame) < 0 ||
        wait_for_geometry(frame, saved_normal) < 0) {
        report_geometry(
            "late identity did not retain maximized saved-monitor state",
            frame, saved_monitor_maximized);
        result = -1;
    }

done:
    XDestroyWindow(display, client);
    XSync(display, False);
    return result;
}

static int exercise_late_identity_maximized_snap(
    const Geometry *initial_maximized, const Geometry *saved_maximized,
    const Geometry *saved_snap, bool seed)
{
    static const char application_id[] =
        "org.win31x.LateIdentityMaxSnapProbe";
    const Geometry saved_normal = {
        saved_maximized->x + 180, saved_maximized->y + 130,
        300 + FRAME_LEFT + FRAME_RIGHT,
        150 + FRAME_TOP + FRAME_BOTTOM
    };
    Atom property = XInternAtom(display, "_GTK_APPLICATION_ID", False);
    Atom utf8 = XInternAtom(display, "UTF8_STRING", False);
    Window client = seed
                        ? create_unidentified_client(
                              saved_maximized->x + 40,
                              saved_maximized->y + 50, 180, 90)
                        : create_initially_maximized_unidentified_client(
                              initial_maximized->x + 40,
                              initial_maximized->y + 50, 180, 90);
    Window frame = wait_for_client_frame(client);
    int result = 0;

    if (frame == None) {
        XDestroyWindow(display, client);
        return -1;
    }
    if (!seed && wait_for_geometry(frame, initial_maximized) < 0) {
        report_geometry("late max-snap client did not initially maximize",
                        frame, initial_maximized);
        result = -1;
        goto done;
    }
    XChangeProperty(display, client, property, utf8, 8, PropModeReplace,
                    (const unsigned char *)application_id,
                    (int)strlen(application_id));
    XSync(display, False);
    if (seed) {
        if (request_client_geometry(client, frame, &saved_normal) < 0 ||
            drag_title_to(frame, saved_snap->x + 1,
                          saved_snap->y + saved_snap->height / 2) < 0 ||
            wait_for_geometry(frame, saved_snap) < 0) {
            report_geometry("late max-snap seed failed", frame, saved_snap);
            result = -1;
        }
    } else if (wait_for_geometry(frame, saved_maximized) < 0) {
        report_geometry("late identity used the wrong maximized monitor", frame,
                        saved_maximized);
        result = -1;
    } else if (click_frame_maximize(frame) < 0 ||
               wait_for_geometry(frame, saved_snap) < 0) {
        report_geometry("late initial maximize lost its saved snap", frame,
                        saved_snap);
        result = -1;
    } else if (click_frame_maximize(frame) < 0 ||
               wait_for_geometry(frame, saved_maximized) < 0) {
        report_geometry("late max-snap client did not maximize again", frame,
                        saved_maximized);
        result = -1;
    }

done:
    XDestroyWindow(display, client);
    XSync(display, False);
    return result;
}

static int exercise_maximized_snap(const Geometry *full,
                                   const Geometry *snapped, bool seed,
                                   bool exercise_restore)
{
    const Geometry normal = {
        full->x + 180, full->y + 130,
        300 + FRAME_LEFT + FRAME_RIGHT,
        150 + FRAME_TOP + FRAME_BOTTOM
    };
    Window client = create_class_client(
        full->x + 40, full->y + 50, 180, 90,
        "win31x-max-snap-probe", "Win31xMaxSnapProbe");
    Window frame = wait_for_client_frame(client);
    int result = 0;

    if (frame == None) {
        XDestroyWindow(display, client);
        return -1;
    }
    if (seed) {
        if (request_client_geometry(client, frame, &normal) < 0 ||
            drag_title_to(frame, snapped->x + 1,
                          snapped->y + snapped->height / 2) < 0 ||
            wait_for_geometry(frame, snapped) < 0 ||
            click_frame_maximize(frame) < 0 ||
            wait_for_geometry(frame, full) < 0) {
            report_geometry("maximize-over-snap seed failed", frame, full);
            result = -1;
        }
    } else if (wait_for_geometry(frame, full) < 0 ||
               (exercise_restore &&
                (click_frame_maximize(frame) < 0 ||
                 wait_for_geometry(frame, snapped) < 0 ||
                 click_frame_maximize(frame) < 0 ||
                 wait_for_geometry(frame, full) < 0))) {
        report_geometry("maximize restore lost its saved snap", frame, full);
        result = -1;
    }
    XDestroyWindow(display, client);
    XSync(display, False);
    return result;
}

static int exercise_known_identity_initial_maximize(
    const Geometry *saved_maximized, const Geometry *saved_snap)
{
    Window client = create_initially_maximized_class_client(
        primary_full_dual.x + 40, primary_full_dual.y + 50, 180, 90,
        "win31x-max-snap-probe", "Win31xMaxSnapProbe");
    Window frame = wait_for_client_frame(client);
    int result = 0;

    if (frame == None || wait_for_geometry(frame, saved_maximized) < 0) {
        if (frame != None)
            report_geometry(
                "known initial maximize used the wrong saved monitor", frame,
                saved_maximized);
        result = -1;
    } else if (click_frame_maximize(frame) < 0 ||
               wait_for_geometry(frame, saved_snap) < 0) {
        report_geometry("known initial maximize lost its saved pre-max snap",
                        frame, saved_snap);
        result = -1;
    } else if (click_frame_maximize(frame) < 0 ||
               wait_for_geometry(frame, saved_maximized) < 0) {
        report_geometry("known initial-max client did not maximize again",
                        frame, saved_maximized);
        result = -1;
    }
    XDestroyWindow(display, client);
    XSync(display, False);
    return result;
}

static int exercise_destroy_during_icon_drag(void)
{
    Window client = create_class_client(100, 90, 180, 90,
                                        "win31x-icon-drag-probe",
                                        "Win31xIconDragProbe");
    Window frame = wait_for_client_frame(client);
    Window icon;
    Geometry geometry;
    Atom change_state;
    XEvent event;

    if (frame == None)
        return -1;
    change_state = XInternAtom(display, "WM_CHANGE_STATE", False);
    memset(&event, 0, sizeof(event));
    event.xclient.type = ClientMessage;
    event.xclient.display = display;
    event.xclient.window = client;
    event.xclient.message_type = change_state;
    event.xclient.format = 32;
    event.xclient.data.l[0] = IconicState;
    if (!XSendEvent(display, root, False,
                    SubstructureRedirectMask | SubstructureNotifyMask,
                    &event)) {
        XDestroyWindow(display, client);
        return -1;
    }
    XSync(display, False);
    icon = wait_for_role("minimized-icon", true);
    if (icon == None || get_geometry(icon, &geometry) < 0) {
        fprintf(stderr,
                "persistence-probe: minimized icon drag fixture failed\n");
        XDestroyWindow(display, client);
        return -1;
    }
    if (!XTestFakeMotionEvent(display, DefaultScreen(display),
                              geometry.x + geometry.width / 2,
                              geometry.y + geometry.height / 2,
                              CurrentTime) ||
        !XTestFakeButtonEvent(display, Button1, True, CurrentTime) ||
        !XTestFakeMotionEvent(display, DefaultScreen(display),
                              geometry.x + geometry.width / 2 + 20,
                              geometry.y + geometry.height / 2,
                              CurrentTime)) {
        (void)XTestFakeButtonEvent(display, Button1, False, CurrentTime);
        XDestroyWindow(display, client);
        return -1;
    }
    XSync(display, False);
    XDestroyWindow(display, client);
    XSync(display, False);
    if (!role_remains_hidden("minimized-icon")) {
        fprintf(stderr,
                "persistence-probe: destroyed client's icon remained visible\n");
        (void)XTestFakeButtonEvent(display, Button1, False, CurrentTime);
        return -1;
    }
    if (!XTestFakeButtonEvent(display, Button1, False, CurrentTime))
        return -1;
    XSync(display, False);
    if (wait_for_manager() < 0 ||
        wait_for_role("applications-icon", true) == None) {
        fprintf(stderr,
                "persistence-probe: WM failed after icon owner exited mid-drag\n");
        return -1;
    }
    return 0;
}

static Window open_run_dialog(void)
{
    if (fake_key_chord(XK_Super_L, XK_r) < 0)
        return None;
    return wait_for_role("run-window", true);
}

static int verify_root_geometry(void)
{
    Geometry actual;

    if (get_geometry(root, &actual) < 0 ||
        !geometry_equal(&actual, &dual_bounds)) {
        fprintf(stderr,
                "persistence-probe: Xvfb must provide a 1600x700 root\n");
        return -1;
    }
    return 0;
}

static int seed_state(void)
{
    Window applications_icon;
    Window control_icon;
    Window launcher = None;
    Window panel = None;
    Window run = None;
    Window client;
    Window frame;

    applications_icon = wait_for_role("applications-icon", true);
    control_icon = wait_for_role("control-panel-icon", true);
    if (applications_icon == None || control_icon == None) {
        fprintf(stderr,
                "persistence-probe: desktop icons were not available\n");
        return -1;
    }
    if (drag_window_to(applications_icon, &applications_icon_dual) < 0 ||
        wait_for_geometry(applications_icon, &applications_icon_dual) < 0) {
        report_geometry("Applications icon drag was not committed",
                        applications_icon, &applications_icon_dual);
        return -1;
    }
    if (!role_remains_hidden("applications-window")) {
        fprintf(stderr,
                "persistence-probe: dragging Applications opened it\n");
        return -1;
    }
    if (drag_window_to(control_icon, &control_icon_dual) < 0 ||
        wait_for_geometry(control_icon, &control_icon_dual) < 0) {
        report_geometry("Control Panel icon drag was not committed",
                        control_icon, &control_icon_dual);
        return -1;
    }
    if (!role_remains_hidden("control-panel-window")) {
        fprintf(stderr,
                "persistence-probe: dragging Control Panel opened it\n");
        return -1;
    }

    if (click_window(applications_icon) < 0 ||
        (launcher = wait_for_role("applications-window", true)) == None ||
        drag_title_to(launcher, 801, 350) < 0 ||
        wait_for_geometry(launcher, &launcher_dual) < 0) {
        if (launcher != None)
            report_geometry("Applications did not snap left", launcher,
                            &launcher_dual);
        else
            fprintf(stderr,
                    "persistence-probe: Applications did not open\n");
        return -1;
    }
    if (click_window(control_icon) < 0 ||
        (panel = wait_for_role("control-panel-window", true)) == None ||
        drag_title_to(panel, 1598, 350) < 0 ||
        wait_for_geometry(panel, &panel_dual) < 0) {
        if (panel != None)
            report_geometry("Control Panel did not snap right", panel,
                            &panel_dual);
        else
            fprintf(stderr,
                    "persistence-probe: Control Panel did not open\n");
        return -1;
    }

    /* Select Colors, then Ocean Blue.  Coordinates are local to the saved
     * 400x600 snapped panel and deliberately avoid title-bar controls. */
    if (fake_click(panel_dual.x + 70, panel_dual.y + 150) < 0 ||
        fake_click(panel_dual.x + 220, panel_dual.y + 173) < 0 ||
        wait_for_root_color("#1f4e79", 30, 580) < 0) {
        fprintf(stderr,
                "persistence-probe: Ocean Blue was not applied\n");
        return -1;
    }

    run = open_run_dialog();
    if (run == None || drag_title_to_geometry(run, &run_dual) < 0 ||
        wait_for_geometry(run, &run_dual) < 0) {
        if (run != None)
            report_geometry("Run dialog move was not committed", run,
                            &run_dual);
        else
            fprintf(stderr,
                    "persistence-probe: Windows+R did not open Run\n");
        return -1;
    }

    if (exercise_destroy_during_icon_drag() < 0)
        return -1;

    client = create_test_client(90, 80, 180, 90);
    frame = wait_for_client_frame(client);
    if (frame == None) {
        fprintf(stderr,
                "persistence-probe: test client was not managed\n");
        XDestroyWindow(display, client);
        return -1;
    }
    if (request_client_geometry(client, frame, &client_dual) < 0) {
        XDestroyWindow(display, client);
        return -1;
    }
    XDestroyWindow(display, client);
    XSync(display, False);
    if (exercise_duplicate_clients(&duplicate_first_dual,
                                   &duplicate_second_dual, true) < 0)
        return -1;
    if (exercise_late_identity(&late_identity_dual, true, false) < 0)
        return -1;
    if (exercise_late_identity_maximized_snap(
            &primary_full_dual, &max_snap_full_dual,
            &max_snap_left_dual, true) < 0)
        return -1;
    return exercise_maximized_snap(&max_snap_full_dual,
                                   &max_snap_left_dual, true, false);
}

static int verify_icons(const Geometry *applications_expected,
                        const Geometry *control_expected,
                        const Geometry *bounds, Window *applications_out,
                        Window *control_out)
{
    Window applications = wait_for_role("applications-icon", true);
    Window control = wait_for_role("control-panel-icon", true);
    Geometry actual;

    if (applications == None || control == None)
        return -1;
    if (wait_for_geometry(applications, applications_expected) < 0) {
        report_geometry("Applications icon was not restored", applications,
                        applications_expected);
        return -1;
    }
    if (wait_for_geometry(control, control_expected) < 0) {
        report_geometry("Control Panel icon was not restored", control,
                        control_expected);
        return -1;
    }
    if (get_geometry(applications, &actual) < 0 ||
        !geometry_inside(&actual, bounds) ||
        get_geometry(control, &actual) < 0 ||
        !geometry_inside(&actual, bounds)) {
        fprintf(stderr,
                "persistence-probe: a restored desktop icon is off-monitor\n");
        return -1;
    }
    *applications_out = applications;
    *control_out = control;
    return 0;
}

static int verify_client(const Geometry *expected, const Geometry *bounds,
                         bool request_same_geometry)
{
    Window client = create_test_client(15, 20, 140, 70);
    Window frame = wait_for_client_frame(client);
    Geometry actual;
    int result = 0;

    if (frame == None || wait_for_geometry(frame, expected) < 0) {
        if (frame != None)
            report_geometry("client geometry was not restored over its request",
                            frame, expected);
        else
            fprintf(stderr,
                    "persistence-probe: restored test client was not managed\n");
        result = -1;
    } else if (get_geometry(frame, &actual) < 0 ||
               !geometry_inside(&actual, bounds)) {
        fprintf(stderr,
                "persistence-probe: restored client is off-monitor\n");
        result = -1;
    }
    if (result == 0 && request_same_geometry &&
        request_client_geometry(client, frame, expected) < 0)
        result = -1;
    XDestroyWindow(display, client);
    XSync(display, False);
    return result;
}

static int verify_dual_state(bool verify_section)
{
    Window applications_icon;
    Window control_icon;
    Window launcher = None;
    Window panel = None;
    Window run = None;

    if (wait_for_root_color("#1f4e79", 30, 580) < 0) {
        fprintf(stderr,
                "persistence-probe: Ocean Blue was not restored\n");
        return -1;
    }
    if (verify_icons(&applications_icon_dual, &control_icon_dual,
                     &dual_bounds, &applications_icon, &control_icon) < 0)
        return -1;
    if (click_window(applications_icon) < 0 ||
        (launcher = wait_for_role("applications-window", true)) == None ||
        wait_for_geometry(launcher, &launcher_dual) < 0) {
        if (launcher != None)
            report_geometry("saved Applications layout was not restored",
                            launcher, &launcher_dual);
        return -1;
    }
    if (click_window(control_icon) < 0 ||
        (panel = wait_for_role("control-panel-window", true)) == None ||
        wait_for_geometry(panel, &panel_dual) < 0) {
        if (panel != None)
            report_geometry("saved Control Panel layout was not restored",
                            panel, &panel_dual);
        return -1;
    }
    if (verify_section) {
        /* A click on Forest only changes the root if the restored section is
         * Colors.  Put Ocean Blue back so later phases see the seeded state. */
        if (fake_click(panel_dual.x + 220, panel_dual.y + 231) < 0 ||
            wait_for_root_color("#3f6b4f", 30, 580) < 0 ||
            fake_click(panel_dual.x + 220, panel_dual.y + 173) < 0 ||
            wait_for_root_color("#1f4e79", 30, 580) < 0) {
            fprintf(stderr,
                    "persistence-probe: Control Panel did not reopen on Colors\n");
            return -1;
        }
    }
    run = open_run_dialog();
    if (run == None || wait_for_geometry(run, &run_dual) < 0) {
        if (run != None)
            report_geometry("saved Run dialog position was not restored", run,
                            &run_dual);
        return -1;
    }
    if (verify_client(&client_dual, &dual_bounds, false) < 0)
        return -1;
    if (exercise_duplicate_clients(&duplicate_first_dual,
                                   &duplicate_second_dual, false) < 0)
        return -1;
    if (exercise_late_identity(&late_identity_dual, false, false) < 0)
        return -1;
    if (exercise_late_identity_initial_maximize(
            &late_identity_dual, &primary_full_dual,
            &max_snap_full_dual) < 0)
        return -1;
    if (exercise_late_identity_maximized_snap(
            &primary_full_dual, &max_snap_full_dual,
            &max_snap_left_dual, false) < 0)
        return -1;
    if (exercise_maximized_snap(&max_snap_full_dual,
                                &max_snap_left_dual, false, true) < 0)
        return -1;
    return exercise_known_identity_initial_maximize(
        &max_snap_full_dual, &max_snap_left_dual);
}

static int verify_single_state(void)
{
    Window applications_icon;
    Window control_icon;
    Window launcher = None;
    Window panel = None;
    Window run = None;

    if (wait_for_root_color("#1f4e79", 20, 450) < 0) {
        fprintf(stderr,
                "persistence-probe: saved color was lost on one monitor\n");
        return -1;
    }
    if (verify_icons(&applications_icon_single, &control_icon_single,
                     &single_bounds, &applications_icon, &control_icon) < 0)
        return -1;
    if (click_window(applications_icon) < 0 ||
        (launcher = wait_for_role("applications-window", true)) == None ||
        wait_for_geometry(launcher, &launcher_single) < 0) {
        if (launcher != None)
            report_geometry("Applications was not clamped on one monitor",
                            launcher, &launcher_single);
        return -1;
    }
    if (click_window(control_icon) < 0 ||
        (panel = wait_for_role("control-panel-window", true)) == None ||
        wait_for_geometry(panel, &panel_single) < 0) {
        if (panel != None)
            report_geometry("Control Panel was not clamped on one monitor",
                            panel, &panel_single);
        return -1;
    }
    run = open_run_dialog();
    if (run == None || wait_for_geometry(run, &run_single) < 0) {
        if (run != None)
            report_geometry("Run dialog was not clamped on one monitor", run,
                            &run_single);
        return -1;
    }
    if (verify_client(&client_single, &single_bounds, true) < 0)
        return -1;
    if (exercise_duplicate_clients(&duplicate_first_single,
                                   &duplicate_second_single, false) < 0)
        return -1;
    if (exercise_late_identity(&late_identity_single, false, true) < 0)
        return -1;
    return exercise_maximized_snap(&max_snap_full_single,
                                   &max_snap_left_single, false, false);
}

static int verify_extreme_state(void)
{
    Window applications = wait_for_role("applications-icon", true);
    Window control = wait_for_role("control-panel-icon", true);
    Geometry geometry;

    if (applications == None || control == None ||
        get_geometry(applications, &geometry) < 0 ||
        !geometry_inside(&geometry, &dual_bounds) ||
        get_geometry(control, &geometry) < 0 ||
        !geometry_inside(&geometry, &dual_bounds)) {
        fprintf(stderr,
                "persistence-probe: extreme saved coordinates were not safely clamped\n");
        return -1;
    }
    return 0;
}

int main(int argc, char **argv)
{
    enum {
        MODE_SEED,
        MODE_VERIFY_DUAL,
        MODE_VERIFY_SINGLE,
        MODE_VERIFY_DUAL_RETURN,
        MODE_VERIFY_EXTREME
    } mode;
    int result;

    if (argc != 2) {
        fprintf(stderr,
                "usage: persistence-probe "
                "--seed|--verify-dual|--verify-single|--verify-dual-return|"
                "--verify-extreme\n");
        return 2;
    }
    if (strcmp(argv[1], "--seed") == 0)
        mode = MODE_SEED;
    else if (strcmp(argv[1], "--verify-dual") == 0)
        mode = MODE_VERIFY_DUAL;
    else if (strcmp(argv[1], "--verify-single") == 0)
        mode = MODE_VERIFY_SINGLE;
    else if (strcmp(argv[1], "--verify-dual-return") == 0)
        mode = MODE_VERIFY_DUAL_RETURN;
    else if (strcmp(argv[1], "--verify-extreme") == 0)
        mode = MODE_VERIFY_EXTREME;
    else {
        fprintf(stderr, "persistence-probe: unknown mode: %s\n", argv[1]);
        return 2;
    }

    display = XOpenDisplay(NULL);
    if (display == NULL) {
        fprintf(stderr,
                "persistence-probe: cannot open DISPLAY\n");
        return 2;
    }
    root = RootWindow(display, DefaultScreen(display));
    role_atom = XInternAtom(display, "_WIN31X_ROLE", False);
    supporting_atom = XInternAtom(display, "_NET_SUPPORTING_WM_CHECK", False);
    if (!xtest_ready() || wait_for_manager() < 0 ||
        verify_root_geometry() < 0) {
        fprintf(stderr,
                "persistence-probe: WM or XTEST did not become ready\n");
        XCloseDisplay(display);
        return 1;
    }

    if (mode == MODE_SEED)
        result = seed_state();
    else if (mode == MODE_VERIFY_DUAL)
        result = verify_dual_state(true);
    else if (mode == MODE_VERIFY_SINGLE)
        result = verify_single_state();
    else if (mode == MODE_VERIFY_EXTREME)
        result = verify_extreme_state();
    else
        result = verify_dual_state(false);
    XCloseDisplay(display);
    return result == 0 ? 0 : 1;
}
