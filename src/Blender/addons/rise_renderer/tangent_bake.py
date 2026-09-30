"""DL-213 corner tangent production. Cycles evaluates the entire shader graph.

The subprocess is intentional: swapping a live RISE RenderEngine to Cycles
invalidates Blender's render RNA. Only copied datablocks enter the bake scene;
user selection, nodes, UVs, attributes and render settings are never edited.
"""
from __future__ import annotations

import json
import math
import os
import subprocess
import tempfile
from pathlib import Path


def find_tangent_socket(material):
    if material is None or not material.use_nodes or material.node_tree is None:
        return None
    # Match the exporter's single-Principled material contract.
    from .material_bake import _find_principled_through_surface
    output = next((n for n in material.node_tree.nodes
                   if n.bl_idname == "ShaderNodeOutputMaterial" and n.is_active_output), None)
    node = _find_principled_through_surface(output)
    return node.inputs.get("Tangent") if node else None


def uv_tangent_source(socket):
    """Return the direct UV Tangent node, traversing only reroutes."""
    if socket is None or not socket.is_linked:
        return None
    node = socket.links[0].from_node
    seen = set()
    while node.bl_idname == "NodeReroute" and node.inputs[0].is_linked:
        if node.as_pointer() in seen:
            return None
        seen.add(node.as_pointer())
        node = node.inputs[0].links[0].from_node
    return node if node.bl_idname == "ShaderNodeTangent" and node.direction_type == "UV_MAP" else None


def _direction_dependencies(socket):
    """Collect reachable outputs with node-group instance input bindings."""
    seen, outputs = set(), []
    def input_socket(inp, groups):
        if inp.is_linked:
            output_socket(inp.links[0].from_socket, groups)
    def output_socket(out, groups):
        key = (out.as_pointer(), tuple(group.as_pointer() for group in groups))
        if key in seen:
            return
        seen.add(key)
        outputs.append(out)
        node = out.node
        if node.bl_idname == 'NodeGroupInput':
            if groups:
                index = list(node.outputs).index(out)
                if index < len(groups[-1].inputs):
                    input_socket(groups[-1].inputs[index], groups[:-1])
        elif node.bl_idname == 'ShaderNodeGroup' and node.node_tree:
            output = next((n for n in node.node_tree.nodes
                           if n.bl_idname == 'NodeGroupOutput' and n.is_active_output), None)
            index = list(node.outputs).index(out)
            if output and index < len(output.inputs):
                input_socket(output.inputs[index], groups + (node,))
        else:
            for inp in node.inputs:
                if not getattr(inp, 'is_unavailable', False):
                    input_socket(inp, groups)
    input_socket(socket, ())
    return outputs


def _f32(value):
    import struct
    return struct.unpack('f', struct.pack('f', value))[0]


def _rna_rgba(owner, name):
    """Cycles BKE_object_dupli_find_rgba_attribute scalar/array RNA contract."""
    if owner is None:
        return None
    try:
        value = owner[name] if name in owner else owner.path_resolve(name)
    except (KeyError, ValueError, AttributeError, TypeError):
        return None
    if isinstance(value, (bool, int, float)):
        return [_f32(value)] * 3 + [1.0]
    try:
        values = list(value)
    except TypeError:
        return None
    if len(values) > 4 or not all(isinstance(v, (int, float)) and not isinstance(v, bool) for v in values):
        return None
    result = [0.0, 0.0, 0.0, 1.0]
    result[:len(values)] = [_f32(v) for v in values]
    return result


def _shader_context(socket, source_object, instance_info):
    """Freeze only source context lost by realizing an evaluated target."""
    info = instance_info or {}
    snapshots = {}
    for out in _direction_dependencies(socket):
        node = out.node
        if node.bl_idname != 'ShaderNodeAttribute' or node.attribute_type not in {'OBJECT', 'INSTANCER'}:
            continue
        key = (node.attribute_type, node.attribute_name)
        if key in snapshots:
            continue
        value = None
        if node.attribute_type == 'INSTANCER' and info.get('is_instance'):
            value = _rna_rgba(info.get('particle_settings'), node.attribute_name)
            if value is None and info.get('private_instance_attributes'):
                raise RuntimeError('RISE Tangent bake cannot reconstruct private Geometry Nodes instancer attributes from Blender DepsgraphObjectInstance RNA: ' + node.attribute_name)
            if value is None:
                value = _rna_rgba(info.get('parent'), node.attribute_name)
        if value is None:
            value = _rna_rgba(source_object, node.attribute_name)
        if value is None:
            value = _rna_rgba(getattr(source_object, 'data', None), node.attribute_name)
        snapshots[key] = value if value is not None else [0.0] * 4
    return [[kind, name, rgba] for (kind, name), rgba in snapshots.items()]


def _unit(vector):
    """Normalize representation magnitude first; finite scale cannot mean zero."""
    if not all(math.isfinite(v) for v in vector):
        raise RuntimeError("RISE Tangent bake produced a nonfinite direction.")
    scale = max(abs(v) for v in vector)
    if scale == 0:
        raise RuntimeError("RISE Tangent bake produced a zero/parallel direction in the shading plane.")
    result = vector / scale
    result.normalize()
    return result


def decode_direction(encoded, inverse_linear, normal, allow_parallel=False):
    """Preserve the full world socket vector in object coordinates.

    The core projects against the current shading normal, and reprojects the
    raw vector after supported normal modifiers. Classify a physical parallel
    vector at the mesh normal only when no exported base-normal modifier can
    change that normal. Representation magnitude never affects validity.
    """
    world = _unit(encoded.__class__((2*encoded[0]-1, 2*encoded[1]-1, 2*encoded[2]-1)))
    if not allow_parallel:
        world_normal = _unit(inverse_linear.transposed() @ normal)
        _unit(world_normal.cross(world).cross(world_normal))
    return (*_unit(inverse_linear @ world), 1.0)


def validate_baked_color(color):
    """Detect unwritten/impossible encoded results, not all Cycles backend faults.

    Normalize limits decoded vectors to the unit ball; corner averaging is
    convex. Allow 32 binary32 eps for normalization/arithmetic/store rounding,
    independent of object scale. Alpha alone cannot detect black backend output.
    """
    allowance = 32 * 2**-23
    if (len(color) != 4 or not all(math.isfinite(v) for v in color) or
            color[3] != 1 or sum((2*c-1)**2 for c in color[:3]) > (1+allowance)**2):
        raise RuntimeError("RISE Tangent Cycles bake returned an unwritten/impossible corner buffer (backend conditioning failure).")


def corner_tangents(mesh, material, matrix_world, source_object=None, instance_info=None, used_corners=None, allow_parallel=False):
    """Return None for unlinked/active-UV fallback; else one vec4 per loop."""
    socket = find_tangent_socket(material)
    if socket is None or not socket.is_linked:
        return None
    node = uv_tangent_source(socket)
    if node is not None:
        name = node.uv_map or (mesh.uv_layers.active.name if mesh.uv_layers.active else "")
        if not name or mesh.uv_layers.get(name) is None:
            raise RuntimeError(f"RISE Tangent UV map '{name}' does not exist on '{mesh.name}'.")
        if mesh.uv_layers.active and name == mesh.uv_layers.active.name:
            return None
        # The evaluated temporary mesh owns this data; no authored UV/normal edits.
        mesh.calc_tangents(uvmap=name)
        try:
            inverse = matrix_world.to_3x3().inverted()
            result = [None] * len(mesh.loops)
            for i in (range(len(mesh.loops)) if used_corners is None else set(used_corners)):
                loop = mesh.loops[i]
                # Cycles Tangent socket uses object_normal_transform (M^-T),
                # not the forward surface-vector convention of glTF TANGENT.
                world = _unit(inverse.transposed() @ loop.tangent)
                world_normal = _unit(inverse.transposed() @ mesh.corner_normals[i].vector)
                # Tangent node itself projects its output, before the BSDF.
                world = _unit(world_normal.cross(world).cross(world_normal))
                encoded = world * .5 + world.__class__((.5,.5,.5))
                direction = decode_direction(encoded, inverse, mesh.corner_normals[i].vector.copy())
                result[i] = (*direction[:3], loop.bitangent_sign)
            return result
        finally:
            mesh.free_tangents()
    return _bake_graph(mesh, material, matrix_world, source_object, instance_info, used_corners, allow_parallel)


def _bake_graph(mesh, material, matrix_world, source_object, instance_info, used_corners, allow_parallel):
    import bpy
    from mathutils import Matrix, Vector
    context = _shader_context(find_tangent_socket(material), source_object, instance_info)
    scene = obj = affine_parent = copied_mesh = copied_material = None
    try:
        # Copy scene settings/world and retain the surrounding object dependencies:
        # AO and object-reference graphs must see the same scene, not a lone quad.
        scene = bpy.context.scene.copy()
        scene.name = "RISE temporary tangent bake"
        copied_mesh = mesh.copy()
        copied_material = material.copy()
        copied_mesh.materials.clear()
        copied_mesh.materials.append(copied_material)
        # Bake only this material bucket, but keep the full evaluated geometry:
        # node attributes/generated coordinates and normals stay faithful.
        for polygon in copied_mesh.polygons:
            polygon.material_index = 0
        if source_object is not None:
            obj = source_object.copy()
            obj.data = copied_mesh
            obj.parent = None
            obj.modifiers.clear()
            obj.constraints.clear()
            obj.animation_data_clear()
            obj["rise_source_name"] = source_object.original.name
            obj["rise_bake_target"] = True
        else:
            obj = bpy.data.objects.new("RISE tangent bake object", copied_mesh)
            obj["rise_bake_target"] = True
        if instance_info and instance_info.get("is_instance"):
            obj["rise_instance_random_id"] = str(int(instance_info["random_id"]) & 0xffffffff)
        obj['rise_attribute_context'] = json.dumps(context)
        info = instance_info or {}
        generated = [_f32(_f32(.5 * v) - .5) for v in info.get('orco', (0, 0, 0))] if info.get('is_instance') else [0.0] * 3
        uv = list(info.get('uv', (0, 0))) + [0.0] if info.get('is_instance') else [0.0] * 3
        obj['rise_dupli_generated'] = generated
        obj['rise_dupli_uv'] = uv
        scene.collection.objects.link(obj)
        # RNA matrix_world assignment decomposes to TRS and loses parent shear.
        # An identity parent with an arbitrary parent-inverse matrix preserves
        # the exact affine frame through .blend serialization and child updates.
        affine_parent = bpy.data.objects.new('RISE tangent affine parent', None)
        scene.collection.objects.link(affine_parent)
        obj.parent = affine_parent
        obj.parent_type = 'OBJECT'
        obj.matrix_parent_inverse = matrix_world
        obj.matrix_basis = Matrix.Identity(4)
        obj['rise_expected_world_matrix'] = [value for row in matrix_world for value in row]
        scene.render.engine = "CYCLES"
        scene.cycles.samples = 1
        with tempfile.TemporaryDirectory(prefix="rise-tangent-") as directory:
            blend = os.path.join(directory, "source.blend")
            result = os.path.join(directory, "directions.json")
            bpy.data.libraries.write(blend, {scene}, path_remap="ABSOLUTE", fake_user=False)
            command = [bpy.app.binary_path, "--background", blend, "--python-exit-code", "1",
                       "--python", str(Path(__file__).resolve()), "--", result]
            completed = subprocess.run(command, capture_output=True, text=True, timeout=300)
            if completed.returncode:
                raise RuntimeError("RISE Tangent Cycles bake failed: " +
                                   (completed.stdout + completed.stderr)[-4000:])
            with open(result, encoding="utf-8") as stream:
                encoded = json.load(stream)
        if len(encoded) != len(mesh.loops):
            raise RuntimeError("RISE Tangent bake corner count changed.")
        inverse = matrix_world.to_3x3().inverted()
        directions = [None] * len(mesh.loops)
        # Keep full evaluated geometry for generated coordinates/dependencies;
        # unrelated material faces do not define this socket's validity domain.
        for i in (range(len(mesh.loops)) if used_corners is None else set(used_corners)):
            validate_baked_color(encoded[i])
            directions[i] = decode_direction(Vector(encoded[i][:3]), inverse, mesh.corner_normals[i].vector.copy(), allow_parallel)
        return directions
    finally:
        # Removing our copies releases their dependencies; original data remains.
        if obj is not None:
            bpy.data.objects.remove(obj, do_unlink=True)
        if affine_parent is not None:
            bpy.data.objects.remove(affine_parent, do_unlink=True)
        if scene is not None:
            bpy.data.scenes.remove(scene)
        if copied_mesh is not None:
            bpy.data.meshes.remove(copied_mesh)
        if copied_material is not None:
            bpy.data.materials.remove(copied_material)


def _run_bake(output_path):
    """Executed only in the disposable background Blender process."""
    import bpy
    scene = next(s for s in bpy.data.scenes if s.name.startswith("RISE temporary tangent bake"))
    bpy.context.window.scene = scene
    obj = next(o for o in scene.objects if o.get("rise_bake_target", False))
    for other in scene.objects:
        other.select_set(False)
        if other is not obj and other.name == obj.get("rise_source_name"):
            other.hide_render = True
            other.name = "RISE original bake source"
    bpy.context.view_layer.objects.active = obj
    obj.select_set(True)
    bpy.context.view_layer.update()
    actual = [value for row in obj.matrix_world for value in row]
    expected = list(obj['rise_expected_world_matrix'])
    if actual != expected:
        raise RuntimeError('RISE Tangent child failed to preserve the exact affine world matrix.')
    if "rise_source_name" in obj:
        obj.name = obj["rise_source_name"]
    mesh = obj.data
    mat = mesh.materials[0]
    # Isolate groups before replacing source identity/RNA/dupli constants.
    # Surrounding scene dependencies retain their original shader graphs.
    snapshots = {(kind, name): rgba for kind, name, rgba in json.loads(obj['rise_attribute_context'])}
    random_value = None
    if 'rise_instance_random_id' in obj:
        random_value = _f32(_f32(int(obj['rise_instance_random_id'])) * _f32(1.0 / _f32(0xFFFFFFFF)))
    def vector_constant(tree, values):
        node = tree.nodes.new('ShaderNodeCombineXYZ')
        for inp, value in zip(node.inputs, values):
            inp.default_value = value
        return node.outputs[0]
    def scalar_constant(tree, value):
        node = tree.nodes.new('ShaderNodeValue')
        node.outputs[0].default_value = value
        return node.outputs[0]
    def rewire(tree, output, replacement):
        for link in list(output.links):
            tree.links.new(replacement, link.to_socket)
    def preserve_context(tree):
        for n in list(tree.nodes):
            if n.bl_idname == 'ShaderNodeGroup' and n.node_tree:
                n.node_tree = n.node_tree.copy()
                preserve_context(n.node_tree)
            elif n.bl_idname == 'ShaderNodeObjectInfo' and random_value is not None:
                rewire(tree, n.outputs['Random'], scalar_constant(tree, random_value))
            elif n.bl_idname == 'ShaderNodeTexCoord' and n.from_instancer:
                for name, key in (('Generated', 'rise_dupli_generated'), ('UV', 'rise_dupli_uv')):
                    rewire(tree, n.outputs[name], vector_constant(tree, obj[key]))
            elif n.bl_idname == 'ShaderNodeAttribute' and (n.attribute_type, n.attribute_name) in snapshots:
                rgba = snapshots[(n.attribute_type, n.attribute_name)]
                color = tree.nodes.new('ShaderNodeRGB'); color.outputs[0].default_value = rgba
                rewire(tree, n.outputs['Color'], color.outputs[0])
                rewire(tree, n.outputs['Vector'], vector_constant(tree, rgba[:3]))
                rewire(tree, n.outputs['Fac'], scalar_constant(tree, _f32(_f32(_f32(rgba[0] + rgba[1]) + rgba[2]) / 3)))
                rewire(tree, n.outputs['Alpha'], scalar_constant(tree, rgba[3]))
    preserve_context(mat.node_tree)
    # Resolve the same Principled socket without importing the add-on package.
    nt = mat.node_tree
    out = next(n for n in nt.nodes if n.bl_idname == "ShaderNodeOutputMaterial" and n.is_active_output)
    node = out.inputs['Surface'].links[0].from_node
    while node.bl_idname == 'NodeReroute':
        node = node.inputs[0].links[0].from_node
    source = node.inputs['Tangent'].links[0].from_socket
    normalize = nt.nodes.new('ShaderNodeVectorMath'); normalize.operation = 'NORMALIZE'
    nt.links.new(source, normalize.inputs[0])
    scale = nt.nodes.new('ShaderNodeVectorMath'); scale.operation = 'SCALE'; scale.inputs['Scale'].default_value = .5
    nt.links.new(normalize.outputs[0], scale.inputs[0])
    add = nt.nodes.new('ShaderNodeVectorMath'); add.operation = 'ADD'; add.inputs[1].default_value = (.5,.5,.5)
    nt.links.new(scale.outputs[0], add.inputs[0])
    emission = nt.nodes.new('ShaderNodeEmission'); nt.links.new(add.outputs[0], emission.inputs['Color'])
    nt.links.new(emission.outputs[0], out.inputs['Surface'])
    if out.inputs['Volume'].is_linked:
        nt.links.remove(out.inputs['Volume'].links[0])
    if out.inputs['Displacement'].is_linked:
        nt.links.remove(out.inputs['Displacement'].links[0])
    attr = mesh.color_attributes.new(name='RISE direction', type='FLOAT_COLOR', domain='CORNER')
    mesh.color_attributes.active_color = attr
    scene.render.bake.target = 'VERTEX_COLORS'
    bpy.ops.object.bake(type='EMIT')
    with open(output_path, 'w', encoding='utf-8') as stream:
        json.dump([list(value.color) for value in attr.data], stream)


if __name__ == '__main__':
    import sys
    _run_bake(sys.argv[sys.argv.index('--')+1])
