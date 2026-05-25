#!/usr/bin/env python3
"""Generate src/Library/Utilities/Color/RGBToSpectrumTable_LUTData.cpp
from a `.coeff` binary produced by tools/JakobHanikaLUTGen.cpp.

Bakes the LUT body floats directly into a static const array so the
runtime no longer needs to find the .coeff on disk (which fails when
the GUI is launched from a shortcut without RISE_MEDIA_PATH set, see
GUI Render Engine for the regression that motivated this).

Default target is `rec709` (matches the 2026-05 colour-space
migration default).  Pass `--target {rec709|romm|acescg}` to bake a
different target's LUT.  The output `.cpp` file uses a target-
agnostic symbol set (`kSpectrumLUTFloats`, `kSpectrumLUTResolution`,
`kSpectrumLUTNumFloats`) so swapping targets is a one-line change
to the build.

End-to-end regeneration recipe (run from project root):
  1. Build LUT generator:
       Mac/Linux:  c++ -O3 -std=c++17 -o bin/tools/JakobHanikaLUTGen \\
                       tools/JakobHanikaLUTGen.cpp -lm
       Windows:    msbuild build/VS2022/Tools/JakobHanikaLUTGen.vcxproj \\
                       /p:Configuration=Release /p:Platform=x64
  2. Regenerate the binary LUT (~30-60 sec):
       ./bin/tools/JakobHanikaLUTGen \\
           --target rec709 \\
           --output extlib/jakob-hanika-luts/rec709.coeff
  3. Run this script (writes the .cpp inline as a static array):
       python tools/GenerateSpectrumLUTHeader.py --target rec709
  4. Rebuild Library — `Filelist`, `rise_sources.cmake`,
     `Library.vcxproj`, `Library.vcxproj.filters`, and
     `rise.xcodeproj` register the .cpp + .h pair under the target-
     agnostic name `RGBToSpectrumTable_LUTData.*`.

The file format produced by tools/JakobHanikaLUTGen.cpp:
  Header  : 20 bytes  ('RJHL', uint32 version, res, nChannels, nCoeffs)
  Body    : 3 * res^3 * nCoeffs floats (little-endian fp32)

We bake just the body — resolution and version are baked as
compile-time constants alongside.

Output cpp file is large (~32 MB at 64-resolution LUT) but compiles
under a minute on MSVC / clang.  Lives in the same directory as
RGBToSpectrumTable.cpp so MSVC's /MP-parallel build pipelines it
cleanly.

Paths INPUT and OUTPUT below are relative to the current working
directory — this script must be run from the project root.
"""
import argparse
import struct
import sys
import os

KNOWN_TARGETS = ("rec709", "romm", "acescg")
OUTPUT = "src/Library/Utilities/Color/RGBToSpectrumTable_LUTData.cpp"
FLOATS_PER_LINE = 8


def main():
    parser = argparse.ArgumentParser(description=__doc__.split("\n\n", 1)[0])
    parser.add_argument(
        "--target",
        default="rec709",
        choices=KNOWN_TARGETS,
        help=("target colour space the LUT was trained against. "
              "Picks the input .coeff path under extlib/jakob-hanika-luts/. "
              "Default 'rec709' matches the runtime's RISEPel = Rec709RGBPel."),
    )
    parser.add_argument(
        "--input",
        default=None,
        help=("override the input .coeff path. "
              "Default 'extlib/jakob-hanika-luts/{target}.coeff'."),
    )
    parser.add_argument(
        "--output",
        default=OUTPUT,
        help=f"override the output .cpp path.  Default '{OUTPUT}'.",
    )
    args = parser.parse_args()

    input_path = args.input or f"extlib/jakob-hanika-luts/{args.target}.coeff"
    output_path = args.output

    # Stage A runtime support: `RGBToSpectrumTable::operator()` does its
    # boundary conversion into Rec709RGBPel.  Baking a LUT whose target
    # isn't `rec709` would produce a runtime mismatch (operator() would
    # feed Rec.709-typed inputs into AP1/ROMM-trained cells), giving
    # wrong colours everywhere.  When AP1 / ROMM support is needed,
    # extend RGBToSpectrumTable.cpp so the boundary conversion type is
    # selected at compile time based on which LUT was baked, then loosen
    # this guard.  Keeping it as a hard error so the next person doesn't
    # silently misbake.
    if args.target != "rec709":
        print(f"ERROR: baking '{args.target}' is currently unsupported. "
              "Runtime RGBToSpectrumTable.cpp converts callers into Rec.709 "
              "before lookup; mixing the LUT target produces silently-wrong "
              "colour.  See the comment in tools/GenerateSpectrumLUTHeader.py.",
              file=sys.stderr)
        sys.exit(1)

    if not os.path.exists(input_path):
        print(f"ERROR: missing {input_path}", file=sys.stderr)
        sys.exit(1)

    with open(input_path, "rb") as f:
        blob = f.read()

    if len(blob) < 20:
        print("ERROR: LUT file truncated header", file=sys.stderr)
        sys.exit(1)

    magic = blob[0:4]
    if magic != b"RJHL":
        print(f"ERROR: bad magic {magic!r}", file=sys.stderr)
        sys.exit(1)

    version, res, nChannels, nCoeffs = struct.unpack("<IIII", blob[4:20])
    if nChannels != 3 or nCoeffs != 3:
        print(f"ERROR: unsupported channels/coeffs: {nChannels}/{nCoeffs}", file=sys.stderr)
        sys.exit(1)

    body = blob[20:]
    expected_floats = 3 * res * res * res * nCoeffs
    expected_bytes  = expected_floats * 4
    if len(body) != expected_bytes:
        print(f"ERROR: body size mismatch (got {len(body)}, expected {expected_bytes})",
              file=sys.stderr)
        sys.exit(1)

    floats = struct.unpack(f"<{expected_floats}f", body)

    print(f"Read {input_path}: target={args.target} res={res} "
          f"version=0x{version:08x} floats={expected_floats}")

    os.makedirs(os.path.dirname(output_path), exist_ok=True)
    with open(output_path, "w", newline="\n") as out:
        out.write(
"""//////////////////////////////////////////////////////////////////////
//
//  RGBToSpectrumTable_LUTData.cpp - Baked-in Jakob-Hanika sigmoid
//    coefficient LUT.
//
//  AUTO-GENERATED by tools/GenerateSpectrumLUTHeader.py from
//  extlib/jakob-hanika-luts/{TARGET}.coeff (which itself is produced by
//  tools/JakobHanikaLUTGen.cpp).  DO NOT EDIT BY HAND.  Re-run the
//  generator if the LUT is regenerated:
//
//    python tools/GenerateSpectrumLUTHeader.py --target {TARGET}
//
//  Target colour space of the baked LUT data: {TARGET}.  The runtime
//  RGBToSpectrumTable::operator() converts its `RISEPel` argument into
//  the LUT's target colour space at the call boundary — when RISEPel
//  matches the LUT target the conversion collapses to identity.
//
//  Why baked in: the lazy std::call_once-protected loader used to
//  fopen this file at first spectral painter access.  When the
//  Windows GUI was launched from Explorer (no RISE_MEDIA_PATH env
//  var, cwd = bin/), MediaPathLocator could not resolve the relative
//  'extlib/jakob-hanika-luts/*.coeff' path before the call_once
//  latched its failed state, and every spectral painter then
//  silently fell back to a constant 0.5 spectrum.  The embedded
//  data path eliminates the runtime file dependency entirely.
//
//////////////////////////////////////////////////////////////////////

#include "pch.h"
#include "RGBToSpectrumTable_LUTData.h"

namespace RISE
{
""".replace("{TARGET}", args.target))
        out.write(f"    extern const unsigned int kSpectrumLUTResolution = {res}u;\n")
        out.write(f"    extern const unsigned int kSpectrumLUTNumFloats  = {expected_floats}u;\n\n")
        out.write("    extern const float kSpectrumLUTFloats[] = {\n")

        # Write floats compactly: fp32 has ~7 sig figs, so %.9g preserves
        # bit-exact round-trip via std::strtof on every conforming compiler.
        # `0` formatted with `%.9g` prints as the bare token "0" — we have
        # to force a decimal so the C++ `f` suffix attaches to a float
        # literal (`0.0f`), not an integer (`0f` is invalid C++).
        def fmt(v):
            if v != v:  # NaN
                raise ValueError(f"NaN encountered in LUT body — re-generate {input_path}")
            if v == float("inf") or v == float("-inf"):
                raise ValueError(f"inf encountered in LUT body — re-generate {input_path}")
            s = f"{v:.9g}"
            if "." not in s and "e" not in s:
                s += ".0"
            return s + "f"

        for i in range(0, len(floats), FLOATS_PER_LINE):
            chunk = floats[i:i+FLOATS_PER_LINE]
            line = ", ".join(fmt(v) for v in chunk)
            out.write(f"        {line},\n")
        out.write("    };\n")
        out.write("} // namespace RISE\n")

    out_size = os.path.getsize(output_path)
    print(f"Wrote {output_path}: {out_size:,} bytes ({out_size/1024/1024:.1f} MB)")


if __name__ == "__main__":
    main()
