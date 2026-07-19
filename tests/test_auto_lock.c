#define _DARWIN_C_SOURCE
#define _DEFAULT_SOURCE
#define _POSIX_C_SOURCE 200809L

#include "auto_lock.h"

#include <X11/Xlib.h>

#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#define TEST_PATH_CAPACITY 4096U
#define PASSWORD_PROMPT_ENVIRONMENT "XSECURELOCK_PASSWORD_PROMPT"
#define PASSWORD_PROMPT_ASTERISKS "asterisks"

typedef struct {
    char directory[TEST_PATH_CAPACITY];
    char supervisor[TEST_PATH_CAPACITY];
    char locker[TEST_PATH_CAPACITY];
    char supervisor_marker[TEST_PATH_CAPACITY];
    char locker_marker[TEST_PATH_CAPACITY];
} TestFixture;

static int failures;
static volatile sig_atomic_t fake_child_running = 1;

#define CHECK(condition)                                                       \
    do {                                                                       \
        if (!(condition)) {                                                    \
            fprintf(stderr, "test-auto-lock: check failed at %s:%d: %s\n",  \
                    __FILE__, __LINE__, #condition);                           \
            ++failures;                                                        \
        }                                                                      \
    } while (0)

static const char *base_name(const char *path)
{
    const char *slash = strrchr(path, '/');

    return slash == NULL ? path : slash + 1;
}

static void stop_fake_child(int signal_number)
{
    (void)signal_number;
    fake_child_running = 0;
}

static int write_fake_marker(const char *environment_name, int argc,
                             char **argv)
{
    const char *path = getenv(environment_name);
    const char *password_prompt = getenv(PASSWORD_PROMPT_ENVIRONMENT);
    char temporary_path[TEST_PATH_CAPACITY];
    FILE *stream;
    int index;
    int written;

    if (path == NULL || path[0] != '/' || password_prompt == NULL)
        return 2;
    written = snprintf(temporary_path, sizeof(temporary_path), "%s.%ld", path,
                       (long)getpid());
    if (written < 0 || (size_t)written >= sizeof(temporary_path))
        return 3;
    stream = fopen(temporary_path, "w");
    if (stream == NULL)
        return 4;
    for (index = 1; index < argc; ++index) {
        if (fprintf(stream, "%s\n", argv[index]) < 0) {
            (void)fclose(stream);
            (void)unlink(temporary_path);
            return 5;
        }
    }
    if (fprintf(stream, "%s=%s\n", PASSWORD_PROMPT_ENVIRONMENT,
                password_prompt) < 0) {
        (void)fclose(stream);
        (void)unlink(temporary_path);
        return 5;
    }
    if (fclose(stream) != 0) {
        (void)unlink(temporary_path);
        return 6;
    }
    if (rename(temporary_path, path) < 0) {
        (void)unlink(temporary_path);
        return 7;
    }
    return 0;
}

static int run_fake_child(const char *marker_environment, int argc, char **argv)
{
    struct sigaction action;
    int result;

    result = write_fake_marker(marker_environment, argc, argv);
    if (result != 0)
        return result;
    memset(&action, 0, sizeof(action));
    action.sa_handler = stop_fake_child;
    sigemptyset(&action.sa_mask);
    (void)sigaction(SIGHUP, &action, NULL);
    (void)sigaction(SIGINT, &action, NULL);
    (void)sigaction(SIGTERM, &action, NULL);
    fake_child_running = 1;
    while (fake_child_running)
        (void)pause();
    return 0;
}

static int format_test_path(char *destination, size_t capacity,
                            const char *directory, const char *leaf)
{
    int written = snprintf(destination, capacity, "%s/%s", directory, leaf);

    if (written < 0 || (size_t)written >= capacity)
        return -1;
    return 0;
}

static int initialize_fixture(TestFixture *fixture, const char *program)
{
    char resolved[TEST_PATH_CAPACITY];
    char directory_template[] = "/tmp/win31x-auto-lock.XXXXXX";

    memset(fixture, 0, sizeof(*fixture));
    if (realpath(program, resolved) == NULL)
        return -1;
    if (mkdtemp(directory_template) == NULL)
        return -1;
    if (snprintf(fixture->directory, sizeof(fixture->directory), "%s",
                 directory_template) < 0 ||
        format_test_path(fixture->supervisor, sizeof(fixture->supervisor),
                         fixture->directory, "fake-xss-lock") < 0 ||
        format_test_path(fixture->locker, sizeof(fixture->locker),
                         fixture->directory, "fake-locker") < 0 ||
        format_test_path(fixture->supervisor_marker,
                         sizeof(fixture->supervisor_marker), fixture->directory,
                         "supervisor.marker") < 0 ||
        format_test_path(fixture->locker_marker,
                         sizeof(fixture->locker_marker), fixture->directory,
                         "locker.marker") < 0) {
        (void)rmdir(directory_template);
        return -1;
    }
    if (symlink(resolved, fixture->supervisor) < 0 ||
        symlink(resolved, fixture->locker) < 0)
        return -1;
    if (setenv("WIN31X_XSS_LOCK", fixture->supervisor, 1) < 0 ||
        setenv("WIN31X_LOCKER", fixture->locker, 1) < 0 ||
        setenv("WIN31X_TEST_SUPERVISOR_MARKER", fixture->supervisor_marker,
               1) < 0 ||
        setenv("WIN31X_TEST_LOCKER_MARKER", fixture->locker_marker, 1) < 0 ||
        setenv(PASSWORD_PROMPT_ENVIRONMENT, "cursor", 1) < 0)
        return -1;
    return 0;
}

static void clean_fixture(const TestFixture *fixture)
{
    (void)unsetenv("WIN31X_XSS_LOCK");
    (void)unsetenv("WIN31X_LOCKER");
    (void)unsetenv("WIN31X_TEST_SUPERVISOR_MARKER");
    (void)unsetenv("WIN31X_TEST_LOCKER_MARKER");
    (void)unsetenv(PASSWORD_PROMPT_ENVIRONMENT);
    (void)unlink(fixture->supervisor_marker);
    (void)unlink(fixture->locker_marker);
    (void)unlink(fixture->supervisor);
    (void)unlink(fixture->locker);
    (void)rmdir(fixture->directory);
}

static bool wait_for_file(const char *path)
{
    struct timespec pause_time;
    unsigned int attempt;

    pause_time.tv_sec = 0;
    pause_time.tv_nsec = 10000000L;
    for (attempt = 0; attempt < 200U; ++attempt) {
        if (access(path, F_OK) == 0)
            return true;
        (void)nanosleep(&pause_time, NULL);
    }
    return false;
}

static bool marker_has_supervisor_arguments(const TestFixture *fixture)
{
    FILE *stream = fopen(fixture->supervisor_marker, "r");
    char first[32];
    char second[TEST_PATH_CAPACITY];
    bool matches = false;

    if (stream == NULL)
        return false;
    if (fgets(first, sizeof(first), stream) != NULL &&
        fgets(second, sizeof(second), stream) != NULL) {
        first[strcspn(first, "\r\n")] = '\0';
        second[strcspn(second, "\r\n")] = '\0';
        matches = strcmp(first, "--") == 0 &&
                  strcmp(second, fixture->locker) == 0;
    }
    (void)fclose(stream);
    return matches;
}

static bool marker_has_asterisk_password_prompt(const char *path)
{
    FILE *stream = fopen(path, "r");
    char line[128];
    bool matches = false;

    if (stream == NULL)
        return false;
    while (fgets(line, sizeof(line), stream) != NULL) {
        line[strcspn(line, "\r\n")] = '\0';
        if (strcmp(line, PASSWORD_PROMPT_ENVIRONMENT "="
                         PASSWORD_PROMPT_ASTERISKS) == 0) {
            matches = true;
            break;
        }
    }
    (void)fclose(stream);
    return matches;
}

static bool parent_password_prompt_is_cursor(void)
{
    const char *value = getenv(PASSWORD_PROMPT_ENVIRONMENT);

    return value != NULL && strcmp(value, "cursor") == 0;
}

static int stop_and_reap(pid_t pid)
{
    int wait_status = 0;
    pid_t result;

    (void)kill(pid, SIGTERM);
    do {
        result = waitpid(pid, &wait_status, 0);
    } while (result < 0 && errno == EINTR);
    CHECK(result == pid);
    return wait_status;
}

static void check_reaped(pid_t pid)
{
    int wait_status;

    errno = 0;
    CHECK(waitpid(pid, &wait_status, WNOHANG) == -1);
    CHECK(errno == ECHILD);
}

static void test_relative_override_is_rejected(const TestFixture *fixture)
{
    Win31xAutoLock lock;

    CHECK(setenv("WIN31X_XSS_LOCK", "relative-xss-lock", 1) == 0);
    CHECK(win31x_auto_lock_init(&lock, NULL, false, 10U) < 0);
    CHECK(lock.initialized);
    CHECK(!lock.available);
    CHECK(lock.locker_available);
    CHECK(lock.supervisor_pid == 0);
    CHECK(strstr(lock.status, "WIN31X_XSS_LOCK") != NULL);
    win31x_auto_lock_shutdown(&lock);
    CHECK(setenv("WIN31X_XSS_LOCK", fixture->supervisor, 1) == 0);
}

static void test_process_lifecycle(const TestFixture *fixture)
{
    Win31xAutoLock lock;
    pid_t direct_pid;
    pid_t supervisor_pid;
    int wait_status;

    (void)unlink(fixture->supervisor_marker);
    (void)unlink(fixture->locker_marker);
    CHECK(win31x_auto_lock_init(&lock, NULL, false, 10U) == 0);
    CHECK(parent_password_prompt_is_cursor());
    CHECK(wait_for_file(fixture->supervisor_marker));
    CHECK(marker_has_supervisor_arguments(fixture));
    CHECK(marker_has_asterisk_password_prompt(fixture->supervisor_marker));
    CHECK(lock.available);
    CHECK(lock.locker_available);
    CHECK(lock.supervisor_pid > 0);
    CHECK(strcmp(lock.provider, "fake-locker") == 0);
    CHECK(!lock.enabled);
    CHECK(lock.timeout_minutes == 10U);

    CHECK(win31x_auto_lock_configure(&lock, true, 0U) == 0);
    CHECK(lock.enabled);
    CHECK(lock.timeout_minutes == 1U);
    CHECK(win31x_auto_lock_set_timeout(&lock, 999U) == 0);
    CHECK(lock.timeout_minutes == 120U);
    CHECK(win31x_auto_lock_set_enabled(&lock, false) == 0);
    CHECK(!lock.enabled);

    CHECK(win31x_auto_lock_lock_now(&lock) == 0);
    CHECK(wait_for_file(fixture->locker_marker));
    CHECK(marker_has_asterisk_password_prompt(fixture->locker_marker));
    CHECK(parent_password_prompt_is_cursor());
    CHECK(lock.direct_pid > 0);
    errno = 0;
    CHECK(win31x_auto_lock_lock_now(&lock) < 0);
    CHECK(errno == EALREADY);
    direct_pid = lock.direct_pid;
    wait_status = stop_and_reap(direct_pid);
    CHECK(win31x_auto_lock_handle_child_exit(&lock, direct_pid, wait_status));
    CHECK(lock.direct_pid == 0);
    CHECK(lock.available);
    CHECK(!win31x_auto_lock_handle_child_exit(&lock, (pid_t)123456,
                                               wait_status));

    supervisor_pid = lock.supervisor_pid;
    wait_status = stop_and_reap(supervisor_pid);
    CHECK(win31x_auto_lock_handle_child_exit(&lock, supervisor_pid,
                                              wait_status));
    CHECK(lock.supervisor_pid == 0);
    CHECK(!lock.available);
    CHECK(strstr(lock.status, "xss-lock") != NULL);

    (void)unlink(fixture->supervisor_marker);
    CHECK(win31x_auto_lock_start_supervisor(&lock) == 0);
    CHECK(lock.available);
    CHECK(lock.supervisor_pid > 0);
    CHECK(wait_for_file(fixture->supervisor_marker));
    CHECK(marker_has_asterisk_password_prompt(fixture->supervisor_marker));
    win31x_auto_lock_shutdown(&lock);
    CHECK(lock.supervisor_pid == 0);
    CHECK(!lock.available);
}

static void test_shutdown_reaps_owned_children(const TestFixture *fixture)
{
    Win31xAutoLock lock;
    pid_t supervisor_pid;
    pid_t direct_pid;

    (void)unlink(fixture->supervisor_marker);
    (void)unlink(fixture->locker_marker);
    CHECK(win31x_auto_lock_init(&lock, NULL, true, 5U) == 0);
    CHECK(wait_for_file(fixture->supervisor_marker));
    CHECK(win31x_auto_lock_lock_now(&lock) == 0);
    CHECK(wait_for_file(fixture->locker_marker));
    supervisor_pid = lock.supervisor_pid;
    direct_pid = lock.direct_pid;
    win31x_auto_lock_shutdown(&lock);
    CHECK(lock.supervisor_pid == 0);
    CHECK(lock.direct_pid == 0);
    check_reaped(supervisor_pid);
    check_reaped(direct_pid);
}

static void test_screen_saver_apply_and_restore(const TestFixture *fixture)
{
    Display *display = XOpenDisplay(NULL);
    Win31xAutoLock lock;
    int original_timeout;
    int original_interval;
    int original_blanking;
    int original_exposures;
    int timeout;
    int interval;
    int blanking;
    int exposures;

    if (display == NULL) {
        puts("test-auto-lock: DISPLAY unavailable; skipped X server policy test");
        return;
    }
    CHECK(XGetScreenSaver(display, &original_timeout, &original_interval,
                          &original_blanking, &original_exposures) != 0);
    (void)unlink(fixture->supervisor_marker);
    CHECK(win31x_auto_lock_init(&lock, display, false, 10U) == 0);
    CHECK(wait_for_file(fixture->supervisor_marker));
    CHECK(XGetScreenSaver(display, &timeout, &interval, &blanking,
                          &exposures) != 0);
    CHECK(timeout == 0);
    CHECK(interval == original_interval);
    CHECK(blanking == original_blanking);
    CHECK(exposures == original_exposures);

    CHECK(win31x_auto_lock_set_timeout(&lock, 2U) == 0);
    CHECK(win31x_auto_lock_set_enabled(&lock, true) == 0);
    CHECK(XGetScreenSaver(display, &timeout, &interval, &blanking,
                          &exposures) != 0);
    CHECK(timeout == 120);
    CHECK(win31x_auto_lock_set_timeout(&lock, 0U) == 0);
    CHECK(XGetScreenSaver(display, &timeout, &interval, &blanking,
                          &exposures) != 0);
    CHECK(timeout == 60);

    win31x_auto_lock_shutdown(&lock);
    CHECK(XGetScreenSaver(display, &timeout, &interval, &blanking,
                          &exposures) != 0);
    CHECK(timeout == original_timeout);
    CHECK(interval == original_interval);
    CHECK(blanking == original_blanking);
    CHECK(exposures == original_exposures);
    XCloseDisplay(display);
}

int main(int argc, char **argv)
{
    const char *program_name = base_name(argv[0]);
    TestFixture fixture;

    if (strcmp(program_name, "fake-xss-lock") == 0)
        return run_fake_child("WIN31X_TEST_SUPERVISOR_MARKER", argc, argv);
    if (strcmp(program_name, "fake-locker") == 0 &&
        getenv("WIN31X_TEST_LOCKER_ONESHOT") != NULL)
        return write_fake_marker("WIN31X_TEST_LOCKER_MARKER", argc, argv);
    if (strcmp(program_name, "fake-locker") == 0)
        return run_fake_child("WIN31X_TEST_LOCKER_MARKER", argc, argv);

    if (initialize_fixture(&fixture, argv[0]) < 0) {
        fprintf(stderr, "test-auto-lock: could not create test fixture: %s\n",
                strerror(errno));
        return 1;
    }
    test_relative_override_is_rejected(&fixture);
    test_process_lifecycle(&fixture);
    test_shutdown_reaps_owned_children(&fixture);
    test_screen_saver_apply_and_restore(&fixture);
    clean_fixture(&fixture);

    if (failures != 0) {
        fprintf(stderr, "test-auto-lock: %d failure%s\n", failures,
                failures == 1 ? "" : "s");
        return 1;
    }
    puts("auto-lock tests passed");
    return 0;
}
