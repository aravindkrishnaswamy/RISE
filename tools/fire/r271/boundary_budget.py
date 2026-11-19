# r271 (c): RISE boundary-face (MAC) mass in/out per open side and top from budgets/step_*.csv (field 9 total species mass), window 10.466-25.091 s.
import csv,glob,os,numpy as np
RUNS={'r270':'/Volumes/SSD/RISE/fire_production_calibration/r270_floor_radiation/tier8_eps1',
      'r264':'/Volumes/SSD/RISE/fire_production_calibration/r264_centered_production/run'}
COLS=['xmin_in','xmin_out','xmax_in','xmax_out','ymin_in','ymin_out','ymax_in','ymax_out','top_in','top_out','bottom_burner_in','bottom_nonburner_in','bottom_nonburner_out']
START,END=10.466040127156969,25.091040127212182
def read(path):
 meta={};rows={}
 with open(path) as f:
  lines=f.read().splitlines()
 for l in lines:
  if l.startswith('# '):
   p=l[2:].split(' ',1)
   if len(p)==2:meta[p[0]]=p[1]
 body=[l for l in lines if not l.startswith('#')]
 for r in csv.DictReader(body):rows[(r['scope'],r['field'])]=r
 return meta,rows
for name,run in RUNS.items():
 files=sorted(glob.glob(run+'/budgets/step_*.csv'))
 meta,rows=read(files[-1]);t1=float(meta['beginning_time_s'])+float(meta['dt_s'])
 st=rows[('statistics','9')];span=t1-float(meta['statistics_start_s'])
 acc=np.zeros(len(COLS));steps=0;series=[]
 for p in files:
  m,r=read(p);t0=float(m['beginning_time_s']);dt=float(m['dt_s'])
  if t0+dt<=START:continue
  frac=1.0 if t0>=START else (t0+dt-START)/dt
  v=np.array([float(r[('step','9')][c]) for c in COLS]);acc+=frac*v;steps+=1
  series.append((t0+.5*dt,dt,v))
 print(f'== {name}: last step {os.path.basename(files[-1])} window {meta["statistics_start_s"][:8]}..{t1:.4f} s ({span:.4f} s), {steps} steps')
 print(' face set        stats_row_kg  step_sum_kg   mean_kg/s')
 for i,c in enumerate(COLS):
  s=float(st[c]);print(f' {c:20s} {s:10.4f}  {acc[i]:10.4f}  {s/span:9.4f}')
 g=lambda k:float(st[k])/span
 sin=sum(g(f'{s}_in') for s in ['xmin','xmax','ymin','ymax']);sout=sum(g(f'{s}_out') for s in ['xmin','xmax','ymin','ymax'])
 print(f' sides: in {sin:.4f}  out {sout:.4f}  net in {sin-sout:.4f} kg/s ; top: in {g("top_in"):.4f} out {g("top_out"):.4f} net out {g("top_out")-g("top_in"):.4f} kg/s ; burner {g("bottom_burner_in"):.5f} kg/s')
 dts=np.array([d for _,d,_ in series]);V=np.array([v for _,_,v in series])/dts[:,None]
 si=V[:,[0,2,4,6]].sum(1);so=V[:,[1,3,5,7]].sum(1)
 print(f' per-step side inflow rate: mean(dt-weighted) {(si*dts).sum()/dts.sum():.4f}  std {si.std():.4f}  min {si.min():.4f} max {si.max():.4f} kg/s; side outflow rate std {so.std():.4f}')
