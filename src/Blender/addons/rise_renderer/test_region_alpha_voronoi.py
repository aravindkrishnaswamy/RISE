"""Unit tests for ABI v19 Blender bridge fixes:
  1. Render region / Crop to Render Region math and buffer slicing.
  2. Transparent film settings marshalling.
  3. Procedural expression compiler (Voronoi, Noise, White Noise, Mix, Hue/Sat,
     Mapping, TexCoord, ColorRamp, fail-closed behaviour, diamond DAG defs).
  4. Manual bake operator procedural-material eligibility.
"""

import array
import os
import sys
import types
import unittest

_ADDON_DIR = os.path.dirname(os.path.abspath(__file__))
_PKG_NAME = "rise_renderer"


def _install_stubs():
    """Install minimal bpy / mathutils / package stubs for headless testing."""
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

from rise_renderer import bridge, exporter, material_bake, procedural_expr  # noqa: E402
from rise_renderer.engine import RISEBlenderRenderEngine  # noqa: E402


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
    _next_ptr = 1000

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


def _link(src_node, src_out_name, dst_node, dst_in_name, dst_type=None, muted=False):
    src_sock = next(o for o in src_node.outputs if o.name == src_out_name)
    dst_sock = next(
        i for i in dst_node.inputs
        if i.name == dst_in_name and (dst_type is None or i.type == dst_type)
    )
    dst_sock.links.append(_FakeLink(src_node, src_sock, is_muted=muted))


def _make_voronoi(dims="3D", feature="F1", distance="EUCLIDEAN", normalize=False, scale=5.0):
    return _FakeNode(
        "ShaderNodeTexVoronoi",
        inputs=[
            _FakeSocket("Vector", "VECTOR", (0.0, 0.0, 0.0)),
            _FakeSocket("W", "VALUE", 0.0),
            _FakeSocket("Scale", "VALUE", scale),
            _FakeSocket("Detail", "VALUE", 0.0),
            _FakeSocket("Roughness", "VALUE", 0.5),
            _FakeSocket("Lacunarity", "VALUE", 2.0),
            _FakeSocket("Smoothness", "VALUE", 1.0),
            _FakeSocket("Exponent", "VALUE", 0.5),
            _FakeSocket("Randomness", "VALUE", 1.0),
        ],
        outputs=[
            _FakeSocket("Distance", "VALUE"),
            _FakeSocket("Color", "RGBA"),
            _FakeSocket("Position", "VECTOR"),
            _FakeSocket("W", "VALUE"),
            _FakeSocket("Radius", "VALUE"),
        ],
        voronoi_dimensions=dims,
        feature=feature,
        distance=distance,
        normalize=normalize,
    )


def _make_material(principled_links_fn):
    out_node = _FakeNode(
        "ShaderNodeOutputMaterial",
        inputs=[_FakeSocket("Surface", "SHADER")],
        is_active_output=True,
        target="ALL",
    )
    principled = _FakeNode(
        "ShaderNodeBsdfPrincipled",
        inputs=[
            _FakeSocket("Base Color", "RGBA", (0.8, 0.8, 0.8, 1.0)),
            _FakeSocket("Roughness", "VALUE", 0.5),
            _FakeSocket("Tangent", "VECTOR", (0.0, 0.0, 0.0)),
        ],
        outputs=[_FakeSocket("BSDF", "SHADER")],
    )
    _link(principled, "BSDF", out_node, "Surface")
    nodes = [out_node, principled]
    principled_links_fn(principled, nodes)
    nt = types.SimpleNamespace(nodes=nodes)
    return types.SimpleNamespace(use_nodes=True, node_tree=nt, name="TestMat")


class RenderRegionTest(unittest.TestCase):
    def test_central_half_480x270_matches_blender(self):
        use, left, top, right, bottom = exporter.compute_render_region(
            480, 270, True, 0.25, 0.75, 0.25, 0.75
        )
        self.assertTrue(use)
        self.assertEqual((left, right), (120, 359))
        self.assertEqual((top, bottom), (68, 202))
        self.assertEqual(right - left + 1, 240)
        self.assertEqual(bottom - top + 1, 135)

    def test_full_border_disables_region(self):
        use, *_ = exporter.compute_render_region(480, 270, True, 0.0, 1.0, 0.0, 1.0)
        self.assertFalse(use)

    def test_disabled_border_returns_false(self):
        use, *_ = exporter.compute_render_region(480, 270, False, 0.25, 0.75, 0.25, 0.75)
        self.assertFalse(use)

    def test_float32_border_rounding_matches_blender_pipeline_cc(self):
        # Blender computes `int(float32(border) * float32(win))` — float64
        # multiplication diverges by 1px on 0.35 / 0.45 / 0.65 / 0.7 / 0.9.
        use, left, top, right, bottom = exporter.compute_render_region(
            480, 270, True, 0.35, 0.7, 0.0, 0.9
        )
        self.assertTrue(use)
        # xmin = int(f32(0.35)*480) = 168; xmax = int(f32(0.7)*480) = 336 -> [168..335]
        # ymin = 0; ymax = int(f32(0.9)*270) = 243 -> top = 270-243 = 27, bottom = 269
        self.assertEqual((left, right), (168, 335))
        self.assertEqual((top, bottom), (27, 269))

        use2, left2, top2, right2, bottom2 = exporter.compute_render_region(
            1920, 1080, True, 0.35, 0.7, 0.35, 0.7
        )
        self.assertTrue(use2)
        self.assertEqual((left2, right2), (672, 1343))
        self.assertEqual((top2, bottom2), (1080 - 756, 1080 - 1 - 378))

    def test_extract_flipped_region_crops_and_flips(self):
        # 4x4 top-down image where pixel (x, y) stores (x, y, 0, 1)
        full = array.array("f")
        for y in range(4):
            for x in range(4):
                full.extend((float(x), float(y), 0.0, 1.0))
        # Crop [left=1..2, top=1..2] -> 2x2
        cropped = RISEBlenderRenderEngine._extract_flipped_region(full, 4, 4, 1, 1, 2, 2)
        self.assertEqual(len(cropped), 2 * 2 * 4)
        # Bottom row in Blender (dst_y=0) is RISE bottom row (src_y=2)
        self.assertEqual(tuple(cropped[0:4]), (1.0, 2.0, 0.0, 1.0))
        self.assertEqual(tuple(cropped[4:8]), (2.0, 2.0, 0.0, 1.0))
        # Top row in Blender (dst_y=1) is RISE top row (src_y=1)
        self.assertEqual(tuple(cropped[8:12]), (1.0, 1.0, 0.0, 1.0))
        self.assertEqual(tuple(cropped[12:16]), (2.0, 1.0, 0.0, 1.0))

    def test_apply_region_to_buffer_clips_to_crop_window(self):
        # 2x2 crop window at (crop_left=1, crop_top=1) inside 4x4 full image
        buf = array.array("f", [0.0]) * (2 * 2 * 4)
        # Incoming 2x2 tile at (region_left=0, region_top=0) overlaps only pixel (1,1)
        tile = array.array("H", [
            100, 0, 0, 65535,   200, 0, 0, 65535,
            300, 0, 0, 65535, 65535, 0, 0, 65535,
        ]).tobytes()
        RISEBlenderRenderEngine._apply_region_to_buffer(
            buf, tile, 2, 2, 4, 4, 0, 0,
            crop_left=1, crop_top=1, crop_width=2, crop_height=2,
        )
        # Full (1,1) -> crop local (0,0) -> bottom-up dst_row = 2-1-0 = 1, dst_col = 0
        idx = (1 * 2 + 0) * 4
        self.assertAlmostEqual(buf[idx], 1.0, places=4)
        self.assertAlmostEqual(buf[idx + 3], 1.0, places=4)
        # All other crop pixels remain untouched
        self.assertEqual(buf[0], 0.0)


class BridgeAbiV19Test(unittest.TestCase):
    def test_abi_v19_fields_present(self):
        self.assertEqual(bridge._EXPECTED_API_VERSION, 19)
        painter_fields = [f[0] for f in bridge._Painter._fields_]
        self.assertIn("expression", painter_fields)
        settings_fields = [f[0] for f in bridge._RenderSettings._fields_]
        for name in ("use_region", "region_left", "region_top", "region_right", "region_bottom", "film_transparent"):
            self.assertIn(name, settings_fields)


class ProceduralExprTest(unittest.TestCase):
    def test_voronoi_color_unlinked_vector_uses_generated(self):
        vor = _make_voronoi(dims="3D", feature="F1", distance="EUCLIDEAN", scale=4.0)
        dst = _FakeSocket("Base Color", "RGBA")
        dst.links.append(_FakeLink(vor, vor.outputs["Color"]))
        res = procedural_expr.compile_socket_expression(
            dst, want_type="vec3", bbox_min=(-1.0, -1.0, -1.0), bbox_max=(1.0, 1.0, 1.0)
        )
        self.assertIsNotNone(res)
        self.assertTrue(res.uses_generated)
        self.assertIn("osl_voronoi_color(", res.expression)
        self.assertIn("vec3(0.5, 0.5, 0.5)", res.expression)

    def test_voronoi_object_coords_does_not_use_generated(self):
        tc = _FakeNode("ShaderNodeTexCoord", outputs=[_FakeSocket("Object", "VECTOR")])
        vor = _make_voronoi(dims="2D", feature="DISTANCE_TO_EDGE", distance="MANHATTAN", normalize=True, scale=3.0)
        _link(tc, "Object", vor, "Vector")
        dst = _FakeSocket("Roughness", "VALUE")
        dst.links.append(_FakeLink(vor, vor.outputs["Distance"]))
        res = procedural_expr.compile_socket_expression(dst, want_type="scalar")
        self.assertIsNotNone(res)
        self.assertFalse(res.uses_generated)
        self.assertIn("osl_voronoi_distance(Po, 0, 2, 3, 1, 3,", res.expression)

    def test_mix_and_huesat_and_diamond_dedup(self):
        vor = _make_voronoi(scale=6.0)
        hsv = _FakeNode(
            "ShaderNodeHueSaturation",
            inputs=[
                _FakeSocket("Hue", "VALUE", 0.5),
                _FakeSocket("Saturation", "VALUE", 1.2),
                _FakeSocket("Value", "VALUE", 1.0),
                _FakeSocket("Fac", "VALUE", 1.0),
                _FakeSocket("Color", "RGBA", (0.5, 0.5, 0.5, 1.0)),
            ],
            outputs=[_FakeSocket("Color", "RGBA")],
        )
        _link(vor, "Color", hsv, "Color")
        mix = _FakeNode(
            "ShaderNodeMix",
            inputs=[
                _FakeSocket("Factor", "VALUE", 0.5),
                _FakeSocket("A", "RGBA", (0.0, 0.0, 0.0, 1.0)),
                _FakeSocket("B", "RGBA", (1.0, 1.0, 1.0, 1.0)),
            ],
            outputs=[_FakeSocket("Result", "RGBA")],
            data_type="RGBA",
            blend_type="OVERLAY",
            clamp_factor=True,
            clamp_result=False,
        )
        _link(vor, "Color", mix, "A", dst_type="RGBA")
        _link(hsv, "Color", mix, "B", dst_type="RGBA")
        dst = _FakeSocket("Base Color", "RGBA")
        dst.links.append(_FakeLink(mix, mix.outputs["Result"]))
        res = procedural_expr.compile_socket_expression(dst, want_type="vec3")
        self.assertIsNotNone(res)
        # Diamond graph: Voronoi must only be called ONCE via a `def` binding.
        self.assertEqual(res.expression.count("osl_voronoi_color("), 1)
        self.assertIn("osl_rgb2hsv(", res.expression)
        self.assertIn("osl_hsv2rgb(", res.expression)
        self.assertIn("osl_mix_color(9,", res.expression)

    def test_unsupported_node_fails_closed(self):
        chk = _FakeNode("ShaderNodeTexChecker", outputs=[_FakeSocket("Color", "RGBA")])
        dst = _FakeSocket("Base Color", "RGBA")
        dst.links.append(_FakeLink(chk, chk.outputs["Color"]))
        self.assertIsNone(procedural_expr.compile_socket_expression(dst, want_type="vec3"))

    def test_combine_and_separate_color_real_blender_rgb_socket_names_in_hsv_hsl(self):
        # In Blender 3.3+/4.x/5.x, CombineColor & SeparateColor keep internal
        # socket names "Red", "Green", "Blue" even when mode is HSV or HSL.
        vor = _make_voronoi(scale=3.0)
        sep = _FakeNode(
            "ShaderNodeSeparateColor",
            inputs=[_FakeSocket("Color", "RGBA", (0.5, 0.5, 0.5, 1.0))],
            outputs=[
                _FakeSocket("Red", "VALUE"),
                _FakeSocket("Green", "VALUE"),
                _FakeSocket("Blue", "VALUE"),
            ],
            mode="HSV",
        )
        _link(vor, "Color", sep, "Color")
        comb = _FakeNode(
            "ShaderNodeCombineColor",
            inputs=[
                _FakeSocket("Red", "VALUE", 0.1),
                _FakeSocket("Green", "VALUE", 0.8),
                _FakeSocket("Blue", "VALUE", 0.9),
            ],
            outputs=[_FakeSocket("Color", "RGBA")],
            mode="HSV",
        )
        _link(sep, "Red", comb, "Red")
        _link(sep, "Blue", comb, "Blue")
        dst = _FakeSocket("Base Color", "RGBA")
        dst.links.append(_FakeLink(comb, comb.outputs["Color"]))
        res = procedural_expr.compile_socket_expression(dst, want_type="vec3")
        self.assertIsNotNone(res)
        # Shared osl_rgb2hsv binding reused across both channels of SeparateColor.
        self.assertEqual(res.expression.count("osl_rgb2hsv("), 1)
        self.assertIn("osl_hsv2rgb(", res.expression)
        self.assertIn("0.8", res.expression)

        # HSL mode with real Blender socket names as well
        sep.mode = "HSL"
        comb.mode = "HSL"
        res_hsl = procedural_expr.compile_socket_expression(dst, want_type="vec3")
        self.assertIsNotNone(res_hsl)
        self.assertEqual(res_hsl.expression.count("osl_rgb2hsl("), 1)
        self.assertIn("osl_hsl2rgb(", res_hsl.expression)

    def test_missing_required_socket_fails_closed(self):
        broken_comb = _FakeNode(
            "ShaderNodeCombineColor",
            inputs=[_FakeSocket("Red", "VALUE", 0.5)],  # missing Green & Blue
            outputs=[_FakeSocket("Color", "RGBA")],
            mode="HSV",
        )
        dst = _FakeSocket("Base Color", "RGBA")
        dst.links.append(_FakeLink(broken_comb, broken_comb.outputs["Color"]))
        self.assertIsNone(procedural_expr.compile_socket_expression(dst, want_type="vec3"))

    def test_white_noise_unlinked_vector_uses_default_not_generated(self):
        wn = _FakeNode(
            "ShaderNodeTexWhiteNoise",
            inputs=[
                _FakeSocket("Vector", "VECTOR", (1.25, 2.5, 3.75)),
                _FakeSocket("W", "VALUE", 0.5),
            ],
            outputs=[_FakeSocket("Value", "VALUE"), _FakeSocket("Color", "RGBA")],
            noise_dimensions="3D",
        )
        dst = _FakeSocket("Base Color", "RGBA")
        dst.links.append(_FakeLink(wn, wn.outputs["Color"]))
        res = procedural_expr.compile_socket_expression(
            dst, want_type="vec3", bbox_min=(-1, -1, -1), bbox_max=(1, 1, 1)
        )
        self.assertIsNotNone(res)
        self.assertFalse(res.uses_generated)
        self.assertIn("osl_white_noise_color(vec3(1.25, 2.5, 3.75), 0.5, 3)", res.expression)

    def test_1d_voronoi_and_noise_do_not_mark_uses_generated(self):
        vor1 = _make_voronoi(dims="1D", scale=2.0)
        dst = _FakeSocket("Base Color", "RGBA")
        dst.links.append(_FakeLink(vor1, vor1.outputs["Color"]))
        res = procedural_expr.compile_socket_expression(
            dst, want_type="vec3", bbox_min=(-2, -2, -2), bbox_max=(2, 2, 2)
        )
        self.assertIsNotNone(res)
        self.assertFalse(res.uses_generated)
        self.assertNotIn("Po", res.expression)

        noise1 = _FakeNode(
            "ShaderNodeTexNoise",
            inputs=[
                _FakeSocket("Vector", "VECTOR", (0.0, 0.0, 0.0)),
                _FakeSocket("W", "VALUE", 1.5),
                _FakeSocket("Scale", "VALUE", 3.0),
                _FakeSocket("Detail", "VALUE", 2.0),
                _FakeSocket("Roughness", "VALUE", 0.5),
                _FakeSocket("Lacunarity", "VALUE", 2.0),
                _FakeSocket("Offset", "VALUE", 0.0),
                _FakeSocket("Gain", "VALUE", 1.0),
                _FakeSocket("Distortion", "VALUE", 0.0),
            ],
            outputs=[_FakeSocket("Fac", "VALUE"), _FakeSocket("Color", "RGBA")],
            noise_dimensions="1D",
            noise_type="FBM",
            normalize=True,
        )
        dst2 = _FakeSocket("Roughness", "VALUE")
        dst2.links.append(_FakeLink(noise1, noise1.outputs["Fac"]))
        res2 = procedural_expr.compile_socket_expression(dst2, want_type="scalar")
        self.assertIsNotNone(res2)
        self.assertFalse(res2.uses_generated)
        self.assertIn("osl_noise_texture(", res2.expression)

    def test_texcoord_normal_and_from_instancer_fail_closed(self):
        tc_norm = _FakeNode("ShaderNodeTexCoord", outputs=[_FakeSocket("Normal", "VECTOR")])
        dst = _FakeSocket("Base Color", "RGBA")
        dst.links.append(_FakeLink(tc_norm, tc_norm.outputs["Normal"]))
        self.assertIsNone(procedural_expr.compile_socket_expression(dst, want_type="vec3"))

        tc_inst = _FakeNode(
            "ShaderNodeTexCoord",
            outputs=[_FakeSocket("Generated", "VECTOR")],
            from_instancer=True,
        )
        dst2 = _FakeSocket("Base Color", "RGBA")
        dst2.links.append(_FakeLink(tc_inst, tc_inst.outputs["Generated"]))
        self.assertIsNone(procedural_expr.compile_socket_expression(dst2, want_type="vec3"))

    def test_mapping_all_modes_and_linked_transform_fail_closed(self):
        tc = _FakeNode("ShaderNodeTexCoord", outputs=[_FakeSocket("Object", "VECTOR")])
        for vtype in ("POINT", "TEXTURE", "VECTOR", "NORMAL"):
            mp = _FakeNode(
                "ShaderNodeMapping",
                inputs=[
                    _FakeSocket("Vector", "VECTOR", (0.0, 0.0, 0.0)),
                    _FakeSocket("Location", "VECTOR", (0.5, -0.25, 0.0)),
                    _FakeSocket("Rotation", "VECTOR", (0.0, 0.0, 0.5)),
                    _FakeSocket("Scale", "VECTOR", (2.0, 1.0, 0.5)),
                ],
                outputs=[_FakeSocket("Vector", "VECTOR")],
                vector_type=vtype,
            )
            _link(tc, "Object", mp, "Vector")
            dst = _FakeSocket("Base Color", "RGBA")
            dst.links.append(_FakeLink(mp, mp.outputs["Vector"]))
            res = procedural_expr.compile_socket_expression(dst, want_type="vec3")
            self.assertIsNotNone(res, f"Mapping {vtype} should compile")
            if vtype == "NORMAL":
                self.assertIn("normalize(", res.expression)

        # Linked Location fails closed
        mp_bad = _FakeNode(
            "ShaderNodeMapping",
            inputs=[
                _FakeSocket("Vector", "VECTOR", (0.0, 0.0, 0.0)),
                _FakeSocket("Location", "VECTOR", (0.0, 0.0, 0.0)),
                _FakeSocket("Rotation", "VECTOR", (0.0, 0.0, 0.0)),
                _FakeSocket("Scale", "VECTOR", (1.0, 1.0, 1.0)),
            ],
            outputs=[_FakeSocket("Vector", "VECTOR")],
            vector_type="POINT",
        )
        _link(tc, "Object", mp_bad, "Location")
        dst_bad = _FakeSocket("Base Color", "RGBA")
        dst_bad.links.append(_FakeLink(mp_bad, mp_bad.outputs["Vector"]))
        self.assertIsNone(procedural_expr.compile_socket_expression(dst_bad, want_type="vec3"))

    def test_color_ramp_and_color_adjust_and_xyz_and_mix_variants(self):
        vor = _make_voronoi(scale=2.0)
        ramp = _FakeNode(
            "ShaderNodeValToRGB",
            inputs=[_FakeSocket("Fac", "VALUE", 0.5)],
            outputs=[_FakeSocket("Color", "RGBA"), _FakeSocket("Alpha", "VALUE")],
            color_ramp=types.SimpleNamespace(
                color_mode="RGB",
                interpolation="LINEAR",
                elements=[
                    types.SimpleNamespace(position=0.2, color=(0.1, 0.2, 0.3, 1.0)),
                    types.SimpleNamespace(position=0.5, color=(0.5, 0.5, 0.5, 1.0)),
                    types.SimpleNamespace(position=0.5, color=(0.8, 0.2, 0.1, 1.0)),
                    types.SimpleNamespace(position=0.9, color=(1.0, 1.0, 1.0, 1.0)),
                ],
            ),
        )
        _link(vor, "Distance", ramp, "Fac")
        inv = _FakeNode(
            "ShaderNodeInvert",
            inputs=[_FakeSocket("Fac", "VALUE", 0.5), _FakeSocket("Color", "RGBA", (0.5, 0.5, 0.5, 1.0))],
            outputs=[_FakeSocket("Color", "RGBA")],
        )
        _link(ramp, "Color", inv, "Color")
        bc = _FakeNode(
            "ShaderNodeBrightContrast",
            inputs=[
                _FakeSocket("Color", "RGBA", (0.5, 0.5, 0.5, 1.0)),
                _FakeSocket("Bright", "VALUE", 0.1),
                _FakeSocket("Contrast", "VALUE", 0.2),
            ],
            outputs=[_FakeSocket("Color", "RGBA")],
        )
        _link(inv, "Color", bc, "Color")
        gam = _FakeNode(
            "ShaderNodeGamma",
            inputs=[_FakeSocket("Color", "RGBA", (0.5, 0.5, 0.5, 1.0)), _FakeSocket("Gamma", "VALUE", 2.2)],
            outputs=[_FakeSocket("Color", "RGBA")],
        )
        _link(bc, "Color", gam, "Color")
        sep_xyz = _FakeNode(
            "ShaderNodeSeparateXYZ",
            inputs=[_FakeSocket("Vector", "VECTOR", (0.0, 0.0, 0.0))],
            outputs=[_FakeSocket("X", "VALUE"), _FakeSocket("Y", "VALUE"), _FakeSocket("Z", "VALUE")],
        )
        _link(gam, "Color", sep_xyz, "Vector")
        mix_f = _FakeNode(
            "ShaderNodeMix",
            inputs=[
                _FakeSocket("Factor", "VALUE", 0.5),
                _FakeSocket("A", "VALUE", 0.0),
                _FakeSocket("B", "VALUE", 1.0),
            ],
            outputs=[_FakeSocket("Result", "VALUE")],
            data_type="FLOAT",
            clamp_factor=True,
        )
        _link(sep_xyz, "X", mix_f, "A", dst_type="VALUE")
        comb_xyz = _FakeNode(
            "ShaderNodeCombineXYZ",
            inputs=[_FakeSocket("X", "VALUE", 0.0), _FakeSocket("Y", "VALUE", 0.5), _FakeSocket("Z", "VALUE", 1.0)],
            outputs=[_FakeSocket("Vector", "VECTOR")],
        )
        _link(mix_f, "Result", comb_xyz, "X")
        mix_rgb = _FakeNode(
            "ShaderNodeMixRGB",
            inputs=[
                _FakeSocket("Fac", "VALUE", 0.5),
                _FakeSocket("Color1", "RGBA", (0.0, 0.0, 0.0, 1.0)),
                _FakeSocket("Color2", "RGBA", (1.0, 1.0, 1.0, 1.0)),
            ],
            outputs=[_FakeSocket("Color", "RGBA")],
            blend_type="MULTIPLY",
            use_clamp=True,
        )
        _link(comb_xyz, "Vector", mix_rgb, "Color1")
        dst = _FakeSocket("Base Color", "RGBA")
        dst.links.append(_FakeLink(mix_rgb, mix_rgb.outputs["Color"]))
        res = procedural_expr.compile_socket_expression(dst, want_type="vec3")
        self.assertIsNotNone(res)
        self.assertIn("osl_mix_color(2,", res.expression)
        self.assertIn("pow(max(", res.expression)

        # ColorRamp Alpha output fails closed
        dst_a = _FakeSocket("Roughness", "VALUE")
        dst_a.links.append(_FakeLink(ramp, ramp.outputs["Alpha"]))
        self.assertIsNone(procedural_expr.compile_socket_expression(dst_a, want_type="scalar"))

    def test_cycle_muted_and_slot_budget_fail_closed(self):
        # Cycle
        r1 = _FakeNode("NodeReroute", inputs=[_FakeSocket("Input")], outputs=[_FakeSocket("Output")])
        r2 = _FakeNode("NodeReroute", inputs=[_FakeSocket("Input")], outputs=[_FakeSocket("Output")])
        _link(r1, "Output", r2, "Input")
        _link(r2, "Output", r1, "Input")
        dst = _FakeSocket("Base Color", "RGBA")
        dst.links.append(_FakeLink(r1, r1.outputs["Output"]))
        self.assertIsNone(procedural_expr.compile_socket_expression(dst, want_type="vec3"))

        # Muted node fails closed
        vor = _make_voronoi()
        vor.mute = True
        dst2 = _FakeSocket("Base Color", "RGBA")
        dst2.links.append(_FakeLink(vor, vor.outputs["Color"]))
        self.assertIsNone(procedural_expr.compile_socket_expression(dst2, want_type="vec3"))

        # Slot budget (> 495 slots -> fails closed instead of aborting native compile)
        tc = _FakeNode("ShaderNodeTexCoord", outputs=[_FakeSocket("Object", "VECTOR")])
        prev = tc
        prev_out = "Object"
        # Build a wide chain of 170 distinct MixRGB nodes (each binds 3 slots = 510 > 495)
        for _ in range(170):
            m = _FakeNode(
                "ShaderNodeMixRGB",
                inputs=[
                    _FakeSocket("Fac", "VALUE", 0.5),
                    _FakeSocket("Color1", "RGBA", (0.0, 0.0, 0.0, 1.0)),
                    _FakeSocket("Color2", "RGBA", (1.0, 1.0, 1.0, 1.0)),
                ],
                outputs=[_FakeSocket("Color", "RGBA")],
                blend_type="MIX",
            )
            _link(prev, prev_out, m, "Color1")
            prev = m
            prev_out = "Color"
        dst3 = _FakeSocket("Base Color", "RGBA")
        dst3.links.append(_FakeLink(prev, prev.outputs[prev_out]))
        # Raise max_depth slightly so slot budget is what trips, not max_depth
        comp = procedural_expr._Compiler(None, None, max_depth=256)
        with self.assertRaises(procedural_expr._UnsupportedGraph):
            comp.eval_node_output(prev, prev_out, 0)

    def test_paired_emission_color_and_strength_product(self):
        vor = _make_voronoi(scale=4.0)
        ec = _FakeSocket("Emission Color", "RGBA")
        es = _FakeSocket("Emission Strength", "VALUE")
        ec.links.append(_FakeLink(vor, vor.outputs["Color"]))
        es.links.append(_FakeLink(vor, vor.outputs["Distance"]))
        res = procedural_expr.compile_socket_product_expression(ec, es)
        self.assertIsNotNone(res)
        self.assertIn("osl_voronoi_color(", res.expression)
        self.assertIn("osl_voronoi_distance(", res.expression)
        self.assertIn(") * (", res.expression)

    def test_scalar_socket_tinted_by_rgb_coerces_color_to_luminance_first(self):
        # Emission Strength driven by Voronoi.Color with unlinked Emission Color
        # must coerce Color -> Rec.709 luminance FIRST, then multiply by RGB.
        vor = _make_voronoi(scale=3.0)
        es = _FakeSocket("Emission Strength", "VALUE")
        es.links.append(_FakeLink(vor, vor.outputs["Color"]))
        res = procedural_expr.compile_socket_expression(
            es, want_type="scalar", scale=(1.0, 0.5, 0.25)
        )
        self.assertIsNotNone(res)
        self.assertIn("dot(", res.expression)
        self.assertIn("vec3(0.2126, 0.7152, 0.0722)", res.expression)
        self.assertIn("vec3(1, 0.5, 0.25)", res.expression)

    def test_separate_xyz_shares_single_binding_across_channels(self):
        vor = _make_voronoi(scale=2.0)
        sep = _FakeNode(
            "ShaderNodeSeparateXYZ",
            inputs=[_FakeSocket("Vector", "VECTOR", (0.0, 0.0, 0.0))],
            outputs=[_FakeSocket("X", "VALUE"), _FakeSocket("Y", "VALUE"), _FakeSocket("Z", "VALUE")],
        )
        _link(vor, "Position", sep, "Vector")
        comb = _FakeNode(
            "ShaderNodeCombineXYZ",
            inputs=[_FakeSocket("X", "VALUE", 0.0), _FakeSocket("Y", "VALUE", 0.0), _FakeSocket("Z", "VALUE", 0.0)],
            outputs=[_FakeSocket("Vector", "VECTOR")],
        )
        _link(sep, "X", comb, "X")
        _link(sep, "Y", comb, "Y")
        _link(sep, "Z", comb, "Z")
        dst = _FakeSocket("Base Color", "RGBA")
        dst.links.append(_FakeLink(comb, comb.outputs["Vector"]))
        res = procedural_expr.compile_socket_expression(dst, want_type="vec3")
        self.assertIsNotNone(res)
        self.assertEqual(res.expression.count("osl_voronoi_position("), 1)
        # Only one SeparateXYZ binding (plus Generated coord + Voronoi + CombineXYZ = 4 defs total)
        def_lines = [ln for ln in res.expression.splitlines() if ln.startswith("def ")]
        self.assertLessEqual(len(def_lines), 4)


class BakeEligibilityTest(unittest.TestCase):
    def test_voronoi_white_noise_and_sepcomb_classify_simple(self):
        def wire(principled, nodes):
            wn = _FakeNode(
                "ShaderNodeTexWhiteNoise",
                inputs=[_FakeSocket("Vector", "VECTOR", (0, 0, 0)), _FakeSocket("W", "VALUE", 0.0)],
                outputs=[_FakeSocket("Value", "VALUE"), _FakeSocket("Color", "RGBA")],
                noise_dimensions="3D",
            )
            sep = _FakeNode(
                "ShaderNodeSeparateColor",
                inputs=[_FakeSocket("Color", "RGBA")],
                outputs=[_FakeSocket("Red", "VALUE"), _FakeSocket("Green", "VALUE"), _FakeSocket("Blue", "VALUE")],
                mode="HSV",
            )
            comb = _FakeNode(
                "ShaderNodeCombineColor",
                inputs=[_FakeSocket("Red", "VALUE"), _FakeSocket("Green", "VALUE"), _FakeSocket("Blue", "VALUE")],
                outputs=[_FakeSocket("Color", "RGBA")],
                mode="HSV",
            )
            nodes.extend([wn, sep, comb])
            _link(wn, "Color", sep, "Color")
            _link(sep, "Red", comb, "Red")
            _link(comb, "Color", principled, "Base Color")

        mat = _make_material(wire)
        self.assertEqual(material_bake.classify_material(mat), "simple")
        self.assertTrue(material_bake.has_procedural_texture_nodes(mat))

    def test_voronoi_is_bake_eligible_for_operator(self):
        def wire(principled, nodes):
            vor = _make_voronoi()
            nodes.append(vor)
            _link(vor, "Color", principled, "Base Color")

        mat = _make_material(wire)
        self.assertEqual(material_bake.classify_material(mat), "simple")
        self.assertTrue(material_bake.has_procedural_texture_nodes(mat))

    def test_muted_intermediate_still_finds_upstream_voronoi(self):
        def wire(principled, nodes):
            vor = _make_voronoi()
            ramp = _FakeNode(
                "ShaderNodeValToRGB",
                inputs=[_FakeSocket("Fac", "VALUE", 0.5)],
                outputs=[_FakeSocket("Color", "RGBA")],
            )
            ramp.mute = True
            nodes.extend([vor, ramp])
            _link(vor, "Distance", ramp, "Fac")
            _link(ramp, "Color", principled, "Base Color")

        mat = _make_material(wire)
        self.assertTrue(material_bake.has_procedural_texture_nodes(mat))

    def test_muted_voronoi_itself_not_eligible(self):
        def wire(principled, nodes):
            vor = _make_voronoi()
            vor.mute = True
            nodes.append(vor)
            _link(vor, "Color", principled, "Base Color")

        mat = _make_material(wire)
        self.assertFalse(material_bake.has_procedural_texture_nodes(mat))

    def test_muted_link_not_eligible(self):
        def wire(principled, nodes):
            vor = _make_voronoi()
            nodes.append(vor)
            _link(vor, "Color", principled, "Base Color", muted=True)

        mat = _make_material(wire)
        self.assertFalse(material_bake.has_procedural_texture_nodes(mat))

    def test_tangent_only_voronoi_excluded(self):
        def wire(principled, nodes):
            vor = _make_voronoi()
            nodes.append(vor)
            _link(vor, "Color", principled, "Tangent")

        mat = _make_material(wire)
        self.assertFalse(material_bake.has_procedural_texture_nodes(mat))


if __name__ == "__main__":
    unittest.main()
