#define _DARWIN_C_SOURCE
#define _DEFAULT_SOURCE
#define _POSIX_C_SOURCE 200809L

#include "desktop_state.h"

#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <stdbool.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

#define TEST_PATH_CAPACITY 4096U
#define TEST_CONTENT_CAPACITY 16384U

static int failures;

#define CHECK(condition)                                                       \
    do {                                                                       \
        if (!(condition)) {                                                    \
            fprintf(stderr, "%s:%d: check failed: %s\n", __FILE__, __LINE__, \
                    #condition);                                               \
            ++failures;                                                        \
        }                                                                      \
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

static int write_bytes(const char *path, const void *contents, size_t length)
{
    const unsigned char *bytes = contents;
    size_t written = 0U;
    int file_fd = open(path, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC,
                       (mode_t)0600);

    if (file_fd < 0)
        return -1;
    while (written < length) {
        ssize_t amount = write(file_fd, bytes + written, length - written);

        if (amount > 0) {
            written += (size_t)amount;
            continue;
        }
        if (amount < 0 && errno == EINTR)
            continue;
        (void)close(file_fd);
        return -1;
    }
    return close(file_fd);
}

static int write_text(const char *path, const char *contents)
{
    return write_bytes(path, contents, strlen(contents));
}

static int read_text(const char *path, char *contents, size_t capacity)
{
    size_t used = 0U;
    int file_fd;

    if (capacity == 0U) {
        errno = EINVAL;
        return -1;
    }
    file_fd = open(path, O_RDONLY | O_CLOEXEC);
    if (file_fd < 0)
        return -1;
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
        (void)close(file_fd);
        return -1;
    }
    if (used + 1U == capacity) {
        char extra_byte;
        ssize_t amount;

        do {
            amount = read(file_fd, &extra_byte, 1U);
        } while (amount < 0 && errno == EINTR);
        if (amount != 0) {
            int saved_errno = amount < 0 ? errno : EFBIG;

            (void)close(file_fd);
            errno = saved_errno;
            return -1;
        }
    }
    if (close(file_fd) < 0)
        return -1;
    contents[used] = '\0';
    return 0;
}

static int append_text(char *contents, size_t capacity, size_t *used,
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
    if (amount < 0 || (size_t)amount >= capacity - *used) {
        errno = EFBIG;
        return -1;
    }
    *used += (size_t)amount;
    return 0;
}

static int create_area(const char *root, const char *name, char *config_root,
                       char *state_directory, char *state_path)
{
    if (path_join(config_root, TEST_PATH_CAPACITY, root, name) < 0 ||
        mkdir(config_root, (mode_t)0755) < 0 ||
        path_join(state_directory, TEST_PATH_CAPACITY, config_root,
                  "win31x") < 0 ||
        mkdir(state_directory, (mode_t)0755) < 0 ||
        path_join(state_path, TEST_PATH_CAPACITY, state_directory,
                  "layout.conf") < 0 ||
        setenv("XDG_CONFIG_HOME", config_root, 1) < 0)
        return -1;
    return 0;
}

static bool placement_equal(const Win31xDesktopPlacement *left,
                            const Win31xDesktopPlacement *right)
{
    return left->valid == right->valid &&
           strcmp(left->monitor_name, right->monitor_name) == 0 &&
           left->monitor_center_x == right->monitor_center_x &&
           left->monitor_center_y == right->monitor_center_y &&
           left->relative_x == right->relative_x &&
           left->relative_y == right->relative_y &&
           left->width == right->width && left->height == right->height &&
           left->layout == right->layout &&
           left->layout_before_maximize == right->layout_before_maximize;
}

static Win31xDesktopPlacement placement(const char *monitor_name,
                                        int monitor_center_x,
                                        int monitor_center_y, int relative_x,
                                        int relative_y, int width, int height,
                                        Win31xDesktopLayout layout)
{
    Win31xDesktopPlacement result;
    int written;

    win31x_desktop_placement_defaults(&result);
    result.valid = true;
    written = snprintf(result.monitor_name, sizeof(result.monitor_name), "%s",
                       monitor_name);
    if (written < 0 || (size_t)written >= sizeof(result.monitor_name))
        result.monitor_name[0] = '\0';
    result.monitor_center_x = monitor_center_x;
    result.monitor_center_y = monitor_center_y;
    result.relative_x = relative_x;
    result.relative_y = relative_y;
    result.width = width;
    result.height = height;
    result.layout = layout;
    return result;
}

static void check_default_state(const Win31xDesktopState *state,
                                bool write_enabled)
{
    CHECK(!state->applications_icon.valid);
    CHECK(!state->control_panel_icon.valid);
    CHECK(!state->launcher.valid);
    CHECK(!state->control_panel.valid);
    CHECK(!state->run_dialog.valid);
    CHECK(state->applications_icon.layout ==
          WIN31X_DESKTOP_LAYOUT_NORMAL);
    CHECK(state->control_panel_icon.layout ==
          WIN31X_DESKTOP_LAYOUT_NORMAL);
    CHECK(state->launcher.layout == WIN31X_DESKTOP_LAYOUT_NORMAL);
    CHECK(state->control_panel.layout == WIN31X_DESKTOP_LAYOUT_NORMAL);
    CHECK(state->run_dialog.layout == WIN31X_DESKTOP_LAYOUT_NORMAL);
    CHECK(state->applications_icon.layout_before_maximize ==
          WIN31X_DESKTOP_LAYOUT_NORMAL);
    CHECK(state->control_panel_icon.layout_before_maximize ==
          WIN31X_DESKTOP_LAYOUT_NORMAL);
    CHECK(state->launcher.layout_before_maximize ==
          WIN31X_DESKTOP_LAYOUT_NORMAL);
    CHECK(state->control_panel.layout_before_maximize ==
          WIN31X_DESKTOP_LAYOUT_NORMAL);
    CHECK(state->run_dialog.layout_before_maximize ==
          WIN31X_DESKTOP_LAYOUT_NORMAL);
    CHECK(state->client_count == 0U);
    CHECK(state->write_enabled == write_enabled);
}

static void test_defaults_and_validation(void)
{
    Win31xDesktopPlacement item;
    Win31xDesktopState state;

    memset(&item, 0xa5, sizeof(item));
    win31x_desktop_placement_defaults(&item);
    CHECK(!item.valid);
    CHECK(item.monitor_name[0] == '\0');
    CHECK(item.monitor_center_x == 0);
    CHECK(item.monitor_center_y == 0);
    CHECK(item.relative_x == 0);
    CHECK(item.relative_y == 0);
    CHECK(item.width == 0);
    CHECK(item.height == 0);
    CHECK(item.layout == WIN31X_DESKTOP_LAYOUT_NORMAL);
    CHECK(item.layout_before_maximize == WIN31X_DESKTOP_LAYOUT_NORMAL);

    memset(&state, 0xa5, sizeof(state));
    win31x_desktop_state_defaults(&state);
    check_default_state(&state, true);
    win31x_desktop_placement_defaults(NULL);
    win31x_desktop_state_defaults(NULL);

    item = placement("DP-1", INT_MIN, INT_MAX, INT_MIN, INT_MAX,
                     WIN31X_DESKTOP_DIMENSION_MAX, 1,
                     (Win31xDesktopLayout)999);
    CHECK(win31x_desktop_placement_is_valid(&item));
    CHECK(!win31x_desktop_placement_is_valid(NULL));
    item.valid = false;
    CHECK(!win31x_desktop_placement_is_valid(&item));
    item.valid = true;
    item.width = 0;
    CHECK(!win31x_desktop_placement_is_valid(&item));
    item.width = -1;
    CHECK(!win31x_desktop_placement_is_valid(&item));
    item.width = WIN31X_DESKTOP_DIMENSION_MAX + 1;
    CHECK(!win31x_desktop_placement_is_valid(&item));
    item.width = 1;
    item.height = WIN31X_DESKTOP_DIMENSION_MAX + 1;
    CHECK(!win31x_desktop_placement_is_valid(&item));
    item.height = 1;
    memset(item.monitor_name, 'x', sizeof(item.monitor_name));
    CHECK(!win31x_desktop_placement_is_valid(&item));
}

static void test_round_trip_and_permissions(const char *root)
{
    char config_root[TEST_PATH_CAPACITY];
    char state_directory[TEST_PATH_CAPACITY];
    char state_path[TEST_PATH_CAPACITY];
    char serialized_contents[TEST_CONTENT_CAPACITY];
    struct stat status;
    Win31xDesktopState saved;
    Win31xDesktopState loaded;
    const Win31xDesktopClientRecord *client;
    static const char first_identity[] =
        "org.example.Terminal\ninstance #1";
    static const char second_identity[] = "xterm instance\tsecondary";

    CHECK(create_area(root, "round-trip", config_root, state_directory,
                      state_path) == 0);
    win31x_desktop_state_defaults(&saved);
    saved.applications_icon = placement("DP-1 east\twing", INT_MIN, INT_MAX,
                                        -71, 93, 48, 72,
                                        WIN31X_DESKTOP_LAYOUT_NORMAL);
    saved.control_panel_icon = placement("", -960, 540, INT_MIN, INT_MAX,
                                         63, 81,
                                         WIN31X_DESKTOP_LAYOUT_NORMAL);
    saved.launcher = placement("HDMI-A-1", -100, -200, -300, -400, 707,
                               509, WIN31X_DESKTOP_LAYOUT_MAXIMIZED);
    saved.launcher.layout_before_maximize =
        WIN31X_DESKTOP_LAYOUT_SNAPPED_LEFT;
    saved.control_panel = placement("Virtual-1", 1500, -800, 41, -51, 643,
                                    487,
                                    WIN31X_DESKTOP_LAYOUT_SNAPPED_RIGHT);
    saved.control_panel.layout_before_maximize =
        WIN31X_DESKTOP_LAYOUT_SNAPPED_RIGHT;
    saved.run_dialog = placement("DP-3", 2000, 600, -155, 77, 420, 180,
                                 WIN31X_DESKTOP_LAYOUT_NORMAL);
    CHECK(win31x_desktop_state_upsert_client(
              &saved, first_identity,
              &(Win31xDesktopPlacement){
                  .valid = true,
                  .monitor_name = "monitor name with spaces",
                  .monitor_center_x = INT_MAX,
                  .monitor_center_y = INT_MIN,
                  .relative_x = -12345,
                  .relative_y = 23456,
                  .width = WIN31X_DESKTOP_DIMENSION_MAX,
                  .height = 1,
                  .layout = WIN31X_DESKTOP_LAYOUT_SNAPPED_LEFT,
              }) == 0);
    CHECK(win31x_desktop_state_upsert_client(
              &saved, second_identity,
              &(Win31xDesktopPlacement){
                  .valid = true,
                  .monitor_name = "eDP-1",
                  .monitor_center_x = 640,
                  .monitor_center_y = 400,
                  .relative_x = 12,
                  .relative_y = 34,
                  .width = 800,
                  .height = 600,
                  .layout = WIN31X_DESKTOP_LAYOUT_MAXIMIZED,
                  .layout_before_maximize =
                      WIN31X_DESKTOP_LAYOUT_SNAPPED_RIGHT,
              }) == 0);

    CHECK(win31x_desktop_state_save(&saved) == 0);
    CHECK(read_text(state_path, serialized_contents,
                    sizeof(serialized_contents)) == 0);
    CHECK(strstr(serialized_contents, "\nversion 2\n") != NULL);
    CHECK(strstr(serialized_contents, "\nversion 1\n") == NULL);
    CHECK(stat(state_directory, &status) == 0);
    CHECK((status.st_mode & (mode_t)0777) == (mode_t)0700);
    CHECK(stat(state_path, &status) == 0);
    CHECK(S_ISREG(status.st_mode));
    CHECK((status.st_mode & (mode_t)0777) == (mode_t)0600);
    CHECK(win31x_desktop_state_load(&loaded) == 0);
    CHECK(placement_equal(&loaded.applications_icon,
                          &saved.applications_icon));
    CHECK(placement_equal(&loaded.control_panel_icon,
                          &saved.control_panel_icon));
    CHECK(placement_equal(&loaded.launcher, &saved.launcher));
    CHECK(placement_equal(&loaded.control_panel, &saved.control_panel));
    CHECK(placement_equal(&loaded.run_dialog, &saved.run_dialog));
    CHECK(loaded.write_enabled);
    CHECK(loaded.client_count == 2U);
    client = win31x_desktop_state_find_client(&loaded, first_identity);
    CHECK(client != NULL);
    if (client != NULL) {
        CHECK(strcmp(client->identity, first_identity) == 0);
        CHECK(placement_equal(&client->placement,
                              &saved.clients[0].placement));
    }
    client = win31x_desktop_state_find_client(&loaded, second_identity);
    CHECK(client != NULL);
    if (client != NULL)
        CHECK(placement_equal(&client->placement,
                              &saved.clients[1].placement));

    loaded.run_dialog.layout_before_maximize =
        (Win31xDesktopLayout)999;
    CHECK(win31x_desktop_state_save(&loaded) == 0);
    CHECK(win31x_desktop_state_load(&loaded) == 0);
    CHECK(loaded.run_dialog.layout_before_maximize ==
          WIN31X_DESKTOP_LAYOUT_NORMAL);
    CHECK(stat(state_path, &status) == 0);
    CHECK((status.st_mode & (mode_t)0777) == (mode_t)0600);
    CHECK(unlink(state_path) == 0);
    CHECK(rmdir(state_directory) == 0);
    CHECK(rmdir(config_root) == 0);
}

static void test_missing_legacy_layout(const char *root)
{
    char config_root[TEST_PATH_CAPACITY];
    char state_directory[TEST_PATH_CAPACITY];
    char state_path[TEST_PATH_CAPACITY];
    char settings_path[TEST_PATH_CAPACITY];
    Win31xDesktopState state;

    CHECK(create_area(root, "legacy", config_root, state_directory,
                      state_path) == 0);
    CHECK(path_join(settings_path, sizeof(settings_path), state_directory,
                    "settings.conf") == 0);
    CHECK(write_text(settings_path, "color_scheme=classic-teal\n") == 0);
    memset(&state, 0xa5, sizeof(state));
    CHECK(win31x_desktop_state_load(&state) == 0);
    check_default_state(&state, true);
    CHECK(access(settings_path, F_OK) == 0);

    CHECK(unlink(settings_path) == 0);
    CHECK(rmdir(state_directory) == 0);
    CHECK(rmdir(config_root) == 0);
}

static void test_malformed_independent_records_and_version(const char *root)
{
    char config_root[TEST_PATH_CAPACITY];
    char state_directory[TEST_PATH_CAPACITY];
    char state_path[TEST_PATH_CAPACITY];
    char disk_contents[TEST_CONTENT_CAPACITY];
    Win31xDesktopState state;
    const Win31xDesktopClientRecord *client;
    static const char future_contents[] =
        "version 3\n"
        "applications_icon 1 - 0 0 1 2 48 48 0\n"
        "future_only_record do-not-discard-this\n";
    static const char supported_contents[] =
        "# records before or after version are accepted\n"
        "applications_icon 1 44502d31 -10 20 -30 40 48 64 0\n"
        "control_panel_icon 1 zz 0 0 0 0 48 64 0\n"
        "launcher 1 - 2147483647 -2147483648 -50 60 700 500 3\n"
        "control_panel 1 4350 0 0 0 0 640 480 1 unexpected\n"
        "run_dialog 1 44502d34 100 200 -25 35 404 166 0 3\n"
        "client 636c69656e742d6f6e65 1 48444d492d31 -1 2 -3 4 640 480 2\n"
        "client not-hex 1 - 0 0 0 0 20 20 0\n"
        "client 6261642d64696d73 1 - 0 0 0 0 0 20 0\n"
        "unknown_record anything is ignored\n"
        "version 1\n"
        "client 636c69656e742d6f6e65 1 44502d32 5 6 7 8 900 700 3 2\n";

    CHECK(create_area(root, "malformed", config_root, state_directory,
                      state_path) == 0);
    CHECK(write_text(state_path, supported_contents) == 0);
    CHECK(win31x_desktop_state_load(&state) == 0);
    CHECK(state.applications_icon.valid);
    CHECK(strcmp(state.applications_icon.monitor_name, "DP-1") == 0);
    CHECK(state.applications_icon.monitor_center_x == -10);
    CHECK(state.applications_icon.relative_x == -30);
    CHECK(!state.control_panel_icon.valid);
    CHECK(state.launcher.valid);
    CHECK(state.launcher.monitor_name[0] == '\0');
    CHECK(state.launcher.monitor_center_x == INT_MAX);
    CHECK(state.launcher.monitor_center_y == INT_MIN);
    CHECK(state.launcher.layout ==
          WIN31X_DESKTOP_LAYOUT_SNAPPED_RIGHT);
    /* Version-1 records written before PREV was added remain compatible. */
    CHECK(state.launcher.layout_before_maximize ==
          WIN31X_DESKTOP_LAYOUT_NORMAL);
    CHECK(!state.control_panel.valid);
    CHECK(state.run_dialog.valid);
    CHECK(strcmp(state.run_dialog.monitor_name, "DP-4") == 0);
    CHECK(state.run_dialog.relative_x == -25);
    CHECK(state.run_dialog.width == 404);
    CHECK(state.run_dialog.layout_before_maximize ==
          WIN31X_DESKTOP_LAYOUT_SNAPPED_RIGHT);
    CHECK(state.write_enabled);
    CHECK(state.client_count == 1U);
    client = win31x_desktop_state_find_client(&state, "client-one");
    CHECK(client != NULL);
    if (client != NULL) {
        CHECK(strcmp(client->placement.monitor_name, "DP-2") == 0);
        CHECK(client->placement.relative_x == 7);
        CHECK(client->placement.width == 900);
        CHECK(client->placement.layout ==
              WIN31X_DESKTOP_LAYOUT_SNAPPED_RIGHT);
        CHECK(client->placement.layout_before_maximize ==
              WIN31X_DESKTOP_LAYOUT_SNAPPED_LEFT);
    }
    CHECK(win31x_desktop_state_save(&state) == 0);
    CHECK(read_text(state_path, disk_contents, sizeof(disk_contents)) == 0);
    CHECK(strstr(disk_contents, "\nversion 2\n") != NULL);
    CHECK(strstr(disk_contents, "\nversion 1\n") == NULL);

    CHECK(write_text(state_path,
                     "applications_icon 1 - 0 0 1 2 48 48 0\n") == 0);
    CHECK(win31x_desktop_state_load(&state) == 0);
    check_default_state(&state, true);

    CHECK(write_text(state_path, future_contents) == 0);
    CHECK(win31x_desktop_state_load(&state) == 0);
    check_default_state(&state, false);
    errno = 0;
    CHECK(win31x_desktop_state_save(&state) < 0);
    CHECK(errno == ENOTSUP);
    CHECK(read_text(state_path, disk_contents, sizeof(disk_contents)) == 0);
    CHECK(strcmp(disk_contents, future_contents) == 0);

    CHECK(write_text(state_path,
                     "version 999999999999999999999999999999999999\n"
                     "future_only_record still-preserved\n") == 0);
    CHECK(win31x_desktop_state_load(&state) == 0);
    check_default_state(&state, false);

    CHECK(write_text(state_path,
                     "version 1\n"
                     "version 1\n"
                     "applications_icon 1 - 0 0 1 2 48 48 0\n") == 0);
    CHECK(win31x_desktop_state_load(&state) == 0);
    check_default_state(&state, true);

    CHECK(write_text(state_path,
                     "version nope\n"
                     "launcher 1 - 0 0 1 2 300 200 0\n") == 0);
    CHECK(win31x_desktop_state_load(&state) == 0);
    check_default_state(&state, true);

    CHECK(unlink(state_path) == 0);
    CHECK(rmdir(state_directory) == 0);
    CHECK(rmdir(config_root) == 0);
}

static void fill_hex_bytes(char *destination, size_t decoded_length)
{
    size_t index;

    for (index = 0U; index < decoded_length; ++index) {
        destination[index * 2U] = '4';
        destination[index * 2U + 1U] = '1';
    }
    destination[decoded_length * 2U] = '\0';
}

static void test_overflow_dimensions_and_encoded_lengths(const char *root)
{
    char config_root[TEST_PATH_CAPACITY];
    char state_directory[TEST_PATH_CAPACITY];
    char state_path[TEST_PATH_CAPACITY];
    char contents[TEST_CONTENT_CAPACITY];
    char overlong_identity[WIN31X_DESKTOP_ID_CAPACITY * 2U + 1U];
    char overlong_monitor[WIN31X_DESKTOP_MONITOR_CAPACITY * 2U + 1U];
    size_t used = 0U;
    Win31xDesktopState state;
    const Win31xDesktopClientRecord *client;

    CHECK(create_area(root, "overflow", config_root, state_directory,
                      state_path) == 0);
    fill_hex_bytes(overlong_identity, WIN31X_DESKTOP_ID_CAPACITY);
    fill_hex_bytes(overlong_monitor, WIN31X_DESKTOP_MONITOR_CAPACITY);
    CHECK(append_text(contents, sizeof(contents), &used,
                      "version 1\n"
                      "applications_icon 1 - 0 0 -1 -2 48 48 999 999\n"
                      "control_panel_icon 1 - 0 0 0 0 "
                      "999999999999999999999999999999999 48 0\n"
                      "launcher 1 - 999999999999999999999999 0 0 0 "
                      "300 200 0\n"
                      "control_panel 1 - 0 0 0 0 0 480 0\n"
                      "run_dialog 1 - 0 0 1 2 400 160 0 nope\n") == 0);
    CHECK(append_text(contents, sizeof(contents), &used,
                      "client %s 1 - 0 0 1 2 640 480 0\n",
                      overlong_identity) == 0);
    CHECK(append_text(contents, sizeof(contents), &used,
                      "client 6f7665726c6f6e672d6d6f6e69746f72 1 %s "
                      "0 0 1 2 640 480 0\n",
                      overlong_monitor) == 0);
    CHECK(append_text(contents, sizeof(contents), &used,
                      "client 746f6f2d77696465 1 - 0 0 1 2 65536 480 0\n"
                      "client 6261642d63656e746572 1 - "
                      "-999999999999999999999 0 1 2 640 480 0\n"
                      "client 6261642d76616c6964 2 - 0 0 1 2 640 480 0\n"
                      "client 676f6f64 1 - -2147483648 2147483647 "
                      "-2147483648 2147483647 65535 1 1 1\n") == 0);
    CHECK(write_bytes(state_path, contents, used) == 0);
    CHECK(win31x_desktop_state_load(&state) == 0);
    CHECK(state.applications_icon.valid);
    CHECK(state.applications_icon.layout ==
          WIN31X_DESKTOP_LAYOUT_NORMAL);
    CHECK(state.applications_icon.layout_before_maximize ==
          WIN31X_DESKTOP_LAYOUT_NORMAL);
    CHECK(!state.control_panel_icon.valid);
    CHECK(!state.launcher.valid);
    CHECK(!state.control_panel.valid);
    CHECK(!state.run_dialog.valid);
    CHECK(state.client_count == 1U);
    client = win31x_desktop_state_find_client(&state, "good");
    CHECK(client != NULL);
    if (client != NULL) {
        CHECK(client->placement.monitor_center_x == INT_MIN);
        CHECK(client->placement.monitor_center_y == INT_MAX);
        CHECK(client->placement.relative_x == INT_MIN);
        CHECK(client->placement.relative_y == INT_MAX);
        CHECK(client->placement.width == WIN31X_DESKTOP_DIMENSION_MAX);
        CHECK(client->placement.height == 1);
        CHECK(client->placement.layout ==
              WIN31X_DESKTOP_LAYOUT_MAXIMIZED);
        CHECK(client->placement.layout_before_maximize ==
              WIN31X_DESKTOP_LAYOUT_NORMAL);
    }

    CHECK(unlink(state_path) == 0);
    CHECK(rmdir(state_directory) == 0);
    CHECK(rmdir(config_root) == 0);
}

static void test_client_upsert_replace_and_capacity(void)
{
    Win31xDesktopState state;
    Win31xDesktopPlacement first =
        placement("DP-1", 100, 200, 10, 20, 300, 400,
                  WIN31X_DESKTOP_LAYOUT_SNAPPED_LEFT);
    Win31xDesktopPlacement replacement =
        placement("DP-2", -100, -200, -10, -20, 500, 600,
                  (Win31xDesktopLayout)999);
    Win31xDesktopPlacement invalid = first;
    Win31xDesktopClientRecord *mutable_client;
    const Win31xDesktopClientRecord *client;
    char identity[64];
    char unterminated_identity[WIN31X_DESKTOP_ID_CAPACITY];
    size_t index;

    first.layout_before_maximize = WIN31X_DESKTOP_LAYOUT_SNAPPED_LEFT;
    replacement.layout_before_maximize =
        WIN31X_DESKTOP_LAYOUT_MAXIMIZED;
    win31x_desktop_state_defaults(&state);
    errno = 0;
    CHECK(win31x_desktop_state_upsert_client(NULL, "client", &first) < 0);
    CHECK(errno == EINVAL);
    errno = 0;
    CHECK(win31x_desktop_state_upsert_client(&state, NULL, &first) < 0);
    CHECK(errno == EINVAL);
    errno = 0;
    CHECK(win31x_desktop_state_upsert_client(&state, "", &first) < 0);
    CHECK(errno == EINVAL);
    invalid.width = 0;
    errno = 0;
    CHECK(win31x_desktop_state_upsert_client(&state, "client", &invalid) < 0);
    CHECK(errno == EINVAL);
    memset(unterminated_identity, 'i', sizeof(unterminated_identity));
    errno = 0;
    CHECK(win31x_desktop_state_upsert_client(
              &state, unterminated_identity, &first) < 0);
    CHECK(errno == EINVAL);

    CHECK(win31x_desktop_state_upsert_client(&state, "client", &first) == 0);
    CHECK(state.client_count == 1U);
    CHECK(state.clients[0].placement.layout_before_maximize ==
          WIN31X_DESKTOP_LAYOUT_SNAPPED_LEFT);
    CHECK(win31x_desktop_state_upsert_client(&state, "client",
                                             &replacement) == 0);
    CHECK(state.client_count == 1U);
    client = win31x_desktop_state_find_client(&state, "client");
    CHECK(client != NULL);
    if (client != NULL) {
        CHECK(strcmp(client->placement.monitor_name, "DP-2") == 0);
        CHECK(client->placement.width == 500);
        CHECK(client->placement.layout == WIN31X_DESKTOP_LAYOUT_NORMAL);
        CHECK(client->placement.layout_before_maximize ==
              WIN31X_DESKTOP_LAYOUT_NORMAL);
    }
    mutable_client = win31x_desktop_state_find_client_mutable(&state,
                                                               "client");
    CHECK(mutable_client != NULL);
    if (mutable_client != NULL)
        mutable_client->placement.relative_x = 777;
    client = win31x_desktop_state_find_client(&state, "client");
    CHECK(client != NULL && client->placement.relative_x == 777);
    CHECK(win31x_desktop_state_find_client(&state, "missing") == NULL);
    CHECK(win31x_desktop_state_find_client(NULL, "client") == NULL);
    CHECK(win31x_desktop_state_find_client(&state, "") == NULL);

    for (index = 1U; index < WIN31X_DESKTOP_CLIENT_MAX; ++index) {
        int amount = snprintf(identity, sizeof(identity), "client-%03zu",
                              index);

        CHECK(amount > 0 && (size_t)amount < sizeof(identity));
        CHECK(win31x_desktop_state_upsert_client(&state, identity, &first) ==
              0);
    }
    CHECK(state.client_count == WIN31X_DESKTOP_CLIENT_MAX);
    CHECK(strcmp(state.clients[0].identity, "client") == 0);
    CHECK(strcmp(state.clients[WIN31X_DESKTOP_CLIENT_MAX - 1U].identity,
                 "client-127") == 0);

    /* Refreshing an existing record makes it newest without growing. */
    CHECK(win31x_desktop_state_upsert_client(&state, "client",
                                             &replacement) == 0);
    CHECK(state.client_count == WIN31X_DESKTOP_CLIENT_MAX);
    CHECK(strcmp(state.clients[0].identity, "client-001") == 0);
    CHECK(strcmp(state.clients[WIN31X_DESKTOP_CLIENT_MAX - 1U].identity,
                 "client") == 0);

    /* A new record deterministically evicts the oldest record. */
    CHECK(win31x_desktop_state_upsert_client(&state, "one-too-many",
                                             &first) == 0);
    CHECK(state.client_count == WIN31X_DESKTOP_CLIENT_MAX);
    CHECK(win31x_desktop_state_find_client(&state, "client-001") == NULL);
    CHECK(win31x_desktop_state_find_client(&state, "client") != NULL);
    CHECK(strcmp(state.clients[0].identity, "client-002") == 0);
    CHECK(strcmp(state.clients[WIN31X_DESKTOP_CLIENT_MAX - 1U].identity,
                 "one-too-many") == 0);

    /* Repeated refreshes also maintain deterministic on-disk ordering. */
    replacement.layout_before_maximize = (Win31xDesktopLayout)999;
    CHECK(win31x_desktop_state_upsert_client(&state, "client",
                                             &replacement) == 0);
    CHECK(state.client_count == WIN31X_DESKTOP_CLIENT_MAX);
    CHECK(strcmp(state.clients[WIN31X_DESKTOP_CLIENT_MAX - 2U].identity,
                 "one-too-many") == 0);
    CHECK(strcmp(state.clients[WIN31X_DESKTOP_CLIENT_MAX - 1U].identity,
                 "client") == 0);
    CHECK(state.clients[WIN31X_DESKTOP_CLIENT_MAX - 1U]
              .placement.layout_before_maximize ==
          WIN31X_DESKTOP_LAYOUT_NORMAL);
}

static void test_symlink_rejection(const char *root)
{
    char config_root[TEST_PATH_CAPACITY];
    char state_directory[TEST_PATH_CAPACITY];
    char state_path[TEST_PATH_CAPACITY];
    char target_path[TEST_PATH_CAPACITY];
    struct stat status;
    Win31xDesktopState state;
    int saved_errno;

    CHECK(create_area(root, "symlink", config_root, state_directory,
                      state_path) == 0);
    CHECK(path_join(target_path, sizeof(target_path), config_root,
                    "target.txt") == 0);
    CHECK(write_text(target_path, "sentinel") == 0);
    CHECK(symlink(target_path, state_path) == 0);
    win31x_desktop_state_defaults(&state);

    errno = 0;
    CHECK(win31x_desktop_state_save(&state) < 0);
    CHECK(errno == ELOOP);
    CHECK(lstat(state_path, &status) == 0);
    CHECK(S_ISLNK(status.st_mode));
    CHECK(stat(target_path, &status) == 0);
    CHECK(status.st_size == (off_t)strlen("sentinel"));

    errno = 0;
    CHECK(win31x_desktop_state_load(&state) < 0);
    saved_errno = errno;
    CHECK(saved_errno == ELOOP || saved_errno == ENOTDIR ||
          saved_errno == EMLINK);

    CHECK(unlink(state_path) == 0);
    CHECK(unlink(target_path) == 0);
    CHECK(rmdir(state_directory) == 0);
    CHECK(rmdir(config_root) == 0);
}

static void test_relative_xdg_falls_back_to_home(const char *root)
{
    char home[TEST_PATH_CAPACITY];
    char config_directory[TEST_PATH_CAPACITY];
    char state_directory[TEST_PATH_CAPACITY];
    char state_path[TEST_PATH_CAPACITY];
    Win31xDesktopState saved;
    Win31xDesktopState loaded;

    CHECK(path_join(home, sizeof(home), root, "home") == 0);
    CHECK(mkdir(home, (mode_t)0700) == 0);
    CHECK(path_join(config_directory, sizeof(config_directory), home,
                    ".config") == 0);
    CHECK(path_join(state_directory, sizeof(state_directory),
                    config_directory, "win31x") == 0);
    CHECK(path_join(state_path, sizeof(state_path), state_directory,
                    "layout.conf") == 0);
    CHECK(setenv("HOME", home, 1) == 0);
    CHECK(setenv("XDG_CONFIG_HOME", "relative-config-must-be-ignored", 1) ==
          0);

    win31x_desktop_state_defaults(&saved);
    saved.launcher = placement("HOME-MONITOR", 500, 400, -12, 23, 640, 480,
                               WIN31X_DESKTOP_LAYOUT_NORMAL);
    CHECK(win31x_desktop_state_save(&saved) == 0);
    CHECK(access(state_path, F_OK) == 0);
    CHECK(win31x_desktop_state_load(&loaded) == 0);
    CHECK(placement_equal(&loaded.launcher, &saved.launcher));

    CHECK(unlink(state_path) == 0);
    CHECK(rmdir(state_directory) == 0);
    CHECK(rmdir(config_directory) == 0);
    CHECK(rmdir(home) == 0);
}

int main(void)
{
    char root_template[] = "/tmp/win31x-desktop-state-test.XXXXXX";
    char *root = mkdtemp(root_template);

    if (root == NULL) {
        perror("mkdtemp");
        return 2;
    }
    test_defaults_and_validation();
    test_round_trip_and_permissions(root);
    test_missing_legacy_layout(root);
    test_malformed_independent_records_and_version(root);
    test_overflow_dimensions_and_encoded_lengths(root);
    test_client_upsert_replace_and_capacity();
    test_symlink_rejection(root);
    test_relative_xdg_falls_back_to_home(root);
    CHECK(rmdir(root) == 0);

    if (failures != 0) {
        fprintf(stderr, "%d desktop-state test(s) failed\n", failures);
        return 1;
    }
    puts("desktop-state tests passed");
    return 0;
}
