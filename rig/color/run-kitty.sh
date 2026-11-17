#!/bin/sh
# run-kitty.sh <kitty tree> <seconds> [kitty args...]   runs kitty inside the headless Mutter and stops it after <seconds>
tree=$1; secs=$2; shift 2
export XDG_RUNTIME_DIR=/tmp/silkie-color-run WAYLAND_DISPLAY=${WAYLAND_DISPLAY:-silkie-color-0}
export DBUS_SESSION_BUS_ADDRESS=$(cat $XDG_RUNTIME_DIR/bus-address 2>/dev/null)
unset DISPLAY
unset SILKIE_DISABLE_WAYLAND KITTY_DISABLE_WAYLAND  # make sure Wayland is used
exec timeout -s INT "$secs" "$tree/kitty/launcher/kitty" --config NONE -o linux_display_server=wayland "$@"
