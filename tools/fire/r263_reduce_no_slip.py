#!/usr/bin/env python3
"""Upward and net mass flux at five shared native planes, retained fields only."""
from pathlib import Path
import csv,json
import numpy as np
from r261_reduce_full import read_slice,DX,START,END,average
ROOT=Path(__file__).resolve().parents[2];BASE=Path('/Volumes/SSD/RISE/fire_production_calibration');OUT=ROOT/'docs/data/fire_r263'
def main():
 runs={'default_wall_FDS':(BASE/'r261_fds_full/fds_run','r261_fds'),'no_slip_FDS':(BASE/'r263_fds_no_slip/fds_run','r263_fds')}
 rise=list(csv.DictReader((ROOT/'docs/data/fire_r253/mean_height_profile.csv').open()));xy=(np.arange(69)-34)*DX;mask=xy[:,None]**2+xy[None,:]**2<.64
 rows=[]
 for z in [9,16,32,65,98]:
  row={'z':z,'height_m':(z+.5)*DX,'r251_up_r08_kg_s':float(rise[z]['mup_r4']),'r251_up_plane_kg_s':float(rise[z]['mup_plane']),'r251_net_r08_kg_s':float(rise[z]['m_r4'])}
  for label,(run,prefix) in runs.items():
   native=run/(prefix+'.out');assert 'FDS completed successfully' in native.read_text(errors='replace')
   arrays=[]
   for k in [z,z+1]:
    name,limits,t,a=read_slice(run/f'{prefix}_1_{k}.sf');assert name=='MASS FLUX Z' and limits[-1]==k
    if arrays:assert np.array_equal(t,previous)
    arrays.append(a[:,1:,1:]);previous=t
   m=.5*(arrays[0]+arrays[1]);positive=np.maximum(m,0)
   row[label+'_up_r08_kg_s']=average(t,positive[:,mask].sum(axis=1,dtype=np.float64)*DX**2)
   row[label+'_up_plane_kg_s']=average(t,positive.sum(axis=(1,2),dtype=np.float64)*DX**2)
   row[label+'_net_r08_kg_s']=average(t,m[:,mask].sum(axis=1,dtype=np.float64)*DX**2)
  a=row['default_wall_FDS_up_r08_kg_s'];b=row['no_slip_FDS_up_r08_kg_s'];r=row['r251_up_r08_kg_s']
  row['no_slip_fractional_change_vs_default']=b/a-1;row['fraction_of_default_to_RISE_gap_closed']=(a-b)/(a-r)
  rows.append(row)
 with (OUT/'upward_mass_comparison.csv').open('w') as f:
  w=csv.DictWriter(f,fieldnames=list(rows[0]),lineterminator='\n');w.writeheader();w.writerows(rows)
 result={'statistics_start_s':START,'statistics_end_s':END,'rows':rows,'definition':'positive part after adjacent native FDS mass-face centering; integrate r<0.8m and whole plane; same r251 checkpoint quadrature and coordinates as r253/r262. Upward and net flux are separate.','decision':'stop for owner review; interpret measured base-gap movement before proposing any RISE wall function; no production change'}
 (OUT/'results.json').write_text(json.dumps(result,indent=2)+'\n');print(json.dumps(result,indent=2))
if __name__=='__main__':main()
