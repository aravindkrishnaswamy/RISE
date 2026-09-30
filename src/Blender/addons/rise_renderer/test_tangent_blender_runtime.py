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
 o.matrix_world=mat;bpy.context.view_layer.update();original=snapshot();run(label)
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
norm=math.sqrt(1.32)
assert abs(d[0]-.4/norm)<1e-5 and abs(d[1]+.4/norm)<1e-5 and abs(d[2]-1/norm)<1e-5,(d, 'world projection must precede inverse')
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
