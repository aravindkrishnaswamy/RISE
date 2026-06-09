#!/usr/bin/env python3
"""Render a dark-hero TURNTABLE of the guilloché watch: orbit the camera around
the +Z axis (the watch's own axis) at a fixed elevation, looking at the dial,
and write one frame per azimuth.  Optionally assemble an animated GIF.

The watch lies face-up (dial faces +Z), so a +Z-axis orbit walks the camera
around the head at a 3/4 elevation -- the dial, case, crown and lugs all rotate
through view while the world lighting stays fixed (highlights sweep across the
guilloché).  Lighting is the scene's (dark HDRI fill + top/bottom softboxes).

Usage:
  python3 tools/render_watch_turntable.py --frames 36 --samples 48 --res 720x720
  python3 tools/render_watch_turntable.py --frames 8 --samples 24 --res 480x480   # quick preview
  python3 tools/render_watch_turntable.py --gif                                     # assemble GIF after
Outputs: rendered/turntable/frame_###.png  (+ rendered/turntable.gif with --gif)
"""
import argparse
import math
import os
import re
import subprocess
import sys

HERE = os.path.dirname(os.path.abspath(__file__))             # the GuillocheWatch scene folder
ROOT = os.path.abspath(os.path.join(HERE, "..", "..", ".."))  # repo root (for bin/rise + rendered/)
SCENE = os.path.join(HERE, "watch_dial.RISEscene")
RISE = os.path.join(ROOT, "bin/rise")
OUTDIR = os.path.join(ROOT, "rendered", "turntable")


def strip_cameras(text):
    """Return the scene body with all pinhole_camera chunks removed."""
    lines = text.split("\n")
    out, i = [], 0
    while i < len(lines):
        if lines[i].strip() == "pinhole_camera":
            depth, opened, j = 0, False, i + 1
            while j < len(lines):
                st = lines[j].strip()
                if st == "{":
                    depth += 1; opened = True
                elif st == "}":
                    depth -= 1
                    if opened and depth == 0:
                        break
                j += 1
            i = j + 1
        else:
            out.append(lines[i]); i += 1
    return "\n".join(out)


def cam_chunk(loc, look, up, fov):
    t = "\t"
    return ("pinhole_camera\n{\n"
            f"{t}name{t}{t}{t}{t}turntable\n"
            f"{t}location{t}{t}{t}{loc}\n"
            f"{t}lookat{t}{t}{t}{t}{look}\n"
            f"{t}up{t}{t}{t}{t}{up}\n"
            f"{t}fov{t}{t}{t}{t}{fov}\n"
            "}\n")


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--frames", type=int, default=36, help="number of turntable frames (default 36 = 10deg steps)")
    ap.add_argument("--samples", type=int, default=48)
    ap.add_argument("--res", default="720x720")
    ap.add_argument("--radius", type=float, default=46.0, help="orbit radius (XY distance from axis)")
    ap.add_argument("--height", type=float, default=36.0, help="camera height above the watch plane (z)")
    ap.add_argument("--fov", type=float, default=42.0)
    ap.add_argument("--start-deg", type=float, default=0.0)
    ap.add_argument("--gif", action="store_true", help="assemble rendered/turntable.gif from the frames")
    ap.add_argument("--gif-only", action="store_true", help="skip rendering, just (re)assemble the GIF")
    args = ap.parse_args()
    W, H = args.res.lower().split("x")

    os.makedirs(OUTDIR, exist_ok=True)

    if not args.gif_only:
        if not os.path.exists(RISE):
            sys.exit("error: %s not built" % RISE)
        body = strip_cameras(open(SCENE).read())
        env = dict(os.environ, RISE_MEDIA_PATH=ROOT + "/")
        for f in range(args.frames):
            theta = math.radians(args.start_deg + 360.0 * f / args.frames)
            x = args.radius * math.cos(theta)
            y = args.radius * math.sin(theta)
            loc = "%.4f %.4f %.4f" % (x, y, args.height)
            scene = body
            outname = "turntable/frame_%03d" % f
            scene = re.sub(r"(pattern\s+)\S+", r"\1rendered/%s" % outname, scene, count=1)
            scene = re.sub(r"(?m)^(\s*samples\s+)\d+", r"\g<1>%d" % args.samples, scene, count=1)
            scene = re.sub(r"(?m)^(\s*width\s+)\d+", r"\g<1>%s" % W, scene, count=1)
            scene = re.sub(r"(?m)^(\s*height\s+)\d+", r"\g<1>%s" % H, scene, count=1)
            # +Z-axis orbit camera, looking at the dial centre, up = orbit axis.
            scene = scene.rstrip() + "\n\n" + cam_chunk(loc, "0 0 -1", "0 0 1", "%.2f" % args.fov) + "\n"
            tmp = "/tmp/watch_turntable_%03d.RISEscene" % f
            open(tmp, "w").write(scene)
            print("frame %d/%d  az=%.0fdeg  cam=(%.1f %.1f %.1f)" % (
                f + 1, args.frames, math.degrees(theta), x, y, args.height), flush=True)
            r = subprocess.run([RISE, tmp], input="render\nquit\n", text=True,
                               env=env, capture_output=True, cwd=ROOT)
            if "Rasterization Time" not in r.stdout:
                print("  !! render may have failed: %s" % r.stdout[-200:], flush=True)

    if args.gif or args.gif_only:
        try:
            from PIL import Image
        except ImportError:
            sys.exit("Pillow required for --gif")
        # Prefer the OIDN-denoised frames if present.
        frames = []
        for f in range(args.frames):
            d = os.path.join(OUTDIR, "frame_%03d_denoised.png" % f)
            p = os.path.join(OUTDIR, "frame_%03d.png" % f)
            use = d if os.path.exists(d) else p
            if os.path.exists(use):
                frames.append(Image.open(use).convert("RGB"))
        if not frames:
            sys.exit("no frames found in %s" % OUTDIR)
        gif = os.path.join(ROOT, "rendered", "turntable.gif")
        frames[0].save(gif, save_all=True, append_images=frames[1:], duration=80, loop=0, optimize=True)
        print("wrote %s (%d frames)" % (gif, len(frames)))


if __name__ == "__main__":
    main()
