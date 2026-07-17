#define _POSIX_C_SOURCE 200809L

#include "icon_assets.h"

#include <png.h>

#include <errno.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#ifndef WIN31X_ICON_DIR
#define WIN31X_ICON_DIR ""
#endif

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

static int load_png(const char *path, const IconAssetDescriptor *descriptor,
                    IconImage *image)
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
        fprintf(stderr, "win31x: cannot allocate PNG loader for %s\n", path);
        return -1;
    }
    state->file = fopen(path, "rb");
    if (state->file == NULL) {
        fprintf(stderr, "win31x: required icon asset %s cannot be opened: %s\n",
                path, strerror(errno));
        png_state_destroy(state);
        return -1;
    }
    if (fread(signature, 1, sizeof(signature), state->file) != sizeof(signature) ||
        png_sig_cmp(signature, 0, sizeof(signature)) != 0) {
        fprintf(stderr, "win31x: required icon asset %s is not a valid PNG\n", path);
        errno = EINVAL;
        png_state_destroy(state);
        return -1;
    }
    state->png = png_create_read_struct(PNG_LIBPNG_VER_STRING, state,
                                        png_error_handler, png_warning_handler);
    if (state->png == NULL) {
        fprintf(stderr, "win31x: libpng initialization failed for %s\n", path);
        errno = ENOMEM;
        png_state_destroy(state);
        return -1;
    }
    state->info = png_create_info_struct(state->png);
    if (state->info == NULL) {
        fprintf(stderr, "win31x: libpng metadata allocation failed for %s\n", path);
        errno = ENOMEM;
        png_state_destroy(state);
        return -1;
    }
    if (setjmp(png_jmpbuf(state->png)) != 0) {
        fprintf(stderr, "win31x: invalid icon asset %s: %s\n", path,
                state->png_error[0] != '\0' ? state->png_error : "PNG decoding failed");
        errno = EINVAL;
        png_state_destroy(state);
        return -1;
    }

    png_init_io(state->png, state->file);
    png_set_sig_bytes(state->png, sizeof(signature));
    png_read_info(state->png, state->info);
    png_get_IHDR(state->png, state->info, &width, &height, &bit_depth,
                 &color_type, &interlace_type, NULL, NULL);
    if (width != descriptor->width || height != descriptor->height) {
        fprintf(stderr,
                "win31x: icon asset %s has size %ux%u; expected %ux%u\n",
                path, (unsigned)width, (unsigned)height, descriptor->width,
                descriptor->height);
        errno = EINVAL;
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
        fprintf(stderr, "win31x: icon asset %s has an unsupported pixel layout\n",
                path);
        errno = EINVAL;
        png_state_destroy(state);
        return -1;
    }
    pixel_bytes = (size_t)row_bytes * height;
    state->pixels = malloc(pixel_bytes);
    state->rows = malloc((size_t)height * sizeof(*state->rows));
    if (state->pixels == NULL || state->rows == NULL) {
        fprintf(stderr, "win31x: cannot allocate pixels for icon asset %s\n", path);
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
            if (load_png(path, descriptor, &loaded.images[category][size]) < 0) {
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
