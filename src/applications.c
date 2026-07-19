#define _POSIX_C_SOURCE 200809L

#include "applications.h"

#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <stdbool.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

#ifndef PATH_MAX
#define PATH_MAX 4096
#endif

#define APP_LAUNCH_MAX_ARGUMENTS 4096U
#define APP_LAUNCH_MAX_ARGUMENT_BYTES (1024U * 1024U)

typedef struct {
    char **items;
    size_t len;
    size_t cap;
} StringVector;

typedef struct {
    dev_t device;
    ino_t inode;
} DirectoryIdentity;

typedef struct {
    DirectoryIdentity *items;
    size_t len;
    size_t cap;
} DirectoryVector;

typedef struct {
    AppList *list;
    StringVector seen_ids;
} LoadContext;

static bool executable_in_path(const char *name);

static char *dup_string(const char *text)
{
    char *copy;
    size_t size;

    if (text == NULL)
        return NULL;
    size = strlen(text) + 1;
    copy = malloc(size);
    if (copy != NULL)
        memcpy(copy, text, size);
    return copy;
}

static char *trim(char *text)
{
    char *end;

    while (isspace((unsigned char)*text))
        ++text;
    end = text + strlen(text);
    while (end > text && isspace((unsigned char)end[-1]))
        --end;
    *end = '\0';
    return text;
}

static char *decode_desktop_value(const char *value, bool list_value)
{
    char *decoded;
    size_t input;
    size_t output = 0;

    decoded = malloc(strlen(value) + 1);
    if (decoded == NULL)
        return NULL;
    for (input = 0; value[input] != '\0'; ++input) {
        char character = value[input];

        if (character != '\\') {
            decoded[output++] = character;
            continue;
        }
        character = value[++input];
        if (character == '\0')
            goto invalid;
        if (character == 's')
            decoded[output++] = ' ';
        else if (character == 'n')
            decoded[output++] = '\n';
        else if (character == 't')
            decoded[output++] = '\t';
        else if (character == 'r')
            decoded[output++] = '\r';
        else if (character == '\\')
            decoded[output++] = '\\';
        else if (character == ';' && list_value)
            decoded[output++] = ';';
        else
            goto invalid;
    }
    decoded[output] = '\0';
    return decoded;

invalid:
    free(decoded);
    errno = EINVAL;
    return NULL;
}

static int parse_bool(const char *value)
{
    return strcasecmp(value, "true") == 0 || strcmp(value, "1") == 0 ||
           strcasecmp(value, "yes") == 0;
}

static bool semicolon_list_contains(const char *list, const char *value,
                                    size_t value_length)
{
    const char *cursor = list;

    while (cursor != NULL && *cursor != '\0') {
        const char *end = strchr(cursor, ';');
        size_t length = end == NULL ? strlen(cursor) : (size_t)(end - cursor);

        if (length == value_length && strncmp(cursor, value, length) == 0)
            return true;
        cursor = end == NULL ? NULL : end + 1;
    }
    return false;
}

static bool list_matches_current_desktop(const char *list)
{
    const char *desktop = getenv("XDG_CURRENT_DESKTOP");
    const char *cursor;

    if (desktop == NULL || desktop[0] == '\0')
        desktop = "Win31X";
    cursor = desktop;
    while (cursor != NULL && *cursor != '\0') {
        const char *end = strchr(cursor, ':');
        size_t length = end == NULL ? strlen(cursor) : (size_t)(end - cursor);

        if (semicolon_list_contains(list, cursor, length))
            return true;
        cursor = end == NULL ? NULL : end + 1;
    }
    return false;
}

void app_entry_free(AppEntry *entry)
{
    if (entry == NULL)
        return;
    free(entry->name);
    free(entry->exec);
    free(entry->icon);
    free(entry->categories);
    free(entry->path);
    free(entry->working_directory);
    memset(entry, 0, sizeof(*entry));
}

int app_parse_desktop_file(const char *path, AppEntry *entry)
{
    FILE *file;
    char *line = NULL;
    size_t capacity = 0;
    ssize_t length;
    bool in_desktop_entry = false;
    bool seen_desktop_entry = false;
    bool application_type = false;
    bool hidden = false;
    bool no_display = false;
    bool invalid_value = false;
    char *try_exec = NULL;
    char *only_show_in = NULL;
    char *not_show_in = NULL;

    if (path == NULL || entry == NULL) {
        errno = EINVAL;
        return -1;
    }
    memset(entry, 0, sizeof(*entry));
    file = fopen(path, "r");
    if (file == NULL)
        return -1;

    while ((length = getline(&line, &capacity, file)) >= 0) {
        char *key;
        char *value;
        char *equals;

        (void)length;
        key = trim(line);
        if (*key == '\0' || *key == '#' || *key == ';')
            continue;
        if (*key == '[') {
            in_desktop_entry = strcmp(key, "[Desktop Entry]") == 0;
            if (in_desktop_entry)
                seen_desktop_entry = true;
            else if (seen_desktop_entry)
                break;
            continue;
        }
        if (!in_desktop_entry)
            continue;
        equals = strchr(key, '=');
        if (equals == NULL)
            continue;
        *equals = '\0';
        value = trim(equals + 1);
        key = trim(key);

        if (strcmp(key, "Name") == 0 && entry->name == NULL) {
            entry->name = decode_desktop_value(value, false);
            invalid_value = entry->name == NULL;
        } else if (strcmp(key, "Exec") == 0 && entry->exec == NULL) {
            entry->exec = decode_desktop_value(value, false);
            invalid_value = entry->exec == NULL;
        } else if (strcmp(key, "Icon") == 0 && entry->icon == NULL) {
            entry->icon = decode_desktop_value(value, false);
            invalid_value = entry->icon == NULL;
        } else if (strcmp(key, "Categories") == 0 && entry->categories == NULL) {
            entry->categories = decode_desktop_value(value, true);
            invalid_value = entry->categories == NULL;
        } else if (strcmp(key, "Path") == 0 && entry->working_directory == NULL) {
            entry->working_directory = decode_desktop_value(value, false);
            invalid_value = entry->working_directory == NULL;
        } else if (strcmp(key, "Type") == 0)
            application_type = strcmp(value, "Application") == 0;
        else if (strcmp(key, "Terminal") == 0)
            entry->terminal = parse_bool(value);
        else if (strcmp(key, "Hidden") == 0)
            hidden = parse_bool(value);
        else if (strcmp(key, "NoDisplay") == 0)
            no_display = parse_bool(value);
        else if (strcmp(key, "TryExec") == 0 && try_exec == NULL) {
            try_exec = decode_desktop_value(value, false);
            invalid_value = try_exec == NULL;
        } else if (strcmp(key, "OnlyShowIn") == 0 && only_show_in == NULL) {
            only_show_in = decode_desktop_value(value, true);
            invalid_value = only_show_in == NULL;
        } else if (strcmp(key, "NotShowIn") == 0 && not_show_in == NULL) {
            not_show_in = decode_desktop_value(value, true);
            invalid_value = not_show_in == NULL;
        }
        if (invalid_value)
            break;
    }

    free(line);
    fclose(file);
    entry->path = dup_string(path);
    if (invalid_value || !seen_desktop_entry || !application_type || hidden ||
        no_display ||
        (try_exec != NULL && try_exec[0] != '\0' &&
         !executable_in_path(try_exec)) ||
        (only_show_in != NULL && only_show_in[0] != '\0' &&
         !list_matches_current_desktop(only_show_in)) ||
        (not_show_in != NULL && not_show_in[0] != '\0' &&
         list_matches_current_desktop(not_show_in)) ||
        entry->name == NULL || entry->exec == NULL || entry->name[0] == '\0' ||
        entry->exec[0] == '\0' || entry->path == NULL) {
        free(try_exec);
        free(only_show_in);
        free(not_show_in);
        app_entry_free(entry);
        errno = EINVAL;
        return -1;
    }
    free(try_exec);
    free(only_show_in);
    free(not_show_in);
    if (entry->icon == NULL)
        entry->icon = dup_string("");
    if (entry->categories == NULL)
        entry->categories = dup_string("");
    if (entry->working_directory == NULL)
        entry->working_directory = dup_string("");
    if (entry->icon == NULL || entry->categories == NULL ||
        entry->working_directory == NULL) {
        app_entry_free(entry);
        errno = ENOMEM;
        return -1;
    }
    return 0;
}

static int vector_push(StringVector *vector, const char *value)
{
    char **new_items;
    char *copy;
    size_t new_capacity;

    if (vector->len + 1 >= vector->cap) {
        new_capacity = vector->cap == 0 ? 8 : vector->cap * 2;
        new_items = realloc(vector->items, new_capacity * sizeof(*new_items));
        if (new_items == NULL)
            return -1;
        vector->items = new_items;
        vector->cap = new_capacity;
        vector->items[vector->len] = NULL;
    }
    copy = dup_string(value);
    if (copy == NULL)
        return -1;
    vector->items[vector->len++] = copy;
    vector->items[vector->len] = NULL;
    return 0;
}

static int append_char(char **buffer, size_t *length, size_t *capacity, char ch)
{
    char *new_buffer;
    size_t new_capacity;

    if (*length + 2 > *capacity) {
        new_capacity = *capacity == 0 ? 32 : *capacity * 2;
        new_buffer = realloc(*buffer, new_capacity);
        if (new_buffer == NULL)
            return -1;
        *buffer = new_buffer;
        *capacity = new_capacity;
    }
    (*buffer)[(*length)++] = ch;
    (*buffer)[*length] = '\0';
    return 0;
}

static int append_text(char **buffer, size_t *length, size_t *capacity,
                       const char *text)
{
    while (*text != '\0') {
        if (append_char(buffer, length, capacity, *text++) < 0)
            return -1;
    }
    return 0;
}

static char *expand_token(const AppEntry *entry, const char *token, int *drop,
                          int *icon_code)
{
    char *result = NULL;
    size_t length = 0;
    size_t capacity = 0;
    size_t index;

    *drop = 0;
    *icon_code = 0;
    if (strcmp(token, "%i") == 0) {
        *icon_code = 1;
        return NULL;
    }
    if (strcmp(token, "%f") == 0 || strcmp(token, "%F") == 0 ||
        strcmp(token, "%u") == 0 || strcmp(token, "%U") == 0 ||
        strcmp(token, "%d") == 0 || strcmp(token, "%D") == 0 ||
        strcmp(token, "%n") == 0 || strcmp(token, "%N") == 0 ||
        strcmp(token, "%v") == 0 || strcmp(token, "%m") == 0) {
        *drop = 1;
        return NULL;
    }

    for (index = 0; token[index] != '\0'; ++index) {
        if (token[index] != '%') {
            if (append_char(&result, &length, &capacity, token[index]) < 0)
                goto fail;
            continue;
        }
        ++index;
        if (token[index] == '\0')
            break;
        if (token[index] == '%') {
            if (append_char(&result, &length, &capacity, '%') < 0)
                goto fail;
        } else if (token[index] == 'c') {
            if (append_text(&result, &length, &capacity, entry->name) < 0)
                goto fail;
        } else if (token[index] == 'k') {
            if (append_text(&result, &length, &capacity, entry->path) < 0)
                goto fail;
        } else if (strchr("fFuUdDnNvmi", token[index]) == NULL) {
            /* The desktop-entry specification says unknown field codes are invalid. */
            goto fail;
        }
    }
    if (result == NULL)
        result = dup_string("");
    return result;

fail:
    free(result);
    return NULL;
}

static int finish_token(const AppEntry *entry, StringVector *vector,
                        char **token, size_t *length)
{
    char *expanded;
    int drop;
    int icon_code;

    expanded = expand_token(entry, *token, &drop, &icon_code);
    if (icon_code) {
        if (entry->icon != NULL && entry->icon[0] != '\0' &&
            (vector_push(vector, "--icon") < 0 ||
             vector_push(vector, entry->icon) < 0))
            return -1;
    } else if (!drop) {
        if (expanded == NULL || vector_push(vector, expanded) < 0) {
            free(expanded);
            return -1;
        }
    }
    free(expanded);
    *length = 0;
    (*token)[0] = '\0';
    return 0;
}

int app_entry_build_argv(const AppEntry *entry, char ***argv_out)
{
    StringVector vector = {0};
    char *token = NULL;
    size_t token_length = 0;
    size_t token_capacity = 0;
    bool quoted = false;
    bool escaped = false;
    bool token_started = false;
    const char *cursor;
    char **argv;

    if (entry == NULL || argv_out == NULL || entry->exec == NULL) {
        errno = EINVAL;
        return -1;
    }
    *argv_out = NULL;
    if (append_char(&token, &token_length, &token_capacity, '\0') < 0)
        return -1;
    token_length = 0;
    token[0] = '\0';

    for (cursor = entry->exec;; ++cursor) {
        char ch = *cursor;

        if (escaped) {
            if (ch == '\0' || append_char(&token, &token_length,
                                           &token_capacity, ch) < 0)
                goto fail;
            escaped = false;
            token_started = true;
            continue;
        }
        if (ch == '\\') {
            escaped = true;
            token_started = true;
            continue;
        }
        if (ch == '"') {
            quoted = !quoted;
            token_started = true;
            continue;
        }
        if (ch == '\0' || (!quoted && isspace((unsigned char)ch))) {
            if (token_started &&
                finish_token(entry, &vector, &token, &token_length) < 0)
                goto fail;
            token_started = false;
            if (ch == '\0')
                break;
            continue;
        }
        if (append_char(&token, &token_length, &token_capacity, ch) < 0)
            goto fail;
        token_started = true;
    }
    if (quoted || escaped || vector.len == 0 || vector.items[0][0] == '\0') {
        errno = EINVAL;
        goto fail;
    }
    argv = realloc(vector.items, (vector.len + 1) * sizeof(*argv));
    if (argv == NULL)
        goto fail;
    argv[vector.len] = NULL;
    free(token);
    *argv_out = argv;
    return 0;

fail:
    free(token);
    app_argv_free(vector.items);
    return -1;
}

int app_command_build_argv(const char *command, char ***argv_out)
{
    StringVector vector = {0};
    char *token = NULL;
    size_t token_length = 0;
    size_t token_capacity = 0;
    char quote = '\0';
    bool escaped = false;
    bool token_started = false;
    const char *cursor;
    char **argv;
    int saved_errno;

    if (command == NULL || argv_out == NULL) {
        errno = EINVAL;
        return -1;
    }
    *argv_out = NULL;
    if (append_char(&token, &token_length, &token_capacity, '\0') < 0)
        return -1;
    token_length = 0;
    token[0] = '\0';

    for (cursor = command;; ++cursor) {
        char ch = *cursor;

        if (escaped) {
            if (ch == '\0') {
                errno = EINVAL;
                goto fail;
            }
            if (append_char(&token, &token_length, &token_capacity, ch) < 0)
                goto fail;
            escaped = false;
            token_started = true;
            continue;
        }
        if (ch == '\\' && quote != '\'') {
            escaped = true;
            token_started = true;
            continue;
        }
        if (quote == '\0' && (ch == '\'' || ch == '"')) {
            quote = ch;
            token_started = true;
            continue;
        }
        if (quote != '\0' && ch == quote) {
            quote = '\0';
            continue;
        }
        if (ch == '\0' && quote != '\0') {
            errno = EINVAL;
            goto fail;
        }
        if (ch == '\0' || (quote == '\0' && isspace((unsigned char)ch))) {
            if (token_started && vector_push(&vector, token) < 0)
                goto fail;
            token_length = 0;
            token[0] = '\0';
            token_started = false;
            if (ch == '\0')
                break;
            continue;
        }
        if (append_char(&token, &token_length, &token_capacity, ch) < 0)
            goto fail;
        token_started = true;
    }
    if (escaped || vector.len == 0 || vector.items[0][0] == '\0') {
        errno = EINVAL;
        goto fail;
    }
    argv = realloc(vector.items, (vector.len + 1U) * sizeof(*argv));
    if (argv == NULL)
        goto fail;
    argv[vector.len] = NULL;
    free(token);
    *argv_out = argv;
    return 0;

fail:
    saved_errno = errno;
    free(token);
    app_argv_free(vector.items);
    errno = saved_errno;
    return -1;
}

void app_argv_free(char **argv)
{
    size_t index;

    if (argv == NULL)
        return;
    for (index = 0; argv[index] != NULL; ++index)
        free(argv[index]);
    free(argv);
}

static bool executable_in_path(const char *name)
{
    const char *path;
    const char *start;

    if (name == NULL || name[0] == '\0')
        return false;
    if (strchr(name, '/') != NULL)
        return access(name, X_OK) == 0;
    path = getenv("PATH");
    if (path == NULL)
        path = "/usr/local/bin:/usr/bin:/bin";
    start = path;
    while (*start != '\0') {
        const char *end = strchr(start, ':');
        char candidate[PATH_MAX];
        size_t directory_length = end == NULL ? strlen(start) : (size_t)(end - start);

        if (directory_length == 0) {
            if (snprintf(candidate, sizeof(candidate), "./%s", name) >=
                (int)sizeof(candidate))
                return false;
        } else if (snprintf(candidate, sizeof(candidate), "%.*s/%s",
                            (int)directory_length, start, name) >=
                   (int)sizeof(candidate)) {
            return false;
        }
        if (access(candidate, X_OK) == 0)
            return true;
        if (end == NULL)
            break;
        start = end + 1;
    }
    return false;
}

static int executable_candidate(const char *path)
{
    struct stat status;

    if (access(path, X_OK) < 0)
        return -1;
    if (stat(path, &status) < 0)
        return -1;
    if (!S_ISREG(status.st_mode)) {
        errno = EACCES;
        return -1;
    }
    return 0;
}

static int resolve_command_executable(const char *name, char **path_out)
{
    const char *path;
    const char *start;
    int result_errno = ENOENT;

    if (path_out == NULL) {
        errno = EINVAL;
        return -1;
    }
    *path_out = NULL;
    if (name == NULL || name[0] == '\0') {
        errno = EINVAL;
        return -1;
    }
    if (strchr(name, '/') != NULL) {
        if (executable_candidate(name) < 0)
            return -1;
        *path_out = strdup(name);
        return *path_out != NULL ? 0 : -1;
    }
    path = getenv("PATH");
    if (path == NULL)
        path = "/usr/local/bin:/usr/bin:/bin";
    start = path;
    for (;;) {
        const char *end = strchr(start, ':');
        char candidate[PATH_MAX];
        size_t directory_length =
            end == NULL ? strlen(start) : (size_t)(end - start);
        int length;

        if (directory_length > (size_t)INT_MAX) {
            length = (int)sizeof(candidate);
        } else if (directory_length == 0U) {
            length = snprintf(candidate, sizeof(candidate), "./%s", name);
        } else {
            length = snprintf(candidate, sizeof(candidate), "%.*s/%s",
                              (int)directory_length, start, name);
        }
        if (length < 0 || length >= (int)sizeof(candidate)) {
            if (result_errno == ENOENT)
                result_errno = ENAMETOOLONG;
        } else if (executable_candidate(candidate) == 0) {
            *path_out = strdup(candidate);
            return *path_out != NULL ? 0 : -1;
        } else if (errno == EACCES) {
            result_errno = EACCES;
        }
        if (end == NULL)
            break;
        start = end + 1;
    }
    errno = result_errno;
    return -1;
}

static void launch_terminal(char **application_argv)
{
    const char *configured = getenv("WIN31X_TERMINAL");
    static const char *const candidates[] = {
        "x-terminal-emulator", "xterm", "uxterm", "foot", "kitty", NULL
    };
    const char *terminal = NULL;
    char **terminal_argv;
    size_t application_count = 0;
    size_t index;

    if (configured != NULL && configured[0] != '\0')
        terminal = configured;
    else {
        for (index = 0; candidates[index] != NULL; ++index) {
            if (executable_in_path(candidates[index])) {
                terminal = candidates[index];
                break;
            }
        }
    }
    if (terminal == NULL) {
        fprintf(stderr, "win31x: no terminal emulator is available\n");
        _exit(127);
    }
    while (application_argv[application_count] != NULL)
        ++application_count;
    terminal_argv = calloc(application_count + 3, sizeof(*terminal_argv));
    if (terminal_argv == NULL)
        _exit(127);
    terminal_argv[0] = (char *)terminal;
    terminal_argv[1] = "-e";
    for (index = 0; index < application_count; ++index)
        terminal_argv[index + 2] = application_argv[index];
    execvp(terminal_argv[0], terminal_argv);
    fprintf(stderr, "win31x: could not execute terminal %s: %s\n",
            terminal_argv[0], strerror(errno));
    _exit(127);
}

pid_t app_launch(const AppEntry *entry)
{
    char **argv;
    pid_t pid;

    if (app_entry_build_argv(entry, &argv) < 0)
        return -1;
    pid = fork();
    if (pid == 0) {
        struct sigaction action;

        memset(&action, 0, sizeof(action));
        action.sa_handler = SIG_DFL;
        sigemptyset(&action.sa_mask);
        sigaction(SIGCHLD, &action, NULL);
        setsid();
        if (entry->working_directory != NULL &&
            entry->working_directory[0] != '\0' &&
            chdir(entry->working_directory) < 0) {
            fprintf(stderr,
                    "win31x: could not enter working directory %s for %s: %s\n",
                    entry->working_directory,
                    entry->name != NULL ? entry->name : argv[0],
                    strerror(errno));
            _exit(127);
        }
        if (entry->terminal)
            launch_terminal(argv);
        execvp(argv[0], argv);
        fprintf(stderr, "win31x: could not execute %s: %s\n",
                argv[0], strerror(errno));
        _exit(127);
    }
    app_argv_free(argv);
    return pid;
}

static pid_t launch_owned_argv(char **argv)
{
    char *executable_path = NULL;
    int error_pipe[2] = {-1, -1};
    int descriptor_flags;
    int child_errno = 0;
    size_t received = 0U;
    pid_t pid;
    int saved_errno;

    if (resolve_command_executable(argv[0], &executable_path) < 0) {
        saved_errno = errno;
        app_argv_free(argv);
        errno = saved_errno;
        return -1;
    }
    if (pipe(error_pipe) < 0) {
        saved_errno = errno;
        free(executable_path);
        app_argv_free(argv);
        errno = saved_errno;
        return -1;
    }
    descriptor_flags = fcntl(error_pipe[1], F_GETFD);
    if (descriptor_flags < 0 ||
        fcntl(error_pipe[1], F_SETFD, descriptor_flags | FD_CLOEXEC) < 0) {
        saved_errno = errno;
        close(error_pipe[0]);
        close(error_pipe[1]);
        free(executable_path);
        app_argv_free(argv);
        errno = saved_errno;
        return -1;
    }
    pid = fork();
    if (pid == 0) {
        struct sigaction action;
        int launch_errno;
        const unsigned char *error_bytes;
        size_t written = 0U;

        close(error_pipe[0]);
        memset(&action, 0, sizeof(action));
        action.sa_handler = SIG_DFL;
        sigemptyset(&action.sa_mask);
        sigaction(SIGCHLD, &action, NULL);
        sigaction(SIGPIPE, &action, NULL);
        if (setsid() < 0) {
            launch_errno = errno;
        } else {
            execv(executable_path, argv);
            launch_errno = errno;
        }
        error_bytes = (const unsigned char *)&launch_errno;
        while (written < sizeof(launch_errno)) {
            ssize_t count = write(error_pipe[1], error_bytes + written,
                                  sizeof(launch_errno) - written);

            if (count > 0)
                written += (size_t)count;
            else if (count < 0 && errno == EINTR)
                continue;
            else
                break;
        }
        _exit(127);
    }
    saved_errno = errno;
    close(error_pipe[1]);
    error_pipe[1] = -1;
    free(executable_path);
    app_argv_free(argv);
    if (pid < 0) {
        close(error_pipe[0]);
        errno = saved_errno;
        return -1;
    }
    while (received < sizeof(child_errno)) {
        ssize_t count = read(error_pipe[0],
                             (unsigned char *)&child_errno + received,
                             sizeof(child_errno) - received);

        if (count > 0)
            received += (size_t)count;
        else if (count < 0 && errno == EINTR)
            continue;
        else if (count == 0)
            break;
        else {
            saved_errno = errno;
            close(error_pipe[0]);
            errno = saved_errno;
            return -1;
        }
    }
    close(error_pipe[0]);
    if (received != 0U) {
        int status;

        while (waitpid(pid, &status, 0) < 0 && errno == EINTR)
            ;
        errno = received == sizeof(child_errno) && child_errno != 0
                    ? child_errno
                    : EIO;
        return -1;
    }
    return pid;
}

pid_t app_launch_command(const char *command)
{
    char **argv = NULL;

    if (app_command_build_argv(command, &argv) < 0)
        return -1;
    return launch_owned_argv(argv);
}

pid_t app_launch_argv(const char *const arguments[])
{
    char **copy;
    size_t count = 0U;
    size_t total = 0U;
    size_t index;

    if (arguments == NULL) {
        errno = EINVAL;
        return -1;
    }
    while (arguments[count] != NULL) {
        size_t length;

        if (count >= APP_LAUNCH_MAX_ARGUMENTS) {
            errno = E2BIG;
            return -1;
        }
        length = strlen(arguments[count]) + 1U;
        if (length > APP_LAUNCH_MAX_ARGUMENT_BYTES - total) {
            errno = E2BIG;
            return -1;
        }
        total += length;
        ++count;
    }
    if (count == 0U || arguments[0][0] == '\0') {
        errno = EINVAL;
        return -1;
    }
    copy = calloc(count + 1U, sizeof(*copy));
    if (copy == NULL)
        return -1;
    for (index = 0U; index < count; ++index) {
        copy[index] = strdup(arguments[index]);
        if (copy[index] == NULL) {
            app_argv_free(copy);
            return -1;
        }
    }
    return launch_owned_argv(copy);
}

static int add_application(AppList *list, const char *path)
{
    AppEntry entry;
    AppEntry *new_entries;

    if (app_parse_desktop_file(path, &entry) < 0)
        return 0;
    new_entries = realloc(list->entries, (list->len + 1) * sizeof(*new_entries));
    if (new_entries == NULL) {
        app_entry_free(&entry);
        return -1;
    }
    list->entries = new_entries;
    list->entries[list->len++] = entry;
    return 0;
}

static bool vector_contains(const StringVector *vector, const char *value)
{
    size_t index;

    for (index = 0; index < vector->len; ++index) {
        if (strcmp(vector->items[index], value) == 0)
            return true;
    }
    return false;
}

static char *desktop_file_id(const char *relative_path)
{
    char *identifier = dup_string(relative_path);
    char *cursor;

    if (identifier == NULL)
        return NULL;
    for (cursor = identifier; *cursor != '\0'; ++cursor) {
        if (*cursor == '/')
            *cursor = '-';
    }
    return identifier;
}

static bool directory_vector_contains(const DirectoryVector *vector,
                                      dev_t device, ino_t inode)
{
    size_t index;

    for (index = 0; index < vector->len; ++index) {
        if (vector->items[index].device == device &&
            vector->items[index].inode == inode)
            return true;
    }
    return false;
}

static int directory_vector_push(DirectoryVector *vector, dev_t device,
                                 ino_t inode)
{
    DirectoryIdentity *new_items;
    size_t new_capacity;

    if (vector->len == vector->cap) {
        new_capacity = vector->cap == 0 ? 16 : vector->cap * 2;
        new_items = realloc(vector->items,
                            new_capacity * sizeof(*new_items));
        if (new_items == NULL)
            return -1;
        vector->items = new_items;
        vector->cap = new_capacity;
    }
    vector->items[vector->len].device = device;
    vector->items[vector->len].inode = inode;
    ++vector->len;
    return 0;
}

static int scan_one_directory(LoadContext *context,
                              const char *applications_root,
                              const char *relative_directory,
                              StringVector *pending,
                              DirectoryVector *visited)
{
    DIR *dir;
    struct dirent *item;
    struct stat directory_info;
    char directory[PATH_MAX];
    int directory_fd;
    int read_error = 0;

    if (relative_directory[0] == '\0') {
        if (snprintf(directory, sizeof(directory), "%s", applications_root) >=
            (int)sizeof(directory))
            return 0;
    } else if (snprintf(directory, sizeof(directory), "%s/%s", applications_root,
                        relative_directory) >= (int)sizeof(directory)) {
        return 0;
    }
    dir = opendir(directory);
    if (dir == NULL)
        return errno == ENOENT || errno == ENOTDIR ? 0 : -1;
    directory_fd = dirfd(dir);
    if (directory_fd < 0 || fstat(directory_fd, &directory_info) < 0) {
        int saved_errno = errno;

        closedir(dir);
        errno = saved_errno;
        return -1;
    }
    if (directory_vector_contains(visited, directory_info.st_dev,
                                  directory_info.st_ino)) {
        closedir(dir);
        return 0;
    }
    if (directory_vector_push(visited, directory_info.st_dev,
                              directory_info.st_ino) < 0) {
        closedir(dir);
        return -1;
    }
    for (;;) {
        char path[PATH_MAX];
        char relative_path[PATH_MAX];
        struct stat info;
        size_t name_length;

        errno = 0;
        item = readdir(dir);
        if (item == NULL) {
            read_error = errno;
            break;
        }
        if (item->d_name[0] == '.')
            continue;
        if (snprintf(path, sizeof(path), "%s/%s", directory, item->d_name) >=
            (int)sizeof(path))
            continue;
        if (relative_directory[0] == '\0') {
            if (snprintf(relative_path, sizeof(relative_path), "%s", item->d_name) >=
                (int)sizeof(relative_path))
                continue;
        } else if (snprintf(relative_path, sizeof(relative_path), "%s/%s",
                            relative_directory, item->d_name) >=
                   (int)sizeof(relative_path)) {
            continue;
        }
        if (stat(path, &info) < 0)
            continue;
        if (S_ISDIR(info.st_mode)) {
            if (vector_push(pending, relative_path) < 0) {
                closedir(dir);
                return -1;
            }
            continue;
        }
        name_length = strlen(item->d_name);
        if (S_ISREG(info.st_mode) && name_length > 8 &&
            strcmp(item->d_name + name_length - 8, ".desktop") == 0) {
            char *identifier = desktop_file_id(relative_path);

            if (identifier == NULL) {
                closedir(dir);
                return -1;
            }
            if (!vector_contains(&context->seen_ids, identifier)) {
                if (vector_push(&context->seen_ids, identifier) < 0 ||
                    add_application(context->list, path) < 0) {
                    free(identifier);
                    closedir(dir);
                    return -1;
                }
            }
            free(identifier);
        }
    }
    closedir(dir);
    if (read_error != 0) {
        errno = read_error;
        return -1;
    }
    return 0;
}

static int scan_directory(LoadContext *context,
                          const char *applications_root)
{
    StringVector pending = {0};
    DirectoryVector visited = {0};
    size_t index;
    int result = -1;

    if (vector_push(&pending, "") < 0)
        goto done;
    for (index = 0; index < pending.len; ++index) {
        if (scan_one_directory(context, applications_root,
                               pending.items[index], &pending, &visited) < 0)
            goto done;
    }
    result = 0;

done:
    app_argv_free(pending.items);
    free(visited.items);
    return result;
}

static int compare_entries(const void *left, const void *right)
{
    const AppEntry *a = left;
    const AppEntry *b = right;
    int by_name = strcasecmp(a->name, b->name);

    if (by_name != 0)
        return by_name;
    return strcmp(a->path, b->path);
}

static int scan_data_root(LoadContext *context, const char *root)
{
    char directory[PATH_MAX];

    if (root == NULL || root[0] == '\0')
        return 0;
    if (snprintf(directory, sizeof(directory), "%s/applications", root) >=
        (int)sizeof(directory))
        return 0;
    return scan_directory(context, directory);
}

int apps_load(AppList *list)
{
    const char *data_home;
    const char *data_dirs;
    char *directories = NULL;
    char *cursor;
    LoadContext context;

    if (list == NULL) {
        errno = EINVAL;
        return -1;
    }
    memset(list, 0, sizeof(*list));
    memset(&context, 0, sizeof(context));
    context.list = list;
    data_home = getenv("XDG_DATA_HOME");
    if (data_home != NULL && data_home[0] != '\0') {
        if (scan_data_root(&context, data_home) < 0)
            goto fail;
    } else {
        const char *home = getenv("HOME");
        char fallback[PATH_MAX];

        if (home != NULL && snprintf(fallback, sizeof(fallback), "%s/.local/share", home) <
                                (int)sizeof(fallback) &&
            scan_data_root(&context, fallback) < 0)
            goto fail;
    }
    data_dirs = getenv("XDG_DATA_DIRS");
    if (data_dirs == NULL || data_dirs[0] == '\0')
        data_dirs = "/usr/local/share:/usr/share";
    directories = dup_string(data_dirs);
    if (directories == NULL)
        goto fail;
    cursor = directories;
    while (cursor != NULL) {
        char *next = strchr(cursor, ':');

        if (next != NULL)
            *next++ = '\0';
        if (scan_data_root(&context, cursor) < 0)
            goto fail;
        cursor = next;
    }
    free(directories);
    app_argv_free(context.seen_ids.items);
    if (list->len > 1)
        qsort(list->entries, list->len, sizeof(*list->entries), compare_entries);
    return 0;

fail:
    free(directories);
    app_argv_free(context.seen_ids.items);
    apps_free(list);
    return -1;
}

void apps_free(AppList *list)
{
    size_t index;

    if (list == NULL)
        return;
    for (index = 0; index < list->len; ++index)
        app_entry_free(&list->entries[index]);
    free(list->entries);
    memset(list, 0, sizeof(*list));
}
