#define _DARWIN_C_SOURCE
#define _POSIX_C_SOURCE 200809L

#include "task_manager_data.h"

#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <signal.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/resource.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

#define TEST_PATH_CAPACITY 4096U

typedef struct {
    char directory[TEST_PATH_CAPACITY];
    char proc_root[TEST_PATH_CAPACITY];
    char os_release[TEST_PATH_CAPACITY];
} TestFixture;

static int failures;

#define CHECK(condition)                                                       \
    do {                                                                       \
        if (!(condition)) {                                                    \
            fprintf(stderr,                                                    \
                    "test-task-manager-data: check failed at %s:%d: %s\n",   \
                    __FILE__, __LINE__, #condition);                           \
            ++failures;                                                        \
        }                                                                      \
    } while (0)

static int path_join(char *destination, size_t capacity, const char *left,
                     const char *right)
{
    int written = snprintf(destination, capacity, "%s/%s", left, right);

    return written < 0 || (size_t)written >= capacity ? -1 : 0;
}

static int write_bytes(const char *path, const void *contents, size_t length)
{
    const unsigned char *cursor = contents;
    size_t written = 0U;
    int descriptor = open(path, O_WRONLY | O_CREAT | O_TRUNC, 0600);

    if (descriptor < 0)
        return -1;
    while (written < length) {
        ssize_t count = write(descriptor, cursor + written, length - written);

        if (count < 0) {
            int saved_errno;

            if (errno == EINTR)
                continue;
            saved_errno = errno;
            (void)close(descriptor);
            errno = saved_errno;
            return -1;
        }
        written += (size_t)count;
    }
    return close(descriptor);
}

static int write_text(const char *path, const char *contents)
{
    return write_bytes(path, contents, strlen(contents));
}

static int make_directory(const char *path)
{
    return mkdir(path, 0700) == 0 || errno == EEXIST ? 0 : -1;
}

static int create_fixture(TestFixture *fixture)
{
    char template[] = "/tmp/win31x-task-manager.XXXXXX";
    char path[TEST_PATH_CAPACITY];

    memset(fixture, 0, sizeof(*fixture));
    if (mkdtemp(template) == NULL)
        return -1;
    if (snprintf(fixture->directory, sizeof(fixture->directory), "%s",
                 template) < 0 ||
        path_join(fixture->proc_root, sizeof(fixture->proc_root),
                  fixture->directory, "proc") < 0 ||
        path_join(fixture->os_release, sizeof(fixture->os_release),
                  fixture->directory, "os-release") < 0 ||
        make_directory(fixture->proc_root) < 0 ||
        path_join(path, sizeof(path), fixture->proc_root, "sys") < 0 ||
        make_directory(path) < 0 ||
        path_join(path, sizeof(path), fixture->proc_root, "sys/kernel") < 0 ||
        make_directory(path) < 0)
        return -1;
    return 0;
}

static int write_process_stat(const TestFixture *fixture, pid_t pid,
                              const char *name, uint64_t user_ticks,
                              uint64_t system_ticks, uint64_t start_time,
                              int64_t resident_pages)
{
    char leaf[64];
    char directory[TEST_PATH_CAPACITY];
    char path[TEST_PATH_CAPACITY];
    char contents[2048];
    int written;

    written = snprintf(leaf, sizeof(leaf), "%ld", (long)pid);
    if (written < 0 || (size_t)written >= sizeof(leaf) ||
        path_join(directory, sizeof(directory), fixture->proc_root, leaf) < 0 ||
        make_directory(directory) < 0 ||
        path_join(path, sizeof(path), directory, "stat") < 0)
        return -1;
    written = snprintf(
        contents, sizeof(contents),
        "%ld (%s) R 1 2 3 4 5 6 7 8 9 10 %" PRIu64 " %" PRIu64
        " 16 17 18 19 20 21 %" PRIu64 " 23 %" PRId64 "\n",
        (long)pid, name, user_ticks, system_ticks, start_time, resident_pages);
    if (written < 0 || (size_t)written >= sizeof(contents))
        return -1;
    return write_text(path, contents);
}

static int write_process_status(const TestFixture *fixture, pid_t pid,
                                uid_t uid)
{
    char leaf[64];
    char directory[TEST_PATH_CAPACITY];
    char path[TEST_PATH_CAPACITY];
    char contents[512];
    int written;

    written = snprintf(leaf, sizeof(leaf), "%ld", (long)pid);
    if (written < 0 || (size_t)written >= sizeof(leaf) ||
        path_join(directory, sizeof(directory), fixture->proc_root, leaf) < 0 ||
        make_directory(directory) < 0 ||
        path_join(path, sizeof(path), directory, "status") < 0)
        return -1;
    written = snprintf(contents, sizeof(contents),
                       "Name:\tworker\nState:\tR (running)\n"
                       "Uid:\t%ju\t%ju\t%ju\t%ju\n",
                       (uintmax_t)uid, (uintmax_t)uid, (uintmax_t)uid,
                       (uintmax_t)uid);
    if (written < 0 || (size_t)written >= sizeof(contents))
        return -1;
    return write_text(path, contents);
}

static int create_process(const TestFixture *fixture, pid_t pid,
                          const char *name, uid_t uid, uint64_t user_ticks,
                          uint64_t system_ticks, uint64_t start_time,
                          int64_t resident_pages)
{
    return write_process_stat(fixture, pid, name, user_ticks, system_ticks,
                              start_time, resident_pages) == 0 &&
                   write_process_status(fixture, pid, uid) == 0
               ? 0
               : -1;
}

static int write_ignored_process_files(const TestFixture *fixture, pid_t pid)
{
    static const unsigned char command[] = "/must/not/be/read\0--ignored\0";
    char leaf[64];
    char directory[TEST_PATH_CAPACITY];
    char path[TEST_PATH_CAPACITY];
    int written;

    written = snprintf(leaf, sizeof(leaf), "%ld", (long)pid);
    if (written < 0 || (size_t)written >= sizeof(leaf) ||
        path_join(directory, sizeof(directory), fixture->proc_root, leaf) < 0 ||
        path_join(path, sizeof(path), directory, "comm") < 0 ||
        write_text(path, "must not be read\n") < 0 ||
        path_join(path, sizeof(path), directory, "cmdline") < 0)
        return -1;
    return write_bytes(path, command, sizeof(command) - 1U);
}

static int write_machine_files(const TestFixture *fixture)
{
    char path[TEST_PATH_CAPACITY];
    static const char cpuinfo[] =
        "processor\t: 0\n"
        "model name\t: TestChip 486DX\n"
        "processor\t: 1\n"
        "model name\t: TestChip 486DX\n";

    if (path_join(path, sizeof(path), fixture->proc_root, "stat") < 0 ||
        write_text(path, "cpu 100 0 100 700 50 0 0 0 10 5\n") < 0 ||
        path_join(path, sizeof(path), fixture->proc_root, "meminfo") < 0 ||
        write_text(path,
                   "MemTotal:       100000 kB\n"
                   "MemFree:         10000 kB\n"
                   "MemAvailable:    40000 kB\n"
                   "Buffers:          2000 kB\n"
                   "Cached:           8000 kB\n") < 0 ||
        path_join(path, sizeof(path), fixture->proc_root, "uptime") < 0 ||
        write_text(path, "1234.50 321.00\n") < 0 ||
        path_join(path, sizeof(path), fixture->proc_root, "loadavg") < 0 ||
        write_text(path, "0.25 0.50 0.75 1/20 42\n") < 0 ||
        path_join(path, sizeof(path), fixture->proc_root, "cpuinfo") < 0 ||
        write_text(path, cpuinfo) < 0 ||
        path_join(path, sizeof(path), fixture->proc_root,
                  "sys/kernel/hostname") < 0 ||
        write_text(path, "test-box\n") < 0 ||
        path_join(path, sizeof(path), fixture->proc_root,
                  "sys/kernel/osrelease") < 0 ||
        write_text(path, "6.12.34-test\n") < 0 ||
        write_text(fixture->os_release,
                   "NAME=TestOS\nPRETTY_NAME=\"Test OS 3.1\"\n") < 0)
        return -1;
    return 0;
}

static const Win31xTaskManagerProcess *find_process(
    const Win31xTaskManagerSnapshot *snapshot, pid_t pid)
{
    size_t index;

    for (index = 0U; index < snapshot->process_count; ++index) {
        if (snapshot->processes[index].pid == pid)
            return &snapshot->processes[index];
    }
    return NULL;
}

static void test_collection_resource_failure(
    Win31xTaskManagerData *data,
    const Win31xTaskManagerProcess *expected_processes,
    size_t expected_process_count, double expected_cpu_percent)
{
    struct rlimit original_limit;
    struct rlimit limited;
    int descriptors[128];
    size_t descriptor_count = 0U;
    bool reached_limit = false;

    if (getrlimit(RLIMIT_NOFILE, &original_limit) < 0)
        return;
    limited = original_limit;
    if (limited.rlim_cur == RLIM_INFINITY || limited.rlim_cur > 64U)
        limited.rlim_cur = 64U;
    if (limited.rlim_cur < 8U || setrlimit(RLIMIT_NOFILE, &limited) < 0)
        return;

    while (descriptor_count < sizeof(descriptors) / sizeof(descriptors[0])) {
        int descriptor = open("/dev/null", O_RDONLY);

        if (descriptor < 0) {
            reached_limit = errno == EMFILE;
            break;
        }
        descriptors[descriptor_count++] = descriptor;
    }
    if (reached_limit && descriptor_count > 0U) {
        const Win31xTaskManagerSnapshot *snapshot;

        /* Leave exactly one descriptor for each sequential aggregate read.
         * opendir() consumes it, so the first per-process read sees EMFILE. */
        (void)close(descriptors[--descriptor_count]);
        errno = 0;
        CHECK(win31x_task_manager_data_refresh(data) < 0);
        CHECK(errno == EMFILE);
        snapshot = win31x_task_manager_data_snapshot(data);
        CHECK(snapshot != NULL);
        if (snapshot != NULL) {
            CHECK(snapshot->processes == expected_processes);
            CHECK(snapshot->process_count == expected_process_count);
            CHECK(snapshot->system.cpu_percent == expected_cpu_percent);
        }
    }
    while (descriptor_count > 0U)
        (void)close(descriptors[--descriptor_count]);
    CHECK(setrlimit(RLIMIT_NOFILE, &original_limit) == 0);
}

static void unlink_process_files(const TestFixture *fixture, pid_t pid)
{
    static const char *const leaves[] = {"stat", "status", "comm", "cmdline"};
    char pid_leaf[64];
    char directory[TEST_PATH_CAPACITY];
    char path[TEST_PATH_CAPACITY];
    size_t index;

    (void)snprintf(pid_leaf, sizeof(pid_leaf), "%ld", (long)pid);
    if (path_join(directory, sizeof(directory), fixture->proc_root, pid_leaf) < 0)
        return;
    for (index = 0U; index < sizeof(leaves) / sizeof(leaves[0]); ++index) {
        if (path_join(path, sizeof(path), directory, leaves[index]) == 0)
            (void)unlink(path);
    }
    (void)rmdir(directory);
}

static void destroy_fixture(const TestFixture *fixture)
{
    static const char *const proc_files[] = {
        "stat", "meminfo", "uptime", "loadavg", "cpuinfo",
        "sys/kernel/hostname", "sys/kernel/osrelease"
    };
    char path[TEST_PATH_CAPACITY];
    size_t index;

    unlink_process_files(fixture, 123);
    unlink_process_files(fixture, 456);
    unlink_process_files(fixture, 789);
    for (index = 0U; index < sizeof(proc_files) / sizeof(proc_files[0]);
         ++index) {
        if (path_join(path, sizeof(path), fixture->proc_root,
                      proc_files[index]) == 0)
            (void)unlink(path);
    }
    if (path_join(path, sizeof(path), fixture->proc_root, "sys/kernel") == 0)
        (void)rmdir(path);
    if (path_join(path, sizeof(path), fixture->proc_root, "sys") == 0)
        (void)rmdir(path);
    (void)rmdir(fixture->proc_root);
    (void)unlink(fixture->os_release);
    (void)rmdir(fixture->directory);
}

static void test_sampling(TestFixture *fixture)
{
    Win31xTaskManagerData data;
    const Win31xTaskManagerSnapshot *snapshot;
    const Win31xTaskManagerProcess *worker;
    const Win31xTaskManagerProcess *other;
    const Win31xTaskManagerProcess *previous_processes;
    char path[TEST_PATH_CAPACITY];
    uid_t foreign_uid = getuid() == (uid_t)-1 ? getuid() - 1U : getuid() + 1U;
    size_t previous_count;

    CHECK(write_machine_files(fixture) == 0);
    CHECK(create_process(fixture, 123, "fallback ) worker", getuid(), 20U,
                         30U, 400U, 25) == 0);
    CHECK(write_ignored_process_files(fixture, 123) == 0);
    CHECK(create_process(fixture, 456, "other", foreign_uid, 5U, 5U, 500U,
                         10) == 0);

    /* A malformed and partially vanished proc entry must not break refresh. */
    CHECK(write_process_stat(fixture, 789, "broken", 1U, 1U, 600U, 1) == 0);
    CHECK(path_join(path, sizeof(path), fixture->proc_root, "789/stat") == 0);
    CHECK(write_text(path, "789 (unterminated R 1 2 3\n") == 0);

    CHECK(win31x_task_manager_data_init_at(&data, fixture->proc_root,
                                           fixture->os_release) == 0);
    CHECK(win31x_task_manager_data_refresh(&data) == 0);
    snapshot = win31x_task_manager_data_snapshot(&data);
    CHECK(snapshot != NULL);
    if (snapshot == NULL) {
        win31x_task_manager_data_destroy(&data);
        return;
    }
    CHECK(snapshot->process_count == 2U);
    CHECK(!snapshot->process_list_truncated);
    CHECK(!snapshot->system.cpu_percent_valid);
    CHECK(snapshot->system.memory_total_bytes == 100000U * 1024U);
    CHECK(snapshot->system.memory_available_bytes == 40000U * 1024U);
    CHECK(snapshot->system.uptime_seconds == 1234.5);
    CHECK(snapshot->system.load_average[0] == 0.25);
    CHECK(snapshot->system.load_average[1] == 0.50);
    CHECK(snapshot->system.load_average[2] == 0.75);
    CHECK(strcmp(snapshot->system.operating_system, "Test OS 3.1") == 0);
    CHECK(strcmp(snapshot->system.kernel, "6.12.34-test") == 0);
    CHECK(strcmp(snapshot->system.hostname, "test-box") == 0);
    CHECK(strcmp(snapshot->system.cpu_model, "TestChip 486DX") == 0);
    CHECK(snapshot->system.cpu_core_count == 2U);

    worker = find_process(snapshot, 123);
    other = find_process(snapshot, 456);
    CHECK(worker != NULL);
    CHECK(other != NULL);
    if (worker != NULL) {
        CHECK(worker->owned_by_user);
        CHECK(worker->uid == getuid());
        CHECK(worker->start_time_ticks == 400U);
        CHECK(worker->state == 'R');
        CHECK(strcmp(worker->name, "fallback ) worker") == 0);
        CHECK(strcmp(worker->command, "fallback ) worker") == 0);
        CHECK(worker->resident_bytes == 25U * (uint64_t)data.page_size);
        CHECK(!worker->cpu_percent_valid);
    }
    if (other != NULL)
        CHECK(!other->owned_by_user);

    CHECK(path_join(path, sizeof(path), fixture->proc_root, "stat") == 0);
    /* guest deltas are already part of user/nice and must not be double-counted. */
    CHECK(write_text(path, "cpu 140 0 110 740 60 0 0 0 90 45\n") == 0);
    CHECK(write_process_stat(fixture, 123, "fallback ) worker", 35U, 35U,
                             400U, 30) == 0);
    /* Same PID but new start time: it must not inherit old CPU usage. */
    CHECK(write_process_stat(fixture, 456, "other", 40U, 20U, 999U, 10) == 0);
    CHECK(path_join(path, sizeof(path), fixture->proc_root, "meminfo") == 0);
    CHECK(write_text(path,
                     "MemTotal: 100000 kB\nMemFree: 10000 kB\n"
                     "Buffers: 5000 kB\nCached: 15000 kB\n") == 0);
    CHECK(win31x_task_manager_data_refresh(&data) == 0);
    snapshot = win31x_task_manager_data_snapshot(&data);
    CHECK(snapshot != NULL);
    if (snapshot == NULL) {
        win31x_task_manager_data_destroy(&data);
        return;
    }
    CHECK(snapshot->system.cpu_percent_valid);
    CHECK(snapshot->system.cpu_percent == 50.0);
    CHECK(snapshot->system.memory_available_bytes == 30000U * 1024U);
    worker = find_process(snapshot, 123);
    other = find_process(snapshot, 456);
    CHECK(worker != NULL && worker->cpu_percent_valid);
    if (worker != NULL)
        CHECK(worker->cpu_percent == 20.0);
    CHECK(other != NULL && !other->cpu_percent_valid);

    /* Critical malformed aggregate data fails atomically. */
    previous_count = snapshot->process_count;
    previous_processes = snapshot->processes;
    {
        static const char *const malformed_meminfo[] = {
            "MemTotal: -1 kB\nMemAvailable: 1 kB\n",
            "MemTotal: 18014398509481984 kB\nMemAvailable: 1 kB\n",
            "MemTotal: 100000 MB\nMemAvailable: 1 kB\n",
            "MemTotal: 100000 kB trailing\nMemAvailable: 1 kB\n",
            "MemTotal: 100000 kB\nMemAvailable: 40000 kB trailing\n",
            "MemFree: not-a-number\n"
        };
        size_t invalid_index;

        for (invalid_index = 0U;
             invalid_index < sizeof(malformed_meminfo) /
                                     sizeof(malformed_meminfo[0]);
             ++invalid_index) {
            CHECK(write_text(path, malformed_meminfo[invalid_index]) == 0);
            errno = 0;
            CHECK(win31x_task_manager_data_refresh(&data) < 0);
            CHECK(errno == EPROTO);
            snapshot = win31x_task_manager_data_snapshot(&data);
            CHECK(snapshot != NULL);
            if (snapshot != NULL) {
                CHECK(snapshot->processes == previous_processes);
                CHECK(snapshot->process_count == previous_count);
                CHECK(snapshot->system.cpu_percent == 50.0);
            }
        }
    }

    CHECK(write_text(path,
                     "MemTotal: 100000 kB\nMemFree: 10000 kB\n"
                     "Buffers: 5000 kB\nCached: 15000 kB\n") == 0);
    test_collection_resource_failure(&data, previous_processes,
                                     previous_count, 50.0);

    win31x_task_manager_data_destroy(&data);
    CHECK(win31x_task_manager_data_snapshot(&data) == NULL);
}

static void test_process_limit(TestFixture *fixture)
{
    Win31xTaskManagerData data;
    const Win31xTaskManagerSnapshot *snapshot;

    CHECK(win31x_task_manager_data_init_at(&data, fixture->proc_root,
                                           fixture->os_release) == 0);
    CHECK(data.process_limit == WIN31X_TASK_MANAGER_PROCESS_LIMIT);
    /* The field is private implementation state; lowering it here keeps this
     * regression fast while exercising the production early-stop branch. */
    data.process_limit = 1U;
    CHECK(win31x_task_manager_data_refresh(&data) == 0);
    snapshot = win31x_task_manager_data_snapshot(&data);
    CHECK(snapshot != NULL);
    if (snapshot != NULL) {
        CHECK(snapshot->process_count == 1U);
        CHECK(snapshot->process_list_truncated);
        if (snapshot->process_count == 1U) {
            CHECK(snapshot->processes[0].name[0] != '\0');
            CHECK(strcmp(snapshot->processes[0].command,
                         snapshot->processes[0].name) == 0);
        }
    }
    win31x_task_manager_data_destroy(&data);
}

static void test_termination(TestFixture *fixture)
{
    Win31xTaskManagerData data;
    char error[WIN31X_TASK_MANAGER_ERROR_CAPACITY];
    pid_t child;
    int status = 0;

    CHECK(win31x_task_manager_data_init_at(&data, fixture->proc_root,
                                           fixture->os_release) == 0);
    errno = 0;
    CHECK(win31x_task_manager_data_terminate(&data, 1, 1U, error,
                                             sizeof(error)) < 0);
    CHECK(errno == EPERM);
    CHECK(strstr(error, "cannot be ended") != NULL);
    errno = 0;
    CHECK(win31x_task_manager_data_terminate(&data, getpid(), 1U, error,
                                             sizeof(error)) < 0);
    CHECK(errno == EPERM);
    CHECK(strstr(error, "window manager") != NULL);

    child = fork();
    CHECK(child >= 0);
    if (child == 0) {
        for (;;)
            (void)pause();
    }
    if (child < 0) {
        win31x_task_manager_data_destroy(&data);
        return;
    }

    /* Alternate/fake proc roots are sampling-only and must never be used to
     * validate a signal sent to a real process with the same numeric PID. */
    errno = 0;
    CHECK(win31x_task_manager_data_terminate(&data, child, 777U, error,
                                             sizeof(error)) < 0);
    CHECK(errno == ENOTSUP);
    CHECK(strstr(error, "system /proc") != NULL);
    CHECK(kill(child, 0) == 0);
    errno = 0;
    CHECK(win31x_task_manager_data_force_terminate(
              &data, child, 777U, error, sizeof(error)) < 0);
    CHECK(errno == ENOTSUP);
    CHECK(strstr(error, "system /proc") != NULL);
    CHECK(kill(child, 0) == 0);

    (void)kill(child, SIGKILL);
    CHECK(waitpid(child, &status, 0) == child);
    CHECK(WIFSIGNALED(status));
    CHECK(WTERMSIG(status) == SIGKILL);
    win31x_task_manager_data_destroy(&data);
    /* Reinitialization is supported after, but only after, destroy. */
    CHECK(win31x_task_manager_data_init_at(&data, fixture->proc_root,
                                           fixture->os_release) == 0);
    win31x_task_manager_data_destroy(&data);
}

static void test_live_signal_when_available(bool force)
{
    Win31xTaskManagerData data;
    const Win31xTaskManagerSnapshot *snapshot;
    const Win31xTaskManagerProcess *process;
    char error[WIN31X_TASK_MANAGER_ERROR_CAPACITY];
    pid_t child;
    int status = 0;
    int result;

    if (access("/proc/stat", R_OK) != 0)
        return;
    CHECK(win31x_task_manager_data_init(&data) == 0);
    child = fork();
    CHECK(child >= 0);
    if (child == 0) {
        for (;;)
            (void)pause();
    }
    if (child < 0) {
        win31x_task_manager_data_destroy(&data);
        return;
    }
    CHECK(win31x_task_manager_data_refresh(&data) == 0);
    snapshot = win31x_task_manager_data_snapshot(&data);
    process = snapshot == NULL ? NULL : find_process(snapshot, child);
    CHECK(process != NULL);
    if (process == NULL) {
        (void)kill(child, SIGKILL);
        (void)waitpid(child, &status, 0);
        win31x_task_manager_data_destroy(&data);
        return;
    }

    errno = 0;
    result = force
                 ? win31x_task_manager_data_force_terminate(
                       &data, child, process->start_time_ticks ^ UINT64_C(1),
                       error, sizeof(error))
                 : win31x_task_manager_data_terminate(
                       &data, child, process->start_time_ticks ^ UINT64_C(1),
                       error, sizeof(error));
    CHECK(result < 0);
    if (errno == ENOTSUP || errno == EPERM || errno == EACCES) {
        if (errno == ENOTSUP)
            CHECK(strstr(error, "pidfd support") != NULL);
        CHECK(kill(child, 0) == 0);
        (void)kill(child, SIGKILL);
        CHECK(waitpid(child, &status, 0) == child);
        win31x_task_manager_data_destroy(&data);
        return;
    }
    CHECK(errno == ESRCH);
    CHECK(strstr(error, "changed") != NULL);
    CHECK(kill(child, 0) == 0);

    result = force ? win31x_task_manager_data_force_terminate(
                         &data, child, process->start_time_ticks, error,
                         sizeof(error))
                   : win31x_task_manager_data_terminate(
                         &data, child, process->start_time_ticks, error,
                         sizeof(error));
    CHECK(result == 0);
    CHECK(error[0] == '\0');
    if (result < 0)
        (void)kill(child, SIGKILL);
    CHECK(waitpid(child, &status, 0) == child);
    CHECK(WIFSIGNALED(status));
    if (WIFSIGNALED(status))
        CHECK(WTERMSIG(status) == (force ? SIGKILL : SIGTERM));
    win31x_task_manager_data_destroy(&data);
}

static void test_live_proc_when_available(void)
{
    Win31xTaskManagerData data;
    const Win31xTaskManagerSnapshot *snapshot;

    if (access("/proc/stat", R_OK) != 0)
        return;
    CHECK(win31x_task_manager_data_init(&data) == 0);
    CHECK(win31x_task_manager_data_refresh(&data) == 0);
    snapshot = win31x_task_manager_data_snapshot(&data);
    CHECK(snapshot != NULL);
    if (snapshot != NULL) {
        CHECK(snapshot->process_count > 0U);
        CHECK(snapshot->system.memory_total_bytes > 0U);
        CHECK(snapshot->system.cpu_core_count > 0U);
        CHECK(snapshot->system.hostname[0] != '\0');
        CHECK(find_process(snapshot, getpid()) != NULL);
    }
    win31x_task_manager_data_destroy(&data);
}

int main(void)
{
    TestFixture fixture;

    if (create_fixture(&fixture) < 0) {
        perror("test-task-manager-data: fixture");
        return 2;
    }
    test_sampling(&fixture);
    test_process_limit(&fixture);
    test_termination(&fixture);
    test_live_proc_when_available();
    test_live_signal_when_available(false);
    test_live_signal_when_available(true);
    destroy_fixture(&fixture);
    if (failures != 0) {
        fprintf(stderr, "task-manager-data tests failed: %d\n", failures);
        return 1;
    }
    puts("task-manager-data tests passed");
    return 0;
}
