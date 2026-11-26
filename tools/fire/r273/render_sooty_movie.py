#!/usr/bin/env python3
"""Render a sooty showcase movie (r273_sooty_showcase.RISEscene.in: r272 sooty room, key light moved to
(1.6,-3.6,2.0) so the plume's shadow falls behind it and the shadow of the domain top is out of frame).
usage: render_sooty_movie.py <seq dir with sequence_manifest> <out dir> <movie name> [width height]"""
import hashlib, json, pathlib, subprocess, os, sys, datetime
ROOT=pathlib.Path("/Volumes/SSD/Working/RISE")
SEQ=pathlib.Path(sys.argv[1]);OUT=pathlib.Path(sys.argv[2]);NAME=sys.argv[3]
W,H=(sys.argv[4],sys.argv[5]) if len(sys.argv)>5 else ("256","384")
BINARY=pathlib.Path(os.environ.get("RISE_RENDER_BINARY",ROOT/"bin/rise"))  # override keeps a long render on one build
def sha(p):
    h=hashlib.sha256()
    with open(p,"rb") as f:
        for b in iter(lambda:f.read(1048576),b""): h.update(b)
    return h.hexdigest()
def record(name,value): (OUT/name).write_text(json.dumps(value,indent=2)+"\n")
OUT.mkdir(parents=True,exist_ok=True)
SCENE=OUT/"scene.RISEscene"
SCENE.write_text((ROOT/"tools/fire/r273/r273_sooty_showcase.RISEscene.in").read_text().replace("@OUT@",str(OUT)).replace("@SEQ@",str(SEQ)))
record("launch.json",dict(started_utc=datetime.datetime.now(datetime.timezone.utc).isoformat(),binary_sha256=sha(BINARY),
    scene_sha256=sha(SCENE),width=int(W),height=int(H),frames=235,fps=16,
    source_manifest_sha256=sha(SEQ/"sequence_manifest.rise-fire.cbor")))
try:
    with open(OUT/"render.log","w") as log:
        result=subprocess.run([str(BINARY),"--width",W,"--height",H,str(SCENE)],input="renderanimation 0 14.625 235\nquit\n",text=True,cwd=ROOT,env=dict(os.environ,RISE_MEDIA_PATH="/"),stdout=log,stderr=subprocess.STDOUT)
    if result.returncode not in (0,1): raise RuntimeError("render failed: inspect render.log")
    for i in range(235):
        p=OUT/f"fire_display_denoised{i:04d}.png"
        if not p.is_file() or p.stat().st_size==0: raise RuntimeError(f"missing output {p}")
    movie=OUT/NAME
    for script,args,logname in [("r242_encode.swift",[str(OUT),str(movie),W,H],"encode.log"),("r242_verify.swift",[str(movie),W,H],"verify.log")]:
        with open(OUT/logname,"w") as log:
            subprocess.run(["swift",str(ROOT/"tools/fire"/script),*args],cwd=ROOT,stdout=log,stderr=subprocess.STDOUT,check=True)
    record("complete.json",dict(movie=str(movie),sha256=sha(movie),frames=235,width=int(W),height=int(H),fps=16))
except Exception as error:
    record("failure.json",dict(error=str(error)));raise
