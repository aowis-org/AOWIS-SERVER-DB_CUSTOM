#!/usr/bin/env python3
"""Exercise the resource monitor with a disposable Linux process tree, not HTTP."""
import json
import importlib.util
from pathlib import Path
import subprocess
import sys
import tempfile
from unittest import mock

ROOT = Path(__file__).resolve().parents[1]
MONITOR = ROOT / "tools" / "qualify_http_resources.py"


def require(condition, message):
    if not condition:
        raise AssertionError(message)


def run_test():
    def mock_proc_read(status, statm="0 0 0 0 0 0 0"):
        def read(path, encoding=None):
            if str(path).endswith("/status"):
                return status
            if str(path).endswith("/statm"):
                return statm
            raise AssertionError(f"unexpected procfs read: {path}")
        return read

    specification = importlib.util.spec_from_file_location("aowis_resource_monitor", MONITOR)
    monitor = importlib.util.module_from_spec(specification)
    specification.loader.exec_module(monitor)
    with mock.patch.object(Path, "read_text", mock_proc_read(
            "Name:\tsleep\nState:\tZ (zombie)\nThreads:\t1\n")):
        try:
            monitor.status_fields(12345)
        except FileNotFoundError:
            pass  # Exited processes are expected sampling races.
        else:
            raise AssertionError("zombie process was treated as a valid RSS sample")
    with mock.patch.object(Path, "read_text", mock_proc_read(
            "Name:\tsleep\nState:\tS (sleeping)\nThreads:\t1\n", "101 11 0 0 0 0 0")):
        try:
            monitor.status_fields(12345)
        except monitor.QualificationError:
            pass  # Missing RSS of a live process remains a hard error.
        else:
            raise AssertionError("missing live-process RSS was incorrectly ignored")

    # Just before becoming a zombie a task can still report running, but
    # its mm is already absent. Such a sample must be skipped, not counted.
    transitional = "Name:\tsleep\nState:\tR (running)\nThreads:\t1\n"
    with mock.patch.object(Path, "read_text", mock_proc_read(transitional)):
        try:
            monitor.status_fields(12345)
        except FileNotFoundError:
            pass
        else:
            raise AssertionError("exiting process without mm was treated as live")

    # A live task with mapped pages and missing VmRSS remains a hard error.
    with mock.patch.object(Path, "read_text", mock_proc_read(
            transitional, "101 11 0 0 0 0 0")):
        try:
            monitor.status_fields(12345)
        except monitor.QualificationError:
            pass
        else:
            raise AssertionError("live task with missing RSS was ignored")

    # A process may exit after the valid status sample but before /proc/fd.
    with mock.patch.object(monitor, "status_fields", return_value={
            "VmRSS": 100, "VmHWM": 100, "Threads": 1}):
        with mock.patch.object(Path, "iterdir", side_effect=FileNotFoundError):
            try:
                monitor.process_metrics(12345)
            except FileNotFoundError:
                pass
            else:
                raise AssertionError("disappearing fd directory was not skipped")

    with tempfile.TemporaryDirectory(prefix="aowis-http-resource-monitor-") as work:
        path = Path(work)
        fake = path / "stress"
        fake.write_text(
            "#!/usr/bin/env python3\n"
            "import subprocess, sys, time\n"
            "server = subprocess.Popen([sys.argv[3], '6'])\n"
            "try:\n"
            "    time.sleep(5.15)\n"
            "finally:\n"
            "    server.terminate()\n"
            "    server.wait()\n"
            "print('Sustained HTTP soak passed: duration_s=5 imports=7 exports=7 '\n"
            "      'admission_503=2 worst_health_ms=11 projects=3', flush=True)\n",
            encoding="utf-8",
        )
        fake.chmod(0o755)
        output = path / "results"
        arguments = [sys.executable, str(MONITOR), "--seconds", "5", "--interval-ms", "100",
                     "--output-root", str(output), "--stress-binary", str(fake),
                     "--server-binary", "/usr/bin/sleep"]
        result = subprocess.run(arguments, text=True, capture_output=True, timeout=15)
        require(result.returncode == 0, f"successful process tree rejected: {result.stderr}")
        runs = list(output.glob("http-resources-*/report.json"))
        require(len(runs) == 1, "expected one complete resource report")
        report = json.loads(runs[0].read_text(encoding="utf-8"))
        require(report["kind"] == "aowis-dvmdb-http-resource-qualification", "report kind")
        require(report["soak"]["imports"] == 7 and report["soak"]["admission_503"] == 2,
                "soak summary wasn't parsed")
        require(report["sampling"]["server"]["samples"] >= 10, "missing server samples")
        require(report["sampling"]["client_harness"]["samples"] >= 10, "missing harness samples")
        require(report["sampling"]["server"]["peak_fds"] >= 3, "bad descriptor statistics")
        require(report["sampling"]["server"]["peak_rss_kib"] > 0, "bad memory statistics")
        require((runs[0].parent / "soak.log").is_file(), "raw log not preserved")
        require((runs[0].parent / "samples.csv").is_file(), "raw samples not preserved")

        # A successful-looking summary without the monitored server is not evidence.
        invalid = path / "fake-no-server"
        invalid.write_text(
            "#!/usr/bin/env python3\n"
            "import time\n"
            "time.sleep(5.3)\n"
            "print('Sustained HTTP soak passed: duration_s=5 imports=7 exports=7 '\n"
            "      'admission_503=0 worst_health_ms=11 projects=3')\n",
            encoding="utf-8")
        invalid.chmod(0o755)
        # Separate output roots make the actual acceptance criterion explicit:
        # a fake success line must never result in a qualifying JSON report.
        invalid_output = path / "no-server-results"
        negative_arguments = [
            sys.executable, str(MONITOR), "--seconds", "5", "--interval-ms", "100",
            "--output-root", str(invalid_output), "--stress-binary", str(invalid),
            "--server-binary", "/usr/bin/sleep",
        ]
        bad = subprocess.run(negative_arguments, text=True, capture_output=True, timeout=25)
        diagnostic = (f"exit={bad.returncode}, stdout={bad.stdout!r}, "
                      f"stderr={bad.stderr!r}")
        require(bad.returncode != 0, f"missing server was accepted: {diagnostic}")
        require("HTTP resource qualification FAILED:" in bad.stderr,
                f"missing server failed without a qualification diagnostic: {diagnostic}")
        require(not list(invalid_output.glob("http-resources-*/report.json")),
                f"missing server produced a qualification report: {diagnostic}")
        invalid_seconds = subprocess.run([sys.executable, str(MONITOR), "--seconds", "0"],
                                         capture_output=True, text=True)
        require(invalid_seconds.returncode != 0 and "--seconds" in invalid_seconds.stderr,
                "invalid duration was accepted")
    print("Linux HTTP resource qualification monitor: passed")


if __name__ == "__main__":
    run_test()
