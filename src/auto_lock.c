#define _POSIX_C_SOURCE 200809L

#include "auto_lock.h"

#include <errno.h>
#include <signal.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#define DEFAULT_XSS_LOCK "/usr/bin/xss-lock"
#define DEFAULT_LOCKER "/usr/bin/xsecurelock"
#define XSECURELOCK_PASSWORD_PROMPT "XSECURELOCK_PASSWORD_PROMPT"
#define XSECURELOCK_PASSWORD_PROMPT_ASTERISKS "asterisks"
#define AUTO_LOCK_MINUTES_MIN 1U
#define AUTO_LOCK_MINUTES_MAX 120U
#define SHUTDOWN_POLL_COUNT 200U
#define SHUTDOWN_POLL_NANOSECONDS 10000000L

static void set_status(Win31xAutoLock *lock, const char *format, ...)
{
    va_list arguments;

    va_start(arguments, format);
    (void)vsnprintf(lock->status, sizeof(lock->status), format, arguments);
    va_end(arguments);
}

static unsigned int clamp_timeout(unsigned int minutes)
{
    if (minutes < AUTO_LOCK_MINUTES_MIN)
        return AUTO_LOCK_MINUTES_MIN;
    if (minutes > AUTO_LOCK_MINUTES_MAX)
        return AUTO_LOCK_MINUTES_MAX;
    return minutes;
}

static bool executable_regular_file(const char *path)
{
    struct stat information;

    return path != NULL && path[0] == '/' && stat(path, &information) == 0 &&
           S_ISREG(information.st_mode) && access(path, X_OK) == 0;
}

static int resolve_program(const char *environment_name,
                           const char *default_path, char *destination,
                           size_t capacity, Win31xAutoLock *lock)
{
    const char *override = getenv(environment_name);
    const char *candidate = default_path;
    int written;

    if (override != NULL && override[0] != '\0') {
        if (override[0] != '/') {
            errno = EINVAL;
            set_status(lock, "%s must name an absolute executable",
                       environment_name);
            return -1;
        }
        candidate = override;
    }
    written = snprintf(destination, capacity, "%s", candidate);
    if (written < 0 || (size_t)written >= capacity) {
        errno = ENAMETOOLONG;
        set_status(lock, "%s path is too long", environment_name);
        return -1;
    }
    if (!executable_regular_file(destination)) {
        errno = ENOENT;
        set_status(lock, "%s is not an executable regular file", destination);
        destination[0] = '\0';
        return -1;
    }
    return 0;
}

static void set_provider_name(Win31xAutoLock *lock)
{
    const char *base = strrchr(lock->locker_path, '/');
    size_t length;

    if (base == NULL)
        base = lock->locker_path;
    else
        ++base;
    length = strlen(base);
    if (length >= sizeof(lock->provider))
        length = sizeof(lock->provider) - 1U;
    memcpy(lock->provider, base, length);
    lock->provider[length] = '\0';
}

static int resolve_programs(Win31xAutoLock *lock)
{
    int locker_result;
    int supervisor_result;

    locker_result = resolve_program("WIN31X_LOCKER", DEFAULT_LOCKER,
                                    lock->locker_path,
                                    sizeof(lock->locker_path), lock);
    lock->locker_available = locker_result == 0;
    if (lock->locker_available)
        set_provider_name(lock);
    else
        (void)snprintf(lock->provider, sizeof(lock->provider), "Unavailable");

    supervisor_result = resolve_program("WIN31X_XSS_LOCK", DEFAULT_XSS_LOCK,
                                        lock->xss_lock_path,
                                        sizeof(lock->xss_lock_path), lock);
    lock->paths_resolved = locker_result == 0 && supervisor_result == 0;
    return lock->paths_resolved ? 0 : -1;
}

static void save_screen_saver(Win31xAutoLock *lock)
{
    if (lock->display == NULL)
        return;
    if (XGetScreenSaver(lock->display, &lock->original_timeout,
                        &lock->original_interval,
                        &lock->original_prefer_blanking,
                        &lock->original_allow_exposures) != 0)
        lock->screen_saver_saved = true;
}

static void restore_screen_saver(Win31xAutoLock *lock)
{
    if (!lock->screen_saver_saved || !lock->screen_saver_applied ||
        lock->display == NULL)
        return;
    (void)XSetScreenSaver(lock->display, lock->original_timeout,
                          lock->original_interval,
                          lock->original_prefer_blanking,
                          lock->original_allow_exposures);
    XFlush(lock->display);
    lock->screen_saver_applied = false;
}

static int apply_configuration(Win31xAutoLock *lock)
{
    int timeout_seconds;

    if (lock->supervisor_pid <= 0 || !lock->available) {
        restore_screen_saver(lock);
        errno = ENODEV;
        return -1;
    }
    if (lock->display == NULL)
        return 0;
    if (!lock->screen_saver_saved) {
        errno = EIO;
        set_status(lock, "Could not read the X screen-saver settings");
        return -1;
    }

    timeout_seconds = lock->enabled
                          ? (int)(lock->timeout_minutes * 60U)
                          : 0;
    if (XSetScreenSaver(lock->display, timeout_seconds,
                        lock->original_interval,
                        lock->original_prefer_blanking,
                        lock->original_allow_exposures) == 0) {
        errno = EIO;
        set_status(lock, "Could not update the X screen-saver settings");
        return -1;
    }
    XFlush(lock->display);
    lock->screen_saver_applied = true;
    return 0;
}

static void set_ready_status(Win31xAutoLock *lock)
{
    if (lock->direct_pid > 0) {
        set_status(lock, "Locking now with %s", lock->provider);
    } else if (!lock->available || lock->supervisor_pid <= 0) {
        set_status(lock, "Auto lock is unavailable");
    } else if (lock->enabled) {
        set_status(lock, "Auto lock after %u minute%s using %s",
                   lock->timeout_minutes,
                   lock->timeout_minutes == 1U ? "" : "s", lock->provider);
    } else {
        set_status(lock, "Auto lock is off; %s is ready", lock->provider);
    }
}

static void reset_one_signal(int signal_number)
{
    struct sigaction action;

    memset(&action, 0, sizeof(action));
    action.sa_handler = SIG_DFL;
    sigemptyset(&action.sa_mask);
    (void)sigaction(signal_number, &action, NULL);
}

static void prepare_child(int display_fd)
{
    sigset_t empty_mask;

    sigemptyset(&empty_mask);
    (void)sigprocmask(SIG_SETMASK, &empty_mask, NULL);
    reset_one_signal(SIGHUP);
    reset_one_signal(SIGINT);
    reset_one_signal(SIGQUIT);
    reset_one_signal(SIGTERM);
    reset_one_signal(SIGCHLD);
    reset_one_signal(SIGPIPE);
    if (display_fd >= 0)
        (void)close(display_fd);
}

static int prepare_locker_environment(void)
{
    return setenv(XSECURELOCK_PASSWORD_PROMPT,
                  XSECURELOCK_PASSWORD_PROMPT_ASTERISKS, 1);
}

static _Noreturn void execute_supervisor(Win31xAutoLock *lock, int display_fd)
{
    char *arguments[4];

    prepare_child(display_fd);
    if (prepare_locker_environment() < 0)
        _exit(126);
    arguments[0] = lock->xss_lock_path;
    arguments[1] = "--";
    arguments[2] = lock->locker_path;
    arguments[3] = NULL;
    execv(arguments[0], arguments);
    _exit(127);
}

static _Noreturn void execute_locker(Win31xAutoLock *lock, int display_fd)
{
    char *arguments[2];

    prepare_child(display_fd);
    if (prepare_locker_environment() < 0)
        _exit(126);
    arguments[0] = lock->locker_path;
    arguments[1] = NULL;
    execv(arguments[0], arguments);
    _exit(127);
}

int win31x_auto_lock_start_supervisor(Win31xAutoLock *lock)
{
    pid_t child;
    int display_fd;

    if (lock == NULL || !lock->initialized) {
        errno = EINVAL;
        return -1;
    }
    if (!lock->paths_resolved) {
        errno = ENOENT;
        return -1;
    }
    if (lock->supervisor_pid > 0)
        return 0;

    display_fd = lock->display == NULL ? -1 : ConnectionNumber(lock->display);
    child = fork();
    if (child < 0) {
        lock->available = false;
        set_status(lock, "Could not start xss-lock: %s", strerror(errno));
        return -1;
    }
    if (child == 0)
        execute_supervisor(lock, display_fd);

    lock->supervisor_pid = child;
    lock->available = true;
    if (apply_configuration(lock) < 0 && lock->display != NULL)
        return -1;
    set_ready_status(lock);
    return 0;
}

int win31x_auto_lock_configure(Win31xAutoLock *lock, bool enabled,
                               unsigned int timeout_minutes)
{
    if (lock == NULL || !lock->initialized) {
        errno = EINVAL;
        return -1;
    }
    lock->enabled = enabled;
    lock->timeout_minutes = clamp_timeout(timeout_minutes);
    if (apply_configuration(lock) < 0) {
        if (lock->supervisor_pid <= 0)
            set_status(lock, "Auto lock is unavailable");
        return -1;
    }
    set_ready_status(lock);
    return 0;
}

int win31x_auto_lock_set_enabled(Win31xAutoLock *lock, bool enabled)
{
    if (lock == NULL) {
        errno = EINVAL;
        return -1;
    }
    return win31x_auto_lock_configure(lock, enabled, lock->timeout_minutes);
}

int win31x_auto_lock_set_timeout(Win31xAutoLock *lock,
                                 unsigned int timeout_minutes)
{
    if (lock == NULL) {
        errno = EINVAL;
        return -1;
    }
    return win31x_auto_lock_configure(lock, lock->enabled, timeout_minutes);
}

int win31x_auto_lock_lock_now(Win31xAutoLock *lock)
{
    pid_t child;
    int display_fd;

    if (lock == NULL || !lock->initialized) {
        errno = EINVAL;
        return -1;
    }
    if (!lock->locker_available) {
        errno = ENOENT;
        set_status(lock, "No screen locker is available");
        return -1;
    }
    if (lock->direct_pid > 0) {
        errno = EALREADY;
        set_status(lock, "%s is already locking the screen", lock->provider);
        return -1;
    }

    display_fd = lock->display == NULL ? -1 : ConnectionNumber(lock->display);
    child = fork();
    if (child < 0) {
        set_status(lock, "Could not start %s: %s", lock->provider,
                   strerror(errno));
        return -1;
    }
    if (child == 0)
        execute_locker(lock, display_fd);

    lock->direct_pid = child;
    set_ready_status(lock);
    return 0;
}

static void format_exit_status(Win31xAutoLock *lock, const char *process,
                               int wait_status)
{
    if (WIFEXITED(wait_status)) {
        set_status(lock, "%s exited with status %d", process,
                   WEXITSTATUS(wait_status));
    } else if (WIFSIGNALED(wait_status)) {
        set_status(lock, "%s was terminated by signal %d", process,
                   WTERMSIG(wait_status));
    } else {
        set_status(lock, "%s stopped unexpectedly", process);
    }
}

bool win31x_auto_lock_handle_child_exit(Win31xAutoLock *lock, pid_t pid,
                                        int wait_status)
{
    if (lock == NULL || pid <= 0)
        return false;
    if (pid == lock->supervisor_pid) {
        lock->supervisor_pid = 0;
        lock->available = false;
        restore_screen_saver(lock);
        if (!lock->shutting_down)
            format_exit_status(lock, "xss-lock", wait_status);
        return true;
    }
    if (pid == lock->direct_pid) {
        lock->direct_pid = 0;
        if (!lock->shutting_down) {
            if (WIFEXITED(wait_status) && WEXITSTATUS(wait_status) == 0)
                set_ready_status(lock);
            else
                format_exit_status(lock, lock->provider, wait_status);
        }
        return true;
    }
    return false;
}

static pid_t wait_without_interruption(pid_t pid, int *wait_status, int options)
{
    pid_t result;

    do {
        result = waitpid(pid, wait_status, options);
    } while (result < 0 && errno == EINTR);
    return result;
}

static void terminate_owned_child(pid_t *slot)
{
    struct timespec pause_time;
    pid_t pid;
    pid_t result;
    int wait_status;
    unsigned int attempt;

    if (slot == NULL || *slot <= 0)
        return;
    pid = *slot;
    result = wait_without_interruption(pid, &wait_status, WNOHANG);
    if (result == pid || (result < 0 && errno == ECHILD)) {
        *slot = 0;
        return;
    }
    if (result < 0) {
        *slot = 0;
        return;
    }

    (void)kill(pid, SIGTERM);
    pause_time.tv_sec = 0;
    pause_time.tv_nsec = SHUTDOWN_POLL_NANOSECONDS;
    for (attempt = 0; attempt < SHUTDOWN_POLL_COUNT; ++attempt) {
        result = wait_without_interruption(pid, &wait_status, WNOHANG);
        if (result == pid || (result < 0 && errno == ECHILD)) {
            *slot = 0;
            return;
        }
        if (result < 0)
            break;
        while (nanosleep(&pause_time, &pause_time) < 0 && errno == EINTR)
            ;
        pause_time.tv_sec = 0;
        pause_time.tv_nsec = SHUTDOWN_POLL_NANOSECONDS;
    }

    if (wait_without_interruption(pid, &wait_status, WNOHANG) == 0) {
        (void)kill(pid, SIGKILL);
        (void)wait_without_interruption(pid, &wait_status, 0);
    }
    *slot = 0;
}

void win31x_auto_lock_shutdown(Win31xAutoLock *lock)
{
    if (lock == NULL || !lock->initialized)
        return;
    lock->shutting_down = true;
    restore_screen_saver(lock);
    terminate_owned_child(&lock->supervisor_pid);
    terminate_owned_child(&lock->direct_pid);
    lock->available = false;
    lock->screen_saver_saved = false;
    lock->display = NULL;
    lock->initialized = false;
    lock->shutting_down = false;
    set_status(lock, "Auto lock stopped");
}

int win31x_auto_lock_init(Win31xAutoLock *lock, Display *display,
                          bool enabled, unsigned int timeout_minutes)
{
    if (lock == NULL) {
        errno = EINVAL;
        return -1;
    }
    memset(lock, 0, sizeof(*lock));
    lock->display = display;
    lock->initialized = true;
    lock->enabled = enabled;
    lock->timeout_minutes = clamp_timeout(timeout_minutes);
    (void)snprintf(lock->provider, sizeof(lock->provider), "Unavailable");
    set_status(lock, "Auto lock is unavailable");
    save_screen_saver(lock);

    if (resolve_programs(lock) < 0)
        return -1;
    if (win31x_auto_lock_start_supervisor(lock) < 0)
        return -1;
    return win31x_auto_lock_configure(lock, enabled, timeout_minutes);
}
