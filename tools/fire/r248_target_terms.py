#!/usr/bin/env python3
"""Pure target-term summaries/wire; supplied fields and stage IDs unauthenticated.

No EOS expansion/enclosure replay, native observer, pressure response or physical
acceptance. Composed is BEFORE finalize_resident_target; issued_final is AFTER.
"""
import hashlib
import json
import math
from fractions import Fraction
from pathlib import Path
import struct
import numpy as np

PACKET_HELPER=Path('/Users/aravind/Working/RISE/worktrees/r243-structure-diagnostic/tools/fire/r248_projection_packet.py')
PACKET_HELPER_SHA='5b4ab5b114645c923a1d38375ae9a671050307a849848b5ec2d5827c0c41a817'
FIELDS=('tangent','frozen_source','absolute_diagnostic','monitored_tail','base_assembled',
        'parent_assembled','composed_before_finalize','issued_final','consumed_projection_target')
DERIVED=('composed_minus_parent','composition_endpoint_minus_tail','finalization_change','issued_minus_consumed')
QUANTITIES=FIELDS+DERIVED
MAGIC=b'R248EOS1'
HEADER_BYTES=336
ROW_BYTES=48
PACKET_BYTES=1584


def require(condition,message):
    if not condition:raise ValueError(message)


def packet_context():
    raw=PACKET_HELPER.read_bytes()
    require(hashlib.sha256(raw).hexdigest()==PACKET_HELPER_SHA,'accepted packet helper hash')
    context={'__name__':'captured_packet'};exec(compile(raw,str(PACKET_HELPER),'exec'),context)
    return context


def stage_header(metadata,layers,stage):
    require(type(stage) is int and 0<=stage<3,'typed R0/R1/R2 stage')
    context=packet_context();_,addresses,slots=context['header_layout'](metadata,layers)
    iteration=metadata['R'+str(stage)+'.issued_target_correction_iteration']
    require(iteration<=2**32-1 and metadata['R'+str(stage)+'.consumed_target_correction_iteration']<=2**32-1,
            'native uint32 consumed/issued correction iteration representation')
    return addresses,slots,iteration


def summarize(dimensions,operands,metadata,layers=80,stage=0):
    addresses,slots,iteration=stage_header(metadata,layers,stage)
    require(type(dimensions) is tuple and dimensions==tuple(metadata[key] for key in ('nx','ny','nz')) and
            all(type(x) is int for x in dimensions),'exact typed operand/header domain')
    require(type(operands) is dict and set(operands)==set(FIELDS),'exact target field set')
    for value in operands.values():
        require(type(value) is np.ndarray and value.dtype==np.float32 and value.shape==(addresses['cells'],) and
                np.isfinite(value).all(),'finite native-layout binary32 target operands')
    composed=operands['composed_before_finalize']
    if iteration==0:
        require(np.array_equal(composed.view(np.uint32),operands['base_assembled'].view(np.uint32)),
                'correction zero exact base copy before finalization')
    else:
        require(np.all(composed.view(np.uint32)[composed==0]==0),'corrected composed zero canonical positive')
    # Do not compare composed against numpy-rounded parent+tail: native strict
    # EOSDD interval-bin certification is an additional, unavailable contract.
    supports={}
    for name,support in addresses['supports'].items():
        count=support['cell_prefix_count'];summaries={}
        def values(quantity):
            for i in range(count):
                if quantity in FIELDS:yield float(operands[quantity][i]);continue
                parent=float(operands['parent_assembled'][i]);tail=float(operands['monitored_tail'][i])
                current=float(composed[i]);final=float(operands['issued_final'][i])
                if quantity=='composed_minus_parent':yield current-parent
                elif quantity=='composition_endpoint_minus_tail':yield math.fsum((current,-parent,-tail))
                elif quantity=='finalization_change':yield final-current
                else:yield final-float(operands['consumed_projection_target'][i])
        for quantity in QUANTITIES:
            signed=math.fsum(values(quantity))/count
            absolute=math.fsum(abs(v) for v in values(quantity))/count
            positive=math.fsum(max(v,0.) for v in values(quantity))/count
            negative=math.fsum(min(v,0.) for v in values(quantity))/count
            summaries[quantity]=dict(count=count,available=True,signed_mean=signed,absolute_mean=absolute,
                                     positive_mean=positive,negative_mean=negative)
        supports[name]=summaries
    return dict(schema='rise.fire.r248.target-term-oracle.v1',stage=stage,unit='1/s',supports=supports,
                caller_values_authenticated=False,stage_lineage_authenticated=False,
                EOS_interval_composition_certified=False,finalization_boundary_authenticated=False,
                sequential_restoration_impulse_available=False)


def sum_interval_from_mean(mean,count):
    """Enclose exact sum for RN(RN(sum)/count), using binary64 bins only."""
    center=Fraction.from_float(mean)
    low=(Fraction.from_float(math.nextafter(mean,-math.inf))+center)/2
    high=(center+Fraction.from_float(math.nextafter(mean,math.inf)))/2
    low*=count;high*=count
    lower=float(low);upper=float(high)
    # Smallest/largest represented sum inside the division-rounding bin.
    if Fraction.from_float(lower)<low:lower=math.nextafter(lower,math.inf)
    if Fraction.from_float(upper)>high:upper=math.nextafter(upper,-math.inf)
    require(lower<=upper,'nonempty represented sum interval')
    return ((Fraction.from_float(math.nextafter(lower,-math.inf))+Fraction.from_float(lower))/2,
            (Fraction.from_float(upper)+Fraction.from_float(math.nextafter(upper,math.inf)))/2)


def linked_moments(numbers,count):
    # Every source is binary32; differences use at most three represented
    # operands. Bound before Fraction-to-float conversion; no arbitrary epsilon.
    require(all(abs(v)<=3*float(np.finfo(np.float32).max) for v in numbers),
            'moments within represented three-term operand magnitude bound')
    signed,absolute,positive,negative=[sum_interval_from_mean(v,count) for v in numbers]
    # One COMMON positive/negative-sum witness must satisfy both identities.
    # Eliminate N from intersecting [nl,nh], [sl-P,sh-P], [P-ah,P-al].
    # Independent pairwise signed/absolute overlaps are insufficient.
    sl,sh=signed;al,ah=max(absolute[0],0),absolute[1]
    pl,ph=max(positive[0],0),positive[1];nl,nh=negative[0],min(negative[1],0)
    lower=max(pl,nl+al,sl-nh,(sl+al)/2)
    upper=min(ph,sh-nl,nh+ah,(sh+ah)/2)
    require(al<=ah and pl<=ph and nl<=nh and lower<=upper,
            'signed/absolute split moments share one exact rounding witness')


def encode(result,metadata,layers=80,stage=0):
    addresses,slots,iteration=stage_header(metadata,layers,stage)
    keys={'schema','stage','unit','supports','caller_values_authenticated','stage_lineage_authenticated',
          'EOS_interval_composition_certified','finalization_boundary_authenticated','sequential_restoration_impulse_available'}
    require(type(result) is dict and set(result)==keys and result['schema']=='rise.fire.r248.target-term-oracle.v1' and
            type(result['stage']) is int and result['stage']==stage and result['unit']=='1/s','exact target summary schema/stage/unit')
    for key in keys-{'schema','stage','unit','supports'}:require(result[key] is False,'no target authentication promotion')
    require(type(result['supports']) is dict and set(result['supports'])=={'native','common'},'exact target support set')
    output=bytearray(MAGIC+struct.pack('<QQ',layers,stage))
    for key in slots:output.extend(struct.pack('<Q',metadata[key]))
    for name in ('native','common'):
        rows=result['supports'][name];count=addresses['supports'][name]['cell_prefix_count']
        require(type(rows) is dict and set(rows)==set(QUANTITIES),'exact target quantity set')
        for quantity in QUANTITIES:
            summary=rows[quantity]
            require(type(summary) is dict and set(summary)=={'count','available','signed_mean','absolute_mean','positive_mean','negative_mean'} and
                    type(summary['count']) is int and summary['count']==count and summary['available'] is True,
                    'present target summary count exactly binds cells')
            numbers=[summary[key] for key in ('signed_mean','absolute_mean','positive_mean','negative_mean')]
            require(all(type(x) is float and math.isfinite(x) for x in numbers),'finite binary64 target moments')
            signed,absolute,positive,negative=numbers
            require(absolute>=abs(signed) and absolute>=positive and absolute>=-negative and
                    negative<=signed<=positive and positive>=0 and negative<=0 and
                    all(v!=0 or math.copysign(1.,v)==1. for v in (absolute,positive,negative)),
                    'signed/absolute/support signs and canonical split zero')
            linked_moments(numbers,count)
            output.extend(struct.pack('<QQdddd',count,1,*numbers))
    require(len(output)==PACKET_BYTES,'fixed target wire bytes')
    return bytes(output)


def decode(payload):
    require(type(payload) is bytes and len(payload)==PACKET_BYTES and payload[:8]==MAGIC,'exact target captured wire bytes')
    layers,stage=struct.unpack_from('<QQ',payload,8)
    context=packet_context();names=list(context['accounting_context']()['layout_context']()['layout']((2,2,1),1)['metadata_uint64_slots'])
    metadata=dict(zip(names,struct.unpack_from('<39Q',payload,24)))
    supports={};cursor=HEADER_BYTES
    for name in ('native','common'):
        rows={}
        for quantity in QUANTITIES:
            count,available,signed,absolute,positive,negative=struct.unpack_from('<QQdddd',payload,cursor);cursor+=ROW_BYTES
            require(available==1,'required target field cannot be unavailable/default-zero')
            rows[quantity]=dict(count=count,available=True,signed_mean=signed,absolute_mean=absolute,
                                positive_mean=positive,negative_mean=negative)
        supports[name]=rows
    result=dict(schema='rise.fire.r248.target-term-oracle.v1',stage=stage,unit='1/s',supports=supports,
                caller_values_authenticated=False,stage_lineage_authenticated=False,EOS_interval_composition_certified=False,
                finalization_boundary_authenticated=False,sequential_restoration_impulse_available=False)
    require(encode(result,metadata,layers,stage)==payload,'canonical target wire round trip')
    return result,metadata,layers,stage


def packet_layout():
    return dict(schema='rise.fire.r248.target-term-packet-layout.v1',magic=MAGIC.decode(),byte_order='little',
                header_bytes=HEADER_BYTES,row_bytes=ROW_BYTES,rows=2*len(QUANTITIES),packet_bytes=PACKET_BYTES,
                support_order=('native','common'),quantity_order=QUANTITIES,
                numeric_columns=('count_uint64','available_uint64','signed_mean_binary64','absolute_mean_binary64',
                                 'positive_mean_binary64','negative_mean_binary64'),unit='1/s',
                stage_lineage_authenticated=False,EOS_interval_composition_certified=False,
                native_allocation_accepted=False,full_observer_numeric_layout_complete=False)


def controls():
    import copy
    dims=(6,8,3);count=math.prod(dims);prefix=6*8*2
    context=packet_context();slots=context['accounting_context']()['layout_context']()['layout'](dims,2)['metadata_uint64_slots']
    metadata={name:i for i,name in enumerate(slots,1)}
    metadata.update(nx=6,ny=8,nz=3,dt_binary32_bits=0x3f000000,dx_binary32_bits=0x3d800000,
                    beginning_time_binary64_bits=0,end_time_binary64_bits=0x3fe0000000000000)
    metadata['R1.issued_target_correction_iteration']=0
    raw=(np.arange(count,dtype=np.float32)%np.float32(11))-np.float32(5)
    operands={key:raw.copy()+np.float32(j) for j,key in enumerate(FIELDS)}
    operands['composed_before_finalize']=operands['base_assembled'].copy()
    result=summarize(dims,operands,metadata,2,1);wire=encode(result,metadata,2,1)
    require(decode(wire)==(result,metadata,2,1) and len(wire)==1584,'target numeric wire round trip')
    require(struct.unpack_from('<QQ',wire,8)==(2,1) and struct.unpack_from('<QQ',wire,336)==(count,1),
            'independent target header/row byte offsets')
    # Independent coordinate prefix oracle; no helper ranges in expected values.
    for name,height in (('native',3),('common',2)):
        selected=[x+6*(y+8*z) for z in range(height) for y in range(8) for x in range(6)]
        for field in FIELDS:
            v=[float(operands[field][i]) for i in selected];row=result['supports'][name][field]
            require(row['count']==len(v) and row['signed_mean']==math.fsum(v)/len(v) and
                    row['absolute_mean']==math.fsum(abs(x) for x in v)/len(v) and
                    row['positive_mean']==math.fsum(max(x,0.) for x in v)/len(v) and
                    row['negative_mean']==math.fsum(min(x,0.) for x in v)/len(v),'independent target moment oracle')
    require(result['supports']['common']['finalization_change']['signed_mean']==3. and
            result['supports']['common']['issued_minus_consumed']['signed_mean']==-1.,
            'finalization and consumed/issued differences distinct')
    # Correction input/output subtraction is observed; exact midpoint remains
    # an uncertified composition, never silently promoted to native interval proof.
    metadata2=dict(metadata);metadata2['R1.issued_target_correction_iteration']=1
    midpoint={key:np.ones(count,dtype=np.float32) for key in FIELDS}
    midpoint['monitored_tail'][:]=np.float32(2**-24)
    mid=summarize(dims,midpoint,metadata2,2,1)
    require(mid['supports']['common']['composition_endpoint_minus_tail']['signed_mean']==-2**-24 and
            mid['EOS_interval_composition_certified'] is False,'midpoint residual is observed and uncertified')
    cancellation={key:np.ones(count,dtype=np.float32) for key in FIELDS}
    cancellation['parent_assembled'][:]=np.float32(2**100)
    cancellation['monitored_tail'][:]=np.float32(-2**100)
    stable=summarize(dims,cancellation,metadata2,2,1)
    require(stable['supports']['common']['composition_endpoint_minus_tail']['signed_mean']==1.,
            'endpoint-tail cancellation retains small represented component')
    # Independent rational sum witness for both reduction and division rounding.
    interval_green=0
    for exponent in (-149,-126,-24,0,24,100,126):
        for v in ((2.**exponent,-2.**exponent,2.**-149),
                  (2.**exponent,2.**(exponent-23),-2.**exponent),
                  (2.**exponent,2.**exponent,-2.**(exponent-23))):
            exact=sum((Fraction.from_float(x) for x in v),Fraction())
            mean=math.fsum(v)/len(v);lo,hi=sum_interval_from_mean(mean,len(v))
            require(lo<=exact<=hi,'independent exact rational rounding interval witness')
            interval_green+=1
    joint_green=0
    rng=np.random.default_rng(248)
    for trial in range(128):
        values=[float(np.float32(math.ldexp(float(rng.uniform(-.5,.5)),int(rng.integers(-149,127)))))
                for _ in range(31)]
        moments=(math.fsum(values)/31,math.fsum(abs(v) for v in values)/31,
                 math.fsum(max(v,0.) for v in values)/31,math.fsum(min(v,0.) for v in values)/31)
        linked_moments(moments,31)
        joint_green+=1
    red=0
    def refuse(fn):
        nonlocal red
        try:fn()
        except ValueError:red+=1
        else:raise AssertionError('missing target refusal')
    for key,value in [('tangent',raw.astype(np.float64)),('monitored_tail',raw[:-1]),('issued_final',np.full(count,np.nan,dtype=np.float32))]:
        bad=dict(operands);bad[key]=value;refuse(lambda bad=bad:summarize(dims,bad,metadata,2,1))
    bad=dict(operands);del bad['absolute_diagnostic'];refuse(lambda:summarize(dims,bad,metadata,2,1))
    bad=dict(operands);bad['composed_before_finalize']=operands['base_assembled']+np.float32(1)
    refuse(lambda:summarize(dims,bad,metadata,2,1))
    negative=dict(midpoint);negative['composed_before_finalize']=np.full(count,-0.,dtype=np.float32)
    refuse(lambda:summarize(dims,negative,metadata2,2,1))
    for call in (lambda:summarize(dims,operands,metadata,2,True),lambda:summarize(dims,operands,metadata,2,3),
                 lambda:summarize((6,8,2),operands,metadata,2,1)):
        refuse(call)
    large=dict(metadata);large['R1.issued_target_correction_iteration']=2**32
    refuse(lambda:summarize(dims,operands,large,2,1))
    for index,fmt,value in [(0,'8s',b'INVALID!'),(16,'Q',3),(336,'Q',prefix),(344,'Q',0),
                            (352,'d',float('inf')),(360,'d',-1.),(368,'d',-1.),(376,'d',1.),(376,'d',-0.)]:
        bad=bytearray(wire);struct.pack_into('<'+fmt,bad,index,value)
        refuse(lambda bad=bad:decode(bytes(bad)))
    refuse(lambda:decode(wire[:-1]));refuse(lambda:decode(wire+b'\0'));refuse(lambda:decode(bytearray(wire)))
    bad=copy.deepcopy(result);bad['EOS_interval_composition_certified']=True;refuse(lambda:encode(bad,metadata,2,1))
    for numbers in ((0.,100.,1.,-1.),(0.,2.,1.,0.),(float(2**140),float(2**140),float(2**140),0.)):
        bad=bytearray(wire);struct.pack_into('<dddd',bad,352,*numbers)
        refuse(lambda bad=bad:decode(bytes(bad)))
    # Both prior pairwise-overlap false greens need one joint witness.
    refuse(lambda:linked_moments((1.4999999999999996,4.500000000000001,3.,-1.5),865280))
    refuse(lambda:linked_moments((229850.36503290065,229850.9436906224,229850.65436176152,-0.2893288608629651),288))
    require(red==28,'target field/stage/count/availability/wire RED count')
    print(json.dumps(dict(status='pure_target_term_numeric_prerequisite_only',red=red,interval_green=interval_green,joint_green=joint_green,packet_bytes=len(wire),
                         native_fields_read=0,EOS_interval_composition_certified=False,
                         stage_lineage_authenticated=False,new_simulation_launched=False)))


if __name__=='__main__':controls()
