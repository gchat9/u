"""config.h: a change there rebuilds mc, and the change shows."""
import os
import shutil
import subprocess
import tempfile
import unittest

import fixtures
import term
from case import MCTestCase

HERE = os.path.dirname(os.path.abspath(__file__))
MC_ROOT = os.path.abspath(os.path.join(HERE, ".."))
TREE = ["mc", "_sys", "_lib", "_arch", "_mk"]


class ConfigTests(MCTestCase):

    def build_with(self, edits):
        """Build a copy of mc with the given (old, new) edits to config.h."""
        copy = tempfile.mkdtemp(prefix="mc-config-")
        self.addCleanup(shutil.rmtree, copy, True)
        for name in TREE:
            src = os.path.join(MC_ROOT, "..", name) if name != "mc" else MC_ROOT
            shutil.copytree(src, os.path.join(copy, name),
                            ignore=shutil.ignore_patterns("mc", "*.o", "*.d"))
        cfg = os.path.join(copy, "mc", "config.h")
        with open(cfg) as f:
            text = f.read()
        for old, new in edits:
            self.assertIn(old, text)
            text = text.replace(old, new)
        with open(cfg, "w") as f:
            f.write(text)
        subprocess.run(["make"], cwd=os.path.join(copy, "mc"), check=True,
                       stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
        return os.path.join(copy, "mc", "mc")

    def test_dir_bold_and_archive_limit(self):
        binary = self.build_with([("#define DIR_BOLD   0", "#define DIR_BOLD   1"),
                                  ("#define AR_MAXE  (1u << 20)", "#define AR_MAXE  100u")])
        d = self.new_dir()
        os.makedirs(os.path.join(d, "adir"))
        fixtures.tar_archive(d, "many", [("d/", "dir", b"", None)] +
                             [("d/f%03d.txt" % i, "file", b"", None) for i in range(300)], "tar")
        t = term.Term([binary], d, {"LANG": "C"}, home=self.home())
        self.terms.append(t)
        cell = t.screen.buffer[t.find("adir") + 2][2]
        self.assertTrue(cell.bold, "directories are bold with DIR_BOLD 1")
        t.goto(t.find("many.tar"))
        t.send(term.ENTER, 2.0)
        t.goto(t.find("d"))
        t.send(term.ENTER, 1.0)
        t.send(term.END, .5)
        self.assertEqual(t.cursor_name().strip(), "f098.txt",
                         "the index keeps AR_MAXE entries, then drops the rest")


if __name__ == "__main__":
    unittest.main()
