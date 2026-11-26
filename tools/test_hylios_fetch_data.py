"""Mocked tests for manifest-pinned, explicit HyLIoS source acquisition."""
from __future__ import annotations

import copy
import hashlib
import io
import json
import sys
import tempfile
import unittest
from unittest.mock import patch
from pathlib import Path
from email.message import Message
from urllib.request import BaseHandler, ProxyHandler, Request, addinfourl, build_opener

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


class RedirectTransport(BaseHandler):
    handler_order = 100

    def __init__(self, redirect_from: str, redirect_to: str, payload: bytes):
        self.redirect_from = redirect_from
        self.redirect_to = redirect_to
        self.payload = payload
        self.dispatched = []
        self.responses = []

    def _response(self, request, code, location=None):
        headers = Message()
        if location:
            headers["Location"] = location
        response = addinfourl(io.BytesIO(self.payload if code == 200 else b""),
                              headers, request.full_url, code=code)
        response.msg = "Found" if code == 302 else "OK"
        self.responses.append(response)
        return response

    def close(self):
        for response in self.responses:
            response.close()

    def https_open(self, request):
        self.dispatched.append(request.full_url)
        if request.full_url == self.redirect_from:
            return self._response(request, 302, self.redirect_to)
        return self._response(request, 200)

    def http_open(self, request):
        self.dispatched.append(request.full_url)
        return self._response(request, 200)


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
            section = ("author_water_data_index" if item_id in
                       {"palmer_williams_real_ior", "hale_querry_water_real_ior", "hale_querry_water_extinction"}
                       else "author_data_index")
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
        self.assertEqual((downloaded, reused), (16, 0))
        self.assertEqual(len(calls), 16)
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
        self.assertEqual(fetcher.fetch_artifacts(self.manifest, self.root, opener=opener), (0, 16))
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

    def test_repository_inventory_adds_only_three_pinned_hale_querry_artifacts(self):
        manifest = json.loads(fetcher.MANIFEST_PATH.read_text())
        artifacts = fetcher.collect_artifacts(manifest)
        self.assertEqual(len(artifacts), 33)
        expected = {
            "water/water2_n.m": "ef4b8b77e910612c59af10acc470152c560f73f59845402577bccc6df55be280",
            "water/water2_k.m": "cb0c5f1fe9f58683e542f031c577ff1db5e1feb473d06f35487acbb2cfd43456",
            "water/ecW2.txt": "22a13caf095a5dc8fdb8c6f4c13f08b4a5d0e5a75a6b482a1c20729459fa964c",
        }
        for relative, digest in expected.items():
            self.assertEqual(artifacts[relative], ("https://www.npsg.uwaterloo.ca/data/" + relative, digest))
        self.assertNotIn("water/riW2.txt", artifacts)  # IAPWS filename alias, not Hale-Querry.
        self.assertNotIn("water/water2.m", artifacts)  # Unselected IAPWS source.

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

    def test_real_urllib_redirect_blocks_foreign_request_before_dispatch(self):
        initial = "https://www.npsg.uwaterloo.ca/data/initial.m"
        foreign = "http://foreign.example/data/final.m"
        transport = RedirectTransport(initial, foreign, self.payload)
        # RED proof: urllib's legacy default redirect chain dispatches the foreign request.
        try:
            legacy = build_opener(ProxyHandler({}), transport)
            with legacy.open(Request(initial), timeout=2) as response:
                response.read()
            self.assertEqual(transport.dispatched, [initial, foreign])

            transport.dispatched.clear()
            original_build = build_opener
            def intercepted_build(*handlers):
                return original_build(ProxyHandler({}), transport, *handlers)
            with patch.object(fetcher.urllib.request, "build_opener", side_effect=intercepted_build):
                with self.assertRaisesRegex(fetcher.FetchError, "must be HTTPS"):
                    fetcher._default_open(Request(initial), timeout=2)
            # Protected path stops after the 302; the foreign URL is never requested.
            self.assertEqual(transport.dispatched, [initial])
        finally:
            transport.close()

    def test_real_urllib_allows_redirect_within_approved_https_origin(self):
        initial = "https://www.npsg.uwaterloo.ca/data/start.m"
        final = "https://www.npsg.uwaterloo.ca/data/final.m"
        transport = RedirectTransport(initial, final, self.payload)
        original_build = build_opener
        def intercepted_build(*handlers):
            return original_build(ProxyHandler({}), transport, *handlers)
        try:
            with patch.object(fetcher.urllib.request, "build_opener", side_effect=intercepted_build):
                response = fetcher._default_open(Request(initial), timeout=2)
            self.assertEqual(transport.dispatched, [initial, final])
            self.assertEqual(response.read(), self.payload)
            response.close()
        finally:
            transport.close()

    def test_rejects_dot_empty_and_windows_drive_cache_paths(self):
        for path in (".", "skin//file.m", "skin/./file.m", "C:/cache/file.m"):
            with self.subTest(path=path):
                bad = copy.deepcopy(self.manifest)
                bad["author_data_index"]["items"][0]["files"][0] = path
                with self.assertRaises(fetcher.FetchError):
                    fetcher.collect_artifacts(bad)

    def test_rejects_redirect_to_nonapproved_host_and_cleans_temp(self):
        opener, _ = self.opener_for(url_transform=lambda _url: "https://example.com/redirected")
        with self.assertRaisesRegex(fetcher.FetchError, "must be HTTPS"):
            fetcher.fetch_artifacts(self.manifest, self.root, opener=opener)
        self.assertFalse(list(self.root.rglob("hylios-fetch-*")))


if __name__ == "__main__":
    unittest.main()
