#!/usr/bin/env python
# License: GPLv3 Copyright: 2026, Adam DePrince

import os
import signal
from types import SimpleNamespace
from unittest.mock import Mock, patch

from kitty.boss import Boss
from kitty.colors import ThemeColors, patch_colors
from kitty.config import load_config
from kitty.constants import kitty_exe
from kitty.fast_data_types import get_options
from kitty.profiles import Profile, apply_to_window
from kitty.window import Watchers, Window

from .base import BaseTest, parse_bytes


class TestColorSchemeResize(BaseTest):

    def setUp(self):
        super().setUp()
        self.screen = self.create_screen()
        w = self.window = Window.__new__(Window)
        w.id, w.os_window_id, w.tab_id = 7, 3, 2
        w.screen, w.child = self.screen, SimpleNamespace(child_fd=42)
        w.child_is_launched, w.destroyed = True, False
        w.last_reported_pty_size = (5, 5, 50, 100)
        w.watchers, w.call_watchers, w.refresh = Watchers(), Mock(), Mock()
        self.boss = SimpleNamespace(
            all_windows=[w], all_tab_managers=[], window_id_map={7: w}, os_window_map={},
            window_for_dispatch=w, active_window=w, show_error=Mock(),
            args=SimpleNamespace(debug_rendering=False, debug_font_fallback=False),
        )
        self.boss.default_bg_changed_for = lambda *a, **kw: Boss.default_bg_changed_for(self.boss, *a, **kw)
        self.real_tcgetpgrp, self.real_killpg = os.tcgetpgrp, os.killpg
        for target, value in (
            ('kitty.colors.get_boss', self.boss), ('kitty.window.get_boss', self.boss),
            ('kitty.colors.glfw_get_system_color_theme', 'no_preference'),
            ('kitty.window.os.tcgetpgrp', 12345),
        ):
            p = patch(target, return_value=value)
            started = p.start()
            self.addCleanup(p.stop)
            if target.endswith('tcgetpgrp'):
                self.tcgetpgrp = started
        p = patch('kitty.window.os.killpg')
        self.killpg = p.start()
        self.addCleanup(p.stop)

    def test_palette_changes_reuse_resize_notifications(self):
        parse_bytes(self.screen, b'\x1b[?2031h\x1b[?2048h')
        self.screen.callbacks.clear()
        size = self.window.last_reported_pty_size
        # Foreground and indexed-color changes count even when the background is unchanged.
        for spec in ({'foreground': 0x123456}, {'color197': 0x654321}):
            self.killpg.reset_mock()
            patch_colors(spec, configured=True)
            self.killpg.assert_called_once_with(12345, signal.SIGWINCH)
            self.ae(self.window.last_reported_pty_size, size)
            self.ae(self.screen.callbacks.wtcbuf, b'\x1b[48;5;5;100;50t')
            self.screen.callbacks.clear()
            self.killpg.reset_mock()
            patch_colors(spec, configured=True)
            self.killpg.assert_not_called()
            self.ae(self.screen.callbacks.wtcbuf, b'')
        observed = []
        self.killpg.side_effect = lambda *a: observed.append(self.screen.callbacks.wtcbuf)
        patch_colors({'background': 0xFFFFFF}, configured=True)
        self.ae(observed, [b'\x1b[?997;2n\x1b[48;5;5;100;50t'])
        # Notification does not require either subscription.
        parse_bytes(self.screen, b'\x1b[?2031l\x1b[?2048l')
        self.screen.callbacks.clear()
        self.killpg.reset_mock()
        patch_colors({'foreground': 0xABCDEF}, configured=True)
        self.killpg.assert_called_once_with(12345, signal.SIGWINCH)
        self.ae(self.screen.callbacks.wtcbuf, b'')

    def test_application_palette_changes_do_not_request_resize(self):
        parse_bytes(self.screen, b'\x1b[?2031h\x1b[?2048h')
        self.screen.callbacks.clear()
        for _ in range(32):
            self.window.set_dynamic_color(11, '#ffffff')
            self.window.set_dynamic_color(11, '#000000')
            self.window.set_color_table_color(4, memoryview(b'1;#abcdef'))
            self.window.color_control(21, '2=#123456')
        self.killpg.assert_not_called()
        self.ae(self.screen.callbacks.wtcbuf, b'')

    def test_runtime_scheme_and_eink_actions(self):
        themes = ThemeColors()
        with patch('kitty.boss.theme_colors', themes), patch.object(themes, 'refresh', return_value=False):
            themes.patch_opts(get_options())
            Boss.set_color_scheme(self.boss, 'light')
            self.killpg.assert_called_once_with(12345, signal.SIGWINCH)
            Boss.set_color_scheme(self.boss, 'light')
            self.ae(self.killpg.call_count, 1)
            # E-paper changes still redraw when an explicit scheme leaves its palette alone.
            Boss.set_eink_mode(self.boss, 'yes')
            self.ae(self.killpg.call_count, 2)
            Boss.set_eink_mode(self.boss, 'yes')
            self.ae(self.killpg.call_count, 2)
            for _ in range(64):
                Boss.set_color_scheme(self.boss, 'toggle')
            self.ae(self.killpg.call_count, 66)

    def test_profile_change_and_display_callback(self):
        profile = Profile('work', 'Work', options={'foreground': ['#123456'], 'color4': ['#ABCDEF']})

        class Manager:
            def __iter__(inner):
                return iter(((self.window,),))

        tm = Manager()
        tm.resize = Mock()
        tm.apply_options = lambda: apply_to_window(self.window, tm.profile)
        self.boss.active_tab_manager = tm
        with patch('kitty.boss.load_profiles', return_value={'work': profile}), patch('kitty.boss.apply_to_os_window'):
            Boss.set_window_profile(self.boss, 'work')
            self.killpg.assert_called_once_with(12345, signal.SIGWINCH)
            Boss.set_window_profile(self.boss, 'work')
            self.ae(self.killpg.call_count, 1)
        self.boss.os_window_map[3] = tm
        Boss.on_display_color_change(self.boss, 3)
        self.ae(self.killpg.call_count, 2)
        Boss.on_display_color_change(self.boss, 999)
        self.ae(self.killpg.call_count, 2)

    def test_config_and_settings_reload_coalesce_color_layers(self):
        themes = ThemeColors()
        self.boss.mappings = SimpleNamespace(update_keymap=Mock())
        self.boss.all_tab_managers = [SimpleNamespace(
            apply_options=lambda: self.screen.color_profile.reload_from_opts(),
            tab_bar=Mock(), mark_tab_bar_dirty=Mock(), active_tab=None, os_window_id=3,
        )]
        with (
            patch('kitty.boss.theme_colors', themes), patch.object(themes, 'refresh', return_value=False),
            patch('kitty.boss.is_macos', False), patch('kitty.boss.apply_options_update'), patch('kitty.boss.set_layout_options'),
            patch('kitty.boss.is_wayland', return_value=False),
            patch('kitty.boss.set_default_env'), patch('kitty.boss.clear_profile_caches'), patch('kitty.boss.load_profiles'),
            patch('kitty.boss.update_native_menus'), patch('kitty.fonts.render.set_font_family'),
            patch('kitty.boss.load_shader_programs'), patch('kitty.colors.set_os_window_chrome'),
        ):
            # Settings writes a layer and follows this same final apply_new_options path.
            for overrides in (['foreground #123456'], ['color_scheme light']):
                self.killpg.reset_mock()
                Boss.apply_new_options(self.boss, load_config(overrides=overrides))
                self.killpg.assert_called_once_with(12345, signal.SIGWINCH)
                Boss.apply_new_options(self.boss, load_config(overrides=overrides))
                self.ae(self.killpg.call_count, 1)

    def test_closed_or_unlaunched_child_is_not_signaled(self):
        for attr, value in (('child_is_launched', False), ('destroyed', True), ('child_died', True)):
            setattr(self.window, attr, value)
            self.window.notify_child_of_color_change()
            self.killpg.assert_not_called()
            setattr(self.window, attr, not value)
        self.window.child.child_fd = None
        self.window.notify_child_of_color_change()
        self.killpg.assert_not_called()

    def test_real_foreground_process_receives_resize_without_size_change(self):
        code = '''
import os, signal
initial = os.get_terminal_size(0)
def resized(sig, frame, initial=initial, get_size=os.get_terminal_size):
    os.write(1, b'WINCH_SAME_SIZE ' + str(get_size(0) == initial).encode() + b'\\n')
    raise SystemExit(0)
signal.signal(signal.SIGWINCH, resized)
os.write(1, b'READY\\n')
signal.pause()
'''
        with self.create_pty([kitty_exe(), '+runpy', code], cols=5, lines=5) as pty:
            pty.wait_till(lambda: b'READY' in pty.received_bytes)
            self.window.child.child_fd = pty.master_fd
            self.tcgetpgrp.side_effect = self.real_tcgetpgrp
            self.killpg.side_effect = self.real_killpg
            self.assertNotEqual(self.real_tcgetpgrp(pty.master_fd), os.getpgrp())
            self.window.notify_child_of_color_change()
            pty.wait_till(lambda: b'WINCH_SAME_SIZE True' in pty.received_bytes)
            pty.wait_till_child_exits(require_exit_code=0)
