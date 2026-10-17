#!/usr/bin/env python3
"""Retained synthetic native compound outputs, independent integer oracle; no launches."""
import hashlib,json,struct,sys
from pathlib import Path
HELPER=Path('/Users/aravind/Working/RISE/worktrees/r243-structure-diagnostic/tools/fire/r248_resident_compound_contract.py')
HELPER_SHA='66abfa13a6590c81f673e9ed62ee5110407e8e847044dc207f0da7ef81a0b828'
def weight(bits):
    exponent=(bits>>23)&255;mantissa=bits&0x7fffff
    assert exponent!=255
    return ((mantissa|0x800000)<<(exponent-1)) if exponent else mantissa

def verify(base):
    raw=HELPER.read_bytes();assert hashlib.sha256(raw).hexdigest()==HELPER_SHA
    helper={'__name__':'captured_product'};exec(compile(raw,'captured_product','exec'),helper)
    controls=json.loads((base/'controls.json').read_bytes());assert controls['green']==36 and controls['red']==15
    shader=hashlib.sha256((base/'r248_compound.metal').read_bytes()).hexdigest();green=red=0
    for case in controls['cases']:
        if 'n' in case:
            n,power,kind=case['n'],case['power'],case['kind'];tag=f'{kind}_{n}_{power}';data=(base/f'input_{kind}_{n}.bin').read_bytes()
            pos=neg=pc=nc=zc=0
            for a,b,alpha,coordinate in struct.iter_unpack('<4I',data):
                wa=-weight(a) if a>>31 else weight(a);wb=-weight(b) if b>>31 else weight(b)
                value=((wa+wb)<<149) if kind==0 else (wa<<149)+wb*weight(alpha)
                value*=coordinate**power
                if value==0:zc+=1
                elif value<0:neg-=value;nc+=1
                else:pos+=value;pc+=1
            assert pc+nc+zc==n
            digits=lambda value:tuple((value>>(16*i))&65535 for i in range(33))
            expected=struct.pack('<72I',kind,power,*digits(pos),*digits(neg),n,pc,nc,zc)
            actual=(base/f'result_{tag}.bin').read_bytes();assert actual==expected
            helper['decode'](actual)
            r=case['report'];assert r==json.loads((base/f'run_{tag}.stdout').read_bytes())
            assert case['process_exit_code']==r['exit_code']==0 and r['kernel_failure_bits']==0
            assert r['coordinate_moment_order']==power and r['synthetic_primitive_control'] is False
            assert r['captured_input_sha256']==hashlib.sha256(data).hexdigest() and r['captured_shader_sha256']==shader
            assert r['result_sha256']==hashlib.sha256(actual).hexdigest() and r['accepted_record_bytes']==288
            assert r['field_buffers_allocated'] is True and 0<r['peak_RSS_bytes']<=268435456
            assert (base/f'run_{tag}.stderr').read_bytes()==b'';green+=1
        else:
            name=case['control']
            if name=='report_failure':
                assert case['process_exit_code']==9 and case['receipt_published'] is False and case['result_unsealed'] is True
                data=(base/'report_failure.result').read_bytes();assert data==(base/'result_0_1_0.bin').read_bytes()
                assert hashlib.sha256(data).hexdigest()==case['result_sha256']
                assert (base/'report_failure.stderr').read_bytes()==b'report publication failed\n'
            else:
                assert not (base/f'{name}.result').exists();r=case['report'];assert r==json.loads((base/f'{name}.stdout').read_bytes())
                assert r['exit_code']==case['process_exit_code']
                if name.startswith('nonfinite'):assert r['exit_code']==8 and r['kernel_failure_bits']==1
                elif name=='count_overflow':assert r['exit_code']==8 and r['kernel_failure_bits']==2 and r['synthetic_primitive_control'] is True
                elif name=='carry_overflow':assert r['exit_code']==8 and r['kernel_failure_bits']==4 and r['synthetic_primitive_control'] is True
                elif name in ('alpha_negative','alpha_high','gas_third'):assert r['exit_code']==8 and r['kernel_failure_bits']==16
                elif name=='preflight':assert r['exit_code']==5 and r['field_buffers_allocated'] is False
                elif name in ('oversized','fifo','symlink','order','kind'):assert r['exit_code']==2
                else:raise AssertionError('unknown control')
                assert (base/f'{name}.stderr').read_bytes()==b''
            red+=1
    from fractions import Fraction
    joint=0
    for kind in (0,1):
        for n in (1,17,255,256,257,65537):
            records=[helper['decode']((base/f'result_{kind}_{n}_{power}.bin').read_bytes()) for power in (0,1,2)]
            assert len({r['positive_count'] for r in records})==1
            positive=[helper['number'](r['positive']) for r in records]
            p,m1,m2=positive
            if p:
                mean=Fraction(m1,2*p);variance=Fraction(m2,4*p)-mean**2
                assert variance>=0
            else:assert m1==m2==0
            hashes={c['report']['captured_input_sha256'] for c in controls['cases'] if c.get('kind')==kind and c.get('n')==n}
            assert len(hashes)==1;joint+=1
    assert joint==12
    assert green==36 and red==15
    return dict(status='retained_standalone_native_compound_bytes_verified',green=green,red=red,production_native_fields_read=0,native_observer_integrated=False,new_simulation_launched=False)
if __name__=='__main__':print(json.dumps(verify(Path(sys.argv[1]))))
