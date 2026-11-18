#!/usr/bin/env python3
"""Read-only r251/r261 native-plane comparison over the accepted statistics window."""
from pathlib import Path
import csv,json
import numpy as np
from r261_reduce_full import read_slice,DX,START,END,RUN
from r255_reduce_fds import csv_data
import matplotlib
matplotlib.use('Agg')
import matplotlib.pyplot as plt
ROOT=Path(__file__).resolve().parents[2];BASE=Path('/Volumes/SSD/RISE/fire_production_calibration');OUT=ROOT/'docs/data/fire_r262';OUT.mkdir(exist_ok=True)
def write(name,rows):
 with (OUT/name).open('w') as f:
  w=csv.DictWriter(f,fieldnames=list(rows[0]),lineterminator='\n');w.writeheader();w.writerows(rows)
def avg(t,a):
 assert t[0]<=START and t[-1]>=END-1e-6
 # Binary32 slice times round the known endpoint; use the exact requested endpoint.
 t=t.copy()
 if abs(t[-1]-END)<1e-6:t[-1]=END
 keep=(t>START)&(t<END);tt=np.r_[START,t[keep],END]
 first=np.array([np.interp(START,t,v) for v in a.reshape(len(t),-1).T]).reshape(a.shape[1:])
 last=np.array([np.interp(END,t,v) for v in a.reshape(len(t),-1).T]).reshape(a.shape[1:])
 return np.trapezoid(np.concatenate([first[None],a[keep],last[None]]),tt,axis=0)/(END-START)
def field(z,offset):
 label,limits,t,a=read_slice(RUN/f'r261_fds_1_{107+12*z+offset}.sf')
 assert limits[-1]==z+1,(z,limits)
 return t,a[:,1:,1:]
def half(r,v):
 target=v[0]/2
 for i in range(1,len(r)):
  if v[i-1]>=target>v[i]:return float(r[i-1]+(target-v[i-1])/(v[i]-v[i-1])*(r[i]-r[i-1]))
 return None
rad=list(csv.DictReader((ROOT/'docs/data/fire_r254/frames_radial_profiles.csv').open()))
flux=list(csv.DictReader((ROOT/'docs/data/fire_r253/mean_height_profile.csv').open()))
xy=(np.arange(69)-34)*DX;rr=np.hypot(xy[:,None],xy[None,:]);rings=np.floor(rr/DX+.5).astype(int);mask=rr<.8
profiles=[];widths=[];mass=[]
fig,axes=plt.subplots(3,2,figsize=(10,10),layout='constrained')
for row,z in enumerate([9,16,32]):
 t,T=field(z,1);tw,W=field(z,4);tl,L=field(z-1,4);assert np.array_equal(t,tw) and np.array_equal(t,tl)
 fields={'theta':avg(t,T+273.15-300),'w':avg(t,.5*(W+L))}
 tr,rho=field(z,5);assert np.array_equal(t,tr);common_flux=rho*.5*(W+L)
 rise=[v for v in rad if int(v['z'])==z]
 for col,key in enumerate(['theta','w']):
  radius=np.array([rr[rings==k].mean() for k in range(27)]);values=np.array([fields[key][rings==k].mean() for k in range(27)])
  ar=np.array([float(v['r_m']) for v in rise]);av=np.array([float(v[key+'_mean']) for v in rise]);assert np.allclose(ar,radius)
  widths.append({'z':z,'height_m':(z+.5)*DX,'quantity':key,'r251_halfwidth_m':half(ar,av),'FDS_halfwidth_m':half(radius,values),'r251_center':float(av[0]),'FDS_center':float(values[0])})
  for r,a,b in zip(radius,av,values):profiles.append({'z':z,'height_m':(z+.5)*DX,'quantity':key,'r_m':float(r),'r251_mean':float(a),'FDS_mean':float(b)})
  ax=axes[row,col];ax.plot(radius,av,label='RISE r251');ax.plot(radius,values,label='FDS LES r261');ax.set(xlabel='Radius (m)',ylabel='Temperature rise (K)' if key=='theta' else 'Vertical velocity (m/s)',title=f'z={(z+.5)*DX:.3f} m');ax.legend();ax.grid(alpha=.25)
 faces=[]
 for k in [z,z+1]:
  label,limits,tm,m=read_slice(RUN/f'r261_fds_1_{k}.sf');assert label=='MASS FLUX Z' and limits[-1]==k
  faces.append(m[:,1:,1:])
 # Average native face mass fluxes before positive clipping, at the common cell plane.
 fm=.5*(faces[0]+faces[1]);rise=flux[z]
 mass.append({'z':z,'height_m':(z+.5)*DX,'r251_up_r08_kg_s':float(rise['mup_r4']),'FDS_up_r08_kg_s':float(avg(tm,np.maximum(fm,0)[:,mask].sum(axis=1)*DX**2)),'r251_up_whole_plane_kg_s':float(rise['mup_plane']),'FDS_up_whole_plane_kg_s':float(avg(tm,np.maximum(fm,0).sum(axis=(1,2))*DX**2)),'FDS_rho_cell_centered_w_up_r08_kg_s':float(avg(t,np.maximum(common_flux,0)[:,mask].sum(axis=1)*DX**2)),'FDS_rho_cell_centered_w_net_r08_kg_s':float(avg(t,common_flux[:,mask].sum(axis=1)*DX**2)),'r251_net_r08_kg_s':float(rise['m_r4']),'FDS_net_r08_kg_s':float(avg(tm,fm[:,mask].sum(axis=1)*DX**2))})
write('radial_profiles.csv',profiles);write('halfwidths.csv',widths);write('near_flame_mass.csv',mass);fig.savefig(OUT/'radial_profiles.png',dpi=150)
dev=csv_data(RUN/'r261_fds_devc.csv');a=np.genfromtxt(BASE/'r259_inlet_forcing/r251_planes.csv',names=True,delimiter=',');times=a['time_s'].reshape(235,106)[:,0];heat=a['slab_heat_W'].reshape(235,106);heights=[]
for z in range(33):
 heights.append({'z':z,'height_m':(z+.5)*DX,'r251_mean_heat_kW_m':float(avg(times,heat[:,z])/1000/DX),'FDS_mean_heat_kW_m':float(avg(dev['Time'],dev[f'QZ{z:03}'])/DX),'fraction_of_layer_below_1m':float(np.clip((1-z*DX)/DX,0,1))})
write('heat_height.csv',heights)
fig,ax=plt.subplots(figsize=(6,5),layout='constrained')
for key,label in [('r251_mean_heat_kW_m','RISE r251'),('FDS_mean_heat_kW_m','FDS LES r261')]:ax.plot([v[key] for v in heights],[v['height_m'] for v in heights],label=label)
ax.set(xlabel='Mean heat release (kW/m)',ylabel='Height (m)');ax.legend();ax.grid(alpha=.25);fig.savefig(OUT/'heat_height.png',dpi=150)
(OUT/'near_flame.json').write_text(json.dumps({'statistics_start_s':START,'statistics_end_s':END,'mass':mass,'halfwidths':widths,'heat_below_1m_kW':{k:sum(v[k]*DX*v['fraction_of_layer_below_1m'] for v in heights) for k in ['r251_mean_heat_kW_m','FDS_mean_heat_kW_m']},'conventions':'same native cell heights; annular means with width dx; half-width first outward crossing of half center value. RISE mass from retained checkpoint quadrature; radial profiles/HRR from235 retained frames. FDS mass centered adjacent native mass-flux faces, positive part after centering; T converted Celsius to Kelvin; W adjacent native faces centered. Statistics quadrature uses actual timestamps.'},indent=2)+'\n')
print((OUT/'near_flame.json').read_text())
