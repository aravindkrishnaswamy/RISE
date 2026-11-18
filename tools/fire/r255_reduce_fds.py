#!/usr/bin/env python3
"""Read FDS native devices and unaveraged cell/face slices, no simulation."""
import csv,json,struct,hashlib
from pathlib import Path
import numpy as np
ROOT=Path(__file__).resolve().parents[2];BASE=Path('/Volumes/SSD/RISE/fire_production_calibration/r255_comparison');RUN=BASE/'fds_run';OUT=ROOT/'docs/data/fire_r255'
START=10.466040127212182;END=25.091040127212182;DX=.030593116067411014

def average(t,y):
 if t[0]>START or t[-1]<END-1e-6:raise ValueError('incomplete statistics window')
 tt=np.r_[START,t[(t>START)&(t<END)],END]
 return float(np.trapezoid(np.interp(tt,t,y),tt)/(END-START))
def interval_average(t,y):
 if t[0]>START or t[-1]<END-1e-6:raise ValueError('incomplete HRR window')
 edges=t.copy();edges[-1]=max(edges[-1],END)
 weights=np.maximum(0,np.minimum(edges[1:],END)-np.maximum(edges[:-1],START))
 return float(np.sum(weights*y[1:])/(END-START))
def write(name,rows):
 with (OUT/name).open('w') as f:
  w=csv.DictWriter(f,fieldnames=list(rows[0]),lineterminator='\n');w.writeheader();w.writerows(rows)
def csv_data(path):
 with path.open() as f:
  next(f);reader=csv.DictReader(f);rows=list(reader)
 return {k.strip():np.array([float(r[k]) for r in rows]) for k in reader.fieldnames}
def record(f):
 b=f.read(4)
 if not b:return None
 if len(b)!=4:raise ValueError('partial record')
 n=struct.unpack('<i',b)[0];data=f.read(n);tail=f.read(4)
 if len(data)!=n or len(tail)!=4 or struct.unpack('<i',tail)[0]!=n:raise ValueError('invalid Fortran record')
 return data

def main():
 dev=csv_data(RUN/'r255_fds_devc.csv');r251=list(csv.DictReader((ROOT/'docs/data/fire_r252/r251_centerline.csv').open()));center=[]
 for z,row in enumerate(r251):
  h=float(row['height_m']);theta=average(dev['Time'],dev[f'T{z:03}'])+273.15-300.;w=average(dev['Time'],.5*(dev[f'WF{z:03}']+dev[f'WF{z+1:03}']))
  tref=23.3*(h/33**.4)**(-5/3);wref=1.13*33**.2*(h/33**.4)**(-1/3)
  center.append({'z':z,'height_m':h,'selected':row['selected']=='True','fds_theta_K':theta,'r251_theta_K':float(row['temperature_rise_K']),'mccaffrey_theta_K':tref,'fds_temperature_relative_error':theta/tref-1,'r251_temperature_relative_error':float(row['temperature_relative_error']),'fds_w_m_s':w,'r251_w_m_s':float(row['velocity_m_per_s']),'mccaffrey_w_m_s':wref,'fds_velocity_relative_error':w/wref-1,'r251_velocity_relative_error':float(row['velocity_relative_error'])})
 write('fds_centerline.csv',center)
 # Physical cells are 1..69; row/column zero are slice padding, as in DUMP_SLICE.
 xy=(np.arange(1,70)-.5-34.5)*DX;mask=xy[:,None]**2+xy[None,:]**2<.64
 faces={};sources=[]
 for path in sorted(RUN.glob('*.sf')):
  with path.open('rb') as f:
   label=record(f).decode().strip();record(f);record(f);limits=struct.unpack('<6i',record(f));i0,i1,j0,j1,k0,k1=limits
   if label!='MASS FLUX Z' or (i0,i1,j0,j1,k1-k0)!=(0,69,0,69,0):raise ValueError('unexpected slice topology')
   times=[];m=[];up=[];plane=[]
   while True:
    t=record(f)
    if t is None:break
    time=struct.unpack('<f',t)[0];field=np.frombuffer(record(f),dtype='<f4').reshape((70,70),order='F')[1:,1:]
    times.append(time);m.append(float(field[mask].sum(dtype=np.float64)*DX**2));up.append(float(np.maximum(field,0).sum(dtype=np.float64)*DX**2));plane.append(float(field.sum(dtype=np.float64)*DX**2))
   t=np.array(times);v={'mass_r08_kg_s':average(t,m),'whole_plane_up_kg_s':average(t,up),'whole_plane_net_kg_s':average(t,plane)}
   if k0 in faces and any(abs(v[k]-faces[k0][k])>1e-12 for k in v):raise ValueError('duplicate face slices differ')
   faces[k0]=v;sources.append({'path':str(path),'face_k':k0,'sha256':hashlib.sha256(path.read_bytes()).hexdigest()})
 if set(faces)!=set(range(1,107)):raise ValueError('expected native faces1..106')
 face_rows=[{'face_k':k,'height_m':k*DX,**v} for k,v in sorted(faces.items())];write('fds_face_mass_profile.csv',face_rows)
 ref=list(csv.DictReader((ROOT/'docs/data/fire_r253/mean_height_profile.csv').open()));profile=[]
 for z,row in enumerate(ref):
  if z not in faces or z+1 not in faces:continue
  v={k:.5*(faces[z][k]+faces[z+1][k]) for k in faces[z]}
  profile.append({'z':z,'height_m':float(row['height_m']),**v,'r251_mass_r08_kg_s':float(row['m_r4']),'r251_plane_up_kg_s':float(row['mup_plane']),'heskestad_total_kg_s':float(row['correlation_total_kg_s'])})
 write('fds_mass_profile.csv',profile)
 selected=[r for r in center if r['selected']];hrr=csv_data(RUN/'r255_fds_hrr.csv')
 result={'statistics_start_s':START,'statistics_end_s':END,'fds_version':'6.11.1','source_commit':'ff928db','max_temperature_error_station':max(selected,key=lambda r:abs(r['fds_temperature_relative_error'])),'max_velocity_error_station':max(selected,key=lambda r:abs(r['fds_velocity_relative_error'])),'mean_hrr_kW':interval_average(hrr['Time'],hrr['HRR']),'mean_radiative_loss_kW':-interval_average(hrr['Time'],hrr['Q_RADI']),'selected_mass':[min(profile,key=lambda r:abs(r['height_m']-h)) for h in [1,1.5,2,2.5,3]],'selection':'same native stations selected for r251 in r252; no FDS flame-tip reselection','lowest_cell_mass_row':'z=0 unavailable: native slice system omits floor face0; not extrapolated',
  'mass_convention':'FDS face flux rho_face*w_face; adjacent z-face averages at r251 cell heights. Not identical to r251 rho_cell*mean(w_faces); no hidden equivalence claimed.'}
 (OUT/'fds_results.json').write_text(json.dumps(result,indent=2)+'\n');(OUT/'fds_slice_inputs.json').write_text(json.dumps(sources,indent=2)+'\n');print(json.dumps(result,indent=2))
if __name__=='__main__':main()
