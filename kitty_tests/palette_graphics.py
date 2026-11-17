#!/usr/bin/env python
# License: GPLv3 Copyright: 2026, Adam DePrince

import os
import struct
import tempfile
import zlib

from . import packed_graphics
from .base import BaseTest
from .graphics import send_command

# The palette formats are 400 plus the bits of each index, see docs/silkie/palette-image-protocol.rst
PALETTE_FORMAT_BASE = 400


def palette_of(bits, alpha=255):
    # every slot distinct: red counts up, green down, blue is the slot again
    return b''.join(bytes((i * 255 // ((1 << bits) - 1), 255 - i * 255 // ((1 << bits) - 1), i, alpha)) for i in range(1 << bits))


class TestPaletteGraphics(BaseTest):

    transmit = packed_graphics.TestPackedGraphics.transmit

    def image(self, screen, palette, encoded, indices, **options):
        response = self.transmit(screen, palette + encoded, **options)
        self.ae(response.code, 'OK', response.msg)
        image = screen.grman.image_for_client_id(options.get('i', 1))
        self.ae((image['data'], image['palette'], image['indexed_px_sz']), (indices, palette, 1))
        self.ae((image['width'], image['height']), (options['s'], options['v']))
        self.ae(image['is_4byte_aligned'], options['s'] % 4 == 0)
        return image

    def rgba(self, palette, indices):
        return b''.join(palette[4 * i:4 * i + 4] for i in indices)

    def test_every_bit_depth_with_row_padding(self):
        screen = self.create_screen()
        for bits, encoded, indices, width in (
            # rows start at a byte boundary, the unused low bits of a row's last byte are ignored
            (1, b'\x80\xff\x7f\x00', (1, 0, 0, 0, 0, 0, 0, 0, 1, 0, 1, 1, 1, 1, 1, 1, 1, 0), 9),
            (2, b'\x1b\xff\xe4\x00', (0, 1, 2, 3, 3, 3, 2, 1, 0, 0), 5),
            (4, b'\x01\x2f\xfe\xdf', (0, 1, 2, 15, 14, 13), 3),
            (8, bytes((0, 7, 200, 255, 128, 1)), (0, 7, 200, 255, 128, 1), 3),
        ):
            with self.subTest(bits=bits):
                palette = palette_of(bits)
                self.image(screen, palette, encoded, bytes(indices), f=PALETTE_FORMAT_BASE + bits, s=width, v=2)
                self.ae(screen.grman.rgba_for_client_id(1), self.rgba(palette, indices))
        # a width that is a multiple of four gives aligned rows of indices
        self.image(screen, palette_of(1), b'\xa0', b'\x01\x00\x01\x00', f=401, s=4, v=1)

    def test_every_slot_has_its_own_alpha(self):
        # unlike GIF, transparency is not one index: every slot has eight bits of straight alpha
        screen = self.create_screen()
        palette = bytes((255, 0, 0, 0, 0, 255, 0, 64, 0, 0, 255, 128, 255, 255, 255, 255))
        indices = (0, 1, 2, 3, 3, 2, 1, 0)
        self.image(screen, palette, b'\x1b\xe4', bytes(indices), f=402, s=4, v=2)
        self.ae(screen.grman.rgba_for_client_id(1), self.rgba(palette, indices))

    def test_sizes_are_checked(self):
        screen = self.create_screen()
        for bits, size in ((1, 8 + 4), (2, 16 + 6), (4, 64 + 10), (8, 1024 + 18)):
            fmt = PALETTE_FORMAT_BASE + bits
            with self.subTest(bits=bits):
                response = self.transmit(screen, bytes(size), a='q', f=fmt, s=9, v=2)
                self.ae(response.code, 'OK', response.msg)
                self.ae(screen.grman.image_count, 0)
                # the palette is always complete, so missing bytes are an error, even ones of the indices
                self.ae(self.transmit(screen, bytes(size - 1), f=fmt, s=9, v=2).code, 'ENODATA')
                self.ae(self.transmit(screen, bytes(size + 64), f=fmt, s=9, v=2).code, 'EFBIG')
                self.ae(self.transmit(screen, zlib.compress(bytes(size + 1)), f=fmt, s=9, v=2, o='z').code, 'EINVAL')
                self.ae(self.transmit(screen, bytes(size), f=fmt, s=0, v=2).code, 'EINVAL')
                self.ae(self.transmit(screen, bytes(size), f=fmt, s=9).code, 'EINVAL')
        # formats between the palette formats are not formats
        for fmt in (400, 403, 405, 409, 416):
            self.ae(self.transmit(screen, bytes(64), f=fmt, s=1, v=1).code, 'EINVAL')
        # there is no largest image: the data is far too short, not an overflow
        self.ae(self.transmit(screen, bytes(8), f=401, s=10000, v=10000).code, 'ENODATA')
        self.ae(self.transmit(screen, bytes(8), f=401, s=10001, v=1).code, 'ENODATA')
        # nor at the largest dimensions there are, whose indices just fit in 64 bits
        self.ae(self.transmit(screen, bytes(8), f=408, s=4294967295, v=4294967295).code, 'ENODATA')

    def test_transports_compression_and_chunks(self):
        screen = self.create_screen()
        palette = palette_of(4, 200)
        encoded = bytes(range(256)) * 8  # 64x64 four bit indices
        indices = bytes(i for b in encoded for i in (b >> 4, b & 15))
        self.image(screen, palette, encoded * 2, indices * 2, f=404, s=64, v=128)  # chunked, see transmit()
        response = self.transmit(screen, zlib.compress(palette + encoded), f=404, s=64, v=64, o='z')
        self.ae(response.code, 'OK', response.msg)
        self.ae(screen.grman.image_for_client_id(1)['data'], indices)
        for transport in ('f', 't'):
            with tempfile.NamedTemporaryFile(prefix='tty-graphics-protocol-palette-', delete=False) as stream:
                stream.write(b'prefix' + palette + encoded + b'suffix')
            try:
                response = self.transmit(screen, stream.name, f=404, s=64, v=64, t=transport, O=6, S=len(palette + encoded))
                self.ae(response.code, 'OK', response.msg)
                self.ae(screen.grman.image_for_client_id(1)['palette'], palette)
                self.ae(os.path.exists(stream.name), transport == 'f')
            finally:
                if os.path.exists(stream.name):
                    os.unlink(stream.name)

    def test_palette_update(self):
        screen = self.create_screen()
        palette = palette_of(2)
        indices = bytes((0, 1, 2, 3))
        self.image(screen, palette, b'\x1b', indices, f=402, s=4, v=1)
        version = screen.grman.version_for_client_id(1)

        def update(data, **options):
            return self.transmit(screen, data, a='e', **options)

        # all of it
        new = bytes(range(16))
        self.ae(update(new).code, 'OK')
        image = screen.grman.image_for_client_id(1)
        self.ae((image['palette'], image['data']), (new, indices))
        self.ae(screen.grman.rgba_for_client_id(1), new)
        self.assertNotEqual(screen.grman.version_for_client_id(1), version)
        # a range of slots, the rest are kept
        self.ae(update(b'\xaa' * 8, x=1, w=2).code, 'OK')
        self.ae(screen.grman.image_for_client_id(1)['palette'], new[:4] + b'\xaa' * 8 + new[12:])
        # from a slot to the end
        self.ae(update(b'\xbb' * 4, x=3).code, 'OK')
        self.ae(screen.grman.image_for_client_id(1)['palette'], new[:4] + b'\xaa' * 8 + b'\xbb' * 4)
        # compressed and from a file, as for any other data
        self.ae(update(zlib.compress(b'\xcc' * 8), w=2, o='z').code, 'OK')
        with tempfile.NamedTemporaryFile(prefix='tty-graphics-protocol-palette-') as stream:
            stream.write(b'\xdd' * 4)
            stream.flush()
            self.ae(update(stream.name, x=2, w=1, t='f').code, 'OK')
        self.ae(screen.grman.image_for_client_id(1)['palette'], b'\xcc' * 8 + b'\xdd' * 4 + b'\xbb' * 4)
        # slots that do not exist and data of the wrong size are refused and change nothing
        before = screen.grman.image_for_client_id(1)['palette']
        for data, options, code in (
            (b'\0' * 4, {'x': 4}, 'EINVAL'),
            (b'\0' * 8, {'x': 3, 'w': 2}, 'EINVAL'),
            (b'\0' * 20, {'w': 5}, 'EINVAL'),
            (b'\0' * 15, {}, 'ENODATA'),
            (b'\0' * 7, {'x': 1, 'w': 2}, 'ENODATA'),
            (b'\0' * 9, {'x': 1, 'w': 2}, 'EFBIG'),
            (zlib.compress(b'\0' * 12), {'x': 1, 'w': 2, 'o': 'z'}, 'EINVAL'),
            (b'\0' * 16, {'G': 1}, 'EINVAL'),
        ):
            with self.subTest(options=options, size=len(data)):
                self.ae(update(data, **options).code, code)
                self.ae(screen.grman.image_for_client_id(1)['palette'], before)
        self.ae(update(b'\0' * 4, i=99).code, 'ENOENT')
        # an image that is neither a palette nor a vector image cannot be edited
        self.ae(self.transmit(screen, b'\0\0\0\xff', i=2, s=1, v=1).code, 'OK')
        self.ae(update(b'\0' * 4, i=2).code, 'EINVAL')

    def test_chunked_palette_update(self):
        screen = self.create_screen()
        self.image(screen, palette_of(8), b'\x05', b'\x05', f=408, s=1, v=1)
        new = bytes(range(256)) * 4
        self.ae(self.transmit(screen, new, a='e').code, 'OK')  # chunked by transmit()
        self.ae(screen.grman.image_for_client_id(1)['palette'], new)
        self.ae(screen.grman.rgba_for_client_id(1), new[20:24])

    def test_gamut(self):
        screen = self.create_screen()
        palette = palette_of(1)
        image = self.image(screen, palette, b'\x80', b'\x01', f=401, s=1, v=1, G=1)
        self.ae(image['gamut'], 1)
        # an update is in the gamut of the image, saying so is allowed, another gamut is not
        self.ae(self.transmit(screen, b'\0' * 8, a='e', G=1).code, 'OK')
        self.ae(self.transmit(screen, b'\0' * 8, a='e', G=2).code, 'EINVAL')
        self.ae(self.transmit(screen, b'\0' * 8, a='e').code, 'OK')
        self.ae(screen.grman.image_for_client_id(1)['gamut'], 1)

    def test_frames_composition_and_replacement(self):
        screen = self.create_screen()
        self.image(screen, palette_of(1), b'\x80', b'\x01', f=401, s=1, v=1)
        response = self.transmit(screen, b'\0\0\0\xff', a='f', s=1, v=1)
        self.ae((response.code, response.msg), ('EINVAL', 'Animation is not supported for palette images'))
        self.ae(self.transmit(screen, b'', a='c', r=1, c=1).code, 'EINVAL')
        # a palette format cannot be a frame of another image either
        self.ae(self.transmit(screen, b'\0\0\0\xff', i=2, s=1, v=1).code, 'OK')
        self.ae(self.transmit(screen, palette_of(1) + b'\x80', a='f', i=2, f=401, s=1, v=1).code, 'EINVAL')
        # sending the image again in another format drops the palette
        self.ae(self.transmit(screen, b'\1\2\3\4', s=1, v=1).code, 'OK')
        image = screen.grman.image_for_client_id(1)
        self.ae((image['palette'], image['indexed_px_sz'], image['data']), (None, 0, b'\1\2\3\4'))

    def test_feature_query(self):
        screen = self.create_screen()

        def q(e, **kw):
            res = send_command(screen, ','.join(f'{k}={v}' for k, v in dict(a='q', i=31, e=e, **kw).items()))
            return res.decode('ascii')

        def r(keys=''):
            return f'\033_Gi=31{keys};OK\033\\'

        for fmt in (401, 402, 404, 408):
            self.ae(q('t', f=fmt, s=1, v=1), r(f',e=t,f={fmt},s=1,v=1'))
            # a palette can be in a gamut, but it is never HDR, and there are no frames
            self.ae(q('T', f=fmt, G=1, T=1), r(f',e=T,f={fmt},G=1'))
            self.ae(q('f', f=fmt, s=1), r(',e=f,s=1'))
        self.ae(q('t', f=403), r(',e=t'))
        self.ae(q('e', x=2, w=3, o='z', t='f'), r(',e=e,t=f,o=z,x=2,w=3'))

    def test_vector_images_draw_palette_images(self):
        screen = self.create_screen()
        palette = bytes((255, 0, 0, 255, 0, 0, 255, 255))
        self.image(screen, palette, b'\x00', b'\x00', f=401, s=1, v=1, i=9)
        doc = 'i id=6 img=9 t=10,0,0,10,70,0\n'
        response = self.transmit(screen, doc, a='T', f=200, s=120, v=80)
        self.ae(response.code, 'OK')
        red = struct.unpack('ffff', screen.grman.render_vector_linear(1, 120, 80)[(5*120+75)*16:(5*120+76)*16])
        self.assertAlmostEqual(red[0], 1, places=3)
        self.assertAlmostEqual(red[2], 0, places=3)
        # a palette update is drawn by the vector images that draw the image
        pixels = bytearray()
        screen.grman.update_vector_pixels(1, 120, 80, pixels)
        self.assertIsNone(screen.grman.update_vector_pixels(1, 120, 80, pixels))
        self.ae(self.transmit(screen, palette[4:] + palette[:4], a='e', i=9).code, 'OK')
        self.assertIsNotNone(screen.grman.update_vector_pixels(1, 120, 80, pixels))
        blue = struct.unpack('ffff', screen.grman.render_vector_linear(1, 120, 80)[(5*120+75)*16:(5*120+76)*16])
        self.assertAlmostEqual(blue[0], 0, places=3)
        self.assertAlmostEqual(blue[2], 1, places=3)
