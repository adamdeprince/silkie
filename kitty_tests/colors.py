#!/usr/bin/env python
# License: GPLv3

from types import SimpleNamespace
from unittest.mock import Mock, patch

from kitty.boss import Boss
from kitty.colors import ThemeColors, color_scheme_spec
from kitty.config import load_config
from kitty.fast_data_types import Color, get_options, set_options
from kitty.options.types import Options
from kitty.window import Window

from .base import BaseTest, parse_bytes


class TestColorSchemes(BaseTest):
    def test_explicit_color_schemes(self):
        for scheme, eink, expected in (
            ('auto', False, None),
            ('auto', True, (0, 0xFFFFFF)),
            ('light', False, (0, 0xFFFFFF)),
            ('dark', False, (0xFFFFFF, 0)),
            ('dark', True, (0xFFFFFF, 0)),
        ):
            with self.subTest(scheme=scheme, eink=eink):
                opts = Options({'color_scheme': scheme, 'eink_mode': eink})
                spec = color_scheme_spec(opts)
                if expected is None:
                    self.assertIsNone(spec)
                    continue
                fg, bg = expected
                self.ae((spec['foreground'], spec['background']), expected)
                self.ae((spec['cursor'], spec['cursor_text_color']), expected)
                self.ae((spec['selection_foreground'], spec['selection_background']), (bg, fg))
                self.ae((spec['active_tab_foreground'], spec['active_tab_background']), (bg, fg))

    def test_eink_startup_and_reload(self):
        themes = ThemeColors()
        themes.dark_mtime = 0
        themes.dark_spec = {'foreground': 0x123456, 'background': 0x654321}
        themes.dark_tbc = ()
        themes.dark_background_image_options = {}
        with patch('kitty.colors.glfw_get_system_color_theme', return_value='dark'):
            for _ in range(32):
                opts = load_config(overrides=['eink_mode yes'])
                themes.patch_opts(opts)
                self.ae((int(opts.foreground), int(opts.background)), (0, 0xFFFFFF))
                set_options(opts)
                self.assertFalse(themes.on_system_color_scheme_change('dark'))
                opts = load_config(overrides=['eink_mode no'])
                themes.patch_opts(opts)
                self.ae((int(opts.foreground), int(opts.background)), (0x123456, 0x654321))
                self.ae(themes.applied_theme, 'dark')

    def test_color_switching_and_notifications(self):
        from kitty.fast_data_types import patch_color_profiles

        screen = self.create_screen()
        opts = get_options()
        opts.foreground, opts.background = Color(12, 34, 56), Color(20, 30, 40)
        themes = ThemeColors()
        window = Window.__new__(Window)
        window.screen = screen
        window.id = 7
        window.refresh = Mock()
        boss = SimpleNamespace(
            all_windows=[window],
            all_tab_managers=[],
            window_for_dispatch=window,
            active_window=window,
            show_error=Mock(),
        )
        boss.default_bg_changed_for = lambda window_id: window.report_color_scheme_preference_if_wanted()
        parse_bytes(screen, b'\x1b[?2031h')
        with (
            patch('kitty.colors.get_boss', return_value=boss),
            patch('kitty.colors.glfw_get_system_color_theme', return_value='no_preference'),
            patch.object(themes, 'refresh', return_value=False),
            patch('kitty.boss.theme_colors', themes),
        ):
            themes.patch_opts(opts)
            patch_color_profiles({'foreground': int(opts.foreground), 'background': int(opts.background)}, (), (screen.color_profile,), True)
            for i in range(256):
                screen.callbacks.clear()
                Boss.set_color_scheme(boss, 'toggle')
                fg, bg, report = (0, 0xFFFFFF, 2) if i % 2 == 0 else (0xFFFFFF, 0, 1)
                self.ae((int(screen.color_profile.default_fg), int(screen.color_profile.default_bg)), (fg, bg))
                self.ae(screen.callbacks.wtcbuf, f'\x1b[?997;{report}n'.encode())
                # Configured colors also reach newly created windows.
                self.ae(int(opts.background), bg)
                self.ae(screen.color_profile.as_dict()['cursor'], fg)
            Boss.set_color_scheme(boss, 'auto')
            self.ae(int(screen.color_profile.default_fg), 0x0C2238)
            self.ae(int(screen.color_profile.default_bg), 0x141E28)
            Boss.set_eink_mode(boss, 'yes')
            self.assertTrue(opts.eink_mode)
            self.ae(int(screen.color_profile.default_bg), 0xFFFFFF)
            Boss.set_color_scheme(boss, 'dark')
            self.assertTrue(opts.eink_mode)
            self.ae(int(screen.color_profile.default_bg), 0)
            Boss.set_color_scheme(boss, 'auto')
            Boss.set_eink_mode(boss, 'no')
            self.assertFalse(opts.eink_mode)
            self.ae(int(screen.color_profile.default_bg), 0x141E28)
            before = opts.color_scheme, opts.eink_mode
            Boss.set_color_scheme(boss, 'invalid')
            Boss.set_eink_mode(boss, 'invalid')
            self.ae((opts.color_scheme, opts.eink_mode), before)
            self.ae(boss.show_error.call_count, 2)

    def test_eink_display_overrides(self):
        from kitty.fast_data_types import set_eink_mode

        # Override lists beyond the former 256-byte buffer must still be honored.
        overrides = ' '.join('ignored=value' for _ in range(1024)) + ' kind=lcd color=gray bits=4 ppi=226'
        screen = self.create_screen(options={'display_info': overrides})
        for _ in range(128):
            for enabled, kind in ((True, 'eink'), (False, 'lcd')):
                set_eink_mode(enabled)
                screen.callbacks.clear()
                parse_bytes(screen, b'\x1b]777;display;v=1:op=query:id=7\x1b\\')
                expected = (
                    f'\x1b]777;display;v=1:op=cap:id=7:mode=0:kind={kind}:color=gray:bits=4:gamut=unknown:headroom=1:scale=0:period=0:ppi=226\x1b\\'
                ).encode()
                self.ae(screen.callbacks.wtcbuf, expected)

    def test_scheme_shortcuts(self):
        bad_lines = []
        opts = load_config(
            overrides=[
                'color_scheme light',
                'eink_mode yes',
                'map f6 set_color_scheme toggle',
                'map f7 set_eink_mode toggle',
            ],
            accumulate_bad_lines=bad_lines,
        )
        self.assertFalse(bad_lines)
        self.ae(opts.color_scheme, 'light')
        self.assertTrue(opts.eink_mode)
        actions = [mapping.definition for mappings in opts.keyboard_modes[''].keymap.values() for mapping in mappings]
        self.assertIn('set_color_scheme toggle', actions)
        self.assertIn('set_eink_mode toggle', actions)
