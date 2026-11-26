#!/usr/bin/env python3
"""Measure lowered size (def count, slot count, AST depth, text bytes) of
Blender procedural shader graphs compiled by `procedural_expr.py`.

Directly measures design doc §8's lowered-size questions comparing naive
bind-every-node lowering (`inline_single_use=False`) against single-use
inlining + depth-splitting (`inline_single_use=True`, the default).

Run from repo root:
    python3 tools/measure_procedural_lowering.py
"""

from __future__ import annotations

import os
import sys
import types

_REPO_ROOT = os.path.abspath(os.path.join(os.path.dirname(__file__), ".."))
_ADDON_DIR = os.path.join(_REPO_ROOT, "src", "Blender", "addons", "rise_renderer")


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
    if "rise_renderer" not in sys.modules:
        pkg = types.ModuleType("rise_renderer")
        pkg.__path__ = [_ADDON_DIR]
        sys.modules["rise_renderer"] = pkg


_install_stubs()
from rise_renderer import procedural_expr  # noqa: E402


class _Link:
    def __init__(self, from_node, from_socket):
        self.from_node = from_node
        self.from_socket = from_socket
        self.is_muted = False


class _Socket:
    def __init__(self, name, sock_type="VALUE", default_value=0.0):
        self.name = name
        self.type = sock_type
        self.default_value = default_value
        self.links = []

    @property
    def is_linked(self):
        return bool(self.links)


class _Node:
    _ptr_seq = 10000

    def __init__(self, bl_idname, inputs=(), outputs=(), **attrs):
        _Node._ptr_seq += 16
        self._ptr = _Node._ptr_seq
        self.bl_idname = bl_idname
        self.name = bl_idname
        self.mute = False
        self.inputs = list(inputs)
        self.outputs = list(outputs)
        for k, v in attrs.items():
            setattr(self, k, v)

    def as_pointer(self):
        return self._ptr


def _link(src, src_out, dst, dst_in, dst_type=None):
    s = next(o for o in src.outputs if o.name == src_out)
    if isinstance(dst_in, int):
        d = dst.inputs[dst_in]
    else:
        d = next(i for i in dst.inputs if i.name == dst_in and (dst_type is None or i.type == dst_type))
    d.links.append(_Link(src, s))


def _make_noise(scale=5.0, detail=2.0):
    return _Node(
        "ShaderNodeTexNoise",
        inputs=[
            _Socket("Vector", "VECTOR", (0.0, 0.0, 0.0)),
            _Socket("W", "VALUE", 0.0),
            _Socket("Scale", "VALUE", scale),
            _Socket("Detail", "VALUE", detail),
            _Socket("Roughness", "VALUE", 0.5),
            _Socket("Lacunarity", "VALUE", 2.0),
            _Socket("Offset", "VALUE", 0.0),
            _Socket("Gain", "VALUE", 1.0),
            _Socket("Distortion", "VALUE", 0.0),
        ],
        outputs=[_Socket("Fac", "VALUE"), _Socket("Color", "RGBA")],
        noise_dimensions="3D",
        noise_type="FBM",
        normalize=True,
    )


def _make_voronoi(scale=6.0):
    return _Node(
        "ShaderNodeTexVoronoi",
        inputs=[
            _Socket("Vector", "VECTOR", (0.0, 0.0, 0.0)),
            _Socket("W", "VALUE", 0.0),
            _Socket("Scale", "VALUE", scale),
            _Socket("Detail", "VALUE", 0.0),
            _Socket("Roughness", "VALUE", 0.5),
            _Socket("Lacunarity", "VALUE", 2.0),
            _Socket("Smoothness", "VALUE", 1.0),
            _Socket("Exponent", "VALUE", 0.5),
            _Socket("Randomness", "VALUE", 1.0),
        ],
        outputs=[
            _Socket("Distance", "VALUE"),
            _Socket("Color", "RGBA"),
            _Socket("Position", "VECTOR"),
            _Socket("W", "VALUE"),
            _Socket("Radius", "VALUE"),
        ],
        voronoi_dimensions="3D",
        feature="F1",
        distance="EUCLIDEAN",
        normalize=False,
    )


def _make_ramp(color_mode="RGB", interp="LINEAR", stops=3):
    elems = [
        types.SimpleNamespace(
            position=i / max(1, stops - 1),
            color=(i / max(1, stops - 1), 0.3, 1.0 - i / max(1, stops - 1), 1.0),
        )
        for i in range(stops)
    ]
    return _Node(
        "ShaderNodeValToRGB",
        inputs=[_Socket("Fac", "VALUE", 0.5)],
        outputs=[_Socket("Color", "RGBA"), _Socket("Alpha", "VALUE")],
        color_ramp=types.SimpleNamespace(
            color_mode=color_mode, interpolation=interp, hue_interpolation="NEAR", elements=elems
        ),
    )


def _count_reachable_nodes(socket) -> int:
    """Count distinct non-reroute nodes reachable upstream of ``socket``."""
    seen: set[int] = set()
    stack = [lnk.from_node for lnk in socket.links if not lnk.is_muted and lnk.from_node is not None]
    while stack:
        node = stack.pop()
        nid = node.as_pointer()
        if nid in seen:
            continue
        if getattr(node, "bl_idname", "") != "NodeReroute":
            seen.add(nid)
        for inp in getattr(node, "inputs", ()):
            for lnk in getattr(inp, "links", ()):
                if not lnk.is_muted and lnk.from_node is not None:
                    stack.append(lnk.from_node)
    return len(seen)


def _build_8_node_material():
    """Median Phase 1 material (8 nodes): TexCoord -> Mapping -> Noise -> Ramp + Math -> Clamp -> Mix -> HueSat."""
    tc = _Node("ShaderNodeTexCoord", outputs=[_Socket("Object", "VECTOR")])
    mp = _Node(
        "ShaderNodeMapping",
        inputs=[
            _Socket("Vector", "VECTOR"),
            _Socket("Location", "VECTOR", (0.1, 0.2, 0.3)),
            _Socket("Rotation", "VECTOR", (0.0, 0.0, 0.4)),
            _Socket("Scale", "VECTOR", (2.0, 2.0, 2.0)),
        ],
        outputs=[_Socket("Vector", "VECTOR")],
        vector_type="POINT",
    )
    _link(tc, "Object", mp, "Vector")
    ns = _make_noise(scale=4.0, detail=3.0)
    _link(mp, "Vector", ns, "Vector")
    rmp = _make_ramp(stops=4)
    _link(ns, "Fac", rmp, "Fac")
    m = _Node(
        "ShaderNodeMath",
        inputs=[_Socket("Value", "VALUE"), _Socket("Value", "VALUE", 0.5), _Socket("Value", "VALUE", 0.0)],
        outputs=[_Socket("Value", "VALUE")],
        operation="MULTIPLY",
        use_clamp=False,
    )
    _link(ns, "Fac", m, 0)
    cl = _Node(
        "ShaderNodeClamp",
        inputs=[_Socket("Value", "VALUE"), _Socket("Min", "VALUE", 0.0), _Socket("Max", "VALUE", 1.0)],
        outputs=[_Socket("Result", "VALUE")],
        clamp_type="MINMAX",
    )
    _link(m, "Value", cl, "Value")
    mx = _Node(
        "ShaderNodeMix",
        inputs=[
            _Socket("Factor", "VALUE"),
            _Socket("A", "RGBA"),
            _Socket("B", "RGBA", (0.8, 0.4, 0.2, 1.0)),
        ],
        outputs=[_Socket("Result", "RGBA")],
        data_type="RGBA",
        blend_type="OVERLAY",
    )
    _link(cl, "Result", mx, "Factor", dst_type="VALUE")
    _link(rmp, "Color", mx, "A", dst_type="RGBA")
    hs = _Node(
        "ShaderNodeHueSaturation",
        inputs=[
            _Socket("Hue", "VALUE", 0.52),
            _Socket("Saturation", "VALUE", 1.1),
            _Socket("Value", "VALUE", 0.95),
            _Socket("Fac", "VALUE", 1.0),
            _Socket("Color", "RGBA"),
        ],
        outputs=[_Socket("Color", "RGBA")],
    )
    _link(mx, "Result", hs, "Color")
    dst = _Socket("Base Color", "RGBA")
    dst.links.append(_Link(hs, hs.outputs[0]))
    return _count_reachable_nodes(dst), dst, "vec3"


def _build_36_node_material():
    """p75 Phase 1 material (36 nodes): warped Noise + Voronoi + VectorRotate + MapRange + HSV Ramp + Mix."""
    tc = _Node("ShaderNodeTexCoord", outputs=[_Socket("Object", "VECTOR"), _Socket("Normal", "VECTOR")])
    vt = _Node(
        "ShaderNodeVectorTransform",
        inputs=[_Socket("Vector", "VECTOR")],
        outputs=[_Socket("Vector", "VECTOR")],
        vector_type="POINT",
        convert_from="OBJECT",
        convert_to="WORLD",
    )
    _link(tc, "Object", vt, "Vector")
    vr = _Node(
        "ShaderNodeVectorRotate",
        inputs=[
            _Socket("Vector", "VECTOR"),
            _Socket("Center", "VECTOR", (0.0, 0.0, 0.0)),
            _Socket("Axis", "VECTOR", (0.0, 1.0, 0.0)),
            _Socket("Angle", "VALUE", 0.5),
            _Socket("Rotation", "VECTOR", (0.1, 0.2, 0.3)),
        ],
        outputs=[_Socket("Vector", "VECTOR")],
        rotation_type="EULER_XYZ",
    )
    _link(vt, "Vector", vr, "Vector")
    # Layer 1: domain-warped Noise
    n1 = _make_noise(scale=3.0, detail=2.0)
    _link(vr, "Vector", n1, "Vector")
    vm_warp = _Node(
        "ShaderNodeVectorMath",
        inputs=[
            _Socket("Vector", "VECTOR"),
            _Socket("Vector", "VECTOR", (0.25, 0.25, 0.25)),
            _Socket("Vector", "VECTOR"),
            _Socket("Scale", "VALUE", 1.0),
        ],
        outputs=[_Socket("Vector", "VECTOR"), _Socket("Value", "VALUE")],
        operation="MULTIPLY_ADD",
    )
    _link(n1, "Color", vm_warp, 0)
    _link(vr, "Vector", vm_warp, 2)
    n2 = _make_noise(scale=6.0, detail=4.0)
    _link(vm_warp, "Vector", n2, "Vector")
    # Layer 2: Voronoi cells
    vor = _make_voronoi(scale=8.0)
    _link(vm_warp, "Vector", vor, "Vector")
    # Combine 4 parallel branches through Math/MapRange/Clamp/Ramp/Mix chain (total 36 nodes)
    mr1 = _Node(
        "ShaderNodeMapRange",
        inputs=[
            _Socket("Value", "VALUE"),
            _Socket("From Min", "VALUE", 0.2),
            _Socket("From Max", "VALUE", 0.8),
            _Socket("To Min", "VALUE", 0.0),
            _Socket("To Max", "VALUE", 1.0),
            _Socket("Steps", "VALUE", 4.0),
        ],
        outputs=[_Socket("Result", "VALUE")],
        data_type="FLOAT",
        interpolation_type="SMOOTHERSTEP",
        clamp=True,
    )
    _link(n2, "Fac", mr1, "Value", dst_type="VALUE")
    prev_s = mr1
    prev_out = "Result"
    for i in range(12):
        op = ("MULTIPLY", "ADD", "POWER", "SMOOTH_MIN")[i % 4]
        m = _Node(
            "ShaderNodeMath",
            inputs=[_Socket("Value", "VALUE"), _Socket("Value", "VALUE", 0.7), _Socket("Value", "VALUE", 0.1)],
            outputs=[_Socket("Value", "VALUE")],
            operation=op,
            use_clamp=(i % 3 == 0),
        )
        _link(prev_s, prev_out, m, 0)
        prev_s, prev_out = m, "Value"
    cl = _Node(
        "ShaderNodeClamp",
        inputs=[_Socket("Value", "VALUE"), _Socket("Min", "VALUE", 0.05), _Socket("Max", "VALUE", 0.95)],
        outputs=[_Socket("Result", "VALUE")],
        clamp_type="MINMAX",
    )
    _link(prev_s, prev_out, cl, "Value")
    rmp1 = _make_ramp(color_mode="RGB", interp="EASE", stops=4)
    _link(cl, "Result", rmp1, "Fac")
    rmp2 = _make_ramp(color_mode="HSV", interp="LINEAR", stops=3)
    _link(vor, "Distance", rmp2, "Fac")
    prev_c = rmp1
    prev_cout = "Color"
    for i in range(12):
        mx = _Node(
            "ShaderNodeMix",
            inputs=[
                _Socket("Factor", "VALUE", 0.35),
                _Socket("A", "RGBA"),
                _Socket("B", "RGBA"),
            ],
            outputs=[_Socket("Result", "RGBA")],
            data_type="RGBA",
            blend_type=("MIX", "MULTIPLY", "OVERLAY", "SOFT_LIGHT")[i % 4],
            clamp_factor=True,
            clamp_result=True,
        )
        _link(cl, "Result", mx, "Factor", dst_type="VALUE")
        _link(prev_c, prev_cout, mx, "A", dst_type="RGBA")
        _link(rmp2 if i % 2 == 0 else vor, "Color", mx, "B", dst_type="RGBA")
        prev_c, prev_cout = mx, "Result"
    hs = _Node(
        "ShaderNodeHueSaturation",
        inputs=[
            _Socket("Hue", "VALUE", 0.5),
            _Socket("Saturation", "VALUE", 1.05),
            _Socket("Value", "VALUE", 1.0),
            _Socket("Fac", "VALUE", 1.0),
            _Socket("Color", "RGBA"),
        ],
        outputs=[_Socket("Color", "RGBA")],
    )
    _link(prev_c, prev_cout, hs, "Color")
    dst = _Socket("Base Color", "RGBA")
    dst.links.append(_Link(hs, hs.outputs[0]))
    return _count_reachable_nodes(dst), dst, "vec3"


def _build_diamond_dag(stages: int):
    """Diamond DAG: each stage consumes the previous stage TWICE (tests multi-use dedup)."""
    tc = _Node("ShaderNodeTexCoord", outputs=[_Socket("Object", "VECTOR")])
    prev = tc
    prev_out = "Object"
    for i in range(stages):
        vm = _Node(
            "ShaderNodeVectorMath",
            inputs=[
                _Socket("Vector", "VECTOR"),
                _Socket("Vector", "VECTOR"),
                _Socket("Vector", "VECTOR", (0.1, 0.2, 0.3)),
                _Socket("Scale", "VALUE", 0.5),
            ],
            outputs=[_Socket("Vector", "VECTOR"), _Socket("Value", "VALUE")],
            operation="ADD" if i % 2 == 0 else "MULTIPLY",
        )
        _link(prev, prev_out, vm, 0)
        _link(prev, prev_out, vm, 1)
        prev, prev_out = vm, "Vector"
    dst = _Socket("Base Color", "RGBA")
    dst.links.append(_Link(prev, prev.outputs[0]))
    return _count_reachable_nodes(dst), dst, "vec3"


def _build_deep_chain(length: int):
    """Linear deep chain exceeding RISE's kMaxParseDepth (200) if unsplit."""
    val = _Node("ShaderNodeValue", outputs=[_Socket("Value", "VALUE", 0.25)])
    prev = val
    for i in range(length):
        m = _Node(
            "ShaderNodeMath",
            inputs=[_Socket("Value", "VALUE"), _Socket("Value", "VALUE", 0.003), _Socket("Value", "VALUE", 0.0)],
            outputs=[_Socket("Value", "VALUE")],
            operation="ADD" if i % 2 == 0 else "MULTIPLY",
            use_clamp=False,
        )
        _link(prev, "Value", m, 0)
        prev = m
    dst = _Socket("Roughness", "VALUE")
    dst.links.append(_Link(prev, prev.outputs[0]))
    return _count_reachable_nodes(dst), dst, "scalar"


def _build_deep_vec3_chain(length: int):
    """Linear deep vec3 chain: overflows 495 slots under naive lowering (3*170=510), fits easily when inlined."""
    tc = _Node("ShaderNodeTexCoord", outputs=[_Socket("Object", "VECTOR")])
    prev = tc
    prev_out = "Object"
    for i in range(length):
        vm = _Node(
            "ShaderNodeVectorMath",
            inputs=[
                _Socket("Vector", "VECTOR"),
                _Socket("Vector", "VECTOR", (0.01, 0.02, 0.03)),
                _Socket("Vector", "VECTOR", (0.0, 0.0, 0.0)),
                _Socket("Scale", "VALUE", 1.0),
            ],
            outputs=[_Socket("Vector", "VECTOR"), _Socket("Value", "VALUE")],
            operation="ADD" if i % 2 == 0 else "MULTIPLY",
        )
        _link(prev, prev_out, vm, 0)
        prev, prev_out = vm, "Vector"
    dst = _Socket("Base Color", "RGBA")
    dst.links.append(_Link(prev, prev.outputs[0]))
    return _count_reachable_nodes(dst), dst, "vec3"


def get_compiled_benchmark_graphs():
    """Return compiled 8-node median and 36-node p75 graphs from procedural_expr.py."""
    n8, s8, t8 = _build_8_node_material()
    n36, s36, t36 = _build_36_node_material()
    c8 = procedural_expr.compile_socket_expression(s8, want_type=t8, inline_single_use=True)
    c36 = procedural_expr.compile_socket_expression(s36, want_type=t36, inline_single_use=True)
    assert c8 is not None and c36 is not None
    return (n8, c8), (n36, c36)


def _write_bench_inc(out_path: str) -> None:
    (n8, c8), (n36, c36) = get_compiled_benchmark_graphs()
    content = (
        "// Auto-generated by tools/measure_procedural_lowering.py --write-bench-inc\n"
        "// Verbatim expressions compiled by procedural_expr.py for the 8-node median\n"
        "// and 36-node p75 Phase 1 benchmark graphs.\n"
        f"static const int kBenchGraph8Nodes = {n8};\n"
        f"static const char kBenchGraph8Expr[] = R\"RISE_EXPR({c8.expression})RISE_EXPR\";\n\n"
        f"static const int kBenchGraph36Nodes = {n36};\n"
        f"static const char kBenchGraph36Expr[] = R\"RISE_EXPR({c36.expression})RISE_EXPR\";\n"
    )
    with open(out_path, "w", encoding="utf-8") as f:
        f.write(content)
    print(f"Wrote {out_path} (8-node={n8} nodes / {c8.def_count} defs / {len(c8.expression)} B, "
          f"36-node={n36} nodes / {c36.def_count} defs / {len(c36.expression)} B)")


def main():
    if "--write-bench-inc" in sys.argv:
        out_path = os.path.join(_REPO_ROOT, "tools", "osl_golden", "bench_procedural_graphs.inc")
        _write_bench_inc(out_path)
        return

    cases = [
        ("8-node median", *_build_8_node_material()),
        ("36-node p75 Phase 1", *_build_36_node_material()),
        ("60-stage vec3 diamond", *_build_diamond_dag(60)),
        ("150-stage vec3 diamond", *_build_diamond_dag(150)),
        ("170-node vec3 chain", *_build_deep_vec3_chain(170)),
        ("250-node scalar chain", *_build_deep_chain(250)),
    ]
    print(f"{'Graph':<24} {'Nodes':>5} | {'Inline defs':>11} {'slots':>6} {'depth':>6} {'bytes':>7} | {'Naive defs':>10} {'slots':>6} {'fits?':>6}")
    print("-" * 100)
    for label, n_nodes, sock, wtype in cases:
        r_in = procedural_expr.compile_socket_expression(sock, want_type=wtype, inline_single_use=True)
        r_nv = procedural_expr.compile_socket_expression(sock, want_type=wtype, inline_single_use=False)
        in_str = (
            f"{r_in.def_count:>11d} {r_in.def_slots:>6d} {r_in.max_expr_depth:>6d} {len(r_in.expression):>7d}"
            if r_in is not None
            else f"{'FAIL':>11} {'-':>6} {'-':>6} {'-':>7}"
        )
        nv_str = (
            f"{r_nv.def_count:>10d} {r_nv.def_slots:>6d} {'yes':>6}"
            if r_nv is not None
            else f"{'OVERFLOW':>10} {'>495':>6} {'no':>6}"
        )
        print(f"{label:<24} {n_nodes:>5d} | {in_str} | {nv_str}")


if __name__ == "__main__":
    main()
