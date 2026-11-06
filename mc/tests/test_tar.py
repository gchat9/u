"""Tar archives, plain and compressed, entered as directories."""
import io
import os
import random
import tarfile
import unittest

import fixtures
import oracle
from case import MCTestCase
from term import ENTER, F10


def compressor_available(form):
    _, how = fixtures.TAR_FORMS[form]
    return how != "zst" or fixtures.have("zstd")


class TarTests(MCTestCase):

    def check_walk(self, form, members, fmt):
        d = self.new_dir()
        path = fixtures.tar_archive(d, "archive", members, form, fmt)
        t = self.start(d)
        name = os.path.basename(path)
        oracle.enter_archive(self, t, name)
        oracle.walk(self, t, name, oracle.members_of_tar(path))
        return t

    def test_basic_archive_in_every_form(self):
        for form in fixtures.TAR_FORMS:
            with self.subTest(form=form):
                if not compressor_available(form):
                    self.skipTest("zstd is not installed")
                self.check_walk(form, fixtures.BASIC, "gnu")

    def test_adversarial_archive_in_every_form(self):
        # the names, links and sizes that a tar reader has to get right, in both
        # header formats
        for i, form in enumerate(fixtures.TAR_FORMS):
            with self.subTest(form=form):
                if not compressor_available(form):
                    self.skipTest("zstd is not installed")
                self.check_walk(form, fixtures.ADVERSARIAL, "pax" if i % 2 else "gnu")

    def test_link_targets_are_in_the_info_line(self):
        d = self.new_dir()
        path = fixtures.tar_archive(d, "adv", fixtures.ADVERSARIAL, "tgz", "gnu")
        t = self.start(d)
        oracle.enter_archive(self, t, os.path.basename(path))
        t.goto(t.find("lnk"))
        self.assertIn("-> " + fixtures.LONG_TARGET[:12], t.info_line())
        t.goto(t.find("adir"))
        t.send(ENTER, 1.0)                  # into adir
        t.goto(t.find("rel-link"))
        self.assertIn("-> inner.txt", t.info_line())

    def test_zstd_suffixes_are_coloured_as_archives(self):
        d = self.new_dir()
        for name in ("t.tar.zst", "t.tar.zstd"):
            fixtures.put(os.path.join(d, name))
        t = self.start(d)
        for name in ("t.tar.zst", "t.tar.zstd"):
            y = 2 + t.find(name)
            cell = t.screen.buffer[y][1 + t.line(y)[1:].index(name[0])]
            self.assertEqual((cell.fg, cell.bold), ("magenta", True), name)


class DamagedTarTests(MCTestCase):

    def enter(self, name, data):
        d = self.new_dir()
        fixtures.put(os.path.join(d, name), data)
        t = self.start(d)
        t.goto(t.find(name))
        t.send(ENTER, 1.5)
        return d, t

    def error_shown(self, t):
        return any("Cannot open archive" in t.line(y) for y in range(24))

    def test_a_truncated_tar_lists_what_it_has(self):
        full = fixtures.tar_bytes(fixtures.ADVERSARIAL, "pax")
        d, t = self.enter("cut.tar", full[:3000])
        self.assertTrue(oracle.in_archive(t))
        names = [n for n, _ in t.entries()]
        self.assertGreater(len(names), 1)
        self.assertTrue(t.alive())

    def test_a_damaged_header_stops_the_listing_there(self):
        b = bytearray(fixtures.tar_bytes(fixtures.ADVERSARIAL, "gnu"))
        headers = [i for i in range(0, len(b) - 512, 512) if b[i + 257:i + 262] == b"ustar"]
        b[headers[2] + 148] ^= 0x55         # the checksum of the third header
        d, t = self.enter("bad.tar", bytes(b))
        self.assertTrue(oracle.in_archive(t))
        self.assertTrue(t.alive())

    def test_garbage_is_an_error(self):
        d, t = self.enter("garbage.tar", bytes(random.Random(1).getrandbits(8) for _ in range(3000)))
        self.assertTrue(self.error_shown(t))
        self.assertEqual(t.title(), d)

    def test_an_empty_tar_is_an_empty_directory(self):
        d, t = self.enter("empty.tar", b"")
        self.assertTrue(oracle.in_archive(t))
        self.assertEqual([n for n, _ in t.entries()], ["/.."])

    def test_an_empty_gzip_is_an_error(self):
        d, t = self.enter("zero.tgz", b"")
        self.assertTrue(self.error_shown(t))

    def test_a_truncated_gzip_is_an_error(self):
        whole = fixtures.compress(fixtures.tar_bytes(fixtures.ADVERSARIAL, "gnu"), "gz")
        d, t = self.enter("cut.tgz", whole[:200])
        self.assertTrue(self.error_shown(t))

    def test_a_fifo_named_tar_does_not_hang_mc(self):
        d = self.new_dir()
        os.mkfifo(os.path.join(d, "fifo.tar"))
        t = self.start(d)
        t.goto(t.find("fifo.tar"))
        t.send(ENTER, 1.0)
        self.assertTrue(t.alive())
        self.assertEqual(t.title(), d)

    def test_mutated_headers_never_crash_mc(self):
        base = bytearray(fixtures.tar_bytes(fixtures.ADVERSARIAL, "pax"))
        rnd = random.Random(7)
        for k in range(8):
            with self.subTest(mutant=k):
                b = bytearray(base)
                for _ in range(rnd.randint(1, 12)):
                    b[rnd.randrange(0, min(len(b), 6000))] = rnd.randrange(256)
                if rnd.random() < 0.2:
                    b = b[:rnd.randrange(512, len(b))]
                d, t = self.enter("m.tar", bytes(b))
                self.assertTrue(t.alive(), "mc exited on mutant %d" % k)


if __name__ == "__main__":
    unittest.main()
