#!/usr/bin/env python3
"""Offline Docker-wrapper and Debian image checksum tests; never starts a VM."""
import argparse
import hashlib
import importlib.util
import os
from pathlib import Path
import subprocess
import sys
import tempfile
from unittest.mock import patch


def check(condition, message):
    if not condition:
        raise AssertionError(message)


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--wrapper", required=True, type=Path)
    parser.add_argument("--helper", required=True, type=Path)
    args = parser.parse_args()
    module_spec = importlib.util.spec_from_file_location("aowis_vm_powercut_docker", args.helper)
    helper = importlib.util.module_from_spec(module_spec)
    module_spec.loader.exec_module(helper)

    digest = "a" * 128
    check(helper.checksum_for(digest + "  ./" + helper.IMAGE_NAME + "\n") == digest,
          "SHA-512 manifest parser rejected valid checksum")
    for bad in ("", "abc " + helper.IMAGE_NAME, digest + "  wrong.qcow2"):
        try:
            helper.checksum_for(bad)
        except RuntimeError:
            continue
        raise AssertionError("accepted invalid checksum manifest")

    with tempfile.TemporaryDirectory(prefix="aowis-vm-docker-tests-") as root:
        root_path = Path(root)
        cache = root_path / "cache"
        cache.mkdir()
        image = cache / helper.IMAGE_NAME
        image.write_bytes(b"fake-image-for-offline-checksum-test")
        (cache / (helper.IMAGE_NAME + ".sha512")).write_text(
            hashlib.sha512(image.read_bytes()).hexdigest() + "\n")
        check(helper.download_guest(cache) == image, "verified cached image rejected")
        image.write_bytes(b"corrupted")
        try:
            helper.download_guest(cache)
        except RuntimeError as error:
            check("checksum mismatch" in str(error), "wrong cache error")
        else:
            raise AssertionError("corrupt guest image silently accepted")

        commands = []
        def pretend_build(command, **_kwargs):
            commands.append([str(part) for part in command])
            if "--build" in command:
                build_dir = cache / "debian12-release-build"
                build_dir.mkdir(parents=True, exist_ok=True)
                (build_dir / "aowis-native-vm-powercut-worker").touch()
        with patch.object(helper, "run", side_effect=pretend_build):
            worker = helper.build_worker(cache, source=root_path)
        check(worker.is_file(), "mock production worker build failed")
        check(any("-DAOWIS_NATIVE_WITH_MODEL_CODEC=OFF" in entry and
                  "-DAOWIS_NATIVE_WITH_HTTP_SERVER=OFF" in entry
                  for entry in commands), "Docker worker build unexpectedly depends on Qt")

        mock = root_path / "fake-docker"
        log = root_path / "docker-args.txt"
        mock.write_text("#!/bin/sh\nprintf 'CALL\\n' >> \"$AOWIS_DOCKER_LOG\"\n"
                        "printf '%s\\n' \"$@\" >> \"$AOWIS_DOCKER_LOG\"\n")
        mock.chmod(0o755)
        env = os.environ.copy()
        env.update({"AOWIS_DOCKER": str(mock), "AOWIS_DOCKER_LOG": str(log),
                    "AOWIS_VM_CACHE": str(cache), "AOWIS_VM_RESULTS": str(root_path / "results")})
        result = subprocess.run([str(args.wrapper.resolve()), "--prepare"], env=env,
                                capture_output=True, text=True, timeout=15)
        check(result.returncode == 0, "Docker prepare failed: " + result.stderr)
        # Both the standalone prepare script and the integrated --prepare mode
        # must be simple Docker-image builds, never VM invocations.
        standalone = args.wrapper.resolve().parent / "tools" / "prepare_vm_powercut_docker.sh"
        if standalone.is_file():
            result_standalone = subprocess.run([str(standalone)], env=env,
                                              capture_output=True, text=True, timeout=15)
            check(result_standalone.returncode == 0, "standalone prepare failed")
            check("run\n" not in log.read_text(), "standalone prepare invoked QEMU")
            log.write_text("")
            result = subprocess.run([str(args.wrapper.resolve()), "--prepare"], env=env,
                                    capture_output=True, text=True, timeout=15)
            check(result.returncode == 0, "integrated prepare failed")
        data = log.read_text()
        check(data.count("CALL\n") == 1 and "build\n" in data and "run\n" not in data,
              "prepare unexpectedly ran the VM")
        log.write_text("")
        result = subprocess.run([str(args.wrapper.resolve()), "--rounds", "2", "--filesystems", "ext4"],
                                env=env, capture_output=True, text=True, timeout=15)
        check(result.returncode == 0, "Docker wrapper failed: " + result.stderr)
        data = log.read_text()
        check(data.count("CALL\n") == 2 and "build\n" in data and "run\n" in data,
              "normal workflow must build the image and run the container")
        check("/src:ro\n" in data and "/cache\n" in data and "/results\n" in data,
              "missing read-only source mount or disposable cache/results mounts")
        check("--network\nhost\n" in data, "default Docker network must use host DNS")
        check("--privileged" not in data and "/dev/sda" not in data,
              "unexpected host disk or privileged mount")
        check(data.count("--network\nhost\n") == 2,
              "build and runner must use identical host networking")
        check("--rounds\n2\n--filesystems\next4\n" in data,
              "qualification parameters were not forwarded")
        env_bridge = dict(env, AOWIS_VM_DOCKER_NETWORK="bridge")
        log.write_text("")
        result = subprocess.run([str(args.wrapper.resolve()), "--prepare"],
                                env=env_bridge, capture_output=True, text=True, timeout=15)
        check(result.returncode == 0 and "--network\nbridge\n" in log.read_text(),
              "explicit Docker bridge network was not forwarded")
        env_bad = dict(env, AOWIS_VM_DOCKER_NETWORK="invalid")
        log.write_text("")
        result = subprocess.run([str(args.wrapper.resolve()), "--prepare"],
                                env=env_bad, capture_output=True, text=True, timeout=15)
        check(result.returncode != 0 and not log.read_text(),
              "invalid network passed to Docker")
        help_result = subprocess.run([str(args.wrapper.resolve()), "--help"], env=env,
                                     capture_output=True, text=True, timeout=15)
        check(help_result.returncode == 0 and "--prepare" in help_result.stdout,
              "wrapper help is broken")
    print("Docker QEMU workflow offline validation passed")


if __name__ == "__main__":
    main()
