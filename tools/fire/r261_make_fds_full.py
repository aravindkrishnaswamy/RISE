#!/usr/bin/env python3
"""One explicit-LES repeat with physical volume slices and native device histories."""
from pathlib import Path
import json,hashlib
ROOT=Path(__file__).resolve().parents[2];BASE=Path('/Volumes/SSD/RISE/fire_production_calibration');OUT=BASE/'r261_fds_full';RUN=OUT/'fds_run';RUN.mkdir(exist_ok=True)
DX=.030593116067411014;half=34.5*DX
s=(ROOT/'docs/data/fire_r258/r258_fds.fds').read_text().replace("CHID='r258_fds'","CHID='r261_fds'")
lines=[v for v in s.splitlines() if not v.startswith('&SLCF') and not v.startswith('&DUMP') and not v.startswith('&TAIL')]
planes=[];devices=[]
def device(id,xyz,q,**extra):
 text=f"&DEVC ID='{id}', XYZ={','.join(format(x,'.17g') for x in xyz)}, QUANTITY='{q}', TEMPORAL_STATISTIC='INSTANT VALUE'"
 for k,v in extra.items():text+=f", {k}='{v}'"
 lines.append(text+' /');devices.append({'id':id,'xyz_m':xyz,'quantity':q,**extra})
# First106files remain native mass-flux faces1..106, for the existing reducer.
for k in range(1,107):
 lines.append(f"&SLCF PBZ={(k-.5)*DX:.17g}, QUANTITY='MASS FLUX Z', CELL_CENTERED=T /");planes.append({'quantity':'MASS FLUX Z','native_face_k':k})
quantities=['HRRPUV','TEMPERATURE','U-VELOCITY','V-VELOCITY','W-VELOCITY','DENSITY','DIVERGENCE','PRESSURE','VISCOSITY','MOLECULAR VISCOSITY']
for z in range(106):
 for q in quantities:
  lines.append(f"&SLCF PBZ={(z+.5)*DX:.17g}, QUANTITY='{q}', CELL_CENTERED=T /");planes.append({'quantity':q,'native_cell_z':z,'height_m':(z+.5)*DX})
 for species in ['METHANE','OXYGEN']:
  lines.append(f"&SLCF PBZ={(z+.5)*DX:.17g}, QUANTITY='MASS FRACTION', SPEC_ID='{species}', CELL_CENTERED=T /");planes.append({'quantity':'MASS FRACTION','species':species,'native_cell_z':z})
 # Native-layer HRR integral (kW). Divide bydx for a plane's kW/m.
 lines.append(f"&DEVC ID='QZ{z:03}', XB={-half:.17g},{half:.17g},{-half:.17g},{half:.17g},{z*DX:.17g},{(z+1)*DX:.17g}, QUANTITY='HRRPUV', SPATIAL_STATISTIC='VOLUME INTEGRAL', TEMPORAL_STATISTIC='INSTANT VALUE' /")
 device(f'HRC{z:03}',[0,0,(z+.5)*DX],'HRRPUV');device(f'RHOC{z:03}',[0,0,(z+.5)*DX],'DENSITY')
# Four radial rays with true cell-centeredT/HRR/rho and paired native W faces.
for z in [32,65,98]:
 for direction,(ax,ay) in enumerate([(1,0),(-1,0),(0,1),(0,-1)]):
  for r in range(1,27):
   x=ax*r*DX;y=ay*r*DX;prefix=f'R{z:02}{direction}{r:02}'
   for suffix,q in [('T','TEMPERATURE'),('Q','HRRPUV'),('R','DENSITY')]:device(prefix+suffix,[x,y,(z+.5)*DX],q)
   device(prefix+'WL',[x,y,z*DX],'W-VELOCITY');device(prefix+'WU',[x,y,(z+1)*DX],'W-VELOCITY')
for q in quantities:
 for axis in ['PBX','PBY']:lines.append(f"&SLCF {axis}=0, QUANTITY='{q}', CELL_CENTERED=T /")
lines+=['&DUMP DT_DEVC=0.03125, DT_HRR=0.03125, DT_SLCF=0.0625, DT_RESTART=5, COLUMN_DUMP_LIMIT=F /','&TAIL /']
text='\n'.join(lines)+'\n';(RUN/'r261_fds.fds').write_text(text);(ROOT/'docs/data/fire_r261/r261_fds.fds').write_text(text)
manifest={'physics':'unchanged r258 explicitLES/Vreman.07/CHARM, PR=SC=.7, 33kW,300K,69x69x106','physical_slices':'all106nativeplanes, unclippedfloat32HRR/T/U/V/W/rho/D/pressure/effective&molecularviscosity/CH4/O2; plus verticalcentralcuts','plane_HRR_devices':'QZ000..105 native volume integrals; reconstruct0-.7m with partialtoplayer fromslices','radial_devices':'four rays,26stations,threeheights; T/HRR/rho and native W pair','existing_centerline_and_burner_devices':'retained fromr258','cadence_s':{'devices':.03125,'slices':.0625},'slice_manifest':planes,'new_device_manifest':devices,'input_sha256':hashlib.sha256(text.encode()).hexdigest(),'raw_outputs':'external; retain all files; no cleanup after reduction'}
(ROOT/'docs/data/fire_r261/output_manifest.json').write_text(json.dumps(manifest,indent=2)+'\n');print('Slices',sum(v.startswith('&SLCF') for v in lines),'devices',sum(v.startswith('&DEVC') for v in lines))
