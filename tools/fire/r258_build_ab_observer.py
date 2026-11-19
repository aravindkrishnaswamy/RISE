#!/usr/bin/env python3
"""Build paired MC/CHARM observers with first-two-step field comparison."""
from pathlib import Path
import shlex,subprocess,json,hashlib
ROOT=Path(__file__).resolve().parents[2];BASE=Path('/Volumes/SSD/RISE/fire_production_calibration');OUT=BASE/'r257_charm_ab'
lines=(BASE/'r256_limiter_diagnostic/build_limiter.sh').read_text().splitlines()
# External launcher only: permit a two-step witness instead of its old >=8 minimum.
harness=(BASE/'r256_limiter_diagnostic/FireSequenceObserverHarness.cpp').read_text()
assert harness.count('persistence.stopAfterAdditionalAcceptedSteps>=8u')==2
harness=harness.replace('persistence.stopAfterAdditionalAcceptedSteps>=8u','persistence.stopAfterAdditionalAcceptedSteps>=2u')
(OUT/'EarlyObserverHarness.cpp').write_text(harness)
compile=next(x for x in lines if x.startswith('c++ ') and ' -c ' in x)
compile=compile.replace(str(BASE/'r256_limiter_diagnostic/FireSequenceObserverHarness.cpp'),str(OUT/'EarlyObserverHarness.cpp'))
compile=compile.replace(str(ROOT/'tools/fire/r256_limiter_driver.cpp'),str(ROOT/'tools/fire/r258_ab_driver.cpp')).replace(str(BASE/'r256_limiter_driver.o'),str(OUT/'ab_observer.o'))
link=next(x for x in lines if x.startswith('c++ ') and ' -c ' not in x)
link=link.replace(str(BASE/'r256_limiter_driver.o'),str(OUT/'ab_observer.o'))
a=link.replace(' -o '+str(BASE/'r256_limiter_driver')+' ',' -o '+str(OUT/'sanity_mc_driver')+' ')
b=link.replace(' -o '+str(BASE/'r256_limiter_driver')+' ',' -o '+str(OUT/'charm_driver')+' ').replace('../../../src/Library/Utilities/FireProductionAdvectionMac.o',str(OUT/'FireProductionAdvectionMac.o'))
script='set -e\ncd '+shlex.quote(str(ROOT/'build/make/rise'))+'\n'+compile+'\n'+a+'\n'+b+'\n';(OUT/'build_ab_observer.sh').write_text(script)
with (OUT/'build_ab_observer.log').open('w') as log:subprocess.run(['sh',str(OUT/'build_ab_observer.sh')],stdout=log,stderr=subprocess.STDOUT,check=True)
files=[ROOT/'tools/fire/r258_ab_driver.cpp',OUT/'sanity_mc_driver',OUT/'charm_driver']
(ROOT/'docs/data/fire_r258/early_check_build.json').write_text(json.dumps({'check':'MC two-step witness, then compare first two B accepted Q/T/M/U fields numerically; abort before third step if identical','sha256':{str(p):hashlib.sha256(p.read_bytes()).hexdigest() for p in files}},indent=2)+'\n')
