#!/usr/bin/env python3
"""Audited-frame top exchange by radius; sampled rates, not integrated budgets."""
import argparse
import csv
import hashlib
import json
from pathlib import Path
import numpy as np
from r245_budget_audit import merkle, read_record, require


def packet(path, root, case):
    payload = path.read_bytes()
    seal = json.loads(Path(str(path)+'.payload-v2.json').read_text())
    require(seal['case_record_id'] == case and hashlib.sha256(payload).hexdigest() == seal['sha256_v1'], 'packet SHA/case')
    tree = merkle(payload)
    require(tree == seal['v2'] and tree['root_sha256'] == root, 'packet differs from full-audit root')
    return payload


def top_exchange(native, flux, dimensions, dx):
    nx,ny,nz = dimensions; cells=nx*ny*nz
    counts=[(nx+1)*ny*nz,nx*(ny+1)*nz,nx*ny*(nz+1)]; faces=sum(counts)
    require(len(native)==(19*cells+9*faces)*4+216 and len(flux)==72+104*faces, 'capsule layout')
    raw=np.frombuffer(native,'<f4',count=19*cells+9*faces)
    rates=np.frombuffer(flux,'<f4',offset=72).reshape(26,faces)
    offset=counts[0]+counts[1]+nz*nx*ny
    selected=slice(offset,offset+nx*ny)
    alpha=raw[9*cells+8*faces:9*cells+9*faces][selected].astype('float64')
    limited=rates[:9,selected].astype('float64')+alpha*rates[9:18,selected].astype('float64')
    mass=limited[1:8].sum(axis=0)*dx*dx
    enthalpy=limited[8]*dx*dx
    temperature=raw[9*cells+9*faces:10*cells+9*faces][-nx*ny:]
    velocity=raw[9*cells+faces:9*cells+2*faces][selected]
    require(np.isfinite(mass).all() and np.isfinite(enthalpy).all() and np.isfinite(temperature).all() and np.isfinite(velocity).all(), 'nonfinite top values')
    y,x=np.indices((ny,nx)); radius=np.hypot((x+.5-nx/2)*dx,(y+.5-ny/2)*dx).ravel()
    return mass,enthalpy,temperature,velocity,radius


def main():
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('run',type=Path);parser.add_argument('audit',type=Path);parser.add_argument('output',type=Path)
    args=parser.parse_args();proof=json.loads(args.audit.read_text())
    require(proof['status']=='full_budget_bytes_verified_not_physical_qualification','full audit required')
    directory=args.run/'budgets';meta,_=read_record(directory/f"step_{proof['steps']:06d}.csv")
    require(hashlib.sha256((directory/f"step_{proof['steps']:06d}.csv").read_bytes()).hexdigest()==proof['final_step_sha256'],'final audit pin')
    dimensions=tuple(map(int,meta['dimensions'].split()));dx=float(meta['binary32_solver_dx_m'])
    # Source centred radii are predefined in burner diameters, not fitted to data.
    edges=[0,.30,.60,1.20,1.80,float('inf')]
    rows=[];inputs=[]
    args.output.mkdir(exist_ok=False,parents=True)
    for record in proof['frames']:
        index=record['frame'];base=directory/f'frame_{index:05d}'
        native=packet(Path(str(base)+'.native.bin'),record['native_root'],proof['case'])
        flux=packet(Path(str(base)+'.flux.bin'),record['flux_root'],proof['case'])
        mass,enthalpy,temperature,velocity,radius=top_exchange(native,flux,dimensions,dx)
        columns=Path(str(base)+'.columns.csv').read_bytes()
        require(hashlib.sha256(columns).hexdigest()==record['columns_sha256'],'audited frame clock pin')
        first=next(csv.DictReader(columns.decode().splitlines()))
        time=float(first['beginning_time_s'])+float(first['dt_s'])
        inputs.append({'frame':index,'native_sha256':hashlib.sha256(native).hexdigest(),'flux_sha256':hashlib.sha256(flux).hexdigest()})
        for ring,(lower,upper) in enumerate(zip(edges[:-1],edges[1:])):
            mask=(radius>=lower)&(radius<upper);incoming=np.maximum(-mass[mask],0);outgoing=np.maximum(mass[mask],0)
            total=float(incoming.sum())
            rows.append({'frame':index,'time_s':time,'ring':ring,'r_min_m':lower,'r_max_m':upper if np.isfinite(upper) else 'outer_edge',
                         'mass_in_kg_per_s':total,'mass_out_kg_per_s':float(outgoing.sum()),
                         'sensible_enthalpy_in_J_per_s':float(np.maximum(-enthalpy[mask],0).sum()),
                         'sensible_enthalpy_out_J_per_s':float(np.maximum(enthalpy[mask],0).sum()),
                         'inflow_weighted_endpoint_top_cell_temperature_K':float(np.dot(incoming,temperature[mask])/total) if total else '',
                         'inflow_weighted_endpoint_top_face_velocity_m_per_s':float(np.dot(incoming,velocity[mask])/total) if total else ''})
        print(f'FRAME {index+1}/235',flush=True)
    with (args.output/'top_exchange_by_radius.csv').open('w') as stream:
        writer=csv.DictWriter(stream,fieldnames=list(rows[0]),lineterminator='\n');writer.writeheader();writer.writerows(rows)
    record={'status':'sampled_top_exchange_not_integrated_budget_or_causal_proof','case':proof['case'],'frames':len(inputs),
            'contract':'Limited accepted Heun scalar flux sampled at 235 original frame boundaries. Positive is outward. Total species excludes Z tracer. Radius at top face centre about fixed source centre. Endpoint temperature/velocity are separate contemporaneous native fields, not stage flux donors or pressure classes. No time integration or spectrum/antialias claim.',
            'audit_sha256':hashlib.sha256(args.audit.read_bytes()).hexdigest(),'tool_sha256':hashlib.sha256(Path(__file__).read_bytes()).hexdigest(),'inputs':inputs}
    (args.output/'top_exchange_provenance.json').write_text(json.dumps(record,indent=2,allow_nan=False)+'\n')


if __name__=='__main__':main()
