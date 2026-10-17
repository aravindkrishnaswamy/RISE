#!/usr/bin/env python3
"""Read-only r246 source/history/native-frame preflight controls; not recovery."""
import argparse
import hashlib
import json
from pathlib import Path
import shutil
import subprocess
from datetime import datetime, timezone

from r244_spectrum_input_gate import cbor_decode, cbor_encode


def sha(path):
    h = hashlib.sha256()
    with path.open('rb') as stream:
        for block in iter(lambda: stream.read(1024 * 1024), b''):
            h.update(block)
    return h.hexdigest()


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('--binary', type=Path, required=True)
    p.add_argument('--simulation', type=Path, required=True)
    p.add_argument('--output', type=Path, required=True)
    p.add_argument('--snapshot-only', action='store_true', help='Review fix controls; no repeat of235-frame preflight')
    a = p.parse_args()
    binary, sim, out = (x.resolve() for x in (a.binary, a.simulation, a.output))
    out.mkdir(exist_ok=False)
    results = []

    def run(name, source, code, marker, candidate=None, command_override=None):
        (out / 'current_stage.json').write_text(json.dumps(dict(stage=name, started_utc=datetime.now(timezone.utc).isoformat())) + '\n')
        log = out / (name + '.log')
        with log.open('w') as stream:
            command = ([str(binary), '--fire-r246-source-snapshot-inspect', str(source), str(candidate)]
                       if candidate else [str(binary), '--fire-r246-consumer-input-inspect', str(source)])
            actual = subprocess.call(command_override or command, stdout=stream, stderr=subprocess.STDOUT)
        if actual != code or marker not in log.read_text():
            raise RuntimeError(f'{name}: expected exit{code} with {marker!r}, got {actual}')
        result = dict(stage=name, exit_code=actual, expected=code, log_sha256=sha(log))
        results.append(result)
        (out / (name + '.completion.json')).write_text(json.dumps(result, indent=2) + '\n')

    if a.snapshot_only:
        # Model the source after replacement deterministically: candidate retains
        # different bytes while the consumed source is the good original manifest.
        candidate = out / 'stale_candidate.cbor'
        envelope = cbor_decode((sim / 'sequence_manifest.rise-fire.cbor').read_bytes())
        envelope['payload']['time_map']['t0'] = 10.531864278900095
        envelope['sequence_id'] = hashlib.sha256(cbor_encode(envelope['payload'])).hexdigest()
        candidate.write_bytes(cbor_encode(envelope))
        run('staged_source_replacement', sim, 98,
            'r246 candidate differs from consumed source manifest', candidate)
        assert 'R246_STAGED_SECOND_READ initial_candidate_equal=1 old_split_accepts=1 replacement_written=1 candidate_bound=0' in (out / 'staged_source_replacement.log').read_text()
        run('same_source_snapshot', sim, 0,
            'R246_SOURCE_SNAPSHOT_INSPECT passed=1 recovery_accepted=0',
            sim / 'sequence_manifest.rise-fire.cbor')
        run('checkpoint_pair_snapshot_reds', sim, 0,
            'R246_CHECKPOINT_SNAPSHOT_REDS passed=1 removed_seal=1 replaced_payload=1',
            command_override=[str(binary), '--fire-r246-checkpoint-snapshot-reds', str(sim)])
        for name in ('regular_pending_marker', 'dangling_pending_marker'):
            fixture = out / name
            fixture.mkdir()
            checkpoint = fixture / 'production_final.checkpoint'
            checkpoint.symlink_to(sim / checkpoint.name)
            shutil.copyfile(sim / (checkpoint.name + '.payload-v2.json'), Path(str(checkpoint) + '.payload-v2.json'))
            for filename in ('sequence_manifest.rise-fire.cbor', 'frame_times.csv'):
                shutil.copyfile(sim / filename, fixture / filename)
            pending = Path(str(checkpoint) + '.preparation.pending.payload-v2.json')
            if name == 'regular_pending_marker':
                pending.write_text('synthetic unfinished publication marker\n')
            else:
                pending.symlink_to(fixture / 'absent.pending')
            run(name, fixture, 98, 'r246 event history refuses incomplete source publication',
                command_override=[str(binary), '--fire-r246-event-history-inspect', str(checkpoint), str(fixture)])
        (out / 'report.json').write_text(json.dumps(dict(
            schema='rise.fire.r246.consumer-snapshot-controls.v1', recovery_accepted=False,
            binary=str(binary), binary_sha256=sha(binary), results=results,
            limitation='No repeated235-frame preflight; prior v1 native results retained as separate evidence.'), indent=2) + '\n')
        (out / 'completion.json').write_text(json.dumps(dict(exit_code=0,
            completed_utc=datetime.now(timezone.utc).isoformat())) + '\n')
        return
    for name in ('relabeled_discard', 'changed_placement', 'missing_checkpoint_seal', 'missing_first_frame'):
        fixture = out / name
        fixture.mkdir()
        checkpoint = fixture / 'production_final.checkpoint'
        checkpoint.symlink_to(sim / checkpoint.name)  # Read-only alias; sidecar itself must be regular.
        if name != 'missing_checkpoint_seal':
            shutil.copyfile(sim / (checkpoint.name + '.payload-v2.json'), Path(str(checkpoint) + '.payload-v2.json'))
        shutil.copyfile(sim / 'frame_times.csv', fixture / 'frame_times.csv')
        envelope = cbor_decode((sim / 'sequence_manifest.rise-fire.cbor').read_bytes())
        payload = envelope['payload']
        if name == 'relabeled_discard':
            payload['time_map']['t0'] = 10.531864278900095
        elif name == 'changed_placement':
            payload['producer_reason_codes'] = [x for x in payload['producer_reason_codes'] if x != 'case_grid_bound']
            for channel in payload['channels']:
                for key in ('voxel_size_m', 'origin_m', 'core_face_bounds_m'):
                    channel[key] = [x * 2.0 for x in channel[key]]
            payload['velocity_halo_width_m'] *= 2.0
        envelope['sequence_id'] = hashlib.sha256(cbor_encode(payload)).hexdigest()
        (fixture / 'sequence_manifest.rise-fire.cbor').write_bytes(cbor_encode(envelope))
        if name == 'missing_first_frame':
            (fixture / 'frames').mkdir()  # Exact retained schedule/hashes; intentionally absent first VDB.
            marker = 'R246_CONSUMER_INPUT_REJECT stage=native_frames error=fire sequence frame whole-file digest mismatch'
        else:
            marker = 'R246_CONSUMER_INPUT_REJECT stage=domain_history'
        run(name, fixture, 98, marker)
    run('actual_full_235_native_frames', sim, 0,
        'R246_CONSUMER_INPUT_INSPECT passed=1 recovery_accepted=0 native_frames=235')
    report = dict(schema='rise.fire.r246.consumer-input-controls.v1', recovery_accepted=False,
                  artifact_fidelity='engineering_source_history_and_native_frame_preflight',
                  binary=str(binary), binary_sha256=sha(binary), simulation=str(sim), results=results,
                  original_input_sha256={str(sim / x): sha(sim / x) for x in
                      ('production_final.checkpoint', 'frame_times.csv', 'sequence_manifest.rise-fire.cbor')},
                  limitations=['No budget operand Merkle audit or recovery publication/acceptance.',
                               'Original consumers and original exit98 remain unchanged.',
                               'Metadata mutants and absent-file fixture are explicitly synthetic.'])
    (out / 'report.json').write_text(json.dumps(report, indent=2) + '\n')
    (out / 'completion.json').write_text(json.dumps(dict(exit_code=0, completed_utc=datetime.now(timezone.utc).isoformat())) + '\n')


if __name__ == '__main__':
    main()
