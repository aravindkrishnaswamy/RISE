#!/usr/bin/env python3
"""Derive Stage A spatial frozen-source observations from retained beginning bytes.

No simulation, terminal HRR reconstruction, full cumulative common-region
budget, canonical taller qualification or physical mechanism acceptance.
"""
import argparse
import hashlib
import json
import math
import os
from pathlib import Path
import resource
import stat
import sys
import tempfile
import types

import numpy as np

ROOT = Path('/Users/aravind/Working/RISE/worktrees/r243-structure-diagnostic')
EXT = Path('/Volumes/SSD/RISE/fire_production_calibration')
METADATA = EXT/'r247_source_metadata_validation.v3/metadata/source_metadata.json'
METADATA_SHA = 'e0114ff6c0371d321294ef8775f698baca67c9033874dc276b04d765ec842624'
METADATA_SEAL_SHA = '99bd8dc82544d31b7c68c8e677dbcb497071b4ca0046fe5c119231e1e307e6ce'
DEPENDENCIES = {
 'admission': ('tools/fire/r247_source_admission.py','cb25a9a96afb2939846ad4e2b55204c45062c3e5c897eb142df7a8e7fe250545'),
 'moments': ('tools/fire/r247_source_moments.py','37b3d1488ac6e114c228a47cdcb5e2be31b658dd36169b3d6639439b61d8a28c'),
 'verifier': ('tools/fire_payload_merkle.py','a6219a285792b671d6a33de41c73556d9c970f8aed32c034cacaa0c3ea405ebf')}
READER_CEILING = 1024**3
PUBLIC_FIXTURE_CEILING = 2*1024**3
OUTPUT_LIMIT = 16*1024**2
ROW_LIMIT = 65536
DX = .040790822356939316


def require(ok, message):
    if not ok: raise ValueError(message)


def sha(raw):
    return hashlib.sha256(raw).hexdigest()


def state(path, size=None):
    path = Path(path)
    require(not os.path.lexists(Path(str(path)+'.preparation.pending.payload-v2.json')), 'pending packet publication')
    require(all(not parent.is_symlink() for parent in path.parents), 'aliased source/output ancestor')
    s = path.lstat()
    require(stat.S_ISREG(s.st_mode) and s.st_nlink == 1 and (size is None or s.st_size == size),
            'regular nonaliased exact packet size')
    return (s.st_dev,s.st_ino,s.st_size,s.st_mtime_ns,s.st_ctime_ns)


def capture(path, expected, limit=OUTPUT_LIMIT):
    before = state(path)
    require(before[2] <= limit,'bounded capture')
    with Path(path).open('rb') as stream:
        s = os.fstat(stream.fileno())
        require((s.st_dev,s.st_ino) == before[:2], 'capture substitution')
        raw = stream.read(limit+1)
    require(state(path) == before and len(raw) == before[2] and sha(raw) == expected, 'exact capture lifecycle/hash')
    return raw


def implementation():
    buffers = {'reader': capture(Path(__file__),sha(Path(__file__).read_bytes()))}
    contexts = {}
    for name,(path,digest) in DEPENDENCIES.items():
        buffers[name] = capture(ROOT/path,digest)
        contexts[name] = {'__name__':'captured_'+name,'__file__':str(ROOT/path)}
        exec(compile(buffers[name],str(ROOT/path),'exec'),contexts[name])
    own = {'__name__':'captured_reader','__file__':__file__}
    exec(compile(buffers['reader'],__file__,'exec'),own)
    key = contexts['admission']['code_key']
    for name,function in list(globals().items()):
        if isinstance(function,types.FunctionType) and function.__module__ == __name__:
            require(key(function.__code__) == key(own[name].__code__) and
                    function.__defaults__ == own[name].__defaults__ and
                    function.__kwdefaults__ == own[name].__kwdefaults__, 'loaded reader changed')
    require(all(globals()[k] == own[k] for k in ('ROOT','EXT','METADATA','METADATA_SHA','METADATA_SEAL_SHA',
            'DEPENDENCIES','READER_CEILING','PUBLIC_FIXTURE_CEILING','OUTPUT_LIMIT','ROW_LIMIT','DX')), 'loaded reader constants')
    return contexts,{name:sha(raw) for name,raw in buffers.items()}


def pinned_sealed(path, digest, seal_digest, case, admission, verifier, callback=None):
    # Bind the certificate actually consumed by the accepted pair reader,
    # not merely a successful earlier certificate capture.
    capture(Path(str(path)+'.payload-v2.json'),seal_digest)
    if callback: callback()
    raw,record=admission['sealed'](path,case,verifier,digest)
    require(record['seal_sha256']==seal_digest,'consumed modern certificate exact pin')
    return raw,record


def peak_rss_bytes():
    require(sys.platform == 'darwin','native reader RSS units require macOS')
    return resource.getrusage(resource.RUSAGE_SELF).ru_maxrss


def allocation_plan(packet_bytes, cells, baseline_rss):
    require(all(type(x) is int and x > 0 for x in (packet_bytes,cells,baseline_rss)), 'typed allocation operands')
    leaves = (packet_bytes+4095)//4096
    # SHA leaf objects/list plus both old/new reduction levels coexist. Bound
    # pointer/object overhead conservatively by explicit 128 bytes per node.
    merkle_nodes = math.ceil(leaves*16/15)+16
    components = dict(captured_packet=packet_bytes,moment_arrays=4*cells*8,
                      finite_and_sign_boolean_scratch=5*cells,
                      merkle_nodes_and_lists=merkle_nodes*128,merkle_chunk_concat=8192,
                      axis_reductions_and_row_buffer=ROW_LIMIT+8192,
                      metadata_rechecks=4*ROW_LIMIT)
    estimate = baseline_rss+sum(components.values())
    require(estimate <= READER_CEILING and packet_bytes <= PUBLIC_FIXTURE_CEILING, 'before-field reader allocation refusal')
    return dict(baseline_peak_rss_bytes=baseline_rss,direct_buffer_components_bytes=components,
                enumerated_peak_estimate_bytes=estimate,reader_ceiling_bytes=READER_CEILING,
                public_fixture_cap_bytes=PUBLIC_FIXTURE_CEILING,
                estimate_is_not_measured_peak=True,source_host_ceiling_bytes=34359738368,
                source_next_step_reserve_bytes=23930269536,
                source_reserve_is_not_reader_allocation=True)


def captured_packet(path, expected, size, verifier, cert, callback=None):
    before = state(path,size)
    with Path(path).open('rb') as stream:
        s = os.fstat(stream.fileno());require((s.st_dev,s.st_ino) == before[:2],'packet capture substitution')
        raw = stream.read(size+1)
    if callback: callback()
    require(state(path,size) == before and len(raw) == size and sha(raw) == expected, 'consumed packet bytes/lifecycle')
    require(cert['v2']['digest_format'] == verifier['FORMAT'] and verifier['verify'](raw,cert['v2']),
            'same-buffer modern beginning seal')
    return raw,before


def enclosed(actual, expected, magnitude, cells):
    n = 32*cells;u = 2.0**-53
    require(n*u < 1 and all(math.isfinite(x) for x in (actual,expected,magnitude)), 'bounded source reduction')
    bound = n*u/(1-n*u)*(magnitude+abs(actual)+abs(expected))
    require(abs(actual-expected) <= bound, 'frozen source selected-ledger roundoff enclosure')
    return dict(actual=actual,derived=expected,roundoff_bound=bound,residual=actual-expected)


def derive(packet, nx, ny, nz, faces, dt, moments, source_components):
    cells=nx*ny*nz
    require(len(packet) == (28*cells+faces)*4+nx*ny,'beginning wire layout')
    sources=np.frombuffer(packet,'<f8',count=5*cells,offset=(18*cells+faces)*4).reshape(5,nz,ny,nx)
    require(np.isfinite(sources).all(),'finite frozen source operands')
    spatial=moments(sources[0],sources[1],DX,80)
    volume=DX**3
    expected=[spatial['native']['signed_chemical_power_W']*dt,
              float(sources[2].sum()*volume*dt),float(sources[3].sum()*volume),float(sources[4].sum()*volume)]
    magnitude=[float((np.abs(sources[0])+np.abs(sources[1])).sum()*volume*dt),
               float(np.abs(sources[2]).sum()*volume*dt),float(np.abs(sources[3]).sum()*volume),
               float(np.abs(sources[4]).sum()*volume)]
    require(len(source_components)==4,'selected ledger source component coverage')
    checks=[enclosed(a,e,m,cells) for a,e,m in zip(source_components,expected,magnitude)]
    separate={}
    for label,layers in [('native',nz),('common80',80)]:
        arrays=sources[:,:layers]
        separate[label]=dict(gas_signed_power_W=float(arrays[0].sum()*volume),
                             soot_signed_power_W=float(arrays[1].sum()*volume),
                             radiative_cooling_signed_power_W=float(arrays[2].sum()*volume),
                             pilot_step_energy_J=float(arrays[3].sum()*volume),
                             reacted_fuel_step_kg=float(arrays[4].sum()*volume))
    return dict(spatial=spatial,separate_owned_source_components=separate,
                native_selected_step_source_checks=checks,
                source_check_units=['J','J','J','kg'],q0_species_summaries='not derived by this Stage A reader')


def generated_stream_capture(path, size, generated_digest, callback=None):
    def identity():
        info=path.lstat()
        require(stat.S_ISREG(info.st_mode) and info.st_nlink==1,'nonaliased generated stream')
        # Access time changes on a valid read. Bind stable storage/publication
        # identity and the generated bytes; do not treat access as mutation.
        return (info.st_dev,info.st_ino,info.st_mode,info.st_nlink,info.st_size,
                info.st_mtime_ns,info.st_ctime_ns)
    before=identity();require(before[4]==size and size<=OUTPUT_LIMIT,'bounded generated stream')
    with path.open('rb') as stream:
        opened=os.fstat(stream.fileno());require((opened.st_dev,opened.st_ino)==before[:2],'stream reopen substitution')
        raw=stream.read(OUTPUT_LIMIT+1)
    if callback:callback()
    require(identity()==before and len(raw)==size and sha(raw)==generated_digest,
            'generated/captured completed stream identity')
    return raw


def publish(path, raw, verifier, case):
    require(len(raw)<=OUTPUT_LIMIT,'bounded publication')
    with path.open('xb') as stream:stream.write(raw)
    cert=dict(schema='rise.fire.published-payload.v2',case_record_id=case,issuer_role='new r247 frozen-source reader',
              sha256_v1=sha(raw),v2=verifier['merkle'](raw))
    with Path(str(path)+'.payload-v2.json').open('x') as stream:json.dump(cert,stream,indent=2)
    return sha(raw)


def run(output):
    require(not os.path.lexists(output),'fresh owner-only reader output')
    require(output.parent == EXT and all(not x.is_symlink() for x in output.parents),
            'external canonical reader output')
    contexts,identity=implementation();admission=contexts['admission'];verifier=contexts['verifier']
    raw,record=pinned_sealed(METADATA,METADATA_SHA,METADATA_SEAL_SHA,admission['CASES'][1][4],admission,verifier)
    metadata=json.loads(raw);del raw
    require(metadata['native_field_packets_read']==0 and metadata['beginning_payload_seals_verified'] is False and
            len(metadata['cases'])==2,'accepted metadata-only prerequisite scope')
    output.mkdir(mode=0o700);path=output/'source_spatial_rows.jsonl'
    pending=Path(str(path)+'.preparation.pending.payload-v2.json')
    pending.write_text('incomplete r247 source derivation\n')
    rows=0;size=0;plans=[];generated_hash=hashlib.sha256()
    with path.open('xb') as stream:
        for case in metadata['cases']:
            nx,ny,nz=case['dimensions'];cells=nx*ny*nz
            faces=(nx+1)*ny*nz+nx*(ny+1)*nz+nx*ny*(nz+1)
            plan=allocation_plan((28*cells+faces)*4+nx*ny,cells,peak_rss_bytes());plans.append(plan)
            for item in case['frames']:
                require(item['beginning_bytes']==plan['direct_buffer_components_bytes']['captured_packet'],'exact planned packet size')
                # Modern metadata captures consume pinned source bytes, preserving
                # the already accepted source/event linkage rather than rerunning its gate.
                for key in ('binding','ledger'):
                    rec=item[key]
                    captured,_=pinned_sealed(Path(rec['path']),rec['sha256'],rec['seal_sha256'],case['case'],admission,verifier)
                    if key=='ledger': ledger=captured
                source=admission['pairs'](line[2:] for line in ledger.decode().splitlines() if line.startswith('# '))
                cert_path=Path(item['beginning_path']+'.payload-v2.json')
                cert_raw=capture(cert_path,item['beginning_seal_sha256']);cert=json.loads(cert_raw)
                require(cert==item['beginning_seal'] and cert['sha256_v1']==item['beginning_sha256'],'exact beginning seal declaration')
                packet,before=captured_packet(item['beginning_path'],item['beginning_sha256'],item['beginning_bytes'],verifier,cert)
                result=derive(packet,nx,ny,nz,faces,item['dt_s'],contexts['moments']['source_moments'],
                              list(map(float,source['step_source'].split(','))))
                require(state(item['beginning_path'],item['beginning_bytes'])==before and
                        capture(cert_path,item['beginning_seal_sha256'])==cert_raw,'source pair lifecycle after derivation')
                del packet
                observed=peak_rss_bytes();require(observed<=READER_CEILING,'observed reader RSS ceiling')
                result.update(variant=case['variant'],source_case=case['case'],source_producer=case['producer'],
                              frame=item['frame'],accepted_step=item['accepted_step'],beginning_time_s=item['beginning_time_s'],
                              dt_s=item['dt_s'],accepted_event_s=item['accepted_event_s'],
                              consumed_beginning_sha256=item['beginning_sha256'],observed_peak_rss_bytes=observed)
                encoded=(json.dumps(result,allow_nan=False)+'\n').encode();require(len(encoded)<=ROW_LIMIT,'bounded spatial row')
                size+=len(encoded);require(size<=OUTPUT_LIMIT,'bounded full row stream')
                stream.write(encoded);generated_hash.update(encoded);rows+=1
    require(rows==470,'complete two235-event windows')
    # The completed stream is captured once; its modern seal derives from those bytes.
    raw=generated_stream_capture(path,size,generated_hash.hexdigest())
    cert=dict(schema='rise.fire.published-payload.v2',case_record_id=admission['CASES'][1][4],
              issuer_role='new r247 frozen-source reader',sha256_v1=sha(raw),v2=verifier['merkle'](raw))
    with Path(str(path)+'.payload-v2.json').open('x') as stream:json.dump(cert,stream,indent=2)
    pending.unlink()
    receipt=dict(schema='rise.fire.r247.source-spatial-reader.v1',rows=rows,metadata_receipt=record,
                 implementation_sha256=identity,rows_sha256=sha(raw),allocation_plans=plans,
                 measured_peak_rss_bytes=peak_rss_bytes(),original_taller_exit_code=98,
                 full_derivation_accepted=False,physical_acceptance=False,canonical_taller_qualification=False,
                 temporal_support='235 selected frozen beginning source states per variant; no15315 cumulative common80 budget',
                 optical_confound='Spatial source observations do not authenticate camera emission/extinction dynamics')
    publish(output/'reader_receipt.json',(json.dumps(receipt,indent=2)+'\n').encode(),verifier,admission['CASES'][1][4])
    print(json.dumps(dict(status='candidate_complete_derived_rows',rows=rows,peak_rss_bytes=receipt['measured_peak_rss_bytes'])))


def controls():
    contexts,identity=implementation();verifier=contexts['verifier'];red=0
    plan=allocation_plan(214776640,1730560,64*1024**2)
    require(plan['enumerated_peak_estimate_bytes']<READER_CEILING,'native direct allocation estimate GREEN')
    def refusal(call):
        nonlocal red
        try:call()
        except (ValueError,FileNotFoundError,FileExistsError):red+=1
        else:raise AssertionError('missing RED')
    refusal(lambda:allocation_plan(214776640,1730560,READER_CEILING))
    refusal(lambda:allocation_plan(PUBLIC_FIXTURE_CEILING+1,1730560,1))
    with tempfile.TemporaryDirectory() as directory:
        p=Path(directory).resolve()/'packet';raw=b'A'*8193;p.write_bytes(raw)
        cert=dict(v2=verifier['merkle'](raw))
        captured,_=captured_packet(p,sha(raw),len(raw),verifier,cert)
        require(captured==raw,'captured packet GREEN')
        refusal(lambda:captured_packet(p,sha(raw),len(raw)+1,verifier,cert))
        refusal(lambda:captured_packet(p,sha(b'B'),len(raw),verifier,cert))
        refusal(lambda:captured_packet(p,sha(raw),len(raw),verifier,dict(v2=dict(cert['v2'],chunk_bytes=8192))))
        refusal(lambda:captured_packet(p,sha(raw),len(raw),verifier,cert,lambda:p.write_bytes(b'B'*len(raw))))
    # Full80-layer synthetic source packet keeps the wire offsets realistic;
    # this is a small2x2 lateral fixture, not an empirical case admission mode.
    nx=ny=2;nz=80;cells=nx*ny*nz;faces=3*2*nz+2*3*nz+4*(nz+1)
    packet=bytearray((28*cells+faces)*4+nx*ny)
    fields=np.frombuffer(packet,'<f8',count=5*cells,offset=(18*cells+faces)*4).reshape(5,nz,ny,nx)
    fields[0,0,0,0]=1.;fields[1,0,1,1]=3.;fields[2,0,0,0]=2.;fields[3,0,0,0]=4.;fields[4,0,0,0]=5.
    expected=[4*DX**3*.5,2*DX**3*.5,4*DX**3,5*DX**3]
    result=derive(bytes(packet),nx,ny,nz,faces,.5,contexts['moments']['source_moments'],expected)
    require(result['spatial']['native']['centroid_cell_centre_xyz']==[1.25,1.25,.5] and
            result['spatial']['native']['positive_chemical_power_W']==4*DX**3,'synthetic wire source GREEN')
    refusal(lambda:derive(bytes(packet),nx,ny,nz,faces,.5,contexts['moments']['source_moments'],[0]*4))
    fields[0,0,0,0]=np.nan
    refusal(lambda:derive(bytes(packet),nx,ny,nz,faces,.5,contexts['moments']['source_moments'],expected))
    with tempfile.TemporaryDirectory() as directory:
        root=Path(directory).resolve();p=root/'artifact';p.write_bytes(b'original')
        refusal(lambda:publish(p,b'replacement',verifier,contexts['admission']['CASES'][1][4]))
        require(p.read_bytes()==b'original','occupied publication preserves bytes')
        refusal(lambda:run(root))
        pending=Path(str(p)+'.preparation.pending.payload-v2.json');pending.symlink_to(root/'absent')
        refusal(lambda:state(p));pending.unlink()
        alias=root/'alias';alias.symlink_to(root,target_is_directory=True)
        refusal(lambda:state(alias/'artifact'))
    with tempfile.TemporaryDirectory() as directory:
        p=Path(directory).resolve()/'sealed_metadata';p.write_bytes(b'captured metadata')
        case=contexts['admission']['CASES'][1][4]
        cert=dict(schema='rise.fire.published-payload.v2',case_record_id=case,
                  sha256_v1=sha(p.read_bytes()),v2=verifier['merkle'](p.read_bytes()))
        cert_path=Path(str(p)+'.payload-v2.json')
        a=json.dumps(cert,sort_keys=True).encode();b=json.dumps(cert,indent=2).encode()
        cert_path.write_bytes(a);digest=sha(p.read_bytes())
        require(pinned_sealed(p,digest,sha(a),case,contexts['admission'],verifier)[1]['seal_sha256']==sha(a),
                'pinned metadata pair GREEN')
        # Old successful initialA capture followed by unbound pair reopen accepts
        # valid semantic-equivalent B. The actual bound helper refuses B.
        capture(cert_path,sha(a));cert_path.write_bytes(b)
        require(contexts['admission']['sealed'](p,case,verifier,digest)[1]['seal_sha256']==sha(b) and sha(a)!=sha(b),
                'old unbound certificate substitution GREEN discriminator')
        for label in ('metadata_receipt','selected_frame','selected_ledger'):
            cert_path.write_bytes(a)
            refusal(lambda:pinned_sealed(p,digest,sha(a),case,contexts['admission'],verifier,
                                         lambda:cert_path.write_bytes(b)))
    with tempfile.TemporaryDirectory() as directory:
        p=Path(directory).resolve()/'generated';p.write_bytes(b'generated A')
        info=p.lstat();os.utime(p,ns=(946684800000000000,info.st_mtime_ns))
        before=p.lstat();raw=p.read_bytes();after=p.lstat()
        require(before!=after and before.st_atime_ns!=after.st_atime_ns and sha(raw)==sha(b'generated A'),
                'old full-stat equality falseRED on unchanged generated bytes discriminator')
        info=p.lstat();os.utime(p,ns=(946684800000000000,info.st_mtime_ns))
        require(generated_stream_capture(p,len(raw),sha(raw))==raw,'stable generated identity atime GREEN')
        p.write_bytes(b'generated B')
        refusal(lambda:generated_stream_capture(p,len(raw),sha(raw)))
        p.write_bytes(raw)
        refusal(lambda:generated_stream_capture(p,len(raw),sha(raw),lambda:p.write_bytes(b'generated B')))
        link=p.parent/'alias';os.link(p,link)
        refusal(lambda:generated_stream_capture(p,len(raw),sha(raw)))
    require(red==18,'reader RED coverage')
    print(json.dumps(dict(status='synthetic_reader_only',red=red,implementation_sha256=identity,
                          allocation_estimate=plan,native_source_packets_read=0)))


if __name__=='__main__':
    parser=argparse.ArgumentParser(description=__doc__);parser.add_argument('--controls',action='store_true')
    parser.add_argument('--run',type=Path);args=parser.parse_args();require(bool(args.run)!=args.controls,'one command')
    controls() if args.controls else run(args.run)
