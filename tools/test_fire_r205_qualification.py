#!/usr/bin/env python3
"""CPU REDs for r205 review findings. All mutations stay in temporary fixtures."""
import hashlib
import contextlib
import io
import json
import os
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile
import time
import unittest
from unittest import mock

from analyze_fire_whole_owner import analyze
from fire_payload_merkle import merkle
from qualify_fire_payload_placement import build_command, build_environment
import qualify_fire_payload_placement as qualification
from seal_fire_payload_placement import sidecars


class QualificationREDs(unittest.TestCase):
    def test_compilation_source_closure_must_be_committed(self):
        with tempfile.TemporaryDirectory(prefix="rise-r205-source-closure-red-") as temporary:
            root = Path(temporary)
            for directory in ("build/make/rise", "extlib/stb", "tests/fire_production_trace"):
                (root / directory).mkdir(parents=True)
            (root / ".gitignore").write_text("*.o\nextra_ignored.cpp\nbuild/make/rise/Config.specific\n")
            config = root / "build/make/rise/Config.OSX"
            config.write_text("CXXARCHFLAGS =\n")
            (config.parent / "Config.specific").symlink_to(config.name)
            vendor = root / "extlib/stb/stb_image.h"
            vendor.write_text("// committed decoder\n")
            for command in (["git", "init", "-q"], ["git", "add", "."],
                            ["git", "-c", "user.name=Fixture", "-c", "user.email=fixture@example.invalid",
                             "commit", "-qm", "source closure"]):
                subprocess.run(command, cwd=root, check=True, capture_output=True)
            previous = Path.cwd()
            try:
                os.chdir(root)
                # Unrelated ignored object caches are allowed and forced rebuilt.
                (root / "tests/unused.o").write_bytes(b"cached object")
                qualification.clean_source("HEAD")
                for mutation in ("vendor", "wildcard", "ignored_wildcard"):
                    extra = root / "tests/fire_production_trace" / (
                        "extra_ignored.cpp" if mutation == "ignored_wildcard" else "extra.cpp")
                    if mutation == "vendor":
                        vendor.write_text("// locally changed decoder\n")
                    else:
                        extra.write_text("int extra_trace_input = 1;\n")
                    # Prior scoped diff passes both realistic source changes.
                    subprocess.run(["git", "diff", "--exit-code", "HEAD", "--", "src", "tests", "build", "tools"],
                                   check=True, capture_output=True)
                    output = root / (mutation + ".json")
                    with mock.patch.object(sys, "argv", ["qualify", str(output)]):
                        with self.assertRaises((ValueError, subprocess.CalledProcessError)):
                            qualification.main()
                    self.assertFalse(output.exists())
                    self.assertFalse(Path(str(output) + ".build.log").exists())
                    vendor.write_text("// committed decoder\n")
                    if extra.exists():
                        extra.unlink()
            finally:
                os.chdir(previous)

    def test_disk_recipe_inputs_cannot_preserve_foreign_object(self):
        with tempfile.TemporaryDirectory(prefix="rise-r205-disk-recipe-red-") as temporary:
            root = Path(temporary)
            (root / "source.cpp").write_bytes(b"current source")
            (root / "GNUmakefile").write_text("all:\n\t@true\n")
            (root / "cached.d").write_text("MAKEFLAGS = -n\n")
            (root / "Makefile").write_text("ALL_DEPS = cached.d\n-include $(ALL_DEPS)\n"
                "all: cached.o\ncached.o: source.cpp\n\tcp source.cpp cached.o\n")
            for mutant in ("implicit_makefile", "generated_dependency"):
                (root / "cached.o").write_bytes(b"foreign object")
                command = build_command(root, "all")
                if mutant == "implicit_makefile":
                    index = command.index("-f")
                    del command[index:index + 2]
                else:
                    command.remove("ALL_DEPS=")
                subprocess.run(command, check=True, capture_output=True, env=build_environment())
                self.assertEqual((root / "cached.o").read_bytes(), b"foreign object")
                subprocess.run(build_command(root, "all"), check=True, capture_output=True,
                               env=build_environment())
                self.assertEqual((root / "cached.o").read_bytes(), b"current source")

    def test_local_configuration_must_match_versioned_bytes(self):
        with tempfile.TemporaryDirectory(prefix="rise-r205-local-config-red-") as temporary:
            root = Path(temporary)
            config_dir = root / "build/make/rise"
            config_dir.mkdir(parents=True)
            (root / ".gitignore").write_text("build/make/rise/Config.specific\n")
            canonical = config_dir / "Config.OSX"
            selected = config_dir / "Config.specific"
            canonical.write_text("CXXARCHFLAGS =\n")
            for command in (["git", "init", "-q"], ["git", "add", "."],
                            ["git", "-c", "user.name=Fixture", "-c", "user.email=fixture@example.invalid",
                             "commit", "-qm", "qualified recipe"]):
                subprocess.run(command, cwd=root, check=True, capture_output=True)
            previous = Path.cwd()
            try:
                os.chdir(root)
                selected.write_bytes(canonical.read_bytes())
                self.assertEqual(qualification.clean_source("HEAD")["sha256"],
                                 hashlib.sha256(canonical.read_bytes()).hexdigest())
                selected.unlink()
                selected.symlink_to("Config.OSX")
                qualification.clean_source("HEAD")
                selected.unlink()
                selected.write_text("CXXARCHFLAGS = -\\#\\#\\#\n")
                # The previous tracked-diff-only admission cannot see this.
                subprocess.run(["git", "diff", "--exit-code", "HEAD", "--", "src", "tests", "build", "tools"],
                               check=True, capture_output=True)
                output = root / "attestation.json"
                with mock.patch.object(sys, "argv", ["qualify", str(output)]):
                    with self.assertRaisesRegex(ValueError, "unqualified local build configuration"):
                        qualification.main()
                self.assertFalse(output.exists())
                self.assertFalse(Path(str(output) + ".build.log").exists())
            finally:
                os.chdir(previous)

    def test_compiler_diagnostic_mode_cannot_preserve_foreign_object(self):
        with tempfile.TemporaryDirectory(prefix="rise-r205-compiler-mode-red-") as temporary:
            root = Path(temporary)
            compiler = shutil.which("c++")
            self.assertIsNotNone(compiler)
            source, obj, executable = root / "source.cpp", root / "cached.o", root / "program"
            source.write_text("int main(){return 1;}\n")
            subprocess.run([compiler, "-c", str(source), "-o", str(obj)], check=True, capture_output=True,
                           env=build_environment())
            source.write_text("int main(){return 0;}\n")
            (root / "Makefile").write_text("all: program\nprogram: cached.o\n\t" + compiler +
                " cached.o -o program\ncached.o: source.cpp\n\t" + compiler +
                " $(CXXARCHFLAGS) -c source.cpp -o cached.o\n")
            environment = build_environment()
            environment["CXXARCHFLAGS"] = "-###"
            command = build_command(root, "all")
            unqualified = subprocess.run(command, check=True, capture_output=True, env=environment)
            self.assertNotIn(b"warning:", unqualified.stdout + unqualified.stderr)
            self.assertEqual(subprocess.run([str(executable)], check=False).returncode, 1)
            subprocess.run(command, check=True, capture_output=True, env=build_environment(environment))
            self.assertEqual(subprocess.run([str(executable)], check=False).returncode, 0)
            self.assertNotIn("CXXFLAGS_DEPS", build_environment(dict(environment, CXXFLAGS_DEPS="-M")))

    def test_zero_exit_without_actual_gate_cannot_be_attested(self):
        evidence = Path(__file__).resolve().parents[1] / "rendered/fire_production_calibration/r205_owner_cost"
        with tempfile.TemporaryDirectory(prefix="rise-r205-empty-gate-red-") as temporary:
            for missing in ("publication", "owner"):
                output = Path(temporary) / (missing + ".json")
                def mock_run(command, **kwargs):
                    stream = kwargs.get("stdout")
                    if hasattr(stream, "write"):
                        mode = ("publication" if "--fire-production-payload-publication" in command
                                else "owner" if "--fire-production-resident-target-metal" in command
                                else "build")
                        if mode == "build":
                            stream.write(b"Compiling fixture\n")
                        elif mode != missing:
                            stream.write((evidence / ("qualification.round3.v1.json." + mode + ".log")).read_bytes())
                    return subprocess.CompletedProcess(command, 0)
                with mock.patch.object(sys, "argv", ["qualify", str(output)]), \
                     mock.patch.object(qualification.subprocess, "check_output", return_value="a" * 40), \
                     mock.patch.object(qualification.subprocess, "run", side_effect=mock_run), \
                     mock.patch.object(qualification, "clean_source", return_value={}), \
                     mock.patch.object(qualification, "sha", return_value="b" * 64), \
                     contextlib.redirect_stdout(io.StringIO()):
                    with self.assertRaises(ValueError):
                        qualification.main()
                self.assertFalse(output.exists())

    def test_foreign_newer_object_is_rebuilt(self):
        with tempfile.TemporaryDirectory(prefix="rise-r205-build-red-") as temporary:
            root = Path(temporary)
            (root / "source.cpp").write_bytes(b"current source")
            (root / "cached.o").write_bytes(b"foreign cached object")
            os.utime(root / "cached.o", (time.time() + 100, time.time() + 100))
            (root / "Makefile").write_text("all: cached.o\ncached.o: source.cpp\n\tcp source.cpp cached.o\n")
            command = build_command(root, "all")
            subprocess.run([arg for arg in command if arg != "-B"], check=True, capture_output=True,
                           env=build_environment())
            self.assertEqual((root / "cached.o").read_bytes(), b"foreign cached object")
            subprocess.run(command, check=True, capture_output=True, env=build_environment())
            self.assertEqual((root / "cached.o").read_bytes(), b"current source")

    def test_inherited_make_flags_cannot_skip_compilation(self):
        with tempfile.TemporaryDirectory(prefix="rise-r205-make-flags-red-") as temporary:
            root = Path(temporary)
            (root / "source.cpp").write_bytes(b"current source")
            (root / "Makefile").write_text("all: cached.o\ncached.o: source.cpp\n\tcp source.cpp cached.o\n")
            command = build_command(root, "all")
            for flag in ("n", "t"):
                environment = build_environment()
                environment["MAKEFLAGS"] = flag
                (root / "cached.o").write_bytes(b"foreign object")
                subprocess.run(command, check=True, capture_output=True, env=environment)
                self.assertEqual((root / "cached.o").read_bytes(), b"foreign object")
                subprocess.run(command, check=True, capture_output=True,
                               env=build_environment(environment))
                self.assertEqual((root / "cached.o").read_bytes(), b"current source")

    def test_inherited_makefile_cannot_reintroduce_dry_run(self):
        with tempfile.TemporaryDirectory(prefix="rise-r205-makefiles-red-") as temporary:
            root = Path(temporary)
            (root / "source.cpp").write_bytes(b"current source")
            (root / "cached.o").write_bytes(b"foreign object")
            (root / "Makefile").write_text("all: cached.o\ncached.o: source.cpp\n\tcp source.cpp cached.o\n")
            injected = root / "site.mk"
            injected.write_text("MAKEFLAGS = -n\n")
            environment = build_environment()
            environment["MAKEFILES"] = str(injected)
            command = build_command(root, "all")
            subprocess.run(command, check=True, capture_output=True, env=environment)
            self.assertEqual((root / "cached.o").read_bytes(), b"foreign object")
            subprocess.run(command, check=True, capture_output=True, env=build_environment(environment))
            self.assertEqual((root / "cached.o").read_bytes(), b"current source")

    def test_orphan_sidecar_refused(self):
        with tempfile.TemporaryDirectory(prefix="rise-r205-orphan-red-") as temporary:
            root = Path(temporary)
            for name in ("one", "two"):
                payload = name.encode()
                (root / name).write_bytes(payload)
                (root / (name + ".payload-v2.json")).write_text(json.dumps(dict(
                    schema="rise.fire.published-payload.v2", case_record_id="a" * 64,
                    sha256_v1=hashlib.sha256(payload).hexdigest(), v2=merkle(payload))))
            self.assertEqual(sidecars(root, "a" * 64), 2)
            (root / "one").unlink()
            with self.assertRaisesRegex(ValueError, "orphan"):
                sidecars(root, "a" * 64)

    def test_attested_inventory_and_wall_accounting(self):
        source = Path(__file__).resolve().parents[1] / "rendered/fire_production_calibration/r204_digest_v2"
        measured = analyze(source)["device_p95_step"]
        self.assertGreater(measured["outside_owner_scope_wall_ms"], 0)
        self.assertGreater(measured["observer_wall_ms"], 0)
        attributed = (sum(v["wall_ms"] for v in measured["exclusive_phases"].values())
                      + measured["observer_wall_ms"] + measured["outside_owner_scope_wall_ms"])
        self.assertAlmostEqual(attributed, measured["wall_ms"], places=7)
        with tempfile.TemporaryDirectory(prefix="rise-r205-attribution-red-") as temporary:
            root = Path(temporary) / "campaign"
            shutil.copytree(source, root)
            inventory = root / "inventory.v1.json"
            records = json.loads(inventory.read_text())
            records["files"] = records["files"][:-1]
            data = (json.dumps(records) + "\n").encode()
            inventory.write_bytes(data)
            Path(str(inventory) + ".seal-v2.json").write_text(json.dumps(dict(
                artifact=inventory.name, sha256=hashlib.sha256(data).hexdigest(), payload_v2=merkle(data))))
            with self.assertRaisesRegex(ValueError, "inventory binding"):
                analyze(root)

    def test_pending_and_relabelled_v1_publications_refuse(self):
        with tempfile.TemporaryDirectory(prefix="rise-r205-publication-format-red-") as temporary:
            root = Path(temporary)
            payload = b"not a published checkpoint"
            for name, digest in (("state.preparation.pending", merkle(payload)),
                                 ("state", dict(digest_format="sha256-bytes", digest_version=1,
                                                sha256=hashlib.sha256(payload).hexdigest()))):
                artifact = root / name
                certificate = root / (name + ".payload-v2.json")
                artifact.write_bytes(payload)
                certificate.write_text(json.dumps(dict(schema="rise.fire.published-payload.v2",
                    case_record_id="a" * 64, sha256_v1=hashlib.sha256(payload).hexdigest(), v2=digest)))
                with self.assertRaisesRegex(ValueError, "pending|invalid published sidecar"):
                    sidecars(root, "a" * 64)
                artifact.unlink()
                certificate.unlink()
            (root / "unpublished.pending").mkdir()
            with self.assertRaisesRegex(ValueError, "pending"):
                sidecars(root, "a" * 64)


if __name__ == "__main__":
    unittest.main()
