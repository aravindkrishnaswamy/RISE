#!/usr/bin/env python3
"""Pure endpoint/branch accounting for a proposed r248 resident observer.

Caller operands are not authenticated here. This is not a pressure replay,
EOS response decomposition, native observer or simulation launch.
"""
import hashlib
import json
import math
from pathlib import Path
import numpy as np

LAYOUT_PATH = Path('/Users/aravind/Working/RISE/worktrees/r243-structure-diagnostic/tools/fire/r248_observer_layout.py')
LAYOUT_SHA = '75b93ef64b8bfd1c932972176f634f51f2e1bf94007811c8fd41e4290dd28a72'
BRANCHES = ('pressure_gradient','prescribed_burner','wall_reset')
QUANTITIES = ('pressure_endpoint_rate','burner_endpoint_rate','wall_endpoint_rate',
              'periodic_copy_endpoint_rate','total_endpoint_rate','observed_closure_residual')


def require(condition,message):
    if not condition:raise ValueError(message)


def layout_context():
    raw=LAYOUT_PATH.read_bytes();require(hashlib.sha256(raw).hexdigest()==LAYOUT_SHA,'accepted address helper hash')
    context={'__name__':'captured_r248_layout'};exec(compile(raw,str(LAYOUT_PATH),'exec'),context)
    return context


def projection_accounting(dimensions,dt,before,corrected,terminal,branch,gradient,prescribed,seam_source,layers=80):
    addresses=layout_context()['layout'](dimensions,layers);faces=addresses['native_faces']
    require(type(dt) is float and math.isfinite(dt) and 0<dt<=float(np.finfo(np.float32).max) and
            float(np.float32(dt))==dt,'exact positive promoted binary32 dt')
    require(all(type(a) is np.ndarray and a.dtype==np.float32 and a.shape==(faces,)
                for a in (before,corrected,terminal,gradient,prescribed)),'binary32 endpoint/branch operand layout')
    require(type(branch) is np.ndarray and branch.dtype==np.uint8 and branch.shape==(faces,) and
            np.all(branch<=2),'exact branch codes')
    require(type(seam_source) is np.ndarray and seam_source.dtype==np.int64 and seam_source.shape==(faces,),
            'typed periodic-copy source addresses')
    require(np.isfinite(before).all() and np.isfinite(corrected).all() and np.isfinite(terminal).all(),'finite endpoints')
    # NaN is explicit absence, not zero padding. No gradient is measured on an
    # early-return burner/wall branch and no prescribed value on other branches.
    for i in range(faces):
        code=int(branch[i]);source=int(seam_source[i])
        require(math.isfinite(float(gradient[i])) if code==0 else math.isnan(float(gradient[i])),
                'gradient presence follows actual branch')
        require(math.isfinite(float(prescribed[i])) if code==1 else math.isnan(float(prescribed[i])),
                'burner value presence follows actual branch')
        if code==1:require(corrected[i].view(np.uint32)==prescribed[i].view(np.uint32),'prescribed burner endpoint exact bits')
        if code==2:require(corrected[i].view(np.uint32)==0,'wall endpoint canonical positive zero')
        require(source==-1 or 0<=source<faces,'valid periodic source')
        expected=corrected[i] if source==-1 else corrected[source]
        require(terminal[i].view(np.uint32)==expected.view(np.uint32),'post-copy endpoint exact bits')
        if source!=-1:
            # copy_periodic_seam maps the HIGH native surface to corresponding
            # LOW surface on that same axis. Its low source is never overwritten.
            valid=False
            nx,ny,nz=dimensions
            for axis,(offset,count) in enumerate(zip(addresses['native_face_offsets'],addresses['native_face_counts'])):
                if offset<=i<offset+count:
                    local=i-offset;xx=nx+(axis==0);yy=ny+(axis==1)
                    z,remainder=divmod(local,xx*yy);y,x=divmod(remainder,xx)
                    coordinate=(x,y,z)[axis];extent=(nx,ny,nz)[axis]
                    low=(x if axis!=0 else 0)+xx*((y if axis!=1 else 0)+yy*(z if axis!=2 else 0))
                    valid=coordinate==extent and source==offset+low and seam_source[source]==-1
            require(valid,'exact high-to-low native seam copy')
    outputs={}
    for support_name,support in addresses['supports'].items():
        axes=[]
        for axis,span in enumerate(support['face_ranges']):
            selected=range(span['begin'],span['end'])
            for kind,indices in (('whole_region',selected),('central',support['central_face_indices'][axis])):
                values={key:[] for key in QUANTITIES};gradients=[];counts=[0,0,0];seams=0
                for i in indices:
                    code=int(branch[i]);counts[code]+=1
                    delta=(float(corrected[i])-float(before[i]))/dt
                    copied=(float(terminal[i])-float(corrected[i]))/dt
                    total=(float(terminal[i])-float(before[i]))/dt
                    terms=[delta if code==c else 0. for c in range(3)]
                    row=terms+[copied,total,total-math.fsum(terms+[copied])]
                    require(all(math.isfinite(x) for x in row),'finite endpoint accounting')
                    for key,value in zip(QUANTITIES,row):values[key].append(value)
                    if code==0:gradients.append(float(gradient[i]))
                    if seam_source[i]!=-1:seams+=1
                def summary(v):
                    return None if not v else dict(count=len(v),signed_mean=math.fsum(v)/len(v),absolute_mean=math.fsum(abs(x) for x in v)/len(v))
                axes.append(dict(axis=axis,support=kind,unique_faces=len(indices),branch_counts=dict(zip(BRANCHES,counts)),
                                 periodic_copy_faces=seams,endpoint_rates={key:summary(v) for key,v in values.items()},
                                 pressure_gradient_on_executed_branch=summary(gradients),
                                 gradient_is_not_endpoint_rounding_residual=True,unit='kg/m2/s2'))
        outputs[support_name]=axes
    # Oracle temporaries scale with selected faces. Bounded numeric wire layout
    # and resident reduction implementation are separate pending prerequisites.
    return dict(schema='rise.fire.r248.projection-accounting-oracle.v1',supports=outputs,
                support_layout_sha256=LAYOUT_SHA,dt_s=dt,caller_values_authenticated=False,
                boundary_configuration_authenticated=False,
                pressure_gradient_arithmetic_replayed=False,EOS_restoration_impulse_available=False,
                native_reduction_packet_accepted=False)


def controls():
    dims=(2,2,2);ctx=layout_context()['layout'](dims,1);faces=ctx['native_faces']
    before=np.arange(faces,dtype=np.float32);corrected=before+np.float32(1);terminal=corrected.copy()
    branch=np.zeros(faces,dtype=np.uint8);gradient=np.full(faces,-2,dtype=np.float32)
    prescribed=np.full(faces,np.nan,dtype=np.float32);seams=np.full(faces,-1,dtype=np.int64)
    branch[0]=1;prescribed[0]=5;corrected[0]=terminal[0]=5;gradient[0]=np.nan
    branch[1]=2;corrected[1]=terminal[1]=0;gradient[1]=np.nan
    # x-face high x=2 copies corresponding low x=0 after correct_faces.
    seams[2]=0;terminal[2]=corrected[0]
    result=projection_accounting(dims,.5,before,corrected,terminal,branch,gradient,prescribed,seams,1)
    x=result['supports']['common'][0]
    require(x['unique_faces']==6 and x['branch_counts']==dict(pressure_gradient=4,prescribed_burner=1,wall_reset=1),
            'independent branch support counts')
    require(x['endpoint_rates']['pressure_endpoint_rate']['signed_mean']==8/6 and
            x['endpoint_rates']['burner_endpoint_rate']['signed_mean']==10/6 and
            x['endpoint_rates']['wall_endpoint_rate']['signed_mean']==-2/6 and
            x['endpoint_rates']['periodic_copy_endpoint_rate']['signed_mean']==4/6 and
            x['endpoint_rates']['total_endpoint_rate']['signed_mean']==20/6,'independent scalar endpoint oracle')
    # Asymmetric all-axis seam fixture; coordinate enumeration is independent
    # of the accepted helper's packed support ranges.
    dims2=(6,8,3);nx,ny,nz=dims2
    shapes=((nx+1,ny,nz),(nx,ny+1,nz),(nx,ny,nz+1))
    count=sum(math.prod(shape) for shape in shapes)
    b=np.arange(count,dtype=np.float32);c=b+np.float32(1);t=c.copy()
    codes=np.zeros(count,dtype=np.uint8);g=np.full(count,-2,dtype=np.float32)
    prescribed2=np.full(count,np.nan,dtype=np.float32);copies=np.full(count,-1,dtype=np.int64)
    independent=[];offset=0
    for axis,(sx,sy,sz) in enumerate(shapes):
        region=[]
        for z in range(sz):
            for y in range(sy):
                for xx in range(sx):
                    i=offset+xx+sx*(y+sy*z)
                    if (xx,y,z)[axis]==dims2[axis]:
                        low=[xx,y,z];low[axis]=0
                        j=offset+low[0]+sx*(low[1]+sy*low[2])
                        copies[i]=j;t[i]=c[j]
                    if z < (3 if axis==2 else 2):region.append(i)
        independent.append(region);offset+=sx*sy*sz
    asym=projection_accounting(dims2,.5,b,c,t,codes,g,prescribed2,copies,2)
    for axis,indices in enumerate(independent):
        row=asym['supports']['common'][2*axis]
        rates=[2*(float(t[i])-float(b[i])) for i in indices]
        require(row['unique_faces']==len(indices) and
                row['endpoint_rates']['total_endpoint_rate']['signed_mean']==math.fsum(rates)/len(rates) and
                row['endpoint_rates']['total_endpoint_rate']['absolute_mean']==math.fsum(abs(v) for v in rates)/len(rates),
                'independent asymmetric all-axis endpoint/seam oracle')
    # Explicitly empty measured-gradient support is unavailable, never a zero.
    wall_codes=np.full(faces,2,dtype=np.uint8);zeros=np.zeros(faces,dtype=np.float32)
    absent=np.full(faces,np.nan,dtype=np.float32)
    wall=projection_accounting((2,2,2),.5,before,zeros,zeros,wall_codes,absent,absent,np.full(faces,-1,dtype=np.int64),1)
    require(all(row['pressure_gradient_on_executed_branch'] is None for rows in wall['supports'].values() for row in rows),
            'absent pressure gradient has no numeric summary')
    red=0
    def refuse(*args):
        nonlocal red
        try:projection_accounting(*args)
        except ValueError:red+=1
        else:raise AssertionError('missing accounting refusal')
    operands=[dims,.5,before,corrected,terminal,branch,gradient,prescribed,seams,1]
    for index,value in [(1,True),(1,.1),(2,before.astype(np.float64)),(5,branch.astype(np.int64)),(8,seams.astype(np.int32))]:
        bad=list(operands);bad[index]=value;refuse(*bad)
    for index,pos,value in [(5,0,3),(6,0,0),(6,3,np.nan),(7,3,0),(7,0,np.nan),
                            (3,0,4),(3,1,-0.),(4,2,4),(8,2,1),(8,0,2)]:
        bad=list(operands);bad[index]=bad[index].copy();bad[index][pos]=value;refuse(*bad)
    require(red==15,'projection branch/operand/seam RED count')
    print(json.dumps(dict(status='pure_projection_accounting_prerequisite_only',red=red,native_fields_read=0,
                         authenticated_native_values=False,full_observer_accepted=False,new_simulation_launched=False)))


if __name__=='__main__':controls()
