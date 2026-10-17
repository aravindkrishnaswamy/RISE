#!/usr/bin/env python3
"""Strict recovered r246 full-budget admission; no fixture or physics qualification."""
import argparse
import copy
import csv
import io
import json
import math
from pathlib import Path
import struct
import stat
import tempfile

import r246_recovery_evidence as collector
import r246_observation_ledger_binding as helper

EXT = Path('/Volumes/SSD/RISE/fire_production_calibration')
RUN = EXT/'r246_tier6_top_double.v1'
COMPLETION = EXT/'r246_tier6_top_launch.v1/completion.json'
ACCEPTANCE = EXT/'r246_recovery_acceptance.v1/recovery_acceptance.json'
# Exact reviewed receipt pairs and retained source bytes, separate from original
# producer authentication. Populated below as immutable authored constants.
PINS = [
    {"path": str(RUN/"production_final.checkpoint"),
     "sha256": "0632960c2b045c8717e2d655cfc8bb340b918023f0fea1bfa1117a0aeaaca043",
     "seal_sha256": "7dc0d34973c61f7988e7a3af1e361ca45791afd06827276dab3671581ea62d3d"},
  {
    "path": "/Volumes/SSD/RISE/fire_production_calibration/r246_recovery_acceptance.v1/recovery_acceptance.json",
    "sha256": "ca1749bdd06da4d4062aa85e291cd7f9404df89b9827febe339e9d93ff66cc5c",
    "seal_sha256": "507ccb5660189eabd59129746f129602eb428c8010ce733257b86e952fbb4a1d"
  },
  {
    "path": "/Volumes/SSD/RISE/fire_production_calibration/r246_recovery_evidence.v1/recovery_evidence.json",
    "sha256": "2e2e9fe96a5deec29b6a88b234aaaa14e670f5b7d8a191f7130f05cd246a424c",
    "seal_sha256": "99fe35128f145f2a01c79d82c4b65964e177c20ce9c8ed4e7613d021fd251b59"
  },
  {
    "path": "/Volumes/SSD/RISE/fire_production_calibration/r246_recovery_operand_validation.v3/actual_operands/checkpoint_operands.json",
    "sha256": "81df738946e1f8537a7845554fd1b81158dd0b6303fd3617ead2e1a64a4fce33",
    "seal_sha256": "1e3406372d5d5adcf0f06b0fe530b56eb825fd4c779c6d6ba95bd650ce77a852"
  },
  {
    "path": "/Volumes/SSD/RISE/fire_production_calibration/r246_recovery_operand_validation.v3/actual_operands/accepted_dt.csv",
    "sha256": "5a25547d3706a346e479c0360f83b0438b624db2b5879b27913377b953df1821",
    "seal_sha256": "9737b9593b61d6aefd7211e1a8db2623597f876ad1222f9d0f2f3af99e95311b"
  },
  {
    "path": "/Volumes/SSD/RISE/fire_production_calibration/r246_observation_ledger_validation.v7/ledger_binding.json",
    "sha256": "3a27bc90f840e12b98303104654d87acba4a59ce7448b65140d6c46bea0fc19c",
    "seal_sha256": "e7b7629b7cca79d5df8795bd6861a5f7c57a0af62b40684f82e3df5cef2d184d"
  },
  {
    "path": "/Volumes/SSD/RISE/fire_production_calibration/r246_capsule_binding_validation.v1/capsule_binding.json",
    "sha256": "4774e99c6b273b3530adf262d4072a848288324686e74f29c25103f9ce7055c2",
    "seal_sha256": "19fc71ac38037d0c29b121afd491a99e1bad73d4ab6e2f75f6b206f8f5826d39"
  },
  {
    "path": "/Volumes/SSD/RISE/fire_production_calibration/r246_tier6_top_double.v1/window_launch.v1",
    "sha256": "bd9df5efcf3d8b2f9e43951b06574fa536b4c793b42453bf0612acdd9cdb751f",
    "seal_sha256": "0c4c12cfcca64a6bf10dc751139072865a1719a47802aef45372f388ae3e3b5d"
  },
  {
    "path": "/Volumes/SSD/RISE/fire_production_calibration/r246_tier6_top_double.v1/frame_times.csv",
    "sha256": "3bb265a8539d5de45356dba419024c077834e3bb4c3770d75c63478006a5a5c8"
  },
  {
    "path": "/Volumes/SSD/RISE/fire_production_calibration/r246_tier6_top_launch.v1/completion.json",
    "sha256": "8d664320d24952999b5d4d97028a05a8b0994392b16074ee027c53ce14e2e017"
  },
  {
    "path": "/Users/aravind/Working/RISE/worktrees/r243-structure-diagnostic/docs/data/fire_r245/r246_complete_recovery_review.json",
    "sha256": "440f082dfba5b9094bad89f3f19ef5878eb6492d7ee68727329d5a2b3d616c4a"
  }
]


def require(condition, message):
    if not condition:
        raise ValueError(message)


def bound_code(audit_namespace=None):
    paths = dict(admission=Path(__file__).resolve(), collector=Path(collector.__file__).resolve(),
                 helper=Path(helper.__file__).resolve(), verifier=Path(helper.fire_payload_merkle.__file__).resolve())
    if audit_namespace is not None:
        paths['audit'] = Path(audit_namespace['__file__']).resolve()
    code = {name: collector.capture(path,helper.sha(path.read_bytes())) for name,path in paths.items()}
    require(helper.sha(code['collector']) == '9a0564701f0467f67870b6f5e7f91e40f1d44c7a9b1b6a9ffd3df2596ac72cdf' and
            helper.sha(code['helper']) == collector.HELPER_SHA and helper.sha(code['verifier']) == collector.VERIFIER_SHA,
            'accepted capture/verifier code')
    for name, loaded in [('admission',globals()),('collector',collector.__dict__),('helper',helper.__dict__)]:
        namespace=dict(__name__='captured_'+name,__file__=str(paths[name]))
        exec(compile(code[name],str(paths[name]),'exec'),namespace)
        helper.require_loaded_functions_match(loaded,namespace)
        constants = ('EXT','RUN','COMPLETION','ACCEPTANCE','PINS') if name == 'admission' else (
                    ('CATALOG','CATALOG_SHA','HELPER_SHA','VERIFIER_SHA','EXT','UNAVAILABLE') if name == 'collector' else
                    ('CASE','PRODUCER','EXECUTABLE','PINS'))
        require(all(loaded[k] == namespace[k] for k in constants),'captured constants: '+name)
    verifier=dict(__name__='captured_budget_verifier',__file__=str(paths['verifier']))
    exec(compile(code['verifier'],str(paths['verifier']),'exec'),verifier)
    if audit_namespace is not None:
        namespace=dict(__name__='captured_audit',__file__=str(paths['audit']))
        exec(compile(code['audit'],str(paths['audit']),'exec'),namespace)
        helper.require_loaded_functions_match(audit_namespace,namespace)
        audit_namespace.update({name:verifier[name] for name in ('FORMAT','merkle','verify')})
    return verifier,{name:helper.sha(raw) for name,raw in code.items()}


def validate_history(dts, frames, ledger, capsule):
    require(len(dts) == len(ledger['ledger_bindings']) == 15315 and len(frames) == len(capsule['frames']) == 235,
            'complete retained budget history')
    time = 0.0
    events = []
    event = 0
    for index,row in enumerate(dts,1):
        dt=float(row['dt_s'])
        require(math.isfinite(dt) and 0 < dt <= 3.4028234663852886e38 and float(struct.unpack('<f',struct.pack('<f',dt))[0]) == dt and
                int(row['step']) == index and float(row['beginning_time_s']) == time,
                'retained binary32 sequential dt')
        beginning=time
        time += dt
        require(float(row['time_s']) == time and ledger['ledger_bindings'][index-1]['step'] == index,
                'retained sequential endpoint')
        if event < 235:
            prescribed = 10.531864278900095 + event*0.0625
            remaining=prescribed-beginning
            ceiling = struct.unpack('<f',struct.pack('<f',remaining))[0]
            if ceiling < remaining:
                bits = struct.unpack('<I',struct.pack('<f',ceiling))[0]+1
                ceiling=struct.unpack('<f',struct.pack('<I',bits))[0]
            if time >= prescribed:
                require(remaining > 0 and dt == ceiling, 'first outward remaining-dt ceiling')
                events.append((index,time)); event += 1
        else:
            raise ValueError('history extends beyond final event')
    require(time == 25.156864278920693 and len(events) == 235, 'full matched window endpoint')
    for index,(frame,(step,time)) in enumerate(zip(frames,events)):
        require(int(frame['frame']) == index and float(frame['simulation_time_s']) == time and
                capsule['frames'][index]['frame'] == index and capsule['frames'][index]['accepted_step'] == step,
                'outward accepted event binding')


def capture_original_checkpoint(record, verifier):
    # The original checkpoint has retained hardlink aliases created by earlier
    # diagnostics. Match the accepted native source capture contract: exact
    # canonical source path, regular nonsymlink payload, nonaliased modern seal,
    # no pending publication, retained bytes pinned to original digest. No loader
    # reopens this path and no new recovery artifact receives this exception.
    path=RUN/'production_final.checkpoint'
    require(record['path']==str(path), 'original checkpoint exception path')
    pending=Path(str(path)+'.preparation.pending.payload-v2.json')
    def identity():
        require(not pending.exists() and not pending.is_symlink(), 'original checkpoint pending')
        info=path.lstat()
        require(stat.S_ISREG(info.st_mode), 'original checkpoint regular nonsymlink')
        return (info.st_dev,info.st_ino,info.st_size,info.st_mtime_ns,info.st_ctime_ns)
    before=identity(); raw=path.read_bytes()
    cert=json.loads(collector.capture(Path(str(path)+'.payload-v2.json'),record['seal_sha256']))
    require(identity()==before and helper.sha(raw)==record['sha256'] and
            cert['schema']=='rise.fire.published-payload.v2' and cert['case_record_id']==helper.CASE and
            cert['sha256_v1']==helper.sha(raw) and cert['v2']['digest_format']==verifier['FORMAT'] and
            verifier['verify'](raw,cert['v2']), 'original checkpoint captured seal/pin')
    return raw


def admit(directory, completion, acceptance, audit_namespace=None):
    require(directory == RUN/'budgets' and directory.resolve() == directory and completion == COMPLETION and
            acceptance == ACCEPTANCE,'exact full r246 recovery/source paths')
    verifier,identity=bound_code(audit_namespace)
    raw={r['path']:capture_original_checkpoint(r,verifier) if r['path']==str(RUN/'production_final.checkpoint') else
         collector.sealed_record(r,verifier) if 'seal_sha256' in r else
         collector.capture(Path(r['path']),r['sha256']) for r in PINS}
    def named(name):
        matches=[value for path,value in raw.items() if Path(path).name == name]
        require(len(matches)==1,'unique admission operand')
        return matches[0]
    receipt=json.loads(named('recovery_acceptance.json'))
    require(receipt['recovery_accepted'] is True and receipt['original_run_success'] is False and
            receipt['original_exit_code']==98 and receipt['source_case_record_id']==helper.CASE and
            receipt['source_producer_build_id']==helper.PRODUCER and receipt['original_executable_sha256']==helper.EXECUTABLE and
            receipt['canonical_taller_qualification'] is False and receipt['physical_acceptance'] is False and
            receipt['signed_budget_audit_accepted'] is False,'scoped data recovery admission')
    require(json.loads(named('completion.json'))['exit_code']==98,'preserved source failure')
    launch=dict(line.split(' ',1) for line in named('window_launch.v1').decode().splitlines())
    require([launch[k] for k in ('case_record_id','producer_build_id','producer_executable_sha256')]==
            [helper.CASE,helper.PRODUCER,helper.EXECUTABLE] and launch['domain_study']=='r246-lateral-double-top-double-v1' and
            launch['from_zero']=='true' and launch['accepted_stage_budget']=='r245-v1' and
            launch['canonical_taller_qualification']=='false' and launch['resolution_tier']=='6' and
            launch['matched_schedule_flow_through_time_hex']=='0x1.0d9da03060ef6p+1' and
            launch['statistics_start_s']=='10.531864278900095' and launch['statistics_duration_s']=='14.625' and
            launch['target_time_s']=='25.156864278900095' and launch['frame_cadence_s']=='0.0625',
            'strict taller matched-clock observation contract')
    meta=json.loads(named('checkpoint_operands.json')); ledger=json.loads(named('ledger_binding.json'))
    capsule=json.loads(named('capsule_binding.json'))
    require(meta['source_checkpoint_sha256']=='0632960c2b045c8717e2d655cfc8bb340b918023f0fea1bfa1117a0aeaaca043' and
            meta['artifacts_sha256']['accepted_dt.csv']==helper.sha(named('accepted_dt.csv')) and
            ledger['source_metadata_sha256']==helper.sha(named('checkpoint_operands.json')) and
            capsule['ledger_binding_sha256']==helper.sha(named('ledger_binding.json')) and
            capsule['frame_csv_sha256']==helper.sha(named('frame_times.csv')),'retained source chain')
    dts=list(csv.DictReader(io.StringIO(named('accepted_dt.csv').decode())))
    frames=list(csv.DictReader(io.StringIO(named('frame_times.csv').decode())))
    validate_history(dts,frames,ledger,capsule)
    return dict(case=helper.CASE,provenance=[helper.CASE,helper.PRODUCER,helper.EXECUTABLE],dts=dts,frames=frames,
                ledger=ledger['ledger_bindings'],capsules=capsule['frames'],implementation_sha256=identity,
                recovery_acceptance_sha256=helper.sha(named('recovery_acceptance.json')),
                admitted_inputs_sha256={path:helper.sha(value) for path,value in raw.items()})


def ledger_matches(context,index,metadata,digest):
    dt=context['dts'][index-1]; binding=context['ledger'][index-1]
    require(digest==binding['ledger_sha256'] and [metadata[k] for k in
            ('case_record_id','producer_build_id','executable_sha256')]==context['provenance'] and
            metadata['dimensions']=='104 104 160' and metadata['binary32_solver_dx_m']=='0.040790822356939316' and
            metadata['statistics_start_s']=='10.531864278900095' and int(metadata['accepted_step'])==index and
            float(metadata['beginning_time_s'])==float(dt['beginning_time_s']) and
            float(metadata['dt_s'])==float(dt['dt_s']) and
            all(metadata[k]==binding[k] for k in ('input_root','native_root','flux_root')),
            'captured ledger exact retained history/domain/provenance')


def controls(context):
    reds=0
    def red(action):
        nonlocal reds
        try: action()
        except (ValueError,FileNotFoundError): reds+=1
        else: raise ValueError('false admission GREEN')
    for directory,completion,acceptance in [(RUN/'foreign',COMPLETION,ACCEPTANCE),
            (RUN/'budgets',COMPLETION.with_name('foreign'),ACCEPTANCE),
            (RUN/'budgets',COMPLETION,ACCEPTANCE.with_name('foreign'))]:
        red(lambda:admit(directory,completion,acceptance))
    ledger_path=RUN/'budgets/step_000001.csv'
    from r245_budget_audit import read_record, sealed, require_fresh_recovered_output
    ledger_raw=sealed(ledger_path,helper.CASE)
    metadata,_=read_record(ledger_path,ledger_raw)
    digest=helper.sha(ledger_raw)
    ledger_matches(context,1,metadata,digest)
    for key,value in [('case_record_id','foreign'),('producer_build_id','foreign'),('executable_sha256','foreign'),
            ('dimensions','104 104 80'),('binary32_solver_dx_m','0.04'),('statistics_start_s','0'),
            ('beginning_time_s','1'),('dt_s','0.1'),('accepted_step','2'),('native_root','foreign')]:
        mutant=dict(metadata); mutant[key]=value
        red(lambda:ledger_matches(context,1,mutant,digest))
    red(lambda:ledger_matches(context,1,metadata,'foreign'))
    dts=copy.deepcopy(context['dts']); dts[1]['dt_s']=dts[0]['dt_s']='0.001'
    red(lambda:validate_history(dts,context['frames'],{'ledger_bindings':context['ledger']},{'frames':context['capsules']}))
    old_loose_float_greens=0
    with tempfile.TemporaryDirectory() as temporary:
        root=Path(temporary).resolve(); payload=root/'unit'; payload.write_bytes(b'A')
        certpath=Path(str(payload)+'.payload-v2.json')
        v2=helper.fire_payload_merkle.merkle(b'A')
        base=dict(schema='rise.fire.published-payload.v2',case_record_id=helper.CASE,sha256_v1=helper.sha(b'A'),v2=v2)
        certpath.write_text(json.dumps(base))
        require(sealed(payload,helper.CASE)==b'A','modern sealed unit GREEN')
        for key in ('digest_version','chunk_bytes','fan_in','payload_bytes'):
            for kind in (float,bool):
                mutant=copy.deepcopy(base); mutant['v2'][key]=kind(v2[key])
                certpath.write_text(json.dumps(mutant))
                if kind is float:
                    require(mutant['v2']==v2,'old loose float discriminator')
                    old_loose_float_greens+=1
                red(lambda:sealed(payload,helper.CASE))
        output=root/'audit.json'; output.write_bytes(b'A')
        red(lambda:require_fresh_recovered_output(output))
        output.unlink(); Path(str(output)+'.payload-v2.json').symlink_to(root/'absent')
        red(lambda:require_fresh_recovered_output(output))
    return dict(actual_admission_green=1,ledger_green=1,modern_unit_green=1,targeted_reds=reds,
                old_loose_float_greens=old_loose_float_greens,full_arithmetic_audit=False)


def main():
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--inspect',action='store_true',required=True)
    args=parser.parse_args()
    context=admit(RUN/'budgets',COMPLETION,ACCEPTANCE)
    print(json.dumps(dict(controls=controls(context),steps=len(context['dts']),frames=len(context['frames']),
                         implementation_sha256=context['implementation_sha256'],
                         admitted_inputs_sha256=context['admitted_inputs_sha256'])))


if __name__=='__main__':
    main()
