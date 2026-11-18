#!/usr/bin/env python3
"""Exercise the actual spectrum consumer against real full-window artifacts and input substitutions."""
import csv
import hashlib
import json
import pathlib
import shutil
import struct
import subprocess
import sys

ROOT = pathlib.Path(__file__).resolve().parents[2]
DATA = pathlib.Path('/Volumes/SSD/RISE/fire_production_calibration')
SIM = DATA/'r244_tier6_baseline.v1'
PREVIEW = DATA/'r244_tier6_baseline_preview.v1'

def sha(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()

def cbor_encode(value):
    """RISE-CBOR64 fixture encoding: binary64 floats and UTF-8-sorted map keys."""
    def head(major, size):
        if size < 24:
            return bytes([major * 32 + size])
        for tag, count in [(24, 1), (25, 2), (26, 4), (27, 8)]:
            if size < 1 << (count * 8):
                return bytes([major * 32 + tag]) + size.to_bytes(count, 'big')
        raise ValueError('oversized fixture integer')
    if value is None:
        return b'\xf6'
    if isinstance(value, bool):
        return b'\xf5' if value else b'\xf4'
    if isinstance(value, int):
        return head(0, value) if value >= 0 else head(1, -1 - value)
    if isinstance(value, float):
        return b'\xfb' + struct.pack('>d', value)
    if isinstance(value, (str, bytes)):
        encoded = value.encode('utf-8') if isinstance(value, str) else value
        return head(3 if isinstance(value, str) else 2, len(encoded)) + encoded
    if isinstance(value, list):
        return head(4, len(value)) + b''.join(cbor_encode(item) for item in value)
    if isinstance(value, dict):
        return head(5, len(value)) + b''.join(cbor_encode(key) + cbor_encode(value[key])
                                             for key in sorted(value, key=lambda key: key.encode('utf-8')))
    raise ValueError('unsupported fixture CBOR value')

def cbor_decode(encoded):
    offset = 0
    def take(count):
        nonlocal offset
        end = offset + count
        if end > len(encoded):
            raise ValueError('truncated fixture CBOR')
        result = encoded[offset:end]; offset = end
        return result
    def read():
        tag = take(1)[0]; major, argument = tag >> 5, tag & 31
        if major == 7:
            if argument == 27:
                return struct.unpack('>d', take(8))[0]
            return {20: False, 21: True, 22: None}[argument]
        size = argument if argument < 24 else int.from_bytes(take({24: 1, 25: 2, 26: 4, 27: 8}[argument]), 'big')
        if major in (0, 1):
            return size if major == 0 else -1 - size
        if major in (2, 3):
            value = take(size)
            return value if major == 2 else value.decode('utf-8')
        if major == 4:
            return [read() for _ in range(size)]
        if major == 5:
            return {read(): read() for _ in range(size)}
        raise ValueError('unsupported fixture CBOR type')
    value = read()
    if offset != len(encoded) or cbor_encode(value) != encoded:
        raise ValueError('fixture must round-trip byte-identically as canonical RISE-CBOR64')
    return value

def synthetic_display_profile(directory, edit):
    """Mutate metadata only; never claim these fixture pixels were re-rendered."""
    directory.mkdir()
    sidecar_name = 'temporal_display_00000.png.provenance.cbor'
    for original in PREVIEW.iterdir():
        if original.is_file() and original.name != sidecar_name:
            (directory/original.name).symlink_to(original)
    envelope = cbor_decode((PREVIEW/sidecar_name).read_bytes())
    payload = envelope['payload']
    config = payload['resolved_render_configuration_v1']
    edit(config)
    payload['resolved_render_configuration_id'] = hashlib.sha256(cbor_encode(config)).hexdigest()
    envelope['provenance_id'] = hashlib.sha256(cbor_encode(payload)).hexdigest()
    (directory/sidecar_name).write_bytes(cbor_encode(envelope))
    (directory/'synthetic_fixture.json').write_text(json.dumps(dict(
        artifact_fidelity='test_fixture_metadata_only',
        note='Original PNG and verified primary EXR bytes and linkage are unchanged. '
             'Only display configuration metadata and its dependent hashes are synthetic; '
             'this is not re-render or native encoder evidence.'), indent=2)+'\n')
    return directory

def main():
    out = pathlib.Path(sys.argv[1]).resolve();out.mkdir(exist_ok=False)
    executable = ROOT/'bin/tests/FireSequenceTest'
    results = []
    def run(name, sim=SIM, preview=PREVIEW, checkpoint=SIM/'production_final.checkpoint', expected=0,
            consumer=executable):
        target=out/(name+'_output')
        cmd=[str(consumer),'--fire-production-puffing-spectrum','6',str(checkpoint),str(sim),str(preview),str(target)]
        with (out/(name+'.log')).open('w') as log:
            code=subprocess.call(cmd,stdout=log,stderr=subprocess.STDOUT,cwd=ROOT)
        if code!=expected:raise RuntimeError(f'{name}: expected {expected}, got {code}')
        if expected and target.exists():raise RuntimeError(name+' published refused evidence')
        results.append(dict(name=name,command=cmd,exit_code=code,expected_exit_code=expected,
                            consumer_sha256=sha(consumer),log_sha256=sha(out/(name+'.log'))))
        print(name,code,flush=True)
        return target
    green=run('full_window_green')
    for filename in ['centerline_signal.csv','lit_area_signal.csv','display_inputs.csv','full_spectrum.csv']:
        if sha(green/filename)!=sha(DATA/'r244_tier6_baseline_spectrum.v1'/filename):
            raise RuntimeError('validation changed the established observable: '+filename)
    def simulation(name, edit):
        path=out/name;path.mkdir()
        shutil.copyfile(SIM/'sequence_manifest.rise-fire.cbor',path/'sequence_manifest.rise-fire.cbor')
        (path/'frames').symlink_to(SIM/'frames',target_is_directory=True)
        with (SIM/'frame_times.csv').open() as stream:values=list(csv.DictReader(stream))
        edit(values)
        with (path/'frame_times.csv').open('w') as stream:
            writer=csv.DictWriter(stream,fieldnames=['frame','simulation_time_s','vdb_sha256'],lineterminator='\n');writer.writeheader();writer.writerows(values)
        return path
    run('copied_csv_green',sim=simulation('copied',lambda values:None))
    truncated=simulation('truncated',lambda values:values.__delitem__(slice(8,None)))
    run('truncated_csv_red',sim=truncated,expected=93)
    shifted=simulation('shifted',lambda values:values[-1].update(simulation_time_s=str(float(values[-1]['simulation_time_s'])+.01)))
    run('shifted_endpoint_red',sim=shifted,expected=93)
    digest=simulation('wrong_digest',lambda values:values[0].update(vdb_sha256='0'*64))
    run('wrong_vdb_digest_red',sim=digest,expected=94)
    retained=sorted((SIM/'checkpoint_history').glob('*.checkpoint'))[-2]
    run('retained_prefix_checkpoint_red',checkpoint=retained,expected=93)
    run('foreign_sequence_preview_red',preview=DATA/'r241_tier8_preview.v1',expected=94)
    swapped=out/'swapped_frame';swapped.mkdir()
    for original in PREVIEW.iterdir():
        if original.is_file():
            source=PREVIEW/original.name.replace('_00000.','_00001.')
            (swapped/original.name).symlink_to(source)
    run('swapped_frame_red',preview=swapped,expected=94)
    # The no-op GREEN proves that our codec and fixture layout reach the real
    # consumer. REDs isolate render-profile validation, not foreign-frame hashes.
    run('synthetic_profile_noop_green',preview=synthetic_display_profile(
        out/'synthetic_profile_noop',lambda config: None))
    def change_camera(config):
        config['camera']['projection']['fov_radians'] *= 0.5
        for state in config['evaluated_camera_states']:
            state['camera']['projection']['fov_radians'] *= 0.5
    for name, edit in [
        ('exposure', lambda config: config['output'].update(view_exposure_ev=7.0)),
        ('camera', change_camera),
        ('sampling', lambda config: config['sampler']['spectral'].update(spectral_samples=8))]:
        fixture=synthetic_display_profile(out/('synthetic_'+name+'_profile'),edit)
        run('synthetic_'+name+'_old_consumer_accepts',preview=fixture,
            consumer=DATA/'r244_runtime.v1/FireSequenceTest')
        run('synthetic_'+name+'_profile_red',preview=fixture,expected=94)
    (out/'completion.json').write_text(json.dumps(dict(status='passed',executable_sha256=sha(executable),
        script_sha256=sha(pathlib.Path(__file__)),tests=results,
        synthetic_profile_scope='Display-sidecar metadata mutations only; original primary and PNG bytes retained. '
                                'No changed-camera rendering or native display re-encoding was performed.'),indent=2)+'\n')

if __name__=='__main__':
    main()
