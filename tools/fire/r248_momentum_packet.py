#!/usr/bin/env python3
"""Whole/unique-central face momentum oracle and fixed numeric wire.

Caller binary32 actual/stage Heun rates are bit-bound. This does not authenticate
native publications, reproduce a pressure solve, or split an EOS-restoration
impulse. Face sums (and sum/count means) are not volume/boundary integrals.
"""
from fractions import Fraction
import hashlib
import json
import math
from pathlib import Path
import struct
import numpy as np

ROOT=Path('/Users/aravind/Working/RISE/worktrees/r243-structure-diagnostic/tools/fire')
TARGET_SHA='35cf1d215582bb34ba4defb2ecf28953d874589075b7faaad07b188258123e4c'
CENTRAL_SHA='dcb3b42b3317b9b3c61d87e8107c32135759298f068b071d12273076e79f87a3'
QUANTITIES=('beginning','provisional','terminal','advection','buoyancy','stress','phase_source',
            'combined_projection','total','closure_residual')
STATS=('signed','absolute','positive','negative')
MAGIC=b'R248MOM1'
HEADER_BYTES=328
ROW_BYTES=336
PACKET_BYTES=4360


def require(ok,message):
    if not ok:raise ValueError(message)


def capture(name,sha):
    p=ROOT/name;raw=p.read_bytes();require(hashlib.sha256(raw).hexdigest()==sha,'accepted helper digest')
    c={'__name__':'captured_momentum_dependency'};exec(compile(raw,str(p),'exec'),c);return c


def header(metadata,layers):
    t=capture('r248_target_terms.py',TARGET_SHA);p=t['packet_context']();_,addresses,slots=p['header_layout'](metadata,layers)
    dt=struct.unpack('<f',struct.pack('<I',metadata['dt_binary32_bits']))[0]
    return t,addresses,slots,dt


def observe(beginning,provisional,terminal,rates,stages,metadata,layers=80):
    t,addresses,slots,dt=header(metadata,layers);faces=addresses['native_faces']
    for a in (beginning,provisional,terminal):
        require(type(a) is np.ndarray and a.dtype==np.float32 and a.shape==(faces,) and np.isfinite(a).all(),
                'finite binary32 packed momentum')
    require(type(rates) is np.ndarray and rates.dtype==np.float32 and rates.shape==(4,faces),'actual Heun rate layout')
    require(type(stages) is np.ndarray and stages.dtype==np.float32 and stages.shape==(4,2,faces),'stage rate layout')
    for i in range(4):
        require(np.isfinite(rates[i]).all() and np.isfinite(stages[i]).all(),'finite actual/stage rate')
        try:
            with np.errstate(over='raise',invalid='raise'):
                expected=np.float32(-.5 if i==0 else .5)*np.add(stages[i,0],stages[i,1],dtype=np.float32)
        except FloatingPointError as error:raise ValueError('recorded binary32 Heun sum overflow') from error
        require(np.array_equal(expected.view(np.uint32),rates[i].view(np.uint32)),
                'actual binary32 Heun operation tree bits including zero')
    output={}
    for name,support in addresses['supports'].items():
        groups={}
        for group in ('whole','central'):
            rows=[]
            for axis in range(3):
                bounds=support['face_ranges'][axis]
                indices=range(bounds['begin'],bounds['end']) if group=='whole' else support['central_face_indices'][axis]
                data=[]
                for index in indices:
                    m0,m1,m2=(float(a[index]) for a in (beginning,provisional,terminal))
                    actual=[float(rates[i,index]) for i in range(4)]
                    projection=(m2-m1)/dt;total=(m2-m0)/dt
                    closure=total-(actual[2]+actual[1]+actual[0]+actual[3]+projection)
                    row=[m0,m1,m2]+actual+[projection,total,closure]
                    require(all(math.isfinite(v) for v in row),'finite derived momentum values');data.append(row)
                moments={}
                for q,key in enumerate(QUANTITIES):
                    values=[v[q] for v in data]
                    moments[key]=dict(signed=math.fsum(values),absolute=math.fsum(abs(v) for v in values),
                                      positive=math.fsum(max(v,0.) for v in values),negative=math.fsum(min(v,0.) for v in values))
                rows.append(dict(axis=axis,count=len(data),available=True,moments=moments))
            groups[group]=rows
        output[name]=groups
    return dict(schema='rise.fire.r248.momentum-oracle.v1',supports=output,caller_values_authenticated=False,
                native_stage_lineage_authenticated=False,native_pressure_solve_replayed=False,
                pressure_restoration_split_available=False,full_accepted_history_authenticated=False,
                physical_acceptance=False)


def joint(t,m):
    s,a,p,n=(t['sum_interval_from_mean'](m[k],1) for k in STATS)
    sl,sh=s;al,ah=max(a[0],0),a[1];pl,ph=max(p[0],0),p[1];nl,nh=n[0],min(n[1],0)
    require(al<=ah and pl<=ph and nl<=nh and max(pl,nl+al,sl-nh,(sl+al)/2)<=
            min(ph,sh-nl,nh+ah,(sh+ah)/2),'joint signed/absolute positive/negative rounding witness')


def endpoint_link(moments,end,start,quantity,dt):
    # Two f32 endpoints: subtraction, division, aggregate RN versus two
    # independently aggregated endpoints. Absolute witnesses bound cancellation.
    u=Fraction(1,2**53);a=moments[end];b=moments[start]
    x=(Fraction(a['absolute'])+Fraction(b['absolute']))/(1-u)
    observed=(Fraction(a['signed'])-Fraction(b['signed']))/Fraction(dt)
    bound=(4*u+3*u*u+u**3)*x/Fraction(dt)
    require(abs(Fraction(moments[quantity]['signed'])-observed)<=bound,'endpoint-to-rate representation link')
    require(Fraction(moments[quantity]['absolute'])<=(1+u)**3*x/Fraction(dt),
            'endpoint-to-rate absolute triangle bound')


def encode(result,metadata,layers=80):
    t,addresses,slots,dt=header(metadata,layers)
    keys={'schema','supports','caller_values_authenticated','native_stage_lineage_authenticated',
          'native_pressure_solve_replayed','pressure_restoration_split_available','full_accepted_history_authenticated','physical_acceptance'}
    require(type(result) is dict and set(result)==keys and result['schema']=='rise.fire.r248.momentum-oracle.v1','exact momentum schema')
    for k in keys-{'schema','supports'}:require(result[k] is False,'no momentum provenance/physics promotion')
    require(type(result['supports']) is dict and set(result['supports'])=={'native','common'},'exact native/common supports')
    out=bytearray(MAGIC+struct.pack('<Q',layers)+struct.pack('<39Q',*(metadata[k] for k in slots)))
    fmax=float(np.finfo(np.float32).max)
    for name in ('native','common'):
        support=addresses['supports'][name];groups=result['supports'][name]
        require(type(groups) is dict and set(groups)=={'whole','central'},'exact whole/central supports')
        for group in ('whole','central'):
            rows=groups[group];require(type(rows) is list and len(rows)==3,'exact axis row cardinality')
            for axis,row in enumerate(rows):
                count=support['face_ranges'][axis]['count'] if group=='whole' else len(support['central_face_indices'][axis])
                require(type(row) is dict and set(row)=={'axis','count','available','moments'} and
                        type(row['axis']) is int and row['axis']==axis and type(row['count']) is int and row['count']==count and
                        row['available'] is True,'typed exact axis/count/availability')
                moments=row['moments'];require(type(moments) is dict and set(moments)==set(QUANTITIES),'exact momentum quantities')
                out.extend(struct.pack('<QQ',count,1))
                for i,key in enumerate(QUANTITIES):
                    m=moments[key];require(type(m) is dict and set(m)==set(STATS),'exact moment statistic keys')
                    values=[m[k] for k in STATS]
                    require(all(type(v) is float and math.isfinite(v) and (v!=0 or math.copysign(1.,v)==1.) for v in values),
                            'finite canonical momentum moments')
                    s,a,p,n=values
                    require(a>=abs(s) and a>=p and a>=-n and n<=s<=p and p>=0 and n<=0,'moment signs/absolute bounds')
                    bound=count*fmax if i<3 else count*.5*fmax if i<7 else count*(4*fmax/dt if i<9 else 8*fmax+8*fmax/dt)
                    require(all(abs(v)<=bound for v in values),'represented momentum magnitude bound')
                    joint(t,m);out.extend(struct.pack('<4d',*values))
                endpoint_link(moments,'terminal','provisional','combined_projection',dt)
                endpoint_link(moments,'terminal','beginning','total',dt)
                # The producer's left-associated five-input closure has five
                # rounded additions/subtractions, followed by the fsum RN. The
                # recorded component sums each add another aggregate RN bound.
                names=('total','stress','buoyancy','advection','phase_source','combined_projection')
                u=Fraction(1,2**53);gamma=5*u/(1-5*u)
                x=sum(Fraction(moments[k]['absolute']) for k in names)/(1-u)
                target=Fraction(moments['total']['signed'])-sum(Fraction(moments[k]['signed']) for k in names[1:])
                require(abs(Fraction(moments['closure_residual']['signed'])-target)<=((1+u)*gamma+2*u)*x,
                        'observed closure representation bound, not forced zero')
                require(Fraction(moments['closure_residual']['absolute'])<=(1+u)*(1+gamma)*x,
                        'observed closure absolute triangle bound')
    def subset(a,b):
        require(a['count']<=b['count'],'subset face counts')
        for key in QUANTITIES:
            x,y=a['moments'][key],b['moments'][key]
            require(x['absolute']<=y['absolute'] and x['positive']<=y['positive'] and x['negative']>=y['negative'],
                    'subset nonnegative/negative reduction support')
            if a['count']==b['count']:
                require(struct.pack('<4d',*(x[k] for k in STATS))==struct.pack('<4d',*(y[k] for k in STATS)),
                        'identical face set exact moments')
    for name in ('native','common'):
        for axis in range(3):subset(result['supports'][name]['central'][axis],result['supports'][name]['whole'][axis])
    for group in ('whole','central'):
        for axis in range(3):subset(result['supports']['common'][group][axis],result['supports']['native'][group][axis])
    require(len(out)==PACKET_BYTES,'fixed momentum numeric wire size');return bytes(out)


def decode(payload):
    require(type(payload) is bytes and len(payload)==PACKET_BYTES and payload[:8]==MAGIC,'immutable exact momentum bytes')
    layers=struct.unpack_from('<Q',payload,8)[0]
    t=capture('r248_target_terms.py',TARGET_SHA);p=t['packet_context']();l=p['accounting_context']()['layout_context']()
    names=list(l['layout']((2,2,1),1)['metadata_uint64_slots']);metadata=dict(zip(names,struct.unpack_from('<39Q',payload,16)))
    output={};cursor=HEADER_BYTES
    for name in ('native','common'):
        groups={}
        for group in ('whole','central'):
            rows=[]
            for axis in range(3):
                count,available=struct.unpack_from('<QQ',payload,cursor);cursor+=16;moments={}
                require(available==1,'required face reduction cannot default to unavailable')
                for key in QUANTITIES:
                    moments[key]=dict(zip(STATS,struct.unpack_from('<4d',payload,cursor)));cursor+=32
                rows.append(dict(axis=axis,count=count,available=True,moments=moments))
            groups[group]=rows
        output[name]=groups
    result=dict(schema='rise.fire.r248.momentum-oracle.v1',supports=output,caller_values_authenticated=False,
                native_stage_lineage_authenticated=False,native_pressure_solve_replayed=False,
                pressure_restoration_split_available=False,full_accepted_history_authenticated=False,physical_acceptance=False)
    require(encode(result,metadata,layers)==payload,'canonical momentum wire roundtrip');return result,metadata,layers


def packet_layout():
    return dict(schema='rise.fire.r248.momentum-layout.v1',header_bytes=HEADER_BYTES,row_bytes=ROW_BYTES,
                rows=12,packet_bytes=PACKET_BYTES,quantities=QUANTITIES,stats=STATS,
                support_order=('native','common'),region_order=('whole','central'),axis_order=(0,1,2),
                units=('kg/m2/s',)*3+('kg/m2/s2',)*7,
                sums_and_means_are_volume_or_boundary_integrals=False,native_stage_authenticated=False,
                pressure_restoration_split_available=False,encoded_bytes_are_measured_allocation=False,new_simulation_launched=False)


def controls():
    import copy
    dims=(6,8,3);layers=2;t=capture('r248_target_terms.py',TARGET_SHA);p=t['packet_context']()
    l=p['accounting_context']()['layout_context']()['layout'](dims,layers);f=l['native_faces']
    meta={k:i for i,k in enumerate(l['metadata_uint64_slots'],1)}
    meta.update(nx=6,ny=8,nz=3,dx_binary32_bits=0x3f000000,dt_binary32_bits=0x3e800000,
                beginning_time_binary64_bits=0,end_time_binary64_bits=0x3fd0000000000000)
    m0=np.zeros(f,np.float32);m1=np.ones(f,np.float32);m2=np.full(f,3,np.float32)
    stages=np.empty((4,2,f),np.float32)
    for i,v in enumerate((2,4,6,8)):stages[i]=v
    rates=np.array([stages[i,0]*(-1 if i==0 else 1) for i in range(4)],np.float32)
    r=observe(m0,m1,m2,rates,stages,meta,layers);wire=encode(r,meta,layers)
    require(decode(wire)==(r,meta,layers),'whole/central momentum roundtrip')
    for name,h in (('native',3),('common',2)):
        for group in ('whole','central'):
            for axis,row in enumerate(r['supports'][name][group]):
                n=row['count'];require([row['moments'][k]['signed']/n for k in QUANTITIES]==[0,1,3,-2,4,6,8,8,12,-12],
                                      'constant analytic sums and sum/count means')
    old=capture('r247_central_momentum.py',CENTRAL_SHA)['central_momentum'](m0,m1,m2,rates,stages,dims,.25,layers)
    require(all(r['supports']['common']['central'][a]['moments'][k]['signed']/r['supports']['common']['central'][a]['count']==
                old['common']['axes'][a]['summary'][k]['mean'] for a in range(3) for k in QUANTITIES),
            'exact synthetic accepted central helper discriminator')
    red=0
    def refuse(fn):
        nonlocal red
        try:fn()
        except ValueError:red+=1
        else:raise AssertionError('missing momentum refusal')
    for index,value in ((0,m0.astype(np.float64)),(1,m1[:-1]),(2,np.full(f,np.nan,np.float32)),(3,rates[:3]),(4,stages[:,:1])):
        args=[m0,m1,m2,rates,stages,meta,layers];args[index]=value;refuse(lambda args=args:observe(*args))
    bad_rates=rates.copy();bad_rates[2,0]=0;refuse(lambda:observe(m0,m1,m2,bad_rates,stages,meta,layers))
    zero_stages=stages.copy();zero_stages[2,:,0]=0;zero_rates=rates.copy();zero_rates[2,0]=np.float32(-0.)
    refuse(lambda:observe(m0,m1,m2,zero_rates,zero_stages,meta,layers))
    refuse(lambda:decode(wire[:-1]));refuse(lambda:decode(wire+b'0'));refuse(lambda:decode(bytearray(wire)))
    for offset,fmt,value in ((0,'8s',b'INVALID!'),(328,'Q',0),(336,'Q',2),(344,'d',float('nan')),(352,'d',-1.),(376,'d',1e308)):
        b=bytearray(wire);struct.pack_into('<'+fmt,b,offset,value);refuse(lambda b=b:decode(bytes(b)))
    bad=copy.deepcopy(r);bad['physical_acceptance']=True;refuse(lambda:encode(bad,meta,layers))
    bad=copy.deepcopy(r);bad['supports']['common']['central'].reverse();refuse(lambda:encode(bad,meta,layers))
    bad=copy.deepcopy(r)
    for stat in ('signed','absolute','positive'):bad['supports']['common']['whole'][0]['moments']['combined_projection'][stat]=0.
    refuse(lambda:encode(bad,meta,layers))
    zz=observe(m0,m0,m0,rates,stages,meta,layers)
    for name in ('native','common'):
        for group in ('whole','central'):
            for row in zz['supports'][name][group]:
                row['moments']['combined_projection'].update(absolute=2.,positive=1.,negative=-1.)
    refuse(lambda:encode(zz,meta,layers))
    require(red==20,'momentum RED count')
    print(json.dumps(dict(status='pure_momentum_arithmetic_wire_only',red=red,packet_bytes=len(wire),native_fields_read=0,
                         pressure_restoration_split_available=False,native_allocation_accepted=False,new_simulation_launched=False)))


if __name__=='__main__':controls()
