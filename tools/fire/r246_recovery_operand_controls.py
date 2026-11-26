#!/usr/bin/env python3
"""Controls for distinct retained-checkpoint operand export; not recovery acceptance."""
import argparse
import csv
from datetime import datetime, timezone
import hashlib
import itertools
import json
import math
from pathlib import Path
import shutil
import subprocess
import sys

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
from fire_payload_merkle import merkle


def sha(path):
    h = hashlib.sha256()
    with path.open('rb') as f:
        for block in iter(lambda: f.read(1024 * 1024), b''):
            h.update(block)
    return h.hexdigest()


def require(ok, message):
    if not ok:
        raise ValueError(message)


def main():
    p = argparse.ArgumentParser(description=__doc__)
    for key in ('binary', 'simulation', 'output'):
        p.add_argument('--' + key, type=Path, required=True)
    a = p.parse_args()
    binary, sim, out = (x.resolve() for x in (a.binary, a.simulation, a.output))
    out.mkdir(exist_ok=False)
    results = []

    def run(name, source, destination, expected, marker):
        log = out / (name + '.log')
        with log.open('w') as f:
            code = subprocess.call([str(binary), '--fire-r246-recovery-operands', str(source), str(destination)],
                                   stdout=f, stderr=subprocess.STDOUT)
        require(code == expected and marker in log.read_text(), name + ': wrong refusal/success')
        result = dict(stage=name, exit_code=code, log_sha256=sha(log))
        results.append(result)
        (out / (name + '.completion.json')).write_text(json.dumps(result) + '\n')

    for name in ('changed_manifest', 'missing_seal'):
        fixture = out / name
        fixture.mkdir()
        checkpoint = fixture / 'production_final.checkpoint'
        checkpoint.symlink_to(sim / checkpoint.name)
        if name != 'missing_seal':
            shutil.copyfile(sim / (checkpoint.name + '.payload-v2.json'), Path(str(checkpoint) + '.payload-v2.json'))
        for filename in ('sequence_manifest.rise-fire.cbor', 'frame_times.csv'):
            shutil.copyfile(sim / filename, fixture / filename)
        if name == 'changed_manifest':
            path = fixture / 'sequence_manifest.rise-fire.cbor'
            raw = bytearray(path.read_bytes())
            raw[-1] ^= 1
            path.write_bytes(raw)
        destination = out / (name + '_export')
        run(name, fixture, destination, 98, 'R246_OPERANDS_REJECT ' +
            ('source_snapshot' if name == 'missing_seal' else 'source_binding'))
        require(not destination.exists(), 'RED emitted recovery operands')
    log = out / 'source_capture_reds.log'
    with log.open('w') as f:
        code = subprocess.call([str(binary), '--fire-r246-source-capture-reds', str(sim)], stdout=f, stderr=subprocess.STDOUT)
    require(code == 0 and 'R246_SOURCE_CAPTURE_REDS passed=1' in log.read_text(), 'capture lifecycle RED failed')
    for mode in ('pending_removed', 'seal_alias_replaced'):
        require('R246_CAPTURE_RED mode=' + mode + ' old_accepts=1 fixed_accepts=0 clear_callback_ran=0 after_capture_refused=1' in log.read_text(),
                'capture control does not distinguish old/fixed lifecycle')
    require('R246_CSV_CAPTURE_RED old_accepts=1 replacement_written=1 bound_accepts=0' in log.read_text(),
            'CSV capture control does not distinguish old/fixed identity binding')
    result = dict(stage='source_capture_reds', exit_code=code, log_sha256=sha(log))
    results.append(result)
    (out / 'source_capture_reds.completion.json').write_text(json.dumps(result) + '\n')
    occupied = out / 'occupied'
    occupied.mkdir()
    (occupied / 'sentinel').write_text('preserve occupied output\n')
    run('occupied_output', sim, occupied, 98, 'R246_OPERANDS_REJECT occupied_or_unavailable_output')
    require(list(occupied.iterdir()) == [occupied / 'sentinel'], 'occupied output changed')
    operands = out / 'actual_operands'
    run('actual_retained_operands', sim, operands, 0, 'R246_OPERANDS_EXPORTED recovery_accepted=0 steps=15315')
    meta = json.loads((operands / 'checkpoint_operands.json').read_text())
    require(meta['recovery_accepted'] is False and meta['original_exit_code'] == 98,
            'original/recovery status mislabeled')
    require(meta['source_producer_build_id'] != meta['reader_build_id'] and
            meta['reader_executable_sha256'] == sha(binary), 'reader/producer identity confused')
    require(set(meta['unavailable']) == {'monitored_vectors_in_checkpoint', 'retry_counters',
                                       'external_fuel_mass', 'post_loop_summary'}, 'unavailable operands lost')
    require(not any(x in meta for x in ('external_fuel_kg', 'hard_bound_retries', 'physical_projection_retries',
                                      'puffing_hz', 'succeeded')), 'unpersisted default exported')
    for file in list(meta['artifacts_sha256']) + ['checkpoint_operands.json']:
        path = operands / file
        raw = path.read_bytes()
        seal = json.loads(Path(str(path) + '.payload-v2.json').read_text())
        require(seal['schema'] == 'rise.fire.published-payload.v2' and
                seal['case_record_id'] == meta['source_case_record_id'] and
                seal['sha256_v1'] == hashlib.sha256(raw).hexdigest() and seal['v2'] == merkle(raw),
                'bad recovery-reader artifact seal ' + file)
        if file in meta['artifacts_sha256']:
            require(meta['artifacts_sha256'][file] == sha(path), 'metadata artifact hash mismatch')
    with (operands / 'accepted_dt.csv').open() as dt, (sim / 'budgets/maximum_velocity_trajectory.csv').open() as observations:
        time = 0.0
        count = 0
        for index, (row, observation) in enumerate(itertools.zip_longest(csv.DictReader(dt), csv.DictReader(observations)), 1):
            require(row is not None and observation is not None, 'history count mismatch')
            require(int(row['step']) == index == int(observation['accepted_step']) and
                    float(row['beginning_time_s']) == time and
                    float(row['dt_s']) == float(observation['dt_s']), 'retained dt observation mismatch')
            time += float(row['dt_s'])
            require(float(row['time_s']) == time == float(observation['time_s']), 'retained endpoint mismatch')
            count += 1
        require(count == meta['accepted_steps'] == 15315 and time == meta['accepted_time_s'], 'terminal mismatch')
    counts = {}
    for file in ('centerline_probes.csv', 'station_probes.csv', 'centerline_integrals.csv'):
        with (operands / file).open() as f:
            count = 0
            for row in csv.DictReader(f):
                require(all(math.isfinite(float(x)) for x in row.values()), 'nonfinite retained operand')
                count += 1
        require(count > 0, 'empty retained operand')
        counts[file] = count
    require(counts['centerline_integrals.csv'] == 160 and
            counts['station_probes.csv'] == 160 * counts['centerline_probes.csv'], 'native station coverage')
    report = dict(schema='rise.fire.r246.recovery-operand-controls.v1', recovery_accepted=False,
                  binary=str(binary), binary_sha256=sha(binary), results=results,
                  operand_metadata=str(operands / 'checkpoint_operands.json'),
                  operand_metadata_sha256=sha(operands / 'checkpoint_operands.json'), row_counts=counts,
                  source_trajectory_sha256=sha(sim / 'budgets/maximum_velocity_trajectory.csv'),
                  limitations=['Only retained checkpoint operands exported; no original success relabel.',
                               'Trajectory clock comparison does not authenticate unsealed observations or other fields.',
                               'Complete ledger/capsule binding, derived summaries, recovery and budget audit remain pending.'])
    (out / 'report.json').write_text(json.dumps(report, indent=2) + '\n')
    (out / 'completion.json').write_text(json.dumps(dict(exit_code=0, completed_utc=datetime.now(timezone.utc).isoformat())) + '\n')


if __name__ == '__main__':
    main()
