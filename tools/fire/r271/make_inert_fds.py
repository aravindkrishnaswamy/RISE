#!/usr/bin/env python3
"""r271 non-reacting plume, FDS side: r263's input with combustion and radiation removed and the fire's convective heat
put in as a fixed volumetric source over the burner. Everything else (domain, grid, LES/Vreman, CHARM, NO_SLIP floor and
burner, open boundaries, slices, devices, cadences, duration) is r263's.
  - &REAC removed; METHANE becomes a tracked inert species (&SPEC).
  - Each burner SURF B0..B68: HRRPUA -> MASS_FLUX(1) = HRRPUA/HEAT_OF_COMBUSTION of METHANE (same fuel inflow, 300 K).
  - One &INIT per burner cell over the first gas layer (z 0..dx): HRRPUV = 0.8 HRRPUA/dx, i.e. 80% of the cell's fire
    heat (the convective share at r263's radiative fraction 0.2) as a volumetric source; total ~26.2 kW.
  - &RADI RADIATION=F: no radiative loss, so the INIT source enters in full (radi.f90 ADD_VOLUMETRIC_HEAT_SOURCE mode 0).
  - OXYGEN mass-fraction slices removed (no oxygen species without a reaction).
argv[1]: output run directory."""
from pathlib import Path
import re,sys,json,hashlib
ROOT=Path(__file__).resolve().parents[3];src=(ROOT/'docs/data/fire_r263/r263_fds.fds').read_text()
OUT=Path(sys.argv[1]);OUT.mkdir(parents=True,exist_ok=True)
lines=src.splitlines();hoc=None;out=[];surf={};inits=[];removed=[]
for l in lines:
    if l.startswith('&REAC'):hoc=float(re.search(r'HEAT_OF_COMBUSTION=([0-9.eE+-]+)',l).group(1))
dx=None
for l in lines:
    m=re.match(r"&MESH IJK=(\d+),(\d+),(\d+), XB=([^/]+)/",l)
    if m:xb=[float(v) for v in m.group(4).split(',')];dx=(xb[5]-xb[4])/int(m.group(3))
assert hoc and dx
for l in lines:
    if l.startswith('&REAC'):out.append("&SPEC ID='METHANE' /");continue
    if l.startswith('&SLCF') and "SPEC_ID='OXYGEN'" in l:removed.append(l);continue
    m=re.match(r"&SURF ID='(B\d+)', HRRPUA=([0-9.eE+-]+), (.*)/\s*$",l)
    if m:
        q=float(m.group(2));surf[m.group(1)]=q
        out.append(f"&SURF ID='{m.group(1)}', MASS_FLUX(1)={q/hoc!r}, SPEC_ID(1)='METHANE', {m.group(3).strip()} /");continue
    m=re.match(r"&VENT XB=([^,]+),([^,]+),([^,]+),([^,]+),0,0, SURF_ID='(B\d+)'",l)
    if m:inits.append(f"&INIT XB={m.group(1)},{m.group(2)},{m.group(3)},{m.group(4)},0,{dx!r}, HRRPUV={0.8*surf[m.group(5)]/dx!r} /")
    out.append(l)
assert len(surf)==69 and len(inits)==69 and len(removed)==106
i=next(n for n,l in enumerate(out) if l.startswith('&TAIL'))
text='\n'.join(out[:i]+["&RADI RADIATION=F /"]+inits+out[i:]).replace("CHID='r263_fds'","CHID='r271_inert'")+'\n'
assert "CHID='r271_inert'" in text and '&REAC' not in text and 'HRRPUA' not in text
(OUT/'r271_inert.fds').write_text(text)
area=dx*dx;total=sum(0.8*q*area for q in surf.values())
(OUT/'input_manifest.json').write_text(json.dumps({'source':'docs/data/fire_r263/r263_fds.fds','heat_of_combustion_kJ_per_kg':hoc,
 'dx_m':dx,'burner_cells':len(surf),'fuel_mass_flux_kg_s':sum(q/hoc*area for q in surf.values()),'heat_source_kW':total,
 'removed_oxygen_slices':len(removed),'sha256':hashlib.sha256(text.encode()).hexdigest()},indent=2)+'\n')
print(f'wrote {OUT/"r271_inert.fds"}: 69 burner cells, fuel {sum(q/hoc*area for q in surf.values()):.6f} kg/s, heat source {total:.3f} kW')
