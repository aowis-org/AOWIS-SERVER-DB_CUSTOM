#!/usr/bin/env python3
"""Offline VM-runner orchestration checks; QEMU, guest, and disks are mocked."""
import importlib.util
import json
from pathlib import Path
import tempfile
from types import SimpleNamespace
from unittest.mock import patch


def load_runner(path):
    module_spec = importlib.util.spec_from_file_location("aowis_powercut_runner", path)
    runner = importlib.util.module_from_spec(module_spec)
    module_spec.loader.exec_module(runner)
    return runner


def check_worker_installed_after_each_boot(runner):
    # The test deliberately makes the OS-installed binary disappear on *every*
    # QEMU reboot. Only the independent repository's simulated data survives.
    # This reproduces the real failure in which the first post-cut verification
    # failed before touching the repository because its helper was missing.
    with tempfile.TemporaryDirectory(prefix="aowis-vm-runner-tests-") as location:
        root = Path(location)
        image = root / "guest.qcow2"
        image.write_bytes(b"mock cloud-image")
        args = SimpleNamespace(guest_image=image, disk_mib=256, rounds=2,
                               cloud_init_seed=None)
        installed = False
        install_count = 0
        boots = 0
        mounted = []
        persisted_sequence = 0
        zero_hash = "0" * 64

        def fake_execute(command, **_kwargs):
            if command[:3] == ["qemu-img", "info", "--output=json"]:
                return json.dumps({"format": "qcow2"})
            if command[:3] == ["qemu-img", "create", "-f"]:
                return ""
            raise AssertionError("unexpected executable in mocked runner: " + repr(command))

        def fake_start(*_args):
            nonlocal installed, boots
            installed = False  # guest OS cache/installation lost across power cut
            boots += 1
            return object()

        def fake_mount(_args, _port, _filesystem, first):
            mounted.append(first)

        def fake_install(_args, _port):
            nonlocal installed, install_count
            assert not installed, "installer unexpectedly reused the preceding boot"
            installed = True
            install_count += 1

        def fake_state(_args, _port, mode):
            nonlocal persisted_sequence
            assert installed, "worker missing after QEMU restart: install it on every boot"
            if mode == "init":
                assert persisted_sequence == 0
                return (0, zero_hash)
            assert mode == "verify", "unexpected worker operation"
            return (persisted_sequence, f"{persisted_sequence:064x}")

        def fake_cut(_args, _port, _qemu, acknowledged, round_index, _case):
            nonlocal persisted_sequence, installed
            assert installed, "tried to write using missing worker"
            assert acknowledged == persisted_sequence
            accepted = acknowledged + 1
            # One extra, unacknowledged commit survives the first power cut;
            # none survives the second. Both outcomes are supported.
            persisted_sequence = accepted + (1 if round_index == 0 else 0)
            installed = False
            return accepted, f"{accepted:064x}"

        with patch.object(runner, "execute", side_effect=fake_execute), \
             patch.object(runner, "unused_port", return_value=22022), \
             patch.object(runner, "start_vm", side_effect=fake_start), \
             patch.object(runner, "stop_vm"), \
             patch.object(runner, "await_ssh"), \
             patch.object(runner, "setup_mount", side_effect=fake_mount), \
             patch.object(runner, "install_worker", side_effect=fake_install), \
             patch.object(runner, "worker_state", side_effect=fake_state), \
             patch.object(runner, "powercut_round", side_effect=fake_cut):
            result = runner.run_fs(args, root, "ext4")

        assert boots == 3 and install_count == 3, (boots, install_count)
        assert mounted == [True, False, False], mounted
        assert result["result"] == "passed"
        assert result["final_sequence"] == 3
        assert len(result["rounds"]) == 2
        assert result["rounds"][0]["acknowledged"] == 1
        assert result["rounds"][0]["recovered_sequence"] == 2
        assert result["rounds"][1]["acknowledged"] == 3
        assert result["rounds"][1]["recovered_sequence"] == 3


def main():
    runner = load_runner(Path(__file__).resolve().parents[1] / "tools" / "qualify_vm_powercut.py")
    check_worker_installed_after_each_boot(runner)
    print("QEMU orchestration smoke passed: helper restaged after every power cut")


if __name__ == "__main__":
    main()
