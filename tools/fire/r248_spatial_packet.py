#!/usr/bin/env python3
"""Captured caller-witness spatial codec. No standalone summary/native admission.

Every admitted byte must equal the exact spatial oracle for immutable captured
caller fields. This proves joint field-witness consistency, not native provenance
or a device reduction replay. Capture/oracle scratch scales with the domain.
"""
import hashlib
import json
from pathlib import Path
import struct
import numpy as np

HELPER=Path('/Users/aravind/Working/RISE/worktrees/r243-structure-diagnostic/tools/fire/r248_spatial_support.py')
HELPER_SHA='cf8c8a263ba2e089b73a80c2df27001f48002915bb0017f98346761c587c5a00'
MAGIC=b'R248SPW1'
PACKET_BYTES=70368
HEADER_BYTES=336
SUMMARY_BYTES=144
LAYER_BYTES=72
MAX_LAYERS=160


def require(ok,message):
    if not ok:raise ValueError(message)


def context():
    raw=HELPER.read_bytes();require(hashlib.sha256(raw).hexdigest()==HELPER_SHA,'accepted exact spatial helper hash')
    c={'__name__':'captured_spatial_oracle'};exec(compile(raw,str(HELPER),'exec'),c);return c


def capture(c,fields,metadata,layers,stage):
    require(type(fields) is tuple and len(fields)==4,'exact gas/soot/CH4/O2 field tuple')
    # Header integers are immutable. Own a copy before geometry or field reads.
    require(type(metadata) is dict,'exact metadata dictionary');meta=dict(metadata)
    addresses,slots,volume=c['header'](meta,layers,stage)
    nx,ny,nz=addresses['dimensions'];shape=(nz,ny,nx);owned=[];pins=[]
    for a in fields:
        require(type(a) is np.ndarray and a.dtype==np.float32 and a.shape==shape,'typed caller field layout')
        raw=a.tobytes(order='C')
        frozen=np.frombuffer(raw,dtype=np.float32).reshape(shape)
        require(not frozen.flags.writeable,'immutable captured field')
        owned.append(frozen);pins.append(hashlib.sha256(raw).hexdigest())
    # Validation/reduction both consume the same four immutable byte buffers.
    result=c['observe'](*owned,meta,layers,stage)
    return result,meta,slots,pins


def pack_summary(row):
    coords=row['centroid_cell_centre_xyz'];variance=row['variance_cell_squared_xyz'];fraction=row['central_positive_fraction']
    geometry=row['positive_geometry_available']
    tail=tuple(coords)+tuple(variance)+(fraction,) if geometry else (0.,)*7
    return struct.pack('<5Q5dQ7d',row['cell_count'],int(row['available']),row['positive_count'],
        row['negative_count'],row['central_positive_count'],*row['totals'].values(),int(geometry),*tail)


def canonical(c,result,meta,slots,layers,stage):
    # Helpers have fixed ordered total columns. This internal routine is not an
    # API admitting arbitrary summary dictionaries: callers must supply fields.
    raw=bytearray(MAGIC+struct.pack('<41Q',*(meta[k] for k in slots),layers,stage))
    require(len(raw)==HEADER_BYTES,'exact spatial header reservation')
    for key in c['QUANTITIES']:
        f=result['common_positive_fraction'][key]
        raw.extend(struct.pack('<Qd',int(f is not None),f if f is not None else 0.))
    for support in ('native','common'):
        for key in c['QUANTITIES']:
            row=result['supports'][support][key];raw.extend(pack_summary(row))
            for z in range(MAX_LAYERS):
                if z<len(row['layers']):
                    r=row['layers'][z]
                    raw.extend(struct.pack('<4Q5d',int(r['available']),r['positive_count'],r['negative_count'],
                        r['central_positive_count'],*r['totals'].values()))
                else:raw.extend(bytes(LAYER_BYTES))
    require(len(raw)==PACKET_BYTES,'fixed spatial wire bytes')
    return bytes(raw)


def encode(fields,metadata,layers=80,stage=0):
    c=context();result,meta,slots,pins=capture(c,fields,metadata,layers,stage)
    return canonical(c,result,meta,slots,layers,stage)


def admit(payload,fields,metadata,layers=80,stage=0):
    # Require immutable bytes: capture of caller input is explicit, never a live
    # mutable bytearray/view used twice during independent reads.
    require(type(payload) is bytes and len(payload)==PACKET_BYTES,'exact immutable spatial wire size')
    require(payload[:8]==MAGIC,'spatial witness codec magic')
    c=context();result,meta,slots,pins=capture(c,fields,metadata,layers,stage)
    expected=canonical(c,result,meta,slots,layers,stage)
    require(payload==expected,'exact captured field-witness spatial wire')
    return dict(schema='rise.fire.r248.spatial-witness-admission.v1',result=result,
                payload_sha256=hashlib.sha256(payload).hexdigest(),captured_field_sha256=pins,
                exact_caller_witness_consistent=True,standalone_summary_admission=False,
                native_values_authenticated=False,native_stage_authenticated=False,
                full_history_authenticated=False,native_reduction_replayed=False,
                native_allocation_accepted=False,physical_acceptance=False)


def packet_layout():
    return dict(schema='rise.fire.r248.spatial-witness-packet-layout.v1',magic=MAGIC.decode(),byte_order='little',
                packet_bytes=PACKET_BYTES,header_bytes=HEADER_BYTES,common_fraction_offset=336,
                first_summary_offset=384,summary_bytes=SUMMARY_BYTES,layer_bytes=LAYER_BYTES,
                rows=6,max_layers=MAX_LAYERS,header='magic8 +39uint64 metadata +common_layers uint64 +stage uint64',
                admission='exact recomputed oracle from same four immutable captured caller fields; no standalone summaries',
                absent_padding='canonical positive zero; absent integer counts zero',
                native_allocation_accepted=False,measured_memory_bytes=None,
                host_oracle_scratch='captured field copies plus exact Fraction lists scale with domain',
                native_stage_authenticated=False,new_simulation_launched=False)


def controls():
    c=context();p=c['capture']('r248_target_terms.py',c['TARGET_SHA'])['packet_context']()
    dims=(4,4,3);l=p['accounting_context']()['layout_context']()['layout'](dims,2)
    meta={k:i for i,k in enumerate(l['metadata_uint64_slots'],1)}
    meta.update(nx=4,ny=4,nz=3,dx_binary32_bits=0x3f000000,dt_binary32_bits=0x3e800000,
                beginning_time_binary64_bits=0,end_time_binary64_bits=0x3fd0000000000000)
    g=np.zeros((3,4,4),np.float32);s=g.copy();f=g.copy();o=np.ones_like(g)
    g[0,0,0]=1;s[0,1,2]=3;g[0,3,3]=-1;g[2,0,0]=4;f[1,1,1]=2
    fields=(g,s,f,o);raw=encode(fields,meta,2);r=admit(raw,fields,meta,2)
    require(r['result']['supports']['common'][c['QUANTITIES'][0]]['totals']['signed']==.375,'known spatial unit witness')
    require(len(raw)==70368 and struct.unpack_from('<Q',raw,328)[0]==0,'stage/header offsets')
    red=0
    def refuse(fn):
        nonlocal red
        try:fn()
        except ValueError:red+=1
        else:raise AssertionError('missing spatial packet refusal')
    for offset in (0,8,320,328,336,344,384,392,400,408,416,424,432,440,448,456,464,472,480,488,496,504,512,520,
                   528,536,544,552,560,568,576,588,704,12048):
        b=bytearray(raw);b[offset]^=1;refuse(lambda b=b:admit(bytes(b),fields,meta,2))
    refuse(lambda:admit(raw[:-1],fields,meta,2));refuse(lambda:admit(raw+b'\0',fields,meta,2))
    refuse(lambda:admit(bytearray(raw),fields,meta,2));refuse(lambda:admit(memoryview(raw),fields,meta,2))
    g2=g.copy();g2[0,0,0]=2;refuse(lambda:admit(raw,(g2,s,f,o),meta,2))
    refuse(lambda:admit(raw,fields,meta,1));refuse(lambda:admit(raw,fields,meta,2,1))
    bad=dict(meta,accepted_ordinal=meta['accepted_ordinal']+1);refuse(lambda:admit(raw,fields,bad,2))
    require(red==42,'spatial isolated wire/witness mutation count')
    # Empty positive support, equal-height and maximum/subnormal exact witnesses.
    for source in (np.zeros_like(g),-np.ones_like(g),np.full_like(g,np.finfo(np.float32).max)):
        x=(source,np.zeros_like(g),np.zeros_like(g),np.zeros_like(g));a=encode(x,meta,3);admit(a,x,meta,3)
    print(json.dumps(dict(status='pure_captured_caller_witness_spatial_codec_only',red=red,packet_bytes=PACKET_BYTES,
                         native_fields_read=0,standalone_summary_admission=False,native_stage_authenticated=False,
                         native_allocation_accepted=False,new_simulation_launched=False)))


if __name__=='__main__':controls()
