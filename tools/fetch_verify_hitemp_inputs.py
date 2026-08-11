#!/usr/bin/env python3
"""Fetch public HITRAN metadata and verify owner-local HITEMP line archives.

This script never downloads, copies, or writes HITEMP line-list bytes.  The
owner supplies them under --input-root; only separately public partition-sum
metadata may be fetched when its manifest entry carries a pinned digest.
"""

from __future__ import annotations

import argparse
import json
import os
import tempfile
import urllib.request
from pathlib import Path

from fire_gas_opacity import sha256_file
from generate_fire_gas_opacity_record import load_verified_manifest


def fetch_public_partition_sums(manifest_path: Path, input_root: Path) -> None:
    manifest = json.loads(manifest_path.read_text(encoding="utf-8"))
    for source in manifest.get("sources", []):
        for entry in source.get("partition_sums", {}).get("files", []):
            destination = input_root / entry.get("path", "")
            if destination.is_file():
                continue
            url = entry.get("fetch_url")
            digest = entry.get("sha256", "")
            if not isinstance(url, str) or not url.startswith("https://") or len(digest) != 64:
                raise ValueError("missing partition sum has no pinned HTTPS fetch boundary")
            destination.parent.mkdir(parents=True, exist_ok=True)
            descriptor, temporary_name = tempfile.mkstemp(
                prefix="rise-hitran-q-", dir=str(destination.parent))
            os.close(descriptor)
            temporary = Path(temporary_name)
            try:
                urllib.request.urlretrieve(url, temporary)
                if sha256_file(temporary) != digest:
                    raise ValueError(
                        f"downloaded partition-sum SHA-256 mismatch for {source['species']}")
                temporary.replace(destination)
            finally:
                if temporary.exists():
                    temporary.unlink()


def main() -> None:
    parser = argparse.ArgumentParser()
    parser.add_argument("--input-root", required=True, type=Path)
    parser.add_argument("--fetch-public-partition-sums", action="store_true")
    parser.add_argument("manifest", type=Path)
    args = parser.parse_args()
    if args.fetch_public_partition_sums:
        fetch_public_partition_sums(args.manifest, args.input_root)
    manifest, _ = load_verified_manifest(args.manifest, args.input_root)
    file_count = sum(len(source["files"]) for source in manifest["sources"])
    q_count = sum(len(source["partition_sums"]["files"])
                  for source in manifest["sources"])
    print(f"verified {file_count} hash-pinned line archive(s) and "
          f"{q_count} partition-sum input(s)")


if __name__ == "__main__":
    main()
