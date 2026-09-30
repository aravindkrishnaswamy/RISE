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


def decode_direction(encoded, inverse_linear, normal):
    """Linear encoded world direction -> projected object-local unit xyz/sign.

    A vector field supplies no independent bitangent: choose local cross(N,T),
    sign +1. UV nodes instead carry Blender's actual MikkTSpace bitangent sign.
    Project in WORLD space first: inverse/projection do not commute under
    nonuniform scale. Then inverse-transform the projected direction. Object
    later promotes the same vector with its established parity rule.
    """
    world = encoded.__class__((2*encoded[0]-1, 2*encoded[1]-1, 2*encoded[2]-1))
    world_normal = inverse_linear.transposed() @ normal
    world_normal.normalize()
    world -= world_normal * world.dot(world_normal)
    direction = inverse_linear @ world
    direction -= normal * direction.dot(normal)
    if not all(math.isfinite(v) for v in direction) or direction.length_squared < 1e-20:
        raise RuntimeError("RISE Tangent bake produced a zero/nonfinite direction in the shading plane.")
    direction.normalize()
    return (*direction, 1.0)


def corner_tangents(mesh, material, matrix_world, source_object=None, instance_info=None):
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
        result = [(*loop.tangent, loop.bitangent_sign) for loop in mesh.loops]
        mesh.free_tangents()
        return result
    return _bake_graph(mesh, material, matrix_world, source_object, instance_info)


def _bake_graph(mesh, material, matrix_world, source_object, instance_info):
    import bpy
    from mathutils import Vector
    scene = obj = copied_mesh = copied_material = None
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
        scene.collection.objects.link(obj)
        obj.matrix_world = matrix_world
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
        return [decode_direction(Vector(color[:3]), inverse, mesh.corner_normals[i].vector.copy())
                for i, color in enumerate(encoded)]
    finally:
        # Removing our copies releases their dependencies; original data remains.
        if obj is not None:
            bpy.data.objects.remove(obj, do_unlink=True)
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
    if "rise_source_name" in obj:
        obj.name = obj["rise_source_name"]
    mesh = obj.data
    mat = mesh.materials[0]
    # Realized instances lose Cycles' dupli random identity. Preserve exactly
    # the value Cycles exports (intern/cycles/scene/object.cpp, Blender 4.5),
    # rather than hashing the temporary object's name. Rewire inside the child
    # only, recursively copying groups to isolate surrounding-scene materials.
    if "rise_instance_random_id" in obj:
        import struct
        f32 = lambda x: struct.unpack('f', struct.pack('f', x))[0]
        random_value = f32(f32(int(obj["rise_instance_random_id"])) * f32(1.0 / f32(0xFFFFFFFF)))
        def replace_random(tree):
            for n in list(tree.nodes):
                if n.bl_idname == 'ShaderNodeGroup' and n.node_tree:
                    n.node_tree = n.node_tree.copy()
                    replace_random(n.node_tree)
                elif n.bl_idname == 'ShaderNodeObjectInfo':
                    value = tree.nodes.new('ShaderNodeValue')
                    value.outputs[0].default_value = random_value
                    for link in list(n.outputs['Random'].links):
                        tree.links.new(value.outputs[0], link.to_socket)
        replace_random(mat.node_tree)
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
