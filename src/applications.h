#ifndef WIN31X_APPLICATIONS_H
#define WIN31X_APPLICATIONS_H

#include <stddef.h>
#include <sys/types.h>

typedef struct {
    char *name;
    char *exec;
    char *icon;
    char *categories;
    char *path;
    char *working_directory;
    int terminal;
} AppEntry;

typedef struct {
    AppEntry *entries;
    size_t len;
} AppList;

/* Load visible Type=Application desktop entries from the XDG data paths. */
int apps_load(AppList *list);
void apps_free(AppList *list);

/* These are public primarily so the parser can be tested without an X server. */
int app_parse_desktop_file(const char *path, AppEntry *entry);
void app_entry_free(AppEntry *entry);
int app_entry_build_argv(const AppEntry *entry, char ***argv_out);

/* Parse a Run-dialog command without invoking a shell or expanding desktop
 * entry field codes. Single and double quotes group literal arguments;
 * outside single quotes, a backslash quotes the following byte. */
int app_command_build_argv(const char *command, char ***argv_out);
void app_argv_free(char **argv);

/* Start an application in a detached child. Returns its pid, or -1 on error. */
pid_t app_launch(const AppEntry *entry);
pid_t app_launch_command(const char *command);

/* Launch an already-separated argument vector directly, without a shell.
 * The vector and its strings are copied before the child is started. */
pid_t app_launch_argv(const char *const argv[]);

#endif
