#!/usr/bin/env python3
"""Build the r271 non-reacting plume driver (inert_driver.cpp) against the committed production objects.

Only an external copy of the test harness is patched (r260 pattern); no production source or object is rewritten:
  1. stop at the first accepted step reaching start + RISE_R271_WINDOW_S (as build_ab.py / build_tg.py);
  2. anchor 16 Hz frame publication at RISE_INERT_FRAME_ORIGIN_S instead of the case's statistics start (10.47 s);
  3. optional momentum Vreman coefficient override RISE_R271_CV (as build_tg.py / build_bt.py; unset = production).
The inert physics itself is the production opt-in RISE_FIRE_INERT_PLUME=1.
"""
from pathlib import Path
import subprocess,shlex,hashlib,json,sys
ROOT=Path(__file__).resolve().parents[3];OUT=Path('/Volumes/SSD/RISE/fire_production_calibration/r271_inert')/(sys.argv[1] if len(sys.argv)>1 else 'inert_build')
OUT.mkdir(parents=True,exist_ok=True)
SRC='inert_driver'
h=(ROOT/'tests/FireSequenceTest.cpp').read_text()
old='\t\tconst char* solverPhase="initialization";';assert h.count(old)==1
h=h.replace(old,old+'\n\t\tconst double r271EndTimeS=simulationTimeS+std::stod(std::getenv("RISE_R271_WINDOW_S"));')
old='(acceptedSteps<effectiveMinimumStepCount||simulationTimeS<targetTimeS)) {';assert h.count(old)==1
h=h.replace(old,'(acceptedSteps<effectiveMinimumStepCount||simulationTimeS<targetTimeS)&&\n\t\t\tsimulationTimeS<r271EndTimeS) {')
old='\t\tdouble nextTemporalSnapshotS=values.statisticsStartS;';assert h.count(old)==1
h=h.replace(old,'\t\tconst double r271FrameOriginS=std::stod(std::getenv("RISE_INERT_FRAME_ORIGIN_S"));\n\t\tdouble nextTemporalSnapshotS=r271FrameOriginS;')
old='\t\t\t\t\tnextTemporalSnapshotS=values.statisticsStartS+';assert h.count(old)==1
h=h.replace(old,'\t\t\t\t\tnextTemporalSnapshotS=r271FrameOriginS+')
old='\t\towner.vremanCoefficient=ordinary.force.vremanCoefficient;';assert h.count(old)==1
h=h.replace(old,'\t\towner.vremanCoefficient=std::getenv("RISE_R271_CV")?std::stof(std::getenv("RISE_R271_CV")):ordinary.force.vremanCoefficient;')
harness=OUT/'Harness.cpp';harness.write_text(h)
make=ROOT/'build/make/rise';cmds=subprocess.check_output(['make','-C',str(make),'-n','-B','../../../bin/tests/FireSequenceTest'],text=True).splitlines()
compile_=next(x for x in cmds if x.startswith('c++ ')and' -c 'in x and'tests/FireSequenceTest.cpp'in x)
link=next(x for x in cmds if x.startswith('c++ ')and' -o ../../../bin/tests/FireSequenceTest 'in x)
obj=OUT/(SRC+'.o');drv=OUT/SRC
compile_=compile_.replace('../../../tests/FireSequenceTest.cpp',str(ROOT/'tools/fire/r271'/(SRC+'.cpp'))).replace('-o ../../../tests/FireSequenceTest.o','-o '+str(obj))
compile_=compile_.replace(' -c ',f' -I{ROOT}/tests -DR271_HARNESS_SOURCE=\\"{harness}\\" -c ')
assert str(obj) in compile_
link=link.replace(' -o ../../../bin/tests/FireSequenceTest ',' -o '+str(drv)+' ').replace(' ../../../tests/FireSequenceTest.o ',' '+str(obj)+' ')
assert str(obj) in link
# (The 2026-11-02 runs pinned openvdb 13.1 here while the production objects still used it; RISE now builds
# against 14.0, so the make command lines are used as they are.)
(OUT/f'build_{SRC}.sh').write_text('set -e\ncd '+shlex.quote(str(make))+'\n'+compile_+'\n'+link+'\n')
with (OUT/f'build_{SRC}.log').open('w') as log:subprocess.run(['sh',str(OUT/f'build_{SRC}.sh')],stdout=log,stderr=subprocess.STDOUT,check=True)
sha=lambda p:hashlib.sha256(p.read_bytes()).hexdigest()
(OUT/f'build_manifest_{SRC}.json').write_text(json.dumps({'production_sources_rewritten':False,'git_head':subprocess.check_output(['git','-C',str(ROOT),'rev-parse','HEAD'],text=True).strip(),
 'git_dirty':subprocess.check_output(['git','-C',str(ROOT),'status','--porcelain','--','src','tests'],text=True).strip()!='',
 'harness_changes':['stop at start + RISE_R271_WINDOW_S','frames anchored at RISE_INERT_FRAME_ORIGIN_S','Vreman override RISE_R271_CV'],
 'sha256':{str(p):sha(p) for p in [ROOT/'tools/fire/r271'/(SRC+'.cpp'),harness,drv]}},indent=2)+'\n')
print(drv)
