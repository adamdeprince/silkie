#!/usr/bin/env python
# License: GPLv3 Copyright: 2026, Adam DePrince

"""
Find: search the screen and scrollback of a window and highlight every match, like Find in
iTerm2. The searching and highlighting is done by kitty/search.c, this module holds the find bar
of each OS window and what it shows. The bar is native when the platform has one (macOS, and
GTK for native tabs), otherwise it is a line drawn in the window, see FallbackPrompt.

Navigation follows iTerm2: Find Next goes up, to older output, and Find Previous goes down.
"""

import re
from typing import TYPE_CHECKING, Any, NamedTuple

from .fast_data_types import (
    GLFW_FKEY_BACKSPACE,
    GLFW_FKEY_ENTER,
    GLFW_FKEY_ESCAPE,
    GLFW_FKEY_F3,
    GLFW_FKEY_KP_ENTER,
    GLFW_MOD_ALT,
    GLFW_MOD_CONTROL,
    GLFW_MOD_SHIFT,
    GLFW_MOD_SUPER,
    add_timer,
    get_boss,
    remove_timer,
)
from .typing_compat import KeyEventType

if TYPE_CHECKING:
    from .window import Window

# The actions whose keys find_keys_to_terminal sends to the program
FIND_ACTIONS = frozenset(('find', 'find_next', 'find_previous', 'find_selection'))
# How long the bar says that the search wrapped around
WRAP_NOTICE_TIME = 1.5


class FindStatus(NamedTuple):
    total: int = 0
    current: int = 0
    done: bool = True
    wrapped: bool = False
    error: str = ''

    @property
    def text(self) -> str:
        if self.error:
            return self.error
        if not self.total:
            return 'Searching…' if not self.done else 'Not found'
        ans = f'{self.current} of {self.total}' if self.current else f'{self.total} found'
        if not self.done:
            ans += '…'
        if self.wrapped:
            ans += ' (wrapped)'
        return ans


class FindBar:
    ' The find bar of an OS window, it searches the active window of the OS window '

    def __init__(self, os_window_id: int) -> None:
        self.os_window_id = os_window_id
        self.window_id = 0
        self.visible = False
        self.native = False
        self.query = ''
        self.case_sensitive = False
        self.regex = False
        self.error = ''
        self.wrapped_until = 0.
        self.wrap_timer = 0
        # the native field has the keyboard, on Wayland the terminal gets the keys too and drops them
        self.native_has_focus = False
        self.prompt: FallbackPrompt | None = None

    @property
    def window(self) -> 'Window | None':
        return get_boss().window_id_map.get(self.window_id)

    def pattern(self) -> 're.Pattern[str] | None':
        if not self.regex:
            return None
        # the text of logical lines is searched together, separated by newlines
        return re.compile(self.query, re.MULTILINE | (0 if self.case_sensitive else re.IGNORECASE))

    def start(self, auto_scroll: bool = True) -> None:
        w = self.window
        self.error = ''
        if w is None:
            return
        if not self.query:
            w.screen.search_stop()
            return
        try:
            pattern = self.pattern()
        except re.error as e:
            w.screen.search_stop()
            self.error = f'Invalid expression: {e}'
            return
        w.screen.search_start(self.query, self.case_sensitive, pattern, auto_scroll)

    def stop(self) -> None:
        if (w := self.window) is not None:
            w.screen.search_stop()

    def status(self) -> FindStatus:
        w = self.window
        if self.error:
            return FindStatus(done=True, error=self.error)
        if w is None or not self.query:
            return FindStatus(done=True)
        s = w.screen.search_status()
        if s is None:
            return FindStatus(done=True)
        from time import monotonic
        return FindStatus(s['total'], s['current'], s['done'], monotonic() < self.wrapped_until, 'Search failed' if s['failed'] else '')

    def model(self, focus: bool = False, select_all: bool = False) -> dict[str, Any]:
        st = self.status()
        return {
            'query': self.query, 'case_sensitive': self.case_sensitive, 'regex': self.regex,
            'status': st.text, 'total': st.total, 'current': st.current, 'done': st.done, 'wrapped': st.wrapped, 'error': bool(st.error),
            'focus': focus, 'select_all': select_all,
        }

    # Showing it {{{
    def show(self, focus: bool = True, select_all: bool = False) -> None:
        from .fast_data_types import native_find_bar
        self.visible = True
        model = self.model(focus, select_all)
        if self.prompt is None and native_find_bar(self.os_window_id, model, self.on_native_event):
            self.native = True
            return
        self.native = False
        if self.prompt is None:
            self.prompt = FallbackPrompt(self)
        if focus:
            self.prompt.focused = True
        self.prompt.update()

    def refresh(self) -> None:
        ' Show the current status '
        if not self.visible:
            return
        if self.native:
            from .fast_data_types import native_find_bar
            native_find_bar(self.os_window_id, self.model(), self.on_native_event)
        elif self.prompt is not None:
            self.prompt.update()

    def hide(self) -> None:
        if self.native:
            from .fast_data_types import native_find_bar
            native_find_bar(self.os_window_id, None, self.on_native_event)
        if self.prompt is not None:
            self.prompt.close()
            self.prompt = None
        self.visible = self.native = self.native_has_focus = False
    # }}}

    # What the user did {{{
    def set_query(self, query: str) -> None:
        if query != self.query:
            self.query = query
            self.start()
            self.refresh()

    def set_case_sensitive(self, val: bool) -> None:
        if val != self.case_sensitive:
            self.case_sensitive = val
            self.start()
            self.refresh()

    def set_regex(self, val: bool) -> None:
        if val != self.regex:
            self.regex = val
            self.start()
            self.refresh()

    def navigate(self, direction: int) -> str:
        ' -1 for the next match, which is older, up the screen, 1 for the previous match '
        w = self.window
        if w is None or not self.query or self.error:
            return 'none'
        if w.screen.search_status() is None:
            self.start(auto_scroll=False)
        ans = w.screen.search_navigate(direction)
        if ans == 'wrapped':
            self.note_wrap()
        elif ans == 'none':
            from .fast_data_types import ring_bell
            if w.screen.search_status() is not None:
                ring_bell()
        self.refresh()
        return str(ans)

    def note_wrap(self) -> None:
        from time import monotonic
        self.wrapped_until = monotonic() + WRAP_NOTICE_TIME
        if self.wrap_timer:
            remove_timer(self.wrap_timer)
        self.wrap_timer = add_timer(self.wrap_notice_ended, WRAP_NOTICE_TIME, False)

    def wrap_notice_ended(self, timer_id: int | None = None) -> None:
        self.wrap_timer = 0
        self.refresh()

    def on_native_event(self, verb: str, value: Any = None) -> None:
        ' Called by the native find bar '
        if verb == 'query':
            self.set_query(str(value or ''))
        elif verb == 'next':
            self.navigate(-1)
        elif verb == 'previous':
            self.navigate(1)
        elif verb == 'case_sensitive':
            self.set_case_sensitive(bool(value))
        elif verb == 'regex':
            self.set_regex(bool(value))
        elif verb == 'focus':
            self.native_has_focus = bool(value)
        elif verb == 'close':
            close_find_bar(self.os_window_id)
    # }}}

    def window_changed(self, w: 'Window') -> None:
        ' The active window of the OS window changed while the bar is shown '
        if w.id == self.window_id:
            return
        self.stop()
        self.window_id = w.id
        self.start(auto_scroll=False)
        self.refresh()


find_bars: dict[int, FindBar] = {}


def bar_for(w: 'Window', create: bool = True) -> FindBar | None:
    b = find_bars.get(w.os_window_id)
    if b is None and create:
        b = find_bars[w.os_window_id] = FindBar(w.os_window_id)
    return b


# The actions {{{
def open_find_bar(w: 'Window', query: str = '') -> FindBar:
    b = bar_for(w)
    assert b is not None
    if query:
        b.query = query
        b.error = ''
        b.window_id = w.id
        b.start()
    if b.window_id != w.id:
        if b.visible:
            b.stop()
        b.window_id = w.id
        if b.query:
            b.start()
    elif b.query and w.screen.search_status() is None:
        b.start()
    b.show(focus=True, select_all=True)
    return b


def find_next(w: 'Window', direction: int) -> None:
    b = bar_for(w)
    assert b is not None
    if not b.query:
        open_find_bar(w)
        return
    if b.window_id != w.id or not b.visible:
        b.window_id = w.id
        b.show(focus=False)
    b.navigate(direction)


def use_selection_for_find(w: 'Window') -> bool:
    text = w.text_for_selection()
    if not text:
        return False
    # matches are found in a logical line, so the lines of a selection cannot all match
    text = text.strip('\n').splitlines()[0] if '\n' in text.strip('\n') else text.strip('\n')
    b = bar_for(w)
    assert b is not None
    b.window_id = w.id
    b.query = re.escape(text) if b.regex else text
    b.start()
    b.show(focus=False)
    return True


def close_find_bar(os_window_id: int) -> None:
    b = find_bars.pop(os_window_id, None)
    if b is None:
        return
    b.stop()
    b.hide()
    if b.wrap_timer:
        remove_timer(b.wrap_timer)
    # hiding a native bar gives the keyboard back to the terminal


def on_search_status_changed(w: 'Window') -> None:
    ' Called by the screen of w while it searches '
    b = find_bars.get(w.os_window_id)
    if b is not None and b.window_id == w.id:
        b.refresh()


def on_window_focused(w: 'Window') -> None:
    ' The find bar follows the active window of its OS window '
    b = find_bars.get(w.os_window_id)
    if b is not None and b.visible:
        b.window_changed(w)


def terminal_clicked(w: 'Window') -> None:
    ' A click in the terminal gives it the keyboard back from the native find bar '
    b = find_bars.get(w.os_window_id)
    if b is not None and b.native and b.native_has_focus:
        from .fast_data_types import native_find_bar
        b.native_has_focus = False
        native_find_bar(b.os_window_id, {**b.model(), 'focus_terminal': True}, b.on_native_event)
    if b is not None and b.prompt is not None and b.window_id == w.id:
        # the drawn bar keeps showing the matches, the keys go to the program again
        b.prompt.focused = False
        b.prompt.update()


def on_os_window_closed(os_window_id: int) -> None:
    b = find_bars.pop(os_window_id, None)
    if b is not None and b.wrap_timer:
        remove_timer(b.wrap_timer)


def on_window_closed(w: 'Window') -> None:
    b = find_bars.get(w.os_window_id)
    if b is not None and b.window_id == w.id:
        if b.prompt is not None and b.prompt.window_id == w.id:
            b.prompt.window_id = 0
        b.window_id = 0
        b.refresh()
# }}}


# Keys {{{
def default_find_triggers(kitty_mod: int) -> frozenset[Any]:
    ' The keys bound to the find actions by default '
    from .options.types import defaults
    return frozenset(
        kd.trigger.resolve_kitty_mod(kitty_mod) for kd in defaults.map if not kd.is_sequence and kd.definition.split(maxsplit=1)[0] in FIND_ACTIONS)


_default_triggers: dict[int, frozenset[Any]] = {}


def key_goes_to_terminal(w: 'Window | None', trigger: Any, definition: str) -> bool:
    ''' Whether the key trigger, mapped to definition, is a default key of Find that
    find_keys_to_terminal sends to the program of w instead. Keys mapped to Find by
    the user always work. '''
    if w is None or trigger is None or not definition or definition.split(maxsplit=1)[0] not in FIND_ACTIONS or not w.find_keys_to_terminal:
        return False
    from .fast_data_types import get_options
    km = get_options().kitty_mod
    if (t := _default_triggers.get(km)) is None:
        t = _default_triggers[km] = default_find_triggers(km)
    return trigger in t


def sync_menu_shortcuts(w: 'Window | None') -> None:
    ''' On macOS the Edit menu shows the keys of Find, and a key that is a menu key
    equivalent never reaches the terminal. While the focused window sends them to its
    program they are taken off the menu items, which keep working when clicked. '''
    from .constants import is_macos
    if not is_macos:
        return
    from .fast_data_types import cocoa_set_find_shortcuts_active
    cocoa_set_find_shortcuts_active(not (w is not None and w.find_keys_to_terminal))
# }}}


# The bar drawn in the window, when there is no native one {{{
class FallbackPrompt:
    ''' A line at the top or bottom of the window, in place of its title bar, that shows
    the query, the counter and the options, and gets the keys while it is focused:
    Enter and F3 for the next match, with Shift for the previous one, Alt+C for case
    sensitivity, Alt+R for regular expressions, Escape to close. '''

    def __init__(self, bar: FindBar) -> None:
        self.bar = bar
        self.focused = True
        self.window_id = 0

    def text(self) -> str:
        b = self.bar
        st = b.status()
        cursor = '\x1b[7m \x1b[27m' if self.focused else ''
        def toggle(on: bool, label: str) -> str:
            return f'\x1b[7m{label}\x1b[27m' if on else label
        status = st.text if b.query else ''
        hints = '  ⏎ next  ⇧⏎ previous  esc close' if self.focused else ''
        return f' Find: {b.query}{cursor}  {status}  [{toggle(b.case_sensitive, "Aa")}] [{toggle(b.regex, ".*")}]{hints}'

    def update(self) -> None:
        w = self.bar.window
        if self.window_id and self.window_id != (w.id if w else 0):
            self.detach()
        if w is None:
            return
        self.window_id = w.id
        w.set_find_prompt(self.text())

    def detach(self) -> None:
        old = get_boss().window_id_map.get(self.window_id)
        if old is not None:
            old.set_find_prompt(None)
        self.window_id = 0

    def close(self) -> None:
        self.detach()

    def on_key(self, ev: KeyEventType) -> bool:
        ' Returns True if the key was used '
        if not self.focused:
            return False
        b = self.bar
        mods = ev.mods & (GLFW_MOD_SHIFT | GLFW_MOD_ALT | GLFW_MOD_CONTROL | GLFW_MOD_SUPER)
        if ev.key == GLFW_FKEY_ESCAPE:
            close_find_bar(b.os_window_id)
            return True
        if ev.key in (GLFW_FKEY_ENTER, GLFW_FKEY_KP_ENTER, GLFW_FKEY_F3):
            b.navigate(1 if mods & GLFW_MOD_SHIFT else -1)
        elif ev.key == GLFW_FKEY_BACKSPACE:
            b.set_query(b.query[:-1])
        elif mods == GLFW_MOD_ALT and ev.key == ord('c'):
            b.set_case_sensitive(not b.case_sensitive)
        elif mods == GLFW_MOD_ALT and ev.key == ord('r'):
            b.set_regex(not b.regex)
        elif mods == GLFW_MOD_CONTROL and ev.key == ord('u'):
            b.set_query('')
        elif ev.text and not mods & (GLFW_MOD_CONTROL | GLFW_MOD_SUPER | GLFW_MOD_ALT):
            b.set_query(b.query + ev.text)
        else:
            # other shortcuts, such as paste or switching windows, keep working
            return False
        self.update()
        return True


def handle_key(w: 'Window | None', ev: KeyEventType) -> bool:
    ' Keys for the find bar of the OS window of w, which has the keyboard. Returns True if the key was used. '
    if w is None:
        return False
    b = find_bars.get(w.os_window_id)
    if b is None or not b.visible or b.window_id != w.id:
        return False
    if b.native:
        # Wayland: the toolkit and the terminal both get the keys typed into the native field
        return b.native_has_focus
    if b.prompt is not None:
        return b.prompt.on_key(ev)
    return False


def paste_into_prompt(w: 'Window', text: str) -> bool:
    b = find_bars.get(w.os_window_id)
    if b is None or b.prompt is None or not b.prompt.focused or b.window_id != w.id:
        return False
    b.set_query(b.query + text.replace('\n', ' '))
    b.prompt.update()
    return True
# }}}


