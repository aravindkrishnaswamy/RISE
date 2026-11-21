"""Survey which shader nodes real Blender materials use, and how many of
them each phase of docs/BLENDER_PROCEDURAL_TRANSLATION_DESIGN.md would
translate.  The report counts coverage three ways: the graph translates
under the path tracer; it translates AND every lobe matches Cycles; it
translates under the path tracer, BDPT and VCM alike (no path-tracer-only
input; MLT does not take the new materials at all).

Three modes, one file:

  collect (runs INSIDE Blender, once per .blend):
      blender -b --factory-startup --disable-autoexec scene.blend \\
          --python tools/blender_node_survey.py -- out.json

  run (plain Python; drives `collect` over many files, one at a time):
      python3 tools/blender_node_survey.py run --blender <Blender binary> \\
          --out <json dir> a.blend b.blend ...

  report (plain Python; aggregates the JSON files):
      python3 tools/blender_node_survey.py report <json dir>

A material is described by a set of FEATURE KEYS collected from the nodes
reachable from its active Material Output: one `node:<type>` key per node
type plus keys for the options that decide translatability (noise type,
Math operation, which coordinate output is used, what feeds a Bump, the
shader-mix pairs ...).  `CAPABILITY_RULES` below assigns each key the unit
of work that supports it and `PLAN` assigns those units to phases; a
material is covered once every capability it needs exists.  A key no rule
matches is "not-planned", never silently counted as supported.
`PATH_TRACER_ONLY` names the capabilities defined only along a path from
the camera, and `INEXACT_RULES` the lobe options RISE renders with a
different model than Cycles.

.blend files are opened with --disable-autoexec: they are untrusted data.
"""

from __future__ import annotations

import collections
import json
import os
import re
import subprocess
import sys

# ---------------------------------------------------------------------------
# collect (inside Blender)
# ---------------------------------------------------------------------------

_PROCEDURAL_TEXTURES = {
    "ShaderNodeTexNoise", "ShaderNodeTexVoronoi", "ShaderNodeTexGradient",
    "ShaderNodeTexBrick", "ShaderNodeTexWave", "ShaderNodeTexMagic",
    "ShaderNodeTexChecker", "ShaderNodeTexGabor",
}   # textures whose unlinked Vector reads Generated.  White Noise is not one:
#     its unlinked Vector is the socket's own constant.
_SKIP_NODES = {"NodeFrame", "NodeReroute", "NodeGroupInput", "NodeGroupOutput"}
_PLAIN_UV_CHAIN = {"ShaderNodeMapping", "ShaderNodeTexCoord", "ShaderNodeUVMap", "NodeReroute"}


def _short(idname):
    return idname[len("ShaderNode"):] if idname.startswith("ShaderNode") else idname


def _source(socket):
    """Node feeding `socket`, looking through reroutes (None if unlinked)."""
    seen = 0
    while socket is not None and socket.is_linked and seen < 64:
        link = socket.links[0]
        node = link.from_node
        if node.bl_idname != "NodeReroute":
            return node, link.from_socket
        socket = node.inputs[0]
        seen += 1
    return None, None


def _reachable(tree, roots):
    """Nodes reachable upstream of `roots` within one tree."""
    seen, stack = [], list(roots)
    ids = set()
    while stack:
        node = stack.pop()
        if node.as_pointer() in ids:
            continue
        ids.add(node.as_pointer())
        seen.append(node)
        for sock in node.inputs:
            for link in sock.links:
                stack.append(link.from_node)
    return seen


def _upstream_types(socket, limit=64):
    """bl_idnames of every node upstream of `socket` (same tree)."""
    node, _ = _source(socket)
    if node is None:
        return set()
    return {n.bl_idname for n in _reachable(node.id_data, [node])[:limit]}


def _nonzero(node, name):
    """True when input `name` is linked or its constant is above zero."""
    sock = node.inputs.get(name)
    if sock is None:
        return False
    return sock.is_linked or float(sock.default_value) > 1e-6


def _tinted(node, name):
    """True when colour input `name` is linked or not white."""
    sock = node.inputs.get(name)
    if sock is None:
        return False
    return sock.is_linked or min(sock.default_value[:3]) < 0.999


_MIX_NODES = ("ShaderNodeMixShader", "ShaderNodeAddShader")
_MAX_GROUP_DEPTH = 8


def _ctx_key(ctx):
    return tuple(group.as_pointer() for group in ctx)


def _source_ctx(socket, ctx):
    """Like `_source`, but crosses node-group boundaries.  `ctx` is the stack
    of group nodes the walk is inside (outermost first).  Returns (node,
    from_socket, ctx of that node).  A group node or group input is returned
    as the source only when it cannot be resolved: a group with no tree or no
    output, or a group input reached with no caller on the stack."""
    for _ in range(256):
        if socket is None or not socket.is_linked:
            return None, None, ctx
        link = socket.links[0]
        node, out = link.from_node, link.from_socket
        b = node.bl_idname
        if b == "NodeReroute":
            socket = node.inputs[0]
        elif b == "ShaderNodeGroup":
            inner = node.node_tree
            outputs = [n for n in inner.nodes if n.bl_idname == "NodeGroupOutput"] if inner is not None else []
            outputs = [n for n in outputs if n.is_active_output] or outputs
            inside = next((s for s in outputs[0].inputs if s.identifier == out.identifier), None) if outputs else None
            if inside is None or len(ctx) >= _MAX_GROUP_DEPTH:
                return node, out, ctx
            socket, ctx = inside, ctx + (node,)
        elif b == "NodeGroupInput":
            if not ctx:
                return node, out, ctx
            socket = next((s for s in ctx[-1].inputs if s.identifier == out.identifier), None)
            ctx = ctx[:-1]
        else:
            return node, out, ctx
    return None, None, ctx


def _targets(output_socket, ctx, limit=512):
    """Immediate consumers of an output socket, as (node, input socket, ctx),
    looking through reroutes and node-group boundaries.  A group output
    reached with no caller on the stack is returned as the consumer."""
    found, stack, seen = [], [(output_socket, ctx)], set()
    while stack and len(seen) < limit:
        socket, c = stack.pop()
        for link in socket.links:
            node, target = link.to_node, link.to_socket
            key = (node.as_pointer(), target.identifier, _ctx_key(c))
            if key in seen:
                continue
            seen.add(key)
            b = node.bl_idname
            if b == "NodeReroute":
                stack.append((node.outputs[0], c))
            elif b == "NodeGroupOutput":
                if not c:
                    found.append((node, target, c))
                    continue
                outer = next((o for o in c[-1].outputs if o.identifier == target.identifier), None)
                if outer is not None:
                    stack.append((outer, c[:-1]))
            elif b == "ShaderNodeGroup" and node.node_tree is not None and len(c) < _MAX_GROUP_DEPTH:
                for inner in node.node_tree.nodes:
                    if inner.bl_idname != "NodeGroupInput":
                        continue
                    for o in inner.outputs:
                        if o.identifier == target.identifier and o.is_linked:
                            stack.append((o, c + (node,)))
            else:
                found.append((node, target, c))
    return found


def _leaves_under(node, ctx, limit=64):
    """Leaf shader nodes at or under `node`, as (leaf, ctx), expanding nested
    Mix / Add Shader nodes across group boundaries.  An unlinked shader input
    contributes nothing."""
    leaves, stack, seen = [], [(node, ctx)], set()
    while stack and len(seen) < limit:
        current, c = stack.pop()
        key = (current.as_pointer(), _ctx_key(c))
        if key in seen:
            continue
        seen.add(key)
        if current.bl_idname not in _MIX_NODES:
            leaves.append((current, c))
            continue
        first = 1 if current.bl_idname == "ShaderNodeMixShader" else 0
        for index in (first, first + 1):
            src, _, src_ctx = _source_ctx(current.inputs[index], c)
            if src is not None:
                stack.append((src, src_ctx))
    return leaves


def _normal_source(node, ctx):
    """What feeds a shader's Normal input: None when the shader has no such
    input, "" when it is unlinked, else an id for the driving socket."""
    sock = node.inputs.get("Normal")
    if sock is None:
        return None
    src, out, src_ctx = _source_ctx(sock, ctx)
    if src is None:
        return ""
    return "%s:%d:%s" % (_ctx_key(src_ctx), src.as_pointer(), out.identifier)


def _layered_kinds(leaf):
    """Which layered or subsurface models a leaf shader needs in RISE."""
    b = leaf.bl_idname
    kinds = []
    if b == "ShaderNodeBsdfPrincipled":
        if _nonzero(leaf, "Coat Weight"):
            kinds.append("coat")
        if _nonzero(leaf, "Sheen Weight"):
            kinds.append("sheen")
        if _nonzero(leaf, "Subsurface Weight"):
            kinds.append("subsurface")
    elif b == "ShaderNodeBsdfSheen":
        kinds.append("sheen")
    elif b == "ShaderNodeSubsurfaceScattering":
        kinds.append("subsurface")
    elif b.startswith("ShaderNodeBsdfHair"):
        kinds.append("hair")
    return kinds


_NORMAL_SHADERS = ("ShaderNodeBsdf", "ShaderNodeSubsurfaceScattering", "ShaderNodeEeveeSpecular",
                   "ShaderNodeBump", "ShaderNodeDisplacement")


def _normal_use(node, socket):
    """How a perturbed normal (a Bump or Normal Map output) is consumed.
    None: as a shading normal, which RISE expresses as a modifier.  "coat":
    a Principled coat normal.  "value:Node.Socket": anything else, where the
    perturbed normal has to exist as a VALUE in the program."""
    b = node.bl_idname
    if b == "NodeGroupOutput":
        return "unresolved"
    if socket.name == "Normal" and b.startswith(_NORMAL_SHADERS):
        return None
    if b == "ShaderNodeBsdfPrincipled" and socket.name in ("Coat Normal", "Clearcoat Normal"):
        return "coat"
    return "value:%s.%s" % (_short(b), socket.name)


_SHADER_SINKS = ("ShaderNodeBsdf", "ShaderNodeEmission", "ShaderNodeMixShader", "ShaderNodeAddShader",
                 "ShaderNodeSubsurfaceScattering", "ShaderNodeEeveeSpecular", "ShaderNodeOutputMaterial",
                 "ShaderNodeBackground", "ShaderNodeVolume", "ShaderNodeHoldout", "ShaderNodeDisplacement",
                 "ShaderNodeBump", "ShaderNodeNormalMap")


def _sinks(output_socket, ctx, limit=256):
    """Where a value ends up: follow links DOWNSTREAM through converter nodes
    and node groups until a shader / bump input is reached.  Returns a set of
    "Node.Socket" strings; a Mix Shader factor also names the two shaders it
    switches between."""

    found, stack, seen = set(), [(output_socket, ctx)], set()
    while stack and len(seen) < limit:
        socket, c = stack.pop()
        for node, target, node_ctx in _targets(socket, c):
            key = (node.as_pointer(), target.identifier, _ctx_key(node_ctx))
            if key in seen:
                continue
            seen.add(key)
            b = node.bl_idname
            if b.startswith(_SHADER_SINKS):
                label = "%s.%s" % (_short(b), target.name)
                if b == "ShaderNodeMixShader" and target.name in ("Fac", "Factor"):
                    kinds = []
                    for index in (1, 2):
                        src, _, _c = _source_ctx(node.inputs[index], node_ctx)
                        kinds.append(_short(src.bl_idname) if src is not None else "none")
                    label += "[%s|%s]" % tuple(kinds)   # [factor 0 | factor 1]
                found.add(label)
            elif b == "NodeGroupOutput":
                found.add("(group output)")
            elif b == "ShaderNodeGroup":
                found.add("(into group)")
            else:
                stack.extend((o, node_ctx) for o in node.outputs if o.is_linked)
    return found



def _describe_node(node, features, state, ctx):
    b = node.bl_idname
    if b in _SKIP_NODES or b == "ShaderNodeOutputMaterial":
        return
    features.add("node:" + _short(b))
    state["nodes"] += 1
    linked_out = [o.name for o in node.outputs if o.is_linked]

    if b in _PROCEDURAL_TEXTURES:
        vector = node.inputs.get("Vector")
        if vector is not None and not vector.is_linked:
            features.add("coord:Generated(implicit)")
    if b == "ShaderNodeTexNoise":
        features.add("noise:%s:%s" % (node.noise_dimensions, getattr(node, "noise_type", "FBM")))
        distortion = node.inputs.get("Distortion")
        if distortion is not None and (distortion.is_linked or distortion.default_value != 0.0):
            features.add("noise:distortion")
        if "Color" in linked_out:
            features.add("noise:color_output")
    elif b == "ShaderNodeTexVoronoi":
        features.add("voronoi:%s:%s:%s" % (node.voronoi_dimensions, node.feature, node.distance))
        for name in linked_out:
            features.add("voronoi:out:" + name)
    elif b == "ShaderNodeTexWave":
        features.add("wave:%s:%s" % (node.wave_type, node.wave_profile))
    elif b == "ShaderNodeTexGradient":
        features.add("gradient:" + node.gradient_type)
    elif b == "ShaderNodeTexWhiteNoise":
        features.add("whitenoise:" + node.noise_dimensions)
    elif b == "ShaderNodeTexImage":
        image = node.image
        if image is None:
            features.add("image:none")
        elif image.source != "FILE":
            features.add("image:source:" + image.source)
        if not node.inputs["Vector"].is_linked:
            state["uv_maps"].add("")
        if node.projection != "FLAT":
            features.add("image:projection:" + node.projection)
        if node.interpolation != "Linear":
            features.add("image:interpolation:" + node.interpolation)
        if "Alpha" in linked_out:
            features.add("image:alpha_output")
        if node.extension != "REPEAT":
            features.add("image:extension:" + node.extension)
        upstream = _upstream_types(node.inputs["Vector"])
        if upstream - _PLAIN_UV_CHAIN == {"NodeGroupInput"}:
            # Inside a node group, fed from the group's own input: what the
            # caller wires there is not resolved by this survey.
            features.add("image:coords:via_group_input")
        elif upstream - _PLAIN_UV_CHAIN:
            features.add("image:coords:computed")
        else:
            src, _ = _source(node.inputs["Vector"])
            chain = [src] if src is not None else []
            while chain and chain[-1].bl_idname == "ShaderNodeMapping":
                nxt, sock = _source(chain[-1].inputs["Vector"])
                if nxt is None:
                    # A Mapping with no input transforms its own constant
                    # Vector: the image is read at one point.
                    features.add("image:coords:nonuv")
                    break
                chain.append(nxt)
                if nxt.bl_idname == "ShaderNodeTexCoord" and sock.name != "UV":
                    features.add("image:coords:nonuv")
            if src is not None and src.bl_idname == "ShaderNodeTexCoord":
                _, sock = _source(node.inputs["Vector"])
                if sock.name != "UV":
                    features.add("image:coords:nonuv")
    elif b == "ShaderNodeMapping":
        features.add("mapping:" + node.vector_type)
        if any(node.inputs[k].is_linked for k in ("Location", "Rotation", "Scale") if k in node.inputs):
            features.add("mapping:linked_trs")
        rotation = node.inputs.get("Rotation")
        if rotation is not None and not rotation.is_linked and (
                abs(rotation.default_value[0]) > 1e-6 or abs(rotation.default_value[1]) > 1e-6):
            features.add("mapping:rotation_xy")
    elif b == "ShaderNodeTexCoord":
        for name in linked_out:
            features.add("coord:" + name)
        if "UV" in linked_out:
            state["uv_maps"].add("")
        if node.object is not None:
            features.add("coord:object_pointer")
        if node.from_instancer:
            features.add("coord:from_instancer")
    elif b == "ShaderNodeUVMap":
        state["uv_maps"].add(node.uv_map or "")
        if node.from_instancer:
            features.add("coord:from_instancer")
    elif b == "ShaderNodeNewGeometry":
        for name in linked_out:
            features.add("geom:" + name)
    elif b == "ShaderNodeObjectInfo":
        for name in linked_out:
            features.add("objinfo:" + name)
    elif b == "ShaderNodeAttribute":
        features.add("attr:" + node.attribute_type)
        if node.attribute_type == "GEOMETRY":
            state["vertex_attrs"].add(node.attribute_name)
    elif b == "ShaderNodeVertexColor":
        state["vertex_attrs"].add("color:" + node.layer_name)
    elif b == "ShaderNodeLightPath":
        for output in node.outputs:
            if not output.is_linked:
                continue
            features.add("lightpath:" + output.name)
            for sink in _sinks(output, ctx):
                features.add("lightpath:%s=>%s" % (output.name, sink))
    elif b == "ShaderNodeAmbientOcclusion":
        features.add("ao:samples:%d" % node.samples)
        if node.inside:
            features.add("ao:inside")
        if node.only_local:
            features.add("ao:only_local")
        distance = node.inputs["Distance"]
        features.add("ao:distance:linked" if distance.is_linked else "ao:distance:%g" % round(distance.default_value, 3))
        if node.inputs["Normal"].is_linked:
            features.add("ao:normal_linked")
        else:
            # Cycles then uses the shading normal facing the arriving ray.
            features.add("ao:default_normal")
        if node.inputs["Color"].is_linked:
            features.add("ao:color_linked")
        for output in node.outputs:
            for sink in (_sinks(output, ctx) if output.is_linked else ()):
                features.add("ao:%s=>%s" % (output.name, sink))
    elif b == "ShaderNodeMath":
        features.add("math:" + node.operation)
    elif b == "ShaderNodeVectorMath":
        features.add("vmath:" + node.operation)
    elif b == "ShaderNodeMix":
        features.add("mix:%s:%s" % (node.data_type, node.blend_type if node.data_type == "RGBA" else "MIX"))
    elif b == "ShaderNodeMixRGB":
        features.add("mix:RGBA:" + node.blend_type)
    elif b == "ShaderNodeMapRange":
        features.add("maprange:%s:%s" % (node.data_type, node.interpolation_type))
    elif b == "ShaderNodeValToRGB":
        ramp = node.color_ramp
        features.add("ramp:%s:%s" % (ramp.interpolation, ramp.color_mode))
    elif b == "ShaderNodeBump":
        src, _ = _source(node.inputs["Height"])
        features.add("bump:height<-" + (_short(src.bl_idname) if src is not None else "none"))
        if node.inputs["Normal"].is_linked:
            features.add("bump:chained")
        if any(node.inputs[k].is_linked for k in ("Strength", "Distance") if k in node.inputs):
            features.add("bump:linked_strength")
        for target, socket, _c in _targets(node.outputs["Normal"], ctx):
            use = _normal_use(target, socket)
            if use is not None:
                features.add("bumpout:%s<-Bump" % use)
    elif b == "ShaderNodeNormalMap":
        features.add("normalmap:" + node.space)
        if node.inputs["Strength"].is_linked:
            features.add("normalmap:linked_strength")
        for target, socket, _c in _targets(node.outputs["Normal"], ctx):
            use = _normal_use(target, socket)
            if use is not None:
                features.add("bumpout:%s<-NormalMap" % use)
        color_source, _ = _source(node.inputs["Color"])
        if color_source is not None and color_source.bl_idname != "ShaderNodeTexImage":
            features.add("bump:normalmap_from_graph")
        state["uv_maps"].add(node.uv_map or "")
    elif b in ("ShaderNodeMixShader", "ShaderNodeAddShader"):
        first = 1 if b == "ShaderNodeMixShader" else 0
        kinds = []
        sides = []
        for index in (first, first + 1):
            src, _, src_ctx = _source_ctx(node.inputs[index], ctx)
            kinds.append(_short(src.bl_idname) if src is not None else "none")
            sides.append(_leaves_under(src, src_ctx) if src is not None else [])
        features.add("%s:%s" % ("mixshader" if first else "addshader", "+".join(sorted(kinds))))
        if first:
            features.add("mixshader:fac_linked" if node.inputs[0].is_linked else "mixshader:fac_const")
        leaves = sides[0] + sides[1]
        # Leaves whose Normal inputs are driven differently: one object-bound
        # modifier cannot express them; each leaf needs its own.
        normals = {n for n in (_normal_source(leaf, c) for leaf, c in leaves) if n is not None}
        if len(normals) > 1:
            features.add("mixshader:leaf_normals_differ")
        # Group boundaries are resolved by the walk; what is left is a group
        # with no tree, or a group input with no caller.
        if any(leaf.bl_idname in ("ShaderNodeGroup", "NodeGroupInput") for leaf, _c in leaves):
            features.add("mixshader:leaf_unresolved")
        # A subsurface leaf under a weight that varies over the surface.
        if first and node.inputs[0].is_linked and any("subsurface" in _layered_kinds(leaf) for leaf, _c in leaves):
            features.add("mixshader:varying_subsurface")
        # An Add of two surfaces (not surface + Emission) is a gain mix; its
        # leaves are wrapped in a scale, which not every material admits.
        surfaces = [[leaf for leaf, _c in side if leaf.bl_idname not in ("ShaderNodeEmission", "ShaderNodeBackground")]
                    for side in sides]
        if not first and surfaces[0] and surfaces[1]:
            features.add("addshader:gain")
            for leaf in surfaces[0] + surfaces[1]:
                for kind in _layered_kinds(leaf):
                    features.add("addshader:gain_leaf:" + kind)
    elif b == "ShaderNodeBsdfPrincipled":
        state["principled"] += 1
        for sock in node.inputs:
            if sock.is_linked:
                features.add("principled_in:" + sock.name)
        # Constant-only settings that select a lobe: invisible to the
        # linked-socket keys above.
        if _nonzero(node, "Subsurface Weight"):
            features.add("principled_opt:subsurface")
        if _nonzero(node, "Transmission Weight"):
            features.add("principled_opt:transmission")
            if _nonzero(node, "Roughness"):
                features.add("principled_opt:rough_transmission")
    elif b in ("ShaderNodeBsdfGlossy", "ShaderNodeBsdfAnisotropic", "ShaderNodeBsdfGlass",
               "ShaderNodeBsdfRefraction"):
        short = _short(b)
        features.add("bsdfopt:%s:dist:%s" % (short, getattr(node, "distribution", "?")))
        if _nonzero(node, "Roughness"):
            features.add("bsdfopt:%s:rough" % short)
        if _tinted(node, "Color"):
            features.add("bsdfopt:%s:tinted" % short)
    elif b == "ShaderNodeBsdfDiffuse":
        if _nonzero(node, "Roughness"):
            features.add("bsdfopt:BsdfDiffuse:rough")
    elif b == "ShaderNodeBsdfSheen":
        features.add("bsdfopt:BsdfSheen:dist:%s" % getattr(node, "distribution", "?"))
    elif b == "ShaderNodeBsdfMetallic":
        features.add("bsdfopt:BsdfMetallic:dist:%s" % getattr(node, "distribution", "?"))
        features.add("bsdfopt:BsdfMetallic:fresnel:%s" % getattr(node, "fresnel_type", "?"))
        if _tinted(node, "Edge Tint"):
            features.add("bsdfopt:BsdfMetallic:edge_tint")
    elif b == "ShaderNodeBsdfTransparent":
        if _tinted(node, "Color"):
            features.add("bsdfopt:BsdfTransparent:tinted")
    elif b == "ShaderNodeSubsurfaceScattering":
        features.add("bsdfopt:SubsurfaceScattering:method:%s" % getattr(node, "falloff", "?"))
        if node.inputs["Color"].is_linked:
            features.add("bsdfopt:SubsurfaceScattering:color_linked")
    elif b == "ShaderNodeVectorTransform":
        features.add("vtransform:%s:%s>%s" % (node.vector_type, node.convert_from, node.convert_to))


def _walk_tree(tree, roots, features, state, ctx=()):
    for node in _reachable(tree, roots):
        if node.bl_idname == "ShaderNodeGroup":
            features.add("group")
            inner = node.node_tree
            if inner is not None and len(ctx) < _MAX_GROUP_DEPTH:
                outs = [n for n in inner.nodes if n.bl_idname == "NodeGroupOutput"]
                _walk_tree(inner, outs, features, state, ctx + (node,))
            continue
        try:
            _describe_node(node, features, state, ctx)
        except Exception as exc:  # a node API that moved between versions
            features.add("survey_error:%s:%s" % (_short(node.bl_idname), type(exc).__name__))


def _collect(out_path):
    import bpy

    # material name -> for each mesh using it in a slot, (render UV map
    # name, all UV map names).  Materials assigned only by geometry nodes
    # have no entry: their UV maps cannot be resolved here.
    slot_materials = collections.defaultdict(list)
    for obj in bpy.data.objects:
        layers = getattr(getattr(obj, "data", None), "uv_layers", None)
        for slot in getattr(obj, "material_slots", []):
            if slot.material is None:
                continue
            if layers is None:
                slot_materials[slot.material.name]
                continue
            render = next((layer.name for layer in layers if layer.active_render), "")
            slot_materials[slot.material.name].append((render, [layer.name for layer in layers]))

    materials = []
    for material in bpy.data.materials:
        tree = material.node_tree
        if tree is None or material.users == 0 or getattr(material, "is_grease_pencil", False):
            continue
        outputs = [n for n in tree.nodes if n.bl_idname == "ShaderNodeOutputMaterial"]
        if not outputs:
            continue
        active = [n for n in outputs if n.is_active_output and n.target in ("ALL", "CYCLES")]
        output = (active or [n for n in outputs if n.target in ("ALL", "CYCLES")] or outputs)[0]
        features, state = set(), {"nodes": 0, "principled": 0, "uv_maps": set(), "vertex_attrs": set()}
        surface, surface_socket = _source(output.inputs["Surface"])
        if surface is not None and surface_socket is not None and surface_socket.type != "SHADER" \
                and surface.bl_idname not in ("ShaderNodeGroup", "NodeGroupInput"):
            # Cycles converts a colour or value wired into Surface to an
            # Emission of strength 1 (scene/shader_graph.cpp).
            features.add("surface:implicit_emission")
        else:
            features.add("surface:" + (_short(surface.bl_idname) if surface is not None else "none"))
        if output.inputs["Displacement"].is_linked:
            features.add("displacement:" + material.displacement_method)
        if output.inputs["Volume"].is_linked:
            features.add("volume")
        _walk_tree(tree, [output], features, state)
        if state["principled"] > 1:
            features.add("multi_principled")
        # How many DISTINCT UV sets does the material read on a mesh?  "" is
        # the render-active map; a named map that IS the render-active map on
        # every mesh using the material is the same set.
        users = slot_materials.get(material.name)
        named = state["uv_maps"] - {""}
        if state["uv_maps"]:
            if not users:
                if named:
                    features.add("uvsets:unresolved")
            else:
                worst, other = 0, False
                for render, _names in users:
                    used = {render if name == "" else name for name in state["uv_maps"]}
                    worst = max(worst, len(used))
                    other = other or used != {render}
                if worst >= 2:
                    features.add("uvsets:%d" % min(worst, 4))
                elif other:
                    # One UV set, but not the map the exporter sends today.
                    features.add("uvsets:other_single")
        if state["vertex_attrs"]:
            features.add("vertex_attrs:%d" % min(len(state["vertex_attrs"]), 5))
        materials.append({
            "name": material.name,
            "in_slot": material.name in slot_materials,
            "nodes": state["nodes"],
            "features": sorted(features),
        })
    with open(out_path, "w", encoding="utf-8") as stream:
        json.dump({"file": os.path.basename(bpy.data.filepath),
                   "saved_with": "%d.%d" % tuple(bpy.data.version[:2]),
                   "materials": materials}, stream)


# ---------------------------------------------------------------------------
# report (plain Python)
# ---------------------------------------------------------------------------

NOT_PLANNED = "not-planned"

# Every feature key belongs to one CAPABILITY: a unit of work that either
# exists in the bridge or does not.  First matching rule wins.  "today" is
# what the exporter already translates faithfully; "none" keys never block.
CAPABILITY_RULES = [
    # Bookkeeping keys: a more specific key always accompanies the node when
    # it is used in a way that needs new work.
    (r"^(principled_in:|surface:(BsdfPrincipled|Group|BsdfHairPrincipled)$|mixshader:fac_|"
     r"node:(Attribute|Displacement|NewGeometry|TexCoord|Mapping|Bump)$)", "none"),
    # Lobe options: they never block a translation, they decide whether the
    # RISE lobe MATCHES Cycles (INEXACT_RULES below).  Two of them are closed
    # by small RISE additions and are a capability of their own.
    (r"^bsdfopt:(BsdfGlossy|BsdfAnisotropic):tinted$", "bsdf-modes"),   # Fresnel-free tinted glossy
    (r"^bsdfopt:BsdfDiffuse:rough$", "bsdf-modes"),                    # energy-preserving Oren-Nayar
    (r"^(bsdfopt:|principled_opt:)", "none"),
    (r"^node:(BsdfPrincipled|TexImage|NormalMap|BsdfHairPrincipled)$", "today"),
    (r"^(coord:UV|mapping:(POINT|TEXTURE)|normalmap:TANGENT|bump:height<-TexImage|image:none)$", "today"),

    # Group nodes are inlined by the compiler; today they force "complex".
    (r"^group$", "node-groups"),
    # Value / RGB feeding an input: today's image walk stops at them and the
    # linked socket's own default is exported instead.
    (r"^node:(Value|RGB|Math|VectorMath|Mix|MixRGB|MapRange|Clamp|ValToRGB|RGBCurve|VectorCurve|FloatCurve|"
     r"HueSaturation|BrightContrast|Gamma|Invert|RGBToBW|SeparateXYZ|CombineXYZ|SeparateColor|CombineColor|"
     r"SeparateRGB|CombineRGB|SeparateHSV|CombineHSV|VectorRotate|Normal)$", "converters"),
    # Colour from a temperature or a wavelength: two builtins of their own.
    (r"^node:(Blackbody|Wavelength)$", "blackbody-wavelength"),
    (r"^(math:|vmath:|mix:|maprange:|ramp:|image:alpha_output$)", "converters"),

    (r"^node:(TexNoise|TexVoronoi|TexWhiteNoise|TexGradient|TexChecker|TexWave|TexMagic)$", "textures"),
    (r"^(noise:|voronoi:|wave:|gradient:|whitenoise:)", "textures"),
    (r"^node:(TexBrick|TexGabor|RadialTiling)$", "textures-2"),

    # Exact for undeformed meshes only; a deformed mesh needs a vertex
    # attribute (phase 2).  Kept apart so the report can show the range.
    (r"^coord:Generated(\(implicit\))?$", "generated-coordinates"),
    # Inputs that read the direction the ray ARRIVED from.  Defined along a
    # path from the camera only, so valid under the path tracer and refused
    # under BDPT / VCM / MLT, like Light Path.
    (r"^(node:(LayerWeight|Fresnel)$|geom:(Incoming|Backfacing)$|coord:Reflection$)", "view-direction"),
    (r"^(coord:(Object|Normal|object_pointer|from_instancer)$|node:UVMap$|"
     r"mapping:(VECTOR|NORMAL|linked_trs|rotation_xy)$|"
     r"geom:(Position|Normal|True Normal|Parametric)$)", "coordinates"),
    # Functions of the shading POINT and the camera transform: valid everywhere.
    (r"^(node:(CameraData|VectorTransform)$|vtransform:|coord:(Camera|Window)$)", "camera-space"),

    (r"^(node:ObjectInfo$|objinfo:|attr:(OBJECT|INSTANCER|VIEW_LAYER)$)", "object-attributes"),
    # A linked Strength or Distance: both RISE modifiers take a constant scale
    # today, so this is a modifier whose scale is a painter.
    (r"^(bump|normalmap):linked_strength$", "modifier-varying-strength"),
    # RISE's normal map is tangent-space only.
    (r"^normalmap:(OBJECT|WORLD|BLENDER_OBJECT|BLENDER_WORLD)$", "normalmap-object-space"),
    # A Bump / Normal Map output used as a VALUE (a Fresnel, Layer Weight or
    # AO normal, vector maths) or as a coat normal from a Bump: the perturbed
    # normal must exist in the program, not only as a modifier.
    (r"^bumpout:(value:.*|coat<-Bump)$", "perturbed-normal-value"),
    (r"^bumpout:coat<-NormalMap$", "none"),
    (r"^(bump:|displacement:BUMP$)", "bump"),

    # Each shader below has a stated RISE mapping (design section 5.6).
    # Holdout, Velvet and Toon have none and fall through to "not planned".
    (r"^(node:(BsdfDiffuse|BsdfGlossy|BsdfAnisotropic|BsdfGlass|BsdfRefraction|BsdfTransparent|"
     r"BsdfSheen|BsdfMetallic|SubsurfaceScattering|Emission)$|"
     r"surface:(BsdfDiffuse|BsdfGlossy|BsdfAnisotropic|BsdfGlass|BsdfRefraction|BsdfTransparent|"
     r"BsdfSheen|BsdfMetallic|SubsurfaceScattering|Emission)$|"
     r"surface:implicit_emission$)", "plain-bsdfs"),
    # EEVEE's Specular BSDF has a Transparency input (a mix with a
    # pass-through) and clear-coat inputs: it is not a single RISE lobe.
    (r"^(node|surface):EeveeSpecular$", "eevee-specular"),
    # Translucent BSDF needs a lobe RISE lacks: a thin sheet that transmits
    # diffusely and tracks no interior (so it may be a mix leaf).
    (r"^(node|surface):BsdfTranslucent$", "diffuse-transmission"),
    # A refracting leaf in a mix is refused (the refraction stack would enter
    # through glass and leave through something else).  In no phase.
    (r"^(mixshader|addshader):.*Bsdf(Glass|Refraction)", "refracting-mix-child"),
    # Mix leaves with different Normal inputs: a modifier per mix child.
    (r"^mixshader:leaf_normals_differ$", "per-leaf-normals"),
    # A subsurface leaf under a spatially varying weight: path tracer only.
    (r"^mixshader:varying_subsurface$", "subsurface-varying-mix"),
    # Gain-mix leaves are wrapped in a scale.  Coated and sheen leaves are
    # outside the first allow-list; a subsurface or hair leaf is refused
    # (its throughput is not scaled), so it has no rule and is not planned.
    (r"^addshader:gain_leaf:(coat|sheen)$", "gain-mix-layered-leaf"),
    (r"^addshader:gain_leaf:", NOT_PLANNED),
    # No rule for mixshader:leaf_unresolved or bumpout:unresolved: unknown
    # is not counted as supported.
    (r"^(mixshader:leaf_unresolved|bumpout:unresolved)", NOT_PLANNED),
    (r"^(node:(MixShader|AddShader)$|mixshader:|addshader:|surface:(MixShader|AddShader)$|multi_principled$)",
     "shader-mix"),

    (r"^(attr:GEOMETRY$|node:(VertexColor|Tangent)$|geom:(Pointiness|Random Per Island|Tangent)$|vertex_attrs:)",
     "vertex-attributes"),
    (r"^uvsets:(2|other_single)$", "second-uv-set"),
    (r"^uvsets:(3|4|unresolved)$", "more-uv-sets"),
    (r"^image:(coords:|projection:|extension:|interpolation:)", "image-coordinates"),
    (r"^(displacement:(DISPLACEMENT|BOTH)$|node:VectorDisplacement$)", "true-displacement"),
    (r"^node:(ParticleInfo|HairInfo|PointInfo)$", "hair-particle-inputs"),
    (r"^image:source:", "image-sources"),
    (r"^(node:AmbientOcclusion$|ao:)", "ambient-occlusion"),
    (r"^(node:LightPath$|lightpath:)", "light-path"),
]
_COMPILED = [(re.compile(pattern), capability) for pattern, capability in CAPABILITY_RULES]

# The proposed plan: capabilities delivered by each phase, in order.  Keep in
# step with docs/BLENDER_PROCEDURAL_TRANSLATION_DESIGN.md section 9.
PLAN = [
    ("step 0", ["plain-bsdfs", "bsdf-modes", "diffuse-transmission"]),
    ("phase 1", ["node-groups", "converters", "textures", "coordinates", "generated-coordinates",
                 "camera-space", "view-direction", "facing-normal", "object-attributes", "bump",
                 "modifier-varying-strength", "normalmap-object-space", "blackbody-wavelength"]),
    ("phase 2", ["shader-mix", "per-leaf-normals", "subsurface-varying-mix", "light-path",
                 "vertex-attributes", "second-uv-set", "more-uv-sets", "image-coordinates",
                 "eevee-specular", "gain-mix-layered-leaf", "perturbed-normal-value"]),
    ("phase 3", ["hair-particle-inputs", "image-sources", "ambient-occlusion", "ao-facing-normal"]),
    ("phase 4", ["true-displacement", "textures-2"]),
]
# Capabilities defined along a path from the camera only: valid under the
# path tracer (which Auto selects for such scenes), refused under BDPT, VCM
# and MLT (docs/MIX_MATERIAL_PATH_STATE_AO_DESIGN.md part II).
# "ao-facing-normal" and "facing-normal" are derived in _load: an input that
# reads the normal Cycles flips to face the arriving ray, on a material that
# also transmits light.
PATH_TRACER_ONLY = ["view-direction", "light-path", "subsurface-varying-mix", "ao-facing-normal",
                    "facing-normal"]
# Inputs that read the facing normal (Cycles negates N and Ng on a back
# face, kernel/geom/shader_data.h).
_FACING_NORMAL = re.compile(r"^(geom:(Normal|True Normal)$|coord:Normal$|image:projection:BOX$|bumpout:value:)")
# Principled inputs RISE takes as constants only (parent section 5.6).
_CONSTANT_ONLY = re.compile(r"^principled_in:(Subsurface (Radius|Scale|IOR|Anisotropy)|Coat IOR)$")
# Features of a material some of whose light crosses the surface at a hit.
_TRANSMITS = re.compile(r"^(node:Bsdf(Translucent|Glass|Refraction)$|principled_opt:transmission$|"
                        r"bsdfopt:BsdfTransparent:tinted$)")
# (label, phase the row is compared with, capabilities assumed NOT delivered,
#  largest graph counted or None)
SENSITIVITY = [
    ("phase 1 if no Generated-coordinate material qualifies", "phase 1", ["generated-coordinates"], None),
    ("phase 4 with no perturbed-normal value in programs", "phase 4", ["perturbed-normal-value"], None),
    ("phase 4 if coated / sheen leaves stay out of gain mixes", "phase 4", ["gain-mix-layered-leaf"], None),
    ("phase 4 with modifier scales left constant", "phase 4", ["modifier-varying-strength"], None),
    ("phase 1a: existing VM, graphs of <= %d nodes", "phase 1", [], 150),
]

# Lobe mappings that translate but do NOT match Cycles: RISE has a model for
# the lobe, with a documented difference.  First matching label is reported;
# a material with any of them is "translated" and not "matching".
INEXACT_RULES = [
    (r"^(node:SubsurfaceScattering$|principled_opt:subsurface$)", "subsurface (approximate model)"),
    (r"^bsdfopt:[A-Za-z]+:dist:(BECKMANN|ASHIKHMIN_SHIRLEY|SHARP|\?)$", "Beckmann / Ashikhmin-Shirley as GGX"),
    (r"^(bsdfopt:(BsdfGlass|BsdfRefraction):(rough|tinted)$|principled_opt:rough_transmission$)",
     "rough or tinted glass"),
    (r"^node:BsdfRefraction$", "Refraction BSDF (no reflection lobe)"),
    (r"^node:BsdfSheen$", "Sheen BSDF (rendered with RISE's Charlie sheen)"),
    (r"^bsdfopt:BsdfMetallic:(fresnel:F82|edge_tint)$", "Metallic BSDF edge tint (not modelled)"),
    (r"^node:EeveeSpecular$", "Specular BSDF (an EEVEE node with no Cycles rendering)"),
]
_INEXACT = [(re.compile(pattern), label) for pattern, label in INEXACT_RULES]

# Graph-level differences from Cycles that are disclosed, not refused: the
# pattern is translated, the values are not Cycles' values pixel for pixel.
GRAPH_INEXACT_RULES = [
    (r"^node:AmbientOcclusion$", "Ambient Occlusion (fixed sample set per point; Cycles draws per path)"),
    (r"^node:TexWhiteNoise$", "White Noise (same distribution, different values)"),
    (r"^image:interpolation:(Cubic|Smart)$", "cubic image filtering (rendered bilinear)"),
]
_GRAPH_INEXACT = [(re.compile(pattern), label) for pattern, label in GRAPH_INEXACT_RULES]


def capability_of(feature):
    for pattern, capability in _COMPILED:
        if pattern.search(feature):
            return capability
    return NOT_PLANNED


def _load(json_dir):
    """-> list of {file, name, nodes, features, needs} over every material."""
    out, skipped, files = [], 0, 0
    for name in sorted(os.listdir(json_dir)):
        if not name.endswith(".json"):
            continue
        with open(os.path.join(json_dir, name), encoding="utf-8") as stream:
            data = json.load(stream)
        files += 1
        for material in data["materials"]:
            features = set(material["features"])
            if "surface:none" in features:
                skipped += 1
                continue
            needs = {capability_of(f) for f in features} - {"none", "today"}
            # A transmissive Principled as a mix leaf is a refracting leaf too.
            if "principled_opt:transmission" in features and any(
                    f.startswith(("mixshader:", "addshader:")) and "BsdfPrincipled" in f for f in features):
                needs.add("refracting-mix-child")
            # Ambient Occlusion with no Normal input uses the normal facing
            # the arriving ray: a view-direction input where light can cross
            # the surface, an ordinary one where it cannot.
            transmits = any(_TRANSMITS.search(f) for f in features)
            if "ao:default_normal" in features and transmits:
                needs.add("ao-facing-normal")
            if transmits and any(_FACING_NORMAL.search(f) for f in features):
                needs.add("facing-normal")
            # Linked Principled weights that are a per-texel choice of lobe.
            if "principled_in:Subsurface Weight" in features:
                needs.add("subsurface-varying-mix")
            if "principled_in:Transmission Weight" in features:
                needs.add("refracting-mix-child")
            # A procedural input to a constant-only parameter of a lobe the
            # material uses: no phase makes it procedural.
            constant_only = {f for f in features if _CONSTANT_ONLY.search(f)}
            if "principled_opt:subsurface" not in features:
                constant_only = {f for f in constant_only if "Subsurface" not in f}
            if constant_only:
                needs.add("constant-only-input")
            inexact = {label for f in features for pattern, label in _INEXACT if pattern.search(f)}
            graph_inexact = {label for f in features for pattern, label in _GRAPH_INEXACT if pattern.search(f)}
            out.append({"file": name, "name": material["name"], "nodes": material["nodes"],
                        "features": features, "needs": needs, "inexact": inexact,
                        "graph_inexact": graph_inexact})
    return out, skipped, files


class _Metric:
    """Three ways to count 'how many materials are covered', because a few
    productions with hundreds of near-identical materials dominate a plain
    count: every distinct (name, features) material; every distinct feature
    set (a 'structure'); and the mean of each file's own percentage."""

    def __init__(self, records):
        by_material, by_structure = {}, {}
        for record in records:
            by_material.setdefault((record["name"], frozenset(record["features"])), record)
            by_structure.setdefault(frozenset(record["features"]), record)
        self.materials = list(by_material.values())
        self.structures = list(by_structure.values())
        per_file = collections.defaultdict(list)
        for record in records:
            per_file[record["file"]].append(record)
        self.files = [group for group in per_file.values() if len(group) >= 5]

    def coverage(self, enabled, also=None):
        ok = lambda record: record["needs"] <= enabled and (also is None or also(record))
        share = lambda items: 100.0 * sum(1 for r in items if ok(r)) / max(1, len(items))
        return share(self.materials), share(self.structures), sum(share(g) for g in self.files) / max(1, len(self.files))


def _report(json_dir, show=30):
    records, skipped, files = _load(json_dir)
    metric = _Metric(records)
    materials = metric.materials
    total = len(materials)
    sizes = sorted(m["nodes"] for m in materials)
    print("files %d; materials %d distinct (of %d instances, %d with no surface shader skipped); "
          "%d distinct structures; %d files with >= 5 materials"
          % (files, total, len(records), skipped, len(metric.structures), len(metric.files)))
    print("nodes per material: median %d, p75 %d, p90 %d, p99 %d, max %d"
          % (sizes[total // 2], sizes[int(total * .75)], sizes[int(total * .9)], sizes[int(total * .99)], sizes[-1]))

    header = "  %-52s %10s %11s %10s" % ("", "materials", "structures", "per-file")
    row = "  %-52s %9.1f%% %10.1f%% %9.1f%%"
    matching = lambda record: not record["inexact"]
    tables = [
        ("GRAPH TRANSLATED under the path tracer (every feature supported)", set(), None),
        ("... and every lobe MATCHES Cycles (no approximate lobe mapping)", set(), matching),
        ("GRAPH TRANSLATED under PT, BDPT and VCM alike (no path-tracer-only input)", set(PATH_TRACER_ONLY), None),
    ]
    at_phase = {}
    for title, withheld, also in tables:
        print("\n" + title + ", cumulative\n" + header)
        enabled = set()
        print(row % (("today",) + metric.coverage(enabled, also)))
        for phase, capabilities in PLAN:
            enabled |= set(capabilities) - withheld
            if not withheld and also is None:
                at_phase[phase] = set(enabled)
            print(row % ((phase,) + metric.coverage(enabled, also)))
    print("\nsensitivity (graph translated under the path tracer)\n" + header)
    for label, phase, withheld, limit in SENSITIVITY:
        also = None if limit is None else (lambda record, limit=limit: record["nodes"] <= limit)
        print(row % ((label % limit if limit is not None else label,)
                     + metric.coverage(at_phase[phase] - set(withheld), also)))
    everything = {c for _, group in PLAN for c in group}
    blocked = sum(1 for m in materials if not m["needs"] <= everything)
    print("  not planned: %d materials (%.1f%%) use a feature no phase covers" % (blocked, 100.0 * blocked / total))
    inexact = collections.Counter(label for m in materials for label in m["inexact"])
    print("\napproximate lobe mappings: materials using each (a material may use several)")
    for label, count in inexact.most_common():
        print("  %5d %5.1f%%   %s" % (count, 100.0 * count / total, label))
    print("  %5d %5.1f%%   any of them" % (sum(1 for m in materials if m["inexact"]),
                                        100.0 * sum(1 for m in materials if m["inexact"]) / total))
    graph_inexact = collections.Counter(label for m in materials for label in m["graph_inexact"])
    print("\ndisclosed graph-level differences: materials using each")
    for label, count in graph_inexact.most_common():
        print("  %5d %5.1f%%   %s" % (count, 100.0 * count / total, label))
    print("  %5d %5.1f%%   any of them" % (sum(1 for m in materials if m["graph_inexact"]),
                                        100.0 * sum(1 for m in materials if m["graph_inexact"]) / total))
    bare = sum(1 for m in materials if not m["needs"] and "node:TexImage" not in m["features"])
    covered_today = sum(1 for m in materials if not m["needs"])
    print("  of the %d materials covered today, %d are a bare Principled with no image" % (covered_today, bare))

    print("\ncapabilities: materials needing it / materials needing ONLY it")
    planned = {c for _, group in PLAN for c in group}
    needing = collections.Counter(c for m in materials for c in m["needs"])
    only = collections.Counter(next(iter(m["needs"])) for m in materials if len(m["needs"]) == 1)
    for capability, count in needing.most_common():
        print("  %5d %5.1f%%   %5d   %s" % (count, 100.0 * count / total, only[capability], capability))

    print("\ngreedy order (each step adds the capability that covers the most additional materials)\n" + header)
    enabled, remaining = set(), set(planned)
    while remaining:
        def gain(capability):
            return (sum(1 for m in materials if m["needs"] <= enabled | {capability}), needing[capability])
        best = max(sorted(remaining), key=gain)
        enabled.add(best)
        remaining.discard(best)
        print("  + %-42s %9.1f%% %10.1f%% %9.1f%%" % ((best,) + metric.coverage(enabled)))

    using = collections.Counter(f for m in materials for f in m["features"])
    by_capability = collections.defaultdict(list)
    for feature, count in using.items():
        by_capability[capability_of(feature)].append((count, feature))
    for capability in sorted(by_capability, key=lambda c: -needing[c]):
        if capability in ("none", "today"):
            continue
        rows = sorted(by_capability[capability], reverse=True)
        print("\n[%s] features by materials using them (%d keys)" % (capability, len(rows)))
        for count, feature in rows[:show]:
            print("  %5d %5.1f%%  %s" % (count, 100.0 * count / total, feature))


# ---------------------------------------------------------------------------
# run (plain Python driver)
# ---------------------------------------------------------------------------

def _run(argv):
    blender, out_dir, files = None, None, []
    index = 0
    while index < len(argv):
        if argv[index] == "--blender":
            blender = argv[index + 1]
            index += 2
        elif argv[index] == "--out":
            out_dir = argv[index + 1]
            index += 2
        else:
            files.append(argv[index])
            index += 1
    if not blender or not out_dir or not files:
        sys.exit("usage: blender_node_survey.py run --blender <binary> --out <dir> files...")
    os.makedirs(out_dir, exist_ok=True)
    script = os.path.abspath(__file__)
    for number, path in enumerate(files):
        out = os.path.join(out_dir, "%03d_%s.json" % (number, re.sub(r"[^A-Za-z0-9_.-]", "_", os.path.basename(path))))
        if os.path.isfile(out):
            continue
        command = [blender, "-b", "--factory-startup", "--disable-autoexec", path, "--python", script, "--", out]
        try:
            subprocess.run(command, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL, timeout=1800)
        except subprocess.TimeoutExpired:
            pass
        print("%s  %s" % ("ok  " if os.path.isfile(out) else "FAIL", path), flush=True)


if __name__ == "__main__":
    try:
        import bpy  # noqa: F401
        _INSIDE_BLENDER = True
    except ImportError:
        _INSIDE_BLENDER = False
    if _INSIDE_BLENDER:
        _collect(sys.argv[sys.argv.index("--") + 1])
    elif len(sys.argv) >= 3 and sys.argv[1] == "run":
        _run(sys.argv[2:])
    elif len(sys.argv) == 3 and sys.argv[1] == "report":
        _report(sys.argv[2])
    else:
        sys.exit(__doc__)
