#define _DARWIN_C_SOURCE
#define _DEFAULT_SOURCE
#define _POSIX_C_SOURCE 200809L

#include "settings.h"

#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

#define TEST_PATH_CAPACITY 4096U

static int failures;

#define CHECK(condition)                                                        \
    do {                                                                        \
        if (!(condition)) {                                                     \
            fprintf(stderr, "%s:%d: check failed: %s\n", __FILE__, __LINE__,  \
                    #condition);                                                \
            ++failures;                                                         \
        }                                                                       \
    } while (0)

static int path_join(char *destination, size_t capacity, const char *left,
                     const char *right)
{
    int written = snprintf(destination, capacity, "%s/%s", left, right);

    if (written < 0 || (size_t)written >= capacity) {
        errno = ENAMETOOLONG;
        return -1;
    }
    return 0;
}

static int write_text_file(const char *path, const char *contents)
{
    size_t length = strlen(contents);
    size_t written = 0U;
    int file_fd = open(path, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC,
                       (mode_t)0600);

    if (file_fd < 0)
        return -1;
    while (written < length) {
        ssize_t amount = write(file_fd, contents + written, length - written);

        if (amount > 0) {
            written += (size_t)amount;
            continue;
        }
        if (amount < 0 && errno == EINTR)
            continue;
        close(file_fd);
        return -1;
    }
    return close(file_fd);
}

static bool valid_hex_color(const char *color)
{
    size_t index;

    if (color == NULL || strlen(color) != 7U || color[0] != '#')
        return false;
    for (index = 1U; index < 7U; ++index) {
        if (isxdigit((unsigned char)color[index]) == 0)
            return false;
    }
    return true;
}

static void test_color_metadata(void)
{
    static const char *const expected_names[WIN31X_COLOR_SCHEME_COUNT] = {
        "Classic Teal", "Ocean Blue", "Forest", "Plum", "Slate"
    };
    size_t index;

    for (index = 0U; index < WIN31X_COLOR_SCHEME_COUNT; ++index) {
        const Win31xColorScheme *scheme = win31x_color_scheme(index);

        CHECK(scheme != NULL);
        if (scheme != NULL) {
            CHECK(strcmp(scheme->name, expected_names[index]) == 0);
            CHECK(scheme->id != NULL && scheme->id[0] != '\0');
            CHECK(valid_hex_color(scheme->desktop_hex));
            CHECK(valid_hex_color(scheme->active_title_hex));
        }
    }
    CHECK(win31x_color_scheme(WIN31X_COLOR_SCHEME_COUNT) == NULL);
}

static void test_defaults(void)
{
    Win31xSettings settings = {
        .color_scheme = 4U,
        .auto_lock_enabled = true,
        .auto_lock_minutes = 120U,
        .control_panel_section = WIN31X_CONTROL_PANEL_SECTION_AUTO_LOCK,
    };

    win31x_settings_defaults(&settings);
    CHECK(settings.color_scheme == 0U);
    CHECK(!settings.auto_lock_enabled);
    CHECK(settings.auto_lock_minutes == 10U);
    CHECK(settings.control_panel_section ==
          WIN31X_CONTROL_PANEL_SECTION_WIFI);
    win31x_settings_defaults(NULL);
}

static void test_round_trip(const char *root)
{
    char config_root[TEST_PATH_CAPACITY];
    char settings_directory[TEST_PATH_CAPACITY];
    char settings_path[TEST_PATH_CAPACITY];
    struct stat status;
    Win31xSettings saved = {
        .color_scheme = 3U,
        .auto_lock_enabled = true,
        .auto_lock_minutes = 42U,
        .control_panel_section = WIN31X_CONTROL_PANEL_SECTION_COLORS,
    };
    Win31xSettings loaded;

    CHECK(path_join(config_root, sizeof(config_root), root, "round-trip") == 0);
    CHECK(mkdir(config_root, (mode_t)0755) == 0);
    CHECK(setenv("XDG_CONFIG_HOME", config_root, 1) == 0);
    CHECK(win31x_settings_save(&saved) == 0);
    CHECK(path_join(settings_directory, sizeof(settings_directory), config_root,
                    "win31x") == 0);
    CHECK(path_join(settings_path, sizeof(settings_path), settings_directory,
                    "settings.conf") == 0);
    CHECK(stat(settings_directory, &status) == 0);
    CHECK((status.st_mode & (mode_t)0777) == (mode_t)0700);
    CHECK(stat(settings_path, &status) == 0);
    CHECK((status.st_mode & (mode_t)0777) == (mode_t)0600);
    CHECK(win31x_settings_load(&loaded) == 0);
    CHECK(loaded.color_scheme == saved.color_scheme);
    CHECK(loaded.auto_lock_enabled == saved.auto_lock_enabled);
    CHECK(loaded.auto_lock_minutes == saved.auto_lock_minutes);
    CHECK(loaded.control_panel_section == saved.control_panel_section);

    saved.control_panel_section = (Win31xControlPanelSection)999;
    CHECK(win31x_settings_save(&saved) == 0);
    CHECK(win31x_settings_load(&loaded) == 0);
    CHECK(loaded.control_panel_section ==
          WIN31X_CONTROL_PANEL_SECTION_WIFI);

    CHECK(unlink(settings_path) == 0);
    CHECK(rmdir(settings_directory) == 0);
    CHECK(rmdir(config_root) == 0);
}

static void test_malformed_and_clamping(const char *root)
{
    char config_root[TEST_PATH_CAPACITY];
    char settings_directory[TEST_PATH_CAPACITY];
    char settings_path[TEST_PATH_CAPACITY];
    Win31xSettings settings;

    CHECK(path_join(config_root, sizeof(config_root), root, "malformed") == 0);
    CHECK(mkdir(config_root, (mode_t)0700) == 0);
    CHECK(path_join(settings_directory, sizeof(settings_directory), config_root,
                    "win31x") == 0);
    CHECK(mkdir(settings_directory, (mode_t)0700) == 0);
    CHECK(path_join(settings_path, sizeof(settings_path), settings_directory,
                    "settings.conf") == 0);
    CHECK(setenv("XDG_CONFIG_HOME", config_root, 1) == 0);
    CHECK(write_text_file(
              settings_path,
              "unknown_key=ignored\n"
              "line without an equals sign\n"
              "color_scheme=Forest\n"
              "auto_lock_enabled=on\n"
              "auto_lock_minutes=0\n"
              "control_panel_section=auto-lock\n") == 0);
    CHECK(win31x_settings_load(&settings) == 0);
    CHECK(settings.color_scheme == 2U);
    CHECK(settings.auto_lock_enabled);
    CHECK(settings.auto_lock_minutes == WIN31X_AUTO_LOCK_MINUTES_MIN);
    CHECK(settings.control_panel_section ==
          WIN31X_CONTROL_PANEL_SECTION_AUTO_LOCK);

    CHECK(write_text_file(
              settings_path,
              "color_scheme=not-a-scheme\n"
              "auto_lock_enabled=perhaps\n"
              "auto_lock_minutes=999999999999999999999999999999999999\n"
              "control_panel_section=not-a-section\n") == 0);
    CHECK(win31x_settings_load(&settings) == 0);
    CHECK(settings.color_scheme == 0U);
    CHECK(!settings.auto_lock_enabled);
    CHECK(settings.auto_lock_minutes == WIN31X_AUTO_LOCK_MINUTES_MAX);
    CHECK(settings.control_panel_section ==
          WIN31X_CONTROL_PANEL_SECTION_WIFI);

    CHECK(write_text_file(settings_path, "control_panel_section=1\n") == 0);
    CHECK(win31x_settings_load(&settings) == 0);
    CHECK(settings.control_panel_section ==
          WIN31X_CONTROL_PANEL_SECTION_COLORS);

    CHECK(write_text_file(settings_path,
                          "color_scheme=ocean-blue\n"
                          "auto_lock_enabled=true\n"
                          "auto_lock_minutes=25\n") == 0);
    CHECK(win31x_settings_load(&settings) == 0);
    CHECK(settings.color_scheme == 1U);
    CHECK(settings.auto_lock_enabled);
    CHECK(settings.auto_lock_minutes == 25U);
    CHECK(settings.control_panel_section ==
          WIN31X_CONTROL_PANEL_SECTION_WIFI);

    CHECK(unlink(settings_path) == 0);
    CHECK(rmdir(settings_directory) == 0);
    CHECK(rmdir(config_root) == 0);
}

static void test_relative_xdg_is_ignored(const char *root)
{
    char home[TEST_PATH_CAPACITY];
    char config[TEST_PATH_CAPACITY];
    char settings_directory[TEST_PATH_CAPACITY];
    char settings_path[TEST_PATH_CAPACITY];
    Win31xSettings saved = {
        .color_scheme = 1U,
        .auto_lock_enabled = false,
        .auto_lock_minutes = 5U,
        .control_panel_section = WIN31X_CONTROL_PANEL_SECTION_AUTO_LOCK,
    };
    Win31xSettings loaded;

    CHECK(path_join(home, sizeof(home), root, "home") == 0);
    CHECK(mkdir(home, (mode_t)0700) == 0);
    CHECK(path_join(config, sizeof(config), home, ".config") == 0);
    CHECK(setenv("HOME", home, 1) == 0);
    CHECK(setenv("XDG_CONFIG_HOME", "relative-config", 1) == 0);
    CHECK(win31x_settings_save(&saved) == 0);
    CHECK(path_join(settings_directory, sizeof(settings_directory), config,
                    "win31x") == 0);
    CHECK(path_join(settings_path, sizeof(settings_path), settings_directory,
                    "settings.conf") == 0);
    CHECK(access(settings_path, F_OK) == 0);
    CHECK(win31x_settings_load(&loaded) == 0);
    CHECK(loaded.color_scheme == saved.color_scheme);
    CHECK(loaded.auto_lock_minutes == saved.auto_lock_minutes);
    CHECK(loaded.control_panel_section == saved.control_panel_section);

    CHECK(unlink(settings_path) == 0);
    CHECK(rmdir(settings_directory) == 0);
    CHECK(rmdir(config) == 0);
    CHECK(rmdir(home) == 0);
}

int main(void)
{
    char root_template[] = "/tmp/win31x-settings-test.XXXXXX";
    char *root = mkdtemp(root_template);

    if (root == NULL) {
        perror("mkdtemp");
        return 2;
    }
    test_color_metadata();
    test_defaults();
    test_round_trip(root);
    test_malformed_and_clamping(root);
    test_relative_xdg_is_ignored(root);
    CHECK(rmdir(root) == 0);

    if (failures != 0) {
        fprintf(stderr, "%d settings test(s) failed\n", failures);
        return 1;
    }
    puts("settings tests passed");
    return 0;
}
