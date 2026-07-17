#define _POSIX_C_SOURCE 200809L

#include <X11/Xatom.h>
#include <X11/Xlib.h>

#include <signal.h>
#include <stdio.h>
#include <string.h>
#include <sys/select.h>
#include <unistd.h>

static volatile sig_atomic_t running = 1;

static void stop_client(int signal_number)
{
    (void)signal_number;
    running = 0;
}

int main(void)
{
    Display *display = XOpenDisplay(NULL);
    Window root;
    Window window;
    Atom marker;
    Atom protocols;
    Atom delete_window;
    struct sigaction action;
    int connection;

    if (display == NULL) {
        fprintf(stderr, "preexisting-client: cannot open DISPLAY\n");
        return 2;
    }
    root = DefaultRootWindow(display);
    marker = XInternAtom(display, "_WIN31X_PREEXISTING_CLIENT", False);
    protocols = XInternAtom(display, "WM_PROTOCOLS", False);
    delete_window = XInternAtom(display, "WM_DELETE_WINDOW", False);
    window = XCreateSimpleWindow(display, root, 130, 120, 280, 150, 1,
                                 BlackPixel(display, DefaultScreen(display)),
                                 WhitePixel(display, DefaultScreen(display)));
    XStoreName(display, window, "Pre-existing client");
    XSetWMProtocols(display, window, &delete_window, 1);
    XSelectInput(display, window, StructureNotifyMask);
    XMapWindow(display, window);
    XChangeProperty(display, root, marker, XA_WINDOW, 32, PropModeReplace,
                    (unsigned char *)&window, 1);
    XSync(display, False);

    memset(&action, 0, sizeof(action));
    action.sa_handler = stop_client;
    sigemptyset(&action.sa_mask);
    sigaction(SIGINT, &action, NULL);
    sigaction(SIGTERM, &action, NULL);
    connection = ConnectionNumber(display);
    while (running) {
        while (XPending(display) > 0) {
            XEvent event;
            XNextEvent(display, &event);
            if (event.type == ClientMessage &&
                event.xclient.message_type == protocols &&
                (Atom)event.xclient.data.l[0] == delete_window)
                running = 0;
            if (event.type == DestroyNotify)
                running = 0;
        }
        if (running) {
            fd_set descriptors;
            struct timeval timeout = {0, 100000};
            FD_ZERO(&descriptors);
            FD_SET(connection, &descriptors);
            select(connection + 1, &descriptors, NULL, NULL, &timeout);
        }
    }
    XDeleteProperty(display, root, marker);
    XDestroyWindow(display, window);
    XCloseDisplay(display);
    return 0;
}
