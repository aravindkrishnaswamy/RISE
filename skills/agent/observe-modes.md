# Observe Modes: Choosing How to Look at the Scene
> hook: Read before deciding HOW to look at the scene -- read_viewport, render{quality:"draft"}, render{mode:"objectmap"}/query_object_at, and a production render each answer a DIFFERENT question at a DIFFERENT cost; the wrong pick either lies to you or burns a full render for nothing.

There are exactly FOUR ways to look at the scene through the agent
surface.  They are not interchangeable, and three of the four are
CHEAP PRECISELY BECAUSE they answer a narrower question than "what
does this look like" -- reaching for the wrong one either gets you a
confidently wrong answer (judging colour from a draft render) or pays
full production cost for a question a free/near-free call already
answers (checking whether an edit landed by re-rendering production).

## The decision table

| Intent | Call | Cost | Trust it for | Do NOT trust it for |
|---|---|---|---|---|
| "What is the user seeing right now?" | `read_viewport {maxEdge?}` | Free -- copies the GUI's last interactive frame, NEVER renders | The exact live frame, whatever pipeline produced it | Anything when `available:false` (`no_controller`: headless session, no viewport at all; `no_frame_yet`: viewport exists but hasn't produced a frame) -- fall back to a `render` call instead of retrying |
| "Is this object roughly where I want it?" | `render {quality:"draft", width, height, camera?}` | Cheap -- a wholly separate fixed studio-preview pipeline, samples capped at 4 regardless of what you ask for | Geometry, silhouette, composition, camera framing; relative depth/placement (ESPECIALLY with a second `camera` angle -- one view alone can't tell front-of/behind/inside) | Materials, lighting, exposure, or colour -- the preview shader IGNORES the scene's authored materials and lights entirely |
| "Which object is where? / Find object X on screen." | `render {mode:"objectmap"}` (survey, whole-frame legend) or `query_object_at {x,y}` (one answer) | About one identity render -- fixed 1 spp, no MC noise, ignores `quality`/`samples` entirely | Exact identity: byte-exact `colorHex` <-> `name` legend match, including CSG composites (legend carries the ROOT only, never the hidden operands) and instance arrays (`grid[i,j]`) | The colours as APPEARANCE -- they are arbitrary per-render identity ids, not materials. Read the objectmap PNG at NATIVE size only (omit `maxEdge` -- a box-downscale blends flat ids and breaks the match) |
| "Does it actually look right -- materials, lighting, exposure?" | `render` (no `quality`, i.e. production) + `read_image`, or `read_viewport` if a GUI is attached AND its integrator is what you care about | The real cost -- the scene's actual configured integrator at its actual sample count | The ONLY mode where colour/shading/exposure judgments are honest | Nothing withheld -- it's simply the expensive one; don't make it your only feedback loop mid-edit |
| "Verify after an edit." | The cheap loop first (draft and/or objectmap/query at small dims), production LAST once confident | Escalating -- cheap checks first, one production pass at the end | Catching gross breakage (wrong object, black frame, object moved to the wrong side) cheaply, repeatedly | Calling it "done" off a cheap check alone -- ship the final judgment on one production render |

## Escalation ladder (cost, cheapest first)

1. **`read_viewport`** -- free, no render at all; only works when a live
   GUI controller is attached and has produced at least one frame.
2. **`query_object_at` / `render {mode:"objectmap"}`** -- about one
   small identity render (fixed single-fidelity, no samples/quality to
   tune); use `query_object_at` for a single pixel's answer, the full
   `mode:"objectmap"` render when you need the whole-frame survey (the
   legend for every visible object at once).
3. **`render {quality:"draft"}`** -- a real render through a cheap
   separate pipeline, samples capped at 4; tiny `width`/`height` keep
   it fast. Both this and step 2 work on a head with NO active
   production rasterizer chunk -- neither dereferences one.
4. **`render` (production)** -- the real integrator at its authored
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
   `renderMode` field ("production"/"draft"/"objectmap") to see which
   pipeline actually ran -- `integrator` always names the head's
   active PRODUCTION rasterizer regardless of `quality`, so it is NOT
   the field that tells you which shading produced THIS image.
2. **Objectmap/`query_object_at` colours are identity ids, not
   appearance.** Match by exact `colorHex` byte, never "by eye" --
   when the palette is exhausted the result's `message` says so
   explicitly. Read an objectmap render's PNG at NATIVE size (omit
   `maxEdge` on `read_image`) -- downscaling box-blends the flat ids
   and breaks the legend match.
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
5. **`width`/`height` overrides must be paired, on every one of these
   verbs.** Passing only one is silently ignored (not rejected) and
   the call proceeds at the scene's authored dims -- always pass BOTH,
   clamped `[16,512]`, and confirm the override took by reading
   `previewWidth`/`previewHeight` (`render`) or `width`/`height`
   (`query_object_at`) back from the result.
6. **Draft and objectmap/query_object_at work on a rasterizer-less
   head; production does not.** Both cheap-render paths run their own
   ephemeral pipeline and never touch the production rasterizer, so
   they succeed even before any `*_rasterizer` chunk exists in the
   scene -- a production `render` call on the same head fails honestly
   instead.

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

Step 3 -- once the edit is confirmed cheaply, the ONE production render
that actually judges the result (materials, lighting, exposure are
only honest here):

```json
{"method": "render", "params": {}}
```

## Traps

1. **`renderMode` vs `integrator` -- read the right field.** `renderMode`
   ("production"/"draft"/"objectmap") tells you which pipeline made
   THIS image; `integrator` always names the head's active PRODUCTION
   rasterizer chunk regardless of `quality`. Checking `integrator` to
   see if a draft render "used PT" is a category error -- it always
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
