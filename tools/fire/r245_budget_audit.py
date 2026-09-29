#!/usr/bin/env python3
"""Independent accepted-stage capsule/ledger audit; never a physical acceptance gate.

Full mode requires the native completed full window. Fixture mode is explicitly
nonempirical and audits the exported focused owner controls only.
"""
import argparse
import csv
import hashlib
import io
import json
import math
from pathlib import Path
import struct
import sys

import numpy as np

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / 'tools'))
from fire_payload_merkle import merkle


def require(condition, message):
    if not condition:
        raise ValueError(message)


def sealed(path, case):
    payload = path.read_bytes()
    seal = json.loads(Path(str(path) + '.payload-v2.json').read_text())
    require(seal['schema'] == 'rise.fire.published-payload.v2' and seal['case_record_id'] == case,
            f'foreign seal: {path}')
    require(hashlib.sha256(payload).hexdigest() == seal['sha256_v1'] and merkle(payload) == seal['v2'],
            f'bad payload seal: {path}')
    return payload


def read_record(path):
    text = path.read_text()
    metadata = dict(line[2:].split(' ', 1) for line in text.splitlines() if line.startswith('# ') and ' ' in line[2:])
    rows = list(csv.DictReader(io.StringIO('\n'.join(line for line in text.splitlines() if not line.startswith('#')))))
    require(len(rows) == 30 and [(row['scope'], int(row['field'])) for row in rows] ==
            [(scope, field) for scope in ('step', 'full', 'statistics') for field in range(10)], 'budget row coverage')
    for row in rows:
        require(row['units'] == ('J' if row['field'] == '8' else 'kg'), 'budget row units')
        require(all(math.isfinite(float(v)) for k, v in row.items() if k not in ('scope', 'field', 'units')), 'nonfinite row')
    return metadata, rows


def enclosed(actual, expected, magnitude, operations, label):
    # Both reductions start from the same recorded binary32/binary64 operands. Enclose
    # floating additions/products in BOTH binary64 expression trees. This is
    # a roundoff audit, not a newly selected conservation/physics tolerance.
    u = 2.0**-53
    n = max(1, operations)
    require(n*u < 1, 'unbounded audit reduction')
    bound = (n*u/(1-n*u)) * (magnitude + abs(actual) + abs(expected))
    require(abs(actual-expected) <= bound, f'{label}: {actual} != {expected}; roundoff={bound}')


def audit_capsule(directory, frame, metadata, rows, case):
    base = directory / f'frame_{frame:05d}'
    binding = dict(line.split(' ', 1) for line in sealed(Path(str(base)+'.v1'), case).decode().splitlines())
    step_path = directory / f"step_{int(metadata['accepted_step']):06d}.csv"
    require(binding['schema'] == 'rise.fire.accepted-stage-frame.v1' and int(binding['frame']) == frame and
            binding['accepted_step'] == metadata['accepted_step'] and
            binding['step_record_sha256'] == hashlib.sha256(step_path.read_bytes()).hexdigest(), 'frame/event linkage')
    packets = {}
    for suffix in ('.flux.bin', '.native.bin', '.beginning.bin'):
        packet = sealed(Path(str(base)+suffix), case)
        require(hashlib.sha256(packet).hexdigest() == binding[suffix], 'capsule linkage')
        packets[suffix] = packet
    nx, ny, nz = map(int, metadata['dimensions'].split())
    cells = nx*ny*nz
    counts = [(nx+1)*ny*nz, nx*(ny+1)*nz, nx*ny*(nz+1)]
    offsets = [0, counts[0], counts[0]+counts[1]]
    faces = sum(counts)
    flux = packets['.flux.bin']; native = packets['.native.bin']; beginning = packets['.beginning.bin']
    require(len(flux) == 72+26*faces*4 and len(native) == (19*cells+9*faces)*4+216 and
            len(beginning) == (28*cells+faces)*4+nx*ny, 'capsule layout')
    require(merkle(flux)['root_sha256'] == metadata['flux_root'] and
            merkle(native)['root_sha256'] == metadata['native_root'], 'GPU root disagrees with Python Merkle')
    require(flux[:32].hex() == metadata['input_root'] == native[-32:].hex() and
            flux[32:64].hex() == metadata['native_root'] and
            struct.unpack_from('<Q', flux, 64)[0] == int(metadata['R1_candidate']), 'capsule authority binding')
    ids = struct.unpack_from('<22Q', native, (19*cells+9*faces)*4+8)
    require(ids[10] == int(metadata['R1_candidate']) and ids[21] != 0, 'native candidate binding')
    raw = np.frombuffer(native, '<f4', count=19*cells+9*faces)
    first = np.frombuffer(beginning, '<f4', count=18*cells+faces)
    q0 = first[:9*cells].reshape(9, cells).astype('float64')
    source = first[9*cells:18*cells].reshape(9, cells).astype('float64')
    q1 = raw[:9*cells].reshape(9, cells).astype('float64')
    alpha = raw[9*cells+8*faces:9*cells+9*faces].astype('float64')
    packet = np.frombuffer(flux, '<f4', offset=72).reshape(26, faces)
    require(np.isfinite(packet).all() and np.isfinite(raw).all() and np.isfinite(first).all() and
            ((alpha >= 0) & (alpha <= 1)).all(), 'invalid capsule field')
    limited = packet[:9].astype('float64') + alpha * packet[9:18].astype('float64')
    footprint = np.frombuffer(beginning, 'u1', offset=(28*cells+faces)*4)
    require(((footprint == 0) | (footprint == 1)).all(), 'footprint bits')
    # Verify all signed actual Heun rates before using the column exports.
    for term, start in enumerate((18, 20, 22, 24)):
        expected = np.float32((-0.5 if term == 0 else 0.5) * np.float32(packet[start] + packet[start+1]))
        observed = raw[9*cells+(4+term)*faces:9*cells+(5+term)*faces]
        require(np.array_equal(expected.view('uint32'), observed.view('uint32')), 'actual stage-rate Heun average')
    dx = float(metadata['binary32_solver_dx_m']); dt = float(metadata['dt_s'])
    volume = dx**3; area_dt = dx*dx*dt
    source_arrays = np.frombuffer(beginning, '<f8', count=5*cells, offset=(18*cells+faces)*4).reshape(5, cells)
    require(np.isfinite(source_arrays).all(), 'nonfinite source operands')
    source_expected = [(source_arrays[0]+source_arrays[1]).sum()*volume*dt,
                       source_arrays[2].sum()*volume*dt, source_arrays[3].sum()*volume,
                       source_arrays[4].sum()*volume]
    source_magnitudes = [(np.abs(source_arrays[0])+np.abs(source_arrays[1])).sum()*volume*dt,
                         np.abs(source_arrays[2]).sum()*volume*dt, np.abs(source_arrays[3]).sum()*volume,
                         np.abs(source_arrays[4]).sum()*volume]
    for actual, expected, magnitude in zip(source_components(metadata, 'step_source'), source_expected, source_magnitudes):
        enclosed(actual, float(expected), float(magnitude), 32*cells, 'frozen source component')
    side_values = []
    for axis, dimensions in enumerate(((nz, ny, nx+1), (nz, ny+1, nx), (nz+1, ny, nx))):
        block = limited[:, offsets[axis]:offsets[axis]+counts[axis]].reshape((9,)+dimensions)
        for side_index in (0, 1):
            plane = np.take(block, -1 if side_index else 0, axis=3-axis).reshape(9, -1)
            side_values.append(plane * area_dt * (1 if side_index else -1))
    names = ('xmin', 'xmax', 'ymin', 'ymax', 'bottom', 'top')
    for field in range(10):
        row = rows[field]
        state0 = q0[field] if field < 9 else q0[1:8].sum(axis=0)
        state1 = q1[field] if field < 9 else q1[1:8].sum(axis=0)
        delta = source[field] if field < 9 else source[1:8].sum(axis=0)
        magnitude = float((np.abs(state0).sum()+np.abs(state1).sum()+np.abs(delta).sum())*volume)
        expected_inventory = [float(a.sum()*volume) for a in (state0, state1, delta)]
        for key, value in zip(('beginning_inventory', 'ending_inventory', 'source_increment'), expected_inventory):
            enclosed(float(row[key]), value, magnitude, 32*cells, key)
        closure = expected_inventory[1]-expected_inventory[0]-expected_inventory[2]
        for side, name in enumerate(names):
            values = side_values[side][field] if field < 9 else side_values[side][1:8].sum(axis=0)
            incoming = float(np.maximum(-values, 0).sum()); outgoing = float(np.maximum(values, 0).sum())
            magnitude += float(np.abs(values).sum())
            enclosed(float(row[name+'_in']), incoming, magnitude, 32*cells+32*faces, name+' inflow')
            enclosed(float(row[name+'_out']), outgoing, magnitude, 32*cells+32*faces, name+' outflow')
            closure += outgoing-incoming
            if side == 4:
                for region, label in ((0, 'nonburner'), (1, 'burner')):
                    selected = values[footprint == region]
                    enclosed(float(row['bottom_'+label+'_in']), float(np.maximum(-selected, 0).sum()), magnitude, 32*cells+32*faces, label)
                    enclosed(float(row['bottom_'+label+'_out']), float(np.maximum(selected, 0).sum()), magnitude, 32*cells+32*faces, label)
        enclosed(float(row['closure_residual']), closure, magnitude, 64*cells+64*faces, 'scalar closure')
    columns = sealed(Path(str(base)+'.columns.csv'), case)
    require(hashlib.sha256(columns).hexdigest() == binding['columns_sha256'], 'column linkage')
    column_rows = list(csv.DictReader(io.StringIO(columns.decode())))
    require(len(column_rows) == 4*(5*nz+1), 'column coverage')
    first_m = first[18*cells:]
    expected_columns = set()
    for y in (ny//2-1, ny//2):
        for x in (nx//2-1, nx//2):
            for z in range(nz):
                for axis, fx, fy in ((0, x, y), (0, x+1, y), (1, x, y), (1, x, y+1)):
                    expected_columns.add((x, y, axis, fx, fy, z))
            for z in range(nz+1):
                expected_columns.add((x, y, 2, x, y, z))
    actual_columns = [(int(row['column_x']), int(row['column_y']), int(row['axis']),
                       int(row['x_face']), int(row['y_face']), int(row['z_face'])) for row in column_rows]
    require(len(set(actual_columns)) == len(actual_columns) and set(actual_columns) == expected_columns, 'fixed column coverage')
    for row in column_rows:
        axis, face = int(row['axis']), int(row['face']); packed = offsets[axis]+face
        require(0 <= axis < 3 and 0 <= face < counts[axis], 'column face bounds')
        require(face == int(row['x_face'])+(nx+(axis == 0))*(int(row['y_face'])+(ny+(axis == 1))*int(row['z_face'])), 'staggered face address')
        for key, value in (
            ('beginning_momentum_kg_per_m2_s', first_m[packed]),
            ('terminal_momentum_kg_per_m2_s', raw[9*cells+packed]),
            ('provisional_momentum_kg_per_m2_s', raw[9*cells+3*faces+packed]),
            ('advection_rate_kg_per_m2_s2', raw[9*cells+4*faces+packed]),
            ('buoyancy_rate_kg_per_m2_s2', raw[9*cells+5*faces+packed]),
            ('stress_rate_kg_per_m2_s2', raw[9*cells+6*faces+packed]),
            ('source_rate_kg_per_m2_s2', raw[9*cells+7*faces+packed])):
            require(float(row[key]) == float(value), 'column operand mismatch '+key)
        combined = (float(row['terminal_momentum_kg_per_m2_s'])-float(row['provisional_momentum_kg_per_m2_s']))/dt
        require(float(row['combined_projection_rate_kg_per_m2_s2']) == combined, 'combined projection mismatch')
        require(all(math.isfinite(float(value)) for value in row.values()), 'nonfinite column output')
        require(float(row['dt_s']) == dt and float(row['beginning_time_s']) == float(metadata['beginning_time_s']), 'column clock')
        total = (float(row['terminal_momentum_kg_per_m2_s'])-float(row['beginning_momentum_kg_per_m2_s']))/dt
        closure = total-(float(row['stress_rate_kg_per_m2_s2'])+float(row['buoyancy_rate_kg_per_m2_s2'])+
                         float(row['advection_rate_kg_per_m2_s2'])+float(row['source_rate_kg_per_m2_s2'])+combined)
        require(float(row['total_rate_kg_per_m2_s2']) == total and float(row['closure_residual_kg_per_m2_s2']) == closure,
                'column momentum closure')
        position = [int(row[key]) for key in ('x_face', 'y_face', 'z_face')]
        lower = position.copy(); upper = position.copy()
        lower[axis] = max(0, lower[axis]-1); upper[axis] = min(upper[axis], (nx, ny, nz)[axis]-1)
        for side, point in (('lower', lower), ('upper', upper)):
            cell = point[0]+nx*(point[1]+ny*point[2])
            require(int(row[side+'_cell']) == cell and float(row[side+'_vreman_m2_per_s']) ==
                    float(raw[12*cells+9*faces+cell]), 'column cell/eddy binding')

    return {'frame': frame, 'accepted_step': int(metadata['accepted_step']), 'flux_root': metadata['flux_root'],
            'native_root': metadata['native_root'], 'columns': len(column_rows)}


def source_components(metadata, key):
    values = [float(value) for value in metadata[key].split(',')]
    require(len(values) == 4 and all(math.isfinite(value) for value in values), 'source component layout')
    return values


def audit_native_history(run, case, records, provenance):
    launch = dict(line.split(' ', 1) for line in sealed(run/'window_launch.v1', case).decode().splitlines())
    require([launch[k] for k in ('case_record_id', 'producer_build_id', 'producer_executable_sha256')] == provenance,
            'launch producer mismatch')
    require(launch['from_zero'] == 'true' and launch['accepted_stage_budget'] == 'r245-v1' and
            launch['domain_study'] == 'r244-lateral-double-v1', 'launch observation contract')
    monitored = list(csv.DictReader(io.StringIO(sealed(run/'monitored_diagnostics.csv', case).decode())))
    require(len(monitored) == len(records), 'accepted history coverage')
    for index, (diagnostic, (meta, _)) in enumerate(zip(monitored, records), 1):
        require(int(diagnostic['step']) == index and float(diagnostic['dt_s']) == float(meta['dt_s']), 'accepted history timestep mismatch')


def audit(directory, fixture, completion=None):
    if not fixture:
        require(completion is not None and json.loads(completion.read_text())["exit_code"] == 0, "full audit requires successful completion receipt")
    paths = sorted(directory.glob('step_*.csv'))
    require(paths, 'missing accepted budgets')
    records = [read_record(path) for path in paths]
    case = records[0][0]['case_record_id']
    if not fixture:
        require(case == 'a9369e770465bd76e2cc8d5fc3b79e7c2aac038ecb14a0e4424a97ba2f98d586', 'changed wide-domain case')
    time = float(records[0][0]['beginning_time_s']) if fixture else 0.0
    provenance = [(records[0][0][k]) for k in ('case_record_id', 'producer_build_id', 'executable_sha256')]
    summaries = []
    statistics_begun = False
    for index, (path, (meta, rows)) in enumerate(zip(paths, records), 1):
        require(path.name == f'step_{index:06d}.csv' and int(meta['accepted_step']) == index, 'missing/duplicate step')
        sealed(path, case)
        require(meta['schema'] == 'rise.fire.accepted-stage-budget.v1', 'budget schema')
        require([meta[k] for k in ('case_record_id', 'producer_build_id', 'executable_sha256')] == provenance, 'mixed producers/cases')
        require(float(meta['beginning_time_s']) == time and math.isfinite(float(meta['dt_s'])) and
                float(meta['dt_s']) > 0 and math.isfinite(float(meta['statistics_start_s'])), 'step clock gap')
        time += float(meta['dt_s'])
        require(meta['dimensions'] == records[0][0]['dimensions'] and
                meta['binary32_solver_dx_m'] == records[0][0]['binary32_solver_dx_m'] and
                meta['statistics_start_s'] == records[0][0]['statistics_start_s'], 'changed geometry/statistics policy')
        start = float(meta['beginning_time_s']); dt = float(meta['dt_s']); boundary = float(meta['statistics_start_s'])
        fraction = max(0.0, start+dt-max(start, boundary))/dt
        for key, scale in (('cumulative_source', 1.0), ('statistics_source', fraction)):
            previous_components = source_components(records[index-2][0], key) if index > 1 else [0.0]*4
            for actual, old, increment in zip(source_components(meta, key), previous_components, source_components(meta, 'step_source')):
                enclosed(actual, old+scale*increment, abs(old)+abs(scale*increment), 16, key)
        for field in range(10):
            current = rows[field]; full = rows[10+field]
            previous = records[index-2][1][10+field] if index > 1 else None
            for key in current:
                if key in ('scope', 'field', 'units', 'beginning_inventory', 'ending_inventory', 'closure_residual', 'interstep_inventory_change'):
                    continue
                old = float(previous[key]) if previous else 0.0
                enclosed(float(full[key]), old+float(current[key]), abs(old)+abs(float(current[key])), 8, 'cumulative '+key)
            require(float(full['ending_inventory']) == float(current['ending_inventory']), 'cumulative ending inventory')
            require(float(full['beginning_inventory']) == float(records[0][1][field]['beginning_inventory']), 'cumulative beginning inventory')
            previous_stats = records[index-2][1][20+field] if index > 1 else None
            stats = rows[20+field]
            for key in current:
                if key in ('scope', 'field', 'units', 'beginning_inventory', 'ending_inventory', 'closure_residual', 'interstep_inventory_change'):
                    continue
                old = float(previous_stats[key]) if previous_stats else 0.0
                expected = old + fraction*float(current[key])
                enclosed(float(stats[key]), expected, abs(old)+abs(fraction*float(current[key])), 16, 'statistics '+key)
            require(float(current['interstep_inventory_change']) == 0, 'step interstep inventory must be zero')
            if fraction > 0:
                stats_begin = (float(previous_stats['beginning_inventory']) if statistics_begun else
                               float(current['ending_inventory'])-fraction*(float(current['ending_inventory'])-float(current['beginning_inventory'])))
                stats_end = float(current['ending_inventory'])
                stats_rebase = (float(previous_stats['interstep_inventory_change']) +
                                float(current['beginning_inventory'])-float(previous_stats['ending_inventory'])) if statistics_begun else 0.0
            else:
                stats_begin = stats_end = stats_rebase = 0.0
            for key, expected in (('beginning_inventory', stats_begin), ('ending_inventory', stats_end),
                                  ('interstep_inventory_change', stats_rebase)):
                magnitude = sum(abs(float(row[k])) for row in (current, previous_stats or current)
                                for k in ('beginning_inventory', 'ending_inventory', 'interstep_inventory_change'))
                enclosed(float(stats[key]), expected, magnitude, 32*index, 'statistics '+key)
            interstep = float(current['beginning_inventory'])-float(previous['ending_inventory']) if previous else 0.0
            previous_rebase = float(previous['interstep_inventory_change']) if previous else 0.0
            enclosed(float(full['interstep_inventory_change']), previous_rebase+interstep,
                     abs(previous_rebase)+abs(interstep), 8, 'interstep inventory')
            for label, row in (('step', current), ('full', full), ('statistics', stats)):
                expected_closure = float(row['ending_inventory'])-float(row['beginning_inventory'])-float(row['source_increment'])
                magnitude = sum(abs(float(row[k])) for k in ('ending_inventory', 'beginning_inventory', 'source_increment'))
                for side in ('xmin', 'xmax', 'ymin', 'ymax', 'bottom', 'top'):
                    expected_closure += float(row[side+'_out'])-float(row[side+'_in'])
                    magnitude += abs(float(row[side+'_out']))+abs(float(row[side+'_in']))
                enclosed(float(row['closure_residual']), expected_closure, magnitude, 128*index, label+' closure')
        statistics_begun = statistics_begun or fraction > 0

    frames = sorted(directory.glob('frame_*.v1'))
    for index, path in enumerate(frames):
        require(path.name == f'frame_{index:05d}.v1', 'missing budget frame')
        binding = dict(line.split(' ', 1) for line in path.read_text().splitlines())
        step = int(binding['accepted_step'])
        require(1 <= step <= len(records), 'foreign frame step')
        summaries.append(audit_capsule(directory, index, *records[step-1], case))
    if not fixture:
        run = directory.parent
        summary = dict(line.split('=', 1) for line in sealed(run/'production_capstone_summary.txt', case).decode().splitlines())
        require(summary['resolution_tier'] == '6' and summary['target_time_s'] == '25.156864278900095' and
                summary['statistics_start_s'] == '10.531864278900095' and summary['snapshot_cadence_s'] == '0.0625', 'altered full-window policy')
        require(int(summary['accepted_steps']) == len(records) and float(summary['accepted_time_s']) == time and
                time >= float(summary['target_time_s']) and len(frames) == 235, 'incomplete budget window')
        require(float(records[0][0]['statistics_start_s']) == float(summary['statistics_start_s']), 'budget statistics policy mismatch')
        audit_native_history(run, case, records, provenance)
        sealed(run/'production_final.checkpoint', case)
        times = list(csv.DictReader(io.StringIO(sealed(run/'frame_times.csv', case).decode())))
        require(len(times) == 235, 'frame time coverage')
        for frame, data in enumerate(summaries):
            meta = records[data['accepted_step']-1][0]
            require(float(times[frame]['simulation_time_s']) == float(meta['beginning_time_s'])+float(meta['dt_s']), 'budget/VDB time mismatch')
    return {'status': 'focused_fixture_only' if fixture else 'full_budget_bytes_verified_not_physical_qualification',
            'case': case, 'steps': len(records), 'end_time_s': time, 'frames': summaries,
            'audit_sha256': hashlib.sha256(Path(__file__).read_bytes()).hexdigest()}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('directory', type=Path)
    parser.add_argument('--fixture', action='store_true')
    parser.add_argument('--completion', type=Path)
    parser.add_argument('--output', type=Path, required=True)
    args = parser.parse_args()
    result = audit(args.directory, args.fixture, args.completion)
    args.output.write_text(json.dumps(result, indent=2, allow_nan=False)+'\n')
    print(f"PASS {result['status']}: {result['steps']} steps, {len(result['frames'])} frames")


if __name__ == '__main__':
    main()
