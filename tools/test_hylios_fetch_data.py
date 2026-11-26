"""Mocked tests for manifest-pinned, explicit HyLIoS source acquisition."""
from __future__ import annotations

import copy
import hashlib
import sys
import tempfile
import unittest
from unittest.mock import patch
from pathlib import Path
from urllib.request import Request

sys.path.insert(0, str(Path(__file__).resolve().parent))

import hylios_fetch_data as fetcher


class FakeResponse:
    def __init__(self, payload: bytes, url: str, status: int = 200):
        self.payload = payload
        self.url = url
        self.status = status
        self.offset = 0

    def __enter__(self):
        return self

    def __exit__(self, *_args):
        return False

    def geturl(self):
        return self.url

    def read(self, size=-1):
        if size < 0:
            size = len(self.payload)
        part = self.payload[self.offset:self.offset + size]
        self.offset += len(part)
        return part


class HyLIoSFetchDataTest(unittest.TestCase):
    def test_concurrent_publication_does_not_replace_other_bytes(self):
        destination = self.root / "raced.m"
        opener, _ = self.opener_for()
        def race(_temporary, target):
            Path(target).write_bytes(b"another writer's bytes")
            raise FileExistsError()
        with patch.object(fetcher.os, "link", side_effect=race):
            with self.assertRaisesRegex(fetcher.FetchError, "existing cached file has SHA256"):
                fetcher._fetch_one("https://www.npsg.uwaterloo.ca/data/raced.m",
                                  destination, self.digest, opener, 20)
        self.assertEqual(destination.read_bytes(), b"another writer's bytes")
        self.assertFalse(list(self.root.rglob("hylios-fetch-*")))

    def test_cached_files_obey_size_limit(self):
        _, relative = self.first_item()
        destination = self.root / relative
        destination.parent.mkdir(parents=True)
        destination.write_bytes(self.payload)
        opener, calls = self.opener_for()
        with patch.object(fetcher, "MAX_ARTIFACT_BYTES", len(self.payload) - 1):
            with self.assertRaisesRegex(fetcher.FetchError, "cached artifact exceeds"):
                fetcher.fetch_artifacts(self.manifest, self.root, opener=opener)
        self.assertFalse(calls)

    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.root = Path(self.tmp.name) / "cache"
        self.payload = b"pinned source fixture\n"
        self.digest = hashlib.sha256(self.payload).hexdigest()
        ids = sorted(fetcher.ALLOWED_MANIFEST_IDS)
        self.manifest = {"author_data_index":{"items":[]},"author_water_data_index":{"items":[]}}
        for index, item_id in enumerate(ids):
            section = "author_water_data_index" if item_id == "palmer_williams_real_ior" else "author_data_index"
            rel = f"fixture/{index:02d}-{item_id}.m"
            self.manifest[section]["items"].append({
                "id": item_id,
                "files": [rel],
                "sha256": [self.digest],
                "download_urls": [f"https://www.npsg.uwaterloo.ca/data/{rel}"],
            })

    def tearDown(self):
        self.tmp.cleanup()

    def opener_for(self, payload=None, url_transform=None, status=200):
        payload = self.payload if payload is None else payload
        calls = []
        def opener(request: Request, timeout: int):
            calls.append((request.full_url, timeout))
            url = url_transform(request.full_url) if url_transform else request.full_url
            return FakeResponse(payload, url, status)
        return opener, calls

    def first_item(self):
        item = next(iter(self.manifest["author_data_index"]["items"]))
        return item, item["files"][0]

    def test_downloads_and_publishes_only_after_checksum_match(self):
        opener, calls = self.opener_for()
        downloaded, reused = fetcher.fetch_artifacts(self.manifest, self.root, opener=opener)
        self.assertEqual((downloaded, reused), (14, 0))
        self.assertEqual(len(calls), 14)
        for section in self.manifest.values():
            for item in section["items"]:
                path = self.root / item["files"][0]
                self.assertEqual(hashlib.sha256(path.read_bytes()).hexdigest(), item["sha256"][0])
        self.assertFalse(list(self.root.rglob("hylios-fetch-*")))

    def test_reuses_all_hash_matching_cached_files_without_network(self):
        for section in self.manifest.values():
            for item in section["items"]:
                path = self.root / item["files"][0]
                path.parent.mkdir(parents=True, exist_ok=True)
                path.write_bytes(self.payload)
        opener, calls = self.opener_for()
        self.assertEqual(fetcher.fetch_artifacts(self.manifest, self.root, opener=opener), (0, 14))
        self.assertEqual(calls, [])

    def test_existing_mismatched_file_is_never_overwritten(self):
        _, relative = self.first_item()
        destination = self.root / relative
        destination.parent.mkdir(parents=True)
        destination.write_bytes(b"keep this mismatched file")
        opener, calls = self.opener_for()
        with self.assertRaisesRegex(fetcher.FetchError, "existing cached file has SHA256"):
            fetcher.fetch_artifacts(self.manifest, self.root, opener=opener)
        self.assertEqual(destination.read_bytes(), b"keep this mismatched file")
        self.assertEqual(calls, [])

    def test_rejects_traversal_before_network_or_writes(self):
        bad = copy.deepcopy(self.manifest)
        item, _ = self.first_item()
        bad_item = next(x for x in bad["author_data_index"]["items"] if x["id"] == item["id"])
        bad_item["files"][0] = "../outside.m"
        opener, calls = self.opener_for()
        with self.assertRaisesRegex(fetcher.FetchError, "clean relative path"):
            fetcher.fetch_artifacts(bad, self.root, opener=opener)
        self.assertEqual(calls, [])
        self.assertFalse(self.root.exists())

    def test_rejects_url_outside_manifest_host(self):
        bad = copy.deepcopy(self.manifest)
        bad["author_data_index"]["items"][0]["download_urls"][0] = "http://example.com/x"
        with self.assertRaisesRegex(fetcher.FetchError, "must be HTTPS"):
            fetcher.collect_artifacts(bad)

    def test_rejects_duplicate_path_with_conflicting_provenance(self):
        bad = copy.deepcopy(self.manifest)
        item = bad["author_data_index"]["items"][0]
        item["files"].append(item["files"][0])
        item["sha256"].append(item["sha256"][0])
        item["download_urls"].append("https://www.npsg.uwaterloo.ca/data/other.m")
        with self.assertRaisesRegex(fetcher.FetchError, "conflicting duplicate artifact path"):
            fetcher.collect_artifacts(bad)

    def test_hash_mismatch_cleans_temporary_file(self):
        opener, _ = self.opener_for(b"wrong data")
        with self.assertRaisesRegex(fetcher.FetchError, "download SHA256 mismatch"):
            fetcher.fetch_artifacts(self.manifest, self.root, opener=opener)
        self.assertFalse(list(self.root.rglob("hylios-fetch-*")))
        self.assertFalse(any(path.is_file() for path in self.root.rglob("*.m")))

    def test_size_limit_cleans_temporary_file(self):
        original = fetcher.MAX_ARTIFACT_BYTES
        fetcher.MAX_ARTIFACT_BYTES = 4
        try:
            opener, _ = self.opener_for(b"too large")
            with self.assertRaisesRegex(fetcher.FetchError, "exceeds 4 byte limit"):
                fetcher.fetch_artifacts(self.manifest, self.root, opener=opener)
            self.assertFalse(list(self.root.rglob("hylios-fetch-*")))
            self.assertFalse(any(path.is_file() for path in self.root.rglob("*.m")))
        finally:
            fetcher.MAX_ARTIFACT_BYTES = original

    def test_rejects_redirect_to_nonapproved_host_and_cleans_temp(self):
        opener, _ = self.opener_for(url_transform=lambda _url: "https://example.com/redirected")
        with self.assertRaisesRegex(fetcher.FetchError, "must be HTTPS"):
            fetcher.fetch_artifacts(self.manifest, self.root, opener=opener)
        self.assertFalse(list(self.root.rglob("hylios-fetch-*")))


if __name__ == "__main__":
    unittest.main()
