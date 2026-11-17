#!/usr/bin/env python
# License: GPL v3 Copyright: 2026, Kovid Goyal <kovid at kovidgoyal.net>

import os
import subprocess

from kitty.child import memory_used_by_process_tree_rooted_at
from kitty.constants import is_macos, kitty_exe

from .base import BaseTest


def spawn_and_wait_for_ready(code: str, what: str) -> subprocess.Popen:
    # Run code via kitty +runpy and wait for it to print ready. stderr is
    # captured, as in test workers it is /dev/null, so that the reason the
    # child failed is reported.
    p = subprocess.Popen([kitty_exe(), '+runpy', code], stdout=subprocess.PIPE, stderr=subprocess.PIPE)
    line = p.stdout.readline().strip()
    p.stdout.close()
    if line != b'ready':
        p.kill()
        err = p.stderr.read().decode('utf-8', 'replace')
        p.stderr.close()
        raise AssertionError(f'Unexpected output from {what}: {line!r} exit code: {p.wait()} stderr:\n{err}')
    p.stderr.close()
    return p


class ChildMemoryTest(BaseTest):
    def _spawn_allocating_child(self, alloc_bytes: int) -> subprocess.Popen:
        return spawn_and_wait_for_ready(
            f"""\
import sys, time
buf = bytearray({alloc_bytes})
for i in range(0, {alloc_bytes}, 4096):
    buf[i] = 1
sys.stdout.write("ready\\n")
sys.stdout.flush()
time.sleep(300)
""",
            'allocating child',
        )

    def _terminate(self, p: subprocess.Popen) -> None:
        p.terminate()
        p.wait()

    def test_memory_returns_positive_for_live_process(self):
        mem = memory_used_by_process_tree_rooted_at(os.getpid())
        self.assertGreater(mem, 0)

    def test_memory_returns_minus_one_for_nonexistent_pid(self):
        self.ae(memory_used_by_process_tree_rooted_at(99999999), -1)

    def test_memory_accounts_for_child_allocation(self):
        # Verify that a child's known resident allocation shows up in the
        # measurement.  check_if_cgroup_root=True falls back to per-process
        # tree walk when pid is not the cgroup root (the common case when
        # running under a shared session cgroup), so this exercises the tree
        # walk path on Linux and the always-tree-walk path on macOS.
        alloc = 20 * 1024 * 1024  # 20 MiB
        child = self._spawn_allocating_child(alloc)
        try:
            mem = memory_used_by_process_tree_rooted_at(child.pid, check_if_cgroup_root=True)
            self.assertGreater(
                mem,
                alloc // 2,
                f'Expected at least {alloc // 2} bytes for a {alloc}-byte allocation, got {mem}',
            )
        finally:
            self._terminate(child)

    def test_memory_of_parent_tree_includes_child(self):
        # Measuring the current process tree must yield more than measuring
        # the child alone, because the test runner itself occupies memory.
        alloc = 20 * 1024 * 1024  # 20 MiB
        child = self._spawn_allocating_child(alloc)
        try:
            mem_child = memory_used_by_process_tree_rooted_at(child.pid, check_if_cgroup_root=True)
            mem_tree = memory_used_by_process_tree_rooted_at(os.getpid(), check_if_cgroup_root=True)
            self.assertGreater(
                mem_tree,
                mem_child,
                'Parent tree memory should exceed child-only memory',
            )
        finally:
            self._terminate(child)

    def test_memory_cgroup_path_returns_positive(self):
        # The fast cgroup path (check_if_cgroup_root=False, the default) must
        # return a usable value on Linux.
        if is_macos:
            self.skipTest('cgroup not available on macOS')
        mem = memory_used_by_process_tree_rooted_at(os.getpid())
        self.assertGreater(mem, 0)

    def test_memory_cgroup_and_tree_walk_both_positive(self):
        # Both the cgroup path and the tree-walk path should give positive
        # results for a process that is alive.
        if is_macos:
            self.skipTest('cgroup path not applicable on macOS')
        alloc = 10 * 1024 * 1024  # 10 MiB
        child = self._spawn_allocating_child(alloc)
        try:
            mem_cgroup = memory_used_by_process_tree_rooted_at(child.pid, check_if_cgroup_root=False)
            mem_walk = memory_used_by_process_tree_rooted_at(child.pid, check_if_cgroup_root=True)
            self.assertGreater(mem_cgroup, 0)
            self.assertGreater(mem_walk, 0)
        finally:
            self._terminate(child)

    def test_descendants_from_threads(self):
        if is_macos:
            self.skipTest('Per thread children lists are Linux only')
        from kitty import child

        # A process that starts its child from a non-main thread, as kitten does
        code = """\
import sys, threading, time
def run_child(started):
    import subprocess, sys, time
    subprocess.Popen(
        [sys.executable, '-c', 'import time; time.sleep(300)'],
        stdin=subprocess.DEVNULL, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    started.set()
    time.sleep(300)  # keep the thread alive so the child stays its child
started = threading.Event()
threading.Thread(target=run_child, args=(started,), daemon=True).start()
if not started.wait(10):
    raise SystemExit('Failed to start child')
sys.stdout.write("ready\\n")
sys.stdout.flush()
time.sleep(300)
"""
        p = spawn_and_wait_for_ready(code, 'wrapper')
        inner: list[int] = []
        try:
            inner = child.children_of_process(p.pid)
            self.ae(len(inner), 1)
            self.ae(child._get_descendants_of(p.pid), set(inner))
            # Kernels without /proc/<pid>/task/<tid>/children
            orig = child.proc_children_supported
            child.proc_children_supported = lambda: False  # type: ignore
            try:
                self.ae(child.children_of_process(p.pid), inner)
                self.ae(child._get_descendants_of(p.pid), set(inner))
            finally:
                child.proc_children_supported = orig
        finally:
            p.terminate()
            p.wait()
            for c in inner:
                try:
                    os.kill(c, 15)
                except ProcessLookupError:
                    pass
