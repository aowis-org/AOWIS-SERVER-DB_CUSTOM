#!/usr/bin/env python3
"""Measure native replication imports in isolated processes with live scratch-disk sampling."""

import argparse
import csv
import contextlib
import json
import hashlib
import os
import resource
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


def invoke(command, scratch_dir, progress=False):
    env = os.environ.copy()
    env["TMPDIR"] = str(scratch_dir)
    if progress:
        env["AOWIS_BENCH_PROGRESS"] = "1"
    start = time.monotonic()
    process = subprocess.Popen(command, stdout=subprocess.PIPE,
                               stderr=None if progress else subprocess.PIPE,
                               text=True, env=env)
    max_logical = 0
    max_allocated = 0
    try:
        while process.poll() is None:
            logical, allocated = scratch_bytes(process.pid)
            max_logical = max(max_logical, logical)
            max_allocated = max(max_allocated, allocated)
            time.sleep(0.01)
        output, error = process.communicate()
    finally:
        if process.poll() is None:
            process.terminate()
            try:
                process.wait(timeout=3)
            except subprocess.TimeoutExpired:
                process.kill()
                process.wait()
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
    "profile_total_ms", "profile_scan_ms", "profile_graph_ms",
    "profile_model_references_ms", "profile_reachability_ms", "profile_branch_ms",
    "profile_write_ms", "profile_publication_ms", "profile_bundle_reads",
    "profile_bundle_read_bytes", "profile_local_reads", "profile_local_read_bytes",
    "import_peak_rss_kib", "process_peak_rss_kib", "observed_peak_scratch_logical_bytes",
    "observed_peak_scratch_allocated_bytes", "storage_pack_bytes", "head",
)


def bundle_checksum(path):
    digest = hashlib.sha256()
    with path.open("rb") as stream:
        while True:
            data = stream.read(1024 * 1024)
            if not data:
                break
            digest.update(data)
    return digest.hexdigest()


def fixture_name(kind, backend):
    return f"fixture-{kind}-{backend}"


def prepare_fixtures(binary, root, args, kinds, backends):
    root.mkdir(parents=True, exist_ok=True)
    scratch = root / "scratch"
    scratch.mkdir(exist_ok=True)
    manifest = {"format_version": 1, "objects": args.objects, "commits": args.commits,
                "payload_bytes": args.payload_bytes, "kinds": kinds,
                "backends": backends, "fixtures": {}}
    for kind in kinds:
        reference_digests = None
        for source_backend in backends:
            name = fixture_name(kind, source_backend)
            fixture = root / name
            metrics = invoke([str(binary), "prepare", str(fixture), str(args.objects),
                              str(args.commits), str(args.payload_bytes),
                              source_backend, kind], scratch, args.progress)
            if metrics.get("phase") != "prepare" or metrics.get("source_head") is None:
                raise RuntimeError(f"fixture {name} returned incomplete metrics")
            digests = {filename: bundle_checksum(fixture / f"{filename}.aowbundle")
                       for filename in ("base", "full", "delta")}
            if reference_digests is not None and digests != reference_digests:
                raise RuntimeError("physical backends exported different bundle bytes")
            reference_digests = digests
            manifest["fixtures"][name] = {"head": metrics["source_head"],
                                           "fixture_ms": metrics["fixture_ms"],
                                           "sha256": digests}
            print(f"Prepared {name}: {args.objects:,} records, "
                  f"{float(metrics['fixture_ms']) / 1000:.2f}s, "
                  f"{int(metrics['bundle_full_bytes']) / (1024 * 1024):.1f} MiB bundle",
                  file=sys.stderr, flush=True)
    temporary = root / "manifest.json.tmp"
    temporary.write_text(json.dumps(manifest, indent=2) + "\n", encoding="utf-8")
    temporary.replace(root / "manifest.json")
    return manifest


def load_fixtures(root, args, kinds, backends):
    manifest_path = root / "manifest.json"
    manifest = json.loads(manifest_path.read_text(encoding="utf-8"))
    expected = {"format_version": 1, "objects": args.objects, "commits": args.commits,
                "payload_bytes": args.payload_bytes, "kinds": kinds,
                "backends": backends}
    for field, value in expected.items():
        if manifest.get(field) != value:
            raise RuntimeError(f"fixture manifest mismatch for {field}; use preparation settings")
    for kind in kinds:
        previous_digests = None
        for source_backend in backends:
            name = fixture_name(kind, source_backend)
            fixture = root / name
            item = manifest["fixtures"][name]
            digests = {filename: bundle_checksum(fixture / f"{filename}.aowbundle")
                       for filename in ("base", "full", "delta")}
            if digests != item["sha256"]:
                raise RuntimeError(f"fixture checksum mismatch: {name}")
            if previous_digests is not None and previous_digests != digests:
                raise RuntimeError("physical backends exported different bundle bytes")
            previous_digests = digests
            heads = (fixture / "heads.txt").read_text(encoding="utf-8").splitlines()
            if len(heads) != 2 or heads[1] != item["head"]:
                raise RuntimeError(f"fixture head mismatch: {name}")
    return manifest


def run_imports(binary, fixtures_root, destination_root, args, kinds, backends, stream):
    writer = csv.DictWriter(stream, fieldnames=COLUMNS)
    writer.writeheader()
    scratch = destination_root / "scratch"
    scratch.mkdir(exist_ok=True)
    for kind in kinds:
        for source_backend in backends:
            fixture = fixtures_root / fixture_name(kind, source_backend)
            for destination_backend in backends:
                for iteration in range(1, args.repeats + 1):
                    prefix = f"{kind}-{source_backend}-{destination_backend}-{iteration}"
                    full = destination_root / f"{prefix}-full"
                    base = destination_root / f"{prefix}-base"
                    complete_metrics = None
                    for phase, destination in (("import-full", full),
                                               ("import-base", base),
                                               ("import-delta", base)):
                        metrics = invoke([str(binary), phase, str(fixture),
                                          str(destination), destination_backend],
                                         scratch, args.progress)
                        if metrics.get("phase") != phase or int(metrics["imported_objects"]) <= 0:
                            raise RuntimeError("invalid benchmark import statistics")
                        import_peak = int(metrics["import_peak_rss_kib"])
                        audit_peak = int(metrics["process_peak_rss_kib"])
                        if import_peak <= 0 or audit_peak < import_peak:
                            raise RuntimeError("invalid import RSS high-water marks")
                        if args.smoke and import_peak >= args.parent_peak_kib - 4096:
                            raise RuntimeError("import RSS includes the Python parent's memory peak")
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
                            continue
                        row = {"run": iteration, "kind": kind,
                               "source_backend": source_backend,
                               "destination_backend": destination_backend,
                               "objects": args.objects, "commits": args.commits,
                               "payload_bytes": args.payload_bytes}
                        row.update(metrics)
                        writer.writerow({column: row.get(column, "") for column in COLUMNS})
                        stream.flush()
                        print(f"Imported {prefix} {phase}: {float(metrics['import_ms']):.0f}ms, "
                              f"{import_peak / 1024:.1f} MiB peak RSS",
                              file=sys.stderr, flush=True)
    if args.smoke:
        print("Replication benchmark smoke: all combinations passed", file=sys.stderr)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--binary", required=True, type=Path,
                        help="Path to aowis-native-replication-scaling-benchmark")
    parser.add_argument("--objects", type=int, default=10000)
    parser.add_argument("--commits", type=int, default=20)
    parser.add_argument("--payload-bytes", type=int, default=96)
    parser.add_argument("--repeats", type=int, default=3)
    parser.add_argument("--kinds", choices=("model", "raw", "both"), default="both")
    parser.add_argument("--backends", choices=("pack",), default="pack")
    parser.add_argument("--output", type=Path, help="Write import CSV (default: stdout)")
    parser.add_argument("--work-root", type=Path,
                        help="Parent of fresh, uniquely created benchmark working directories")
    parser.add_argument("--fixture-dir", type=Path,
                        help="Persistent fixtures for separate prepare/import runs")
    parser.add_argument("--mode", choices=("all", "prepare", "imports"), default="all",
                        help="Split expensive fixture preparation from timed imports")
    parser.add_argument("--progress", action="store_true",
                        help="Show per-commit progress during fixture creation")
    parser.add_argument("--smoke", action="store_true", help="Small full/delta correctness test")
    args = parser.parse_args()
    args.parent_peak_kib = 0
    if args.smoke:
        ballast = bytearray(64 * 1024 * 1024)
        for page in range(0, len(ballast), 4096):
            ballast[page] = 1
        del ballast
        args.parent_peak_kib = resource.getrusage(resource.RUSAGE_SELF).ru_maxrss
        args.objects = 24
        args.commits = 4
        args.payload_bytes = 32
        args.repeats = 1
        args.kinds = "both"
        args.backends = "pack"
    if not (2 <= args.commits <= args.objects < 0xFFFFFFFF and
            4 <= args.payload_bytes <= 1024 * 1024 and 1 <= args.repeats <= 100):
        parser.error("require 2 <= commits <= objects < 2^32-1, 4 <= payload <= 1 MiB and 1 <= repeats <= 100")
    if not Path("/proc/self/fd").is_dir():
        parser.error("the scratch-file sampler requires Linux /proc")
    if args.mode != "all" and args.fixture_dir is None:
        parser.error("--mode prepare/imports requires --fixture-dir")
    if args.mode == "all" and args.fixture_dir is not None:
        parser.error("--fixture-dir requires --mode prepare or imports")
    if args.mode == "prepare" and args.output is not None:
        parser.error("--output is for import measurements, not fixture preparation")
    binary = args.binary.resolve(strict=True)
    kinds = ["model", "raw"] if args.kinds == "both" else [args.kinds]
    backends = [args.backends]

    if args.work_root is not None:
        args.work_root.mkdir(parents=True, exist_ok=True)
    with contextlib.ExitStack() as resources:
        if args.mode == "all":
            fixtures_root = Path(resources.enter_context(
                tempfile.TemporaryDirectory(prefix="aowis-db-custom-replication-",
                                            dir=args.work_root)))
            prepare_fixtures(binary, fixtures_root, args, kinds, backends)
        else:
            fixtures_root = args.fixture_dir.resolve()
            if args.mode == "prepare":
                if fixtures_root.exists():
                    raise RuntimeError(f"fixture destination already exists: {fixtures_root}")
                prepare_fixtures(binary, fixtures_root, args, kinds, backends)
                return
            load_fixtures(fixtures_root, args, kinds, backends)
        destination_root = Path(resources.enter_context(
            tempfile.TemporaryDirectory(prefix="aowis-db-custom-replication-imports-",
                                        dir=args.work_root)))
        if args.output is not None:
            args.output.parent.mkdir(parents=True, exist_ok=True)
            stream = resources.enter_context(args.output.open("w", newline="", encoding="utf-8"))
        else:
            stream = sys.stdout
        run_imports(binary, fixtures_root, destination_root, args, kinds, backends, stream)


if __name__ == "__main__":
    try:
        main()
    except (OSError, ValueError, RuntimeError, KeyError, TypeError, json.JSONDecodeError) as error:
        print(f"Replication scaling benchmark failed: {error}", file=sys.stderr)
        sys.exit(1)
