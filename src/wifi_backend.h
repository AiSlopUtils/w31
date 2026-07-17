#ifndef WIN31X_WIFI_BACKEND_H
#define WIN31X_WIFI_BACKEND_H

#include <stdbool.h>
#include <stddef.h>
#include <sys/select.h>
#include <sys/types.h>
#include <time.h>

#define WIFI_BACKEND_MAX_NETWORKS 128
#define WIFI_BACKEND_MAX_SSID_BYTES 32
#define WIFI_BACKEND_MAX_PASSWORD_BYTES 128

typedef enum {
    WIFI_SECURITY_OPEN,
    WIFI_SECURITY_OWE,
    WIFI_SECURITY_WPA_PSK,
    WIFI_SECURITY_SAE,
    WIFI_SECURITY_UNSUPPORTED
} WifiSecurity;

typedef enum {
    WIFI_JOB_NONE,
    WIFI_JOB_SCAN,
    WIFI_JOB_CONNECT,
    WIFI_JOB_DISCONNECT
} WifiJob;

typedef enum {
    WIFI_STATUS_IDLE,
    WIFI_STATUS_WORKING,
    WIFI_STATUS_READY,
    WIFI_STATUS_CONNECTED,
    WIFI_STATUS_DISCONNECTED,
    WIFI_STATUS_UNAVAILABLE,
    WIFI_STATUS_UNSUPPORTED,
    WIFI_STATUS_INVALID_DATA,
    WIFI_STATUS_TIMED_OUT,
    WIFI_STATUS_FAILED
} WifiStatus;

typedef struct {
    unsigned char ssid[WIFI_BACKEND_MAX_SSID_BYTES];
    size_t ssid_len;
    char ssid_hex[WIFI_BACKEND_MAX_SSID_BYTES * 2 + 1];
    char display_name[WIFI_BACKEND_MAX_SSID_BYTES * 4 + 1];
    char bssid[18];
    char device[16];
    char security_name[48];
    unsigned int signal;
    bool active;
    WifiSecurity security;
} WifiNetwork;

/*
 * The state is intentionally concrete so it can live directly inside the
 * window manager.  Fields below the public result fields are backend-private;
 * callers should use the accessors rather than modifying them.
 */
typedef struct {
    WifiNetwork networks[WIFI_BACKEND_MAX_NETWORKS];
    size_t network_count;
    WifiJob job;
    WifiStatus status;
    char status_text[192];
    int last_exit_code;

    char nmcli_path[1024];
    pid_t child_pid;
    int stdout_fd;
    int stderr_fd;
    bool child_exited;
    int child_status;
    bool stdout_eof;
    bool stderr_eof;
    bool output_overflow;
    bool timed_out;
    bool terminate_sent;
    bool kill_sent;
    bool pipe_grace_active;
    bool descriptor_error;
    struct timespec deadline;
    struct timespec kill_deadline;
    struct timespec pipe_deadline;

    char *stdout_data;
    size_t stdout_len;
    size_t stdout_capacity;
    char *stderr_data;
    size_t stderr_len;
    size_t stderr_capacity;

    int stage;
    WifiNetwork selected;
    unsigned char *password;
    size_t password_len;
    char profile_id[160];
    char profile_uuid[37];
    bool profile_created;
} WifiBackend;

/* Initialize an idle backend. Missing nmcli is represented by UNAVAILABLE. */
int wifi_backend_init(WifiBackend *backend);
void wifi_backend_destroy(WifiBackend *backend);

int wifi_backend_start_scan(WifiBackend *backend);
int wifi_backend_start_connect(WifiBackend *backend,
                               const WifiNetwork *network,
                               const unsigned char *password,
                               size_t password_len);
int wifi_backend_start_disconnect(WifiBackend *backend, const char *device);

/* Add the backend's nonblocking output descriptors to an existing select set. */
void wifi_backend_add_select_fds(WifiBackend *backend,
                                 fd_set *read_fds, int *maximum_fd);

/* Drain ready output and enforce monotonic operation deadlines. */
void wifi_backend_dispatch_fds(WifiBackend *backend,
                                const fd_set *read_fds);
void wifi_backend_tick(WifiBackend *backend);

/*
 * Feed centrally reaped children here. Returns true only for this backend's
 * current child. The backend waits for pipe EOF before completing the stage.
 */
bool wifi_backend_handle_child_exit(WifiBackend *backend, pid_t pid,
                                    int wait_status);

bool wifi_backend_busy(const WifiBackend *backend);
WifiJob wifi_backend_job(const WifiBackend *backend);
WifiStatus wifi_backend_status(const WifiBackend *backend);
const char *wifi_backend_status_text(const WifiBackend *backend);
int wifi_backend_last_exit_code(const WifiBackend *backend);
size_t wifi_backend_network_count(const WifiBackend *backend);
const WifiNetwork *wifi_backend_network_at(const WifiBackend *backend,
                                           size_t index);
bool wifi_backend_network_supported(const WifiNetwork *network);

/* Exposed for deterministic parser tests and for refreshing cached scans. */
int wifi_backend_parse_scan_output(WifiBackend *backend,
                                   const char *output, size_t length);

#endif
