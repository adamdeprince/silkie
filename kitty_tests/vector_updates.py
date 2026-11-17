#!/usr/bin/env python
# License: GPLv3 Copyright: 2026, Adam DePrince

from .base import BaseTest
from .graphics import parse_full_response, send_command


class TestVectorUpdates(BaseTest):

    def test_partial_redraw_preserves_tiny_coverage(self):
        screen = self.create_screen()
        graphics = screen.grman
        # All rectangles belong to one fill. In a full render the opaque
        # rectangles widen the emitted span to include the very thin one. A
        # cropped redraw must retain that thin rectangle's coverage even when
        # it is the only positive coverage in its row.
        retained = 'p id=1 d=M0,5h1v1h-1ZM12,5h1v1h-1ZM5,5h0.000244140625v1h-0.000244140625Z fill=#ff8800'
        marker = 'p id=2 d=M4,5h3v1h-3Z fill='

        def command(image, text, action='t'):
            response = parse_full_response(send_command(screen, f'a={action},f=200,i={image},s=16,v=12', text))
            self.ae(response.code, 'OK', response.msg)

        command(1, retained + '\n' + marker + '#0044cc20')
        pixels = bytearray()
        graphics.update_vector_pixels(1, 16, 12, pixels)
        command(1, marker + '#0044cc80', 'e')
        rect = graphics.update_vector_pixels(1, 16, 12, pixels)
        self.assertIsNotNone(rect)
        self.assertGreater(rect[0], 1)
        command(2, retained + '\n' + marker + '#0044cc80')
        expected = graphics.render_vector_image(2, 16, 12)
        self.assertGreater(expected[(5 * 16 + 5) * 4], 0)
        self.ae(pixels, expected)

    def test_partial_redraw_of_retained_stroke_subpaths(self):
        screen = self.create_screen()
        graphics = screen.grman
        subpaths = []
        for row in range(6):
            for col in range(8):
                x, y = 4 + col * 12, 4 + row * 14
                subpaths.append(f'M{x},{y}L{x},{y}L{x+3},{y+4}L{x+3},{y+4}L{x+6},{y}'
                                f'M{x+8},{y}L{x+8},{y}'
                                f'M{x},{y+7}L{x+5},{y+7}L{x+.5},{y+7.5}Z')
        geometry = ''.join(subpaths)
        width, height = 192, 128

        def command(image, text, action='t'):
            response = parse_full_response(send_command(screen, f'a={action},f=200,i={image},s={width},v={height}', text))
            self.ae(response.code, 'OK', response.msg)

        transforms = ((1.8, .3, .45, .65, 2, 2), (-1.6, .3, .4, .75, 190, 2))
        for transform in transforms:
            a, b, c, d, e, f = transform
            matrix = ','.join(map(str, transform))
            for cap in ('round', 'square'):
                for join in ('miter', 'round', 'bevel'):
                    retained = f'p id=1 d={geometry} stroke=#ff8800a0 w=3 cap={cap} join={join} t={matrix}'
                    # The marker exposes underlying strokes around an end cap,
                    # an acute join and a repeated-point-only subpath. Most
                    # other subpaths are far outside each small damaged area.
                    for x, y in ((40, 32), (43, 36), (48, 32), (40.5, 39.5)):
                        px, py = a*x + c*y + e, b*x + d*y + f
                        marker = f'p id=2 d=M{px-4},{py-4}h8v8h-8Z fill='
                        command(1, retained + '\n' + marker + '#0044cc20')
                        pixels = bytearray()
                        graphics.update_vector_pixels(1, width, height, pixels)
                        command(1, marker + '#0044cc80', 'e')
                        rect = graphics.update_vector_pixels(1, width, height, pixels)
                        self.assertIsNotNone(rect)
                        self.assertLess(rect[2] * rect[3], width * height // 50)
                        command(2, retained + '\n' + marker + '#0044cc80')
                        self.ae(pixels, graphics.render_vector_image(2, width, height), f'{cap=} {join=} {transform=} {x=} {y=}')

    def test_partial_redraw_preserves_dashed_budget(self):
        screen = self.create_screen(options={'vector_dash_limit': 8})
        graphics = screen.grman
        # The first subpath, away from the marker, spends six of the eight dash
        # ends, and the second, beside it, then falls back to solid. Leaving out
        # the first because it is outside the part drawn again would give the
        # second a fresh budget. Dashes off the image are not cut, so not counted.
        retained = 'p id=1 d=M2,10L7,10M18,30L22,30 stroke=#ff8800 w=2 dash=1,1'
        marker = 'p id=2 d=M18,29h2v2h-2Z fill='

        def command(image, text, action='t'):
            response = parse_full_response(send_command(screen, f'a={action},f=200,i={image},s=48,v=48', text))
            self.ae(response.code, 'OK', response.msg)

        command(1, retained + '\n' + marker + '#0044cc20')
        pixels = bytearray()
        graphics.update_vector_pixels(1, 48, 48, pixels)
        self.ae(pixels[(29 * 48 + 19) * 4 + 3], 255)
        command(1, marker + '#0044cc80', 'e')
        rect = graphics.update_vector_pixels(1, 48, 48, pixels)
        self.assertIsNotNone(rect)
        self.assertLess(rect[2] * rect[3], 48 * 48 // 10)
        command(2, retained + '\n' + marker + '#0044cc80')
        self.ae(pixels, graphics.render_vector_image(2, 48, 48))

    def test_replaced_elements_keep_drawing_order(self):
        screen = self.create_screen()
        graphics = screen.grman
        records = {
            10: 'g id=10',
            20: 'g id=20',
            1: 'p id=1 g=10 d=M1,1L23,1L23,19L1,19Z fill=#ff000080',
            2: 'p id=2 g=10 d=M7,3L29,3L29,21L7,21Z fill=#0000ff80',
            3: 'p id=3 g=20 d=M3,7L27,7L27,17L3,17Z fill=#00ff0080',
        }

        def command(image, text, action='t'):
            response = parse_full_response(send_command(screen, f'a={action},f=200,i={image},s=32,v=24', text))
            self.ae(response.code, 'OK', response.msg)

        command(1, '\n'.join(records.values()))
        pixels = bytearray()
        graphics.update_vector_pixels(1, 32, 24, pixels)

        def check():
            command(2, '\n'.join(records.values()))
            graphics.update_vector_pixels(1, 32, 24, pixels)
            self.ae(pixels, graphics.render_vector_image(2, 32, 24))
            self.ae(graphics.render_vector_linear(1, 32, 24), graphics.render_vector_linear(2, 32, 24))

        def replace(ident, record):
            records[ident] = record
            command(1, record, 'e')
            check()

        for i in range(32):
            replace(1, f'p id=1 g=10 d=M{i%5},1L23,1L23,19L1,19Z fill=#ff{(i*19)%256:02x}0080')
            replace(2, f'p id=2 g=10 d=M7,3L29,3L29,21L7,21Z fill=#00{(i*13)%256:02x}ff80')
        # Parent changes, and transitions into and out of non-drawing resources,
        # must rebuild the cached order rather than keep the replaced pointer.
        replace(1, 'p id=1 g=20 d=M1,1L23,1L23,19L1,19Z fill=#ff000080')
        replace(1, 's id=1 d=M1,1L23,1L23,19L1,19Z')
        replace(1, 'p id=1 g=10 d=M1,1L23,1L23,19L1,19Z fill=#ff000080')
        replace(1, 'l id=1 x1=0 y1=0 x2=32 y2=0 stops=0,#ff0000,1,#0000ff')
        replace(1, 'p id=1 g=20 d=M1,1L23,1L23,19L1,19Z fill=#ff000080')
        # A group replacement must preserve the relation to its descendants.
        replace(10, 'g id=10 p=20 t=1,0,0,1,2,0')
        replace(1, 'g id=1 p=10 o=75')
        replace(4, 'p id=4 g=1 d=M1,1L23,1L23,19L1,19Z fill=#ffff0080')
        command(1, 'x g=10', 'e')
        for ident in (10, 1, 2, 4):
            records.pop(ident)
        check()
        replace(10, 'g id=10')
        replace(1, 'p id=1 g=10 d=M1,1L23,1L23,19L1,19Z fill=#ff000080')

    def test_sustained_vector_replacements(self):
        screen = self.create_screen()
        graphics = screen.grman
        records = {
            ident: f'p id={ident} d=M{ident%16},{ident//16%12}h2v2h-2Z fill=#0000ff80'
            for ident in range(1, 258)
        }

        def command(image, text, action='t'):
            response = parse_full_response(send_command(screen, f'a={action},f=200,i={image},s=32,v=24', text))
            self.ae(response.code, 'OK', response.msg)

        command(1, '\n'.join(records.values()))
        pixels = bytearray()
        graphics.update_vector_pixels(1, 32, 24, pixels)
        for iteration in range(512):
            ident = iteration % 257 + 1
            record = f'p id={ident} d=M{ident%16},{ident//16%12}h2v2h-2Z fill=#{iteration%256:02x}40ff80'
            records[ident] = record
            command(1, record, 'e')
            graphics.update_vector_pixels(1, 32, 24, pixels)
            if iteration % 31 == 0 or iteration == 511:
                command(2, '\n'.join(records.values()))
                self.ae(pixels, graphics.render_vector_image(2, 32, 24))
                self.ae(graphics.image_for_client_id(1)['vector_elements'], len(records))
