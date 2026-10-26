#!/usr/bin/env python3
"""Verify capacity measurements cannot accidentally reuse a stale build tree."""

import argparse
import importlib.util
import os
import tempfile
import unittest
from pathlib import Path
from unittest.mock import patch


parser = argparse.ArgumentParser()
parser.add_argument("--script", type=Path, required=True)
settings = parser.parse_args()
spec = importlib.util.spec_from_file_location("index_benchmark", settings.script)
driver = importlib.util.module_from_spec(spec)
spec.loader.exec_module(driver)
source_root = settings.script.resolve().parent.parent


class ReleaseBuildSelectionTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory(prefix="aowis-index-build-tests-")
        self.addCleanup(self.temp.cleanup)
        self.build_dir = Path(self.temp.name) / "build-linux-tests-release"
        self.build_dir.mkdir()
        self.binary = self.build_dir / "aowis-dvmdb-index-scaling-benchmark"

    def configure(self, kind="Release", project=source_root):
        (self.build_dir / "CMakeCache.txt").write_text(
            f"CMAKE_HOME_DIRECTORY:INTERNAL={project}\n"
            f"CMAKE_BUILD_TYPE:STRING={kind}\n", encoding="utf-8")

    def test_rebuilds_explicit_selected_target(self):
        self.configure()
        commands = []

        def fake_build(command, check, stdout, stderr):
            self.assertTrue(check)
            self.assertIs(stdout, driver.sys.stderr)
            self.assertIs(stderr, driver.sys.stderr)
            commands.append(command)
            self.binary.write_bytes(b"updated by rebuild")
            self.binary.chmod(0o755)

        with patch.object(driver.subprocess, "run", side_effect=fake_build):
            driver.rebuild_selected_release_benchmark(self.binary)
        self.assertEqual(commands, [["cmake", "--build", str(self.build_dir),
                                     "--target", self.binary.name]])

    def test_refuses_debug_build(self):
        self.configure(kind="Debug")
        with patch.object(driver.subprocess, "run") as invoked:
            with self.assertRaisesRegex(RuntimeError, "not configured as Release"):
                driver.rebuild_selected_release_benchmark(self.binary)
            invoked.assert_not_called()

    def test_refuses_other_source_tree(self):
        self.configure(project=Path(self.temp.name) / "unrelated")
        with patch.object(driver.subprocess, "run") as invoked:
            with self.assertRaisesRegex(RuntimeError, "not configured from this source tree"):
                driver.rebuild_selected_release_benchmark(self.binary)
            invoked.assert_not_called()

    def test_refuses_unconfigured_directory(self):
        with patch.object(driver.subprocess, "run") as invoked:
            with self.assertRaisesRegex(RuntimeError, "not a configured CMake build directory"):
                driver.rebuild_selected_release_benchmark(self.binary)
            invoked.assert_not_called()

    def test_refuses_unexpected_executable_name(self):
        self.configure()
        with self.assertRaisesRegex(RuntimeError, "unexpected physical-index benchmark"):
            driver.rebuild_selected_release_benchmark(self.build_dir / "wrong-benchmark")


if __name__ == "__main__":
    unittest.main(argv=["index_benchmark_build_tests.py"])
