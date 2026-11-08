"""Group selection: + and - select or unselect the entries that match a shell pattern,
and * inverts the selection of the files. The counts are read off the Delete question,
which names the selected items, so the tests need no look at the marks themselves."""
import os
import re
import unittest

import term
from case import MCTestCase
from term import DOWN, ENTER

PLUS, MINUS, STAR, ESC = b"+", b"-", b"*", b"\x1b"
KEY = 0.1       # mc answers in a few milliseconds: this is the quiet time after a key
ESC_WAIT = 0.6  # a lone Esc is held a moment, to tell it from an escape sequence
F8 = b"\x1b[19~"
FILES = ["a.txt", "b.txt", "A.TXT", "c.log", "readme", "x y.txt"]
DIRS = ["sub", ".dot"]


def fixture(tc):
    d = tc.new_dir()
    for name in FILES:
        open(os.path.join(d, name), "w").close()
    for name in DIRS:
        os.mkdir(os.path.join(d, name))
    return d


def selected_by_delete(t):
    """What the Delete question says is selected: its count, or the one item it names."""
    t.send(F8, KEY)
    text = " ".join(t.line(y)[20:80].strip() for y in range(5, 14))
    t.send(ESC, ESC_WAIT)
    m = re.search(r"Delete (\d+) files/directories\?|Delete (\d+) files\?", text)
    if m:
        return int(m.group(1) or m.group(2))
    return text


class SelectTests(MCTestCase):

    def test_plus_with_nothing_offered_selects_nothing(self):
        t = self.start(fixture(self))
        t.send(PLUS, KEY)
        t.send(ENTER, KEY)
        self.assertNotIsInstance(selected_by_delete(t), int)

    def test_plus_selects_the_matching_files(self):
        t = self.start(fixture(self))
        t.send(PLUS, KEY)
        t.send(b"*.txt", KEY)
        t.send(ENTER, KEY)
        self.assertEqual(selected_by_delete(t), 3)      # a.txt, b.txt, x y.txt: not A.TXT

    def test_plus_with_star_takes_the_directories_too_but_not_dotdot(self):
        t = self.start(fixture(self))
        t.send(PLUS, KEY)
        t.send(b"*", KEY)
        t.send(ENTER, KEY)
        self.assertEqual(selected_by_delete(t), len(FILES) + len(DIRS))

    def test_minus_unselects_the_matching(self):
        t = self.start(fixture(self))
        t.send(PLUS, KEY)
        t.send(b"*", KEY)
        t.send(ENTER, KEY)
        t.send(MINUS, KEY)
        t.send(b"a*", KEY)
        t.send(ENTER, KEY)
        self.assertEqual(selected_by_delete(t), len(FILES) + len(DIRS) - 1)   # a.txt only

    def test_star_inverts_the_files_and_leaves_the_directories(self):
        t = self.start(fixture(self))
        t.send(PLUS, KEY)
        t.send(b"*.txt", KEY)
        t.send(ENTER, KEY)
        t.send(STAR, KEY)
        self.assertEqual(selected_by_delete(t), 3)      # A.TXT, c.log, readme

    def test_escape_changes_nothing(self):
        t = self.start(fixture(self))
        t.send(PLUS, KEY)
        t.send(b"*", KEY)
        t.send(ESC, ESC_WAIT)
        t.send(DOWN, KEY)
        t.send(F8, KEY)                           # the file under the cursor alone
        self.assertRegex(" ".join(t.lines()), r"Delete (file|directory) ")   # one item, not a count
        t.send(ESC, ESC_WAIT)

    @unittest.skipUnless(os.path.exists(term.REFERENCE), "the installed mc is not there")
    def test_matches_reference(self):
        d = fixture(self)
        for keys in ([(PLUS, b"*.txt")], [(PLUS, b"*"), (MINUS, b"a*")], [(PLUS, b"[ab]*.t?t")]):
            results = []
            for start in (self.start_reference, self.start):
                t = start(d)
                for key, pattern in keys:
                    t.send(key, KEY)
                    t.send(pattern, KEY)
                    t.send(ENTER, KEY)
                results.append(selected_by_delete(t))
                t.close()
            self.assertEqual(results[0], results[1], keys)


if __name__ == "__main__":
    unittest.main()
