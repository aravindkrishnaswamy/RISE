import bpy,sys,math
from pathlib import Path
from mathutils import Matrix
sys.path.insert(0,str(Path(__file__).resolve().parent.parent))
import rise_renderer
from rise_renderer import exporter,tangent_bake
rise_renderer.register()
bpy.ops.wm.read_factory_settings(use_empty=True)
me=bpy.data.meshes.new('quad');me.from_pydata([(-1,-1,0),(1,-1,0),(1,1,0),(-1,1,0)],[],[(0,1,2,3)])
for name,coords in [('UV',[(0,0),(1,0),(1,1),(0,1)]),('Second',[(0,1),(0,0),(1,0),(1,1)])]:
 u=me.uv_layers.new(name=name)
 for l in me.loops:u.data[l.index].uv=coords[l.vertex_index]
me.uv_layers.active_index=0
o=bpy.data.objects.new('quad',me);bpy.context.collection.objects.link(o)
m=bpy.data.materials.new('brushed');m.use_nodes=True;me.materials.append(m);nt=m.node_tree;p=nt.nodes.get('Principled BSDF');p.inputs['Anisotropic'].default_value=.8
p.inputs['Anisotropic Rotation'].default_value=.25
t=nt.nodes.new('ShaderNodeTangent');t.direction_type='UV_MAP';t.uv_map='Second';nt.links.new(t.outputs[0],p.inputs['Tangent'])
def snapshot():return (len(bpy.data.scenes),len(bpy.data.objects),len(bpy.data.meshes),len(bpy.data.materials),len(me.color_attributes),me.uv_layers.active.name,len(nt.nodes),bpy.context.scene.render.engine,bpy.context.view_layer.objects.active,bpy.data.filepath)
original=snapshot()
def run(label):
 dg=bpy.context.evaluated_depsgraph_get();ev=o.evaluated_get(dg);state=exporter._ExportState();res=exporter._mesh_buckets(ev,state,o.matrix_world.copy());assert res and len(state.meshes[0].tangent_attribute)==24
 print(label,state.meshes[0].tangent_attribute,flush=True);assert snapshot()==original
 return state.meshes[0].tangent_attribute
second=run('second');assert abs(second[1]-1)<1e-5
# Entire graph (linked angle + Geometry position) gets Cycles evaluation.
v=nt.nodes.new('ShaderNodeVectorRotate');v.rotation_type='Z_AXIS';value=nt.nodes.new('ShaderNodeValue');value.outputs[0].default_value=math.pi/2
nt.links.new(t.outputs[0],v.inputs['Vector']);nt.links.new(value.outputs[0],v.inputs['Angle']);nt.links.new(v.outputs[0],p.inputs['Tangent'])
original=snapshot();graph=run('linked-rotation');assert graph[0]<-.999
for label,mat in [('rotated',Matrix.Rotation(.7,4,'Z')),('mirror',Matrix.Diagonal((-2,3,1,1)))]:
 o.matrix_world=mat;bpy.context.view_layer.update();original=snapshot();d=run(label)
 from mathutils import Vector
 world=(mat.to_3x3()@Vector(d[:3])).normalized();expected=Vector((-math.cos(.7),-math.sin(.7),0)) if label=='rotated' else Vector((-1,0,0))
 assert (world-expected).length<1e-5,(label,world,expected)
# Bake failure must restore copies and temp data too.
old=tangent_bake.subprocess.run
def fail(*a,**kw):raise RuntimeError('injected failure')
tangent_bake.subprocess.run=fail;original=snapshot()
try:run('failure');raise AssertionError('expected failure')
except RuntimeError as e:assert 'injected' in str(e)
finally:tangent_bake.subprocess.run=old
assert snapshot()==original;print('RESTORATION PASS',flush=True)

# Exercise the same production export from an actual RenderEngine.render callback.
class TangentContextProbe(bpy.types.RenderEngine):
    bl_idname='RISE_TANGENT_CONTEXT_PROBE'
    bl_label='RISE Tangent test context'
    def render(self,depsgraph):
        state=exporter._ExportState()
        exporter._mesh_buckets(o.evaluated_get(depsgraph),state,o.matrix_world.copy())
        assert len(state.meshes[0].tangent_attribute)==24
        print('RENDER CONTEXT EXPORT PASS',flush=True)
        result=self.begin_result(0,0,1,1);self.end_result(result)
bpy.utils.register_class(TangentContextProbe)
cam=bpy.data.objects.new('camera',bpy.data.cameras.new('camera'));bpy.context.collection.objects.link(cam);bpy.context.scene.camera=cam
bpy.context.scene.render.engine='RISE_TANGENT_CONTEXT_PROBE'
bpy.context.scene.render.resolution_x=bpy.context.scene.render.resolution_y=1
bpy.ops.render.render()
bpy.utils.unregister_class(TangentContextProbe)
# Independent world-projection oracle on a physically tilted face under scale.
me2=bpy.data.meshes.new('tilted');me2.from_pydata([(0,0,0),(1,-1,0),(0,0,-1)],[],[(0,1,2)])
o2=bpy.data.objects.new('tilted',me2);bpy.context.collection.objects.link(o2);o2.matrix_world=Matrix.Diagonal((2,1,1,1))
m2=bpy.data.materials.new('world direction');m2.use_nodes=True;me2.materials.append(m2);nt2=m2.node_tree;p2=nt2.nodes.get('Principled BSDF')
xyz=nt2.nodes.new('ShaderNodeCombineXYZ');xyz.inputs['X'].default_value=1;xyz.inputs['Z'].default_value=1;nt2.links.new(xyz.outputs[0],p2.inputs['Tangent'])
bpy.context.view_layer.update();dg=bpy.context.evaluated_depsgraph_get();state=exporter._ExportState();exporter._mesh_buckets(o2.evaluated_get(dg),state,o2.matrix_world.copy());d=state.meshes[0].tangent_attribute
world=o2.matrix_world.to_3x3()@Vector(d[:3]);world_normal=Vector((.5,1,0)).normalized();projected=(world-world_normal*world.dot(world_normal)).normalized()
assert (projected-Vector((.8,-.4,1)).normalized()).length<1e-5,(d,projected,'project promoted raw vector at current normal')
assert (world.normalized()-Vector((1,0,1)).normalized()).length<1e-5,(d,world,'full raw vector preserved')
print('NONUNIFORM TILTED NORMAL WORLD PROJECTION PASS',d,flush=True)
# Object Info Location is world-dependent; one state and one mesh must not cache.
info=nt.nodes.new('ShaderNodeObjectInfo');nt.links.new(info.outputs['Location'],p.inputs['Tangent']);o.matrix_world=Matrix.Translation((1,0,0));bpy.context.view_layer.update();dg=bpy.context.evaluated_depsgraph_get();ev=o.evaluated_get(dg);state=exporter._ExportState()
exporter._mesh_buckets(ev,state,o.matrix_world.copy());first=state.meshes[-1].tangent_attribute
exporter._mesh_buckets(ev,state,Matrix.Translation((0,1,0)));second=state.meshes[-1].tangent_attribute
assert first[0]>.999 and second[1]>.999,(first,second)
print('SHARED MESH OBJECT INFO LOCATION INSTANCE PASS',flush=True)
# Carry Cycles dupli random identity, rather than a temporary-object hash.
combine=nt.nodes.new('ShaderNodeCombineXYZ');combine.inputs['Y'].default_value=1;nt.links.new(info.outputs['Random'],combine.inputs['X']);nt.links.new(combine.outputs[0],p.inputs['Tangent'])
bpy.context.view_layer.update();dg=bpy.context.evaluated_depsgraph_get();ev=o.evaluated_get(dg)
for random_id in (0x3fffffff,0xbfffffff):
 state=exporter._ExportState();exporter._mesh_buckets(ev,state,Matrix.Identity(4),{'is_instance':True,'random_id':random_id});d=state.meshes[0].tangent_attribute
 assert abs(d[0]/d[1]-random_id/0xffffffff)<1e-5,d
print('OBJECT INFO RANDOM INSTANCE ID PASS',flush=True)
# Two material buckets share positions, retain independent corner payloads.
me3=bpy.data.meshes.new('material seams');me3.from_pydata([(-1,-1,0),(1,-1,0),(1,1,0),(-1,1,0)],[],[(0,1,2),(0,2,3)])
for name,coords in [('UV',[(0,0),(1,0),(1,1),(0,1)]),('Second',[(0,1),(0,0),(1,0),(1,1)])]:
 u=me3.uv_layers.new(name=name)
 for l in me3.loops:u.data[l.index].uv=coords[l.vertex_index]
me3.uv_layers.active_index=0
ma=m.copy();mb=m.copy()
for mat,direction in [(ma,(0,1,0)),(mb,(-1,0,0))]:
 tree=mat.node_tree;node=tree.nodes.new('ShaderNodeCombineXYZ')
 for k,name in enumerate(('X','Y','Z')):node.inputs[name].default_value=direction[k]
 tree.links.new(node.outputs[0],tree.nodes.get('Principled BSDF').inputs['Tangent'])
me3.materials.append(ma);me3.materials.append(mb);me3.polygons[1].material_index=1
o3=bpy.data.objects.new('seams',me3);bpy.context.collection.objects.link(o3);bpy.context.view_layer.update();dg=bpy.context.evaluated_depsgraph_get();state=exporter._ExportState();exporter._mesh_buckets(o3.evaluated_get(dg),state,Matrix.Identity(4))
assert len(state.meshes)==2
assert len(state.meshes[0].tangent_attribute)==12 and state.meshes[0].tangent_attribute[1]>.999
assert len(state.meshes[1].tangent_attribute)==12 and state.meshes[1].tangent_attribute[0]<-.999
assert state.meshes[0].uvs==state.meshes[1].uvs
print('MATERIAL CORNER SEAM PAYLOAD PASS',flush=True)
# Actual depsgraph collection instances carry distinct Random IDs into export_scene.
collection=bpy.data.collections.new('tangent prototypes')
prototype=bpy.data.objects.new('prototype',me.copy());prototype.data.materials.clear();prototype.data.materials.append(m);collection.objects.link(prototype)
for name,location in [('instance A',(4,0,0)),('instance B',(6,0,0))]:
 empty=bpy.data.objects.new(name,None);empty.instance_type='COLLECTION';empty.instance_collection=collection;empty.location=location;bpy.context.collection.objects.link(empty)
bpy.context.view_layer.update();dg=bpy.context.evaluated_depsgraph_get()
expected={}
for instance in dg.object_instances:
 if instance.is_instance and instance.object.original.name=='prototype':
  expected[round(instance.matrix_world.translation.x)]=(instance.random_id & 0xffffffff)/0xffffffff
exported,_=exporter.export_scene(dg);by_name={mesh.name:mesh for mesh in exported.meshes};observed={}
for obj in exported.objects:
 if round(obj.transform[3]) in expected:
  d=by_name[obj.geometry_name].tangent_attribute;observed[round(obj.transform[3])]=d[0]/d[1]
assert len(expected)==2 and len(observed)==2,(expected,observed,[(obj.name,obj.transform[3]) for obj in exported.objects])
for location,r in expected.items():assert abs(observed[location]-r)<1e-5,(expected,observed)
assert len(set(round(x,5) for x in observed.values()))==2,(expected,observed)
print('ACTUAL DEPSGRAPH INSTANCE EXPORT PASS',expected,observed,flush=True)

# R1: alternate shader direction with supported active-UV Coat Normal.
import os,subprocess,tempfile
from mathutils import Vector
from rise_renderer import bridge
with tempfile.TemporaryDirectory(prefix='rise-tangent-runtime-') as directory:
    rm=bpy.data.meshes.new('R1 triangle');rm.from_pydata([(0,0,0),(1,0,0),(0,1,0)],[],[(0,1,2)])
    uv=rm.uv_layers.new(name='Primary')
    for loop in rm.loops:uv.data[loop.index].uv=[(0,0),(1,0),(0,1)][loop.vertex_index]
    ro=bpy.data.objects.new('R1 coupled material',rm);bpy.context.collection.objects.link(ro)
    rmat=bpy.data.materials.new('R1 coupled material');rmat.use_nodes=True;rm.materials.append(rmat)
    tree=rmat.node_tree;rp=tree.nodes.get('Principled BSDF');rp.inputs['Coat Weight'].default_value=1;rp.inputs['Anisotropic'].default_value=.8;rp.inputs['Anisotropic Rotation'].default_value=.125
    xyz=tree.nodes.new('ShaderNodeCombineXYZ');xyz.inputs['Y'].default_value=1;tree.links.new(xyz.outputs[0],rp.inputs['Tangent'])
    image=bpy.data.images.new('R1 encoded normal',1,1,float_buffer=True);image.colorspace_settings.name='Non-Color';image.pixels=[.8,.5,.9,1];image.filepath_raw=directory+'/normal.exr';image.file_format='OPEN_EXR';image.save()
    tex=tree.nodes.new('ShaderNodeTexImage');tex.image=image;normal=tree.nodes.new('ShaderNodeNormalMap');normal.uv_map='Primary';tree.links.new(tex.outputs['Color'],normal.inputs['Color']);tree.links.new(normal.outputs[0],rp.inputs['Coat Normal']);base_normal=tree.nodes.new('ShaderNodeNormalMap');base_normal.uv_map='Primary';tree.links.new(tex.outputs['Color'],base_normal.inputs['Color']);tree.links.new(base_normal.outputs[0],rp.inputs['Normal'])
    # Independent Cycles emission of the NORMAL MAP output, not shader Tangent.
    out=tree.nodes.get('Material Output');emit=tree.nodes.new('ShaderNodeEmission');tree.links.new(normal.outputs[0],emit.inputs['Color']);tree.links.new(emit.outputs[0],out.inputs['Surface'])
    bpy.context.scene.render.engine='CYCLES';bpy.context.scene.cycles.samples=1
    for ob in bpy.context.selected_objects:ob.select_set(False)
    ro.select_set(True);bpy.context.view_layer.objects.active=ro
    color=rm.color_attributes.new(name='R1 normal oracle',type='FLOAT_COLOR',domain='CORNER');rm.color_attributes.active_color=color;bpy.context.scene.render.bake.target='VERTEX_COLORS';bpy.ops.object.bake(type='EMIT')
    for value in color.data:assert (Vector(value.color[:3])-Vector((.6,0,.8))).length<1e-5,list(value.color)
    print('R1 CYCLES COAT NORMAL INDEPENDENT ORACLE PASS (SIMULTANEOUS BASE NORMAL LINK)',flush=True)
    rm.color_attributes.remove(color);tree.links.new(rp.outputs[0],out.inputs['Surface']);tree.links.remove(rp.inputs['Normal'].links[0])
    def r1_snapshot():
        return (len(bpy.data.scenes),len(bpy.data.objects),len(bpy.data.meshes),len(bpy.data.materials),len(rm.color_attributes),len(tree.nodes),bpy.context.scene.render.engine,bpy.context.view_layer.objects.active,bpy.data.filepath,tuple(tuple(row) for row in ro.matrix_world),tuple(ob.name for ob in bpy.context.selected_objects))
    def export_r1(matrix):
        ro.matrix_world=matrix;bpy.context.view_layer.update();before=r1_snapshot();dg=bpy.context.evaluated_depsgraph_get();state=exporter._ExportState()
        try:exporter._mesh_buckets(ro.evaluated_get(dg),state,matrix.copy())
        finally:assert r1_snapshot()==before,'R1 exporter state cleanup'
        mesh=state.meshes[0];assert mesh.tangent_is_shader_direction
        marshaler=bridge._SceneHandle.__new__(bridge._SceneHandle);marshaler.keepalive=[];payload=marshaler._marshal_mesh(mesh);assert payload.tangent_is_shader_direction==1 and payload.num_tangents==3
        material=state.materials[0];assert material.coat_normal_painter_name and material.coat_normal_scale==1
        painters={paint.name:paint for paint in state.painters};rotation=painters[material.anisotropy_rotation_painter_name].color[0];assert abs(rotation-math.pi/4)<1e-6
        return mesh.tangent_attribute,rotation
    cases=[]
    d,angle=export_r1(Matrix.Identity(4));cases.append((Matrix.Identity(4),d,(0,1,0),angle))
    xyz.inputs['X'].default_value=1
    for label,matrix in [('identity',Matrix.Identity(4)),('mirror',Matrix.Diagonal((-1,1,1,1))),('rotated',Matrix.Rotation(.7,4,'Z')),('valid-scale3',Matrix.Diagonal((1e3,1e3,1e3,1))),('valid-scale5',Matrix.Diagonal((1e5,1e5,1e5,1))),('valid-scale7',Matrix.Diagonal((1e7,1e7,1e7,1))),('valid-scale9',Matrix.Diagonal((1e9,1e9,1e9,1)))]:
        d,angle=export_r1(matrix);world=(matrix.to_3x3()@Vector(d[:3])).normalized();expected=Vector((math.sqrt(.5),math.sqrt(.5),0))
        assert (world-expected).length<1e-5,(label,world,expected)
        rotated=world*math.cos(angle)+Vector((0,0,1)).cross(world)*math.sin(angle)
        assert (rotated-Vector((0,1,0))).length<1e-5,(label,rotated)
        cases.append((matrix,d,tuple(expected),angle))
    print('R1 GENERIC MIRROR ROTATION WORLD ORACLE PASS',flush=True)
    # Decoder scale invariant; backend cannot bake this huge world geometry.
    for scales in ((1e11,1e11,1e11,1),(2e11,1e11,1e11,1)):
        try:export_r1(Matrix.Diagonal(scales));raise AssertionError('huge invalid bake buffer accepted')
        except RuntimeError as error:assert 'backend conditioning failure' in str(error),str(error)
    print('R1 CYCLES LARGE GEOMETRY BACKEND DIAGNOSTIC PASS',flush=True)
    # New second-UV direct producer must equal full graph identity under M^-T.
    oblique=rm.uv_layers.new(name='Oblique')
    for loop in rm.loops:oblique.data[loop.index].uv=[(0,0),(.5,-.5),(.5,.5)][loop.vertex_index]
    rm.uv_layers.active_index=0
    tangent=tree.nodes.new('ShaderNodeTangent');tangent.direction_type='UV_MAP';tangent.uv_map='Oblique';identity=tree.nodes.new('ShaderNodeVectorMath');identity.operation='ADD';identity.inputs[1].default_value=(0,0,0);tree.links.new(tangent.outputs[0],identity.inputs[0])
    for label,matrix in [('nonuniform',Matrix.Diagonal((2,1,1,1))),('mirror-nonuniform',Matrix.Diagonal((-2,1,1,1)))]:
        expected=(matrix.to_3x3().inverted().transposed()@Vector((math.sqrt(.5),math.sqrt(.5),0))).normalized()
        for socket in (tangent.outputs[0],identity.outputs[0]):
            tree.links.new(socket,rp.inputs['Tangent']);d,angle=export_r1(matrix);world=(matrix.to_3x3()@Vector(d[:3])).normalized();assert (world-expected).length<1e-5,(label,world,expected);cases.append((matrix,d,tuple(expected),angle))
    print('R1 SECOND UV DIRECT IDENTITY NONUNIFORM MIRROR PASS',flush=True)
    # Same direction equivalence with tilted face normal and a normal component.
    tilted=bpy.data.meshes.new('R1 tilted UV');tilted.from_pydata([(0,0,0),(1,-1,0),(0,0,-1)],[],[(0,1,2)]);tilted.materials.append(rmat)
    for name in ('Primary','Oblique'):
        layer=tilted.uv_layers.new(name=name)
        for loop in tilted.loops:layer.data[loop.index].uv=[(0,0),(.5,-.5),(.5,.5)][loop.vertex_index]
    tilted.uv_layers.active_index=0;old_mesh=ro.data;ro.data=tilted;matrix=Matrix.Diagonal((2,1,3,1));results=[]
    for socket in (tangent.outputs[0],identity.outputs[0]):
        tree.links.new(socket,rp.inputs['Tangent']);d,_=export_r1(matrix);results.append((matrix.to_3x3()@Vector(d[:3])).normalized())
    assert (results[0]-results[1]).length<1e-5,results
    tilted.calc_tangents(uvmap='Oblique');raw=tilted.loops[0].tangent.copy();tilted.free_tangents();world_t=matrix.to_3x3().inverted().transposed()@raw;world_n=(matrix.to_3x3().inverted().transposed()@tilted.corner_normals[0].vector).normalized();expected=(world_t-world_n*world_t.dot(world_n)).normalized();assert (results[0]-expected).length<1e-5,(results,expected)
    ro.data=old_mesh
    print('R1 SECOND UV TILTED NORMAL INDEPENDENT WORLD ORACLE PASS',flush=True)
    # Generic raw normal component survives until the final shading normal.
    tree.links.new(xyz.outputs[0],rp.inputs['Tangent']);tree.links.new(base_normal.outputs[0],rp.inputs['Normal'])
    for raw in ((1,1,1),(0,0,1)):
        for index,value in enumerate(raw):xyz.inputs[index].default_value=value
        d,angle=export_r1(Matrix.Identity(4));observed=Vector(d[:3]);assert (observed.normalized()-Vector(raw).normalized()).length<1e-5,(raw,observed)
        # Mesh plane fallback is temporary for the parallel direction. Base
        # NormalMap changes N to (.6,0,.8), making the raw Z vector valid.
        expected=(math.sqrt(.5),math.sqrt(.5),0) if raw==(1,1,1) else (1,0,0)
        cases.append((Matrix.Identity(4),d,expected,angle))
        bpy.context.view_layer.update();state=exporter._ExportState();exporter._mesh_buckets(ro.evaluated_get(bpy.context.evaluated_depsgraph_get()),state,Matrix.Identity(4));assert state.modifiers and state.material_map[exporter._pointer_key(rmat)].modifier_name
    tree.links.remove(rp.inputs['Normal'].links[0])
    try:export_r1(Matrix.Identity(4));raise AssertionError('parallel raw accepted without supported normal modifier')
    except RuntimeError as error:assert 'zero/parallel' in str(error),str(error)
    print('R1 RAW NORMAL COMPONENT AND SUPPORTED PARALLEL NORMAL MODIFIER PASS',flush=True)
    # Actual exported corner payload/material rotation through shipping bridge/core.
    path=Path(directory)/'cases.txt'
    rows=[str(len(cases))]
    for matrix,d,expected,angle in cases:rows.append(' '.join(str(v) for v in ([v for row in matrix for v in row]+d+list(expected)+[angle]+list((matrix.to_3x3()@Vector(d[:3])).normalized()))))
    path.write_text('\n'.join(rows)+'\n')
    executable=Path(__file__).resolve().parents[4]/'bin/tests/BlenderShaderDirectionTest'
    env=dict(os.environ,RISE_TANGENT_SHADER_CASES=str(path));completed=subprocess.run([str(executable)],env=env,capture_output=True,text=True,timeout=60);print(completed.stdout,completed.stderr,flush=True);assert completed.returncode==0,completed.returncode
    print('R1 ACTUAL EXPORT SHIPPING CONSUMER COAT ROTATION RGB NM SCATTER ROUNDTRIP PASS',flush=True)
# Material A direction is valid only on its own disjoint faces; B is unlinked.
domain=bpy.data.meshes.new('R1 material domain');domain.from_pydata([(2,0,0),(3,0,0),(2,1,0),(0,0,0),(1,0,0),(0,1,0)],[],[(0,1,2),(3,4,5)])
a=bpy.data.materials.new('R1 valid A');a.use_nodes=True;tree=a.node_tree;ap=tree.nodes.get('Principled BSDF');position=tree.nodes.new('ShaderNodeNewGeometry');sep=tree.nodes.new('ShaderNodeSeparateXYZ');mask=tree.nodes.new('ShaderNodeMath');mask.operation='GREATER_THAN';mask.inputs[1].default_value=1.5;combine=tree.nodes.new('ShaderNodeCombineXYZ');tree.links.new(position.outputs['Position'],sep.inputs[0]);tree.links.new(sep.outputs['X'],mask.inputs[0]);tree.links.new(mask.outputs[0],combine.inputs['X']);tree.links.new(combine.outputs[0],ap.inputs['Tangent'])
b=bpy.data.materials.new('R1 ordinary B');b.use_nodes=True;domain.materials.append(a);domain.materials.append(b);domain.polygons[1].material_index=1
domain_object=bpy.data.objects.new('R1 material domain',domain);bpy.context.collection.objects.link(domain_object)
def domain_export():
    bpy.context.view_layer.update();before=(len(bpy.data.scenes),len(bpy.data.objects),len(bpy.data.meshes),len(bpy.data.materials));dg=bpy.context.evaluated_depsgraph_get();state=exporter._ExportState()
    try:exporter._mesh_buckets(domain_object.evaluated_get(dg),state,Matrix.Identity(4))
    finally:assert before==(len(bpy.data.scenes),len(bpy.data.objects),len(bpy.data.meshes),len(bpy.data.materials))
    return state.meshes
meshes=domain_export();assert len(meshes)==2 and meshes[0].tangent_attribute==[1,0,0,1]*3 and not meshes[1].tangent_attribute
mask.inputs[1].default_value=3.5
try:domain_export();raise AssertionError('invalid direction inside assigned material accepted')
except RuntimeError as error:assert 'zero/parallel' in str(error),str(error)
print('R1 MATERIAL VALID DOMAIN IRRELEVANT ZERO AND ASSIGNED ZERO CLEANUP PASS',flush=True)

# Independent constant encoded EMIT separates backend conditioning from shader
# zero/decoder arithmetic. Never rescale world Position or graph dependencies.
control_mesh=bpy.data.meshes.new('constant EMIT conditioning');control_mesh.from_pydata([(0,0,0),(1,0,0),(0,1,0)],[],[(0,1,2)])
control_object=bpy.data.objects.new('constant EMIT conditioning',control_mesh);bpy.context.collection.objects.link(control_object)
control_material=bpy.data.materials.new('constant encoded EMIT');control_material.use_nodes=True;control_mesh.materials.append(control_material)
control_tree=control_material.node_tree;control_emit=control_tree.nodes.new('ShaderNodeEmission');control_emit.inputs['Color'].default_value=(.8535534,.8535534,.5,1);control_tree.links.new(control_emit.outputs[0],control_tree.nodes.get('Material Output').inputs['Surface'])
bpy.context.scene.render.engine='CYCLES';bpy.context.scene.cycles.samples=1
for ob in bpy.context.selected_objects:ob.select_set(False)
control_object.select_set(True);bpy.context.view_layer.objects.active=control_object;bpy.context.scene.render.bake.target='VERTEX_COLORS'
control_color=control_mesh.color_attributes.new(name='write sentinel',type='FLOAT_COLOR',domain='CORNER');control_mesh.color_attributes.active_color=control_color
for scale in (1,1e3,1e5,1e7,1e9,1e11):
    control_object.matrix_world=Matrix.Diagonal((scale,scale,scale,1));bpy.context.view_layer.update()
    for value in control_color.data:value.color=(-1,-1,-1,-1)
    bpy.ops.object.bake(type='EMIT');colors=[list(value.color) for value in control_color.data]
    print('ENCODED_EMIT_CONTROL',scale,colors,flush=True)
    for color in colors:
        if scale<=1e9:
            tangent_bake.validate_baked_color(color);assert max(abs(a-b) for a,b in zip(color,(.8535534,.8535534,.5,1)))<1e-6,color
        else:
            try:tangent_bake.validate_baked_color(color);raise AssertionError('measured conditioning failure unexpectedly valid; reclassify runtime evidence')
            except RuntimeError as error:assert 'backend conditioning failure' in str(error),str(error)
print('R1 CONSTANT ENCODED EMIT BACKEND CONDITIONING CONTROLS PASS',flush=True)
