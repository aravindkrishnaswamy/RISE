#!/usr/bin/env python3
"""r204 placement evidence: local physics equality, explicit identity migration, cost.

The CSV comparison is a short-prefix diagnostic, not an r78 certificate or a
substitute for the per-cell owner gate. Historical owner token bytes necessarily
change under the authorized digest version boundary.
"""
import argparse
import csv
import hashlib
import json
import math
from pathlib import Path
import statistics
import subprocess

from analyze_fire_producer_kernels import summarize
from check_fire_owner_cost_prefix import metadata
from fire_payload_merkle import merkle, verify

PHYSICS = "accepted_step time_s dt_s maximum_velocity_m_per_s axis face x y z manifold_max manifold_p95 manifold_p50 tail_cells tail_drained_m3 owner_commits owner_projections owner_r0_iterations owner_r1_iterations owner_r2_iterations".split()


def records(path):
    with path.open() as stream:
        return list(csv.DictReader(stream))


def check_rows(rows, baseline):
    if len(rows) != 3 or len(baseline) != 3:
        raise ValueError("three-step scope required")
    for row, old in zip(rows, baseline):
        for key in PHYSICS:
            if key not in row or key not in old or row[key] != old[key]:
                raise ValueError("prefix differs: " + key)
        if (row["intermediate_seal_format"] != "qualified-kernel-stage-token"
                or row["intermediate_digest_version"] != "2"
                or row["payload_digest_format"] != "rise-payload-sha256-merkle"
                or row["payload_digest_version"] != "2"):
            raise ValueError("missing explicit version boundary")
        for key in ("qualified_kernel_set_sha256", "input_payload_root_sha256", "publication_payload_root_sha256"):
            value = row[key]
            if len(value) != 64 or any(c not in "0123456789abcdef" for c in value):
                raise ValueError("missing root: " + key)
        if int(row["publication_payload_bytes"]) <= 32:
            raise ValueError("empty publication packet")
        for key in ("device_ms", "wall_ms"):
            if not math.isfinite(float(row[key])) or float(row[key]) <= 0:
                raise ValueError("invalid cost")


def sidecars(directory, case_id=None):
    count = 0
    for path in sorted(directory.rglob("*")):
        if not path.is_file() or path.name.endswith(".payload-v2.json"):
            continue
        payload = path.read_bytes()
        seal = json.loads(Path(str(path) + ".payload-v2.json").read_text())
        if (set(seal) != {"schema", "case_record_id", "sha256_v1", "v2"}
                or seal["schema"] != "rise.fire.published-payload.v2"
                or len(seal["case_record_id"]) != 64
                or (case_id is not None and seal["case_record_id"] != case_id)
                or seal["sha256_v1"] != hashlib.sha256(payload).hexdigest()
                or not verify(payload, seal["v2"])):
            raise ValueError("invalid published sidecar: " + str(path))
        count += 1
    if not count:
        raise ValueError("empty evidence directory")
    return count


def gate(path):
    text = path.read_text()
    rows = [dict(item.split("=", 1) for item in line.split()[1:] if "=" in item)
            for line in text.splitlines() if line.startswith("OWNER_SEALING_EQUIVALENCE field=")]
    if len(rows) != 41 or len({r["field"] for r in rows}) != 41 or any(r["bit_mismatches"] != "0" for r in rows):
        raise ValueError("incomplete per-field bit comparison")
    for required in ("OWNER_PUBLICATION_DIGEST_RED full_packet_cpu_match=1 bit_mutation_refused=1 copied_authority_refused=1",
                     "OWNER_SEALING_EQUIVALENCE passed=1",
                     "PROJECTED_HEUN_METAL_OWNER_FP64 source=1 begin=1 r0=1 r1=1 accepted=1 criterion=conjunction_of_per_cell_per_field_same_unit_enclosures error= passed=1"):
        if required not in text:
            raise ValueError("missing owner/publication gate: " + required)


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("directory", type=Path)
    parser.add_argument("--commit", required=True)
    parser.add_argument("--repeats", nargs="+", required=True)
    parser.add_argument("--gate", type=Path, required=True)
    parser.add_argument("--baseline", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    args = parser.parse_args()
    if len(args.repeats) < 3:
        raise ValueError("minimum three independent process runs")
    commit = subprocess.check_output(["git", "rev-parse", args.commit], text=True).strip()
    subprocess.run(["git", "diff", "--exit-code", commit, "--", "src", "tests", "build"], check=True,
                   stdout=subprocess.DEVNULL)
    executable = hashlib.sha256(Path("bin/tests/FireSequenceTest").read_bytes()).hexdigest()
    gate(args.gate)
    baseline = records(args.baseline)
    samples, profiles, roots, counts = [], [], [], []
    for name in args.repeats:
        directory = args.directory / name
        rows = records(directory / "budgets/maximum_velocity_trajectory.csv")
        check_rows(rows, baseline)
        protocol = metadata(directory / "diagnostic_prefix_protocol.v1")
        outcome = metadata(directory / "diagnostic_prefix_outcome.v1")
        if (protocol.get("producer_executable_sha256") != executable
                or protocol.get("from_zero") != "true" or protocol.get("resume") != "false"
                or outcome.get("prefix_complete") != "true" or outcome.get("accepted_steps") != "3"):
            raise ValueError("wrong executable or incomplete prefix")
        for key, name_on_disk in (("protocol_sha256", "diagnostic_prefix_protocol.v1"),
                                  ("from_zero_identity_sha256", "diagnostic_from_zero_identity.v1"),
                                  ("trajectory_sha256", "budgets/maximum_velocity_trajectory.csv"),
                                  ("retry_trajectory_sha256", "budgets/retry_attempt_trajectory.csv")):
            if outcome.get(key) != hashlib.sha256((directory / name_on_disk).read_bytes()).hexdigest():
                raise ValueError("stale outcome: " + key)
        samples.append(rows)
        roots.append([(r["input_payload_root_sha256"], r["publication_payload_root_sha256"]) for r in rows])
        counts.append(sidecars(directory, outcome["case_record_id"]))
        profiles.append(summarize(args.directory / (name + ".log")))
    if any(value != roots[0] for value in roots[1:]):
        raise ValueError("cross-process payload roots differ")
    # Named REDs: erased/altered roots, a changed numerical observable, and a
    # historical-mode relabel must each defeat this evidence gate.
    for key, bad in (("publication_payload_root_sha256", ""), ("manifold_max", "1"),
                     ("payload_digest_version", "1")):
        mutant = [dict(r) for r in samples[0]]
        mutant[0][key] = bad
        try:
            check_rows(mutant, baseline)
        except ValueError:
            continue
        raise AssertionError("evidence mutant escaped: " + key)
    def stats(values):
        return {"samples": len(values), "mean": statistics.mean(values),
                "stdev": statistics.stdev(values), "p95_nearest_rank": sorted(values)[math.ceil(.95 * len(values)) - 1]}
    evidence = {"schema": "rise.fire.payload-placement-evidence.v2", "source_commit": commit,
                "producer_executable_sha256": executable,
                "scope": "three independent three-step cold prefixes; not r78, onset, or fixed-k authority",
                "owner_gate": "41 numerical fields bit-identical; reviewed r190 local/class envelope gate unchanged",
                "prefix_physics_columns_equal": PHYSICS, "process_roots_equal": True,
                "published_file_counts": counts,
                "device_step_ms": stats([float(r["device_ms"]) for run in samples for r in run]),
                "wall_step_ms": stats([float(r["wall_ms"]) for run in samples for r in run]),
                "producer_profiles": profiles, "affordable_producer_claim": False, "files": {}}
    paths = [args.gate, args.baseline]
    for name in args.repeats:
        paths += [args.directory / (name + ".log")]
        paths += sorted(p for p in (args.directory / name).rglob("*") if p.is_file())
    for path in paths:
        data = path.read_bytes()
        evidence["files"][str(path)] = {"sha256_v1": hashlib.sha256(data).hexdigest(), "v2": merkle(data)}
    data = (json.dumps(evidence, indent=2, sort_keys=True) + "\n").encode()
    with args.output.open("xb") as output:
        output.write(data)
    with Path(str(args.output) + ".seal-v2.json").open("x") as output:
        json.dump({"sha256_v1": hashlib.sha256(data).hexdigest(), "v2": merkle(data)}, output, indent=2)
    print(json.dumps({k: evidence[k] for k in ("source_commit", "device_step_ms", "wall_step_ms")}))


if __name__ == "__main__":
    main()
