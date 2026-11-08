"""The menu bar (F9, or ESC 9), its Left and Right menus, and the Sort order dialog
that the Left and Right menus open, with the sort it sets on a panel."""
import os
import re
import tempfile
import unittest

import term
from case import MCTestCase
from term import DOWN, ENTER, ESC, LEFT, RIGHT, TAB

F9 = b"\x1b[20~"
BACK_TAB = b"\x1b[Z"
SIZES = {"b.txt": 3, "A.txt": 1, "a10": 2, "a2": 5, "Zeta": 0, "run": 4}
DIRS = ["Dir_b", "dir_a"]
DIALOG_TITLE = re.compile(r"[q\u2500] Sort order [q\u2500]")


def version_sort_built_in():
    """Whether mc is built with the Version choice: VERSION_SORT in config.h."""
    with open(os.path.join(os.path.dirname(__file__), "..", "config.h")) as f:
        return re.search(r"#define VERSION_SORT\s+(\d)", f.read()).group(1) == "1"


VERSION_SORT = version_sort_built_in()


def fresh_dir(tc):
    """A directory of its own in the test's root, so a test can make several."""
    return tempfile.mkdtemp(dir=tc.root)


def write_fixture(d):
    """Files of distinct sizes, one executable, two directories."""
    for name, size in SIZES.items():
        with open(os.path.join(d, name), "wb") as f:
            f.write(b"x" * size)
    for name in DIRS:
        os.mkdir(os.path.join(d, name))
    os.chmod(os.path.join(d, "run"), 0o755)
    return d


def bar_selected(t):
    """The title the menu is on: the black cells of row 0."""
    s = "".join(t.screen.buffer[0][x].data for x in range(60)
                if t.screen.buffer[0][x].bg == "black")
    return s.strip()


def dropdown_open(t):
    return any("Sort order" in t.line(y) for y in range(1, 20))


def dialog_open(t):
    return any(DIALOG_TITLE.search(t.line(y)) for y in range(2, 6))


def dialog(t):
    """The dialog's choices: the labels of the fields, the field chosen, the three
    checkboxes, and what has the focus. Rows are found by their labels, as the frame
    is one row shorter without the Version choice."""
    labels, chosen, boxes = [], [], []
    for y in range(2, 18):
        line = t.line(y)
        if line[29:33] in ("( ) ", "(*) "):
            labels.append(line[33:44].strip())
            if line[29:33] == "(*) ":
                chosen.append(line[29:44].strip())
        for box in ("Executable first", "Case sensitive", "Reverse"):
            if box in line:
                boxes.append(line[50:70].strip())
    focus = []
    top = next(y for y in range(2, 6) if DIALOG_TITLE.search(t.line(y)))
    for y in range(top + 1, top + 12):          # the rows inside the frame
        s = "".join(t.screen.buffer[y][x].data for x in range(26, 75)
                    if t.screen.buffer[y][x].bg == "cyan")
        if s.strip():
            focus.append(s.strip())
    return {"labels": labels, "chosen": chosen, "boxes": boxes, "focus": focus}


def names(t):
    """The left panel's entries, without \"..\" and without the mark each one shows."""
    return [n[1:] for n, _ in t.entries() if n[1:] != ".."]


def cursor(t):
    return t.cursor_name()[1:]


class MenuTests(MCTestCase):

    def fixture(self):
        return write_fixture(fresh_dir(self))

    def test_f9_opens_the_bar_on_the_side_of_the_active_panel(self):
        t = self.start(self.fixture())
        t.send(F9, .6)
        self.assertEqual(bar_selected(t), "Left")
        self.assertFalse(dropdown_open(t))
        t.send(ESC, 2.0)
        t.send(TAB, .4)
        t.send(F9, .6)
        self.assertEqual(bar_selected(t), "Right")

    def test_esc_nine_is_the_same_as_f9(self):
        t = self.start(self.fixture())
        t.send(b"\x1b9", 1.0)
        self.assertEqual(bar_selected(t), "Left")

    def test_left_and_right_move_across_the_titles_and_wrap(self):
        t = self.start(self.fixture())
        t.send(F9, .6)
        t.send(LEFT, .5)
        self.assertEqual(bar_selected(t), "Right")
        t.send(RIGHT, .5)
        self.assertEqual(bar_selected(t), "Left")
        for _ in range(3):
            t.send(RIGHT, .5)
        self.assertEqual(bar_selected(t), "Options")

    def test_down_and_enter_open_the_dropdown(self):
        t = self.start(self.fixture())
        t.send(F9, .6)
        t.send(DOWN, .6)
        self.assertTrue(dropdown_open(t))
        t.send(ESC, 2.0)
        self.assertFalse(dropdown_open(t))
        t.send(F9, .6)
        t.send(ENTER, .6)
        self.assertTrue(dropdown_open(t))

    def test_escape_takes_the_menu_away(self):
        t = self.start(self.fixture())
        t.send(F9, .6)
        t.send(ENTER, .6)
        t.send(ESC, 2.0)
        self.assertFalse(dropdown_open(t))
        self.assertEqual(bar_selected(t), "")

    def test_empty_titles_have_no_dropdown(self):
        t = self.start(self.fixture())
        t.send(F9, .6)
        t.send(RIGHT, .5)                 # File: empty for now
        t.send(ENTER, .6)
        self.assertEqual(bar_selected(t), "File")
        self.assertFalse(dropdown_open(t))

    def test_sort_order_opens_the_dialog_for_the_left_panel(self):
        t = self.start(self.fixture())
        t.send(F9, .6)
        t.send(ENTER, .6)
        t.send(b"s", .8)
        self.assertTrue(dialog_open(t))
        self.assertEqual(dialog(t)["chosen"], ["(*) Name"])

    def test_right_menu_sorts_the_right_panel(self):
        t = self.start(self.fixture())
        t.send(TAB, .4)
        t.send(F9, .6)
        t.send(ENTER, .6)
        t.send(b"s", .8)
        t.send(b"r", .3)                  # Reverse
        t.send(b"o", 1.2)
        right = [t.line(y)[51:78].strip().lstrip("/*@") for y in range(2, 20)]
        right = [r.split()[0] for r in right if r]
        self.assertEqual(right[:3], ["..", "dir_a", "Dir_b"])


class SortOrderTests(MCTestCase):
    """The order the dialog's sort gives the panel. The expected orders follow upstream's
    rules, and test_reference_orders_match checks each field against the installed mc."""

    def sorted_by(self, keys, reference=False):
        d = write_fixture(fresh_dir(self))
        t = self.start_reference(d) if reference else self.start(d)
        t.send(F9, .6)
        t.send(ENTER, .6)
        t.send(b"s", .8)
        for k in keys:
            t.send(k.encode(), .3)
        t.send(b"o", 1.2)
        return names(t)

    def test_the_version_choice_is_built_in_only_when_asked(self):
        t = self.start(write_fixture(fresh_dir(self)))
        t.send(F9, .6)
        t.send(ENTER, .6)
        t.send(b"s", .8)
        labels = dialog(t)["labels"]
        self.assertEqual("Version" in labels, VERSION_SORT)
        self.assertEqual(labels[:2], ["Unsorted", "Name"])
        self.assertEqual(labels[-1], "Inode")

    def test_by_name_with_directories_first(self):
        self.assertEqual(self.sorted_by([]),
                         ["Dir_b", "dir_a", "A.txt", "Zeta", "a10", "a2", "b.txt", "run"])

    def test_the_options_of_the_dialog(self):
        cases = [
            (["r"], ["dir_a", "Dir_b", "run", "b.txt", "a2", "a10", "Zeta", "A.txt"]),
            (["e"], ["dir_a", "Dir_b", "A.txt", "a10", "a2", "b.txt", "run", "Zeta"]),
            (["f"], ["Dir_b", "dir_a", "run", "A.txt", "Zeta", "a10", "a2", "b.txt"]),
            (["s"], ["Dir_b", "dir_a", "Zeta", "A.txt", "a10", "b.txt", "run", "a2"]),
            (["x"], ["Dir_b", "dir_a", "Zeta", "a10", "a2", "run", "A.txt", "b.txt"]),
            (["v"], ["Dir_b", "dir_a", "A.txt", "Zeta", "a2", "a10", "b.txt", "run"]
             if VERSION_SORT else ["Dir_b", "dir_a", "A.txt", "Zeta", "a10", "a2", "b.txt", "run"]),
        ]
        for keys, want in cases:
            with self.subTest(keys=keys):
                self.assertEqual(self.sorted_by(keys), want)

    @unittest.skipUnless(os.path.exists(term.REFERENCE), "the installed mc is not there")
    def test_reference_orders_match(self):
        """Each field, plain, reversed, and with the flags, as the installed mc orders it.
        The reference has the Version field: it is compared only when mc is built with it."""
        for letter in ("unvxsmahi" if VERSION_SORT else "unxsmahi"):
            for keys in ([letter], [letter, "r"], [letter, "e", "f"]):
                with self.subTest(keys=keys):
                    self.assertEqual(self.sorted_by(keys), self.sorted_by(keys, reference=True))


class DialogTests(MCTestCase):

    def open(self, t):
        t.send(F9, .6)
        t.send(ENTER, .6)
        t.send(b"s", .8)
        self.assertTrue(dialog_open(t))

    def test_focus_moves_in_tab_order(self):
        t = self.start(write_fixture(fresh_dir(self)))
        self.open(t)
        self.assertEqual(dialog(t)["focus"], ["(*) Name"])
        t.send(TAB, .3)
        self.assertEqual(dialog(t)["focus"], ["[ ] Executable first"])
        for _ in range(3):
            t.send(TAB, .3)
        self.assertEqual(dialog(t)["focus"], ["[< OK >]"])
        t.send(TAB, .3)
        self.assertEqual(dialog(t)["focus"], ["[ Cancel ]"])
        t.send(BACK_TAB, .3)
        t.send(BACK_TAB, .3)
        self.assertEqual(dialog(t)["focus"], ["[ ] Reverse"])

    def test_space_chooses_a_field_and_toggles_a_box(self):
        t = self.start(write_fixture(fresh_dir(self)))
        self.open(t)
        t.send(DOWN, .3)                  # the third choice has the focus, not yet chosen
        third = dialog(t)["labels"][2]
        self.assertEqual(dialog(t)["chosen"], ["(*) Name"])
        t.send(b" ", .3)
        self.assertEqual(dialog(t)["chosen"], ["(*) " + third])
        t.send(TAB, .3)
        t.send(b" ", .3)
        self.assertEqual(dialog(t)["boxes"][0], "[x] Executable first")

    def test_letters_pick_and_toggle(self):
        t = self.start(write_fixture(fresh_dir(self)))
        self.open(t)
        t.send(b"x", .3)
        self.assertEqual(dialog(t)["chosen"], ["(*) Extension"])
        t.send(b"r", .3)
        self.assertEqual(dialog(t)["boxes"][2], "[x] Reverse")
        t.send(b"R", .3)                  # capitals do nothing
        self.assertEqual(dialog(t)["boxes"][2], "[x] Reverse")

    def test_cancel_changes_nothing(self):
        t = self.start(write_fixture(fresh_dir(self)))
        self.open(t)
        t.send(b"s", .3)
        t.send(ESC, 2.0)
        self.assertFalse(dialog_open(t))
        self.open(t)
        self.assertEqual(dialog(t)["chosen"], ["(*) Name"])

    def test_ok_keeps_the_highlight_on_its_file(self):
        d = fresh_dir(self)
        for i, name in enumerate("abcd"):
            with open(os.path.join(d, name), "wb") as f:
                f.write(b"x" * (4 - i))
        t = self.start(d)
        for _ in range(3):
            t.send(DOWN, .2)              # from "..": a, b, c
        self.assertEqual(cursor(t), "c")
        t.send(F9, .6)
        t.send(ENTER, .6)
        t.send(b"s", .8)
        t.send(b"s", .3)                  # Size: d, c, b, a
        t.send(b"o", 1.2)
        self.assertEqual(names(t), ["d", "c", "b", "a"])
        self.assertEqual(cursor(t), "c")

    @unittest.skipUnless(os.path.exists(term.REFERENCE) and VERSION_SORT,
                         "needs the installed mc, and mc built with VERSION_SORT")
    def test_matches_reference(self):
        """The dialog cell by cell, colours and boldness included, as the installed mc
        draws it. The frame's own characters are left out: they are encoded differently."""
        d = write_fixture(fresh_dir(self))
        ours, ref = self.start(d), self.start_reference(d)
        for t in (ours, ref):
            t.send(F9, .6)
            t.send(ENTER, .6)
            t.send(b"s", 1.0)                 # Sort order..., by its letter
        for y in range(3, 16):
            for x in range(27, 72):
                a, b = ref.screen.buffer[y][x], ours.screen.buffer[y][x]
                if a.data in "─│┌┐└┘├┤" or b.data in "qxlkmjtu":
                    continue
                self.assertEqual((a.fg, a.bg, bool(a.bold)), (b.fg, b.bg, bool(b.bold)), (y, x))


if __name__ == "__main__":
    unittest.main()
