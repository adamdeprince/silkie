#!/usr/bin/env python
# License: GPLv3 Copyright: 2026, Adam DePrince

import math
import re

from kitty.config import load_config
from kitty.fast_data_types import simulate_os_window_output, simulate_output_depth_changes
from kitty.options.types import defaults
from kitty.shaders.slang import get_shader_src

from .base import BaseTest


def bayer_threshold(x: int, y: int) -> float:
    # Mirrors bayer_threshold() in kitty/shaders/dither.slang
    x &= 7
    y &= 7
    v = x ^ y
    index = ((v & 1) << 5) | ((x & 1) << 4) | ((v & 2) << 2) | ((x & 2) << 1) | ((v & 4) >> 1) | ((x & 4) >> 2)
    return (index + 0.5) / 64 - 0.5


def shader_round(v: float) -> float:
    # GLSL round() may go either way at exactly one half, which the tests never hit
    return math.floor(v + 0.5)


def dither(straight: float, alpha: float, x: int, y: int, levels: int, exact: float) -> int:
    # Mirrors dither_premultiplied() in kitty/shaders/dither.slang, for one channel, returning the step written
    v = straight * levels
    nearest = shader_round(v)
    d = v - nearest
    far = max(abs(d) - exact, 0.0)
    stretched = nearest + math.copysign(far, d) * (0.5 / (0.5 - exact))
    noise = bayer_threshold(x, y) if far >= 1e-6 else 0.0
    return int(min(max(math.floor(stretched * alpha + noise + 0.5), 0), levels))


class TestDeepColor(BaseTest):

    def setUp(self):
        super().setUp()
        src = get_shader_src('dither')
        m = re.search(r'static const float exact = ([0-9.]+)f;', src)
        self.assertIsNotNone(m, 'the exact threshold of dither.slang has moved, update this test')
        self.exact = float(m.group(1))

    def test_options(self):
        self.ae(defaults.deep_color, 'always')
        self.assertTrue(defaults.output_dither)
        for mode in ('always', 'never'):
            bad_lines = []
            opts = load_config(overrides=[f'deep_color {mode}', 'output_dither no'], accumulate_bad_lines=bad_lines)
            self.ae(bad_lines, [])
            self.ae(opts.deep_color, mode)
            self.assertFalse(opts.output_dither)
        for bad in ('auto', '10bit'):
            bad_lines = []
            opts = load_config(overrides=[f'deep_color {bad}'], accumulate_bad_lines=bad_lines)
            self.ae(len(bad_lines), 1)
            self.ae(opts.deep_color, 'always')

    def test_what_is_done_with_a_surface(self):
        self.set_options()
        cases = {
            # bits, is_float, display is deep: linear, encode_in_shader, needs_layers, levels, output_bits, output_type, output_dither
            (8, 8, 8, False, False): (False, False, True, (255, 255, 255), 8, 'fixed', 8),
            (8, 8, 8, False, True): (False, False, True, (255, 255, 255), 8, 'fixed', 8),
            # a deep surface on a display not known to be deep is dithered to the steps of 8 bits
            (10, 10, 10, False, False): (False, True, True, (255, 255, 255), 10, 'fixed', 8),
            (10, 10, 10, False, True): (False, True, True, (1023, 1023, 1023), 10, 'fixed', 10),
            (16, 16, 16, True, False): (True, False, True, (255, 255, 255), 16, 'float', 8),
            (16, 16, 16, True, True): (True, False, False, (0, 0, 0), 16, 'float', 0),
            (5, 6, 5, False, False): (False, False, True, (31, 63, 31), 0, 'fixed', 5),
            (0, 0, 0, False, False): (False, False, False, (0, 0, 0), 0, 'unknown', 0),
        }
        for (r, g, b, is_float, deep), expected in cases.items():
            with self.subTest(bits=(r, g, b), is_float=is_float, display_deep=deep):
                ans = simulate_os_window_output(r, g, b, is_float, deep)
                self.ae((ans['linear'], ans['encode_in_shader'], ans['needs_layers'], ans['levels'], ans['output_bits'], ans['output_type'],
                         ans['output_dither']), expected)
        self.set_options({'output_dither': False})
        for args in ((8, 8, 8, False, False), (10, 10, 10, False, False), (16, 16, 16, True, False)):
            ans = simulate_os_window_output(*args)
            self.assertFalse(ans['dither'])
            self.ae(ans['output_dither'], 0)
        self.ae(simulate_os_window_output(10, 10, 10, False)['needs_layers'], True)

    def summary(self, states):
        return [(s['display_deep'], s['output_type'], s['output_bits'], s['output_dither'], s['switched']) for s in states]

    def test_switchable_surface_follows_the_display(self):
        # macOS: floating point on a deep screen, 8 bits on another, once the move has lasted
        self.set_options({'deep_color': 'always', 'hdr': 'no', 'macos_colorspace': 'srgb'})
        ticks = [
            (0, True, False),     # opens on a deep screen: acted on at once
            (100, False, False),  # dragged across the boundary and back, too short to act on
            (200, True, False),
            (300, False, False),  # moved to an 8 bit screen
            (500, False, False),
            (650, False, False),  # has lasted long enough
            (700, True, False),   # and back
            (1100, True, False),
        ]
        float_deep = (True, 'float', 16, 0)
        fixed_8 = (False, 'fixed', 8, 8)
        expected = [
            float_deep + (True,), float_deep + (False,), float_deep + (False,), float_deep + (False,), float_deep + (False,),
            fixed_8 + (True,), fixed_8 + (False,), float_deep + (True,)]
        states = simulate_output_depth_changes(8, 8, 8, False, True, ticks)
        self.ae(self.summary(states), expected)
        # the layers follow the surface, floating point for a linear one
        self.ae([s['layers_float'] for s in states], [s['linear'] for s in states])
        # a window whose colors are not color managed cannot have a floating point surface
        self.set_options({'deep_color': 'always', 'hdr': 'no', 'macos_colorspace': 'default'})
        states = simulate_output_depth_changes(8, 8, 8, False, True, ticks)
        self.assertTrue(all(s['output_type'] == 'fixed' and not s['switched'] for s in states))

    def test_never_keeps_8_bits_across_screens(self):
        ticks = [(0, True, False), (400, False, False), (800, True, False), (1200, False, False), (1600, True, False)]
        self.set_options({'deep_color': 'never', 'hdr': 'no'})
        for start in ((8, 8, 8, False), (16, 16, 16, True)):
            states = simulate_output_depth_changes(*start, True, ticks)
            self.ae([(s['output_type'], s['output_bits']) for s in states], [('fixed', 8)] * len(ticks))
            self.assertTrue(all(s['output_dither'] == 8 for s in states))
        # HDR needs a floating point surface, which it gets while it is in use, on a screen that has HDR, even with never.
        # HDR stops as soon as the window is on a screen without it, so the surface follows at once; a move onto such a
        # screen is acted on once it has lasted
        self.set_options({'deep_color': 'never', 'hdr': 'detect'})
        ticks = [(0, True, True), (400, False, False), (800, False, False), (900, True, True), (1300, True, True)]
        states = simulate_output_depth_changes(8, 8, 8, False, True, ticks)
        self.ae([s['output_type'] for s in states], ['float', 'fixed', 'fixed', 'fixed', 'float'])

    def test_fixed_surface_changes_only_its_dithering(self):
        # Wayland and X11: a 10 bit surface stays, dithered to 8 bits until the display is known to show more
        self.set_options({'deep_color': 'always'})
        ticks = [(0, False, False), (100, True, False), (450, True, False), (500, False, False), (700, True, False), (1000, False, False),
                 (1400, False, False)]
        states = simulate_output_depth_changes(10, 10, 10, False, False, ticks)
        self.ae([(s['output_bits'], s['output_dither'], s['switched']) for s in states],
                [(10, 8, False), (10, 8, False), (10, 10, False), (10, 10, False), (10, 10, False), (10, 10, False), (10, 8, False)])

    def test_bayer_thresholds(self):
        thresholds = sorted(bayer_threshold(x, y) for x in range(8) for y in range(8))
        self.ae(thresholds, [(i + 0.5) / 64 - 0.5 for i in range(64)])
        self.ae(bayer_threshold(3, 5), bayer_threshold(11, 13))

    def test_dither_keeps_exact_colors(self):
        # A color the surface holds comes out exactly, even when it reaches the blit a little off, from every
        # position of the matrix, opaque or premultiplied with the alpha of a transparent window
        for levels in (255, 63, 31):
            for k in range(levels + 1):
                for error in (0, 0.24, -0.24, 0.18, -0.025):
                    straight = min(max((k + error) / levels, 0), 1)
                    for y in range(8):
                        for x in range(8):
                            self.ae(dither(straight, 1.0, x, y, levels, self.exact), k)
                for alpha in (0.9, 0.5, 0.123):
                    expected = shader_round(k * alpha)
                    for y in range(8):
                        for x in range(8):
                            self.ae(dither(k / levels, alpha, x, y, levels, self.exact), expected)

    def test_dither_follows_a_gradient(self):
        # Between steps the average of an 8x8 block follows the color within the exact window, never jumps, and
        # never strays further than one step from it
        levels = 255
        previous = None
        for i in range(0, 25501):
            v = 16 + i / 25500 * 16
            values = [dither(v / levels, 1.0, x, y, levels, self.exact) for y in range(8) for x in range(8)]
            self.assertTrue(all(abs(q - v) < 1 for q in values), (v, values))
            mean = sum(values) / 64
            self.assertLessEqual(abs(mean - v), self.exact + 1 / 64 + 1e-9, v)
            if previous is not None:
                self.assertGreaterEqual(mean, previous - 1e-9)
                self.assertLessEqual(mean - previous, 2 / 64 + 1e-9, v)
            previous = mean
