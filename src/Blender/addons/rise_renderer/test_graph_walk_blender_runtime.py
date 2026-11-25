"""Runs INSIDE Blender (no add-on install needed):

    blender -b --factory-startup --python-exit-code 1 \\
        --python test_graph_walk_blender_runtime.py

Regression for shader-graph walks keyed on Python object identity.  Blender
hands out a fresh wrapper object per node access, so ``id(node)`` is reused
across different nodes; a visited-set keyed on it treats an unvisited node as
seen and the walk stops short.  Which nodes are hit depends on allocator
reuse: fixed for a given graph shape, different for the next one.

Each case below is a graph shape on which the ``id()``-keyed walk gave a
wrong answer, checked two ways: the shipped code must be right on every one
of N runs, and the same function with `rna_key` swapped back to ``id`` must
be wrong at least once (the red control; without it a case could pass on a
build where the bug is merely not triggered).  The premise check states the
mechanism directly, independent of any particular walk.
"""

import importlib.util
import os
import sys

import bpy

HERE = os.path.dirname(os.path.abspath(__file__))
if "rise_renderer" not in sys.modules:
    spec = importlib.util.spec_from_file_location(
        "rise_renderer", os.path.join(HERE, "__init__.py"), submodule_search_locations=[HERE])
    module = importlib.util.module_from_spec(spec)
    sys.modules["rise_renderer"] = module
    spec.loader.exec_module(module)
from rise_renderer import blender_compat, exporter, material_bake  # noqa: E402

RUNS = 100
FAILURES = 0


def check(label, ok, detail=""):
    global FAILURES
    FAILURES += 0 if ok else 1
    print("GRAPHWALK %s %s %s" % ("PASS" if ok else "FAIL", label, detail))


def case(label, modules, run, expected):
    """`run()` -> a hashable answer.  Shipped key: always `expected`.  With
    ``id`` as the key: `expected` must be missed at least once."""

    shipped = [run() for _ in range(RUNS)]
    real = blender_compat.rna_key
    for module in modules:
        module.rna_key = id
    try:
        control = [run() for _ in range(RUNS)]
    finally:
        for module in modules:
            module.rna_key = real
    wrong = sum(answer != expected for answer in control)
    check(label, all(answer == expected for answer in shipped),
          "shipped %d/%d right" % (sum(a == expected for a in shipped), RUNS))
    check(label + " [red control]", wrong > 0, "id()-keyed walk wrong in %d/%d runs" % (wrong, RUNS))


def new_material(name):
    material = bpy.data.materials.new(name)
    tree = material.node_tree
    principled = tree.nodes["Principled BSDF"]
    output = next(n for n in tree.nodes if n.bl_idname == "ShaderNodeOutputMaterial")
    return material, tree, principled, output


def reroute_chain(tree, socket, count):
    for _ in range(count):
        reroute = tree.nodes.new("NodeReroute")
        tree.links.new(socket, reroute.inputs[0])
        socket = reroute.outputs[0]
    return socket


def main():
    # Premise: the mechanism itself.
    _, tree, _, _ = new_material("premise")
    for _ in range(12):
        tree.nodes.new("ShaderNodeMath")
    ids = len({id(node) for node in tree.nodes})
    pointers = len({blender_compat.rna_key(node) for node in tree.nodes})
    check("premise: wrapper id() is reused across nodes, RNA pointers are not",
          ids < len(tree.nodes) and pointers == len(tree.nodes),
          "%d nodes: %d distinct id(), %d distinct pointers" % (len(tree.nodes), ids, pointers))

    # 1. classify_material: a truly unaccepted node (Magic Texture) behind a
    #    chain of accepted ones (Bump, ColorRamp, Noise, Mapping, Combine XYZ,
    #    Math).  The id()-keyed walk skipped them and called the material
    #    "simple", so the render gate never fired.  Whether a given shape trips
    #    the reuse depends on the allocator, so several shapes are built: the
    #    shipped code must be right on all of them and the control must be
    #    wrong on at least one.
    def classify_fixture(branches):
        material, tree, principled, _ = new_material("classify_%d" % branches)
        for index in range(branches):
            bump = tree.nodes.new("ShaderNodeBump")
            ramp = tree.nodes.new("ShaderNodeValToRGB")
            noise = tree.nodes.new("ShaderNodeTexNoise")
            mapping = tree.nodes.new("ShaderNodeMapping")
            coord = tree.nodes.new("ShaderNodeTexCoord")
            combine = tree.nodes.new("ShaderNodeCombineXYZ")
            math_node = tree.nodes.new("ShaderNodeMath")
            magic = tree.nodes.new("ShaderNodeTexMagic")
            tree.links.new(magic.outputs["Fac"], math_node.inputs[0])
            tree.links.new(math_node.outputs[0], combine.inputs[0])
            tree.links.new(combine.outputs[0], mapping.inputs["Location"])
            tree.links.new(coord.outputs["Object"], mapping.inputs["Vector"])
            tree.links.new(mapping.outputs[0], noise.inputs["Vector"])
            tree.links.new(noise.outputs["Fac"], ramp.inputs[0])
            if index == 0:
                tree.links.new(ramp.outputs["Color"], principled.inputs["Base Color"])
                tree.links.new(noise.outputs["Fac"], bump.inputs["Height"])
                tree.links.new(bump.outputs[0], principled.inputs["Normal"])
            else:
                tree.links.new(ramp.outputs["Color"], principled.inputs["Roughness" if index == 1 else "Metallic"])
        return material

    fixtures = [classify_fixture(branches) for branches in (1, 2, 3, 4)]
    case("classify_material sees unaccepted nodes behind accepted ones (4 shapes)", [material_bake],
         lambda: tuple(material_bake.classify_material(m) for m in fixtures), ("complex",) * len(fixtures))

    # 2. _find_principled_through_surface: reroutes between the Principled
    #    and the output.  The id()-keyed walk reported a cycle.
    material, tree, principled, output = new_material("surface_reroutes")
    tree.links.new(reroute_chain(tree, principled.outputs[0], 8), output.inputs["Surface"])
    case("a Principled behind 8 reroutes is still found", [material_bake],
         lambda: material_bake.classify_material(material), "simple")

    # 3. exporter._skip_reroutes: reroutes between a Bump and the Normal
    #    input.  The id()-keyed walk returned None and the bump was dropped.
    material, tree, principled, _ = new_material("bump_reroutes")
    bump = tree.nodes.new("ShaderNodeBump")
    tree.links.new(reroute_chain(tree, bump.outputs[0], 8), principled.inputs["Normal"])

    def skip():
        node = exporter._skip_reroutes(principled.inputs["Normal"].links[0].from_node)
        return node.bl_idname if node is not None else None
    case("a Bump behind 8 reroutes is still found", [exporter], skip, "ShaderNodeBump")

    # 4. exporter._walk_socket_for_image_node: an image behind pass-through
    #    nodes that each have a second linked input.  The id()-keyed walk
    #    lost the image and exported the socket default.
    material, tree, principled, _ = new_material("image_chain")
    image = bpy.data.images.new("graph_walk_image", 4, 4)
    texture = tree.nodes.new("ShaderNodeTexImage")
    texture.image = image
    socket = texture.outputs["Color"]
    for _ in range(6):
        hue = tree.nodes.new("ShaderNodeHueSaturation")
        value = tree.nodes.new("ShaderNodeValue")
        math_node = tree.nodes.new("ShaderNodeMath")
        tree.links.new(value.outputs[0], math_node.inputs[0])
        tree.links.new(math_node.outputs[0], hue.inputs["Fac"])
        tree.links.new(socket, hue.inputs["Color"])
        socket = hue.outputs[0]
    tree.links.new(socket, principled.inputs["Base Color"])
    case("an image behind 6 Hue/Saturation nodes is still found", [exporter],
         lambda: exporter._walk_socket_for_image_node(principled.inputs["Base Color"]) is not None, True)

    # 5. The world walk must terminate on a cyclic graph (it compared the
    #    last Background node with `is`, which is never true for two
    #    wrappers of one node, so the cycle was revisited forever).
    world = bpy.data.worlds.new("cyclic_world")
    tree = world.node_tree
    background = tree.nodes["Background"]
    gamma = tree.nodes.new("ShaderNodeGamma")
    tree.links.new(background.outputs[0], gamma.inputs["Color"])
    tree.links.new(gamma.outputs[0], background.inputs["Color"])
    walker = getattr(exporter, "_walk_world_for_environment", None)
    if walker is None:
        check("world walk terminates on a cyclic graph", False, "exporter._walk_world_for_environment not found")
    else:
        import signal

        def too_long(_signum, _frame):
            raise TimeoutError()
        signal.signal(signal.SIGALRM, too_long)
        signal.alarm(20)
        try:
            output = next(n for n in tree.nodes if n.bl_idname == "ShaderNodeOutputWorld")
            try:
                walker(output)
            except TimeoutError:
                raise
            except Exception:
                pass  # the answer is not under test here, only termination
            check("world walk terminates on a cyclic graph", True)
        except TimeoutError:
            check("world walk terminates on a cyclic graph", False, "still running after 20 s")
        finally:
            signal.alarm(0)

    print("GRAPHWALK %d failure(s)" % FAILURES)
    sys.exit(1 if FAILURES else 0)


main()
