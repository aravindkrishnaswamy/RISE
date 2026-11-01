#!/usr/bin/env python3
"""r270 copy of tools/fire/r264_reduce_production.py for the tier-8 floor-radiation run (read-only).

Differences from r264: run/work paths; outputs go to docs/data/fire_r270/tier8_*; reuses r264's
flame_height and mass_probe binaries; r270 has no production_capstone_summary.txt (finalisation
bug), so accepted_steps/accepted_time come from the step budgets and frame_times.csv instead."""
from pathlib import Path
import csv,json,subprocess,sys
import numpy as np
sys.path.insert(0,str(Path(__file__).resolve().parents[1]))
import r250_acceptance
import r252_centerline_report as centerline
ROOT=Path(__file__).resolve().parents[3];BASE=Path('/Volumes/SSD/RISE/fire_production_calibration')
R264=BASE/'r264_centered_production';WORK=BASE/'r270_floor_radiation';RUN=WORK/'tier8_eps1';OUT=ROOT/'docs/data/fire_r270'
SCRATCH=Path(sys.argv[1]) if len(sys.argv)>1 else WORK  # logs and checkpoint listing
def native(args,log):
 with log.open('w') as f:r=subprocess.run([str(v) for v in args],cwd=ROOT,stdout=f,stderr=subprocess.STDOUT)
 if r.returncode:raise RuntimeError('read-only reduction failed: '+str(args[0]))
def avg(t,v,start,end):
 assert t[0]<=start and t[-1]>=end-1e-8
 tt=np.r_[start,t[(t>start)&(t<end)],end];return float(np.trapezoid(np.interp(tt,t,v),tt)/(end-start))
def measure(run):
 # Feed r250_acceptance.measure a synthetic summary built from the run's own ledgers.
 steps=len(list((run/'budgets').glob('step_*.csv')));last=list(csv.reader((run/'frame_times.csv').open()))[-1]
 summary=SCRATCH/'r270_summary_substitute.txt';summary.write_text(f'accepted_steps={steps}\naccepted_time_s={last[1]}\n')
 orig=Path.read_text
 def patched(p,*a,**k):return orig(summary if p==run/'production_capstone_summary.txt' else p,*a,**k)
 orig_bytes=Path.read_bytes
 def patched_bytes(p):return orig_bytes(summary if p==run/'production_capstone_summary.txt' else p)
 Path.read_text=patched;Path.read_bytes=patched_bytes
 try:result=r250_acceptance.measure(run)
 finally:Path.read_text=orig;Path.read_bytes=orig_bytes
 art=result['artifacts'];art[str(summary)+' (substitute for missing production_capstone_summary.txt)']=art.pop(str(run/'production_capstone_summary.txt'));return result
def main():
 SCRATCH.mkdir(exist_ok=True);acceptance=measure(RUN)
 stats=OUT/'tier8_retained_statistics.txt'
 native([ROOT/'bin/tests/FireSequenceTest','--fire-production-checkpoint-physics-diagnostic',RUN/'production_final.checkpoint',stats],SCRATCH/'retained_statistics.log')
 centerline.OUT=OUT;mc=centerline.analyze('tier8',stats)
 native([R264/'flame_height',RUN/'checkpoints',OUT/'tier8_flame_height.csv'],SCRATCH/'flame_height.log')
 h=np.genfromtxt(OUT/'tier8_flame_height.csv',delimiter=',',names=True,dtype=None,encoding='utf-8');t=h['time_s'];start=float(t[0]);end=float(t[-1]);assert len(t)==235
 height=avg(t,h['h99_realized_m'],start,end);acceptance.update(FDS_mean_instantaneous_h99_realized_m=height,FDS_flame_height_relative_error=height/acceptance['heskestad_expected_m']-1,FDS_height_definition='time mean of per-frame height containing99% realized instantaneous HRR; retained16Hzframes, trapezoidal time quadrature')
 (OUT/'tier8_acceptance.json').write_text(json.dumps(acceptance,indent=2)+'\n');(OUT/'tier8_mccaffrey.json').write_text(json.dumps(mc,indent=2)+'\n')
 paths=sorted((RUN/'checkpoint_history').glob('*.checkpoint'))+[RUN/'production_final.checkpoint'];listing=SCRATCH/'checkpoint_inputs.txt';listing.write_text('\n'.join(str(p) for p in paths)+'\n')
 native([R264/'mass_probe',listing,OUT/'tier8_mass_samples.csv'],SCRATCH/'mass_probe.log')
 data=np.genfromtxt(OUT/'tier8_mass_samples.csv',names=True,delimiter=',');fds=json.loads((ROOT/'docs/data/fire_r263/results.json').read_text())['rows'];rows=[]
 for z in [32,65,98]:
  a=data[data['z']==z];order=np.argsort(a['time_s']);a=a[order];_,index=np.unique(a['time_s'],return_index=True);a=a[index];reference=next(r for r in fds if r['z']==z)
  row={'z':z,'height_m':float(a['height_m'][0]),'r270_net_r08_kg_s':avg(a['time_s'],a['net_r08_kg_s'],start,end),'r270_up_r08_kg_s':avg(a['time_s'],a['up_r08_kg_s'],start,end),'r270_up_plane_kg_s':avg(a['time_s'],a['up_plane_kg_s'],start,end),'FDS_no_slip_net_r08_kg_s':reference['no_slip_FDS_net_r08_kg_s'],'FDS_no_slip_up_r08_kg_s':reference['no_slip_FDS_up_r08_kg_s'],'FDS_no_slip_up_plane_kg_s':reference['no_slip_FDS_up_plane_kg_s']}
  row['net_relative_error']=row['r270_net_r08_kg_s']/row['FDS_no_slip_net_r08_kg_s']-1;rows.append(row)
 with (OUT/'tier8_entrainment.csv').open('w') as f:
  w=csv.DictWriter(f,fieldnames=list(rows[0]),lineterminator='\n');w.writeheader();w.writerows(rows)
 trajectory=list(csv.DictReader((RUN/'budgets/maximum_velocity_trajectory.csv').open()));peak=max(trajectory,key=lambda r:float(r['maximum_velocity_m_per_s']))
 keys=['accepted_step','time_s','dt_s','maximum_velocity_m_per_s','axis','face','x','y','z']
 result={'acceptance':acceptance,'entrainment':rows,'McCaffrey':mc,'peak_velocity':{k:peak[k] for k in keys},'statistics_start_s':start,'statistics_end_s':end,'mass_method':'retained native checkpoint quadrature, centered MAC velocity and cellgasdensity; net and positive upward separately. No-slip FDS same full statistics interval.'}
 (OUT/'tier8_results.json').write_text(json.dumps(result,indent=2)+'\n');print(json.dumps(result,indent=2))
if __name__=='__main__':main()
