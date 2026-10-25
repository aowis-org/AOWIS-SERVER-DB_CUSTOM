#!/usr/bin/env python3
"""Offline tests for historical resource report verification and comparisons."""
import csv
import hashlib
import importlib.util
import json
from pathlib import Path
import tempfile

ROOT = Path(__file__).resolve().parents[1]
SCRIPT = ROOT / "tools" / "analyze_http_resource_runs.py"


def checksum(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def require(condition, message):
    if not condition:
        raise AssertionError(message)


def fixture(base, name, imports, executable="a" * 64):
    folder = base / name
    folder.mkdir()
    (folder / "soak.log").write_text("a preserved workload log\n", encoding="utf-8")
    fieldnames = ["elapsed_ms", "role", "pid", "rss_kib", "high_water_kib",
                  "threads", "fds", "cpu_seconds"]
    server = []
    with (folder / "samples.csv").open("w", encoding="utf-8", newline="") as output:
        writer = csv.DictWriter(output, fieldnames=fieldnames)
        writer.writeheader()
        for index in range(20):
            row = {"elapsed_ms": index * 6000, "role": "server", "pid": 12345,
                   "rss_kib": 28000 + 10 * index,
                   "high_water_kib": 28100 + 10 * index,
                   "threads": 10, "fds": 37,
                   "cpu_seconds": float(index * 18)}
            writer.writerow(row)
            server.append(row)
            writer.writerow({"elapsed_ms": index * 6000, "role": "client_harness",
                             "pid": 12344, "rss_kib": 12000, "high_water_kib": 13000,
                             "threads": 7, "fds": 9, "cpu_seconds": float(index * 2)})
    report = {
        "kind": "aowis-native-http-resource-qualification", "version": 1,
        "log_sha256": checksum(folder / "soak.log"),
        "soak": {"duration_s": 120, "imports": imports, "exports": imports,
                 "projects": 3, "admission_503": 0, "worst_health_ms": 4},
        "host": {"system": "Linux", "release": "test-kernel", "machine": "x86_64",
                 "cpu_count": 8},
        "executables": {"server": {"sha256": executable},
                        "stress": {"sha256": "b" * 64}},
        "sampling": {"observed_server_s": 114.0,
                     "server": {"samples": 20, "initial_rss_kib": 28000,
                                "final_rss_kib": 28190, "peak_rss_kib": 28190,
                                "peak_high_water_kib": 28290, "peak_fds": 37,
                                "peak_threads": 10, "cpu_seconds_delta": 342.0}},
    }
    (folder / "report.json").write_text(json.dumps(report) + "\n", encoding="utf-8")
    return folder


def run_test():
    specification = importlib.util.spec_from_file_location("aowis_http_resource_analysis", SCRIPT)
    analyzer = importlib.util.module_from_spec(specification)
    specification.loader.exec_module(analyzer)
    with tempfile.TemporaryDirectory(prefix="aowis-resource-analysis-") as root:
        base = Path(root)
        first = fixture(base, "first", 6000)
        second = fixture(base, "second", 5970)
        analysis = analyzer.analyze([first, second / "report.json"])
        require(analysis["comparison"]["same_duration_host_kernel_and_executables"],
                "same binaries/duration/host should be comparable")
        require(analysis["comparison"]["throughput_range_percent_of_mean"] > 0,
                "different operation counts should produce a throughput spread")
        entry = analysis["runs"][0]
        require(entry["server_first_last_rss_delta_kib"] == 190,
                "first-to-last memory change incorrect")
        require(entry["server_early_late_median_rss_delta_kib"] == 130,
                "window median drift incorrect")
        require(entry["server_avg_cpu_cores"] == 3.0, "CPU-core equivalents incorrect")
        require(entry["operations_per_second"] == 100.0, "transfer normalization incorrect")
        require(entry["server_peak_fds"] == 37, "descriptor summary incorrect")
        different = fixture(base, "changed-binary", 6000, executable="f" * 64)
        require(not analyzer.analyze([first, different])["comparison"][
            "same_duration_host_kernel_and_executables"],
            "different binary must never be marked fully comparable")
        # A summary that no longer matches the measurements must be rejected.
        bad = fixture(base, "tampered-sample", 6000)
        with (bad / "samples.csv").open("a", encoding="utf-8") as output:
            output.write("119999,server,12345,99,99,10,37,500\n")
        try:
            analyzer.read_run(bad)
        except analyzer.AnalysisError:
            pass
        else:
            raise AssertionError("report/sample mismatch was accepted")
        # Soak logs are retained evidence and protected by the report checksum.
        bad_log = fixture(base, "tampered-log", 6000)
        (bad_log / "soak.log").write_text("rewritten\n", encoding="utf-8")
        try:
            analyzer.read_run(bad_log)
        except analyzer.AnalysisError:
            pass
        else:
            raise AssertionError("incorrect soak-log hash was accepted")
        print("Historical HTTP resource run analysis: passed")


if __name__ == "__main__":
    run_test()
