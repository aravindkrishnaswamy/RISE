#!/usr/bin/env python3
"""Finish one bounded serial diagnostic campaign, then stop for owner review."""
from pathlib import Path
import os,sys,time,subprocess,json,csv,datetime,shutil
import numpy as np
from r260_reduce_components import mean
ROOT=Path(__file__).resolve().parents[2];BASE=Path('/Volumes/SSD/RISE/fire_production_calibration');WORK=BASE/'r262_combined';OUT=ROOT/'docs/data/fire_r262'
def status(**data):
 data['updated_utc']=datetime.datetime.now(datetime.timezone.utc).isoformat();p=WORK/'campaign_status.json';tmp=p.with_suffix('.tmp');tmp.write_text(json.dumps(data,indent=2)+'\n');tmp.replace(p)
def alive(pid):
 p=subprocess.run(['ps','-p',str(pid),'-o','stat='],capture_output=True,text=True)
 return p.returncode==0 and not p.stdout.strip().startswith('Z')
def receipt(path):return dict(l.split(' ',1) for l in path.read_text().splitlines() if ' ' in l)
try:
 pid=json.loads((WORK/'status.json').read_text())['pid'];deadline=time.monotonic()+8*3600
 status(stage='waiting_for_combined',pid=pid)
 while alive(pid):
  if time.monotonic()>deadline:raise RuntimeError('combined exceeded bounded wait; inspect existing process, no onset launched')
  time.sleep(20)
 result=receipt(WORK/'combined/result.txt')
 if result['succeeded']!='1' or float(result['end_time_s'])<19.018720638239756:raise RuntimeError('combined did not complete four seconds; no onset selection made')
 early=list(csv.DictReader((WORK/'combined/early_fields.csv').open()))
 assert {int(r['accepted_step']) for r in early}=={1,2} and any(int(r['different_numeric_values'])>0 for r in early)
 shutil.copyfile(WORK/'combined/early_fields.csv',OUT/'combined_early_fields.csv')
 data=np.genfromtxt(WORK/'combined/mass_timeseries.csv',names=True,delimiter=',');m=[mean(data,z) for z in [32,65,98]]
 rows=json.loads((ROOT/'docs/data/fire_r260/results.json').read_text())['rows'];a=rows[0]['m3_kg_s']
 combined={'variant':'CHARM + centered momentum','status':'complete; two-step check passed','window_s':4.,'m1_kg_s':m[0],'m2_kg_s':m[1],'m3_kg_s':m[2],'recovered_3m_gap_fraction':(m[2]-a)/(1.2381499657108872-a)}
 rows.insert(-1,combined)
 with (OUT/'component_comparison.csv').open('w') as f:
  w=csv.DictWriter(f,fieldnames=list(rows[0]),lineterminator='\n');w.writeheader();w.writerows(rows)
 centered=next(r for r in rows if r['variant']=='centered_momentum');candidate='combined' if m[2]>centered['m3_kg_s'] else 'centered'
 summary={'rows':rows,'candidate':candidate,'selection':'largest measured 3m mass flux among combined and centered; 1m result reported separately','onset':[],'production_change':False}
 (OUT/'results.json').write_text(json.dumps(summary,indent=2)+'\n')
 for tier,end in [(8,3.5),(10,3.)]:
  out=WORK/f'onset_tier{tier}';status(stage=f'running_onset_tier{tier}',candidate=candidate,target_s=end)
  with (WORK/f'onset_tier{tier}.log').open('w') as f:
   code=subprocess.run(['/usr/bin/caffeinate','-i',str(WORK/(candidate+'_onset')),str(tier),str(out)],cwd=ROOT,stdout=f,stderr=subprocess.STDOUT,timeout=12*3600).returncode
  entry={'tier':tier,'target_s':end,'returncode':code,'candidate':candidate}
  if (out/'result.txt').exists():entry.update(receipt(out/'result.txt'))
  trajectory=out/'budgets/maximum_velocity_trajectory.csv'
  if trajectory.exists():
   shutil.copyfile(trajectory,OUT/f'tier{tier}_velocity_trajectory.csv')
   native=list(csv.DictReader(trajectory.open()));entry['trajectory_rows']=len(native)
   if native:
    # Preserve the native header/values; derive the maximum without assumptions about its spelling.
    entry['trajectory_columns']=list(native[0])
    peak=max(native,key=lambda r:float(r['maximum_velocity_m_per_s']));entry['maximum_velocity_m_per_s']=float(peak['maximum_velocity_m_per_s']);entry['maximum_velocity_time_s']=float(peak['time_s'])
  summary['onset'].append(entry);(OUT/'results.json').write_text(json.dumps(summary,indent=2)+'\n')
  if code:raise RuntimeError(f'tier{tier} onset stopped or failed; preserve evidence, no next run')
 status(stage='stopped_for_owner_review',candidate=candidate,production_change=False)
except Exception as e:
 status(stage='stopped_error',error=str(e));raise
