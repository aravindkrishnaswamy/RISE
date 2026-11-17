#!/usr/bin/env python3
"""Census of one or more RISE agent trajectories (GUI chat sessions).

Reads the JSONL trajectory files the GUI writes under
~/Library/Application Support/RISE/trajectories/gui/ and prints, per run:

  * model, prompt head, turns, tool calls, input / cached tokens and the
    per-turn input-token growth (first, median, max) -- the context-volume
    signal;
  * which tool results dominate the history text (images excluded) and what
    the history_edit elisions reclaimed;
  * the final document's chunk census plus the geometry-toolkit adoption
    markers (two-axis point_scale, point_morph, aspect, mirror, source /
    count_u, superellipsoid, skin / hair / displaced geometry);
  * unreferenced (orphaned) geometry / material / painter chunks;
  * design-note clauses that fired (render notes + validate codes);
  * the smallest objects the last render's inventory reports, so "authored
    but too small to see" is a number.

With --images DIR, every render / finish_element / imagine_scene image is
decoded to DIR as PNG for eyeballing.

Delivery is not compliance: everything here is measured on the document and
the results, never on what a note asked for.

Usage:
    python3 -I tools/trajectory_census.py <trajectory.jsonl> [...] [--images DIR]
    python3 -I tools/trajectory_census.py --latest [N] [--images DIR]
"""

import base64
import collections
import json
import os
import re
import statistics
import sys

TRAJ_DIR = os.path.expanduser("~/Library/Application Support/RISE/trajectories/gui")
IMG_RE = re.compile(r"[A-Za-z0-9+/=]{2000,}")
CHUNK_RE = re.compile(r"^([a-z_0-9]+)\s*\n\{(.*?)\n\}", re.M | re.S)

ADOPTION_MARKERS = [
    ("point_scale sx sy", r"^\s*point_scale\s+\S+\s+\S+\s*$"),
    ("point_morph", r"^\s*point_morph\s"),
    ("joint aspect", r"^\s*joint\s+(?:\S+\s+){6}\S+\s+\S+\s*$"),
    ("mirror", r"^\s*mirror\s+[xyz]\s*$"),
    ("source", r"^\s*source\s"),
    ("count_u", r"^\s*count_u\s"),
    ("superellipsoid part", r"^\s*part\s+superellipsoid\s"),
]
ORPHAN_KINDS = ("_geometry", "_material", "_painter", "_function2d", "_function")


def load(path):
    out = []
    with open(path) as f:
        for line in f:
            line = line.strip()
            if not line:
                continue
            try:
                out.append(json.loads(line))
            except ValueError:
                pass
    return out


def text_len(obj):
    s = json.dumps(obj)
    return len(IMG_RE.sub("", s)), sum(len(m) for m in IMG_RE.findall(s))


def chunks(doc):
    for m in CHUNK_RE.finditer(doc):
        body = m.group(2)
        nm = re.search(r"^\s*name\s+(\S+)", body, re.M)
        yield m.group(1), (nm.group(1) if nm else None), body


def census(path, image_dir=None):
    L = load(path)
    by = collections.defaultdict(list)
    for x in L:
        by[x.get("run_type")].append(x)
    sess = (by["session"] or [{}])[0]
    summ = (by["summary"] or [{}])[0]
    print("=" * 78)
    print(os.path.basename(path))
    print("model:", sess.get("gen_ai.request.model"), " provider:", sess.get("provider"))
    for u in by["user"]:
        print("USER:", (u.get("text") or "").split("\n\n[Background")[0][:300].replace("\n", " "))

    toks = [int(x.get("gen_ai.usage.input_tokens", -1)) for x in by["llm"]]
    toks = [t for t in toks if t > 0]
    print("turns(llm calls): %d  tool calls: %d  input tokens: %s  cached: %s  output: %s" % (
        len(by["llm"]), len(by["tool"]), summ.get("gen_ai.usage.input_tokens"),
        summ.get("gen_ai.usage.cache_read_input_tokens"), summ.get("gen_ai.usage.output_tokens")))
    if toks:
        print("per-turn input tokens: first %d  median %d  max %d" % (
            toks[0], int(statistics.median(toks)), max(toks)))

    seq = collections.Counter(x.get("name") for x in by["tool"])
    print("tools:", ", ".join("%s x%d" % kv for kv in seq.most_common()))

    # Persistent tool-result text by tool (images excluded).
    agg = collections.defaultdict(lambda: [0, 0])
    for x in by["tool"]:
        t, _ = text_len(x.get("jsonrpc.response"))
        a = agg[x.get("name")]
        a[0] += 1
        a[1] += t
    top = sorted(agg.items(), key=lambda kv: -kv[1][1])[:8]
    print("largest result text:", ", ".join("%s %dKB/%d" % (k, v[1] // 1024, v[0]) for k, v in top))
    edits = collections.defaultdict(lambda: [0, 0])
    for x in by["history_edit"]:
        e = edits[x.get("reason")]
        e[0] += 1
        e[1] += int(x.get("before_bytes", 0)) - int(x.get("after_bytes", 0))
    if edits:
        print("history edits:", ", ".join("%s x%d (-%dKB)" % (k, v[0], v[1] // 1024) for k, v in edits.items()))

    # Final document census.
    snaps = by["document_snapshot"]
    if snaps:
        doc = snaps[-1].get("document_text", "")
        cs = list(chunks(doc))
        kinds = collections.Counter(k for k, _, _ in cs)
        print("final doc: rev %s, %d KB, %d chunks" % (
            snaps[-1].get("head_version_revision"), len(doc) // 1024, len(cs)))
        print("  kinds:", ", ".join("%s %d" % kv for kv in kinds.most_common(18)))
        marks = []
        for label, pat in ADOPTION_MARKERS:
            marks.append("%s %d" % (label, len(re.findall(pat, doc, re.M))))
        for k in ("skin_geometry", "hair_geometry", "displaced_geometry", "sdf_geometry",
                  "skeleton_geometry", "sweep_geometry", "lathe_geometry"):
            marks.append("%s %d" % (k, kinds.get(k, 0)))
        print("  adoption:", ", ".join(marks))
        names = [(k, n) for k, n, _ in cs if n and k.endswith(ORPHAN_KINDS)]
        orphans = []
        for k, n in names:
            refs = len(re.findall(r"(?<![\w.])" + re.escape(n) + r"(?![\w.])", doc))
            if refs <= 1:
                orphans.append(n)
        print("  orphaned geometry/material/painter chunks: %d%s" % (
            len(orphans), (" (" + ", ".join(orphans[:6]) + (" ..." if len(orphans) > 6 else "") + ")") if orphans else ""))

    # Design notes / diagnostics.
    clauses = collections.Counter()
    for x in by["tool"]:
        r = x.get("jsonrpc.response")
        res = r.get("result") if isinstance(r, dict) else None
        if not isinstance(res, dict):
            continue
        note = res.get("note")
        if isinstance(note, str) and note.startswith("DESIGN NOTE"):
            clauses["render-note"] += 1
        for d in res.get("diagnostics", []) or []:
            if isinstance(d, dict) and str(d.get("code", "")).startswith("DESIGN"):
                clauses[d["code"]] += 1
    if clauses:
        print("design notes:", ", ".join("%s x%d" % kv for kv in clauses.most_common()))

    # Smallest objects in the last render's inventory.
    renders = [x for x in by["tool"] if x.get("name") == "render"]
    for x in reversed(renders):
        s = json.dumps(x.get("jsonrpc.response"))
        inv = re.findall(r"\\n\s+(\S+) -- (\d+) px, ([\d.]+)% of frame", s)
        if inv:
            inv = sorted(inv, key=lambda t: int(t[1]))
            head = re.search(r"(\d+) objects; (\d+) covered at least one pixel and (\d+) covered none", s)
            print("last render inventory: %s objects, %s covered >=1 px, %s covered none; "
                  "smallest of the %d listed: %s" % (
                      head.group(1) if head else "?", head.group(2) if head else "?",
                      head.group(3) if head else "?", len(inv),
                      ", ".join("%s %spx" % (n, p) for n, p, _ in inv[:6])))
            break

    if image_dir:
        os.makedirs(image_dir, exist_ok=True)
        stem = os.path.basename(path).split(".")[0]
        i = 0
        for x in by["tool"]:
            if x.get("name") not in ("render", "finish_element", "imagine_scene", "render_isolate"):
                continue
            s = json.dumps(x.get("jsonrpc.response"))
            for m in IMG_RE.finditer(s):
                try:
                    data = base64.b64decode(m.group(0))
                except ValueError:
                    continue
                if not data.startswith(b"\x89PNG"):
                    continue
                with open(os.path.join(image_dir, "%s_%02d_%s.png" % (stem, i, x.get("name"))), "wb") as f:
                    f.write(data)
                i += 1
        print("images written:", i, "->", image_dir)


def main(argv):
    image_dir = None
    if "--images" in argv:
        k = argv.index("--images")
        image_dir = argv[k + 1]
        argv = argv[:k] + argv[k + 2:]
    paths = []
    if argv and argv[0] == "--latest":
        n = int(argv[1]) if len(argv) > 1 else 1
        files = sorted(f for f in os.listdir(TRAJ_DIR) if f.endswith(".jsonl"))
        paths = [os.path.join(TRAJ_DIR, f) for f in files[-n:]]
    else:
        paths = argv
    if not paths:
        print(__doc__)
        return 2
    for p in paths:
        census(p, image_dir)
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
