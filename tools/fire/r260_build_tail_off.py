#!/usr/bin/env python3
"""Owner-approved external R0/R1 EOS-tail-off experiment; retain physical projections."""
from pathlib import Path
import subprocess,difflib,json,hashlib,shlex
ROOT=Path(__file__).resolve().parents[2];BASE=Path('/Volumes/SSD/RISE/fire_production_calibration');OUT=BASE/'r260_components'
p=ROOT/'src/Library/Utilities/FireProductionAdvectionMac.mm';original=p.read_text();s=original
old='CorrectTargetAuthority(command,stage,projectionTarget,applyTailCorrection,';assert s.count(old)==1
s=s.replace(old,'CorrectTargetAuthority(command,stage,projectionTarget,false,')
old='const bool applyTailCorrection,id<MTLBuffer> selectedAlpha,';assert s.count(old)==1
s=s.replace(old,'const bool /*applyTailCorrection: disabled in this experiment*/,id<MTLBuffer> selectedAlpha,')
source=OUT/'TailOffAdvection.mm';source.write_text(s)
(ROOT/'docs/data/fire_r260/eos_tail_off.patch').write_text(''.join(difflib.unified_diff(original.splitlines(True),s.splitlines(True),fromfile='production',tofile='external_EOS_tail_off')))
lines=(OUT/'build.sh').read_text().splitlines()
compile=next(x for x in lines if x.startswith('c++ ') and 'CenteredAdvection.mm' in x)
compile=compile.replace('CenteredAdvection.mm','TailOffAdvection.mm').replace('CenteredAdvection.o','TailOffAdvection.o')
link=next(x for x in lines if x.startswith('c++ ') and ' -o '+str(OUT/'centered_driver')+' ' in x)
link=link.replace(str(OUT/'centered_driver'),str(OUT/'tail_off_driver')).replace('CenteredAdvection.o','TailOffAdvection.o')
script='set -e\ncd '+shlex.quote(str(ROOT/'build/make/rise'))+'\n'+compile+'\n'+link+'\n';(OUT/'build_tail.sh').write_text(script)
with (OUT/'build_tail.log').open('w') as log:subprocess.run(['sh',str(OUT/'build_tail.sh')],stdout=log,stderr=subprocess.STDOUT,check=True)
(ROOT/'docs/data/fire_r260/eos_tail_build.json').write_text(json.dumps({'owner_choice':'Test EOS-tail correction off','change':'CorrectTargetAuthority correction=false inR0/R1; R2alreadyfalse. All physical projections retained.','production_change':False,'sha256':{str(q):hashlib.sha256(q.read_bytes()).hexdigest() for q in [p,source,OUT/'tail_off_driver',OUT/'observer.o']}},indent=2)+'\n')
