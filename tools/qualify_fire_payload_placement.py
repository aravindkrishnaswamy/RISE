#!/usr/bin/env python3
"""Record an executed build and owner gate, not an inferred source/binary association."""
import argparse
import hashlib
import json
import os
from pathlib import Path
import subprocess

from seal_fire_payload_placement import gate


def sha(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def clean_source(commit):
    subprocess.run(["git", "diff", "--exit-code", commit, "--"],
                   check=True, stdout=subprocess.DEVNULL)
    # Vendored decoders are linked too, and the trace/mirror source lists use
    # wildcards. Ignored build products are not sources; nonignored additions
    # under compilation roots must be committed before source attestation.
    untracked = subprocess.check_output(["git", "ls-files", "--others", "--exclude-standard",
                                        "-z", "--", "src", "tests", "extlib"])
    if untracked:
        raise ValueError("uncommitted compilation input")
    # Ignore rules do not remove files from make's wildcard inputs or C/C++
    # include lookup. Check source-like ignored files in the source-owned roots
    # too; installed third-party SDK trees are toolchain inputs, not this list.
    hidden = subprocess.check_output(["git", "ls-files", "--others", "-z", "--",
                                      "src", "tests", "extlib/stb", "extlib/cgltf", "build/make/rise"])
    source_suffixes = {".c", ".cc", ".cpp", ".cxx", ".m", ".mm", ".h", ".hh", ".hpp",
                       ".hxx", ".inl", ".inc", ".ipp", ".tpp", ".tcc", ".metal"}
    # The tracked make recipe forcibly regenerates this exact header before
    # Job.o. No other local header in the -I$(CURDIR) root has that authority.
    generated_header = b"build/make/rise/RendererBuildIdentity.generated.h"
    if any(Path(os.fsdecode(path)).suffix.lower() in source_suffixes
           for path in hidden.split(b"\0") if path and path != generated_header):
        raise ValueError("uncommitted ignored compilation input")
    return build_configuration(commit)


def build_configuration(commit):
    # This is the macOS/Metal qualifier. Config.specific is intentionally
    # ignored by git and may be a copy; authenticate its bytes, not its name.
    canonical = "build/make/rise/Config.OSX"
    expected = subprocess.check_output(["git", "show", commit + ":" + canonical])
    selected = Path("build/make/rise/Config.specific")
    if selected.read_bytes() != expected:
        raise ValueError("unqualified local build configuration")
    return {"canonical": canonical, "selected": str(selected),
            "sha256": hashlib.sha256(expected).hexdigest()}


def build_command(directory="build/make/rise", target="build-test/FireSequenceTest"):
    # Do not select an ignored GNUmakefile or import generated dependency
    # snippets as recipes. -B already rebuilds every prerequisite from source.
    return ["make", "-B", "-C", str(directory), "-f", "Makefile", "ALL_DEPS=", "-j8", target]


def build_environment(inherited=None):
    environment = dict(os.environ if inherited is None else inherited)
    # Recipe inputs come from the versioned build configuration, not inherited
    # make/compiler flags. A denylist misses compiler-level dry runs (-###).
    platform_inputs = {"PATH", "HOME", "USER", "LOGNAME", "SHELL", "TMPDIR", "TMP", "TEMP",
                       "SYSTEMROOT", "WINDIR", "COMSPEC", "PATHEXT", "DEVELOPER_DIR", "SDKROOT",
                       "MACOSX_DEPLOYMENT_TARGET", "LANG", "LC_ALL", "LC_CTYPE"}
    return {name: value for name, value in environment.items() if name in platform_inputs}


def validate_run_log(name, log):
    if name == "owner":
        # Exit zero is insufficient: unsupported CLI modes can run the ordinary
        # suite without ever invoking a Metal owner. Require its full verdict.
        gate(log)
    elif name == "publication":
        lines = [line for line in log.read_text().splitlines() if line.startswith("PUBLICATION_RED ")]
        if len(lines) != 1 or any(token not in lines[0].split() for token in (
                "mutable_advance=pass", "interrupted_pair_refused=pass", "immutable_bytes_preserved=pass",
                "final_equivalence_not_mutable=pass", "orphan_refused=pass", "empty_refused=pass",
                "pending_refused=pass", "foreign_case_unchanged=pass", "kernel_library_mutants=5")):
            raise ValueError("missing publication gate verdict")


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("output", type=Path)
    args = parser.parse_args()
    commit = subprocess.check_output(["git", "rev-parse", "HEAD"], text=True).strip()
    configuration = clean_source(commit)
    executable = Path("bin/tests/FireSequenceTest")
    # Rebuild every linked object, not just objects older than their sources:
    # ignored caches and prior flag variants are not source attestations.
    commands = [("build", build_command()),
                ("publication", [str(executable.resolve()), "--fire-production-payload-publication"]),
                ("owner", [str(executable.resolve()), "--fire-production-resident-target-metal"])]
    result = {"schema": "rise.fire.executed-build-and-owner-gate.v1", "source_commit": commit,
              "build_configuration": configuration, "runs": {}}
    for name, command in commands:
        clean_source(commit)
        log = Path(str(args.output) + "." + name + ".log")
        print("running " + name, flush=True)
        with log.open("xb") as stream:
            completed = subprocess.run(command, stdout=stream, stderr=subprocess.STDOUT,
                                       env=build_environment() if name == "build" else None)
        if completed.returncode:
            raise RuntimeError(name + " failed: " + str(completed.returncode))
        validate_run_log(name, log)
        if name == "build":
            if "warning:" in log.read_text():
                raise RuntimeError("build warning gate failed")
            result["producer_executable_sha256"] = sha(executable)
        elif sha(executable) != result["producer_executable_sha256"]:
            raise RuntimeError("executable changed during qualification")
        result["runs"][name] = {"command": command, "exit_code": completed.returncode,
                               "log": str(log), "log_sha256": sha(log)}
    clean_source(commit)
    with args.output.open("x") as stream:
        json.dump(result, stream, sort_keys=True, indent=2, allow_nan=False)
        stream.write("\n")
    print("qualified " + sha(args.output), flush=True)


if __name__ == "__main__":
    main()
