#define _DARWIN_C_SOURCE
#define _DEFAULT_SOURCE
#define _POSIX_C_SOURCE 200809L

#include "app_icons.h"

#include <dirent.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

static int failures;

#define CHECK(condition)                                                       \
    do {                                                                       \
        if (!(condition)) {                                                    \
            fprintf(stderr, "%s:%d: check failed: %s\n", __FILE__, __LINE__, \
                    #condition);                                               \
            ++failures;                                                        \
        }                                                                      \
    } while (0)

static int make_directory(const char *path)
{
    char copy[4096];
    char *cursor;

    if (snprintf(copy, sizeof(copy), "%s", path) >= (int)sizeof(copy))
        return -1;
    for (cursor = copy + 1; *cursor != '\0'; ++cursor) {
        if (*cursor != '/')
            continue;
        *cursor = '\0';
        if (mkdir(copy, 0700) < 0 && errno != EEXIST)
            return -1;
        *cursor = '/';
    }
    return mkdir(copy, 0700) == 0 || errno == EEXIST ? 0 : -1;
}

static int copy_file(const char *source, const char *destination)
{
    FILE *input = fopen(source, "rb");
    FILE *output;
    unsigned char buffer[8192];
    size_t count;
    int result = -1;

    if (input == NULL)
        return -1;
    output = fopen(destination, "wb");
    if (output == NULL) {
        fclose(input);
        return -1;
    }
    while ((count = fread(buffer, 1, sizeof(buffer), input)) > 0U) {
        if (fwrite(buffer, 1, count, output) != count)
            goto done;
    }
    if (ferror(input) || fflush(output) < 0)
        goto done;
    result = 0;

done:
    if (fclose(output) < 0)
        result = -1;
    fclose(input);
    return result;
}

static int write_xpm(const char *path)
{
    FILE *file = fopen(path, "w");

    if (file == NULL)
        return -1;
    if (fputs("/* XPM */\nstatic char *icon[] = {\n"
              "\"2 2 2 1\",\n\". c None\",\n\"X c #12ab34\",\n"
              "\"XX\",\n\"X.\"\n};\n",
              file) == EOF ||
        fclose(file) < 0)
        return -1;
    return 0;
}

static void remove_tree(const char *path)
{
    DIR *directory = opendir(path);
    struct dirent *entry;

    if (directory == NULL) {
        unlink(path);
        return;
    }
    while ((entry = readdir(directory)) != NULL) {
        char child[4096];

        if (strcmp(entry->d_name, ".") == 0 ||
            strcmp(entry->d_name, "..") == 0)
            continue;
        if (snprintf(child, sizeof(child), "%s/%s", path, entry->d_name) >=
            (int)sizeof(child))
            continue;
        remove_tree(child);
    }
    closedir(directory);
    rmdir(path);
}

static int expect_path(const char *name, unsigned int size,
                       const char *expected)
{
    char *resolved = NULL;
    int result = app_icon_resolve(name, size, &resolved);

    CHECK(result == 0);
    CHECK(resolved != NULL);
    if (resolved != NULL)
        CHECK(strcmp(resolved, expected) == 0);
    free(resolved);
    return result;
}

int main(void)
{
    char temporary[] = "/tmp/win31x-app-icons-XXXXXX";
    char user[4096];
    char system[4096];
    char home[4096];
    char user32[4096];
    char user48[4096];
    char user64[4096];
    char system48[4096];
    char pixmaps[4096];
    char path32[4096];
    char path48_png[4096];
    char path48_xpm[4096];
    char path64[4096];
    char system_path[4096];
    char pixmap_path[4096];
    char *resolved = NULL;

    if (mkdtemp(temporary) == NULL) {
        perror("mkdtemp");
        return 1;
    }
#define SET_PATH(destination, format, ...)                                     \
    CHECK(snprintf(destination, sizeof(destination), format, __VA_ARGS__) <    \
          (int)sizeof(destination))
    SET_PATH(user, "%s/user", temporary);
    SET_PATH(system, "%s/system", temporary);
    SET_PATH(home, "%s/home", temporary);
    SET_PATH(user32, "%s/icons/hicolor/32x32/apps", user);
    SET_PATH(user48, "%s/icons/hicolor/48x48/apps", user);
    SET_PATH(user64, "%s/icons/hicolor/64x64/apps", user);
    SET_PATH(system48, "%s/icons/hicolor/48x48/apps", system);
    SET_PATH(pixmaps, "%s/pixmaps", user);
    CHECK(make_directory(user32) == 0);
    CHECK(make_directory(user48) == 0);
    CHECK(make_directory(user64) == 0);
    CHECK(make_directory(system48) == 0);
    CHECK(make_directory(pixmaps) == 0);
    CHECK(make_directory(home) == 0);
    CHECK(setenv("HOME", home, 1) == 0);
    CHECK(setenv("XDG_DATA_HOME", user, 1) == 0);
    CHECK(setenv("XDG_DATA_DIRS", system, 1) == 0);

    SET_PATH(path32, "%s/demo.png", user32);
    SET_PATH(path48_png, "%s/demo.png", user48);
    SET_PATH(path48_xpm, "%s/demo.xpm", user48);
    SET_PATH(path64, "%s/demo.png", user64);
    SET_PATH(system_path, "%s/demo.png", system48);
    SET_PATH(pixmap_path, "%s/pix-only.xpm", pixmaps);
    CHECK(copy_file("assets/icons/settings-48.png", path32) == 0);
    CHECK(copy_file("assets/icons/settings-48.png", path48_png) == 0);
    CHECK(write_xpm(path48_xpm) == 0);
    CHECK(copy_file("assets/icons/settings-48.png", path64) == 0);
    CHECK(copy_file("assets/icons/settings-48.png", system_path) == 0);
    CHECK(write_xpm(pixmap_path) == 0);

    expect_path("demo", 48U, path48_png);
    CHECK(unlink(path48_png) == 0);
    expect_path("demo", 48U, path48_xpm);
    CHECK(unlink(path48_xpm) == 0);
    expect_path("demo", 48U, path64);
    CHECK(unlink(path64) == 0);
    /* A user-root raster remains ahead of an exact system-root raster. */
    expect_path("demo", 48U, path32);
    expect_path("pix-only", 48U, pixmap_path);
    expect_path(pixmap_path, 48U, pixmap_path);

    errno = 0;
    CHECK(app_icon_resolve("subdir/demo", 48U, &resolved) < 0);
    CHECK(resolved == NULL);
    CHECK(errno == EINVAL);
    errno = 0;
    CHECK(app_icon_resolve("demo.svg", 48U, &resolved) < 0);
    CHECK(resolved == NULL);
    CHECK(errno == ENOENT);
    errno = 0;
    CHECK(app_icon_resolve("missing", 48U, &resolved) < 0);
    CHECK(resolved == NULL);
    CHECK(errno == ENOENT);

    remove_tree(temporary);
    if (failures != 0) {
        fprintf(stderr, "%d application icon resolver test(s) failed\n",
                failures);
        return 1;
    }
    puts("application icon resolver tests passed");
    return 0;
}
