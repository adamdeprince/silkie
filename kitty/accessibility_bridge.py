# License: GPLv3

import json
import os
import subprocess
import sys
from collections import deque
from threading import Condition, Thread
from typing import TYPE_CHECKING, Any

from . import fast_data_types as fdt
from .accessibility import pane_text
from .constants import is_macos, kitten_exe

if TYPE_CHECKING:
    from .boss import Boss
    from .window import Window


class AtspiBridge:
    def __init__(self) -> None:
        self.ready = False
        self.pending: list[dict[str, Any]] | None = None
        self.announcements: deque[dict[str, Any]] = deque()
        self.condition = Condition()
        self.closed = False
        self.process = subprocess.Popen([kitten_exe(), '__atspi__'], stdin=subprocess.PIPE, stdout=subprocess.PIPE)
        Thread(target=self.run, name='Silkie AT-SPI', daemon=True).start()

    def run(self) -> None:
        process = self.process
        assert process.stdout is not None and process.stdin is not None
        try:
            if process.stdout.readline() != b'ready\n':
                return
            self.ready = True
            while True:
                with self.condition:
                    self.condition.wait_for(lambda: self.closed or self.pending is not None or self.announcements)
                    if self.closed:
                        return
                    windows, self.pending = self.pending, None
                    announcements, self.announcements = self.announcements, deque()
                if windows is not None:
                    process.stdin.write(json.dumps({'windows': windows}).encode('ascii') + b'\n')
                for announcement in announcements:
                    process.stdin.write(json.dumps({'announcement': announcement}).encode('ascii') + b'\n')
                process.stdin.flush()
        except (BrokenPipeError, OSError):
            pass
        finally:
            self.ready = False
            try:
                process.stdin.close()
            except OSError:
                pass
            process.stdout.close()
            process.wait()

    def sync(self, windows: list[dict[str, Any]]) -> None:
        with self.condition:
            self.pending = windows
            self.condition.notify()

    def announce(self, pane: int, text: str, assertive: bool) -> None:
        with self.condition:
            self.announcements.append(dict(pane=pane, text=text, assertive=assertive))
            self.condition.notify()

    def close(self) -> None:
        with self.condition:
            self.closed = True
            self.condition.notify()
        if self.process.poll() is None:
            self.process.terminate()


class AccessibilityBridge:
    def __init__(self, boss: 'Boss') -> None:
        self.boss = boss
        self.previous: list[dict[str, Any]] = []
        self.atspi = None
        self.timer = None
        if sys.platform.startswith('linux') and (os.environ.get('DBUS_SESSION_BUS_ADDRESS') or os.environ.get('AT_SPI_BUS_ADDRESS')):
            try:
                self.atspi = AtspiBridge()
            except OSError:
                pass
        if is_macos or self.atspi is not None:
            self.timer = fdt.add_timer(self.sync, 0.1)

    def close(self) -> None:
        if self.timer is not None:
            fdt.remove_timer(self.timer)
            self.timer = None
        if self.atspi is not None:
            self.atspi.close()

    def sync(self, _timer_id: int | None = None, *, force: bool = False) -> None:
        if not force and (not fdt.cocoa_accessibility_active() if is_macos else not (self.atspi and self.atspi.ready)):
            return
        windows: list[dict[str, Any]] = []
        for os_window_id, manager in self.boss.os_window_map.items():
            tab = manager.active_tab
            if tab is None:
                windows.append(dict(id=os_window_id, title='Silkie', focused=False, origin=(0, 0), panes=[]))
                continue
            origin = (0, 0)
            if not is_macos and not os.environ.get('WAYLAND_DISPLAY'):
                origin = fdt.get_os_window_pos(os_window_id)
            panes = []
            for window in tab:
                if window.destroyed or not window.is_visible_in_layout:
                    continue
                snapshot = pane_text(window.screen, window.accessibility_annotations)
                roles = [dict(start=start, end=end, name=role) for start, end, role in snapshot['roles']]
                panes.append(dict(snapshot, roles=roles, id=window.id, title=window.title,
                                  focused=window.is_focused, frame=window.accessibility_frame))
            windows.append(dict(id=os_window_id, title=tab.title or 'Silkie', focused=any(p['focused'] for p in panes),
                                origin=origin, panes=panes))
        if windows == self.previous:
            return
        if is_macos:
            previous = {w['id']: w['panes'] for w in self.previous}
            for data in windows:
                if previous.get(data['id']) != data['panes']:
                    fdt.cocoa_accessibility_update(data['id'], data['panes'])
        elif self.atspi is not None:
            self.atspi.sync(windows)
        self.previous = windows

    def announce(self, window: 'Window', message: str, assertive: bool) -> str:
        if is_macos:
            fdt.cocoa_accessibility_announce(window.os_window_id, message, assertive)
        elif self.atspi is not None and self.atspi.ready:
            self.sync(force=True)
            self.atspi.announce(window.id, message, assertive)
        else:
            return 'ENOTSUP'
        return 'OK'
