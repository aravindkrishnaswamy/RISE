#!/usr/bin/env python3
"""One FDS run after the existing bounded RISE queue; no recurring automation."""
from pathlib import Path
import subprocess,os,time,json,datetime,sys,struct
import numpy as np
from r255_reduce_fds import record
ROOT=Path(__file__).resolve().parents[2];BASE=Path('/Volumes/SSD/RISE/fire_production_calibration');WORK=BASE/'r263_fds_no_slip';RUN=WORK/'fds_run';OUT=ROOT/'docs/data/fire_r263'
def status(**values):
 values['updated_utc']=datetime.datetime.now(datetime.timezone.utc).isoformat();p=WORK/'run_status.json';tmp=p.with_suffix('.tmp');tmp.write_text(json.dumps(values,indent=2)+'\n');tmp.replace(p)
def active():
 commands=subprocess.check_output(['ps','-axo','command='],text=True).splitlines()
 return [v for v in commands if any(v.startswith(str(BASE/p)) for p in ['r262_combined/combined_driver','r262_combined/combined_onset','r262_combined/centered_onset','r255_comparison/fds_source/Build/ompi_gnu_osx/fds_ompi_gnu_osx'])]
def early(path,count=2,include_initial=False):
 # Read only complete native records. Ignore the initial state. Stop after two retained times.
 fields=[]
 with path.open('rb') as f:
  for i in range(4):record(f)
  while len(fields)<count:
   t=record(f)
   if t is None:break
   a=record(f)
   if a is None:break
   t=struct.unpack('<f',t)[0]
   if include_initial or t>1e-8:fields.append((t,np.frombuffer(a,dtype='<f4').copy()))
 return fields
p=None
try:
 status(state='waiting_for_r262_queue');deadline=time.monotonic()+36*3600
 while True:
  stage=json.loads((BASE/'r262_combined/campaign_status.json').read_text())['stage']
  if stage.startswith('stopped_') and not active():break
  if time.monotonic()>deadline:raise RuntimeError('bounded queue wait expired; no FDS run launched')
  time.sleep(20)
 env=os.environ.copy();env['OMP_NUM_THREADS']='8';env['OMP_STACKSIZE']='64M'
 binary=BASE/'r255_comparison/fds_source/Build/ompi_gnu_osx/fds_ompi_gnu_osx'
 with (WORK/'run.log').open('w') as f:
  p=subprocess.Popen(['/usr/bin/caffeinate','-i',str(binary),'r263_fds.fds'],cwd=RUN,env=env,stdin=subprocess.DEVNULL,stdout=f,stderr=subprocess.STDOUT)
  status(state='running',pid=p.pid,early_check='first two retained noninitial states; default-wall first-two-step fields were not retained')
  checked=False;deadline=time.monotonic()+4*3600
  while p.poll() is None:
   if not checked:
    try:
     rows=[]
     # Bottom-layer temperature and vertical velocity reveal actual computation changes.
     for n in [108,111]:
      aa=early(BASE/f'r261_fds_full/fds_run/r261_fds_1_{n}.sf',count=6,include_initial=True);bb=early(RUN/f'r263_fds_1_{n}.sf')
      if len(aa)<3 or len(bb)<2:raise EOFError()
      at=np.array([v[0] for v in aa])
      for tb,b in bb:
       if tb<at[0] or tb>at[-1]:raise RuntimeError('early B timestamp lacks retained A bracket')
       hi=min(max(1,int(np.searchsorted(at,tb))),len(at)-1);lo=hi-1
       fraction=(tb-at[lo])/(at[hi]-at[lo]);a=(1.-fraction)*aa[lo][1].astype(np.float64)+fraction*aa[hi][1]
       assert a.shape==b.shape and np.isfinite(a).all() and np.isfinite(b).all()
       rows.append({'slice':n,'time_s':tb,'baseline_lower_time_s':float(at[lo]),'baseline_upper_time_s':float(at[hi]),'baseline_interpolation_fraction':float(fraction),'different_values':int(np.count_nonzero(a!=b)),'max_abs_difference':float(np.max(np.abs(a-b)))})
     (OUT/'early_fields.json').write_text(json.dumps({'scope':'first two retained B output states against temporally interpolated retained A; not first two internal timesteps or a bitwise matched-time reference','rows':rows},indent=2)+'\n')
     if not any(v['different_values'] for v in rows):raise RuntimeError('no computation change at first two retained states; full run refused')
     checked=True;status(state='running',pid=p.pid,early_retained_fields_differ=True)
    except (FileNotFoundError,EOFError,ValueError,struct.error):pass
   if time.monotonic()>deadline:raise RuntimeError('FDS wall-clock bound exceeded')
   time.sleep(1)
  code=p.returncode
 if code:raise RuntimeError(f'FDS exited {code}; raw outputs retained')
 if not checked:raise RuntimeError('FDS completed without early field evidence; inspect outputs before reporting')
 status(state='reducing',returncode=code)
 with (WORK/'reduction.log').open('w') as f:
  r=subprocess.run([sys.executable,str(ROOT/'tools/fire/r263_reduce_no_slip.py')],cwd=ROOT,stdout=f,stderr=subprocess.STDOUT)
 if r.returncode:raise RuntimeError('FDS finished but reduction failed; no further physical run')
 status(state='complete_stopped_for_review',returncode=0)
except Exception as e:
 if p is not None and p.poll() is None:
  # caffeinate execs the solver and keeps an assertion helper; terminate the native solver.
  p.terminate()
  try:p.wait(timeout=30)
  except subprocess.TimeoutExpired:p.kill();p.wait()
 status(state='stopped_error',error=str(e));raise
