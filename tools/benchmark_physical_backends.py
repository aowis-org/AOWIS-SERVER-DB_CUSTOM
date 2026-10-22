#!/usr/bin/env python3
"""Compare native pack/block storage with equal model and canonical hydraulic workflows.

Every measured phase is executed by a distinct process, and each repository
is newly created. A failed benchmark exits nonzero and leaves completed CSV
rows readable, without treating partial samples as a valid comparison.
"""

import argparse
import csv
import pathlib
import statistics
import subprocess
import sys
import tempfile
import time


COLUMNS = [
    "run", "scenario", "backend", "pipes", "edits", "head",
    "create_wall_ms", "inspect_wall_ms", "create_peak_rss_kib", "inspect_peak_rss_kib",
    "initial_ms", "import_ms", "edit_mean_ms", "edit_total_ms", "merge_ms",
    "indexed_read_ms", "diff_ms", "diff_count",
    "indexed_scalar_mean_ms", "indexed_topology_mean_ms",
    "indexed_membership_insert_batch_ms", "indexed_membership_delete_label_ms",
    "indexed_membership_delete_pipe_junction_batch_ms",
    "indexed_catalogue_update_mean_ms", "indexed_catalogue_insert_batch_ms",
    "indexed_catalogue_delete_batch_ms", "indexed_physical_update_batch_mean_ms",
    "indexed_structural_insert_batch_ms", "indexed_structural_delete_batch_ms",
    "indexed_dependent_insert_batch_ms", "indexed_dependent_update_batch_ms",
    "indexed_dependent_delete_batch_ms", "network_membership_chunk_count",
    "scan_count", "scan_ms", "audit_ms", "compact_ms", "reopen_ms", "post_compact_verify_ms",
    "verified_revisions", "verified_reachable", "objects_before", "objects_after",
    "pack_bytes_before", "pack_bytes_after", "disk_bytes_before", "disk_bytes_after",
]


def parse_metrics(output, source):
    metrics = {}
    for line in output.splitlines():
        if not line.strip():
            continue
        if "=" not in line:
            raise ValueError(f"{source}: not a key=value measurement: {line!r}")
        key, value = line.split("=", 1)
        if not key or key in metrics:
            raise ValueError(f"{source}: duplicate or invalid metric: {key!r}")
        metrics[key] = value
    return metrics


def run_binary(command, timeout):
    beginning = time.monotonic()
    execution = subprocess.run(command, text=True, capture_output=True, timeout=timeout, check=False)
    wall_ms = (time.monotonic() - beginning) * 1000.0
    if execution.returncode != 0:
        raise RuntimeError(f"{' '.join(map(str, command))} exited {execution.returncode}:\n"
                           f"{execution.stderr.strip()}\n{execution.stdout.strip()}")
    return parse_metrics(execution.stdout, str(command[0])), wall_ms


def require_number(metrics, key):
    if key not in metrics:
        raise RuntimeError(f"benchmark result is missing {key}")
    try:
        value = float(metrics[key])
    except ValueError as error:
        raise RuntimeError(f"invalid numeric benchmark metric {key}") from error
    if not 0 <= value < float("inf"):
        raise RuntimeError(f"invalid benchmark value {key}: {metrics[key]}")
    return value


def run_case(args, scenario, mode, repeat, root):
    directory = root / f"{scenario}-{mode}-{repeat}"
    if scenario == "model":
        command = [str(args.model_binary), "model", str(directory), mode,
                   str(args.pipes), str(args.edits)]
    else:
        command = [str(args.hydraulic_binary), str(directory), str(args.pipes),
                   str(args.scalar_edits), str(args.topology_edits)]
        if mode == "block":
            command.append("block")
    created, create_wall = run_binary(command, args.timeout)
    if scenario == "model":
        if created.get("scenario") != "model" or created.get("backend") != mode:
            raise RuntimeError("model benchmark returned incorrect identity")
        require_number(created, "initial_ms")
        require_number(created, "merge_ms")
        if require_number(created, "diff_count") != args.edits * 2:
            raise RuntimeError("merged difference count is incorrect")
    else:
        if created.get("fixture") != "canonical-hydraulic-network":
            raise RuntimeError("hydraulic benchmark did not use the canonical model fixture")
        if created.get("engine") != ("blocks" if mode == "block" else "stage7"):
            raise RuntimeError("hydraulic benchmark selected the wrong backend")
        require_number(created, "import_ms")
        require_number(created, "indexed_scalar_mean_ms")
    inspected, inspect_wall = run_binary(
        [str(args.model_binary), "inspect", str(directory), mode], args.timeout)
    if inspected.get("scenario") != "inspect" or inspected.get("backend") != mode:
        raise RuntimeError("inspector returned incorrect backend identity")
    for name in ("scan_count", "audit_ms", "compact_ms", "reopen_ms",
                 "verified_revisions", "verified_reachable", "objects_before", "objects_after",
                 "pack_bytes_before", "pack_bytes_after", "disk_bytes_before", "disk_bytes_after"):
        require_number(inspected, name)
    if require_number(inspected, "objects_after") > require_number(inspected, "objects_before"):
        raise RuntimeError("compaction increased the object count")
    if scenario == "model" and created.get("head") != inspected.get("head"):
        raise RuntimeError("inspector head disagrees with created model fixture")
    row = {field: "" for field in COLUMNS}
    row.update(run=repeat, scenario=scenario, backend=mode, pipes=args.pipes,
               edits=args.edits if scenario == "model" else args.scalar_edits,
               head=inspected["head"], create_wall_ms=round(create_wall, 3),
               inspect_wall_ms=round(inspect_wall, 3),
               create_peak_rss_kib=created.get("peak_rss_kib", ""),
               inspect_peak_rss_kib=inspected.get("peak_rss_kib", ""))
    for source in (created, inspected):
        for key in COLUMNS:
            if key in source and key not in ("backend", "scenario", "head", "run"):
                row[key] = source[key]
    return row


def compare_pairs(pairs):
    for (scenario, repeat), rows in pairs.items():
        if len(rows) != 2:
            raise RuntimeError(f"incomplete comparison for {scenario} repetition {repeat}")
        left, right = rows
        if left["head"] != right["head"]:
            raise RuntimeError(f"pack/block produced different revision identities for {scenario} run {repeat}")
        if left["scan_count"] != right["scan_count"]:
            raise RuntimeError(f"pack/block reconstructed different entity counts for {scenario} run {repeat}")
        if left["verified_revisions"] != right["verified_revisions"]:
            raise RuntimeError(f"pack/block retained different histories for {scenario} run {repeat}")


def summarize(rows):
    print("\nMedian of completed Release samples (lower is better for timing/size):")
    for scenario in sorted(set(row["scenario"] for row in rows)):
        for field in ("import_ms" if scenario == "hydraulic" else "initial_ms",
                      "indexed_scalar_mean_ms" if scenario == "hydraulic" else "edit_mean_ms",
                      "compact_ms", "audit_ms", "disk_bytes_before", "disk_bytes_after"):
            medians = {}
            for mode in ("pack", "block"):
                values = [float(row[field]) for row in rows if row["scenario"] == scenario
                          and row["backend"] == mode and row[field] != ""]
                if values:
                    medians[mode] = statistics.median(values)
            if len(medians) == 2:
                print(f"  {scenario:9s} {field:37s} pack={medians['pack']:,.3f}  "
                      f"block={medians['block']:,.3f}")
    print("These are measurements, not an automatic backend-selection decision.")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--model-binary", type=pathlib.Path, required=True)
    parser.add_argument("--hydraulic-binary", type=pathlib.Path)
    parser.add_argument("--scenario", choices=("model", "hydraulic", "both"), default="both")
    parser.add_argument("--pipes", type=int, default=1000)
    parser.add_argument("--edits", type=int, default=6, help="disjoint edits per branch, model scenario")
    parser.add_argument("--scalar-edits", type=int, default=6)
    parser.add_argument("--topology-edits", type=int, default=2)
    parser.add_argument("--repeats", type=int, default=3)
    parser.add_argument("--timeout", type=int, default=3600)
    parser.add_argument("--output", type=pathlib.Path)
    parser.add_argument("--smoke", action="store_true", help="tiny model-only correctness comparison")
    args = parser.parse_args()
    if args.smoke:
        if args.scenario == "both":
            args.scenario = "model"
        args.pipes = 24
        args.edits = 4
        args.scalar_edits = 2
        args.topology_edits = 2
        args.repeats = 1
    if not 2 <= args.pipes <= 1000000 or not 1 <= args.edits <= args.pipes // 2:
        parser.error("pipes must be 2..1000000 and edits must be 1..pipes/2")
    if args.scalar_edits < 1 or args.topology_edits < 1 or args.repeats < 1 or args.timeout < 1:
        parser.error("edit/repetition/timeout values must be positive")
    scenarios = ("model", "hydraulic") if args.scenario == "both" else (args.scenario,)
    if "hydraulic" in scenarios and args.hydraulic_binary is None:
        parser.error("hydraulic scenario requires --hydraulic-binary (Qt-enabled build)")
    for binary in (args.model_binary, args.hydraulic_binary):
        if binary is not None and not binary.is_file():
            parser.error(f"benchmark executable not found: {binary}")
    if args.output is None and not args.smoke:
        parser.error("--output is required for a permanent, interruption-safe CSV")
    if args.output is not None:
        args.output = args.output.resolve()
        if args.output.exists():
            parser.error(f"refusing to overwrite previous benchmark results: {args.output}")
        args.output.parent.mkdir(parents=True, exist_ok=True)
    rows = []
    pairs = {}
    with tempfile.TemporaryDirectory(prefix="aowis-backend-compare-") as temp:
        root = pathlib.Path(temp)
        output = args.output if args.output is not None else root / "smoke.csv"
        with output.open("x", newline="", encoding="utf-8") as file:
            writer = csv.DictWriter(file, fieldnames=COLUMNS)
            writer.writeheader()
            file.flush()
            for scenario in scenarios:
                for repeat in range(1, args.repeats + 1):
                    # Reverse order every other repetition to reduce order bias.
                    order = ("pack", "block") if repeat % 2 else ("block", "pack")
                    key = (scenario, repeat)
                    pairs[key] = []
                    for mode in order:
                        print(f"Running {scenario} {mode} repetition {repeat}/{args.repeats}",
                              file=sys.stderr, flush=True)
                        row = run_case(args, scenario, mode, repeat, root)
                        pairs[key].append(row)
                        writer.writerow(row)
                        file.flush()
                        rows.append(row)
                    compare_pairs({key: pairs[key]})
    summarize(rows)
    if args.output is not None:
        print(f"Results: {args.output}")


if __name__ == "__main__":
    try:
        main()
    except (ValueError, RuntimeError, subprocess.TimeoutExpired) as failure:
        sys.exit(f"Benchmark failed: {failure}")
