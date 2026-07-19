#!/bin/sh
set -eu

project_dir=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
wm=${WIN31X_WM_BINARY:-"$project_dir/build/win31x"}
probe="$project_dir/build/wm-probe"
preexisting="$project_dir/build/preexisting-client"
log_file=${TMPDIR:-/tmp}/win31x-smoke-$$.log
second_log=${TMPDIR:-/tmp}/win31x-smoke-second-$$.log
data_dir=$(mktemp -d "${TMPDIR:-/tmp}/win31x-smoke-data.XXXXXX")
launch_marker=${TMPDIR:-/tmp}/win31x-smoke-launched-$$
run_marker=${TMPDIR:-/tmp}/w31run-$$
config_dir="$data_dir/config"
wifi_dir="$data_dir/wifi"
wifi_marker="$wifi_dir/secret-ok"
locker_marker="$data_dir/locker.marker"
supervisor_marker="$data_dir/supervisor.marker"
system_action_marker="$data_dir/system-action.marker"
system_action_log="$data_dir/system-action.log"
expected_system_action_log="$data_dir/expected-system-action.log"
monitor_layout=${WIN31X_TEST_MONITORS-}
fake_xss_lock="$data_dir/fake-xss-lock"
fake_locker="$data_dir/fake-locker"
fake_systemctl_backend="$data_dir/fake-systemctl"
fake_systemctl_wrapper="$data_dir/fake-systemctl-wrapper"
wm_pid=
preexisting_pid=

cleanup() {
    if [ -n "$wm_pid" ]; then
        kill "$wm_pid" 2>/dev/null || true
        wait "$wm_pid" 2>/dev/null || true
    fi
    if [ -n "$preexisting_pid" ]; then
        kill "$preexisting_pid" 2>/dev/null || true
        wait "$preexisting_pid" 2>/dev/null || true
    fi
    rm -f "$log_file" "$second_log" "$launch_marker" "$run_marker"
    rm -rf "$data_dir"
}
trap cleanup EXIT HUP INT TERM

if [ -z "${DISPLAY-}" ]; then
    echo "smoke-x11: DISPLAY is not set (use 'make smoke-xvfb')" >&2
    exit 2
fi

if [ -n "$monitor_layout" ] &&
   [ "$monitor_layout" != '800x600+0+0,800x600+800+100' ]; then
    echo "smoke-x11: unsupported deterministic monitor layout: $monitor_layout" >&2
    echo "smoke-x11: expected 800x600+0+0,800x600+800+100" >&2
    exit 2
fi

mkdir -p "$data_dir/applications" "$data_dir/empty" "$config_dir" "$wifi_dir" \
    "$data_dir/icons/hicolor/48x48/apps"
cp "$project_dir/assets/icons/settings-48.png" \
    "$data_dir/icons/hicolor/48x48/apps/smoke-actual.png"
ln -s "$project_dir/build/test-auto-lock" "$fake_xss_lock"
ln -s "$project_dir/build/test-auto-lock" "$fake_locker"
ln -s "$project_dir/build/test-session-actions" "$fake_systemctl_backend"
printf '%s\n' \
    '#!/bin/sh' \
    'set -eu' \
    'printf "%s\n" "$1" >>"$WIN31X_TEST_SYSTEM_ACTION_LOG"' \
    'exec "$WIN31X_TEST_SYSTEMCTL_BACKEND" "$@"' \
    >"$fake_systemctl_wrapper"
chmod 0700 "$fake_systemctl_wrapper"
printf '%s\n' \
    '[Desktop Entry]' \
    'Type=Application' \
    'Name=Smoke Application' \
    "Exec=/usr/bin/touch $launch_marker" \
    'Icon=smoke-actual' \
    'Terminal=false' >"$data_dir/applications/smoke.desktop"

if [ -z "$monitor_layout" ]; then
    "$preexisting" >"$data_dir/preexisting.log" 2>&1 &
    preexisting_pid=$!
    sleep 0.1
fi

XDG_DATA_HOME="$data_dir" XDG_DATA_DIRS="$data_dir/empty" \
    XDG_CONFIG_HOME="$config_dir" \
    WIN31X_NMCLI="$project_dir/build/test-wifi-backend" \
    WIN31X_WIFI_FAKE_DIR="$wifi_dir" \
    WIN31X_XSS_LOCK="$fake_xss_lock" WIN31X_LOCKER="$fake_locker" \
    WIN31X_SYSTEMCTL="$fake_systemctl_wrapper" \
    WIN31X_TEST_SYSTEMCTL_BACKEND="$fake_systemctl_backend" \
    WIN31X_TEST_SYSTEM_ACTION_LOG="$system_action_log" \
    WIN31X_TEST_SUPERVISOR_MARKER="$supervisor_marker" \
    WIN31X_TEST_LOCKER_MARKER="$locker_marker" \
    WIN31X_TEST_LOCKER_ONESHOT=1 \
    WIN31X_TEST_SYSTEM_ACTION_MARKER="$system_action_marker" \
    WIN31X_TEST_MONITORS="$monitor_layout" \
    "$wm" >"$log_file" 2>&1 &
wm_pid=$!

if [ -n "$monitor_layout" ]; then
    if ! "$probe" --multi-monitor; then
        echo "smoke-x11: multi-monitor probe failed" >&2
        sed -n '1,160p' "$log_file" >&2
        exit 1
    fi
    echo "Win31 X multi-monitor smoke suite passed"
    exit 0
else
    if ! WIN31X_SMOKE_MARKER="$launch_marker" \
         WIN31X_RUN_MARKER="$run_marker" \
         WIN31X_WIFI_SECRET_MARKER="$wifi_marker" \
         WIN31X_LOCKER_MARKER="$locker_marker" \
         WIN31X_SYSTEM_ACTION_MARKER="$system_action_marker" "$probe"; then
        echo "smoke-x11: probe failed" >&2
        sed -n '1,120p' "$log_file" >&2
        exit 1
    fi
fi
printf '%s\n' reboot poweroff >"$expected_system_action_log"
if [ ! -f "$system_action_log" ] ||
   ! cmp -s "$expected_system_action_log" "$system_action_log"; then
    echo "smoke-x11: confirmed system actions were not invoked exactly once" >&2
    if [ -f "$system_action_log" ]; then
        sed -n '1,20p' "$system_action_log" >&2
    else
        echo "smoke-x11: system action log was not created" >&2
    fi
    exit 1
fi
if ! grep -q '^color_scheme=ocean-blue$' \
        "$config_dir/win31x/settings.conf"; then
    echo "smoke-x11: Control Panel color setting was not persisted" >&2
    exit 1
fi
if ! grep -q '^add$' "$wifi_dir/stage-log" ||
   ! grep -q '^up$' "$wifi_dir/stage-log"; then
    echo "smoke-x11: Wi-Fi profile workflow did not complete" >&2
    exit 1
fi
if grep -Fq 'correct horse' "$log_file"; then
    echo "smoke-x11: Wi-Fi password leaked into the WM log" >&2
    exit 1
fi

if "$wm" >"$second_log" 2>&1; then
    echo "smoke-x11: a second window manager unexpectedly started" >&2
    exit 1
fi
if ! grep -q "another window manager" "$second_log"; then
    echo "smoke-x11: second-manager error was not informative" >&2
    sed -n '1,80p' "$second_log" >&2
    exit 1
fi

if ! "$probe" --desktop-menu-logout; then
    echo "smoke-x11: desktop menu logout probe failed" >&2
    sed -n '1,120p' "$log_file" >&2
    exit 1
fi

exit_attempt=0
while kill -0 "$wm_pid" 2>/dev/null; do
    if [ "$exit_attempt" -ge 200 ]; then
        echo "smoke-x11: window manager did not exit after Log Out" >&2
        sed -n '1,120p' "$log_file" >&2
        exit 1
    fi
    exit_attempt=$((exit_attempt + 1))
    sleep 0.01
done
if ! wait "$wm_pid"; then
    echo "smoke-x11: window manager exited unsuccessfully after Log Out" >&2
    sed -n '1,120p' "$log_file" >&2
    exit 1
fi
wm_pid=

echo "Win31 X smoke suite passed"
