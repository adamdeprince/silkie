#!/usr/bin/env python3
"""Print the monitor IDs Silkie uses on macOS and Linux.

Run python3 monitor-identifiers.py with each monitor connected. Please send
back the output, the monitor's marketed model name, and whether it is
monochrome e-paper, color e-paper, or an ordinary display.

This script uses the standard library and reads local display information.
It does not print serial numbers, read user files, or use the network.
"""

import json
import os
import re
import subprocess
import sys
from pathlib import Path


def monitor_id(vendor, product):
    """Format an EDID manufacturer code and product number for monitor_info."""
    letters = [(vendor >> shift) & 31 for shift in (10, 5, 0)]
    if not 0 < vendor < 0x8000 or not all(1 <= n <= 26 for n in letters):
        return None
    if not 0 <= product <= 0xFFFF:
        return None
    return ''.join(chr(64 + n) for n in letters) + f':0x{product:04X}'


def read_edid(edid):
    """Read only the manufacturer code, product number, and model name."""
    if len(edid) < 128 or edid[:8] != bytes.fromhex('00 FF FF FF FF FF FF 00'):
        return None, None
    vendor = int.from_bytes(edid[8:10], 'big')
    product = int.from_bytes(edid[10:12], 'little')
    model = None
    # EDID has four 18-byte descriptors starting at offset 54. FC is the model name.
    for offset in range(54, 126, 18):
        descriptor = edid[offset:offset + 18]
        if descriptor[:4] == bytes.fromhex('00 00 00 FC'):
            name = descriptor[5:].split(b'\n', 1)[0]
            model = ''.join(chr(c) for c in name if 32 <= c < 127).rstrip(' ')
            if model:
                break
    return monitor_id(vendor, product), model


def mac_names():
    # AppKit supplies the same localized names Silkie uses. Run this separately
    # so the CoreGraphics IDs are still available without an AppKit session.
    source = r'''
ObjC.import('AppKit');
const screens = $.NSScreen.screens;
const names = {};
for (let i = 0; i < screens.count; ++i) {
    const screen = screens.objectAtIndex(i);
    const display = screen.deviceDescription.objectForKey('NSScreenNumber');
    names[ObjC.unwrap(display)] = ObjC.unwrap(screen.localizedName);
}
JSON.stringify(names);
'''
    try:
        output = subprocess.check_output(
            ['/usr/bin/osascript', '-l', 'JavaScript', '-e', source],
            text=True, stderr=subprocess.DEVNULL)
        return json.loads(output)
    except (OSError, subprocess.CalledProcessError, ValueError):
        return {}


def mac_monitors():
    import ctypes as c

    cg = c.CDLL('/System/Library/Frameworks/CoreGraphics.framework/CoreGraphics')
    # CoreGraphics also lists online displays that lack a separate NSScreen.
    online = cg.CGGetOnlineDisplayList
    online.argtypes = [c.c_uint32, c.POINTER(c.c_uint32), c.POINTER(c.c_uint32)]
    online.restype = c.c_int32
    count = c.c_uint32()
    while True:
        if online(0, None, c.byref(count)):
            raise OSError('CoreGraphics could not count online displays')
        if not count.value:
            return
        displays = (c.c_uint32 * count.value)()
        if online(len(displays), displays, c.byref(count)):
            raise OSError('CoreGraphics could not list online displays')
        actual = count.value
        if actual < len(displays):
            break
        # Check whether a display was added while the array was being filled.
        if online(0, None, c.byref(count)):
            raise OSError('CoreGraphics could not count online displays')
        if count.value <= len(displays):
            break
    names = mac_names()
    vendor, product = cg.CGDisplayVendorNumber, cg.CGDisplayModelNumber
    for get_number in (vendor, product):
        get_number.argtypes, get_number.restype = [c.c_uint32], c.c_uint32
    for display in displays[:actual]:
        yield monitor_id(vendor(display), product(display)), names.get(str(display)), None


def x11_monitors():
    """Use the same RandR connector names and EDIDs as Silkie on X11."""
    # A hostname in DISPLAY can point to a remote X server or SSH forwarding.
    if not os.environ.get('DISPLAY', '').startswith((':', 'unix:')):
        return
    if os.environ.get('XDG_SESSION_TYPE') == 'wayland' or os.environ.get('WAYLAND_DISPLAY'):
        return
    try:
        output = subprocess.check_output(
            ['xrandr', '--current', '--prop'], text=True, stderr=subprocess.DEVNULL)
    except (OSError, subprocess.CalledProcessError):
        return
    # An unindented line starts each output. EDID lines are indented.
    for section in re.split(r'\n(?=\S)', output):
        connector = re.match(r'(\S+) connected\b', section)
        if connector is None:
            continue
        edid = re.search(r'\n[ \t]+EDID:[ \t]*\n((?:[ \t]+[a-fA-F0-9]+\n?)+)', section)
        try:
            data = bytes.fromhex(edid[1]) if edid else b''
        except ValueError:
            data = b''
        key, model = read_edid(data)
        yield key, model, connector[1]


def linux_monitors(root=Path('/sys/class/drm')):
    monitors = list(x11_monitors())
    if monitors:
        yield from monitors
        return
    for status in sorted(root.glob('card*-*/status')):
        try:
            if status.read_text().strip() != 'connected':
                continue
        except OSError:
            continue
        try:
            edid = status.with_name('edid').read_bytes()
        except OSError:
            edid = b''
        key, model = read_edid(edid)
        yield key, model, status.parent.name.partition('-')[2]


def main():
    if sys.platform == 'darwin':
        monitors = mac_monitors()
    elif sys.platform.startswith('linux'):
        monitors = linux_monitors()
    else:
        sys.exit('Run this script on macOS or Linux.')
    try:
        found = False
        for key, model, connector in monitors:
            found = True
            details = [f'model={model!r}'] if model else []
            if connector:
                details.append(f'connector={connector!r}')
            print('  '.join([key or '(no EDID ID)', *details]))
        if not found:
            print('No monitors reported by the local display system.')
    except OSError as error:
        sys.exit(str(error))


if __name__ == '__main__':
    main()
