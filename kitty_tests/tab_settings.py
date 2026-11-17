#!/usr/bin/env python
# License: GPLv3 Copyright: 2026, Adam DePrince

from types import SimpleNamespace
from unittest.mock import MagicMock, patch

from kitty import tab_settings as ts

from .base import BaseTest


def profile(pid: str, name: str) -> SimpleNamespace:
    return SimpleNamespace(id=pid, name=name)


PROFILES = {'default': profile('default', 'Default'), 'work': profile('work', 'Work')}


class FakeTab(list):
    def __init__(self, name: str = '', title: str = 'vim', windows: tuple = ()) -> None:
        super().__init__(windows)
        self.name, self.title = name, title
        self.scrollback_settings = None
        self.titles: list[str] = []

    def set_title(self, title: str) -> None:
        self.name = title
        self.titles.append(title)

    def set_scrollback_settings(self, lines: int, compression: str) -> None:
        self.scrollback_settings = ts.ScrollbackSettings(lines, compression)


def tab_manager(os_window_id: int = 3, profile_id: str = 'default', tab: FakeTab | None = None) -> SimpleNamespace:
    tab = FakeTab() if tab is None else tab
    return SimpleNamespace(os_window_id=os_window_id, profile=PROFILES[profile_id], active_tab=tab, native_tabs=True, native_tab_settings_button_shown=False)


def fields_by_name(model: dict) -> dict:
    return {f['name']: f for f in model['fields'] if f['name']}


class TestTabSettings(BaseTest):

    def setUp(self) -> None:
        super().setUp()
        self.opts = SimpleNamespace(scrollback_lines=2000, scrollback_compression='none', native_tab_settings_button=True)
        self.boss = SimpleNamespace(os_window_map={}, set_profile_of=MagicMock())
        self.patches = [
            patch('kitty.tab_settings.load_profiles', return_value=PROFILES),
            patch('kitty.tab_settings.get_options', return_value=self.opts),
            patch('kitty.tab_settings.get_boss', return_value=self.boss),
        ]
        for p in self.patches:
            p.start()

    def tearDown(self) -> None:
        for p in self.patches:
            p.stop()
        super().tearDown()

    def test_tab_settings_model(self) -> None:
        tm = tab_manager(profile_id='work', tab=FakeTab(name='', title='htop'))
        with patch('kitty.tab_settings.scrollback_settings_supported', return_value=False):
            model = ts.tab_settings_model(tm)  # type: ignore
        self.ae(model['title'], 'Tab Settings')
        kinds = [f['kind'] for f in model['fields']]
        self.ae(kinds, ['choice', 'button', 'section', 'text', 'section', 'combo', 'choice'])
        f = fields_by_name(model)
        self.ae(f['profile']['value'], 'work')
        self.ae(f['profile']['choices'], [['default', 'Default'], ['work', 'Work']])
        self.ae((f['title']['value'], f['title']['placeholder']), ('', 'htop'))
        self.ae(f['scrollback_lines']['value'], '2000')
        self.ae(f['scrollback_compression']['value'], 'none')
        self.ae(f['scrollback_compression']['choices'][0], ['none', 'None'])
        self.ae(f['scrollback_compression']['choices'][-1], ['zstd:22', 'zstd level 22'])
        # until windows can change their scrollback, its fields are shown but cannot be used
        self.assertFalse(f['scrollback_lines']['enabled'])
        self.assertIn('Not available', f['scrollback_lines']['help'])
        for x in model['fields']:
            self.assertTrue(x['title'], 'every field has a label, also used for assistive technology')

        tm.active_tab.name = 'build'
        tm.active_tab.scrollback_settings = ts.ScrollbackSettings(-1, 'zstd:3')
        with patch('kitty.tab_settings.scrollback_settings_supported', return_value=True):
            f = fields_by_name(ts.tab_settings_model(tm))  # type: ignore
        self.ae(f['title']['value'], 'build')
        self.ae((f['scrollback_lines']['value'], f['scrollback_compression']['value']), ('Unlimited', 'zstd:3'))
        self.assertTrue(f['scrollback_lines']['enabled'])
        # the option values are used while the tab has no settings of its own; a negative count is parsed to 2**32 - 1
        self.opts.scrollback_lines, self.opts.scrollback_compression = 2**32 - 1, True
        f = fields_by_name(ts.tab_settings_model(tab_manager()))  # type: ignore
        self.ae((f['scrollback_lines']['value'], f['scrollback_compression']['value']), ('Unlimited', 'zstd:3'))

    def test_tab_settings_events(self) -> None:
        c = ts.TabSettingsController()
        tm = tab_manager()
        self.boss.os_window_map[3] = tm
        self.ae(c.on_event(4, 'set', 'title', 'x'), 'This tab has been closed')
        self.ae(c.on_event(3, 'set', 'nonsense', 'x'), 'Unknown request: set nonsense')

        self.ae(c.on_event(3, 'set', 'profile', 'default'), '')
        self.boss.set_profile_of.assert_not_called()
        self.ae(c.on_event(3, 'set', 'profile', 'work'), '')
        self.boss.set_profile_of.assert_called_once_with(tm, PROFILES['work'])
        self.ae(c.on_event(3, 'set', 'profile', 'gone'), 'This profile no longer exists')

        self.ae(c.on_event(3, 'set', 'title', 'logs'), '')
        self.ae(c.on_event(3, 'set', 'title', 'logs'), '')
        self.ae(c.on_event(3, 'set', 'title', ''), '')
        self.ae(tm.active_tab.titles, ['logs', ''], 'an unchanged name is not set again, empty follows the window title')

        self.ae(c.on_event(3, 'set', 'scrollback_lines', ' 5000 '), '')
        self.ae(tm.active_tab.scrollback_settings, ts.ScrollbackSettings(5000, 'none'))
        self.ae(c.on_event(3, 'set', 'scrollback_lines', 'unlimited'), '')
        self.ae(tm.active_tab.scrollback_settings.lines, -1)
        self.ae(c.on_event(3, 'set', 'scrollback_lines', 'lots'), 'Enter a number of lines or Unlimited')
        self.ae(c.on_event(3, 'set', 'scrollback_lines', '1e9'), 'Enter a number of lines or Unlimited')
        self.ae(c.on_event(3, 'set', 'scrollback_compression', 'zstd:19'), '')
        self.ae(tm.active_tab.scrollback_settings, ts.ScrollbackSettings(-1, 'zstd:19'))
        self.ae(c.on_event(3, 'set', 'scrollback_compression', 'zstd:23'), 'Choose None or a zstd level')
        self.ae(c.on_event(3, 'set', 'scrollback_compression', 'gzip:3'), 'Choose None or a zstd level')
        self.ae(c.on_event(3, 'set', 'scrollback_compression', 'none'), '')
        self.ae(tm.active_tab.scrollback_settings, ts.ScrollbackSettings(-1, 'none'))

        with patch('kitty.native_ui.open_settings') as open_settings:
            self.ae(c.on_event(3, 'activate', 'edit_profile'), '')
        open_settings.assert_called_once_with('profiles:default')

    def test_tab_settings_show(self) -> None:
        c = ts.TabSettingsController()
        tm = tab_manager(os_window_id=9)
        self.boss.os_window_map[9] = tm
        with patch('kitty.fast_data_types.native_tab_settings_show', create=True) as show:
            c.show(tm)  # type: ignore
        (os_window_id, model, callback), _ = show.call_args
        self.ae(os_window_id, 9)
        self.ae(model['fields'][0]['name'], 'profile')
        # the callback is bound to the tab the popover is for
        callback('set', 'title', 'mine')
        self.ae(tm.active_tab.name, 'mine')

    def test_tab_settings_button(self) -> None:
        calls: list = []
        tm = tab_manager()
        with patch('kitty.fast_data_types.native_tab_settings_button', side_effect=lambda *a: calls.append(a), create=True):
            ts.update_native_tab_settings_button(tm)  # type: ignore
            ts.update_native_tab_settings_button(tm)  # type: ignore
            self.opts.native_tab_settings_button = False
            ts.update_native_tab_settings_button(tm)  # type: ignore
        self.ae(calls, [(3, True), (3, False)])

    def test_tab_scrollback_settings(self) -> None:
        from kitty.tabs import Tab
        applied: list = []
        windows = [SimpleNamespace(set_scrollback_settings=lambda *a: applied.append(a)), SimpleNamespace()]
        tab = Tab.__new__(Tab)
        tab.windows = windows  # type: ignore
        tab.set_scrollback_settings(100, 'zstd:3')
        self.ae(tab.scrollback_settings, ts.ScrollbackSettings(100, 'zstd:3'))
        self.ae(applied, [(100, 'zstd:3')], 'windows that can change their scrollback are told, others are left alone')

    def test_settings_window_selects_profile(self) -> None:
        from kitty import native_ui
        shown: list = []
        with patch.object(native_ui, 'native_settings_show', side_effect=lambda model, cb: shown.append(model)), \
                patch.object(native_ui.SettingsController, 'model', lambda self, page='', profile='': {'select_page': page, 'select_profile': profile}):
            native_ui.settings_controller.show('profiles:work')
            native_ui.settings_controller.show('appearance')
            native_ui.settings_controller.show('appearance:work')
        self.ae(shown, [{'select_page': 'profiles', 'select_profile': 'work'}, {'select_page': 'appearance', 'select_profile': ''},
                        {'select_page': 'appearance', 'select_profile': ''}])
