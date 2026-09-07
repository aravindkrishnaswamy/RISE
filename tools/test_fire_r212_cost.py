#!/usr/bin/env python3
"""Exact quantile/invalid-input checks; these do not qualify a solver."""
import unittest
import hashlib
from report_fire_r212_cost import stats, authenticated_input, observer_record


class CostStatistics(unittest.TestCase):
    def test_nearest_rank_and_scope(self):
        result = stats(list(range(1, 21)))
        self.assertEqual(result["p95"], 19)
        self.assertEqual(result["samples"], 20)
        self.assertEqual(result["mean"], 10.5)

    def test_one_sample(self):
        self.assertEqual(stats([3]), dict(samples=1, mean=3, stdev=0, p95=3))

    def test_invalid_samples_refused(self):
        for values in ([], [-1], [float("nan")], [float("inf")]):
            with self.assertRaises(ValueError):
                stats(values)

    def test_changed_wall_or_iteration_and_duplicate_repeat_refused(self):
        first = b"wall_ms,owner_r0_iterations\n20000,5\n"
        second = b"wall_ms,owner_r0_iterations\n21000,6\n"
        expected = {"repeat1": hashlib.sha256(first).hexdigest(),
                    "repeat2": hashlib.sha256(second).hexdigest()}
        self.assertEqual(authenticated_input("repeat1", first, expected), expected["repeat1"])
        for key, raw in (("repeat1", first.replace(b"20000", b"1000000")),
                         ("repeat1", first.replace(b",5", b",0")),
                         ("repeat2", first), ("unrecorded", first)):
            with self.assertRaises(ValueError):
                authenticated_input(key, raw, expected)

    def test_observer_requires_one_executed_success(self):
        line = "OWNER_CONVERGENCE_CROSSING path=fixture terminal_bit_identity=1 diagnostic_wall_ms=123.25 passed=1\n"
        self.assertEqual(observer_record(line), dict(calls=1, wall_ms=123.25,
                                                   included_in_production_rows=False))
        for text in ("", line + line, line.replace("passed=1", "passed=0")):
            with self.assertRaises(ValueError):
                observer_record(text)


if __name__ == "__main__":
    unittest.main()
