#!/usr/bin/env python3
"""extract_embedded_texture.py -- Phase 1 helper.

Extracts a single image from a .glb's BIN chunk and writes it as a
sidecar file next to the .glb.  RISE Phase 1's gltfmesh_geometry chunk
imports geometry only -- it does not auto-extract embedded textures.
The Phase 1 normal-map verification scene
(gltf_normal_mapped.RISEscene) needs the normal-map texture as a
sidecar PNG so it can be referenced via png_painter.

This is a Phase 1 / Phase 1.5 stopgap: when Phase 2 ships the
gltf_import chunk, the importer will extract embedded textures
automatically (see docs/GLTF_IMPORT.md "Phase 1.5 / Phase 2 cleanups"
table).  At that point this script and the committed sidecar PNG can
be deleted; the test scene will be rewritten to use gltf_import.

Usage (from repo root):

    python3 scenes/Tests/Geometry/assets/extract_embedded_texture.py \\
        scenes/Tests/Geometry/assets/NormalTangentMirrorTest.glb \\
        --material 0 --texture-kind normal \\
        -o scenes/Tests/Geometry/assets/NormalTangentMirrorTest_normal.png

The script is committed alongside the extracted PNG so future
maintainers can re-derive the sidecar from the source .glb without
guessing extraction parameters.
"""

import argparse
import json
import os
import struct
import sys


def load_glb(path):
    """Return (json_dict, bin_bytes) for a .glb file."""
    with open(path, "rb") as f:
        data = f.read()
    magic, version, total_len = struct.unpack_from("<4sII", data, 0)
    if magic != b"glTF" or version != 2:
        raise SystemExit(f"{path}: not a glTF 2.0 binary file")

    # JSON chunk.
    json_len, json_type = struct.unpack_from("<II", data, 12)
    if json_type != 0x4E4F534A:  # 'JSON' little-endian
        raise SystemExit(f"{path}: missing JSON chunk")
    j = json.loads(data[20 : 20 + json_len].decode("utf-8").rstrip())

    # Optional BIN chunk follows the JSON chunk.
    bin_offset = 20 + json_len
    bin_bytes = b""
    if bin_offset + 8 <= total_len:
        bin_len, bin_type = struct.unpack_from("<II", data, bin_offset)
        if bin_type == 0x004E4942:  # 'BIN\0' little-endian
            bin_bytes = data[bin_offset + 8 : bin_offset + 8 + bin_len]

    return j, bin_bytes


def slice_bufferview(j, bin_bytes, bv_index):
    bv = j["bufferViews"][bv_index]
    if bv.get("buffer", 0) != 0:
        raise SystemExit(
            f"bufferView {bv_index} references buffer "
            f"{bv['buffer']}; this script only handles the BIN chunk (buffer 0)"
        )
    off = bv.get("byteOffset", 0)
    return bin_bytes[off : off + bv["byteLength"]]


def resolve_image(j, material_index, texture_kind):
    """Return the images[] index for material[material_index]'s texture-of-kind."""
    mat = j["materials"][material_index]
    info_key = {
        "base_color": ("pbrMetallicRoughness", "baseColorTexture"),
        "normal": (None, "normalTexture"),
        "occlusion": (None, "occlusionTexture"),
        "emissive": (None, "emissiveTexture"),
        "metallic_roughness": ("pbrMetallicRoughness", "metallicRoughnessTexture"),
    }
    if texture_kind not in info_key:
        raise SystemExit(f"unknown texture kind: {texture_kind}")
    parent_key, child_key = info_key[texture_kind]
    container = mat.get(parent_key, {}) if parent_key else mat
    info = container.get(child_key)
    if info is None:
        raise SystemExit(
            f"material[{material_index}] has no {texture_kind} texture"
        )
    tex_index = info["index"]
    image_index = j["textures"][tex_index]["source"]
    return image_index


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("glb")
    ap.add_argument(
        "--material",
        type=int,
        default=0,
        help="Material index (default: 0)",
    )
    ap.add_argument(
        "--texture-kind",
        choices=("base_color", "normal", "occlusion", "emissive", "metallic_roughness"),
        default="normal",
    )
    ap.add_argument("-o", "--output", required=True, help="Output sidecar path")
    args = ap.parse_args()

    j, bin_bytes = load_glb(args.glb)

    image_index = resolve_image(j, args.material, args.texture_kind)
    img = j["images"][image_index]

    if "uri" in img and img["uri"]:
        raise SystemExit(
            f"image {image_index} is referenced by URI ({img['uri'][:60]}...); "
            "this script only extracts BIN-embedded images"
        )

    bv_index = img.get("bufferView")
    if bv_index is None:
        raise SystemExit(f"image {image_index} has no bufferView reference")

    payload = slice_bufferview(j, bin_bytes, bv_index)
    mime = img.get("mimeType", "application/octet-stream")

    # Sanity-check the output extension matches the actual mime type.
    out_ext = os.path.splitext(args.output)[1].lower()
    expected_ext = {"image/png": ".png", "image/jpeg": ".jpg"}.get(mime, "")
    if expected_ext and out_ext not in (expected_ext, ".jpeg"):
        sys.stderr.write(
            f"WARNING: output extension {out_ext!r} does not match mime type "
            f"{mime!r} (expected {expected_ext!r})\n"
        )

    with open(args.output, "wb") as f:
        f.write(payload)

    print(
        f"Extracted material[{args.material}].{args.texture_kind} from "
        f"{args.glb} -> {args.output} ({len(payload)} bytes, {mime})"
    )


if __name__ == "__main__":
    main()
