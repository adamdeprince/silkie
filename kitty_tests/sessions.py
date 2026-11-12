#!/usr/bin/env python
# License: GPL v3 Copyright: 2026, Kovid Goyal <kovid at kovidgoyal.net>

from collections.abc import Callable, Iterator
from pathlib import Path
from tempfile import TemporaryDirectory
from typing import cast
from unittest.mock import Mock, patch

from kitty.session import save_as_session

from .base import BaseTest


class TestSessions(BaseTest):
    def test_save_as_session_nothing_to_save(self) -> None:
        boss = Mock(active_session='work')
        serialized: tuple[str, ...] = ('launch',)
        matched: tuple[str, ...] = ()
        boss.serialize_state_as_session.side_effect = lambda *a: iter(serialized)
        boss.match_windows.side_effect = lambda *a: iter(matched)

        def save(*args: str) -> None:
            for m in (boss.show_error, boss.match_windows, boss.get_save_filepath):
                m.reset_mock()
            save_as_session(boss, args)

        def assert_error(*fragments: str) -> None:
            self.ae(boss.show_error.call_count, 1)
            title, msg = boss.show_error.call_args.args
            self.ae(title, 'No windows to save')
            for frag in fragments:
                self.assertIn(frag, msg)

        match = '--match=title:^editor$'
        with TemporaryDirectory() as tmp:
            existing = Path(tmp, 'work.kitty-session')
            new = Path(tmp, 'new', 'work.kitty-session')
            with patch.dict('kitty.session.seen_session_paths', work=str(existing)):
                with self.subTest(destination='active session'):
                    save('--save-only', match, '.')
                    self.ae(existing.read_bytes(), b'launch')
                    boss.show_error.assert_not_called()
                with self.subTest(destination='prompt for path, windows closed before answer'):
                    save(match)
                    boss.get_save_filepath.assert_called_once()
                    callback = boss.get_save_filepath.call_args.args[1]
                    serialized = ()
                    callback(str(new))
                    self.assertFalse(new.parent.exists())
                    assert_error('No windows match: title:^editor$')
                with self.subTest(destination='active session, no windows match'):
                    save(match, '.')
                    self.ae(existing.read_bytes(), b'launch')
                    assert_error('No windows match: title:^editor$')
                    boss.match_windows.assert_called_with('title:^editor$')
                matched = ('window',)
                with self.subTest(destination='active session, matched windows not saveable'):
                    save(match, '.')
                    self.ae(existing.read_bytes(), b'launch')
                    assert_error('None of the matching windows')
                with self.subTest(destination='active session, no --match'):
                    save('--save-only', '.')
                    self.ae(existing.read_bytes(), b'launch')
                    assert_error('None of the windows')
                    boss.match_windows.assert_not_called()
                with self.subTest(destination='new file'):
                    save('--save-only', match, str(new))
                    self.assertFalse(new.parent.exists())
                    assert_error('None of the matching windows')
                with self.subTest(destination='prompt for path, nothing to save'):
                    save(match)
                    boss.get_save_filepath.assert_not_called()
                    assert_error('None of the matching windows')
        boss.edit_file.assert_not_called()

    def test_moved_windows_and_tabs_keep_session(self) -> None:
        # Drives the real Boss move methods and Tab detach/attach/take over code over fake OS Windows
        from contextlib import nullcontext
        from itertools import count

        from kitty.boss import Boss
        from kitty.layout.interface import Tall
        from kitty.tabs import Tab
        from kitty.window import Window as RealWindow
        from kitty.window_list import WindowList, reset_group_id_counter

        from .layout import Window as LayoutWindow
        from .layout import create_layout

        ids = count(1)

        class FakeWindow(LayoutWindow):
            effective_session_name = RealWindow.effective_session_name

            def __init__(self, session: str) -> None:
                super().__init__(next(ids))
                self.created_in_session_name = session
                self.tabref: Callable[[], FakeTab | None] = lambda: None

            def change_tab(self, tab: 'FakeTab') -> None:
                self.tabref = lambda: tab

        class FakeTab:
            name = cwd = ''
            enabled_layouts, _last_used_layout, _current_layout_name = ['tall'], None, ''

            def __init__(self, tm: 'FakeTabManager', session: str, *windows: FakeWindow) -> None:
                self.id, self.os_window_id, self.created_in_session_name = next(ids), tm.os_window_id, session
                self.tab_manager_ref = lambda: tm
                self.current_layout = create_layout(Tall)
                self.windows = WindowList(cast(Tab, self))
                real = 'remove_window detach_window _add_window _take_ownership_of_window attach_window attach_window_at_edge attach_windows set_active_window'
                for name in real.split():
                    setattr(self, name, getattr(Tab, name).__get__(self))
                for w in windows:
                    w.change_tab(self)
                    self._add_window(w)
                tm.tabs.append(self)

            def __len__(self) -> int:
                return len(self.windows)

            def __iter__(self) -> Iterator[FakeWindow]:
                return iter(cast(Iterator[FakeWindow], self.windows))

            def _add_window(self, w: FakeWindow) -> None:  # replaced by the real method
                pass

            def take_over_from(self, other: 'FakeTab') -> None:
                Tab.take_over_from(cast(Tab, self), cast(Tab, other))

            def mark_tab_bar_dirty(self) -> None:
                pass

            relayout = post_window_removal_update = active_window_changed = make_active = destroy = mark_tab_bar_dirty

        class FakeTabManager:
            def __init__(self, os_window_id: int, session: str = '') -> None:
                self.os_window_id, self.created_in_session_name = os_window_id, session
                self.tabs: list[FakeTab] = []

            def __len__(self) -> int:
                return len(self.tabs)

            def new_tab(self, empty_tab: bool = False, location: str = 'last', title: str = '') -> FakeTab:
                return FakeTab(self, '')

            def tab_at_location(self, location: str) -> FakeTab | None:
                return next((t for t in self.tabs if t.name == location), None)

            def remove(self, tab: FakeTab) -> None:
                self.tabs.remove(tab)

        class FakeBoss:
            shutting_down = False
            suppress_focus_change_events = nullcontext

            def __init__(self) -> None:
                self.os_window_map: dict[int, FakeTabManager] = {}
                real = '_move_window_to _move_tab_to _insert_window_in_direction _new_os_window_for_session _cleanup_tab_after_window_removal'
                for name in real.split():
                    setattr(self, name, getattr(Boss, name).__get__(self))

            def add_os_window(self) -> int:
                os_window_id = len(self.os_window_map) + 1
                self.os_window_map[os_window_id] = FakeTabManager(os_window_id)
                return os_window_id

            @property
            def all_tabs(self) -> Iterator[FakeTab]:
                for tm in self.os_window_map.values():
                    yield from tm.tabs

            def mark_os_window_for_close(self, os_window_id: int) -> None:
                pass

        def world() -> tuple[FakeBoss, FakeWindow, FakeWindow, FakeTab, FakeTab]:
            # Tab a of session a has a window of no session and one of session b, next to it is tab other of session c,
            # tab b of session b is in another OS Window
            boss = FakeBoss()
            reset_group_id_counter()
            for os_window_id, session in ((1, 'a'), (2, 'b')):
                boss.os_window_map[os_window_id] = FakeTabManager(os_window_id, session)
            unnamed, from_b = FakeWindow(''), FakeWindow('b')
            tab_a = FakeTab(boss.os_window_map[1], 'a', unnamed, from_b, FakeWindow(''))
            FakeTab(boss.os_window_map[1], 'c', FakeWindow('')).name = 'other'
            tab_b = FakeTab(boss.os_window_map[2], 'b', FakeWindow('b'))
            return boss, unnamed, from_b, tab_a, tab_b

        def sessions(w: FakeWindow) -> tuple[str, str, str]:
            tab = w.tabref()
            assert tab is not None
            tm = tab.tab_manager_ref()
            return w.effective_session_name, tab.created_in_session_name, tm.created_in_session_name

        patches = (patch('kitty.tabs.detach_window'), patch('kitty.tabs.attach_window'), patch('kitty.boss.current_os_window', return_value=1))
        with patches[0], patches[1], patches[2]:
            # A window keeps its own session, which may be none, wherever it goes. New tabs and OS Windows made for it
            # belong to its effective session, a window of no session in an existing tab belongs to that tab's session.
            # (window, target_tab_id, target_os_window_id) -> (window session, its tab's session, its OS Window's session)
            for which, target_tab_id, target_os_window_id, expected in (
                ('unnamed', 'new', None, ('a', 'a', 'a')),
                ('from_b', 'new', None, ('b', 'b', 'a')),
                ('unnamed', 'left', None, ('a', 'a', 'a')),  # no tab there, so a new one
                ('unnamed', 'other', None, ('c', 'c', 'a')),  # the existing tab there
                ('from_b', 'other', None, ('b', 'c', 'a')),
                ('unnamed', None, 'new', ('a', 'a', 'a')),
                ('from_b', None, 'new', ('b', 'b', 'b')),
                ('unnamed', 'tab_b', None, ('b', 'b', 'b')),
            ):
                boss, unnamed, from_b, tab_a, tab_b = world()
                w = unnamed if which == 'unnamed' else from_b
                own_session = w.created_in_session_name
                tid = tab_b.id if target_tab_id == 'tab_b' else target_tab_id
                cast(Boss, boss)._move_window_to(cast(RealWindow, w), tid, target_os_window_id)
                self.ae(sessions(w), expected, f'{which} {target_tab_id} {target_os_window_id}')
                self.ae(w.created_in_session_name, own_session)

            # Dropped next to a window in a tab of another session, the same as moving into that tab
            boss, unnamed, from_b, tab_a, tab_b = world()
            cast(Boss, boss)._insert_window_in_direction(cast(RealWindow, unnamed), cast(RealWindow, next(iter(tab_b))), 'left')
            self.ae((sessions(unnamed), unnamed.created_in_session_name), (('b', 'b', 'b'), ''))

            # A tab moved to a new OS Window, or an existing one, keeps its session and that of its windows
            for target_os_window_id, expected in ((None, ('a', 'a', 'a')), (2, ('a', 'a', 'b'))):
                boss, unnamed, from_b, tab_a, tab_b = world()
                cast(Boss, boss)._move_tab_to(cast(Tab, tab_a), target_os_window_id)
                self.ae(sessions(unnamed), expected)
                self.ae(sessions(from_b)[0], 'b')
                self.assertNotIn(tab_a, boss.os_window_map[1].tabs)

            # A window of no session in a tab of no session stays in no session
            boss, unnamed, from_b, tab_a, tab_b = world()
            tab_a.created_in_session_name = boss.os_window_map[1].created_in_session_name = ''
            cast(Boss, boss)._move_window_to(cast(RealWindow, unnamed), 'new')
            self.ae(sessions(unnamed), ('', '', ''))
