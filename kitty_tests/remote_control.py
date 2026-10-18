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

    def match_tabs(self, expr: str):
        if expr == 'all':
            yield list(self.all_windows)


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

    def test_run_mapping_ignores_stdin(self):
        from kitty.boss import Boss as KittyBoss

        command = command_for_name('run')
        for is_tty in (False, True):
            with self.subTest(is_tty=is_tty):
                stdin = Mock()
                stdin.isatty.return_value = is_tty
                boss = Mock()
                with patch('sys.stdin', stdin), patch.object(command, 'create_async_responder'):
                    KittyBoss.call_remote_control(boss, None, ('run', 'cat'))
                    boss.run_background_process.call_args.kwargs['notify_on_death'](0, None)
                stdin.buffer.read.assert_not_called()
                boss.run_background_process.assert_called_once()
                call = boss.run_background_process.call_args
                self.ae(call.args[0], ['cat'])
                self.ae(call.kwargs['stdin'], b'')

    def test_set_enabled_layouts_configured(self):
        from kitty.boss import Boss as KittyBoss

        for configured in (False, True):
            with self.subTest(configured=configured):
                options = self.set_options({'enabled_layouts': ['tall']})
                boss = Mock(active_tab=Mock())
                args = ('set-enabled-layouts',) + (('--configured',) if configured else ()) + ('stack',)

                KittyBoss.call_remote_control(boss, None, args)

                boss.active_tab.set_enabled_layouts.assert_called_once_with(['stack'])
                self.ae(options.enabled_layouts, ['stack'] if configured else ['tall'])

    def test_ls_self_and_output_format(self):
        from kitty.boss import Boss as KittyBoss

        self_window = Mock()
        boss = Mock(list_os_windows=Mock(return_value=[]), serialize_state_as_session=Mock(return_value=['a', 'b']))
        self.ae(KittyBoss.call_remote_control(boss, None, ('ls', '--output-format', 'session')), 'a\nb')
        boss.list_os_windows.assert_not_called()

        KittyBoss.call_remote_control(boss, self_window, ('ls', '--self'))
        window_filter = boss.list_os_windows.call_args.args[2]
        self.assertIsNotNone(window_filter)
        self.assertTrue(window_filter(self_window))
        self.assertFalse(window_filter(Mock()))

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

    def test_send_key_excludes_active_window(self):
        windows = [Window(1, 11), Window(2, 11)]
        boss = Boss(windows)
        cmd = command_for_name('send-key')
        keys = ['ctrl+a', 'ctrl+b']
        # selector -> (expected recipients without --exclude-active, with --exclude-active)
        cases = {
            'all': ({'all': True}, windows, windows[1:]),
            'match': ({'match': 'all'}, windows, windows[1:]),
            'match_tab': ({'match_tab': 'all'}, windows, windows[1:]),
            'default': ({}, windows[:1], []),
        }

        for name, (selector, expected, expected_excluding_active) in cases.items():
            for exclude_active in (False, True):
                with (
                    self.subTest(selector=name, exclude_active=exclude_active),
                    patch.object(windows[0], 'send_key', create=True) as active,
                    patch.object(windows[1], 'send_key', create=True) as other,
                ):
                    cmd.response_from_kitty(boss, None, PayloadGetter(cmd, {**selector, 'exclude_active': exclude_active, 'keys': keys}))

                    recipients = expected_excluding_active if exclude_active else expected
                    for w, mock in ((windows[0], active), (windows[1], other)):
                        if w in recipients:
                            mock.assert_called_once_with(*keys)
                        else:
                            mock.assert_not_called()
