#!/usr/bin/env python3
"""Render the guilloche watch dial from each named pinhole_camera in
watch_dial.RISEscene, one PNG per angle.

Why a helper: RISE's CLI `render` command renders the ACTIVE camera, and the
active camera is simply the LAST one added (there is no `camera <name>` CLI
command -- camera switching is a GUI / Job::SetActiveCamera affordance).  So to
render every angle from the command line we emit one temp scene per camera, each
containing exactly that one camera (hence active) and its own output filename.
The durable scene keeps ALL cameras for the GUI and for documentation.

Usage:
  python3 tools/render_watch_views.py                       # all cameras
  python3 tools/render_watch_views.py --cam cam_face cam_graze
  python3 tools/render_watch_views.py --samples 32 --res 700x700
Outputs: rendered/watch_<camname>.png
"""
import argparse
import os
import re
import subprocess
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
SCENE = os.path.join(ROOT, "scenes/FeatureBased/Materials/watch_dial.RISEscene")
RISE = os.path.join(ROOT, "bin/rise")
TMPDIR = "/tmp/watch_views"


def extract_cameras(text):
    """Split scene text into (body_without_cameras, [(name, block_text), ...])."""
    lines = text.split("\n")
    body, cams = [], []
    i = 0
    while i < len(lines):
        if lines[i].strip() == "pinhole_camera":
            block = [lines[i]]
            depth, opened, j = 0, False, i + 1
            while j < len(lines):
                block.append(lines[j])
                s = lines[j].strip()
                if s == "{":
                    depth += 1
                    opened = True
                elif s == "}":
                    depth -= 1
                    if opened and depth == 0:
                        break
                j += 1
            blocktext = "\n".join(block)
            m = re.search(r"^\s*name\s+(\S+)", blocktext, re.M)
            cams.append((m.group(1) if m else "default", blocktext))
            i = j + 1
        else:
            body.append(lines[i])
            i += 1
    return "\n".join(body), cams


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--samples", type=int, default=20, help="override rasterizer samples (default 20, fast diagnostic)")
    ap.add_argument("--res", default="600x600", help="WxH film override (default 600x600)")
    ap.add_argument("--cam", nargs="+", default=None, help="only these camera names")
    ap.add_argument("--oxide-bias", type=float, default=None, help="torch START thickness nm (centre); patches oxide_thk bias for a heat-tint sweep demo")
    ap.add_argument("--oxide-scale", type=float, default=None, help="torch SPAN nm (rim = bias+scale); patches oxide_thk scale")
    ap.add_argument("--suffix", default="", help="append to each output filename (e.g. for a torch-setting sweep)")
    ap.add_argument("--ar-thickness", type=float, default=None, help="override the crystal AR coating thickness nm (0 = AR off) for an A/B")
    args = ap.parse_args()
    W, H = args.res.lower().split("x")

    if not os.path.exists(RISE):
        sys.exit("error: %s not built (make -C build/make/rise -j8 all)" % RISE)

    text = open(SCENE).read()
    body, cams = extract_cameras(text)
    if not cams:
        sys.exit("error: no pinhole_camera chunks found in scene")
    if args.cam:
        want = set(args.cam)
        cams = [c for c in cams if c[0] in want]
        if not cams:
            sys.exit("error: no cameras matched %s" % args.cam)

    os.makedirs(TMPDIR, exist_ok=True)
    os.makedirs(os.path.join(ROOT, "rendered"), exist_ok=True)
    env = dict(os.environ, RISE_MEDIA_PATH=ROOT + "/")

    for name, block in cams:
        scene = body
        outname = "watch_%s%s" % (name, args.suffix)
        scene = re.sub(r"(pattern\s+)\S+", r"\1rendered/%s" % outname, scene, count=1)
        scene = re.sub(r"(?m)^(\s*samples\s+)\d+", r"\g<1>%d" % args.samples, scene, count=1)
        scene = re.sub(r"(?m)^(\s*width\s+)\d+", r"\g<1>%s" % W, scene, count=1)
        scene = re.sub(r"(?m)^(\s*height\s+)\d+", r"\g<1>%s" % H, scene, count=1)
        # Optional torch-tint sweep: patch the oxide_thk painter's bias/scale.
        if args.oxide_bias is not None:
            scene = re.sub(r"(name\s+oxide_thk\b.*?bias\s+)[0-9.]+", r"\g<1>%g" % args.oxide_bias, scene, count=1, flags=re.S)
        if args.oxide_scale is not None:
            scene = re.sub(r"(name\s+oxide_thk\b.*?scale\s+)[0-9.]+", r"\g<1>%g" % args.oxide_scale, scene, count=1, flags=re.S)
        if args.ar_thickness is not None:
            scene = re.sub(r"(ar_film_thickness\s+)[0-9.]+", r"\g<1>%g" % args.ar_thickness, scene, count=1)
        # The one camera, appended last -> active.
        scene = scene.rstrip() + "\n\n" + block + "\n"
        tmp = os.path.join(TMPDIR, "watch_%s.RISEscene" % name)
        open(tmp, "w").write(scene)

        print("=== %s -> rendered/watch_%s.png (samples=%d res=%sx%s) ===" % (name, name, args.samples, W, H), flush=True)
        r = subprocess.run([RISE, tmp], input="render\nquit\n", text=True,
                           env=env, capture_output=True, cwd=ROOT)
        tail = [ln for ln in r.stdout.splitlines() if ln.strip()][-5:]
        print("\n".join(tail), flush=True)
        if r.returncode != 0 and "Rasterization Time" not in r.stdout:
            print("  !! exit=%d stderr: %s" % (r.returncode, r.stderr[-300:]), flush=True)


if __name__ == "__main__":
    main()
