#define _POSIX_C_SOURCE 200809L

#include <X11/Xatom.h>
#include <X11/Xlib.h>
#include <X11/Xutil.h>
#include <X11/extensions/XTest.h>
#include <X11/keysym.h>

#include <errno.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

static Display *display;
static Window root;
static Atom wm_state_atom;
static Atom net_wm_state_atom;
static Atom hidden_atom;
static Atom maximized_horz_atom;
static Atom maximized_vert_atom;
static Atom role_atom;
static Atom client_atom;
static Atom net_wm_name_atom;
static int held_button_one;

static int wait_until_unmanaged(Window window);
static void report_frame_geometry(const char *message, Window frame);

enum {
    TEST_NET_WM_STATE_REMOVE = 0,
    TEST_FRAME_RIGHT = 3,
    TEST_TITLE_BUTTON = 17,
    TEST_TITLE_BUTTON_GAP = 3,
    TEST_TITLE_BUTTON_RIGHT_INSET = 2,
    TEST_INTERNAL_CLOSE_INSET = 6,
    TEST_TITLE_BUTTON_Y = 4,
    TEST_SESSION_CONFIRM_BUTTON_WIDTH = 78,
    TEST_SESSION_CONFIRM_BUTTON_HEIGHT = 26,
    TEST_SESSION_CONFIRM_BUTTON_GAP = 8,
    TEST_SESSION_CONFIRM_BUTTON_MARGIN = 13,
    TEST_DRAG_OUTLINE_THICKNESS = 3,
    TEST_DRAG_OUTLINE_COUNT = 4,
    TEST_DESKTOP_ICON_WIDTH = 112,
    TEST_DESKTOP_ICON_HEIGHT = 80
};

typedef struct {
    int x;
    int y;
    int width;
    int height;
} FrameGeometry;

typedef struct {
    Window window;
    FrameGeometry geometry;
    int override_redirect;
    unsigned int stack_index;
} DragOutlineWindow;

static Window root_window_property(Atom property)
{
    Atom type;
    int format;
    unsigned long count;
    unsigned long after;
    unsigned char *data = NULL;
    Window result = None;

    if (XGetWindowProperty(display, root, property, 0, 1, False, XA_WINDOW,
                           &type, &format, &count, &after, &data) == Success &&
        type == XA_WINDOW && format == 32 && count == 1 && data != NULL)
        result = *(Window *)data;
    if (data != NULL)
        XFree(data);
    return result;
}

static void wait_a_bit(void)
{
    struct timespec delay = {0, 20000000};
    nanosleep(&delay, NULL);
}

static long window_state(Window window)
{
    Atom type;
    int format;
    unsigned long count;
    unsigned long after;
    unsigned char *data = NULL;
    long state = WithdrawnState;

    if (XGetWindowProperty(display, window, wm_state_atom, 0, 2, False,
                           wm_state_atom, &type, &format, &count, &after,
                           &data) == Success &&
        type == wm_state_atom && format == 32 && count > 0 && data != NULL)
        state = ((long *)data)[0];
    if (data != NULL)
        XFree(data);
    return state;
}

static int window_atom_property_contains(Window window, Atom property,
                                         Atom expected)
{
    Atom type;
    int format;
    unsigned long count;
    unsigned long after;
    unsigned char *data = NULL;
    unsigned long index;
    int found = 0;

    if (XGetWindowProperty(display, window, property, 0, 128, False, XA_ATOM,
                           &type, &format, &count, &after, &data) == Success &&
        type == XA_ATOM && format == 32 && data != NULL) {
        Atom *atoms = (Atom *)data;

        for (index = 0; index < count; ++index) {
            if (atoms[index] == expected) {
                found = 1;
                break;
            }
        }
    }
    if (data != NULL)
        XFree(data);
    return found;
}

static int window_cardinal_property(Window window, Atom property,
                                    unsigned long *value)
{
    Atom type;
    int format;
    unsigned long count;
    unsigned long after;
    unsigned char *data = NULL;
    int found = 0;

    if (value != NULL &&
        XGetWindowProperty(display, window, property, 0, 1, False,
                           XA_CARDINAL, &type, &format, &count, &after,
                           &data) == Success &&
        type == XA_CARDINAL && format == 32 && count == 1 && data != NULL) {
        *value = *(unsigned long *)data;
        found = 1;
    }
    if (data != NULL)
        XFree(data);
    return found;
}

static int wait_for_cardinal_value(Window window, Atom property,
                                   unsigned long expected)
{
    int attempt;

    for (attempt = 0; attempt < 150; ++attempt) {
        unsigned long value = 0;

        XSync(display, False);
        if (window_cardinal_property(window, property, &value) &&
            value == expected)
            return 0;
        wait_a_bit();
    }
    return -1;
}

static int wait_for_cardinal_change(Window window, Atom property,
                                    unsigned long previous,
                                    unsigned long *updated)
{
    int attempt;

    for (attempt = 0; attempt < 150; ++attempt) {
        unsigned long value = 0;

        XSync(display, False);
        if (window_cardinal_property(window, property, &value) &&
            value > previous) {
            if (updated != NULL)
                *updated = value;
            return 0;
        }
        wait_a_bit();
    }
    return -1;
}

static int wait_for_maximized_state(Window window, int expected)
{
    int attempt;

    for (attempt = 0; attempt < 150; ++attempt) {
        int has_horizontal;
        int has_vertical;

        XSync(display, False);
        has_horizontal = window_atom_property_contains(
            window, net_wm_state_atom, maximized_horz_atom);
        has_vertical = window_atom_property_contains(
            window, net_wm_state_atom, maximized_vert_atom);
        if (has_horizontal == expected && has_vertical == expected)
            return 0;
        wait_a_bit();
    }
    return -1;
}

static int wait_for_state(Window window, long expected)
{
    int attempt;

    for (attempt = 0; attempt < 150; ++attempt) {
        XSync(display, False);
        if (window_state(window) == expected)
            return 0;
        wait_a_bit();
    }
    return -1;
}

static int role_matches(Window window, const char *expected)
{
    Atom type;
    int format;
    unsigned long count;
    unsigned long after;
    unsigned char *data = NULL;
    int matches = 0;

    if (XGetWindowProperty(display, window, role_atom, 0, 128, False,
                           AnyPropertyType, &type, &format, &count, &after,
                           &data) == Success &&
        format == 8 && data != NULL && strlen(expected) == count &&
        memcmp(data, expected, count) == 0)
        matches = 1;
    if (data != NULL)
        XFree(data);
    return matches;
}

static Window find_role(const char *role, Window expected_client, int must_be_viewable)
{
    Window root_return;
    Window parent_return;
    Window *children = NULL;
    unsigned int count = 0;
    unsigned int index;
    Window result = None;

    if (!XQueryTree(display, root, &root_return, &parent_return, &children, &count))
        return None;
    for (index = 0; index < count; ++index) {
        XWindowAttributes attributes;

        if (!role_matches(children[index], role))
            continue;
        if (must_be_viewable &&
            (!XGetWindowAttributes(display, children[index], &attributes) ||
             attributes.map_state != IsViewable))
            continue;
        if (expected_client != None) {
            Atom type;
            int format;
            unsigned long items;
            unsigned long after;
            unsigned char *data = NULL;
            Window linked = None;

            if (XGetWindowProperty(display, children[index], client_atom, 0, 1,
                                   False, XA_WINDOW, &type, &format, &items,
                                   &after, &data) == Success &&
                type == XA_WINDOW && format == 32 && items == 1 && data != NULL)
                linked = *(Window *)data;
            if (data != NULL)
                XFree(data);
            if (linked != expected_client)
                continue;
        }
        result = children[index];
        break;
    }
    if (children != NULL)
        XFree(children);
    return result;
}

static Window wait_for_role(const char *role, Window expected_client,
                            int must_be_viewable)
{
    int attempt;

    for (attempt = 0; attempt < 150; ++attempt) {
        Window found = find_role(role, expected_client, must_be_viewable);
        if (found != None)
            return found;
        wait_a_bit();
    }
    return None;
}

static unsigned int count_windows_with_role(const char *role)
{
    Window root_return;
    Window parent_return;
    Window *children = NULL;
    unsigned int count = 0;
    unsigned int index;
    unsigned int matches = 0;

    if (!XQueryTree(display, root, &root_return, &parent_return, &children,
                    &count))
        return 0;
    for (index = 0; index < count; ++index) {
        if (role_matches(children[index], role))
            ++matches;
    }
    if (children != NULL)
        XFree(children);
    return matches;
}

static void send_button_one_at(Window window, int x, int y)
{
    XEvent event;

    memset(&event, 0, sizeof(event));
    event.xbutton.type = ButtonPress;
    event.xbutton.display = display;
    event.xbutton.window = window;
    event.xbutton.root = root;
    event.xbutton.button = Button1;
    event.xbutton.x = x;
    event.xbutton.y = y;
    event.xbutton.same_screen = True;
    XSendEvent(display, window, False, ButtonPressMask, &event);
    XFlush(display);
}

static void send_button_one(Window window)
{
    send_button_one_at(window, 1, 1);
}

static int wait_for_file(const char *path)
{
    int attempt;

    if (path == NULL || path[0] == '\0')
        return -1;
    for (attempt = 0; attempt < 150; ++attempt) {
        if (access(path, F_OK) == 0)
            return 0;
        wait_a_bit();
    }
    return -1;
}

static int file_remains_absent(const char *path)
{
    int attempt;

    if (path == NULL || path[0] == '\0')
        return -1;
    for (attempt = 0; attempt < 20; ++attempt) {
        if (access(path, F_OK) == 0)
            return -1;
        wait_a_bit();
    }
    return 0;
}

static int window_string_property_matches(Window window, Atom property,
                                          const char *expected)
{
    Atom type;
    int format;
    unsigned long count;
    unsigned long after;
    unsigned char *data = NULL;
    int matches = 0;

    if (expected != NULL &&
        XGetWindowProperty(display, window, property, 0, 128, False,
                           AnyPropertyType, &type, &format, &count, &after,
                           &data) == Success &&
        format == 8 && data != NULL && strlen(expected) == count &&
        memcmp(data, expected, count) == 0)
        matches = 1;
    if (data != NULL)
        XFree(data);
    return matches;
}

static int wait_for_window_name(Window window, const char *expected)
{
    int attempt;

    for (attempt = 0; attempt < 150; ++attempt) {
        XSync(display, False);
        if (window_string_property_matches(window, net_wm_name_atom,
                                           expected))
            return 0;
        wait_a_bit();
    }
    return -1;
}

static int file_contents_equal(const char *path, const char *expected)
{
    FILE *file;
    char buffer[64];
    size_t expected_length;
    size_t length;
    int extra;

    if (path == NULL || path[0] == '\0' || expected == NULL)
        return 0;
    expected_length = strlen(expected);
    if (expected_length >= sizeof(buffer))
        return 0;
    file = fopen(path, "rb");
    if (file == NULL)
        return 0;
    length = fread(buffer, 1, sizeof(buffer), file);
    extra = fgetc(file);
    if (ferror(file)) {
        fclose(file);
        return 0;
    }
    fclose(file);
    return length == expected_length && extra == EOF &&
           memcmp(buffer, expected, expected_length) == 0;
}

static int wait_for_file_contents(const char *path, const char *expected)
{
    int attempt;

    for (attempt = 0; attempt < 150; ++attempt) {
        if (file_contents_equal(path, expected))
            return 0;
        wait_a_bit();
    }
    return -1;
}

static int internal_window_is_hidden(Window window)
{
    int attempt;

    for (attempt = 0; attempt < 100; ++attempt) {
        XWindowAttributes attributes;
        if (XGetWindowAttributes(display, window, &attributes) &&
            attributes.map_state == IsUnmapped)
            return 1;
        wait_a_bit();
    }
    return 0;
}

static int internal_window_remains_viewable(Window window)
{
    int attempt;

    for (attempt = 0; attempt < 20; ++attempt) {
        XWindowAttributes attributes;

        XSync(display, False);
        if (!XGetWindowAttributes(display, window, &attributes) ||
            attributes.map_state != IsViewable)
            return 0;
        wait_a_bit();
    }
    return 1;
}

static int internal_window_remains_unmapped(Window window)
{
    int attempt;

    for (attempt = 0; attempt < 30; ++attempt) {
        XWindowAttributes attributes;

        XSync(display, False);
        if (!XGetWindowAttributes(display, window, &attributes) ||
            attributes.map_state != IsUnmapped)
            return 0;
        wait_a_bit();
    }
    return 1;
}

static int role_remains_unmapped(const char *role)
{
    int attempt;

    for (attempt = 0; attempt < 30; ++attempt) {
        XSync(display, False);
        if (find_role(role, None, 1) != None)
            return 0;
        wait_a_bit();
    }
    return 1;
}

static int click_internal_close_button(Window window)
{
    XWindowAttributes attributes;

    if (!XGetWindowAttributes(display, window, &attributes) ||
        attributes.width < TEST_TITLE_BUTTON + 16)
        return -1;
    send_button_one_at(window, attributes.width - 14,
                       TEST_TITLE_BUTTON_Y + TEST_TITLE_BUTTON / 2);
    return 0;
}

static int wait_for_wm_check(Atom supporting)
{
    int attempt;

    for (attempt = 0; attempt < 150; ++attempt) {
        Atom type;
        int format;
        unsigned long count;
        unsigned long after;
        unsigned char *data = NULL;
        int ready = XGetWindowProperty(display, root, supporting, 0, 1, False,
                                       XA_WINDOW, &type, &format, &count, &after,
                                       &data) == Success &&
                    type == XA_WINDOW && format == 32 && count == 1;

        if (data != NULL)
            XFree(data);
        if (ready)
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
    unsigned int count = 0;

    if (!XQueryTree(display, client, &root_return, &parent, &children, &count))
        parent = None;
    if (children != NULL)
        XFree(children);
    return parent;
}

static Window child_with_role(Window parent, const char *role)
{
    Window root_return;
    Window parent_return;
    Window *children = NULL;
    unsigned int count = 0;
    unsigned int index;
    Window result = None;

    if (!XQueryTree(display, parent, &root_return, &parent_return,
                    &children, &count))
        return None;
    for (index = 0; index < count; ++index) {
        if (role_matches(children[index], role)) {
            result = children[index];
            break;
        }
    }
    if (children != NULL)
        XFree(children);
    return result;
}

static int window_is_above(Window upper, Window lower)
{
    Window root_return;
    Window parent_return;
    Window *children = NULL;
    unsigned int count = 0;
    unsigned int index;
    int upper_index = -1;
    int lower_index = -1;

    if (!XQueryTree(display, root, &root_return, &parent_return, &children, &count))
        return 0;
    for (index = 0; index < count; ++index) {
        if (children[index] == upper)
            upper_index = (int)index;
        if (children[index] == lower)
            lower_index = (int)index;
    }
    if (children != NULL)
        XFree(children);
    return upper_index >= 0 && lower_index >= 0 && upper_index > lower_index;
}

static int wait_for_above(Window upper, Window lower)
{
    int attempt;

    for (attempt = 0; attempt < 150; ++attempt) {
        XSync(display, False);
        if (window_is_above(upper, lower))
            return 0;
        wait_a_bit();
    }
    return -1;
}

static int client_is_active_and_above(Window client, Window frame,
                                      Window other_frame)
{
    Window focused = None;
    int revert_to;
    Atom active_window = XInternAtom(display, "_NET_ACTIVE_WINDOW", False);

    XGetInputFocus(display, &focused, &revert_to);
    return focused == client && root_window_property(active_window) == client &&
           window_is_above(frame, other_frame);
}

static int wait_for_active_and_above(Window client, Window frame,
                                     Window other_frame)
{
    int attempt;

    for (attempt = 0; attempt < 150; ++attempt) {
        XSync(display, False);
        if (client_is_active_and_above(client, frame, other_frame))
            return 0;
        wait_a_bit();
    }
    return -1;
}

static int fake_button_at(unsigned int button, int x, int y)
{
    int event_base;
    int error_base;
    int major_version;
    int minor_version;

    if (!XTestQueryExtension(display, &event_base, &error_base,
                             &major_version, &minor_version)) {
        fprintf(stderr, "wm-probe: XTEST extension is unavailable\n");
        return -1;
    }
    if (!XTestFakeMotionEvent(display, DefaultScreen(display), x, y,
                              CurrentTime)) {
        fprintf(stderr, "wm-probe: could not inject pointer button %u\n",
                button);
        return -1;
    }
    XSync(display, False);
    wait_a_bit();
    if (!XTestFakeButtonEvent(display, button, True, CurrentTime)) {
        fprintf(stderr, "wm-probe: could not press pointer button %u\n",
                button);
        return -1;
    }
    XSync(display, False);
    wait_a_bit();
    if (!XTestFakeButtonEvent(display, button, False, CurrentTime)) {
        fprintf(stderr, "wm-probe: could not release pointer button %u\n",
                button);
        return -1;
    }
    XFlush(display);
    return 0;
}

static int fake_click_at(int x, int y)
{
    return fake_button_at(Button1, x, y);
}

static int window_center_on_root(Window window, int *root_x, int *root_y)
{
    XWindowAttributes attributes;
    Window child = None;

    if (root_x == NULL || root_y == NULL ||
        !XGetWindowAttributes(display, window, &attributes) ||
        !XTranslateCoordinates(display, window, root,
                               attributes.width / 2, attributes.height / 2,
                               root_x, root_y, &child))
        return -1;
    return 0;
}

static int find_bare_root_near_bottom_right(int *root_x, int *root_y)
{
    int screen = DefaultScreen(display);
    int width = DisplayWidth(display, screen);
    int height = DisplayHeight(display, screen);
    int minimum_x = width > 208 ? width - 208 : 0;
    int minimum_y = height > 128 ? height - 128 : 0;
    int y;

    if (root_x == NULL || root_y == NULL || width < 1 || height < 1)
        return -1;
    for (y = height - 1; y >= minimum_y; y -= 8) {
        int x;

        for (x = width - 1; x >= minimum_x; x -= 8) {
            Window root_return = None;
            Window child_return = None;
            int query_root_x;
            int query_root_y;
            int window_x;
            int window_y;
            unsigned int mask;

            if (!XTestFakeMotionEvent(display, screen, x, y, CurrentTime))
                return -1;
            XSync(display, False);
            if (XQueryPointer(display, root, &root_return, &child_return,
                              &query_root_x, &query_root_y, &window_x,
                              &window_y, &mask) && child_return == None) {
                *root_x = x;
                *root_y = y;
                return 0;
            }
        }
    }
    fprintf(stderr,
            "wm-probe: no bare desktop point was available near the bottom-right corner\n");
    return -1;
}

static int fake_key_chord(KeySym modifier, KeySym key)
{
    KeyCode modifier_code = XKeysymToKeycode(display, modifier);
    KeyCode key_code = XKeysymToKeycode(display, key);

    if (modifier_code == 0 || key_code == 0 ||
        !XTestFakeKeyEvent(display, modifier_code, True, CurrentTime) ||
        !XTestFakeKeyEvent(display, key_code, True, CurrentTime) ||
        !XTestFakeKeyEvent(display, key_code, False, CurrentTime) ||
        !XTestFakeKeyEvent(display, modifier_code, False, CurrentTime)) {
        fprintf(stderr, "wm-probe: could not inject keyboard chord\n");
        return -1;
    }
    XFlush(display);
    return 0;
}

static int fake_three_key_chord(KeySym first_modifier,
                                KeySym second_modifier, KeySym key)
{
    KeyCode first_code = XKeysymToKeycode(display, first_modifier);
    KeyCode second_code = XKeysymToKeycode(display, second_modifier);
    KeyCode key_code = XKeysymToKeycode(display, key);

    if (first_code == 0 || second_code == 0 || key_code == 0 ||
        !XTestFakeKeyEvent(display, first_code, True, CurrentTime) ||
        !XTestFakeKeyEvent(display, second_code, True, CurrentTime) ||
        !XTestFakeKeyEvent(display, key_code, True, CurrentTime) ||
        !XTestFakeKeyEvent(display, key_code, False, CurrentTime) ||
        !XTestFakeKeyEvent(display, second_code, False, CurrentTime) ||
        !XTestFakeKeyEvent(display, first_code, False, CurrentTime)) {
        fprintf(stderr, "wm-probe: could not inject three-key chord\n");
        return -1;
    }
    XSync(display, False);
    return 0;
}

static int fake_key(KeySym key)
{
    KeyCode code = XKeysymToKeycode(display, key);

    if (code == 0 ||
        !XTestFakeKeyEvent(display, code, True, CurrentTime)) {
        fprintf(stderr, "wm-probe: could not inject keyboard key\n");
        return -1;
    }
    XSync(display, False);
    wait_a_bit();
    if (!XTestFakeKeyEvent(display, code, False, CurrentTime)) {
        fprintf(stderr, "wm-probe: could not release keyboard key\n");
        return -1;
    }
    XSync(display, False);
    wait_a_bit();
    return 0;
}

static int fake_key_state(KeySym key, Bool pressed)
{
    KeyCode code = XKeysymToKeycode(display, key);

    if (code == 0 ||
        !XTestFakeKeyEvent(display, code, pressed, CurrentTime)) {
        fprintf(stderr, "wm-probe: could not inject keyboard key state\n");
        return -1;
    }
    XSync(display, False);
    wait_a_bit();
    return 0;
}

static int fake_legacy_auto_repeat_pair(KeySym key)
{
    KeyCode code = XKeysymToKeycode(display, key);

    /* Without detectable auto-repeat, Xorg reports each repeat as an
     * adjacent release/press pair with the same server timestamp. */
    if (code == 0 ||
        !XTestFakeKeyEvent(display, code, False, CurrentTime) ||
        !XTestFakeKeyEvent(display, code, True, CurrentTime)) {
        fprintf(stderr,
                "wm-probe: could not inject legacy keyboard auto-repeat\n");
        return -1;
    }
    XSync(display, False);
    wait_a_bit();
    return 0;
}

static int fake_toggle_key(KeySym key)
{
    KeyCode code = XKeysymToKeycode(display, key);

    if (code == 0)
        return 1;
    if (!XTestFakeKeyEvent(display, code, True, CurrentTime) ||
        !XTestFakeKeyEvent(display, code, False, CurrentTime))
        return -1;
    XFlush(display);
    return 0;
}

static unsigned int modifier_mask_for_keysym(KeySym symbol)
{
    KeyCode code = XKeysymToKeycode(display, symbol);
    XModifierKeymap *mapping;
    unsigned int mask = 0U;
    int modifier;
    int index;

    if (code == 0)
        return 0U;
    mapping = XGetModifierMapping(display);
    if (mapping == NULL)
        return 0U;
    for (modifier = 0; modifier < 8; ++modifier) {
        for (index = 0; index < mapping->max_keypermod; ++index) {
            if (mapping->modifiermap[
                    modifier * mapping->max_keypermod + index] == code) {
                mask = 1U << modifier;
                break;
            }
        }
        if (mask != 0U)
            break;
    }
    XFreeModifiermap(mapping);
    return mask;
}

static int begin_held_drag_at(int x, int y)
{
    int event_base;
    int error_base;
    int major_version;
    int minor_version;

    if (held_button_one) {
        fprintf(stderr, "wm-probe: attempted to nest pointer drags\n");
        return -1;
    }
    if (!XTestQueryExtension(display, &event_base, &error_base,
                             &major_version, &minor_version)) {
        fprintf(stderr, "wm-probe: XTEST extension is unavailable\n");
        return -1;
    }
    if (!XTestFakeMotionEvent(display, DefaultScreen(display), x, y,
                              CurrentTime)) {
        fprintf(stderr, "wm-probe: could not position a held drag pointer\n");
        return -1;
    }
    XSync(display, False);
    wait_a_bit();
    if (!XTestFakeButtonEvent(display, Button1, True, CurrentTime)) {
        fprintf(stderr, "wm-probe: could not begin a held pointer drag\n");
        return -1;
    }
    held_button_one = 1;
    XSync(display, False);
    wait_a_bit();
    return 0;
}

static int move_held_drag_to(int x, int y)
{
    if (!held_button_one ||
        !XTestFakeMotionEvent(display, DefaultScreen(display), x, y,
                              CurrentTime)) {
        fprintf(stderr, "wm-probe: could not move a held pointer drag\n");
        return -1;
    }
    XSync(display, False);
    return 0;
}

static int release_held_drag(void)
{
    if (!held_button_one) {
        fprintf(stderr, "wm-probe: no held pointer drag was active\n");
        return -1;
    }
    if (!XTestFakeButtonEvent(display, Button1, False, CurrentTime)) {
        fprintf(stderr, "wm-probe: could not release a held pointer drag\n");
        return -1;
    }
    held_button_one = 0;
    XSync(display, False);
    return 0;
}

static void abort_held_drag(void)
{
    if (!held_button_one)
        return;
    if (XTestFakeButtonEvent(display, Button1, False, CurrentTime))
        held_button_one = 0;
    XSync(display, False);
}

static int wait_for_pointer_available(void)
{
    int attempt;

    for (attempt = 0; attempt < 150; ++attempt) {
        int status = XGrabPointer(display, root, False, NoEventMask,
                                  GrabModeAsync, GrabModeAsync, None, None,
                                  CurrentTime);

        if (status == GrabSuccess) {
            XUngrabPointer(display, CurrentTime);
            XSync(display, False);
            return 0;
        }
        wait_a_bit();
    }
    return -1;
}

static int wait_for_keyboard_available(void)
{
    int attempt;

    for (attempt = 0; attempt < 150; ++attempt) {
        int status = XGrabKeyboard(display, root, False, GrabModeAsync,
                                   GrabModeAsync, CurrentTime);

        if (status == GrabSuccess) {
            XUngrabKeyboard(display, CurrentTime);
            XSync(display, False);
            return 0;
        }
        wait_a_bit();
    }
    return -1;
}

static int fake_drag_at(int start_x, int start_y, int end_x, int end_y)
{
    int event_base;
    int error_base;
    int major_version;
    int minor_version;
    int intermediate_x = start_x;
    int intermediate_y = start_y;
    int final_x = end_x;
    int final_y = end_y;

    if (!XTestQueryExtension(display, &event_base, &error_base,
                             &major_version, &minor_version)) {
        fprintf(stderr, "wm-probe: XTEST extension is unavailable\n");
        return -1;
    }
    if (!XTestFakeMotionEvent(display, DefaultScreen(display), start_x,
                              start_y, CurrentTime)) {
        fprintf(stderr, "wm-probe: could not position a drag pointer\n");
        return -1;
    }
    XSync(display, False);
    wait_a_bit();
    if (!XTestFakeButtonEvent(display, Button1, True, CurrentTime)) {
        fprintf(stderr, "wm-probe: could not begin a pointer drag\n");
        return -1;
    }
    XSync(display, False);
    wait_a_bit();
    if (end_y > start_y) {
        ++intermediate_y;
        ++final_y;
    } else if (end_y < start_y) {
        --intermediate_y;
        --final_y;
    } else if (end_x > start_x) {
        ++intermediate_x;
        ++final_x;
    } else if (end_x < start_x) {
        --intermediate_x;
        --final_x;
    }
    if (!XTestFakeMotionEvent(display, DefaultScreen(display), intermediate_x,
                              intermediate_y, CurrentTime)) {
        (void)XTestFakeButtonEvent(display, Button1, False, CurrentTime);
        XFlush(display);
        fprintf(stderr, "wm-probe: could not begin pointer motion\n");
        return -1;
    }
    XSync(display, False);
    wait_a_bit();
    if (!XTestFakeMotionEvent(display, DefaultScreen(display), final_x, final_y,
                              CurrentTime)) {
        (void)XTestFakeButtonEvent(display, Button1, False, CurrentTime);
        XFlush(display);
        fprintf(stderr, "wm-probe: could not move a dragged pointer\n");
        return -1;
    }
    XSync(display, False);
    wait_a_bit();
    if (!XTestFakeButtonEvent(display, Button1, False, CurrentTime)) {
        fprintf(stderr, "wm-probe: could not release a pointer drag\n");
        return -1;
    }
    XFlush(display);
    return 0;
}

static int get_frame_geometry(Window frame, FrameGeometry *geometry)
{
    XWindowAttributes attributes;

    if (geometry == NULL || !XGetWindowAttributes(display, frame, &attributes))
        return -1;
    geometry->x = attributes.x;
    geometry->y = attributes.y;
    geometry->width = attributes.width;
    geometry->height = attributes.height;
    return 0;
}

static int frame_geometry_equals(const FrameGeometry *left,
                                 const FrameGeometry *right)
{
    return left->x == right->x && left->y == right->y &&
           left->width == right->width && left->height == right->height;
}

static int frame_geometry_inside(const FrameGeometry *geometry,
                                 const FrameGeometry *bounds)
{
    long long right;
    long long bottom;
    long long bounds_right;
    long long bounds_bottom;

    if (geometry == NULL || bounds == NULL || geometry->width < 1 ||
        geometry->height < 1 || bounds->width < 1 || bounds->height < 1)
        return 0;
    right = (long long)geometry->x + geometry->width;
    bottom = (long long)geometry->y + geometry->height;
    bounds_right = (long long)bounds->x + bounds->width;
    bounds_bottom = (long long)bounds->y + bounds->height;
    return geometry->x >= bounds->x && geometry->y >= bounds->y &&
           right <= bounds_right && bottom <= bounds_bottom;
}

static int wait_for_frame_inside(Window window,
                                 const FrameGeometry *bounds)
{
    int attempt;

    for (attempt = 0; attempt < 150; ++attempt) {
        FrameGeometry actual;

        XSync(display, False);
        if (get_frame_geometry(window, &actual) == 0 &&
            frame_geometry_inside(&actual, bounds))
            return 0;
        wait_a_bit();
    }
    return -1;
}

static int move_pointer_to(int x, int y)
{
    if (!XTestFakeMotionEvent(display, DefaultScreen(display), x, y,
                              CurrentTime))
        return -1;
    XSync(display, False);
    wait_a_bit();
    return 0;
}

static int wait_for_pointer_root_child(Window expected)
{
    Window root_return = None;
    Window child_return = None;
    int root_x;
    int root_y;
    int window_x;
    int window_y;
    unsigned int mask;
    int attempt;

    for (attempt = 0; attempt < 150; ++attempt) {
        XSync(display, False);
        if (XQueryPointer(display, root, &root_return, &child_return,
                          &root_x, &root_y, &window_x, &window_y, &mask) &&
            child_return == expected)
            return 0;
        wait_a_bit();
    }
    fprintf(stderr,
            "wm-probe: pointer target did not become frame "
            "(expected 0x%lx, actual 0x%lx)\n",
            (unsigned long)expected, (unsigned long)child_return);
    return -1;
}

static int fake_click_on_root_child(Window expected, int x, int y)
{
    if (wait_for_pointer_available() < 0) {
        fprintf(stderr,
                "wm-probe: pointer remained grabbed before frame click\n");
        return -1;
    }
    if (move_pointer_to(x, y) < 0 ||
        wait_for_pointer_root_child(expected) < 0)
        return -1;
    return fake_click_at(x, y);
}

static int wait_for_frame_geometry(Window frame,
                                   const FrameGeometry *expected)
{
    int attempt;

    for (attempt = 0; attempt < 150; ++attempt) {
        FrameGeometry actual;

        XSync(display, False);
        if (get_frame_geometry(frame, &actual) == 0 &&
            frame_geometry_equals(&actual, expected))
            return 0;
        wait_a_bit();
    }
    return -1;
}

static int frame_geometry_remains(Window frame,
                                  const FrameGeometry *expected)
{
    int attempt;

    for (attempt = 0; attempt < 30; ++attempt) {
        FrameGeometry actual;

        XSync(display, False);
        if (get_frame_geometry(frame, &actual) < 0 ||
            !frame_geometry_equals(&actual, expected))
            return -1;
        wait_a_bit();
    }
    return 0;
}

static int collect_drag_outline_windows(
    DragOutlineWindow outlines[TEST_DRAG_OUTLINE_COUNT],
    size_t *total_count, size_t *viewable_count, int *topmost)
{
    Window root_return;
    Window parent_return;
    Window *children = NULL;
    unsigned int child_count = 0U;
    unsigned int index;
    size_t total = 0U;
    size_t viewable = 0U;

    if (outlines == NULL || total_count == NULL || viewable_count == NULL ||
        topmost == NULL ||
        !XQueryTree(display, root, &root_return, &parent_return, &children,
                    &child_count))
        return -1;
    for (index = 0U; index < child_count; ++index) {
        XWindowAttributes attributes;

        if (!role_matches(children[index], "drag-outline"))
            continue;
        ++total;
        if (!XGetWindowAttributes(display, children[index], &attributes) ||
            attributes.map_state != IsViewable)
            continue;
        if (viewable < (size_t)TEST_DRAG_OUTLINE_COUNT) {
            DragOutlineWindow *outline = &outlines[viewable];

            outline->window = children[index];
            outline->geometry.x = attributes.x;
            outline->geometry.y = attributes.y;
            outline->geometry.width = attributes.width;
            outline->geometry.height = attributes.height;
            outline->override_redirect = attributes.override_redirect;
            outline->stack_index = index;
        }
        ++viewable;
    }
    *topmost = viewable == (size_t)TEST_DRAG_OUTLINE_COUNT &&
               child_count >= (unsigned int)TEST_DRAG_OUTLINE_COUNT;
    if (*topmost) {
        unsigned int first_top =
            child_count - (unsigned int)TEST_DRAG_OUTLINE_COUNT;
        size_t outline_index;

        for (outline_index = 0U;
             outline_index < (size_t)TEST_DRAG_OUTLINE_COUNT;
             ++outline_index) {
            if (outlines[outline_index].stack_index < first_top) {
                *topmost = 0;
                break;
            }
        }
    }
    if (children != NULL)
        XFree(children);
    *total_count = total;
    *viewable_count = viewable;
    return 0;
}

static void expected_drag_outline_geometries(
    const FrameGeometry *bounds,
    FrameGeometry expected[TEST_DRAG_OUTLINE_COUNT])
{
    int horizontal_thickness = TEST_DRAG_OUTLINE_THICKNESS;
    int vertical_thickness = TEST_DRAG_OUTLINE_THICKNESS;
    int middle_height;
    int side_y;

    if (horizontal_thickness > bounds->height)
        horizontal_thickness = bounds->height;
    if (vertical_thickness > bounds->width)
        vertical_thickness = bounds->width;
    middle_height = bounds->height - horizontal_thickness * 2;
    if (middle_height < 1)
        middle_height = 1;
    side_y = bounds->y + (bounds->height - middle_height) / 2;
    expected[0] = (FrameGeometry){
        bounds->x, bounds->y, bounds->width, horizontal_thickness
    };
    expected[1] = (FrameGeometry){
        bounds->x, bounds->y + bounds->height - horizontal_thickness,
        bounds->width, horizontal_thickness
    };
    expected[2] = (FrameGeometry){
        bounds->x, side_y, vertical_thickness, middle_height
    };
    expected[3] = (FrameGeometry){
        bounds->x + bounds->width - vertical_thickness, side_y,
        vertical_thickness, middle_height
    };
}

static int drag_outline_geometries_match(
    const DragOutlineWindow actual[TEST_DRAG_OUTLINE_COUNT],
    const FrameGeometry expected[TEST_DRAG_OUTLINE_COUNT])
{
    int matched[TEST_DRAG_OUTLINE_COUNT] = {0, 0, 0, 0};
    size_t actual_index;

    for (actual_index = 0U;
         actual_index < (size_t)TEST_DRAG_OUTLINE_COUNT; ++actual_index) {
        size_t expected_index;
        int found = 0;

        if (!actual[actual_index].override_redirect)
            return 0;
        for (expected_index = 0U;
             expected_index < (size_t)TEST_DRAG_OUTLINE_COUNT;
             ++expected_index) {
            if (!matched[expected_index] &&
                frame_geometry_equals(&actual[actual_index].geometry,
                                      &expected[expected_index])) {
                matched[expected_index] = 1;
                found = 1;
                break;
            }
        }
        if (!found)
            return 0;
    }
    return 1;
}

static int wait_for_drag_outline_bounds(Window target,
                                        const FrameGeometry *stationary,
                                        const FrameGeometry *bounds,
                                        const char *label)
{
    FrameGeometry expected[TEST_DRAG_OUTLINE_COUNT];
    int attempt;
    size_t last_total = 0U;
    size_t last_viewable = 0U;
    int last_topmost = 0;

    expected_drag_outline_geometries(bounds, expected);
    for (attempt = 0; attempt < 150; ++attempt) {
        DragOutlineWindow outlines[TEST_DRAG_OUTLINE_COUNT];
        FrameGeometry actual = {0, 0, 0, 0};
        XWindowAttributes attributes;

        XSync(display, False);
        if (get_frame_geometry(target, &actual) < 0 ||
            !frame_geometry_equals(&actual, stationary)) {
            fprintf(stderr,
                    "wm-probe: %s moved before the held drag was released "
                    "(expected %d,%d %dx%d, actual %d,%d %dx%d)\n",
                    label, stationary->x, stationary->y,
                    stationary->width, stationary->height,
                    actual.x, actual.y, actual.width, actual.height);
            return -1;
        }
        if (!XGetWindowAttributes(display, target, &attributes) ||
            attributes.map_state != IsViewable) {
            fprintf(stderr,
                    "wm-probe: %s stopped being viewable during a held drag\n",
                    label);
            return -1;
        }
        if (collect_drag_outline_windows(outlines, &last_total,
                                         &last_viewable,
                                         &last_topmost) == 0 &&
            last_total == (size_t)TEST_DRAG_OUTLINE_COUNT &&
            last_viewable == (size_t)TEST_DRAG_OUTLINE_COUNT &&
            last_topmost &&
            drag_outline_geometries_match(outlines, expected))
            return 0;
        wait_a_bit();
    }
    fprintf(stderr,
            "wm-probe: %s drag outline did not reach %d,%d %dx%d "
            "(total %zu, viewable %zu, topmost %d)\n",
            label, bounds->x, bounds->y, bounds->width, bounds->height,
            last_total, last_viewable, last_topmost);
    return -1;
}

static int wait_for_drag_outlines_hidden(const char *label)
{
    int attempt;
    size_t last_total = 0U;
    size_t last_viewable = 0U;

    for (attempt = 0; attempt < 150; ++attempt) {
        DragOutlineWindow outlines[TEST_DRAG_OUTLINE_COUNT];
        int topmost;

        XSync(display, False);
        if (collect_drag_outline_windows(outlines, &last_total,
                                         &last_viewable, &topmost) == 0 &&
            last_total == (size_t)TEST_DRAG_OUTLINE_COUNT &&
            last_viewable == 0U)
            return 0;
        wait_a_bit();
    }
    fprintf(stderr,
            "wm-probe: %s left drag outlines mapped "
            "(total %zu, viewable %zu)\n",
            label, last_total, last_viewable);
    return -1;
}

static int offset_internal_geometry(const FrameGeometry *original,
                                    int delta_x, int delta_y,
                                    FrameGeometry *result)
{
    int screen = DefaultScreen(display);
    int screen_width = DisplayWidth(display, screen);
    int screen_height = DisplayHeight(display, screen);
    int maximum_x;
    int maximum_y;

    if (original == NULL || result == NULL || original->width < 1 ||
        original->height < 1 || original->width > screen_width ||
        original->height > screen_height)
        return -1;
    maximum_x = screen_width - original->width;
    maximum_y = screen_height - original->height;
    *result = *original;
    result->x += delta_x;
    result->y += delta_y;
    if (result->x < 0)
        result->x = 0;
    if (result->x > maximum_x)
        result->x = maximum_x;
    if (result->y < 0)
        result->y = 0;
    if (result->y > maximum_y)
        result->y = maximum_y;
    return 0;
}

static int verify_internal_outline_drag(Window window, const char *label,
                                        int first_dx, int first_dy,
                                        int second_dx, int second_dy)
{
    FrameGeometry original;
    FrameGeometry first;
    FrameGeometry second;
    int start_x;
    int start_y;
    int result = -1;

    if (get_frame_geometry(window, &original) < 0 ||
        offset_internal_geometry(&original, first_dx, first_dy, &first) < 0 ||
        offset_internal_geometry(&original, second_dx, second_dy,
                                 &second) < 0) {
        fprintf(stderr,
                "wm-probe: %s geometry could not support outline testing\n",
                label);
        return -1;
    }
    if (frame_geometry_equals(&original, &first) ||
        frame_geometry_equals(&original, &second) ||
        frame_geometry_equals(&first, &second)) {
        fprintf(stderr,
                "wm-probe: skipping %s outline motion on an immovable display\n",
                label);
        return 0;
    }
    if (wait_for_drag_outlines_hidden(label) < 0)
        return -1;
    start_x = original.x + original.width / 2;
    start_y = original.y + TEST_TITLE_BUTTON_Y + TEST_TITLE_BUTTON / 2;
    if (begin_held_drag_at(start_x, start_y) < 0)
        goto cleanup;
    if (move_held_drag_to(start_x + first.x - original.x,
                          start_y + first.y - original.y) < 0 ||
        wait_for_drag_outline_bounds(window, &original, &first, label) < 0 ||
        frame_geometry_remains(window, &original) < 0 ||
        !internal_window_remains_viewable(window)) {
        fprintf(stderr,
                "wm-probe: %s did not remain stationary at its first outline\n",
                label);
        goto cleanup;
    }
    if (move_held_drag_to(start_x + second.x - original.x,
                          start_y + second.y - original.y) < 0 ||
        wait_for_drag_outline_bounds(window, &original, &second, label) < 0 ||
        frame_geometry_remains(window, &original) < 0 ||
        !internal_window_remains_viewable(window)) {
        fprintf(stderr,
                "wm-probe: %s did not remain stationary at its second outline\n",
                label);
        goto cleanup;
    }
    if (release_held_drag() < 0)
        goto cleanup;
    if (wait_for_frame_geometry(window, &second) < 0) {
        report_frame_geometry("outline drag did not commit on release", window);
        goto cleanup;
    }
    if (wait_for_drag_outlines_hidden(label) < 0) {
        fprintf(stderr,
                "wm-probe: %s drag outlines remained after release\n", label);
        goto cleanup;
    }
    if (wait_for_pointer_available() < 0) {
        fprintf(stderr,
                "wm-probe: %s drag left the pointer grabbed after release\n",
                label);
        goto cleanup;
    }
    result = 0;

cleanup:
    abort_held_drag();
    return result;
}

static int wait_for_restored_frame(Window frame,
                                   const FrameGeometry *original,
                                   int require_new_position)
{
    int attempt;

    for (attempt = 0; attempt < 150; ++attempt) {
        FrameGeometry actual;

        XSync(display, False);
        if (get_frame_geometry(frame, &actual) == 0 &&
            actual.width == original->width &&
            actual.height == original->height &&
            (!require_new_position ||
             (actual.x != original->x || actual.y != original->y)))
            return 0;
        wait_a_bit();
    }
    return -1;
}

static int click_frame_maximize_button(Window frame)
{
    FrameGeometry geometry;
    int close_x;
    int maximize_x;

    if (get_frame_geometry(frame, &geometry) < 0)
        return -1;
    close_x = geometry.width - TEST_FRAME_RIGHT - TEST_TITLE_BUTTON -
              TEST_TITLE_BUTTON_RIGHT_INSET;
    maximize_x = close_x - TEST_TITLE_BUTTON - TEST_TITLE_BUTTON_GAP;
    return fake_click_on_root_child(
        frame, geometry.x + maximize_x + TEST_TITLE_BUTTON / 2,
        geometry.y + TEST_TITLE_BUTTON_Y + TEST_TITLE_BUTTON / 2);
}

static int click_internal_maximize_button(Window window)
{
    FrameGeometry geometry;
    int close_x;
    int maximize_x;

    if (get_frame_geometry(window, &geometry) < 0)
        return -1;
    close_x = geometry.width - TEST_TITLE_BUTTON -
              TEST_INTERNAL_CLOSE_INSET;
    maximize_x = close_x - TEST_TITLE_BUTTON - TEST_TITLE_BUTTON_GAP;
    return fake_click_at(geometry.x + maximize_x + TEST_TITLE_BUTTON / 2,
                         geometry.y + TEST_TITLE_BUTTON_Y +
                             TEST_TITLE_BUTTON / 2);
}

static int drag_frame_title_to(Window frame, int root_x, int root_y)
{
    FrameGeometry geometry;

    if (get_frame_geometry(frame, &geometry) < 0)
        return -1;
    return fake_drag_at(geometry.x + geometry.width / 2,
                        geometry.y + TEST_TITLE_BUTTON_Y +
                            TEST_TITLE_BUTTON / 2,
                        root_x, root_y);
}

static int drag_internal_back_to_geometry(Window window,
                                          const FrameGeometry *original)
{
    FrameGeometry arranged;
    int pointer_offset_x;
    int destination_x;

    if (get_frame_geometry(window, &arranged) < 0 || arranged.width < 1)
        return -1;
    pointer_offset_x = arranged.width / 2;
    destination_x = original->x +
                    (int)((long long)pointer_offset_x * original->width /
                          arranged.width);
    if (fake_drag_at(arranged.x + pointer_offset_x,
                     arranged.y + TEST_TITLE_BUTTON_Y +
                         TEST_TITLE_BUTTON / 2,
                     destination_x,
                     original->y + TEST_TITLE_BUTTON_Y +
                         TEST_TITLE_BUTTON / 2) < 0)
        return -1;
    return wait_for_restored_frame(window, original, 0);
}

static void report_frame_geometry(const char *message, Window frame)
{
    FrameGeometry geometry;

    if (get_frame_geometry(frame, &geometry) == 0) {
        fprintf(stderr, "wm-probe: %s (actual %d,%d %dx%d)\n", message,
                geometry.x, geometry.y, geometry.width, geometry.height);
    } else {
        fprintf(stderr, "wm-probe: %s (geometry unavailable)\n", message);
    }
}

static int wait_for_conflicting_client_click(Window client, Window frame,
                                             Window other_frame)
{
    int attempt;
    int saw_button_press = 0;

    for (attempt = 0; attempt < 150; ++attempt) {
        XEvent event;

        XSync(display, False);
        while (XCheckWindowEvent(display, client, ButtonPressMask, &event)) {
            if (event.type == ButtonPress)
                saw_button_press = 1;
        }
        if (saw_button_press &&
            client_is_active_and_above(client, frame, other_frame))
            return 0;
        wait_a_bit();
    }
    {
        Window focused = None;
        int revert_to;
        Atom active_window = XInternAtom(display, "_NET_ACTIVE_WINDOW", False);

        XGetInputFocus(display, &focused, &revert_to);
        fprintf(stderr,
                "wm-probe: conflicting click details: replay=%d focus=0x%lx "
                "active=0x%lx raised=%d\n",
                saw_button_press, (unsigned long)focused,
                (unsigned long)root_window_property(active_window),
                window_is_above(frame, other_frame));
    }
    return -1;
}

static void send_iconify(Window client, Atom change_state)
{
    XEvent event;

    memset(&event, 0, sizeof(event));
    event.xclient.type = ClientMessage;
    event.xclient.display = display;
    event.xclient.window = client;
    event.xclient.message_type = change_state;
    event.xclient.format = 32;
    event.xclient.data.l[0] = IconicState;
    XSendEvent(display, root, False,
               SubstructureRedirectMask | SubstructureNotifyMask, &event);
    XFlush(display);
}

static void send_net_wm_state(Window client, long action, Atom first,
                              Atom second)
{
    XEvent event;

    memset(&event, 0, sizeof(event));
    event.xclient.type = ClientMessage;
    event.xclient.display = display;
    event.xclient.window = client;
    event.xclient.message_type = net_wm_state_atom;
    event.xclient.format = 32;
    event.xclient.data.l[0] = action;
    event.xclient.data.l[1] = (long)first;
    event.xclient.data.l[2] = (long)second;
    XSendEvent(display, root, False,
               SubstructureRedirectMask | SubstructureNotifyMask, &event);
    XFlush(display);
}

static int verify_stale_focus_is_ignored(Window minimized, Window expected_active)
{
    XEvent event;
    Atom active_window = XInternAtom(display, "_NET_ACTIVE_WINDOW", False);
    int attempt;

    memset(&event, 0, sizeof(event));
    event.xfocus.type = FocusIn;
    event.xfocus.display = display;
    event.xfocus.window = minimized;
    event.xfocus.mode = NotifyNormal;
    event.xfocus.detail = NotifyNonlinear;
    for (attempt = 0; attempt < 10; ++attempt) {
        XSendEvent(display, minimized, False, FocusChangeMask, &event);
        XFlush(display);
        wait_a_bit();
        if (root_window_property(active_window) != expected_active) {
            fprintf(stderr,
                    "wm-probe: stale FocusIn reactivated a minimized client\n");
            return -1;
        }
    }
    return 0;
}

static int verify_click_to_raise(Atom change_state)
{
    Window lower;
    Window upper;
    Window lower_frame;
    Window upper_frame;
    Window conflicting;
    Window cover;
    Window conflicting_frame;
    Window cover_frame;
    unsigned long black = BlackPixel(display, DefaultScreen(display));
    unsigned long white = WhitePixel(display, DefaultScreen(display));

    lower = XCreateSimpleWindow(display, root, 80, 80, 260, 160, 0,
                                black, white);
    XStoreName(display, lower, "Win31 X Focus Lower");
    XMapWindow(display, lower);
    XFlush(display);
    if (wait_for_state(lower, NormalState) < 0) {
        fprintf(stderr, "wm-probe: lower focus client was not managed\n");
        return -1;
    }
    upper = XCreateSimpleWindow(display, root, 220, 160, 260, 160, 0,
                                black, white);
    XStoreName(display, upper, "Win31 X Focus Upper");
    XMapWindow(display, upper);
    XFlush(display);
    if (wait_for_state(upper, NormalState) < 0) {
        fprintf(stderr, "wm-probe: upper focus client was not managed\n");
        return -1;
    }
    lower_frame = client_frame(lower);
    upper_frame = client_frame(upper);
    if (lower_frame == None || upper_frame == None ||
        wait_for_active_and_above(upper, upper_frame, lower_frame) < 0) {
        fprintf(stderr, "wm-probe: newly mapped client was not active and above\n");
        return -1;
    }
    if (fake_click_at(100, 110) < 0 ||
        wait_for_active_and_above(lower, lower_frame, upper_frame) < 0) {
        fprintf(stderr,
                "wm-probe: clicking lower client did not focus and raise it\n");
        return -1;
    }
    if (fake_click_at(430, 290) < 0 ||
        wait_for_active_and_above(upper, upper_frame, lower_frame) < 0) {
        fprintf(stderr,
                "wm-probe: clicking upper client did not restore its stack position\n");
        return -1;
    }

    send_iconify(lower, change_state);
    if (wait_for_state(lower, IconicState) < 0) {
        fprintf(stderr, "wm-probe: focus client did not minimize\n");
        return -1;
    }
    if (verify_stale_focus_is_ignored(lower, upper) < 0)
        return -1;
    XDestroyWindow(display, lower);
    XDestroyWindow(display, upper);
    XSync(display, False);

    conflicting = XCreateSimpleWindow(display, root, 80, 400, 300, 180, 0,
                                      black, white);
    XStoreName(display, conflicting, "Win31 X Conflicting Grab");
    XSelectInput(display, conflicting, ButtonPressMask | FocusChangeMask);
    XGrabButton(display, Button1, AnyModifier, conflicting, False,
                ButtonPressMask, GrabModeAsync, GrabModeAsync, None, None);
    XMapWindow(display, conflicting);
    XFlush(display);
    if (wait_for_state(conflicting, NormalState) < 0) {
        fprintf(stderr, "wm-probe: conflicting-grab client was not managed\n");
        return -1;
    }
    cover = XCreateSimpleWindow(display, root, 230, 480, 260, 150, 0,
                                black, white);
    XStoreName(display, cover, "Win31 X Grab Cover");
    XMapWindow(display, cover);
    XFlush(display);
    if (wait_for_state(cover, NormalState) < 0) {
        fprintf(stderr, "wm-probe: conflicting-grab cover was not managed\n");
        return -1;
    }
    conflicting_frame = client_frame(conflicting);
    cover_frame = client_frame(cover);
    if (conflicting_frame == None || cover_frame == None ||
        wait_for_active_and_above(cover, cover_frame, conflicting_frame) < 0) {
        fprintf(stderr, "wm-probe: grab cover was not active and above\n");
        return -1;
    }
    if (fake_click_at(100, 430) < 0 ||
        wait_for_conflicting_client_click(conflicting, conflicting_frame,
                                          cover_frame) < 0) {
        fprintf(stderr,
                "wm-probe: conflicting-grab click was not replayed, focused, and raised\n");
        return -1;
    }
    XDestroyWindow(display, conflicting);
    XDestroyWindow(display, cover);
    XSync(display, False);
    return 0;
}

static int wait_for_map_state(Window window, int expected)
{
    int attempt;

    for (attempt = 0; attempt < 150; ++attempt) {
        XWindowAttributes attributes;

        XSync(display, False);
        if (XGetWindowAttributes(display, window, &attributes) &&
            attributes.map_state == expected)
            return 0;
        wait_a_bit();
    }
    return -1;
}

static int wait_for_active_with_stack(Window client, Window upper,
                                      Window lower)
{
    Atom active_window = XInternAtom(display, "_NET_ACTIVE_WINDOW", False);
    int attempt;

    for (attempt = 0; attempt < 150; ++attempt) {
        Window focused = None;
        int revert_to;

        XSync(display, False);
        XGetInputFocus(display, &focused, &revert_to);
        if (focused == client && root_window_property(active_window) == client &&
            window_is_above(upper, lower))
            return 0;
        wait_a_bit();
    }
    return -1;
}

static int verify_transient_lifecycle(Atom change_state)
{
    Window owner;
    Window dialog;
    Window owner_frame;
    Window dialog_frame;
    Window icon;
    unsigned long black = BlackPixel(display, DefaultScreen(display));
    unsigned long white = WhitePixel(display, DefaultScreen(display));

    owner = XCreateSimpleWindow(display, root, 100, 100, 320, 220, 0,
                                black, white);
    XStoreName(display, owner, "Win31 X Transient Owner");
    XMapWindow(display, owner);
    XFlush(display);
    if (wait_for_state(owner, NormalState) < 0) {
        fprintf(stderr, "wm-probe: transient owner was not managed\n");
        return -1;
    }
    dialog = XCreateSimpleWindow(display, root, 220, 170, 180, 100, 0,
                                 black, white);
    XStoreName(display, dialog, "Win31 X Transient Dialog");
    XSetTransientForHint(display, dialog, owner);
    XMapWindow(display, dialog);
    XFlush(display);
    if (wait_for_state(dialog, NormalState) < 0) {
        fprintf(stderr, "wm-probe: transient dialog was not managed\n");
        return -1;
    }
    owner_frame = client_frame(owner);
    dialog_frame = client_frame(dialog);
    if (owner_frame == None || dialog_frame == None ||
        !window_is_above(dialog_frame, owner_frame)) {
        fprintf(stderr, "wm-probe: transient dialog was not above its owner\n");
        return -1;
    }
    if (fake_click_at(120, 130) < 0 ||
        wait_for_active_with_stack(owner, dialog_frame, owner_frame) < 0) {
        fprintf(stderr,
                "wm-probe: clicking transient owner covered its dialog\n");
        return -1;
    }

    send_iconify(owner, change_state);
    if (wait_for_state(owner, IconicState) < 0 ||
        wait_for_state(dialog, IconicState) < 0 ||
        wait_for_map_state(owner, IsUnmapped) < 0 ||
        wait_for_map_state(dialog, IsUnmapped) < 0) {
        fprintf(stderr, "wm-probe: minimizing owner did not hide transient group\n");
        return -1;
    }
    icon = wait_for_role("minimized-icon", owner, 1);
    if (icon == None) {
        fprintf(stderr, "wm-probe: minimized transient owner has no desktop icon\n");
        return -1;
    }
    if (find_role("minimized-icon", dialog, 0) != None) {
        fprintf(stderr, "wm-probe: transient dialog received a separate icon\n");
        return -1;
    }
    send_button_one(icon);
    if (wait_for_state(owner, NormalState) < 0 ||
        wait_for_state(dialog, NormalState) < 0 ||
        wait_for_map_state(owner, IsViewable) < 0 ||
        wait_for_map_state(dialog, IsViewable) < 0 ||
        !window_is_above(dialog_frame, owner_frame)) {
        fprintf(stderr, "wm-probe: restoring owner did not restore transient group\n");
        return -1;
    }

    XDestroyWindow(display, dialog);
    XDestroyWindow(display, owner);
    XSync(display, False);
    return 0;
}

static int read_client_list(Window **windows, unsigned long *count)
{
    Atom client_list = XInternAtom(display, "_NET_CLIENT_LIST", False);
    Atom type;
    int format;
    unsigned long after;
    unsigned char *data = NULL;

    *windows = NULL;
    *count = 0;
    if (XGetWindowProperty(display, root, client_list, 0, 4096, False,
                           XA_WINDOW, &type, &format, count, &after,
                           &data) != Success || type != XA_WINDOW ||
        format != 32) {
        if (data != NULL)
            XFree(data);
        return -1;
    }
    *windows = (Window *)data;
    return 0;
}

static int client_list_contains(Window target)
{
    Window *windows = NULL;
    unsigned long count = 0;
    unsigned long index;
    int found = 0;

    if (read_client_list(&windows, &count) < 0)
        return 0;
    for (index = 0; index < count; ++index) {
        if (windows[index] == target) {
            found = 1;
            break;
        }
    }
    if (windows != NULL)
        XFree(windows);
    return found;
}

static int wait_for_client_list_order(Window older, Window newer)
{
    int attempt;

    for (attempt = 0; attempt < 150; ++attempt) {
        Window *windows = NULL;
        unsigned long count = 0;
        unsigned long index;
        long older_index = -1;
        long newer_index = -1;

        XSync(display, False);
        if (read_client_list(&windows, &count) == 0) {
            for (index = 0; index < count; ++index) {
                if (windows[index] == older)
                    older_index = (long)index;
                if (windows[index] == newer)
                    newer_index = (long)index;
            }
        }
        if (windows != NULL)
            XFree(windows);
        if (older_index >= 0 && newer_index > older_index)
            return 0;
        wait_a_bit();
    }
    return -1;
}

static int verify_client_list_order(void)
{
    Window older;
    Window newer;
    unsigned long black = BlackPixel(display, DefaultScreen(display));
    unsigned long white = WhitePixel(display, DefaultScreen(display));

    older = XCreateSimpleWindow(display, root, 520, 80, 180, 100, 0,
                                black, white);
    XStoreName(display, older, "Win31 X Older Client");
    XMapWindow(display, older);
    XFlush(display);
    if (wait_for_state(older, NormalState) < 0) {
        fprintf(stderr, "wm-probe: older client-list window was not managed\n");
        return -1;
    }
    newer = XCreateSimpleWindow(display, root, 580, 130, 180, 100, 0,
                                black, white);
    XStoreName(display, newer, "Win31 X Newer Client");
    XMapWindow(display, newer);
    XFlush(display);
    if (wait_for_state(newer, NormalState) < 0) {
        fprintf(stderr, "wm-probe: newer client-list window was not managed\n");
        return -1;
    }
    if (wait_for_client_list_order(older, newer) < 0) {
        fprintf(stderr, "wm-probe: _NET_CLIENT_LIST is not oldest-first\n");
        return -1;
    }
    XDestroyWindow(display, newer);
    XDestroyWindow(display, older);
    XSync(display, False);
    return 0;
}

static int verify_client_stack_requests(void)
{
    Window lower;
    Window upper;
    Window lower_frame;
    Window upper_frame;
    Window upper_overlay;
    unsigned long black = BlackPixel(display, DefaultScreen(display));
    unsigned long white = WhitePixel(display, DefaultScreen(display));

    lower = XCreateSimpleWindow(display, root, 520, 80, 180, 100, 0,
                                black, white);
    XStoreName(display, lower, "Win31 X Stack Lower");
    XMapWindow(display, lower);
    XFlush(display);
    if (wait_for_state(lower, NormalState) < 0) {
        fprintf(stderr, "wm-probe: lower stack client was not managed\n");
        return -1;
    }
    upper = XCreateSimpleWindow(display, root, 580, 130, 180, 100, 0,
                                black, white);
    XStoreName(display, upper, "Win31 X Stack Upper");
    XSelectInput(display, upper, ButtonPressMask);
    XMapWindow(display, upper);
    XFlush(display);
    if (wait_for_state(upper, NormalState) < 0) {
        fprintf(stderr, "wm-probe: upper stack client was not managed\n");
        return -1;
    }
    lower_frame = client_frame(lower);
    upper_frame = client_frame(upper);
    upper_overlay = upper_frame != None
                        ? child_with_role(upper_frame, "focus-overlay")
                        : None;
    if (lower_frame == None || upper_frame == None ||
        upper_overlay == None ||
        wait_for_above(upper_frame, lower_frame) < 0 ||
        wait_for_map_state(upper_overlay, IsUnmapped) < 0) {
        fprintf(stderr, "wm-probe: initial stack order was invalid\n");
        return -1;
    }
    XLowerWindow(display, upper);
    XFlush(display);
    if (wait_for_above(lower_frame, upper_frame) < 0 ||
        wait_for_map_state(upper_overlay, IsViewable) < 0) {
        fprintf(stderr, "wm-probe: client Below request was overridden\n");
        return -1;
    }
    if (fake_click_at(730, 210) < 0 ||
        wait_for_conflicting_client_click(upper, upper_frame, lower_frame) < 0 ||
        wait_for_map_state(upper_overlay, IsUnmapped) < 0) {
        fprintf(stderr,
                "wm-probe: clicking active-but-obscured client did not replay and raise\n");
        return -1;
    }
    XLowerWindow(display, upper);
    XFlush(display);
    if (wait_for_above(lower_frame, upper_frame) < 0) {
        fprintf(stderr, "wm-probe: repeated client Below request was overridden\n");
        return -1;
    }
    XRaiseWindow(display, upper);
    XFlush(display);
    if (wait_for_above(upper_frame, lower_frame) < 0) {
        fprintf(stderr, "wm-probe: client Above request was not honored\n");
        return -1;
    }
    XDestroyWindow(display, upper);
    XDestroyWindow(display, lower);
    XSync(display, False);
    return 0;
}

static int verify_input_only_map(void)
{
    XSetWindowAttributes attributes;
    Window input_only;

    memset(&attributes, 0, sizeof(attributes));
    input_only = XCreateWindow(display, root, 760, 40, 24, 24, 0, 0,
                               InputOnly, CopyFromParent, 0, &attributes);
    XMapWindow(display, input_only);
    XFlush(display);
    if (wait_for_map_state(input_only, IsViewable) < 0) {
        fprintf(stderr, "wm-probe: top-level InputOnly window was not mapped\n");
        return -1;
    }
    if (client_list_contains(input_only)) {
        fprintf(stderr, "wm-probe: InputOnly window was added to client list\n");
        return -1;
    }
    XDestroyWindow(display, input_only);
    XSync(display, False);
    return 0;
}

static int wait_for_minimum_geometry(Window window, int minimum_width,
                                     int minimum_height)
{
    int attempt;

    for (attempt = 0; attempt < 150; ++attempt) {
        XWindowAttributes attributes;

        XSync(display, False);
        if (XGetWindowAttributes(display, window, &attributes) &&
            attributes.width >= minimum_width &&
            attributes.height >= minimum_height)
            return 0;
        wait_a_bit();
    }
    return -1;
}

static int wait_for_configured_minimum(Window window, int minimum_width,
                                       int minimum_height)
{
    int attempt;

    for (attempt = 0; attempt < 150; ++attempt) {
        XEvent event;

        XSync(display, False);
        while (XCheckWindowEvent(display, window, StructureNotifyMask, &event)) {
            if (event.type == ConfigureNotify &&
                event.xconfigure.width >= minimum_width &&
                event.xconfigure.height >= minimum_height)
                return 0;
        }
        wait_a_bit();
    }
    return -1;
}

static int verify_resize_increment_minimum(void)
{
    Window window;
    XSizeHints hints;
    XEvent event;
    unsigned long black = BlackPixel(display, DefaultScreen(display));
    unsigned long white = WhitePixel(display, DefaultScreen(display));

    window = XCreateSimpleWindow(display, root, 500, 360, 1, 1, 0,
                                 black, white);
    XStoreName(display, window, "Win31 X Resize Increment Probe");
    XSelectInput(display, window, StructureNotifyMask);
    memset(&hints, 0, sizeof(hints));
    hints.flags = PBaseSize | PResizeInc;
    hints.base_width = 0;
    hints.base_height = 0;
    hints.width_inc = 100;
    hints.height_inc = 60;
    XSetWMNormalHints(display, window, &hints);
    XMapWindow(display, window);
    XFlush(display);
    if (wait_for_state(window, NormalState) < 0 ||
        wait_for_minimum_geometry(window, 96, 48) < 0) {
        XWindowAttributes actual;

        memset(&actual, 0, sizeof(actual));
        XGetWindowAttributes(display, window, &actual);
        fprintf(stderr,
                "wm-probe: resize increments defeated hard minimum on map "
                "(%dx%d)\n", actual.width, actual.height);
        return -1;
    }
    while (XCheckWindowEvent(display, window, StructureNotifyMask, &event))
        ;
    XResizeWindow(display, window, 1, 1);
    XFlush(display);
    if (wait_for_configured_minimum(window, 96, 48) < 0 ||
        wait_for_minimum_geometry(window, 96, 48) < 0) {
        fprintf(stderr,
                "wm-probe: resize increments defeated hard minimum on request\n");
        return -1;
    }
    XDestroyWindow(display, window);
    XSync(display, False);
    return 0;
}

static int gravity_horizontal_factor(int gravity)
{
    if (gravity == NorthGravity || gravity == CenterGravity ||
        gravity == SouthGravity)
        return 1;
    if (gravity == NorthEastGravity || gravity == EastGravity ||
        gravity == SouthEastGravity)
        return 2;
    return 0;
}

static int gravity_vertical_factor(int gravity)
{
    if (gravity == WestGravity || gravity == CenterGravity ||
        gravity == EastGravity)
        return 1;
    if (gravity == SouthWestGravity || gravity == SouthGravity ||
        gravity == SouthEastGravity)
        return 2;
    return 0;
}

static int verify_gravity_case(int gravity, int set_hint)
{
    const int requested_x = 300;
    const int requested_y = 200;
    const int requested_border = 7;
    Window window;
    Window frame;
    Window child = None;
    XWindowAttributes client_attributes;
    XWindowAttributes frame_attributes;
    XSizeHints hints;
    int inner_x = 0;
    int inner_y = 0;
    int horizontal = gravity_horizontal_factor(gravity);
    int vertical = gravity_vertical_factor(gravity);
    unsigned long black = BlackPixel(display, DefaultScreen(display));
    unsigned long white = WhitePixel(display, DefaultScreen(display));

    window = XCreateSimpleWindow(display, root, requested_x, requested_y,
                                 180, 100, (unsigned)requested_border,
                                 black, white);
    XStoreName(display, window, "Win31 X Gravity Probe");
    if (set_hint) {
        memset(&hints, 0, sizeof(hints));
        hints.flags = PWinGravity;
        hints.win_gravity = gravity;
        XSetWMNormalHints(display, window, &hints);
    }
    XMapWindow(display, window);
    XFlush(display);
    if (wait_for_state(window, NormalState) < 0) {
        fprintf(stderr, "wm-probe: gravity client was not managed\n");
        return -1;
    }
    frame = client_frame(window);
    if (frame == None ||
        !XGetWindowAttributes(display, window, &client_attributes) ||
        !XGetWindowAttributes(display, frame, &frame_attributes) ||
        !XTranslateCoordinates(display, window, root, 0, 0,
                               &inner_x, &inner_y, &child)) {
        fprintf(stderr, "wm-probe: gravity geometry was unavailable\n");
        return -1;
    }
    if (gravity == StaticGravity) {
        if (inner_x != requested_x + requested_border ||
            inner_y != requested_y + requested_border) {
            fprintf(stderr, "wm-probe: StaticGravity moved the client interior\n");
            return -1;
        }
    } else if (2LL * requested_x +
                       (long long)horizontal *
                           (client_attributes.width + 2 * requested_border) !=
                   2LL * frame_attributes.x +
                       (long long)horizontal * frame_attributes.width ||
               2LL * requested_y +
                       (long long)vertical *
                           (client_attributes.height + 2 * requested_border) !=
                   2LL * frame_attributes.y +
                       (long long)vertical * frame_attributes.height) {
        fprintf(stderr, "wm-probe: gravity reference points were not aligned\n");
        return -1;
    }
    if (!set_hint) {
        int attempt;

        memset(&hints, 0, sizeof(hints));
        hints.flags = PWinGravity;
        hints.win_gravity = StaticGravity;
        XSetWMNormalHints(display, window, &hints);
        XMoveWindow(display, window, 360, 240);
        XFlush(display);
        for (attempt = 0; attempt < 150; ++attempt) {
            XSync(display, False);
            if (XTranslateCoordinates(display, window, root, 0, 0,
                                      &inner_x, &inner_y, &child) &&
                inner_x == 360 + requested_border &&
                inner_y == 240 + requested_border)
                break;
            wait_a_bit();
        }
        if (attempt == 150) {
            fprintf(stderr,
                    "wm-probe: updated PWinGravity was not used for configure\n");
            return -1;
        }
    }
    XDestroyWindow(display, window);
    XSync(display, False);
    return 0;
}

static int verify_window_gravity(void)
{
    if (verify_gravity_case(NorthWestGravity, 0) < 0 ||
        verify_gravity_case(CenterGravity, 1) < 0 ||
        verify_gravity_case(SouthEastGravity, 1) < 0 ||
        verify_gravity_case(StaticGravity, 1) < 0)
        return -1;
    return 0;
}

static int verify_synthetic_configure_geometry(void)
{
    const int requested_border = 7;
    Window window;
    XEvent event;
    int attempt;
    int destination_x = 0;
    int destination_y = 0;
    Window child = None;
    unsigned long black = BlackPixel(display, DefaultScreen(display));
    unsigned long white = WhitePixel(display, DefaultScreen(display));

    window = XCreateSimpleWindow(display, root, 430, 280, 180, 100,
                                 (unsigned)requested_border, black, white);
    XStoreName(display, window, "Win31 X Configure Geometry Probe");
    XSelectInput(display, window, StructureNotifyMask);
    XMapWindow(display, window);
    XFlush(display);
    if (wait_for_state(window, NormalState) < 0) {
        fprintf(stderr, "wm-probe: configure geometry client was not managed\n");
        return -1;
    }
    XSync(display, False);
    while (XCheckWindowEvent(display, window, StructureNotifyMask, &event))
        ;
    XMoveWindow(display, window, 460, 310);
    XFlush(display);
    for (attempt = 0; attempt < 150; ++attempt) {
        XSync(display, False);
        while (XCheckWindowEvent(display, window, StructureNotifyMask, &event)) {
            if (event.type != ConfigureNotify || !event.xconfigure.send_event)
                continue;
            if (!XTranslateCoordinates(display, window, root, 0, 0,
                                       &destination_x, &destination_y, &child) ||
                event.xconfigure.x != destination_x - requested_border ||
                event.xconfigure.y != destination_y - requested_border ||
                event.xconfigure.border_width != requested_border) {
                fprintf(stderr,
                        "wm-probe: synthetic ConfigureNotify geometry is invalid\n");
                return -1;
            }
            destination_x = event.xconfigure.x;
            destination_y = event.xconfigure.y;
            XUnmapWindow(display, window);
            XFlush(display);
            if (wait_until_unmanaged(window) < 0) {
                fprintf(stderr,
                        "wm-probe: bordered client was not cleanly unmanaged\n");
                return -1;
            }
            {
                XWindowAttributes attributes;

                if (!XGetWindowAttributes(display, window, &attributes) ||
                    attributes.x != destination_x ||
                    attributes.y != destination_y ||
                    attributes.border_width != requested_border) {
                    fprintf(stderr,
                            "wm-probe: bordered client shifted during unmanage\n");
                    return -1;
                }
            }
            XDestroyWindow(display, window);
            XSync(display, False);
            return 0;
        }
        wait_a_bit();
    }
    fprintf(stderr, "wm-probe: no synthetic ConfigureNotify was received\n");
    return -1;
}

static int verify_maximize_and_snap(void)
{
    int screen = DefaultScreen(display);
    int screen_width = DisplayWidth(display, screen);
    int screen_height = DisplayHeight(display, screen);
    Atom net_supported = XInternAtom(display, "_NET_SUPPORTED", False);
    Window window;
    Window frame;
    FrameGeometry original;
    FrameGeometry maximized = {0, 0, screen_width, screen_height};
    FrameGeometry left = {0, 0, screen_width / 2, screen_height};
    FrameGeometry right = {
        screen_width / 2,
        0,
        screen_width - screen_width / 2,
        screen_height
    };
    unsigned long black = BlackPixel(display, screen);
    unsigned long white = WhitePixel(display, screen);

    if (!window_atom_property_contains(root, net_supported,
                                       maximized_horz_atom) ||
        !window_atom_property_contains(root, net_supported,
                                       maximized_vert_atom)) {
        fprintf(stderr,
                "wm-probe: EWMH maximize atoms were not advertised\n");
        return -1;
    }

    window = XCreateSimpleWindow(display, root, 180, 120, 320, 180, 0,
                                 black, white);
    XStoreName(display, window, "Win31 X Maximize and Snap Probe");
    XMapWindow(display, window);
    XFlush(display);
    if (wait_for_state(window, NormalState) < 0) {
        fprintf(stderr, "wm-probe: maximize client was not managed\n");
        return -1;
    }
    frame = client_frame(window);
    if (frame == None || wait_for_map_state(frame, IsViewable) < 0 ||
        get_frame_geometry(frame, &original) < 0) {
        fprintf(stderr, "wm-probe: maximize client frame was unavailable\n");
        return -1;
    }

    if (click_frame_maximize_button(frame) < 0 ||
        wait_for_frame_geometry(frame, &maximized) < 0) {
        report_frame_geometry("maximize button did not fill the work area", frame);
        return -1;
    }
    if (wait_for_maximized_state(window, 1) < 0) {
        fprintf(stderr,
                "wm-probe: maximize did not publish both EWMH state atoms\n");
        return -1;
    }
    if (click_frame_maximize_button(frame) < 0 ||
        wait_for_frame_geometry(frame, &original) < 0) {
        report_frame_geometry("maximize toggle did not restore exact geometry",
                              frame);
        return -1;
    }
    if (wait_for_maximized_state(window, 0) < 0) {
        fprintf(stderr,
                "wm-probe: restore did not clear both EWMH maximize atoms\n");
        return -1;
    }

    if (click_frame_maximize_button(frame) < 0 ||
        wait_for_frame_geometry(frame, &maximized) < 0 ||
        drag_frame_title_to(frame, screen_width / 2, screen_height / 3) < 0 ||
        wait_for_restored_frame(frame, &original, 1) < 0) {
        report_frame_geometry("dragging a maximized title did not restore it",
                              frame);
        return -1;
    }
    if (wait_for_maximized_state(window, 0) < 0) {
        fprintf(stderr,
                "wm-probe: dragging out of maximize retained EWMH state\n");
        return -1;
    }

    if (drag_frame_title_to(frame, 0, screen_height / 3) < 0 ||
        wait_for_frame_geometry(frame, &left) < 0) {
        report_frame_geometry("left-edge title drag did not snap left", frame);
        return -1;
    }
    if (wait_for_maximized_state(window, 0) < 0) {
        fprintf(stderr, "wm-probe: left snap incorrectly claimed maximize state\n");
        return -1;
    }
    send_net_wm_state(window, TEST_NET_WM_STATE_REMOVE, maximized_horz_atom,
                      maximized_vert_atom);
    if (frame_geometry_remains(frame, &left) < 0) {
        report_frame_geometry(
            "removing maximize state from a snapped client unsnapped it", frame);
        return -1;
    }
    if (click_frame_maximize_button(frame) < 0 ||
        wait_for_frame_geometry(frame, &maximized) < 0 ||
        wait_for_maximized_state(window, 1) < 0 ||
        click_frame_maximize_button(frame) < 0 ||
        wait_for_frame_geometry(frame, &left) < 0 ||
        wait_for_maximized_state(window, 0) < 0) {
        report_frame_geometry(
            "maximizing a snapped client did not restore its prior snap", frame);
        return -1;
    }
    if (drag_frame_title_to(frame, screen_width - 1, screen_height / 3) < 0 ||
        wait_for_frame_geometry(frame, &right) < 0) {
        report_frame_geometry("right-edge title drag did not snap right", frame);
        return -1;
    }
    if (wait_for_maximized_state(window, 0) < 0) {
        fprintf(stderr, "wm-probe: right snap incorrectly claimed maximize state\n");
        return -1;
    }
    if (drag_frame_title_to(frame, screen_width / 2,
                            screen_height / 2) < 0 ||
        wait_for_restored_frame(frame, &original, 0) < 0) {
        report_frame_geometry("dragging a snapped title did not restore its size",
                              frame);
        return -1;
    }

    XDestroyWindow(display, window);
    XSync(display, False);
    return 0;
}

static int wait_for_active_client(Window client)
{
    Atom active_window = XInternAtom(display, "_NET_ACTIVE_WINDOW", False);
    int attempt;

    for (attempt = 0; attempt < 150; ++attempt) {
        Window focused = None;
        int revert_to;

        XSync(display, False);
        XGetInputFocus(display, &focused, &revert_to);
        if (focused == client && root_window_property(active_window) == client)
            return 0;
        wait_a_bit();
    }
    return -1;
}

static int request_active_client(Window client)
{
    XEvent event;
    Atom active_window = XInternAtom(display, "_NET_ACTIVE_WINDOW", False);

    memset(&event, 0, sizeof(event));
    event.xclient.type = ClientMessage;
    event.xclient.display = display;
    event.xclient.window = client;
    event.xclient.message_type = active_window;
    event.xclient.format = 32;
    event.xclient.data.l[0] = 1L;
    event.xclient.data.l[1] = CurrentTime;
    if (!XSendEvent(display, root, False,
                    SubstructureRedirectMask | SubstructureNotifyMask,
                    &event))
        return -1;
    XFlush(display);
    return wait_for_active_client(client);
}

static int verify_map_preserves_launcher(void)
{
    Window applications_icon;
    Window launcher;
    Window client;
    Window stale_focus;
    unsigned long black = BlackPixel(display, DefaultScreen(display));
    unsigned long white = WhitePixel(display, DefaultScreen(display));

    stale_focus = XCreateSimpleWindow(display, root, 540, 260, 180, 100, 0,
                                      black, white);
    XStoreName(display, stale_focus, "Win31 X Stale Focus Probe");
    XMapWindow(display, stale_focus);
    XFlush(display);
    if (wait_for_state(stale_focus, NormalState) < 0) {
        fprintf(stderr, "wm-probe: stale-focus client was not managed\n");
        return -1;
    }
    applications_icon = wait_for_role("applications-icon", None, 1);
    if (applications_icon == None) {
        fprintf(stderr, "wm-probe: Applications icon was unavailable\n");
        return -1;
    }
    /* Queue the real launcher click before a real FocusIn while the WM cannot
     * run. That FocusIn is stale once the click focuses the launcher. */
    XGrabServer(display);
    XSync(display, False);
    if (fake_click_at(30, 30) < 0) {
        XUngrabServer(display);
        XFlush(display);
        return -1;
    }
    XSetInputFocus(display, stale_focus, RevertToPointerRoot, CurrentTime);
    XUngrabServer(display);
    XFlush(display);
    launcher = wait_for_role("applications-window", None, 1);
    if (launcher == None) {
        fprintf(stderr, "wm-probe: Applications window did not open for map test\n");
        return -1;
    }
    client = XCreateSimpleWindow(display, root, 600, 300, 220, 120, 0,
                                 black, white);
    XStoreName(display, client, "Win31 X Launcher Map Probe");
    XMapWindow(display, client);
    XFlush(display);
    if (wait_for_state(client, NormalState) < 0 ||
        wait_for_active_client(client) < 0 ||
        !internal_window_remains_viewable(launcher)) {
        fprintf(stderr,
                "wm-probe: mapping/focusing a client dismissed Applications\n");
        return -1;
    }
    XDestroyWindow(display, client);
    XDestroyWindow(display, stale_focus);
    XSync(display, False);
    return 0;
}

static int verify_preexisting_client(void)
{
    Atom marker = XInternAtom(display, "_WIN31X_PREEXISTING_CLIENT", False);
    Window window = root_window_property(marker);
    Window root_return;
    Window parent;
    Window *children = NULL;
    unsigned int count;

    if (window == None) {
        fprintf(stderr, "wm-probe: pre-existing test client was not published\n");
        return -1;
    }
    if (wait_for_state(window, NormalState) < 0) {
        fprintf(stderr, "wm-probe: pre-existing client was not adopted\n");
        return -1;
    }
    if (!XQueryTree(display, window, &root_return, &parent, &children, &count) ||
        parent == root || !role_matches(parent, "client-frame")) {
        if (children != NULL)
            XFree(children);
        fprintf(stderr, "wm-probe: pre-existing client has no Win31 X frame\n");
        return -1;
    }
    if (children != NULL)
        XFree(children);
    return 0;
}

static void send_escape(Window window)
{
    XEvent event;

    memset(&event, 0, sizeof(event));
    event.xkey.type = KeyPress;
    event.xkey.display = display;
    event.xkey.window = window;
    event.xkey.root = root;
    event.xkey.keycode = XKeysymToKeycode(display, XK_Escape);
    event.xkey.same_screen = True;
    XSendEvent(display, window, False, KeyPressMask, &event);
    XFlush(display);
}

static void send_key_sym_with_state(Window window, KeySym symbol,
                                    unsigned int state)
{
    XEvent event;

    memset(&event, 0, sizeof(event));
    event.xkey.type = KeyPress;
    event.xkey.display = display;
    event.xkey.window = window;
    event.xkey.root = root;
    event.xkey.keycode = XKeysymToKeycode(display, symbol);
    event.xkey.state = state;
    event.xkey.same_screen = True;
    XSendEvent(display, window, False, KeyPressMask, &event);
    XFlush(display);
}

static void send_key_sym(Window window, KeySym symbol)
{
    send_key_sym_with_state(window, symbol, 0U);
}

static void send_text(Window window, const char *text)
{
    const unsigned char *cursor = (const unsigned char *)text;

    while (*cursor != '\0') {
        send_key_sym(window, (KeySym)*cursor);
        ++cursor;
    }
}

static int wait_for_focus_window(Window expected)
{
    int attempt;

    for (attempt = 0; attempt < 150; ++attempt) {
        Window focused = None;
        int revert_to;

        XSync(display, False);
        XGetInputFocus(display, &focused, &revert_to);
        if (focused == expected)
            return 0;
        wait_a_bit();
    }
    return -1;
}

static int verify_run_dialog(const char *marker, Window client,
                             Window launcher, Window panel)
{
    Window run;
    int toggle_result;

    if (marker == NULL || marker[0] == '\0')
        return -1;
    XSelectInput(display, client, KeyPressMask);
    unlink(marker);
    if (request_active_client(client) < 0) {
        fprintf(stderr,
                "wm-probe: client did not receive focus before Super+R\n");
        return -1;
    }
    if (fake_key_chord(XK_Super_L, XK_r) < 0) {
        fprintf(stderr, "wm-probe: could not inject Super+R\n");
        return -1;
    }
    run = wait_for_role("run-window", None, 1);
    if (run == None || wait_for_focus_window(run) < 0 ||
        !internal_window_remains_viewable(launcher) ||
        !internal_window_remains_viewable(panel)) {
        fprintf(stderr,
                "wm-probe: Super+R did not open a focused Run window without closing internal windows\n");
        return -1;
    }
    send_text(run, "x");
    send_key_sym(run, XK_BackSpace);
    send_text(run, "/usr/bin/touch ");
    send_text(run, marker);
    send_key_sym(run, XK_Return);
    if (wait_for_file(marker) < 0 || !internal_window_is_hidden(run) ||
        wait_for_active_client(client) < 0 ||
        !internal_window_remains_viewable(launcher) ||
        !internal_window_remains_viewable(panel)) {
        fprintf(stderr,
                "wm-probe: Run did not execute and close independently\n");
        return -1;
    }

    toggle_result = fake_toggle_key(XK_Caps_Lock);
    if (toggle_result < 0 || request_active_client(client) < 0 ||
        fake_key_chord(XK_Super_L, XK_r) < 0 ||
        wait_for_role("run-window", None, 1) == None) {
        fprintf(stderr, "wm-probe: Super+R failed with Caps Lock active\n");
        return -1;
    }
    send_escape(run);
    if (!internal_window_is_hidden(run) || wait_for_active_client(client) < 0)
        return -1;
    if (toggle_result == 0)
        (void)fake_toggle_key(XK_Caps_Lock);

    toggle_result = fake_toggle_key(XK_Num_Lock);
    if (toggle_result < 0 || request_active_client(client) < 0 ||
        fake_key_chord(XK_Super_L, XK_r) < 0 ||
        wait_for_role("run-window", None, 1) == None) {
        fprintf(stderr, "wm-probe: Super+R failed with Num Lock active\n");
        return -1;
    }
    send_escape(run);
    if (!internal_window_is_hidden(run) || wait_for_active_client(client) < 0)
        return -1;
    if (toggle_result == 0)
        (void)fake_toggle_key(XK_Num_Lock);

    if (modifier_mask_for_keysym(XK_Scroll_Lock) != 0U) {
        toggle_result = fake_toggle_key(XK_Scroll_Lock);
        if (toggle_result != 0 || request_active_client(client) < 0 ||
            fake_key_chord(XK_Super_L, XK_r) < 0 ||
            wait_for_role("run-window", None, 1) == None) {
            fprintf(stderr,
                    "wm-probe: Super+R failed with Scroll Lock active\n");
            return -1;
        }
        send_escape(run);
        if (!internal_window_is_hidden(run) ||
            wait_for_active_client(client) < 0)
            return -1;
        (void)fake_toggle_key(XK_Scroll_Lock);
    }

    send_button_one_at(panel, 10, 30);
    if (wait_for_focus_window(panel) < 0 ||
        fake_key_chord(XK_Super_L, XK_r) < 0 ||
        wait_for_focus_window(run) < 0) {
        fprintf(stderr, "wm-probe: Run did not open from Control Panel\n");
        return -1;
    }
    send_escape(run);
    if (!internal_window_is_hidden(run) || wait_for_focus_window(panel) < 0) {
        fprintf(stderr,
                "wm-probe: Run did not restore Control Panel focus\n");
        return -1;
    }
    return 0;
}

static int focus_window_remains(Window expected)
{
    int attempt;

    for (attempt = 0; attempt < 30; ++attempt) {
        Window focused = None;
        int revert_to;

        XSync(display, False);
        XGetInputFocus(display, &focused, &revert_to);
        if (focused != expected)
            return -1;
        wait_a_bit();
    }
    return 0;
}

static int wait_for_external_active_focus(Window launcher, Window panel)
{
    Atom active_window = XInternAtom(display, "_NET_ACTIVE_WINDOW", False);
    int attempt;

    for (attempt = 0; attempt < 150; ++attempt) {
        Window focused = None;
        Window active;
        int revert_to;

        XSync(display, False);
        XGetInputFocus(display, &focused, &revert_to);
        active = root_window_property(active_window);
        if (active != None && focused == active && focused != launcher &&
            focused != panel && focused != root)
            return 0;
        wait_a_bit();
    }
    return -1;
}

static int wait_for_non_input_focus_with_active(Window client,
                                                Window launcher, Window panel)
{
    Atom active_window = XInternAtom(display, "_NET_ACTIVE_WINDOW", False);
    int attempt;

    for (attempt = 0; attempt < 150; ++attempt) {
        Window focused = None;
        int revert_to;

        XSync(display, False);
        XGetInputFocus(display, &focused, &revert_to);
        if (root_window_property(active_window) == client &&
            focused != client && focused != launcher && focused != panel)
            return 0;
        wait_a_bit();
    }
    return -1;
}

static int verify_no_input_client_focus(Window applications_icon,
                                        Window launcher, Window panel)
{
    int screen = DefaultScreen(display);
    int screen_width = DisplayWidth(display, screen);
    int client_x = screen_width > 150 ? screen_width - 150 : 0;
    Window client;
    Window frame;
    FrameGeometry geometry;
    XWMHints hints;
    int click_x;
    int click_y;
    unsigned long black = BlackPixel(display, screen);
    unsigned long white = WhitePixel(display, screen);

    client = XCreateSimpleWindow(display, root, client_x, 32, 120, 72, 0,
                                 black, white);
    XStoreName(display, client, "Win31 X No Input Focus Probe");
    memset(&hints, 0, sizeof(hints));
    hints.flags = InputHint;
    hints.input = False;
    XSetWMHints(display, client, &hints);
    XMapWindow(display, client);
    XFlush(display);
    if (wait_for_state(client, NormalState) < 0) {
        fprintf(stderr, "wm-probe: no-input client was not managed\n");
        return -1;
    }
    frame = client_frame(client);
    if (frame == None || get_frame_geometry(frame, &geometry) < 0) {
        fprintf(stderr, "wm-probe: no-input client frame was unavailable\n");
        return -1;
    }
    click_x = geometry.x + geometry.width / 2;
    click_y = geometry.y + geometry.height / 2;

    send_button_one(applications_icon);
    if (wait_for_focus_window(launcher) < 0) {
        fprintf(stderr,
                "wm-probe: Applications did not refocus before no-input click\n");
        return -1;
    }
    if (fake_click_at(click_x, click_y) < 0) {
        fprintf(stderr, "wm-probe: could not inject no-input client click\n");
        return -1;
    }
    if (wait_for_non_input_focus_with_active(client, launcher, panel) < 0 ||
        !internal_window_remains_viewable(launcher) ||
        !internal_window_remains_viewable(panel)) {
        Atom active_window =
            XInternAtom(display, "_NET_ACTIVE_WINDOW", False);
        Window focused = None;
        int revert_to;

        XGetInputFocus(display, &focused, &revert_to);
        fprintf(stderr,
                "wm-probe: no-input client click violated focus persistence "
                "(focus=0x%lx active=0x%lx client=0x%lx frame=0x%lx "
                "launcher=0x%lx panel=0x%lx click=%d,%d)\n",
                (unsigned long)focused,
                (unsigned long)root_window_property(active_window),
                (unsigned long)client, (unsigned long)frame,
                (unsigned long)launcher, (unsigned long)panel,
                click_x, click_y);
        return -1;
    }

    XDestroyWindow(display, client);
    XFlush(display);
    wait_a_bit();
    return 0;
}

static int verify_internal_focus_state_regressions(
    Window client, Window applications_icon, Window launcher, Window panel)
{
    Atom change_state = XInternAtom(display, "WM_CHANGE_STATE", False);
    Window minimized_icon;

    send_button_one(applications_icon);
    if (wait_for_focus_window(launcher) < 0 ||
        window_state(client) != NormalState ||
        window_atom_property_contains(client, net_wm_state_atom, hidden_atom)) {
        fprintf(stderr,
                "wm-probe: no-op hidden-state preconditions were not met\n");
        return -1;
    }
    send_net_wm_state(client, TEST_NET_WM_STATE_REMOVE, hidden_atom, None);
    if (focus_window_remains(launcher) < 0 ||
        window_state(client) != NormalState ||
        window_atom_property_contains(client, net_wm_state_atom, hidden_atom) ||
        !internal_window_remains_viewable(launcher) ||
        !internal_window_remains_viewable(panel)) {
        fprintf(stderr,
                "wm-probe: removing absent hidden state stole internal focus\n");
        return -1;
    }

    if (fake_click_at(150, 140) < 0 || wait_for_active_client(client) < 0) {
        fprintf(stderr,
                "wm-probe: could not activate client for background minimize test\n");
        return -1;
    }
    send_button_one(applications_icon);
    if (wait_for_focus_window(launcher) < 0) {
        fprintf(stderr,
                "wm-probe: Applications did not focus before background minimize\n");
        return -1;
    }
    send_iconify(client, change_state);
    if (wait_for_state(client, IconicState) < 0 ||
        focus_window_remains(launcher) < 0 ||
        !internal_window_remains_viewable(launcher) ||
        !internal_window_remains_viewable(panel)) {
        fprintf(stderr,
                "wm-probe: minimizing the background active client stole internal focus\n");
        return -1;
    }
    minimized_icon = wait_for_role("minimized-icon", client, 1);
    if (minimized_icon == None) {
        fprintf(stderr,
                "wm-probe: background minimized client did not create an icon\n");
        return -1;
    }
    send_button_one(minimized_icon);
    if (wait_for_state(client, NormalState) < 0 ||
        wait_for_active_client(client) < 0 ||
        !internal_window_remains_viewable(launcher) ||
        !internal_window_remains_viewable(panel)) {
        fprintf(stderr,
                "wm-probe: background minimized client did not restore cleanly\n");
        return -1;
    }

    send_button_one(applications_icon);
    if (wait_for_focus_window(launcher) < 0) {
        fprintf(stderr, "wm-probe: Applications did not refocus for Alt+Tab\n");
        return -1;
    }
    send_key_sym_with_state(launcher, XK_Tab, Mod1Mask);
    if (wait_for_external_active_focus(launcher, panel) < 0 ||
        !internal_window_remains_viewable(launcher) ||
        !internal_window_remains_viewable(panel)) {
        fprintf(stderr,
                "wm-probe: Applications Alt+Tab did not focus a client while staying open\n");
        return -1;
    }

    if (verify_no_input_client_focus(applications_icon, launcher, panel) < 0)
        return -1;
    return 0;
}

static int verify_internal_window_layouts(Window applications_icon,
                                          Window control_icon,
                                          Window launcher, Window panel)
{
    int screen = DefaultScreen(display);
    int screen_width = DisplayWidth(display, screen);
    int screen_height = DisplayHeight(display, screen);
    FrameGeometry launcher_original;
    FrameGeometry panel_original;
    FrameGeometry maximized = {0, 0, screen_width, screen_height};
    FrameGeometry left = {0, 0, screen_width / 2, screen_height};
    FrameGeometry right = {
        screen_width / 2,
        0,
        screen_width - screen_width / 2,
        screen_height
    };

    send_button_one(applications_icon);
    if (wait_for_focus_window(launcher) < 0 ||
        verify_internal_outline_drag(launcher, "Applications",
                                     40, -60, 80, -30) < 0) {
        fprintf(stderr,
                "wm-probe: Applications outline drag verification failed\n");
        return -1;
    }
    send_button_one(control_icon);
    if (wait_for_focus_window(panel) < 0 ||
        verify_internal_outline_drag(panel, "Control Panel",
                                     -40, 60, -80, 30) < 0) {
        fprintf(stderr,
                "wm-probe: Control Panel outline drag verification failed\n");
        return -1;
    }
    if (get_frame_geometry(launcher, &launcher_original) < 0 ||
        get_frame_geometry(panel, &panel_original) < 0) {
        fprintf(stderr, "wm-probe: internal window geometry was unavailable\n");
        return -1;
    }

    send_button_one(applications_icon);
    if (wait_for_focus_window(launcher) < 0 ||
        click_internal_maximize_button(launcher) < 0 ||
        wait_for_frame_geometry(launcher, &maximized) < 0 ||
        click_internal_maximize_button(launcher) < 0 ||
        wait_for_frame_geometry(launcher, &launcher_original) < 0) {
        report_frame_geometry(
            "Applications maximize toggle did not restore exact geometry",
            launcher);
        return -1;
    }

    send_button_one(control_icon);
    if (wait_for_focus_window(panel) < 0 ||
        click_internal_maximize_button(panel) < 0 ||
        wait_for_frame_geometry(panel, &maximized) < 0 ||
        click_internal_maximize_button(panel) < 0 ||
        wait_for_frame_geometry(panel, &panel_original) < 0) {
        report_frame_geometry(
            "Control Panel maximize toggle did not restore exact geometry",
            panel);
        return -1;
    }

    send_button_one(applications_icon);
    if (wait_for_focus_window(launcher) < 0 ||
        drag_frame_title_to(launcher, 0, screen_height / 3) < 0 ||
        wait_for_frame_geometry(launcher, &left) < 0) {
        report_frame_geometry("Applications did not snap left", launcher);
        return -1;
    }
    if (click_internal_maximize_button(launcher) < 0 ||
        wait_for_frame_geometry(launcher, &maximized) < 0 ||
        click_internal_maximize_button(launcher) < 0 ||
        wait_for_frame_geometry(launcher, &left) < 0) {
        report_frame_geometry(
            "maximizing snapped Applications did not restore its prior snap",
            launcher);
        return -1;
    }
    send_button_one(control_icon);
    if (wait_for_focus_window(panel) < 0 ||
        drag_frame_title_to(panel, screen_width - 1,
                            screen_height / 3) < 0 ||
        wait_for_frame_geometry(panel, &right) < 0) {
        report_frame_geometry("Control Panel did not snap right", panel);
        return -1;
    }
    if (click_internal_maximize_button(panel) < 0 ||
        wait_for_frame_geometry(panel, &maximized) < 0 ||
        click_internal_maximize_button(panel) < 0 ||
        wait_for_frame_geometry(panel, &right) < 0) {
        report_frame_geometry(
            "maximizing snapped Control Panel did not restore its prior snap",
            panel);
        return -1;
    }

    if (drag_internal_back_to_geometry(panel, &panel_original) < 0) {
        report_frame_geometry(
            "dragging Control Panel out of snap did not restore it", panel);
        return -1;
    }
    send_button_one(applications_icon);
    if (wait_for_focus_window(launcher) < 0 ||
        drag_internal_back_to_geometry(launcher, &launcher_original) < 0) {
        report_frame_geometry(
            "dragging Applications out of snap did not restore it", launcher);
        return -1;
    }
    if (!internal_window_remains_viewable(launcher) ||
        !internal_window_remains_viewable(panel)) {
        fprintf(stderr,
                "wm-probe: internal layout operations closed a window\n");
        return -1;
    }
    return 0;
}

static unsigned long root_pixel_at(int x, int y)
{
    XImage *image = XGetImage(display, root, x, y, 1U, 1U, AllPlanes,
                              ZPixmap);
    unsigned long pixel = 0;

    if (image != NULL) {
        pixel = XGetPixel(image, 0, 0);
        XDestroyImage(image);
    }
    return pixel;
}

static unsigned long window_pixel_at(Window window, int x, int y)
{
    XImage *image = XGetImage(display, window, x, y, 1U, 1U, AllPlanes,
                              ZPixmap);
    unsigned long pixel = 0UL;

    if (image != NULL) {
        pixel = XGetPixel(image, 0, 0);
        XDestroyImage(image);
    }
    return pixel;
}

static unsigned long expected_rgb_pixel(unsigned char red,
                                        unsigned char green,
                                        unsigned char blue)
{
    XColor color;

    memset(&color, 0, sizeof(color));
    color.red = (unsigned short)((unsigned int)red * 257U);
    color.green = (unsigned short)((unsigned int)green * 257U);
    color.blue = (unsigned short)((unsigned int)blue * 257U);
    color.flags = DoRed | DoGreen | DoBlue;
    if (!XAllocColor(display, DefaultColormap(display, DefaultScreen(display)),
                     &color))
        return 0UL;
    return color.pixel;
}

static int wait_for_window_pixel(Window window, int x, int y,
                                 unsigned long expected)
{
    int attempt;

    for (attempt = 0; attempt < 150; ++attempt) {
        XSync(display, False);
        if (window_pixel_at(window, x, y) == expected)
            return 0;
        wait_a_bit();
    }
    return -1;
}

static int wait_for_root_pixel_change(int x, int y, unsigned long previous)
{
    int attempt;

    for (attempt = 0; attempt < 150; ++attempt) {
        XSync(display, False);
        if (root_pixel_at(x, y) != previous)
            return 0;
        wait_a_bit();
    }
    return -1;
}

static int wait_until_unmanaged(Window window)
{
    int attempt;

    for (attempt = 0; attempt < 150; ++attempt) {
        Window root_return;
        Window parent_return;
        Window *children = NULL;
        unsigned int count;
        if (window_state(window) == WithdrawnState &&
            XQueryTree(display, window, &root_return, &parent_return,
                       &children, &count) && parent_return == root) {
            if (children != NULL)
                XFree(children);
            return 0;
        }
        if (children != NULL)
            XFree(children);
        wait_a_bit();
    }
    return -1;
}

static int publish_test_window_icon(Window window)
{
    Atom property = XInternAtom(display, "_NET_WM_ICON", False);
    const unsigned int width = 48U;
    const unsigned int height = 48U;
    const size_t count = 2U + (size_t)width * height;
    unsigned long *values = calloc(count, sizeof(*values));
    size_t index;

    if (values == NULL)
        return -1;
    values[0] = width;
    values[1] = height;
    for (index = 2U; index < count; ++index)
        values[index] = 0xff12ab34UL;
    XChangeProperty(display, window, property, XA_CARDINAL, 32,
                    PropModeReplace, (unsigned char *)values, (int)count);
    free(values);
    return 0;
}

static int verify_task_manager_delete_repeat(Window manager,
                                             Atom selected_pid_atom)
{
    pid_t child;
    pid_t waited = 0;
    int wait_status = 0;
    int attempt;
    int result = -1;

    child = fork();
    if (child < 0) {
        fprintf(stderr,
                "wm-probe: could not create End Process safety probe\n");
        return -1;
    }
    if (child == 0) {
        for (;;)
            pause();
    }

    /* Refresh after the child exists, then navigate by the published stable
     * PID identity rather than assuming it is the final row. */
    if (fake_key(XK_F5) < 0 || fake_key(XK_End) < 0)
        goto done;
    for (attempt = 0; attempt < 500; ++attempt) {
        unsigned long selected = 0U;

        if (!window_cardinal_property(manager, selected_pid_atom, &selected))
            goto done;
        if (selected == (unsigned long)child)
            break;
        if (fake_key(selected > (unsigned long)child ? XK_Up : XK_Down) < 0)
            goto done;
    }
    if (attempt == 500) {
        fprintf(stderr,
                "wm-probe: End Process safety child was absent from the process list\n");
        goto done;
    }

    /* Arm once, then inject the exact release/press pair used by legacy Xorg
     * auto-repeat.  It must not count as a genuine release or confirmation. */
    if (fake_key_state(XK_Delete, True) < 0 ||
        fake_legacy_auto_repeat_pair(XK_Delete) < 0)
        goto done;
    for (attempt = 0; attempt < 20; ++attempt) {
        waited = waitpid(child, &wait_status, WNOHANG);
        if (waited != 0)
            break;
        wait_a_bit();
    }
    if (fake_key_state(XK_Delete, False) < 0)
        goto done;
    if (waited == child) {
        fprintf(stderr,
                "wm-probe: repeated Delete KeyPress confirmed End Process without a genuine release\n");
        child = -1;
        goto done;
    }
    if (waited < 0) {
        fprintf(stderr,
                "wm-probe: could not inspect End Process safety child\n");
        child = -1;
        goto done;
    }

    /* A genuine second key activation after release must still send SIGTERM;
     * this prevents the repeat guard from making End Process inert. */
    if (fake_key(XK_Delete) < 0)
        goto done;
    for (attempt = 0; attempt < 100; ++attempt) {
        waited = waitpid(child, &wait_status, WNOHANG);
        if (waited != 0)
            break;
        wait_a_bit();
    }
    if (waited != child || !WIFSIGNALED(wait_status) ||
        WTERMSIG(wait_status) != SIGTERM) {
        fprintf(stderr,
                "wm-probe: genuine End Process confirmation did not send SIGTERM\n");
        if (waited == child)
            child = -1;
        goto done;
    }
    child = -1;
    result = 0;

done:
    if (child > 0) {
        pid_t cleanup_wait;

        (void)kill(child, SIGKILL);
        do {
            cleanup_wait = waitpid(child, &wait_status, 0);
        } while (cleanup_wait < 0 && errno == EINTR);
    }
    /* F5 must clear the warning armed by the first Delete before later test
     * actions can interact with this Task Manager instance. */
    if (fake_key(XK_F5) < 0)
        result = -1;
    return result;
}

static int verify_task_manager(Window client)
{
    static const char role[] = "task-manager-window";
    Atom tab_atom =
        XInternAtom(display, "_WIN31X_TASK_MANAGER_TAB", False);
    Atom sample_atom =
        XInternAtom(display, "_WIN31X_TASK_MANAGER_SAMPLE_SERIAL", False);
    Atom cpu_atom =
        XInternAtom(display, "_WIN31X_TASK_MANAGER_CPU_TENTHS", False);
    Atom memory_atom =
        XInternAtom(display, "_WIN31X_TASK_MANAGER_MEMORY_TENTHS", False);
    Atom process_count_atom =
        XInternAtom(display, "_WIN31X_TASK_MANAGER_PROCESS_COUNT", False);
    Atom selected_pid_atom =
        XInternAtom(display, "_WIN31X_TASK_MANAGER_SELECTED_PID", False);
    Window frame = client_frame(client);
    Window manager;
    Window reopened;
    Window transient_owner;
    Window transient_dialog;
    Window launcher;
    unsigned long sample = 0;
    unsigned long updated_sample = 0;
    unsigned long cpu = 0;
    unsigned long memory = 0;
    unsigned long process_count = 0;
    unsigned long selected_pid = 0;
    int toggle_result;
    unsigned long black = BlackPixel(display, DefaultScreen(display));
    unsigned long white = WhitePixel(display, DefaultScreen(display));

    if (frame == None || request_active_client(client) < 0) {
        fprintf(stderr,
                "wm-probe: could not prepare Task Manager focus test\n");
        return -1;
    }
    transient_owner = XCreateSimpleWindow(display, root, 360, 120, 260, 150,
                                          0, black, white);
    XStoreName(display, transient_owner, "Task Manager Family Owner");
    XMapWindow(display, transient_owner);
    XFlush(display);
    if (wait_for_state(transient_owner, NormalState) < 0) {
        fprintf(stderr,
                "wm-probe: Task Manager transient owner was not managed\n");
        return -1;
    }
    transient_dialog = XCreateSimpleWindow(display, root, 410, 155, 180, 90,
                                           0, black, white);
    XStoreName(display, transient_dialog, "Task Manager Family Dialog");
    XSetTransientForHint(display, transient_dialog, transient_owner);
    XMapWindow(display, transient_dialog);
    XFlush(display);
    if (wait_for_state(transient_dialog, NormalState) < 0 ||
        fake_three_key_chord(XK_Control_L, XK_Shift_L, XK_Escape) < 0) {
        fprintf(stderr,
                "wm-probe: could not prepare or invoke Task Manager with Ctrl+Shift+Escape\n");
        return -1;
    }
    manager = wait_for_role(role, None, 1);
    if (manager == None || wait_for_focus_window(manager) < 0 ||
        wait_for_above(manager, frame) < 0 ||
        count_windows_with_role(role) != 1U) {
        fprintf(stderr,
                "wm-probe: Ctrl+Shift+Escape did not open one focused Task Manager above the active client\n");
        return -1;
    }
    if (wait_for_window_name(manager, "Windows 98 Task Manager") < 0 ||
        wait_for_cardinal_value(manager, tab_atom, 0U) < 0 ||
        wait_for_cardinal_change(manager, sample_atom, 0U, &sample) < 0 ||
        !window_cardinal_property(manager, process_count_atom,
                                  &process_count) ||
        !window_cardinal_property(manager, cpu_atom, &cpu) ||
        !window_cardinal_property(manager, memory_atom, &memory) ||
        process_count == 0U || cpu > 1000U || memory > 1000U) {
        fprintf(stderr,
                "wm-probe: Task Manager did not publish a valid initial system sample\n");
        return -1;
    }
    if (wait_for_cardinal_change(manager, sample_atom, sample,
                                 &updated_sample) < 0) {
        fprintf(stderr,
                "wm-probe: Task Manager did not refresh its system sample while open\n");
        return -1;
    }

    if (fake_key_chord(XK_Control_L, XK_Tab) < 0 ||
        wait_for_cardinal_value(manager, tab_atom, 1U) < 0 ||
        !window_cardinal_property(manager, selected_pid_atom,
                                  &selected_pid) ||
        selected_pid == 0U ||
        verify_task_manager_delete_repeat(manager, selected_pid_atom) < 0 ||
        fake_key_chord(XK_Control_L, XK_Tab) < 0 ||
        wait_for_cardinal_value(manager, tab_atom, 2U) < 0 ||
        fake_key_chord(XK_Control_L, XK_Tab) < 0 ||
        wait_for_cardinal_value(manager, tab_atom, 3U) < 0 ||
        fake_key_chord(XK_Control_L, XK_Tab) < 0 ||
        wait_for_cardinal_value(manager, tab_atom, 0U) < 0) {
        fprintf(stderr,
                "wm-probe: Task Manager tabs or process selection did not respond to Ctrl+Tab\n");
        return -1;
    }

    if (fake_key(XK_Return) < 0 ||
        wait_for_active_client(transient_dialog) < 0 ||
        !internal_window_remains_viewable(manager)) {
        fprintf(stderr,
                "wm-probe: Task Manager Applications page focused behind an active transient dialog\n");
        return -1;
    }
    if (fake_key_chord(XK_Alt_L, XK_F2) < 0 ||
        (launcher = wait_for_role("applications-window", None, 1)) == None ||
        wait_for_focus_window(launcher) < 0 ||
        click_internal_close_button(launcher) < 0 ||
        wait_for_active_client(transient_dialog) < 0 ||
        !internal_window_remains_viewable(manager)) {
        fprintf(stderr,
                "wm-probe: closing another internal window raised a background Task Manager over the active app\n");
        return -1;
    }
    if (fake_three_key_chord(XK_Control_L, XK_Shift_L, XK_Escape) < 0 ||
        wait_for_focus_window(manager) < 0 || wait_for_above(manager, frame) < 0 ||
        find_role(role, None, 1) != manager ||
        count_windows_with_role(role) != 1U) {
        fprintf(stderr,
                "wm-probe: invoking an open Task Manager did not refocus its singleton window\n");
        return -1;
    }

    if (click_internal_close_button(manager) < 0 ||
        !internal_window_is_hidden(manager) ||
        count_windows_with_role(role) != 1U ||
        wait_for_active_client(transient_dialog) < 0) {
        fprintf(stderr,
                "wm-probe: Task Manager close button did not hide only its singleton window\n");
        return -1;
    }

    XDestroyWindow(display, transient_dialog);
    XDestroyWindow(display, transient_owner);
    XSync(display, False);
    if (request_active_client(client) < 0) {
        fprintf(stderr,
                "wm-probe: could not restore the primary client after Task Manager family test\n");
        return -1;
    }

    toggle_result = fake_toggle_key(XK_Caps_Lock);
    if (toggle_result < 0 ||
        fake_three_key_chord(XK_Control_L, XK_Shift_L, XK_Escape) < 0 ||
        (reopened = wait_for_role(role, None, 1)) == None ||
        reopened != manager || wait_for_focus_window(reopened) < 0 ||
        count_windows_with_role(role) != 1U) {
        fprintf(stderr,
                "wm-probe: Task Manager did not reopen as one window with Caps Lock active\n");
        return -1;
    }
    if (toggle_result == 0)
        (void)fake_toggle_key(XK_Caps_Lock);
    if (click_internal_close_button(reopened) < 0 ||
        !internal_window_is_hidden(reopened) ||
        wait_for_active_client(client) < 0) {
        fprintf(stderr,
                "wm-probe: reopened Task Manager did not close cleanly\n");
        return -1;
    }
    return 0;
}

static int verify_control_panel(Window client, const char *wifi_marker,
                                const char *lock_marker)
{
    Window control_icon;
    Window applications_icon;
    Window panel;
    Window launcher;
    int sample_x = DisplayWidth(display, DefaultScreen(display)) - 5;
    int sample_y = DisplayHeight(display, DefaultScreen(display)) - 5;
    unsigned long original_pixel;
    int attempt;

    control_icon = wait_for_role("control-panel-icon", None, 1);
    applications_icon = wait_for_role("applications-icon", None, 1);
    if (control_icon == None || applications_icon == None) {
        fprintf(stderr, "wm-probe: Control Panel desktop icon was not found\n");
        return -1;
    }
    send_button_one(control_icon);
    panel = wait_for_role("control-panel-window", None, 1);
    if (panel == None || wait_for_focus_window(panel) < 0) {
        fprintf(stderr, "wm-probe: Control Panel did not open and take focus\n");
        return -1;
    }

    send_button_one(applications_icon);
    launcher = wait_for_role("applications-window", None, 1);
    if (launcher == None || !internal_window_remains_viewable(panel) ||
        !internal_window_remains_viewable(launcher)) {
        fprintf(stderr,
                "wm-probe: Applications and Control Panel did not coexist\n");
        return -1;
    }
    send_escape(launcher);
    if (!internal_window_remains_viewable(launcher) ||
        !internal_window_remains_viewable(panel)) {
        fprintf(stderr,
                "wm-probe: Escape closed an internal window during panel test\n");
        return -1;
    }
    if (verify_internal_focus_state_regressions(
            client, applications_icon, launcher, panel) < 0)
        return -1;
    if (verify_internal_window_layouts(applications_icon, control_icon,
                                       launcher, panel) < 0)
        return -1;

    original_pixel = root_pixel_at(sample_x, sample_y);
    send_button_one_at(panel, 50, 155);
    send_button_one_at(panel, 205, 165);
    if (wait_for_root_pixel_change(sample_x, sample_y, original_pixel) < 0) {
        fprintf(stderr, "wm-probe: Control Panel color scheme did not apply\n");
        return -1;
    }

    send_button_one_at(panel, 50, 245);
    send_button_one_at(panel, 190, 295);
    if (wait_for_file(lock_marker) < 0) {
        fprintf(stderr, "wm-probe: Control Panel Lock Now did not run the locker\n");
        return -1;
    }

    send_button_one_at(panel, 50, 65);
    for (attempt = 0; attempt < 35; ++attempt)
        wait_a_bit();
    send_button_one_at(panel, 205, 104);
    send_button_one_at(panel, 205, 365);
    send_text(panel, "correct horse");
    send_button_one_at(panel, 205, 415);
    if (wait_for_file(wifi_marker) < 0) {
        fprintf(stderr, "wm-probe: Control Panel Wi-Fi connect did not finish\n");
        return -1;
    }

    if (fake_click_at(150, 100) < 0 || wait_for_active_client(client) < 0 ||
        !internal_window_remains_viewable(panel) ||
        !internal_window_remains_viewable(launcher)) {
        fprintf(stderr,
                "wm-probe: focusing a client dismissed an internal window\n");
        return -1;
    }
    send_escape(panel);
    if (!internal_window_remains_viewable(panel) ||
        !internal_window_remains_viewable(launcher)) {
        fprintf(stderr, "wm-probe: Escape closed Control Panel\n");
        return -1;
    }
    return 0;
}

static int focus_and_active_remain(Window expected_focus,
                                   Window expected_active)
{
    Atom active_window = XInternAtom(display, "_NET_ACTIVE_WINDOW", False);
    int attempt;

    for (attempt = 0; attempt < 30; ++attempt) {
        Window focused = None;
        int revert_to;

        XSync(display, False);
        XGetInputFocus(display, &focused, &revert_to);
        if (focused != expected_focus ||
            root_window_property(active_window) != expected_active)
            return -1;
        wait_a_bit();
    }
    return 0;
}

static int wait_for_focus_and_active(Window expected_focus,
                                     Window expected_active)
{
    Atom active_window = XInternAtom(display, "_NET_ACTIVE_WINDOW", False);
    int attempt;

    for (attempt = 0; attempt < 150; ++attempt) {
        Window focused = None;
        int revert_to;

        XSync(display, False);
        XGetInputFocus(display, &focused, &revert_to);
        if (focused == expected_focus &&
            root_window_property(active_window) == expected_active)
            return 0;
        wait_a_bit();
    }
    return -1;
}

static Window open_desktop_menu_near_bottom_right(FrameGeometry *geometry,
                                                   int *request_x,
                                                   int *request_y)
{
    Window menu;
    int x;
    int y;

    if (find_bare_root_near_bottom_right(&x, &y) < 0 ||
        fake_button_at(Button3, x, y) < 0)
        return None;
    menu = wait_for_role("desktop-menu", None, 1);
    if (menu == None || get_frame_geometry(menu, geometry) < 0) {
        fprintf(stderr,
                "wm-probe: bare desktop right-click did not open the desktop menu\n");
        return None;
    }
    if (request_x != NULL)
        *request_x = x;
    if (request_y != NULL)
        *request_y = y;
    return menu;
}

static int desktop_menu_click_row(int row_y)
{
    FrameGeometry geometry;
    Window menu = open_desktop_menu_near_bottom_right(&geometry, NULL, NULL);

    if (menu == None)
        return -1;
    if (fake_click_at(geometry.x + 15, geometry.y + row_y) < 0)
        return -1;
    if (!internal_window_is_hidden(menu)) {
        fprintf(stderr,
                "wm-probe: choosing a desktop-menu item did not dismiss the menu\n");
        return -1;
    }
    return 0;
}

static Window open_session_confirmation(int menu_row_y,
                                        const char *expected_title,
                                        Window expected_active)
{
    int screen = DefaultScreen(display);
    int screen_width = DisplayWidth(display, screen);
    int screen_height = DisplayHeight(display, screen);
    Atom active_window = XInternAtom(display, "_NET_ACTIVE_WINDOW", False);
    FrameGeometry geometry;
    FrameGeometry shield_geometry;
    Window dialog;
    Window shield;

    if (desktop_menu_click_row(menu_row_y) < 0)
        return None;
    dialog = wait_for_role("session-confirmation", None, 1);
    if (dialog == None || wait_for_focus_window(dialog) < 0 ||
        wait_for_window_name(dialog, expected_title) < 0 ||
        get_frame_geometry(dialog, &geometry) < 0) {
        fprintf(stderr,
                "wm-probe: %s confirmation did not open with its role, title, and focus\n",
                expected_title);
        return None;
    }
    if (root_window_property(active_window) != expected_active) {
        fprintf(stderr,
                "wm-probe: %s confirmation changed _NET_ACTIVE_WINDOW\n",
                expected_title);
        return None;
    }
    shield = wait_for_role("session-confirmation-shield", None, 1);
    if (shield == None || get_frame_geometry(shield, &shield_geometry) < 0 ||
        shield_geometry.x != 0 || shield_geometry.y != 0 ||
        shield_geometry.width != screen_width ||
        shield_geometry.height != screen_height ||
        !window_is_above(dialog, shield)) {
        fprintf(stderr,
                "wm-probe: %s confirmation did not have a fullscreen shield below it\n",
                expected_title);
        return None;
    }
    if (geometry.width != 420 || geometry.height != 176 ||
        geometry.x != (screen_width - geometry.width) / 2 ||
        geometry.y != (screen_height - geometry.height) / 2 ||
        geometry.x < 0 || geometry.y < 0 ||
        geometry.x + geometry.width > screen_width ||
        geometry.y + geometry.height > screen_height) {
        fprintf(stderr,
                "wm-probe: %s confirmation geometry was %d,%d %dx%d on %dx%d\n",
                expected_title, geometry.x, geometry.y, geometry.width,
                geometry.height, screen_width, screen_height);
        return None;
    }
    return dialog;
}

static int click_session_confirmation_button(Window dialog, int yes)
{
    FrameGeometry geometry;
    int no_x;
    int yes_x;
    int button_y;

    if (get_frame_geometry(dialog, &geometry) < 0)
        return -1;
    no_x = geometry.x + geometry.width -
           TEST_SESSION_CONFIRM_BUTTON_MARGIN -
           TEST_SESSION_CONFIRM_BUTTON_WIDTH;
    yes_x = no_x - TEST_SESSION_CONFIRM_BUTTON_GAP -
            TEST_SESSION_CONFIRM_BUTTON_WIDTH;
    button_y = geometry.y + geometry.height -
               TEST_SESSION_CONFIRM_BUTTON_MARGIN -
               TEST_SESSION_CONFIRM_BUTTON_HEIGHT;
    return fake_click_at((yes ? yes_x : no_x) +
                             TEST_SESSION_CONFIRM_BUTTON_WIDTH / 2,
                         button_y + TEST_SESSION_CONFIRM_BUTTON_HEIGHT / 2);
}

static int click_session_confirmation_close(Window dialog)
{
    FrameGeometry geometry;

    if (get_frame_geometry(dialog, &geometry) < 0)
        return -1;
    return fake_click_at(geometry.x + geometry.width -
                             TEST_INTERNAL_CLOSE_INSET -
                             TEST_TITLE_BUTTON / 2,
                         geometry.y + TEST_TITLE_BUTTON_Y +
                             TEST_TITLE_BUTTON / 2);
}

static int send_synthetic_session_confirmation_yes_click(Window dialog)
{
    FrameGeometry geometry;
    XEvent event;
    int no_x;
    int yes_x;
    int button_y;

    if (get_frame_geometry(dialog, &geometry) < 0)
        return -1;
    no_x = geometry.width - TEST_SESSION_CONFIRM_BUTTON_MARGIN -
           TEST_SESSION_CONFIRM_BUTTON_WIDTH;
    yes_x = no_x - TEST_SESSION_CONFIRM_BUTTON_GAP -
            TEST_SESSION_CONFIRM_BUTTON_WIDTH;
    button_y = geometry.height - TEST_SESSION_CONFIRM_BUTTON_MARGIN -
               TEST_SESSION_CONFIRM_BUTTON_HEIGHT;
    memset(&event, 0, sizeof(event));
    event.xbutton.type = ButtonPress;
    event.xbutton.display = display;
    event.xbutton.window = dialog;
    event.xbutton.root = root;
    event.xbutton.time = CurrentTime;
    event.xbutton.x = yes_x + TEST_SESSION_CONFIRM_BUTTON_WIDTH / 2;
    event.xbutton.y = button_y + TEST_SESSION_CONFIRM_BUTTON_HEIGHT / 2;
    event.xbutton.x_root = geometry.x + event.xbutton.x;
    event.xbutton.y_root = geometry.y + event.xbutton.y;
    event.xbutton.button = Button1;
    event.xbutton.same_screen = True;
    if (!XSendEvent(display, dialog, False, ButtonPressMask, &event))
        return -1;
    event.xbutton.type = ButtonRelease;
    event.xbutton.state = Button1Mask;
    if (!XSendEvent(display, dialog, False, ButtonReleaseMask, &event))
        return -1;
    XFlush(display);
    return 0;
}

static int session_confirmation_remains_safe(Window dialog,
                                             const char *action_marker,
                                             const char *label)
{
    Window shield;

    if (!internal_window_remains_viewable(dialog) ||
        find_role("session-confirmation", None, 1) != dialog ||
        file_remains_absent(action_marker) < 0 ||
        wait_for_focus_window(dialog) < 0) {
        fprintf(stderr,
                "wm-probe: synthetic %s dismissed or executed the session confirmation\n",
                label);
        return -1;
    }
    shield = find_role("session-confirmation-shield", None, 1);
    if (shield == None || !internal_window_remains_viewable(shield)) {
        fprintf(stderr,
                "wm-probe: synthetic %s removed the session confirmation shield\n",
                label);
        return -1;
    }
    return 0;
}

static int fullscreen_locker_yields_session_confirmation(
    Window dialog, Window expected_active, const char *action_marker)
{
    int screen = DefaultScreen(display);
    int screen_width = DisplayWidth(display, screen);
    int screen_height = DisplayHeight(display, screen);
    XSetWindowAttributes attributes;
    XWindowAttributes actual;
    Window shield = find_role("session-confirmation-shield", None, 1);
    Window locker = None;
    int result = -1;

    if (shield == None) {
        fprintf(stderr,
                "wm-probe: the confirmation shield was unavailable for the locker test\n");
        return -1;
    }
    memset(&attributes, 0, sizeof(attributes));
    attributes.override_redirect = True;
    attributes.background_pixel = BlackPixel(display, screen);
    attributes.event_mask = StructureNotifyMask;
    locker = XCreateWindow(display, root, 0, 0,
                           (unsigned)screen_width,
                           (unsigned)screen_height, 0, CopyFromParent,
                           InputOutput, CopyFromParent,
                           CWOverrideRedirect | CWBackPixel | CWEventMask,
                           &attributes);
    if (locker == None) {
        fprintf(stderr,
                "wm-probe: could not create the fullscreen locker-like window\n");
        return -1;
    }
    XStoreName(display, locker, "Win31 X Fullscreen Locker Probe");
    XMapRaised(display, locker);
    XSetInputFocus(display, locker, RevertToPointerRoot, CurrentTime);
    XFlush(display);

    if (!internal_window_is_hidden(dialog) ||
        !internal_window_is_hidden(shield) ||
        !internal_window_remains_unmapped(dialog) ||
        !internal_window_remains_unmapped(shield) ||
        find_role("session-confirmation", None, 1) != None ||
        find_role("session-confirmation-shield", None, 1) != None) {
        fprintf(stderr,
                "wm-probe: a fullscreen locker did not dismiss both confirmation windows\n");
        goto cleanup;
    }
    if (!XGetWindowAttributes(display, locker, &actual) ||
        actual.map_state != IsViewable || !actual.override_redirect ||
        actual.class != InputOutput || actual.x != 0 || actual.y != 0 ||
        actual.width != screen_width || actual.height != screen_height ||
        !window_is_above(locker, dialog) ||
        !window_is_above(locker, shield)) {
        fprintf(stderr,
                "wm-probe: the fullscreen locker was covered or reconfigured\n");
        goto cleanup;
    }
    if (focus_and_active_remain(locker, expected_active) < 0) {
        fprintf(stderr,
                "wm-probe: the confirmation stole focus back from the fullscreen locker\n");
        goto cleanup;
    }
    if (wait_for_pointer_available() < 0 ||
        wait_for_keyboard_available() < 0 ||
        focus_and_active_remain(locker, expected_active) < 0) {
        fprintf(stderr,
                "wm-probe: dismissing for a fullscreen locker left an input grab or changed focus\n");
        goto cleanup;
    }
    if (file_remains_absent(action_marker) < 0) {
        fprintf(stderr,
                "wm-probe: showing a fullscreen locker executed the pending action\n");
        goto cleanup;
    }
    result = 0;

cleanup:
    XDestroyWindow(display, locker);
    XSync(display, False);
    return result;
}

static int verify_session_confirmation_security(Window expected_active,
                                                const char *action_marker)
{
    Window dialog;

    (void)unlink(action_marker);
    dialog = open_session_confirmation(76, "Confirm Restart",
                                       expected_active);
    if (dialog == None)
        return -1;
    send_key_sym(dialog, XK_y);
    if (session_confirmation_remains_safe(dialog, action_marker,
                                          "XSendEvent Y") < 0)
        return -1;
    send_key_sym(dialog, XK_Return);
    if (session_confirmation_remains_safe(dialog, action_marker,
                                          "XSendEvent Enter") < 0)
        return -1;
    if (send_synthetic_session_confirmation_yes_click(dialog) < 0 ||
        session_confirmation_remains_safe(dialog, action_marker,
                                          "XSendEvent Yes click") < 0)
        return -1;
    return fullscreen_locker_yields_session_confirmation(
        dialog, expected_active, action_marker);
}

static int verify_session_confirmation_dismissed(
    Window dialog, Window expected_focus, Window expected_active,
    const char *label)
{
    if (!internal_window_is_hidden(dialog) ||
        find_role("session-confirmation", None, 1) != None) {
        fprintf(stderr,
                "wm-probe: %s did not dismiss the session confirmation\n",
                label);
        return -1;
    }
    if (wait_for_focus_and_active(expected_focus, expected_active) < 0 ||
        focus_and_active_remain(expected_focus, expected_active) < 0) {
        fprintf(stderr,
                "wm-probe: %s did not restore the previous focus and active window\n",
                label);
        return -1;
    }
    if (wait_for_pointer_available() < 0 ||
        wait_for_keyboard_available() < 0) {
        fprintf(stderr,
                "wm-probe: %s left an input grab active\n", label);
        return -1;
    }
    if (find_role("desktop-menu", None, 1) != None) {
        fprintf(stderr,
                "wm-probe: %s left the desktop menu mapped\n", label);
        return -1;
    }
    return 0;
}

static int wm_check_remains_owned(Atom supporting)
{
    int attempt;

    for (attempt = 0; attempt < 20; ++attempt) {
        XSync(display, False);
        if (root_window_property(supporting) == None)
            return -1;
        wait_a_bit();
    }
    return 0;
}

static int verify_desktop_menu(Window client, Window applications_icon,
                               Window launcher, Window panel,
                               const char *lock_marker,
                               const char *system_action_marker)
{
    int screen = DefaultScreen(display);
    int screen_width = DisplayWidth(display, screen);
    int screen_height = DisplayHeight(display, screen);
    Atom active_window = XInternAtom(display, "_NET_ACTIVE_WINDOW", False);
    FrameGeometry menu_geometry;
    Window menu;
    Window focused = None;
    Window active;
    int revert_to;
    int x;
    int y;
    int request_x;
    int request_y;
    int attempt;
    Atom supporting = XInternAtom(display, "_NET_SUPPORTING_WM_CHECK", False);
    Window confirmation;

    if (lock_marker == NULL || lock_marker[0] == '\0' ||
        system_action_marker == NULL || system_action_marker[0] == '\0') {
        fprintf(stderr,
                "wm-probe: desktop-menu marker paths were not configured\n");
        return -1;
    }
    if (request_active_client(client) < 0 ||
        window_center_on_root(client, &x, &y) < 0 ||
        fake_button_at(Button3, x, y) < 0 ||
        !role_remains_unmapped("desktop-menu")) {
        fprintf(stderr,
                "wm-probe: right-clicking a client opened the desktop menu\n");
        return -1;
    }
    if (window_center_on_root(applications_icon, &x, &y) < 0 ||
        fake_button_at(Button3, x, y) < 0 ||
        !role_remains_unmapped("desktop-menu")) {
        fprintf(stderr,
                "wm-probe: right-clicking a desktop icon opened the desktop menu\n");
        return -1;
    }
    if (request_active_client(client) < 0)
        return -1;
    XGetInputFocus(display, &focused, &revert_to);
    active = root_window_property(active_window);
    if (focused != client || active != client) {
        fprintf(stderr,
                "wm-probe: desktop-menu focus precondition was not met\n");
        return -1;
    }

    menu = open_desktop_menu_near_bottom_right(
        &menu_geometry, &request_x, &request_y);
    if (menu == None)
        return -1;
    if (menu_geometry.x < 0 || menu_geometry.y < 0 ||
        menu_geometry.x + menu_geometry.width > screen_width ||
        menu_geometry.y + menu_geometry.height > screen_height ||
        (menu_geometry.x == request_x && menu_geometry.y == request_y)) {
        fprintf(stderr,
                "wm-probe: bottom-right desktop menu was not clamped onscreen "
                "(request %d,%d actual %d,%d %dx%d screen %dx%d)\n",
                request_x, request_y, menu_geometry.x, menu_geometry.y,
                menu_geometry.width, menu_geometry.height,
                screen_width, screen_height);
        return -1;
    }
    if (focus_and_active_remain(focused, active) < 0) {
        fprintf(stderr,
                "wm-probe: opening the desktop menu changed focus or _NET_ACTIVE_WINDOW\n");
        return -1;
    }
    if (fake_click_at(1, screen_height - 1) < 0 ||
        !internal_window_is_hidden(menu)) {
        fprintf(stderr,
                "wm-probe: an outside click did not dismiss the desktop menu\n");
        return -1;
    }

    (void)unlink(lock_marker);
    if (desktop_menu_click_row(17) < 0 || wait_for_file(lock_marker) < 0) {
        fprintf(stderr,
                "wm-probe: desktop-menu Lock did not invoke the configured locker\n");
        return -1;
    }
    if (!role_remains_unmapped("session-confirmation")) {
        fprintf(stderr,
                "wm-probe: desktop-menu Lock opened a confirmation dialog\n");
        return -1;
    }
    if (!internal_window_remains_viewable(launcher) ||
        !internal_window_remains_viewable(panel)) {
        fprintf(stderr,
                "wm-probe: desktop-menu Lock closed an internal window\n");
        return -1;
    }

    (void)unlink(system_action_marker);
    send_button_one_at(launcher, 10, 30);
    if (wait_for_focus_and_active(launcher, None) < 0) {
        fprintf(stderr,
                "wm-probe: could not establish Applications focus before Log Out confirmation\n");
        return -1;
    }
    confirmation = open_session_confirmation(50, "Confirm Log Out", None);
    if (confirmation == None || file_remains_absent(system_action_marker) < 0 ||
        wm_check_remains_owned(supporting) < 0 ||
        fake_key(XK_Escape) < 0 ||
        verify_session_confirmation_dismissed(
            confirmation, launcher, None, "Escape on Log Out") < 0) {
        fprintf(stderr,
                "wm-probe: Escape did not safely cancel Log Out\n");
        return -1;
    }

    confirmation = open_session_confirmation(50, "Confirm Log Out", None);
    if (confirmation == None || click_session_confirmation_close(confirmation) < 0 ||
        verify_session_confirmation_dismissed(
            confirmation, launcher, None, "close button on Log Out") < 0 ||
        wm_check_remains_owned(supporting) < 0) {
        fprintf(stderr,
                "wm-probe: the close button did not safely cancel Log Out\n");
        return -1;
    }

    confirmation = open_session_confirmation(50, "Confirm Log Out", None);
    if (confirmation == None || fake_key_chord(XK_Alt_L, XK_F4) < 0 ||
        verify_session_confirmation_dismissed(
            confirmation, launcher, None, "Alt+F4 on Log Out") < 0 ||
        wm_check_remains_owned(supporting) < 0) {
        fprintf(stderr,
                "wm-probe: Alt+F4 did not safely cancel Log Out\n");
        return -1;
    }

    confirmation = open_session_confirmation(50, "Confirm Log Out", None);
    if (confirmation == None || fake_key(XK_Return) < 0 ||
        verify_session_confirmation_dismissed(
            confirmation, launcher, None,
            "default Enter on Log Out") < 0 ||
        wm_check_remains_owned(supporting) < 0) {
        fprintf(stderr,
                "wm-probe: the safe default did not cancel Log Out on Enter\n");
        return -1;
    }

    if (request_active_client(client) < 0 ||
        wait_for_focus_and_active(client, client) < 0)
        return -1;
    (void)unlink(system_action_marker);
    confirmation = open_session_confirmation(76, "Confirm Restart", client);
    if (confirmation == None || file_remains_absent(system_action_marker) < 0 ||
        click_session_confirmation_button(confirmation, 0) < 0 ||
        verify_session_confirmation_dismissed(
            confirmation, client, client, "No button on Restart") < 0 ||
        file_remains_absent(system_action_marker) < 0) {
        fprintf(stderr,
                "wm-probe: the No button did not safely cancel Restart\n");
        return -1;
    }

    if (verify_session_confirmation_security(client,
                                             system_action_marker) < 0 ||
        request_active_client(client) < 0 ||
        wait_for_focus_and_active(client, client) < 0) {
        fprintf(stderr,
                "wm-probe: session confirmation security verification failed\n");
        return -1;
    }

    confirmation = open_session_confirmation(76, "Confirm Restart", client);
    if (confirmation == None || file_remains_absent(system_action_marker) < 0 ||
        fake_key(XK_y) < 0 ||
        wait_for_file_contents(system_action_marker, "reboot\n") < 0 ||
        verify_session_confirmation_dismissed(
            confirmation, client, client, "Y on Restart") < 0) {
        fprintf(stderr,
                "wm-probe: Y did not confirm exactly one reboot request\n");
        return -1;
    }
    for (attempt = 0; attempt < 10; ++attempt)
        wait_a_bit();

    (void)unlink(system_action_marker);
    send_button_one_at(panel, 10, 30);
    if (wait_for_focus_and_active(panel, None) < 0) {
        fprintf(stderr,
                "wm-probe: could not establish Control Panel focus before Shut Down confirmation\n");
        return -1;
    }
    confirmation = open_session_confirmation(102, "Confirm Shut Down", None);
    if (confirmation == None || file_remains_absent(system_action_marker) < 0 ||
        fake_key(XK_n) < 0 ||
        verify_session_confirmation_dismissed(
            confirmation, panel, None, "N on Shut Down") < 0 ||
        file_remains_absent(system_action_marker) < 0) {
        fprintf(stderr,
                "wm-probe: N did not safely cancel Shut Down\n");
        return -1;
    }

    confirmation = open_session_confirmation(102, "Confirm Shut Down", None);
    if (confirmation == None || fake_key(XK_Left) < 0 ||
        wait_for_focus_window(confirmation) < 0 || fake_key(XK_Right) < 0 ||
        wait_for_focus_window(confirmation) < 0 || fake_key(XK_Return) < 0 ||
        verify_session_confirmation_dismissed(
            confirmation, panel, None,
            "Left, Right, and Enter on Shut Down") < 0 ||
        file_remains_absent(system_action_marker) < 0) {
        fprintf(stderr,
                "wm-probe: keyboard navigation did not return to No before Enter\n");
        return -1;
    }

    confirmation = open_session_confirmation(102, "Confirm Shut Down", None);
    if (confirmation == None || file_remains_absent(system_action_marker) < 0 ||
        click_session_confirmation_button(confirmation, 1) < 0 ||
        wait_for_file_contents(system_action_marker, "poweroff\n") < 0 ||
        verify_session_confirmation_dismissed(
            confirmation, panel, None, "Yes button on Shut Down") < 0) {
        fprintf(stderr,
                "wm-probe: the Yes button did not confirm exactly one poweroff request\n");
        return -1;
    }
    if (!internal_window_remains_viewable(launcher) ||
        !internal_window_remains_viewable(panel)) {
        fprintf(stderr,
                "wm-probe: desktop-menu actions closed an internal window\n");
        return -1;
    }
    return 0;
}

static int verify_desktop_menu_logout(Atom supporting)
{
    Atom active_window = XInternAtom(display, "_NET_ACTIVE_WINDOW", False);
    Window active = root_window_property(active_window);
    Window confirmation;
    int attempt;

    confirmation = open_session_confirmation(50, "Confirm Log Out", active);
    if (confirmation == None || wm_check_remains_owned(supporting) < 0) {
        fprintf(stderr,
                "wm-probe: selecting Log Out did not wait for confirmation\n");
        return -1;
    }
    if (fake_key(XK_Tab) < 0 || wait_for_focus_window(confirmation) < 0 ||
        fake_key(XK_Return) < 0)
        return -1;
    for (attempt = 0; attempt < 150; ++attempt) {
        XSync(display, False);
        if (root_window_property(supporting) == None &&
            find_role("desktop-menu", None, 1) == None &&
            find_role("session-confirmation", None, 1) == None) {
            if (wait_for_pointer_available() < 0 ||
                wait_for_keyboard_available() < 0) {
                fprintf(stderr,
                        "wm-probe: Log Out left an input grab active\n");
                return -1;
            }
            return 0;
        }
        wait_a_bit();
    }
    fprintf(stderr,
            "wm-probe: confirmed Log Out did not release WM ownership\n");
    return -1;
}

static int verify_multi_monitor_static_icons(Window *applications_icon,
                                             Window *control_icon)
{
    const FrameGeometry expected_applications = {
        12, 12, TEST_DESKTOP_ICON_WIDTH, TEST_DESKTOP_ICON_HEIGHT
    };
    const FrameGeometry expected_control = {
        12, 100, TEST_DESKTOP_ICON_WIDTH, TEST_DESKTOP_ICON_HEIGHT
    };

    *applications_icon = wait_for_role("applications-icon", None, 1);
    *control_icon = wait_for_role("control-panel-icon", None, 1);
    if (*applications_icon == None || *control_icon == None) {
        fprintf(stderr,
                "wm-probe: multi-monitor desktop icons were unavailable\n");
        return -1;
    }
    if (wait_for_frame_geometry(*applications_icon,
                                &expected_applications) < 0) {
        report_frame_geometry(
            "Applications icon was not anchored to the primary monitor",
            *applications_icon);
        return -1;
    }
    if (wait_for_frame_geometry(*control_icon, &expected_control) < 0) {
        report_frame_geometry(
            "Control Panel icon was not anchored to the primary monitor",
            *control_icon);
        return -1;
    }
    return 0;
}

static int verify_multi_monitor_client(Atom change_state, Window *client_out)
{
    const FrameGeometry right_monitor = {800, 100, 800, 600};
    const FrameGeometry maximized = {800, 100, 800, 600};
    const FrameGeometry snapped_left = {800, 100, 400, 600};
    const FrameGeometry snapped_right = {1200, 100, 400, 600};
    const FrameGeometry expected_icon = {
        812, 608, TEST_DESKTOP_ICON_WIDTH, TEST_DESKTOP_ICON_HEIGHT
    };
    int screen = DefaultScreen(display);
    unsigned long black = BlackPixel(display, screen);
    unsigned long white = WhitePixel(display, screen);
    Window client;
    Window frame;
    Window icon;
    FrameGeometry original;
    FrameGeometry restored;
    int icon_x;
    int icon_y;

    client = XCreateSimpleWindow(display, root, 950, 220, 260, 140, 0,
                                 black, white);
    XStoreName(display, client, "Win31 X Multi-Monitor Client");
    XMapWindow(display, client);
    XFlush(display);
    if (wait_for_state(client, NormalState) < 0) {
        fprintf(stderr,
                "wm-probe: multi-monitor client was not managed\n");
        return -1;
    }
    frame = client_frame(client);
    if (frame == None || get_frame_geometry(frame, &original) < 0 ||
        !frame_geometry_inside(&original, &right_monitor)) {
        fprintf(stderr,
                "wm-probe: multi-monitor client did not begin on the right monitor\n");
        return -1;
    }

    if (click_frame_maximize_button(frame) < 0 ||
        wait_for_frame_geometry(frame, &maximized) < 0 ||
        wait_for_maximized_state(client, 1) < 0) {
        report_frame_geometry(
            "client maximize did not use the active right monitor", frame);
        return -1;
    }
    if (click_frame_maximize_button(frame) < 0 ||
        wait_for_frame_geometry(frame, &original) < 0 ||
        wait_for_maximized_state(client, 0) < 0) {
        report_frame_geometry(
            "right-monitor maximize did not restore exact client geometry",
            frame);
        return -1;
    }

    /* Exercise the inter-monitor seam just inside the offset monitor's snap
     * threshold so this cannot be mistaken for the root screen's left edge. */
    if (drag_frame_title_to(frame, 801, 350) < 0 ||
        wait_for_frame_geometry(frame, &snapped_left) < 0 ||
        wait_for_maximized_state(client, 0) < 0) {
        report_frame_geometry(
            "client did not snap left at the inter-monitor seam", frame);
        return -1;
    }
    if (click_frame_maximize_button(frame) < 0 ||
        wait_for_frame_geometry(frame, &maximized) < 0 ||
        click_frame_maximize_button(frame) < 0 ||
        wait_for_frame_geometry(frame, &snapped_left) < 0) {
        report_frame_geometry(
            "maximizing a right-monitor snapped client lost its snap", frame);
        return -1;
    }
    /* Exercise the offset monitor's right edge, not the root-sized layout. */
    if (drag_frame_title_to(frame, 1598, 350) < 0 ||
        wait_for_frame_geometry(frame, &snapped_right) < 0 ||
        wait_for_maximized_state(client, 0) < 0) {
        report_frame_geometry(
            "client did not snap right on the offset monitor", frame);
        return -1;
    }
    if (drag_frame_title_to(frame, 1101, 350) < 0 ||
        wait_for_restored_frame(frame, &original, 0) < 0 ||
        get_frame_geometry(frame, &restored) < 0 ||
        !frame_geometry_inside(&restored, &right_monitor)) {
        report_frame_geometry(
            "dragging out of right-monitor snap did not restore on that monitor",
            frame);
        return -1;
    }

    send_iconify(client, change_state);
    if (wait_for_state(client, IconicState) < 0) {
        fprintf(stderr,
                "wm-probe: right-monitor client did not minimize\n");
        return -1;
    }
    icon = wait_for_role("minimized-icon", client, 1);
    if (icon == None || wait_for_frame_geometry(icon, &expected_icon) < 0) {
        if (icon != None)
            report_frame_geometry(
                "minimized icon was not placed at the right monitor's bottom edge",
                icon);
        else
            fprintf(stderr,
                    "wm-probe: right-monitor minimized icon was unavailable\n");
        return -1;
    }
    if (window_center_on_root(icon, &icon_x, &icon_y) < 0 ||
        fake_click_at(icon_x, icon_y) < 0 ||
        wait_for_state(client, NormalState) < 0 ||
        wait_for_active_client(client) < 0) {
        fprintf(stderr,
                "wm-probe: right-monitor minimized icon did not restore its client\n");
        return -1;
    }
    *client_out = client;
    return 0;
}

static int verify_multi_monitor_internal_windows(Window client,
                                                 Window applications_icon,
                                                 Window control_icon)
{
    const FrameGeometry right_monitor = {800, 100, 800, 600};
    const FrameGeometry applications_normal = {860, 170, 680, 460};
    const FrameGeometry control_normal = {60, 70, 680, 460};
    const FrameGeometry task_manager_normal = {840, 140, 720, 520};
    const FrameGeometry maximized = {800, 100, 800, 600};
    const FrameGeometry snapped_left = {800, 100, 400, 600};
    Window launcher;
    Window panel;
    Window task_manager;
    FrameGeometry moved_panel;
    int icon_x;
    int icon_y;

    if (request_active_client(client) < 0 ||
        fake_key_chord(XK_Alt_L, XK_F2) < 0) {
        fprintf(stderr,
                "wm-probe: could not open Applications on the active monitor\n");
        return -1;
    }
    launcher = wait_for_role("applications-window", None, 1);
    if (launcher == None || wait_for_focus_window(launcher) < 0 ||
        wait_for_frame_geometry(launcher, &applications_normal) < 0) {
        if (launcher != None)
            report_frame_geometry(
                "Applications was not centered on the active right monitor",
                launcher);
        return -1;
    }
    if (click_internal_maximize_button(launcher) < 0 ||
        wait_for_frame_geometry(launcher, &maximized) < 0 ||
        click_internal_maximize_button(launcher) < 0 ||
        wait_for_frame_geometry(launcher, &applications_normal) < 0) {
        report_frame_geometry(
            "Applications maximize did not stay on the active monitor",
            launcher);
        return -1;
    }
    if (drag_frame_title_to(launcher, 801, 350) < 0 ||
        wait_for_frame_geometry(launcher, &snapped_left) < 0 ||
        click_internal_maximize_button(launcher) < 0 ||
        wait_for_frame_geometry(launcher, &maximized) < 0 ||
        click_internal_maximize_button(launcher) < 0 ||
        wait_for_frame_geometry(launcher, &snapped_left) < 0) {
        report_frame_geometry(
            "Applications snap/maximize did not use the right monitor",
            launcher);
        return -1;
    }
    if (drag_internal_back_to_geometry(launcher, &applications_normal) < 0 ||
        click_internal_close_button(launcher) < 0 ||
        !internal_window_is_hidden(launcher)) {
        fprintf(stderr,
                "wm-probe: Applications did not restore and close after multi-monitor layout tests\n");
        return -1;
    }

    if (window_center_on_root(control_icon, &icon_x, &icon_y) < 0 ||
        fake_click_at(icon_x, icon_y) < 0) {
        fprintf(stderr,
                "wm-probe: could not open Control Panel from its desktop icon\n");
        return -1;
    }
    panel = wait_for_role("control-panel-window", None, 1);
    if (panel == None || wait_for_focus_window(panel) < 0 ||
        wait_for_frame_geometry(panel, &control_normal) < 0) {
        if (panel != None)
            report_frame_geometry(
                "Control Panel did not open on its primary-monitor icon",
                panel);
        return -1;
    }
    if (drag_frame_title_to(panel, 1200, 112) < 0 ||
        wait_for_frame_inside(panel, &right_monitor) < 0 ||
        get_frame_geometry(panel, &moved_panel) < 0) {
        report_frame_geometry(
            "Control Panel could not be moved onto the offset monitor", panel);
        return -1;
    }
    if (click_internal_maximize_button(panel) < 0 ||
        wait_for_frame_geometry(panel, &maximized) < 0 ||
        click_internal_maximize_button(panel) < 0 ||
        wait_for_frame_geometry(panel, &moved_panel) < 0) {
        report_frame_geometry(
            "Control Panel maximize did not use its current monitor", panel);
        return -1;
    }
    if (click_internal_close_button(panel) < 0 ||
        !internal_window_is_hidden(panel)) {
        fprintf(stderr,
                "wm-probe: Control Panel did not close after multi-monitor layout tests\n");
        return -1;
    }
    if (!role_remains_unmapped("applications-window") ||
        !role_remains_unmapped("control-panel-window")) {
        fprintf(stderr,
                "wm-probe: an internal window reopened unexpectedly\n");
        return -1;
    }
    if (request_active_client(client) < 0 ||
        fake_three_key_chord(XK_Control_L, XK_Shift_L, XK_Escape) < 0) {
        fprintf(stderr,
                "wm-probe: could not open Task Manager on the active monitor\n");
        return -1;
    }
    task_manager = wait_for_role("task-manager-window", None, 1);
    if (task_manager == None || wait_for_focus_window(task_manager) < 0 ||
        wait_for_frame_geometry(task_manager, &task_manager_normal) < 0) {
        if (task_manager != None)
            report_frame_geometry(
                "Task Manager was not centered on the active right monitor",
                task_manager);
        return -1;
    }
    if (click_internal_maximize_button(task_manager) < 0 ||
        wait_for_frame_geometry(task_manager, &maximized) < 0 ||
        click_internal_maximize_button(task_manager) < 0 ||
        wait_for_frame_geometry(task_manager, &task_manager_normal) < 0 ||
        drag_frame_title_to(task_manager, 801, 350) < 0 ||
        wait_for_frame_geometry(task_manager, &snapped_left) < 0 ||
        drag_internal_back_to_geometry(task_manager,
                                       &task_manager_normal) < 0 ||
        click_internal_close_button(task_manager) < 0 ||
        !internal_window_is_hidden(task_manager)) {
        report_frame_geometry(
            "Task Manager maximize, snap, restore, or close failed on the active monitor",
            task_manager);
        return -1;
    }
    if (!role_remains_unmapped("task-manager-window")) {
        fprintf(stderr,
                "wm-probe: Task Manager reopened unexpectedly after multi-monitor tests\n");
        return -1;
    }
    (void)applications_icon;
    return 0;
}

static int verify_multi_monitor_dialogs(Window client)
{
    const FrameGeometry root_geometry = {0, 0, 1600, 700};
    const FrameGeometry run_geometry = {965, 312, 470, 176};
    const FrameGeometry menu_geometry = {1404, 581, 196, 119};
    const FrameGeometry confirmation_geometry = {990, 312, 420, 176};
    Atom active_window = XInternAtom(display, "_NET_ACTIVE_WINDOW", False);
    Window run;
    Window menu;
    Window confirmation;
    Window shield;
    FrameGeometry actual;

    if (request_active_client(client) < 0 || move_pointer_to(100, 300) < 0 ||
        fake_key_chord(XK_Super_L, XK_r) < 0) {
        fprintf(stderr,
                "wm-probe: could not open Run for the active-monitor test\n");
        return -1;
    }
    run = wait_for_role("run-window", None, 1);
    if (run == None || wait_for_focus_window(run) < 0 ||
        wait_for_frame_geometry(run, &run_geometry) < 0) {
        if (run != None)
            report_frame_geometry(
                "Run was not centered on the focused client's monitor", run);
        return -1;
    }
    if (fake_key(XK_Escape) < 0 || !internal_window_is_hidden(run) ||
        wait_for_active_client(client) < 0) {
        fprintf(stderr,
                "wm-probe: closing multi-monitor Run did not restore client focus\n");
        return -1;
    }

    if (fake_button_at(Button3, 1599, 699) < 0) {
        fprintf(stderr,
                "wm-probe: could not open the right-monitor context menu\n");
        return -1;
    }
    menu = wait_for_role("desktop-menu", None, 1);
    if (menu == None || wait_for_frame_geometry(menu, &menu_geometry) < 0 ||
        wait_for_focus_and_active(client, client) < 0) {
        if (menu != None)
            report_frame_geometry(
                "desktop menu was not clamped to the offset monitor", menu);
        return -1;
    }
    if (fake_click_at(menu_geometry.x + 15, menu_geometry.y + 76) < 0 ||
        !internal_window_is_hidden(menu)) {
        fprintf(stderr,
                "wm-probe: could not select Restart from the right-monitor menu\n");
        return -1;
    }
    confirmation = wait_for_role("session-confirmation", None, 1);
    shield = wait_for_role("session-confirmation-shield", None, 1);
    if (confirmation == None || shield == None ||
        wait_for_focus_window(confirmation) < 0 ||
        wait_for_window_name(confirmation, "Confirm Restart") < 0 ||
        wait_for_frame_geometry(confirmation, &confirmation_geometry) < 0 ||
        wait_for_frame_geometry(shield, &root_geometry) < 0 ||
        !window_is_above(confirmation, shield) ||
        root_window_property(active_window) != client) {
        if (confirmation != None &&
            get_frame_geometry(confirmation, &actual) == 0)
            fprintf(stderr,
                    "wm-probe: right-monitor confirmation geometry was %d,%d %dx%d\n",
                    actual.x, actual.y, actual.width, actual.height);
        else
            fprintf(stderr,
                    "wm-probe: right-monitor confirmation was unavailable\n");
        return -1;
    }
    if (fake_key(XK_Escape) < 0 ||
        verify_session_confirmation_dismissed(
            confirmation, client, client,
            "Escape on the right-monitor confirmation") < 0) {
        fprintf(stderr,
                "wm-probe: right-monitor confirmation did not cancel safely\n");
        return -1;
    }
    return 0;
}

static int verify_multi_monitor(Atom change_state)
{
    const FrameGeometry expected_root = {0, 0, 1600, 700};
    Window applications_icon;
    Window control_icon;
    Window client = None;
    FrameGeometry actual_root = {0, 0, 0, 0};

    if (get_frame_geometry(root, &actual_root) < 0 ||
        !frame_geometry_equals(&actual_root, &expected_root)) {
        fprintf(stderr,
                "wm-probe: multi-monitor mode requires a 1600x700 X screen "
                "(got %d,%d %dx%d)\n",
                actual_root.x, actual_root.y, actual_root.width,
                actual_root.height);
        return -1;
    }
    if (verify_multi_monitor_static_icons(&applications_icon,
                                          &control_icon) < 0 ||
        verify_multi_monitor_client(change_state, &client) < 0 ||
        verify_multi_monitor_internal_windows(client, applications_icon,
                                              control_icon) < 0 ||
        verify_multi_monitor_dialogs(client) < 0)
        return -1;
    XDestroyWindow(display, client);
    XSync(display, False);
    return 0;
}

int main(int argc, char **argv)
{
    Window client;
    Window icon;
    Window applications_icon;
    Window launcher;
    Window control_panel;
    Atom supporting;
    Atom net_supported;
    Atom net_wm_icon;
    Atom delete_window;
    Atom change_state;
    XEvent event;
    const char *launch_marker = getenv("WIN31X_SMOKE_MARKER");
    const char *run_marker = getenv("WIN31X_RUN_MARKER");
    const char *wifi_marker = getenv("WIN31X_WIFI_SECRET_MARKER");
    const char *lock_marker = getenv("WIN31X_LOCKER_MARKER");
    const char *system_action_marker =
        getenv("WIN31X_SYSTEM_ACTION_MARKER");
    int logout_mode = argc == 2 &&
                      strcmp(argv[1], "--desktop-menu-logout") == 0;
    int multi_monitor_mode = argc == 2 &&
                             strcmp(argv[1], "--multi-monitor") == 0;

    if (argc != 1 && !logout_mode && !multi_monitor_mode) {
        fprintf(stderr,
                "usage: wm-probe [--desktop-menu-logout|--multi-monitor]\n");
        return 2;
    }

    display = XOpenDisplay(NULL);
    if (display == NULL) {
        fprintf(stderr, "wm-probe: cannot open DISPLAY\n");
        return 2;
    }
    root = DefaultRootWindow(display);
    wm_state_atom = XInternAtom(display, "WM_STATE", False);
    net_wm_state_atom = XInternAtom(display, "_NET_WM_STATE", False);
    hidden_atom = XInternAtom(display, "_NET_WM_STATE_HIDDEN", False);
    maximized_horz_atom =
        XInternAtom(display, "_NET_WM_STATE_MAXIMIZED_HORZ", False);
    maximized_vert_atom =
        XInternAtom(display, "_NET_WM_STATE_MAXIMIZED_VERT", False);
    role_atom = XInternAtom(display, "_WIN31X_ROLE", False);
    client_atom = XInternAtom(display, "_WIN31X_CLIENT", False);
    net_wm_name_atom = XInternAtom(display, "_NET_WM_NAME", False);
    supporting = XInternAtom(display, "_NET_SUPPORTING_WM_CHECK", False);
    net_supported = XInternAtom(display, "_NET_SUPPORTED", False);
    net_wm_icon = XInternAtom(display, "_NET_WM_ICON", False);
    delete_window = XInternAtom(display, "WM_DELETE_WINDOW", False);
    change_state = XInternAtom(display, "WM_CHANGE_STATE", False);
    if (wait_for_wm_check(supporting) < 0) {
        fprintf(stderr, "wm-probe: Win31 X did not publish its WM check\n");
        return 1;
    }
    if (logout_mode) {
        int result = verify_desktop_menu_logout(supporting);

        XCloseDisplay(display);
        if (result < 0)
            return 1;
        puts("X11 desktop-menu logout test passed");
        return 0;
    }
    if (multi_monitor_mode) {
        int result = verify_multi_monitor(change_state);

        XCloseDisplay(display);
        if (result < 0)
            return 1;
        puts("X11 multi-monitor window-manager test passed");
        return 0;
    }
    if (!window_atom_property_contains(root, net_supported, net_wm_icon)) {
        fprintf(stderr, "wm-probe: _NET_WM_ICON was not advertised\n");
        return 1;
    }
    if (verify_preexisting_client() < 0)
        return 1;
    if (verify_transient_lifecycle(change_state) < 0)
        return 1;
    if (verify_input_only_map() < 0)
        return 1;
    if (verify_client_list_order() < 0)
        return 1;
    if (verify_client_stack_requests() < 0)
        return 1;
    if (verify_map_preserves_launcher() < 0)
        return 1;
    if (verify_resize_increment_minimum() < 0)
        return 1;
    if (verify_window_gravity() < 0)
        return 1;
    if (verify_synthetic_configure_geometry() < 0)
        return 1;
    if (verify_maximize_and_snap() < 0)
        return 1;
    if (verify_click_to_raise(change_state) < 0)
        return 1;

    client = XCreateSimpleWindow(display, root, 80, 80, 260, 140, 0,
                                 BlackPixel(display, DefaultScreen(display)),
                                 WhitePixel(display, DefaultScreen(display)));
    XStoreName(display, client, "Win31 X Probe");
    XSetWMProtocols(display, client, &delete_window, 1);
    if (publish_test_window_icon(client) < 0) {
        fprintf(stderr, "wm-probe: could not publish test window icon\n");
        return 1;
    }
    XMapWindow(display, client);
    XFlush(display);
    if (wait_for_state(client, NormalState) < 0) {
        fprintf(stderr, "wm-probe: client never reached NormalState\n");
        return 1;
    }

    memset(&event, 0, sizeof(event));
    event.xclient.type = ClientMessage;
    event.xclient.display = display;
    event.xclient.window = client;
    event.xclient.message_type = change_state;
    event.xclient.format = 32;
    event.xclient.data.l[0] = IconicState;
    XSendEvent(display, root, False,
               SubstructureRedirectMask | SubstructureNotifyMask, &event);
    XFlush(display);
    if (wait_for_state(client, IconicState) < 0) {
        fprintf(stderr, "wm-probe: client never reached IconicState\n");
        return 1;
    }
    icon = wait_for_role("minimized-icon", client, 1);
    if (icon == None) {
        fprintf(stderr, "wm-probe: no desktop icon appeared for minimized client\n");
        return 1;
    }
    if (wait_for_window_pixel(icon, 56, 29,
                              expected_rgb_pixel(0x12U, 0xabU, 0x34U)) < 0) {
        fprintf(stderr,
                "wm-probe: minimized client did not use its _NET_WM_ICON\n");
        return 1;
    }
    send_button_one(icon);
    if (wait_for_state(client, NormalState) < 0) {
        fprintf(stderr, "wm-probe: clicking the desktop icon did not restore client\n");
        return 1;
    }
    if (verify_task_manager(client) < 0)
        return 1;
    if (verify_control_panel(client, wifi_marker, lock_marker) < 0)
        return 1;

    applications_icon = wait_for_role("applications-icon", None, 1);
    if (applications_icon == None) {
        fprintf(stderr, "wm-probe: Applications desktop icon was not found\n");
        return 1;
    }
    launcher = wait_for_role("applications-window", None, 1);
    control_panel = wait_for_role("control-panel-window", None, 1);
    if (launcher == None || control_panel == None) {
        fprintf(stderr,
                "wm-probe: internal windows did not remain open for launch test\n");
        return 1;
    }
    send_button_one(applications_icon);
    if (wait_for_focus_window(launcher) < 0) {
        fprintf(stderr,
                "wm-probe: Applications did not refocus for icon test\n");
        return 1;
    }
    {
        unsigned long expected_icon_pixel =
            expected_rgb_pixel(0xffU, 0xffU, 0x99U);

        if (wait_for_window_pixel(launcher, 72, 63,
                                  expected_icon_pixel) < 0) {
            XWindowAttributes launcher_attributes;

            memset(&launcher_attributes, 0, sizeof(launcher_attributes));
            (void)XGetWindowAttributes(display, launcher,
                                       &launcher_attributes);
        fprintf(stderr,
                    "wm-probe: Applications did not render the desktop entry's real icon (got 0x%lx, expected 0x%lx, size %dx%d, map %d)\n",
                    window_pixel_at(launcher, 72, 63), expected_icon_pixel,
                    launcher_attributes.width, launcher_attributes.height,
                    launcher_attributes.map_state);
            return 1;
        }
    }
    if (verify_run_dialog(run_marker, client, launcher, control_panel) < 0) {
        fprintf(stderr, "wm-probe: Run dialog verification failed\n");
        return 1;
    }
    send_button_one_at(launcher, 50, 50);
    send_button_one_at(launcher, 50, 50);
    if (wait_for_file(launch_marker) < 0) {
        fprintf(stderr, "wm-probe: launcher did not start its selected application\n");
        return 1;
    }
    if (!internal_window_remains_viewable(launcher) ||
        !internal_window_remains_viewable(control_panel)) {
        fprintf(stderr,
                "wm-probe: launching an application closed an internal window\n");
        return 1;
    }
    send_escape(launcher);
    send_escape(control_panel);
    if (!internal_window_remains_viewable(launcher) ||
        !internal_window_remains_viewable(control_panel)) {
        fprintf(stderr, "wm-probe: Escape closed an internal window\n");
        return 1;
    }
    if (verify_desktop_menu(client, applications_icon, launcher,
                            control_panel, lock_marker,
                            system_action_marker) < 0) {
        fprintf(stderr, "wm-probe: desktop menu verification failed\n");
        return 1;
    }

    if (click_internal_close_button(launcher) < 0 ||
        !internal_window_is_hidden(launcher) ||
        !internal_window_remains_viewable(control_panel)) {
        fprintf(stderr,
                "wm-probe: Applications X did not close only Applications\n");
        return 1;
    }
    send_button_one(applications_icon);
    launcher = wait_for_role("applications-window", None, 1);
    if (launcher == None || !internal_window_remains_viewable(control_panel)) {
        fprintf(stderr, "wm-probe: Applications window did not reopen alone\n");
        return 1;
    }
    if (click_internal_close_button(control_panel) < 0 ||
        !internal_window_is_hidden(control_panel) ||
        !internal_window_remains_viewable(launcher)) {
        fprintf(stderr,
                "wm-probe: Control Panel X did not close only Control Panel\n");
        return 1;
    }
    if (click_internal_close_button(launcher) < 0 ||
        !internal_window_is_hidden(launcher)) {
        fprintf(stderr, "wm-probe: Applications X did not close Applications\n");
        return 1;
    }

    XUnmapWindow(display, client);
    XFlush(display);
    if (wait_until_unmanaged(client) < 0) {
        fprintf(stderr, "wm-probe: withdrawn client was not returned to root\n");
        return 1;
    }
    XDestroyWindow(display, client);
    XCloseDisplay(display);
    puts("X11 window-manager smoke test passed");
    return 0;
}
