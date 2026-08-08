#!/usr/bin/env python3
"""Generate the source-state half of renderer_build_v1."""

from __future__ import annotations

import argparse
import hashlib
import pathlib
import subprocess


def git(repo: pathlib.Path, *args: str) -> bytes:
    return subprocess.check_output(
        ["git", "-C", str(repo), *args], stderr=subprocess.DEVNULL
    )


def c_string(value: str) -> str:
    return '"' + value.replace("\\", "\\\\").replace('"', '\\"') + '"'


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--repo-root", required=True, type=pathlib.Path)
    parser.add_argument("--output", required=True, type=pathlib.Path)
    args = parser.parse_args()
    repo = args.repo_root.resolve()
    revision = git(repo, "rev-parse", "HEAD").decode("ascii").strip()
    status = git(repo, "status", "--porcelain=v1", "-z", "--untracked-files=all")
    material = bytearray(b"RISE-renderer-dirty-state-v1\0")
    material.extend(status)
    material.extend(git(repo, "diff", "--binary", "HEAD", "--"))
    entries = status.split(b"\0")
    for entry in sorted(item for item in entries if item.startswith(b"?? ")):
        relative = entry[3:].decode("utf-8", "surrogateescape")
        path = repo / relative
        material.extend(relative.encode("utf-8", "surrogateescape"))
        material.extend(b"\0")
        if path.is_file():
            material.extend(path.read_bytes())
        material.extend(b"\0")
    dirty_hash = hashlib.sha256(material).hexdigest()
    content = (
        "#ifndef RISE_RENDERER_BUILD_IDENTITY_GENERATED_H\n"
        "#define RISE_RENDERER_BUILD_IDENTITY_GENERATED_H\n"
        f"#define RISE_BUILD_SOURCE_REVISION {c_string(revision)}\n"
        f"#define RISE_BUILD_DIRTY_STATE {c_string('dirty' if status else 'clean')}\n"
        f"#define RISE_BUILD_DIRTY_DIFF_SHA256 {c_string(dirty_hash)}\n"
        "#endif\n"
    )
    args.output.parent.mkdir(parents=True, exist_ok=True)
    if not args.output.exists() or args.output.read_text(encoding="utf-8") != content:
        args.output.write_text(content, encoding="utf-8")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
