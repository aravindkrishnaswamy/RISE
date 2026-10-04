#!/usr/bin/env python3
"""Prepare distinct r246 recovery receipt/profiles from accepted retained operands.

Preparation does not accept full recovery or physical behavior. No original
producer files, post-loop defaults, or unavailable observations are recreated.
"""
import argparse
import csv
import io
import json
import math
from pathlib import Path

import r246_observation_ledger_binding as dependency

HELPER_SHA = '8996e94d88ddede087b5d73a75fbbdb29f721803cee46a8f0af14bd566cc70c7'
PINS = {
    'checkpoint_operands.json': '81df738946e1f8537a7845554fd1b81158dd0b6303fd3617ead2e1a64a4fce33',
    'ledger_binding.json': '3a27bc90f840e12b98303104654d87acba4a59ce7448b65140d6c46bea0fc19c',
    'capsule_binding.json': '4774e99c6b273b3530adf262d4072a848288324686e74f29c25103f9ce7055c2',
    'production_final.checkpoint': '0632960c2b045c8717e2d655cfc8bb340b918023f0fea1bfa1117a0aeaaca043',
    'sequence_manifest.rise-fire.cbor': 'f036ff7f96a3f5827104cdd2f06e8aa068c3e8f097acd7ff3c069bab82fb3cc1',
    'frame_times.csv': '3bb265a8539d5de45356dba419024c077834e3bb4c3770d75c63478006a5a5c8',
    'window_launch.v1': 'bd9df5efcf3d8b2f9e43951b06574fa536b4c793b42453bf0612acdd9cdb751f',
    'completion.json': '8d664320d24952999b5d4d97028a05a8b0994392b16074ee027c53ce14e2e017',
}
R = None


def require(ok, message):
    if not ok:
        raise ValueError(message)


def derive_profiles(rows, duration, native_layers, common_layers):
    require(math.isfinite(duration) and duration > 0 and len(rows) == native_layers and
            0 < common_layers < native_layers, 'retained profile duration/support')
    profiles = []
    for z, row in enumerate(rows):
        require(int(row['z']) == z, 'retained layer ordinal')
        temperature, velocity, heat = (float(row[key]) for key in
                                      ('temperature_k_s', 'velocity_m', 'plane_heat_release_j'))
        require(all(math.isfinite(x) for x in (temperature, velocity, heat)) and
                temperature > 0 and heat >= 0, 'finite retained integral')
        profiles.append(dict(z=z, centerline_mean_temperature_k=temperature/duration,
                             centerline_mean_vertical_velocity_m_per_s=velocity/duration,
                             plane_mean_reaction_w=heat/duration))
    totals = {name: math.fsum(float(row['plane_heat_release_j']) for row in rows[:count])/duration
              for name, count in (('native_layers_0_159', native_layers), ('common_layers_0_79', common_layers))}
    return profiles, totals


def controls():
    rows = [dict(z='0', temperature_k_s='600', velocity_m='-2', plane_heat_release_j='8'),
            dict(z='1', temperature_k_s='800', velocity_m='6', plane_heat_release_j='4')]
    profile, totals = derive_profiles(rows, 2.0, 2, 1)
    require(profile[0]['centerline_mean_temperature_k'] == 300 and
            profile[0]['centerline_mean_vertical_velocity_m_per_s'] == -1 and
            profile[1]['plane_mean_reaction_w'] == 2 and list(totals.values()) == [6, 4],
            'hand-calculated profile GREEN')
    reds = 0
    for duration in (0.0, -1.0, float('nan')):
        try:
            derive_profiles(rows, duration, 2, 1)
        except ValueError:
            reds += 1
        else:
            raise ValueError('false duration GREEN')
    for key, value in (('z', '0'), ('plane_heat_release_j', '-1'), ('temperature_k_s', 'nan')):
        mutant = [dict(row) for row in rows]
        mutant[1][key] = value
        try:
            derive_profiles(mutant, 2.0, 2, 1)
        except ValueError:
            reds += 1
        else:
            raise ValueError('false integral GREEN: ' + key)
    return dict(hand_calculated_green=1, targeted_reds=reds, empirical=False)


def main():
    global R
    paths = {'script': Path(__file__).resolve(), 'ledger_helper': Path(dependency.__file__).resolve(),
             'merkle_verifier': Path(dependency.fire_payload_merkle.__file__).resolve()}
    captured = dependency.implementation_snapshot(paths)
    require(dependency.sha(captured['ledger_helper']) == HELPER_SHA, 'accepted helper identity')
    helper = {'__name__': 'r246_captured_helper', '__file__': str(paths['ledger_helper'])}
    exec(compile(captured['ledger_helper'], str(paths['ledger_helper']), 'exec'), helper)
    dependency.require_loaded_functions_match(dependency.__dict__, helper)
    own = {'__name__': 'r246_captured_recovery', '__file__': str(paths['script'])}
    exec(compile(captured['script'], str(paths['script']), 'exec'), own)
    dependency.require_loaded_functions_match(globals(), own)
    require(PINS == own['PINS'] and HELPER_SHA == own['HELPER_SHA'], 'loaded recovery constants')
    verifier = {'__name__': 'r246_captured_merkle', '__file__': str(paths['merkle_verifier'])}
    exec(compile(captured['merkle_verifier'], str(paths['merkle_verifier']), 'exec'), verifier)
    helper.update({name: verifier[name] for name in ('FORMAT', 'merkle', 'verify')})
    R = helper
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('--controls', action='store_true')
    for name in ('simulation', 'launch', 'operands', 'ledger_binding', 'capsule_binding', 'output'):
        p.add_argument('--'+name.replace('_', '-'), type=Path)
    a = p.parse_args()
    if a.controls:
        require(not any((a.simulation, a.launch, a.operands, a.ledger_binding, a.capsule_binding, a.output)),
                'controls cannot admit native recovery')
        print(json.dumps(controls()))
        return
    require(all((a.simulation, a.launch, a.operands, a.ledger_binding, a.capsule_binding, a.output)),
            'strict source/prerequisite/output paths required')
    inputs = {}

    def read(path, seal=True, expected=None):
        raw = R['sealed'](path) if seal else path.read_bytes()
        digest = R['sha'](raw)
        require(digest == (expected or PINS[path.name]), 'pinned recovery input: '+str(path))
        inputs[str(path.resolve())] = digest
        return raw

    meta = json.loads(read(a.operands/'checkpoint_operands.json'))
    ledger = json.loads(read(a.ledger_binding/'ledger_binding.json'))
    capsule = json.loads(read(a.capsule_binding/'capsule_binding.json'))
    for record in (meta, ledger, capsule):
        require(record['source_case_record_id'] == R['CASE'] and
                record['source_producer_build_id'] == R['PRODUCER'] and
                record['recovery_accepted'] is False and record['original_exit_code'] == 98,
                'distinct original/recovery provenance')
    require(meta['accepted_steps'] == ledger['accepted_steps'] == 15315 and
            meta['accepted_time_s'] == ledger['accepted_time_s'] == 25.156864278920693 and
            len(capsule['frames']) == 235 and capsule['ledger_binding_sha256'] == PINS['ledger_binding.json'] and
            ledger['source_metadata_sha256'] == PINS['checkpoint_operands.json'], 'complete prerequisite linkage')
    completion = json.loads(read(a.launch/'completion.json', False))
    require(completion['exit_code'] == 98 and completion['launch_sha256'] ==
            '417fc7bf6c62d3d8180bb67eaf86e3748390cb11f079948ada35758262ab1e1d', 'original exit98 launch')
    read(a.simulation/'production_final.checkpoint')
    read(a.simulation/'window_launch.v1')
    read(a.simulation/'sequence_manifest.rise-fire.cbor', False)
    read(a.simulation/'frame_times.csv', False)
    # Reuse accepted source receipts; do not repeat native preflight/capsule/ledger gates.
    raw_integrals = read(a.operands/'centerline_integrals.csv', expected=meta['artifacts_sha256']['centerline_integrals.csv'])
    profiles, plane_totals = derive_profiles(R['rows'](raw_integrals),
                                             meta['sampled_statistics_duration_s'], 160, 80)
    hrr = meta['integrated_hrr_j']
    radiation = meta['integrated_radiative_loss_j']
    fuel = meta['integrated_fuel_consumption_kg']
    require(all(math.isfinite(x) for x in (hrr, radiation, fuel)) and hrr > 0 and radiation >= 0 and fuel >= 0,
            'retained scalar integrals')
    unavailable = ['retry_counters', 'external_fuel_mass', 'checkpoint_monitored_vectors',
                   'original_post_loop_summary', 'physical_acceptance', 'puffing_localization']
    summary = dict(schema='rise.fire.r246.recovered-retained-summary.v1', sampled_duration_s=meta['sampled_statistics_duration_s'],
                   native_support='layers_0_159', common_height_support='layers_0_79',
                   integrated_hrr_j=hrr, integrated_radiative_loss_j=radiation,
                   integrated_fuel_consumption_kg=fuel, integrated_radiative_fraction=radiation/hrr,
                   mean_hrr_w=hrr/meta['sampled_statistics_duration_s'], plane_profile_mean_reaction_w=plane_totals,
                   unavailable=unavailable,
                   reduction_note='Plane totals use fsum of retained plane integrals; not an assertion of equality with a different producer reduction tree.')
    stream = io.StringIO()
    writer = csv.DictWriter(stream, fieldnames=list(profiles[0]))
    writer.writeheader()
    writer.writerows(profiles)
    artifacts = {'native_profile.csv': stream.getvalue().encode(),
                 'common_height_profile.csv': None,
                 'retained_summary.json': (json.dumps(summary, indent=2)+'\n').encode()}
    common = io.StringIO()
    writer = csv.DictWriter(common, fieldnames=list(profiles[0]))
    writer.writeheader()
    writer.writerows(profiles[:80])
    artifacts['common_height_profile.csv'] = common.getvalue().encode()
    dependency.require_implementation_unchanged(paths, captured)
    a.output.mkdir(exist_ok=False)
    identity = {name: R['sha'](raw) for name, raw in captured.items()}
    receipt = dict(schema='rise.fire.r246.prepared-recovery.v1', prepared_bundle=True,
                   recovery_accepted=False, original_exit_code=98, source_simulation=str(a.simulation.resolve()),
                   source_case_record_id=R['CASE'], source_producer_build_id=R['PRODUCER'],
                   source_executable_sha256=R['EXECUTABLE'], implementation_sha256=identity,
                   inputs_sha256=inputs, artifacts_sha256={name: R['sha'](raw) for name, raw in artifacts.items()},
                   accepted_steps=15315, accepted_time_s=meta['accepted_time_s'], frames=235,
                   canonical_taller_qualification=False, unavailable=unavailable,
                   limitations=['Separate recovery preparation; does not relabel original exit98 as success.',
                                'Other originally unsealed velocity/host/owner measurements remain observations.',
                                'Consumer integration, signed budget arithmetic and physical interpretation pending.'])
    artifacts['recovery_receipt.json'] = (json.dumps(receipt, indent=2)+'\n').encode()
    for name, raw in artifacts.items():
        (a.output/name).write_bytes(raw)
        cert = dict(schema='rise.fire.published-payload.v2', case_record_id=R['CASE'], sha256_v1=R['sha'](raw),
                    v2=R['merkle'](raw), recovery_implementation_sha256=identity,
                    provenance='new recovery preparation issuer; not an original producer certificate')
        (a.output/(name+'.payload-v2.json')).write_text(json.dumps(cert)+'\n')
    for name, raw in captured.items():
        (a.output/(name+'.frozen.py')).write_bytes(raw)
    (a.output/'completion.json').write_text(json.dumps(dict(exit_code=0, recovery_accepted=False,
                                                          implementation_sha256=identity))+'\n')


if __name__ == '__main__':
    main()
