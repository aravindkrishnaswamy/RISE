#!/usr/bin/env python3
"""Build the VDB import process without linking OpenVDB into RISE-GUI."""
import hashlib
import json
import os
from pathlib import Path
import shlex
import shutil
import subprocess

ROOT = Path(__file__).resolve().parents[1]
LIB = ROOT / "src/Library"
SOURCES = [
    LIB / "Simulation/SimulationGrid.cpp",
    LIB / "Simulation/SimulationCache.cpp",
    LIB / "Simulation/SimulationMesh.cpp",  # SimulationCache reads mesh payload references
    LIB / "Utilities/FireSequence.cpp",
    LIB / "Utilities/FireSequenceCache.cpp",
    LIB / "Utilities/FireSimulationRecords.cpp",
    LIB / "Utilities/FireCase.cpp",
    LIB / "Utilities/RISECBOR64.cpp",
    LIB / "Rendering/FrameStore.cpp",
    ROOT / "tools/simulation_cache_import.cpp",
]


def output(*command):
    return subprocess.check_output(command, text=True).strip()


def run(*command):
    subprocess.run(command, check=True)


def dependencies(path):
    # -MT below pins a simple target even when DerivedData contains spaces.
    if not path.is_file():
        return []
    return [Path(f) for f in shlex.split(path.read_text().replace("\\\n", " ").split(":", 1)[1])]


def current(target, inputs):
    return target.exists() and all(p.exists() and p.stat().st_mtime_ns <= target.stat().st_mtime_ns for p in inputs)


def build():
    build_dir = Path(os.environ["DERIVED_FILE_DIR"]) / "simulation-importer"
    app_dir = Path(os.environ["TARGET_BUILD_DIR"]) / os.environ["WRAPPER_NAME"] / "Contents"
    exe = app_dir / "MacOS/rise-cache-import"
    exe.parent.mkdir(parents=True, exist_ok=True)
    archs = os.environ["ARCHS"].split()
    prefix = Path(os.environ.get("HOMEBREW_PREFIX", "/opt/homebrew" if archs[0] == "arm64" else "/usr/local"))
    vdb = Path(os.environ.get("OPENVDB_PREFIX", prefix / "opt/openvdb"))
    tbb = Path(os.environ.get("TBB_PREFIX", prefix / "opt/tbb"))
    blosc = Path(os.environ.get("BLOSC_PREFIX", prefix / "opt/c-blosc"))
    if not (vdb / "include/openvdb/openvdb.h").is_file():
        if os.environ.get("RISE_RELEASE_PACKAGING") == "YES":
            raise SystemExit(f"OpenVDB headers for rise-cache-import are missing at {vdb}")
        print(
            f"note: OpenVDB headers not found at {vdb}; skipping optional rise-cache-import helper "
            f"(install Homebrew openvdb or set OPENVDB_PREFIX to enable VDB import)."
        )
        return
    compiler = output("xcrun", "--find", "clang++")
    sdk = os.environ.get("SDKROOT") or output("xcrun", "--sdk", "macosx", "--show-sdk-path")
    flags = ["-std=c++17", "-O2", "-ffp-contract=off", "-fno-fast-math", "-Wall",
             "-isysroot", sdk, "-mmacosx-version-min=" + os.environ["MACOSX_DEPLOYMENT_TARGET"],
             "-I", str(LIB), "-isystem", str(vdb / "include"), "-isystem", str(tbb / "include"), "-isystem", str(blosc / "include"), "-isystem", str(prefix / "include"),
             "-DRISE_ENABLE_OPENVDB"]
    variants = []
    for arch in archs:
        # A flag or toolchain change gets a fresh dependency graph.
        fingerprint = hashlib.sha256(json.dumps([compiler, output(compiler, "--version"), flags, str(prefix), str(vdb), str(tbb), str(blosc)]).encode()).hexdigest()[:16]
        work = build_dir / arch / fingerprint
        work.mkdir(parents=True, exist_ok=True)
        objects = []
        for source in SOURCES:
            obj = work / (source.stem + ".o")
            dep = obj.with_suffix(".d")
            inputs = dependencies(dep)
            if not inputs or not current(obj, inputs + [Path(__file__)]):
                # Match the certified make build for identity-bearing FireCase arithmetic.
                case_flags = ["-O3", "-ffast-math", "-fno-finite-math-only", "-funroll-loops", "-ffp-contract=fast"] if source.name == "FireCase.cpp" else []
                run(compiler, "-arch", arch, *flags, *case_flags, "-MMD", "-MF", str(dep), "-MT", "object",
                    "-c", str(source), "-o", str(obj))
            objects.append(obj)
        variant = work / "rise-cache-import"
        if not current(variant, objects + [vdb / "lib/libopenvdb.dylib", tbb / "lib/libtbb.dylib", blosc / "lib/libblosc.dylib"]):
            run(compiler, "-arch", arch, *flags, "-Wl,-dead_strip", "-Wl,-headerpad_max_install_names",
                *map(str, objects), "-L" + str(vdb / "lib"), "-L" + str(tbb / "lib"), "-L" + str(blosc / "lib"), "-L" + str(prefix / "lib"),
                "-lopenvdb", "-ltbb", "-lblosc", "-o", str(variant))
        variants.append(variant)
    # Always restore the original install names before release packaging.
    if len(variants) == 1:
        shutil.copy2(variants[0], exe)
    else:
        run("lipo", "-create", *map(str, variants), "-output", str(exe))
    if os.environ.get("RISE_RELEASE_PACKAGING") != "YES":
        # Release builds bundle the GUI and helper together before distribution
        # signing. Development products also carry their own decoder libraries.
        run("bash", str(ROOT / "scripts/bundle_macos_runtime.sh"), str(app_dir.parent),
            str(ROOT / "extlib/oidn/install"), str(prefix), str(exe))
        identity = os.environ.get("EXPANDED_CODE_SIGN_IDENTITY") or "-"
        for library in sorted((app_dir / "Frameworks").glob("*.dylib")):
            run("codesign", "--force", "--sign", identity, "--timestamp=none", str(library))
        run("codesign", "--force", "--sign", identity, "--timestamp=none", str(exe))


if __name__ == "__main__":
    build()
