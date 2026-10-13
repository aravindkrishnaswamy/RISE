#!/usr/bin/env python3
"""Run DL-382 gates sequentially; binaries must already be built.

Optional pre-merge baseline checks use independent existing checkout binaries.
Outputs are saved in --log-dir; no repository state is changed.
"""
import argparse
import os
from pathlib import Path
import re
import subprocess
import sys

ROOT = Path(__file__).resolve().parents[1]
GATES = [
    "OpenSheetConsumerScopeTest", "OpenSheetLegacyRenderTest",
    "MeshInteriorSignalTest", "TransmissionPushGateTest", "SMSDomainReplayTest",
    "OpenSheetWarningTest", "BSSRDFOpenSheetEntryTest", "RandomWalkOpenSheetRejectionTest",
    "CurvedOpenSheetSlabTest", "OpenSheetIndexConventionTest",
    "ExteriorIndexInvarianceTest", "CompositeEnergyConservationTest",
]


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--log-dir", type=Path, required=True)
    parser.add_argument("--baseline-root", type=Path,
                        help="built 0131003bc checkout for frozen pixel comparison")
    parser.add_argument("--before-root", type=Path,
                        help="built pre-fix master checkout for P1 red proofs")
    args = parser.parse_args()
    args.log_dir.mkdir(parents=True, exist_ok=True)
    env = dict(os.environ, RISE_MEDIA_PATH=str(ROOT) + os.sep, OPEN_SHEET_N="8")
    failed = []

    def run(root, name, extra=(), label=None, expect_failure=False):
        label = label or name
        path = args.log_dir / (label + ".log")
        with path.open("w") as log:
            result = subprocess.run([str(root / "bin/tests" / name), *extra],
                                    cwd=ROOT, env=env, stdout=log,
                                    stderr=subprocess.STDOUT)
        text = path.read_text()
        passed = (result.returncode == 1 and bool(re.search(r"(?:Failed: [1-9]\d*|[1-9]\d* failed)", text))) if expect_failure else result.returncode == 0
        print(f"{label}: exit={result.returncode} {'PASS' if passed else 'FAIL'}", flush=True)
        if not passed:
            failed.append(label)
        return text

    fixtures = ROOT / "tests/fixtures/dl382"
    scenes = (fixtures / "meshscenes.txt").read_text().splitlines()
    audit_path = args.log_dir / "certification-audit.txt"
    with audit_path.open("w") as log:
        audit = subprocess.run([str(ROOT / "bin/tools/OpenSheetCertificateAudit"), *scenes],
                               cwd=ROOT, env=env, stdout=log, stderr=subprocess.PIPE)
    expected = (fixtures / "certification-audit.txt").read_bytes()
    same = audit.returncode == 0 and audit_path.read_bytes() == expected
    print(f"certification audit: {'UNCHANGED' if same else 'FAIL'} ({len(scenes)} inputs)", flush=True)
    if not same:
        failed.append("certification audit")
    if failed:
        print("Stopped before renders: " + ", ".join(failed), flush=True)
        return True

    expected_hash = None
    if args.before_root:
        for name in ["RandomWalkOpenSheetRejectionTest", "OpenSheetWarningTest"]:
            run(args.before_root, name, label="red-" + name, expect_failure=True)
    if args.baseline_root:
        text = run(args.baseline_root, "OpenSheetLegacyRenderTest", label="baseline-frozen")
        match = re.search(r"hash=(\d+)", text)
        if not match:
            raise RuntimeError("baseline did not report its pixel hash")
        expected_hash = match.group(1)
        run(args.baseline_root, "CurvedOpenSheetSlabTest", label="red-curved", expect_failure=True)
        run(args.baseline_root, "RandomWalkOpenSheetRejectionTest", label="baseline-randomwalk")

    for name in GATES:
        extra = ["--sidedness-only"] if name == "CompositeEnergyConservationTest" else []
        if name == "OpenSheetLegacyRenderTest" and expected_hash:
            extra = [expected_hash]
        run(ROOT, name, extra)
        if failed:
            print("Stopped after failed gate: " + ", ".join(failed), flush=True)
            return True

    print("Failed: " + (", ".join(failed) if failed else "none"), flush=True)
    return bool(failed)


if __name__ == "__main__":
    sys.exit(main())
