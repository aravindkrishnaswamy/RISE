#!/usr/bin/env python3
"""Fetch only hash-pinned HyLIoS public data artifacts into an explicit cache.

This utility does not execute downloaded files or participate in rendering.
"""
from __future__ import annotations

import argparse
import hashlib
import json
import os
import re
import sys
import tempfile
import urllib.request
from pathlib import Path, PurePosixPath
from typing import Any, Callable
from urllib.parse import urlparse

ROOT = Path(__file__).resolve().parents[1]
MANIFEST_PATH = ROOT / "data/hylios/source_manifest.json"
ALLOWED_MANIFEST_IDS = {
    "keratin_abs", "dna_abs", "uro_acid_abs", "eumelanin_extinction",
    "pheomelanin_extinction", "oxy_deoxy_hemoglobin", "carboxyhemoglobin",
    "methemoglobin", "sulfhemoglobin", "beta_carotene", "bilirubin",
    "water_absorption", "lipid_absorption", "palmer_williams_real_ior",
}
MAX_ARTIFACT_BYTES = 16 * 1024 * 1024
DEFAULT_TIMEOUT_SECONDS = 20
EXPECTED_HOST = "www.npsg.uwaterloo.ca"
SHA256_RE = re.compile(r"^[0-9a-f]{64}$")


class FetchError(ValueError):
    pass


def _require(condition: bool, message: str) -> None:
    if not condition:
        raise FetchError(message)


def _digest(data: bytes) -> str:
    return hashlib.sha256(data).hexdigest()


def _validate_relative_path(relative: str) -> PurePosixPath:
    _require(isinstance(relative, str) and relative != "", "artifact path must be a non-empty string")
    _require("\\" not in relative, f"artifact path must use POSIX separators: {relative!r}")
    path = PurePosixPath(relative)
    _require(not path.is_absolute() and all(part not in {"", ".", ".."} for part in path.parts),
             f"artifact path must be a clean relative path: {relative!r}")
    return path


def _validate_url(url: str) -> None:
    parsed = urlparse(url)
    _require(parsed.scheme == "https" and parsed.hostname == EXPECTED_HOST and
             parsed.username is None and parsed.password is None and not parsed.fragment,
             f"artifact URL must be HTTPS on {EXPECTED_HOST}: {url!r}")


def collect_artifacts(manifest: dict[str, Any]) -> dict[str, tuple[str, str]]:
    """Return relative-path -> (URL, SHA256), rejecting incomplete/conflicting inventory."""
    entries: dict[str, dict[str, Any]] = {}
    for section in ("author_data_index", "author_water_data_index"):
        for entry in manifest.get(section, {}).get("items", []):
            if entry.get("id") in ALLOWED_MANIFEST_IDS:
                _require(entry["id"] not in entries, f"duplicate manifest item id: {entry['id']}")
                entries[entry["id"]] = entry
    _require(set(entries) == ALLOWED_MANIFEST_IDS,
             f"manifest supported-source inventory mismatch; missing={sorted(ALLOWED_MANIFEST_IDS-set(entries))}, extra={sorted(set(entries)-ALLOWED_MANIFEST_IDS)}")
    artifacts: dict[str, tuple[str, str]] = {}
    for entry_id, entry in entries.items():
        files, hashes, urls = entry.get("files"), entry.get("sha256"), entry.get("download_urls")
        _require(isinstance(files, list) and isinstance(hashes, list) and isinstance(urls, list) and
                 len(files) == len(hashes) == len(urls) and len(files) > 0,
                 f"{entry_id}: files, sha256, and download_urls must be aligned non-empty lists")
        for relative, digest, url in zip(files, hashes, urls):
            path = str(_validate_relative_path(relative))
            _require(isinstance(digest, str) and SHA256_RE.fullmatch(digest),
                     f"{entry_id}: invalid SHA256 for {relative!r}")
            _validate_url(url)
            item = (url, digest)
            if path in artifacts:
                _require(artifacts[path] == item,
                         f"conflicting duplicate artifact path {path!r}: URL or digest differs")
            else:
                artifacts[path] = item
    return dict(sorted(artifacts.items()))


def _destination(root: Path, relative: str) -> Path:
    resolved_root = root.resolve()
    destination = (resolved_root / relative).resolve()
    _require(destination.is_relative_to(resolved_root),
             f"artifact path resolves outside output root: {relative!r}")
    return destination


def _check_cached(path: Path, expected_sha: str) -> bool:
    if not path.exists():
        return False
    _require(path.is_file(), f"existing cache path is not a regular file: {path}")
    _require(path.stat().st_size <= MAX_ARTIFACT_BYTES,
             f"cached artifact exceeds {MAX_ARTIFACT_BYTES} byte limit: {path}")
    actual = _digest(path.read_bytes())
    _require(actual == expected_sha,
             f"existing cached file has SHA256 {actual}, expected {expected_sha}: {path}")
    return True


def _fetch_one(url: str, destination: Path, expected_sha: str,
               opener: Callable[..., Any], timeout: int) -> None:
    destination.parent.mkdir(parents=True, exist_ok=True)
    descriptor, temporary_name = tempfile.mkstemp(prefix="hylios-fetch-", dir=str(destination.parent))
    os.close(descriptor)
    temporary = Path(temporary_name)
    try:
        request = urllib.request.Request(url, headers={"User-Agent": "RISE-HyLIoS-source-cache/1"})
        with opener(request, timeout=timeout) as response, temporary.open("wb") as output:
            status = getattr(response, "status", 200)
            _require(status == 200, f"unexpected HTTP status {status} for {url}")
            final_url = response.geturl() if hasattr(response, "geturl") else url
            _validate_url(final_url)
            digest = hashlib.sha256()
            total = 0
            while True:
                chunk = response.read(64 * 1024)
                if not chunk:
                    break
                total += len(chunk)
                _require(total <= MAX_ARTIFACT_BYTES,
                         f"artifact exceeds {MAX_ARTIFACT_BYTES} byte limit: {url}")
                digest.update(chunk)
                output.write(chunk)
            _require(digest.hexdigest() == expected_sha,
                     f"download SHA256 mismatch for {url}: expected {expected_sha}, got {digest.hexdigest()}")
        # Atomic create-if-absent, unlike replace(): a concurrent writer must
        # not lose its bytes between an existence check and publication.
        try:
            os.link(temporary, destination)
        except FileExistsError:
            _require(_check_cached(destination, expected_sha),
                     f"destination appeared during download and is invalid: {destination}")
    finally:
        if temporary.exists():
            temporary.unlink()


def fetch_artifacts(manifest: dict[str, Any], output_root: Path,
                    opener: Callable[..., Any] = urllib.request.urlopen,
                    timeout: int = DEFAULT_TIMEOUT_SECONDS) -> tuple[int, int]:
    artifacts = collect_artifacts(manifest)
    root = output_root.resolve()
    destinations = {relative: _destination(root, relative) for relative in artifacts}
    # Validate all existing cache entries before downloading anything.
    cached = {relative: _check_cached(destinations[relative], digest)
              for relative, (_, digest) in artifacts.items()}
    downloaded = 0
    for relative, (url, digest) in artifacts.items():
        if cached[relative]:
            continue
        # Recheck resolved containment immediately before creating the destination parent.
        destination = _destination(root, relative)
        _fetch_one(url, destination, digest, opener, timeout)
        downloaded += 1
    return downloaded, sum(cached.values())


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--output", type=Path, required=True, help="explicit external source cache directory")
    parser.add_argument("--manifest", type=Path, default=MANIFEST_PATH,
                        help="pinned source manifest (defaults to repository data/hylios/source_manifest.json)")
    args = parser.parse_args()
    try:
        manifest = json.loads(args.manifest.read_text(encoding="utf-8"))
        downloaded, reused = fetch_artifacts(manifest, args.output)
    except (OSError, json.JSONDecodeError, KeyError, FetchError) as exc:
        print(f"ERROR: {exc}", file=sys.stderr)
        return 2
    print(f"Downloaded {downloaded} artifacts; reused {reused} checksum-verified cached artifacts in {args.output}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
