"""Zip archives, entered as directories."""
import os
import time

import fixtures
import oracle
from case import MCTestCase
from term import CTRL_R, DOWN, END, ENTER, F3, F8, UP


class ZipTests(MCTestCase):

    def archive(self, name, files, dirs=(), dir_entries=True, d=None):
        d = d or self.new_dir()
        path = os.path.join(d, name)
        fixtures.zip_archive(path, files, dirs=dirs, dir_entries=dir_entries)
        return d, path

    def check_walk(self, name, files, **kw):
        d, path = self.archive(name, files, **kw)
        t = self.start(d)
        oracle.enter_archive(self, t, name)
        oracle.walk(self, t, name, oracle.members_of_zip(path))

    def test_stored_directories_are_listed(self):
        self.check_walk("notes.zip", fixtures.ZIP_NOTES, dirs=["src"])

    def test_implicit_directories_are_listed(self):
        self.check_walk("implicit.zip", fixtures.ZIP_IMPLICIT, dir_entries=False)

    def test_nested_directories(self):
        self.check_walk("deep.zip", fixtures.ZIP_DEEP, dir_entries=False)

    def test_names_with_dot_slash_are_listed_without_it(self):
        self.check_walk("dotted.zip", fixtures.ZIP_DOTTED, dir_entries=False)

    def test_a_long_listing_scrolls(self):
        d = self.new_dir()
        fixtures.zip_big(os.path.join(d, "big.zip"))
        t = self.start(d)
        oracle.enter_archive(self, t, "big.zip")
        t.send(END, .5)
        self.assertEqual(t.cursor_name().strip(), "item119.dat")

    def test_ctrl_r_reads_a_changed_archive(self):
        d, path = self.archive("a.zip", {"notes.txt": b"n\n"})
        t = self.start(d)
        oracle.enter_archive(self, t, "a.zip")
        time.sleep(1.1)                      # a different modification time
        fixtures.zip_archive(path, {"notes.txt": b"n\n", "newfile.txt": b"new\n"})
        t.send(CTRL_R, .8)
        self.assertIsNotNone(t.find("newfile.txt"))

    def test_leaving_puts_the_cursor_on_the_archive(self):
        d, _ = self.archive("a.zip", {"notes.txt": b"n\n"}, dirs=[])
        t = self.start(d)
        oracle.enter_archive(self, t, "a.zip")
        t.send(UP, .2)
        t.send(ENTER, 1.0)
        self.assertEqual(t.title(), d)
        self.assertEqual(t.cursor_name().strip(), "a.zip")

    def test_a_damaged_archive_is_an_error_and_the_panel_stays(self):
        d = self.new_dir()
        fixtures.put(os.path.join(d, "fake.zip"), b"this is not a zip\n")
        t = self.start(d)
        t.goto(t.find("fake.zip"))
        t.send(ENTER, 1.0)
        self.assertTrue(any("Cannot open archive" in t.line(y) for y in range(24)))
        self.assertEqual(t.title(), d)

    def test_f3_shows_the_archived_file_and_f8_is_refused(self):
        d = self.new_dir()
        fixtures.put(os.path.join(d, "notes.txt"), b"HOST copy\n")
        fixtures.zip_archive(os.path.join(d, "box.zip"), {"notes.txt": b"ARCHIVED\n"})
        t = self.start(d)
        oracle.enter_archive(self, t, "box.zip")
        t.goto(t.find("notes.txt"))
        t.send(F3, 2.0)
        self.assertTrue(any("ARCHIVED" in t.line(y) for y in range(24)))
        self.assertFalse(any("HOST copy" in t.line(y) for y in range(24)))
        t.send(b"q", 1.0)
        t.goto(t.find("notes.txt"))
        t.send(F8, .8)
        self.assertTrue(any("Read-only archive" in t.line(y) for y in range(24)))
        t.send(b" ", .5)
        self.assertTrue(oracle.in_archive(t))
        with open(os.path.join(d, "notes.txt"), "rb") as f:
            self.assertEqual(f.read(), b"HOST copy\n")

    def test_a_fifo_named_zip_does_not_hang_mc(self):
        d = self.new_dir()
        os.mkfifo(os.path.join(d, "fifo.zip"))
        t = self.start(d)
        t.goto(t.find("fifo.zip"))
        t.send(ENTER, 1.0)
        self.assertTrue(t.alive())
        self.assertEqual(t.title(), d)


if __name__ == "__main__":
    import unittest
    unittest.main()
