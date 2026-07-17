#define _POSIX_C_SOURCE 200809L

#include "wifi_backend.h"

#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <pwd.h>
#include <signal.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

#define WIFI_STDOUT_CAPACITY (256u * 1024u)
#define WIFI_STDERR_CAPACITY (16u * 1024u)
#define WIFI_SCAN_ROW_LIMIT 512u
#define WIFI_SCAN_FIELD_COUNT 8u
#define WIFI_SCAN_FIELD_SIZE 512u
#define WIFI_SCAN_ROW_SIZE 4096u
#define WIFI_SECRET_FD 3

typedef enum {
    STAGE_NONE,
    STAGE_SCAN,
    STAGE_PROFILE_LOOKUP,
    STAGE_PROFILE_ADD,
    STAGE_CONNECT_UP,
    STAGE_DISCONNECT
} WifiStage;

static void maybe_finish_stage(WifiBackend *backend);
static int start_profile_add(WifiBackend *backend);
static int start_connect_up(WifiBackend *backend);
static void make_display_name(WifiNetwork *network);

static void secure_clear(void *memory, size_t length)
{
    volatile unsigned char *cursor = memory;

    while (length > 0) {
        *cursor++ = 0;
        --length;
    }
}

static void clear_password(WifiBackend *backend)
{
    if (backend->password != NULL) {
        secure_clear(backend->password, backend->password_len);
        free(backend->password);
    }
    backend->password = NULL;
    backend->password_len = 0;
}

static void set_status(WifiBackend *backend, WifiStatus status,
                       const char *format, ...)
{
    va_list arguments;

    backend->status = status;
    va_start(arguments, format);
    (void)vsnprintf(backend->status_text, sizeof(backend->status_text),
                    format, arguments);
    va_end(arguments);
}

static int close_checked(int *descriptor)
{
    int result = 0;

    if (*descriptor >= 0) {
        result = close(*descriptor);
        *descriptor = -1;
    }
    return result;
}

static int set_descriptor_flags(int descriptor, int command, int flag)
{
    int current = fcntl(descriptor, command);

    if (current < 0)
        return -1;
    if (command == F_GETFD)
        return fcntl(descriptor, F_SETFD, current | flag);
    return fcntl(descriptor, F_SETFL, current | flag);
}

static int create_pipe(int descriptors[2])
{
    if (pipe(descriptors) < 0)
        return -1;
    if (set_descriptor_flags(descriptors[0], F_GETFD, FD_CLOEXEC) < 0 ||
        set_descriptor_flags(descriptors[1], F_GETFD, FD_CLOEXEC) < 0) {
        int saved_errno = errno;
        (void)close(descriptors[0]);
        (void)close(descriptors[1]);
        errno = saved_errno;
        return -1;
    }
    return 0;
}

static struct timespec monotonic_now(void)
{
    struct timespec now = {0, 0};

    (void)clock_gettime(CLOCK_MONOTONIC, &now);
    return now;
}

static struct timespec seconds_after(struct timespec value, time_t seconds)
{
    value.tv_sec += seconds;
    return value;
}

static int compare_timespec(struct timespec left, struct timespec right)
{
    if (left.tv_sec < right.tv_sec)
        return -1;
    if (left.tv_sec > right.tv_sec)
        return 1;
    if (left.tv_nsec < right.tv_nsec)
        return -1;
    if (left.tv_nsec > right.tv_nsec)
        return 1;
    return 0;
}

static void reset_command_state(WifiBackend *backend)
{
    (void)close_checked(&backend->stdout_fd);
    (void)close_checked(&backend->stderr_fd);
    backend->child_pid = -1;
    backend->child_exited = false;
    backend->child_status = 0;
    backend->stdout_eof = false;
    backend->stderr_eof = false;
    backend->output_overflow = false;
    backend->timed_out = false;
    backend->terminate_sent = false;
    backend->kill_sent = false;
    backend->pipe_grace_active = false;
    backend->descriptor_error = false;
    backend->stdout_len = 0;
    backend->stderr_len = 0;
    if (backend->stdout_data != NULL)
        backend->stdout_data[0] = '\0';
    if (backend->stderr_data != NULL)
        backend->stderr_data[0] = '\0';
}

static bool valid_device_name(const char *device)
{
    size_t index;
    size_t length;

    if (device == NULL)
        return false;
    length = strlen(device);
    if (length == 0 || length >= sizeof(((WifiNetwork *)0)->device))
        return false;
    for (index = 0; index < length; ++index) {
        unsigned char ch = (unsigned char)device[index];
        if (!isalnum(ch) && ch != '_' && ch != '-' && ch != '.' && ch != ':' &&
            ch != '@')
            return false;
    }
    return true;
}

static bool valid_bssid(const char *bssid)
{
    size_t index;

    if (bssid == NULL || strlen(bssid) != 17)
        return false;
    for (index = 0; index < 17; ++index) {
        if ((index + 1u) % 3u == 0u) {
            if (bssid[index] != ':')
                return false;
        } else if (!isxdigit((unsigned char)bssid[index])) {
            return false;
        }
    }
    return true;
}

static bool valid_uuid(const char *uuid)
{
    size_t index;

    if (uuid == NULL || strlen(uuid) != 36)
        return false;
    for (index = 0; index < 36; ++index) {
        if (index == 8 || index == 13 || index == 18 || index == 23) {
            if (uuid[index] != '-')
                return false;
        } else if (!isxdigit((unsigned char)uuid[index])) {
            return false;
        }
    }
    return true;
}

static bool valid_password(const WifiNetwork *network,
                           const unsigned char *password, size_t length)
{
    size_t index;

    if (network->security == WIFI_SECURITY_OPEN ||
        network->security == WIFI_SECURITY_OWE)
        return length == 0;
    if (network->security != WIFI_SECURITY_WPA_PSK &&
        network->security != WIFI_SECURITY_SAE)
        return false;
    if (password == NULL || length == 0 ||
        length > WIFI_BACKEND_MAX_PASSWORD_BYTES)
        return false;
    for (index = 0; index < length; ++index) {
        if (password[index] == '\0' || password[index] == '\n' ||
            password[index] == '\r')
            return false;
    }
    if (network->security == WIFI_SECURITY_SAE)
        return true;
    if (length >= 8 && length <= 63)
        return true;
    if (length != 64)
        return false;
    for (index = 0; index < length; ++index) {
        if (!isxdigit(password[index]))
            return false;
    }
    return true;
}

static int canonicalize_selected_network(WifiBackend *backend,
                                         const WifiNetwork *network)
{
    static const char digits[] = "0123456789ABCDEF";
    size_t index;

    if (network->ssid_len == 0 ||
        network->ssid_len > WIFI_BACKEND_MAX_SSID_BYTES ||
        !valid_bssid(network->bssid) || !valid_device_name(network->device))
        return -1;
    backend->selected = *network;
    for (index = 0; index < network->ssid_len; ++index) {
        unsigned char ch = network->ssid[index];
        if (ch == 0)
            return -1;
        backend->selected.ssid_hex[index * 2u] = digits[ch >> 4u];
        backend->selected.ssid_hex[index * 2u + 1u] = digits[ch & 0x0fu];
    }
    backend->selected.ssid_hex[network->ssid_len * 2u] = '\0';
    make_display_name(&backend->selected);
    return 0;
}

static int write_all(int descriptor, const unsigned char *data, size_t length)
{
    size_t offset = 0;

    while (offset < length) {
        ssize_t written = write(descriptor, data + offset, length - offset);
        if (written > 0) {
            offset += (size_t)written;
            continue;
        }
        if (written < 0 && errno == EINTR)
            continue;
        return -1;
    }
    return 0;
}

static void close_child_descriptors(const int output_pipe[2],
                                    const int error_pipe[2],
                                    const int secret_pipe[2], bool has_secret)
{
    int descriptors[6];
    size_t count = has_secret ? 6u : 4u;
    size_t index;

    descriptors[0] = output_pipe[0];
    descriptors[1] = output_pipe[1];
    descriptors[2] = error_pipe[0];
    descriptors[3] = error_pipe[1];
    descriptors[4] = secret_pipe[0];
    descriptors[5] = secret_pipe[1];
    for (index = 0; index < count; ++index) {
        if (descriptors[index] > WIFI_SECRET_FD)
            (void)close(descriptors[index]);
    }
}

static int spawn_command(WifiBackend *backend, char *const arguments[],
                         const unsigned char *secret, size_t secret_length,
                         time_t timeout_seconds)
{
    int output_pipe[2] = {-1, -1};
    int error_pipe[2] = {-1, -1};
    int secret_pipe[2] = {-1, -1};
    bool has_secret = secret != NULL;
    pid_t pid;

    if (create_pipe(output_pipe) < 0 || create_pipe(error_pipe) < 0)
        goto fail;
    if (output_pipe[0] >= FD_SETSIZE || error_pipe[0] >= FD_SETSIZE) {
        errno = EMFILE;
        goto fail;
    }
    if (has_secret) {
        if (secret_length == 0 || create_pipe(secret_pipe) < 0)
            goto fail;
        /* Fill the small private pipe before fork, avoiding SIGPIPE races. */
        if (write_all(secret_pipe[1], secret, secret_length) < 0)
            goto fail;
        if (close(secret_pipe[1]) < 0)
            goto fail;
        secret_pipe[1] = -1;
    }

    pid = fork();
    if (pid < 0)
        goto fail;
    if (pid == 0) {
        struct sigaction action;

        if (dup2(output_pipe[1], STDOUT_FILENO) < 0 ||
            dup2(error_pipe[1], STDERR_FILENO) < 0 ||
            fcntl(STDOUT_FILENO, F_SETFD, 0) < 0 ||
            fcntl(STDERR_FILENO, F_SETFD, 0) < 0)
            _exit(126);
        if (has_secret) {
            if (dup2(secret_pipe[0], WIFI_SECRET_FD) < 0 ||
                fcntl(WIFI_SECRET_FD, F_SETFD, 0) < 0)
                _exit(126);
        }
        close_child_descriptors(output_pipe, error_pipe, secret_pipe,
                                has_secret);

        memset(&action, 0, sizeof(action));
        action.sa_handler = SIG_DFL;
        (void)sigemptyset(&action.sa_mask);
        (void)sigaction(SIGCHLD, &action, NULL);
        (void)sigaction(SIGPIPE, &action, NULL);
        (void)setenv("LC_ALL", "C", 1);
        (void)setenv("LANG", "C", 1);
        (void)setenv("TERM", "dumb", 1);
        (void)setenv("NO_COLOR", "1", 1);
        (void)setenv("PAGER", "", 1);
        execv(backend->nmcli_path, arguments);
        _exit(errno == ENOENT ? 127 : 126);
    }

    (void)close(output_pipe[1]);
    output_pipe[1] = -1;
    (void)close(error_pipe[1]);
    error_pipe[1] = -1;
    if (has_secret) {
        (void)close(secret_pipe[0]);
        secret_pipe[0] = -1;
    }
    if (set_descriptor_flags(output_pipe[0], F_GETFL, O_NONBLOCK) < 0 ||
        set_descriptor_flags(error_pipe[0], F_GETFL, O_NONBLOCK) < 0) {
        int saved_errno = errno;
        (void)kill(pid, SIGKILL);
        (void)close(output_pipe[0]);
        (void)close(error_pipe[0]);
        errno = saved_errno;
        return -1;
    }
    backend->child_pid = pid;
    backend->stdout_fd = output_pipe[0];
    backend->stderr_fd = error_pipe[0];
    backend->deadline = seconds_after(monotonic_now(), timeout_seconds);
    return 0;

fail:
    {
        int saved_errno = errno;
        if (output_pipe[0] >= 0)
            (void)close(output_pipe[0]);
        if (output_pipe[1] >= 0)
            (void)close(output_pipe[1]);
        if (error_pipe[0] >= 0)
            (void)close(error_pipe[0]);
        if (error_pipe[1] >= 0)
            (void)close(error_pipe[1]);
        if (secret_pipe[0] >= 0)
            (void)close(secret_pipe[0]);
        if (secret_pipe[1] >= 0)
            (void)close(secret_pipe[1]);
        errno = saved_errno;
        return -1;
    }
}

static int start_stage(WifiBackend *backend, WifiStage stage,
                       char *const arguments[], const unsigned char *secret,
                       size_t secret_length, time_t timeout_seconds)
{
    reset_command_state(backend);
    backend->stage = (int)stage;
    if (spawn_command(backend, arguments, secret, secret_length,
                      timeout_seconds) < 0) {
        int saved_errno = errno;
        backend->stage = (int)STAGE_NONE;
        if (saved_errno == EMFILE) {
            set_status(backend, WIFI_STATUS_FAILED,
                       "Too many files are open for Wi-Fi controls.");
        } else {
            set_status(backend, WIFI_STATUS_FAILED,
                       "Could not start NetworkManager command.");
        }
        clear_password(backend);
        errno = saved_errno;
        return -1;
    }
    return 0;
}

static int hex_value(unsigned char ch)
{
    if (ch >= '0' && ch <= '9')
        return (int)(ch - '0');
    if (ch >= 'a' && ch <= 'f')
        return (int)(ch - 'a') + 10;
    if (ch >= 'A' && ch <= 'F')
        return (int)(ch - 'A') + 10;
    return -1;
}

static int decode_ssid_hex(const char *hex, WifiNetwork *network)
{
    static const char digits[] = "0123456789ABCDEF";
    size_t length = strlen(hex);
    size_t index;

    if (length > WIFI_BACKEND_MAX_SSID_BYTES * 2u || length % 2u != 0u)
        return -1;
    network->ssid_len = length / 2u;
    for (index = 0; index < network->ssid_len; ++index) {
        int high = hex_value((unsigned char)hex[index * 2u]);
        int low = hex_value((unsigned char)hex[index * 2u + 1u]);
        unsigned int value;

        if (high < 0 || low < 0)
            return -1;
        value = (unsigned int)high * 16u + (unsigned int)low;
        if (value == 0)
            return -1;
        network->ssid[index] = (unsigned char)value;
        network->ssid_hex[index * 2u] = digits[value >> 4u];
        network->ssid_hex[index * 2u + 1u] = digits[value & 0x0fu];
    }
    network->ssid_hex[length] = '\0';
    return 0;
}

static void make_display_name(WifiNetwork *network)
{
    static const char digits[] = "0123456789ABCDEF";
    size_t input_index;
    size_t output_index = 0;

    if (network->ssid_len == 0) {
        (void)snprintf(network->display_name, sizeof(network->display_name),
                       "%s", "<Hidden network>");
        return;
    }
    for (input_index = 0; input_index < network->ssid_len; ++input_index) {
        unsigned char ch = network->ssid[input_index];

        if (ch >= 0x20u && ch <= 0x7eu && ch != '\\') {
            network->display_name[output_index++] = (char)ch;
        } else {
            network->display_name[output_index++] = '\\';
            network->display_name[output_index++] = 'x';
            network->display_name[output_index++] = digits[ch >> 4u];
            network->display_name[output_index++] = digits[ch & 0x0fu];
        }
    }
    network->display_name[output_index] = '\0';
}

static int split_scan_fields(const char *line, size_t length,
                             char fields[WIFI_SCAN_FIELD_COUNT]
                                        [WIFI_SCAN_FIELD_SIZE])
{
    size_t input_index;
    size_t field_index = 0;
    size_t output_index = 0;
    bool escaped = false;

    memset(fields, 0, WIFI_SCAN_FIELD_COUNT * WIFI_SCAN_FIELD_SIZE);
    for (input_index = 0; input_index < length; ++input_index) {
        unsigned char ch = (unsigned char)line[input_index];

        if (escaped) {
            if (ch != ':' && ch != '\\')
                return -1;
            if (output_index + 1u >= WIFI_SCAN_FIELD_SIZE)
                return -1;
            fields[field_index][output_index++] = (char)ch;
            escaped = false;
            continue;
        }
        if (ch == '\\') {
            escaped = true;
            continue;
        }
        if (ch == ':') {
            if (field_index + 1u >= WIFI_SCAN_FIELD_COUNT)
                return -1;
            fields[field_index][output_index] = '\0';
            ++field_index;
            output_index = 0;
            continue;
        }
        if (ch == '\0' || ch == '\r' || ch == '\n' ||
            output_index + 1u >= WIFI_SCAN_FIELD_SIZE)
            return -1;
        fields[field_index][output_index++] = (char)ch;
    }
    if (escaped || field_index + 1u != WIFI_SCAN_FIELD_COUNT)
        return -1;
    fields[field_index][output_index] = '\0';
    return 0;
}

static bool contains_case_insensitive(const char *text, const char *needle)
{
    size_t needle_length;

    if (text == NULL || needle == NULL)
        return false;
    needle_length = strlen(needle);
    if (needle_length == 0)
        return true;
    while (*text != '\0') {
        if (strncasecmp(text, needle, needle_length) == 0)
            return true;
        ++text;
    }
    return false;
}

static void copy_bounded(char *destination, size_t capacity,
                         const char *source)
{
    size_t length = strlen(source);

    if (length >= capacity)
        length = capacity - 1u;
    if (length > 0)
        memcpy(destination, source, length);
    destination[length] = '\0';
}

static WifiSecurity classify_security(const char *security,
                                      const char *wpa_flags,
                                      const char *rsn_flags)
{
    bool enterprise = contains_case_insensitive(wpa_flags,
                                                "key_mgmt_802_1x") ||
                      contains_case_insensitive(rsn_flags,
                                                "key_mgmt_802_1x") ||
                      contains_case_insensitive(wpa_flags,
                                                "key_mgmt_eap") ||
                      contains_case_insensitive(rsn_flags,
                                                "key_mgmt_eap") ||
                      contains_case_insensitive(security, "802.1X") ||
                      contains_case_insensitive(security, "EAP");
    bool psk = contains_case_insensitive(wpa_flags, "key_mgmt_psk") ||
               contains_case_insensitive(rsn_flags, "key_mgmt_psk");
    bool sae = contains_case_insensitive(wpa_flags, "key_mgmt_sae") ||
               contains_case_insensitive(rsn_flags, "key_mgmt_sae");
    bool owe = contains_case_insensitive(wpa_flags, "key_mgmt_owe") ||
               contains_case_insensitive(rsn_flags, "key_mgmt_owe");

    if (enterprise || contains_case_insensitive(security, "WEP"))
        return WIFI_SECURITY_UNSUPPORTED;
    if (owe)
        return WIFI_SECURITY_OWE;
    if (psk)
        return WIFI_SECURITY_WPA_PSK;
    if (sae)
        return WIFI_SECURITY_SAE;
    if (strcmp(security, "--") == 0 || security[0] == '\0')
        return WIFI_SECURITY_OPEN;
    if (contains_case_insensitive(security, "WPA"))
        return WIFI_SECURITY_WPA_PSK;
    return WIFI_SECURITY_UNSUPPORTED;
}

static int parse_signal(const char *text, unsigned int *signal_out)
{
    char *end = NULL;
    unsigned long value;

    errno = 0;
    value = strtoul(text, &end, 10);
    if (errno != 0 || end == text || *end != '\0' || value > 100ul)
        return -1;
    *signal_out = (unsigned int)value;
    return 0;
}

static int parse_scan_row(const char *line, size_t length,
                          WifiNetwork *network)
{
    char fields[WIFI_SCAN_FIELD_COUNT][WIFI_SCAN_FIELD_SIZE];

    memset(network, 0, sizeof(*network));
    if (length == 0 || length >= WIFI_SCAN_ROW_SIZE ||
        split_scan_fields(line, length, fields) < 0)
        return -1;
    if (strcmp(fields[0], "yes") == 0 || strcmp(fields[0], "*") == 0)
        network->active = true;
    else if (strcmp(fields[0], "no") != 0 && fields[0][0] != '\0')
        return -1;
    if (decode_ssid_hex(fields[1], network) < 0 ||
        !valid_bssid(fields[2]) || parse_signal(fields[3], &network->signal) < 0 ||
        !valid_device_name(fields[7]))
        return -1;
    copy_bounded(network->bssid, sizeof(network->bssid), fields[2]);
    copy_bounded(network->device, sizeof(network->device), fields[7]);
    copy_bounded(network->security_name, sizeof(network->security_name),
                 fields[4]);
    network->security = classify_security(fields[4], fields[5], fields[6]);
    if (network->ssid_len == 0)
        network->security = WIFI_SECURITY_UNSUPPORTED;
    make_display_name(network);
    return 0;
}

static bool same_network_identity(const WifiNetwork *left,
                                  const WifiNetwork *right)
{
    if (left->ssid_len == 0 || right->ssid_len == 0)
        return left->ssid_len == 0 && right->ssid_len == 0 &&
               strcmp(left->bssid, right->bssid) == 0 &&
               strcmp(left->device, right->device) == 0;
    return left->ssid_len == right->ssid_len &&
           left->security == right->security &&
           memcmp(left->ssid, right->ssid, left->ssid_len) == 0;
}

static bool candidate_is_better(const WifiNetwork *candidate,
                                const WifiNetwork *existing)
{
    if (candidate->active != existing->active)
        return candidate->active;
    return candidate->signal > existing->signal;
}

static size_t weakest_network_index(const WifiNetwork *networks, size_t count)
{
    size_t weakest = 0;
    size_t index;

    for (index = 1; index < count; ++index) {
        if (candidate_is_better(&networks[weakest], &networks[index]))
            weakest = index;
    }
    return weakest;
}

static int compare_networks(const void *left_pointer, const void *right_pointer)
{
    const WifiNetwork *left = left_pointer;
    const WifiNetwork *right = right_pointer;
    int name_comparison;

    if (left->active != right->active)
        return left->active ? -1 : 1;
    if (left->signal != right->signal)
        return left->signal > right->signal ? -1 : 1;
    name_comparison = strcasecmp(left->display_name, right->display_name);
    if (name_comparison != 0)
        return name_comparison;
    return strcmp(left->bssid, right->bssid);
}

int wifi_backend_parse_scan_output(WifiBackend *backend,
                                   const char *output, size_t length)
{
    WifiNetwork parsed[WIFI_BACKEND_MAX_NETWORKS];
    size_t parsed_count = 0;
    size_t row_count = 0;
    size_t offset = 0;

    if (backend == NULL || (output == NULL && length != 0)) {
        errno = EINVAL;
        return -1;
    }
    while (offset < length) {
        size_t end = offset;
        WifiNetwork candidate;
        size_t index;

        while (end < length && output[end] != '\n')
            ++end;
        if (end > offset) {
            ++row_count;
            if (row_count > WIFI_SCAN_ROW_LIMIT ||
                parse_scan_row(output + offset, end - offset, &candidate) < 0) {
                errno = EINVAL;
                return -1;
            }
            for (index = 0; index < parsed_count; ++index) {
                if (same_network_identity(&parsed[index], &candidate))
                    break;
            }
            if (index < parsed_count) {
                if (candidate_is_better(&candidate, &parsed[index]))
                    parsed[index] = candidate;
            } else if (parsed_count < WIFI_BACKEND_MAX_NETWORKS) {
                parsed[parsed_count++] = candidate;
            } else {
                size_t weakest = weakest_network_index(parsed, parsed_count);
                if (candidate_is_better(&candidate, &parsed[weakest]))
                    parsed[weakest] = candidate;
            }
        }
        offset = end < length ? end + 1u : end;
    }
    qsort(parsed, parsed_count, sizeof(parsed[0]), compare_networks);
    if (parsed_count > 0)
        memcpy(backend->networks, parsed, parsed_count * sizeof(parsed[0]));
    backend->network_count = parsed_count;
    return 0;
}

static int random_bytes(unsigned char *bytes, size_t length)
{
    int descriptor;
    size_t offset = 0;

    descriptor = open("/dev/urandom", O_RDONLY | O_CLOEXEC);
    if (descriptor < 0)
        return -1;
    while (offset < length) {
        ssize_t count = read(descriptor, bytes + offset, length - offset);
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

static int create_uuid(char output[37])
{
    static const char digits[] = "0123456789abcdef";
    unsigned char bytes[16];
    size_t input_index;
    size_t output_index = 0;

    if (random_bytes(bytes, sizeof(bytes)) < 0)
        return -1;
    bytes[6] = (unsigned char)((bytes[6] & 0x0fu) | 0x40u);
    bytes[8] = (unsigned char)((bytes[8] & 0x3fu) | 0x80u);
    for (input_index = 0; input_index < sizeof(bytes); ++input_index) {
        if (input_index == 4 || input_index == 6 || input_index == 8 ||
            input_index == 10)
            output[output_index++] = '-';
        output[output_index++] = digits[bytes[input_index] >> 4u];
        output[output_index++] = digits[bytes[input_index] & 0x0fu];
    }
    output[output_index] = '\0';
    secure_clear(bytes, sizeof(bytes));
    return 0;
}

static int current_username(char *output, size_t capacity)
{
    struct passwd password_entry;
    struct passwd *result = NULL;
    long suggested_size = sysconf(_SC_GETPW_R_SIZE_MAX);
    size_t buffer_size = suggested_size > 0 && suggested_size < 65536
                             ? (size_t)suggested_size
                             : 4096u;
    char *buffer = malloc(buffer_size);
    int status;

    if (buffer == NULL)
        return -1;
    status = getpwuid_r(getuid(), &password_entry, buffer, buffer_size, &result);
    if (status != 0 || result == NULL || result->pw_name == NULL ||
        result->pw_name[0] == '\0' || strlen(result->pw_name) >= capacity) {
        free(buffer);
        errno = status != 0 ? status : EINVAL;
        return -1;
    }
    (void)snprintf(output, capacity, "%s", result->pw_name);
    free(buffer);
    return 0;
}

static const char *security_profile_name(WifiSecurity security)
{
    switch (security) {
    case WIFI_SECURITY_OPEN:
        return "open";
    case WIFI_SECURITY_OWE:
        return "owe";
    case WIFI_SECURITY_WPA_PSK:
        return "wpa";
    case WIFI_SECURITY_SAE:
        return "sae";
    case WIFI_SECURITY_UNSUPPORTED:
        break;
    }
    return "unsupported";
}

static const char *security_key_management(WifiSecurity security)
{
    switch (security) {
    case WIFI_SECURITY_OWE:
        return "owe";
    case WIFI_SECURITY_WPA_PSK:
        return "wpa-psk";
    case WIFI_SECURITY_SAE:
        return "sae";
    case WIFI_SECURITY_OPEN:
    case WIFI_SECURITY_UNSUPPORTED:
        break;
    }
    return NULL;
}

static int start_profile_lookup(WifiBackend *backend)
{
    char *arguments[] = {
        backend->nmcli_path,
        "--colors", "no",
        "--terse",
        "--escape", "yes",
        "--get-values", "UUID",
        "connection", "show", "id", backend->profile_id,
        NULL
    };

    return start_stage(backend, STAGE_PROFILE_LOOKUP, arguments, NULL, 0, 10);
}

static int start_profile_add(WifiBackend *backend)
{
    char username[256];
    char permissions[sizeof(username) + 6u];
    char ssid[WIFI_BACKEND_MAX_SSID_BYTES + 1u];
    const char *key_management;
    char *arguments[28];
    size_t argument_count = 0;

    if (current_username(username, sizeof(username)) < 0 ||
        create_uuid(backend->profile_uuid) < 0)
        return -1;
    (void)snprintf(permissions, sizeof(permissions), "user:%s", username);
    memcpy(ssid, backend->selected.ssid, backend->selected.ssid_len);
    ssid[backend->selected.ssid_len] = '\0';

    arguments[argument_count++] = backend->nmcli_path;
    arguments[argument_count++] = "--colors";
    arguments[argument_count++] = "no";
    arguments[argument_count++] = "--wait";
    arguments[argument_count++] = "15";
    arguments[argument_count++] = "connection";
    arguments[argument_count++] = "add";
    arguments[argument_count++] = "type";
    arguments[argument_count++] = "wifi";
    arguments[argument_count++] = "con-name";
    arguments[argument_count++] = backend->profile_id;
    arguments[argument_count++] = "connection.uuid";
    arguments[argument_count++] = backend->profile_uuid;
    arguments[argument_count++] = "connection.permissions";
    arguments[argument_count++] = permissions;
    arguments[argument_count++] = "wifi.ssid";
    arguments[argument_count++] = ssid;
    key_management = security_key_management(backend->selected.security);
    if (key_management != NULL) {
        arguments[argument_count++] = "wifi-sec.key-mgmt";
        arguments[argument_count++] = (char *)key_management;
    }
    arguments[argument_count] = NULL;
    backend->profile_created = true;
    if (start_stage(backend, STAGE_PROFILE_ADD, arguments, NULL, 0, 20) < 0) {
        backend->profile_created = false;
        secure_clear(ssid, sizeof(ssid));
        return -1;
    }
    secure_clear(ssid, sizeof(ssid));
    return 0;
}

static int start_connect_up(WifiBackend *backend)
{
    char *arguments[24];
    size_t argument_count = 0;
    unsigned char *secret = NULL;
    size_t secret_length = 0;
    static const char prefix[] = "802-11-wireless-security.psk:";
    int result;

    arguments[argument_count++] = backend->nmcli_path;
    arguments[argument_count++] = "--colors";
    arguments[argument_count++] = "no";
    arguments[argument_count++] = "--wait";
    arguments[argument_count++] = "45";
    arguments[argument_count++] = "connection";
    arguments[argument_count++] = "up";
    arguments[argument_count++] = "uuid";
    arguments[argument_count++] = backend->profile_uuid;
    arguments[argument_count++] = "ifname";
    arguments[argument_count++] = backend->selected.device;
    arguments[argument_count++] = "ap";
    arguments[argument_count++] = backend->selected.bssid;
    if (backend->password_len > 0) {
        secret_length = sizeof(prefix) - 1u + backend->password_len + 1u;
        secret = malloc(secret_length);
        if (secret == NULL)
            return -1;
        memcpy(secret, prefix, sizeof(prefix) - 1u);
        memcpy(secret + sizeof(prefix) - 1u, backend->password,
               backend->password_len);
        secret[secret_length - 1u] = '\n';
        arguments[argument_count++] = "passwd-file";
        arguments[argument_count++] = "/proc/self/fd/3";
    }
    arguments[argument_count] = NULL;
    result = start_stage(backend, STAGE_CONNECT_UP, arguments, secret,
                         secret_length, 50);
    if (secret != NULL) {
        secure_clear(secret, secret_length);
        free(secret);
    }
    /* The pipe now owns the only backend-created copy of the secret. */
    clear_password(backend);
    return result;
}

static int start_scan_command(WifiBackend *backend)
{
    char *arguments[] = {
        backend->nmcli_path,
        "--colors", "no",
        "--terse",
        "--escape", "yes",
        "--wait", "15",
        "--fields",
        "ACTIVE,SSID-HEX,BSSID,SIGNAL,SECURITY,WPA-FLAGS,RSN-FLAGS,DEVICE",
        "device", "wifi", "list", "--rescan", "yes",
        NULL
    };

    return start_stage(backend, STAGE_SCAN, arguments, NULL, 0, 20);
}

static int start_disconnect_command(WifiBackend *backend, const char *device)
{
    char *arguments[] = {
        backend->nmcli_path,
        "--colors", "no",
        "--wait", "10",
        "device", "down", (char *)device,
        NULL
    };

    return start_stage(backend, STAGE_DISCONNECT, arguments, NULL, 0, 15);
}

static int exit_code_from_status(int wait_status)
{
    if (WIFEXITED(wait_status))
        return WEXITSTATUS(wait_status);
    if (WIFSIGNALED(wait_status))
        return 128 + WTERMSIG(wait_status);
    return -1;
}

static int parse_lookup_uuid(const char *output, size_t length, char uuid[37])
{
    size_t start = 0;
    size_t end = length;

    if (output == NULL)
        return -1;
    while (start < end && isspace((unsigned char)output[start]))
        ++start;
    while (end > start && isspace((unsigned char)output[end - 1u]))
        --end;
    if (end - start != 36u)
        return -1;
    memcpy(uuid, output + start, 36u);
    uuid[36] = '\0';
    return valid_uuid(uuid) ? 0 : -1;
}

static void complete_with_exit_error(WifiBackend *backend, int exit_code)
{
    if (backend->timed_out || exit_code == 3) {
        set_status(backend, WIFI_STATUS_TIMED_OUT,
                   "The NetworkManager operation timed out.");
    } else if (exit_code == 8 || exit_code == 127) {
        set_status(backend, WIFI_STATUS_UNAVAILABLE,
                   "NetworkManager is not available.");
    } else if (backend->stage == (int)STAGE_CONNECT_UP && exit_code == 4) {
        set_status(backend, WIFI_STATUS_FAILED,
                   "Could not connect. Check the password and signal.");
    } else if (backend->stage == (int)STAGE_DISCONNECT && exit_code == 6) {
        set_status(backend, WIFI_STATUS_FAILED,
                   "Could not disconnect the Wi-Fi device.");
    } else {
        set_status(backend, WIFI_STATUS_FAILED,
                   "NetworkManager command failed (status %d).", exit_code);
    }
    clear_password(backend);
    backend->stage = (int)STAGE_NONE;
}

static void finish_successful_stage(WifiBackend *backend)
{
    WifiStage completed_stage = (WifiStage)backend->stage;

    switch (completed_stage) {
    case STAGE_SCAN:
        if (wifi_backend_parse_scan_output(backend, backend->stdout_data,
                                           backend->stdout_len) < 0) {
            set_status(backend, WIFI_STATUS_INVALID_DATA,
                       "NetworkManager returned invalid Wi-Fi data.");
            backend->stage = (int)STAGE_NONE;
            return;
        }
        set_status(backend, WIFI_STATUS_READY,
                   backend->network_count == 0
                       ? "No Wi-Fi networks were found."
                       : "Wi-Fi scan complete.");
        backend->stage = (int)STAGE_NONE;
        return;

    case STAGE_PROFILE_LOOKUP:
        if (parse_lookup_uuid(backend->stdout_data, backend->stdout_len,
                              backend->profile_uuid) < 0) {
            set_status(backend, WIFI_STATUS_INVALID_DATA,
                       "NetworkManager returned an invalid profile identifier.");
            clear_password(backend);
            backend->stage = (int)STAGE_NONE;
            return;
        }
        if (start_connect_up(backend) < 0) {
            clear_password(backend);
            backend->stage = (int)STAGE_NONE;
            set_status(backend, WIFI_STATUS_FAILED,
                       "Could not start Wi-Fi activation.");
        }
        return;

    case STAGE_PROFILE_ADD:
        if (start_connect_up(backend) < 0) {
            clear_password(backend);
            backend->stage = (int)STAGE_NONE;
            set_status(backend, WIFI_STATUS_FAILED,
                       "Could not start Wi-Fi activation.");
        }
        return;

    case STAGE_CONNECT_UP:
        {
            size_t index;
            for (index = 0; index < backend->network_count; ++index)
                backend->networks[index].active = false;
            for (index = 0; index < backend->network_count; ++index) {
                if (same_network_identity(&backend->networks[index],
                                          &backend->selected)) {
                    backend->networks[index].active = true;
                    break;
                }
            }
        }
        set_status(backend, WIFI_STATUS_CONNECTED, "Wi-Fi connected.");
        backend->stage = (int)STAGE_NONE;
        return;

    case STAGE_DISCONNECT:
        {
            size_t index;
            for (index = 0; index < backend->network_count; ++index) {
                if (strcmp(backend->networks[index].device,
                           backend->selected.device) == 0)
                    backend->networks[index].active = false;
            }
        }
        set_status(backend, WIFI_STATUS_DISCONNECTED, "Wi-Fi disconnected.");
        backend->stage = (int)STAGE_NONE;
        return;

    case STAGE_NONE:
        return;
    }
}

static void finish_stage(WifiBackend *backend)
{
    int exit_code = exit_code_from_status(backend->child_status);
    WifiStage completed_stage = (WifiStage)backend->stage;

    backend->last_exit_code = exit_code;
    backend->child_pid = -1;
    if (backend->stdout_data != NULL)
        backend->stdout_data[backend->stdout_len] = '\0';
    if (backend->stderr_data != NULL)
        backend->stderr_data[backend->stderr_len] = '\0';

    if (backend->descriptor_error) {
        set_status(backend, WIFI_STATUS_FAILED,
                   "A Wi-Fi process descriptor cannot be monitored safely.");
        clear_password(backend);
        backend->stage = (int)STAGE_NONE;
        return;
    }
    if (backend->output_overflow) {
        set_status(backend, WIFI_STATUS_INVALID_DATA,
                   "NetworkManager returned too much data.");
        clear_password(backend);
        backend->stage = (int)STAGE_NONE;
        return;
    }
    if (exit_code != 0) {
        /* A missing deterministic profile is the expected lookup miss. */
        if (completed_stage == STAGE_PROFILE_LOOKUP && exit_code == 10) {
            if (start_profile_add(backend) < 0) {
                clear_password(backend);
                backend->stage = (int)STAGE_NONE;
                set_status(backend, WIFI_STATUS_FAILED,
                           "Could not create a Wi-Fi profile.");
            }
            return;
        }
        complete_with_exit_error(backend, exit_code);
        return;
    }
    finish_successful_stage(backend);
}

static void maybe_finish_stage(WifiBackend *backend)
{
    if (backend->stage != (int)STAGE_NONE && backend->child_exited &&
        backend->stdout_eof && backend->stderr_eof)
        finish_stage(backend);
}

static void append_output(WifiBackend *backend, bool standard_output,
                          const char *bytes, size_t length)
{
    char *destination = standard_output ? backend->stdout_data
                                        : backend->stderr_data;
    size_t *used = standard_output ? &backend->stdout_len
                                   : &backend->stderr_len;
    size_t capacity = standard_output ? backend->stdout_capacity
                                      : backend->stderr_capacity;
    size_t available;
    size_t copied;

    if (*used >= capacity) {
        backend->output_overflow = true;
        return;
    }
    available = capacity - *used;
    copied = length < available ? length : available;
    if (copied > 0)
        memcpy(destination + *used, bytes, copied);
    *used += copied;
    destination[*used] = '\0';
    if (copied != length)
        backend->output_overflow = true;
}

static void drain_descriptor(WifiBackend *backend, bool standard_output)
{
    int *descriptor = standard_output ? &backend->stdout_fd
                                      : &backend->stderr_fd;
    bool *eof = standard_output ? &backend->stdout_eof
                                : &backend->stderr_eof;
    char buffer[4096];

    if (*descriptor < 0 || *eof)
        return;
    for (;;) {
        ssize_t count = read(*descriptor, buffer, sizeof(buffer));
        if (count > 0) {
            append_output(backend, standard_output, buffer, (size_t)count);
            continue;
        }
        if (count == 0) {
            *eof = true;
            (void)close_checked(descriptor);
            return;
        }
        if (errno == EINTR)
            continue;
        if (errno == EAGAIN || errno == EWOULDBLOCK)
            return;
        *eof = true;
        backend->output_overflow = true;
        (void)close_checked(descriptor);
        return;
    }
}

static void terminate_for_timeout(WifiBackend *backend)
{
    struct timespec now;

    if (!wifi_backend_busy(backend))
        return;
    now = monotonic_now();
    if (backend->child_exited) {
        if (backend->pipe_grace_active &&
            compare_timespec(now, backend->pipe_deadline) >= 0) {
            (void)close_checked(&backend->stdout_fd);
            (void)close_checked(&backend->stderr_fd);
            backend->stdout_eof = true;
            backend->stderr_eof = true;
            backend->pipe_grace_active = false;
        }
        return;
    }
    if (backend->child_pid <= 0)
        return;
    if (!backend->terminate_sent &&
        compare_timespec(now, backend->deadline) >= 0) {
        (void)kill(backend->child_pid, SIGTERM);
        backend->timed_out = true;
        backend->terminate_sent = true;
        backend->kill_deadline = seconds_after(now, 1);
        return;
    }
    if (backend->terminate_sent && !backend->kill_sent &&
        compare_timespec(now, backend->kill_deadline) >= 0) {
        (void)kill(backend->child_pid, SIGKILL);
        backend->kill_sent = true;
    }
}

int wifi_backend_init(WifiBackend *backend)
{
    const char *override;

    if (backend == NULL) {
        errno = EINVAL;
        return -1;
    }
    memset(backend, 0, sizeof(*backend));
    backend->child_pid = -1;
    backend->stdout_fd = -1;
    backend->stderr_fd = -1;
    backend->stage = (int)STAGE_NONE;
    backend->job = WIFI_JOB_NONE;
    backend->status = WIFI_STATUS_IDLE;
    backend->last_exit_code = -1;
    backend->stdout_capacity = WIFI_STDOUT_CAPACITY;
    backend->stderr_capacity = WIFI_STDERR_CAPACITY;
    backend->stdout_data = calloc(backend->stdout_capacity + 1u, 1u);
    backend->stderr_data = calloc(backend->stderr_capacity + 1u, 1u);
    if (backend->stdout_data == NULL || backend->stderr_data == NULL) {
        wifi_backend_destroy(backend);
        errno = ENOMEM;
        return -1;
    }

    override = getenv("WIN31X_NMCLI");
    if (override != NULL && override[0] != '\0') {
        if (override[0] != '/' || strlen(override) >= sizeof(backend->nmcli_path)) {
            set_status(backend, WIFI_STATUS_UNAVAILABLE,
                       "WIN31X_NMCLI must be an absolute path.");
            return 0;
        }
        (void)snprintf(backend->nmcli_path, sizeof(backend->nmcli_path),
                       "%s", override);
    } else {
        (void)snprintf(backend->nmcli_path, sizeof(backend->nmcli_path),
                       "%s", "/usr/bin/nmcli");
    }
    if (access(backend->nmcli_path, X_OK) < 0) {
        set_status(backend, WIFI_STATUS_UNAVAILABLE,
                   "NetworkManager is not installed.");
        return 0;
    }
    set_status(backend, WIFI_STATUS_IDLE, "Wi-Fi controls are ready.");
    return 0;
}

void wifi_backend_destroy(WifiBackend *backend)
{
    if (backend == NULL)
        return;
    if (backend->child_pid > 0 && !backend->child_exited) {
        int status;
        pid_t waited;

        (void)kill(backend->child_pid, SIGKILL);
        do {
            waited = waitpid(backend->child_pid, &status, 0);
        } while (waited < 0 && errno == EINTR);
    }
    (void)close_checked(&backend->stdout_fd);
    (void)close_checked(&backend->stderr_fd);
    clear_password(backend);
    if (backend->stdout_data != NULL) {
        secure_clear(backend->stdout_data, backend->stdout_capacity + 1u);
        free(backend->stdout_data);
    }
    if (backend->stderr_data != NULL) {
        secure_clear(backend->stderr_data, backend->stderr_capacity + 1u);
        free(backend->stderr_data);
    }
    secure_clear(backend, sizeof(*backend));
    backend->child_pid = -1;
    backend->stdout_fd = -1;
    backend->stderr_fd = -1;
}

int wifi_backend_start_scan(WifiBackend *backend)
{
    if (backend == NULL) {
        errno = EINVAL;
        return -1;
    }
    if (wifi_backend_busy(backend)) {
        errno = EBUSY;
        return -1;
    }
    if (backend->nmcli_path[0] == '\0' ||
        access(backend->nmcli_path, X_OK) < 0) {
        set_status(backend, WIFI_STATUS_UNAVAILABLE,
                   "NetworkManager is not installed.");
        errno = ENOENT;
        return -1;
    }
    backend->job = WIFI_JOB_SCAN;
    set_status(backend, WIFI_STATUS_WORKING, "Scanning for Wi-Fi networks...");
    if (start_scan_command(backend) < 0)
        return -1;
    return 0;
}

int wifi_backend_start_connect(WifiBackend *backend,
                               const WifiNetwork *network,
                               const unsigned char *password,
                               size_t password_len)
{
    int count;

    if (backend == NULL || network == NULL) {
        errno = EINVAL;
        return -1;
    }
    if (wifi_backend_busy(backend)) {
        errno = EBUSY;
        return -1;
    }
    if (!wifi_backend_network_supported(network)) {
        set_status(backend, WIFI_STATUS_UNSUPPORTED,
                   "This Wi-Fi security type is not supported.");
        errno = ENOTSUP;
        return -1;
    }
    if (canonicalize_selected_network(backend, network) < 0 ||
        !valid_password(&backend->selected, password, password_len)) {
        set_status(backend, WIFI_STATUS_INVALID_DATA,
                   "The Wi-Fi network or password is invalid.");
        errno = EINVAL;
        return -1;
    }
    clear_password(backend);
    if (password_len > 0) {
        backend->password = malloc(password_len);
        if (backend->password == NULL)
            return -1;
        memcpy(backend->password, password, password_len);
        backend->password_len = password_len;
    }
    count = snprintf(backend->profile_id, sizeof(backend->profile_id),
                     "Win31 X Wi-Fi %s-%s",
                     security_profile_name(backend->selected.security),
                     backend->selected.ssid_hex);
    if (count < 0 || (size_t)count >= sizeof(backend->profile_id)) {
        clear_password(backend);
        set_status(backend, WIFI_STATUS_INVALID_DATA,
                   "The Wi-Fi profile name is too long.");
        errno = EOVERFLOW;
        return -1;
    }
    backend->profile_uuid[0] = '\0';
    backend->profile_created = false;
    backend->job = WIFI_JOB_CONNECT;
    set_status(backend, WIFI_STATUS_WORKING, "Connecting to Wi-Fi...");
    if (start_profile_lookup(backend) < 0) {
        clear_password(backend);
        return -1;
    }
    return 0;
}

int wifi_backend_start_disconnect(WifiBackend *backend, const char *device)
{
    if (backend == NULL || !valid_device_name(device)) {
        errno = EINVAL;
        return -1;
    }
    if (wifi_backend_busy(backend)) {
        errno = EBUSY;
        return -1;
    }
    memset(&backend->selected, 0, sizeof(backend->selected));
    (void)snprintf(backend->selected.device,
                   sizeof(backend->selected.device), "%s", device);
    backend->job = WIFI_JOB_DISCONNECT;
    set_status(backend, WIFI_STATUS_WORKING, "Disconnecting Wi-Fi...");
    if (start_disconnect_command(backend, backend->selected.device) < 0)
        return -1;
    return 0;
}

static void fail_unsafe_descriptors(WifiBackend *backend)
{
    backend->descriptor_error = true;
    backend->pipe_grace_active = false;
    (void)close_checked(&backend->stdout_fd);
    (void)close_checked(&backend->stderr_fd);
    backend->stdout_eof = true;
    backend->stderr_eof = true;
    set_status(backend, WIFI_STATUS_FAILED,
               "A Wi-Fi process descriptor cannot be monitored safely.");
    if (backend->child_pid > 0 && !backend->child_exited) {
        (void)kill(backend->child_pid, SIGKILL);
        backend->kill_sent = true;
    }
    maybe_finish_stage(backend);
}

void wifi_backend_add_select_fds(WifiBackend *backend,
                                 fd_set *read_fds, int *maximum_fd)
{
    if (backend == NULL || read_fds == NULL || maximum_fd == NULL)
        return;
    if (backend->stdout_fd >= FD_SETSIZE ||
        backend->stderr_fd >= FD_SETSIZE) {
        fail_unsafe_descriptors(backend);
        return;
    }
    if (backend->stdout_fd >= 0) {
        FD_SET(backend->stdout_fd, read_fds);
        if (backend->stdout_fd > *maximum_fd)
            *maximum_fd = backend->stdout_fd;
    }
    if (backend->stderr_fd >= 0) {
        FD_SET(backend->stderr_fd, read_fds);
        if (backend->stderr_fd > *maximum_fd)
            *maximum_fd = backend->stderr_fd;
    }
}

void wifi_backend_dispatch_fds(WifiBackend *backend,
                                const fd_set *read_fds)
{
    if (backend == NULL)
        return;
    if (backend->stdout_fd >= FD_SETSIZE ||
        backend->stderr_fd >= FD_SETSIZE) {
        fail_unsafe_descriptors(backend);
        return;
    }
    if (read_fds != NULL) {
        if (backend->stdout_fd >= 0 &&
            FD_ISSET(backend->stdout_fd, read_fds))
            drain_descriptor(backend, true);
        if (backend->stderr_fd >= 0 &&
            FD_ISSET(backend->stderr_fd, read_fds))
            drain_descriptor(backend, false);
    }
    terminate_for_timeout(backend);
    maybe_finish_stage(backend);
}

void wifi_backend_tick(WifiBackend *backend)
{
    if (backend == NULL)
        return;
    terminate_for_timeout(backend);
    maybe_finish_stage(backend);
}

bool wifi_backend_handle_child_exit(WifiBackend *backend, pid_t pid,
                                    int wait_status)
{
    if (backend == NULL || pid <= 0 || pid != backend->child_pid)
        return false;
    backend->child_exited = true;
    backend->child_status = wait_status;
    drain_descriptor(backend, true);
    drain_descriptor(backend, false);
    if (!backend->stdout_eof || !backend->stderr_eof) {
        backend->pipe_grace_active = true;
        backend->pipe_deadline = seconds_after(monotonic_now(), 1);
    }
    maybe_finish_stage(backend);
    return true;
}

bool wifi_backend_busy(const WifiBackend *backend)
{
    return backend != NULL && backend->stage != (int)STAGE_NONE;
}

WifiJob wifi_backend_job(const WifiBackend *backend)
{
    return backend != NULL ? backend->job : WIFI_JOB_NONE;
}

WifiStatus wifi_backend_status(const WifiBackend *backend)
{
    return backend != NULL ? backend->status : WIFI_STATUS_FAILED;
}

const char *wifi_backend_status_text(const WifiBackend *backend)
{
    return backend != NULL ? backend->status_text : "Wi-Fi backend is invalid.";
}

int wifi_backend_last_exit_code(const WifiBackend *backend)
{
    return backend != NULL ? backend->last_exit_code : -1;
}

size_t wifi_backend_network_count(const WifiBackend *backend)
{
    return backend != NULL ? backend->network_count : 0;
}

const WifiNetwork *wifi_backend_network_at(const WifiBackend *backend,
                                           size_t index)
{
    if (backend == NULL || index >= backend->network_count)
        return NULL;
    return &backend->networks[index];
}

bool wifi_backend_network_supported(const WifiNetwork *network)
{
    return network != NULL &&
           (network->security == WIFI_SECURITY_OPEN ||
            network->security == WIFI_SECURITY_OWE ||
            network->security == WIFI_SECURITY_WPA_PSK ||
            network->security == WIFI_SECURITY_SAE);
}
