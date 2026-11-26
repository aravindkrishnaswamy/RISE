#!/usr/bin/env python3
"""Replay the four versioned S4 diagnostic settings sequentially.

The input must already resolve 550 and 1300 nm. Reports preserve unresolved
histories; this experiment does not validate physiological reflectance.
"""
from __future__ import annotations

import argparse
import json
import os
from pathlib import Path
import subprocess

ROOT = Path(__file__).resolve().parents[1]
DEFAULT_BINARY = ROOT / "bin" / "tools" / ("hylios_measure.exe" if os.name == "nt" else "hylios_measure")


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--specimen", type=Path, required=True, help="Resolved S4 JSON containing 550 and 1300 nm")
    parser.add_argument("--output-dir", type=Path, required=True, help="Destination for four reports and a summary")
    parser.add_argument("--binary", type=Path,
                        default=Path(os.environ.get("RISE_HYLIOS_MEASURE", str(DEFAULT_BINARY))))
    options = parser.parse_args()
    binary = options.binary.expanduser().resolve()
    if not binary.is_file():
        parser.error(f"measurement binary missing: {binary}; build with "
                     "make -C build/make/rise build-tool/hylios_measure")
    options.output_dir.mkdir(parents=True, exist_ok=True)
    rows = []
    for wavelength in (550, 1300):
        for placement in ("fixed", "null"):
            output = options.output_dir / f"s4-{wavelength}-{placement}.json"
            subprocess.run([str(binary), "--specimen", str(options.specimen),
                            "--wavelength-nm", str(wavelength), "--placement", placement,
                            "--post-scatter", "diagnostic", "--boundary", "smooth", "--histories", "1000",
                            "--max-transitions", "10000", "--seed", str(wavelength * 100000 + 1),
                            "--output", str(output)], check=True)
            value = json.loads(output.read_text())
            row = {
                "wavelength_nm": wavelength,
                "placement": placement,
                "input_bytes_sha256": value["input_bytes_sha256"],
                "initial_histories": value["initial_history_count"],
                "outcome_counts": {key: record["count"] for key, record in value["outcomes"].items()},
                "live_unresolved_weight": sum(record["terminal_live_weight"]
                                              for record in value["outcomes"].values()
                                              if record["unresolved"]),
                "physical_absorbed_weight": sum(value["host_absorbed_weight_by_layer"] +
                                                value["particle_absorbed_weight_by_layer"]),
                "escaped_weight": sum(value["outcomes"][key]["terminal_live_weight"]
                                      for key in ("external_reflection", "subsurface_return")),
                "counters": value["aggregate_counters"],
            }
            if sum(row["outcome_counts"].values()) != row["initial_histories"]:
                raise RuntimeError("measurement does not account for every incident history")
            # This replay deliberately uses analog absorption without roulette.
            # Its per-history accounted powers are exact integer zero/one values.
            if (row["physical_absorbed_weight"] + row["escaped_weight"] +
                    row["live_unresolved_weight"] != row["initial_histories"]):
                raise RuntimeError("analog replay does not account for all incident power")
            rows.append(row)
            print(wavelength, placement, row["outcome_counts"],
                  "unresolved_live_weight", row["live_unresolved_weight"])
    summary = options.output_dir / "s4-exploration-summary.json"
    summary.write_text(json.dumps(rows, indent=2, allow_nan=False) + "\n")


if __name__ == "__main__":
    main()
