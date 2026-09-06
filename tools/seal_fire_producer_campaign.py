#!/usr/bin/env python3
"""Seal r203 qualification separately from its exploratory timing evidence."""
import argparse
import hashlib
import json
import math
from pathlib import Path
import struct
import subprocess
import tempfile

from analyze_fire_producer_kernels import fields, summarize
from check_fire_owner_cost_prefix import bind_profile, compare, completion_record, metadata, records
from check_fire_owner_instrumentation import qualify_artifact, trees


def tokens(line):
    if line.split()[:1] in (["OWNER_COST_PREFIX"], ["PRODUCER_COMMAND_V1"], ["PRODUCER_KERNEL_V1"]):
        return fields(line)
    return dict(word.split("=", 1) for word in line.split()[1:] if "=" in word)


def validate_prefix(directory, stem):
    prefix = directory / (stem + "_prefix")
    log = (directory / (stem + "_prefix.log")).read_text()
    outcome_path = prefix / "diagnostic_prefix_outcome.v1"
    outcome = metadata(outcome_path)
    for key, name in (("protocol_sha256", "diagnostic_prefix_protocol.v1"),
                      ("from_zero_identity_sha256", "diagnostic_from_zero_identity.v1"),
                      ("trajectory_sha256", "budgets/maximum_velocity_trajectory.csv"),
                      ("retry_trajectory_sha256", "budgets/retry_attempt_trajectory.csv")):
        if outcome.get(key) != hashlib.sha256((prefix / name).read_bytes()).hexdigest():
            raise ValueError("stale prefix outcome: " + key)
    completion_record(log, records(prefix / "budgets/maximum_velocity_trajectory.csv"), outcome_path)
    profile = trees(log)
    bind_profile(profile, records(prefix / "budgets/maximum_velocity_trajectory.csv"))
    scopes = [row for tree in profile for row in tree if row["phase"] == "BuildStageProducerGroup"]
    commands = [tokens(line) for line in log.splitlines() if line.split()[:1] == ["PRODUCER_COMMAND_V1"]]
    if len(scopes) != len(commands):
        raise ValueError("missing producer command")
    for scope, command in zip(scopes, commands):
        if any(scope[key] != int(command[key]) for key in ("stage", "raw_iteration")):
            raise ValueError("producer command/stage association mismatch")
        duration = float(command["device_ms"])
        # Same decimal-formatting-only bound as bind_profile; not a solver gate.
        if abs(duration - scope["device_sum_ms"]) > 1e-9 + 4 * math.ulp(duration):
            raise ValueError("producer command/time association mismatch")


def validate_witness(text, checkpoint_sha):
    lines = text.splitlines()
    def one(prefix):
        found = [tokens(line) for line in lines if line.startswith(prefix)]
        if len(found) != 1:
            raise ValueError("missing/duplicate witness record: " + prefix)
        return found[0]
    start = one("OWNER_EOS_DIAGNOSTIC ")
    header = one("EOS_REFUSAL_INPUT_V1 ")
    end = one("OWNER_EOS_DIAGNOSTIC_END ")
    if (start.get("checkpoint_sha256") != checkpoint_sha or start.get("accepted_steps") != "44"
            or start.get("migration_authority") != "false"
            or header.get("cell") != "179246" or header.get("stage") != "0"
            or [header.get(key) for key in ("nx", "ny", "nz")] != ["69", "69", "106"]
            or header.get("publication") != "false" or end.get("solver_accepted") != "0"
            or end.get("checkpoint_unchanged") != "1" or end.get("migration_authority") != "false"
            or end.get("eos_failure_cell") != "179246"
            or end.get("eos_failure_term_bitmap") != "3"):
        raise ValueError("wrong checkpoint/cell/refusal association")
    components = [tokens(line) for line in lines if line.startswith("EOS_REFUSAL_COMPONENT ")]
    keys = [(int(row["field"]), int(row["component"])) for row in components]
    if len(keys) != 36 or set(keys) != {(field, component) for field in range(4) for component in range(9)}:
        raise ValueError("incomplete candidate/parent/source witness")
    for row in components:
        value = struct.unpack("<f", struct.pack("<I", int(row["bits"])))[0]
        if not math.isfinite(value) or value != float(row["value"]):
            raise ValueError("witness component bits/value mismatch")
    mirror = one("EOS_REFUSAL_FP64 ")
    expansion = one("EOS_REFUSAL_EXPANSION ")
    if (mirror.get("accepted") != "1" or float(mirror["temperature_K"]) != 300.0
            or float(expansion["temperature"]) != 300.0 or expansion.get("unique") != "0"
            or expansion.get("lower_order") != "1" or expansion.get("upper_order") != "-1"
            or not 0.0 < float(expansion["bound"]) < abs(float(expansion["tail"]))):
        raise ValueError("missing or contradictory rounding witness")


def self_test(directory):
    original = (directory / "eos_refusal_expansion.log").read_text()
    checkpoint_sha = "4a5d1c2f5bac9b23bf149f5e700668314e52658c83e22f15eef597294ca8300a"
    validate_witness(original, checkpoint_sha)
    mutants = ["", original.replace("cell=179246", "cell=179245"),
               "\n".join(line for line in original.splitlines()
                         if not line.startswith("EOS_REFUSAL_COMPONENT field=3 component=8 "))]
    for mutant in mutants:
        try:
            validate_witness(mutant, checkpoint_sha)
        except ValueError:
            continue
        raise AssertionError("incomplete/wrong-cell witness escaped")
    import shutil
    with tempfile.TemporaryDirectory(prefix="rise-producer-association-reds-") as temporary:
        trial = Path(temporary)
        shutil.copytree(directory / "tier8_counter_prefix", trial / "tier8_counter_prefix")
        log_path = trial / "tier8_counter_prefix.log"
        log_path.write_bytes((directory / "tier8_counter_prefix.log").read_bytes())
        validate_prefix(trial, "tier8_counter")
        log_path.write_bytes((directory / "tier8_gather_prefix.log").read_bytes())
        try:
            validate_prefix(trial, "tier8_counter")
        except ValueError:
            pass
        else:
            raise AssertionError("swapped timing log escaped")
        log_path.write_bytes((directory / "tier8_counter_prefix.log").read_bytes())
        outcome = trial / "tier8_counter_prefix/diagnostic_prefix_outcome.v1"
        outcome.write_text(outcome.read_text() + "\n")
        try:
            validate_prefix(trial, "tier8_counter")
        except ValueError:
            pass
        else:
            raise AssertionError("stale outcome escaped")
    print("PRODUCER_SEAL_REDS passed=1 witness=3 association=2")


def seal(root, directory, output, qualification_commit):
    kernel = directory / "kernel_sweep.final.log"
    lines = kernel.read_text().splitlines()
    for prefix in ("RESIDENT_TRANSPORT_METAL_FP64 ", "RESIDENT_PHYSICAL_FLUX_METAL ",
                   "RESIDENT_EOS_R203_REFUSAL_CELL ", "RESIDENT_EOS_ENDPOINT_RESERVE_RED ",
                   "RESIDENT_EOS_LOG_ENCLOSURE ", "RESIDENT_EOS ", "RESIDENT_TARGET ",
                   "SCALAR_FCT_METAL_STAGES ", "SCALAR_FCT_METAL_MIXED ",
                   "COMPATIBLE_MOMENTUM_METAL_FP64 "):
        matches = [line for line in lines if line.startswith(prefix)]
        if len(matches) != 1 or "passed=1" not in matches[0].split():
            raise ValueError("missing/failed final qualification: " + prefix)
    if not any("samples=49512449 " in line and "passed=1" in line
               for line in lines if line.startswith("RESIDENT_EOS_LOG_ENCLOSURE ")):
        raise ValueError("full live case domain not qualified")
    owner_log = directory / "owner_gate.final.log"
    owner_trace = directory / "owner_gate.final"
    owner_csv = directory / "owner_gate.final.csv"
    qualify_artifact(owner_log.read_text(), owner_trace.read_bytes(), owner_csv.read_bytes())
    baseline = directory / "tier8_counter_prefix/budgets/maximum_velocity_trajectory.csv"
    gather = directory / "tier8_gather_prefix/budgets/maximum_velocity_trajectory.csv"
    compared = compare(records(baseline), records(gather))
    if len(compared) != 22:
        raise ValueError("unexpected non-timing field scope")
    for stem in ("tier8_counter", "tier8_gather"):
        validate_prefix(directory, stem)
        if summarize(directory / (stem + "_prefix.log")) != json.loads(
                (directory / (stem + "_profile.v1.json")).read_text()):
            raise ValueError("stale counter analysis")
    validate_witness((directory / "eos_refusal_expansion.log").read_text(), hashlib.sha256(
        (root / "rendered/fire_production_calibration/r202_owner_cost/step_0000000044.checkpoint").read_bytes()).hexdigest())
    sources = [root / name for name in (
        "src/Library/Utilities/FireProductionAdvectionMac.mm", "tests/FireSequenceTest.cpp",
        "tools/analyze_fire_producer_kernels.py", "tools/fire_metal_counter_probe.mm",
        "tools/seal_fire_producer_campaign.py", "docs/FIRE_SMOKE_PRODUCER_KERNEL_CAMPAIGN.md")]
    subprocess.run(["git", "diff", "--exit-code", "HEAD", "--"] +
                   [str(path.relative_to(root)) for path in sources], cwd=root, check=True,
                   stdout=subprocess.DEVNULL)
    qualification_commit = subprocess.check_output(
        ["git", "rev-parse", qualification_commit + "^{commit}"], cwd=root, text=True).strip()
    # A checker-only review repair may reseal existing GPU evidence, but may
    # not relabel the executable as a build of the newer checker commit.
    subprocess.run(["git", "diff", "--exit-code", qualification_commit, "--", "src", "tests", "build"],
                   cwd=root, check=True, stdout=subprocess.DEVNULL)
    artifacts = [kernel, owner_log, owner_trace, owner_csv,
                 directory / "eos_refusal_expansion.log",
                 directory / "kernel_sweep.v2.log",
                 directory / "gather_prefix_comparison.v1.json",
                 directory / "tier8_counter_profile.v1.json",
                 directory / "tier8_gather_profile.v1.json",
                 directory / "tier8_counter_prefix.log", directory / "tier8_gather_prefix.log"]
    for stem in ("tier8_counter_prefix", "tier8_gather_prefix"):
        artifacts.extend(sorted(path for path in (directory / stem).rglob("*") if path.is_file()))
    files = sources + artifacts
    result = {
        "schema": "rise.fire.producer_campaign.v1",
        "sealing_source_commit": subprocess.check_output(
            ["git", "rev-parse", "HEAD"], cwd=root, text=True).strip(),
        "qualification_source_commit": qualification_commit,
        "qualification_executable_sha256": hashlib.sha256(
            (root / "bin/tests/FireSequenceTest").read_bytes()).hexdigest(),
        "qualified_scope": "EOS bin-search repair, full case log domain, existing owner and kernel gates",
        "performance_scope": "exploratory matched three-step prefix; not final-commit production timing",
        "timing_nontiming_fields_exact": compared,
        "migration_certificate": False, "affordable_producers": False,
        "fixed_k_selected": False, "focusing_verdict": "unavailable",
        "inputs": {str(path.relative_to(root)): hashlib.sha256(path.read_bytes()).hexdigest()
                   for path in files},
        "exploratory_executables": {name: hashlib.sha256((root / "bin/tests" / name).read_bytes()).hexdigest()
            for name in ("FireSequenceTest_counter_baseline", "FireSequenceTest_gather_baseline",
                         "FireSequenceTest_refusal_witness")},
    }
    with output.open("x") as stream:
        json.dump(result, stream, indent=2)
        stream.write("\n")


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--root", type=Path, default=Path.cwd())
    parser.add_argument("--directory", type=Path, required=True)
    parser.add_argument("--output", type=Path)
    parser.add_argument("--self-test", action="store_true")
    parser.add_argument("--qualification-commit")
    args = parser.parse_args()
    if args.self_test:
        self_test(args.directory.resolve())
    elif args.output and args.qualification_commit:
        seal(args.root.resolve(), args.directory.resolve(), args.output.resolve(), args.qualification_commit)
    else:
        parser.error("--output and --qualification-commit, or --self-test, are required")
