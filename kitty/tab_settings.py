#!/usr/bin/env python
# License: GPLv3 Copyright: 2026, Adam DePrince

"""
The settings of a single native tab, edited in a popover opened from the gear on the tab. The native code
renders the list of field descriptors built here, much as it renders the pages of the Settings window
built by settings_model.py, and reports every change back to on_event(). Adding a setting means adding
a descriptor to tab_settings_model() and a set_<name>() method to TabSettingsController.

Descriptors are dicts with:
    name: what on_event() is told when the field changes
    title: the label of the field, also its label for assistive technology
    kind: section (a heading), choice (one of choices), combo (text, with choices offered), text or button
    value: the current value, choices: [value, title] pairs, placeholder: shown in an empty text field
    help: the tooltip, enabled: False to show the field without letting it be changed
"""

from functools import partial
from typing import TYPE_CHECKING, Any, NamedTuple

from .fast_data_types import get_boss, get_options
from .profiles import load_profiles

if TYPE_CHECKING:
    from .tabs import Tab, TabManager


UNLIMITED = 'Unlimited'
# The compression levels of zstd, 1 to ZSTD_maxCLevel(), which is 22 in every version of the format
ZSTD_LEVELS = range(1, 23)
SCROLLBACK_SUGGESTIONS = (UNLIMITED, '1000', '10000', '100000', '1000000')


class ScrollbackSettings(NamedTuple):
    lines: int  # negative for unlimited, as with the scrollback_lines option
    compression: str  # none or zstd:<level>


def scrollback_settings_supported() -> bool:
    ' Per tab scrollback settings need windows that can change theirs, see Tab.set_scrollback_settings() '
    from .window import Window
    return hasattr(Window, 'set_scrollback_settings')


def scrollback_lines_text(lines: int) -> str:
    return UNLIMITED if lines < 0 or lines >= 2**32 - 1 else str(lines)


def parse_scrollback_lines(text: str) -> int:
    ' Raises ValueError for text that is neither a number of lines nor Unlimited '
    text = text.strip()
    if text.lower() in (UNLIMITED.lower(), 'infinite', '-1'):
        return -1
    if not text.isdigit():
        raise ValueError(text)
    return int(text)


def parse_compression(text: str) -> str:
    text = text.strip().lower()
    if text in ('', 'none'):
        return 'none'
    kind, _, level = text.partition(':')
    if kind != 'zstd' or not level.isdigit() or int(level) not in ZSTD_LEVELS:
        raise ValueError(text)
    return f'zstd:{int(level)}'


def current_scrollback_settings(tab: 'Tab') -> ScrollbackSettings:
    if (s := getattr(tab, 'scrollback_settings', None)) is not None:
        return s
    opts = get_options()
    compression = getattr(opts, 'scrollback_compression', 'none')
    if compression is True:
        compression = 'zstd:3'
    elif compression is False:
        compression = 'none'
    return ScrollbackSettings(opts.scrollback_lines, str(compression))


def section(title: str) -> dict[str, Any]:
    return {'name': '', 'title': title, 'kind': 'section'}


def tab_settings_model(tm: 'TabManager') -> dict[str, Any]:
    tab = tm.active_tab
    profiles = load_profiles()
    fields: list[dict[str, Any]] = [
        {'name': 'profile', 'title': 'Profile', 'kind': 'choice', 'value': tm.profile.id, 'choices': [[p.id, p.name] for p in profiles.values()],
         'help': 'The profile of this tab: its colors, font, command and more'},
        {'name': 'edit_profile', 'title': 'Edit Profile…', 'kind': 'button', 'help': 'Open the profile of this tab in the Settings window'},
    ]
    if tab is not None:
        fields += [
            section('Tab'),
            {'name': 'title', 'title': 'Tab name', 'kind': 'text', 'value': tab.name, 'placeholder': tab.title,
             'help': 'The name of this tab. Leave empty to follow the title of its window.'},
        ]
        supported = scrollback_settings_supported()
        s = current_scrollback_settings(tab)
        note = '' if supported else ' Not available in this build.'
        fields += [
            section('Scrollback'),
            {'name': 'scrollback_lines', 'title': 'Lines', 'kind': 'combo', 'value': scrollback_lines_text(s.lines),
             'choices': [[x, x] for x in SCROLLBACK_SUGGESTIONS], 'enabled': supported,
             'help': f'How many lines of history the windows of this tab keep, a number or {UNLIMITED}.{note}'},
            {'name': 'scrollback_compression', 'title': 'Compression', 'kind': 'choice', 'value': s.compression,
             'choices': [['none', 'None']] + [[f'zstd:{n}', f'zstd level {n}'] for n in ZSTD_LEVELS], 'enabled': supported,
             'help': f'Compress the history kept in memory. Higher levels are smaller and slower.{note}'},
        ]
    return {'title': 'Tab Settings', 'fields': fields}


class TabSettingsController:

    def show(self, tm: 'TabManager') -> None:
        from .fast_data_types import native_tab_settings_show
        native_tab_settings_show(tm.os_window_id, tab_settings_model(tm), partial(self.on_event, tm.os_window_id))

    def on_event(self, os_window_id: int, verb: str, name: str = '', value: str = '') -> str:
        ' Called by the native code. The result is an error message to show the user, empty if there is none. '
        tm = get_boss().os_window_map.get(os_window_id)
        if tm is None:
            return 'This tab has been closed'
        handler = getattr(self, f'{verb}_{name}', None)
        if handler is None:
            return f'Unknown request: {verb} {name}'
        return str(handler(tm, value) or '')

    def set_profile(self, tm: 'TabManager', value: str) -> str:
        profile = load_profiles().get(value)
        if profile is None:
            return 'This profile no longer exists'
        if profile.id != tm.profile.id:
            get_boss().set_profile_of(tm, profile)
        return ''

    def activate_edit_profile(self, tm: 'TabManager', value: str) -> None:
        from .native_ui import open_settings
        open_settings(f'profiles:{tm.profile.id}')

    def set_title(self, tm: 'TabManager', value: str) -> None:
        if (tab := tm.active_tab) is not None and value != tab.name:
            tab.set_title(value)

    def set_scrollback_lines(self, tm: 'TabManager', value: str) -> str:
        try:
            lines = parse_scrollback_lines(value)
        except ValueError:
            return f'Enter a number of lines or {UNLIMITED}'
        if (tab := tm.active_tab) is not None:
            tab.set_scrollback_settings(lines, current_scrollback_settings(tab).compression)
        return ''

    def set_scrollback_compression(self, tm: 'TabManager', value: str) -> str:
        try:
            compression = parse_compression(value)
        except ValueError:
            return 'Choose None or a zstd level'
        if (tab := tm.active_tab) is not None:
            tab.set_scrollback_settings(current_scrollback_settings(tab).lines, compression)
        return ''


tab_settings_controller = TabSettingsController()


def update_native_tab_settings_button(tm: 'TabManager') -> None:
    ' Show or hide the gear on the native tab of tm, as the option says '
    show = bool(get_options().native_tab_settings_button)
    if show != tm.native_tab_settings_button_shown:
        from .fast_data_types import native_tab_settings_button
        tm.native_tab_settings_button_shown = show
        native_tab_settings_button(tm.os_window_id, show)
