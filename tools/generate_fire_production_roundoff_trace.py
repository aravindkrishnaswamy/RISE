#!/usr/bin/env python3
"""Generate the test-only traced scalar instantiation of production CPU arithmetic."""

from __future__ import annotations

import argparse
import difflib
import hashlib
import pathlib
import re
import sys

ROOT = pathlib.Path(__file__).resolve().parents[1]
SOURCE = ROOT / "src" / "Library" / "Utilities"
DESTINATION = ROOT / "tests" / "fire_production_trace"
NAMES = ("FireProductionAdvection", "FireProductionProjection",
         "FireProductionTransport", "FireProductionForce")


def transform(text: str, name: str, suffix: str) -> str:
    text = text.replace("namespace RISE", "namespace RISEFireProductionTrace")
    text = text.replace('#include "FireSimulationRecords.h"',
                        '#include "../../src/Library/Utilities/FireSimulationRecords.h"')
    text = text.replace("FireStateProducerPrecision", "RISE::FireStateProducerPrecision")
    text = re.sub(r"\bfloat\b", "FireProductionRoundoffTrace::TraceFloat", text)
    text = text.replace("sizeof(FireProductionRoundoffTrace::TraceFloat)", "sizeof(float)")
    text = re.sub(r"std::(min|max)\(([-+]?[0-9.]+f),",
                  r"std::\1(FireProductionRoundoffTrace::TraceFloat(\2),", text)
    if name == "FireProductionAdvection" and suffix == ".cpp":
        # Tag every data-dependent branch in the remap arithmetic before applying
        # site-specific equivalence certificates. These are test-only wrappers;
        # the rounded production predicate and path remain unchanged.
        branch_replacements = {
            "if( im2==center&&im1==center&&ip1==center&&ip2==center )":
                "if( FireProductionRoundoffTrace::EvaluateBranch("
                "FireProductionRoundoffTrace::BranchSite::FlatStencil,[&](){ return "
                "im2==center&&im1==center&&ip1==center&&ip2==center; }) )",
            "if( left==center&&right==center )":
                "if( FireProductionRoundoffTrace::EvaluateBranch("
                "FireProductionRoundoffTrace::BranchSite::FlatIntegral,[&](){ return "
                "left==center&&right==center; }) )",
            "while( remaining>0.0f )":
                "while( FireProductionRoundoffTrace::EvaluateBranch("
                "FireProductionRoundoffTrace::BranchSite::RemainingPositive,[&](){ return "
                "remaining>0.0f; }) )",
            "while( remaining>0.0f&&cell<request.lineLength )":
                "while( FireProductionRoundoffTrace::EvaluateBranch("
                "FireProductionRoundoffTrace::BranchSite::RemainingPositive,[&](){ return "
                "remaining>0.0f; })&&cell<request.lineLength )",
            "if( courant>=0.0f )":
                "if( FireProductionRoundoffTrace::EvaluateBranch("
                "FireProductionRoundoffTrace::BranchSite::CourantNonnegative,[&](){ return "
                "courant>=0.0f; }) )",
            "if( fractional>0.0f )":
                "if( FireProductionRoundoffTrace::EvaluateBranch("
                "FireProductionRoundoffTrace::BranchSite::FractionPositive,[&](){ return "
                "fractional>0.0f; }) )",
            "if( maximumDeviation>0.0f )":
                "if( FireProductionRoundoffTrace::EvaluateBranch("
                "FireProductionRoundoffTrace::BranchSite::LimiterPositive,[&](){ return "
                "maximumDeviation>0.0f; }) )",
            "if( minimumDeviation<0.0f )":
                "if( FireProductionRoundoffTrace::EvaluateBranch("
                "FireProductionRoundoffTrace::BranchSite::LimiterNegative,[&](){ return "
                "minimumDeviation<0.0f; }) )",
            "faceVelocity>0.0f ?":
                "FireProductionRoundoffTrace::EvaluateBranch("
                "FireProductionRoundoffTrace::BranchSite::InflowSign,[&](){ return "
                "faceVelocity>0.0f; }) ?",
            "faceVelocity<0.0f ?":
                "FireProductionRoundoffTrace::EvaluateBranch("
                "FireProductionRoundoffTrace::BranchSite::InflowSign,[&](){ return "
                "faceVelocity<0.0f; }) ?",
        }
        for before, after in branch_replacements.items():
            if before == "if( left==center&&right==center )":
                if text.count(before) != 2:
                    raise RuntimeError("flat integral branch seams changed")
            elif before in ("if( courant>=0.0f )", "if( fractional>0.0f )"):
                if text.count(before) != 2:
                    raise RuntimeError("swept integral branch seams changed: " + before)
            elif text.count(before) != 1:
                raise RuntimeError("remap branch seam changed: " + before)
            text = text.replace(before, after)
        limiter_positive = ("\t\t\t\tif( FireProductionRoundoffTrace::EvaluateBranch("
            "FireProductionRoundoffTrace::BranchSite::LimiterPositive,[&](){ return "
            "maximumDeviation>0.0f; }) ) alpha=std::min(alpha,\n"
            "\t\t\t\t\t(envelopeMaximum-center)/maximumDeviation);")
        limiter_negative = ("\t\t\t\tif( FireProductionRoundoffTrace::EvaluateBranch("
            "FireProductionRoundoffTrace::BranchSite::LimiterNegative,[&](){ return "
            "minimumDeviation<0.0f; }) ) alpha=std::min(alpha,\n"
            "\t\t\t\t\t(center-envelopeMinimum)/(-minimumDeviation));")
        if text.count(limiter_positive) != 1 or text.count(limiter_negative) != 1:
            raise RuntimeError("traced limiter branch seams changed")
        text = text.replace(limiter_positive,
            "\t\t\t\talpha=FireProductionRoundoffTrace::ApplyLimiterBranch(alpha,\n"
            "\t\t\t\t\tenvelopeMaximum,center,maximumDeviation,true);")
        text = text.replace(limiter_negative,
            "\t\t\t\talpha=FireProductionRoundoffTrace::ApplyLimiterBranch(alpha,\n"
            "\t\t\t\t\tenvelopeMinimum,center,minimumDeviation,false);")
        ppm_branch = ("\t\t\tif( quadratic!=0.0f ) {\n"
                      "\t\t\t\tconst FireProductionRoundoffTrace::TraceFloat stationary="
                      "-linear/(2.0f*quadratic);\n"
                      "\t\t\t\tif( stationary>0.0f&&stationary<1.0f ) {\n"
                      "\t\t\t\t\tconst FireProductionRoundoffTrace::TraceFloat value="
                      "(quadratic*stationary+linear)*stationary+leftDeviation;\n"
                      "\t\t\t\t\tminimum=std::min(minimum,value);\n"
                      "\t\t\t\t\tmaximum=std::max(maximum,value);\n"
                      "\t\t\t\t}\n"
                      "\t\t\t}")
        traced_ppm_branch = ("\t\t\tbool quadraticNonzero=false;\n"
                             "\t\t\tconst FireProductionRoundoffTrace::TraceFloat "
                             "endpointMinimum=minimum,endpointMaximum=maximum;\n"
                             "\t\t\tFireProductionRoundoffTrace::BeginPPMBranchEnvelope();\n"
                             "\t\t\t{ FireProductionRoundoffTrace::BranchSiteScope branchScope(\n"
                             "\t\t\t\tFireProductionRoundoffTrace::BranchSite::PPMQuadraticZero);\n"
                             "\t\t\t\tquadraticNonzero=quadratic!=0.0f; }\n"
                             "\t\t\tFireProductionRoundoffTrace::SetPPMQuadraticAmbiguous(\n"
                             "\t\t\t\tFireProductionRoundoffTrace::PPMQuadraticZeroObligationPending());\n"
                             "\t\t\tif( quadraticNonzero ) {\n"
                             "\t\t\t\tFireProductionRoundoffTrace::CoveredBranchScope "
                             "coveredBranch(true,true);\n"
                             "\t\t\t\tconst FireProductionRoundoffTrace::TraceFloat stationary="
                             "-linear/(2.0f*quadratic);\n"
                             "\t\t\t\tconst bool stationaryPositive="
                             "FireProductionRoundoffTrace::EvaluateBranch(\n"
                             "\t\t\t\t\tFireProductionRoundoffTrace::BranchSite::"
                             "PPMStationaryLower,[&](){ return stationary>0.0f; });\n"
                             "\t\t\t\tconst bool stationaryBelowOne=stationaryPositive&&"
                             "FireProductionRoundoffTrace::EvaluateBranch(\n"
                             "\t\t\t\t\tFireProductionRoundoffTrace::BranchSite::"
                             "PPMStationaryUpper,[&](){ return stationary<1.0f; });\n"
                             "\t\t\t\tif( stationaryBelowOne ) {\n"
                             "\t\t\t\t\tconst FireProductionRoundoffTrace::TraceFloat value="
                             "(quadratic*stationary+linear)*stationary+leftDeviation;\n"
                             "\t\t\t\t\tminimum=std::min(minimum,value);\n"
                             "\t\t\t\t\tmaximum=std::max(maximum,value);\n"
                             "\t\t\t\t}\n"
                             "\t\t\t}\n"
                             "\t\t\tFireProductionRoundoffTrace::ApplyPPMQuadraticZeroCertificate(\n"
                             "\t\t\t\tquadratic,linear,endpointMinimum,endpointMaximum,minimum,maximum);")
        if text.count(ppm_branch) != 1:
            raise RuntimeError("PPM quadratic branch seam changed")
        text = text.replace(ppm_branch, traced_ppm_branch)
    if suffix == ".h":
        guards = {"FireProductionAdvection": "FIREPRODUCTIONADVECTION_",
                  "FireProductionProjection": "FIREPRODUCTIONPROJECTION_",
                  "FireProductionTransport": "FIREPRODUCTIONTRANSPORT_",
                  "FireProductionForce": "FIRE_PRODUCTION_FORCE_H"}
        text = text.replace(guards[name], "TRACE_" + guards[name])
    if name == "FireProductionForce" and suffix == ".cpp":
        text = text.replace("std::memcpy(&bits,&value,sizeof(bits));",
            "const float rounded=value.Rounded();std::memcpy(&bits,&rounded,sizeof(bits));")
    if name == "FireProductionTransport" and suffix == ".cpp":
        cell_loop = ("for( unsigned int pass=0u;pass<5u;++pass )\n"
                     "\t\t\t\tif( !ApplyAxis(request,axes[pass],steps[pass],values,error) ) return false;")
        traced_cell_loop = ("for( unsigned int pass=0u;pass<5u;++pass ) {\n"
                            "\t\t\t\tif( !ApplyAxis(request,axes[pass],steps[pass],values,error) ) return false;\n"
                            "\t\t\t\tFireProductionRoundoffTrace::SealStageAndReset(values);\n"
                            "\t\t\t}")
        if text.count(cell_loop) != 1:
            raise RuntimeError("cell palindrome stage seam changed")
        text = text.replace(cell_loop, traced_cell_loop)
        dual_loop = ("for( const unsigned int sweepAxis : axes )\n"
                     "\t\t\t\t\tif( !ApplyDualAxis(request,component,sweepAxis,axisTimeStep[sweepAxis],\n"
                     "\t\t\t\t\t\tcomputed.auxiliaryFaceDensity[component],computed.momentum[component],\n"
                     "\t\t\t\t\t\terror) ) return false;")
        traced_dual_loop = ("for( const unsigned int sweepAxis : axes ) {\n"
                            "\t\t\t\t\tif( !ApplyDualAxis(request,component,sweepAxis,axisTimeStep[sweepAxis],\n"
                            "\t\t\t\t\t\tcomputed.auxiliaryFaceDensity[component],computed.momentum[component],\n"
                            "\t\t\t\t\t\terror) ) return false;\n"
                            "\t\t\t\t\tFireProductionRoundoffTrace::SealStageAndReset(\n"
                            "\t\t\t\t\t\tcomputed.auxiliaryFaceDensity[component],computed.momentum[component]);\n"
                            "\t\t\t\t}")
        if text.count(dual_loop) != 1:
            raise RuntimeError("dual palindrome stage seam changed")
        text = text.replace(dual_loop, traced_dual_loop)
    return ("// GENERATED by tools/generate_fire_production_roundoff_trace.py.\n"
            "// Do not edit; regenerate from the strict production CPU arithmetic.\n"
            '#include "../FireProductionRoundoffTrace.h"\n' + text)


def manifest() -> str:
    lines = ["// GENERATED by tools/generate_fire_production_roundoff_trace.py.\n",
             "#ifndef FIRE_PRODUCTION_TRACE_SOURCE_MANIFEST_H\n",
             "#define FIRE_PRODUCTION_TRACE_SOURCE_MANIFEST_H\n\n",
             "namespace RISEFireProductionTrace { namespace SourceManifest {\n"]
    for name in NAMES:
        for suffix in (".h", ".cpp"):
            digest = hashlib.sha256((SOURCE / (name + suffix)).read_bytes()).hexdigest()
            lines.append(f'inline constexpr const char* {name}{"Header" if suffix == ".h" else "Source"}="{digest}";\n')
    support = (("TraceCore", ROOT / "tests" / "FireProductionRoundoffTrace.h"),
               ("IndependentWalker", ROOT / "tests" / "FireProductionRoundoffWalker.h"))
    for name, path in support:
        lines.append(f'inline constexpr const char* {name}="{hashlib.sha256(path.read_bytes()).hexdigest()}";\n')
    digest = hashlib.sha256(pathlib.Path(__file__).read_bytes()).hexdigest()
    lines.extend([f'inline constexpr const char* Generator="{digest}";\n',
                  "} }\n\n#endif\n"])
    return "".join(lines)


def main() -> int:
    parser = argparse.ArgumentParser();parser.add_argument("--check", action="store_true")
    options = parser.parse_args();failed = False
    if not options.check: DESTINATION.mkdir(parents=True, exist_ok=True)
    for name in NAMES:
        for suffix in (".h", ".cpp"):
            destination = DESTINATION / (name + suffix)
            expected = transform((SOURCE / (name + suffix)).read_text(), name, suffix)
            if options.check:
                actual = destination.read_text() if destination.exists() else ""
                if actual != expected:
                    failed = True
                    sys.stderr.writelines(difflib.unified_diff(actual.splitlines(True),
                        expected.splitlines(True), fromfile=str(destination),
                        tofile=str(SOURCE / (name + suffix)), n=2))
            else: destination.write_text(expected)
    expected = manifest();path = DESTINATION / "SourceManifest.h"
    if options.check:
        if not path.exists() or path.read_text() != expected: failed = True
    else: path.write_text(expected)
    return 1 if failed else 0


if __name__ == "__main__": raise SystemExit(main())
