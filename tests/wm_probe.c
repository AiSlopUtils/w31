#define _POSIX_C_SOURCE 200809L

#include <X11/Xatom.h>
#include <X11/Xlib.h>
#include <X11/Xutil.h>
#include <X11/extensions/XTest.h>
#include <X11/keysym.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

static Display *display;
static Window root;
static Atom wm_state_atom;
static Atom role_atom;
static Atom client_atom;

static int wait_until_unmanaged(Window window);

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

static int fake_click_at(int x, int y)
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
    if (!XTestFakeMotionEvent(display, DefaultScreen(display), x, y, CurrentTime) ||
        !XTestFakeButtonEvent(display, Button1, True, CurrentTime) ||
        !XTestFakeButtonEvent(display, Button1, False, CurrentTime)) {
        fprintf(stderr, "wm-probe: could not inject a pointer click\n");
        return -1;
    }
    XFlush(display);
    return 0;
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

static int verify_map_dismisses_launcher(void)
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
        !internal_window_is_hidden(launcher) ||
        wait_for_active_client(client) < 0) {
        fprintf(stderr,
                "wm-probe: mapping client did not dismiss launcher and activate it\n");
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

static void send_key_sym(Window window, KeySym symbol)
{
    XEvent event;

    memset(&event, 0, sizeof(event));
    event.xkey.type = KeyPress;
    event.xkey.display = display;
    event.xkey.window = window;
    event.xkey.root = root;
    event.xkey.keycode = XKeysymToKeycode(display, symbol);
    event.xkey.same_screen = True;
    XSendEvent(display, window, False, KeyPressMask, &event);
    XFlush(display);
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
    if (launcher == None || !internal_window_is_hidden(panel)) {
        fprintf(stderr,
                "wm-probe: Applications and Control Panel were visible together\n");
        return -1;
    }
    send_escape(launcher);
    if (!internal_window_is_hidden(launcher)) {
        fprintf(stderr, "wm-probe: Applications did not close during panel test\n");
        return -1;
    }

    send_button_one(control_icon);
    panel = wait_for_role("control-panel-window", None, 1);
    if (panel == None)
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

    if (fake_click_at(150, 100) < 0 ||
        !internal_window_is_hidden(panel) || wait_for_active_client(client) < 0) {
        fprintf(stderr,
                "wm-probe: clicking a client did not dismiss and lower Control Panel\n");
        return -1;
    }
    send_button_one(control_icon);
    panel = wait_for_role("control-panel-window", None, 1);
    if (panel == None)
        return -1;
    send_escape(panel);
    if (!internal_window_is_hidden(panel)) {
        fprintf(stderr, "wm-probe: Escape did not close Control Panel\n");
        return -1;
    }
    return 0;
}

int main(void)
{
    Window client;
    Window icon;
    Window applications_icon;
    Window launcher;
    Atom supporting;
    Atom delete_window;
    Atom change_state;
    XEvent event;
    const char *launch_marker = getenv("WIN31X_SMOKE_MARKER");
    const char *wifi_marker = getenv("WIN31X_WIFI_SECRET_MARKER");
    const char *lock_marker = getenv("WIN31X_LOCKER_MARKER");

    display = XOpenDisplay(NULL);
    if (display == NULL) {
        fprintf(stderr, "wm-probe: cannot open DISPLAY\n");
        return 2;
    }
    root = DefaultRootWindow(display);
    wm_state_atom = XInternAtom(display, "WM_STATE", False);
    role_atom = XInternAtom(display, "_WIN31X_ROLE", False);
    client_atom = XInternAtom(display, "_WIN31X_CLIENT", False);
    supporting = XInternAtom(display, "_NET_SUPPORTING_WM_CHECK", False);
    delete_window = XInternAtom(display, "WM_DELETE_WINDOW", False);
    change_state = XInternAtom(display, "WM_CHANGE_STATE", False);
    if (wait_for_wm_check(supporting) < 0) {
        fprintf(stderr, "wm-probe: Win31 X did not publish its WM check\n");
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
    if (verify_map_dismisses_launcher() < 0)
        return 1;
    if (verify_resize_increment_minimum() < 0)
        return 1;
    if (verify_window_gravity() < 0)
        return 1;
    if (verify_synthetic_configure_geometry() < 0)
        return 1;
    if (verify_click_to_raise(change_state) < 0)
        return 1;

    client = XCreateSimpleWindow(display, root, 80, 80, 260, 140, 0,
                                 BlackPixel(display, DefaultScreen(display)),
                                 WhitePixel(display, DefaultScreen(display)));
    XStoreName(display, client, "Win31 X Probe");
    XSetWMProtocols(display, client, &delete_window, 1);
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
    send_button_one(icon);
    if (wait_for_state(client, NormalState) < 0) {
        fprintf(stderr, "wm-probe: clicking the desktop icon did not restore client\n");
        return 1;
    }
    if (verify_control_panel(client, wifi_marker, lock_marker) < 0)
        return 1;

    applications_icon = wait_for_role("applications-icon", None, 1);
    if (applications_icon == None) {
        fprintf(stderr, "wm-probe: Applications desktop icon was not found\n");
        return 1;
    }
    send_button_one(applications_icon);
    launcher = wait_for_role("applications-window", None, 1);
    if (launcher == None) {
        fprintf(stderr, "wm-probe: Applications window did not open\n");
        return 1;
    }
    send_button_one_at(launcher, 50, 50);
    send_button_one_at(launcher, 50, 50);
    if (wait_for_file(launch_marker) < 0) {
        fprintf(stderr, "wm-probe: launcher did not start its selected application\n");
        return 1;
    }
    if (!internal_window_is_hidden(launcher)) {
        fprintf(stderr, "wm-probe: Applications window stayed open after launch\n");
        return 1;
    }
    send_button_one(applications_icon);
    launcher = wait_for_role("applications-window", None, 1);
    if (launcher == None) {
        fprintf(stderr, "wm-probe: Applications window did not reopen\n");
        return 1;
    }
    send_escape(launcher);
    if (!internal_window_is_hidden(launcher)) {
        fprintf(stderr, "wm-probe: Escape did not close Applications window\n");
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
