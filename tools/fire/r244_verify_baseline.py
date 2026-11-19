#!/usr/bin/env python3
"""Independently check either frozen r244 run's published full-window bytes."""
import argparse
import csv
import hashlib
import json
import math
import pathlib
import sys

ROOT=pathlib.Path(__file__).resolve().parents[2]
sys.path.insert(0,str(ROOT/'tools'))
from fire_payload_merkle import merkle

BASE=pathlib.Path('/Volumes/SSD/RISE/fire_production_calibration/r244_tier6_baseline.v1')
CASE='73d1cac70e5e68b697e0462ef3b5784c606606372397823d08cce9815b833b1e'

def require(condition,message):
    if not condition: raise ValueError(message)

def main():
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--variant',choices=['baseline','lateral'],default='baseline')
    variant=parser.parse_args().variant
    global BASE, CASE
    if variant=='lateral':
        BASE=BASE.parent/'r244_tier6_lateral_double.v1'
        CASE='a9369e770465bd76e2cc8d5fc3b79e7c2aac038ecb14a0e4424a97ba2f98d586'
    receipt=BASE.parent/f'r244_tier6_{variant}_launch.v1/completion.json'
    require(json.loads(receipt.read_text())['exit_code']==0,'run did not complete successfully')
    summary=dict(line.split('=',1) for line in (BASE/'production_capstone_summary.txt').read_text().splitlines())
    require(summary['resolution_tier']=='6','tier mismatch')
    require(summary['snapshot_count']=='235','frame count')
    require(summary['target_time_s']=='25.156864278900095','target changed')
    require(summary['statistics_start_s']=='10.531864278900095','discard changed')
    require(summary['snapshot_cadence_s']=='0.0625','cadence changed')
    require(float(summary['accepted_time_s'])>=float(summary['target_time_s']),'shortened window')
    require(summary['hard_bound_retries']=='0' and summary['physical_projection_retries']=='0','unexpected retries')
    frames=list(csv.DictReader((BASE/'frame_times.csv').open()))
    require(len(frames)==235,'time index coverage')
    inventory=[]
    paths=['window_launch.v1','host_memory_admission.v1','sequence_manifest.rise-fire.cbor',
           'frame_times.csv','monitored_diagnostics.csv','production_capstone_summary.txt','production_final.checkpoint']
    for i,row in enumerate(frames):
        require(int(row['frame'])==i,'frame ordering')
        actual=float(row['simulation_time_s'])
        offset=i/16
        # Existing ProductionTemporalFramesComplete allowance, not a new bound.
        allowance=8*(2**-23)*max(1/16,abs(offset))
        require(math.isfinite(actual) and abs(actual-(float(summary['statistics_start_s'])+offset))<=allowance,'frame schedule')
        paths.append(f'frames/frame_{i:05d}.vdb')
    for name in paths:
        path=BASE/name
        payload=path.read_bytes()
        digest=hashlib.sha256(payload).hexdigest()
        sealpath=pathlib.Path(str(path)+'.payload-v2.json')
        seal=json.loads(sealpath.read_text())
        require(seal['schema']=='rise.fire.published-payload.v2' and seal['case_record_id']==CASE,'foreign publication')
        require(digest==seal['sha256_v1'] and merkle(payload)==seal['v2'],'bad payload hash '+name)
        if name.startswith('frames/'):
            index=int(path.stem.split('_')[1])
            require(digest==frames[index]['vdb_sha256']==summary[f'frame_{index:05d}_sha256'],'frame linkage')
        for target,key in [('frame_times.csv','frame_times_sha256'),('sequence_manifest.rise-fire.cbor','sequence_manifest_sha256'),('monitored_diagnostics.csv','diagnostics_sha256')]:
            if name==target: require(digest==summary[key],'summary linkage')
        inventory.append(dict(path=str(path),sha256=digest,bytes=len(payload),sidecar_sha256=hashlib.sha256(sealpath.read_bytes()).hexdigest()))
    rows=list(csv.DictReader((BASE/'monitored_diagnostics.csv').open()))
    require(len(rows)==int(summary['accepted_steps']),'history coverage')
    for i,row in enumerate(rows):
        require(int(row['step'])==i+1,'history ordering')
        require(all(math.isfinite(float(v)) for v in row.values()),'nonfinite history')
        require(float(row['dt_s'])>0,'invalid timestep')
    record=dict(status='full_window_bytes_verified_not_empirical_qualification',case_record_id=CASE,
        summary=summary,history_rows=len(rows),history_dt_fsum_s=math.fsum(float(r['dt_s']) for r in rows),
        formal_contract_verdict=summary['formal_contract_verdict'],artifacts=inventory,
        verifier_sha256=hashlib.sha256(pathlib.Path(__file__).read_bytes()).hexdigest())
    target=ROOT/f'docs/data/fire_r244/{variant}_completion.json'
    with target.open('w') as stream: json.dump(record,stream,indent=2,allow_nan=False);stream.write('\n')
    print(f'PASS complete {len(rows)}-step, {len(frames)}-frame {variant}; all selected payload v1/v2 seals match')

if __name__=='__main__': main()
