#!/usr/bin/env python
# License: GPLv3 Copyright: 2026, Adam DePrince

from types import SimpleNamespace
from unittest import mock

from kitty.boss import Boss
from kitty.config import load_config
from kitty.fast_data_types import set_options
from kitty.native_ui import view_items

from .base import BaseTest


class TestFontSizeScope(BaseTest):
    # Every native tab is an OS window of its own, so changing the font size
    # of the current OS window, rather than of all of them, leaves the other
    # tabs as they are

    def test_default_shortcuts_change_the_current_os_window(self):
        from kitty.options.types import defaults

        definitions = [kd.definition for kd in defaults.map if kd.definition.startswith('change_font_size')]
        self.assertTrue(definitions)
        for d in definitions:
            self.ae(d.split()[1], 'current', d)

    def test_view_menu_changes_the_current_os_window(self):
        set_options(load_config())
        # the menu can depend on the active window, there is none here
        with mock.patch('kitty.native_ui.get_boss', return_value=SimpleNamespace(active_window=None), create=True):
            items = view_items({})
        actions = [item['action'] for item in items if item.get('action', '').startswith('change_font_size')]
        self.ae(actions, ['change_font_size current +2.0', 'change_font_size current -2.0', 'change_font_size current 0'])

    def test_changing_the_current_os_window_leaves_the_others(self):
        opts = load_config()
        set_options(opts)
        sizes = {1: opts.font_size, 2: opts.font_size}

        def os_window_font_size(os_window_id, new_size=None, force=False):
            if new_size is not None:
                sizes[os_window_id] = new_size
            return sizes.get(os_window_id, 0)

        boss = SimpleNamespace(
            os_window_map={1: SimpleNamespace(resize=mock.Mock()), 2: SimpleNamespace(resize=mock.Mock())},
            window_for_dispatch=None,
            active_window=SimpleNamespace(os_window_id=2),
        )
        boss._change_font_size = lambda sz_map: Boss._change_font_size(boss, sz_map)
        with mock.patch('kitty.boss.os_window_font_size', os_window_font_size), mock.patch('kitty.boss.global_font_size') as global_size:
            # what the 'change_font_size current +2.0' of the shortcuts and the menu does
            Boss.change_font_size(boss, False, '+', 2.0)
            self.ae(sizes, {1: opts.font_size, 2: opts.font_size + 2})
            global_size.assert_not_called()
