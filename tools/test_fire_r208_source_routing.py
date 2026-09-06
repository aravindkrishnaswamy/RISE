#!/usr/bin/env python3
"""Structural REDs for the live source-owner fork (not a numerical oracle)."""

import re
import unittest
from pathlib import Path


SOURCE = Path(__file__).resolve().parents[1] / "tests/FireSequenceTest.cpp"


def body_after(text, marker):
    start = text.index(marker) + len(marker)
    start = text.index("{", start)
    depth = 1
    end = start + 1
    while depth:
        if text[end] == "{":
            depth += 1
        elif text[end] == "}":
            depth -= 1
        end += 1
    return text[start + 1:end - 1], end


def routing_is_single_source(text):
    try:
        start = text.index('solverPhase="production source packet construction";')
        end = text.index("profileSourceMS=", start)
        fork = text[start:end]
        code = re.sub(r"//[^\n]*|/\*.*?\*/", "", fork, flags=re.S)
        marker = "if(persistence.UsesProjectedHeunOwner())"
        canonical, end_owner = body_after(code, marker)
        before = code[:code.index(marker)]
        if "BuildFrozenMethaneSourcePackets(" in before:
            return False
        if (canonical.count("FireProductionCanonicalSourceAuthority::Build(") != 1
                or canonical.count("CarryCanonicalSourceForPersistence(") != 1
                or "BuildFrozenMethaneSourcePackets(" in canonical):
            return False
        if not code[end_owner:].lstrip().startswith("else if(persistence.forceZeroSourceForTest)"):
            return False
        request_start = text.index('solverPhase="production resident request construction";', end)
        request_end = text.index("profileLayoutMS=", request_start)
        carry = text[request_start:request_end]
        expected = (r"if\(persistence\.UsesProjectedHeunOwner\(\)\)\s*"
                    r"request\.cellSourceIncrement=projectedHeunSource\.SourceDelta\(\);\s*else ")
        return (re.search(expected, carry) is not None
                and "FireProductionCanonicalSourceAuthority::Build(" not in carry
                and "BuildFrozenMethaneSourcePackets(" not in carry)
    except (ValueError, IndexError):
        return False


class SourceRouting(unittest.TestCase):
    def test_live_owner_has_one_constructor_and_exact_carry(self):
        self.assertTrue(routing_is_single_source(SOURCE.read_text()))

    def test_second_constructor_cannot_enter_owner_branch(self):
        source = SOURCE.read_text()
        marker = 'solverPhase="production source packet construction";'
        for insertion in (marker, "if(persistence.UsesProjectedHeunOwner()){"):
            changed = source.replace(insertion, insertion + "\nBuildFrozenMethaneSourcePackets();\n", 1)
            # The second insertion must really alter the owner branch, not miss its marker.
            self.assertNotEqual(changed, source)
            self.assertFalse(routing_is_single_source(changed))

    def test_reconstructed_dose_and_unconditional_legacy_fork_refused(self):
        source = SOURCE.read_text()
        for old, new in (("request.cellSourceIncrement=projectedHeunSource.SourceDelta();",
                          "request.cellSourceIncrement=ReconstructSource();"),
                         ("}else if(persistence.forceZeroSourceForTest){", "}if(persistence.forceZeroSourceForTest){")):
            changed = source.replace(old, new)
            self.assertNotEqual(changed, source)
            self.assertFalse(routing_is_single_source(changed))


if __name__ == "__main__":
    unittest.main()
