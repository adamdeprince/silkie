#!/usr/bin/env python
# License: GPLv3 Copyright: 2026, Adam DePrince

from base64 import standard_b64encode
from types import SimpleNamespace
from unittest.mock import patch

from kitty.program_status import ProgramStatus, parse_report

from .base import BaseTest


def b64(text: str) -> str:
    return standard_b64encode(text.encode()).decode()


def window(*bodies: str) -> SimpleNamespace:
    ps = ProgramStatus()
    for b in bodies:
        ps.apply(parse_report(b))
    return SimpleNamespace(program_status=ps)


class FakeTab(list):
    active_window = None


def report(w: SimpleNamespace, body: str) -> None:
    w.program_status.apply(parse_report(body))


class TestNativeTabStatus(BaseTest):

    def setUp(self) -> None:
        super().setUp()
        self.pushed: list[tuple[int, dict | None]] = []
        self.opts = SimpleNamespace(native_tab_status_badge=True)
        self.patches = (
            patch('kitty.native_ui.native_tab_status', side_effect=lambda os_window_id, model: self.pushed.append((os_window_id, model)) or True),
            patch('kitty.native_ui.get_options', return_value=self.opts),
        )
        for p in self.patches:
            p.start()

    def tearDown(self) -> None:
        for p in self.patches:
            p.stop()
        super().tearDown()

    def update(self, tm: SimpleNamespace) -> dict | None:
        from kitty.native_ui import update_native_tab_status
        update_native_tab_status(tm)  # type: ignore
        return tm.native_tab_status_shown

    def test_native_tab_status_combines_windows(self) -> None:
        # A native tab is a tab manager of its own, normally with one tab, whose windows are combined
        a, b, c = window(), window(), window()
        tm = SimpleNamespace(os_window_id=7, tabs=[[a, b], [c]], native_tab_status_shown=None)
        self.ae(self.update(tm), None)
        self.ae(self.pushed, [], 'nothing is sent while there is no status to show')

        report(a, 'state=idle:app=sh')
        self.ae(self.update(tm), None)
        self.ae(self.pushed, [])

        report(a, f'state=working:progress=40:app=make:title={b64("Building")}')
        m = self.update(tm)
        self.ae(self.pushed, [(7, m)])
        assert m is not None
        self.ae((m['state'], m['kind'], m['progress'], m['count'], m['text']), ('working', '', 40, 1, '40%'))
        self.ae(m['accessibility'], 'make: Working, 40%')
        self.ae(m['tooltip'], 'make — Building\nWorking, 40%')

        # the same status again is not sent again
        self.update(tm)
        self.ae(len(self.pushed), 1)

        # more urgent states win, whichever window and tab they are in
        report(c, f'state=done:msg={b64("All tests passed")}')
        m = self.update(tm)
        assert m is not None
        self.ae((m['state'], m['progress'], m['text']), ('done', -1, ''))
        self.ae(m['tooltip'], 'Done\nAll tests passed')
        self.ae(m['accessibility'], 'Program status: Done')
        report(b, 'state=error:app=cargo')
        m = self.update(tm)
        assert m is not None
        self.ae((m['state'], m['accessibility']), ('error', 'cargo: Failed'))
        report(c, 'state=blocked:kind=question:app=claude')
        m = self.update(tm)
        assert m is not None
        self.ae((m['state'], m['kind'], m['accessibility']), ('blocked', 'question', 'claude: Waiting for an answer'))

        # windows in the most urgent state are counted, several records in a window count once
        report(a, 'state=blocked:kind=permission:id=sub')
        m = self.update(tm)
        assert m is not None
        self.ae((m['state'], m['kind'], m['count'], m['text']), ('blocked', 'permission', 2, '×2'))
        self.ae(m['accessibility'], 'make: Waiting for permission (2 windows)')

        # progress is shown for working only, along with the count
        tm2 = SimpleNamespace(os_window_id=8, tabs=[[window('state=working:progress=5'), window('state=working:progress=70')]], native_tab_status_shown=None)
        m = self.update(tm2)
        assert m is not None
        self.ae((m['progress'], m['text']), (70, '70% ×2'))
        m = self.update(SimpleNamespace(os_window_id=9, tabs=[[window('state=blocked:progress=50')]], native_tab_status_shown=None))
        assert m is not None
        self.ae((m['kind'], m['progress'], m['text'], m['accessibility']), ('', -1, '', 'Program status: Waiting for you, 50%'))

    def test_native_tab_status_follows_record_lifetimes(self) -> None:
        a, b = window('state=working'), window('state=done')
        tm = SimpleNamespace(os_window_id=3, tabs=[[a, b]], native_tab_status_shown=None)
        self.ae(self.update(tm)['state'], 'done')  # type: ignore
        # the user saw the tab: done and error records go, working remains
        b.program_status.on_seen_by_user()
        self.ae(self.update(tm)['state'], 'working')  # type: ignore
        # a new shell prompt drops the working record, which removes the badge
        a.program_status.on_process_exit_or_prompt()
        self.ae(self.update(tm), None)
        self.ae(self.pushed[-1], (3, None))

        # a window that moves to another native tab takes its status with it
        report(a, 'state=error')
        other = SimpleNamespace(os_window_id=4, tabs=[[]], native_tab_status_shown=None)
        self.ae(self.update(tm)['state'], 'error')  # type: ignore
        tm.tabs[0].remove(a)
        other.tabs[0].append(a)
        self.ae(self.update(tm), None)
        self.ae(self.update(other)['state'], 'error')  # type: ignore
        self.ae(self.pushed[-2:], [(3, None), (4, other.native_tab_status_shown)])

        # a closed window takes its status away
        other.tabs[0].clear()
        self.ae(self.update(other), None)

        # a tab manager that is being destroyed has no tabs and sends nothing
        n = len(self.pushed)
        gone = SimpleNamespace(os_window_id=5, native_tab_status_shown={'state': 'done'})
        self.update(gone)
        self.ae(len(self.pushed), n)

    def test_native_tab_status_option(self) -> None:
        tm = SimpleNamespace(os_window_id=2, tabs=[[window('state=error')]], native_tab_status_shown=None)
        self.ae(self.update(tm)['state'], 'error')  # type: ignore
        self.opts.native_tab_status_badge = False
        self.ae(self.update(tm), None)
        self.ae(self.pushed[-1], (2, None))
        self.opts.native_tab_status_badge = True
        self.ae(self.update(tm)['state'], 'error')  # type: ignore

    def test_tab_manager_pushes_native_tab_status(self) -> None:
        from kitty.tabs import TabManager
        w = window('state=working')
        tm = TabManager.__new__(TabManager)
        tm.os_window_id, tm.tabs, tm._active_tab_idx, tm.tab_bar_hidden = 11, [], 0, True
        tm.native_tabs = False
        with patch('kitty.tabs.mark_tab_bar_dirty'), patch('kitty.tabs.global_watchers', return_value=SimpleNamespace(on_tab_bar_dirty=())):
            tm.tabs = [FakeTab([w])]  # type: ignore
            tm.mark_tab_bar_dirty()
            self.ae(self.pushed, [], 'the tab bar drawn by the terminal shows {tab.program_status} instead')
            tm.native_tabs = True
            tm.mark_tab_bar_dirty()
            self.ae([(i, m['state'] if m else None) for i, m in self.pushed], [(11, 'working')])
