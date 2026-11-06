#!/usr/bin/env python3
"""Run the tests of mc: python3 tests/run.py. make tests builds mc, then runs
this. The tests need the pyte package (pip install pyte); the reference tests
need the installed mc (apt-get install mc), and the compressed ones zstd."""
import os
import sys
import unittest

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)

try:
    import pyte  # noqa: F401
except ImportError:
    sys.exit("the tests need the pyte package: pip install pyte")

if not os.path.exists(os.path.join(HERE, "..", "mc")):
    sys.exit("mc is not built: run make first")

suite = unittest.defaultTestLoader.discover(HERE, pattern="test_*.py", top_level_dir=HERE)
result = unittest.TextTestRunner(verbosity=2).run(suite)
sys.exit(0 if result.wasSuccessful() else 1)
