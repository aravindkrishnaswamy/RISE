#!/usr/bin/env python3
"""One bounded serial campaign; no recurrence, adoption or automatic solver fixes."""
from pathlib import Path
import subprocess,json,datetime,time,os,sys,csv
ROOT=Path(__file__).resolve().parents[2];BASE=Path('/Volumes/SSD/RISE/fire_production_calibration');WORK=BASE/'r260_components';FDS=BASE/'r261_fds_full'
START=BASE/'r251_tier8_full_window/checkpoint_history/step_0000009224.checkpoint';FINAL=BASE/'r251_tier8_full_window/production_final.checkpoint'
ENV=os.environ.copy();ENV.pop('RISE_R260_HALF_STEP',None);ENV['PYTHONPATH']=str(BASE/'r257_readonly_comparison/python_packages')
def status(path,**v):
 v['updated_utc']=datetime.datetime.now(datetime.timezone.utc).isoformat();path.write_text(json.dumps(v,indent=2)+'\n')
def process(args,path,env=ENV):
 with path.open('w') as f:return subprocess.run([str(a) for a in args],cwd=ROOT,env=env,stdout=f,stderr=subprocess.STDOUT).returncode
try:
 status(WORK/'campaign_status.json',stage='waiting_for_full_output_FDS')
 deadline=time.monotonic()+4*3600
 while True:
  fs=json.loads((FDS/'run_status.json').read_text())
  if fs['state']=='complete':break
  if fs['state']=='failed' or time.monotonic()>deadline:raise RuntimeError('FDS did not complete; no concurrent RISE run launched')
  time.sleep(20)
 code=process([sys.executable,'tools/fire/r261_reduce_full.py'],FDS/'reduction.log')
 if code:raise RuntimeError('full FDS reduction failed; physical outputs retained')
 reference=WORK/'early_reference'
 code=process(['/usr/bin/caffeinate','-i',WORK/'mc_driver',START,FINAL,WORK/'baseline_two_steps','A2',reference],WORK/'baseline_two_steps.log')
 if code:raise RuntimeError('two-step MC reference failed')
 for name in ['half_dt','restoration_off','centered_momentum']:
  plan=json.loads((WORK/'plan.json').read_text())[name];out=WORK/name;out.mkdir(exist_ok=True)
  if plan.get('state')!='ready':status(out/'status.json',state=plan['state'],reason=plan.get('reason',''));continue
  env=ENV.copy();env.update(plan.get('env',{}));status(WORK/'campaign_status.json',stage='running_'+name)
  status(out/'status.json',state='running',early_check='firsttwo fullfields must differ before step3')
  code=process(['/usr/bin/caffeinate','-i',WORK/plan['binary'],START,FINAL,out,'B4',reference],WORK/(name+'.log'),env)
  if code:status(out/'status.json',state='failed_or_sanity_refused',exitcode=code);continue
  receipt=dict(l.split(' ',1) for l in (out/'result.txt').read_text().splitlines() if ' ' in l)
  state='complete' if receipt['succeeded']=='1' and float(receipt['end_time_s'])>=15.018720638239756+4 else 'incomplete'
  status(out/'status.json',state=state,end_time_s=receipt['end_time_s'])
  if process([sys.executable,'tools/fire/r260_reduce_components.py'],WORK/'reduction.log'):raise RuntimeError('component reduction failed; native outputs retained')
 if process([sys.executable,'tools/fire/r260_reduce_components.py'],WORK/'reduction.log'):raise RuntimeError('final comparison reduction failed; native outputs retained')
 status(WORK/'campaign_status.json',stage='stopped_for_owner_review_no_production_change')
except Exception as e:
 status(WORK/'campaign_status.json',stage='stopped_error',error=str(e));raise
