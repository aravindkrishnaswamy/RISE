#!/usr/bin/env python3
"""Sequential engineering controls for r246 accepted event history, not recovery."""
import argparse
import csv
import hashlib
import io
import json
import math
import os
from pathlib import Path
import subprocess

from r244_spectrum_input_gate import cbor_decode, cbor_encode


def sha(path):
    digest = hashlib.sha256()
    with path.open('rb') as source:
        for block in iter(lambda: source.read(1024 * 1024), b''):
            digest.update(block)
    return digest.hexdigest()


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--binary', type=Path, required=True)
    parser.add_argument('--simulation', type=Path, required=True)
    parser.add_argument('--output', type=Path, required=True)
    args = parser.parse_args()
    binary, sim, out = (p.resolve() for p in (args.binary, args.simulation, args.output))
    out.mkdir(exist_ok=False)
    checkpoint = sim / 'production_final.checkpoint'
    original_manifest = (sim / 'sequence_manifest.rise-fire.cbor').read_bytes()
    original_csv = (sim / 'frame_times.csv').read_text()
    results = []

    def run(name, command, expected, required_marker=None):
        log = out / (name + '.log')
        with log.open('w') as output:
            code = subprocess.call([str(binary)] + command, stdout=output, stderr=subprocess.STDOUT)
        if code != expected:
            raise RuntimeError(f'{name}: expected {expected}, got {code}')
        text = log.read_text()
        marker = required_marker or ('r246 event history rejected:' if expected else
                  ('R246_EVENT_HISTORY_GATE passed=1' if name.startswith('synthetic') else
                   'R246_EVENT_HISTORY_INSPECT passed=1 recovery_accepted=0 steps=15315 frames=235'))
        if marker not in text:
            raise RuntimeError(f'{name}: missing intended diagnostic {marker!r}')
        results.append(dict(name=name, exit_code=code, expected=expected, log_sha256=sha(log)))

    def inspect(name, directory, expected):
        run(name, ['--fire-r246-event-history-inspect', str(checkpoint), str(directory)], expected)

    run('synthetic_green_and_eight_reds', ['--fire-r246-event-history-gate'], 0)
    inspect('actual_full_checkpoint_history', sim, 0)
    for name in ('one_ulp_frame_time', 'relabeled_exact_discard', 'missing_terminal_frame'):
        fixture = out / name
        fixture.mkdir()
        manifest = cbor_decode(original_manifest)
        rows = list(csv.DictReader(io.StringIO(original_csv)))
        if name == 'one_ulp_frame_time':
            rows[117]['simulation_time_s'] = repr(math.nextafter(float(rows[117]['simulation_time_s']), math.inf))
        elif name == 'relabeled_exact_discard':
            rows[0]['simulation_time_s'] = repr(10.531864278900095)
            manifest['payload']['time_map']['t0'] = 10.531864278900095
            manifest['sequence_id'] = hashlib.sha256(cbor_encode(manifest['payload'])).hexdigest()
        else:
            rows.pop()
        with (fixture / 'frame_times.csv').open('w', newline='') as output:
            writer = csv.DictWriter(output, fieldnames=['frame', 'simulation_time_s', 'vdb_sha256'], lineterminator='\n')
            writer.writeheader()
            writer.writerows(rows)
        (fixture / 'sequence_manifest.rise-fire.cbor').write_bytes(cbor_encode(manifest))
        inspect(name, fixture, 98)
    for name in ('missing_checkpoint_seal', 'aliased_checkpoint_seal'):
        fixture = out / name
        fixture.mkdir()
        payload = fixture / 'checkpoint'
        os.link(checkpoint, payload)  # Data are never edited; no giant redundant copy.
        if name.startswith('aliased'):
            Path(str(payload) + '.payload-v2.json').symlink_to(Path(str(checkpoint) + '.payload-v2.json'))
        run(name, ['--fire-r246-event-history-inspect', str(payload), str(sim)], 98,
            required_marker='r246 event history requires a nonaliased v2 checkpoint seal')
    report = dict(schema='rise.fire.r246.event-history-controls.v1', recovery_accepted=False,
                  artifact_fidelity='engineering_event_history_controls', binary=str(binary),
                  binary_sha256=sha(binary), simulation=str(sim), results=results,
                  original_input_sha256={str(p): sha(p) for p in
                      (checkpoint, sim / 'frame_times.csv', sim / 'sequence_manifest.rise-fire.cbor')},
                  limitations=['No native VDB preflight or full budget audit in this prerequisite.',
                               'Synthetic mutants alter metadata only; no simulation or pixels are produced.',
                               'Original exit98 and files remain unchanged; consumers remain unrepaired.'])
    (out / 'report.json').write_text(json.dumps(report, indent=2) + '\n')


if __name__ == '__main__':
    main()
