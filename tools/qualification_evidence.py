#!/usr/bin/env python3
"""Check saved native-database qualification evidence without re-running drills.

This validates report consistency and captures input hashes. It is NOT a
power-loss simulator, proof of hardware durability, or authenticity signature.
"""
import argparse
from datetime import datetime, timezone
import hashlib
import json
from pathlib import Path
import re
import sys

HEX_HASH = re.compile(r"[0-9a-f]{64}\Z")
CTEST_PASS = re.compile(r"100% tests passed out of (\d+)")
SOAK_PASS = re.compile(
    r"Sustained HTTP soak passed: duration_s=(\d+) imports=(\d+) "
    r"exports=(\d+) admission_503=(\d+) worst_health_ms=(\d+) projects=(\d+)"
)


class EvidenceError(ValueError):
    pass


def require(condition, message):
    if not condition:
        raise EvidenceError(message)


def natural_number(value, description, *, minimum=0):
    require(type(value) is int and value >= minimum,
            f"{description} must be an integer >= {minimum}")
    return value


def check_hash(value, description):
    require(isinstance(value, str) and HEX_HASH.fullmatch(value) is not None,
            f"{description} is not a lowercase SHA-256 hex string")
    return value


def check_vm_report(report, minimum_rounds):
    require(isinstance(report, list), "QEMU report must be a list")
    require(len(report) == 2, "QEMU report must cover ext4 and xfs exactly once")
    fs_results = {}
    for entry in report:
        require(isinstance(entry, dict), "QEMU filesystem entry must be an object")
        fs = entry.get("filesystem")
        require(fs in ("ext4", "xfs") and fs not in fs_results,
                "QEMU filesystems must be distinct ext4 and xfs")
        require(entry.get("result") == "passed", f"{fs}: result must be passed")
        rounds = entry.get("rounds")
        require(isinstance(rounds, list) and len(rounds) >= minimum_rounds,
                f"{fs}: insufficient completed power-cut rounds")
        previous = 0
        extra = 0
        for i, round_result in enumerate(rounds):
            require(isinstance(round_result, dict), f"{fs} round {i}: missing object")
            require(round_result.get("round") == i and type(round_result.get("round")) is int,
                    f"{fs} round {i}: nonsequential round number")
            ack = natural_number(round_result.get("acknowledged"),
                                 f"{fs} round {i} acknowledged", minimum=1)
            rec = natural_number(round_result.get("recovered_sequence"),
                                 f"{fs} round {i} recovered", minimum=1)
            head = check_hash(round_result.get("acknowledged_head"),
                              f"{fs} round {i} acknowledged head")
            recovered_head = check_hash(round_result.get("recovered_head"),
                                        f"{fs} round {i} recovered head")
            require(ack == previous + 1,
                    f"{fs} round {i}: acknowledgement does not follow recovered state")
            require(ack <= rec <= ack + 1,
                    f"{fs} round {i}: acknowledged commit lost or multiple surprise commits")
            if rec == ack:
                require(recovered_head == head,
                        f"{fs} round {i}: recovered head differs from acknowledged head")
            else:
                extra += 1
                require(recovered_head != head,
                        f"{fs} round {i}: unacknowledged commit has unchanged head")
            previous = rec
        require(entry.get("final_sequence") == previous and
                type(entry.get("final_sequence")) is int,
                f"{fs}: final sequence does not match last recovered state")
        fs_results[fs] = {"power_cuts": len(rounds), "final_sequence": previous,
                          "surviving_unacknowledged_commits": extra}
    return {"filesystems": fs_results,
            "total_power_cuts": sum(r["power_cuts"] for r in fs_results.values()),
            "scope": "QEMU guest-memory/cache loss; not physical controller power loss"}


def check_ctest_log(content, minimum_tests):
    require("Errors while running CTest" not in content and
            not re.search(r"\d+% tests passed, \d+ tests failed", content),
            "CTest log contains a failure")
    counts = [int(n) for n in CTEST_PASS.findall(content)]
    require(len(counts) == 2,
            "CTest log must have exactly two 100%-passing suite summaries")
    require(all(n >= minimum_tests for n in counts),
            f"CTest suites contain fewer than {minimum_tests} tests")
    debug = content.find("build-linux-tests-debug")
    release = content.find("build-linux-tests-release")
    require(debug >= 0 and release > debug,
            "CTest log must identify Debug followed by Release build directories")
    return {"debug_tests": counts[0], "release_tests": counts[1]}


def check_soak_log(content, minimum_seconds):
    results = SOAK_PASS.findall(content)
    require(len(results) == 1, "HTTP soak log must contain exactly one successful summary")
    duration, imports, exports, busy, latency, projects = map(int, results[0])
    require(duration >= minimum_seconds and imports > 0 and exports > 0 and projects >= 3,
            "HTTP soak is shorter than required or missing activity on three projects")
    return {"duration_seconds": duration, "imports": imports, "exports": exports,
            "admission_503": busy, "worst_health_ms": latency, "projects": projects}


def read_evidence(path):
    data = path.read_bytes()
    return data.decode("utf-8"), hashlib.sha256(data).hexdigest()


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--qemu-report", required=True, type=Path,
                        help="saved build-vm-powercut/run-*/report.json")
    parser.add_argument("--ctest-log", type=Path,
                        help="combined output from ./compile_linux_tests.sh --both")
    parser.add_argument("--soak-log", type=Path,
                        help="stdout from a sustained HTTP soak")
    parser.add_argument("--min-rounds", type=int, default=4)
    parser.add_argument("--min-tests", type=int, default=40)
    parser.add_argument("--min-soak-seconds", type=int, default=300)
    parser.add_argument("--require-all", action="store_true",
                        help="reject absent CTest or HTTP soak evidence")
    parser.add_argument("--output", type=Path,
                        help="write a NEW JSON evidence summary (never overwrite)")
    args = parser.parse_args(argv)
    if min(args.min_rounds, args.min_tests, args.min_soak_seconds) < 1:
        parser.error("minimums must be positive")
    if args.require_all and (args.ctest_log is None or args.soak_log is None):
        parser.error("--require-all needs --ctest-log and --soak-log")
    try:
        evidence = {}
        inputs = {}
        for kind, path in (("qemu", args.qemu_report), ("ctest", args.ctest_log),
                           ("http_soak", args.soak_log)):
            if path is None:
                evidence[kind] = {"status": "not_supplied"}
                continue
            content, digest = read_evidence(path)
            inputs[kind] = {"path": str(path), "sha256": digest}
            if kind == "qemu":
                result = check_vm_report(json.loads(content), args.min_rounds)
            elif kind == "ctest":
                result = check_ctest_log(content, args.min_tests)
            else:
                result = check_soak_log(content, args.min_soak_seconds)
            evidence[kind] = {"status": "validated_report", **result}
        record = {"kind": "aowis-native-qualification-evidence", "version": 1,
                  "recorded_utc": datetime.now(timezone.utc).isoformat(),
                  "scope": "Consistency check of supplied reports; not independent execution or hardware certification",
                  "source_sha256": inputs, "evidence": evidence}
        formatted = json.dumps(record, indent=2, sort_keys=True) + "\n"
        if args.output:
            with args.output.open("x", encoding="utf-8") as file:
                file.write(formatted)
        print(formatted, end="")
    except (ValueError, OSError, UnicodeError, json.JSONDecodeError) as exc:
        print(f"qualification evidence INVALID: {exc}", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
