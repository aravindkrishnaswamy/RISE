#!/usr/bin/env python3
"""Bind retained observation clocks/roots to sealed ledgers; never recovery acceptance.

Non-clock host/velocity measurements remain newly captured, originally unsealed
observations. This prerequisite does not verify capsule payloads or budget arithmetic.
"""
import argparse
import copy
import csv
import hashlib
import io
import json
import marshal
from pathlib import Path
import sys
import tempfile
import types

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
import fire_payload_merkle
from fire_payload_merkle import FORMAT, merkle, verify

CASE = '805372d567af6fafd03ba0ec96c8fbd30d64fdc21c64117dc2ea5580be9b52a2'
PRODUCER = '2e19f9326f6987bf577a7485b4aee92d3ebb6cb1212d4fe0b2df42f1db2b371b'
EXECUTABLE = '56189ad7e4853e3195de6ea24086e76273f1209209ef2fd2a30632ccf8535430'
PINS = {
    'metadata': '81df738946e1f8537a7845554fd1b81158dd0b6303fd3617ead2e1a64a4fce33',
    'velocity': '5e8b6b06360c8557e336a0edefd43527b39c880cc2cb7af85ecf4e07f328754a',
    'host': '1d9232934e5a43b603de232634c0b436127318252ae53c0509c02ee7d9778e08',
}


def require(ok, message):
    if not ok:
        raise ValueError(message)


def sha(raw):
    return hashlib.sha256(raw).hexdigest()


def implementation_snapshot(paths):
    return {name: path.read_bytes() for name, path in paths.items()}


def require_implementation_unchanged(paths, captured):
    require(implementation_snapshot(paths) == captured, 'recovery implementation changed during binding')


def constant_identity(value):
    if isinstance(value, types.FunctionType):
        return ('function', code_identity(value.__code__), constant_identity(value.__defaults__),
                constant_identity(value.__kwdefaults__))
    if isinstance(value, dict):
        return ('dict', tuple(sorted(((constant_identity(k), constant_identity(v))
                                     for k, v in value.items()), key=repr)))
    if isinstance(value, types.CodeType):
        return ('code', code_identity(value))
    if isinstance(value, tuple):
        return ('tuple', tuple(constant_identity(x) for x in value))
    if isinstance(value, frozenset):
        return ('frozenset', tuple(sorted((constant_identity(x) for x in value), key=repr)))
    return ('scalar', marshal.dumps(value))


def code_identity(code):
    constants = tuple(constant_identity(x) for x in code.co_consts)
    return (code.co_argcount, code.co_posonlyargcount, code.co_kwonlyargcount, code.co_flags,
            code.co_code, constants, code.co_names, code.co_varnames, code.co_freevars,
            code.co_cellvars, getattr(code, 'co_exceptiontable', b''))


def require_loaded_functions_match(loaded, captured_namespace):
    for name, value in loaded.items():
        if isinstance(value, types.FunctionType) and value.__module__ == loaded['__name__']:
            candidate = captured_namespace.get(name)
            require(isinstance(candidate, types.FunctionType) and
                    constant_identity(value) == constant_identity(candidate),
                    'loaded implementation differs from captured source: ' + name)


def bind_captured_implementation(paths, captured):
    namespace = {'__name__': 'r246_captured_script', '__file__': str(paths['script'])}
    exec(compile(captured['script'], str(paths['script']), 'exec'), namespace)
    require_loaded_functions_match(globals(), namespace)
    require(all(globals()[name] == namespace[name] for name in ('CASE', 'PRODUCER', 'EXECUTABLE', 'PINS')),
            'loaded script constants differ from captured source')
    verifier = {'__name__': 'r246_captured_merkle', '__file__': str(paths['merkle_verifier'])}
    exec(compile(captured['merkle_verifier'], str(paths['merkle_verifier']), 'exec'), verifier)
    # Validation executes these captured bytes, never a previously imported cache.
    globals().update({name: verifier[name] for name in ('FORMAT', 'merkle', 'verify')})


def sealed(path, after_capture=None):
    certificate = Path(str(path) + '.payload-v2.json')
    pending = Path(str(path) + '.preparation.pending.payload-v2.json')

    def admitted():
        return certificate.is_file() and not certificate.is_symlink() and not (
            pending.exists() or pending.is_symlink())

    require(admitted(), 'mandatory complete nonaliased seal: ' + str(path))
    raw, cert_raw = path.read_bytes(), certificate.read_bytes()
    if after_capture is not None:
        after_capture()
    require(admitted(), 'publication changed during capture: ' + str(path))
    cert = json.loads(cert_raw)
    require(cert['schema'] == 'rise.fire.published-payload.v2' and
            cert['case_record_id'] == CASE and cert['sha256_v1'] == sha(raw) and
            cert['v2'].get('digest_format') == FORMAT and verify(raw, cert['v2']),
            'payload seal: ' + str(path))
    return raw


def rows(raw):
    return list(csv.DictReader(io.StringIO(raw.decode())))


def ledger_metadata(raw):
    pairs = [line[2:].split(' ', 1) for line in raw.decode().splitlines() if line.startswith('# ')]
    require(all(len(x) == 2 for x in pairs) and len({x[0] for x in pairs}) == len(pairs),
            'ambiguous ledger metadata')
    return dict(pairs)


def bind_step(index, dt_row, velocity, host, ledger):
    require(ledger['schema'] == 'rise.fire.accepted-stage-budget.v1' and
            ledger['case_record_id'] == CASE and ledger['producer_build_id'] == PRODUCER and
            ledger['executable_sha256'] == EXECUTABLE and ledger['dimensions'] == '104 104 160' and
            float(ledger['binary32_solver_dx_m']) == 0.040790822356939316 and
            float(ledger['statistics_start_s']) == 10.531864278900095, 'ledger authority/domain')
    require(int(dt_row['step']) == int(velocity['accepted_step']) == int(host['accepted_step']) ==
            int(ledger['accepted_step']) == index, 'step ordinal')
    dt, beginning, end = (float(dt_row[k]) for k in ('dt_s', 'beginning_time_s', 'time_s'))
    require(dt > 0 and beginning + dt == end and float(ledger['dt_s']) == dt ==
            float(velocity['dt_s']) and float(ledger['beginning_time_s']) == beginning and
            float(velocity['time_s']) == float(host['time_s']) == end, 'accepted clock')
    require(velocity['input_payload_root_sha256'] == ledger['input_root'] and
            velocity['publication_payload_root_sha256'] == ledger['native_root'] and
            velocity['payload_digest_format'] == 'rise-payload-sha256-merkle' and
            int(velocity['payload_digest_version']) == 2 and
            int(velocity['publication_payload_bytes']) == 320010712, 'observation roots/layout')
    require(int(host['host_ceiling_bytes']) == 34359738368 and
            int(host['next_step_reserve_bytes']) == 23930269536 and int(host['stop']) == 0,
            'observed host contract')
    return end


def controls():
    # Hand-authored clocks, not an empirical run or a full-mode admission exception.
    ledger = dict(schema='rise.fire.accepted-stage-budget.v1', case_record_id=CASE,
                  producer_build_id=PRODUCER, executable_sha256=EXECUTABLE,
                  dimensions='104 104 160', binary32_solver_dx_m='0.040790822356939316',
                  statistics_start_s='10.531864278900095', accepted_step='1', dt_s='0.5',
                  beginning_time_s='0', input_root='a'*64, native_root='b'*64)
    dt = dict(step='1', beginning_time_s='0', time_s='0.5', dt_s='0.5')
    velocity = dict(accepted_step='1', time_s='0.5', dt_s='0.5', input_payload_root_sha256='a'*64,
                    publication_payload_root_sha256='b'*64, payload_digest_format='rise-payload-sha256-merkle',
                    payload_digest_version='2', publication_payload_bytes='320010712')
    host = dict(accepted_step='1', time_s='0.5', host_ceiling_bytes='34359738368',
                next_step_reserve_bytes='23930269536', stop='0')
    original = [dt, velocity, host, ledger]
    require(bind_step(1, *original) == 0.5, 'hand-authored GREEN')
    mutations = [(3, 'producer_build_id', 'foreign'), (3, 'dimensions', '104 104 80'),
                 (3, 'dt_s', '0.25'), (3, 'beginning_time_s', '0.25'),
                 (1, 'publication_payload_root_sha256', 'c'*64), (1, 'accepted_step', '2'),
                 (2, 'time_s', '0.25'), (2, 'stop', '1'), (2, 'next_step_reserve_bytes', '24292928552')]
    for slot, key, value in mutations:
        sample = copy.deepcopy(original)
        sample[slot][key] = value
        try:
            bind_step(1, *sample)
        except ValueError:
            continue
        raise ValueError('false GREEN: ' + key)
    seal_reds = 0
    with tempfile.TemporaryDirectory(prefix='r246-ledger-seal-controls-') as tmp:
        path = Path(tmp) / 'operand'
        cert = Path(str(path) + '.payload-v2.json')
        pending = Path(str(path) + '.preparation.pending.payload-v2.json')
        raw = b'hand-authored seal control\n'
        good = json.dumps(dict(schema='rise.fire.published-payload.v2', case_record_id=CASE,
                               sha256_v1=sha(raw), v2=merkle(raw)))
        modes = ('missing', 'corrupt_payload', 'foreign_case', 'regular_pending',
                 'dangling_pending', 'aliased_certificate', 'float_digest_version',
                 'float_chunk_bytes', 'float_fan_in', 'float_payload_bytes',
                 'after_regular_pending', 'after_dangling_pending', 'after_aliased_certificate')
        for mode in modes:
            for marker in (cert, pending):
                if marker.exists() or marker.is_symlink():
                    marker.unlink()
            path.write_bytes(raw)
            cert.write_text(good)
            require(sealed(path) == raw, 'seal GREEN')
            def invalidate_after():
                if mode == 'after_regular_pending':
                    pending.write_text('pending')
                elif mode == 'after_dangling_pending':
                    pending.symlink_to(Path(tmp) / 'absent')
                elif mode == 'after_aliased_certificate':
                    target = Path(tmp) / 'real_certificate'
                    target.write_text(good)
                    cert.unlink()
                    cert.symlink_to(target)

            if mode.startswith('float_'):
                bad = json.loads(good)
                field = mode[len('float_'):]
                bad['v2'][field] = float(bad['v2'][field])
                cert.write_text(json.dumps(bad))
            elif mode.startswith('after_'):
                pass
            elif mode == 'missing':
                cert.unlink()
            elif mode == 'corrupt_payload':
                path.write_bytes(raw + b'x')
            elif mode == 'foreign_case':
                bad = json.loads(good)
                bad['case_record_id'] = 'foreign'
                cert.write_text(json.dumps(bad))
            elif mode == 'regular_pending':
                pending.write_text('pending')
            elif mode == 'dangling_pending':
                pending.symlink_to(Path(tmp) / 'absent')
            else:
                target = Path(tmp) / 'real_certificate'
                target.write_text(good)
                cert.unlink()
                cert.symlink_to(target)
            try:
                sealed(path, invalidate_after if mode.startswith('after_') else None)
            except ValueError:
                seal_reds += 1
                continue
            raise ValueError('false seal GREEN: ' + mode)
    identity_reds = 0
    with tempfile.TemporaryDirectory(prefix='r246-issuer-controls-') as tmp:
        paths = {name: Path(tmp) / name for name in ('script', 'merkle_verifier')}
        for path in paths.values():
            path.write_bytes(b'original implementation')
        captured = implementation_snapshot(paths)
        require_implementation_unchanged(paths, captured)
        for name, path in paths.items():
            path.write_bytes(b'replacement implementation')
            try:
                require_implementation_unchanged(paths, captured)
            except ValueError:
                identity_reds += 1
            else:
                raise ValueError('false issuer GREEN: ' + name)
            path.write_bytes(captured[name])
        module_path = Path(tmp) / 'loaded_control.py'
        module_path.write_text('def value():\n    return "A"\n')
        loaded = {'__name__': 'loaded_control'}
        exec(compile(module_path.read_bytes(), str(module_path), 'exec'), loaded)
        module_path.write_text('def value():\n    return "B"\n')
        replacement = {'__name__': 'replacement_control'}
        exec(compile(module_path.read_bytes(), str(module_path), 'exec'), replacement)
        require(loaded['value']() == 'A' and replacement['value']() == 'B', 'actual staged module load')
        try:
            require_loaded_functions_match(loaded, replacement)
        except ValueError:
            identity_reds += 1
        else:
            raise ValueError('false loaded/source identity GREEN')
        loaded_default = {'__name__': 'loaded_default'}
        captured_default = {'__name__': 'captured_default'}
        exec('def value(x=None, *, y=None):\n    return (x, y)\n', loaded_default)
        for definition in ('def value(x=False, *, y=None):\n    return (x, y)\n',
                           'def value(x=None, *, y=False):\n    return (x, y)\n',
                           'def value(x=lambda: False, *, y=None):\n    return (x, y)\n'):
            exec(definition, captured_default)
            try:
                require_loaded_functions_match(loaded_default, captured_default)
            except ValueError:
                identity_reds += 1
            else:
                raise ValueError('false loaded/source default identity GREEN')
    return dict(hand_authored_green=1, targeted_reds=len(mutations), seal_reds=seal_reds,
                identity_reds=identity_reds, empirical=False)


def main():
    implementation_paths = {'script': Path(__file__).resolve(),
                            'merkle_verifier': Path(fire_payload_merkle.__file__).resolve()}
    implementation_bytes = implementation_snapshot(implementation_paths)
    bind_captured_implementation(implementation_paths, implementation_bytes)
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('--controls', action='store_true')
    for key in ('simulation', 'operands', 'output'):
        p.add_argument('--' + key, type=Path)
    a = p.parse_args()
    if a.controls:
        require(not any((a.simulation, a.operands, a.output)), 'controls cannot admit native evidence')
        print(json.dumps(controls()))
        return
    require(all((a.simulation, a.operands, a.output)), 'full source paths required')
    sim, operands, out = a.simulation, a.operands, a.output
    meta_raw = sealed(operands / 'checkpoint_operands.json')
    require(sha(meta_raw) == PINS['metadata'], 'accepted operand metadata identity')
    meta = json.loads(meta_raw)
    require(meta['source_case_record_id'] == CASE and meta['source_producer_build_id'] == PRODUCER and
            meta['recovery_accepted'] is False and meta['original_exit_code'] == 98, 'operand provenance')
    dt_raw = sealed(operands / 'accepted_dt.csv')
    require(sha(dt_raw) == meta['artifacts_sha256']['accepted_dt.csv'], 'operand dt linkage')
    vel_raw = (sim / 'budgets/maximum_velocity_trajectory.csv').read_bytes()
    host_raw = (sim / 'budgets/host_memory_trajectory.csv').read_bytes()
    require(sha(vel_raw) == PINS['velocity'] and sha(host_raw) == PINS['host'], 'original observation bytes')
    dt, velocity, host = map(rows, (dt_raw, vel_raw, host_raw))
    require(len(dt) == len(velocity) == 15315 and len(host) == 15316, 'full observation cardinality')
    require(int(host[0]['accepted_step']) == 0 and float(host[0]['time_s']) == 0.0,
            'initial host clock')
    records, time = [], 0.0
    for index, (d, v, h) in enumerate(zip(dt, velocity, host[1:]), 1):
        path = sim / 'budgets' / f'step_{index:06d}.csv'
        raw = sealed(path)
        ledger = ledger_metadata(raw)
        require(float(d['beginning_time_s']) == time, 'sequential history')
        time = bind_step(index, d, v, h, ledger)
        records.append(dict(step=index, ledger_sha256=sha(raw), input_root=ledger['input_root'],
                            native_root=ledger['native_root'], flux_root=ledger['flux_root']))
    require(time == meta['accepted_time_s'] == 25.156864278920693 and
            meta['accepted_steps'] == 15315, 'terminal history')
    # Publication occurs only after complete binding. Never write source artifacts.
    require_implementation_unchanged(implementation_paths, implementation_bytes)
    out.mkdir(exist_ok=False)
    identity = {name: sha(raw) for name, raw in implementation_bytes.items()}
    issuer = identity['script']
    report = dict(schema='rise.fire.r246.observation-ledger-binding.v1', recovery_accepted=False,
                  original_exit_code=98, source_case_record_id=CASE, source_producer_build_id=PRODUCER,
                  recovery_issuer_script_sha256=issuer, source_metadata_sha256=sha(meta_raw),
                  recovery_implementation_sha256=identity,
                  accepted_steps=15315, accepted_time_s=time, ledger_bindings=records,
                  limitations=['Only clock and payload-root observation fields cross-checked against sealed ledgers.',
                               'Other velocity/host fields remain originally unsealed observations, not authenticated measurements.',
                               'Capsule payload verification, budget arithmetic, derived summaries and full recovery remain pending.'])
    for name, raw in [('maximum_velocity_trajectory.observation.csv', vel_raw),
                      ('host_memory_trajectory.observation.csv', host_raw),
                      ('ledger_binding.json', (json.dumps(report, indent=2)+'\n').encode())]:
        path = out / name
        path.write_bytes(raw)
        cert = dict(schema='rise.fire.published-payload.v2', case_record_id=CASE,
                    sha256_v1=sha(raw), v2=merkle(raw), recovery_issuer_script_sha256=issuer,
                    provenance='new recovery capture; not an original producer certificate')
        Path(str(path)+'.payload-v2.json').write_text(json.dumps(cert)+'\n')
    for name, raw in implementation_bytes.items():
        (out / (name+'.frozen.py')).write_bytes(raw)
    (out / 'completion.json').write_text(json.dumps(dict(exit_code=0, recovery_accepted=False,
                                                       implementation_sha256=identity))+'\n')


if __name__ == '__main__':
    main()
