"""The same keys, sent to the installed Midnight Commander and to this build: the
title, the entries and the cursor must agree at every step. Skipped if the
installed mc is not there (apt-get install mc).

Known differences, which the fixtures here avoid: the reference hides members
named "./x", lists names over 255 bytes, sizes a hard link by its target, and
marks a link to nothing as stale.
"""
import os
import random
import unittest

import fixtures
import term
from case import MCTestCase
from term import DOWN, ENTER, END, HOME, PGDN, PGUP, UP

ARCHIVE_ENDINGS = (".zip", ".tgz", ".tar", ".tar.gz", ".txz", ".tar.xz", ".tar.zst", ".tar.zstd")
DEPTH = 4


def record(t, depth=0):
    """The screen at each step of a walk down every directory of the panel that
    is open: (title, entries, cursor) at each step."""
    steps = [(t.title(), t.entries(), t.cursor_name())]
    if depth >= DEPTH:
        return steps
    for i, (name, _) in enumerate(t.entries()):
        if not name.startswith("/") or name.startswith("/.."):
            continue
        t.goto(i)
        t.send(ENTER, 0.9)
        steps += record(t, depth + 1)
        t.goto(0)
        t.send(ENTER, 0.9)
        steps.append((t.title(), t.entries(), t.cursor_name()))
    return steps


@unittest.skipUnless(os.path.exists(term.REFERENCE), "the installed mc is not there")
class ReferenceTests(MCTestCase):

    def both(self, d, open_archive=None):
        """Run record() on the reference and on this build, from directory D."""
        results = []
        for start in (self.start_reference, self.start):
            t = start(d)
            if open_archive:
                t.goto(t.find(open_archive))
                t.send(ENTER, 2.0)
            results.append(record(t))
            t.close()
        return results

    def assert_same(self, ref, ours):
        self.assertEqual(len(ref), len(ours), "steps")
        for i, (a, b) in enumerate(zip(ref, ours)):
            self.assertEqual(a, b, "step %d" % i)

    def test_zip_archives_match(self):
        for name, files, dirs, dir_entries in (
                ("notes.zip", fixtures.ZIP_NOTES, ["src"], True),
                ("implicit.zip", fixtures.ZIP_IMPLICIT, [], False),
                ("deep.zip", fixtures.ZIP_DEEP, [], False)):
            with self.subTest(archive=name):
                d = self.new_dir(name)
                fixtures.zip_archive(os.path.join(d, name), files, dirs=dirs,
                                     dir_entries=dir_entries)
                ref, ours = self.both(d, name)
                self.assert_same(ref, ours)

    def test_compressed_tar_matches(self):
        d = self.new_dir()
        fixtures.tar_archive(d, "basic", fixtures.BASIC, "tgz", "gnu")
        ref, ours = self.both(d, "basic.tgz")
        self.assert_same(ref, ours)

    def test_host_paging_matches(self):
        d = self.new_dir()
        for i in range(120):
            fixtures.put(os.path.join(d, "item%03d.dat" % i))
        rnd = random.Random(2)
        keys = [UP, DOWN, PGUP, PGDN, END, HOME]
        seq = [rnd.choice(keys) for _ in range(40)]
        results = []
        for start in (self.start_reference, self.start):
            t = start(d)
            steps = [(t.title(), t.entries(), t.cursor_name())]
            for key in seq:
                t.send(key, .2)
                steps.append((t.title(), t.entries(), t.cursor_name()))
            t.close()
            results.append(steps)
        self.assert_same(*results)

    def test_random_walk_through_archives_matches(self):
        d = self.new_dir()
        fixtures.zip_archive(os.path.join(d, "implicit.zip"), fixtures.ZIP_IMPLICIT,
                             dir_entries=False)
        fixtures.zip_archive(os.path.join(d, "deep.zip"), fixtures.ZIP_DEEP, dir_entries=False)
        fixtures.tar_archive(d, "basic", fixtures.BASIC, "tgz", "gnu")
        rnd = random.Random(5)
        seq = [rnd.choice(["up", "down", "pgdn", "end", "home", "enter"]) for _ in range(30)]
        keys = {"up": UP, "down": DOWN, "pgdn": PGDN, "end": END, "home": HOME, "enter": ENTER}
        results = []
        for start in (self.start_reference, self.start):
            t = start(d)
            steps = [(t.title(), t.entries(), t.cursor_name())]
            for name in seq:
                if name == "enter":
                    row = t.cursor_name() or ""
                    if not (row.startswith("/") or row.strip().endswith(ARCHIVE_ENDINGS)):
                        continue                    # Enter on a file would run a viewer
                t.send(keys[name], .25)
                steps.append((t.title(), t.entries(), t.cursor_name()))
            t.close()
            results.append(steps)
        self.assert_same(*results)


if __name__ == "__main__":
    unittest.main()
