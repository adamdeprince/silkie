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
    from kitty.cli_stub import SetFindKeysToTerminalRCOptions as CLIOptions


RESET_WORDS = ('default', 'none', 'reset')


def parse_value(raw: str) -> bool | None:
    ' yes or no, None for one of the words that remove the setting '
    if raw.strip().lower() in RESET_WORDS:
        return None
    from kitty.window import parse_find_keys_to_terminal_value

    return parse_find_keys_to_terminal_value(raw)


class SetFindKeysToTerminal(RemoteCommand):
    protocol_spec = __doc__ = """
    value+/str: yes to send the keys of Find to the program, no to keep them for Find, default to remove the setting
    match_window/str: Window to change
    match_tab/str: Tab to change
    all/bool: Boolean indicating whether to change all windows
    """

    short_desc = 'Send the keys of Find to the terminal or keep them for Find'
    desc = (
        'Set :opt:`find_keys_to_terminal` for the specified windows (defaults to the active window): with'
        ' :code:`yes` the keys Silkie binds to Find by default, such as :kbd:`⌘+F`, go to the program'
        ' running in the window, with :code:`no` they open Find. The Find items of the Edit menu work either'
        ' way. For example::\n\n'
        '    kitten @ set-find-keys-to-terminal yes\n\n'
        'When a tab is specified with :option:`--match-tab`, the setting of the tab is changed: it applies to'
        ' all its windows, replacing what they set for themselves, and to the windows opened in it later. The value'
        ' :code:`default` removes the setting, so that the window follows its tab, profile and the'
        ' :opt:`find_keys_to_terminal` option again. The current values are shown by :ref:`kitten @ ls <at-ls>`.'
    )
    options_spec = (
        """\
--all -a
type=bool-set
By default, only the active window is changed. This option will cause all windows to be changed.
"""
        + '\n\n'
        + MATCH_WINDOW_OPTION
        + '\n\n'
        + MATCH_TAB_OPTION.replace('--match -m', '--match-tab -t')
    )
    args = RemoteCommand.Args(spec='yes|no|default', count=1, json_field='value')

    def message_to_kitty(self, global_opts: RCOptions, opts: 'CLIOptions', args: ArgsType) -> PayloadType:
        if not args:
            self.fatal('You must specify yes, no or default')
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
                tab.set_find_keys_to_terminal(value)
            return None
        for w in self.windows_for_payload(boss, window, payload_get):
            if w:
                w.set_find_keys_to_terminal(value)
        return None


set_find_keys_to_terminal = SetFindKeysToTerminal()
