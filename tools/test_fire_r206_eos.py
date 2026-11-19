#!/usr/bin/env python3
"""CPU evidence REDs; mutations never alter the published fixture files."""
from pathlib import Path
import contextlib
import hashlib
import io
import json
import shutil
import tempfile
import unittest
from unittest.mock import patch

from analyze_fire_eos_warm_start import analyze, endpoint_eos_gate, histogram, self_test
from analyze_fire_producer_kernels import fields, summarize
from seal_fire_payload_placement import bind_counters, distinct_repeats, gate, records
from check_fire_owner_instrumentation import FP64_PASS, RED_NAMES, qualify_artifact, trees
import check_fire_owner_cost_prefix as cost_prefix
import check_fire_owner_instrumentation as instrumentation


ROOT = Path(__file__).resolve().parents[1]
EVIDENCE = ROOT / "rendered/fire_production_calibration/r206_eos"


class EOSGateREDs(unittest.TestCase):
    def test_copied_runs_with_cosmetic_log_changes_are_not_independent(self):
        # Copies live only in an isolated parser-test directory. No published
        # artifact is modified or represented as an additional execution.
        with tempfile.TemporaryDirectory(prefix="rise-r206-copied-execution-red-") as temporary:
            directory = Path(temporary)
            for name in ("warm.owner_gate.v1.log", "warm_prototype.v1.patch", "warm_iteration_prefix.v1.log",
                         "qualification.eos.v2.json", "endpoints_qualified.eos_gate.v2.log"):
                shutil.copy2(EVIDENCE / name, directory / name)
            shutil.copytree(EVIDENCE / "warm_iteration_prefix.v1", directory / "warm_iteration_prefix.v1")
            for kind in ("baseline", "warm", "endpoints_qualified"):
                for repeat in (1, 2, 3):
                    name = f"{kind}_profile_{repeat}.v1"
                    shutil.copytree(EVIDENCE / name, directory / name)
                    shutil.copy2(EVIDENCE / (name+".log"), directory / (name+".log"))
            qualification = EVIDENCE / "qualification.v1.json"
            analyze(directory, qualification)
            names = [f"endpoints_qualified_profile_{repeat}.v1" for repeat in (1, 2, 3)]
            for suffix in ("\n", "\n# copied diagnostic commentary\n"):
                for repeat in (2, 3):
                    shutil.copytree(directory / names[0], directory / names[repeat-1], dirs_exist_ok=True)
                    (directory / (names[repeat-1]+".log")).write_bytes(
                        (directory / (names[0]+".log")).read_bytes() + (suffix*repeat).encode())
                with self.subTest(suffix=suffix):
                    with self.assertRaises(ValueError):
                        distinct_repeats(directory, names)
                    with self.assertRaises(ValueError):
                        analyze(directory, qualification)

    def test_disabled_observer_cli_rejects_every_profile_separator(self):
        fixture = ROOT / "rendered/fire_production_calibration/r202_owner_cost/exact_edb4afb6"
        off, on = fixture / "fixture_off.log", fixture / "fixture_on.log"
        original = Path.read_text
        off_text = off.read_text()
        profiles = "\n".join(line for line in on.read_text().splitlines() if line.startswith(instrumentation.PREFIX))
        self.assertTrue(profiles)
        with tempfile.TemporaryDirectory(prefix="rise-r206-observer-parser-") as temporary:
            for index, separator in enumerate((None, " ", "\t", "\u00a0", "  ")):
                output = Path(temporary) / (str(index)+".json")
                changed = off_text if separator is None else off_text+"\n"+profiles.replace(instrumentation.PREFIX, instrumentation.PREFIX.strip()+separator)
                argv = ["check_fire_owner_instrumentation.py", "--off", str(off), "--on", str(on),
                        "--off-trace", str(fixture / "fixture_off.v1"), "--on-trace", str(fixture / "fixture_on.v1"),
                        "--output", str(output)]
                with patch("sys.argv", argv), patch.object(Path, "read_text", lambda p: changed if p == off else original(p)), \
                     contextlib.redirect_stdout(io.StringIO()):
                    if separator is None:
                        instrumentation.main()
                        self.assertTrue(output.is_file())
                    else:
                        with self.assertRaises(ValueError):
                            instrumentation.main()
                        self.assertFalse(output.exists())

    def test_exact_executable_eos_fixture_is_required(self):
        qualification = EVIDENCE / "qualification.v1.json"
        qualified = json.loads(qualification.read_text())
        path = EVIDENCE / "qualification.eos.v2.json"
        evidence = json.loads(path.read_text())
        original_text, original_bytes = Path.read_text, Path.read_bytes
        endpoint_eos_gate(EVIDENCE, qualified, qualification)
        for key in ("source_commit", "parent_qualification_sha256", "executable_before_sha256",
                    "executable_after_sha256", "command", "exit_code", "log_sha256", "metal_library_source_sha256"):
            changed = dict(evidence)
            changed[key] = 1 if key == "exit_code" else "wrong"
            with self.subTest(identity=key), \
                 patch.object(Path, "read_text", lambda p: json.dumps(changed) if p == path else original_text(p)), \
                 patch.object(Path, "read_bytes", lambda p: json.dumps(changed).encode() if p == path else original_bytes(p)):
                with self.assertRaises(ValueError):
                    endpoint_eos_gate(EVIDENCE, qualified, qualification)
        with patch.object(Path, "read_text", lambda p: (_ for _ in ()).throw(FileNotFoundError(str(p))) if p == path else original_text(p)), \
             patch.object(Path, "read_bytes", lambda p: (_ for _ in ()).throw(FileNotFoundError(str(p))) if p == path else original_bytes(p)):
            with self.assertRaises(FileNotFoundError):
                endpoint_eos_gate(EVIDENCE, qualified, qualification)
            with self.assertRaises(FileNotFoundError):
                analyze(EVIDENCE, qualification)
        log = EVIDENCE / evidence["log"]
        text = log.read_text()
        line = next(line for line in text.splitlines() if "name=endpoint_enclosure_bit_mutation " in line)
        mutants = [text.replace(line, ""), text.replace(line, line.replace("observed=0x00000080", "observed=0x00000000")),
                   text.replace("eos_table_mutation_refused=1", "eos_table_mutation_refused=0"),
                   text.replace("name=forged_device_stage", "name=unrelated"),
                   text.replace("samples=49512449", "samples=1"),
                   text.replace("temperature_bit_equal=1", "temperature_bit_equal=0"),
                   text.replace("lattice=24756225", "lattice=1"),
                   text.replace("midpoints=24756224", "midpoints=1"),
                   text.replace("metal_identity_consistent=1", "metal_identity_consistent=0"),
                   text.replace("lower_K=300", "lower_K=301")]
        domain = next(row for row in text.splitlines() if row.startswith("RESIDENT_EOS_LOG_DOMAIN "))
        mutants += [text.replace(domain, ""), text+"\n"+domain]
        for row in text.splitlines():
            if row.startswith("RESIDENT_EOS_RED "):
                for key, value in fields(row).items():
                    mutants.append(text.replace(row, row.replace(key+"="+value, key+"=wrong", 1), 1))
        for mutant in mutants:
            changed = dict(evidence, log_sha256=hashlib.sha256(mutant.encode()).hexdigest())
            with self.subTest(log_mutation=hashlib.sha256(mutant.encode()).hexdigest()), \
                 patch.object(Path, "read_text", lambda p: json.dumps(changed) if p == path else original_text(p)), \
                 patch.object(Path, "read_bytes", lambda p: json.dumps(changed).encode() if p == path else mutant.encode() if p == log else original_bytes(p)):
                with self.assertRaises(ValueError):
                    endpoint_eos_gate(EVIDENCE, qualified, qualification)
                with self.assertRaises(ValueError):
                    analyze(EVIDENCE, qualification)

    def test_whitespace_prefixed_duplicate_records_are_not_ignored(self):
        original_text, original_bytes = Path.read_text, Path.read_bytes
        for stem, tags in (
                ("warm_iteration_prefix.v1", ("EOS_ITERATIONS_V1", "EOS_WARM_PROBES_V1")),
                ("endpoints_qualified_profile_1.v1", ("PRODUCER_COMMAND_V1", "PRODUCER_KERNEL_V1", "RISE_FIRE_OWNER_PROFILE_V1"))):
            path = EVIDENCE / (stem+".log")
            prefix = path.with_suffix("")
            text = path.read_text()
            for tag in tags:
                line = next(line for line in text.splitlines() if line.startswith(tag+" "))
                for separator in (" ", "\t", "\u00a0", "  "):
                    for extra in (separator+line, line.replace(tag+" ", tag+separator, 1)):
                        changed = text+"\n"+extra+"\n"
                        with self.subTest(tag=tag, separator=repr(separator)):
                            with patch.object(Path, "read_bytes", lambda p: changed.encode() if p == path else original_bytes(p)), \
                                 patch.object(Path, "read_text", lambda p: changed if p == path else original_text(p)):
                                with self.assertRaises(ValueError):
                                    if tag.startswith("EOS_"):
                                        histogram(path)
                                    elif tag == "PRODUCER_KERNEL_V1":
                                        summarize(path)
                                    else:
                                        bind_counters(path, records(prefix / "budgets/maximum_velocity_trajectory.csv"),
                                                      prefix / "diagnostic_prefix_outcome.v1")

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
