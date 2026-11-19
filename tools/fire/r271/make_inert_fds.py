#!/usr/bin/env python3
"""r271 non-reacting plume, FDS side: r263's input with combustion and radiation removed and the fire's convective heat
put in as a fixed volumetric source over the burner. Everything else (domain, grid, LES/Vreman, CHARM, NO_SLIP floor and
burner, open boundaries, slices, devices, cadences, duration) is r263's.
  - &REAC removed; METHANE becomes a tracked inert species (&SPEC).
  - Each burner SURF B0..B68: HRRPUA -> MASS_FLUX(1) = HRRPUA/HEAT_OF_COMBUSTION of METHANE (same fuel inflow, 300 K).
  - A frozen volumetric heat source of 26.4 kW (0.8 x 33 kW, the convective share at r263's radiative fraction 0.2),
    uniform over the 69 burner cells of each layer 0..26 and split between layers by r263's time-mean heat release
    (docs/data/fire_r271/inert_source_profile.json; RISE's FireCase::InertPlumeFraction holds the same table). Written
    as &INIT HRRPUV boxes, one per contiguous x-run of burner cells per y-row per layer.
  - &RADI RADIATION=F: no radiative loss, so the INIT source enters in full (radi.f90 ADD_VOLUMETRIC_HEAT_SOURCE mode 0).
  - OXYGEN mass-fraction slices removed (no oxygen species without a reaction).
argv[1]: output run directory."""
from pathlib import Path
import re,sys,json,hashlib
ROOT=Path(__file__).resolve().parents[3];src=(ROOT/'docs/data/fire_r263/r263_fds.fds').read_text()
OUT=Path(sys.argv[1]);OUT.mkdir(parents=True,exist_ok=True)
lines=src.splitlines();hoc=None;out=[];surf={};cells=[];removed=[]
prof=json.loads((ROOT/'docs/data/fire_r271/inert_source_profile.json').read_text())
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
    if m:cells.append(tuple(float(m.group(i)) for i in range(1,5)))
    out.append(l)
assert len(surf)==69 and len(cells)==69 and len(removed)==106 and prof['layers']==27 and abs(sum(prof['fractions'])-1)<1e-12
# C++ table must hold the same fractions.
cpp=(ROOT/'src/Library/Utilities/FireCase.cpp').read_text();table=cpp[cpp.index('static const double fraction[InertPlumeLayers]={')+49:]
table=[float(v) for v in table[:table.index('}')].replace('\n','').split(',')];assert table==prof['fractions'],'FireCase.cpp table differs'
rows={}
for x0,x1,y0,y1 in cells:rows.setdefault((y0,y1),[]).append((x0,x1))
runs=[]
for (y0,y1),xs in sorted(rows.items()):
    xs.sort();a,b=xs[0]
    for x0,x1 in xs[1:]:
        if abs(x0-b)<1e-9*dx:b=x1
        else:runs.append((a,b,y0,y1));a,b=x0,x1
    runs.append((a,b,y0,y1))
assert abs(sum((b-a)*(y1-y0) for a,b,y0,y1 in runs)-69*dx*dx)<1e-9
inits=[]
for k,f in enumerate(prof['fractions']):
    q=prof['total_W']*1e-3*f/(69*dx**3)  # kW/m3
    for a,b,y0,y1 in runs:inits.append(f"&INIT XB={a!r},{b!r},{y0!r},{y1!r},{k*dx!r},{(k+1)*dx!r}, HRRPUV={q!r} /")
i=next(n for n,l in enumerate(out) if l.startswith('&TAIL'))
text='\n'.join(out[:i]+["&RADI RADIATION=F /"]+inits+out[i:]).replace("CHID='r263_fds'","CHID='r271_inert'")+'\n'
assert "CHID='r271_inert'" in text and '&REAC' not in text and 'HRRPUA' not in text
(OUT/'r271_inert.fds').write_text(text)
area=dx*dx;total=prof['total_W']*1e-3
(OUT/'input_manifest.json').write_text(json.dumps({'source':'docs/data/fire_r263/r263_fds.fds','heat_of_combustion_kJ_per_kg':hoc,
 'dx_m':dx,'burner_cells':len(surf),'fuel_mass_flux_kg_s':sum(q/hoc*area for q in surf.values()),'heat_source_kW':total,
 'removed_oxygen_slices':len(removed),'init_boxes':len(inits),'profile':'docs/data/fire_r271/inert_source_profile.json','sha256':hashlib.sha256(text.encode()).hexdigest()},indent=2)+'\n')
print(f'wrote {OUT/"r271_inert.fds"}: 69 burner cells, fuel {sum(q/hoc*area for q in surf.values()):.6f} kg/s, heat source {total:.3f} kW')
