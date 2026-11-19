#!/usr/bin/env python3
"""Reduce the bounded limiter diagnostic and canonical tier-6 run."""
from pathlib import Path
import csv,json,sys
import numpy as np
ROOT=Path(__file__).resolve().parents[2];BASE=Path('/Volumes/SSD/RISE/fire_production_calibration/r255_comparison');OUT=ROOT/'docs/data/fire_r255'
def write(name,rows):
 with (OUT/name).open('w') as f:
  w=csv.DictWriter(f,fieldnames=list(rows[0]),lineterminator='\n');w.writeheader();w.writerows(rows)
def limiter():
 a=np.genfromtxt(BASE/'limiter_run/limiter_instant.csv',names=True,delimiter=',');result=[]
 for axis in [-1,0,1,2]:
  for z in np.unique(a['z']).astype(int):
   p=a[(a['z']==z)&((a['axis']==axis) if axis>=0 else True)];den=float(np.sum(p['dt_s']*p['faces']));nlim=float(np.sum(p['dt_s']*p['limited']));blend=float(np.sum(p['dt_s']*p['low_order_sum']));alpha=float(np.sum(p['dt_s']*p['alpha_sum']))
   result.append({'axis':axis,'z':int(z),'height_m':float(p[0]['height_m']),'z_face_height_m':float((z+.5)*(.030593116067411014)-.5*.030593116067411014) if axis==2 and z<105 else None,'vertical_binning':'z-face assigned to slab immediately above; top face also in last slab' if axis in [-1,2] else 'native transverse-face center','face_seconds':den,'limited_fraction':nlim/den if den else None,'mean_low_order_blend':blend/den if den else None,'mean_alpha':alpha/den if den else None,'mean_blend_on_limited_faces':blend/nlim if nlim else None})
 write('limiter_height_profile.csv',result)
 eq=list(csv.DictReader((BASE/'limiter_run/equivalence.csv').open()));assert len(eq)==2 and all(r['bit_identical']=='1' and r['mismatches']=='0' for r in eq)
 (OUT/'limiter_results.json').write_text(json.dumps({'mask':'MAC primary faces at r<0.8 m, mean of adjacent accepted temperatures>301 K','coefficient':'accepted R1 scalar-carrier alpha inherited by compatible momentum; not an independent momentum-velocity alpha','time_start_s':float(a['beginning_s'].min()),'time_end_s':float(np.max(a['beginning_s']+a['dt_s'])),'accepted_steps':len(np.unique(a['step'])),'bit_identical_reference_states':eq,'selected':[min([r for r in result if r['axis']==-1],key=lambda r:abs(r['height_m']-h)) for h in [1,1.5,2,2.5,3]]},indent=2)+'\n')
def tier6():
 run=BASE/'tier6_run';summary=dict(l.split('=',1) for l in (run/'production_capstone_summary.txt').read_text().splitlines() if '=' in l);start=float(summary['statistics_start_s']);end=float(summary['accepted_time_s'])
 center=list(csv.DictReader((BASE/'tier6_centerline_native.csv').open()));tip=max(float(r['height_m']) for r in center if float(r['plane_heat_W'])>=.01*max(float(q['plane_heat_W']) for q in center));dx=2*float(center[0]['height_m']);lower=max(tip,.2*33**.4);upper=float(center[-1]['height_m'])+dx/2-.24474492853928809/2
 for r in center:
  for k in list(r):r[k]=float(r[k])
  h=r['height_m'];tref=23.3*(h/33**.4)**(-5/3);wref=1.13*33**.2*(h/33**.4)**(-1/3)
  r.update(selected=lower<h<=upper,mccaffrey_theta_K=tref,mccaffrey_w_m_s=wref,temperature_relative_error=r['temperature_rise_K']/tref-1,velocity_relative_error=r['velocity_m_s']/wref-1)
 write('tier6_centerline.csv',center)
 a=np.genfromtxt(BASE/'tier6_instant_planes.csv',names=True,delimiter=',');zs=np.unique(a['z']).astype(int);profile=[]
 for z in zs:
  p=a[a['z']==z];ids=np.argsort(p['time_s']);p=p[ids];t=p['time_s'];assert t[0]<start and t[-1]>=end
  tt=np.r_[start,t[(t>start)&(t<end)],end]
  def avg(k):return float(np.trapezoid(np.interp(tt,t,p[k]),tt)/(end-start))
  h=float(p[0]['height_m']);ref=.071*26.4**(1/3)*max(0,h-.0301117293)**(5/3)+.071*.026*26.4+33000/50027364.880448513
  profile.append({'z':int(z),'height_m':h,'mass_r08_kg_s':avg('m_r4'),'whole_plane_up_kg_s':avg('mup_plane'),'convective_cp_kW':avg('cpheat_plane')/1000,'heskestad_total_kg_s':ref,'entrainment_relative_error':avg('m_r4')/ref-1})
 write('tier6_mass_profile.csv',profile);selected=[r for r in center if r['selected']]
 (OUT/'tier6_results.json').write_text(json.dumps({'statistics_start_s':start,'statistics_end_s':end,'statistics_duration_s':end-start,'native_dx_m':dx,'lower_station_exclusive_m':lower,'upper_station_inclusive_m':upper,'max_temperature_error_station':max(selected,key=lambda r:abs(r['temperature_relative_error'])),'max_velocity_error_station':max(selected,key=lambda r:abs(r['velocity_relative_error'])),'selected_mass':[min(profile,key=lambda r:abs(r['height_m']-h)) for h in [1,1.5,2,2.5,3]]},indent=2)+'\n')
if __name__=='__main__':globals()[sys.argv[1]]()
