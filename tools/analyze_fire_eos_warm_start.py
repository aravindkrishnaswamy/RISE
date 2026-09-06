#!/usr/bin/env python3
"""Measure the r206 warm-start experiment, never certify a production window.

Timing prefixes are three independent processes per variant, three cold steps
each. The additional iteration dispatch is diagnostic and is excluded from all
timing claims. Per-cell owner qualification remains a separate requirement.
"""
import argparse
import collections
import hashlib
import json
import math
import shlex
from pathlib import Path
import statistics
from unittest.mock import patch

from analyze_fire_producer_kernels import fields, summarize
from seal_fire_payload_placement import bind_counters, check_rows, completion_record, distinct_repeats, gate, records, sidecars
from check_fire_owner_cost_prefix import metadata
from check_fire_owner_instrumentation import unique_object


def histogram(path):
    raw = path.read_bytes()
    result = {}
    sequences = []
    for tag, size, work_name in (("EOS_ITERATIONS_V1", 33, "bisections"),
                                 ("EOS_WARM_PROBES_V1", 97, "interior_energy_probes")):
        required = {"stage", "raw_iteration", "cells", "total", "scope", "histogram", work_name, "bit_mismatch"}
        required |= ({"refused", "lower_endpoint", "previous_temperature_bit_equal", "Tmin", "Tmax"}
                     if tag == "EOS_ITERATIONS_V1" else {"cold_fallback", "overflow"})
        rows = []
        for line in raw.decode().splitlines():
            if line.split()[:1] != [tag]:
                continue
            row = fields(line)
            if set(row) != required or len(line.split()) != len(required)+1:
                raise ValueError("incomplete, duplicate, or unknown diagnostic counters: " + tag)
            rows.append(row)
        if len(rows) != 42:
            raise ValueError("expected all 42 accepted candidate evaluations: " + tag)
        bins = collections.Counter()
        cells = work = fallbacks = lower = 0
        sequence = []
        for row in rows:
            values = list(map(int, row["histogram"].split(",")))
            count = int(row["cells"])
            if (len(values) != size or any(n < 0 for n in values)
                    or count != 504666 or sum(values) != count
                    or int(row["total"]) != count
                    or sum(i*n for i, n in enumerate(values)) != int(row[work_name])
                    or row["scope"] != "diagnostic_extra_dispatch"
                    or any(int(row[k]) != 0 for k in required & {"refused", "bit_mismatch", "overflow"})):
                raise ValueError("invalid or non-identical iteration histogram: " + tag)
            if tag == "EOS_ITERATIONS_V1":
                # This experiment is the pinned tier-8 300..2300 K case, not
                # a general domain-study parser. Zero bisections means Tmin.
                if (int(row["lower_endpoint"]) != values[0]
                        or not 0 <= int(row["previous_temperature_bit_equal"]) <= count
                        or float(row["Tmin"]) != 300.0 or float(row["Tmax"]) != 2300.0):
                    raise ValueError("invalid cold population or case temperature bounds")
            elif not 0 <= int(row["cold_fallback"]) <= count-values[0]:
                raise ValueError("fallback count outside interior population")
            sequence.append((int(row["stage"]), int(row["raw_iteration"])))
            cells += count
            work += int(row[work_name])
            fallbacks += int(row.get("cold_fallback", "0"))
            lower += int(row.get("lower_endpoint", "0"))
            bins.update({i: n for i, n in enumerate(values) if n})
        sequences.append(sequence)
        result[tag] = dict(calls=len(rows), cell_evaluations=cells,
                           work_name=work_name, work=work,
                           mean_work_per_cell=work/cells, histogram=dict(bins),
                           lower_endpoint=lower, cold_fallback=fallbacks,
                           bit_mismatches=0)
    if sequences[0] != sequences[1]:
        raise ValueError("warm/cold candidate sequence differs")
    prefix = path.with_suffix("")
    sidecars(prefix)
    trajectory = records(prefix / "budgets/maximum_velocity_trajectory.csv")
    completion_record(raw.decode(), trajectory, prefix / "diagnostic_prefix_outcome.v1")
    expected = [(s, i) for row in trajectory for s in range(3)
                for count in [int(row[f"owner_r{s}_iterations"])]
                for i in [0xffffffff, *range(count), 0x80000000 | (count-1)]]
    if len(trajectory) != 3 or sequences[0] != expected:
        raise ValueError("missing, repeated, or reordered candidate")
    result["log_sha256"] = hashlib.sha256(raw).hexdigest()
    return result


def summary(values):
    return dict(samples=len(values), mean=statistics.mean(values),
                stdev=statistics.stdev(values),
                p95=sorted(values)[math.ceil(.95*len(values))-1])


def endpoint_eos_gate(directory, qualified, qualification_path):
    path = directory / "qualification.eos.v2.json"
    evidence = json.loads(path.read_text(), object_pairs_hook=unique_object)
    if (evidence["schema"] != "rise.fire.executed-eos-gate.v1"
            or evidence["source_commit"] != qualified["source_commit"]
            or evidence["parent_qualification_sha256"] != hashlib.sha256(qualification_path.read_bytes()).hexdigest()
            or any(evidence[key] != qualified["producer_executable_sha256"]
                   for key in ("executable_before_sha256", "executable_after_sha256"))
            or evidence["command"] != [qualified["runs"]["owner"]["command"][0], "--fire-production-resident-eos-metal"]
            or evidence["exit_code"] != 0):
        raise ValueError("standalone EOS execution identity failed")
    raw = (directory / evidence["log"]).read_bytes()
    if hashlib.sha256(raw).hexdigest() != evidence["log_sha256"]:
        raise ValueError("standalone EOS log identity failed")
    def one(tag):
        lines = [line for line in raw.decode().splitlines() if line.split()[:1] == [tag]]
        if len(lines) != 1:
            raise ValueError("missing/duplicate EOS verdict: " + tag)
        words = shlex.split(lines[0])[1:]
        result = dict(word.split("=", 1) for word in words)
        if len(result) != len(words):
            raise ValueError("duplicate EOS verdict counter")
        return result
    verdict = one("RESIDENT_EOS")
    if verdict.get("passed") != "1" or verdict.get("eos_table_mutation_refused") != "1":
        raise ValueError("standalone EOS fixture failed")
    study = one("RESIDENT_EOS_LOG_ENCLOSURE")
    if (study.get("passed") != "1" or study.get("samples") != "49512449"
            or study.get("metal_library_source_sha256") != evidence["metal_library_source_sha256"]
            or not math.isfinite(float(study["max_residual_over_bound"]))
            or not 0 <= float(study["max_residual_over_bound"]) <= 1):
        raise ValueError("standalone EOS domain qualification failed")
    reds = [fields(line) for line in raw.decode().splitlines() if line.split()[:1] == ["RESIDENT_EOS_RED"]]
    expected_names = set("r170_hard_bound_30_percent pressure_midpoint_rounding_ambiguous "
        "deviation_midpoint_rounding_ambiguous zero_lower_upper_bin_ambiguous subnormal_lower_upper_bin_ambiguous "
        "eos_lower_inversion_endpoint eos_upper_inversion_endpoint r170_exact_above_binary32_rounds_to_bound "
        "endpoint_enclosure_bit_mutation forged_device_stage forged_device_precision forged_device_attempt "
        "forged_device_cells forged_device_timestep paired_first_eos_failure_witness".split())
    if (len(reds) != len(expected_names) or {row["name"] for row in reds} != expected_names
            or any(row.get("passed") != "1" for row in reds)):
        raise ValueError("incomplete or failed EOS RED battery")
    mutation = [row for row in reds if row["name"] == "endpoint_enclosure_bit_mutation"]
    expected = dict(name="endpoint_enclosure_bit_mutation", expected="0x00000080", observed="0x00000080",
                    attempted="1", read="1", commands="1", staging="1", candidate_identity="0", eos_identity="0", passed="1")
    if mutation != [expected]:
        raise ValueError("endpoint mutation was not atomically refused")
    return dict(artifact_sha256=hashlib.sha256(path.read_bytes()).hexdigest(),
                log_sha256=evidence["log_sha256"], metal_library_source_sha256=evidence["metal_library_source_sha256"])


def analyze(directory, qualification=None):
    owner_gate = directory / "warm.owner_gate.v1.log"
    gate(owner_gate)
    output = {"schema": "rise.fire.eos-warm-start-experiment.v1",
              "scope": "three-step cold-prefix diagnostic; no focusing or window verdict",
              "units": "milliseconds", "iterations": histogram(directory / "warm_iteration_prefix.v1.log"),
              "prototype_owner_gate_sha256": hashlib.sha256(owner_gate.read_bytes()).hexdigest(),
              "prototype_source_patch_sha256": hashlib.sha256((directory / "warm_prototype.v1.patch").read_bytes()).hexdigest(),
              "qualification_status": "exploratory dirty-tree prototype, not a commit-attested production qualification"}
    reference = records(directory / "baseline_profile_1.v1/budgets/maximum_velocity_trajectory.csv")
    reference_identity = metadata(directory / "baseline_profile_1.v1/diagnostic_from_zero_identity.v1")
    kinds = ["baseline", "warm"]
    executable_sha = {
        "baseline": "96fdc911d7d3fd59f4604f320052e3e658199fa7ff45caa605b054d9cfe77f16",
        "warm": "38095fe2a64eaba5e6e671e68cbf6c212f274a09fb4edfcbe3880563d86ef953"}
    if qualification is not None:
        qualified = json.loads(qualification.read_text(), object_pairs_hook=unique_object)
        if qualified["schema"] != "rise.fire.executed-build-and-owner-gate.v1":
            raise ValueError("wrong qualification schema")
        for name, run in qualified["runs"].items():
            log = Path(run["log"])
            if run["exit_code"] != 0 or hashlib.sha256(log.read_bytes()).hexdigest() != run["log_sha256"]:
                raise ValueError("qualification log identity failed: " + name)
        if set(qualified["runs"]) != {"build", "owner", "publication"}:
            raise ValueError("incomplete qualification")
        gate(Path(qualified["runs"]["owner"]["log"]))
        output["endpoint_eos_qualification"] = endpoint_eos_gate(directory, qualified, qualification)
        kinds.append("endpoints_qualified")
        executable_sha["endpoints_qualified"] = qualified["producer_executable_sha256"]
        output["endpoint_qualification"] = dict(source_commit=qualified["source_commit"],
            executable_sha256=qualified["producer_executable_sha256"],
            artifact_sha256=hashlib.sha256(qualification.read_bytes()).hexdigest())
    for kind in kinds:
        distinct_repeats(directory, [f"{kind}_profile_{repeat}.v1" for repeat in (1, 2, 3)])
        run_eos, step_eos, device, wall, evidence = [], [], [], [], []
        for repeat in (1, 2, 3):
            prefix = directory / f"{kind}_profile_{repeat}.v1"
            log = directory / f"{kind}_profile_{repeat}.v1.log"
            rows = records(prefix / "budgets/maximum_velocity_trajectory.csv")
            check_rows(rows, reference)
            outcome = prefix / "diagnostic_prefix_outcome.v1"
            identity_path = prefix / "diagnostic_from_zero_identity.v1"
            identity = metadata(identity_path)
            count = sidecars(prefix, identity["case_record_id"])
            outcome_fields = metadata(outcome)
            if (identity["producer_executable_sha256"] != executable_sha[kind]
                    or outcome_fields["from_zero_identity_sha256"] != hashlib.sha256(identity_path.read_bytes()).hexdigest()
                    or identity["resolution_tier"] != "8" or identity["seed"] != "1234"
                    or identity["initial_time_s"] != "0"
                    or identity["initial_state_sha256"] != reference_identity["initial_state_sha256"]
                    or identity["case_record_id"] != outcome_fields["case_record_id"]):
                raise ValueError("prefix producer/input identity failed")
            if outcome_fields["trajectory_sha256"] != hashlib.sha256((prefix / "budgets/maximum_velocity_trajectory.csv").read_bytes()).hexdigest():
                raise ValueError("outcome/trajectory identity failed")
            for key, name in (("protocol_sha256", "diagnostic_prefix_protocol.v1"),
                              ("retry_trajectory_sha256", "budgets/retry_attempt_trajectory.csv")):
                if outcome_fields[key] != hashlib.sha256((prefix / name).read_bytes()).hexdigest():
                    raise ValueError("outcome artifact identity failed: " + key)
            bind_counters(log, rows, outcome)
            profile = summarize(log)
            eos = profile["kernels"]["evaluate_resident_eos_candidate"]
            if eos["calls"] != 42:
                raise ValueError("unexpected EOS call count")
            run_eos.append(eos["mean_ms"])
            step_eos.append(eos["inclusive_total_ms"]/3)
            device.extend(float(r["device_ms"]) for r in rows)
            wall.extend(float(r["wall_ms"]) for r in rows)
            evidence.append(dict(log=log.name, log_sha256=profile["input_sha256"],
                                 outcome_sha256=hashlib.sha256(outcome.read_bytes()).hexdigest(),
                                 verified_publication_sidecars=count))
        output[kind] = dict(eos_per_call_process_means=summary(run_eos),
                            eos_per_step_process_means=summary(step_eos),
                            whole_owner_device=summary(device), whole_owner_wall=summary(wall),
                            evidence=evidence)
    output["cold_prefix_physics_columns_equal"] = True
    output["warning"] = "Prefix column equality is not the per-cell owner gate or resume certificate. Kernel intervals are inclusive."
    return output


def self_test(directory):
    """Mutate only in-memory diagnostic text; published evidence stays intact."""
    path = directory / "warm_iteration_prefix.v1.log"
    original_read = Path.read_bytes
    raw = original_read(path)
    histogram(path)
    text = raw.decode()
    cold_line = next(line for line in text.splitlines() if line.startswith("EOS_ITERATIONS_V1 "))
    warm_line = next(line for line in text.splitlines() if line.startswith("EOS_WARM_PROBES_V1 "))
    mutations = {
        "missing_bit_verdict": text.replace(" bit_mismatch=0", ""),
        "missing_refusal_verdict": text.replace(" refused=0", ""),
        "missing_overflow_verdict": text.replace(" overflow=0", ""),
        "duplicate_bit_verdict": text.replace("bit_mismatch=0", "bit_mismatch=1 bit_mismatch=0", 1),
        "temperature_bit_mismatch": text.replace("bit_mismatch=0", "bit_mismatch=1", 1),
        "missing_candidate": text.replace(cold_line + "\n", "", 1),
        "wrong_stage": text.replace(cold_line, cold_line.replace("stage=0", "stage=1"), 1),
        "negative_bin": text.replace("histogram=504566,", "histogram=-504566,", 1),
        "incorrect_work": text.replace(cold_line, cold_line.replace("bisections=", "bisections=9"), 1),
        "warm_overflow": text.replace(warm_line, warm_line.replace("overflow=0", "overflow=1"), 1),
        "missing_terminal": text[:text.index("OWNER_COST_PREFIX ")],
    }
    for field, line, invalid in (
            ("lower_endpoint", cold_line, ("-504566", "504667", "504565")),
            ("previous_temperature_bit_equal", cold_line, ("-1", "504667")),
            ("cold_fallback", warm_line, ("-40", "101", "504667")),
            ("Tmin", cold_line, ("nan", "inf", "-1", "301")),
            ("Tmax", cold_line, ("nan", "inf", "-1", "2299"))):
        old = fields(line)[field]
        for value in invalid:
            mutations[field+"_"+value] = text.replace(line, line.replace(field+"="+old, field+"="+value), 1)
    for name, mutant in mutations.items():
        if mutant == text:
            raise AssertionError("RED failed to mutate: " + name)
        def read_bytes(candidate):
            return mutant.encode() if candidate == path else original_read(candidate)
        with patch.object(Path, "read_bytes", read_bytes):
            try:
                histogram(path)
            except ValueError:
                continue
        raise AssertionError("accepted mutated diagnostic: " + name)
    first = directory / "baseline_profile_1.v1.log"
    def copied_read(candidate):
        if candidate in {directory / "baseline_profile_2.v1.log", directory / "baseline_profile_3.v1.log"}:
            return original_read(first)
        return original_read(candidate)
    with patch.object(Path, "read_bytes", copied_read):
        try:
            analyze(directory)
        except ValueError as error:
            if str(error) != "duplicate process logs":
                raise
        else:
            raise AssertionError("copied runs accepted as independent")
    try:
        distinct_repeats(directory, ["baseline_profile_1.v1"]*3)
    except ValueError:
        pass
    else:
        raise AssertionError("repeated resolved paths accepted")
    return {"schema": "rise.fire.eos-iteration-parser-reds.v1",
            "refused": list(mutations)+["copied_process_logs", "repeated_resolved_paths"]}


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("directory", type=Path)
    parser.add_argument("--self-test", action="store_true")
    parser.add_argument("--qualification", type=Path)
    args = parser.parse_args()
    result = self_test(args.directory) if args.self_test else analyze(args.directory, args.qualification)
    print(json.dumps(result, indent=2, sort_keys=True, allow_nan=False))
