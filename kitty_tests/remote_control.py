#!/usr/bin/env python
# License: GPLv3 Copyright: 2026 Kovid Goyal <kovid at kovidgoyal.net>

from unittest.mock import Mock, patch

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
    def test_select_window_exclude_active(self):
        from kitty.boss import Boss as KittyBoss

        command = command_for_name('select-window')
        for exclude_active in (False, True):
            with self.subTest(exclude_active=exclude_active):
                tab = Mock(all_window_ids_except_active_window=set())
                boss = Mock(active_tab=tab)
                args = ('select-window',) + (('--exclude-active',) if exclude_active else ())
                with patch.object(command, 'create_async_responder'):
                    KittyBoss.call_remote_control(boss, None, args)
                self.ae(boss.visual_window_select_action.call_args.kwargs['only_window_ids'], set() if exclude_active else None)

    def test_visual_window_select_empty_allowed_set(self):
        from kitty.boss import Boss as KittyBoss

        self.set_options()
        for only_active_window_visible in (False, True):
            for only_window_ids, expected in ((set(), 0), (None, 1)):
                with self.subTest(only_active_window_visible=only_active_window_visible, only_window_ids=only_window_ids):
                    window = Mock(id=1)
                    tab = Mock(tab_manager_ref=Mock(return_value=None), os_window_id=1)
                    tab.current_layout.only_active_window_visible = only_active_window_visible
                    tab.windows.iter_windows_with_number.return_value = [(0, window)]
                    boss = Mock()
                    boss.select_window_in_tab_using_overlay = lambda *a: KittyBoss.select_window_in_tab_using_overlay(boss, *a)
                    with patch('kitty.boss.current_os_window', return_value=1):
                        KittyBoss.visual_window_select_action(boss, tab, Mock(), 'Choose', only_window_ids=only_window_ids)
                    boss.visual_window_select_action_trigger.assert_called_once_with(expected)
                    boss.choose_entry.assert_not_called()

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
