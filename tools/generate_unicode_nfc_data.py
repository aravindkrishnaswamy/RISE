#!/usr/bin/env python3
"""Generate the compact Unicode tables used by RISE-CBOR64-v1 NFC checks."""

from __future__ import annotations

import argparse
from pathlib import Path


def parse_unicode_data(path: Path):
    combining: dict[int, int] = {}
    decompositions: dict[int, list[int]] = {}
    with path.open("r", encoding="utf-8") as source:
        for line in source:
            fields = line.rstrip("\n").split(";")
            codepoint = int(fields[0], 16)
            ccc = int(fields[3])
            if ccc:
                combining[codepoint] = ccc
            decomposition = fields[5]
            if decomposition and not decomposition.startswith("<"):
                decompositions[codepoint] = [
                    int(value, 16) for value in decomposition.split()
                ]
    return combining, decompositions


def parse_exclusions(path: Path) -> set[int]:
    exclusions: set[int] = set()
    with path.open("r", encoding="utf-8") as source:
        for line in source:
            payload = line.split("#", 1)[0].strip()
            if payload:
                exclusions.add(int(payload, 16))
    return exclusions


def emit(output: Path, combining, decompositions, exclusions) -> None:
    flat: list[int] = []
    decomposition_rows: list[tuple[int, int, int]] = []
    for codepoint, values in sorted(decompositions.items()):
        decomposition_rows.append((codepoint, len(flat), len(values)))
        flat.extend(values)

    composition_rows: list[tuple[int, int, int]] = []
    for composite, values in sorted(decompositions.items()):
        if (
            len(values) == 2
            and composite not in exclusions
            and combining.get(values[0], 0) == 0
        ):
            composition_rows.append((values[0], values[1], composite))
    composition_rows.sort()

    with output.open("w", encoding="utf-8", newline="\n") as target:
        target.write(
            "// Generated from Unicode 17.0.0 data files.\n"
            "// Copyright (c) 1991-2025 Unicode, Inc.\n"
            "// Distributed under the Unicode Data Files and Software License:\n"
            "// https://www.unicode.org/license.txt\n\n"
        )
        target.write("static const CanonicalDecomposition kCanonicalDecompositions[] = {\n")
        for codepoint, offset, length in decomposition_rows:
            target.write(f"\t{{0x{codepoint:x}u,{offset}u,{length}u}},\n")
        target.write("};\n")
        target.write(
            "static const std::size_t kCanonicalDecompositionCount = "
            "sizeof(kCanonicalDecompositions)/sizeof(kCanonicalDecompositions[0]);\n\n"
        )

        target.write("static const std::uint32_t kCanonicalDecompositionData[] = {\n")
        for start in range(0, len(flat), 12):
            values = ",".join(f"0x{value:x}u" for value in flat[start : start + 12])
            target.write(f"\t{values},\n")
        target.write("};\n\n")

        target.write("static const CanonicalCombiningClass kCanonicalCombiningClasses[] = {\n")
        for codepoint, value in sorted(combining.items()):
            target.write(f"\t{{0x{codepoint:x}u,{value}u}},\n")
        target.write("};\n")
        target.write(
            "static const std::size_t kCanonicalCombiningClassCount = "
            "sizeof(kCanonicalCombiningClasses)/sizeof(kCanonicalCombiningClasses[0]);\n\n"
        )

        target.write("static const CanonicalComposition kCanonicalCompositions[] = {\n")
        for first, second, composite in composition_rows:
            target.write(f"\t{{0x{first:x}u,0x{second:x}u,0x{composite:x}u}},\n")
        target.write("};\n")
        target.write(
            "static const std::size_t kCanonicalCompositionCount = "
            "sizeof(kCanonicalCompositions)/sizeof(kCanonicalCompositions[0]);\n"
        )


def main() -> None:
    parser = argparse.ArgumentParser()
    parser.add_argument("unicode_data", type=Path)
    parser.add_argument("composition_exclusions", type=Path)
    parser.add_argument("output", type=Path)
    args = parser.parse_args()
    combining, decompositions = parse_unicode_data(args.unicode_data)
    exclusions = parse_exclusions(args.composition_exclusions)
    emit(args.output, combining, decompositions, exclusions)


if __name__ == "__main__":
    main()
