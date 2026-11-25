#!/usr/bin/env python3
"""Build an r273 tool against the committed production objects (r271 build_ab.py pattern; the harness copy is unpatched).
usage: build_tool.py <tool> [out dir]   (tools/fire/r273/<tool>.cpp -> <out dir, default r273_poor_burn/build>/<tool>)
A second output directory keeps a running driver's binary untouched."""
from pathlib import Path
import subprocess,shlex,sys
ROOT=Path(__file__).resolve().parents[3];NAME=sys.argv[1]
OUT=Path(sys.argv[2] if len(sys.argv)>2 else '/Volumes/SSD/RISE/fire_production_calibration/r273_poor_burn/build');OUT.mkdir(parents=True,exist_ok=True)
h=(ROOT/'tests/FireSequenceTest.cpp').read_text()
# M2 (2026-11-23): the A/B drivers (ab_driver_tier, r275/ab_partial_manifest) run the library fire driver and set the
# former harness patches (window bound, frame origin, snapshots, 8192-step probe limit) as FireRunPersistenceOptions;
# no tool patches the harness any more. Read-only tools still include the unpatched harness copy.
harness=OUT/'Harness.cpp';harness.write_text(h)
make=ROOT/'build/make/rise';cmds=subprocess.check_output(['make','-C',str(make),'-n','-B','../../../bin/tests/FireSequenceTest'],text=True).splitlines()
compile_=next(x for x in cmds if x.startswith('c++ ')and' -c 'in x and'tests/FireSequenceTest.cpp'in x)
link=next(x for x in cmds if x.startswith('c++ ')and' -o ../../../bin/tests/FireSequenceTest 'in x)
obj=OUT/(NAME+'.o');exe=OUT/NAME
SRC=next(d/(NAME+'.cpp') for d in (ROOT/'tools/fire/r273',ROOT/'tools/fire/r275') if (d/(NAME+'.cpp')).exists())  # r275 drivers live in tools/fire/r275
compile_=compile_.replace('../../../tests/FireSequenceTest.cpp',str(SRC)).replace('-o ../../../tests/FireSequenceTest.o','-o '+str(obj))
compile_=compile_.replace(' -c ',f' -I{ROOT}/tests -DR271_HARNESS_SOURCE=\\"{harness}\\" -c ')
assert str(obj) in compile_
link=link.replace(' -o ../../../bin/tests/FireSequenceTest ',' -o '+str(exe)+' ').replace(' ../../../tests/FireSequenceTest.o ',' '+str(obj)+' ')
assert str(obj) in link
(OUT/f'build_{NAME}.sh').write_text('set -e\ncd '+shlex.quote(str(make))+'\n'+compile_+'\n'+link+'\n')
with (OUT/f'build_{NAME}.log').open('w') as log:subprocess.run(['sh',str(OUT/f'build_{NAME}.sh')],stdout=log,stderr=subprocess.STDOUT,check=True)
print(exe)
