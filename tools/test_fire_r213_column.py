#!/usr/bin/env python3
"""Synthetic parser fixtures only; never production evidence."""
import hashlib
import json
from pathlib import Path
import tempfile
import unittest
from unittest.mock import patch

import report_fire_r213_column as column
from fire_payload_merkle import merkle


class ColumnTest(unittest.TestCase):
    def test_equal_time_and_refusal_surface(self):
        with tempfile.TemporaryDirectory(prefix="rise-r213-synthetic-") as temporary:
            root = Path(temporary)
            (root / "checkpoints").mkdir()

            def publish(path, raw):
                if isinstance(raw, str):
                    raw = raw.encode()
                path.write_bytes(raw)
                path.with_name(path.name + ".payload-v2.json").write_text(json.dumps({
                    "sha256_v1": hashlib.sha256(raw).hexdigest(), "v2": merkle(raw)}))

            dt = (column.END - column.BEGIN) / 8
            header = "beginning_time_s,dt_s,x,y,z_face,advection_rate\n"
            production_raw = header + "".join(f"{column.BEGIN!r},{8*dt!r},30,33,{z},20\n" for z in range(107))
            production = root / "synthetic_production.csv"
            production.write_text(production_raw)
            schedule = "substep,beginning_time_s,end_time_s,dt_s,checkpoint_sha256\n"
            time = column.BEGIN
            for step in range(1, 9):
                raw = f"synthetic checkpoint {step}".encode()
                publish(root / "checkpoints" / f"step_{step:02d}.checkpoint", raw)
                schedule += f"{step},{time!r},{time+dt!r},{dt!r},{hashlib.sha256(raw).hexdigest()}\n"
                base = root / f"attempt_{805+step}_0.budget.csv.column.csv"
                publish(base, header + "".join(f"{time!r},{dt!r},30,33,{z},10\n" for z in range(107)))
                publish(root / f"attempt_{805+step}_0.source_packets.bin", b"synthetic source")
                time += dt
            publish(root / "accepted_schedule.csv", schedule)
            publish(root / "composition_request.v1", "synthetic parser request; no solver authority\n")
            publish(root / "composition_summary.v1", "solver_accepted 1\nexact_endpoint_and_schedule_valid 1\n")
            with patch.object(column, "PRODUCTION_COLUMN_SHA", hashlib.sha256(production_raw.encode()).hexdigest()):
                result = column.report(root, production)
                self.assertEqual(result["oracle_column_max_abs"], 10)
                self.assertEqual(result["production_over_oracle_max_abs"], 2)
                self.assertTrue(result["formal_three_way_verdict"].startswith("pending"))
                # Re-seal each mutant: metadata consistency alone cannot waive
                # the exact-time, spatial, and unique-budget contracts.
                for mutant in (schedule.replace(repr(column.END), repr(column.END + 0.001)),
                               schedule + schedule.splitlines()[-1] + "\n"):
                    publish(root / "accepted_schedule.csv", mutant)
                    with self.assertRaises(ValueError):
                        column.report(root, production)
                publish(root / "accepted_schedule.csv", schedule)
                budget = root / "attempt_806_0.budget.csv.column.csv"
                original = budget.read_bytes()
                for mutant in (original.replace(b",30,33,", b",38,42,"),
                               original + original.splitlines()[-1] + b"\n"):
                    publish(budget, mutant)
                    with self.assertRaises(ValueError):
                        column.report(root, production)
                publish(budget, original)
                publish(root / "attempt_806_1.budget.csv.column.csv", original)
                with self.assertRaises(ValueError):
                    column.report(root, production)


if __name__ == "__main__":
    unittest.main()
