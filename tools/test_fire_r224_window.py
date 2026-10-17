#!/usr/bin/env python3
"""Native preflight and source-compiled complete-history mutation REDs (no GPU)."""
import os
from pathlib import Path
import re
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]
BINARY = Path(os.environ.get("RISE_R224_TEST_BINARY", ROOT / "bin/tests/FireSequenceTest"))


class FullWindow(unittest.TestCase):
    def test_native_preflight_refuses_overrides_before_output(self):
        cases = [("RISE_FIRE_TEMPORAL_TARGET_S", "0.01"),
                 ("RISE_FIRE_TEMPORAL_TARGET_S", ""),
                 ("RISE_FIRE_TEMPORAL_RESUME", "1"),
                 ("RISE_FIRE_TEMPORAL_RESUME", "0"),
                 ("RISE_FIRE_REFUSAL_CLAUSES", "1"),
                 ("RISE_FIRE_REFUSAL_CLAUSES", ""),
                 ("RISE_FIRE_SINGLE_STAGE_FCT_ONSET", "1"),
                 ("RISE_FIRE_PRODUCTION_STEP_FAILURE", "1")]
        with tempfile.TemporaryDirectory(prefix="rise-r224-preflight-") as directory:
            for index, (name, value) in enumerate(cases):
                output = Path(directory) / str(index)
                environment = {key: item for key, item in os.environ.items()
                               if not key.startswith("RISE_FIRE_")}
                environment[name] = value
                result = subprocess.run([str(BINARY), "--fire-production-temporal-capstone",
                                         "8", "0.0625", str(output)], env=environment,
                                        capture_output=True, timeout=20)
                self.assertEqual(result.returncode, 91, (name, result.stderr.decode()))
                self.assertFalse(output.exists(), name + " touched the output before refusal")

    def test_complete_history_gate_and_each_deleted_clause_mutant(self):
        source = (ROOT / "tests/FireSequenceTest.cpp").read_text()
        start = source.index("\tbool ProductionTemporalHistoriesComplete(")
        end = source.index("\n\tbool ProductionTemporalFramesComplete(", start)
        body = source[start:end]
        fields = ["acceptedTimeStepHistoryS", "monitoredManifoldMaximumHistory",
                  "monitoredManifoldP95History", "monitoredManifoldP50History",
                  "monitoredTailCellHistory", "monitoredTailDrainedVolumeHistoryM3",
                  "productionDeviceHistoryMS", "productionWallHistoryMS"]
        setup = "\n".join("r." + field + ".resize(2);" for field in fields)
        challenges = "\n".join(
            "{auto bad=r;bad." + field + ".pop_back();"
            "if(ProductionTemporalHistoriesComplete(bad))return 2;}" for field in fields)
        prefix = "#include <vector>\n#include <cstddef>\nstruct SolverFrameValues{\n" + \
            "\n".join("std::vector<double> " + field + ";" for field in fields) + "\n};\n"
        main = "\nint main(){SolverFrameValues r;" + \
            "if(ProductionTemporalHistoriesComplete(r))return 1;" + setup + \
            "if(!ProductionTemporalHistoriesComplete(r))return 3;" + challenges + "return 0;}\n"
        variants = {"actual": body, "empty_admitted": body.replace("count>0u&&", "")}
        for field in fields[1:]:
            clause = "result." + field + ".size()==count"
            self.assertEqual(body.count(clause), 1)
            variants[field] = body.replace(clause, "true")
        with tempfile.TemporaryDirectory(prefix="rise-r224-history-") as directory:
            for name, variant in variants.items():
                cpp, executable = Path(directory) / (name + ".cpp"), Path(directory) / name
                cpp.write_text(prefix + variant + main)
                subprocess.run(["clang++", "-std=c++17", "-Wall", "-Wextra", "-Werror",
                                str(cpp), "-o", str(executable)], check=True, capture_output=True)
                result = subprocess.run([str(executable)], capture_output=True)
                self.assertEqual(result.returncode == 0, name == "actual", name)
        # Guard against testing a disconnected helper while the writer still
        # silently takes min(history sizes), the historical failure mode.
        begin = source.index("\tint RunProductionTemporalCapstoneChild(")
        finish = source.index("\n#if defined(RISE_ENABLE_OPENVDB)\n\tbool OwnerCostPrefix", begin)
        caller = source[begin:finish]
        self.assertIn("!ProductionTemporalHistoriesComplete(result))return 98;", caller)
        self.assertNotIn("std::min({result.acceptedTimeStepHistoryS", caller)
        self.assertRegex(caller, re.escape("persistence.productionOnsetDiagnosticDirectory=outputDirectory/\"budgets\";"))

    def test_native_publication_reds(self):
        result = subprocess.run([str(BINARY), "--fire-r224-window-publication-gate"],
                                capture_output=True, timeout=30)
        self.assertEqual(result.returncode, 0, result.stderr.decode())
        self.assertIn("R224_WINDOW_PUBLICATION_RED synthetic_inputs_only=1", result.stderr.decode())
        with tempfile.TemporaryDirectory(prefix="rise-r224-cadence-") as directory:
            output = Path(directory) / "forbidden"
            result = subprocess.run([str(BINARY), "--fire-production-temporal-capstone",
                                     "8", "0.125", str(output)], capture_output=True, timeout=20)
            self.assertEqual(result.returncode, 91, result.stderr.decode())
            self.assertFalse(output.exists())


if __name__ == "__main__":
    unittest.main()
