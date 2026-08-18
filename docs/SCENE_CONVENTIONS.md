# RISE Scene-Authoring Conventions

This doc captures the scene-file conventions that have caused recurring
"why does my scene look wrong?" bugs.  When a scene renders unexpectedly
(too dark, geometry oriented wrong, reflectance off, etc.) before
opening the integrator code, walk this list.  When authoring a new
scene, lean on the **anti-patterns** at the bottom of each section
to avoid the standard traps.

The companion skill is
[docs/skills/effective-rise-scene-authoring.md](skills/effective-rise-scene-authoring.md);
this doc is the reference, the skill is the procedure.

---

## 1. Directional light `direction` is FROM-surface-TO-light

This has bitten us at least twice; capturing it loudly so it stops.

```text
directional_light
{
    name        key
    power       3.14
    color       1 1 1
    direction   X Y Z      # vector pointing FROM any surface TO the light source
}
```

Source of truth: [src/Library/Lights/DirectionalLight.cpp:48](../src/Library/Lights/DirectionalLight.cpp):

```cpp
Scalar fDot = Vector3Ops::Dot( vDirection, ri.vNormal );
if( fDot <= 0.0 ) {
    return;        // surface is in shadow, nothing reflected from this light
}
```

A surface is lit when `N · direction > 0`.  In other words, `direction`
points **toward** the light source from the geometry's perspective.
This is the OpenGL / glTF *to-light* convention, **not** the
*shine-direction* convention some other engines use.

### Worked example

Camera at `(0, 0, +5)` looking at the origin.  A typical asset (sphere,
helmet, …) at the origin has camera-facing surface normals close to
`(0, 0, +1)`.  To light the visible side of the asset:

| Light placement (mental model) | `direction` value | `N · dir` for camera-facing N=(0,0,1) | Lit? |
|---|---|---|---|
| Light source above-front-right of camera (where you'd put a key) | `0.4 0.4 0.7` | +0.7 | yes |
| Light source dead in front of camera | `0 0 1` | +1.0 | yes (matches `scenes/Tests/Cameras/realistic.RISEscene`) |
| Light source above-back-left of asset | `-0.4 -0.4 -0.7` | -0.7 | NO — asset front renders dark |

If a Lambertian-white sphere with `power 3.14` and ambient 0.25 comes
out dark gray, the direction is wrong.

### Cross-check: the importer must do the same flip

`KHR_lights_punctual` directional lights shine down their local `-Z`
axis.  After applying the node's world transform, you have a
*shine direction* — exactly opposite of what RISE wants.  The glTF
importer at
[src/Library/Importers/GLTFSceneImporter.cpp](../src/Library/Importers/GLTFSceneImporter.cpp)
in `CreateLightForNode` negates the shine direction before handing
it to `Job::AddDirectionalLight`.  **Always do that conversion when
importing from a foreign format.**

### Anti-patterns

- Copy-pasting `direction X Y Z` from another graphics tool's scene
  description without checking which convention that tool uses.
- Using a "negative-Z so it shines toward -Z" mental model from
  rasterizers (e.g. Unity / Unreal directional lights) — RISE is the
  opposite.
- "Direction toward asset" — also wrong; it's "direction FROM asset".

---

## 2. Spot light `target` defines the cone axis (different convention)

```text
spot_light
{
    name     torch
    power    50
    color    1 1 1
    position Px Py Pz
    target   Fx Fy Fz   # any point along the cone-axis BEYOND the source
    inner    20         # inner cone HALF-angle, in DEGREES
    outer    45         # outer cone HALF-angle, in DEGREES
}
```

For spots, RISE wants a **target point**, not a direction (the
`spot_light` chunk parameters are `target`/`inner`/`outer` — see the
descriptor in
[src/Library/Parsers/ChunkParserRegistry.cpp](../src/Library/Parsers/ChunkParserRegistry.cpp)).
Internally it computes the cone-axis shine direction as
`target - position`; `inner`/`outer` are cone **half-angles in
degrees** (converted to radians at parse time; defaults 45/90).  This
is symmetric to how SpotLight tests `dot(toLight, -vDirection)` at hit
time.  See
[src/Library/Lights/SpotLight.cpp:37](../src/Library/Lights/SpotLight.cpp).

The glTF importer for spots passes `position + shine_direction` as the
target point — that's correct (no extra flip needed).

---

## 3. Light power semantics

`power` on each light type acts as a multiplier on `color`.  Roughly:

| Light type | Effective radiance at hit |
|---|---|
| `directional_light` | `color · power` (since rays are parallel; no distance falloff) |
| `omni_light` (point) | `color · power / r²` |
| `spot_light` | `color · power / r²` within the cone (cosine fall-off across inner→outer angles) |
| `ambient_light` | `color · power` (constant, all surfaces) — **do not use; see §3.5** |

`power = π` (often written `3.14`) is a common starting point for a
directional sun-like key on a Lambertian-white scene because the
Lambertian BRDF's `1/π` factor cancels with it: a fully-lit Lambertian
white surface returns `color = 1`.

An emissive material has no `power`; its `scale` multiplies the
`exitance` painter's colour instead (§3.5).  `rect_light` and
`shape_light` have no `power` either — they take `exitance`, which is
that same per-unit-area quantity, and the parser rejects a `power` line
on either.

---

## 3.5. Which light kind to use — area lights are the norm

House convention (project owner, 2026-08-12).  This is a **convention**
for hand-authored scenes: the parser accepts every light chunk listed
below, and nothing in the scene loader rejects one.  The **agent
surface** is the one place it is enforced in code — see the end of this
section.

**Most scenes should be lit by AREA LIGHTS: an object wearing a
luminaire material, in most cases a rectangle.**

### The rectangle case: `rect_light` (one chunk)

Because a rectangular panel is the common case, it has a chunk of its
own (2026-08-12):

```text
rect_light
{
	name		window_light
	center		0 4 0
	size		2 1
	facing		0 -1 0
	color		1.0 0.95 0.85
	exitance	6000
}
```

- `center` is the panel's world-space centre; `size` is `width height`
  in scene units; `facing` is **the direction the panel emits toward**
  (a ceiling panel lighting the floor is `0 -1 0`).  It need not be unit
  length, but a zero vector fails the load.
- The panel emits toward `facing` **only** — its back face is not hit at
  all.  `facing` becomes the quad's geometric normal exactly.
- `exitance` is emitted radiance **per unit area** (it is the `scale` of
  the luminaire material below), so the same number on a panel twice the
  size delivers twice the light.  It must be > 0.  **There is no `power`
  parameter** — `power` on the zero-area lights is a different quantity,
  and writing one here fails the load rather than being ignored.
- `color` defaults to `1 1 1`, linear Rec.709.

`rect_light` is **parse-time sugar**: the parser expands it into exactly
the four chunks below, so the renderer treats it as an ordinary emitting
object.  Its `name` names the OBJECT; the helpers it also creates are
`<name>__pnt` (painter), `<name>__mat` (luminaire material) and
`<name>__geo` (quad).  A collision on any of the four fails the load
with the ordinary duplicate-name error.  The scene file keeps the
compact text — save serializes the CST document, so a `rect_light` saves
back as a `rect_light`.

### The solid case: `shape_light` (one chunk)

A bulb, an orb, a lamp body or any other glowing solid has a chunk of
its own too (2026-08-12):

```text
shape_light
{
	name		bulb_light
	shape		sphere
	center		0 3 0
	size		0.15
	color		1.0 0.92 0.8
	exitance	400
}
```

- `shape` is one of `sphere`, `ellipsoid`, `box` or `cylinder`, and it
  fixes what `size` means:

  | `shape` | `size` |
  |---|---|
  | `sphere` | one number — the radius |
  | `ellipsoid` | three numbers — the semi-axis radii, X Y Z |
  | `box` | three numbers — width height depth |
  | `cylinder` | two numbers — radius height (the cylinder stands on the **+Y** axis until `orientation` turns it) |

  A wrong number of `size` values fails the load and names the count
  that shape needs; an unknown `shape` value fails the load and names
  the four valid ones.
- `center` is the solid's world-space centre.  `orientation` is an
  optional Euler rotation in **degrees**, applied about `center` —
  the same parameter `standard_object` takes.  It defaults to `0 0 0`;
  a sphere is unchanged by it, and it is what turns a cylinder off its
  default +Y axis, or tilts a box or an ellipsoid.
- **There is no `facing` parameter, and none is needed.**  All four
  shapes are closed solids whose surface normal points outward
  everywhere, and lambertian emission is one-sided about that normal,
  so a `shape_light` emits outward over its whole surface and inward
  nowhere.  (`rect_light` needs `facing` only because a quad is an open
  surface with two sides.)
- `exitance` is emitted radiance **per unit area** — the same quantity
  `rect_light`'s `exitance` is — so the same number on a bigger solid
  delivers more light.  It must be > 0.  **There is no `power`
  parameter**, and writing one fails the load rather than being
  ignored.
- `color` defaults to `1 1 1`, linear Rec.709.

Like `rect_light`, `shape_light` is **parse-time sugar**: the parser
expands it into exactly the same four-chunk area-light chain — a
`uniformcolor_painter` holding `color`, a `lambertian_luminaire_material`
whose `exitance` is that painter and whose `scale` is the chunk's
`exitance`, the shape's own geometry chunk, and a `standard_object`
placing it at `center` with `orientation`.  The derived names are the
same three `rect_light` uses: `<name>__pnt`, `<name>__mat` and
`<name>__geo`; the chunk's own `name` names the OBJECT.  A collision on
any of the four fails the load with the ordinary duplicate-name error.
The scene file keeps the compact text — a `shape_light` saves back as a
`shape_light`.

Being an ordinary object, a `shape_light` is visible in the frame,
casts soft shadows, and falls off with distance, exactly like
`rect_light`.

### Any other shape: the four-chunk chain

There is no `area_light` chunk.  `rect_light` is a *rectangle* and
`shape_light` is a sphere, ellipsoid, box or cylinder; an emitter of any
other shape — a mesh fixture, a torus, a curved patch — is written as
the chain both of those expand into, as used by
[scenes/FeatureBased/PathTracing/pt_jewel_vault.RISEscene](../scenes/FeatureBased/PathTracing/pt_jewel_vault.RISEscene):

```text
uniformcolor_painter
{
	name				pnt_window
	color				1.0 0.95 0.85
}
lambertian_luminaire_material
{
	name				window_mat
	exitance			pnt_window
	scale				6000.0
	material			none
}
clippedplane_geometry
{
	name				window_geo
	pta					-1.0 2.0 -1.0
	ptb					 1.0 2.0 -1.0
	ptc					 1.0 2.0  1.0
	ptd					-1.0 2.0  1.0
}
standard_object
{
	name				window_obj
	geometry			window_geo
	material			window_mat
}
```

- `material none` means the surface only emits (no underlying BRDF).
- `phong_luminaire_material` is the directional sibling (same
  `exitance` / `scale` / `material`, plus `N`).
- Any geometry works; a mesh wearing the material emits from every
  triangle.  `clippedplane_geometry` is a quad given by four corner
  points and is the usual choice for a window or a panel.
- **Sidedness is the corner winding plus `doublesided`.**  The quad's
  normal is `normalize(Cross(ptb - pta, ptd - pta))`, and a Lambertian
  luminaire emits only where `Dot(out, N) > 0`.  With `doublesided TRUE`
  (the default) a back-face hit flips the normal toward the ray, so the
  quad emits from BOTH faces; with `doublesided FALSE` the back face is
  not hit at all and only the winding's face emits.  `rect_light` sets
  `FALSE` and derives the winding from `facing`.
- **`scale` sets exitance — brightness per unit area** — so the same
  `scale` on a panel twice the size delivers twice the light.  The
  number is therefore scene-dependent, and the values in `scenes/` span
  four orders of magnitude: single digits and tens for interior fill
  panels, hundreds to tens of thousands for small bright emitters (the
  jewel-vault's 11 × 0.4 slot window, seen through a narrow gap, uses
  6000).
- An area light has real area, so it produces soft shadows and physical
  falloff, and it is visible in the frame wherever the camera can see
  it.  Point it away from the camera, or place it outside the view, if
  you do not want to see the emitter itself.

**`hosek_wilkie_skylight` is fine** — it is a physically based analytic
sun-and-sky model, not an anachronism.  It builds the scene's global
radiance map and (unless `create_sun false`) a matched
`directional_light` named `__hw_sun__`.

**`omni_light`, `spot_light` and `directional_light` are for special
cases only.**  They are zero-area idealizations: the light arrives from
a single point or from infinity, so their shadows have hard edges with
no penumbra at any distance, no material governs what they emit, and
nothing about them appears in the frame.  Reach for them when that is
what you actually want (a stand-in sun on a scene with no sky, a hard
key for a diagram, a cheap probe while iterating), not as the default
way to light a scene.  On the **agent surface** they additionally carry
a **confirmation requirement**: EVERY creation request for one of these
three kinds is refused once, stating the physics and the alternatives,
and the identical request lands when re-issued unchanged — from the
first request, not past a free allowance (the free budget of 2 this
mechanism shipped with on 2026-08-12 was removed 2026-08-13: it
exempted exactly the uses this confirmation exists to make deliberate).
`shape_light`, `rect_light`, an emissive object and
`hosek_wilkie_skylight` carry no confirmation requirement at all.

**`ambient_light`: never.**  It contributes the same `color · power` at
every shading point, scaled only by that surface's own reflectance; it
has no position and no direction (`name`, `power`, `color` are its only
parameters), and it casts no shadow ray
([RayCaster.h](../src/Library/Rendering/RayCaster.h) says so in the
shadow-routing contract: *"Ambient casts no shadow ray"*).  So it can
produce neither a shadow nor any falloff — it is a pre-GI flat-fill hack
([AmbientLight.h](../src/Library/Lights/AmbientLight.h)'s own header
comment says it exists for the ray tracer, which cannot do global
illumination).  In a path-traced scene, indirect light already does this
job physically; if a scene reads too dark, add or enlarge an emitting
surface, or add a sky, rather than a constant term.  The chunk still
parses — existing scenes that use it are not broken by this convention —
but do not author a new one.

**Where this is enforced in code:** the agent surface (`insert_chunk`,
`insert_chunks`, `light_scene`, the scaffold verbs that route through
them, and the `propose_patch` value-splice path) **refuses** to create
an `ambient_light` chunk, unconditionally — in every build phase and
with `--agent-build-protocol=off` — and its refusal states the physics
above and names `shape_light` beside `rect_light` first, then this
four-chunk chain as the general form.  The same surface, the same
routes, and the same unconditional treatment (not part of the
build-phase machinery, no phase-refusal budget consumed, survives
`--agent-build-protocol=off`) apply the 2-per-scene `omni_light` /
`spot_light` / `directional_light` budget above.  Everything else in
this section is convention, not enforcement: the CLI, the GUI and the
scene loader will all happily author and load any light kind or any
number of them.

---

## 4. Color spaces in painters

Most painters take a `colorspace` parameter:

| `colorspace` value | Meaning | When to use |
|---|---|---|
| `sRGB` | sRGB-encoded display value; gamma-decoded on load | Hand-authored colors copied from a colour picker / RGB hex code (e.g. `#FF7F00`) |
| `Rec709RGB_Linear` | Already-linear Rec.709 RGB | Numerical weights / multipliers / measured spectra |
| `ROMMRGB_Linear` | Linear ROMM (ProPhoto primaries); applies a REAL Rec.709 → ROMM conversion on load (the working space has been Rec.709 since the 2026-05 Stage-B migration) | Only when you've explicitly authored ROMM-encoded data.  NEVER for normal maps — the conversion warps the vectors; the verbatim-store idiom is `Rec709RGB_Linear` |
| `ProPhotoRGB` | ROMM with the ProPhoto display-encoding curve | Rare; only when you've explicitly authored ProPhoto-encoded data |

`uniformcolor_painter` defaults to `Rec709RGB_Linear` (its `color` is treated
as already-linear Rec.709 — i.e. a numerical weight, no gamma decode).  Pass an
explicit `colorspace` (e.g. `sRGB`) when the value was picked perceptually and
should be gamma-decoded.  Painters that load image data (`png_painter`,
`jpg_painter`, `exr_painter`, `hdr_painter`) default to sRGB for PNG/JPG and
linear for EXR/HDR.

### Anti-patterns

- Using `colorspace sRGB` for normal maps — gamma-decoding warps the
  encoded vector.  See `gltf_normal_mapped.RISEscene` for the right
  recipe.
- Using `Rec709RGB_Linear` for hand-picked perceptual colors — the
  painted result will look ~2.2× brighter than a colour picker
  suggested.
- Applying gamma twice: pass `1.0 1.0 1.0` to a `uniformcolor_painter`
  with `colorspace sRGB`, then pass the painter through another
  sRGB-decoding stage.  Match the colour space to the data's encoding.

---

## 5. `standard_object` transform precedence (Phase 3+)

```text
standard_object
{
    name        my_mesh
    geometry    geom
    material    mat
    position    Px Py Pz
    orientation Rx Ry Rz       # Euler XYZ degrees (legacy)
    quaternion  Qx Qy Qz Qw    # glTF/OpenGL convention (xyzw); takes precedence over orientation
    matrix      m00 m01 ... m33  # 16 doubles, column-major; takes precedence over both above
    scale       Sx Sy Sz
}
```

**Order of precedence** (highest first):
1. `matrix` — full 4×4 supersedes everything; emits warning if combined with
   `quaternion`, `orientation`, or `position`.
2. `quaternion` — replaces Euler rotation; still composes with `position`
   and `scale`.  Emits warning if combined with `orientation`.
3. `orientation` — Euler XYZ degrees (RX·RY·RZ, lossy at gimbal-lock).

The glTF importer always uses the `matrix` path for losslessness.  Most
hand-authored scenes use the Euler form for simplicity.

`scale` is per-axis (`Vector3`, not scalar).

### `parent` — the transform is LOCAL, relative to the parent

A `standard_object` is the scene-graph node
([87](agentic-redesign/87-recursive-scene-graph.md)).  It carries either a
`geometry` (a leaf shape) or no geometry at all (a pure CONTAINER: a transform
that other objects hang off, invisible to the renderer, never a luminaire, not
in the acceleration structure), plus an optional `parent`:

```text
standard_object          # a container -- note: NO `geometry` line
{
    name        dragon
    position    3 0 -2
    orientation 0 40 0
}

standard_object
{
    name        dragon_head
    parent      dragon
    geometry    head_mesh
    material    scales
    position    0 1.4 2.1   # LOCAL: relative to `dragon`
}
```

- The node's world transform is `parent.world × local`.  **Everything the
  chunk authors — `position`, `orientation`, `quaternion`, `matrix`, `scale` —
  describes the LOCAL transform**, so a child's `position 1 0 0` under a parent
  with `scale 2 2 2` lands two units out, not one.
- Nesting is arbitrary.  Moving a node moves its whole subtree — on load, on
  any scene-document edit, on any interactive transform edit, and **on every
  animated frame**.  A `timeline` on a parent therefore carries its children:
  animate the assembly, not each part.  The children need no timeline of their
  own, and a container (a transform with no `geometry`) is keyframable exactly
  like any other object.
- The one place hierarchy is **not** re-composed is a per-sample motion-blur
  sample.  Those read the frame's base-time bake: sub-frame motion of a leaf
  blurs as before, but sub-frame motion of a **parent** does not reach its
  children, so within one shutter an assembly visibly separates — a body smears
  away from its wheels.  This is related to the stale-TLAS limitation
  ([docs/ARCHITECTURE.md](ARCHITECTURE.md)) but is **not** the same shape, and
  it is worth being precise: the TLAS one is *uniform* (every object's bound is
  equally stale), this one is *differential between objects*.  No energy or PDF
  consequence — each object's area scale and emitter sampling stay
  self-consistent with its own current matrix — but if you are blurring a
  hierarchy, expect it to come apart.  Animate the leaf, or accept the
  separation.
- **`rect_light` and `shape_light` take a `parent` too.**  Each synthesizes an
  ordinary scene-graph object, so a lamp can be carried by an assembly: parent
  it to the fixture and the fixture's transform moves the light with it.  Their
  `center` is then LOCAL to that parent, exactly as a `standard_object`'s
  `position` is.
- A container is **not a CSG operand** either (a boolean needs a shape), and a
  CSG operand cannot take a `parent`: an operand's transform is interpreted in
  its `csg_object`'s frame, not the world's.  Parent the `csg_object` itself —
  it takes a `parent` param of its own and is an ordinary scene-graph node.
- Removing the `parent` line — or writing `parent none` — detaches the object
  back to a root.
- **A parent must be DECLARED BEFORE the object that names it** — the same rule
  `standard_shader`'s `shaderop` references live under.  A forward reference is
  a hard parse error.  It also makes a cycle impossible at parse time; a
  runtime reparent is guarded separately.
- The GUI transform panel shows and edits these LOCAL values.  The gizmo still
  drags in world space.

One asymmetry to know about if you mix `parent` with `instance_array`: the
generator expands AFTER every ordinary object, wherever its chunk sits in the
file, so a generated instance may name a parent declared textually after the
generator — while an ordinary object can never name a generated `name[i,j]`
instance, because no instance exists yet when ordinary objects are applied.
Composition is correct either way (the tree is walked after both passes) and
cycles are still refused.

Worked example: `scenes/Tests/Geometry/object_parenting.RISEscene`.

### Instancing a subtree — `source`

A `standard_object` may carry **`source <object-name>`** instead of `geometry`.
The node is then a COPY of that object: it takes the source's bindings
(geometry / material / modifier / shader / radiance map / interior medium /
shadow flags) while its own `position` / `orientation` / `scale` say where the
copy goes.  **The source's own local transform is DROPPED, not composed** — so
`position 5 0 0` means "the copy sits at x=5", not "5 units from wherever the
original happened to be".  The source keeps rendering; `source` copies, it does
not move or hide anything.

If the source has **children**, its whole subtree is copied.  Each descendant
becomes one further object named `<instance>.<descendant>`, parented to the copy
of its own parent — so moving the instancing node moves the whole assembly:

```text
standard_object { name lamp        position 0 0 0 }          # the assembly root
standard_object { name lamp_base   parent lamp   geometry base_mesh   material brass }
standard_object { name lamp_arm    parent lamp   geometry arm_mesh    material brass  position 0 0.4 0 }
rect_light      { name lamp_glow   parent lamp_arm  center 0 0.9 0  size 0.2 0.2  facing 0 -1 0  color 1 0.9 0.7  exitance 40 }

standard_object { name lamp_b  source lamp  position 3 0 0 }
```

That last chunk produces four more objects: `lamp_b`, `lamp_b.lamp_base`,
`lamp_b.lamp_arm` and `lamp_b.lamp_glow` (whose painter, luminaire material and
panel geometry are renamed with it, as `lamp_b.lamp_glow__pnt` and friends).
Things worth knowing:

- **One level of qualification, always.**  A grandchild is
  `lamp_b.lamp_glow`, never a path `lamp_b.lamp_arm.lamp_glow` — descendant
  names are already unique, so the copy needs no path.  Instancing something
  that itself contains an instance composes the same way: the copied entry's
  name already carried a level, so it reads `outer.inner.part`.
- **The whole subtree must be declared before the instancing chunk.**  A
  `parent` line added BELOW the instance is refused rather than silently left
  out of the copy.
- **A `csg_object` in the subtree shares its operands** with the original.  That
  is correct: an operand's transform is read in its composite's own frame, so
  one operand serves composites at different world poses.
- **An `override_object` layer on a subtree member is refused.**  An override is
  applied to the live object by name after its base chunk, so a copy built from
  that base chunk would carry the un-overridden pose.  Fold it into the base
  chunk.
- **An `instance_array` parented into the subtree is refused** for the mirror
  reason: generators expand last and their `parent` names the original node, so
  their objects would stay behind.
- **Animation does not follow an instance.**  A `timeline` on the source moves
  the source only — an instance is a copy, not a live view.  Give each instance
  its own timeline, or animate a container the instances are parented to.
- **Per-frame cost.**  A `source` naming a LEAF (or a container with no
  children) costs nothing: it collapses to one object with no parent link.  A
  SUBTREE instance costs one parent link per copied descendant, and the
  per-frame hierarchy re-bake is sized by link count — measured at ~1.4 µs per
  link per render pass, i.e. ~5.5 ms/pass for 1000 instances of a 5-node
  subtree.  Authoring the same 5000 objects flat costs ~0.2 ms/pass.  That is a
  property of hierarchy, not of instancing (5000 hand-parented objects measure
  the same), but subtree instancing is the easiest way to author thousands of
  links by accident.

#### Repeating an instance — `count_u` / `count_v`

A chunk carrying `source` may also carry **`count_u U`** and, optionally,
**`count_v V`** (default 1).  The whole instance — root plus subtree — is then
repeated `U x V` times:

```text
standard_object { name post  geometry post_mesh  material wood }

standard_object
{
name fence
source post
count_u 20
position expr(i * 1.5)  0  0
orientation 0 expr(u * 15) 0
}
```

- **Names.**  Each repetition's root is `<name>[i,j]` and each of its subtree
  copies is `<name>[i,j].<member>` — so the fence above is `fence[0,0]` …
  `fence[19,0]`.  `i` runs fastest.
- **`count_u 1` is still `[0,0]`.**  The PRESENCE of a count selects the
  repeated naming, not its value; a count may be an `expr(...)` over a `let`,
  and a name that flipped when a constant changed from 2 to 1 would dangle
  every `parent fence[0,0]` in the file.
- **Per-instance expressions.**  Every OTHER parameter on the instancing chunk
  may use per-component `expr(...)` over four variables: `i` and `j` (the
  indices) and `u` and `v` (the same, normalized into `[0,1]`; `0` when that
  count is 1).  This is the same expression scope `instance_array` has.
- **The counts vary THIS chunk only.**  A member of the copied subtree is not
  per-instance variable — its parameters are the same in every repetition.
- **Refused, each with its own message:** counts without a `source`; `count_v`
  without `count_u`; a fractional, negative or out-of-range count (never
  rounded); instancing a chunk that itself carries counts, or copying a subtree
  that contains one (either would silently copy just one of its N entries).
- **Refer to a repetition BY ITS OWN NAME, never by the chunk's.**  A counted
  chunk called `fence` produces `fence[0,0]` … and NO object called `fence` at
  all, so `parent fence` and `override_object { name fence }` both refuse —
  each naming that as the cause and spelling the working form
  (`parent fence[0,0]`, `name fence[0,0]`).  That form is the intended idiom,
  not a workaround: a repetition's entry parents and overrides like any other
  object.
- **Cost.**  Repeating a LEAF is free — 10 000 repetitions of a leaf source
  measure at the flat baseline, because the collapse case has no parent links
  at all.  Repeating a SUBTREE costs `count x (subtree size - 1)` links, and the
  per-frame re-bake is sized by links (see below).
- **A document-wide cap** bounds `count_u x count_v x subtree size` — ENTRIES,
  not repetitions — at 10 000 000, and each count is separately clamped to 1e6.

##### Three ways a per-instance `expr(...)` fails QUIETLY

These are properties of RISE's expression evaluator, which is shared with the
procedural painters and is deliberately total (it never faults, never produces
`inf`/`nan`).  That is right for a texture evaluated a million times a frame and
surprising when the same evaluator computes how many objects a scene has.

- **Divide or modulo by zero evaluates to `0`, with no diagnostic.**  The
  instance index makes a natural divisor, so this is easy to write by accident:

  ```text
  source S  count_u 2  position expr(1/(i-1)) 0 0
  ```

  derives `x = -1` for `i = 0` and `x = 0` — not a refusal — for `i = 1`
  (verified against the derive, 2026-08-17).  Reshape the expression so the
  divisor cannot reach zero, or offset it (`expr(1/(i+1))`).  It bites hardest in
  a COUNT: `count_u expr(4/0)` is `count_u 0`, and the array simply is not there.
- **`i` / `j` / `u` / `v` inside a COUNT are always zero.**  A count says how
  many repetitions there will be, so it is evaluated BEFORE any repetition
  exists; the per-instance variables are bound to `0` there.  `count_u expr(i)`
  therefore means `count_u 0` and the whole array vanishes silently.  A count may
  use `let` constants and arithmetic freely — just not the indices it produces.
- **An `expr(...)` on a BOOLEAN parameter is always false.**  Boolean scene
  values are read as "true if the first character is `t`" (`TRUE` / `FALSE`), and
  an expr evaluates to a NUMBER — so `casts_shadows expr(i)` is `casts_shadows 0`
  is `FALSE` in every repetition, including the ones where `i` is nonzero.  This
  is not specific to instancing (any `expr(...)` on any bool slot has always
  behaved this way); to vary a boolean per repetition, author the two variants as
  separate chunks.

---

## 6. Coordinate system

RISE uses a right-handed coordinate system.  Common authored conventions:

- `up = (0, 1, 0)` (Y-up) for cameras and lights — matches glTF, OpenGL.
- Asset Z-axis points "out of the screen" toward the viewer.
- A camera at `(0, 0, +5)` looks at `(0, 0, 0)` along the `-Z` axis.

When you import an asset whose source convention is Z-up (3DS Max, Blender
defaults), you typically need a `90°` X rotation on the importing
`standard_object`.  The glTF importer assumes Y-up per the glTF spec
and does not insert this rotation; check `OrientationTest.glb` if in
doubt.

---

## 7. Texture V-axis flip

glTF textures use OpenGL V-up (V=0 at the bottom of the image).  RISE
samplers use DirectX V-down (V=0 at the top).  The
`gltfmesh_geometry` chunk has `flip_v TRUE` by default to compensate
at mesh-load time — this is correct for almost every glTF asset.
Native PLY / 3DS / RAW2 files don't flip.

If a texture maps "upside down" on a glTF asset, check that `flip_v` is
TRUE; if it maps wrong on a non-glTF asset, the source asset itself was
authored against the opposite convention.

---

## 8. Render-rasterizer pairing

A scene needs exactly one rasterizer.  The shader-op pipeline behaves
differently across rasterizers; some features (notably
`alpha_test_shaderop`) only work under integrators that go through
`IShader::Shade()`:

| Rasterizer | Honours shader-ops? | glTF MASK alpha? |
|---|---|---|
| `pixelpel_rasterizer` (PT) | yes | yes |
| `bdpt_pel_rasterizer` (BDPT) | no — bypasses for path construction | no, surface treated as opaque |
| `vcm_*_rasterizer` (VCM) | no | no |
| `mlt_*_rasterizer` (MLT) | no | no |
| Photon tracers | no | no |

If a scene relies on alpha cutout (foliage, decals) or any
shader-op-driven effect, render with PT.

---

## 8.5. The `film` chunk — pixel-grid output settings

Output settings (image width, height, pixel aspect ratio) live on a
scene-level `film` chunk, not on the camera.  The camera handles
imaging optics (FOV, aperture, focal length); the `film` describes
the discretization of the continuous image to pixels.  Background:
[docs/ARCHITECTURE.md](ARCHITECTURE.md) "Camera / Film / Output
Separation".

```
film {
    width 1920
    height 1080
    pixelAR 1.0
}
```

- `width` (UInt, default 960) — image width in pixels.
- `height` (UInt, default 540) — image height in pixels.
- `pixelAR` (Double, default 1.0) — pixel aspect ratio. 1.0 = square
  pixels (the common case). Use ≠ 1.0 for anamorphic / NTSC-style
  non-square pixel grids.

If no `film` chunk is present, the default is **qHD = 960 × 540 with
square pixels** — sized for fast iteration on test scenes. Override
at the command line with `RISE-CLI --width N --height N --pixel-ar X`.

**Camera chunks no longer accept `width`/`height`/`pixelAR`.** Scene
format v6 (Phase B2 of the Camera/Film/Output split, 2026-05) moved
those into the dedicated `film` chunk above.  Cameras read dims from
the active Film at construction.  v5 scenes that authored
`width`/`height` inside camera chunks must be migrated:

```sh
python tools/migrate_scenes_v5_to_v7.py [path-or-dir]
```

The script is idempotent — running it on a v6 scene is a no-op.
Loading a v5 scene through the parser without migration produces a
clear error pointing at this command.

**Multiple `film` chunks: last-declared wins.** Each `film` chunk
calls `Scene::ResizeFilm` which mutates the active Film in place AND
resyncs every previously-added camera's projection matrix.  So you
can author multiple `film` chunks (rare; useful for scene-time
overrides) and the final state is always: Film = the most recent
chunk, every camera's projection matches Film.

**Multi-camera with different per-camera dims is not preserved.**
Authoring two cameras at different resolutions and switching between
them with `SetActiveCamera` won't restore the per-camera dims —
every camera resyncs to whatever Film is. If you genuinely want
multiple cameras at different resolutions, you need separate render
invocations (e.g., `RISE-CLI --width A scene.RISEscene` then
`RISE-CLI --width B scene.RISEscene`).

---

## 9. Sanity-check workflow

When a new scene renders unexpectedly:

1. **Render a Lambertian-white sphere with the same lights and camera.**
   If the sphere is dark, the lighting is wrong — investigate
   `direction` first (§1), then `power` (§3), then color space (§4).
2. **Check `RISE_Log.txt`** for parser warnings — many config errors
   are diagnosed there but never surface in the rendered image (e.g.
   "Material X not found, falling back to default").
3. **Render with `samples 4` first** to iterate quickly; bump to 64+
   only after the scene is qualitatively correct.
4. **Bisect by removing chunks** if a multi-feature scene is broken —
   start from a known-good scene and add one chunk at a time.

---

## 10. HDR output pipeline

Added by Landing 1 of the
[PB pipeline plan](PHYSICALLY_BASED_PIPELINE_PLAN_LANDING_1.md).
The recommended pattern for any new scene is to declare **two**
`file_rasterizeroutput` chunks: an HDR primary and a tone-mapped
display preview.

**Declaration order matters:** a `file_rasterizeroutput` chunk
attaches to the rasterizer that is active when it is parsed, so the
rasterizer chunk must appear **before** it (same convention as
`camera_defaults` before cameras).  A scene that declares
`file_rasterizeroutput` first — or omits the rasterizer chunk
entirely — fails to load with a "no rasterizer is set" diagnostic.

```
# HDR primary — the integrator's verbatim radiometric output.
# No exposure, no display transform.  Use as the source of truth
# for variance / RMSE comparisons and external compositing.
# (`Rec709RGB_Linear` is the identity/verbatim output since the
# Stage-B migration; `ROMMRGB_Linear` here would apply a real
# Rec.709 → ROMM conversion at the write boundary — use it only
# when you specifically want a ROMM-encoded EXR.)
file_rasterizeroutput
{
    pattern             rendered/myscene
    type                EXR
    color_space         Rec709RGB_Linear
    exr_compression     piz
}

# PNG display preview — derived view of the EXR.  Exposure 0
# means the EXR's nominal radiance values pass through unscaled;
# the ACES tone curve provides proper highlight rolloff for an
# LDR display.
file_rasterizeroutput
{
    pattern             rendered/myscene
    type                PNG
    bpp                 8
    color_space         sRGB
    exposure            0.0
    display_transform   aces
}
```

Both outputs share the same render — they're alternative views of
the same `IRasterImage`.  Both files coexist:
`rendered/myscene.exr` and `rendered/myscene.png`.

### Display transform options

| Curve | When to pick |
|---|---|
| `none` | Default for **HDR** types (EXR / HDR / RGBEA) — they archive linear radiance verbatim.  Also use on LDR for byte-exact regression testing or when downstream tooling expects raw linear. |
| `reinhard` | Cheapest; asymptotic to 1; good baseline; can look flat. |
| `aces` | Default for **LDR** types (PNG / TGA / PPM / TIFF).  Krzysztof Narkowicz fit to ACES RRT+ODT for sRGB.  Industry-standard PBR look. |
| `agx` | Modern alternative.  v1 ships a scalar sigmoid placeholder; the proper primaries-aware AgX lands with the spectral pipeline (Landing 3). |
| `hable` | John Hable's Uncharted 2 filmic.  Older; for compatibility with assets tuned against it. |

The default depends on the format: HDR types default to `none` (so they remain
the radiometric ground truth), LDR types default to `aces`.  Setting
`display_transform` on an HDR format triggers a warning and is ignored.

### Exposure semantics

`exposure` is in EV stops.  `+1` doubles brightness, `-1` halves.
Computed as `radiance × 2^EV` before the tone curve.  Lets you
over- / under-expose a render without re-integrating; the EXR
primary captures the un-exposed radiance so any number of
preview PNGs can be derived without re-rendering.

### HDR formats ignore exposure / display_transform

`type = EXR / HDR / RGBEA` are radiometric ground truth — applying
a tone curve to them would corrupt the recorded radiance.  Setting
`exposure` or `display_transform` non-default on an HDR output
fires a warning and is ignored.

### Migration from pre-Landing-1 scenes

Existing `file_rasterizeroutput { type PNG; ... }` chunks still
parse, but the **default `display_transform` for LDR types is now
`aces`**, not the previous implicit `none`.  PNG (and other LDR)
renders will look subtly different — highlights roll off rather
than clip.  HDR types (EXR / HDR / RGBEA) default to `none` and
their behaviour is byte-identical to before.

Scenes that require the legacy clip-at-1.0 behaviour for an LDR
output (e.g., byte-exact regression testing) must opt in
explicitly:

```
file_rasterizeroutput
{
    pattern             ...
    type                PNG
    color_space         sRGB
    display_transform   none      # opt back in to legacy clipping
}
```

### External tools and chromaticities

Our EXR output writes the `chromaticities` attribute matching the
selected `color_space` (Rec.709 primaries for `Rec709RGB_Linear`,
ROMM RGB primaries when `color_space = ROMMRGB_Linear`).  Colour-managed viewers that
honour the tag (tev, mrViewer, Nuke + OCIO) display correct hues.
Viewers that ignore it (Photoshop's EXR support) show slightly
desaturated values — the data is recoverable via an explicit
primaries conversion in oiiotool / Resolve / etc.

### Build dependency

OpenEXR is now a build requirement.  Windows pulls it via vcpkg
(automatic when building the VS2022 solution); macOS auto-detects
Homebrew's `openexr` package; Linux requires
`libopenexr-dev` (Debian/Ubuntu) or `OpenEXR-devel` (Fedora/RHEL).
Override `OPENEXR_PREFIX` / `IMATH_PREFIX` in
`build/make/rise/Config.Linux` for non-standard install paths.

---

## See also

- [scenes/README.md](../scenes/README.md): where to put scenes
  (`Tests/` vs `FeatureBased/`).
- [src/Library/Parsers/README.md](../src/Library/Parsers/README.md):
  how the chunk parser works, how to add a new chunk.
- [docs/skills/effective-rise-scene-authoring.md](skills/effective-rise-scene-authoring.md):
  procedure for authoring scenes that render right the first time.
- [docs/GLTF_IMPORT.md](GLTF_IMPORT.md): glTF-specific conventions and
  the convention-conversion sites in the importer.
- [docs/PHYSICALLY_BASED_PIPELINE_PLAN_LANDING_1.md](PHYSICALLY_BASED_PIPELINE_PLAN_LANDING_1.md):
  Landing 1 detailed design for the HDR output pipeline.
