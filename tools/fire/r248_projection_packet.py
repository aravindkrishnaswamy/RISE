#!/usr/bin/env python3
"""Fixed-size projection-summary wire oracle; no native issuance/authentication.

The 39 identity slots remain caller-supplied opaque uint64 values. Numeric wire
canonicality does not authenticate their stage lineage or original native values.
This is one packet family, not the full scalar/source/limiter observer layout.
"""
import hashlib
import json
import math
from pathlib import Path
import struct

ACCOUNTING_PATH=Path('/Users/aravind/Working/RISE/worktrees/r243-structure-diagnostic/tools/fire/r248_projection_accounting.py')
ACCOUNTING_SHA='7e3d979e6c51a63c95ddf4caf3a9a0c76ea444b18d47fb62b8e52f7902b50bf2'
MAGIC=b'R248PRJ1'
ROW_BYTES=264
PACKET_BYTES=3496
UINT64_MAX=2**64-1


def require(condition,message):
    if not condition:raise ValueError(message)


def accounting_context():
    raw=ACCOUNTING_PATH.read_bytes()
    require(hashlib.sha256(raw).hexdigest()==ACCOUNTING_SHA,'accepted accounting helper hash')
    context={'__name__':'captured_projection_accounting'}
    exec(compile(raw,str(ACCOUNTING_PATH),'exec'),context)
    return context


def uint64(value):
    require(type(value) is int and 0<=value<=UINT64_MAX,'typed uint64 slot')
    return value


def float64(value):
    require(type(value) is float and math.isfinite(value),'typed finite binary64 summary')
    return value


def header_layout(metadata,layers):
    require(type(metadata) is dict,'exact metadata dictionary')
    for key in ('nx','ny','nz'):require(key in metadata,'domain metadata present')
    context=accounting_context()
    addresses=context['layout_context']()['layout']((metadata['nx'],metadata['ny'],metadata['nz']),layers)
    slots=addresses['metadata_uint64_slots']
    require(set(metadata)==set(slots),'exact metadata slot set')
    for value in metadata.values():uint64(value)
    # Bounds/representation checks only. Publication values are opaque, including
    # zero; their availability and native parent/consumed/issued lineage are pending.
    for key in ('dt_binary32_bits','dx_binary32_bits'):
        require(metadata[key]<2**32,'binary32 bits zero-extended')
        value=struct.unpack('<f',struct.pack('<I',metadata[key]))[0]
        require(math.isfinite(value) and value>0,'positive finite binary32 measure')
    beginning=struct.unpack('<d',struct.pack('<Q',metadata['beginning_time_binary64_bits']))[0]
    end=struct.unpack('<d',struct.pack('<Q',metadata['end_time_binary64_bits']))[0]
    dt=struct.unpack('<f',struct.pack('<I',metadata['dt_binary32_bits']))[0]
    require(metadata['accepted_ordinal']>0 and math.isfinite(beginning) and beginning>=0 and
            math.isfinite(end) and end>beginning and end==beginning+dt,'local exact accepted clock pair')
    return context,addresses,slots


def packet_layout():
    # Each row: unique faces, three branch counts, copy count, then seven
    # summaries, each with uint64 count/availability and binary64 signed/absolute.
    return dict(schema='rise.fire.r248.projection-packet-layout.v1',magic=MAGIC.decode(),
                byte_order='little',header_bytes=328,row_bytes=ROW_BYTES,rows=12,packet_bytes=PACKET_BYTES,
                summaries=('pressure_endpoint_rate','burner_endpoint_rate','wall_endpoint_rate',
                           'periodic_copy_endpoint_rate','total_endpoint_rate','observed_closure_residual',
                           'pressure_gradient_on_executed_branch'),
                native_values_authenticated=False,stage_lineage_authenticated=False,
                full_observer_numeric_layout_complete=False,native_allocation_accepted=False)


def encode(result,metadata,layers=80):
    context,addresses,slots=header_layout(metadata,layers)
    expected_keys={'schema','supports','support_layout_sha256','dt_s','caller_values_authenticated',
                   'boundary_configuration_authenticated','pressure_gradient_arithmetic_replayed',
                   'EOS_restoration_impulse_available','native_reduction_packet_accepted'}
    require(type(result) is dict and set(result)==expected_keys,'exact accounting result schema')
    require(result['schema']=='rise.fire.r248.projection-accounting-oracle.v1' and
            result['support_layout_sha256']==context['LAYOUT_SHA'],'accepted result/helper schema')
    for key in expected_keys-{'schema','supports','support_layout_sha256','dt_s'}:
        require(result[key] is False,'no authentication/acceptance promotion')
    dt=struct.unpack('<f',struct.pack('<I',metadata['dt_binary32_bits']))[0]
    require(type(result['dt_s']) is float and result['dt_s']==dt,'summary/header exact dt')
    require(type(result['supports']) is dict and set(result['supports'])=={'native','common'},'exact support set')
    output=bytearray(MAGIC+struct.pack('<Q',layers))
    for name in slots:output.extend(struct.pack('<Q',metadata[name]))
    for support_name in ('native','common'):
        rows=result['supports'][support_name];support=addresses['supports'][support_name]
        require(type(rows) is list and len(rows)==6,'exact ordered axis/support cardinality')
        for ordinal,row in enumerate(rows):
            axis=ordinal//2;kind='whole_region' if ordinal%2==0 else 'central'
            expected_count=(support['face_ranges'][axis]['count'] if kind=='whole_region' else
                            len(support['central_face_indices'][axis]))
            keys={'axis','support','unique_faces','branch_counts','periodic_copy_faces','endpoint_rates',
                  'pressure_gradient_on_executed_branch','gradient_is_not_endpoint_rounding_residual','unit'}
            require(type(row) is dict and set(row)==keys and type(row['axis']) is int and
                    row['axis']==axis and row['support']==kind and row['unit']=='kg/m2/s2' and
                    row['gradient_is_not_endpoint_rounding_residual'] is True,'exact row metadata/order/unit')
            require(uint64(row['unique_faces'])==expected_count,'exact geometry count')
            counts=row['branch_counts']
            require(type(counts) is dict and set(counts)==set(context['BRANCHES']),'exact branch count keys')
            count_values=[uint64(counts[key]) for key in context['BRANCHES']]
            require(sum(count_values)==expected_count,'branch counts partition support')
            copies=uint64(row['periodic_copy_faces']);require(copies<=expected_count,'copy count within support')
            output.extend(struct.pack('<5Q',expected_count,*count_values,copies))
            rates=row['endpoint_rates']
            require(type(rates) is dict and set(rates)==set(context['QUANTITIES']),'exact endpoint quantity set')
            summaries=[(rates[key],expected_count) for key in context['QUANTITIES']]
            summaries.append((row['pressure_gradient_on_executed_branch'],count_values[0]))
            for summary,count in summaries:
                if count==0:
                    require(summary is None,'empty gradient explicitly unavailable')
                    output.extend(struct.pack('<QQdd',0,0,0.,0.))
                else:
                    require(type(summary) is dict and set(summary)=={'count','signed_mean','absolute_mean'} and
                            uint64(summary['count'])==count,'summary count exactly binds support')
                    signed=float64(summary['signed_mean']);absolute=float64(summary['absolute_mean'])
                    require(absolute>=abs(signed) and math.copysign(1.,absolute)==1.,'canonical absolute mean bounds signed mean')
                    output.extend(struct.pack('<QQdd',count,1,signed,absolute))
    require(len(output)==PACKET_BYTES,'fixed projection wire byte bound')
    return bytes(output)


def decode(payload):
    require(type(payload) is bytes and len(payload)==PACKET_BYTES and payload[:8]==MAGIC,'exact captured wire bytes')
    layers=struct.unpack_from('<Q',payload,8)[0]
    context=accounting_context();names=list(context['layout_context']()['layout']((2,2,1),1)['metadata_uint64_slots'])
    metadata=dict(zip(names,struct.unpack_from('<39Q',payload,16)))
    context,addresses,slots=header_layout(metadata,layers)
    supports={};cursor=328
    for support_name in ('native','common'):
        rows=[]
        for ordinal in range(6):
            faces,p,b,w,copies=struct.unpack_from('<5Q',payload,cursor);cursor+=40
            summaries=[]
            for _ in range(7):
                count,available,signed,absolute=struct.unpack_from('<QQdd',payload,cursor);cursor+=32
                require(available in (0,1),'typed availability flag')
                if not available:
                    require(count==0 and struct.pack('<dd',signed,absolute)==bytes(16),'canonical absent summary')
                    summaries.append(None)
                else:
                    require(count>0,'present summary nonempty')
                    summaries.append(dict(count=count,signed_mean=signed,absolute_mean=absolute))
            rows.append(dict(axis=ordinal//2,support='whole_region' if ordinal%2==0 else 'central',unique_faces=faces,
                             branch_counts=dict(zip(context['BRANCHES'],(p,b,w))),periodic_copy_faces=copies,
                             endpoint_rates=dict(zip(context['QUANTITIES'],summaries[:6])),
                             pressure_gradient_on_executed_branch=summaries[6],
                             gradient_is_not_endpoint_rounding_residual=True,unit='kg/m2/s2'))
        supports[support_name]=rows
    result=dict(schema='rise.fire.r248.projection-accounting-oracle.v1',supports=supports,
                support_layout_sha256=context['LAYOUT_SHA'],dt_s=struct.unpack('<f',struct.pack('<I',metadata['dt_binary32_bits']))[0],
                caller_values_authenticated=False,boundary_configuration_authenticated=False,
                pressure_gradient_arithmetic_replayed=False,EOS_restoration_impulse_available=False,
                native_reduction_packet_accepted=False)
    require(encode(result,metadata,layers)==payload,'canonical wire round trip')
    return result,metadata,layers


def controls():
    import copy
    context=accounting_context();np=context['np'];dims=(6,8,3)
    addresses=context['layout_context']()['layout'](dims,2);faces=addresses['native_faces']
    before=np.arange(faces,dtype=np.float32);after=before+np.float32(1)
    codes=np.full(faces,2,dtype=np.uint8);after[:]=0
    absent=np.full(faces,np.nan,dtype=np.float32)
    result=context['projection_accounting'](dims,.5,before,after,after.copy(),codes,absent,absent,
                                         np.full(faces,-1,dtype=np.int64),2)
    metadata={name:i for i,name in enumerate(addresses['metadata_uint64_slots'],1)}
    metadata.update(nx=6,ny=8,nz=3,dt_binary32_bits=0x3f000000,dx_binary32_bits=0x3d800000,
                    beginning_time_binary64_bits=0,end_time_binary64_bits=0x3fe0000000000000)
    payload=encode(result,metadata,2)
    require(len(payload)==3496 and decode(payload)==(result,metadata,2),'exact wire round trip')
    # Independent numeric offsets: first row 40-byte counts, six 32-byte
    # present rate triples and one explicit absent-gradient tuple.
    require(struct.unpack_from('<5Q',payload,328)==(168,0,0,168,0),'independent first-row counts')
    require(struct.unpack_from('<QQdd',payload,368)==(168,1,0.,0.) and
            payload[328+40+6*32:328+264]==bytes(32),'independent summary/absence offsets')
    # Nonempty pressure and prescribed-value rows preserve all numeric triples.
    for code in (0,1):
        endpoint=before+np.float32(1);branch=np.full(faces,code,dtype=np.uint8)
        gradient=np.full(faces,-2,dtype=np.float32) if code==0 else absent
        imposed=endpoint if code==1 else absent
        other=context['projection_accounting'](dims,.5,before,endpoint,endpoint.copy(),branch,gradient,imposed,
                                             np.full(faces,-1,dtype=np.int64),2)
        wire=encode(other,metadata,2)
        require(decode(wire)==(other,metadata,2),'present gradient/burner exact numeric round trip')
        # First executed pressure gradient: count168, available1, -2 signed/+2 absolute.
        if code==0:require(struct.unpack_from('<QQdd',wire,328+40+6*32)==(168,1,-2.,2.),
                           'independent present gradient wire offset')
    red=0
    def refuse(fn):
        nonlocal red
        try:fn()
        except ValueError:red+=1
        else:raise AssertionError('missing packet refusal')
    for index,fmt,value in [(0,'8s',b'INVALID!'),(8,'Q',0),(16+3*8,'Q',2**32),
                            (328,'Q',111),(336,'Q',1),(360,'Q',169),(368+8,'Q',2),
                            (368,'Q',111),(368+16,'d',float('nan')),(368+24,'d',-1.),(368+24,'d',-0.),
                            (328+40+6*32+8,'Q',1),(328+40+6*32+16,'d',-0.)]:
        bad=bytearray(payload);struct.pack_into('<'+fmt,bad,index,value)
        refuse(lambda bad=bad:decode(bytes(bad)))
    refuse(lambda:decode(payload[:-1]));refuse(lambda:decode(payload+b'\0'));refuse(lambda:decode(bytearray(payload)))
    for key,value in [('nx',True),('R0.issued_target_publication',False),('dt_binary32_bits',0),('dx_binary32_bits',0x7f800000),
                      ('accepted_ordinal',0),('beginning_time_binary64_bits',0x7ff8000000000000),
                      ('end_time_binary64_bits',0)]:
        bad=dict(metadata);bad[key]=value;refuse(lambda bad=bad:encode(result,bad,2))
    bad=dict(metadata);bad['extra']=0;refuse(lambda:encode(result,bad,2))
    bad=copy.deepcopy(result);bad['caller_values_authenticated']=True;refuse(lambda:encode(bad,metadata,2))
    bad=copy.deepcopy(result);bad['supports']['common'].reverse();refuse(lambda:encode(bad,metadata,2))
    bad=copy.deepcopy(result);bad['supports']['native'][0]['pressure_gradient_on_executed_branch']={'count':0,'signed_mean':0.,'absolute_mean':0.}
    refuse(lambda:encode(bad,metadata,2))
    require(red==27,'wire/header/cardinality/absence RED count')
    print(json.dumps(dict(status='pure_projection_wire_prerequisite_only',red=red,packet_bytes=len(payload),
                         native_fields_read=0,stage_lineage_authenticated=False,
                         full_observer_numeric_layout_complete=False,new_simulation_launched=False)))


if __name__=='__main__':controls()
