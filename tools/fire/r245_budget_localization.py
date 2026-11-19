#!/usr/bin/env python3
"""Summarize verified r245 budgets; sampled momentum is not a time integral."""
import argparse
import csv
import hashlib
import io
import json
from pathlib import Path
import numpy as np
from r245_budget_audit import read_record, require, sealed

TERMS = ('stress', 'buoyancy', 'advection', 'source', 'combined_projection', 'total', 'closure_residual')
SIDES = ('xmin', 'xmax', 'ymin', 'ymax', 'bottom', 'top')
FIELDS = ('Z', 'CH4', 'O2', 'N2', 'CO2', 'H2O', 'CO', 'carbon', 'sensible_enthalpy', 'total_species_mass')


def sha(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('run', type=Path)
    parser.add_argument('verification', type=Path)
    parser.add_argument('output', type=Path)
    args = parser.parse_args()
    proof = args.verification/'full_budget_audit.json'
    audit = json.loads(proof.read_text())
    require(audit['status'] == 'full_budget_bytes_verified_not_physical_qualification', 'full audit required')
    require(json.loads((args.verification/'completion.json').read_text())['exit_code'] == 0, 'verification failed')
    directory = args.run/'budgets'
    paths = sorted(directory.glob('step_*.csv'))
    require(len(paths) == audit['steps'], 'audit coverage changed')
    require(sha(paths[-1]) == audit.get('final_step_sha256'), 'final ledger differs from audited bytes')
    meta, rows = read_record(paths[-1])
    require(meta['case_record_id'] == audit['case'], 'foreign run')
    sealed(paths[-1], audit['case'])
    frame_paths = sorted(directory.glob('frame_*.columns.csv'))
    require(len(frame_paths) == len(audit['frames']) == 235, 'frame coverage')
    dx = float(meta['binary32_solver_dx_m'])
    nz = int(meta['dimensions'].split()[2])
    # Vertical faces occur once per selected column. Do not sum horizontal
    # exports: shared faces appear in both adjacent selected columns.
    samples = []
    provenance = []
    for index, path in enumerate(frame_paths):
        require(sha(path) == audit['frames'][index].get('columns_sha256'), 'columns differ from audited bytes')
        columns = list(csv.DictReader(io.StringIO(sealed(path, audit['case']).decode())))
        selected = [row for row in columns if row['axis'] == '2']
        require(len(selected) == 4*(nz+1), 'vertical coverage')
        station = []
        for z in range(nz+1):
            faces = [row for row in selected if int(row['z_face']) == z]
            require(len(faces) == 4 and len({row['face'] for row in faces}) == 4, 'duplicate vertical face')
            station.append([np.mean([float(row[term+'_rate_kg_per_m2_s2' if term != 'closure_residual' else 'closure_residual_kg_per_m2_s2']) for row in faces]) for term in TERMS])
        samples.append(station)
        provenance.append({'frame': index, 'path': str(path), 'sha256': sha(path)})
    values = np.asarray(samples)
    args.output.mkdir(exist_ok=False, parents=True)
    with (args.output/'vertical_momentum_summary.csv').open('w') as stream:
        writer = csv.writer(stream, lineterminator="\n")
        writer.writerow(['z_face', 'height_m', 'term', 'sample_mean_kg_per_m2_s2', 'sample_rms_kg_per_m2_s2', 'sample_std_kg_per_m2_s2'])
        for z in range(nz+1):
            for term, name in enumerate(TERMS):
                signal = values[:,z,term]
                writer.writerow([z,z*dx,name,float(signal.mean()),float(np.sqrt(np.mean(signal**2))),float(signal.std())])
    with (args.output/'scalar_final_ledgers.csv').open('w') as stream:
        writer = csv.DictWriter(stream, fieldnames=list(rows[0])+['field_name'], lineterminator='\n')
        writer.writeheader()
        writer.writerows(dict(row,field_name=FIELDS[int(row['field'])]) for row in rows)
    scalar = {}
    for scope in ('full', 'statistics'):
        scalar[scope] = {}
        for row in rows:
            if row['scope'] != scope:
                continue
            entry = {key: float(value) for key,value in row.items() if key not in ('scope','field','units')}
            entry['units'] = row['units']
            entry['lateral_in'] = sum(entry[side+'_in'] for side in SIDES[:4])
            entry['lateral_out'] = sum(entry[side+'_out'] for side in SIDES[:4])
            entry['inventory_change'] = entry['ending_inventory']-entry['beginning_inventory']
            # Closure includes the explicit interstep inventory term when nonzero;
            # retain it separately, not as physical boundary transport.
            scalar[scope][FIELDS[int(row['field'])]] = entry
    source = {name: dict(zip(('chemical_J','radiated_J','pilot_J','reacted_fuel_kg'),map(float,meta[key].split(','))))
              for name,key in (('full','cumulative_source'),('statistics','statistics_source'))}
    mass = scalar['statistics']['total_species_mass']
    result = {'status':'verified_budget_descriptive_localization_not_causal_proof',
              'case':audit['case'], 'steps':audit['steps'], 'frames':len(frame_paths),
              'end_time_s':audit['end_time_s'], 'statistics_start_s':float(meta['statistics_start_s']),
              'statistics_duration_s':audit['end_time_s']-float(meta['statistics_start_s']),
              'scalar':scalar,'source_components':source,
              'top_fraction_of_open_boundary_mass_in':mass['top_in']/(mass['top_in']+mass['lateral_in']),
              'sampled_momentum_contract':'Arithmetic average of four distinct source-centred vertical faces at each height, then unweighted statistics over 235 published frames. Instantaneous accepted-step rates: no time integration, stationarity, spectrum, pressure/restoration split or dissipation causation claim.',
              'maximum_absolute_sampled_mean_face_closure_kg_per_m2_s2':float(np.max(np.abs(values[:,:,6]))),
              'provenance':{'run':str(args.run),'verification':str(args.verification),'full_budget_audit_sha256':sha(proof),
                            'full_window_audit_sha256':sha(args.verification/'full_window_audit.json'),
                            'final_step_sha256':sha(paths[-1]),'analysis_sha256':sha(Path(__file__)), 'columns':provenance}}
    (args.output/'budget_localization.json').write_text(json.dumps(result,indent=2,allow_nan=False)+'\n')
    print(json.dumps({'steps':result['steps'],'top_mass_in_fraction':result['top_fraction_of_open_boundary_mass_in'],'statistics_mass':mass,'source':source['statistics']},indent=2))


if __name__ == '__main__':
    main()
