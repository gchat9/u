"""The panels of a directory: Alt+Enter on the command line, Enter, and the
command line itself."""
import os
import tarfile
import unittest

import fixtures
from case import MCTestCase
from term import ALT_C, ALT_ENTER, ALT_LF, DOWN, ENTER, LEFT


def cmdline(t):
    return t.line(23).rstrip()


class AltEnterTests(MCTestCase):

    def files(self, names):
        d = self.new_dir()
        fixtures.tree_files(d, names)
        return d

    @unittest.skipUnless(fixtures.have("tar"), "needs tar")
    def test_spec_example(self):
        d = self.files(["foo", "one", "two"])
        t = self.start(d)
        t.send(b"tar czf archive.tgz ")
        t.send(DOWN, .2)
        t.send(DOWN, .2)
        t.send(ALT_ENTER, .3)                 # "one"
        t.send(DOWN, .2)
        t.send(ALT_ENTER, .3)                 # "two"
        self.assertEqual(cmdline(t), "$ tar czf archive.tgz one two")
        t.send(ENTER, 1.5)
        with tarfile.open(os.path.join(d, "archive.tgz")) as tf:
            self.assertEqual(tf.getnames(), ["one", "two"])

    def test_names_are_quoted_for_the_shell(self):
        names = ["a b", "-x", "~", "#h", "x$y", "p&q"]
        d = self.files(names)
        t = self.start(d)
        t.send(b"printf '[%s]' > out.txt ")
        names = [shown[1:] for shown, _ in t.entries()[1:]]   # without "..", and the mark
        for _ in names:
            t.send(DOWN, .2)
            t.send(ALT_ENTER, .3)
        t.send(ENTER, 1.5)
        with open(os.path.join(d, "out.txt")) as f:
            got = f.read()
        # printf prints each argument: "-x" must be given as "./-x" to reach it intact
        want = "".join("[%s]" % (("./" + n) if n.startswith("-") else n) for n in names)
        self.assertEqual(got, want)

    def test_dot_dot_inserts_dot_dot(self):
        t = self.start(self.files(["foo"]))
        t.send(b"ls ")
        t.send(ALT_ENTER, .3)
        self.assertEqual(cmdline(t), "$ ls ..")

    def test_inserts_at_the_cursor(self):
        t = self.start(self.files(["foo", "one", "two"]))
        t.send(b"cat END")
        for _ in range(3):
            t.send(LEFT, .2)
        t.send(DOWN, .2)
        t.send(DOWN, .2)
        t.send(ALT_ENTER, .3)
        self.assertEqual(cmdline(t), "$ cat one END")

    def test_alt_linefeed_is_the_same_key(self):
        t = self.start(self.files(["foo", "one", "two"]))
        t.send(b"echo ")
        t.send(DOWN, .2)
        t.send(DOWN, .2)
        t.send(ALT_LF, .3)
        self.assertEqual(cmdline(t), "$ echo one")

    def test_ignored_while_a_dialog_is_open(self):
        t = self.start(self.files(["foo"]))
        t.send(b"ab")
        t.send(ALT_C, .5)                     # the quick cd box
        t.send(DOWN, .2)
        t.send(ALT_ENTER, .3)
        self.assertEqual(cmdline(t), "$ ab")

    def test_a_name_that_does_not_fit_is_not_typed(self):
        for typed, inserted in ((253, False), (251, True)):
            with self.subTest(typed=typed):
                d = self.files(["one"])
                t = self.start(d)
                t.send(b"x" * typed)
                before = t.line(23)
                t.send(DOWN, .2)
                t.send(ALT_ENTER, .3)
                self.assertEqual(t.line(23) != before, inserted)


class EnterTests(MCTestCase):

    def test_enter_goes_into_a_directory_and_runs_commands(self):
        d = self.new_dir()
        fixtures.tree_files(os.path.join(d, "sub"), ["inner"])
        t = self.start(d)
        t.goto(t.find("sub"))
        t.send(ENTER, .8)
        self.assertEqual(t.title(), os.path.join(d, "sub"))
        t.send(b"echo hi > h.txt", .2)
        t.send(ENTER, 1.0)
        self.assertTrue(os.path.exists(os.path.join(d, "sub", "h.txt")))


if __name__ == "__main__":
    unittest.main()
