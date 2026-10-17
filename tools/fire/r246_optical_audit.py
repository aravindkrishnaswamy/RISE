#!/usr/bin/env python3
"""Independently decode actual EXRs for the synthetic r246 optical control.
Requires OpenEXR and NumPy. Does not render, simulate or modify any input.
"""
import argparse
import csv
import hashlib
import json
import subprocess
from pathlib import Path
import OpenEXR
import numpy as np


def sha(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def audit(root, vdb_verifier):
    if not __debug__:
        raise RuntimeError("optimized Python execution is refused; acceptance checks must execute")
    report_path = root / "optical_report.json"
    report = json.loads(report_path.read_text())
    assert report["artifact_fidelity"] == "synthetic_unit_preview"
    assert report["accepted_simulation_steps"] == 0
    assert not report["full_window_simulation_evidence"]
    assert not report["original_region_dynamics_claim"]
    binary = json.loads((root / "binary.json").read_text())
    assert report["binary_sha256"] == binary["sha256"] == sha(Path(binary["path"]))
    assert len(report["input_proofs"]) == 4
    assert len(report["stages"]) == 4
    arrays = []
    proofs = []
    lower = []
    common = []
    names = ["base", "base_repeat", "upper_emission", "upper_emission_and_extinction"]
    for variant, name in enumerate(names):
        proof = report["input_proofs"][variant]
        stage = report["stages"][variant]
        assert proof["variant"] == variant and stage["variant"] == variant and stage["exit_code"] == 0
        assert stage["log_sha256"] == sha(root / ("variant_" + str(variant) + ".log"))
        directory = root / ("variant_" + str(variant)) / name
        receipt = directory.parent / "synthetic_optical_control.v1"
        assert proof["receipt_sha256"] == sha(receipt)
        fields = proof["native_receipt"]
        lines = receipt.read_text().splitlines()
        native = next(line.split() for line in lines if " lower_channel_bits_sha256 " in line)
        stations = next(line.split() for line in lines if " synthetic_common_stations_sha256 " in line)
        assert fields == dict(zip(native[1::2], native[2::2]))
        assert proof["synthetic_stations"] == dict(zip(stations[1::2], stations[2::2]))
        for key, filename in [("vdb_sha256", "frame.vdb"), ("manifest_sha256", "synthetic_manifest.cbor"),
                              ("scene_sha256", "synthetic_control.RISEscene"), ("exr_sha256", "temporal_primary.exr"),
                              ("decoded_rgb_sha256", "decoded_linear_rgb.csv")]:
            assert fields[key] == sha(directory / filename)
        for key, filename in [("primary_sidecar_sha256", "temporal_primary.exr.provenance.cbor"),
                              ("display_sha256", "temporal_display.png"),
                              ("display_sidecar_sha256", "temporal_display.png.provenance.cbor")]:
            assert proof[key] == sha(directory / filename)
        persisted = subprocess.run([str(vdb_verifier), str(directory / "frame.vdb")],
                                   check=True, stdout=subprocess.PIPE).stdout
        assert len(persisted) == 104 * 104 * 80 * 43
        lower_digest = hashlib.sha256(persisted).hexdigest()
        assert lower_digest == fields["lower_channel_bits_sha256"], "persisted lower VDB values/masks differ"
        lower.append(lower_digest)
        station_sha = proof["synthetic_stations"]["synthetic_common_stations_sha256"]
        assert station_sha == sha(directory / "synthetic_common_stations.csv")
        common.append(station_sha)
        with OpenEXR.File(str(directory / "temporal_primary.exr")) as image:
            channels = image.channels()
            actual = channels["RGBA"].pixels[:, :, :3].astype(np.float64)
        assert actual.shape == (64, 64, 3) and np.isfinite(actual).all()
        with (directory / "decoded_linear_rgb.csv").open(newline="") as stream:
            rows = list(csv.DictReader(stream))
        assert len(rows) == actual.size and all(int(row["component"]) == i for i, row in enumerate(rows))
        decoded = np.array([float(row["value"]) for row in rows]).reshape(actual.shape)
        assert np.array_equal(actual, decoded), "actual EXR and producer CSV differ"
        arrays.append(actual)
        proofs.append({"variant": variant, "exr_sha256": sha(directory / "temporal_primary.exr"),
                       "csv_exactly_matches_actual_exr": True})
    def l1(a, b):
        total = 0.0
        for x, y in zip(arrays[a].flat, arrays[b].flat):
            total += abs(float(x)-float(y))
        return total
    repeat, emission, extinction = l1(0, 1), l1(0, 2), l1(2, 3)
    assert repeat == report["repeat_linear_rgb_l1"] == 0.0
    assert emission == report["upper_emission_linear_rgb_l1"] and emission > 0.0
    assert extinction == report["upper_extinction_linear_rgb_l1"] and extinction > 0.0
    assert len(set(lower)) == len(set(common)) == 1
    assert report["lower_channels_identical"] and report["synthetic_common_stations_identical"] and report["passed"]
    return {"scope": "independent actual EXR decode and proof-root audit; synthetic optical control only",
            "passed": True, "vdb_verifier_sha256": sha(vdb_verifier),
            "persisted_lower_values_and_masks_recomputed": True, "report_sha256": sha(report_path), "OpenEXR_version": OpenEXR.__version__,
            "repeat_linear_rgb_l1": repeat, "upper_emission_linear_rgb_l1": emission,
            "upper_extinction_linear_rgb_l1": extinction,
            "upper_emission_changed_pixels": int(np.any(arrays[0] != arrays[2], axis=2).sum()),
            "upper_extinction_changed_pixels": int(np.any(arrays[2] != arrays[3], axis=2).sum()),
            "primary_csv_proofs": proofs, "actual_taller_allocation_exercised": False,
            "physical_qualification_established": False}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--input", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--vdb-verifier", type=Path, required=True)
    args = parser.parse_args()
    result = audit(args.input, args.vdb_verifier)
    with args.output.open("x") as stream:
        json.dump(result, stream, indent=2)
        stream.write("\n")


if __name__ == "__main__":
    main()
