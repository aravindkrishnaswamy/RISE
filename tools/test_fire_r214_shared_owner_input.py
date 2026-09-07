#!/usr/bin/env python3
"""CPU-only capture/mapping qualification; never executes a Metal entry point.

The adapter/test and their real CPU implementations are rebuilt with ASan/UBSan.
No temporal/spatial/precision allowance or accepted flow state is manufactured.
"""
import argparse
import hashlib
from pathlib import Path
import shutil
import subprocess
import tempfile


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.parse_args()
    root = Path(__file__).resolve().parents[1]
    source = root / "tests/FireProductionSharedOwnerInputTest.cc"
    header = root / "tests/FireProductionSharedOwnerInput.h"
    subprocess.run(["python3", str(root / "tools/generate_fire_production_fp64_mirror.py"), "--check"], check=True)
    for bound in [source, header, Path(__file__), root / "tests/fire_production_fp64/SourceManifest.h"]:
        print(f"SHARED_OWNER_INPUT_SOURCE path={bound.relative_to(root)} sha256={hashlib.sha256(bound.read_bytes()).hexdigest()}", flush=True)
    implementations = ["FireProductionTransport", "FireProductionForce",
                       "FireProductionProjection", "FireProductionAdvection",
                       "FireProductionSource", "FireSimulationRecords", "FireCase",
                       "RISECBOR64", "FireOptics"]
    sources = [root / "src/Library/Utilities" / (name + ".cpp")
               for name in implementations]
    sources += [root / "tests/fire_production_fp64" / (name + ".cpp")
                for name in implementations[:4]]
    original = header.read_text()
    mutants = [
        ("omit_eligibility", "Numbers(bytes,source.eligibilityBeginningConservativeValues);", ""),
        ("omit_contact_control", "Octets(bytes,source.sourceBoundaryContactMask);", ""),
        ("omit_mixing_control", "Numbers(bytes,source.mixingTimeS);", ""),
        ("wrong_gravity", "mirror.forceContract.gravityMPerS2[axis]=live.gravityMPerS2[axis];",
         "mirror.forceContract.gravityMPerS2[axis]=0.0;"),
    ]
    # Temporary tree symlinks keep real headers/generation intact. Only the
    # adapter is copied/mutated; CPU implementations retain their exact source.
    with tempfile.TemporaryDirectory(prefix="rise-r214-shared-input-") as directory:
        temporary = Path(directory)
        (temporary / "tests").mkdir()
        (temporary / "src").symlink_to(root / "src", target_is_directory=True)
        (temporary / "tests/fire_production_fp64").symlink_to(
            root / "tests/fire_production_fp64", target_is_directory=True)
        fixture = temporary / "tests" / source.name
        shutil.copyfile(source, fixture)
        variant_header = temporary / "tests" / header.name
        flags = ["c++", "-std=c++17", "-O1", "-g", "-fno-fast-math",
                 "-ffp-contract=off", "-Wall", "-pedantic", "-Werror",
                 "-fsanitize=address,undefined", "-I", str(root / "src/Library")]
        objects = []
        for index, implementation in enumerate(sources):
            compiled = temporary / f"implementation_{index}.o"
            subprocess.run([*flags, "-c", str(implementation), "-o", str(compiled)], check=True)
            objects.append(compiled)
        for name, before, after in [("green", "", "")] + mutants:
            text = original
            if before:
                if text.count(before) != 1:
                    raise RuntimeError(f"mutant anchor changed: {name}")
                text = text.replace(before, after)
            variant_header.write_text(text)
            executable = temporary / name
            command = [*flags, str(fixture),
                       *map(str, objects), "-Wl,-dead_strip", "-o", str(executable)]
            subprocess.run(command, check=True)
            print(f"SHARED_OWNER_INPUT_EXECUTABLE name={name} sha256={hashlib.sha256(executable.read_bytes()).hexdigest()}", flush=True)
            result = subprocess.run([str(executable)], text=True, capture_output=True)
            print(result.stdout, end="")
            print(result.stderr, end="")
            expected = 0 if name == "green" else 1
            if result.returncode != expected or "runtime error:" in result.stderr or \
                    "AddressSanitizer" in result.stderr:
                raise RuntimeError(f"{name}: expected exit {expected}, got {result.returncode}")
            print(f"SHARED_OWNER_INPUT_MUTANT name={name} expected_exit={expected} passed=1")


if __name__ == "__main__":
    main()
