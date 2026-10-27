#!/usr/bin/env python3
"""Measured four-second mass balance table from already-finished component runs."""
from pathlib import Path
import csv,json
import numpy as np
ROOT=Path(__file__).resolve().parents[2];BASE=Path('/Volumes/SSD/RISE/fire_production_calibration');WORK=BASE/'r260_components';OUT=ROOT/'docs/data/fire_r260'
START=15.018720638239756;END=START+4.
def mean(data,z):
 p=data[data['z']==z];t=p['time_s'];assert t[0]<=START and t[-1]>=END
 tt=np.r_[START,t[(t>START)&(t<END)],END];return float(np.trapezoid(np.interp(tt,t,p['mass_r08_kg_s']),tt)/4.)
def main():
 baseline=np.genfromtxt(BASE/'r256_limiter_diagnostic/run/mass_timeseries.csv',names=True,delimiter=',');rows=[]
 a=[mean(baseline,z) for z in [32,65,98]]
 rows.append({'variant':'MC baseline A','status':'complete','window_s':4.,'m1_kg_s':a[0],'m2_kg_s':a[1],'m3_kg_s':a[2],'recovered_3m_gap_fraction':0.})
 c=list(csv.DictReader((ROOT/'docs/data/fire_r258/charm_ab.csv').open()))
 rows.append({'variant':'CHARM','status':'complete; original common window3.981891s','window_s':float(c[0]['common_end_s'])-float(c[0]['common_start_s']),'m1_kg_s':float(c[0]['B_CHARM_kg_s']),'m2_kg_s':float(c[1]['B_CHARM_kg_s']),'m3_kg_s':float(c[2]['B_CHARM_kg_s']),'recovered_3m_gap_fraction':(float(c[2]['B_CHARM_kg_s'])-a[2])/(1.2381499657108872-a[2])})
 for name in ['half_dt','restoration_off','centered_momentum']:
  row={'variant':name,'status':'pending','window_s':None,'m1_kg_s':None,'m2_kg_s':None,'m3_kg_s':None,'recovered_3m_gap_fraction':None}
  path=WORK/name
  if (path/'status.json').exists():row['status']=json.loads((path/'status.json').read_text())['state']
  if (path/'early_fields.csv').exists():
   evidence=list(csv.DictReader((path/'early_fields.csv').open()))
   if evidence:
    with (OUT/(name+'_early_fields.csv')).open('w') as f:
     w=csv.DictWriter(f,fieldnames=list(evidence[0]),lineterminator='\n');w.writeheader();w.writerows(evidence)
  if row['status']=='complete':
   receipt=dict(line.split(' ',1) for line in (path/'result.txt').read_text().splitlines() if ' ' in line);assert receipt['succeeded']=='1'
   early=list(csv.DictReader((path/'early_fields.csv').open()));assert any(int(r['different_numeric_values']) for r in early)
   data=np.genfromtxt(path/'mass_timeseries.csv',names=True,delimiter=',');m=[mean(data,z) for z in [32,65,98]]
   row.update(window_s=4.,m1_kg_s=m[0],m2_kg_s=m[1],m3_kg_s=m[2],recovered_3m_gap_fraction=(m[2]-a[2])/(1.2381499657108872-a[2]))
   unique=data[data['z']==32];positive=unique['dt_s'][unique['dt_s']>0];row['status']+='; mean dt='+format(float(positive.mean()),'.8g')
   with (OUT/(name+'_early_fields.csv')).open('w') as f:
    w=csv.DictWriter(f,fieldnames=list(early[0]),lineterminator='\n');w.writeheader();w.writerows(early)
  rows.append(row)
 rows.append({'variant':'LES FDS reference (r258)','status':'common15.018721–19.000612s','window_s':3.981891074392479,'m1_kg_s':.30253307319452427,'m2_kg_s':.701386878509548,'m3_kg_s':1.2381499657108872,'recovered_3m_gap_fraction':1.})
 with (OUT/'component_comparison.csv').open('w') as f:
  w=csv.DictWriter(f,fieldnames=list(rows[0]),lineterminator='\n');w.writeheader();w.writerows(rows)
 (OUT/'results.json').write_text(json.dumps({'window_start_s':START,'window_end_s':END,'rows':rows,'decision':'owner review required; no production change or from-zero validation launched'},indent=2)+'\n');print(json.dumps(rows,indent=2))
if __name__=='__main__':main()
