#ifndef WIN31X_TASK_MANAGER_DATA_H
#define WIN31X_TASK_MANAGER_DATA_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <sys/types.h>

#define WIN31X_TASK_MANAGER_PATH_CAPACITY 4096U
#define WIN31X_TASK_MANAGER_NAME_CAPACITY 128U
#define WIN31X_TASK_MANAGER_COMMAND_CAPACITY 512U
#define WIN31X_TASK_MANAGER_LABEL_CAPACITY 256U
#define WIN31X_TASK_MANAGER_ERROR_CAPACITY 192U
#define WIN31X_TASK_MANAGER_PROCESS_LIMIT 16384U

typedef struct {
    pid_t pid;
    uid_t uid;
    uint64_t start_time_ticks;
    bool owned_by_user;
    char state;
    uint64_t resident_bytes;
    double cpu_percent;
    bool cpu_percent_valid;
    char name[WIN31X_TASK_MANAGER_NAME_CAPACITY];
    char command[WIN31X_TASK_MANAGER_COMMAND_CAPACITY];
} Win31xTaskManagerProcess;

typedef struct {
    double cpu_percent;
    bool cpu_percent_valid;
    uint64_t memory_total_bytes;
    uint64_t memory_available_bytes;
    double uptime_seconds;
    double load_average[3];
    char operating_system[WIN31X_TASK_MANAGER_LABEL_CAPACITY];
    char kernel[WIN31X_TASK_MANAGER_LABEL_CAPACITY];
    char hostname[WIN31X_TASK_MANAGER_LABEL_CAPACITY];
    char cpu_model[WIN31X_TASK_MANAGER_LABEL_CAPACITY];
    /* Logical processors, matching Linux /proc/stat and Task Manager usage. */
    unsigned int cpu_core_count;
} Win31xTaskManagerSystem;

typedef struct {
    Win31xTaskManagerProcess *processes;
    size_t process_count;
    bool process_list_truncated;
    Win31xTaskManagerSystem system;
} Win31xTaskManagerSnapshot;

/*
 * The fields below the snapshot are implementation state. Keeping the object
 * allocation-free at construction makes it straightforward for the window
 * manager to own one for its full lifetime; callers should otherwise treat
 * those fields as private.
 */
typedef struct {
    Win31xTaskManagerSnapshot snapshot;
    char proc_root[WIN31X_TASK_MANAGER_PATH_CAPACITY];
    char os_release_path[WIN31X_TASK_MANAGER_PATH_CAPACITY];
    uid_t user_id;
    long clock_ticks_per_second;
    long page_size;
    size_t process_limit;
    uint64_t previous_total_ticks;
    uint64_t previous_idle_ticks;
    bool has_previous_cpu_sample;
    void *previous_process_samples;
    size_t previous_process_sample_count;
    bool initialized;
} Win31xTaskManagerData;

/*
 * Configure a sampler for the host's /proc and /etc/os-release. An object may
 * be initialized only once at a time; call win31x_task_manager_data_destroy()
 * before initializing the same storage again.
 */
int win31x_task_manager_data_init(Win31xTaskManagerData *data);

/*
 * Configure alternate read-only roots for sampling tests or a read-only
 * procfs view. Both paths must be absolute. No files are read here. Process
 * signaling is deliberately disabled for objects initialized with any proc
 * root other than exactly /proc, because a numeric PID in another procfs view
 * cannot safely be bound to a process in the caller's PID namespace.
 *
 * As above, destroy an initialized object before reusing its storage.
 */
int win31x_task_manager_data_init_at(Win31xTaskManagerData *data,
                                     const char *proc_root,
                                     const char *os_release_path);

/*
 * Capture a complete point-in-time view. CPU percentages become valid on the
 * second successful refresh and compare like-for-like processes using PID and
 * kernel start time, so PID reuse cannot inherit another process's history.
 * Sampling stops at WIN31X_TASK_MANAGER_PROCESS_LIMIT rows and sets
 * process_list_truncated. A failed refresh leaves the previous snapshot
 * intact.
 */
int win31x_task_manager_data_refresh(Win31xTaskManagerData *data);

const Win31xTaskManagerSnapshot *
win31x_task_manager_data_snapshot(const Win31xTaskManagerData *data);

void win31x_task_manager_data_destroy(Win31xTaskManagerData *data);

/*
 * Send SIGTERM only after verifying that PID and expected_start_time_ticks
 * still identify the sampled process and that it belongs to the real user who
 * initialized this object. PID 1, non-positive PIDs, and the caller are always
 * refused. Signaling requires the standard /proc initializer and usable Linux
 * pidfd_open/pidfd_send_signal support (Linux 5.3 or newer); the function will
 * not fall back to race-prone kill-by-number. On failure, error_text receives
 * a display-ready explanation when a non-zero capacity is supplied. This
 * function never invokes a shell.
 */
int win31x_task_manager_data_terminate(const Win31xTaskManagerData *data,
                                       pid_t pid,
                                       uint64_t expected_start_time_ticks,
                                       char *error_text,
                                       size_t error_capacity);

/*
 * Apply the identical identity and ownership checks, then send SIGKILL. The UI
 * should expose this only as an explicit escalation after SIGTERM was given a
 * reasonable grace period.
 */
int win31x_task_manager_data_force_terminate(
    const Win31xTaskManagerData *data, pid_t pid,
    uint64_t expected_start_time_ticks, char *error_text,
    size_t error_capacity);

#endif
