# Observe Modes: Choosing How to Look at the Scene
> hook: Read before deciding HOW to look at the scene -- read_viewport, render{quality:"draft"}, render{mode:"objectmap"|"normals"|"depth"|"facets"|"wireframe"|"deep_reflect"|"direct"}/query_object_at, and a production render each answer a DIFFERENT question at a DIFFERENT cost; the wrong pick either lies to you or burns a full render for nothing.

There are exactly FOUR FAMILIES of call for looking at the scene
through the agent surface.  They are not interchangeable, and three of
the four are CHEAP PRECISELY BECAUSE they answer a narrower question
than "what does this look like" -- reaching for the wrong one either
gets you a confidently wrong answer (judging colour from a draft
render) or pays full production cost for a question a free/near-free
call already answers (checking whether an edit landed by re-rendering
production).

Within the third family (`render{mode:}`), GUI render modes P1
(docs/gui/RENDER_MODES.md) widens the mode value from a single choice
(`objectmap`) to SIX: `beauty` (default), `objectmap`, and four
structural DIAGNOSTIC modes -- `normals`, `depth`, `facets`,
`wireframe` -- each answering one narrow structural question with the
SAME exact, single-pass, 1-spp cost profile as `objectmap`.  GUI render
modes P2a adds TWO MORE mode values, `deep_reflect` and `direct` -- but
these are a DIFFERENT cost tier: real production-class path-traced
renders (real materials, real lights, real OIDN denoise) at a FIXED
reduced resolution and FIXED higher sample count, not single-pass
diagnostics. They cost SECONDS, not milliseconds -- more than a
diagnostic mode, still far cheaper than a full-resolution production
render because of the resolution divisor. See "View modes" below for
the per-mode decision table, and "Transport modes" for the two P2a
additions specifically.

## The decision table

| Intent | Call | Cost | Trust it for | Do NOT trust it for |
|---|---|---|---|---|
| "What is the user seeing right now?" | `read_viewport {maxEdge?}` | Free -- copies the GUI's last interactive frame, NEVER renders | The exact live frame, whatever pipeline produced it | Anything when `available:false` (`no_controller`: headless session, no viewport at all; `no_frame_yet`: viewport exists but hasn't produced a frame) -- fall back to a `render` call instead of retrying |
| "Is this object roughly where I want it?" | `render {quality:"draft", width, height, camera?}` | Cheap -- a wholly separate fixed studio-preview pipeline, samples capped at 4 regardless of what you ask for | Geometry, silhouette, composition, camera framing; relative depth/placement (ESPECIALLY with a second `camera` angle -- one view alone can't tell front-of/behind/inside) | Materials, lighting, exposure, or colour -- the preview shader IGNORES the scene's authored materials and lights entirely |
| "Which object is where? / Find object X on screen." | `render {mode:"objectmap"}` (survey, whole-frame legend) or `query_object_at {x,y}` (one answer) | About one identity render -- fixed 1 spp, no MC noise, ignores `quality`/`samples` entirely | Exact identity: byte-exact `colorHex` <-> `name` legend match, including CSG composites (legend carries the ROOT only, never the hidden operands) and instance arrays (`grid[i,j]`) | The colours as APPEARANCE -- they are arbitrary per-render identity ids, not materials. Read the objectmap PNG at NATIVE size only (omit `maxEdge` -- a box-downscale blends flat ids and breaks the match) |
| "Does it actually look right -- materials, lighting, exposure?" | `render` (no `quality`, i.e. production) + `read_image`, or `read_viewport` if a GUI is attached AND its integrator is what you care about | The real cost -- the scene's actual configured integrator at its actual sample count | The ONLY mode where colour/shading/exposure judgments are honest | Nothing withheld -- it's simply the expensive one; don't make it your only feedback loop mid-edit |
| "Verify after an edit." | The cheap loop first (draft and/or objectmap/query at small dims), production LAST once confident | Escalating -- cheap checks first, one production pass at the end | Catching gross breakage (wrong object, black frame, object moved to the wrong side) cheaply, repeatedly | Calling it "done" off a cheap check alone -- ship the final judgment on one production render |

## View modes: which `render{mode:}` value answers which question

`objectmap` answers "which object is where"; the other four are
structural DIAGNOSTIC modes that answer a narrower question still --
not identity, but geometry.  All six share ONE call shape
(`render {mode:"..."}`) and ONE cost profile (fixed 1 spp, single
exact pass, `quality`/`samples` both honestly ignored -- noted in the
result `message`).  `renderMode` in the result always echoes back the
exact mode name that ran.

| mode | Question it answers | Read it as | `legend`? |
|---|---|---|---|
| `beauty` (default) | What does the scene actually look like -- materials, lighting honored? | The real image (or the draft preview under `quality:"draft"`) | No |
| `objectmap` | Which object is at which pixel? | Flat per-object identity colour, byte-exact `colorHex` match | Yes |
| `normals` | Which way do surfaces face? | World-space shading normal as false colour (`0.5*(N+1)`) | No |
| `depth` | How far away is everything? | Grayscale, near = bright, AUTO-WINDOWED per render to the VISIBLE hit-distance range in that frame (see limitation below) | No |
| `facets` | What does the actual tessellation look like -- where does shading disagree with geometry? | Headlamp-shaded GEOMETRIC normal -- no smoothing, no bump, no shading-normal interpolation | No |
| `wireframe` | Where are the polygon edges? | Triangle-mesh edges over dim facet shading (see mesh-only limitation below) | No |
| `deep_reflect` | What do reflections and refractions resolve to? | REAL path-traced render, quarter-res, 16 spp, 24 bounces, OIDN-denoised | No |
| `direct` | What does direct lighting alone contribute? | REAL path-traced render, half-res, 8 spp, direct lighting only (1 bounce), OIDN-denoised | No |

### Transport modes: `deep_reflect` / `direct` (GUI render modes P2a)

Unlike the four false-colour diagnostics above, `deep_reflect` and
`direct` are REAL renders through the scene's actual materials and
lights -- an ephemeral, fixed-config production-class path tracer, not
a first-hit shader. Two consequences that don't apply to
`normals`/`depth`/`facets`/`wireframe`:

- **Cost**: seconds, not milliseconds. Still cheap relative to a
  full-resolution production render (the fixed resolution divisor --
  quarter-res for `deep_reflect`, half-res for `direct` -- does most of
  the work), but do not treat them as free the way you'd treat
  `objectmap`/`normals`/etc.
- **`effectiveSamples` reports the REAL spp used** (16 or 8), not the
  "1" the false-colour diagnostics report -- `quality`/`samples` are
  still ignored (the config is fixed by the registry), but check
  `effectiveSamples` if you want to confirm which mode actually ran at
  what fidelity.
- **`xray` is ignored** (honestly noted) -- skipping through glass
  would defeat the whole point of `deep_reflect` (seeing what the glass
  itself resolves to).

Recipes:

- **"What do the reflections/refractions in the crystal/case/metal
  actually resolve to?"** -- `render {mode:"deep_reflect"}`. Deep
  bounce depth (24) and a real sample count (16 spp) let specular
  chains (glass-through-glass, metal-on-metal, a caustic-adjacent
  reflection) actually converge, unlike a draft/false-colour render
  which either ignores materials entirely or only resolves the first
  hit.
- **"Is the LIGHTING right, independent of indirect bounce / GI?"** --
  `render {mode:"direct"}`. Direct-only transport (no indirect bounces)
  isolates whether light placement/power/colour looks right before
  indirect bounce and material response are layered on top -- a
  narrower, cheaper question than a full production render.

### Recipes: matching a symptom to a mode

1. **Placement check ("is X where I think it is / did the move land?")**
   -- `render {mode:"objectmap"}` for the whole-frame survey, then
   `query_object_at {x,y}` to confirm a specific pixel. Same recipe the
   decision table above already gives for identity questions.
2. **Orientation or smoothing artifact ("this looks shaded wrong / the
   normal looks flipped / there's a facet crease that shouldn't be
   there")** -- reach for `normals` first (world-space shading normal
   as false colour -- catches flipped windings, bad tangent frames,
   inverted normal maps at a glance). If `normals` looks plausible but
   the SILHOUETTE still creases oddly, switch to `facets` (the
   GEOMETRIC normal, bypassing any shading-normal smoothing/bump) to
   tell apart "the mesh itself is faceted" from "the shading normal is
   wrong" -- `normals` can look smooth over a low-poly mesh (smoothed
   shading normals hiding the facets) while `facets` reveals the true
   tessellation underneath.
3. **Tessellation / edge layout ("how is this mesh actually
   triangulated? where are the UV seams likely to fall? is this
   polycount too coarse for that curve?")** -- `wireframe`. Remember
   the mesh-only limitation below before reaching for it on an analytic
   primitive or SDF.
4. **Scale or occlusion sanity ("is this object actually behind that
   wall, or does the camera clip through geometry, or is that gap real
   depth or just dim shading?")** -- `depth`. Read brightness as
   RELATIVE distance within this one frame, not an absolute unit scale
   (see the normalization limitation below).
5. **"What's under/inside that glass / crystal / transparent cover?"**
   -- any of the four false-colour view modes already sees through it:
   `xray` defaults to `true`, so `render {mode:"depth"}` or `render
   {mode:"facets"}` already resolve the ray straight through
   transmissive surfaces to the first opaque hit (no refraction bending
   -- deliberately an x-ray, not an optics simulation). Add
   `xray:false` to see the glass surface itself instead of what's
   underneath it. See "X-ray axis" below. (`deep_reflect` answers a
   DIFFERENT question about the same glass -- not what's under it, but
   what the glass ITSELF does to light passing through -- and ignores
   `xray` entirely; see "Transport modes" above.)
6. **"What do reflections/refractions actually resolve to?"** --
   `render {mode:"deep_reflect"}`. See "Transport modes" above.
7. **"Is the lighting right, independent of materials/indirect
   bounce?"** -- `render {mode:"direct"}`. See "Transport modes" above.
8. **"Render the same scene from several saved angles."** -- pass
   `view:"<name>"` on ANY `render` call (composes with every mode
   above). Resolves a live GUI session's Named View bookmark, or --
   headless -- a scene camera of that exact name. See "The `view`
   param" below.

### Known limitations (read before reporting a "bug")

1. **`wireframe` draws edges on triangle meshes only.** Analytic
   primitives (sphere, box, cylinder, ...) and SDF geometry have no
   tessellation to draw an edge FROM -- they render as dim facet
   shading with no lines. That is correct, documented behaviour, not a
   missing feature -- see docs/gui/RENDER_MODES.md §10.
2. **`depth` brightness is AUTO-WINDOWED per render to the VISIBLE
   hit-distance range in THAT frame, not a fixed distance unit.** Two
   renders of the same scene from different camera framings (a wide
   shot vs. a close-up) use DIFFERENT brightness scales -- never compare
   two `depth` renders as if they shared one absolute scale, only read
   brightness RELATIVE to other pixels in the SAME image. The window
   self-calibrates within a single `render` call, so you always get a
   windowed (not flat/fallback) image. (The engine falls back to a fixed
   scene-bounding-box-diagonal scale only for a genuinely degenerate
   scene -- an empty frame or a single flat plane filling it -- so a
   mostly-flat scene can still read as unusually uniform; that is
   expected, not a broken render.)
3. **Every non-`beauty` mode (`objectmap`, the four false-colour view
   modes, AND the two transport modes) ignores `quality` and `samples`
   unconditionally.** Each has exactly one FIXED fidelity by design --
   an exact single-pass diagnostic image for `objectmap`/`normals`/
   `depth`/`facets`/`wireframe`, a fixed higher-spp production-class
   render for `deep_reflect`/`direct` -- requesting a different sample
   count or `quality:"draft"` under any of them is a silent no-op,
   honestly noted in the result `message`, not an error. Check
   `effectiveSamples` for the ACTUAL spp used (1 for the false-colour
   modes, 16/8 for deep_reflect/direct).
4. **`xray` is a straight line, not real refraction.** It follows the
   ORIGINAL ray direction through every transmissive surface it skips
   -- no bending, no lensing. It answers "what's under/inside this
   transmissive geometry", not "what would actually be seen looking
   through it". A miss after 16 skipped surfaces shows the LAST
   transmissive surface instead of a black hole -- an honest partial
   answer, not a bug.

### X-ray axis: seeing through transmissive geometry

`xray` is an optional boolean param on `render`, ORTHOGONAL to `mode` --
it composes with all four view modes (`normals`/`depth`/`facets`/
`wireframe`), not a mode of its own. **Defaults to `true`** (2026-07-17):
a view-mode render already resolves each ray THROUGH transmissive
(glass-like) surfaces to the first OPAQUE hit, following the ORIGINAL
ray's straight line with NO refraction bending -- deliberately an
x-ray, not an optics simulation. Up to 16 transmissive surfaces are
skipped per ray.

Recipe: **"what's under/inside the glass/crystal/transparent cover?"**
-- just `render {mode:"depth"}` or `render {mode:"facets"}`, no extra
param needed. Add `xray:false` to see the glass/crystal surface itself
instead of what's underneath it -- e.g. to inspect a cover's own
tessellation or normals rather than the mechanism inside.

`xray` is silently ignored (honestly noted in the result `message`)
under `mode:"beauty"`, `mode:"objectmap"`, or a transport mode
(`deep_reflect`/`direct`) -- it only means something for the four
false-colour view-mode diagnostics.

## The `view` param: rendering from a saved vantage

`view` is an optional string param on `render`, ORTHOGONAL to `mode` --
it composes with EVERY mode (`beauty`, `objectmap`, any view mode,
either transport mode). Pass a name instead of raw `camera`
location/lookat/up/fov numbers:

```json
{"method": "render", "params": {"mode": "deep_reflect", "view": "hero-angle"}}
```

Resolution order: (1) a live in-app GUI session's Named View bookmark
of that exact name; (2) headless (`rise --agent-stdio`, no live
controller), a scene CAMERA of that exact name. An unresolved name
FAILS the render (`ok:false`) with the available-name list in
`message` -- it never silently falls back to the active camera. If
BOTH `view` and an explicit `camera` override are supplied, `view`
wins.

Use it to compare the SAME render mode from several saved angles
without re-deriving camera math each call -- e.g. checking
`deep_reflect` from three named views of a jewel/watch-crystal scene to
confirm the reflections read correctly from every angle a user might
actually look from.

## Escalation ladder (cost, cheapest first)

1. **`read_viewport`** -- free, no render at all; only works when a live
   GUI controller is attached and has produced at least one frame.
2. **`query_object_at` / `render {mode:"objectmap"|"normals"|"depth"|
   "facets"|"wireframe"}`** -- about one identity-or-diagnostic render
   at the EFFECTIVE dims (the scene's authored dims unless you pass
   paired `width`/`height` -- do pass small ones, e.g. 128x128, or a
   large authored film makes this step needlessly expensive); fixed
   single-fidelity, no samples/quality to tune, for ALL five mode
   values. Use `query_object_at` for a single pixel's identity, the
   full `mode:"objectmap"` render for a whole-frame identity survey, or
   one of the four structural view modes (see "View modes" above) when
   the question is about geometry/normals/tessellation rather than
   identity.
3. **`render {quality:"draft"}`** -- a real render through a cheap
   separate pipeline, samples capped at 4; tiny `width`/`height` keep
   it fast. Both this and step 2 work on a head with NO active
   production rasterizer chunk -- neither dereferences one.
4. **`render {mode:"deep_reflect"|"direct"}`** -- a REAL production-class
   render (real materials/lights/OIDN) at a fixed reduced resolution and
   fixed higher sample count -- seconds, not milliseconds, but still far
   cheaper than full-res production. Reach for this ONLY for the narrow
   transport/lighting question each answers (see "Transport modes"
   above); it costs more than every step before it. Also works on a
   head with NO active production rasterizer chunk (its own ephemeral
   pipeline, never the production one).
5. **`render` (production)** -- the real integrator at its authored
   (or overridden) sample count. The only step that costs what the
   final image costs; do it last, once the cheap steps already agree
   the edit is roughly right.

This is the loop the modeling-workflow-and-geometry skill's "observe
loop" section already teaches for the geometry-blockout case (insert,
draft-preview, second angle, confirm, THEN full-size) -- this skill
generalizes that same cost-aware escalation to the other three lenses
(viewport, objectmap/query, production) it doesn't cover. Read that
skill first for the multi-angle geometry workflow; come here to decide
which LENS to reach for at each step, including the two it doesn't
mention (viewport, objectmap/query).

## Hard warnings

1. **`quality:"draft"` renders through a wholly separate, fixed
   studio-preview pipeline that IGNORES the scene's authored materials
   and lighting entirely.** Geometry, composition, and camera framing
   are representative; materials, lighting, exposure, and colour are
   NOT. NEVER judge those from a draft image. Check the result's
   `renderMode` field ("production"/"draft"/"objectmap"/"normals"/
   "depth"/"facets"/"wireframe") to see which pipeline actually ran --
   `integrator` always names the head's active PRODUCTION rasterizer
   regardless of `quality`, so it is NOT the field that tells you which
   shading produced THIS image.
2. **Objectmap/`query_object_at` colours are identity ids, not
   appearance.** Match by exact `colorHex` byte, never "by eye" --
   when the palette is exhausted the result's `message` says so
   explicitly. Read an objectmap render's PNG at NATIVE size (omit
   `maxEdge` on `read_image`) -- downscaling box-blends the flat ids
   and breaks the legend match. A pixel that matches NO legend entry
   is the reserved BACKGROUND: RGB `000000` at alpha 0 (never assigned
   to an object); the magenta `FF00FF` sentinel plus an `<unmapped>`
   legend entry mark the should-not-happen unregistered-object case.
   Pixel coordinates: `(0,0)` is the TOP-LEFT corner and y grows
   DOWNWARD, for both `query_object_at {x,y}` and the decoded PNG's
   row order -- they always agree.
3. **`read_viewport` does not exist on a headless session.**
   `available:false` with `reason:"no_controller"` (no live GUI at
   all) or `"no_frame_yet"` (a viewport exists but hasn't rendered
   yet) is a STRUCTURED SUCCESS, not an error -- don't retry blindly;
   fall back to a `render` call when it's unavailable.
4. **A generator-synthesized legend name (`grid[0,1]`) is not a CST
   chunk.** `mode:"objectmap"` and `query_object_at` both report it
   verbatim so you can tell instances apart on screen, but to EDIT
   that instance you target the GENERATOR chunk (strip the `[i,j]`
   suffix), not the instance name.
5. **`width`/`height` overrides must be paired (on `render` and
   `query_object_at`; `read_viewport`/`read_image` take `maxEdge`
   instead).** Passing only one is silently ignored (not rejected) and
   the call proceeds at the scene's authored dims -- always pass BOTH,
   clamped `[16,512]`, and confirm the override took by reading
   `previewWidth`/`previewHeight` (`render`) or `width`/`height`
   (`query_object_at`) back from the result.
6. **Draft, objectmap/query_object_at, AND every view mode --
   `normals`/`depth`/`facets`/`wireframe` AND `deep_reflect`/`direct`
   -- all work on a rasterizer-less head; production does not.** Every
   cheap-render path (including the two transport modes) runs its OWN
   ephemeral pipeline and never touches the production rasterizer, so
   all of them succeed even before any `*_rasterizer` chunk exists in
   the scene -- a production `render` call on the same head fails
   honestly instead.
7. **`deep_reflect`/`direct` are NOT free like the other four view
   modes.** They cost seconds (a real path-traced render at a fixed
   reduced resolution/sample count), not milliseconds. Reach for a
   false-colour diagnostic (`normals`/`depth`/`facets`/`wireframe`) or
   `objectmap` first if the question is structural rather than about
   transport/lighting -- don't default to `deep_reflect` for a question
   `facets` or `depth` already answers for free.

## Worked example: locating an object, cheaply, before editing it

The scene below has a red sphere on the left and a blue box on the
right -- a stand-in for "find the sphere, then confirm it's roughly
centered before nudging it."

```rise
RISE ASCII SCENE 7

standard_shader
{
	name		global
	shaderop	DefaultPathTracing
}

pathtracing_pel_rasterizer
{
	samples			16
	pixel_filter	box
	oidn_denoise	FALSE
}

film
{
	width	128
	height	128
}

pinhole_camera
{
	location	0 2 6
	lookat		0 0.5 0
	up			0 1 0
	fov			45.0
}

uniformcolor_painter
{
	name	pnt_floor
	color	0.5 0.5 0.5
}

uniformcolor_painter
{
	name	pnt_sphere
	color	0.8 0.2 0.2
}

uniformcolor_painter
{
	name	pnt_box
	color	0.2 0.3 0.8
}

lambertian_material
{
	name		mat_floor
	reflectance	pnt_floor
}

lambertian_material
{
	name		mat_sphere
	reflectance	pnt_sphere
}

lambertian_material
{
	name		mat_box
	reflectance	pnt_box
}

clippedplane_geometry
{
	name	floor
	pta		-4 0 -4
	ptb		4 0 -4
	ptc		4 0 4
	ptd		-4 0 4
}

standard_object
{
	name		obj_floor
	geometry	floor
	material	mat_floor
}

sphere_geometry
{
	name	sph
	radius	0.6
}

standard_object
{
	name		obj_sphere
	geometry	sph
	material	mat_sphere
	position	-1.1 0.6 0
}

box_geometry
{
	name	box
	width	1.0
	height	1.0
	depth	1.0
}

standard_object
{
	name		obj_box
	geometry	box
	material	mat_box
	position	1.1 0.5 0
}

directional_light
{
	name		key
	power		3.0
	color		1 1 1
	direction	0.3 0.6 0.7
}
```

Step 1 -- cheap placement check, small draft render (materials/colour
NOT trustworthy here, only silhouette/position):

```json
{"method": "render", "params": {"quality": "draft", "width": 160, "height": 120}}
```

Step 2 -- "which object is at the pixel I think `obj_sphere` occupies?"
(cheap identity probe, no beauty render needed):

```json
{"method": "query_object_at", "params": {"x": 45, "y": 70}}
```

...or the whole-frame survey when more than one object needs locating
at once:

```json
{"method": "render", "params": {"mode": "objectmap"}}
```

If instead the box looked like it had a shading crease along one edge,
reach for a view mode rather than staring at the beauty render harder:

```json
{"method": "render", "params": {"mode": "facets"}}
```

Step 3 -- once the edit is confirmed cheaply, the ONE production render
that actually judges the result (materials, lighting, exposure are
only honest here):

```json
{"method": "render", "params": {}}
```

## Traps

1. **`renderMode` vs `integrator` -- read the right field.** `renderMode`
   ("production"/"draft"/"objectmap"/"normals"/"depth"/"facets"/
   "wireframe"/"deep_reflect"/"direct") tells you which pipeline made
   THIS image; `integrator`
   always names the head's active PRODUCTION rasterizer chunk
   regardless of `quality`. Checking `integrator` to see if a draft
   render "used PT" is a category error -- it always
   answers a different question.
2. **A draft render's `samples` request is honestly capped, not
   silently honored.** Ask for `samples:64` under `quality:"draft"`
   and the result's `effectiveSamples` still reads back the cap (4)
   with a note in `message` -- don't assume a higher request took
   effect just because it wasn't rejected.
3. **`read_viewport`'s `available:false` is not a retry signal.** A
   headless session (`rise --agent-stdio`, or this same skill's
   render-contract test harness) will NEVER have a viewport --
   spinning on `read_viewport` waiting for `available:true` there
   hangs forever for no reason. Use `render` instead.
4. **An empty-pixel `query_object_at` is `hit:false`, not an error.**
   The probe pixel resolved to background -- a normal, structured
   result. An actually out-of-range `(x,y)` for the effective film
   dims IS a clean error (checked before any render runs), which is a
   different failure mode from `hit:false` -- don't conflate the two.
