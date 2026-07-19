#define _DARWIN_C_SOURCE
#define _DEFAULT_SOURCE
#define _POSIX_C_SOURCE 200809L

#include "session_actions.h"

#include <errno.h>
#include <fcntl.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

#define TEST_PATH_CAPACITY 4096U
#define SYSTEMCTL_ENVIRONMENT "WIN31X_SYSTEMCTL"
#define MARKER_ENVIRONMENT "WIN31X_TEST_SYSTEM_ACTION_MARKER"
#define EXIT_STATUS_ENVIRONMENT "WIN31X_TEST_SYSTEM_ACTION_EXIT"

typedef struct {
    char directory[TEST_PATH_CAPACITY];
    char program[TEST_PATH_CAPACITY];
    char marker[TEST_PATH_CAPACITY];
    char non_executable[TEST_PATH_CAPACITY];
    char broken_interpreter[TEST_PATH_CAPACITY];
    char missing[TEST_PATH_CAPACITY];
} TestFixture;

static int failures;

#define CHECK(condition)                                                       \
    do {                                                                       \
        if (!(condition)) {                                                    \
            fprintf(stderr, "test-session-actions: check failed at %s:%d: "  \
                    "%s\n", __FILE__, __LINE__, #condition);                \
            ++failures;                                                        \
        }                                                                      \
    } while (0)

static const char *base_name(const char *path)
{
    const char *slash = strrchr(path, '/');

    return slash == NULL ? path : slash + 1;
}

static int format_test_path(char *destination, size_t capacity,
                            const char *directory, const char *leaf)
{
    int written = snprintf(destination, capacity, "%s/%s", directory, leaf);

    if (written < 0 || (size_t)written >= capacity) {
        errno = ENAMETOOLONG;
        return -1;
    }
    return 0;
}

static int write_file(const char *path, const char *contents, mode_t mode)
{
    int descriptor = open(path, O_WRONLY | O_CREAT | O_EXCL, mode);
    const char *cursor = contents;
    size_t remaining = strlen(contents);

    if (descriptor < 0)
        return -1;
    while (remaining > 0U) {
        ssize_t count = write(descriptor, cursor, remaining);

        if (count > 0) {
            cursor += count;
            remaining -= (size_t)count;
        } else if (count < 0 && errno == EINTR) {
            continue;
        } else {
            int saved_errno = count == 0 ? EIO : errno;

            (void)close(descriptor);
            (void)unlink(path);
            errno = saved_errno;
            return -1;
        }
    }
    if (close(descriptor) < 0) {
        int saved_errno = errno;

        (void)unlink(path);
        errno = saved_errno;
        return -1;
    }
    return 0;
}

static int initialize_fixture(TestFixture *fixture, const char *program)
{
    char resolved[TEST_PATH_CAPACITY];
    char directory_template[] = "/tmp/win31x-session-actions.XXXXXX";

    memset(fixture, 0, sizeof(*fixture));
    if (realpath(program, resolved) == NULL)
        return -1;
    if (mkdtemp(directory_template) == NULL)
        return -1;
    if (snprintf(fixture->directory, sizeof(fixture->directory), "%s",
                 directory_template) < 0 ||
        format_test_path(fixture->program, sizeof(fixture->program),
                         fixture->directory, "fake-systemctl") < 0 ||
        format_test_path(fixture->marker, sizeof(fixture->marker),
                         fixture->directory, "system-action.marker") < 0 ||
        format_test_path(fixture->non_executable,
                         sizeof(fixture->non_executable), fixture->directory,
                         "not-executable") < 0 ||
        format_test_path(fixture->broken_interpreter,
                         sizeof(fixture->broken_interpreter),
                         fixture->directory, "broken-interpreter") < 0 ||
        format_test_path(fixture->missing, sizeof(fixture->missing),
                         fixture->directory, "missing-systemctl") < 0)
        return -1;
    if (symlink(resolved, fixture->program) < 0 ||
        write_file(fixture->non_executable, "not executable\n", 0600) < 0 ||
        write_file(fixture->broken_interpreter,
                   "#!/definitely/missing/win31x-test-interpreter\n", 0700) < 0)
        return -1;
    if (setenv(SYSTEMCTL_ENVIRONMENT, fixture->program, 1) < 0 ||
        setenv(MARKER_ENVIRONMENT, fixture->marker, 1) < 0)
        return -1;
    return 0;
}

static void clean_fixture(const TestFixture *fixture)
{
    (void)unsetenv(SYSTEMCTL_ENVIRONMENT);
    (void)unsetenv(MARKER_ENVIRONMENT);
    (void)unsetenv(EXIT_STATUS_ENVIRONMENT);
    (void)unlink(fixture->marker);
    (void)unlink(fixture->program);
    (void)unlink(fixture->non_executable);
    (void)unlink(fixture->broken_interpreter);
    (void)rmdir(fixture->directory);
}

static int fake_systemctl(int argc, char **argv)
{
    const char *marker = getenv(MARKER_ENVIRONMENT);
    const char *configured_exit = getenv(EXIT_STATUS_ENVIRONMENT);
    char temporary[TEST_PATH_CAPACITY];
    FILE *stream;
    int index;
    int written;
    long exit_status = 0;

    if (marker == NULL || marker[0] != '/')
        return 120;
    written = snprintf(temporary, sizeof(temporary), "%s.%ld", marker,
                       (long)getpid());
    if (written < 0 || (size_t)written >= sizeof(temporary))
        return 121;
    stream = fopen(temporary, "w");
    if (stream == NULL)
        return 122;
    for (index = 1; index < argc; ++index) {
        if (fprintf(stream, "%s\n", argv[index]) < 0) {
            (void)fclose(stream);
            (void)unlink(temporary);
            return 123;
        }
    }
    if (fclose(stream) != 0) {
        (void)unlink(temporary);
        return 124;
    }
    if (rename(temporary, marker) < 0) {
        (void)unlink(temporary);
        return 125;
    }
    if (configured_exit != NULL && configured_exit[0] != '\0') {
        char *end = NULL;

        errno = 0;
        exit_status = strtol(configured_exit, &end, 10);
        if (errno != 0 || end == configured_exit || *end != '\0' ||
            exit_status < 0 || exit_status > 255)
            return 126;
    }
    return (int)exit_status;
}

static int reap_child(pid_t pid)
{
    int wait_status = 0;
    pid_t result;

    do {
        result = waitpid(pid, &wait_status, 0);
    } while (result < 0 && errno == EINTR);
    CHECK(result == pid);
    return wait_status;
}

static bool marker_equals(const char *path, const char *expected)
{
    FILE *stream = fopen(path, "r");
    char contents[128];
    size_t count;
    bool matches;

    if (stream == NULL)
        return false;
    count = fread(contents, 1U, sizeof(contents) - 1U, stream);
    matches = !ferror(stream) && feof(stream);
    contents[count] = '\0';
    if (fclose(stream) != 0)
        matches = false;
    return matches && strcmp(contents, expected) == 0;
}

static void test_invalid_parameters(const TestFixture *fixture)
{
    Win31xSessionActions actions;

    errno = 0;
    CHECK(win31x_session_actions_init(NULL) < 0);
    CHECK(errno == EINVAL);

    memset(&actions, 0, sizeof(actions));
    errno = 0;
    CHECK(win31x_session_action_start(NULL,
                                      WIN31X_SESSION_ACTION_RESTART) < 0);
    CHECK(errno == ENOTSUP);
    errno = 0;
    CHECK(win31x_session_action_start(&actions,
                                      WIN31X_SESSION_ACTION_RESTART) < 0);
    CHECK(errno == ENOTSUP);

    CHECK(setenv(SYSTEMCTL_ENVIRONMENT, fixture->program, 1) == 0);
    CHECK(win31x_session_actions_init(&actions) == 0);
    errno = 0;
    CHECK(win31x_session_action_start(&actions,
                                      (Win31xSessionAction)999) < 0);
    CHECK(errno == EINVAL);
    CHECK(actions.child_pid == 0);
    CHECK(!win31x_session_actions_handle_child_exit(NULL, 1, 0));
    CHECK(!win31x_session_actions_handle_child_exit(&actions, 0, 0));
}

static void check_rejected_override(const char *path, int expected_errno)
{
    Win31xSessionActions actions;

    CHECK(setenv(SYSTEMCTL_ENVIRONMENT, path, 1) == 0);
    errno = 0;
    CHECK(win31x_session_actions_init(&actions) < 0);
    CHECK(errno == expected_errno);
    CHECK(!actions.available);
    CHECK(actions.child_pid == 0);
    CHECK(actions.program[0] == '\0');
    CHECK(strstr(actions.status, SYSTEMCTL_ENVIRONMENT) != NULL);
}

static void test_invalid_overrides(const TestFixture *fixture)
{
    char *long_path = malloc(WIN31X_SESSION_ACTION_PATH_CAPACITY + 1U);

    check_rejected_override("relative-systemctl", EINVAL);
    check_rejected_override(fixture->directory, EACCES);
    check_rejected_override(fixture->non_executable, EACCES);
    check_rejected_override(fixture->missing, ENOENT);
    CHECK(long_path != NULL);
    if (long_path != NULL) {
        long_path[0] = '/';
        memset(long_path + 1, 'x', WIN31X_SESSION_ACTION_PATH_CAPACITY - 1U);
        long_path[WIN31X_SESSION_ACTION_PATH_CAPACITY] = '\0';
        check_rejected_override(long_path, ENAMETOOLONG);
        free(long_path);
    }
    CHECK(setenv(SYSTEMCTL_ENVIRONMENT, fixture->program, 1) == 0);
}

static void test_successful_actions(const TestFixture *fixture)
{
    Win31xSessionActions actions;
    pid_t pid;
    int wait_status;

    CHECK(setenv(SYSTEMCTL_ENVIRONMENT, fixture->program, 1) == 0);
    CHECK(win31x_session_actions_init(&actions) == 0);
    CHECK(actions.available);
    CHECK(strcmp(actions.program, fixture->program) == 0);
    CHECK(strcmp(actions.status, "Ready") == 0);

    (void)unlink(fixture->marker);
    pid = win31x_session_action_start(&actions,
                                      WIN31X_SESSION_ACTION_RESTART);
    CHECK(pid > 0);
    CHECK(actions.child_pid == pid);
    CHECK(strcmp(actions.status, "reboot requested") == 0);
    errno = 0;
    CHECK(win31x_session_action_start(&actions,
                                      WIN31X_SESSION_ACTION_SHUT_DOWN) < 0);
    CHECK(errno == EBUSY);
    CHECK(actions.child_pid == pid);
    wait_status = reap_child(pid);
    CHECK(WIFEXITED(wait_status));
    if (WIFEXITED(wait_status))
        CHECK(WEXITSTATUS(wait_status) == 0);
    CHECK(marker_equals(fixture->marker, "reboot\n"));
    CHECK(!win31x_session_actions_handle_child_exit(&actions, pid + 1,
                                                     wait_status));
    CHECK(actions.child_pid == pid);
    CHECK(win31x_session_actions_handle_child_exit(&actions, pid,
                                                    wait_status));
    CHECK(actions.child_pid == 0);
    CHECK(strcmp(actions.status, "Ready") == 0);
    CHECK(!win31x_session_actions_handle_child_exit(&actions, pid,
                                                     wait_status));

    (void)unlink(fixture->marker);
    pid = win31x_session_action_start(&actions,
                                      WIN31X_SESSION_ACTION_SHUT_DOWN);
    CHECK(pid > 0);
    CHECK(actions.child_pid == pid);
    CHECK(strcmp(actions.status, "poweroff requested") == 0);
    wait_status = reap_child(pid);
    CHECK(WIFEXITED(wait_status));
    if (WIFEXITED(wait_status))
        CHECK(WEXITSTATUS(wait_status) == 0);
    CHECK(marker_equals(fixture->marker, "poweroff\n"));
    CHECK(win31x_session_actions_handle_child_exit(&actions, pid,
                                                    wait_status));
    CHECK(actions.child_pid == 0);
    CHECK(strcmp(actions.status, "Ready") == 0);
}

static void test_failed_child(const TestFixture *fixture)
{
    Win31xSessionActions actions;
    pid_t pid;
    int wait_status;

    CHECK(setenv(SYSTEMCTL_ENVIRONMENT, fixture->program, 1) == 0);
    CHECK(setenv(EXIT_STATUS_ENVIRONMENT, "7", 1) == 0);
    CHECK(win31x_session_actions_init(&actions) == 0);
    (void)unlink(fixture->marker);
    pid = win31x_session_action_start(&actions,
                                      WIN31X_SESSION_ACTION_RESTART);
    CHECK(pid > 0);
    wait_status = reap_child(pid);
    CHECK(WIFEXITED(wait_status));
    if (WIFEXITED(wait_status))
        CHECK(WEXITSTATUS(wait_status) == 7);
    CHECK(marker_equals(fixture->marker, "reboot\n"));
    CHECK(win31x_session_actions_handle_child_exit(&actions, pid,
                                                    wait_status));
    CHECK(actions.child_pid == 0);
    CHECK(strcmp(actions.status, "The system action failed") == 0);
    CHECK(unsetenv(EXIT_STATUS_ENVIRONMENT) == 0);
}

static void test_exec_failure(const TestFixture *fixture)
{
    Win31xSessionActions actions;

    CHECK(setenv(SYSTEMCTL_ENVIRONMENT, fixture->broken_interpreter, 1) == 0);
    CHECK(win31x_session_actions_init(&actions) == 0);
    CHECK(actions.available);
    errno = 0;
    CHECK(win31x_session_action_start(&actions,
                                      WIN31X_SESSION_ACTION_RESTART) < 0);
    CHECK(errno == ENOENT);
    CHECK(actions.child_pid == 0);
    CHECK(setenv(SYSTEMCTL_ENVIRONMENT, fixture->program, 1) == 0);
}

int main(int argc, char **argv)
{
    TestFixture fixture;

    if (strcmp(base_name(argv[0]), "fake-systemctl") == 0)
        return fake_systemctl(argc, argv);
    if (initialize_fixture(&fixture, argv[0]) < 0) {
        fprintf(stderr, "test-session-actions: fixture setup failed: %s\n",
                strerror(errno));
        return 2;
    }
    test_invalid_parameters(&fixture);
    test_invalid_overrides(&fixture);
    test_successful_actions(&fixture);
    test_failed_child(&fixture);
    test_exec_failure(&fixture);
    clean_fixture(&fixture);

    if (failures != 0) {
        fprintf(stderr, "test-session-actions: %d failure%s\n", failures,
                failures == 1 ? "" : "s");
        return 1;
    }
    puts("session action tests passed");
    return 0;
}
