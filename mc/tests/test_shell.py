"""Shell links, sh://user@host/dir: a directory on another machine, reached by ssh.
The tests use this machine as the other one, with key login to root at 127.0.0.1;
they are skipped without that."""
import os
import shutil
import subprocess
import tempfile
import unittest

import term
from case import MCTestCase
from term import DOWN, ENTER, ESC, UP

F9 = b"\x1b[20~"
F8 = b"\x1b[19~"
F6 = b"\x1b[17~"
F5 = b"\x1b[15~"
F3 = b"\x1b[13~"
TAB = b"\t"
HOST = "root@127.0.0.1"


def key_login_works():
    if not shutil.which("ssh"):
        return False
    try:
        r = subprocess.run(["ssh", "-o", "BatchMode=yes", "-o", "ConnectTimeout=5", HOST, "true"],
                           stdin=subprocess.DEVNULL, stdout=subprocess.DEVNULL,
                           stderr=subprocess.DEVNULL, timeout=20)
        return r.returncode == 0
    except (subprocess.SubprocessError, OSError):
        return False


NEEDS_SSH = unittest.skipUnless(key_login_works(),
                                "needs key login to root@127.0.0.1 (sshd running)")


def names(t):
    return [n[1:] for n, _ in t.entries() if n[1:] != ".."]


def remote_fixture(tc):
    """A directory of this machine, used as the remote one: a subdirectory and a file."""
    d = tempfile.mkdtemp(dir=tc.root)
    os.mkdir(os.path.join(d, "remote"))
    os.mkdir(os.path.join(d, "remote", "d1"))
    with open(os.path.join(d, "remote", "f1"), "w") as f:
        f.write("data")
    return d, os.path.join(d, "remote")


def link(path):
    return "sh://" + HOST + path


@NEEDS_SSH
class ShellLinkTests(MCTestCase):

    def test_cd_to_a_link_lists_the_remote_directory(self):
        local = self.new_dir()
        _, remote = remote_fixture(self)
        t = self.start(local)
        t.send(("cd " + link(remote) + "\r").encode(), 5.0)
        self.assertIn("sh://" + HOST, t.line(0))
        self.assertEqual(names(t), ["d1", "f1"])

    @unittest.skipUnless(os.path.exists(term.REFERENCE), "the installed mc is not there")
    def test_matches_reference_listing(self):
        local = self.new_dir()
        _, remote = remote_fixture(self)
        shown = []
        for which in ("ref", "ours"):
            t = self.start_reference(local) if which == "ref" else self.start(local)
            t.send(("cd " + link(remote) + "\r").encode(), 5.0)
            shown.append((t.line(0).strip(), names(t), [t.line(y)[27:45].strip() for y in (3, 4)]))
            t.close()
        self.assertEqual(shown[0][1], shown[1][1])

    def test_enter_and_up_inside_the_link(self):
        local = self.new_dir()
        _, remote = remote_fixture(self)
        t = self.start(local)
        t.send(("cd " + link(remote) + "\r").encode(), 5.0)
        t.send(DOWN, .4)                       # d1, which is empty
        t.send(ENTER, 2.0)
        self.assertEqual(names(t), [])
        t.send(UP, .4)
        t.send(ENTER, 2.0)                     # ".." is the remote directory again
        self.assertEqual(names(t), ["d1", "f1"])

    def test_up_from_the_root_of_the_link_goes_back_to_the_local_directory(self):
        local = self.new_dir()
        t = self.start(local)
        t.send(("cd " + link("/") + "\r").encode(), 5.0)
        self.assertIn("sh://", t.line(0))
        for _ in range(30):                    # "cd /" leaves the cursor on the way back: to ".."
            t.send(UP, .05)
        t.send(ENTER, 2.0)                     # ".." at the root
        self.assertIn(local, t.line(0))
        self.assertNotIn("sh://", t.line(0))

    def test_links_are_shown_with_the_directories_and_the_files(self):
        local = self.new_dir()
        _, remote = remote_fixture(self)
        os.symlink(os.path.join(remote, "d1"), os.path.join(remote, "linkdir"))
        os.symlink(os.path.join(remote, "f1"), os.path.join(remote, "linkfile"))
        os.symlink(os.path.join(remote, "gone"), os.path.join(remote, "dangling"))
        t = self.start(local)
        t.send(("cd " + link(remote) + "\r").encode(), 5.0)
        marked = [(n, m) for n, m in t.entries()]
        marks = {n[1:]: n[0] for n, _ in marked}
        self.assertEqual(marks["linkdir"], "~")
        self.assertEqual(marks["linkfile"], "@")
        self.assertEqual(marks["dangling"], "!")
        order = [n[1:] for n, _ in marked if n[1:] != ".."]
        self.assertEqual(order[:2], ["d1", "linkdir"])          # with the directories
        self.assertLess(order.index("linkfile"), order.index("f1") + 2)
        self.assertLess(order.index("f1"), len(order))

    def test_view_shows_the_remote_file(self):
        local = self.new_dir()
        _, remote = remote_fixture(self)
        with open(os.path.join(remote, "f1"), "w") as f:
            f.write("REMOTE TEXT\n")
        t = self.start(local)
        t.send(("cd " + link(remote) + "\r").encode(), 5.0)
        t.send(DOWN, .4)
        t.send(DOWN, .4)                       # f1
        t.send(F3, 2.5)
        self.assertTrue(any("REMOTE TEXT" in t.line(y) for y in range(24)))
        t.send(b"q", 1.5)

    def test_delete_removes_a_remote_file(self):
        local = self.new_dir()
        _, remote = remote_fixture(self)
        t = self.start(local)
        t.send(("cd " + link(remote) + "\r").encode(), 5.0)
        t.send(DOWN, .4)
        t.send(DOWN, .4)                       # f1
        t.send(F8, 1.2)
        t.send(b"\r", 3.0)                     # Delete file "f1"?
        self.assertFalse(os.path.exists(os.path.join(remote, "f1")))

    def test_delete_keeps_a_directory_that_is_not_empty(self):
        local = self.new_dir()
        _, remote = remote_fixture(self)
        os.mkdir(os.path.join(remote, "d2"))             # sorts after d1
        with open(os.path.join(remote, "d2", "inner"), "w") as f:
            f.write("x")
        t = self.start(local)
        t.send(("cd " + link(remote) + "\r").encode(), 5.0)
        t.send(DOWN, .4)                                 # d1, which is empty: removed
        t.send(F8, 1.2)
        t.send(b"\r", 3.0)
        self.assertFalse(os.path.exists(os.path.join(remote, "d1")))
        t.send(F8, 1.2)                                  # d2 now, which is not
        t.send(b"\r", 3.0)
        self.assertTrue(os.path.exists(os.path.join(remote, "d2", "inner")))
        self.assertIn("Cannot delete", " ".join(t.lines()))

    def test_copy_from_the_link_to_this_machine(self):
        local = self.new_dir()
        _, remote = remote_fixture(self)
        t = self.start(local)
        t.send(("cd " + link(remote) + "\r").encode(), 5.0)
        t.send(DOWN, .4)
        t.send(DOWN, .4)                       # f1
        t.send(F5, 1.2)
        t.send(b"\r", 4.0)
        self.assertTrue(os.path.exists(os.path.join(remote, "f1")))
        with open(os.path.join(local, "f1")) as f:
            self.assertEqual(f.read(), "data")

    def test_move_from_the_link_to_this_machine(self):
        local = self.new_dir()
        _, remote = remote_fixture(self)
        t = self.start(local)
        t.send(("cd " + link(remote) + "\r").encode(), 5.0)
        t.send(DOWN, .4)
        t.send(DOWN, .4)
        t.send(F6, 1.2)
        t.send(b"\r", 4.0)
        self.assertFalse(os.path.exists(os.path.join(remote, "f1")))
        self.assertTrue(os.path.exists(os.path.join(local, "f1")))

    def test_copy_from_this_machine_to_the_link(self):
        local = self.new_dir()
        _, remote = remote_fixture(self)
        with open(os.path.join(local, "up"), "w") as f:
            f.write("UP")
        t = self.start(local)
        t.send(TAB, .4)
        t.send(("cd " + link(remote) + "\r").encode(), 5.0)
        t.send(TAB, .4)
        for _ in range(2):
            t.send(DOWN, .3)                   # up
        t.send(F5, 1.2)
        t.send(b"\r", 4.0)
        with open(os.path.join(remote, "up")) as f:
            self.assertEqual(f.read(), "UP")

    def test_a_name_that_exists_on_the_link_is_not_replaced(self):
        local = self.new_dir()
        _, remote = remote_fixture(self)
        with open(os.path.join(local, "f1"), "w") as f:
            f.write("NEW")
        t = self.start(local)
        t.send(TAB, .4)
        t.send(("cd " + link(remote) + "\r").encode(), 5.0)
        t.send(TAB, .4)
        for _ in range(2):
            t.send(DOWN, .3)                   # f1 is local now: the second entry
        t.send(F5, 1.2)
        t.send(b"\r", 4.0)
        with open(os.path.join(remote, "f1")) as f:
            self.assertEqual(f.read(), "data")

    def test_a_missing_directory_is_reported_and_the_panel_stays(self):
        local = self.new_dir()
        _, remote = remote_fixture(self)
        t = self.start(local)
        t.send(("cd " + link(os.path.join(remote, "nosuch")) + "\r").encode(), 5.0)
        self.assertIn("Cannot change directory to", " ".join(t.lines()))
        t.send(ESC, 1.0)
        self.assertIn(local, t.line(0))

    def test_an_unreachable_host_is_reported(self):
        local = self.new_dir()
        t = self.start(local)
        t.send(b"cd sh://nobody@nosuchhost.invalid/tmp\r", 9.0)
        self.assertIn("Cannot connect to", " ".join(t.lines()))
        self.assertIn(local, t.line(0))

    def test_the_menu_opens_the_shell_link_dialog(self):
        local = self.new_dir()
        _, remote = remote_fixture(self)
        t = self.start(local)
        t.send(F9, .6)
        t.send(ENTER, .6)
        t.send(DOWN, .5)                       # Shell link...
        t.send(ENTER, 1.0)
        self.assertIn("Shell link to machine", " ".join(t.lines()))
        t.send((HOST + remote).encode(), .5)
        t.send(b"\r", 5.0)
        self.assertIn("sh://" + HOST, t.line(0))
        self.assertEqual(names(t), ["d1", "f1"])

    def test_escape_cancels_the_shell_link_dialog(self):
        local = self.new_dir()
        t = self.start(local)
        t.send(F9, .6)
        t.send(ENTER, .6)
        t.send(DOWN, .5)
        t.send(ENTER, 1.0)
        t.send(ESC, 1.5)
        self.assertNotIn("Shell link to machine", " ".join(t.lines()))
        self.assertIn(local, t.line(0))


if __name__ == "__main__":
    unittest.main()
