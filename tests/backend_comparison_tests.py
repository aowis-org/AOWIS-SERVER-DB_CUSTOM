#!/usr/bin/env python3
"""Independent CLI checks for backend comparison setup and data integrity."""

import argparse
import csv
import pathlib
import subprocess
import tempfile


def run(arguments, expected=0):
    completed = subprocess.run(arguments, text=True, capture_output=True, check=False, timeout=60)
    if (completed.returncode == 0) != (expected == 0):
        raise AssertionError(f"Unexpected exit {completed.returncode}: {completed.stderr}")
    return completed


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--script", required=True)
    parser.add_argument("--binary", required=True)
    arguments = parser.parse_args()
    with tempfile.TemporaryDirectory(prefix="aowis-backend-test-") as directory:
        base = pathlib.Path(directory)
        output = base / "samples.csv"
        cmd = ["python3", arguments.script, "--model-binary", arguments.binary,
               "--scenario", "model", "--pipes", "32", "--edits", "3", "--repeats", "2",
               "--output", str(output)]
        run(cmd)
        with output.open(newline="", encoding="utf-8") as file:
            samples = list(csv.DictReader(file))
        assert len(samples) == 4
        assert {row["backend"] for row in samples} == {"pack", "block"}
        assert {row["run"] for row in samples} == {"1", "2"}
        for row in samples:
            assert int(row["scan_count"]) == 65
            assert int(row["diff_count"]) == 6
            assert int(row["verified_revisions"]) == 9
            assert row["head"]
            assert int(row["disk_bytes_after"]) > 0
            assert float(row["audit_ms"]) > 0
            assert float(row["post_compact_verify_ms"]) > 0
        for repetition in ("1", "2"):
            pairs = [row for row in samples if row["run"] == repetition]
            assert pairs[0]["head"] == pairs[1]["head"]
        # Existing results must never be overwritten or silently appended.
        run(cmd, expected=1)
        # Invalid backend workload input must be rejected before writing results.
        invalid = ["python3", arguments.script, "--model-binary", arguments.binary,
                   "--scenario", "model", "--pipes", "1", "--edits", "1",
                   "--output", str(base / "invalid.csv")]
        run(invalid, expected=1)
        assert not (base / "invalid.csv").exists()
        # Missing canonical hydraulic benchmark must fail explicitly.
        missing = ["python3", arguments.script, "--model-binary", arguments.binary,
                   "--scenario", "hydraulic", "--output", str(base / "missing.csv")]
        run(missing, expected=1)
        assert not (base / "missing.csv").exists()
        assert not list(base.glob("*repo*"))
    print("Backend comparison CLI regressions passed")


if __name__ == "__main__":
    main()
