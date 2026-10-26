#!/usr/bin/env python3
"""Validate and compare saved canonical hydraulic qualification reports.

Inputs are existing results from qualify_hydraulic_workload.py. No repositories
are opened or modified, and OS cache state is not inferred from measurements.
"""

import argparse
import csv
import json
import math
import os
from pathlib import Path
import statistics
import sys
import tempfile

from qualify_hydraulic_workload import parse_metrics

METRICS = (
    ("import_ms", "import_ms"),
    ("indexed_scalar_mean_ms", "scalar_edit_ms"),
    ("indexed_topology_mean_ms", "topology_edit_ms"),
    ("targeted_read_mean_ms", "targeted_read_ms"),
    ("full_network_read_ms", "network_read_ms"),
    ("divergent_diff_ms", "diff_ms"),
    ("canonical_merge_ms", "merge_ms"),
    ("fresh_process_reopen_full_verify_ms", "reopen_verify_ms"),
    ("pack_bytes_final", "pack_mib"),
    ("index_bytes_final", "index_mib"),
    ("pack_growth_bytes", "pack_growth_mib"),
    ("index_growth_bytes", "index_growth_mib"),
    ("peak_rss_kib", "peak_rss_mib"),
)


def require(condition, detail):
    if not condition:
        raise ValueError(detail)


def evidence_path(folder, name, expected):
    require(isinstance(name, str) and name == expected, "unexpected evidence log name")
    path = folder / name
    require(path.is_file() and not path.is_symlink(), "missing or unsafe evidence log: " + name)
    return path


def file_metrics(folder, entry, index, pipes, edits, topology):
    benchmark = evidence_path(folder, entry.get("benchmark_log"), f"benchmark-{index:02d}.log")
    verifier = evidence_path(folder, entry.get("verification_log"), f"verify-{index:02d}.log")
    raw = benchmark.read_text(encoding="utf-8")
    require(raw.startswith("COMMAND: ") and "\nSTDOUT:\n" in raw and "\nSTDERR:\n" in raw,
            "benchmark log is incomplete")
    stdout = raw.split("\nSTDOUT:\n", 1)[1].split("\nSTDERR:\n", 1)[0]
    recorded = parse_metrics(stdout, pipes, edits, topology)
    require(entry.get("metrics") == recorded,
            "benchmark log and JSON metrics disagree for repetition " + str(index))
    verification = verifier.read_text(encoding="utf-8")
    require("\nSTDOUT:\nFull native storage integrity verified:" in verification,
            "independent full verification evidence is missing")
    for name in ("benchmark_wall_ms", "fresh_process_reopen_full_verify_ms"):
        value = entry.get(name)
        require(type(value) in (int, float) and math.isfinite(value) and value >= 0,
                "invalid timing: " + name)
    for name in ("pack_sha256", "index_sha256"):
        digest = entry.get(name)
        require(isinstance(digest, str) and len(digest) == 64 and
                all(letter in "0123456789abcdef" for letter in digest),
                "invalid digest: " + name)
    return entry


def read_report(user_path):
    path = user_path / "report.json" if user_path.is_dir() else user_path
    require(path.is_file(), "report not found: " + str(path))
    report = json.loads(path.read_text(encoding="utf-8"))
    require(report.get("kind") == "aowis-dvmdb-hydraulic-workload-qualification" and
            report.get("version") == 1, "unsupported hydraulic report")
    parameters = report.get("parameters")
    require(isinstance(parameters, dict), "missing workload parameters")
    pipes, edits, topology, repeats = (parameters.get(key) for key in
                                     ("pipes", "scalar_edits", "topology_edits", "repeats"))
    require(all(type(value) is int and value > 0 for value in
                (pipes, edits, topology, repeats)) and pipes >= 2,
            "invalid workload parameters")
    runs = report.get("runs")
    require(isinstance(runs, list) and len(runs) == repeats,
            "incomplete workload repetitions")
    for binary_key in ("binary", "verifier"):
        info = report.get(binary_key)
        require(isinstance(info, dict), "missing executable identity: " + binary_key)
        sha = info.get("sha256")
        require(isinstance(sha, str) and len(sha) == 64 and
                all(letter in "0123456789abcdef" for letter in sha),
                "invalid executable SHA-256: " + binary_key)
    for index, run in enumerate(runs, 1):
        require(isinstance(run, dict) and run.get("iteration") == index,
                "invalid repetition number")
        file_metrics(path.parent, run, index, pipes, edits, topology)
    return path, report


def aggregate(path, report):
    parameters = report["parameters"]
    runs = report["runs"]
    row = {
        "report": str(path),
        "pipes": parameters["pipes"],
        "repeats": len(runs),
        "scalar_edits": parameters["scalar_edits"],
        "topology_edits": parameters["topology_edits"],
        "benchmark_sha256": report["binary"]["sha256"],
        "verifier_sha256": report["verifier"]["sha256"],
    }
    for source, target in METRICS:
        values = [run["fresh_process_reopen_full_verify_ms"] if
                  source == "fresh_process_reopen_full_verify_ms" else run["metrics"][source]
                  for run in runs]
        number = statistics.median(values)
        if source.endswith("_bytes_final") or source.endswith("_growth_bytes") or source == "peak_rss_kib":
            number /= 1048576.0 if source != "peak_rss_kib" else 1024.0
        row[target] = round(number, 5)
    row["pack_bytes_per_pipe"] = round(row["pack_mib"] * 1048576 / row["pipes"], 2)
    row["index_bytes_per_pipe"] = round(row["index_mib"] * 1048576 / row["pipes"], 2)
    return row


def analyze(paths):
    require(paths, "at least one hydraulic report is required")
    seen = set()
    summaries = []
    signature = None
    for user_path in paths:
        path, report = read_report(user_path)
        require(path.resolve() not in seen, "duplicate report: " + str(path))
        seen.add(path.resolve())
        parameters = report["parameters"]
        current = (report["binary"]["sha256"], report["verifier"]["sha256"],
                   parameters["scalar_edits"], parameters["topology_edits"])
        if signature is None:
            signature = current
        require(current == signature,
                "cannot compare different benchmark/verifier binaries or edit settings")
        summaries.append(aggregate(path, report))
    summaries.sort(key=lambda item: (item["pipes"], item["report"]))
    base = summaries[0]
    for row in summaries:
        row["pipe_ratio"] = round(row["pipes"] / base["pipes"], 4)
        row["network_read_ratio"] = round(row["network_read_ms"] / base["network_read_ms"], 4) if base["network_read_ms"] else None
        row["merge_ratio"] = round(row["merge_ms"] / base["merge_ms"], 4) if base["merge_ms"] else None
    return summaries


def export_csv(path, rows):
    path.parent.mkdir(parents=True, exist_ok=True)
    temp = None
    try:
        with tempfile.NamedTemporaryFile(mode="w", newline="", encoding="utf-8",
                                         dir=path.parent, prefix=path.name + ".",
                                         suffix=".tmp", delete=False) as stream:
            temp = Path(stream.name)
            writer = csv.DictWriter(stream, fieldnames=list(rows[0]))
            writer.writeheader()
            writer.writerows(rows)
        os.replace(temp, path)
    finally:
        if temp is not None:
            temp.unlink(missing_ok=True)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("reports", nargs="+", type=Path,
                        help="hydraulic-workload-* directory or report.json, one or more")
    parser.add_argument("--csv", type=Path, help="optional CSV summary output")
    arguments = parser.parse_args()
    try:
        rows = analyze(arguments.reports)
        print("Canonical hydraulic scaling (median of independent fresh repositories):")
        print("pipes   runs  pack_MiB   KiB/pipe  read_ms   merge_ms  verify_ms  peak_RSS_MiB")
        for row in rows:
            print(f"{row['pipes']:7d} {row['repeats']:5d} "
                  f"{row['pack_mib']:9.2f} {row['pack_bytes_per_pipe'] / 1024:10.2f} "
                  f"{row['network_read_ms']:8.2f} {row['merge_ms']:9.2f} "
                  f"{row['reopen_verify_ms']:10.2f} {row['peak_rss_mib']:13.2f}")
        print("Comparison uses identical benchmark/verifier SHA-256 and edit settings.")
        print("Read/merge timings use an uncontrolled OS cache; verify includes new process startup and full integrity scan.")
        if arguments.csv is not None:
            export_csv(arguments.csv, rows)
            print("Saved CSV: " + str(arguments.csv))
    except (ValueError, OSError, KeyError, TypeError, json.JSONDecodeError) as error:
        print("hydraulic analysis FAILED: " + str(error), file=sys.stderr)
        sys.exit(1)


if __name__ == "__main__":
    main()
