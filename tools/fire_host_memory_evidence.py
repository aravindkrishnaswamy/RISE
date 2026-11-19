#!/usr/bin/env python3
"""Check the r220 diagnostic identity comparison, never migration authority."""
import argparse
import csv
import hashlib
import json
from pathlib import Path


def binding(path):
    payload = path.read_bytes()
    return {"path": str(path.resolve()), "bytes": len(payload),
            "sha256": hashlib.sha256(payload).hexdigest()}


def compare(root, repaired_prefix="pool_prefix.v1",
            repaired_executable="5abb059c13cbc36c47c1e4b59ce943f494dbcc1a61a10156cb40fec9e2236135"):
    expected = (
        ("frozen_prefix.v2", "91978041bb53124e45324838a082d6fb64bb0f2a6da28c68ddc7e8c186faaefa"),
        (repaired_prefix, repaired_executable),
    )
    timing = {"device_ms", "wall_ms", "owner_projection_device_ms", "owner_nonprojection_device_ms"}
    required = {"accepted_step", "time_s", "dt_s", "maximum_velocity_m_per_s",
                "input_payload_root_sha256", "publication_payload_root_sha256",
                "qualified_kernel_set_sha256", "publication_payload_bytes"}
    trajectories, files, producers, protocols = [], [], [], []
    for name, executable in expected:
        protocol = root / name / "diagnostic_prefix_protocol.v1"
        trajectory = root / name / "budgets/maximum_velocity_trajectory.csv"
        fields = dict(line.split(" ", 1) for line in protocol.read_text().splitlines())
        if fields["producer_executable_sha256"] != executable or fields["accepted_step_cap"] != "3":
            raise ValueError("actual execution identity or scope mismatch")
        with trajectory.open() as stream:
            rows = list(csv.DictReader(stream))
        if [row["accepted_step"] for row in rows] != ["1", "2", "3"]:
            raise ValueError("not exactly three accepted steps")
        if any(not (required | timing).issubset(row) for row in rows):
            raise ValueError("trajectory schema mismatch")
        trajectories.append([{key: value for key, value in row.items() if key not in timing} for row in rows])
        files.extend((binding(protocol), binding(trajectory)))
        producers.append({key: fields[key] for key in ("producer_build_id", "producer_executable_sha256")})
        protocols.append({key: value for key, value in fields.items() if key not in producers[-1]})
    if protocols[0] != protocols[1]:
        raise ValueError("diagnostic protocols differ beyond producer identities")
    if trajectories[0] != trajectories[1]:
        raise ValueError("non-timing trajectory mismatch")
    return {"schema": "rise.fire.host-memory-prefix-comparison.v1", "passed": True,
            "migration_authority": False, "files": files, "producers": producers,
            "compared_fields": sorted(trajectories[0][0]), "excluded_fields": sorted(timing),
            "accepted_steps": 3}


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("root", type=Path)
    parser.add_argument("--repaired-prefix", default="pool_prefix.v1")
    parser.add_argument("--repaired-executable", default="5abb059c13cbc36c47c1e4b59ce943f494dbcc1a61a10156cb40fec9e2236135")
    args = parser.parse_args()
    print(json.dumps(compare(args.root, args.repaired_prefix, args.repaired_executable), indent=2))
