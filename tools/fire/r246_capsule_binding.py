#!/usr/bin/env python3
"""Strict full235 capsule byte/root/event binding, not budget arithmetic or recovery acceptance."""
import argparse
import copy
import json
from pathlib import Path
import struct

import r246_observation_ledger_binding as dependency

LEDGER_REPORT = '3a27bc90f840e12b98303104654d87acba4a59ce7448b65140d6c46bea0fc19c'
DEPENDENCY = '8996e94d88ddede087b5d73a75fbbdb29f721803cee46a8f0af14bd566cc70c7'
FRAME_CSV = '3bb265a8539d5de45356dba419024c077834e3bb4c3770d75c63478006a5a5c8'
R = None


def require(ok, message):
    if not ok:
        raise ValueError(message)


def text_record(raw):
    pairs = [x.split(' ', 1) for x in raw.decode().splitlines()]
    require(all(len(x) == 2 for x in pairs) and len({x[0] for x in pairs}) == len(pairs),
            'ambiguous capsule binding')
    return dict(pairs)


def validate_capsule(frame, step, ledger_sha, metadata, binding, packets, columns, dimensions):
    require(binding['schema'] == 'rise.fire.accepted-stage-frame.v1' and
            int(binding['frame']) == frame and int(binding['accepted_step']) == step and
            binding['step_record_sha256'] == ledger_sha, 'capsule event/ledger linkage')
    for suffix, raw in packets.items():
        require(R['sha'](raw) == binding[suffix], 'capsule payload linkage: ' + suffix)
    require(R['sha'](columns) == binding['columns_sha256'], 'capsule columns linkage')
    nx, ny, nz = dimensions
    cells = nx*ny*nz
    faces = (nx+1)*ny*nz + nx*(ny+1)*nz + nx*ny*(nz+1)
    native, flux, beginning = (packets[x] for x in ('.native.bin', '.flux.bin', '.beginning.bin'))
    require(len(native) == (19*cells+9*faces)*4+216 and len(flux) == 72+26*faces*4 and
            len(beginning) == (28*cells+faces)*4+nx*ny, 'capsule binary layout')
    require(R['merkle'](native)['root_sha256'] == metadata['native_root'] and
            R['merkle'](flux)['root_sha256'] == metadata['flux_root'], 'capsule recorded roots')
    require(flux[:32].hex() == metadata['input_root'] == native[-32:].hex() and
            flux[32:64].hex() == metadata['native_root'] and
            struct.unpack_from('<Q', flux, 64)[0] == int(metadata['R1_candidate']),
            'capsule input/native/candidate authority')
    ids = struct.unpack_from('<22Q', native, (19*cells+9*faces)*4+8)
    require(ids[10] == int(metadata['R1_candidate']) and ids[21] != 0, 'native candidate identity')


def controls():
    # Private byte-only packet controls, never empirical admission or fixture-mode full audit.
    native = bytearray((19+9*6)*4+216)
    ids = [0]*22
    ids[10], ids[21] = 7, 1
    struct.pack_into('<22Q', native, (19+9*6)*4+8, *ids)
    input_root = 'a'*64
    native[-32:] = bytes.fromhex(input_root)
    native = bytes(native)
    native_root = R['merkle'](native)['root_sha256']
    flux = bytes.fromhex(input_root+native_root) + struct.pack('<Q', 7) + bytes(26*6*4)
    packets = {'.native.bin': native, '.flux.bin': flux, '.beginning.bin': bytes((28+6)*4+1)}
    metadata = dict(input_root=input_root, native_root=native_root,
                    flux_root=R['merkle'](flux)['root_sha256'], R1_candidate='7')
    columns = b'private byte control columns\n'
    binding = dict(schema='rise.fire.accepted-stage-frame.v1', frame='0', accepted_step='1',
                   step_record_sha256='b'*64, columns_sha256=R['sha'](columns))
    binding.update({key: R['sha'](raw) for key, raw in packets.items()})
    args = [0, 1, 'b'*64, metadata, binding, packets, columns, (1, 1, 1)]
    validate_capsule(*args)
    mutations = [(4, 'accepted_step', '2'), (4, 'frame', '1'), (4, 'step_record_sha256', 'c'*64),
                 (4, 'columns_sha256', 'c'*64), (3, 'input_root', 'c'*64),
                 (3, 'native_root', 'c'*64), (3, 'flux_root', 'c'*64), (3, 'R1_candidate', '8')]
    for slot, key, value in mutations:
        sample = copy.deepcopy(args)
        sample[slot][key] = value
        try:
            validate_capsule(*sample)
        except ValueError:
            continue
        raise ValueError('false packet GREEN: ' + key)
    payload_reds = 0
    for key in packets:
        sample = copy.deepcopy(args)
        sample[5][key] += b'x'
        sample[4][key] = R['sha'](sample[5][key])  # Consistent binding still cannot hide wrong layout.
        try:
            validate_capsule(*sample)
        except ValueError:
            payload_reds += 1
        else:
            raise ValueError('false layout GREEN: ' + key)
    return dict(byte_only_green=1, binding_reds=len(mutations), resealed_layout_reds=payload_reds,
                empirical=False)


def main():
    global R
    paths = {'script': Path(__file__).resolve(), 'ledger_helper': Path(dependency.__file__).resolve(),
             'merkle_verifier': Path(dependency.fire_payload_merkle.__file__).resolve()}
    captured = dependency.implementation_snapshot(paths)
    require(dependency.sha(captured['ledger_helper']) == DEPENDENCY, 'accepted helper identity')
    helper = {'__name__': 'r246_captured_helper', '__file__': str(paths['ledger_helper'])}
    exec(compile(captured['ledger_helper'], str(paths['ledger_helper']), 'exec'), helper)
    dependency.require_loaded_functions_match(dependency.__dict__, helper)
    own = {'__name__': 'r246_captured_capsule', '__file__': str(paths['script'])}
    exec(compile(captured['script'], str(paths['script']), 'exec'), own)
    dependency.require_loaded_functions_match(globals(), own)
    require(all(globals()[key] == own[key] for key in ('LEDGER_REPORT', 'DEPENDENCY', 'FRAME_CSV')),
            'loaded capsule constants')
    verifier = {'__name__': 'r246_captured_merkle', '__file__': str(paths['merkle_verifier'])}
    exec(compile(captured['merkle_verifier'], str(paths['merkle_verifier']), 'exec'), verifier)
    helper.update({name: verifier[name] for name in ('FORMAT', 'merkle', 'verify')})
    R = helper
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('--controls', action='store_true')
    for name in ('simulation', 'ledger_binding', 'output'):
        p.add_argument('--'+name.replace('_', '-'), type=Path)
    a = p.parse_args()
    if a.controls:
        require(not any((a.simulation, a.ledger_binding, a.output)), 'controls cannot admit full evidence')
        print(json.dumps(controls()))
        return
    require(all((a.simulation, a.ledger_binding, a.output)), 'full native paths required')
    report_raw = R['sealed'](a.ledger_binding/'ledger_binding.json')
    require(R['sha'](report_raw) == LEDGER_REPORT, 'accepted full ledger binding identity')
    history = json.loads(report_raw)
    require(history['accepted_steps'] == 15315 and history['recovery_accepted'] is False and
            history['original_exit_code'] == 98 and history['source_case_record_id'] == R['CASE'] and
            history['source_producer_build_id'] == R['PRODUCER'], 'ledger history authority')
    records = history['ledger_bindings']
    require(len(records) == 15315 and [x['step'] for x in records] == list(range(1, 15316)),
            'full ledger coverage')
    csv_raw = (a.simulation/'frame_times.csv').read_bytes()
    require(R['sha'](csv_raw) == FRAME_CSV, 'original frame/event snapshot')
    frames = R['rows'](csv_raw)
    require(len(frames) == 235 and [int(x['frame']) for x in frames] == list(range(235)), 'full frame coverage')
    budget = a.simulation/'budgets'
    # Map sealed selected ledger endpoints to the exact already-accepted frame events.
    bindings = []
    for index, frame in enumerate(frames):
        base = budget/f'frame_{index:05d}'
        raw_binding = R['sealed'](Path(str(base)+'.v1'))
        binding = text_record(raw_binding)
        step = int(binding['accepted_step'])
        require(1 <= step <= 15315, 'selected accepted step')
        record = records[step-1]
        ledger_raw = R['sealed'](budget/f'step_{step:06d}.csv')
        require(R['sha'](ledger_raw) == record['ledger_sha256'], 'selected ledger identity')
        metadata = R['ledger_metadata'](ledger_raw)
        require(metadata['case_record_id'] == R['CASE'] and metadata['producer_build_id'] == R['PRODUCER'] and
                metadata['executable_sha256'] == R['EXECUTABLE'] and metadata['dimensions'] == '104 104 160' and
                int(metadata['accepted_step']) == step and
                float(metadata['beginning_time_s'])+float(metadata['dt_s']) == float(frame['simulation_time_s']) and
                all(metadata[key] == record[key] for key in ('input_root', 'native_root', 'flux_root')),
                'selected ledger domain/event/roots')
        packets = {suffix: R['sealed'](Path(str(base)+suffix))
                   for suffix in ('.native.bin', '.flux.bin', '.beginning.bin')}
        columns = R['sealed'](Path(str(base)+'.columns.csv'))
        validate_capsule(index, step, record['ledger_sha256'], metadata, binding, packets, columns, (104, 104, 160))
        bindings.append(dict(frame=index, accepted_step=step, binding_sha256=R['sha'](raw_binding),
                             payload_sha256={key: R['sha'](raw) for key, raw in packets.items()},
                             columns_sha256=R['sha'](columns), native_root=metadata['native_root'],
                             flux_root=metadata['flux_root'], input_root=metadata['input_root']))
        print('R246_CAPSULE_BOUND frame='+str(index), flush=True)
    dependency.require_implementation_unchanged(paths, captured)
    a.output.mkdir(exist_ok=False)
    identity = {name: R['sha'](raw) for name, raw in captured.items()}
    report = dict(schema='rise.fire.r246.capsule-binding.v1', recovery_accepted=False, original_exit_code=98,
                  source_case_record_id=R['CASE'], source_producer_build_id=R['PRODUCER'],
                  implementation_sha256=identity, ledger_binding_sha256=LEDGER_REPORT,
                  frame_csv_sha256=FRAME_CSV, frames=bindings,
                  limitations=['Byte/root/event binding only; signed per-side budget arithmetic remains pending.',
                               'No new authentication of originally unsealed observation measurements.',
                               'Full recovery, consumer repair and physical interpretation remain pending.'])
    raw = (json.dumps(report, indent=2)+'\n').encode()
    (a.output/'capsule_binding.json').write_bytes(raw)
    cert = dict(schema='rise.fire.published-payload.v2', case_record_id=R['CASE'], sha256_v1=R['sha'](raw),
                v2=R['merkle'](raw), recovery_implementation_sha256=identity,
                provenance='new recovery issuer; not an original producer certificate')
    (a.output/'capsule_binding.json.payload-v2.json').write_text(json.dumps(cert)+'\n')
    for name, raw in captured.items():
        (a.output/(name+'.frozen.py')).write_bytes(raw)
    (a.output/'completion.json').write_text(json.dumps(dict(exit_code=0, recovery_accepted=False,
                                                          implementation_sha256=identity))+'\n')


if __name__ == '__main__':
    main()
