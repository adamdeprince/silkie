#!/usr/bin/env python
# License: GPLv3 Copyright: 2026 Kovid Goyal <kovid at kovidgoyal.net>

from unittest.mock import patch

from kitty.rc.base import PayloadGetter, all_command_names, command_for_name

from .base import BaseTest


class Window:
    def __init__(self, id: int, os_window_id: int):
        self.id = id
        self.os_window_id = os_window_id


class Boss:
    def __init__(self, windows: list[Window]):
        self.active_window = windows[0]
        self.all_windows = windows

    def match_windows(self, expr: str, self_window: Window | None = None):
        if expr == 'all':
            yield from self.all_windows
            return
        for window in self.all_windows:
            if str(window.id) == expr:
                yield window


class TestRemoteControl(BaseTest):
    def test_set_os_window_title_command_is_registered(self):
        self.assertIn('set_os_window_title', all_command_names())
        self.ae(command_for_name('set-os-window-title').name, 'set-os-window-title')

    def test_set_os_window_title_dedupes_os_windows(self):
        windows = [Window(1, 11), Window(2, 11), Window(3, 22)]
        boss = Boss(windows)
        cmd = command_for_name('set-os-window-title')

        with patch('kitty.rc.set_os_window_title.set_os_window_title_impl') as setter:
            cmd.response_from_kitty(boss, windows[0], PayloadGetter(cmd, {'match': 'all', 'self': True, 'title': 'repo'}))

        self.ae({call.args for call in setter.call_args_list}, {(11, 'repo'), (22, 'repo')})

    def test_set_os_window_title_without_title_resets_override(self):
        windows = [Window(1, 11)]
        boss = Boss(windows)
        cmd = command_for_name('set-os-window-title')

        with patch('kitty.rc.set_os_window_title.set_os_window_title_impl') as setter:
            cmd.response_from_kitty(boss, windows[0], PayloadGetter(cmd, {'self': True}))

        setter.assert_called_once_with(11, '')

    def test_set_colors_transparent_background_colors(self):
        from kitty.fast_data_types import Color
        from kitty.rc.base import RemoteControlErrorWithoutTraceback, parse_subcommand_cli
        from kitty.remote_control import parse_rc_args

        cmd = command_for_name('set-colors')

        def payload(*args: str):
            opts, items = parse_subcommand_cli(cmd, ['set-colors'] + list(args))
            return cmd.message_to_kitty(parse_rc_args([])[0], opts, items)

        # omitted means leave unchanged, explicitly empty means clear
        self.assertNotIn('transparent_background_colors', payload('foreground=red')['colors'])
        self.ae(payload('transparent_background_colors=')['colors']['transparent_background_colors'], '')
        self.ae(payload('transparent_background_colors=red@0.5')['colors']['transparent_background_colors'], '#ff0000@0.5')

        windows = [Window(1, 11)]
        boss = Boss(windows)
        boss.color_settings_at_startup = {'foreground': Color(1, 2, 3)}
        boss.transparent_background_colors_at_startup = ((Color(0, 0, 0), 0.25),)

        def response(p):
            with patch('kitty.colors.patch_colors') as patcher:
                cmd.response_from_kitty(boss, windows[0], PayloadGetter(cmd, p))
            patcher.assert_called_once()
            return patcher.call_args.args

        self.ae(response({'colors': {'foreground': 0xFF0000}}), ({'foreground': 0xFF0000}, None, False))
        colors = {'foreground': 0xFF0000, 'transparent_background_colors': 'red@0.5'}
        self.ae(response({'colors': colors, 'configured': True}), ({'foreground': 0xFF0000}, ((Color(255, 0, 0), 0.5),), True))
        self.ae(colors['transparent_background_colors'], 'red@0.5')  # payload must not be mutated
        self.ae(response({'colors': {'transparent_background_colors': ''}}), ({}, (), False))
        # reset implies configured even when the client does not set it
        self.ae(
            response({'colors': {'transparent_background_colors': 'red@0.5'}, 'reset': True}),
            ({'foreground': int(Color(1, 2, 3))}, ((Color(0, 0, 0), 0.25),), True),
        )
        for bad in (0, 'not-a-color@0.5'):
            with self.subTest(bad=bad), self.assertRaises(RemoteControlErrorWithoutTraceback):
                response({'colors': {'transparent_background_colors': bad}})
