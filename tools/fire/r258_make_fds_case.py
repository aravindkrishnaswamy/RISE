#!/usr/bin/env python3
"""Explicit LES/Vreman/CHARM repeat; retain near-base diagnostics and radial fields."""
from pathlib import Path
import json,hashlib
ROOT=Path(__file__).resolve().parents[2];BASE=Path('/Volumes/SSD/RISE/fire_production_calibration');OUT=BASE/'r258_fds_les';RUN=OUT/'fds_run';RUN.mkdir(parents=True,exist_ok=True)
dx=.030593116067411014
s=(ROOT/'docs/data/fire_r255/r255_fds.fds').read_text().replace("CHID='r255_fds'","CHID='r258_fds'")
s=s.replace('&MISC TMPA=',"&MISC SIMULATION_MODE='LES', FLUX_LIMITER='CHARM', TMPA=")
extra=[]
# All low planes are retained, so actual divergence can be reconstructed from
# native face velocities as well as inspecting FDS's thermodynamic target D.
for z in list(range(0,18))+[32,65,98]:
 for q in ['TEMPERATURE','U-VELOCITY','V-VELOCITY','W-VELOCITY','DENSITY','DIVERGENCE']:
  extra.append(f"&SLCF PBZ={(z+.5)*dx:.17g}, QUANTITY='{q}', CELL_CENTERED=T /")
# Output inlet velocity at every authored burner-face center, including the rim.
metadata=(BASE/'r255_comparison/case_metadata.txt').read_text().splitlines()[3:]
for k,line in enumerate(metadata):
 _,x,y,p=line.split();xx=(int(x)+.5-34.5)*dx;yy=(int(y)+.5-34.5)*dx
 extra.append(f"&DEVC ID='BURNW{k:03}', XYZ={xx:.17g},{yy:.17g},0, QUANTITY='W-VELOCITY', TEMPORAL_STATISTIC='INSTANT VALUE' /")
s=s.replace('&TAIL /','\n'.join(extra)+'\n&TAIL /')
(RUN/'r258_fds.fds').write_text(s);(ROOT/'docs/data/fire_r258/r258_fds.fds').write_text(s)
manifest={'mode':'LES','scalar_limiter':'CHARM','sgs':'VREMAN','C_VREMAN':.07,'PR':.7,'SC':.7,'grid':[69,69,106],'dx_m':dx,'geometry_and_burner':'unchanged r255 input','output_additions':'instantaneous near-base T/u/v/w/rho/thermodynamicD and 1/2/3m planes; burner-face w devices','input_sha256':hashlib.sha256(s.encode()).hexdigest(),'no_RISE_production_changes':True}
(ROOT/'docs/data/fire_r258/input_manifest.json').write_text(json.dumps(manifest,indent=2)+'\n')
print('Prepared',RUN/'r258_fds.fds')
