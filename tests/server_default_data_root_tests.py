#!/usr/bin/env python3
"""Start the real DVMDB HTTP server and check data-root precedence and safety."""

import argparse
import os
from pathlib import Path
import socket
import subprocess
import tempfile
import time


def require(condition: bool, message: str) -> None:
    if not condition:
        raise AssertionError(message)


def unused_local_port() -> int:
    with socket.socket(socket.AF_INET, socket.SOCK_STREAM) as listener:
        listener.bind(("127.0.0.1", 0))
        return int(listener.getsockname()[1])


def run_server(server: Path, cwd: Path, environment: dict[str, str],
               expected_root: Path, arguments: list[str] | None = None) -> None:
    port = unused_local_port()
    command = [str(server), "--bind", "127.0.0.1", "--port", str(port),
               "--worker-threads", "1"] + (arguments or [])
    process = subprocess.Popen(command, cwd=str(cwd), env=environment,
                               stdout=subprocess.PIPE, stderr=subprocess.PIPE)
    try:
        limit = time.monotonic() + 10.0
        while time.monotonic() < limit:
            if process.poll() is not None:
                output, errors = process.communicate(timeout=2)
                raise AssertionError(
                    f"server exited during startup ({process.returncode}): "
                    f"{output.decode(errors='replace')} {errors.decode(errors='replace')}")
            if (expected_root / "catalogue.lock").exists() and (expected_root / "projects").is_dir():
                try:
                    with socket.create_connection(("127.0.0.1", port), timeout=0.2):
                        return
                except OSError:
                    pass
            time.sleep(0.05)
        raise AssertionError(f"server did not start with expected storage root {expected_root}")
    finally:
        if process.poll() is None:
            process.terminate()
        try:
            output, errors = process.communicate(timeout=6)
        except subprocess.TimeoutExpired:
            process.kill()
            output, errors = process.communicate(timeout=3)
            raise AssertionError("server did not shut down after SIGTERM")
        require(process.returncode == 0,
                f"server did not shut down cleanly ({process.returncode}): "
                f"{output.decode(errors='replace')} {errors.decode(errors='replace')}")


def run_test(server: Path) -> None:
    require(server.is_file() and os.access(server, os.X_OK), "missing server executable")
    with tempfile.TemporaryDirectory(prefix="aowis-dvmdb-default-root-") as directory:
        base = Path(directory)
        cwd = base / "working-directory"
        cwd.mkdir()
        xdg = base / "xdg-data-home"
        environment = dict(os.environ)
        environment["XDG_DATA_HOME"] = str(xdg)
        environment["HOME"] = str(base / "home")
        environment.pop("QT_STANDARDPATHS_TEST_MODE", None)
        environment.pop("XDG_CONFIG_HOME", None)
        default_root = xdg / "aowis-dvmdb"

        # No INI and no CLI option: Qt's user-data directory, never cwd.
        run_server(server, cwd, environment, default_root)
        require(not (cwd / "aowis-dvmdb-data").exists(),
                "default server unexpectedly created a working-directory data root")
        run_server(server, cwd, environment, default_root)

        # Refuse to quietly use a *different* empty database when a previous
        # default is present in the current working directory.
        legacy_root = cwd / "aowis-dvmdb-data"
        legacy_root.mkdir()
        marker = legacy_root / "do-not-overwrite"
        marker.write_text("persisted-test-data", encoding="utf-8")
        result = subprocess.run(
            [str(server), "--bind", "127.0.0.1", "--port", str(unused_local_port())],
            cwd=str(cwd), env=environment, capture_output=True, text=True, timeout=8)
        require(result.returncode == 2 and "Existing ./aowis-dvmdb-data" in result.stderr,
                f"legacy data root did not trigger migration guard: "
                f"exit={result.returncode} stderr={result.stderr!r}")
        require(marker.read_text(encoding="utf-8") == "persisted-test-data",
                "legacy data was changed by default startup")

        # CLI and INI overrides must bypass the new default and may explicitly
        # point at an existing legacy database root.
        run_server(server, cwd, environment, legacy_root,
                   ["--data-root", str(legacy_root)])
        ini_root = base / "configured-root"
        ini_path = cwd / "dvmdb-test.ini"
        ini_path.write_text("[storage]\nroot=" + str(ini_root) + "\n", encoding="utf-8")
        run_server(server, cwd, environment, ini_root,
                   ["--config", str(ini_path)])
        override_root = base / "cli-override"
        run_server(server, cwd, environment, override_root,
                   ["--config", str(ini_path), "--data-root", str(override_root)])
        require((ini_root / "catalogue.lock").exists(), "INI storage root was ignored")
        require((override_root / "catalogue.lock").exists(), "CLI storage root was ignored")
        require(marker.read_text(encoding="utf-8") == "persisted-test-data",
                "legacy data changed after explicit root tests")

    print("DVMDB per-user default, legacy guard, INI and CLI overrides passed")


if __name__ == "__main__":
    parser = argparse.ArgumentParser()
    parser.add_argument("--server", type=Path, required=True)
    args = parser.parse_args()
    run_test(args.server.resolve())
