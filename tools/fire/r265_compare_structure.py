#!/usr/bin/env python3
"""Retained r251/no-slip FDS flame structure; no solver changes or simulation."""
from pathlib import Path
import csv,json,hashlib
import numpy as np
from r261_reduce_full import read_slice,DX,START,END
import matplotlib
matplotlib.use('Agg')
import matplotlib.pyplot as plt
ROOT=Path(__file__).resolve().parents[2];BASE=Path('/Volumes/SSD/RISE/fire_production_calibration');OUT=ROOT/'docs/data/fire_r265';OUT.mkdir(exist_ok=True)
FDS=BASE/'r263_fds_no_slip/fds_run';WORK=BASE/'r265_structure'
def write(name,rows):
 with (OUT/name).open('w') as f:
  w=csv.DictWriter(f,fieldnames=list(rows[0]),lineterminator='\n');w.writeheader();w.writerows(rows)
def avg(t,a):
 assert t[0]<=START and t[-1]>=END-1e-6
 t=t.copy()
 if abs(t[-1]-END)<1e-6:t[-1]=END
 keep=(t>START)&(t<END);tt=np.r_[START,t[keep],END]
 first=np.array([np.interp(START,t,v) for v in a.reshape(len(t),-1).T]).reshape(a.shape[1:]);last=np.array([np.interp(END,t,v) for v in a.reshape(len(t),-1).T]).reshape(a.shape[1:])
 return np.trapezoid(np.concatenate([first[None],a[keep],last[None]]),tt,axis=0)/(END-START)
def field(z,offset):
 path=FDS/f'r263_fds_1_{107+12*z+offset}.sf';label,limits,t,a=read_slice(path);assert limits[-1]==z+1
 sources[str(path)]=path.stat().st_size
 return t,a
xy=(np.arange(69)-34)*DX;rr=np.hypot(xy[:,None],xy[None,:]);ring=np.floor(rr/DX+.5).astype(int)
def radius90(q):
 # Heat is integrated with actual Cartesian-cell area per annulus, then the
 # crossing annulus is fractionally filled in r^2. Width is twice this radius.
 assert np.isfinite(q).all() and np.min(q)>=0
 weights=np.bincount(ring.ravel(),weights=(q*DX**2).ravel());total=weights.sum();target=.9*total;cum=np.cumsum(weights);k=int(np.searchsorted(cum,target));before=0 if k==0 else cum[k-1];fraction=(target-before)/weights[k];lo=max(0,(k-.5)*DX);hi=(k+.5)*DX
 estimate=float(np.sqrt(lo*lo+fraction*(hi*hi-lo*lo)))
 # Discrete cell-center quantile provides a second no-interpolation convention.
 sort=np.argsort(rr.ravel());cs=np.cumsum((q*DX**2).ravel()[sort]);discrete=float(rr.ravel()[sort[np.searchsorted(cs,target)]])
 return {'plane_heat_W_per_m':float(total),'R90_m':estimate,'D90_m':2*estimate,'R90_annulus_lower_m':lo,'R90_annulus_upper_m':hi,'crossing_annulus_heat_fraction':float(fraction),'R90_cellcenter_quantile_m':discrete}
rad=np.genfromtxt(ROOT/'docs/data/fire_r254/frames_radial_profiles.csv',names=True,delimiter=',');heat=np.genfromtxt(WORK/'r251_HRR_planes.csv',names=True,delimiter=',');div=np.genfromtxt(BASE/'r258_fds_les/r251_near_base.csv',names=True,delimiter=',')
sources={};profiles=[];widths=[]
fig,axes=plt.subplots(2,4,figsize=(16,8),layout='constrained')
for row,z in enumerate([9,16]):
 t,q=field(z,0);tt,T=field(z,1);tu,U=field(z,2);tv,V=field(z,3);tw,W=field(z,4);tl,L=field(z-1,4);td,D=field(z,6)
 for time in [tt,tu,tv,tw,tl,td]:assert np.array_equal(t,time)
 actual=(U[:,1:,1:]-U[:,:-1,1:]+V[:,1:,1:]-V[:,1:,:-1]+W[:,1:,1:]-L[:,1:,1:])/DX
 fds={'HRR_W_m3':avg(t,q[:,1:,1:]*1000),'temperature_rise_K':avg(t,T[:,1:,1:]+273.15-300),'w_m_s':avg(t,.5*(W[:,1:,1:]+L[:,1:,1:])),'divergence_per_s':avg(t,actual),'abs_divergence_per_s':avg(t,np.abs(actual)),'thermodynamic_divergence_target_per_s':avg(t,D[:,1:,1:])}
 h=heat[heat['z']==z];d=div[div['z']==z];r=rad[rad['z']==z];assert len(h)==69*69 and len(d)==69*69
 # All spatially averaged quantities are arrays indexed [x,y].
 rq=np.empty((69,69));rd=np.empty_like(rq);rda=np.empty_like(rq)
 for v in h:rq[int(v['x']),int(v['y'])]=v['mean_HRR_W_m3']
 for v in d:rd[int(v['x']),int(v['y'])]=v['mean_divergence'];rda[int(v['x']),int(v['y'])]=v['mean_abs_divergence']
 for code,values in [('r251',rq),('FDS_no_slip',fds['HRR_W_m3'])]:widths.append({'code':code,'z':z,'height_m':(z+.5)*DX,**radius90(values)})
 for k in range(27):
  m=ring==k;rv=r[r['ring']==k][0];item={'z':z,'height_m':(z+.5)*DX,'ring':k,'r_m':float(rr[m].mean()),'cells':int(m.sum()),'r251_HRR_W_m3':float(rq[m].mean()),'r251_temperature_rise_K':float(rv['theta_mean']),'r251_w_m_s':float(rv['w_mean']),'r251_divergence_per_s':float(rd[m].mean()),'r251_abs_divergence_per_s':float(rda[m].mean())}
  for key,value in fds.items():item['FDS_'+key]=float(value[m].mean())
  profiles.append(item)
 for col,(key,label) in enumerate([('HRR_W_m3','Mean heat release (kW/m³)'),('temperature_rise_K','Temperature rise (K)'),('w_m_s','Vertical velocity (m/s)'),('divergence_per_s','Actual MAC divergence (1/s)')]):
  p=[v for v in profiles if v['z']==z];radius=[v['r_m'] for v in p];factor=.001 if key=='HRR_W_m3' else 1.
  ax=axes[row,col];ax.plot(radius,[v['r251_'+key]*factor for v in p],label='RISE r251');ax.plot(radius,[v['FDS_'+key]*factor for v in p],label='FDS no-slip');ax.set(xlabel='Radius (m)',ylabel=label,title=f'z={(z+.5)*DX:.3f}m',xlim=(0,.4));ax.grid(alpha=.25);ax.legend(fontsize=8)
write('radial_structure.csv',profiles);write('reaction_zone_width.csv',widths);fig.savefig(OUT/'radial_structure.png',dpi=160)
result={'statistics_start_s':START,'statistics_end_s':END,'r251_HRR_T_w_samples':235,'r251_divergence_sampling':'204 nonzero retained-checkpoint quadrature weights from r254/r258 (214path input list); not the235frame time grid','FDS_sampling':'physical slices, native timestamps, trapezoidal time integral; actual MAC divergence reconstructed from native staggered U/V/W, separately from thermodynamic target','reaction_zone_definition':'R90 radial extent containing90%of time-mean plane HRR using actual Cartesian-cell area per annulus; interpolate crossing annulus in radius squared; D90=2R90; cell-center quantile also reported','widths':widths,'source_sizes':sources,'source_sha256':{str(p):hashlib.sha256(p.read_bytes()).hexdigest() for p in [ROOT/'tools/fire/r265_reaction_planes.cpp',ROOT/'tools/fire/r265_compare_structure.py',WORK/'r251_HRR_planes.csv',BASE/'r251_tier8_full_window/frame_times.csv',BASE/'r258_fds_les/r251_near_base.csv',ROOT/'docs/data/fire_r254/frames_radial_profiles.csv',ROOT/'docs/data/fire_r263/r263_fds.fds']},'read_only':True}
result['width_comparison']=[]
for z in [9,16]:
 a=next(v for v in widths if v['z']==z and v['code']=='r251');b=next(v for v in widths if v['z']==z and v['code']=='FDS_no_slip');result['width_comparison'].append({'z':z,'height_m':(z+.5)*DX,'R90_relative_RISE_vs_FDS':a['R90_m']/b['R90_m']-1,'R90_difference_m':a['R90_m']-b['R90_m'],'difference_in_dx':(a['R90_m']-b['R90_m'])/DX})
(OUT/'structure_results.json').write_text(json.dumps(result,indent=2)+'\n');print(json.dumps({k:v for k,v in result.items() if k!='source_sizes'},indent=2))
