#!/usr/bin/env python
# License: GPLv3 Copyright: 2026, Adam DePrince

from types import SimpleNamespace
from unittest.mock import patch

from kitty.dock_status import NO_STATUS, DockStatus, DockStatusController, dock_status
from kitty.program_status import ProgramStatus, parse_report

from .base import BaseTest


def window(wid: int, *bodies: str, os_window_id: int = 1) -> SimpleNamespace:
    ps = ProgramStatus()
    for b in bodies:
        ps.apply(parse_report(b))
    return SimpleNamespace(id=wid, os_window_id=os_window_id, program_status=ps)


def report(w: SimpleNamespace, body: str) -> None:
    w.program_status.apply(parse_report(body))


class TestDockStatus(BaseTest):

    def test_dock_status_summary(self) -> None:
        self.ae(dock_status([]), NO_STATUS)
        self.ae(dock_status([window(1), window(2, 'state=idle'), window(3, 'state=working:progress=50')]), NO_STATUS)
        # failed windows are urgent, finished ones are not
        self.ae(dock_status([window(1, 'state=done')]), DockStatus('\u2713', 1))
        self.ae(dock_status([window(1, 'state=done'), window(2, 'state=done')]), DockStatus('2\u2713', 2))
        self.ae(dock_status([window(1, 'state=done'), window(2, 'state=error')]), DockStatus('\u2715', 1, True))
        self.ae(dock_status([window(1, 'state=error'), window(2, 'state=error'), window(3, 'state=done')]), DockStatus('2\u2715', 2, True))
        a, b, c = window(1, 'state=blocked:kind=question'), window(2, 'state=blocked'), window(3, 'state=error')
        self.ae(dock_status([a, b, c]), DockStatus('2', 2, True, frozenset((1, 2))))
        # the window the user is looking at does not count, but is still known to be blocked
        self.ae(dock_status([a, b, c], looking_at=a), DockStatus('1', 1, True, frozenset((1, 2))))
        self.ae(dock_status([a, c], looking_at=a), DockStatus('\u2715', 1, True, frozenset((1,))))
        self.ae(dock_status([a], looking_at=a), DockStatus(blocked=frozenset((1,))))
        # a blocked record in a window counts that window once, whatever else is in it
        self.ae(dock_status([window(1, 'state=working', 'state=blocked:id=x', 'state=done:id=y')]), DockStatus('1', 1, True, frozenset((1,))))


class TestDockStatusController(BaseTest):

    def setUp(self) -> None:
        super().setUp()
        self.windows: list[SimpleNamespace] = []
        self.active: SimpleNamespace | None = None
        self.focused_os_window = 0
        self.shown: list[DockStatus] = []
        self.attention: list[int] = []
        self.timers: list = []
        self.opts = SimpleNamespace(dock_status_badge=True, dock_attention_on_blocked=False)
        # The test case stands in for Boss, see the properties below
        self.patches = [
            patch('kitty.dock_status.get_boss', return_value=self),
            patch('kitty.dock_status.get_options', return_value=self.opts),
            patch('kitty.dock_status.current_focused_os_window_id', side_effect=lambda: self.focused_os_window),
            patch('kitty.dock_status.show_dock_status', side_effect=self.shown.append),
            patch('kitty.dock_status.request_attention', side_effect=self.attention.append),
            patch('kitty.dock_status.add_timer', side_effect=lambda cb, delay, repeats: self.timers.append(cb) or len(self.timers)),
        ]
        for p in self.patches:
            p.start()
        self.c = DockStatusController()

    def tearDown(self) -> None:
        for p in self.patches:
            p.stop()
        super().tearDown()

    # The parts of Boss that the controller uses
    @property
    def all_windows(self) -> list[SimpleNamespace]:
        return self.windows

    @property
    def active_window(self) -> SimpleNamespace | None:
        return self.active

    @property
    def window_id_map(self) -> dict[int, SimpleNamespace]:
        return {w.id: w for w in self.windows}

    def test_dock_status_updates(self) -> None:
        a, b = window(1, os_window_id=10), window(2, os_window_id=20)
        self.windows = [a, b]
        self.active, self.focused_os_window = a, 10
        self.c.update()
        self.ae(self.shown, [], 'nothing is sent while there is nothing to show')

        # a program in another window finishes while the user looks at a
        report(b, 'state=done')
        self.c.update()
        self.ae([s.label for s in self.shown], ['\u2713'])
        self.c.update()
        self.ae(len(self.shown), 1, 'the same status is not sent again')

        # the user looks at b: its record is in plain sight and is then seen, which removes it
        self.active, self.focused_os_window = b, 20
        self.c.update()
        self.ae(self.shown[-1], NO_STATUS)
        b.program_status.on_seen_by_user()
        self.c.update()
        self.ae(len(self.shown), 2)

        # a program blocks in the window the user looks at: nothing on the Dock until Silkie goes to the background
        report(b, 'state=blocked:kind=permission')
        self.c.update()
        self.ae(len(self.shown), 2)
        self.focused_os_window = 0
        self.c.update()
        self.ae(self.shown[-1][:3], ('1', 1, True))
        report(a, 'state=error')
        self.c.update()
        self.ae(self.shown[-1][:3], ('1', 1, True), 'blocked windows are more urgent than failed ones')
        # the blocked program carries on and its window closes, the failure in a remains
        report(b, 'state=working')
        self.windows.remove(b)
        self.c.update()
        self.ae(self.shown[-1][:3], ('\u2715', 1, True), 'failed windows are urgent too')
        self.ae(self.attention, [], 'attention is not requested unless asked for')

    def test_dock_status_option(self) -> None:
        self.windows = [window(1, 'state=error')]
        self.c.update()
        self.ae(self.shown[-1].label, '\u2715')
        self.opts.dock_status_badge = False
        self.c.update()
        self.ae(self.shown[-1][:3], ('', 0, False))
        self.opts.dock_status_badge = True
        self.c.update()
        self.ae(self.shown[-1].label, '\u2715')

    def test_dock_attention_on_blocked(self) -> None:
        self.opts.dock_attention_on_blocked = True
        self.opts.dock_status_badge = False
        a, b = window(1, os_window_id=10), window(2, os_window_id=20)
        self.windows = [a, b]
        # Silkie in front: no attention, even for a window the user is not looking at
        self.active, self.focused_os_window = a, 10
        report(b, 'state=blocked')
        self.c.update()
        self.ae(self.attention, [])
        # going to the background does not make b newly blocked
        self.focused_os_window = 0
        self.c.update()
        self.ae(self.attention, [])
        # a window that becomes blocked in the background bounces the icon once
        report(a, 'state=blocked:kind=question')
        self.c.update()
        self.c.update()
        self.ae(self.attention, [10])
        # and again only after it was unblocked and blocks again
        report(a, 'state=working')
        self.c.update()
        report(a, 'state=blocked:kind=question')
        self.c.update()
        self.ae(self.attention, [10, 10])
        self.ae(self.shown, [], 'the badge stays off')

    def test_dock_status_coalesces_updates(self) -> None:
        self.c.schedule_update()
        self.c.schedule_update()
        self.ae(len(self.timers), 1)
        self.timers[0](1)
        self.c.schedule_update()
        self.ae(len(self.timers), 2)

    def test_dock_status_platforms(self) -> None:
        from kitty import dock_status as ds
        for p in self.patches[3:4]:
            p.stop()
        try:
            calls: list = []
            with patch.object(ds, 'is_macos', True), patch('kitty.fast_data_types.cocoa_set_dock_status_badge', side_effect=calls.append, create=True):
                ds.show_dock_status(DockStatus('2', 2, True))
            self.ae(calls, ['2'])
            calls.clear()
            with patch.object(ds, 'is_macos', False), patch('kitty.fast_data_types.dbus_set_launcher_entry', side_effect=lambda *a: calls.append(a), create=True):
                ds.show_dock_status(DockStatus('2', 2, True))
                ds.show_dock_status(NO_STATUS)
            self.ae(calls, [('application://silkie.desktop', 2, True), ('application://silkie.desktop', 0, False)])
        finally:
            self.patches[3].start()
