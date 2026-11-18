#!/usr/bin/env python3
"""Execute three sealed, sequential hot profiles; never migration authority."""
import hashlib
import json
import os
from pathlib import Path
import subprocess
import time

ROOT = Path(__file__).resolve().parents[1]
EXE = ROOT / "bin/tests/FireSequenceTest"
EXE_SHA = "02b705b284888c92a512e060b8fb04b74c09dd353284ae8abf5326a12fd22d9d"
SOURCE_COMMIT = "8a608832d6cc3058f9121d58912850422e13ea51"
CHECKPOINT = Path("/Users/aravind/Working/RISE/worktrees/r203-producers/rendered/"
                  "fire_production_calibration/r208_single_source/ported_from_zero.v1/"
                  "checkpoints/step_0000001300.checkpoint")
CHECKPOINT_SHA = "2422002e0d45746989b1fa3676f1f4027c785e91bec6b99c3d88b9b01ef12bb2"
OUT = ROOT / "rendered/fire_production_calibration/r215_target_basis"


def sha(path):
    digest = hashlib.sha256()
    with path.open("rb") as stream:
        for block in iter(lambda: stream.read(1024*1024), b""):
            digest.update(block)
    return digest.hexdigest()


def main():
    receipt = OUT / "hot_profiles.execution.v1.json"
    if receipt.exists():
        raise ValueError("immutable execution receipt already exists")
    qualification = OUT / "qualification.contended.v1.json"
    qualified = json.loads(qualification.read_text())
    if (qualified["source_commit"] != SOURCE_COMMIT or
            qualified["producer_executable_sha256"] != EXE_SHA or
            any(run["exit_code"] != 0 for run in qualified["runs"].values())):
        raise ValueError("unqualified executable")
    records = []
    environment = dict(os.environ)
    environment["RISE_FIRE_OWNER_PROFILE"] = "1"
    environment["RISE_FIRE_PRODUCER_KERNEL_PROFILE"] = "1"
    for index in range(2, 5):
        directory = OUT / ("hot_profile_"+str(index)+".v1")
        log = Path(str(directory)+".log")
        if directory.exists() or log.exists():
            raise ValueError("profile output already exists")
        if sha(EXE) != EXE_SHA or sha(CHECKPOINT) != CHECKPOINT_SHA:
            raise ValueError("executed input changed")
        command = [str(EXE), "--fire-production-owner-eos-refusal",
                   str(CHECKPOINT), str(directory)]
        start = time.time_ns()
        with log.open("xb") as stream:
            result = subprocess.run(command, cwd=ROOT, env=environment,
                                    stdout=stream, stderr=subprocess.STDOUT)
        end = time.time_ns()
        # This historical diagnostic returns 93 when its eight steps succeed:
        # its original success condition was reproduction of an EOS refusal.
        # Require the positive terminal result separately, never exit-code alone.
        terminal = [line for line in log.read_text().splitlines()
                    if line.startswith("OWNER_EOS_DIAGNOSTIC_END ")]
        expected = ["OWNER_EOS_DIAGNOSTIC_END solver_accepted=1 "
                    "checkpoint_unchanged=1 migration_authority=false error="]
        if result.returncode != 93 or terminal != expected:
            raise ValueError("hot execution incomplete or refused")
        if sha(EXE) != EXE_SHA or sha(CHECKPOINT) != CHECKPOINT_SHA:
            raise ValueError("executed input changed during run")
        trajectory = directory / "budgets/maximum_velocity_trajectory.csv"
        records.append(dict(command=command, process_exit_code=result.returncode,
                            solver_accepted=True, start_unix_ns=start, end_unix_ns=end,
                            log=str(log.relative_to(ROOT)), log_sha256=sha(log),
                            trajectory=str(trajectory.relative_to(ROOT)),
                            trajectory_sha256=sha(trajectory),
                            executable_before_after_sha256=EXE_SHA,
                            checkpoint_before_after_sha256=CHECKPOINT_SHA))
        print("completed independent profile", index, flush=True)
    evidence = dict(schema="rise.fire.r215.executed-hot-profiles.v1",
                    source_commit=SOURCE_COMMIT, executable_sha256=EXE_SHA,
                    qualification_sha256=sha(qualification),
                    checkpoint_sha256=CHECKPOINT_SHA, runs=records,
                    producer_kernel_profile="1", owner_profile="1",
                    migration_authority=False,
                    scope="Three sequential independent eight-step hot processes. "
                          "Numerical admission and performance report are separate gates.")
    with receipt.open("x") as stream:
        json.dump(evidence, stream, sort_keys=True, indent=2, allow_nan=False)
        stream.write("\n")
    print("execution_receipt_sha256", sha(receipt), flush=True)


if __name__ == "__main__":
    main()
