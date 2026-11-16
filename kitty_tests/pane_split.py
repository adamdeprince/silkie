#!/usr/bin/env python
# License: GPLv3 Copyright: 2026, Silkie contributors

from functools import partial
from types import SimpleNamespace
from unittest.mock import Mock

from kitty.layout.interface import Splits, Stack
from kitty.tabs import Tab

from .base import BaseTest
from .layout import Window, create_layout, create_windows


class TestPaneSplit(BaseTest):
    def test_vertical_splits_switch_layout_and_preserve_existing_tree(self):
        self.set_options({'tab_bar_style': 'hidden'})
        for enabled in (['stack'], ['stack', 'splits:axis=vertical', 'splits:axis=horizontal']):
            with self.subTest(enabled=enabled):
                layout = create_layout(Stack)
                windows = create_windows(layout, num=1)
                tab = SimpleNamespace(
                    current_layout=layout, windows=windows, enabled_layouts=enabled.copy(),
                    _current_layout_name='stack', _used_layouts={}, mark_tab_bar_dirty=Mock(), relayout=Mock(),
                    create_layout_object=lambda name: create_layout(Splits),
                )
                tab._set_current_layout = partial(Tab._set_current_layout, tab)
                tab.goto_layout = partial(Tab.goto_layout, tab)
                tab.relayout = lambda: tab.current_layout(windows)

                def new_window(location):
                    tab.current_layout.add_window(windows, Window(len(windows) + 1), location=location)

                tab.new_window = new_window
                Tab.split_pane_vertically(tab)
                root = tab.current_layout.pairs_root
                self.assertTrue(root.horizontal)
                self.ae((root.one, root.two), (windows.groups[0].id, windows.groups[1].id))
                root.bias = 0.3
                # Each new split must retain the existing tree and divide the active pane.
                for _ in range(64):
                    Tab.split_pane_vertically(tab)
                    self.assertIs(tab.current_layout.pairs_root, root)
                    self.ae(root.bias, 0.3)
                    pair = root.pair_for_window(windows.active_group.id)
                    self.assertTrue(pair.horizontal)
                    self.ae(pair.two, windows.active_group.id)
                self.ae(len(windows), 66)
                self.ae(tab.enabled_layouts.count('splits'), 1 if enabled == ['stack'] else 0)
                self.ae(tab._current_layout_name, 'splits' if enabled == ['stack'] else enabled[1])
