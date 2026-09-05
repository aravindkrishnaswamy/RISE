#!/usr/bin/env python3
"""Independent r204 byte-payload Merkle specification and immutable v1 bridge.

The format namespace is rise-payload-sha256-merkle; this is not the historical
accepted-state 64-bit fast digest, whose version numbering is independent.
"""
import argparse
import hashlib
import json
from pathlib import Path
import struct
import re
import subprocess

CHUNK_BYTES = 4096
FAN_IN = 16
FORMAT = "rise-payload-sha256-merkle"


def merkle(payload):
    size = len(payload)
    nodes = [hashlib.sha256(b"RISELEAF" + struct.pack(">IQQI", 2, size, i, len(chunk)) + chunk).digest()
             for i, chunk in enumerate(payload[j:j + CHUNK_BYTES]
                                       for j in range(0, max(1, size), CHUNK_BYTES))]
    leaves, level = len(nodes), 0
    while len(nodes) > 1:
        level += 1
        nodes = [hashlib.sha256(b"RISENODE" + struct.pack(">IQIQI", 2, size, level, j // FAN_IN,
                                                         len(nodes[j:j + FAN_IN])) +
                                b"".join(nodes[j:j + FAN_IN])).digest()
                 for j in range(0, len(nodes), FAN_IN)]
    root = hashlib.sha256(b"RISEROOT" + struct.pack(">IIIQQI", 2, CHUNK_BYTES, FAN_IN, size, leaves, level)
                          + nodes[0]).hexdigest()
    return {"digest_format": FORMAT, "digest_version": 2, "chunk_bytes": CHUNK_BYTES,
            "fan_in": FAN_IN, "payload_bytes": size, "root_sha256": root}


def pattern(size):
    return bytes((i * 37 + (i >> 8) * 19 + 11) & 255 for i in range(size))


def verify(payload, seal):
    if seal.get("digest_format") == "sha256-bytes" and seal.get("digest_version") == 1:
        return seal == {"digest_format": "sha256-bytes", "digest_version": 1,
                        "sha256": hashlib.sha256(payload).hexdigest()}
    if seal.get("digest_format") != FORMAT or seal.get("digest_version") != 2:
        return False
    return seal == merkle(payload)


def self_test():
    payload = pattern(65537)
    sealed = merkle(payload)
    assert verify(payload, sealed)
    assert verify(payload, {"digest_format": "sha256-bytes", "digest_version": 1,
                            "sha256": hashlib.sha256(payload).hexdigest()})
    for key, value in (("digest_format", "sha256-bytes"), ("digest_version", 1),
                       ("chunk_bytes", 2048), ("fan_in", 8), ("payload_bytes", 65536),
                       ("root_sha256", "0" * 64)):
        mutant = dict(sealed, **{key: value})
        assert not verify(payload, mutant), key
    assert not verify(payload + b"\0", sealed)
    assert not verify(payload[4096:] + payload[:4096], sealed)
    for i in (0, 4095, 4096, 65535, 65536):
        for bit in range(8):
            mutant = bytearray(payload)
            mutant[i] ^= 1 << bit
            assert not verify(mutant, sealed)
    print("MERKLE_V2_PYTHON_RED_PASS version format shape length order bit_mutations historical_v1")


def bridge(repository, golden, output):
    """Add a bridge; never mutate any legacy payload or existing bridge."""
    golden_sha = "1b944176a1dad4937872b0b63057854659cb37672ff833635c3d1827cbcb4947"
    payload = golden.read_bytes()
    if hashlib.sha256(payload).hexdigest() != golden_sha:
        raise ValueError("golden checkpoint identity mismatch")
    records = [{"role": "golden-checkpoint", "path": str(golden.resolve()),
                "v1": {"digest_format": "sha256-bytes", "digest_version": 1, "sha256": golden_sha},
                "v2": merkle(payload)}]
    commit = subprocess.check_output(["git", "rev-parse", "HEAD"], cwd=repository, text=True).strip()
    files = subprocess.check_output(["git", "ls-tree", "-r", "--name-only", commit, "--",
                                     "rendered/fire_production_calibration"], cwd=repository, text=True).splitlines()
    selected = [path for path in files if re.search(r"/r(?:18[0-9]|19[0-9]|20[0-3])(?:_|/)", path)]
    if not selected:
        raise ValueError("bridge cannot omit the preserved r18x-r203 evidence tree")
    for path in selected:
        payload = (repository / path).read_bytes()
        historical = subprocess.check_output(["git", "show", f"{commit}:{path}"], cwd=repository)
        if payload != historical:
            raise ValueError(f"historical evidence differs from pinned commit: {path}")
        records.append({"role": "preserved-evidence", "path": path,
                        "v1": {"digest_format": "sha256-bytes", "digest_version": 1,
                               "sha256": hashlib.sha256(payload).hexdigest()}, "v2": merkle(payload)})
    record = {"schema": "rise.fire.payload-digest-bridge.v2", "historical_tree_commit": commit,
              "scope": "unchanged golden checkpoint and every tracked r180-r203 calibration artifact",
              "digest_format": FORMAT, "digest_version": 2, "records": records}
    encoded = (json.dumps(record, indent=2, sort_keys=True) + "\n").encode()
    # The detached seal avoids a self-referential root. Exclusive creation
    # preserves v1 evidence and makes accidental bridge replacement a refusal.
    seal = {"schema": "rise.fire.payload-digest-bridge-seal.v2",
            "v1_sha256": hashlib.sha256(encoded).hexdigest(), "v2": merkle(encoded)}
    seal_path = output.with_name(output.name + ".seal.json")
    if output.exists() or seal_path.exists():
        raise FileExistsError("bridge or seal already exists; historical bridges are immutable")
    with output.open("xb") as stream:
        stream.write(encoded)
    with seal_path.open("x") as stream:
        json.dump(seal, stream, indent=2, sort_keys=True)
        stream.write("\n")
    print(json.dumps({"records": len(records), "bridge": str(output), "seal": seal}, sort_keys=True))


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("paths", nargs="*", type=Path)
    parser.add_argument("--vectors", action="store_true")
    parser.add_argument("--self-test", action="store_true")
    parser.add_argument("--bridge", type=Path)
    parser.add_argument("--golden", type=Path)
    parser.add_argument("--repository", type=Path, default=Path(__file__).resolve().parents[1])
    args = parser.parse_args()
    if args.self_test:
        self_test()
    if args.bridge:
        if not args.golden:
            parser.error("--bridge requires --golden")
        bridge(args.repository, args.golden, args.bridge)
    if args.vectors:
        for size in (0, 1, 23, 24, 31, 32, 55, 56, 63, 64, 4095, 4096, 4097,
                     65535, 65536, 65537, 1048576, 1048577):
            print(json.dumps(merkle(pattern(size)), sort_keys=True))
    for path in args.paths:
        payload = path.read_bytes()
        print(json.dumps({"path": str(path.resolve()), "v1": {"digest_format": "sha256-bytes",
                         "digest_version": 1, "sha256": hashlib.sha256(payload).hexdigest()},
                          "v2": merkle(payload)}, sort_keys=True))


if __name__ == "__main__":
    main()
