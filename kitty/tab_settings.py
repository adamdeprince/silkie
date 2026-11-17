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
from typing import TYPE_CHECKING, Any

from .fast_data_types import get_boss, get_options
from .profiles import load_profiles

if TYPE_CHECKING:
    from .tabs import Tab, TabManager


UNLIMITED = 'Unlimited'
SCROLLBACK_SUGGESTIONS = (UNLIMITED, '1000', '10000', '100000', '1000000')
DEFAULT_CONTRAST = 'Default'
CONTRAST_SUGGESTIONS = (DEFAULT_CONTRAST, '0%', '20%', '40%', '60%', '80%', '100%')


def zstd_levels() -> range:
    ' The compression levels of the zstd library in use, the negative, fastest ones included '
    from .fast_data_types import HistoryBuf
    lo, hi, _ = HistoryBuf.zstd_levels()
    return range(lo, hi + 1)


def scrollback_lines_text(lines: int) -> str:
    return UNLIMITED if lines < 0 else str(lines)


def parse_scrollback_lines(text: str) -> int:
    ' Raises ValueError for text that is neither a number of lines nor Unlimited, -1 is unlimited '
    text = text.strip()
    if text.lower() in (UNLIMITED.lower(), 'infinite', '-1'):
        return -1
    if not text.isdigit():
        raise ValueError(text)
    return int(text)


def compression_text(compression: int | str) -> str:
    return 'none' if compression == 'none' else str(compression)


def parse_compression(text: str) -> str:
    ' none or a zstd level of the library in use, raises ValueError for anything else '
    text = text.strip().lower()
    if text in ('', 'none'):
        return 'none'
    if text.startswith('zstd:'):
        text = text[5:]
    try:
        level = int(text)
    except ValueError:
        raise ValueError(text) from None
    if level not in zstd_levels():
        raise ValueError(text)
    return str(level)


def current_scrollback_settings(tab: 'Tab') -> tuple[int, int | str]:
    ' (lines, compression) of the tab: -1 lines is unlimited, compression is none or a zstd level '
    if (w := tab.active_window) is not None:
        return w.scrollback_settings
    from .options.utils import scrollback_settings_for_display
    if (s := getattr(tab, 'scrollback_settings', None)) is not None:
        return scrollback_settings_for_display(*s)
    opts = get_options()
    return scrollback_settings_for_display(opts.scrollback_lines, opts.scrollback_compression)


def contrast_text(value: float | None) -> str:
    ' The minimum contrast of a tab as shown, Default when it follows its profile or the options '
    return DEFAULT_CONTRAST if value is None else f'{value:g}%'


def parse_contrast(text: str) -> float | None:
    ' None for Default, a percentage otherwise, raises ValueError for anything else '
    text = text.strip()
    if not text or text.lower() == DEFAULT_CONTRAST.lower():
        return None
    from .window import parse_minimum_contrast_value
    return parse_minimum_contrast_value(text)


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
        lines, compression = current_scrollback_settings(tab)
        levels = zstd_levels()
        fields += [
            section('Scrollback'),
            {'name': 'scrollback_lines', 'title': 'Lines', 'kind': 'combo', 'value': scrollback_lines_text(lines),
             'choices': [[x, x] for x in SCROLLBACK_SUGGESTIONS],
             'help': f'How many lines of history the windows of this tab keep, a number or {UNLIMITED}.'},
            {'name': 'scrollback_compression', 'title': 'Compression', 'kind': 'choice', 'value': compression_text(compression),
             'choices': [['none', 'None']] + [[str(n), f'zstd level {n}'] for n in levels],
             'help': 'Compress the history kept in memory. Higher levels are smaller and slower, levels below 1 are the fastest.'},
            section('Colors'),
            {'name': 'minimum_contrast', 'title': 'Minimum contrast', 'kind': 'combo', 'value': contrast_text(tab.minimum_contrast_override),
             'choices': [[x, x] for x in CONTRAST_SUGGESTIONS], 'placeholder': contrast_text(tab.minimum_contrast),
             'help': f'Make text at least this much more legible against its background, from 0% to 100%. {DEFAULT_CONTRAST} follows the profile of '
             'this tab or the minimum_contrast option.'},
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
            tab.set_scrollback_settings(lines, current_scrollback_settings(tab)[1])
        return ''

    def set_scrollback_compression(self, tm: 'TabManager', value: str) -> str:
        try:
            compression = parse_compression(value)
        except ValueError:
            return 'Choose None or a zstd level'
        if (tab := tm.active_tab) is not None:
            tab.set_scrollback_settings(current_scrollback_settings(tab)[0], compression)
        return ''

    def set_minimum_contrast(self, tm: 'TabManager', value: str) -> str:
        try:
            contrast = parse_contrast(value)
        except ValueError:
            return f'Enter a percentage from 0% to 100% or {DEFAULT_CONTRAST}'
        if (tab := tm.active_tab) is not None:
            tab.set_minimum_contrast(contrast)
        return ''


tab_settings_controller = TabSettingsController()


def update_native_tab_settings_button(tm: 'TabManager') -> None:
    ' Show or hide the gear on the native tab of tm, as the option says '
    show = bool(get_options().native_tab_settings_button)
    if show != tm.native_tab_settings_button_shown:
        from .fast_data_types import native_tab_settings_button
        tm.native_tab_settings_button_shown = show
        native_tab_settings_button(tm.os_window_id, show)
