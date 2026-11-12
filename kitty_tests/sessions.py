#!/usr/bin/env python
# License: GPL v3 Copyright: 2026, Kovid Goyal <kovid at kovidgoyal.net>

from pathlib import Path
from tempfile import TemporaryDirectory
from unittest.mock import Mock, patch

from kitty.session import save_as_session

from .base import BaseTest


class TestSessions(BaseTest):
    def test_save_as_session_nothing_to_save(self) -> None:
        boss = Mock(active_session='work')
        serialized: tuple[str, ...] = ('launch',)
        matched: tuple[str, ...] = ()
        boss.serialize_state_as_session.side_effect = lambda *a: iter(serialized)
        boss.match_windows.side_effect = lambda *a: iter(matched)

        def save(*args: str) -> None:
            for m in (boss.show_error, boss.match_windows, boss.get_save_filepath):
                m.reset_mock()
            save_as_session(boss, args)

        def assert_error(*fragments: str) -> None:
            self.ae(boss.show_error.call_count, 1)
            title, msg = boss.show_error.call_args.args
            self.ae(title, 'No windows to save')
            for frag in fragments:
                self.assertIn(frag, msg)

        match = '--match=title:^editor$'
        with TemporaryDirectory() as tmp:
            existing = Path(tmp, 'work.kitty-session')
            new = Path(tmp, 'new', 'work.kitty-session')
            with patch.dict('kitty.session.seen_session_paths', work=str(existing)):
                with self.subTest(destination='active session'):
                    save('--save-only', match, '.')
                    self.ae(existing.read_bytes(), b'launch')
                    boss.show_error.assert_not_called()
                with self.subTest(destination='prompt for path, windows closed before answer'):
                    save(match)
                    boss.get_save_filepath.assert_called_once()
                    callback = boss.get_save_filepath.call_args.args[1]
                    serialized = ()
                    callback(str(new))
                    self.assertFalse(new.parent.exists())
                    assert_error('No windows match: title:^editor$')
                with self.subTest(destination='active session, no windows match'):
                    save(match, '.')
                    self.ae(existing.read_bytes(), b'launch')
                    assert_error('No windows match: title:^editor$')
                    boss.match_windows.assert_called_with('title:^editor$')
                matched = ('window',)
                with self.subTest(destination='active session, matched windows not saveable'):
                    save(match, '.')
                    self.ae(existing.read_bytes(), b'launch')
                    assert_error('None of the matching windows')
                with self.subTest(destination='active session, no --match'):
                    save('--save-only', '.')
                    self.ae(existing.read_bytes(), b'launch')
                    assert_error('None of the windows')
                    boss.match_windows.assert_not_called()
                with self.subTest(destination='new file'):
                    save('--save-only', match, str(new))
                    self.assertFalse(new.parent.exists())
                    assert_error('None of the matching windows')
                with self.subTest(destination='prompt for path, nothing to save'):
                    save(match)
                    boss.get_save_filepath.assert_not_called()
                    assert_error('None of the matching windows')
        boss.edit_file.assert_not_called()
