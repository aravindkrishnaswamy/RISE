#!/usr/bin/env python3
"""Compare retained actual MAC divergence, inlet velocity and radial rim shear."""
from pathlib import Path
import csv,json
import numpy as np
from r258_reduce_fds import read_slice,write,ROOT,BASE,RUN,OUT,DX,START,END
from r255_reduce_fds import csv_data

def weights(t):
 # Slice times are binary32; align the known final event within its printed ULP.
 t=t.copy()
 if abs(t[-1]-END)<=.5*float(np.spacing(np.float32(END))):t[-1]=END
 dt=np.diff(t);lo=np.maximum(t[:-1],START);hi=np.minimum(t[1:],END);length=np.maximum(0,hi-lo);mid=.5*((lo-t[:-1])/dt+(hi-t[:-1])/dt)
 w=np.zeros(len(t));w[:-1]+=length*(1-mid);w[1:]+=length*mid
 assert abs(w.sum()-(END-START))<1e-10
 return w/(END-START)
def field(z,q):
 quantities=['TEMPERATURE','U-VELOCITY','V-VELOCITY','W-VELOCITY','DENSITY','DIVERGENCE'];zs=list(range(18))+[32,65,98]
 k=107+zs.index(z)*6+quantities.index(q)
 label,limits,t,v=read_slice(RUN/f'r258_fds_1_{k}.sf');assert label==q and limits[-1]==z+1
 return t,v.astype(float)
def shear(r,w):
 peak=int(np.argmax(w));top=w[peak];cross=[]
 for fraction in [.9,.1]:
  target=top*fraction;radius=None
  for j in range(peak,len(w)-1):
   if w[j]>=target>w[j+1]:radius=float(r[j]+(r[j+1]-r[j])*(target-w[j])/(w[j+1]-w[j]));break
  cross.append(radius)
 return {'peak_radius_m':float(r[peak]),'peak_w_m_s':float(top),'outer_r90_m':cross[0],'outer_r10_m':cross[1],'outer_delta10_90_m':cross[1]-cross[0] if None not in cross else None}
def main():
 rise=np.genfromtxt(BASE/'r258_fds_les/r251_near_base.csv',names=True,delimiter=',')
 xy=(np.arange(69)-34)*DX;radius=np.hypot(xy[:,None],xy[None,:]);mask=radius<.3;bins=np.floor(radius/DX+.5).astype(int)
 rows=[];profiles=[];widths=[]
 for z in range(3,17):
  t,u=field(z,'U-VELOCITY');tv,v=field(z,'V-VELOCITY');tw,w=field(z,'W-VELOCITY');tl,wl=field(z-1,'W-VELOCITY');td,d=field(z,'DIVERGENCE')
  for other in [tv,tw,tl,td]:np.testing.assert_array_equal(t,other)
  wt=weights(t);div=(u[:,1:,1:]-u[:,:-1,1:]+v[:,1:,1:]-v[:,1:,:-1]+w[:,1:,1:]-wl[:,1:,1:])/DX
  mean_w=np.tensordot(wt,.5*(w[:,1:,1:]+wl[:,1:,1:]),axes=1)
  # RISE CSV enumerates y outer then x; transpose so both arrays are [x,y].
  p=rise[rise['z']==z];assert len(p)==69*69
  rw=p['mean_w'].reshape(69,69).T;rd=p['mean_divergence'].reshape(69,69).T;ra=p['mean_abs_divergence'].reshape(69,69).T;rr=p['mean_divergence_squared'].reshape(69,69).T
  rows.append({'z':z,'height_m':(z+.5)*DX,'mask':'r<0.30m','RISE_mean_div_s_1':float(rd[mask].mean()),'FDS_mean_div_s_1':float(np.dot(wt,div[:,mask].mean(axis=1))),'RISE_mean_abs_div_s_1':float(ra[mask].mean()),'FDS_mean_abs_div_s_1':float(np.dot(wt,np.abs(div[:,mask]).mean(axis=1))),'RISE_rms_div_s_1':float(np.sqrt(rr[mask].mean())),'FDS_rms_div_s_1':float(np.sqrt(np.dot(wt,(div[:,mask]**2).mean(axis=1)))),'FDS_target_D_mean_abs_s_1':float(np.dot(wt,np.abs(d[:,1:,1:][:,mask]).mean(axis=1)))})
  radial=[]
  for ring in range(20):
   m=bins==ring
   radial.append({'z':z,'height_m':(z+.5)*DX,'r_m':float(radius[m].mean()),'RISE_mean_w_m_s':float(rw[m].mean()),'FDS_mean_w_m_s':float(mean_w[m].mean())})
  profiles.extend(radial)
  r=np.array([q['r_m'] for q in radial])
  for key in ['RISE','FDS']:widths.append({'z':z,'height_m':(z+.5)*DX,'code':key,**shear(r,np.array([q[key+'_mean_w_m_s'] for q in radial])),'definition':'first outward 90%-to-10% of radial peak mean w, zero reference; resolution dx=0.030593m'})
 write('near_base_divergence.csv',rows);write('near_base_radial_w.csv',profiles);write('rim_shear_thickness.csv',widths)
 dev=csv_data(RUN/'r258_fds_devc.csv');dev['Time'][-1]=END;wt=weights(dev['Time']);inlet=np.genfromtxt(BASE/'r258_fds_les/r251_inlet.csv',names=True,delimiter=',');data=[]
 metadata=(BASE/'r255_comparison/case_metadata.txt').read_text().splitlines()[3:]
 for k,line in enumerate(metadata):
  _,x,y,p=line.split();x=int(x);y=int(y);a=inlet[(inlet['x']==x)&(inlet['y']==y)][0]
  data.append({'burner_face':k,'x':x,'y':y,'r_m':float(a['r_m']),'RISE_inflow_w_m_s':float(a['mean_w']),'FDS_inflow_w_m_s':float(np.dot(wt,dev[f'BURNW{k:03}']))})
 write('burner_inflow_profile.csv',data)
 (OUT/'near_base_results.json').write_text(json.dumps({'statistics_start_s':START,'statistics_end_s':END,'dx_m':DX,'actual_divergence':'native MAC differences, distinct from FDS thermodynamic D target','shear_definition':'first outward crossings at 90% and10% of peak radial mean w; resolution-limited and not an instantaneous material-layer thickness','divergence':rows,'shear':widths,'burner_w_means':{key:float(np.mean([r[key+'_inflow_w_m_s'] for r in data])) for key in ['RISE','FDS']}},indent=2)+'\n')
if __name__=='__main__':main()
