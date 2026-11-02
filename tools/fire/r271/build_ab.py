#!/usr/bin/env python3
"""Build the r271 resume driver against the committed production objects.

Only an external copy of the test harness is patched (r260 pattern): lift the
64-step cap on retained-production probes and stop at a physical window set by
RISE_R271_WINDOW_S. No production source or object is rewritten.
"""
from pathlib import Path
import subprocess,shlex,hashlib,json,sys
ROOT=Path(__file__).resolve().parents[3];OUT=Path('/Volumes/SSD/RISE/fire_production_calibration/r271_compact_stencil')/(sys.argv[1] if len(sys.argv)>1 else 'ab_build');OUT.mkdir(parents=True,exist_ok=True)
h=(ROOT/'tests/FireSequenceTest.cpp').read_text()
old='persistence.stopAfterAdditionalAcceptedSteps<=64u&&';assert h.count(old)==1;h=h.replace(old,'persistence.stopAfterAdditionalAcceptedSteps<=8192u&&')
old='\t\tconst char* solverPhase="initialization";';assert h.count(old)==1
h=h.replace(old,old+'\n\t\tconst double r271EndTimeS=simulationTimeS+std::stod(std::getenv("RISE_R271_WINDOW_S"));')
old='(acceptedSteps<effectiveMinimumStepCount||simulationTimeS<targetTimeS)) {';assert h.count(old)==1
h=h.replace(old,'(acceptedSteps<effectiveMinimumStepCount||simulationTimeS<targetTimeS)&&\n\t\t\tsimulationTimeS<r271EndTimeS) {')
harness=OUT/'Harness.cpp';harness.write_text(h)
make=ROOT/'build/make/rise';cmds=subprocess.check_output(['make','-C',str(make),'-n','-B','../../../bin/tests/FireSequenceTest'],text=True).splitlines()
compile_=next(x for x in cmds if x.startswith('c++ ')and' -c 'in x and'tests/FireSequenceTest.cpp'in x)
link=next(x for x in cmds if x.startswith('c++ ')and' -o ../../../bin/tests/FireSequenceTest 'in x)
obj=OUT/'ab_driver.o';drv=OUT/'ab_driver'
compile_=compile_.replace('../../../tests/FireSequenceTest.cpp',str(ROOT/'tools/fire/r271/ab_driver.cpp')).replace('-o ../../../tests/FireSequenceTest.o','-o '+str(obj))
compile_=compile_.replace(' -c ',f' -I{ROOT}/tests -DR271_HARNESS_SOURCE=\\"{harness}\\" -c ')
assert str(obj) in compile_,'compile output path not replaced'
link=link.replace(' -o ../../../bin/tests/FireSequenceTest ',' -o '+str(drv)+' ').replace(' ../../../tests/FireSequenceTest.o ',' '+str(obj)+' ')
assert str(obj) in link
(OUT/'build.sh').write_text('set -e\ncd '+shlex.quote(str(make))+'\n'+compile_+'\n'+link+'\n')
with (OUT/'build.log').open('w') as log:subprocess.run(['sh',str(OUT/'build.sh')],stdout=log,stderr=subprocess.STDOUT,check=True)
sha=lambda p:hashlib.sha256(p.read_bytes()).hexdigest()
(OUT/'build_manifest.json').write_text(json.dumps({'production_changed':False,'git_head':subprocess.check_output(['git','-C',str(ROOT),'rev-parse','HEAD'],text=True).strip(),
 'harness_changes':['retained-production probe cap 64 -> 8192 steps','stop at first accepted step reaching start + RISE_R271_WINDOW_S'],
 'sha256':{str(p):sha(p) for p in [ROOT/'tools/fire/r271/ab_driver.cpp',harness,drv]}},indent=2)+'\n')
print(drv)
