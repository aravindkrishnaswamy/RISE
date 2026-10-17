#!/usr/bin/env python3
"""Compare pinned full-window retained observations; no physical qualification."""
import argparse
import csv
import hashlib
import io
import json
import os
from pathlib import Path
import stat
import sys
import tempfile
import types

import numpy as np

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / 'tools'))
from fire_payload_merkle import FORMAT, verify

DX = .040790821423214683
CATALOG_SHA256 = 'f75c46ca9cb0a69078713543d655a088ec7a135c1e34ab9a0d5597bd02800446'
SIDES = ('xmin', 'xmax', 'ymin', 'ymax', 'bottom', 'top')
FIELDS = ('Z', 'CH4', 'O2', 'N2', 'CO2', 'H2O', 'CO', 'carbon', 'sensible_enthalpy', 'total_species_mass')


def require(condition, message):
    if not condition:
        raise ValueError(message)


def capture(item):
    path = Path(item['path'])
    before = path.lstat()
    require(stat.S_ISREG(before.st_mode) and before.st_nlink == 1, 'nonregular or aliased input')
    with path.open('rb') as stream:
        opened = os.fstat(stream.fileno())
        require((opened.st_dev, opened.st_ino) == (before.st_dev, before.st_ino), 'capture substitution')
        raw = stream.read()
        after = os.fstat(stream.fileno())
    require((opened.st_size, opened.st_mtime_ns, opened.st_ctime_ns) ==
            (after.st_size, after.st_mtime_ns, after.st_ctime_ns), 'capture changed')
    require(path.lstat() == after and hashlib.sha256(raw).hexdigest() == item['sha256'], 'input pin changed')
    return raw


def spectral_stats(time, values):
    require(len(time) >= 3 and np.all(np.diff(time) > 0) and np.isfinite(values).all(), 'invalid signal')
    n = 8192
    uniform_time = np.linspace(time[0], time[-1], n)
    shifted = values-values[:1]
    uniform = np.stack([np.interp(uniform_time, time, shifted[:, z]) for z in range(values.shape[1])], axis=1)
    x = np.arange(n, dtype=float); x -= x.mean()
    residual = uniform-uniform.mean(axis=0)-x[:, None]*(x@uniform/np.dot(x, x))[None, :]
    power = abs(np.fft.rfft(residual*np.hanning(n)[:, None], axis=0))**2
    frequency = np.fft.rfftfreq(n, (time[-1]-time[0])/(n-1))
    expected = int(np.argmin(abs(frequency-1.5/np.sqrt(.30))))
    rows = []
    for z in range(values.shape[1]):
        p = power[:, z]; total = float(p[1:].sum()); peak = 1+int(np.argmax(p[1:]))
        rows.append(dict(layer=z, sample_mean=float(values[:, z].mean()), sample_std=float(values[:, z].std()),
                         dominant_hz=float(frequency[peak]) if total else None,
                         expected_to_peak=float(p[expected]/p[peak]) if total else None,
                         slow_first_two_fraction=float(p[1:3].sum()/total) if total else None))
    return rows


def budget(raw, proof):
    text = raw.decode()
    metadata = dict(line[2:].split(' ', 1) for line in text.splitlines() if line.startswith('# '))
    require(metadata['case_record_id'] == proof['case'] and metadata['accepted_step'] == '15315', 'foreign ledger')
    rows = list(csv.DictReader(io.StringIO('\n'.join(l for l in text.splitlines() if not l.startswith('#')))))
    require(len(rows) == 30 and [(r['scope'], r['field']) for r in rows] ==
            [(s, str(f)) for s in ('step', 'full', 'statistics') for f in range(10)], 'ledger coverage')
    result = {}
    for row in rows:
        if row['scope'] != 'statistics':
            continue
        values = {k: float(v) for k, v in row.items() if k not in ('scope', 'field', 'units')}
        require(all(np.isfinite(list(values.values()))), 'nonfinite ledger')
        require(row['units'] == ('J' if row['field'] == '8' else 'kg'), 'ledger units')
        values['units'] = row['units']
        values['signed_out_minus_in'] = {s: values[s+'_out']-values[s+'_in'] for s in SIDES}
        result[FIELDS[int(row['field'])]] = values
    return dict(dimensions=metadata['dimensions'], statistics_start_s=float(metadata['statistics_start_s']),
                statistics_source_components=list(map(float, metadata['statistics_source'].split(','))), scalar=result)


def controls():
    t = np.linspace(0, 8, 257)
    y = -3 + np.sin(2*np.pi*t)
    r = spectral_stats(t, y[:, None])[0]
    require(r['sample_mean'] < 0 and abs(r['dominant_hz']-1) < .001, 'signed sine GREEN')
    require(spectral_stats(t, np.zeros((len(t), 1)))[0]['dominant_hz'] is None, 'zero unavailable GREEN')
    require(spectral_stats(t, np.full((len(t), 1), 300.1))[0]['dominant_hz'] is None, 'nonzero constant null GREEN')
    red = 0
    for bad_t, bad_y in [(t[::-1], y[:, None]), (t, np.full((len(t), 1), np.nan))]:
        try:
            spectral_stats(bad_t, bad_y)
        except ValueError:
            red += 1
    require(red == 2, 'signal RED controls')
    with tempfile.TemporaryDirectory(prefix='rise-r246-comparison-controls-') as directory:
        path = Path(directory)/'source'; path.write_bytes(b'signed operand A')
        item = dict(path=str(path), sha256=hashlib.sha256(path.read_bytes()).hexdigest())
        require(capture(item) == b'signed operand A', 'captured operand GREEN')
        path.write_bytes(b'signed operand B')
        try:
            capture(item)
        except ValueError:
            red += 1
        path.write_bytes(b'signed operand A')
        alias = Path(directory)/'alias'; os.link(path, alias)
        try:
            capture(item)
        except ValueError:
            red += 1
        alias.unlink(); path.unlink(); path.symlink_to(Path(directory)/'absent')
        try:
            capture(item)
        except ValueError:
            red += 1
    require(red == 5, 'signal/byte/alias/dangling RED controls')
    a = {'__name__': 'control'}; b = {'__name__': 'control'}
    exec(compile('def value(): return 8192\n', 'staged.py', 'exec'), a)
    exec(compile('def value(): return 4096\n', 'staged.py', 'exec'), b)
    try:
        bind_functions(a, b)
    except ValueError:
        red += 1
    require(red == 6, 'loaded A/captured B RED')
    return dict(signed_sine_green=True, zero_power_null_green=True, nonzero_constant_null_green=True,
                captured_operand_green=True, loaded_source_red=True, red=red)


def bind_functions(loaded, captured):
    for name, value in loaded.items():
        if isinstance(value, types.FunctionType) and value.__module__ == loaded['__name__']:
            candidate = captured.get(name)
            require(isinstance(candidate, types.FunctionType) and
                    code_identity(value.__code__) == code_identity(candidate.__code__) and
                    constant_identity(value.__defaults__) == constant_identity(candidate.__defaults__) and
                    constant_identity(value.__kwdefaults__) == constant_identity(candidate.__kwdefaults__),
                    'loaded code/default differs from captured source: '+name)


def constant_identity(value):
    if isinstance(value, types.CodeType):
        return ('code', code_identity(value))
    if isinstance(value, tuple):
        return ('tuple', tuple(constant_identity(x) for x in value))
    if isinstance(value, dict):
        return ('dict', tuple(sorted((constant_identity(k), constant_identity(v)) for k, v in value.items())))
    if isinstance(value, frozenset):
        return ('frozenset', tuple(sorted(constant_identity(x) for x in value)))
    return (type(value).__name__, value)


def code_identity(code):
    return (code.co_argcount, code.co_posonlyargcount, code.co_kwonlyargcount, code.co_flags,
            code.co_code, tuple(constant_identity(x) for x in code.co_consts), code.co_names,
            code.co_varnames, code.co_freevars, code.co_cellvars, getattr(code, 'co_exceptiontable', b''))


def implementations():
    paths = dict(script=Path(__file__).resolve(), verifier=ROOT/'tools/fire_payload_merkle.py')
    buffers = {name: capture(dict(path=str(path), sha256=hashlib.sha256(path.read_bytes()).hexdigest()))
               for name, path in paths.items()}
    require(hashlib.sha256(buffers['verifier']).hexdigest() ==
            'a6219a285792b671d6a33de41c73556d9c970f8aed32c034cacaa0c3ea405ebf', 'verifier source identity')
    own = {'__name__': 'captured_comparison', '__file__': str(paths['script'])}
    exec(compile(buffers['script'], str(paths['script']), 'exec'), own)
    bind_functions(globals(), own)
    require(all(globals()[name] == own[name] for name in ('DX', 'SIDES', 'FIELDS', 'CATALOG_SHA256')), 'loaded constants')
    verifier = {'__name__': 'captured_verifier', '__file__': str(paths['verifier'])}
    exec(compile(buffers['verifier'], str(paths['verifier']), 'exec'), verifier)
    globals().update({name: verifier[name] for name in ('FORMAT', 'verify')})
    return {name: hashlib.sha256(raw).hexdigest() for name, raw in buffers.items()}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('catalog', type=Path)
    parser.add_argument('output', type=Path)
    args = parser.parse_args()
    require(not os.path.lexists(args.output), 'occupied output')
    identity = implementations()
    catalog_raw = args.catalog.read_bytes(); catalog = json.loads(catalog_raw)
    require(hashlib.sha256(catalog_raw).hexdigest() == CATALOG_SHA256 and
            catalog['schema'] == 'rise.fire.r246.retained-comparison-inputs.v1', 'catalog pin/schema')
    raw = {name: capture(item) for name, item in catalog['inputs'].items()}
    for name, case in catalog['modern_pairs'].items():
        seal = json.loads(raw[name+'_seal'])
        require(seal['schema'] == 'rise.fire.published-payload.v2' and seal['case_record_id'] == case and
                seal['sha256_v1'] == hashlib.sha256(raw[name]).hexdigest() and
                seal['v2']['digest_format'] == FORMAT and verify(raw[name], seal['v2']), 'modern pair refusal')
    history = {}
    for name in ('baseline', 'wide'):
        with np.load(io.BytesIO(raw[name+'_stations']), allow_pickle=False) as data:
            history[name] = {k: data[k].copy() for k in data.files}
    tall = np.loadtxt(io.BytesIO(raw['taller_stations']), delimiter=',', skiprows=1).reshape(8916, 160, 5)
    require(np.isfinite(tall).all(), 'nonfinite taller stations')
    history['taller'] = dict(time_s=tall[:, 0, 0], height_m=tall[0, :, 1],
                             temperature_K=tall[:, :, 2], reaction_W_per_m3=tall[:, :, 3], vertical_velocity_m_per_s=tall[:, :, 4])
    station_rows = []; region = {}; contrasts = {}
    for name, data in history.items():
        t = data['time_s']; h = data['height_m']; nz = 160 if name == 'taller' else 80
        require(t.shape == (8916,) and h.shape == (nz,) and np.array_equal(h, (np.arange(nz)+.5)*DX), 'station geometry')
        require(t[0] == 10.531864278949797 and t[-1] == 25.156864278920693 and
                np.array_equal(t, history['baseline']['time_s']), 'full matched clock')
        if name == 'taller':
            require(np.array_equal(tall[:, :, 0], np.repeat(t[:, None], 160, axis=1)) and
                    np.array_equal(tall[:, :, 1], np.repeat(h[None, :], 8916, axis=0)), 'station ordering')
        for observable in ('temperature_K', 'reaction_W_per_m3', 'vertical_velocity_m_per_s'):
            y = data[observable]; require(y.shape == (8916, nz), 'observable coverage')
            station_rows.extend(dict(variant=name, observable=observable, height_m=float(h[r['layer']]), **r)
                                for r in spectral_stats(t, y))
            if name != 'baseline':
                reference = history['wide' if name == 'taller' else 'baseline'][observable]
                delta = y[:, :80]-reference
                contrasts[name+'_'+observable] = dict(reference='wide' if name == 'taller' else 'baseline',
                    layer_sample_mean_delta=delta.mean(axis=0).tolist(), layer_sample_rms_delta=np.sqrt((delta**2).mean(axis=0)).tolist())
        q = data['reaction_W_per_m3']*DX**3
        region[name] = {scope: dict(sample_mean_W=float(signal.mean()), sample_std_W=float(signal.std()),
                         integrated_J=float(np.trapezoid(signal, t))) for scope, signal in
                        [('common_0_79', q[:, :80].sum(axis=1)), ('native', q.sum(axis=1))]}
        if name == 'taller':
            region[name]['upper_80_159_max_abs_reaction_W'] = float(np.abs(q[:, 80:].sum(axis=1)).max())
    budgets = {}
    for name in ('wide', 'taller'):
        proof = json.loads(raw[name+'_audit'])
        require(proof['steps'] == 15315 and len(proof['frames']) == 235 and proof['end_time_s'] == 25.156864278920693 and
                hashlib.sha256(raw[name+'_ledger']).hexdigest() == proof['final_step_sha256'], 'accepted budget coverage/pin')
        budgets[name] = budget(raw[name+'_ledger'], proof)
    require(budgets['wide']['dimensions'] == '104 104 80' and budgets['taller']['dimensions'] == '104 104 160', 'budget domain')
    require(all(b['statistics_start_s'] == 10.531864278900095 for b in budgets.values()), 'budget statistics clock')
    deltas = {field: {side: budgets['taller']['scalar'][field]['signed_out_minus_in'][side]-
                     budgets['wide']['scalar'][field]['signed_out_minus_in'][side] for side in SIDES}
              for field in FIELDS}
    gate = controls()
    args.output.mkdir(mode=0o700)
    with (args.output/'station_comparison.csv').open('x') as stream:
        writer = csv.DictWriter(stream, fieldnames=list(station_rows[0])); writer.writeheader(); writer.writerows(station_rows)
    result = dict(status='descriptive_full_window_comparison_not_causal_localization', controls=gate,
                  inputs=catalog['inputs'], catalog_sha256=hashlib.sha256(catalog_raw).hexdigest(),
                  implementation_sha256=identity, regions=region,
                  common_layer_contrasts=contrasts, signed_statistics_budgets=budgets,
                  taller_minus_wide_signed_boundary_net=deltas,
                  station_summary_sha256=hashlib.sha256((args.output/'station_comparison.csv').read_bytes()).hexdigest(),
                  limitations=['Baseline signed per-side budgets were not retained; unavailable, not zero.',
                    'Wide budgets are the accepted r245 instrumented wide run; station history is accepted r244 wide.',
                    'Native whole-domain side budgets differ in height/side area; no common80 cumulative budget is claimed.',
                    'Centerline central2x2 is not plume-following; thresholded display and upper-path optical confound remain.',
                    '8192 interpolation/Hann spectra are descriptive, without antialiasing or uncertainty certification.',
                    'No canonical taller qualification, original run success, unavailable-claim authentication or causal remedy.'])
    (args.output/'comparison.json').write_text(json.dumps(result, indent=2, allow_nan=False)+'\n')
    print(json.dumps(dict(status=result['status'], regions=region, controls=result['controls'])))


if __name__ == '__main__':
    main()
