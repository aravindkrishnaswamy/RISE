"""Bpy-free arithmetic/ABI tests; Blender execution is a separate runtime gate."""
import importlib.util
import math
from pathlib import Path
from types import SimpleNamespace
import unittest

ROOT = Path(__file__).parent

def load(name):
    spec = importlib.util.spec_from_file_location(name, ROOT / (name + '.py'))
    module = importlib.util.module_from_spec(spec)
    import sys
    sys.modules[name] = module
    spec.loader.exec_module(module)
    return module

tangent = load('tangent_bake')
bridge = load('bridge')

class V(list):
    def dot(self, b): return sum(a*c for a,c in zip(self,b))
    def __mul__(self, x): return V(a*x for a in self)
    def __truediv__(self, x): return V(a/x for a in self)
    def cross(self, b): return V((self[1]*b[2]-self[2]*b[1],self[2]*b[0]-self[0]*b[2],self[0]*b[1]-self[1]*b[0]))
    def __isub__(self, b): self[:] = [a-c for a,c in zip(self,b)]; return self
    @property
    def length_squared(self): return self.dot(self)
    def normalize(self): self[:] = [a/math.sqrt(self.length_squared) for a in self]

class M:
    def __init__(self, diagonal): self.diagonal=diagonal
    def transposed(self):return self
    def __matmul__(self, v): return V(a*b for a,b in zip(v,self.diagonal))

class TangentExportTest(unittest.TestCase):
    def test_signed_linear_direction(self):
        self.assertEqual(tangent.decode_direction(V((0,.5,.5)), M((1,1,1)), V((0,0,1))),(-1,0,0,1))
    def test_nonuniform_inverse_direction(self):
        d=tangent.decode_direction(V((.8,.9,.5)), M((.5,1/3,1)), V((0,0,1)))
        norm=math.hypot(.3,.8/3)
        self.assertAlmostEqual(d[0],.3/norm);self.assertAlmostEqual(d[1],(.8/3)/norm)
    def test_mirror_inverse_direction(self):
        self.assertEqual(tangent.decode_direction(V((0,.5,.5)), M((-.5,1/3,1)), V((0,0,1))),(1,0,0,1))
    def test_normal_projection(self):
        d=tangent.decode_direction(V((1,.5,1)), M((1,1,1)), V((0,0,1)))
        self.assertAlmostEqual(d[0],math.sqrt(.5));self.assertEqual(d[1],0);self.assertAlmostEqual(d[2],math.sqrt(.5))
    def test_world_projection_precedes_inverse(self):
        n=V((1/math.sqrt(2),1/math.sqrt(2),0))
        d=tangent.decode_direction(V((1,.5,1)),M((.5,1,1)),n)
        # Preserve raw inverse transport; projecting the promoted raw vector
        # at the current world normal gives normalize(.8,-.4,1).
        norm=math.sqrt(1.25)
        self.assertAlmostEqual(d[0],.5/norm);self.assertEqual(d[1],0);self.assertAlmostEqual(d[2],1/norm)
        promoted=V((2*d[0],d[1],d[2]));world_n=V((.5,1,0));world_n.normalize()
        projected=world_n.cross(promoted).cross(world_n);projected.normalize()
        expected=V((.8,-.4,1));expected.normalize()
        for value,truth in zip(projected,expected):self.assertAlmostEqual(value,truth)
    def test_parallel_with_actual_normal_modifier_preserved(self):
        self.assertEqual(tangent.decode_direction(V((.5,.5,1)),M((1,1,1)),V((0,0,1)),allow_parallel=True),(0,0,1,1))
        with self.assertRaises(RuntimeError):tangent.decode_direction(V((.5,.5,.5)),M((1,1,1)),V((0,0,1)),allow_parallel=True)
    def test_zero_direction_rejected(self):
        with self.assertRaises(RuntimeError):tangent.decode_direction(V((.5,.5,.5)),M((1,1,1)),V((0,0,1)))
    def test_normal_only_direction_rejected(self):
        with self.assertRaises(RuntimeError):tangent.decode_direction(V((.5,.5,1)),M((1,1,1)),V((0,0,1)))
    def test_impossible_bake_buffer_rejected(self):
        for color in ((0,0,0,0),(0,0,0,1),(math.nan,.5,.5,1)):
            with self.assertRaisesRegex(RuntimeError,'backend conditioning'):tangent.validate_baked_color(color)
        tangent.validate_baked_color((.8535534,.8535534,.5,1))
        tangent.validate_baked_color((.5,.5,.5,1)) # genuine zero is decoder's separate physical rejection
    def test_representation_scale_invariance(self):
        for scale in (1e-100,1,1e100):
            d=tangent.decode_direction(V((1,1,.5)),M((scale,scale,scale)),V((0,0,1)))
            self.assertAlmostEqual(d[0],math.sqrt(.5));self.assertAlmostEqual(d[1],math.sqrt(.5))
        d=tangent.decode_direction(V((1,1,.5)),M((.5e-11,1e-11,1e-11)),V((0,0,1)))
        self.assertAlmostEqual(d[1]/d[0],2)
    def test_nonfinite_direction_rejected(self):
        for invalid in (math.nan,math.inf):
            with self.assertRaises(RuntimeError):tangent.decode_direction(V((invalid,.5,.5)),M((1,1,1)),V((0,0,1)))
    def test_shader_direction_semantic_marshalling(self):
        h=bridge._SceneHandle.__new__(bridge._SceneHandle);h.keepalive=[]
        mesh=SimpleNamespace(name='shader',vertices=[],normals=[],uvs=[],vertex_indices=[],normal_indices=[],uv_indices=[],num_vertices=0,num_normals=0,num_uvs=0,num_triangles=0,double_sided=False,use_face_normals=False,tangent_is_shader_direction=True)
        self.assertEqual(h._marshal_mesh(mesh).tangent_is_shader_direction,1)
    def test_per_corner_abi_lifetime(self):
        h=bridge._SceneHandle.__new__(bridge._SceneHandle);h.keepalive=[]
        mesh=SimpleNamespace(name='seams',vertices=[0,0,0]*3,normals=[0,0,1],uvs=[0,0,0],vertex_indices=[0,1,2],normal_indices=[0,0,0],uv_indices=[0,0,0],num_vertices=3,num_normals=1,num_uvs=1,num_triangles=1,double_sided=True,use_face_normals=False,tangent_attribute=[0,1,0,1,1,0,0,-1,-1,0,0,1])
        payload=h._marshal_mesh(mesh)
        self.assertEqual(payload.num_tangents,3)
        self.assertEqual([payload.tangent_attribute[i] for i in range(12)],mesh.tangent_attribute)
        self.assertGreater(len(h.keepalive),1)
    def test_old_payload_has_no_tangent(self):
        h=bridge._SceneHandle.__new__(bridge._SceneHandle);h.keepalive=[]
        mesh=SimpleNamespace(name='old',vertices=[],normals=[],uvs=[],vertex_indices=[],normal_indices=[],uv_indices=[],num_vertices=0,num_normals=0,num_uvs=0,num_triangles=0,double_sided=False,use_face_normals=False)
        payload=h._marshal_mesh(mesh);self.assertEqual(payload.num_tangents,0);self.assertFalse(payload.tangent_attribute)

if __name__=='__main__':unittest.main()
