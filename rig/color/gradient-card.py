#!/usr/bin/env python3
"""A test card for banding in slow, dark gradients, to be judged by eye on a real display.

Run it inside the terminal under test and look at the ramps. Each band shows
the same ramp twice: on top as an image with 16 bit floating point channels
(f=64), which holds many levels between two steps of 8 bits, and below as an
8 bit image (f=32), which cannot. The ramps are slow and dark, a step of 8 bits
every few dozen pixels, where bands are easiest to see.

Compare the terminal at its defaults with one that keeps an 8 bit surface and
rounds instead of dithering:

    kitty python3 rig/color/gradient-card.py
    kitty -o deep_color=never -o output_dither=no python3 rig/color/gradient-card.py

With the second the top ramp of each band shows the same bands as the 8 bit
one below it, a stripe every few dozen pixels. With the first, on a display
that shows more than 8 bits, the top ramps are smooth while the 8 bit ones
keep their bands; on an 8 bit display the top ramps are dithered, a fine even
grain without bands. -o deep_color=never alone shows the dithering on any
display; -o output_dither=no alone shows what the surface and display do
without it. Dim the room and look from a little further away than usual, as
the steps are faint by design. The line at the top is what the terminal
reports about its surface (output_bits, output_type and output_dither of the
display protocol), which says which of these cases you are looking at.

No screen recording permission is needed: nothing is captured, you look.
Press q or Enter to leave, or pass --seconds N.
"""

import argparse
import base64
import os
import re
import select
import struct
import sys
import termios
import time
import tty
import zlib

ESC = '\x1b'
ST = ESC + '\\'
WIDTH = 1024
HEIGHT = 48
# (label, start and end in sRGB encoded 8 bit units, which channels)
BANDS = (
    ('near black, 0 to 8', 0, 8, (1, 1, 1)),
    ('dark gray, 8 to 24', 8, 24, (1, 1, 1)),
    ('gray, 40 to 56', 40, 56, (1, 1, 1)),
    ('dark blue, 8 to 40', 8, 40, (0.15, 0.3, 1)),
)


def out(s: str) -> None:
    sys.stdout.write(s)
    sys.stdout.flush()


def srgb_to_linear(e: float) -> float:
    return e / 12.92 if e <= 0.04045 else ((e + 0.055) / 1.055) ** 2.4


def send_image(ctl: str, data: bytes) -> None:
    payload = base64.standard_b64encode(zlib.compress(data))
    first = True
    while payload:
        chunk, payload = payload[:4096], payload[4096:]
        out(f'{ESC}_G{ctl if first else ""}m={1 if payload else 0};' + chunk.decode() + ST)
        first = False


def ramp_rows(start: float, end: float, tint: tuple[float, float, float]) -> tuple[bytes, bytes]:
    'One row of the ramp as RGBA half floats in linear light and as RGBA 8 bit sRGB'
    deep, shallow = bytearray(), bytearray()
    for x in range(WIDTH):
        encoded = (start + (end - start) * x / (WIDTH - 1)) / 255
        lin = srgb_to_linear(encoded)
        channels = [lin * t for t in tint]
        deep += struct.pack('<4e', *channels, 1.0)
        shallow += bytes(round(255 * min(max(e, 0), 1)) for e in (encoded_of(c) for c in channels)) + b'\xff'
    return bytes(deep), bytes(shallow)


def encoded_of(lin: float) -> float:
    return 12.92 * lin if lin <= 0.0031308 else 1.055 * lin ** (1 / 2.4) - 0.055


def surface_report(fd: int) -> str:
    out(f'{ESC}]777;display;v=1:op=query:id=1{ST}{ESC}[c')
    buf, end = b'', time.monotonic() + 1.0
    while not re.search(rb'\x1b\[\?[0-9;]*c', buf):
        left = end - time.monotonic()
        if left <= 0 or not select.select([fd], [], [], left)[0]:
            break
        buf += os.read(fd, 4096)
    m = re.search(rb'\x1b\]777;display;([^\x07\x1b]*)', buf)
    if not m:
        return 'the terminal does not answer the display protocol'
    fields = dict(f.split('=', 1) for f in m.group(1).decode('utf-8', 'replace').split(':') if '=' in f)
    return ' '.join(f'{k}={fields.get(k, "?")}' for k in ('output_bits', 'output_type', 'output_dither', 'bits', 'gamut'))


def main() -> None:
    p = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    p.add_argument('--seconds', type=float, default=0, help='Leave after this many seconds instead of waiting for a key')
    args = p.parse_args()
    fd = sys.stdin.fileno()
    old = termios.tcgetattr(fd)
    tty.setraw(fd)
    try:
        cols = os.get_terminal_size().columns
        out(f'{ESC}[2J{ESC}[H{ESC}[?25l')
        out(f'Surface: {surface_report(fd)}\r\n')
        out('Each band: top 16 bit float (f=64), below it 8 bit (f=32). Look for stripes.\r\n\r\n')
        for label, start, end, tint in BANDS:
            deep, shallow = ramp_rows(start, end, tint)
            out(f'{label} (a step of 8 bits every {WIDTH // max(1, round(end - start))} image pixels)\r\n')
            send_image(f'a=T,f=64,s={WIDTH},v={HEIGHT},o=z,q=2,c={cols - 1},r=2,', deep * HEIGHT)
            out('\r\n  above: 16 bit float, below: 8 bit\r\n')
            send_image(f'a=T,f=32,s={WIDTH},v={HEIGHT},o=z,q=2,c={cols - 1},r=2,', shallow * HEIGHT)
            out('\r\n\r\n')
        out('q or Enter leaves')
        end = time.monotonic() + args.seconds if args.seconds > 0 else None
        while True:
            timeout = None if end is None else max(0, end - time.monotonic())
            if end is not None and timeout == 0:
                break
            if select.select([fd], [], [], timeout)[0] and os.read(fd, 64)[:1] in (b'q', b'\r', b'\n', b'\x03'):
                break
    finally:
        out(f'{ESC}_Ga=d,d=A,q=2{ST}{ESC}[?25h{ESC}[2J{ESC}[H')
        termios.tcsetattr(fd, termios.TCSADRAIN, old)


if __name__ == '__main__':
    main()
