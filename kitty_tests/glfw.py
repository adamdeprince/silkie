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

    def test_wayland_xdg_geometry_backoff(self):
        # The schedule on which glfw asks a Wayland compositor again for the
        # xdg_output geometry it did not send after one roundtrip, see
        # glfw/wl_monitor.c
        import ctypes
        import os

        from kitty.constants import glfw_path

        path = glfw_path('wayland')
        if is_macos or not os.path.exists(path):
            self.skipTest('The Wayland backend is not built on this platform')
        lib = ctypes.CDLL(path)
        backoff = lib._glfwTestXdgGeometryBackoff
        backoff.restype = ctypes.c_int
        backoff.argtypes = (ctypes.c_int, ctypes.POINTER(ctypes.c_longlong), ctypes.c_size_t, ctypes.POINTER(ctypes.c_size_t))
        PENDING, GAVE_UP, REPLACED = 1, 2, 4

        def run(arrive_after_timers, capacity=32):
            delays = (ctypes.c_longlong * max(capacity, 1))()
            n = ctypes.c_size_t()
            state = backoff(arrive_after_timers, delays, capacity, ctypes.byref(n))
            return state, list(delays[: min(n.value, capacity)]), n.value

        full_schedule = [50, 100, 200, 400, 800, 1600, 3200, 6400]
        # Never answered: 8 requests in all, each waited for twice as long as
        # the previous one, then give up and stay pending on fallback values
        self.ae(run(-1), (PENDING | GAVE_UP, full_schedule, 8))
        # Answered after the first request was retried twice
        self.ae(run(2), (REPLACED, full_schedule[:3], 3))
        # Answered before any retry
        self.ae(run(0), (REPLACED, full_schedule[:1], 1))
        # Answered spontaneously after giving up still replaces the fallback
        self.ae(run(100), (REPLACED, full_schedule, 8))
        # A short array gets what fits and the count of all
        self.ae(run(-1, capacity=3), (PENDING | GAVE_UP, full_schedule[:3], 8))
    @unittest.skipIf(is_macos, 'glfw-cocoa.so is not built with backend_utils, see test_cocoa_timer_table_grows')
    def test_event_loop_tables_grow(self):
        # The timer and watch tables of the Linux backends used to be fixed arrays of 128 and 64 entries
        import ctypes
        import resource

        from kitty.constants import glfw_path

        num_timers, num_watches = 1000, 500
        # Each watch is a pipe and a callback adds half as many again
        soft, hard = resource.getrlimit(resource.RLIMIT_NOFILE)
        needed = 3 * num_watches + 256
        if soft != resource.RLIM_INFINITY and soft < needed:
            if hard != resource.RLIM_INFINITY and hard < needed:
                self.skipTest(f'The limit on open files, {hard}, is too low for {num_watches} watched pipes')
            resource.setrlimit(resource.RLIMIT_NOFILE, (needed, hard))
            self.addCleanup(resource.setrlimit, resource.RLIMIT_NOFILE, (soft, hard))
        for module in ('x11', 'wayland'):
            path = glfw_path(module)
            if not os.path.exists(path):
                continue
            with self.subTest(module=module):
                f = ctypes.CDLL(path)._glfwTestEventLoopTables
                f.restype = ctypes.c_char_p
                f.argtypes = (ctypes.c_size_t, ctypes.c_size_t)
                err = f(num_timers, num_watches)
                self.assertIsNone(err, (err or b'').decode())

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
    lib.glfwUpdateTimerWithTolerance.argtypes = (ctypes.c_ulonglong, ctypes.c_int64, ctypes.c_int64, ctypes.c_bool)
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

    fired.clear(), freed.clear()
    timer_id = add(2, 3600 * 10**9)
    lib.glfwUpdateTimerWithTolerance(timer_id, 0, 10**7, False)
    cf.CFRunLoopRunInMode(default_mode, 0.05, False)
    check(not fired)
    lib.glfwUpdateTimerWithTolerance(timer_id, 0, 10**7, True)
    run_until(lambda: fired and freed)
    check(fired == freed == [2], fired, freed)
    print('ok', flush=True)


main()
'''
