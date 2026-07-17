#define _POSIX_C_SOURCE 200809L

#include "icon_assets.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int failures;

#define CHECK(condition)                                                        \
    do {                                                                        \
        if (!(condition)) {                                                     \
            fprintf(stderr, "%s:%d: check failed: %s\n", __FILE__, __LINE__,  \
                    #condition);                                                \
            ++failures;                                                         \
        }                                                                       \
    } while (0)

static void test_loaded_assets(void)
{
    IconAssets assets = {0};
    int category;
    int size;

    CHECK(icon_assets_load(&assets) == 0);
    CHECK(icon_assets_directory(&assets) != NULL);
    for (category = 0; category < ICON_CATEGORY_COUNT; ++category) {
        for (size = 0; size < ICON_SIZE_COUNT; ++size) {
            const IconImage *image = icon_assets_get(
                &assets, (IconCategory)category, (IconSize)size);
            const IconAssetDescriptor *descriptor = icon_assets_descriptor(
                (IconCategory)category, (IconSize)size);

            CHECK(image != NULL);
            CHECK(descriptor != NULL);
            if (image != NULL && descriptor != NULL) {
                CHECK(image->rgba != NULL);
                CHECK(image->width == descriptor->width);
                CHECK(image->height == descriptor->height);
            }
        }
    }
    CHECK(icon_assets_get(&assets, ICON_CATEGORY_APPLICATIONS,
                          ICON_SIZE_LARGE)->width == 48);
    CHECK(icon_assets_get(&assets, ICON_CATEGORY_EDITOR,
                          ICON_SIZE_LARGE)->width == 48);
    CHECK(icon_assets_get(&assets, ICON_CATEGORY_TERMINAL,
                          ICON_SIZE_LARGE)->width == 32);
    CHECK(icon_assets_get(&assets, ICON_CATEGORY_SETTINGS,
                          ICON_SIZE_LARGE)->width == 48);
    CHECK(icon_assets_get(&assets, ICON_CATEGORY_NETWORK,
                          ICON_SIZE_LARGE)->width == 48);
    CHECK(icon_assets_get(&assets, ICON_CATEGORY_GRAPHICS,
                          ICON_SIZE_LARGE)->width == 48);
    CHECK(icon_assets_get(&assets, ICON_CATEGORY_CLOCK,
                          ICON_SIZE_LARGE)->width == 32);
    icon_assets_free(&assets);
    CHECK(icon_assets_directory(&assets) == NULL);
}

static void test_classification(void)
{
    CHECK(icon_assets_classify("Applications", NULL, NULL) ==
          ICON_CATEGORY_APPLICATIONS);
    CHECK(icon_assets_classify("XTerm", "utilities-terminal", "xterm") ==
          ICON_CATEGORY_TERMINAL);
    CHECK(icon_assets_classify("Clock", "xclock", "xclock") ==
          ICON_CATEGORY_CLOCK);
    CHECK(icon_assets_classify("Writer", "writer", "Utility;TextEditor;") ==
          ICON_CATEGORY_EDITOR);
    CHECK(icon_assets_classify("Firefox", "firefox", "firefox") ==
          ICON_CATEGORY_WEB);
    CHECK(icon_assets_classify("XCalc", "xcalc", "xcalc") ==
          ICON_CATEGORY_CALCULATOR);
    CHECK(icon_assets_classify("Vim", "vim", "vim") ==
          ICON_CATEGORY_EDITOR);
    CHECK(icon_assets_classify("Chess", "game", "gnome-chess") ==
          ICON_CATEGORY_GAMES);
    CHECK(icon_assets_classify("Thunar", "system-file-manager", "thunar") ==
          ICON_CATEGORY_FILES);
    CHECK(icon_assets_classify("Thunderbird", "email", "thunderbird") ==
          ICON_CATEGORY_MAIL);
    CHECK(icon_assets_classify("Paint", "image-editor", "paint") ==
          ICON_CATEGORY_GRAPHICS);
    CHECK(icon_assets_classify("VLC", "media-player", "vlc") ==
          ICON_CATEGORY_MEDIA);
    CHECK(icon_assets_classify("Network Connections", "network", "nm-connection-editor") ==
          ICON_CATEGORY_NETWORK);
    CHECK(icon_assets_classify("Control Panel", "preferences", "settings") ==
          ICON_CATEGORY_SETTINGS);
    CHECK(icon_assets_classify("Manual", "help", "yelp") ==
          ICON_CATEGORY_HELP);
    CHECK(icon_assets_classify("Unknown program", "unknown", "/bin/true") ==
          ICON_CATEGORY_EXECUTABLE);
}

int main(void)
{
    test_loaded_assets();
    test_classification();
    if (failures != 0) {
        fprintf(stderr, "%d icon asset test(s) failed\n", failures);
        return 1;
    }
    puts("supplied icon asset tests passed");
    return 0;
}
