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

## 0. Chunk braces must be on their own line — this is a hard parse error

`{` and `}` each need their own line (see
[src/Library/Parsers/README.md](../src/Library/Parsers/README.md)'s chunk
syntax rules); `standard_object { name x geometry g material m }` written on
one line is not a second, more compact syntax.  Since task_7f42984d this is
enforced, not just documented: a shared-line brace is a hard PASS-1 derive
error ("chunk braces must be on their own lines", naming the source line),
refusing the whole scene the same way an unknown chunk type or a value-less
parameter does — never a silent partial load.  Before that fix it was the
opposite of loud: the parser's per-param value-collection loop has no
newline to stop at when a whole chunk shares one line, so it swallowed every
token after the first param's name as more values of that param — a
`standard_object` written this way silently kept its name but lost its
`geometry`/`material`, and the object it produced (with neither) rendered
as nothing, with zero diagnostics.  If a scene you are authoring by hand
(or generating with a script) ever collapses a chunk onto one line to save
space, expect a load failure naming the line, not a quietly wrong render.

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

The `color` these four multiply is **linear Rec.709 by default** — the
same reading a `uniformcolor_painter` gets — and each takes an optional
`colorspace` parameter.  See §4 ("Lights").

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
  `FALSE` and derives the winding from `facing`.  Both faces emit for
  every integrator and every strategy (section 4, "Lights: a double-sided
  emitter emits from both faces").
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

## 4. Color spaces in painters and lights

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

### Lights

**Since 2026-09-02 the four zero-area lights — `omni_light`, `spot_light`,
`directional_light`, `ambient_light` — read `color` as linear Rec.709 by
default, and take the same `colorspace` parameter as the painters** (same
four values, same meanings; the conversion goes through the same code).
So `color 1.0 0.2 0.2` on a light and on a `uniformcolor_painter` now mean
the same thing.  Use `colorspace sRGB` for a value picked out of a colour
picker or copied from a hex code:

```
omni_light
{
	name        key
	power       80.0
	position    0 5 0
	color       1.0 0.2 0.2
	colorspace  sRGB
}
```

**Before that date lights silently gamma-decoded `color` as sRGB** — with
nothing in the scene language, the descriptors or this document saying so.
An authored `color 1.0 0.2 0.2` lit the scene with (1.0, 0.033, 0.033), a
6× error in two channels, while the identical triple on a painter meant
what it said.  Two consequences worth knowing about:

- The live editor / animation path (`KeyframeFromParameters("color")`)
  never decoded, so editing a light's colour in the GUI and reloading the
  saved file gave two different renders.  They agree now.
- The glTF importer pre-applied the sRGB OETF to cancel the decode
  (`KHR_lights_punctual` colour is spec-linear), as did the Blender
  bridge.  Both workarounds are retired.

`tools/migrate_scenes_light_colorspace.py` adds `colorspace sRGB` to every
pre-existing light chunk whose `color` is chromatic, so an old scene's look
is preserved exactly (verified: `CstDeriveGoldenTest` reports 0 drift
across the migrated corpus, and it digests each light's colour at 9
significant figures).  It skips chunks whose components are all 0 or 1 —
the fixed points of the transfer function, where the line would be noise.
The migrator is idempotent and takes `--dry-run` (and `--selftest`, which
exercises its brace scanner against comment-decorated braces).

**`colorspace` describes the AUTHORED `color` line, and nothing else.**
Two places bypass it, both deliberately:

- **Animation keyframes are always linear.**  A `color` keyframe reaches the
  light through `ILight::KeyframeFromParameters`, which builds a `RISEPel`
  straight from the digits — there is no colour-space hook on that path.  So
  on a light that carries `colorspace sRGB`, the chunk's own `color` line is
  decoded but every keyed value on its colour timeline is read as linear.
  Author such a timeline in linear, or (simpler) convert the light to
  `colorspace Rec709RGB_Linear` and write the decoded triple in the `color`
  line, so the whole light speaks one convention.
- **Editing a light's colour in the GUI converts the chunk to linear.**  The
  properties panel shows the light's live, already-converted linear colour,
  so the editor writes `colorspace Rec709RGB_Linear` alongside the new
  `color` (otherwise the re-derive would decode the panel's linear digits a
  second time).  The light's look is unchanged by the conversion itself; the
  chunk simply stops carrying the legacy reading.  The pair is ONE ATOMIC
  edit — if either half is refused (a malformed colour, or a chunk that
  spells `color` or `colorspace` twice, which the editor refuses to write
  into) nothing changes at all — and it is FULLY UNDOABLE: undo restores both
  original lines verbatim, `colorspace sRGB` spelling included, so the chunk
  comes back byte-identical.  That matters beyond tidiness, because an agent
  edit's undo replays the chunk's raw text and must find the convention its
  digits were captured under.

### Lights: a double-sided emitter emits from both faces

**Since 2026-09-28 (DL-320) a luminaire on double-sided geometry emits
from BOTH faces for every strategy** -- path-traced hits, next-event
estimation, BDPT/VCM/MLT light subpaths and connections, the legacy
direct-lighting chain and the photon maps alike.  Double-sided geometry
means: `clippedplane_geometry` (its `doublesided` defaults to `TRUE`), a
mesh with `double_sided TRUE`, a `displaced_geometry` over such a mesh,
and a Bezier patch.  Spheres, boxes, disks, bilinear patches and the
other analytic primitives are one-sided: their back face does not emit.
Before that date only strategies that HIT the emitter saw the back face;
next-event estimation and light-subpath emission treated it as one-sided,
so a back-facing panel lit the scene 7-14x too dark under PT and BDPT
and not at all under the legacy direct-lighting chain
([DL320_DOUBLE_SIDED_EMITTER.md](DL320_DOUBLE_SIDED_EMITTER.md)).

- **A `clippedplane_geometry` luminaire is double-sided by default and
  emits from both faces -- size `scale` for both, or set `doublesided
  FALSE` if only one face should emit.**
- **Each face emits the full exitance**, so a double-sided panel's total
  power is `2 * exitance * area`.  A ceiling panel wound face-down with
  open space above it sends half its power up.  For a one-sided panel say
  `doublesided FALSE` on the `clippedplane_geometry`, or use `rect_light`,
  which is one-sided by construction.
- **Winding does not change a double-sided panel's lighting**; it decides
  a one-sided panel's lit face, `normalize(Cross(ptb - pta, ptd - pta))`.
- A double-sided panel mounted flush against a ceiling or wall wastes its
  back face's light harmlessly, but under BDPT/VCM that is half the light
  subpaths: expect more light-tracing noise, and prefer `doublesided
  FALSE` for such fixtures.

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

`scale` is per-axis (`Vector3`: `Sx Sy Sz`).  A single number IS accepted as an
explicit shorthand -- `scale 0.35` broadcasts to `(0.35, 0.35, 0.35)`, with a
log warning suggesting the fully-spelled form -- but nothing else short of all
three: two numbers, four numbers, or a non-numeric token is a hard parse error
(DL-32, [docs/DEBT_LEDGER.md](DEBT_LEDGER.md)).  Before that fix a partial
value silently zero-filled the missing axes (`scale 0.35` derived
`(0.35, 0, 0)`), vanishing the object from the render with no diagnostic and
making it refuse every `proximity()`/`interior()` query
(`Object::DistanceToSurface`'s `sigma_min <= 0` gate) -- a trap worth knowing
even though the parser no longer springs it.

**General rule (every vector/matrix parameter, not just `scale`).** Every
`Vector2`-, `Vector3`-, quaternion (`Vector4`)-, and 4x4-matrix-valued
parameter in the scene language -- `position`, `orientation`,
`radiance_orient`, a camera's `location`/`lookat`/`up`, absorption/
scattering/emission on a medium, mesh corner points, `bbox_min`/`bbox_max`,
a painter's `scale`/`shift`, a UV pair like `perlin2d_painter`'s
`scale`/`shift` or `polynomial_function2d_painter`'s `center`/`scale`, a
camera's `target_orientation` (theta, phi), `orthographic_camera`'s
`viewport_scale`, and every other field declared `DoubleVec2`/
`DoubleVec3`/`DoubleVec4`/`DoubleMat4` -- requires EXACTLY 2 / 3 / 4 / 16
space-separated finite numbers.  Anything else (fewer, more, or a
non-numeric token) is a hard parse error naming the chunk, the parameter,
and the expected-vs-actual count, fixed at the shared accessor
(`ParseStateBag::GetVec2`/`GetVec3`/`GetVec4`/`GetMat4`, `src/Library/
Parsers/ChunkDescriptor.h`) -- but ONLY for a Finalize() that actually
calls the accessor.  DL-32 round 2 (docs/DEBT_LEDGER.md) fixed every site
that does; a round-3 review found 15 sites that instead read a
fixed-2-token value via a RAW `sscanf` on the bag's raw string, bypassing
the accessor (and the dispatcher's own finite-number gate) entirely, and
were consequently just as unprotected as pre-round-1 `scale` -- these were
mis-declared `DoubleVec3` (reading only 2 of the "3" components) or, for
`viewport_scale`, plain `Double` while being read as 2 components; round 3
added the genuine `DoubleVec2` kind and re-routed all 15 through
`GetVec2`.  The general rule holds today for every DECLARED vector/matrix
parameter -- but "declared `DoubleVec2`/`DoubleVec3`/`DoubleVec4`/
`DoubleMat4`" is the operative test, not "looks like a vector on the
page"; a Finalize() that hand-rolls its own numeric parsing instead of
calling the shared accessor is NOT covered by this rule and needs its own
audit (exactly the round-3 finding).  `standard_object`/`override_object`'s
`scale` and `orthographic_camera`'s `viewport_scale` are the two sanctioned
exceptions -- single-number UNIFORM broadcasts -- each resolved by a
dedicated helper (`ResolveScaleVec3` / `ResolveVec2UniformBroadcast`,
`ChunkParserRegistry.cpp`) that pre-validates the 1-or-N arity itself and
only reaches the shared accessor once the full token count is already
confirmed, so the generic hard-error never fires for that authored
shorthand.  A future field that wants the same broadcast shorthand should
follow that pattern (a dedicated resolver, not a change to the shared
accessor) and should mark its `ParameterDescriptor` entry with
`allowsUniformScalarBroadcast = true` so the editor's suggestion engine can
advertise the shorthand.

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
- Per-sample motion blur re-composes the hierarchy too (DL-457): every
  motion-blur sample carries a child along with its animated parent, so a
  blurred assembly stays together.  Only the subtrees the frame's shutter
  sweep saw move are re-composed per sample (DL-463).
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

An object may also name a SYNTHESIZED entry as its `parent` — `name[i,j]` from
a counted `source`, or `I.X` from a copied subtree — as long as the instancing
chunk that mints it is declared earlier, the same rule every other reference
lives under.  Composition is correct either way (the tree is walked after the
whole document is applied) and cycles are still refused.

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
  count is 1).
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

## 5.5. `csg_object` operand transforms are CSG-LOCAL — a positioned `csg_object` RE-BASES positioned operands

Reported as an all-black render with a `csg_object` that carries its own
`position` on top of operand `standard_object`s that are themselves
positioned.  This is **not** a transform-composition bug:
`CSGObject::IntersectRay` composes the operand's transform into the
csg_object's exactly once (world → CSG-local via the csg's inverse
matrix, then the operand applies its own matrix) — union, intersection
and subtraction all render correctly with positioned operands under a
positioned `csg_object`.  The trap is semantic, not arithmetic:

**An operand's `position`/`orientation` is interpreted in its
`csg_object`'s LOCAL frame, not the world's** (§5's `parent` sub-bullet
already says this for the "operand can't take a `parent`" case — this
is the same rule, for the `csg_object`'s OWN transform).  So when the
`csg_object` ALSO carries a `position`/`orientation`, that transform
**re-bases** every already-positioned operand: the composite lands at
`(csg transform) ∘ (operand transform)`, **not** at the operand's
authored world coordinates.

```text
standard_object { name sphereA  geometry sphA  position -0.25 0 0 }
standard_object { name sphereB  geometry sphB  position  0.25 0 0 }

csg_object
{
    name       csg1
    obja       sphereA
    objb       sphereB
    operation  union
    position   5 0 0          # re-bases BOTH operands, not "moves the union 5 units further out"
}
```

`sphereA`'s composed-once world position is `(5 + -0.25, 0, 0) =
(4.75, 0, 0)`, not `(-0.25, 0, 0)` and not `(4.75 + -0.25, 0, 0)`.  If
`csg1`'s own `position` is large enough — or its own `orientation`
rotates the operands out of the camera's direction — the whole
composite can leave the frustum entirely, rendering **all-black with no
visual clue why**: the image looks the same as "nothing here" and
"wrong place."

This is a **valid, supported construction** — rebasing an
already-authored sub-assembly as a unit is exactly what you want when,
say, a hand-built CSG shape needs to be moved as one piece.  It is the
in-corpus **lens idiom**: a lens built from two half-spheres offset
*locally* from each other, with the whole assembly then positioned as a
unit — e.g. `scenes/FeatureBased/Combined/crystal_lens.RISEscene`'s
`lens_half_a` at local `position 0 0.95 0` under a `lens` csg_object at
`position 0 2.5 0`.  Nine in-corpus scenes use exactly this pattern (see
`allow_transformed_operands` below).  It is only a trap when the
operands were positioned assuming they'd keep their authored world
coordinates once wrapped in a `csg_object`, with nothing in the document
signalling that the rebase was intentional.

**Job::AddCSGObject warns at parse time** when both halves are
transformed (the csg_object's own `position`/`orientation` is
non-identity, AND at least one operand already carries a non-identity
transform), naming the csg_object and which operand(s).  The wording
branches on how many operands are transformed — shown here for the
common case where both are (see `Job::AddCSGObject` in `src/Library/Job.cpp`
for the single-operand grammar variant, "operand `X` ... RE-BASES it"):

```text
Job::AddCSGObject:: `csg1` carries its own position/orientation on top of
operands `sphereA` and `sphereB`, which are already transformed -- an
operand's transform is interpreted in `csg1`'s LOCAL frame, not the
world's, so `csg1`'s own transform RE-BASES them: the composite lands at
(`csg1`'s transform composed with each operand's transform), NOT at the
operands' authored coordinates.  This is a valid construction (a
sub-assembly rebased as a unit); if it is what you intend, set
`allow_transformed_operands TRUE` to acknowledge it.  But if the
composite renders shifted, empty, or entirely out of view (an all-black
frame), this composition is the likely reason -- either position the
composite solely through `csg1` (author its operands untransformed), or
drop `csg1`'s own position/orientation.
```

It is a WARNING, not a refusal — the construction is valid, so nothing
fails to load.  A `csg_object` positioned over UNtransformed operands
(the common case — see `scenes/Tests/Geometry/csg.RISEscene`) does not
trigger it at all.

**`allow_transformed_operands` (Bool, default `FALSE`) acknowledges the
rebase and silences the warning.**  It mirrors the `allow_non_sampling_emitter`
idiom on this same chunk (§ elsewhere in this doc / `ChunkParserRegistry.cpp`),
except it is not inert: it is threaded straight through to
`Job::AddCSGObject`, which reads it and skips the advisory entirely.  Set
it when the rebase is deliberate — the lens idiom above is the
canonical case:

```text
csg_object
{
    name       lens
    obja       lens_half_a
    objb       lens_half_b
    operation  intersection
    position   0 2.5 0
    allow_transformed_operands TRUE
}
```

**Scope note:** a csg_object placed via `parent` re-bases its operands
the same way, but does NOT need (or trigger) this advisory — parenting
the csg_object is this codebase's explicitly recommended grouping idiom
(its own operand-parent refusal says "Parent the csg_object instead"),
which already expresses the same intent as the acknowledgment flag. The
advisory targets only the dual-inline-authoring case: operand `position`
and csg_object `position` written in the same document.

**Fix (when the rebase was NOT intended):** either position the
composite solely through the `csg_object` (author its operands with no
`position`/`orientation` of their own — the common pattern), or drop the
`csg_object`'s own transform and position the operands directly.  Don't
do both unless the rebase is what you actually want — and if it is,
prefer `allow_transformed_operands TRUE` over silently living with the
warning.

Regression coverage:
`scenes/Tests/Geometry/csg_positioned_operands.RISEscene` (the
supported, in-view case — intentionally trips the warning, and
deliberately does NOT carry `allow_transformed_operands`, so it stays a
live tripwire) and `tests/CsgOperandTransformTest.cpp` (compose-once
arithmetic lock + parse-time diagnostic fire/no-fire/acknowledged-silence).
Agent-facing validation surfaces this structurally too, as a
Warning-severity `CSG_OPERAND_REBASE` diagnostic from
`AgentSession::ValidateText` (Warning tier only — no paired creation
gate, unlike `LUMINAIRE_NULL_GEOMETRY`; see
`AgentDiagnosticCode::CSG_OPERAND_REBASE` in
`src/Library/Agent/AgentDiagnostic.h` for why); covered in both
`tests/CsgOperandTransformTest.cpp` and `tests/AgentChunkCrudTest.cpp`.

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

A scene needs exactly one rasterizer. Imported glTF MASK and BLEND use
scalar material coverage at intersections across all transport paths. Explicit
legacy shader operations still require a rasterizer that invokes `IShader::Shade()`.

| Rasterizer | Honours explicit shader-ops? | glTF MASK / BLEND coverage? |
|---|---|---|
| `pixelpel_rasterizer` (legacy) | yes | yes |
| `pathtracing_*_rasterizer` | no | yes |
| `bdpt_*_rasterizer` | no | yes |
| `vcm_*_rasterizer` | no | yes |
| `mlt_*_rasterizer` | no | yes |
| Photon tracers | no | yes |

MASK deterministically accepts alpha at or above the cutoff. BLEND accepts a
surface with probability alpha; rejection continues the original ray without a
bounce or medium transition. See [DL-214](ALPHA_COVERAGE.md) for emitter,
medium, SMS, and subsurface semantics. Explicit shader-op effects still require
`pixelpel_rasterizer`; imported coverage does not install a second alpha shader.

### 8.1 Under `pixelpel_rasterizer` the shader chain must list an op per transport mode

`standard_shader { shaderop DefaultDirectLighting }` is **direct lighting
only**.  `Job::AddStandardShader` prepends `DefaultEmission` and nothing
else; `EmissionShaderOp` and `DirectLightingShaderOp` both report
`RequireSPF() == false`, so `StandardShader::Shade` never calls the
material's SPF, no continuation ray is ever scattered, and
`max_recursion` is inert.  Anything a material carries *besides* its
BSDF is silently dropped: a `dielectric_material` pane renders **black**
(the emitter behind it is invisible), a `weave_material { transmission
thin }` curtain loses the light seen straight through its gaps (a
gap-0.1 linen reads 0.41× the modern path tracer in front of an area
emitter), a mirror reflects nothing.  Nothing warns.  Add the op for
each transport mode the scene uses — `DefaultRefraction` for
transmission (it follows every `eRayRefraction` ray, weave gaps
included, not only dielectrics), `DefaultReflection` for reflection —
or render with `pathtracing_pel_rasterizer`, whose walk follows every
lobe the material samples.  `scenes/FeatureBased/Caustics/pool_caustics.RISEscene`
shows the full chain.  This is also why `tests/BDPTStrategyBalanceTest.cpp`
switched its PT reference to `pathtracing_pel_rasterizer`
([CLOTH_FABRIC_DESIGN.md](CLOTH_FABRIC_DESIGN.md) §15 debt 26).

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

## 8.6. Motion blur: camera `exposure` is a CENTRED shutter

A camera's `exposure` turns on motion blur in ANIMATION renders
(`RasterizeAnimation`; a still render never samples time).  Each pixel sample
draws its own time in `[t - exposure/2, t + exposure/2]` around the frame time
`t` -- the shutter is centred, not trailing.  So a frame at `t = 0` with
`exposure 1` and keyframes at 0 and 1 spends half its exposure clamped to the
`t = 0` key; to blur over a keyframed span `[0, 1]`, render the frame at
`t = 0.5`.  A reference "average of static renders" must use the same centred
interval (DL-457 was filed against one that did not).  `scanning_rate` /
`pixel_rate` shift each scanline's / pixel's window further.

Consequences worth knowing:

- An animation frame with `exposure > 0` renders SINGLE-THREADED for PT,
  BDPT, MLT and the legacy rasterizers (per-sample scene evaluation mutates
  the scene, so the frame runs on the calling thread).
- VCM with vertex merging instead traces each progressive pass at ONE
  shutter time (the scene moves once per pass; the N passes take the N
  equal strata of the shutter in a random order, so any prefix of the passes
  -- an adaptively converged pixel, an early preview -- covers the whole
  shutter), so its light store and the eye samples merged
  against it share a time; those frames render multi-threaded (DL-463).  At
  low pass counts the blur is a set of N superimposed poses rather than
  per-pixel noise.  A camera with `scanning_rate` / `pixel_rate` keeps the
  per-sample path (and a frame-time light store).
- The top-level acceleration bounds each object over the whole shutter --
  exactly for motion linear between keyframes, approximately for rotations
  (see `IObjectManager.h`, "Motion blur").
- The light-selection tables (alias weights, light BVH boxes and cones) are
  rebuilt every animation frame and, under exposure, cover the whole shutter:
  a light dark, turned away or elsewhere at the frame time stays selectable
  (DL-463).



A weave, a tile course, a brick bond — the natural way to author any of them is
the cell formula: `floor(u*N)` for the cell index, `mod(i + k*j, 5)` for the
phase. §5.3 of [CLOTH_FABRIC_DESIGN.md](CLOTH_FABRIC_DESIGN.md) gives exactly
those formulas, and they are what a weave *is*.

**Feeding one of them into an anisotropic material's direction slot produces a
checkerboard, not a fabric.** This was measured while authoring
`scenes/FeatureBased/Materials/fabric_swatches.RISEscene`, and it is worth
stating as a rule because the failure looks like a bug in the renderer and is
not one:

- A `ggx_material` with `alphax 0.34 / alphay 0.06` — the `satin` preset's
  substrate — has a highlight that is nearly a line. Rotating that lobe by even
  a tenth of a radian moves the highlight completely off a given shading point.
- A cell field is **piecewise constant**. So adjacent cells get lobes at
  different angles, each cell is either lit or dark with nothing in between, and
  the cell grid becomes the dominant visible structure in the frame.
- **Shrinking the per-cell excursion does not fix it.** It lowers the contrast of
  the checks and leaves the checks. The first pass used 0.42 rad and produced a
  literal checkerboard; retuning to 0.11 rad produced a fainter checkerboard of
  the same size. Only removing the discontinuity removed it.

The tighter the anisotropy, the worse it is — which is exactly backwards from
what an author wants, because the tight-anisotropy materials are the ones a
weave direction is *for*.

### Use a continuous field instead

A constant base angle — the direction the material is actually about — plus a
low-amplitude continuous drift:

```
scalar_painter
{
	name		weave_satin_float
	param		base        0.0     # the float direction; a CONSTANT
	param		drift_amp   0.030   # radians, and small: <= ~0.05
	param		drift_scale 8.0     # world-space feature size = 1/scale
	param		grain_world 0.020   # the drift's finest octave, in fw's units
	def			jitter vec3(8.9, 3.3, 1.1)
	def			drift  drift_amp*fbm( P*drift_scale + jitter, 4, 0.5, 2.0 )
	def			fade   smoothstep( grain_world*0.5, grain_world*2.0, fw )
	expression	mix( base + drift, base, fade )
}
```

`fbm` is **signed with mean ~0**, so `drift_amp*fbm(...)` is already a zero-mean
wobble about `base` — no remapping needed. Sampling at `P` (world space) rather
than `(u,v)` means several objects sharing one painter each get their own
realisation instead of looking copy-pasted, and it makes the field continuous
across a seam between two abutting instances.

Give the tightest lobe the *smallest* drift, not the largest: a narrow highlight
converts a given angular wobble into the biggest visible change.

### The `fw` fade, and what it is actually for

`fw` is the shading footprint, in the same world units the field is written in,
and it is a real always-available context variable. The `fade` above relaxes the
drift to the constant base angle as the footprint outgrows the drift's finest
octave — the correct minification limit, since a woven surface seen from far
enough away *is* isotropic.

Three things about it that are easy to get wrong:

- **`grain_world` is the finest feature's size in WORLD units** — for the fbm
  above, roughly `1 / (drift_scale · lacunarity^(octaves-1))`. It is the one
  number to re-derive when the subject is rescaled.
- **`fw` is `0.0` where no footprint is available** — secondary bounces, and the
  rim band of a `fisheye_camera` render (the pixels whose +x/+y neighbour falls
  outside the projection's 180° disc, where no honest differential exists) —
  and `smoothstep(a, b, 0)` is `0`, so the fade correctly *disengages* there
  rather than snapping to the mean.  (This list used to be much longer:
  "non-mesh geometry" left it when every geometry started publishing a
  primary-hit footprint on 2026-09-06, `thinlens_camera` /
  `orthographic_camera` on 2026-09-10, and the fisheye's INTERIOR the same day
  — every camera RISE ships now emits differentials —
  [TEXTURE_FOOTPRINT_ANALYTIC_DESIGN.md](TEXTURE_FOOTPRINT_ANALYTIC_DESIGN.md)
  §12.)
- **`fbm` already fades its own high octaves against `fw` internally.** The
  explicit fade above is doing a different job — retiring the whole *anisotropy*,
  not just the noise band — so the two are complementary, not redundant.

For a field that genuinely must be discontinuous (a tile *colour*, a brick
*albedo* — anything that is not steering a narrow lobe), the same `fw` fade is
still the anti-aliasing tool, fading the cell value to the cell population's
mean. The rule above is specifically about *direction* slots feeding anisotropic
BSDFs.

`stochastic_tile_painter` is the complementary tool for a different problem —
visible repetition of a tiling source — so do not reach for it expecting
anti-aliasing.

Background: [CLOTH_FABRIC_DESIGN.md](CLOTH_FABRIC_DESIGN.md) §5.5 for the
footprint argument, §9.5 for why Phase-1 fabric has no pattern-scale structure
to draw in the first place, and §9.9 gate 9b for the measurement.

---

## 8.8. Dielectric material transmittance (`tau`) is unit transmittance, not absorption coefficient

`dielectric_material`'s `tau` parameter specifies the internal transmittance of the medium over one unit of ray travel distance. In the shader evaluation ([DielectricSPF.cpp](../src/Library/Materials/DielectricSPF.cpp)), it is applied via Beer's Law as:
$$\text{attenuation} = \tau^{\text{distance}} = \text{pow}(\tau, \text{distance})$$

Consequently:
1. **$\tau$ must lie in $[0, 1]$** across all channels and visible wavelengths ($380\text{--}780\text{ nm}$). Values $> 1.0$ violate energy conservation and produce exponential gain with travel distance; `Job::AddDielectricMaterial` emits a diagnostic warning whenever any channel or visible sample exceeds $1.0$.
2. **$\tau$ is NOT an absorption coefficient ($\sigma_a$).** If you are working from a published physical absorption table (e.g. Pope & Fry 1997 for pure water), values are typically given as an absorption coefficient $\sigma_a(\lambda)$ in units of $\text{m}^{-1}$ (often exceeding $1.0$ at long visible wavelengths). Pasting these raw values directly into a `.spectra` file bound to `tau` will produce wrong attenuation and render artifacts.
3. **Conversion formula:** convert raw absorption coefficients $\sigma_a(\lambda)$ to unit transmittance $\tau(\lambda)$ via:
   $$\tau(\lambda) = \exp\left(-\sigma_a(\lambda) \cdot \text{unitLength}\right)$$
   where $\text{unitLength}$ is the physical length of 1 world unit in metres (e.g. $1.0$ if 1 world unit = 1 metre, or $0.01$ if 1 world unit = 1 centimetre).

---

## 8.9. An open glass sheet: its FRONT is the outside, its BACK is the glass

A `dielectric_material` / `perfectrefractor_material` on an OPEN sheet -- a
`clippedplane_geometry`, or a `displaced_geometry` over a flat convex one --
is an interface, not a solid: the side its normal points to (the winding's
front) is the surrounding medium and the side behind it is glass.  Every ray
refracts by the face it strikes (front: enters, back: exits), in every
integrator and in SMS (DL-345, 2026-10-02,
[DL345_OPEN_SHEET_FACE_RULE.md](DL345_OPEN_SHEET_FACE_RULE.md)).  So:

- a slab built from two sheets needs both normals pointing OUT of the slab
  (top facing up, bottom facing down); a sheet wound the other way makes
  the slab's inside "air" and its outside "glass";
- such a slab still differs from a `box_geometry` slab at its open side
  edges (light leaves through them instead of meeting a side face) -- a few
  percent near the edges on `sms_k2_flatslab`; author a closed solid when
  the edges matter;
- a lone sheet is a half-space of glass behind it: a receiver seen directly
  under it is lit as if it sat in that glass.
  This holds for the SHEET'S OWN crossing only: another refractor behind a
  lone sheet sees the walk's stack, which is air when the walk was seeded
  behind the sheet (an ior-1.5 sphere under a water sheet reads ~10 % low
  in PT/BDPT/VCM against the closed-water twin; DL-382 (4)).
- only provably open surfaces get this rule.  Since DL-382 (2026-10-08) that
  includes a triangle mesh (`indexedmesh_geometry`, `rawmesh_geometry`, an
  imported mesh) that is ONE planar sheet with consistent winding and
  authored normals agreeing with it -- a window pane imported as a quad.
  Anything else -- a non-planar open mesh, a slab authored as ONE mesh
  holding both sheets, a Bezier patch set, a CSG of sheets -- is not
  certified open and keeps the stack-based ("entering unless already
  inside") convention, which is right for a closed solid but not reciprocal
  on open sheets.  A slab built as ONE two-sheet mesh still renders right
  (both crossings key on one object); a slab built from two separate
  NON-planar open meshes does not. `ObjectManager` warns once for each
  unchanged set of uncertain transmissive objects, naming the first one.
  Besides indexed/displaced meshes, this includes Bezier and bilinear
  patch sets, uncertified clipped planes, and CSG containing uncertain
  or open transmitting operands. Raw meshes without a closed-volume
  certificate produce an informational diagnostic because they may be
  closed solids. These warnings confer no certificate: a boundary loop
  does not establish non-self-intersection or prove empty interior.
  Use planar consistently wound sheets or certified closed solids when
  reciprocal transmission is required.

## 8.10. Depth caps do not mean the same paths in every integrator (DL-351)

Count a path's **scattering surfaces** K: every surface hit between the
camera and the light, including delta pass-throughs and the crossings of
an index-matched medium shell (medium scatter vertices are not surfaces;
they are bounded by `max_volume_bounce`, which is per PATH in every
integrator since DL-247).

| Setting | PT | BDPT / MLT | VCM |
|---|---|---|---|
| surface depth | no scene knob: a fixed 128-vertex loop | `max_eye_depth` / `max_light_depth` bound SURFACE hits per SUBPATH; since DL-351 the MIS weights know it, so the render estimates exactly the paths that have at least one CONNECTIBLE split (eye part <= `max_eye_depth`, light part <= `max_light_depth`, joined at non-delta, connectible vertices); in an all-diffuse scene that is **K <= max_eye_depth + max_light_depth** and only the sum matters | same path set; since DL-467 its recurrence MIS drops every connection / merge strategy past the caps too (closed box at (1,1): 0.0643 -> 0.0845, BDPT 0.0839), except one residual: a MERGE's weight still reserves the connections whose eye part would cover the light subpath behind the merge point past `max_eye_depth` (DL-470) |
| `max_diffuse_bounce` & co. | per PATH: at most N continuations of that type; NEE at the last vertex is free, so N = 0 is direct lighting; since DL-467 a non-delta continuation past the cap is traced for its MIS-weighted EMISSION only (as PBRT-v4 / Cycles do), so the last vertex's direct light keeps its full MIS partition -- in an all-diffuse scene `max_diffuse_bounce N` = BDPT's (N+1, 0) | per SUBPATH: each walk may take N continuations of that type, so a joined path can carry up to about 2N + 2 such vertices; the MIS weights ignore these caps -- a connection endpoint has no sampled lobe type, so whether the alternative walk's scatter there would exceed the cap is undefined without a per-lobe-type split of the BSDF (DL-471) | as BDPT |

Consequences for authors:

- For a BDPT/MLT render that should match an unlimited PT render, make
  `max_eye_depth + max_light_depth` exceed the path depth that carries
  energy (the defaults 8 + 8, MLT 10 + 10, are 16 / 20 surfaces).  In an
  ALL-DIFFUSE scene only the sum matters: (2, 0), (1, 1) and (0, 2)
  render the same image.  Not with delta or non-connectible vertices: a
  split cannot join at a mirror, so camera -> mirror -> floor -> light is
  reached at (2, 0) but missed by (1, 1) and (0, 2).
- A medium shell's two crossings count: an env-lit fog box needs
  `max_eye_depth + max_light_depth >= 2` before anything inside is lit.
- Do not use per-type caps to compare integrators -- the same
  `max_diffuse_bounce` admits more paths under BDPT/VCM than under PT
  (open corner at `max_diffuse_bounce 0`: BDPT +2.97 % over PT).
- A capped scene can change brightness when `auto_rasterizer` changes
  its route.

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

## 11. Backlit hair/fur rims read achromatic, regardless of the fibre's colour

A `hair_material` fibre lit from behind (rim/kicker light roughly
opposite the camera) shows a bright silver-white glint along its edge
even when `color` / `sigma_a` / `eumelanin`+`pheomelanin` are set to a
strongly saturated hue (black fur, red fur, dyed fur — doesn't matter).
This is expected, physically-correct behaviour, not a lost tint or a
missing painter binding — **don't chase it as a colour-pipe bug.**

**Why:** the bright backlit rim is the R (primary reflection) lobe of
the Chiang et al. 2016 / Marschner hair BCSDF — a specular reflection
directly off the fibre's outer cuticle, governed by dielectric Fresnel
reflectance at grazing angles.  Fresnel reflectance at grazing
incidence approaches 1.0 for every wavelength alike (hair's IOR is
~1.55 with negligible dispersion across the visible band), so the R
lobe is essentially colourless.  The fibre's melanin absorption
(`sigma_a` / `eumelanin` / `pheomelanin`) only tints the **transmitted**
lobes (TT, TRT) — light that actually enters the fibre core and picks
up the pigment's absorption spectrum on the way through.  A grazing
backlit ray is exactly the geometry where the specular R lobe
dominates and the pigmented TT/TRT lobes contribute least, so the rim
you see is mostly R — mostly white — by construction, on every real
furred/haired subject as much as in RISE.

**Rim vs. halo — these are two different, coexisting effects, not a
contradiction.** [docs/HAIR_FUR_DESIGN.md](HAIR_FUR_DESIGN.md) §2's own
lobe glossary calls TT "the bright halo when backlit," which is also
correct and describes a DIFFERENT visual feature: TT is genuine
transmission straight through the fibre core, so it produces a
broader, pigment-tinted GLOW across the fibre's visible width — the
classic warm backlit-hair glow. The RIM this section is about is a
much thinner feature sitting right at the fibre's silhouette edge,
where the local surface is nearly edge-on to the viewer and Fresnel
reflectance saturates toward 1.0 regardless of wavelength — that is R,
not TT. A single backlit strand typically shows both at once: a
colourless bright line exactly at the edge (R), inside a broader
tinted glow across the body (TT). If what you are chasing reads as a
broad warm glow rather than a hairline-thin white edge, you are
already seeing the (correctly tinted) TT halo — the achromatic-rim
note above applies specifically to the thin edge feature.

**The levers that actually change a backlit rim's character are NOT
colour:**

- **Fibre density / count** (`hair_geometry.count`) — a denser coat
  scatters more TT/TRT (tinted) light back toward camera alongside the
  R rim, softening the white-edge effect with colour from behind.
- **Light angle** — directly behind (180°) maximizes the pure-R grazing
  geometry; rotating the kicker off-axis brings more TT/TRT into the
  visible mix.
- **`width_root` / `width_tip`** and **`medulla_ratio`/`medulla_scatter`**
  (animal fur) — a thicker fibre or a stronger medulla increases
  internal scattering path length, giving TT/TRT more chance to tint
  the escaping light.

Tuning `color` / `sigma_a` / melanin further will change the ALBEDO
(the base fur colour under front/top lighting) without moving the rim,
because the rim was never reading that tier to begin with.

See [docs/HAIR_FUR_DESIGN.md](HAIR_FUR_DESIGN.md) §2.3 (Chiang et al.
2016 lobe decomposition) and §3.1 (parameter → pipe mapping) for the
model this follows.

---

## 11.5. Deprecated materials (DL-323 follow-through, 2026-10-02)

**Ruling (user, 2026-10-02):** the legacy non-physically-based material
chunks are being DEPRECATED, not retrofitted.  A deprecated chunk **keeps
parsing, deriving and rendering exactly as it always has** -- no
energy-conservation clip is added to it (that is the DL-310 policy, which
stops at Schlick and Ward; DL-323 recorded that Cook-Torrance and Phong
exceed 1 at grazing).  New scenes should use the modern chunks; this
section is the legacy -> modern table.

**What deprecation does.**  `ChunkDescriptor::deprecated` + `replacement`
(set by `MarkDeprecated()` in `ChunkParserRegistry.cpp`):

- `DeriveToJob` logs ONE `eLog_Warning` per deprecated chunk TYPE per
  scene load (`` `schlick_material` is DEPRECATED (2 chunk(s) in this scene;
  they still render exactly as before): use ggx_material ... ``).  It is
  never a derive diagnostic, so a successful load stays successful.
- the agent's `read_schema` JSON carries `"deprecated":true` and a
  `"replacement"` string; the descriptor `description` is prefixed
  `DEPRECATED (...)`, so the keyword completion popup and the property
  panel's chunk-type row show it too.
- nothing else moves: `tests/DeprecatedMaterialRenderIdentityTest.cpp`
  renders a scene binding all seven chunks and requires the pixel hash
  measured on the parent commit.

**Classification of every material chunk.**

| chunk | verdict | why |
|---|---|---|
| `cooktorrance_material`, `isotropic_phong_material`, `ashikminshirley_anisotropicphong_material`, `schlick_material`, `ward_isotropic_material`, `ward_anisotropic_material`, `polished_material` | **DEPRECATED** | legacy lobe models, each fully covered by `ggx_material` (+ `coated_material` for polished); see the table below |
| `ggx_material`, `pbr_metallic_roughness_material`, `coated_material`, `fabric_material`, `weave_material`, `composite_material`, `sheen_material`, `hair_material`, `lambertian_material`, `orennayar_material`, `dielectric_material`, `perfectreflector_material`, `perfectrefractor_material`, the SSS / skin / tissue family, `lambertian_luminaire_material`, `datadriven_material` | modern / specialised -- keep | physically based, or an ideal/measured model with no deprecated twin (Oren-Nayar is a physically derived rough-diffuse whose albedo is baked exactly, DL-07; `datadriven_material` is measured data) |
| `translucent_material` | **NOT deprecated** (no real replacement) | the only two-sided diffuse-transmitting THIN-sheet model (leaf, paper, lampshade) with per-side `ref`/`tau` and Beer extinction.  Considered and rejected as replacements: `dielectric_material` (a Fresnel interface with an IOR-stack, no per-side diffuse reflect/transmit split), `subsurfacescattering_material` / `randomwalk_sss_material` (volumetric SOLIDS that need a closed body and a mean free path), `weave_material transmission thin` (cloth gap).  It was also rebuilt as one consistent sampler/density/evaluator in DL-157/DL-41.  Revisit if a thin-sheet material is added |
| `phong_luminaire_material` | **NOT deprecated** (no real replacement) | an emission PROFILE (`cos^N` directional exitance) over another material; `lambertian_luminaire_material` has no equivalent shaping, and `spot_light` is a different entity |

**Legacy -> modern mapping.**  The translations are *starting points*:
the modern model is energy-bounded and multiple-scattering-compensated, so
a converted material is physically better but is not pixel-identical, and
the parametrisations differ (Phong exponent vs GGX alpha, etc.).  **No
translation is exact.**  The closest is Cook-Torrance, which shares the GGX
distribution `D` with `ggx_material` -- but its `G` is the separable
`G1(wi) G1(wo)` where GGX uses the height-correlated `G2`, its multiscatter
compensation reads the separable `LookupEss`/`LookupEavg` tables where GGX
reads the `G2` twins (DL-63), and it adds an uncoupled `Rd/pi` diffuse where
GGX applies DL-37's `(1-A(i))(1-A(o))`; so even that one is a close
starting point, not identical.

| legacy chunk | modern replacement | parameter translation |
|---|---|---|
| `cooktorrance_material` | `ggx_material`, `fresnel_mode conductor` (the default) | `rd`->`rd`, `rs`->`rs`, `ior`->`ior`, `extinction`->`extinction`, `facets`->`alphax` and `alphay` (same GGX `D`, so the highlight width matches; `G`, the multiscatter LUT and the diffuse coupling differ -- a close starting point, not identical).  For a glTF-style metal or plastic use `pbr_metallic_roughness_material` instead (`roughness` = sqrt(alpha)) |
| `isotropic_phong_material` | `ggx_material`, `fresnel_mode schlick_f0` | `rd`->`rd`, `rs`->`rs` (becomes the F0 tint), `alphax = alphay = 1/sqrt(2N+1)`: the Phong lobe is `cos^N` about the REFLECTION vector (`IsotropicPhongBRDF.cpp`), whose angle is twice the half-vector angle, so the half-vector-equivalent exponent is `4N` (the Blinn-Phong rule) and `alpha = sqrt(2/(4N+2))` |
| `ashikminshirley_anisotropicphong_material` | `ggx_material`, `fresnel_mode schlick_f0` | `rd`->`rd`, `rs`->`rs` (F0; Ashikhmin-Shirley already uses Schlick Fresnel), `alphax = sqrt(2/(nu+2))`, `alphay = sqrt(2/(nv+2))`; brush direction via `tangent_rotation_scalar` |
| `schlick_material` | `ggx_material`, `fresnel_mode schlick_f0` | `rd`->`rd`, `rs`->`rs`, `alphax = alphay = sqrt(roughness)` (Schlick's `r` is the GGX alpha squared: its Z(t) is the GGX D without the 1/pi).  `isotropy < 1` has no exact translation -- pick `alphax != alphay` by eye |
| `ward_isotropic_material` | `ggx_material`, `fresnel_mode schlick_f0` | `rd`->`rd`, `rs`->`rs`, `alphax = alphay = alpha` (GGX has heavier tails than a Gaussian lobe of the same width, so expect a slightly broader halo) |
| `ward_anisotropic_material` | `ggx_material`, `fresnel_mode schlick_f0` | `alphax`->`alphax`, `alphay`->`alphay`; `tangent_rotation_scalar` for the direction |
| `polished_material` | `coated_material` over a `lambertian_material` | `base` = a `lambertian_material` with the same `reflectance`; `coat_ior = ior`; `coat_roughness = 1/sqrt(2*scattering+1)` (the coat lobe is `cos^N` about the reflection vector, `PolishedBRDF::ComponentDensity`, same 4N rule as Phong), and for the delta case (`scattering` >= 1e6, `kPhongDeltaThreshold`) use the smallest `coat_roughness` (it is floored at 1e-3, so `coated_material` is never a true delta).  **`tau` has no equivalent**: it is NOT a transmittance tint -- `PolishedBRDF` multiplies only the coat REFLECTION by `tau` (`tau min(F(ci),F(co))` glossy, `tau F(ci)` delta) while the substrate term `(1-F(ci))(1-F(co))` is `tau`-independent; `tau 1` is a full coat, and `coat_weight` is the nearest knob for a weaker one (`coat_tint` / `coat_absorption` act on transmission, a different thing).  `henyey-greenstein TRUE` has no equivalent either.  `add_wetness` already emits the `coated_material` shape |

**Producers audited (2026-10-02).**  Nothing in the importers emits a
deprecated chunk: the glTF importer and the Blender bridge build
`pbr_metallic_roughness_material` / `ggx_material` / `coated_material` /
`fabric_material` / `randomwalk_sss_material` only, and `add_wetness`
wraps with `coated_material`.  The agent texture recipes
`rough_stone` and `aged_bronze` (were `cooktorrance_material`) and
`brushed_metal` (was `ward_anisotropic_material`) in `AgentSession.cpp`
emit `ggx_material` since DL-400 (2026-10-02; `conductor` with
`extinction 1` for `aged_bronze`, `schlick_f0` for `brushed_metal` and, since DL-415, `rough_stone` (0.04 dielectric F0 tint); a measured
look change, numbers in the DL-400 ledger row); the eval
fixtures that name the legacy kinds are hand-written scenes and keep them.
The GUI node palettes list the deprecated kinds last with a
"(deprecated -> ...)" badge (DL-401).  The
agent's skill docs now steer to the modern chunks
(`skills/agent/materials-and-media-basics.md`).  The 3ds Max plugin
(`src/3DSMax`) calls the legacy `IJob` entry points directly and is not
built here.

**Shipped scenes (not migrated -- their looks are kept).**  Counted by
CHUNK-HEADER match (a line that is exactly the keyword, optionally with a
trailing comment; not a word grep) over `git ls-files scenes` at the merge
with master `0dfda8c05` -- untracked / gitignored local scenes such as
`scenes/Internal` are NOT counted (a filesystem grep of a developer
checkout reads higher, 40 / 19 in the main checkout today).  Of the 464
tracked `.RISEscene` files, **35** use at least one deprecated chunk:
`polished_material` 17 scenes / 139 chunks, `cooktorrance_material` 11 /
26, `isotropic_phong_material` 9 / 9, `ward_anisotropic_material` 6 / 6,
`ward_isotropic_material` 5 / 11, `ashikminshirley_anisotropicphong_material`
5 / 5, `schlick_material` 4 / 4.  (Not deprecated, for reference:
`translucent_material` 12 scenes / 14 chunks, `phong_luminaire_material`
1 / 1.)  Loading any of them now logs the warnings above and renders
unchanged; `CstDeriveGoldenTest` (derive-state digests) is unaffected.

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

## Material coverage (DL-214)

Every material chunk accepts `alpha_mode opaque|mask|blend`, `alpha_coverage`
(a physical scalar painter name or numeric literal, default 1), and
`alpha_cutoff` (default 0.5). MASK accepts coverage at or above cutoff; BLEND
accepts with probability equal to coverage. Values clamp to [0,1]; invalid
values become zero. OPAQUE ignores the scalar. This is surface coverage,
separate from dielectric transmission. Existing Ward/hair `alpha` parameters
retain their original meanings. See [ALPHA_COVERAGE.md](ALPHA_COVERAGE.md).
