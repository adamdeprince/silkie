#!/usr/bin/env python
# License: GPLv3 Copyright: 2026, Adam DePrince

from kitty.fast_data_types import test_vao_slot_table

from .base import BaseTest

# The fixed tables that were replaced had room for 4 * MAX_CHILDREN + 10 VAOs and 6 * MAX_CHILDREN + 4 buffers
OLD_VAO_CAP = 4 * 512 + 10


class TestGLTables(BaseTest):

    def test_tables_grow_past_the_old_cap(self):
        count = 50 * OLD_VAO_CAP
        ok, capacity = test_vao_slot_table(count, 0)
        self.assertTrue(ok)
        self.assertGreaterEqual(capacity, count)
        self.assertLess(capacity, 2 * count + 64)

    def test_freed_entries_are_reused(self):
        # every third entry is freed straight after it is taken, so the next take gets its index again and the
        # table holds two thirds of what was taken
        count = 3 * OLD_VAO_CAP
        ok, capacity = test_vao_slot_table(count, 3)
        self.assertTrue(ok)
        self.assertGreaterEqual(capacity, 2 * OLD_VAO_CAP)
        self.assertLess(capacity, 2 * (2 * OLD_VAO_CAP) + 64)
        ok, capacity = test_vao_slot_table(1000, 1)
        self.assertTrue(ok)
        self.assertEqual(capacity, 64)
