#!/usr/bin/env python3
"""Historical INVALID B: changed inactive fct path. Use r258_build_resident_charm.py."""
from pathlib import Path
import subprocess,shlex,difflib,hashlib,json
ROOT=Path(__file__).resolve().parents[2];BASE=Path('/Volumes/SSD/RISE/fire_production_calibration');OUT=BASE/'r257_charm_ab';OUT.mkdir(exist_ok=True)
p=ROOT/'src/Library/Utilities/FireProductionAdvectionMac.mm';original=p.read_text();s=original
old='''inline float fct_mc(float backward,float forward){if(backward*forward<=0.0f)return 0.0f;
 float centered=0.5f*(backward+forward),sign=centered<0.0f?-1.0f:1.0f;
 return sign*min(abs(centered),2.0f*min(abs(backward),abs(forward)));}'''
new='''inline float fct_charm(float backward,float forward,bool fromLeft){
 if(!((backward>0.0f&&forward>0.0f)||(backward<0.0f&&forward<0.0f)))return 0.0f;
 float up=fromLeft?backward:forward,local=fromLeft?forward:backward;
 // t=s/(1+s), s=local/up. B(s)*up = up*t*(1+2*t).
 // Same-sign sum avoids cancellation and an unbounded slope ratio.
 float t=local/(up+local);return up*t*(1.0f+2.0f*t);
}'''
replacements=[(old,new),('uint cellZ,uint axis){','uint cellZ,uint axis,bool fromLeft){'),('coordinateSlope[b]=fct_mc(backward,forward);','coordinateSlope[b]=fct_charm(backward,forward,fromLeft);'),('p,component,dx,dy,dz,axis):','p,component,dx,dy,dz,axis,fromLeft):'),('fct_mc(donor-fct_stage_value','fct_charm(donor-fct_stage_value'),('fct_stage_value(q,ambient,inflow,p,component,dx,dy,dz,axis,1)-donor);','fct_stage_value(q,ambient,inflow,p,component,dx,dy,dz,axis,1)-donor,fromLeft);')]
for old,new in replacements:
 assert s.count(old)==1,(old,s.count(old))
 s=s.replace(old,new)
assert 'fct_mc(' not in s
source=OUT/'FireProductionAdvectionMac.mm';source.write_text(s)
patch=''.join(difflib.unified_diff(original.splitlines(True),s.splitlines(True),fromfile='production/FireProductionAdvectionMac.mm',tofile='diagnostic_CHARM/FireProductionAdvectionMac.mm'))
(ROOT/'docs/data/fire_r257/charm_diagnostic.patch').write_text(patch)
cmd=subprocess.check_output(['make','-C',str(ROOT/'build/make/rise'),'-n','-B','../../../src/Library/Utilities/FireProductionAdvectionMac.o'],text=True)
compile_command=next(line for line in cmd.splitlines() if line.startswith('c++ '))
compile_command=compile_command.replace('../../../src/Library/Utilities/FireProductionAdvectionMac.mm',shlex.quote(str(source))).replace('../../../src/Library/Utilities/FireProductionAdvectionMac.o',shlex.quote(str(OUT/'FireProductionAdvectionMac.o')))
compile_command=compile_command.replace(' -c ',f' -I{ROOT}/src/Library/Utilities -c ')
link=next(line for line in (BASE/'r256_limiter_diagnostic/build_limiter.sh').read_text().splitlines() if line.startswith('c++ ') and ' -c ' not in line)
link=link.replace('../../../src/Library/Utilities/FireProductionAdvectionMac.o',shlex.quote(str(OUT/'FireProductionAdvectionMac.o')))
link=link.replace(' -o '+str(BASE/'r256_limiter_driver')+' ',' -o '+str(OUT/'charm_driver')+' ')
(OUT/'build.sh').write_text('set -e\ncd '+shlex.quote(str(ROOT/'build/make/rise'))+'\n'+compile_command+'\n'+link+'\n')
with (OUT/'build.log').open('w') as log:subprocess.run(['sh',str(OUT/'build.sh')],stdout=log,stderr=subprocess.STDOUT,check=True)
manifest={'production_source_sha256':hashlib.sha256(p.read_bytes()).hexdigest(),'diagnostic_source_sha256':hashlib.sha256(source.read_bytes()).hexdigest(),'diagnostic_binary_sha256':hashlib.sha256((OUT/'charm_driver').read_bytes()).hexdigest(),'only_kernel_change':'MC scalar reconstruction -> direction-aware CHARM, affine basis/projection unchanged','host_observer':'existing r256 observer object, no identity gate','A':'completed r256 15.01872063824–19.01872063824s'}
(ROOT/'docs/data/fire_r257/charm_build.json').write_text(json.dumps(manifest,indent=2)+'\n')
