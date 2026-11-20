# License: GPLv3 Copyright: 2026, Adam DePrince

import os
import tempfile
import zlib
from base64 import b64encode
from contextlib import suppress

from kitty.fast_data_types import shm_unlink, shm_write

from .base import BaseTest
from .graphics import parse_full_response, png_data, send_command


class TestSVG(BaseTest):

    def test_svg_placement_and_rendering(self):
        screen = self.create_screen()
        graphics = screen.grman
        svg = '''
            <svg xmlns="http://www.w3.org/2000/svg" viewBox="10 20 20 10">
              <style>.mark { fill: #ff0000; }</style>
              <defs><path id="dot" d="M 10 25 A 4 4 0 1 0 18 25 A 4 4 0 1 0 10 25 Z"/></defs>
              <g opacity=".5" transform="translate(3 0)">
                <use href="#dot" class="mark"/>
              </g>
            </svg>
        '''
        reply = parse_full_response(send_command(screen, 'a=T,f=201,i=9,c=4,C=1', svg))
        self.ae((reply.code, reply.image_id), ('OK', 9))
        image = graphics.image_for_client_id(9)
        self.ae((image['width'], image['height'], image['refs.count']), (20, 10, 1))

        def pixel(data, w, x, y):
            i = (w * y + x) * 4
            return tuple(data[i:i+4])

        self.ae(pixel(image['data'], 20, 7, 5), (255, 0, 0, 128))
        self.ae(pixel(image['data'], 20, 2, 5), (0, 0, 0, 0))
        full = graphics.render_vector_image(9, 200, 100)
        self.ae(pixel(full, 200, 70, 50), (255, 0, 0, 128))
        part = graphics.render_vector_view(9, 200, 100, 50, 40, 40, 15)
        self.ae(part, b''.join(full[(y*200+50)*4:(y*200+90)*4] for y in range(40, 55)))
        self.ae(parse_full_response(send_command(screen, 'a=e,i=9', 'x all')).code, 'EINVAL')
        self.ae(parse_full_response(send_command(screen, 'a=f,i=9,f=201', svg)).code, 'EINVAL')
        self.ae(graphics.render_vector_image(9, 200, 100), full)

    def test_svg_transports_and_sustained_replacement(self):
        screen = self.create_screen()
        graphics = screen.grman
        svg = b'<svg width="2" height="1"><rect width="2" height="1" fill="lime"/></svg>'
        large = svg.replace(b'<rect', b'<!--' + b'x' * 150_000 + b'--><rect')

        def chunks(command, data):
            for offset in range(0, len(data), 3072):
                end = offset + 3072
                more = int(end < len(data))
                response = send_command(screen, f'{command},m={more}' if offset == 0 else f'm={more}', data[offset:end])
                if more:
                    self.assertFalse(response)
            return parse_full_response(response)

        reply = chunks('a=q,f=201,i=7', large)
        self.ae((reply.code, reply.image_id, graphics.image_count), ('OK', 7, 0))
        for i in range(32):
            data = large if i % 2 == 0 else large.replace(b'lime', b'blue')
            expected = b'\x00\xff\x00\xff' if i % 2 == 0 else b'\x00\x00\xff\xff'
            if i % 4 < 2:
                reply = chunks('a=t,f=201,i=7', data)
            else:
                reply = parse_full_response(send_command(screen, 'a=t,f=201,i=7,o=z', zlib.compress(data)))
            self.ae((reply.code, graphics.image_count), ('OK', 1))
            self.ae(graphics.image_for_client_id(7)['data'], expected * 2)

        with tempfile.NamedTemporaryFile(prefix='tty-graphics-protocol-') as f:
            f.write(b'prefix' + svg + b'suffix')
            f.flush()
            for medium in ('f', 't'):
                command = f'a=t,f=201,i=7,t={medium},O=6,S={len(svg)},s=4,v=2'
                self.ae(parse_full_response(send_command(screen, command, f.name)).code, 'OK')
            self.assertFalse(os.path.exists(f.name))
        image = graphics.image_for_client_id(7)
        self.ae((image['width'], image['height'], image['data']), (4, 2, b'\x00\xff\x00\xff' * 8))

    def test_svg_shared_memory_transport(self):
        screen = self.create_screen()
        svg = b'<svg width="2" height="1"><rect width="2" height="1" fill="lime"/></svg>'
        name = f'kitty-svg-test-{os.getpid()}'
        shm_write(name, zlib.compress(svg))
        try:
            self.ae(parse_full_response(send_command(screen, 'a=t,f=201,i=7,t=s,o=z', name)).code, 'OK')
            self.assertRaises(FileNotFoundError, shm_unlink, name)
            self.ae(screen.grman.image_for_client_id(7)['data'], b'\x00\xff\x00\xff' * 2)
        finally:
            with suppress(FileNotFoundError):
                shm_unlink(name)

    def test_svg_invalid_payloads_and_resource_policy(self):
        screen = self.create_screen()
        graphics = screen.grman
        svg = '<svg width="4" height="2"/>'
        for payload, keys in (
            (b'', ''), (b'\xff', ''), ('<svg width="1">', ''), ('<html/>', ''),
            (svg, ',s=2'), (svg, ',t=g'), (svg, ',o=z'),
            ('<!DOCTYPE svg [<!ENTITY a "bad">]><svg>&a;</svg>', ''),
        ):
            with self.subTest(payload=payload, keys=keys):
                reply = parse_full_response(send_command(screen, 'a=q,f=201,i=7' + keys, payload))
                self.ae((reply.code, graphics.image_count), ('EINVAL', 0))
        with tempfile.NamedTemporaryFile(suffix='.png') as f:
            f.write(png_data)
            f.flush()
            source = f'<svg width="1" height="1"><image href="{f.name}" width="1" height="1"/></svg>'
            reply = parse_full_response(send_command(screen, 'a=t,f=201,i=7', source))
            self.ae(reply.code, 'OK')
        self.ae(graphics.image_for_client_id(7)['data'], bytes(4))
        source = source.replace(f.name, f'data:image/png;base64,{b64encode(png_data).decode("ascii")}')
        self.ae(parse_full_response(send_command(screen, 'a=t,f=201,i=7', source)).code, 'OK')
        self.assertGreater(graphics.image_for_client_id(7)['data'][3], 0)
