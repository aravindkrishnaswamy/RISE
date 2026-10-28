#!/usr/bin/env python3
"""One controlled FDS wall-velocity-BC substitution; retain all physical outputs."""
from pathlib import Path
import re,json,hashlib,difflib
ROOT=Path(__file__).resolve().parents[2];BASE=Path('/Volumes/SSD/RISE/fire_production_calibration');WORK=BASE/'r263_fds_no_slip';RUN=WORK/'fds_run';RUN.mkdir(parents=True,exist_ok=True);OUT=ROOT/'docs/data/fire_r263';OUT.mkdir(exist_ok=True)
a=(ROOT/'docs/data/fire_r261/r261_fds.fds').read_text();b=a.replace("CHID='r261_fds'","CHID='r263_fds'")
lines=b.splitlines();surfaces=[]
for i,line in enumerate(lines):
 if line.startswith('&SURF '):
  name=re.search("ID='([^']+)'",line).group(1);assert name=='FLOOR' or re.fullmatch('B[0-9]+',name)
  assert 'NO_SLIP' not in line and 'FREE_SLIP' not in line
  lines[i]=line.rsplit('/',1)[0].rstrip()+', NO_SLIP=T /';surfaces.append(name)
b='\n'.join(lines)+'\n';assert len(surfaces)==70 and len(set(surfaces))==70
# Round-trip equality makes all other namelists/output definitions identical.
assert b.replace(", NO_SLIP=T /"," / ").replace("CHID='r263_fds'","CHID='r261_fds'").split()==a.split()
(RUN/'r263_fds.fds').write_text(b);(OUT/'r263_fds.fds').write_text(b)
(OUT/'input.patch').write_text(''.join(difflib.unified_diff(a.splitlines(True),b.splitlines(True),fromfile='default_wall_r261',tofile='no_slip_r263')))
(OUT/'input_manifest.json').write_text(json.dumps({'only_changes':['CHID output prefix','NO_SLIP=T on FLOOR and B0..B68'],'surfaces':surfaces,'unchanged':'LES,Vreman,Cv,CHARM,domain,grid,gravity,thermochemistry,source flux,temperature,radiation,all devices,slices,cadences and run duration','limitation':'NO_SLIP bypasses wall shear function; existing FDS near-wall eddy-viscosity treatment is unchanged, so this isolates the velocity/shear boundary choice rather than making both entire solvers identical','sha256':{'default_input':hashlib.sha256(a.encode()).hexdigest(),'no_slip_input':hashlib.sha256(b.encode()).hexdigest()}},indent=2)+'\n')
