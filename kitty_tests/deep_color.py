#!/usr/bin/env python
# License: GPLv3 Copyright: 2026, Adam DePrince

import math
import re

from kitty.config import load_config
from kitty.fast_data_types import simulate_os_window_output
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
        self.ae(defaults.deep_color, 'auto')
        self.assertTrue(defaults.output_dither)
        for mode in ('auto', 'always', 'never'):
            bad_lines = []
            opts = load_config(overrides=[f'deep_color {mode}', 'output_dither no'], accumulate_bad_lines=bad_lines)
            self.ae(bad_lines, [])
            self.ae(opts.deep_color, mode)
            self.assertFalse(opts.output_dither)
        bad_lines = []
        opts = load_config(overrides=['deep_color 10bit'], accumulate_bad_lines=bad_lines)
        self.ae(len(bad_lines), 1)
        self.ae(opts.deep_color, 'auto')

    def test_what_is_done_with_a_surface(self):
        self.set_options()
        cases = {
            # bits, is_float: linear, encode_in_shader, dither, levels, output_bits, output_type
            (8, 8, 8, False): (False, False, True, (255, 255, 255), 8, 'fixed'),
            (10, 10, 10, False): (False, True, False, (0, 0, 0), 10, 'fixed'),
            (16, 16, 16, True): (True, False, False, (0, 0, 0), 16, 'float'),
            (5, 6, 5, False): (False, False, True, (31, 63, 31), 0, 'fixed'),
            (0, 0, 0, False): (False, False, False, (0, 0, 0), 0, 'unknown'),
        }
        for (r, g, b, is_float), expected in cases.items():
            with self.subTest(bits=(r, g, b), is_float=is_float):
                ans = simulate_os_window_output(r, g, b, is_float)
                self.ae(
                    (ans['linear'], ans['encode_in_shader'], ans['dither'], ans['levels'], ans['output_bits'], ans['output_type']), expected)
                self.ae(ans['output_dither'], expected[2])
        self.set_options({'output_dither': False})
        ans = simulate_os_window_output(8, 8, 8, False)
        self.assertFalse(ans['dither'])
        self.assertFalse(ans['output_dither'])
        self.ae(ans['output_bits'], 8)

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
