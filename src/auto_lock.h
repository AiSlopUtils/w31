#ifndef WIN31X_AUTO_LOCK_H
#define WIN31X_AUTO_LOCK_H

#include <X11/Xlib.h>

#include <stdbool.h>
#include <sys/types.h>

#define WIN31X_AUTO_LOCK_PATH_CAPACITY 4096U
#define WIN31X_AUTO_LOCK_PROVIDER_CAPACITY 128U
#define WIN31X_AUTO_LOCK_STATUS_CAPACITY 256U

/*
 * Process and X server state for the external screen-lock supervisor.
 *
 * The public fields are intended for the Control Panel status display.  The
 * remaining fields let the object have static storage without hiding an
 * allocation behind the API; callers should otherwise treat them as private.
 */
typedef struct {
    bool available;
    bool locker_available;
    bool enabled;
    unsigned int timeout_minutes;
    pid_t supervisor_pid;
    pid_t direct_pid;
    char provider[WIN31X_AUTO_LOCK_PROVIDER_CAPACITY];
    char status[WIN31X_AUTO_LOCK_STATUS_CAPACITY];

    Display *display;
    bool initialized;
    bool paths_resolved;
    bool shutting_down;
    bool screen_saver_saved;
    bool screen_saver_applied;
    int original_timeout;
    int original_interval;
    int original_prefer_blanking;
    int original_allow_exposures;
    char xss_lock_path[WIN31X_AUTO_LOCK_PATH_CAPACITY];
    char locker_path[WIN31X_AUTO_LOCK_PATH_CAPACITY];
} Win31xAutoLock;

/*
 * Resolve the lock programs, save the X server's current saver policy, start
 * `xss-lock -- locker`, and apply the requested idle-lock configuration.
 *
 * WIN31X_XSS_LOCK and WIN31X_LOCKER may override the Debian defaults for
 * tests or local installations.  Overrides must be absolute executable
 * regular files.  On failure the object remains safe to inspect and shut down.
 */
int win31x_auto_lock_init(Win31xAutoLock *lock, Display *display,
                          bool enabled, unsigned int timeout_minutes);

/* Start or restart the asynchronous xss-lock supervisor. */
int win31x_auto_lock_start_supervisor(Win31xAutoLock *lock);

/* Configure automatic locking. Minutes are clamped to the inclusive 1..120 range. */
int win31x_auto_lock_configure(Win31xAutoLock *lock, bool enabled,
                               unsigned int timeout_minutes);
int win31x_auto_lock_set_enabled(Win31xAutoLock *lock, bool enabled);
int win31x_auto_lock_set_timeout(Win31xAutoLock *lock,
                                 unsigned int timeout_minutes);

/* Start the resolved locker immediately without a shell or password handling. */
int win31x_auto_lock_lock_now(Win31xAutoLock *lock);

/*
 * Pass raw waitpid(2) results here. Returns true when pid belonged to this
 * module. The caller owns SIGCHLD handling and must not use SA_NOCLDWAIT.
 */
bool win31x_auto_lock_handle_child_exit(Win31xAutoLock *lock, pid_t pid,
                                        int wait_status);

/* Restore the saved server policy, terminate owned children, and reap them. */
void win31x_auto_lock_shutdown(Win31xAutoLock *lock);

#endif
