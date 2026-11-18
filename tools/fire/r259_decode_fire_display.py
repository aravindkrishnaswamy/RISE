#!/usr/bin/env python3
"""Audit lossy FDS fire-display storage; never call it physical plane HRR."""
from pathlib import Path
import struct,csv,json
import numpy as np
from r255_reduce_fds import record
BASE=Path('/Volumes/SSD/RISE/fire_production_calibration');OUT=Path(__file__).resolve().parents[2]/'docs/data/fire_r259';DX=.030593116067411014

def main():
 run=BASE/'r258_fds_les/fds_run';rows=[];wxy=np.ones(70);wxy[[0,-1]]=.5;area=wxy[:,None]*wxy[None,:]*DX**2
 with (run/'r258_fds_1_2.s3d').open('rb') as f:
  header=struct.unpack('<8i',record(f));assert header==(1,1,0,69,0,69,0,106)
  while True:
   tr=record(f)
   if tr is None:break
   t=struct.unpack('<f',tr)[0];n,compressed=struct.unpack('<2i',record(f));b=record(f);assert len(b)==compressed
   a=np.empty(n,dtype=np.uint8);i=j=0
   while i<len(b):
    value=b[i];i+=1
    if value==255:
     value=b[i];count=b[i+1];i+=2;a[j:j+count]=value;j+=count
    else:a[j]=value;j+=1
   assert j==n==70*70*107
   a=a.reshape((70,70,107),order='F');q=a.astype(float)*(1200000/254);planes=(q*area[:,:,None]).sum(axis=(0,1));z=np.arange(107)*DX
   top=.7;zt=np.r_[z[z<top],top];pt=np.interp(zt,z,planes)
   row={'time_s':t,'display_volume_0p7_W':float(np.trapezoid(pt,zt)),'display_volume_all_W':float(np.trapezoid(planes,z)),'saturated_nodes_0p7':int(np.sum(a[:,:,:24]==254)),'saturated_nodes_all':int(np.sum(a==254))}
   for k in [3,9,16]:row[f'display_plane_z{k}_W_m']=float(planes[k])
   rows.append(row)
 out=BASE/'r259_inlet_forcing/fds_clipped_display.csv'
 with out.open('w') as f:
  w=csv.DictWriter(f,fieldnames=list(rows[0]),lineterminator='\n');w.writeheader();w.writerows(rows)
 selected=[r for r in rows if 10.466040127212182<=r['time_s']<=25.091041]
 result={'top_code_note':'254 includes rounding at >=1197.64kW/m3; not an exact count of values strictly above1200kW/m3','source':'FDS Smoke3D HRRPUV; not physical cell HRR history','mapping':'cell HRR first averaged to nodes, then clipped to1200kW/m3, round254*q/qmax, RLE encoded','nodes':n,'frames_in_statistics':len(selected),'frames_with_top_quantization_code':sum(r['saturated_nodes_all']>0 for r in selected),'max_top_coded_nodes':max(r['saturated_nodes_all'] for r in selected),'display_domain_mean_W':float(np.mean([r['display_volume_all_W'] for r in selected])),'reason_not_a_forcing_measurement':'clipping loses unbounded peak rate information; node interpolation is not native cell-plane HRR; cannot recover physical plane RMS/PSD','display_csv_external':str(out)}
 (OUT/'fds_heat_retention_audit.json').write_text(json.dumps(result,indent=2)+'\n');print(result)
if __name__=='__main__':main()
