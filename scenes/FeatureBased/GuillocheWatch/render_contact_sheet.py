#!/usr/bin/env python3
"""Render a metal × palette CONTACT SHEET for the guilloché watch and assemble it
into one ~4K PNG.  Columns = base metal (Ti/Nb/Ta/steel), rows = colour palette
(temper window).  Each cell is a separate full-resolution render; the sheet is
the PIL montage with axis labels.

Switching per cell = set the dial object's `material` to tf_dial_<metal> and that
material's `film_thickness` painter to the metal's palette:
    Ti:        oxide_thk[_warm|_cool|_wide]            (vivid = oxide_thk)
    <metal>:   oxide_thk_<metal>[_warm|_cool|_wide]    (vivid = oxide_thk_<metal>)

At 960 px/cell, 4 columns = 3840 px = 4K width, each render native (no upscaling).
The big PNGs are NOT committed (gitignored rendered/); this script is the memory.

Usage:
  python3 scenes/FeatureBased/GuillocheWatch/render_contact_sheet.py            # 4x3, 960px, 96spp
  python3 ... --ranges warm vivid cool wide        # 4x4
  python3 ... --res 720 --samples 48 --quick       # faster preview
Output: rendered/metal_palette_contactsheet.png  (+ rendered/grid_<metal>_<range>.png cells)
"""
import argparse
import math
import os
import re
import subprocess
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.abspath(os.path.join(HERE, "..", "..", ".."))
SCENE = os.path.join(HERE, "watch_dial.RISEscene")
RISE = os.path.join(ROOT, "bin", "rise")

METAL_MAT = {"ti": "tf_dial", "nb": "tf_dial_nb", "ta": "tf_dial_ta", "steel": "tf_dial_steel"}
METAL_LABEL = {"ti": "Titanium", "nb": "Niobium", "ta": "Tantalum", "steel": "Stainless"}


def painter(metal, rng):
    """film_thickness painter name for (metal, range)."""
    base = "oxide_thk" if metal == "ti" else "oxide_thk_" + metal
    return base if rng == "vivid" else base + "_" + rng


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--metals", nargs="+", default=["ti", "nb", "ta", "steel"])
    ap.add_argument("--ranges", nargs="+", default=["warm", "vivid", "cool"])
    ap.add_argument("--res", type=int, default=960, help="pixels per cell (4 cols x 960 = 3840 = 4K width)")
    ap.add_argument("--samples", type=int, default=96)
    ap.add_argument("--cam", default="cam_high34", help="camera to render (timelines are stripped)")
    ap.add_argument("--out", default=os.path.join(ROOT, "rendered", "metal_palette_contactsheet.png"))
    ap.add_argument("--gif-only", "--assemble-only", dest="assemble_only", action="store_true",
                    help="skip rendering; just (re)assemble from existing rendered/grid_*.png")
    args = ap.parse_args(argv)

    if not args.assemble_only:
        if not os.path.exists(RISE):
            sys.exit("error: %s not built" % RISE)
        base = open(SCENE).read()
        env = dict(os.environ, RISE_MEDIA_PATH=ROOT + "/")
        for mk in args.metals:
            mat = METAL_MAT[mk]
            for rng in args.ranges:
                thk = painter(mk, rng)
                s = base
                s = re.sub(r"(geometry\s+)dialmesh\b", r"\g<1>dialmesh", s, count=1)  # (keep stock geom)
                s = re.sub(r"(material\s+)tf_dial\b", r"\g<1>%s" % mat, s, count=1)
                s = re.sub(r"(name\s+" + mat + r"\b.*?film_thickness\s+)\S+", r"\g<1>%s" % thk, s, count=1, flags=re.S)
                s = re.sub(r"(?m)^(\s*samples\s+)\d+", r"\g<1>%d" % args.samples, s, count=1)
                s = re.sub(r"(?m)^(\s*width\s+)\d+", r"\g<1>%d" % args.res, s, count=1)
                s = re.sub(r"(?m)^(\s*height\s+)\d+", r"\g<1>%d" % args.res, s, count=1)
                s = re.sub(r"(pattern\s+)\S+", r"\1rendered/grid_%s_%s" % (mk, rng), s, count=1)
                s = re.sub(r"(?m)^(\s*multiple\s+)\w+", r"\g<1>FALSE", s)
                s = re.sub(r"\n(timeline|animation_options)\s*\{[^{}]*\}", "", s)  # static cam
                # make the chosen camera active (move it last) is unnecessary: cam_high34 is already
                # active; for another --cam, append it last so it wins.
                if args.cam != "cam_high34":
                    m = re.search(r"(pinhole_camera|thinlens_camera)\s*\{[^{}]*?name\s+" + args.cam + r"\b[^{}]*\}", s, re.S)
                    if m:
                        s = s + "\n" + m.group(0) + "\n"
                tmp = "/tmp/contact_%s_%s.RISEscene" % (mk, rng)
                open(tmp, "w").write(s)
                r = subprocess.run([RISE, tmp], input="render\nquit\n", text=True, env=env,
                                   capture_output=True, cwd=ROOT)
                ok = "Rasterization Time" in r.stdout
                print("  %s/%s -> rendered/grid_%s_%s.png  %s" % (mk, rng, mk, rng, "OK" if ok else "FAILED"), flush=True)

    # assemble
    try:
        from PIL import Image, ImageDraw, ImageFont
    except ImportError:
        sys.exit("Pillow required to assemble the contact sheet")
    CELL = args.res; GAP = max(8, CELL // 96); LM = max(150, CELL // 6); TM = max(70, CELL // 10); PAD = GAP

    def font(sz):
        for p in ["/System/Library/Fonts/Helvetica.ttc", "/System/Library/Fonts/Supplemental/Arial.ttf",
                  "/Library/Fonts/Arial.ttf", "/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf"]:
            try:
                return ImageFont.truetype(p, sz)
            except Exception:
                pass
        return ImageFont.load_default()
    fm = font(max(28, CELL * 62 // 960)); fr = font(max(24, CELL * 54 // 960))

    nC, nR = len(args.metals), len(args.ranges)
    W = LM + nC * CELL + (nC - 1) * GAP + PAD
    H = TM + nR * CELL + (nR - 1) * GAP + PAD
    sheet = Image.new("RGB", (W, H), (22, 22, 24))
    d = ImageDraw.Draw(sheet)

    def ctext(cx, cy, txt, fnt):
        b = d.textbbox((0, 0), txt, font=fnt); d.text((cx - (b[2] - b[0]) / 2, cy - (b[3] - b[1]) / 2 - b[1]), txt, font=fnt, fill=(238, 238, 240))
    for ci, mk in enumerate(args.metals):
        ctext(LM + ci * (CELL + GAP) + CELL // 2, TM // 2, METAL_LABEL.get(mk, mk), fm)
    for ri, rng in enumerate(args.ranges):
        ctext(LM // 2, TM + ri * (CELL + GAP) + CELL // 2, rng, fr)
    for ri, rng in enumerate(args.ranges):
        for ci, mk in enumerate(args.metals):
            f = os.path.join(ROOT, "rendered", "grid_%s_%s_denoised.png" % (mk, rng))
            if not os.path.exists(f):
                f = os.path.join(ROOT, "rendered", "grid_%s_%s.png" % (mk, rng))
            if not os.path.exists(f):
                continue
            im = Image.open(f).convert("RGB")
            if im.size != (CELL, CELL):
                im = im.resize((CELL, CELL))
            sheet.paste(im, (LM + ci * (CELL + GAP), TM + ri * (CELL + GAP)))
    sheet.save(args.out)
    print("contact sheet -> %s  (%d x %d)" % (args.out, W, H))
    return 0


if __name__ == "__main__":
    sys.exit(main())
