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
        self.assertEqual(tangent.decode_direction(V((1,.5,1)), M((1,1,1)), V((0,0,1))),(1,0,0,1))
    def test_world_projection_precedes_inverse(self):
        n=V((1/math.sqrt(2),1/math.sqrt(2),0))
        d=tangent.decode_direction(V((1,.5,1)),M((.5,1,1)),n)
        # World projection of (1,0,1) against normalize(.5,1,0)
        # is (.8,-.4,1). Stored local vector is normalize(.4,-.4,1).
        norm=math.sqrt(.4*.4+.4*.4+1)
        self.assertAlmostEqual(d[0],.4/norm);self.assertAlmostEqual(d[1],-.4/norm);self.assertAlmostEqual(d[2],1/norm)
    def test_zero_direction_rejected(self):
        with self.assertRaises(RuntimeError):tangent.decode_direction(V((.5,.5,.5)),M((1,1,1)),V((0,0,1)))
    def test_normal_only_direction_rejected(self):
        with self.assertRaises(RuntimeError):tangent.decode_direction(V((.5,.5,1)),M((1,1,1)),V((0,0,1)))
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
