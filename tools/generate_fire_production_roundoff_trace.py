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
    if name == "FireProductionForce" and suffix == ".h":
        publication_friend = """\n\t\tfriend bool PublishFireProductionAcceptedManifoldObservation(
\t\t\tdouble,FireProductionResidentStepResult&,
\t\t\tFireProductionAcceptedManifoldObservation&,std::string* );"""
        if text.count(publication_friend) != 1:
            raise RuntimeError("accepted manifold observation friend seam changed")
        text = text.replace(publication_friend, "")
        result_publication_friend = """\n\t\tfriend bool PublishFireProductionAcceptedManifoldObservation(
\t\t\tdouble,
\t\t\tFireProductionResidentStepResult&,
\t\t\tFireProductionAcceptedManifoldObservation&,
\t\t\tstd::string* );"""
        if text.count(result_publication_friend) != 2:
            raise RuntimeError("accepted manifold result friend seam changed")
        text = text.replace(result_publication_friend, "")
        token_begin = text.find("\n\t//! Opaque proof")
        token_end = text.find("\n\t//! Selects the production step", token_begin)
        if token_begin < 0 or token_end < 0:
            raise RuntimeError("accepted manifold token declaration seam changed")
        text = text[:token_begin] + text[token_end:]
        token_field = "\n\t\tFireProductionAcceptedManifoldToken acceptedManifoldToken_;"
        if text.count(token_field) != 1:
            raise RuntimeError("accepted manifold token field seam changed")
        text = text.replace(token_field, "")
        token_accessor = "\n\t\tbool HasAcceptedManifoldToken() const { return acceptedManifoldToken_.Available(); }"
        if text.count(token_accessor) != 1:
            raise RuntimeError("accepted manifold token accessor seam changed")
        text = text.replace(token_accessor, "")
        disposition_begin = text.find(
            "\n\tenum class FireProductionResidentStepAttemptDisposition")
        disposition_end = text.find("\n\t//! Single owner predicate", disposition_begin)
        if disposition_begin < 0 or disposition_end < 0:
            raise RuntimeError("production retry disposition seam changed")
        text = text[:disposition_begin] + text[disposition_end:]
        begin = text.find("\n\t//! Publishes the only manifold metadata")
        end = text.find("\n\t//! Full resident P3 shadow step:", begin)
        if begin < 0 or end < 0:
            raise RuntimeError("accepted manifold publication declaration seam changed")
        text = text[:begin] + text[end:]
    if name == "FireProductionForce" and suffix == ".cpp":
        # Accepted-observation publication is owner lifecycle logic, not part
        # of the arithmetic mirror.  In particular it consumes the completed
        # binary32 result as metadata and must not be scalar-substituted with
        # TraceFloat.  Keep the removal seam exact so source drift fails the
        # generator instead of silently changing the traced DAG.
        begin = text.find("\n\tbool PublishFireProductionAcceptedManifoldObservation(")
        end = text.find("\n\tbool EvaluateFireProductionVremanEddyViscosity(", begin)
        if begin < 0 or end < 0:
            raise RuntimeError("accepted manifold publication seam changed")
        text = text[:begin] + text[end:]
    text = text.replace("namespace RISE", "namespace RISEFireProductionTrace")
    text = text.replace('#include "FireSimulationRecords.h"',
                        '#include "../../src/Library/Utilities/FireSimulationRecords.h"')
    text = text.replace("FireStateProducerPrecision", "RISE::FireStateProducerPrecision")
    text = re.sub(r"\bfloat\b", "FireProductionRoundoffTrace::TraceFloat", text)
    text = text.replace("sizeof(FireProductionRoundoffTrace::TraceFloat)", "sizeof(float)")
    text = re.sub(r"std::(min|max)\(([-+]?[0-9.]+f),",
                  r"std::\1(FireProductionRoundoffTrace::TraceFloat(\2),", text)
    text = text.replace("std::max(0x1p-126f,",
                        "std::max(FireProductionRoundoffTrace::TraceFloat(0x1p-126f),")
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
                "if( FireProductionRoundoffTrace::EvaluateFlatIntegralBranch("
                "left,center,right) )",
            "while( remaining>0.0f )":
                "while( FireProductionRoundoffTrace::EvaluateRemainingPositiveBranch("
                "[&](){ return "
                "remaining>0.0f; }) )",
            "while( remaining>0.0f&&cell<request.lineLength )":
                "while( FireProductionRoundoffTrace::EvaluateRemainingPositiveBranch("
                "[&](){ return "
                "remaining>0.0f; })&&cell<request.lineLength )",
            "if( courant>=0.0f )":
                "if( FireProductionRoundoffTrace::EvaluateCourantSignBranch([&](){ return "
                "courant>=0.0f; }) )",
            "if( fractional>0.0f )":
                "if( FireProductionRoundoffTrace::EvaluateFractionPositiveBranch([&](){ return "
                "fractional>0.0f; }) )",
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
        periodic_profile = ("\t\t\tconst FireProductionRoundoffTrace::TraceFloat count="
                            "static_cast<FireProductionRoundoffTrace::TraceFloat>(request.lineLength);")
        periodic_profile_traced = (
            "\t\t\tconst std::size_t profileBase=ValueIndex(request,component,line,0u);\n"
            "\t\t\tFireProductionRoundoffTrace::TransportProfileScope profileScope(\n"
            "\t\t\t\trequest.values,left,right,profileBase,request.lineLength,0.0f,0.0f);\n" +
            periodic_profile)
        if text.count(periodic_profile) != 1:
            raise RuntimeError("periodic swept profile seam changed")
        text = text.replace(periodic_profile, periodic_profile_traced)
        courant_branch = ("\t\t\tif( FireProductionRoundoffTrace::EvaluateCourantSignBranch("
                           "[&](){ return courant>=0.0f; }) )")
        first_courant = text.find(courant_branch)
        second_courant = text.find(courant_branch, first_courant + 1)
        if first_courant < 0 or second_courant < 0 or text.find(
                courant_branch, second_courant + 1) >= 0:
            raise RuntimeError("open swept profile seam changed")
        open_profile_scope = (
            "\t\t\tconst std::size_t profileBase=ValueIndex(request,component,line,0u);\n"
            "\t\t\tFireProductionRoundoffTrace::TransportProfileScope profileScope(\n"
            "\t\t\t\trequest.values,left,right,profileBase,request.lineLength,\n"
            "\t\t\t\tleftExtension,rightExtension);\n")
        text = text[:second_courant] + open_profile_scope + text[second_courant:]
        periodic_begin = text.find("\t\tFireProductionRoundoffTrace::TraceFloat "
                                   "PeriodicSweptIntegral(")
        periodic_end = text.find("\n\t\tFireProductionRoundoffTrace::TraceFloat "
                                 "OpenLocalForwardIntegral(", periodic_begin)
        open_begin = text.find("\t\tFireProductionRoundoffTrace::TraceFloat "
                               "OpenSweptIntegral(", periodic_end)
        open_end = text.find("\n\tbool ValidateFireProduction", open_begin)
        if min(periodic_begin, periodic_end, open_begin, open_end) < 0:
            raise RuntimeError("swept-integral local envelope regions changed")
        periodic = text[periodic_begin:periodic_end]
        opened = text[open_begin:open_end]
        periodic = periodic.replace("\n\t\t{", "\n\t\t{\n\t\t\t"
            "FireProductionRoundoffTrace::LocalTransportBranchScope branchScope;", 1)
        opened = opened.replace("\n\t\t{", "\n\t\t{\n\t\t\t"
            "FireProductionRoundoffTrace::LocalTransportBranchScope branchScope;", 1)
        if periodic.count("return result;") != 1 or periodic.count("return -result;") != 1:
            raise RuntimeError("periodic swept return topology changed")
        periodic = periodic.replace("return result;",
            "return FireProductionRoundoffTrace::FinalizeTransportBranchEnvelope(result);")
        periodic = periodic.replace("return -result;",
            "return FireProductionRoundoffTrace::FinalizeTransportBranchEnvelope(-result);")
        open_positive = ("return result+OpenLocalForwardIntegral(request,component,line,"
                         "wholeBeginning,\n\t\t\t\t\t0.0f,whole,left,right);")
        open_negative = ("return -(OpenLocalForwardIntegral(request,component,line,face,0.0f,"
                         "\n\t\t\t\tinteriorLength,left,right)+(magnitude-interiorLength)*"
                         "rightExtension);")
        if opened.count(open_positive) != 1 or opened.count(open_negative) != 1:
            raise RuntimeError("open swept return topology changed")
        opened = opened.replace(open_positive,
            "return FireProductionRoundoffTrace::FinalizeTransportBranchEnvelope("
            "result+OpenLocalForwardIntegral(request,component,line,wholeBeginning,\n"
            "\t\t\t\t\t0.0f,whole,left,right));")
        opened = opened.replace(open_negative,
            "return FireProductionRoundoffTrace::FinalizeTransportBranchEnvelope("
            "-(OpenLocalForwardIntegral(request,component,line,face,0.0f,\n"
            "\t\t\t\tinteriorLength,left,right)+(magnitude-interiorLength)*rightExtension));")
        text = text[:periodic_begin] + periodic + text[periodic_end:open_begin] + opened + text[open_end:]
        seam = ("request.faceVelocityMPerS[base]!=request.faceVelocityMPerS[base+"
                "request.lineLength]")
        if text.count(seam) != 1:
            raise RuntimeError("remap periodic seam predicate changed")
        text = text.replace(seam,
            "FireProductionRoundoffTrace::EvaluateBranch("
            "FireProductionRoundoffTrace::BranchSite::PeriodicSeamEquality,"
            "[&](){return request.faceVelocityMPerS[base]!=request.faceVelocityMPerS["
            "base+request.lineLength];})")
        lower_join = "if( signedVelocity<=-width ) return nearest;"
        upper_join = "if( signedVelocity>=width ) return ambient;"
        if text.count(lower_join) != 1 or text.count(upper_join) != 1:
            raise RuntimeError("continuous inflow join seams changed")
        text = text.replace(lower_join,
            "if( FireProductionRoundoffTrace::EvaluateContinuousInflowJoin("
            "signedVelocity,width,nearest,ambient,true) ) return nearest;")
        text = text.replace(upper_join,
            "if( FireProductionRoundoffTrace::EvaluateContinuousInflowJoin("
            "signedVelocity,width,nearest,ambient,false) ) return ambient;")
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
    if name == "FireProductionTransport" and suffix == ".cpp":
        seam = "velocity!=seamVelocity"
        if text.count(seam) != 1:
            raise RuntimeError("periodic carrier seam predicate changed")
        text = text.replace(seam,
            "FireProductionRoundoffTrace::EvaluateBranch("
            "FireProductionRoundoffTrace::BranchSite::PeriodicSeamEquality,"
            "[&](){return velocity!=seamVelocity;})")
        density = "density>0.0f"
        if text.count(density) != 4:
            raise RuntimeError("dual density admissibility predicates changed")
        text = text.replace(density,
            "FireProductionRoundoffTrace::EvaluateBranch("
            "FireProductionRoundoffTrace::BranchSite::AdmissibilityGuard,"
            "[&](){return density>0.0f;})")
        publication_seam = "return first==second;"
        if text.count(publication_seam) != 1:
            raise RuntimeError("periodic dual publication seam predicate changed")
        text = text.replace(publication_seam,
            "return FireProductionRoundoffTrace::EvaluateBranch("
            "FireProductionRoundoffTrace::BranchSite::PeriodicSeamEquality,"
            "[&](){return first==second;});")
    if name == "FireProductionProjection" and suffix == ".cpp":
        negative_maximum = "maximumResidualPerS<0.0f"
        if text.count(negative_maximum) != 2:
            raise RuntimeError("projection nonnegative-reduction guards changed")
        text = text.replace(negative_maximum,
            "FireProductionRoundoffTrace::EvaluateNonnegativeReductionGuard("
            "maximumResidualPerS)")
        negative_velocity_maximum = "maximumVelocityMPerS<0.0f"
        if text.count(negative_velocity_maximum) != 1:
            raise RuntimeError("projection velocity nonnegative-reduction guard changed")
        text = text.replace(negative_velocity_maximum,
            "FireProductionRoundoffTrace::EvaluateNonnegativeReductionGuard("
            "maximumVelocityMPerS)")
        band = "maximumResidualPerS<=tolerance"
        if text.count(band) != 2:
            raise RuntimeError("projection validation predicates changed")
        text = text.replace(band,
            "FireProductionRoundoffTrace::EvaluateBranch("
            "FireProductionRoundoffTrace::BranchSite::ProjectionValidationBand,"
            "[&](){return maximumResidualPerS<=tolerance;})")
        inflow = "outward<0.0f"
        if text.count(inflow) != 1:
            raise RuntimeError("projection open active-set predicate changed")
        text = text.replace(inflow,
            "FireProductionRoundoffTrace::EvaluateBranch("
            "FireProductionRoundoffTrace::BranchSite::OpenBoundaryActiveSet,"
            "[&](){return outward<0.0f;})")
        interpolation = ("\t\tvoid InterpolationCoordinate( std::size_t fine, "
                         "std::size_t fineExtent,\n\t\t\tstd::size_t coarseExtent, "
                         "std::size_t& first, std::size_t& second, "
                         "FireProductionRoundoffTrace::TraceFloat& weight )\n\t\t{")
        if text.count(interpolation) != 1:
            raise RuntimeError("projection interpolation topology context changed")
        text = text.replace(interpolation,interpolation+
            "\n\t\t\tFireProductionRoundoffTrace::ProjectionInterpolationScope "
            "topologyScope(fine,fineExtent,coarseExtent);")
        cycle_loop = ("\t\tfor( unsigned int cycle=0;cycle<cycleCount;++cycle ) {\n"
                      "\t\t\tVCycle(hierarchy,0u,request.boundary,nullspace,\n"
                      "\t\t\t\tresult.executedJacobiSweepCount);\n"
                      "\t\t\t++result.executedVCycleCount;\n"
                      "\t\t\tif( nullspace ) RemoveMean(hierarchy[0].pressure);\n"
                      "\t\t}")
        if text.count(cycle_loop) != 1:
            raise RuntimeError("projection multigrid-cycle proof scope changed")
        indented_cycle = "\t" + cycle_loop.replace("\n", "\n\t")
        text = text.replace(cycle_loop,
            "\t\t{\n\t\t\tFireProductionRoundoffTrace::ProjectionSolveDependencyScope "
            "solveScope;\n" + indented_cycle + "\n\t\t}")
    if suffix == ".h":
        guards = {"FireProductionAdvection": "FIREPRODUCTIONADVECTION_",
                  "FireProductionProjection": "FIREPRODUCTIONPROJECTION_",
                  "FireProductionTransport": "FIREPRODUCTIONTRANSPORT_",
                  "FireProductionForce": "FIRE_PRODUCTION_FORCE_H"}
        text = text.replace(guards[name], "TRACE_" + guards[name])
    if name == "FireProductionForce" and suffix == ".cpp":
        digest_word = ("std::uint32_t bits=0u;\n"
                       "\t\t\t\tstd::memcpy(&bits,&value,sizeof(bits));")
        if text.count(digest_word) != 1:
            raise RuntimeError("momentum diagnostic word seam changed")
        text = text.replace(digest_word,
            "std::uint32_t bits=0u;\n"
            "\t\t\t\tconst float rounded=value.Rounded();"
            "std::memcpy(&bits,&rounded,sizeof(bits));")
    if name == "FireProductionTransport" and suffix == ".cpp":
        cell_loop = ("for( unsigned int pass=0u;pass<5u;++pass )\n"
                     "\t\t\t\tif( !ApplyAxis(request,axes[pass],steps[pass],values,error) ) return false;")
        traced_cell_loop = ("for( unsigned int pass=0u;pass<5u;++pass ) {\n"
                            "\t\t\t\tif( !ApplyAxis(request,axes[pass],steps[pass],values,error) ) return false;\n"
                            "\t\t\t\tFireProductionRoundoffTrace::SealCellStageAndReset(values,"
                            "request.componentCount,request.shape.CellCount());\n"
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
                            "\t\t\t\t\tFireProductionRoundoffTrace::SealDualStageAndReset(\n"
                            "\t\t\t\t\t\tcomputed.auxiliaryFaceDensity[component],computed.momentum[component],component);\n"
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
    support = (("TraceAdapter", ROOT / "tests" / "FireProductionRoundoffTraceAdapter.h"),
               ("TraceCore", ROOT / "tests" / "FireProductionRoundoffTrace.h"),
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
