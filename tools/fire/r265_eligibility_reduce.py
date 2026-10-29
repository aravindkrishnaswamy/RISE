#!/usr/bin/env python3
"""Summarize source-only reconstructions at retained beginnings; no solver steps."""
from pathlib import Path
import csv,json,hashlib
import numpy as np
ROOT=Path(__file__).resolve().parents[2];BASE=Path('/Volumes/SSD/RISE/fire_production_calibration');WORK=BASE/'r265_reaction_structure';OUT=ROOT/'docs/data/fire_r265'
def write(name,rows):
 with (OUT/name).open('w') as f:
  w=csv.DictWriter(f,fieldnames=list(rows[0]),lineterminator='\n');w.writeheader();w.writerows(rows)
def main():
 a=np.genfromtxt(WORK/'eligibility_samples.csv',names=True,delimiter=',');b=np.genfromtxt(WORK/'eligibility_budget_check.csv',names=True,delimiter=',')
 assert len(a)%106==0 and len(b)==len(a)//106
 times=np.unique(a['time_s']);assert len(times)==len(b)
 names=['total_heat_W','warm_heat_W','edge_heat_W','holder_cold_heat_W','unclassified_cold_heat_W','warm_burning_cells','edge_burning_cells','holder_cold_burning_cells','unclassified_cold_burning_cells','cold_parcel_ineligible_fuel_kg']
 means=[]
 for z in range(106):
  p=a[a['z']==z];assert abs(p['weight'].sum()-1)<1e-10
  row={'z':z,'height_m':float(p[0]['height_m'])}
  for key in names:row['mean_'+key]=float(np.dot(p['weight'],p[key]))
  for key in ['warm','edge','holder_cold','unclassified_cold']:row[key+'_heat_fraction']=row['mean_'+key+'_heat_W']/row['mean_total_heat_W'] if row['mean_total_heat_W'] else 0.
  means.append(row)
 write('eligibility_height.csv',means)
 result=[]
 for region,mask in [('plane_0p29m',a['z']==9),('plane_0p5m',a['z']==16),('below_1m',a['height_m']<1.),('whole_domain',np.ones(len(a),dtype=bool))]:
  p=a[mask];row={'region':region,'sampled_beginnings':len(times),'beginning_min_s':float(times.min()),'beginning_max_s':float(times.max())}
  spatial=np.clip((1-p['z']*.030593116067411014)/.030593116067411014,0,1) if region=='below_1m' else np.ones(len(p))
  w=p['weight']*spatial
  heat={key:float(np.dot(w,p[key+'_heat_W'])) for key in ['total','warm','edge','holder_cold','unclassified_cold']}
  row.update({'mean_'+key+'_heat_W':value for key,value in heat.items()})
  for key in ['warm','edge','holder_cold','unclassified_cold']:row[key+'_heat_fraction']=heat[key]/heat['total'] if heat['total'] else 0.
  not_end=p['endpoint_uses_last_dt']==0
  row['edge_fraction_excluding_final_endpoint']=float(np.dot(w[not_end],p['edge_heat_W'][not_end])/np.dot(w[not_end],p['total_heat_W'][not_end]))
  mid=.5*(times.min()+times.max());parts=[]
  for select in [p['time_s']<=mid,p['time_s']>mid]:
   den=np.dot(w[select],p['total_heat_W'][select]);parts.append(float(np.dot(w[select],p['edge_heat_W'][select])/den) if den else 0.)
  row['first_half_edge_fraction'],row['second_half_edge_fraction']=parts;result.append(row)
 write('eligibility_summary.csv',result)
 finite=b[b['endpoint']==0];maxrel=float(np.max(np.abs(finite['relative_difference'])));rmsrel=float(np.sqrt(np.mean(finite['relative_difference']**2)))
 checks={'source_samples_compared':len(finite),'max_abs_relative_source_budget_difference':maxrel,'rms_relative_source_budget_difference':rmsrel,'final_endpoint_weight':float(a[a['z']==9]['weight'][-1]),'total_weight':float(a[a['z']==9]['weight'].sum())}
 assert np.max(a['unclassified_cold_heat_W'])<1e-10
 # A source reconstruction can be called the actual sampled next source only if it matches the retained budget.
 checks['sampled_next_source_matches_budget']=maxrel<1e-6
 accepted=json.loads((ROOT/'docs/data/fire_r251/full_window_acceptance.json').read_text());checks['all_step_mean_hrr_W']=float(accepted['mean_hrr_W']);checks['sampled_source_mean_hrr_W']=next(r['mean_total_heat_W'] for r in result if r['region']=='whole_domain');checks['sampled_mean_hrr_relative_difference']=checks['sampled_source_mean_hrr_W']/checks['all_step_mean_hrr_W']-1
 frames=np.genfromtxt(BASE/'r259_inlet_forcing/r251_planes.csv',names=True,delimiter=',');plane_checks=[]
 for z in [9,16]:
  f=frames[frames['z']==z];reference=float(np.trapezoid(f['slab_heat_W'],f['time_s'])/(f['time_s'][-1]-f['time_s'][0]));sample=means[z]['mean_total_heat_W'];plane_checks.append({'z':z,'sampled_source_mean_plane_W':sample,'retained_235_frame_mean_plane_W':reference,'relative_difference':sample/reference-1})
 checks['plane_sampling_checks']=plane_checks
 data={'scope':'checkpoint-sampled canonical source reconstruction at retained r251 beginning states; no solver stage or new physical run; not an all-step historical eligibility log','records':'actual next accepted dt from final checkpoint; source case+mixing-time coefficients+canonical source authority unchanged; final endpoint reuses last dt and is separately sensitivity-checked','temperature_classification':'eligibilityBeginning conservative state inverted by same canonical accepted-state inversion, before source; warm strictly T>600K; edge T<=600K and connected eligible; floor-holder cold separate','weights':'existing normalized retained-checkpoint quadrature from r254; same weight for every layer within a beginning; divide weighted category heat by weighted total heat, not average instantaneous percentages','limitations':['Retained snapshots and VDB temperature are post-step whereas reaction is the preceding source packet; directly masking that HRR by the post-step temperature would mislabel edge-waived burning.','These are sparse retained beginning reconstructions. Full all-step heat-weighted historical classification was not logged.','Two temporal-half fractions and endpoint exclusion quantify observed sampling sensitivity, not an ensemble confidence interval.','Eligibility is a binary rule. A small or large waived fraction does not alone establish whether it causes a radial-width difference; that would need a controlled change, which is not authorized here.'],'checks':checks,'summary':result,'sha256':{str(p):hashlib.sha256(p.read_bytes()).hexdigest() for p in [ROOT/'tools/fire/r265_eligibility_probe.cpp',ROOT/'src/Library/Utilities/FireProductionSource.cpp',ROOT/'src/Library/Utilities/FireProductionSourceKernel.h',WORK/'eligibility_probe',WORK/'build_eligibility.sh',WORK/'build_eligibility.log',WORK/'eligibility_samples.csv',WORK/'eligibility_budget_check.csv',BASE/'r254_turbulence_diagnosis/weighted_inputs.txt']}}
 (OUT/'eligibility_results.json').write_text(json.dumps(data,indent=2)+'\n')
 write('eligibility_budget_check.csv',[dict(zip(b.dtype.names,[float(v) for v in r])) for r in b]);print(json.dumps(data,indent=2))
if __name__=='__main__':main()
