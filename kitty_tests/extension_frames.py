#!/usr/bin/env python
# License: GPLv3 Copyright: 2026, Adam DePrince

# The rules every OSC 777 extension shares for the frames a program sends, see
# docs/extension-framing.rst. The same file is on the branch of every extension.

from .base import parse_bytes


def assert_extension_frames(test, screen, selector, query='op=query', key='id'):
    # query is a frame without its version that the terminal answers once key
    # is added to it with a non-negative 31 bit integer
    c = screen.callbacks

    def reply(body):
        c.clear()
        parse_bytes(screen, f'\x1b]777;{selector};{body}\x1b\\'.encode())
        return c.wtcbuf

    good = reply(f'v=1:{query}:{key}=5')
    test.assertTrue(good, f'{selector}: the query is not answered')
    # A key the terminal does not know is ignored, so that fields can be added
    test.ae(reply(f'v=1:{query}:{key}=5:later=x-y'), good)
    test.ae(reply(f'later=x:{key}=5:v=1:{query}'), good)
    # A frame is rejected as a whole for a duplicate key, a value that is not a
    # decimal integer in range, a field that is not key=value, a missing v or
    # op, or a version the terminal does not speak
    for body in (
        f'v=1:{query}:{key}=5:{key}=5', f'v=1:{query}:{key}=5:later=1:later=2', f'v=1:v=1:{query}:{key}=5', f'v=1:{query}:{key}=+5',
        f'v=1:{query}:{key}=5x', f'v=1:{query}:{key}=', f'v=1:{query}:{key}=-1', f'v=1:{query}:{key}=2147483648', f'v=1:{query}:{key}=5:',
        f'v=1::{query}:{key}=5', f'v=1:{query}:{key}=5:junk', f'v=1:{query}:{key}=5:=0', f'{query}:{key}=5', f'v=1:{key}=5',
        f'v=2:{query}:{key}=5', f'v=+1:{query}:{key}=5', f'v=1x:{query}:{key}=5',
    ):
        test.ae(reply(body), b'', f'{selector}: {body}')
    # Storage grows with the frame, including fields introduced in later versions.
    body = f'v=1:{query}:{key}=5:p='
    for size in (127, 128, 4096, 16384):
        test.ae(reply(body + 'x' * (size - len(body))), good, f'{selector}: {size} bytes')
