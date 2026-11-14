"""Command, Compare directories (F9, then Command): the question of the method, and
the marks each method sets on the two panels, each against the other. The expected
marks are those the installed mc gives on the same files (test_reference_matches
checks them against it, where it is installed)."""
import os
import time
import unittest

import fixtures
import oracle
import term
from case import MCTestCase
from term import DOWN, ENTER, ESC, RIGHT, TAB
from test_menu import bar_selected

F9 = b"\x1b[20~"
INSERT = b"\x1b[2~"
CTRL_X_D = b"\x18d"
QUICK, SIZE, THOROUGH = b"q", b"s", b"t"
T0, T1, T2 = 1700000000, 1700000100, 1700000200     # older, newer, newest

# The fixture. A file is (bytes, mtime); None is a directory; ("link", target) a link.
LEFT = {
    "same": (b"abc", T0),
    "diffbytes": (b"abc", T0),                 # right: other bytes, same size and date
    "newer_l": (b"abcd", T2),                  # right: same size, older
    "older_l_sized": (b"ab", T0),              # right: larger and newer
    "sizediff_eq_time": (b"ab", T0),           # right: larger, same date
    "left_only": (b"q", T0),
    "clash": (b"ab", T0),                      # right: a directory of this name
    "empty": (b"", T0),
    "newer_empty": (b"", T2),                  # right: a non-empty file, older
    "dir_both": None,
    "dir_left": None,
    "lnk": ("link", "same"),
}
RIGHT_SIDE = {
    "same": (b"abc", T0),
    "diffbytes": (b"xyz", T0),
    "newer_l": (b"abcd", T1),
    "older_l_sized": (b"abcdef", T1),
    "sizediff_eq_time": (b"abc", T0),
    "right_only": (b"q", T0),
    "clash": None,
    "empty": (b"", T0),
    "newer_empty": (b"zz", T0),
    "dir_both": None,
}

# The marks each method gives the panels of the fixture, as mc marks them.
EXPECT = {
    QUICK: ({"left_only", "lnk", "newer_empty", "newer_l", "sizediff_eq_time"},
            {"older_l_sized", "right_only", "sizediff_eq_time"}),
    SIZE: ({"clash", "left_only", "lnk", "newer_empty", "older_l_sized", "sizediff_eq_time"},
           {"newer_empty", "older_l_sized", "right_only", "sizediff_eq_time"}),
    THOROUGH: ({"diffbytes", "left_only", "lnk", "newer_empty", "sizediff_eq_time"},
               {"diffbytes", "older_l_sized", "right_only", "sizediff_eq_time"}),
}

FRAME = "\u2500\u2502\u250c\u2510\u2514\u2518\u251c\u2524"   # the frame, as the reference draws it
DEC_FRAME = "qxlkmjtu"                                       # the same, as this build draws it


def build(d, spec):
    os.makedirs(d, exist_ok=True)
    for name, v in spec.items():
        path = os.path.join(d, name)
        if v is None:
            os.mkdir(path)
        elif isinstance(v, tuple) and v[0] == "link":
            os.symlink(v[1], path)
        elif isinstance(v, tuple) and v[0] == "sparse":       # a file of v[1] bytes, no data
            with open(path, "wb") as f:
                f.truncate(v[1])
            os.utime(path, (v[2], v[2]))
        else:
            data, mtime = v
            with open(path, "wb") as f:
                f.write(data)
            os.utime(path, (mtime, mtime))
    return d


def wait_titles(t, left, right, limit=15.0):
    """Pump the output until the left panel's title names LEFT and the right's names
    RIGHT. A key sent before mc has drawn, or before it has taken a directory, can go
    to the wrong panel; this makes sure the setup has happened before any key."""
    end = time.time() + limit
    top = ""
    while time.time() < end:
        t.pump(.2)
        top = t.line(0)
        if os.path.basename(left) in top[:50] and os.path.basename(right) in top[50:]:
            return
    raise AssertionError("panels not at %s and %s: %r" % (left, right, top))


def arrange(t, left, right):
    """Start on LEFT on both panels, put RIGHT on the right one, and leave the left
    panel active."""
    wait_titles(t, left, left)
    t.send(TAB, .4)
    t.send(("cd " + right + "\r").encode(), 1.5)
    wait_titles(t, left, right)
    t.send(TAB, .4)


def two_panels(tc, name="", start=None):
    """The fixture in two directories under the test's root: LEFT on the left panel,
    RIGHT on the right, the left active. START starts the program (the test's own,
    or the installed mc's, see MCTestCase)."""
    L = build(tc.new_dir(name + "left"), LEFT)
    R = build(tc.new_dir(name + "right"), RIGHT_SIDE)
    t = (start or tc.start)(L)
    arrange(t, L, R)
    return t


def marked(t, x0):
    """The names of the marked entries of the panel at column X0. A marked entry is
    drawn brown and bold; the directories and the plain files are not."""
    out = set()
    for y in range(2, 20):
        name = t.line(y)[x0 + 1:x0 + 28].rstrip()
        if not name:
            break
        cell = t.screen.buffer[y][x0 + 2]
        if cell.fg == "brown" and cell.bold:
            out.add(name[1:])
    return out


def question_cells(t):
    """The question's box, cell by cell: the character (the frame's, which differ in
    their encoding, left out), its colours and boldness."""
    out = {}
    for y in range(6, 13):
        for x in range(20, 80):
            c = t.screen.buffer[y][x]
            if c.data in FRAME or c.data in DEC_FRAME:
                continue
            out[(y, x)] = (c.data, c.fg, c.bg, bool(c.bold))
    return out


def open_compare(t):
    """F9, then Command by its letter, then its entry by its letter: the question."""
    t.send(F9, .6)
    t.send(b"c", .6)
    t.send(b"c", .8)


def question_open(t):
    return any("Select compare method:" in t.line(y) for y in range(24))


def run_method(t, key):
    open_compare(t)
    t.send(key, 1.5)
    return marked(t, 0), marked(t, 50)


class CompareTests(MCTestCase):

    def test_command_title_and_entry_are_in_the_menu(self):
        t = two_panels(self)
        t.send(F9, .6)
        t.send(b"c", .6)                   # the title's letter: its dropdown opens
        self.assertEqual(bar_selected(t), "Command")
        self.assertTrue(any("Compare directories" in t.line(y) for y in range(1, 6)))
        self.assertFalse(question_open(t))

    def test_the_menu_is_reached_by_its_arrows_too(self):
        t = two_panels(self)
        t.send(F9, .6)
        t.send(RIGHT, .5)                  # File
        t.send(RIGHT, .5)                  # Command
        self.assertEqual(bar_selected(t), "Command")
        t.send(DOWN, .6)                   # opens the dropdown
        t.send(ENTER, .8)                  # Compare directories
        self.assertTrue(question_open(t))

    def test_the_question_offers_the_three_methods_and_cancel(self):
        t = two_panels(self)
        open_compare(t)
        self.assertTrue(question_open(t))
        rows = [t.line(y) for y in range(24) if "[ Quick ]" in t.line(y)]
        self.assertEqual(len(rows), 1)
        for label in ("[ Quick ]", "[ Size only ]", "[ Thorough ]", "[ Cancel ]"):
            self.assertIn(label, rows[0])

    def test_escape_changes_no_marks(self):
        t = two_panels(self)
        t.goto(t.find("same"))
        t.send(INSERT, .3)                 # marked by hand
        open_compare(t)
        t.send(ESC, 1.5)
        self.assertFalse(question_open(t))
        self.assertEqual(marked(t, 0), {"same"})
        self.assertEqual(marked(t, 50), set())

    def test_quick_marks_what_differs_in_each_panel(self):
        t = two_panels(self)
        self.assertEqual(run_method(t, QUICK), EXPECT[QUICK])
        self.assertFalse(question_open(t))

    def test_size_only_ignores_the_dates(self):
        t = two_panels(self)
        self.assertEqual(run_method(t, SIZE), EXPECT[SIZE])

    def test_thorough_marks_the_same_size_files_whose_bytes_differ(self):
        t = two_panels(self)
        self.assertEqual(run_method(t, THOROUGH), EXPECT[THOROUGH])

    def test_quick_does_not_look_at_the_bytes(self):
        t = two_panels(self)
        left, _ = run_method(t, QUICK)
        self.assertNotIn("diffbytes", left)

    def test_the_marks_a_panel_had_are_replaced(self):
        t = two_panels(self)
        t.goto(t.find("same"))
        t.send(INSERT, .3)                 # marked by hand, though the same on both sides
        self.assertIn("same", marked(t, 0))
        open_compare(t)
        t.send(QUICK, 1.5)
        self.assertNotIn("same", marked(t, 0))

    def test_directories_are_never_marked(self):
        t = two_panels(self)
        for method in (QUICK, SIZE, THOROUGH):
            with self.subTest(method=method):
                left, right = run_method(t, method)
                self.assertNotIn("dir_left", left)
                self.assertNotIn("dir_both", left | right)

    def test_thorough_is_refused_in_an_archive(self):
        d = self.new_dir("work")
        other = build(os.path.join(d, "other"), {"x": (b"x", T0)})
        fixtures.zip_archive(os.path.join(d, "a.zip"), {"x": b"x"})
        t = self.start(d)
        arrange(t, d, other)
        t.goto(t.find("a.zip"))
        t.send(ENTER, 2.0)
        self.assertTrue(oracle.in_archive(t))
        open_compare(t)
        t.send(THOROUGH, 1.5)
        self.assertTrue(any("Thorough compare is not done" in t.line(y) for y in range(24)))
        t.send(b" ", 1.0)                  # any key closes the error box
        self.assertFalse(any("Thorough compare is not done" in t.line(y) for y in range(24)))

    def test_quick_works_in_an_archive(self):
        d = self.new_dir("work")
        other = build(os.path.join(d, "other"), {"x": (b"x", T0)})
        fixtures.zip_archive(os.path.join(d, "a.zip"), {"x": b"x", "only_in_zip": b"z"})
        t = self.start(d)
        arrange(t, d, other)
        t.goto(t.find("a.zip"))
        t.send(ENTER, 2.0)
        self.assertTrue(oracle.in_archive(t))
        left, right = run_method(t, QUICK)
        self.assertIn("only_in_zip", left)
        self.assertEqual(right, set())

    def test_huge_sizes_that_round_alike_are_told_apart(self):
        """From a GiB up the listing keeps a size to the KiB, so 1 GiB + 1 and 1 GiB + 2
        bytes look alike there, and are not the same file: they are marked. Equal sizes
        are not."""
        GIB = 1 << 30
        spec_l = {"near": ("sparse", GIB + 1, T0), "same": ("sparse", GIB + 5, T0)}
        spec_r = {"near": ("sparse", GIB + 2, T0), "same": ("sparse", GIB + 5, T0)}
        for method in (SIZE, QUICK):
            with self.subTest(method=method):
                L = build(self.new_dir("hl-" + method.decode()), spec_l)
                R = build(self.new_dir("hr-" + method.decode()), spec_r)
                t = self.start(L)
                arrange(t, L, R)
                left, right = run_method(t, method)
                self.assertEqual(left, {"near"})
                self.assertEqual(right, {"near"})

    @unittest.skipUnless(os.path.exists(term.REFERENCE), "the installed mc is not there")
    def test_huge_sizes_match_reference(self):
        GIB = 1 << 30
        spec = {"near": ("sparse", GIB + 1, T0), "same": ("sparse", GIB + 5, T0)}
        spec2 = {"near": ("sparse", GIB + 2, T0), "same": ("sparse", GIB + 5, T0)}
        L, R = build(self.new_dir("ql"), spec), build(self.new_dir("qr"), spec2)
        ref = self.start_reference(L)
        arrange(ref, L, R)
        ours = self.start(build(self.new_dir("ol"), spec))
        arrange(ours, self.new_dir("ol"), build(self.new_dir("or"), spec2))
        ref.send(b"\x18d", 1.0)
        ref.send(SIZE, 1.5)
        ours_marks = run_method(ours, SIZE)
        self.assertEqual(marked(ref, 0), ours_marks[0])
        self.assertEqual(marked(ref, 50), ours_marks[1])

    @unittest.skipUnless(os.path.exists(term.REFERENCE), "the installed mc is not there")
    def test_reference_matches(self):
        """The same fixture and method on this build and the installed mc: the question
        drawn cell by cell, then the marks on both panels."""
        for key in (QUICK, SIZE, THOROUGH):
            with self.subTest(method=key):
                ours = two_panels(self, "o" + key.decode() + "-")
                ref = two_panels(self, "r" + key.decode() + "-", start=self.start_reference)
                open_compare(ours)
                ref.send(CTRL_X_D, 1.0)
                self.assertEqual(question_cells(ours), question_cells(ref), "the question")
                ours.send(key, 1.5)
                ref.send(key, 1.5)
                self.assertEqual(marked(ours, 0), marked(ref, 0), "left panel")
                self.assertEqual(marked(ours, 50), marked(ref, 50), "right panel")
                self.assertEqual(marked(ours, 0), EXPECT[key][0])
                self.assertEqual(marked(ours, 50), EXPECT[key][1])


if __name__ == "__main__":
    unittest.main()
