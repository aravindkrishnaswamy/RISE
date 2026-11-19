#!/usr/bin/env python3
import argparse,datetime,hashlib,json,os,subprocess
from pathlib import Path
parser=argparse.ArgumentParser(description='Sequential synthetic native-owner allocation controls, not a campaign simulation')
parser.add_argument('--binary',type=Path,required=True)
parser.add_argument('--output',type=Path,required=True)
args=parser.parse_args()
root=args.output.resolve();root.mkdir()
binary=args.binary.resolve()
digest=hashlib.sha256(binary.read_bytes()).hexdigest()
def write(name,obj):
 p=root/name;t=p.with_suffix(p.suffix+'.tmp');t.write_text(json.dumps(obj,indent=2)+'\n');t.replace(p)
def now():return datetime.datetime.now(datetime.timezone.utc).isoformat()
write('pid.json',{'wrapper_pid':os.getpid(),'started_at':now(),'binary_sha256':digest,'scope':'synthetic native operator fixture only; no campaign run'})
stages=[('trace',['--fire-r246-owner-allocation-fixture','trace',str(root/'trace')]),('short',['--fire-r246-owner-allocation-fixture','short',str(root/'short')]),('off',['--fire-r246-owner-allocation-fixture','off',str(root/'off')]),('on',['--fire-r246-owner-allocation-fixture','on',str(root/'on')]),('native_gate',['--fire-r246-launcher-foundation-gate']),('legacy_gate',['--fire-r244-domain-study-gate'])]
results=[];code=0
try:
 for name,args in stages:
  if hashlib.sha256(binary.read_bytes()).hexdigest()!=digest:raise RuntimeError('frozen binary changed')
  with (root/(name+'.log')).open('x') as log:
   child=subprocess.Popen([str(binary)]+args,stdout=log,stderr=subprocess.STDOUT)
   write('current_stage.json',{'stage':name,'pid':child.pid,'started_at':now()})
   rc=child.wait()
  results.append({'stage':name,'exit_code':rc,'log_sha256':hashlib.sha256((root/(name+'.log')).read_bytes()).hexdigest()});write('results.json',results)
  if rc:code=rc;break
 if code==0:
  def fields(mode):return dict(line.split(' ',1) for line in (root/mode/'fixture.txt').read_text().splitlines())
  a,b=fields('off'),fields('on')
  if fields('trace')['trace_optin_refused_before_fields']!='true' or fields('short')['short_certificate_refused_before_fields']!='true':raise RuntimeError('admission RED failed')
  keys=['artifact_fidelity','accepted_campaign_steps','full_window_simulation_evidence','case_id','native_shape','source_begin_time_s','source_dt_s','source_zero','source_packet_identity','source_beginning_state_identity','synthetic_input_sha256','native_result_sha256','input_root','publication_root','owner_identity']
  if any(a[k]!=b[k] for k in keys):raise RuntimeError('observation changes native fields or identities')
  if a['passed']!='true' or b['passed']!='true' or a['budget_payload_bytes']!='0' or b['budget_seals_verified']!='true' or b['budget_operands_and_source_binding_verified']!='true':raise RuntimeError('fixture acceptance/budget mismatch')
  write('observation_equivalence.json',{'passed':True,'keys':keys,'off_receipt_sha256':hashlib.sha256((root/'off/fixture.txt').read_bytes()).hexdigest(),'on_receipt_sha256':hashlib.sha256((root/'on/fixture.txt').read_bytes()).hexdigest(),'scope':'synthetic native operator only, not campaign qualification'})
except Exception as error:
 code=99;write('failure.json',{'error':str(error)})
write('completion.json',{'exit_code':code,'finished_at':now(),'stages':results,'scope':'synthetic native operator fixture; fresh implementation reviews still required'})
