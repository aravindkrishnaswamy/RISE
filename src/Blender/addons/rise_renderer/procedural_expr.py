"""Compile Blender Cycles procedural shader subgraphs into RISE expressions.

Targets RISE's `expression_painter` VM (`ExpressionEval.h` +
`ExpressionOslBuiltins.h`), which implements exact IEEE-754 single-precision
ports of Cycles' Voronoi, Noise Texture, White Noise, RGB/HSV/HSL conversions,
and Mix Color kernels (`osl_voronoi_*`, `osl_noise_texture*`,
`osl_white_noise*`, `osl_rgb2hsv`, `osl_hsv2rgb`, `osl_rgb2hsl`, `osl_hsl2rgb`,
`osl_mix_color`).

Design rules:
  * Fail closed: if ANY node, output socket, or mode in the upstream subgraph
    is not faithfully supported, return ``None`` rather than silently dropping
    part of the graph.
  * Multi-line output uses ``def <var> <expr>`` bindings (parsed by
    ``rise_blender_bridge.cpp`` into ``IJob::AddExpressionPainter``'s ``defs``
    array) followed by the final expression line, so shared/diamond nodes and
    multi-channel intermediates evaluate once per shading point.
  * Pure Python with no top-level ``bpy`` import so unit tests can run both
    outside and inside Blender.
"""

from __future__ import annotations

from dataclasses import dataclass
import math

from .blender_compat import rna_key


@dataclass(frozen=True)
class CompiledExpression:
    """Compiled RISE expression_painter source plus metadata."""

    expression: str
    uses_generated: bool


# Cycles enum tables matching OslVoronoi.h, OslNoise.h, OslColor.h.
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


def _live_links(socket):
    if socket is None or not getattr(socket, "is_linked", False):
        return []
    return [link for link in socket.links if not getattr(link, "is_muted", False)]


def _input_by_name(node, name: str, socket_type: str | None = None):
    for inp in getattr(node, "inputs", ()):
        if inp.name != name:
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


class _UnsupportedGraph(Exception):
    """Internal signal to abort compilation cleanly on an unsupported node."""


class _Compiler:
    # ExpressionProgram::kMaxSlots (512) minus kContextSlotCount (16) and
    # the auto-registered `seed` parameter (1) in Job::AddExpressionPainter.
    _MAX_DEF_SLOTS = 512 - 17

    def __init__(self, bbox_min, bbox_max, max_depth: int = 32):
        self._bbox_min = bbox_min
        self._bbox_max = bbox_max
        self._max_depth = max_depth
        self._defs: list[tuple[str, str]] = []
        self._def_slots = 0
        self._cache: dict[tuple[int, str], tuple[str, str]] = {}
        self._active: set[tuple[int, str]] = set()
        self.uses_generated = False

    def _bind(self, expr: str, vtype: str) -> tuple[str, str]:
        slots = 3 if vtype == "vec3" else 1
        if self._def_slots + slots > self._MAX_DEF_SLOTS:
            raise _UnsupportedGraph("expression slot budget exceeded")
        name = f"_v{len(self._defs)}"
        self._defs.append((name, expr))
        self._def_slots += slots
        return (name, vtype)

    def _generated_coord(self) -> tuple[str, str]:
        self.uses_generated = True
        if self._bbox_min is not None and self._bbox_max is not None:
            bmin = self._bbox_min
            bmax = self._bbox_max
            inv = [
                (1.0 / (bmax[i] - bmin[i])) if (bmax[i] - bmin[i]) > 1e-12 else 0.0
                for i in range(3)
            ]
            # Cycles Generated: (Po - bbox_min) / (bbox_max - bbox_min),
            # 0 along any degenerate axis where max == min.
            if all(abs(bmin[i]) <= 1e-12 and abs(inv[i] - 1.0) <= 1e-12 for i in range(3)):
                return ("Po", "vec3")
            return self._bind(
                f"((Po - {_fmt_vec3(bmin)}) * {_fmt_vec3(inv)})",
                "vec3",
            )
        return ("Po", "vec3")

    def _coerce(self, val: tuple[str, str], want_type: str, source_socket_type: str = "") -> str:
        expr, have_type = val
        if have_type == want_type:
            return expr
        if want_type == "vec3" and have_type == "scalar":
            return f"vec3({expr}, {expr}, {expr})"
        if want_type == "scalar" and have_type == "vec3":
            # Cycles converts Color -> Float by Rec.709 luminance, and
            # Vector -> Float by channel average.
            if source_socket_type == "VECTOR":
                return f"(({expr}.x + {expr}.y + {expr}.z) * 0.3333333333333333)"
            return f"dot({expr}, vec3(0.2126, 0.7152, 0.0722))"
        raise _UnsupportedGraph(f"cannot coerce {have_type} -> {want_type}")

    def eval_input(
        self,
        socket,
        want_type: str,
        depth: int,
        default_override=None,
        missing_default=None,
    ) -> str:
        if socket is None:
            if default_override is not None:
                return default_override
            if missing_default is not None:
                if want_type == "scalar":
                    return _fmt(float(missing_default))
                if want_type == "vec3":
                    return _fmt_vec3(missing_default)
            raise _UnsupportedGraph("missing required input socket")
        links = _live_links(socket)
        if links:
            link = links[0]
            from_node = link.from_node
            from_socket = link.from_socket
            val = self.eval_node_output(from_node, from_socket.name, depth + 1)
            return self._coerce(val, want_type, getattr(from_socket, "type", ""))
        if default_override is not None:
            return default_override
        if want_type == "scalar":
            return _fmt(_default_scalar(socket, 0.0))
        if want_type == "vec3":
            return _fmt_vec3(_default_vec3(socket, (0.0, 0.0, 0.0)))
        raise _UnsupportedGraph(f"unknown want_type {want_type}")

    def eval_node_output(self, node, output_name: str, depth: int) -> tuple[str, str]:
        if node is None or depth > self._max_depth:
            raise _UnsupportedGraph("null node or max depth exceeded")

        key = (rna_key(node), output_name)
        if key in self._cache:
            return self._cache[key]
        if key in self._active:
            raise _UnsupportedGraph("cycle detected")
        self._active.add(key)
        try:
            result = self._compile_node_output(node, output_name, depth)
        finally:
            self._active.discard(key)
        self._cache[key] = result
        return result

    def _compile_node_output(self, node, output_name: str, depth: int) -> tuple[str, str]:
        bl = getattr(node, "bl_idname", "")

        if bl == "NodeReroute":
            inp = node.inputs[0] if node.inputs else None
            links = _live_links(inp)
            if not links:
                raise _UnsupportedGraph("unlinked reroute")
            return self.eval_node_output(links[0].from_node, links[0].from_socket.name, depth + 1)

        if getattr(node, "mute", False):
            # Fail closed on muted nodes so we never misinterpret Blender's
            # internal pass-through rules.
            raise _UnsupportedGraph(f"muted node {bl}")

        if bl == "ShaderNodeValue":
            out = node.outputs[0] if node.outputs else None
            return (_fmt(_default_scalar(out, 0.0)), "scalar")

        if bl == "ShaderNodeRGB":
            out = node.outputs[0] if node.outputs else None
            return (_fmt_vec3(_default_vec3(out, (0.5, 0.5, 0.5))), "vec3")

        if bl == "ShaderNodeTexCoord":
            if getattr(node, "from_instancer", False):
                raise _UnsupportedGraph("TexCoord.from_instancer not supported")
            if getattr(node, "object", None) is not None and output_name == "Object":
                raise _UnsupportedGraph("TexCoord.Object with external object reference")
            if output_name == "Generated":
                return self._generated_coord()
            if output_name == "Object":
                return ("Po", "vec3")
            if output_name == "UV":
                return ("vec3(u, v, 0)", "vec3")
            # Cycles TexCoord.Normal is object-space (inverse-transposed),
            # whereas RISE's `N` is world-space.  Fail closed until an
            # object-space normal transform builtin exists.
            raise _UnsupportedGraph(f"unsupported TexCoord output {output_name}")

        if bl == "ShaderNodeMapping":
            return self._compile_mapping(node, depth)

        if bl == "ShaderNodeTexVoronoi":
            return self._compile_voronoi(node, output_name, depth)

        if bl == "ShaderNodeTexNoise":
            return self._compile_noise(node, output_name, depth)

        if bl == "ShaderNodeTexWhiteNoise":
            return self._compile_white_noise(node, output_name, depth)

        if bl in ("ShaderNodeMix", "ShaderNodeMixRGB"):
            return self._compile_mix(node, output_name, depth)

        if bl == "ShaderNodeHueSaturation":
            return self._compile_hue_sat(node, depth)

        if bl == "ShaderNodeInvert":
            fac = self.eval_input(_input_by_name(node, "Fac"), "scalar", depth)
            col = self.eval_input(_input_by_name(node, "Color"), "vec3", depth)
            cvar, _ = self._bind(col, "vec3")
            return self._bind(f"mix({cvar}, (vec3(1, 1, 1) - {cvar}), {fac})", "vec3")

        if bl == "ShaderNodeBrightContrast":
            col = self.eval_input(_input_by_name(node, "Color"), "vec3", depth)
            bright = self.eval_input(_input_by_name(node, "Bright"), "scalar", depth)
            contrast = self.eval_input(_input_by_name(node, "Contrast"), "scalar", depth)
            cvar, _ = self._bind(col, "vec3")
            bvar, _ = self._bind(bright, "scalar")
            kvar, _ = self._bind(contrast, "scalar")
            a = f"(1 + {kvar})"
            b = f"({bvar} - 0.5 * {kvar})"
            return self._bind(
                f"vec3(max({a} * {cvar}.x + {b}, 0), max({a} * {cvar}.y + {b}, 0), max({a} * {cvar}.z + {b}, 0))",
                "vec3",
            )

        if bl == "ShaderNodeGamma":
            col = self.eval_input(_input_by_name(node, "Color"), "vec3", depth)
            gamma = self.eval_input(_input_by_name(node, "Gamma"), "scalar", depth)
            cvar, _ = self._bind(col, "vec3")
            gvar, _ = self._bind(gamma, "scalar")
            return self._bind(
                f"vec3(select({cvar}.x > 0, pow(max({cvar}.x, 0), {gvar}), {cvar}.x), "
                f"select({cvar}.y > 0, pow(max({cvar}.y, 0), {gvar}), {cvar}.y), "
                f"select({cvar}.z > 0, pow(max({cvar}.z, 0), {gvar}), {cvar}.z))",
                "vec3",
            )

        if bl == "ShaderNodeSeparateXYZ":
            comp = {"X": "x", "Y": "y", "Z": "z"}.get(output_name)
            if comp is None:
                raise _UnsupportedGraph(f"SeparateXYZ output {output_name}")
            shared_key = (rna_key(node), "__sepxyz")
            if shared_key in self._cache:
                vvar, _ = self._cache[shared_key]
            else:
                vec = self.eval_input(_input_by_name(node, "Vector"), "vec3", depth)
                vvar, _ = self._bind(vec, "vec3")
                self._cache[shared_key] = (vvar, "vec3")
            return (f"{vvar}.{comp}", "scalar")

        if bl == "ShaderNodeCombineXYZ":
            x = self.eval_input(_input_by_name(node, "X"), "scalar", depth)
            y = self.eval_input(_input_by_name(node, "Y"), "scalar", depth)
            z = self.eval_input(_input_by_name(node, "Z"), "scalar", depth)
            return self._bind(f"vec3({x}, {y}, {z})", "vec3")

        if bl == "ShaderNodeSeparateColor":
            mode = getattr(node, "mode", "RGB")
            # In Blender 3.3+/4.x/5.x, SeparateColor keeps internal socket
            # names "Red", "Green", "Blue" in every mode (HSV/HSL only alter
            # UI labels).  Accept both real Blender names and mode labels,
            # and share the rgb2hsv/rgb2hsl binding across outputs of the
            # same node.
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
            shared_key = (rna_key(node), f"__sepcol_{mode}")
            if shared_key in self._cache:
                vvar, _ = self._cache[shared_key]
            else:
                col = self.eval_input(_input_by_name(node, "Color"), "vec3", depth)
                if mode == "HSV":
                    vvar, _ = self._bind(f"osl_rgb2hsv({col})", "vec3")
                elif mode == "HSL":
                    vvar, _ = self._bind(f"osl_rgb2hsl({col})", "vec3")
                else:
                    vvar, _ = self._bind(col, "vec3")
                self._cache[shared_key] = (vvar, "vec3")
            return (f"{vvar}.{comp}", "scalar")

        if bl == "ShaderNodeCombineColor":
            mode = getattr(node, "mode", "RGB")
            # Like SeparateColor, Blender keeps CombineColor's internal socket
            # names as "Red", "Green", "Blue" in HSV/HSL modes.  Accept both
            # mode-specific labels and the real "Red"/"Green"/"Blue" names.
            if mode == "HSV":
                a = self.eval_input(_input_by_names(node, ("Hue", "Red")), "scalar", depth)
                b = self.eval_input(_input_by_names(node, ("Saturation", "Green")), "scalar", depth)
                c = self.eval_input(_input_by_names(node, ("Value", "Blue")), "scalar", depth)
                return self._bind(f"osl_hsv2rgb(vec3({a}, {b}, {c}))", "vec3")
            if mode == "HSL":
                a = self.eval_input(_input_by_names(node, ("Hue", "Red")), "scalar", depth)
                b = self.eval_input(_input_by_names(node, ("Saturation", "Green")), "scalar", depth)
                c = self.eval_input(_input_by_names(node, ("Lightness", "Blue")), "scalar", depth)
                return self._bind(f"osl_hsl2rgb(vec3({a}, {b}, {c}))", "vec3")
            if mode == "RGB":
                a = self.eval_input(_input_by_name(node, "Red"), "scalar", depth)
                b = self.eval_input(_input_by_name(node, "Green"), "scalar", depth)
                c = self.eval_input(_input_by_name(node, "Blue"), "scalar", depth)
                return self._bind(f"vec3({a}, {b}, {c})", "vec3")
            raise _UnsupportedGraph(f"CombineColor mode {mode}")

        if bl == "ShaderNodeValToRGB":
            return self._compile_color_ramp(node, output_name, depth)

        raise _UnsupportedGraph(f"unsupported node {bl}")

    def _compile_mapping(self, node, depth: int) -> tuple[str, str]:
        loc_sock = _input_by_name(node, "Location")
        rot_sock = _input_by_name(node, "Rotation")
        scl_sock = _input_by_name(node, "Scale")
        if _live_links(loc_sock) or _live_links(rot_sock) or _live_links(scl_sock):
            raise _UnsupportedGraph("Mapping with linked transform inputs")

        vec_sock = _input_by_name(node, "Vector")
        vec_expr = self.eval_input(vec_sock, "vec3", depth)
        vvar, _ = self._bind(vec_expr, "vec3")

        loc = _default_vec3(loc_sock, (0.0, 0.0, 0.0))
        rot = _default_vec3(rot_sock, (0.0, 0.0, 0.0))
        scl = _default_vec3(scl_sock, (1.0, 1.0, 1.0))
        rot_m = _euler_xyz_matrix(rot[0], rot[1], rot[2])
        vtype = getattr(node, "vector_type", "POINT")

        if vtype == "POINT":
            mat = _mat3_mul_diag(rot_m, scl[0], scl[1], scl[2])
            return self._bind(_emit_affine(vvar, mat, loc), "vec3")
        if vtype == "VECTOR":
            mat = _mat3_mul_diag(rot_m, scl[0], scl[1], scl[2])
            return self._bind(_emit_affine(vvar, mat), "vec3")
        if vtype == "TEXTURE":
            # Inverse of POINT: diag(1/scale) * R^T * (v - loc)
            inv_s = tuple((1.0 / s) if abs(s) > 1e-12 else 0.0 for s in scl)
            rt = _mat3_transpose(rot_m)
            mat = _diag_mul_mat3(inv_s[0], inv_s[1], inv_s[2], rt)
            trans = tuple(
                -(mat[r][0] * loc[0] + mat[r][1] * loc[1] + mat[r][2] * loc[2])
                for r in range(3)
            )
            return self._bind(_emit_affine(vvar, mat, trans), "vec3")
        if vtype == "NORMAL":
            inv_s = tuple((1.0 / s) if abs(s) > 1e-12 else 0.0 for s in scl)
            mat = _mat3_mul_diag(rot_m, inv_s[0], inv_s[1], inv_s[2])
            raw = _emit_affine(vvar, mat)
            return self._bind(f"normalize({raw})", "vec3")
        raise _UnsupportedGraph(f"Mapping vector_type {vtype}")

    def _compile_voronoi(self, node, output_name: str, depth: int) -> tuple[str, str]:
        dims = _DIMS_MAP.get(getattr(node, "voronoi_dimensions", "3D"))
        feature = _VORONOI_FEATURE_MAP.get(getattr(node, "feature", "F1"))
        metric = _VORONOI_METRIC_MAP.get(getattr(node, "distance", "EUCLIDEAN"))
        out_spec = _VORONOI_OUTPUT_FN.get(output_name)
        if dims is None or feature is None or metric is None or out_spec is None:
            raise _UnsupportedGraph(f"unsupported Voronoi config/output {output_name}")

        normalize_flag = 1 if bool(getattr(node, "normalize", False)) else 0

        vec_sock = _input_by_name(node, "Vector")
        if dims == 1:
            # 1D Voronoi ignores Vector in Cycles; avoid emitting a dead
            # Generated coordinate binding or marking uses_generated.
            p_expr = "vec3(0, 0, 0)"
        elif _live_links(vec_sock):
            p_expr = self.eval_input(vec_sock, "vec3", depth)
        else:
            p_expr, _ = self._generated_coord()

        w_expr = self.eval_input(_input_by_name(node, "W"), "scalar", depth, missing_default=0.0)
        scale_expr = self.eval_input(_input_by_name(node, "Scale"), "scalar", depth, missing_default=5.0)
        detail_expr = self.eval_input(_input_by_name(node, "Detail"), "scalar", depth, missing_default=0.0)
        rough_expr = self.eval_input(_input_by_name(node, "Roughness"), "scalar", depth, missing_default=0.5)
        lac_expr = self.eval_input(_input_by_name(node, "Lacunarity"), "scalar", depth, missing_default=2.0)
        smooth_expr = self.eval_input(_input_by_name(node, "Smoothness"), "scalar", depth, missing_default=1.0)
        exp_expr = self.eval_input(_input_by_name(node, "Exponent"), "scalar", depth, missing_default=0.5)
        rand_expr = self.eval_input(_input_by_name(node, "Randomness"), "scalar", depth, missing_default=1.0)

        fn_name, ret_type = out_spec
        call = (
            f"{fn_name}({p_expr}, {w_expr}, {dims}, {feature}, {metric}, "
            f"{scale_expr}, {detail_expr}, {rough_expr}, {lac_expr}, "
            f"{smooth_expr}, {exp_expr}, {rand_expr}, {normalize_flag})"
        )
        return self._bind(call, ret_type)

    def _compile_noise(self, node, output_name: str, depth: int) -> tuple[str, str]:
        dims = _DIMS_MAP.get(getattr(node, "noise_dimensions", "3D"))
        ntype = _NOISE_TYPE_MAP.get(getattr(node, "noise_type", "FBM"))
        if dims is None or ntype is None or output_name not in ("Fac", "Color"):
            raise _UnsupportedGraph(f"unsupported Noise config/output {output_name}")

        normalize_flag = 1 if bool(getattr(node, "normalize", True)) else 0
        vec_sock = _input_by_name(node, "Vector")
        if dims == 1:
            p_expr = "vec3(0, 0, 0)"
        elif _live_links(vec_sock):
            p_expr = self.eval_input(vec_sock, "vec3", depth)
        else:
            p_expr, _ = self._generated_coord()

        w_expr = self.eval_input(_input_by_name(node, "W"), "scalar", depth, missing_default=0.0)
        scale_expr = self.eval_input(_input_by_name(node, "Scale"), "scalar", depth, missing_default=5.0)
        svar, _ = self._bind(scale_expr, "scalar")
        detail_expr = self.eval_input(_input_by_name(node, "Detail"), "scalar", depth, missing_default=2.0)
        rough_expr = self.eval_input(_input_by_name(node, "Roughness"), "scalar", depth, missing_default=0.5)
        lac_expr = self.eval_input(_input_by_name(node, "Lacunarity"), "scalar", depth, missing_default=2.0)
        offset_expr = self.eval_input(_input_by_name(node, "Offset"), "scalar", depth, missing_default=0.0)
        gain_expr = self.eval_input(_input_by_name(node, "Gain"), "scalar", depth, missing_default=1.0)
        dist_expr = self.eval_input(_input_by_name(node, "Distortion"), "scalar", depth, missing_default=0.0)

        fn_name = "osl_noise_texture_color" if output_name == "Color" else "osl_noise_texture"
        ret_type = "vec3" if output_name == "Color" else "scalar"
        call = (
            f"{fn_name}(({p_expr}) * {svar}, ({w_expr}) * {svar}, {dims}, {ntype}, "
            f"{detail_expr}, {rough_expr}, {lac_expr}, {offset_expr}, {gain_expr}, "
            f"{dist_expr}, {normalize_flag})"
        )
        return self._bind(call, ret_type)

    def _compile_white_noise(self, node, output_name: str, depth: int) -> tuple[str, str]:
        dims = _DIMS_MAP.get(getattr(node, "noise_dimensions", "3D"))
        if dims is None or output_name not in ("Value", "Color"):
            raise _UnsupportedGraph(f"unsupported White Noise config/output {output_name}")
        # Unlike Noise/Voronoi, Cycles WhiteNoiseTextureNode has NO implicit
        # LINK_TEXTURE_GENERATED default — an unlinked Vector evaluates its
        # socket default (0, 0, 0).
        vec_sock = _input_by_name(node, "Vector")
        if dims == 1:
            p_expr = "vec3(0, 0, 0)"
        else:
            p_expr = self.eval_input(vec_sock, "vec3", depth, missing_default=(0.0, 0.0, 0.0))
        w_expr = self.eval_input(_input_by_name(node, "W"), "scalar", depth, missing_default=0.0)
        fn_name = "osl_white_noise_color" if output_name == "Color" else "osl_white_noise"
        ret_type = "vec3" if output_name == "Color" else "scalar"
        return self._bind(f"{fn_name}({p_expr}, {w_expr}, {dims})", ret_type)

    def _compile_mix(self, node, output_name: str, depth: int) -> tuple[str, str]:
        bl = getattr(node, "bl_idname", "")
        if bl == "ShaderNodeMixRGB":
            mode = _MIX_MODE_MAP.get(getattr(node, "blend_type", "MIX"))
            if mode is None:
                raise _UnsupportedGraph(f"MixRGB blend_type {getattr(node, 'blend_type', '')}")
            t = self.eval_input(_input_by_name(node, "Fac"), "scalar", depth)
            a = self.eval_input(_input_by_name(node, "Color1"), "vec3", depth)
            b = self.eval_input(_input_by_name(node, "Color2"), "vec3", depth)
            clamp_result = 1 if bool(getattr(node, "use_clamp", False)) else 0
            return self._bind(f"osl_mix_color({mode}, {t}, {a}, {b}, 1, {clamp_result})", "vec3")

        data_type = getattr(node, "data_type", "FLOAT")
        clamp_factor = bool(getattr(node, "clamp_factor", True))
        if data_type == "RGBA":
            mode = _MIX_MODE_MAP.get(getattr(node, "blend_type", "MIX"))
            if mode is None:
                raise _UnsupportedGraph(f"Mix RGBA blend_type {getattr(node, 'blend_type', '')}")
            t = self.eval_input(_input_by_name(node, "Factor", "VALUE"), "scalar", depth)
            a = self.eval_input(_input_by_name(node, "A", "RGBA"), "vec3", depth)
            b = self.eval_input(_input_by_name(node, "B", "RGBA"), "vec3", depth)
            cf = 1 if clamp_factor else 0
            cr = 1 if bool(getattr(node, "clamp_result", False)) else 0
            return self._bind(f"osl_mix_color({mode}, {t}, {a}, {b}, {cf}, {cr})", "vec3")
        if data_type == "FLOAT":
            t = self.eval_input(_input_by_name(node, "Factor", "VALUE"), "scalar", depth)
            if clamp_factor:
                t = f"clamp({t}, 0, 1)"
            a = self.eval_input(_input_by_name(node, "A", "VALUE"), "scalar", depth)
            b = self.eval_input(_input_by_name(node, "B", "VALUE"), "scalar", depth)
            return self._bind(f"mix({a}, {b}, {t})", "scalar")
        if data_type == "VECTOR":
            if getattr(node, "factor_mode", "UNIFORM") != "UNIFORM":
                raise _UnsupportedGraph("Mix VECTOR non-uniform factor_mode")
            t = self.eval_input(_input_by_name(node, "Factor", "VALUE"), "scalar", depth)
            if clamp_factor:
                t = f"clamp({t}, 0, 1)"
            a = self.eval_input(_input_by_name(node, "A", "VECTOR"), "vec3", depth)
            b = self.eval_input(_input_by_name(node, "B", "VECTOR"), "vec3", depth)
            return self._bind(f"mix({a}, {b}, {t})", "vec3")
        raise _UnsupportedGraph(f"Mix data_type {data_type}")

    def _compile_hue_sat(self, node, depth: int) -> tuple[str, str]:
        hue = self.eval_input(_input_by_name(node, "Hue"), "scalar", depth)
        sat = self.eval_input(_input_by_name(node, "Saturation"), "scalar", depth)
        val = self.eval_input(_input_by_name(node, "Value"), "scalar", depth)
        fac = self.eval_input(_input_by_name(node, "Fac"), "scalar", depth)
        col = self.eval_input(_input_by_name(node, "Color"), "vec3", depth)
        cvar, _ = self._bind(col, "vec3")
        hvar, _ = self._bind(f"osl_rgb2hsv({cvar})", "vec3")
        h_adj = f"frac({hvar}.x + ({hue}) + 0.5)"
        s_adj = f"clamp({hvar}.y * ({sat}), 0, 1)"
        v_adj = f"max({hvar}.z * ({val}), 0)"
        rgb_var, _ = self._bind(f"osl_hsv2rgb(vec3({h_adj}, {s_adj}, {v_adj}))", "vec3")
        mix_var, _ = self._bind(f"mix({cvar}, {rgb_var}, {fac})", "vec3")
        return self._bind(
            f"vec3(max({mix_var}.x, 0), max({mix_var}.y, 0), max({mix_var}.z, 0))",
            "vec3",
        )

    def _compile_color_ramp(self, node, output_name: str, depth: int) -> tuple[str, str]:
        if output_name != "Color":
            raise _UnsupportedGraph(f"ColorRamp output {output_name}")
        ramp = getattr(node, "color_ramp", None)
        if ramp is None or getattr(ramp, "color_mode", "RGB") != "RGB":
            raise _UnsupportedGraph("ColorRamp non-RGB mode")
        interp = getattr(ramp, "interpolation", "LINEAR")
        if interp not in ("LINEAR", "CONSTANT"):
            raise _UnsupportedGraph(f"ColorRamp interpolation {interp}")
        elems = sorted(
            [(float(e.position), (float(e.color[0]), float(e.color[1]), float(e.color[2])))
             for e in ramp.elements],
            key=lambda item: item[0],
        )
        if not elems:
            raise _UnsupportedGraph("ColorRamp empty")
        fac = self.eval_input(_input_by_name(node, "Fac"), "scalar", depth)
        tvar, _ = self._bind(f"clamp({fac}, 0, 1)", "scalar")
        # Build right-to-left piecewise conditional.
        expr = _fmt_vec3(elems[-1][1])
        for idx in range(len(elems) - 2, -1, -1):
            p0, c0 = elems[idx]
            p1, c1 = elems[idx + 1]
            c0_s = _fmt_vec3(c0)
            c1_s = _fmt_vec3(c1)
            if interp == "CONSTANT" or (p1 - p0) <= 1e-12:
                seg = c0_s
            else:
                inv_span = _fmt(1.0 / (p1 - p0))
                seg = f"mix({c0_s}, {c1_s}, clamp(({tvar} - {_fmt(p0)}) * {inv_span}, 0, 1))"
            expr = f"mix({expr}, {seg}, ({tvar} < {_fmt(p1)}))"
        # Clamp below first stop.
        first_p, first_c = elems[0]
        if first_p > 0.0:
            expr = f"mix({expr}, {_fmt_vec3(first_c)}, ({tvar} <= {_fmt(first_p)}))"
        return self._bind(expr, "vec3")

    def finalize(self, final_expr: str) -> CompiledExpression:
        lines = [f"def {name} {body}" for name, body in self._defs]
        lines.append(final_expr)
        return CompiledExpression(
            expression="\n".join(lines),
            uses_generated=self.uses_generated,
        )


def compile_socket_expression(
    socket,
    *,
    want_type: str = "vec3",
    bbox_min=None,
    bbox_max=None,
    scale: tuple[float, float, float] | float | None = None,
) -> CompiledExpression | None:
    """Compile a linked Blender shader input socket into a RISE expression.

    Returns ``CompiledExpression`` on success, or ``None`` if the socket is
    unlinked or any upstream node is not faithfully supported.
    """
    links = _live_links(socket)
    if not links:
        return None
    compiler = _Compiler(bbox_min=bbox_min, bbox_max=bbox_max)
    try:
        link = links[0]
        val = compiler.eval_node_output(link.from_node, link.from_socket.name, 0)
        expr = compiler._coerce(val, want_type, getattr(link.from_socket, "type", ""))
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
                    # Coerce the socket to scalar first (Cycles Color->Float
                    # luminance / Vector->Float average), then tint by RGB.
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
    compiler = _Compiler(bbox_min=bbox_min, bbox_max=bbox_max)
    try:
        c_val = compiler.eval_node_output(
            color_links[0].from_node, color_links[0].from_socket.name, 0
        )
        c_expr = compiler._coerce(
            c_val, "vec3", getattr(color_links[0].from_socket, "type", "")
        )
        s_val = compiler.eval_node_output(
            scalar_links[0].from_node, scalar_links[0].from_socket.name, 0
        )
        s_expr = compiler._coerce(
            s_val, "scalar", getattr(scalar_links[0].from_socket, "type", "")
        )
        return compiler.finalize(f"(({c_expr}) * ({s_expr}))")
    except _UnsupportedGraph:
        return None
