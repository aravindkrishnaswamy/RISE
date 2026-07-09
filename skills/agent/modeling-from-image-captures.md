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

### 5. Coarse-to-fine, matching 2-3 views before ANY detail pass

Do not add surface detail, bevels, or secondary geometry until the
blockout's silhouette agrees with the reference from every view you
have.  Refining a proportion that is about to change wastes the detail
work.  This is the object-modeling-recipes blockout->refine rule,
applied per-view instead of per-object.

### 6. Material matching from captures

Start with `uniformcolor_painter` sampling the DOMINANT color per
region the user describes (or you can see on the reference plane) --
get albedo/lightness roughly right before reaching for a texture map.
Only promote to a real `png_painter` texture once (a) the user has
supplied an actual capture file AND (b) uniform color has proven
insufficient (the object has printed detail, wood grain, a label) --
see materials-and-media-basics for the painter-vs-scalar routing rules
once you're picking real material parameters.

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
