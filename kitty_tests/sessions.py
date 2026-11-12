#!/usr/bin/env python
# License: GPL v3 Copyright: 2026, Kovid Goyal <kovid at kovidgoyal.net>

from pathlib import Path
from tempfile import TemporaryDirectory
from unittest.mock import Mock, patch

from kitty.session import save_as_session

from .base import BaseTest


class TestSessions(BaseTest):
    def test_save_as_session_empty_match(self) -> None:
        boss = Mock(active_session='work')
        match = '--match=title:^editor$'
        with TemporaryDirectory() as tmp:
            existing = Path(tmp, 'work.kitty-session')
            new = Path(tmp, 'new', 'work.kitty-session')
            with patch.dict('kitty.session.seen_session_paths', work=str(existing)):
                boss.serialize_state_as_session.return_value = iter(('launch',))
                save_as_session(boss, ['--save-only', match, '.'])
                self.ae(existing.read_bytes(), b'launch')
                boss.serialize_state_as_session.return_value = iter(())
                with self.subTest(destination='active session'):
                    save_as_session(boss, [match, '.'])
                    self.ae(existing.read_bytes(), b'launch')
                with self.subTest(destination='new file'):
                    save_as_session(boss, ['--save-only', match, str(new)])
                    self.assertFalse(new.parent.exists())
        self.ae(boss.show_error.call_count, 2)
        boss.edit_file.assert_not_called()
