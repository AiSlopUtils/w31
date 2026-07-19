#define _POSIX_C_SOURCE 200809L

#include "icon_assets.h"

#include <png.h>

#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <unistd.h>

#ifndef WIN31X_ICON_DIR
#define WIN31X_ICON_DIR ""
#endif

#define APPLICATION_ICON_MAX_DIMENSION 2048U
#define APPLICATION_ICON_MAX_PIXELS (APPLICATION_ICON_MAX_DIMENSION * \
                                     APPLICATION_ICON_MAX_DIMENSION)

typedef struct {
    FILE *file;
    png_structp png;
    png_infop info;
    unsigned char *pixels;
    png_bytep *rows;
    char png_error[160];
} PngLoadState;

static const IconAssetDescriptor descriptors[ICON_CATEGORY_COUNT]
                                                    [ICON_SIZE_COUNT] = {
    [ICON_CATEGORY_APPLICATIONS] = {
        [ICON_SIZE_SMALL] = {"applications-16.png", 16, 16},
        [ICON_SIZE_LARGE] = {"applications-48.png", 48, 48},
    },
    [ICON_CATEGORY_EXECUTABLE] = {
        [ICON_SIZE_SMALL] = {"executable-16.png", 16, 16},
        [ICON_SIZE_LARGE] = {"executable-32.png", 32, 32},
    },
    [ICON_CATEGORY_TERMINAL] = {
        [ICON_SIZE_SMALL] = {"terminal-16.png", 16, 16},
        [ICON_SIZE_LARGE] = {"terminal-32.png", 32, 32},
    },
    [ICON_CATEGORY_CLOCK] = {
        [ICON_SIZE_SMALL] = {"clock-16.png", 16, 16},
        [ICON_SIZE_LARGE] = {"clock-32.png", 32, 32},
    },
    [ICON_CATEGORY_EDITOR] = {
        [ICON_SIZE_SMALL] = {"editor-16.png", 16, 16},
        [ICON_SIZE_LARGE] = {"editor-48.png", 48, 48},
    },
    [ICON_CATEGORY_CALCULATOR] = {
        [ICON_SIZE_SMALL] = {"calculator-16.png", 16, 16},
        [ICON_SIZE_LARGE] = {"calculator-32.png", 32, 32},
    },
    [ICON_CATEGORY_FILES] = {
        [ICON_SIZE_SMALL] = {"files-16.png", 16, 16},
        [ICON_SIZE_LARGE] = {"files-48.png", 48, 48},
    },
    [ICON_CATEGORY_WEB] = {
        [ICON_SIZE_SMALL] = {"web-16.png", 16, 16},
        [ICON_SIZE_LARGE] = {"web-48.png", 48, 48},
    },
    [ICON_CATEGORY_MAIL] = {
        [ICON_SIZE_SMALL] = {"mail-16.png", 16, 16},
        [ICON_SIZE_LARGE] = {"mail-48.png", 48, 48},
    },
    [ICON_CATEGORY_GRAPHICS] = {
        [ICON_SIZE_SMALL] = {"graphics-16.png", 16, 16},
        [ICON_SIZE_LARGE] = {"graphics-48.png", 48, 48},
    },
    [ICON_CATEGORY_MEDIA] = {
        [ICON_SIZE_SMALL] = {"media-16.png", 16, 16},
        [ICON_SIZE_LARGE] = {"media-48.png", 48, 48},
    },
    [ICON_CATEGORY_TASK_MANAGER] = {
        [ICON_SIZE_SMALL] = {"task-manager-16.png", 16, 16},
        [ICON_SIZE_LARGE] = {"task-manager-32.png", 32, 32},
    },
    [ICON_CATEGORY_SETTINGS] = {
        [ICON_SIZE_SMALL] = {"settings-16.png", 16, 16},
        [ICON_SIZE_LARGE] = {"settings-48.png", 48, 48},
    },
    [ICON_CATEGORY_NETWORK] = {
        [ICON_SIZE_SMALL] = {"network-16.png", 16, 16},
        [ICON_SIZE_LARGE] = {"network-48.png", 48, 48},
    },
    [ICON_CATEGORY_GAMES] = {
        [ICON_SIZE_SMALL] = {"games-16.png", 16, 16},
        [ICON_SIZE_LARGE] = {"games-48.png", 48, 48},
    },
    [ICON_CATEGORY_HELP] = {
        [ICON_SIZE_SMALL] = {"help-16.png", 16, 16},
        [ICON_SIZE_LARGE] = {"help-32.png", 32, 32},
    },
};

static bool valid_category(IconCategory category)
{
    return category >= 0 && category < ICON_CATEGORY_COUNT;
}

static bool valid_size(IconSize size)
{
    return size >= 0 && size < ICON_SIZE_COUNT;
}

const IconAssetDescriptor *icon_assets_descriptor(IconCategory category,
                                                   IconSize size)
{
    if (!valid_category(category) || !valid_size(size))
        return NULL;
    return &descriptors[category][size];
}

const IconImage *icon_assets_get(const IconAssets *assets,
                                 IconCategory category, IconSize size)
{
    if (assets == NULL || !valid_category(category) || !valid_size(size) ||
        assets->images[category][size].rgba == NULL)
        return NULL;
    return &assets->images[category][size];
}

const char *icon_assets_directory(const IconAssets *assets)
{
    return assets == NULL ? NULL : assets->directory;
}

void icon_assets_free(IconAssets *assets)
{
    int category;
    int size;

    if (assets == NULL)
        return;
    for (category = 0; category < ICON_CATEGORY_COUNT; ++category) {
        for (size = 0; size < ICON_SIZE_COUNT; ++size)
            free(assets->images[category][size].rgba);
    }
    free(assets->directory);
    memset(assets, 0, sizeof(*assets));
}

static int join_path(char **result, const char *directory, const char *filename)
{
    size_t directory_length;
    size_t filename_length;
    bool needs_slash;
    char *joined;

    if (result == NULL || directory == NULL || filename == NULL) {
        errno = EINVAL;
        return -1;
    }
    directory_length = strlen(directory);
    filename_length = strlen(filename);
    needs_slash = directory_length > 0 && directory[directory_length - 1] != '/';
    if (directory_length > SIZE_MAX - filename_length - (needs_slash ? 2u : 1u)) {
        errno = ENAMETOOLONG;
        return -1;
    }
    joined = malloc(directory_length + filename_length + (needs_slash ? 2u : 1u));
    if (joined == NULL)
        return -1;
    memcpy(joined, directory, directory_length);
    if (needs_slash)
        joined[directory_length++] = '/';
    memcpy(joined + directory_length, filename, filename_length + 1);
    *result = joined;
    return 0;
}

static void png_error_handler(png_structp png, png_const_charp message)
{
    PngLoadState *state = png_get_error_ptr(png);

    if (state != NULL)
        snprintf(state->png_error, sizeof(state->png_error), "%s", message);
    png_longjmp(png, 1);
}

static void png_warning_handler(png_structp png, png_const_charp message)
{
    (void)png;
    (void)message;
}

static void png_state_destroy(PngLoadState *state)
{
    if (state == NULL)
        return;
    free(state->rows);
    free(state->pixels);
    if (state->png != NULL)
        png_destroy_read_struct(&state->png, &state->info, NULL);
    if (state->file != NULL)
        fclose(state->file);
    free(state);
}

static bool size_multiplication_overflows(size_t count, size_t item_size)
{
    return item_size != 0 && count > SIZE_MAX / item_size;
}

static int load_png(FILE *input, const char *path,
                    unsigned int expected_width,
                    unsigned int expected_height, IconImage *image,
                    bool required_asset)
{
    PngLoadState *state;
    unsigned char signature[8];
    png_uint_32 width;
    png_uint_32 height;
    int bit_depth;
    int color_type;
    int interlace_type;
    png_size_t row_bytes;
    size_t pixel_bytes;
    png_uint_32 row;

    state = calloc(1, sizeof(*state));
    if (state == NULL) {
        if (input != NULL)
            fclose(input);
        fprintf(stderr, "win31x: cannot allocate PNG loader for %s\n", path);
        errno = ENOMEM;
        return -1;
    }
    state->file = input != NULL ? input : fopen(path, "rb");
    if (state->file == NULL) {
        if (required_asset)
            fprintf(stderr,
                    "win31x: required icon asset %s cannot be opened: %s\n",
                    path, strerror(errno));
        png_state_destroy(state);
        return -1;
    }
    if (fread(signature, 1, sizeof(signature), state->file) != sizeof(signature) ||
        png_sig_cmp(signature, 0, sizeof(signature)) != 0) {
        if (required_asset)
            fprintf(stderr,
                    "win31x: required icon asset %s is not a valid PNG\n",
                    path);
        errno = EINVAL;
        png_state_destroy(state);
        return -1;
    }
    state->png = png_create_read_struct(PNG_LIBPNG_VER_STRING, state,
                                        png_error_handler, png_warning_handler);
    if (state->png == NULL) {
        if (required_asset)
            fprintf(stderr, "win31x: libpng initialization failed for %s\n",
                    path);
        errno = ENOMEM;
        png_state_destroy(state);
        return -1;
    }
    state->info = png_create_info_struct(state->png);
    if (state->info == NULL) {
        if (required_asset)
            fprintf(stderr,
                    "win31x: libpng metadata allocation failed for %s\n",
                    path);
        errno = ENOMEM;
        png_state_destroy(state);
        return -1;
    }
    if (setjmp(png_jmpbuf(state->png)) != 0) {
        if (required_asset)
            fprintf(stderr, "win31x: invalid icon asset %s: %s\n", path,
                    state->png_error[0] != '\0' ? state->png_error
                                                : "PNG decoding failed");
        errno = EINVAL;
        png_state_destroy(state);
        return -1;
    }

    png_init_io(state->png, state->file);
    png_set_sig_bytes(state->png, sizeof(signature));
    png_read_info(state->png, state->info);
    png_get_IHDR(state->png, state->info, &width, &height, &bit_depth,
                 &color_type, &interlace_type, NULL, NULL);
    if ((expected_width != 0U && width != expected_width) ||
        (expected_height != 0U && height != expected_height)) {
        fprintf(stderr,
                "win31x: icon asset %s has size %ux%u; expected %ux%u\n",
                path, (unsigned)width, (unsigned)height, expected_width,
                expected_height);
        errno = EINVAL;
        png_state_destroy(state);
        return -1;
    }
    if (width == 0U || height == 0U ||
        width > APPLICATION_ICON_MAX_DIMENSION ||
        height > APPLICATION_ICON_MAX_DIMENSION ||
        (uint64_t)width * (uint64_t)height > APPLICATION_ICON_MAX_PIXELS) {
        if (required_asset)
            fprintf(stderr, "win31x: icon asset %s has unreasonable dimensions\n",
                    path);
        errno = EFBIG;
        png_state_destroy(state);
        return -1;
    }
    if (bit_depth == 16)
        png_set_strip_16(state->png);
    if (color_type == PNG_COLOR_TYPE_PALETTE)
        png_set_palette_to_rgb(state->png);
    if (color_type == PNG_COLOR_TYPE_GRAY && bit_depth < 8)
        png_set_expand_gray_1_2_4_to_8(state->png);
    if (png_get_valid(state->png, state->info, PNG_INFO_tRNS))
        png_set_tRNS_to_alpha(state->png);
    if (color_type == PNG_COLOR_TYPE_GRAY ||
        color_type == PNG_COLOR_TYPE_GRAY_ALPHA)
        png_set_gray_to_rgb(state->png);
    if ((color_type & PNG_COLOR_MASK_ALPHA) == 0 &&
        !png_get_valid(state->png, state->info, PNG_INFO_tRNS))
        png_set_add_alpha(state->png, 0xff, PNG_FILLER_AFTER);
    if (interlace_type != PNG_INTERLACE_NONE)
        (void)png_set_interlace_handling(state->png);
    png_read_update_info(state->png, state->info);

    row_bytes = png_get_rowbytes(state->png, state->info);
    if (row_bytes != (png_size_t)width * 4u ||
        size_multiplication_overflows((size_t)height,
                                      sizeof(*state->rows)) ||
        size_multiplication_overflows((size_t)height,
                                      (size_t)row_bytes)) {
        if (required_asset)
            fprintf(stderr,
                    "win31x: icon asset %s has an unsupported pixel layout\n",
                    path);
        errno = EINVAL;
        png_state_destroy(state);
        return -1;
    }
    pixel_bytes = (size_t)row_bytes * height;
    state->pixels = malloc(pixel_bytes);
    state->rows = malloc((size_t)height * sizeof(*state->rows));
    if (state->pixels == NULL || state->rows == NULL) {
        if (required_asset)
            fprintf(stderr,
                    "win31x: cannot allocate pixels for icon asset %s\n",
                    path);
        errno = ENOMEM;
        png_state_destroy(state);
        return -1;
    }
    for (row = 0; row < height; ++row)
        state->rows[row] = state->pixels + (size_t)row * row_bytes;
    png_read_image(state->png, state->rows);
    png_read_end(state->png, NULL);

    image->width = (unsigned int)width;
    image->height = (unsigned int)height;
    image->rgba = state->pixels;
    state->pixels = NULL;
    png_state_destroy(state);
    return 0;
}

typedef struct {
    char *key;
    unsigned char rgba[4];
} XpmColor;

static int xpm_append_character(char **text, size_t *length, size_t *capacity,
                                unsigned char character)
{
    char *grown;
    size_t next_capacity;

    if (*length + 1U >= *capacity) {
        next_capacity = *capacity == 0U ? 128U : *capacity * 2U;
        if (next_capacity > 4U * 1024U * 1024U) {
            errno = EFBIG;
            return -1;
        }
        grown = realloc(*text, next_capacity);
        if (grown == NULL)
            return -1;
        *text = grown;
        *capacity = next_capacity;
    }
    (*text)[(*length)++] = (char)character;
    (*text)[*length] = '\0';
    return 0;
}

/* Return one decoded C string from an XPM3 source stream. */
static int xpm_read_string(FILE *file, char **text_out)
{
    char *text = NULL;
    size_t length = 0U;
    size_t capacity = 0U;
    int character;

    *text_out = NULL;
    do {
        character = fgetc(file);
        if (character == EOF)
            return ferror(file) ? -1 : 0;
    } while (character != '"');

    for (;;) {
        character = fgetc(file);
        if (character == EOF) {
            free(text);
            errno = EINVAL;
            return -1;
        }
        if (character == '"')
            break;
        if (character == '\\') {
            unsigned int value;
            int escaped = fgetc(file);

            if (escaped == EOF) {
                free(text);
                errno = EINVAL;
                return -1;
            }
            if (escaped == '\n')
                continue;
            if (escaped == 'n')
                character = '\n';
            else if (escaped == 'r')
                character = '\r';
            else if (escaped == 't')
                character = '\t';
            else if (escaped >= '0' && escaped <= '7') {
                int count;

                value = (unsigned int)(escaped - '0');
                for (count = 1; count < 3; ++count) {
                    int next = fgetc(file);

                    if (next < '0' || next > '7') {
                        if (next != EOF)
                            ungetc(next, file);
                        break;
                    }
                    value = value * 8U + (unsigned int)(next - '0');
                }
                character = (int)(value & 0xffU);
            } else {
                character = escaped;
            }
        }
        if (character == '\0' ||
            xpm_append_character(&text, &length, &capacity,
                                 (unsigned char)character) < 0) {
            free(text);
            errno = EINVAL;
            return -1;
        }
    }
    if (text == NULL) {
        text = strdup("");
        if (text == NULL)
            return -1;
    }
    *text_out = text;
    return 1;
}

static int hex_digit_value(unsigned char character)
{
    if (character >= '0' && character <= '9')
        return character - '0';
    if (character >= 'a' && character <= 'f')
        return character - 'a' + 10;
    if (character >= 'A' && character <= 'F')
        return character - 'A' + 10;
    return -1;
}

static bool parse_hex_component(const char *text, size_t digits,
                                unsigned char *component)
{
    size_t index;
    unsigned int value = 0U;
    unsigned int maximum = 0U;

    for (index = 0U; index < digits; ++index) {
        int digit = hex_digit_value((unsigned char)text[index]);

        if (digit < 0)
            return false;
        value = value * 16U + (unsigned int)digit;
        maximum = maximum * 16U + 15U;
    }
    *component = (unsigned char)((value * 255U + maximum / 2U) / maximum);
    return true;
}

static bool xpm_named_color(const char *name, unsigned char rgba[4])
{
    static const struct {
        const char *name;
        unsigned char red;
        unsigned char green;
        unsigned char blue;
    } colors[] = {
        {"black", 0, 0, 0},       {"white", 255, 255, 255},
        {"red", 255, 0, 0},       {"green", 0, 128, 0},
        {"blue", 0, 0, 255},      {"yellow", 255, 255, 0},
        {"cyan", 0, 255, 255},    {"magenta", 255, 0, 255},
        {"gray", 128, 128, 128},  {"grey", 128, 128, 128},
        {"darkgray", 169, 169, 169}, {"darkgrey", 169, 169, 169},
        {"lightgray", 211, 211, 211}, {"lightgrey", 211, 211, 211},
        {"navy", 0, 0, 128},      {"maroon", 128, 0, 0},
        {"olive", 128, 128, 0},   {"purple", 128, 0, 128},
        {"teal", 0, 128, 128},    {"silver", 192, 192, 192},
        {"orange", 255, 165, 0},  {"brown", 165, 42, 42},
    };
    size_t index;

    if (strcasecmp(name, "none") == 0) {
        memset(rgba, 0, 4U);
        return true;
    }
    if ((strncasecmp(name, "gray", 4U) == 0 ||
         strncasecmp(name, "grey", 4U) == 0) &&
        isdigit((unsigned char)name[4])) {
        char *end = NULL;
        long percentage = strtol(name + 4, &end, 10);

        if (end != name + 4 && *end == '\0' && percentage >= 0 &&
            percentage <= 100) {
            unsigned char value =
                (unsigned char)((percentage * 255L + 50L) / 100L);

            rgba[0] = value;
            rgba[1] = value;
            rgba[2] = value;
            rgba[3] = 255U;
            return true;
        }
    }
    for (index = 0U; index < sizeof(colors) / sizeof(colors[0]); ++index) {
        if (strcasecmp(name, colors[index].name) == 0) {
            rgba[0] = colors[index].red;
            rgba[1] = colors[index].green;
            rgba[2] = colors[index].blue;
            rgba[3] = 255U;
            return true;
        }
    }
    return false;
}

static bool xpm_parse_color_value(const char *value, unsigned char rgba[4])
{
    size_t length;
    size_t digits;

    while (isspace((unsigned char)*value))
        ++value;
    length = strlen(value);
    while (length > 0U && isspace((unsigned char)value[length - 1U]))
        --length;
    if (length == 0U)
        return false;
    if (value[0] != '#') {
        char name[64];

        if (length >= sizeof(name))
            return false;
        memcpy(name, value, length);
        name[length] = '\0';
        return xpm_named_color(name, rgba);
    }
    if (length != 4U && length != 7U && length != 13U)
        return false;
    digits = (length - 1U) / 3U;
    if (!parse_hex_component(value + 1U, digits, &rgba[0]) ||
        !parse_hex_component(value + 1U + digits, digits, &rgba[1]) ||
        !parse_hex_component(value + 1U + digits * 2U, digits, &rgba[2]))
        return false;
    rgba[3] = 255U;
    return true;
}

static bool xpm_is_color_field(const char *field, size_t length)
{
    return (length == 1U &&
            (field[0] == 's' || field[0] == 'm' || field[0] == 'g' ||
             field[0] == 'c')) ||
           (length == 2U && field[0] == 'g' && field[1] == '4');
}

static const char *xpm_color_value(char *line, unsigned int characters_per_pixel)
{
    char *cursor = line + characters_per_pixel;

    while (*cursor != '\0') {
        char *field;

        while (isspace((unsigned char)*cursor))
            ++cursor;
        if (*cursor == '\0')
            break;
        field = cursor;
        while (*cursor != '\0' && !isspace((unsigned char)*cursor))
            ++cursor;
        if ((size_t)(cursor - field) == 1U && field[0] == 'c') {
            char *value;

            while (isspace((unsigned char)*cursor))
                ++cursor;
            value = cursor;
            while (*cursor != '\0') {
                char *separator;

                while (*cursor != '\0' &&
                       !isspace((unsigned char)*cursor))
                    ++cursor;
                separator = cursor;
                while (isspace((unsigned char)*cursor))
                    ++cursor;
                if (*cursor == '\0')
                    break;
                field = cursor;
                while (*cursor != '\0' &&
                       !isspace((unsigned char)*cursor))
                    ++cursor;
                if (xpm_is_color_field(field, (size_t)(cursor - field))) {
                    while (separator > value &&
                           isspace((unsigned char)separator[-1]))
                        --separator;
                    *separator = '\0';
                    break;
                }
            }
            return value;
        }
    }
    return NULL;
}

static uint64_t xpm_key_hash(const char *key, unsigned int length)
{
    uint64_t hash = 1469598103934665603ULL;
    unsigned int index;

    for (index = 0U; index < length; ++index) {
        hash ^= (unsigned char)key[index];
        hash *= 1099511628211ULL;
    }
    return hash;
}

static bool xpm_header_number(const char **cursor, unsigned int *value)
{
    const char *start = *cursor;
    char *end;
    unsigned long parsed;

    while (isspace((unsigned char)*start))
        ++start;
    if (!isdigit((unsigned char)*start))
        return false;
    errno = 0;
    parsed = strtoul(start, &end, 10);
    if (errno == ERANGE || end == start || parsed > UINT_MAX)
        return false;
    *cursor = end;
    *value = (unsigned int)parsed;
    return true;
}

static bool xpm_parse_header(const char *line, unsigned int *width,
                             unsigned int *height,
                             unsigned int *color_count,
                             unsigned int *characters_per_pixel)
{
    const char *cursor = line;

    if (!xpm_header_number(&cursor, width) ||
        !xpm_header_number(&cursor, height) ||
        !xpm_header_number(&cursor, color_count) ||
        !xpm_header_number(&cursor, characters_per_pixel))
        return false;
    return *cursor == '\0' || isspace((unsigned char)*cursor);
}

static int load_xpm(FILE *file, IconImage *image)
{
    char *line = NULL;
    XpmColor *colors = NULL;
    size_t *table = NULL;
    size_t table_size = 1U;
    unsigned char *pixels = NULL;
    unsigned int width;
    unsigned int height;
    unsigned int color_count;
    unsigned int cpp;
    unsigned int index;
    int result = -1;

    if (file == NULL) {
        errno = EINVAL;
        return -1;
    }
    if (xpm_read_string(file, &line) != 1 ||
        !xpm_parse_header(line, &width, &height, &color_count, &cpp) ||
        width == 0U || height == 0U || color_count == 0U || cpp == 0U ||
        cpp > 16U || color_count > 65536U ||
        width > APPLICATION_ICON_MAX_DIMENSION ||
        height > APPLICATION_ICON_MAX_DIMENSION ||
        (uint64_t)width * height > APPLICATION_ICON_MAX_PIXELS ||
        (uint64_t)width * cpp > 4U * 1024U * 1024U) {
        errno = EINVAL;
        goto done;
    }
    free(line);
    line = NULL;
    colors = calloc(color_count, sizeof(*colors));
    while (table_size < (size_t)color_count * 2U)
        table_size *= 2U;
    table = calloc(table_size, sizeof(*table));
    if (colors == NULL || table == NULL)
        goto done;
    for (index = 0U; index < color_count; ++index) {
        const char *value;
        size_t slot;

        if (xpm_read_string(file, &line) != 1 || strlen(line) < cpp) {
            errno = EINVAL;
            goto done;
        }
        colors[index].key = malloc((size_t)cpp + 1U);
        if (colors[index].key == NULL)
            goto done;
        memcpy(colors[index].key, line, cpp);
        colors[index].key[cpp] = '\0';
        value = xpm_color_value(line, cpp);
        if (value == NULL || !xpm_parse_color_value(value, colors[index].rgba)) {
            /* Unknown X11 color names are uncommon in application XPMs.  A
             * neutral opaque pixel preserves the icon's silhouette safely. */
            colors[index].rgba[0] = 128U;
            colors[index].rgba[1] = 128U;
            colors[index].rgba[2] = 128U;
            colors[index].rgba[3] = 255U;
        }
        slot = (size_t)xpm_key_hash(colors[index].key, cpp) & (table_size - 1U);
        while (table[slot] != 0U) {
            if (memcmp(colors[table[slot] - 1U].key, colors[index].key,
                       cpp) == 0) {
                errno = EINVAL;
                goto done;
            }
            slot = (slot + 1U) & (table_size - 1U);
        }
        table[slot] = (size_t)index + 1U;
        free(line);
        line = NULL;
    }
    pixels = malloc((size_t)width * height * 4U);
    if (pixels == NULL)
        goto done;
    for (index = 0U; index < height; ++index) {
        unsigned int x;

        if (xpm_read_string(file, &line) != 1 ||
            strlen(line) < (size_t)width * cpp) {
            errno = EINVAL;
            goto done;
        }
        for (x = 0U; x < width; ++x) {
            const char *key = line + (size_t)x * cpp;
            size_t slot = (size_t)xpm_key_hash(key, cpp) & (table_size - 1U);
            size_t probes = 0U;
            size_t color_index;

            while (table[slot] != 0U &&
                   memcmp(colors[table[slot] - 1U].key, key, cpp) != 0) {
                slot = (slot + 1U) & (table_size - 1U);
                if (++probes >= table_size)
                    break;
            }
            if (table[slot] == 0U || probes >= table_size) {
                errno = EINVAL;
                goto done;
            }
            color_index = table[slot] - 1U;
            memcpy(pixels + ((size_t)index * width + x) * 4U,
                   colors[color_index].rgba, 4U);
        }
        free(line);
        line = NULL;
    }
    image->width = width;
    image->height = height;
    image->rgba = pixels;
    pixels = NULL;
    result = 0;

done:
    free(line);
    free(pixels);
    free(table);
    if (colors != NULL) {
        for (index = 0U; index < color_count; ++index)
            free(colors[index].key);
    }
    free(colors);
    fclose(file);
    return result;
}

int icon_image_load_file(const char *path, IconImage *image)
{
    struct stat info;
    const char *extension;
    FILE *file = NULL;
    int descriptor;
    int flags = O_RDONLY | O_NONBLOCK;
    unsigned char signature[64];
    ssize_t signature_length;
    bool is_png;
    bool is_xpm = false;
    size_t offset = 0U;
    static const unsigned char xpm_signature[] = "/* XPM */";

    if (path == NULL || path[0] == '\0' || image == NULL) {
        errno = EINVAL;
        return -1;
    }
    memset(image, 0, sizeof(*image));
#ifdef O_CLOEXEC
    flags |= O_CLOEXEC;
#endif
    descriptor = open(path, flags);
    if (descriptor < 0)
        return -1;
    if (fstat(descriptor, &info) < 0) {
        int saved_errno = errno;

        close(descriptor);
        errno = saved_errno;
        return -1;
    }
    if (!S_ISREG(info.st_mode) || info.st_size <= 0 ||
        (uint64_t)info.st_size > 64U * 1024U * 1024U) {
        close(descriptor);
        errno = !S_ISREG(info.st_mode) ? EINVAL : EFBIG;
        return -1;
    }
    signature_length = read(descriptor, signature, sizeof(signature));
    if (signature_length < 0 || lseek(descriptor, 0, SEEK_SET) < 0) {
        int saved_errno = errno;

        close(descriptor);
        errno = saved_errno;
        return -1;
    }
    file = fdopen(descriptor, "rb");
    if (file == NULL) {
        int saved_errno = errno;

        close(descriptor);
        errno = saved_errno;
        return -1;
    }
    is_png = signature_length >= 8 && png_sig_cmp(signature, 0, 8U) == 0;
    while (offset < (size_t)signature_length &&
           isspace((unsigned char)signature[offset]))
        ++offset;
    if ((size_t)signature_length - offset >= sizeof(xpm_signature) - 1U &&
        memcmp(signature + offset, xpm_signature,
               sizeof(xpm_signature) - 1U) == 0)
        is_xpm = true;
    extension = strrchr(path, '.');
    if (is_png)
        return load_png(file, path, 0U, 0U, image, false);
    if (is_xpm &&
        (extension == NULL || strcasecmp(extension, ".xpm") == 0))
        return load_xpm(file, image);
    fclose(file);
    errno = ENOTSUP;
    return -1;
}

void icon_image_free(IconImage *image)
{
    if (image == NULL)
        return;
    free(image->rgba);
    memset(image, 0, sizeof(*image));
}

static unsigned char scale_channel(unsigned int value)
{
    return (unsigned char)(value > 255U ? 255U : value);
}

int icon_image_scale_fit(const IconImage *source, unsigned int max_width,
                         unsigned int max_height, IconImage *scaled)
{
    unsigned int width;
    unsigned int height;
    unsigned int x;
    unsigned int y;
    unsigned char *pixels;

    if (source == NULL || source->rgba == NULL || source->width == 0U ||
        source->height == 0U || max_width == 0U || max_height == 0U ||
        scaled == NULL) {
        errno = EINVAL;
        return -1;
    }
    memset(scaled, 0, sizeof(*scaled));
    if (source->width <= max_width && source->height <= max_height) {
        width = source->width;
        height = source->height;
    } else if ((uint64_t)max_width * source->height <=
               (uint64_t)max_height * source->width) {
        width = max_width;
        height = (unsigned int)(((uint64_t)source->height * max_width +
                                 source->width / 2U) /
                                source->width);
    } else {
        height = max_height;
        width = (unsigned int)(((uint64_t)source->width * max_height +
                                source->height / 2U) /
                               source->height);
    }
    if (width == 0U)
        width = 1U;
    if (height == 0U)
        height = 1U;
    if ((uint64_t)width * height > SIZE_MAX / 4U) {
        errno = EOVERFLOW;
        return -1;
    }
    pixels = malloc((size_t)width * height * 4U);
    if (pixels == NULL)
        return -1;

    /* Bilinear filtering gives large desktop icons a clean 48-pixel result. */
    for (y = 0U; y < height; ++y) {
        uint64_t source_y_fixed = height == 1U
                                      ? 0U
                                      : (uint64_t)y * (source->height - 1U) *
                                            65536U / (height - 1U);
        unsigned int y0 = (unsigned int)(source_y_fixed >> 16);
        unsigned int y1 = y0 + 1U < source->height ? y0 + 1U : y0;
        unsigned int fy = (unsigned int)(source_y_fixed & 0xffffU);

        for (x = 0U; x < width; ++x) {
            uint64_t source_x_fixed = width == 1U
                                          ? 0U
                                          : (uint64_t)x *
                                                (source->width - 1U) * 65536U /
                                                (width - 1U);
            unsigned int x0 = (unsigned int)(source_x_fixed >> 16);
            unsigned int x1 = x0 + 1U < source->width ? x0 + 1U : x0;
            unsigned int fx = (unsigned int)(source_x_fixed & 0xffffU);
            size_t destination = ((size_t)y * width + x) * 4U;
            unsigned int channel;

            for (channel = 0U; channel < 4U; ++channel) {
                unsigned int p00 = source->rgba[
                    ((size_t)y0 * source->width + x0) * 4U + channel];
                unsigned int p10 = source->rgba[
                    ((size_t)y0 * source->width + x1) * 4U + channel];
                unsigned int p01 = source->rgba[
                    ((size_t)y1 * source->width + x0) * 4U + channel];
                unsigned int p11 = source->rgba[
                    ((size_t)y1 * source->width + x1) * 4U + channel];
                uint64_t top = (uint64_t)p00 * (65536U - fx) +
                               (uint64_t)p10 * fx;
                uint64_t bottom = (uint64_t)p01 * (65536U - fx) +
                                  (uint64_t)p11 * fx;
                uint64_t value = top * (65536U - fy) + bottom * fy;

                pixels[destination + channel] =
                    scale_channel((unsigned int)((value + (1ULL << 31)) >> 32));
            }
        }
    }
    scaled->width = width;
    scaled->height = height;
    scaled->rgba = pixels;
    return 0;
}

static bool directory_has_all_assets(const char *directory,
                                     const char **missing_filename)
{
    int category;
    int size;

    for (category = 0; category < ICON_CATEGORY_COUNT; ++category) {
        for (size = 0; size < ICON_SIZE_COUNT; ++size) {
            const IconAssetDescriptor *descriptor = &descriptors[category][size];
            struct stat info;
            char *path = NULL;

            if (join_path(&path, directory, descriptor->filename) < 0 ||
                stat(path, &info) < 0 || !S_ISREG(info.st_mode) ||
                access(path, R_OK) < 0) {
                if (missing_filename != NULL)
                    *missing_filename = descriptor->filename;
                free(path);
                return false;
            }
            free(path);
        }
    }
    return true;
}

static int load_directory(IconAssets *assets, const char *directory)
{
    IconAssets loaded;
    const char *missing = NULL;
    int category;
    int size;

    memset(&loaded, 0, sizeof(loaded));
    if (!directory_has_all_assets(directory, &missing)) {
        char *path = NULL;

        if (missing != NULL && join_path(&path, directory, missing) == 0) {
            fprintf(stderr, "win31x: required icon asset is missing or unreadable: %s\n",
                    path);
            free(path);
        } else {
            fprintf(stderr, "win31x: icon asset directory is incomplete: %s\n",
                    directory);
        }
        errno = ENOENT;
        return -1;
    }
    loaded.directory = strdup(directory);
    if (loaded.directory == NULL) {
        fprintf(stderr, "win31x: cannot remember icon asset directory %s\n",
                directory);
        return -1;
    }
    for (category = 0; category < ICON_CATEGORY_COUNT; ++category) {
        for (size = 0; size < ICON_SIZE_COUNT; ++size) {
            const IconAssetDescriptor *descriptor = &descriptors[category][size];
            char *path = NULL;

            if (join_path(&path, directory, descriptor->filename) < 0) {
                fprintf(stderr, "win31x: icon asset path is too long in %s\n",
                        directory);
                icon_assets_free(&loaded);
                return -1;
            }
            if (load_png(NULL, path, descriptor->width, descriptor->height,
                         &loaded.images[category][size], true) < 0) {
                free(path);
                icon_assets_free(&loaded);
                return -1;
            }
            free(path);
        }
    }
    *assets = loaded;
    return 0;
}

int icon_assets_load(IconAssets *assets)
{
    const char *override;
    const char *candidates[] = {
        "./assets/icons",
        WIN31X_ICON_DIR,
        "/usr/local/share/win31x/icons",
        "/usr/share/win31x/icons",
    };
    size_t index;

    if (assets == NULL) {
        errno = EINVAL;
        return -1;
    }
    memset(assets, 0, sizeof(*assets));
    override = getenv("WIN31X_ICON_DIR");
    if (override != NULL && override[0] != '\0')
        return load_directory(assets, override);

    for (index = 0; index < sizeof(candidates) / sizeof(candidates[0]); ++index) {
        if (candidates[index][0] != '\0' &&
            directory_has_all_assets(candidates[index], NULL))
            return load_directory(assets, candidates[index]);
    }

    fprintf(stderr, "win31x: could not locate the required Win98 icon asset set; searched");
    for (index = 0; index < sizeof(candidates) / sizeof(candidates[0]); ++index) {
        if (candidates[index][0] != '\0')
            fprintf(stderr, "%s%s", index == 0 ? " " : ", ", candidates[index]);
    }
    fprintf(stderr, "; set WIN31X_ICON_DIR to the directory containing the PNG files\n");
    errno = ENOENT;
    return -1;
}

static unsigned char ascii_lower(unsigned char character)
{
    if (character >= 'A' && character <= 'Z')
        return (unsigned char)(character + ('a' - 'A'));
    return character;
}

static bool contains_ascii_case_insensitive(const char *text, const char *word)
{
    const unsigned char *start;
    size_t word_length;

    if (text == NULL || word == NULL || word[0] == '\0')
        return false;
    word_length = strlen(word);
    for (start = (const unsigned char *)text; *start != '\0'; ++start) {
        size_t offset;

        for (offset = 0; offset < word_length; ++offset) {
            unsigned char character = start[offset];

            if (character == '\0' ||
                ascii_lower(character) !=
                    ascii_lower((unsigned char)word[offset]))
                break;
        }
        if (offset == word_length)
            return true;
    }
    return false;
}

static bool equals_ascii_case_insensitive(const char *left, const char *right)
{
    if (left == NULL || right == NULL)
        return false;
    while (*left != '\0' && *right != '\0') {
        if (ascii_lower((unsigned char)*left) !=
            ascii_lower((unsigned char)*right))
            return false;
        ++left;
        ++right;
    }
    return *left == '\0' && *right == '\0';
}

typedef struct {
    IconCategory category;
    const char *words[16];
} ClassificationRule;

static const ClassificationRule classification_rules[] = {
    {ICON_CATEGORY_APPLICATIONS,
     {"application-menu", "appfinder", "program group", NULL}},
    {ICON_CATEGORY_TASK_MANAGER,
     {"task manager", "task-manager", "taskmgr", "computer_taskmgr",
      "system monitor", "system-monitor", NULL}},
    {ICON_CATEGORY_TERMINAL,
     {"terminal", "xterm", "uxterm", "console", "konsole", "alacritty",
      "kitty", "rxvt", "tilix", "shell", NULL}},
    {ICON_CATEGORY_CLOCK, {"clock", "timepiece", NULL}},
    {ICON_CATEGORY_CALCULATOR,
     {"calculator", "xcalc", "kcalc", "galculator", "mate-calc",
      "libreoffice-calc", "gnome-calc", NULL}},
    {ICON_CATEGORY_GRAPHICS,
     {"graphics", "image-editor", "imageeditor", "paint", "gimp", "inkscape",
      "krita", "drawing", "image", NULL}},
    {ICON_CATEGORY_EDITOR,
     {"text-editor", "texteditor", "editor", "notepad", "wordpad", "gedit",
      "leafpad", "mousepad", "emacs", "gvim", "vim", "nano",
      "libreoffice-writer", "office", "document", NULL}},
    {ICON_CATEGORY_FILES,
     {"file-manager", "filemanager", "org.gnome.files", "nautilus", "thunar",
      "dolphin", "pcmanfm", "spacefm", "nemo", "explorer", "files", NULL}},
    {ICON_CATEGORY_MAIL,
     {"email", "e-mail", "mail", "thunderbird", "evolution", "kmail", NULL}},
    {ICON_CATEGORY_WEB,
     {"web-browser", "webbrowser", "browser", "firefox", "chromium", "chrome",
      "brave", "epiphany", "qutebrowser", "midori", "internet", NULL}},
    {ICON_CATEGORY_MEDIA,
     {"media-player", "mediaplayer", "multimedia", "audio", "video", "music",
      "player", "vlc", "mpv", "rhythmbox", "audacious", NULL}},
    {ICON_CATEGORY_NETWORK,
     {"network", "wireless", "wi-fi", "wifi", "ethernet", "nm-connection", NULL}},
    {ICON_CATEGORY_SETTINGS,
     {"settings", "preferences", "control-panel", "control_center",
      "control-center", "systemsettings", "configuration", NULL}},
    {ICON_CATEGORY_GAMES,
     {"game", "games", "steam", "lutris", "retroarch", "chess", "solitaire",
      "arcade", "joystick", NULL}},
    {ICON_CATEGORY_HELP,
     {"help", "documentation", "manual", "yelp", NULL}},
};

static IconCategory classify_one(const char *text)
{
    size_t rule_index;

    if (equals_ascii_case_insensitive(text, "applications") ||
        equals_ascii_case_insensitive(text, "programs"))
        return ICON_CATEGORY_APPLICATIONS;

    for (rule_index = 0;
         rule_index < sizeof(classification_rules) / sizeof(classification_rules[0]);
         ++rule_index) {
        size_t word_index;

        for (word_index = 0;
             word_index < sizeof(classification_rules[rule_index].words) /
                                  sizeof(classification_rules[rule_index].words[0]) &&
             classification_rules[rule_index].words[word_index] != NULL;
             ++word_index) {
            if (contains_ascii_case_insensitive(
                    text, classification_rules[rule_index].words[word_index]))
                return classification_rules[rule_index].category;
        }
    }
    return ICON_CATEGORY_COUNT;
}

IconCategory icon_assets_classify(const char *name, const char *icon,
                                  const char *exec_command)
{
    IconCategory category;

    category = classify_one(icon);
    if (category != ICON_CATEGORY_COUNT)
        return category;
    category = classify_one(name);
    if (category != ICON_CATEGORY_COUNT)
        return category;
    category = classify_one(exec_command);
    if (category != ICON_CATEGORY_COUNT)
        return category;
    return ICON_CATEGORY_EXECUTABLE;
}
