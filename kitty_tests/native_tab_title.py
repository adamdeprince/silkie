#!/usr/bin/env python
# License: GPLv3 Copyright: 2026, Adam DePrince

from contextlib import ExitStack
from types import SimpleNamespace
from unittest import mock

from kitty.config import load_config
from kitty.fast_data_types import set_options
from kitty.launch import parse_launch_args, tab_for_window
from kitty.profiles import Profile
from kitty.session import parse_session
from kitty.tabs import Tab

from .base import BaseTest


class OSWindowTitles:
    # What set_os_window_title() and the title updates of the render loop do to the title of an OS window
    # (kitty/state.c), which is the label of its native tab: a non-empty title overrides the title of the
    # active window, an empty one removes the override and shows the title of the active window again

    def __init__(self) -> None:
        self.window_titles: dict[int, str] = {}
        self.overrides: dict[int, str] = {}

    def set_os_window_title(self, os_window_id: int, title: str) -> None:
        if title:
            self.overrides[os_window_id] = title
        else:
            self.overrides.pop(os_window_id, None)

    def label(self, os_window_id: int) -> str:
        return self.overrides.get(os_window_id) or self.window_titles.get(os_window_id, '')


class FakeTabManager(SimpleNamespace):
    # Tabs keep a weak reference to their tab manager
    pass


class TestNativeTabTitle(BaseTest):
    # A native tab is an OS window of its own. Its label follows the rule of the tab bar: the tab name when
    # one is set, otherwise the title of the active window, followed as it changes.

    def setUp(self) -> None:
        super().setUp()
        self.opts = load_config()
        set_options(self.opts)
        self.titles = OSWindowTitles()
        self.os_window_ids = iter(range(1, 1000))
        self.tab_managers: list[FakeTabManager] = []
        self.stack = ExitStack()
        self.addCleanup(self.stack.close)
        self.stack.enter_context(mock.patch('kitty.tabs.set_os_window_title', self.titles.set_os_window_title))
        self.stack.enter_context(mock.patch('kitty.tabs.add_tab', return_value=1))
        self.stack.enter_context(mock.patch('kitty.tabs.Borders'))
        # windows are not created here, their titles are set with window_title()
        self.stack.enter_context(mock.patch.object(Tab, 'startup'))
        self.stack.enter_context(mock.patch.object(Tab, 'new_window'))
        self.stack.enter_context(mock.patch.object(Tab, '_set_current_layout'))

    def tab_manager(self, native_tabs: bool = True) -> FakeTabManager:
        tm = FakeTabManager(
            os_window_id=next(self.os_window_ids), native_tabs=native_tabs, args=SimpleNamespace(directory=None),
            profile=Profile(id='', name=''), mark_tab_bar_dirty=mock.Mock(), os_window_docks=[],
        )
        self.tab_managers.append(tm)  # the boss holds them, tabs only have weak references
        return tm

    def window_title(self, tab: Tab, title: str) -> None:
        self.titles.window_titles[tab.os_window_id] = title

    def session_tabs(self, text: str, native_tabs: bool = True) -> list[Tab]:
        session = next(parse_session(text, self.opts))
        return [Tab(self.tab_manager(native_tabs), session_tab=t) for t in session.tabs]  # type: ignore[arg-type]

    def test_session_named_tab_shows_its_name(self):
        named, unnamed = self.session_tabs('new_tab alpha\nlaunch sh\nnew_tab\nlaunch sh\n')
        for tab in (named, unnamed):
            self.window_title(tab, 'sh')
        self.ae(named.name, 'alpha')
        self.ae(self.titles.label(named.os_window_id), 'alpha')
        self.ae(self.titles.label(unnamed.os_window_id), 'sh')
        # the name stays while the window title changes, the unnamed tab follows it
        for tab in (named, unnamed):
            self.window_title(tab, 'vim')
        self.ae(self.titles.label(named.os_window_id), 'alpha')
        self.ae(self.titles.label(unnamed.os_window_id), 'vim')

    def test_launch_named_tab_shows_its_name(self):
        tm = self.tab_manager()

        def new_tab(**kw):
            # like TabManager.new_tab(), which puts every new native tab into an OS window of its own
            tab = Tab(self.tab_manager(), no_initial_window=True)  # type: ignore[arg-type]
            self.window_title(tab, 'sh')
            # launch passes --tab-title on to TabManager.new_tab(), which sets it
            if kw.get('title'):
                tab.set_title(kw['title'])
            return tab

        tm.new_tab = new_tab
        boss = SimpleNamespace(active_tab_manager=tm)
        opts, _ = parse_launch_args(['--type=tab', '--tab-title', 'launched', 'sh'])
        tab = tab_for_window(boss, opts, None, None, '')  # type: ignore[arg-type]
        self.ae(self.titles.label(tab.os_window_id), 'launched')
        opts, _ = parse_launch_args(['--type=tab', 'sh'])
        tab = tab_for_window(boss, opts, None, None, '')  # type: ignore[arg-type]
        self.ae(self.titles.label(tab.os_window_id), 'sh')

    def test_clearing_the_name_follows_the_window_title_again(self):
        # set_tab_title "", kitten @ set-tab-title "" and ConEmu OSC 9;3 with no text
        for empty in ('', None):
            (tab,) = self.session_tabs('new_tab alpha\nlaunch sh\n')
            self.window_title(tab, 'sh')
            tab.set_title(empty)  # type: ignore[arg-type]
            self.ae(tab.name, '')
            self.ae(self.titles.label(tab.os_window_id), 'sh')
            self.window_title(tab, 'vim')
            self.ae(self.titles.label(tab.os_window_id), 'vim')
            tab.set_title('beta')
            self.ae(self.titles.label(tab.os_window_id), 'beta')

    def test_unnamed_tab_leaves_an_os_window_title_override(self):
        # An unnamed tab must not clear a title given to its OS window, for example with --title
        tm = self.tab_manager()
        self.titles.set_os_window_title(tm.os_window_id, 'from --title')
        Tab(tm, no_initial_window=True)  # type: ignore[arg-type]
        self.ae(self.titles.label(tm.os_window_id), 'from --title')

    def test_tab_bar_tabs_leave_the_os_window_title(self):
        with mock.patch('kitty.tabs.set_os_window_title') as set_title:
            (tab,) = self.session_tabs('new_tab alpha\nlaunch sh\n', native_tabs=False)
            tab.set_title('beta')
            tab.set_title('')
        set_title.assert_not_called()
