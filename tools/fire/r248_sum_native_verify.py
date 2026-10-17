#!/usr/bin/env python3
"""Verify retained standalone synthetic Metal outputs; no launches or native fields."""
import hashlib
import json
from pathlib import Path
import struct
import sys
ROOT=Path('/Users/aravind/Working/RISE/worktrees/r243-structure-diagnostic/tools/fire')
SUM_SHA='c79be0c3bb79b3939d5c9015ebcee2908abc33a2a15175cb6eac5e57557a1ec2'

def verify(base):
    raw=(ROOT/'r248_resident_sum_contract.py').read_bytes()
    assert hashlib.sha256(raw).hexdigest()==SUM_SHA
    c={'__name__':'captured_accepted_sum'};exec(compile(raw,'captured_sum','exec'),c)
    a=json.loads((base/'controls.json').read_bytes());assert a['green']==6 and a['red']==6
    lifecycle=json.loads((base/'descriptor_lifecycle.json').read_bytes())
    assert lifecycle['regular_to_fifo_nonblocking_refused'] is True and lifecycle['regular_to_symlink_refused'] is True
    assert lifecycle['opened_descriptor_regular_required'] is True
    source=(base/'r248_sum.metal').read_bytes();greens=0;reds=0
    for case in a['cases']:
        if case.get('control')=='report_failure':
            assert case['process_exit_code']==9 and case['receipt_published'] is False and case['result_unsealed'] is True
            data=(base/'report_failure.result').read_bytes()
            assert data==(base/'result_1.bin').read_bytes() and hashlib.sha256(data).hexdigest()==case['result_sha256']
            assert (base/'report_failure.stderr').read_bytes()==b'report publication failed\n'
            reds+=1;continue
        report=case['report'];assert case['process_exit_code']==report['exit_code']
        if case.get('control') in ('oversized','fifo','symlink'):
            assert report['status']=='bounded shader capture refused' and report['exit_code']==2
            assert 'captured_shader_sha256' not in report and 'before_field_inventory' not in report
        else:assert report['captured_shader_sha256']==hashlib.sha256(source).hexdigest()
        assert report['native_observer_integrated'] is False and report['full_history_authenticated'] is False
        assert report['new_simulation_launched'] is False
        if 'n' in case:
            n=case['n'];data=(base/f'input_{n}.bin').read_bytes();bits=struct.unpack('<'+str(n)+'I',data)
            expected=c['encode'](c['reduce_bits'](bits));actual=(base/f'result_{n}.bin').read_bytes()
            assert actual==expected and report['accepted_record_bytes']==176
            assert report['captured_input_sha256']==hashlib.sha256(data).hexdigest()
            assert report['result_sha256']==hashlib.sha256(actual).hexdigest()
            assert report['kernel_failure_bits']==0 and report['exit_code']==0
            assert 0<report['peak_RSS_bytes']<=268435456 and report['field_buffers_allocated'] is True
            assert report==json.loads((base/f'run_{n}.stdout').read_bytes())
            assert (base/f'run_{n}.stderr').read_bytes()==b''
            greens+=1
        else:
            name=case['control'];assert not (base/f'{name}.result').exists()
            assert report==json.loads((base/f'{name}.stdout').read_bytes())
            if name=='preflight':assert report['exit_code']==5 and report['field_buffers_allocated'] is False
            elif name=='nonfinite':assert report['exit_code']==8 and report['kernel_failure_bits']==1
            elif name in ('oversized','fifo','symlink'):assert report['exit_code']==2
            else:raise AssertionError('unknown control')
            reds+=1
    assert greens==6 and reds==6
    return dict(status='retained_native_sum_control_bytes_verified',green=greens,red=reds,
                production_native_fields_read=0,native_observer_integrated=False,new_simulation_launched=False)

if __name__=='__main__':print(json.dumps(verify(Path(sys.argv[1]))))
