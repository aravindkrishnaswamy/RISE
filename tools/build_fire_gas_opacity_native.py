#!/usr/bin/env python3
"""Build the optional production HITEMP native accumulator."""

from __future__ import annotations

import argparse
import os
import subprocess
from pathlib import Path


def build(output: Path, compiler: str | None = None) -> None:
    source = Path(__file__).with_name("fire_gas_opacity_native.cpp")
    output.parent.mkdir(parents=True, exist_ok=True)
    if os.name == "nt":
        command = [compiler or "cl", "/nologo", "/std:c++17", "/O2", "/LD",
                   str(source), f"/Fe:{output}"]
    else:
        command = [compiler or os.environ.get("CXX", "c++"), "-std=c++17", "-O3",
                   "-DNDEBUG", "-Wall", "-Wextra", "-Werror", "-shared", "-fPIC",
                   str(source), "-o", str(output)]
    subprocess.run(command, check=True)


def main() -> None:
    parser = argparse.ArgumentParser()
    parser.add_argument("--compiler")
    parser.add_argument("output", type=Path)
    args = parser.parse_args()
    build(args.output, args.compiler)


if __name__ == "__main__":
    main()
