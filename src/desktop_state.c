#define _DARWIN_C_SOURCE
#define _DEFAULT_SOURCE
#define _POSIX_C_SOURCE 200809L

#include "desktop_state.h"

#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

#define DESKTOP_STATE_DIRECTORY "win31x"
#define DESKTOP_STATE_FILENAME "layout.conf"
#define DESKTOP_STATE_PATH_CAPACITY 4096U
#define DESKTOP_STATE_FILE_LIMIT (256U * 1024U)
#define DESKTOP_STATE_TEMP_ATTEMPTS 128U
#define DESKTOP_STATE_TOKEN_MAX 12U
#define DESKTOP_STATE_LEGACY_VERSION 1U

static unsigned long desktop_state_temp_sequence;

static size_t bounded_length(const char *text, size_t maximum)
{
    size_t length;

    if (text == NULL)
        return maximum + 1U;
    for (length = 0U; length <= maximum; ++length) {
        if (text[length] == '\0')
            return length;
    }
    return maximum + 1U;
}

void win31x_desktop_placement_defaults(Win31xDesktopPlacement *placement)
{
    if (placement == NULL)
        return;
    memset(placement, 0, sizeof(*placement));
    placement->layout = WIN31X_DESKTOP_LAYOUT_NORMAL;
    placement->layout_before_maximize = WIN31X_DESKTOP_LAYOUT_NORMAL;
}

void win31x_desktop_state_defaults(Win31xDesktopState *state)
{
    if (state == NULL)
        return;
    memset(state, 0, sizeof(*state));
    state->applications_icon.layout = WIN31X_DESKTOP_LAYOUT_NORMAL;
    state->control_panel_icon.layout = WIN31X_DESKTOP_LAYOUT_NORMAL;
    state->launcher.layout = WIN31X_DESKTOP_LAYOUT_NORMAL;
    state->control_panel.layout = WIN31X_DESKTOP_LAYOUT_NORMAL;
    state->run_dialog.layout = WIN31X_DESKTOP_LAYOUT_NORMAL;
    state->write_enabled = true;
}

static Win31xDesktopLayout normalized_layout(Win31xDesktopLayout layout)
{
    switch (layout) {
    case WIN31X_DESKTOP_LAYOUT_NORMAL:
    case WIN31X_DESKTOP_LAYOUT_MAXIMIZED:
    case WIN31X_DESKTOP_LAYOUT_SNAPPED_LEFT:
    case WIN31X_DESKTOP_LAYOUT_SNAPPED_RIGHT:
        return layout;
    default:
        return WIN31X_DESKTOP_LAYOUT_NORMAL;
    }
}

static Win31xDesktopLayout normalized_layout_before_maximize(
    Win31xDesktopLayout layout)
{
    switch (layout) {
    case WIN31X_DESKTOP_LAYOUT_NORMAL:
    case WIN31X_DESKTOP_LAYOUT_SNAPPED_LEFT:
    case WIN31X_DESKTOP_LAYOUT_SNAPPED_RIGHT:
        return layout;
    case WIN31X_DESKTOP_LAYOUT_MAXIMIZED:
    default:
        return WIN31X_DESKTOP_LAYOUT_NORMAL;
    }
}

bool win31x_desktop_placement_is_valid(
    const Win31xDesktopPlacement *placement)
{
    size_t monitor_length;

    if (placement == NULL || !placement->valid)
        return false;
    monitor_length = bounded_length(placement->monitor_name,
                                    WIN31X_DESKTOP_MONITOR_NAME_MAX);
    return monitor_length <= WIN31X_DESKTOP_MONITOR_NAME_MAX &&
           placement->width > 0 &&
           placement->width <= WIN31X_DESKTOP_DIMENSION_MAX &&
           placement->height > 0 &&
           placement->height <= WIN31X_DESKTOP_DIMENSION_MAX;
}

static bool valid_identity(const char *identity)
{
    size_t length = bounded_length(identity, WIN31X_DESKTOP_IDENTITY_MAX);

    return length > 0U && length <= WIN31X_DESKTOP_IDENTITY_MAX;
}

Win31xDesktopClientRecord *win31x_desktop_state_find_client_mutable(
    Win31xDesktopState *state, const char *identity)
{
    size_t client_index;
    size_t count;

    if (state == NULL || !valid_identity(identity))
        return NULL;
    count = state->client_count;
    if (count > WIN31X_DESKTOP_CLIENT_MAX)
        count = WIN31X_DESKTOP_CLIENT_MAX;
    for (client_index = 0U; client_index < count; ++client_index) {
        if (strcmp(state->clients[client_index].identity, identity) == 0)
            return &state->clients[client_index];
    }
    return NULL;
}

const Win31xDesktopClientRecord *win31x_desktop_state_find_client(
    const Win31xDesktopState *state, const char *identity)
{
    size_t client_index;
    size_t count;

    if (state == NULL || !valid_identity(identity))
        return NULL;
    count = state->client_count;
    if (count > WIN31X_DESKTOP_CLIENT_MAX)
        count = WIN31X_DESKTOP_CLIENT_MAX;
    for (client_index = 0U; client_index < count; ++client_index) {
        if (strcmp(state->clients[client_index].identity, identity) == 0)
            return &state->clients[client_index];
    }
    return NULL;
}

int win31x_desktop_state_upsert_client(
    Win31xDesktopState *state, const char *identity,
    const Win31xDesktopPlacement *placement)
{
    Win31xDesktopClientRecord refreshed_record;
    size_t client_index;
    size_t client_count;
    size_t identity_length;

    if (state == NULL || !valid_identity(identity) ||
        !win31x_desktop_placement_is_valid(placement)) {
        errno = EINVAL;
        return -1;
    }
    identity_length = strlen(identity);
    memcpy(refreshed_record.identity, identity, identity_length + 1U);
    refreshed_record.placement = *placement;
    refreshed_record.placement.layout = normalized_layout(placement->layout);
    refreshed_record.placement.layout_before_maximize =
        normalized_layout_before_maximize(
            placement->layout_before_maximize);

    client_count = state->client_count;
    if (client_count > WIN31X_DESKTOP_CLIENT_MAX)
        client_count = WIN31X_DESKTOP_CLIENT_MAX;
    for (client_index = 0U; client_index < client_count; ++client_index) {
        if (strcmp(state->clients[client_index].identity, identity) == 0)
            break;
    }
    if (client_index < client_count) {
        if (client_index + 1U < client_count) {
            memmove(&state->clients[client_index],
                    &state->clients[client_index + 1U],
                    (client_count - client_index - 1U) *
                        sizeof(state->clients[0]));
        }
    } else if (client_count == WIN31X_DESKTOP_CLIENT_MAX) {
        memmove(&state->clients[0], &state->clients[1],
                (WIN31X_DESKTOP_CLIENT_MAX - 1U) *
                    sizeof(state->clients[0]));
    } else {
        ++client_count;
    }
    state->clients[client_count - 1U] = refreshed_record;
    state->client_count = client_count;
    return 0;
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
    const char *xdg_config = getenv("XDG_CONFIG_HOME");
    const char *home_directory;

    if (xdg_config != NULL && xdg_config[0] == '/')
        return format_path(path, capacity, "%s", xdg_config);

    home_directory = getenv("HOME");
    if (home_directory == NULL || home_directory[0] != '/') {
        errno = ENOENT;
        return -1;
    }
    return format_path(path, capacity, "%s/.config", home_directory);
}

static int open_desktop_state_directory(bool create)
{
    char root_path[DESKTOP_STATE_PATH_CAPACITY];
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
        mkdirat(root_fd, DESKTOP_STATE_DIRECTORY, (mode_t)0700) < 0 &&
        errno != EEXIST) {
        saved_errno = errno;
        close(root_fd);
        errno = saved_errno;
        return -1;
    }

    directory_fd = openat(root_fd, DESKTOP_STATE_DIRECTORY,
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

static int read_desktop_state_file(int directory_fd, char *contents,
                                   size_t capacity, size_t *length_out)
{
    struct stat file_status;
    int file_fd;
    size_t used = 0U;
    int saved_errno;

    file_fd = openat(directory_fd, DESKTOP_STATE_FILENAME,
                     O_RDONLY | O_CLOEXEC | O_NOFOLLOW);
    if (file_fd < 0)
        return -1;
    if (fstat(file_fd, &file_status) < 0) {
        saved_errno = errno;
        close(file_fd);
        errno = saved_errno;
        return -1;
    }
    if (!S_ISREG(file_status.st_mode)) {
        close(file_fd);
        errno = EINVAL;
        return -1;
    }
    if (file_status.st_size < 0 ||
        (uintmax_t)file_status.st_size > (uintmax_t)(capacity - 1U)) {
        close(file_fd);
        errno = EFBIG;
        return -1;
    }

    while (used + 1U < capacity) {
        ssize_t amount = read(file_fd, contents + used,
                              capacity - used - 1U);

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
        char extra_byte;
        ssize_t amount;

        do {
            amount = read(file_fd, &extra_byte, 1U);
        } while (amount < 0 && errno == EINTR);
        if (amount != 0) {
            saved_errno = amount < 0 ? errno : EFBIG;
            close(file_fd);
            errno = saved_errno;
            return -1;
        }
    }
    if (close(file_fd) < 0)
        return -1;
    contents[used] = '\0';
    *length_out = used;
    return 0;
}

static bool parse_unsigned_decimal(const char *text, uintmax_t *result_out)
{
    const unsigned char *cursor = (const unsigned char *)text;
    uintmax_t result = 0U;

    if (cursor == NULL || *cursor == '\0')
        return false;
    while (*cursor != '\0') {
        unsigned int digit;

        if (*cursor < (unsigned char)'0' || *cursor > (unsigned char)'9')
            return false;
        digit = (unsigned int)(*cursor - (unsigned char)'0');
        if (result > (UINTMAX_MAX - (uintmax_t)digit) / UINTMAX_C(10))
            return false;
        result = result * UINTMAX_C(10) + (uintmax_t)digit;
        ++cursor;
    }
    *result_out = result;
    return true;
}

static bool unsigned_decimal_syntax(const char *text)
{
    const unsigned char *cursor = (const unsigned char *)text;

    if (cursor == NULL || *cursor == '\0')
        return false;
    while (*cursor != '\0') {
        if (*cursor < (unsigned char)'0' ||
            *cursor > (unsigned char)'9')
            return false;
        ++cursor;
    }
    return true;
}

static bool desktop_state_version_is_supported(uintmax_t version)
{
    return version == (uintmax_t)DESKTOP_STATE_LEGACY_VERSION ||
           version == (uintmax_t)WIN31X_DESKTOP_STATE_VERSION;
}

static bool parse_signed_int(const char *text, int *result_out)
{
    const unsigned char *cursor = (const unsigned char *)text;
    uintmax_t magnitude = 0U;
    uintmax_t limit;
    bool negative = false;

    if (cursor == NULL || *cursor == '\0')
        return false;
    if (*cursor == (unsigned char)'-') {
        negative = true;
        ++cursor;
    }
    if (*cursor == '\0')
        return false;
    limit = negative ? (uintmax_t)INT_MAX + UINTMAX_C(1) :
                       (uintmax_t)INT_MAX;
    while (*cursor != '\0') {
        unsigned int digit;

        if (*cursor < (unsigned char)'0' || *cursor > (unsigned char)'9')
            return false;
        digit = (unsigned int)(*cursor - (unsigned char)'0');
        if (magnitude > (limit - (uintmax_t)digit) / UINTMAX_C(10))
            return false;
        magnitude = magnitude * UINTMAX_C(10) + (uintmax_t)digit;
        ++cursor;
    }
    if (negative) {
        if (magnitude == (uintmax_t)INT_MAX + UINTMAX_C(1))
            *result_out = INT_MIN;
        else
            *result_out = -(int)magnitude;
    } else {
        *result_out = (int)magnitude;
    }
    return true;
}

static int hexadecimal_value(unsigned char character)
{
    if (character >= (unsigned char)'0' && character <= (unsigned char)'9')
        return (int)(character - (unsigned char)'0');
    if (character >= (unsigned char)'a' && character <= (unsigned char)'f')
        return 10 + (int)(character - (unsigned char)'a');
    if (character >= (unsigned char)'A' && character <= (unsigned char)'F')
        return 10 + (int)(character - (unsigned char)'A');
    return -1;
}

static bool decode_hexadecimal(const char *encoded, char *decoded,
                               size_t maximum_length, bool allow_empty)
{
    size_t encoded_length;
    size_t byte_index;

    if (encoded == NULL || decoded == NULL)
        return false;
    if (allow_empty && strcmp(encoded, "-") == 0) {
        decoded[0] = '\0';
        return true;
    }
    encoded_length = strlen(encoded);
    if (encoded_length == 0U || (encoded_length & 1U) != 0U ||
        encoded_length / 2U > maximum_length)
        return false;
    for (byte_index = 0U; byte_index < encoded_length / 2U; ++byte_index) {
        int high = hexadecimal_value((unsigned char)encoded[byte_index * 2U]);
        int low = hexadecimal_value(
            (unsigned char)encoded[byte_index * 2U + 1U]);
        unsigned char decoded_byte;

        if (high < 0 || low < 0)
            return false;
        decoded_byte = (unsigned char)((unsigned int)high * 16U +
                                       (unsigned int)low);
        if (decoded_byte == '\0')
            return false;
        decoded[byte_index] = (char)decoded_byte;
    }
    decoded[encoded_length / 2U] = '\0';
    return true;
}

static bool parse_placement_tokens(char *const *tokens, size_t token_count,
                                   Win31xDesktopPlacement *placement)
{
    Win31xDesktopPlacement parsed;
    uintmax_t valid_number;
    uintmax_t width_number;
    uintmax_t height_number;
    uintmax_t layout_number;
    uintmax_t layout_before_maximize_number =
        (uintmax_t)WIN31X_DESKTOP_LAYOUT_NORMAL;

    win31x_desktop_placement_defaults(&parsed);
    if ((token_count != 9U && token_count != 10U) ||
        !parse_unsigned_decimal(tokens[0], &valid_number) ||
        valid_number > UINTMAX_C(1) ||
        !decode_hexadecimal(tokens[1], parsed.monitor_name,
                            WIN31X_DESKTOP_MONITOR_NAME_MAX, true) ||
        !parse_signed_int(tokens[2], &parsed.monitor_center_x) ||
        !parse_signed_int(tokens[3], &parsed.monitor_center_y) ||
        !parse_signed_int(tokens[4], &parsed.relative_x) ||
        !parse_signed_int(tokens[5], &parsed.relative_y) ||
        !parse_unsigned_decimal(tokens[6], &width_number) ||
        !parse_unsigned_decimal(tokens[7], &height_number) ||
        !parse_unsigned_decimal(tokens[8], &layout_number) ||
        (token_count == 10U &&
         !parse_unsigned_decimal(tokens[9],
                                 &layout_before_maximize_number)) ||
        width_number > (uintmax_t)WIN31X_DESKTOP_DIMENSION_MAX ||
        height_number > (uintmax_t)WIN31X_DESKTOP_DIMENSION_MAX)
        return false;

    if (valid_number == UINTMAX_C(0)) {
        if (width_number != UINTMAX_C(0) || height_number != UINTMAX_C(0))
            return false;
        *placement = parsed;
        return true;
    }
    if (width_number == UINTMAX_C(0) || height_number == UINTMAX_C(0))
        return false;
    parsed.valid = true;
    parsed.width = (int)width_number;
    parsed.height = (int)height_number;
    if (layout_number <=
        (uintmax_t)WIN31X_DESKTOP_LAYOUT_SNAPPED_RIGHT) {
        parsed.layout = (Win31xDesktopLayout)layout_number;
    } else {
        parsed.layout = WIN31X_DESKTOP_LAYOUT_NORMAL;
    }
    if (layout_before_maximize_number ==
        (uintmax_t)WIN31X_DESKTOP_LAYOUT_SNAPPED_LEFT) {
        parsed.layout_before_maximize =
            WIN31X_DESKTOP_LAYOUT_SNAPPED_LEFT;
    } else if (layout_before_maximize_number ==
               (uintmax_t)WIN31X_DESKTOP_LAYOUT_SNAPPED_RIGHT) {
        parsed.layout_before_maximize =
            WIN31X_DESKTOP_LAYOUT_SNAPPED_RIGHT;
    } else {
        parsed.layout_before_maximize = WIN31X_DESKTOP_LAYOUT_NORMAL;
    }
    *placement = parsed;
    return true;
}

static size_t tokenize_record(char *line, char **tokens, size_t capacity,
                              bool *too_many)
{
    char *cursor = line;
    size_t token_count = 0U;

    *too_many = false;
    while (*cursor != '\0') {
        while (*cursor != '\0' &&
               isspace((unsigned char)*cursor) != 0)
            ++cursor;
        if (*cursor == '\0')
            break;
        if (token_count == capacity) {
            *too_many = true;
            return token_count;
        }
        tokens[token_count] = cursor;
        ++token_count;
        while (*cursor != '\0' &&
               isspace((unsigned char)*cursor) == 0)
            ++cursor;
        if (*cursor != '\0') {
            *cursor = '\0';
            ++cursor;
        }
    }
    return token_count;
}

static void parse_fixed_record(Win31xDesktopPlacement *destination,
                               char **tokens, size_t token_count)
{
    Win31xDesktopPlacement parsed;

    if ((token_count == 10U || token_count == 11U) &&
        parse_placement_tokens(&tokens[1], token_count - 1U, &parsed))
        *destination = parsed;
}

static void parse_client_record(Win31xDesktopState *state, char **tokens,
                                size_t token_count)
{
    char identity[WIN31X_DESKTOP_IDENTITY_MAX + 1U];
    Win31xDesktopPlacement placement;

    if ((token_count != 11U && token_count != 12U) ||
        !decode_hexadecimal(tokens[1], identity,
                            WIN31X_DESKTOP_IDENTITY_MAX, false) ||
        !parse_placement_tokens(&tokens[2], token_count - 2U, &placement) ||
        !placement.valid)
        return;
    (void)win31x_desktop_state_upsert_client(state, identity, &placement);
}

static void parse_desktop_state(Win31xDesktopState *state, char *contents)
{
    Win31xDesktopState candidate;
    bool version_seen = false;
    bool version_supported = true;
    bool unsupported_version_seen = false;
    char *line = contents;

    win31x_desktop_state_defaults(&candidate);
    while (line != NULL) {
        char *next_line = strchr(line, '\n');
        char *tokens[DESKTOP_STATE_TOKEN_MAX];
        size_t token_count;
        bool too_many;

        if (next_line != NULL) {
            *next_line = '\0';
            ++next_line;
        }
        token_count = tokenize_record(line, tokens,
                                      DESKTOP_STATE_TOKEN_MAX, &too_many);
        if (!too_many && token_count > 0U && tokens[0][0] != '#') {
            if (strcmp(tokens[0], "version") == 0) {
                uintmax_t version_number;
                bool version_parsed =
                    token_count == 2U &&
                    parse_unsigned_decimal(tokens[1], &version_number);
                bool record_version_supported =
                    version_parsed &&
                    desktop_state_version_is_supported(version_number);

                if (token_count == 2U &&
                    unsigned_decimal_syntax(tokens[1]) &&
                    (!version_parsed || !record_version_supported)) {
                    unsupported_version_seen = true;
                }

                if (version_seen || !record_version_supported) {
                    version_supported = false;
                }
                version_seen = true;
            } else if (strcmp(tokens[0], "applications_icon") == 0) {
                parse_fixed_record(&candidate.applications_icon, tokens,
                                   token_count);
            } else if (strcmp(tokens[0], "control_panel_icon") == 0) {
                parse_fixed_record(&candidate.control_panel_icon, tokens,
                                   token_count);
            } else if (strcmp(tokens[0], "launcher") == 0) {
                parse_fixed_record(&candidate.launcher, tokens, token_count);
            } else if (strcmp(tokens[0], "control_panel") == 0) {
                parse_fixed_record(&candidate.control_panel, tokens,
                                   token_count);
            } else if (strcmp(tokens[0], "run_dialog") == 0) {
                parse_fixed_record(&candidate.run_dialog, tokens,
                                   token_count);
            } else if (strcmp(tokens[0], "client") == 0) {
                parse_client_record(&candidate, tokens, token_count);
            }
        }
        line = next_line;
    }
    if (version_seen && version_supported)
        *state = candidate;
    else if (unsupported_version_seen)
        state->write_enabled = false;
}

int win31x_desktop_state_load(Win31xDesktopState *state)
{
    char *contents;
    size_t content_length;
    size_t byte_index;
    int directory_fd;
    int result;
    int saved_errno;

    if (state == NULL) {
        errno = EINVAL;
        return -1;
    }
    win31x_desktop_state_defaults(state);
    directory_fd = open_desktop_state_directory(false);
    if (directory_fd < 0) {
        if (errno == ENOENT)
            return 0;
        return -1;
    }
    contents = malloc(DESKTOP_STATE_FILE_LIMIT + 1U);
    if (contents == NULL) {
        saved_errno = errno;
        close(directory_fd);
        errno = saved_errno;
        return -1;
    }
    result = read_desktop_state_file(directory_fd, contents,
                                     DESKTOP_STATE_FILE_LIMIT + 1U,
                                     &content_length);
    saved_errno = errno;
    close(directory_fd);
    if (result < 0) {
        free(contents);
        if (saved_errno == ENOENT)
            return 0;
        errno = saved_errno;
        return -1;
    }
    /* Embedded NULs invalidate only the record containing them. */
    for (byte_index = 0U; byte_index < content_length; ++byte_index) {
        if (contents[byte_index] == '\0')
            contents[byte_index] = '\1';
    }
    parse_desktop_state(state, contents);
    free(contents);
    return 0;
}

static bool encode_hexadecimal(const char *plain_text, size_t maximum_length,
                               char *encoded, size_t encoded_capacity,
                               bool empty_as_dash)
{
    static const char digits[] = "0123456789abcdef";
    size_t plain_length = bounded_length(plain_text, maximum_length);
    size_t byte_index;

    if (plain_length > maximum_length)
        return false;
    if (plain_length == 0U && empty_as_dash) {
        if (encoded_capacity < 2U)
            return false;
        encoded[0] = '-';
        encoded[1] = '\0';
        return true;
    }
    if (plain_length == 0U ||
        plain_length > (encoded_capacity - 1U) / 2U)
        return false;
    for (byte_index = 0U; byte_index < plain_length; ++byte_index) {
        unsigned char value = (unsigned char)plain_text[byte_index];

        if (value == '\0')
            return false;
        encoded[byte_index * 2U] = digits[value >> 4U];
        encoded[byte_index * 2U + 1U] = digits[value & 0x0fU];
    }
    encoded[plain_length * 2U] = '\0';
    return true;
}

static int append_formatted(char *contents, size_t capacity, size_t *used,
                            const char *format, ...)
{
    va_list arguments;
    int amount;

    if (*used >= capacity) {
        errno = EFBIG;
        return -1;
    }
    va_start(arguments, format);
    amount = vsnprintf(contents + *used, capacity - *used, format, arguments);
    va_end(arguments);
    if (amount < 0) {
        errno = EIO;
        return -1;
    }
    if ((size_t)amount >= capacity - *used) {
        errno = EFBIG;
        return -1;
    }
    *used += (size_t)amount;
    return 0;
}

static int append_placement(char *contents, size_t capacity, size_t *used,
                            const char *record_name,
                            const Win31xDesktopPlacement *placement)
{
    char monitor_hex[WIN31X_DESKTOP_MONITOR_NAME_MAX * 2U + 1U];
    Win31xDesktopLayout layout;
    Win31xDesktopLayout layout_before_maximize;

    if (!placement->valid) {
        return append_formatted(contents, capacity, used,
                                "%s 0 - 0 0 0 0 0 0 0 0\n",
                                record_name);
    }
    if (!win31x_desktop_placement_is_valid(placement) ||
        !encode_hexadecimal(placement->monitor_name,
                            WIN31X_DESKTOP_MONITOR_NAME_MAX, monitor_hex,
                            sizeof(monitor_hex), true)) {
        errno = EINVAL;
        return -1;
    }
    layout = normalized_layout(placement->layout);
    layout_before_maximize = normalized_layout_before_maximize(
        placement->layout_before_maximize);
    return append_formatted(
        contents, capacity, used,
        "%s 1 %s %d %d %d %d %d %d %d %d\n",
        record_name, monitor_hex, placement->monitor_center_x,
        placement->monitor_center_y, placement->relative_x,
        placement->relative_y, placement->width, placement->height,
        (int)layout, (int)layout_before_maximize);
}

static int append_client(char *contents, size_t capacity, size_t *used,
                         const Win31xDesktopClientRecord *client_record)
{
    char identity_hex[WIN31X_DESKTOP_IDENTITY_MAX * 2U + 1U];
    char monitor_hex[WIN31X_DESKTOP_MONITOR_NAME_MAX * 2U + 1U];
    Win31xDesktopLayout layout;
    Win31xDesktopLayout layout_before_maximize;

    if (!valid_identity(client_record->identity) ||
        !win31x_desktop_placement_is_valid(&client_record->placement) ||
        !encode_hexadecimal(client_record->identity,
                            WIN31X_DESKTOP_IDENTITY_MAX, identity_hex,
                            sizeof(identity_hex), false) ||
        !encode_hexadecimal(client_record->placement.monitor_name,
                            WIN31X_DESKTOP_MONITOR_NAME_MAX, monitor_hex,
                            sizeof(monitor_hex), true)) {
        errno = EINVAL;
        return -1;
    }
    layout = normalized_layout(client_record->placement.layout);
    layout_before_maximize = normalized_layout_before_maximize(
        client_record->placement.layout_before_maximize);
    return append_formatted(
        contents, capacity, used,
        "client %s 1 %s %d %d %d %d %d %d %d %d\n", identity_hex,
        monitor_hex, client_record->placement.monitor_center_x,
        client_record->placement.monitor_center_y,
        client_record->placement.relative_x,
        client_record->placement.relative_y,
        client_record->placement.width, client_record->placement.height,
        (int)layout, (int)layout_before_maximize);
}

static int serialize_desktop_state(const Win31xDesktopState *state,
                                   char *contents, size_t capacity,
                                   size_t *content_length)
{
    size_t used = 0U;
    size_t client_index;

    if (state->client_count > WIN31X_DESKTOP_CLIENT_MAX) {
        errno = EINVAL;
        return -1;
    }
    if (append_formatted(contents, capacity, &used,
                         "# Win31 X desktop layout\nversion %u\n",
                         WIN31X_DESKTOP_STATE_VERSION) < 0 ||
        append_placement(contents, capacity, &used, "applications_icon",
                         &state->applications_icon) < 0 ||
        append_placement(contents, capacity, &used, "control_panel_icon",
                         &state->control_panel_icon) < 0 ||
        append_placement(contents, capacity, &used, "launcher",
                         &state->launcher) < 0 ||
        append_placement(contents, capacity, &used, "control_panel",
                         &state->control_panel) < 0 ||
        append_placement(contents, capacity, &used, "run_dialog",
                         &state->run_dialog) < 0)
        return -1;
    for (client_index = 0U; client_index < state->client_count;
         ++client_index) {
        if (append_client(contents, capacity, &used,
                          &state->clients[client_index]) < 0)
            return -1;
    }
    *content_length = used;
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
    struct stat file_status;

    if (fstatat(directory_fd, DESKTOP_STATE_FILENAME, &file_status,
                AT_SYMLINK_NOFOLLOW) == 0) {
        if (S_ISLNK(file_status.st_mode)) {
            errno = ELOOP;
            return -1;
        }
        if (!S_ISREG(file_status.st_mode)) {
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

    for (attempt = 0U; attempt < DESKTOP_STATE_TEMP_ATTEMPTS; ++attempt) {
        int name_length;
        int file_fd;

        ++desktop_state_temp_sequence;
        name_length = snprintf(name, name_capacity,
                               ".layout.conf.tmp.%ld.%lu", (long)getpid(),
                               desktop_state_temp_sequence);
        if (name_length < 0 || (size_t)name_length >= name_capacity) {
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

int win31x_desktop_state_save(const Win31xDesktopState *state)
{
    char *contents;
    size_t content_length;
    char temporary_name[96] = "";
    int directory_fd = -1;
    int file_fd = -1;
    int saved_errno;
    bool renamed = false;

    if (state == NULL) {
        errno = EINVAL;
        return -1;
    }
    if (!state->write_enabled) {
        errno = ENOTSUP;
        return -1;
    }
    contents = malloc(DESKTOP_STATE_FILE_LIMIT + 1U);
    if (contents == NULL)
        return -1;
    if (serialize_desktop_state(state, contents,
                                DESKTOP_STATE_FILE_LIMIT + 1U,
                                &content_length) < 0) {
        saved_errno = errno;
        free(contents);
        errno = saved_errno;
        return -1;
    }

    directory_fd = open_desktop_state_directory(true);
    if (directory_fd < 0)
        goto fail;
    if (ensure_safe_final_target(directory_fd) < 0)
        goto fail;
    file_fd = create_temporary_file(directory_fd, temporary_name,
                                    sizeof(temporary_name));
    if (file_fd < 0)
        goto fail;
    if (fchmod(file_fd, (mode_t)0600) < 0 ||
        write_all(file_fd, contents, content_length) < 0 ||
        fsync(file_fd) < 0)
        goto fail;
    if (close(file_fd) < 0) {
        file_fd = -1;
        goto fail;
    }
    file_fd = -1;
    if (renameat(directory_fd, temporary_name, directory_fd,
                 DESKTOP_STATE_FILENAME) < 0)
        goto fail;
    renamed = true;
    if (fsync(directory_fd) < 0 && errno != EINVAL && errno != EROFS)
        goto fail;
    if (close(directory_fd) < 0) {
        directory_fd = -1;
        goto fail;
    }
    directory_fd = -1;
    free(contents);
    return 0;

fail:
    saved_errno = errno;
    if (file_fd >= 0)
        close(file_fd);
    if (!renamed && directory_fd >= 0 && temporary_name[0] != '\0')
        unlinkat(directory_fd, temporary_name, 0);
    if (directory_fd >= 0)
        close(directory_fd);
    free(contents);
    errno = saved_errno;
    return -1;
}
