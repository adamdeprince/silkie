#!/usr/bin/env python
# License: GPLv3 Copyright: 2026 Adam DePrince
import base64
import json
import math
import os
import struct
import tempfile
import zlib

from kittens.tui.vector import clip_group, font_upload, measure_text, parse_service_response, scene, service_request, text_record
from kitty.fonts.vector import bounds

from .base import BaseTest, parse_bytes
from .graphics import parse_full_response, send_command


def encoded(obj):
    return base64.b64encode(json.dumps(obj, ensure_ascii=False).encode()).decode()


class VectorText(BaseTest):

    def transmit(self, screen, command, data):
        if isinstance(data, str):
            data = data.encode()
        for offset in range(0, len(data), 3072):
            more = offset + 3072 < len(data)
            response = send_command(screen, (command + ',' if offset == 0 else '') + f'm={int(more)}', data[offset:offset+3072])
            if more:
                self.assertFalse(response)
        return response

    def service(self, screen, obj, **kw):
        data = json.dumps(obj, ensure_ascii=False).encode()
        if kw.get('o') == 'z':
            data = zlib.compress(data)
        command = 'a=v,f=200,i=77' + ''.join(f',{k}={v}' for k, v in kw.items())
        response = self.transmit(screen, command, data)
        self.assertTrue(response, command)
        body = response.partition(b';')[2].partition(b'\x1b')[0]
        if not body.startswith(b'OK;'):
            self.fail(body.decode())
        return json.loads(base64.b64decode(body[3:]))

    def upload(self, screen, name='test'):
        with open(os.path.join(os.path.dirname(__file__), 'iosevka-regular.ttf'), 'rb') as f:
            raw = f.read()
        return self.service(screen, {'op': 'upload', 'id': name, 'data': base64.b64encode(raw).decode()}, o='z')

    def scene(self, screen, doc, image=1, width=120, height=80, action='T'):
        return parse_full_response(self.transmit(screen, f'a={action},f=200,i={image},s={width},v={height}', doc))

    def pixels(self, screen, image=1, width=120, height=80):
        return screen.grman.render_vector_linear(image, width, height)

    def test_vector_font_resources(self):
        s = self.create_screen()
        listing = self.service(s, {'op': 'fonts'})
        self.assertTrue(listing['fonts'])
        self.assertIn('serif', listing['roles'])
        info = self.upload(s)
        self.assertGreater(info['glyphs'], 0)
        self.ae(self.service(s, {'op': 'inspect', 'font': '@test'})['upem'], info['upem'])
        # The session spans the main and alternate screen, but not another PTY.
        parse_bytes(s, b'\x1b[?1049h')
        self.assertGreater(self.service(s, {'op': 'measure', 'font': '@test', 'text': 'Hello'})['width'], 0)
        parse_bytes(s, b'\x1b[?1049l')
        other = self.create_screen()
        self.ae(self.service(other, {'op': 'fonts'})['session'], [])
        raw = send_command(s, 'a=v,f=200,i=81', b'{"op":"upload","id":"test","data":"AA=="}')
        self.assertIn(b'EINVAL:', raw)
        self.assertGreater(self.service(s, {'op': 'measure', 'font': '@test', 'text': 'still valid'})['width'], 0)

    def test_vector_font_service_transport(self):
        s = self.create_screen()
        self.upload(s)
        obj = {'op': 'measure', 'font': '@test', 'text': 'Text with spaces\n\u03b1\u03b2'}
        expected = self.service(s, obj)
        data = zlib.compress(json.dumps(obj).encode())
        self.assertFalse(send_command(s, 'a=v,f=200,i=77,o=z,m=1', data[:12]))
        raw = send_command(s, 'm=0', data[12:])
        self.assertTrue(raw.startswith(b'\x1b_Gi=77;OK;'), raw)
        self.ae(json.loads(base64.b64decode(raw.partition(b'OK;')[2].partition(b'\x1b')[0])), expected)
        with tempfile.NamedTemporaryFile() as f:
            f.write(json.dumps(obj).encode())
            f.flush()
            raw = send_command(s, 'a=v,f=200,i=77,t=f', f.name)
            self.assertIn(b'OK;', raw)

    def test_vector_text_measure_and_render(self):
        s = self.create_screen()
        self.upload(s)
        obj = {'font': '@test', 'size': 22, 'x': 8, 'y': 28, 'fill': '#ff8800*250', 'text': 'Hello'}
        m = self.service(s, {'op': 'measure', **obj})
        self.assertGreater(m['width'], 30)
        self.ae(m['missing'], [])
        self.ae(self.scene(s, 't id=1 text=' + encoded(obj)).code, 'OK')
        px = list(struct.iter_unpack('ffff', self.pixels(s)))
        painted = [(i % 120, i // 120, p) for i, p in enumerate(px) if p[3] > .001]
        self.assertTrue(painted)
        self.assertGreater(max(p[0] for _, _, p in painted), 2)
        b = m['ink']
        for x, y, _ in painted:
            self.assertTrue(b[0]-1 <= x <= b[2]+1 and b[1]-1 <= y <= b[3]+1)
        # Releasing a font removes the handle; retained outlines remain usable.
        before = self.pixels(s)
        self.service(s, {'op': 'release', 'id': 'test'})
        self.ae(self.pixels(s), before)

    def test_vector_rich_text_and_atomic_failure(self):
        s = self.create_screen()
        self.upload(s)
        obj = {'font': '@test', 'size': 18, 'x': 4, 'y': 24, 'runs': [
            {'text': 'abc', 'fill': 'p3:1/0/0', 'underline': 'double'},
            {'text': 'def', 'weight': 100, 'slant': 12, 'fill': 'linear:0/2/0', 'strike': True},
            {'text': 'ghi', 'italic': True, 'slant': 0, 'fill': '#0000ff', 'underline': 'curly'}]}
        self.ae(self.scene(s, 't id=1 text=' + encoded(obj)).code, 'OK')
        m = self.service(s, {'op': 'measure', **obj})
        self.assertGreater(m['width'], 60)
        before = self.pixels(s)
        bad = 'p id=10 d=M0,0L20,0L20,20Z\nt id=2 text=' + encoded({'font': '@missing', 'text': 'bad'})
        self.ae(self.scene(s, bad, action='e').code, 'EINVAL')
        self.ae(self.pixels(s), before)
        rotated = {**obj, 'angle': 90, 'x': 60, 'y': 0}
        mr = self.service(s, {'op': 'measure', **rotated})
        self.assertGreater(mr['ink'][3]-mr['ink'][1], mr['ink'][2]-mr['ink'][0])
        self.ae(self.scene(s, 't id=1 text=' + encoded(rotated), action='e').code, 'OK')
        self.assertNotEqual(self.pixels(s), before)

    def test_vector_nested_clipping_boxes(self):
        s = self.create_screen()
        self.upload(s)
        text = encoded({'font': '@test', 'text': 'A long window title', 'size': 30, 'fill': '#ffffff*400', 'y': 28})
        doc = ('g id=1 box=10,10,40,30\ng id=2 p=1 box=0,0,100,20 t=1,0,0,1,20,5\n'
               'p id=3 g=2 d=M-100,-100L100,-100L100,100L-100,100Z fill=linear:2/1/0\n'
               f't id=4 g=2 text={text}\n')
        self.ae(self.scene(s, doc).code, 'OK')
        data = list(struct.iter_unpack('ffff', self.pixels(s)))
        for i, p in enumerate(data):
            x, y = i % 120, i // 120
            inside = 20 <= x < 50 and 10 <= y < 25
            self.ae(p[3] > 0, inside)
        self.ae(self.scene(s, 'g id=2 p=1 box=0,0,30,30 t=0,1,-1,0,45,0', action='e').code, 'OK')
        data = list(struct.iter_unpack('ffff', self.pixels(s)))
        for i, p in enumerate(data):
            x, y = i % 120, i // 120
            self.ae(p[3] > 0, 15 <= x < 45 and 10 <= y < 30)
        # Root clips constrain even descendants transformed out of their window.
        self.ae(self.scene(s, 'g id=0 box=25,15,10,5', action='e').code, 'OK')
        data = list(struct.iter_unpack('ffff', self.pixels(s)))
        self.ae(sum(p[3] > 0 for p in data), 50)
        self.ae(self.scene(s, 'g id=2 p=1 box=0,0,0,20', action='e').code, 'OK')
        self.assertFalse(any(p[3] for p in struct.iter_unpack('ffff', self.pixels(s))))

    def test_vector_deep_clipping_boxes(self):
        s = self.create_screen()
        groups = '\n'.join(f'g id={i} p={i-1} box=1,1,8,8' for i in range(1, 1025))
        doc = groups + '\np id=2000 g=1024 d=M-100,-100L100,-100L100,100L-100,100Z fill=#ffffff*300'
        self.ae(self.scene(s, doc, width=10, height=10).code, 'OK')
        data = list(struct.iter_unpack('ffff', self.pixels(s, width=10, height=10)))
        self.ae(sum(p[3] > 0 for p in data), 64)
        self.assertAlmostEqual(data[55][0], 3, places=5)
        self.ae(self.scene(s, 'x g=1', action='e', width=10, height=10).code, 'OK')
        self.ae(s.grman.image_for_client_id(1)['vector_elements'], 0)

    def test_vector_hdr_all_primitives(self):
        s = self.create_screen()
        raster = struct.pack('eeee', 3, -.2, .25, 1)
        self.ae(parse_full_response(send_command(s, 'a=t,f=64,i=9,s=1,v=1', raster)).code, 'OK')
        doc = ('g id=1 o=50 box=0,0,100,80\n'
               'l id=10 x1=0 y1=0 x2=20 y2=0 stops=0,linear:2/-0.1/0,1,linear:4/-0.1/0\n'
               'p id=2 g=1 d=M0,0L20,0L20,20L0,20Z fill=@10\n'
               'k id=3 pts=30,10,10,40,10,10 fill=#ffffff*300\n'
               's id=4 d=M0,0L10,0L10,10L0,10Z\nu id=5 s=4 t=1,0,0,1,50,0 fill=p3:1/0/0*200\n'
               'i id=6 img=9 t=10,0,0,10,70,0\n')
        self.ae(self.scene(s, doc).code, 'OK')
        data = list(struct.iter_unpack('ffff', self.pixels(s)))
        self.assertGreater(data[5*120+5][0], 2)
        self.assertLess(data[5*120+5][1], 0)
        self.assertAlmostEqual(data[5*120+5][3], .5)
        self.assertGreater(data[10*120+35][0], 2.9)
        self.assertGreater(data[5*120+55][0], 2)
        self.assertLess(data[5*120+55][1], 0)
        self.assertAlmostEqual(data[5*120+75][0], 3, places=5)
        self.assertLess(data[5*120+75][1], -.19)
        pixels = bytearray()
        s.grman.update_vector_pixels(1, 120, 80, pixels)
        self.ae(parse_full_response(send_command(s, 'a=t,f=64,i=9,s=1,v=1', struct.pack('eeee', .1, 4, .2, 1))).code, 'OK')
        self.assertIsNotNone(s.grman.update_vector_pixels(1, 120, 80, pixels))
        data = list(struct.iter_unpack('ffff', self.pixels(s)))
        self.assertGreater(data[5*120+75][1], 3.9)
        send_command(s, 'a=d,d=I,i=9')
        self.assertIsNotNone(s.grman.update_vector_pixels(1, 120, 80, pixels))
        data = list(struct.iter_unpack('ffff', self.pixels(s)))
        self.ae(data[5*120+75][3], 0)

    def test_vector_sustained_text_and_fonts(self):
        s = self.create_screen()
        self.upload(s)
        # Long text and many style runs cross former graphics response/field sizes.
        obj = {'op': 'measure', 'font': '@test', 'text': 'x' * 8192, 'size': 10}
        m = self.service(s, obj)
        self.assertGreater(m['width'], 40000)
        doc = '\n'.join('t id=%d text=%s' % (i+1, encoded({
            'font': '@test', 'text': 'x', 'size': 8, 'x': i % 100, 'y': 20, 'slant': (i % 31)-15})) for i in range(300))
        self.ae(self.scene(s, doc).code, 'OK')
        self.ae(s.grman.image_for_client_id(1)['vector_elements'], 300)
        for i in range(40):
            self.ae(self.scene(s, 'x id=%d' % (i+1), action='e').code, 'OK')
        self.ae(s.grman.image_for_client_id(1)['vector_elements'], 260)
        self.assertTrue(any(self.pixels(s)))

    def test_vector_curve_bounds(self):
        b = bounds([(0, 0, 0), (2, 10, 20, 20, 0)])
        self.ae(b, [0, 0, 20, 10])
        b = bounds([(0, 0, 0), (3, 0, 12, 12, 12, 12, 0)])
        self.assertTrue(math.isclose(b[3], 9))

    def test_vector_cjk_fallback_and_layout_variants(self):
        s = self.create_screen()
        self.upload(s)
        for text in ('A\u4e2d\u6587', '\u304b\u306a', '\ud55c\uae00', 'a\u0301'):
            m = self.service(s, {'op': 'measure', 'font': '@test', 'text': text})
            self.ae(m['missing'], [], text)
            self.assertGreater(m['width'], 0)
        for direction in ('ltr', 'rtl', 'ttb', 'btt'):
            m = self.service(s, {'op': 'measure', 'font': '@test', 'text': 'abc', 'direction': direction})
            self.assertGreater(m['height'], 0)
        self.assertGreater(self.service(s, {'op': 'measure', 'font': '@test', 'text': ''})['height'], 0)
        bad = send_command(s, 'a=v,f=200,i=80', json.dumps({'op': 'measure', 'font': '@test', 'text': 'x', 'fill': 'bad'}))
        self.assertIn(b'EINVAL:', bad)

    def test_vector_client_helpers(self):
        s = self.create_screen()
        with open(os.path.join(os.path.dirname(__file__), 'iosevka-regular.ttf'), 'rb') as f:
            frames = font_upload(900, 'client', f.read())
        for frame in frames:
            self.assertLessEqual(len(frame.partition(b';')[2][:-2]), 4096)
            s.callbacks.clear()
            parse_bytes(s, frame)
        request, info = parse_service_response(s.callbacks.wtcbuf)
        self.ae(request, 900)
        self.ae(info['id'], 'client')
        layout = {'font': '@client', 'text': 'Unicode: \u03bb & spaces', 'y': 20, 'fill': '#ffffff*200'}
        for frame in measure_text(901, layout):
            s.callbacks.clear()
            parse_bytes(s, frame)
        self.assertGreater(parse_service_response(s.callbacks.wtcbuf)[1]['width'], 20)
        for frame in scene(10, 120, 80, [clip_group(1, [0, 0, 60, 40]), clip_group(2, [4, 4, 40, 30], 1), text_record(3, layout, 2)]):
            s.callbacks.clear()
            parse_bytes(s, frame)
        self.ae(s.grman.image_for_client_id(10)['vector_elements'], 3)
        for frame in service_request(902, {'op': 'release', 'id': 'missing'}):
            s.callbacks.clear()
            parse_bytes(s, frame)
        with self.assertRaises(ValueError):
            parse_service_response(s.callbacks.wtcbuf)

    def test_vector_variable_font_and_resource_accumulation(self):
        s = self.create_screen()
        # A small original test font: fixed 600-unit advances, rectangle A,
        # interpolating its stroke width over wght=100..900, default 400.
        with open(os.path.join(os.path.dirname(__file__), 'vector-variable.ttf'), 'rb') as f:
            data = base64.b64encode(f.read()).decode()
        info = self.service(s, {'op': 'upload', 'id': 'variable', 'data': data})
        self.ae(info['axes'][0]['tag'], 'wght')
        self.ae((info['axes'][0]['minimum'], info['axes'][0]['maximum']), (100, 900))
        widths = []
        for weight in (100, 400, 900):
            m = self.service(s, {'op': 'measure', 'font': '@variable', 'text': 'A', 'weight': weight, 'size': 20})
            widths.append(m['ink'][2]-m['ink'][0])
            self.assertAlmostEqual(m['width'], 12)
        self.assertTrue(widths[0] < widths[1] < widths[2], widths)
        explicit = self.service(s, {'op': 'measure', 'font': '@variable', 'text': 'A', 'axes': {'wght': 900}, 'size': 20})
        self.assertAlmostEqual(explicit['ink'][2]-explicit['ink'][0], widths[2])
        for i in range(300):
            self.service(s, {'op': 'upload', 'id': f'resource-{i}', 'data': data})
        self.ae(len(self.service(s, {'op': 'fonts'})['session']), 301)
        for i in range(300):
            self.service(s, {'op': 'release', 'id': f'resource-{i}'})
        self.ae(len(self.service(s, {'op': 'fonts'})['session']), 1)
