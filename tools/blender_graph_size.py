"""Measure how large each Blender material's value graphs are, as the
expression VM would see them.  This is the measurement
docs/BLENDER_PROCEDURAL_TRANSLATION_DESIGN.md section 8 asks for before
Phase 1 is promised: does a compiled material fit the VM's 512 variable
slots (src/Library/Painters/ExpressionEval.h, kMaxSlots)?

A material is split into PROGRAMS: one per linked non-shader input of a
shader node (a BSDF's Base Color, a Mix Shader's Fac, ...), which is what
Phase 1 compiles to one expression painter.  For each program the cone of
value nodes feeding it is walked, node groups expanded per instance, and
counted two ways:

  slots, lower bound   every node output used by two or more inputs in the
                       cone becomes a named `def` (1 slot for a number, 3
                       for a vector or colour); single-use outputs are
                       inlined into their consumer's expression and take no
                       slot.  This is what a compiler that inlines
                       aggressively needs.
  slots, upper bound   every used node output is a `def`.  This is what a
                       compiler that names every node output needs.

Both exclude the VM's reserved context slots (kContextSlots below) and any
temporaries a node's own lowering introduces, so the lower bound is a true
lower bound and the upper bound is not a true upper bound: a node that
lowers to several statements (Map Range, a colour-mix mode, a Mapping)
adds to it.  `depth` is the longest chain of nodes, which bounds how deep
an inlined expression nests.

The same counts are made for the UNION of a material's programs, which is
what one multi-output program (Phase 2) would hold.

Repeat and closure zones are not unrolled by this walk; a material that
has one is flagged and reported apart.

Modes, as tools/blender_node_survey.py:

  collect (inside Blender, once per .blend):
      blender -b --factory-startup --disable-autoexec scene.blend \\
          --python tools/blender_graph_size.py -- out.json
  run     python3 tools/blender_graph_size.py run --blender <binary> \\
              --out <json dir> a.blend b.blend ...
  report  python3 tools/blender_graph_size.py report <json dir>

.blend files are opened with --disable-autoexec: they are untrusted data.
"""

from __future__ import annotations

import collections
import json
import os
import re
import subprocess
import sys

# Reserved context variables in the VM today (u, v, P, Po, N, fw, time,
# curv, curvR, fwo; 16 slots): ExpressionEval.h kContextSlotCount.  Phase 1 adds hit
# inputs as zero-argument builtins, not slots.
kContextSlots = 16
kMaxSlots = 512

_WIDTH = {"VALUE": 1, "INT": 1, "BOOLEAN": 1, "VECTOR": 3, "RGBA": 3, "ROTATION": 3}
_ZONE_NODES = ("GeometryNodeRepeatInput", "GeometryNodeRepeatOutput", "NodeClosureInput",
               "NodeClosureOutput", "NodeEvaluateClosure")


# ---------------------------------------------------------------------------
# collect (inside Blender)
# ---------------------------------------------------------------------------

def _collect(out_path):
    import bpy

    sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
    import blender_node_survey as survey

    def key_of(node, ctx):
        return (survey._ctx_key(ctx), node.as_pointer())

    def cone(root_socket, root_ctx, root_id):
        """Walk the value nodes feeding one input socket.  Returns
        (node count, {(node key, output id): [width, set of consuming
        input sockets]}, depth, has_zone, set of node keys)."""
        outputs, nodes, zone = {}, set(), False
        depth_of = {}

        def visit(socket, ctx, consumer):
            """Record the link into `socket`; return the source's depth."""
            nonlocal zone
            src, out, src_ctx = survey._source_ctx(socket, ctx)
            if src is None or out is None or out.type == "SHADER":
                return 0
            b = src.bl_idname
            if b in ("ShaderNodeGroup", "NodeGroupInput"):
                return 0                      # unresolved boundary: a constant
            if b in _ZONE_NODES:
                zone = True
            k = key_of(src, src_ctx)
            entry = outputs.setdefault((k, out.identifier), [_WIDTH.get(out.type, 0), set()])
            entry[1].add(consumer)
            if k in depth_of:
                return depth_of[k]
            nodes.add(k)
            depth_of[k] = 1                   # provisional, guards a cycle
            deepest = 0
            for sock in src.inputs:
                if sock.is_linked and sock.type != "SHADER":
                    deepest = max(deepest, visit(sock, src_ctx, (k, sock.identifier)))
            depth_of[k] = deepest + 1
            return depth_of[k]

        depth = visit(root_socket, root_ctx, root_id)
        return len(nodes), outputs, depth, zone, nodes

    def slots(outputs):
        lower = sum(width for width, users in outputs.values() if len(users) >= 2)
        upper = sum(width for width, _users in outputs.values())
        return lower, upper

    materials = []
    for material in bpy.data.materials:
        tree = material.node_tree
        if tree is None or material.users == 0 or getattr(material, "is_grease_pencil", False):
            continue
        outs = [n for n in tree.nodes if n.bl_idname == "ShaderNodeOutputMaterial"]
        if not outs:
            continue
        active = [n for n in outs if n.is_active_output and n.target in ("ALL", "CYCLES")]
        output = (active or [n for n in outs if n.target in ("ALL", "CYCLES")] or outs)[0]

        # Shader nodes reachable from the output, each with its group context.
        shader_nodes, stack, seen = [(output, ())], [(output, ())], {key_of(output, ())}
        while stack:
            node, ctx = stack.pop()
            for sock in node.inputs:
                if sock.type != "SHADER" or not sock.is_linked:
                    continue
                src, _out, src_ctx = survey._source_ctx(sock, ctx)
                if src is None or src.bl_idname in ("ShaderNodeGroup", "NodeGroupInput"):
                    continue
                k = key_of(src, src_ctx)
                if k not in seen:
                    seen.add(k)
                    shader_nodes.append((src, src_ctx))
                    stack.append((src, src_ctx))

        programs, union_outputs, union_nodes, any_zone = [], {}, set(), False
        for node, ctx in shader_nodes:
            for sock in node.inputs:
                if sock.type == "SHADER" or not sock.is_linked:
                    continue
                try:
                    count, outputs, depth, zone, nodes = cone(sock, ctx, (key_of(node, ctx), sock.identifier))
                except RecursionError:
                    programs.append({"sink": "%s.%s" % (survey._short(node.bl_idname), sock.name), "error": "recursion"})
                    continue
                if count == 0:
                    continue
                lower, upper = slots(outputs)
                programs.append({"sink": "%s.%s" % (survey._short(node.bl_idname), sock.name),
                                 "nodes": count, "lower": lower, "upper": upper, "depth": depth})
                any_zone = any_zone or zone
                union_nodes |= nodes
                for k, (width, users) in outputs.items():
                    entry = union_outputs.setdefault(k, [width, set()])
                    entry[1] |= users
        if not programs:
            continue
        lower, upper = slots(union_outputs)
        materials.append({"name": material.name, "programs": programs, "zone": any_zone,
                          "union": {"nodes": len(union_nodes), "lower": lower, "upper": upper}})
    with open(out_path, "w", encoding="utf-8") as stream:
        json.dump({"file": os.path.basename(bpy.data.filepath), "materials": materials}, stream)


# ---------------------------------------------------------------------------
# report (plain Python)
# ---------------------------------------------------------------------------

def _percentiles(values):
    values = sorted(values)
    pick = lambda q: values[min(len(values) - 1, int(len(values) * q))]
    return "median %d, p90 %d, p99 %d, max %d" % (pick(.5), pick(.9), pick(.99), values[-1])


def _report(json_dir):
    seen, materials, files = set(), [], 0
    for name in sorted(os.listdir(json_dir)):
        if not name.endswith(".json"):
            continue
        files += 1
        with open(os.path.join(json_dir, name), encoding="utf-8") as stream:
            data = json.load(stream)
        for material in data["materials"]:
            signature = (material["name"], json.dumps(material["programs"], sort_keys=True))
            if signature in seen:
                continue
            seen.add(signature)
            material["file"] = name
            materials.append(material)
    budget = kMaxSlots - kContextSlots
    programs = [p for m in materials for p in m["programs"] if "error" not in p]
    errors = sum(1 for m in materials for p in m["programs"] if "error" in p)
    zoned = [m for m in materials if m["zone"]]
    print("files %d; materials with at least one value program %d; programs %d (%d walk errors); "
          "%d materials have a repeat or closure zone (not unrolled, counted as authored)"
          % (files, len(materials), len(programs), errors, len(zoned)))
    print("slot budget per program: %d - %d reserved = %d" % (kMaxSlots, kContextSlots, budget))
    for label, field in (("nodes per program", "nodes"), ("slots, lower bound", "lower"),
                         ("slots, upper bound", "upper"), ("longest node chain", "depth")):
        print("  %-22s %s" % (label, _percentiles([p[field] for p in programs])))

    def over(field, items, getter):
        return [i for i in items if getter(i)[field] > budget]

    print("\nPhase 1 (one program per driven socket):")
    for label, field in (("lower bound", "lower"), ("upper bound", "upper")):
        bad = over(field, programs, lambda p: p)
        bad_materials = [m for m in materials if any(p.get(field, 0) > budget for p in m["programs"])]
        print("  over budget by the %s: %d programs (%.2f%%) in %d materials (%.2f%%)"
              % (label, len(bad), 100.0 * len(bad) / max(1, len(programs)),
                 len(bad_materials), 100.0 * len(bad_materials) / max(1, len(materials))))
    print("\nPhase 2 (one multi-output program per material):")
    for label, field in (("nodes", "nodes"), ("slots, lower bound", "lower"), ("slots, upper bound", "upper")):
        print("  %-22s %s" % (label, _percentiles([m["union"][field] for m in materials])))
    for label, field in (("lower bound", "lower"), ("upper bound", "upper")):
        bad = over(field, materials, lambda m: m["union"])
        print("  over budget by the %s: %d materials (%.2f%%)"
              % (label, len(bad), 100.0 * len(bad) / max(1, len(materials))))

    print("\nlargest programs (upper bound):")
    ranked = sorted(((p["upper"], p["lower"], p["nodes"], p["depth"], m["file"], m["name"], p["sink"])
                     for m in materials for p in m["programs"] if "error" not in p), reverse=True)
    for upper, lower, nodes, depth, file, name, sink in ranked[:15]:
        print("  upper %4d  lower %4d  nodes %4d  depth %3d  %s :: %s -> %s"
              % (upper, lower, nodes, depth, file[:28], name[:32], sink))


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
        sys.exit("usage: blender_graph_size.py run --blender <binary> --out <dir> files...")
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
