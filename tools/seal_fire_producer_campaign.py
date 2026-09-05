#!/usr/bin/env python3
"""Seal r203 qualification separately from its exploratory timing evidence."""
import argparse
import hashlib
import json
from pathlib import Path
import subprocess

from analyze_fire_producer_kernels import summarize
from check_fire_owner_cost_prefix import compare, records
from check_fire_owner_instrumentation import qualify_artifact


def seal(root, directory, output):
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
        if summarize(directory / (stem + "_prefix.log")) != json.loads(
                (directory / (stem + "_profile.v1.json")).read_text()):
            raise ValueError("stale counter analysis")
    sources = [root / name for name in (
        "src/Library/Utilities/FireProductionAdvectionMac.mm", "tests/FireSequenceTest.cpp",
        "tools/analyze_fire_producer_kernels.py", "tools/fire_metal_counter_probe.mm",
        "tools/seal_fire_producer_campaign.py", "docs/FIRE_SMOKE_PRODUCER_KERNEL_CAMPAIGN.md")]
    subprocess.run(["git", "diff", "--exit-code", "HEAD", "--"] +
                   [str(path.relative_to(root)) for path in sources], cwd=root, check=True,
                   stdout=subprocess.DEVNULL)
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
        "qualification_source_commit": subprocess.check_output(
            ["git", "rev-parse", "HEAD"], cwd=root, text=True).strip(),
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
    parser.add_argument("--output", type=Path, required=True)
    args = parser.parse_args()
    seal(args.root.resolve(), args.directory.resolve(), args.output.resolve())
