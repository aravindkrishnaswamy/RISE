#!/usr/bin/env python3
"""Render the guilloche watch dial from each named camera (pinhole AND thinlens)
in watch_dial.RISEscene, one PNG per angle.

Why a helper: RISE's CLI `render` command renders the ACTIVE camera, and the
active camera is simply the LAST one added (there is no `camera <name>` CLI
command -- camera switching is a GUI / Job::SetActiveCamera affordance).  So to
render every angle from the command line we emit one temp scene per camera, each
containing exactly that one camera (hence active) and its own output filename.
The durable scene keeps ALL cameras for the GUI and for documentation.

Usage:
  python3 tools/render_watch_views.py                       # all cameras
  python3 tools/render_watch_views.py --cam cam_macro cam_profile
  python3 tools/render_watch_views.py --samples 32 --res 700x700
Outputs: rendered/watch_<camname>.png
"""
import argparse
import os
import re
import subprocess
import sys

HERE = os.path.dirname(os.path.abspath(__file__))             # the GuillocheWatch scene folder
ROOT = os.path.abspath(os.path.join(HERE, "..", "..", ".."))  # repo root (for bin/rise + rendered/)
SCENE = os.path.join(HERE, "watch_dial.RISEscene")
RISE = os.path.join(ROOT, "bin/rise")
TMPDIR = "/tmp/watch_views"


CAMERA_CHUNKS = ("pinhole_camera", "thinlens_camera")
# timeline/animation_options target the scene's animated camera; strip them for a
# single still (a timeline whose target camera was stripped fails to load, which
# aborts the parse and leaves the scene with no camera).
DROP_CHUNKS = ("timeline", "animation_options")


def extract_cameras(text):
    """Split scene text into (body_without_cameras, [(name, block_text), ...]).

    Matches BOTH pinhole_camera and thinlens_camera so the macro/hero thinlens
    rigs render too.  All cameras are pulled out of the body; each is appended
    back individually (hence last -> active) for its own render.
    """
    lines = text.split("\n")
    body, cams = [], []
    i = 0
    while i < len(lines):
        kw = lines[i].strip()
        if kw in CAMERA_CHUNKS or kw in DROP_CHUNKS:
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
            if kw in CAMERA_CHUNKS:
                blocktext = "\n".join(block)
                m = re.search(r"^\s*name\s+(\S+)", blocktext, re.M)
                cams.append((m.group(1) if m else "default", blocktext))
            # DROP_CHUNKS (timeline / animation_options) are consumed and discarded
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
        # the scene sets `multiple TRUE` for renderanimation frame-numbering; force it
        # off here so single-camera stills stay un-numbered (rendered/watch_<cam>.png).
        scene = re.sub(r"(?m)^(\s*multiple\s+)\w+", r"\g<1>FALSE", scene)
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
