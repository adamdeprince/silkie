#!/usr/bin/env python
# License: GPLv3 Copyright: 2026, Adam DePrince

import os
import struct
import tempfile
import zlib

from kitty.fast_data_types import shm_unlink, shm_write

from .base import BaseTest, parse_bytes
from .graphics import parse_full_response, send_command

# The packed formats are 300 plus their bits per pixel, see docs/silkie/packed-image-protocol.rst
PACKED_FORMAT_BASE = 300


class TestPackedGraphics(BaseTest):

    def transmit(self, screen, data, **options):
        options.setdefault('a', 't')
        options.setdefault('i', 1)
        if isinstance(data, str):
            data = data.encode()
        command = ','.join(f'{key}={value}' for key, value in options.items())
        if options.get('t', 'd') != 'd' or not data:
            return parse_full_response(send_command(screen, command, data))
        for offset in range(0, len(data), 3072):
            more = int(offset + 3072 < len(data))
            header = command + ',' if not offset else (f'a=f,i={options["i"]},' if options['a'] == 'f' else '')
            response = send_command(screen, header + f'm={more}', data[offset:offset+3072])
            if more:
                self.assertFalse(response)
        return parse_full_response(response)

    def image(self, screen, data, expected, **options):
        response = self.transmit(screen, data, **options)
        self.ae(response.code, 'OK', response.msg)
        image = screen.grman.image_for_client_id(options.get('i', 1))
        self.ae(image['data'], expected)
        self.ae((image['width'], image['height']), (options['s'], options['v']))
        self.ae(image['is_4byte_aligned'], options['s'] % 4 == 0)
        return image

    def test_grayscale_rows_levels_and_padding(self):
        screen = self.create_screen()
        for bits, encoded, values, width in (
            (1, b'\x80\xff\x7f\x00', (255, 0, 0, 0, 0, 0, 0, 0, 255, 0, 255, 255, 255, 255, 255, 255, 255, 0), 9),
            (2, b'\x1b\xff\xe4\x00', (0, 85, 170, 255, 255, 255, 170, 85, 0, 0), 5),
            (4, b'\x01\x2f\xfe\xdf', (0, 17, 34, 255, 238, 221), 3),
            (8, bytes(range(256)), tuple(range(256)), 128),
        ):
            with self.subTest(bits=bits):
                expected = bytes(channel for value in values for channel in (value, value, value))
                self.image(screen, encoded, expected, f=PACKED_FORMAT_BASE + bits, s=width, v=2)

    def test_kitty_format_numbers_are_not_packed_formats(self):
        # f=1 to f=16 were the packed formats before they moved to 300 plus
        # their bits per pixel. f=8 and f=16 are kitty's indexed color formats.
        screen = self.create_screen()
        for f in (1, 2, 4):
            with self.subTest(f=f):
                response = self.transmit(screen, b'\x00\x00', f=f, s=1, v=1)
                self.ae(response.code, 'EINVAL', response.msg)
        # Indexed color images keep their palette indices, they are not
        # expanded to RGB as packed images are, and they have no gamut, their
        # colors being those of the terminal
        for f, data in ((8, b'\x07\x09'), (16, b'\x07\x80\x09\xff')):
            with self.subTest(f=f):
                response = self.transmit(screen, data, f=f, s=2, v=1, G=1)
                self.ae(response.code, 'OK', response.msg)
                image = screen.grman.image_for_client_id(1)
                self.ae((image['indexed_px_sz'], image['data'], image['gamut']), (f // 8, data, 0))

    def test_all_rgb565_words_and_gamut(self):
        screen = self.create_screen()
        encoded = bytes(byte for word in range(65536) for byte in (word & 255, word >> 8))
        expected = bytes(channel for word in range(65536) for channel in (
            (((word >> 11) & 31) * 255 + 15) // 31,
            (((word >> 5) & 63) * 255 + 31) // 63,
            ((word & 31) * 255 + 15) // 31,
        ))
        image = self.image(screen, encoded, expected, f=316, s=256, v=256, G=1)
        self.ae(image['gamut'], 1)
        image = self.image(screen, b'\x7f\xff', b'\x7f' * 3 + b'\xff' * 3, f=308, s=2, v=1, G=2)
        self.ae(image['gamut'], 0)

    def test_formats_are_independent_of_display(self):
        for display in ('color=rgb bits=10', 'color=gray bits=4', 'color=mono'):
            screen = self.create_screen(options={'display_info': display})
            screen.callbacks.clear()
            parse_bytes(screen, b'\x1b]777;caps;v=1:op=query:id=7\x1b\\')
            self.assertIn(b':format=8,16,24,32,64,100,200,301,302,304,308,316,401,402,404,408\x1b\\', screen.callbacks.wtcbuf)
            for fmt, encoded, expected in (
                (301, b'\x40', b'\x00' * 3 + b'\xff' * 3),
                (308, b'\x00\xff', b'\x00' * 3 + b'\xff' * 3),
                (316, b'\x00\xf8\x1f\x00', b'\xff\0\0\0\0\xff'),
                (24, b'\xff\0\0\0\0\xff', b'\xff\0\0\0\0\xff'),
                (32, b'\xff\0\0\xff\0\0\xff\x80', b'\xff\0\0\xff\0\0\xff\x80'),
                (64, struct.pack('eeeeeeee', 2, .25, -.1, .5, 0, 0, 1, 1),
                 struct.pack('eeeeeeee', 2, .25, -.1, .5, 0, 0, 1, 1)),
            ):
                response = self.transmit(screen, encoded, f=fmt, s=2, v=1)
                self.ae(response.code, 'OK', (display, fmt, response.msg))
                self.ae(screen.grman.image_for_client_id(1)['data'], expected)

    def test_packed_queries_sizes_and_recovery(self):
        screen = self.create_screen()
        for fmt, size in ((301, 4), (302, 6), (304, 10), (308, 18), (316, 36)):
            response = self.transmit(screen, bytes(size), a='q', f=fmt, s=9, v=2)
            self.ae(response.code, 'OK', response.msg)
            self.ae(screen.grman.image_count, 0)
            response = self.transmit(screen, bytes(size - 1), f=fmt, s=9, v=2)
            self.ae(response.code, 'ENODATA')
            response = self.transmit(screen, b'\xff', f=fmt, s=0, v=1)
            self.ae(response.code, 'EINVAL')
            self.image(screen, b'\x80', b'\xff' * 3, f=301, s=1, v=1)
            self.transmit(screen, b'', a='d', d='I')
        self.ae(self.transmit(screen, b'\xff', f=3, s=1, v=1).code, 'EINVAL')

    def test_packed_transports_and_animation(self):
        screen = self.create_screen()
        encoded = b'\xaa\xff\x55\x00'
        values = (255, 0, 255, 0, 255, 0, 255, 0, 255, 0, 255, 0, 255, 0, 255, 0, 255, 0)
        expected = bytes(channel for value in values for channel in (value, value, value))
        self.image(screen, zlib.compress(encoded), expected, f=301, s=9, v=2, o='z')
        for transport in ('f', 't'):
            with tempfile.NamedTemporaryFile(prefix='tty-graphics-protocol-packed-', delete=False) as stream:
                stream.write(b'prefix' + encoded + b'suffix')
            try:
                self.image(screen, stream.name, expected, f=301, s=9, v=2, t=transport, O=6, S=len(encoded))
                self.ae(os.path.exists(stream.name), transport == 'f')
            finally:
                if os.path.exists(stream.name):
                    os.unlink(stream.name)
        name = f'/silkie-packed-{os.getpid()}'
        shm_write(name, b'pad' + encoded)
        try:
            self.image(screen, name, expected, f=301, s=9, v=2, t='s', O=3, S=len(encoded))
            self.assertRaises(FileNotFoundError, shm_unlink, name)
        finally:
            try:
                shm_unlink(name)
            except FileNotFoundError:
                pass
        response = self.transmit(screen, encoded, a='f', f=301, s=9, v=2)
        self.ae(response.code, 'OK', response.msg)
        self.ae(screen.grman.image_for_client_id(1)['extra_frames'][0]['data'], expected)

    def test_sustained_updates_and_large_incompressible_payload(self):
        screen = self.create_screen()
        for iteration in range(96):
            value = iteration % 16
            self.image(screen, bytes((value * 17,)) * 57, bytes((value * 17,)) * (37 * 3 * 3), f=304, s=37, v=3)
            self.ae(screen.grman.image_count, 1)
        # A zlib stream can exceed its raw size by more than an arbitrary
        # header allowance; transport chunks must keep growing until complete.
        raw = os.urandom(2048 * 2048)
        compressed = zlib.compress(raw, 1)
        self.assertGreater(len(compressed), len(raw) + 1024)
        expected = bytes(channel for value in raw for channel in (value, value, value))
        self.image(screen, compressed, expected, f=308, s=2048, v=2048, o='z')

    def test_packed_frame_chunks_and_vector_references(self):
        screen = self.create_screen()
        root = bytes(438 * 2)
        self.image(screen, root, bytes(3500 * 2 * 3), f=301, s=3500, v=2)
        frame = bytes(i % 256 for i in range(3500 * 2))
        response = self.transmit(screen, frame, a='f', f=308, s=3500, v=2)
        self.ae(response.code, 'OK', response.msg)
        expected = bytes(channel for value in frame for channel in (value, value, value))
        self.ae(screen.grman.image_for_client_id(1)['extra_frames'][0]['data'], expected)

        self.image(screen, b'\x80', b'\xff' * 3 + b'\x00' * 3, f=301, s=2, v=1, i=10)
        self.image(screen, b'\xff' * 3 + b'\x00' * 3, b'\xff' * 3 + b'\x00' * 3, f=24, s=2, v=1, i=11)
        for image, raster in ((20, 10), (21, 11)):
            response = self.transmit(screen, f'i id=1 img={raster} t=4,0,0,4,0,0', f=200, s=8, v=4, i=image)
            self.ae(response.code, 'OK', response.msg)
        self.ae(screen.grman.render_vector_linear(20, 8, 4), screen.grman.render_vector_linear(21, 8, 4))
