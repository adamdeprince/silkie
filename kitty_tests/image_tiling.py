#!/usr/bin/env python
# License: GPLv3 Copyright: 2026, Adam DePrince

# Images of any size, drawn from tiles when larger than the GPU's largest
# texture, see docs/silkie/image-tiling.rst

import struct
import zlib

from kitty.fast_data_types import set_image_tile_limit

from .base import BaseTest
from .graphics import make_png, parse_full_response, send_command


def transmit(screen, data, **options):
    options.setdefault('a', 't')
    options.setdefault('i', 1)
    command = ','.join(f'{key}={value}' for key, value in options.items())
    return parse_full_response(send_command(screen, command, data))


def gray_png(width, height):
    # an eight bit gray PNG, small however large it is, as its rows compress well
    def chunk(kind, payload):
        return struct.pack('>I', len(payload)) + kind + payload + struct.pack('>I', zlib.crc32(kind + payload))
    rows = (b'\0' + bytes(i % 256 for i in range(width))) * height
    return b'\x89PNG\r\n\x1a\n' + chunk(b'IHDR', struct.pack('>IIBBBBB', width, height, 8, 0, 0, 0, 0)) + chunk(
        b'IDAT', zlib.compress(rows)) + chunk(b'IEND', b'')


class TestImageTiling(BaseTest):

    def setUp(self):
        super().setUp()
        set_image_tile_limit(0)

    def tearDown(self):
        set_image_tile_limit(0)
        super().tearDown()

    def rgb(self, width, height):
        # every pixel knows where it is: red and green the column, blue the row, modulo 256
        return b''.join(bytes((x % 256, (x >> 8) % 256, y % 256)) for y in range(height) for x in range(width))

    def test_images_larger_than_ten_thousand_pixels(self):
        s = self.create_screen()
        g = s.grman
        for width, height in ((20000, 100), (100, 40000)):
            with self.subTest(width=width, height=height):
                data = bytes(width * height * 3)
                r = transmit(s, zlib.compress(data), f=24, s=width, v=height, o='z')
                self.ae(r.code, 'OK', r.msg)
                img = g.image_for_client_id(1)
                self.ae((img['width'], img['height'], len(img['data'])), (width, height, len(data)))
                # without a GPU, as in the tests, nothing is tiled
                self.assertIsNone(img['tiles'])
                # a feature query reports the size as supported
                res = send_command(s, f'a=q,i=31,e=t,f=24,s={width},v={height}').decode('ascii')
                self.ae(res, f'\033_Gi=31,e=t,f=24,s={width},v={height};OK\033\\')
        # PNG has no largest size either, only the one the format allows
        r = transmit(s, gray_png(20000, 3), f=100, i=2)
        self.ae(r.code, 'OK', r.msg)
        img = g.image_for_client_id(2)
        self.ae((img['width'], img['height']), (20000, 3))
        self.ae(img['data'][4 * 300:4 * 301], bytes((300 % 256,) * 3 + (255,)))
        # pixels sent directly arrive in parts of any size, the buffer grows as they come
        width, height = 12000, 2
        data = self.rgb(width, height)
        for offset in range(0, len(data), 4095):
            more = int(offset + 4095 < len(data))
            header = f'a=t,i=3,f=24,s={width},v={height},' if not offset else ''
            res = send_command(s, header + f'm={more}', data[offset:offset + 4095])
        self.ae(parse_full_response(res).code, 'OK')
        self.ae(g.image_for_client_id(3)['data'], data)

    def put(self, s, img_id=1, **keys):
        command = ','.join(f'{k}={v}' for k, v in dict(a='p', i=img_id, **keys).items())
        return parse_full_response(send_command(s, command))

    def layers(self, s, cw, ch):
        return s.grman.update_layers(0, -1, 1, 2 / s.columns, 2 / s.lines, s.columns, s.lines, cw, ch)

    def assert_rect(self, actual, left, top, right, bottom, places=5):
        for side, expected in zip(('left', 'top', 'right', 'bottom'), (left, top, right, bottom)):
            self.assertAlmostEqual(actual[side], expected, places=places, msg=f'{side} of {actual}')

    def check_partition(self, items, whole):
        # the parts of the tiles cover the placement exactly: the outer edges are those of the placement and
        # neighboring parts share their edges, to the bit, so that there is no gap or overlap between them
        cols = sorted({it['tile'][0] for it in items})
        rows = sorted({it['tile'][1] for it in items})
        by_tile = {it['tile']: it['dest_rect'] for it in items}
        self.ae(len(by_tile), len(cols) * len(rows))
        # the outer edges are those of the placement, in single precision as it is
        for c in cols:
            self.assertAlmostEqual(by_tile[(c, rows[0])]['top'], whole['top'], places=5)
            self.assertAlmostEqual(by_tile[(c, rows[-1])]['bottom'], whole['bottom'], places=5)
        for r in rows:
            self.assertAlmostEqual(by_tile[(cols[0], r)]['left'], whole['left'], places=5)
            self.assertAlmostEqual(by_tile[(cols[-1], r)]['right'], whole['right'], places=5)
        for (c, r), d in by_tile.items():
            if (c + 1, r) in by_tile:
                self.ae(d['right'], by_tile[(c + 1, r)]['left'])
            if (c, r + 1) in by_tile:
                self.ae(d['bottom'], by_tile[(c, r + 1)]['top'])
            self.ae(d['top'], by_tile[(cols[0], r)]['top'])
            self.ae(d['left'], by_tile[(c, rows[0])]['left'])

    def test_tile_layout(self):
        cw, ch = 10, 20
        s = self.create_screen(cols=30, lines=10, cell_width=cw, cell_height=ch)
        g = s.grman
        # tiles of at most 100 pixels: an interior of 98 and an apron of a pixel either side
        set_image_tile_limit(100)
        width, height = 250, 120
        r = transmit(s, zlib.compress(self.rgb(width, height)), a='T', f=24, s=width, v=height, o='z')
        self.ae(r.code, 'OK', r.msg)
        img = g.image_for_client_id(1)
        self.ae(img['tiles'], {'cols': 3, 'rows': 2, 'pitch': 98})
        self.ae(img['data'], self.rgb(width, height))
        items = self.layers(s, cw, ch)
        self.ae(len(items), 6)
        self.ae([it['tile'] for it in items], [(0, 0), (1, 0), (2, 0), (0, 1), (1, 1), (2, 1)])
        self.ae({it['group_count'] for it in items}, {1})
        # the placement at its natural size, at the top left of a 300x200 window
        whole = {'left': -1, 'top': 1, 'right': -1 + 2 * width / 300, 'bottom': 1 - 2 * height / 200}
        self.check_partition(items, whole)
        by_tile = {it['tile']: it for it in items}
        # the interior of each tile in its texture, which begins with the apron
        self.assert_rect(by_tile[(0, 0)]['src_rect'], 1 / 100, 1 / 100, 99 / 100, 99 / 100)
        self.assert_rect(by_tile[(2, 0)]['src_rect'], 1 / 56, 1 / 100, 55 / 56, 99 / 100)
        self.assert_rect(by_tile[(1, 1)]['src_rect'], 1 / 100, 1 / 24, 99 / 100, 23 / 24)
        # the edge between the first two columns is where pixel 98 of 250 is
        self.assert_rect(by_tile[(0, 0)]['dest_rect'], -1, 1, -1 + 2 * 98 / 300, 1 - 2 * 98 / 200)
        # the counts of the layers are of the parts drawn
        self.ae(g.image_for_client_id(1)['refs.count'], 1)

    def test_crops_across_tile_boundaries(self):
        cw, ch = 10, 20
        s = self.create_screen(cols=30, lines=10, cell_width=cw, cell_height=ch)
        set_image_tile_limit(100)
        width, height = 250, 120
        self.ae(transmit(s, zlib.compress(self.rgb(width, height)), f=24, s=width, v=height, o='z').code, 'OK')
        # 20x100 pixels from (90, 10), across the boundaries at x=98 and y=98, shown at twice the size in 4x10 cells
        self.ae(self.put(s, x=90, y=10, w=20, h=100, c=4, r=10).code, 'OK')
        items = self.layers(s, cw, ch)
        self.ae(sorted(it['tile'] for it in items), [(0, 0), (0, 1), (1, 0), (1, 1)])
        whole = {'left': -1, 'top': 1, 'right': -1 + 2 * 40 / 300, 'bottom': -1}
        self.check_partition(items, whole)
        by_tile = {it['tile']: it for it in items}
        # pixels 90 to 98 of the first column of tiles and 98 to 110 of the second
        self.assert_rect(by_tile[(0, 0)]['src_rect'], 91 / 100, 11 / 100, 99 / 100, 99 / 100)
        self.assert_rect(by_tile[(1, 1)]['src_rect'], 1 / 100, 1 / 24, 13 / 100, 13 / 24)
        # split at 8/20 of the width and 88/100 of the height
        self.assert_rect(by_tile[(0, 0)]['dest_rect'], -1, 1, -1 + 2 * 40 / 300 * 8 / 20, 1 - 2 * 88 / 100)
        # a crop inside one tile is one part
        send_command(s, 'a=p,i=1,x=10,y=10,w=20,h=20,p=2')
        items = [it for it in self.layers(s, cw, ch) if it['tile'] == (0, 0) and abs(it['src_rect']['left'] - 11 / 100) < 1e-6]
        self.ae(len(items), 1)
        self.assert_rect(items[0]['src_rect'], 11 / 100, 11 / 100, 31 / 100, 31 / 100)

    def test_tiles_off_screen_are_not_drawn(self):
        cw, ch = 10, 20
        # a window of 100x100 pixels
        s = self.create_screen(cols=10, lines=5, cell_width=cw, cell_height=ch)
        set_image_tile_limit(100)
        width, height = 250, 300
        self.ae(transmit(s, zlib.compress(bytes(width * height * 3)), a='T', f=24, s=width, v=height, o='z', C=1).code, 'OK')
        items = self.layers(s, cw, ch)
        # the first column of tiles ends at 98, the second shows two pixels, the rows likewise
        self.ae(sorted(it['tile'] for it in items), [(0, 0), (0, 1), (1, 0), (1, 1)])
        # placed at (50, 40) only the first tile is on screen, it ends at (148, 138)
        s.reset()
        s.cursor_position(3, 6)
        self.ae(transmit(s, zlib.compress(bytes(width * height * 3)), a='T', f=24, s=width, v=height, o='z', C=1).code, 'OK')
        items = self.layers(s, cw, ch)
        self.ae([it['tile'] for it in items], [(0, 0)])
        self.assert_rect(items[0]['dest_rect'], -1 + 2 * 50 / 100, 1 - 2 * 40 / 200 * 2, -1 + 2 * 148 / 100, 1 - 2 * 138 / 100)

    def test_minified_images(self):
        cw, ch = 10, 20
        s = self.create_screen(cols=30, lines=10, cell_width=cw, cell_height=ch)
        set_image_tile_limit(100)
        width, height = 250, 120
        self.ae(transmit(s, zlib.compress(bytes(width * height * 3)), f=24, s=width, v=height, o='z').code, 'OK')
        # the whole image in 5x2 cells, 50x40 pixels
        self.ae(self.put(s, c=5, r=2).code, 'OK')
        items = self.layers(s, cw, ch)
        self.ae(len(items), 6)
        whole = {'left': -1, 'top': 1, 'right': -1 + 2 * 50 / 300, 'bottom': 1 - 2 * 40 / 200}
        self.check_partition(items, whole)

    def test_unicode_placeholders_spanning_tiles(self):
        cw, ch = 10, 20
        s = self.create_screen(cols=30, lines=10, cell_width=cw, cell_height=ch)
        set_image_tile_limit(100)
        width, height = 250, 120
        self.ae(transmit(s, zlib.compress(self.rgb(width, height)), f=24, s=width, v=height, o='z', i=42).code, 'OK')
        # a virtual placement 25x6 cells, the image at its natural size
        self.ae(self.put(s, img_id=42, U=1, c=25, r=6).code, 'OK')
        s.apply_sgr('38;5;42')
        # all of row 0 and the first ten cells of row 5 of the image
        s.draw('\U0010eeee\u0305\u0305' + '\U0010eeee' * 24)
        s.cursor_position(6, 0)
        s.draw('\U0010eeee\u033d\u0305' + '\U0010eeee' * 9)
        s.update_only_line_graphics_data()
        items = self.layers(s, cw, ch)
        # row 0 of the image is pixels 0 to 20, in the first row of tiles, all three columns of it
        row0 = [it for it in items if abs(it['dest_rect']['top'] - 1) < 1e-6]
        self.ae(sorted(it['tile'] for it in row0), [(0, 0), (1, 0), (2, 0)])
        self.check_partition(row0, {'left': -1, 'top': 1, 'right': -1 + 2 * 250 / 300, 'bottom': 1 - 2 * 20 / 200})
        self.assert_rect({it['tile']: it for it in row0}[(1, 0)]['src_rect'], 1 / 100, 1 / 100, 99 / 100, 21 / 100)
        # row 5 is pixels 100 to 120 of the image, the second row of tiles, and its first ten cells 100 pixels
        row5 = [it for it in items if it not in row0]
        self.ae(sorted(it['tile'] for it in row5), [(0, 1), (1, 1)])
        by_tile = {it['tile']: it for it in row5}
        self.assert_rect(by_tile[(0, 1)]['src_rect'], 1 / 100, 3 / 24, 99 / 100, 23 / 24)
        self.assert_rect(by_tile[(1, 1)]['src_rect'], 1 / 100, 3 / 24, 3 / 100, 23 / 24)

    def test_indexed_and_palette_images(self):
        cw, ch = 10, 20
        s = self.create_screen(cols=30, lines=10, cell_width=cw, cell_height=ch)
        set_image_tile_limit(100)
        width, height = 250, 120
        indices = bytes((x + y) % 256 for y in range(height) for x in range(width))
        palette = bytes(range(256)) * 4
        self.ae(transmit(s, zlib.compress(palette + indices), a='T', f=408, s=width, v=height, o='z').code, 'OK')
        img = s.grman.image_for_client_id(1)
        self.ae((img['tiles'], img['data']), ({'cols': 3, 'rows': 2, 'pitch': 98}, indices))
        self.ae(transmit(s, zlib.compress(indices), a='T', f=8, s=width, v=height, o='z', i=2).code, 'OK')
        for item in self.layers(s, cw, ch):
            self.assertTrue(item['tile'])
            self.ae(item['indexed_px_sz'], 1)
            # at its natural size every part is drawn with the program that reads each pixel once
            self.assertTrue(item['is_unscaled'])
        # pixels of the palette image as RGBA, as pointer and vector images use them
        rgba = s.grman.rgba_for_client_id(1)
        self.ae(len(rgba), width * height * 4)
        self.ae(rgba[4 * 251:4 * 252], palette[4 * 2:4 * 3])

    def test_images_that_fit_are_not_tiled(self):
        s = self.create_screen()
        set_image_tile_limit(100)
        for width, height, tiled in ((100, 100, False), (101, 1, True), (1, 101, True)):
            self.ae(transmit(s, zlib.compress(bytes(width * height * 4)), f=32, s=width, v=height, o='z').code, 'OK')
            self.ae(s.grman.image_for_client_id(1)['tiles'] is not None, tiled)
        # vector images are drawn at the size they are shown at, the textures of which fit
        self.ae(transmit(s, b's id=1 d=M0,0L10,10', f=200, s=500, v=500, i=2).code, 'OK')
        self.assertIsNone(s.grman.image_for_client_id(2)['tiles'])
        # the part of a vector image that is rasterized fits a texture
        # a 30000x100 image at its natural size, its first 3000 pixels in a window 3000x2000
        v = s.grman.choose_vector_view(30000, 100, (0, 0, 0.1, 1), 3000, 2000, (0, 0, 0, 0, 0, 0), 8192)
        self.ae(v, (30000, 100, 0, 0, 3002 + 750, 100))
        # without a limit it is all of it
        self.ae(s.grman.choose_vector_view(30000, 100, (0, 0, 0.1, 1), 3000, 2000)[4], 30000)
        # a window larger than a texture shows the image at less than its size
        v = s.grman.choose_vector_view(30000, 100, (0, 0, 1, 1), 30000, 2000, (0, 0, 0, 0, 0, 0), 8192)
        self.assertLessEqual(v[4], 8192)
        self.assertLess(v[0], 30000)

    def test_animation_of_a_tiled_image(self):
        cw, ch = 10, 20
        s = self.create_screen(cols=30, lines=10, cell_width=cw, cell_height=ch)
        set_image_tile_limit(100)
        width, height = 250, 120
        self.ae(transmit(s, zlib.compress(bytes(width * height * 4)), a='T', f=32, s=width, v=height, o='z').code, 'OK')
        r = transmit(s, zlib.compress(b'\xff' * 400), a='f', f=32, s=10, v=10, x=95, y=95, o='z')
        self.ae(r.code, 'OK', r.msg)
        send_command(s, 'a=a,i=1,c=2')
        img = s.grman.image_for_client_id(1)
        self.ae(img['current_frame_index'], 1)
        frame = img['extra_frames'][0]['data']
        # the frame composed onto the root frame, across four tiles
        self.ae(frame[4 * (100 * width + 100):4 * (100 * width + 101)], b'\xff' * 4)
        self.ae(frame[4 * (90 * width + 90):4 * (90 * width + 91)], bytes(4))
        self.ae(len(self.layers(s, cw, ch)), 6)

    def test_png_larger_than_ten_thousand_pixels_tiles(self):
        s = self.create_screen()
        set_image_tile_limit(8192)
        rgba = bytes(4) * 10001
        self.ae(transmit(s, make_png(rgba, 10001), f=100).code, 'OK')
        self.ae(s.grman.image_for_client_id(1)['tiles'], {'cols': 3, 'rows': 1, 'pitch': 4094})

    def test_high_precision_images_tile_as_decoded_half_floats(self):
        # The tiles of an image the shader decodes hold its decoded pixels,
        # linear, which the GPU filters and mipmaps, see docs/silkie/image-tiling.rst
        cw, ch = 10, 20
        s = self.create_screen(cols=30, lines=10, cell_width=cw, cell_height=ch)
        set_image_tile_limit(100)
        width, height = 250, 120
        for i, (fmt, extra, px, decoded) in enumerate((
            (664, '', 8, True),  # 16 bit RGBA, sRGB
            (632, ',E=16', 4, True),  # RGB10A2, PQ
            (316, '', 2, True),  # 16 bit gray
            (664, ',E=8', 8, False),  # 16 bit linear RGBA, which the GPU filters as it is
            (32, ',E=18', 4, True),  # eight bit HLG
        ), start=1):
            with self.subTest(fmt=fmt, extra=extra):
                s.reset()
                data = zlib.compress(bytes(width * height * px))
                r = parse_full_response(send_command(s, f'a=T,i={i},f={fmt},s={width},v={height},o=z{extra}', data))
                self.ae(r.code, 'OK', r.msg)
                self.ae(s.grman.image_for_client_id(i)['tiles'], {'cols': 3, 'rows': 2, 'pitch': 98})
                items = self.layers(s, cw, ch)
                self.ae(len(items), 6)
                for item in items:
                    self.ae(item['linear_texture'], decoded)
                    self.assertFalse(item['filter_decoded'] and decoded)

    def test_tile_pixels_and_aprons(self):
        # A tile holds its interior and an apron of a pixel all round: the
        # pixels of its neighbors, or at the edge of the image transparent
        # black for colors and the edge pixel for indices
        s = self.create_screen()
        set_image_tile_limit(100)
        width, height = 250, 120
        rgb = self.rgb(width, height)
        self.ae(transmit(s, zlib.compress(rgb), f=24, s=width, v=height, o='z').code, 'OK')

        def rgba_at(x, y):
            o = 3 * (y * width + x)
            return rgb[o:o + 3] + b'\xff'

        def tile(col, row, i=1):
            t = s.grman.tile_pixels(i, col, row)
            px = len(t['data']) // (t['width'] * t['height'])
            return t, lambda u, v: t['data'][px * (v * t['width'] + u):px * (v * t['width'] + u + 1)]

        t, at = tile(0, 0)
        self.ae((t['width'], t['height'], t['format']), (100, 100, 0))
        self.ae(at(0, 0), bytes(4))
        self.ae(at(0, 5), bytes(4))
        self.ae(at(5, 0), bytes(4))
        self.ae(at(1, 1), rgba_at(0, 0))
        self.ae(at(98, 98), rgba_at(97, 97))
        # the apron on the right and at the bottom is the first pixels of the next tiles
        self.ae(at(99, 1), rgba_at(98, 0))
        self.ae(at(1, 99), rgba_at(0, 98))
        t, at = tile(1, 1)
        self.ae((t['width'], t['height']), (100, 24))
        self.ae(at(0, 0), rgba_at(97, 97))
        self.ae(at(1, 1), rgba_at(98, 98))
        self.ae(at(50, 22), rgba_at(147, 119))
        self.ae(at(50, 23), bytes(4))
        t, at = tile(2, 1)
        self.ae((t['width'], t['height']), (56, 24))
        self.ae(at(54, 22), rgba_at(249, 119))
        self.ae(at(55, 10), bytes(4))
        # every pixel of the image is in the interior of one tile
        for col in range(3):
            for row in range(2):
                t, at = tile(col, row)
                for v in range(1, t['height'] - 1, 7):
                    for u in range(1, t['width'] - 1, 5):
                        self.ae(at(u, v), rgba_at(col * 98 + u - 1, row * 98 + v - 1))
        # indices: the apron at the edges of the image is the edge pixel
        indices = bytes((x * 3 + y) % 256 for y in range(height) for x in range(width))
        self.ae(transmit(s, zlib.compress(indices), f=8, s=width, v=height, o='z', i=2).code, 'OK')
        t, at = tile(0, 0, 2)
        self.ae(t['format'], 6)
        self.ae(at(0, 0), indices[0:1])
        self.ae(at(0, 7), indices[6 * width:6 * width + 1])
        self.ae(at(1, 1), indices[0:1])
        self.ae(at(2, 1), indices[1:2])
        self.ae(at(99, 1), indices[98:99])
        t, at = tile(2, 1, 2)
        self.ae(at(55, 23), indices[-1:])
        self.ae(at(54, 22), indices[-1:])
        self.ae(at(55, 5), indices[(98 + 4) * width + 249:(98 + 4) * width + 250])

    def test_decoded_tile_pixels(self):
        # sixteen bit sRGB RGBA becomes linear half floats as it is uploaded
        s = self.create_screen()
        set_image_tile_limit(100)
        width, height = 150, 10
        data = b''.join(struct.pack('<4H', x * 400, 32768, 65535, 65535) for y in range(height) for x in range(width))
        self.ae(transmit(s, zlib.compress(data), f=664, s=width, v=height, o='z').code, 'OK')
        t = s.grman.tile_pixels(1, 1, 0)
        self.ae((t['width'], t['height'], t['format']), (54, 12, 1))

        def linear(v):
            v /= 65535
            return v / 12.92 if v <= 0.04045 else ((v + 0.055) / 1.055) ** 2.4

        def at(u, v):
            o = 8 * (v * t['width'] + u)
            return struct.unpack('<4e', t['data'][o:o + 8])
        # pixel (120, 3) is at (120 - 98 + 1, 4) in the texture of the second tile
        r, g, b, a = at(23, 4)
        self.assertAlmostEqual(r, linear(120 * 400), places=2)
        self.assertAlmostEqual(g, linear(32768), places=2)
        self.ae((b, a), (1.0, 1.0))
        self.ae(at(53, 4), (0, 0, 0, 0))

    def test_64_bit_size_and_offset(self):
        # S and O are 64 bits: data past 4 GiB in a file is read, see
        # docs/silkie/image-tiling.rst. The file is sparse, nothing is written
        # before the image.
        import tempfile
        s = self.create_screen()
        width, height = 7, 3
        rgb = self.rgb(width, height)
        offset = 2**32 + 12345
        with tempfile.NamedTemporaryFile(suffix='.rgb') as f:
            f.seek(offset)
            f.write(rgb)
            f.write(b'tail')
            f.flush()
            path = f.name.encode()
            r = transmit(s, path, t='f', f=24, s=width, v=height, O=offset, S=len(rgb))
            self.ae(r.code, 'OK', r.msg)
            self.ae(s.grman.image_for_client_id(1)['data'], rgb)
            # without S, all from the offset on, which for raw pixels is what they need
            r = transmit(s, path, t='f', f=24, s=width, v=height, O=offset, i=2)
            self.ae(r.code, 'OK', r.msg)
            self.ae(s.grman.image_for_client_id(2)['data'], rgb)
            # a compressed payload past 4 GiB, its size more than 32 bits can say in a second file
        with tempfile.NamedTemporaryFile(suffix='.z') as f:
            data = zlib.compress(rgb)
            f.seek(2**32 + 7)
            f.write(data)
            f.flush()
            r = transmit(s, f.name.encode(), t='f', f=24, s=width, v=height, o='z', O=2**32 + 7, S=len(data), i=3)
            self.ae(r.code, 'OK', r.msg)
            self.ae(s.grman.image_for_client_id(3)['data'], rgb)
            # an offset and size whose end does not fit in a file offset are refused before the file is opened
            for o, size in ((2**64 - 1, 2), (2**63, 0), (2**63 - 1, 1)):
                r = transmit(s, f.name.encode(), t='f', f=24, s=width, v=height, O=o, S=size, i=4)
                self.ae(r.code, 'EOVERFLOW', (o, size))
            # past the end of the file is insufficient data, reported as the generic file error
            r = transmit(s, f.name.encode(), t='f', f=24, s=width, v=height, O=2**40, i=4)
            self.ae(r.code, 'EBADF')
        # a value that does not fit in 64 bits is a malformed command, and has no reply
        self.assertFalse(send_command(s, 'a=t,i=5,t=f,f=24,s=1,v=1,O=%d' % 2**64, b'/x'))
        self.assertFalse(send_command(s, 'a=t,i=5,t=f,f=24,s=1,v=1,O=123456789012345678901', b'/x'))
        # the feature query echoes them, so that a program can tell before it relies on them; a terminal whose keys
        # are 32 bits answers nothing, the command is malformed there
        res = send_command(s, f'a=q,i=31,e=t,t=f,O={2**32},S={2**40}').decode('ascii')
        self.ae(res, f'\033_Gi=31,e=t,t=f,S={2**40},O={2**32};OK\033\\')

    def test_64_bit_offset_in_shared_memory(self):
        # On Linux a POSIX shared memory object is a file in /dev/shm, which can be made sparse
        import os
        import unittest
        if not os.path.isdir('/dev/shm'):
            raise unittest.SkipTest('POSIX shared memory objects are not files here')
        s = self.create_screen()
        width, height = 5, 2
        rgb = self.rgb(width, height)
        name = f'/silkie-test-tiling-{os.getpid()}'
        offset = 2**32 + 99
        with open('/dev/shm' + name, 'wb') as f:
            f.seek(offset)
            f.write(rgb)
        try:
            r = transmit(s, name.encode(), t='s', f=24, s=width, v=height, O=offset, S=len(rgb))
            self.ae(r.code, 'OK', r.msg)
            self.ae(s.grman.image_for_client_id(1)['data'], rgb)
            # the terminal removes the object once it has read it
            self.assertFalse(os.path.exists('/dev/shm' + name))
        finally:
            if os.path.exists('/dev/shm' + name):
                os.unlink('/dev/shm' + name)
