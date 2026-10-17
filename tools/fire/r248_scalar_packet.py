#!/usr/bin/env python3
"""Pure scalar balance oracle/fixed wire; no native issuance or full history.

Reductions use math.fsum over caller binary32 cell/face operands. This is an
independent reduction oracle, not a replay of Metal's reduction tree. Encoded
bytes do not establish resident memory cost, ownership, stage or boundary law.
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
SCALAR_SHA='4e814f7cb55739cd39d645e2136072c0e2e2bc6b56599e2f19536de3072ed754'
FIELDS=('mixture_fraction_density','CH4','O2','N2','CO2','H2O','CO','carbon','sensible_enthalpy','total_species')
SURFACES=('xmin','xmax','ymin','ymax','bottom','upper','bottom_burner','bottom_nonburner')
VALUES=('beginning','candidate','frozen_source_increment','beginning_absolute','candidate_absolute','frozen_source_absolute','net_outward','closure_residual')
MAGIC=b'R248SCL1'
HEADER_BYTES=328
ROW_BYTES=328
PACKET_BYTES=6888


def require(ok,message):
    if not ok:raise ValueError(message)


def capture(name,sha):
    p=ROOT/name;raw=p.read_bytes();require(hashlib.sha256(raw).hexdigest()==sha,'accepted dependency digest')
    result={'__name__':'captured_scalar_dependency'};exec(compile(raw,str(p),'exec'),result);return result


def header(metadata,layers):
    target=capture('r248_target_terms.py',TARGET_SHA);packet=target['packet_context']()
    _,addresses,slots=packet['header_layout'](metadata,layers)
    dx=struct.unpack('<f',struct.pack('<I',metadata['dx_binary32_bits']))[0]
    dt=struct.unpack('<f',struct.pack('<I',metadata['dt_binary32_bits']))[0]
    return target,addresses,slots,dx**3,dx**2*dt


def planes(a,dims,height):
    nx,ny,nz=dims;fx=(nx+1)*ny*nz;fy=nx*(ny+1)*nz
    x=a[:fx].reshape(nz,ny,nx+1);y=a[fx:fx+fy].reshape(nz,ny+1,nx);z=a[fx+fy:].reshape(nz+1,ny,nx)
    return (x[:height,:,0],x[:height,:,nx],y[:height,0,:],y[:height,ny,:],z[0],z[height])


def observe(q0,q1,source,low,anti,alpha,burner,metadata,layers=80):
    target,addresses,slots,volume,measure=header(metadata,layers)
    nx,ny,nz=addresses['dimensions'];faces=addresses['native_faces']
    for a in (q0,q1,source):
        require(type(a) is np.ndarray and a.dtype==np.float32 and a.shape==(9,nz,ny,nx),'binary32 cell layout')
    for a in (low,anti):
        require(type(a) is np.ndarray and a.dtype==np.float32 and a.shape==(9,faces),'binary32 flux layout')
    require(type(alpha) is np.ndarray and alpha.dtype==np.float32 and alpha.shape==(faces,) and
            np.isfinite(alpha).all() and np.all(alpha>=0) and np.all(alpha<=1),'binary32 limiter layout/range')
    require(type(burner) is np.ndarray and burner.dtype==np.uint8 and burner.shape==(ny,nx) and
            np.all((burner==0)|(burner==1)),'literal burner mask')
    for i in range(9):require(all(np.isfinite(a[i]).all() for a in (q0,q1,source,low,anti)),'finite field operands')
    output={}
    for name,support in addresses['supports'].items():
        h=support['layers'];ap=planes(alpha,(nx,ny,nz),h);rows=[]
        for field,label in enumerate(FIELDS):
            indices=(field,) if field<9 else tuple(range(1,8))
            inventories=[];absolute_inventories=[]
            for state in (q0,q1,source):
                products=[math.fsum(float(state[i,z,y,x]) for i in indices)*volume
                          for z in range(h) for y in range(ny) for x in range(nx)]
                inventories.append(math.fsum(products));absolute_inventories.append(math.fsum(abs(v) for v in products))
            lp=[planes(low[i],(nx,ny,nz),h) for i in indices]
            adp=[planes(anti[i],(nx,ny,nz),h) for i in indices]
            surfaces=[]
            for side,direction in enumerate((-1,1,-1,1,-1,1)):
                shape=ap[side].shape
                increments=[math.fsum(float(lp[k][side][coord])+float(ap[side][coord])*float(adp[k][side][coord])
                                     for k in range(len(indices)))*(direction*measure)
                            for coord in np.ndindex(shape)]
                def reduction(values):
                    return dict(count=len(values),incoming=math.fsum(max(-v,0.) for v in values),
                                outgoing=math.fsum(max(v,0.) for v in values),net_outward=math.fsum(values))
                surfaces.append(reduction(increments))
                if side==4:
                    mask=burner.ravel();bottom=[reduction([v for v,m in zip(increments,mask) if m==b]) for b in (1,0)]
            surfaces+=bottom
            net=math.fsum(v['net_outward'] for v in surfaces[:6])
            start,end,inc=inventories
            rows.append(dict(field=label,unit='J' if field==8 else 'kg',cell_count=nx*ny*h,
                             beginning=start,candidate=end,frozen_source_increment=inc,
                             beginning_absolute=absolute_inventories[0],candidate_absolute=absolute_inventories[1],
                             frozen_source_absolute=absolute_inventories[2],net_outward=net,
                             closure_residual=math.fsum((end,-start,-inc,net)),surfaces=surfaces))
        output[name]=rows
    return dict(schema='rise.fire.r248.scalar-oracle.v1',supports=output,caller_values_authenticated=False,
                burner_configuration_authenticated=False,full_accepted_history_authenticated=False,
                native_reduction_replayed=False,physical_acceptance=False)


def species_consistent(signed,absolute,total_signed,total_absolute):
    """Necessary signed/triangle checks for the shared species formation.

    X is the unrounded sum of absolute represented operand*measure products.
    Individual leaf+aggregate RN stages give |sum(S_i)-U|<=u(2+u)X.
    Per-cell/per-face total formation adds one RN stage, giving
    |T-U|<=((1+u)^3-1)X. Measured positive absolute sums (or exact Fraction
    incoming+outgoing pairs) bound X by sum(A_i)/(1-u)^2. All nonzero
    products are normal (cell min2**-596, limited face min2**-745).
    """
    u=Fraction(1,2**53)
    observed_signed=sum(Fraction(v) for v in signed)
    observed_absolute=sum(Fraction(v) for v in absolute)
    x=observed_absolute/(1-u)**2
    require(abs(Fraction(total_signed)-observed_signed)<=(5*u+4*u*u+u**3)*x,
            'cross-species signed representation bound')
    require(Fraction(total_absolute)<=(1+u)**3*x,'total species absolute triangle representation bound')


def encode(result,metadata,layers=80):
    target,addresses,slots,volume,measure=header(metadata,layers)
    keys={'schema','supports','caller_values_authenticated','burner_configuration_authenticated',
          'full_accepted_history_authenticated','native_reduction_replayed','physical_acceptance'}
    require(type(result) is dict and set(result)==keys and result['schema']=='rise.fire.r248.scalar-oracle.v1','exact scalar schema')
    for k in keys-{'schema','supports'}:require(result[k] is False,'no provenance/physical promotion')
    require(type(result['supports']) is dict and set(result['supports'])=={'native','common'},'exact scalar supports')
    out=bytearray(MAGIC+struct.pack('<Q',layers)+struct.pack('<39Q',*(metadata[k] for k in slots)))
    nx,ny,nz=addresses['dimensions'];fmax=float(np.finfo(np.float32).max)
    interval=lambda v:target['sum_interval_from_mean'](v,1)
    for name in ('native','common'):
        h=addresses['supports'][name]['layers'];rows=result['supports'][name]
        require(type(rows) is list and len(rows)==10,'exact field cardinality')
        expected_counts=(ny*h,ny*h,nx*h,nx*h,nx*ny,nx*ny)
        for field,row in enumerate(rows):
            require(type(row) is dict and set(row)=={'field','unit','cell_count','surfaces',*VALUES} and
                    row['field']==FIELDS[field] and row['unit']==('J' if field==8 else 'kg') and
                    type(row['cell_count']) is int and row['cell_count']==nx*ny*h,'exact field order/unit/cell count')
            values=[row[k] for k in VALUES]
            require(all(type(v) is float and math.isfinite(v) and (v!=0 or math.copysign(1.,v)==1.) for v in values),
                    'finite canonical scalar values')
            multiplicity=7 if field==9 else 1
            require(all(abs(v)<=2*multiplicity*fmax*volume*row['cell_count'] for v in values[:6]),'represented cell magnitude bound')
            require(all(values[3+i]>=abs(values[i]) for i in range(3)),'inventory absolute dominates signed')
            surfaces=row['surfaces'];require(type(surfaces) is list and len(surfaces)==8,'six surfaces plus bottom partition')
            for side,s in enumerate(surfaces):
                require(type(s) is dict and set(s)=={'count','incoming','outgoing','net_outward'} and
                        type(s['count']) is int and 0<=s['count']<=nx*ny*h,'typed bounded surface count')
                if side<6:require(s['count']==expected_counts[side],'exact geometric surface count')
                require(all(type(s[k]) is float and math.isfinite(s[k]) and (s[k]!=0 or math.copysign(1.,s[k])==1.)
                            for k in ('incoming','outgoing','net_outward')),'finite canonical surface values')
                a,b,net=(s[k] for k in ('incoming','outgoing','net_outward'))
                require(a>=0 and b>=0 and all(abs(v)<=4*multiplicity*fmax*measure*s['count'] for v in (a,b,net)),
                        'nonnegative splits and represented flux magnitude bound')
                if s['count']==0:require(a==b==net==0,'known empty partition exact zero')
                if s['count']==1:
                    require(a==max(-net,0.) and b==max(net,0.),'one face exact sign partition')
                ai,bi,ni=interval(a),interval(b),interval(net)
                require(max(ni[0],bi[0]-ai[1])<=min(ni[1],bi[1]-ai[0]),'one signed incoming/outgoing rounding witness')
            require(surfaces[6]['count']+surfaces[7]['count']==nx*ny,'bottom partition count')
            for partition,other in ((6,7),(7,6)):
                if surfaces[partition]['count']==0:
                    require(struct.pack('<Q3d',*(surfaces[4][k] for k in ('count','incoming','outgoing','net_outward')))==
                            struct.pack('<Q3d',*(surfaces[other][k] for k in ('count','incoming','outgoing','net_outward'))),
                            'empty burner complement identical face values')
            for k in ('incoming','outgoing','net_outward'):
                full,left,right=(interval(surfaces[i][k]) for i in (4,6,7))
                require(max(full[0],left[0]+right[0])<=min(full[1],left[1]+right[1]),'same bottom partition reduction')
            net=math.fsum(s['net_outward'] for s in surfaces[:6])
            require(struct.pack('<d',row['net_outward'])==struct.pack('<d',net),'exact six-surface reported net')
            residual=math.fsum((row['candidate'],-row['beginning'],-row['frozen_source_increment'],net))
            require(struct.pack('<d',row['closure_residual'])==struct.pack('<d',residual),'exact observed closure, not forced zero')
            out.extend(struct.pack('<Q8d',row['cell_count'],*values))
            for s in surfaces:out.extend(struct.pack('<Q3d',s['count'],s['incoming'],s['outgoing'],s['net_outward']))
    for i in range(10):
        n=result['supports']['native'][i];c=result['supports']['common'][i]
        require(all(c[k]<=n[k] for k in VALUES[3:6]),'common absolute cell inventory subset')
        if layers==nz:
            require(struct.pack('<8d',*(n[k] for k in VALUES))==struct.pack('<8d',*(c[k] for k in VALUES)),
                    'identical cell-support value bits')
        for side in range(8):
            a=n['surfaces'][side];b=c['surfaces'][side]
            if side in (4,6,7) or layers==nz:
                require(struct.pack('<Q3d',a['count'],a['incoming'],a['outgoing'],a['net_outward'])==
                        struct.pack('<Q3d',b['count'],b['incoming'],b['outgoing'],b['net_outward']),
                        'identical face-support value bits')
            elif side<4:
                require(b['count']<=a['count'] and b['incoming']<=a['incoming'] and b['outgoing']<=a['outgoing'],
                        'common lateral count and nonnegative subset')
    for name in ('native','common'):
        rows=result['supports'][name]
        for signed,absolute in zip(VALUES[:3],VALUES[3:6]):
            species_consistent([rows[i][signed] for i in range(1,8)],
                               [rows[i][absolute] for i in range(1,8)],rows[9][signed],rows[9][absolute])
        for side in range(8):
            surfaces=[rows[i]['surfaces'][side] for i in range(1,8)]
            total=rows[9]['surfaces'][side]
            species_consistent([s['net_outward'] for s in surfaces],
                [Fraction(s['incoming'])+Fraction(s['outgoing']) for s in surfaces],total['net_outward'],
                Fraction(total['incoming'])+Fraction(total['outgoing']))
        require(all((r['surfaces'][6]['count'],r['surfaces'][7]['count'])==
                    (rows[0]['surfaces'][6]['count'],rows[0]['surfaces'][7]['count']) for r in rows),
                'same burner mask cardinality for every field')
    require(len(out)==PACKET_BYTES,'fixed scalar wire size');return bytes(out)


def decode(payload):
    require(type(payload) is bytes and len(payload)==PACKET_BYTES and payload[:8]==MAGIC,'immutable exact scalar bytes')
    layers=struct.unpack_from('<Q',payload,8)[0]
    t=capture('r248_target_terms.py',TARGET_SHA);p=t['packet_context']();l=p['accounting_context']()['layout_context']()
    names=list(l['layout']((2,2,1),1)['metadata_uint64_slots']);metadata=dict(zip(names,struct.unpack_from('<39Q',payload,16)))
    output={};cursor=HEADER_BYTES
    for name in ('native','common'):
        rows=[]
        for field,label in enumerate(FIELDS):
            count,*values=struct.unpack_from('<Q8d',payload,cursor);cursor+=72;surfaces=[]
            for side in range(8):
                count_s,a,b,net=struct.unpack_from('<Q3d',payload,cursor);cursor+=32
                surfaces.append(dict(count=count_s,incoming=a,outgoing=b,net_outward=net))
            rows.append(dict(field=label,unit='J' if field==8 else 'kg',cell_count=count,
                             **dict(zip(VALUES,values)),surfaces=surfaces))
        output[name]=rows
    result=dict(schema='rise.fire.r248.scalar-oracle.v1',supports=output,caller_values_authenticated=False,
                burner_configuration_authenticated=False,full_accepted_history_authenticated=False,
                native_reduction_replayed=False,physical_acceptance=False)
    require(encode(result,metadata,layers)==payload,'canonical scalar wire roundtrip');return result,metadata,layers


def packet_layout():
    return dict(schema='rise.fire.r248.scalar-layout.v1',packet_bytes=PACKET_BYTES,header_bytes=HEADER_BYTES,
                row_bytes=ROW_BYTES,rows=20,inventory_order=VALUES,surfaces=SURFACES,fields=FIELDS,
                reduction='binary64 math.fsum independent oracle; no native tree replay',
                native_reduction_replayed=False,full_accepted_history_authenticated=False,
                encoded_bytes_are_measured_allocation=False,new_simulation_launched=False)


def controls():
    import copy
    nx,ny,nz=6,8,3;h=2
    t=capture('r248_target_terms.py',TARGET_SHA);p=t['packet_context']();l=p['accounting_context']()['layout_context']()['layout']((nx,ny,nz),h)
    meta={k:i for i,k in enumerate(l['metadata_uint64_slots'],1)}
    meta.update(nx=nx,ny=ny,nz=nz,dx_binary32_bits=0x3f000000,dt_binary32_bits=0x3e800000,
                beginning_time_binary64_bits=0,end_time_binary64_bits=0x3fd0000000000000)
    q0=np.zeros((9,nz,ny,nx),np.float32);q1=q0.copy();source=q0.copy()
    q0[1]=8;q1[1]=12;source[1]=4;q0[1,2]=1024;q1[1,2]=2048
    low=np.zeros((9,l['native_faces']),np.float32);anti=low.copy();alpha=np.zeros(l['native_faces'],np.float32)
    mask=np.zeros((ny,nx),np.uint8);mask[0,0]=1
    zoff=(nx+1)*ny*nz+nx*(ny+1)*nz;upper=zoff+nx*ny*h
    low[1,upper:upper+2]=(2,-4);anti[1,upper:upper+2]=(4,4);alpha[upper:upper+2]=.5
    low[2,upper:upper+2]=(-4,2);low[1,zoff:zoff+2]=(8,-8)
    r=observe(q0,q1,source,low,anti,alpha,mask,meta,h);wire=encode(r,meta,h)
    require(decode(wire)==(r,meta,h),'scalar canonical roundtrip')
    ch=r['supports']['common'][1];total=r['supports']['common'][9]
    require((ch['beginning'],ch['candidate'],ch['frozen_source_increment'])==(96.,144.,48.),'common cell inventories')
    require(ch['surfaces'][5]==dict(count=48,incoming=.125,outgoing=.25,net_outward=.125),'upper signed transfer')
    require(total['surfaces'][5]['incoming']==total['surfaces'][5]['outgoing']==0,'species face counterflow before split')
    scalar=capture('r247_region_transfers.py',SCALAR_SHA)
    old=scalar['region_transfers'](q0,q1,source,low,anti,alpha,mask,.5,.25,h)
    require(all(ch[k]==old['fields'][1][k] for k in ('beginning','candidate','frozen_source_increment','net_outward','closure_residual')),'accepted scalar exact synthetic reference')
    for value in (0,1):
        rr=observe(q0,q1,source,low,anti,alpha,np.full_like(mask,value),meta,h)
        require(decode(encode(rr,meta,h))[0]==rr,'empty/full burner mask canonical GREEN')
    red=0
    def refuse(fn):
        nonlocal red
        try:fn()
        except ValueError:red+=1
        else:raise AssertionError('missing scalar refusal')
    for index,value in ((0,q0.astype(np.float64)),(1,q1[:,:2]),(3,low[:,:-1]),(5,alpha-1),(6,mask.astype(bool)),(6,mask+2)):
        args=[q0,q1,source,low,anti,alpha,mask,meta,h];args[index]=value;refuse(lambda args=args:observe(*args))
    refuse(lambda:decode(wire[:-1]));refuse(lambda:decode(wire+b'0'));refuse(lambda:decode(bytearray(wire)))
    for offset,fmt,value in ((0,'8s',b'BADMAGIC'),(328,'Q',0),(336,'d',float('nan')),(392,'d',1.),
                             (400,'Q',1),(408,'d',-1.),(416,'d',1e308)):
        b=bytearray(wire);struct.pack_into('<'+fmt,b,offset,value);refuse(lambda b=b:decode(bytes(b)))
    bad=copy.deepcopy(r);bad['physical_acceptance']=True;refuse(lambda:encode(bad,meta,h))
    bad=copy.deepcopy(r);bad['supports']['common'].reverse();refuse(lambda:encode(bad,meta,h))
    bad=copy.deepcopy(r);s=bad['supports']['common'][1]['surfaces'][6];s['count']+=1;refuse(lambda:encode(bad,meta,h))
    bad=copy.deepcopy(r)
    for name in ('native','common'):
        rr=bad['supports'][name][9];rr['beginning']=rr['candidate']=0.
        rr['closure_residual']=math.fsum((rr['candidate'],-rr['beginning'],-rr['frozen_source_increment'],rr['net_outward']))
    refuse(lambda:encode(bad,meta,h))
    zz=observe(q0*0,q1*0,source*0,low*0,anti*0,alpha*0,mask,meta,h)
    for name in ('native','common'):
        for side in (4,6):zz['supports'][name][0]['surfaces'][side].update(incoming=1.,outgoing=1.)
    refuse(lambda:encode(zz,meta,h))
    zz=observe(q0*0,q1*0,source*0,low*0,anti*0,alpha*0,mask,meta,h)
    for name in ('native','common'):
        rr=zz['supports'][name][9];rr['surfaces'][5].update(outgoing=1.,net_outward=1.)
        rr['net_outward']=rr['closure_residual']=1.
    refuse(lambda:encode(zz,meta,h))
    zz=observe(q0*0,q1*0,source*0,low*0,anti*0,alpha*0,mask,meta,h)
    zz['supports']['common'][0].update(beginning=1.,candidate=1.,beginning_absolute=1.,candidate_absolute=1.)
    refuse(lambda:encode(zz,meta,h))
    require(red==23,'scalar RED count')
    print(json.dumps(dict(status='pure_scalar_balance_arithmetic_wire_only',red=red,packet_bytes=len(wire),
                         native_fields_read=0,native_reduction_replayed=False,full_history_authenticated=False,
                         native_allocation_accepted=False,new_simulation_launched=False)))


if __name__=='__main__':controls()
