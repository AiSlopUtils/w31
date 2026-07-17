#define _POSIX_C_SOURCE 200809L
#define _XOPEN_SOURCE 700

#include "wifi_backend.h"

#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <signal.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/select.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

extern char **environ;

static int failures;
static const char expected_password[] = "correct horse";

#define CHECK(condition)                                                       \
    do {                                                                       \
        if (!(condition)) {                                                     \
            fprintf(stderr, "test-wifi-backend: %s:%d: %s\n", __FILE__,      \
                    __LINE__, #condition);                                      \
            ++failures;                                                         \
        }                                                                      \
    } while (0)

static int path_join(char *output, size_t capacity, const char *directory,
                     const char *name)
{
    int count = snprintf(output, capacity, "%s/%s", directory, name);

    return count >= 0 && (size_t)count < capacity ? 0 : -1;
}

static int write_file(const char *path, const char *data, size_t length,
                      bool append)
{
    int flags = O_WRONLY | O_CREAT | (append ? O_APPEND : O_TRUNC);
    int descriptor = open(path, flags, 0600);
    size_t offset = 0;

    if (descriptor < 0)
        return -1;
    while (offset < length) {
        ssize_t count = write(descriptor, data + offset, length - offset);
        if (count > 0) {
            offset += (size_t)count;
            continue;
        }
        if (count < 0 && errno == EINTR)
            continue;
        (void)close(descriptor);
        return -1;
    }
    return close(descriptor);
}

static ssize_t read_file(const char *path, char *buffer, size_t capacity)
{
    int descriptor = open(path, O_RDONLY);
    size_t used = 0;

    if (descriptor < 0)
        return -1;
    while (used + 1u < capacity) {
        ssize_t count = read(descriptor, buffer + used, capacity - used - 1u);
        if (count > 0) {
            used += (size_t)count;
            continue;
        }
        if (count == 0)
            break;
        if (errno == EINTR)
            continue;
        (void)close(descriptor);
        return -1;
    }
    (void)close(descriptor);
    buffer[used] = '\0';
    return (ssize_t)used;
}

static bool has_argument(int argc, char **argv, const char *argument)
{
    int index;

    for (index = 1; index < argc; ++index) {
        if (strcmp(argv[index], argument) == 0)
            return true;
    }
    return false;
}

static const char *argument_after(int argc, char **argv, const char *argument)
{
    int index;

    for (index = 1; index + 1 < argc; ++index) {
        if (strcmp(argv[index], argument) == 0)
            return argv[index + 1];
    }
    return NULL;
}

static bool secret_is_exposed(int argc, char **argv)
{
    int index;
    char **environment;

    for (index = 0; index < argc; ++index) {
        if (strstr(argv[index], expected_password) != NULL)
            return true;
    }
    for (environment = environ; environment != NULL && *environment != NULL;
         ++environment) {
        if (strstr(*environment, expected_password) != NULL)
            return true;
    }
    return false;
}

static int fake_scan(const char *directory)
{
    static const char output[] =
        "no:436166652057694669:AA\\:BB\\:CC\\:DD\\:EE\\:01:82:WPA2:"
        "key_mgmt_psk:key_mgmt_psk:wlan0\n"
        "no:4F70656E204E6574:AA\\:BB\\:CC\\:DD\\:EE\\:02:61:--:--:--:"
        "wlan0\n";
    char marker_path[PATH_MAX];
    char pid_path[PATH_MAX];

    if (path_join(marker_path, sizeof(marker_path), directory, "hold-pipes") < 0 ||
        path_join(pid_path, sizeof(pid_path), directory, "holder-pid") < 0)
        return 1;
    if (access(marker_path, F_OK) == 0) {
        pid_t holder = fork();
        char pid_text[64];
        int count;

        if (holder < 0)
            return 1;
        if (holder == 0) {
            for (;;)
                (void)pause();
        }
        count = snprintf(pid_text, sizeof(pid_text), "%ld\n", (long)holder);
        if (count < 0 || (size_t)count >= sizeof(pid_text) ||
            write_file(pid_path, pid_text, (size_t)count, false) < 0)
            return 1;
    }

    return write(STDOUT_FILENO, output, sizeof(output) - 1u) ==
                   (ssize_t)(sizeof(output) - 1u)
               ? 0
               : 1;
}

static int fake_profile_lookup(const char *directory)
{
    char path[PATH_MAX];
    char uuid[64];
    ssize_t length;

    if (path_join(path, sizeof(path), directory, "profile-uuid") < 0)
        return 1;
    length = read_file(path, uuid, sizeof(uuid));
    if (length < 0)
        return 10;
    if (write(STDOUT_FILENO, uuid, (size_t)length) != length ||
        write(STDOUT_FILENO, "\n", 1) != 1)
        return 1;
    return 0;
}

static int fake_profile_add(const char *directory, int argc, char **argv)
{
    const char *uuid = argument_after(argc, argv, "connection.uuid");
    char uuid_path[PATH_MAX];
    char log_path[PATH_MAX];

    if (uuid == NULL || path_join(uuid_path, sizeof(uuid_path), directory,
                                  "profile-uuid") < 0 ||
        path_join(log_path, sizeof(log_path), directory, "stage-log") < 0 ||
        write_file(uuid_path, uuid, strlen(uuid), false) < 0 ||
        write_file(log_path, "add\n", 4u, true) < 0)
        return 1;
    return 0;
}

static int fake_connect(const char *directory, int argc, char **argv)
{
    static const char expected_line[] =
        "802-11-wireless-security.psk:correct horse\n";
    const char *password_file = argument_after(argc, argv, "passwd-file");
    char secret[256];
    size_t used = 0;
    char result_path[PATH_MAX];
    char log_path[PATH_MAX];
    int secret_descriptor = 3;

    if (secret_is_exposed(argc, argv) || password_file == NULL ||
        strcmp(password_file, "/proc/self/fd/3") != 0)
        return 41;
#ifdef __linux__
    secret_descriptor = open(password_file, O_RDONLY);
    if (secret_descriptor < 0)
        return 42;
#endif
    while (used + 1u < sizeof(secret)) {
        ssize_t count = read(secret_descriptor, secret + used,
                             sizeof(secret) - used - 1u);
        if (count > 0) {
            used += (size_t)count;
            continue;
        }
        if (count == 0)
            break;
        if (errno == EINTR)
            continue;
        return 43;
    }
#ifdef __linux__
    if (close(secret_descriptor) < 0)
        return 44;
#endif
    secret[used] = '\0';
    if (used != sizeof(expected_line) - 1u ||
        memcmp(secret, expected_line, sizeof(expected_line) - 1u) != 0)
        return 45;
    memset(secret, 0, sizeof(secret));
    if (path_join(result_path, sizeof(result_path), directory, "secret-ok") < 0 ||
        path_join(log_path, sizeof(log_path), directory, "stage-log") < 0 ||
        write_file(result_path, "ok\n", 3u, false) < 0 ||
        write_file(log_path, "up\n", 3u, true) < 0)
        return 46;
    return 0;
}

static int fake_disconnect(const char *directory)
{
    char log_path[PATH_MAX];

    if (path_join(log_path, sizeof(log_path), directory, "stage-log") < 0 ||
        write_file(log_path, "down\n", 5u, true) < 0)
        return 1;
    return 0;
}

static int fake_nmcli(int argc, char **argv, const char *directory)
{
    if (has_argument(argc, argv, "list") &&
        has_argument(argc, argv, "--rescan"))
        return fake_scan(directory);
    if (has_argument(argc, argv, "show") && has_argument(argc, argv, "id"))
        return fake_profile_lookup(directory);
    if (has_argument(argc, argv, "add"))
        return fake_profile_add(directory, argc, argv);
    if (has_argument(argc, argv, "up"))
        return fake_connect(directory, argc, argv);
    if (has_argument(argc, argv, "down"))
        return fake_disconnect(directory);
    return 2;
}

static const WifiNetwork *find_network(const WifiBackend *backend,
                                       const char *display_name)
{
    size_t index;

    for (index = 0; index < wifi_backend_network_count(backend); ++index) {
        const WifiNetwork *network = wifi_backend_network_at(backend, index);
        if (network != NULL && strcmp(network->display_name, display_name) == 0)
            return network;
    }
    return NULL;
}

static void test_parser(void)
{
    static const char scan[] =
        "no:43616665:AA\\:BB\\:CC\\:DD\\:EE\\:01:80:WPA2:"
        "key_mgmt_psk:key_mgmt_psk:wlan0\n"
        "yes:43616665:AA\\:BB\\:CC\\:DD\\:EE\\:02:40:WPA2:"
        "key_mgmt_psk:key_mgmt_psk:wlan0\n"
        "no:4F70656E:AA\\:BB\\:CC\\:DD\\:EE\\:03:95:--:--:--:wlan0\n"
        "no:534145:AA\\:BB\\:CC\\:DD\\:EE\\:04:75:WPA3:--:"
        "key_mgmt_sae:wlan0\n"
        "no:4F5745:AA\\:BB\\:CC\\:DD\\:EE\\:05:65:OWE:--:"
        "key_mgmt_owe:wlan0\n"
        "no:436F7270:AA\\:BB\\:CC\\:DD\\:EE\\:06:55:WPA2 802.1X:--:"
        "key_mgmt_802_1x:wlan0\n"
        "no:4F6C64:AA\\:BB\\:CC\\:DD\\:EE\\:07:45:WEP:--:--:wlan0\n";
    static const char hostile[] =
        "no:783B746F756368202F746D702F70776E:AA\\:BB\\:CC\\:DD\\:EE\\:08:"
        "35:--:--:--:wlan0\n";
    static const char hidden[] =
        "no::AA\\:BB\\:CC\\:DD\\:EE\\:09:20:WPA2:key_mgmt_psk:"
        "key_mgmt_psk:wlan0\n";
    static const char odd_hex[] =
        "no:123:AA\\:BB\\:CC\\:DD\\:EE\\:01:50:--:--:--:wlan0\n";
    static const char bad_escape[] =
        "no:4142:AA\\qBB\\:CC\\:DD\\:EE\\:01:50:--:--:--:wlan0\n";
    WifiBackend backend;
    const WifiNetwork *network;

    CHECK(wifi_backend_init(&backend) == 0);
    CHECK(wifi_backend_parse_scan_output(&backend, scan, sizeof(scan) - 1u) == 0);
    CHECK(wifi_backend_network_count(&backend) == 6u);
    network = wifi_backend_network_at(&backend, 0);
    CHECK(network != NULL);
    CHECK(network != NULL && strcmp(network->display_name, "Cafe") == 0);
    CHECK(network != NULL && network->active);
    CHECK(network != NULL && network->signal == 40u);
    CHECK(network != NULL && strcmp(network->bssid, "AA:BB:CC:DD:EE:02") == 0);

    network = find_network(&backend, "Open");
    CHECK(network != NULL && network->security == WIFI_SECURITY_OPEN);
    CHECK(network != NULL && wifi_backend_network_supported(network));
    network = find_network(&backend, "SAE");
    CHECK(network != NULL && network->security == WIFI_SECURITY_SAE);
    network = find_network(&backend, "OWE");
    CHECK(network != NULL && network->security == WIFI_SECURITY_OWE);
    network = find_network(&backend, "Corp");
    CHECK(network != NULL && network->security == WIFI_SECURITY_UNSUPPORTED);
    CHECK(network != NULL && !wifi_backend_network_supported(network));
    network = find_network(&backend, "Old");
    CHECK(network != NULL && network->security == WIFI_SECURITY_UNSUPPORTED);

    CHECK(wifi_backend_parse_scan_output(&backend, hostile,
                                         sizeof(hostile) - 1u) == 0);
    CHECK(wifi_backend_network_count(&backend) == 1u);
    network = wifi_backend_network_at(&backend, 0);
    CHECK(network != NULL &&
          strcmp(network->display_name, "x;touch /tmp/pwn") == 0);
    CHECK(wifi_backend_parse_scan_output(&backend, hidden,
                                         sizeof(hidden) - 1u) == 0);
    network = wifi_backend_network_at(&backend, 0);
    CHECK(network != NULL && network->ssid_len == 0);
    CHECK(network != NULL &&
          strcmp(network->display_name, "<Hidden network>") == 0);
    CHECK(network != NULL && !wifi_backend_network_supported(network));
    CHECK(wifi_backend_parse_scan_output(&backend, odd_hex,
                                         sizeof(odd_hex) - 1u) < 0);
    CHECK(wifi_backend_parse_scan_output(&backend, bad_escape,
                                         sizeof(bad_escape) - 1u) < 0);
    wifi_backend_destroy(&backend);
}

static int run_backend(WifiBackend *backend)
{
    unsigned int iterations;

    for (iterations = 0; iterations < 400u; ++iterations) {
        fd_set read_fds;
        int maximum_fd = -1;
        struct timeval timeout;
        int selected;
        pid_t pid;

        FD_ZERO(&read_fds);
        wifi_backend_add_select_fds(backend, &read_fds, &maximum_fd);
        timeout.tv_sec = 0;
        timeout.tv_usec = 25000;
        selected = select(maximum_fd + 1, &read_fds, NULL, NULL, &timeout);
        if (selected < 0 && errno != EINTR)
            return -1;
        if (selected >= 0)
            wifi_backend_dispatch_fds(backend, &read_fds);
        for (;;) {
            int status;
            pid = waitpid(-1, &status, WNOHANG);
            if (pid > 0) {
                (void)wifi_backend_handle_child_exit(backend, pid, status);
                continue;
            }
            if (pid < 0 && errno == EINTR)
                continue;
            break;
        }
        wifi_backend_tick(backend);
        if (!wifi_backend_busy(backend))
            return 0;
    }
    return -1;
}

static unsigned int count_lines_equal(const char *text, const char *line)
{
    size_t line_length = strlen(line);
    unsigned int count = 0;

    while (*text != '\0') {
        const char *end = strchr(text, '\n');
        size_t length = end != NULL ? (size_t)(end - text) : strlen(text);
        if (length == line_length && memcmp(text, line, line_length) == 0)
            ++count;
        if (end == NULL)
            break;
        text = end + 1;
    }
    return count;
}

static void test_relative_override(void)
{
    WifiBackend backend;

    CHECK(setenv("WIN31X_NMCLI", "relative-nmcli", 1) == 0);
    CHECK(wifi_backend_init(&backend) == 0);
    CHECK(wifi_backend_status(&backend) == WIFI_STATUS_UNAVAILABLE);
    CHECK(wifi_backend_start_scan(&backend) < 0);
    wifi_backend_destroy(&backend);
}

static void test_select_descriptor_limit(void)
{
    WifiBackend backend;
    fd_set read_fds;
    int maximum_fd = -1;
    int low_descriptor;
    int high_descriptor;

    CHECK(wifi_backend_init(&backend) == 0);
    low_descriptor = open("/dev/null", O_RDONLY);
    CHECK(low_descriptor >= 0);
    high_descriptor = low_descriptor >= 0
                          ? fcntl(low_descriptor, F_DUPFD, FD_SETSIZE)
                          : -1;
    backend.stdout_fd = high_descriptor >= FD_SETSIZE
                            ? high_descriptor
                            : FD_SETSIZE;
    backend.stdout_eof = false;
    FD_ZERO(&read_fds);
    wifi_backend_add_select_fds(&backend, &read_fds, &maximum_fd);
    CHECK(backend.stdout_fd == -1);
    CHECK(wifi_backend_status(&backend) == WIFI_STATUS_FAILED);
    CHECK(maximum_fd == -1);
    if (high_descriptor >= FD_SETSIZE) {
        errno = 0;
        CHECK(fcntl(high_descriptor, F_GETFD) < 0 && errno == EBADF);
    }
    if (low_descriptor >= 0)
        CHECK(close(low_descriptor) == 0);
    wifi_backend_destroy(&backend);
}

static void stop_pipe_holder(const char *directory)
{
    char path[PATH_MAX];
    char contents[64];
    char *end = NULL;
    long value;

    if (path_join(path, sizeof(path), directory, "holder-pid") < 0 ||
        read_file(path, contents, sizeof(contents)) <= 0)
        return;
    errno = 0;
    value = strtol(contents, &end, 10);
    if (errno == 0 && end != contents && value > 1 &&
        (unsigned long)value <= (unsigned long)INT_MAX)
        (void)kill((pid_t)value, SIGTERM);
}

static void test_pipe_holding_descendant(const char *self_path,
                                         const char *directory)
{
    WifiBackend backend;
    char marker_path[PATH_MAX];

    CHECK(path_join(marker_path, sizeof(marker_path), directory,
                    "hold-pipes") == 0);
    CHECK(write_file(marker_path, "hold\n", 5u, false) == 0);
    CHECK(setenv("WIN31X_NMCLI", self_path, 1) == 0);
    CHECK(setenv("WIN31X_WIFI_FAKE_DIR", directory, 1) == 0);
    CHECK(wifi_backend_init(&backend) == 0);
    CHECK(wifi_backend_start_scan(&backend) == 0);
    CHECK(run_backend(&backend) == 0);
    CHECK(!wifi_backend_busy(&backend));
    CHECK(wifi_backend_status(&backend) == WIFI_STATUS_READY);
    CHECK(wifi_backend_network_count(&backend) == 2u);
    stop_pipe_holder(directory);
    CHECK(unlink(marker_path) == 0);
    wifi_backend_destroy(&backend);
    CHECK(unsetenv("WIN31X_WIFI_FAKE_DIR") == 0);
}

static void test_async_workflow(const char *self_path, const char *directory)
{
    WifiBackend backend;
    const WifiNetwork *network;
    WifiNetwork selected;
    char path[PATH_MAX];
    char contents[1024];

    CHECK(setenv("WIN31X_NMCLI", self_path, 1) == 0);
    CHECK(setenv("WIN31X_WIFI_FAKE_DIR", directory, 1) == 0);
    CHECK(wifi_backend_init(&backend) == 0);
    CHECK(wifi_backend_start_scan(&backend) == 0);
    CHECK(wifi_backend_busy(&backend));
    CHECK(wifi_backend_job(&backend) == WIFI_JOB_SCAN);
    CHECK(run_backend(&backend) == 0);
    CHECK(wifi_backend_status(&backend) == WIFI_STATUS_READY);
    CHECK(wifi_backend_network_count(&backend) == 2u);
    network = find_network(&backend, "Cafe WiFi");
    CHECK(network != NULL);
    if (network == NULL) {
        wifi_backend_destroy(&backend);
        return;
    }
    selected = *network;

    CHECK(wifi_backend_start_connect(
              &backend, &selected,
              (const unsigned char *)expected_password,
              sizeof(expected_password) - 1u) == 0);
    CHECK(wifi_backend_job(&backend) == WIFI_JOB_CONNECT);
    CHECK(run_backend(&backend) == 0);
    CHECK(wifi_backend_status(&backend) == WIFI_STATUS_CONNECTED);
    CHECK(wifi_backend_last_exit_code(&backend) == 0);
    CHECK(path_join(path, sizeof(path), directory, "secret-ok") == 0);
    CHECK(read_file(path, contents, sizeof(contents)) == 3);
    CHECK(strcmp(contents, "ok\n") == 0);
    CHECK(path_join(path, sizeof(path), directory, "stage-log") == 0);
    CHECK(read_file(path, contents, sizeof(contents)) > 0);
    CHECK(count_lines_equal(contents, "add") == 1u);
    CHECK(count_lines_equal(contents, "up") == 1u);

    /* The lookup-hit path must reuse the deterministic profile. */
    CHECK(wifi_backend_start_connect(
              &backend, &selected,
              (const unsigned char *)expected_password,
              sizeof(expected_password) - 1u) == 0);
    CHECK(run_backend(&backend) == 0);
    CHECK(wifi_backend_status(&backend) == WIFI_STATUS_CONNECTED);
    CHECK(read_file(path, contents, sizeof(contents)) > 0);
    CHECK(count_lines_equal(contents, "add") == 1u);
    CHECK(count_lines_equal(contents, "up") == 2u);

    CHECK(wifi_backend_start_disconnect(&backend, selected.device) == 0);
    CHECK(wifi_backend_job(&backend) == WIFI_JOB_DISCONNECT);
    CHECK(run_backend(&backend) == 0);
    CHECK(wifi_backend_status(&backend) == WIFI_STATUS_DISCONNECTED);
    CHECK(read_file(path, contents, sizeof(contents)) > 0);
    CHECK(count_lines_equal(contents, "down") == 1u);

    CHECK(wifi_backend_start_connect(
              &backend, &selected, (const unsigned char *)"short", 5u) < 0);
    CHECK(wifi_backend_status(&backend) == WIFI_STATUS_INVALID_DATA);
    wifi_backend_destroy(&backend);
    CHECK(unsetenv("WIN31X_WIFI_FAKE_DIR") == 0);
}

static void clean_test_directory(const char *directory)
{
    static const char *const files[] = {
        "profile-uuid", "stage-log", "secret-ok", "hold-pipes",
        "holder-pid", NULL
    };
    size_t index;

    for (index = 0; files[index] != NULL; ++index) {
        char path[PATH_MAX];
        if (path_join(path, sizeof(path), directory, files[index]) == 0)
            (void)unlink(path);
    }
    (void)rmdir(directory);
}

int main(int argc, char **argv)
{
    const char *fake_directory = getenv("WIN31X_WIFI_FAKE_DIR");
    char self_path[PATH_MAX];
    char directory_template[] = "/tmp/win31x-wifi-test.XXXXXX";
    const char *directory;
    int temporary_descriptor;

    if (fake_directory != NULL && fake_directory[0] != '\0')
        return fake_nmcli(argc, argv, fake_directory);
    if (realpath(argv[0], self_path) == NULL) {
        fprintf(stderr, "test-wifi-backend: realpath: %s\n", strerror(errno));
        return 1;
    }
    test_relative_override();
    CHECK(setenv("WIN31X_NMCLI", self_path, 1) == 0);
    test_parser();
    test_select_descriptor_limit();
    temporary_descriptor = mkstemp(directory_template);
    if (temporary_descriptor < 0 || close(temporary_descriptor) < 0 ||
        unlink(directory_template) < 0 || mkdir(directory_template, 0700) < 0) {
        fprintf(stderr, "test-wifi-backend: temporary directory: %s\n",
                strerror(errno));
        return 1;
    }
    directory = directory_template;
    test_pipe_holding_descendant(self_path, directory);
    test_async_workflow(self_path, directory);
    clean_test_directory(directory);
    CHECK(unsetenv("WIN31X_NMCLI") == 0);

    if (failures != 0) {
        fprintf(stderr, "test-wifi-backend: %d failure(s)\n", failures);
        return 1;
    }
    puts("Wi-Fi backend tests passed");
    return 0;
}
