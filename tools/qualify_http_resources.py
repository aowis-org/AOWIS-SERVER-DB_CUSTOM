#!/usr/bin/env python3
"""Opt-in Linux resource sampling around the real sustained HTTP transfer workload.

The stress executable starts/stops the AOWIS HTTP server. This tool observes
its direct server child through /proc; it neither controls production data nor
changes the stress scenario. Results use unique, disposable run directories.
"""
import argparse
import csv
from datetime import datetime, timezone
import hashlib
import json
import os
from pathlib import Path
import re
import signal
import subprocess
import sys
import tempfile
import time

SUCCESS = re.compile(
    r"Sustained HTTP soak passed: duration_s=(\d+) imports=(\d+) "
    r"exports=(\d+) admission_503=(\d+) worst_health_ms=(\d+) projects=(\d+)"
)


class QualificationError(RuntimeError):
    pass


def hash_file(path):
    digest = hashlib.sha256()
    with path.open("rb") as stream:
        for chunk in iter(lambda: stream.read(1024 * 1024), b""):
            digest.update(chunk)
    return digest.hexdigest()


def status_fields(pid):
    # /proc/<pid>/status, /proc/<pid>/statm and /proc/<pid>/fd are not a
    # snapshot. During exec/exit a task can still report R/S while VmRSS is
    # absent. Retry that short transition, then distinguish a disappearing
    # address space from missing metrics on a genuinely live process.
    values = {}
    state = None
    for attempt in range(2):
        values = {}
        state = None
        for line in Path(f"/proc/{pid}/status").read_text(encoding="ascii").splitlines():
            name, delimiter, text = line.partition(":")
            if delimiter and name == "State":
                state = text.strip().split()[0]
            if delimiter and name in ("VmRSS", "VmHWM", "Threads"):
                values[name] = int(text.strip().split()[0])
        if state in ("Z", "X", "x"):
            raise FileNotFoundError(f"PID {pid} exited while sampling")
        if "VmRSS" in values and "Threads" in values:
            return values
        if attempt == 0:
            time.sleep(0.002)

    # A process in the last stages of exit (or an early exec transition) can
    # have no mm even before its state becomes Z. Linux statm reports 0/0 in
    # that case. This is an *absent sample*, never a successful measurement.
    # In particular, a live process with mapped memory and missing fields
    # still fails qualification rather than having its error hidden.
    statm = Path(f"/proc/{pid}/statm").read_text(encoding="ascii").split()
    if len(statm) >= 2 and int(statm[0]) == 0 and int(statm[1]) == 0:
        raise FileNotFoundError(f"PID {pid} has no address space while sampling")
    raise QualificationError(f"missing memory or thread metrics for PID {pid}")


def cpu_seconds(pid):
    # /proc/PID/stat comm is parenthesized and can contain whitespace.
    content = Path(f"/proc/{pid}/stat").read_text(encoding="ascii")
    suffix = content[content.rfind(")") + 2:].split()
    ticks = int(suffix[11]) + int(suffix[12])  # utime/stime (fields 14/15)
    return round(ticks / os.sysconf("SC_CLK_TCK"), 3)


def process_metrics(pid):
    values = status_fields(pid)
    try:
        descriptors = sum(1 for _ in Path(f"/proc/{pid}/fd").iterdir())
    except FileNotFoundError:
        # Process disappeared between reading status and enumerating its fds.
        raise
    except OSError as error:
        raise QualificationError(f"cannot inspect descriptor count of PID {pid}: {error}") from error
    return {
        "rss_kib": values["VmRSS"],
        "high_water_kib": values.get("VmHWM", values["VmRSS"]),
        "threads": values["Threads"],
        "fds": descriptors,
        "cpu_seconds": cpu_seconds(pid),
    }


def children_of(pid):
    path = Path(f"/proc/{pid}/task/{pid}/children")
    try:
        text = path.read_text(encoding="ascii").strip()
        return [int(value) for value in text.split()] if text else []
    except FileNotFoundError:
        # Some container kernels hide the optional task/children file.
        result = []
        for entry in Path("/proc").iterdir():
            if not entry.name.isdecimal():
                continue
            try:
                for line in (entry / "status").read_text(encoding="ascii").splitlines():
                    if line.startswith("PPid:") and int(line.split()[1]) == pid:
                        result.append(int(entry.name))
                        break
            except (FileNotFoundError, PermissionError, ProcessLookupError):
                continue  # A process may have exited during enumeration.
        return result


def executable_of(pid):
    return Path(os.readlink(f"/proc/{pid}/exe")).resolve()


def summary_metrics(rows, role):
    relevant = [row for row in rows if row["role"] == role]
    if not relevant:
        raise QualificationError(f"no process samples captured for {role}")
    first, last = relevant[0], relevant[-1]
    return {
        "samples": len(relevant),
        "initial_rss_kib": first["rss_kib"],
        "final_rss_kib": last["rss_kib"],
        "peak_rss_kib": max(row["rss_kib"] for row in relevant),
        "peak_high_water_kib": max(row["high_water_kib"] for row in relevant),
        "peak_fds": max(row["fds"] for row in relevant),
        "peak_threads": max(row["threads"] for row in relevant),
        "cpu_seconds_delta": round(max(0.0, last["cpu_seconds"] - first["cpu_seconds"]), 3),
    }


def execute(args):
    if sys.platform != "linux" or not Path("/proc/self/status").is_file():
        raise QualificationError("resource qualification requires Linux procfs")
    stress = args.stress_binary.resolve(strict=True)
    server = args.server_binary.resolve(strict=True)
    for binary in (stress, server):
        if not binary.is_file() or not os.access(binary, os.X_OK):
            raise QualificationError(f"not an executable: {binary}")
    root = args.output_root.resolve()
    root.mkdir(parents=True, exist_ok=True)
    run = Path(tempfile.mkdtemp(prefix="http-resources-", dir=root))
    log_path = run / "soak.log"
    rows = []
    server_pid = None
    return_code = None
    start = time.monotonic()
    process = None
    try:
        with log_path.open("x", encoding="utf-8") as log:
            process = subprocess.Popen(
                [str(stress), "--soak-seconds", str(args.seconds), str(server)],
                stdout=log, stderr=subprocess.STDOUT, start_new_session=True)
            while True:
                elapsed = time.monotonic() - start
                if elapsed > args.seconds + 180:
                    raise QualificationError("soak exceeded requested duration plus 180 seconds")
                if process.poll() is not None:
                    return_code = process.returncode
                    break
                try:
                    children = children_of(process.pid)
                except FileNotFoundError:
                    # Child may have exited between poll() and procfs lookup.
                    if process.poll() is not None:
                        return_code = process.returncode
                        break
                    raise
                for child in children:
                    try:
                        candidate = executable_of(child)
                    except FileNotFoundError:
                        continue
                    if candidate == server:
                        if server_pid is None:
                            server_pid = child
                        elif server_pid != child:
                            raise QualificationError("unexpected replacement of the soak server process")
                for role, pid in (("client_harness", process.pid), ("server", server_pid)):
                    if pid is None:
                        continue
                    try:
                        sample = process_metrics(pid)
                    except FileNotFoundError:
                        continue
                    rows.append({"elapsed_ms": int(elapsed * 1000), "role": role, "pid": pid, **sample})
                time.sleep(args.interval_ms / 1000.0)
            process.wait()
        log_content = log_path.read_text(encoding="utf-8", errors="replace")
        match = SUCCESS.findall(log_content)
        if return_code != 0 or len(match) != 1:
            raise QualificationError(f"soak exited {return_code}; check {log_path}")
        seconds, imports, exports, busy, latency, projects = map(int, match[0])
        if seconds != args.seconds or imports < 1 or exports != imports or projects != 3:
            raise QualificationError("soak returned an inconsistent completion summary")
        # Server evidence is mandatory. Check it first, so a fast-exiting
        # harness cannot mask the actual absence of a server process.
        service = summary_metrics(rows, "server")
        client = summary_metrics(rows, "client_harness")
        server_samples = [row for row in rows if row["role"] == "server"]
        observed_seconds = (server_samples[-1]["elapsed_ms"] - server_samples[0]["elapsed_ms"]) / 1000.0
        if observed_seconds < args.seconds * 0.95:
            raise QualificationError("server was not observed for the requested workload duration")
        report = {
            "kind": "aowis-native-http-resource-qualification",
            "version": 1,
            "recorded_utc": datetime.now(timezone.utc).isoformat(),
            "scope": "Linux procfs process samples during sustained real HTTP bundle transfers; not a production resource guarantee",
            "host": {"system": os.uname().sysname, "release": os.uname().release,
                     "machine": os.uname().machine, "cpu_count": os.cpu_count()},
            "executables": {
                "stress": {"path": str(stress), "sha256": hash_file(stress)},
                "server": {"path": str(server), "sha256": hash_file(server)},
            },
            "soak": {"duration_s": seconds, "imports": imports, "exports": exports,
                     "admission_503": busy, "worst_health_ms": latency, "projects": projects},
            "sampling": {"interval_ms": args.interval_ms, "observed_server_s": round(observed_seconds, 3),
                         "server": service, "client_harness": client},
            "log_sha256": hash_file(log_path),
            "limitations": ["RSS includes legitimate growth in branch-history metadata",
                            "CPU samples omit processes other than the monitored HTTP server and harness",
                            "This tool does not impose a fixed memory or latency SLA"],
        }
        if args.max_server_rss_mib is not None:
            report["policy"] = {"max_server_rss_mib": args.max_server_rss_mib,
                                "passed": service["peak_high_water_kib"] <= args.max_server_rss_mib * 1024}
        with (run / "samples.csv").open("x", newline="", encoding="utf-8") as output:
            writer = csv.DictWriter(output, fieldnames=("elapsed_ms", "role", "pid", "rss_kib",
                                                        "high_water_kib", "threads", "fds", "cpu_seconds"))
            writer.writeheader()
            writer.writerows(rows)
        with (run / "report.json").open("x", encoding="utf-8") as output:
            json.dump(report, output, indent=2, sort_keys=True)
            output.write("\n")
        print(f"HTTP resource soak passed: duration_s={seconds} imports={imports} exports={exports} "
              f"peak_server_rss_mib={service['peak_rss_kib'] / 1024:.1f} "
              f"peak_server_fds={service['peak_fds']} "
              f"server_cpu_s={service['cpu_seconds_delta']} report={run / 'report.json'}")
        if "policy" in report and not report["policy"]["passed"]:
            raise QualificationError("operator-selected server RSS limit exceeded")
        return 0
    except Exception as error:
        print(f"HTTP resource qualification FAILED: {error} (evidence: {run})", file=sys.stderr)
        return 1
    finally:
        if process is not None and process.poll() is None:
            # The stress binary and its server child share a private process group.
            os.killpg(process.pid, signal.SIGTERM)
            try:
                process.wait(timeout=4)
            except subprocess.TimeoutExpired:
                os.killpg(process.pid, signal.SIGKILL)
                process.wait()


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--seconds", type=int, default=300)
    parser.add_argument("--stress-binary", type=Path,
                        default=Path("build-linux-tests-release/aowis-native-http-transfer-stress-tests"))
    parser.add_argument("--server-binary", type=Path,
                        default=Path("build-linux-tests-release/aowis-server-dvmdb"))
    parser.add_argument("--output-root", type=Path, default=Path("build-qualification"))
    parser.add_argument("--interval-ms", type=int, default=500)
    parser.add_argument("--max-server-rss-mib", type=int)
    args = parser.parse_args(argv)
    if not 5 <= args.seconds <= 86400:
        parser.error("--seconds must be between 5 and 86400")
    if not 100 <= args.interval_ms <= 60000:
        parser.error("--interval-ms must be between 100 and 60000")
    if args.max_server_rss_mib is not None and args.max_server_rss_mib <= 0:
        parser.error("--max-server-rss-mib must be positive")
    try:
        return execute(args)
    except (OSError, QualificationError) as error:
        print(f"HTTP resource qualification FAILED: {error}", file=sys.stderr)
        return 1


if __name__ == "__main__":
    sys.exit(main())
