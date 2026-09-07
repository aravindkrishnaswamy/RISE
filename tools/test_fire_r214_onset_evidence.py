#!/usr/bin/env python3
"""CPU mutation proofs for onset evidence; no solver or performance claim."""
from pathlib import Path
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]


class OnsetEvidenceMutants(unittest.TestCase):
    def run_header(self, text, main):
        with tempfile.TemporaryDirectory(prefix="rise-r214-evidence-") as directory:
            source = Path(directory) / "evidence.cpp"
            binary = Path(directory) / "evidence"
            source.write_text(text + "\n" + main)
            result = subprocess.run(["clang++", "-std=c++17", "-O2", "-fsanitize=address,undefined", "-Wall", "-Wextra",
                                     "-Werror", str(source), "-o", str(binary)],
                                    capture_output=True, text=True)
            self.assertEqual(result.returncode, 0, result.stderr)
            return subprocess.run([str(binary)], capture_output=True).returncode

    def staggered(self, old=None, new=None):
        text = (ROOT / "tests/FireProductionStaggeredColumnBudget.h").read_text()
        if old is not None:
            self.assertEqual(text.count(old), 1)
            text = text.replace(old, new)
        return self.run_header(text, "int main(){return FireProductionStaggeredColumn::Fixture()?0:1;}")

    def test_staggered_fields_green(self):
        self.assertEqual(self.staggered(), 0)

    def test_momentum_term_swap_mutant_red(self):
        self.assertEqual(self.staggered(
            "row.stress=input.stress.Value(axis,row.face)",
            "row.stress=input.buoyancy.Value(axis,row.face)"), 1)

    def test_horizontal_omission_mutant_red(self):
        self.assertEqual(self.staggered(
            "if(!append(0u,columnX,columnY,z)||!append(0u,columnX+1u,columnY,z)||",
            "if(!append(1u,columnX,columnY,z)||!append(1u,columnX,columnY,z)||"), 1)

    def test_boundary_neighbor_mutant_red(self):
        self.assertEqual(self.staggered(
            "upper[axis]=std::min(upper[axis],dimensions[axis]-1u);",
            "upper[axis]=lower[axis];"), 1)

    def test_horizon_crossing_mutant_red(self):
        text = (ROOT / "tests/FireProductionOnsetEvidence.h").read_text()
        main = "int main(){return FireProductionOnsetEvidence::Survived(true,true)?1:0;}"
        self.assertEqual(self.run_header(text, main), 0)
        old = "return reachedHorizon&&!crossed;"
        self.assertEqual(text.count(old), 1)
        # Use both arguments so the mutation is a logic failure, not a warning.
        self.assertEqual(self.run_header(text.replace(old, "return reachedHorizon||crossed;"), main), 1)


if __name__ == "__main__":
    unittest.main()
