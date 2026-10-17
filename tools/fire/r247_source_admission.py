#!/usr/bin/env python3
"""Capture strict Stage A metadata admission, without reading native field packets.

Wide beginning/frame hashes are newly captured original modern publications;
only taller hashes are also pinned by the accepted capsule inventory.
"""
import argparse
import csv
import hashlib
import io
import json
import math
import os
from pathlib import Path
import stat
import struct
import tempfile
import types

EXT = Path('/Volumes/SSD/RISE/fire_production_calibration')
ROOT = Path('/Users/aravind/Working/RISE/worktrees/r243-structure-diagnostic')
VERIFIER_SHA = 'a6219a285792b671d6a33de41c73556d9c970f8aed32c034cacaa0c3ea405ebf'
CASES = (
 ('wide', 'r245_tier6_lateral_budget.v1', 'r245_full_verification.v2/full_budget_audit.json',
  '085f5674499c6677e6f356c5a1e18b1d8128e3e5062d1f987106c66ea9ad9c38',
  'a9369e770465bd76e2cc8d5fc3b79e7c2aac038ecb14a0e4424a97ba2f98d586',
  'ec0854d960c84d4c2a432c0234dd29b35cbc416175f5a5a2d3327df54c0ef5c4', 80),
 ('taller', 'r246_tier6_top_double.v1', 'r246_full_signed_budget_audit.v1/signed_budget_audit.json',
  '8105dc52c044e6e39e8937fc36baa2be60f8e2e2a3d9f75a9384fc19646b3e72',
  '805372d567af6fafd03ba0ec96c8fbd30d64fdc21c64117dc2ea5580be9b52a2',
  '2e19f9326f6987bf577a7485b4aee92d3ebb6cb1212d4fe0b2df42f1db2b371b', 160))
CAPSULE = 'r246_capsule_binding_validation.v1/capsule_binding.json'
CAPSULE_SHA = '4774e99c6b273b3530adf262d4072a848288324686e74f29c25103f9ce7055c2'
LIMIT = 16*1024*1024


def require(ok, message):
    if not ok:
        raise ValueError(message)


def sha(raw):
    return hashlib.sha256(raw).hexdigest()


def capture(path, expected=None, callback=None):
    path = Path(path)
    pending = Path(str(path)+'.preparation.pending.payload-v2.json')
    def state():
        require(not os.path.lexists(pending), 'pending publication')
        s = path.lstat()
        require(stat.S_ISREG(s.st_mode) and s.st_nlink == 1 and s.st_size <= LIMIT,
                'regular nonaliased bounded metadata')
        return (s.st_dev, s.st_ino, s.st_size, s.st_mtime_ns, s.st_ctime_ns)
    before = state()
    with path.open('rb') as stream:
        s = os.fstat(stream.fileno())
        require((s.st_dev, s.st_ino) == before[:2], 'capture substitution')
        raw = stream.read(LIMIT+1)
    if callback:
        callback()
    require(state() == before and len(raw) == before[2], 'capture lifecycle changed')
    require(expected is None or sha(raw) == expected, 'exact metadata pin')
    return raw


def sealed(path, case, verifier, expected=None):
    raw = capture(path, expected)
    certificate = capture(Path(str(path)+'.payload-v2.json'))
    cert = json.loads(certificate)
    require(capture(path, sha(raw)) == raw and
            capture(Path(str(path)+'.payload-v2.json'), sha(certificate)) == certificate, 'pair changed')
    require(cert['schema'] == 'rise.fire.published-payload.v2' and cert['case_record_id'] == case and
            cert['sha256_v1'] == sha(raw) and cert['v2']['digest_format'] == verifier['FORMAT'] and
            verifier['verify'](raw, cert['v2']), 'mandatory modern metadata seal')
    return raw, dict(path=str(path), sha256=sha(raw), seal_sha256=sha(certificate))


def pairs(lines):
    result = {}
    for line in lines:
        key, value = line.split(' ', 1)
        require(key not in result, 'duplicate metadata key')
        result[key] = value
    return result


def admit(binding_raw, ledger_raw, cert_raw, proof, frame, event, case, producer, nz, pinned=None):
    require(type(frame) is int and type(proof['frame']) is int and type(proof['accepted_step']) is int and
            proof['frame'] == frame and 6400 <= proof['accepted_step'] <= 15315, 'typed selected ordinal')
    binding = pairs(binding_raw.decode().splitlines())
    require(set(binding) == {'schema','accepted_step','frame','step_record_sha256','.flux.bin','.native.bin',
                            '.beginning.bin','columns_sha256'}, 'frame record fields')
    require(all(type(binding[k]) is str and len(binding[k]) == 64 and
                all(c in '0123456789abcdef' for c in binding[k]) for k in
                ('step_record_sha256','.flux.bin','.native.bin','.beginning.bin','columns_sha256')),
            'frame SHA256 encoding')
    metadata = pairs(line[2:] for line in ledger_raw.decode().splitlines() if line.startswith('# '))
    require(binding['schema'] == 'rise.fire.accepted-stage-frame.v1' and binding['frame'] == str(frame) and
            binding['accepted_step'] == metadata['accepted_step'] == str(proof['accepted_step']) and
            binding['step_record_sha256'] == sha(ledger_raw), 'frame/ledger linkage')
    require(metadata['schema'] == 'rise.fire.accepted-stage-budget.v1' and metadata['case_record_id'] == case and
            metadata['producer_build_id'] == producer and metadata['dimensions'] == f'104 104 {nz}' and
            metadata['binary32_solver_dx_m'] == '0.040790822356939316' and
            metadata['statistics_start_s'] == '10.531864278900095', 'source/domain identity')
    require(metadata['native_root'] == proof['native_root'] and metadata['flux_root'] == proof['flux_root'] and
            binding['columns_sha256'] == proof['columns_sha256'], 'accepted audit roots/columns')
    beginning, dt = float(metadata['beginning_time_s']), float(metadata['dt_s'])
    require(math.isfinite(beginning) and math.isfinite(dt) and dt > 0 and
            struct.unpack('<f', struct.pack('<f', dt))[0] == dt and beginning+dt == event,
            'exact accepted binary32 dt/event')
    require(all(len(metadata[k]) == 64 and all(c in '0123456789abcdef' for c in metadata[k])
                for k in ('input_root','native_root','flux_root')), 'root encoding')
    cells = 104*104*nz
    faces = 105*104*nz+104*105*nz+104*104*(nz+1)
    size = (28*cells+faces)*4+104*104
    cert = json.loads(cert_raw)
    require(cert['schema'] == 'rise.fire.published-payload.v2' and cert['case_record_id'] == case and
            cert['sha256_v1'] == binding['.beginning.bin'] and cert['v2']['digest_format'] ==
            'rise-payload-sha256-merkle' and all(type(cert['v2'][k]) is int for k in
            ('digest_version','chunk_bytes','fan_in','payload_bytes')) and
            [cert['v2'][k] for k in ('digest_version','chunk_bytes','fan_in','payload_bytes')] ==
            [2,4096,16,size], 'beginning modern seal declaration')
    require(len(cert['v2']['root_sha256']) == 64 and all(c in '0123456789abcdef' for c in
            cert['v2']['root_sha256']), 'beginning root encoding')
    if pinned is not None:
        require(sha(binding_raw) == pinned['binding_sha256'] and
                binding['.beginning.bin'] == pinned['payload_sha256']['.beginning.bin'] and
                metadata['input_root'] == pinned['input_root'], 'accepted taller capsule pins')
    return dict(frame=frame, accepted_step=proof['accepted_step'], beginning_time_s=beginning,
                dt_s=dt, accepted_event_s=event, input_root=metadata['input_root'],
                native_root=metadata['native_root'], flux_root=metadata['flux_root'],
                beginning_sha256=binding['.beginning.bin'], beginning_bytes=size,
                beginning_seal_sha256=sha(cert_raw), beginning_seal=cert)


def code_key(code):
    return (code.co_code, code.co_names, code.co_varnames, code.co_freevars, code.co_cellvars,
            code.co_argcount, code.co_kwonlyargcount, code.co_posonlyargcount, code.co_flags,
            tuple(code_key(c) if isinstance(c, types.CodeType) else (type(c).__name__,c)
                  for c in code.co_consts), getattr(code,'co_exceptiontable',b''))


def implementation():
    own_path = Path(__file__); raw = capture(own_path)
    own = {'__name__':'captured_admission','__file__':str(own_path)}
    exec(compile(raw,str(own_path),'exec'),own)
    for name, function in list(globals().items()):
        if isinstance(function, types.FunctionType) and function.__module__ == __name__:
            require(code_key(function.__code__) == code_key(own[name].__code__) and
                    function.__defaults__ == own[name].__defaults__ and
                    function.__kwdefaults__ == own[name].__kwdefaults__, 'loaded implementation changed')
    require(all(globals()[k] == own[k] for k in ('EXT','ROOT','VERIFIER_SHA','CASES','CAPSULE','CAPSULE_SHA','LIMIT')),
            'loaded constants changed')
    verifier_raw = capture(ROOT/'tools/fire_payload_merkle.py', VERIFIER_SHA)
    verifier = {'__name__':'captured_verifier'}
    exec(compile(verifier_raw,'captured_verifier','exec'),verifier)
    return verifier, dict(script=sha(raw),verifier=sha(verifier_raw))


def inspect(output):
    require(not os.path.lexists(output), 'occupied output')
    verifier, identity = implementation()
    capsule_raw, capsule_record = sealed(EXT/CAPSULE, CASES[1][4], verifier, CAPSULE_SHA)
    capsule = json.loads(capsule_raw)
    cases = []
    for variant, run, audit_path, audit_sha, case, producer, nz in CASES:
        audit_raw = capture(EXT/audit_path, audit_sha); audit = json.loads(audit_raw)
        require(audit['case'] == case and audit['steps'] == 15315 and len(audit['frames']) == 235 and
                audit['end_time_s'] == 25.156864278920693, 'accepted full audit window')
        csv_path = EXT/run/'frame_times.csv' if variant == 'wide' else EXT/'r246_preview_source_snapshot.v8/frame_times.csv'
        csv_raw, csv_record = sealed(csv_path, case, verifier, capsule['frame_csv_sha256'] if variant == 'taller' else None)
        if variant == 'taller':
            require(capture(EXT/run/'frame_times.csv', capsule['frame_csv_sha256']) == csv_raw,
                    'accepted captured CSV equals original unsealed source bytes')
            csv_record['seal_issuer_scope'] = 'accepted snapshot reader seal; original CSV has no original sidecar'
        events = list(csv.DictReader(io.StringIO(csv_raw.decode())))
        require(len(events) == 235 and [e['frame'] for e in events] == [str(i) for i in range(235)], 'event cardinality')
        records = []
        for frame, proof in enumerate(audit['frames']):
            directory = EXT/run/'budgets'; base = directory/f'frame_{frame:05d}'
            binding, binding_record = sealed(Path(str(base)+'.v1'),case,verifier)
            ledger, ledger_record = sealed(directory/f"step_{proof['accepted_step']:06d}.csv",case,verifier)
            seal_path = Path(str(base)+'.beginning.bin.payload-v2.json'); cert = capture(seal_path)
            item = admit(binding,ledger,cert,proof,frame,float(events[frame]['simulation_time_s']),case,producer,nz,
                         capsule['frames'][frame] if variant == 'taller' else None)
            item.update(binding=binding_record,ledger=ledger_record,beginning_path=str(base)+'.beginning.bin')
            records.append(item)
        require(records[0]['accepted_step'] == 6400 and records[-1]['accepted_step'] == 15315 and
                records[-1]['accepted_event_s'] == 25.156864278920693 and
                all(a['accepted_step'] < b['accepted_step'] and a['accepted_event_s'] < b['accepted_event_s']
                    for a,b in zip(records,records[1:])), 'complete ordered selected window')
        cases.append(dict(variant=variant,case=case,producer=producer,dimensions=[104,104,nz],
                          audit=dict(path=str(EXT/audit_path),sha256=audit_sha),frame_csv=csv_record,frames=records,
                          beginning_pin_origin='accepted taller capsule receipt' if variant == 'taller' else
                          'new captured original modern frame and beginning-seal declarations; absent from old audit inventory'))
    report = dict(schema='rise.fire.r247.source-metadata-admission.v1',implementation_sha256=identity,
                  capsule_receipt=capsule_record,cases=cases,native_field_packets_read=0,
                  beginning_payload_seals_verified=False,field_allocation_or_full_derivation_accepted=False,
                  original_taller_exit_code=98,scope='Metadata/event/declaration prerequisite only; reader must verify captured beginning bytes before moments')
    output.mkdir(mode=0o700)
    raw = (json.dumps(report,indent=2,allow_nan=False)+'\n').encode()
    with (output/'source_metadata.json').open('xb') as stream: stream.write(raw)
    seal = dict(schema='rise.fire.published-payload.v2',case_record_id=CASES[1][4],
                issuer_role='new r247 metadata reader, not original producer',sha256_v1=sha(raw),v2=verifier['merkle'](raw))
    with (output/'source_metadata.json.payload-v2.json').open('x') as stream: json.dump(seal,stream,indent=2)
    print(json.dumps(dict(status='metadata_only',frames_per_case=235,native_field_packets_read=0,report_sha256=sha(raw))))


def controls():
    red = 0
    with tempfile.TemporaryDirectory() as directory:
        p = Path(directory)/'input';p.write_bytes(b'A'); h=sha(b'A')
        require(capture(p,h) == b'A','capture GREEN')
        def refusal(call):
            nonlocal red
            try: call()
            except (ValueError,FileNotFoundError): red += 1
            else: raise AssertionError('missing RED')
        refusal(lambda:capture(p,sha(b'B')))
        refusal(lambda:capture(p,h,lambda:p.write_bytes(b'B')));p.write_bytes(b'A')
        link=Path(directory)/'link';link.symlink_to(p);refusal(lambda:capture(link,h));link.unlink()
        os.link(p,link);refusal(lambda:capture(p,h));link.unlink()
        pending=Path(str(p)+'.preparation.pending.payload-v2.json');pending.symlink_to(Path(directory)/'absent')
        refusal(lambda:capture(p,h));pending.unlink()
        refusal(lambda:capture(p,h,lambda:pending.write_bytes(b'')));pending.unlink()
    require(red == 6,'lifecycle RED coverage')
    case, producer, nz = CASES[0][4], CASES[0][5], 80
    proof = dict(frame=0,accepted_step=6400,native_root='a'*64,flux_root='b'*64,columns_sha256='c'*64)
    metadata = dict(schema='rise.fire.accepted-stage-budget.v1',case_record_id=case,producer_build_id=producer,
                    accepted_step='6400',dimensions='104 104 80',binary32_solver_dx_m='0.040790822356939316',
                    statistics_start_s='10.531864278900095',beginning_time_s='10',dt_s='0.5',
                    input_root='d'*64,native_root='a'*64,flux_root='b'*64)
    def ledger_bytes(meta): return ''.join('# '+k+' '+v+'\n' for k,v in meta.items()).encode()
    ledger = ledger_bytes(metadata)
    binding = dict(schema='rise.fire.accepted-stage-frame.v1',accepted_step='6400',frame='0',
                   step_record_sha256=sha(ledger),columns_sha256='c'*64)
    binding.update({suffix:'e'*64 for suffix in ('.flux.bin','.native.bin','.beginning.bin')})
    def binding_bytes(item): return ''.join(k+' '+v+'\n' for k,v in item.items()).encode()
    cert = dict(schema='rise.fire.published-payload.v2',case_record_id=case,sha256_v1='e'*64,
                v2=dict(digest_format='rise-payload-sha256-merkle',digest_version=2,chunk_bytes=4096,
                        fan_in=16,payload_bytes=107415360,root_sha256='f'*64))
    def call(b=binding,m=metadata,c=cert,p=proof,event=10.5,pin=None):
        return admit(binding_bytes(b),ledger_bytes(m),json.dumps(c).encode(),p,0,event,case,producer,nz,pin)
    require(call()['accepted_event_s'] == 10.5,'synthetic declaration GREEN; no payload attestation')
    for key,value in [('case_record_id','foreign'),('producer_build_id','foreign'),('dimensions','104 104 160'),
                      ('binary32_solver_dx_m','0.040790821423214683'),('native_root','0'*64),('dt_s','0.1')]:
        m=dict(metadata,**{key:value});b=dict(binding,step_record_sha256=sha(ledger_bytes(m)))
        refusal(lambda b=b,m=m:call(b=b,m=m))
    refusal(lambda:call(event=10.500000000000002))
    refusal(lambda:call(p=dict(proof,accepted_step=6400.0)))
    refusal(lambda:call(b=dict(binding,extra='x')))
    refusal(lambda:call(b=dict(binding,step_record_sha256='0'*64)))
    for key,value in [('digest_version',2.0),('payload_bytes',107415360.0),('chunk_bytes',8192)]:
        c=dict(cert,v2=dict(cert['v2'],**{key:value}));refusal(lambda c=c:call(c=c))
    refusal(lambda:call(pin=dict(binding_sha256='0'*64,payload_sha256={'.beginning.bin':'e'*64},input_root='d'*64)))
    for bad in ('x','e'*63,'E'*64):
        b=dict(binding,**{'.beginning.bin':bad});c=dict(cert,sha256_v1=bad)
        refusal(lambda b=b,c=c:call(b=b,c=c))
    require(red == 23,'lifecycle/declaration RED coverage')
    print(json.dumps(dict(status='synthetic_metadata_lifecycle_only',red=red,metadata_green=True,
                          native_field_packets_read=0,beginning_payload_seals_verified=False)))


if __name__ == '__main__':
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--inspect',type=Path);parser.add_argument('--controls',action='store_true')
    args=parser.parse_args()
    require(bool(args.inspect) != args.controls,'one command required')
    controls() if args.controls else inspect(args.inspect)
