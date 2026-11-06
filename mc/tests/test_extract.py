"""Files of an archive, used from its panel: F3 views one, F5 copies some out to a
plain directory. F6, F8, and a copy into an archive are refused: an archive is
read-only."""
import hashlib
import io
import os
import random
import re
import shutil
import subprocess
import tarfile
import unittest
import zipfile

import fixtures
import oracle
from case import MCTestCase
from term import ENTER, F3, F5, F6, F8, INSERT, TAB, BS

STAGED = re.compile(r"mc-\d+-\d+$")


def sha(path):
    with open(path, "rb") as f:
        return hashlib.sha256(f.read()).hexdigest()


def staged_left(tmp):
    return [n for n in os.listdir(tmp) if STAGED.match(n)] if os.path.isdir(tmp) else []


def tree_bytes(root):
    """Each regular file under ROOT, relative to it, with its content. Links are
    checked on their own."""
    out = {}
    for dirpath, _, files in os.walk(root):
        for f in files:
            full = os.path.join(dirpath, f)
            if os.path.islink(full):
                continue
            rel = os.path.relpath(full, root)
            with open(full, "rb") as f:
                out[rel] = f.read()
    return out


def deflated_zip(path):
    rnd = random.Random(3)
    block = bytes(rnd.getrandbits(8) for _ in range(20000))
    big = block + b"ab" * 3000 + block[:15000] + bytes(rnd.getrandbits(8) for _ in range(90000)) \
        + block * 2
    files = {"big.dat": big, "small.txt": b"hello from a deflated member\n"}
    with zipfile.ZipFile(path, "w", zipfile.ZIP_DEFLATED) as z:
        for name, data in files.items():
            z.writestr(name, data)
    return files


class ExtractTests(MCTestCase):

    def setUp(self):
        super().setUp()
        self.tmp = os.path.join(self.root, "tmp")
        os.makedirs(self.tmp)
        self.env = {"TMPDIR": self.tmp}

    def panel(self, archive):
        """The directory of ARCHIVE, with the archive entered from the left panel."""
        t = self.start(os.path.dirname(archive), env=self.env)
        t.goto(t.find(os.path.basename(archive)))
        t.send(ENTER, 2.0)
        self.assertTrue(oracle.in_archive(t), "did not enter the archive")
        return t

    def mark(self, t, name):
        """Mark the entry NAME: a long name is found by its start, as mc cuts it."""
        t.goto(t.find(name if len(name) <= 26 else name[:12]))
        t.send(INSERT, .3)

    def copy_to(self, t, dest):
        """F5 on the marked items, or on the highlighted one, to DEST."""
        t.send(F5, .8)
        for _ in range(200):
            t.send(BS, .001)
        t.send(dest.encode() + b"/", .2)
        t.send(ENTER, 2.5)

    def test_f3_shows_a_file_in_every_form(self):
        d = self.new_dir()
        for form, (ending, how) in fixtures.TAR_FORMS.items():
            with self.subTest(form=form):
                if how == "zst" and not fixtures.have("zstd"):
                    self.skipTest("zstd is not installed")
                fixtures.tar_archive(d, "basic", fixtures.BASIC, form)
                t = self.panel(os.path.join(d, "basic" + ending))
                t.goto(t.find("docs"))
                t.send(ENTER, 1.0)
                t.goto(t.find("readme.md"))
                t.send(F3, 2.0)
                self.assertTrue(any("# readme" in t.line(y) for y in range(24)))
                t.send(b"q", 1.0)
                self.assertEqual(staged_left(self.tmp), [], "the staging directory is not removed")
                t.close()

    def test_f3_on_a_deflated_zip_member(self):
        d = self.new_dir()
        deflated_zip(os.path.join(d, "def.zip"))
        t = self.panel(os.path.join(d, "def.zip"))
        t.goto(t.find("small.txt"))
        t.send(F3, 2.0)
        self.assertTrue(any("hello from a deflated member" in t.line(y) for y in range(24)))
        t.send(b"q", 1.0)
        self.assertEqual(staged_left(self.tmp), [])

    def test_copy_of_marked_items_is_byte_exact(self):
        d = self.new_dir("src")
        dest = self.new_dir("dest")
        path = fixtures.tar_archive(d, "basic", fixtures.BASIC, "tgz")
        t = self.panel(path)
        for name in ("top.txt", "docs", "big.bin"):
            self.mark(t, name)
        self.copy_to(t, dest)
        want = {k: v for k, v in oracle.file_bytes(path).items()
                if k.split("/")[0] in ("top.txt", "docs", "big.bin")}
        self.assertEqual(tree_bytes(dest), want)
        t.close()

    def test_a_copy_of_several_items_runs_one_decompressor(self):
        if not fixtures.have("gzip"):
            self.skipTest("gzip is not installed")
        d = self.new_dir("src")
        dest = self.new_dir("dest")
        path = fixtures.tar_archive(d, "basic", fixtures.BASIC, "tgz")
        bindir = self.new_dir("bin")
        log = os.path.join(self.root, "gzip.log")
        with open(os.path.join(bindir, "gzip"), "w") as f:
            f.write("#!/bin/sh\necho \"$@\" >> %s\nexec %s \"$@\"\n"
                    % (log, shutil.which("gzip")))
        os.chmod(os.path.join(bindir, "gzip"), 0o755)
        env = dict(self.env, PATH=bindir + os.pathsep + os.environ["PATH"])
        t = self.start(d, env=env)
        t.goto(t.find("basic.tgz"))
        t.send(ENTER, 2.0)                               # reading the index is a pass too
        os.remove(log)
        for name in ("top.txt", "docs", "big.bin"):
            self.mark(t, name)
        self.copy_to(t, dest)
        with open(log) as f:
            self.assertEqual(len(f.read().splitlines()), 1,
                             "the group is extracted in one pass over the archive")
        want = {k: v for k, v in oracle.file_bytes(path).items()
                if k.split("/")[0] in ("top.txt", "docs", "big.bin")}
        self.assertEqual(tree_bytes(dest), want)
        t.close()

    def test_deflated_zip_members_are_byte_exact(self):
        d = self.new_dir("src")
        dest = self.new_dir("dest")
        files = deflated_zip(os.path.join(d, "def.zip"))
        t = self.panel(os.path.join(d, "def.zip"))
        self.mark(t, "big.dat")
        self.mark(t, "small.txt")
        self.copy_to(t, dest)
        self.assertEqual(tree_bytes(dest), files)
        t.close()

    def test_a_copy_into_an_archive_is_refused(self):
        d = self.new_dir("work")
        fixtures.put(os.path.join(d, "note.txt"), b"host\n")
        fixtures.tar_archive(d, "basic", fixtures.BASIC, "tgz")
        before = sha(os.path.join(d, "basic.tgz"))
        t = self.start(d, env=self.env)
        t.goto(t.find("basic.tgz"))
        t.send(ENTER, 2.0)                               # the left panel shows the archive
        t.tab()                                          # the right panel, the host one
        t.goto(t.find("note.txt"))
        t.send(F5, .8)
        self.assertTrue(any("Read-only archive" in t.line(y) for y in range(24)))
        t.send(b" ", .5)
        self.assertEqual(sha(os.path.join(d, "basic.tgz")), before)

    def test_move_and_delete_are_refused_inside_an_archive(self):
        d = self.new_dir("work")
        fixtures.put(os.path.join(d, "keep.txt"), b"keep\n")
        path = fixtures.tar_archive(d, "basic", fixtures.BASIC, "tgz")
        before = sha(path)
        t = self.panel(path)
        t.goto(t.find("top.txt"))
        for key, text in ((F6, "Read-only archive"), (F8, "Read-only archive")):
            t.send(key, .8)
            self.assertTrue(any(text in t.line(y) for y in range(24)))
            t.send(b" ", .5)
        self.assertEqual(sha(path), before)
        self.assertTrue(os.path.exists(os.path.join(d, "keep.txt")))
        t.close()

    def test_nothing_is_written_outside_the_staging_directory(self):
        d = self.new_dir("src")
        dest = self.new_dir("dest")
        outside = self.new_dir("outside")
        members = [
            ("dtrav/", "dir", b"", None),
            ("dtrav/ok.txt", "file", b"ok\n", None),
            ("dtrav/../../escape.txt", "file", b"escaped\n", None),
            ("lk", "link", b"", outside),
            ("lk/planted.txt", "file", b"planted\n", None),
        ]
        path = fixtures.tar_archive(d, "evil", members, "tar")
        t = self.panel(path)
        self.mark(t, "dtrav")
        self.mark(t, "lk")
        self.copy_to(t, dest)
        t.close()
        # the link is never followed: nothing is written where it points, and the
        # ".." name never reaches the file system
        self.assertEqual(os.listdir(outside), [])
        for dirpath, _, files in os.walk(self.root):
            self.assertNotIn("escape.txt", files)
        self.assertEqual(tree_bytes(os.path.join(dest, "dtrav")), {"ok.txt": b"ok\n"})

    def test_adversarial_members_are_copied_as_they_are(self):
        d = self.new_dir("src")
        dest = self.new_dir("dest")
        path = fixtures.tar_archive(d, "adv", fixtures.ADVERSARIAL, "tgz", "gnu")
        t = self.panel(path)
        picked = ["adir", "hard.txt", "a b", "u\u00f1i", fixtures.LONG_DIR]
        for name in picked:
            self.mark(t, name)
        self.copy_to(t, dest)
        want = {k: v for k, v in oracle.file_bytes(path).items()
                if k.split("/")[0] in picked}
        if "hard.txt" in picked:                         # a hard link: its target's content
            want["hard.txt"] = oracle.file_bytes(path)["top.txt"]
        got = tree_bytes(dest)
        self.maxDiff = None
        self.assertEqual(sorted(got), sorted(want))
        self.assertEqual(got, want)
        self.assertEqual(os.readlink(os.path.join(dest, "adir", "rel-link")), "inner.txt")
        t.close()

    def test_f3_takes_one_pass_over_the_archive(self):
        if not fixtures.have("gzip"):
            self.skipTest("gzip is not installed")
        d = self.new_dir("src")
        fixtures.tar_archive(d, "basic", fixtures.BASIC, "tgz")
        bindir = self.new_dir("bin")
        log = os.path.join(self.root, "gzip.log")
        with open(os.path.join(bindir, "gzip"), "w") as f:
            f.write("#!/bin/sh\necho \"$@\" >> %s\nexec %s \"$@\"\n"
                    % (log, shutil.which("gzip")))
        os.chmod(os.path.join(bindir, "gzip"), 0o755)
        env = dict(self.env, PATH=bindir + os.pathsep + os.environ["PATH"])
        t = self.start(d, env=env)
        t.goto(t.find("basic.tgz"))
        t.send(ENTER, 2.0)
        os.remove(log)
        t.goto(t.find("docs"))
        t.send(ENTER, 1.0)
        t.goto(t.find("readme.md"))
        t.send(F3, 2.0)
        t.send(b"q", 1.0)
        with open(log) as f:
            self.assertEqual(len(f.read().splitlines()), 1)
        t.close()

    def test_a_staging_failure_is_reported(self):
        d = self.new_dir("src")
        dest = self.new_dir("dest")
        path = fixtures.tar_archive(d, "basic", fixtures.BASIC, "tgz")
        env = {"TMPDIR": os.path.join(self.root, "missing", "dir")}
        t = self.panel(path)
        t.close()
        t = self.start(os.path.dirname(path), env=env)
        t.goto(t.find("basic.tgz"))
        t.send(ENTER, 2.0)
        t.goto(t.find("top.txt"))
        t.send(F3, 1.0)
        self.assertTrue(any("Cannot extract" in t.line(y) for y in range(24)))
        t.send(b" ", .5)
        self.copy_to(t, dest)
        self.assertTrue(any("Cannot extract" in t.line(y) for y in range(24)))
        self.assertEqual(tree_bytes(dest), {})
        t.close()


if __name__ == "__main__":
    unittest.main()
