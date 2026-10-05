#!/usr/bin/env python3
"""Pure address/identity-slot prerequisite for the proposed r248 observer.

No payload reader, allocation, solver acceptance or new run is supplied here.
Ranges address native packed MAC faces; central support is deduplicated.
"""
import json

UINT32_MAX = 2**32-1
UINT64_MAX = 2**64-1
MAX_LAYERS = 160
HEADER_FIELDS = ('accepted_ordinal','beginning_time_binary64_bits','end_time_binary64_bits',
                 'dt_binary32_bits','nx','ny','nz','dx_binary32_bits')
STAGE_FIELDS = ('projection_publication','transport_publication','physical_flux_publication',
                'candidate_publication','eos_publication','frozen_source_publication',
                'issued_target_publication','consumed_projection_target_publication',
                'consumed_target_correction_iteration','issued_target_correction_iteration')


def require(condition, message):
    if not condition: raise ValueError(message)


def layout(dimensions, common_layers=80):
    require(type(dimensions) is tuple and len(dimensions)==3 and
            all(type(n) is int and 1<=n<=UINT32_MAX for n in dimensions),'typed positive domain')
    nx,ny,nz=dimensions
    require(nx>=2 and ny>=2 and nz<=MAX_LAYERS and nx%2==ny%2==0 and type(common_layers) is int and 1<=common_layers<=nz,
            'typed central/common support')
    cells=nx*ny*nz
    counts=((nx+1)*ny*nz,nx*(ny+1)*nz,nx*ny*(nz+1))
    offsets=(0,counts[0],counts[0]+counts[1]);faces=sum(counts)
    # Active Metal owner dispatch and packed counts use uint32; reject rather
    # than make a layout which cannot be represented by its actual owner.
    require(cells<=UINT32_MAX and faces<=UINT32_MAX,'native owner uint32 cell/face count')
    supports={}
    for name,height in (('native',nz),('common',common_layers)):
        ranges=[dict(axis=axis,begin=offsets[axis],end=offsets[axis]+count,count=count)
                for axis,count in enumerate(((nx+1)*ny*height,nx*(ny+1)*height,nx*ny*(height+1)))]
        lateral=[dict(axis=axis,side=side,count=(ny if axis==0 else nx)*height)
                 for axis in (0,1) for side in ('lower','upper')]
        horizontal=[dict(axis=2,z_face=z,count=nx*ny) for z in (0,height)]
        # Logical associations describe column observation rows, not physical
        # faces. Common z-face height is included; x/y layer height is excluded.
        groups=[[],[],[]]
        for z in range(height):
            for y in (ny//2-1,ny//2):
                for x in (nx//2-1,nx//2,nx//2+1):
                    groups[0].append(offsets[0]+x+(nx+1)*(y+ny*z))
            for y in (ny//2-1,ny//2,ny//2+1):
                for x in (nx//2-1,nx//2):
                    groups[1].append(offsets[1]+x+nx*(y+(ny+1)*z))
        for z in range(height+1):
            for y in (ny//2-1,ny//2):
                for x in (nx//2-1,nx//2):
                    groups[2].append(offsets[2]+x+nx*(y+ny*z))
        require(all(len(group)==len(set(group)) for group in groups),'deduplicated central faces')
        require(all(offsets[a]<=index<offsets[a]+counts[a] for a,g in enumerate(groups) for index in g),
                'central native packed address')
        supports[name]=dict(layers=height,cell_prefix_count=nx*ny*height,face_ranges=ranges,
                            central_face_indices=groups,central_unique_faces=sum(map(len,groups)),
                            central_column_associations=4*(5*height+1),
                            scalar_surfaces=lateral+horizontal,
                            upper_classification='native_upper_surface' if height==nz else 'interior_plane',
                            face_means_are_volume_integrals=False)
    slots={};cursor=0
    for name in HEADER_FIELDS:slots[name]=cursor;cursor+=1
    for stage in range(3):
        for name in STAGE_FIELDS:slots['R'+str(stage)+'.'+name]=cursor;cursor+=1
    slots['owner_publication']=cursor;cursor+=1
    # Slot count/bytes only: implementation must authenticate resident values,
    # append case/build/modern roots and issue a sealed publication.
    require(cursor*8<=UINT64_MAX,'metadata byte count')
    return dict(schema='rise.fire.r248.observer-support-layout.v1',dimensions=dimensions,
                cells=cells,native_face_counts=counts,native_face_offsets=offsets,
                native_faces=faces,supports=supports,metadata_uint64_slots=slots,
                metadata_bytes=cursor*8,authenticated_native_values_available=False,
                numeric_reduction_layout_complete=False,native_allocation_accepted=False,boundary_condition_authenticated=False)


def controls():
    result=layout((6,8,83),80);common=result['supports']['common']
    # Independent coordinate enumeration through logical four columns.
    nx,ny,nz=6,8,83;offsets=result['native_face_offsets'];counts=result['native_face_counts']
    sets=[set(),set(),set()]
    for y in (3,4):
        for x in (2,3):
            for z in range(80):
                for axis,xx,yy in ((0,x,y),(0,x+1,y),(1,x,y),(1,x,y+1)):
                    sets[axis].add(offsets[axis]+xx+(nx+(axis==0))*(yy+(ny+(axis==1))*z))
            for z in range(81):sets[2].add(offsets[2]+x+nx*(y+ny*z))
    require(all(set(common['central_face_indices'][a])==sets[a] for a in range(3)),
            'independent asymmetric column address oracle')
    require(common['central_unique_faces']==1284 and common['central_column_associations']==1604,
            'common unique/association count')
    require([r['count'] for r in common['face_ranges']]==[4480,4320,3888] and
            common['cell_prefix_count']==3840,'independent common full-region coverage')
    require(common['upper_classification']=='interior_plane' and
            result['supports']['native']['upper_classification']=='native_upper_surface','boundary/interior labels')
    require(all(r['end']<=offsets[a]+counts[a] for a,r in enumerate(common['face_ranges'])),'native range bounds')
    slots=result['metadata_uint64_slots']
    require(len(slots)==39 and len(set(slots.values()))==39 and result['metadata_bytes']==312 and
            slots['R0.issued_target_publication']!=slots['R0.consumed_projection_target_publication'],
            'explicit nonaliased consumed/issued target slots')
    small=layout((2,2,1),1)
    require(small['supports']['common']['central_unique_faces']==20 and
            small['supports']['common']['central_column_associations']==24,'minimum-domain shared faces')
    red=0
    for call in (lambda:layout([6,8,83]),lambda:layout((True,8,83)),lambda:layout((6.,8,83)),
                 lambda:layout((5,8,83)),lambda:layout((6,7,83)),lambda:layout((6,8,0)),
                 lambda:layout((6,8,83),True),lambda:layout((6,8,83),0),lambda:layout((6,8,83),84),
                 lambda:layout((6,8,83),80.),lambda:layout((2**32,8,83)),lambda:layout((2**16,2**16,80))):
        try:call()
        except ValueError:red+=1
        else:raise AssertionError('missing layout refusal')
    require(red==12,'typed/coverage/overflow RED count')
    print(json.dumps(dict(status='pure_support_layout_prerequisite_only',red=red,
                         common_unique_faces=1284,metadata_slots=39,native_fields_read=0,
                         new_simulation_launched=False)))


if __name__=='__main__': controls()
