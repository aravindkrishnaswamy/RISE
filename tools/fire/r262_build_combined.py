#!/usr/bin/env python3
"""Compose the previously tested CHARM and centered momentum external variants."""
from pathlib import Path
import subprocess,hashlib,json,difflib
ROOT=Path(__file__).resolve().parents[2]
BASE=Path('/Volumes/SSD/RISE/fire_production_calibration')
OUT=BASE/'r262_combined';OUT.mkdir(exist_ok=True)
DATA=ROOT/'docs/data/fire_r262';DATA.mkdir(exist_ok=True)
# Apply exactly r258's active resident CHARM substitutions to r260's centered source.
source=(BASE/'r260_components/CenteredAdvection.mm').read_text()
builder=(ROOT/'tools/fire/r258_build_resident_charm.py').read_text()
substitutions=builder[builder.index("old='''inline float pf_mc"):builder.index("source=OUT/")]
namespace={'s':source};exec(substitutions,namespace)
s=namespace['s'];path=OUT/'CombinedAdvection.mm';path.write_text(s)
(DATA/'combined.patch').write_text(''.join(difflib.unified_diff(source.splitlines(True),s.splitlines(True),fromfile='r260_centered',tofile='r262_combined')))
lines=(BASE/'r260_components/build.sh').read_text().splitlines()
compile=next(x for x in lines if x.startswith('c++ ') and 'CenteredAdvection.mm' in x)
link=next(x for x in lines if x.startswith('c++ ') and ' -o '+str(BASE/'r260_components/centered_driver')+' ' in x)
compile=compile.replace(str(BASE/'r260_components/CenteredAdvection.mm'),str(path)).replace(str(BASE/'r260_components/CenteredAdvection.o'),str(OUT/'CombinedAdvection.o'))
link=link.replace(str(BASE/'r260_components/CenteredAdvection.o'),str(OUT/'CombinedAdvection.o')).replace(str(BASE/'r260_components/centered_driver'),str(OUT/'combined_driver'))
(OUT/'build.sh').write_text('set -e\ncd '+str(ROOT/'build/make/rise')+'\n'+compile+'\n'+link+'\n')
with (OUT/'build.log').open('w') as f:subprocess.run(['sh',str(OUT/'build.sh')],stdout=f,stderr=subprocess.STDOUT,check=True)
(DATA/'build.json').write_text(json.dumps({'production_change':False,'composition':'r260 centered momentum plus exactly r258 active resident direction-aware CHARM','sha256':{str(p):hashlib.sha256(p.read_bytes()).hexdigest() for p in [path,OUT/'combined_driver',BASE/'r260_components/observer.o']}},indent=2)+'\n')
