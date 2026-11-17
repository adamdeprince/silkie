#!/usr/bin/env python
# License: GPLv3 Copyright: 2026, Adam DePrince

"""
The program status (OSC 7501) of all windows, shown on the icon of Silkie in the Dock on macOS
and in the launchers that support the Unity LauncherEntry D-Bus API on Linux (Ubuntu Dock, Dash to
Dock, KDE Plasma). It complements the badges on the tabs: it works when the tab bar is not visible,
for example in a window with a single tab, and when Silkie is not in front at all.

The window the user is looking at does not count: its status is in plain sight. That is the active
window of the focused OS window, and there is none while another application is in front.
"""

from collections.abc import Iterable
from typing import TYPE_CHECKING, NamedTuple

from .constants import app_class, is_macos
from .fast_data_types import add_timer, current_focused_os_window_id, get_boss, get_options, request_attention

if TYPE_CHECKING:
    from .window import Window


class DockStatus(NamedTuple):
    label: str = ''  # the text of the badge on macOS, empty for none
    count: int = 0  # the number of windows in the most urgent state, 0 for none
    urgent: bool = False  # a window is blocked on the user
    # the ids of the windows that are blocked on the user, including the one the user is looking at
    blocked: frozenset[int] = frozenset()


NO_STATUS = DockStatus()


def dock_status(windows: Iterable['Window'], looking_at: 'Window | None' = None) -> DockStatus:
    """
    The most urgent of: windows blocked on the user, shown as their number; windows whose program failed,
    shown as a cross; windows whose program finished, shown as a check mark, both with the number of windows
    if more than one. Working and idle windows are not shown: they need nothing from the user.
    """
    from .program_status import State
    blocked: set[int] = set()
    num_blocked = error = done = 0
    for w in windows:
        r = w.program_status.most_urgent()
        if r is None:
            continue
        if r.state is State.blocked:
            blocked.add(w.id)
        if w is looking_at:
            continue
        if r.state is State.blocked:
            num_blocked += 1
        elif r.state is State.error:
            error += 1
        elif r.state is State.done:
            done += 1
    if num_blocked:
        return DockStatus(str(num_blocked), num_blocked, True, frozenset(blocked))
    for n, sym in ((error, '\u2715'), (done, '\u2713')):
        if n:
            return DockStatus(f'{n}{sym}' if n > 1 else sym, n, blocked=frozenset(blocked))
    return DockStatus(blocked=frozenset(blocked))


def show_dock_status(s: DockStatus) -> None:
    if is_macos:
        from .fast_data_types import cocoa_set_dock_status_badge
        cocoa_set_dock_status_badge(s.label)
    else:
        from .fast_data_types import dbus_set_launcher_entry
        dbus_set_launcher_entry(f'application://{app_class}.desktop', s.count, s.urgent)


class DockStatusController:
    shown: DockStatus = NO_STATUS
    update_timer: int = 0

    def schedule_update(self) -> None:
        ' Several changes often come together, such as a focus change in one window and another, update once for all of them '
        if not self.update_timer:
            self.update_timer = add_timer(self.update, 0, False)

    def update(self, timer_id: int | None = None) -> None:
        self.update_timer = 0
        opts = get_options()
        boss = get_boss()
        s = NO_STATUS
        if opts.dock_status_badge or opts.dock_attention_on_blocked:
            looking_at = boss.active_window if current_focused_os_window_id() > 0 else None
            s = dock_status(boss.all_windows, looking_at)
        if opts.dock_attention_on_blocked and current_focused_os_window_id() <= 0:
            # Silkie is in the background: bounce its icon once for every window that has become blocked on the
            # user since the last update. A window that was already blocked when Silkie went to the background does not.
            for wid in s.blocked - self.shown.blocked:
                if (w := boss.window_id_map.get(wid)) is not None:
                    request_attention(w.os_window_id)
        if not opts.dock_status_badge:
            s = s._replace(label='', count=0, urgent=False)
        previous, self.shown = self.shown, s
        if s[:3] != previous[:3]:
            show_dock_status(s)


dock_status_controller = DockStatusController()


def schedule_dock_status_update() -> None:
    dock_status_controller.schedule_update()
