#!/usr/bin/env python3
"""Render the r272 soot look pass (derived render_seq frames, room scene) at 256x384, encode, decode-check."""
import hashlib, json, pathlib, subprocess, os, datetime
ROOT=pathlib.Path("/Volumes/SSD/Working/RISE")
OUT=pathlib.Path("/Volumes/SSD/RISE/fire_production_calibration/r272_soot/movie_look")
SCENE=ROOT/"tools/fire/r272/r272_soot_look.RISEscene"
BINARY=ROOT/"bin/rise"
def sha(p):
    h=hashlib.sha256()
    with open(p,"rb") as f:
        for b in iter(lambda:f.read(1048576),b""): h.update(b)
    return h.hexdigest()
def record(name,value):
    (OUT/name).write_text(json.dumps(value,indent=2)+"\n")
OUT.mkdir(parents=True,exist_ok=True)  # Reuse this campaign directory for retries.
record("launch.json",dict(started_utc=datetime.datetime.now(datetime.timezone.utc).isoformat(),
    binary_sha256=sha(BINARY),scene_sha256=sha(SCENE),width=256,height=384,frames=235,fps=16,
    source_manifest_sha256=sha(pathlib.Path("/Volumes/SSD/RISE/fire_production_calibration/r272_soot/render_seq/sequence_manifest.rise-fire.cbor")),
    label="r272 tier-8 soot (yield 0.03) preview; r242 spectral scene look pass at 256x384",host_rule="one render; no concurrent builds or solver"))
try:
    with open(OUT/"render.log","w") as log:
        result=subprocess.run([str(BINARY),"--width","256","--height","384",str(SCENE)],input="renderanimation 0 14.625 235\nquit\n",text=True,cwd=ROOT,env=dict(os.environ,RISE_MEDIA_PATH="/"),stdout=log,stderr=subprocess.STDOUT)
    text=(OUT/"render.log").read_text(errors="replace")
    if result.returncode not in (0,1) or "Total Rasterization Time:" not in text:
        raise RuntimeError("render failed: inspect render.log")
    products=[]
    for i in range(235):
        for stem,ext in [("fire_primary","exr"),("fire_primary_denoised","exr"),("fire_display","png"),("fire_display_denoised","png")]:
            p=OUT/f"{stem}{i:04d}.{ext}"
            for q in [p,pathlib.Path(str(p)+".provenance.cbor")]:
                if not q.is_file() or q.stat().st_size==0: raise RuntimeError(f"missing output {q}")
                products.append(dict(file=q.name,sha256=sha(q),bytes=q.stat().st_size))
    record("frames.sha256.json",products)
    movie=OUT/"r272_soot_look_256x384.mov"
    for script,args,logname in [
        ("r242_encode.swift",[str(OUT),str(movie),"256","384"],"encode.log"),
        ("r242_verify.swift",[str(movie),"256","384"],"verify.log")]:
        with open(OUT/logname,"w") as log:
            subprocess.run(["swift",str(ROOT/"tools/fire"/script),*args],cwd=ROOT,stdout=log,stderr=subprocess.STDOUT,check=True)
    record("complete.json",dict(movie=str(movie),sha256=sha(movie),frames=235,width=256,height=384,fps=16,duration_s=14.6875))
except Exception as error:
    record("failure.json",dict(error=str(error)))
    raise
