#!/usr/bin/env python3
"""Pixel gate for the v6->v7 `global_medium` migration.

RISE rendering is NON-DETERMINISTIC (Monte-Carlo: two renders of the same scene
differ in ~90% of bytes), so a byte-for-byte image compare is invalid.  But the
MEAN luminance of an image is LLN-stable across renders, so it is the robust
signal.  For each scene that uses `> set global_medium`, this renders the
ORIGINAL (the v6 command) and the MIGRATED (the v7 `global_medium{}` chunk) and
compares per-channel mean luminance.  A faithful migration leaves the mean within
Monte-Carlo noise (TOL); a dropped or wrong medium moves the mean far more (the
fog/volume changes the whole image).

Both versions render through the normal CLI (the legacy parser handles `> run`
includes AND the registered `global_medium{}` chunk), from the repo root with
RISE_MEDIA_PATH set, so relative includes/media resolve.  Renders are sequential
(each uses all cores).  Reduced samples keep it quick; the mean converges fast.

Usage:  python3 tools/pixel_gate_global_medium.py <scene.RISEscene> [more ...]
Exit 0 if every scene passes (or is skipped with a reason), 1 if any FAILS.
"""
import sys, os, re, subprocess
from PIL import Image

REPO    = os.getcwd()
SAMPLES = 4          # mean converges fast; the signal (noise ~0.1 vs medium ~50) is huge
TOL     = 2.0        # per-channel mean |diff| (0-255): MC noise << this, a wrong medium >> this

def reduce_and_retarget(text, outbase):
    text = re.sub(r'(samples\s+)\d+', r'\g<1>' + str(SAMPLES), text, count=1)
    text = re.sub(r'(pattern\s+)\S+', r'\g<1>' + outbase, text)   # all outputs -> outbase
    return text

def migrate(text):
    # NOTE: looser standalone regex (matches `>set`, not comment-aware) -- fine for this corpus;
    # the SHIPPING migrator is the comment-aware token transform in the C++ gate (ProcessLines).
    return re.sub(r'>\s*set\s+global_medium\s+(\S+)', r'global_medium\n{\nmedium \1\n}', text)

def render(scene_text, name):
    path = f"/tmp/{name}.RISEscene"
    open(path, "w").write(scene_text)
    png = os.path.join(REPO, name + ".png")          # pattern=outbase resolves CWD-relative to REPO (RISE_MEDIA_PATH is NOT prepended unless the scene opts in), type PNG
    try: os.remove(png)                              # drop any stale output so a failed render cannot pass on a previous PNG
    except OSError: pass
    env = dict(os.environ); env["RISE_MEDIA_PATH"] = REPO + "/"
    # NOTE: bin/rise exits NON-ZERO on the `render\nquit` path even on a SUCCESSFUL render, so its
    # return code is not a usable success signal -- the produced-PNG check below is.  The stale-remove
    # above is what stops a failed render from passing on a previous run's PNG.
    subprocess.run(["./bin/rise", path], input="render\nquit\n", text=True,
                   stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL, env=env, timeout=900)
    return png if os.path.exists(png) else None

def chan_mean(png):
    im = Image.open(png).convert("RGB")
    d  = list(im.getdata()); n = len(d)
    return [sum(p[c] for p in d) / n for c in range(3)]

def cleanup():
    for f in os.listdir(REPO):
        if f.startswith(("pg_orig", "pg_mig")) or f.startswith("fro_temp_"):
            try: os.remove(os.path.join(REPO, f))
            except OSError: pass
    for tmp in ("/tmp/pg_orig.RISEscene", "/tmp/pg_mig.RISEscene"):
        try: os.remove(tmp)
        except OSError: pass

def gate(scene):
    text = open(scene).read()
    if not re.search(r'>\s*set\s+global_medium', text):
        return ("SKIP", scene, "no `> set global_medium`")
    op = render(reduce_and_retarget(text,          "pg_orig"), "pg_orig")
    mp = render(reduce_and_retarget(migrate(text), "pg_mig"),  "pg_mig")
    if op is None or mp is None:
        return ("SKIP", scene, f"no PNG output (orig={op}, mig={mp})")
    mo, mm = chan_mean(op), chan_mean(mp)
    diff = sum(abs(a - b) for a, b in zip(mo, mm)) / 3.0
    verdict = "PASS" if diff < TOL else "FAIL"
    return (verdict, scene, f"mean|diff|={diff:.3f} (tol {TOL})  orig={[round(x,1) for x in mo]} mig={[round(x,1) for x in mm]}")

def main(argv):
    if not argv:
        print(__doc__); return 2
    fails = 0
    for scene in argv:
        v, s, msg = gate(scene); cleanup()
        print(f"[{v}] {s}  --  {msg}"); sys.stdout.flush()
        if v == "FAIL": fails += 1
    print(f"\n=== {len(argv)} scenes: {fails} FAILED ===")
    return 1 if fails else 0

if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
