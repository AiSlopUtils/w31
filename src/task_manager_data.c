#define _GNU_SOURCE
#define _POSIX_C_SOURCE 200809L

#include "task_manager_data.h"

#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <limits.h>
#include <math.h>
#include <signal.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/utsname.h>
#include <unistd.h>

#if defined(__linux__)
#include <sys/syscall.h>
#endif

#define TASK_MANAGER_SMALL_FILE_LIMIT (64U * 1024U)
#define TASK_MANAGER_STATUS_FILE_LIMIT (256U * 1024U)
#define TASK_MANAGER_CPUINFO_FILE_LIMIT (2U * 1024U * 1024U)
#define TASK_MANAGER_PROCESS_LIMIT WIN31X_TASK_MANAGER_PROCESS_LIMIT

typedef struct {
    pid_t pid;
    uint64_t start_time;
    uint64_t total_ticks;
} ProcessSample;

typedef struct {
    Win31xTaskManagerProcess process;
    ProcessSample sample;
} ProcessCandidate;

static int open_process_handle(pid_t pid)
{
#if defined(__linux__) && defined(SYS_pidfd_open) &&                         \
    defined(SYS_pidfd_send_signal)
    long result;

    do {
        result = syscall(SYS_pidfd_open, pid, 0U);
    } while (result < 0L && errno == EINTR);
    if (result < 0L) {
        if (errno == ENOSYS)
            errno = ENOTSUP;
        return -1;
    }
    if (result > INT_MAX) {
        (void)close((int)result);
        errno = EMFILE;
        return -1;
    }
    return (int)result;
#else
    (void)pid;
    errno = ENOTSUP;
    return -1;
#endif
}

static int signal_process_handle(int handle, int signal_number)
{
#if defined(__linux__) && defined(SYS_pidfd_open) &&                         \
    defined(SYS_pidfd_send_signal)
    if (handle >= 0) {
        long result;

        do {
            result = syscall(SYS_pidfd_send_signal, handle, signal_number,
                             NULL, 0U);
        } while (result < 0L && errno == EINTR);
        if (result < 0L && errno == ENOSYS)
            errno = ENOTSUP;
        return result < 0L ? -1 : 0;
    }
#else
    (void)handle;
    (void)signal_number;
#endif
    errno = ENOTSUP;
    return -1;
}

static void set_error(char *destination, size_t capacity,
                      const char *format, ...)
{
    va_list arguments;

    if (destination == NULL || capacity == 0U)
        return;
    va_start(arguments, format);
    (void)vsnprintf(destination, capacity, format, arguments);
    va_end(arguments);
}

static uint64_t add_saturated(uint64_t left, uint64_t right)
{
    if (UINT64_MAX - left < right)
        return UINT64_MAX;
    return left + right;
}

static uint64_t multiply_saturated(uint64_t left, uint64_t right)
{
    if (left != 0U && right > UINT64_MAX / left)
        return UINT64_MAX;
    return left * right;
}

static int copy_path(char *destination, size_t capacity, const char *source)
{
    size_t length;

    if (source == NULL || source[0] != '/') {
        errno = EINVAL;
        return -1;
    }
    length = strlen(source);
    while (length > 1U && source[length - 1U] == '/')
        --length;
    if (length >= capacity) {
        errno = ENAMETOOLONG;
        return -1;
    }
    memcpy(destination, source, length);
    destination[length] = '\0';
    return 0;
}

static int make_path(char *destination, size_t capacity, const char *root,
                     const char *leaf)
{
    int written;

    written = snprintf(destination, capacity, "%s/%s", root, leaf);
    if (written < 0 || (size_t)written >= capacity) {
        errno = ENAMETOOLONG;
        return -1;
    }
    return 0;
}

static int make_process_path(char *destination, size_t capacity,
                             const char *root, pid_t pid, const char *leaf)
{
    int written;

    written = snprintf(destination, capacity, "%s/%ld/%s", root, (long)pid,
                       leaf);
    if (written < 0 || (size_t)written >= capacity) {
        errno = ENAMETOOLONG;
        return -1;
    }
    return 0;
}

static int read_limited_file(const char *path, size_t limit, char **contents,
                             size_t *length)
{
    char *buffer;
    size_t used = 0U;
    size_t capacity = 4096U;
    int descriptor;

    if (contents == NULL || length == NULL || limit == 0U) {
        errno = EINVAL;
        return -1;
    }
    *contents = NULL;
    *length = 0U;
    if (capacity > limit)
        capacity = limit;
    buffer = malloc(capacity + 1U);
    if (buffer == NULL)
        return -1;
    descriptor = open(path, O_RDONLY | O_CLOEXEC);
    if (descriptor < 0) {
        free(buffer);
        return -1;
    }
    for (;;) {
        ssize_t count;

        if (used == capacity) {
            char *larger;
            size_t next_capacity;

            if (capacity == limit) {
                char extra;

                count = read(descriptor, &extra, 1U);
                if (count < 0 && errno == EINTR)
                    continue;
                if (count > 0) {
                    (void)close(descriptor);
                    free(buffer);
                    errno = EFBIG;
                    return -1;
                }
                if (count < 0) {
                    int saved_errno = errno;

                    (void)close(descriptor);
                    free(buffer);
                    errno = saved_errno;
                    return -1;
                }
                break;
            }
            next_capacity = capacity > limit / 2U ? limit : capacity * 2U;
            larger = realloc(buffer, next_capacity + 1U);
            if (larger == NULL) {
                int saved_errno = errno;

                (void)close(descriptor);
                free(buffer);
                errno = saved_errno;
                return -1;
            }
            buffer = larger;
            capacity = next_capacity;
        }
        count = read(descriptor, buffer + used, capacity - used);
        if (count < 0) {
            int saved_errno;

            if (errno == EINTR)
                continue;
            saved_errno = errno;
            (void)close(descriptor);
            free(buffer);
            errno = saved_errno;
            return -1;
        }
        if (count == 0)
            break;
        used += (size_t)count;
    }
    if (close(descriptor) < 0) {
        int saved_errno = errno;

        free(buffer);
        errno = saved_errno;
        return -1;
    }
    buffer[used] = '\0';
    *contents = buffer;
    *length = used;
    return 0;
}

static bool parse_unsigned_token(const char *token, uint64_t *value)
{
    char *end;
    uintmax_t parsed;

    if (token == NULL || token[0] == '\0' || token[0] == '-')
        return false;
    errno = 0;
    parsed = strtoumax(token, &end, 10);
    if (errno != 0 || end == token || *end != '\0' || parsed > UINT64_MAX)
        return false;
    *value = (uint64_t)parsed;
    return true;
}

static bool parse_signed_token(const char *token, int64_t *value)
{
    char *end;
    intmax_t parsed;

    if (token == NULL || token[0] == '\0')
        return false;
    errno = 0;
    parsed = strtoimax(token, &end, 10);
    if (errno != 0 || end == token || *end != '\0' ||
        parsed < INT64_MIN || parsed > INT64_MAX)
        return false;
    *value = (int64_t)parsed;
    return true;
}

static char *next_space_token(char **cursor)
{
    char *begin = *cursor;
    char *end;

    while (*begin != '\0' && isspace((unsigned char)*begin))
        ++begin;
    if (*begin == '\0') {
        *cursor = begin;
        return NULL;
    }
    end = begin;
    while (*end != '\0' && !isspace((unsigned char)*end))
        ++end;
    if (*end != '\0')
        *end++ = '\0';
    *cursor = end;
    return begin;
}

static void copy_clean_text(char *destination, size_t capacity,
                            const char *source, size_t source_length)
{
    size_t input;
    size_t output = 0U;

    if (capacity == 0U)
        return;
    for (input = 0U; input < source_length && output + 1U < capacity;
         ++input) {
        unsigned char character = (unsigned char)source[input];

        if (character == '\0' || character == '\r' || character == '\n' ||
            character == '\t')
            character = ' ';
        else if (character < 32U || character == 127U)
            character = '?';
        if (character == ' ' &&
            (output == 0U || destination[output - 1U] == ' '))
            continue;
        destination[output++] = (char)character;
    }
    while (output > 0U && destination[output - 1U] == ' ')
        --output;
    destination[output] = '\0';
}

static int parse_process_stat(char *contents, pid_t expected_pid,
                              ProcessCandidate *candidate)
{
    char *open_parenthesis;
    char *close_parenthesis;
    char *cursor;
    char *token;
    char *pid_end;
    intmax_t parsed_pid;
    unsigned int field = 4U;
    uint64_t user_ticks = 0U;
    uint64_t system_ticks = 0U;
    uint64_t start_time = 0U;
    int64_t resident_pages = 0;
    bool have_user = false;
    bool have_system = false;
    bool have_start = false;
    bool have_resident = false;

    errno = 0;
    parsed_pid = strtoimax(contents, &pid_end, 10);
    if (errno != 0 || pid_end == contents || parsed_pid != (intmax_t)expected_pid)
        return -1;
    while (*pid_end != '\0' && isspace((unsigned char)*pid_end))
        ++pid_end;
    if (*pid_end != '(')
        return -1;
    open_parenthesis = pid_end;
    close_parenthesis = strrchr(open_parenthesis + 1, ')');
    if (close_parenthesis == NULL)
        return -1;
    cursor = close_parenthesis + 1;
    while (*cursor != '\0' && isspace((unsigned char)*cursor))
        ++cursor;
    if (*cursor == '\0')
        return -1;
    candidate->process.state = *cursor++;
    if (*cursor != '\0' && !isspace((unsigned char)*cursor))
        return -1;
    copy_clean_text(candidate->process.name,
                    sizeof(candidate->process.name), open_parenthesis + 1,
                    (size_t)(close_parenthesis - open_parenthesis - 1));

    while ((token = next_space_token(&cursor)) != NULL) {
        if (field == 14U) {
            have_user = parse_unsigned_token(token, &user_ticks);
            if (!have_user)
                return -1;
        } else if (field == 15U) {
            have_system = parse_unsigned_token(token, &system_ticks);
            if (!have_system)
                return -1;
        } else if (field == 22U) {
            have_start = parse_unsigned_token(token, &start_time);
            if (!have_start)
                return -1;
        } else if (field == 24U) {
            have_resident = parse_signed_token(token, &resident_pages);
            if (!have_resident)
                return -1;
            break;
        }
        ++field;
    }
    if (!have_user || !have_system || !have_start || !have_resident)
        return -1;
    candidate->sample.pid = expected_pid;
    candidate->sample.start_time = start_time;
    candidate->sample.total_ticks = add_saturated(user_ticks, system_ticks);
    candidate->process.start_time_ticks = start_time;
    if (resident_pages > 0)
        candidate->process.resident_bytes = (uint64_t)resident_pages;
    return 0;
}

static bool find_status_uid(const char *contents, uid_t *uid)
{
    const char *line = contents;

    while (*line != '\0') {
        const char *end = strchr(line, '\n');

        if (strncmp(line, "Uid:", 4U) == 0) {
            char *number_end;
            uintmax_t parsed;
            const char *number = line + 4;

            while (*number != '\0' && isspace((unsigned char)*number))
                ++number;
            errno = 0;
            parsed = strtoumax(number, &number_end, 10);
            if (errno != 0 || number_end == number ||
                parsed > (uintmax_t)(uid_t)-1 ||
                (*number_end != '\0' &&
                 !isspace((unsigned char)*number_end)))
                return false;
            if (end != NULL && number_end > end)
                return false;
            *uid = (uid_t)parsed;
            return true;
        }
        if (end == NULL)
            break;
        line = end + 1;
    }
    return false;
}

static int read_process(const Win31xTaskManagerData *data, pid_t pid,
                        ProcessCandidate *candidate)
{
    char path[WIN31X_TASK_MANAGER_PATH_CAPACITY];
    char *contents = NULL;
    size_t length;
    uid_t uid;
    ProcessCandidate verification;

    memset(candidate, 0, sizeof(*candidate));
    candidate->process.pid = pid;
    if (make_process_path(path, sizeof(path), data->proc_root, pid, "stat") < 0 ||
        read_limited_file(path, TASK_MANAGER_SMALL_FILE_LIMIT, &contents,
                          &length) < 0)
        return -1;
    if (parse_process_stat(contents, pid, candidate) < 0) {
        free(contents);
        errno = EPROTO;
        return -1;
    }
    free(contents);
    contents = NULL;

    if (make_process_path(path, sizeof(path), data->proc_root, pid, "status") < 0 ||
        read_limited_file(path, TASK_MANAGER_STATUS_FILE_LIMIT, &contents,
                          &length) < 0)
        return -1;
    if (!find_status_uid(contents, &uid)) {
        free(contents);
        errno = EPROTO;
        return -1;
    }
    free(contents);
    candidate->process.uid = uid;
    candidate->process.owned_by_user = uid == data->user_id;

    /* Discard a row assembled across PID reuse instead of showing mixed data. */
    if (make_process_path(path, sizeof(path), data->proc_root, pid, "stat") < 0 ||
        read_limited_file(path, TASK_MANAGER_SMALL_FILE_LIMIT, &contents,
                          &length) < 0)
        return -1;
    memset(&verification, 0, sizeof(verification));
    if (parse_process_stat(contents, pid, &verification) < 0 ||
        verification.sample.start_time != candidate->sample.start_time) {
        free(contents);
        errno = ESRCH;
        return -1;
    }
    free(contents);
    candidate->sample = verification.sample;
    candidate->process.start_time_ticks = verification.sample.start_time;
    candidate->process.state = verification.process.state;
    candidate->process.resident_bytes = verification.process.resident_bytes;
    (void)snprintf(candidate->process.name,
                   sizeof(candidate->process.name), "%s",
                   verification.process.name);
    (void)snprintf(candidate->process.command,
                   sizeof(candidate->process.command), "%s",
                   candidate->process.name);
    candidate->process.resident_bytes = multiply_saturated(
        candidate->process.resident_bytes, (uint64_t)data->page_size);
    return 0;
}

static int process_candidate_compare(const void *left_pointer,
                                     const void *right_pointer)
{
    const ProcessCandidate *left = left_pointer;
    const ProcessCandidate *right = right_pointer;

    if (left->process.pid < right->process.pid)
        return -1;
    if (left->process.pid > right->process.pid)
        return 1;
    return 0;
}

static int process_sample_compare(const void *left_pointer,
                                  const void *right_pointer)
{
    const ProcessSample *left = left_pointer;
    const ProcessSample *right = right_pointer;

    if (left->pid < right->pid)
        return -1;
    if (left->pid > right->pid)
        return 1;
    return 0;
}

static const ProcessSample *find_previous_sample(
    const Win31xTaskManagerData *data, pid_t pid)
{
    ProcessSample key;

    if (data->previous_process_sample_count == 0U)
        return NULL;

    key.pid = pid;
    key.start_time = 0U;
    key.total_ticks = 0U;
    return bsearch(&key, data->previous_process_samples,
                   data->previous_process_sample_count,
                   sizeof(ProcessSample), process_sample_compare);
}

static bool directory_name_to_pid(const char *name, pid_t *pid)
{
    const unsigned char *cursor = (const unsigned char *)name;
    char *end;
    uintmax_t parsed;

    if (*cursor == '\0')
        return false;
    while (*cursor != '\0') {
        if (!isdigit(*cursor))
            return false;
        ++cursor;
    }
    errno = 0;
    parsed = strtoumax(name, &end, 10);
    if (errno != 0 || *end != '\0' || parsed == 0U || parsed > INT_MAX)
        return false;
    *pid = (pid_t)parsed;
    return true;
}

static int append_candidate(ProcessCandidate **candidates, size_t *count,
                            size_t *capacity,
                            const ProcessCandidate *candidate)
{
    if (*count == *capacity) {
        ProcessCandidate *larger;
        size_t next_capacity = *capacity == 0U ? 64U : *capacity * 2U;

        if (next_capacity < *capacity ||
            next_capacity > SIZE_MAX / sizeof(**candidates)) {
            errno = ENOMEM;
            return -1;
        }
        larger = realloc(*candidates, next_capacity * sizeof(**candidates));
        if (larger == NULL)
            return -1;
        *candidates = larger;
        *capacity = next_capacity;
    }
    (*candidates)[(*count)++] = *candidate;
    return 0;
}

static int collect_processes(const Win31xTaskManagerData *data,
                             ProcessCandidate **result, size_t *result_count,
                             bool *truncated)
{
    DIR *directory;
    struct dirent *entry;
    ProcessCandidate *candidates = NULL;
    size_t count = 0U;
    size_t capacity = 0U;
    size_t process_limit = data->process_limit;
    int read_error = 0;

    *truncated = false;
    if (process_limit == 0U || process_limit > TASK_MANAGER_PROCESS_LIMIT)
        process_limit = TASK_MANAGER_PROCESS_LIMIT;

    directory = opendir(data->proc_root);
    if (directory == NULL)
        return -1;
    errno = 0;
    while ((entry = readdir(directory)) != NULL) {
        ProcessCandidate candidate;
        pid_t pid;

        if (!directory_name_to_pid(entry->d_name, &pid)) {
            errno = 0;
            continue;
        }
        if (read_process(data, pid, &candidate) == 0) {
            if (append_candidate(&candidates, &count, &capacity,
                                 &candidate) < 0) {
                read_error = errno;
                break;
            }
            if (count == process_limit) {
                *truncated = true;
                break;
            }
        } else {
            int candidate_error = errno;

            /*
             * Individual proc entries routinely vanish, become inaccessible,
             * or contain a short/malformed view during a racing exit.  Those
             * rows can be omitted.  Resource and global I/O failures must
             * abort the refresh so the last complete snapshot remains live.
             */
            if (candidate_error != ENOENT && candidate_error != ESRCH &&
                candidate_error != EACCES && candidate_error != EPERM &&
                candidate_error != EPROTO && candidate_error != EFBIG &&
                candidate_error != ENOTDIR && candidate_error != ELOOP) {
                read_error = candidate_error;
                break;
            }
        }
        errno = 0;
    }
    if (entry == NULL && errno != 0)
        read_error = errno;
    if (closedir(directory) < 0 && read_error == 0)
        read_error = errno;
    if (read_error != 0) {
        free(candidates);
        errno = read_error;
        return -1;
    }
    if (count > 1U)
        qsort(candidates, count, sizeof(*candidates),
              process_candidate_compare);
    *result = candidates;
    *result_count = count;
    return 0;
}

static int parse_cpu_totals(char *contents, uint64_t *total,
                            uint64_t *idle)
{
    char *cursor;
    char *token;
    uint64_t fields[10];
    size_t count = 0U;
    size_t index;

    if (strncmp(contents, "cpu", 3U) != 0 ||
        !isspace((unsigned char)contents[3])) {
        errno = EPROTO;
        return -1;
    }
    cursor = contents + 3;
    while (count < sizeof(fields) / sizeof(fields[0]) &&
           (token = next_space_token(&cursor)) != NULL) {
        if (!parse_unsigned_token(token, &fields[count])) {
            errno = EPROTO;
            return -1;
        }
        ++count;
    }
    if (count < 4U) {
        errno = EPROTO;
        return -1;
    }
    *total = 0U;
    /* guest and guest_nice (fields 9 and 10) are included in user/nice. */
    for (index = 0U; index < count && index < 8U; ++index)
        *total = add_saturated(*total, fields[index]);
    *idle = fields[3];
    if (count > 4U)
        *idle = add_saturated(*idle, fields[4]);
    return 0;
}

static int read_cpu_totals(const Win31xTaskManagerData *data, uint64_t *total,
                           uint64_t *idle)
{
    char path[WIN31X_TASK_MANAGER_PATH_CAPACITY];
    char *contents;
    size_t length;
    int result;

    if (make_path(path, sizeof(path), data->proc_root, "stat") < 0 ||
        read_limited_file(path, TASK_MANAGER_SMALL_FILE_LIMIT, &contents,
                          &length) < 0)
        return -1;
    (void)length;
    {
        char *line_end = strchr(contents, '\n');

        if (line_end != NULL)
            *line_end = '\0';
    }
    result = parse_cpu_totals(contents, total, idle);
    free(contents);
    return result;
}

/* Return 1 for a valid value, 0 when the key is absent, and -1 if malformed. */
static int read_meminfo_value(const char *contents, const char *key,
                              uint64_t *value)
{
    size_t key_length = strlen(key);
    const char *line = contents;

    while (*line != '\0') {
        const char *newline = strchr(line, '\n');
        const char *end = newline == NULL ? line + strlen(line) : newline;

        if (strncmp(line, key, key_length) == 0) {
            const char *number = line + key_length;
            char *number_end;
            const char *unit;
            uintmax_t parsed;

            while (number < end && isspace((unsigned char)*number))
                ++number;
            if (number == end || !isdigit((unsigned char)*number))
                goto malformed;
            errno = 0;
            parsed = strtoumax(number, &number_end, 10);
            if (errno != 0 || number_end == number || number_end > end ||
                parsed > UINT64_MAX / 1024U)
                goto malformed;
            if (number_end == end ||
                !isspace((unsigned char)*number_end))
                goto malformed;
            unit = number_end;
            while (unit < end && isspace((unsigned char)*unit))
                ++unit;
            if ((size_t)(end - unit) < 2U || unit[0] != 'k' ||
                unit[1] != 'B')
                goto malformed;
            unit += 2;
            while (unit < end && isspace((unsigned char)*unit))
                ++unit;
            if (unit != end)
                goto malformed;
            *value = (uint64_t)parsed * 1024U;
            return 1;

malformed:
            errno = EPROTO;
            return -1;
        }
        if (newline == NULL)
            break;
        line = newline + 1;
    }
    return 0;
}

static int read_memory(const Win31xTaskManagerData *data, uint64_t *total,
                       uint64_t *available)
{
    char path[WIN31X_TASK_MANAGER_PATH_CAPACITY];
    char *contents;
    size_t length;
    uint64_t free_memory = 0U;
    uint64_t buffers = 0U;
    uint64_t cached = 0U;
    int parse_result;

    if (make_path(path, sizeof(path), data->proc_root, "meminfo") < 0 ||
        read_limited_file(path, TASK_MANAGER_SMALL_FILE_LIMIT, &contents,
                          &length) < 0)
        return -1;
    (void)length;
    parse_result = read_meminfo_value(contents, "MemTotal:", total);
    if (parse_result != 1) {
        free(contents);
        errno = EPROTO;
        return -1;
    }
    parse_result = read_meminfo_value(contents, "MemAvailable:", available);
    if (parse_result < 0)
        goto malformed;
    if (parse_result == 0) {
        parse_result = read_meminfo_value(contents, "MemFree:", &free_memory);
        if (parse_result < 0)
            goto malformed;
        parse_result = read_meminfo_value(contents, "Buffers:", &buffers);
        if (parse_result < 0)
            goto malformed;
        parse_result = read_meminfo_value(contents, "Cached:", &cached);
        if (parse_result < 0)
            goto malformed;
        *available = add_saturated(add_saturated(free_memory, buffers), cached);
    }
    if (*available > *total)
        *available = *total;
    free(contents);
    return 0;

malformed:
    free(contents);
    errno = EPROTO;
    return -1;
}

static bool parse_nonnegative_double(const char *text, char **end,
                                     double *value)
{
    double parsed;

    errno = 0;
    parsed = strtod(text, end);
    if (errno != 0 || *end == text || !isfinite(parsed) || parsed < 0.0)
        return false;
    *value = parsed;
    return true;
}

static void read_uptime(const Win31xTaskManagerData *data,
                        Win31xTaskManagerSystem *system)
{
    char path[WIN31X_TASK_MANAGER_PATH_CAPACITY];
    char *contents;
    size_t length;
    char *end;
    double value;

    if (make_path(path, sizeof(path), data->proc_root, "uptime") < 0 ||
        read_limited_file(path, TASK_MANAGER_SMALL_FILE_LIMIT, &contents,
                          &length) < 0)
        return;
    (void)length;
    if (parse_nonnegative_double(contents, &end, &value))
        system->uptime_seconds = value;
    free(contents);
}

static void read_load_average(const Win31xTaskManagerData *data,
                              Win31xTaskManagerSystem *system)
{
    char path[WIN31X_TASK_MANAGER_PATH_CAPACITY];
    char *contents;
    size_t length;
    char *cursor;
    char *end;
    size_t index;

    if (make_path(path, sizeof(path), data->proc_root, "loadavg") < 0 ||
        read_limited_file(path, TASK_MANAGER_SMALL_FILE_LIMIT, &contents,
                          &length) < 0)
        return;
    (void)length;
    cursor = contents;
    for (index = 0U; index < 3U; ++index) {
        while (*cursor != '\0' && isspace((unsigned char)*cursor))
            ++cursor;
        if (!parse_nonnegative_double(cursor, &end,
                                      &system->load_average[index]))
            break;
        cursor = end;
    }
    free(contents);
}

static void read_trimmed_text_file(const char *path, char *destination,
                                   size_t capacity)
{
    char *contents;
    size_t length;

    if (read_limited_file(path, TASK_MANAGER_SMALL_FILE_LIMIT, &contents,
                          &length) < 0)
        return;
    copy_clean_text(destination, capacity, contents, length);
    free(contents);
}

static bool os_release_value(const char *contents, const char *key,
                             char *destination, size_t capacity)
{
    const char *line = contents;
    size_t key_length = strlen(key);

    while (*line != '\0') {
        const char *line_end = strchr(line, '\n');
        size_t line_length = line_end == NULL ? strlen(line)
                                              : (size_t)(line_end - line);

        if (line_length > key_length + 1U &&
            strncmp(line, key, key_length) == 0 && line[key_length] == '=') {
            const char *value = line + key_length + 1U;
            const char *value_end = line + line_length;
            size_t output = 0U;
            bool quoted = value < value_end && *value == '"';

            if (quoted)
                ++value;
            while (value < value_end && output + 1U < capacity) {
                unsigned char character = (unsigned char)*value++;

                if (quoted && character == '"')
                    break;
                if (character == '\\' && value < value_end)
                    character = (unsigned char)*value++;
                if (character < 32U || character == 127U)
                    character = '?';
                destination[output++] = (char)character;
            }
            while (output > 0U && isspace((unsigned char)destination[output - 1U]))
                --output;
            destination[output] = '\0';
            return output > 0U;
        }
        if (line_end == NULL)
            break;
        line = line_end + 1;
    }
    return false;
}

static void read_operating_system(const Win31xTaskManagerData *data,
                                  Win31xTaskManagerSystem *system,
                                  const struct utsname *identity)
{
    char *contents;
    size_t length;

    if (read_limited_file(data->os_release_path,
                          TASK_MANAGER_SMALL_FILE_LIMIT, &contents,
                          &length) == 0) {
        (void)length;
        if (!os_release_value(contents, "PRETTY_NAME",
                              system->operating_system,
                              sizeof(system->operating_system)))
            (void)os_release_value(contents, "NAME", system->operating_system,
                                   sizeof(system->operating_system));
        free(contents);
    }
    if (system->operating_system[0] == '\0' && identity != NULL)
        (void)snprintf(system->operating_system,
                       sizeof(system->operating_system), "%s",
                       identity->sysname);
}

static bool cpuinfo_line_value(const char *line, size_t line_length,
                               const char *key, char *destination,
                               size_t capacity)
{
    const char *colon;
    const char *value;
    size_t key_end;

    colon = memchr(line, ':', line_length);
    if (colon == NULL)
        return false;
    key_end = (size_t)(colon - line);
    while (key_end > 0U && isspace((unsigned char)line[key_end - 1U]))
        --key_end;
    if (strlen(key) != key_end || strncmp(line, key, key_end) != 0)
        return false;
    value = colon + 1;
    while (value < line + line_length && isspace((unsigned char)*value))
        ++value;
    copy_clean_text(destination, capacity, value,
                    (size_t)((line + line_length) - value));
    return destination[0] != '\0';
}

static void read_cpu_information(const Win31xTaskManagerData *data,
                                 Win31xTaskManagerSystem *system)
{
    static const char *const model_keys[] = {
        "model name", "Model", "Processor", "Hardware", "cpu model"
    };
    char path[WIN31X_TASK_MANAGER_PATH_CAPACITY];
    char *contents;
    size_t length;
    const char *line;
    unsigned int processor_lines = 0U;
    size_t model_priority = sizeof(model_keys) / sizeof(model_keys[0]);

    if (make_path(path, sizeof(path), data->proc_root, "cpuinfo") < 0 ||
        read_limited_file(path, TASK_MANAGER_CPUINFO_FILE_LIMIT, &contents,
                          &length) < 0)
        goto fallback;
    (void)length;
    line = contents;
    while (*line != '\0') {
        const char *line_end = strchr(line, '\n');
        size_t line_length = line_end == NULL ? strlen(line)
                                              : (size_t)(line_end - line);
        size_t key_index;
        char ignored[2];

        if (cpuinfo_line_value(line, line_length, "processor", ignored,
                               sizeof(ignored)) &&
            processor_lines < UINT_MAX)
            ++processor_lines;
        for (key_index = 0U; key_index < model_priority; ++key_index) {
            char model[WIN31X_TASK_MANAGER_LABEL_CAPACITY];

            if (cpuinfo_line_value(line, line_length, model_keys[key_index],
                                   model, sizeof(model))) {
                (void)snprintf(system->cpu_model, sizeof(system->cpu_model),
                               "%s", model);
                model_priority = key_index;
                break;
            }
        }
        if (line_end == NULL)
            break;
        line = line_end + 1;
    }
    free(contents);
    system->cpu_core_count = processor_lines;

fallback:
    if (system->cpu_core_count == 0U) {
        long configured = 1L;

#ifdef _SC_NPROCESSORS_ONLN
        configured = sysconf(_SC_NPROCESSORS_ONLN);
#elif defined(_SC_NPROCESSORS_CONF)
        configured = sysconf(_SC_NPROCESSORS_CONF);
#endif

        system->cpu_core_count = configured > 0L && configured <= UINT_MAX
                                     ? (unsigned int)configured
                                     : 1U;
    }
    if (system->cpu_model[0] == '\0')
        (void)snprintf(system->cpu_model, sizeof(system->cpu_model),
                       "Unknown processor");
}

static void read_system_identity(const Win31xTaskManagerData *data,
                                 Win31xTaskManagerSystem *system)
{
    struct utsname identity;
    struct utsname *identity_pointer = NULL;
    char path[WIN31X_TASK_MANAGER_PATH_CAPACITY];

    if (uname(&identity) == 0)
        identity_pointer = &identity;
    read_operating_system(data, system, identity_pointer);
    if (make_path(path, sizeof(path), data->proc_root,
                  "sys/kernel/osrelease") == 0)
        read_trimmed_text_file(path, system->kernel, sizeof(system->kernel));
    if (system->kernel[0] == '\0' && identity_pointer != NULL)
        (void)snprintf(system->kernel, sizeof(system->kernel), "%s",
                       identity.release);
    if (make_path(path, sizeof(path), data->proc_root,
                  "sys/kernel/hostname") == 0)
        read_trimmed_text_file(path, system->hostname,
                               sizeof(system->hostname));
    if (system->hostname[0] == '\0') {
        if (gethostname(system->hostname, sizeof(system->hostname)) < 0)
            system->hostname[0] = '\0';
        else
            system->hostname[sizeof(system->hostname) - 1U] = '\0';
    }
    read_cpu_information(data, system);
    if (strcmp(system->cpu_model, "Unknown processor") == 0 &&
        identity_pointer != NULL && identity.machine[0] != '\0')
        (void)snprintf(system->cpu_model, sizeof(system->cpu_model),
                       "%s processor", identity.machine);
}

static void calculate_percentages(const Win31xTaskManagerData *data,
                                  ProcessCandidate *candidates, size_t count,
                                  uint64_t total, uint64_t idle,
                                  Win31xTaskManagerSystem *system)
{
    uint64_t total_delta = 0U;
    uint64_t idle_delta = 0U;
    size_t index;

    if (data->has_previous_cpu_sample &&
        total >= data->previous_total_ticks &&
        idle >= data->previous_idle_ticks) {
        total_delta = total - data->previous_total_ticks;
        idle_delta = idle - data->previous_idle_ticks;
        if (total_delta > 0U && idle_delta <= total_delta) {
            system->cpu_percent =
                (double)(total_delta - idle_delta) * 100.0 /
                (double)total_delta;
            system->cpu_percent_valid = true;
        }
    }
    if (!system->cpu_percent_valid)
        return;
    for (index = 0U; index < count; ++index) {
        const ProcessSample *previous =
            find_previous_sample(data, candidates[index].sample.pid);

        if (previous != NULL &&
            previous->start_time == candidates[index].sample.start_time &&
            candidates[index].sample.total_ticks >= previous->total_ticks) {
            uint64_t process_delta =
                candidates[index].sample.total_ticks - previous->total_ticks;
            double percentage =
                (double)process_delta * 100.0 / (double)total_delta;

            if (percentage > 100.0)
                percentage = 100.0;
            candidates[index].process.cpu_percent = percentage;
            candidates[index].process.cpu_percent_valid = true;
        }
    }
}

int win31x_task_manager_data_init_at(Win31xTaskManagerData *data,
                                     const char *proc_root,
                                     const char *os_release_path)
{
    long clock_ticks;
    long page_size;

    if (data == NULL || proc_root == NULL || os_release_path == NULL) {
        errno = EINVAL;
        return -1;
    }
    memset(data, 0, sizeof(*data));
    if (copy_path(data->proc_root, sizeof(data->proc_root), proc_root) < 0 ||
        copy_path(data->os_release_path, sizeof(data->os_release_path),
                  os_release_path) < 0)
        return -1;
    clock_ticks = sysconf(_SC_CLK_TCK);
    page_size = sysconf(_SC_PAGESIZE);
    data->clock_ticks_per_second = clock_ticks > 0L ? clock_ticks : 100L;
    data->page_size = page_size > 0L ? page_size : 4096L;
    data->user_id = getuid();
    data->process_limit = TASK_MANAGER_PROCESS_LIMIT;
    data->initialized = true;
    return 0;
}

int win31x_task_manager_data_init(Win31xTaskManagerData *data)
{
    return win31x_task_manager_data_init_at(data, "/proc", "/etc/os-release");
}

int win31x_task_manager_data_refresh(Win31xTaskManagerData *data)
{
    Win31xTaskManagerSystem system;
    ProcessCandidate *candidates = NULL;
    Win31xTaskManagerProcess *processes = NULL;
    ProcessSample *samples = NULL;
    size_t process_count = 0U;
    bool process_list_truncated = false;
    size_t index;
    uint64_t total;
    uint64_t idle;

    if (data == NULL || !data->initialized) {
        errno = EINVAL;
        return -1;
    }
    memset(&system, 0, sizeof(system));
    if (read_cpu_totals(data, &total, &idle) < 0 ||
        read_memory(data, &system.memory_total_bytes,
                    &system.memory_available_bytes) < 0 ||
        collect_processes(data, &candidates, &process_count,
                          &process_list_truncated) < 0)
        goto fail;
    read_uptime(data, &system);
    read_load_average(data, &system);
    read_system_identity(data, &system);
    calculate_percentages(data, candidates, process_count, total, idle,
                          &system);

    if (process_count > 0U) {
        if (process_count > SIZE_MAX / sizeof(*processes) ||
            process_count > SIZE_MAX / sizeof(*samples)) {
            errno = ENOMEM;
            goto fail;
        }
        processes = malloc(process_count * sizeof(*processes));
        samples = malloc(process_count * sizeof(*samples));
        if (processes == NULL || samples == NULL)
            goto fail;
        for (index = 0U; index < process_count; ++index) {
            processes[index] = candidates[index].process;
            samples[index] = candidates[index].sample;
        }
    }
    free(candidates);

    free(data->snapshot.processes);
    free(data->previous_process_samples);
    data->snapshot.processes = processes;
    data->snapshot.process_count = process_count;
    data->snapshot.process_list_truncated = process_list_truncated;
    data->snapshot.system = system;
    data->previous_process_samples = samples;
    data->previous_process_sample_count = process_count;
    data->previous_total_ticks = total;
    data->previous_idle_ticks = idle;
    data->has_previous_cpu_sample = true;
    return 0;

fail:
    free(candidates);
    free(processes);
    free(samples);
    return -1;
}

const Win31xTaskManagerSnapshot *
win31x_task_manager_data_snapshot(const Win31xTaskManagerData *data)
{
    if (data == NULL || !data->initialized)
        return NULL;
    return &data->snapshot;
}

void win31x_task_manager_data_destroy(Win31xTaskManagerData *data)
{
    if (data == NULL)
        return;
    free(data->snapshot.processes);
    free(data->previous_process_samples);
    memset(data, 0, sizeof(*data));
}

static int signal_selected_process(const Win31xTaskManagerData *data,
                                   pid_t pid,
                                   uint64_t expected_start_time_ticks,
                                   int signal_number, char *error_text,
                                   size_t error_capacity)
{
    char path[WIN31X_TASK_MANAGER_PATH_CAPACITY];
    char *contents = NULL;
    size_t length;
    uid_t owner;
    ProcessCandidate current;
    int process_handle;

    if (error_text != NULL && error_capacity > 0U)
        error_text[0] = '\0';
    if (data == NULL || !data->initialized) {
        errno = EINVAL;
        set_error(error_text, error_capacity,
                  "Task Manager is not initialized.");
        return -1;
    }
    if (pid <= 1) {
        errno = EPERM;
        set_error(error_text, error_capacity,
                  "System process %ld cannot be ended.", (long)pid);
        return -1;
    }
    if (pid == getpid()) {
        errno = EPERM;
        set_error(error_text, error_capacity,
                  "Task Manager cannot end the window manager itself.");
        return -1;
    }
    if (strcmp(data->proc_root, "/proc") != 0) {
        errno = ENOTSUP;
        set_error(error_text, error_capacity,
                  "Process control is available only with the system /proc "
                  "sampler.");
        return -1;
    }
    /* A Linux pidfd pins the selected kernel process across all validation. */
    process_handle = open_process_handle(pid);
    if (process_handle < 0) {
        int saved_errno = errno;

        if (saved_errno == ESRCH) {
            set_error(error_text, error_capacity,
                      "Process %ld is no longer running.", (long)pid);
        } else if (saved_errno == ENOTSUP) {
            set_error(error_text, error_capacity,
                      "Ending processes requires Linux 5.3 or newer with "
                      "pidfd support.");
        } else {
            set_error(error_text, error_capacity,
                      "Process %ld could not be securely opened: %s.",
                      (long)pid, strerror(saved_errno));
        }
        errno = saved_errno;
        return -1;
    }
    if (make_process_path(path, sizeof(path), data->proc_root, pid, "status") < 0 ||
        read_limited_file(path, TASK_MANAGER_STATUS_FILE_LIMIT, &contents,
                          &length) < 0) {
        int saved_errno = errno;

        set_error(error_text, error_capacity,
                  saved_errno == ENOENT
                      ? "Process %ld is no longer running."
                      : "Process %ld could not be inspected: %s.",
                  (long)pid, strerror(saved_errno));
        if (process_handle >= 0)
            (void)close(process_handle);
        errno = saved_errno;
        return -1;
    }
    (void)length;
    if (!find_status_uid(contents, &owner)) {
        free(contents);
        set_error(error_text, error_capacity,
                  "Process %ld has invalid ownership information.",
                  (long)pid);
        if (process_handle >= 0)
            (void)close(process_handle);
        errno = EPROTO;
        return -1;
    }
    free(contents);
    if (owner != data->user_id) {
        set_error(error_text, error_capacity,
                  "Process %ld belongs to another user and cannot be ended.",
                  (long)pid);
        if (process_handle >= 0)
            (void)close(process_handle);
        errno = EPERM;
        return -1;
    }
    if (make_process_path(path, sizeof(path), data->proc_root, pid, "stat") < 0 ||
        read_limited_file(path, TASK_MANAGER_SMALL_FILE_LIMIT, &contents,
                          &length) < 0) {
        int saved_errno = errno;

        set_error(error_text, error_capacity,
                  "Process %ld is no longer the selected process.", (long)pid);
        if (process_handle >= 0)
            (void)close(process_handle);
        errno = saved_errno;
        return -1;
    }
    memset(&current, 0, sizeof(current));
    if (parse_process_stat(contents, pid, &current) < 0 ||
        current.sample.start_time != expected_start_time_ticks) {
        free(contents);
        set_error(error_text, error_capacity,
                  "Process %ld changed before it could be ended.", (long)pid);
        if (process_handle >= 0)
            (void)close(process_handle);
        errno = ESRCH;
        return -1;
    }
    free(contents);
    if (signal_process_handle(process_handle, signal_number) < 0) {
        int saved_errno = errno;

        if (process_handle >= 0)
            (void)close(process_handle);
        if (saved_errno == ESRCH) {
            set_error(error_text, error_capacity,
                      "Process %ld is no longer running.", (long)pid);
        } else if (saved_errno == ENOTSUP) {
            set_error(error_text, error_capacity,
                      "Ending processes requires Linux 5.3 or newer with "
                      "pidfd support.");
        } else {
            set_error(error_text, error_capacity,
                      "Process %ld could not be ended: %s.", (long)pid,
                      strerror(saved_errno));
        }
        errno = saved_errno;
        return -1;
    }
    if (process_handle >= 0)
        (void)close(process_handle);
    return 0;
}

int win31x_task_manager_data_terminate(const Win31xTaskManagerData *data,
                                       pid_t pid,
                                       uint64_t expected_start_time_ticks,
                                       char *error_text,
                                       size_t error_capacity)
{
    return signal_selected_process(data, pid, expected_start_time_ticks,
                                   SIGTERM, error_text, error_capacity);
}

int win31x_task_manager_data_force_terminate(
    const Win31xTaskManagerData *data, pid_t pid,
    uint64_t expected_start_time_ticks, char *error_text,
    size_t error_capacity)
{
    return signal_selected_process(data, pid, expected_start_time_ticks,
                                   SIGKILL, error_text, error_capacity);
}
