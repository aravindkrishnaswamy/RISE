#!/usr/bin/env python3
"""Bind the external SDR movie to its ordered, independently sealed frames."""
import hashlib, json, pathlib
ROOT=pathlib.Path(__file__).resolve().parents[2]
OUT=pathlib.Path('/Volumes/SSD/RISE/fire_production_calibration/r242_tier8_hq.v1')
def sha(p):
    h=hashlib.sha256()
    with p.open('rb') as f:
        for block in iter(lambda:f.read(1048576),b''): h.update(block)
    return h.hexdigest()
completion=json.loads((OUT/'complete.json').read_text())
inventory=json.loads((OUT/'frames.sha256.json').read_text())
for entry in inventory:
    assert sha(OUT/entry['file'])==entry['sha256'], entry['file']
assert sha(pathlib.Path(completion['movie']))==completion['sha256']
assert 'PASS decoded_frames=235' in (OUT/'verify.log').read_text()
entries={row['file']:row for row in inventory}
frames=[]
for index in range(235):
    primary=f'fire_primary{index:04d}.exr'
    display=f'fire_display_denoised{index:04d}.png'
    frames.append(dict(frame_index=index,time_numerator=index,time_denominator=16,
        primary=entries[primary],primary_sidecar=entries[primary+'.provenance.cbor'],
        encoded_input=entries[display],encoded_input_sidecar=entries[display+'.provenance.cbor']))
record=dict(record_kind='r242_run_evidence',
    limitation='External SDR display derivative, not canonical fire movie provenance; primary and display frame CBOR sidecars remain authoritative.',
    movie=completion,derived_from_frames=frames,
    encoder=dict(codec='H264',bitrate=10000000,width=512,height=768,fps=16,frames=235),
    launch_sha256=sha(OUT/'launch.json'),inventory_sha256=sha(OUT/'frames.sha256.json'),
    scripts={name:sha(ROOT/'tools/fire'/name) for name in ['r242_encode.swift','r242_verify.swift','r242_finalize.py']})
target=OUT/'movie.derivation.json'
encoded=json.dumps(record,sort_keys=True,indent=2)+'\n'
if target.exists(): assert target.read_text()==encoded
else: target.write_text(encoded)
print('PASS ordered_frame_linkage_sha256='+sha(target))
