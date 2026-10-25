#!/usr/bin/env python3
"""Validate and compare saved Linux HTTP resource qualification runs.

No workload is executed. This tool reads the original report.json, samples.csv
and soak.log in each run directory. Early/late RSS comparisons are descriptive,
not a memory-leak test or a production SLA.
"""
import argparse
import csv
import hashlib
import json
import re
from pathlib import Path
import statistics
import sys


class AnalysisError(ValueError):
    pass


PROGRESS = re.compile(
    r"^Sustained HTTP soak progress: elapsed_s=(\d+) imports=(\d+) "
    r"exports=(\d+) admission_503=(\d+)$", re.MULTILINE)
SUMMARY = re.compile(
    r"^Sustained HTTP soak passed: duration_s=(\d+) imports=(\d+) "
    r"exports=(\d+) admission_503=(\d+) worst_health_ms=(\d+) "
    r"projects=(\d+)$", re.MULTILINE)


def file_hash(path):
    digest = hashlib.sha256()
    with path.open("rb") as source:
        for chunk in iter(lambda: source.read(1024 * 1024), b""):
            digest.update(chunk)
    return digest.hexdigest()


def require(condition, reason):
    if not condition:
        raise AnalysisError(reason)


def progress_phases(log_text, soak):
    """Recover independently logged progress, preserving compatibility with old logs.

    Each phase reports completed import/export transfers during its interval.
    No progress exists in older recordings; overall rate alone cannot locate
    a slowdown within such a run.
    """
    matches = list(PROGRESS.finditer(log_text))
    written = [line for line in log_text.splitlines()
               if line.startswith("Sustained HTTP soak progress:")]
    require(len(matches) == len(written), "malformed soak progress checkpoint")
    if not matches:
        return None
    duration = soak["duration_s"]
    previous = {"elapsed_s": 0, "imports": 0, "exports": 0, "admission_503": 0}
    phases = []
    for match in matches:
        checkpoint = dict(zip(("elapsed_s", "imports", "exports", "admission_503"),
                              map(int, match.groups())))
        require(previous["elapsed_s"] < checkpoint["elapsed_s"] < duration,
                "nonmonotonic or out-of-range soak progress time")
        require(checkpoint["imports"] >= previous["imports"] and
                checkpoint["exports"] >= previous["exports"] and
                checkpoint["admission_503"] >= previous["admission_503"] and
                checkpoint["exports"] <= checkpoint["imports"],
                "nonmonotonic or inconsistent soak progress counters")
        phases.append(phase(previous, checkpoint))
        previous = checkpoint
    completed = {"elapsed_s": duration, "imports": soak["imports"],
                 "exports": soak["exports"], "admission_503": soak["admission_503"]}
    require(completed["imports"] >= previous["imports"] and
            completed["exports"] >= previous["exports"] and
            completed["admission_503"] >= previous["admission_503"],
            "soak completion precedes a progress checkpoint")
    phases.append(phase(previous, completed))
    return phases


def phase(previous, current):
    elapsed = current["elapsed_s"] - previous["elapsed_s"]
    require(elapsed > 0, "invalid progress interval")
    imports = current["imports"] - previous["imports"]
    exports = current["exports"] - previous["exports"]
    return {"start_s": previous["elapsed_s"], "end_s": current["elapsed_s"],
            "imports": imports, "exports": exports,
            "transfers_per_second": round((imports + exports) / elapsed, 3),
            "admission_503": current["admission_503"] - previous["admission_503"]}


def read_run(argument):
    directory = argument if argument.is_dir() else argument.parent
    report_path = directory / "report.json"
    require(argument.is_dir() or argument.name == "report.json",
            f"expected run directory or report.json: {argument}")
    report = json.loads(report_path.read_text(encoding="utf-8"))
    require(report.get("kind") == "aowis-native-http-resource-qualification" and
            report.get("version") == 1, f"unsupported resource report: {report_path}")
    soak = report["soak"]
    duration = soak["duration_s"]
    require(isinstance(duration, int) and duration >= 5 and
            soak["imports"] > 0 and soak["imports"] == soak["exports"] and
            soak["projects"] == 3 and soak["admission_503"] >= 0,
            f"inconsistent soak summary: {report_path}")
    if "policy" in report:
        require(report["policy"]["passed"], f"resource policy failed: {report_path}")
    log = directory / "soak.log"
    require(file_hash(log) == report["log_sha256"],
            f"soak.log checksum differs from report: {directory}")
    log_text = log.read_text(encoding="utf-8")
    final = SUMMARY.findall(log_text)
    require(len(final) == 1, f"missing or duplicate soak completion line: {log}")
    observed = tuple(map(int, final[0]))
    require(observed == (duration, soak["imports"], soak["exports"],
                         soak["admission_503"], soak["worst_health_ms"], soak["projects"]),
            f"soak completion differs from report: {log}")
    phases = progress_phases(log_text, soak)
    csv_path = directory / "samples.csv"
    samples = []
    with csv_path.open(newline="", encoding="utf-8") as source:
        reader = csv.DictReader(source)
        required = {"role", "elapsed_ms", "pid", "rss_kib", "high_water_kib",
                    "fds", "threads", "cpu_seconds"}
        require(required.issubset(set(reader.fieldnames or [])),
                f"missing sample columns: {csv_path}")
        for row in reader:
            if row["role"] != "server":
                continue
            sample = {key: (float(row[key]) if key == "cpu_seconds" else int(row[key]))
                      for key in required if key != "role"}
            require(sample["rss_kib"] > 0 and sample["high_water_kib"] > 0 and
                    sample["fds"] >= 0 and sample["threads"] > 0,
                    f"invalid server sample: {csv_path}")
            samples.append(sample)
    require(len(samples) >= 3, f"not enough server samples: {csv_path}")
    require(len(samples) == report["sampling"]["server"]["samples"],
            f"sample count differs from report: {csv_path}")
    first = samples[0]
    last = samples[-1]
    require(all(sample["pid"] == first["pid"] for sample in samples),
            f"server PID changed in recorded samples: {csv_path}")
    require(all(right["elapsed_ms"] > left["elapsed_ms"] and
                right["cpu_seconds"] >= left["cpu_seconds"] for left, right in
                zip(samples, samples[1:])),
            f"nonmonotonic server samples: {csv_path}")
    summary = report["sampling"]["server"]
    for key, observed in (("initial_rss_kib", first["rss_kib"]),
                          ("final_rss_kib", last["rss_kib"]),
                          ("peak_rss_kib", max(row["rss_kib"] for row in samples)),
                          ("peak_high_water_kib", max(row["high_water_kib"] for row in samples)),
                          ("peak_fds", max(row["fds"] for row in samples)),
                          ("peak_threads", max(row["threads"] for row in samples))):
        require(summary[key] == observed, f"{key} differs from samples: {csv_path}")
    observed_seconds = (last["elapsed_ms"] - first["elapsed_ms"]) / 1000.0
    require(abs(observed_seconds - report["sampling"]["observed_server_s"]) <= 0.005 and
            observed_seconds >= duration * 0.95,
            f"server observation duration inconsistent: {csv_path}")
    cpu_delta = last["cpu_seconds"] - first["cpu_seconds"]
    require(abs(cpu_delta - summary["cpu_seconds_delta"]) <= 0.02,
            f"CPU seconds differ from samples: {csv_path}")

    # Central windows avoid interpreting initial process startup or final
    # teardown as persistent RSS. 10–25% and 75–90% are descriptive, and do
    # not establish which objects should remain live as revisions accumulate.
    early = [row["rss_kib"] for row in samples
             if duration * 100 <= row["elapsed_ms"] <= duration * 250]
    late = [row["rss_kib"] for row in samples
            if duration * 750 <= row["elapsed_ms"] <= duration * 900]
    require(early and late, f"insufficient samples in early/late windows: {csv_path}")
    early_median = statistics.median(early)
    late_median = statistics.median(late)
    return {
        "directory": str(directory.resolve()),
        "report_sha256": file_hash(report_path),
        "samples_sha256": file_hash(csv_path),
        "duration_s": duration,
        "imports": soak["imports"],
        "exports": soak["exports"],
        "operations_per_second": round((soak["imports"] + soak["exports"]) / duration, 3),
        "admission_503": soak["admission_503"],
        "worst_health_ms": soak["worst_health_ms"],
        "server_peak_rss_mib": round(summary["peak_rss_kib"] / 1024, 3),
        "server_peak_fds": summary["peak_fds"],
        "server_peak_threads": summary["peak_threads"],
        "server_avg_cpu_cores": round(cpu_delta / observed_seconds, 3),
        "server_first_last_rss_delta_kib": last["rss_kib"] - first["rss_kib"],
        "server_early_median_rss_kib": early_median,
        "server_late_median_rss_kib": late_median,
        "server_early_late_median_rss_delta_kib": late_median - early_median,
        "host": report["host"],
        "server_sha256": report["executables"]["server"]["sha256"],
        "stress_sha256": report["executables"]["stress"]["sha256"],
        "throughput_phases": phases,
    }


def analyze(arguments):
    runs = [read_run(path) for path in arguments]
    baseline = runs[0]
    compatible = all(
        (run["duration_s"], run["host"], run["server_sha256"],
         run["stress_sha256"]) ==
        (baseline["duration_s"], baseline["host"], baseline["server_sha256"],
         baseline["stress_sha256"]) for run in runs[1:])
    return {
        "kind": "aowis-native-http-resource-run-analysis", "version": 1,
        "comparison": {
            "same_duration_host_kernel_and_executables": compatible,
            "throughput_range_percent_of_mean":
                round(100 * (max(run["operations_per_second"] for run in runs) -
                             min(run["operations_per_second"] for run in runs)) /
                      statistics.mean(run["operations_per_second"] for run in runs), 3),
        },
        "runs": runs,
        "limitations": [
            "CPU-core equivalents do not measure host utilization or other workloads",
            "RSS is sampled; retained revision history, caches and allocators can grow normally",
            "Equal kernel and binary hashes do not establish equal disk/cache/background load",
            "Early/late medians describe a run; they are not a memory-leak classification",
            "Within-run throughput checkpoints cannot explain host contention or storage causes",
        ],
    }


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("reports", nargs="+", type=Path,
                        help="saved http-resources-* directory or its report.json")
    parser.add_argument("--output-json", type=Path,
                        help="optional derived comparison evidence, outside source run directories")
    args = parser.parse_args(argv)
    try:
        result = analyze(args.reports)
        if args.output_json is not None:
            output = args.output_json.resolve()
            require(not output.exists(), f"refusing to overwrite: {output}")
            output.write_text(json.dumps(result, indent=2, sort_keys=True) + "\n", encoding="utf-8")
        for run in result["runs"]:
            print(f"{Path(run['directory']).name}: {run['duration_s']} s, "
                  f"{run['operations_per_second']:.2f} transfers/s, "
                  f"peak RSS {run['server_peak_rss_mib']:.2f} MiB, "
                  f"peak FDs {run['server_peak_fds']}, "
                  f"CPU {run['server_avg_cpu_cores']:.2f} cores, "
                  f"early/late median RSS {run['server_early_late_median_rss_delta_kib']:+g} KiB")
            if run["throughput_phases"] is None:
                print("  No within-run throughput checkpoints (older log format).")
            else:
                for phase_result in run["throughput_phases"]:
                    print(f"  {phase_result['start_s']:>5}-{phase_result['end_s']:<5} s: "
                          f"{phase_result['transfers_per_second']:.2f} transfers/s")
        if len(result["runs"]) > 1:
            print("Comparable duration/host/kernel/binaries: " +
                  ("yes" if result["comparison"]["same_duration_host_kernel_and_executables"]
                   else "NO (compare trends descriptively only)"))
            print("Throughput spread: " +
                  f"{result['comparison']['throughput_range_percent_of_mean']:.2f}% of mean")
        return 0
    except (AnalysisError, OSError, ValueError, KeyError, TypeError) as error:
        print(f"HTTP resource analysis FAILED: {error}", file=sys.stderr)
        return 1


if __name__ == "__main__":
    sys.exit(main())
