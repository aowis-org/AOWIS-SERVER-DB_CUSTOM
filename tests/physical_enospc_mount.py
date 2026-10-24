#!/usr/bin/env python3
"""Run a real ENOSPC regression on an isolated Linux user-namespace tmpfs.

Never fill the host's /tmp or a pre-existing filesystem. CTest returns SKIP
(77) when unprivileged user/mount namespaces are unavailable.
"""
import argparse
import pathlib
import shutil
import subprocess
import sys
import tempfile

SKIP = 77


def inner(binary: str) -> int:
    with tempfile.TemporaryDirectory(prefix="aowis-enospc-mount-") as location:
        mountpoint = pathlib.Path(location)
        command = ["mount", "-t", "tmpfs", "-o", "size=32m,mode=0700,nosuid,nodev", "tmpfs", str(mountpoint)]
        mounted = subprocess.run(command, capture_output=True, text=True)
        if mounted.returncode:
            print("SKIP: private tmpfs mount unavailable: " + mounted.stderr.strip(), flush=True)
            return SKIP
        try:
            completed = subprocess.run([binary, str(mountpoint)], timeout=90, check=False)
            return completed.returncode
        finally:
            unmounted = subprocess.run(["umount", str(mountpoint)], capture_output=True, text=True)
            if unmounted.returncode:
                print("Cannot unmount private test filesystem: " + unmounted.stderr, file=sys.stderr)


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--binary", required=True)
    parser.add_argument("--inner", action="store_true", help=argparse.SUPPRESS)
    args = parser.parse_args()
    binary = str(pathlib.Path(args.binary).resolve())
    if args.inner:
        return inner(binary)
    if sys.platform != "linux" or not shutil.which("unshare") or not shutil.which("mount"):
        print("SKIP: physical ENOSPC test needs Linux unshare/mount")
        return SKIP
    command = ["unshare", "--user", "--map-root-user", "--mount", "--", sys.executable,
               str(pathlib.Path(__file__).resolve()), "--inner", "--binary", binary]
    completed = subprocess.run(command, capture_output=True, text=True, timeout=110)
    if completed.stdout:
        print(completed.stdout, end="")
    if completed.stderr:
        print(completed.stderr, end="", file=sys.stderr)
    if completed.returncode and "unshare failed:" in completed.stderr and "Operation not permitted" in completed.stderr:
        print("SKIP: unprivileged user/mount namespaces disabled")
        return SKIP
    return completed.returncode


if __name__ == "__main__":
    sys.exit(main())
