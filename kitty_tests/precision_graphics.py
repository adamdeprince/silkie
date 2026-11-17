#!/usr/bin/env python
# License: GPLv3 Copyright: 2026, Adam DePrince

import math
import struct
import zlib

from .base import BaseTest, parse_bytes
from .graphics import parse_full_response, send_command

# The high precision formats, see docs/silkie/precision-image-protocol.rst
GRAY16, RGB10A2, RGBA16 = 316, 632, 664
# The E key carries ITU-T H.273 transfer characteristics
SRGB, LINEAR, PQ, HLG = 13, 8, 16, 18
SDR_WHITE_NITS, HLG_PEAK_NITS = 203, 1000


def srgb_eotf(v):
    return v / 12.92 if v <= 0.04045 else ((v + 0.055) / 1.055) ** 2.4


def pq_eotf(v):
    m1, m2, c1, c2, c3 = 2610 / 16384, 2523 / 4096 * 128, 3424 / 4096, 2413 / 4096 * 32, 2392 / 4096 * 32
    p = v ** (1 / m2)
    return 10000 * (max(p - c1, 0) / (c2 - c3 * p)) ** (1 / m1) / SDR_WHITE_NITS


def pq_oetf(nits):
    m1, m2, c1, c2, c3 = 2610 / 16384, 2523 / 4096 * 128, 3424 / 4096, 2413 / 4096 * 32, 2392 / 4096 * 32
    y = (nits / 10000) ** m1
    return ((c1 + c2 * y) / (1 + c3 * y)) ** m2


def hlg_gray(v):
    # the inverse OETF and the OOTF of a neutral color, whose luminance is its value
    a, b, c = 0.17883277, 0.28466892, 0.55991073
    e = v * v / 3 if v <= 0.5 else (math.exp((v - c) / a) + b) / 12
    return HLG_PEAK_NITS / SDR_WHITE_NITS * e ** 1.2


def rgb10a2(r, g, b, a):
    return struct.pack('<I', r | (g << 10) | (b << 20) | (a << 30))


def floats(data):
    return struct.unpack(f'{len(data) // 4}f', data)


class TestPrecisionGraphics(BaseTest):

    def transmit(self, screen, data, **options):
        options.setdefault('a', 't')
        options.setdefault('i', 1)
        command = ','.join(f'{key}={value}' for key, value in options.items())
        if len(data) <= 3072:
            return parse_full_response(send_command(screen, command, data))
        for offset in range(0, len(data), 3072):
            more = int(offset + 3072 < len(data))
            response = send_command(screen, (command + ',' if not offset else '') + f'm={more}', data[offset:offset + 3072])
            if more:
                self.assertFalse(response)
        return parse_full_response(response)

    def image(self, screen, data, **options):
        response = self.transmit(screen, data, **options)
        self.ae(response.code, 'OK', response.msg)
        image = screen.grman.image_for_client_id(options.get('i', 1))
        # the stored pixels are the bytes that were sent
        self.ae(image['data'], data)
        self.ae((image['width'], image['height']), (options['s'], options['v']))
        return image

    def decoded(self, screen, i=1):
        return floats(screen.grman.decoded_image_pixels(i))

    def assert_close(self, actual, expected, places=5, rel=0):
        self.ae(len(actual), len(expected))
        for n, (a, e) in enumerate(zip(actual, expected)):
            if rel:
                self.assertLessEqual(abs(a - e), rel * max(abs(e), 1), msg=f'component {n}: {actual} != {expected}')
            else:
                self.assertAlmostEqual(a, e, places=places, msg=f'component {n}: {actual} != {expected}')

    def test_sizes_and_alignment(self):
        screen = self.create_screen()
        for fmt, px in ((GRAY16, 2), (RGB10A2, 4), (RGBA16, 8)):
            with self.subTest(fmt=fmt):
                screen.reset()
                size = 9 * 2 * px
                response = self.transmit(screen, bytes(size), a='q', f=fmt, s=9, v=2)
                self.ae(response.code, 'OK', response.msg)
                self.ae(screen.grman.image_count, 0)
                self.ae(self.transmit(screen, bytes(size - 1), f=fmt, s=9, v=2).code, 'ENODATA')
                self.ae(self.transmit(screen, bytes(px), f=fmt, s=0, v=1).code, 'EINVAL')
                image = self.image(screen, bytes(size), f=fmt, s=9, v=2)
                # rows of sixteen bit gray start at a multiple of four bytes only for even widths
                self.ae(image['is_4byte_aligned'], fmt != GRAY16)
                self.ae(self.image(screen, bytes(px * 2), f=fmt, s=2, v=1)['is_4byte_aligned'], True)
                self.ae(image['deep'], {GRAY16: 1, RGB10A2: 2, RGBA16: 3}[fmt])
                # compressed payloads count decoded bytes
                self.image(screen, bytes(range(size)), f=fmt, s=9, v=2)
                response = self.transmit(screen, zlib.compress(bytes(range(size))), f=fmt, s=9, v=2, o='z')
                self.ae(response.code, 'OK', response.msg)
                self.ae(screen.grman.image_for_client_id(1)['data'], bytes(range(size)))
        # there is no largest image, the size is computed without overflow and the data is far too short
        self.ae(self.transmit(screen, b'\0' * 8, f=RGBA16, s=100000, v=100000).code, 'ENODATA')
        # a size that overflows the address space is refused
        self.ae(self.transmit(screen, b'\0' * 8, f=RGBA16, s=4294967295, v=4294967295).code, 'EOVERFLOW')

    def test_gray16(self):
        screen = self.create_screen()
        data = struct.pack('<4H', 0, 0x8000, 0xffff, 0x0102)
        image = self.image(screen, data, f=GRAY16, s=4, v=1, G=2)
        # gray has no gamut and is opaque, sRGB unless told otherwise
        self.ae((image['gamut'], image['transfer'], image['deep']), (0, 0, 1))
        expected = []
        for value in (0, 0x8000, 0xffff, 0x0102):
            v = srgb_eotf(value / 65535)
            expected += [v, v, v, 1]
        self.assert_close(self.decoded(screen), expected)

    def test_f316_is_no_longer_rgb565(self):
        # RGB565 moved to f=516; f=316 is sixteen bit gray, so the bytes of RGB565 red are a light gray
        screen = self.create_screen()
        image = self.image(screen, b'\x00\xf8', f=316, s=1, v=1)
        self.ae(image['deep'], 1)
        v = srgb_eotf(0xf800 / 65535)
        self.assert_close(self.decoded(screen), [v, v, v, 1])
        response = self.transmit(screen, b'\x00\xf8', f=516, s=1, v=1)
        self.ae(response.code, 'OK', response.msg)
        image = screen.grman.image_for_client_id(1)
        self.ae((image['data'], image['deep']), (b'\xff\x00\x00', 0))

    def test_rgb10a2_layout(self):
        screen = self.create_screen()
        # red is in the low bits of a little-endian word, alpha in the top two
        self.ae(rgb10a2(1023, 0, 0, 3), b'\xff\x03\x00\xc0')
        self.ae(rgb10a2(512, 512, 512, 3), b'\x00\x02\x08\xe0')
        data = rgb10a2(1023, 0, 0, 3) + rgb10a2(0, 1023, 0, 2) + rgb10a2(0, 0, 1023, 1) + rgb10a2(512, 256, 1, 0)
        image = self.image(screen, data, f=RGB10A2, s=4, v=1, E=LINEAR)
        self.ae((image['deep'], image['transfer']), (2, 1))
        self.assert_close(self.decoded(screen), [
            1, 0, 0, 1, 0, 1, 0, 2 / 3, 0, 0, 1, 1 / 3, 512 / 1023, 256 / 1023, 1 / 1023, 0])
        self.image(screen, rgb10a2(512, 512, 512, 3), f=RGB10A2, s=1, v=1)
        v = srgb_eotf(512 / 1023)
        self.assert_close(self.decoded(screen), [v, v, v, 1])

    def test_rgba16(self):
        screen = self.create_screen()
        data = struct.pack('<8H', 0xffff, 0x8000, 1, 0xffff, 0x1234, 0, 0xfedc, 0x4000)
        image = self.image(screen, data, f=RGBA16, s=2, v=1)
        self.ae((image['deep'], image['transfer']), (3, 0))
        self.assert_close(self.decoded(screen), [
            1, srgb_eotf(0x8000 / 65535), srgb_eotf(1 / 65535), 1,
            srgb_eotf(0x1234 / 65535), 0, srgb_eotf(0xfedc / 65535), 0x4000 / 65535])
        # every code of a channel survives: the stored data is the wire data
        data = struct.pack('<65536H', *range(65536))
        self.image(screen, data, f=RGBA16, s=128, v=128, E=LINEAR)
        self.assert_close(self.decoded(screen)[:8], [0, 1 / 65535, 2 / 65535, 3 / 65535, 4 / 65535, 5 / 65535, 6 / 65535, 7 / 65535], places=9)

    def test_pq_and_hlg(self):
        screen = self.create_screen()
        # HDR10: ten bit PQ in Rec. 2020; neutral colors stay neutral in the sRGB primaries
        codes = (0, round(pq_oetf(203) * 1023), round(pq_oetf(1000) * 1023), 1023)
        data = b''.join(rgb10a2(c, c, c, 3) for c in codes)
        image = self.image(screen, data, f=RGB10A2, s=4, v=1, E=PQ, G=2)
        self.ae((image['transfer'], image['gamut']), (2, 2))
        expected = []
        for c in codes:
            v = pq_eotf(c / 1023)
            expected += [v, v, v, 1]
        # the D65 whites of the sRGB and Rec. 2020 matrices differ in the fourth decimal
        self.assert_close(self.decoded(screen), expected, rel=1e-3)
        # SDR white is the reference white of BT.2408
        self.assertAlmostEqual(pq_eotf(codes[1] / 1023), 1, places=2)
        self.assertAlmostEqual(image['peak'], 10000 / SDR_WHITE_NITS, places=2)
        # a saturated Rec. 2020 color is converted to the sRGB primaries, unclipped
        self.image(screen, rgb10a2(codes[1], 0, 0, 3), f=RGB10A2, s=1, v=1, E=PQ, G=2)
        r, g, b, a = self.decoded(screen)
        self.assertGreater(r, 1)
        self.assertLess(g, 0)
        # HLG: its reference white, a signal of 0.75, is SDR white and its peak 1000 nits
        data = struct.pack('<3H', 0, round(0.75 * 65535), 65535)
        image = self.image(screen, data, f=GRAY16, s=3, v=1, E=HLG)
        self.ae(image['transfer'], 3)
        expected = []
        for c in (0, round(0.75 * 65535), 65535):
            v = hlg_gray(c / 65535)
            expected += [v, v, v, 1]
        self.assert_close(self.decoded(screen), expected, places=4)
        self.assertAlmostEqual(hlg_gray(0.75), 1, places=2)
        self.assertAlmostEqual(image['peak'], HLG_PEAK_NITS / SDR_WHITE_NITS, places=3)
        # SDR transfer functions have no peak
        self.ae(self.image(screen, data, f=GRAY16, s=3, v=1, E=LINEAR)['peak'], 0)

    def test_transfer_key_on_eight_bit_formats(self):
        screen = self.create_screen()
        image = self.image(screen, b'\x00\x80\xff', f=24, s=1, v=1, E=LINEAR)
        self.ae((image['transfer'], image['deep']), (1, 0))
        self.assert_close(self.decoded(screen), [0, 128 / 255, 1, 1])
        image = self.image(screen, b'\xff\xff\xff\x80', f=32, s=1, v=1, E=PQ)
        self.ae(image['transfer'], 2)
        self.assert_close(self.decoded(screen), [10000 / 203] * 3 + [128 / 255], places=3)
        # packed formats are expanded to eight bits and keep the transfer function
        response = self.transmit(screen, b'\xff', f=308, s=1, v=1, E=HLG)
        self.ae(response.code, 'OK', response.msg)
        self.ae(screen.grman.image_for_client_id(1)['transfer'], 3)
        self.assert_close(self.decoded(screen), [HLG_PEAK_NITS / SDR_WHITE_NITS] * 3 + [1], places=3)
        # explicit sRGB is the default, which the GPU decodes by itself
        image = self.image(screen, b'\x00\x80\xff', f=24, s=1, v=1, E=SRGB)
        self.ae(image['transfer'], 0)
        self.assertRaises(ValueError, screen.grman.decoded_image_pixels, 1)

    def test_transfer_key_rejection(self):
        screen = self.create_screen()
        for value in (1, 2, 6, 14, 15, 17, 99, 0xffffffff):
            with self.subTest(E=value):
                self.ae(self.transmit(screen, rgb10a2(0, 0, 0, 3), f=RGB10A2, s=1, v=1, E=value).code, 'EINVAL')
        half = struct.pack('<4e', 1, 1, 1, 1)
        # half floats are linear by definition
        self.ae(self.transmit(screen, half, f=64, s=1, v=1, E=LINEAR).code, 'OK')
        self.ae(screen.grman.image_for_client_id(1)['transfer'], 0)
        for E in (SRGB, PQ, HLG):
            self.ae(self.transmit(screen, half, f=64, s=1, v=1, E=E).code, 'EINVAL')
        # PNG describes itself, indexed color images are in the colors of the terminal and vector paints have their own
        for fmt, data in ((100, b'\x89PNG'), (8, b'\x01'), (16, b'\x01\xff'), (200, b'r 0 0 1 1 #fff'), (401, b'\xff' * 8 + b'\x80')):
            with self.subTest(fmt=fmt):
                self.ae(self.transmit(screen, data, f=fmt, s=1, v=1, E=LINEAR).code, 'EINVAL')

    def test_no_animation_or_composition(self):
        screen = self.create_screen()
        self.image(screen, rgb10a2(0, 0, 0, 3) * 4, f=RGB10A2, s=2, v=2)
        self.ae(self.transmit(screen, b'\0' * 16, a='f', f=32, s=2, v=2).code, 'EINVAL')
        response = parse_full_response(send_command(screen, 'a=c,i=1,r=1,c=1,w=1,h=1,x=1'))
        self.ae(response.code, 'EINVAL')
        self.image(screen, b'\0' * 16, f=32, s=2, v=2)
        self.ae(self.transmit(screen, rgb10a2(0, 0, 0, 3) * 4, a='f', f=RGB10A2, s=2, v=2).code, 'EINVAL')
        self.ae(self.transmit(screen, b'\0' * 16, a='f', f=32, s=2, v=2, E=PQ).code, 'EINVAL')
        self.ae(self.transmit(screen, b'\0' * 16, a='f', f=32, s=2, v=2, E=SRGB).code, 'OK')
        self.image(screen, b'\0' * 12, f=24, s=2, v=2, E=LINEAR)
        self.ae(self.transmit(screen, b'\0' * 12, a='f', f=24, s=2, v=2).code, 'EINVAL')

    def test_queries_and_capabilities(self):
        screen = self.create_screen()

        def q(e, **kw):
            res = send_command(screen, ','.join(f'{k}={v}' for k, v in dict(a='q', i=31, e=e, **kw).items()))
            return res.decode('ascii') if res else None

        def r(keys=''):
            return f'\033_Gi=31{keys};OK\033\\'

        self.ae(q('t', f=RGB10A2, G=2, E=PQ, s=1, v=1), r(',e=t,f=632,G=2,E=16,s=1,v=1'))
        self.ae(q('T', f=GRAY16, E=HLG), r(',e=T,f=316,E=18'))
        self.ae(q('t', f=RGBA16, E=LINEAR), r(',e=t,f=664,E=8'))
        self.ae(q('t', f=516), r(',e=t,f=516'))
        self.ae(q('t', f=24, E=PQ), r(',e=t,f=24,E=16'))
        # unknown values and formats without a transfer function are not reported
        self.ae(q('t', f=RGBA16, E=3), r(',e=t,f=664'))
        self.ae(q('t', f=100, E=PQ), r(',e=t,f=100'))
        self.ae(q('t', f=64, E=PQ), r(',e=t,f=64'))
        self.ae(q('t', f=64, E=LINEAR), r(',e=t,f=64,E=8'))
        # nor are animation frames in high precision or with a transfer function
        self.ae(q('f', f=RGB10A2, E=PQ), r(',e=f'))
        self.ae(q('f', f=516), r(',e=f,f=516'))
        # an a=q query with pixels checks one particular image
        response = self.transmit(screen, rgb10a2(0, 0, 0, 3), a='q', i=31, f=RGB10A2, s=1, v=1, E=PQ, G=2)
        self.ae(response.code, 'OK', response.msg)
        self.ae(screen.grman.image_count, 0)
        screen.callbacks.clear()
        parse_bytes(screen, b'\x1b]777;caps;v=1:op=query:id=7\x1b\\')
        self.assertIn(b':format=8,16,24,32,64,100,200,301,302,304,308,316,401,402,404,408,516,632,664\x1b\\', screen.callbacks.wtcbuf)

    def test_vector_images_draw_high_precision_images(self):
        screen = self.create_screen()
        # the same linear pixels as sixteen bit RGBA and as half floats
        self.image(screen, struct.pack('<8H', 65535, 0, 0, 65535, 0, 65535, 65535, 32768), f=RGBA16, s=2, v=1, i=10, E=LINEAR)
        self.image(screen, struct.pack('<8e', 1, 0, 0, 1, 0, 1, 1, 32768 / 65535), f=64, s=2, v=1, i=11)
        # and in PQ, the decoded value of whose largest code is far above SDR white
        self.image(screen, rgb10a2(1023, 1023, 1023, 3) * 2, f=RGB10A2, s=2, v=1, i=12, E=PQ)
        for image, raster in ((20, 10), (21, 11), (22, 12)):
            response = self.transmit(screen, f'i id=1 img={raster} t=4,0,0,4,0,0'.encode(), a='t', f=200, s=8, v=4, i=image)
            self.ae(response.code, 'OK', response.msg)
        a, b = floats(screen.grman.render_vector_linear(20, 8, 4)), floats(screen.grman.render_vector_linear(21, 8, 4))
        self.assert_close(a, b, places=3)
        self.assertGreater(max(floats(screen.grman.render_vector_linear(22, 8, 4))), 40)

    def test_pointer_images_of_high_precision_images(self):
        # A pointer image is eight bit sRGB RGBA: half float pixels are encoded
        # from their linear values, clamped to SDR white, as decoded ones are
        screen = self.create_screen()

        def srgb8(v):
            v = min(max(v, 0), 1)
            return round(255 * (12.92 * v if v <= 0.0031308 else 1.055 * v ** (1 / 2.4) - 0.055))

        px = (0.5, 2.0, -0.25, 0.25), (0.0031, 1.0, 0.18, 1.0)
        self.image(screen, b''.join(struct.pack('<4e', *p) for p in px), f=64, s=2, v=1, i=1)
        expected = bytes(b for p in px for b in (*map(srgb8, p[:3]), round(255 * p[3])))
        self.ae(screen.grman.rgba_for_client_id(1), expected)
        # the same colors as a decoded image, sixteen bit linear RGBA
        self.image(screen, b''.join(struct.pack('<4H', *(round(65535 * min(max(c, 0), 1)) for c in p)) for p in px), f=RGBA16, s=2, v=1, i=2, E=LINEAR)
        self.ae(screen.grman.rgba_for_client_id(2), expected)

    def test_scaled_decoded_images_are_filtered_in_linear_light(self):
        # The GPU filters a texture before the shader decodes it, so a scaled
        # image the shader decodes is filtered by the shader, in linear light.
        # One drawn at its natural size, and images the GPU decodes itself,
        # keep the hardware path. See graphics.slang.
        from .graphics import parse_response, put_helpers, send_command
        cw, ch = 10, 20
        s, dx, dy, put_image, put_ref, layers, rect_eq = put_helpers(self, cw, ch)
        n = cw * ch
        images = (
            (rgb10a2(100, 600, 1023, 3) * n, dict(f=RGB10A2, E=PQ), True),
            (rgb10a2(100, 600, 1023, 3) * n, dict(f=RGB10A2, E=HLG), True),
            (struct.pack('<4H', 1, 2, 3, 65535) * n, dict(f=RGBA16), True),
            (b'\x10\x80\xf0\xff' * n, dict(f=32, E=PQ), True),
            (b'\x10\x80\xf0\xff' * n, dict(f=32), False),
            (struct.pack('<4e', 0.5, 2, 0, 1) * n, dict(f=64), False),
            (struct.pack('<4H', 1, 2, 3, 65535) * n, dict(f=RGBA16, E=LINEAR), False),
        )
        expected = []
        for i, (data, options, decoded) in enumerate(images, start=1):
            options = ','.join(f'{k}={v}' for k, v in options.items())
            # at its natural size of a cell, then scaled up to two by two cells
            self.ae(parse_response(send_command(s, f'a=T,i={i},p=1,C=1,s={cw},v={ch},{options}', data)), 'OK')
            self.ae(parse_response(send_command(s, f'a=p,i={i},p=2,C=1,c=2,r=2')), 'OK')
            # refs drawn differently are in groups of their own, the others in one
            expected += [(decoded, True, False, 1 if decoded else 2), (decoded, False, decoded, 1)]
        found = [(x['image_id'], x['ref_id'], x['is_unscaled'], x['filter_decoded'], x['group_count']) for x in layers(s)]
        found.sort()
        self.ae(len(found), len(expected))
        for (image_id, ref_id, is_unscaled, filter_decoded, group_count), (decoded, natural, filtered, count) in zip(found, expected):
            # only images the shader decodes are told apart by their scale
            self.ae((is_unscaled, filter_decoded, group_count), (natural and decoded, filtered, count), f'image {image_id} ref {ref_id}')
