#!/usr/bin/env python3
"""Time-weighted retained-state measurements, without a solver advance."""
import csv,json,hashlib
from pathlib import Path
import numpy as np
BASE=Path('/Volumes/SSD/RISE/fire_production_calibration/r253_plume_diagnosis')
OUT=Path(__file__).resolve().parents[2]/'docs/data/fire_r253'
START=10.466040127156969
END=25.091040127212182

def reduce(path):
 rows=list(csv.DictReader(path.open()))
 keys=list(rows[0]); by={}
 for r in rows:
  if r.get('hot_area') is None:raise ValueError('incomplete native CSV')
  by.setdefault(float(r['time_s']),{})[int(r['z'])]=[float(r[k]) for k in keys]
 times=np.array(sorted(by));values=np.array([[by[t][z] for z in range(106)] for t in times])
 assert times[0]<START and times[-1]>=END and np.all(np.diff(times)>0)
 def sample_grid(indices):
  ts=times[indices];vs=values[indices];tt=np.r_[START,ts[(ts>START)&(ts<END)],END]
  vv=np.stack([np.stack([np.interp(tt,ts,vs[:,z,j]) for j in range(len(keys))],axis=1) for z in range(106)],axis=1)
  return tt,vv
 t,v=sample_grid(np.arange(len(times)));duration=END-START
 mean=np.trapezoid(v,t,axis=0)/duration
 idx={k:keys.index(k) for k in keys}
 z0=-1.02*.30+.083*33**.4;mf=33000/50027364.880448513
 results=[]
 for z in range(106):
  r={k:float(mean[z,j]) for j,k in enumerate(keys) if k!='time_s'}
  h=r['height_m'];r['correlation_far_field']=.2*33**.4<h<106*.030593116067411014-.24474492853928809/2
  r['correlation_total_kg_s']=.071*26.4**(1/3)*max(0.,h-z0)**(5/3)+.071*.026*26.4+mf
  r['mass_r08_relative_error']=r['m_r4']/r['correlation_total_kg_s']-1
  r['plane_up_relative_error']=r['mup_plane']/r['correlation_total_kg_s']-1
  r['cpheat_relative_error']=r['cpheat_plane']/26400-1
  r['hheat_relative_error']=r['hheat_plane']/26400-1
  r['temperature_rms_radius_m']=(r['temp_r2']/r['temp_sum'])**.5 if r['temp_sum']>0 else 0
  r['heat_rms_radius_m']=(r['heat_r2']/r['heat_sum'])**.5 if r['heat_sum']>0 else 0
  r['nut_heat_weighted_m2_s']=r['nut_heat_sum']/r['nut_weight'] if r['nut_weight']>0 else 0
  r['nut_area_weighted_m2_s']=r['nut_area_sum']/r['hot_area'] if r['hot_area']>0 else 0
  r['nut_peak_m2_s']=float(v[:,z,idx['nut_max']].max())
  for name,a,b in [('first_third',START,START+duration/3),('last_third',END-duration/3,END)]:
   tt=np.r_[a,t[(t>a)&(t<b)],b]
   r['ambient_'+name+'_K']=float(np.trapezoid(np.interp(tt,t,v[:,z,idx['ambient_K']]),tt)/(b-a))
  y=v[:,z,idx['ambient_K']]-300
  r['ambient_slope_K_s']=float(np.trapezoid((t-(START+END)/2)*y,t)/(duration**3/12))
  for k in ['ambient_K','xlo_K','xhi_K','ylo_K','yhi_K','far08_K']:
   r[k+'_min']=float(v[:,z,idx[k]].min());r[k+'_max']=float(v[:,z,idx[k]].max())
  results.append(r)
 OUT.mkdir(parents=True,exist_ok=True)
 def write_csv(p,rs):
  with p.open('w') as f:
   w=csv.DictWriter(f,fieldnames=list(rs[0]),lineterminator='\n');w.writeheader();w.writerows(rs)
 write_csv(OUT/'mean_height_profile.csv',results)
 selected=sorted(set(min(range(106),key=lambda i:abs(results[i]['height_m']-h)) for h in [.5,.8,1,1.25,1.5,2,2.5,3,3.044,3.1,3.2]))
 signals=[]
 for n,time in enumerate(t):
  for z in selected:
   signals.append({k:float(v[n,z,idx[k]]) for k in ['time_s','z','height_m','ambient_K','xlo_K','xhi_K','ylo_K','yhi_K','far08_K','center_K','center_w','m_r4','m_plane','mup_plane','cpheat_plane','hheat_plane']})
 # The endpoint's time column must reflect the interpolated measurement time.
 for n,time in enumerate(t):
  for j in range(len(selected)):signals[n*len(selected)+j]['time_s']=float(time)
 write_csv(OUT/'selected_height_timeseries.csv',signals)
 # Actual retained sampling sensitivity, not a new acceptance tolerance.
 coarse={}
 for phase in [0,1]:
  ids=sorted(set([0,len(times)-1]+list(range(phase,len(times),2))))
  tc,vc=sample_grid(ids);mc=np.trapezoid(vc,tc,axis=0)/duration
  mask=np.array([.81<r['height_m']<3.12 for r in results])
  coarse[str(phase)]={k:float(np.max(np.abs((mc[:,idx[k]][mask]-mean[:,idx[k]][mask])/mean[:,idx[k]][mask]))) for k in ['m_r4','mup_plane','cpheat_plane','hheat_plane']}
 exact=list(csv.DictReader((OUT.parent/'fire_r252/r251_centerline.csv').open()))
 check={}
 for metric,ref in [('center_K','temperature_rise_K'),('center_w','velocity_m_per_s')]:
  offsets=[]
  for z,r in enumerate(results):
   if .81<r['height_m']<3.12:
    observed=r[metric]-(300 if metric=='center_K' else 0)
    target=float(exact[z][ref]);offsets.append(abs(observed/target-1))
  check[metric]=max(offsets)
 summary={'sampled_centerline_relative_difference_from_accepted_integrals':check,'statistics_start_s':START,'statistics_end_s':END,'duration_s':duration,'native_snapshots':len(times),'window_nodes':len(t),'maximum_window_sample_gap_s':float(np.diff(t).max()),'virtual_origin_m':z0,'reference_Qc_kW':26.4,'reference_Q_total_kW':33,'correlation':'NIST SP1026 (2009),eq3.32 plus burner mass(eq3.33); kg/s, kW,m','coarsening_relative_difference':coarse,'instant_native_csv':str(path),'instant_native_csv_sha256':hashlib.sha256(path.read_bytes()).hexdigest(),'selected_heights':[results[z] for z in selected]}
 (OUT/'results.json').write_text(json.dumps(summary,indent=2)+'\n')
 print('snapshots',len(times),'window_nodes',len(t),'gap',summary['maximum_window_sample_gap_s'])
 for z in selected:
  r=results[z];print('z %.3f Tamb %.9f slope%+.6g M.8 %.4f Mup %.4f ref %.4f Qcp %.3f Qh %.3f bT %.3f bH %.3f nuH %.6g nuA %.6g nuMax %.6g'%(r['height_m'],r['ambient_K'],r['ambient_slope_K_s'],r['m_r4'],r['mup_plane'],r['correlation_total_kg_s'],r['cpheat_plane']/1000,r['hheat_plane']/1000,r['temperature_rms_radius_m'],r['heat_rms_radius_m'],r['nut_heat_weighted_m2_s'],r['nut_area_weighted_m2_s'],r['nut_peak_m2_s']))
 print('coarsening',coarse)
if __name__=='__main__':reduce(BASE/'instant_planes.csv')
