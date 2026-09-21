#!/usr/bin/env python3
"""Render the full immutable r241 sequence, then encode and decode-check."""
import hashlib, json, pathlib, subprocess, os, sys, datetime
ROOT=pathlib.Path("/Users/aravind/Working/RISE/worktrees/r220-host-memory")
OUT=pathlib.Path("/Volumes/SSD/RISE/fire_production_calibration/r242_tier8_hq.v1")
SCENE=ROOT/"tools/fire/r242_high_quality.RISEscene"
BINARY=ROOT/"bin/rise"
def sha(p):
    h=hashlib.sha256()
    with open(p,"rb") as f:
        for b in iter(lambda:f.read(1048576),b""): h.update(b)
    return h.hexdigest()
def record(name,value):
    (OUT/name).write_text(json.dumps(value,indent=2)+"\n")
OUT.mkdir(parents=True,exist_ok=False)  # Atomic claim: even an empty existing run refuses.
record("launch.json",dict(started_utc=datetime.datetime.now(datetime.timezone.utc).isoformat(),
    binary_sha256=sha(BINARY),scene_sha256=sha(SCENE),width=512,height=768,frames=235,fps=16,
    source_manifest_sha256=sha(pathlib.Path("/Volumes/SSD/RISE/fire_production_calibration/r241_tier8_full_window.v1/sequence_manifest.rise-fire.cbor")),
    label="tier-8 preview; high-quality spectral render",host_rule="one render; no concurrent builds or solver"))
try:
    with open(OUT/"render.log","w") as log:
        result=subprocess.run([str(BINARY),str(SCENE)],input="renderanimation 0 14.625 235\nquit\n",text=True,cwd=ROOT,env=dict(os.environ,RISE_MEDIA_PATH="/"),stdout=log,stderr=subprocess.STDOUT)
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
    movie=OUT/"tier8_high_quality_temporal_16fps.mov"
    for script,args,logname in [
        ("r242_encode.swift",[str(OUT),str(movie)],"encode.log"),
        ("r242_verify.swift",[str(movie)],"verify.log")]:
        with open(OUT/logname,"w") as log:
            subprocess.run(["swift",str(ROOT/"tools/fire"/script),*args],cwd=ROOT,stdout=log,stderr=subprocess.STDOUT,check=True)
    record("complete.json",dict(movie=str(movie),sha256=sha(movie),frames=235,width=512,height=768,fps=16,duration_s=14.6875))
except Exception as error:
    record("failure.json",dict(error=str(error)))
    raise
