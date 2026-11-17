#!/usr/bin/env python
# License: GPLv3 Copyright: 2026, Adam DePrince

# Find: searching the screen and the scrollback (kitty/search.c), its highlights and
# navigation, and the find bar, actions and keys (kitty/find.py)

import re
from types import SimpleNamespace
from unittest.mock import patch

from kitty.fast_data_types import test_set_history_compression
from kitty.options.utils import SCROLLBACK_COMPRESSION_NONE

from .base import BaseTest, draw_multicell, parse_bytes

TINY_PARAMS = (1, 2, 64)


class TestFind(BaseTest):

    def tearDown(self):
        test_set_history_compression()
        super().tearDown()

    def screen(self, cols=10, lines=4, scrollback=100, compression=None):
        options = {}
        if compression is not None:
            options['scrollback_compression'] = compression
        return self.create_screen(cols=cols, lines=lines, scrollback=scrollback, options=options)

    def find(self, s, query, case_sensitive=False, regex=False):
        s.search_start(query, case_sensitive, re.compile(query, re.M | (0 if case_sensitive else re.IGNORECASE)) if regex else None)
        s.search_run_to_completion()
        return s.search_matches()

    def lines(self, s, *texts):
        for t in texts:
            s.draw(t)
            s.carriage_return()
            s.linefeed()

    def test_literal_and_case(self):
        s = self.screen(cols=20)
        self.lines(s, 'one Foo two', 'foo FOO', 'nothing here', 'last foo')
        s.draw('screen foo')
        # rows: 0 is the top of the screen, the scrollback is negative
        m = self.find(s, 'foo')
        self.ae(len(m), 5)
        self.ae(m[0][1:], (4, m[0][0], 7))
        self.ae(self.find(s, 'FOO', True), [(m[2][0], 4, m[2][0], 7)])
        self.ae(len(self.find(s, 'Foo', True)), 1)
        self.ae(self.find(s, 'absent'), [])
        st = s.search_status()
        self.ae((st['total'], st['done'], st['current']), (0, True, 0))
        # an empty query finds nothing
        self.ae(self.find(s, ''), [])
        # matches do not overlap
        s = self.screen(cols=20)
        s.draw('aaaa')
        self.ae(self.find(s, 'aa'), [(0, 0, 0, 2), (0, 2, 0, 4)])

    def test_scrollback_and_screen(self):
        s = self.screen(cols=10, lines=3, scrollback=50)
        for i in range(20):
            self.lines(s, f'x{i} hit' if i % 5 == 0 else f'x{i}')
        s.draw('hit')
        m = self.find(s, 'hit')
        self.ae([r for r, *_ in m], [-18, -13, -8, -3, 2])
        self.ae(s.search_current(), (2, 0, 2, 3))
        self.assertTrue(s.search_status()['done'])

    def test_across_soft_wraps(self):
        s = self.screen(cols=5, lines=4)
        s.draw('abcdefghij')  # wraps after e
        self.ae(self.find(s, 'def'), [(0, 3, 1, 1)])
        self.ae(self.find(s, 'abcdefghij'), [(0, 0, 1, 5)])
        # a hard line break separates logical lines
        s = self.screen(cols=5, lines=4)
        self.lines(s, 'abc')
        s.draw('def')
        self.ae(self.find(s, 'cd'), [])
        self.ae(self.find(s, 'c.?d', regex=True), [])
        # but a regex can still match inside the next line
        self.ae(self.find(s, r'\w+', regex=True), [(0, 0, 0, 3), (1, 0, 1, 3)])
        # a regex anchored at a line start
        self.ae(self.find(s, '^d', regex=True), [(1, 0, 1, 1)])

    def test_wide_combining_and_multicell(self):
        s = self.screen(cols=10)
        self.lines(s, 'a日本b', 'éx ex', 'x\tyz')
        # wide chars take two cells
        self.ae(self.find(s, '日本b'), [(0, 1, 0, 6)])
        self.ae(self.find(s, '本'), [(0, 3, 0, 5)])
        # a combining char is part of its cell: e does not match the e of é
        self.ae(self.find(s, 'e'), [(1, 3, 1, 4)])
        self.ae(self.find(s, 'é'), [(1, 0, 1, 1)])
        self.ae(self.find(s, 'éx'), [(1, 0, 1, 2)])
        # a tab is one char covering its cells
        m = self.find(s, '\ty')
        self.ae(len(m), 1)
        self.ae((m[0][1], m[0][3]), (1, 9))
        # text sizing: a scaled char is found and covers its cells
        s = self.screen(cols=20, lines=6)
        s.draw('ab')
        draw_multicell(s, 'X', scale=2)
        s.draw('cd')
        m = self.find(s, 'bXc')
        self.ae(m, [(0, 1, 0, 5)])
        hl = s.search_highlights()
        # both rows of the scaled char are highlighted
        self.ae(hl[0][1:5], b'\x03\x03\x03\x03')
        self.ae(hl[1][2:4], b'\x03\x03')
        self.ae(hl[1][1:2], b'\x00')

    def test_case_folding(self):
        s = self.screen(cols=30)
        self.lines(s, 'STRASSE straße', 'ΣΊΣΥΦΟΣ σίσυφος')
        # full case folding in literal searches
        self.ae(len(self.find(s, 'strasse')), 2)
        self.ae(len(self.find(s, 'STRAẞE')), 2)
        self.ae(self.find(s, 'straße', True), [(0, 8, 0, 14)])
        self.ae(len(self.find(s, 'σίσυφος')), 2)
        # the folded text keeps the cells: ß folds to ss, so a match covers it whole
        self.ae(self.find(s, 'sse'), [(0, 4, 0, 7), (0, 12, 0, 14)])
        self.ae(self.find(s, 'sstr'), [])
        self.ae(self.find(s, 'ase'), [])  # starts in the middle of ß
        # regular expressions use Python's case insensitive matching
        self.ae(len(self.find(s, r'σ\w+ς', regex=True)), 2)

    def test_regex(self):
        s = self.screen(cols=30)
        self.lines(s, 'id=17 id=27 id=3', 'status=ok')
        self.ae(self.find(s, r'id=\d7\b', True, True), [(0, 0, 0, 5), (0, 6, 0, 11)])
        # empty matches are skipped
        self.ae(self.find(s, 'x*', regex=True), [])
        self.ae(len(self.find(s, '(?i)STATUS', True, True)), 1)

    def test_compressed_scrollback(self):
        # compressed history is searched exactly like plain history
        test_set_history_compression(*TINY_PARAMS)
        results = []
        for compression in (3, SCROLLBACK_COMPRESSION_NONE):
            s = self.screen(cols=12, lines=4, scrollback=10000, compression=compression)
            for i in range(600):
                s.draw(f'{i:04d} ' + ('needle wrapped around' if i % 37 == 0 else 'hay'))
                s.carriage_return()
                s.linefeed()
            if compression == 3:
                s.historybuf.wait_for_compression()
                self.assertGreater(s.historybuf.compression_stats()['compressed_lines'], 100)
            results.append((self.find(s, 'needle wrapped'), self.find(s, 'AROUND'), self.find(s, r'0\d3 hay', regex=True)))
        self.ae(results[0], results[1])
        self.ae(len(results[0][0]), 17)
        test_set_history_compression()

    def test_incremental_search(self):
        s = self.screen(cols=10, lines=3, scrollback=10000)
        for i in range(3000):
            self.lines(s, 'match' if i % 100 == 0 else f'l{i}')
        s.search_start('match', False, None, budget_ms=0)
        # the start searched the screen only, the scrollback is searched a slice at a time
        st = s.search_status()
        self.assertFalse(st['done'])
        self.assertGreater(st['lines_to_search'], 0)
        ticks = 0
        while s.search_tick(0.01):
            ticks += 1
        self.assertGreater(ticks, 3)
        st = s.search_status()
        self.assertTrue(st['done'])
        self.ae(st['total'], 30)
        # new output that scrolls into the scrollback is searched as it comes
        self.lines(s, 'new match', 'more', 'more', 'more', 'match again')
        s.search_run_to_completion()
        self.ae(s.search_status()['total'], 32)
        # changing a line of the screen changes its matches
        s.draw('match')
        s.search_run_to_completion()
        self.ae(s.search_status()['total'], 33)
        s.carriage_return()
        s.erase_in_line(2, False)
        s.search_run_to_completion()
        self.ae(s.search_status()['total'], 32)
        # an endless line that wraps from the scrollback onto the screen
        s = self.screen(cols=4, lines=3, scrollback=100)
        s.draw('xxxx' * 10 + 'ab')
        self.ae(len(self.find(s, 'ab')), 1)
        s.draw('cdab')
        s.search_run_to_completion()
        self.ae(len(s.search_matches()), 2)

    def test_cancel(self):
        s = self.screen(cols=10, lines=3, scrollback=10000)
        for i in range(3000):
            self.lines(s, 'alpha' if i % 2 else 'beta')
        s.search_start('alpha', False, None, budget_ms=0)
        s.search_tick(0.01)
        self.assertFalse(s.search_status()['done'])
        # a new query replaces the search in progress
        s.search_start('beta', False, None)
        self.ae(s.search_status()['query'], 'beta')
        s.search_run_to_completion()
        self.ae(s.search_status()['total'], 1500)
        self.assertTrue(all(str(s.historybuf.line(-r - 1)).startswith('beta') for r, *_ in s.search_matches() if r < 0))
        # stopping removes the matches and the highlights
        s.search_stop()
        self.ae(s.search_status(), None)
        self.ae(s.search_matches(), [])
        self.assertFalse(any(any(row) for row in s.search_highlights()))

    def test_navigation(self):
        s = self.screen(cols=10, lines=3, scrollback=100)
        for i in range(30):
            self.lines(s, 'hit' if i % 10 == 0 else f'x{i}')
        s.draw('hit')
        m = self.find(s, 'hit')
        self.ae(len(m), 4)
        # the first current match is the newest at or above the bottom of the screen
        self.ae(s.search_current(), m[3])
        self.ae(s.search_status()['current'], 4)
        # next goes up, to older output, as in iTerm2
        self.ae(s.search_navigate(-1), 'moved')
        self.ae(s.search_current(), m[2])
        # the view scrolls so that the match can be seen
        self.assertGreater(s.scrolled_by, 0)
        self.ae(s.search_navigate(-1), 'moved')
        self.ae(s.search_navigate(-1), 'moved')
        self.ae(s.search_current(), m[0])
        # at the oldest it wraps to the newest
        self.ae(s.search_navigate(-1), 'wrapped')
        self.ae(s.search_current(), m[3])
        self.assertTrue(s.search_status()['wrapped'])
        self.ae(s.search_navigate(1), 'wrapped')
        self.ae(s.search_current(), m[0])
        self.ae(s.search_navigate(1), 'moved')
        self.assertFalse(s.search_status()['wrapped'])
        self.ae(s.search_current(), m[1])
        # the current match is centered when it is not visible
        r = m[1][0]
        self.ae(r + s.scrolled_by, s.lines // 2)
        # nothing to move to
        self.find(s, 'absent')
        self.ae(s.search_navigate(-1), 'none')

    def test_navigation_while_searching(self):
        s = self.screen(cols=10, lines=3, scrollback=10000)
        self.lines(s, 'oldest hit')
        for i in range(3000):
            self.lines(s, f'x{i}')
        s.search_start('hit', False, None, budget_ms=0)
        self.ae(s.search_status()['total'], 0)
        # a move before the match is found waits for it
        self.ae(s.search_navigate(-1), 'pending')
        self.assertTrue(s.search_status()['pending'])
        s.search_run_to_completion()
        st = s.search_status()
        self.ae((st['total'], st['current'], st['pending']), (1, 1, False))
        self.ae(s.search_current()[0], -s.historybuf.count)

    def test_highlights(self):
        s = self.screen(cols=10, lines=3, scrollback=100)
        self.lines(s, 'ab ab')
        s.draw('ab')
        self.find(s, 'ab')
        hl = s.search_highlights()
        # the current match (the newest) is 3, the others 1
        self.ae(hl[0][:5], b'\x01\x01\x00\x01\x01')
        self.ae(hl[1][:2], b'\x03\x03')
        # highlights follow the view and the output
        self.lines(s, '', 'x', 'y')
        s.search_run_to_completion()
        cur = s.search_current()
        self.assertLess(cur[0], 0)
        s.scroll(-cur[0], True)
        self.ae(s.search_highlights()[0][:2], b'\x03\x03')
        self.ae(s.search_highlights()[1], bytes(10))
        # cells are not changed: marks and colors stay
        s.search_stop()
        self.ae(str(s.historybuf.line(0)).strip(), 'ab')

    def test_history_changes(self):
        s = self.screen(cols=10, lines=3, scrollback=10)
        for i in range(8):
            self.lines(s, f'hit {i}')
        self.ae(len(self.find(s, 'hit')), 8)
        # lines that leave the scrollback lose their matches
        for i in range(8):
            self.lines(s, 'x')
        s.search_run_to_completion()
        self.ae(len(s.search_matches()), 4)  # hit 4 to hit 7 are still in the scrollback
        self.assertTrue(all(r >= -s.historybuf.count for r, *_ in s.search_matches()))
        # clearing the scrollback starts the search again
        self.find(s, 'x')
        parse_bytes(s, b'\x1b[3J')
        s.search_run_to_completion()
        self.assertTrue(all(r >= 0 for r, *_ in s.search_matches()))
        # resizing rewraps and finds the matches again
        s = self.screen(cols=10, lines=3, scrollback=100)
        self.lines(s, 'aaaa needle', 'b')
        self.ae(self.find(s, 'needle'), [(-1, 5, 0, 1)])
        s.resize(3, 20)
        s.search_run_to_completion()
        self.ae([m[1:] for m in s.search_matches()], [(5, s.search_matches()[0][0], 11)])

    def test_alternate_screen(self):
        s = self.screen(cols=10, lines=3, scrollback=100)
        self.lines(s, 'main hit')
        s.toggle_alt_screen()
        s.draw('alt hit')
        m = self.find(s, 'hit')
        self.ae(m, [(0, 4, 0, 7)])
        s.toggle_alt_screen()
        s.search_run_to_completion()
        self.ae([x[1:] for x in s.search_matches()], [(5, s.search_matches()[0][0], 8)])

    def test_measure_large_scrollback(self):
        # many lines are searched a slice at a time, without any slice being long
        import time
        test_set_history_compression()
        s = self.screen(cols=80, lines=24, scrollback=10**7, compression=3)
        data = ''.join(f'{i:08d} log line with some words status=ok\r\n' + ('NEEDLE here\r\n' if i % 5000 == 0 else '') for i in range(100000))
        parse_bytes(s, data.encode())
        s.search_start('needle', False, None)
        longest = 0.
        while True:
            t = time.monotonic()
            more = s.search_tick(4)
            longest = max(longest, time.monotonic() - t)
            if not more and s.search_status()['done']:
                break
        self.ae(s.search_status()['total'], 20)
        # generous, for loaded test machines: a slice is a few ms
        self.assertLess(longest, 0.5)


class TestFindBar(BaseTest):

    def make(self, find_keys_to_terminal=None):
        from kitty.tabs import Tab
        from kitty.window import Window
        from kitty.profiles import Profile
        tab = Tab.__new__(Tab)
        tab.find_keys_to_terminal_override = None
        tab.windows = []
        manager = SimpleNamespace(profile=Profile('default', 'Default'))
        tab.tab_manager_ref = lambda: manager
        w = Window.__new__(Window)
        w.screen = self.create_screen(cols=20, lines=4, scrollback=100)
        w.id, w.os_window_id = 7, 3
        w.find_keys_to_terminal_override = find_keys_to_terminal
        w.find_prompt = None
        w.is_focused = False
        w.tabref = lambda: tab
        tab.windows.append(w)
        return tab, w

    def test_bar_and_prompt(self):
        from kitty import find
        self.set_options()
        tab, w = self.make()
        for t in ('apple pie', 'banana', 'apple tart'):
            w.screen.draw(t)
            w.screen.carriage_return()
            w.screen.linefeed()
        prompts = []
        w.set_find_prompt = lambda text: prompts.append(text)
        w.text_for_selection = lambda: 'banana'
        boss = SimpleNamespace(window_id_map={w.id: w})
        with patch('kitty.find.get_boss', return_value=boss), patch('kitty.fast_data_types.native_find_bar', return_value=False), \
                patch('kitty.find.add_timer', return_value=1), patch('kitty.find.remove_timer'):
            b = find.open_find_bar(w)
            self.assertTrue(b.visible)
            self.assertFalse(b.native)
            self.assertIsNotNone(b.prompt)
            b.set_query('apple')
            w.screen.search_run_to_completion()
            st = b.status()
            self.ae((st.total, st.current), (2, 2))
            self.ae(st.text, '2 of 2')
            self.assertIn('Find: apple', prompts[-1])
            b.refresh()
            self.assertIn('2 of 2', prompts[-1])
            # Return goes to the next match, up
            key = SimpleNamespace(key=find.GLFW_FKEY_ENTER, mods=0, text='')
            self.assertTrue(find.handle_key(w, key))
            self.ae(b.status().current, 1)
            # and wraps around, which is shown for a moment
            self.assertTrue(find.handle_key(w, key))
            self.ae(b.status().current, 2)
            self.assertIn('wrapped', b.status().text)
            self.assertTrue(find.handle_key(w, SimpleNamespace(key=find.GLFW_FKEY_ENTER, mods=find.GLFW_MOD_SHIFT, text='')))
            # typing edits the query
            self.assertTrue(find.handle_key(w, SimpleNamespace(key=ord(' '), mods=0, text=' ')))
            self.ae(b.query, 'apple ')
            self.assertTrue(find.handle_key(w, SimpleNamespace(key=find.GLFW_FKEY_BACKSPACE, mods=0, text='')))
            self.ae(b.query, 'apple')
            # toggles
            self.assertTrue(find.handle_key(w, SimpleNamespace(key=ord('c'), mods=find.GLFW_MOD_ALT, text='')))
            self.assertTrue(b.case_sensitive)
            self.assertTrue(find.handle_key(w, SimpleNamespace(key=ord('r'), mods=find.GLFW_MOD_ALT, text='')))
            self.assertTrue(b.regex)
            b.set_query('(')
            self.assertIn('Invalid expression', b.status().text)
            b.set_query('ap+le')
            w.screen.search_run_to_completion()
            self.ae(b.status().total, 2)
            # other shortcuts keep working while the prompt has the keyboard
            self.assertFalse(find.handle_key(w, SimpleNamespace(key=ord('t'), mods=find.GLFW_MOD_CONTROL | find.GLFW_MOD_SHIFT, text='')))
            # use selection for find
            b.set_regex(False)
            self.assertTrue(find.use_selection_for_find(w))
            self.ae(b.query, 'banana')
            w.screen.search_run_to_completion()
            self.ae(b.status().total, 1)
            # Escape closes, removing the highlights and the prompt
            self.assertTrue(find.handle_key(w, SimpleNamespace(key=find.GLFW_FKEY_ESCAPE, mods=0, text='')))
            self.assertNotIn(w.os_window_id, find.find_bars)
            self.ae(w.screen.search_status(), None)
            self.ae(prompts[-1], None)
            self.assertFalse(find.handle_key(w, key))
            # next with nothing to find opens the bar
            find.find_next(w, -1)
            self.assertTrue(find.find_bars[w.os_window_id].visible)
            find.close_find_bar(w.os_window_id)

    def test_native_bar(self):
        from kitty import find
        self.set_options()
        tab, w = self.make()
        w.screen.draw('hello world')
        calls = []

        def native(os_window_id, model, callback):
            calls.append((os_window_id, model))
            return True
        boss = SimpleNamespace(window_id_map={w.id: w})
        with patch('kitty.find.get_boss', return_value=boss), patch('kitty.fast_data_types.native_find_bar', side_effect=native), \
                patch('kitty.find.add_timer', return_value=1), patch('kitty.find.remove_timer'):
            b = find.open_find_bar(w)
            self.assertTrue(b.native)
            self.ae(calls[-1][1]['focus'], True)
            b.on_native_event('query', 'world')
            w.screen.search_run_to_completion()
            find.on_search_status_changed(w)
            self.ae(calls[-1][1]['status'], '1 of 1')
            self.ae(calls[-1][1]['query'], 'world')
            b.on_native_event('case_sensitive', True)
            self.assertTrue(calls[-1][1]['case_sensitive'])
            b.on_native_event('focus', True)
            # Wayland: keys the terminal gets while the native field has the keyboard are dropped
            self.assertTrue(find.handle_key(w, SimpleNamespace(key=ord('x'), mods=0, text='x')))
            b.on_native_event('close')
            self.ae(calls[-1][1], None)
            self.ae(w.screen.search_status(), None)

    def test_find_keys_to_terminal_precedence(self):
        # global < profile < tab < window
        from kitty.profiles import Profile, clear_caches, parsed_options
        clear_caches()
        self.addCleanup(clear_caches)
        tab, w = self.make()
        profile = Profile('vim', 'Vim', options={'find_keys_to_terminal': ['yes']})
        with patch('kitty.window.get_options') as go, patch('kitty.tabs.get_options') as go2:
            go.return_value = go2.return_value = SimpleNamespace(find_keys_to_terminal=False)
            self.assertFalse(w.find_keys_to_terminal)
            self.assertTrue(w.find_enabled)
            go.return_value = go2.return_value = SimpleNamespace(find_keys_to_terminal=True)
            self.assertTrue(w.find_keys_to_terminal)
            go.return_value = go2.return_value = SimpleNamespace(find_keys_to_terminal=False)
            self.ae(parsed_options(profile)['find_keys_to_terminal'], True)
            tab.tab_manager_ref = lambda: SimpleNamespace(profile=profile)
            self.assertTrue(w.find_keys_to_terminal)
            tab.set_find_keys_to_terminal('no')
            self.assertFalse(w.find_keys_to_terminal)
            self.assertFalse(tab.find_keys_to_terminal)
            w.set_find_keys_to_terminal(True)
            self.assertTrue(w.find_keys_to_terminal)
            w.set_find_enabled(True)
            self.assertFalse(w.find_keys_to_terminal)
            # setting the tab again applies to all its windows
            tab.set_find_enabled(False)
            self.ae(w.find_keys_to_terminal_override, None)
            self.assertTrue(w.find_keys_to_terminal)
            w.set_find_keys_to_terminal(None)
            tab.set_find_keys_to_terminal(None)
            self.assertTrue(w.find_keys_to_terminal)  # the profile
            with self.assertRaises(ValueError):
                w.set_find_keys_to_terminal('maybe')

    def test_session_launch_rc(self):
        from kitty.launch import clone_safe_opts, parse_launch_args
        from kitty.rc.base import command_for_name, parse_subcommand_cli
        from kitty.rc.set_find_keys_to_terminal import parse_value
        from kitty.session import parse_session

        opts = self.set_options()
        sessions = list(parse_session('new_tab one\nfind_keys_to_terminal yes\nlaunch --find-keys-to-terminal=no sh\nnew_tab two\nlaunch sh\n', opts))
        tabs = sessions[0].tabs
        self.ae((tabs[0].find_keys_to_terminal, tabs[1].find_keys_to_terminal), (True, None))
        self.ae(tabs[0].windows[0].launch_spec.opts.find_keys_to_terminal, 'no')
        self.assertIn('find_keys_to_terminal', clone_safe_opts())
        self.ae(parse_launch_args([]).opts.find_keys_to_terminal, 'default')
        with self.assertRaises(ValueError):
            list(parse_session('find_keys_to_terminal maybe\n', opts))

        self.ae((parse_value('yes'), parse_value('no'), parse_value('default')), (True, False, None))
        cmd = command_for_name('set_find_keys_to_terminal')
        o, items = parse_subcommand_cli(cmd, ['set-find-keys-to-terminal', '--match-tab', 'id:1', 'yes'])
        payload = cmd.message_to_kitty(SimpleNamespace(), o, items)
        self.ae((payload['value'], payload['match_tab']), ('yes', 'id:1'))
        tab, w1 = self.make()
        boss = SimpleNamespace(active_window=w1, all_windows=[w1], match_tabs=lambda m: [tab], match_windows=lambda m, w: [w1])
        cmd.response_from_kitty(boss, None, {'value': 'yes'}.get)
        self.ae(w1.find_keys_to_terminal_override, True)
        cmd.response_from_kitty(boss, None, {'value': 'no', 'match_tab': 'id:1'}.get)
        self.ae((tab.find_keys_to_terminal_override, w1.find_keys_to_terminal_override), (False, None))
        # the set_find_keys_to_terminal action
        from kitty.boss import Boss
        boss.window_for_dispatch, boss.show_error = w1, lambda *a: None
        Boss.set_find_keys_to_terminal(boss, 'toggle')
        self.ae(w1.find_keys_to_terminal_override, True)
        Boss.set_find_keys_to_terminal(boss, 'default')
        self.ae(w1.find_keys_to_terminal_override, None)

    def test_tab_settings_field(self):
        from kitty.tab_settings import TabSettingsController, tab_settings_model
        self.set_options()
        tab, w = self.make()
        tab.name, tab.minimum_contrast_override, tab.scrollback_settings = '', None, (-1, 'none')
        with patch.object(type(tab), 'title', 'x', create=True), patch.object(type(tab), 'minimum_contrast', 0., create=True), \
                patch('kitty.tab_settings.current_scrollback_settings', return_value=(-1, 'none')):
            tm = SimpleNamespace(active_tab=tab, profile=tab.tab_manager_ref().profile)
            fields = {f['name']: f for f in tab_settings_model(tm)['fields']}
            self.ae(fields['find_keys_to_terminal']['value'], 'default')
            self.assertIn('Send Find shortcuts to the terminal', fields['find_keys_to_terminal']['title'])
            c = TabSettingsController()
            self.ae(c.set_find_keys_to_terminal(tm, 'yes'), '')
            self.ae(tab.find_keys_to_terminal_override, True)
            self.assertTrue(c.set_find_keys_to_terminal(tm, 'bogus'))

    def test_shortcuts(self):
        from kitty.constants import is_macos
        from kitty.options.types import defaults
        from kitty.options.utils import parse_shortcut
        actions = {kd.definition: [] for kd in defaults.map if kd.definition.startswith('find')}
        for kd in defaults.map:
            if kd.definition in actions:
                actions[kd.definition].append(kd.trigger)
        self.ae(set(actions), {'find', 'find_next', 'find_previous', 'find_selection'})
        opts = self.set_options()

        def keys(action):
            return {t.resolve_kitty_mod(opts.kitty_mod) for t in actions[action]}
        self.assertIn(parse_shortcut('ctrl+shift+alt+f'), keys('find'))
        self.assertIn(parse_shortcut('ctrl+shift+alt+g'), keys('find_next'))
        self.assertIn(parse_shortcut('ctrl+shift+alt+h'), keys('find_previous'))
        self.assertIn(parse_shortcut('ctrl+shift+alt+e'), keys('find_selection'))
        if is_macos:
            self.assertIn(parse_shortcut('cmd+f'), keys('find'))
            self.assertIn(parse_shortcut('cmd+g'), keys('find_next'))
            self.assertIn(parse_shortcut('shift+cmd+g'), keys('find_previous'))
            self.assertIn(parse_shortcut('cmd+e'), keys('find_selection'))
        # the default keys of Find do not clash with other default keys
        triggers = {}
        for kd in defaults.map:
            if not kd.is_sequence and not kd.options.mode:
                triggers.setdefault(kd.trigger, set()).add(kd.definition.split()[0])
        for a in actions:
            for t in actions[a]:
                self.ae(triggers[t], {a}, f'{t} clashes')

    def test_keys_go_to_terminal(self):
        import kitty.fast_data_types as defines
        from kitty.config import load_config
        from kitty.keys import Mappings
        from kitty.options.utils import parse_shortcut
        self.set_options()
        win = SimpleNamespace(find_keys_to_terminal=False, id=1)

        class TM(Mappings):
            global_shortcuts_for_test: dict = {}

            def __init__(self, *lines):
                self.options = load_config(overrides=lines)
                super().__init__()

            def get_active_window(self):
                return win

            def combine(self, action_definition: str) -> bool:
                self.actions.append(action_definition)
                return True

            def set_cocoa_global_shortcuts(self, opts):
                return dict(self.global_shortcuts_for_test)

            def get_options(self):
                return self.options

            def set_ignore_os_keyboard_processing(self, on: bool) -> None:
                pass

            def debug_print(self, *a, end='\n') -> None:
                pass

            def __call__(self, key):
                self.actions = []
                sk = parse_shortcut(key)
                return self.dispatch_possible_special_key(defines.KeyEvent(sk.key, 0, 0, sk.mods))

        tm = TM('map f5 find')
        self.assertTrue(tm('ctrl+shift+alt+f'))
        self.ae(tm.actions, ['find'])
        win.find_keys_to_terminal = True
        # the default key goes to the program, the actions stay mappable to other keys
        self.assertFalse(tm('ctrl+shift+alt+f'))
        self.ae(tm.actions, [])
        self.assertFalse(tm('ctrl+shift+alt+g'))
        self.assertTrue(tm('f5'))
        self.ae(tm.actions, ['find'])
        # other actions are not affected
        self.assertTrue(tm('ctrl+shift+t'))
        win.find_keys_to_terminal = False
        # macOS: a key of a menu item reaches the terminal only when the menu item does not take it
        sk = parse_shortcut('ctrl+shift+alt+f')
        TM.global_shortcuts_for_test = {'find': sk}
        tm = TM()
        with patch('kitty.keys.is_macos', True), patch('kitty.keys.get_boss', return_value=SimpleNamespace(active_window=win)), \
                patch('kitty.keys.grab_keyboard', return_value=False):
            self.assertTrue(tm('ctrl+shift+alt+f'))
            win.find_keys_to_terminal = True
            self.assertFalse(tm('ctrl+shift+alt+f'))
        TM.global_shortcuts_for_test = {}

    def test_menu_actions_always_work(self):
        # the menu runs the actions directly, whatever find_keys_to_terminal says
        from kitty import find
        from kitty.native_ui import native_menu_model
        self.set_options()
        tab, w = self.make(find_keys_to_terminal=True)
        w.screen.draw('menu item')
        w.set_find_prompt = lambda text: None
        boss = SimpleNamespace(window_id_map={w.id: w}, active_window=w)
        with patch('kitty.find.get_boss', return_value=boss), patch('kitty.fast_data_types.native_find_bar', return_value=False), \
                patch('kitty.native_ui.get_boss', return_value=boss):
            w.find()
            self.assertTrue(find.find_bars[w.os_window_id].visible)
            find.close_find_bar(w.os_window_id)
            from kitty.constants import is_macos
            if not is_macos:
                model = native_menu_model()
                edit = next(m for m in model if m['title'] == 'Edit')
                find_menu = next(m for m in edit['items'] if m.get('title') == 'Find')
                self.ae([i['action'] for i in find_menu['items'] if 'action' in i][:4], ['find', 'find_next', 'find_previous', 'find_selection'])
