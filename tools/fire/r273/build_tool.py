#!/usr/bin/env python3
"""Build an r273 read-only tool against the committed production objects (r271 build_ab.py pattern; the harness copy is unpatched except for ab_* tools).
usage: build_tool.py <tool>   (tools/fire/r273/<tool>.cpp -> r273_poor_burn/build/<tool>)"""
from pathlib import Path
import subprocess,shlex,sys
ROOT=Path(__file__).resolve().parents[3];NAME=sys.argv[1]
OUT=Path('/Volumes/SSD/RISE/fire_production_calibration/r273_poor_burn/build');OUT.mkdir(parents=True,exist_ok=True)
h=(ROOT/'tests/FireSequenceTest.cpp').read_text()
if NAME.startswith('ab_'):
    # r271 build_ab.py harness patches: lift the 64-step probe cap, stop at start + RISE_R271_WINDOW_S.
    old='persistence.stopAfterAdditionalAcceptedSteps<=64u&&';assert h.count(old)==1;h=h.replace(old,'persistence.stopAfterAdditionalAcceptedSteps<=8192u&&')
    old='\t\tconst char* solverPhase="initialization";';assert h.count(old)==1
    h=h.replace(old,old+'\n\t\tconst double r271EndTimeS=simulationTimeS+std::stod(std::getenv("RISE_R271_WINDOW_S"));')
    old='(acceptedSteps<effectiveMinimumStepCount||simulationTimeS<targetTimeS)) {';assert h.count(old)==1
    h=h.replace(old,'(acceptedSteps<effectiveMinimumStepCount||simulationTimeS<targetTimeS)&&\n\t\t\tsimulationTimeS<r271EndTimeS) {')
    # r271 build_inert.py patch: 16 Hz frames anchored at RISE_R273_FRAME_ORIGIN_S (absolute, later than the start time)
    # instead of the statistics start, so a restart before the statistics window still writes frames.
    old='\t\tdouble nextTemporalSnapshotS=values.statisticsStartS;';assert h.count(old)==1
    h=h.replace(old,'\t\tconst double r271FrameOriginS=std::stod(std::getenv("RISE_R273_FRAME_ORIGIN_S"));\n\t\tdouble nextTemporalSnapshotS=r271FrameOriginS;')
    old='\t\t\t\t\tnextTemporalSnapshotS=values.statisticsStartS+';assert h.count(old)==1
    h=h.replace(old,'\t\t\t\t\tnextTemporalSnapshotS=r271FrameOriginS+')
harness=OUT/('HarnessAB.cpp' if NAME.startswith('ab_') else 'Harness.cpp');harness.write_text(h)
make=ROOT/'build/make/rise';cmds=subprocess.check_output(['make','-C',str(make),'-n','-B','../../../bin/tests/FireSequenceTest'],text=True).splitlines()
compile_=next(x for x in cmds if x.startswith('c++ ')and' -c 'in x and'tests/FireSequenceTest.cpp'in x)
link=next(x for x in cmds if x.startswith('c++ ')and' -o ../../../bin/tests/FireSequenceTest 'in x)
obj=OUT/(NAME+'.o');exe=OUT/NAME
compile_=compile_.replace('../../../tests/FireSequenceTest.cpp',str(ROOT/'tools/fire/r273'/(NAME+'.cpp'))).replace('-o ../../../tests/FireSequenceTest.o','-o '+str(obj))
compile_=compile_.replace(' -c ',f' -I{ROOT}/tests -DR271_HARNESS_SOURCE=\\"{harness}\\" -c ')
assert str(obj) in compile_
link=link.replace(' -o ../../../bin/tests/FireSequenceTest ',' -o '+str(exe)+' ').replace(' ../../../tests/FireSequenceTest.o ',' '+str(obj)+' ')
assert str(obj) in link
(OUT/f'build_{NAME}.sh').write_text('set -e\ncd '+shlex.quote(str(make))+'\n'+compile_+'\n'+link+'\n')
with (OUT/f'build_{NAME}.log').open('w') as log:subprocess.run(['sh',str(OUT/f'build_{NAME}.sh')],stdout=log,stderr=subprocess.STDOUT,check=True)
print(exe)
