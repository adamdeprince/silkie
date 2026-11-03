#!/usr/bin/env python
# License: GPLv3 Copyright: 2026 Kovid Goyal <kovid at kovidgoyal.net>

import os
import subprocess
import tempfile
from typing import Any, cast
from unittest.mock import Mock, patch

from kitty.constants import kitty_exe
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
    def test_cancel_async_auth_bypass(self):
        from kitty.boss import Boss as KittyBoss
        from kitty.remote_control import handle_cmd, is_cmd_allowed

        self.set_options({'remote_control_password': {'secret': frozenset(('ls',))}})
        base = {'cmd': 'send_text', 'version': [0, 26, 0], 'payload': {'data': 'text:x'}, 'cancel_async': True}
        for extra in ({'async_id': 'x'}, {'async_id': 'x', 'password': 'wrong'}, {'async_id': 'x', 'password': 'secret'}, {'async': ''}):
            with self.subTest(extra=extra):
                self.assertIsNot(is_cmd_allowed({**base, **extra}, None, False, {}), True)
        # a genuine cancellation is allowed without a password and must only cancel
        pcmd = {**base, 'async': 'x'}
        self.assertIs(is_cmd_allowed(pcmd, None, False, {}), True)
        command = command_for_name('send_text')
        with patch.object(command, 'response_from_kitty') as rfk, patch.object(command, 'cancel_async_request') as car:
            self.assertIsNone(handle_cmd(Mock(spec=KittyBoss), None, pcmd, 0, None))
        rfk.assert_not_called()
        car.assert_called_once()
        # a cancellation must not register its stream, otherwise later requests
        # on that stream would be allowed without authentication
        from kitty.remote_control import active_streams, close_active_stream

        sid = 'cancel-async-stream'
        self.addCleanup(close_active_stream, sid)
        pcmd = {'cmd': 'run', 'version': [0, 26, 0], 'stream_id': sid, 'async': 'y', 'cancel_async': True}
        self.assertIs(is_cmd_allowed(pcmd, None, False, {}), True)
        command = command_for_name('run')
        with patch.object(command, 'response_from_kitty') as rfk, patch.object(command, 'cancel_async_request') as car:
            self.assertIsNone(handle_cmd(Mock(spec=KittyBoss), None, pcmd, 0, None))
        rfk.assert_not_called()
        car.assert_called_once()
        self.assertNotIn(sid, active_streams)
        pcmd = {'cmd': 'run', 'version': [0, 26, 0], 'stream_id': sid, 'payload': {'cmdline': ['true'], 'data': ''}}
        self.assertIsNot(is_cmd_allowed(pcmd, None, False, {}), True)

    def test_stream_continuation_auth(self):
        from base64 import standard_b64encode

        from kitty.boss import Boss as KittyBoss
        from kitty.remote_control import abort_active_stream, abort_active_streams_from, active_streams, handle_cmd, is_cmd_allowed, register_active_stream

        self.set_options({'remote_control_password': {'secret': frozenset(('ls',))}})
        command = command_for_name('run')

        def cleanup() -> None:
            for sid in tuple(active_streams):
                abort_active_stream(sid)

        self.addCleanup(cleanup)

        def chunk(sid: str, data: bytes = b'x', payload: dict[str, Any] | None = None, **kw: Any) -> dict[str, Any]:
            p: dict[str, Any] = {'cmdline': ['true'], 'data': standard_b64encode(data).decode('ascii'), **(payload or {})}
            return {'cmd': 'run', 'version': [0, 26, 0], 'stream_id': sid, 'async': 'a-' + sid, 'payload': p, **kw}

        def open_stream(sid: str, peer_id: int = 0, window: Window | None = None) -> None:
            # handle_cmd() is only called for authorized requests
            r = handle_cmd(Mock(spec=KittyBoss), window, chunk(sid, stream=True), peer_id, None)
            self.ae(r, {'ok': True, 'stream': True})
            self.assertIn(sid, active_streams)

        def allowed(pcmd: dict[str, Any], peer_id: int = 0, window: Window | None = None) -> bool:
            return is_cmd_allowed(pcmd, cast(Any, window), peer_id > 0, {}, peer_id) is True

        # continuations from the same origin with only the data changed are allowed
        open_stream('s1', peer_id=7)
        self.assertTrue(allowed(chunk('s1', b'y'), 7))
        self.assertTrue(allowed(chunk('s1', b''), 7))
        # continuations must not change anything other than the data
        self.assertFalse(allowed(chunk('s1', payload={'cmdline': ['evil']}), 7))
        self.assertFalse(allowed(chunk('s1', payload={'env': ['A=b']}), 7))
        self.assertFalse(allowed({**chunk('s1'), 'async': 'other'}, 7))
        self.assertFalse(allowed({**chunk('s1'), 'cmd': 'set-window-logo'}, 7))
        # continuations must come from the origin that opened the stream
        self.assertFalse(allowed(chunk('s1'), 8))
        self.assertFalse(allowed(chunk('s1'), 0, Window(7, 1)))
        self.assertFalse(allowed(chunk('unknown'), 7))

        # cancellations only abort streams opened from the same origin
        cancel = {'cmd': 'run', 'version': [0, 26, 0], 'stream_id': 's1', 'async': 'a-s1', 'cancel_async': True}
        self.assertIsNone(handle_cmd(Mock(spec=KittyBoss), None, dict(cancel), 8, None))
        self.assertIn('s1', active_streams)
        self.ae(command.stream_in_flight.stream_id, 's1')
        self.assertIsNone(handle_cmd(Mock(spec=KittyBoss), None, dict(cancel), 7, None))
        self.assertNotIn('s1', active_streams)
        self.ae(command.stream_in_flight.stream_id, '')
        self.assertIsNone(command.stream_in_flight.tempfile)
        self.assertFalse(allowed(chunk('s1'), 7))

        # streams are aborted when the peer or window that opened them goes away
        w = Window(5, 1)
        open_stream('s2', peer_id=7)
        open_stream('s3', window=w)
        self.assertTrue(allowed(chunk('s3'), 0, w))
        abort_active_streams_from(peer_id=7)
        self.assertNotIn('s2', active_streams)
        self.assertIn('s3', active_streams)
        self.ae(command.stream_in_flight.stream_id, 's3')
        abort_active_streams_from(window_id=w.id)
        self.assertNotIn('s3', active_streams)
        self.ae(command.stream_in_flight.stream_id, '')

        # a command has only one stream in flight, opening another aborts the first
        open_stream('s4', peer_id=7)
        open_stream('s5', peer_id=8)
        self.assertNotIn('s4', active_streams)
        self.assertFalse(allowed(chunk('s4'), 7))
        self.assertTrue(allowed(chunk('s5'), 8))

        # the number of registered streams is bounded
        for i in range(40):
            register_active_stream(f'many-{i}', chunk(f'many-{i}'), None, 9)
        self.ae(len(active_streams), 32)
        self.assertNotIn('many-0', active_streams)
        self.assertIn('many-39', active_streams)

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


TALK_THREAD_SURVIVES_ACCEPT_ERROR = """
import os, socket, sys, time
from kitty.fast_data_types import ChildMonitor
ls = socket.socket(socket.AF_UNIX)
ls.bind(sys.argv[1])
ls.listen(8)
cm = ChildMonitor(lambda *a: None, None, -1, ls.fileno())
a, b = socket.socketpair()
cm.inject_peer(os.dup(a.fileno()))
cm.fail_next_accepts(2)
c = socket.socket(socket.AF_UNIX)
c.connect(sys.argv[1])
deadline = time.monotonic() + 10
while cm.fail_next_accepts(-1) and time.monotonic() < deadline:
    time.sleep(0.01)
assert cm.fail_next_accepts(-1) == 0, 'injected accept errors not consumed'
cm.inject_peer(os.dup(b.fileno()))
print('ok', flush=True)
os._exit(0)
"""


TALK_THREAD_STALLED_PEER = """
import os, socket
from kitty.fast_data_types import ChildMonitor, send_data_to_peer
cm = ChildMonitor(lambda *a: None, None)
a, a_remote = socket.socketpair()
b, b_remote = socket.socketpair()
big = os.urandom(8 * 1024 * 1024)
pa = cm.inject_peer(os.dup(a.fileno()))
send_data_to_peer(pa, big)
pb = cm.inject_peer(os.dup(b.fileno()))
send_data_to_peer(pb, b'hello')
b_remote.settimeout(10)
assert b_remote.recv(16) == b'hello'
a_remote.settimeout(10)
got = bytearray()
while len(got) < len(big):
    got += a_remote.recv(1024 * 1024)
assert got == big, 'stalled peer response corrupted'
print('ok', flush=True)
os._exit(0)
"""


class TestTalkThread(BaseTest):
    def test_talk_thread_not_blocked_by_stalled_peer(self):
        p = subprocess.run([kitty_exe(), '+runpy', TALK_THREAD_STALLED_PEER], capture_output=True, text=True, timeout=30)
        self.assertEqual(p.returncode, 0, p.stderr)
        self.assertIn('ok', p.stdout, p.stderr)

    def test_talk_thread_survives_accept_errors(self):
        with tempfile.TemporaryDirectory() as tdir:
            p = subprocess.run([kitty_exe(), '+runpy', TALK_THREAD_SURVIVES_ACCEPT_ERROR, os.path.join(tdir, 's')], capture_output=True, text=True, timeout=30)
        self.assertEqual(p.returncode, 0, p.stderr)
        self.assertIn('ok', p.stdout, p.stderr)
        self.assertEqual(p.stderr.count('accept() on talk socket failed'), 1, p.stderr)
