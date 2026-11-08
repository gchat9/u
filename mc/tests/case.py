"""The base of the tests: a directory of their own, and the programs they start."""
import os
import shutil
import tempfile
import unittest

import term


class MCTestCase(unittest.TestCase):

    def setUp(self):
        self.root = tempfile.mkdtemp(prefix="mc-test-")
        self.terms = []

    def tearDown(self):
        for t in self.terms:
            t.close()
        shutil.rmtree(self.root, ignore_errors=True)

    def home(self):
        """A HOME of its own, outside the test's directories: mc shows paths
        under it as ~, so no test directory may lie under it."""
        path = os.path.join(self.root, "home")
        os.makedirs(path, exist_ok=True)
        return path

    def new_dir(self, name="work"):
        path = os.path.join(self.root, name)
        os.makedirs(path, exist_ok=True)
        return path

    def start(self, cwd, env=None):
        t = term.ours(cwd, home=self.home(), env=env)
        self.terms.append(t)
        return t

    def start_reference(self, cwd, env=None):
        t = term.reference(cwd, self.home(), env)
        self.terms.append(t)
        return t
