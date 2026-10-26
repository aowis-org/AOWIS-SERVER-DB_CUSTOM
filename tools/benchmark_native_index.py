#!/usr/bin/env python3
"""Repeatable Linux physical-index capacity benchmark for the native pack store."""

import argparse
import csv
import json
import os
import signal
import shutil
import subprocess
import sys
import tempfile
import time
from pathlib import Path


def rebuild_selected_release_benchmark(binary):
    """Build the *selected* benchmark target, never a different build tree.

    This avoids silently benchmarking a stale build-linux-tests-release binary
    after somebody rebuilt only build-linux (or vice versa).
    """
    if binary.name != "aowis-dvmdb-index-scaling-benchmark":
        raise RuntimeError(f"unexpected physical-index benchmark executable: {binary.name}")
    build_dir = binary.parent
    cache = build_dir / "CMakeCache.txt"
    if not cache.is_file():
        raise RuntimeError(f"{build_dir} is not a configured CMake build directory; "
                           "configure a Release build before benchmarking")
    values = {}
    for line in cache.read_text(encoding="utf-8").splitlines():
        if line.startswith("//") or line.startswith("#") or "=" not in line:
            continue
        name_type, value = line.split("=", 1)
        if ":" in name_type:
            name, _kind = name_type.split(":", 1)
            values[name] = value
    project = Path(__file__).resolve().parent.parent
    source = values.get("CMAKE_HOME_DIRECTORY", "")
    if not source or Path(source).resolve() != project:
        raise RuntimeError(f"{build_dir} was not configured from this source tree: {source!r}")
    if values.get("CMAKE_BUILD_TYPE") != "Release":
        raise RuntimeError(f"{build_dir} is not configured as Release "
                           f"(found {values.get('CMAKE_BUILD_TYPE')!r}); "
                           "benchmark the Release build directory")
    command = ["cmake", "--build", str(build_dir), "--target", binary.name]
    print(f"Checking selected Release benchmark in {build_dir}", file=sys.stderr, flush=True)
    try:
        subprocess.run(command, check=True, stdout=sys.stderr, stderr=sys.stderr)
    except subprocess.CalledProcessError as error:
        raise RuntimeError(f"could not rebuild the selected benchmark: {error}") from error
    if not binary.is_file() or not os.access(binary, os.X_OK):
        raise RuntimeError(f"benchmark executable missing after build: {binary}")


def parse_metrics(output):
    result = {}
    for line in output.splitlines():
        if "=" not in line:
            raise RuntimeError(f"unexpected benchmark output: {line!r}")
        key, value = line.split("=", 1)
        if not key or key in result:
            raise RuntimeError(f"duplicate/empty metric name: {key!r}")
        result[key] = value
    if "phase" not in result:
        raise RuntimeError("benchmark did not identify its phase")
    return result


def execute(binary, args, expected_phase, target, run_name):
    start = time.monotonic()
    command = [str(binary), *map(str, args)]
    process = subprocess.run(command, text=True, capture_output=True, check=False)
    if process.returncode:
        raise RuntimeError(f"{run_name} failed ({process.returncode}):\n"
                           f"command: {' '.join(command)}\n"
                           f"stdout:\n{process.stdout}\nstderr:\n{process.stderr}")
    metrics = parse_metrics(process.stdout)
    if metrics["phase"] != expected_phase:
        raise RuntimeError(f"expected phase {expected_phase}, got {metrics['phase']}")
    metrics.update({"run": run_name, "target": target,
                    "process_wall_ms": f"{(time.monotonic() - start) * 1000:.3f}"})
    return metrics


def clone(source, destination):
    # Leave room for both the source copy and a later compacted generation.
    source_bytes = sum(entry.stat().st_size for entry in source.rglob("*") if entry.is_file())
    available = shutil.disk_usage(destination.parent).free
    reserve = 256 * 1024 * 1024
    if available < source_bytes * 2 + reserve:
        raise RuntimeError(f"not enough free space to clone {source_bytes:,} bytes "
                           f"safely (available {available:,} bytes)")
    if destination.exists():
        raise RuntimeError(f"copy destination already exists: {destination}")
    shutil.copytree(source, destination, symlinks=True)


def active_index(directory):
    marker = directory / "objects.current"
    if marker.exists():
        parts = marker.read_text(encoding="ascii").strip().split()
        if len(parts) != 2 or parts[0] != "PACK1" or len(parts[1]) != 32 or \
                any(char not in "0123456789abcdef" for char in parts[1]):
            raise RuntimeError("unexpected active pack marker; refusing to remove sidecar")
        return directory / f"objects.{parts[1]}.idx"
    return directory / "objects.idx"


def benchmark_size(binary, root, size, keys, batch, reads, rounds, replicas,
                   skip_rebuild=False, skip_compact=False):
    fixture = root / f"fixture-{size}"
    metrics = []
    print(f"Preparing >= {size:,} physical objects", file=sys.stderr, flush=True)
    prepared = execute(binary, ("prepare", fixture, size, keys, batch), "prepare", size, "prepare")
    metrics.append(prepared)
    actual = int(prepared["physical_objects"])
    if actual < size:
        raise RuntimeError("fixture did not meet physical object target")
    head = prepared["head"]

    probe = execute(binary, ("probe", fixture, keys, reads), "probe", size, "reopen")
    if probe["head"] != head or int(probe["physical_objects"]) != actual:
        raise RuntimeError("reopen changed fixture state")
    metrics.append(probe)

    # A missing sidecar must rebuild from pack bytes. Never remove the source
    # fixture's index: this phase is intentionally destructive only to a copy.
    if not skip_rebuild:
        print(f"Rebuilding derived index for {actual:,} objects", file=sys.stderr, flush=True)
        copy = root / f"rebuild-{size}"
        clone(fixture, copy)
        try:
            sidecar = active_index(copy)
            if not sidecar.is_file():
                raise RuntimeError("expected physical index sidecar is missing")
            sidecar.unlink()
            rebuilt = execute(binary, ("probe", copy, keys, reads), "probe", size, "rebuild")
            if rebuilt["head"] != head or int(rebuilt["physical_objects"]) != actual:
                raise RuntimeError("index rebuild changed logical repository state")
            metrics.append(rebuilt)
        finally:
            shutil.rmtree(copy)

    if not skip_compact:
        print(f"Compacting {actual:,} objects", file=sys.stderr, flush=True)
        copy = root / f"compact-{size}"
        clone(fixture, copy)
        try:
            compact = execute(binary, ("compact", copy), "compact", size, "compact")
            if compact["head"] != head or int(compact["objects_before"]) != actual:
                raise RuntimeError("compaction changed revision identity or object count")
            after = execute(binary, ("probe", copy, keys, reads), "probe", size, "post_compaction_reopen")
            if after["head"] != head:
                raise RuntimeError("compaction damaged published head")
            metrics.extend((compact, after))
        finally:
            shutil.rmtree(copy)

    copy = root / f"write-{size}"
    clone(fixture, copy)
    try:
        write = execute(binary, ("write", copy, keys, batch, rounds), "write", size, "writes")
        if write["head"] == head or int(write["objects_added"]) <= 0:
            raise RuntimeError("write probe failed to advance history")
        metrics.append(write)
    finally:
        shutil.rmtree(copy)

    if replicas >= 2:
        copies = []
        try:
            for index in range(replicas):
                copy = root / f"multi-{size}-{index}"
                clone(fixture, copy)
                copies.append(copy)
            multi = execute(binary, ("multi", *copies), "multi", size, "multi_project_open")
            if int(multi["projects"]) != replicas:
                raise RuntimeError("not all projects were opened")
            metrics.append(multi)
        finally:
            for copy in copies:
                shutil.rmtree(copy)

    shutil.rmtree(fixture)
    return metrics


def write_csv(output, rows):
    columns = ["target", "iteration", "run", "phase", "physical_objects", "pack_bytes",
               "index_bytes", "duration_ms", "process_wall_ms", "open_ms", "rss_before_bytes",
               "rss_after_open_bytes", "rss_after_probe_bytes", "rss_bytes",
               "peak_rss_bytes", "lookup_hit_ms", "lookup_miss_ms", "write_mean_ms",
               "write_p95_ms", "objects_added", "objects_before", "objects_after",
               "pack_bytes_before", "pack_bytes_after", "head", "keys", "batch",
               "revisions", "rounds", "samples", "projects", "target_objects"]
    columns.extend(sorted({name for row in rows for name in row} - set(columns)))
    if hasattr(output, "write"):
        writer = csv.DictWriter(output, fieldnames=columns)
        writer.writeheader()
        writer.writerows(rows)
        return
    path = Path(output)
    path.parent.mkdir(parents=True, exist_ok=True)
    # A failed later run must not damage results from already completed ones.
    with tempfile.NamedTemporaryFile(mode="w", encoding="utf-8", newline="",
                                     prefix=".aowis-index-csv-", dir=path.parent,
                                     delete=False) as stream:
        staged = Path(stream.name)
        try:
            writer = csv.DictWriter(stream, fieldnames=columns)
            writer.writeheader()
            writer.writerows(rows)
            stream.flush()
            os.fsync(stream.fileno())
        except BaseException:
            staged.unlink(missing_ok=True)
            raise
    os.replace(staged, path)


def handle_termination(_signal_number, _frame):
    raise RuntimeError("interrupted; completed-size results retained")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--binary", type=Path, required=True,
                        help="Release-build aowis-dvmdb-index-scaling-benchmark")
    parser.add_argument("--sizes", default="100000,1000000,10000000",
                        help="comma-separated minimum physical object targets")
    parser.add_argument("--keys", type=int, default=4096,
                        help="live logical UUIDs (physical index tracks all past revisions)")
    parser.add_argument("--batch", type=int, default=1024)
    parser.add_argument("--reads", type=int, default=1000)
    parser.add_argument("--write-rounds", type=int, default=5)
    parser.add_argument("--multi-projects", type=int, default=2)
    parser.add_argument("--work-root", type=Path,
                        help="existing volume for randomized disposable fixtures; needs ample disk space")
    parser.add_argument("--output", type=Path, help="CSV file; stdout if omitted")
    parser.add_argument("--skip-rebuild", action="store_true",
                        help="skip costly full-pack index rebuild")
    parser.add_argument("--skip-compact", action="store_true",
                        help="skip costly offline compaction")
    parser.add_argument("--repeats", type=int, default=1,
                        help="independent complete fixture/phase repetitions")
    parser.add_argument("--smoke", action="store_true",
                        help="quick fixture and invariants test instead of capacity run")
    parser.add_argument("--no-build", action="store_true",
                        help="explicitly skip automatic Release target rebuilding "
                             "(only for comparing previously built binaries)")
    args = parser.parse_args()
    signal.signal(signal.SIGTERM, handle_termination)
    if args.smoke:
        args.sizes, args.keys, args.batch, args.reads, args.write_rounds = "500", 96, 24, 25, 2
    sizes = [int(value) for value in args.sizes.split(",")]
    if not sizes or any(value <= 0 or value > 50000000 for value in sizes):
        parser.error("all physical object targets must be between 1 and 50,000,000")
    if not 1 <= args.batch <= args.keys <= 1000000 or args.reads <= 0 or \
            args.write_rounds <= 0 or not 0 <= args.multi_projects <= 8 or not 1 <= args.repeats <= 50:
        parser.error("invalid keys/batch/reads/write-rounds/multi-projects")
    binary = args.binary.expanduser().absolute()
    # CTest builds the smoke-test executable before running it; invoking Ninja
    # recursively from CTest would be unnecessary and can compete with tests.
    if not args.smoke and not args.no_build:
        rebuild_selected_release_benchmark(binary)
    binary = binary.resolve(strict=True)
    if not binary.is_file():
        parser.error("benchmark binary is not a file")
    if args.work_root is not None and not args.work_root.is_dir():
        parser.error("work-root must already be a directory")

    with tempfile.TemporaryDirectory(prefix="aowis-dvmdb-index-bench-",
                                     dir=args.work_root) as temporary:
        root = Path(temporary)
        rows = []
        for size in sizes:
            for iteration in range(1, args.repeats + 1):
                completed = benchmark_size(binary, root, size, args.keys, args.batch,
                                           args.reads, args.write_rounds,
                                           args.multi_projects, args.skip_rebuild,
                                           args.skip_compact)
                for row in completed:
                    row["iteration"] = iteration
                rows.extend(completed)
                # Persist the already completed samples in a valid CSV, even
                # when a later size or repetition fails or gets interrupted.
                if args.output:
                    write_csv(args.output, rows)
        # Every subprocess runs separately; its VmHWM is not polluted by prior phases.
        if not args.output:
            write_csv(sys.stdout, rows)
        print(json.dumps({"targets": sizes, "runs": len(rows),
                          "csv": str(args.output) if args.output else "stdout"}),
              file=sys.stderr)


if __name__ == "__main__":
    try:
        main()
    except (OSError, ValueError, RuntimeError, subprocess.SubprocessError) as error:
        print(f"Physical index benchmark failed: {error}", file=sys.stderr)
        sys.exit(1)
