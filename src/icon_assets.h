#ifndef WIN31X_ICON_ASSETS_H
#define WIN31X_ICON_ASSETS_H

#include <stddef.h>

typedef enum {
    ICON_CATEGORY_APPLICATIONS,
    ICON_CATEGORY_EXECUTABLE,
    ICON_CATEGORY_TERMINAL,
    ICON_CATEGORY_CLOCK,
    ICON_CATEGORY_EDITOR,
    ICON_CATEGORY_CALCULATOR,
    ICON_CATEGORY_FILES,
    ICON_CATEGORY_WEB,
    ICON_CATEGORY_MAIL,
    ICON_CATEGORY_GRAPHICS,
    ICON_CATEGORY_MEDIA,
    ICON_CATEGORY_SETTINGS,
    ICON_CATEGORY_NETWORK,
    ICON_CATEGORY_GAMES,
    ICON_CATEGORY_HELP,
    ICON_CATEGORY_COUNT
} IconCategory;

typedef enum {
    ICON_SIZE_SMALL,
    ICON_SIZE_LARGE,
    ICON_SIZE_COUNT
} IconSize;

typedef struct {
    const char *filename;
    unsigned int width;
    unsigned int height;
} IconAssetDescriptor;

typedef struct {
    unsigned int width;
    unsigned int height;
    unsigned char *rgba;
} IconImage;

typedef struct {
    IconImage images[ICON_CATEGORY_COUNT][ICON_SIZE_COUNT];
    char *directory;
} IconAssets;

/*
 * Load the complete icon set. The IconAssets object must be zero-initialized
 * and must be released with icon_assets_free(). On failure, an informative
 * diagnostic is written to stderr and the object is left empty.
 *
 * Directories are searched in this order:
 *   WIN31X_ICON_DIR from the environment (an authoritative override),
 *   ./assets/icons,
 *   the WIN31X_ICON_DIR compile-time macro, when defined,
 *   /usr/local/share/win31x/icons, and /usr/share/win31x/icons.
 */
int icon_assets_load(IconAssets *assets);
void icon_assets_free(IconAssets *assets);

const IconImage *icon_assets_get(const IconAssets *assets,
                                 IconCategory category, IconSize size);
const IconAssetDescriptor *icon_assets_descriptor(IconCategory category,
                                                   IconSize size);
const char *icon_assets_directory(const IconAssets *assets);

/*
 * Choose a supplied Win98-style icon using freedesktop Name/Icon/Exec text.
 * Matching is locale-independent and ASCII case-insensitive. Unrecognized
 * applications deliberately use the supplied executable icon.
 */
IconCategory icon_assets_classify(const char *name, const char *icon,
                                  const char *exec_command);

#endif
