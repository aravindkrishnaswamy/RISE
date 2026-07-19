# Modeling From Image Captures
> hook: Read when a user wants to reconstruct an object from photos/captures they took -- what image input actually works today, and the reference-plane + canonical-view-matching workflow.

## Honest capability check (read this before promising anything)

As of this writing, **a user CAN attach reference images directly to a
chat message** (the picker button or a drag-and-drop onto the input
row) -- the agent genuinely sees the photo, as a real multimodal image
block/part on the wire, not a description of it.  Verify this hasn't
regressed before relying on it, but also do not overclaim beyond what
actually ships:

- The chat panel's attach affordance downscales the image (long edge
  1024px) and re-encodes it before sending -- a photo straight off a
  phone is never sent at full resolution.  The four attachable types
  are PNG, JPEG, GIF, and WEBP; anything else is rejected in the panel
  with a visible message, not silently dropped.
- Attached images PERSIST across turns up to a live cap (currently 4
  across the whole conversation) -- the agent can keep comparing new
  renders against a reference photo several turns later without the
  user re-attaching it.  Attaching beyond the cap elides the OLDEST
  live reference first (replaced by a placeholder note); if a
  multi-turn session needs more than the cap allows, the user may need
  to re-attach an earlier photo once it falls off.
- **There is still no `read_file` verb or equivalent in the agent RPC
  surface.**  The only file paths the agent can point at are
  scene-language `file` parameters resolved by the RENDERER when a
  scene derives (e.g. a `png_painter`'s `file`) -- the agent never sees
  THOSE bytes directly, it only sees the rendered result.  This
  distinction still matters for the reference-plane technique below:
  a capture placed on disk and referenced by a scene chunk is NOT the
  same path as a chat attachment, and the agent cannot inspect an
  arbitrary disk path the way it can inspect a chat attachment.
- `read_image` (the render-result tool result) is a SEPARATE mechanism
  from chat attachments, with its own independent policy (only the
  most recent render stays live) -- don't conflate the two when
  reasoning about what's "in context."

**What this means in practice:** ask the user to attach 2-3 capture
photos directly in chat rather than routing through the reference-plane
texture trick FIRST -- it's simpler, the agent sees the actual photo,
and it doesn't require the user to have a file already placed in the
scene's media path.  Fall back to the structured-questions flow (below)
only when the user hasn't got captures to attach, or when a capture has
fallen off the live-image cap in a long session and re-attaching isn't
convenient.  The reference-plane technique (placing a capture file on a
textured quad IN the render) is still useful for a different purpose --
letting the render itself put a side-by-side comparison in front of the
user -- but it is no longer the ONLY way a capture reaches the agent.

## The workflow

### 1. What makes a good set of captures

Ask for (or ask the user to confirm they have):

- **2-3 orthogonal-ish angles minimum**: front, 3/4 or side, and top if
  the object has interesting silhouette from above.  A single photo
  cannot separate "the handle is behind the body" from "the handle is
  attached" -- exactly the depth-ambiguity trap the observe loop's
  second-angle render exists to catch, now applied to the REFERENCE
  instead of the render.
- **A scale reference in frame** (a hand, a coin, a ruler) or a stated
  dimension -- "the mug is about 9cm tall" lets you size the blockout
  correctly instead of guessing a `radius` that matches the photo's
  crop but not the real object.
- **Even, diffuse lighting** on the captures if color/material matching
  matters -- a photo with hard directional shadows or colored bounce
  light makes it hard to read the object's OWN base color.

### 2. Structured questions (the FALLBACK, when there's no attached image)

When the user hasn't attached captures (or an earlier one has fallen
off the live-image cap and re-attaching isn't convenient), ask per-view
questions that map directly onto scene-authoring decisions instead:

- **Silhouette**: "From the front, is the outline closer to a cylinder,
  a tapered cone, or does it flare out then in (like a vase)?"
- **Proportions**: "What's the rough height-to-width ratio? Does the
  widest point sit at the top, middle, or bottom?"
- **Distinguishing features and their placement**: "Is there a handle,
  spout, or foot? Which side, and roughly what fraction of the total
  height is it at?"
- **Color/material per region**: "Is it one uniform color, or does the
  base differ from the body? Matte, glossy, or reflective?" -- feeds
  straight into `uniformcolor_painter` first (materials-and-media-basics),
  textures later.

Turn each answer into ONE geometry/material decision at a time, using
the blockout->refine workflow from object-modeling-recipes -- don't
try to encode every answer into one giant chunk edit.

### 3. The reference-plane technique (a secondary option, when a capture file IS on disk)

Now that chat attachments work, this technique is no longer the primary
way a capture reaches the agent -- prefer asking the user to attach the
photo directly (section 1's captures, sent via the chat panel).  Reach
for the reference-plane technique when the user separately has a photo
file already placed in the scene's media path (not necessarily the same
one they attached) and specifically wants it rendered INTO the frame for
their own side-by-side comparison: put it on a bounded quad NEXT TO the
blockout using `png_painter` + `clippedplane_geometry` -- the reference
and the work-in-progress render in the SAME frame, and the user (who can
see the render via `read_image`) can compare them directly instead of
tabbing between windows:

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
	location	0 0.6 4
	lookat		0 0.4 0
	up			0 1 0
	fov			45.0
}

uniformcolor_painter
{
	name	pnt_floor
	color	0.45 0.45 0.45
}

lambertian_material
{
	name		mat_floor
	reflectance	pnt_floor
}

infiniteplane_geometry
{
	name	floor
	xtile	1.0
	ytile	1.0
}

standard_object
{
	name		obj_floor
	geometry	floor
	material	mat_floor
	position	0 -0.5 0
	orientation	-90 0 0
}

# The reference capture as a texture on a bounded quad -- swap the
# `file` for the user's actual capture path once it's in the media
# path.  This snippet uses a texture already shipped in the repo at a
# path relative to the repo root, which resolves here only because
# this snippet is run from repo root (see the note below the snippet);
# a real session should substitute an ABSOLUTE path to the user's
# capture instead of relying on a relative one resolving by luck.
png_painter
{
	name	pnt_reference
	file	textures/cel.png
}

lambertian_material
{
	name	mat_reference
	reflectance	pnt_reference
}

clippedplane_geometry
{
	name	ref_quad
	pta		-1.2 -0.5 -1.5
	ptb		-0.2 -0.5 -1.5
	ptc		-0.2 0.5 -1.5
	ptd		-1.2 0.5 -1.5
}

standard_object
{
	name	obj_reference
	geometry	ref_quad
	material	mat_reference
}

# The work-in-progress blockout, placed BESIDE the reference quad so
# both are in frame together.
uniformcolor_painter
{
	name	pnt_subject
	color	0.6 0.3 0.2
}

lambertian_material
{
	name	mat_subject
	reflectance	pnt_subject
}

sphere_geometry
{
	name	subject_sph
	radius	0.5
}

standard_object
{
	name	obj_subject
	geometry	subject_sph
	material	mat_subject
	position	0.7 0 -1.0
}

directional_light
{
	name		key
	power		3.0
	color		1 1 1
	direction	0.3 0.6 0.7
}
```

`png_painter`'s `file` resolves the same way any scene-language file
path does -- first as literally given (relative to the process's
current working directory, or absolute), then through any configured
media-path search roots.  The snippet above uses the relative path
`textures/cel.png`, which only resolves without extra configuration
when the process's cwd IS the repo root (true in CI: the test suite
runs from repo root, and this is why the snippet works there
unmodified).  **In a real agent session the cwd is very likely
something else**, and a relative path that doesn't resolve as-is and
isn't found on any configured media-path root does NOT fail the
render -- `MediaPathLocator::Find` logs an error and gives up,
returning the path unresolved; the PNG reader then gets nothing to
read and the texture silently comes back empty.  The render still
completes with `ok:true` and no diagnostic anywhere in the result --
you get a scene that quietly has no reference image on the quad, and
nothing tells you that happened.

To avoid this: **use an ABSOLUTE path** for the user's reference
capture (or copy the capture next to the scene file and set
`RISE_MEDIA_PATH`/cwd accordingly so a relative path resolves).  Then,
regardless of which you chose, **verify the reference actually
appears** in the first render that includes the quad -- render at a
small size, `read_image`, and look at the reference quad's rectangle
for actual photo content (edges, color variation, a recognizable
silhouette) rather than a flat, featureless patch.  A flat patch where
the photo should be means the texture didn't load -- fix the path
before trusting anything about the comparison, since a missing texture
gives you no other signal that something is wrong.

### 4. Canonical-view matching

Once the reference is in-frame (or described precisely enough to
picture), match the RENDER's camera to the CAPTURE's approximate
viewpoint using a `render` camera override -- same mechanics as the
observe loop's multi-angle checks:

1. Guess the capture's camera height/distance/angle from the photo's
   framing (is the object shot from eye level, from above, close-up or
   from across a room -- these map to `location`/`fov`).
2. `render` with that `camera` override at a small size (128-160px);
   compare silhouette and proportions against the reference (either
   in-frame via the reference-plane technique, or against the user's
   description).
3. Adjust the BLOCKOUT geometry (not the camera) to fix proportion
   mismatches once the viewing angle is roughly matched -- don't chase
   a proportion error by fudging the camera instead of the model.
4. Repeat for a second and third view.  Matching one view and assuming
   the others follow is the same depth-ambiguity trap the observe loop
   warns about -- a silhouette that matches from the front can still be
   completely wrong in profile.

**Compare like-for-like.** When judging progress, render at the SAME
pose AND the SAME aspect ratio/dims as the reference photo before
deciding what to fix -- pass matching `width`/`height` on the `render`
override, not just a small default square.  A pose or crop mismatch
masquerades as a shape error: don't chase a proportion "fix" for a
difference that's actually camera framing.  Confirm pose and framing
agree first, then judge the shape.

### 5. The multi-view verification loop

Every iteration that touches geometry should end with verification
renders from MULTIPLE TEMPORARY viewpoints, not just the reference
pose.  Use the `render` tool's `camera` override at `quality:"draft"`,
small dims (128-160px) -- see observe-modes for why draft is the right
cost tier here -- to orbit the work-in-progress: at minimum the
reference pose plus 2-3 offset poses (a side view, a back-3/4, an
elevated view).  The override is TEMPORARY -- it never edits the
scene's `pinhole_camera` chunk -- so the scene camera stays pristine
for the final production comparison against the reference.

```json
{"method": "render", "params": {"quality": "draft", "width": 128, "height": 128,
  "camera": {"location": [4, 0.6, 0], "lookat": [0, 0.4, 0], "up": [0, 1, 0], "fov": 45.0}}}
```

An object that matches the reference view but looks WRONG from an
offset view has a SHAPE error the reference view was hiding -- the
same depth/silhouette ambiguity the observe loop's second-angle check
exists to catch (observe-modes), now applied every iteration instead
of once at the start.  Fix a shape error the moment an offset view
reveals it; don't keep polishing materials or detail on a form that's
about to change underneath them.

If only ONE reference image exists for the object, multi-view
self-consistency is the ONLY defense against building a billboard --
geometry that matches the single photo perfectly from its one angle
and is nonsense from every other.  State this bluntly: a lone
reference photo cannot confirm depth, back-side shape, or anything
occluded in that frame -- those parts are being invented, and the
offset-view renders only check that the invention is plausible, not
that it's correct.

When an offset view raises a question a beauty render can't answer
cleanly -- is the notch on the correct side, is the object actually
centered -- reach for `render {mode:"objectmap"}` or `query_object_at`
(observe-modes) instead of squinting at colors: one identity render
answers "what is actually there" structurally, cheaper than iterating
on a beauty render's ambiguity.

**Measure every iteration.** When reference images are REGISTERED (a
reconstruction eval registers its prompt attachments as `view1`,
`view2`, ... for this purpose; a live chat host may register attached
images the same way -- check what names are actually registered rather
than assuming), end EVERY iteration of this loop with
`compare_to_reference` against the primary view instead of eyeballing
convergence -- the returned `rmse` IS the graded objective, and the
job is to make it fall monotonically across iterations, not to feel
like it's converging:

- Use `channelDelta`'s SIGN to fix global colour/brightness first,
  before chasing anything structural: `channelDelta.r > 0` means the
  render is too RED on that channel -- check the environment tint and
  the key light's colour before touching geometry.
- Use `worstCell` plus the 3x3 `grid` to LOCALIZE the biggest
  remaining error before guessing what to fix next -- per the
  environment-first doctrine (section 6 below), the worst cell is very
  often the background/environment, not the object.
- Use the composite `[render | reference | abs-diff heatmap]` image
  (the default `visual:true`) to SEE the error's structure when the
  number alone doesn't tell you what changed; set `visual:false` once
  you only need the number, to save the encode cost and the response's
  token footprint.
- Use draft compares (the default -- omit `samples`) for composition
  passes: cheap, and adequate for silhouette/placement feedback.
  Switch to a production compare (`samples: 24` or similar) once
  you're judging colour, lighting, or material fidelity -- draft
  shading is NOT what the grader renders, so a low draft RMSE only
  confirms geometry/composition, never colour or material match (see
  the tool's own `samples` parameter doc for the full tradeoff).

**If -- and only if -- you need to settle whether the residual is the
STAGING or the OBJECT, you can measure it instead of guessing.**
`compare_to_reference` takes `split: true`, which returns
`objectRmse`, `backgroundRmse` and `objectPixelFraction`: the same
RMSE computed separately over the pixels your object covers and
everything else, from an objectmap mask of your OWN candidate.

**Use it sparingly -- at most once, at a genuine decision point.** It
costs an EXTRA render every time, and on this task your tool/LLM
budget, not your information, is the binding constraint. That is
measured, not cautionary: an earlier version of this skill told you to
pass it from your third compare onward, and in a controlled A/B that
mandate changed the final RMSE by -0.0004 (95% CI -0.05..+0.05, i.e.
nothing at all) while pushing 5 of 6 runs into budget exhaustion,
versus 1 of 6 without it. Runs that skipped it had budget left to
finish deliberately. So: if you already know what to fix next, just
fix it. Reach for the split only when you would otherwise burn
iterations guessing.

When you do use it, it beats the old rule of thumb that a plateau
above ~0.1 means the staging is wrong -- that guess is often wrong,
and this is a measurement.

**If you use it, pass `splitObjects` -- do not skip it.** Unscoped,
"OBJECT" means EVERY registered object, and your ground plane and
backdrop ARE registered objects, so they land in the OBJECT bucket and
"background" shrinks to just the sky. Measured on real runs of this
task, that put `objectPixelFraction` at ~0.86 and made the split
report geometry-vs-environment rather than object-vs-staging -- the
reading below would then be nonsense. Naming your hero object (the
`standard_object` name you gave it, e.g. `obj_subject`) puts the
ground, the backdrop and the sky all in BACKGROUND, which is the
split you actually want. Sanity-check `objectPixelFraction`: it
should be roughly the share of frame your subject covers. If it is
near 1.0 you almost certainly forgot to scope. Then act, decisively:

- **`backgroundRmse` low, `objectRmse` high** -> the stage is DONE.
  Stop touching the environment, ground and lights entirely. Spend
  every remaining iteration on the object's silhouette and
  proportions.
- **`backgroundRmse` still high** -> staging is your biggest lever
  regardless of how the object looks. Keep working section 6 and do
  not touch the object's shape or materials yet.
- **Both high** -> staging first (section 6). A wrong environment
  changes how the object is lit, so fixing the object against wrong
  lighting is work you will redo.

One safety rule before you act on either figure: **check it is
`>= 0`.** Each sentinels to `-1` when its bucket is empty --
`objectRmse` when no object is visible (camera aimed away, object
off-frame, or you named an object that does not exist -- READ the
`note`, which tells these apart and lists the names actually
available), `backgroundRmse` when your objects fill the entire frame.
`-1` means "not measured", NOT "zero error". Reading a `-1`
`backgroundRmse` as "staging is done" would send you off tuning the
object having never checked the staging at all. On a `-1`, fix the
framing and re-compare before drawing any conclusion.

### 6. Match order: silhouette -> proportions -> surface -> materials -> lighting -> environment

**Stage before object: environment first.** In a photo reconstruction
the background/environment fills most of the frame's PIXELS -- the
fastest RMSE reduction is almost always staging, not the object. Work
in this order before you spend real effort on the hero:

1. **Environment/backdrop tint** -- the single biggest pixel-count
   lever in the frame.
2. **Ground tone** -- usually the second-biggest flat region in frame.
3. **Key-light direction and colour**, read from the shadows (section
   8 below has the how-to).
4. **THEN** object silhouette/proportions.
5. **Materials.**
6. **Fine shape detail.**

An object-fixated session that nails the hero on a wrong stage scores
WORSE than an empty correct stage -- this is measured fact from the
July-2026 baseline. Only once the stage (steps 1-3 above) reads
correctly does the per-object match order below (silhouette ->
proportions -> surface -> materials -> lighting -> environment) take
over for the object itself: that finer-grained order governs HOW to
build the object once you've started on it, not WHETHER to start on it
before the stage is right.

Do not add surface detail, bevels, or secondary geometry until the
blockout's silhouette agrees with the reference from every view in the
multi-view loop above -- refining a proportion that's about to change
wastes the detail work (the object-modeling-recipes blockout->refine
rule, applied per-view instead of per-object).  The full order, and
why each stage waits for the last:

1. **Silhouette** -- outline, at draft quality, from every
   verification view.  Nothing else matters until this holds.
2. **Proportions** -- relative part sizes/placement, once the outline
   holds.
3. **Surface** -- blend radii (`smin` on `sdf_geometry`), bevels,
   fillets -- only once shape is stable, since a later proportion fix
   moves the surface you just tuned.
4. **Materials** -- albedo/roughness per region -- only once shape is
   fully stable, or you'll re-match color under a silhouette that's
   about to move.
5. **Lighting** -- match the reference's key direction/softness (see
   below) -- only after materials are plausible, so a wrong albedo and
   a wrong light don't both read as "the color's off" at once.
6. **Environment** -- background/environment tint -- last, the least
   pin-downable from a photo, and the most likely to be re-tuned after
   everything else is locked.

A shape edit invalidates a blend-radius tune; a material edit
invalidates a lighting match.  Fixing a later stage while an earlier
one still hasn't settled wastes the work.

### 7. Material matching from captures

Start with `uniformcolor_painter` sampling the DOMINANT color per
region the user describes (or you can see on the reference plane) --
get albedo/lightness roughly right before reaching for a texture map.
Only promote to a real `png_painter` texture once (a) the user has
supplied an actual capture file AND (b) uniform color has proven
insufficient (the object has printed detail, wood grain, a label) --
see materials-and-media-basics for the painter-vs-scalar routing rules
once you're picking real material parameters.

### 8. Reading lighting from a photo

Once shape and materials are stable (match-order step 5), read the
reference photo's lighting instead of guessing a `directional_light`
from scratch:

- **Shadow direction** on the ground names the key light's azimuth --
  the shadow falls OPPOSITE the light (remember `direction` is FROM
  the surface TO the light, SCENE_CONVENTIONS.md, so the light chunk's
  vector points the opposite way from the shadow you see in the
  photo).
- **Shadow softness** names the light's angular size -- a hard, crisp
  edge means a small/distant source; a soft, wide penumbra means a
  large or close area light.
- **Warm-vs-cool split** between the lit and shadowed sides names the
  key color vs the fill/environment color -- warm-lit/cool-shadow
  usually means a warm key (sun, tungsten) against a cool sky fill, and
  vice versa for an overcast or studio-strobe reference.
- **A visible ground contact shadow** anchors the object to the stage
  -- if the render's object looks pasted-on, either the key is too
  ambient (soften it toward one dominant direction) or the object's
  `position` floats above the floor plane instead of resting on it.

### 9. Iteration budgeting + stopping rule

A healthy iteration is about 3-4 tool calls: edit the scene, draft
render (one or more verification-loop poses), `read_image`.  Rough
budget across a reconstruction task: blockout 2-3 iterations, shape
refinement 3-4, materials/lighting 2-3, final verification 1-2.

**When you are working under a hard external budget** (a stated round
or tool-call cap), treat it as load-bearing: (a) COUNT rounds as you
spend them; (b) BATCH aggressively -- several `insert_chunk`/
`propose_patch` calls can go in ONE assistant round, and one round can
render multiple verification poses back-to-back, so a blockout that
would naively take six rounds fits in two; (c) reserve the LAST few
rounds, no matter what state the reconstruction is in, for one final
verification render and your finished summary.  Running out of budget
mid-iteration without delivering a final answer scores as a total
failure even when the scene itself is close -- an honest "here is
where it stands and what I am least sure of" final message always
beats being cut off.  STOP
refining a stage once consecutive iterations stop changing the
multi-view assessment -- chasing sub-noise differences burns iteration
budget for no payoff the user can see.  Before calling the task
finished: confirm the scene's `pinhole_camera` is still the original
one (the verification loop's `camera` overrides never touch it, but
confirm rather than assume), do ONE production-quality render (no
`quality` override) at the reference pose -- the only render whose
materials/lighting judgment is honest (observe-modes) -- and state
which aspects of the reconstruction you are LEAST confident about
(back-side shape inferred from a single photo, absolute scale without
a stated dimension, a material guessed from a verbal description)
rather than presenting the result as more certain than the input
photos support.

## When to use which geometry

Cross-reference object-modeling-recipes for the full vocabulary and
the mug/table/lamp recipes.  In short: analytic primitives
(sphere/box/cylinder/torus/ellipsoid) for anything whose silhouette a
primitive already matches; `csg_object` for anything with a hole,
cutout, or subtracted cavity (a mug's hollow, a drilled bracket);
`sdf_geometry` for a taper or fillet no analytic primitive provides
(a lampshade, a rounded bezel); mesh import only once the shape is
confirmed at the blockout stage and needs authored detail beyond what
recipes can reach.

An organic single-form object that photographs as ONE blended mass
(most hand tools, produce, soft-bodied objects) calls for
`sdf_geometry` with generous `smin` blends (k 0.1-0.2) rather than
stacking discrete primitives with a hard union -- a visible crease
where two parts meet means k is too small, not that a different
primitive is needed.  Asymmetric features (notches, cutouts) are
`subtract` parts on the SDF; place them by checking MORE than one
verification view (section 5 above) -- a notch positioned from the
front reference view alone can match that view perfectly and still
sit on the wrong side of the object, invisible until an offset render
shows the error.

## Limits (read this before setting expectations with the user)

- **This is artist-style reference-based modeling, not photogrammetry.**
  There is no mesh reconstruction from photos, no structure-from-motion,
  no automatic silhouette-to-mesh pipeline.  Every shape decision is a
  human (or agent, from a human's description) choosing a primitive/
  CSG/SDF chunk and a size.
- **No image import into a mesh.**  A capture can only appear IN a
  render as a flat texture on a plane (the reference-plane technique)
  -- it can never become geometry by itself.
- **Proportions and colors are only as good as the attached photo's
  resolution/framing or the user's description.**  Without a scale
  reference in the original capture, absolute size is still a guess
  pinned to whatever the user states -- attaching the photo doesn't by
  itself give the agent a metric scale.
- **Attached images are downscaled (long edge 1024px) before the agent
  sees them** -- fine detail below that resolution (fine text, hairline
  seams) may not be legible even though the agent can "see" the photo
  in the broad-strokes sense this skill relies on.
- **Only a bounded number of reference images stay live at once**
  (the cap noted in the capability check above) -- a long multi-object
  or multi-session modeling task may need the user to re-attach an
  earlier photo once it's elided.  There is still **no arbitrary
  file-read verb** -- the agent can only see images the user explicitly
  attaches in chat, or (separately) the renderer's own output via
  `read_image`; it still cannot browse the user's filesystem for a
  photo on its own.
