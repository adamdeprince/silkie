#!/bin/bash
# License: GPLv3 Copyright: 2026, Adam DePrince
#
# Runs a built kitty tree inside a headless sway, with wlproxy.py between
# them playing the input devices, and compares what a program inside the
# terminal receives with what was played. See README.rst.
#
#   rig/run.sh /path/to/built/tree run-name [sequence ...]
#
# Everything the run writes is in /tmp/silkie-rig-<run-name>. Only one run
# goes at a time: the directory /tmp/silkie-rig.lock is the lock.

set -u
if [ $# -lt 2 ] || [ ! -x "$1/kitty/launcher/kitty" ] || [[ ! $2 =~ ^[A-Za-z0-9._-]+$ ]]; then
    echo "usage: $0 /path/to/built/kitty/tree run-name [sequence ...]" >&2
    echo "the run name may hold letters, digits, dots, dashes and underscores" >&2
    exit 2
fi
tree=$(realpath "$1")
run=/tmp/silkie-rig-$2
shift 2
here=$(dirname "$(realpath "$0")")
lock=/tmp/silkie-rig.lock
locked=
pids=()

cleanup() {
    for pid in "${pids[@]}"; do kill "$pid" 2>/dev/null; done
    wait 2>/dev/null
    rm -f "$run"/wayland-* "$run"/control "$run"/sway-ipc.*
    [ -n "$locked" ] && rmdir "$lock"
    locked=
}
trap cleanup EXIT
trap 'exit 130' INT TERM HUP

wait_for() { # a path, seconds
    for _ in $(seq $(($2 * 20))); do
        [ -e "$1" ] && return 0
        sleep 0.05
    done
    echo "timed out waiting for $1" >&2
    return 1
}

# Two runs at once would share the machine's time, and the reports are timed
for i in $(seq 1800); do
    mkdir "$lock" 2>/dev/null && locked=1 && break
    [ "$i" = 1 ] && echo "another rig run holds $lock, waiting for up to half an hour" >&2
    sleep 1
done
if [ -z "$locked" ]; then
    echo "gave up waiting for $lock. If no rig is running, a run was killed before it could clean up: rmdir $lock" >&2
    exit 1
fi

rm -rf "$run"
mkdir -m 700 "$run" "$run/cache" "$run/config" "$run/udev" || exit 1

# A terminal asks libudev how large a tablet is and whether it is a display. The tablets here are not in the
# kernel, so the terminal, and nothing else, is given a libudev that knows them. Without it, or with RIG_NO_UDEV,
# the tablets name nodes that do not exist and the terminal must say nothing of their size
udev=
if [ -z "${RIG_NO_UDEV:-}" ]; then
    if ${CC:-cc} -shared -fPIC -Wall -Wextra -O1 -Wl,-soname,libudev.so.1 -o "$run/udev/libudev.so.1" "$here/fakeudev.c" 2>"$run/udev.err"; then
        udev=1
    else
        echo "could not build fakeudev.c, see $run/udev.err: playing without the sizes of the tablets" >&2
    fi
fi

# The compositor: no devices, no seat, no GPU. Everything it needs is in $run
cat >"$run/sway.conf" <<EOF
xwayland disable
swaybg_command -
swaynag_command -
default_border none
seat seat0 fallback true
output HEADLESS-1 mode --custom ${RIG_SIZE:-1280x800} scale 1
EOF
env -u WAYLAND_DISPLAY -u DISPLAY XDG_RUNTIME_DIR="$run" WLR_BACKENDS=headless WLR_HEADLESS_OUTPUTS=1 \
    WLR_LIBINPUT_NO_DEVICES=1 WLR_RENDERER=pixman sway --config "$run/sway.conf" >"$run/sway.log" 2>&1 &
pids+=($!)
wait_for "$run/wayland-1" 10 || exit 1

python3 "$here/wlproxy.py" --listen "$run/wayland-rig" --upstream "$run/wayland-1" --control "$run/control" \
    --log "$run/proxy.log" --xml "$tree/glfw" --tablet-version "${RIG_TABLET_VERSION:-2}" 2>"$run/proxy.err" &
pids+=($!)
wait_for "$run/control" 10 || exit 1

# The terminal, drawing in software since the compositor does. The client inside it logs what it is sent
mesa=/usr/share/glvnd/egl_vendor.d/50_mesa.json
# kitty's caches and Mesa's stay out of the home directory
env -u DISPLAY XDG_RUNTIME_DIR="$run" WAYLAND_DISPLAY=wayland-rig LIBGL_ALWAYS_SOFTWARE=1 \
    KITTY_CACHE_DIRECTORY="$run/cache" SILKIE_CONFIG_DIRECTORY="$run/config" MESA_SHADER_CACHE_DISABLE=true \
    ${RIG_WAYLAND_DEBUG:+WAYLAND_DEBUG=1} $([ -e $mesa ] && echo __EGL_VENDOR_LIBRARY_FILENAMES=$mesa) \
    ${udev:+LD_LIBRARY_PATH="$run/udev${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}"} \
    "$tree/kitty/launcher/kitty" --config NONE -o linux_display_server=wayland -o placement_strategy=top-left \
    -o window_padding_width=0 -o confirm_os_window_close=0 -o tablet_hardware_ids=yes --debug-input \
    python3 "$here/client.py" --log "$run/client.log" --ready "$run/ready" --stop "$run/stop" --duration 600 ${RIG_CLIENT_ARGS:-} \
    >"$run/kitty.log" 2>&1 &
kitty=$!
pids+=($kitty)

python3 "$here/play.py" --control "$run/control" --expect "$run/expect.jsonl" --wait "$run/ready" ${udev:+--udev} "$@"
played=$?
touch "$run/stop"
for _ in $(seq 100); do
    kill -0 $kitty 2>/dev/null || break
    sleep 0.05
done
status=0
if [ $played = 3 ]; then
    echo "the last lines of $run/kitty.log:" >&2
    grep -v '^\[' "$run/kitty.log" | tail -4 >&2
    status=1
elif [ $played != 0 ]; then
    echo "playing failed, see $run/proxy.log and $run/proxy.err" >&2
    exit 1
fi
grep -E ' (error|warning) ' "$run/proxy.log" && status=1
python3 "$here/check.py" "$run/client.log" "$run/expect.jsonl" || status=1
echo "logs are in $run"
exit $status
