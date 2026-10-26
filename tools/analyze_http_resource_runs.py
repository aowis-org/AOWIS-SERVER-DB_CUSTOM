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


def quarter_throughput(phases, duration):
    """Average transfer rates in four equal elapsed-time quarters.

    Progress samples can have nonuniform spacing. Assign each interval's
    completed transfers proportionally to its overlap with a quarter; this
    assumes a uniform rate *within* a logged interval, not across the run.
    """
    if phases is None:
        return None
    require(duration > 0 and phases and phases[0]["start_s"] == 0 and
            phases[-1]["end_s"] == duration,
            "throughput phases do not cover the complete run")
    output = []
    for index in range(4):
        begin = duration * index / 4
        end = duration * (index + 1) / 4
        estimated = 0.0
        for item in phases:
            overlap = max(0.0, min(end, item["end_s"]) -
                          max(begin, item["start_s"]))
            if overlap > 0:
                estimated += ((item["imports"] + item["exports"]) * overlap /
                              (item["end_s"] - item["start_s"]))
        output.append({"quarter": index + 1, "start_s": begin, "end_s": end,
                       "transfers_per_second": round(estimated / (end - begin), 3)})
    return output


def read_run(argument):
    directory = argument if argument.is_dir() else argument.parent
    report_path = directory / "report.json"
    require(argument.is_dir() or argument.name == "report.json",
            f"expected run directory or report.json: {argument}")
    report = json.loads(report_path.read_text(encoding="utf-8"))
    require(report.get("kind") == "aowis-dvmdb-http-resource-qualification" and
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
    # This is a descriptive first/last interval comparison, not a slope fit.
    # Runs without saved checkpoints cannot supply an interval trend.
    phase_change = None
    quarters = quarter_throughput(phases, duration)
    quarter_change = None
    if quarters is not None and quarters[0]["transfers_per_second"] > 0:
        quarter_change = round(100 * (
            quarters[-1]["transfers_per_second"] /
            quarters[0]["transfers_per_second"] - 1), 3)
    if phases is not None and len(phases) >= 2:
        first_rate = phases[0]["transfers_per_second"]
        last_rate = phases[-1]["transfers_per_second"]
        require(first_rate > 0, "first progress interval contains no completed transfers")
        phase_change = round(100 * (last_rate / first_rate - 1), 3)
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
        "first_last_phase_change_percent": phase_change,
        "throughput_quarters": quarters,
        "first_last_quarter_change_percent": quarter_change,
    }


def analyze(arguments):
    runs = [read_run(path) for path in arguments]
    baseline = runs[0]
    same_host_duration = all(
        (run["duration_s"], run["host"]) ==
        (baseline["duration_s"], baseline["host"]) for run in runs[1:])
    same_server = all(run["server_sha256"] == baseline["server_sha256"]
                      for run in runs[1:])
    same_harness = all(run["stress_sha256"] == baseline["stress_sha256"]
                       for run in runs[1:])
    compatible = same_host_duration and same_server and same_harness
    first_to_last = None
    if len(runs) >= 2:
        first_to_last = round(100 * (
            runs[-1]["operations_per_second"] / baseline["operations_per_second"] - 1), 3)
    return {
        "kind": "aowis-dvmdb-http-resource-run-analysis", "version": 1,
        "comparison": {
            "same_duration_host_kernel_and_executables": compatible,
            "same_duration_and_host": same_host_duration,
            "same_server_executable": same_server,
            "same_stress_executable": same_harness,
            "first_to_last_overall_throughput_change_percent": first_to_last,
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
            "Cross-build improvements are associations, not controlled causal attribution",
            "First/last interval change is not a monotonicity or statistical trend test",
            "Quarter rates estimate uniform activity within each checkpoint interval; "
            "they are descriptive, not a performance SLA",
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
                if run["first_last_phase_change_percent"] is not None:
                    print("  First-to-last interval change: " +
                          f"{run['first_last_phase_change_percent']:+.2f}%")
                if run["throughput_quarters"] is not None:
                    rates = ", ".join(
                        f"Q{quarter['quarter']}={quarter['transfers_per_second']:.2f}/s"
                        for quarter in run["throughput_quarters"])
                    print("  Quarter-average throughput: " + rates)
                    if run["first_last_quarter_change_percent"] is not None:
                        print("  First-to-last quarter change: " +
                              f"{run['first_last_quarter_change_percent']:+.2f}%")
        if len(result["runs"]) > 1:
            print("Comparable duration/host/kernel/binaries: " +
                  ("yes" if result["comparison"]["same_duration_host_kernel_and_executables"]
                   else "NO (compare trends descriptively only)"))
            comparison = result["comparison"]
            print("Same duration and host: " +
                  ("yes" if comparison["same_duration_and_host"] else "NO"))
            print("Same server executable: " +
                  ("yes" if comparison["same_server_executable"] else "NO"))
            print("Same stress executable: " +
                  ("yes" if comparison["same_stress_executable"] else "NO"))
            print("First-to-last overall rate change (ordered runs): " +
                  f"{comparison['first_to_last_overall_throughput_change_percent']:+.2f}%")
            print("Throughput spread: " +
                  f"{comparison['throughput_range_percent_of_mean']:.2f}% of mean")
            if not comparison["same_duration_host_kernel_and_executables"]:
                print("  Descriptive comparison only: runs differ in host, duration "
                      "or binaries; external load was not controlled.")
        return 0
    except (AnalysisError, OSError, ValueError, KeyError, TypeError) as error:
        print(f"HTTP resource analysis FAILED: {error}", file=sys.stderr)
        return 1


if __name__ == "__main__":
    sys.exit(main())
