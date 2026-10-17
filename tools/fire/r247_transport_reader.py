#!/usr/bin/env python3
"""Strict selected-interval original-region transport reader; no simulation.

Full admission is the exact accepted235-wide/235-taller metadata publication.
Synthetic controls are separate and cannot enable an empirical fixture mode.
"""
import argparse
import csv
import hashlib
import io
import json
import math
import mmap
import os
from pathlib import Path
import resource
import stat
import struct
import subprocess
import sys
import tempfile
import types
import traceback
import numpy as np

ROOT = Path('/Users/aravind/Working/RISE/worktrees/r243-structure-diagnostic')
EXT = Path('/Volumes/SSD/RISE/fire_production_calibration')
METADATA = EXT/'r247_source_metadata_validation.v3/metadata/source_metadata.json'
METADATA_SHA = 'e0114ff6c0371d321294ef8775f698baca67c9033874dc276b04d765ec842624'
METADATA_SEAL_SHA = '99bd8dc82544d31b7c68c8e677dbcb497071b4ca0046fe5c119231e1e307e6ce'
DEPENDENCIES = {
 'primitives': ('tools/fire/r247_source_reader.py','4fb8e13f371a7828a034f94892cb84c0de00c852ce6851a6b522d69b59a62215'),
 'admission': ('tools/fire/r247_source_admission.py','cb25a9a96afb2939846ad4e2b55204c45062c3e5c897eb142df7a8e7fe250545'),
 'scalar': ('tools/fire/r247_region_transfers.py','4e814f7cb55739cd39d645e2136072c0e2e2bc6b56599e2f19536de3072ed754'),
 'momentum': ('tools/fire/r247_central_momentum.py','dcb3b42b3317b9b3c61d87e8107c32135759298f068b071d12273076e79f87a3'),
 'verifier': ('tools/fire_payload_merkle.py','a6219a285792b671d6a33de41c73556d9c970f8aed32c034cacaa0c3ea405ebf')}
READER_CEILING = 2*1024**3
PUBLIC_FIXTURE_CEILING = 2*1024**3
OUTPUT_LIMIT = 64*1024**2
ROW_LIMIT = 1024**2
DX = .040790822356939316


def require(condition, message):
    if not condition: raise ValueError(message)


def sha(raw): return hashlib.sha256(raw).hexdigest()


def implementation():
    contexts = {}; buffers = {}
    # Bootstrap only the already pinned lifecycle primitive; all consumer calls
    # below use functions compiled from these captured bytes.
    path, digest = DEPENDENCIES['primitives']; raw = (ROOT/path).read_bytes()
    require(sha(raw) == digest, 'pinned lifecycle implementation')
    primitive = {'__name__':'captured_primitives','__file__':str(ROOT/path)}
    exec(compile(raw,str(ROOT/path),'exec'),primitive)
    for name,(relative,digest) in DEPENDENCIES.items():
        path = ROOT/relative; buffers[name] = primitive['capture'](path,digest)
        contexts[name] = {'__name__':'captured_'+name,'__file__':str(path)}
        exec(compile(buffers[name],str(path),'exec'),contexts[name])
    own_raw = primitive['capture'](Path(__file__),sha(Path(__file__).read_bytes()))
    own = {'__name__':'captured_transport','__file__':__file__}
    exec(compile(own_raw,__file__,'exec'),own)
    key = contexts['admission']['code_key']
    for name,function in list(globals().items()):
        if isinstance(function,types.FunctionType) and function.__module__ == __name__:
            require(key(function.__code__) == key(own[name].__code__) and
                    function.__defaults__ == own[name].__defaults__ and
                    function.__kwdefaults__ == own[name].__kwdefaults__, 'loaded consumer implementation changed')
    require(all(globals()[k] == own[k] for k in ('ROOT','EXT','METADATA','METADATA_SHA','METADATA_SEAL_SHA',
            'DEPENDENCIES','READER_CEILING','PUBLIC_FIXTURE_CEILING','OUTPUT_LIMIT','ROW_LIMIT','DX')),
            'loaded consumer constants changed')
    buffers['consumer'] = own_raw
    return contexts,{k:sha(v) for k,v in buffers.items()}


def layout(nx,ny,nz):
    require(all(type(v) is int and v >= 2 for v in (nx,ny,nz)), 'typed layout')
    cells=nx*ny*nz; faces=(nx+1)*ny*nz+nx*(ny+1)*nz+nx*ny*(nz+1)
    return cells,faces,dict(beginning=(28*cells+faces)*4+nx*ny,native=(19*cells+9*faces)*4+216,flux=72+26*faces*4)


def rss():
    require(sys.platform == 'darwin','macOS RSS units')
    current = subprocess.check_output(['ps','-o','rss=','-p',str(os.getpid())],text=True).strip()
    require(current.isdecimal() and int(current) > 0,'actual current RSS')
    return int(current)*1024,resource.getrusage(resource.RUSAGE_SELF).ru_maxrss


def allocation_plan(nx,ny,nz,current_rss):
    require(type(current_rss) is int and current_rss > 0,'typed current RSS')
    cells,faces,sizes=layout(nx,ny,nz); largest=max(sizes.values())
    require(largest <= PUBLIC_FIXTURE_CEILING,'public per-packet cap')
    leaves=(largest+4095)//4096
    components=dict(all_three_captured_packets=sum(sizes.values()),
                    bounded_packet_copy_scratch=65536,private_mapping_and_io_overhead=65536, two_inventory_conversions=2*cells*8,
                    plane_arithmetic_and_sign_buffers=8*max(nx*ny,nx*nz,ny*nz)*8,
                    sequential_finite_boolean_scratch=max(cells,faces),
                    merkle_nodes_and_lists=(math.ceil(leaves*16/15)+16)*128,merkle_chunk_concat=8192,
                    bounded_metadata_columns_and_python_objects=64*1024**2,
                    bounded_central_faces_and_rows=8*1024**2,
                    final_output_capture=OUTPUT_LIMIT,encoded_row_and_json_buffers=2*ROW_LIMIT)
    estimate=current_rss+sum(components.values())
    require(estimate <= READER_CEILING,'before-field Stage B allocation refusal')
    return dict(current_rss_before_fields_bytes=current_rss,packet_sizes=sizes,
                direct_buffer_components_bytes=components,enumerated_peak_estimate_bytes=estimate,
                estimate_is_not_measured_peak=True,reader_ceiling_bytes=READER_CEILING,
                stage_a_reader_ceiling_bytes=1024**3,stage_a_ceiling_unchanged=True,
                public_per_packet_cap_bytes=PUBLIC_FIXTURE_CEILING,
                source_host_ceiling_bytes=34359738368,source_next_step_reserve_bytes=23930269536,
                source_reserve_is_not_reader_allocation=True)


def packet_certificate(raw,expected,size,case,root,verifier):
    cert=json.loads(raw);v2=cert['v2']
    require(cert['schema']=='rise.fire.published-payload.v2' and cert['case_record_id']==case and
            cert['sha256_v1']==expected and v2['digest_format']==verifier['FORMAT'] and
            all(type(v2[k]) is int for k in ('digest_version','chunk_bytes','fan_in','payload_bytes')) and
            [v2[k] for k in ('digest_version','chunk_bytes','fan_in','payload_bytes')]==[2,4096,16,size] and
            type(v2['root_sha256']) is str and len(v2['root_sha256'])==64 and
            all(c in '0123456789abcdef' for c in v2['root_sha256']) and
            (root is None or v2['root_sha256']==root),'exact source packet certificate declaration')
    return cert


def certificate_capture(path,primitives,admission,expected=None):
    before=primitives['state'](path)
    raw=admission['capture'](path,expected)
    require(primitives['state'](path)==before,'certificate actual lifecycle')
    return raw


def mapped_packet(path, expected, size, verifier, cert, primitives, callback=None):
    """Copy into an unnamed private file, then retain only its read-only mapping.

    The mapping owns captured bytes, never mutable source-file pages. Closing
    it releases packet pages independently of the Python heap allocator.
    """
    before=primitives['state'](path,size)
    captured=None
    try:
        with tempfile.TemporaryFile(mode='w+b') as spool, Path(path).open('rb',buffering=0) as source:
            st=os.fstat(source.fileno())
            require((st.st_dev,st.st_ino)==before[:2],'mapped packet capture substitution')
            scratch=bytearray(65536);remaining=size
            while remaining:
                view=memoryview(scratch)[:min(remaining,len(scratch))]
                count=source.readinto(view)
                require(type(count) is int and count>0,'complete mapped packet capture')
                require(spool.write(view[:count])==count,'complete private packet write')
                remaining-=count
                view.release()
            require(source.read(1)==b'','mapped packet exact length')
            spool.flush()
            captured=mmap.mmap(spool.fileno(),size,access=mmap.ACCESS_READ)
        if callback:callback()
        require(primitives['state'](path,size)==before and len(captured)==size and sha(captured)==expected,
                'mapped packet captured bytes/lifecycle')
        require(cert['v2']['digest_format']==verifier['FORMAT'] and verifier['verify'](captured,cert['v2']),
                'same mapped buffer modern packet seal')
        return captured,before
    except BaseException:
        if captured is not None:captured.close()
        raise


def source_frame(case,item,contexts):
    p=contexts['primitives'];a=contexts['admission'];v=contexts['verifier']
    captured={}
    for name in ('binding','ledger'):
        rec=item[name];captured[name],_=p['pinned_sealed'](Path(rec['path']),rec['sha256'],rec['seal_sha256'],case['case'],a,v)
    binding=a['pairs'](captured['binding'].decode().splitlines())
    metadata=a['pairs'](line[2:] for line in captured['ledger'].decode().splitlines() if line.startswith('# '))
    require(binding['schema']=='rise.fire.accepted-stage-frame.v1' and binding['step_record_sha256']==sha(captured['ledger']) and
            binding['frame']==str(item['frame']) and binding['accepted_step']==metadata['accepted_step']==str(item['accepted_step']) and
            metadata['case_record_id']==case['case'] and metadata['producer_build_id']==case['producer'] and
            metadata['dimensions']==' '.join(map(str,case['dimensions'])) and
            metadata['binary32_solver_dx_m']==str(DX) and
            float(metadata['beginning_time_s'])==item['beginning_time_s'] and float(metadata['dt_s'])==item['dt_s'] and
            item['beginning_time_s']+item['dt_s']==item['accepted_event_s'] and
            all(metadata[k]==item[k] for k in ('input_root','native_root','flux_root')) and
            binding['.beginning.bin']==item['beginning_sha256'],'exact retained event/source linkage')
    nx,ny,nz=case['dimensions'];_,_,sizes=layout(nx,ny,nz)
    base=Path(item['binding']['path']).with_suffix('');packets={};receipts={};lifecycles=[];admitted=[]
    for name in ('beginning','native','flux'):
        suffix='.'+name+'.bin';path=Path(str(base)+suffix);cert_path=Path(str(path)+'.payload-v2.json')
        expected=binding[suffix];require(len(expected)==64 and all(c in '0123456789abcdef' for c in expected),'packet SHA encoding')
        cert_raw=certificate_capture(cert_path,p,a,item['beginning_seal_sha256'] if name=='beginning' else None)
        cert=packet_certificate(cert_raw,expected,sizes[name],case['case'],None if name=='beginning' else item[name+'_root'],v)
        if name=='beginning': require(cert==item['beginning_seal'],'accepted beginning certificate pin')
        before=p['state'](path,sizes[name])
        admitted.append((name,path,cert_path,expected,cert,cert_raw,before))
    try:
        # All three regular sizes and modern declarations precede any packet read.
        for name,path,cert_path,expected,cert,cert_raw,initial in admitted:
            packets[name],before=mapped_packet(path,expected,sizes[name],v,cert,p)
            require(before==initial and p['capture'](cert_path,sha(cert_raw))==cert_raw,'same preflight source pair')
            receipts[name]=dict(path=str(path),sha256=sha(packets[name]),seal_sha256=sha(cert_raw),merkle_root=cert['v2']['root_sha256'])
            lifecycles.append((path,sizes[name],before,cert_path,cert_raw))
        columns_path=Path(str(base)+'.columns.csv')
        column_cert=certificate_capture(Path(str(columns_path)+'.payload-v2.json'),p,a)
        columns,colrec=p['pinned_sealed'](columns_path,binding['columns_sha256'],sha(column_cert),case['case'],a,v)
        require(len(columns)<=4*1024**2,'bounded central column bytes')
        captured['columns']=columns;receipts['columns']=colrec
        return packets,captured,metadata,receipts,lifecycles
    except BaseException:
        for packet in packets.values():packet.close()
        raise


def unpack(packets,dimensions,metadata):
    nx,ny,nz=dimensions;cells,faces,sizes=layout(nx,ny,nz)
    require(all(len(packets[k])==size for k,size in sizes.items()),'exact three packet layout')
    flux=packets['flux'];native=packets['native'];beginning=packets['beginning']
    require(flux[:32].hex()==metadata['input_root']==native[-32:].hex() and
            flux[32:64].hex()==metadata['native_root'] and
            struct.unpack_from('<Q',flux,64)[0]==int(metadata['R1_candidate']),'actual packet input/native/candidate')
    ids=struct.unpack_from('<22Q',native,(19*cells+9*faces)*4+8)
    require(ids[10]==int(metadata['R1_candidate']) and ids[21]!=0,'native R1/owner authority')
    first=np.frombuffer(beginning,'<f4',count=18*cells+faces)
    raw=np.frombuffer(native,'<f4',count=19*cells+9*faces)
    flux_fields=np.frombuffer(flux,'<f4',offset=72).reshape(26,faces)
    return dict(q0=first[:9*cells].reshape(9,nz,ny,nx),source=first[9*cells:18*cells].reshape(9,nz,ny,nx),
                q1=raw[:9*cells].reshape(9,nz,ny,nx),low=flux_fields[:9],anti=flux_fields[9:18],
                alpha=raw[9*cells+8*faces:9*cells+9*faces],burner=np.frombuffer(beginning,'u1',offset=(28*cells+faces)*4).reshape(ny,nx),
                beginning=first[18*cells:],provisional=raw[9*cells+3*faces:9*cells+4*faces],terminal=raw[9*cells:9*cells+faces],
                rates=raw[9*cells+4*faces:9*cells+8*faces].reshape(4,faces),stage_rates=flux_fields[18:].reshape(4,2,faces),
                eddy=raw[12*cells+9*faces:13*cells+9*faces])


def column_check(raw,arrays,dimensions,dt,beginning_time):
    nx,ny,nz=dimensions;cells,faces,_=layout(nx,ny,nz)
    counts=((nx+1)*ny*nz,nx*(ny+1)*nz,nx*ny*(nz+1));offsets=(0,counts[0],counts[0]+counts[1])
    rows=list(csv.DictReader(io.StringIO(raw.decode())))
    expected=set()
    for y in (ny//2-1,ny//2):
        for x in (nx//2-1,nx//2):
            for z in range(nz):
                for axis,xx,yy in ((0,x,y),(0,x+1,y),(1,x,y),(1,x,y+1)):expected.add((x,y,axis,xx,yy,z))
            for z in range(nz+1):expected.add((x,y,2,x,y,z))
    seen=set()
    for row in rows:
        require(all(math.isfinite(float(value)) for value in row.values()),'finite full central column record')
        def integer(key):
            value=int(row[key]);require(row[key]==str(value),'canonical column integer');return value
        association=tuple(integer(k) for k in ('column_x','column_y','axis','x_face','y_face','z_face'))
        require(association in expected and association not in seen,'exact nonduplicate association');seen.add(association)
        _,_,axis,x,y,z=association;face=integer('face')
        # PrepareR245Budget writes a local single-observation index0 at every
        # retained frame. Global ordinal/event is bound by frame/ledger pins.
        require(integer('observation_index')==0,'exact producer local column observation index')
        require(face==x+(nx+(axis==0))*(y+(ny+(axis==1))*z) and 0<=face<counts[axis],'column face address')
        index=offsets[axis]+face
        for key,array in [('beginning',arrays['beginning']),('provisional',arrays['provisional']),('terminal',arrays['terminal'])]:
            require(float(row[key+'_momentum_kg_per_m2_s'])==float(array[index]),'column momentum actual source')
        for term,key in enumerate(('advection','buoyancy','stress','source')):
            require(float(row[key+'_rate_kg_per_m2_s2'])==float(arrays['rates'][term,index]),'column rate actual source')
        m0,m1,m2=(float(arrays[key][index]) for key in ('beginning','provisional','terminal'))
        combined=(m2-m1)/dt;total=(m2-m0)/dt
        r=[float(arrays['rates'][t,index]) for t in range(4)];closure=total-(r[2]+r[1]+r[0]+r[3]+combined)
        require(float(row['combined_projection_rate_kg_per_m2_s2'])==combined and float(row['total_rate_kg_per_m2_s2'])==total and
                float(row['closure_residual_kg_per_m2_s2'])==closure and float(row['dt_s'])==dt and
                float(row['beginning_time_s'])==beginning_time,'exact column stage/clock closure')
        position=[x,y,z]
        for side in ('lower','upper'):
            point=position.copy();point[axis]=max(0,point[axis]-1) if side=='lower' else min(point[axis],(nx,ny,nz)[axis]-1)
            cell=point[0]+nx*(point[1]+ny*point[2])
            require(integer(side+'_cell')==cell and float(row[side+'_vreman_m2_per_s'])==float(arrays['eddy'][cell]),'column cell/eddy source')
    require(seen==expected and len(rows)==4*(5*nz+1),'full native column coverage')
    return dict(column_associations=len(rows),actual_operand_checks_per_association=8,columns_sha256=sha(raw))


def enclosed(actual,expected,magnitude,operations,label):
    u=2.**-53;require(operations*u<1 and all(math.isfinite(v) for v in (actual,expected,magnitude)),'bounded comparison')
    bound=operations*u/(1-operations*u)*(magnitude+abs(actual)+abs(expected))
    require(abs(actual-expected)<=bound,'wide selected native '+label+' roundoff enclosure')
    return dict(label=label,residual=actual-expected,roundoff_bound=bound)


def wide_ledger_control(result,arrays,ledger,dimensions):
    nx,ny,nz=dimensions;cells,faces,_=layout(nx,ny,nz)
    rows=list(csv.DictReader(io.StringIO('\n'.join(line for line in ledger.decode().splitlines() if not line.startswith('#')))))
    require(len(rows)==30 and [(r['scope'],r['field']) for r in rows]==
            [(scope,str(field)) for scope in ('step','full','statistics') for field in range(10)],'wide complete ledger coverage')
    rows=rows[:10];checks=[]
    for field,(row,derived) in enumerate(zip(rows,result['fields'])):
        require(row['field']==str(field) and row['units']==derived['unit'],'ordered scalar field ledger')
        magnitude=0.
        for name in ('q0','q1','source'):
            state=arrays[name]
            values=state[field].astype(np.float64) if field<9 else state[1].astype(np.float64)
            if field==9:
                for k in range(2,8):values+=state[k]
            magnitude+=float(np.abs(values).sum()*DX**3)
        for key,value in zip(('beginning_inventory','ending_inventory','source_increment'),
                             (derived['beginning'],derived['candidate'],derived['frozen_source_increment'])):
            checks.append(enclosed(float(row[key]),value,magnitude,32*cells,key))
        for name,side in derived['surfaces'].items():
            label='top' if name=='upper' else name
            magnitude+=side['incoming']+side['outgoing']
            for suffix,key in (('in','incoming'),('out','outgoing')):
                checks.append(enclosed(float(row[label+'_'+suffix]),side[key],magnitude,32*cells+32*faces,label+'_'+suffix))
                if name=='bottom':
                    for region in ('burner','nonburner'):
                        checks.append(enclosed(float(row['bottom_'+region+'_'+suffix]),side[region][key],magnitude,
                                               32*cells+32*faces,'bottom_'+region+'_'+suffix))
        checks.append(enclosed(float(row['closure_residual']),derived['closure_residual'],magnitude,64*cells+64*faces,'closure'))
    return dict(checks=len(checks),max_absolute_residual=max(abs(x['residual']) for x in checks),
                max_roundoff_bound=max(x['roundoff_bound'] for x in checks),bounds='unchanged r245 32C /32C+32F /64C+64F binary64 reduction enclosure')


def derive(packets,captured,metadata,dimensions,contexts):
    arrays=unpack(packets,dimensions,metadata);dt=float(metadata['dt_s'])
    scalar=contexts['scalar']['region_transfers'](*(arrays[k] for k in ('q0','q1','source','low','anti','alpha','burner')),DX,dt,80)
    momentum=contexts['momentum']['central_momentum'](*(arrays[k] for k in ('beginning','provisional','terminal','rates','stage_rates')),tuple(dimensions),dt,80)
    columns=column_check(captured['columns'],arrays,dimensions,dt,float(metadata['beginning_time_s']))
    control=wide_ledger_control(scalar,arrays,captured['ledger'],dimensions) if dimensions[2]==80 else None
    return dict(scalar_common80=scalar,central_momentum=momentum,actual_column_source_check=columns,
                wide_native_selected_ledger_control=control)


def generated_stream_capture(path,size,digest,callback=None):
    def stable():
        require(all(not parent.is_symlink() for parent in path.parents),'unalias generated ancestors')
        s=path.lstat()
        require(stat.S_ISREG(s.st_mode) and s.st_nlink==1,'nonaliased generated stream')
        return (s.st_dev,s.st_ino,s.st_mode,s.st_nlink,s.st_size,s.st_mtime_ns,s.st_ctime_ns)
    before=stable();require(before[4]==size and size<=OUTPUT_LIMIT,'bounded generated stream')
    with path.open('rb') as stream:
        s=os.fstat(stream.fileno());require((s.st_dev,s.st_ino)==before[:2],'generated stream substitution')
        raw=stream.read(OUTPUT_LIMIT+1)
    if callback:callback()
    require(stable()==before and len(raw)==size and sha(raw)==digest,'generated/captured output identity')
    return raw


def publish(path,raw,verifier,case):
    require(len(raw)<=OUTPUT_LIMIT and all(not parent.is_symlink() for parent in path.parents),'bounded unaliased transport publication')
    with path.open('xb') as stream:stream.write(raw)
    cert=dict(schema='rise.fire.published-payload.v2',case_record_id=case,
              issuer_role='new r247 selected transport reader',sha256_v1=sha(raw),v2=verifier['merkle'](raw))
    with Path(str(path)+'.payload-v2.json').open('x') as stream:json.dump(cert,stream,indent=2)


def run(output):
    contexts,identity=implementation();p=contexts['primitives'];a=contexts['admission'];v=contexts['verifier']
    require(output.parent==EXT and not os.path.lexists(output) and all(not x.is_symlink() for x in output.parents),'fresh external owner output')
    raw,record=p['pinned_sealed'](METADATA,METADATA_SHA,METADATA_SEAL_SHA,a['CASES'][1][4],a,v)
    metadata=json.loads(raw);del raw
    require(metadata['schema']=='rise.fire.r247.source-metadata-admission.v1' and len(metadata['cases'])==2,'exact accepted metadata scope')
    output.mkdir(mode=0o700);owner=output.lstat();owner_identity=(owner.st_dev,owner.st_ino);path=output/'transport_rows.jsonl';pending=Path(str(path)+'.preparation.pending.payload-v2.json')
    pending.write_text('incomplete selected transport derivation\n');rows=0;size=0;digest=hashlib.sha256();plans=[];covered={}
    with path.open('xb') as stream:
        for case,expected in zip(metadata['cases'],a['CASES']):
            require(case['variant']==expected[0] and case['case']==expected[4] and case['producer']==expected[5] and
                    case['dimensions']==[104,104,expected[6]] and len(case['frames'])==235,'strict native full case/domain/events')
            covered[case['variant']]=[]
            for ordinal,item in enumerate(case['frames']):
                require(type(item['frame']) is int and item['frame']==ordinal,'strict frame ordinal')
                current,_=rss();plan=allocation_plan(*case['dimensions'],current);plans.append(plan) if ordinal==0 else None
                packets,captured,source,receipts,lifecycles=source_frame(case,item,contexts)
                try:
                    result=derive(packets,captured,source,case['dimensions'],contexts)
                    for member,nbytes,before,cert_path,cert_raw in lifecycles:
                        require(p['state'](member,nbytes)==before and p['capture'](cert_path,sha(cert_raw))==cert_raw,'source pair lifecycle after consumer')
                except BaseException as error:
                    traceback.clear_frames(error.__traceback__)
                    raise
                finally:
                    for packet in packets.values():packet.close()
                del packets,captured,lifecycles
                current,peak=rss();require(peak<=READER_CEILING,'actual Stage B measured peak refusal')
                result.update(variant=case['variant'],source_case=case['case'],source_producer=case['producer'],frame=ordinal,
                              accepted_step=item['accepted_step'],beginning_time_s=item['beginning_time_s'],dt_s=item['dt_s'],
                              accepted_event_s=item['accepted_event_s'],consumed_source_receipts=receipts,
                              before_field_allocation=plan,observed_current_rss_bytes=current,observed_peak_rss_bytes=peak)
                encoded=(json.dumps(result,allow_nan=False)+'\n').encode();require(len(encoded)<=ROW_LIMIT,'bounded result row')
                size+=len(encoded);require(size<=OUTPUT_LIMIT,'bounded result stream');stream.write(encoded);digest.update(encoded)
                covered[case['variant']].append(item['dt_s']);rows+=1
                print(json.dumps(dict(rows=rows,variant=case['variant'],frame=ordinal,peak_rss_bytes=peak)),flush=True)
                del result,encoded,receipts
    actual_owner=output.lstat()
    require(stat.S_ISDIR(actual_owner.st_mode) and (actual_owner.st_dev,actual_owner.st_ino)==owner_identity,'same owner output directory')
    require(rows==470,'complete selected event windows')
    raw=generated_stream_capture(path,size,digest.hexdigest())
    cert=dict(schema='rise.fire.published-payload.v2',case_record_id=a['CASES'][1][4],issuer_role='new r247 selected transport reader',
              sha256_v1=sha(raw),v2=v['merkle'](raw))
    with Path(str(path)+'.payload-v2.json').open('x') as out:json.dump(cert,out,indent=2)
    pending.unlink();_,peak=rss();require(peak<=READER_CEILING,'final measured Stage B peak refusal')
    receipt=dict(schema='rise.fire.r247.selected-transport.v1',rows=rows,metadata_receipt=record,rows_sha256=sha(raw),
                 implementation_sha256=identity,allocation_plans=plans,measured_peak_rss_bytes=peak,
                 selected_covered_dt_s={key:math.fsum(values) for key,values in covered.items()},
                 temporal_support='235 selected accepted intervals per variant, not full15315 cumulativecommon80',
                 original_taller_exit_code=98,full_derivation_accepted=False,physical_acceptance=False,
                 canonical_taller_qualification=False,pressure_restoration_split_available=False)
    publish(output/'reader_receipt.json',(json.dumps(receipt,indent=2)+'\n').encode(),v,a['CASES'][1][4])


def synthetic_fixture(contexts):
    dimensions=(2,2,80);nx,ny,nz=dimensions;cells,faces,sizes=layout(*dimensions)
    beginning=bytearray(sizes['beginning']);native=bytearray(sizes['native']);flux=bytearray(sizes['flux'])
    first=np.frombuffer(beginning,'<f4',count=18*cells+faces)
    raw=np.frombuffer(native,'<f4',count=19*cells+9*faces)
    raw[9*cells:9*cells+faces]=2;raw[9*cells+3*faces:9*cells+4*faces]=1
    raw[9*cells+4*faces:9*cells+5*faces]=np.float32(-0.)
    candidate=77;input_root='a'*64
    struct.pack_into('<Q',native,(19*cells+9*faces)*4+8+10*8,candidate)
    struct.pack_into('<Q',native,(19*cells+9*faces)*4+8+21*8,88)
    native[-32:]=bytes.fromhex(input_root)
    native_root=contexts['verifier']['merkle'](native)['root_sha256']
    flux[:32]=bytes.fromhex(input_root);flux[32:64]=bytes.fromhex(native_root);struct.pack_into('<Q',flux,64,candidate)
    packets=dict(beginning=bytes(beginning),native=bytes(native),flux=bytes(flux))
    metadata=dict(input_root=input_root,native_root=native_root,R1_candidate=str(candidate),dt_s='.5',beginning_time_s='10')
    arrays=unpack(packets,dimensions,metadata)
    scalar=contexts['scalar']['region_transfers'](*(arrays[k] for k in ('q0','q1','source','low','anti','alpha','burner')),DX,.5)
    ledger=[]
    for scope in ('step','full','statistics'):
        for field,d in enumerate(scalar['fields']):
            row=dict(scope=scope,field=str(field),units=d['unit'],beginning_inventory=d['beginning'],ending_inventory=d['candidate'],
                     source_increment=d['frozen_source_increment'],closure_residual=d['closure_residual'])
            for side,values in d['surfaces'].items():
                label='top' if side=='upper' else side
                for suffix,key in (('in','incoming'),('out','outgoing')):
                    row[label+'_'+suffix]=values[key]
                    if side=='bottom':
                        for region in ('burner','nonburner'):row['bottom_'+region+'_'+suffix]=values[region][key]
            ledger.append(row)
    def encode(rows):
        stream=io.StringIO();writer=csv.DictWriter(stream,fieldnames=list(rows[0]));writer.writeheader();writer.writerows(rows)
        return stream.getvalue().encode()
    columns=[];counts=((nx+1)*ny*nz,nx*(ny+1)*nz,nx*ny*(nz+1))
    for y in (0,1):
        for x in (0,1):
            positions=[]
            for z in range(nz):positions.extend(((0,x,y,z),(0,x+1,y,z),(1,x,y,z),(1,x,y+1,z)))
            positions.extend((2,x,y,z) for z in range(nz+1))
            for axis,xx,yy,z in positions:
                row=dict(column_x=x,column_y=y,beginning_time_s=10,observation_index=0,dt_s=.5,axis=axis,
                         face=xx+(nx+(axis==0))*(yy+(ny+(axis==1))*z),x_face=xx,y_face=yy,z_face=z)
                for side in ('lower','upper'):
                    point=[xx,yy,z];point[axis]=max(0,point[axis]-1) if side=='lower' else min(point[axis],dimensions[axis]-1)
                    row[side+'_cell']=point[0]+nx*(point[1]+ny*point[2]);row[side+'_vreman_m2_per_s']=0
                row.update(beginning_momentum_kg_per_m2_s=0,provisional_momentum_kg_per_m2_s=1,terminal_momentum_kg_per_m2_s=2,
                           stress_rate_kg_per_m2_s2=0,buoyancy_rate_kg_per_m2_s2=0,advection_rate_kg_per_m2_s2=-0.,source_rate_kg_per_m2_s2=0,
                           combined_projection_rate_kg_per_m2_s2=2,total_rate_kg_per_m2_s2=4,closure_residual_kg_per_m2_s2=2)
                columns.append(row)
    return packets,dict(columns=encode(columns),ledger=encode(ledger)),metadata,dimensions


def controls():
    contexts,identity=implementation();p=contexts['primitives'];v=contexts['verifier'];red=0
    plan=allocation_plan(104,104,160,64*1024**2)
    require(plan['enumerated_peak_estimate_bytes']<READER_CEILING and
            plan['packet_sizes']==dict(beginning=214776640,native=320010712,flux=544520776),'actual layout/allocation estimate GREEN')
    def refusal(call):
        nonlocal red
        try:call()
        except (ValueError,FileNotFoundError,FileExistsError):red+=1
        else:raise AssertionError('missing refusal')
    refusal(lambda:allocation_plan(104,104,160,READER_CEILING))
    refusal(lambda:allocation_plan(104,104,160,True))
    refusal(lambda:allocation_plan(104,104,640,1))
    case=contexts['admission']['CASES'][0][4]
    with tempfile.TemporaryDirectory() as directory:
        path=Path(directory).resolve()/'packet';data=b'A'*8193;path.write_bytes(data)
        cert=dict(schema='rise.fire.published-payload.v2',case_record_id=case,sha256_v1=sha(data),v2=v['merkle'](data))
        raw=json.dumps(cert).encode();accepted=packet_certificate(raw,sha(data),len(data),case,cert['v2']['root_sha256'],v)
        require(p['captured_packet'](path,sha(data),len(data),v,accepted)[0]==data,'same buffer modern certificate GREEN')
        for key,value in [('digest_version',2.),('payload_bytes',float(len(data))),('chunk_bytes',8192),('root_sha256','0'*64)]:
            bad=dict(cert,v2=dict(cert['v2'],**{key:value}))
            refusal(lambda bad=bad:packet_certificate(json.dumps(bad).encode(),sha(data),len(data),case,cert['v2']['root_sha256'],v))
        refusal(lambda:packet_certificate(raw,sha(data),len(data),'foreign',None,v))
        refusal(lambda:packet_certificate(raw,sha(b'B'),len(data),case,None,v))
        refusal(lambda:p['captured_packet'](path,sha(data),len(data),v,accepted,lambda:path.write_bytes(b'B'*len(data))))
        path.write_bytes(data);pending=Path(str(path)+'.preparation.pending.payload-v2.json');pending.symlink_to(path.parent/'absent')
        refusal(lambda:p['captured_packet'](path,sha(data),len(data),v,accepted));pending.unlink()
        alias=path.parent/'alias';os.link(path,alias);refusal(lambda:p['captured_packet'](path,sha(data),len(data),v,accepted))
        refusal(lambda:run(path.parent))
    packets,captured,metadata,dimensions=synthetic_fixture(contexts)
    result=derive(packets,captured,metadata,dimensions,contexts)
    require(result['wide_native_selected_ledger_control']['checks']==200 and
            result['central_momentum']['common']['unique_faces']==1284 and
            result['actual_column_source_check']['column_associations']==1604,'full synthetic wire consumer GREEN')
    for kind in ('input','native_header','candidate','owner'):
        bad={k:bytearray(v) for k,v in packets.items()}
        if kind=='input':bad['native'][-1]^=1
        elif kind=='native_header':bad['flux'][32]^=1
        elif kind=='candidate':struct.pack_into('<Q',bad['flux'],64,78)
        else:
            c,f,_=layout(*dimensions);struct.pack_into('<Q',bad['native'],(19*c+9*f)*4+8+21*8,0)
        refusal(lambda bad=bad:unpack({k:bytes(v) for k,v in bad.items()},dimensions,metadata))
    with tempfile.TemporaryDirectory() as directory:
        root=Path(directory).resolve();base=root/'frame_00000';binding_path=Path(str(base)+'.v1');ledger_path=root/'step_006400.csv'
        case_record=contexts['admission']['CASES'][0][4];producer=contexts['admission']['CASES'][0][5]
        fields=dict(schema='rise.fire.accepted-stage-budget.v1',case_record_id=case_record,producer_build_id=producer,
                    dimensions='2 2 80',binary32_solver_dx_m=str(DX),accepted_step='6400',beginning_time_s='10',dt_s='.5',
                    input_root=metadata['input_root'],native_root=metadata['native_root'],flux_root=v['merkle'](packets['flux'])['root_sha256'],
                    R1_candidate=metadata['R1_candidate'])
        ledger=(''.join('# '+k+' '+val+'\n' for k,val in fields.items())).encode()+captured['ledger']
        publish(ledger_path,ledger,v,case_record)
        binding=dict(schema='rise.fire.accepted-stage-frame.v1',frame='0',accepted_step='6400',step_record_sha256=sha(ledger),
                     columns_sha256=sha(captured['columns']))
        for name,raw in packets.items():
            binding['.'+name+'.bin']=sha(raw);publish(Path(str(base)+'.'+name+'.bin'),raw,v,case_record)
        publish(Path(str(base)+'.columns.csv'),captured['columns'],v,case_record)
        binding_raw=(''.join(k+' '+value+'\n' for k,value in binding.items())).encode();publish(binding_path,binding_raw,v,case_record)
        def pinned(path):return dict(path=str(path),sha256=sha(path.read_bytes()),seal_sha256=sha(Path(str(path)+'.payload-v2.json').read_bytes()))
        cert_path=Path(str(base)+'.beginning.bin.payload-v2.json');cert_raw=cert_path.read_bytes()
        item=dict(frame=0,accepted_step=6400,beginning_time_s=10.,dt_s=.5,accepted_event_s=10.5,input_root=fields['input_root'],
                  native_root=fields['native_root'],flux_root=fields['flux_root'],beginning_sha256=sha(packets['beginning']),
                  beginning_seal_sha256=sha(cert_raw),beginning_seal=json.loads(cert_raw),binding=pinned(binding_path),ledger=pinned(ledger_path))
        case_meta=dict(case=case_record,producer=producer,dimensions=[2,2,80])
        owned,consumed,source,receipts,lifecycles=source_frame(case_meta,item,contexts)
        require(all(owned[k][:]==packets[k] for k in packets) and consumed['columns']==captured['columns'] and len(receipts)==4,'synthetic composed source capture GREEN')
        require(derive(owned,consumed,source,dimensions,contexts)==result,'heap/mapped exact arithmetic parity')
        for packet in owned.values():packet.close()
        refusal(lambda:source_frame(dict(case_meta,producer='foreign'),item,contexts))
        refusal(lambda:source_frame(case_meta,dict(item,accepted_event_s=10.500000000000002),contexts))
        refusal(lambda:source_frame(case_meta,dict(item,ledger=dict(item['ledger'],seal_sha256='0'*64)),contexts))
        native_path=Path(str(base)+'.native.bin');native_path.write_bytes(packets['native'][:-1])
        calls=[];original_capture=mapped_packet
        def tracked(*args,**kwargs):
            calls.append(str(args[0]));return original_capture(*args,**kwargs)
        globals()['mapped_packet']=tracked
        try:
            refusal(lambda:source_frame(case_meta,item,contexts))
            require(calls==[],'bad later packet refused before any beginning field read')
        finally:globals()['mapped_packet']=original_capture
        native_path.write_bytes(packets['native'])
        cert_path.write_bytes(cert_raw+b' ')
        refusal(lambda:source_frame(case_meta,item,contexts))
    arrays=unpack(packets,dimensions,metadata)
    # Changed clock, missing association and valid-format wrong native operand.
    rows=list(csv.DictReader(io.StringIO(captured['columns'].decode())))
    for key,value in (('observation_index','1'),('stress_rate_kg_per_m2_s2','1')):
        altered=[dict(row) for row in rows];altered[0][key]=value
        out=io.StringIO();writer=csv.DictWriter(out,fieldnames=list(altered[0]));writer.writeheader();writer.writerows(altered)
        refusal(lambda raw=out.getvalue().encode():column_check(raw,arrays,dimensions,.5,10.))
    truncated='\n'.join(captured['columns'].decode().splitlines()[:-1]).encode()
    refusal(lambda:column_check(truncated,arrays,dimensions,.5,10.))
    second_rows=[dict(row,beginning_time_s='10.5') for row in rows]
    second=io.StringIO();writer=csv.DictWriter(second,fieldnames=list(second_rows[0]));writer.writeheader();writer.writerows(second_rows)
    require(column_check(second.getvalue().encode(),arrays,dimensions,.5,10.5)['column_associations']==1604,
            'second event retains producer local observation0 GREEN')
    old_raw=(EXT/'r247_transport_reader_runtime.v1/r247_transport_reader.py').read_bytes()
    require(sha(old_raw)=='771b47b821a3f513c8fe7f5e81d031c805e0ff2240caede296f5e63146432ae5','preserved failed reader identity')
    old={'__name__':'preserved_failed_transport','__file__':str(EXT/'r247_transport_reader_runtime.v1/r247_transport_reader.py')}
    exec(compile(old_raw,old['__file__'],'exec'),old)
    refusal(lambda:old['column_check'](second.getvalue().encode(),arrays,dimensions,.5,10.5,1))
    wrong=captured['ledger'].replace(b'step,0,kg,0.0',b'step,0,kg,1.0',1)
    refusal(lambda:wide_ledger_control(result['scalar_common80'],arrays,wrong,dimensions))
    with tempfile.TemporaryDirectory() as directory:
        path=Path(directory).resolve()/'generated';raw=b'owned';path.write_bytes(raw)
        require(generated_stream_capture(path,len(raw),sha(raw))==raw,'generated capture GREEN')
        refusal(lambda:generated_stream_capture(path,len(raw),sha(raw),lambda:path.write_bytes(b'other')))
        refusal(lambda:publish(path,b'replacement',v,case))
    saved_dx=DX
    try:
        globals()['DX']=.1
        refusal(implementation)
    finally:globals()['DX']=saved_dx
    with tempfile.TemporaryDirectory() as directory:
        root=Path(directory).resolve();path=root/'generated';path.write_bytes(b'A');alias=root/'alias';alias.symlink_to(root,target_is_directory=True)
        refusal(lambda:generated_stream_capture(alias/'generated',1,sha(b'A')))
        refusal(lambda:publish(alias/'elsewhere',b'A',v,case))
    with tempfile.TemporaryDirectory() as directory:
        path=Path(directory).resolve()/'private-source';data=b'A'*8193;path.write_bytes(data)
        cert=dict(v2=v['merkle'](data))
        for repeat in range(3):
            mapped,_=mapped_packet(path,sha(data),len(data),v,cert,p)
            require(mapped[:]==data and v['merkle'](mapped)==v['merkle'](data),'same mapped byte/root GREEN')
            try:mapped[0]=66
            except TypeError:pass
            else:raise AssertionError('writable retained packet')
            view=np.frombuffer(mapped,'u1')
            try:mapped.close()
            except BufferError:pass
            else:raise AssertionError('live mapped view not accounted')
            del view;mapped.close();require(mapped.closed,'explicit mapped packet release')
        refusal(lambda:mapped_packet(path,sha(data),len(data),v,cert,p,lambda:path.write_bytes(b'B'*len(data))))
        path.write_bytes(data)
        refusal(lambda:mapped_packet(path,'0'*64,len(data),v,cert,p))
        refusal(lambda:mapped_packet(path,sha(data),len(data),v,dict(v2=dict(cert['v2'],root_sha256='0'*64)),p))
    require(red==35,'reader allocation/pair/wire/column/code/output REDs')
    print(json.dumps(dict(status='synthetic_reader_prerequisite_only',red=red,allocation_estimate=plan,
                         implementation_sha256=identity,native_empirical_fields_read=0,full_derivation_accepted=False)))


if __name__=='__main__':
    parser=argparse.ArgumentParser(description=__doc__);parser.add_argument('--run',type=Path);parser.add_argument('--controls',action='store_true')
    args=parser.parse_args();require(bool(args.run)!=args.controls,'one explicit command')
    run(args.run) if args.run else controls()
