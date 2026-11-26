"""Material baking + node-graph classification for the RISE bridge.

See ``docs/BLENDER_MATERIAL_TRANSLATION.md`` for the design.  This
module owns:

- ``classify_material(mat)`` — decides whether a material's node graph
  is "simple" (direct procedural translation possible) or "complex"
  (manual channel-image bake or existing fallback); Tangent is excluded
  because it has an independent export-time corner producer.
- ``bake_material_to_images(obj, mat, options)`` — drives Blender's
  ``bpy.ops.object.bake`` for a three-channel approximation to
  PNG files, then stores the resulting paths as ID-property metadata
  on the material for subsequent exports to consume. Rendering does
  not invoke this channel-image bake automatically.
- ``baked_paths(mat)`` — accessor returning the cached PNG paths, or
  ``None`` if no recorded channel file exists. The synthetic exporter
  requires diffuse; roughness and normal are optional.
- ``baked_cache_is_stale(mat)`` — content-hash comparison so the UI
  can flag detected fingerprint changes or missing recorded files;
  it does not detect every shader edit or enforce export freshness.
- ``bake_complex_materials_in_scene(scene, …)`` — driver used by
  the user-facing operator ``RISE_OT_bake_materials``.

Workflow
--------

The bake pipeline is invoked **explicitly** by the user via the
``RISE_OT_bake_materials`` operator (button in Render Properties →
"RISE Material Baking" → "Bake Procedural Materials") rather than
automatically during ``engine.render()``.

We tried automatic baking during render — wiring `bpy.ops.object.bake`
into a `render_init` handler with a full `bpy.context.temp_override`
populated for `scene`, `view_layer`, `window`, `area`, `region`,
`active_object`, `selected_objects` — and hit four cascading Blender-
API failure modes:

  1. Engine-swap mid-render destroys the live engine RNA struct →
     SIGSEGV on the next `self.report` call.
  2. `render_init` runs inside a restricted context that blocks reads
     of `bpy.context.selected_objects` etc. unless explicitly
     overridden.
  3. Cycles' bake operator refuses objects with `hide_render = True`
     (in interactive mode only — `--background` skips the check).
  4. Libc malloc double-free on bake-target image teardown when the
     bake is invoked from a render handler.

Each fix unlocked the next; the auto-bake-on-render architecture
fundamentally fights `bpy.ops.object.bake`'s preconditions (it
expects to be invoked from a user click in a 3D View).  The supported channel-image workflow here is the explicit operator;
these historical failures do not describe the separate Tangent child bake.

`engine.render()` gates viable complex materials whose limited current graph fingerprint
has no matching attempted marker, directing the user to the Bake button. A
matching attempted hash allows render despite failure or missing PNGs.
The isolated automatic Tangent corner bake belongs to tangent_bake.py
and does not use this manual channel driver or live engine swap.

The simple-graph translator (the "Option A" half of the hybrid
described in BLENDER_MATERIAL_TRANSLATION.md) lives in
``exporter.py``; this module is responsible only for the "Option B"
bake side and the classifier that routes between them.
"""

from __future__ import annotations

from dataclasses import dataclass
import math
import os
import tempfile
from typing import Optional

import bpy
from .blender_compat import rna_key, uses_nodes
from .tangent_bake import find_material_output

# ---------------------------------------------------------------------------
# Classifier
# ---------------------------------------------------------------------------

# Ordinary material-input nodes the simple translator can handle
# (or transparently traverse); Tangent has a separate full-graph producer.
# See docs/BLENDER_MATERIAL_TRANSLATION.md "Supported node table" — keep
# the two in sync.
_SIMPLE_TRAVERSABLE_NODES = frozenset({
    "NodeReroute",
    "ShaderNodeRGB",
    "ShaderNodeValue",
    "ShaderNodeTexImage",
    "ShaderNodeTexCoord",
    "ShaderNodeUVMap",          # named/active UV layers compiled natively via mesh_attr3 (ABI v23)
    "ShaderNodeMapping",
    # Noise, Voronoi, and White Noise compile natively via `procedural_expr`
    # into RISE `expression_painter` expressions on supported Principled
    # sockets (and Checker stays "simple" with a per-input fallback warning
    # so it does not trigger a lossy metallic-0 bake gate on F12).
    "ShaderNodeTexNoise",
    "ShaderNodeTexVoronoi",
    "ShaderNodeTexWhiteNoise",
    "ShaderNodeTexChecker",
    "ShaderNodeTexGradient",    # all 7 Cycles gradient types compiled natively via osl_gradient
    "ShaderNodeTexBrick",       # Cycles brick pattern compiled natively via osl_brick
    "ShaderNodeValToRGB",       # Color Ramp (RGB LINEAR/CONSTANT/EASE + HSV/HSL + Alpha compiled natively)
    "ShaderNodeMix",            # 4.x mix node (FLOAT/VECTOR uniform+non-uniform/RGBA compiled natively)
    "ShaderNodeMixRGB",         # legacy mix node (compiled natively)
    "ShaderNodeMath",           # all 41 ops + use_clamp compiled natively via osl_math
    "ShaderNodeVectorMath",     # all 30 ops compiled natively via osl_vmath / osl_vmath_scalar
    "ShaderNodeClamp",          # MINMAX + RANGE compiled natively via osl_clamp
    "ShaderNodeMapRange",       # FLOAT + FLOAT_VECTOR compiled natively via osl_map_range / osl_vmap_range
    "ShaderNodeVectorRotate",   # AXIS/X/Y/Z/EULER_XYZ compiled natively via osl_vector_rotate
    "ShaderNodeVectorTransform",# WORLD/OBJECT/CAMERA compiled natively via Group 7 space transforms
    "ShaderNodeObjectInfo",     # Location/Color/Alpha/Object Index/Material Index/Random compiled natively
    "ShaderNodeAttribute",      # GEOMETRY attributes compiled natively via mesh_attr3 / mesh_attr_alpha (ABI v21)
    "ShaderNodeVertexColor",    # vertex/color attributes compiled natively via mesh_attr3 / mesh_attr_alpha
    "ShaderNodeNewGeometry",    # Backfacing / Random Per Island / Position / Normal / True Normal
    "ShaderNodeParticleInfo",   # Random compiled natively via osl_particle_random (ABI v23)
    "ShaderNodeLightPath",      # Is Camera Ray compiled natively via osl_is_camera_ray (Phase 3)
    "ShaderNodeLayerWeight",    # Fresnel / Facing compiled natively via osl_layer_weight (Phase 3)
    "ShaderNodeFresnel",        # Fresnel IOR compiled natively via osl_fresnel (Phase 3)
    "ShaderNodeAmbientOcclusion",  # ray-traced AO compiled natively via osl_ao (ABI v21)
    "ShaderNodeGroup",          # pure-node group inlining (depth <= 8, cycle-guarded)
    "NodeGroupInput",
    "ShaderNodeRGBToBW",        # Rec.709 luminance compiled natively
    "ShaderNodeRGBCurve",       # compiled natively via exact Cycles R(C(t))/G(C(t))/B(C(t)) ramp
    "ShaderNodeHueSaturation",  # compiled natively in procedural subgraphs
    "ShaderNodeBrightContrast", # compiled natively in procedural subgraphs
    "ShaderNodeGamma",          # compiled natively in procedural subgraphs
    "ShaderNodeInvert",         # compiled natively in procedural subgraphs
    "ShaderNodeSeparateXYZ",    # compiled natively in procedural subgraphs
    "ShaderNodeCombineXYZ",     # compiled natively in procedural subgraphs
    "ShaderNodeSeparateColor",  # compiled natively in procedural subgraphs
    "ShaderNodeCombineColor",   # compiled natively in procedural subgraphs
    "ShaderNodeBump",
    "ShaderNodeNormalMap",
})

# Public alias for cross-module reuse.  exporter.py's hair-material
# upstream-graph classifier (`_hair_graph_supported`) walks the nodes
# feeding a Principled Hair BSDF against this set -- a hair material has
# no bake fallback (baking is meaningless for curves), and curves carry no
# named mesh attributes and do not run procedural_expr, so
# mesh/hit-context/group nodes are intentionally excluded here so hair
# materials refuse them loudly instead of silently falling back to defaults.
SUPPORTED_UPSTREAM_NODES = _SIMPLE_TRAVERSABLE_NODES - frozenset({
    "ShaderNodeAttribute",
    "ShaderNodeVertexColor",
    "ShaderNodeNewGeometry",
    "ShaderNodeParticleInfo",
    "ShaderNodeLightPath",
    "ShaderNodeLayerWeight",
    "ShaderNodeFresnel",
    "ShaderNodeAmbientOcclusion",
    "ShaderNodeUVMap",
    "ShaderNodeGroup",
    "NodeGroupInput",
})

# Nodes that force the bake fallback — see docs/BLENDER_MATERIAL_TRANSLATION.md
# "Force-bake list". On ordinary inputs other than Tangent, nodes not in
# _SIMPLE_TRAVERSABLE_NODES classify complex for the manual workflow.
# This set is for documentation / fast-path diagnostics, not Tangent.
_FORCE_BAKE_NODES = frozenset({
    "ShaderNodeAddShader",
    "ShaderNodeGeometry",
    "ShaderNodeWireframe",
    "ShaderNodeTexWave",
    "ShaderNodeTexMagic",
    "ShaderNodeTexIES",
    "ShaderNodeTexPointDensity",
})


@dataclass(frozen=True)
class MixLeafSpec:
    """One flattened leaf of a supported ``ShaderNodeMixShader`` tree."""

    kind: str  # "principled" | "diffuse" | "glossy" | "emission" | "transparent" | "absorber"
    node: object | None
    # Root-to-leaf chain of (fac_socket, fac_default, is_second_branch).
    factors: tuple[tuple[object | None, float, bool], ...]


def _live_socket_links(socket) -> list:
    if socket is None or not getattr(socket, "is_linked", False):
        return []
    return [lk for lk in socket.links if not getattr(lk, "is_muted", False)]


def _unwrap_reroute_node(node, max_depth: int = 16) -> tuple[object | None, bool]:
    """Unwrap ``NodeReroute`` chains.  Returns ``(node_or_none, True)`` on a
    valid chain or genuinely unlinked reroute, or ``(None, False)`` on cycles
    or depth overflow so callers fail closed instead of fabricating absorbers."""
    seen: set[int] = set()
    depth = 0
    while node is not None and getattr(node, "bl_idname", "") == "NodeReroute":
        depth += 1
        if depth > max_depth:
            return (None, False)
        nid = rna_key(node)
        if nid in seen:
            return (None, False)
        seen.add(nid)
        inp = node.inputs[0] if getattr(node, "inputs", None) else None
        links = _live_socket_links(inp)
        if not links:
            return (None, True)
        node = links[0].from_node
    return (node, True)


def _principled_is_transmission_heavy(node) -> bool:
    """Mirror exporter.py's transmission_heavy predicate (MixMaterial refuses
    containment-tracking DielectricMaterial leaves)."""
    for sock_name in ("Transmission Weight", "Transmission"):
        sock = node.inputs.get(sock_name) if hasattr(node.inputs, "get") else None
        if sock is None:
            for inp in getattr(node, "inputs", ()):
                if inp.name == sock_name:
                    sock = inp
                    break
        if sock is not None:
            if _live_socket_links(sock):
                return True
            try:
                if float(sock.default_value) >= 0.05:
                    return True
            except (TypeError, ValueError):
                pass
            break
    return False


def _transparent_passthrough_factor(node) -> tuple[object | None, float, bool] | None:
    """Return a MixShader weight-factor tuple ``(socket_or_None, default_val, True)``
    iff ``node`` is a grayscale ``ShaderNodeBsdfTransparent`` whose color maps
    exact-physically to ``w_clear = w * clamp(gray, 0, 1)`` (with ``1 - gray``
    absorbed in MixMaterial's shortfall).  Returns ``None`` for chromatic
    transparent shaders."""
    color_sock = None
    for inp in getattr(node, "inputs", ()):
        if inp.name == "Color":
            color_sock = inp
            break
    if color_sock is None:
        return (None, 1.0, True)
    links = _live_socket_links(color_sock)
    if links:
        # Walk reroutes and muted passthrough nodes to inspect the true upstream
        # output socket type (failing closed if a muted node has no passthrough).
        from . import procedural_expr as _procedural_expr
        src_node = links[0].from_node
        src_sock = links[0].from_socket
        seen: set[tuple[int, str]] = set()
        steps = 0
        while src_node is not None and steps < 16:
            steps += 1
            out_key = _procedural_expr.canonical_output_name(src_node, src_sock) if src_sock is not None else ""
            vkey = (rna_key(src_node), out_key if getattr(src_node, "mute", False) else "")
            if vkey in seen:
                return None
            seen.add(vkey)
            if getattr(src_node, "bl_idname", "") == "NodeReroute":
                rin = src_node.inputs[0] if getattr(src_node, "inputs", None) else None
                rlinks = _live_socket_links(rin)
                if not rlinks:
                    return None
                src_node = rlinks[0].from_node
                src_sock = rlinks[0].from_socket
                continue
            if getattr(src_node, "mute", False):
                pass_inp = _procedural_expr.resolve_muted_input_socket(src_node, out_key)
                if pass_inp is None:
                    return None
                plinks = _live_socket_links(pass_inp)
                if not plinks:
                    # Muted node with unlinked scalar passthrough input -> scalar default
                    if getattr(pass_inp, "type", "") in ("VALUE", "INT"):
                        return (color_sock, 1.0, True)
                    return None
                src_node = plinks[0].from_node
                src_sock = plinks[0].from_socket
                continue
            break
        # Any scalar (VALUE/INT) socket feeding Transparent.Color broadcasts
        # (s, s, s) in Cycles -> exact grayscale passthrough.
        if src_sock is not None and getattr(src_sock, "type", "") in ("VALUE", "INT"):
            return (color_sock, 1.0, True)
        return None
    try:
        val = color_sock.default_value
        r, g, b = float(val[0]), float(val[1]), float(val[2])
        if not (math.isfinite(r) and math.isfinite(g) and math.isfinite(b)):
            return None
        if abs(r - g) <= 1e-3 and abs(g - b) <= 1e-3 and -1e-4 <= r <= 1.01:
            return (None, max(0.0, min(1.0, r)), True)
        return None
    except (TypeError, ValueError, IndexError):
        return None


def flatten_mix_shader_surface(
    output_node,
    *,
    max_mix_depth: int = 8,
    max_leaves: int = 16,
) -> list[MixLeafSpec] | None:
    """If ``output_node.Surface`` is driven by a ``ShaderNodeMixShader`` tree
    whose leaves are supported non-refracting shaders, flatten it into a list
    of :class:`MixLeafSpec` entries.  Returns ``None`` otherwise (including
    when the surface is a single Principled BSDF or uses unsupported shaders
    or carries an interior Volume).
    """
    if output_node is None:
        return None
    surface_socket = output_node.inputs.get("Surface") if hasattr(output_node.inputs, "get") else None
    if surface_socket is None:
        for inp in getattr(output_node, "inputs", ()):
            if inp.name == "Surface":
                surface_socket = inp
                break
    surf_links = _live_socket_links(surface_socket)
    if not surf_links:
        return None

    # MixMaterial refuses interior_medium; if Volume is linked, fail closed.
    vol_socket = output_node.inputs.get("Volume") if hasattr(output_node.inputs, "get") else None
    if vol_socket is None:
        for inp in getattr(output_node, "inputs", ()):
            if inp.name == "Volume":
                vol_socket = inp
                break
    if _live_socket_links(vol_socket):
        return None

    root, root_ok = _unwrap_reroute_node(surf_links[0].from_node)
    if not root_ok or root is None or getattr(root, "bl_idname", "") != "ShaderNodeMixShader":
        return None
    if getattr(root, "mute", False):
        return None

    leaves: list[MixLeafSpec] = []

    def _walk(node, factors: tuple[tuple[object | None, float, bool], ...], depth: int, path_seen: frozenset[int]) -> bool:
        if len(leaves) >= max_leaves:
            return False
        if node is None:
            leaves.append(MixLeafSpec(kind="absorber", node=None, factors=factors))
            return True
        node, node_ok = _unwrap_reroute_node(node)
        if not node_ok:
            return False
        if node is None:
            leaves.append(MixLeafSpec(kind="absorber", node=None, factors=factors))
            return True
        if getattr(node, "mute", False):
            return False
        nid = rna_key(node)
        if nid in path_seen:
            return False
        bl = getattr(node, "bl_idname", "")
        if bl == "ShaderNodeMixShader":
            if depth >= max_mix_depth:
                return False
            inputs = list(getattr(node, "inputs", ()))
            if len(inputs) < 3:
                return False
            fac_sock = inputs[0]
            try:
                fac_default = float(fac_sock.default_value)
            except (AttributeError, TypeError, ValueError):
                fac_default = 0.5
            next_seen = path_seen | {nid}
            for branch_idx, sock_idx in ((False, 1), (True, 2)):
                sh_sock = inputs[sock_idx]
                sh_links = _live_socket_links(sh_sock)
                child_node = sh_links[0].from_node if sh_links else None
                if not _walk(child_node, factors + ((fac_sock, fac_default, branch_idx),), depth + 1, next_seen):
                    return False
            return True
        if bl == "ShaderNodeBsdfPrincipled":
            if _principled_is_transmission_heavy(node):
                return False
            leaves.append(MixLeafSpec(kind="principled", node=node, factors=factors))
            return True
        if bl == "ShaderNodeBsdfDiffuse":
            leaves.append(MixLeafSpec(kind="diffuse", node=node, factors=factors))
            return True
        if bl in ("ShaderNodeBsdfGlossy", "ShaderNodeBsdfAnisotropic"):
            leaves.append(MixLeafSpec(kind="glossy", node=node, factors=factors))
            return True
        if bl == "ShaderNodeEmission":
            leaves.append(MixLeafSpec(kind="emission", node=node, factors=factors))
            return True
        if bl == "ShaderNodeBsdfTransparent":
            tp_factor = _transparent_passthrough_factor(node)
            if tp_factor is None:
                return False
            leaves.append(MixLeafSpec(kind="transparent", node=node, factors=factors + (tp_factor,)))
            return True
        return False

    if not _walk(root, (), 0, frozenset()):
        return None
    return leaves if leaves else None


def find_simple_bsdf_through_surface(output_node) -> tuple[str, object] | None:
    """If ``output_node.Surface`` (through reroutes, with no Volume linked)
    terminates at a single standalone ``Diffuse``, ``Glossy``/``Anisotropic``,
    or ``Emission`` shader node, return ``(kind, node)`` matching
    :class:`MixLeafSpec` kinds."""
    if output_node is None:
        return None
    vol_socket = output_node.inputs.get("Volume") if hasattr(output_node.inputs, "get") else None
    if vol_socket is None:
        for inp in getattr(output_node, "inputs", ()):
            if inp.name == "Volume":
                vol_socket = inp
                break
    if _live_socket_links(vol_socket):
        return None
    surface_socket = output_node.inputs.get("Surface") if hasattr(output_node.inputs, "get") else None
    if surface_socket is None:
        for inp in getattr(output_node, "inputs", ()):
            if inp.name == "Surface":
                surface_socket = inp
                break
    surf_links = _live_socket_links(surface_socket)
    if not surf_links:
        return None
    node, ok = _unwrap_reroute_node(surf_links[0].from_node)
    if not ok or node is None or getattr(node, "mute", False):
        return None
    bl = getattr(node, "bl_idname", "")
    if bl == "ShaderNodeBsdfDiffuse":
        return ("diffuse", node)
    if bl in ("ShaderNodeBsdfGlossy", "ShaderNodeBsdfAnisotropic"):
        return ("glossy", node)
    if bl == "ShaderNodeEmission":
        return ("emission", node)
    return None


def _probe_image_node_supported(node) -> str | None:
    """Lightweight image-resolver probe used by ``classify_material`` (never
    unpacks files to disk).  Returns a valid RISE identifier iff ``node`` is a
    supported ``ShaderNodeTexImage``."""
    from . import procedural_expr as _procedural_expr
    if getattr(node, "bl_idname", "") != "ShaderNodeTexImage":
        return None
    if getattr(node, "projection", "FLAT") != "FLAT":
        return None
    if getattr(node, "extension", "REPEAT") not in _procedural_expr.SUPPORTED_TEX_IMAGE_EXTENSIONS:
        return None
    image = getattr(node, "image", None)
    if image is None:
        return None
    if getattr(image, "source", "FILE") not in ("FILE", "GENERATED"):
        return None
    return "__probe_tex"


def _is_traversable_input_node(node) -> bool:
    from . import procedural_expr as _procedural_expr
    if getattr(node, "mute", False):
        return False
    bl = getattr(node, "bl_idname", "")
    if bl not in _SIMPLE_TRAVERSABLE_NODES:
        return False
    if bl == "ShaderNodeAttribute":
        if getattr(node, "attribute_type", "GEOMETRY") != "GEOMETRY":
            return False
        if not str(getattr(node, "attribute_name", "") or ""):
            return False
    if bl == "ShaderNodeUVMap":
        if getattr(node, "from_instancer", False):
            return False
    if bl == "ShaderNodeTexImage":
        if getattr(node, "projection", "FLAT") != "FLAT":
            return False
        if getattr(node, "extension", "REPEAT") not in _procedural_expr.SUPPORTED_TEX_IMAGE_EXTENSIONS:
            return False
        image = getattr(node, "image", None)
        if image is None or getattr(image, "source", "FILE") not in ("FILE", "GENERATED"):
            return False
    if bl == "ShaderNodeNormalMap":
        if getattr(node, "space", "TANGENT") != "TANGENT":
            return False
        if str(getattr(node, "uv_map", "") or ""):
            return False
    return True


def _walk_simple_inputs(root_sockets: list) -> bool:
    """Return True iff every node reachable from ``root_sockets`` (within depth
    <= 16) is in ``_SIMPLE_TRAVERSABLE_NODES`` and satisfies node-specific gates.

    Follows muted passthrough nodes via ``internal_links`` (matching socket
    identifiers first), inlines ``ShaderNodeGroup`` subgraphs (depth <= 8,
    cycle-guarded), and enforces classifier-vs-exporter parity (skipping
    ``AO.Color`` when only ``AO`` is consumed, skipping ``TexBrick`` colors
    when only ``Fac`` is consumed, rejecting ``Bump``/``NormalMap`` feeding
    ``AO.Normal``, gating ``NewGeometry`` / ``ParticleInfo`` / ``UVMap`` /
    ``TexImage`` modes, and requiring ``Normal`` / ``Coat Normal`` roots to hit
    a direct ``Bump`` / ``NormalMap`` node before branching into Color/Height
    subgraphs).
    """
    from . import procedural_expr as _procedural_expr

    for root_socket in root_sockets:
        root_links = _live_socket_links(root_socket)
        if not root_links:
            continue
        root_name = getattr(root_socket, "name", "")
        is_normal_root = root_name in ("Normal", "Coat Normal")
        is_coat_normal_root = (root_name == "Coat Normal")
        # Stack items: (group_stack_tuple, node, out_key, depth, in_ao_normal, before_normal_mod)
        root_from = root_links[0].from_node
        root_out_key = _procedural_expr.canonical_output_name(root_from, root_links[0].from_socket)
        stack: list[tuple[tuple, object, str, int, bool, bool]] = [(
            (),
            root_from,
            root_out_key,
            0,
            False,
            is_normal_root,
        )]
        visited: set[tuple] = set()
        saw_image = False
        saw_checker = False

        while stack:
            gstack, node, out_key, depth, in_ao_normal, before_normal_mod = stack.pop()
            if depth > 16:
                return False
            bl = getattr(node, "bl_idname", "")
            g_ids = tuple(rna_key(g) for g in gstack)
            visit_out = out_key if bl in (
                "ShaderNodeAmbientOcclusion",
                "ShaderNodeTexBrick",
                "ShaderNodeNewGeometry",
                "ShaderNodeParticleInfo",
                "ShaderNodeLightPath",
                "ShaderNodeGroup",
                "NodeGroupInput",
            ) or getattr(node, "mute", False) else ""
            visit_key = (g_ids, rna_key(node), visit_out, in_ao_normal, before_normal_mod)
            if visit_key in visited:
                continue
            visited.add(visit_key)

            # Follow Blender internal passthrough links on muted nodes; fail
            # closed if the muted node has no internal passthrough for out_key.
            if getattr(node, "mute", False):
                pass_inp = _procedural_expr.resolve_muted_input_socket(node, out_key)
                if pass_inp is None:
                    return False
                plinks = _live_socket_links(pass_inp)
                if plinks:
                    pnode = plinks[0].from_node
                    stack.append((
                        gstack,
                        pnode,
                        _procedural_expr.canonical_output_name(pnode, plinks[0].from_socket),
                        depth + 1,
                        in_ao_normal,
                        before_normal_mod,
                    ))
                continue

            if not _is_traversable_input_node(node):
                return False

            if before_normal_mod:
                if is_coat_normal_root:
                    if bl not in ("NodeReroute", "ShaderNodeNormalMap"):
                        return False
                elif bl == "ShaderNodeMix":
                    dt = getattr(node, "data_type", "FLOAT")
                    if dt not in ("RGBA", "VECTOR"):
                        return False
                    if dt == "RGBA":
                        if getattr(node, "blend_type", "MIX") != "MIX" or getattr(node, "clamp_result", False):
                            return False
                    elif getattr(node, "factor_mode", "UNIFORM") != "UNIFORM":
                        return False
                    if not getattr(node, "clamp_factor", True):
                        fac_s = next((i for i in getattr(node, "inputs", ()) if getattr(i, "name", "") == "Factor" and getattr(i, "type", "") == "VALUE"), None)
                        if fac_s is None or _live_socket_links(fac_s):
                            return False
                        try:
                            fv = float(fac_s.default_value)
                            if not (0.0 <= fv <= 1.0):
                                return False
                        except (TypeError, ValueError):
                            return False
                elif bl == "ShaderNodeMixRGB":
                    if getattr(node, "blend_type", "MIX") != "MIX" or getattr(node, "use_clamp", False):
                        return False
                elif bl not in ("NodeReroute", "ShaderNodeBump", "ShaderNodeNormalMap"):
                    return False

            if bl == "ShaderNodeGroup":
                nt = getattr(node, "node_tree", None)
                if nt is None or len(gstack) >= 8:
                    return False
                if any(rna_key(getattr(g, "node_tree", None)) == rna_key(nt) for g in gstack):
                    return False
                gout = _procedural_expr.find_group_output_node(nt)
                if gout is None:
                    return False
                g_inp = _procedural_expr.match_socket_by_key(getattr(gout, "inputs", ()), out_key)
                if g_inp is None:
                    return False
                glinks = _live_socket_links(g_inp)
                if glinks:
                    gnode = glinks[0].from_node
                    stack.append((
                        gstack + (node,),
                        gnode,
                        _procedural_expr.canonical_output_name(gnode, glinks[0].from_socket),
                        depth + 1,
                        in_ao_normal,
                        before_normal_mod,
                    ))
                continue

            if bl == "NodeGroupInput":
                if not gstack:
                    return False
                enclosing = gstack[-1]
                outer_inp = _procedural_expr.match_socket_by_key(getattr(enclosing, "inputs", ()), out_key)
                if outer_inp is None:
                    return False
                olinks = _live_socket_links(outer_inp)
                if olinks:
                    onode = olinks[0].from_node
                    stack.append((
                        gstack[:-1],
                        onode,
                        _procedural_expr.canonical_output_name(onode, olinks[0].from_socket),
                        depth + 1,
                        in_ao_normal,
                        before_normal_mod,
                    ))
                continue

            if bl == "ShaderNodeNewGeometry" and out_key not in _procedural_expr.SUPPORTED_GEOMETRY_OUTPUTS:
                return False
            if bl == "ShaderNodeParticleInfo" and out_key not in _procedural_expr.SUPPORTED_PARTICLE_INFO_OUTPUTS:
                return False
            if bl == "ShaderNodeLightPath" and out_key not in _procedural_expr.SUPPORTED_LIGHT_PATH_OUTPUTS:
                return False

            if in_ao_normal and bl in ("ShaderNodeBump", "ShaderNodeNormalMap"):
                return False
            if bl == "ShaderNodeTexImage":
                saw_image = True
            elif bl == "ShaderNodeTexChecker":
                saw_checker = True
            if saw_image and saw_checker:
                return False

            # Phase 3: Normal modifier trees allow Bump.Normal stacking and
            # Mix(RGBA/VECTOR, MIX) blending; keep before_normal_mod=True on
            # the normal-bearing inputs (Bump.Normal, Mix A/B) and switch to
            # expression mode (False) on Color/Height/Factor inputs.
            for socket in getattr(node, "inputs", ()):
                if bl == "ShaderNodeAmbientOcclusion" and out_key == "AO" and socket.name == "Color":
                    continue
                if bl == "ShaderNodeTexBrick" and out_key == "Fac" and socket.name in ("Color1", "Color2", "Mortar"):
                    continue
                # On ShaderNodeMix, skip sockets belonging to inactive data_types.
                if bl == "ShaderNodeMix":
                    dt = getattr(node, "data_type", "FLOAT")
                    sname = getattr(socket, "name", "")
                    stype = getattr(socket, "type", "")
                    if sname in ("A", "B"):
                        want_stype = "RGBA" if dt == "RGBA" else ("VECTOR" if dt == "VECTOR" else "VALUE")
                        if stype and stype != want_stype:
                            continue
                    elif sname == "Factor":
                        want_ftype = "VECTOR" if (dt == "VECTOR" and getattr(node, "factor_mode", "UNIFORM") == "NON_UNIFORM") else "VALUE"
                        if stype and stype != want_ftype:
                            continue
                links = _live_socket_links(socket)
                if links:
                    if bl == "ShaderNodeBump" and socket.name in ("Strength", "Distance"):
                        return False
                    if bl == "ShaderNodeNormalMap" and socket.name == "Strength":
                        return False
                    if before_normal_mod:
                        if bl == "ShaderNodeBump":
                            sock_before_normal = (socket.name == "Normal")
                        elif bl == "ShaderNodeNormalMap":
                            sock_before_normal = False
                        elif bl == "ShaderNodeMix":
                            sock_before_normal = (socket.name in ("A", "B"))
                        elif bl == "ShaderNodeMixRGB":
                            sock_before_normal = (socket.name in ("Color1", "Color2"))
                        else:
                            sock_before_normal = True
                    else:
                        sock_before_normal = False
                    fnode = links[0].from_node
                    next_ao_normal = in_ao_normal or (
                        bl in ("ShaderNodeAmbientOcclusion", "ShaderNodeLayerWeight", "ShaderNodeFresnel")
                        and socket.name == "Normal"
                    )
                    stack.append((
                        gstack,
                        fnode,
                        _procedural_expr.canonical_output_name(fnode, links[0].from_socket),
                        depth + 1,
                        next_ao_normal,
                        sock_before_normal,
                    ))

    return True


def _find_principled_through_surface(output_node):
    """Walk from the Material Output's Surface socket toward a single
    ShaderNodeBsdfPrincipled.  Returns the principled node, or None if
    the chain branches (Mix Shader / Add Shader) or doesn't terminate
    at a single Principled BSDF.

    Used by classify_material to detect the "exactly one Principled"
    rule from BLENDER_MATERIAL_TRANSLATION.md.
    """

    if output_node is None:
        return None
    surface_socket = output_node.inputs.get("Surface")
    if surface_socket is None or not surface_socket.is_linked:
        return None

    visited: set[int] = set()
    cursor = surface_socket.links[0].from_node
    depth = 0
    while depth < 16:
        depth += 1
        node_id = rna_key(cursor)
        if node_id in visited:
            return None
        visited.add(node_id)
        bl = cursor.bl_idname
        if bl == "ShaderNodeBsdfPrincipled":
            return cursor
        if bl == "NodeReroute":
            inp = cursor.inputs[0] if cursor.inputs else None
            if inp is None or not inp.is_linked:
                return None
            cursor = inp.links[0].from_node
            continue
        # Any other terminal shader (Mix Shader, Add Shader, Glass BSDF,
        # Diffuse BSDF, Emission, Volume) is "not a single Principled"
        # — classify complex for the manual channel-image approximation.
        # This does not schedule an automatic or faithful whole-BSDF bake.
        return None
    return None


def classify_material(material) -> str:
    """Return ``'simple'`` or ``'complex'``.

    Simple iff:
      - either a single Principled BSDF reaches Material Output.Surface, OR a
        supported non-refracting ``ShaderNodeMixShader`` tree (leaves in
        Principled/Diffuse/Glossy/Emission/white Transparent/absorber, no
        Volume, no linked Tangent on Principled leaves, and all factor/passthrough
        weights compile cleanly via ``procedural_expr``),
      - each reachable node from inputs (excluding Tangent on single-Principled
        materials, which has an independent corner producer) is in
        ``_SIMPLE_TRAVERSABLE_NODES``, unmuted, and satisfies node-specific
        gates (e.g. ``ShaderNodeAttribute`` must be ``GEOMETRY`` with a
        non-empty name; ``ShaderNodeTexImage`` must be ``FLAT`` with a valid
        ``FILE``/``GENERATED`` image and supported extension; ``ShaderNodeTexImage``
        cannot be combined with ``ShaderNodeTexChecker`` in the same socket),
      - ordinary upstream traversal depth does not exceed 16 (root 0).
    Otherwise complex.
    """

    if not uses_nodes(material):
        # No node tree (just diffuse_color on legacy materials): trivially
        # simple — exporter uses the constant colour.
        return "simple"

    nt = material.node_tree
    output_node = find_material_output(material)
    if output_node is None:
        if any(n.bl_idname == 'ShaderNodeOutputMaterial' for n in nt.nodes):
            return 'complex' # No Cycles/ALL output; unrelated targets are excluded.
        # No output node — fall back to default principled detection;
        # treat as simple so the existing exporter's first-principled
        # walk catches it.
        return "simple"

    principled = _find_principled_through_surface(output_node)
    if principled is not None:
        # Walk ordinary inputs except Tangent. An unsupported reachable
        # node classifies complex for the manual image-bake workflow.
        root_sockets = [
            socket for socket in principled.inputs
            # DL-213: tangent directions have their own full-graph corner bake.
            if socket.name != "Tangent"
        ]
        return "simple" if _walk_simple_inputs(root_sockets) else "complex"

    # Standalone Diffuse / Glossy (Anisotropic) / Emission surface root.
    simple_bsdf = find_simple_bsdf_through_surface(output_node)
    if simple_bsdf is not None:
        _kind, bsdf_node = simple_bsdf
        return "simple" if _walk_simple_inputs(list(getattr(bsdf_node, "inputs", ()))) else "complex"

    # ABI v21: ShaderNodeMixShader trees whose leaves are supported
    # non-refracting shaders (Principled/Diffuse/Glossy/Emission/white
    # Transparent/absorber) translate natively to RISE `mix_material`.
    mix_leaves = flatten_mix_shader_surface(output_node)
    if mix_leaves is None:
        return "complex"

    from . import procedural_expr as _procedural_expr

    # Validate that every factor weight expression AND combined transparent
    # passthrough expression compiles cleanly so classify_material agrees
    # 100% with _export_mix_shader_material (R1 B-P1-1 / B-P1-2).
    transparent_factors: list[list] = []
    root_sockets = []
    seen_fac: set[int] = set()
    for leaf in mix_leaves:
        if _procedural_expr.compile_mix_weight_expression(
            list(leaf.factors), image_resolver=_probe_image_node_supported
        ) is None:
            return "complex"
        if leaf.kind == "transparent":
            transparent_factors.append(list(leaf.factors))
        for fac_sock, _default, _is_second in leaf.factors:
            if fac_sock is not None:
                fid = rna_key(fac_sock)
                if fid not in seen_fac:
                    seen_fac.add(fid)
                    root_sockets.append(fac_sock)
        if leaf.node is not None:
            for socket in getattr(leaf.node, "inputs", ()):
                # tangent_bake only handles single-Principled materials (R1 B-P1-7).
                if leaf.kind == "principled" and socket.name == "Tangent" and _live_socket_links(socket):
                    return "complex"
                root_sockets.append(socket)
    if transparent_factors and _procedural_expr.compile_mix_passthrough_expression(
        transparent_factors, image_resolver=_probe_image_node_supported
    ) is None:
        return "complex"
    return "simple" if _walk_simple_inputs(root_sockets) else "complex"


# ---------------------------------------------------------------------------
# Bake metadata storage on materials
# ---------------------------------------------------------------------------

# ID-property keys used to persist bake outputs on a Blender Material.
# Lives in `mat["<key>"]` (generic ID properties added on demand).
# Keys are chosen to be obvious in the Blender UI's Custom Properties
# panel — easy to diagnose / clear by hand if needed.
BAKED_DIFFUSE_KEY = "rise_baked_diffuse_path"
BAKED_ROUGHNESS_KEY = "rise_baked_roughness_path"
BAKED_NORMAL_KEY = "rise_baked_normal_path"
BAKED_RESOLUTION_KEY = "rise_baked_resolution"
BAKED_FRAME_KEY = "rise_baked_frame"
# Limited top-level fingerprint at the last attempt with any saved channel.
# The UI compares it and recorded file existence; export does not enforce it.
BAKED_GRAPH_HASH_KEY = "rise_baked_graph_hash"
# Limited fingerprint from the last completed channel-stage attempt, even if
# no channel saved. The viable-complex render gate requires a matching marker.
# Undetected edits do not reopen the gate. Existing diffuse cache may survive
# a failed retry; otherwise export uses its existing translation/fallback.
BAKED_ATTEMPTED_HASH_KEY = "rise_baked_attempted_hash"


def _material_graph_hash(material) -> str:
    """Fingerprint selected top-level node data, not all bake dependencies.

    Includes node type/name, every input name and readable default (including
    linked inputs), upstream node/socket links, image name/path, and color-ramp
    interpolation/stops/colors. Omits other node properties (including Math
    operation and output target), nested group contents, image pixels, and
    object/scene/time/evaluation dependencies. No hashing-cost benchmark is
    claimed. A shader edit can therefore leave this fingerprint unchanged.

    Stored as attempted hash after the channel stage and as success hash only
    when any channel saves. The render guard compares the attempted marker;
    the UI compares success hash and file existence. Export consumes existing
    diffuse cache without this freshness check; this is not an auto-bake path.
    """

    import hashlib
    h = hashlib.md5()
    if not uses_nodes(material):
        return ""
    for node in material.node_tree.nodes:
        h.update(node.bl_idname.encode("utf-8"))
        h.update(node.name.encode("utf-8"))
        for sock in node.inputs:
            h.update(sock.name.encode("utf-8"))
            try:
                v = sock.default_value
                if hasattr(v, "__len__"):
                    h.update(str(tuple(v)).encode("utf-8"))
                else:
                    h.update(str(v).encode("utf-8"))
            except (AttributeError, TypeError):
                pass
            for link in sock.links:
                h.update(
                    f"link:{link.from_node.name}.{link.from_socket.name}"
                    .encode("utf-8")
                )
        # Image identity for ShaderNodeTexImage — catches "user
        # swapped the assigned image".  Image pixel contents are not
        # hashed; name/path are only a partial dependency signal.
        if node.bl_idname == "ShaderNodeTexImage" and node.image is not None:
            h.update(node.image.name.encode("utf-8"))
            h.update(node.image.filepath.encode("utf-8"))
        # Colour-ramp stops — these aren't input sockets in Blender's
        # data model, they live on the node's `color_ramp.elements`.
        if node.bl_idname == "ShaderNodeValToRGB":
            cr = node.color_ramp
            h.update(cr.interpolation.encode("utf-8"))
            for elem in cr.elements:
                h.update(f"stop:{elem.position}:{tuple(elem.color)}"
                         .encode("utf-8"))
    return h.hexdigest()


def baked_paths(material) -> Optional[dict[str, str]]:
    """Return recorded channel paths whose files exist, or None if none do.
    Keys are ``diffuse``, ``roughness``, ``normal``. No hash freshness check
    occurs. Any existing channel controls the operator's optional skip, but
    the synthetic exporter requires diffuse and uses the other two optionally.
    """

    if material is None:
        return None
    out: dict[str, str] = {}
    diffuse = material.get(BAKED_DIFFUSE_KEY)
    roughness = material.get(BAKED_ROUGHNESS_KEY)
    normal = material.get(BAKED_NORMAL_KEY)
    if diffuse and os.path.isfile(str(diffuse)):
        out["diffuse"] = str(diffuse)
    if roughness and os.path.isfile(str(roughness)):
        out["roughness"] = str(roughness)
    if normal and os.path.isfile(str(normal)):
        out["normal"] = str(normal)
    return out if out else None


def clear_baked_metadata(material) -> None:
    """Remove the seven defined keys below, not every rise_baked_* property.
    Viable complex graphs then require a manual attempt.
    This does not delete PNGs or start a bake. Simple graphs translate directly.
    """
    if material is None:
        return
    for key in (
        BAKED_DIFFUSE_KEY,
        BAKED_ROUGHNESS_KEY,
        BAKED_NORMAL_KEY,
        BAKED_RESOLUTION_KEY,
        BAKED_FRAME_KEY,
        BAKED_GRAPH_HASH_KEY,
        BAKED_ATTEMPTED_HASH_KEY,
    ):
        if key in material:
            del material[key]


def needs_bake_attempt(scene, material) -> bool:
    """Returns True iff this material should be queued for the bake
    operator from the user's perspective.

    Used by both ``engine.render()`` (to decide whether to gate the
    render) and ``ui.RISE_RENDER_PT_materials`` (to surface the CTA).
    Single source of truth: panel warning + render gate stay aligned.

    Rules:
      - simple materials → False (direct channels; Tangent is separate)
      - complex with no viable proxy → False (cannot queue a bake;
        existing slot/default fallback handled by exporter)
      - complex with viable proxy AND no attempted-hash → True
        (never tried)
      - complex with viable proxy AND attempted-hash differs from
        current limited graph hash → True (a detected edit since last attempt)
      - complex with viable proxy AND attempted-hash matches current
        graph → False (we tried; whatever the outcome, render
        proceeds: existing diffuse selects the synthetic approximation,
        with optional roughness/normal; otherwise existing translation/
        slot/default fallback applies. Undetected edits do not gate)
    """

    if classify_material(material) != "complex":
        return False
    if find_bakeable_proxy_object(scene, material) is None:
        return False
    attempted_hash = material.get(BAKED_ATTEMPTED_HASH_KEY)
    if not attempted_hash:
        return True
    return str(attempted_hash) != _material_graph_hash(material)


def collect_used_render_materials(scene, depsgraph=None) -> list:
    """Return unique original ``bpy.types.Material`` datablocks actually used
    by renderable geometry in ``scene`` (via ``depsgraph`` when available, or
    walking renderable scene objects + particle/collection/GeoNodes instance
    targets otherwise).  Mirrors ``exporter.export_scene``'s ``hide_render``
    and ``skip_collections_pattern`` rules."""
    seen: set[int] = set()
    used: list = []

    skip_patterns: set[str] = set()
    rise_settings = getattr(scene, "rise", None)
    if rise_settings is not None:
        raw = getattr(rise_settings, "skip_collections_pattern", "") or ""
        for part in raw.split(","):
            cleaned = part.strip().lower()
            if cleaned:
                skip_patterns.add(cleaned)

    def _in_skipped_collection(obj) -> bool:
        if not skip_patterns or obj is None:
            return False
        for col in getattr(obj, "users_collection", ()):
            if getattr(col, "name", "").lower() in skip_patterns:
                return True
        return False

    def _add_mat(mat):
        if mat is None:
            return
        orig = getattr(mat, "original", mat)
        k = rna_key(orig)
        if k not in seen:
            seen.add(k)
            used.append(orig)

    def _scan_geo_node_tree(nt, visited_trees: set[int], visited_obs: set[int]):
        if nt is None:
            return
        tid = rna_key(nt)
        if tid in visited_trees:
            return
        visited_trees.add(tid)
        for node in getattr(nt, "nodes", ()):
            bl = getattr(node, "bl_idname", "")
            if bl == "GeometryNodeGroup":
                _scan_geo_node_tree(getattr(node, "node_tree", None), visited_trees, visited_obs)
            elif bl == "GeometryNodeInputMaterial":
                _add_mat(getattr(node, "material", None))
            for inp in getattr(node, "inputs", ()):
                val = getattr(inp, "default_value", None)
                if val is None:
                    continue
                if isinstance(val, bpy.types.Material):
                    _add_mat(val)
                elif isinstance(val, bpy.types.Object):
                    _scan_object(val, visited_obs, visited_trees)
                elif isinstance(val, bpy.types.Collection):
                    for col_ob in getattr(val, "all_objects", ()):
                        _scan_object(col_ob, visited_obs, visited_trees)

    def _scan_object(ob, visited_obs: set[int], visited_trees: set[int]):
        if ob is None or _in_skipped_collection(ob):
            return
        oid = rna_key(ob)
        if oid in visited_obs:
            return
        visited_obs.add(oid)
        for slot in getattr(ob, "material_slots", ()):
            _add_mat(getattr(slot, "material", None))
        data = getattr(ob, "data", None)
        if data is not None:
            for m in getattr(data, "materials", ()):
                _add_mat(m)
        if getattr(ob, "instance_type", "") == "COLLECTION":
            inst_col = getattr(ob, "instance_collection", None)
            if inst_col is not None:
                for col_ob in getattr(inst_col, "all_objects", ()):
                    _scan_object(col_ob, visited_obs, visited_trees)
        for psys in getattr(ob, "particle_systems", ()):
            settings = getattr(psys, "settings", None)
            if settings is None:
                continue
            inst_ob = getattr(settings, "instance_object", None)
            if inst_ob is not None:
                _scan_object(inst_ob, visited_obs, visited_trees)
            inst_col = getattr(settings, "instance_collection", None)
            if inst_col is not None:
                for col_ob in getattr(inst_col, "all_objects", ()):
                    _scan_object(col_ob, visited_obs, visited_trees)
        for mod in getattr(ob, "modifiers", ()):
            if getattr(mod, "type", "") != "NODES" or not getattr(mod, "show_render", True):
                continue
            try:
                for val in mod.values():
                    if isinstance(val, bpy.types.Material):
                        _add_mat(val)
                    elif isinstance(val, bpy.types.Object):
                        _scan_object(val, visited_obs, visited_trees)
                    elif isinstance(val, bpy.types.Collection):
                        for col_ob in getattr(val, "all_objects", ()):
                            _scan_object(col_ob, visited_obs, visited_trees)
            except (AttributeError, TypeError):
                pass
            _scan_geo_node_tree(getattr(mod, "node_group", None), visited_trees, visited_obs)

    if depsgraph is not None:
        for inst in getattr(depsgraph, "object_instances", ()):
            is_instance = (
                getattr(inst, "is_instance", False)
                and getattr(inst, "instance_object", None) is not None
            )
            base_ob = inst.instance_object if is_instance else getattr(inst, "object", None)
            if base_ob is None:
                continue
            orig_ob = getattr(base_ob, "original", base_ob)
            if not is_instance and getattr(orig_ob, "hide_render", False):
                continue
            if _in_skipped_collection(orig_ob):
                continue
            if getattr(orig_ob, "type", "") not in {"MESH", "CURVE", "SURFACE", "FONT", "META"}:
                continue
            eval_ob = getattr(inst, "object", None)
            if eval_ob is None:
                continue
            for slot in getattr(eval_ob, "material_slots", ()):
                _add_mat(getattr(slot, "material", None))
            data = getattr(eval_ob, "data", None)
            if data is not None:
                for m in getattr(data, "materials", ()):
                    _add_mat(m)
        return used

    visited_obs: set[int] = set()
    visited_trees: set[int] = set()
    for ob in getattr(scene, "objects", ()):
        if getattr(ob, "hide_render", False) or _in_skipped_collection(ob):
            continue
        _scan_object(ob, visited_obs, visited_trees)
    return used


def materials_needing_bake(scene, depsgraph=None) -> list:
    """Single source of truth for both ``engine.render()`` and
    ``ui.RISE_RENDER_PT_materials``: returns render-used materials that
    currently require a bake attempt (never gating unused datablocks)."""
    return [
        mat for mat in collect_used_render_materials(scene, depsgraph=depsgraph)
        if needs_bake_attempt(scene, mat)
    ]


def baked_cache_is_stale(material) -> bool:
    """UI diagnostic for a stored success fingerprint or recorded file.

    With a success hash, True means the limited current fingerprint differs
    or a recorded channel file is missing. Without a success hash, False.
    False does not prove shader freshness: omitted dependencies can change.
    This neither enforces export freshness nor starts an automatic bake;
    the render guard separately compares the attempted fingerprint.
    """

    if material is None:
        return False
    cached_hash = material.get(BAKED_GRAPH_HASH_KEY)
    if not cached_hash:
        return False  # never baked → not stale (just absent)
    if str(cached_hash) != _material_graph_hash(material):
        return True
    # Hash matches; verify each cached PNG still exists on disk.  A
    # missing recorded file marks the UI diagnostic stale, not an export gate.
    for key in (BAKED_DIFFUSE_KEY, BAKED_ROUGHNESS_KEY, BAKED_NORMAL_KEY):
        path = material.get(key)
        if path and not os.path.isfile(str(path)):
            return True
    return False


# ---------------------------------------------------------------------------
# Bake driver
# ---------------------------------------------------------------------------

def _ensure_uv_layer(obj) -> bool:
    """Ensure ``obj`` has at least one UV layer suitable for baking.
    Returns True if UVs are present (either pre-existing or generated
    via Smart UV Project here), False if we can't make UVs.

    Side effect: may add a new UV layer named ``rise_bake_uv`` and
    leave it active.  We deliberately don't roll this back — once UVs
    are created they stay, which is what the user wants for repeated
    re-bakes of the same scene.
    """

    me = getattr(obj, "data", None)
    if me is None or not hasattr(me, "uv_layers"):
        return False
    if len(me.uv_layers) > 0:
        return True

    # Unwrap.  Smart UV Project is the most general option for opaque
    # solids with no pre-existing UVs.  Requires EDIT mode on the object.
    try:
        prev_active = bpy.context.view_layer.objects.active
        prev_selected = list(bpy.context.selected_objects)
        bpy.ops.object.select_all(action="DESELECT")
        obj.select_set(True)
        bpy.context.view_layer.objects.active = obj
        bpy.ops.object.mode_set(mode="EDIT")
        bpy.ops.mesh.select_all(action="SELECT")
        bpy.ops.uv.smart_project(angle_limit=math.radians(66.0), island_margin=0.02)
        bpy.ops.object.mode_set(mode="OBJECT")
        # Restore previous selection / active.
        bpy.ops.object.select_all(action="DESELECT")
        for o in prev_selected:
            o.select_set(True)
        bpy.context.view_layer.objects.active = prev_active
        return len(me.uv_layers) > 0
    except (RuntimeError, AttributeError) as exc:
        print(f"[RISE bake] Failed to unwrap {obj.name}: {exc}")
        return False


def _bake_one_channel(
    obj,
    material,
    bake_type: str,
    image,
    margin: int,
    pass_filter: Optional[set[str]],
) -> bool:
    """Bake a single channel ('DIFFUSE' / 'ROUGHNESS' / 'NORMAL' / etc.)
    into ``image``.  Returns True on success, False on failure.

    Expects:
    - obj is the active object, selected, in OBJECT mode
    - material is in the object's material slots
    - material has an Image Texture node selected and active pointing
      at ``image`` (the bake operator writes to that node's image)
    - scene render engine is CYCLES

    Caller is responsible for adding/removing the temporary Image
    Texture node and restoring the previous active node afterwards.
    """

    # Find or add the Image Texture node and make it active.  The bake
    # operator targets the active image node on each selected material.
    nt = material.node_tree
    if nt is None:
        return False
    tex_node = next(
        (n for n in nt.nodes if n.bl_idname == "ShaderNodeTexImage"
         and n.image == image),
        None,
    )
    if tex_node is None:
        tex_node = nt.nodes.new(type="ShaderNodeTexImage")
        tex_node.image = image
        tex_node.name = "_RISE_BAKE_TARGET"
        tex_node.label = "RISE bake target"
        tex_node.location = (-800, -400)
    # Make it the active node (Blender requires this for bake target).
    nt.nodes.active = tex_node

    bake_kwargs = {
        "type": bake_type,
        "margin": margin,
        "use_clear": True,
        "use_selected_to_active": False,
        "save_mode": "INTERNAL",
    }
    if bake_type == "NORMAL":
        bake_kwargs["normal_space"] = "TANGENT"
    if pass_filter is not None:
        bake_kwargs["pass_filter"] = pass_filter

    try:
        result = bpy.ops.object.bake(**bake_kwargs)
        return "FINISHED" in result
    except (RuntimeError, TypeError) as exc:
        print(f"[RISE bake] bake({bake_type}) failed for {material.name}: {exc}")
        return False


def _save_image_to_temp_png(image, out_path: str) -> bool:
    """Save the ``Image`` data block to ``out_path`` as PNG.  Returns
    True on success.

    Sets ``filepath_raw`` and PNG format, then calls ``Image.save()``.
    """
    try:
        image.filepath_raw = out_path
        image.file_format = "PNG"
        image.save()
        return os.path.isfile(out_path)
    except RuntimeError as exc:
        print(f"[RISE bake] save_to_png({out_path}) failed: {exc}")
        return False


def bake_material_to_images(
    obj,
    material,
    *,
    resolution: int = 1024,
    out_dir: Optional[str] = None,
    bake_roughness: bool = True,
    bake_normal: bool = True,
) -> dict[str, str]:
    """Bake ``material`` on ``obj`` to PNG files.  Updates the
    material's ID properties for newly saved channels and returns only
    those channels. Failed channels do not clear previous path metadata, so
    this result can differ from :func:`baked_paths`. This approximates three
    channels, not the complete BSDF or its view-dependent lobes.

    Requires:
    - The scene's render engine is CYCLES (caller switches and
      restores).
    - ``obj`` is selectable in the current view layer.

    Channels:
    - Diffuse COLOR pass only — always attempted; not lit reflectance or F0
    - Roughness — attempted when ``bake_roughness`` is True
    - Normal — tangent-space channel attempted when ``bake_normal`` is True

    The synthetic consumer requires an existing diffuse PNG, supplies metallic0,
    and drops transmission, sheen, coat, emission, subsurface and alpha lobes.
    Roughness defaults to0.5 if absent; normal is optional. The output PNGs
    are colour-space-tagged appropriately when read
    back via the bridge (sRGB for diffuse, Linear for roughness and
    normal).
    """

    if obj is None or material is None or material.node_tree is None:
        return {}

    if not _ensure_uv_layer(obj):
        print(f"[RISE bake] {obj.name} has no UV layer and unwrap failed; skipping bake")
        return {}

    # Capture the graph hash BEFORE we add any temporary bake-target
    # nodes — `_bake_one_channel` inserts a `_RISE_BAKE_TARGET` Image
    # Texture node into the tree, and the `finally` block below
    # removes it.  If we hashed after the bake (with the temp node
    # still in place) the stored hash would reflect a transient
    # graph state, and `baked_cache_is_stale` would always return
    # True on the next render because the live graph (cleaned up)
    # hashes differently.  Capture it here, commit it after cleanup.
    pre_bake_hash = _material_graph_hash(material)

    if out_dir is None:
        out_dir = bpy.app.tempdir or tempfile.gettempdir()
    out_dir = os.path.join(out_dir, "rise_baked")
    os.makedirs(out_dir, exist_ok=True)

    # Make obj the active+selected; remember prior state to restore.
    #
    # The supported caller is the explicit user operator, outside a live
    # render callback. Snapshot selection here and restore it on exit.
    # Restricted-context reads historically failed in the abandoned
    # render-handler experiment; these defensive fallbacks do not make
    # that old automatic workflow supported.
    try:
        prev_active = bpy.context.view_layer.objects.active
        prev_selected = list(bpy.context.selected_objects)
        prev_mode = (
            bpy.context.object.mode if bpy.context.object is not None else "OBJECT"
        )
    except AttributeError:
        prev_active = None
        prev_selected = []
        prev_mode = "OBJECT"

    if prev_mode != "OBJECT":
        try:
            bpy.ops.object.mode_set(mode="OBJECT")
        except RuntimeError:
            pass
    try:
        bpy.ops.object.select_all(action="DESELECT")
    except RuntimeError:
        # A failed deselect is tolerated; the supported operator does not
        # install a context override or guarantee this fallback selects correctly.
        pass
    obj.select_set(True)
    # Setting active via the view layer directly (rather than
    # `bpy.context.view_layer.objects.active = obj`) avoids the
    # restricted-context attribute access.  Falls through gracefully
    # if no view layer is accessible.
    try:
        bpy.context.view_layer.objects.active = obj
    except AttributeError:
        # Tolerate missing active-object access; no caller-installed override
        # or successful bake in such a context is promised.
        pass

    # Remember the previously-active node on the material so we can
    # restore — bake hijacks .active.
    nt = material.node_tree
    prev_active_node = nt.nodes.active

    safe_name = "".join(
        c if c.isalnum() or c in ("_", "-") else "_"
        for c in material.name
    )

    out: dict[str, str] = {}
    images: list[bpy.types.Image] = []
    target_nodes: list[bpy.types.Node] = []

    try:
        # Diffuse COLOR pass; not a full visual-result or conductor-F0 bake.
        diffuse_img = bpy.data.images.new(
            f"_rise_bake_diffuse_{safe_name}",
            width=resolution,
            height=resolution,
            alpha=False,
            float_buffer=False,
        )
        diffuse_img.colorspace_settings.name = "sRGB"
        images.append(diffuse_img)
        if _bake_one_channel(
            obj,
            material,
            "DIFFUSE",
            diffuse_img,
            margin=8,
            pass_filter={"COLOR"},
        ):
            diffuse_path = os.path.join(out_dir, f"{safe_name}_diffuse.png")
            if _save_image_to_temp_png(diffuse_img, diffuse_path):
                out["diffuse"] = diffuse_path
                material[BAKED_DIFFUSE_KEY] = diffuse_path

        # Roughness.
        if bake_roughness:
            rough_img = bpy.data.images.new(
                f"_rise_bake_rough_{safe_name}",
                width=resolution,
                height=resolution,
                alpha=False,
                float_buffer=False,
            )
            rough_img.colorspace_settings.name = "Non-Color"
            images.append(rough_img)
            if _bake_one_channel(
                obj,
                material,
                "ROUGHNESS",
                rough_img,
                margin=8,
                pass_filter=None,
            ):
                rough_path = os.path.join(out_dir, f"{safe_name}_roughness.png")
                if _save_image_to_temp_png(rough_img, rough_path):
                    out["roughness"] = rough_path
                    material[BAKED_ROUGHNESS_KEY] = rough_path

        # Normal.
        if bake_normal:
            normal_img = bpy.data.images.new(
                f"_rise_bake_normal_{safe_name}",
                width=resolution,
                height=resolution,
                alpha=False,
                float_buffer=False,
            )
            normal_img.colorspace_settings.name = "Non-Color"
            images.append(normal_img)
            if _bake_one_channel(
                obj,
                material,
                "NORMAL",
                normal_img,
                margin=8,
                pass_filter=None,
            ):
                normal_path = os.path.join(out_dir, f"{safe_name}_normal.png")
                if _save_image_to_temp_png(normal_img, normal_path):
                    out["normal"] = normal_path
                    material[BAKED_NORMAL_KEY] = normal_path

        material[BAKED_RESOLUTION_KEY] = resolution
        material[BAKED_FRAME_KEY] = int(bpy.context.scene.frame_current)

    finally:
        # Clean up temp bake-target nodes we added.  Leaving them in
        # the graph would confuse subsequent renders (the active node
        # would be the bake target on every render).
        for node in list(nt.nodes):
            if node.name.startswith("_RISE_BAKE_TARGET"):
                target_nodes.append(node)
        for node in target_nodes:
            nt.nodes.remove(node)
        if prev_active_node is not None and prev_active_node.name in nt.nodes:
            nt.nodes.active = prev_active_node

        # Restore the explicit caller's selection/active state; tolerate
        # unavailable contexts during teardown. No automatic handler
        # or caller-installed override is assumed.
        try:
            bpy.ops.object.select_all(action="DESELECT")
            for o in prev_selected:
                if o.name in bpy.context.view_layer.objects:
                    o.select_set(True)
            bpy.context.view_layer.objects.active = prev_active
        except (RuntimeError, AttributeError):
            pass

        # Keep channel image datablocks, including failed targets, for inspection.
        # Successfully saved PNGs persist separately; removing a datablock
        # would not delete its file. No successful image is implied here.

    # Stamp after the channel stage and cleanup, even if no channel saved.
    # Early returns/exceptions before this point can leave the guard pending.
    # Failed retries do not clear prior path/success metadata: existing diffuse
    # may still select the synthetic approximation, otherwise export uses its
    # existing translation/fallback. This marker prevents repeat gating only
    # while the limited fingerprint matches, not after every shader edit.
    material[BAKED_ATTEMPTED_HASH_KEY] = pre_bake_hash

    # Commit the success-hash AFTER the finally block has restored
    # the node tree to its pre-bake shape, but use the value we
    # captured at function entry.  Either order would produce the
    # same string in the no-temp-node case; we use `pre_bake_hash`
    # so a future refactor that leaves residual nodes (e.g. a stuck
    # UV-unwrap helper) doesn't silently invalidate the cache on
    # every render.
    if out:
        material[BAKED_GRAPH_HASH_KEY] = pre_bake_hash

    return out


# ---------------------------------------------------------------------------
# High-level entry point for the operator
# ---------------------------------------------------------------------------

def find_bakeable_proxy_object(scene, material):
    """Return the first MESH object in ``scene`` that has ``material``
    in one of its material slots AND has actual mesh geometry (NOT
    geometry-nodes-generated).  Returns None if no suitable proxy
    found.

    Blender's ``bpy.ops.object.bake`` operates on the EVALUATED mesh
    but writes UV coordinates from the ORIGINAL mesh data.  Objects
    whose visible geometry comes entirely from a Geometry Nodes
    modifier (no static vertices) have no UV layout to bake against
    — those return None here so callers can skip them with a
    diagnostic rather than failing inside the bake operator.

    Also used by ``engine.render()`` to decide whether a complex
    material is *bakeable* before deciding to abort the render with a
    "please bake" error.  Materials with no viable proxy fall through
    without gating render: existing diffuse cache may still be consumed,
    otherwise the existing translation/slot/default fallback applies.
    """

    for obj in scene.objects:
        if obj.type != "MESH" or obj.data is None:
            continue
        if material is None:
            continue
        if material.name not in obj.data.materials.keys():
            continue
        # Bake needs an actual non-geometry-nodes mesh with vertices.
        # Geometry-nodes-only objects have len(verts) == 0 on the
        # source mesh.
        if len(obj.data.vertices) == 0:
            continue
        return obj
    return None


# Procedural texture nodes recognized as bake-eligible when the user
# explicitly invokes the "Bake Procedural Materials" operator, even when
# `classify_material` keeps the material `"simple"` so F12 can render
# natively (e.g. Voronoi / Noise / White Noise via expression_painter)
# without blocking on a bake gate.
_PROCEDURAL_TEXTURE_NODES = frozenset({
    "ShaderNodeTexNoise",
    "ShaderNodeTexVoronoi",
    "ShaderNodeTexWhiteNoise",
    "ShaderNodeTexChecker",
    "ShaderNodeTexWave",
    "ShaderNodeTexMagic",
    "ShaderNodeTexBrick",
    "ShaderNodeTexGradient",
    "ShaderNodeTexGabor",
})


def has_procedural_texture_nodes(material) -> bool:
    """Return True if any active procedural texture node is reachable from the
    material's selected Material Output (excluding the Tangent branch)."""
    if material is None or not uses_nodes(material):
        return False
    output_node = find_material_output(material)
    if output_node is None:
        return False
    surface_socket = output_node.inputs.get("Surface")
    if surface_socket is None or not surface_socket.is_linked:
        return False
    surface_links = [l for l in surface_socket.links if not getattr(l, "is_muted", False)]
    if not surface_links:
        return False

    stack = [surface_links[0].from_node]
    visited: set[int] = set()
    while stack:
        node = stack.pop()
        if node is None:
            continue
        node_id = rna_key(node)
        if node_id in visited:
            continue
        visited.add(node_id)
        is_muted = bool(getattr(node, "mute", False))
        if not is_muted and node.bl_idname in _PROCEDURAL_TEXTURE_NODES:
            return True
        for socket in node.inputs:
            if node.bl_idname == "ShaderNodeBsdfPrincipled" and socket.name == "Tangent":
                continue
            if socket.is_linked:
                for link in socket.links:
                    if not getattr(link, "is_muted", False):
                        stack.append(link.from_node)
                        break
    return False


def bake_complex_materials_in_scene(
    scene,
    *,
    resolution: int = 1024,
    only_unbaked: bool = False,
    report=print,
) -> int:
    """Walk the scene, find every object whose material classifies as
    complex OR contains active procedural texture nodes (e.g. Voronoi,
    Noise, White Noise, Checker), once per material, using the first
    encountered MESH object.
    Returns the number with any newly saved channel, not necessarily diffuse
    or a complete three-channel cache. A zero count may mean no eligible
    material, all skipped, or all attempts failed.

    ``only_unbaked``: when True, skip materials with any existing
    channel returned by ``baked_paths``. This does not check graph
    freshness; False retries eligible materials regardless of cache.

    The scene's render engine MUST be switched to ``CYCLES`` by the
    caller before invoking; the operator handles that.  We don't do
    it here so this function is testable in isolation.
    """

    seen: set[str] = set()
    baked = 0
    for obj in scene.objects:
        if obj.type != "MESH":
            continue
        for slot in obj.material_slots:
            mat = slot.material
            if mat is None:
                continue
            if mat.name in seen:
                continue
            seen.add(mat.name)
            if classify_material(mat) != "complex" and not has_procedural_texture_nodes(mat):
                continue
            if only_unbaked and baked_paths(mat) is not None:
                continue
            report(f"[RISE bake] {obj.name} / {mat.name} — baking")
            result = bake_material_to_images(obj, mat, resolution=resolution)
            if result:
                report(
                    f"[RISE bake] {mat.name}: "
                    + ", ".join(f"{k}={os.path.basename(v)}" for k, v in result.items())
                )
                baked += 1
            else:
                report(f"[RISE bake] {mat.name}: no new channels saved; existing diffuse cache or translation/fallback will be used")
    return baked
