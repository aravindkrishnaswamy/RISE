"""Cycles vs RISE light-power parity through the Blender add-on.

Renders one white Lambertian floor tile (Principled, Roughness 1,
Specular IOR Level 0, so a pure Lambertian in both renderers) seen by an
orthographic camera, lit by a single light 20 m above it, once with
Cycles and once with RISE (Path Tracing Pel, OIDN off), and compares the
mean pixel value.  Covers every light type, Blender's "Normalize" on and
off, area-light shapes, sun angles and uniform / non-uniform object
scale -- the cases src/Blender/addons/rise_renderer/light_math.py converts
(unit-tested formula-by-formula by LightMathTest in test_hair_export.py).

Run (renders take all cores; never run two at once):

    ~/Applications/Blender.app/Contents/MacOS/Blender -b --factory-startup \\
        --python tools/blender_light_parity.py -- [output_dir]

Requires the add-on to be installed (symlinked) and the native bridge
built.  Exits non-zero when any GATED RISE/Cycles ratio leaves [0.995, 1.005]
-- tight enough that the 0.56%-dark disk / ellipse error this tool was
written against fails.  Point / spot lights read ~0.3% high because
Cycles' point light is a small sphere and RISE's a point.
"""

import math
import os
import sys
import tempfile

import bpy
from mathutils import Matrix

BAND = (0.995, 1.005)


def _mean(path):
    img = bpy.data.images.load(path)
    values = list(img.pixels)
    bpy.data.images.remove(img)
    return sum(values[i] for i in range(0, len(values), 4)) / (len(values) / 4)


def _build(kind, shape, size, size_y, normalize, obj_scale, angle, energy, shear=0.0, scale_x=1.0):
    """`shear` != 0 builds a GENUINELY sheared light: Blender stores object
    transforms as location / rotation / scale, so a sheared matrix_world
    is decomposed and the shear dropped.  Real shear only arises through a
    parent: the light becomes the child, rotated 45 degrees about Z, of an
    empty scaled (1 + shear, 1, 1)."""
    bpy.ops.wm.read_factory_settings(use_empty=True)
    bpy.ops.preferences.addon_enable(module="rise_renderer")
    scene = bpy.context.scene
    scene.render.resolution_x = scene.render.resolution_y = 16
    scene.view_settings.view_transform = "Standard"
    scene.render.image_settings.file_format = "OPEN_EXR"
    scene.world = bpy.data.worlds.new("black")
    scene.world.node_tree.nodes["Background"].inputs["Strength"].default_value = 0.0

    bpy.ops.mesh.primitive_plane_add(size=0.5, location=(0, 0, 0))
    floor = bpy.data.materials.new("white")
    principled = floor.node_tree.nodes["Principled BSDF"]
    principled.inputs["Base Color"].default_value = (1, 1, 1, 1)
    principled.inputs["Roughness"].default_value = 1.0
    principled.inputs["Specular IOR Level"].default_value = 0.0
    bpy.context.object.data.materials.append(floor)

    light = bpy.data.lights.new("L", kind)
    light.energy = energy
    light.normalize = normalize
    if kind == "AREA":
        light.shape, light.size, light.size_y = shape, size, size_y
    elif kind == "SUN":
        light.angle = angle
    else:
        light.shadow_soft_size = size
        if kind == "SPOT":
            light.spot_size = math.radians(60)
    light_object = bpy.data.objects.new("L", light)
    scene.collection.objects.link(light_object)
    if shear:
        parent = bpy.data.objects.new("P", None)
        scene.collection.objects.link(parent)
        parent.location = (0, 0, 20.0)
        parent.scale = (1.0 + shear, 1.0, 1.0)
        light_object.parent = parent
        light_object.rotation_euler = (0, 0, math.radians(45))
        light_object.scale = (obj_scale,) * 3
    else:
        light_object.location = (0, 0, 20.0)
        light_object.scale = (obj_scale * scale_x, obj_scale, obj_scale)
    bpy.context.view_layer.update()

    camera = bpy.data.cameras.new("C")
    camera.type, camera.ortho_scale = "ORTHO", 0.2
    camera_object = bpy.data.objects.new("C", camera)
    camera_object.location = (0, 0, 5)
    scene.collection.objects.link(camera_object)
    scene.camera = camera_object
    return scene


def _render(scene, engine, path):
    scene.render.engine = engine
    if engine == "CYCLES":
        scene.cycles.samples, scene.cycles.device, scene.cycles.use_denoising = 64, "CPU", False
    else:
        scene.rise.pixel_samples = 64
        scene.rise.rasterizer_type = "1"      # Path Tracing (Pel)
        scene.rise.oidn_denoise = False
    scene.render.filepath = path
    bpy.ops.render.render(write_still=True)
    return _mean(path)


# (kind, shape, size, size_y, normalize, object scale, sun angle, energy,
#  in-plane shear, x scale)
CASES = [
    ("POINT", None, 1.0, 1.0, True, 1.0, 0.0, 100.0, 0.0, 1.0),
    ("POINT", None, 1.0, 1.0, False, 1.0, 0.0, 100.0, 0.0, 1.0),
    ("POINT", None, 0.0, 0.0, True, 1.0, 0.0, 100.0, 0.0, 1.0),
    ("POINT", None, 0.0, 0.0, False, 1.0, 0.0, 100.0, 0.0, 1.0),
    ("SPOT", None, 0.5, 0.5, True, 1.0, 0.0, 100.0, 0.0, 1.0),
    ("SPOT", None, 0.5, 0.5, False, 1.0, 0.0, 100.0, 0.0, 1.0),
    ("SUN", None, 0.0, 0.0, True, 1.0, math.radians(30), 2.0, 0.0, 1.0),
    ("SUN", None, 0.0, 0.0, False, 1.0, math.radians(30), 2.0, 0.0, 1.0),
    ("SUN", None, 0.0, 0.0, False, 1.0, math.radians(10), 2.0, 0.0, 1.0),
    ("SUN", None, 0.0, 0.0, False, 1.0, 0.0, 2.0, 0.0, 1.0),
    ("AREA", "SQUARE", 2.0, 2.0, True, 1.0, 0.0, 100.0, 0.0, 1.0),
    ("AREA", "SQUARE", 2.0, 2.0, False, 1.0, 0.0, 100.0, 0.0, 1.0),
    ("AREA", "SQUARE", 2.0, 2.0, True, 2.0, 0.0, 100.0, 0.0, 1.0),
    ("AREA", "SQUARE", 2.0, 2.0, False, 2.0, 0.0, 100.0, 0.0, 1.0),
    ("AREA", "SQUARE", 2.0, 2.0, True, 1.0, 0.0, 100.0, 0.0, 2.0),
    ("AREA", "RECTANGLE", 3.0, 1.0, False, 1.0, 0.0, 100.0, 0.0, 1.0),
    ("AREA", "DISK", 3.0, 3.0, True, 1.0, 0.0, 100.0, 0.0, 1.0),
    ("AREA", "DISK", 2.0, 2.0, False, 1.0, 0.0, 100.0, 0.0, 1.0),
    ("AREA", "ELLIPSE", 4.0, 1.5, True, 1.0, 0.0, 100.0, 0.0, 1.0),
    ("AREA", "ELLIPSE", 4.0, 1.5, False, 1.0, 0.0, 100.0, 0.0, 1.0),
]

# Reported, NOT gated: a genuinely sheared panel.  RISE emits exactly the
# stated power over the true parallelogram (Normalize on: the same flux as
# the unsheared panel; off: power x that parallelogram's area); Cycles 5.3
# normalises by |X| |Y| instead and does not -- measured 2026-11-14 at
# rise/cycles = 0.645 (Normalize on) and 0.516 (off).
KNOWN_DIVERGENCES = [
    ("AREA", "SQUARE", 2.0, 2.0, True, 1.0, 0.0, 100.0, 1.0, 1.0),
    ("AREA", "SQUARE", 2.0, 2.0, False, 1.0, 0.0, 100.0, 1.0, 1.0),
]


def main():
    argv = sys.argv[sys.argv.index("--") + 1:] if "--" in sys.argv else []
    out_dir = argv[0] if argv else tempfile.mkdtemp(prefix="rise_light_parity_")
    os.makedirs(out_dir, exist_ok=True)
    failures = 0
    rows = [(case, True) for case in CASES] + [(case, False) for case in KNOWN_DIVERGENCES]
    for (kind, shape, size, size_y, normalize, obj_scale, angle, energy, shear, scale_x), gated in rows:
        tag = (f"{kind}_{shape or ''}_{size:g}x{size_y:g}_{'norm' if normalize else 'unnorm'}"
               f"_scale{obj_scale:g}_angle{math.degrees(angle):g}_shear{shear:g}_sx{scale_x:g}")
        scene = _build(kind, shape, size, size_y, normalize, obj_scale, angle, energy, shear, scale_x)
        cycles = _render(scene, "CYCLES", os.path.join(out_dir, tag + "_cycles.exr"))
        rise = _render(scene, "RISE_RENDER", os.path.join(out_dir, tag + "_rise.exr"))
        ratio = rise / cycles if cycles > 0.0 else float("inf")
        ok = BAND[0] <= ratio <= BAND[1]
        if gated:
            failures += 0 if ok else 1
            status = "ok  " if ok else "FAIL"
        else:
            status = "info"
        print(f"PARITY {status} {tag:58s} cycles={cycles:.6f} rise={rise:.6f} ratio={ratio:.4f}")
    print(f"PARITY {len(CASES) - failures}/{len(CASES)} within {BAND}")
    sys.exit(1 if failures else 0)


main()
