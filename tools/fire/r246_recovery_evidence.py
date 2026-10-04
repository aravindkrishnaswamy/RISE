#!/usr/bin/env python3
"""Collect reviewed full-window recovery evidence; acceptance requires a separate review attestation.

No native payload scan, simulation, budget arithmetic, rendering or physical
qualification is performed. Original exit98 and prepared false receipts remain.
"""
import argparse
import json
import os
from pathlib import Path
import stat
import tempfile

import r246_observation_ledger_binding as dependency

CATALOG = Path('/Users/aravind/Working/RISE/worktrees/r243-structure-diagnostic/docs/data/fire_r245/r246_recovery_evidence_catalog.json')
CATALOG_SHA = 'e5ad468897af2b4d948f969ed91fb77eaab2cf4eef0c93c4445dd61a97c368e6'
HELPER_SHA = '8996e94d88ddede087b5d73a75fbbdb29f721803cee46a8f0af14bd566cc70c7'
VERIFIER_SHA = 'a6219a285792b671d6a33de41c73556d9c970f8aed32c034cacaa0c3ea405ebf'
EXT = Path('/Volumes/SSD/RISE/fire_production_calibration')
UNAVAILABLE = ['retry_counters', 'external_fuel_mass', 'checkpoint_monitored_vectors',
               'original_post_loop_summary', 'physical_acceptance', 'puffing_localization']


def require(ok, message):
    if not ok:
        raise ValueError(message)


def capture(path, expected, after_capture=None):
    pending = Path(str(path)+'.preparation.pending.payload-v2.json')

    def identity():
        require(not pending.exists() and not pending.is_symlink(), 'pending publication')
        info = path.lstat()
        require(stat.S_ISREG(info.st_mode) and info.st_nlink == 1, 'regular nonaliased payload')
        return (info.st_dev, info.st_ino, info.st_size, info.st_mtime_ns, info.st_ctime_ns)

    before = identity()
    raw = path.read_bytes()
    if after_capture:
        after_capture()
    require(identity() == before, 'capture lifecycle changed')
    require(dependency.sha(raw) == expected, 'exact evidence pin')
    return raw


def sealed_record(record, verifier):
    path = Path(record['path'])
    raw = capture(path, record['sha256'])
    cert = json.loads(capture(Path(str(path)+'.payload-v2.json'), record['seal_sha256']))
    # Recheck source lifecycle after both captures, without replacing retained bytes.
    require(capture(path, record['sha256']) == raw, 'payload changed across certificate capture')
    require(cert['schema'] == 'rise.fire.published-payload.v2' and
            cert['case_record_id'] == dependency.CASE and cert['sha256_v1'] == dependency.sha(raw) and
            cert['v2']['digest_format'] == verifier['FORMAT'] and verifier['verify'](raw, cert['v2']),
            'mandatory modern evidence seal')
    return raw


def validate_linkage(records, captured):
    def named(name):
        matches = [json.loads(captured[r['path']]) for r in records if Path(r['path']).name == name]
        require(len(matches) == 1, 'unique evidence operand: '+name)
        return matches[0]

    prepared = named('recovery_receipt.json')
    meta, ledger, capsule = [named(n) for n in ('checkpoint_operands.json','ledger_binding.json','capsule_binding.json')]
    require(prepared['prepared_bundle'] is True and prepared['recovery_accepted'] is False and
            prepared['original_exit_code'] == 98 and prepared['accepted_steps'] == 15315 and prepared['frames'] == 235 and
            prepared['canonical_taller_qualification'] is False and prepared['unavailable'] == UNAVAILABLE,
            'prepared recovery scope')
    for item in (prepared, meta, ledger, capsule):
        require(item['source_case_record_id'] == dependency.CASE and
                item['source_producer_build_id'] == dependency.PRODUCER and
                item['recovery_accepted'] is False and item['original_exit_code'] == 98,
                'source identity and failure retained')
    require(meta['accepted_steps'] == ledger['accepted_steps'] == 15315 and
            meta['accepted_time_s'] == ledger['accepted_time_s'] == prepared['accepted_time_s'] == 25.156864278920693 and
            len(capsule['frames']) == 235, 'full original window')
    pins = {r['path']: r['sha256'] for r in records}
    for path, digest in prepared['inputs_sha256'].items():
        # Large original checkpoint and window were captured by accepted native
        # admission; bind their published pins through the prepared receipt.
        if path in pins:
            require(pins[path] == digest, 'prepared prerequisite input link')
    require(named('completion.json')['exit_code'] == 98, 'original completion exit98')
    summary = named('retained_summary.json')
    require(summary['unavailable'] == UNAVAILABLE and
            summary['sampled_duration_s'] == meta['sampled_statistics_duration_s'] == 14.625000000020599,
            'retained summary scope')
    require(named('r246_recovered_cli_review.json')['status'] == 'accepted_zero_p1_p2', 'consumer fresh review')
    return prepared


def publish(output, artifacts):
    require(output.is_absolute() and output.parent.resolve() == output.parent, 'canonical output parent')
    output.mkdir(mode=0o700, exist_ok=False)
    for name, raw in artifacts.items():
        with (output/name).open('xb') as handle:
            handle.write(raw)


def controls():
    with tempfile.TemporaryDirectory() as directory:
        root = Path(directory).resolve()
        payload = root/'payload'; payload.write_bytes(b'A'); digest = dependency.sha(b'A')
        require(capture(payload, digest) == b'A', 'captured GREEN')
        reds = 0
        def red(action):
            nonlocal reds
            try:
                action()
            except (ValueError, FileExistsError):
                reds += 1
            else:
                raise ValueError('false GREEN')
        red(lambda: capture(payload, dependency.sha(b'B')))
        alias = root/'alias'; alias.symlink_to(payload)
        red(lambda: capture(alias, digest))
        link = root/'hardlink'; os.link(payload, link)
        red(lambda: capture(payload, digest)); link.unlink()
        pending = Path(str(payload)+'.preparation.pending.payload-v2.json')
        pending.symlink_to(root/'absent')
        red(lambda: capture(payload, digest)); pending.unlink()
        red(lambda: capture(payload, digest, lambda: pending.write_text('pending'))); pending.unlink()
        red(lambda: capture(payload, digest, lambda: payload.write_bytes(b'B')))
        output = root/'out'; publish(output, {'A': b'A'})
        red(lambda: publish(output, {'A': b'B'}))
        require((output/'A').read_bytes() == b'A', 'occupied output retained')
        return dict(captured_green=1, targeted_reds=reds, empirical=False)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--controls', action='store_true')
    parser.add_argument('--output', type=Path)
    args = parser.parse_args()
    paths = dict(script=Path(__file__).resolve(), helper=Path(dependency.__file__).resolve(),
                 verifier=Path(dependency.fire_payload_merkle.__file__).resolve())
    code = {name: capture(path, dependency.sha(path.read_bytes())) for name,path in paths.items()}
    require(dependency.sha(code['helper']) == HELPER_SHA and dependency.sha(code['verifier']) == VERIFIER_SHA,
            'accepted implementation dependencies')
    helper = dict(__name__='r246_captured_helper', __file__=str(paths['helper']))
    exec(compile(code['helper'], str(paths['helper']), 'exec'), helper)
    dependency.require_loaded_functions_match(dependency.__dict__, helper)
    require(all(getattr(dependency,n) == helper[n] for n in ('CASE','PRODUCER','EXECUTABLE')),
            'loaded source identity constants')
    own = dict(__name__='r246_captured_evidence', __file__=str(paths['script']))
    exec(compile(code['script'], str(paths['script']), 'exec'), own)
    dependency.require_loaded_functions_match(globals(), own)
    require(all(globals()[n] == own[n] for n in ('CATALOG','CATALOG_SHA','HELPER_SHA','VERIFIER_SHA','EXT','UNAVAILABLE')),
            'loaded constants match captured source')
    verifier = dict(__name__='r246_captured_verifier', __file__=str(paths['verifier']))
    exec(compile(code['verifier'], str(paths['verifier']), 'exec'), verifier)
    if args.controls:
        require(args.output is None, 'controls cannot publish native evidence')
        print(json.dumps(controls())); return
    require(args.output == EXT/'r246_recovery_evidence.v1', 'exact new evidence output')
    catalog_raw = capture(CATALOG, CATALOG_SHA)
    records = json.loads(catalog_raw)['records']
    require(len({r['path'] for r in records}) == len(records) == 50, 'unique complete catalog')
    captured = {r['path']: sealed_record(r, verifier) if 'seal_sha256' in r else capture(Path(r['path']),r['sha256'])
                for r in records}
    prepared = validate_linkage(records, captured)
    identity = {name: dependency.sha(raw) for name,raw in code.items()}
    receipt = dict(schema='rise.fire.r246.complete-recovery-evidence.v1', recovery_accepted=False,
                   acceptance_pending_fresh_review=True, original_exit_code=98,
                   accepted_steps=15315, accepted_time_s=prepared['accepted_time_s'], frames=235,
                   source_case_record_id=dependency.CASE, source_producer_build_id=dependency.PRODUCER,
                   original_executable_sha256=dependency.EXECUTABLE,
                   implementation_sha256=identity, evidence_catalog_sha256=CATALOG_SHA, evidence=records,
                   canonical_taller_qualification=False, unavailable=UNAVAILABLE,
                   scope='Complete retained full-window data recovery and reviewed consumer composition only.',
                   remaining=['Full signed budget arithmetic audit','Actual full preview and spectrum launches',
                              'Native/common physical comparison','Puffing localization'],
                   limitations=['Original exit98 and all prerequisite false receipts remain unchanged.',
                                'No authentication promotion of originally unsealed non-clock observations.',
                                'Prior full native/capsule/ledger gates reused; no duplicate scans.',
                                'Preview/spectrum launch branches source-reviewed, not yet executed.'])
    raw = (json.dumps(receipt,indent=2)+'\n').encode()
    cert = dict(schema='rise.fire.published-payload.v2',case_record_id=dependency.CASE,
                sha256_v1=dependency.sha(raw),v2=verifier['merkle'](raw),
                recovery_implementation_sha256=identity,provenance='new recovery evidence collector; not original producer')
    artifacts = {'recovery_evidence.json':raw,'recovery_evidence.json.payload-v2.json':(json.dumps(cert)+'\n').encode(),
                 'evidence_catalog.json':catalog_raw}
    artifacts.update({name+'.frozen.py':raw for name,raw in code.items()})
    for name,path in paths.items():
        require(capture(path,identity[name]) == code[name], 'implementation stable')
    publish(args.output,artifacts)
    print(json.dumps(dict(exit_code=0,recovery_accepted=False,receipt_sha256=dependency.sha(raw),
                         evidence_records=len(records),implementation_sha256=identity)))


if __name__ == '__main__':
    main()
