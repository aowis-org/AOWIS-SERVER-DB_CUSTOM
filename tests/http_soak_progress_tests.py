#!/usr/bin/env python3
"""Exercise the real HTTP soak's elapsed-time progress reporting.

A short run uses one-second checkpoints. Production-length runs retain their
one-minute cadence, without adding requests or modifying the server protocol.
"""
import re
import subprocess
import sys

PROGRESS = re.compile(
    r"^Sustained HTTP soak progress: elapsed_s=(\d+) imports=(\d+) "
    r"exports=(\d+) admission_503=(\d+)$", re.MULTILINE)
FINISHED = re.compile(
    r"^Sustained HTTP soak passed: duration_s=(\d+) imports=(\d+) "
    r"exports=(\d+) admission_503=(\d+) worst_health_ms=(\d+) "
    r"projects=(\d+)$", re.MULTILINE)


def validate(stdout):
    checkpoints = [tuple(map(int, match.groups())) for match in PROGRESS.finditer(stdout)]
    if len(checkpoints) < 2:
        raise AssertionError("real-server soak emitted fewer than two elapsed-time checkpoints")
    previous = (0, 0, 0, 0)
    for checkpoint in checkpoints:
        elapsed, imported, exported, busy = checkpoint
        if not (previous[0] < elapsed < 6 and imported >= previous[1] and
                exported >= previous[2] and busy >= previous[3] and exported <= imported):
            raise AssertionError(f"invalid or nonmonotonic progress checkpoint: {checkpoint}")
        previous = checkpoint
    results = FINISHED.findall(stdout)
    if len(results) != 1:
        raise AssertionError("real-server soak completion summary missing or repeated")
    duration, imported, exported, busy, health, projects = map(int, results[0])
    if not (duration == 6 and imported > 0 and imported == exported and
            projects == 3 and health < 3000 and imported >= previous[1] and
            exported >= previous[2] and busy >= previous[3]):
        raise AssertionError("real-server soak completion inconsistent with progress")


def main():
    if len(sys.argv) != 3:
        raise SystemExit("usage: http_soak_progress_tests.py STRESS_BINARY SERVER_BINARY")
    command = [sys.argv[1], "--soak-seconds", "6", sys.argv[2]]
    try:
        result = subprocess.run(command, capture_output=True, text=True, timeout=90)
    except subprocess.TimeoutExpired as error:
        raise AssertionError("real-server progress smoke timed out") from error
    if result.returncode != 0:
        raise AssertionError(f"real-server soak failed ({result.returncode}):\n"
                             f"stdout:\n{result.stdout}\nstderr:\n{result.stderr}")
    try:
        validate(result.stdout)
    except AssertionError as error:
        raise AssertionError(f"{error}\nstdout:\n{result.stdout}\n"
                             f"stderr:\n{result.stderr}") from error
    print("PASS: real HTTP soak reported consistent elapsed-time checkpoints")


if __name__ == "__main__":
    main()
