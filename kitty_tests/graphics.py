#!/usr/bin/env python
# License: GPL v3 Copyright: 2016, Kovid Goyal <kovid at kovidgoyal.net>

import os
import random
import tempfile
import time
import unittest
import zlib
from contextlib import suppress
from dataclasses import dataclass
from io import BytesIO

from kitty.fast_data_types import base64_decode, base64_encode, load_png_data, shm_unlink, shm_write

from .base import BaseTest, parse_bytes

try:
    from PIL import Image
except ImportError:
    Image = None
png_data = base64_decode('iVBORw0KGgoAAAANSUhEUgAAAAEAAAABCAYAAAAfFcSJAAAADUlEQVR42mNk+P+/HgAFhAJ/wlseKgAAAABJRU5ErkJggg==')


def num_open_fds():
    with suppress(OSError):
        return len(os.listdir('/proc/self/fd'))
    return len(os.listdir('/dev/fd'))


def send_command(screen, cmd, payload=b''):
    cmd = '\033_G' + cmd
    if payload:
        if isinstance(payload, str):
            payload = payload.encode('utf-8')
        payload = base64_encode(payload).decode('ascii')
        cmd += ';' + payload
    cmd += '\033\\'
    c = screen.callbacks
    c.clear()
    parse_bytes(screen, cmd.encode('ascii'))
    return c.wtcbuf


def parse_response(res):
    if not res:
        return
    return res.decode('ascii').partition(';')[2].partition('\033')[0]


def parse_response_with_ids(res):
    if not res:
        return
    a, b = res.decode('ascii').split(';', 1)
    code = b.partition('\033')[0].split(':', 1)[0]
    a = a.split('G', 1)[1]
    return code, a


@dataclass(frozen=True)
class Response:
    code: str = 'OK'
    msg: str = ''
    image_id: int = 0
    image_number: int = 0
    frame_number: int = 0


def parse_full_response(res):
    if not res:
        return
    a, b = res.decode('ascii').split(';', 1)
    code = b.partition('\033')[0].split(':', 1)
    if len(code) == 1:
        code = code[0]
        msg = ''
    else:
        code, msg = code
    a = a.split('G', 1)[1]
    ans = {'code': code, 'msg': msg}
    for x in a.split(','):
        k, _, v = x.partition('=')
        ans[{'i': 'image_id', 'I': 'image_number', 'r': 'frame_number'}[k]] = int(v)
    return Response(**ans)


all_bytes = bytes(bytearray(range(256)))


def byte_block(sz):
    d, m = divmod(sz, len(all_bytes))
    return (all_bytes * d) + all_bytes[:m]


def load_helpers(self):
    s = self.create_screen()
    g = s.grman

    def pl(payload, **kw):
        kw.setdefault('i', 1)
        cmd = ','.join(f'{k}={v}' for k, v in kw.items())
        res = send_command(s, cmd, payload)
        return parse_response(res)

    def sl(payload, **kw):
        if isinstance(payload, str):
            payload = payload.encode('utf-8')
        data = kw.pop('expecting_data', payload)
        cid = kw.setdefault('i', 1)
        self.ae('OK', pl(payload, **kw))
        img = g.image_for_client_id(cid)
        self.assertIsNotNone(img, f'No image with id {cid} found')
        self.ae(img['client_id'], cid)
        self.ae(img['data'], data)
        if 's' in kw:
            self.ae((kw['s'], kw['v']), (img['width'], img['height']))
        self.ae(img['is_4byte_aligned'], kw.get('f') != 24)
        return img

    return s, g, pl, sl


def put_helpers(self, cw, ch, cols=10, lines=5):
    iid = 0

    def create_screen():
        s = self.create_screen(cols, lines, cell_width=cw, cell_height=ch)
        return s, 2 / s.columns, 2 / s.lines

    def put_cmd(
        z=0,
        num_cols=0,
        num_lines=0,
        x_off=0,
        y_off=0,
        width=0,
        height=0,
        cell_x_off=0,
        cell_y_off=0,
        placement_id=0,
        cursor_movement=0,
        unicode_placeholder=0,
        parent_id=0,
        parent_placement_id=0,
        offset_from_parent_x=0,
        offset_from_parent_y=0,
    ):
        return (
            f'z={z},c={num_cols},r={num_lines},x={x_off},y={y_off},w={width},h={height},'
            f'X={cell_x_off},Y={cell_y_off},p={placement_id},C={cursor_movement},'
            f'U={unicode_placeholder},P={parent_id},Q={parent_placement_id},'
            f'H={offset_from_parent_x},V={offset_from_parent_y}'
        )

    def put_image(screen, w, h, **kw):
        nonlocal iid
        iid += 1
        imgid = kw.pop('id', None) or iid
        no_id = kw.pop('no_id', False)
        a = kw.pop('a', 'T')
        if no_id:
            cmd = f'a={a},f=24,s=%d,v=%d,%s' % (w, h, put_cmd(**kw))
        else:
            cmd = f'a={a},f=24,i=%d,s=%d,v=%d,%s' % (imgid, w, h, put_cmd(**kw))
        data = b'x' * w * h * 3
        res = send_command(screen, cmd, data)
        return imgid, parse_response(res)

    def put_ref(screen, **kw):
        imgid = kw.pop('id', None) or iid
        cmd = 'a=p,i=%d,%s' % (imgid, put_cmd(**kw))
        return imgid, parse_response_with_ids(send_command(screen, cmd))

    def layers(screen, scrolled_by=0, xstart=-1, ystart=1):
        return screen.grman.update_layers(scrolled_by, xstart, ystart, dx, dy, screen.columns, screen.lines, cw, ch)

    def rect_eq(r, left, top, right, bottom):
        for side in 'left top right bottom'.split():
            a, b = r[side], locals()[side]
            if abs(a - b) > 0.0001:
                self.ae(a, b, 'the %s side is not equal' % side)

    s, dx, dy = create_screen()
    return s, dx, dy, put_image, put_ref, layers, rect_eq


def make_send_command(screen):
    def li(payload='abcdefghijkl' * 3, s=4, v=3, f=24, a='f', i=1, **kw):
        if s:
            kw['s'] = s
        if v:
            kw['v'] = v
        if f:
            kw['f'] = f
        if i:
            kw['i'] = i
        kw['a'] = a
        cmd = ','.join(f'{k}={v}' for k, v in kw.items())
        res = send_command(screen, cmd, payload)
        return parse_full_response(res)

    return li


class TestGraphics(BaseTest):
    def test_disk_cache(self):
        s = self.create_screen()
        dc = s.grman.disk_cache
        dc.small_hole_threshold = 0
        data = {}

        def key_as_bytes(key):
            if isinstance(key, int):
                key = str(key)
            if isinstance(key, str):
                key = key.encode('utf-8')
            return bytes(key)

        def add(key, val):
            bkey = key_as_bytes(key)
            data[key] = key_as_bytes(val)
            dc.add(bkey, data[key])

        def remove(key):
            bkey = key_as_bytes(key)
            data.pop(key, None)
            return dc.remove(bkey)

        def check_data():
            for key, val in data.items():
                self.ae(dc.get(key_as_bytes(key)), val)

        def reset(small_hole_threshold=0, defrag_factor=2):
            nonlocal dc, data, s
            s = self.create_screen()
            dc = s.grman.disk_cache
            dc.small_hole_threshold = small_hole_threshold
            dc.defrag_factor = defrag_factor
            data = {}

        holes_to_create = 2, 4, 6, 8
        for i in range(25):
            self.assertIsNone(add(i, f'{i}' * i))
            if i <= max(holes_to_create):
                # We wait here to ensure data is written in order, otherwise the
                # holes test below can fail
                self.assertTrue(dc.wait_for_write())

        self.assertEqual(dc.total_size, sum(map(len, data.values())))
        self.assertTrue(dc.wait_for_write())
        check_data()
        sz = dc.end_of_data_offset()
        self.assertEqual(sz, sum(map(len, data.values())))
        self.assertFalse(dc.holes())
        holes = set()
        for x in holes_to_create:
            remove(x)
            holes.add(x)
            check_data()
            self.assertRaises(KeyError, dc.get, key_as_bytes(x))
            self.assertEqual(sz, dc.end_of_data_offset())
            self.assertEqual(holes, {x[1] for x in dc.holes()})
        self.assertEqual(sz, dc.end_of_data_offset())
        # fill holes largest first to ensure small one doesn't go into large accidentally causing fragmentation
        for i, x in enumerate(sorted(holes, reverse=True)):
            x = 'ABCDEFGH'[i] * x
            add(x, x)
            self.assertTrue(dc.wait_for_write())
            check_data()
            holes.discard(len(x))
            self.assertEqual(holes, {x[1] for x in dc.holes()})
            self.assertEqual(sz, dc.end_of_data_offset(), f'Disk cache has unexpectedly grown from {sz} to {dc.end_of_data_offset()} with data: {x!r}')
        check_data()
        dc.clear()
        self.assertEqual(dc.end_of_data_offset(), 0)

        data.clear()
        for i in range(25):
            self.assertIsNone(add(i, f'{i}' * i))
        dc.wait_for_write()
        check_data()

        before = dc.end_of_data_offset()
        while dc.total_size > before // 3:
            key = random.choice(tuple(data))
            self.assertTrue(remove(key))
        check_data()
        add('trigger defrag', 'XXX')
        dc.wait_for_write()
        self.assertLess(dc.end_of_data_offset(), before)
        check_data()
        dc.clear()

        st = time.monotonic()
        while dc.end_of_data_offset() and time.monotonic() - st < 20:
            time.sleep(0.01)
        self.assertEqual(dc.end_of_data_offset(), 0)
        for frame in range(32):
            add(f'1:{frame}', f'{frame:02d}' * 8)
        dc.wait_for_write()
        self.assertEqual(dc.end_of_data_offset(), 32 * 16)
        self.assertEqual(dc.num_cached_in_ram(), 0)
        num_in_ram = 0
        for frame in range(32):
            dc.get(key_as_bytes(f'1:{frame}'))
        self.assertEqual(dc.num_cached_in_ram(), num_in_ram)
        for frame in range(32):
            dc.get(key_as_bytes(f'1:{frame}'), True)
            num_in_ram += 1
        self.assertEqual(dc.num_cached_in_ram(), num_in_ram)

        def clear_predicate(key):
            return key.startswith(b'1:')

        dc.remove_from_ram(clear_predicate)
        self.assertEqual(dc.num_cached_in_ram(), 0)

        reset(small_hole_threshold=512, defrag_factor=20)
        self.assertIsNone(add(1, '1' * 1024))
        self.assertIsNone(add(2, '2' * 1024))
        dc.wait_for_write()
        sz = dc.end_of_data_offset()
        remove(1)
        self.ae(sz, dc.end_of_data_offset())
        self.ae({x[1] for x in dc.holes()}, {1024})
        self.assertIsNone(add(3, '3' * 800))
        dc.wait_for_write()
        self.assertFalse(dc.holes())
        self.ae(sz, dc.end_of_data_offset())
        self.assertIsNone(add(4, '4' * 100))
        sz += 100
        dc.wait_for_write()
        self.ae(sz, dc.end_of_data_offset())
        check_data()
        self.assertFalse(dc.holes())
        remove(4)
        self.assertFalse(dc.holes())
        self.assertIsNone(add(5, '5' * 10))
        sz += 10
        dc.wait_for_write()
        self.ae(sz, dc.end_of_data_offset())

        # test hole coalescing
        reset(defrag_factor=20)
        for i in range(1, 6):
            self.assertIsNone(add(i, str(i) * i))
            dc.wait_for_write()
        remove(2)
        remove(4)
        self.assertEqual(dc.holes(), {(1, 2), (6, 4)})
        remove(3)
        self.assertEqual(dc.holes(), {(1, 9)})

    def test_disk_cache_entry_changed_while_being_written(self):
        # The disk cache write thread releases the lock while writing an entry
        # to disk, so the entry can be replaced or removed in the meantime. The
        # data written for it is then stale and must neither be associated with
        # the entry nor be allowed to leak space in the cache file.
        s = self.create_screen()
        dc = s.grman.disk_cache
        dc.small_hole_threshold = 0

        # Replaced while being written
        dc.pause_writes()
        dc.add(b'k1', b'a' * 100)
        self.assertTrue(dc.wait_until_writes_paused())
        dc.add(b'k1', b'b' * 200)
        self.assertTrue(dc.resume_writes())
        self.assertTrue(dc.wait_for_write())
        # The new data must have been written to disk rather than the entry
        # being marked as written at the position of the old data
        self.assertEqual(dc.num_cached_in_ram(), 0)
        self.assertEqual(dc.get(b'k1'), b'b' * 200)
        self.assertEqual(dc.total_size, 200)
        # The space used by the stale data must have been reclaimed
        self.assertEqual(dc.holes(), {(0, 100)})
        self.assertEqual(dc.end_of_data_offset(), 300)

        # Removed while being written
        dc.pause_writes()
        dc.add(b'k2', b'c' * 50)
        self.assertTrue(dc.wait_until_writes_paused())
        self.assertTrue(dc.remove(b'k2'))
        self.assertTrue(dc.resume_writes())
        self.assertTrue(dc.wait_for_write())
        self.assertRaises(KeyError, dc.get, b'k2')
        self.assertEqual(dc.total_size, 200)
        # k2 was written into the existing 100 byte hole, both the 50 bytes it
        # used and the 50 byte remainder must be reclaimed and coalesced
        self.assertEqual(dc.holes(), {(0, 100)})
        self.assertEqual(dc.end_of_data_offset(), 300)
        # The untouched entry must be unaffected throughout
        self.assertEqual(dc.get(b'k1'), b'b' * 200)

    def test_suppressing_gr_command_responses(self):
        s, g, pl, sl = load_helpers(self)
        self.ae(pl('abcd', s=10, v=10, q=1), 'ENODATA:Insufficient image data: 4 < 400')
        self.ae(pl('abcd', s=10, v=10, q=2), None)
        self.assertIsNone(pl('abcd', s=1, v=1, a='q', q=1))
        # Test chunked load
        self.assertIsNone(pl('abcd', s=2, v=2, m=1, q=1))
        self.assertIsNone(pl('efgh', m=1))
        self.assertIsNone(pl('ijkl', m=1))
        self.assertIsNone(pl('mnop', m=0))

        # errors
        self.assertIsNone(pl('abcd', s=2, v=2, m=1, q=1))
        self.ae(pl('mnop', m=0), 'ENODATA:Insufficient image data: 8 < 16')
        self.assertIsNone(pl('abcd', s=2, v=2, m=1, q=2))
        self.assertIsNone(pl('mnop', m=0))

        # frames
        s = self.create_screen()
        li = make_send_command(s)
        self.assertEqual(li().code, 'ENOENT')
        self.assertIsNone(li(q=2))
        self.assertIsNone(li(a='t', q=1))
        self.assertIsNone(li(payload='2' * 12, z=77, m=1, q=1))
        self.assertIsNone(li(payload='2' * 12, m=1))
        self.assertIsNone(li(payload='2' * 12))
        self.assertIsNone(li(payload='2' * 12, z=77, m=1, q=1))
        self.ae(li(payload='2' * 12).code, 'ENODATA')
        self.assertIsNone(li(payload='2' * 12, z=77, m=1, q=2))
        self.assertIsNone(li(payload='2' * 12))

    def test_transient_graphics_image(self):
        s, g, pl, sl = load_helpers(self)
        self.assertEqual(g.disk_cache.end_of_data_offset(), 0)
        self.ae(pl('abc', s=1, v=1, f=24, N=1), 'OK')
        self.assertTrue(g.disk_cache.wait_for_write())
        self.assertEqual(g.disk_cache.end_of_data_offset(), 0)
        img = g.image_for_client_id(1)
        self.assertIsNotNone(img)
        self.ae(img['data'], b'abc')

        self.ae(pl('def', s=1, v=1, f=24, i=2), 'OK')
        self.assertTrue(g.disk_cache.wait_for_write())
        self.assertGreater(g.disk_cache.end_of_data_offset(), 0)

    def test_load_images(self):
        s, g, pl, sl = load_helpers(self)
        self.assertEqual(g.disk_cache.total_size, 0)

        # Test load query
        self.ae(pl('abcd', s=1, v=1, a='q'), 'OK')
        self.ae(g.image_count, 0)

        # Test simple load
        for f in 32, 24:
            p = 'abc' + ('d' if f == 32 else '')
            img = sl(p, s=1, v=1, f=f)
            self.ae(bool(img['is_4byte_aligned']), f == 32)

        # Test chunked load
        self.assertIsNone(pl('abcd', s=2, v=2, m=1))
        self.assertIsNone(pl('efgh', m=1))
        self.assertIsNone(pl('ijkl', m=1))
        self.ae(pl('mnop', m=0), 'OK')
        img = g.image_for_client_id(1)
        self.ae(img['data'], b'abcdefghijklmnop')

        # Test interrupted and retried chunked load
        self.assertIsNone(pl('abcd', s=2, v=2, m=1))
        self.assertIsNone(pl('efgh', m=1))
        send_command(s, 'a=d')  # delete command should clear partial transfer
        self.assertIsNone(pl('abcd', s=2, v=2, m=1))
        self.assertIsNone(pl('efgh', m=1))
        self.assertIsNone(pl('ijkl', m=1))
        self.ae(pl('1234', m=0), 'OK')
        img = g.image_for_client_id(1)
        self.ae(img['data'], b'abcdefghijkl1234')

        random_data = byte_block(32 * 1024)
        sl(random_data, s=1024, v=8, expecting_data=random_data)

        # Test compression
        compressed_random_data = zlib.compress(random_data)
        sl(compressed_random_data, s=1024, v=8, o='z', expecting_data=random_data)

        # Test chunked + compressed
        b = len(compressed_random_data) // 2
        self.assertIsNone(pl(compressed_random_data[:b], s=1024, v=8, o='z', m=1))
        self.ae(pl(compressed_random_data[b:], m=0), 'OK')
        img = g.image_for_client_id(1)
        self.ae(img['data'], random_data)

        # Test loading from file
        def load_temp(prefix='tty-graphics-protocol-'):
            f = tempfile.NamedTemporaryFile(prefix=prefix)
            f.write(random_data), f.flush()
            sl(f.name, s=1024, v=8, t='f', expecting_data=random_data)
            self.assertTrue(os.path.exists(f.name))
            f.seek(0), f.truncate(), f.write(compressed_random_data), f.flush()
            sl(f.name, s=1024, v=8, t='t', o='z', expecting_data=random_data)
            return f

        f = load_temp()
        self.assertFalse(os.path.exists(f.name), f'Temp file at {f.name} was not deleted')
        with suppress(FileNotFoundError):
            f.close()
        f = load_temp('')
        self.assertTrue(os.path.exists(f.name), f'Temp file at {f.name} was deleted')
        f.close()

        # Test loading from POSIX SHM
        name = '/kitty-test-shm'
        shm_write(name, random_data)
        sl(name, s=1024, v=8, t='s', expecting_data=random_data)
        self.assertRaises(FileNotFoundError, shm_unlink, name)  # check that file was deleted
        s.reset()
        self.assertEqual(g.disk_cache.total_size, 0)

    def test_load_images_from_file_edge_cases(self):
        s, g, pl, sl = load_helpers(self)
        random_data = byte_block(32 * 1024)
        # Failures to read an image file must all be reported with the same
        # response, otherwise a client, which can be a program running on a
        # remote machine or in a sandbox, can use the response to probe the
        # filesystem for the existence, type and size of files.
        generic_error = 'EBADF:Failed to read image file'

        with tempfile.NamedTemporaryFile(prefix='tty-graphics-protocol-') as f:
            # A window of the file specified with a non page aligned offset
            f.write(b'x' * 3 + random_data + b'y' * 5), f.flush()
            sl(f.name, s=1024, v=8, t='f', S=len(random_data), O=3, expecting_data=random_data)

            # A file that is truncated after the size declared in the command
            # must be reported as a generic failure rather than crashing or
            # leaking the size of the file
            f.seek(0), f.truncate(), f.write(random_data[:128]), f.flush()
            self.ae(pl(f.name, s=1024, v=8, t='f', S=len(random_data)), generic_error)

            # Ditto when the size is not declared and is read from the file itself
            self.ae(pl(f.name, s=1024, v=8, t='f'), generic_error)

            # An offset past the end of the file
            self.ae(pl(f.name, s=1024, v=8, t='f', O=4096), generic_error)

        # Only regular files may be read
        with tempfile.TemporaryDirectory(prefix='tty-graphics-protocol-') as tdir:
            fifo = os.path.join(tdir, 'fifo')
            os.mkfifo(fifo)
            self.ae(pl(fifo, s=1024, v=8, t='f'), generic_error, 'Reading from a FIFO was not refused')

            # Neither the existence nor the type of a file may be leaked, so a
            # non-existent file, a directory and a file that is too small must
            # all give byte for byte identical responses
            small = os.path.join(tdir, 'small')
            with open(small, 'wb') as sf:
                sf.write(random_data[:7])
            for path in (os.path.join(tdir, 'does-not-exist'), tdir, small):
                self.ae(pl(path, s=1024, v=8, t='f'), generic_error, path)

            # Failing to read a file must not leak the file descriptor opened
            # for it, else a client can exhaust the process wide fd limit
            paths = (small, tdir, os.path.join(tdir, 'does-not-exist'), fifo)
            for path in paths:
                pl(path, s=1024, v=8, t='f')  # warm up any lazily opened fds
            before = num_open_fds()
            for i in range(64):
                for path in paths:
                    pl(path, s=1024, v=8, t='f')
            self.ae(before, num_open_fds(), 'File descriptors were leaked when failing to read image files')

        # A window of a shared memory object with a non page aligned offset
        name = '/kitty-test-shm-offset'
        shm_write(name, b'x' * 3 + random_data + b'y' * 5)
        sl(name, s=1024, v=8, t='s', S=len(random_data), O=3, expecting_data=random_data)
        self.assertRaises(FileNotFoundError, shm_unlink, name)  # check that the object was deleted

        # A shared memory object truncated to less than the declared size
        name = '/kitty-test-shm-truncated'
        shm_write(name, random_data[:64])
        self.ae(pl(name, s=1024, v=8, t='s', S=len(random_data)), generic_error)
        self.assertRaises(FileNotFoundError, shm_unlink, name)  # check that the object was deleted

        # A shared memory object that could not be opened must also be
        # removed, and reported with the same generic error
        self.ae(pl('/kitty-test-shm-missing', s=1024, v=8, t='s'), generic_error)
        self.ae(pl('kitty-test-shm-no-leading-slash', s=1024, v=8, t='s'), generic_error)

        s.reset()
        self.assertEqual(g.disk_cache.total_size, 0)

    def test_graphics_file_reading_policy(self):
        from kitty.fast_data_types import set_boss
        from kitty.utils import is_ok_to_read_image_file, is_ok_to_read_image_path

        class Boss:
            def __init__(self):
                self.path_checks = []
                self.fd_checks = []

            def is_ok_to_read_image_path(self, path):
                self.path_checks.append(path)
                return is_ok_to_read_image_path(path)

            def is_ok_to_read_image_file(self, path, fd):
                self.fd_checks.append(path)
                return is_ok_to_read_image_file(path, fd)

            def safe_delete_temp_file(self, path):
                with suppress(FileNotFoundError):
                    os.remove(path)

        s, g, pl, sl = load_helpers(self)
        random_data = byte_block(32 * 1024)
        generic_error = 'EBADF:Failed to read image file'
        boss = Boss()
        set_boss(boss)
        try:
            # Files in protected locations must be refused based on their path
            # alone, without ever being opened, since both opening a file and
            # the error from a failed open leak its existence
            protected = ('/proc/self/cmdline', '/proc/does-not-exist', '/sys/kernel/does-not-exist', '/dev/null', '/dev/does-not-exist')
            for path in protected:
                self.ae(pl(path, s=1024, v=8, t='f'), generic_error, path)
            self.assertFalse(boss.fd_checks, 'A file in a protected location was opened before the policy was applied')
            self.ae(len(boss.path_checks), len(protected))

            # Refusing to read a file must not leak the fd opened for it
            before = num_open_fds()
            for i in range(64):
                for path in protected:
                    pl(path, s=1024, v=8, t='f')
            self.ae(before, num_open_fds(), 'File descriptors were leaked when refusing to read image files')

            # Allowed files are still readable
            with tempfile.NamedTemporaryFile() as f:
                f.write(random_data), f.flush()
                sl(f.name, s=1024, v=8, t='f', expecting_data=random_data)
                self.ae(boss.fd_checks[-1], f.name)
        finally:
            set_boss(None)
        s.reset()
        self.assertEqual(g.disk_cache.total_size, 0)

    @unittest.skipIf(Image is None, 'PIL not available, skipping PNG tests')
    def test_load_png(self):
        s, g, pl, sl = load_helpers(self)
        w, h = 5, 3
        rgba_data = byte_block(w * h * 4)
        img = Image.frombytes('RGBA', (w, h), rgba_data)
        rgb_data = img.convert('RGB').convert('RGBA').tobytes()
        self.assertEqual(g.disk_cache.total_size, 0)

        def png(mode='RGBA'):
            buf = BytesIO()
            i = img
            if mode != i.mode:
                i = img.convert(mode)
            i.save(buf, 'PNG')
            return buf.getvalue()

        for mode in 'RGBA RGB'.split():
            data = png(mode)
            sl(data, f=100, expecting_data=rgb_data if mode == 'RGB' else rgba_data)

        for m in 'LP':
            img = img.convert(m)
            rgba_data = img.convert('RGBA').tobytes()
            data = png(m)
        sl(data, f=100, expecting_data=rgba_data)

        self.ae(pl(b'a' * 20, f=100, S=20).partition(':')[0], 'EBADPNG')
        s.reset()
        self.assertEqual(g.disk_cache.total_size, 0)

    def test_load_png_simple(self):
        # 1x1 transparent PNG
        expected = b'\x00\xff\xff\x7f'
        self.ae(load_png_data(png_data), (expected, 1, 1))
        s, g, pl, sl = load_helpers(self)
        sl(png_data, f=100, expecting_data=expected)
        # test error handling for loading bad png data
        self.assertRaisesRegex(ValueError, '[EBADPNG]', load_png_data, b'dsfsdfsfsfd')
        # Test that a large PNG chunk sent via direct transmission doesn't crash
        # when its size exceeds 2 * initial_buf_capacity. Without the fix in
        # commit 48ab623, the buffer reallocation used MIN(2*buf_capacity, MAX_DATA_SZ)
        # which could leave buf_capacity smaller than the payload size, causing a
        # heap buffer overflow when the data was copied in.
        # Initial buf_capacity for PNG without an explicit S= is 10, so 2*10=20;
        # a 25-byte chunk previously caused a crash.
        res = pl(b'x' * 25, f=100)
        self.ae(res.partition(':')[0], 'EBADPNG')
        if Image is None:
            return
        # Test that a truncated PNG (valid header + IHDR declaring more rows
        # than the IDAT data can produce, with no trailing chunks) is rejected
        # cleanly instead of causing libpng to parse uninitialized memory. The
        # read callback must abort via png_error() when it runs out of bytes.
        w, h = 3, 3
        buf = BytesIO()
        Image.frombytes('RGBA', (w, h), byte_block(w * h * 4)).save(buf, 'PNG')
        full = buf.getvalue()
        # Drop the trailing bytes (IDAT tail + IEND) so the stream runs out
        # while libpng still expects more compressed data.
        truncated = full[:-16]
        self.assertRaisesRegex(ValueError, '[EBADPNG]', load_png_data, truncated)
        # Same, but with an embedded ICC profile so that lcms2 handles are
        # allocated before libpng longjmps out, they must be freed in the
        # error path (detected by LeakSanitizer in ASAN builds).
        try:
            from PIL import ImageCms
        except ImportError:
            return
        icc = ImageCms.ImageCmsProfile(ImageCms.createProfile('sRGB')).tobytes()
        buf = BytesIO()
        Image.frombytes('RGBA', (w, h), byte_block(w * h * 4)).save(buf, 'PNG', icc_profile=icc)
        self.assertRaisesRegex(ValueError, '[EBADPNG]', load_png_data, buf.getvalue()[:-16])

    def test_gr_operations_with_numbers(self):
        s = self.create_screen()
        g = s.grman
        self.assertEqual(g.disk_cache.total_size, 0)

        def li(payload, **kw):
            cmd = ','.join(f'{k}={v}' for k, v in kw.items())
            res = send_command(s, cmd, payload)
            return parse_response_with_ids(res)

        code, ids = li('abc', s=1, v=1, f=24, I=1, i=3)
        self.ae(code, 'EINVAL')

        code, ids = li('abc', s=1, v=1, f=24, I=1)
        self.ae((code, ids), ('OK', 'i=1,I=1'))
        img = g.image_for_client_number(1)
        self.ae(img['client_number'], 1)
        self.ae(img['client_id'], 1)
        code, ids = li('abc', s=1, v=1, f=24, I=1)
        self.ae((code, ids), ('OK', 'i=2,I=1'))
        img = g.image_for_client_number(1)
        self.ae(img['client_number'], 1)
        self.ae(img['client_id'], 2)
        code, ids = li('abc', s=1, v=1, f=24, I=1)
        self.ae((code, ids), ('OK', 'i=3,I=1'))
        code, ids = li('abc', s=1, v=1, f=24, i=5)
        self.ae((code, ids), ('OK', 'i=5'))
        code, ids = li('abc', s=1, v=1, f=24, I=3)
        self.ae((code, ids), ('OK', 'i=4,I=3'))

        # Test chunked load with number
        self.assertIsNone(li('abcd', s=2, v=2, m=1, I=93))
        self.assertIsNone(li('efgh', m=1))
        self.assertIsNone(li('ijkx', m=1))
        self.ae(li('mnop', m=0), ('OK', 'i=6,I=93'))
        img = g.image_for_client_number(93)
        self.ae(img['data'], b'abcdefghijkxmnop')
        self.ae(img['client_id'], 6)

        # test put with number
        def put(**kw):
            cmd = ','.join(f'{k}={v}' for k, v in kw.items())
            cmd = 'a=p,' + cmd
            return parse_response_with_ids(send_command(s, cmd))

        code, idstr = put(c=2, r=2, I=93)
        self.ae((code, idstr), ('OK', 'i=6,I=93'))
        code, idstr = put(c=2, r=2, I=94)
        self.ae(code, 'ENOENT')

        # test delete with number
        def delete(ac='N', **kw):
            cmd = 'a=d'
            if ac:
                cmd += f',d={ac}'
            if kw:
                cmd += ',' + ','.join(f'{k}={v}' for k, v in kw.items())
            send_command(s, cmd)

        count = s.grman.image_count
        put(i=1), put(i=2), put(i=3), put(i=4), put(i=5)
        delete(I=94)
        self.ae(s.grman.image_count, count)
        delete(I=93)
        self.ae(s.grman.image_count, count - 1)
        delete(I=1)
        self.ae(s.grman.image_count, count - 2)
        cn = 1117
        li('abc', s=1, v=1, f=24, I=cn)
        first_id = g.image_for_client_number(cn)['internal_id']
        li('abc', s=1, v=1, f=24, I=cn)
        second_id = g.image_for_client_number(cn)['internal_id']
        self.assertNotEqual(first_id, second_id)
        count = s.grman.image_count
        delete(I=cn)
        self.ae(g.image_for_client_number(cn)['internal_id'], first_id)
        self.ae(s.grman.image_count, count - 1)
        s.reset()
        self.assertEqual(g.disk_cache.total_size, 0)

    def test_image_put(self):
        cw, ch = 10, 20
        s, dx, dy, put_image, put_ref, layers, rect_eq = put_helpers(self, cw, ch)
        self.ae(put_image(s, cw, ch)[1], 'OK')
        l0 = layers(s)
        self.ae(len(l0), 1)
        rect_eq(l0[0]['src_rect'], 0, 0, 1, 1)
        rect_eq(l0[0]['dest_rect'], -1, 1, -1 + dx, 1 - dy)
        self.ae(l0[0]['group_count'], 1)
        self.ae(s.cursor.x, 1), self.ae(s.cursor.y, 0)
        src_width, src_height = 3, 5
        iid, (code, idstr) = put_ref(
            s, num_cols=s.columns, num_lines=1, x_off=2, y_off=1, width=src_width, height=src_height, cell_x_off=3, cell_y_off=1, z=-1, placement_id=17
        )
        self.ae(idstr, f'i={iid},p=17')
        l2 = layers(s)
        self.ae(len(l2), 2)
        self.ae(l2[1], l0[0])
        rect_eq(l2[0]['src_rect'], 2 / 10, 1 / 20, (2 + 3) / 10, (1 + 5) / 20)
        self.ae(l2[0]['group_count'], 2)
        left, top = -1 + dx + 3 * dx / cw, 1 - 1 * dy / ch
        right = -1 + (1 + s.columns) * dx
        bottom = 1 - dy
        rect_eq(l2[0]['dest_rect'], left, top, right, bottom)
        self.ae(s.cursor.x, 0), self.ae(s.cursor.y, 1)
        self.ae(put_image(s, 10, 20, cursor_movement=1)[1], 'OK')
        self.ae(s.cursor.x, 0), self.ae(s.cursor.y, 1)
        s.reset()
        self.assertEqual(s.grman.disk_cache.total_size, 0)
        self.ae(put_image(s, 2 * cw, 2 * ch, num_cols=3)[1], 'OK')
        self.ae((s.cursor.x, s.cursor.y), (3, 2))
        rect_eq(layers(s)[0]['dest_rect'], -1, 1, -1 + 3 * dx, 1 - 3 * dy)

    def test_graphics_put_with_pixel_offsets(self):
        cw, ch = 10, 20
        # Image 10x20 placed with 5px X and Y pixel offsets
        s, dx, dy, put_image, put_ref, layers, rect_eq = put_helpers(self, cw, ch)
        self.ae(put_image(s, 10, 20, cell_x_off=5, cell_y_off=5)[1], 'OK')
        self.ae((s.cursor.x, s.cursor.y), (2, 1))

    def test_image_layer_grouping(self):
        cw, ch = 10, 20
        s, dx, dy, put_image, put_ref, layers, rect_eq = put_helpers(self, cw, ch)

        def group_counts():
            return tuple(x['group_count'] for x in layers(s))

        self.ae(put_image(s, 10, 20, id=1)[1], 'OK')
        self.ae(group_counts(), (1,))
        put_ref(s, id=1, num_cols=2, num_lines=1, placement_id=2)
        put_ref(s, id=1, num_cols=2, num_lines=1, placement_id=3, z=-2)
        put_ref(s, id=1, num_cols=2, num_lines=1, placement_id=4, z=-2)
        self.ae(group_counts(), (4, 3, 2, 1))
        self.ae(put_image(s, 8, 16, id=2, z=-1)[1], 'OK')
        self.ae(group_counts(), (2, 1, 1, 2, 1))

    def test_image_parents(self):
        cw, ch = 10, 20
        iw, ih = 10, 20
        s, dx, dy, put_image, put_ref, layers, rect_eq = put_helpers(self, cw, ch)

        def positions():
            ans = {}

            def x(x):
                return round(((x + 1) / 2) * s.columns)

            def y(y):
                return int(((-y + 1) / 2) * s.lines)

            for i in layers(s):
                d = i['dest_rect']
                ans[(i['image_id'], i['ref_id'])] = {'x': x(d['left']), 'y': y(d['top'])}
            return ans

        def p(x, y=0):
            return {'x': x, 'y': y}

        self.ae(put_image(s, iw, ih, id=1)[1], 'OK')
        self.ae(put_ref(s, id=1, placement_id=1), (1, ('OK', 'i=1,p=1')))
        pos = {(1, 1): p(0), (1, 2): p(1)}
        self.ae(positions(), pos)
        # check that adding a reference to a non-existent parent fails
        self.ae(put_ref(s, id=1, placement_id=33, parent_id=1, parent_placement_id=2), (1, ('ENOPARENT', 'i=1,p=33')))
        self.ae(put_ref(s, id=1, placement_id=33, parent_id=33), (1, ('ENOPARENT', 'i=1,p=33')))
        # check that we cannot add a reference that is its own parent
        self.ae(put_ref(s, id=1, placement_id=1, parent_id=1, parent_placement_id=1), (1, ('EINVAL', 'i=1,p=1')))

        self.ae(put_image(s, iw, ih, id=2)[1], 'OK')
        pos[(2, 1)] = p(2)
        self.ae(positions(), pos)
        # Add two children to the first placement of img2
        before = s.cursor.x, s.cursor.y
        self.ae(put_ref(s, id=1, placement_id=2, parent_id=2, offset_from_parent_y=3), (1, ('OK', 'i=1,p=2')))
        self.ae(before, (s.cursor.x, s.cursor.y), 'Cursor must not move for child image')
        pos[(1, 3)] = p(2, 3)
        self.ae(positions(), pos)
        self.ae(put_ref(s, id=2, placement_id=3, parent_id=2, offset_from_parent_y=4), (2, ('OK', 'i=2,p=3')))
        pos[(2, 2)] = p(2, 4)
        self.ae(positions(), pos)
        # Add a grand child to the second child of img2
        self.ae(put_ref(s, id=2, placement_id=4, parent_id=2, parent_placement_id=3, offset_from_parent_x=-1), (2, ('OK', 'i=2,p=4')))
        pos[(2, 3)] = p(pos[(2, 2)]['x'] - 1, pos[(2, 2)]['y'])
        self.ae(positions(), pos)
        # Check that creating a cycle is prevented
        self.ae(put_ref(s, id=2, placement_id=3, parent_id=2, parent_placement_id=4), (2, ('ECYCLE', 'i=2,p=3')))
        self.ae(positions(), pos)
        # Check that depth is limited
        for i in range(5, 12):
            q = put_ref(s, id=2, placement_id=i, parent_id=2, parent_placement_id=i - 1, offset_from_parent_x=-1)[1][0]
            if q == 'ETOODEEP':
                break
            self.ae(q, 'OK')
        else:
            self.assertTrue(False, 'Failed to limit reference chain depth')
        # Check that deleting a parent removes all descendants
        send_command(s, 'a=d,d=i,i=2,p=3')
        pos.pop((2, 3)), pos.pop((2, 2))
        self.ae(positions(), pos)
        # Check that deleting a parent deletes all descendants and also removes
        # images with no remaining placements
        self.ae(put_ref(s, id=2, placement_id=3, parent_id=2, offset_from_parent_y=4), (2, ('OK', 'i=2,p=3')))
        pos[(2, 11)] = p(2, 4)
        self.ae(positions(), pos)
        self.ae(put_image(s, iw, ih, id=3, placement_id=97, parent_id=2, parent_placement_id=3)[1], 'OK')
        pos[(3, 1)] = p(2, 4)
        self.ae(positions(), pos)
        send_command(s, 'a=d,d=i,i=2')
        pos.pop((3, 1)), pos.pop((2, 11)), pos.pop((2, 1)), pos.pop((1, 3))
        self.ae(positions(), pos)
        # Check that virtual placements that try to be relative are rejected
        self.ae(put_ref(s, id=1, placement_id=11, parent_id=1, unicode_placeholder=1), (1, ('EINVAL', 'i=1,p=11')))
        # Check creation of children of a unicode placeholder based image
        s, dx, dy, put_image, put_ref, layers, rect_eq = put_helpers(self, cw, ch)
        put_image(s, 20, 20, num_cols=4, num_lines=2, unicode_placeholder=1, id=42)
        s.update_only_line_graphics_data()
        self.assertFalse(positions())  # the reference is virtual
        self.ae(put_ref(s, id=42, placement_id=11, parent_id=42, offset_from_parent_y=2, offset_from_parent_x=1), (42, ('OK', 'i=42,p=11')))
        self.assertFalse(positions())  # the reference is virtual without any cell images so the child is invisible
        s.apply_sgr('38;5;42')
        # These two characters will become one 2x1 ref.
        s.cursor.x = s.cursor.y = 1
        s.draw('\U0010eeee\u0305\u0305\U0010eeee\u0305\u030d')
        s.cursor.x = s.cursor.y = 0
        s.draw('\U0010eeee\u0305\u0305\U0010eeee\u0305\u030d')
        s.update_only_line_graphics_data()
        pos = {(1, 2): p(1, 2), (1, 3): p(0), (1, 4): p(1)}
        self.ae(positions(), pos)
        s.cursor.x = s.cursor.y = 0
        s.erase_in_display(0, False)
        s.update_only_line_graphics_data()
        self.assertFalse(positions())  # the reference is virtual without any cell images so the child is invisible
        s.cursor.x = s.cursor.y = 2
        s.draw('\U0010eeee\u0305\u0305\U0010eeee\u0305\u030d')
        s.update_only_line_graphics_data()
        self.ae(positions(), {(1, 5): {'x': 2, 'y': 2}, (1, 2): {'x': 3, 'y': 4}})

    def test_unicode_placeholders(self):
        # This test tests basic image placement using unicode placeholders
        cw, ch = 10, 20
        s, dx, dy, put_image, put_ref, layers, rect_eq = put_helpers(self, cw, ch)
        # Upload two images.
        put_image(s, 20, 20, num_cols=4, num_lines=2, unicode_placeholder=1, id=42)
        put_image(s, 10, 20, num_cols=4, num_lines=2, unicode_placeholder=1, id=(42 << 16) + (43 << 8) + 44)
        # The references are virtual, so no visible refs yet.
        s.update_only_line_graphics_data()
        refs = layers(s)
        self.ae(len(refs), 0)
        # A reminder of row/column diacritics meaning (assuming 0-based):
        # \u0305 -> 0
        # \u030D -> 1
        # \u030E -> 2
        # \u0310 -> 3
        # Now print the placeholders for the first image.
        # Encode the id as an 8-bit color.
        s.apply_sgr('38;5;42')
        # These two characters will become one 2x1 ref.
        s.draw('\U0010eeee\u0305\u0305\U0010eeee\u0305\u030d')
        # These two characters will be two separate refs (not contiguous).
        s.draw('\U0010eeee\u0305\u0305\U0010eeee\u0305\u030e')
        s.cursor_move(4)
        s.update_only_line_graphics_data()
        refs = layers(s)
        self.ae(len(refs), 3)
        self.ae(refs[0]['src_rect'], {'left': 0.0, 'top': 0.0, 'right': 0.5, 'bottom': 0.5})
        self.ae(refs[1]['src_rect'], {'left': 0.0, 'top': 0.0, 'right': 0.25, 'bottom': 0.5})
        self.ae(refs[2]['src_rect'], {'left': 0.5, 'top': 0.0, 'right': 0.75, 'bottom': 0.5})
        # Erase the line.
        s.erase_in_line(2)
        # There must be 0 refs after the line is erased.
        s.update_only_line_graphics_data()
        refs = layers(s)
        self.ae(len(refs), 0)
        # Now test encoding IDs with the 24-bit color.
        # The first image, 1x1
        s.apply_sgr('38;2;0;0;42')
        s.draw('\U0010eeee\u0305\u0305')
        # The second image, 2x1
        s.apply_sgr('38;2;42;43;44')
        s.draw('\U0010eeee\u0305\u030d\U0010eeee\u0305\u030e')
        s.cursor_move(2)
        s.update_only_line_graphics_data()
        refs = layers(s)
        self.ae(len(refs), 2)
        self.ae(refs[0]['src_rect'], {'left': 0.0, 'top': 0.0, 'right': 0.25, 'bottom': 0.5})
        # The second ref spans the whole widths of the second image because it's
        # fit to height and centered in a 4x2 box (specified in put_image).
        self.ae(refs[1]['src_rect'], {'left': 0.0, 'top': 0.0, 'right': 1.0, 'bottom': 0.5})
        # Erase the line.
        s.erase_in_line(2)
        # Now test implicit column numbers.
        # We will mix implicit and explicit column/row specifications, but they
        # will be combine into just two references.
        s.apply_sgr('38;5;42')
        # full row 0 of the first image
        s.draw('\U0010eeee\u0305\u0305\U0010eeee\u0305\U0010eeee\U0010eeee\u0305')
        # full row 1 of the first image
        s.draw('\U0010eeee\u030d\U0010eeee\U0010eeee\U0010eeee\u030d\u0310')
        s.cursor_move(8)
        s.update_only_line_graphics_data()
        refs = layers(s)
        self.ae(len(refs), 2)
        self.ae(refs[0]['src_rect'], {'left': 0.0, 'top': 0.0, 'right': 1.0, 'bottom': 0.5})
        self.ae(refs[1]['src_rect'], {'left': 0.0, 'top': 0.5, 'right': 1.0, 'bottom': 1.0})
        # Now reset the screen, the images should be erased.
        s.reset()
        refs = layers(s)
        self.ae(len(refs), 0)

    def test_unicode_placeholders_3rd_combining_char(self):
        # This test tests that we can use the 3rd diacritic for the most
        # significant byte
        cw, ch = 10, 20
        s, dx, dy, put_image, put_ref, layers, rect_eq = put_helpers(self, cw, ch)
        # Upload two images.
        put_image(s, 20, 20, num_cols=4, num_lines=2, unicode_placeholder=1, id=42)
        put_image(s, 20, 10, num_cols=4, num_lines=1, unicode_placeholder=1, id=(42 << 24) + 43)
        # This one will have id=43, which does not exist.
        s.apply_sgr('38;2;0;0;43')
        s.draw('\U0010eeee\u0305\U0010eeee\U0010eeee\U0010eeee')
        s.cursor_move(4)
        s.update_only_line_graphics_data()
        refs = layers(s)
        self.ae(len(refs), 0)
        s.erase_in_line(2)
        # This one will have id=42. We explicitly specify that the most
        # significant byte is 0 (third \u305). Specifying the zero byte like
        # this is not necessary but is correct.
        s.apply_sgr('38;2;0;0;42')
        s.draw('\U0010eeee\u0305\u0305\u0305\U0010eeee\u0305\u030d\u0305')
        # This is the second image.
        # \u059C -> 42
        s.apply_sgr('38;2;0;0;43')
        s.draw('\U0010eeee\u0305\u0305\u059c\U0010eeee\u0305\u030d\u059c')
        # Check that we can continue by using implicit row/column specification.
        s.draw('\U0010eeee\u0305\U0010eeee')
        s.cursor_move(6)
        s.update_only_line_graphics_data()
        refs = layers(s)
        self.ae(len(refs), 2)
        self.ae(refs[0]['src_rect'], {'left': 0.0, 'top': 0.0, 'right': 0.5, 'bottom': 0.5})
        self.ae(refs[1]['src_rect'], {'left': 0.0, 'top': 0.0, 'right': 1.0, 'bottom': 1.0})
        s.erase_in_line(2)
        # Now test the 8-bit color mode. Using the third diacritic, we can
        # specify 16 bits: the most significant byte and the least significant
        # byte.
        s.apply_sgr('38;5;42')
        s.draw('\U0010eeee\u0305\u0305\u0305\U0010eeee')
        s.apply_sgr('38;5;43')
        s.draw('\U0010eeee\u0305\u0305\u059c\U0010eeee\U0010eeee\u0305\U0010eeee')
        s.cursor_move(6)
        s.update_only_line_graphics_data()
        refs = layers(s)
        self.ae(len(refs), 2)
        self.ae(refs[0]['src_rect'], {'left': 0.0, 'top': 0.0, 'right': 0.5, 'bottom': 0.5})
        self.ae(refs[1]['src_rect'], {'left': 0.0, 'top': 0.0, 'right': 1.0, 'bottom': 1.0})

    def test_unicode_placeholders_multiple_placements(self):
        # Here we test placement specification via underline color.
        cw, ch = 10, 20
        s, dx, dy, put_image, put_ref, layers, rect_eq = put_helpers(self, cw, ch)
        put_image(s, 20, 20, num_cols=1, num_lines=1, placement_id=1, unicode_placeholder=1, id=42)
        put_ref(s, id=42, num_cols=2, num_lines=1, placement_id=22, unicode_placeholder=1)
        put_ref(s, id=42, num_cols=4, num_lines=2, placement_id=44, unicode_placeholder=1)
        # The references are virtual, so no visible refs yet.
        s.update_only_line_graphics_data()
        refs = layers(s)
        self.ae(len(refs), 0)
        # Draw the first row of each placement.
        s.apply_sgr('38;5;42')
        s.apply_sgr('58;5;1')
        s.draw('\U0010eeee\u0305')
        s.apply_sgr('58;5;22')
        s.draw('\U0010eeee\u0305\U0010eeee\u0305')
        s.apply_sgr('58;5;44')
        s.draw('\U0010eeee\u0305\U0010eeee\u0305\U0010eeee\u0305\U0010eeee\u0305')
        s.update_only_line_graphics_data()
        refs = layers(s)
        self.ae(len(refs), 3)
        self.ae(refs[0]['src_rect'], {'left': 0.0, 'top': 0.0, 'right': 1.0, 'bottom': 1.5})
        self.ae(refs[1]['src_rect'], {'left': 0.0, 'top': 0.0, 'right': 1.0, 'bottom': 1.0})
        self.ae(refs[2]['src_rect'], {'left': 0.0, 'top': 0.0, 'right': 1.0, 'bottom': 0.5})

    def test_unicode_placeholders_scroll(self):
        # Here we test scrolling of a region. We'll draw an image spanning 8
        # rows and then scroll only the middle part of this image. Each
        # reference corresponds to one row.
        cw, ch = 5, 10
        s, dx, dy, put_image, put_ref, layers, rect_eq = put_helpers(self, cw, ch, lines=8)
        put_image(s, 5, 80, num_cols=1, num_lines=8, unicode_placeholder=1, id=42)
        s.apply_sgr('38;5;42')
        s.cursor_position(1, 0)
        s.draw('\U0010eeee\u0305\n')
        s.cursor_position(2, 0)
        s.draw('\U0010eeee\u030d\n')
        s.cursor_position(3, 0)
        s.draw('\U0010eeee\u030e\n')
        s.cursor_position(4, 0)
        s.draw('\U0010eeee\u0310\n')
        s.cursor_position(5, 0)
        s.draw('\U0010eeee\u0312\n')
        s.cursor_position(6, 0)
        s.draw('\U0010eeee\u033d\n')
        s.cursor_position(7, 0)
        s.draw('\U0010eeee\u033e\n')
        s.cursor_position(8, 0)
        s.draw('\U0010eeee\u033f')
        # Each line will contain a part of the image.
        s.update_only_line_graphics_data()
        refs = layers(s)
        refs = sorted(refs, key=lambda r: r['src_rect']['top'])
        self.ae(len(refs), 8)
        for i in range(8):
            self.ae(refs[i]['src_rect'], {'left': 0.0, 'top': 0.125 * i, 'right': 1.0, 'bottom': 0.125 * (i + 1)})
            self.ae(refs[i]['dest_rect']['top'], 1 - 0.25 * i)
        # Now set margins to lines 3 and 6.
        s.set_margins(3, 6)  # 1-based indexing
        # Scroll two lines down (i.e. move lines 3..6 up).
        # Lines 3 and 4 will be erased.
        s.cursor_position(6, 0)
        s.index()
        s.index()
        s.update_only_line_graphics_data()
        refs = layers(s)
        refs = sorted(refs, key=lambda r: r['src_rect']['top'])
        self.ae(len(refs), 6)
        # Lines 1 and 2 are outside of the region, not scrolled.
        self.ae(refs[0]['src_rect'], {'left': 0.0, 'top': 0.0, 'right': 1.0, 'bottom': 0.125})
        self.ae(refs[0]['dest_rect']['top'], 1.0)
        self.ae(refs[1]['src_rect'], {'left': 0.0, 'top': 0.125 * 1, 'right': 1.0, 'bottom': 0.125 * 2})
        self.ae(refs[1]['dest_rect']['top'], 1.0 - 0.25 * 1)
        # Lines 3 and 4 are erased.
        # Lines 5 and 6 are now higher.
        self.ae(refs[2]['src_rect'], {'left': 0.0, 'top': 0.125 * 4, 'right': 1.0, 'bottom': 0.125 * 5})
        self.ae(refs[2]['dest_rect']['top'], 1.0 - 0.25 * 2)
        self.ae(refs[3]['src_rect'], {'left': 0.0, 'top': 0.125 * 5, 'right': 1.0, 'bottom': 0.125 * 6})
        self.ae(refs[3]['dest_rect']['top'], 1.0 - 0.25 * 3)
        # Lines 7 and 8 are outside of the region.
        self.ae(refs[4]['src_rect'], {'left': 0.0, 'top': 0.125 * 6, 'right': 1.0, 'bottom': 0.125 * 7})
        self.ae(refs[4]['dest_rect']['top'], 1.0 - 0.25 * 6)
        self.ae(refs[5]['src_rect'], {'left': 0.0, 'top': 0.125 * 7, 'right': 1.0, 'bottom': 0.125 * 8})
        self.ae(refs[5]['dest_rect']['top'], 1.0 - 0.25 * 7)
        # Now scroll three lines up (i.e. move lines 5..6 down).
        # Line 6 will be erased.
        s.cursor_position(3, 0)
        s.reverse_index()
        s.reverse_index()
        s.reverse_index()
        s.update_only_line_graphics_data()
        refs = layers(s)
        refs = sorted(refs, key=lambda r: r['src_rect']['top'])
        self.ae(len(refs), 5)
        # Lines 1 and 2 are outside of the region, not scrolled.
        self.ae(refs[0]['src_rect'], {'left': 0.0, 'top': 0.0, 'right': 1.0, 'bottom': 0.125})
        self.ae(refs[0]['dest_rect']['top'], 1.0)
        self.ae(refs[1]['src_rect'], {'left': 0.0, 'top': 0.125 * 1, 'right': 1.0, 'bottom': 0.125 * 2})
        self.ae(refs[1]['dest_rect']['top'], 1.0 - 0.25 * 1)
        # Lines 3, 4 and 6 are erased.
        # Line 5 is now lower.
        self.ae(refs[2]['src_rect'], {'left': 0.0, 'top': 0.125 * 4, 'right': 1.0, 'bottom': 0.125 * 5})
        self.ae(refs[2]['dest_rect']['top'], 1.0 - 0.25 * 5)
        # Lines 7 and 8 are outside of the region.
        self.ae(refs[3]['src_rect'], {'left': 0.0, 'top': 0.125 * 6, 'right': 1.0, 'bottom': 0.125 * 7})
        self.ae(refs[3]['dest_rect']['top'], 1.0 - 0.25 * 6)
        self.ae(refs[4]['src_rect'], {'left': 0.0, 'top': 0.125 * 7, 'right': 1.0, 'bottom': 0.125 * 8})
        self.ae(refs[4]['dest_rect']['top'], 1.0 - 0.25 * 7)

    def test_unicode_placeholders_reverse_scroll(self):
        # A cell image pushed off the bottom of the screen by a reverse scroll
        # must not come back when the screen is scrolled forward again.
        cw, ch = 5, 10
        s, dx, dy, put_image, put_ref, layers, rect_eq = put_helpers(self, cw, ch, lines=4)
        put_image(s, 5, 10, num_cols=1, num_lines=1, unicode_placeholder=1, id=42)
        s.apply_sgr('38;5;42')
        s.cursor_position(4, 1)
        s.draw('\U0010eeee̅')
        s.update_only_line_graphics_data()
        self.ae(len(layers(s)), 1)
        s.cursor_position(1, 1)
        s.reverse_index()
        s.update_only_line_graphics_data()
        self.ae(len(layers(s)), 0)
        s.cursor_position(4, 1)
        s.index()
        s.update_only_line_graphics_data()
        self.ae(len(layers(s)), 0)
        # Same via CSI T
        s.reset()
        s.apply_sgr('38;5;42')
        s.cursor_position(4, 1)
        s.draw('\U0010eeee̅')
        s.update_only_line_graphics_data()
        self.ae(len(layers(s)), 1)
        s.reverse_scroll(2)
        s.update_only_line_graphics_data()
        self.ae(len(layers(s)), 0)
        s.cursor_position(4, 1)
        s.index(), s.index()
        s.update_only_line_graphics_data()
        self.ae(len(layers(s)), 0)

    def test_unicode_placeholders_history_margins(self):
        # Lines scrolled out of a region at the top of the screen (as tmux does for
        # a pane above its status line) enter the history, and the cell images
        # rendered for them while the view was scrolled back must move with them.
        cw, ch = 5, 10
        s, dx, dy, put_image, put_ref, layers, rect_eq = put_helpers(self, cw, ch, lines=6)
        put_image(s, 5, 20, num_cols=1, num_lines=2, unicode_placeholder=1, id=42)

        def draw_placeholders():
            s.apply_sgr('38;5;42')
            s.cursor_position(3, 1)
            s.draw('\U0010eeee\u0305')
            s.cursor_position(4, 1)
            s.draw('\U0010eeee\u030d')
            s.apply_sgr('39')

        def index_lines(row, n):
            s.cursor_position(row, 1)
            for i in range(n):
                s.draw('plain')
                s.index()
                s.carriage_return()
            s.update_only_line_graphics_data()

        def scroll_view(amt, up):
            s.scroll(amt, up)
            s.update_only_line_graphics_data()
            return layers(s, s.scrolled_by)

        for region in (True, False):
            s.reset()
            if region:
                s.set_margins(1, s.lines - 1)
            bottom = s.lines - 1 if region else s.lines
            draw_placeholders()
            index_lines(bottom, 8)
            self.ae(len(scroll_view(6, True)), 2)
            scroll_view(6, False)
            index_lines(bottom, 3)
            self.ae(len(scroll_view(9, True)), 0, f'region={region}')
            scroll_view(9, False)

        # A reverse index must not move the refs of lines in the history
        s.reset()
        draw_placeholders()
        index_lines(s.lines, 5)

        def ref_rows(refs):
            return sorted(round((1 - r['dest_rect']['top']) / dy) - s.scrolled_by for r in refs)

        before = ref_rows(scroll_view(6, True))
        self.ae(before, [-3, -2])
        scroll_view(6, False)
        s.cursor_position(1, 1)
        s.reverse_index()
        s.update_only_line_graphics_data()
        self.ae(ref_rows(scroll_view(6, True)), before)

    def test_unicode_placeholders_history_fill(self):
        # Lines popped back from the history (CSI + T, or enlarging the window with
        # scrollback_fill_enlarged_window) move the history down, so cell images
        # rendered for lines in the history must move with them.
        cw, ch = 5, 10
        s, dx, dy, put_image, put_ref, layers, rect_eq = put_helpers(self, cw, ch, lines=6)
        put_image(s, 5, 10, num_cols=1, num_lines=1, unicode_placeholder=1, id=42)

        def ref_rows(scrolled_by=0):
            s.update_only_line_graphics_data()
            return sorted(round((1 - r['dest_rect']['top']) / dy) - scrolled_by for r in layers(s, scrolled_by))

        def rows_in_history():
            # Render the history lines as when the view is scrolled back
            n = s.historybuf.count
            s.scroll(n, True)
            ans = [r for r in ref_rows(n) if r < 0]
            s.scroll(n, False)
            s.update_only_line_graphics_data()
            return ans

        def setup(region):
            # Push 'plain' and then a placeholder line into the history
            s.reset()
            if region:
                s.set_margins(1, s.lines - 1)
            s.draw('plain')
            s.carriage_return(), s.linefeed()
            s.apply_sgr('38;5;42')
            s.draw('\U0010eeee\u0305')
            s.apply_sgr('39')
            s.cursor_position(s.lines - 1 if region else s.lines, 1)
            s.index(), s.index()
            self.ae(str(s.historybuf.line(0)), '\U0010eeee\u0305')
            self.ae(rows_in_history(), [-1])

        for region in (False, True):
            setup(region)
            s.reverse_scroll(1, True)
            self.ae(str(s.line(0)), '\U0010eeee\u0305')
            self.ae(rows_in_history(), [], f'region={region}')
            self.ae(ref_rows(), [0], f'region={region}')

        def classic_in_history():
            nonlocal s, dy, layers
            s, _, dy, put_image, _, layers, _ = put_helpers(self, cw, ch, lines=6)
            put_image(s, cw, ch)  # a one cell image at (0, 0)
            s.draw('img')
            s.cursor_position(s.lines, 1)
            s.index()
            self.ae(ref_rows(1), [-1])

        # A reverse index that does not pull lines from the history leaves classic
        # placements in the history in place
        classic_in_history()
        s.cursor_position(1, 1)
        s.reverse_index()
        self.ae(ref_rows(), [])
        self.ae(ref_rows(1), [-1])

        # Enlarging the window pulls lines back from the history (cell images are
        # removed on resize, so only classic placements are left to move)
        classic_in_history()
        self.set_options({'scrollback_fill_enlarged_window': True})
        s.cursor_position(2, 1)
        s.resize(s.lines + 1, s.columns)
        self.ae(str(s.line(0)), ' img')
        self.ae(ref_rows(), [0])

    def test_gr_scroll(self):
        cw, ch = 10, 20
        s, dx, dy, put_image, put_ref, layers, rect_eq = put_helpers(self, cw, ch)
        put_image(s, 10, 20, no_id=True)  # a one cell image at (0, 0)
        self.ae(len(layers(s)), 1)
        for i in range(s.lines):
            s.index()
        self.ae(len(layers(s)), 0), self.ae(s.grman.image_count, 1)
        for i in range(s.historybuf.ynum - 1):
            s.index()
            self.ae(len(layers(s)), 0), self.ae(s.grman.image_count, 1)
        s.index()
        self.ae(s.grman.image_count, 0)

        # Now test with margins
        s.reset()
        # Test images outside page area untouched
        put_image(s, cw, ch)  # a one cell image at (0, 0)
        for i in range(s.lines - 1):
            s.index()
        put_image(s, cw, ch)  # a one cell image at (0, bottom)
        s.set_margins(2, 4)  # 1-based indexing
        self.ae(s.grman.image_count, 2)
        for i in range(s.lines + s.historybuf.ynum):
            s.index()
            self.ae(s.grman.image_count, 2)
        for i in range(s.lines):  # ensure cursor is at top margin
            s.reverse_index()
        # Test clipped scrolling during index
        put_image(s, cw, 2 * ch, z=-1, no_id=True)  # 1x2 cell image
        self.ae(s.grman.image_count, 3)
        self.ae(layers(s)[0]['src_rect'], {'left': 0.0, 'top': 0.0, 'right': 1.0, 'bottom': 1.0})
        s.index(), s.index()
        l0 = layers(s)
        self.ae(len(l0), 3)
        self.ae(layers(s)[0]['src_rect'], {'left': 0.0, 'top': 0.5, 'right': 1.0, 'bottom': 1.0})
        s.index()
        self.ae(s.grman.image_count, 2)
        # Test clipped scrolling during reverse_index
        for i in range(s.lines):
            s.reverse_index()
        put_image(s, cw, 2 * ch, z=-1, no_id=True)  # 1x2 cell image
        self.ae(s.grman.image_count, 3)
        self.ae(layers(s)[0]['src_rect'], {'left': 0.0, 'top': 0.0, 'right': 1.0, 'bottom': 1.0})
        while s.cursor.y != 1:
            s.reverse_index()
        s.reverse_index(), s.reverse_index()
        self.ae(layers(s)[0]['src_rect'], {'left': 0.0, 'top': 0.0, 'right': 1.0, 'bottom': 0.5})
        s.reverse_index()
        self.ae(s.grman.image_count, 2)
        # Test that scaled images (r=/c=) are clipped rather than distorted
        # when scrolled against a margin (#10377)
        s.reset()
        s.set_margins(1, 3)  # 1-based indexing
        put_image(s, cw, 4 * ch, num_cols=1, num_lines=2, no_id=True)  # 10x80 px image scaled into 1x2 cells at (0, 0)
        self.ae(s.grman.image_count, 1)
        self.ae(layers(s)[0]['src_rect'], {'left': 0.0, 'top': 0.0, 'right': 1.0, 'bottom': 1.0})
        rect_eq(layers(s)[0]['dest_rect'], -1, 1, -1 + dx, 1 - 2 * dy)
        while s.cursor.y != 2:
            s.index()
        s.index()  # scroll up, the top row of the image is clipped
        l0 = layers(s)
        self.ae(len(l0), 1)
        self.ae(l0[0]['src_rect'], {'left': 0.0, 'top': 0.5, 'right': 1.0, 'bottom': 1.0})
        rect_eq(l0[0]['dest_rect'], -1, 1, -1 + dx, 1 - dy)
        s.index()
        self.ae(s.grman.image_count, 0)
        # Now check clipping of a scaled image at the bottom margin
        s.reset()
        s.set_margins(1, 3)
        s.index()
        put_image(s, cw, 4 * ch, num_cols=1, num_lines=2, no_id=True)  # 1x2 cells at (0, 1)
        while s.cursor.y != 0:
            s.reverse_index()
        s.reverse_index()  # scroll down, the bottom row of the image is clipped
        l0 = layers(s)
        self.ae(len(l0), 1)
        self.ae(l0[0]['src_rect'], {'left': 0.0, 'top': 0.0, 'right': 1.0, 'bottom': 0.5})
        rect_eq(l0[0]['dest_rect'], -1, 1 - 2 * dy, -1 + dx, 1 - 3 * dy)
        s.reverse_index()
        self.ae(s.grman.image_count, 0)
        # Scaling specified via c= only, with the height derived from the aspect ratio
        s.reset()
        s.set_margins(1, 3)
        put_image(s, 2 * cw, 4 * ch, num_cols=1, no_id=True)  # 20x80 px image scaled into 1 col => 10x40 px => 1x2 cells
        rect_eq(layers(s)[0]['dest_rect'], -1, 1, -1 + dx, 1 - 2 * dy)
        while s.cursor.y != 2:
            s.index()
        s.index()  # scroll up, the top row of the image is clipped
        l0 = layers(s)
        self.ae(l0[0]['src_rect'], {'left': 0.0, 'top': 0.5, 'right': 1.0, 'bottom': 1.0})
        rect_eq(l0[0]['dest_rect'], -1, 1, -1 + dx, 1 - dy)
        s.index()
        self.ae(s.grman.image_count, 0)
        s.reset()
        self.assertEqual(s.grman.disk_cache.total_size, 0)

    def test_gr_reset(self):
        cw, ch = 10, 20
        s, dx, dy, put_image, put_ref, layers, rect_eq = put_helpers(self, cw, ch)
        put_image(s, cw, ch)  # a one cell image at (0, 0)
        self.ae(len(layers(s)), 1)
        s.reset()
        self.ae(s.grman.image_count, 0)
        put_image(s, cw, ch)  # a one cell image at (0, 0)
        self.ae(s.grman.image_count, 1)
        for i in range(s.lines):
            s.index()
        s.reset()
        self.ae(s.grman.image_count, 1)

    def test_gr_delete(self):
        cw, ch = 10, 20
        s, dx, dy, put_image, put_ref, layers, rect_eq = put_helpers(self, cw, ch)

        def delete(ac=None, **kw):
            cmd = 'a=d'
            if ac:
                cmd += f',d={ac}'
            if kw:
                cmd += ',' + ','.join(f'{k}={v}' for k, v in kw.items())
            send_command(s, cmd)

        iid = put_image(s, cw, ch, a='t')[0]
        self.ae(s.grman.image_count, 1)
        delete('I', i=iid)
        self.ae(s.grman.image_count, 0)
        iid1 = put_image(s, cw, ch, a='t')[0]
        iid2 = put_image(s, cw, ch, a='t')[0]
        self.ae(s.grman.image_count, 2)
        delete('R', x=iid1, y=iid2)
        self.ae(s.grman.image_count, 0)

        put_image(s, cw, ch)
        delete()
        self.ae(s.grman.image_count, 1)
        self.ae(len(layers(s)), 0)
        delete('A')
        self.ae(s.grman.image_count, 1)
        s.reset()
        self.ae(s.grman.image_count, 0)
        put_image(s, cw, ch)
        self.ae(s.grman.image_count, 1)
        delete('A')
        self.ae(s.grman.image_count, 0)
        self.assertEqual(s.grman.disk_cache.total_size, 0)
        iid = put_image(s, cw, ch)[0]
        delete('I', i=iid, p=7)
        self.ae(s.grman.image_count, 1)
        delete('I', i=iid)
        self.ae(s.grman.image_count, 0)
        self.assertEqual(s.grman.disk_cache.total_size, 0)
        iid = put_image(s, cw, ch, placement_id=9)[0]
        delete('I', i=iid, p=9)
        self.ae(s.grman.image_count, 0)
        self.assertEqual(s.grman.disk_cache.total_size, 0)
        s.reset()
        put_image(s, cw, ch)
        put_image(s, cw, ch)
        delete('C')
        self.ae(s.grman.image_count, 2)
        s.cursor_position(1, 1)
        delete('C')
        self.ae(s.grman.image_count, 1)
        delete('P', x=2, y=1)
        self.ae(s.grman.image_count, 0)
        self.assertEqual(s.grman.disk_cache.total_size, 0)
        put_image(s, cw, ch, z=9)
        delete('Z', z=9)
        self.ae(s.grman.image_count, 0)
        put_image(s, cw, ch, id=1)
        put_image(s, cw, ch, id=2)
        put_image(s, cw, ch, id=3)
        delete('R', y=2)
        self.ae(s.grman.image_count, 1)
        delete('R', x=3, y=3)
        self.ae(s.grman.image_count, 0)
        self.assertEqual(s.grman.disk_cache.total_size, 0)

        # test put + delete + put
        iid = 999999
        self.ae(put_image(s, cw, ch, id=iid), (iid, 'OK'))
        self.ae(put_ref(s, id=iid), (iid, ('OK', f'i={iid}')))
        delete('i', i=iid)
        self.ae(s.grman.image_count, 1)
        self.ae(put_ref(s, id=iid), (iid, ('OK', f'i={iid}')))
        delete('I', i=iid)
        self.ae(put_ref(s, id=iid), (iid, ('ENOENT', f'i={iid}')))
        self.ae(s.grman.image_count, 0)
        self.assertEqual(s.grman.disk_cache.total_size, 0)

        # test delete but not free
        s.reset()
        iid = 9999999
        self.ae(put_image(s, cw, ch, id=iid), (iid, 'OK'))
        self.ae(put_ref(s, id=iid), (iid, ('OK', f'i={iid}')))
        self.ae(put_image(s, cw, ch, id=iid + 1), (iid + 1, 'OK'))
        self.ae(put_ref(s, id=iid + 1), (iid + 1, ('OK', f'i={iid + 1}')))
        delete('i', i=iid)
        self.ae(s.grman.image_count, 2)
        delete('I', i=iid + 1)
        self.ae(s.grman.image_count, 1)

    def test_animation_frame_loading(self):
        s = self.create_screen()
        g = s.grman
        li = make_send_command(s)

        def t(code='OK', image_id=1, frame_number=2, **kw):
            res = li(**kw)
            if code is not None:
                self.assertEqual(code, res.code, f'{code} != {res.code}: {res.msg}')
            if image_id is not None:
                self.assertEqual(image_id, res.image_id)
            if frame_number is not None:
                self.assertEqual(frame_number, res.frame_number)

        # test error on send frame for non-existent image
        self.assertEqual(li().code, 'ENOENT')

        # create image
        self.assertEqual(li(a='t').code, 'OK')
        self.assertEqual(g.disk_cache.total_size, 36)

        # simple new frame (width=4, height=3)
        self.assertIsNone(li(payload='2' * 12, z=77, m=1))
        self.assertIsNone(li(payload='2' * 12, z=77, m=1))
        t(payload='2' * 12, z=77)
        img = g.image_for_client_id(1)
        self.assertEqual(img['extra_frames'], ({'gap': 77, 'id': 2, 'data': b'2' * 36},))
        # test editing a frame
        t(payload='3' * 36, r=2)
        img = g.image_for_client_id(1)
        self.assertEqual(img['extra_frames'], ({'gap': 77, 'id': 2, 'data': b'3' * 36},))
        # test editing part of a frame
        t(payload='4' * 12, r=2, s=2, v=2)
        img = g.image_for_client_id(1)

        def expand(*rows):
            ans = []
            for r in rows:
                ans.append(''.join(x * 3 for x in str(r)))
            return ''.join(ans).encode('ascii')

        self.assertEqual(img['extra_frames'], ({'gap': 77, 'id': 2, 'data': expand(4433, 4433, 3333)},))
        t(payload='5' * 12, r=2, s=2, v=2, x=1, y=1)
        img = g.image_for_client_id(1)
        self.assertEqual(img['extra_frames'], ({'gap': 77, 'id': 2, 'data': expand(4433, 4553, 3553)},))
        t(payload='3' * 36, r=2)
        img = g.image_for_client_id(1)
        self.assertEqual(img['extra_frames'], ({'gap': 77, 'id': 2, 'data': b'3' * 36},))
        # the composition mode for frame edits must be controlled by the X key,
        # with fully transparent pixels being a no-op unless X=1 (issue #10379)
        transparent = b'\x00' * 48
        t(payload=transparent, r=2, f=32)
        img = g.image_for_client_id(1)
        self.assertEqual(img['extra_frames'], ({'gap': 77, 'id': 2, 'data': b'3' * 36},))
        t(payload=transparent, r=2, f=32, C=1)
        img = g.image_for_client_id(1)
        self.assertEqual(img['extra_frames'], ({'gap': 77, 'id': 2, 'data': b'3' * 36},))
        t(payload=transparent, r=2, f=32, X=1)
        img = g.image_for_client_id(1)
        self.assertEqual(img['extra_frames'], ({'gap': 77, 'id': 2, 'data': b'\x00' * 36},))
        t(payload='3' * 36, r=2)
        img = g.image_for_client_id(1)
        self.assertEqual(img['extra_frames'], ({'gap': 77, 'id': 2, 'data': b'3' * 36},))
        # test loading from previous frame
        t(payload='4' * 12, c=2, s=2, v=2, z=101, frame_number=3)
        img = g.image_for_client_id(1)
        self.assertEqual(
            img['extra_frames'],
            (
                {'gap': 77, 'id': 2, 'data': b'3' * 36},
                {'gap': 101, 'id': 3, 'data': b'444444333333444444333333333333333333'},
            ),
        )
        # test changing gaps
        img = g.image_for_client_id(1)
        self.assertEqual(img['root_frame_gap'], 0)
        self.assertIsNone(li(a='a', i=1, r=1, z=13))
        img = g.image_for_client_id(1)
        self.assertEqual(img['root_frame_gap'], 13)
        self.assertIsNone(li(a='a', i=1, r=2, z=43))
        img = g.image_for_client_id(1)
        self.assertEqual(img['extra_frames'][0]['gap'], 43)
        # test changing current frame
        img = g.image_for_client_id(1)
        self.assertEqual(img['current_frame_index'], 0)
        self.assertIsNone(li(a='a', i=1, c=2))
        img = g.image_for_client_id(1)
        self.assertEqual(img['current_frame_index'], 1)

        # test delete of frames
        t(payload='5' * 36, frame_number=4)
        img = g.image_for_client_id(1)
        self.assertEqual(
            img['extra_frames'],
            (
                {'gap': 43, 'id': 2, 'data': b'3' * 36},
                {'gap': 101, 'id': 3, 'data': b'444444333333444444333333333333333333'},
                {'gap': 40, 'id': 4, 'data': b'5' * 36},
            ),
        )
        self.assertEqual(img['current_frame_index'], 1)
        self.assertIsNone(li(a='d', d='f', i=1, r=1))
        img = g.image_for_client_id(1)
        self.assertEqual(img['current_frame_index'], 0)
        self.assertEqual(img['data'], b'3' * 36)
        self.assertEqual(
            img['extra_frames'],
            (
                {'gap': 101, 'id': 3, 'data': b'444444333333444444333333333333333333'},
                {'gap': 40, 'id': 4, 'data': b'5' * 36},
            ),
        )
        self.assertIsNone(li(a='a', i=1, c=3))
        img = g.image_for_client_id(1)
        self.assertEqual(img['current_frame_index'], 2)
        self.assertIsNone(li(a='d', d='f', i=1, r=2))
        img = g.image_for_client_id(1)
        self.assertEqual(img['current_frame_index'], 1)
        self.assertEqual(img['data'], b'3' * 36)
        self.assertEqual(img['extra_frames'], ({'gap': 40, 'id': 4, 'data': b'5' * 36},))
        self.assertIsNone(li(a='d', d='f', i=1))
        img = g.image_for_client_id(1)
        self.assertEqual(img['current_frame_index'], 0)
        self.assertEqual(img['data'], b'5' * 36)
        self.assertFalse(img['extra_frames'])
        self.assertIsNone(li(a='d', d='f', i=1))
        img = g.image_for_client_id(1)
        self.assertEqual(img['data'], b'5' * 36)
        self.ae(g.image_count, 1)
        self.assertIsNone(li(a='d', d='F', i=1))
        self.ae(g.image_count, 0)
        self.assertEqual(g.disk_cache.total_size, 0)

        # test frame composition
        self.assertEqual(li(a='t').code, 'OK')
        self.assertEqual(g.disk_cache.total_size, 36)
        t(payload='2' * 36)
        t(payload='3' * 36, frame_number=3)
        img = g.image_for_client_id(1)
        self.assertEqual(
            img['extra_frames'],
            (
                {'gap': 40, 'id': 2, 'data': b'2' * 36},
                {'gap': 40, 'id': 3, 'data': b'3' * 36},
            ),
        )
        self.assertEqual(li(a='c', i=11).code, 'ENOENT')
        self.assertEqual(li(a='c', i=1, r=1, c=2).code, 'OK')
        img = g.image_for_client_id(1)
        self.assertEqual(
            img['extra_frames'],
            (
                {'gap': 40, 'id': 2, 'data': b'abcdefghijkl' * 3},
                {'gap': 40, 'id': 3, 'data': b'3' * 36},
            ),
        )
        self.assertEqual(li(a='c', i=1, r=2, c=3, w=1, h=2, x=1, y=1).code, 'OK')
        img = g.image_for_client_id(1)
        self.assertEqual(
            img['extra_frames'],
            (
                {'gap': 40, 'id': 2, 'data': b'abcdefghijkl' * 3},
                {'gap': 40, 'id': 3, 'data': b'3' * 12 + (b'333abc' + b'3' * 6) * 2},
            ),
        )

        # Composing into a frame materializes it as a full frame. Its pixel
        # format metadata must be updated to match the coalesced frame data.
        rgba_screen = self.create_screen()
        rgba_grman = rgba_screen.grman
        rgba_li = make_send_command(rgba_screen)
        self.assertEqual(rgba_li(payload=b'\0' * 48, a='t', f=32).code, 'OK')
        self.assertEqual(rgba_li(payload=b'R' * 12, c=1, s=2, v=2, f=24).code, 'OK')
        frame_before_composition = rgba_grman.image_for_client_id(1)['extra_frames'][0]['data']
        self.assertEqual(len(frame_before_composition), 48)
        self.assertEqual(rgba_li(payload=b'', a='c', f=0, s=0, v=0, r=1, c=2, w=1, h=1, x=3, y=2).code, 'OK')
        frame_after_composition = rgba_grman.image_for_client_id(1)['extra_frames'][0]['data']
        self.assertEqual(frame_after_composition, frame_before_composition)

        # Test that compose commands with offset values that would overflow a 32-bit
        # unsigned integer are correctly rejected with EINVAL instead of crashing.
        # In the old code, UINT32_MAX + img->width wrapped around as uint32_t to a
        # small value that bypassed the bounds check, causing a crash (fix: e9661f0).
        for offset_param in ('x', 'y', 'X', 'Y'):
            self.assertEqual(
                li(payload=b'', a='c', i=1, r=1, c=2, s=0, v=0, f=0, **{offset_param: 0xFFFFFFFF}).code,
                'EINVAL',
                f'Expected EINVAL for overflow in compose offset parameter {offset_param!r}',
            )

    def test_graphics_compose_canvas_bounds(self):
        from kitty.fast_data_types import create_canvas

        # Sanity: a well formed overlay is copied correctly onto the canvas.
        overlay = bytes(range(1, 13))  # 2x2 RGB overlay
        canvas = create_canvas(overlay, 2, 0, 0, 2, 2, 3)
        self.assertEqual(canvas, overlay)
        # out of bounds overlay
        big_overlay = bytes((i % 251) + 1 for i in range(4 * 4 * 3))
        canvas = create_canvas(big_overlay, 4, 0, 0, 2, 2, 3)
        self.assertEqual(len(canvas), 2 * 2 * 3)
        expected = big_overlay[0:6] + big_overlay[12:18]  # first two rows, first two pixels
        self.assertEqual(canvas, expected)
        # out of bounds offset overlay
        canvas = create_canvas(overlay, 2, 1000, 1000, 2, 2, 3)
        self.assertEqual(canvas, b'\x00' * (2 * 2 * 3))
        # A huge offset (near UINT32_MAX) must not overflow the address math.
        canvas = create_canvas(overlay, 2, 0xFFFFFFFF, 0xFFFFFFFF, 2, 2, 3)
        self.assertEqual(canvas, b'\x00' * (2 * 2 * 3))
        # A partially overlapping overlay: place a 2x2 overlay at x=1,y=1 of a
        # 2x2 canvas so only its top-left pixel lands inside the canvas.
        canvas = create_canvas(overlay, 2, 1, 1, 2, 2, 3)
        expected = bytearray(2 * 2 * 3)
        expected[9:12] = overlay[0:3]  # bottom-right pixel of canvas
        self.assertEqual(canvas, bytes(expected))
        # Degenerate/invalid parameters must raise instead of crashing.
        self.assertRaises(ValueError, create_canvas, overlay, 0, 0, 0, 2, 2, 3)
        self.assertRaises(ValueError, create_canvas, overlay, 2, 0, 0, 2, 2, 0)
        self.assertRaises(ValueError, create_canvas, overlay, 2, 0, 0, 2, 2, 5)

    def test_animation_frame_long_reference_chain(self):
        # A new frame based on another frame with a long/large reference chain is
        # stored as a fully coalesced key frame rather than as a delta
        s = self.create_screen()
        g = s.grman
        li = make_send_command(s)
        self.assertEqual(li(a='t').code, 'OK')
        self.assertEqual(g.disk_cache.total_size, 36)

        # frame 2 is a delta on top of the root frame
        self.assertEqual(li(payload='2' * 36, c=1).frame_number, 2)
        self.assertEqual(g.disk_cache.total_size, 72)

        # frame 3 is based on frame 2, whose reference chain is now large enough
        # that frame 3 must be coalesced into a full key frame
        self.assertEqual(li(payload='4' * 12, c=2, s=2, v=2).frame_number, 3)
        img = g.image_for_client_id(1)
        self.assertEqual(
            img['extra_frames'],
            (
                {'gap': 40, 'id': 2, 'data': b'2' * 36},
                {'gap': 40, 'id': 3, 'data': b'444444222222' * 2 + b'2' * 12},
            ),
        )
        # a full 36 byte key frame, not a 12 byte delta
        self.assertEqual(g.disk_cache.total_size, 108)

    def test_animation_frame_chunked_loading(self):
        # continuation chunks of a chunked a=f transmission carry only the m key,
        # they must be routed to the frame load handler, not the add handler
        s = self.create_screen()
        g = s.grman
        li = make_send_command(s)
        self.assertEqual(li(a='t').code, 'OK')

        def chunked(payload, last_payload, cont='', **kw):
            self.assertIsNone(li(payload=payload, m=1, **kw))
            self.assertFalse(send_command(s, cont + 'm=1', payload))
            return parse_full_response(send_command(s, cont + 'm=0', last_payload))

        # create a new frame with continuation chunks
        res = chunked('2' * 12, '2' * 12, z=77)
        self.assertEqual((res.code, res.image_id, res.frame_number), ('OK', 1, 2))
        img = g.image_for_client_id(1)
        self.assertEqual(img['data'], b'abcdefghijkl' * 3)  # root frame must be untouched
        self.assertEqual(img['extra_frames'], ({'gap': 77, 'id': 2, 'data': b'2' * 36},))
        # edit an existing frame with continuation chunks, r= is only present
        # in the start command
        res = chunked('3' * 12, '3' * 12, r=2)
        self.assertEqual((res.code, res.image_id, res.frame_number), ('OK', 1, 2))
        img = g.image_for_client_id(1)
        self.assertEqual(img['extra_frames'], ({'gap': 77, 'id': 2, 'data': b'3' * 36},))
        # the documented form of continuation chunks for frame data, a=f
        # without an image id, must also get a response identifying the image
        res = chunked('4' * 12, '4' * 12, cont='a=f,')
        self.assertEqual((res.code, res.image_id, res.frame_number), ('OK', 1, 3))
        res = chunked('5' * 12, '5' * 12, cont='a=f,', r=3)
        self.assertEqual((res.code, res.image_id, res.frame_number), ('OK', 1, 3))
        img = g.image_for_client_id(1)
        self.assertEqual(img['data'], b'abcdefghijkl' * 3)
        self.assertEqual(img['extra_frames'], ({'gap': 77, 'id': 2, 'data': b'3' * 36}, {'gap': 40, 'id': 3, 'data': b'5' * 36}))
        # repeating the image id in continuation chunks works as well
        res = chunked('6' * 12, '6' * 12, cont='a=f,i=1,', r=3)
        self.assertEqual((res.code, res.image_id, res.frame_number), ('OK', 1, 3))
        # with q=1 success is not reported but failure is
        self.assertIsNone(chunked('7' * 12, '7' * 12, cont='a=f,', q=1))
        self.assertIsNone(li(payload='8' * 12, m=1, q=1))
        res = parse_full_response(send_command(s, 'a=f,m=0'))
        self.assertEqual((res.code, res.image_id), ('ENODATA', 1))
        # with q=2 failure is not reported either
        self.assertIsNone(li(payload='8' * 12, m=1, q=2))
        self.assertIsNone(parse_full_response(send_command(s, 'a=f,m=0')))

    def test_vector_images(self):
        s = self.create_screen()
        g = s.grman

        def cmd(payload, **kw):
            kw.setdefault('i', 1)
            return parse_full_response(send_command(s, ','.join(f'{k}={v}' for k, v in kw.items()), payload))

        def pixel(img, x, y):
            o = (y * img['width'] + x) * 4
            return tuple(img['data'][o:o + 4])

        # probing with a query answers OK, and the image is not kept
        self.ae(cmd('p id=1 d=M0,0L1,0L1,1Z', a='q', f=200, s=1, v=1).code, 'OK')
        self.ae(g.image_count, 0)
        self.ae(cmd('p id=1 d=M0,0L1,0L1,1Z', a='q', f=200, s=1, v=1, o='z').code, 'EINVAL')

        # a filled square and a stroked line, rasterized at the intrinsic size
        doc = 'p id=1 d=M2,2L6.5,2L6.5,6L2,6Z fill=#ff0000\np id=2 d=M0,9L10,9 stroke=#0000ff w=2 fill=none\n'
        res = cmd(doc, a='T', f=200, s=10, v=10)
        self.ae((res.code, res.image_id), ('OK', 1))
        img = g.image_for_client_id(1)
        self.ae((img['width'], img['height'], img['vector_elements']), (10, 10, 2))
        self.ae(len(img['data']), 10 * 10 * 4)
        self.ae(pixel(img, 3, 3), (255, 0, 0, 255))
        self.ae(pixel(img, 0, 0), (0, 0, 0, 0))
        self.ae(pixel(img, 5, 9), (0, 0, 255, 255))
        self.ae(pixel(img, 5, 7), (0, 0, 0, 0))
        self.ae(pixel(img, 5, 8), (0, 0, 255, 255))
        # an edge through the middle of a pixel gives half coverage
        self.ae(pixel(img, 6, 3), (255, 0, 0, 128))

        # a patch moves the square with a group transform, chunked and compressed
        self.ae(cmd('g id=10 t=1,0,0,1,-2,-2\n', a='e').code, 'OK')
        self.assertIsNone(cmd('p id=1 g=10 d=M2,2L6,2', a='e', m=1))
        self.ae(cmd('L6,6L2,6Z fill=#00ff00\n', m=0).code, 'OK')
        img = g.image_for_client_id(1)
        self.ae(img['vector_elements'], 3)
        self.ae(pixel(img, 1, 1), (0, 255, 0, 255))
        self.ae(pixel(img, 4, 4), (0, 0, 0, 0))
        self.ae(cmd(zlib.compress(b'g id=10 t=1,0,0,1,0,0\n'), a='e', o='z').code, 'OK')
        self.ae(pixel(g.image_for_client_id(1), 3, 3), (0, 255, 0, 255))

        # deleting a group takes its subtree, deleting all empties the document
        self.ae(cmd('x g=10\n', a='e').code, 'OK')
        img = g.image_for_client_id(1)
        self.ae(img['vector_elements'], 1)
        self.ae(pixel(img, 3, 3), (0, 0, 0, 0))
        self.ae(cmd('x all', a='e').code, 'OK')
        self.ae(g.image_for_client_id(1)['vector_elements'], 0)

        # bad records fail the whole command and change nothing
        self.ae(cmd('p id=5 d=M0,0L1,1Z\np id=6', a='e').code, 'EINVAL')
        self.ae(g.image_for_client_id(1)['vector_elements'], 0)
        self.ae(cmd('p id=5 d=M0,0L1,1Z\nq id=6 whatever=1\n', a='e').code, 'OK')
        self.ae(g.image_for_client_id(1)['vector_elements'], 1)
        self.ae(cmd('p id=1 d=M0,0', a='T', f=200, s=0, v=1, i=3).code, 'EINVAL')
        self.ae(cmd('p id=1 d=M0,0L1,1Z5', a='T', f=200, s=1, v=1, i=3).code, 'EINVAL')  # a number after Z
        self.ae(cmd('p id=1 d=M0,0', a='e', i=2).code, 'ENOENT')

        # frames and composition are refused, edits of raster images too
        self.ae(cmd('abcd', a='f', s=1, v=1).code, 'EINVAL')
        self.ae(cmd('', a='c', r=1, c=1).code, 'EINVAL')
        self.ae(cmd('abcd', a='T', f=32, i=2, s=1, v=1).code, 'OK')
        self.ae(cmd('x all', a='e', i=2).code, 'EINVAL')

        # a raster image drawn through an i record, scaled by its transform
        self.ae(cmd(b'\xff\x00\x00\x00\xff\x00' * 2, a='T', f=24, i=2, s=2, v=2).code, 'OK')
        self.ae(cmd('i id=1 img=2 t=5,0,0,5,0,0\n', a='T', f=200, s=10, v=10).code, 'OK')
        img = g.image_for_client_id(1)
        self.ae(pixel(img, 2, 2), (255, 0, 0, 255))
        self.ae(pixel(img, 7, 7), (0, 255, 0, 255))

    def test_vector_growing_geometry(self):
        s = self.create_screen()
        g = s.grman

        def command(records, action='t', image=1):
            res = parse_full_response(send_command(s, f'a={action},f=200,s=10,v=10,i={image}', records))
            self.ae(res.code, 'OK', res.msg)

        # Numeric tokens can be longer than the old 63-character parsing buffer.
        one = '1.' + '0' * 2048
        command(f'k id=1 pts={one},1,1 fill=#ff0000')
        command('k id=1 pts=1,1,1 fill=#ff0000', image=2)
        self.ae(g.render_vector_image(1, 10, 10), g.render_vector_image(2, 10, 10))
        # A stroke accumulates many small appends across multiple capacity changes.
        points = ['1,1,1']
        for j in range(2048):
            point = f'{1 + j % 8},{1 + (j // 8) % 8},1'
            points.append(point)
            command(f'a id=1 pts={point}', action='e')
        command('k id=1 pts=' + ','.join(points) + ' fill=#ff0000', image=2)
        self.ae(g.render_vector_image(1, 10, 10), g.render_vector_image(2, 10, 10))

    def test_vector_deep_groups(self):
        s = self.create_screen()
        g = s.grman

        def command(records, action='t'):
            res = parse_full_response(send_command(s, f'a={action},f=200,s=10,v=10,i=1', records))
            self.ae(res.code, 'OK', res.msg)

        # Heap-backed traversal and ancestor transforms must reach beyond the old depth 64.
        groups = ''.join(f'g id={i} p={i - 1} t=1,0,0,1,0,0\n' for i in range(1, 2049))
        command(groups + 'p id=3000 g=2048 d=M1,1L9,1L9,9L1,9Z fill=#ff0000')
        pixels = g.render_vector_image(1, 10, 10)
        self.ae(pixels[(5 * 10 + 5) * 4 : (5 * 10 + 5) * 4 + 4], b'\xff\x00\x00\xff')
        command('g id=100 p=99 o=50', action='e')
        pixels = g.render_vector_image(1, 10, 10)
        self.ae(pixels[(5 * 10 + 5) * 4 + 3], 128)
        command('x g=1', action='e')
        self.ae(g.image_for_client_id(1)['vector_elements'], 0)
        # Parent cycles remain harmless rather than becoming an infinite traversal.
        command('g id=1 p=2\ng id=2 p=1\np id=3 g=1 d=M1,1L9,9 stroke=#ff0000')
        self.ae(g.render_vector_image(1, 10, 10), bytes(10 * 10 * 4))

    def test_vector_dash_limit(self):
        # A dash pattern far finer than its path is long must not cut it into more dashes than memory or time allow,
        # nor may a path far longer than the image, and neither may change what the dashes look like.
        import signal
        s = self.create_screen()
        g = s.grman

        def draw(payload, image_id, size=20):
            # the default action of the alarm ends the process, so a hang fails the run instead of stalling it
            signal.signal(signal.SIGALRM, signal.SIG_DFL)
            signal.alarm(60)
            try:
                start = time.monotonic()
                res = parse_full_response(send_command(s, f'a=T,f=200,s={size},v={size},i={image_id}', payload))
                self.ae((res.code, res.msg), ('OK', ''))
                pixels = g.render_vector_image(image_id, size, size)
                self.assertLess(time.monotonic() - start, 10, payload)
                return pixels
            finally:
                signal.alarm(0)

        def alpha(pixels, x, y, size=20):
            return pixels[(y * size + x) * 4 + 3]

        # A pattern finer than the rasterizer samples, a millionth of a unit along a line of a million units, is
        # drawn at the part of the line the dashes cover
        px = draw('p id=1 d=M0,10L1000000,10 stroke=#000000 w=4 fill=none dash=0.000001,0.000001', 1)
        self.assertTrue(all(120 < alpha(px, x, 10) < 136 for x in (0, 7, 13, 19)), [alpha(px, x, 10) for x in range(20)])
        self.ae(alpha(px, 5, 4), 0)
        # with caps that close the gaps the dashes are solid, with a third of them covered a third
        px = draw('p id=1 d=M0,10L20,10 stroke=#000000 w=4 fill=none dash=0.001,0.001 cap=square', 2)
        self.ae([alpha(px, x, 10) for x in (0, 7, 13, 19)], [255] * 4)
        px = draw('p id=1 d=M0,10L20,10 stroke=#000000 w=4 fill=none dash=0.001,0.002', 3)
        self.assertTrue(all(80 < alpha(px, x, 10) < 90 for x in (0, 7, 13, 19)), [alpha(px, x, 10) for x in range(20)])
        # every subpath of a path is drawn the same way, there is no budget for the whole path
        many = ''.join(f'M0,{y}.5L1000,{y}.5' for y in range(0, 20, 2))
        px = draw(f'p id=1 d={many} stroke=#000000 w=1 fill=none dash=0.01,0.01', 4)
        coverage = [alpha(px, 10, y) for y in range(0, 20, 2)]
        self.assertTrue(all(120 < a < 136 for a in coverage), coverage)
        # dashes that can be drawn are drawn as before
        px = draw('p id=1 d=M0,10L20,10 stroke=#000000 w=4 fill=none dash=4,4 cap=butt', 5)
        self.ae([alpha(px, x, 10) for x in (1, 5, 9, 13)], [255, 0, 255, 0])
        # The dashes of a line two million million units long, of which twenty are seen, are those of the twenty:
        # 2**40 keeps every position along it a whole number, so the two must come out the same to the bit
        sane = draw('p id=1 d=M0,10L20,10 stroke=#000000 w=4 fill=none dash=1,1 cap=butt', 6)
        self.ae(draw(f'p id=1 d=M-{2**40},10L{2**40},10 stroke=#000000 w=4 fill=none dash=1,1 cap=butt', 7), sane)
        sane = draw('p id=1 d=M-4,10L28,10 stroke=#000000 w=1.5 fill=none dash=1,3 cap=round', 8)
        self.ae(draw(f'p id=1 d=M-{2**40},10L{2**40},10 stroke=#000000 w=1.5 fill=none dash=1,3 cap=round', 9), sane)
        # a line whose length is all rounding, which no number of steps along it could reach the end of
        draw('p id=1 d=M1e15,10L-1e15,10 stroke=#000000 w=4 fill=none dash=1,1', 10)
        draw('p id=1 d=M1e300,10L-1e300,10 stroke=#000000 w=4 fill=none dash=1e-300,1', 11)
        # A stroke ten thousand million units wide is dashed across the image like one a hundred wide, and with
        # round caps that overlap its gaps are closed
        sane = draw('p id=1 d=M-2,10L22,10 stroke=#000000 w=100 dash=1,1 cap=butt fill=none', 12)
        self.ae(draw('p id=1 d=M-1e9,10L1e9,10 stroke=#000000 w=1e10 dash=1,1 cap=butt fill=none', 13), sane)
        px = draw('p id=1 d=M-1e9,10L1e9,10 stroke=#000000 w=1e10 dash=1,1 cap=round fill=none', 14)
        self.ae(set(px[3::4]), {255})
        # a corner in the middle of a dash keeps its join: here only the tip of a miter reaches the image
        sane = draw('p id=1 d=M-106,0L-6,10L-106,20 stroke=#000000 w=8 dash=1000,1 join=miter ml=20 fill=none', 15)
        self.assertIn(255, sane[3::4])
        huge = f'M-{6 + 10 * 2**40},{10 - 2**40}L-6,10L-{6 + 10 * 2**40},{10 + 2**40}'
        self.ae(draw(f'p id=1 d={huge} stroke=#000000 w=8 dash={2**45},1 join=miter ml=20 fill=none', 16), sane)

    def test_vector_huge_geometry(self):
        # Coordinates, radii, widths and transforms of any size, or that overflow, take time in proportion to what
        # can be seen, and what can be seen is drawn as it would be by an equivalent drawing of sensible size.
        import signal
        s = self.create_screen()
        g = s.grman
        ids = iter(range(1, 1000))

        def draw(payload, size=20):
            signal.signal(signal.SIGALRM, signal.SIG_DFL)
            signal.alarm(60)
            try:
                i = next(ids)
                start = time.monotonic()
                res = parse_full_response(send_command(s, f'a=T,f=200,s={size},v={size},i={i}', payload))
                self.ae((res.code, res.msg), ('OK', ''), payload)
                pixels = g.render_vector_image(i, size, size)
                self.assertLess(time.monotonic() - start, 10, payload)
                return pixels
            finally:
                signal.alarm(0)

        def alphas(pixels):
            return set(pixels[3::4])

        nothing = bytes(20 * 20 * 4)
        red = b'\xff\x00\x00\xff' * 400
        top_half = draw('p id=1 d=M0,0L20,0L20,10L0,10Z fill=#ff0000')
        # a circle a hundred thousand million million pixels across, as a pen point, a round cap and a round join
        self.ae(draw('k id=1 pts=10,10,2e17 fill=#ff0000'), red)
        self.ae(draw('p id=1 d=M10,10L10,10 stroke=#ff0000 w=2e17 cap=round fill=none'), red)
        self.ae(draw('p id=1 d=M0,0L1e17,0L0,1e17 stroke=#ff0000 w=1e17 join=round fill=none'), red)
        self.ae(draw('k id=1 pts=1e17,1e17,1e16 fill=#ff0000'), nothing)
        # its edge through the image is where it is: 2**50 is the radius, the center is a whole number from it
        px = draw(f'k id=1 pts={2**50 + 10},10,{2**51} fill=#ff0000')
        for y in range(20):
            row = [px[(y * 20 + x) * 4 + 3] for x in range(20)]
            self.ae(row[:9], [0] * 9)
            self.ae(row[11:], [255] * 9)
        # a curve a hundred thousand million million pixels across that closes across the image, which keeps
        # its winding for the parts of it far outside
        self.ae(draw('p id=1 d=M-1e17,10C-1e17,-1e17,1e17,-1e17,1e17,10Z fill=#ff0000'), top_half)
        self.ae(draw('p id=1 d=M-1e17,10C-1e17,-1e17,1e17,-1e17,1e17,10Z fill=#ff0000 rule=eo'), top_half)
        self.ae(draw('p id=1 d=M-1e17,10Q0,-1e17,1e17,10Z fill=#ff0000'), top_half)
        self.ae(draw('p id=1 d=M-1e17,10C-1e17,-1e17,1e17,-1e17,1e17,10Z stroke=#00ff00 w=4 fill=#ff0000'),
                draw('p id=1 d=M-30,10L-30,-30L50,-30L50,10Z stroke=#00ff00 w=4 fill=#ff0000'))
        self.ae(draw('s id=1 d=M-1e17,10C-1e17,-1e17,1e17,-1e17,1e17,10Z\nu id=2 s=1 fill=#ff0000'), top_half)
        self.ae(draw('p id=1 d=M-1e17,10C-1e17,-1e17,1e17,-1e17,1e17,10Z\ng id=2 clip=1\np id=3 g=2 d=M0,0L20,0L20,20L0,20Z fill=#ff0000'), top_half)
        # the same with the size in the transform
        self.ae(draw('g id=1 t=1e16,0,0,1e16,0,0\np id=2 g=1 d=M-10,0.000000000000001C-10,-10,10,-10,10,0.000000000000001Z fill=#ff0000'), top_half)
        # a stroke that is tiny across one way and vast along the other is flattened for the size it is drawn at
        px = draw('g id=1 t=1e-12,0,0,1,0,0\np id=2 g=1 d=M0,5C1e13,5,1e13,15,2e13,15 stroke=#ff0000 w=1 fill=none')
        self.assertGreater(max(alphas(px)), 200)
        # what overflows a double draws nothing and takes no time
        self.ae(draw('g id=1 t=1e300,0,0,1e300,0,0\np id=2 g=1 d=M0,0L1e10,0L1e10,1e10Z fill=#ff0000 stroke=#00ff00 w=3'), nothing)
        self.ae(draw('g id=1 t=10,0,0,10,0,0\np id=2 g=1 d=M0,0L1,1 stroke=#ff0000 w=1e308 cap=round fill=none'), nothing)
        self.ae(draw('g id=1 t=10,0,0,10,0,0\nk id=2 g=1 pts=1,1,1e308 fill=#ff0000'), nothing)
        draw('p id=1 d=M1e308,1e308C-1e308,1e308,-1e308,-1e308,1e308,-1e308Z fill=#ff0000 stroke=#ff0000 w=1e308')
        # A stroke whose width dwarfs the image, along a curve all of which it reaches: the band of the curve
        # around the image that the stroke edge passes through can be as long as the stroke is wide
        self.ae(alphas(draw('p id=1 d=M-1e9,10C-1e9,-5.5e8,-5.5e8,-1e9,10,-1e9 stroke=#000000 w=2.0000002e9 fill=none')), {255})
        draw('p id=1 d=M-1e15,10C-1e15,-5.5e14,-5.5e14,-1e15,10,-1e15 stroke=#000000 w=2e15 fill=none join=bevel dash=1,1')
        # what is drawn of part of an image is what is drawn of all of it, whatever its coordinates
        doc = '\n'.join((
            f'k id=1 pts={2**50 + 10},10,{2**51} fill=#ff000080',
            'p id=2 d=M-1e17,10C-1e17,-1e17,1e17,-1e17,1e17,13Z fill=#00ff0080',
            'p id=3 d=M-1e12,-1e12L7,4L1e12,-1e12 stroke=#0000ff w=3 join=round dash=2,1 cap=round fill=none',
        ))
        i = next(ids)
        self.ae(parse_full_response(send_command(s, f'a=T,f=200,s=20,v=20,i={i}', doc)).code, 'OK')
        full = g.render_vector_image(i, 37, 29)
        part = bytearray()
        g.update_vector_pixels(i, 37, 29, part)
        self.ae(parse_full_response(send_command(s, f'a=e,i={i}', 'k id=4 pts=12,12,3 fill=#ffffff')).code, 'OK')
        rect = g.update_vector_pixels(i, 37, 29, part)
        self.assertLess(rect[2] * rect[3], 37 * 29 / 4)
        self.ae(bytes(part), g.render_vector_image(i, 37, 29))
        self.assertNotEqual(bytes(part), full)

    def test_vector_amplification(self):
        # Work and memory for a vector command are in proportion to its size and to what it changes, not to that
        # times the size of the document, and not many times that in memory.
        import signal
        s = self.create_screen()
        g = s.grman

        def command(records, action='t', image=1):
            # in chunks, as clients send large payloads
            from kitty.fast_data_types import VT_PARSER_MAX_ESCAPE_CODE_SIZE
            records = records.encode()
            step = (VT_PARSER_MAX_ESCAPE_CODE_SIZE - 64) // 4 * 3
            chunks = [records[i:i + step] for i in range(0, len(records), step)]
            signal.signal(signal.SIGALRM, signal.SIG_DFL)
            signal.alarm(120)
            try:
                start = time.monotonic()
                for k, chunk in enumerate(chunks):
                    more = int(k + 1 < len(chunks))
                    keys = f'a={action},f=200,s=10,v=10,i={image}' if k == 0 else (f'i={image}' if action == 'e' else '')
                    res = send_command(s, ','.join(x for x in (keys, f'm={more}') if x), chunk)
                res = parse_full_response(res)
                self.ae(res.code, 'OK', res.msg)
                return time.monotonic() - start
            finally:
                signal.alarm(0)

        n = 40000
        paths = ''.join(f'p id={i} d=M1,1L9,1L9,9Z fill=#ff0000\n' for i in range(1, n + 1))
        # with a clip group every path replaced asks whether it is a clip path
        command(paths + f'p id={n + 1} d=M0,0L10,0L10,10Z\ng id={n + 2} clip={n + 1}\n')
        self.assertLess(command(paths, 'e'), 10)
        # deleting groups looks at what is under them, not at the whole document
        self.assertLess(command(''.join(f'x g={i}\n' for i in range(n + 10, 2 * n + 10)), 'e'), 10)
        self.ae(g.image_for_client_id(1)['vector_elements'], n + 2)
        groups = ''.join(f'g id={i} p={i - 1}\n' for i in range(n + 3, n + 3 + 2000))
        self.assertLess(command(groups + ''.join(f'p id={i} g={n + 2002} d=M1,1L2,2 stroke=#00ff00\n' for i in range(1, 4000)), 'e'), 10)
        command('x g=0', 'e')
        self.ae(g.image_for_client_id(1)['vector_elements'], 0)
        # a nested chain of groups, with elements added deep inside it one record at a time
        chain = ''.join(f'g id={i} p={i - 1}\n' for i in range(1, n + 1))
        command(chain, image=2)
        self.assertLess(command(''.join(f'p id={n + i} g={n} d=M1,1L9,9 stroke=#ff0000\n' for i in range(1, n + 1)), 'e', 2), 10)
        self.ae(g.render_vector_image(2, 10, 10)[(5 * 10 + 5) * 4 + 3], 255)
        self.assertLess(command(f'x g={n // 2}\n', 'e', 2), 10)
        self.ae(g.image_for_client_id(2)['vector_elements'], n // 2 - 1)
        # a million deletes of a few bytes each are not held as a million elements while they are parsed
        self.assertLess(command('x id=1\n' * 1000000, 'e', 2), 30)
        # gradients of very many stops are looked up, not searched, at every pixel
        stops = ','.join(f'{i / 200000:.6f},#{i % 256:02x}0000' for i in range(200000))
        self.assertLess(command(f'l id=1 x1=0 y1=0 x2=10 y2=0 stops={stops}\np id=2 d=M0,0L10,0L10,10L0,10Z fill=@1\n', image=3), 10)
        start = time.monotonic()
        g.render_vector_image(3, 300, 300)
        self.assertLess(time.monotonic() - start, 10)

    def test_vector_image_damage(self):
        # A patch to a vector image draws only the part of it that changed.
        # The pixels kept up that way must be those of drawing all of it.
        s = self.create_screen()
        g = s.grman
        W, H = 120, 90
        # one image shown at its own size, one stretched by amounts that are not whole numbers
        sizes = {1: (W, H), 2: (173, 131)}
        pixels = {i: bytearray() for i in sizes}

        def patch(records, **kw):
            for i in sizes:
                res = parse_full_response(send_command(s, ','.join(f'{k}={v}' for k, v in dict(kw, i=i).items()), records))
                self.ae(res.code, 'OK', f'{res.msg}: {records}')

        def update(what=''):
            # the rectangles that were drawn again, as fractions of the image
            ans = []
            for i, (w, h) in sizes.items():
                rect = g.update_vector_pixels(i, w, h, pixels[i])
                self.assertTrue(pixels[i] == g.render_vector_image(i, w, h), f'Image {i} differs from a full render after: {what}')
                ans.append(0 if rect is None else rect[2] * rect[3] / (w * h))
            return ans

        def edit(records, how='some'):
            patch(records, a='e')
            for frac in update(records):
                if how == 'none':
                    self.ae(frac, 0, records)
                elif how == 'all':
                    self.ae(frac, 1, records)
                elif how == 'some':
                    self.assertTrue(0 < frac < 0.5, f'{frac} of the image was drawn again after: {records}')

        send_command(s, 'a=T,f=24,i=9,s=2,v=2', b'\xff\x00\x00\x00\xff\x00\x00\x00\xff\xff\xff\x00')
        patch(
            '\n'.join((
                'p id=1 d=M0,0L120,0L120,90L0,90Z fill=#fffffff0',
                'l id=2 x1=0 y1=0 x2=60 y2=0 stops=0,#ff000080,1,#0000ff',
                'p id=3 d=M5,5L65,5L65,35L5,35Z fill=@2 stroke=#00000080 w=3',
                'p id=4 d=M30,20L90,20L90,60L30,60Z fill=#00ff0060',
                's id=5 d=M0,0C3,-6,9,-6,12,0C9,6,3,6,0,0Z',
                'u id=6 s=5 t=1,0,0,1,70,70 fill=#804000',
                'u id=7 s=5 t=0,2,-2,0,100,40 stroke=#000080 w=1.5 fill=none',
                'i id=8 img=9 t=6,0,0,6,100,70 o=70',
                'g id=20 t=0.8,0.3,-0.3,0.8,35,5',
                'p id=30 d=M20,45L75,40L80,80L15,85Z',
                'g id=31 clip=30',
                'p id=32 g=31 d=M0,50L120,50L120,75L0,75Z fill=#ffff0090',
                'g id=40 o=50',
                'p id=41 g=40 d=M60,5L110,5L110,30L60,30Z fill=#ff00ff',
                'p id=42 g=40 d=M80,15L118,15L118,45L80,45Z fill=#00ffff',
                'g id=50 blend=multiply',
                'p id=51 g=50 d=M2,60L40,60L40,88L2,88Z fill=#c0c0ff',
            ))
            + '\n',
            a='T',
            f=200,
            s=W,
            v=H,
        )
        self.ae(update('transmission'), [1, 1])
        self.ae(update('nothing'), [0, 0])

        # a pen stroke, a point at a time: translucent, widths that vary, doubling back over itself
        edit('k id=100 pts=10,10,3 fill=#10203080')
        for x, y, w in ((14, 13, 4), (19.5, 12, 2.5), (25, 18, 6), (22, 24, 1), (15, 20, 5), (12, 12, 3), (30, 30, 0), (33.25, 30.5, 2)):
            edit(f'a id=100 pts={x},{y},{w}')
        edit('a id=100 pts=40,31,2,44,35,3,47,40,2')
        # with butt ends the old last point is drawn differently once it is no longer last
        edit('k id=101 pts=60,80,6 cap=butt fill=#000000')
        edit('a id=101 pts=70,82,6')
        edit('a id=101 pts=78,78,4')

        # stroked paths, growing by lines and curves, with every kind of join and end
        edit('p id=110 d=M8,70 fill=none stroke=#c00000a0 w=5 join=round cap=round')
        for d in ('L16,62', 'L24,71', 'Q30,60,36,70', 'C40,80,44,60,50,72', 'L50,72', 'L44,66'):
            edit(f'a id=110 d={d}')
        edit('p id=111 d=M70,10L80,12 fill=none stroke=#0060c0c0 w=3 join=miter ml=8 cap=square')
        for d in ('L72,14', 'L84,17', 'L74,18.5', 'Q90,25,76,24'):
            edit(f'a id=111 d={d}')
        edit('p id=112 d=M5,40L15,42 fill=none stroke=#208020 w=2 join=bevel cap=butt dash=6,3,1,2 dp=2')
        for d in ('L25,38', 'L27,48', 'C35,48,35,40,45,44', 'L60,44'):
            edit(f'a id=112 d={d}')
        # closing a subpath reaches back to its start, and a new subpath begins somewhere else
        edit('a id=112 d=Z', how=None)
        edit('a id=112 d=M100,5L110,8L104,12')
        edit('a id=112 d=Z')
        edit('a id=112 d=L112,14')
        # a fill is closed across its ends, so all of the element is drawn again
        edit('p id=113 d=M90,60L110,62L100,75 fill=#ff800090 stroke=#000000 w=1', how=None)
        edit('a id=113 d=L92,80', how=None)
        edit('a id=113 d=L85,70Z', how=None)

        # replacing an element draws where it was and where it is, deleting one where it was
        edit('k id=100 pts=50,50,4,56,58,2,62,52,5 fill=#40208080', how=None)
        edit('p id=4 d=M32,22L92,22L92,62L32,62Z fill=#00ff0060', how=None)
        edit('u id=6 s=5 t=1.5,0,0,1.5,60,74 fill=#804000')
        edit('k id=110 pts=8,70,2,20,64,3', how=None)
        edit('x id=111')
        edit('x id=101')
        edit('x id=77777', how='none')
        edit('a id=77777 pts=1,1,1', how='none')

        # strokes inside a group that is turned and scaled, one that is clipped, one drawn
        # through a layer and one that multiplies
        edit('k id=120 g=20 pts=10,10,3 fill=#000000c0')
        for x, y, w in ((16, 14, 4), (24, 12, 2), (30, 22, 5)):
            edit(f'a id=120 pts={x},{y},{w}')
        edit('k id=121 g=31 pts=10,55,4 fill=#0000ffb0')
        for x, y, w in ((22, 60, 6), (30, 52, 3), (45, 44, 5), (70, 38, 4)):
            edit(f'a id=121 pts={x},{y},{w}', how=None)
        edit('k id=122 g=40 pts=70,10,4 fill=#ffff00')
        for x, y, w in ((84, 20, 6), (96, 14, 3), (104, 28, 5)):
            edit(f'a id=122 pts={x},{y},{w}')
        edit('p id=123 g=50 d=M10,65L20,70 fill=none stroke=#ffc0c0 w=6 cap=round join=round')
        for d in ('L28,82', 'Q36,70,30,64'):
            edit(f'a id=123 d={d}')

        # nothing to draw for geometry off the image or for an append of nothing
        edit('k id=130 pts=500,500,4 fill=#000000', how='none')
        edit('a id=130 pts=520,510,4', how='none')
        edit('a id=130 pts=-90,-80,2', how=None)
        edit('a id=100 pts=', how=None)
        edit('a id=112 d=', how=None)
        # a stroke as wide as the image, and one with a miter limit that reaches right across it
        edit('k id=131 pts=60,45,300 fill=#00000010', how='all')
        edit('p id=132 d=M10,10L60,11L10,12 fill=none stroke=#00000020 w=4 ml=1000', how=None)

        # what cannot be bounded is drawn in full: groups, what other elements
        # refer to, raster images, the path a group is clipped by
        edit('g id=20 t=0.8,0.3,-0.3,0.8,36,5', how='all')
        edit('g id=0 t=1,0,0,1,1,0.5', how='all')
        edit('g id=60', how='all')
        edit('a id=5 d=M2,0L4,1', how='all')
        edit('s id=61 d=M0,0L1,1', how='all')
        edit('l id=2 x1=0 y1=0 x2=60 y2=0 stops=0,#00ff0080,1,#0000ff', how='all')
        edit('i id=8 img=9 t=5,0,0,5,100,70', how='all')
        edit('a id=30 d=M0,0L5,0L5,5Z', how='all')
        edit('p id=30 d=M20,45L75,40L80,80Z', how='all')
        edit('p id=33 d=M50,5L90,5L90,30 fill=none stroke=#000000\ng id=34 clip=33\np id=35 g=34 d=M0,0L120,0L120,90L0,90Z fill=#80808080', how='all')
        edit('a id=33 d=L50,30', how='all')
        edit('p id=36 d=M0,0L9,0L9,9Z fill=#ff0000\ng id=0 clip=36', how='all')
        edit('a id=36 d=M0,0L120,0L120,90L0,90Z', how='all')
        edit('g id=0', how='all')
        edit('x id=6\nx id=5', how='all')
        edit('x g=40', how='all')
        # in the coordinates of a transformed root, and after a failed patch
        edit('k id=140 pts=20,20,3,30,26,5 fill=#000000')
        edit('a id=140 pts=41,22,4')
        for i in sizes:
            self.ae(parse_full_response(send_command(s, f'a=e,i={i}', 'k id=141 pts=5,5,3\na id=141 d=M0,0L9,9\n')).code, 'EINVAL')
        self.ae(update('a patch that failed part way'), [1, 1])
        edit('x all', how='all')
        edit('k id=1 pts=5,5,4,9,9,4')

        # many patches of every kind, in an order that is always the same
        rnd = random.Random(20261013)

        def coord():
            return f'{rnd.uniform(-10, 130):.2f},{rnd.uniform(-10, 100):.2f}'

        def style():
            return (
                f'stroke=#{rnd.randrange(1 << 32):08x} w={rnd.uniform(0.3, 9):.2f} join={rnd.choice(("miter", "round", "bevel"))}'
                f' cap={rnd.choice(("butt", "round", "square"))}' + rnd.choice(('', f' dash={rnd.uniform(1, 9):.1f},{rnd.uniform(1, 5):.1f}'))
            )

        groups = (0, 0, 200, 201, 202)
        edit('p id=3 d=M10,10L110,20L100,80L20,70Z\ng id=200 t=0.9,-0.2,0.2,0.9,5,8\ng id=201 clip=3\ng id=202 p=200 o=60 blend=screen', how='all')
        for step in range(250):
            op = rnd.choice('kkkkpppaaaaaaaaaaaaaaaaaaaaxxrf')
            eid = 1000 + rnd.randrange(12)
            where = f' g={rnd.choice(groups)}' if rnd.random() < 0.5 else ''
            if op == 'k':
                rec = f'k id={eid}{where} pts={coord()},{rnd.uniform(0, 8):.1f} fill=#{rnd.randrange(1 << 32):08x} cap={rnd.choice(("round", "butt"))}'
            elif op == 'p':
                rec = f'p id={eid}{where} d=M{coord()} fill=none {style()}'
            elif op == 'f':
                rec = f'p id={eid}{where} d=M{coord()}L{coord()}L{coord()} fill=#{rnd.randrange(1 << 32):08x} rule={rnd.choice(("nz", "eo"))} {style()}'
            elif op == 'r':
                rec = f'u id={eid}{where} s=5000 t=1,0,0,1,{coord()} {style()}'
            elif op == 'x':
                rec = f'x id={eid}'
            else:
                rec = rnd.choice((
                    f'a id={eid} pts={coord()},{rnd.uniform(0, 8):.1f}',
                    f'a id={eid} pts={coord()},{rnd.uniform(0, 8):.1f},{coord()},{rnd.uniform(0, 8):.1f}',
                    f'a id={eid} d=L{coord()}',
                    f'a id={eid} d=Q{coord()},{coord()}',
                    f'a id={eid} d=C{coord()},{coord()},{coord()}L{coord()}',
                    f'a id={eid} d=Z',
                    f'a id={eid} d=M{coord()}L{coord()}',
                ))
            if step == 100:
                rec += '\ns id=5000 d=M-4,-4L4,-4L4,4L-4,4Z'
            for i in sizes:
                # an append to an element of the other kind fails, and whatever came before it in the patch stays
                send_command(s, f'a=e,i={i}', rec + '\n')
            update(f'step {step}: {rec}')

    def test_graphics_quota_enforcement(self):
        s = self.create_screen()
        g = s.grman
        g.storage_limit = 36 * 2
        li = make_send_command(s)
        # test quota for simple images
        self.assertEqual(li(a='T').code, 'OK')
        self.assertEqual(li(a='T', i=2).code, 'OK')
        self.assertEqual(g.disk_cache.total_size, g.storage_limit)
        self.assertEqual(g.image_count, 2)
        self.assertEqual(li(a='T', i=3).code, 'OK')
        self.assertEqual(g.disk_cache.total_size, g.storage_limit)
        self.assertEqual(g.image_count, 2)
        # test quota for frames
        for i in range(8):
            self.assertEqual(li(payload=f'{i}' * 36, i=2).code, 'OK')
        self.assertEqual(li(payload='x' * 36, i=2).code, 'ENOSPC')
        # test editing should not trigger quota
        self.assertEqual(li(payload='4' * 12, r=2, s=2, v=2, i=2).code, 'OK')

        s.reset()
        self.ae(g.image_count, 0)
        self.assertEqual(g.disk_cache.total_size, 0)

    def test_transient_image_preferential_eviction(self):
        # Transient images should be evicted before non-transient ones when
        # the storage quota is exceeded, regardless of insertion order.
        s = self.create_screen()
        g = s.grman
        g.storage_limit = 36 * 2
        li = make_send_command(s)
        # Load a non-transient image first (older atime) and a transient image second.
        self.assertEqual(li(a='T', i=1).code, 'OK')
        self.assertEqual(li(a='T', i=2, N=1).code, 'OK')
        self.assertEqual(g.image_count, 2)
        # Adding a third image triggers the quota; the transient image (i=2) must be
        # evicted first even though the non-transient image (i=1) is older.
        self.assertEqual(li(a='T', i=3).code, 'OK')
        self.assertEqual(g.image_count, 2)
        self.assertIsNone(g.image_for_client_id(2), 'transient image should have been evicted')
        self.assertIsNotNone(g.image_for_client_id(1), 'non-transient image should survive')
        self.assertIsNotNone(g.image_for_client_id(3), 'newly added image should survive')

    @unittest.skipIf(Image is None, 'PIL not available, skipping PNG tests')
    def test_cached_rgba_conversion(self):
        from kitty.render_cache import ImageRenderCacheForTesting

        w, h = 5, 3
        rgba_data = byte_block(w * h * 4)
        img = Image.frombytes('RGBA', (w, h), rgba_data)
        buf = BytesIO()
        img.save(buf, 'PNG')
        png_data = buf.getvalue()
        with tempfile.TemporaryDirectory() as cache_path:
            irc = ImageRenderCacheForTesting(cache_path)
            srcs, outputs = [], []
            for i in range(2 * irc.max_entries):
                with open(os.path.join(cache_path, f'{i}.png'), 'wb') as f:
                    f.write(png_data)
                srcs.append(f.name)
                outputs.append(irc.render(f.name))
                entries = list(irc.entries())
                self.assertLessEqual(len(entries), irc.max_entries)
            self.ae(irc.num_of_renders, len(outputs))
            remaining_outputs = outputs[-irc.max_entries :]
            for x in remaining_outputs:
                self.assertTrue(os.path.exists(x))
            for x in outputs[: -irc.max_entries]:
                self.assertFalse(os.path.exists(x))
            self.assertLess(os.path.getmtime(remaining_outputs[0]), os.path.getmtime(remaining_outputs[1]))
            remaining_srcs = srcs[-irc.max_entries :]
            self.ae(irc.render(remaining_srcs[0]), remaining_outputs[0])
            self.ae(irc.num_of_renders, len(outputs))
            self.assertGreater(os.path.getmtime(remaining_outputs[0]), os.path.getmtime(remaining_outputs[1]))

            width, height, fd = irc(remaining_srcs[-1])
            with open(fd, 'rb') as f:
                self.ae((width, height), (w, h))
                f.seek(8)
                self.ae(rgba_data, f.read())
