#!/usr/bin/env python
# License: GPLv3 Copyright: 2026, Adam DePrince


from typing import TYPE_CHECKING

from .base import (
    MATCH_TAB_OPTION,
    MATCH_WINDOW_OPTION,
    ArgsType,
    Boss,
    MatchError,
    PayloadGetType,
    PayloadType,
    RCOptions,
    RemoteCommand,
    RemoteControlErrorWithoutTraceback,
    ResponseType,
    Window,
)

if TYPE_CHECKING:
    from kitty.cli_stub import SetMinimumContrastRCOptions as CLIOptions


RESET_WORDS = ('default', 'none', 'reset')


def parse_value(raw: str) -> float | None:
    ' The minimum contrast, None for one of the words that remove the setting '
    if raw.strip().lower() in RESET_WORDS:
        return None
    from kitty.options.utils import minimum_contrast

    return minimum_contrast(raw)


class SetMinimumContrast(RemoteCommand):
    protocol_spec = __doc__ = """
    value+/str: A percentage from 0 to 100, such as 40 or 40%, or default to remove the setting
    match_window/str: Window to change the minimum contrast of
    match_tab/str: Tab to change the minimum contrast of
    all/bool: Boolean indicating whether to change the minimum contrast of all windows
    """

    short_desc = 'Set the minimum contrast of text'
    desc = (
        'Set the :opt:`minimum_contrast` of the specified windows (defaults to the active window), a percentage'
        ' from 0 to 100 by which the lightness of text must differ from that of its background. Text closer to'
        ' its background is drawn lighter or darker, keeping its hue, 0 turns this off and 100 leaves only black'
        ' and white text. For example::\n\n'
        '    kitten @ set-minimum-contrast 40\n\n'
        'When a tab is specified with :option:`--match-tab`, the minimum contrast of the tab is set: it applies to'
        ' all its windows, replacing what they set for themselves, and to the windows opened in it later. The value'
        ' :code:`default` removes the setting, so that the window follows its tab, profile and the'
        ' :opt:`minimum_contrast` option again. The current values are shown by :ref:`kitten @ ls <at-ls>`.'
    )
    options_spec = (
        """\
--all -a
type=bool-set
By default, only the active window is changed. This option will cause the minimum
contrast of all windows to be changed.
"""
        + '\n\n'
        + MATCH_WINDOW_OPTION
        + '\n\n'
        + MATCH_TAB_OPTION.replace('--match -m', '--match-tab -t')
    )
    args = RemoteCommand.Args(spec='PERCENTAGE', count=1, json_field='value')

    def message_to_kitty(self, global_opts: RCOptions, opts: 'CLIOptions', args: ArgsType) -> PayloadType:
        if not args:
            self.fatal('You must specify the minimum contrast, a percentage from 0 to 100, or default')
        try:
            parse_value(args[0])
        except ValueError as e:
            self.fatal(str(e))
        return {'value': args[0], 'match_window': opts.match, 'match_tab': opts.match_tab, 'all': opts.all}

    def response_from_kitty(self, boss: Boss, window: Window | None, payload_get: PayloadGetType) -> ResponseType:
        try:
            value = parse_value(str(payload_get('value') or ''))
        except ValueError as e:
            raise RemoteControlErrorWithoutTraceback(str(e)) from e
        if (match := payload_get('match_tab')) and not payload_get('all'):
            tabs = tuple(boss.match_tabs(match))
            if not tabs:
                raise MatchError(match, 'tabs')
            for tab in tabs:
                tab.set_minimum_contrast(value)
            return None
        for w in self.windows_for_payload(boss, window, payload_get):
            if w:
                w.set_minimum_contrast(value)
        return None


set_minimum_contrast = SetMinimumContrast()
