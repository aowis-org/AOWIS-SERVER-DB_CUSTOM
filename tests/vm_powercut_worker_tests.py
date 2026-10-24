#!/usr/bin/env python3
"""Offline smoke for the production-store worker; no QEMU or privileges required."""
import argparse
import os
from pathlib import Path
import re
import subprocess
import tempfile


def launch(binary, action, repository, stdin=None):
    return subprocess.run([str(binary), action, str(repository)], input=stdin,
                          text=True, capture_output=True, timeout=45)


def run(binary):
    with tempfile.TemporaryDirectory(prefix="aowis-vm-powercut-worker-") as location:
        repo = Path(location) / "repository"
        assert launch(binary, "verify", repo).returncode != 0, "missing repository accepted"
        initialized = launch(binary, "init", repo)
        assert initialized.returncode == 0, initialized.stderr
        assert re.search(r"^STATE 0 [0-9a-f]{64}$", initialized.stdout, re.MULTILINE)
        first = launch(binary, "writer", repo, stdin="")
        assert first.returncode == 0, first.stderr
        assert re.search(r"^ACK 1 [0-9a-f]{64}$", first.stdout, re.MULTILINE)
        second = launch(binary, "writer", repo, stdin="GO\n")
        assert second.returncode == 0, second.stderr
        assert re.search(r"^ACK 2 [0-9a-f]{64}$", second.stdout, re.MULTILINE)
        assert re.search(r"^ACK 3 [0-9a-f]{64}$", second.stdout, re.MULTILINE)
        verified = launch(binary, "verify", repo)
        assert verified.returncode == 0, verified.stderr
        assert re.search(r"^STATE 3 [0-9a-f]{64}$", verified.stdout, re.MULTILINE)
        assert launch(binary, "unknown", repo).returncode != 0
        print("QEMU durability worker smoke passed: 3 durable commits, reopen, full verify")


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--binary", type=Path, required=True)
    args = parser.parse_args()
    if not args.binary.is_file() or not os.access(args.binary, os.X_OK):
        parser.error("compiled worker missing or not executable")
    run(args.binary)


if __name__ == "__main__":
    main()
