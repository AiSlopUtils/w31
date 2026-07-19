#define _POSIX_C_SOURCE 200809L

#include "session_actions.h"

#include "applications.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

static int usable_program(const char *path)
{
    struct stat information;

    if (path == NULL || path[0] != '/') {
        errno = EINVAL;
        return -1;
    }
    if (strlen(path) >= WIN31X_SESSION_ACTION_PATH_CAPACITY) {
        errno = ENAMETOOLONG;
        return -1;
    }
    if (stat(path, &information) < 0)
        return -1;
    if (!S_ISREG(information.st_mode) || access(path, X_OK) < 0) {
        errno = EACCES;
        return -1;
    }
    return 0;
}

int win31x_session_actions_init(Win31xSessionActions *actions)
{
    static const char *const defaults[] = {
        "/usr/bin/systemctl", "/bin/systemctl", NULL
    };
    const char *configured;
    const char *program = NULL;
    size_t index;

    if (actions == NULL) {
        errno = EINVAL;
        return -1;
    }
    memset(actions, 0, sizeof(*actions));
    configured = getenv("WIN31X_SYSTEMCTL");
    if (configured != NULL && configured[0] != '\0') {
        if (usable_program(configured) < 0) {
            snprintf(actions->status, sizeof(actions->status),
                     "WIN31X_SYSTEMCTL is not an executable absolute file");
            return -1;
        }
        program = configured;
    } else {
        for (index = 0U; defaults[index] != NULL; ++index) {
            if (usable_program(defaults[index]) == 0) {
                program = defaults[index];
                break;
            }
        }
        if (program == NULL) {
            errno = ENOENT;
            snprintf(actions->status, sizeof(actions->status),
                     "systemctl is unavailable");
            return -1;
        }
    }
    snprintf(actions->program, sizeof(actions->program), "%s", program);
    actions->available = true;
    snprintf(actions->status, sizeof(actions->status), "Ready");
    return 0;
}

pid_t win31x_session_action_start(Win31xSessionActions *actions,
                                  Win31xSessionAction action)
{
    const char *verb;
    const char *arguments[3];

    pid_t pid;

    if (actions == NULL || !actions->available || actions->program[0] == '\0') {
        errno = ENOTSUP;
        return -1;
    }
    if (actions->child_pid > 0) {
        errno = EBUSY;
        return -1;
    }
    if (action == WIN31X_SESSION_ACTION_RESTART)
        verb = "reboot";
    else if (action == WIN31X_SESSION_ACTION_SHUT_DOWN)
        verb = "poweroff";
    else {
        errno = EINVAL;
        return -1;
    }
    arguments[0] = actions->program;
    arguments[1] = verb;
    arguments[2] = NULL;
    pid = app_launch_argv(arguments);
    if (pid > 0) {
        actions->child_pid = pid;
        snprintf(actions->status, sizeof(actions->status), "%s requested",
                 verb);
    }
    return pid;
}

bool win31x_session_actions_handle_child_exit(Win31xSessionActions *actions,
                                              pid_t pid, int wait_status)
{
    if (actions == NULL || pid <= 0 || actions->child_pid != pid)
        return false;
    actions->child_pid = 0;
    if (WIFEXITED(wait_status) && WEXITSTATUS(wait_status) == 0)
        snprintf(actions->status, sizeof(actions->status), "Ready");
    else
        snprintf(actions->status, sizeof(actions->status),
                 "The system action failed");
    return true;
}
