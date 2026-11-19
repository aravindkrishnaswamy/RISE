#!/usr/bin/env python3
"""Reproduce the executed r213 EOS cost comparison; not fidelity or migration authority."""
import argparse
import collections
import csv
import hashlib
import io
import json
import math
from pathlib import Path

from analyze_fire_producer_kernels import fields, summarize
from check_fire_owner_instrumentation import trees
from report_fire_r212_cost import report as baseline_report, stats

ROOT = Path(__file__).resolve().parents[1]
BASE = ROOT / "rendered/fire_production_calibration"
OUT = BASE / "r213_composition"
EXECUTABLE = "8b7432df0c5637fc019df52dd77cae0648d9a5c82bc9c3db4f2779ef97c465bb"
QUALIFICATION = "bb4bbbf97e2722bfe9740c39949f258f1681f56a9270b866348c7deb3d9b9757"
EOS_LOG = "2525e547997dfdac9c90bfad3350461bc8fae5e887c25f823843c1cf9bbd4024"
CHECKPOINT = "2422002e0d45746989b1fa3676f1f4027c785e91bec6b99c3d88b9b01ef12bb2"
KERNEL_SET = "d580d078ea083ee18032abfb027271ada9d4d1dabab50b35cb6eb7a503258a0e"
LOGS = (
    "d78a711fec47fe3a722ea4c90f183a05402f2c9f7621ba3a192e9880756dc65e",
    "5c9ae61286e3b1ccfc05cb909baab475aadf1369d49591c6bd7b8c9f51814cbe",
    "8217b75404e9d7540a97cd329a8573417aa8d828ead07a4447b97e2ee53606b8",
)
TRAJECTORIES = (
    "ccb7b42ded700273c49218c11c7c917de22f805f019afe674fdc2d0508154e3e",
    "afcb99cbf7c70692223808887df06160dd304aacad2eb60ddd8d5ff7dcba2db2",
    "e194f68f09b3f57fa024a30d45cfc2152e9df835d3dd3791b9653abce7616b54",
)
PHYSICS = ("accepted_step time_s dt_s maximum_velocity_m_per_s axis face x y z "
           "manifold_max manifold_p95 manifold_p50 tail_cells tail_drained_m3 "
           "owner_r0_iterations owner_r1_iterations owner_r2_iterations").split()


def authenticated(path, expected, inputs):
    raw = path.read_bytes()
    digest = hashlib.sha256(raw).hexdigest()
    if digest != expected:
        raise ValueError("executed input SHA mismatch: "+str(path))
    inputs[str(path.relative_to(ROOT))] = digest
    return raw


def validate_hot(log, rows, reference):
    headers = [fields(row) for row in log.splitlines() if row.split()[:1] == ["OWNER_EOS_DIAGNOSTIC"]]
    endings = [fields(row) for row in log.splitlines() if row.split()[:1] == ["OWNER_EOS_DIAGNOSTIC_END"]]
    if (len(headers) != 1 or headers[0].get("checkpoint_sha256") != CHECKPOINT
            or headers[0].get("accepted_steps") != "1300"
            or headers[0].get("beginning_s") != "2.1080244191689417"
            or endings != [dict(solver_accepted="1", checkpoint_unchanged="1",
                                migration_authority="false", error="")]):
        raise ValueError("missing, contradictory, or incomplete hot execution")
    if len(rows) != 8 or len(reference) != 8:
        raise ValueError("hot comparison requires exactly eight accepted steps")
    for index, (row, old) in enumerate(zip(rows, reference)):
        if row["accepted_step"] != str(1301+index) or any(row[k] != old[k] for k in PHYSICS):
            raise ValueError("equal-time physics summary mismatch at step "+str(1301+index))
        if row["qualified_kernel_set_sha256"] != KERNEL_SET:
            raise ValueError("unexpected kernel-set identity")
        for key in ("input_payload_root_sha256", "publication_payload_root_sha256"):
            if (len(row[key]) != 64 or any(c not in "0123456789abcdef" for c in row[key])
                    or row[key] == old[key]):
                raise ValueError("kernel-dependent root identity does not match this execution")
    forest = trees(log)
    if len(forest) != 8:
        raise ValueError("missing whole-owner profile")
    for tree, row in zip(forest, rows):
        total = sum(x["exclusive_device_sum_ms"] for x in tree)
        formatting = (2*len(tree)+4)*1e-9+4*math.ulp(float(row["device_ms"]))
        if abs(total-float(row["device_ms"])) > formatting:
            raise ValueError("exclusive device accounting does not reconcile")
    return forest


def table(rows):
    return {key: stats([float(row[key]) for row in rows]) for key in
            ("device_ms", "wall_ms", "owner_projection_device_ms", "owner_nonprojection_device_ms")}


def report():
    inputs = {}
    if len(set(LOGS)) != 3 or len(set(TRAJECTORIES)) != 3:
        raise ValueError("independent process evidence must be distinct")
    qualified = json.loads(authenticated(OUT / "qualification.v2.json", QUALIFICATION, inputs))
    if (qualified["producer_executable_sha256"] != EXECUTABLE
            or qualified["source_commit"] != "18183251ed4e5b45eba8cf60050bf55d22c8ef36"):
        raise ValueError("qualified source/executable binding changed")
    authenticated(ROOT / "bin/tests/FireSequenceTest_r213_18183251", EXECUTABLE, inputs)
    authenticated(BASE / "r208_single_source/ported_from_zero.v1/checkpoints/step_0000001300.checkpoint",
                  CHECKPOINT, inputs)
    eos = authenticated(OUT / "eos_domain.v1.log", EOS_LOG, inputs).decode()
    if sum(row.startswith("RESIDENT_EOS passed=1 ") for row in eos.splitlines()) != 1:
        raise ValueError("EOS qualification missing")
    old = baseline_report(BASE)
    inputs.update({str(Path(k).relative_to(ROOT)) if Path(k).is_absolute() else k: v
                   for k,v in old["inputs_sha256"].items()})
    # The reference bytes were authenticated by baseline_report; bind this read
    # again before admitting its parsed rows to the new comparison.
    ref_path = BASE / "r211_resident_migration/continuation.v2/budgets/maximum_velocity_trajectory.csv"
    reference = list(csv.DictReader(io.StringIO(authenticated(ref_path,
        "cb10278dc49f33c9a9718496804bf81fe5493faf4af0eb27cd1a3b7db3d1a057", inputs).decode())))[:8]
    collected, repeats, comparisons = [], [], []
    kernels = collections.defaultdict(lambda: [0, 0.0])
    phases = collections.defaultdict(lambda: [0.0, 0.0])
    for index in range(3):
        directory = OUT / f"hot_profile_{index+1}.v1"
        log_path = Path(str(directory)+".log")
        log = authenticated(log_path, LOGS[index], inputs).decode()
        rows = list(csv.DictReader(io.StringIO(authenticated(directory / "budgets/maximum_velocity_trajectory.csv",
                                                           TRAJECTORIES[index], inputs).decode())))
        forest = validate_hot(log, rows, reference)
        counter = summarize(log_path)
        if counter["input_sha256"] != LOGS[index]:
            raise ValueError("producer log changed during second parse")
        for tree in forest:
            for phase in tree:
                phases[phase["phase"]][0] += phase["exclusive_device_sum_ms"]
                phases[phase["phase"]][1] += phase["exclusive_wall_ms"]
        for name, value in counter["kernels"].items():
            kernels[name][0] += value["calls"]
            kernels[name][1] += value["inclusive_total_ms"]
        collected.extend(rows)
        repeats.append(table(rows))
        comparisons.append([dict(accepted_step=row["accepted_step"], time_s=row["time_s"],
            dt_s=row["dt_s"], summary_fields_equal=True,
            input_root=row["input_payload_root_sha256"], output_root=row["publication_payload_root_sha256"],
            full_roots_equal=False, numerical_payload_byte_equality="pending_snapshot_comparison") for row in rows])
    current = table(collected)
    current["wall_minus_device_ms"] = stats([float(r["wall_ms"])-float(r["device_ms"]) for r in collected])
    return dict(schema="rise.fire.r213.eos-cost-observation.v1", inputs_sha256=inputs,
        source_commit=qualified["source_commit"], executable_sha256=EXECUTABLE,
        process_exit_codes=[93, 93, 93], legacy_exit_semantics="93 plus explicit solver_accepted=1 and eight rows; diagnostic, not migration",
        eos_qualification=dict(log_sha256=EOS_LOG, samples=49512449,
            lattice=24756225, midpoints=24756224, worst_log_residual_over_bound=0.5192248117002557,
            cellwise_temperature_pressure_deviation_bit_match=True, timing_scope="correctness_only_contended"),
        baseline_r212=old["hot_profiles"], after=current, independent_processes=repeats,
        process_mean_device_ms=stats([r["device_ms"]["mean"] for r in repeats]),
        process_mean_wall_ms=stats([r["wall_ms"]["mean"] for r in repeats]),
        mean_device_speedup=old["hot_profiles"]["device_ms"]["mean"]/current["device_ms"]["mean"],
        mean_wall_speedup=old["hot_profiles"]["wall_ms"]["mean"]/current["wall_ms"]["mean"],
        inclusive_kernels={k:dict(calls=v[0], ms_per_call=v[1]/v[0], ms_per_step=v[1]/24)
                           for k,v in kernels.items()},
        baseline_inclusive_kernels=old["inclusive_kernels"],
        exclusive_phases_mean_ms={k:dict(device=v[0]/24, wall=v[1]/24) for k,v in phases.items()},
        equal_time_comparisons=comparisons,
        scope="Three isolated eight-step hot processes. Timings are observation, not optimization adoption: hot numerical byte equality awaits retained snapshots. Kernel intervals overlap and are not additive. Wall-device residual is not measured hashing cost. No fixed-k, full-window, fidelity, or migration verdict.")


if __name__ == "__main__":
    parser = argparse.ArgumentParser()
    parser.add_argument("--output", type=Path)
    args = parser.parse_args()
    text = json.dumps(report(), sort_keys=True, indent=2, allow_nan=False)+"\n"
    if args.output:
        args.output.write_text(text)
    else:
        print(text, end="")
