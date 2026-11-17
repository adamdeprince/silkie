#!/usr/bin/env python
# License: GPLv3 Copyright: 2026, Adam DePrince

import math
import os
import tempfile
from types import SimpleNamespace
from unittest.mock import patch

from kitty.config import load_config
from kitty.fast_data_types import contrast_adjustment_for
from kitty.options.utils import minimum_contrast, text_fg_override_threshold
from kitty.profiles import Profile, clear_caches, parsed_options
from kitty.session import parse_session
from kitty.tabs import Tab
from kitty.window import Window

from .base import BaseTest

# FG_OVERRIDE_* in cell.slang and state.h
NONE, LUMINANCE, RATIO, LIGHTNESS = range(4)

# A reference of the color math in cell.slang, for a window drawn in sRGB. {{{
# Colors are linear sRGB, as the cell shader has them.

TO_XYZ = ((0.4124564, 0.3575761, 0.1804375), (0.2126729, 0.7151522, 0.0721750), (0.0193339, 0.1191920, 0.9503041))
FROM_XYZ = ((3.2404542, -1.5371385, -0.4985314), (-0.9692660, 1.8760108, 0.0415560), (0.0556434, -0.2040259, 1.0572252))
# The sRGB to XYZ matrix of HSLuv, its chroma bounds come from it
HSLUV_M = (
    (3.2409699419045214, -1.5373831775700935, -0.49861076029300328),
    (-0.96924363628087983, 1.8759675015077207, 0.041555057407175613),
    (0.055630079696993609, -0.20397695888897657, 1.0569715142428786),
)
REF_U, REF_V = 0.19783000664283681, 0.468319994938791
KAPPA, EPSILON = 903.2962962962963, 0.0088564516790356308


def mul(m, v):
    return tuple(sum(m[i][j] * v[j] for j in range(3)) for i in range(3))


def decode(c):
    return c / 12.92 if c <= 0.04045 else ((c + 0.055) / 1.055) ** 2.4


def encode(c):
    return c * 12.92 if c <= 0.0031308 else 1.055 * max(c, 0) ** (1 / 2.4) - 0.055


def from_hex(h):
    return tuple(decode(int(h[i:i + 2], 16) / 255) for i in (0, 2, 4))


def to_hex(c):
    return '#' + ''.join(f'{round(min(max(encode(x), 0), 1) * 255):02x}' for x in c)


def luminance(c):
    return mul(TO_XYZ, c)[1]


def lightness(y):
    return y * KAPPA if y <= EPSILON else 116 * y ** (1 / 3) - 16


def lightness_to_y(L):
    return L / KAPPA if L <= 8 else ((L + 16) / 116) ** 3


def max_chroma(L, H):
    sub1 = (L + 16) ** 3 / 1560896
    sub2 = sub1 if sub1 > EPSILON else L / KAPPA
    hrad, ans = math.radians(H), math.inf
    for m1, m2, m3 in HSLUV_M:
        for t in (0, 1):
            top1 = (284517 * m1 - 94839 * m3) * sub2
            top2 = (838422 * m3 + 769860 * m2 + 731718 * m1) * L * sub2 - 769860 * t * L
            bottom = (632260 * m3 - 126452 * m2) * sub2 + 126452 * t
            d = math.sin(hrad) - top1 / bottom * math.cos(hrad)
            if d:
                length = (top2 / bottom) / d
                if length >= 0:
                    ans = min(ans, length)
    return ans


def to_hsluv(c):
    X, Y, Z = mul(TO_XYZ, c)
    L = lightness(Y)
    if L < 1e-4 or L > 99.9999:
        return 0., 0., min(max(L, 0), 100)
    d = X + 15 * Y + 3 * Z
    u, v = 13 * L * (4 * X / d - REF_U), 13 * L * (9 * Y / d - REF_V)
    H = math.degrees(math.atan2(v, u)) % 360
    return H, math.hypot(u, v) / max_chroma(L, H) * 100, L


def from_hsluv(H, S, L):
    L = min(max(L, 0), 100)
    if L < 1e-4 or L > 99.9999:
        y = lightness_to_y(L)
        return y, y, y
    C = max_chroma(L, H) / 100 * S
    u, v = math.cos(math.radians(H)) * C, math.sin(math.radians(H)) * C
    U, V = u / (13 * L) + REF_U, v / (13 * L) + REF_V
    Y = lightness_to_y(L)
    X = 2.25 * U * Y / V
    Z = (3 / V - 5) * Y - X / 3
    return tuple(min(max(x, 0), 1) for x in mul(FROM_XYZ, (X, Y, Z)))


def with_lightness(c, L):
    H, S, _ = to_hsluv(c)
    return from_hsluv(H, S, L)


def adjust_lightness(fg, bg, percent, fg_gain=1., bg_gain=1.):
    ' fg_override_lightness() of cell.slang: minimum_contrast '
    o, u = tuple(x / fg_gain for x in fg), tuple(x / bg_gain for x in bg)
    lb, lf = lightness(luminance(u)), lightness(luminance(o))
    if abs(lf - lb) >= percent:
        return fg
    room_above, room_below = 100 - lb, lb
    above = lf > lb or (lf == lb and room_above >= room_below)
    own, other = (room_above, room_below) if above else (room_below, room_above)
    if own < percent and other > own:
        above = not above
    target = min(lb + percent, 100) if above else max(lb - percent, 0)
    return tuple(x * fg_gain for x in with_lightness(o, target))


def contrast_ratio(a, b):
    return min(max((max(a, b) + 0.05) / (min(a, b) + 0.05), 1), 21)


def adjust_ratio(fg, bg, ratio):
    ' fg_override_contrast() of cell.slang: text_fg_override_threshold in ratio mode '
    ul = luminance(bg)
    if contrast_ratio(ul, luminance(fg)) >= ratio or sum(abs(a - b) for a, b in zip(fg, bg)) <= 0.001:
        return fg
    a = with_lightness(fg, lightness(min(max((ul + 0.05) * ratio - 0.05, 0), 1)))
    b = with_lightness(fg, lightness(min(max((ul + 0.05) / ratio - 0.05, 0), 1)))
    return b if contrast_ratio(ul, luminance(a)) <= contrast_ratio(ul, luminance(b)) else a
# }}}


BLACK, WHITE = (0., 0., 0.), (1., 1., 1.)
# The dark blue of ls over ssh, the problem minimum_contrast is for
SSH_BLUE = from_hex('0000ee')


class TestMinimumContrast(BaseTest):

    def assert_color(self, a, b, places=4):
        for x, y in zip(a, b):
            self.assertAlmostEqual(x, y, places=places, msg=f'{a} != {b}')

    def test_option_parsing(self):
        for raw, val in (('0', 0), ('40', 40), ('40%', 40), (' 40 % ', 40), ('100', 100), ('12.5', 12.5), ('0%', 0)):
            self.ae(minimum_contrast(raw), val)
        for raw in ('101', '-1', 'nan', 'inf', 'abc', '', '4.5 ratio', '0.3 %%'):
            with self.assertRaises(ValueError, msg=raw):
                minimum_contrast(raw)
        # the kitty option keeps its own syntax
        self.ae(text_fg_override_threshold('4.5 ratio'), (4.5, 'ratio'))
        self.ae(text_fg_override_threshold('10 %'), (10, '%'))
        self.ae(text_fg_override_threshold('10'), (10, '%'))
        with tempfile.TemporaryDirectory() as tdir:
            conf = os.path.join(tdir, 'silkie.conf')
            with open(conf, 'w') as f:
                f.write('minimum_contrast 40%\ntext_fg_override_threshold 4.5 ratio\n')
            opts = load_config(conf)
            self.ae(opts.minimum_contrast, 40)
            self.ae(opts.text_fg_override_threshold, (4.5, 'ratio'))
            self.ae(load_config().minimum_contrast, 0)

    def test_renderer_precedence_with_kitty_option(self):
        s = self.create_screen()
        self.ae(contrast_adjustment_for(s), (0, NONE))
        s = self.create_screen(options={'text_fg_override_threshold': (4.5, 'ratio')})
        self.ae(contrast_adjustment_for(s), (4.5, RATIO))
        s = self.create_screen(options={'text_fg_override_threshold': (10, '%')})
        t, algo = contrast_adjustment_for(s)
        self.ae(algo, LUMINANCE)
        self.assertAlmostEqual(t, 0.1, places=6)
        # minimum_contrast takes precedence over the kitty option
        s = self.create_screen(options={'text_fg_override_threshold': (4.5, 'ratio'), 'minimum_contrast': 40})
        t, algo = contrast_adjustment_for(s)
        self.ae(algo, LIGHTNESS)
        self.assertAlmostEqual(t, 0.4, places=6)
        # set for the window
        s.minimum_contrast = 70
        self.assertAlmostEqual(contrast_adjustment_for(s)[0], 0.7, places=6)
        self.ae(s.minimum_contrast, 70)
        # an explicit 0 turns contrast adjustment off, the kitty option too
        s.minimum_contrast = 0
        self.ae(contrast_adjustment_for(s), (0, NONE))
        s.minimum_contrast = None
        self.ae(s.minimum_contrast, None)
        self.ae(contrast_adjustment_for(s)[1], LIGHTNESS)
        s = self.create_screen(options={'text_fg_override_threshold': (4.5, 'ratio')})
        s.minimum_contrast = 0
        self.ae(contrast_adjustment_for(s), (0, NONE))
        s.minimum_contrast = None
        self.ae(contrast_adjustment_for(s), (4.5, RATIO))
        for bad in (-1, 101, math.nan):
            with self.assertRaises(ValueError):
                s.minimum_contrast = bad

    def make_tab(self, profile=None, windows=()):
        tab = Tab.__new__(Tab)
        tab.minimum_contrast_override = None
        tab.windows = list(windows)
        manager = SimpleNamespace(profile=profile or Profile('default', 'Default'))
        tab.tab_manager_ref = lambda: manager
        return tab

    def make_window(self, tab):
        w = Window.__new__(Window)
        w.screen = self.create_screen(options=self.options)
        w.minimum_contrast_override = None
        w.tabref = lambda: tab
        tab.windows.append(w)
        w.update_minimum_contrast()
        return w

    def test_per_window_precedence(self):
        # global < profile < tab < window (launch, remote control, Window.set_minimum_contrast)
        self.options = {'minimum_contrast': 20}
        clear_caches()
        self.addCleanup(clear_caches)
        profile = Profile('work', 'Work', options={'minimum_contrast': ['35%']})
        plain_tab, profile_tab = self.make_tab(), self.make_tab(profile)
        a, b = self.make_window(plain_tab), self.make_window(profile_tab)

        def check(w, effective, sent):
            self.ae(w.minimum_contrast, effective)
            self.ae(w.screen.minimum_contrast, sent)

        with patch('kitty.window.get_options') as go, patch('kitty.tabs.get_options') as go2:
            go.return_value = go2.return_value = SimpleNamespace(minimum_contrast=20)
            self.ae(parsed_options(profile)['minimum_contrast'], 35)
            check(a, 20, None)
            check(b, 35, 35)
            self.ae(profile_tab.minimum_contrast, 35)
            # the tab wins over its profile, for its windows and new ones
            profile_tab.set_minimum_contrast('50%')
            check(b, 50, 50)
            c = self.make_window(profile_tab)
            check(c, 50, 50)
            # the window wins over its tab
            c.set_minimum_contrast(80)
            check(c, 80, 80)
            check(b, 50, 50)
            # setting the tab again applies to all its windows
            profile_tab.set_minimum_contrast(10)
            check(c, 10, 10)
            self.ae(c.minimum_contrast_override, None)
            # removing the settings goes back to the profile, then the option
            c.set_minimum_contrast(0)
            check(c, 0, 0)
            c.set_minimum_contrast(None)
            check(c, 10, 10)
            profile_tab.set_minimum_contrast(None)
            check(c, 35, 35)
            # moving a window to a tab with another profile
            c.tabref = lambda: plain_tab
            c.update_minimum_contrast()
            check(c, 20, None)
            with self.assertRaises(ValueError):
                c.set_minimum_contrast(101)
            with self.assertRaises(ValueError):
                profile_tab.set_minimum_contrast('loud')

    def test_session_and_launch(self):
        from kitty.launch import clone_safe_opts, parse_launch_args

        opts = self.set_options()
        sessions = list(parse_session('new_tab one\nminimum_contrast 45%\nlaunch --minimum-contrast 60 sh\nnew_tab two\nlaunch sh\n', opts))
        tabs = sessions[0].tabs
        self.ae((tabs[0].minimum_contrast, tabs[1].minimum_contrast), (45, None))
        self.ae(tabs[0].windows[0].launch_spec.opts.minimum_contrast, '60')
        self.assertIn('minimum_contrast', clone_safe_opts())
        self.ae(parse_launch_args(['--minimum-contrast=30%']).opts.minimum_contrast, '30%')
        with self.assertRaises(ValueError):
            list(parse_session('minimum_contrast 120\n', opts))

    def test_remote_control(self):
        from kitty.rc.base import command_for_name, parse_subcommand_cli
        from kitty.rc.set_minimum_contrast import parse_value

        self.ae(parse_value('40%'), 40)
        self.ae(parse_value('default'), None)
        with self.assertRaises(ValueError):
            parse_value('200')
        cmd = command_for_name('set_minimum_contrast')
        opts, items = parse_subcommand_cli(cmd, ['set-minimum-contrast', '--match-tab', 'id:1', '45'])
        payload = cmd.message_to_kitty(SimpleNamespace(), opts, items)
        self.ae(payload['value'], '45')
        self.ae(payload['match_tab'], 'id:1')

        self.options = {}
        tab = self.make_tab()
        w1, w2 = self.make_window(tab), self.make_window(tab)
        boss = SimpleNamespace(active_window=w1, all_windows=[w1, w2], match_tabs=lambda m: [tab], match_windows=lambda m, w: [w2])

        def run(**payload):
            return cmd.response_from_kitty(boss, None, payload.get)

        run(value='40')
        self.ae((w1.minimum_contrast_override, w2.minimum_contrast_override), (40, None))
        run(value='55', match_window='id:2')
        self.ae(w2.minimum_contrast_override, 55)
        run(value='65', all=True)
        self.ae((w1.screen.minimum_contrast, w2.screen.minimum_contrast), (65, 65))
        run(value='30', match_tab='id:1')
        self.ae((tab.minimum_contrast_override, w1.minimum_contrast_override, w2.screen.minimum_contrast), (30, None, 30))
        run(value='default', match_tab='id:1')
        self.ae(tab.minimum_contrast_override, None)
        with self.assertRaises(Exception):
            run(value='-5')

    def test_reference_lightness_algorithm(self):
        def L(c):
            return lightness(luminance(c))

        def hue(c):
            return to_hsluv(c)[0]

        blue_l = L(SSH_BLUE)
        self.assertAlmostEqual(blue_l, 29.8, delta=0.1)  # close to invisible on black
        # 0% changes nothing, nor does a minimum the color already meets
        self.ae(adjust_lightness(SSH_BLUE, BLACK, 0), SSH_BLUE)
        self.ae(adjust_lightness(SSH_BLUE, BLACK, 29), SSH_BLUE)
        kitty_blue = from_hex('0d73cc')
        self.ae(adjust_lightness(kitty_blue, BLACK, 40), kitty_blue)
        self.ae(adjust_lightness(WHITE, BLACK, 100), WHITE)
        for percent in (30, 40, 50, 75):
            ans = adjust_lightness(SSH_BLUE, BLACK, percent)
            self.assertAlmostEqual(L(ans), percent, delta=0.05)
            self.assertAlmostEqual(hue(ans), hue(SSH_BLUE), delta=0.05)  # still the same blue
            self.assertGreater(ans[2], max(ans[0], ans[1]))
        self.ae(to_hex(adjust_lightness(SSH_BLUE, BLACK, 50)), '#6161ff')
        # 100% is white on black and black on white
        self.assert_color(adjust_lightness(SSH_BLUE, BLACK, 100), WHITE)
        self.assert_color(adjust_lightness(from_hex('c4a000'), WHITE, 100), BLACK)
        # light backgrounds make text darker, keeping its hue
        yellow = from_hex('c4a000')
        ans = adjust_lightness(yellow, WHITE, 50)
        self.assertAlmostEqual(L(ans), 50, delta=0.05)
        self.assertAlmostEqual(hue(ans), hue(yellow), delta=0.05)
        # text stays on its own side of the background where there is room
        gray = from_hsluv(0, 0, 50)
        self.assertAlmostEqual(L(adjust_lightness(from_hsluv(120, 80, 55), gray, 30)), 80, delta=0.05)
        self.assertAlmostEqual(L(adjust_lightness(from_hsluv(120, 80, 45), gray, 30)), 20, delta=0.05)
        # on a gray background 100% is black or white, whichever is further away
        for text_l in (35, 45):
            self.assert_color(adjust_lightness(from_hsluv(120, 80, text_l), from_hsluv(0, 0, 40), 100), WHITE)
            self.assert_color(adjust_lightness(from_hsluv(120, 80, text_l + 20), from_hsluv(0, 0, 60), 100), BLACK)
        # without room on its own side text goes to the other side
        light_bg = from_hsluv(0, 0, 90)
        ans = adjust_lightness(from_hsluv(250, 60, 92), light_bg, 40)
        self.assertAlmostEqual(L(ans), 50, delta=0.05)
        # text the color of its background is made visible too
        ans = adjust_lightness(BLACK, BLACK, 40)
        self.assertAlmostEqual(L(ans), 40, delta=0.05)
        # HDR: measured on the SDR colors, the gain applies to the result
        sdr = adjust_lightness(SSH_BLUE, BLACK, 50)
        self.assert_color(adjust_lightness(tuple(2 * x for x in SSH_BLUE), BLACK, 50, fg_gain=2), tuple(2 * x for x in sdr))

    def test_reference_ratio_algorithm(self):
        # text_fg_override_threshold in ratio mode reaches its ratio
        for fg in ('0000ee', '000080', '1a1aff'):
            for ratio in (3, 4.5, 7):
                ans = adjust_ratio(from_hex(fg), BLACK, ratio)
                self.assertGreaterEqual(contrast_ratio(0, luminance(ans)), ratio - 0.01, f'#{fg} at {ratio}')
        self.ae(adjust_ratio(WHITE, BLACK, 4.5), WHITE)
