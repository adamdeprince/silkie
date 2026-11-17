#!/usr/bin/env python
# License: GPLv3 Copyright: 2026

import tempfile
from types import SimpleNamespace

from kitty.notifications import Action, FreeDesktopIntegration, sanitize_id

from .base import BaseTest
from .notifications import Channel, DesktopIntegration, NotificationManager


class TestNotificationRoutes(BaseTest):
    def test_notification_routes(self):
        with tempfile.TemporaryDirectory() as directory:
            desktop, channel = DesktopIntegration(None), Channel()
            manager = NotificationManager(desktop, channel, lambda *a: None, base_cache_dir=directory)
            desktop.notification_manager = manager
            manager.route_instance = 'terminal'
            a, b = 'odin-seat/session/3', 'odin-seat/session/8'
            send = manager.handle_notification_cmd
            send(12, 99, f'i=foo:r={a}:d=0:a=-focus,report;First')
            send(12, 99, f'i=foo:r={b}:d=0;Second')
            send(13, 99, f'i=foo:r={a}:d=0;Third')
            send(12, 99, f'i=foo:r={a}:p=body;Body')
            send(12, 99, f'i=foo:r={b}:p=body;Other body')
            send(13, 99, f'i=foo:r={a}:p=body;Third body')
            self.ae([(n['title'], n['body']) for n in desktop.notifications],
                    [('First', 'Body'), ('Second', 'Other body'), ('Third', 'Third body')])
            self.ae(len(manager.in_progress_notification_commands_by_client_id), 3)
            self.ae(manager.in_progress_notification_commands[1].actions, frozenset({Action.report}))
            manager.notification_activated(1, 2)
            self.ae(channel.focus_events, [])
            self.ae(channel.responses[-1], f'99;i=foo:r={a},silkie/terminal/12:a=report;2')
            manager.notification_activated(2, 0)
            self.ae(len(channel.focus_events), 1)
            self.ae(channel.responses[-1], f'99;i=foo:r={b},silkie/terminal/12:a=focus;')
            manager.notification_closed(3)
            self.ae(manager.in_progress_notification_commands_by_client_id, {})
            send(12, 99, f'i=query:r={a}:p=?;')
            self.assertTrue(f':r={a};' in channel.responses[-1])
            self.assertTrue(channel.responses[-1].endswith(':r=1'))
            send(12, 99, f'i=close:r={a}:c=1;Close me')
            send(12, 99, f'i=close:r={b}:p=close;')
            self.ae(desktop.close_events, [])
            send(12, 99, f'i=close:r={a}:p=close;')
            self.assertTrue(channel.responses[-1].startswith(f'99;i=close:r={a}:p=close;'))

    def test_notification_sustained_routing(self):
        with tempfile.TemporaryDirectory() as directory:
            desktop, channel = DesktopIntegration(None), Channel()
            manager = NotificationManager(desktop, channel, lambda *a: None, base_cache_dir=directory)
            desktop.notification_manager = manager
            manager.route_instance = 'terminal'
            identifier = 'x' * 16384
            self.ae(sanitize_id(identifier), identifier)
            route = ','.join(f'odin-seat/session{i}/{i + 1}' for i in range(40))
            for i in range(1024):
                manager.handle_notification_cmd(12, 99, f'i={i}.{identifier}:r={route}:a=report;Done {i}')
            self.ae(len(manager.in_progress_notification_commands), 1024)
            for i in range(1024):
                manager.notification_activated(i + 1, 0)
                self.assertTrue(channel.responses[-1].startswith(f'99;i={i}.{identifier}:r={route},silkie/terminal/12:'))
            self.ae(len(manager.in_progress_notification_commands), 0)
            self.ae(len(manager.in_progress_notification_commands_by_client_id), 0)
            self.ae(len(channel.responses), 1024)

    def test_invalid_notification_route(self):
        with tempfile.TemporaryDirectory() as directory:
            desktop = DesktopIntegration(None)
            manager = NotificationManager(desktop, Channel(), lambda *a: None, base_cache_dir=directory)
            desktop.notification_manager = manager
            for route in ('bad', 'odin-seat/session/3,', 'odin-seat/session/-1', 'odin-seat/a b/3'):
                manager.handle_notification_cmd(12, 99, f'i=foo:r={route};Invalid')
            self.ae(desktop.notifications, [])

    def test_dbus_notification_routing_lifetime(self):
        desktop = FreeDesktopIntegration.__new__(FreeDesktopIntegration)
        closed = []
        desktop.notification_manager = SimpleNamespace(notification_created=lambda n: None, notification_closed=closed.append)
        desktop.desktop_to_dbus, desktop.dbus_to_desktop, desktop.pending_closes = {}, {}, set()
        for i in range(1, 1025):
            desktop.created(i, 10000 + i)
        self.ae(len(desktop.dbus_to_desktop), 1024)
        desktop.created(1, 20000)
        self.assertNotIn(10001, desktop.dbus_to_desktop)
        desktop.created(1025, 20000)
        self.assertNotIn(1, desktop.desktop_to_dbus)
        for native, dbus in tuple(desktop.desktop_to_dbus.items()):
            desktop.dispatch_event_from_desktop('closed', dbus, 0)
        self.ae(len(closed), 1024)
        self.ae(desktop.desktop_to_dbus, {})
        self.ae(desktop.dbus_to_desktop, {})
