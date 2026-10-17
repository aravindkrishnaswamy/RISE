#!/usr/bin/env python3
"""Exact dyadic spatial source/CH4/O2 oracle and proposed bounded row layout.

No native inputs, byte codec, resident reductions or physical qualification are
implemented. Exact rational reduction is a reference, not Metal/NumPy parity.
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
SOURCE_SHA='37b3d1488ac6e114c228a47cdcb5e2be31b658dd36169b3d6639439b61d8a28c'
QUANTITIES=('frozen_chemical_gas_plus_soot','CH4_density','O2_density')
TOTALS=('signed','absolute','positive','negative','central_positive')
MAX_LAYERS=160
HEADER_BYTES=336
SUMMARY_BYTES=144
LAYER_BYTES=72
FRACTION_BYTES=16
PROPOSED_BYTES=70368


def require(ok,message):
    if not ok:raise ValueError(message)


def capture(name,sha):
    p=ROOT/name;raw=p.read_bytes();require(hashlib.sha256(raw).hexdigest()==sha,'accepted spatial helper digest')
    c={'__name__':'captured_spatial_dependency'};exec(compile(raw,str(p),'exec'),c);return c


def header(metadata,layers,stage):
    require(type(stage) is int and stage in (0,1),'typed frozen R0/R1 stage')
    t=capture('r248_target_terms.py',TARGET_SHA);p=t['packet_context']();_,addresses,slots=p['header_layout'](metadata,layers)
    dx=struct.unpack('<f',struct.pack('<I',metadata['dx_binary32_bits']))[0]
    # Match the shared computed binary64 volume convention, then reduce exactly.
    volume=dx**3;require(math.isfinite(volume) and volume>0,'positive represented volume')
    return addresses,slots,Fraction.from_float(volume)


def observe(gas,soot,ch4,oxygen,metadata,layers=80,stage=0):
    addresses,slots,volume=header(metadata,layers,stage);nx,ny,nz=addresses['dimensions']
    for a in (gas,soot,ch4,oxygen):
        require(type(a) is np.ndarray and a.dtype==np.float32 and a.shape==(nz,ny,nx) and np.isfinite(a).all(),
                'finite caller binary32 cell fields')
    require(np.all(ch4>=0) and np.all(oxygen>=0),'nonnegative represented CH4/O2 densities')
    # Exact gas+soot combination of the represented f32 inputs, rather than
    # replaying a materialized native f32 or old f64 chemical field.
    fields={key:[] for key in QUANTITIES}
    for z in range(nz):
        for y in range(ny):
            for x in range(nx):
                g,s,c,o=(Fraction.from_float(float(a[z,y,x])) for a in (gas,soot,ch4,oxygen))
                for key,value in zip(QUANTITIES,(g+s,c,o)):
                    fields[key].append((x,y,z,value))

    def summarize(entries,geometry):
        positive=[(x,y,z,v) for x,y,z,v in entries if v>0]
        negative=[v for x,y,z,v in entries if v<0]
        p=sum(v for x,y,z,v in positive);n=sum(negative)
        central=[v for x,y,z,v in positive if x in (nx//2-1,nx//2) and y in (ny//2-1,ny//2)]
        centre=None;variance=None;fraction=None
        if geometry and p:
            centre=[];variance=[]
            for axis in range(3):
                mean=sum(v*(Fraction((x,y,z)[axis])+Fraction(1,2)) for x,y,z,v in positive)/p
                spread=sum(v*(Fraction((x,y,z)[axis])+Fraction(1,2)-mean)**2 for x,y,z,v in positive)/p
                centre.append(float(mean));variance.append(float(spread))
            fraction=float(sum(central)/p)
        totals=dict(zip(TOTALS,(float((p+n)*volume),float((p-n)*volume),float(p*volume),
                                float(n*volume),float(sum(central)*volume))))
        require(all(math.isfinite(v) for v in totals.values()),'finite spatial unit totals')
        return dict(cell_count=len(entries),available=True,positive_count=len(positive),negative_count=len(negative),
                    central_positive_count=len(central),totals=totals,positive_geometry_available=bool(p) if geometry else False,
                    centroid_cell_centre_xyz=centre,variance_cell_squared_xyz=variance,
                    central_positive_fraction=fraction),p

    output={};exact_positive={}
    for name,support in addresses['supports'].items():
        h=support['layers'];rows={};positive={}
        for key in QUANTITIES:
            entries=[v for v in fields[key] if v[2]<h];row,p=summarize(entries,True)
            row['unit']='W' if key==QUANTITIES[0] else 'kg'
            row['layers']=[summarize([v for v in entries if v[2]==z],False)[0] for z in range(h)]
            rows[key]=row;positive[key]=p
        output[name]=rows;exact_positive[name]=positive
    fractions={key:float(exact_positive['common'][key]/exact_positive['native'][key])
               if exact_positive['native'][key] else None for key in QUANTITIES}
    return dict(schema='rise.fire.r248.spatial-support-oracle.v1',stage=stage,supports=output,
                common_positive_fraction=fractions,coordinate_convention='integer native cell index+1/2; dx applied by consumer',
                chemical_combination='exact dyadic sum of supplied gas and soot f32 values',
                caller_values_authenticated=False,native_stage_authenticated=False,native_reduction_replayed=False,
                codec_implemented=False,physical_acceptance=False)


def proposed_layout():
    # This is a row reservation, NOT encoded bytes or a measured live allocation.
    require(HEADER_BYTES+3*FRACTION_BYTES+6*(SUMMARY_BYTES+MAX_LAYERS*LAYER_BYTES)==PROPOSED_BYTES,'bounded reservation arithmetic')
    return dict(schema='rise.fire.r248.spatial-support-layout.v1',header_bytes=HEADER_BYTES,
                metadata_uint64_slots=39,stage_uint64=True,quantity_order=QUANTITIES,support_order=('native','common'),
                summary_rows=6,summary_bytes=SUMMARY_BYTES,max_layers=MAX_LAYERS,layer_rows=960,layer_bytes=LAYER_BYTES,
                common_fraction_rows=3,common_fraction_bytes=FRACTION_BYTES,proposed_payload_bytes=PROPOSED_BYTES,
                summary_uint64_columns=('cell_count','available','positive_count','negative_count','central_positive_count'),
                summary_binary64_columns=TOTALS,
                geometry_available_uint64=True,geometry_binary64_columns=('centroid_x','centroid_y','centroid_z',
                    'variance_x','variance_y','variance_z','central_positive_fraction'),
                layer_uint64_columns=('available','positive_count','negative_count','central_positive_count'),
                layer_binary64_columns=TOTALS,unpopulated_layer_slots='unavailable; zero counts and canonical positive-zero numeric padding',
                absent_positive_geometry='unavailable; centroid/variance/fraction None in oracle, positive-zero reserved padding in future codec',
                common_fraction_availability='native positive count>0; common empty positive count has known fraction0',
                codec_implemented=False,native_allocation_accepted=False,measured_memory_bytes=None,
                authenticated_stage_lifetimes=False,new_simulation_launched=False)


def controls():
    dims=(4,4,3);nx,ny,nz=dims;h=2;t=capture('r248_target_terms.py',TARGET_SHA);p=t['packet_context']()
    l=p['accounting_context']()['layout_context']()['layout'](dims,h)
    meta={k:i for i,k in enumerate(l['metadata_uint64_slots'],1)}
    meta.update(nx=nx,ny=ny,nz=nz,dx_binary32_bits=0x3f000000,dt_binary32_bits=0x3e800000,
                beginning_time_binary64_bits=0,end_time_binary64_bits=0x3fd0000000000000)
    gas=np.zeros((nz,ny,nx),np.float32);soot=gas.copy();ch4=gas.copy();oxygen=np.ones_like(gas)
    gas[0,0,0]=1;soot[0,1,2]=3;gas[0,3,3]=-1;gas[2,0,0]=4;ch4[1,1,1]=2
    r=observe(gas,soot,ch4,oxygen,meta,h);a=r['supports']['common'][QUANTITIES[0]]
    require(a['totals']==dict(signed=.375,absolute=.625,positive=.5,negative=-.125,central_positive=.375),'signed power and central four total')
    require(a['centroid_cell_centre_xyz']==[2.,1.25,.5] and a['variance_cell_squared_xyz']==[.75,.1875,0.] and
            a['central_positive_fraction']==.75 and r['common_positive_fraction'][QUANTITIES[0]]==.5,'unequalweight/variance/nativecommon fraction')
    fuel=r['supports']['common'][QUANTITIES[1]]
    require(fuel['unit']=='kg' and fuel['totals']['positive']==.25 and fuel['centroid_cell_centre_xyz']==[1.5,1.5,1.5] and
            fuel['variance_cell_squared_xyz']==[0.,0.,0.],'density mass versus source power')
    oxy=r['supports']['common'][QUANTITIES[2]]
    require(oxy['totals']['positive']==4 and oxy['variance_cell_squared_xyz']==[1.25,1.25,.25] and
            r['common_positive_fraction'][QUANTITIES[2]]==float(Fraction(2,3)),'uniformoxygen exact population variance')
    old=capture('r247_source_moments.py',SOURCE_SHA)['source_moments'](gas.astype(np.float64),soot.astype(np.float64),.5,h)
    require(a['centroid_cell_centre_xyz']==old['common']['centroid_cell_centre_xyz'] and
            a['variance_cell_squared_xyz']==old['common']['variance_cell_squared_xyz'],'exact synthetic accepted spatial reference')
    for source in (np.zeros_like(gas),-np.ones_like(gas)):
        q=observe(source,gas*0,ch4*0,oxygen*0,meta,h)
        for support in ('native','common'):
            for row in q['supports'][support].values():
                require(row['centroid_cell_centre_xyz'] is None and row['variance_cell_squared_xyz'] is None and
                        row['central_positive_fraction'] is None and not row['positive_geometry_available'],'empty positive geometry unavailable')
        require(all(v is None for v in q['common_positive_fraction'].values()),'empty native fraction unavailable')
    tiny=np.zeros_like(gas);tiny[0,0,0]=np.finfo(np.float32).max;tiny[0,3,3]=np.nextafter(np.float32(0),np.float32(1))
    small=observe(tiny,gas*0,ch4*0,oxygen*0,meta,h)['supports']['common'][QUANTITIES[0]]
    require(small['positive_count']==2 and small['variance_cell_squared_xyz'][0]>0 and
            small['variance_cell_squared_xyz'][1]>0,'extreme weight spread retained without subtractive cancellation')
    minimum=dict(meta,dx_binary32_bits=1)
    minimum_row=observe(gas,soot,ch4,oxygen,minimum,h)['supports']['common'][QUANTITIES[0]]
    require(minimum_row['totals']['positive']>0 and minimum_row['central_positive_fraction']==.75,'subnormal f32 dx/volume independent fraction')
    red=0
    def refuse(fn):
        nonlocal red
        try:fn()
        except ValueError:red+=1
        else:raise AssertionError('missing spatial refusal')
    for index,value in ((0,gas.astype(np.float64)),(1,soot[:2]),(2,-np.ones_like(ch4)),(3,-np.ones_like(oxygen)),
                        (0,np.full_like(gas,np.nan)),(3,np.full_like(oxygen,np.inf))):
        args=[gas,soot,ch4,oxygen,meta,h,0];args[index]=value;refuse(lambda args=args:observe(*args))
    for layers in (True,0,4):refuse(lambda layers=layers:observe(gas,soot,ch4,oxygen,meta,layers))
    for stage in (True,2,-1):refuse(lambda stage=stage:observe(gas,soot,ch4,oxygen,meta,h,stage))
    bad=dict(meta,dx_binary32_bits=0);refuse(lambda:observe(gas,soot,ch4,oxygen,bad,h))
    require(red==13,'spatial type/shape/finite/density/stage/measure RED count')
    require(proposed_layout()['proposed_payload_bytes']==70368,'proposed row reservation counts')
    print(json.dumps(dict(status='pure_exact_spatial_arithmetic_and_row_reservation_only',red=red,
                         proposed_payload_bytes=PROPOSED_BYTES,codec_implemented=False,native_fields_read=0,
                         native_allocation_accepted=False,new_simulation_launched=False)))


if __name__=='__main__':controls()
