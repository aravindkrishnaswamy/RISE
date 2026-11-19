#!/usr/bin/env python3
"""Read completed r264 ledgers/checkpoints; report the owner-requested observables."""
from pathlib import Path
import csv,json,subprocess,sys,hashlib
import numpy as np
from r250_acceptance import measure
import r252_centerline_report as centerline
ROOT=Path(__file__).resolve().parents[2];BASE=Path('/Volumes/SSD/RISE/fire_production_calibration');WORK=BASE/'r264_centered_production';RUN=WORK/'run';OUT=ROOT/'docs/data/fire_r264'
def native(args,log):
 with log.open('w') as f:r=subprocess.run([str(v) for v in args],cwd=ROOT,stdout=f,stderr=subprocess.STDOUT)
 if r.returncode:raise RuntimeError('read-only reduction failed: '+str(args[0]))
def avg(t,v,start,end):
 assert t[0]<=start and t[-1]>=end-1e-8
 tt=np.r_[start,t[(t>start)&(t<end)],end];return float(np.trapezoid(np.interp(tt,t,v),tt)/(end-start))
def main():
 OUT.mkdir(exist_ok=True);acceptance=measure(RUN)
 native([ROOT/'bin/tests/FireSequenceTest','--fire-production-checkpoint-physics-diagnostic',RUN/'production_final.checkpoint',OUT/'retained_statistics.txt'],WORK/'retained_statistics.log')
 centerline.OUT=OUT;mc=centerline.analyze('r264',OUT/'retained_statistics.txt')
 native([WORK/'flame_height',RUN/'checkpoints',OUT/'flame_height.csv'],WORK/'flame_height.log')
 h=np.genfromtxt(OUT/'flame_height.csv',delimiter=',',names=True,dtype=None,encoding='utf-8');t=h['time_s'];start=float(t[0]);end=float(t[-1]);assert len(t)==235
 height=avg(t,h['h99_realized_m'],start,end);acceptance.update(FDS_mean_instantaneous_h99_realized_m=height,FDS_flame_height_relative_error=height/acceptance['heskestad_expected_m']-1,FDS_height_definition='time mean of per-frame height containing99% realized instantaneous HRR; retained16Hzframes, trapezoidal time quadrature')
 (OUT/'acceptance.json').write_text(json.dumps(acceptance,indent=2)+'\n');(OUT/'mccaffrey.json').write_text(json.dumps(mc,indent=2)+'\n')
 paths=sorted((RUN/'checkpoint_history').glob('*.checkpoint'))+[RUN/'production_final.checkpoint'];listing=WORK/'checkpoint_inputs.txt';listing.write_text('\n'.join(str(p) for p in paths)+'\n')
 native([WORK/'mass_probe',listing,OUT/'mass_samples.csv'],WORK/'mass_probe.log')
 data=np.genfromtxt(OUT/'mass_samples.csv',names=True,delimiter=',');fds=json.loads((ROOT/'docs/data/fire_r263/results.json').read_text())['rows'];rows=[]
 for z in [32,65,98]:
  a=data[data['z']==z];order=np.argsort(a['time_s']);a=a[order];_,index=np.unique(a['time_s'],return_index=True);a=a[index];reference=next(r for r in fds if r['z']==z)
  row={'z':z,'height_m':float(a['height_m'][0]),'r264_net_r08_kg_s':avg(a['time_s'],a['net_r08_kg_s'],start,end),'r264_up_r08_kg_s':avg(a['time_s'],a['up_r08_kg_s'],start,end),'r264_up_plane_kg_s':avg(a['time_s'],a['up_plane_kg_s'],start,end),'FDS_no_slip_net_r08_kg_s':reference['no_slip_FDS_net_r08_kg_s'],'FDS_no_slip_up_r08_kg_s':reference['no_slip_FDS_up_r08_kg_s'],'FDS_no_slip_up_plane_kg_s':reference['no_slip_FDS_up_plane_kg_s']}
  row['net_relative_error']=row['r264_net_r08_kg_s']/row['FDS_no_slip_net_r08_kg_s']-1;rows.append(row)
 with (OUT/'entrainment.csv').open('w') as f:
  w=csv.DictWriter(f,fieldnames=list(rows[0]),lineterminator='\n');w.writeheader();w.writerows(rows)
 trajectory=list(csv.DictReader((RUN/'budgets/maximum_velocity_trajectory.csv').open()));peak=max(trajectory,key=lambda r:float(r['maximum_velocity_m_per_s']))
 keys=['accepted_step','time_s','dt_s','maximum_velocity_m_per_s','axis','face','x','y','z']
 with (OUT/'velocity_trajectory.csv').open('w') as f:
  w=csv.DictWriter(f,fieldnames=keys,extrasaction='ignore',lineterminator='\n');w.writeheader();w.writerows(trajectory)
 result={'acceptance':acceptance,'entrainment':rows,'McCaffrey':mc,'peak_velocity':{k:peak[k] for k in keys},'statistics_start_s':start,'statistics_end_s':end,'mass_method':'retained native checkpoint quadrature, centered MAC velocity and cellgasdensity; net and positive upward separately. No-slip FDS same full statistics interval.','decision':'full-window measurement complete; stop for owner review; no tier10 run'}
 (OUT/'results.json').write_text(json.dumps(result,indent=2)+'\n');print(json.dumps(result,indent=2))
if __name__=='__main__':main()
