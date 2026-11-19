#!/usr/bin/env python3
"""GREEN/RED controls using copied, real focused-owner capsules (never empirical)."""
import argparse
import csv
import hashlib
import io
import json
from pathlib import Path
import shutil
import struct
import tempfile

from r245_budget_audit import audit, audit_native_history, merkle, read_record


def reseal(path, case):
    payload = path.read_bytes()
    Path(str(path)+'.payload-v2.json').write_text(json.dumps({
        'schema': 'rise.fire.published-payload.v2', 'case_record_id': case,
        'sha256_v1': hashlib.sha256(payload).hexdigest(), 'v2': merkle(payload)})+'\n')


def rebind(directory, case, flux_root=None):
    step = directory/'step_000001.csv'; frame = directory/'frame_00000.v1'
    if flux_root:
        lines = step.read_text().splitlines()
        step.write_text('\n'.join('# flux_root '+flux_root if line.startswith('# flux_root ') else line for line in lines)+'\n')
    reseal(step, case)
    binding = dict(line.split(' ', 1) for line in frame.read_text().splitlines())
    binding['step_record_sha256'] = hashlib.sha256(step.read_bytes()).hexdigest()
    for suffix in ('.flux.bin', '.native.bin', '.beginning.bin'):
        payload = directory/('frame_00000'+suffix)
        reseal(payload, case)
        binding[suffix] = hashlib.sha256(payload.read_bytes()).hexdigest()
    columns = directory/'frame_00000.columns.csv'; reseal(columns, case)
    binding['columns_sha256'] = hashlib.sha256(columns.read_bytes()).hexdigest()
    frame.write_text(''.join(f'{key} {value}\n' for key, value in binding.items()))
    reseal(frame, case)


def mutate_rows(directory):
    path = directory/'step_000001.csv'; text = path.read_text()
    comments = [line for line in text.splitlines() if line.startswith('#')]
    _, rows = read_record(path)
    keys = ['xmin_in', 'xmax_in', 'ymin_in', 'ymax_in', 'bottom_in', 'top_in']
    choice = next((field, a, b) for field in range(10) for a in keys for b in keys
                  if float(rows[field][a]) != float(rows[field][b]))
    field, a, b = choice
    for offset in (0, 10, 20):
        row = rows[offset+field]; row[a], row[b] = row[b], row[a]
    stream = io.StringIO(); writer = csv.DictWriter(stream, fieldnames=list(rows[0]), lineterminator='\n')
    writer.writeheader(); writer.writerows(rows)
    path.write_text('\n'.join(comments)+'\n'+stream.getvalue())


def clipped_ledger_control(directory, fixture):
    """Hand-calculated three-event ledger: half of event 2, then event 3.

    Synthetic arithmetic fixture only; no native frame or empirical claim.
    Inventories change by 3 and 2 between events. This catches first-clip and
    subsequent interstep accounting independently of native reducer fixtures.
    """
    directory.mkdir()
    metadata, template = read_record(fixture/'step_000001.csv')
    case = metadata['case_record_id']
    cases = [((10, 12, 1, 1, 0, 0), (10, 12, 1, 1, 0, 0), (0, 0, 0, 0, 0, 0)),
             ((15, 19, 2, 2, 0, 0), (10, 19, 3, 3, 3, 3), (17, 19, 1, 1, 0, 0)),
             ((21, 27, 3, 3, 0, 0), (10, 27, 6, 6, 5, 5), (17, 27, 4, 4, 2, 2))]
    for index, scopes in enumerate(cases, 1):
        meta = dict(metadata, accepted_step=str(index), beginning_time_s=str(index-1), dt_s='1', statistics_start_s='1.5',
                    step_source='1,1,1,1', cumulative_source=','.join([str(index)]*4),
                    statistics_source=','.join([str((0, .5, 1.5)[index-1])]*4))
        rows = []
        for scope, values in zip(('step', 'full', 'statistics'), scopes):
            for field in range(10):
                row = {key: '0' for key in template[0]}
                row.update(scope=scope, field=str(field), units='J' if field == 8 else 'kg')
                for key, value in zip(('beginning_inventory', 'ending_inventory', 'source_increment',
                                       'bottom_in', 'closure_residual', 'interstep_inventory_change'), values):
                    row[key] = str(value)
                row['bottom_nonburner_in'] = row['bottom_in']
                rows.append(row)
        stream = io.StringIO(); writer = csv.DictWriter(stream, fieldnames=list(rows[0]), lineterminator='\n')
        writer.writeheader(); writer.writerows(rows)
        path = directory/f'step_{index:06d}.csv'
        path.write_text(''.join(f'# {key} {value}\n' for key, value in meta.items())+stream.getvalue()); reseal(path, case)
    audit(directory, True)
    return case, [read_record(path) for path in sorted(directory.glob('step_*.csv'))]


def native_history_controls(directory, case, records):
    meta = records[0][0]
    provenance = [meta[k] for k in ('case_record_id', 'producer_build_id', 'executable_sha256')]
    launch = dict(zip(('case_record_id', 'producer_build_id', 'producer_executable_sha256'), provenance))
    launch.update(from_zero='true', accepted_stage_budget='r245-v1', domain_study='r244-lateral-double-v1')
    launch_path = directory/'window_launch.v1'; history_path = directory/'monitored_diagnostics.csv'
    results = []
    for name in ('native_history_green', 'native_history_dt_swap', 'native_history_producer'):
        current = dict(launch)
        if name == 'native_history_producer':
            current['producer_executable_sha256'] = '0'*64
        launch_path.write_text(''.join(f'{key} {value}\n' for key, value in current.items())); reseal(launch_path, case)
        # Same count and endpoint, changed internal accepted history.
        steps = [1.125, .875, 1] if name == 'native_history_dt_swap' else [1, 1, 1]
        history_path.write_text('step,dt_s\n'+''.join(f'{index},{dt}\n' for index, dt in enumerate(steps, 1))); reseal(history_path, case)
        try:
            audit_native_history(directory, case, records, provenance)
            accepted = True; reason = ''
        except ValueError as error:
            accepted = False; reason = str(error)
        expected = {'native_history_dt_swap': 'accepted history timestep mismatch',
                    'native_history_producer': 'launch producer mismatch'}
        if accepted != (name == 'native_history_green') or (name in expected and expected[name] not in reason):
            raise RuntimeError(f'{name}: {accepted}, {reason}')
        results.append({'control': name, 'accepted': accepted, 'reason': reason, 'scope': 'synthetic_native_history_binding'})
    return results


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('fixture', type=Path)
    parser.add_argument('--output', type=Path, required=True)
    args = parser.parse_args()
    case = read_record(args.fixture/'step_000001.csv')[0]['case_record_id']
    results = []
    with tempfile.TemporaryDirectory(prefix='r245-budget-red-', dir='/Volumes/SSD/RISE/fire_production_calibration') as temporary:
        synthetic = Path(temporary)/'synthetic_clipped_ledger'
        synthetic_case, synthetic_records = clipped_ledger_control(synthetic, args.fixture)
        results.extend(native_history_controls(synthetic, synthetic_case, synthetic_records))
        results.append({'control': 'synthetic_clipped_ledger', 'accepted': True, 'scope': 'hand_calculated_nonempirical'})
        for name in ('copied_green', 'side_swap', 'flux_sign', 'missing_stage_terms', 'column_term', 'rejected_clock', 'statistics_inventory', 'statistics_closure', 'statistics_interstep', 'cumulative_source', 'statistics_source', 'step_source_operands'):
            directory = Path(temporary)/name
            shutil.copytree(args.fixture, directory)
            flux_root = None
            if name == 'side_swap':
                mutate_rows(directory)
            elif name in ('statistics_inventory', 'statistics_closure', 'statistics_interstep'):
                path = directory/'step_000001.csv'
                comments = [line for line in path.read_text().splitlines() if line.startswith('#')]
                _, rows = read_record(path)
                key = {'statistics_inventory': 'beginning_inventory', 'statistics_closure': 'closure_residual',
                       'statistics_interstep': 'interstep_inventory_change'}[name]
                rows[28][key] = '123456789'
                stream = io.StringIO(); writer = csv.DictWriter(stream, fieldnames=list(rows[0]), lineterminator='\n')
                writer.writeheader(); writer.writerows(rows)
                path.write_text('\n'.join(comments)+'\n'+stream.getvalue())
            elif name in ('cumulative_source', 'statistics_source', 'step_source_operands'):
                path = directory/'step_000001.csv'
                keys = ('step_source', 'cumulative_source', 'statistics_source') if name == 'step_source_operands' else (name,)
                # The operand RED keeps all three ledgers mutually consistent;
                # only comparison with retained frozen-source doubles can refuse it.
                path.write_text('\n'.join('# '+key+' 123456789,123456789,123456789,123456789'
                                if (key := line[2:].split(' ', 1)[0]) in keys else line
                                for line in path.read_text().splitlines())+'\n')
            elif name in ('flux_sign', 'missing_stage_terms'):
                path = directory/'frame_00000.flux.bin'; payload = bytearray(path.read_bytes())
                if name == 'missing_stage_terms':
                    del payload[-4:]
                else:
                    metadata, _ = read_record(directory/'step_000001.csv')
                    nx, ny, nz = map(int, metadata['dimensions'].split())
                    faces = (nx+1)*ny*nz+nx*(ny+1)*nz+nx*ny*(nz+1)
                    # Find a nonzero low flux at an x boundary in any species.
                    choices = [field*faces+(z*ny+y)*(nx+1)+x for field in range(1, 8)
                               for z in range(nz) for y in range(ny) for x in (0, nx)]
                    index = next(i for i in choices if struct.unpack_from('<f', payload, 72+4*i)[0] != 0)
                    value = struct.unpack_from('<f', payload, 72+4*index)[0]
                    struct.pack_into('<f', payload, 72+4*index, -value)
                path.write_bytes(payload); flux_root = merkle(payload)['root_sha256']
            elif name == 'column_term':
                path = directory/'frame_00000.columns.csv'
                rows = list(csv.DictReader(io.StringIO(path.read_text())))
                rows[0]['stress_rate_kg_per_m2_s2'] = str(float(rows[0]['stress_rate_kg_per_m2_s2'])+1)
                stream = io.StringIO(); writer = csv.DictWriter(stream, fieldnames=list(rows[0]), lineterminator='\n')
                writer.writeheader(); writer.writerows(rows); path.write_text(stream.getvalue())
            rebind(directory, case, flux_root)
            if name == 'rejected_clock':
                path = directory/'step_000002.csv'
                path.write_text((directory/'step_000001.csv').read_text().replace('# accepted_step 1\n', '# accepted_step 2\n'))
                reseal(path, case)
            try:
                audit(directory, True)
                accepted = True; reason = ''
            except (ValueError, AssertionError) as error:
                accepted = False; reason = str(error)
            expected = {'side_swap': 'inflow', 'flux_sign': 'inflow', 'missing_stage_terms': 'capsule layout',
                        'column_term': 'column operand', 'rejected_clock': 'clock gap', 'statistics_inventory': 'statistics beginning_inventory',
                        'statistics_closure': 'statistics closure', 'statistics_interstep': 'statistics interstep',
                        'cumulative_source': 'cumulative_source', 'statistics_source': 'statistics_source',
                        'step_source_operands': 'frozen source component'}
            if accepted != (name == 'copied_green') or (name in expected and expected[name] not in reason):
                raise RuntimeError(f'{name}: unexpected accepted={accepted}, reason={reason}')
            results.append({'control': name, 'accepted': accepted, 'reason': reason, 'all_mutated_seals_recomputed': True})
    args.output.write_text(json.dumps({'scope': 'synthetic_mutations_of_real_focused_owner_capsules', 'results': results,
                                      'gate_sha256': hashlib.sha256(Path(__file__).read_bytes()).hexdigest()}, indent=2)+'\n')
    print(f'PASS {sum(row["accepted"] for row in results)} GREENs and {sum(not row["accepted"] for row in results)} targeted REDs')


if __name__ == '__main__':
    main()
