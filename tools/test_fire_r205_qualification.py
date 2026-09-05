#!/usr/bin/env python3
"""CPU REDs for r205 review findings. All mutations stay in temporary fixtures."""
import hashlib
import json
import os
from pathlib import Path
import shutil
import subprocess
import tempfile
import time
import unittest

from analyze_fire_whole_owner import analyze
from fire_payload_merkle import merkle
from qualify_fire_payload_placement import build_command, build_environment
from seal_fire_payload_placement import sidecars


class QualificationREDs(unittest.TestCase):
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
