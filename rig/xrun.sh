#!/bin/bash
# License: GPLv3 Copyright: 2026, Adam DePrince
#
# The X11 half of the rig: runs a built kitty tree on an X server that has no
# hardware, only devices of the X server's own test driver, plays input
# through them with x11.py and compares what a program inside the terminal
# receives with what was played. See README.rst.
#
#   rig/xrun.sh /path/to/built/tree run-name [sequence ...]
#
# Everything the run writes is in /tmp/silkie-rig-<run-name>, and it shares
# the lock /tmp/silkie-rig.lock with run.sh.

set -u
if [ $# -lt 2 ] || [ ! -x "$1/kitty/launcher/kitty" ] || [[ ! $2 =~ ^[A-Za-z0-9._-]+$ ]]; then
    echo "usage: $0 /path/to/built/kitty/tree run-name [sequence ...]" >&2
    exit 2
fi
tree=$(realpath "$1")
run=/tmp/silkie-rig-$2
shift 2
here=$(dirname "$(realpath "$0")")
size=${RIG_SIZE:-1280x800}
lock=/tmp/silkie-rig.lock
locked=
pids=()

cleanup() {
    for pid in "${pids[@]}"; do kill "$pid" 2>/dev/null; done
    wait 2>/dev/null
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
mkdir -m 700 "$run" "$run/devices" "$run/cache" "$run/config" "$run/udev" || exit 1

# As in run.sh, the terminal alone gets a libudev that knows the rig's tablets. Their X devices are given the
# properties by which the libinput and wacom drivers name the device node of a tablet and its ids, so with
# RIG_NO_UDEV the terminal asks the real libudev about /dev/null, which is no tablet, and must say nothing
udev=
if [ -z "${RIG_NO_UDEV:-}" ]; then
    if ${CC:-cc} -shared -fPIC -Wall -Wextra -O1 -Wl,-soname,libudev.so.1 -o "$run/udev/libudev.so.1" "$here/fakeudev.c" 2>"$run/udev.err"; then
        udev=1
    else
        echo "could not build fakeudev.c, see $run/udev.err: playing without the sizes of the tablets" >&2
    fi
fi
xdevprop=
if ${CC:-cc} -Wall -Wextra -O1 -o "$run/xdevprop" "$here/xdevprop.c" -lXi -lX11 2>"$run/xdevprop.err"; then
    xdevprop="$run/xdevprop"
else
    echo "could not build xdevprop.c, see $run/xdevprop.err: the tablets name no device node" >&2
    udev=
fi

# The X server: the dummy video driver, no devices from the kernel, and one
# device of the inputtest driver for each kind of hardware. A device with the
# name of a tool in its own name is what kitty takes for a tablet tool
device() { # name, type, options
    cat <<EOF
Section "InputDevice"
    Identifier "$1"
    Driver "inputtest"
    Option "DeviceType" "$2"
    Option "SocketPath" "$run/devices/${1##* }"
$3
EndSection
EOF
}
{
    cat <<EOF
Section "ServerFlags"
    Option "AutoAddDevices" "false"
    Option "AutoEnableDevices" "false"
    Option "AutoAddGPU" "false"
    Option "DontVTSwitch" "true"
    Option "PciForceNone" "true"
    Option "BlankTime" "0"
EndSection
Section "Device"
    Identifier "card"
    Driver "dummy"
    VideoRam 64000
EndSection
Section "Monitor"
    Identifier "monitor"
    HorizSync 5.0-1000.0
    VertRefresh 5.0-200.0
    Modeline "rig" 100 ${size%x*} $((${size%x*} + 64)) $((${size%x*} + 128)) $((${size%x*} + 256)) ${size#*x} $((${size#*x} + 2)) $((${size#*x} + 6)) $((${size#*x} + 30))
EndSection
Section "Screen"
    Identifier "screen"
    Device "card"
    Monitor "monitor"
    DefaultDepth 24
    SubSection "Display"
        Depth 24
        Modes "rig"
        Virtual ${size/x/ }
    EndSubSection
EndSection
Section "ServerLayout"
    Identifier "rig"
    Screen "screen"
    InputDevice "Rig keyboard" "CoreKeyboard"
    InputDevice "Rig mouse" "CorePointer"
    InputDevice "Rig Wacom Intuos5 touch M Pen stylus" "SendCoreEvents"
    InputDevice "Rig Wacom Intuos5 touch M Pen eraser" "SendCoreEvents"
    InputDevice "Rig touchscreen" "SendCoreEvents"
    InputDevice "Rig touchpad" "SendCoreEvents"
    InputDevice "Rig Wacom Intuos5 touch M Pad pad" "SendCoreEvents"
EndSection
EOF
    device "Rig keyboard" Keyboard ""
    device "Rig mouse" PointerAbsolute ""
    device "Rig Wacom Intuos5 touch M Pen stylus" PointerAbsoluteProximity '    Option "PointerHasPressure" "true"
    Option "PointerButtonCount" "9"'
    device "Rig Wacom Intuos5 touch M Pen eraser" PointerAbsoluteProximity '    Option "PointerHasPressure" "true"'
    device "Rig touchscreen" Touch ""
    device "Rig touchpad" PointerGesture ""
    # A pad of nine keys, as the wacom driver makes it: a device of thirteen buttons, of which 4 to 7 scroll
    device "Rig Wacom Intuos5 touch M Pad pad" PointerAbsolute '    Option "PointerButtonCount" "13"'
} >"$run/xorg.conf"

display=
for n in $(seq 70 99); do
    [ -e /tmp/.X11-unix/X$n ] || [ -e /tmp/.X$n-lock ] || { display=$n; break; }
done
[ -n "$display" ] || { echo "no free X display between :70 and :99" >&2; exit 1; }
# the binary behind /usr/bin/Xorg, which on Debian and Ubuntu is a wrapper that only lets a console user in
xorg=/usr/lib/xorg/Xorg
[ -x $xorg ] || xorg=$(command -v Xorg)
$xorg :$display -config "$run/xorg.conf" -logfile "$run/xorg.log" -logverbose 4 \
    -seat seat-silkie-rig -noreset -nolisten tcp -nolisten local >"$run/xorg.out" 2>&1 &
pids+=($!)
wait_for /tmp/.X11-unix/X$display 15 || { tail -5 "$run/xorg.out" >&2; exit 1; }
wait_for "$run/devices/touchpad" 10 || exit 1
if [ -n "$xdevprop" ]; then
    for tool in stylus eraser; do
        name="Rig Wacom Intuos5 touch M Pen $tool"
        DISPLAY=:$display "$xdevprop" "$name" string "Device Node" /dev/null &&
            DISPLAY=:$display "$xdevprop" "$name" int32 "Device Product ID" 0x056a 0x0027 || { xdevprop=; udev=; }
    done
    # The test driver types its devices itself, so the pad is one by the property the wacom driver says it with
    [ -n "$xdevprop" ] && { DISPLAY=:$display "$xdevprop" "Rig Wacom Intuos5 touch M Pad pad" atom "Wacom Tool Type" PAD &&
        DISPLAY=:$display "$xdevprop" "Rig Wacom Intuos5 touch M Pad pad" int32 "Device Product ID" 0x056a 0x0027 || xdevprop=; }
fi
if [ -n "${RIG_XINPUT_TEST:-}" ]; then # what the X server itself reports, to tell its doing from the terminal's
    DISPLAY=:$display xinput test-xi2 --root >"$run/xi2.log" 2>&1 &
    pids+=($!)
fi

# kitty's caches and Mesa's stay out of the home directory
env -u WAYLAND_DISPLAY DISPLAY=:$display LIBGL_ALWAYS_SOFTWARE=1 __GLX_VENDOR_LIBRARY_NAME=mesa \
    KITTY_CACHE_DIRECTORY="$run/cache" SILKIE_CONFIG_DIRECTORY="$run/config" MESA_SHADER_CACHE_DISABLE=true \
    $([ -e /usr/share/glvnd/egl_vendor.d/50_mesa.json ] && echo __EGL_VENDOR_LIBRARY_FILENAMES=/usr/share/glvnd/egl_vendor.d/50_mesa.json) \
    ${udev:+LD_LIBRARY_PATH="$run/udev${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}"} \
    "$tree/kitty/launcher/kitty" --config NONE -o linux_display_server=x11 -o placement_strategy=top-left \
    -o window_padding_width=0 -o confirm_os_window_close=0 -o tablet_hardware_ids=yes -o remember_window_size=no \
    -o initial_window_width=${size%x*} -o initial_window_height=${size#*x} --debug-input \
    python3 "$here/client.py" --log "$run/client.log" --ready "$run/ready" --stop "$run/stop" --duration 600 ${RIG_CLIENT_ARGS:-} \
    >"$run/kitty.log" 2>&1 &
kitty=$!
pids+=($kitty)

python3 "$here/x11.py" --sockets "$run/devices" --expect "$run/expect.jsonl" --wait "$run/ready" --size "$size" --display ":$display" \
    ${udev:+--udev} ${xdevprop:+--node --pad} "$@"
played=$?
touch "$run/stop"
for _ in $(seq 100); do
    kill -0 $kitty 2>/dev/null || break
    sleep 0.05
done
status=0
if [ $played != 0 ]; then
    echo "playing failed. The last lines of $run/kitty.log and $run/xorg.log:" >&2
    grep -v '^\[' "$run/kitty.log" | tail -4 >&2
    grep -E '\((EE|WW)\)' "$run/xorg.log" | tail -4 >&2
    status=1
fi
python3 "$here/check.py" "$run/client.log" "$run/expect.jsonl" || status=1
echo "logs are in $run"
exit $status
