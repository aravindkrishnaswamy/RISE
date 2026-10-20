#!/usr/bin/env python3
"""Accepted-step alpha activity and same-window RISE entrainment, without identity tests."""
from pathlib import Path
import csv,json
import numpy as np
ROOT=Path(__file__).resolve().parents[2];BASE=Path('/Volumes/SSD/RISE/fire_production_calibration/r256_limiter_diagnostic');OUT=ROOT/'docs/data/fire_r256'
def write(name,rows):
 with (OUT/name).open('w') as f:
  w=csv.DictWriter(f,fieldnames=list(rows[0]),lineterminator='\n');w.writeheader();w.writerows(rows)
def main():
 receipt=dict(l.split(' ',1) for l in (BASE/'run/result.txt').read_text().splitlines() if ' ' in l)
 if receipt['succeeded']!='1':raise ValueError('continuation incomplete')
 a=np.genfromtxt(BASE/'run/limiter_instant.csv',names=True,delimiter=',');rows=[]
 for axis in [-1,0,1,2]:
  for z in np.unique(a['z']).astype(int):
   p=a[(a['z']==z)&((a['axis']==axis) if axis>=0 else True)];den=float(np.sum(p['dt_s']*p['faces']));count=float(np.sum(p['dt_s']*p['limited']));alpha=float(np.sum(p['dt_s']*p['alpha_sum']))
   rows.append({'axis':axis,'z':int(z),'slab_center_height_m':float(p[0]['height_m']),'face_seconds':den,'scalar_limited_fraction':count/den if den else None,'shared_momentum_carrier_limited_fraction':count/den if den else None,'mean_scalar_alpha':alpha/den if den else None,'mean_momentum_carrier_source_alpha':alpha/den if den else None,'independent_momentum_alpha':'not defined; centered velocity factor','note':'both numeric alpha columns are the same shared primal field; not an averaged dual-face alpha'})
 write('alpha_height_profile.csv',rows)
 m=np.genfromtxt(BASE/'run/mass_timeseries.csv',names=True,delimiter=',');start=float(receipt['start_time_s']);end=float(receipt['end_time_s']);duration=end-start
 old=np.genfromtxt(BASE.parent/'r253_plume_diagnosis/instant_planes.csv',names=True,delimiter=',');full={int(r['z']):r for r in csv.DictReader((ROOT/'docs/data/fire_r253/mean_height_profile.csv').open())};masses=[]
 for z in np.unique(m['z']).astype(int):
  p=m[m['z']==z];ref=old[old['z']==z];t=ref['time_s'];tt=np.r_[start,t[(t>start)&(t<end)],end]
  if t[0]>start or t[-1]<end:raise ValueError('reference does not bracket diagnostic')
  native=float(p[-1]['integrated_mass_kg']/duration);independent=float(np.trapezoid(p['mass_r08_kg_s'],p['time_s'])/duration);baseline=float(np.trapezoid(np.interp(tt,t,ref['m_r4']),tt)/duration)
  masses.append({'z':int(z),'height_m':float(p[0]['height_m']),'diagnostic_mass_kg_s':native,'independent_trapezoid_kg_s':independent,'r251_same_window_kg_s':baseline,'relative_difference_from_r251_window':native/baseline-1,'r251_full_window_kg_s':float(full[z]['m_r4']),'heskestad_26p4kW_kg_s':float(full[z]['correlation_total_kg_s'])})
 write('entrainment_comparison.csv',masses)
 result={'identity_requirement':'waived by owner; no timestep or state equality tests','statistics_start_s':start,'statistics_end_s':end,'duration_s':duration,'accepted_steps':len(np.unique(a['step'])),'alpha_mask':'r<0.8m and adjacent accepted T mean>301K; axes separate and combined','vertical_binning':'z faces assigned to slab immediately above, top face folded into final slab','momentum_alpha_interpretation':'shared primal scalar-carrier alpha; no independent velocity alpha or average of alpha on dual faces','selected_alpha':[min([r for r in rows if r['axis']==-1],key=lambda r:abs(r['slab_center_height_m']-h)) for h in [1,1.5,2,2.5,3]],'entrainment':masses,'decision':'pending interpretation; no A/B or production change selected by reducer'}
 (OUT/'results.json').write_text(json.dumps(result,indent=2)+'\n');print(json.dumps(result,indent=2))
if __name__=='__main__':main()
