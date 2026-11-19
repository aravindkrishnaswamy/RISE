#!/usr/bin/env python3
"""r271 section 33: the non-reacting plume FDS input (make_inert_fds.py) on RISE's grid at another resolution tier.
dx = D*/tier with D* = 8 x the tier-8 cell width; the domain, burner mask and per-cell inflow factors follow RISE's
FireCase (extent >= 7 D by >= 5 flame heights; cells whose centre lies within D/2 of the domain centre; SplitMix64
1% jitter normalised to the authored area). The frozen heat source keeps its physical vertical profile: the tier-8
table (inert_source_profile.json) integrated over each cell layer, uniform over that tier's burner cells
(FireCase::InertPlumeLayerFraction). Slices: T, W (layer and the one below), MASS FLUX Z at the layers nearest the
tier-8 analysis heights, plus T/U/V/W in layers 0..3.
argv: tier, output run dir [, T_END s (default 10)]. With tier 8 it also checks itself against make_inert_fds.py's input."""
from pathlib import Path
import re,sys,json,math
ROOT=Path(__file__).resolve().parents[3];tier=float(sys.argv[1]);OUT=Path(sys.argv[2]);TEND=float(sys.argv[3]) if len(sys.argv)>3 else 10.
DX8=.030593116067411014;DSTAR=8*DX8;dx=DSTAR/tier;D=.30;Q=33.
ceil=lambda v:int(math.ceil(v-1e-12))
n=ceil(7*D/dx);flame=.235*Q**.4-1.02*D;nz=ceil(5*max(flame,D)/dx);half=n*dx/2
src=(ROOT/'docs/data/fire_r263/r263_fds.fds').read_text()
hoc=float(re.search(r'&REAC.*HEAT_OF_COMBUSTION=([0-9.eE+-]+)',src).group(1))
prof=json.loads((ROOT/'docs/data/fire_r271/inert_source_profile.json').read_text());frac8=prof['fractions']
M=(1<<64)-1
def splitmix(x):
    x=(x+0x9E3779B97F4A7C15)&M;x=((x^(x>>30))*0xBF58476D1CE4E5B9)&M;x=((x^(x>>27))*0x94D049BB133111EB)&M;return x^(x>>31)
mask=[];raw=[]
for j in range(n):
    for i in range(n):
        x=(i+.5)*dx;y=(j+.5)*dx
        if (x-half)**2+(y-half)**2<=.25*D*D:mask.append((i,j));raw.append(2*(splitmix(1234^(i+n*j))>>11)*2.0**-53-1)
mean=sum(raw)/len(raw);base=(math.pi*D*D/4)/(dx*dx)/len(mask);factor=[base*(1+.01*(r-mean)) for r in raw]
nominal=Q/(math.pi*D*D/4)
L=[f"&HEAD CHID='r271_inert', TITLE='r271 inert plume, RISE tier {tier:g} grid; Vreman 0.07' /",
 f'&MESH IJK={n},{n},{nz}, XB={-half:.17g},{half:.17g},{-half:.17g},{half:.17g},0,{nz*dx:.17g} /',f'&TIME T_END={TEND:.17g} /']
L+=[l for l in (Path('/Volumes/SSD/RISE/fire_production_calibration/r271_inert_fds/fds_run/r271_inert.fds').read_text().splitlines())
    if l.startswith(('&MISC','&SPEC','&SURF ID=\'FLOOR\'','&VENT MB','&RADI'))]
assert len(L)==3+1+1+1+5+1
for k,((i,j),f) in enumerate(zip(mask,factor)):
    L.append(f"&SURF ID='B{k}', MASS_FLUX(1)={nominal*f/hoc!r}, SPEC_ID(1)='METHANE', TMP_FRONT=26.85, NO_SLIP=T /")
    L.append(f"&VENT XB={(i*dx-half):.17g},{((i+1)*dx-half):.17g},{(j*dx-half):.17g},{((j+1)*dx-half):.17g},0,0, SURF_ID='B{k}' / ")
def lfrac(z):
    lo,hi=z*dx,(z+1)*dx;return sum(f*max(0.,min(hi,(s+1)*DX8)-max(lo,s*DX8))/DX8 for s,f in enumerate(frac8))
tot=0.
for z in range(nz):
    f=(frac8[z] if z<len(frac8) else 0.) if tier==8 else lfrac(z)
    if f==0:continue
    q=26.4*f/(len(mask)*dx**3);tot+=f
    for j in sorted({j for _,j in mask}):
        xs=sorted(i for i,jj in mask if jj==j);assert xs==list(range(xs[0],xs[-1]+1))
        L.append(f"&INIT XB={(xs[0]*dx-half)!r},{((xs[-1]+1)*dx-half)!r},{(j*dx-half)!r},{((j+1)*dx-half)!r},{z*dx!r},{(z+1)*dx!r}, HRRPUV={q!r} /")
assert abs(tot-1)<1e-9,tot
ZT=[(k+.5)*DX8 for k in (9,16,24,32,49,65,81,98)];K=[int(round(z/dx-.5)) for z in ZT];K=[k for k in K if k<nz]
def slc(q,k):L.append(f"&SLCF PBZ={(k+.5)*dx:.17g}, QUANTITY='{q}', CELL_CENTERED=T /")
for k in sorted(set(range(4))|set(K)|{k-1 for k in K}):
    slc('TEMPERATURE',k);slc('W-VELOCITY',k)
    if k<4:slc('U-VELOCITY',k);slc('V-VELOCITY',k)
for k in K:slc('MASS FLUX Z',k)
L+=['&DUMP DT_DEVC=0.03125, DT_HRR=0.03125, DT_SLCF=0.0625, DT_RESTART=1000, COLUMN_DUMP_LIMIT=F /','&TAIL /']
OUT.mkdir(parents=True,exist_ok=True);(OUT/'r271_inert.fds').write_text('\n'.join(L)+'\n')
(OUT/'grid.json').write_text(json.dumps({'tier':tier,'dx':dx,'n':n,'nz':nz,'burner_cells':len(mask),'layers':K,'heights':[(k+.5)*dx for k in K]})+'\n')
print(f'tier {tier:g}: dx {dx:.6f} m, grid {n}x{n}x{nz}, burner cells {len(mask)}, layers {K}')
if tier==8:
    ref=Path('/Volumes/SSD/RISE/fire_production_calibration/r271_inert_fds/fds_run/r271_inert.fds').read_text().splitlines()
    pick=lambda t:[l.strip() for l in t if l.startswith(("&SURF ID='B","&VENT XB","&INIT","&MESH"))]
    a,b=pick(ref),pick(L);num=lambda l:[float(v) for v in re.findall(r'-?\d+\.?\d*(?:[eE][+-]?\d+)?',l)]
    assert len(a)==len(b),(len(a),len(b));worst=max(abs(x-y)/max(1e-30,abs(x)+abs(y)) for l,m in zip(a,b) for x,y in zip(num(l),num(m)))
    print('tier-8 self-check against make_inert_fds.py input: lines',len(a),'max relative difference',worst)
