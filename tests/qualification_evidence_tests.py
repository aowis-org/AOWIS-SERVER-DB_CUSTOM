#!/usr/bin/env python3
"""Offline evidence-validation regressions; no QEMU/Docker or privileged devices."""
import copy
import importlib.util
import json
from pathlib import Path
import subprocess
import sys
import tempfile


def load_checker(path):
    specification = importlib.util.spec_from_file_location("qualification_evidence", path)
    module = importlib.util.module_from_spec(specification)
    specification.loader.exec_module(module)
    return module


def sample_report():
    entries = []
    for filesystem in ("ext4", "xfs"):
        rounds = []
        previous = 0
        for i in range(4):
            ack = previous + 1
            recovered = ack if i == 0 else ack + 1
            rounds.append({"round": i, "acknowledged": ack,
                           "acknowledged_head": f"{ack:064x}",
                           "recovered_sequence": recovered,
                           "recovered_head": f"{recovered:064x}"})
            previous = recovered
        entries.append({"filesystem": filesystem, "rounds": rounds,
                        "final_sequence": previous, "result": "passed"})
    return entries


def should_reject(checker, report, reason):
    try:
        checker.check_vm_report(report, 4)
    except checker.EvidenceError:
        return
    raise AssertionError("evidence checker accepted " + reason)


def main():
    root = Path(__file__).resolve().parent.parent
    script = root / "tools/qualification_evidence.py"
    checker = load_checker(script)
    base = sample_report()
    result = checker.check_vm_report(base, 4)
    assert result["total_power_cuts"] == 8
    assert result["filesystems"]["ext4"]["final_sequence"] == 7
    assert result["filesystems"]["xfs"]["surviving_unacknowledged_commits"] == 3

    invalid_cases = []
    changed = copy.deepcopy(base); changed[0]["result"] = "failed"
    invalid_cases.append((changed, "failed result"))
    changed = copy.deepcopy(base); changed.pop()
    invalid_cases.append((changed, "missing filesystem"))
    changed = copy.deepcopy(base); changed[1]["filesystem"] = "ext4"
    invalid_cases.append((changed, "duplicate filesystem"))
    changed = copy.deepcopy(base); changed[0]["rounds"].pop()
    invalid_cases.append((changed, "insufficient rounds"))
    changed = copy.deepcopy(base); changed[0]["rounds"][1]["recovered_sequence"] = 1
    invalid_cases.append((changed, "lost acknowledgement"))
    changed = copy.deepcopy(base); changed[0]["rounds"][0]["recovered_head"] = "a" * 64
    invalid_cases.append((changed, "acknowledged head mismatch"))
    changed = copy.deepcopy(base); changed[0]["rounds"][2]["acknowledged"] += 1
    invalid_cases.append((changed, "nonsequential acknowledgement"))
    changed = copy.deepcopy(base); changed[0]["rounds"][1]["round"] = 0
    invalid_cases.append((changed, "repeated round"))
    changed = copy.deepcopy(base); changed[0]["final_sequence"] += 1
    invalid_cases.append((changed, "wrong final head sequence"))
    changed = copy.deepcopy(base); changed[0]["rounds"][0]["acknowledged"] = True
    invalid_cases.append((changed, "boolean counter"))
    for report, reason in invalid_cases:
        should_reject(checker, report, reason)

    ctest = ("Build files have been written to: /tmp/build-linux-tests-debug\n"
             "100% tests passed out of 40\n"
             "Build files have been written to: /tmp/build-linux-tests-release\n"
             "100% tests passed out of 40\n")
    assert checker.check_ctest_log(ctest, 40) == {"debug_tests": 40, "release_tests": 40}
    for bad in (ctest.replace("40", "39", 1), ctest + "Errors while running CTest\n",
                ctest.replace("build-linux-tests-debug", "build-linux-tests-release"),
                ctest[:ctest.index("Build files have been written to: /tmp/build-linux-tests-release")]):
        try:
            checker.check_ctest_log(bad, 40)
        except checker.EvidenceError:
            continue
        raise AssertionError("invalid CTest evidence accepted")
    soak = ("Sustained HTTP soak passed: duration_s=300 imports=7516 exports=7516 "
            "admission_503=0 worst_health_ms=12 projects=3\n")
    assert checker.check_soak_log(soak, 300)["exports"] == 7516
    for bad in (soak.replace("duration_s=300", "duration_s=30"),
                soak.replace("projects=3", "projects=2"),
                soak.replace("imports=7516", "imports=0")):
        try:
            checker.check_soak_log(bad, 300)
        except checker.EvidenceError:
            continue
        raise AssertionError("invalid soak evidence accepted")

    with tempfile.TemporaryDirectory(prefix="aowis-qualification-evidence-") as temporary:
        directory = Path(temporary)
        vm = directory / "qemu.json"
        tests = directory / "ctest.log"
        http = directory / "soak.log"
        report = directory / "evidence.json"
        vm.write_text(json.dumps(base))
        tests.write_text(ctest)
        http.write_text(soak)
        command = [sys.executable, str(script), "--qemu-report", str(vm),
                   "--ctest-log", str(tests), "--soak-log", str(http),
                   "--require-all", "--output", str(report)]
        completed = subprocess.run(command, capture_output=True, text=True)
        assert completed.returncode == 0, completed.stderr
        written = json.loads(report.read_text())
        assert written["evidence"]["qemu"]["total_power_cuts"] == 8
        assert written["evidence"]["ctest"]["debug_tests"] == 40
        assert written["evidence"]["http_soak"]["duration_seconds"] == 300
        assert len(written["source_sha256"]["qemu"]["sha256"]) == 64
        assert subprocess.run(command, capture_output=True).returncode != 0, "overwrote report"
        vm.write_text(json.dumps(base[:-1]))
        assert subprocess.run(command[: -2], capture_output=True).returncode != 0, \
            "accepted missing XFS despite --require-all"
        vm.write_text(json.dumps(base))
        missing = subprocess.run([sys.executable, str(script), "--qemu-report", str(vm)],
                                 capture_output=True, text=True)
        assert missing.returncode == 0, missing.stderr
        assert json.loads(missing.stdout)["evidence"]["ctest"]["status"] == "not_supplied"
        assert subprocess.run([sys.executable, str(script), "--qemu-report", str(vm),
                               "--require-all"], capture_output=True).returncode != 0
    print("AOWIS qualification evidence checks passed")


if __name__ == "__main__":
    main()
