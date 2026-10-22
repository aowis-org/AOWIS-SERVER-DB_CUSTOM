#!/usr/bin/env python3
"""Measure native replication imports in isolated processes with live scratch-disk sampling."""

import argparse
import csv
import hashlib
import os
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile
import time


def parse_output(output):
    result = {}
    for line in output.splitlines():
        if "=" not in line:
            raise RuntimeError(f"unexpected benchmark output: {line!r}")
        key, value = line.split("=", 1)
        if key in result:
            raise RuntimeError(f"duplicate benchmark result field: {key}")
        result[key] = value
    return result


def scratch_bytes(pid):
    """Sample still-open, often unlinked importer files (not a filesystem directory scan)."""
    descriptors = Path(f"/proc/{pid}/fd")
    logical = 0
    allocated = 0
    seen = set()
    try:
        entries = list(descriptors.iterdir())
    except (FileNotFoundError, ProcessLookupError, PermissionError):
        return (0, 0)
    for entry in entries:
        try:
            target = os.readlink(entry)
            if not any(name in target for name in (
                    "aowis-db-bundle-index-", "aowis-db-bundle-marks-",
                    "aowis-db-tree-summaries-", "aowis-db-reference-edges-")):
                continue
            info = entry.stat()
            identity = (info.st_dev, info.st_ino)
            if identity not in seen:
                seen.add(identity)
                logical += info.st_size
                allocated += info.st_blocks * 512
        except (OSError, FileNotFoundError):
            continue
    return (logical, allocated)


def invoke(command, scratch_dir):
    env = os.environ.copy()
    env["TMPDIR"] = str(scratch_dir)
    start = time.monotonic()
    process = subprocess.Popen(command, stdout=subprocess.PIPE, stderr=subprocess.PIPE,
                               text=True, env=env)
    max_logical = 0
    max_allocated = 0
    while process.poll() is None:
        logical, allocated = scratch_bytes(process.pid)
        max_logical = max(max_logical, logical)
        max_allocated = max(max_allocated, allocated)
        time.sleep(0.01)
    output, error = process.communicate()
    elapsed = (time.monotonic() - start) * 1000
    if process.returncode:
        raise RuntimeError(f"benchmark failed ({process.returncode}): {' '.join(map(str, command))}\n"
                           f"stdout:\n{output}\nstderr:\n{error}")
    result = parse_output(output)
    result["process_wall_ms"] = f"{elapsed:.3f}"
    # Samples are lower bounds: short-lived open files can exist between polls.
    result["observed_peak_scratch_logical_bytes"] = max_logical
    result["observed_peak_scratch_allocated_bytes"] = max_allocated
    if list(scratch_dir.iterdir()):
        raise RuntimeError(f"benchmark leaked named temporary files in {scratch_dir}")
    return result


COLUMNS = (
    "run", "kind", "source_backend", "destination_backend", "objects", "commits",
    "payload_bytes", "phase", "bundle_bytes", "imported_objects", "verified_revisions",
    "verified_reachable_objects", "import_ms", "verify_ms", "process_wall_ms",
    "import_peak_rss_kib", "process_peak_rss_kib", "observed_peak_scratch_logical_bytes",
    "observed_peak_scratch_allocated_bytes", "storage_pack_bytes", "head",
)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--binary", required=True, type=Path,
                        help="Path to aowis-native-replication-scaling-benchmark")
    parser.add_argument("--objects", type=int, default=10000)
    parser.add_argument("--commits", type=int, default=20)
    parser.add_argument("--payload-bytes", type=int, default=96)
    parser.add_argument("--repeats", type=int, default=3)
    parser.add_argument("--kinds", choices=("model", "raw", "both"), default="both")
    parser.add_argument("--backends", choices=("pack", "block", "both"), default="both")
    parser.add_argument("--output", type=Path, help="Write CSV file (default: CSV on stdout)")
    parser.add_argument("--work-root", type=Path,
                        help="Parent of a fresh, uniquely created run directory")
    parser.add_argument("--smoke", action="store_true", help="Small full/delta correctness test")
    args = parser.parse_args()
    if args.smoke:
        args.objects = 24
        args.commits = 4
        args.payload_bytes = 32
        args.repeats = 1
        args.kinds = "both"
        args.backends = "both"
    if not (2 <= args.commits <= args.objects < 0xFFFFFFFF and
            4 <= args.payload_bytes <= 1024 * 1024 and 1 <= args.repeats <= 100):
        parser.error("require 2 <= commits <= objects < 2^32-1, 4 <= payload <= 1 MiB and 1 <= repeats <= 100")
    if not Path("/proc/self/fd").is_dir():
        parser.error("the scratch-file sampler requires Linux /proc")
    binary = args.binary.resolve(strict=True)
    kinds = ["model", "raw"] if args.kinds == "both" else [args.kinds]
    backends = ["pack", "block"] if args.backends == "both" else [args.backends]

    if args.work_root is not None:
        args.work_root.mkdir(parents=True, exist_ok=True)
    if args.output is not None:
        args.output.parent.mkdir(parents=True, exist_ok=True)
        stream = args.output.open("w", newline="", encoding="utf-8")
    else:
        stream = sys.stdout
    writer = csv.DictWriter(stream, fieldnames=COLUMNS)
    writer.writeheader()
    try:
        with tempfile.TemporaryDirectory(prefix="aowis-db-custom-replication-",
                                         dir=args.work_root) as root_name:
            root = Path(root_name)
            for kind in kinds:
                reference_bundle_digests = None
                for source_backend in backends:
                    fixture = root / f"fixture-{kind}-{source_backend}"
                    scratch = root / "scratch"
                    scratch.mkdir(exist_ok=True)
                    created = invoke([str(binary), "prepare", str(fixture), str(args.objects),
                                      str(args.commits), str(args.payload_bytes),
                                      source_backend, kind], scratch)
                    if created.get("phase") != "prepare" or created.get("source_head") is None:
                        raise RuntimeError("fixture preparation returned incomplete metrics")
                    if args.smoke:
                        digests = tuple(hashlib.sha256((fixture / f"{name}.aowbundle").read_bytes()).digest()
                                        for name in ("base", "full", "delta"))
                        if reference_bundle_digests is not None and digests != reference_bundle_digests:
                            raise RuntimeError("physical backends exported different bundle bytes")
                        reference_bundle_digests = digests
                    for destination_backend in backends:
                        for iteration in range(1, args.repeats + 1):
                            prefix = f"{kind}-{source_backend}-{destination_backend}-{iteration}"
                            full = root / f"{prefix}-full"
                            base = root / f"{prefix}-base"
                            complete_metrics = None
                            for phase, destination in (("import-full", full),
                                                       ("import-base", base),
                                                       ("import-delta", base)):
                                metrics = invoke([str(binary), phase, str(fixture),
                                                  str(destination), destination_backend], scratch)
                                if metrics.get("phase") != phase or int(metrics["imported_objects"]) <= 0:
                                    raise RuntimeError("invalid benchmark import statistics")
                                if phase == "import-full":
                                    complete_metrics = metrics
                                elif phase == "import-delta":
                                    if (complete_metrics is None or
                                            metrics["head"] != complete_metrics["head"] or
                                            metrics["verified_reachable_objects"] !=
                                            complete_metrics["verified_reachable_objects"] or
                                            int(metrics["imported_objects"]) >=
                                            int(complete_metrics["imported_objects"])):
                                        raise RuntimeError("incremental import differs from full import")
                                if phase == "import-base":
                                    continue  # Seeding is not an incremental-import measurement.
                                row = {"run": iteration, "kind": kind,
                                       "source_backend": source_backend,
                                       "destination_backend": destination_backend,
                                       "objects": args.objects, "commits": args.commits,
                                       "payload_bytes": args.payload_bytes}
                                row.update(metrics)
                                writer.writerow({column: row.get(column, "") for column in COLUMNS})
                                stream.flush()
                            # Never reuse a prepared destination: filesystem caches and state
                            # from previous imports would invalidate the comparison.
            if args.smoke:
                print("Replication benchmark smoke: all combinations passed", file=sys.stderr)
    finally:
        if stream is not sys.stdout:
            stream.close()


if __name__ == "__main__":
    try:
        main()
    except (OSError, ValueError, RuntimeError) as error:
        print(f"Replication scaling benchmark failed: {error}", file=sys.stderr)
        sys.exit(1)
