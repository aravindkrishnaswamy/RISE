#!/usr/bin/env python3
"""CPU evidence REDs; mutations never alter the published fixture files."""
from pathlib import Path
import contextlib
import hashlib
import io
import tempfile
import unittest
from unittest.mock import patch

from analyze_fire_eos_warm_start import histogram, self_test
from analyze_fire_producer_kernels import fields, summarize
from seal_fire_payload_placement import bind_counters, gate, records
from check_fire_owner_instrumentation import FP64_PASS, RED_NAMES, qualify_artifact, trees
import check_fire_owner_cost_prefix as cost_prefix


ROOT = Path(__file__).resolve().parents[1]
EVIDENCE = ROOT / "rendered/fire_production_calibration/r206_eos"


class EOSGateREDs(unittest.TestCase):
    def test_legacy_cost_cli_requires_unambiguous_successful_completion(self):
        path = EVIDENCE / "endpoints_qualified_profile_1.v1.log"
        prefix = path.with_suffix("")
        text = path.read_text()
        original = Path.read_text
        mutants = [text.replace("OWNER_COST_PREFIX complete=1", "OWNER_COST_PREFIX complete=0"),
                   text.replace("OWNER_COST_PREFIX complete=1", "OWNER_COST_PREFIX complete=0 complete=1")]
        line = next(line for line in text.splitlines() if line.startswith("OWNER_COST_PREFIX "))
        mutants += [text.replace(line, line.replace("outcome_sha256=", "outcome_sha256=bad outcome_sha256="))]
        with tempfile.TemporaryDirectory(prefix="rise-r206-parser-only-") as temporary:
            for index, data in enumerate([text, *mutants]):
                output = Path(temporary) / (str(index)+".json")
                argv = ["check_fire_owner_cost_prefix.py", "--reference", str(prefix / "budgets/maximum_velocity_trajectory.csv"),
                        "--reference-identity", str(prefix / "diagnostic_from_zero_identity.v1"),
                        "--probe", str(prefix), "--profile", str(path), "--output", str(output)]
                with patch("sys.argv", argv), patch.object(Path, "read_text", lambda p: data if p == path else original(p)), \
                     contextlib.redirect_stdout(io.StringIO()):
                    if index == 0:
                        cost_prefix.main()
                        self.assertTrue(output.is_file())
                    else:
                        with self.assertRaises(ValueError):
                            cost_prefix.main()
                        self.assertFalse(output.exists())

    def test_instrumentation_qualification_requires_singleton_verdicts(self):
        # Synthetic parser fixture only: never published as solver evidence.
        csv = b"synthetic-parser-input\n"
        trace = ("OWNER_CONVERGENCE_CSV sha256="+hashlib.sha256(csv).hexdigest()+"\n").encode()
        log = "\n".join([FP64_PASS, "RESIDENT_TARGET passed=1 source_bit_equal=1",
            "OWNER_CONVERGENCE_PROBE passed=1 error=",
            "OWNER_CONVERGENCE_ARTIFACT path=synthetic-only sha256="+hashlib.sha256(trace).hexdigest()+
            " scope=qualified_fixture_only passed=1",
            *["OWNER_CONVERGENCE_RED name="+name+" atomic_refusal=1 error=synthetic-only passed=1"
              for name in sorted(RED_NAMES)]])+"\n"
        qualify_artifact(log, trace, csv)
        for tag in ("PROJECTED_HEUN_METAL_OWNER_FP64", "OWNER_CONVERGENCE_PROBE", "RESIDENT_TARGET"):
            line = next(line for line in log.splitlines() if line.startswith(tag+" "))
            for extra in (line, line.replace("passed=1", "passed=0")):
                with self.subTest(tag=tag, extra=extra):
                    with self.assertRaises(ValueError):
                        qualify_artifact(log+extra+"\n", trace, csv)
        for tag in ("OWNER_CONVERGENCE_ARTIFACT", "OWNER_CONVERGENCE_RED"):
            line = next(line for line in log.splitlines() if line.startswith(tag+" "))
            mutants = [line+" unknown=0", ""]
            for key, value in fields(line).items():
                token = key+"="+value
                mutants += [line.replace(token, "", 1), line+" "+key+"=bad",
                            line.replace(token, key+"=bad "+token, 1)]
            if tag == "OWNER_CONVERGENCE_RED":
                for separator in (" ", "\t", "\u00a0", "\v", "\f", "  "):
                    for token in ("atomic_refusal=0", "passed=0", "unknown=0"):
                        mutants.append(line.replace(" passed=1", separator+token+" passed=1"))
            for mutant in mutants:
                with self.subTest(tag=tag, mutant=mutant):
                    with self.assertRaises(ValueError):
                        qualify_artifact(log.replace(line, mutant), trace, csv)

    def test_metadata_rejects_duplicate_identity_fields(self):
        path = EVIDENCE / "endpoints_qualified_profile_1.v1/diagnostic_from_zero_identity.v1"
        text = path.read_text()
        cost_prefix.metadata(path)
        for line in text.splitlines():
            key = line.split(" ", 1)[0]
            with self.subTest(key=key), patch.object(Path, "read_text", return_value=text+key+" bad\n"):
                with self.assertRaises(ValueError):
                    cost_prefix.metadata(path)

    def test_counter_family_rejects_duplicates_missing_and_unknown_fields(self):
        path = EVIDENCE / "endpoints_qualified_profile_1.v1.log"
        prefix = path.with_suffix("")
        text = path.read_text()
        rows = records(prefix / "budgets/maximum_velocity_trajectory.csv")
        outcome = prefix / "diagnostic_prefix_outcome.v1"
        original_text, original_bytes = Path.read_text, Path.read_bytes
        consumers = {
            "OWNER_COST_PREFIX": lambda: bind_counters(path, rows, outcome),
            "PRODUCER_COMMAND_V1": lambda: (bind_counters(path, rows, outcome), summarize(path)),
            "PRODUCER_KERNEL_V1": lambda: summarize(path),
        }
        for tag, consume in consumers.items():
            consume()
            line = next(line for line in text.splitlines() if line.startswith(tag+" "))
            mutants = [line+" unknown=0", line+" malformed"]
            for key, value in fields(line).items():
                token = key+"="+value
                mutants += [line.replace(token, "", 1), line+" "+key+"=conflict",
                            line.replace(token, key+"=conflict "+token, 1)]
            for mutant in mutants:
                with self.subTest(tag=tag, mutant=mutant):
                    changed = text.replace(line, mutant, 1)
                    with patch.object(Path, "read_text", lambda p: changed if p == path else original_text(p)), \
                         patch.object(Path, "read_bytes", lambda p: changed.encode() if p == path else original_bytes(p)):
                        with self.assertRaises(ValueError):
                            consume()
        import json
        profile_tag = "RISE_FIRE_OWNER_PROFILE_V1 "
        line = next(line for line in text.splitlines() if line.startswith(profile_tag))
        row = json.loads(line[len(profile_tag):])
        for key in row:
            mutants = [line[:-1]+',"'+key+'":0}',
                       profile_tag+json.dumps({k:v for k,v in row.items() if k != key})]
            for mutant in mutants:
                with self.subTest(profile_key=key, mutant=mutant):
                    with self.assertRaises(ValueError):
                        trees(text.replace(line, mutant, 1))
        with self.assertRaises(ValueError):
            trees(text.replace(line, line[:-1]+',"unknown":0}', 1))
        path = EVIDENCE / "warm_iteration_prefix.v1.log"
        text = path.read_text()
        line = next(line for line in text.splitlines() if line.startswith("OWNER_COST_PREFIX "))
        for key, value in fields(line).items():
            token = key+"="+value
            for mutant in (line.replace(token, "", 1), line+" "+key+"=conflict",
                           line.replace(token, key+"=conflict "+token, 1)):
                changed = text.replace(line, mutant, 1)
                with self.subTest(histogram_terminal=key, mutant=mutant):
                    with patch.object(Path, "read_bytes", lambda p: changed.encode() if p == path else original_bytes(p)):
                        with self.assertRaises(ValueError):
                            histogram(path)

    def test_iteration_and_independent_process_evidence(self):
        result = self_test(EVIDENCE)
        self.assertIn("missing_bit_verdict", result["refused"])
        self.assertIn("copied_process_logs", result["refused"])

    def test_owner_requires_each_named_field_and_its_complete_extent(self):
        path = EVIDENCE / "qualification.v1.json.owner.log"
        text = path.read_text()
        gate(path)
        rows = [line for line in text.splitlines()
                if line.startswith("OWNER_SEALING_EQUIVALENCE field=")]
        self.assertEqual(len(rows), 41)
        for row in rows:
            values = dict(word.split("=", 1) for word in row.split()[1:])
            field, words = values["field"], int(values["words"])
            replacements = {
                "renamed": row.replace("field="+field, "field=NOT_"+field),
                "dropped": "",
                "zero": row.replace("words="+str(words), "words=0"),
                "short": row.replace("words="+str(words), "words="+str(words-1)),
                "long": row.replace("words="+str(words), "words="+str(words+1)),
                "mismatch": row.replace("bit_mismatches=0", "bit_mismatches=1"),
                "duplicate_counter": row.replace("words=", "words=0 words="),
                "duplicate_row": row+"\n"+row,
            }
            for name, replacement in replacements.items():
                with self.subTest(field=field, mutation=name):
                    mutant = text.replace(row, replacement, 1)
                    self.assertNotEqual(mutant, text)
                    with patch.object(Path, "read_text", return_value=mutant):
                        with self.assertRaises(ValueError):
                            gate(path)
        for replacement in ("iterations=0/2/4", "iterations=2/2", "iterations=3/2/4"):
            mutant = text.replace("iterations=2/2/4", replacement)
            self.assertNotEqual(mutant, text)
            with patch.object(Path, "read_text", return_value=mutant):
                with self.assertRaises(ValueError):
                    gate(path)
        for prefix in ("OWNER_SEALING_EQUIVALENCE passed=", "PROJECTED_HEUN_METAL_OWNER_FP64 "):
            row = next(line for line in text.splitlines() if line.startswith(prefix))
            for replacement in (row.replace("passed=1", "passed=10"), row+"\n"+row):
                mutant = text.replace(row, replacement, 1)
                with patch.object(Path, "read_text", return_value=mutant):
                    with self.assertRaises(ValueError):
                        gate(path)
        for tag, counter in (("PROJECTED_HEUN_METAL_OWNER_FP64", "passed"),
                             ("OWNER_PUBLICATION_DIGEST_RED", "full_packet_cpu_match"),
                             ("OWNER_SEALING_EQUIVALENCE passed=", "passed")):
            row = next(line for line in text.splitlines() if line.startswith(tag))
            with self.subTest(contradictory_verdict=tag):
                mutant = text + "\n" + row.replace(counter+"=1", counter+"=0") + "\n"
                with patch.object(Path, "read_text", return_value=mutant):
                    with self.assertRaises(ValueError):
                        gate(path)
        for replacement in ("accepted=0 accepted=1", "iterations=0/0/0 iterations=2/2/4"):
            old = "accepted=1" if replacement.startswith("accepted") else "iterations=2/2/4"
            row = next(line for line in text.splitlines() if line.startswith("PROJECTED_HEUN_METAL_OWNER_SMOKE "))
            mutant = text.replace(row, row.replace(old, replacement), 1)
            with patch.object(Path, "read_text", return_value=mutant):
                with self.assertRaises(ValueError):
                    gate(path)


if __name__ == "__main__":
    unittest.main()
