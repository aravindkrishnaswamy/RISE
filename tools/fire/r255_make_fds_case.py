#!/usr/bin/env python3
"""Match physical r251 geometry, inlet mask and nominal fuel power in FDS 6.11.1."""
from pathlib import Path
import math
BASE=Path('/Volumes/SSD/RISE/fire_production_calibration/r255_comparison')
rows=[l.split() for l in (BASE/'case_metadata.txt').read_text().splitlines()]
dx=float(rows[0][1]);ambient=[float(v) for v in rows[1][1:]];lhv=float(rows[2][1]);half=69*dx/2;height=106*dx
lines=["&HEAD CHID='r255_fds', TITLE='r251 matched physical configuration; Vreman 0.07' /",f'&MESH IJK=69,69,106, XB={-half:.17g},{half:.17g},{-half:.17g},{half:.17g},0,{height:.17g} /','&TIME T_END=25.091040127212182 /',"&MISC TMPA=26.85, HUMIDITY=0, GVEC=0,0,-9.80665, TURBULENCE_MODEL='VREMAN', C_VREMAN=0.07, PR=0.7, SC=0.7, Y_O2_INFTY=0.23291751145757966, Y_CO2_INFTY=0 / ", f"&REAC FUEL='METHANE', HEAT_OF_COMBUSTION={lhv/1000:.17g}, RADIATIVE_FRACTION=0.20, SOOT_YIELD=0, CO_YIELD=0 /", "&SURF ID='FLOOR', ADIABATIC=T, DEFAULT=T /"]
for side in ['XMIN','XMAX','YMIN','YMAX','ZMAX']:lines.append(f"&VENT MB='{side}', SURF_ID='OPEN' /")
nominal=33/(math.pi*.3*.3/4)
for k,row in enumerate(rows[3:]):
 _,x,y,p=row;x=int(x);y=int(y);power=nominal*float(p)
 lines.append(f"&SURF ID='B{k}', HRRPUA={power:.17g}, TMP_FRONT=26.85 / ")
 xb=[(x-34.5)*dx,(x+1-34.5)*dx,(y-34.5)*dx,(y+1-34.5)*dx,0,0]
 lines.append(f"&VENT XB={','.join(format(v,'.17g') for v in xb)}, SURF_ID='B{k}' / ")
for z in range(106):
 h=(z+.5)*dx
 lines.append(f"&DEVC ID='T{z:03}', XYZ=0,0,{h:.17g}, QUANTITY='TEMPERATURE', TEMPORAL_STATISTIC='INSTANT VALUE' /")
# FDS velocity devices select a nearest staggered face, not a cell-center mean.
for k in range(107):lines.append(f"&DEVC ID='WF{k:03}', XYZ=0,0,{k*dx:.17g}, QUANTITY='W-VELOCITY', TEMPORAL_STATISTIC='INSTANT VALUE' /")
# Conservative face mass flux; reduce at native z faces and interpolate to the
# cell-center heights used by r253, retaining the face/cell convention explicitly.
for k in range(1,107):lines.append(f"&SLCF PBZ={(k-.5)*dx:.17g}, QUANTITY='MASS FLUX Z', CELL_CENTERED=T /")
lines.extend(['&DUMP DT_DEVC=0.03125, DT_HRR=0.03125, DT_SLCF=0.0625, DT_RESTART=5 /','&TAIL /'])
(BASE/'fds_run').mkdir(exist_ok=True);text='\n'.join(lines)+'\n';(BASE/'fds_run/r255_fds.fds').write_text(text)
(Path(__file__).resolve().parents[2]/'docs/data/fire_r255/r255_fds.fds').write_text(text)
print('inlet nominal kW',sum(nominal*float(r[3])*dx*dx for r in rows[3:]),'burner faces',len(rows)-3,'domain',half*2,height)
