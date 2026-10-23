#!/usr/bin/env python3
"""Regression test for persistent replication fixtures and restartable measurements."""

import argparse
import csv
import json
import subprocess
import sys
import tempfile
from pathlib import Path


def run(command, success=True):
    process = subprocess.run(command, text=True, capture_output=True, check=False)
    if (process.returncode == 0) != success:
        raise AssertionError(f"unexpected result {process.returncode}: {' '.join(map(str, command))}\n"
                             f"stdout:\n{process.stdout}\nstderr:\n{process.stderr}")
    return process


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--script", required=True, type=Path)
    parser.add_argument("--binary", required=True, type=Path)
    args = parser.parse_args()
    with tempfile.TemporaryDirectory(prefix="aowis-db-custom-benchmark-phases-") as temp:
        root = Path(temp)
        fixtures = root / "fixtures"
        results = root / "results.csv"
        common = [sys.executable, str(args.script), "--binary", str(args.binary), "--smoke"]
        prepare = common + ["--mode", "prepare", "--fixture-dir", str(fixtures), "--progress"]
        imported = common + ["--mode", "imports", "--fixture-dir", str(fixtures),
                             "--output", str(results)]
        progress = run(prepare)
        if "Fixture model/pack: commit 1/4" not in progress.stderr:
            raise AssertionError("per-commit preparation progress is missing")
        manifest = json.loads((fixtures / "manifest.json").read_text(encoding="utf-8"))
        if len(manifest["fixtures"]) != 2:
            raise AssertionError("fixture preparation did not cover all source combinations")
        if "already exists" not in run(prepare, success=False).stderr:
            raise AssertionError("preparing fixtures can overwrite existing data")
        if "fixture manifest mismatch" not in run([sys.executable, str(args.script), "--binary", str(args.binary),
                                                   "--mode", "imports", "--fixture-dir", str(fixtures),
                                                   "--objects", "100", "--commits", "4",
                                                   "--payload-bytes", "32", "--kinds", "both",
                                                   "--backends", "pack", "--repeats", "1"],
                                                  success=False).stderr:
            raise AssertionError("mismatched fixtures were accepted")
        run(imported)
        with results.open(newline="", encoding="utf-8") as stream:
            rows = list(csv.DictReader(stream))
        if len(rows) != 4 or any(int(row["import_peak_rss_kib"]) <= 0 for row in rows):
            raise AssertionError("split benchmark produced incomplete import results")
        # Profile data comes from inside the native importer, not from the
        # independent post-import audit or Python launcher. Every phase must
        # be measured and the nonoverlapping phases must fit the total.
        phases = ("profile_scan_ms", "profile_graph_ms",
                  "profile_model_references_ms", "profile_reachability_ms",
                  "profile_branch_ms", "profile_write_ms", "profile_publication_ms")
        for row in rows:
            total = float(row["profile_total_ms"])
            summed = sum(float(row[field]) for field in phases)
            if total <= 0 or any(float(row[field]) < 0 for field in phases):
                raise AssertionError("import profile has missing or negative stage timings")
            if summed > total + max(0.2, total * 0.02):
                raise AssertionError("import profile double-counted a stage")
            # The profile runs inside the benchmark's import timer, so it can
            # never exceed it. (A fixed upper gap would only measure machine
            # load: other processes can preempt between the two clocks.)
            if total > float(row["import_ms"]) + 0.5:
                raise AssertionError("native import profile exceeds the benchmark timer")
            if int(row["profile_bundle_reads"]) <= 0 or int(row["profile_bundle_read_bytes"]) <= 0:
                raise AssertionError("import profile omitted transferred object reads")
            if row["phase"] == "import-full":
                if int(row["profile_local_reads"]) != 0:
                    raise AssertionError("full import unexpectedly used local object fallback")
            elif row["phase"] == "import-delta":
                if int(row["profile_local_reads"]) <= 0 or int(row["profile_local_read_bytes"]) <= 0:
                    raise AssertionError("incremental import omitted existing object reads")
        expected_heads = {}
        for row in rows:
            key = (row["kind"], row["source_backend"])
            expected_heads.setdefault(key, row["head"])
            if row["head"] != expected_heads[key]:
                raise AssertionError("incremental import does not match full import")
        if list(fixtures.glob("*full*")):
            raise AssertionError("benchmark destination repositories leaked into fixtures")
        # Reused fixtures must support multiple independent repeatable measurements.
        run(imported)
        bundle = fixtures / "fixture-model-pack" / "delta.aowbundle"
        original = bundle.read_bytes()
        try:
            bundle.write_bytes(original + b"corrupt")
            if "fixture checksum mismatch" not in run(imported, success=False).stderr:
                raise AssertionError("altered persistent fixture was accepted")
        finally:
            bundle.write_bytes(original)
        run(imported)
        if any(item.name.endswith(".tmp") for item in fixtures.iterdir()):
            raise AssertionError("benchmark left temporary manifest files")
    print("Replication benchmark separate prepare/import phases passed")


if __name__ == "__main__":
    main()
