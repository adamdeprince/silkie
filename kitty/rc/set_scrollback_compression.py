#!/usr/bin/env python
# License: GPLv3 Copyright: 2026, Kovid Goyal <kovid at kovidgoyal.net>


from typing import TYPE_CHECKING

from .base import MATCH_TAB_OPTION, MATCH_WINDOW_OPTION, ArgsType, Boss, PayloadGetType, PayloadType, RCOptions, RemoteCommand, ResponseType, Window

if TYPE_CHECKING:
    from kitty.cli_stub import SetScrollbackCompressionRCOptions as CLIOptions


class SetScrollbackCompression(RemoteCommand):
    protocol_spec = __doc__ = """
    compression+/str: :code:`none` or a zstd compression level
    lines/str: The number of scrollback lines or :code:`unlimited`, empty to leave it unchanged
    recompress/bool: Boolean indicating whether to also recompress what is already compressed
    match_window/str: Window to change the scrollback of
    match_tab/str: Tab to change the scrollback of
    all/bool: Boolean indicating whether to change the scrollback of all windows
    """

    short_desc = 'Set how the scrollback is kept'
    desc = (
        'Set how the scrollback of the specified windows or tabs (defaults to the active window) is kept. The'
        ' :italic:`COMPRESSION` is :code:`none` or a zstd compression level, see :opt:`scrollback_compression`.'
        ' It applies to lines compressed from now on, and with :option:`--recompress` also to the lines'
        ' compressed already. When a tab is specified, the windows opened in it later get the same settings.'
    )
    options_spec = (
        """\
--lines
Also set the number of scrollback lines, or :code:`unlimited`. A lower limit drops the oldest lines.


--recompress
type=bool-set
Also recompress, in the background, the lines that are compressed already.


--all -a
type=bool-set
Change the scrollback of all windows.

"""
        + '\n\n'
        + MATCH_WINDOW_OPTION
        + '\n\n'
        + MATCH_TAB_OPTION.replace('--match -m', '--match-tab -t')
    )
    args = RemoteCommand.Args(spec='COMPRESSION', count=1, json_field='compression')

    def message_to_kitty(self, global_opts: RCOptions, opts: 'CLIOptions', args: ArgsType) -> PayloadType:
        if not args:
            self.fatal('You must specify the COMPRESSION, none or a zstd compression level')
        compression = args[0].strip().lower()
        if compression != 'none':
            try:
                int(compression.removeprefix('zstd:'))
            except ValueError:
                self.fatal(f'{compression} is not none or a zstd compression level')
        return {
            'compression': compression,
            'lines': opts.lines,
            'recompress': opts.recompress,
            'match_window': opts.match,
            'match_tab': opts.match_tab,
            'all': opts.all,
        }

    def response_from_kitty(self, boss: Boss, window: Window | None, payload_get: PayloadGetType) -> ResponseType:
        compression = payload_get('compression')
        lines_spec = payload_get('lines') or ''
        recompress = bool(payload_get('recompress'))
        if match_tab := payload_get('match_tab'):
            tabs = list(boss.match_tabs(match_tab))
            if not tabs:
                from .base import MatchError
                raise MatchError(match_tab, 'tabs')
            for tab in tabs:
                lines = lines_spec
                if not lines:
                    if tab.scrollback_settings:
                        lines = tab.scrollback_settings[0]
                    else:
                        w = tab.active_window
                        lines = w.scrollback_settings[0] if w else -1
                tab.set_scrollback_settings(lines, compression, recompress)
            return None
        for w in self.windows_for_payload(boss, window, payload_get):
            if w:
                w.set_scrollback_settings(lines_spec or w.scrollback_settings[0], compression, recompress)
        return None


set_scrollback_compression = SetScrollbackCompression()
