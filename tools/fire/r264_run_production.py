#!/usr/bin/env python3
"""One owner-authorized from-zero tier-8 window, followed by read-only reductions."""
from pathlib import Path
import subprocess,os,datetime,json,sys
ROOT=Path(__file__).resolve().parents[2];WORK=Path('/Volumes/SSD/RISE/fire_production_calibration/r264_centered_production');RUN=WORK/'run'
def status(**v):
 v['updated_utc']=datetime.datetime.now(datetime.timezone.utc).isoformat();p=WORK/'run_status.json';q=p.with_suffix('.tmp');q.write_text(json.dumps(v,indent=2)+'\n');q.replace(p)
try:
 # The native capstone creates its output directory and refuses an existing one.
 args=['/usr/bin/caffeinate','-i',str(ROOT/'bin/tests/FireSequenceTest'),'--fire-production-temporal-capstone','8','0.0625',str(RUN)]
 status(state='running',command=args,from_zero=True)
 with (WORK/'run.log').open('w') as f:r=subprocess.run(args,cwd=ROOT,stdin=subprocess.DEVNULL,stdout=f,stderr=subprocess.STDOUT)
 if r.returncode:raise RuntimeError('native tier8 stopped with exit '+str(r.returncode)+'; no resume or rerun attempted')
 status(state='reducing',native_returncode=0)
 env=os.environ.copy();env['PYTHONPATH']='/Volumes/SSD/RISE/fire_production_calibration/r257_readonly_comparison/python_packages'
 with (WORK/'reduction.log').open('w') as f:r=subprocess.run([sys.executable,str(ROOT/'tools/fire/r264_reduce_production.py')],cwd=ROOT,env=env,stdout=f,stderr=subprocess.STDOUT)
 if r.returncode:raise RuntimeError('physical run completed but read-only reduction failed; retained outputs preserved')
 status(state='complete_stopped_for_review',native_returncode=0)
except Exception as e:
 status(state='stopped_error',error=str(e));raise
