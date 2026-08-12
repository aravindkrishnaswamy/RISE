#!/usr/bin/env python3
"""Verify owner-local HITEMP bytes and generate the section-3.5 record.

HITEMP archives are never downloaded or copied by this script.  The owner
places the 36 pinned files under --input-root.  Public MIT-licensed TIPS-2021
partition sums may optionally be fetched into input-root/TIPS, always through
their committed SHA-256 boundary.  Record generation consumes only the
committed derived dataset, so no raw line-list bytes enter the repository.
"""

from __future__ import annotations

import argparse
import bz2
import gzip
import hashlib
import json
import os
import pickle
import shutil
import subprocess
import sys
import tempfile
import urllib.request
import zipfile
from pathlib import Path

from generate_fire_gas_opacity_planck_record import (
    EXPECTED_SOURCE_MANIFEST_SHA256, generate,
)


TIPS_URL = "https://zenodo.org/records/4708099/files/{name}?download=1"


def sha256(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as source:
        for chunk in iter(lambda: source.read(1024 * 1024), b""):
            digest.update(chunk)
    return digest.hexdigest()


def verify_file(path: Path, expected: str) -> None:
    if not path.is_file() or sha256(path) != expected:
        raise ValueError(f"missing or SHA-256-mismatched input: {path}")


def fetch_tips(manifest: dict, input_root: Path) -> None:
    destination_root = input_root / "TIPS"
    destination_root.mkdir(parents=True, exist_ok=True)
    for name, digest in manifest["tips_2021_source_files"]["sha256"].items():
        destination = destination_root / name
        if destination.is_file():
            verify_file(destination, digest)
            continue
        descriptor, temporary_name = tempfile.mkstemp(
            prefix="rise-tips-", dir=str(destination_root))
        os.close(descriptor)
        temporary = Path(temporary_name)
        try:
            urllib.request.urlretrieve(TIPS_URL.format(name=name), temporary)
            verify_file(temporary, digest)
            temporary.replace(destination)
        finally:
            if temporary.exists():
                temporary.unlink()


def verify_inputs(manifest: dict, input_root: Path) -> tuple[int, int]:
    line_files = manifest["source_files"]["sha256"]
    if len(line_files) != manifest["source_files"]["file_count"] or len(line_files) != 36:
        raise ValueError("HITEMP archive inventory is not the adopted 36-file set")
    for relative, digest in line_files.items():
        verify_file(input_root / relative, digest)
    tips = manifest["tips_2021_source_files"]["sha256"]
    if set(tips) != ({f"1_{index}.QTpy" for index in range(1, 7)} |
                     {f"2_{index}.QTpy" for index in range(1, 13)}):
        raise ValueError("TIPS isotopologue inventory is incomplete")
    for name, digest in tips.items():
        verify_file(input_root / "TIPS" / name, digest)
    outputs = manifest["outputs"]
    if (outputs["h2o"].get("records_read") != "114241164" or
            outputs["h2o"].get("isotopologues") != 6 or
            outputs["co2"].get("records_read") != "326260084" or
            outputs["co2"].get("isotopologues") != 12):
        raise ValueError("derived HITEMP record-count/isotopologue certificate is invalid")
    return len(line_files), len(tips)


def compile_tool(compiler: str, source: Path, output: Path) -> None:
    subprocess.run([compiler, "-O3", "-std=c++17", "-Wall", "-Wextra",
                    "-Wpedantic", str(source), "-o", str(output)], check=True)


def stream_hitemp_to_reducer(manifest: dict, input_root: Path, species: str,
                              reducer: Path, output: Path,
                              visible_output: Path | None = None,
                              visible_tail_output: Path | None = None,
                              visible_band: tuple[float, float] | None = None) -> None:
    molecule = 1 if species == "h2o" else 2
    command = [str(reducer), "--mol", str(molecule), "--out", str(output)]
    if not ((visible_output is None and visible_tail_output is None and visible_band is None) or
            (visible_output is not None and visible_tail_output is not None and
             visible_band is not None)):
        raise ValueError("visible outputs and band must be specified together")
    if (visible_output is not None and visible_tail_output is not None and
            visible_band is not None):
        command.extend(["--visible-out", str(visible_output),
                        "--visible-tail-out", str(visible_tail_output),
                        "--visible-lo", format(visible_band[0], ".12g"),
                        "--visible-hi", format(visible_band[1], ".12g")])
    process = subprocess.Popen(command, stdin=subprocess.PIPE)
    assert process.stdin is not None
    try:
        if species == "h2o":
            paths = sorted(relative for relative in manifest["source_files"]["sha256"]
                           if relative.startswith("H2O/") and relative.endswith(".zip"))
            if len(paths) != 34:
                raise ValueError("HITEMP-2010 H2O archive inventory is not 34 segments")
            for relative in paths:
                with zipfile.ZipFile(input_root / relative) as archive:
                    members = sorted(item for item in archive.namelist()
                                     if item.lower().endswith(".par"))
                    if not members:
                        raise ValueError(f"empty HITEMP archive: {relative}")
                    for member in members:
                        with archive.open(member) as source:
                            shutil.copyfileobj(source, process.stdin, 1024 * 1024)
        else:
            with bz2.open(input_root / "CO2/02_HITEMP2024.par.bz2", "rb") as source:
                shutil.copyfileobj(source, process.stdin, 1024 * 1024)
    finally:
        process.stdin.close()
    if process.wait() != 0:
        raise ValueError(f"HITEMP {species} reduction failed")


def convert_tips(input_root: Path, destination: Path, molecule: int,
                 isotopologues: int) -> None:
    destination.mkdir(parents=True, exist_ok=True)
    for isotopologue in range(1, isotopologues + 1):
        source = input_root / "TIPS" / f"{molecule}_{isotopologue}.QTpy"
        # The pickle is accepted only after its exact public MIT-source digest
        # was verified above.  It contains a string-key/string-value Q(T) map.
        with source.open("rb") as encoded:
            values = pickle.load(encoded)
        rows = sorted((float(key.decode() if isinstance(key, bytes) else key),
                       float(value.decode() if isinstance(value, bytes) else value))
                      for key, value in values.items())
        if len(rows) < 2500 or any(temperature <= 0.0 or partition <= 0.0
                                   for temperature, partition in rows):
            raise ValueError(f"TIPS file is malformed: {source}")
        (destination / f"q_iso{isotopologue}.txt").write_text(
            "".join(f"{temperature:.0f} {partition:.17g}\n"
                    for temperature, partition in rows), encoding="ascii")


def reproduce_and_verify(manifest_path: Path, manifest: dict,
                         input_root: Path) -> None:
    if hashlib.sha256(manifest_path.read_bytes()).hexdigest() != EXPECTED_SOURCE_MANIFEST_SHA256:
        raise ValueError("HITEMP manifest is not the independently adopted identity")
    compiler = shutil.which("c++") or shutil.which("clang++") or shutil.which("g++")
    if compiler is None:
        raise ValueError("a C++17 compiler is required for HITEMP reproduction")
    repository = Path(__file__).resolve().parent.parent
    data = manifest_path.parent
    for relative, expected in manifest["tools"].items():
        verify_file(repository / relative, expected)
    with tempfile.TemporaryDirectory(prefix="rise-hitemp-reproduce-") as directory:
        work = Path(directory)
        reducer = work / "hitemp_reduce"
        planck = work / "hitemp_planck_mean"
        compile_tool(compiler, repository / "tools/hitemp_reduce.cpp", reducer)
        compile_tool(compiler, repository / "tools/hitemp_planck_mean.cpp", planck)
        for species, molecule, isotopologues in (("h2o", 1, 6), ("co2", 2, 12)):
            raw_histogram = work / f"{species}.hist"
            visible_histogram = work / f"{species}_visible.hist"
            visible_tail_histogram = work / f"{species}_visible_tail.hist"
            visible_band = (1.0e7 / 780.0, 1.0e7 / 380.0)
            stream_hitemp_to_reducer(
                manifest, input_root, species, reducer, raw_histogram,
                visible_histogram, visible_tail_histogram, visible_band)
            tips = work / f"tips_{species}"
            convert_tips(input_root, tips, molecule, isotopologues)
            full_prefix = work / f"{species}_full"
            subprocess.run([str(planck), "--hist", str(raw_histogram),
                            "--tips", str(tips), "--out", str(full_prefix),
                            "--tlo", "300", "--thi", "2500", "--tstep", "50",
                            "--prune", "1e-7"], check=True)
            pruned_histogram = Path(f"{full_prefix}_pruned.hist")
            analytic_prefix = work / f"{species}_analytic"
            subprocess.run([str(planck), "--hist", str(pruned_histogram),
                            "--tips", str(tips), "--out", str(analytic_prefix),
                            "--tlo", "300", "--thi", "2500", "--tstep", "50",
                            "--prune", "0"], check=True)
            validation_prefix = work / f"{species}_validation_25K"
            subprocess.run([str(planck), "--hist", str(pruned_histogram),
                            "--tips", str(tips), "--out", str(validation_prefix),
                            "--tlo", "300", "--thi", "2500", "--tstep", "25",
                            "--prune", "0"], check=True)
            visible_certificate = work / f"{species}_visible_certificate.json"
            subprocess.run([
                sys.executable, str(repository / "tools/hitemp_visible_bound.py"),
                str(visible_histogram), str(visible_tail_histogram), str(tips), "--total-table",
                str(Path(f"{full_prefix}_kappaP.txt")), "--species", species.upper(),
                "--band-lo", format(visible_band[0], ".12g"), "--band-hi",
                format(visible_band[1], ".12g"), "--output", str(visible_certificate),
            ], check=True)
            expected_basis = gzip.decompress(
                (data / f"hitemp_spectral_basis_{species}_v1.hist.gz").read_bytes())
            expected_visible = gzip.decompress(
                (data / f"hitemp_visible_basis_{species}_v1.hist.gz").read_bytes())
            expected_visible_tail = gzip.decompress(
                (data / f"hitemp_visible_tail_basis_{species}_v1.hist.gz").read_bytes())
            comparisons = (
                (pruned_histogram.read_bytes(), expected_basis, "pruned basis"),
                (visible_histogram.read_bytes(), expected_visible, "visible basis"),
                (visible_tail_histogram.read_bytes(), expected_visible_tail,
                 "visible Voigt-tail basis"),
                (Path(f"{full_prefix}_kappaP.txt").read_bytes(),
                 (data / f"hitemp_planck_mean_{species}_v1.txt").read_bytes(),
                 "full Planck table"),
                (Path(f"{analytic_prefix}_surface.txt").read_bytes(),
                 (data / f"hitemp_planck_surface_{species}_v1.txt").read_bytes(),
                 "analytic surface"),
                (Path(f"{validation_prefix}_kappaP.txt").read_bytes(),
                 (data / f"hitemp_planck_validation_25K_{species}_v1.txt").read_bytes(),
                 "25 K validation table"),
                (visible_certificate.read_bytes(),
                 (data / f"hitemp_visible_certificate_{species}_v1.json").read_bytes(),
                 "visible certificate"),
            )
            for actual, expected, label in comparisons:
                if actual != expected:
                    raise ValueError(f"reproduced {species} {label} differs from committed bytes")


def main() -> None:
    parser = argparse.ArgumentParser()
    parser.add_argument("--input-root", required=True, type=Path)
    parser.add_argument("--fetch-public-tips", action="store_true")
    parser.add_argument("--record", required=True, type=Path)
    parser.add_argument("--embedded-inc", required=True, type=Path)
    parser.add_argument("manifest", type=Path)
    args = parser.parse_args()
    manifest = json.loads(args.manifest.read_text(encoding="utf-8"))
    if args.fetch_public_tips:
        fetch_tips(manifest, args.input_root)
    line_count, tips_count = verify_inputs(manifest, args.input_root)
    reproduce_and_verify(args.manifest, manifest, args.input_root)
    encoded, include = generate(args.manifest)
    args.record.write_bytes(encoded)
    args.embedded_inc.write_text(include, encoding="utf-8")
    print(f"verified {line_count} owner-local HITEMP archive(s) and "
          f"{tips_count} TIPS isotopologue file(s); generated adopted record")


if __name__ == "__main__":
    main()
