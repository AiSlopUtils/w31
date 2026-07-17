#define _DARWIN_C_SOURCE
#define _DEFAULT_SOURCE
#define _POSIX_C_SOURCE 200809L

#include "settings.h"

#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

#define SETTINGS_DIRECTORY "win31x"
#define SETTINGS_FILENAME "settings.conf"
#define SETTINGS_PATH_CAPACITY 4096U
#define SETTINGS_FILE_LIMIT 16384U
#define SETTINGS_TEMP_ATTEMPTS 128U

const Win31xColorScheme win31x_color_schemes[WIN31X_COLOR_SCHEME_COUNT] = {
    {"classic-teal", "Classic Teal", "#008080", "#000080"},
    {"ocean-blue", "Ocean Blue", "#1f4e79", "#003399"},
    {"forest", "Forest", "#3f6b4f", "#244a32"},
    {"plum", "Plum", "#6b4a6b", "#4b275f"},
    {"slate", "Slate", "#5b6573", "#36454f"},
};

static unsigned long settings_temp_sequence;

const Win31xColorScheme *win31x_color_scheme(size_t index)
{
    if (index >= WIN31X_COLOR_SCHEME_COUNT)
        return NULL;
    return &win31x_color_schemes[index];
}

void win31x_settings_defaults(Win31xSettings *settings)
{
    if (settings == NULL)
        return;
    settings->color_scheme = 0U;
    settings->auto_lock_enabled = false;
    settings->auto_lock_minutes = 10U;
}

static int format_path(char *destination, size_t capacity, const char *format,
                       const char *value)
{
    int written;

    written = snprintf(destination, capacity, format, value);
    if (written < 0 || (size_t)written >= capacity) {
        errno = ENAMETOOLONG;
        return -1;
    }
    return 0;
}

static int configuration_root(char *path, size_t capacity)
{
    const char *xdg = getenv("XDG_CONFIG_HOME");
    const char *home;

    if (xdg != NULL && xdg[0] == '/')
        return format_path(path, capacity, "%s", xdg);

    home = getenv("HOME");
    if (home == NULL || home[0] != '/') {
        errno = ENOENT;
        return -1;
    }
    return format_path(path, capacity, "%s/.config", home);
}

static int open_settings_directory(bool create)
{
    char root_path[SETTINGS_PATH_CAPACITY];
    int root_fd;
    int directory_fd;
    int saved_errno;

    if (configuration_root(root_path, sizeof(root_path)) < 0)
        return -1;
    if (create && mkdir(root_path, (mode_t)0700) < 0 && errno != EEXIST)
        return -1;

    root_fd = open(root_path, O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    if (root_fd < 0)
        return -1;
    if (create &&
        mkdirat(root_fd, SETTINGS_DIRECTORY, (mode_t)0700) < 0 &&
        errno != EEXIST) {
        saved_errno = errno;
        close(root_fd);
        errno = saved_errno;
        return -1;
    }

    directory_fd = openat(root_fd, SETTINGS_DIRECTORY,
                          O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW);
    saved_errno = errno;
    close(root_fd);
    if (directory_fd < 0) {
        errno = saved_errno;
        return -1;
    }
    if (create && fchmod(directory_fd, (mode_t)0700) < 0) {
        saved_errno = errno;
        close(directory_fd);
        errno = saved_errno;
        return -1;
    }
    return directory_fd;
}

static char *trim_space(char *text)
{
    char *end;

    while (*text != '\0' && isspace((unsigned char)*text) != 0)
        ++text;
    end = text + strlen(text);
    while (end > text && isspace((unsigned char)end[-1]) != 0)
        --end;
    *end = '\0';
    return text;
}

static bool parse_decimal(const char *text, uintmax_t *value)
{
    uintmax_t result = 0U;
    bool overflow = false;
    const unsigned char *cursor = (const unsigned char *)text;

    if (*cursor == '\0')
        return false;
    while (*cursor != '\0') {
        unsigned int digit;

        if (*cursor < (unsigned char)'0' || *cursor > (unsigned char)'9')
            return false;
        digit = (unsigned int)(*cursor - (unsigned char)'0');
        if (result > (UINTMAX_MAX - (uintmax_t)digit) / UINTMAX_C(10)) {
            overflow = true;
        } else if (!overflow) {
            result = result * UINTMAX_C(10) + (uintmax_t)digit;
        }
        ++cursor;
    }
    *value = overflow ? UINTMAX_MAX : result;
    return true;
}

static bool parse_boolean(const char *value, bool *result)
{
    if (strcmp(value, "true") == 0 || strcmp(value, "yes") == 0 ||
        strcmp(value, "on") == 0 || strcmp(value, "1") == 0) {
        *result = true;
        return true;
    }
    if (strcmp(value, "false") == 0 || strcmp(value, "no") == 0 ||
        strcmp(value, "off") == 0 || strcmp(value, "0") == 0) {
        *result = false;
        return true;
    }
    return false;
}

static bool parse_color_scheme(const char *value, size_t *index)
{
    uintmax_t numeric;
    size_t candidate;

    for (candidate = 0U; candidate < WIN31X_COLOR_SCHEME_COUNT; ++candidate) {
        if (strcmp(value, win31x_color_schemes[candidate].id) == 0 ||
            strcmp(value, win31x_color_schemes[candidate].name) == 0) {
            *index = candidate;
            return true;
        }
    }
    if (parse_decimal(value, &numeric) &&
        numeric < (uintmax_t)WIN31X_COLOR_SCHEME_COUNT) {
        *index = (size_t)numeric;
        return true;
    }
    return false;
}

static unsigned int clamp_lock_minutes(uintmax_t minutes)
{
    if (minutes < (uintmax_t)WIN31X_AUTO_LOCK_MINUTES_MIN)
        return WIN31X_AUTO_LOCK_MINUTES_MIN;
    if (minutes > (uintmax_t)WIN31X_AUTO_LOCK_MINUTES_MAX)
        return WIN31X_AUTO_LOCK_MINUTES_MAX;
    return (unsigned int)minutes;
}

static void parse_setting_line(Win31xSettings *settings, char *line)
{
    char *key = trim_space(line);
    char *separator;
    char *value;

    if (key[0] == '\0' || key[0] == '#')
        return;
    separator = strchr(key, '=');
    if (separator == NULL)
        return;
    *separator = '\0';
    value = trim_space(separator + 1);
    key = trim_space(key);

    if (strcmp(key, "color_scheme") == 0) {
        size_t scheme;

        if (parse_color_scheme(value, &scheme))
            settings->color_scheme = scheme;
    } else if (strcmp(key, "auto_lock_enabled") == 0) {
        bool enabled;

        if (parse_boolean(value, &enabled))
            settings->auto_lock_enabled = enabled;
    } else if (strcmp(key, "auto_lock_minutes") == 0) {
        uintmax_t minutes;

        if (parse_decimal(value, &minutes))
            settings->auto_lock_minutes = clamp_lock_minutes(minutes);
    }
}

static void parse_settings(Win31xSettings *settings, char *contents)
{
    char *line = contents;

    while (line != NULL) {
        char *next = strchr(line, '\n');

        if (next != NULL) {
            *next = '\0';
            ++next;
        }
        parse_setting_line(settings, line);
        line = next;
    }
}

static int read_settings_file(int directory_fd, char *contents, size_t capacity)
{
    struct stat status;
    int file_fd;
    size_t used = 0U;
    int saved_errno;

    file_fd = openat(directory_fd, SETTINGS_FILENAME,
                     O_RDONLY | O_CLOEXEC | O_NOFOLLOW);
    if (file_fd < 0)
        return -1;
    if (fstat(file_fd, &status) < 0) {
        saved_errno = errno;
        close(file_fd);
        errno = saved_errno;
        return -1;
    }
    if (!S_ISREG(status.st_mode)) {
        close(file_fd);
        errno = EINVAL;
        return -1;
    }
    if (status.st_size < 0 || (uintmax_t)status.st_size >= (uintmax_t)capacity) {
        close(file_fd);
        errno = EFBIG;
        return -1;
    }

    while (used + 1U < capacity) {
        ssize_t amount = read(file_fd, contents + used, capacity - used - 1U);

        if (amount > 0) {
            used += (size_t)amount;
            continue;
        }
        if (amount == 0)
            break;
        if (errno == EINTR)
            continue;
        saved_errno = errno;
        close(file_fd);
        errno = saved_errno;
        return -1;
    }
    if (used + 1U == capacity) {
        char extra;
        ssize_t amount;

        do {
            amount = read(file_fd, &extra, 1U);
        } while (amount < 0 && errno == EINTR);
        if (amount != 0) {
            saved_errno = amount < 0 ? errno : EFBIG;
            close(file_fd);
            errno = saved_errno;
            return -1;
        }
    }
    contents[used] = '\0';
    if (close(file_fd) < 0)
        return -1;
    return 0;
}

int win31x_settings_load(Win31xSettings *settings)
{
    char contents[SETTINGS_FILE_LIMIT + 1U];
    int directory_fd;
    int result;
    int saved_errno;

    if (settings == NULL) {
        errno = EINVAL;
        return -1;
    }
    win31x_settings_defaults(settings);
    directory_fd = open_settings_directory(false);
    if (directory_fd < 0) {
        if (errno == ENOENT)
            return 0;
        return -1;
    }
    result = read_settings_file(directory_fd, contents, sizeof(contents));
    saved_errno = errno;
    close(directory_fd);
    if (result < 0) {
        if (saved_errno == ENOENT)
            return 0;
        errno = saved_errno;
        return -1;
    }
    parse_settings(settings, contents);
    return 0;
}

static int write_all(int file_fd, const char *contents, size_t length)
{
    size_t written = 0U;

    while (written < length) {
        ssize_t amount = write(file_fd, contents + written, length - written);

        if (amount > 0) {
            written += (size_t)amount;
            continue;
        }
        if (amount < 0 && errno == EINTR)
            continue;
        if (amount == 0)
            errno = EIO;
        return -1;
    }
    return 0;
}

static int ensure_safe_final_target(int directory_fd)
{
    struct stat status;

    if (fstatat(directory_fd, SETTINGS_FILENAME, &status,
                AT_SYMLINK_NOFOLLOW) == 0) {
        if (S_ISLNK(status.st_mode)) {
            errno = ELOOP;
            return -1;
        }
        if (!S_ISREG(status.st_mode)) {
            errno = EINVAL;
            return -1;
        }
        return 0;
    }
    return errno == ENOENT ? 0 : -1;
}

static int create_temporary_file(int directory_fd, char *name,
                                 size_t name_capacity)
{
    unsigned int attempt;

    for (attempt = 0U; attempt < SETTINGS_TEMP_ATTEMPTS; ++attempt) {
        int written;
        int file_fd;

        ++settings_temp_sequence;
        written = snprintf(name, name_capacity, ".settings.conf.tmp.%ld.%lu",
                           (long)getpid(), settings_temp_sequence);
        if (written < 0 || (size_t)written >= name_capacity) {
            errno = ENAMETOOLONG;
            return -1;
        }
        file_fd = openat(directory_fd, name,
                         O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC | O_NOFOLLOW,
                         (mode_t)0600);
        if (file_fd >= 0)
            return file_fd;
        if (errno != EEXIST)
            return -1;
    }
    errno = EEXIST;
    return -1;
}

static Win31xSettings normalized_settings(const Win31xSettings *settings)
{
    Win31xSettings normalized = *settings;

    if (normalized.color_scheme >= WIN31X_COLOR_SCHEME_COUNT)
        normalized.color_scheme = 0U;
    normalized.auto_lock_minutes = clamp_lock_minutes(
        (uintmax_t)normalized.auto_lock_minutes);
    return normalized;
}

int win31x_settings_save(const Win31xSettings *settings)
{
    Win31xSettings normalized;
    const Win31xColorScheme *scheme;
    char contents[256];
    char temporary_name[96] = "";
    int content_length;
    int directory_fd = -1;
    int file_fd = -1;
    int saved_errno;
    bool renamed = false;

    if (settings == NULL) {
        errno = EINVAL;
        return -1;
    }
    normalized = normalized_settings(settings);
    scheme = win31x_color_scheme(normalized.color_scheme);
    if (scheme == NULL) {
        errno = EINVAL;
        return -1;
    }
    content_length = snprintf(
        contents, sizeof(contents),
        "# Win31 X settings\n"
        "color_scheme=%s\n"
        "auto_lock_enabled=%s\n"
        "auto_lock_minutes=%u\n",
        scheme->id, normalized.auto_lock_enabled ? "true" : "false",
        normalized.auto_lock_minutes);
    if (content_length < 0 || (size_t)content_length >= sizeof(contents)) {
        errno = EOVERFLOW;
        return -1;
    }

    directory_fd = open_settings_directory(true);
    if (directory_fd < 0)
        return -1;
    if (ensure_safe_final_target(directory_fd) < 0)
        goto fail;
    file_fd = create_temporary_file(directory_fd, temporary_name,
                                    sizeof(temporary_name));
    if (file_fd < 0)
        goto fail;
    if (fchmod(file_fd, (mode_t)0600) < 0 ||
        write_all(file_fd, contents, (size_t)content_length) < 0 ||
        fsync(file_fd) < 0)
        goto fail;
    if (close(file_fd) < 0) {
        file_fd = -1;
        goto fail;
    }
    file_fd = -1;
    if (renameat(directory_fd, temporary_name, directory_fd,
                 SETTINGS_FILENAME) < 0)
        goto fail;
    renamed = true;
    if (fsync(directory_fd) < 0 && errno != EINVAL && errno != EROFS)
        goto fail;
    if (close(directory_fd) < 0)
        return -1;
    return 0;

fail:
    saved_errno = errno;
    if (file_fd >= 0)
        close(file_fd);
    if (!renamed && temporary_name[0] != '\0')
        unlinkat(directory_fd, temporary_name, 0);
    if (directory_fd >= 0)
        close(directory_fd);
    errno = saved_errno;
    return -1;
}
