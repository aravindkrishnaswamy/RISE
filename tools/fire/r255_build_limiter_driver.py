#!/usr/bin/env python3
"""Build a host observer against existing solver objects; kernels are untouched."""
from pathlib import Path
import subprocess,shlex
ROOT=Path(__file__).resolve().parents[2];BASE=Path('/Volumes/SSD/RISE/fire_production_calibration');OUT=BASE/'r255_comparison'
source=(ROOT/'tests/FireSequenceTest.cpp').read_text()
old='persistence.stopAfterAdditionalAcceptedSteps<=64u&&'
assert source.count(old)==1
# Only the test-launcher's bounded diagnostic duration is extended. No arithmetic
# or solver code is rewritten. Keep original authorizations and case checks.
source=source.replace(old,'persistence.stopAfterAdditionalAcceptedSteps<=4096u&&')
(OUT/'FireSequenceObserverHarness.cpp').write_text(source)
script=(BASE/'r253_plume_diagnosis/build.sh').read_text().replace('r253_plume_probe','r255_limiter_driver')
script=script.replace(' -c /Volumes/SSD/Working/RISE/tools/fire/r255_limiter_driver.cpp',f' -I{shlex.quote(str(ROOT/"tests"))} -DR255_HARNESS_SOURCE=\\"{OUT}/FireSequenceObserverHarness.cpp\\" -c /Volumes/SSD/Working/RISE/tools/fire/r255_limiter_driver.cpp')
(OUT/'build_limiter.sh').write_text(script)
with (OUT/'build_limiter.log').open('w') as log:subprocess.run(['sh',str(OUT/'build_limiter.sh')],stdout=log,stderr=subprocess.STDOUT,check=True)
