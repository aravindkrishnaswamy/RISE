#!/usr/bin/env python3
"""Immutable inventory of r205 claim-bearing exports, fixtures, and gate logs."""
import hashlib
import json
from pathlib import Path
import sys

ROOT = Path(__file__).resolve().parent
sys.path.insert(0, str(ROOT.parents[2] / "tools"))
from fire_payload_merkle import merkle


def main():
    output = ROOT / "inventory.v1.json"
    seal = ROOT / "inventory.v1.json.seal-v2.json"
    if output.exists() or seal.exists():
        raise ValueError("r205 inventory already published")
    attestation = json.loads((ROOT / "qualification.final.v1.json").read_text())
    for run in attestation["runs"].values():
        log = ROOT.parents[2] / run["log"]
        if run["exit_code"] != 0 or hashlib.sha256(log.read_bytes()).hexdigest() != run["log_sha256"]:
            raise ValueError("invalid qualification log")
    full_log = ROOT / "full.fixture.final.attested.v1.log"
    if "FireSequenceTest: canonical sequence/loadability/preparation gates passed" not in full_log.read_text():
        raise ValueError("missing complete-suite verdict")
    # Native capture is redundant local tooling data, not a dependency of any
    # claim. The exact exported tables used by the reproducible analysis ARE
    # inventoried and tracked. Do not put a 400 MB opaque bundle in source git.
    local_only = {"whole_owner_metal.v1.trace", "whole_owner_metal.v1.trace.tar.gz"}
    files = []
    for path in sorted(ROOT.rglob("*")):
        relative = path.relative_to(ROOT)
        if relative.parts[0] in local_only or "__pycache__" in relative.parts:
            continue
        if path.is_symlink():
            raise ValueError("symlink in evidence")
        if not path.is_file():
            continue
        data = path.read_bytes()
        if len(data) >= 100 * 1024 * 1024:
            raise ValueError("oversized claim-bearing artifact")
        files.append({"path": str(relative), "sha256_v1": hashlib.sha256(data).hexdigest(),
                      "v2": merkle(data)})
    document = {"schema": "rise.fire.r205-evidence-inventory.v1", "files": files,
                "qualified_source_commit": attestation["source_commit"],
                "scope": "fixture repairs and cold-prefix accounting; no EOS speedup or onset verdict",
                "local_only_redundant_tooling": sorted(local_only),
                "native_trace_claim_source": "whole_owner_metal.v1.target_tables.xml"}
    data = (json.dumps(document, sort_keys=True, indent=2, allow_nan=False) + "\n").encode()
    with output.open("xb") as stream:
        stream.write(data)
    with seal.open("x") as stream:
        json.dump({"artifact": output.name, "sha256_v1": hashlib.sha256(data).hexdigest(),
                   "v2": merkle(data)}, stream, sort_keys=True, indent=2)
        stream.write("\n")
    print(hashlib.sha256(data).hexdigest(), len(files))


if __name__ == "__main__":
    main()
