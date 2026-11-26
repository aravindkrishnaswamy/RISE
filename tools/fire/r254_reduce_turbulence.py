#!/usr/bin/env python3
"""Eulerian temporal variance first; radial averaging second. No simulation."""
from pathlib import Path
import csv,json
import numpy as np
BASE=Path('/Volumes/SSD/RISE/fire_production_calibration/r254_turbulence_diagnosis')
OUT=Path(__file__).resolve().parents[2]/'docs/data/fire_r254'
DX=.030593116067411014

def write(name,rows):
 OUT.mkdir(parents=True,exist_ok=True)
 with (OUT/name).open('w') as f:
  w=csv.DictWriter(f,fieldnames=list(rows[0]),lineterminator='\n');w.writeheader();w.writerows(rows)

def reduce(name,prefix):
 a=np.genfromtxt(BASE/name,names=True,delimiter=',');profiles=[];radials=[]
 for z in range(106):
  p=a[a['z']==z];center=p[(p['x']==34)&(p['y']==34)][0]
  wvar=np.maximum(0,p['w2']-p['w']**2);tvar=np.maximum(0,p['theta2']-p['theta']**2)
  r={'z':z,'height_m':(z+.5)*DX,'center_w_mean_m_s':float(center['w']),'center_theta_mean_K':float(center['theta'])}
  r['center_w_rms_m_s']=float(np.sqrt(max(0,center['w2']-center['w']**2)))
  r['center_theta_rms_K']=float(np.sqrt(max(0,center['theta2']-center['theta']**2)))
  r['center_w_rms_over_mean']=r['center_w_rms_m_s']/r['center_w_mean_m_s'] if center['w'] else 0
  r['center_theta_rms_over_rise']=r['center_theta_rms_K']/r['center_theta_mean_K'] if center['theta'] else 0
  r['center_T_rms_over_absolute_mean']=r['center_theta_rms_K']/(center['theta']+300)
  # Fixed annuli: center point, then width dx annuli centered on k*dx.
  bins=np.floor(p['r_m']/DX+.5).astype(int);rings=[]
  for k in range(27):
   mask=bins==k
   if not mask.any():continue
   q={'z':z,'height_m':(z+.5)*DX,'ring':k,'r_m':float(p['r_m'][mask].mean()),'cells':int(mask.sum()),'w_mean':float(p['w'][mask].mean()),'theta_mean':float(p['theta'][mask].mean()),'w_variance':float(wvar[mask].mean()),'theta_variance':float(tvar[mask].mean())}
   rings.append(q);radials.append(q)
  for key,mean,var in [('w','w_mean','w_variance'),('theta','theta_mean','theta_variance')]:
   target=float(center[key])/2;edge=None
   for left,right in zip(rings,rings[1:]):
    if left[mean]>=target>right[mean]:
     f=(target-left[mean])/(right[mean]-left[mean]);edge=(1-f)*left['r_m']+f*right['r_m'];variance=(1-f)*left[var]+f*right[var];break
   r[key+'_halfwidth_m']=edge
   r[key+'_halfwidth_rms_over_local_mean']=float(np.sqrt(variance)/target) if edge is not None and target>0 else None
   if key=='theta':r['halfwidth_T_rms_over_absolute_mean']=float(np.sqrt(variance)/(target+300)) if edge is not None else None
  if 'rho' in a.dtype.names:
   mask=p['r_m']<.8;flux=float(p['rhow'][mask].sum()*DX**2);meanflux=float((p['rho'][mask]*p['w'][mask]).sum()*DX**2)
   r.update(axial_mass_kg_s=flux,axial_mean_product_kg_s=meanflux,axial_covariance_kg_s=flux-meanflux,axial_covariance_fraction=(flux-meanflux)/flux)
  profiles.append(r)
 write(prefix+'_height_profile.csv',profiles);write(prefix+'_radial_profiles.csv',radials)
 return profiles

if __name__=='__main__':
 frames=reduce('frame_moments.csv','frames')
 for h in [.5,1,1.5,2,2.5,3]:
  r=min(frames,key=lambda r:abs(r['height_m']-h));print({k:round(v,5) if isinstance(v,float) else v for k,v in r.items()})
 if (BASE/'cell_moments.csv').stat().st_size>0:
  checkpoints=reduce('cell_moments.csv','checkpoints')
  n=np.genfromtxt(BASE/'viscosity.csv',names=True,delimiter=',');rows=[]
  for p in n:
   den=float(p['weight']);rows.append({'z':int(p['z']),'height_m':float(p['height_m']),'nu_production_m2_s':float(p['nu_production_sum']/den) if den else 0,'nu_fds_m2_s':float(p['nu_fds_sum']/den) if den else 0,'relative_L1_difference':float(p['absolute_difference_sum']/p['nu_fds_sum']) if p['nu_fds_sum'] else 0,'rms_velocity_gradient_per_s':float(np.sqrt(p['gradient_norm_sum']/den)) if den else 0})
  write('viscosity_profile.csv',rows)
  summary={'statistics_start_s':10.466040127212182,'statistics_end_s':25.091040127212182,'frames':235,'frame_spacing_s':.0625,'checkpoint_input_paths':214,'nonzero_checkpoint_weights':sum(float(l.split()[0])>0 for l in (BASE/'weighted_inputs.txt').read_text().splitlines()),'selected':[]}
  for h in [.5,1,1.5,2,2.5,3]:
   r=min(frames,key=lambda r:abs(r['height_m']-h));i=r['z'];summary['selected'].append({'frames':r,'checkpoints':checkpoints[i],'viscosity':rows[i]})
  (OUT/'results.json').write_text(json.dumps(summary,indent=2)+'\n')
