#!/usr/bin/env python3
"""Render the two temper-comparison contact sheets and assemble them.

Set A (controlled): all four metals under the SAME 200->1000 C torch ramp
                    -- which metal showcases the most colour.
Set B (optimal):    each metal clamped to its own beautiful window
                    -- what each metal looks like at its best.

Renders each dial sequentially (RISE pegs every core -- never two at once),
then montages each set into a labelled 2x2 contact sheet.  Outputs are NOT
committed (big renders); they land in rendered/.

Run from the repo root:
    export RISE_MEDIA_PATH="$(pwd)/"
    python3 scenes/FeatureBased/GuillocheWatch/render_temper_comparison.py
Options: --res 1600 --spp 128 (defaults), --quick for a fast 800/40 proof.
"""
import argparse, os, subprocess, sys, tempfile

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.abspath(os.path.join(HERE, "..", "..", ".."))
GEN = os.path.join(HERE, "temper_comparison_gen.py")
RISE = os.path.join(ROOT, "bin", "rise")

# (key, label, controlled-range-note, optimal-range from the table)
METALS = [
    ("titanium",  "Titanium",  "300-580 C"),
    ("niobium",   "Niobium",   "250-520 C"),
    ("tantalum",  "Tantalum",  "300-560 C"),
    ("stainless", "Stainless", "230-350 C"),
]


def run(cmd, **kw):
    r = subprocess.run(cmd, **kw)
    if r.returncode != 0:
        sys.exit("command failed: %s" % " ".join(cmd))


def render_one(metal, mode, res, spp, tmpdir):
    pat = "rendered/temper_%s_%s" % (metal, mode)
    scene = os.path.join(tmpdir, "temper_%s_%s.RISEscene" % (metal, mode))
    run([sys.executable, GEN, "--metal", metal, "--mode", mode,
         "--scene-out", scene, "--out-pattern", pat,
         "--width", str(res), "--height", str(res), "--samples", str(spp)],
        cwd=ROOT, stdout=subprocess.DEVNULL)
    # RISE renders sequentially; feed render+quit on stdin.  The CLI console
    # exits non-zero on `quit` even after a clean render, so verify by the
    # output file rather than the return code.
    print("  rendering %-10s %s  (%dx%d, %d spp) ..." % (metal, mode, res, res, spp), flush=True)
    env = dict(os.environ, RISE_MEDIA_PATH=ROOT + "/")
    out_d = os.path.join(ROOT, pat + "_denoised0000.png")
    out_r = os.path.join(ROOT, pat + "0000.png")
    for f in (out_d, out_r):
        if os.path.exists(f):
            os.remove(f)
    subprocess.run([RISE, scene], cwd=ROOT, input="render\nquit\n", text=True,
                   stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL, env=env)
    out = out_d if os.path.exists(out_d) else out_r
    if not os.path.exists(out):
        sys.exit("render produced no output for %s %s" % (metal, mode))
    return out


def montage(images, labels, sublabels, title, out_path):
    from PIL import Image, ImageDraw
    cell = Image.open(images[0]).size[0]
    pad, top, capt = 18, 56, 52
    cols, rows = 2, 2
    W = cols * cell + (cols + 1) * pad
    H = top + rows * (cell + capt) + (rows + 1) * pad
    canvas = Image.new("RGB", (W, H), (250, 250, 250))
    dr = ImageDraw.Draw(canvas)
    dr.text((pad, 18), title, fill=(20, 20, 20))
    for i, (img, lab, sub) in enumerate(zip(images, labels, sublabels)):
        r, c = divmod(i, cols)
        x = pad + c * (cell + pad)
        y = top + pad + r * (cell + capt + pad)
        canvas.paste(Image.open(img).convert("RGB"), (x, y))
        dr.text((x + 6, y + cell + 8),  lab, fill=(20, 20, 20))
        dr.text((x + 6, y + cell + 28), sub, fill=(90, 90, 90))
    canvas.save(out_path)
    print("  -> %s" % out_path)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--res", type=int, default=1600)
    ap.add_argument("--spp", type=int, default=128)
    ap.add_argument("--quick", action="store_true", help="fast proof: 800 res / 40 spp")
    args = ap.parse_args()
    res, spp = (800, 40) if args.quick else (args.res, args.spp)

    os.makedirs(os.path.join(ROOT, "rendered"), exist_ok=True)
    tmpdir = tempfile.mkdtemp(prefix="temper_")

    sheets = {
        "controlled": ("CONTROLLED COMPARISON -- identical 200-1000 C torch on every metal",
                       lambda key, note: "200-1000 C (same torch)"),
        "optimal":    ("OPTIMAL WINDOWS -- each metal clamped to its beautiful range",
                       lambda key, note: note),
    }
    for mode, (title, subf) in sheets.items():
        print("[%s]" % mode, flush=True)
        imgs, labs, subs = [], [], []
        for key, label, note in METALS:
            imgs.append(render_one(key, mode, res, spp, tmpdir))
            labs.append(label)
            subs.append(subf(key, note))
        out = os.path.join(ROOT, "rendered", "temper_sheet_%s.png" % mode)
        montage(imgs, labs, subs, title, out)
    print("done.")


if __name__ == "__main__":
    main()
