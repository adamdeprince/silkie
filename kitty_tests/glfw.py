#!/usr/bin/env python
# License: GPL v3 Copyright: 2020, Kovid Goyal <kovid at kovidgoyal.net>

import os
import sys
import unittest

from .base import BaseTest

_plat = sys.platform.lower()
is_macos = 'darwin' in _plat


class TestGLFW(BaseTest):
    def test_os_window_size_calculation(self):
        from kitty.utils import get_new_os_window_size

        def t(w, h, width=0, height=0, unit='cells', incremental=False):
            self.ae((w, h), get_new_os_window_size(metrics, width, height, unit, incremental, has_window_scaling))

        with self.subTest(has_window_scaling=False):
            has_window_scaling = False
            metrics = {
                'width': 200,
                'height': 100,
                'framebuffer_width': 200,
                'framebuffer_height': 100,
                'xscale': 2.0,
                'yscale': 2.0,
                'xdpi': 192.0,
                'ydpi': 192.0,
                'cell_width': 8,
                'cell_height': 16,
            }
            t(80 * metrics['cell_width'], 100, 80)
            t(80 * metrics['cell_width'] + metrics['width'], 100, 80, incremental=True)
            t(1217, 100, 1217, unit='pixels')
            t(1217 + metrics['width'], 100, 1217, unit='pixels', incremental=True)

        with self.subTest(has_window_scaling=True):
            has_window_scaling = True
            metrics['framebuffer_width'] = metrics['width'] * 2
            metrics['framebuffer_height'] = metrics['height'] * 2
            t(80 * metrics['cell_width'] / metrics['xscale'], 100, 80)
            t(80 * metrics['cell_width'] / metrics['xscale'] + metrics['width'], 100, 80, incremental=True)
            t(1217, 100, 1217, unit='pixels')
            t(1217 + metrics['width'], 100, 1217, unit='pixels', incremental=True)

    @unittest.skipIf(is_macos, 'Skipping test on macOS because glfw-cocoa.so is not built with backend_utils')
    def test_utf_8_strndup(self):
        import ctypes

        from kitty.constants import glfw_path

        backend_utils = glfw_path('x11')
        lib = ctypes.CDLL(backend_utils)
        libc = ctypes.CDLL(None)

        class allocated_c_char_p(ctypes.c_char_p):
            def __del__(self):
                libc.free(self)

        utf_8_strndup = lib.utf_8_strndup
        utf_8_strndup.restype = allocated_c_char_p
        utf_8_strndup.argtypes = (ctypes.c_char_p, ctypes.c_size_t)

        def test(string):
            string_bytes = bytes(string, 'utf-8')
            prev_part_bytes = b''
            prev_length_bytes = -1
            for length in range(len(string) + 1):
                part = string[:length]
                part_bytes = bytes(part, 'utf-8')
                length_bytes = len(part_bytes)
                for length_bytes_2 in range(prev_length_bytes + 1, length_bytes):
                    self.ae(utf_8_strndup(string_bytes, length_bytes_2).value, prev_part_bytes)
                self.ae(utf_8_strndup(string_bytes, length_bytes).value, part_bytes)
                prev_part_bytes = part_bytes
                prev_length_bytes = length_bytes
            self.ae(utf_8_strndup(string_bytes, len(string_bytes) + 1).value, string_bytes)  # Try to go one character after the end of the string

        self.ae(utf_8_strndup(None, 2).value, None)
        self.ae(utf_8_strndup(b'', 2).value, b'')

        test('ö')
        test('>a<')
        test('>ä<')
        test('>ế<')
        test('>𐍈<')
        test('∮ E⋅da = Q,  n → ∞, 𐍈∑ f(i) = ∏ g(i)')
        self.ae(utf_8_strndup(b'\xf0\x9f\x98\xb8', 3).value, b'')
        self.ae(utf_8_strndup(b'\xc3\xb6\xf0\x9f\x98\xb8', 4).value, b'\xc3\xb6')

    @unittest.skipIf(is_macos, 'glfw-cocoa.so is not built with backend_utils, see test_cocoa_timer_table_grows')
    def test_timer_table_grows(self):
        # The timer table of the Linux backends used to be a fixed array of 128 entries
        import ctypes

        from kitty.constants import glfw_path

        n = 1000
        for module in ('x11', 'wayland'):
            path = glfw_path(module)
            if not os.path.exists(path):
                continue
            with self.subTest(module=module):
                lib = ctypes.CDLL(path)
                cb_type = ctypes.CFUNCTYPE(None, ctypes.c_ulonglong, ctypes.c_void_p)
                lib.addTimer.restype = ctypes.c_ulonglong
                lib.addTimer.argtypes = (
                    ctypes.c_void_p, ctypes.c_char_p, ctypes.c_int64, ctypes.c_int, ctypes.c_bool, cb_type, ctypes.c_void_p, cb_type)
                lib.dispatchTimers.restype = ctypes.c_size_t
                lib.dispatchTimers.argtypes = (ctypes.c_void_p,)
                lib.removeTimer.argtypes = (ctypes.c_void_p, ctypes.c_ulonglong)
                lib.removeAllTimers.argtypes = (ctypes.c_void_p,)
                # Zeroed storage for an EventLoopData, which is far smaller than this
                eld = ctypes.create_string_buffer(1 << 20)
                fired: list[int] = []
                freed: list[int] = []
                added_in_callback: list[int] = []

                def on_timer(timer_id: int, data: int | None) -> None:
                    fired.append(data or 0)
                    if data == 1:
                        # Grow the table from inside a callback, while timers are being dispatched
                        added_in_callback.extend(add(n + 1 + i) for i in range(n // 2))

                def on_free(timer_id: int, data: int | None) -> None:
                    freed.append(data or 0)

                cb, free_cb = cb_type(on_timer), cb_type(on_free)

                def add(data: int, interval: int = 0, repeats: bool = False) -> int:
                    return lib.addTimer(eld, b'test', interval, 1, repeats, cb, data, free_cb)

                ids = [add(i + 1) for i in range(n)]
                self.assertNotIn(0, ids)
                self.ae(len(set(ids)), n)
                self.ae(lib.dispatchTimers(eld), n)
                self.ae(sorted(fired), list(range(1, n + 1)))
                self.ae(sorted(freed), list(range(1, n + 1)))
                self.ae(len(added_in_callback), n // 2)
                self.assertNotIn(0, added_in_callback)
                fired.clear(), freed.clear()
                self.ae(lib.dispatchTimers(eld), n // 2)
                self.ae(sorted(fired), list(range(n + 1, n + 1 + n // 2)))
                self.ae(sorted(freed), sorted(fired))
                # Timers that are not due are kept, and can be removed one at a time or all at once
                fired.clear(), freed.clear()
                one_hour = 3600 * 10**9
                ids = [add(i + 1, one_hour, True) for i in range(n)]
                self.assertNotIn(0, ids)
                self.ae(lib.dispatchTimers(eld), 0)
                for timer_id in ids[: n // 2]:
                    lib.removeTimer(eld, timer_id)
                self.ae(sorted(freed), list(range(1, n // 2 + 1)))
                lib.removeAllTimers(eld)
                self.ae(sorted(freed), list(range(1, n + 1)))
                self.ae(fired, [])

    @unittest.skipUnless(is_macos, 'macOS only')
    def test_cocoa_timer_table_grows(self):
        # The timer table of the Cocoa backend used to be a fixed array of 128 entries.
        # Run in a child, since this loads glfw-cocoa.so and runs a run loop.
        import subprocess

        from kitty.constants import kitty_exe

        p = subprocess.run([kitty_exe(), '+runpy', COCOA_TIMERS_SCRIPT], capture_output=True, text=True, timeout=60)
        self.assertEqual(p.returncode, 0, p.stderr)
        self.assertIn('ok', p.stdout, p.stderr)


COCOA_TIMERS_SCRIPT = '''
def main():
    # +runpy executes this with separate globals and locals, so everything lives in
    # here, and with -OO, so check() is used instead of assert
    import ctypes
    import time

    from kitty.constants import glfw_path

    n = 1000
    lib = ctypes.CDLL(glfw_path('cocoa'))
    cf = ctypes.CDLL('/System/Library/Frameworks/CoreFoundation.framework/CoreFoundation')
    cf.CFRunLoopRunInMode.restype = ctypes.c_int32
    cf.CFRunLoopRunInMode.argtypes = (ctypes.c_void_p, ctypes.c_double, ctypes.c_bool)
    default_mode = ctypes.c_void_p.in_dll(cf, 'kCFRunLoopDefaultMode')
    cb_type = ctypes.CFUNCTYPE(None, ctypes.c_ulonglong, ctypes.c_void_p)
    lib.glfwAddTimer.restype = ctypes.c_ulonglong
    lib.glfwAddTimer.argtypes = (ctypes.c_int64, ctypes.c_bool, cb_type, ctypes.c_void_p, cb_type)
    lib.glfwRemoveTimer.argtypes = (ctypes.c_ulonglong,)
    fired, freed, added_in_callback = [], [], []

    def check(ok, *msg):
        if not ok:
            raise SystemExit(f'check failed: {msg}')

    def on_timer(timer_id, data):
        fired.append(data or 0)
        if data == 1:
            added_in_callback.extend(add(n + 1 + i) for i in range(n // 2))

    def on_free(timer_id, data):
        freed.append(data or 0)

    cb, free_cb = cb_type(on_timer), cb_type(on_free)

    def add(data, interval=0, repeats=False):
        return lib.glfwAddTimer(interval, repeats, cb, data, free_cb)

    def run_until(pred):
        deadline = time.monotonic() + 30
        while not pred() and time.monotonic() < deadline:
            cf.CFRunLoopRunInMode(default_mode, 0.05, False)
        check(pred(), (len(fired), len(freed)))

    ids = [add(i + 1) for i in range(n)]
    check(0 not in ids and len(set(ids)) == n)
    total = n + n // 2
    run_until(lambda: len(fired) >= total and len(freed) >= total)
    check(sorted(fired) == list(range(1, total + 1)), sorted(fired)[:10])
    check(sorted(freed) == sorted(fired))
    check(len(added_in_callback) == n // 2 and 0 not in added_in_callback)
    fired.clear(), freed.clear()
    ids = [add(i + 1, 3600 * 10**9, True) for i in range(n)]
    check(0 not in ids)
    for timer_id in ids:
        lib.glfwRemoveTimer(timer_id)
    check(sorted(freed) == list(range(1, n + 1)) and not fired)
    print('ok', flush=True)


main()
'''
