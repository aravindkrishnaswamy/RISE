# Modeling From Image Captures
> hook: Read when a user wants to reconstruct an object from photos/captures they took -- what image input actually works today, and the reference-plane + canonical-view-matching workflow.

## Honest capability check (read this before promising anything)

As of this writing, **a user cannot attach an image to a chat message**,
and **the agent cannot read an arbitrary image file from disk**.  Verify
this hasn't changed before relying on it, but do not invent a path that
doesn't exist:

- User messages are plain text end to end (the chat panel sends a
  string; the wire format wraps it in exactly one text content block).
  Images only ever appear as a TOOL RESULT -- specifically `read_image`,
  which returns the last render's cached PNG, not anything the user
  supplied.
- There is no `read_file` verb or equivalent in the agent RPC surface.
  The only file paths the agent can point at are scene-language `file`
  parameters resolved by the RENDERER when a scene derives (e.g. a
  `png_painter`'s `file`) -- the agent never sees those bytes directly,
  it only sees the rendered result.

**What this means in practice:** the agent cannot look at the user's
photos.  The workflow below is the honest one that works TODAY --
reference captures live as texture files in the scene directory and get
displayed IN THE RENDER via `png_painter`, so the user (who CAN see
rendered images through the normal render/read_image loop) does the
visual comparison, describing back to the agent what doesn't match.
The agent's job is to ask good structured questions, place geometry
based on the answers, and set up the comparison rendering -- not to
directly perceive the photo.

If the user has NOT placed an image file on disk where the scene can
reference it, skip the reference-plane technique entirely and rely on
the description-based flow (below) -- don't block on an image path
that isn't there.

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

### 2. Structured questions when there's no way to pull the image in

Since the agent cannot see the photo, ask per-view questions that map
directly onto scene-authoring decisions:

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

### 3. The reference-plane technique (when a capture file IS on disk)

If the user has placed a photo file in the scene's media path, put it
on a bounded quad NEXT TO the blockout using `png_painter` +
`clippedplane_geometry` -- now the reference and the work-in-progress
render in the SAME frame, and the user (who can see the render via
`read_image`) can compare them directly instead of tabbing between
windows:

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
# path; this snippet uses a texture already shipped in the repo so it
# is self-contained.
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
path does -- first as literally given (so a path relative to the
process's working directory, or an absolute path, needs no extra
configuration), then through any configured media-path search roots.
It does NOT require `RISE_MEDIA_PATH` to be set if the given path
already resolves as-is.

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
- **Proportions and colors are only as good as the description or the
  reference-plane comparison.**  Without a scale reference in the
  original capture, absolute size is a guess pinned to whatever the
  user states.
- **User image attachment and arbitrary file reads are not implemented
  today** (see the capability check above) -- if this changes, this
  section is the first thing to revisit; until then, don't tell a user
  the agent can "look at" their photo directly.
