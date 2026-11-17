#!/usr/bin/env python
# License: GPL v3 Copyright: 2016, Kovid Goyal <kovid at kovidgoyal.net>

import os
import struct
import random
import sys
import tempfile
import time
import unittest
import zlib
from contextlib import suppress
from dataclasses import dataclass
from io import BytesIO

from kitty.constants import is_macos
from kitty.fast_data_types import base64_decode, base64_encode, load_png_data, shm_unlink, shm_write

from .base import BaseTest, parse_bytes
from .extension_frames import assert_extension_frames

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


def icc_profile(colorants, gamma=None):
    # A minimal RGB matrix profile: the D50 adapted XYZ of red, green and blue
    # and one curve for the three, the sRGB one unless a plain gamma is given
    import struct

    def s15f16(x):
        return struct.pack('>i', round(x * 65536))

    def xyz(v):
        return b'XYZ \0\0\0\0' + b''.join(map(s15f16, v))

    if gamma is None:
        curve = b'para\0\0\0\0' + struct.pack('>HH', 3, 0) + b''.join(map(s15f16, (2.4, 1 / 1.055, 0.055 / 1.055, 1 / 12.92, 0.04045)))
    else:
        curve = b'para\0\0\0\0' + struct.pack('>HH', 0, 0) + s15f16(gamma)
    d50 = (0.9642, 1.0, 0.8249)
    tags = [(b'wtpt', xyz(d50)), (b'rXYZ', xyz(colorants[0])), (b'gXYZ', xyz(colorants[1])), (b'bXYZ', xyz(colorants[2])), (b'rTRC', curve)]
    table, data, offset = b'', b'', 128 + 4 + 12 * (len(tags) + 2)
    for sig, payload in tags:
        payload += b'\0' * (-len(payload) % 4)
        table += sig + struct.pack('>II', offset + len(data), len(payload))
        if sig == b'rTRC':
            for other in (b'gTRC', b'bTRC'):
                table += other + struct.pack('>II', offset + len(data), len(payload))
        data += payload
    body = struct.pack('>I', len(tags) + 2) + table + data
    header = struct.pack('>I4sI4s4s4s', 128 + len(body), b'', 0x04200000, b'mntr', b'RGB ', b'XYZ ') + b'\0' * 12 + b'acsp' + b'\0' * 28
    header += b''.join(map(s15f16, d50)) + b'\0' * 48
    assert len(header) == 128
    return header + body


SRGB_COLORANTS = (0.4360747, 0.2225045, 0.0139322), (0.3850649, 0.7168786, 0.0971045), (0.1430804, 0.0606169, 0.7141733)
DISPLAY_P3_COLORANTS = (0.5151, 0.2412, -0.0011), (0.2920, 0.6922, 0.0419), (0.1571, 0.0666, 0.7841)
REC2020_COLORANTS = (0.67348, 0.27904, -0.00194), (0.16566, 0.67535, 0.02998), (0.12505, 0.04561, 0.79686)
ADOBE_RGB_COLORANTS = (0.6097559, 0.3111242, 0.0194811), (0.2052401, 0.6256560, 0.0608902), (0.1492240, 0.0632197, 0.7448387)


def make_png(rgba, width, *chunks, profile=None):
    # An RGBA PNG with the given chunks, and an embedded profile if any, ahead of the pixels
    import struct

    def chunk(kind, payload):
        return struct.pack('>I', len(payload)) + kind + payload + struct.pack('>I', zlib.crc32(kind + payload))

    height = len(rgba) // (4 * width)
    rows = b''.join(b'\0' + rgba[i * 4 * width : (i + 1) * 4 * width] for i in range(height))
    ans = b'\x89PNG\r\n\x1a\n' + chunk(b'IHDR', struct.pack('>IIBBBBB', width, height, 8, 6, 0, 0, 0))
    for kind, payload in chunks:
        ans += chunk(kind, payload)
    if profile is not None:
        ans += chunk(b'iCCP', b'icc\0\0' + zlib.compress(profile))
    return ans + chunk(b'IDAT', zlib.compress(rows)) + chunk(b'IEND', b'')


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

    def test_disk_cache_entries_changed_during_defrag(self):
        # Stale copies become holes
        s = self.create_screen()
        dc = s.grman.disk_cache
        dc.small_hole_threshold = 0
        dc.defrag_factor = 1000
        data = {}
        for i in range(20):
            key = f'k{i}'.encode()
            data[key] = bytes([65 + i]) * 100
            dc.add(key, data[key])
        self.assertTrue(dc.wait_for_write())
        for i in range(12):
            dc.remove(f'k{i}'.encode())
            del data[f'k{i}'.encode()]
        self.assertEqual(dc.end_of_data_offset(), 2000)
        dc.pause_writes()
        dc.defrag_factor = 2
        data[b'trigger'] = b'T' * 10
        dc.add(b'trigger', data[b'trigger'])
        self.assertTrue(dc.wait_until_writes_paused())
        dc.defrag_factor = 1000
        for i in range(12, 16):
            dc.remove(f'k{i}'.encode())
            del data[f'k{i}'.encode()]
        data[b'k16'] = b'r' * 60
        dc.add(b'k16', data[b'k16'])
        data[b'k17'] = b'm' * 70
        dc.add(b'k17', data[b'k17'], True)
        self.assertTrue(dc.resume_writes())
        self.assertTrue(dc.wait_for_write())
        for k, v in data.items():
            self.assertEqual(dc.get(k), v)
        holes = sorted(dc.holes())
        for (p1, s1), (p2, s2) in zip(holes, holes[1:]):
            self.assertLessEqual(p1 + s1, p2)
        on_disk = sum(len(v) for k, v in data.items() if k != b'k17')
        self.assertEqual(sum(x[1] for x in holes) + on_disk, dc.end_of_data_offset())
        # Memory only entries own no disk space
        dc.remove(b'k17')
        del data[b'k17']
        self.assertEqual(sorted(dc.holes()), holes)
        for k, v in data.items():
            self.assertEqual(dc.get(k), v)

    def test_disk_cache_empty_entry_replaced(self):
        dc = self.create_screen().grman.disk_cache
        dc.small_hole_threshold = 0
        dc.defrag_factor = 1000
        dc.add(b'a', b'A' * 1000)
        self.assertTrue(dc.wait_for_write())
        dc.add(b'z', b'')
        self.assertTrue(dc.wait_for_write())
        dc.remove(b'a')
        dc.add(b'c', b'C' * 100)
        self.assertTrue(dc.wait_for_write())
        dc.pause_writes()
        dc.defrag_factor = 2
        dc.add(b'z', b'Z' * 50)
        self.assertTrue(dc.wait_until_writes_paused())
        dc.defrag_factor = 1000
        self.assertTrue(dc.resume_writes())
        self.assertTrue(dc.wait_for_write())
        self.assertEqual(dc.end_of_data_offset(), 150)
        self.assertEqual(dc.get(b'z'), b'Z' * 50)
        self.assertEqual(dc.get(b'c'), b'C' * 100)

    def test_disk_cache_encryption_while_being_written(self):
        s = self.create_screen()
        dc = s.grman.disk_cache
        dc.set_needs_encryption(True)
        data = bytes(range(256)) * 1000 + b'xyz'

        # Reads while writing return plaintext
        dc.pause_writes()
        dc.add(b'k1', data)
        self.assertTrue(dc.wait_until_writes_paused())
        self.assertEqual(dc.get(b'k1'), data)
        self.assertTrue(dc.resume_writes())
        self.assertTrue(dc.wait_for_write())
        self.assertEqual(dc.num_cached_in_ram(), 0)
        self.assertEqual(dc.get(b'k1'), data)

        dc.pause_writes()
        dc.add(b'k2', data)
        self.assertTrue(dc.wait_until_writes_paused())
        dc.add(b'k2', data[::-1])
        self.assertTrue(dc.resume_writes())
        self.assertTrue(dc.wait_for_write())
        self.assertEqual(dc.get(b'k2'), data[::-1])
        self.assertEqual(dc.get(b'k1'), data)

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

        # Test feature queries
        def q(e, payload='', **kw):
            res = send_command(s, ','.join(f'{k}={v}' for k, v in dict(a='q', i=31, e=e, **kw).items()), payload)
            return res.decode('ascii') if res else None

        def r(keys=''):
            return f'\033_Gi=31{keys};OK\033\\'

        self.ae(q('T', s=1, v=1, f=24, U=1), r(',e=T,f=24,s=1,v=1,U=1'))
        self.ae(q('T', s=1, v=1, f=24, t='d', C=1, z=-3, N=1), r(',e=T,t=d,f=24,s=1,v=1,N=1,C=1,z=-3'))
        # no image data is needed or loaded
        self.ae(q('t', 'abcd', s=1, v=1, f=32, c=2), r(',e=t,f=32,s=1,v=1'))
        self.ae(q('t', 'abcd', s=1, v=1, f=8, c=2), r(',e=t,f=8,s=1,v=1'))
        # the colors of indexed color images are those of the terminal, they have no gamut and are never HDR
        self.ae(q('T', f=16, G=1, T=1, s=1, v=1), r(',e=T,f=16,s=1,v=1'))
        # formats that are not supported are not reported
        self.ae(q('t', s=1, v=1, f=12), r(',e=t,s=1,v=1'))
        self.ae(g.image_count, 0)
        # keys are interpreted according to the action being queried
        self.ae(q('p', c=2, r=3, U=1, C=1, s=1), r(',e=p,c=2,r=3,C=1,U=1'))
        # indexed color images cannot have animation frames
        self.ae(q('f', c=2, r=3, z=40, X=1, f=8, s=1), r(',e=f,s=1,c=2,r=3,z=40,X=1'))
        # Silkie's packed formats can be animation frames
        self.ae(q('f', f=308, s=1), r(',e=f,f=308,s=1'))
        self.ae(q('a', s=3, v=1, r=2, c=1, z=-1), r(',e=a,s=3,v=1,r=2,c=1,z=-1'))
        self.ae(q('c', r=1, c=2, w=3, X=1, C=1, U=1), r(',e=c,r=1,c=2,w=3,X=1,C=1'))
        self.ae(q('d', d='F', p=2, s=1), r(',e=d,d=F,p=2'))
        # unsupported values are not reported
        self.ae(q('T', s=1, v=1, f=24, C=7, N=6, U=2), r(',e=T,f=24,s=1,v=1'))
        self.ae(q('t', f=42, s=100000), r(',e=t'))
        self.ae(q('a', s=4), r(',e=a'))
        # Silkie's formats, gamuts, tone mapping, GPU surfaces and actions
        self.ae(q('T', s=1, v=1, f=64, G=1, T=1), r(',e=T,f=64,G=1,T=1,s=1,v=1'))
        self.ae(q('t', s=8, v=1, f=301), r(',e=t,f=301,s=8,v=1'))
        self.ae(q('t', s=8, v=1, f=1), r(',e=t,s=8,v=1'))
        self.ae(q('t', s=1, v=1, f=316, G=3, T=2), r(',e=t,f=316,s=1,v=1'))
        self.ae(q('t', f=200), r(',e=t,f=200'))
        self.ae(q('f', f=200, G=2, T=1), r(',e=f,G=2'))
        if is_macos or sys.platform.startswith('linux'):
            self.ae(q('t', t='g'), r(',e=t,t=g'))
        self.ae(q('t', t='g', o='z'), r(',e=t'))
        self.ae(q('f', t='g'), r(',e=f'))
        self.ae(q('r'), r(',e=r'))
        self.ae(q('e', t='f'), r(',e=e,t=f'))
        self.ae(q('v', f=200), r(',e=v,f=200'))
        self.ae(q('v', f=32, t='g'), r(',e=v'))
        # unknown actions are not reported
        self.ae(q('x', s=1, v=1, f=24, U=1), r())
        # a feature query does not disturb a chunked load in progress
        self.assertIsNone(pl('ab', i=99, s=1, v=1, f=24, m=1))
        self.ae(q('T', U=1), r(',e=T,U=1'))
        self.ae(pl('c', i=99, m=0), 'OK')
        self.ae(g.image_count, 1)
        s.reset()
        self.ae(g.image_count, 0)
        self.assertIsNone(q('T', U=1, q=1))

        # Test simple load
        for f in 32, 24:
            p = 'abc' + ('d' if f == 32 else '')
            img = sl(p, s=1, v=1, f=f)
            self.ae(bool(img['is_4byte_aligned']), f == 32)

        # Test HDR load
        self.ae(pl('abcdefgh', s=1, v=1, a='q', f=64), 'OK')
        self.ae(pl('abcdefg', s=1, v=1, f=64), 'ENODATA:Insufficient image data: 7 < 8')
        sl('abcdefgh' * 6, s=3, v=2, f=64)
        # Test GPU surfaces, a real one needs a GPU so only the error paths
        self.assertTrue(pl('dmabuf:1', t='g', i=5).startswith('EINVAL:'))
        self.assertTrue(pl('iosurface:0', a='q', t='g', i=5).startswith('EINVAL:'))
        self.ae(pl('', a='r', i=77), 'ENOENT:Refresh command refers to non-existent image with id: 77 and number: 0')
        self.ae(pl('', a='r', i=1), 'EINVAL:Refresh command refers to image with id: 1 that is not backed by a GPU surface')

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

    @unittest.skipUnless(sys.platform == 'linux', 'dma-bufs are Linux only')
    def test_dmabuf_surfaces(self):
        import fcntl
        import struct
        import subprocess
        from kitty.fast_data_types import dmabuf_parse_spec, dmabuf_take_fds
        s, g, pl, sl = load_helpers(self)

        # What a dma-buf surface names
        self.ae(dmabuf_parse_spec(b'dmabuf:42:AB24:0:7,0,256'), (42, 'AB24', 0, ((7, 0, 256),)))
        self.ae(dmabuf_parse_spec(b'dmabuf:42:XR24:0x0100000000000002:7,0,1024:7,65536,128'),
                (42, 'XR24', 0x0100000000000002, ((7, 0, 1024), (7, 65536, 128))))
        self.ae(dmabuf_parse_spec(b'dmabuf:1:AR24:0x00ffffffffffffff:3,4096,64')[2], 0x00ffffffffffffff)
        for bad, msg in (
            (b'iosurface:1', 'Not a dma-buf'), (b'dmabuf:0:AB24:0:7,0,256', 'process id'), (b'dmabuf:-1:AB24:0:7,0,256', 'process id'),
            (b'dmabuf:4294967296:AB24:0:7,0,256', 'process id'), (b'dmabuf:42:AB2:0:7,0,256', 'four characters'),
            (b'dmabuf:42:NV12:0:7,0,256', 'Unsupported dma-buf format: NV12'), (b'dmabuf:42:AB24:x:7,0,256', 'modifier'),
            (b'dmabuf:42:AB24:0x10000000000000000:7,0,256', 'modifier'), (b'dmabuf:42:AB24:0:', 'plane 1'), (b'dmabuf:42:AB24:0:7,0,0', 'plane 1'),
            (b'dmabuf:42:AB24:0:7,0', 'plane 1'), (b'dmabuf:42:AB24:0:7,0,256;', 'after plane 1'), (b'dmabuf:42:AB24:0:7,0,256:', 'plane 2'),
            (b'dmabuf:42:AB24:0:' + b':'.join([b'7,0,256'] * 5), 'at most 4 planes'),
        ):
            with self.assertRaises(ValueError, msg=bad) as cm:
                dmabuf_parse_spec(bad)
            self.assertIn(msg, str(cm.exception), bad)

        # Which processes the terminal takes descriptors from: only those in the session of the window's program
        me = os.getpid()
        r, w = os.pipe()
        try:
            def take(spec, session_of=me):
                with self.assertRaises(ValueError) as cm:
                    dmabuf_take_fds(spec.encode(), session_of)
                return str(cm.exception)
            self.assertIn('no program in this window', take(f'dmabuf:{me}:AB24:0:{r},0,4', 0))
            self.assertIn('is not a dma-buf', take(f'dmabuf:{me}:AB24:0:{r},0,4'))
            self.assertIn('Cannot take descriptor 9999', take(f'dmabuf:{me}:AB24:0:9999,0,4'))
            # a process id nobody has, pid_max itself is never given out
            with open('/proc/sys/kernel/pid_max') as f:
                pid_max = int(f.read())
            self.assertIn('Cannot open process', take(f'dmabuf:{pid_max}:AB24:0:{r},0,4'))
            other = subprocess.Popen(['sleep', '30'], start_new_session=True)
            try:
                self.assertIn('is not in the session', take(f'dmabuf:{other.pid}:AB24:0:0,0,4'))
                # and the other way round, a program in a session of its own cannot name the test's process
                self.assertIn('is not in the session', take(f'dmabuf:{me}:AB24:0:{r},0,4', other.pid))
            finally:
                other.kill()
                other.wait()
        finally:
            os.close(r), os.close(w)

        # The graphics command: dma-bufs are imported with EGL, which the terminal has only on Wayland
        self.assertIn('Wayland', pl(f'dmabuf:{me}:AB24:0:0,0,4', t='g', s=1, v=1, i=5))

        # A real dma-buf, made from memory with udmabuf where the user may
        try:
            dev = os.open('/dev/udmabuf', os.O_RDWR)
        except OSError:
            return
        try:
            mfd = os.memfd_create('kitty-test', os.MFD_ALLOW_SEALING)
            os.ftruncate(mfd, 4096)
            fcntl.fcntl(mfd, 1033, 2)  # F_ADD_SEALS, F_SEAL_SHRINK, which udmabuf requires
            # UDMABUF_CREATE: _IOW('u', 0x42, struct udmabuf_create {u32 memfd, flags; u64 offset, size})
            # a mutable buffer, so that the result of the call, the new descriptor, comes back
            dmabuf = fcntl.ioctl(dev, 0x40187542, bytearray(struct.pack('IIQQ', mfd, 1, 0, 4096)))
        except OSError:
            dmabuf = -1
        finally:
            os.close(dev)
        if dmabuf < 0:
            os.close(mfd)
            return
        try:
            fds = dmabuf_take_fds(f'dmabuf:{me}:AB24:0:{dmabuf},0,64'.encode(), me)
            self.ae(len(fds), 1)
            self.assertNotEqual(fds[0], dmabuf)
            self.ae(os.fstat(fds[0]).st_ino, os.fstat(dmabuf).st_ino)
            self.assertFalse(os.get_inheritable(fds[0]))
            os.close(fds[0])
            # from another process of the same session, which the terminal may trace being its parent
            child = subprocess.Popen(['sleep', '30'], pass_fds=(dmabuf,))
            try:
                fds = dmabuf_take_fds(f'dmabuf:{child.pid}:AB24:0:{dmabuf},0,64:{dmabuf},0,64'.encode(), me)
                self.ae(len(fds), 2)
                for fd in fds:
                    self.ae(os.fstat(fd).st_ino, os.fstat(dmabuf).st_ino)
                    os.close(fd)
            finally:
                child.kill()
                child.wait()
        finally:
            os.close(dmabuf), os.close(mfd)

    @unittest.skipUnless(sys.platform == 'darwin', 'IOSurfaces are macOS only')
    def test_surface_image_with_paused_rendering(self):
        # The copy of the images made while rendering is paused must not take
        # the GPU surface of a t=g image along, freeing the copy released it
        # a second time.
        from kitty.fast_data_types import iosurface_create_for_tests, iosurface_held_count
        s, g, pl, sl = load_helpers(self)
        base = iosurface_held_count()
        sid, handle = iosurface_create_for_tests(4, 2)
        self.ae(pl(f'iosurface:{sid}', t='g', i=5), 'OK')
        img = g.image_for_client_id(5)
        self.ae((img['width'], img['height']), (4, 2))
        self.ae(iosurface_held_count(), base + 1)
        # paused and resumed, which frees the copies
        self.assertTrue(s.pause_rendering(True, 5000))
        self.ae(iosurface_held_count(), base + 1)
        self.assertTrue(s.pause_rendering(False))
        self.ae(iosurface_held_count(), base + 1)
        self.ae(pl('', a='r', i=5), 'OK')
        # the image deleted while its copy is shown, then the copy freed
        self.assertTrue(s.pause_rendering(True, 5000))
        pl('', a='d', d='I', i=5)
        self.assertIsNone(g.image_for_client_id(5))
        self.ae(iosurface_held_count(), base)
        self.assertTrue(s.pause_rendering(False))
        self.ae(iosurface_held_count(), base)
        # a screen freed while paused frees its copies too
        self.ae(pl(f'iosurface:{sid}', t='g', i=6), 'OK')
        self.assertTrue(s.pause_rendering(True, 5000))
        self.ae(iosurface_held_count(), base + 1)
        del s, g, pl, sl
        import gc
        gc.collect()
        self.ae(iosurface_held_count(), base)
        del handle

    def test_inflate_failures(self):
        from kitty.fast_data_types import fail_next_graphics_allocation
        s, g, pl, sl = load_helpers(self)
        data = byte_block(4 * 16 * 8)
        z = zlib.compress(data)
        # Out of memory fails the command and nothing else
        fail_next_graphics_allocation('inflate')
        self.assertTrue(pl(z, s=16, v=8, o='z').startswith('ENOMEM:'))
        sl(z, s=16, v=8, o='z', expecting_data=data)
        # more or less data than the size of the image, and data that is not zlib
        for wrong in data + b'abcd', data[:-4]:
            self.ae(pl(zlib.compress(wrong), s=16, v=8, o='z'), 'EINVAL:Image data size post inflation does not match expected size')
        for bad in z[:len(z) // 2], b'not zlib data':
            r = pl(bad, s=16, v=8, o='z')
            self.assertTrue(r.startswith('EINVAL:Failed to inflate image data'), r)
        # a stream that ends exactly at the end of the image, compressed in many small blocks
        c = zlib.compressobj()
        many = b''.join(c.compress(data[i:i + 7]) + c.flush(zlib.Z_FULL_FLUSH) for i in range(0, len(data), 7)) + c.flush()
        sl(many, s=16, v=8, o='z', expecting_data=data)

    def test_out_of_memory(self):
        # Running out of memory fails the command, or skips what is being
        # drawn, and leaves everything else as it was
        from kitty.fast_data_types import fail_next_graphics_allocation
        s, g, pl, sl = load_helpers(self)
        self.assertRaises(KeyError, fail_next_graphics_allocation, 'nonsense')

        # a new image, and its texture
        for which in 'image', 'texture':
            fail_next_graphics_allocation(which)
            self.assertTrue(pl('abc', s=1, v=1, f=24, i=3).startswith('ENOMEM:'), which)
            self.ae(g.image_count, 0)
        sl('abc', s=1, v=1, f=24, i=3)
        # the texture of an image sent again, whose old pixels are gone already
        fail_next_graphics_allocation('texture')
        self.assertTrue(pl('def', s=1, v=1, f=24, i=3).startswith('ENOMEM:'))
        self.ae(g.image_count, 0)
        sl('def', s=1, v=1, f=24, i=3)
        # a free id for an image sent by number
        fail_next_graphics_allocation('client_ids')
        self.assertTrue(pl('ghi', s=1, v=1, f=24, i=0, I=17).startswith('ENOMEM:'))
        self.ae(g.image_count, 1)
        self.ae(pl('ghi', s=1, v=1, f=24, i=0, I=17), 'OK')
        self.ae(g.image_count, 2)
        self.ae(g.image_for_client_number(17)['client_id'], 1)
        # a placement
        fail_next_graphics_allocation('ref')
        self.assertTrue(pl('', a='p', i=3).startswith('ENOMEM:'))
        self.ae(g.image_for_client_id(3)['refs.count'], 0)
        self.ae(pl('', a='p', i=3), 'OK')
        self.ae(g.image_for_client_id(3)['refs.count'], 1)

    def test_out_of_memory_for_render_data(self):
        from kitty.fast_data_types import fail_next_graphics_allocation
        cw, ch = 10, 20
        s, dx, dy, put_image, put_ref, layers, rect_eq = put_helpers(self, cw, ch, cols=100, lines=10)
        self.ae(put_image(s, cw, ch, cursor_movement=1)[1], 'OK')
        # more placements than the render data has room for at first
        n = 70
        for i in range(1, n):
            self.ae(put_ref(s, id=1, placement_id=i + 1, cursor_movement=1)[1][0], 'OK')
        fail_next_graphics_allocation('render_data')
        self.assertLess(len(layers(s)), n)
        self.ae(len(layers(s)), n)

    def test_out_of_memory_enforcing_the_storage_quota(self):
        # Without the memory to sort the images the same ones are removed
        from kitty.fast_data_types import fail_next_graphics_allocation
        s = self.create_screen()
        g = s.grman
        g.storage_limit = 36 * 2
        li = make_send_command(s)
        self.assertEqual(li(a='T', i=1).code, 'OK')
        self.assertEqual(li(a='T', i=2, N=1).code, 'OK')
        fail_next_graphics_allocation('sorted')
        self.assertEqual(li(a='T', i=3).code, 'OK')
        self.assertEqual(g.image_count, 2)
        self.assertEqual(g.disk_cache.total_size, g.storage_limit)
        self.assertIsNotNone(g.image_for_client_id(1))
        self.assertIsNotNone(g.image_for_client_id(3))
        self.assertEqual(g.image_count, 2)
        fail_next_graphics_allocation('sorted')
        self.assertEqual(li(a='T', i=4).code, 'OK')
        self.assertEqual(g.image_count, 2)
        self.assertIsNotNone(g.image_for_client_id(3))
        self.assertIsNotNone(g.image_for_client_id(4))
        self.assertEqual(g.image_count, 2)

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
        self.ae(pl('kitty-test-shm-missing-no-leading-slash', s=1024, v=8, t='s'), generic_error)

        # Names without a leading slash must be accepted, as, for example,
        # sandboxed applications on macOS cannot use a leading slash
        name = 'kitty-test-shm-no-leading-slash'
        shm_write(name, random_data)
        sl(name, s=1024, v=8, t='s', expecting_data=random_data)
        self.assertRaises(FileNotFoundError, shm_unlink, name)  # check that the object was deleted

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

    def test_image_gamut(self):
        s, g, pl, sl = load_helpers(self)

        def close(a, b, by=1):
            self.ae(len(a), len(b))
            self.assertTrue(all(abs(x - y) <= by for x, y in zip(a, b)), f'{tuple(a)} is not within {by} of {tuple(b)}')

        # Pixels sent as they are say what gamut they are in, an unknown one is sRGB
        self.ae(sl('abcd', s=1, v=1)['gamut'], 0)
        for fmt, payload in ((32, 'abcd'), (24, 'abc')):
            for gamut, expected in ((0, 0), (1, 1), (2, 2), (3, 0), (77, 0)):
                self.ae(sl(payload, s=1, v=1, f=fmt, G=gamut)['gamut'], expected)
        self.ae(pl('abcd', s=1, v=1, a='q', G=1), 'OK')
        self.ae(pl('abcd', s=1, v=1, a='q', G=9), 'OK')
        # so does a chunked load, in its first chunk
        self.assertIsNone(pl('abcd', s=2, v=1, m=1, G=2))
        self.ae(pl('efgh', m=0), 'OK')
        img = g.image_for_client_id(1)
        self.ae((img['gamut'], img['data']), (2, b'abcdefgh'))
        # and sending the image again without one makes it sRGB again
        self.ae(sl('abcd', s=1, v=1)['gamut'], 0)
        # HDR pixels use extended sRGB; vector paints specify their own gamut.
        # Neither takes G, and replacing an image clears its old gamut tag.
        self.ae(sl('abcdefgh', s=1, v=1, f=64, G=1)['gamut'], 0)
        self.ae(sl('abcd', s=1, v=1, G=2)['gamut'], 2)
        self.ae(sl('abcdefgh', s=1, v=1, f=64, G=2)['gamut'], 0)
        self.ae(pl('abcdefgh', s=1, v=1, a='q', f=64, G=1), 'OK')
        self.ae(sl('abcd', s=1, v=1, G=1)['gamut'], 1)
        self.ae(pl('p id=1 d=M0,0L1,0L1,1Z', s=4, v=4, f=200, G=1), 'OK')
        self.ae(g.image_for_client_id(1)['gamut'], 0)
        self.ae(sl('abcd', s=1, v=1)['gamut'], 0)

        # A PNG says it with its profile. One in a wide gamut stays there, any other is made sRGB
        rgba = bytes((255, 0, 0, 255, 10, 200, 30, 128, 200, 50, 50, 255, 128, 128, 128, 255))

        def load(*chunks, profile=None, gamut=0):
            self.ae(pl(make_png(rgba, 2, *chunks, profile=profile), f=100), 'OK')
            img = g.image_for_client_id(1)
            self.ae((img['gamut'], img['width'], img['height']), (gamut, 2, 2))
            return img['data']

        self.ae(load(), rgba)
        close(load(profile=icc_profile(SRGB_COLORANTS)), rgba)
        close(load(profile=icc_profile(DISPLAY_P3_COLORANTS), gamut=1), rgba)
        close(load(profile=icc_profile(REC2020_COLORANTS), gamut=2), rgba)
        # a profile with another curve is brought to the sRGB curve, the alpha is left alone
        linear_p3 = load(profile=icc_profile(DISPLAY_P3_COLORANTS, gamma=1), gamut=1)
        close(linear_p3, bytes((255, 0, 0, 255, 56, 229, 96, 128, 229, 122, 122, 255, 188, 188, 188, 255)))
        adobe = load(profile=icc_profile(ADOBE_RGB_COLORANTS, gamma=2.19921875))
        self.assertNotEqual(adobe[8:11], rgba[8:11])
        self.ae(adobe[3::4], rgba[3::4])
        # the sRGB chunk and a profile are not meant to be in one file, the chunk wins as before
        self.ae(load((b'sRGB', b'\0'), profile=icc_profile(DISPLAY_P3_COLORANTS)), rgba)
        # a profile takes precedence over a gamma
        close(load((b'gAMA', (45455).to_bytes(4, 'big')), profile=icc_profile(DISPLAY_P3_COLORANTS), gamut=1), rgba)
        # code points in place of a profile: the pixels are what they say, only the sRGB curve is understood
        self.ae(load((b'cICP', bytes((12, 13, 0, 1))), gamut=1), rgba)
        self.ae(load((b'cICP', bytes((9, 13, 0, 1))), gamut=2), rgba)
        self.ae(load((b'cICP', bytes((1, 13, 0, 1)))), rgba)
        self.ae(load((b'cICP', bytes((9, 16, 0, 1)))), rgba)
        # elsewhere PNG is always read as sRGB
        self.assertNotEqual(load_png_data(make_png(rgba, 2, profile=icc_profile(DISPLAY_P3_COLORANTS)))[0][4:7], rgba[4:7])
        self.ae(load_png_data(make_png(rgba, 2))[0], rgba)

        # The frames of an image are all in its gamut, a frame in another one is converted when it is loaded
        srgb_red, p3_for_srgb_red = bytes((255, 0, 0, 255)), bytes((234, 51, 35, 255))

        def frames():
            return tuple(f['data'] for f in g.image_for_client_id(1)['extra_frames'])

        self.ae(sl(p3_for_srgb_red, s=1, v=1, G=1)['gamut'], 1)
        self.ae(pl(p3_for_srgb_red, a='f', s=1, v=1, G=1).partition(':')[0], 'OK')
        self.ae(pl(srgb_red, a='f', s=1, v=1).partition(':')[0], 'OK')
        self.ae(pl(make_png(srgb_red, 1), a='f', f=100).partition(':')[0], 'OK')
        self.ae(pl(make_png(p3_for_srgb_red, 1, profile=icc_profile(DISPLAY_P3_COLORANTS)), a='f', f=100).partition(':')[0], 'OK')
        self.ae(len(frames()), 4)
        self.ae(frames()[0], p3_for_srgb_red)
        for frame in frames()[1:]:
            close(frame, p3_for_srgb_red)
        self.ae(g.image_for_client_id(1)['gamut'], 1)
        self.ae(sl(srgb_red, s=1, v=1)['gamut'], 0)
        self.ae(pl(p3_for_srgb_red, a='f', s=1, v=1, G=1).partition(':')[0], 'OK')
        self.ae(pl(make_png(p3_for_srgb_red, 1, (b'cICP', bytes((12, 13, 0, 1)))), a='f', f=100).partition(':')[0], 'OK')
        self.ae(pl(srgb_red[:3], a='f', s=1, v=1, f=24).partition(':')[0], 'OK')
        self.ae(len(frames()), 3)
        close(frames()[0], srgb_red)
        self.ae(frames()[2], srgb_red[:3])
        self.ae(g.image_for_client_id(1)['gamut'], 0)

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

    def test_indexed_color_images(self):
        s, g, pl, sl = load_helpers(self)
        li = make_send_command(s)

        # Loading
        self.ae(pl('ab', s=1, v=1, f=8, a='q'), 'OK')
        self.ae(g.image_count, 0)
        img = sl(bytes(range(8)), s=4, v=2, f=8)
        self.ae(img['indexed_px_sz'], 1)
        img = sl(bytes(range(8)), s=2, v=2, f=16)
        self.ae(img['indexed_px_sz'], 2)
        data = bytes(range(256)) * 4
        img = sl(zlib.compress(data), s=32, v=16, f=16, o='z', expecting_data=data)
        self.ae(img['indexed_px_sz'], 2)
        self.ae(pl(bytes(range(3)), s=3, v=1, f=8), 'OK')
        img = g.image_for_client_id(1)
        self.ae((img['indexed_px_sz'], img['is_4byte_aligned'], img['data']), (1, False, bytes(range(3))))
        self.assertTrue(pl(bytes(range(5)), s=3, v=1, f=16).startswith('ENODATA'))
        self.assertTrue(pl(b'', s=0, v=1, f=8).startswith('EINVAL'))

        # Replacing an indexed image with a normal one
        img = sl(b'abcd', s=1, v=1, f=32)
        self.ae(img['indexed_px_sz'], 0)

        # Animation is not supported
        def unsupported(r, what='Animation'):
            self.ae((r.code, r.msg), ('EINVAL', f'{what} is not supported for indexed color images'))

        self.ae(li(b'abcd', s=2, v=2, f=8, a='t').code, 'OK')
        unsupported(li(b'abcd', s=2, v=2, f=8, a='f'))
        unsupported(li(b'abcd', s=1, v=1, f=32, a='f'))
        unsupported(li(b'', a='c', r=1, c=1, w=1, h=1, x=1, s=0, v=0, f=0), 'Composition')
        self.ae(g.image_for_client_id(1)['extra_frames'], ())
        self.ae(li(b'abcd', s=1, v=1, f=32, a='t').code, 'OK')
        unsupported(li(b'abcd', s=2, v=2, f=8, a='f'))

        # Render data
        cw, ch = 10, 20
        s, dx, dy, put_image, put_ref, layers, rect_eq = put_helpers(self, cw, ch)
        self.ae(put_image(s, 10, 20, id=1)[1], 'OK')
        self.ae(parse_response(send_command(s, 'a=T,f=16,i=2,s=2,v=2,z=-1', b'\x01\xff' * 4)), 'OK')
        self.ae(parse_response(send_command(s, 'a=T,f=8,i=3,s=2,v=2,z=1', b'\xf8' * 4)), 'OK')
        self.ae(tuple((x['image_id'], x['indexed_px_sz']) for x in layers(s)), ((2, 2), (1, 0), (3, 1)))
        self.ae(s.grman.image_count, 3)

        # Refs drawn at natural size use a cheaper program, so must not share
        # a group with scaled refs of the same image
        s, dx, dy, put_image, put_ref, layers, rect_eq = put_helpers(self, cw, ch)
        self.ae(parse_response(send_command(s, 'a=T,f=8,i=1,s=2,v=2', b'\x01' * 4)), 'OK')
        self.ae(parse_response(send_command(s, 'a=p,i=1,c=1,r=1')), 'OK')
        self.ae(parse_response(send_command(s, 'a=p,i=1')), 'OK')
        self.ae(parse_response(send_command(s, 'a=T,f=32,i=2,s=2,v=2', b'abcd' * 4)), 'OK')
        self.ae(tuple((x['is_unscaled'], x['group_count']) for x in layers(s)), ((True, 1), (False, 1), (True, 1), (False, 1)))

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

    def test_image_layers_with_paused_rendering(self):
        # While rendering is paused, images are drawn from a frozen copy of the
        # graphics manager, so they must be positioned using the scroll state
        # captured with the frozen text, not the live one, otherwise output
        # received during the pause shifts them relative to the text.
        cw, ch = 10, 20

        def screen(pixel_scroll=False, lines=5, before=10, after=20):
            s = self.create_screen(10, lines, scrollback=50, cell_width=cw, cell_height=ch, options={'pixel_scroll': pixel_scroll})
            for i in range(before):
                parse_bytes(s, f'row{i:03d}\r\n'.encode())
            # The image is placed in the cell just after the text IMG
            s.draw('IMG')
            send_command(s, 'a=T,f=24,i=1,s=10,v=20,c=1,r=1,C=1,q=2', b'x' * 10 * 20 * 3)
            s.carriage_return(), s.linefeed()
            for i in range(after):
                parse_bytes(s, f'row{i + before:03d}\r\n'.encode())
            return s

        def dy(s):
            return 2 / s.lines

        def live_layers(s):
            offset = s.pixel_scroll_offset_y / ch if s.pixel_scroll_offset_y else 0
            return s.grman.update_layers(s.scrolled_by, -1, 1, 2 / s.columns, dy(s), s.columns, s.lines, cw, ch, offset)

        def image_rows(s, layers):
            offset = s.rendered_scroll_state()[1] / ch
            return [round((1 - r['dest_rect']['top']) / dy(s) - offset) for r in layers]

        def text_row(s):
            for y in range(s.lines):
                if str(s.visual_line(y)).startswith('IMG'):
                    return y

        def assert_layers_close(a, b):
            # Positions computed from different scroll states differ by float rounding
            self.ae(len(a), len(b))
            for x, y in zip(a, b):
                self.ae({k: v for k, v in x.items() if k != 'dest_rect'}, {k: v for k, v in y.items() if k != 'dest_rect'})
                for side in ('left', 'top', 'right', 'bottom'):
                    self.assertAlmostEqual(x['dest_rect'][side], y['dest_rect'][side], places=5)

        def assert_aligned(s, layers, msg=''):
            row = text_row(s)
            self.assertIsNotNone(row, msg)
            self.ae(image_rows(s, layers), [row], msg)

        # Not paused: matches positioning with the live scroll state
        for scrolled_by in (0, 5, 16, 17, 19, 21, 22):
            s = screen()
            s.scroll(scrolled_by, True)
            self.ae(s.scrolled_by, scrolled_by)
            layers = s.update_graphics_layers()
            self.ae(layers, live_layers(s))
            if 17 <= scrolled_by <= 21:
                assert_aligned(s, layers, f'not paused, scrolled by {scrolled_by}')
            else:
                self.ae(layers, ())  # image is not visible

        # Output arriving while paused and scrolled back
        for num_new in (1, 3, 4, 10):
            with self.subTest(num_new=num_new):
                s = screen()
                s.scroll(19, True)
                before = s.update_graphics_layers()
                self.ae(len(before), 1)
                assert_aligned(s, before, 'before pause')
                parse_bytes(s, b'\x1b[?2026h')
                for i in range(num_new):
                    parse_bytes(s, f'new{i}\r\n'.encode())
                self.ae(s.scrolled_by, 19 + num_new)
                during = s.update_graphics_layers()
                self.ae(during, before)
                assert_aligned(s, during, 'during pause')
                parse_bytes(s, b'\x1b[?2026l')
                after = s.update_graphics_layers()
                self.ae(after, live_layers(s))
                assert_aligned(s, after, 'after pause')
                assert_layers_close(after, before)  # the view is anchored on the same content

        # Several synchronized updates before the end of the pause
        s = screen()
        s.scroll(19, True)
        before = s.update_graphics_layers()
        parse_bytes(s, b'\x1b[?2026hnew\r\n\x1b[?2026l\x1b[?2026hmore\r\n')
        self.ae(s.scrolled_by, 21)
        # The second pause captures a different, anchored, scroll position, so
        # the frozen positions match the original only up to float rounding
        assert_layers_close(s.update_graphics_layers(), before)
        self.assertTrue(s.pause_rendering(False))
        assert_aligned(s, s.update_graphics_layers(), 'after consecutive updates')

        # Pixel scrolling: the fractional line offset is also frozen
        for position in (18.5, 19.25):
            with self.subTest(position=position):
                s = screen(pixel_scroll=True)
                s.scroll_to_absolute(position)
                self.assertNotEqual(s.pixel_scroll_offset_y, 0)
                before = s.update_graphics_layers()
                self.ae(before, live_layers(s))
                self.ae(len(before), 1)
                parse_bytes(s, b'\x1b[?2026h')
                for i in range(3):
                    parse_bytes(s, f'new{i}\r\n'.encode())
                self.ae(s.update_graphics_layers(), before)
                parse_bytes(s, b'\x1b[?2026l')
                after = s.update_graphics_layers()
                self.ae(after, live_layers(s))
                assert_layers_close(after, before)  # the view is anchored on the same content

        # Paused at the bottom: output scrolls the live image up, the frozen one stays
        s = screen(before=2, after=1)
        self.ae(s.scrolled_by, 0)
        before = s.update_graphics_layers()
        assert_aligned(s, before, 'at bottom')
        parse_bytes(s, b'\x1b[?2026h')
        for i in range(2):
            parse_bytes(s, f'new{i}\r\n'.encode())
        self.ae(s.scrolled_by, 0)
        self.ae(s.update_graphics_layers(), before)
        self.assertNotEqual(live_layers(s), before)
        parse_bytes(s, b'\x1b[?2026l')
        after = s.update_graphics_layers()
        self.ae(image_rows(s, after), [r - 2 for r in image_rows(s, before)])
        assert_aligned(s, after, 'at bottom after pause')

        # Filling from the scrollback during the pause moves the live view to the bottom
        s = screen(after=4)
        s.scroll(1, True)
        self.ae(s.scrolled_by, 1)
        before = s.update_graphics_layers()
        self.ae(len(before), 1)
        parse_bytes(s, b'\x1b[?2026h\x1b[+T')
        self.ae(s.scrolled_by, 0)
        self.ae(s.update_graphics_layers(), before)
        self.assertTrue(s.pause_rendering(False))
        self.ae(s.update_graphics_layers(), live_layers(s))

        # Images deleted during the pause remain in the frozen frame
        s = screen()
        s.scroll(19, True)
        before = s.update_graphics_layers()
        parse_bytes(s, b'\x1b[?2026hnew\r\n')
        send_command(s, 'a=d,d=I,i=1,q=2')
        self.ae(s.update_graphics_layers(), before)
        parse_bytes(s, b'\x1b[?2026l')
        self.ae(s.update_graphics_layers(), ())

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

    def test_animation_frame_delete_dependents(self):
        s = self.create_screen()
        g = s.grman
        li = make_send_command(s)

        def t(**kw):
            self.assertEqual(li(**kw).code, 'OK')

        def load_frames():
            t(a='t')
            for i in '234':
                t(payload=i * 36)

        # Preserve the successor display
        load_frames()
        self.assertIsNone(li(a='a', i=1, c=3))
        img = g.image_for_client_id(1)
        self.assertEqual(img['displayed_frame_id'], img['extra_frames'][1]['id'])
        successor = img['extra_frames'][2]['id']
        self.assertIsNone(li(a='d', d='f', i=1, r=3))
        img = g.image_for_client_id(1)
        self.assertEqual(img['current_frame_index'], 2)
        self.assertEqual(img['displayed_frame_id'], successor)
        self.assertEqual([f['data'] for f in img['extra_frames']], [b'2' * 36, b'4' * 36])
        s.reset()

        # Preserve the root successor display
        load_frames()
        successor = g.image_for_client_id(1)['extra_frames'][0]['id']
        self.assertIsNone(li(a='a', i=1, c=1))
        self.assertIsNone(li(a='d', d='f', i=1, r=1))
        img = g.image_for_client_id(1)
        self.assertEqual(img['current_frame_index'], 0)
        self.assertEqual(img['displayed_frame_id'], successor)
        s.reset()

        # Preserve the last frame display
        load_frames()
        self.assertIsNone(li(a='a', i=1, c=4))
        self.assertIsNone(li(a='d', d='f', i=1, r=4))
        img = g.image_for_client_id(1)
        self.assertEqual(img['current_frame_index'], 2)
        self.assertEqual(img['displayed_frame_id'], img['extra_frames'][1]['id'])
        s.reset()

        # Preserve dependent pixels
        t(a='t')
        t(payload='2' * 36)
        t(payload='4' * 12, c=2, s=2, v=2)
        expected = b'444444222222444444222222222222222222'
        self.assertEqual(g.image_for_client_id(1)['extra_frames'][1]['data'], expected)
        self.assertIsNone(li(a='d', d='f', i=1, r=2))
        img = g.image_for_client_id(1)
        self.assertEqual(len(img['extra_frames']), 1)
        self.assertEqual(img['extra_frames'][0]['data'], expected)
        s.reset()

        # Preserve root dependents
        t(a='t')
        t(payload='4' * 12, c=1, s=2, v=2)
        expected = g.image_for_client_id(1)['extra_frames'][0]['data']
        self.assertIsNone(li(a='d', d='f', i=1, r=1))
        img = g.image_for_client_id(1)
        self.assertEqual(img['data'], expected)
        self.assertFalse(img['extra_frames'])
        s.reset()
        self.assertEqual(g.disk_cache.total_size, 0)

        # Respect the cache quota
        g.storage_limit = 36 * 2
        t(a='t')
        for i in range(10):
            t(payload='4' * 12, c=1, s=2, v=2)
        before = g.image_for_client_id(1)
        size = g.disk_cache.total_size
        self.assertIsNone(li(a='d', d='f', i=1, r=1))
        after = g.image_for_client_id(1)
        self.assertEqual(g.disk_cache.total_size, size)
        self.assertEqual(after['data'], before['data'])
        self.assertEqual([f['data'] for f in after['extra_frames']], [f['data'] for f in before['extra_frames']])
        s.reset()
        self.assertEqual(g.disk_cache.total_size, 0)

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

    def test_hdr_animation_frames(self):
        from struct import pack, unpack

        s = self.create_screen()
        g = s.grman
        li = make_send_command(s)

        def px(*channels):
            return pack('<4e', *channels)

        def pixels(data):
            return tuple(unpack('<4e', data[i:i + 8]) for i in range(0, len(data), 8))

        self.assertEqual(li(payload=px(0.5, 0.5, 0.5, 1.0) * 4, s=2, v=2, f=64, a='t').code, 'OK')
        # frames must have the format of the image
        self.assertEqual(li(payload=b'\0' * 16, s=2, v=2, f=32).code, 'EINVAL')
        self.assertEqual(li(payload=b'\0' * 32, s=2, v=2, f=64, i=2, a='t').code, 'OK')
        self.assertEqual(li(payload=b'\0' * 16, s=2, v=2, f=32, i=2, a='t').code, 'OK')
        self.assertEqual(li(payload=px(1, 1, 1, 1), s=1, v=1, f=64, i=2).code, 'EINVAL')
        # a half transparent frame blended in linear light over the root frame
        self.assertEqual(li(payload=px(2.0, 0.0, 0.0, 0.5), s=1, v=1, f=64, x=1, y=1, c=1, z=10).code, 'OK')
        img = g.image_for_client_id(1)
        self.assertEqual(len(img['extra_frames'][0]['data']), 32)
        self.assertEqual(pixels(img['extra_frames'][0]['data']), ((0.5, 0.5, 0.5, 1.0),) * 3 + ((1.25, 0.25, 0.25, 1.0),))
        # the same frame without a base is composed over transparent
        self.assertEqual(li(payload=px(2.0, 0.0, 0.0, 0.5), s=1, v=1, f=64, x=1, y=1, z=10).code, 'OK')
        img = g.image_for_client_id(1)
        self.assertEqual(pixels(img['extra_frames'][1]['data']), ((0.0, 0.0, 0.0, 0.0),) * 3 + ((2.0, 0.0, 0.0, 0.5),))
        # composition between frames
        self.assertEqual(li(payload=b'', a='c', f=0, s=0, v=0, r=2, c=1, w=1, h=1, X=1, Y=1).code, 'OK')
        img = g.image_for_client_id(1)
        self.assertEqual(pixels(img['data']), ((1.25, 0.25, 0.25, 1.0),) + ((0.5, 0.5, 0.5, 1.0),) * 3)
        # frame background color
        self.assertEqual(li(payload=px(1.0, 1.0, 1.0, 1.0), s=1, v=1, f=64, Y=0xff00007f).code, 'OK')
        img = g.image_for_client_id(1)
        self.assertEqual(pixels(img['extra_frames'][2]['data']), ((1.0, 1.0, 1.0, 1.0),) + ((1.0, 0.0, 0.0, unpack('<e', pack('<e', 127 / 255))[0]),) * 3)
        # the peak used for tone mapping covers all frames
        self.assertEqual((img['tone_map'], img['peak']), (False, 2.0))
        self.assertEqual(li(payload=px(3.0, 0.25, 0.25, 1.0), s=1, v=1, f=64, i=3, a='t', T=1).code, 'OK')
        img = g.image_for_client_id(3)
        self.assertEqual((img['tone_map'], img['peak']), (True, 3.0))
        self.assertEqual(li(payload=b'abcd', s=1, v=1, f=32, i=3, a='t', T=1).code, 'OK')
        img = g.image_for_client_id(3)
        self.assertEqual((img['tone_map'], img['peak']), (True, 0.0))

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

    def test_terminal_side_ink(self):
        # A program gives the terminal a brush for a vector image, and the terminal then draws the strokes of
        # the pen itself, by applying to the image the records the program would have sent, see
        # docs/ink-protocol.rst. What it draws must be exactly what those records draw.
        import math

        from kitty.fast_data_types import send_pointer_report

        cw, ch = 10, 20
        s = self.create_screen(cols=30, lines=12, cell_width=cw, cell_height=ch)
        g = s.grman
        c = s.callbacks

        def gr(payload='', **kw):
            return parse_full_response(send_command(s, ','.join(f'{k}={v}' for k, v in kw.items()), payload))

        def osc(selector, body):
            c.clear()
            parse_bytes(s, f'\x1b]777;{selector};v=1:{body}\x1b\\'.encode())
            return c.wtcbuf.decode()

        def ink(body):
            return osc('ink', body)

        def report(phase, x, y, p=None, tool=1, device=7, delivered=True):
            # a report of a tool at x, y in pixels of the cell area, returns its ink field
            c.clear()
            kw = {} if p is None else {'valid': 1, 'p': p}
            self.ae(send_pointer_report(s, 1, phase, device, 0, round(x * 256), round(y * 256), tool=tool, buttons=int(phase in (1, 2)), **kw), delivered)
            if not delivered:
                self.ae(c.wtcbuf, b'')
                return None
            fields = dict(f.split('=') for f in c.wtcbuf.decode().split(';')[2].rstrip('\x1b\\').split(':'))
            self.ae(fields['phase'], str(phase))
            return int(fields['ink']) if 'ink' in fields else None

        def num(v):
            return '%.17g' % v

        def point(x, y, p, place, w0, w1, e=1.0, to_group=None):
            # the point a report adds to a stroke, by the formulas of the specification
            left, top, width, height, sx, sy, sw, sh = place
            ux = sx + (x - left) * sw / width
            uy = sy + (y - top) * sh / height
            if to_group is not None:
                ux, uy = to_group(ux, uy)
            f = 1.0 if p is None else p / 65535.0
            return f'{num(ux)},{num(uy)},{num(w0 + (w1 - w0) * math.pow(f, e))}'

        def stroke(element, points, fill, group=0):
            g_ = f' g={group}' if group else ''
            return f'k id={element}{g_} pts={",".join(points)} fill={fill}\n'

        def elements(i=1):
            return g.image_for_client_id(i)['vector_elements']

        base = 'p id=1 d=M0,0L100,0L100,80L0,80Z fill=#ffffff\np id=2 d=M10,10L60,70 stroke=#0000ff w=3 fill=none\n'

        def same_as(records, base=base, i=1):
            # the image the terminal drew on against the same scene with the strokes sent as records
            self.ae(gr(base + records, a='t', f=200, i=99, s=100, v=80).code, 'OK')
            for w, h in ((100, 80), (173, 131)):
                self.assertTrue(g.render_vector_image(i, w, h) == g.render_vector_image(99, w, h), f'Ink differs from the records:\n{records}')

        # nothing is inked until a brush is given, and the query says what there is
        self.ae(ink('op=query:id=3'), '\x1b]777;ink;v=1:op=cap:id=3:caps=1:mode=0\x1b\\')
        self.ae(ink('op=mode'), '\x1b]777;ink;v=1:op=mode:mode=0\x1b\\')
        c.clear()
        parse_bytes(s, b'\x1b]777;ink;v=2:op=query:id=3\x1b\\')
        parse_bytes(s, b'\x1b]777;ink;v=1:op=query\x1b\\')
        parse_bytes(s, b'\x1b]777;ink;garbage\x1b\\')
        self.ae(c.wtcbuf, b'')
        self.ae(str(s.line(0)), '')
        # A frame from a program follows the rules every extension shares, see docs/extension-framing.rst
        assert_extension_frames(self, s, 'ink')

        parse_bytes(s, b'\x1b[2;3H')  # the image goes at the third column of the second line
        self.ae(gr(base, a='T', f=200, i=1, s=100, v=80, C=1).code, 'OK')
        plain = (2 * cw, ch, 100, 80, 0, 0, 100, 80)
        osc('tablet-pointer', 'op=mode:mode=1')
        self.assertIsNone(report(1, 30, 30, 20000))
        report(3, 30, 30)
        self.ae(elements(), 2)

        # a brush that cannot be used changes nothing
        for bad in (
            'k=100:w1=5', 'i=1:w1=5', 'i=1:k=100', 'i=0:k=100:w1=5', 'i=1:k=0:w1=5', 'i=1:k=100:n=0:w1=5', 'i=1:k=4294967295:n=2:w1=5',
            'i=1:k=100:w1=-1', 'i=1:k=100:w1=5:w0=x', 'i=1:k=100:w1=5:e=0', 'i=1:k=100:w1=5:fill=none', 'i=1:k=100:w1=5:fill=red',
            'i=1:k=100:w1=5:tools=0', 'i=1:k=100:w1=5:tools=512', 'i=1:k=1x:w1=5', 'i=1:k=100:w1=inf', 'i=1:k=100:w1=5:w1=6',
        ):
            self.ae(ink('op=brush:' + bad), '')
            self.ae(ink('op=mode'), '\x1b]777;ink;v=1:op=mode:mode=0\x1b\\', bad)
        self.ae(ink('op=mode:mode=1'), '')  # there is no brush to turn on
        self.ae(ink('op=mode'), '\x1b]777;ink;v=1:op=mode:mode=0\x1b\\')
        # nothing caps the length of the fill, the widths or the exponent, and a query reports them in full
        fill = '#ff0000*1.' + '0' * 400
        self.ae(ink(f'op=brush:i=1:k=100:fill={fill}:w0=2000000000:w1=3000000000:e=1000'), '')
        self.ae(ink('op=query:id=6'), f'\x1b]777;ink;v=1:op=cap:id=6:caps=1:mode=1:i=1:g=0:k=100:n=1:fill={fill}:w0=2e+09:w1=3e+09:e=1e+03:tools=58\x1b\\')
        self.ae(ink('op=mode:mode=0'), '')

        # a stroke: the report that begins it names the element, every report adds a point
        self.ae(ink('op=brush:i=1:k=100:n=3:fill=#ff0000c0:w0=1:w1=5'), '')
        self.ae(ink('op=query:id=4'), '\x1b]777;ink;v=1:op=cap:id=4:caps=1:mode=1:i=1:g=0:k=100:n=3:fill=#ff0000c0:w0=1:w1=5:e=1:tools=58\x1b\\')
        self.assertIsNone(report(0, 28, 28, 0))  # hovering draws nothing
        self.ae(elements(), 2)
        path = [(30, 30, 9000), (41.5, 36.25, 30000), (60, 52, 65535), (75.75, 61, 41000)]
        self.ae(report(1, *path[0]), 100)
        self.ae(elements(), 3)
        pixels = bytearray()
        g.update_vector_pixels(1, 100, 80, pixels)
        for x, y, p in path[1:]:
            self.assertIsNone(report(2, x, y, p))
        rect = g.update_vector_pixels(1, 100, 80, pixels)  # only the part under the stroke is drawn again
        self.assertTrue(0 < rect[2] * rect[3] < 100 * 80 / 2)
        self.assertTrue(pixels == g.render_vector_image(1, 100, 80))
        self.assertIsNone(report(3, 80, 64, 12345))  # the pressure of an up is reported as none
        pts = [point(x, y, p, plain, 1, 5) for x, y, p in path] + [point(80, 64, 0, plain, 1, 5)]
        first = stroke(100, pts, '#ff0000c0')
        same_as(first)
        self.ae(ink('op=query:id=5'), '\x1b]777;ink;v=1:op=cap:id=5:caps=1:mode=1:i=1:g=0:k=101:n=2:fill=#ff0000c0:w0=1:w1=5:e=1:tools=58\x1b\\')

        # the eraser does not draw, nor does a pen while ink is off, and turning it on again goes on with the ids
        self.assertIsNone(report(1, 30, 60, 30000, tool=2))
        report(3, 30, 60, tool=2)
        self.ae(ink('op=mode:mode=0'), '')
        self.assertIsNone(report(1, 30, 60, 30000))
        report(3, 30, 60)
        self.ae(elements(), 3)
        self.ae(ink('op=mode:mode=1'), '')
        self.ae(ink('op=mode'), '\x1b]777;ink;v=1:op=mode:mode=1\x1b\\')

        # a tool that measures no pressure draws at full width, and a stroke goes on outside the placement
        self.ae(report(1, 100, 90), 101)
        self.assertIsNone(report(2, 150, 130))
        self.assertIsNone(report(3, 10, 5))
        second = stroke(101, [point(100, 90, None, plain, 1, 5), point(150, 130, None, plain, 1, 5), point(10, 5, None, plain, 1, 5)], '#ff0000c0')
        same_as(first + second)

        # a stroke that is cancelled is taken away again
        self.ae(report(1, 50, 50, 30000), 102)
        report(2, 60, 55, 30000)
        self.ae(elements(), 5)
        self.assertIsNone(report(4, 60, 55))
        self.ae(elements(), 4)
        same_as(first + second)
        # and so is one cut short by a change of the tablet mode
        self.ae(ink('op=brush:i=1:k=200:n=2:w0=2:w1=2'), '')
        self.ae(report(1, 50, 50, 30000), 200)
        self.ae(elements(), 5)
        self.assertIn('phase=4', osc('tablet-pointer', 'op=mode:mode=1'))
        self.ae(elements(), 4)
        self.assertIsNone(report(2, 60, 55, 30000, delivered=False))
        # the last of the ids, after which nothing is inked
        self.ae(report(1, 40, 70, 65535), 201)
        report(3, 44, 70, 65535)
        third = stroke(201, [point(40, 70, 65535, plain, 2, 2), point(44, 70, 0, plain, 2, 2)], '#000000')
        same_as(first + second + third)
        self.assertIsNone(report(1, 50, 50, 30000))
        report(3, 50, 50)
        self.ae(elements(), 5)
        self.assertIn(':k=202:n=0:', ink('op=query:id=6'))

        # a curve for the pressure, the tools that draw, and a group that does not exist
        self.ae(ink('op=brush:i=1:k=300:n=5:fill=#00ff00:w0=0.5:w1=8.5:e=2:tools=4:g=77'), '')
        self.assertIsNone(report(1, 60, 40, 30000, tool=2))
        report(3, 60, 40, tool=2)
        self.ae(ink('op=brush:i=1:k=300:n=5:fill=#00ff00:w0=0.5:w1=8.5:e=2:tools=4'), '')
        self.assertIsNone(report(1, 60, 40, 30000))
        report(3, 60, 40)
        self.ae(report(1, 60, 40, 30000, tool=2), 300)
        report(3, 90, 40, 0, tool=2)
        fourth = stroke(300, [point(60, 40, 30000, plain, 0.5, 8.5, 2.0), point(90, 40, 0, plain, 0.5, 8.5, 2.0)], '#00ff00')
        same_as(first + second + third + fourth)

        # the stroke is in the units of the group it goes into, whatever the transforms above it
        def mul(m, n):
            return (
                m[0] * n[0] + m[2] * n[1], m[1] * n[0] + m[3] * n[1], m[0] * n[2] + m[2] * n[3], m[1] * n[2] + m[3] * n[3],
                m[0] * n[4] + m[2] * n[5] + m[4], m[1] * n[4] + m[3] * n[5] + m[5])

        def inverse(m):
            det = m[0] * m[3] - m[1] * m[2]
            return (m[3] / det, -m[1] / det, -m[2] / det, m[0] / det, (m[2] * m[5] - m[3] * m[4]) / det, (m[1] * m[4] - m[0] * m[5]) / det)

        root, outer, inner = (0.5, 0, 0, 0.5, 4, 8), (2, 0, 0, 4, 16, 8), (0, 1, -1, 0, 32, 0)
        inv = inverse(mul(root, mul(outer, inner)))

        def to_inner(x, y):
            return inv[0] * x + inv[2] * y + inv[4], inv[1] * x + inv[3] * y + inv[5]

        groups = f'g id=0 t={",".join(map(str, root))}\ng id=10 t={",".join(map(str, outer))}\ng id=11 p=10 t={",".join(map(str, inner))}\n'
        self.ae(gr(groups, a='e', i=1).code, 'OK')
        self.ae(ink('op=brush:i=1:g=11:k=400:fill=#0000ff:w0=1:w1=3'), '')
        self.ae(report(1, 44, 52, 65535), 400)
        report(2, 60.5, 71.25, 20000)
        report(3, 90, 30, 0)
        fifth = stroke(400, [point(x, y, p, plain, 1, 3, to_group=to_inner) for x, y, p in ((44, 52, 65535), (60.5, 71.25, 20000), (90, 30, 0))], '#0000ff', 11)
        same_as(first + second + third + fourth + groups + fifth)
        # a group whose transform cannot be undone takes no ink
        self.ae(gr('g id=12 t=1,2,2,4,0,0\n', a='e', i=1).code, 'OK')
        self.ae(ink('op=brush:i=1:g=12:k=500:w1=3'), '')
        self.assertIsNone(report(1, 44, 52, 65535))
        report(3, 44, 52)

        # a placement that scales and crops the image, and that scrolls during the stroke
        scene = 'p id=1 d=M0,0L100,0L100,80L0,80Z fill=#ffffff\n'
        self.ae(gr(scene, a='t', f=200, i=2, s=100, v=80).code, 'OK')
        parse_bytes(s, b'\x1b[7;11H')
        self.ae(gr(a='p', i=2, x=20, y=10, w=50, h=40, c=20, r=4, X=3, Y=5, C=1).code, 'OK')
        scaled = [10 * cw + 3, 6 * ch + 5, 20 * cw - 3, 4 * ch - 5, 20, 10, 50, 40]
        self.ae(ink('op=brush:i=2:k=7:n=9:fill=#800080:w0=1:w1=6'), '')
        self.ae(report(1, 130.5, 140, 40000), 7)
        pts = [point(130.5, 140, 40000, scaled, 1, 6)]
        report(2, 180, 170.25, 50000)
        pts.append(point(180, 170.25, 50000, scaled, 1, 6))
        parse_bytes(s, b'\x1b[12;1H\n')  # the screen moves up a line and the image with it
        scaled[1] -= ch
        report(2, 180, 170.25, 50000)
        pts.append(point(180, 170.25, 50000, scaled, 1, 6))
        report(3, 250, 150, 0)
        pts.append(point(250, 150, 0, scaled, 1, 6))
        same_as(stroke(7, pts, '#800080'), scene, 2)

        # of two placements of the image the one on top is drawn on
        self.ae(gr(a='p', i=2, z=5, C=1).code, 'OK')  # at the cursor, the start of the last line, at its own size
        on_top = (0, 11 * ch, 100, 80, 0, 0, 100, 80)
        self.ae(report(1, 99, 11 * ch + 2, 65535), 8)  # inside the first placement as well
        report(3, 99, 11 * ch + 2)
        same_as(
            stroke(7, pts, '#800080') + stroke(8, [point(99, 11 * ch + 2, 65535, on_top, 1, 6), point(99, 11 * ch + 2, 0, on_top, 1, 6)], '#800080'),
            scene, 2,
        )

        # when the image goes the stroke on it is over, and the reports go on
        self.ae(report(1, 130.5, 140, 40000), 9)
        send_command(s, 'a=d,d=I,i=2')
        self.assertIsNone(report(2, 140, 140, 40000))
        self.assertIsNone(report(3, 140, 140))
        self.assertIsNone(report(1, 130.5, 140, 40000))
        report(3, 130.5, 140)

        # More simultaneous tools than the old four-slot array, followed by reuse and reset.
        parse_bytes(s, b'\x1b[H')
        self.ae(gr(base, a='T', f=200, i=1, s=100, v=80).code, 'OK')
        self.ae(ink('op=brush:i=1:k=10000:n=1000:w1=2'), '')
        for batch in range(3):
            for device in range(128):
                self.ae(report(1, 20, 20, device=1000 + device), 10000 + batch * 128 + device)
            for device in range(128):
                report(2, 30, 30, device=1000 + device)
                report(3, 40, 40, device=1000 + device)
        self.ae(elements(), 2 + 384)
        ink('op=mode:mode=0')
        ink('op=mode:mode=1')
        self.ae(report(1, 20, 20, device=1000), 10384)
        report(3, 40, 40, device=1000)

        # without tool reports there is nothing to ink, and a reset forgets the brush
        self.ae(ink('op=brush:i=1:k=600:w1=3'), '')
        osc('tablet-pointer', 'op=mode:mode=0')
        before = elements()
        report(1, 30, 30, 30000, delivered=False)
        self.ae(elements(), before)
        osc('tablet-pointer', 'op=mode:mode=1')
        s.reset()
        self.ae(ink('op=query:id=9'), '\x1b]777;ink;v=1:op=cap:id=9:caps=1:mode=0\x1b\\')

    def test_vector_many_gradient_stops(self):
        s = self.create_screen()
        stops = [(i / 1024, ((i % 31) / 32, (i % 17) / 16, (i % 9) / 8)) for i in range(1025)]
        # Equal offsets have no specified sort order; either copy must yield the same HDR color.
        stops[512] = (.5, (2, -.25, .5))
        stops.insert(513, stops[512])
        records = ('l id=2 x1=1.5 y1=0 x2=2049.5 y2=0 stops='
                   + ','.join(f'{offset},linear:{r}/{g}/{b}' for offset, (r, g, b) in stops)
                   + '\np id=1 d=M0,0L2052,0L2052,1L0,1Z fill=@2')
        response = parse_full_response(send_command(s, 'a=t,f=200,i=1,s=2052,v=1', records))
        self.ae(response.code, 'OK', response.msg)
        pixels = list(struct.iter_unpack('ffff', s.grman.render_vector_linear(1, 2052, 1)))
        for x, pixel in enumerate(pixels):
            t = (x - 1) / 2048
            if t <= stops[0][0]:
                expected = stops[0][1]
            elif t >= stops[-1][0]:
                expected = stops[-1][1]
            else:
                right = next(i for i in range(1, len(stops)) if stops[i][0] >= t)
                offset, color = stops[right - 1]
                end, other = stops[right]
                f = (t - offset) / (end - offset) if end > offset else 0
                expected = tuple(a + (b - a) * f for a, b in zip(color, other))
            for actual, value in zip(pixel[:3], expected):
                self.assertAlmostEqual(actual, value, places=6, msg=f'gradient at pixel {x}')
            self.ae(pixel[3], 1)

    def test_vector_raster_byte_colors(self):
        s = self.create_screen()
        rgb = bytes(value for i in range(256) for value in (i, (i + 85) % 256, (i + 171) % 256))
        rgba = bytes(value for i in range(256) for value in (i, (i + 85) % 256, (i + 171) % 256, i))
        for fmt, data in ((24, rgb), (32, rgba)):
            response = parse_full_response(send_command(s, f'a=t,f={fmt},i={fmt},s=256,v=1', data))
            self.ae(response.code, 'OK', response.msg)
        response = parse_full_response(send_command(s, 'a=t,f=200,i=1,s=256,v=2',
                                                   'i id=1 img=24\ni id=2 img=32 t=1,0,0,1,0,1'))
        self.ae(response.code, 'OK', response.msg)
        for _ in range(8):
            pixels = list(struct.iter_unpack('ffff', s.grman.render_vector_linear(1, 256, 2)))
            for row in range(2):
                for x in range(256):
                    pixel = pixels[row * 256 + x]
                    alpha = 1 if not row else x / 255
                    self.assertAlmostEqual(pixel[3], alpha, places=6)
                    for actual, byte in zip(pixel[:3], rgb[3*x:3*x+3]):
                        value = byte / 255
                        value = value / 12.92 if value <= .04045 else ((value + .055) / 1.055) ** 2.4
                        self.assertAlmostEqual(actual, value if alpha else 0, places=6)

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

    def test_vector_dash_limit_option(self):
        # vector_dash_limit, when set, strokes solid the subpaths whose dashes would take the path past it
        s = self.create_screen(options={'vector_dash_limit': 16})
        g = s.grman

        def coverage(payload, image_id, y):
            self.ae(parse_full_response(send_command(s, f'a=T,f=200,s=20,v=20,i={image_id}', payload)).code, 'OK')
            px = g.render_vector_image(image_id, 20, 20)
            return [px[(y * 20 + x) * 4 + 3] for x in range(20)]

        self.ae(coverage('p id=1 d=M0,10L20,10 stroke=#000000 w=2 dash=5,5 cap=butt fill=none', 1, 10), ([255] * 5 + [0] * 5) * 2)
        self.ae(coverage('p id=1 d=M0,10L20,10 stroke=#000000 w=2 dash=1,1 cap=butt fill=none', 2, 10), [255] * 20)
        # the count is for the whole path: the first subpath fits, the second would take it past the limit
        two = 'M0,5L20,5M0,15L20,15'
        self.ae(coverage(f'p id=1 d={two} stroke=#000000 w=2 dash=2,2 cap=butt fill=none', 3, 5), [255, 255, 0, 0] * 5)
        self.ae(coverage(f'p id=1 d={two} stroke=#000000 w=2 dash=2,2 cap=butt fill=none', 4, 15), [255] * 20)

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

    def test_vector_render_view(self):
        # The raster of a vector image is that of the part of it that is shown, so its size has nothing to do with
        # how large the image is shown, and an allocation too large fails rather than wrapping around.
        s = self.create_screen()
        g = s.grman
        doc = 'p id=1 d=M2,2L18,3L10,18Z fill=#ff8000c0 stroke=#0000ff w=1.5\nk id=2 pts=3,15,2,15,4,3 fill=#00ff0080\n'
        self.ae(parse_full_response(send_command(s, 'a=T,f=200,s=20,v=20,i=1', doc)).code, 'OK')
        W, H = 200, 160
        full = g.render_vector_image(1, W, H)
        # a part comes out as that part of all of it, to within the rounding of where it starts
        x, y, cols, rows = 53, 31, 70, 45
        part = g.render_vector_view(1, W, H, x, y, cols, rows)
        crop = b''.join(full[((y + r) * W + x) * 4:((y + r) * W + x + cols) * 4] for r in range(rows))
        self.ae(len(part), len(crop))
        self.assertLessEqual(max(abs(a - b) for a, b in zip(part, crop)), 1)
        # an image shown at a thousand million pixels across takes no longer to draw a part of than a small one
        start = time.monotonic()
        part = g.render_vector_view(1, 1e9, 8e8, 5e8, 1.5e8, 64, 64)
        self.assertLess(time.monotonic() - start, 5)
        self.ae(len(part), 64 * 64 * 4)
        self.ae(set(part[3::4]), {192})  # inside the triangle, far from its edges
        # sizes that cannot be allocated, or held in the types they are counted in, fail
        for args in ((W, H, 0, 0, 2**31 - 1, 2**31 - 1), (W, H, 0, 0, 2**31, 1), (W, H, 0.5, 0, 10, 10), (float('inf'), H, 0, 0, 10, 10)):
            with self.assertRaises(RuntimeError, msg=str(args)):
                g.render_vector_view(1, *args)
        with self.assertRaises(RuntimeError):
            g.render_vector_image(1, 2**31 - 1, 2**31 - 1)

        # Which part is rasterized: all of an image no bigger than the window, the part on screen of one that is,
        # and the part already drawn while it still holds what is on screen
        view = g.choose_vector_view
        self.ae(view(300, 200, (0, 0, 1, 1), 800, 600), (300, 200, 0, 0, 300, 200))
        self.ae(view(300.4, 200.6, (0.2, 0.2, 0.3, 0.3), 800, 600), (300, 201, 0, 0, 300, 201))
        W, H, x, y, cols, rows = v = view(1e9, 1e9, (0.5, 0.5, 0.5000008, 0.5000006), 800, 600)
        self.ae((W, H), (1e9, 1e9))
        self.assertLessEqual(x, 5e8 - 2)
        self.assertGreaterEqual(x + cols, 5e8 + 800 + 2)
        self.assertLessEqual(y, 5e8 - 2)
        self.assertGreaterEqual(y + rows, 5e8 + 600 + 2)
        self.assertLessEqual(cols * rows, 3 * 800 * 600)
        # to the pixel: the quarter window of margin, the 2 pixels of filtering and nothing more
        self.ae((x, y, x + cols, y + rows), (5e8 - 202, 5e8 - 152, 5e8 + 1002, 5e8 + 752))
        self.ae(view(1e9, 1e9, (0.50000001, 0.5, 0.50000081, 0.5000006), 800, 600, v), v)
        self.assertNotEqual(view(1e9, 1e9, (0.6, 0.5, 0.6000008, 0.5000006), 800, 600, v), v)
        # placements that show far apart parts of one image at the largest magnification are drawn smaller
        W, H, x, y, cols, rows = view(1e6, 1e6, (0, 0, 1, 1), 800, 600)
        self.assertLess(W, 1e6)
        self.assertLessEqual(cols * rows, 4 * 800 * 600 * 1.6)
        # an image shown larger than doubles count pixels exactly is shown at that size
        W, H, x, y, cols, rows = view(1e300, 1e300, (0.5, 0.5, 0.5, 0.5), 800, 600)
        self.assertLessEqual(W, 2**53)
        self.assertLessEqual(cols * rows, 3 * 800 * 600)

    def test_vector_amplification(self):
        # Work and memory for a vector command are in proportion to its size and to what it changes, not to that
        # times the size of the document, and not many times that in memory.
        import signal
        s = self.create_screen()
        g = s.grman

        def command(records, action='t', image=1):
            # an escape code may be of any length, so this is a single chunk
            records = records.encode()
            step = len(records)
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

    def test_vector_image_references(self):
        # Every raster image that is added, changed or deleted is looked up in every vector image, so that the ones
        # drawing it are drawn again. That must not mean going through all of the elements of each.
        s = self.create_screen()
        g = s.grman
        n = 300000
        records = ''.join(f'p id={i} d=M1,1L2,2Z\n' for i in range(1, n + 1)) + 'i id=999999 img=7 t=4,0,0,4,0,0\n'
        self.ae(parse_full_response(send_command(s, 'a=t,f=200,s=10,v=10,i=1', records)).code, 'OK')
        pixels = bytearray()
        g.update_vector_pixels(1, 10, 10, pixels)
        self.assertIsNone(g.update_vector_pixels(1, 10, 10, pixels))
        start = time.monotonic()
        for i in range(2000, 7000):
            send_command(s, f'a=t,f=24,i={i},s=1,v=1', b'\x00\xff\x00')
        for i in range(2000, 7000):
            send_command(s, f'a=d,d=I,i={i}')
        self.assertLess(time.monotonic() - start, 10)
        # images it does not draw change nothing, the one it draws all of it
        self.assertIsNone(g.update_vector_pixels(1, 10, 10, pixels))
        self.ae(parse_full_response(send_command(s, 'a=t,f=24,i=7,s=1,v=1', b'\xff\x00\x00')).code, 'OK')
        self.ae(g.update_vector_pixels(1, 10, 10, pixels), (0, 0, 10, 10))
        self.ae(pixels[(1 * 10 + 1) * 4:(1 * 10 + 1) * 4 + 4], b'\xff\x00\x00\xff')
        send_command(s, 'a=d,d=I,i=7')
        self.ae(g.update_vector_pixels(1, 10, 10, pixels), (0, 0, 10, 10))
        self.ae(pixels[(1 * 10 + 1) * 4 + 3], 0)
        # replacing or deleting the i record stops it being asked about
        self.ae(parse_full_response(send_command(s, 'a=e,i=1', 'x id=999999')).code, 'OK')
        g.update_vector_pixels(1, 10, 10, pixels)
        send_command(s, 'a=t,f=24,i=7,s=1,v=1', b'\xff\x00\x00')
        self.assertIsNone(g.update_vector_pixels(1, 10, 10, pixels))

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

            # non-regular files must be rejected rather than streamed into the converter
            num = irc.num_of_renders
            fifo = os.path.join(cache_path, 'fifo')
            os.mkfifo(fifo)
            for bad in (os.devnull, fifo, cache_path):
                with self.assertRaises(ValueError, msg=bad):
                    irc.render(bad)
                with self.assertRaises(ValueError, msg=bad):
                    irc.render_image(bad, os.path.join(cache_path, 'out'))
                self.assertFalse(os.path.exists(os.path.join(cache_path, 'out')))
            self.ae(irc.num_of_renders, num)
