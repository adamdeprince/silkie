#!/usr/bin/env python
# License: GPLv3 Copyright: 2026

import tempfile
from types import SimpleNamespace
from unittest.mock import Mock, patch

from kitty.fast_data_types import VT_PARSER_BUFFER_SIZE
from kitty.notifications import MacOSIntegration
from kitty.progress import Progress, ProgressState
from kitty.window import Window

from .base import BaseTest, parse_bytes
from .notifications import Channel, DesktopIntegration, NotificationManager


class TestOSC9(BaseTest):
    def notification_window(self, directory):
        screen = self.create_screen()
        desktop, channel = DesktopIntegration(None), Channel()
        manager = NotificationManager(desktop, channel, base_cache_dir=directory)
        desktop.notification_manager = manager
        window = Window.__new__(Window)
        window.id = 7
        window.screen = SimpleNamespace(set_progress=Mock())
        window.progress = Progress()
        window.tabref = Mock(return_value=SimpleNamespace(update_progress=Mock()))
        window.clear_progress_if_needed = Mock()
        screen.callbacks.desktop_notify = window.desktop_notify
        return screen, window, manager, desktop, channel

    def test_osc9_notifications_through_parser(self):
        with tempfile.TemporaryDirectory() as directory:
            screen, window, manager, desktop, channel = self.notification_window(directory)
            with patch('kitty.window.get_boss', return_value=SimpleNamespace(notification_manager=manager)):
                for terminator in (b'\x07', b'\x1b\\'):
                    message = 'Claude finished; ready for your next prompt 🐔'
                    sequence = b'\x1b]9;' + message.encode() + terminator
                    # Every possible boundary includes split UTF-8 and split ST.
                    for split in range(1, len(sequence)):
                        before = len(desktop.notifications)
                        parse_bytes(screen, sequence[:split])
                        self.ae(len(desktop.notifications), before)
                        parse_bytes(screen, sequence[split:])
                        self.ae(len(desktop.notifications), before + 1)
                        self.ae(desktop.notifications[-1]['title'], message)
                # Exercise accumulation beyond the parser's input buffer.
                message = 'Done; ' + 'x' * (VT_PARSER_BUFFER_SIZE + 1)
                parse_bytes(screen, b'\x1b]9;' + message.encode() + b'\x07')
                self.ae(desktop.notifications[-1]['title'], message)
                for i in range(1024):
                    parse_bytes(screen, f'\x1b]9;Done {i}\x07'.encode())
                self.ae([n['title'] for n in desktop.notifications[-1024:]], [f'Done {i}' for i in range(1024)])
                for notification_id in tuple(manager.in_progress_notification_commands):
                    manager.notification_activated(notification_id, 0)
                self.ae(manager.in_progress_notification_commands, {})
                self.ae(len(channel.focus_events), len(desktop.notifications))
                self.ae(channel.responses, [])
                window.screen.set_progress.assert_not_called()

    def test_osc9_progress_and_notifications(self):
        with tempfile.TemporaryDirectory() as directory:
            screen, window, manager, desktop, channel = self.notification_window(directory)
            with patch('kitty.window.get_boss', return_value=SimpleNamespace(notification_manager=manager)):
                for payload, state, percent in (
                    ('4;3', ProgressState.indeterminate, 0),
                    ('4;1;75', ProgressState.set, 75),
                    ('4;4', ProgressState.paused, 75),
                    ('4', ProgressState.unset, 0),
                    ('4;1;100', ProgressState.set, 100),
                    ('4;', ProgressState.unset, 0),
                    ('4;0', ProgressState.unset, 0),
                ):
                    with self.subTest(payload=payload):
                        parse_bytes(screen, f'\x1b]9;{payload}\x1b\\'.encode())
                        self.ae((window.progress.state, window.progress.percent), (state, percent))
                        self.ae(desktop.notifications, [])
                with patch('kitty.window.log_error') as log:
                    parse_bytes(screen, b'\x1b]9;4;invalid\x07')
                    log.assert_called_once()
                parse_bytes(screen, b'\x1b]9;Claude finished\x07')
                self.ae([n['title'] for n in desktop.notifications], ['Claude finished'])

    def test_osc9_macos_delivery(self):
        from kitty.constants import is_macos

        if not is_macos:
            self.skipTest('macOS notification integration')
        with tempfile.TemporaryDirectory() as directory:
            screen, window, manager, desktop, channel = self.notification_window(directory)
            native = MacOSIntegration.__new__(MacOSIntegration)
            native.notification_manager = manager
            with (
                patch('kitty.fast_data_types.cocoa_set_notification_activated_callback'),
                patch('kitty.fast_data_types.cocoa_send_notification') as send,
                patch('kitty.window.get_boss', return_value=SimpleNamespace(notification_manager=manager)),
            ):
                native.initialize()
                manager.desktop_integration = native
                parse_bytes(screen, b'\x1b]9;Claude finished\x07')
                self.ae(send.call_args.args[2:4], ('Claude finished', ' '))
                self.ae(send.call_args.kwargs['muted'], False)
                identifier = send.call_args.args[1]
                native.notification_activated('creation_failed', identifier, '')
                self.ae(manager.in_progress_notification_commands, {})
                parse_bytes(screen, b'\x1b]9;Claude finished again\x1b\\')
                self.ae(send.call_count, 2)
                identifier = send.call_args.args[1]
                native.notification_activated('activated', identifier, '')
                self.ae(channel.focus_events, [''])
                self.ae(channel.responses, [])
