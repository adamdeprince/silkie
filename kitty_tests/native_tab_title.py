#!/usr/bin/env python
# License: GPLv3 Copyright: 2026, Adam DePrince

from types import SimpleNamespace
from unittest import mock

from kitty.tabs import Tab

from .base import BaseTest


def make_tab(native_tabs: bool) -> Tab:
    tab = Tab.__new__(Tab)
    tab.name = ''
    tab.os_window_id = 7
    tab.mark_tab_bar_dirty = mock.Mock()  # type: ignore[method-assign]
    tm = SimpleNamespace(native_tabs=native_tabs)
    tab.tab_manager_ref = lambda: tm  # type: ignore[assignment]
    return tab


class TestNativeTabTitle(BaseTest):
    # A native tab is an OS window of its own and its tab title is the title of
    # that OS window, so a tab title is set as an override of the OS window
    # title. set_os_window_title() with an empty title removes the override:
    # the OS window, and so the tab, shows the title of its active window again
    # and follows that window's later title changes.

    def test_tab_title_overrides_the_os_window_title(self):
        tab = make_tab(native_tabs=True)
        with mock.patch('kitty.tabs.set_os_window_title') as set_title:
            tab.set_title('build')
        self.ae(tab.name, 'build')
        set_title.assert_called_once_with(7, 'build')

    def test_empty_tab_title_restores_the_default(self):
        # set_tab_title "", kitten @ set-tab-title "" and ConEmu OSC 9;3 with no text
        for empty in ('', None):
            tab = make_tab(native_tabs=True)
            with mock.patch('kitty.tabs.set_os_window_title') as set_title:
                tab.set_title('build')
                tab.set_title(empty)  # type: ignore[arg-type]
            self.ae(tab.name, '')
            self.ae(set_title.call_args_list, [mock.call(7, 'build'), mock.call(7, '')])

    def test_tab_bar_tabs_leave_the_os_window_title(self):
        tab = make_tab(native_tabs=False)
        with mock.patch('kitty.tabs.set_os_window_title') as set_title:
            tab.set_title('build')
            tab.set_title('')
        set_title.assert_not_called()
        self.ae(tab.mark_tab_bar_dirty.call_count, 2)
