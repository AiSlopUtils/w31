#!/bin/sh
set -eu

project_dir=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
wm=${WIN31X_WM_BINARY:-"$project_dir/build/win31x"}
probe=${WIN31X_PERSISTENCE_PROBE:-"$project_dir/build/persistence-probe"}

if [ "${WIN31X_PERSISTENCE_UNDER_XVFB-}" != 1 ]; then
    if ! command -v xvfb-run >/dev/null 2>&1; then
        echo "persistence-x11: xvfb-run is required" >&2
        exit 2
    fi
    WIN31X_PERSISTENCE_UNDER_XVFB=1 exec xvfb-run -a \
        -s "-screen 0 1600x700x24" "$0"
fi
if [ -z "${DISPLAY-}" ]; then
    echo "persistence-x11: Xvfb did not provide DISPLAY" >&2
    exit 2
fi

if [ ! -x "$wm" ]; then
    echo "persistence-x11: WM binary is missing: $wm" >&2
    exit 2
fi
if [ ! -x "$probe" ]; then
    echo "persistence-x11: probe binary is missing: $probe" >&2
    exit 2
fi

data_dir=$(mktemp -d "${TMPDIR:-/tmp}/win31x-persistence.XXXXXX")
config_dir="$data_dir/config"
data_home="$data_dir/data"
empty_data="$data_dir/empty"
wm_pid=
current_log=

mkdir -p "$config_dir" "$data_home" "$empty_data"

cleanup() {
    if [ -n "$wm_pid" ]; then
        kill -TERM "$wm_pid" 2>/dev/null || true
        wait "$wm_pid" 2>/dev/null || true
    fi
    rm -rf "$data_dir"
}
trap cleanup EXIT HUP INT TERM

show_log() {
    if [ -n "$current_log" ] && [ -f "$current_log" ]; then
        sed -n '1,180p' "$current_log" >&2
    fi
}

start_wm() {
    phase=$1
    monitors=$2
    current_log="$data_dir/wm-$phase.log"
    XDG_CONFIG_HOME="$config_dir" \
        XDG_DATA_HOME="$data_home" XDG_DATA_DIRS="$empty_data" \
        WIN31X_ICON_DIR="$project_dir/assets/icons" \
        WIN31X_TEST_MONITORS="$monitors" \
        "$wm" >"$current_log" 2>&1 &
    wm_pid=$!
}

stop_wm() {
    attempt=0

    kill -TERM "$wm_pid"
    while kill -0 "$wm_pid" 2>/dev/null; do
        if [ "$attempt" -ge 250 ]; then
            echo "persistence-x11: WM did not stop gracefully" >&2
            show_log
            exit 1
        fi
        attempt=$((attempt + 1))
        sleep 0.02
    done
    if ! wait "$wm_pid"; then
        echo "persistence-x11: WM exited unsuccessfully" >&2
        show_log
        exit 1
    fi
    wm_pid=
}

run_probe() {
    mode=$1

    if ! "$probe" "$mode"; then
        echo "persistence-x11: probe failed in $mode" >&2
        show_log
        exit 1
    fi
}

dual_monitors='800x600+0+0,800x600+800+100'
single_monitor='640x480+0+0'

start_wm seed "$dual_monitors"
run_probe --seed
stop_wm

layout_file="$config_dir/win31x/layout.conf"
settings_file="$config_dir/win31x/settings.conf"
if [ ! -s "$layout_file" ]; then
    echo "persistence-x11: layout.conf was not saved" >&2
    show_log
    exit 1
fi
if ! grep -q '^color_scheme=ocean-blue$' "$settings_file" ||
   ! grep -q '^control_panel_section=colors$' "$settings_file"; then
    echo "persistence-x11: color or Control Panel section was not saved" >&2
    show_log
    exit 1
fi

start_wm verify-dual "$dual_monitors"
run_probe --verify-dual
stop_wm

cksum "$layout_file" >"$data_dir/layout-before-single.cksum"
start_wm verify-single "$single_monitor"
run_probe --verify-single
stop_wm
cksum "$layout_file" >"$data_dir/layout-after-single.cksum"
if ! cmp -s "$data_dir/layout-before-single.cksum" \
          "$data_dir/layout-after-single.cksum"; then
    echo "persistence-x11: monitor fallback overwrote preferred layout" >&2
    exit 1
fi

start_wm verify-dual-return "$dual_monitors"
run_probe --verify-dual-return
stop_wm

printf '%s\n' \
    '# Win31 X hostile-coordinate regression fixture' \
    'version 2' \
    'applications_icon 1 - 1200 400 -2147483648 2147483647 112 80 0 0' \
    'control_panel_icon 1 - 1200 400 2147483647 -2147483648 112 80 0 0' \
    >"$layout_file"
start_wm verify-extreme "$dual_monitors"
run_probe --verify-extreme
stop_wm

echo "Win31 X persistence X11 suite passed"
