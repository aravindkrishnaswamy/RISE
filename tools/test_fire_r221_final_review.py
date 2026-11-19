#!/usr/bin/env python3
"""Exact evidence and source-compiled orchestration REDs; no GPU or simulation."""
import copy
import csv
import io
import json
from pathlib import Path
import shutil
import struct
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]
SUMMARY = ROOT / "docs/evidence/fire/r221_retained_overlap.v1.json"


def validate_exact_summary(summary, trace, rows):
    for key, column in (("accepted_time_s_bits", "time_s"), ("dt_s_bits", "dt_s")):
        actual = [struct.unpack("<Q", struct.pack("<d", float(row[column])))[0] for row in rows]
        if any(type(value) is not int for value in summary[key]):
            raise ValueError("bit patterns must be exact integers")
        if summary[key] != trace[key] or summary[key] != actual:
            raise ValueError(key + " differs from bridge/raw CSV")


class FinalReview(unittest.TestCase):
    def test_exact_summary_and_rounded_mutant(self):
        summary = json.loads(SUMMARY.read_text())
        bridge_path = next(Path(item["path"]) for item in summary["files"]
                           if item["path"].endswith("/bridge.v2.json"))
        trace = json.loads(bridge_path.read_text())["execution"]["trace"]
        rows = list(csv.DictReader(io.StringIO(Path(trace["csv_path"]).read_text())))
        validate_exact_summary(summary, trace, rows)
        self.assertEqual(summary["accepted_steps"], [int(row["accepted_step"]) for row in rows])
        for key in ("accepted_time_s_bits", "dt_s_bits"):
            self.assertTrue(all(value > 2**53 for value in summary[key]))
            for index in range(9):
                bad = copy.deepcopy(summary)
                bad[key][index] += 1
                with self.assertRaises(ValueError):
                    validate_exact_summary(bad, trace, rows)
        bad = copy.deepcopy(summary)
        bad["accepted_time_s_bits"][0] = 4608738977373487000
        with self.assertRaises(ValueError):
            validate_exact_summary(bad, trace, rows)

    def test_native_orchestration_and_deleted_call_guard_mutants(self):
        source = (ROOT / "tests/FireSequenceTest.cpp").read_text()
        begin = source.index("\tbool RunR221NativeOverlap(")
        end = source.index("\n\tbool R221NativeOverlapREDs(", begin)
        body = source[begin:end]
        producer = ("RunProductionOwnerEOSRefusalChild(checkpointPath,outputDirectory,\n"
                    "\t\t\t\ttrue,false,false,false,false,true)")
        guard = "if(!ResidentExecutedOverlapRowsMatch(csv,certificate,error))return false;"
        self.assertEqual(body.count(producer), 1)
        self.assertEqual(body.count(guard), 1)
        variants = {"actual": body,
                    "deleted_producer": body.replace(producer, "0"),
                    "deleted_mismatch_guard": body.replace(guard,
                        "ResidentExecutedOverlapRowsMatch(csv,certificate,error);")}
        compiler = shutil.which("clang++")
        self.assertIsNotNone(compiler, "orchestration qualification requires clang++")
        with tempfile.TemporaryDirectory(prefix="rise-r221-orchestration-") as temporary:
            root = Path(temporary)
            for name, function in variants.items():
                unit = root / (name + ".cpp")
                unit.write_text(PREAMBLE + function + DRIVER)
                executable = root / name
                built = subprocess.run([compiler, "-std=c++17", "-Wall", "-Wextra", "-Werror",
                                        str(unit), "-o", str(executable)], capture_output=True, text=True)
                self.assertEqual(built.returncode, 0, built.stderr)
                result = subprocess.run([str(executable), str(root / (name + "-output"))],
                                        capture_output=True, text=True)
                expected = {"actual": 0, "deleted_producer": 10, "deleted_mismatch_guard": 11}[name]
                self.assertEqual(result.returncode, expected, name + result.stdout + result.stderr)
                print("R221_ORCHESTRATION", name, "exit", result.returncode)


# Dependency doubles are intentionally not numerical mirrors. The unchanged
# source body above owns all invocation, ordering, failure and receipt control.
PREAMBLE = r'''
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>
namespace RISECBOR64 { using Bytes=std::vector<unsigned char>; }
struct ResumeEquivalenceCertificate {
    std::string newBuildId="build", newExecutableDigest="exe", checkpointDigest="checkpoint";
    unsigned long long acceptedStepCount=9, resumedFromStep=826;
};
int executions=0, comparisons=0, seals=0; bool matching=true; std::string order;
int RunProductionOwnerEOSRefusalChild(const std::filesystem::path&,const std::filesystem::path& output,
    bool,bool,bool,bool,bool,bool) {
    ++executions; order+='E'; std::filesystem::create_directories(output/"budgets");
    std::ofstream csv(output/"budgets/maximum_velocity_trajectory.csv");
    csv<<(matching?"matched":"mismatch"); return 0;
}
bool ResidentExecutedOverlapRowsMatch(std::istream& input,const ResumeEquivalenceCertificate&,std::string&) {
    ++comparisons; order+='C'; std::string row; input>>row; return row=="matched";
}
bool CurrentRendererBuildIdentity(RISECBOR64::Bytes&,std::string& build) { build="build"; return true; }
bool CurrentExecutableDigest(RISECBOR64::Bytes&,std::string& exe,std::string&) { exe="exe"; return true; }
std::string DigestFile(const std::filesystem::path&) { return "checkpoint"; }
bool ClosePublishedStream(std::ofstream& stream) { stream.close(); return bool(stream); }
bool PublishPayloadMerkleSidecar(const std::filesystem::path&,const std::string&,std::string&) {
    ++seals; order+='S'; return true;
}
'''
DRIVER = r'''
int main(int argc,char** argv) {
    if(argc!=2)return 99;
    const std::filesystem::path root=argv[1];
    ResumeEquivalenceCertificate certificate; std::string error;
    // Fresh matching output: removing execution fails this observable witness.
    bool accepted=RunR221NativeOverlap("checkpoint",root/"green/native_overlap",certificate,"cert",error);
    if(!accepted||executions!=1||comparisons!=1||seals!=1||order!="ECS"||
       !std::filesystem::exists(root/"green/native_overlap_verified.v2"))return 10;
    executions=comparisons=seals=0; order.clear(); matching=false;
    // The emitted mismatch must not produce a receipt or authorize onward work.
    accepted=RunR221NativeOverlap("checkpoint",root/"red/native_overlap",certificate,"cert",error);
    if(accepted||executions!=1||comparisons!=1||seals!=0||order!="EC"||
       std::filesystem::exists(root/"red/native_overlap_verified.v2"))return 11;
    return 0;
}
'''

if __name__ == "__main__":
    unittest.main()
