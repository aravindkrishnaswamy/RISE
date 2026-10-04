#!/usr/bin/env python3
"""Run explicitly synthetic r246 optical controls in fresh sequential processes.

This is a render harness, never a simulation/window qualification. Preserve all
refused runs. Exact baseline pixel repetition is mandatory; no noise tolerance
is inferred from this small experiment.
"""
import argparse
import csv
import datetime
import hashlib
import json
import math
from pathlib import Path
import subprocess


def digest(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def write(path, value):
    path.write_text(json.dumps(value, indent=2) + "\n")


def run(binary, output):
    binary = binary.resolve(strict=True)
    output = output.resolve()
    binary_sha = digest(binary)
    write(output / "binary.json", {"path": str(binary), "sha256": binary_sha})
    stages = []
    images = []
    proofs = []
    names = ["base", "base_repeat", "upper_emission", "upper_emission_and_extinction"]
    for variant, name in enumerate(names):
        if digest(binary) != binary_sha:
            raise RuntimeError("frozen binary changed before stage")
        root = output / ("variant_" + str(variant))
        command = [str(binary), "--fire-r246-optical-single-control", str(variant), str(root)]
        log = output / ("variant_" + str(variant) + ".log")
        with log.open("w") as stream:
            process = subprocess.Popen(command, stdout=stream, stderr=subprocess.STDOUT)
            write(output / "current_stage.json", {"variant": variant, "pid": process.pid,
                  "command": command, "started_at": datetime.datetime.now(datetime.timezone.utc).isoformat()})
            code = process.wait()
        stages.append({"variant": variant, "exit_code": code, "log_sha256": digest(log)})
        write(output / "results.json", stages)
        if code:
            raise RuntimeError("native optical stage refused: " + str(variant))
        directory = root / name
        receipt_path = root / "synthetic_optical_control.v1"
        lines = receipt_path.read_text().splitlines()
        optical = next(line.split() for line in lines if " lower_channel_bits_sha256 " in line)
        stations = next(line.split() for line in lines if " synthetic_common_stations_sha256 " in line)
        fields = dict(zip(optical[1::2], optical[2::2]))
        station_fields = dict(zip(stations[1::2], stations[2::2]))
        for key, path in [("vdb_sha256", directory / "frame.vdb"),
                          ("manifest_sha256", directory / "synthetic_manifest.cbor"),
                          ("scene_sha256", directory / "synthetic_control.RISEscene"),
                          ("exr_sha256", directory / "temporal_primary.exr"),
                          ("decoded_rgb_sha256", directory / "decoded_linear_rgb.csv")]:
            if fields[key] != digest(path):
                raise RuntimeError("receipt input hash mismatch: " + key)
        if station_fields["synthetic_common_stations_sha256"] != digest(directory / "synthetic_common_stations.csv"):
            raise RuntimeError("common station hash mismatch")
        with (directory / "decoded_linear_rgb.csv").open(newline="") as stream:
            rows = list(csv.DictReader(stream))
        if len(rows) != 64 * 64 * 3 or any(int(row["component"]) != i for i, row in enumerate(rows)):
            raise RuntimeError("incomplete or shifted pixel components")
        pixels = [float(row["value"]) for row in rows]
        if not all(math.isfinite(value) for value in pixels):
            raise RuntimeError("nonfinite decoded pixels")
        images.append(pixels)
        proofs.append({"variant": variant, "receipt_sha256": digest(receipt_path),
                       "native_receipt": fields, "synthetic_stations": station_fields,
                       "primary_sidecar_sha256": digest(directory / "temporal_primary.exr.provenance.cbor"),
                       "display_sha256": digest(directory / "temporal_display.png"),
                       "display_sidecar_sha256": digest(directory / "temporal_display.png.provenance.cbor")})
    # Explicit order reproduces the original Python3.9 receipt on newer Python,
    # whose builtin sum uses a different floating-point reduction.
    def l1(first, second):
        total = 0.0
        for a, b in zip(first, second):
            total += abs(a-b)
        return total
    repeat = l1(images[0], images[1])
    emission = l1(images[0], images[2])
    extinction = l1(images[2], images[3])
    lower_equal = len({p["native_receipt"]["lower_channel_bits_sha256"] for p in proofs}) == 1
    common_equal = len({p["synthetic_stations"]["synthetic_common_stations_sha256"] for p in proofs}) == 1
    passed = lower_equal and common_equal and repeat == 0.0 and emission > 0.0 and extinction > 0.0
    report = {"schema": "rise.fire.r246.synthetic-optical-report.v1", "passed": passed,
              "artifact_fidelity": "synthetic_unit_preview", "accepted_simulation_steps": 0,
              "full_window_simulation_evidence": False, "original_region_dynamics_claim": False,
              "binary_sha256": binary_sha, "lower_channels_identical": lower_equal,
              "synthetic_common_stations_identical": common_equal, "repeat_linear_rgb_l1": repeat,
              "upper_emission_linear_rgb_l1": emission, "upper_extinction_linear_rgb_l1": extinction,
              "input_proofs": proofs, "stages": stages}
    write(output / "optical_report.json", report)
    return 0 if passed else 97


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--binary", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    args = parser.parse_args()
    try:
        args.output.mkdir(parents=True, exist_ok=False)
    except OSError:
        return 91
    try:
        code = run(args.binary, args.output)
    except Exception as error:
        if args.output.is_dir():
            write(args.output / "failure.json", {"error": str(error)})
        code = 98
    if args.output.is_dir():
        write(args.output / "completion.json", {"exit_code": code,
              "finished_at": datetime.datetime.now(datetime.timezone.utc).isoformat(),
              "scope": "four fresh-process sequential synthetic optical controls; no simulation"})
    return code


if __name__ == "__main__":
    raise SystemExit(main())
