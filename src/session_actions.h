#ifndef WIN31X_SESSION_ACTIONS_H
#define WIN31X_SESSION_ACTIONS_H

#include <stdbool.h>
#include <sys/types.h>

#define WIN31X_SESSION_ACTION_PATH_CAPACITY 4096U
#define WIN31X_SESSION_ACTION_STATUS_CAPACITY 192U

typedef enum {
    WIN31X_SESSION_ACTION_RESTART,
    WIN31X_SESSION_ACTION_SHUT_DOWN
} Win31xSessionAction;

typedef struct {
    bool available;
    pid_t child_pid;
    char program[WIN31X_SESSION_ACTION_PATH_CAPACITY];
    char status[WIN31X_SESSION_ACTION_STATUS_CAPACITY];
} Win31xSessionActions;

/* Resolve an absolute systemctl executable. WIN31X_SYSTEMCTL is accepted as
 * an explicit override for testing and local installations. */
int win31x_session_actions_init(Win31xSessionActions *actions);

/* Start `systemctl reboot` or `systemctl poweroff` directly, without a shell. */
pid_t win31x_session_action_start(Win31xSessionActions *actions,
                                  Win31xSessionAction action);

/* Clear a completed action child. Returns true when pid belonged here. */
bool win31x_session_actions_handle_child_exit(Win31xSessionActions *actions,
                                              pid_t pid, int wait_status);

#endif
