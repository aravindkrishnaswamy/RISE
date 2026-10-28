#!/usr/bin/env python3
"""Link the same diagnostic solver objects into a bounded from-zero onset driver."""
from pathlib import Path
import subprocess
ROOT=Path(__file__).resolve().parents[2];BASE=Path('/Volumes/SSD/RISE/fire_production_calibration');OUT=BASE/'r262_combined'
lines=(BASE/'r260_components/build.sh').read_text().splitlines()
c=next(x for x in lines if x.startswith('c++ ') and 'r260_component_driver.cpp' in x)
c=c.replace(str(ROOT/'tools/fire/r260_component_driver.cpp'),str(ROOT/'tools/fire/r262_onset_driver.cpp')).replace(str(BASE/'r260_components/observer.o'),str(OUT/'onset.o'))
links=[]
for name,obj in [('centered',BASE/'r260_components/CenteredAdvection.o'),('combined',OUT/'CombinedAdvection.o')]:
 l=next(x for x in lines if x.startswith('c++ ') and ' -o '+str(BASE/'r260_components/centered_driver')+' ' in x)
 l=l.replace(str(BASE/'r260_components/centered_driver'),str(OUT/(name+'_onset'))).replace(str(BASE/'r260_components/observer.o'),str(OUT/'onset.o')).replace(str(BASE/'r260_components/CenteredAdvection.o'),str(obj));links.append(l)
(OUT/'build_onset.sh').write_text('set -e\ncd '+str(ROOT/'build/make/rise')+'\n'+'\n'.join([c]+links)+'\n')
with (OUT/'build_onset.log').open('w') as f:subprocess.run(['sh',str(OUT/'build_onset.sh')],stdout=f,stderr=subprocess.STDOUT,check=True)
