"""Comprehensive unit tests for `procedural_expr.py` (all 7 builtin groups).

Covers:
  1. ShaderNodeMath: all 41 Cycles NodeMathType operations + use_clamp.
  2. ShaderNodeVectorMath: all 30 operations (27 vector + 3 scalar outputs).
  3. ShaderNodeClamp: MINMAX and RANGE modes.
  4. ShaderNodeMapRange: FLOAT and FLOAT_VECTOR, all 4 interpolation modes + clamp.
  5. ShaderNodeVectorRotate: all 5 rotation types + invert + center.
  6. ShaderNodeVectorTransform: all 3 types x all 9 (from, to) space pairs.
  7. ShaderNodeTexCoord: Generated, Object, UV, Normal, Camera + fail-closed cases.
  8. ShaderNodeRGBToBW.
  9. ShaderNodeValToRGB: RGB (LINEAR/CONSTANT/EASE), HSV & HSL (NEAR/FAR/CW/CCW),
     Color & Alpha outputs, fail-closed on CARDINAL/B_SPLINE.
 10. ShaderNodeMix VECTOR NON_UNIFORM (with and without clamp_factor).
 11. ShaderNodeMapping with dynamically linked Location/Rotation/Scale in all 4 modes.
 12. Noise Texture dual-output sharing (Fac + Color -> 1 `osl_noise_texture_color` def).
 13. Single-use inlining vs multi-use `def` binding + 250-node deep-chain splitting.
 14. Material classifier (`material_bake.classify_material`) admitting Groups 5-7 nodes.
 15. End-to-end RISE C++ VM verification: every compiled expression parses and
     compiles cleanly inside RISE's real `expression_painter` parser when `bin/rise`
     is built.
"""

import os
import subprocess
import sys
import tempfile
import types
import unittest

_ADDON_DIR = os.path.dirname(os.path.abspath(__file__))
_REPO_ROOT = os.path.abspath(os.path.join(_ADDON_DIR, "..", "..", "..", ".."))
_PKG_NAME = "rise_renderer"


def _install_stubs():
    if "bpy" not in sys.modules:
        bpy_mod = types.ModuleType("bpy")
        bpy_mod.app = types.SimpleNamespace(version=(5, 3, 0), tempdir="/tmp")
        bpy_mod.types = types.SimpleNamespace(
            RenderEngine=type("RenderEngine", (), {}),
            AddonPreferences=type("AddonPreferences", (), {}),
            PropertyGroup=type("PropertyGroup", (), {}),
            Scene=type("Scene", (), {}),
        )
        bpy_mod.props = types.SimpleNamespace(
            StringProperty=lambda **kw: None,
            IntProperty=lambda **kw: None,
            FloatProperty=lambda **kw: None,
            BoolProperty=lambda **kw: None,
            EnumProperty=lambda **kw: None,
            PointerProperty=lambda **kw: None,
        )
        bpy_mod.utils = types.SimpleNamespace(
            register_class=lambda cls: None,
            unregister_class=lambda cls: None,
        )
        sys.modules["bpy"] = bpy_mod
    if "bpy_extras" not in sys.modules:
        be = types.ModuleType("bpy_extras")
        nsu = types.ModuleType("bpy_extras.node_shader_utils")
        nsu.PrincipledBSDFWrapper = type("PrincipledBSDFWrapper", (), {})
        be.node_shader_utils = nsu
        sys.modules["bpy_extras"] = be
        sys.modules["bpy_extras.node_shader_utils"] = nsu
    if "mathutils" not in sys.modules:
        mu = types.ModuleType("mathutils")
        mu.Vector = lambda v: tuple(v)
        sys.modules["mathutils"] = mu
    if _PKG_NAME not in sys.modules:
        pkg = types.ModuleType(_PKG_NAME)
        pkg.__path__ = [_ADDON_DIR]
        sys.modules[_PKG_NAME] = pkg


_install_stubs()

from rise_renderer import material_bake, procedural_expr  # noqa: E402


class _FakeLink:
    def __init__(self, from_node, from_socket, is_muted=False):
        self.from_node = from_node
        self.from_socket = from_socket
        self.is_muted = is_muted


class _FakeSocket:
    def __init__(self, name, sock_type="VALUE", default_value=0.0):
        self.name = name
        self.type = sock_type
        self.default_value = default_value
        self.links = []

    @property
    def is_linked(self):
        return bool(self.links)


class _FakeSocketCollection(list):
    def get(self, name, default=None):
        for s in self:
            if s.name == name:
                return s
        return default

    def __getitem__(self, key):
        if isinstance(key, str):
            for s in self:
                if s.name == key:
                    return s
            raise KeyError(key)
        return super().__getitem__(key)


class _FakeNode:
    _next_ptr = 50000

    def __init__(self, bl_idname, name=None, inputs=(), outputs=(), **attrs):
        _FakeNode._next_ptr += 16
        self._ptr = _FakeNode._next_ptr
        self.bl_idname = bl_idname
        self.name = name or bl_idname
        self.mute = False
        self.inputs = _FakeSocketCollection(inputs)
        self.outputs = _FakeSocketCollection(outputs)
        for k, v in attrs.items():
            setattr(self, k, v)

    def as_pointer(self):
        return self._ptr


def _link(src_node, src_out_name, dst_node, dst_in_key, dst_type=None):
    src_sock = next(o for o in src_node.outputs if o.name == src_out_name)
    if isinstance(dst_in_key, int):
        dst_sock = dst_node.inputs[dst_in_key]
    else:
        dst_sock = next(
            i
            for i in dst_node.inputs
            if i.name == dst_in_key and (dst_type is None or i.type == dst_type)
        )
    dst_sock.links.append(_FakeLink(src_node, src_sock))


def _compile_node(node, out_name, want_type="vec3", **kw):
    dst = _FakeSocket("Target", "RGBA" if want_type == "vec3" else "VALUE")
    out_sock = next(o for o in node.outputs if o.name == out_name)
    dst.links.append(_FakeLink(node, out_sock))
    return procedural_expr.compile_socket_expression(dst, want_type=want_type, **kw)


def _format_rise_painter_chunk(name: str, compiled: procedural_expr.CompiledExpression) -> str:
    lines = [f"expression_painter", "{", f"\tname {name}"]
    raw_lines = compiled.expression.splitlines()
    for ln in raw_lines[:-1]:
        lines.append(f"\t{ln}")
    lines.append(f"\texpr {raw_lines[-1]}")
    lines.append("}")
    return "\n".join(lines)


class ProceduralExprFullCoverageTest(unittest.TestCase):
    def test_math_all_41_ops_and_clamp(self):
        self.assertEqual(len(procedural_expr._MATH_OP_MAP), 41)
        val_node = _FakeNode("ShaderNodeValue", outputs=[_FakeSocket("Value", "VALUE", 0.35)])
        for op_name, op_id in procedural_expr._MATH_OP_MAP.items():
            for clamp in (False, True):
                m = _FakeNode(
                    "ShaderNodeMath",
                    inputs=[
                        _FakeSocket("Value", "VALUE", 0.5),
                        _FakeSocket("Value", "VALUE", 0.25),
                        _FakeSocket("Value", "VALUE", 0.1),
                    ],
                    outputs=[_FakeSocket("Value", "VALUE")],
                    operation=op_name,
                    use_clamp=clamp,
                )
                _link(val_node, "Value", m, 0)
                res = _compile_node(m, "Value", want_type="scalar")
                self.assertIsNotNone(res, f"Math {op_name} clamp={clamp} failed")
                self.assertIn(f"osl_math({op_id},", res.expression)
                if clamp:
                    self.assertIn("osl_clamp(0,", res.expression)
        # Unknown operation fails closed
        bad = _FakeNode("ShaderNodeMath", outputs=[_FakeSocket("Value", "VALUE")], operation="UNKNOWN_OP")
        self.assertIsNone(_compile_node(bad, "Value", want_type="scalar"))

    def test_vector_math_all_30_ops(self):
        self.assertEqual(len(procedural_expr._VECTOR_MATH_OP_MAP), 30)
        tc = _FakeNode("ShaderNodeTexCoord", outputs=[_FakeSocket("Object", "VECTOR")])
        for op_name, op_id in procedural_expr._VECTOR_MATH_OP_MAP.items():
            vm = _FakeNode(
                "ShaderNodeVectorMath",
                inputs=[
                    _FakeSocket("Vector", "VECTOR", (1.0, 2.0, 3.0)),
                    _FakeSocket("Vector", "VECTOR", (0.5, -1.0, 2.0)),
                    _FakeSocket("Vector", "VECTOR", (0.0, 1.0, 0.0)),
                    _FakeSocket("Scale", "VALUE", 2.5),
                ],
                outputs=[_FakeSocket("Vector", "VECTOR"), _FakeSocket("Value", "VALUE")],
                operation=op_name,
            )
            _link(tc, "Object", vm, 0)
            if op_id in (7, 8, 9):
                res = _compile_node(vm, "Value", want_type="scalar")
                self.assertIsNotNone(res, f"VectorMath scalar {op_name} failed")
                self.assertIn(f"osl_vmath_scalar({op_id},", res.expression)
            else:
                res = _compile_node(vm, "Vector", want_type="vec3")
                self.assertIsNotNone(res, f"VectorMath vector {op_name} failed")
                self.assertIn(f"osl_vmath({op_id},", res.expression)

    def test_clamp_minmax_and_range(self):
        for ctype, cid in (("MINMAX", 0), ("RANGE", 1)):
            c = _FakeNode(
                "ShaderNodeClamp",
                inputs=[
                    _FakeSocket("Value", "VALUE", 1.5),
                    _FakeSocket("Min", "VALUE", 0.2),
                    _FakeSocket("Max", "VALUE", 0.8),
                ],
                outputs=[_FakeSocket("Result", "VALUE")],
                clamp_type=ctype,
            )
            res = _compile_node(c, "Result", want_type="scalar")
            self.assertIsNotNone(res)
            self.assertIn(f"osl_clamp({cid},", res.expression)

    def test_map_range_float_and_vector_all_modes(self):
        for itype, iid in (("LINEAR", 0), ("STEPPED", 1), ("SMOOTHSTEP", 2), ("SMOOTHERSTEP", 3)):
            for clamp in (False, True):
                mr_f = _FakeNode(
                    "ShaderNodeMapRange",
                    inputs=[
                        _FakeSocket("Value", "VALUE", 0.5),
                        _FakeSocket("From Min", "VALUE", 0.0),
                        _FakeSocket("From Max", "VALUE", 1.0),
                        _FakeSocket("To Min", "VALUE", 0.2),
                        _FakeSocket("To Max", "VALUE", 0.9),
                        _FakeSocket("Steps", "VALUE", 4.0),
                        _FakeSocket("Vector", "VECTOR", (0.5, 0.5, 0.5)),
                        _FakeSocket("From Min", "VECTOR", (0.0, 0.0, 0.0)),
                        _FakeSocket("From Max", "VECTOR", (1.0, 1.0, 1.0)),
                        _FakeSocket("To Min", "VECTOR", (0.0, 0.0, 0.0)),
                        _FakeSocket("To Max", "VECTOR", (1.0, 1.0, 1.0)),
                        _FakeSocket("Steps", "VECTOR", (4.0, 4.0, 4.0)),
                    ],
                    outputs=[_FakeSocket("Result", "VALUE"), _FakeSocket("Vector", "VECTOR")],
                    data_type="FLOAT",
                    interpolation_type=itype,
                    clamp=clamp,
                )
                res_f = _compile_node(mr_f, "Result", want_type="scalar")
                self.assertIsNotNone(res_f)
                self.assertIn(f"osl_map_range({iid}, {1 if clamp else 0},", res_f.expression)

                mr_f.data_type = "FLOAT_VECTOR"
                res_v = _compile_node(mr_f, "Vector", want_type="vec3")
                self.assertIsNotNone(res_v)
                self.assertIn(f"osl_vmap_range({iid}, {1 if clamp else 0},", res_v.expression)

    def test_vector_rotate_all_modes(self):
        tc = _FakeNode("ShaderNodeTexCoord", outputs=[_FakeSocket("Object", "VECTOR")])
        for rtype, rid in (("AXIS_ANGLE", 0), ("X_AXIS", 1), ("Y_AXIS", 2), ("Z_AXIS", 3), ("EULER_XYZ", 4)):
            for inv in (False, True):
                vr = _FakeNode(
                    "ShaderNodeVectorRotate",
                    inputs=[
                        _FakeSocket("Vector", "VECTOR", (1.0, 0.0, 0.0)),
                        _FakeSocket("Center", "VECTOR", (0.1, 0.2, 0.3)),
                        _FakeSocket("Axis", "VECTOR", (0.0, 0.0, 1.0)),
                        _FakeSocket("Angle", "VALUE", 1.57),
                        _FakeSocket("Rotation", "VECTOR", (0.1, 0.2, 0.3)),
                    ],
                    outputs=[_FakeSocket("Vector", "VECTOR")],
                    rotation_type=rtype,
                    invert=inv,
                )
                _link(tc, "Object", vr, "Vector")
                res = _compile_node(vr, "Vector", want_type="vec3")
                self.assertIsNotNone(res)
                self.assertIn(f"osl_vector_rotate({rid}, {1 if inv else 0},", res.expression)

    def test_vector_transform_all_types_and_spaces(self):
        tc = _FakeNode("ShaderNodeTexCoord", outputs=[_FakeSocket("Object", "VECTOR")])
        spaces = ("WORLD", "OBJECT", "CAMERA")
        for vtype in ("POINT", "VECTOR", "NORMAL"):
            for cfrom in spaces:
                for cto in spaces:
                    vt = _FakeNode(
                        "ShaderNodeVectorTransform",
                        inputs=[_FakeSocket("Vector", "VECTOR", (1.0, 2.0, 3.0))],
                        outputs=[_FakeSocket("Vector", "VECTOR")],
                        vector_type=vtype,
                        convert_from=cfrom,
                        convert_to=cto,
                    )
                    _link(tc, "Object", vt, "Vector")
                    res = _compile_node(vt, "Vector", want_type="vec3")
                    self.assertIsNotNone(res, f"VectorTransform {vtype} {cfrom}->{cto} failed")
                    k = vtype.lower()
                    if cfrom == cto:
                        self.assertEqual(res.expression, "Po")
                    elif cfrom == "WORLD" and cto == "OBJECT":
                        self.assertEqual(res.expression, f"osl_to_object_{k}(Po)")
                    elif cfrom == "WORLD" and cto == "CAMERA":
                        self.assertEqual(res.expression, f"osl_to_camera_{k}(Po)")
                    elif cfrom == "OBJECT" and cto == "WORLD":
                        self.assertEqual(res.expression, f"osl_to_world_{k}(Po)")
                    elif cfrom == "CAMERA" and cto == "WORLD":
                        self.assertEqual(res.expression, f"osl_camera_to_world_{k}(Po)")
                    elif cfrom == "OBJECT" and cto == "CAMERA":
                        self.assertEqual(res.expression, f"osl_to_camera_{k}(osl_to_world_{k}(Po))")
                    elif cfrom == "CAMERA" and cto == "OBJECT":
                        self.assertEqual(res.expression, f"osl_to_object_{k}(osl_camera_to_world_{k}(Po))")

    def test_rgb_to_bw_and_mix_non_uniform_vector(self):
        rgb = _FakeNode("ShaderNodeRGB", outputs=[_FakeSocket("Color", "RGBA", (0.2, 0.5, 0.8, 1.0))])
        bw = _FakeNode(
            "ShaderNodeRGBToBW",
            inputs=[_FakeSocket("Color", "RGBA", (0.0, 0.0, 0.0, 1.0))],
            outputs=[_FakeSocket("Val", "VALUE")],
        )
        _link(rgb, "Color", bw, "Color")
        res_bw = _compile_node(bw, "Val", want_type="scalar")
        self.assertIsNotNone(res_bw)
        self.assertIn("dot(", res_bw.expression)

        mx = _FakeNode(
            "ShaderNodeMix",
            inputs=[
                _FakeSocket("Factor", "VECTOR", (0.2, 0.5, 0.8)),
                _FakeSocket("A", "VECTOR", (0.0, 0.0, 0.0)),
                _FakeSocket("B", "VECTOR", (1.0, 2.0, 3.0)),
            ],
            outputs=[_FakeSocket("Result", "VECTOR")],
            data_type="VECTOR",
            factor_mode="NON_UNIFORM",
            clamp_factor=True,
        )
        res_mx = _compile_node(mx, "Result", want_type="vec3")
        self.assertIsNotNone(res_mx)
        self.assertIn("osl_vclamp(", res_mx.expression)

    def test_color_ramp_ease_hsv_hsl_and_alpha(self):
        tc = _FakeNode("ShaderNodeTexCoord", outputs=[_FakeSocket("Object", "VECTOR")])
        sep = _FakeNode(
            "ShaderNodeSeparateXYZ",
            inputs=[_FakeSocket("Vector", "VECTOR")],
            outputs=[_FakeSocket("X", "VALUE")],
        )
        _link(tc, "Object", sep, "Vector")

        # RGB EASE + Alpha
        ramp_ease = _FakeNode(
            "ShaderNodeValToRGB",
            inputs=[_FakeSocket("Fac", "VALUE", 0.5)],
            outputs=[_FakeSocket("Color", "RGBA"), _FakeSocket("Alpha", "VALUE")],
            color_ramp=types.SimpleNamespace(
                color_mode="RGB",
                interpolation="EASE",
                elements=[
                    types.SimpleNamespace(position=0.0, color=(1.0, 0.0, 0.0, 0.25)),
                    types.SimpleNamespace(position=1.0, color=(0.0, 0.0, 1.0, 0.75)),
                ],
            ),
        )
        _link(sep, "X", ramp_ease, "Fac")
        res_c = _compile_node(ramp_ease, "Color", want_type="vec3")
        res_a = _compile_node(ramp_ease, "Alpha", want_type="scalar")
        self.assertIsNotNone(res_c)
        self.assertIsNotNone(res_a)
        self.assertIn("smoothstep(0, 1,", res_c.expression)
        self.assertIn("smoothstep(0, 1,", res_a.expression)

        # HSV and HSL with all 4 hue interpolation modes — verify distinct hue wrapping!
        # Stop 0 = (1.0, 0.1, 0.0) has hue ~0.0167, Stop 1 = (0.1, 0.0, 1.0) has hue ~0.6833 (h2 - h1 = 0.667 > 0.5).
        # NEAR & CW wrap h1+1 (emit `frac(mix(1.016`), while FAR & CCW interpolate directly (no `frac(`).
        for cmode, fn in (("HSV", "osl_hsv2rgb"), ("HSL", "osl_hsl2rgb")):
            exprs_by_mode = {}
            for hmode in ("NEAR", "FAR", "CW", "CCW"):
                ramp_h = _FakeNode(
                    "ShaderNodeValToRGB",
                    inputs=[_FakeSocket("Fac", "VALUE", 0.5)],
                    outputs=[_FakeSocket("Color", "RGBA")],
                    color_ramp=types.SimpleNamespace(
                        color_mode=cmode,
                        interpolation="LINEAR",
                        hue_interpolation=hmode,
                        elements=[
                            types.SimpleNamespace(position=0.0, color=(1.0, 0.1, 0.0, 1.0)),
                            types.SimpleNamespace(position=1.0, color=(0.1, 0.0, 1.0, 1.0)),
                        ],
                    ),
                )
                _link(sep, "X", ramp_h, "Fac")
                res_h = _compile_node(ramp_h, "Color", want_type="vec3")
                self.assertIsNotNone(res_h, f"ColorRamp {cmode}/{hmode} failed")
                self.assertIn(fn, res_h.expression)
                exprs_by_mode[hmode] = res_h.expression
            self.assertIn("frac(mix(1.0", exprs_by_mode["NEAR"])
            self.assertIn("frac(mix(1.0", exprs_by_mode["CW"])
            self.assertNotIn("frac(", exprs_by_mode["FAR"])
            self.assertNotIn("frac(", exprs_by_mode["CCW"])
            self.assertNotEqual(exprs_by_mode["NEAR"], exprs_by_mode["FAR"])

        # R1 P1-4: HDR / negative ColorRamp HSL stops where (2 - cmax - cmin) == 0 or (cmax + cmin) == 0
        ramp_hdr = _FakeNode(
            "ShaderNodeValToRGB",
            inputs=[_FakeSocket("Fac", "VALUE", 0.5)],
            outputs=[_FakeSocket("Color", "RGBA")],
            color_ramp=types.SimpleNamespace(
                color_mode="HSL",
                interpolation="LINEAR",
                hue_interpolation="NEAR",
                elements=[
                    types.SimpleNamespace(position=0.0, color=(2.0, 0.0, 0.0, 1.0)),
                    types.SimpleNamespace(position=1.0, color=(-0.5, 0.5, 0.0, 1.0)),
                ],
            ),
        )
        _link(sep, "X", ramp_hdr, "Fac")
        res_hdr = _compile_node(ramp_hdr, "Color", want_type="vec3")
        self.assertIsNotNone(res_hdr, "HSL ColorRamp with HDR/negative stops must not raise ZeroDivisionError")

        # R1 P2-8: 48-stop ColorRamp splits into shallow chunks well below kMaxParseDepth=200
        ramp_48 = _FakeNode(
            "ShaderNodeValToRGB",
            inputs=[_FakeSocket("Fac", "VALUE", 0.5)],
            outputs=[_FakeSocket("Color", "RGBA")],
            color_ramp=types.SimpleNamespace(
                color_mode="RGB",
                interpolation="LINEAR",
                hue_interpolation="NEAR",
                elements=[
                    types.SimpleNamespace(position=i / 47.0, color=(i / 47.0, 0.2, 1.0 - i / 47.0, 1.0))
                    for i in range(48)
                ],
            ),
        )
        _link(sep, "X", ramp_48, "Fac")
        res_48 = _compile_node(ramp_48, "Color", want_type="vec3")
        self.assertIsNotNone(res_48)
        self.assertLessEqual(res_48.max_expr_depth, 24)

    def test_reroute_preserves_vector_coercion_and_deep_chain(self):
        # R1 P1-5: VECTOR -> Reroute -> Scalar must use channel average (0.3333...), NOT Rec.709 luminance!
        tc = _FakeNode("ShaderNodeTexCoord", outputs=[_FakeSocket("Object", "VECTOR")])
        rr = _FakeNode("NodeReroute", inputs=[_FakeSocket("Input", "RGBA")], outputs=[_FakeSocket("Output", "RGBA")])
        _link(tc, "Object", rr, "Input")
        res = _compile_node(rr, "Output", want_type="scalar")
        self.assertIsNotNone(res)
        self.assertIn("0.3333333333333333", res.expression)
        self.assertNotIn("0.2126", res.expression)

        # 600 chained reroutes must unwrap iteratively without hitting Python recursion limit or max_depth
        prev = tc
        prev_out = "Object"
        for _ in range(600):
            r = _FakeNode("NodeReroute", inputs=[_FakeSocket("Input", "RGBA")], outputs=[_FakeSocket("Output", "RGBA")])
            _link(prev, prev_out, r, "Input")
            prev, prev_out = r, "Output"
        res_600 = _compile_node(prev, prev_out, want_type="scalar")
        self.assertIsNotNone(res_600)
        self.assertIn("0.3333333333333333", res_600.expression)

    def test_inactive_socket_pruning_and_vector_math_validation(self):
        # R1 P2-5: Unary Math (SINE) ignores an unsupported node wired to hidden input 1
        bad_node = _FakeNode("ShaderNodeBsdfPrincipled", outputs=[_FakeSocket("BSDF", "SHADER")])
        m = _FakeNode(
            "ShaderNodeMath",
            inputs=[
                _FakeSocket("Value", "VALUE", 0.5),
                _FakeSocket("Value", "VALUE", 0.0),
                _FakeSocket("Value", "VALUE", 0.0),
            ],
            outputs=[_FakeSocket("Value", "VALUE")],
            operation="SINE",
        )
        _link(bad_node, "BSDF", m, 1)
        res_sin = _compile_node(m, "Value", want_type="scalar")
        self.assertIsNotNone(res_sin, "Unary Math should ignore dead linked input 1")
        self.assertIn("osl_math(4, 0.5, 0, 0)", res_sin.expression)

        # R1 P2-6: VectorMath scalar op rejects requesting Vector output; REFRACT reads IOR socket
        vm_dot = _FakeNode(
            "ShaderNodeVectorMath",
            inputs=[_FakeSocket("Vector", "VECTOR", (1, 0, 0)), _FakeSocket("Vector", "VECTOR", (0, 1, 0))],
            outputs=[_FakeSocket("Vector", "VECTOR"), _FakeSocket("Value", "VALUE")],
            operation="DOT_PRODUCT",
        )
        self.assertIsNone(_compile_node(vm_dot, "Vector", want_type="vec3"))

        vm_ref = _FakeNode(
            "ShaderNodeVectorMath",
            inputs=[
                _FakeSocket("Vector", "VECTOR", (0, -1, 0)),
                _FakeSocket("Vector", "VECTOR", (0, 1, 0)),
                _FakeSocket("IOR", "VALUE", 1.45),
            ],
            outputs=[_FakeSocket("Vector", "VECTOR")],
            operation="REFRACT",
        )
        res_ref = _compile_node(vm_ref, "Vector", want_type="vec3")
        self.assertIsNotNone(res_ref)
        self.assertIn("1.45", res_ref.expression)

        # R2 P2-4: VectorRotate and non-STEPPED MapRange prune dead links on inactive sockets
        vr_eul = _FakeNode(
            "ShaderNodeVectorRotate",
            inputs=[
                _FakeSocket("Vector", "VECTOR", (1, 0, 0)),
                _FakeSocket("Center", "VECTOR", (0, 0, 0)),
                _FakeSocket("Axis", "VECTOR", (0, 0, 1)),
                _FakeSocket("Angle", "VALUE", 0.5),
                _FakeSocket("Rotation", "VECTOR", (0.1, 0.2, 0.3)),
            ],
            outputs=[_FakeSocket("Vector", "VECTOR")],
            rotation_type="EULER_XYZ",
        )
        _link(bad_node, "BSDF", vr_eul, "Axis")
        _link(bad_node, "BSDF", vr_eul, "Angle")
        self.assertIsNotNone(_compile_node(vr_eul, "Vector", want_type="vec3"))

        mr_lin = _FakeNode(
            "ShaderNodeMapRange",
            inputs=[
                _FakeSocket("Value", "VALUE", 0.5),
                _FakeSocket("From Min", "VALUE", 0.0),
                _FakeSocket("From Max", "VALUE", 1.0),
                _FakeSocket("To Min", "VALUE", 0.0),
                _FakeSocket("To Max", "VALUE", 1.0),
                _FakeSocket("Steps", "VALUE", 4.0),
            ],
            outputs=[_FakeSocket("Result", "VALUE")],
            data_type="FLOAT",
            interpolation_type="LINEAR",
        )
        _link(bad_node, "BSDF", mr_lin, "Steps")
        self.assertIsNotNone(_compile_node(mr_lin, "Result", want_type="scalar"))

    def test_noise_dual_output_sharing_fac_and_color(self):
        noise = _FakeNode(
            "ShaderNodeTexNoise",
            inputs=[
                _FakeSocket("Vector", "VECTOR", (0.0, 0.0, 0.0)),
                _FakeSocket("W", "VALUE", 0.0),
                _FakeSocket("Scale", "VALUE", 4.0),
                _FakeSocket("Detail", "VALUE", 3.0),
                _FakeSocket("Roughness", "VALUE", 0.5),
                _FakeSocket("Lacunarity", "VALUE", 2.0),
                _FakeSocket("Offset", "VALUE", 0.0),
                _FakeSocket("Gain", "VALUE", 1.0),
                _FakeSocket("Distortion", "VALUE", 0.0),
            ],
            outputs=[_FakeSocket("Fac", "VALUE"), _FakeSocket("Color", "RGBA")],
            noise_dimensions="3D",
            noise_type="FBM",
            normalize=True,
        )
        # Mix using Color as A and Fac as Factor -> should emit ONE osl_noise_texture_color def and read .x!
        mx = _FakeNode(
            "ShaderNodeMix",
            inputs=[
                _FakeSocket("Factor", "VALUE", 0.5),
                _FakeSocket("A", "RGBA", (0.0, 0.0, 0.0, 1.0)),
                _FakeSocket("B", "RGBA", (1.0, 1.0, 1.0, 1.0)),
            ],
            outputs=[_FakeSocket("Result", "RGBA")],
            data_type="RGBA",
            blend_type="MIX",
        )
        _link(noise, "Fac", mx, "Factor", dst_type="VALUE")
        _link(noise, "Color", mx, "A", dst_type="RGBA")
        res = _compile_node(mx, "Result", want_type="vec3")
        self.assertIsNotNone(res)
        self.assertEqual(res.expression.count("osl_noise_texture_color("), 1)
        self.assertEqual(res.expression.count("osl_noise_texture("), 0)
        self.assertIn(".x", res.expression)

    def test_single_use_inlining_and_250_node_chain_splitting(self):
        # 250-node linear Math chain: exceeds RISE's kMaxParseDepth (200) if
        # fully inlined, and would spend 250 def slots if naively bound.
        # With single-use inlining + split at depth 24, it uses ~10 defs and
        # keeps max_expr_depth <= 25!
        val = _FakeNode("ShaderNodeValue", outputs=[_FakeSocket("Value", "VALUE", 0.1)])
        prev = val
        for i in range(250):
            m = _FakeNode(
                "ShaderNodeMath",
                inputs=[
                    _FakeSocket("Value", "VALUE", 0.0),
                    _FakeSocket("Value", "VALUE", 0.01),
                    _FakeSocket("Value", "VALUE", 0.0),
                ],
                outputs=[_FakeSocket("Value", "VALUE")],
                operation="ADD",
                use_clamp=False,
            )
            _link(prev, "Value", m, 0)
            prev = m
        res_inline = _compile_node(prev, "Value", want_type="scalar", inline_single_use=True)
        res_naive = _compile_node(prev, "Value", want_type="scalar", inline_single_use=False)
        self.assertIsNotNone(res_inline)
        self.assertIsNotNone(res_naive)
        self.assertLessEqual(res_inline.max_expr_depth, 25)
        self.assertLess(res_inline.def_slots, 20)
        self.assertEqual(res_naive.def_slots, 250)

    def test_classifier_admits_groups_5_to_7(self):
        for bl in (
            "ShaderNodeMath",
            "ShaderNodeVectorMath",
            "ShaderNodeClamp",
            "ShaderNodeMapRange",
            "ShaderNodeVectorRotate",
            "ShaderNodeVectorTransform",
            "ShaderNodeRGBToBW",
        ):
            self.assertIn(bl, material_bake._SIMPLE_TRAVERSABLE_NODES)

    def test_rise_vm_parses_all_generated_expressions(self):
        rise_bin = os.path.join(_REPO_ROOT, "bin", "rise")
        if not os.path.isfile(rise_bin):
            self.skipTest("bin/rise not built")

        tc = _FakeNode(
            "ShaderNodeTexCoord",
            outputs=[
                _FakeSocket("Object", "VECTOR"),
                _FakeSocket("Normal", "VECTOR"),
                _FakeSocket("Camera", "VECTOR"),
            ],
        )
        # Build a rich multi-group graph exercising all 7 groups + 250-node chain
        samples = {}

        # 1. Math + Clamp + MapRange + VectorRotate + VectorTransform + ColorRamp HSV
        vt = _FakeNode(
            "ShaderNodeVectorTransform",
            inputs=[_FakeSocket("Vector", "VECTOR")],
            outputs=[_FakeSocket("Vector", "VECTOR")],
            vector_type="POINT",
            convert_from="OBJECT",
            convert_to="CAMERA",
        )
        _link(tc, "Object", vt, "Vector")
        vr = _FakeNode(
            "ShaderNodeVectorRotate",
            inputs=[
                _FakeSocket("Vector", "VECTOR"),
                _FakeSocket("Center", "VECTOR", (0.0, 0.0, 0.0)),
                _FakeSocket("Axis", "VECTOR", (0.0, 0.0, 1.0)),
                _FakeSocket("Angle", "VALUE", 0.7),
                _FakeSocket("Rotation", "VECTOR", (0.1, 0.2, 0.3)),
            ],
            outputs=[_FakeSocket("Vector", "VECTOR")],
            rotation_type="EULER_XYZ",
            invert=True,
        )
        _link(vt, "Vector", vr, "Vector")
        vm = _FakeNode(
            "ShaderNodeVectorMath",
            inputs=[
                _FakeSocket("Vector", "VECTOR"),
                _FakeSocket("Vector", "VECTOR", (0.0, 0.0, 1.0)),
            ],
            outputs=[_FakeSocket("Vector", "VECTOR"), _FakeSocket("Value", "VALUE")],
            operation="DOT_PRODUCT",
        )
        _link(vr, "Vector", vm, 0)
        mr = _FakeNode(
            "ShaderNodeMapRange",
            inputs=[
                _FakeSocket("Value", "VALUE"),
                _FakeSocket("From Min", "VALUE", -1.0),
                _FakeSocket("From Max", "VALUE", 1.0),
                _FakeSocket("To Min", "VALUE", 0.0),
                _FakeSocket("To Max", "VALUE", 1.0),
                _FakeSocket("Steps", "VALUE", 4.0),
            ],
            outputs=[_FakeSocket("Result", "VALUE")],
            data_type="FLOAT",
            interpolation_type="SMOOTHSTEP",
            clamp=True,
        )
        _link(vm, "Value", mr, "Value", dst_type="VALUE")
        ramp = _FakeNode(
            "ShaderNodeValToRGB",
            inputs=[_FakeSocket("Fac", "VALUE")],
            outputs=[_FakeSocket("Color", "RGBA")],
            color_ramp=types.SimpleNamespace(
                color_mode="HSV",
                interpolation="LINEAR",
                hue_interpolation="NEAR",
                elements=[
                    types.SimpleNamespace(position=0.0, color=(1.0, 0.1, 0.0, 1.0)),
                    types.SimpleNamespace(position=1.0, color=(0.0, 0.2, 1.0, 1.0)),
                ],
            ),
        )
        _link(mr, "Result", ramp, "Fac")
        samples["p_all_groups"] = _compile_node(ramp, "Color", want_type="vec3")

        # 2. 250-node deep Math chain (proves depth-split survives kMaxParseDepth=200!)
        prev = _FakeNode("ShaderNodeValue", outputs=[_FakeSocket("Value", "VALUE", 0.1)])
        for _ in range(250):
            m = _FakeNode(
                "ShaderNodeMath",
                inputs=[
                    _FakeSocket("Value", "VALUE", 0.0),
                    _FakeSocket("Value", "VALUE", 0.002),
                    _FakeSocket("Value", "VALUE", 0.0),
                ],
                outputs=[_FakeSocket("Value", "VALUE")],
                operation="ADD",
                use_clamp=False,
            )
            _link(prev, "Value", m, 0)
            prev = m
        samples["p_deep_250"] = _compile_node(prev, "Value", want_type="scalar")

        # 3. Dynamic Mapping + Noise dual output
        mp = _FakeNode(
            "ShaderNodeMapping",
            inputs=[
                _FakeSocket("Vector", "VECTOR"),
                _FakeSocket("Location", "VECTOR"),
                _FakeSocket("Rotation", "VECTOR", (0.1, 0.2, 0.3)),
                _FakeSocket("Scale", "VECTOR", (2.0, 2.0, 2.0)),
            ],
            outputs=[_FakeSocket("Vector", "VECTOR")],
            vector_type="POINT",
        )
        _link(tc, "Camera", mp, "Vector")
        _link(tc, "Normal", mp, "Location")
        noise = _FakeNode(
            "ShaderNodeTexNoise",
            inputs=[
                _FakeSocket("Vector", "VECTOR"),
                _FakeSocket("W", "VALUE", 0.0),
                _FakeSocket("Scale", "VALUE", 3.0),
                _FakeSocket("Detail", "VALUE", 2.0),
                _FakeSocket("Roughness", "VALUE", 0.5),
                _FakeSocket("Lacunarity", "VALUE", 2.0),
                _FakeSocket("Offset", "VALUE", 0.0),
                _FakeSocket("Gain", "VALUE", 1.0),
                _FakeSocket("Distortion", "VALUE", 0.1),
            ],
            outputs=[_FakeSocket("Fac", "VALUE"), _FakeSocket("Color", "RGBA")],
            noise_dimensions="3D",
            noise_type="FBM",
            normalize=True,
        )
        _link(mp, "Vector", noise, "Vector")
        mx = _FakeNode(
            "ShaderNodeMix",
            inputs=[
                _FakeSocket("Factor", "VALUE"),
                _FakeSocket("A", "RGBA"),
                _FakeSocket("B", "RGBA", (0.2, 0.4, 0.8, 1.0)),
            ],
            outputs=[_FakeSocket("Result", "RGBA")],
            data_type="RGBA",
            blend_type="OVERLAY",
        )
        _link(noise, "Fac", mx, "Factor", dst_type="VALUE")
        _link(noise, "Color", mx, "A", dst_type="RGBA")
        samples["p_dyn_noise_dual"] = _compile_node(mx, "Result", want_type="vec3")

        # 4. Groups 2, 3, 5, 6 coverage: WhiteNoise + Voronoi + Clamp + MapRange(FLOAT_VECTOR) + Mix(NON_UNIFORM) + RGBToBW + 48-stop Ramp
        wn = _FakeNode(
            "ShaderNodeTexWhiteNoise",
            inputs=[_FakeSocket("Vector", "VECTOR"), _FakeSocket("W", "VALUE", 0.0)],
            outputs=[_FakeSocket("Value", "VALUE"), _FakeSocket("Color", "RGBA")],
            noise_dimensions="3D",
        )
        _link(tc, "Object", wn, "Vector")
        vor = _FakeNode(
            "ShaderNodeTexVoronoi",
            inputs=[
                _FakeSocket("Vector", "VECTOR"),
                _FakeSocket("W", "VALUE", 0.0),
                _FakeSocket("Scale", "VALUE", 5.0),
                _FakeSocket("Detail", "VALUE", 0.0),
                _FakeSocket("Roughness", "VALUE", 0.5),
                _FakeSocket("Lacunarity", "VALUE", 2.0),
                _FakeSocket("Smoothness", "VALUE", 1.0),
                _FakeSocket("Exponent", "VALUE", 0.5),
                _FakeSocket("Randomness", "VALUE", 1.0),
            ],
            outputs=[_FakeSocket("Distance", "VALUE"), _FakeSocket("Color", "RGBA")],
            voronoi_dimensions="3D",
            feature="F1",
            distance="EUCLIDEAN",
            normalize=False,
        )
        _link(tc, "Object", vor, "Vector")
        mrv = _FakeNode(
            "ShaderNodeMapRange",
            inputs=[
                _FakeSocket("Vector", "VECTOR"),
                _FakeSocket("From Min", "VECTOR", (0, 0, 0)),
                _FakeSocket("From Max", "VECTOR", (1, 1, 1)),
                _FakeSocket("To Min", "VECTOR", (0.1, 0.1, 0.1)),
                _FakeSocket("To Max", "VECTOR", (0.9, 0.9, 0.9)),
                _FakeSocket("Steps", "VECTOR", (4, 4, 4)),
            ],
            outputs=[_FakeSocket("Vector", "VECTOR")],
            data_type="FLOAT_VECTOR",
            interpolation_type="SMOOTHERSTEP",
            clamp=True,
        )
        _link(wn, "Color", mrv, "Vector")
        mx_nu = _FakeNode(
            "ShaderNodeMix",
            inputs=[
                _FakeSocket("Factor", "VECTOR"),
                _FakeSocket("A", "VECTOR"),
                _FakeSocket("B", "VECTOR", (1.0, 0.5, 0.2)),
            ],
            outputs=[_FakeSocket("Result", "VECTOR")],
            data_type="VECTOR",
            factor_mode="NON_UNIFORM",
            clamp_factor=True,
        )
        _link(mrv, "Vector", mx_nu, "Factor")
        _link(vor, "Color", mx_nu, "A")
        bw = _FakeNode("ShaderNodeRGBToBW", inputs=[_FakeSocket("Color", "RGBA")], outputs=[_FakeSocket("Val", "VALUE")])
        _link(mx_nu, "Result", bw, "Color")
        cl = _FakeNode(
            "ShaderNodeClamp",
            inputs=[_FakeSocket("Value", "VALUE"), _FakeSocket("Min", "VALUE", 0.0), _FakeSocket("Max", "VALUE", 1.0)],
            outputs=[_FakeSocket("Result", "VALUE")],
            clamp_type="RANGE",
        )
        _link(bw, "Val", cl, "Value")
        ramp48 = _FakeNode(
            "ShaderNodeValToRGB",
            inputs=[_FakeSocket("Fac", "VALUE")],
            outputs=[_FakeSocket("Color", "RGBA")],
            color_ramp=types.SimpleNamespace(
                color_mode="RGB",
                interpolation="EASE",
                hue_interpolation="NEAR",
                elements=[
                    types.SimpleNamespace(position=i / 47.0, color=(i / 47.0, 0.3, 1.0 - i / 47.0, 1.0))
                    for i in range(48)
                ],
            ),
        )
        _link(cl, "Result", ramp48, "Fac")
        samples["p_groups_2356_ramp48"] = _compile_node(ramp48, "Color", want_type="vec3")

        scene_parts = ["RISE ASCII SCENE 7", ""]
        for name, comp in samples.items():
            self.assertIsNotNone(comp, f"{name} failed to compile")
            scene_parts.append(_format_rise_painter_chunk(name, comp))
        scene_text = "\n\n".join(scene_parts) + "\n"

        with tempfile.NamedTemporaryFile("w", suffix=".RISEscene", delete=False) as tf:
            tf.write(scene_text)
            scene_path = tf.name
        try:
            env = os.environ.copy()
            env["RISE_MEDIA_PATH"] = _REPO_ROOT + os.sep
            proc = subprocess.run(
                [rise_bin, scene_path],
                input="quit\n",
                text=True,
                capture_output=True,
                env=env,
                timeout=30,
            )
            combined = proc.stdout + "\n" + proc.stderr
            self.assertEqual(proc.returncode, 0, f"bin/rise exited {proc.returncode}:\n{combined}")
            self.assertNotIn("failed to apply", combined, f"RISE failed to parse expression:\n{combined}")
            self.assertNotIn("expression nesting too deep", combined)
            self.assertNotIn("unknown function", combined)
            self.assertNotIn("unknown variable", combined)
        finally:
            os.unlink(scene_path)


class Blender53RenamesObjectInfoAndBumpTest(unittest.TestCase):
    def test_blender53_noise_factor_and_val_to_rgb_factor(self):
        # Blender 5.3 renamed Noise.Fac -> "Factor" and ValToRGB.Fac -> "Factor"
        noise = _FakeNode(
            "ShaderNodeTexNoise",
            inputs=[
                _FakeSocket("Vector", "VECTOR", (0.0, 0.0, 0.0)),
                _FakeSocket("W", "VALUE", 0.0),
                _FakeSocket("Scale", "VALUE", 5.0),
                _FakeSocket("Detail", "VALUE", 2.0),
                _FakeSocket("Roughness", "VALUE", 0.5),
                _FakeSocket("Lacunarity", "VALUE", 2.0),
                _FakeSocket("Offset", "VALUE", 0.0),
                _FakeSocket("Gain", "VALUE", 1.0),
                _FakeSocket("Distortion", "VALUE", 0.0),
            ],
            outputs=[
                _FakeSocket("Factor", "VALUE"),
                _FakeSocket("Color", "RGBA"),
            ],
            noise_dimensions="3D",
            noise_type="FBM",
            normalize=True,
        )
        ramp = _FakeNode(
            "ShaderNodeValToRGB",
            inputs=[_FakeSocket("Factor", "VALUE", 0.5)],
            outputs=[_FakeSocket("Color", "RGBA"), _FakeSocket("Alpha", "VALUE")],
            color_ramp=types.SimpleNamespace(
                color_mode="RGB",
                interpolation="LINEAR",
                elements=[
                    types.SimpleNamespace(position=0.2, color=(0.1, 0.2, 0.3, 1.0)),
                    types.SimpleNamespace(position=0.8, color=(0.9, 0.8, 0.7, 1.0)),
                ],
            ),
        )
        _link(noise, "Factor", ramp, "Factor")
        dst = _FakeSocket("Base Color", "RGBA")
        dst.links.append(_FakeLink(ramp, ramp.outputs["Color"]))
        res = procedural_expr.compile_socket_expression(dst, want_type="vec3")
        self.assertIsNotNone(res)
        self.assertIn("osl_noise_texture(", res.expression)

    def test_blender53_noise_dual_output_sharing_factor_and_color(self):
        # Using both "Factor" and "Color" from the same Noise node must lower to
        # ONE osl_noise_texture_color() call with Factor reading `.x`.
        noise = _FakeNode(
            "ShaderNodeTexNoise",
            inputs=[
                _FakeSocket("Vector", "VECTOR", (0.0, 0.0, 0.0)),
                _FakeSocket("W", "VALUE", 0.0),
                _FakeSocket("Scale", "VALUE", 4.0),
                _FakeSocket("Detail", "VALUE", 2.0),
                _FakeSocket("Roughness", "VALUE", 0.5),
                _FakeSocket("Lacunarity", "VALUE", 2.0),
                _FakeSocket("Offset", "VALUE", 0.0),
                _FakeSocket("Gain", "VALUE", 1.0),
                _FakeSocket("Distortion", "VALUE", 0.0),
            ],
            outputs=[
                _FakeSocket("Factor", "VALUE"),
                _FakeSocket("Color", "RGBA"),
            ],
            noise_dimensions="3D",
            noise_type="FBM",
            normalize=True,
        )
        mix = _FakeNode(
            "ShaderNodeMixRGB",
            inputs=[
                _FakeSocket("Factor", "VALUE", 0.5),
                _FakeSocket("Color1", "RGBA", (0.0, 0.0, 0.0, 1.0)),
                _FakeSocket("Color2", "RGBA", (1.0, 1.0, 1.0, 1.0)),
            ],
            outputs=[_FakeSocket("Color", "RGBA")],
            blend_type="MIX",
            use_clamp=False,
        )
        _link(noise, "Factor", mix, "Factor")
        _link(noise, "Color", mix, "Color1")
        dst = _FakeSocket("Base Color", "RGBA")
        dst.links.append(_FakeLink(mix, mix.outputs["Color"]))
        res = procedural_expr.compile_socket_expression(dst, want_type="vec3")
        self.assertIsNotNone(res)
        self.assertEqual(res.expression.count("osl_noise_texture_color("), 1)
        self.assertEqual(res.expression.count("osl_noise_texture("), 0)
        self.assertIn(".x", res.expression)

    def test_blender53_brightness_huesat_invert_renames(self):
        bc = _FakeNode(
            "ShaderNodeBrightContrast",
            inputs=[
                _FakeSocket("Color", "RGBA", (0.5, 0.4, 0.3, 1.0)),
                _FakeSocket("Brightness", "VALUE", 0.15),
                _FakeSocket("Contrast", "VALUE", 0.2),
            ],
            outputs=[_FakeSocket("Color", "RGBA")],
        )
        hsv = _FakeNode(
            "ShaderNodeHueSaturation",
            inputs=[
                _FakeSocket("Hue", "VALUE", 0.5),
                _FakeSocket("Saturation", "VALUE", 1.1),
                _FakeSocket("Value", "VALUE", 0.9),
                _FakeSocket("Factor", "VALUE", 0.75),
                _FakeSocket("Color", "RGBA", (0.5, 0.5, 0.5, 1.0)),
            ],
            outputs=[_FakeSocket("Color", "RGBA")],
        )
        inv = _FakeNode(
            "ShaderNodeInvert",
            inputs=[
                _FakeSocket("Factor", "VALUE", 0.37),
                _FakeSocket("Color", "RGBA", (0.5, 0.5, 0.5, 1.0)),
            ],
            outputs=[_FakeSocket("Color", "RGBA")],
        )
        _link(bc, "Color", hsv, "Color")
        _link(hsv, "Color", inv, "Color")
        dst = _FakeSocket("Base Color", "RGBA")
        dst.links.append(_FakeLink(inv, inv.outputs["Color"]))
        res = procedural_expr.compile_socket_expression(dst, want_type="vec3")
        self.assertIsNotNone(res)
        self.assertIn("0.15", res.expression)
        self.assertIn("0.75", res.expression)
        self.assertIn("0.37", res.expression)

    def test_object_info_all_six_outputs(self):
        oi = _FakeNode(
            "ShaderNodeObjectInfo",
            inputs=[],
            outputs=[
                _FakeSocket("Location", "VECTOR"),
                _FakeSocket("Color", "RGBA"),
                _FakeSocket("Alpha", "VALUE"),
                _FakeSocket("Object Index", "VALUE"),
                _FakeSocket("Material Index", "VALUE"),
                _FakeSocket("Random", "VALUE"),
            ],
        )
        expectations = [
            ("Location", "vec3", "osl_to_world_point(vec3(0, 0, 0))"),
            ("Color", "vec3", "osl_object_color()"),
            ("Alpha", "scalar", "osl_object_alpha()"),
            ("Object Index", "scalar", "osl_object_index()"),
            ("Material Index", "scalar", "7"),
            ("Random", "scalar", "osl_object_random()"),
        ]
        for out_name, want, expected_frag in expectations:
            dst = _FakeSocket("Target", "RGBA" if want == "vec3" else "VALUE")
            dst.links.append(_FakeLink(oi, oi.outputs[out_name]))
            res = procedural_expr.compile_socket_expression(
                dst, want_type=want, material_pass_index=7.0
            )
            self.assertIsNotNone(res, f"ObjectInfo.{out_name} failed to compile")
            self.assertIn(expected_frag, res.expression)

    def test_compile_bump_height_expression(self):
        noise = _FakeNode(
            "ShaderNodeTexNoise",
            inputs=[
                _FakeSocket("Vector", "VECTOR", (0.0, 0.0, 0.0)),
                _FakeSocket("W", "VALUE", 0.0),
                _FakeSocket("Scale", "VALUE", 12.0),
                _FakeSocket("Detail", "VALUE", 3.0),
                _FakeSocket("Roughness", "VALUE", 0.5),
                _FakeSocket("Lacunarity", "VALUE", 2.0),
                _FakeSocket("Offset", "VALUE", 0.0),
                _FakeSocket("Gain", "VALUE", 1.0),
                _FakeSocket("Distortion", "VALUE", 0.0),
            ],
            outputs=[_FakeSocket("Factor", "VALUE"), _FakeSocket("Color", "RGBA")],
            noise_dimensions="3D",
            noise_type="FBM",
            normalize=True,
        )
        bump = _FakeNode(
            "ShaderNodeBump",
            inputs=[
                _FakeSocket("Strength", "VALUE", 0.3),
                _FakeSocket("Distance", "VALUE", 0.1),
                _FakeSocket("Height", "VALUE", 0.0),
                _FakeSocket("Normal", "VECTOR", (0.0, 0.0, 0.0)),
            ],
            outputs=[_FakeSocket("Normal", "VECTOR")],
        )
        # Unlinked Height returns None
        self.assertIsNone(procedural_expr.compile_bump_height_expression(bump))
        # Linked Noise.Factor -> Bump.Height compiles cleanly and flags uses_generated
        _link(noise, "Factor", bump, "Height")
        res = procedural_expr.compile_bump_height_expression(
            bump, bbox_min=(-1.0, -1.0, -1.0), bbox_max=(1.0, 1.0, 1.0)
        )
        self.assertIsNotNone(res)
        self.assertTrue(res.uses_generated)
        self.assertIn("osl_noise_texture(", res.expression)

    def test_object_info_live_rise_parse(self):
        rise_bin = os.path.join(_REPO_ROOT, "bin", "rise")
        if not os.path.isfile(rise_bin):
            self.skipTest("bin/rise not built")
        oi = _FakeNode(
            "ShaderNodeObjectInfo",
            inputs=[],
            outputs=[
                _FakeSocket("Location", "VECTOR"),
                _FakeSocket("Color", "RGBA"),
                _FakeSocket("Alpha", "VALUE"),
                _FakeSocket("Object Index", "VALUE"),
                _FakeSocket("Material Index", "VALUE"),
                _FakeSocket("Random", "VALUE"),
            ],
        )
        ramp = _FakeNode(
            "ShaderNodeValToRGB",
            inputs=[_FakeSocket("Factor", "VALUE", 0.5)],
            outputs=[_FakeSocket("Color", "RGBA")],
            color_ramp=types.SimpleNamespace(
                color_mode="RGB",
                interpolation="LINEAR",
                elements=[
                    types.SimpleNamespace(position=0.0, color=(0.2, 0.4, 0.8, 1.0)),
                    types.SimpleNamespace(position=1.0, color=(0.9, 0.2, 0.1, 1.0)),
                ],
            ),
        )
        _link(oi, "Random", ramp, "Factor")
        # Also combine Color, Alpha, Object Index, and Location into one expression
        # so bin/rise parses ALL osl_object_* builtins end-to-end.
        comb = _FakeNode(
            "ShaderNodeCombineXYZ",
            inputs=[
                _FakeSocket("X", "VALUE", 0.0),
                _FakeSocket("Y", "VALUE", 0.0),
                _FakeSocket("Z", "VALUE", 0.0),
            ],
            outputs=[_FakeSocket("Vector", "VECTOR")],
        )
        _link(oi, "Alpha", comb, "X")
        _link(oi, "Object Index", comb, "Y")
        _link(oi, "Random", comb, "Z")
        add_v = _FakeNode(
            "ShaderNodeVectorMath",
            inputs=[_FakeSocket("Vector", "VECTOR"), _FakeSocket("Vector", "VECTOR")],
            outputs=[_FakeSocket("Vector", "VECTOR")],
            operation="ADD",
        )
        _link(oi, "Color", add_v, 0)
        _link(comb, "Vector", add_v, 1)
        mix = _FakeNode(
            "ShaderNodeMix",
            inputs=[
                _FakeSocket("Factor", "VALUE", 0.5),
                _FakeSocket("A", "RGBA"),
                _FakeSocket("B", "RGBA"),
            ],
            outputs=[_FakeSocket("Result", "RGBA")],
            data_type="RGBA",
            blend_type="ADD",
            clamp_factor=False,
            clamp_result=False,
        )
        _link(ramp, "Color", mix, "A")
        _link(add_v, "Vector", mix, "B")
        dst = _FakeSocket("Base Color", "RGBA")
        dst.links.append(_FakeLink(mix, mix.outputs["Result"]))
        res = procedural_expr.compile_socket_expression(dst, want_type="vec3")
        self.assertIsNotNone(res)
        for fn in ("osl_object_color()", "osl_object_alpha()", "osl_object_index()", "osl_object_random()"):
            self.assertIn(fn, res.expression)

        lines = res.expression.splitlines()
        body_lines = [f"\t{ln}" if ln.startswith("def ") else f"\texpr\t\t{ln}" for ln in lines]
        scene_txt = (
            "RISE ASCII SCENE 7\n"
            "expression_painter\n{\n\tname\tobjinfo_test\n"
            + "\n".join(body_lines)
            + "\n}\n"
        )
        with tempfile.NamedTemporaryFile("w", suffix=".RISEscene", delete=False) as tf:
            tf.write(scene_txt)
            scene_path = tf.name
        try:
            env = dict(os.environ)
            env["RISE_MEDIA_PATH"] = _REPO_ROOT + os.sep
            proc = subprocess.run(
                [rise_bin, scene_path],
                input="quit\n",
                text=True,
                capture_output=True,
                timeout=15,
                env=env,
            )
            combined = proc.stdout + proc.stderr
            self.assertEqual(proc.returncode, 0, f"bin/rise exited {proc.returncode}:\n{combined}")
            self.assertNotIn("failed to apply", combined, f"RISE failed to parse:\n{combined}")
            self.assertNotIn("unknown function", combined)
        finally:
            os.unlink(scene_path)


class AttributeAOAndMixWeightTest(unittest.TestCase):
    def test_attribute_geometry_outputs_and_blender53_factor_rename(self):
        attr = _FakeNode(
            "ShaderNodeAttribute",
            outputs=[
                _FakeSocket("Color", "RGBA"),
                _FakeSocket("Vector", "VECTOR"),
                _FakeSocket("Factor", "VALUE"),  # Blender 5.3 display rename
                _FakeSocket("Alpha", "VALUE"),
            ],
            attribute_type="GEOMETRY",
            attribute_name="distance",
        )
        c_col = _compile_node(attr, "Color", want_type="vec3")
        self.assertIsNotNone(c_col)
        self.assertEqual(c_col.expression, 'mesh_attr3("distance")')
        self.assertEqual(c_col.mesh_attributes, ("distance",))

        c_vec = _compile_node(attr, "Vector", want_type="vec3")
        self.assertIsNotNone(c_vec)
        self.assertEqual(c_vec.expression, 'mesh_attr3("distance")')

        c_fac = _compile_node(attr, "Factor", want_type="scalar")
        self.assertIsNotNone(c_fac)
        # Must use exact Cycles average (1/3, 1/3, 1/3), NOT Rec.709 luminance!
        self.assertIn("0.3333333432674408", c_fac.expression)
        self.assertNotIn("0.2126", c_fac.expression)
        self.assertEqual(c_fac.mesh_attributes, ("distance",))

        c_alpha = _compile_node(attr, "Alpha", want_type="scalar")
        self.assertIsNotNone(c_alpha)
        self.assertEqual(c_alpha.expression, 'mesh_attr_alpha("distance")')
        self.assertEqual(c_alpha.mesh_attributes, ("distance",))

        # Verify Blender <=5.2 "Fac" output name AND FLOAT2 .x Cycles parity (R1 A-P1-3)
        attr_f2 = _FakeNode(
            "ShaderNodeAttribute",
            outputs=[_FakeSocket("Fac", "VALUE")],
            attribute_type="GEOMETRY",
            attribute_name="uv2",
        )
        dst_f2 = _FakeSocket("Fac", "VALUE")
        dst_f2.links.append(_FakeLink(attr_f2, attr_f2.outputs["Fac"]))
        c_f2 = procedural_expr.compile_socket_expression(dst_f2, want_type="scalar", attr_types={"uv2": "FLOAT2"})
        self.assertIsNotNone(c_f2)
        self.assertEqual(c_f2.expression, 'mesh_attr3("uv2").x')

    def test_attribute_non_geometry_and_empty_fail_closed(self):
        for bad_type in ("OBJECT", "INSTANCER", "VIEW_LAYER"):
            attr = _FakeNode(
                "ShaderNodeAttribute",
                outputs=[_FakeSocket("Color", "RGBA")],
                attribute_type=bad_type,
                attribute_name="foo",
            )
            self.assertIsNone(_compile_node(attr, "Color", want_type="vec3"))

        for bad_name in ("", "a\x00b", "bad\ud800surrogate"):
            bad = _FakeNode(
                "ShaderNodeAttribute",
                outputs=[_FakeSocket("Color", "RGBA")],
                attribute_type="GEOMETRY",
                attribute_name=bad_name,
            )
            self.assertIsNone(_compile_node(bad, "Color", want_type="vec3"))

    def test_attribute_name_escaping(self):
        attr = _FakeNode(
            "ShaderNodeAttribute",
            outputs=[_FakeSocket("Color", "RGBA")],
            attribute_type="GEOMETRY",
            attribute_name='a"b\\c\td\ne{f}#\u00e9',
        )
        c = _compile_node(attr, "Color", want_type="vec3")
        self.assertIsNotNone(c)
        self.assertNotIn("\n", c.expression)
        self.assertNotIn("{", c.expression)
        self.assertNotIn("}", c.expression)
        self.assertNotIn("#", c.expression)
        self.assertIn(r'\"', c.expression)
        self.assertIn(r'\\', c.expression)
        self.assertIn(r'\t', c.expression)
        self.assertIn(r'\x0a', c.expression)
        self.assertIn(r'\x7b', c.expression)
        self.assertIn(r'\x7d', c.expression)
        self.assertIn(r'\x23', c.expression)
        self.assertIn(r'\xc3\xa9', c.expression)

    def test_ambient_occlusion_ao_color_and_flags(self):
        ao = _FakeNode(
            "ShaderNodeAmbientOcclusion",
            inputs=[
                _FakeSocket("Color", "RGBA", (0.2, 0.5, 0.8, 1.0)),
                _FakeSocket("Distance", "VALUE", 0.07),
                _FakeSocket("Normal", "VECTOR", (0.0, 0.0, 0.0)),
            ],
            outputs=[
                _FakeSocket("Color", "RGBA"),
                _FakeSocket("AO", "VALUE"),
            ],
            samples=16,
            inside=True,
            only_local=False,
        )
        c_ao = _compile_node(ao, "AO", want_type="scalar")
        self.assertIsNotNone(c_ao)
        self.assertIn("osl_ao(0.07, 16, vec3(0, 0, 0), 1)", c_ao.expression)

        c_col = _compile_node(ao, "Color", want_type="vec3")
        self.assertIsNotNone(c_col)
        self.assertIn("osl_ao(0.07, 16, vec3(0, 0, 0), 1)", c_col.expression)
        self.assertIn("vec3(0.2, 0.5, 0.8)", c_col.expression)

        # Shared osl_ao def slot when BOTH AO and Color are consumed in one graph
        mix_both = _FakeNode(
            "ShaderNodeMix",
            inputs=[
                _FakeSocket("Factor", "VALUE"),
                _FakeSocket("A", "RGBA"),
                _FakeSocket("B", "RGBA", (0.0, 0.0, 0.0, 1.0)),
            ],
            outputs=[_FakeSocket("Result", "RGBA")],
            data_type="RGBA",
            blend_type="MIX",
        )
        _link(ao, "AO", mix_both, "Factor", dst_type="VALUE")
        _link(ao, "Color", mix_both, "A", dst_type="RGBA")
        c_both = _compile_node(mix_both, "Result", want_type="vec3")
        self.assertIsNotNone(c_both)
        self.assertEqual(c_both.expression.count("osl_ao("), 1)

        # Linked Normal + only_local -> flags = 1 | 2 | 4 = 7; samples clamped to [1, 128]
        ao2 = _FakeNode(
            "ShaderNodeAmbientOcclusion",
            inputs=[
                _FakeSocket("Color", "RGBA", (1.0, 1.0, 1.0, 1.0)),
                _FakeSocket("Distance", "VALUE", 0.5),
                _FakeSocket("Normal", "VECTOR", (0.0, 0.0, 0.0)),
            ],
            outputs=[_FakeSocket("AO", "VALUE")],
            samples=256,  # clamped to 128 (R1 A-P2-3)
            inside=True,
            only_local=True,
        )
        tc = _FakeNode("ShaderNodeTexCoord", outputs=[_FakeSocket("Normal", "VECTOR")])
        _link(tc, "Normal", ao2, "Normal")
        c_ao2 = _compile_node(ao2, "AO", want_type="scalar")
        self.assertIsNotNone(c_ao2)
        self.assertIn("osl_ao(0.5, 128, osl_to_object_normal(N), 7)", c_ao2.expression)

        # _active_node_inputs ignores unsupported node on ao.Color when only AO output is used
        bad_node = _FakeNode("ShaderNodeGeometry", outputs=[_FakeSocket("Position", "VECTOR")])
        ao_pruned = _FakeNode(
            "ShaderNodeAmbientOcclusion",
            inputs=[
                _FakeSocket("Color", "RGBA", (1.0, 1.0, 1.0, 1.0)),
                _FakeSocket("Distance", "VALUE", 0.2),
                _FakeSocket("Normal", "VECTOR", (0.0, 0.0, 0.0)),
            ],
            outputs=[_FakeSocket("AO", "VALUE")],
            samples=0,  # clamped to 1
            inside=False,
            only_local=False,
        )
        _link(bad_node, "Position", ao_pruned, "Color")
        c_pruned = _compile_node(ao_pruned, "AO", want_type="scalar")
        self.assertIsNotNone(c_pruned)
        self.assertIn("osl_ao(0.2, 1, vec3(0, 0, 0), 0)", c_pruned.expression)

    def test_mix_weight_expression_single_nested_nan_and_passthrough(self):
        # Constant unlinked chain: (1 - 0.25) * 0.4 = 0.3
        c_const = procedural_expr.compile_mix_weight_expression(
            [(None, 0.25, False), (None, 0.4, True)]
        )
        self.assertIsNotNone(c_const)
        self.assertAlmostEqual(float(c_const.expression), 0.3, places=6)

        # R1 A-P1-2: NaN default must clamp to 0.0 (branch 0 -> 1.0, branch 1 -> 0.0)
        c_nan0 = procedural_expr.compile_mix_weight_expression([(None, float("nan"), False)])
        c_nan1 = procedural_expr.compile_mix_weight_expression([(None, float("nan"), True)])
        self.assertAlmostEqual(float(c_nan0.expression), 1.0, places=6)
        self.assertAlmostEqual(float(c_nan1.expression), 0.0, places=6)

        # Dynamic chain sharing an Attribute (named "_v0" to red-proof R1 B-P1-2!) + AO node
        attr = _FakeNode(
            "ShaderNodeAttribute",
            outputs=[_FakeSocket("Factor", "VALUE")],
            attribute_type="GEOMETRY",
            attribute_name="_v0",
        )
        ao = _FakeNode(
            "ShaderNodeAmbientOcclusion",
            inputs=[
                _FakeSocket("Color", "RGBA", (1.0, 1.0, 1.0, 1.0)),
                _FakeSocket("Distance", "VALUE", 0.1),
                _FakeSocket("Normal", "VECTOR", (0.0, 0.0, 0.0)),
            ],
            outputs=[_FakeSocket("AO", "VALUE")],
            samples=8,
            inside=True,
            only_local=False,
        )
        s0 = _FakeSocket("Fac", "VALUE", 0.5)
        s0.links.append(_FakeLink(attr, attr.outputs["Factor"]))
        s1 = _FakeSocket("Fac", "VALUE", 0.5)
        s1.links.append(_FakeLink(ao, ao.outputs["AO"]))

        w0 = procedural_expr.compile_mix_weight_expression([(s0, 0.5, False), (s1, 0.5, True)])
        self.assertIsNotNone(w0)
        self.assertIn('mesh_attr3("_v0")', w0.expression)
        self.assertIn("osl_ao(0.1, 8, vec3(0, 0, 0), 1)", w0.expression)
        self.assertIn("clamp(", w0.expression)
        self.assertEqual(w0.mesh_attributes, ("_v0",))

        # R1 B-P1-2: Multi-transparent passthrough shares ONE DAG (1 osl_ao call) and
        # NEVER corrupts mesh_attr3("_v0") string literals!
        pt = procedural_expr.compile_mix_passthrough_expression([
            [(s0, 0.5, False), (s1, 0.5, True)],
            [(s0, 0.5, True), (s1, 0.5, True)],
        ])
        self.assertIsNotNone(pt)
        self.assertEqual(pt.expression.count("osl_ao("), 1)
        self.assertIn('mesh_attr3("_v0")', pt.expression)
        self.assertEqual(pt.mesh_attributes, ("_v0",))

        # R2 P2-2: Deep 30-factor product & 30-leaf passthrough chains split via def slots
        # well below ExpressionProgram::kMaxParseDepth (200)
        deep_factors = []
        for idx in range(30):
            s_i = _FakeSocket(f"Fac_{idx}", "VALUE", 0.5)
            s_i.links.append(_FakeLink(attr, attr.outputs["Factor"]))
            deep_factors.append((s_i, 0.5, bool(idx % 2)))
        c_deep = procedural_expr.compile_mix_weight_expression(deep_factors)
        self.assertIsNotNone(c_deep)
        self.assertLessEqual(c_deep.max_expr_depth, 28)

        pt_lists = [[(None, 0.0, True)], [(None, 0.1, True)]] + [
            [(deep_factors[i][0], 0.5, True)] for i in range(28)
        ]
        c_pt_deep = procedural_expr.compile_mix_passthrough_expression(pt_lists)
        self.assertIsNotNone(c_pt_deep)
        self.assertLessEqual(c_pt_deep.max_expr_depth, 28)

    def test_rise_parses_generated_attr_and_ao_expressions(self):
        rise_bin = os.path.join(_REPO_ROOT, "bin", "rise")
        if not os.path.isfile(rise_bin):
            self.skipTest("bin/rise not built")

        attr = _FakeNode(
            "ShaderNodeAttribute",
            outputs=[_FakeSocket("Factor", "VALUE"), _FakeSocket("Alpha", "VALUE")],
            attribute_type="GEOMETRY",
            attribute_name='weird "attr"\t#1',
        )
        attr_f2 = _FakeNode(
            "ShaderNodeAttribute",
            outputs=[_FakeSocket("Factor", "VALUE")],
            attribute_type="GEOMETRY",
            attribute_name="uv2",
        )
        ao = _FakeNode(
            "ShaderNodeAmbientOcclusion",
            inputs=[
                _FakeSocket("Color", "RGBA", (1.0, 1.0, 1.0, 1.0)),
                _FakeSocket("Distance", "VALUE", 0.07),
                _FakeSocket("Normal", "VECTOR", (0.0, 0.0, 0.0)),
            ],
            outputs=[_FakeSocket("AO", "VALUE")],
            samples=16,
            inside=True,
            only_local=False,
        )
        s0 = _FakeSocket("Fac", "VALUE", 0.5)
        s0.links.append(_FakeLink(attr, attr.outputs["Factor"]))
        s1 = _FakeSocket("Fac", "VALUE", 0.5)
        s1.links.append(_FakeLink(ao, ao.outputs["AO"]))
        s2 = _FakeSocket("Fac", "VALUE", 0.5)
        s2.links.append(_FakeLink(attr_f2, attr_f2.outputs["Factor"]))
        comp = procedural_expr.compile_mix_weight_expression(
            [(s0, 0.5, False), (s1, 0.5, True), (s2, 0.5, True)],
            attr_types={"uv2": "FLOAT2"},
        )
        self.assertIsNotNone(comp)
        self.assertIn('mesh_attr3("uv2").x', comp.expression)

        scene_txt = "RISE ASCII SCENE 7\n\n" + _format_rise_painter_chunk("p_attr_ao", comp) + "\n"
        with tempfile.NamedTemporaryFile("w", suffix=".RISEscene", delete=False) as tf:
            tf.write(scene_txt)
            scene_path = tf.name
        try:
            env = dict(os.environ)
            env["RISE_MEDIA_PATH"] = _REPO_ROOT + os.sep
            proc = subprocess.run(
                [rise_bin, scene_path],
                input="quit\n",
                text=True,
                capture_output=True,
                timeout=15,
                env=env,
            )
            combined = proc.stdout + "\n" + proc.stderr
            self.assertEqual(proc.returncode, 0, f"bin/rise exited {proc.returncode}:\n{combined}")
            self.assertNotIn("failed to apply", combined, f"RISE failed to parse:\n{combined}")
            self.assertNotIn("unknown function", combined)
        finally:
            os.unlink(scene_path)

    def test_phase1_nodes_groups_and_muted_passthrough(self):
        # 1. TexGradient (SPHERICAL) + TexBrick + VertexColor + NewGeometry + ParticleInfo
        grad = _FakeNode(
            "ShaderNodeTexGradient",
            inputs=[_FakeSocket("Vector", "VECTOR", (0.0, 0.0, 0.0))],
            outputs=[_FakeSocket("Color", "RGBA"), _FakeSocket("Factor", "VALUE")],
            gradient_type="SPHERICAL",
        )
        s_grad = _FakeSocket("Fac", "VALUE", 0.0)
        s_grad.links.append(_FakeLink(grad, grad.outputs["Factor"]))
        c_grad = procedural_expr.compile_socket_expression(s_grad, want_type="scalar")
        self.assertIsNotNone(c_grad)
        self.assertIn("osl_gradient(", c_grad.expression)
        self.assertTrue(c_grad.uses_generated)

        brick = _FakeNode(
            "ShaderNodeTexBrick",
            inputs=[
                _FakeSocket("Vector", "VECTOR", (0.0, 0.0, 0.0)),
                _FakeSocket("Color1", "RGBA", (0.8, 0.2, 0.1, 1.0)),
                _FakeSocket("Color2", "RGBA", (0.2, 0.2, 0.2, 1.0)),
                _FakeSocket("Mortar", "RGBA", (0.0, 0.0, 0.0, 1.0)),
                _FakeSocket("Scale", "VALUE", 5.0),
                _FakeSocket("Mortar Size", "VALUE", 0.02),
                _FakeSocket("Mortar Smooth", "VALUE", 0.1),
                _FakeSocket("Bias", "VALUE", 0.0),
                _FakeSocket("Brick Width", "VALUE", 0.5),
                _FakeSocket("Row Height", "VALUE", 0.25),
            ],
            outputs=[_FakeSocket("Color", "RGBA"), _FakeSocket("Fac", "VALUE")],
            offset=0.5, offset_frequency=2, squash=1.0, squash_frequency=2,
        )
        s_brick = _FakeSocket("Base Color", "RGBA", (0.5, 0.5, 0.5, 1.0))
        s_brick.links.append(_FakeLink(brick, brick.outputs["Color"]))
        c_brick = procedural_expr.compile_socket_expression(s_brick, want_type="vec3")
        self.assertIsNotNone(c_brick)
        self.assertIn("osl_brick(", c_brick.expression)

        geom = _FakeNode(
            "ShaderNodeNewGeometry",
            inputs=[],
            outputs=[
                _FakeSocket("Backfacing", "VALUE"),
                _FakeSocket("Random Per Island", "VALUE"),
                _FakeSocket("Pointiness", "VALUE"),
            ],
        )
        part = _FakeNode("ShaderNodeParticleInfo", inputs=[], outputs=[_FakeSocket("Random", "VALUE")])
        vcol = _FakeNode("ShaderNodeVertexColor", inputs=[], outputs=[_FakeSocket("Color", "RGBA")], layer_name="Col")
        vcol_def = _FakeNode("ShaderNodeVertexColor", inputs=[], outputs=[_FakeSocket("Color", "RGBA")], layer_name="")

        for sock_src, expected_sub, expected_attr in (
            ((geom, "Backfacing"), "osl_backfacing()", None),
            ((geom, "Random Per Island"), procedural_expr.ISLAND_RANDOM_ATTR, procedural_expr.ISLAND_RANDOM_ATTR),
            ((part, "Random"), "osl_particle_random()", None),
        ):
            s = _FakeSocket("Fac", "VALUE", 0.0)
            s.links.append(_FakeLink(sock_src[0], sock_src[0].outputs[sock_src[1]]))
            comp = procedural_expr.compile_socket_expression(s, want_type="scalar")
            self.assertIsNotNone(comp)
            self.assertIn(expected_sub, comp.expression)
            if expected_attr:
                self.assertIn(expected_attr, comp.mesh_attributes)

        # Unsupported NewGeometry.Pointiness must fail closed
        s_bad_g = _FakeSocket("Fac", "VALUE", 0.0)
        s_bad_g.links.append(_FakeLink(geom, geom.outputs["Pointiness"]))
        self.assertIsNone(procedural_expr.compile_socket_expression(s_bad_g, want_type="scalar"))

        # VertexColor named + default active color attribute
        s_vc = _FakeSocket("Base Color", "RGBA", (0, 0, 0, 1))
        s_vc.links.append(_FakeLink(vcol, vcol.outputs["Color"]))
        c_vc = procedural_expr.compile_socket_expression(s_vc, want_type="vec3")
        self.assertIsNotNone(c_vc)
        self.assertIn('mesh_attr3("Col")', c_vc.expression)
        self.assertIn("Col", c_vc.mesh_attributes)

        s_vcd = _FakeSocket("Base Color", "RGBA", (0, 0, 0, 1))
        s_vcd.links.append(_FakeLink(vcol_def, vcol_def.outputs["Color"]))
        c_vcd = procedural_expr.compile_socket_expression(s_vcd, want_type="vec3")
        self.assertIsNotNone(c_vcd)
        self.assertIn(procedural_expr.ACTIVE_COLOR_ATTR, c_vcd.mesh_attributes)

        # 2. Muted node with internal_links passes through cleanly
        val_node = _FakeNode("ShaderNodeValue", inputs=[], outputs=[_FakeSocket("Value", "VALUE", 0.42)])
        muted_mix = _FakeNode(
            "ShaderNodeMix",
            inputs=[_FakeSocket("A", "VALUE", 0.0), _FakeSocket("B", "VALUE", 1.0)],
            outputs=[_FakeSocket("Result", "VALUE")],
        )
        muted_mix.mute = True
        muted_mix.inputs["A"].links.append(_FakeLink(val_node, val_node.outputs["Value"]))
        muted_mix.internal_links = [
            type("_IL", (), {"from_socket": muted_mix.inputs["A"], "to_socket": muted_mix.outputs["Result"]})()
        ]
        s_mut = _FakeSocket("Roughness", "VALUE", 0.5)
        s_mut.links.append(_FakeLink(muted_mix, muted_mix.outputs["Result"]))
        c_mut = procedural_expr.compile_socket_expression(s_mut, want_type="scalar")
        self.assertIsNotNone(c_mut)
        self.assertEqual(c_mut.expression, "0.42")

        # 3. ShaderNodeGroup inlining with TWO distinct instances sharing one node_tree
        g_in = _FakeNode("NodeGroupInput", inputs=[], outputs=[_FakeSocket("X", "VALUE")])
        g_math = _FakeNode(
            "ShaderNodeMath",
            inputs=[_FakeSocket("Value", "VALUE", 0.0), _FakeSocket("Value", "VALUE", 2.0)],
            outputs=[_FakeSocket("Value", "VALUE")],
            operation="MULTIPLY",
        )
        g_math.inputs[0].links.append(_FakeLink(g_in, g_in.outputs["X"]))
        g_out = _FakeNode(
            "NodeGroupOutput",
            inputs=[_FakeSocket("Y", "VALUE", 0.0)],
            outputs=[],
            is_active_output=True,
        )
        g_out.inputs["Y"].links.append(_FakeLink(g_math, g_math.outputs["Value"]))
        shared_tree = type("_Tree", (), {"nodes": [g_in, g_math, g_out], "as_pointer": lambda self: 999991})()

        grp1 = _FakeNode(
            "ShaderNodeGroup",
            inputs=[_FakeSocket("X", "VALUE", 0.1)],
            outputs=[_FakeSocket("Y", "VALUE")],
            node_tree=shared_tree,
        )
        grp2 = _FakeNode(
            "ShaderNodeGroup",
            inputs=[_FakeSocket("X", "VALUE", 0.4)],
            outputs=[_FakeSocket("Y", "VALUE")],
            node_tree=shared_tree,
        )
        add = _FakeNode(
            "ShaderNodeMath",
            inputs=[_FakeSocket("Value", "VALUE", 0.0), _FakeSocket("Value", "VALUE", 0.0)],
            outputs=[_FakeSocket("Value", "VALUE")],
            operation="ADD",
        )
        add.inputs[0].links.append(_FakeLink(grp1, grp1.outputs["Y"]))
        add.inputs[1].links.append(_FakeLink(grp2, grp2.outputs["Y"]))
        s_grp = _FakeSocket("Fac", "VALUE", 0.0)
        s_grp.links.append(_FakeLink(add, add.outputs["Value"]))
        c_grp = procedural_expr.compile_socket_expression(s_grp, want_type="scalar")
        self.assertIsNotNone(c_grp)
        # Must NOT alias grp1's 0.1 and grp2's 0.4 in _cache!
        self.assertIn("0.1", c_grp.expression)
        self.assertIn("0.4", c_grp.expression)

        # 4. Muted ShaderNodeMix (RGBA) with 3 duplicate-named "Result" outputs preserves RGB (no luminance collapse)
        rgb_node = _FakeNode("ShaderNodeRGB", inputs=[], outputs=[_FakeSocket("Color", "RGBA", (0.2, 0.6, 0.9, 1.0))])
        out_val = _FakeSocket("Result", "VALUE")
        out_val.identifier = "Result"
        out_vec = _FakeSocket("Result", "VECTOR")
        out_vec.identifier = "Result_Vector"
        out_col = _FakeSocket("Result", "RGBA")
        out_col.identifier = "Result_Color"
        in_val_a = _FakeSocket("A", "VALUE", 0.0)
        in_val_a.identifier = "A_Float"
        in_col_a = _FakeSocket("A", "RGBA", (0.0, 0.0, 0.0, 1.0))
        in_col_a.identifier = "A_Color"
        in_col_a.links.append(_FakeLink(rgb_node, rgb_node.outputs["Color"]))
        muted_rgba_mix = _FakeNode(
            "ShaderNodeMix",
            inputs=[in_val_a, in_col_a],
            outputs=[out_val, out_vec, out_col],
            data_type="RGBA",
        )
        muted_rgba_mix.mute = True
        muted_rgba_mix.internal_links = [
            type("_IL", (), {"from_socket": in_val_a, "to_socket": out_val})(),
            type("_IL", (), {"from_socket": in_col_a, "to_socket": out_col})(),
        ]
        s_mut_col = _FakeSocket("Base Color", "RGBA", (0, 0, 0, 1))
        s_mut_col.links.append(_FakeLink(muted_rgba_mix, out_col))
        c_mut_col = procedural_expr.compile_socket_expression(s_mut_col, want_type="vec3")
        self.assertIsNotNone(c_mut_col)
        self.assertIn("vec3(0.2, 0.6, 0.9)", c_mut_col.expression)
        self.assertNotIn("0.2126", c_mut_col.expression)

        # 5. Duplicate-named ShaderNodeGroup / NodeGroupInput sockets distinguished by .identifier
        gi_0 = _FakeSocket("Color", "RGBA")
        gi_0.identifier = "Socket_0"
        gi_1 = _FakeSocket("Color", "RGBA")
        gi_1.identifier = "Socket_1"
        dup_gin = _FakeNode("NodeGroupInput", inputs=[], outputs=[gi_0, gi_1])
        go_0 = _FakeSocket("Out", "RGBA", (0, 0, 0, 1))
        go_0.identifier = "Socket_2"
        go_1 = _FakeSocket("Out", "RGBA", (0, 0, 0, 1))
        go_1.identifier = "Socket_3"
        go_0.links.append(_FakeLink(dup_gin, gi_0))
        go_1.links.append(_FakeLink(dup_gin, gi_1))
        dup_gout = _FakeNode("NodeGroupOutput", inputs=[go_0, go_1], outputs=[], is_active_output=True)
        dup_tree = type("_Tree", (), {"nodes": [dup_gin, dup_gout], "as_pointer": lambda self: 999992})()

        g_in0 = _FakeSocket("Color", "RGBA", (0.1, 0.2, 0.3, 1.0))
        g_in0.identifier = "Socket_0"
        g_in1 = _FakeSocket("Color", "RGBA", (0.7, 0.8, 0.9, 1.0))
        g_in1.identifier = "Socket_1"
        g_out0 = _FakeSocket("Out", "RGBA")
        g_out0.identifier = "Socket_2"
        g_out1 = _FakeSocket("Out", "RGBA")
        g_out1.identifier = "Socket_3"
        dup_grp = _FakeNode("ShaderNodeGroup", inputs=[g_in0, g_in1], outputs=[g_out0, g_out1], node_tree=dup_tree)
        s_d1 = _FakeSocket("Base Color", "RGBA", (0, 0, 0, 1))
        s_d1.links.append(_FakeLink(dup_grp, g_out1))
        c_d1 = procedural_expr.compile_socket_expression(s_d1, want_type="vec3")
        self.assertIsNotNone(c_d1)
        self.assertIn("vec3(0.7, 0.8, 0.9)", c_d1.expression)

        # 6. ShaderNodeNewGeometry.Normal -> osl_shading_normal() (pre-modifier N0)
        geom_n = _FakeNode("ShaderNodeNewGeometry", inputs=[], outputs=[_FakeSocket("Normal", "VECTOR")])
        s_gn = _FakeSocket("Vector", "VECTOR", (0, 0, 0))
        s_gn.links.append(_FakeLink(geom_n, geom_n.outputs["Normal"]))
        c_gn = procedural_expr.compile_socket_expression(s_gn, want_type="vec3")
        self.assertIsNotNone(c_gn)
        self.assertEqual(c_gn.expression, "osl_shading_normal()")

        # 7. ShaderNodeTexBrick Fac-only ignores unsupported Color1 input
        bad_node = _FakeNode("ShaderNodeUnsupportedDummy", inputs=[], outputs=[_FakeSocket("Color", "RGBA")])
        brick = _FakeNode(
            "ShaderNodeTexBrick",
            inputs=[
                _FakeSocket("Vector", "VECTOR", (0, 0, 0)),
                _FakeSocket("Color1", "RGBA", (0.8, 0.8, 0.8, 1.0)),
                _FakeSocket("Color2", "RGBA", (0.2, 0.2, 0.2, 1.0)),
                _FakeSocket("Mortar", "RGBA", (0.0, 0.0, 0.0, 1.0)),
                _FakeSocket("Scale", "VALUE", 5.0),
                _FakeSocket("Mortar Size", "VALUE", 0.02),
                _FakeSocket("Mortar Smooth", "VALUE", 0.1),
                _FakeSocket("Bias", "VALUE", 0.0),
                _FakeSocket("Brick Width", "VALUE", 0.5),
                _FakeSocket("Row Height", "VALUE", 0.25),
            ],
            outputs=[_FakeSocket("Color", "RGBA"), _FakeSocket("Fac", "VALUE")],
            offset=0.5,
            offset_frequency=2,
            squash=1.0,
            squash_frequency=2,
        )
        brick.inputs["Color1"].links.append(_FakeLink(bad_node, bad_node.outputs["Color"]))
        s_bfac = _FakeSocket("Roughness", "VALUE", 0.5)
        s_bfac.links.append(_FakeLink(brick, brick.outputs["Fac"]))
        c_bfac = procedural_expr.compile_socket_expression(s_bfac, want_type="scalar")
        self.assertIsNotNone(c_bfac)
        self.assertIn("osl_brick(", c_bfac.expression)
        # Consuming Color MUST still fail closed because Color1 is unsupported
        s_bcol = _FakeSocket("Base Color", "RGBA", (0, 0, 0, 1))
        s_bcol.links.append(_FakeLink(brick, brick.outputs["Color"]))
        self.assertIsNone(procedural_expr.compile_socket_expression(s_bcol, want_type="vec3"))

        # 8. osl_gradient enum matches Cycles NodeGradientType (RADIAL=4, QUADRATIC_SPHERE=5, SPHERICAL=6)
        self.assertEqual(procedural_expr._GRADIENT_TYPE_MAP["RADIAL"], 4)
        self.assertEqual(procedural_expr._GRADIENT_TYPE_MAP["QUADRATIC_SPHERE"], 5)
        self.assertEqual(procedural_expr._GRADIENT_TYPE_MAP["SPHERICAL"], 6)

    def test_phase2_uvmap_rgbcurve_teximage(self):
        # 1. ShaderNodeUVMap (empty -> primary UV, named -> mesh_attr3, from_instancer -> fail closed)
        uv_empty = _FakeNode("ShaderNodeUVMap", inputs=[], outputs=[_FakeSocket("UV", "VECTOR")], uv_map="", from_instancer=False)
        uv_named = _FakeNode("ShaderNodeUVMap", inputs=[], outputs=[_FakeSocket("UV", "VECTOR")], uv_map="decal", from_instancer=False)
        uv_inst = _FakeNode("ShaderNodeUVMap", inputs=[], outputs=[_FakeSocket("UV", "VECTOR")], uv_map="decal", from_instancer=True)

        s0 = _FakeSocket("Vector", "VECTOR", (0, 0, 0))
        s0.links.append(_FakeLink(uv_empty, uv_empty.outputs["UV"]))
        c0 = procedural_expr.compile_socket_expression(s0, want_type="vec3")
        self.assertIsNotNone(c0)
        self.assertEqual(c0.expression, "vec3(u, v, 0)")
        self.assertEqual(c0.mesh_attributes, ())

        s1 = _FakeSocket("Vector", "VECTOR", (0, 0, 0))
        s1.links.append(_FakeLink(uv_named, uv_named.outputs["UV"]))
        c1 = procedural_expr.compile_socket_expression(s1, want_type="vec3")
        self.assertIsNotNone(c1)
        self.assertIn(procedural_expr.UV_MAP_ATTR_PREFIX + "decal", c1.mesh_attributes)

        s2 = _FakeSocket("Vector", "VECTOR", (0, 0, 0))
        s2.links.append(_FakeLink(uv_inst, uv_inst.outputs["UV"]))
        self.assertIsNone(procedural_expr.compile_socket_expression(s2, want_type="vec3"))

        # 2. ShaderNodeTexImage: primary UV REPEAT -> sample(), CLIP -> bounds mask, UVMap -> sample_uv(), Alpha -> sample_uv_alpha()
        tex_rep = _FakeNode(
            "ShaderNodeTexImage",
            inputs=[_FakeSocket("Vector", "VECTOR", (0, 0, 0))],
            outputs=[_FakeSocket("Color", "RGBA"), _FakeSocket("Alpha", "VALUE")],
            projection="FLAT",
            extension="REPEAT",
        )
        tex_clip = _FakeNode(
            "ShaderNodeTexImage",
            inputs=[_FakeSocket("Vector", "VECTOR", (0, 0, 0))],
            outputs=[_FakeSocket("Color", "RGBA"), _FakeSocket("Alpha", "VALUE")],
            projection="FLAT",
            extension="CLIP",
        )
        tex_clip.inputs["Vector"].links.append(_FakeLink(uv_named, uv_named.outputs["UV"]))

        # Without image_resolver, TexImage must fail closed
        s_no_res = _FakeSocket("Base Color", "RGBA", (0, 0, 0, 1))
        s_no_res.links.append(_FakeLink(tex_rep, tex_rep.outputs["Color"]))
        self.assertIsNone(procedural_expr.compile_socket_expression(s_no_res, want_type="vec3"))

        # With image_resolver: REPEAT primary UV -> sample(pnt_albedo)
        c_rep = procedural_expr.compile_socket_expression(
            s_no_res, want_type="vec3", image_resolver=lambda n: "pnt_albedo"
        )
        self.assertIsNotNone(c_rep)
        self.assertEqual(c_rep.expression, "sample(pnt_albedo)")

        # CLIP + named UVMap -> sample_uv_alpha with [0,1] bounds mask & UV mesh attr
        s_clip_a = _FakeSocket("Fac", "VALUE", 0.0)
        s_clip_a.links.append(_FakeLink(tex_clip, tex_clip.outputs["Alpha"]))
        c_clip_a = procedural_expr.compile_socket_expression(
            s_clip_a, want_type="scalar", image_resolver=lambda n: "pnt_decal"
        )
        self.assertIsNotNone(c_clip_a)
        self.assertIn("sample_uv_alpha(pnt_decal,", c_clip_a.expression)
        self.assertIn("clamp(", c_clip_a.expression)
        self.assertIn(procedural_expr.UV_MAP_ATTR_PREFIX + "decal", c_clip_a.mesh_attributes)

        # 3. ShaderNodeRGBCurve: nonlinear C curve emits 64-stop RISE ramp(),
        #    inward control points ignore clip_min_x/clip_max_x (Cycles parity),
        #    extend='HORIZONTAL' clamps identity and linear channels, Fac blends.
        def _pt(x, y):
            return type("_Pt", (), {"location": (x, y)})()

        def _curve(pts):
            return type("_Curve", (), {"points": [_pt(x, y) for x, y in pts]})()

        ident_c = _curve([(0.0, 0.0), (1.0, 1.0)])
        bent_c = _curve([(0.0, 0.0), (0.5, 0.8), (1.0, 1.0)])
        mapping_bent = type(
            "_CM",
            (),
            {
                "curves": [ident_c, ident_c, ident_c, bent_c],
                "use_clip": True,
                "clip_min_x": 0.0,
                "clip_max_x": 1.0,
                "extend": "EXTRAPOLATED",
            },
        )()
        rgb_curve = _FakeNode(
            "ShaderNodeRGBCurve",
            inputs=[_FakeSocket("Fac", "VALUE", 1.0), _FakeSocket("Color", "RGBA", (0.5, 0.5, 0.5, 1.0))],
            outputs=[_FakeSocket("Color", "RGBA")],
            mapping=mapping_bent,
        )
        rgb_curve.inputs["Color"].links.append(_FakeLink(tex_rep, tex_rep.outputs["Color"]))
        s_crv = _FakeSocket("Base Color", "RGBA", (0, 0, 0, 1))
        s_crv.links.append(_FakeLink(rgb_curve, rgb_curve.outputs["Color"]))
        c_crv = procedural_expr.compile_socket_expression(
            s_crv, want_type="vec3", image_resolver=lambda n: "pnt_albedo"
        )
        self.assertIsNotNone(c_crv)
        self.assertIn("sample(pnt_albedo)", c_crv.expression)
        self.assertIn("ramp(", c_crv.expression)

        # P1-1: _curvemapping_minmax MUST scan control points, ignoring use_clip/clip_min_x/clip_max_x
        inward = _curve([(0.2, 0.1), (0.8, 0.9)])
        mapping_inward = type(
            "_CM", (),
            {
                "curves": [inward, inward, inward, inward],
                "use_clip": True, "clip_min_x": 0.0, "clip_max_x": 1.0,
                "extend": "HORIZONTAL",
            },
        )()
        self.assertEqual(procedural_expr._curvemapping_minmax(mapping_inward, 4), (0.2, 0.8))

        # P1-2: identity channel with extend='HORIZONTAL' MUST clamp to [min_x, max_x]
        mapping_horiz_ident = type(
            "_CM", (),
            {
                "curves": [ident_c, ident_c, ident_c, ident_c],
                "use_clip": True, "clip_min_x": 0.0, "clip_max_x": 1.0,
                "extend": "HORIZONTAL",
            },
        )()
        rgb_horiz = _FakeNode(
            "ShaderNodeRGBCurve",
            inputs=[_FakeSocket("Fac", "VALUE", 0.5), _FakeSocket("Color", "RGBA", (2.0, -0.5, 0.5, 1.0))],
            outputs=[_FakeSocket("Color", "RGBA")],
            mapping=mapping_horiz_ident,
        )
        rgb_horiz.inputs["Color"].links.append(_FakeLink(tex_rep, tex_rep.outputs["Color"]))
        s_horiz = _FakeSocket("Base Color", "RGBA", (0, 0, 0, 1))
        s_horiz.links.append(_FakeLink(rgb_horiz, rgb_horiz.outputs["Color"]))
        c_horiz = procedural_expr.compile_socket_expression(
            s_horiz, want_type="vec3", image_resolver=lambda n: "pnt_albedo"
        )
        self.assertIsNotNone(c_horiz)
        self.assertIn("clamp(", c_horiz.expression)
        self.assertIn("mix(", c_horiz.expression)

        # Projection != FLAT must fail closed; EXTEND/MIRROR on primary UV succeed via sample()
        tex_box = _FakeNode(
            "ShaderNodeTexImage",
            inputs=[_FakeSocket("Vector", "VECTOR", (0, 0, 0))],
            outputs=[_FakeSocket("Color", "RGBA"), _FakeSocket("Alpha", "VALUE")],
            projection="BOX", extension="REPEAT",
        )
        s_box = _FakeSocket("Base Color", "RGBA", (0, 0, 0, 1))
        s_box.links.append(_FakeLink(tex_box, tex_box.outputs["Color"]))
        self.assertIsNone(procedural_expr.compile_socket_expression(
            s_box, want_type="vec3", image_resolver=lambda n: "pnt_box"
        ))
        for ext_mode in ("EXTEND", "MIRROR"):
            tex_em = _FakeNode(
                "ShaderNodeTexImage",
                inputs=[_FakeSocket("Vector", "VECTOR", (0, 0, 0))],
                outputs=[_FakeSocket("Color", "RGBA"), _FakeSocket("Alpha", "VALUE")],
                projection="FLAT", extension=ext_mode,
            )
            s_em = _FakeSocket("Base Color", "RGBA", (0, 0, 0, 1))
            s_em.links.append(_FakeLink(tex_em, tex_em.outputs["Color"]))
            c_em = procedural_expr.compile_socket_expression(
                s_em, want_type="vec3", image_resolver=lambda n: f"pnt_{ext_mode.lower()}"
            )
            self.assertIsNotNone(c_em)
            self.assertEqual(c_em.expression, f"sample(pnt_{ext_mode.lower()})")

    def test_phase3_layer_weight_fresnel_light_path(self):
        # 1. ShaderNodeLayerWeight (Fresnel & Facing)
        lw = _FakeNode(
            "ShaderNodeLayerWeight",
            inputs=[_FakeSocket("Blend", "VALUE", 0.5), _FakeSocket("Normal", "VECTOR", (0, 0, 0))],
            outputs=[_FakeSocket("Fresnel", "VALUE"), _FakeSocket("Facing", "VALUE")],
        )
        s_f = _FakeSocket("Fac", "VALUE", 0.0)
        s_f.links.append(_FakeLink(lw, lw.outputs["Fresnel"]))
        c_f = procedural_expr.compile_socket_expression(s_f, want_type="scalar")
        self.assertIsNotNone(c_f)
        self.assertEqual(c_f.expression, "osl_layer_weight(0.5, vec3(0, 0, 0), 0)")

        s_fc = _FakeSocket("Fac", "VALUE", 0.0)
        s_fc.links.append(_FakeLink(lw, lw.outputs["Facing"]))
        c_fc = procedural_expr.compile_socket_expression(s_fc, want_type="scalar")
        self.assertIsNotNone(c_fc)
        self.assertEqual(c_fc.expression, "osl_layer_weight(0.5, vec3(0, 0, 0), 1)")

        # 2. ShaderNodeFresnel
        fr = _FakeNode(
            "ShaderNodeFresnel",
            inputs=[_FakeSocket("IOR", "VALUE", 1.5), _FakeSocket("Normal", "VECTOR", (0, 0, 0))],
            outputs=[_FakeSocket("Fac", "VALUE")],
        )
        s_fr = _FakeSocket("Fac", "VALUE", 0.0)
        s_fr.links.append(_FakeLink(fr, fr.outputs["Fac"]))
        c_fr = procedural_expr.compile_socket_expression(s_fr, want_type="scalar")
        self.assertIsNotNone(c_fr)
        self.assertEqual(c_fr.expression, "osl_fresnel(1.5, vec3(0, 0, 0), 0)")

        # 3. ShaderNodeLightPath: Is Camera Ray succeeds, unsupported output fails closed
        lp = _FakeNode(
            "ShaderNodeLightPath",
            inputs=[],
            outputs=[_FakeSocket("Is Camera Ray", "VALUE"), _FakeSocket("Ray Depth", "VALUE")],
        )
        s_cam = _FakeSocket("Fac", "VALUE", 0.0)
        s_cam.links.append(_FakeLink(lp, lp.outputs["Is Camera Ray"]))
        c_cam = procedural_expr.compile_socket_expression(s_cam, want_type="scalar")
        self.assertIsNotNone(c_cam)
        self.assertEqual(c_cam.expression, "osl_is_camera_ray()")

        s_dep = _FakeSocket("Fac", "VALUE", 0.0)
        s_dep.links.append(_FakeLink(lp, lp.outputs["Ray Depth"]))
        self.assertIsNone(procedural_expr.compile_socket_expression(s_dep, want_type="scalar"))


if __name__ == "__main__":
    unittest.main()
