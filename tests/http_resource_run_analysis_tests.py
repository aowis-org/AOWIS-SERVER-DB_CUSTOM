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


def fixture(base, name, imports, executable="a" * 64, progress=True):
    folder = base / name
    folder.mkdir()
    log = "a preserved workload log\n"
    if progress:
        log += (f"Sustained HTTP soak progress: elapsed_s=60 imports={imports // 2} "
                f"exports={imports // 2} admission_503=0\n")
    log += (f"Sustained HTTP soak passed: duration_s=120 imports={imports} "
            f"exports={imports} admission_503=0 worst_health_ms=4 projects=3\n")
    (folder / "soak.log").write_text(log, encoding="utf-8")
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
        require(entry["first_last_phase_change_percent"] == 0.0,
                "flat synthetic throughput should have no phase drift")
        require(len(entry["throughput_phases"]) == 2,
                "missing completed throughput intervals")
        require(entry["throughput_phases"][0]["transfers_per_second"] == 100.0 and
                entry["throughput_phases"][1]["transfers_per_second"] == 100.0,
                "progress rate calculation incorrect")
        require(sum(item["imports"] for item in entry["throughput_phases"]) == 6000 and
                sum(item["exports"] for item in entry["throughput_phases"]) == 6000,
                "throughput checkpoint counts do not reconcile with summary")
        old = fixture(base, "old-no-progress", 6000, progress=False)
        require(analyzer.read_run(old)["throughput_phases"] is None and
                analyzer.read_run(old)["first_last_phase_change_percent"] is None,
                "legacy report without progress must remain valid")
        require(analysis["comparison"]["same_duration_and_host"] and
                analysis["comparison"]["same_server_executable"] and
                analysis["comparison"]["same_stress_executable"],
                "same-binary repeat comparison classification incorrect")
        require(analysis["comparison"]["first_to_last_overall_throughput_change_percent"] == -0.5,
                "ordered overall throughput percentage incorrect")
        slowed = fixture(base, "slowed-phase", 6000)
        log_file = slowed / "soak.log"
        log_file.write_text(log_file.read_text().replace("imports=3000", "imports=4000")
                            .replace("exports=3000", "exports=4000"))
        slowed_report = json.loads((slowed / "report.json").read_text())
        slowed_report["log_sha256"] = checksum(log_file)
        (slowed / "report.json").write_text(json.dumps(slowed_report))
        require(analyzer.read_run(slowed)["first_last_phase_change_percent"] == -50.0,
                "early/late interval slowdown calculated incorrectly")
        malformed = fixture(base, "malformed-progress", 6000)
        text = (malformed / "soak.log").read_text()
        (malformed / "soak.log").write_text(text.replace("elapsed_s=60", "elapsed_s=120"))
        report = json.loads((malformed / "report.json").read_text())
        report["log_sha256"] = checksum(malformed / "soak.log")
        (malformed / "report.json").write_text(json.dumps(report))
        try:
            analyzer.read_run(malformed)
        except analyzer.AnalysisError:
            pass
        else:
            raise AssertionError("out-of-range progress accepted")
        inconsistent = fixture(base, "inconsistent-progress", 6000)
        text = (inconsistent / "soak.log").read_text()
        (inconsistent / "soak.log").write_text(text.replace("imports=3000", "imports=8000"))
        report = json.loads((inconsistent / "report.json").read_text())
        report["log_sha256"] = checksum(inconsistent / "soak.log")
        (inconsistent / "report.json").write_text(json.dumps(report))
        try:
            analyzer.read_run(inconsistent)
        except analyzer.AnalysisError:
            pass
        else:
            raise AssertionError("progress greater than final count accepted")
        forged = fixture(base, "forged-final", 6000)
        text = (forged / "soak.log").read_text()
        (forged / "soak.log").write_text(text.replace("imports=6000", "imports=7000"))
        report = json.loads((forged / "report.json").read_text())
        report["log_sha256"] = checksum(forged / "soak.log")
        (forged / "report.json").write_text(json.dumps(report))
        try:
            analyzer.read_run(forged)
        except analyzer.AnalysisError:
            pass
        else:
            raise AssertionError("forged completion count accepted")
        different = fixture(base, "changed-binary", 6000, executable="f" * 64)
        cross_build = analyzer.analyze([first, different])["comparison"]
        require(not cross_build["same_duration_host_kernel_and_executables"] and
                cross_build["same_duration_and_host"] and
                not cross_build["same_server_executable"] and
                cross_build["same_stress_executable"],
                "different server binary must be labeled as cross-build comparison")
        other_harness = fixture(base, "different-harness", 6000)
        other_report_path = other_harness / "report.json"
        other_report = json.loads(other_report_path.read_text())
        other_report["executables"]["stress"]["sha256"] = "c" * 64
        other_report_path.write_text(json.dumps(other_report))
        require(not analyzer.analyze([first, other_harness])["comparison"][
            "same_stress_executable"], "different stress binary misidentified")
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
