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
config_dir="$data_dir/config"
wifi_dir="$data_dir/wifi"
wifi_marker="$wifi_dir/secret-ok"
locker_marker="$data_dir/locker.marker"
supervisor_marker="$data_dir/supervisor.marker"
fake_xss_lock="$data_dir/fake-xss-lock"
fake_locker="$data_dir/fake-locker"
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
    rm -f "$log_file" "$second_log" "$launch_marker"
    rm -rf "$data_dir"
}
trap cleanup EXIT HUP INT TERM

if [ -z "${DISPLAY-}" ]; then
    echo "smoke-x11: DISPLAY is not set (use 'make smoke-xvfb')" >&2
    exit 2
fi

mkdir -p "$data_dir/applications" "$data_dir/empty" "$config_dir" "$wifi_dir"
ln -s "$project_dir/build/test-auto-lock" "$fake_xss_lock"
ln -s "$project_dir/build/test-auto-lock" "$fake_locker"
printf '%s\n' \
    '[Desktop Entry]' \
    'Type=Application' \
    'Name=Smoke Application' \
    "Exec=/usr/bin/touch $launch_marker" \
    'Terminal=false' >"$data_dir/applications/smoke.desktop"

"$preexisting" >"$data_dir/preexisting.log" 2>&1 &
preexisting_pid=$!
sleep 0.1

XDG_DATA_HOME="$data_dir" XDG_DATA_DIRS="$data_dir/empty" \
    XDG_CONFIG_HOME="$config_dir" \
    WIN31X_NMCLI="$project_dir/build/test-wifi-backend" \
    WIN31X_WIFI_FAKE_DIR="$wifi_dir" \
    WIN31X_XSS_LOCK="$fake_xss_lock" WIN31X_LOCKER="$fake_locker" \
    WIN31X_TEST_SUPERVISOR_MARKER="$supervisor_marker" \
    WIN31X_TEST_LOCKER_MARKER="$locker_marker" \
    "$wm" >"$log_file" 2>&1 &
wm_pid=$!

if ! WIN31X_SMOKE_MARKER="$launch_marker" \
     WIN31X_WIFI_SECRET_MARKER="$wifi_marker" \
     WIN31X_LOCKER_MARKER="$locker_marker" "$probe"; then
    echo "smoke-x11: probe failed" >&2
    sed -n '1,120p' "$log_file" >&2
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

echo "Win31 X smoke suite passed"
