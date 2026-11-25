"""Compile Blender Cycles procedural shader subgraphs into RISE expressions.

Targets RISE's `expression_painter` VM (`ExpressionEval.h` +
`ExpressionOslBuiltins.h`), which implements exact IEEE-754 single-precision
ports of Cycles' procedural kernels plus live object/world/camera space
transforms:
  * Group 1-2: Noise Texture, gradient/fractal noise, White Noise
  * Group 3:   Voronoi Texture (F1/F2/Smooth F1/Distance to Edge/N-Sphere Radius)
  * Group 4:   RGB/HSV/HSL colour conversions and Mix Color (19 blend modes)
  * Group 5:   Safe Math (41 ops), Vector Math (30 ops), Clamp, Map Range
  * Group 6:   Componentwise vec3 ops and Vector Rotate (Axis/X/Y/Z/Euler XYZ)
  * Group 7:   Live space transforms (World <-> Object <-> Camera)

Lowering strategy (design doc §5.2(a) & §8):
  * Pre-pass reference counting over the reachable DAG counts how many times
    each `(node, output_name)` is consumed.
  * Single-use expressions are inlined directly (zero `def` slots consumed)
    unless their AST nesting depth reaches `_INLINE_SPLIT_DEPTH` (24), which
    splits long chains into `def` bindings well below RISE's
    `ExpressionProgram::kMaxParseDepth` (200).
  * Multi-use outputs (diamond DAGs), multi-output shared nodes (Noise Texture
    Color+Fac sharing one `osl_noise_texture_color` call, SeparateXYZ,
    SeparateColor), and expressions whose operands appear multiple times in the
    emitted formula are bound once into a `def _vN` slot so they never duplicate
    work or blow up expression size.
  * Fail closed: if ANY node, output socket, or mode in the upstream subgraph
    is not faithfully supported (or exceeds the 495-slot `def` budget), return
    ``None`` rather than silently dropping part of the graph.
  * Pure Python with no top-level ``bpy`` import so unit tests can run both
    outside and inside Blender.
"""

from __future__ import annotations

from dataclasses import dataclass
import math

from .blender_compat import rna_key


@dataclass(frozen=True)
class CompiledExpression:
    """Compiled RISE expression_painter source plus lowering statistics."""

    expression: str
    uses_generated: bool
    def_count: int = 0
    def_slots: int = 0
    max_expr_depth: int = 0
    mesh_attributes: tuple[str, ...] = ()


# Cycles enum tables matching OslVoronoi.h, OslNoise.h, OslColor.h, OslMath.h, OslVector.h.
_DIMS_MAP = {
    "1D": 1,
    "2D": 2,
    "3D": 3,
    "4D": 4,
}

_VORONOI_FEATURE_MAP = {
    "F1": 0,
    "F2": 1,
    "SMOOTH_F1": 2,
    "DISTANCE_TO_EDGE": 3,
    "N_SPHERE_RADIUS": 4,
}

_VORONOI_METRIC_MAP = {
    "EUCLIDEAN": 0,
    "MANHATTAN": 1,
    "CHEBYCHEV": 2,
    "MINKOWSKI": 3,
}

_VORONOI_OUTPUT_FN = {
    "Distance": ("osl_voronoi_distance", "scalar"),
    "Color": ("osl_voronoi_color", "vec3"),
    "Position": ("osl_voronoi_position", "vec3"),
    "W": ("osl_voronoi_w", "scalar"),
    "Radius": ("osl_voronoi_radius", "scalar"),
}

_NOISE_TYPE_MAP = {
    "MULTIFRACTAL": 0,
    "FBM": 1,
    "HYBRID_MULTIFRACTAL": 2,
    "RIDGED_MULTIFRACTAL": 3,
    "HETERO_TERRAIN": 4,
}

_MIX_MODE_MAP = {
    "MIX": 0,
    "ADD": 1,
    "MULTIPLY": 2,
    "SUBTRACT": 3,
    "SCREEN": 4,
    "DIVIDE": 5,
    "DIFFERENCE": 6,
    "DARKEN": 7,
    "LIGHTEN": 8,
    "OVERLAY": 9,
    "DODGE": 10,
    "BURN": 11,
    "HUE": 12,
    "SATURATION": 13,
    "VALUE": 14,
    "COLOR": 15,
    "SOFT_LIGHT": 16,
    "LINEAR_LIGHT": 17,
    "EXCLUSION": 18,
}

# Blender ShaderNodeMath.operation -> OslMath::MathOp (0..40)
_MATH_OP_MAP = {
    "ADD": 0,
    "SUBTRACT": 1,
    "MULTIPLY": 2,
    "DIVIDE": 3,
    "SINE": 4,
    "COSINE": 5,
    "TANGENT": 6,
    "ARCSINE": 7,
    "ARCCOSINE": 8,
    "ARCTANGENT": 9,
    "POWER": 10,
    "LOGARITHM": 11,
    "MINIMUM": 12,
    "MAXIMUM": 13,
    "ROUND": 14,
    "LESS_THAN": 15,
    "GREATER_THAN": 16,
    "MODULO": 17,
    "ABSOLUTE": 18,
    "ARCTAN2": 19,
    "FLOOR": 20,
    "CEIL": 21,
    "FRACT": 22,
    "SQRT": 23,
    "INVERSE_SQRT": 24,
    "SIGN": 25,
    "EXPONENT": 26,
    "RADIANS": 27,
    "DEGREES": 28,
    "SINH": 29,
    "COSH": 30,
    "TANH": 31,
    "TRUNC": 32,
    "SNAP": 33,
    "WRAP": 34,
    "COMPARE": 35,
    "MULTIPLY_ADD": 36,
    "PINGPONG": 37,
    "SMOOTH_MIN": 38,
    "SMOOTH_MAX": 39,
    "FLOORED_MODULO": 40,
}

# Blender ShaderNodeVectorMath.operation -> OslMath::VectorMathOp (0..29)
_VECTOR_MATH_OP_MAP = {
    "ADD": 0,
    "SUBTRACT": 1,
    "MULTIPLY": 2,
    "DIVIDE": 3,
    "CROSS_PRODUCT": 4,
    "PROJECT": 5,
    "REFLECT": 6,
    "DOT_PRODUCT": 7,
    "DISTANCE": 8,
    "LENGTH": 9,
    "SCALE": 10,
    "NORMALIZE": 11,
    "SNAP": 12,
    "FLOOR": 13,
    "CEIL": 14,
    "MODULO": 15,
    "FRACTION": 16,
    "ABSOLUTE": 17,
    "MINIMUM": 18,
    "MAXIMUM": 19,
    "WRAP": 20,
    "SINE": 21,
    "COSINE": 22,
    "TANGENT": 23,
    "REFRACT": 24,
    "FACEFORWARD": 25,
    "MULTIPLY_ADD": 26,
    "POWER": 27,
    "SIGN": 28,
    "ROUND": 29,
}

_MATH_UNARY_OPS = frozenset({
    4, 5, 6, 7, 8, 9, 14, 18, 20, 21, 22, 23, 24, 25, 26, 27, 28, 29, 30, 31, 32,
})
_MATH_TERNARY_OPS = frozenset({34, 35, 36, 38, 39})

_VECTOR_MATH_SCALAR_OPS = frozenset({7, 8, 9})
_VECTOR_MATH_UNARY_OPS = frozenset({9, 11, 13, 14, 16, 17, 21, 22, 23, 28, 29})
_VECTOR_MATH_TERNARY_OPS = frozenset({20, 25, 26})

_CLAMP_TYPE_MAP = {
    "MINMAX": 0,
    "RANGE": 1,
}

_MAP_RANGE_TYPE_MAP = {
    "LINEAR": 0,
    "STEPPED": 1,
    "SMOOTHSTEP": 2,
    "SMOOTHERSTEP": 3,
}

_VECTOR_ROTATE_TYPE_MAP = {
    "AXIS_ANGLE": 0,
    "X_AXIS": 1,
    "Y_AXIS": 2,
    "Z_AXIS": 3,
    "EULER_XYZ": 4,
}

_GRADIENT_TYPE_MAP = {
    "LINEAR": 0,
    "QUADRATIC": 1,
    "EASING": 2,
    "DIAGONAL": 3,
    "RADIAL": 4,
    "QUADRATIC_SPHERE": 5,
    "SPHERICAL": 6,
}

# Synthetic named mesh attribute keys resolved by exporter._extract_named_mesh_attributes:
#   - ACTIVE_COLOR_ATTR: active/default mesh color attribute (ShaderNodeVertexColor with empty layer_name)
#   - ISLAND_RANDOM_ATTR: per-connected-component island hash (ShaderNodeNewGeometry.Random Per Island)
#   - UV_MAP_ATTR_PREFIX + <uv_name>: named mesh UV layer (ShaderNodeUVMap with non-empty uv_map)
ACTIVE_COLOR_ATTR = "__rise_active_color__"
ISLAND_RANDOM_ATTR = "__rise_island_random"
UV_MAP_ATTR_PREFIX = "__rise_uv_map__"

SUPPORTED_TEX_IMAGE_EXTENSIONS = frozenset({
    "REPEAT",
    "EXTEND",
    "CLIP",
    "MIRROR",
})

SUPPORTED_GEOMETRY_OUTPUTS = frozenset({
    "Backfacing",
    "Random Per Island",
    "Position",
    "Normal",
    "True Normal",
})

SUPPORTED_PARTICLE_INFO_OUTPUTS = frozenset({
    "Random",
})

SUPPORTED_LIGHT_PATH_OUTPUTS = frozenset({
    "Is Camera Ray",
})


def _fmt(value: float) -> str:
    """Format a finite float cleanly for RISE's expression lexer."""
    f = float(value)
    if not math.isfinite(f):
        f = 0.0
    if f == 0.0:
        return "0"
    return f"{f:.9g}"


def _fmt_vec3(xyz) -> str:
    return f"vec3({_fmt(xyz[0])}, {_fmt(xyz[1])}, {_fmt(xyz[2])})"


def _escape_expr_string(name: str) -> str:
    """Escape a UTF-8 attribute name for RISE ExpressionEval's string literal syntax
    (supported escapes: \\", \\\\, \\t, \\xHH; empty and NUL are forbidden)."""
    try:
        raw = name.encode("utf-8")
    except UnicodeEncodeError as exc:
        raise _UnsupportedGraph("invalid UTF-8 attribute name") from exc
    if not raw or b"\x00" in raw:
        raise _UnsupportedGraph("empty or NUL-containing attribute name")
    out: list[str] = []
    for b in raw:
        if b == ord("\\"):
            out.append("\\\\")
        elif b == ord('"'):
            out.append('\\"')
        elif b == ord("\t"):
            out.append("\\t")
        elif b < 0x20 or b == 0x7F or b >= 0x80 or b in (ord("{"), ord("}"), ord("#")):
            out.append(f"\\x{b:02x}")
        else:
            out.append(chr(b))
    return "".join(out)


def _clamp_unit_default(value) -> float:
    """Clamp an unlinked MixShader factor default to [0, 1], mapping NaN -> 0.0
    (matching Cycles `saturatef(NaN) == 0.0f` instead of Python's `min(1.0, nan) == 1.0`)."""
    f = float(value)
    if math.isnan(f):
        return 0.0
    if f <= 0.0:
        return 0.0
    if f >= 1.0:
        return 1.0
    return f


def _rgb_to_hsv_py(r: float, g: float, b: float) -> tuple[float, float, float]:
    """Match Cycles util/color.h rgb_to_hsv for compile-time ColorRamp stops."""
    cmax = max(r, g, b)
    cmin = min(r, g, b)
    cdelta = cmax - cmin
    v = cmax
    if cmax != 0.0:
        s = cdelta / cmax
    else:
        return (0.0, 0.0, 0.0)
    if s == 0.0:
        return (0.0, 0.0, v)
    if r == cmax:
        h = (g - b) / cdelta
    elif g == cmax:
        h = 2.0 + (b - r) / cdelta
    else:
        h = 4.0 + (r - g) / cdelta
    h /= 6.0
    if h < 0.0:
        h += 1.0
    return (h, s, v)


def _rgb_to_hsl_py(r: float, g: float, b: float) -> tuple[float, float, float]:
    """Match Cycles util/color.h rgb_to_hsl for compile-time ColorRamp stops."""
    cmax = max(r, g, b)
    cmin = min(r, g, b)
    l = 0.5 * (cmax + cmin)
    if cmax == cmin:
        return (0.0, 0.0, l)
    d = cmax - cmin
    denom = (2.0 - cmax - cmin) if l > 0.5 else (cmax + cmin)
    s = (d / denom) if denom != 0.0 else 0.0
    if cmax == r:
        h = (g - b) / d + (6.0 if g < b else 0.0)
    elif cmax == g:
        h = (b - r) / d + 2.0
    else:
        h = (r - g) / d + 4.0
    h /= 6.0
    return (h, s, l)


def _colorband_hue_interp_mode(hue_mode: str, h1: float, h2: float) -> int:
    """Return Blender colorband_hue_interp mode: 0 direct, 1 wrap h1+1, 2 wrap h2+1."""
    if hue_mode == "NEAR":
        if (h1 < h2) and (h2 - h1) > 0.5:
            return 1
        if (h1 > h2) and (h2 - h1) < -0.5:
            return 2
        return 0
    if hue_mode == "FAR":
        if h1 == h2:
            return 1
        if (h1 < h2) and (h2 - h1) < 0.5:
            return 1
        if (h1 > h2) and (h2 - h1) > -0.5:
            return 2
        return 0
    if hue_mode == "CCW":
        return 2 if h1 > h2 else 0
    if hue_mode == "CW":
        return 1 if h1 < h2 else 0
    raise _UnsupportedGraph(f"ColorRamp hue_interpolation {hue_mode}")


def _live_links(socket):
    if socket is None or not getattr(socket, "is_linked", False):
        return []
    return [link for link in socket.links if not getattr(link, "is_muted", False)]


def _resolve_reroute(node, socket):
    """Unwrap NodeReroute chains iteratively, preserving the upstream socket."""
    seen: set[int] = set()
    while node is not None and getattr(node, "bl_idname", "") == "NodeReroute":
        rid = rna_key(node)
        if rid in seen:
            raise _UnsupportedGraph("cycle detected in reroute")
        seen.add(rid)
        inp = node.inputs[0] if getattr(node, "inputs", None) else None
        links = _live_links(inp)
        if not links:
            raise _UnsupportedGraph("unlinked reroute")
        node = links[0].from_node
        socket = links[0].from_socket
    return node, socket


def _active_node_inputs(node, out_name: str | None = None) -> list:
    """Return only the sockets that the compiler will evaluate for ``node``."""
    bl = getattr(node, "bl_idname", "")
    all_inputs = list(getattr(node, "inputs", ()))
    if bl == "ShaderNodeTexBrick":
        if out_name == "Fac":
            return [inp for inp in all_inputs if inp.name not in ("Color1", "Color2", "Mortar")]
        return all_inputs
    if bl == "ShaderNodeAmbientOcclusion":
        socks = [_input_by_name(node, "Distance"), _input_by_name(node, "Normal")]
        if out_name != "AO":
            socks.append(_input_by_name(node, "Color"))
        return [s for s in socks if s is not None]
    if bl == "ShaderNodeAttribute":
        return []
    if bl == "ShaderNodeTexVoronoi":
        if getattr(node, "voronoi_dimensions", "3D") == "1D":
            return [inp for inp in all_inputs if inp.name != "Vector"]
        return all_inputs
    if bl in ("ShaderNodeTexNoise", "ShaderNodeTexWhiteNoise"):
        if getattr(node, "noise_dimensions", "3D") == "1D":
            return [inp for inp in all_inputs if inp.name != "Vector"]
        return all_inputs
    if bl == "ShaderNodeMix":
        data_type = getattr(node, "data_type", "FLOAT")
        if data_type == "RGBA":
            socks = [
                _input_by_name(node, "Factor", "VALUE"),
                _input_by_name(node, "A", "RGBA"),
                _input_by_name(node, "B", "RGBA"),
            ]
            return [s for s in socks if s is not None]
        if data_type == "FLOAT":
            socks = [
                _input_by_name(node, "Factor", "VALUE"),
                _input_by_name(node, "A", "VALUE"),
                _input_by_name(node, "B", "VALUE"),
            ]
            return [s for s in socks if s is not None]
        if data_type == "VECTOR":
            factor_mode = getattr(node, "factor_mode", "UNIFORM")
            fac_sock = (
                _input_by_name(node, "Factor", "VALUE")
                if factor_mode == "UNIFORM"
                else (_input_by_name(node, "Factor", "VECTOR") or _input_by_name(node, "Factor"))
            )
            socks = [
                fac_sock,
                _input_by_name(node, "A", "VECTOR"),
                _input_by_name(node, "B", "VECTOR"),
            ]
            return [s for s in socks if s is not None]
        return all_inputs
    if bl == "ShaderNodeMapRange":
        data_type = getattr(node, "data_type", "FLOAT")
        is_stepped = (getattr(node, "interpolation_type", "LINEAR") == "STEPPED")
        if data_type == "FLOAT":
            names = ("Value", "From Min", "From Max", "To Min", "To Max") + (("Steps",) if is_stepped else ())
            socks = [_input_by_name(node, n, "VALUE") or _input_by_name(node, n) for n in names]
            return [s for s in socks if s is not None]
        if data_type == "FLOAT_VECTOR":
            names = ("Vector", "From Min", "From Max", "To Min", "To Max") + (("Steps",) if is_stepped else ())
            socks = [_input_by_name(node, n, "VECTOR") or _input_by_name(node, n) for n in names]
            return [s for s in socks if s is not None]
        return all_inputs
    if bl == "ShaderNodeVectorRotate":
        rtype = getattr(node, "rotation_type", "AXIS_ANGLE")
        if rtype == "EULER_XYZ":
            names = ("Vector", "Center", "Rotation")
        elif rtype in ("X_AXIS", "Y_AXIS", "Z_AXIS"):
            names = ("Vector", "Center", "Angle")
        else:
            names = ("Vector", "Center", "Axis", "Angle")
        socks = [_input_by_name(node, n) for n in names]
        return [s for s in socks if s is not None]
    if bl == "ShaderNodeMath":
        op = _MATH_OP_MAP.get(getattr(node, "operation", "ADD"))
        if op in _MATH_UNARY_OPS:
            return all_inputs[:1]
        if op in _MATH_TERNARY_OPS:
            return all_inputs[:3]
        return all_inputs[:2]
    if bl == "ShaderNodeVectorMath":
        op = _VECTOR_MATH_OP_MAP.get(getattr(node, "operation", "ADD"))
        vec_inps = _inputs_of_type(node, "VECTOR")
        if not vec_inps:
            vec_inps = [inp for inp in all_inputs if inp.name not in ("Scale", "IOR")]
        scale_sock = _input_by_names(node, ("Scale", "IOR"))
        if op in _VECTOR_MATH_UNARY_OPS:
            return vec_inps[:1]
        if op == 10:  # SCALE
            return [s for s in (vec_inps[:1] + [scale_sock]) if s is not None]
        if op == 24:  # REFRACT
            return [s for s in (vec_inps[:2] + [scale_sock]) if s is not None]
        if op in _VECTOR_MATH_TERNARY_OPS:
            return vec_inps[:3]
        return vec_inps[:2]
    return all_inputs


def _canonical_output_name(node, out_sock_or_name) -> str:
    """Normalize output socket key:
      - For muted nodes, ``ShaderNodeGroup``, and ``NodeGroupInput``, prefer
        ``socket.identifier`` over ``socket.name`` so multi-typed outputs
        (e.g. ``ShaderNodeMix``'s three ``Result`` sockets) and duplicate-named
        group sockets never collide or coerce RGB -> scalar.
      - Normalize Blender 5.3 display-name renames (``Factor`` vs ``Fac``)."""
    if isinstance(out_sock_or_name, str):
        name = out_sock_or_name
        ident = ""
    else:
        name = str(getattr(out_sock_or_name, "name", "") or "")
        ident = str(getattr(out_sock_or_name, "identifier", "") or "")
    bl = getattr(node, "bl_idname", "")
    if getattr(node, "mute", False) or bl in ("ShaderNodeGroup", "NodeGroupInput"):
        return ident or name
    if bl in ("ShaderNodeTexNoise", "ShaderNodeAttribute", "ShaderNodeTexGradient", "ShaderNodeTexBrick"):
        if name == "Factor" or ident == "Fac":
            return "Fac"
    return name


def canonical_output_name(node, out_sock_or_name) -> str:
    """Public helper mirroring ``_canonical_output_name`` for ``material_bake`` / ``exporter``."""
    return _canonical_output_name(node, out_sock_or_name)


def match_socket_by_key(sockets, key: str):
    """Match a socket by exact ``.identifier`` first, falling back to ``.name``."""
    if not key:
        return None
    for s in sockets:
        if getattr(s, "identifier", "") == key:
            return s
    for s in sockets:
        if getattr(s, "name", "") == key:
            return s
    return None


def resolve_muted_input_socket(node, output_key_or_sock):
    """For a muted Blender node, return the input socket routed to ``output_key_or_sock``
    via ``node.internal_links``, preferring exact ``.identifier`` match before ``.name``."""
    if isinstance(output_key_or_sock, str):
        want_ident = output_key_or_sock
        want_name = output_key_or_sock
    else:
        want_ident = str(getattr(output_key_or_sock, "identifier", "") or "")
        want_name = str(getattr(output_key_or_sock, "name", "") or "")
    internal_links = getattr(node, "internal_links", ())
    if want_ident:
        for il in internal_links:
            to_sock = getattr(il, "to_socket", None)
            from_sock = getattr(il, "from_socket", None)
            if to_sock is not None and from_sock is not None and getattr(to_sock, "identifier", "") == want_ident:
                return from_sock
    for il in internal_links:
        to_sock = getattr(il, "to_socket", None)
        from_sock = getattr(il, "from_socket", None)
        if to_sock is None or from_sock is None:
            continue
        to_ident = str(getattr(to_sock, "identifier", "") or "")
        if not isinstance(output_key_or_sock, str) and want_ident and to_ident and to_ident != want_ident:
            continue
        if (
            (want_name and getattr(to_sock, "name", "") == want_name)
            or (want_ident and _canonical_output_name(node, to_sock) == want_ident)
            or (want_name and _canonical_output_name(node, to_sock) == want_name)
        ):
            return from_sock
    return None


def find_group_output_node(node_tree):
    """Return the active ``NodeGroupOutput`` inside a ``ShaderNodeGroup.node_tree``."""
    if node_tree is None:
        return None
    fallback = None
    for n in getattr(node_tree, "nodes", ()):
        if getattr(n, "bl_idname", "") == "NodeGroupOutput":
            if getattr(n, "is_active_output", False):
                return n
            if fallback is None:
                fallback = n
    return fallback


def _input_by_name(node, name: str, socket_type: str | None = None):
    for inp in getattr(node, "inputs", ()):
        if inp.name != name and getattr(inp, "identifier", None) != name:
            continue
        if socket_type is not None and getattr(inp, "type", None) != socket_type:
            continue
        return inp
    return None


def _input_by_names(node, names: tuple[str, ...], socket_type: str | None = None):
    for name in names:
        sock = _input_by_name(node, name, socket_type)
        if sock is not None:
            return sock
    return None


def _inputs_of_type(node, socket_type: str) -> list:
    return [inp for inp in getattr(node, "inputs", ()) if getattr(inp, "type", None) == socket_type]


def _default_scalar(socket, fallback: float = 0.0) -> float:
    if socket is None:
        return fallback
    try:
        val = socket.default_value
        if hasattr(val, "__len__"):
            return float(val[0]) if len(val) > 0 else fallback
        return float(val)
    except (AttributeError, TypeError, ValueError):
        return fallback


def _default_vec3(socket, fallback=(0.0, 0.0, 0.0)) -> tuple[float, float, float]:
    if socket is None:
        return (float(fallback[0]), float(fallback[1]), float(fallback[2]))
    try:
        val = socket.default_value
        if hasattr(val, "__len__"):
            n = len(val)
            if n >= 3:
                return (float(val[0]), float(val[1]), float(val[2]))
            if n == 2:
                return (float(val[0]), float(val[1]), 0.0)
            if n == 1:
                c = float(val[0])
                return (c, c, c)
        c = float(val)
        return (c, c, c)
    except (AttributeError, TypeError, ValueError):
        return (float(fallback[0]), float(fallback[1]), float(fallback[2]))


def _euler_xyz_matrix(rx: float, ry: float, rz: float):
    """3x3 XYZ Euler rotation matrix matching Blender's Euler((rx, ry, rz), 'XYZ')."""
    cx, sx = math.cos(rx), math.sin(rx)
    cy, sy = math.cos(ry), math.sin(ry)
    cz, sz = math.cos(rz), math.sin(rz)
    # R = Rz * Ry * Rx
    return (
        (cy * cz, sx * sy * cz - cx * sz, cx * sy * cz + sx * sz),
        (cy * sz, sx * sy * sz + cx * cz, cx * sy * sz - sx * cz),
        (-sy, sx * cy, cx * cy),
    )


def _mat3_transpose(m):
    return (
        (m[0][0], m[1][0], m[2][0]),
        (m[0][1], m[1][1], m[2][1]),
        (m[0][2], m[1][2], m[2][2]),
    )


def _mat3_mul_diag(m, sx: float, sy: float, sz: float):
    """Compute M * diag(sx, sy, sz)."""
    return (
        (m[0][0] * sx, m[0][1] * sy, m[0][2] * sz),
        (m[1][0] * sx, m[1][1] * sy, m[1][2] * sz),
        (m[2][0] * sx, m[2][1] * sy, m[2][2] * sz),
    )


def _diag_mul_mat3(sx: float, sy: float, sz: float, m):
    """Compute diag(sx, sy, sz) * M."""
    return (
        (sx * m[0][0], sx * m[0][1], sx * m[0][2]),
        (sy * m[1][0], sy * m[1][1], sy * m[1][2]),
        (sz * m[2][0], sz * m[2][1], sz * m[2][2]),
    )


def _emit_affine(v_var: str, mat, trans=(0.0, 0.0, 0.0)) -> str:
    """Emit `vec3(m00*v.x + m01*v.y + m02*v.z + tx, ...)` cleanly."""
    parts = []
    for row in range(3):
        terms = []
        for col, comp in enumerate(("x", "y", "z")):
            c = mat[row][col]
            if abs(c) > 1e-12:
                if abs(c - 1.0) <= 1e-12:
                    terms.append(f"{v_var}.{comp}")
                elif abs(c + 1.0) <= 1e-12:
                    terms.append(f"(-{v_var}.{comp})")
                else:
                    terms.append(f"({_fmt(c)} * {v_var}.{comp})")
        t = trans[row]
        if abs(t) > 1e-12:
            terms.append(_fmt(t))
        parts.append(" + ".join(terms) if terms else "0")
    return f"vec3({parts[0]}, {parts[1]}, {parts[2]})"


def _is_atomic_expr(expr: str) -> bool:
    """True when `expr` is a simple variable, swizzle, or numeric literal."""
    s = expr.strip()
    if not s:
        return True
    if s in ("u", "v", "P", "Po", "N"):
        return True
    if s.startswith("_v") and all(c.isalnum() or c in "_." for c in s):
        return True
    try:
        float(s)
        return True
    except ValueError:
        return False


def _is_valid_rise_ident(name: str) -> bool:
    if not name:
        return False
    c0 = name[0]
    if not (("a" <= c0 <= "z") or ("A" <= c0 <= "Z") or c0 == "_"):
        return False
    return all(("a" <= c <= "z") or ("A" <= c <= "Z") or ("0" <= c <= "9") or c == "_" for c in name)


def _eval_curve_points_fallback(curve, x: float) -> float:
    """Piecewise-linear fallback for CurveMap evaluation when Blender's
    `CurveMapping.evaluate(curve, x)` is unavailable (pure-Python unit tests)."""
    pts = list(getattr(curve, "points", ()))
    if not pts:
        return x
    coords = sorted((float(p.location[0]), float(p.location[1])) for p in pts)
    if len(coords) == 1:
        return coords[0][1]
    if x <= coords[0][0]:
        return coords[0][1]
    if x >= coords[-1][0]:
        return coords[-1][1]
    for i in range(len(coords) - 1):
        x0, y0 = coords[i]
        x1, y1 = coords[i + 1]
        if x <= x1:
            dx = x1 - x0
            if dx <= 1e-12:
                return y0
            t = (x - x0) / dx
            return y0 + (y1 - y0) * t
    return coords[-1][1]


def _curvemapping_minmax(mapping, num_curves: int) -> tuple[float, float]:
    """Match Cycles `curvemapping_minmax` (`intern/cycles/blender/util.h:152-173`):
    always scan the first/last control-point X across curves (never short-circuit
    on `use_clip` / `clip_min_x` / `clip_max_x`)."""
    curves = list(getattr(mapping, "curves", ()))
    mn = float("inf")
    mx = float("-inf")
    for c in curves[:num_curves]:
        pts = list(getattr(c, "points", ()))
        if not pts:
            continue
        first_x = float(pts[0].location[0])
        last_x = float(pts[-1].location[0])
        if first_x < mn:
            mn = first_x
        if last_x > mx:
            mx = last_x
    if not math.isfinite(mn) or not math.isfinite(mx) or mx <= mn + 1e-9:
        return (0.0, 1.0)
    return (mn, mx)


def _is_primary_uv_vector_socket(vec_sock) -> bool:
    """True when a TexImage `Vector` socket reads the mesh's unmodified primary
    UV `(u, v)` directly (unlinked, or linked through reroutes to `TexCoord.UV`
    or an unnamed `ShaderNodeUVMap` without `from_instancer`)."""
    links = _live_links(vec_sock)
    if not links:
        return True
    try:
        src_node, src_sock = _resolve_reroute(links[0].from_node, links[0].from_socket)
    except _UnsupportedGraph:
        return False
    if src_node is None or src_sock is None or getattr(src_node, "mute", False):
        return False
    bl = getattr(src_node, "bl_idname", "")
    if bl == "ShaderNodeTexCoord":
        return (not getattr(src_node, "from_instancer", False)) and getattr(src_sock, "name", "") == "UV"
    if bl == "ShaderNodeUVMap":
        return (not getattr(src_node, "from_instancer", False)) and not str(getattr(src_node, "uv_map", "") or "")
    return False


class _UnsupportedGraph(Exception):
    """Internal signal to abort compilation cleanly on an unsupported node."""


class _Compiler:
    # ExpressionProgram::kMaxSlots (512) minus kContextSlotCount (16) and
    # the auto-registered `seed` parameter (1) in Job::AddExpressionPainter.
    _MAX_DEF_SLOTS = 512 - 17
    # Split inline chains well below ExpressionProgram::kMaxParseDepth (200)
    # so even 250-node chains parse cleanly with bounded recursion depth.
    _INLINE_SPLIT_DEPTH = 24

    def __init__(
        self,
        bbox_min,
        bbox_max,
        *,
        max_depth: int = 512,
        inline_single_use: bool = True,
        inline_split_depth: int = _INLINE_SPLIT_DEPTH,
        material_pass_index: float = 0.0,
        attr_types: dict[str, str] | None = None,
        image_resolver=None,
    ):
        self._bbox_min = bbox_min
        self._bbox_max = bbox_max
        self._max_depth = max_depth
        self._inline_single_use = inline_single_use
        self._inline_split_depth = inline_split_depth
        self._material_pass_index = float(material_pass_index)
        self._attr_types: dict[str, str] = dict(attr_types) if attr_types else {}
        self._image_resolver = image_resolver
        self._defs: list[tuple[str, str]] = []
        self._def_slots = 0
        # Enclosing ShaderNodeGroup call stack so identical inner nodes inside
        # two distinct ShaderNodeGroup instances never alias in _cache.
        self._group_stack: list[object] = []
        # Maps (scoped_node_key, output_name) -> (expr, vtype, expr_depth)
        self._cache: dict[tuple, tuple[str, str, int]] = {}
        self._active: set[tuple] = set()
        self._use_counts: dict[tuple, int] = {}
        self._node_outputs_used: dict[tuple, set[str]] = {}
        self._mesh_attributes: set[str] = set()
        self.uses_generated = False
        self.max_expr_depth = 0

    def _scoped_nid(self, node, group_stack=None) -> tuple:
        gs = self._group_stack if group_stack is None else group_stack
        return (tuple(rna_key(g) for g in gs), rna_key(node))

    def prepass_count_uses(self, root_links: list) -> list[tuple[tuple, object, str]]:
        """Iterative post-order DAG walk counting uses and returning topological order.

        Evaluating nodes in the returned bottom-up topological order guarantees
        every upstream input is already cached, keeping Python recursion depth
        O(1) even on 500+ node chains.  Tracks enclosing ``ShaderNodeGroup``
        frames and follows ``internal_links`` through muted passthrough nodes.
        """
        state: dict[tuple, int] = {}  # 1 = visiting, 2 = done
        topo: list[tuple[tuple, object, str]] = []
        # Stack items: (group_stack_tuple, node, out_sock, stage)
        stack: list[tuple[tuple, object, object, int]] = []
        for link in root_links:
            if link is not None and getattr(link, "from_node", None) is not None:
                stack.append(((), link.from_node, link.from_socket, 0))

        steps = 0
        max_steps = 200_000
        while stack:
            steps += 1
            if steps > max_steps:
                raise _UnsupportedGraph("graph too large during use-count prepass")
            gstack, node, out_sock, stage = stack.pop()
            if node is None:
                continue
            node, out_sock = _resolve_reroute(node, out_sock)
            if node is None or out_sock is None:
                continue
            out_name = _canonical_output_name(node, out_sock)

            snid = self._scoped_nid(node, gstack)
            key = (snid, out_name)
            if stage == 1:
                state[key] = 2
                topo.append((gstack, node, out_name))
                continue

            self._use_counts[key] = self._use_counts.get(key, 0) + 1
            self._node_outputs_used.setdefault(snid, set()).add(out_name)

            st = state.get(key, 0)
            if st == 1:
                raise _UnsupportedGraph("cycle detected")
            if st == 2:
                continue
            state[key] = 1
            stack.append((gstack, node, out_sock, 1))

            if getattr(node, "mute", False):
                pass_inp = resolve_muted_input_socket(node, out_name)
                if pass_inp is not None:
                    links = _live_links(pass_inp)
                    if links and links[0].from_node is not None:
                        stack.append((gstack, links[0].from_node, links[0].from_socket, 0))
                continue

            bl = getattr(node, "bl_idname", "")
            if bl == "ShaderNodeGroup":
                nt = getattr(node, "node_tree", None)
                if nt is None or len(gstack) >= 8:
                    continue
                if any(rna_key(getattr(g, "node_tree", None)) == rna_key(nt) for g in gstack):
                    raise _UnsupportedGraph("recursive node group cycle")
                gout = find_group_output_node(nt)
                if gout is not None:
                    g_inp = match_socket_by_key(getattr(gout, "inputs", ()), out_name)
                    if g_inp is not None:
                        links = _live_links(g_inp)
                        if links and links[0].from_node is not None:
                            stack.append((gstack + (node,), links[0].from_node, links[0].from_socket, 0))
                continue

            if bl == "NodeGroupInput":
                if gstack:
                    enclosing = gstack[-1]
                    outer_inp = match_socket_by_key(getattr(enclosing, "inputs", ()), out_name)
                    if outer_inp is not None:
                        links = _live_links(outer_inp)
                        if links and links[0].from_node is not None:
                            stack.append((gstack[:-1], links[0].from_node, links[0].from_socket, 0))
                continue

            for inp in _active_node_inputs(node, out_name):
                links = _live_links(inp)
                if links and links[0].from_node is not None:
                    stack.append((gstack, links[0].from_node, links[0].from_socket, 0))

        return topo

    def evaluate_topo(self, topo: list[tuple[tuple, object, str]]) -> None:
        saved = self._group_stack
        try:
            for gstack, node, out_name in topo:
                self._group_stack = list(gstack)
                self.eval_node_output(node, out_name, 0)
        finally:
            self._group_stack = saved

    def _record_depth(self, expr_depth: int) -> int:
        if expr_depth > self.max_expr_depth:
            self.max_expr_depth = expr_depth
        return expr_depth

    def _bind(self, expr: str, vtype: str) -> tuple[str, str, int]:
        if _is_atomic_expr(expr):
            return (expr, vtype, 0)
        slots = 3 if vtype == "vec3" else 1
        if self._def_slots + slots > self._MAX_DEF_SLOTS:
            raise _UnsupportedGraph("expression slot budget exceeded")
        name = f"_v{len(self._defs)}"
        self._defs.append((name, expr))
        self._def_slots += slots
        return (name, vtype, 0)

    def _emit_result(
        self,
        key: tuple[int, str] | None,
        expr: str,
        vtype: str,
        expr_depth: int,
        *,
        force_bind: bool = False,
    ) -> tuple[str, str, int]:
        """Inline single-use shallow expressions; bind multi-use or deep ones."""
        self._record_depth(expr_depth)
        if _is_atomic_expr(expr):
            return (expr, vtype, 0)
        uses = self._use_counts.get(key, 1) if key is not None else 1
        need_bind = (
            force_bind
            or not self._inline_single_use
            or uses >= 2
            or expr_depth >= self._inline_split_depth
        )
        if need_bind:
            return self._bind(expr, vtype)
        return (expr, vtype, expr_depth)

    def _bind_if_needed_for_multi_ref(self, val: tuple[str, str, int]) -> str:
        """Bind a sub-expression that will be substituted >= 2 times in one formula."""
        expr, vtype, _ = val
        if _is_atomic_expr(expr):
            return expr
        bound, _, _ = self._bind(expr, vtype)
        return bound

    def _generated_coord(self) -> tuple[str, str, int]:
        self.uses_generated = True
        if self._bbox_min is not None and self._bbox_max is not None:
            bmin = self._bbox_min
            bmax = self._bbox_max
            inv = [
                (1.0 / (bmax[i] - bmin[i])) if (bmax[i] - bmin[i]) > 1e-12 else 0.0
                for i in range(3)
            ]
            if all(abs(bmin[i]) <= 1e-12 and abs(inv[i] - 1.0) <= 1e-12 for i in range(3)):
                return ("Po", "vec3", 0)
            shared_key = (-1, "__generated")
            if shared_key in self._cache:
                return self._cache[shared_key]
            res = self._bind(f"((Po - {_fmt_vec3(bmin)}) * {_fmt_vec3(inv)})", "vec3")
            self._cache[shared_key] = res
            return res
        return ("Po", "vec3", 0)

    def _coerce(
        self,
        val: tuple[str, str, int],
        want_type: str,
        source_socket_type: str = "",
    ) -> tuple[str, int]:
        expr, have_type, depth = val
        if have_type == want_type:
            return (expr, depth)
        if want_type == "vec3" and have_type == "scalar":
            # Scalar is repeated 3 times in vec3(s, s, s): bind if non-atomic!
            s = self._bind_if_needed_for_multi_ref(val)
            d = 1 if s != expr else (depth + 1)
            return (f"vec3({s}, {s}, {s})", self._record_depth(d))
        if want_type == "scalar" and have_type == "vec3":
            # Cycles converts Color -> Float by Rec.709 luminance, and
            # Vector -> Float by channel average.
            if source_socket_type == "VECTOR":
                v = self._bind_if_needed_for_multi_ref(val)
                d = 2 if v != expr else (depth + 2)
                return (
                    f"(({v}.x + {v}.y + {v}.z) * 0.3333333333333333)",
                    self._record_depth(d),
                )
            return (
                f"dot({expr}, vec3(0.2126, 0.7152, 0.0722))",
                self._record_depth(depth + 1),
            )
        raise _UnsupportedGraph(f"cannot coerce {have_type} -> {want_type}")

    def eval_input(
        self,
        socket,
        want_type: str,
        depth: int,
        default_override=None,
        missing_default=None,
    ) -> tuple[str, int]:
        if socket is None:
            if default_override is not None:
                return (default_override, 0)
            if missing_default is not None:
                if want_type == "scalar":
                    return (_fmt(float(missing_default)), 0)
                if want_type == "vec3":
                    return (_fmt_vec3(missing_default), 0)
            raise _UnsupportedGraph("missing required input socket")
        links = _live_links(socket)
        if links:
            link = links[0]
            from_node, from_socket = _resolve_reroute(link.from_node, link.from_socket)
            if from_node is None or from_socket is None:
                raise _UnsupportedGraph("unlinked reroute")
            val = self.eval_node_output(from_node, _canonical_output_name(from_node, from_socket), depth + 1)
            return self._coerce(val, want_type, getattr(from_socket, "type", ""))
        if default_override is not None:
            return (default_override, 0)
        if want_type == "scalar":
            return (_fmt(_default_scalar(socket, 0.0)), 0)
        if want_type == "vec3":
            return (_fmt_vec3(_default_vec3(socket, (0.0, 0.0, 0.0))), 0)
        raise _UnsupportedGraph(f"unknown want_type {want_type}")

    def eval_node_output(self, node, output_name: str, depth: int) -> tuple[str, str, int]:
        if node is None or depth > self._max_depth:
            raise _UnsupportedGraph("null node or max depth exceeded")

        key = (self._scoped_nid(node), output_name)
        if key in self._cache:
            return self._cache[key]
        if key in self._active:
            raise _UnsupportedGraph("cycle detected")
        self._active.add(key)
        try:
            result = self._compile_node_output(node, output_name, depth, key)
        finally:
            self._active.discard(key)
        self._cache[key] = result
        return result

    def _compile_node_output(
        self, node, output_name: str, depth: int, key: tuple
    ) -> tuple[str, str, int]:
        bl = getattr(node, "bl_idname", "")

        if bl == "NodeReroute":
            inp = node.inputs[0] if node.inputs else None
            links = _live_links(inp)
            if not links:
                raise _UnsupportedGraph("unlinked reroute")
            src_node, src_sock = _resolve_reroute(links[0].from_node, links[0].from_socket)
            if src_node is None or src_sock is None:
                raise _UnsupportedGraph("unlinked reroute")
            return self.eval_node_output(src_node, _canonical_output_name(src_node, src_sock), depth + 1)

        if getattr(node, "mute", False):
            pass_inp = resolve_muted_input_socket(node, output_name)
            if pass_inp is None:
                raise _UnsupportedGraph(f"muted node {bl} without internal passthrough")
            out_sock = match_socket_by_key(getattr(node, "outputs", ()), output_name)
            if out_sock is None:
                for o in getattr(node, "outputs", ()):
                    if _canonical_output_name(node, o) == output_name:
                        out_sock = o
                        break
            want_vec = getattr(out_sock, "type", "") in ("RGBA", "VECTOR")
            if not want_vec and out_sock is None:
                want_vec = getattr(pass_inp, "type", "") in ("RGBA", "VECTOR")
            vtype = "vec3" if want_vec else "scalar"
            expr, d = self.eval_input(pass_inp, vtype, depth)
            return self._emit_result(key, expr, vtype, d)

        if bl == "ShaderNodeGroup":
            nt = getattr(node, "node_tree", None)
            if nt is None:
                raise _UnsupportedGraph("empty ShaderNodeGroup")
            if len(self._group_stack) >= 8:
                raise _UnsupportedGraph("node group nesting too deep")
            if any(rna_key(getattr(g, "node_tree", None)) == rna_key(nt) for g in self._group_stack):
                raise _UnsupportedGraph("recursive node group cycle")
            gout = find_group_output_node(nt)
            if gout is None:
                raise _UnsupportedGraph("ShaderNodeGroup missing NodeGroupOutput")
            g_inp = match_socket_by_key(getattr(gout, "inputs", ()), output_name)
            if g_inp is None:
                raise _UnsupportedGraph(f"ShaderNodeGroup output {output_name} not found on GroupOutput")
            vtype = "vec3" if getattr(g_inp, "type", "") in ("RGBA", "VECTOR") else "scalar"
            self._group_stack.append(node)
            try:
                expr, d = self.eval_input(g_inp, vtype, depth)
            finally:
                self._group_stack.pop()
            return self._emit_result(key, expr, vtype, d)

        if bl == "NodeGroupInput":
            if not self._group_stack:
                raise _UnsupportedGraph("NodeGroupInput outside ShaderNodeGroup")
            enclosing = self._group_stack.pop()
            try:
                outer_inp = match_socket_by_key(getattr(enclosing, "inputs", ()), output_name)
                if outer_inp is None:
                    raise _UnsupportedGraph(f"NodeGroupInput {output_name} missing on enclosing group")
                vtype = "vec3" if getattr(outer_inp, "type", "") in ("RGBA", "VECTOR") else "scalar"
                expr, d = self.eval_input(outer_inp, vtype, depth)
            finally:
                self._group_stack.append(enclosing)
            return self._emit_result(key, expr, vtype, d)

        if bl == "ShaderNodeValue":
            out = node.outputs[0] if node.outputs else None
            return (_fmt(_default_scalar(out, 0.0)), "scalar", 0)

        if bl == "ShaderNodeRGB":
            out = node.outputs[0] if node.outputs else None
            return (_fmt_vec3(_default_vec3(out, (0.5, 0.5, 0.5))), "vec3", 0)

        if bl == "ShaderNodeLightPath":
            if output_name == "Is Camera Ray":
                return self._emit_result(key, "osl_is_camera_ray()", "scalar", 1)
            raise _UnsupportedGraph(f"unsupported LightPath output {output_name}")

        if bl == "ShaderNodeLayerWeight":
            if output_name not in ("Fresnel", "Facing"):
                raise _UnsupportedGraph(f"unsupported LayerWeight output {output_name}")
            blend, bd = self.eval_input(_input_by_name(node, "Blend"), "scalar", depth, missing_default=0.5)
            normal_sock = _input_by_name(node, "Normal")
            normal_linked = bool(_live_links(normal_sock))
            if normal_linked:
                n_expr, nd = self.eval_input(normal_sock, "vec3", depth)
            else:
                n_expr, nd = "vec3(0, 0, 0)", 0
            flags = (1 if output_name == "Facing" else 0) | (2 if normal_linked else 0)
            return self._emit_result(
                key,
                f"osl_layer_weight({blend}, {n_expr}, {flags})",
                "scalar",
                max(bd, nd) + 1,
            )

        if bl == "ShaderNodeFresnel":
            if output_name != "Fac":
                raise _UnsupportedGraph(f"unsupported Fresnel output {output_name}")
            ior, id_ = self.eval_input(_input_by_name(node, "IOR"), "scalar", depth, missing_default=1.45)
            normal_sock = _input_by_name(node, "Normal")
            normal_linked = bool(_live_links(normal_sock))
            if normal_linked:
                n_expr, nd = self.eval_input(normal_sock, "vec3", depth)
            else:
                n_expr, nd = "vec3(0, 0, 0)", 0
            return self._emit_result(
                key,
                f"osl_fresnel({ior}, {n_expr}, {1 if normal_linked else 0})",
                "scalar",
                max(id_, nd) + 1,
            )

        if bl == "ShaderNodeObjectInfo":
            if output_name == "Location":
                return self._emit_result(key, "osl_to_world_point(vec3(0, 0, 0))", "vec3", 1)
            if output_name == "Color":
                return self._emit_result(key, "osl_object_color()", "vec3", 1)
            if output_name == "Alpha":
                return self._emit_result(key, "osl_object_alpha()", "scalar", 1)
            if output_name == "Object Index":
                return self._emit_result(key, "osl_object_index()", "scalar", 1)
            if output_name == "Material Index":
                return (_fmt(self._material_pass_index), "scalar", 0)
            if output_name == "Random":
                return self._emit_result(key, "osl_object_random()", "scalar", 1)
            raise _UnsupportedGraph(f"unsupported ObjectInfo output {output_name}")

        if bl == "ShaderNodeAttribute":
            attr_type = getattr(node, "attribute_type", "GEOMETRY")
            if attr_type != "GEOMETRY":
                raise _UnsupportedGraph(f"unsupported Attribute type {attr_type}")
            attr_name = str(getattr(node, "attribute_name", "") or "")
            if not attr_name:
                raise _UnsupportedGraph("empty Attribute name")
            escaped = _escape_expr_string(attr_name)
            self._mesh_attributes.add(attr_name)
            if output_name in ("Color", "Vector"):
                return self._emit_result(key, f'mesh_attr3("{escaped}")', "vec3", 1)
            if output_name == "Fac":
                if self._attr_types.get(attr_name) == "FLOAT2":
                    # Exact Cycles `attribute.h:104-125`: NODE_ATTR_FLOAT2 Fac returns f.x.
                    return self._emit_result(key, f'mesh_attr3("{escaped}").x', "scalar", 2)
                # Exact Cycles `average(f) = (f.x + f.y + f.z) / 3.0` (attribute.h:63,141,152)
                return self._emit_result(
                    key,
                    f'dot(mesh_attr3("{escaped}"), vec3(0.3333333432674408, 0.3333333432674408, 0.3333333432674408))',
                    "scalar",
                    2,
                )
            if output_name == "Alpha":
                return self._emit_result(key, f'mesh_attr_alpha("{escaped}")', "scalar", 1)
            raise _UnsupportedGraph(f"unsupported Attribute output {output_name}")

        if bl == "ShaderNodeAmbientOcclusion":
            if output_name not in ("AO", "Color"):
                raise _UnsupportedGraph(f"unsupported AmbientOcclusion output {output_name}")
            shared_key = (self._scoped_nid(node), "__ao_shared")
            if shared_key in self._cache:
                ao_var, _, ao_d = self._cache[shared_key]
            else:
                samples = max(1, min(128, int(getattr(node, "samples", 16))))
                inside = bool(getattr(node, "inside", False))
                only_local = bool(getattr(node, "only_local", False))
                normal_sock = _input_by_name(node, "Normal")
                normal_linked = bool(_live_links(normal_sock))
                flags = (1 if inside else 0) | (2 if only_local else 0) | (4 if normal_linked else 0)
                dist, dd = self.eval_input(_input_by_name(node, "Distance"), "scalar", depth, missing_default=1.0)
                if normal_linked:
                    norm, nd = self.eval_input(normal_sock, "vec3", depth, missing_default=(0.0, 0.0, 0.0))
                else:
                    norm, nd = "vec3(0, 0, 0)", 0
                ao_expr = f"osl_ao({dist}, {samples}, {norm}, {flags})"
                # Bind osl_ao into a def slot so AO + Color or multi-use consumers
                # never fire redundant ray-traced AO hemispheres.
                ao_var, _, ao_d = self._bind(ao_expr, "scalar")
                self._record_depth(max(dd, nd) + 1)
                self._cache[shared_key] = (ao_var, "scalar", ao_d)
            if output_name == "AO":
                return (ao_var, "scalar", ao_d)
            col, cd = self.eval_input(_input_by_name(node, "Color"), "vec3", depth, missing_default=(1.0, 1.0, 1.0))
            return self._emit_result(key, f"(({col}) * {ao_var})", "vec3", cd + 1)

        if bl == "ShaderNodeVertexColor":
            layer_name = str(getattr(node, "layer_name", "") or "")
            attr_name = layer_name if layer_name else ACTIVE_COLOR_ATTR
            escaped = _escape_expr_string(attr_name)
            self._mesh_attributes.add(attr_name)
            if output_name == "Color":
                return self._emit_result(key, f'mesh_attr3("{escaped}")', "vec3", 1)
            if output_name == "Alpha":
                return self._emit_result(key, f'mesh_attr_alpha("{escaped}")', "scalar", 1)
            raise _UnsupportedGraph(f"unsupported VertexColor output {output_name}")

        if bl == "ShaderNodeNewGeometry":
            if output_name == "Backfacing":
                return self._emit_result(key, "osl_backfacing()", "scalar", 1)
            if output_name == "Random Per Island":
                self._mesh_attributes.add(ISLAND_RANDOM_ATTR)
                return self._emit_result(key, f'mesh_attr3("{ISLAND_RANDOM_ATTR}").x', "scalar", 2)
            if output_name == "Position":
                return ("P", "vec3", 0)
            if output_name == "Normal":
                return self._emit_result(key, "osl_shading_normal()", "vec3", 1)
            if output_name == "True Normal":
                return self._emit_result(key, "osl_true_normal()", "vec3", 1)
            raise _UnsupportedGraph(f"unsupported NewGeometry output {output_name}")

        if bl == "ShaderNodeParticleInfo":
            if output_name == "Random":
                return self._emit_result(key, "osl_particle_random()", "scalar", 1)
            raise _UnsupportedGraph(f"unsupported ParticleInfo output {output_name}")

        if bl == "ShaderNodeTexGradient":
            gtype = _GRADIENT_TYPE_MAP.get(getattr(node, "gradient_type", "LINEAR"))
            if gtype is None or output_name not in ("Fac", "Color"):
                raise _UnsupportedGraph(f"unsupported TexGradient type/output {output_name}")
            vec_sock = _input_by_name(node, "Vector")
            if vec_sock is not None and _live_links(vec_sock):
                vec, vd = self.eval_input(vec_sock, "vec3", depth)
            else:
                vec, _, vd = self._generated_coord()
            fac_expr = f"osl_gradient({vec}, {gtype})"
            if output_name == "Fac":
                return self._emit_result(key, fac_expr, "scalar", vd + 1)
            fvar = self._bind_if_needed_for_multi_ref((fac_expr, "scalar", vd + 1))
            return self._emit_result(key, f"vec3({fvar}, {fvar}, {fvar})", "vec3", 1)

        if bl == "ShaderNodeTexBrick":
            if output_name not in ("Color", "Fac"):
                raise _UnsupportedGraph(f"unsupported TexBrick output {output_name}")
            shared_key = (self._scoped_nid(node), "__brick_shared")
            if shared_key in self._cache:
                bvar, _, bd = self._cache[shared_key]
            else:
                vec_sock = _input_by_name(node, "Vector")
                if vec_sock is not None and _live_links(vec_sock):
                    vec, vd = self.eval_input(vec_sock, "vec3", depth)
                else:
                    vec, _, vd = self._generated_coord()
                scale, sd = self.eval_input(_input_by_name(node, "Scale"), "scalar", depth, missing_default=5.0)
                mortar_size, mszd = self.eval_input(_input_by_name(node, "Mortar Size"), "scalar", depth, missing_default=0.02)
                mortar_smooth, msmd = self.eval_input(_input_by_name(node, "Mortar Smooth"), "scalar", depth, missing_default=0.1)
                bias, biasd = self.eval_input(_input_by_name(node, "Bias"), "scalar", depth, missing_default=0.0)
                brick_width, bwd = self.eval_input(_input_by_name(node, "Brick Width"), "scalar", depth, missing_default=0.5)
                row_height, rhd = self.eval_input(_input_by_name(node, "Row Height"), "scalar", depth, missing_default=0.25)
                offset_amt = float(getattr(node, "offset", 0.5))
                offset_freq = max(0, min(1000000, int(getattr(node, "offset_frequency", 2))))
                squash_amt = float(getattr(node, "squash", 1.0))
                squash_freq = max(0, min(1000000, int(getattr(node, "squash_frequency", 2))))
                p_scaled = f"(({vec}) * ({scale}))"
                brick_expr = (
                    f"osl_brick({p_scaled}, {mortar_size}, {mortar_smooth}, {bias}, "
                    f"{brick_width}, {row_height}, {_fmt(offset_amt)}, {offset_freq}, "
                    f"{_fmt(squash_amt)}, {squash_freq})"
                )
                bvar, _, bd = self._bind(brick_expr, "vec3")
                self._record_depth(max(vd, sd, mszd, msmd, biasd, bwd, rhd) + 2)
                self._cache[shared_key] = (bvar, "vec3", bd)
            if output_name == "Fac":
                return (f"{bvar}.y", "scalar", bd + 1)
            c1, c1d = self.eval_input(_input_by_name(node, "Color1"), "vec3", depth, missing_default=(0.8, 0.8, 0.8))
            c2, c2d = self.eval_input(_input_by_name(node, "Color2"), "vec3", depth, missing_default=(0.2, 0.2, 0.2))
            mc, mcd = self.eval_input(_input_by_name(node, "Mortar"), "vec3", depth, missing_default=(0.0, 0.0, 0.0))
            col_expr = f"mix(mix({c1}, {c2}, {bvar}.x), {mc}, {bvar}.y)"
            return self._emit_result(key, col_expr, "vec3", max(c1d, c2d, mcd, bd) + 2)

        if bl == "ShaderNodeUVMap":
            if getattr(node, "from_instancer", False):
                raise _UnsupportedGraph("UVMap.from_instancer not supported")
            if output_name != "UV":
                raise _UnsupportedGraph(f"unsupported UVMap output {output_name}")
            uv_name = str(getattr(node, "uv_map", "") or "")
            if not uv_name:
                return ("vec3(u, v, 0)", "vec3", 1)
            attr_name = UV_MAP_ATTR_PREFIX + uv_name
            escaped = _escape_expr_string(attr_name)
            self._mesh_attributes.add(attr_name)
            return self._emit_result(key, f'mesh_attr3("{escaped}")', "vec3", 1)

        if bl == "ShaderNodeTexImage":
            return self._compile_tex_image(node, output_name, depth, key)

        if bl == "ShaderNodeRGBCurve":
            return self._compile_rgb_curve(node, output_name, depth, key)

        if bl == "ShaderNodeTexCoord":
            if getattr(node, "from_instancer", False):
                raise _UnsupportedGraph("TexCoord.from_instancer not supported")
            if getattr(node, "object", None) is not None and output_name == "Object":
                raise _UnsupportedGraph("TexCoord.Object with external object reference")
            if output_name == "Generated":
                return self._generated_coord()
            if output_name == "Object":
                return ("Po", "vec3", 0)
            if output_name == "UV":
                return ("vec3(u, v, 0)", "vec3", 1)
            if output_name == "Normal":
                return self._emit_result(key, "osl_to_object_normal(N)", "vec3", 1)
            if output_name == "Camera":
                return self._emit_result(key, "osl_to_camera_point(P)", "vec3", 1)
            raise _UnsupportedGraph(f"unsupported TexCoord output {output_name}")

        if bl == "ShaderNodeMapping":
            return self._compile_mapping(node, depth, key)

        if bl == "ShaderNodeTexVoronoi":
            return self._compile_voronoi(node, output_name, depth, key)

        if bl == "ShaderNodeTexNoise":
            return self._compile_noise(node, output_name, depth, key)

        if bl == "ShaderNodeTexWhiteNoise":
            return self._compile_white_noise(node, output_name, depth, key)

        if bl in ("ShaderNodeMix", "ShaderNodeMixRGB"):
            return self._compile_mix(node, output_name, depth, key)

        if bl == "ShaderNodeMath":
            return self._compile_math(node, depth, key)

        if bl == "ShaderNodeVectorMath":
            return self._compile_vector_math(node, output_name, depth, key)

        if bl == "ShaderNodeClamp":
            return self._compile_clamp(node, depth, key)

        if bl == "ShaderNodeMapRange":
            return self._compile_map_range(node, output_name, depth, key)

        if bl == "ShaderNodeVectorRotate":
            return self._compile_vector_rotate(node, depth, key)

        if bl == "ShaderNodeVectorTransform":
            return self._compile_vector_transform(node, depth, key)

        if bl == "ShaderNodeRGBToBW":
            col, cd = self.eval_input(_input_by_name(node, "Color"), "vec3", depth)
            return self._emit_result(key, f"dot({col}, vec3(0.2126, 0.7152, 0.0722))", "scalar", cd + 1)

        if bl == "ShaderNodeHueSaturation":
            return self._compile_hue_sat(node, depth, key)

        if bl == "ShaderNodeInvert":
            fac, fd = self.eval_input(_input_by_names(node, ("Fac", "Factor")), "scalar", depth)
            col, cd = self.eval_input(_input_by_name(node, "Color"), "vec3", depth)
            cvar = self._bind_if_needed_for_multi_ref((col, "vec3", cd))
            ed = max(fd, 0 if cvar != col else cd) + 2
            return self._emit_result(key, f"mix({cvar}, (vec3(1, 1, 1) - {cvar}), {fac})", "vec3", ed)

        if bl == "ShaderNodeBrightContrast":
            col, cd = self.eval_input(_input_by_name(node, "Color"), "vec3", depth)
            bright, bd = self.eval_input(_input_by_names(node, ("Bright", "Brightness")), "scalar", depth)
            contrast, kd = self.eval_input(_input_by_name(node, "Contrast"), "scalar", depth)
            cvar = self._bind_if_needed_for_multi_ref((col, "vec3", cd))
            bvar = self._bind_if_needed_for_multi_ref((bright, "scalar", bd))
            kvar = self._bind_if_needed_for_multi_ref((contrast, "scalar", kd))
            a = f"(1 + {kvar})"
            b = f"({bvar} - 0.5 * {kvar})"
            return self._emit_result(
                key,
                f"vec3(max({a} * {cvar}.x + {b}, 0), max({a} * {cvar}.y + {b}, 0), max({a} * {cvar}.z + {b}, 0))",
                "vec3",
                4,
            )

        if bl == "ShaderNodeGamma":
            col, cd = self.eval_input(_input_by_name(node, "Color"), "vec3", depth)
            gamma, gd = self.eval_input(_input_by_name(node, "Gamma"), "scalar", depth)
            cvar = self._bind_if_needed_for_multi_ref((col, "vec3", cd))
            gvar = self._bind_if_needed_for_multi_ref((gamma, "scalar", gd))
            return self._emit_result(
                key,
                f"vec3(select({cvar}.x > 0, pow(max({cvar}.x, 0), {gvar}), {cvar}.x), "
                f"select({cvar}.y > 0, pow(max({cvar}.y, 0), {gvar}), {cvar}.y), "
                f"select({cvar}.z > 0, pow(max({cvar}.z, 0), {gvar}), {cvar}.z))",
                "vec3",
                4,
            )

        if bl == "ShaderNodeSeparateXYZ":
            comp = {"X": "x", "Y": "y", "Z": "z"}.get(output_name)
            if comp is None:
                raise _UnsupportedGraph(f"SeparateXYZ output {output_name}")
            shared_key = (self._scoped_nid(node), "__sepxyz")
            if shared_key in self._cache:
                vvar, _, vd = self._cache[shared_key]
            else:
                vec, vd = self.eval_input(_input_by_name(node, "Vector"), "vec3", depth)
                used_outs = self._node_outputs_used.get(self._scoped_nid(node), set())
                if len(used_outs) > 1 or not _is_atomic_expr(vec):
                    vvar, _, vd = self._bind(vec, "vec3")
                else:
                    vvar = vec
                self._cache[shared_key] = (vvar, "vec3", vd)
            return (f"{vvar}.{comp}", "scalar", vd + 1)

        if bl == "ShaderNodeCombineXYZ":
            x, xd = self.eval_input(_input_by_name(node, "X"), "scalar", depth)
            y, yd = self.eval_input(_input_by_name(node, "Y"), "scalar", depth)
            z, zd = self.eval_input(_input_by_name(node, "Z"), "scalar", depth)
            return self._emit_result(key, f"vec3({x}, {y}, {z})", "vec3", max(xd, yd, zd) + 1)

        if bl == "ShaderNodeSeparateColor":
            mode = getattr(node, "mode", "RGB")
            if mode == "HSV":
                comp = {
                    "Red": "x", "Hue": "x",
                    "Green": "y", "Saturation": "y",
                    "Blue": "z", "Value": "z",
                }.get(output_name)
            elif mode == "HSL":
                comp = {
                    "Red": "x", "Hue": "x",
                    "Green": "y", "Saturation": "y",
                    "Blue": "z", "Lightness": "z",
                }.get(output_name)
            elif mode == "RGB":
                comp = {"Red": "x", "Green": "y", "Blue": "z"}.get(output_name)
            else:
                raise _UnsupportedGraph(f"SeparateColor mode {mode}")
            if comp is None:
                raise _UnsupportedGraph(f"SeparateColor output {output_name}")
            shared_key = (self._scoped_nid(node), f"__sepcol_{mode}")
            if shared_key in self._cache:
                vvar, _, vd = self._cache[shared_key]
            else:
                col, cd = self.eval_input(_input_by_name(node, "Color"), "vec3", depth)
                if mode == "HSV":
                    vvar, _, vd = self._bind(f"osl_rgb2hsv({col})", "vec3")
                elif mode == "HSL":
                    vvar, _, vd = self._bind(f"osl_rgb2hsl({col})", "vec3")
                else:
                    vvar, _, vd = self._bind(col, "vec3")
                self._cache[shared_key] = (vvar, "vec3", vd)
            return (f"{vvar}.{comp}", "scalar", vd + 1)

        if bl == "ShaderNodeCombineColor":
            mode = getattr(node, "mode", "RGB")
            if mode == "HSV":
                a, ad = self.eval_input(_input_by_names(node, ("Hue", "Red")), "scalar", depth)
                b, bd = self.eval_input(_input_by_names(node, ("Saturation", "Green")), "scalar", depth)
                c, cd = self.eval_input(_input_by_names(node, ("Value", "Blue")), "scalar", depth)
                return self._emit_result(key, f"osl_hsv2rgb(vec3({a}, {b}, {c}))", "vec3", max(ad, bd, cd) + 2)
            if mode == "HSL":
                a, ad = self.eval_input(_input_by_names(node, ("Hue", "Red")), "scalar", depth)
                b, bd = self.eval_input(_input_by_names(node, ("Saturation", "Green")), "scalar", depth)
                c, cd = self.eval_input(_input_by_names(node, ("Lightness", "Blue")), "scalar", depth)
                return self._emit_result(key, f"osl_hsl2rgb(vec3({a}, {b}, {c}))", "vec3", max(ad, bd, cd) + 2)
            if mode == "RGB":
                a, ad = self.eval_input(_input_by_name(node, "Red"), "scalar", depth)
                b, bd = self.eval_input(_input_by_name(node, "Green"), "scalar", depth)
                c, cd = self.eval_input(_input_by_name(node, "Blue"), "scalar", depth)
                return self._emit_result(key, f"vec3({a}, {b}, {c})", "vec3", max(ad, bd, cd) + 1)
            raise _UnsupportedGraph(f"CombineColor mode {mode}")

        if bl == "ShaderNodeValToRGB":
            return self._compile_color_ramp(node, output_name, depth, key)

        raise _UnsupportedGraph(f"unsupported node {bl}")

    def _compile_mapping(self, node, depth: int, key: tuple[int, str]) -> tuple[str, str, int]:
        loc_sock = _input_by_name(node, "Location")
        rot_sock = _input_by_name(node, "Rotation")
        scl_sock = _input_by_name(node, "Scale")
        vtype = getattr(node, "vector_type", "POINT")
        vec_sock = _input_by_name(node, "Vector")
        vec_expr, vec_d = self.eval_input(vec_sock, "vec3", depth)

        if _live_links(loc_sock) or _live_links(rot_sock) or _live_links(scl_sock):
            # Dynamic Mapping using Group 5/6 builtins (matches Cycles mapping.h).
            loc_expr, loc_d = self.eval_input(loc_sock, "vec3", depth, missing_default=(0.0, 0.0, 0.0))
            rot_expr, rot_d = self.eval_input(rot_sock, "vec3", depth, missing_default=(0.0, 0.0, 0.0))
            scl_expr, scl_d = self.eval_input(scl_sock, "vec3", depth, missing_default=(1.0, 1.0, 1.0))
            zero3 = "vec3(0, 0, 0)"
            ed = max(vec_d, loc_d, rot_d, scl_d) + 3
            if vtype == "POINT":
                scaled = f"(({vec_expr}) * ({scl_expr}))"
                rot_call = f"osl_vector_rotate(4, 0, {scaled}, {zero3}, {zero3}, 0, {rot_expr})"
                return self._emit_result(key, f"({rot_call} + ({loc_expr}))", "vec3", ed)
            if vtype == "VECTOR":
                scaled = f"(({vec_expr}) * ({scl_expr}))"
                rot_call = f"osl_vector_rotate(4, 0, {scaled}, {zero3}, {zero3}, 0, {rot_expr})"
                return self._emit_result(key, rot_call, "vec3", ed)
            if vtype == "TEXTURE":
                diff = f"(({vec_expr}) - ({loc_expr}))"
                inv_rot = f"osl_vector_rotate(4, 1, {diff}, {zero3}, {zero3}, 0, {rot_expr})"
                return self._emit_result(key, f"osl_vmath(3, {inv_rot}, {scl_expr}, {zero3}, 0)", "vec3", ed)
            if vtype == "NORMAL":
                div_s = f"osl_vmath(3, {vec_expr}, {scl_expr}, {zero3}, 0)"
                rot_call = f"osl_vector_rotate(4, 0, {div_s}, {zero3}, {zero3}, 0, {rot_expr})"
                return self._emit_result(key, f"osl_vmath(11, {rot_call}, {zero3}, {zero3}, 0)", "vec3", ed)
            raise _UnsupportedGraph(f"Mapping vector_type {vtype}")

        vvar = self._bind_if_needed_for_multi_ref((vec_expr, "vec3", vec_d))
        loc = _default_vec3(loc_sock, (0.0, 0.0, 0.0))
        rot = _default_vec3(rot_sock, (0.0, 0.0, 0.0))
        scl = _default_vec3(scl_sock, (1.0, 1.0, 1.0))
        rot_m = _euler_xyz_matrix(rot[0], rot[1], rot[2])

        if vtype == "POINT":
            mat = _mat3_mul_diag(rot_m, scl[0], scl[1], scl[2])
            return self._emit_result(key, _emit_affine(vvar, mat, loc), "vec3", 3)
        if vtype == "VECTOR":
            mat = _mat3_mul_diag(rot_m, scl[0], scl[1], scl[2])
            return self._emit_result(key, _emit_affine(vvar, mat), "vec3", 3)
        if vtype == "TEXTURE":
            inv_s = tuple((1.0 / s) if abs(s) > 1e-12 else 0.0 for s in scl)
            rt = _mat3_transpose(rot_m)
            mat = _diag_mul_mat3(inv_s[0], inv_s[1], inv_s[2], rt)
            trans = tuple(
                -(mat[r][0] * loc[0] + mat[r][1] * loc[1] + mat[r][2] * loc[2])
                for r in range(3)
            )
            return self._emit_result(key, _emit_affine(vvar, mat, trans), "vec3", 3)
        if vtype == "NORMAL":
            inv_s = tuple((1.0 / s) if abs(s) > 1e-12 else 0.0 for s in scl)
            mat = _mat3_mul_diag(rot_m, inv_s[0], inv_s[1], inv_s[2])
            raw = _emit_affine(vvar, mat)
            return self._emit_result(key, f"normalize({raw})", "vec3", 4)
        raise _UnsupportedGraph(f"Mapping vector_type {vtype}")

    def _compile_voronoi(
        self, node, output_name: str, depth: int, key: tuple[int, str]
    ) -> tuple[str, str, int]:
        dims = _DIMS_MAP.get(getattr(node, "voronoi_dimensions", "3D"))
        feature = _VORONOI_FEATURE_MAP.get(getattr(node, "feature", "F1"))
        metric = _VORONOI_METRIC_MAP.get(getattr(node, "distance", "EUCLIDEAN"))
        out_spec = _VORONOI_OUTPUT_FN.get(output_name)
        if dims is None or feature is None or metric is None or out_spec is None:
            raise _UnsupportedGraph(f"unsupported Voronoi config/output {output_name}")

        normalize_flag = 1 if bool(getattr(node, "normalize", False)) else 0

        vec_sock = _input_by_name(node, "Vector")
        if dims == 1:
            p_expr, p_d = "vec3(0, 0, 0)", 0
        elif _live_links(vec_sock):
            p_expr, p_d = self.eval_input(vec_sock, "vec3", depth)
        else:
            p_expr, _, p_d = self._generated_coord()

        w_expr, w_d = self.eval_input(_input_by_name(node, "W"), "scalar", depth, missing_default=0.0)
        scale_expr, sc_d = self.eval_input(_input_by_name(node, "Scale"), "scalar", depth, missing_default=5.0)
        detail_expr, dt_d = self.eval_input(_input_by_name(node, "Detail"), "scalar", depth, missing_default=0.0)
        rough_expr, rg_d = self.eval_input(_input_by_name(node, "Roughness"), "scalar", depth, missing_default=0.5)
        lac_expr, lc_d = self.eval_input(_input_by_name(node, "Lacunarity"), "scalar", depth, missing_default=2.0)
        smooth_expr, sm_d = self.eval_input(_input_by_name(node, "Smoothness"), "scalar", depth, missing_default=1.0)
        exp_expr, ex_d = self.eval_input(_input_by_name(node, "Exponent"), "scalar", depth, missing_default=0.5)
        rand_expr, rn_d = self.eval_input(_input_by_name(node, "Randomness"), "scalar", depth, missing_default=1.0)

        fn_name, ret_type = out_spec
        call = (
            f"{fn_name}({p_expr}, {w_expr}, {dims}, {feature}, {metric}, "
            f"{scale_expr}, {detail_expr}, {rough_expr}, {lac_expr}, "
            f"{smooth_expr}, {exp_expr}, {rand_expr}, {normalize_flag})"
        )
        ed = max(p_d, w_d, sc_d, dt_d, rg_d, lc_d, sm_d, ex_d, rn_d) + 1
        return self._emit_result(key, call, ret_type, ed)

    def _compile_noise(
        self, node, output_name: str, depth: int, key: tuple[int, str]
    ) -> tuple[str, str, int]:
        dims = _DIMS_MAP.get(getattr(node, "noise_dimensions", "3D"))
        ntype = _NOISE_TYPE_MAP.get(getattr(node, "noise_type", "FBM"))
        if output_name == "Factor":
            output_name = "Fac"
        if dims is None or ntype is None or output_name not in ("Fac", "Color"):
            raise _UnsupportedGraph(f"unsupported Noise config/output {output_name}")

        snid = self._scoped_nid(node)
        used_outs = self._node_outputs_used.get(snid, {output_name})
        # Design doc §5.2(b): Color's .x channel IS Fac bit-for-bit; when both
        # Fac and Color of the same Noise node are used, evaluate Color ONCE in
        # a def and serve Fac as `.x` (3 fractal evaluations instead of 4).
        if "Fac" in used_outs and "Color" in used_outs:
            shared_key = (snid, "__noise_color_shared")
            if shared_key not in self._cache:
                cvar, _, cd = self._emit_noise_call(node, "Color", dims, ntype, depth, shared_key, force_bind=True)
                self._cache[shared_key] = (cvar, "vec3", cd)
            cvar, _, cd = self._cache[shared_key]
            if output_name == "Color":
                return (cvar, "vec3", cd)
            return (f"{cvar}.x", "scalar", cd + 1)

        return self._emit_noise_call(node, output_name, dims, ntype, depth, key, force_bind=False)

    def _emit_noise_call(
        self,
        node,
        output_name: str,
        dims: int,
        ntype: int,
        depth: int,
        key: tuple[int, str],
        *,
        force_bind: bool,
    ) -> tuple[str, str, int]:
        normalize_flag = 1 if bool(getattr(node, "normalize", True)) else 0
        vec_sock = _input_by_name(node, "Vector")
        if dims == 1:
            p_expr, p_d = "vec3(0, 0, 0)", 0
        elif _live_links(vec_sock):
            p_expr, p_d = self.eval_input(vec_sock, "vec3", depth)
        else:
            p_expr, _, p_d = self._generated_coord()

        w_expr, w_d = self.eval_input(_input_by_name(node, "W"), "scalar", depth, missing_default=0.0)
        scale_expr, sc_d = self.eval_input(_input_by_name(node, "Scale"), "scalar", depth, missing_default=5.0)
        svar = self._bind_if_needed_for_multi_ref((scale_expr, "scalar", sc_d))
        detail_expr, dt_d = self.eval_input(_input_by_name(node, "Detail"), "scalar", depth, missing_default=2.0)
        rough_expr, rg_d = self.eval_input(_input_by_name(node, "Roughness"), "scalar", depth, missing_default=0.5)
        lac_expr, lc_d = self.eval_input(_input_by_name(node, "Lacunarity"), "scalar", depth, missing_default=2.0)
        offset_expr, of_d = self.eval_input(_input_by_name(node, "Offset"), "scalar", depth, missing_default=0.0)
        gain_expr, gn_d = self.eval_input(_input_by_name(node, "Gain"), "scalar", depth, missing_default=1.0)
        dist_expr, ds_d = self.eval_input(_input_by_name(node, "Distortion"), "scalar", depth, missing_default=0.0)

        fn_name = "osl_noise_texture_color" if output_name == "Color" else "osl_noise_texture"
        ret_type = "vec3" if output_name == "Color" else "scalar"
        call = (
            f"{fn_name}(({p_expr}) * {svar}, ({w_expr}) * {svar}, {dims}, {ntype}, "
            f"{detail_expr}, {rough_expr}, {lac_expr}, {offset_expr}, {gain_expr}, "
            f"{dist_expr}, {normalize_flag})"
        )
        ed = max(p_d + 1, w_d + 1, dt_d, rg_d, lc_d, of_d, gn_d, ds_d) + 1
        return self._emit_result(key, call, ret_type, ed, force_bind=force_bind)

    def _compile_white_noise(
        self, node, output_name: str, depth: int, key: tuple[int, str]
    ) -> tuple[str, str, int]:
        dims = _DIMS_MAP.get(getattr(node, "noise_dimensions", "3D"))
        if dims is None or output_name not in ("Value", "Color"):
            raise _UnsupportedGraph(f"unsupported White Noise config/output {output_name}")
        vec_sock = _input_by_name(node, "Vector")
        if dims == 1:
            p_expr, p_d = "vec3(0, 0, 0)", 0
        else:
            p_expr, p_d = self.eval_input(vec_sock, "vec3", depth, missing_default=(0.0, 0.0, 0.0))
        w_expr, w_d = self.eval_input(_input_by_name(node, "W"), "scalar", depth, missing_default=0.0)
        fn_name = "osl_white_noise_color" if output_name == "Color" else "osl_white_noise"
        ret_type = "vec3" if output_name == "Color" else "scalar"
        return self._emit_result(key, f"{fn_name}({p_expr}, {w_expr}, {dims})", ret_type, max(p_d, w_d) + 1)

    def _compile_mix(
        self, node, output_name: str, depth: int, key: tuple[int, str]
    ) -> tuple[str, str, int]:
        bl = getattr(node, "bl_idname", "")
        if bl == "ShaderNodeMixRGB":
            mode = _MIX_MODE_MAP.get(getattr(node, "blend_type", "MIX"))
            if mode is None:
                raise _UnsupportedGraph(f"MixRGB blend_type {getattr(node, 'blend_type', '')}")
            t, td = self.eval_input(_input_by_names(node, ("Fac", "Factor")), "scalar", depth)
            a, ad = self.eval_input(_input_by_name(node, "Color1"), "vec3", depth)
            b, bd = self.eval_input(_input_by_name(node, "Color2"), "vec3", depth)
            clamp_result = 1 if bool(getattr(node, "use_clamp", False)) else 0
            return self._emit_result(
                key,
                f"osl_mix_color({mode}, {t}, {a}, {b}, 1, {clamp_result})",
                "vec3",
                max(td, ad, bd) + 1,
            )

        data_type = getattr(node, "data_type", "FLOAT")
        clamp_factor = bool(getattr(node, "clamp_factor", True))
        if data_type == "RGBA":
            mode = _MIX_MODE_MAP.get(getattr(node, "blend_type", "MIX"))
            if mode is None:
                raise _UnsupportedGraph(f"Mix RGBA blend_type {getattr(node, 'blend_type', '')}")
            t, td = self.eval_input(_input_by_name(node, "Factor", "VALUE"), "scalar", depth)
            a, ad = self.eval_input(_input_by_name(node, "A", "RGBA"), "vec3", depth)
            b, bd = self.eval_input(_input_by_name(node, "B", "RGBA"), "vec3", depth)
            cf = 1 if clamp_factor else 0
            cr = 1 if bool(getattr(node, "clamp_result", False)) else 0
            return self._emit_result(
                key,
                f"osl_mix_color({mode}, {t}, {a}, {b}, {cf}, {cr})",
                "vec3",
                max(td, ad, bd) + 1,
            )
        if data_type == "FLOAT":
            t, td = self.eval_input(_input_by_name(node, "Factor", "VALUE"), "scalar", depth)
            if clamp_factor:
                t = f"clamp({t}, 0, 1)"
                td += 1
            a, ad = self.eval_input(_input_by_name(node, "A", "VALUE"), "scalar", depth)
            b, bd = self.eval_input(_input_by_name(node, "B", "VALUE"), "scalar", depth)
            return self._emit_result(key, f"mix({a}, {b}, {t})", "scalar", max(td, ad, bd) + 1)
        if data_type == "VECTOR":
            factor_mode = getattr(node, "factor_mode", "UNIFORM")
            a, ad = self.eval_input(_input_by_name(node, "A", "VECTOR"), "vec3", depth)
            b, bd = self.eval_input(_input_by_name(node, "B", "VECTOR"), "vec3", depth)
            if factor_mode == "UNIFORM":
                t, td = self.eval_input(_input_by_name(node, "Factor", "VALUE"), "scalar", depth)
                if clamp_factor:
                    t = f"clamp({t}, 0, 1)"
                    td += 1
                return self._emit_result(key, f"mix({a}, {b}, {t})", "vec3", max(td, ad, bd) + 1)
            if factor_mode == "NON_UNIFORM":
                tv_sock = _input_by_name(node, "Factor", "VECTOR") or _input_by_name(node, "Factor")
                t, td = self.eval_input(tv_sock, "vec3", depth)
                if clamp_factor:
                    t = f"osl_vclamp({t}, vec3(0, 0, 0), vec3(1, 1, 1))"
                    td += 1
                avar = self._bind_if_needed_for_multi_ref((a, "vec3", ad))
                return self._emit_result(
                    key,
                    f"({avar} + (({b}) - {avar}) * ({t}))",
                    "vec3",
                    max(td, bd, 0 if avar != a else ad) + 2,
                )
            raise _UnsupportedGraph(f"Mix VECTOR factor_mode {factor_mode}")
        raise _UnsupportedGraph(f"Mix data_type {data_type}")

    def _compile_math(self, node, depth: int, key: tuple[int, str]) -> tuple[str, str, int]:
        op_name = getattr(node, "operation", "ADD")
        op = _MATH_OP_MAP.get(op_name)
        if op is None:
            raise _UnsupportedGraph(f"unsupported Math operation {op_name}")
        inps = list(getattr(node, "inputs", ()))
        s0 = inps[0] if len(inps) > 0 else None
        s1 = inps[1] if len(inps) > 1 else None
        s2 = inps[2] if len(inps) > 2 else None
        a, ad = self.eval_input(s0, "scalar", depth, missing_default=0.5)
        if op in _MATH_UNARY_OPS:
            b, bd = "0", 0
            c, cd = "0", 0
        elif op in _MATH_TERNARY_OPS:
            b, bd = self.eval_input(s1, "scalar", depth, missing_default=0.5)
            c, cd = self.eval_input(s2, "scalar", depth, missing_default=0.5)
        else:
            b, bd = self.eval_input(s1, "scalar", depth, missing_default=0.5)
            c, cd = "0", 0
        expr = f"osl_math({op}, {a}, {b}, {c})"
        ed = max(ad, bd, cd) + 1
        if bool(getattr(node, "use_clamp", False)):
            expr = f"osl_clamp(0, {expr}, 0, 1)"
            ed += 1
        return self._emit_result(key, expr, "scalar", ed)

    def _compile_vector_math(
        self, node, output_name: str, depth: int, key: tuple[int, str]
    ) -> tuple[str, str, int]:
        op_name = getattr(node, "operation", "ADD")
        op = _VECTOR_MATH_OP_MAP.get(op_name)
        if op is None:
            raise _UnsupportedGraph(f"unsupported VectorMath operation {op_name}")
        vec_inps = _inputs_of_type(node, "VECTOR")
        if not vec_inps:
            # Fallback for minimal stub nodes whose sockets lack explicit VECTOR types
            vec_inps = [inp for inp in getattr(node, "inputs", ()) if inp.name not in ("Scale", "IOR")]
        s0 = vec_inps[0] if len(vec_inps) > 0 else None
        s1 = vec_inps[1] if len(vec_inps) > 1 else None
        s2 = vec_inps[2] if len(vec_inps) > 2 else None
        scale_sock = _input_by_names(node, ("Scale", "IOR"))

        a, ad = self.eval_input(s0, "vec3", depth, missing_default=(0.0, 0.0, 0.0))

        if op in _VECTOR_MATH_SCALAR_OPS:
            if output_name != "Value":
                raise _UnsupportedGraph(f"VectorMath scalar op {op_name} requested output {output_name}")
            if op == 9:  # LENGTH
                b, bd = "vec3(0, 0, 0)", 0
            else:
                b, bd = self.eval_input(s1, "vec3", depth, missing_default=(0.0, 0.0, 0.0))
            expr = f"osl_vmath_scalar({op}, {a}, {b})"
            return self._emit_result(key, expr, "scalar", max(ad, bd) + 1)

        if output_name != "Vector":
            raise _UnsupportedGraph(f"VectorMath vector op {op_name} requested output {output_name}")
        if op in _VECTOR_MATH_UNARY_OPS:
            b, bd = "vec3(0, 0, 0)", 0
            c, cd = "vec3(0, 0, 0)", 0
            sc, sd = "1", 0
        elif op == 10:  # SCALE
            b, bd = "vec3(0, 0, 0)", 0
            c, cd = "vec3(0, 0, 0)", 0
            sc, sd = self.eval_input(scale_sock, "scalar", depth, missing_default=1.0)
        elif op == 24:  # REFRACT
            b, bd = self.eval_input(s1, "vec3", depth, missing_default=(0.0, 0.0, 0.0))
            c, cd = "vec3(0, 0, 0)", 0
            sc, sd = self.eval_input(scale_sock, "scalar", depth, missing_default=1.0)
        elif op in _VECTOR_MATH_TERNARY_OPS:
            b, bd = self.eval_input(s1, "vec3", depth, missing_default=(0.0, 0.0, 0.0))
            c, cd = self.eval_input(s2, "vec3", depth, missing_default=(0.0, 0.0, 0.0))
            sc, sd = "1", 0
        else:
            b, bd = self.eval_input(s1, "vec3", depth, missing_default=(0.0, 0.0, 0.0))
            c, cd = "vec3(0, 0, 0)", 0
            sc, sd = "1", 0
        expr = f"osl_vmath({op}, {a}, {b}, {c}, {sc})"
        return self._emit_result(key, expr, "vec3", max(ad, bd, cd, sd) + 1)

    def _compile_clamp(self, node, depth: int, key: tuple[int, str]) -> tuple[str, str, int]:
        ctype = _CLAMP_TYPE_MAP.get(getattr(node, "clamp_type", "MINMAX"))
        if ctype is None:
            raise _UnsupportedGraph(f"unsupported Clamp clamp_type {getattr(node, 'clamp_type', '')}")
        val, vd = self.eval_input(_input_by_name(node, "Value"), "scalar", depth, missing_default=1.0)
        mn, mnd = self.eval_input(_input_by_name(node, "Min"), "scalar", depth, missing_default=0.0)
        mx, mxd = self.eval_input(_input_by_name(node, "Max"), "scalar", depth, missing_default=1.0)
        return self._emit_result(key, f"osl_clamp({ctype}, {val}, {mn}, {mx})", "scalar", max(vd, mnd, mxd) + 1)

    def _compile_map_range(
        self, node, output_name: str, depth: int, key: tuple[int, str]
    ) -> tuple[str, str, int]:
        data_type = getattr(node, "data_type", "FLOAT")
        itype = _MAP_RANGE_TYPE_MAP.get(getattr(node, "interpolation_type", "LINEAR"))
        if itype is None:
            raise _UnsupportedGraph(f"unsupported MapRange interpolation {getattr(node, 'interpolation_type', '')}")
        clamp_flag = 1 if bool(getattr(node, "clamp", True)) else 0

        if data_type == "FLOAT":
            if output_name not in ("Result", "Value"):
                raise _UnsupportedGraph(f"MapRange FLOAT output {output_name}")
            val, vd = self.eval_input(_input_by_name(node, "Value", "VALUE") or _input_by_name(node, "Value"), "scalar", depth, missing_default=1.0)
            fmin, d1 = self.eval_input(_input_by_name(node, "From Min", "VALUE") or _input_by_name(node, "From Min"), "scalar", depth, missing_default=0.0)
            fmax, d2 = self.eval_input(_input_by_name(node, "From Max", "VALUE") or _input_by_name(node, "From Max"), "scalar", depth, missing_default=1.0)
            tmin, d3 = self.eval_input(_input_by_name(node, "To Min", "VALUE") or _input_by_name(node, "To Min"), "scalar", depth, missing_default=0.0)
            tmax, d4 = self.eval_input(_input_by_name(node, "To Max", "VALUE") or _input_by_name(node, "To Max"), "scalar", depth, missing_default=1.0)
            if itype == 1:  # STEPPED
                steps, d5 = self.eval_input(_input_by_name(node, "Steps", "VALUE") or _input_by_name(node, "Steps"), "scalar", depth, missing_default=4.0)
            else:
                steps, d5 = "4", 0
            expr = f"osl_map_range({itype}, {clamp_flag}, {val}, {fmin}, {fmax}, {tmin}, {tmax}, {steps})"
            return self._emit_result(key, expr, "scalar", max(vd, d1, d2, d3, d4, d5) + 1)

        if data_type == "FLOAT_VECTOR":
            if output_name != "Vector":
                raise _UnsupportedGraph(f"MapRange FLOAT_VECTOR output {output_name}")
            vec, vd = self.eval_input(_input_by_name(node, "Vector", "VECTOR") or _input_by_name(node, "Vector"), "vec3", depth, missing_default=(0.0, 0.0, 0.0))
            fmin, d1 = self.eval_input(_input_by_name(node, "From Min", "VECTOR") or _input_by_name(node, "From Min"), "vec3", depth, missing_default=(0.0, 0.0, 0.0))
            fmax, d2 = self.eval_input(_input_by_name(node, "From Max", "VECTOR") or _input_by_name(node, "From Max"), "vec3", depth, missing_default=(1.0, 1.0, 1.0))
            tmin, d3 = self.eval_input(_input_by_name(node, "To Min", "VECTOR") or _input_by_name(node, "To Min"), "vec3", depth, missing_default=(0.0, 0.0, 0.0))
            tmax, d4 = self.eval_input(_input_by_name(node, "To Max", "VECTOR") or _input_by_name(node, "To Max"), "vec3", depth, missing_default=(1.0, 1.0, 1.0))
            if itype == 1:  # STEPPED
                steps, d5 = self.eval_input(_input_by_name(node, "Steps", "VECTOR") or _input_by_name(node, "Steps"), "vec3", depth, missing_default=(4.0, 4.0, 4.0))
            else:
                steps, d5 = "vec3(4, 4, 4)", 0
            expr = f"osl_vmap_range({itype}, {clamp_flag}, {vec}, {fmin}, {fmax}, {tmin}, {tmax}, {steps})"
            return self._emit_result(key, expr, "vec3", max(vd, d1, d2, d3, d4, d5) + 1)

        raise _UnsupportedGraph(f"unsupported MapRange data_type {data_type}")

    def _compile_vector_rotate(self, node, depth: int, key: tuple[int, str]) -> tuple[str, str, int]:
        rtype = _VECTOR_ROTATE_TYPE_MAP.get(getattr(node, "rotation_type", "AXIS_ANGLE"))
        if rtype is None:
            raise _UnsupportedGraph(f"unsupported VectorRotate type {getattr(node, 'rotation_type', '')}")
        inv = 1 if bool(getattr(node, "invert", False)) else 0
        vec, d0 = self.eval_input(_input_by_name(node, "Vector"), "vec3", depth, missing_default=(0.0, 0.0, 0.0))
        center, d1 = self.eval_input(_input_by_name(node, "Center"), "vec3", depth, missing_default=(0.0, 0.0, 0.0))
        if rtype == 4:  # EULER_XYZ
            axis, d2 = "vec3(0, 0, 1)", 0
            angle, d3 = "0", 0
            euler, d4 = self.eval_input(_input_by_name(node, "Rotation"), "vec3", depth, missing_default=(0.0, 0.0, 0.0))
        elif rtype in (1, 2, 3):  # X/Y/Z_AXIS
            axis, d2 = "vec3(0, 0, 1)", 0
            angle, d3 = self.eval_input(_input_by_name(node, "Angle"), "scalar", depth, missing_default=0.0)
            euler, d4 = "vec3(0, 0, 0)", 0
        else:  # AXIS_ANGLE
            axis, d2 = self.eval_input(_input_by_name(node, "Axis"), "vec3", depth, missing_default=(0.0, 0.0, 1.0))
            angle, d3 = self.eval_input(_input_by_name(node, "Angle"), "scalar", depth, missing_default=0.0)
            euler, d4 = "vec3(0, 0, 0)", 0
        expr = f"osl_vector_rotate({rtype}, {inv}, {vec}, {center}, {axis}, {angle}, {euler})"
        return self._emit_result(key, expr, "vec3", max(d0, d1, d2, d3, d4) + 1)

    def _compile_vector_transform(self, node, depth: int, key: tuple[int, str]) -> tuple[str, str, int]:
        vtype = getattr(node, "vector_type", "VECTOR")
        cfrom = getattr(node, "convert_from", "WORLD")
        cto = getattr(node, "convert_to", "OBJECT")
        kind = {"POINT": "point", "VECTOR": "vector", "NORMAL": "normal"}.get(vtype)
        if kind is None or cfrom not in ("WORLD", "OBJECT", "CAMERA") or cto not in ("WORLD", "OBJECT", "CAMERA"):
            raise _UnsupportedGraph(f"unsupported VectorTransform ({vtype}, {cfrom} -> {cto})")

        vec, vd = self.eval_input(_input_by_name(node, "Vector"), "vec3", depth, missing_default=(0.0, 0.0, 0.0))
        if cfrom == cto:
            # Matches Cycles svm_node_vector_transform: no-op when from == to.
            return self._emit_result(key, vec, "vec3", vd)

        expr = vec
        ed = vd
        # Step 1: convert to WORLD if starting in OBJECT or CAMERA
        if cfrom == "OBJECT":
            expr = f"osl_to_world_{kind}({expr})"
            ed += 1
        elif cfrom == "CAMERA":
            expr = f"osl_camera_to_world_{kind}({expr})"
            ed += 1
        # Step 2: convert from WORLD to target if target is OBJECT or CAMERA
        if cto == "OBJECT":
            expr = f"osl_to_object_{kind}({expr})"
            ed += 1
        elif cto == "CAMERA":
            expr = f"osl_to_camera_{kind}({expr})"
            ed += 1
        return self._emit_result(key, expr, "vec3", ed)

    def _compile_tex_image(
        self, node, output_name: str, depth: int, key: tuple
    ) -> tuple[str, str, int]:
        if output_name not in ("Color", "Alpha"):
            raise _UnsupportedGraph(f"unsupported TexImage output {output_name}")
        proj = getattr(node, "projection", "FLAT")
        if proj != "FLAT":
            raise _UnsupportedGraph(f"unsupported TexImage projection {proj}")
        ext = getattr(node, "extension", "REPEAT")
        if ext not in SUPPORTED_TEX_IMAGE_EXTENSIONS:
            raise _UnsupportedGraph(f"unsupported TexImage extension {ext}")
        if self._image_resolver is None:
            raise _UnsupportedGraph("TexImage requires image_resolver")
        pnt_name = self._image_resolver(node)
        if not pnt_name or not _is_valid_rise_ident(str(pnt_name)):
            raise _UnsupportedGraph("TexImage could not resolve to a valid RISE painter identifier")
        pnt_name = str(pnt_name)

        is_alpha = (output_name == "Alpha")
        ret_type = "scalar" if is_alpha else "vec3"
        vec_sock = _input_by_name(node, "Vector")

        # Fast mip-filtered path when reading the mesh's primary UV directly.
        if _is_primary_uv_vector_socket(vec_sock):
            base_call = f"sample_alpha({pnt_name})" if is_alpha else f"sample({pnt_name})"
            if ext == "CLIP":
                in_bounds = "((u >= 0) * (u <= 1) * (v >= 0) * (v <= 1))"
                return self._emit_result(key, f"({base_call} * {in_bounds})", ret_type, 3)
            return self._emit_result(key, base_call, ret_type, 1)

        vec_expr, vd = self.eval_input(vec_sock, "vec3", depth)
        uv_var = self._bind_if_needed_for_multi_ref((vec_expr, "vec3", vd))
        uv_d = 1 if uv_var != vec_expr else (vd + 1)
        if ext == "CLIP":
            in_bounds = f"(({uv_var}.x >= 0) * ({uv_var}.x <= 1) * ({uv_var}.y >= 0) * ({uv_var}.y <= 1))"
            u_clamped = f"clamp({uv_var}.x, 0, 1)"
            v_clamped = f"clamp({uv_var}.y, 0, 1)"
            fn = "sample_uv_alpha" if is_alpha else "sample_uv"
            call = f"({fn}({pnt_name}, {u_clamped}, {v_clamped}) * {in_bounds})"
            return self._emit_result(key, call, ret_type, uv_d + 3)

        fn = "sample_uv_alpha" if is_alpha else "sample_uv"
        call = f"{fn}({pnt_name}, {uv_var}.x, {uv_var}.y)"
        return self._emit_result(key, call, ret_type, uv_d + 1)

    def _compile_rgb_curve(
        self, node, output_name: str, depth: int, key: tuple
    ) -> tuple[str, str, int]:
        """Compile ``ShaderNodeRGBCurve`` matching Cycles ``svm_node_curves``
        (`R(C(t)), G(C(t)), B(C(t))` over `[min_x, max_x]`, with optional
        linear extrapolation and `Fac` blend)."""
        if output_name != "Color":
            raise _UnsupportedGraph(f"unsupported RGBCurve output {output_name}")
        mapping = getattr(node, "mapping", None)
        curves = list(getattr(mapping, "curves", ())) if mapping is not None else []
        if len(curves) < 4:
            raise _UnsupportedGraph("RGBCurve missing 4 CurveMap channels (R, G, B, C)")

        update_fn = getattr(mapping, "update", None)
        if callable(update_fn):
            try:
                update_fn()
            except (AttributeError, RuntimeError):
                pass
        init_fn = getattr(mapping, "initialize", None)
        if callable(init_fn):
            try:
                init_fn()
            except (AttributeError, RuntimeError):
                pass
        eval_fn = getattr(mapping, "evaluate", None)
        use_native_eval = callable(eval_fn)

        min_x, max_x = _curvemapping_minmax(mapping, 4)
        range_x = max_x - min_x
        if not math.isfinite(range_x) or range_x <= 1e-9:
            raise _UnsupportedGraph("RGBCurve degenerate domain range")

        extrapolate = (getattr(mapping, "extend", "EXTRAPOLATED") == "EXTRAPOLATED")
        n_steps = 63  # 64 stops (exact ExpressionProgram::kMaxRampStops ceiling)
        ts = [min_x + (float(i) / float(n_steps)) * range_x for i in range(n_steps + 1)]

        def _eval_one(curve, x: float) -> float:
            if use_native_eval:
                try:
                    return float(eval_fn(curve, x))
                except (AttributeError, RuntimeError, TypeError):
                    pass
            return _eval_curve_points_fallback(curve, x)

        c_curve = curves[3]
        ys_per_ch: list[list[float]] = [[], [], []]
        for t in ts:
            ct = _eval_one(c_curve, t)
            for ch in range(3):
                val = _eval_one(curves[ch], ct)
                ys_per_ch[ch].append(val if math.isfinite(val) else 0.0)

        col, cd = self.eval_input(_input_by_name(node, "Color"), "vec3", depth, missing_default=(0.0, 0.0, 0.0))
        fac_sock = _input_by_names(node, ("Fac", "Factor"))
        fac_linked = bool(_live_links(fac_sock))
        fac_default = _default_scalar(fac_sock, 1.0)
        if not fac_linked and abs(fac_default) <= 1e-6:
            return self._emit_result(key, col, "vec3", cd)

        cvar = self._bind_if_needed_for_multi_ref((col, "vec3", cd))
        comp_names = ("x", "y", "z")
        ch_exprs: list[str] = []
        max_ch_d = 1
        inv_range = 1.0 / range_x
        dt = range_x / float(n_steps)

        for ch in range(3):
            ys = ys_per_ch[ch]
            x_expr = f"{cvar}.{comp_names[ch]}"
            if all(abs(ys[i] - ts[i]) <= 1e-5 for i in range(len(ts))):
                if extrapolate:
                    ch_exprs.append(x_expr)
                else:
                    ch_exprs.append(f"clamp({x_expr}, {_fmt(min_x)}, {_fmt(max_x)})")
                    max_ch_d = max(max_ch_d, 2)
                continue
            y0, yN = ys[0], ys[-1]
            # Straight-line fast path (covers ~90% of 2-point curves in scenes)
            is_linear = all(
                abs(ys[i] - (y0 + (yN - y0) * (float(i) / float(n_steps)))) <= 1e-4
                for i in range(len(ts))
            )
            if is_linear:
                if abs(yN - y0) <= 1e-7:
                    ch_exprs.append(_fmt(y0))
                elif extrapolate:
                    slope = (yN - y0) * inv_range
                    ch_exprs.append(f"(({x_expr} - {_fmt(min_x)}) * {_fmt(slope)} + {_fmt(y0)})")
                    max_ch_d = max(max_ch_d, 3)
                else:
                    u_expr = f"clamp(({x_expr} - {_fmt(min_x)}) * {_fmt(inv_range)}, 0, 1)"
                    ch_exprs.append(f"mix({_fmt(y0)}, {_fmt(yN)}, {u_expr})")
                    max_ch_d = max(max_ch_d, 4)
                continue

            # General spline curve via 64-stop piecewise-linear RISE ramp()
            stops = ", ".join(f"{_fmt(ts[i])}, {_fmt(ys[i])}" for i in range(len(ts)))
            ramp_expr = f"ramp({x_expr}, {stops})"
            if extrapolate:
                slope_lo = (ys[1] - ys[0]) / dt
                slope_hi = (ys[-1] - ys[-2]) / dt
                lo_expr = f"({_fmt(y0)} + {_fmt(slope_lo)} * ({x_expr} - {_fmt(min_x)}))"
                hi_expr = f"({_fmt(yN)} + {_fmt(slope_hi)} * ({x_expr} - {_fmt(max_x)}))"
                full_ch = f"select(({x_expr} < {_fmt(min_x)}), {lo_expr}, select(({x_expr} > {_fmt(max_x)}), {hi_expr}, {ramp_expr}))"
                ch_var, _, _ = self._bind(full_ch, "scalar")
                ch_exprs.append(ch_var)
                max_ch_d = max(max_ch_d, 4)
            else:
                ch_var, _, _ = self._bind(ramp_expr, "scalar")
                ch_exprs.append(ch_var)
                max_ch_d = max(max_ch_d, 2)

        curved = f"vec3({ch_exprs[0]}, {ch_exprs[1]}, {ch_exprs[2]})"
        if not fac_linked and abs(fac_default - 1.0) <= 1e-6:
            return self._emit_result(key, curved, "vec3", max_ch_d + 1)
        fac_expr, fd = self.eval_input(fac_sock, "scalar", depth, missing_default=1.0)
        return self._emit_result(key, f"mix({cvar}, {curved}, {fac_expr})", "vec3", max(max_ch_d + 1, fd) + 1)

    def _compile_hue_sat(self, node, depth: int, key: tuple[int, str]) -> tuple[str, str, int]:
        hue, hd = self.eval_input(_input_by_name(node, "Hue"), "scalar", depth)
        sat, sd = self.eval_input(_input_by_name(node, "Saturation"), "scalar", depth)
        val, vd = self.eval_input(_input_by_name(node, "Value"), "scalar", depth)
        fac, fd = self.eval_input(_input_by_names(node, ("Fac", "Factor")), "scalar", depth)
        col, cd = self.eval_input(_input_by_name(node, "Color"), "vec3", depth)
        cvar = self._bind_if_needed_for_multi_ref((col, "vec3", cd))
        hvar, _, _ = self._bind(f"osl_rgb2hsv({cvar})", "vec3")
        h_adj = f"frac({hvar}.x + ({hue}) + 0.5)"
        s_adj = f"clamp({hvar}.y * ({sat}), 0, 1)"
        v_adj = f"max({hvar}.z * ({val}), 0)"
        rgb_expr = f"osl_hsv2rgb(vec3({h_adj}, {s_adj}, {v_adj}))"
        mix_var, _, _ = self._bind(f"mix({cvar}, {rgb_expr}, {fac})", "vec3")
        return self._emit_result(
            key,
            f"vec3(max({mix_var}.x, 0), max({mix_var}.y, 0), max({mix_var}.z, 0))",
            "vec3",
            max(hd, sd, vd, fd) + 3,
        )

    def _compile_color_ramp(
        self, node, output_name: str, depth: int, key: tuple[int, str]
    ) -> tuple[str, str, int]:
        if output_name not in ("Color", "Alpha"):
            raise _UnsupportedGraph(f"ColorRamp output {output_name}")
        ramp = getattr(node, "color_ramp", None)
        if ramp is None:
            raise _UnsupportedGraph("ColorRamp missing color_ramp")
        color_mode = getattr(ramp, "color_mode", "RGB")
        if color_mode not in ("RGB", "HSV", "HSL"):
            raise _UnsupportedGraph(f"ColorRamp color_mode {color_mode}")
        interp = getattr(ramp, "interpolation", "LINEAR")
        # Blender forces LINEAR interpolation for HSV/HSL modes (colorband.cc:415).
        eff_interp = interp if color_mode == "RGB" else "LINEAR"
        if eff_interp not in ("LINEAR", "CONSTANT", "EASE"):
            raise _UnsupportedGraph(f"ColorRamp interpolation {eff_interp}")
        hue_interp = getattr(ramp, "hue_interpolation", "NEAR")

        elems = sorted(
            [
                (
                    float(e.position),
                    (float(e.color[0]), float(e.color[1]), float(e.color[2])),
                    float(e.color[3]) if len(e.color) >= 4 else 1.0,
                )
                for e in ramp.elements
            ],
            key=lambda item: item[0],
        )
        if not elems:
            raise _UnsupportedGraph("ColorRamp empty")

        fac, fd = self.eval_input(_input_by_names(node, ("Fac", "Factor")), "scalar", depth)
        tvar, _, _ = self._bind(f"clamp({fac}, 0, 1)", "scalar")

        def _seg_t(p0: float, p1: float) -> str:
            inv_span = _fmt(1.0 / (p1 - p0))
            lin = f"clamp(({tvar} - {_fmt(p0)}) * {inv_span}, 0, 1)"
            if eff_interp == "EASE":
                return f"smoothstep(0, 1, {lin})"
            return lin

        _RAMP_CHUNK = 16

        if output_name == "Alpha":
            expr = _fmt(elems[-1][2])
            chunk_d = 1
            for idx in range(len(elems) - 2, -1, -1):
                p0, _, a0 = elems[idx]
                p1, _, a1 = elems[idx + 1]
                if eff_interp == "CONSTANT" or (p1 - p0) <= 1e-12:
                    seg = _fmt(a0)
                else:
                    seg = f"mix({_fmt(a0)}, {_fmt(a1)}, {_seg_t(p0, p1)})"
                expr = f"mix({expr}, {seg}, ({tvar} < {_fmt(p1)}))"
                chunk_d += 1
                if chunk_d >= _RAMP_CHUNK and idx > 0:
                    self._record_depth(chunk_d + 2)
                    expr, _, _ = self._bind(expr, "scalar")
                    chunk_d = 1
            first_p, _, first_a = elems[0]
            if first_p > 0.0:
                expr = f"mix({expr}, {_fmt(first_a)}, ({tvar} <= {_fmt(first_p)}))"
                chunk_d += 1
            return self._emit_result(key, expr, "scalar", chunk_d + 2)

        if color_mode == "RGB":
            expr = _fmt_vec3(elems[-1][1])
            chunk_d = 1
            for idx in range(len(elems) - 2, -1, -1):
                p0, c0, _ = elems[idx]
                p1, c1, _ = elems[idx + 1]
                c0_s = _fmt_vec3(c0)
                c1_s = _fmt_vec3(c1)
                if eff_interp == "CONSTANT" or (p1 - p0) <= 1e-12:
                    seg = c0_s
                else:
                    seg = f"mix({c0_s}, {c1_s}, {_seg_t(p0, p1)})"
                expr = f"mix({expr}, {seg}, ({tvar} < {_fmt(p1)}))"
                chunk_d += 1
                if chunk_d >= _RAMP_CHUNK and idx > 0:
                    self._record_depth(chunk_d + 2)
                    expr, _, _ = self._bind(expr, "vec3")
                    chunk_d = 1
            first_p, first_c, _ = elems[0]
            if first_p > 0.0:
                expr = f"mix({expr}, {_fmt_vec3(first_c)}, ({tvar} <= {_fmt(first_p)}))"
                chunk_d += 1
            return self._emit_result(key, expr, "vec3", chunk_d + 2)

        # HSV / HSL interpolation matching Blender's colorband.cc
        to_space = _rgb_to_hsv_py if color_mode == "HSV" else _rgb_to_hsl_py
        back_fn = "osl_hsv2rgb" if color_mode == "HSV" else "osl_hsl2rgb"
        space_stops = [(p, c, to_space(c[0], c[1], c[2])) for p, c, _ in elems]
        expr = _fmt_vec3(elems[-1][1])
        chunk_d = 1
        for idx in range(len(space_stops) - 2, -1, -1):
            p0, c0_rgb, s0 = space_stops[idx]
            p1, _, s1 = space_stops[idx + 1]
            if (p1 - p0) <= 1e-12:
                seg = _fmt_vec3(c0_rgb)
            else:
                ft = _seg_t(p0, p1)
                fvar, _, _ = self._bind(ft, "scalar")
                hmode = _colorband_hue_interp_mode(hue_interp, s0[0], s1[0])
                if hmode == 0:
                    h_expr = f"mix({_fmt(s0[0])}, {_fmt(s1[0])}, {fvar})"
                elif hmode == 1:
                    h_expr = f"frac(mix({_fmt(s0[0] + 1.0)}, {_fmt(s1[0])}, {fvar}))"
                else:
                    h_expr = f"frac(mix({_fmt(s0[0])}, {_fmt(s1[0] + 1.0)}, {fvar}))"
                c2_expr = f"mix({_fmt(s0[1])}, {_fmt(s1[1])}, {fvar})"
                c3_expr = f"mix({_fmt(s0[2])}, {_fmt(s1[2])}, {fvar})"
                seg = f"{back_fn}(vec3({h_expr}, {c2_expr}, {c3_expr}))"
            expr = f"mix({expr}, {seg}, ({tvar} < {_fmt(p1)}))"
            chunk_d += 1
            if chunk_d >= _RAMP_CHUNK and idx > 0:
                self._record_depth(chunk_d + 3)
                expr, _, _ = self._bind(expr, "vec3")
                chunk_d = 1
        first_p, first_c, _ = elems[0]
        if first_p > 0.0:
            expr = f"mix({expr}, {_fmt_vec3(first_c)}, ({tvar} <= {_fmt(first_p)}))"
            chunk_d += 1
        return self._emit_result(key, expr, "vec3", chunk_d + 3)

    def finalize(self, final_expr: str) -> CompiledExpression:
        lines = [f"def {name} {body}" for name, body in self._defs]
        lines.append(final_expr)
        return CompiledExpression(
            expression="\n".join(lines),
            uses_generated=self.uses_generated,
            def_count=len(self._defs),
            def_slots=self._def_slots,
            max_expr_depth=self.max_expr_depth,
            mesh_attributes=tuple(sorted(self._mesh_attributes)),
        )


def compile_socket_expression(
    socket,
    *,
    want_type: str = "vec3",
    bbox_min=None,
    bbox_max=None,
    scale: tuple[float, float, float] | float | None = None,
    inline_single_use: bool = True,
    material_pass_index: float = 0.0,
    attr_types: dict[str, str] | None = None,
    image_resolver=None,
) -> CompiledExpression | None:
    """Compile a linked Blender shader input socket into a RISE expression.

    Returns ``CompiledExpression`` on success, or ``None`` if the socket is
    unlinked or any upstream node is not faithfully supported.
    """
    links = _live_links(socket)
    if not links:
        return None
    compiler = _Compiler(
        bbox_min=bbox_min,
        bbox_max=bbox_max,
        inline_single_use=inline_single_use,
        material_pass_index=material_pass_index,
        attr_types=attr_types,
        image_resolver=image_resolver,
    )
    try:
        link = links[0]
        topo = compiler.prepass_count_uses([link])
        compiler.evaluate_topo(topo)
        src_node, src_sock = _resolve_reroute(link.from_node, link.from_socket)
        if src_node is None or src_sock is None:
            return None
        val = compiler.eval_node_output(src_node, _canonical_output_name(src_node, src_sock), 0)
        expr, _ = compiler._coerce(val, want_type, getattr(src_sock, "type", ""))
        if scale is not None:
            if want_type == "vec3":
                if isinstance(scale, (int, float)):
                    s = float(scale)
                    if abs(s - 1.0) > 1e-12:
                        expr = f"(({expr}) * {_fmt(s)})"
                else:
                    sx, sy, sz = float(scale[0]), float(scale[1]), float(scale[2])
                    if any(abs(c - 1.0) > 1e-12 for c in (sx, sy, sz)):
                        expr = f"(({expr}) * {_fmt_vec3((sx, sy, sz))})"
            elif want_type == "scalar":
                if hasattr(scale, "__len__") and len(scale) >= 3:
                    sx, sy, sz = float(scale[0]), float(scale[1]), float(scale[2])
                    expr = f"(({expr}) * {_fmt_vec3((sx, sy, sz))})"
                else:
                    s = float(scale[0] if hasattr(scale, "__len__") else scale)
                    if abs(s - 1.0) > 1e-12:
                        expr = f"(({expr}) * {_fmt(s)})"
        return compiler.finalize(expr)
    except _UnsupportedGraph:
        return None


def compile_socket_product_expression(
    color_socket,
    scalar_socket,
    *,
    bbox_min=None,
    bbox_max=None,
    inline_single_use: bool = True,
    material_pass_index: float = 0.0,
    attr_types: dict[str, str] | None = None,
    image_resolver=None,
) -> CompiledExpression | None:
    """Compile two linked sockets into ``((color_expr) * (scalar_expr))``.

    Used when both ``Emission Color`` and ``Emission Strength`` are driven by
    procedural subgraphs so both share one DAG binding table and neither
    silently drops the other.
    """
    color_links = _live_links(color_socket)
    scalar_links = _live_links(scalar_socket)
    if not color_links or not scalar_links:
        return None
    compiler = _Compiler(
        bbox_min=bbox_min,
        bbox_max=bbox_max,
        inline_single_use=inline_single_use,
        material_pass_index=material_pass_index,
        attr_types=attr_types,
        image_resolver=image_resolver,
    )
    try:
        topo = compiler.prepass_count_uses([color_links[0], scalar_links[0]])
        compiler.evaluate_topo(topo)
        c_node, c_sock = _resolve_reroute(color_links[0].from_node, color_links[0].from_socket)
        s_node, s_sock = _resolve_reroute(scalar_links[0].from_node, scalar_links[0].from_socket)
        if c_node is None or c_sock is None or s_node is None or s_sock is None:
            return None
        c_val = compiler.eval_node_output(c_node, _canonical_output_name(c_node, c_sock), 0)
        c_expr, _ = compiler._coerce(c_val, "vec3", getattr(c_sock, "type", ""))
        s_val = compiler.eval_node_output(s_node, _canonical_output_name(s_node, s_sock), 0)
        s_expr, _ = compiler._coerce(s_val, "scalar", getattr(s_sock, "type", ""))
        return compiler.finalize(f"(({c_expr}) * ({s_expr}))")
    except _UnsupportedGraph:
        return None


def _emit_factor_chain(
    compiler: _Compiler,
    factors: list[tuple[object | None, float, bool]],
) -> tuple[str, int]:
    """Emit a single root-to-leaf MixShader factor product inside ``compiler``,
    splitting long products via ``_bind`` to stay well below kMaxParseDepth."""
    const_scale = 1.0
    terms: list[tuple[str, int]] = []
    for sock, default_val, is_second in factors:
        links = _live_links(sock)
        if not links:
            f = _clamp_unit_default(default_val)
            const_scale *= f if is_second else (1.0 - f)
            if const_scale <= 0.0:
                return ("0", 0)
            continue
        src_node, src_sock = _resolve_reroute(links[0].from_node, links[0].from_socket)
        if src_node is None or src_sock is None:
            raise _UnsupportedGraph("unlinked reroute in mix factor")
        val = compiler.eval_node_output(src_node, _canonical_output_name(src_node, src_sock), 0)
        f_expr, fd = compiler._coerce(val, "scalar", getattr(src_sock, "type", ""))
        clamped = f"clamp({f_expr}, 0, 1)"
        td = fd + 1
        if not is_second:
            clamped = f"(1 - {clamped})"
            td += 1
        terms.append((clamped, td))

    if not terms:
        return (_fmt(const_scale), 0)

    expr, cur_d = terms[0]
    for t_expr, t_d in terms[1:]:
        next_d = max(cur_d, t_d) + 1
        if next_d >= compiler._inline_split_depth:
            expr, _, cur_d = compiler._bind(expr, "scalar")
            next_d = max(cur_d, t_d) + 1
        expr = f"({expr} * {t_expr})"
        cur_d = next_d
    if abs(const_scale - 1.0) > 1e-12:
        expr = f"({expr} * {_fmt(const_scale)})"
        cur_d += 1
    compiler._record_depth(cur_d)
    return (expr, cur_d)


def compile_mix_weight_expression(
    factors: list[tuple[object | None, float, bool]],
    *,
    bbox_min=None,
    bbox_max=None,
    inline_single_use: bool = True,
    material_pass_index: float = 0.0,
    attr_types: dict[str, str] | None = None,
    image_resolver=None,
) -> CompiledExpression | None:
    """Compile a root-to-leaf chain of ``ShaderNodeMixShader`` factors into a
    single bounded scalar weight expression.

    Each element of ``factors`` is ``(fac_socket, fac_default, is_second_branch)``:
      * ``is_second_branch == False`` contributes ``(1 - clamp(f, 0, 1))``
      * ``is_second_branch == True``  contributes ``clamp(f, 0, 1)``

    Returns ``CompiledExpression`` on success, or ``None`` if any linked factor
    subgraph is not faithfully supported.
    """
    if not factors:
        return CompiledExpression(expression="1", uses_generated=False)

    # Check constant factors first: an unlinked 0 branch zeroes the entire product.
    const_scale = 1.0
    root_links = []
    for sock, default_val, is_second in factors:
        links = _live_links(sock)
        if links:
            root_links.append(links[0])
        else:
            f = _clamp_unit_default(default_val)
            const_scale *= f if is_second else (1.0 - f)

    if const_scale <= 0.0:
        return CompiledExpression(expression="0", uses_generated=False)
    if not root_links:
        return CompiledExpression(expression=_fmt(const_scale), uses_generated=False)

    compiler = _Compiler(
        bbox_min=bbox_min,
        bbox_max=bbox_max,
        inline_single_use=inline_single_use,
        material_pass_index=material_pass_index,
        attr_types=attr_types,
        image_resolver=image_resolver,
    )
    try:
        topo = compiler.prepass_count_uses(root_links)
        compiler.evaluate_topo(topo)
        expr, _ = _emit_factor_chain(compiler, factors)
        return compiler.finalize(expr)
    except _UnsupportedGraph:
        return None


def compile_mix_passthrough_expression(
    factor_lists: list[list[tuple[object | None, float, bool]]],
    *,
    bbox_min=None,
    bbox_max=None,
    inline_single_use: bool = True,
    material_pass_index: float = 0.0,
    attr_types: dict[str, str] | None = None,
    image_resolver=None,
) -> CompiledExpression | None:
    """Compile the sum of one or more ``Transparent BSDF`` leaf factor chains in
    a single shared ``_Compiler`` DAG (sharing ancestor nodes / ``osl_ao`` calls
    and enforcing the 495-slot VM budget without string regex rewriting)."""
    if not factor_lists:
        return CompiledExpression(expression="0", uses_generated=False)
    if len(factor_lists) == 1:
        return compile_mix_weight_expression(
            factor_lists[0],
            bbox_min=bbox_min,
            bbox_max=bbox_max,
            inline_single_use=inline_single_use,
            material_pass_index=material_pass_index,
            attr_types=attr_types,
            image_resolver=image_resolver,
        )

    active_chains: list[list[tuple[object | None, float, bool]]] = []
    const_sum = 0.0
    root_links = []
    for factors in factor_lists:
        c_scale = 1.0
        chain_links = []
        for sock, default_val, is_second in factors:
            links = _live_links(sock)
            if links:
                chain_links.append(links[0])
            else:
                f = _clamp_unit_default(default_val)
                c_scale *= f if is_second else (1.0 - f)
        if c_scale <= 0.0:
            continue
        if not chain_links:
            const_sum += c_scale
        else:
            active_chains.append(factors)
            root_links.extend(chain_links)

    if not active_chains:
        return CompiledExpression(expression=_fmt(min(1.0, const_sum)), uses_generated=False)

    compiler = _Compiler(
        bbox_min=bbox_min,
        bbox_max=bbox_max,
        inline_single_use=inline_single_use,
        material_pass_index=material_pass_index,
        attr_types=attr_types,
        image_resolver=image_resolver,
    )
    try:
        topo = compiler.prepass_count_uses(root_links)
        compiler.evaluate_topo(topo)
        terms: list[tuple[str, int]] = []
        if const_sum > 0.0:
            terms.append((_fmt(const_sum), 0))
        for factors in active_chains:
            t_expr, t_d = _emit_factor_chain(compiler, factors)
            if t_expr != "0":
                terms.append((t_expr, t_d))
        if not terms:
            return CompiledExpression(expression="0", uses_generated=False)
        expr, cur_d = terms[0]
        for t_expr, t_d in terms[1:]:
            next_d = max(cur_d, t_d) + 1
            if next_d >= compiler._inline_split_depth:
                expr, _, cur_d = compiler._bind(expr, "scalar")
                next_d = max(cur_d, t_d) + 1
            expr = f"({expr} + {t_expr})"
            cur_d = next_d
        compiler._record_depth(cur_d)
        return compiler.finalize(expr)
    except _UnsupportedGraph:
        return None


def compile_bump_height_expression(
    bump_node,
    *,
    bbox_min=None,
    bbox_max=None,
    inline_single_use: bool = True,
    material_pass_index: float = 0.0,
    attr_types: dict[str, str] | None = None,
    image_resolver=None,
) -> CompiledExpression | None:
    """Compile a ``ShaderNodeBump`` node's ``Height`` input as a scalar expression."""
    if bump_node is None or getattr(bump_node, "bl_idname", "") != "ShaderNodeBump":
        return None
    height_sock = _input_by_name(bump_node, "Height")
    if height_sock is None or not _live_links(height_sock):
        return None
    return compile_socket_expression(
        height_sock,
        want_type="scalar",
        bbox_min=bbox_min,
        bbox_max=bbox_max,
        inline_single_use=inline_single_use,
        material_pass_index=material_pass_index,
        attr_types=attr_types,
        image_resolver=image_resolver,
    )
