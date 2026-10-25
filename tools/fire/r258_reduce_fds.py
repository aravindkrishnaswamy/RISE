#!/usr/bin/env python3
"""Reduce only completed LES FDS histories; common definitions with r255/r257."""
from pathlib import Path
import csv,json,struct
import numpy as np
from r255_reduce_fds import record,csv_data
from r257_compare_retained import welch
ROOT=Path(__file__).resolve().parents[2];BASE=Path('/Volumes/SSD/RISE/fire_production_calibration');RUN=BASE/'r258_fds_les/fds_run';OUT=ROOT/'docs/data/fire_r258'
DX=.030593116067411014;START=10.466040127212182;END=25.091040127212182

def write(name,rows):
 with (OUT/name).open('w') as f:
  w=csv.DictWriter(f,fieldnames=list(rows[0]),lineterminator='\n');w.writeheader();w.writerows(rows)
def average(t,y):
 if t[0]>START or t[-1]<END-.5*float(np.spacing(np.float32(END))):raise ValueError('incomplete statistics interval')
 tt=np.r_[START,t[(t>START)&(t<END)],END];return float(np.trapezoid(np.interp(tt,t,y),tt)/(END-START))
def read_slice(path):
 with path.open('rb') as f:
  label=record(f).decode().strip();record(f);record(f);limits=struct.unpack('<6i',record(f));times=[];fields=[]
  while True:
   t=record(f)
   if t is None:break
   times.append(struct.unpack('<f',t)[0]);fields.append(np.frombuffer(record(f),dtype='<f4').reshape((70,70),order='F').copy())
 return label,limits,np.array(times),np.array(fields)
def main():
 log=(BASE/'r258_fds_les/run.log').read_text()
 # Completion state lives in FDS's output too, because stdout may be buffered.
 if 'FDS completed successfully' not in log:raise ValueError('FDS completion not yet confirmed')
 xy=(np.arange(1,70)-.5-34.5)*DX;mask=xy[:,None]**2+xy[None,:]**2<.64
 faces={}
 for k in [32,33,65,66,98,99]:
  label,limits,t,q=read_slice(RUN/f'r258_fds_1_{k}.sf');assert label=='MASS FLUX Z' and limits[-1]==k
  faces[k]=average(t,q[:,1:,1:][:,mask].sum(axis=1,dtype=np.float64)*DX**2)
 old=list(csv.DictReader((ROOT/'docs/data/fire_r255/fds_mass_profile.csv').open()));rise=list(csv.DictReader((ROOT/'docs/data/fire_r253/mean_height_profile.csv').open()));mass=[]
 for z in [32,65,98]:
  earlier=next(r for r in old if int(r['z'])==z)
  mass.append({'z':z,'height_m':(z+.5)*DX,'r251_kg_s':float(rise[z]['m_r4']),'earlier_FDS_VLES_Superbee_kg_s':float(earlier['mass_r08_kg_s']),'new_FDS_LES_CHARM_kg_s':.5*(faces[z]+faces[z+1]),'Heskestad_26p4kW_kg_s':float(rise[z]['correlation_total_kg_s'])})
 write('entrainment_comparison.csv',mass)
 dev=csv_data(RUN/'r258_fds_devc.csv');time=START+np.arange(235)/16
 assert abs(dev['Time'][-1]-END)<=.5e-6;dev['Time'][-1]=END
 signals=[('theta',np.interp(time,dev['Time'],dev['T032']+273.15-300.)),('w',np.interp(time,dev['Time'],.5*(dev['WF032']+dev['WF033'])))]
 previous=list(csv.DictReader((ROOT/'docs/data/fire_r257/spectra.csv').open()));power=[];spectra=[]
 for key,signal in signals:
  f,p=welch(signal,fs=16,window='hann',nperseg=64,noverlap=32,detrend='linear',scaling='density');selected=(f>=1)&(f<3)
  refs=[r for r in previous if int(r['z'])==32 and r['quantity']==key];assert len(refs)==len(f)
  power.append({'quantity':key,'height_m':32.5*DX,'band_Hz':'1 <= f < 3','r251_power':sum(float(r['r251_PSD'])*.25 for r in refs if 1<=float(r['frequency_Hz'])<3),'earlier_FDS_VLES_Superbee_power':sum(float(r['fds_PSD'])*.25 for r in refs if 1<=float(r['frequency_Hz'])<3),'new_FDS_LES_CHARM_power':float(p[selected].sum()*.25),'units':'K^2' if key=='theta' else 'm^2/s^2'})
  for freq,v,ref in zip(f,p,refs):spectra.append({'quantity':key,'frequency_Hz':float(freq),'new_FDS_LES_CHARM_PSD':float(v),'earlier_FDS_VLES_Superbee_PSD':float(ref['fds_PSD']),'r251_PSD':float(ref['r251_PSD'])})
 write('spectral_power_1m.csv',power);write('spectra_1m.csv',spectra)
 (OUT/'fds_results.json').write_text(json.dumps({'statistics_start_s':START,'statistics_end_s':END,'entrainment':mass,'spectral_power_1m':power,'conditional_near_base_analysis':'pending corrected active-path CHARM B and FDS outcome; no generation/dissipation cause claimed'},indent=2)+'\n')
 print(json.dumps({'entrainment':mass,'power':power},indent=2))
if __name__=='__main__':main()
