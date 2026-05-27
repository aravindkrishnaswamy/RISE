"""Material baking + node-graph classification for the RISE bridge.

See ``docs/BLENDER_MATERIAL_TRANSLATION.md`` for the design.  This
module owns:

- ``classify_material(mat)`` — decides whether a material's node graph
  is "simple" (direct procedural translation possible) or "complex"
  (must be baked to image textures).
- ``bake_material_to_images(obj, mat, options)`` — drives Blender's
  ``bpy.ops.object.bake`` to render the material's BSDF channels to
  PNG files, then stores the resulting paths as ID-property metadata
  on the material so subsequent renders skip the bake.
- ``baked_paths(mat)`` — accessor returning the cached PNG paths, or
  ``None`` if the material hasn't been baked.
- ``baked_cache_is_stale(mat)`` — content-hash comparison so the UI
  can flag materials needing re-bake after a node-graph edit.
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
expects to be invoked from a user click in a 3D View).  The
industry-standard workflow used by LuxCore, Octane, and Renderman
Blender add-ons is exactly the explicit-operator UX we settled on
here — same reason.

`engine.render()` detects unbaked complex materials and aborts with
a clear actionable error pointing the user at the Bake button.

The simple-graph translator (the "Option A" half of the hybrid
described in BLENDER_MATERIAL_TRANSLATION.md) lives in
``exporter.py``; this module is responsible only for the "Option B"
bake side and the classifier that routes between them.
"""

from __future__ import annotations

import math
import os
import tempfile
from typing import Optional

import bpy

# ---------------------------------------------------------------------------
# Classifier
# ---------------------------------------------------------------------------

# Nodes the simple translator can handle (or transparently traverse).
# See docs/BLENDER_MATERIAL_TRANSLATION.md "Supported node table" — keep
# the two in sync.
_SIMPLE_TRAVERSABLE_NODES = frozenset({
    "NodeReroute",
    "ShaderNodeRGB",
    "ShaderNodeValue",
    "ShaderNodeTexImage",
    "ShaderNodeTexCoord",
    "ShaderNodeMapping",
    "ShaderNodeTexNoise",
    "ShaderNodeTexVoronoi",
    "ShaderNodeTexChecker",
    "ShaderNodeValToRGB",       # Color Ramp
    "ShaderNodeMix",            # 4.x mix node
    "ShaderNodeMixRGB",         # legacy mix node
    "ShaderNodeMath",           # Some Math ops fold inline; full Math is force-bake
    "ShaderNodeRGBCurve",       # currently lossy pass-through
    "ShaderNodeHueSaturation",  # currently lossy pass-through
    "ShaderNodeBrightContrast", # currently lossy pass-through
    "ShaderNodeGamma",          # currently lossy pass-through
    "ShaderNodeInvert",         # currently lossy pass-through
    "ShaderNodeBump",
    "ShaderNodeNormalMap",
})

# Nodes that force the bake fallback — see docs/BLENDER_MATERIAL_TRANSLATION.md
# "Force-bake list".  Anything not in _SIMPLE_TRAVERSABLE_NODES is implicitly
# force-bake; this set is for documentation / fast-path diagnostics.
_FORCE_BAKE_NODES = frozenset({
    "ShaderNodeAmbientOcclusion",
    "ShaderNodeMixShader",
    "ShaderNodeAddShader",
    "ShaderNodeGeometry",
    "ShaderNodeLayerWeight",
    "ShaderNodeFresnel",
    "ShaderNodeWireframe",
    "ShaderNodeTexWave",
    "ShaderNodeTexMagic",
    "ShaderNodeTexBrick",
    "ShaderNodeTexGradient",
    "ShaderNodeTexIES",
    "ShaderNodeTexPointDensity",
    "ShaderNodeGroup",
})


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
        node_id = id(cursor)
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
        # — force-bake the whole material.
        return None
    return None


def classify_material(material) -> str:
    """Return ``'simple'`` or ``'complex'``.

    Simple iff:
      - exactly one Principled BSDF reaches Material Output.Surface,
      - every node anywhere reachable from any Principled input is in
        ``_SIMPLE_TRAVERSABLE_NODES``,
      - no socket chain exceeds 16 nodes.
    Otherwise complex.

    The classifier inspects the WHOLE reachable subgraph from every
    Principled input.  An Ambient Occlusion node feeding a Color Ramp
    that feeds Base Color forces complex; an Image Texture chained
    via Color Ramp + Math feeding Base Color stays simple.
    """

    if material is None or not material.use_nodes or material.node_tree is None:
        # No node tree (just diffuse_color on legacy materials): trivially
        # simple — exporter uses the constant colour.
        return "simple"

    nt = material.node_tree
    output_node = next(
        (n for n in nt.nodes if n.bl_idname == "ShaderNodeOutputMaterial"),
        None,
    )
    if output_node is None:
        # No output node — fall back to default principled detection;
        # treat as simple so the existing exporter's first-principled
        # walk catches it.
        return "simple"

    principled = _find_principled_through_surface(output_node)
    if principled is None:
        return "complex"

    # Walk every Principled input back through the graph.  If any
    # reachable node is not in _SIMPLE_TRAVERSABLE_NODES, complex.
    stack = []
    for socket in principled.inputs:
        if socket.is_linked:
            stack.append((socket.links[0].from_node, 0))

    visited: set[int] = set()
    while stack:
        node, depth = stack.pop()
        if depth > 16:
            return "complex"
        node_id = id(node)
        if node_id in visited:
            continue
        visited.add(node_id)

        bl = node.bl_idname
        if bl not in _SIMPLE_TRAVERSABLE_NODES:
            return "complex"

        for socket in node.inputs:
            if socket.is_linked:
                stack.append((socket.links[0].from_node, depth + 1))

    return "simple"


# ---------------------------------------------------------------------------
# Bake metadata storage on materials
# ---------------------------------------------------------------------------

# ID-property keys used to persist bake outputs on a Blender Material.
# Lives in `mat["<key>"]` (a generic StringProperty added on demand).
# Keys are chosen to be obvious in the Blender UI's Custom Properties
# panel — easy to diagnose / clear by hand if needed.
BAKED_DIFFUSE_KEY = "rise_baked_diffuse_path"
BAKED_ROUGHNESS_KEY = "rise_baked_roughness_path"
BAKED_NORMAL_KEY = "rise_baked_normal_path"
BAKED_RESOLUTION_KEY = "rise_baked_resolution"
BAKED_FRAME_KEY = "rise_baked_frame"
# Content hash of the material's node graph at the last successful
# bake.  Set only when `out` was non-empty (at least one channel
# baked).  Used by `baked_cache_is_stale` to decide whether the
# cached PNGs match the current graph or need re-baking.
BAKED_GRAPH_HASH_KEY = "rise_baked_graph_hash"
# Content hash captured at the last bake ATTEMPT, regardless of
# whether the attempt succeeded.  Used by the render-gate filter:
# the gate fires only for complex materials with NO attempted-hash
# OR whose graph has changed since the last attempt.  Prevents the
# user from getting stuck in a render → bake → render loop when a
# material's bake can't succeed (hide_render, UV layout, etc.); the
# render then proceeds and the exporter uses flat-colour fallback
# for the never-baked material.
BAKED_ATTEMPTED_HASH_KEY = "rise_baked_attempted_hash"


def _material_graph_hash(material) -> str:
    """Compute a stable content hash of a material's node graph.

    Captures everything that would affect the baked outputs:
    - Every node's ``bl_idname`` and ``name``
    - Every input socket's default value (for unlinked sockets)
    - Every link (from_node.name + from_socket.name)
    - For ``ShaderNodeTexImage``: the image data block's ``name`` and
      ``filepath`` (catches "user swapped the image but kept the node")
    - For ``ShaderNodeValToRGB``: the colour-ramp stop positions and
      colours (catches "user dragged a ColorRamp handle")

    Used by the auto-bake path to decide whether the cached PNGs are
    stale.  Cheap: a 30-node material hashes in < 1 ms.

    Stored alongside the bake outputs as ``rise_baked_graph_hash``.
    """

    import hashlib
    h = hashlib.md5()
    if material is None or not material.use_nodes or material.node_tree is None:
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
        # swapped the assigned image".  Image PIXEL contents aren't
        # hashed (would be too slow); the filepath / name change is
        # the usual user-edit signal.
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
    """Returns the cached bake outputs for a material, or None if the
    material hasn't been baked yet.  Dict keys: ``diffuse``,
    ``roughness``, ``normal``; values are filesystem paths.  Missing
    channels (e.g. material with no normal map) won't appear in the
    dict.
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
    """Remove the bake metadata so subsequent renders fall back to
    direct procedural translation (or force a re-bake)."""
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
      - simple materials → False (direct translation, no bake)
      - complex with no viable proxy → False (can't bake; flat-colour
        fallback handled by exporter)
      - complex with viable proxy AND no attempted-hash → True
        (never tried)
      - complex with viable proxy AND attempted-hash differs from
        current graph hash → True (user edited since last attempt)
      - complex with viable proxy AND attempted-hash matches current
        graph → False (we tried; whatever the outcome, render
        proceeds with what we've got — successful PNGs consumed,
        failed bakes use flat-colour fallback)
    """

    if classify_material(material) != "complex":
        return False
    if find_bakeable_proxy_object(scene, material) is None:
        return False
    attempted_hash = material.get(BAKED_ATTEMPTED_HASH_KEY)
    if not attempted_hash:
        return True
    return str(attempted_hash) != _material_graph_hash(material)


def baked_cache_is_stale(material) -> bool:
    """Returns True if the material has bake metadata but the graph
    hash no longer matches — i.e. the user has edited the node graph
    since the last bake.  Also True if any of the cached PNG paths
    on disk are missing (cleaned up by a temp-dir sweep, for
    example).  False when the cache is current or absent entirely.

    Used by ``auto_bake_complex_materials`` to decide whether to
    re-bake on the next render.
    """

    if material is None:
        return False
    cached_hash = material.get(BAKED_GRAPH_HASH_KEY)
    if not cached_hash:
        return False  # never baked → not stale (just absent)
    if str(cached_hash) != _material_graph_hash(material):
        return True
    # Hash matches; verify each cached PNG still exists on disk.  A
    # vacated temp dir invalidates the cache transparently.
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
         and n.image is image),
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

    Uses Blender's ``Image.save_render`` if a render context is
    available, falling back to ``Image.filepath_raw = …; save()``.
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
    material's ID properties with the resulting paths and returns the
    same dict that :func:`baked_paths` would.

    Requires:
    - The scene's render engine is CYCLES (caller switches and
      restores).
    - ``obj`` is selectable in the current view layer.

    Channels:
    - Diffuse (Base Color) — always baked
    - Roughness — baked when ``bake_roughness`` is True
    - Normal — baked when ``bake_normal`` is True

    The output PNGs are colour-space-tagged appropriately when read
    back via the bridge (sRGB for diffuse, Linear for roughness,
    ROMM-Linear for normal).
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
    # `bpy.context.selected_objects` / `view_layer` / `object` are
    # blocked inside Blender's restricted context (e.g. when this
    # function is reached from a `render_init` handler).  The caller
    # in `auto_bake_complex_materials` provides a
    # `bpy.context.temp_override` populated with the required
    # selection state, but as belt-and-suspenders, fail soft on the
    # state-snapshot reads.  When running from the user-driven
    # operator path (`bake_complex_materials_in_scene`) the reads
    # succeed and we restore selection on exit; when running from the
    # handler path we don't bother (there's no user selection to
    # preserve).
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
        # No active 3D view in this context — selection is managed by
        # the temp_override the caller installed, so deselect-all is
        # not necessary.
        pass
    obj.select_set(True)
    # Setting active via the view layer directly (rather than
    # `bpy.context.view_layer.objects.active = obj`) avoids the
    # restricted-context attribute access.  Falls through gracefully
    # if no view layer is accessible.
    try:
        bpy.context.view_layer.objects.active = obj
    except AttributeError:
        # The temp_override owns the active-object slot; our caller
        # already pointed it at `obj`, so no further action needed.
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
        # Diffuse (Base Color).
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

        # Restore selection / active.  Both errors here are
        # context-restriction symptoms when running from a handler —
        # they're harmless because the temp_override the caller
        # installed gets unwound on its own.
        try:
            bpy.ops.object.select_all(action="DESELECT")
            for o in prev_selected:
                if o.name in bpy.context.view_layer.objects:
                    o.select_set(True)
            bpy.context.view_layer.objects.active = prev_active
        except (RuntimeError, AttributeError):
            pass

        # We INTENTIONALLY don't bpy.data.images.remove() the temp
        # images — they're saved to disk now, and removing the data
        # block doesn't delete the file; keeping the data block lets
        # the user inspect the bake result in Blender's Image Editor.

    # Stamp the "we tried this graph" marker UNCONDITIONALLY, even if
    # the bake produced no usable channels.  The render-gate predicate
    # `needs_bake_attempt` keys off this so the user isn't trapped in
    # a render → bake → render loop when a material's bake can't
    # succeed (e.g. hide_render-disabled GN-instanced meshes).  The
    # SUCCESS marker (`BAKED_GRAPH_HASH_KEY`) is set below only when
    # at least one channel landed, so `baked_paths()` still returns
    # None for failed-bake materials and the exporter routes them
    # through its flat-colour fallback.
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
    to the exporter's flat-colour fallback instead of gating render.
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


def bake_complex_materials_in_scene(
    scene,
    *,
    resolution: int = 1024,
    only_unbaked: bool = False,
    report=print,
) -> int:
    """Walk the scene, find every object whose material classifies as
    complex, and bake each.  Returns the number of materials baked.

    ``only_unbaked``: when True, skip materials that already have a
    ``rise_baked_diffuse_path`` ID property — useful for "bake
    incrementally" workflows.

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
            if classify_material(mat) != "complex":
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
                report(f"[RISE bake] {mat.name}: bake failed; will render as flat colour")
    return baked
