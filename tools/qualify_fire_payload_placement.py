#!/usr/bin/env python3
"""Record an executed build and owner gate, not an inferred source/binary association."""
import argparse
import hashlib
import json
import os
from pathlib import Path
import subprocess


def sha(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def clean_source(commit):
    subprocess.run(["git", "diff", "--exit-code", commit, "--", "src", "tests", "build", "tools"],
                   check=True, stdout=subprocess.DEVNULL)


def build_command(directory="build/make/rise", target="build-test/FireSequenceTest"):
    return ["make", "-B", "-C", str(directory), "-j8", target]


def build_environment(inherited=None):
    environment = dict(os.environ if inherited is None else inherited)
    # Parent make invocations may export dry-run/touch/question flags or
    # command-line overrides. None may turn qualification into a cache check.
    for name in ("MAKEFLAGS", "MFLAGS", "GNUMAKEFLAGS", "MAKEOVERRIDES"):
        environment.pop(name, None)
    return environment


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("output", type=Path)
    args = parser.parse_args()
    commit = subprocess.check_output(["git", "rev-parse", "HEAD"], text=True).strip()
    clean_source(commit)
    executable = Path("bin/tests/FireSequenceTest")
    # Rebuild every linked object, not just objects older than their sources:
    # ignored caches and prior flag variants are not source attestations.
    commands = [("build", build_command()),
                ("publication", [str(executable.resolve()), "--fire-production-payload-publication"]),
                ("owner", [str(executable.resolve()), "--fire-production-resident-target-metal"])]
    result = {"schema": "rise.fire.executed-build-and-owner-gate.v1", "source_commit": commit, "runs": {}}
    for name, command in commands:
        clean_source(commit)
        log = Path(str(args.output) + "." + name + ".log")
        print("running " + name, flush=True)
        with log.open("xb") as stream:
            completed = subprocess.run(command, stdout=stream, stderr=subprocess.STDOUT,
                                       env=build_environment() if name == "build" else None)
        if completed.returncode:
            raise RuntimeError(name + " failed: " + str(completed.returncode))
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
