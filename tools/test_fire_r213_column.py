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
                    "schema": "rise.fire.published-payload.v2",
                    "case_record_id": column.REQUEST_FIXED["case_record_id"],
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
            request = dict(column.REQUEST_FIXED, beginning_time_s=repr(column.BEGIN),
                           end_time_s=repr(column.END), subdivision_nomination="8", maximum_substep_s=repr(dt))
            request_raw = "".join(f"{k} {v}\n" for k, v in request.items())
            summary = {"schema": "rise.fire.r213.oracle-composition-summary.v1", "solver_accepted": "1",
                       "exact_endpoint_and_schedule_valid": "1", "end_time_s": repr(column.END), "wall_s": "1",
                       "schedule_sha256": hashlib.sha256(schedule.encode()).hexdigest(),
                       "formal_contract_verdict": column.REQUEST_FIXED["formal_contract_verdict"], "error": ""}
            summary_raw = "".join(f"{k} {v}\n" for k, v in summary.items())
            publish(root / "composition_request.v1", request_raw)
            publish(root / "composition_summary.v1", summary_raw)
            with patch.object(column, "PRODUCTION_COLUMN_SHA", hashlib.sha256(production_raw.encode()).hexdigest()):
                result = column.report(root, production)
                self.assertEqual(result["oracle_column_max_abs"], 10)
                self.assertEqual(result["production_over_oracle_max_abs"], 2)
                self.assertTrue(result["formal_three_way_verdict"].startswith("pending"))
                self.assertEqual(result["same_production_face_magnitude_ratio"], 2)
                # An unrelated high oracle face can shrink a maximum ratio;
                # it must not hide the discrepancy at production's own face.
                altered_budgets = []
                for step in range(1, 9):
                    budget = root / f"attempt_{805+step}_0.budget.csv.column.csv"
                    original = budget.read_bytes()
                    altered_budgets.append((budget, original))
                    publish(budget, original.replace(b",106,10\n", b",106,100\n"))
                separated = column.report(root, production)
                self.assertEqual(separated["production_over_oracle_max_abs"], 0.2)
                self.assertEqual(separated["same_production_face_magnitude_ratio"], 2)
                self.assertEqual(separated["oracle_maximum_face_z"], 106)
                self.assertEqual(separated["production_maximum_face_z"], 0)
                self.assertTrue(separated["formal_three_way_verdict"].startswith("pending"))
                for budget, original in altered_budgets:
                    publish(budget, original)
                seal_path = root / "composition_request.v1.payload-v2.json"
                seal_raw = seal_path.read_bytes()
                seal = json.loads(seal_raw)
                for key in ("schema", "case_record_id", "sha256_v1"):
                    seal_path.write_text(json.dumps(dict(seal, **{key: "wrong"})))
                    with self.assertRaises(ValueError):
                        column.report(root, production)
                seal_path.write_bytes(b'{"schema":"duplicate",' + seal_raw[1:])
                with self.assertRaises(ValueError):
                    column.report(root, production)
                seal_path.write_bytes(seal_raw)
                for key in request:
                    mutated = dict(request, **{key: "wrong"})
                    publish(root / "composition_request.v1", "".join(f"{k} {v}\n" for k, v in mutated.items()))
                    with self.assertRaises(ValueError):
                        column.report(root, production)
                publish(root / "composition_request.v1", request_raw + "resolution_tier 8\n")
                with self.assertRaises(ValueError):
                    column.report(root, production)
                publish(root / "composition_request.v1", request_raw)
                for key in summary:
                    mutated = dict(summary, **{key: "wrong"})
                    publish(root / "composition_summary.v1", "".join(f"{k} {v}\n" for k, v in mutated.items()))
                    with self.assertRaises(ValueError):
                        column.report(root, production)
                publish(root / "composition_summary.v1", summary_raw + "solver_accepted 0\n")
                with self.assertRaises(ValueError):
                    column.report(root, production)
                publish(root / "composition_summary.v1", summary_raw)
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
