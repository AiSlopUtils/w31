#define _POSIX_C_SOURCE 200809L

#include "app_icons.h"

#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <unistd.h>

#define APP_ICON_MAX_NAME 255U
#define APP_ICON_MAX_PATH 4096U
#define APP_ICON_MAX_ENVIRONMENT 65536U
#define APP_ICON_MAX_ROOTS 128U
#define APP_ICON_MAX_THEME_ENTRIES 4096U
#define APP_ICON_MAX_DIRECTORY_SIZE 4096U
#define APP_ICON_MAX_SCALE 8U
#define APP_ICON_MAX_FILE_BYTES (64U * 1024U * 1024U)

typedef enum {
    APP_ICON_FORMAT_PNG,
    APP_ICON_FORMAT_XPM
} AppIconFormat;

typedef struct {
    char **items;
    size_t length;
    size_t capacity;
} PathVector;

typedef struct {
    char *path;
    unsigned int nominal_size;
    AppIconFormat format;
} IconCandidate;

typedef struct {
    const char *base_name;
    bool explicit_format;
    AppIconFormat format;
} ParsedIconName;

static bool size_add_overflows(size_t left, size_t right)
{
    return left > SIZE_MAX - right;
}

static char *join_path(const char *left, const char *right)
{
    size_t left_length;
    size_t right_length;
    size_t total;
    bool slash;
    char *result;

    if (left == NULL || right == NULL)
        return NULL;
    left_length = strlen(left);
    right_length = strlen(right);
    slash = left_length > 0U && left[left_length - 1U] != '/';
    if (size_add_overflows(left_length, right_length) ||
        size_add_overflows(left_length + right_length, slash ? 2U : 1U)) {
        errno = ENAMETOOLONG;
        return NULL;
    }
    total = left_length + right_length + (slash ? 2U : 1U);
    if (total > APP_ICON_MAX_PATH) {
        errno = ENAMETOOLONG;
        return NULL;
    }
    result = malloc(total);
    if (result == NULL)
        return NULL;
    memcpy(result, left, left_length);
    if (slash)
        result[left_length++] = '/';
    memcpy(result + left_length, right, right_length + 1U);
    return result;
}

static bool path_vector_contains(const PathVector *vector, const char *path)
{
    size_t index;

    for (index = 0U; index < vector->length; ++index) {
        if (strcmp(vector->items[index], path) == 0)
            return true;
    }
    return false;
}

static int path_vector_push_owned(PathVector *vector, char *path)
{
    char **new_items;
    size_t new_capacity;

    if (path == NULL)
        return -1;
    if (path_vector_contains(vector, path)) {
        free(path);
        return 0;
    }
    if (vector->length >= APP_ICON_MAX_ROOTS) {
        free(path);
        errno = E2BIG;
        return -1;
    }
    if (vector->length == vector->capacity) {
        new_capacity = vector->capacity == 0U ? 8U : vector->capacity * 2U;
        if (new_capacity > APP_ICON_MAX_ROOTS)
            new_capacity = APP_ICON_MAX_ROOTS;
        new_items = realloc(vector->items,
                            new_capacity * sizeof(*vector->items));
        if (new_items == NULL) {
            free(path);
            return -1;
        }
        vector->items = new_items;
        vector->capacity = new_capacity;
    }
    vector->items[vector->length++] = path;
    return 0;
}

static int path_vector_push_join(PathVector *vector, const char *left,
                                 const char *right)
{
    char *path = join_path(left, right);

    if (path == NULL)
        return -1;
    return path_vector_push_owned(vector, path);
}

static void path_vector_free(PathVector *vector)
{
    size_t index;

    for (index = 0U; index < vector->length; ++index)
        free(vector->items[index]);
    free(vector->items);
    memset(vector, 0, sizeof(*vector));
}

static int add_data_root(PathVector *theme_directories,
                         PathVector *pixmap_directories, const char *root)
{
    char *icons;
    char *hicolor;

    if (root == NULL || root[0] != '/')
        return 0;
    icons = join_path(root, "icons");
    if (icons == NULL)
        return -1;
    hicolor = join_path(icons, "hicolor");
    free(icons);
    if (hicolor == NULL)
        return -1;
    if (path_vector_push_owned(theme_directories, hicolor) < 0)
        return -1;
    return path_vector_push_join(pixmap_directories, root, "pixmaps");
}

static int add_user_roots(PathVector *theme_directories,
                          PathVector *pixmap_directories)
{
    const char *data_home = getenv("XDG_DATA_HOME");
    const char *home = getenv("HOME");
    char *fallback = NULL;
    char *legacy = NULL;
    int result = -1;

    if (data_home != NULL && data_home[0] == '/') {
        if (add_data_root(theme_directories, pixmap_directories, data_home) < 0)
            return -1;
    } else if (home != NULL && home[0] == '/') {
        fallback = join_path(home, ".local/share");
        if (fallback == NULL)
            return -1;
        if (add_data_root(theme_directories, pixmap_directories, fallback) < 0)
            goto done;
    }

    if (home != NULL && home[0] == '/') {
        char *icons = join_path(home, ".icons");

        if (icons == NULL)
            goto done;
        legacy = join_path(icons, "hicolor");
        free(icons);
        if (legacy == NULL)
            goto done;
        if (path_vector_push_owned(theme_directories, legacy) < 0) {
            legacy = NULL;
            goto done;
        }
        legacy = NULL;
    }
    result = 0;

done:
    free(legacy);
    free(fallback);
    return result;
}

static int add_system_roots(PathVector *theme_directories,
                            PathVector *pixmap_directories)
{
    const char *environment = getenv("XDG_DATA_DIRS");
    const char *defaults = "/usr/local/share:/usr/share";
    char *copy;
    char *cursor;

    if (environment == NULL || environment[0] == '\0' ||
        strlen(environment) > APP_ICON_MAX_ENVIRONMENT)
        environment = defaults;
    copy = strdup(environment);
    if (copy == NULL)
        return -1;
    cursor = copy;
    while (cursor != NULL) {
        char *next = strchr(cursor, ':');

        if (next != NULL)
            *next++ = '\0';
        if (cursor[0] == '/' &&
            add_data_root(theme_directories, pixmap_directories, cursor) < 0) {
            free(copy);
            return -1;
        }
        cursor = next;
    }
    free(copy);
    return 0;
}

static bool has_suffix_case_insensitive(const char *text, const char *suffix)
{
    size_t text_length = strlen(text);
    size_t suffix_length = strlen(suffix);

    return text_length >= suffix_length &&
           strcasecmp(text + text_length - suffix_length, suffix) == 0;
}

static int parse_icon_name(const char *icon_name, ParsedIconName *parsed)
{
    size_t length = strlen(icon_name);

    if (length == 0U || length > APP_ICON_MAX_NAME ||
        strchr(icon_name, '/') != NULL || strcmp(icon_name, ".") == 0 ||
        strcmp(icon_name, "..") == 0) {
        errno = EINVAL;
        return -1;
    }
    parsed->base_name = icon_name;
    parsed->explicit_format = false;
    parsed->format = APP_ICON_FORMAT_PNG;
    if (has_suffix_case_insensitive(icon_name, ".png")) {
        parsed->explicit_format = true;
        parsed->format = APP_ICON_FORMAT_PNG;
    } else if (has_suffix_case_insensitive(icon_name, ".xpm")) {
        parsed->explicit_format = true;
        parsed->format = APP_ICON_FORMAT_XPM;
    } else if (has_suffix_case_insensitive(icon_name, ".svg") ||
               has_suffix_case_insensitive(icon_name, ".svgz")) {
        errno = ENOENT;
        return -1;
    }
    return 0;
}

static bool file_signature_matches(int descriptor, AppIconFormat format)
{
    unsigned char bytes[64];
    ssize_t count;
    size_t offset = 0U;
    static const unsigned char png_signature[8] = {
        0x89U, 'P', 'N', 'G', 0x0dU, 0x0aU, 0x1aU, 0x0aU
    };

    count = read(descriptor, bytes, sizeof(bytes));
    if (count < 0)
        return false;
    if (format == APP_ICON_FORMAT_PNG)
        return count >= (ssize_t)sizeof(png_signature) &&
               memcmp(bytes, png_signature, sizeof(png_signature)) == 0;
    while (offset < (size_t)count && isspace(bytes[offset]))
        ++offset;
    return (size_t)count - offset >= 9U &&
           memcmp(bytes + offset, "/* XPM */", 9U) == 0;
}

static bool usable_icon_file(const char *path, AppIconFormat format)
{
    struct stat information;
    int descriptor;
    int flags = O_RDONLY | O_NONBLOCK;
    bool usable = false;

#ifdef O_CLOEXEC
    flags |= O_CLOEXEC;
#endif
    descriptor = open(path, flags);
    if (descriptor < 0)
        return false;
    if (fstat(descriptor, &information) == 0 && S_ISREG(information.st_mode) &&
        information.st_size > 0 &&
        (uintmax_t)information.st_size <= APP_ICON_MAX_FILE_BYTES &&
        file_signature_matches(descriptor, format))
        usable = true;
    close(descriptor);
    return usable;
}

static int make_icon_filename(const ParsedIconName *parsed,
                              AppIconFormat format, char **filename_out)
{
    const char *extension = format == APP_ICON_FORMAT_PNG ? ".png" : ".xpm";
    size_t name_length = strlen(parsed->base_name);
    size_t extension_length = strlen(extension);
    char *filename;

    *filename_out = NULL;
    if (parsed->explicit_format) {
        if (parsed->format != format)
            return 0;
        filename = strdup(parsed->base_name);
    } else {
        if (size_add_overflows(name_length, extension_length + 1U)) {
            errno = ENAMETOOLONG;
            return -1;
        }
        filename = malloc(name_length + extension_length + 1U);
        if (filename != NULL) {
            memcpy(filename, parsed->base_name, name_length);
            memcpy(filename + name_length, extension, extension_length + 1U);
        }
    }
    if (filename == NULL)
        return -1;
    *filename_out = filename;
    return 1;
}

static int candidate_path(const char *directory, const ParsedIconName *parsed,
                          AppIconFormat format, char **path_out)
{
    char *filename = NULL;
    char *path;
    int made;

    *path_out = NULL;
    made = make_icon_filename(parsed, format, &filename);
    if (made <= 0)
        return made;
    path = join_path(directory, filename);
    free(filename);
    if (path == NULL)
        return -1;
    if (!usable_icon_file(path, format)) {
        free(path);
        return 0;
    }
    *path_out = path;
    return 1;
}

static bool parse_decimal(const char **cursor, unsigned int *value)
{
    const char *text = *cursor;
    unsigned int result = 0U;
    bool any = false;

    while (*text >= '0' && *text <= '9') {
        unsigned int digit = (unsigned int)(*text - '0');

        if (result > (APP_ICON_MAX_DIRECTORY_SIZE - digit) / 10U)
            return false;
        result = result * 10U + digit;
        any = true;
        ++text;
    }
    if (!any || result == 0U)
        return false;
    *cursor = text;
    *value = result;
    return true;
}

static bool parse_size_directory(const char *name, unsigned int *nominal_size)
{
    const char *cursor = name;
    unsigned int width;
    unsigned int height;
    unsigned int scale = 1U;

    if (!parse_decimal(&cursor, &width) || *cursor++ != 'x' ||
        !parse_decimal(&cursor, &height))
        return false;
    if (*cursor == '@') {
        ++cursor;
        if (!parse_decimal(&cursor, &scale) || scale > APP_ICON_MAX_SCALE)
            return false;
    }
    if (*cursor != '\0' || width != height ||
        width > APP_ICON_MAX_DIRECTORY_SIZE / scale)
        return false;
    *nominal_size = width * scale;
    return true;
}

static bool candidate_is_better(const IconCandidate *candidate,
                                const IconCandidate *current,
                                unsigned int target_size)
{
    bool candidate_exact = candidate->nominal_size == target_size;
    bool current_exact = current->nominal_size == target_size;
    bool candidate_larger = candidate->nominal_size > target_size;
    bool current_larger = current->nominal_size > target_size;

    if (current->path == NULL)
        return true;
    if (candidate_exact != current_exact)
        return candidate_exact;
    if (!candidate_exact && candidate_larger != current_larger)
        return candidate_larger;
    if (candidate->nominal_size != current->nominal_size) {
        if (candidate_larger)
            return candidate->nominal_size < current->nominal_size;
        return candidate->nominal_size > current->nominal_size;
    }
    return candidate->format == APP_ICON_FORMAT_PNG &&
           current->format == APP_ICON_FORMAT_XPM;
}

static int consider_theme_candidate(const char *apps_directory,
                                    const ParsedIconName *parsed,
                                    unsigned int nominal_size,
                                    unsigned int target_size,
                                    AppIconFormat format,
                                    IconCandidate *best)
{
    IconCandidate candidate = {0};
    int result;

    result = candidate_path(apps_directory, parsed, format, &candidate.path);
    if (result <= 0)
        return result;
    candidate.nominal_size = nominal_size;
    candidate.format = format;
    if (candidate_is_better(&candidate, best, target_size)) {
        free(best->path);
        *best = candidate;
    } else {
        free(candidate.path);
    }
    return 1;
}

static int search_theme_directory(const char *theme_directory,
                                  const ParsedIconName *parsed,
                                  unsigned int target_size, char **path_out)
{
    DIR *directory;
    struct dirent *entry;
    IconCandidate best = {0};
    unsigned int inspected = 0U;
    int saved_error = 0;

    *path_out = NULL;
    directory = opendir(theme_directory);
    if (directory == NULL)
        return 0;
    while (inspected < APP_ICON_MAX_THEME_ENTRIES) {
        unsigned int nominal_size;
        char *size_directory;
        char *apps_directory;
        int result;

        errno = 0;
        entry = readdir(directory);
        if (entry == NULL) {
            saved_error = errno;
            break;
        }
        ++inspected;
        if (!parse_size_directory(entry->d_name, &nominal_size))
            continue;
        size_directory = join_path(theme_directory, entry->d_name);
        if (size_directory == NULL) {
            saved_error = errno;
            break;
        }
        apps_directory = join_path(size_directory, "apps");
        free(size_directory);
        if (apps_directory == NULL) {
            saved_error = errno;
            break;
        }
        result = consider_theme_candidate(
            apps_directory, parsed, nominal_size, target_size,
            APP_ICON_FORMAT_PNG, &best);
        if (result >= 0)
            result = consider_theme_candidate(
                apps_directory, parsed, nominal_size, target_size,
                APP_ICON_FORMAT_XPM, &best);
        free(apps_directory);
        if (result < 0) {
            saved_error = errno;
            break;
        }
    }
    closedir(directory);
    if (saved_error != 0) {
        free(best.path);
        errno = saved_error;
        return -1;
    }
    if (best.path == NULL)
        return 0;
    *path_out = best.path;
    return 1;
}

static int search_pixmap_directory(const char *pixmap_directory,
                                   const ParsedIconName *parsed,
                                   char **path_out)
{
    int result;

    *path_out = NULL;
    result = candidate_path(pixmap_directory, parsed, APP_ICON_FORMAT_PNG,
                            path_out);
    if (result != 0)
        return result;
    return candidate_path(pixmap_directory, parsed, APP_ICON_FORMAT_XPM,
                          path_out);
}

static int resolve_absolute(const char *path, char **path_out)
{
    AppIconFormat format;

    if (strlen(path) >= APP_ICON_MAX_PATH) {
        errno = ENAMETOOLONG;
        return -1;
    }
    if (has_suffix_case_insensitive(path, ".png"))
        format = APP_ICON_FORMAT_PNG;
    else if (has_suffix_case_insensitive(path, ".xpm"))
        format = APP_ICON_FORMAT_XPM;
    else {
        errno = ENOENT;
        return -1;
    }
    if (!usable_icon_file(path, format)) {
        errno = ENOENT;
        return -1;
    }
    *path_out = strdup(path);
    return *path_out != NULL ? 0 : -1;
}

int app_icon_resolve(const char *icon_name, unsigned int target_size,
                     char **path_out)
{
    PathVector theme_directories = {0};
    PathVector pixmap_directories = {0};
    ParsedIconName parsed;
    size_t index;
    int result = -1;

    if (path_out == NULL) {
        errno = EINVAL;
        return -1;
    }
    *path_out = NULL;
    if (icon_name == NULL || icon_name[0] == '\0' || target_size == 0U) {
        errno = EINVAL;
        return -1;
    }
    if (icon_name[0] == '/')
        return resolve_absolute(icon_name, path_out);
    if (parse_icon_name(icon_name, &parsed) < 0)
        return -1;
    if (add_user_roots(&theme_directories, &pixmap_directories) < 0 ||
        add_system_roots(&theme_directories, &pixmap_directories) < 0)
        goto done;

    for (index = 0U; index < theme_directories.length; ++index) {
        result = search_theme_directory(theme_directories.items[index], &parsed,
                                        target_size, path_out);
        if (result < 0)
            goto done;
        if (result > 0) {
            result = 0;
            goto done;
        }
    }
    for (index = 0U; index < pixmap_directories.length; ++index) {
        result = search_pixmap_directory(pixmap_directories.items[index],
                                         &parsed, path_out);
        if (result < 0)
            goto done;
        if (result > 0) {
            result = 0;
            goto done;
        }
    }
    errno = ENOENT;
    result = -1;

done:
    path_vector_free(&theme_directories);
    path_vector_free(&pixmap_directories);
    return result;
}
