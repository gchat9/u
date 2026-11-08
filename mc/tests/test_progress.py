"""The progress of a copy: the bytes written to the terminal stay far below the bytes
copied, and a transfer shows its Total bar. The transfers to a link need key login to
root at 127.0.0.1, as test_shell does."""
import filecmp
import os
import time
import unittest

from case import MCTestCase
from term import DOWN, ENTER, TAB
from test_shell import NEEDS_SSH, link

F5 = b"\x1b[15~"


def progress_rows(t):
    """The rows of the progress box that show a bar, and the box's Total bar, if any."""
    total = [t.line(y) for y in range(24) if "Total [" in t.line(y)]
    return total


class ProgressTests(MCTestCase):

    def test_many_small_files_write_far_less_to_the_terminal_than_they_copy(self):
        base = self.new_dir()
        src, dst = os.path.join(base, "src"), os.path.join(base, "dst")
        os.makedirs(src)
        os.makedirs(dst)
        for i in range(1500):
            with open(os.path.join(src, "f%04d" % i), "wb") as f:
                f.write(b"x" * 100)
        t = self.start(base)
        t.send(TAB, .4)                       # the right panel: into dst
        t.send(DOWN, .3)
        t.send(ENTER, .6)
        t.send(TAB, .4)                       # left, on src
        t.send(DOWN, .3)
        t.send(DOWN, .3)
        t.sent = 0
        t.send(F5, .3)
        t.send(ENTER, 0.5)
        deadline = time.time() + 60
        while time.time() < deadline and len(os.listdir(os.path.join(dst, "src"))) < 1500:
            t.pump(0.3, 0.5)
        t.pump(1.0, 2.0)
        copied = 1500 * 100
        self.assertEqual(len(os.listdir(os.path.join(dst, "src"))), 1500)
        self.assertLess(t.sent, copied)

    @NEEDS_SSH
    def test_a_transfer_to_a_link_shows_its_total(self):
        base = self.new_dir()
        src, remote = os.path.join(base, "src"), os.path.join(base, "remote")
        os.makedirs(src)
        os.makedirs(remote)
        blob = os.urandom(4 * 1024 * 1024)
        for i in range(8):
            with open(os.path.join(src, "f%d" % i), "wb") as f:
                f.write(blob)
        t = self.start(base)
        t.send(TAB, .4)
        t.send(("cd " + link(remote) + "\r").encode(), 5.0)
        t.send(TAB, .4)
        t.send(DOWN, .3)
        t.send(DOWN, .3)
        t.send(F5, .3)
        t.send(ENTER, 0.0)
        seen = False
        for _ in range(40):
            t.pump(0.1, 0.2)
            if progress_rows(t):
                seen = True
                break
        self.assertTrue(seen, "the progress box with a Total bar was not drawn")
        t.pump(2.0, 4.0)
        for i in range(8):
            self.assertTrue(filecmp.cmp(os.path.join(src, "f%d" % i),
                                        os.path.join(remote, "src", "f%d" % i), shallow=False))


if __name__ == "__main__":
    unittest.main()
