#!/usr/bin/env python3
"""Exact quantile/invalid-input checks; these do not qualify a solver."""
import unittest
from report_fire_r212_cost import stats


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


if __name__ == "__main__":
    unittest.main()
