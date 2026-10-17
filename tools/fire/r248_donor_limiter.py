#!/usr/bin/env python3
"""Pure CH4/O2 donor/limiter surface witness and fixed numeric wire.

Donor densities, stage fluxes and shared limiter are caller-provided, not native
publications. No inference from terminal cells, no Heun stage averaging here.
"""
from fractions import Fraction
import hashlib
import json
import math
from pathlib import Path
import struct
import numpy as np

TARGET_PATH=Path('/Users/aravind/Working/RISE/worktrees/r243-structure-diagnostic/tools/fire/r248_target_terms.py')
TARGET_SHA='35cf1d215582bb34ba4defb2ecf28953d874589075b7faaad07b188258123e4c'
SCALAR_PATH=Path('/Users/aravind/Working/RISE/worktrees/r243-structure-diagnostic/tools/fire/r247_region_transfers.py')
SCALAR_SHA='4e814f7cb55739cd39d645e2136072c0e2e2bc6b56599e2f19536de3072ed754'
SIDES=('xmin','xmax','ymin','ymax','bottom','upper')
SPECIES=('CH4','O2')
ALPHA_BINS=('zero','(0,1/4)','[1/4,1/2)','[1/2,3/4)','[3/4,1)','one')
DONOR_BINS=('exact_zero','positive')
VALUES=('low_net_outward','anti_net_outward','limited_anti_net_outward','limited_net_outward',
        'incoming','outgoing','anti_absolute','limited_anti_absolute','component_residual')
HEADER_BYTES=336
ROW_BYTES=88
ROWS=288
PACKET_BYTES=25680
MAGIC=b'R248DNR1'


def require(condition,message):
    if not condition:raise ValueError(message)


def captured(path,digest):
    raw=path.read_bytes();require(hashlib.sha256(raw).hexdigest()==digest,'accepted helper digest')
    context={'__name__':'captured_donor_dependency'};exec(compile(raw,str(path),'exec'),context)
    return context


def header(metadata,layers,stage):
    require(type(stage) is int and stage in (0,1),'typed R0/R1 donor witness stage')
    target=captured(TARGET_PATH,TARGET_SHA);packet=target['packet_context']()
    _,addresses,slots=packet['header_layout'](metadata,layers)
    return target,addresses,slots


def alpha_bin(value):
    if value==0:return 0
    if value==1:return 5
    return 1 if value<.25 else 2 if value<.5 else 3 if value<.75 else 4


def surfaces(dimensions,offsets,height):
    nx,ny,nz=dimensions;fx,fy,fz=offsets
    return ([fx+(nx+1)*(y+ny*z) for z in range(height) for y in range(ny)],
            [fx+nx+(nx+1)*(y+ny*z) for z in range(height) for y in range(ny)],
            [fy+x+nx*(ny+1)*z for z in range(height) for x in range(nx)],
            [fy+x+nx*(ny+(ny+1)*z) for z in range(height) for x in range(nx)],
            [fz+x+nx*y for y in range(ny) for x in range(nx)],
            [fz+x+nx*(y+ny*height) for y in range(ny) for x in range(nx)])


def witness(donor,low,anti,alpha,metadata,layers=80,stage=1):
    target,addresses,slots=header(metadata,layers,stage);faces=addresses['native_faces']
    require(all(type(a) is np.ndarray and a.dtype==np.float32 and a.shape==(2,faces) and
                np.isfinite(a).all() for a in (donor,low,anti)),'finite CH4/O2 native face operands')
    require(np.all(donor>=0),'nonnegative represented donor species density')
    require(type(alpha) is np.ndarray and alpha.dtype==np.float32 and alpha.shape==(faces,) and
            np.isfinite(alpha).all() and np.all(alpha>=0) and np.all(alpha<=1),'shared native limiter range')
    dx=struct.unpack('<f',struct.pack('<I',metadata['dx_binary32_bits']))[0]
    dt=struct.unpack('<f',struct.pack('<I',metadata['dt_binary32_bits']))[0]
    measure=dx**2*dt;require(math.isfinite(measure) and measure>0,'surface transfer measure')
    output={}
    for name,support in addresses['supports'].items():
        rows=[];planes=surfaces(addresses['dimensions'],addresses['native_face_offsets'],support['layers'])
        for side,(indices,direction) in enumerate(zip(planes,(-1,1,-1,1,-1,1))):
            for species in range(2):
                buckets=[[] for _ in range(12)]
                for index in indices:
                    a=float(alpha[index]);lo=float(low[species,index]);ad=float(anti[species,index])
                    signed=direction*measure
                    low_part=lo*signed;anti_part=ad*signed;limited_anti=(a*ad)*signed
                    limited=(lo+a*ad)*signed
                    values=(low_part,anti_part,limited_anti,limited,max(-limited,0.),max(limited,0.),
                            abs(anti_part),abs(limited_anti))
                    require(all(math.isfinite(v) for v in values),'finite donor transfer')
                    b=2*alpha_bin(a)+(0 if donor[species,index]==0 else 1)
                    buckets[b].append(values)
                for bin_index,bucket in enumerate(buckets):
                    values=[math.fsum(v[j] for v in bucket) for j in range(8)] if bucket else None
                    if values is not None:values.append(math.fsum((values[3],-values[0],-values[2])))
                    rows.append(dict(side=SIDES[side],species=SPECIES[species],alpha_bin=ALPHA_BINS[bin_index//2],
                                     donor_bin=DONOR_BINS[bin_index%2],count=len(bucket),available=bool(bucket),
                                     values=dict(zip(VALUES,values)) if values is not None else None,unit='kg'))
        output[name]=rows
    return dict(schema='rise.fire.r248.donor-limiter-oracle.v1',stage=stage,supports=output,
                caller_values_authenticated=False,native_donor_state_authenticated=False,
                Heun_stage_composition_performed=False,physical_depletion_causality_accepted=False)


def fractional_bin_consistent(unlimited,limited,index):
    """Necessary joint ratio witness, including leaf and fsum RN rounding.

    alpha*anti is exact in binary64 (two binary32 significands). Each
    nonzero transfer is normal: even minimum alpha*anti*dx**2*dt is 2**-745.
    There is one rounded multiply by the shared measure and one rounded
    positive fsum. IEEE binary64 RN relative error is bounded by 2**-53
    at each operation; these exact rational bounds are representation bounds,
    not a physical tolerance. Exclusive endpoints use adjacent binary32 values.
    """
    boundaries=((0x00000001,0x3e7fffff),(0x3e800000,0x3effffff),
                (0x3f000000,0x3f3fffff),(0x3f400000,0x3f7fffff))
    lower_bits,upper_bits=boundaries[index-1]
    lower=Fraction.from_float(struct.unpack('<f',struct.pack('<I',lower_bits))[0])
    upper=Fraction.from_float(struct.unpack('<f',struct.pack('<I',upper_bits))[0])
    unit_roundoff=Fraction(1,2**53)
    high_factor=(1+unit_roundoff)**2;low_factor=(1-unit_roundoff)**2
    a=Fraction.from_float(unlimited);b=Fraction.from_float(limited)
    alo,ahi=a/high_factor,a/low_factor
    blo,bhi=b/high_factor,b/low_factor
    # One nonnegative unlimited total must support this same limited total.
    return max(alo,blo/upper)<=min(ahi,bhi/lower)


def encode(result,metadata,layers=80,stage=1):
    target,addresses,slots=header(metadata,layers,stage)
    keys={'schema','stage','supports','caller_values_authenticated','native_donor_state_authenticated',
          'Heun_stage_composition_performed','physical_depletion_causality_accepted'}
    require(type(result) is dict and set(result)==keys and result['schema']=='rise.fire.r248.donor-limiter-oracle.v1' and
            type(result['stage']) is int and result['stage']==stage,'exact donor result schema/stage')
    for key in keys-{'schema','stage','supports'}:require(result[key] is False,'no donor provenance/physics promotion')
    require(type(result['supports']) is dict and set(result['supports'])=={'native','common'},'exact donor support set')
    out=bytearray(MAGIC+struct.pack('<QQ',layers,stage))
    for key in slots:out.extend(struct.pack('<Q',metadata[key]))
    for name in ('native','common'):
        support=addresses['supports'][name];nx,ny,nz=addresses['dimensions'];height=support['layers']
        dx=struct.unpack('<f',struct.pack('<I',metadata['dx_binary32_bits']))[0]
        dt=struct.unpack('<f',struct.pack('<I',metadata['dt_binary32_bits']))[0]
        measure=dx**2*dt
        counts=(ny*height,ny*height,nx*height,nx*height,nx*ny,nx*ny)
        rows=result['supports'][name]
        require(type(rows) is list and len(rows)==144,'exact donor row cardinality')
        for side in range(6):
            for species in range(2):
                total=0
                for bin_index in range(12):
                    row=rows[(side*2+species)*12+bin_index]
                    require(type(row) is dict and set(row)=={'side','species','alpha_bin','donor_bin','count','available','values','unit'} and
                            (row['side'],row['species'],row['alpha_bin'],row['donor_bin'],row['unit'])==
                            (SIDES[side],SPECIES[species],ALPHA_BINS[bin_index//2],DONOR_BINS[bin_index%2],'kg'),
                            'exact donor bin row order/unit')
                    count=row['count'];require(type(count) is int and 0<=count<=counts[side],'bounded bin count')
                    total+=count
                    require(row['available'] is (count>0),'count/availability exact')
                    if count==0:
                        require(row['values'] is None,'known empty bin numeric values unavailable')
                        values=[0.]*9
                    else:
                        v=row['values'];require(type(v) is dict and set(v)==set(VALUES),'exact required donor quantities')
                        values=[v[key] for key in VALUES]
                        require(all(type(x) is float and math.isfinite(x) for x in values),'finite binary64 bin values')
                        # Each face uses at most low+alpha*anti (two binary32
                        # magnitudes); the reported residual uses three totals.
                        # Eight magnitudes conservatively cover those operations
                        # and binary64 rounding, not an acceptance tolerance.
                        bound=8*float(np.finfo(np.float32).max)*measure*count
                        require(all(abs(v)<=bound for v in values),'binary32-operand transfer magnitude bound')
                        lo,ad,limited_ad,limited,inc,outgoing,abs_ad,abs_limited,residual=values
                        if bin_index//2==0:
                            require(limited_ad==0 and abs_limited==0 and limited==lo,'exact zero-alpha transfer identity')
                        if bin_index//2==5:
                            require(limited_ad==ad and abs_limited==abs_ad,'exact unit-alpha anti identity')
                        require(all(x>=0 and (x!=0 or math.copysign(1.,x)==1.) for x in (inc,outgoing,abs_ad,abs_limited)) and
                                abs_limited<=abs_ad and abs_ad>=abs(ad) and abs_limited>=abs(limited_ad),
                                'signed/absolute limiter bounds')
                        if 0<bin_index//2<5:
                            require(fractional_bin_consistent(abs_ad,abs_limited,bin_index//2),
                                    'fractional alpha bin joint representational ratio witness')
                        net_interval=target['sum_interval_from_mean'](limited,1)
                        incoming_interval=target['sum_interval_from_mean'](inc,1)
                        outgoing_interval=target['sum_interval_from_mean'](outgoing,1)
                        require(max(net_interval[0],outgoing_interval[0]-incoming_interval[1])<=
                                min(net_interval[1],outgoing_interval[1]-incoming_interval[0]),'same signed in/out reduction rounding bins')
                        require(struct.pack('<d',residual)==struct.pack('<d',math.fsum((limited,-lo,-limited_ad))),
                                'exact reported component residual, not forced zero')
                    out.extend(struct.pack('<QQ9d',count,int(count>0),*values))
                require(total==counts[side],'bins partition every selected surface face')
    for name in ('native','common'):
        for side in range(6):
            for b in range(6):
                rows=result['supports'][name]
                require(sum(rows[(side*2)*12+2*b+d]['count'] for d in (0,1))==
                        sum(rows[(side*2+1)*12+2*b+d]['count'] for d in (0,1)),
                        'shared alpha species histogram cardinality')
    for side in range(6):
        for species in range(2):
            for b in range(12):
                i=(side*2+species)*12+b
                native=result['supports']['native'][i];common=result['supports']['common'][i]
                if side==4 or layers==nz:
                    require(native['count']==common['count'] and native['available']==common['available'],
                            'same-face support exact donor counts')
                    if native['available']:
                        require(all(struct.pack('<d',native['values'][k])==struct.pack('<d',common['values'][k])
                                    for k in VALUES),'same-face support exact donor values')
                elif side<4:
                    require(common['count']<=native['count'],'lateral common bin count subset')
                    if common['available']:
                        require(all(common['values'][k]<=native['values'][k]
                                    for k in ('incoming','outgoing','anti_absolute','limited_anti_absolute')),
                                'lateral common nonnegative reduction subset')
    require(len(out)==PACKET_BYTES,'fixed donor witness wire bytes')
    return bytes(out)


def decode(payload):
    require(type(payload) is bytes and len(payload)==PACKET_BYTES and payload[:8]==MAGIC,'exact immutable donor wire bytes')
    layers,stage=struct.unpack_from('<QQ',payload,8)
    target=captured(TARGET_PATH,TARGET_SHA);packet=target['packet_context']();context=packet['accounting_context']()['layout_context']()
    names=list(context['layout']((2,2,1),1)['metadata_uint64_slots'])
    metadata=dict(zip(names,struct.unpack_from('<39Q',payload,24)))
    rows_by_support={};cursor=HEADER_BYTES
    for name in ('native','common'):
        rows=[]
        for side in range(6):
            for species in range(2):
                for bin_index in range(12):
                    count,available,*values=struct.unpack_from('<QQ9d',payload,cursor);cursor+=ROW_BYTES
                    require(available in (0,1) and available==int(count>0),'exact wire availability')
                    if not available:require(struct.pack('<9d',*values)==bytes(72),'canonical empty-bin padding')
                    rows.append(dict(side=SIDES[side],species=SPECIES[species],alpha_bin=ALPHA_BINS[bin_index//2],
                                     donor_bin=DONOR_BINS[bin_index%2],count=count,available=bool(available),
                                     values=dict(zip(VALUES,values)) if available else None,unit='kg'))
        rows_by_support[name]=rows
    result=dict(schema='rise.fire.r248.donor-limiter-oracle.v1',stage=stage,supports=rows_by_support,
                caller_values_authenticated=False,native_donor_state_authenticated=False,
                Heun_stage_composition_performed=False,physical_depletion_causality_accepted=False)
    require(encode(result,metadata,layers,stage)==payload,'canonical donor wire round trip')
    return result,metadata,layers,stage


def packet_layout():
    return dict(schema='rise.fire.r248.donor-limiter-layout.v1',magic=MAGIC.decode(),byte_order='little',
                header_bytes=HEADER_BYTES,row_bytes=ROW_BYTES,rows=ROWS,packet_bytes=PACKET_BYTES,
                support_order=('native','common'),side_order=SIDES,species_order=SPECIES,
                alpha_bins=ALPHA_BINS,donor_bins=DONOR_BINS,value_order=VALUES,unit='kg',
                native_donor_state_authenticated=False,Heun_stage_composition_performed=False,
                scalar_source_spatial_momentum_packet_families_complete=False,
                native_allocation_accepted=False,new_simulation_launched=False)


def controls():
    import copy
    dims=(6,8,3);nx,ny,nz=dims;target=captured(TARGET_PATH,TARGET_SHA);packet=target['packet_context']()
    layout=packet['accounting_context']()['layout_context']()['layout'](dims,2);faces=layout['native_faces']
    meta={key:i for i,key in enumerate(layout['metadata_uint64_slots'],1)}
    meta.update(nx=nx,ny=ny,nz=nz,dt_binary32_bits=0x3e800000,dx_binary32_bits=0x3f000000,
                beginning_time_binary64_bits=0,end_time_binary64_bits=0x3fd0000000000000)
    donor=np.ones((2,faces),dtype=np.float32);donor[:,::2]=0
    low=np.full((2,faces),2,dtype=np.float32);anti=np.full((2,faces),-4,dtype=np.float32)
    alpha=np.resize(np.array((0,.125,.25,.5,.75,1),dtype=np.float32),faces)
    result=witness(donor,low,anti,alpha,meta,2,1);wire=encode(result,meta,2,1)
    require(decode(wire)==(result,meta,2,1) and len(wire)==25680,'donor fixed numeric round trip')
    require([alpha_bin(x) for x in (0,.125,.25,.5,.75,1)]==list(range(6)),'fixed exact alpha endpoint bins')
    # Independently enumerate common upper z-face; native upper is different.
    zoffset=(nx+1)*ny*nz+nx*(ny+1)*nz;indices=[zoffset+x+nx*(y+ny*2) for y in range(ny) for x in range(nx)]
    selected=result['supports']['common'][(5*2)*12:(5*2)*12+12]
    expected=math.fsum((2-4*float(alpha[i]))/16 for i in indices)
    require(sum(row['count'] for row in selected)==48 and math.fsum(row['values']['limited_net_outward'] for row in selected if row['available'])==expected,
            'independent common upper histogram transfer')
    # Compare just this synthetic whole-surface arithmetic with the captured
    # accepted scalar oracle; actual reduction-tree equality is not asserted.
    scalar=captured(SCALAR_PATH,SCALAR_SHA)
    q=np.zeros((9,nz,ny,nx),dtype=np.float32);full_low=np.zeros((9,faces),dtype=np.float32);full_anti=full_low.copy()
    full_low[1:3]=low;full_anti[1:3]=anti
    check=scalar['region_transfers'](q,q.copy(),q.copy(),full_low,full_anti,alpha,np.zeros((ny,nx),dtype=np.uint8),.5,.25,2)
    require(check['fields'][1]['surfaces']['upper']['net_outward']==expected,'same synthetic limited scalar transfer')
    # Independent coordinate-loop scalar surface oracle across every orientation.
    offset_x=0;offset_y=(nx+1)*ny*nz;offset_z=offset_y+nx*(ny+1)*nz
    for support_name,height in (('native',3),('common',2)):
        expected_planes=(
            [offset_x+(nx+1)*(y+ny*z) for z in range(height) for y in range(ny)],
            [offset_x+nx+(nx+1)*(y+ny*z) for z in range(height) for y in range(ny)],
            [offset_y+x+nx*(ny+1)*z for z in range(height) for x in range(nx)],
            [offset_y+x+nx*(ny+(ny+1)*z) for z in range(height) for x in range(nx)],
            [offset_z+x+nx*y for y in range(ny) for x in range(nx)],
            [offset_z+x+nx*(y+ny*height) for y in range(ny) for x in range(nx)])
        for side,(indices,orientation) in enumerate(zip(expected_planes,(-1,1,-1,1,-1,1))):
            for species in range(2):
                selected=result['supports'][support_name][(side*2+species)*12:(side*2+species+1)*12]
                increments=[orientation*(2-4*float(alpha[i]))/16 for i in indices]
                for key,reference in (('limited_net_outward',math.fsum(increments)),
                                      ('incoming',math.fsum(max(-v,0.) for v in increments)),
                                      ('outgoing',math.fsum(max(v,0.) for v in increments))):
                    require(math.fsum(row['values'][key] for row in selected if row['available'])==reference,
                            'independent full six-surface signed/incoming/outgoing oracle')
                require(sum(row['count'] for row in selected)==len(indices),'independent all-surface count')
    red=0
    def refuse(fn):
        nonlocal red
        try:fn()
        except ValueError:red+=1
        else:raise AssertionError('missing donor refusal')
    for index,value in ((0,donor.astype(np.float64)),(0,np.full_like(donor,-1)),(1,low[:,:-1]),
                        (2,np.full_like(anti,np.nan)),(3,np.full_like(alpha,-1)),(3,np.full_like(alpha,2))):
        args=[donor,low,anti,alpha,meta,2,1];args[index]=value;refuse(lambda args=args:witness(*args))
    refuse(lambda:witness(donor,low,anti,alpha,meta,2,True));refuse(lambda:witness(donor,low,anti,alpha,meta,2,2))
    refuse(lambda:decode(wire[:-1]));refuse(lambda:decode(wire+b'\0'));refuse(lambda:decode(bytearray(wire)))
    for offset,fmt,value in ((0,'8s',b'INVALID!'),(16,'Q',2),(336,'Q',49),(344,'Q',2),
                             (352,'d',float('nan')),(352+8*8,'d',1.)):
        bad=bytearray(wire);struct.pack_into('<'+fmt,bad,offset,value);refuse(lambda bad=bad:decode(bytes(bad)))
    bad=copy.deepcopy(result);bad['supports']['common'].reverse();refuse(lambda:encode(bad,meta,2,1))
    bad=copy.deepcopy(result);bad['native_donor_state_authenticated']=True;refuse(lambda:encode(bad,meta,2,1))
    bad=bytearray(wire);struct.pack_into('<d',bad,352,1e308);refuse(lambda:decode(bytes(bad)))
    bad=bytearray(wire);struct.pack_into('<d',bad,HEADER_BYTES+ROW_BYTES+16,-0.)
    refuse(lambda:decode(bytes(bad)))
    for b in range(1,5):
        require(fractional_bin_consistent(4.,4.*(.125,.25,.5,.75)[b-1],b),
                'fractional bin lower/interior GREEN')
        refuse(lambda b=b:require(fractional_bin_consistent(4.,0.,b),'impossible fractional lower'))
        refuse(lambda b=b:require(fractional_bin_consistent(4.,4.,b),'impossible fractional upper'))
    for b,value in ((1,1.),(2,0.)):
        bad=copy.deepcopy(result)
        row=next(r for r in bad['supports']['native'] if r['available'] and r['alpha_bin']==ALPHA_BINS[b])
        v=row['values'];v['limited_anti_absolute']=value*v['anti_absolute']
        v['limited_anti_net_outward']=math.copysign(v['limited_anti_absolute'],v['anti_net_outward'])
        v['limited_net_outward']=math.fsum((v['low_net_outward'],v['limited_anti_net_outward']))
        v['incoming']=max(-v['limited_net_outward'],0.);v['outgoing']=max(v['limited_net_outward'],0.)
        v['component_residual']=math.fsum((v['limited_net_outward'],-v['low_net_outward'],-v['limited_anti_net_outward']))
        refuse(lambda bad=bad:encode(bad,meta,2,1))
    bad=copy.deepcopy(result)
    row=next(r for r in bad['supports']['common'] if r['side']=='bottom' and r['available'])
    for k in VALUES:row['values'][k]*=2
    refuse(lambda:encode(bad,meta,2,1))
    bad=copy.deepcopy(result)
    row=next(r for r in bad['supports']['common'] if r['side']=='xmin' and r['available'])
    for k in VALUES:row['values'][k]*=100
    refuse(lambda:encode(bad,meta,2,1))
    bad=copy.deepcopy(result)
    rows=[r for r in bad['supports']['native'] if r['side']=='upper' and r['species']=='CH4' and r['available']]
    rows[0]['count']+=1;rows[1]['count']-=1
    refuse(lambda:encode(bad,meta,2,1))
    require(red==34,'donor/type/layout/finite/alpha/stage/wire RED count')
    print(json.dumps(dict(status='pure_donor_limiter_witness_prerequisite_only',red=red,packet_bytes=len(wire),
                         native_fields_read=0,native_donor_state_authenticated=False,
                         physical_depletion_causality_accepted=False,new_simulation_launched=False)))


if __name__=='__main__':controls()
