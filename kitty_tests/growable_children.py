#!/usr/bin/env python
# License: GPLv3 Copyright: 2026, Adam DePrince

from kitty.fast_data_types import test_child_arrays, test_monitored_pids, test_reserve_os_windows

from .base import BaseTest

# The fixed arrays that were replaced had room for 512 children and OS windows and 256 monitored pids, and only 64
# reaped pids were reported at a time
OLD_CAP = 512


class TestGrowableChildren(BaseTest):

    def test_many_children(self):
        count = 10 * OLD_CAP
        left, removed, capacity = test_child_arrays(count, 0)
        # each batch the I/O thread takes is taken from the end of the queue, as it always was
        self.ae(sorted(left), list(range(1, count + 1)))
        self.ae(removed, [])
        self.assertGreaterEqual(capacity, count)

    def test_children_removed_keep_the_order_of_the_rest(self):
        count = 3 * OLD_CAP + 7
        order = test_child_arrays(count, 0)[0]
        left, removed, capacity = test_child_arrays(count, 3)
        self.ae(left, [i for i in order if i % 3])
        # removed from the end, as the I/O thread does
        self.ae(removed, [i for i in reversed(order) if i % 3 == 0])
        # the arrays keep their room, so doing it again needs no more
        left2, removed2, capacity2 = test_child_arrays(count, 3)
        self.ae((left2, removed2), (left, removed))
        self.ae(capacity2, capacity)

    def test_many_monitored_pids_are_all_reported(self):
        count = 20 * 256
        reaped, monitored = test_monitored_pids(count)
        self.ae((reaped, monitored), (count, 0))

    def test_many_os_windows(self):
        ok, capacity = test_reserve_os_windows(4 * OLD_CAP)
        self.assertTrue(ok)
        self.assertGreaterEqual(capacity, 4 * OLD_CAP)
