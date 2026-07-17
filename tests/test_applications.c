#define _POSIX_C_SOURCE 200809L

#include "applications.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

static int failures;

#define CHECK(condition)                                                        \
    do {                                                                        \
        if (!(condition)) {                                                     \
            fprintf(stderr, "%s:%d: check failed: %s\n", __FILE__, __LINE__,  \
                    #condition);                                                \
            ++failures;                                                        \
        }                                                                       \
    } while (0)

static char *write_desktop_file(const char *contents)
{
    char template[] = "/tmp/win31x-app-test-XXXXXX";
    int descriptor = mkstemp(template);
    FILE *file;

    if (descriptor < 0) {
        perror("mkstemp");
        exit(2);
    }
    file = fdopen(descriptor, "w");
    if (file == NULL) {
        perror("fdopen");
        close(descriptor);
        exit(2);
    }
    if (fputs(contents, file) == EOF || fclose(file) != 0) {
        perror("write desktop entry");
        exit(2);
    }
    return strdup(template);
}

static int make_temporary_directory(char *path_template)
{
    int descriptor = mkstemp(path_template);

    if (descriptor < 0)
        return -1;
    if (close(descriptor) < 0) {
        int saved_errno = errno;

        unlink(path_template);
        errno = saved_errno;
        return -1;
    }
    if (unlink(path_template) < 0)
        return -1;
    return mkdir(path_template, 0700);
}

static void test_valid_entry(void)
{
    char *path = write_desktop_file(
        "[Desktop Entry]\n"
        "Type=Application\n"
        "Name=Text Editor\n"
        "Exec=my-editor --new-window %F\n"
        "Icon=editor\n"
        "Categories=Utility;TextEditor;\n"
        "Path=/tmp\n"
        "Terminal=false\n"
        "\n"
        "[Desktop Action New]\n"
        "Name=Ignored Action\n");
    AppEntry entry;

    CHECK(app_parse_desktop_file(path, &entry) == 0);
    CHECK(strcmp(entry.name, "Text Editor") == 0);
    CHECK(strcmp(entry.exec, "my-editor --new-window %F") == 0);
    CHECK(strcmp(entry.icon, "editor") == 0);
    CHECK(strcmp(entry.categories, "Utility;TextEditor;") == 0);
    CHECK(strcmp(entry.working_directory, "/tmp") == 0);
    CHECK(entry.terminal == 0);
    app_entry_free(&entry);
    unlink(path);
    free(path);
}

static void test_hidden_entries(void)
{
    char *hidden = write_desktop_file(
        "[Desktop Entry]\nType=Application\nName=Hidden\nExec=hidden\nHidden=true\n");
    char *no_display = write_desktop_file(
        "[Desktop Entry]\nType=Application\nName=Internal\nExec=internal\nNoDisplay=1\n");
    AppEntry entry;

    CHECK(app_parse_desktop_file(hidden, &entry) < 0);
    CHECK(app_parse_desktop_file(no_display, &entry) < 0);
    unlink(hidden);
    unlink(no_display);
    free(hidden);
    free(no_display);
}

static void test_exec_expansion(void)
{
    AppEntry entry = {
        .name = "Example Viewer",
        .exec = "viewer --title=\"%c\" %% %f %i --desktop %k",
        .icon = "example-icon",
        .path = "/usr/share/applications/example.desktop",
        .terminal = 0
    };
    char **argv = NULL;

    CHECK(app_entry_build_argv(&entry, &argv) == 0);
    if (argv != NULL) {
        CHECK(strcmp(argv[0], "viewer") == 0);
        CHECK(strcmp(argv[1], "--title=Example Viewer") == 0);
        CHECK(strcmp(argv[2], "%") == 0);
        CHECK(strcmp(argv[3], "--icon") == 0);
        CHECK(strcmp(argv[4], "example-icon") == 0);
        CHECK(strcmp(argv[5], "--desktop") == 0);
        CHECK(strcmp(argv[6], "/usr/share/applications/example.desktop") == 0);
        CHECK(argv[7] == NULL);
    }
    app_argv_free(argv);
}

static void test_escaped_values(void)
{
    char *path = write_desktop_file(
        "[Desktop Entry]\n"
        "Type=Application\n"
        "Name=Escaped\\sName\\\\Tool\n"
        "Exec=runner \"back\\\\\\\\slash\" \"space\\svalue\" %%\n"
        "Icon=folder\\sicon\n"
        "Categories=Utility;TextEditor;\n"
        "Path=/tmp/work\\sarea\n");
    char *invalid_path = write_desktop_file(
        "[Desktop Entry]\n"
        "Type=Application\n"
        "Name=Bad\\qName\n"
        "Exec=/bin/true\n");
    AppEntry entry;
    char **argv = NULL;

    CHECK(app_parse_desktop_file(path, &entry) == 0);
    if (entry.name != NULL)
        CHECK(strcmp(entry.name, "Escaped Name\\Tool") == 0);
    if (entry.icon != NULL)
        CHECK(strcmp(entry.icon, "folder icon") == 0);
    if (entry.working_directory != NULL)
        CHECK(strcmp(entry.working_directory, "/tmp/work area") == 0);
    CHECK(app_entry_build_argv(&entry, &argv) == 0);
    if (argv != NULL) {
        CHECK(strcmp(argv[0], "runner") == 0);
        CHECK(strcmp(argv[1], "back\\slash") == 0);
        CHECK(strcmp(argv[2], "space value") == 0);
        CHECK(strcmp(argv[3], "%") == 0);
        CHECK(argv[4] == NULL);
    }
    app_argv_free(argv);
    app_entry_free(&entry);

    CHECK(app_parse_desktop_file(invalid_path, &entry) < 0);
    unlink(path);
    unlink(invalid_path);
    free(path);
    free(invalid_path);
}

static void test_empty_quoted_argument(void)
{
    AppEntry entry = {
        .name = "Argument test",
        .exec = "program first \"\" \"two words\"",
        .icon = "",
        .path = "/tmp/argument-test.desktop"
    };
    AppEntry empty_program = {
        .name = "Invalid",
        .exec = "\"\" argument",
        .icon = "",
        .path = "/tmp/invalid.desktop"
    };
    char **argv = NULL;

    CHECK(app_entry_build_argv(&entry, &argv) == 0);
    if (argv != NULL) {
        CHECK(strcmp(argv[0], "program") == 0);
        CHECK(strcmp(argv[1], "first") == 0);
        CHECK(strcmp(argv[2], "") == 0);
        CHECK(strcmp(argv[3], "two words") == 0);
        CHECK(argv[4] == NULL);
    }
    app_argv_free(argv);
    argv = NULL;
    CHECK(app_entry_build_argv(&empty_program, &argv) < 0);
    CHECK(argv == NULL);
}

static void test_launch_working_directory(void)
{
    char directory_template[] = "/tmp/win31x-working-directory-XXXXXX";
    char marker[512];
    AppEntry entry = {
        .name = "Working directory test",
        .exec = "/usr/bin/touch launch-marker",
        .icon = "",
        .path = "/tmp/working-directory.desktop",
        .working_directory = directory_template
    };
    pid_t pid;
    pid_t waited;
    int status = 0;

    if (make_temporary_directory(directory_template) < 0) {
        CHECK(0);
        return;
    }
    CHECK(snprintf(marker, sizeof(marker), "%s/launch-marker",
                   directory_template) < (int)sizeof(marker));
    pid = app_launch(&entry);
    CHECK(pid > 0);
    do {
        waited = waitpid(pid, &status, 0);
    } while (waited < 0 && errno == EINTR);
    CHECK(waited == pid);
    if (waited == pid) {
        CHECK(WIFEXITED(status));
        if (WIFEXITED(status))
            CHECK(WEXITSTATUS(status) == 0);
    }
    CHECK(access(marker, F_OK) == 0);
    unlink(marker);
    rmdir(directory_template);
}

static void test_invalid_exec(void)
{
    AppEntry unclosed = {
        .name = "Broken",
        .exec = "broken \"unterminated",
        .icon = "",
        .path = "/tmp/broken.desktop"
    };
    AppEntry unknown_code = {
        .name = "Broken",
        .exec = "broken %Z",
        .icon = "",
        .path = "/tmp/broken.desktop"
    };
    char **argv = NULL;

    errno = 0;
    CHECK(app_entry_build_argv(&unclosed, &argv) < 0);
    CHECK(argv == NULL);
    CHECK(app_entry_build_argv(&unknown_code, &argv) < 0);
}

static void write_path(const char *path, const char *contents)
{
    FILE *file = fopen(path, "w");

    if (file == NULL) {
        perror(path);
        exit(2);
    }
    if (fputs(contents, file) == EOF || fclose(file) != 0) {
        perror(path);
        exit(2);
    }
}

static void test_xdg_hidden_override(void)
{
    char root_template[] = "/tmp/win31x-xdg-test-XXXXXX";
    char *root = root_template;
    int root_descriptor = mkstemp(root_template);
    char home[512];
    char system[512];
    char home_apps[512];
    char home_nested[512];
    char system_apps[512];
    char hidden_path[512];
    char visible_path[512];
    AppList list;

    CHECK(root_descriptor >= 0);
    if (root_descriptor < 0)
        return;
    close(root_descriptor);
    CHECK(unlink(root) == 0);
    CHECK(mkdir(root, 0700) == 0);
    snprintf(home, sizeof(home), "%s/home", root_template);
    snprintf(system, sizeof(system), "%s/system", root_template);
    snprintf(home_apps, sizeof(home_apps), "%s/home/applications", root_template);
    snprintf(home_nested, sizeof(home_nested), "%s/home/applications/folder",
             root_template);
    snprintf(system_apps, sizeof(system_apps), "%s/system/applications",
             root_template);
    CHECK(mkdir(home, 0700) == 0);
    CHECK(mkdir(system, 0700) == 0);
    CHECK(mkdir(home_apps, 0700) == 0);
    CHECK(mkdir(home_nested, 0700) == 0);
    CHECK(mkdir(system_apps, 0700) == 0);
    snprintf(hidden_path, sizeof(hidden_path),
             "%s/home/applications/folder/tool.desktop", root_template);
    snprintf(visible_path, sizeof(visible_path),
             "%s/system/applications/folder-tool.desktop", root_template);
    write_path(hidden_path,
               "[Desktop Entry]\nType=Application\nName=Hidden override\n"
               "Exec=/bin/true\nHidden=true\n");
    write_path(visible_path,
               "[Desktop Entry]\nType=Application\nName=Must stay hidden\n"
               "Exec=/bin/true\n");
    CHECK(setenv("XDG_DATA_HOME", home, 1) == 0);
    CHECK(setenv("XDG_DATA_DIRS", system, 1) == 0);
    CHECK(apps_load(&list) == 0);
    CHECK(list.len == 0);
    apps_free(&list);
    unsetenv("XDG_DATA_HOME");
    unsetenv("XDG_DATA_DIRS");

    unlink(hidden_path);
    unlink(visible_path);
    rmdir(home_nested);
    rmdir(home_apps);
    rmdir(system_apps);
    rmdir(home);
    rmdir(system);
    rmdir(root);
}

static void test_deep_discovery_and_symlink_cycle(void)
{
    char root_template[] = "/tmp/win31x-deep-xdg-test-XXXXXX";
    const char *components[] = {"applications", "a", "b", "c", "d", "e"};
    char directories[sizeof(components) / sizeof(components[0])][512];
    char desktop_path[512];
    char loop_path[512];
    size_t index;
    AppList list;

    if (make_temporary_directory(root_template) < 0) {
        CHECK(0);
        return;
    }
    for (index = 0; index < sizeof(components) / sizeof(components[0]); ++index) {
        const char *parent = index == 0 ? root_template : directories[index - 1];

        CHECK(snprintf(directories[index], sizeof(directories[index]), "%s/%s",
                       parent, components[index]) <
              (int)sizeof(directories[index]));
        CHECK(mkdir(directories[index], 0700) == 0);
    }
    CHECK(snprintf(desktop_path, sizeof(desktop_path), "%s/deep.desktop",
                   directories[5]) < (int)sizeof(desktop_path));
    CHECK(snprintf(loop_path, sizeof(loop_path), "%s/loop", directories[5]) <
          (int)sizeof(loop_path));
    write_path(desktop_path,
               "[Desktop Entry]\nType=Application\nName=Deep Application\n"
               "Exec=/bin/true\n");
    CHECK(symlink(directories[0], loop_path) == 0);

    CHECK(setenv("XDG_DATA_HOME", root_template, 1) == 0);
    CHECK(setenv("XDG_DATA_DIRS", "/nonexistent", 1) == 0);
    CHECK(apps_load(&list) == 0);
    CHECK(list.len == 1);
    if (list.len == 1)
        CHECK(strcmp(list.entries[0].name, "Deep Application") == 0);
    apps_free(&list);
    unsetenv("XDG_DATA_HOME");
    unsetenv("XDG_DATA_DIRS");

    unlink(loop_path);
    unlink(desktop_path);
    for (index = sizeof(components) / sizeof(components[0]); index > 0; --index)
        rmdir(directories[index - 1]);
    rmdir(root_template);
}

int main(void)
{
    test_valid_entry();
    test_hidden_entries();
    test_exec_expansion();
    test_escaped_values();
    test_empty_quoted_argument();
    test_launch_working_directory();
    test_invalid_exec();
    test_xdg_hidden_override();
    test_deep_discovery_and_symlink_cycle();
    if (failures != 0) {
        fprintf(stderr, "%d test%s failed\n", failures, failures == 1 ? "" : "s");
        return 1;
    }
    puts("application parser tests passed");
    return 0;
}
