#!/usr/bin/env python
# License: GPLv3 Copyright: 2026, Adam DePrince

from unittest.mock import patch

from kitty.fast_data_types import Screen, current_fonts, test_render_line
from kitty.fonts import FontSpec, render
from kitty.fonts import styles as fs
from kitty.fonts.render import setup_for_testing
from kitty.options.types import defaults

from .base import BaseTest, draw_multicell, parse_bytes


def select(screen, fields='', text=''):
    parse_bytes(screen, f'\x1b]777;font;v=1:op=select:{fields}\x1b\\{text}'.encode())


class FontStyles(BaseTest):
    def tearDown(self):
        fs.configure(defaults)
        super().tearDown()

    def test_font_protocol(self):
        s = self.create_screen(cols=40)
        fs.configure(defaults)
        select(s, 'role=serif:weight=100:slant=1500:italic=1', 'abc')
        style = s.cursor.text_style
        self.ae(fs.styles[style], fs.TextStyle('serif', 100, 1500))
        self.assertTrue(s.line(0).cursor_from(0).italic)
        self.ae(s.line(0).cursor_from(0).text_style, style)
        self.ae(s.cursor.x, 3)
        parse_bytes(s, b'\x1b]777;font;v=1:op=state:id=9\x1b\\')
        self.assertIn(b'role=serif:weight=100:slant=1500:italic=1', s.callbacks.wtcbuf)
        parse_bytes(s, b'\x1b7\x1b[0m')
        self.ae(s.cursor.text_style, 0)
        parse_bytes(s, b'\x1b8')
        self.ae(s.cursor.text_style, style)
        select(s, 'slant=0', 'd')
        self.assertTrue(s.line(0).cursor_from(3).italic)
        self.ae(fs.styles[s.cursor.text_style].slant, 0)
        parse_bytes(s, b'\x1b]777;font;v=1:op=reset\x1b\\')
        self.ae(s.cursor.text_style, 0)
        self.assertTrue(s.cursor.italic)
        parse_bytes(s, b'\x1b[12m')
        self.ae(fs.styles[s.cursor.text_style].role, 'serif')
        parse_bytes(s, b'\x1b[10m')
        self.ae(fs.styles[s.cursor.text_style].role, 'default')
        select(s, 'role=unknown:weight=900')
        self.ae(fs.styles[s.cursor.text_style], fs.TextStyle('default', 900))

    def test_font_invalid_requests_are_atomic(self):
        s = self.create_screen()
        fs.configure(defaults)
        select(s, 'weight=300:italic=1')
        original = s.cursor.copy()
        for fields in ('weight=99', 'weight=901', 'weight=+100', 'weight=100x', 'weight=', 'slant=-1501', 'slant=1501',
                       'italic=2', 'italic=0:weight=1', 'weight=100:weight=200', 'extra=x:extra=y', 'role=a,b',
                       ':weight=100', 'id=-1', 'id=2147483648', 'id=+1'):
            select(s, fields)
            self.ae(s.cursor, original, fields)
        for body in (b'op=reset', b'v=2:op=reset', b'v=1:op=select:role=\xff', b'v=1:op=select:role=a\0b'):
            parse_bytes(s, b'\x1b]777;font;' + body + b'\x1b\\')
            self.ae(s.cursor, original, body)
        select(s, 'weight=100:future=' + 'x' * 10000)
        self.ae(fs.styles[s.cursor.text_style].weight, 100)

    def test_font_discovery_and_large_roles(self):
        s = self.create_screen()
        long_role = 'custom_' + 'r' * 4096
        roles = {**defaults.font_role, long_role: FontSpec(system='monospace')}
        fs.configure(defaults._replace(font_role=roles))
        parse_bytes(s, b'\x1b]777;font;v=1:op=query:id=42\x1b\\')
        self.assertIn(long_role.encode(), s.callbacks.wtcbuf)
        self.assertIn(b'weight_min=100:weight_max=900', s.callbacks.wtcbuf)
        select(s, f'role={long_role}:weight=200')
        self.ae(fs.styles[s.cursor.text_style].role, long_role)
        parse_bytes(s, b'\x1b]777;caps;v=1:op=query:id=7\x1b\\')
        self.assertIn(b'font.1', s.callbacks.wtcbuf)

    def test_font_scrollback_reflow_and_ansi(self):
        s = self.create_screen(cols=20, lines=3, scrollback=2200)
        fs.configure(defaults)
        ids = []
        for i in range(2048):
            select(s, f'weight={100 + i % 801}:slant={i - 1024}', 'x\r\n')
            ids.append(s.line(s.cursor.y - 1).cursor_from(0).text_style if s.cursor.y else 0)
        self.ae(len(set(ids)), 2048)
        self.assertGreater(s.historybuf.count, 2000)
        first = s.historybuf.line(s.historybuf.count - 1)
        first_style = first.cursor_from(0).text_style
        self.ae(fs.styles[first_style], fs.TextStyle('default', 100, -1024))
        ansi = first.as_ansi()
        restored = self.create_screen(cols=20)
        parse_bytes(restored, ansi.encode())
        self.ae(restored.line(0).cursor_from(0).text_style, first_style)
        s.resize(3, 10)
        self.ae(s.historybuf.line(s.historybuf.count - 1).cursor_from(0).text_style, first_style)
        fs.configure(defaults._replace(font_family=FontSpec(system='monospace')))
        self.ae(fs.styles[first_style], fs.TextStyle('default', 100, -1024))

    def test_font_multicells_and_alternate_screen(self):
        s = self.create_screen(cols=40, lines=5)
        fs.configure(defaults)
        select(s, 'weight=200:slant=-900')
        style = s.cursor.text_style
        draw_multicell(s, '漢', scale=2)
        for y in (0, 1):
            for x in range(4):
                self.ae(s.line(y).cursor_from(x).text_style, style)
        parse_bytes(s, b'\x1b[?1049h\x1b[0m\x1b[?1049l')
        self.ae(s.cursor.text_style, style)
        self.ae(s.line(0).cursor_from(0).text_style, style)

    def test_font_weight_and_synthetic_slant_selection(self):
        descriptors = [dict(family='Test', style=style, postscript_name=style, path=style)
                       for style in ('Thin', 'Light', 'Regular', 'Heavy', 'Light Italic')]
        weights = {'Thin': 100, 'Light': 300, 'Regular': 400, 'Heavy': 900, 'Light Italic': 300}
        with patch.object(fs, 'get_font_files', return_value={x: descriptors[2] for x in ('medium', 'bold', 'italic', 'bi')}), \
                patch.object(fs, 'all_fonts_map', return_value={'family_map': {'test': descriptors}}), \
                patch.object(fs, 'get_variable_data_for_descriptor', return_value={'axes': ()}), \
                patch.object(fs, 'native_metrics', side_effect=lambda d: (weights[d['style']], -9.4 if 'Italic' in d['style'] else 0)):
            fs.configure(defaults)
            for weight, expected in ((100, 'Thin'), (180, 'Thin'), (300, 'Light'), (900, 'Heavy')):
                idx = fs.descriptor_index(fs.intern(fs.TextStyle(weight=weight)), False, False, False)
                self.ae(render.current_faces[idx][0]['style'], expected)
            idx = fs.descriptor_index(fs.intern(fs.TextStyle(weight=300, slant=0)), False, True, False)
            descriptor = render.current_faces[idx][0]
            self.ae(descriptor['style'], 'Light Italic')
            self.assertLess(descriptor['matrix'][1], 0)
            for slant in (-1000, 1000):
                idx = fs.descriptor_index(fs.intern(fs.TextStyle(slant=slant)), False, False, False)
                self.assertEqual(render.current_faces[idx][0]['matrix'][1] > 0, slant < 0)

    def test_cjk_regional_preferences(self):
        for region, family in (('jp', 'Sarasa Term J'), ('kr', 'Sarasa Term K'), ('tc', 'Noto Sans CJK TC'), ('hk', 'Sarasa Term HC')):
            fs.configure(defaults._replace(cjk_font_locale=region))
            with patch.object(fs, 'all_fonts_map', return_value={'family_map': {family.lower(): []}}):
                self.ae(fs.cjk_spec(), FontSpec(family=family))
        fs.configure(defaults._replace(cjk_font=FontSpec(family='Custom CJK')))
        self.ae(fs.cjk_spec(), FontSpec(family='Custom CJK'))

    def test_font_rendering_many_styles(self):
        with setup_for_testing('monospace', size=16) as (sprites, cw, ch):
            s = Screen(None, 1, 3)
            images = []
            for angle in range(-1500, 1501, 10):
                s.reset()
                select(s, f'slant={angle}', 'Hm')
                test_render_line(s.line(0))
                images.append(tuple(s.line(0).sprite_at(i) for i in (0, 1)))
                self.ae(s.cursor.x, 2)
            self.assertGreater(len(set(images)), 100)
            self.assertGreater(len(current_fonts()['fallback']), 100)
            self.assertTrue(sprites)

    def test_cjk_rendering_preserves_grid(self):
        with setup_for_testing('monospace', size=16):
            if not any(fs.cjk_spec(kind) for kind in (1, 2, 3)):
                self.skipTest('No preferred CJK font installed')
            s = Screen(None, 1, 20)
            for weight in (100, 300, 700, 900):
                s.reset()
                select(s, f'weight={weight}:slant=1200', 'A漢かな한B')
                test_render_line(s.line(0))
                self.ae(s.cursor.x, 10)
                self.ae(str(s.line(0)), 'A漢かな한B')
                for column in (1, 3, 5, 7):
                    self.assertGreater(s.line(0).sprite_at(column), 5)
            self.assertTrue(current_fonts()['fallback'])
