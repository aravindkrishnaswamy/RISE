# Scene Skeleton and Conventions
> hook: Read before authoring a scene from scratch, or when a scene renders dark, backwards, or fails to parse.

## The minimal complete scene

A `.RISEscene` is a sequence of chunks.  The header line must be exactly
`RISE ASCII SCENE 7`, and every chunk brace goes on its OWN line.
Entities are referenced BY NAME, and a reference must be declared
EARLIER in the file than its consumer (painter before material,
geometry/material before object).

```rise
RISE ASCII SCENE 7

# Integrator: unidirectional path tracing (the default choice).
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

# The film owns the raster dims; cameras are imaging-only (no width/height).
film
{
	width	128
	height	128
}

# Right-handed, Y-up.  A camera at +Z looks down -Z at the origin.
pinhole_camera
{
	location	0 0 5
	lookat		0 0 0
	up			0 1 0
	fov			40.0
}

# A painter supplies color; the material references it by name.
uniformcolor_painter
{
	name	pnt_white
	color	0.8 0.8 0.8
}

lambertian_material
{
	name		mat_white
	reflectance	pnt_white
}

sphere_geometry
{
	name	sph
	radius	1.0
}

standard_object
{
	name		obj_sphere
	geometry	sph
	material	mat_white
	position	0 0 0
}

# direction points FROM the surface TO the light (see trap 1 below):
# positive Z lights the side the camera at +Z sees.
directional_light
{
	name		key
	power		3.14
	color		1 1 1
	direction	0.3 0.5 0.8
}
```

## Convention traps (each has caused real bugs)

1. **`directional_light.direction` is FROM-surface-TO-light**, not the
   shine direction.  A surface is lit when `N . direction > 0`.  With
   the camera at `+Z` looking at the origin, a light needs **positive
   Z** in `direction` to light what the camera sees.  Importers from
   shine-direction conventions (glTF, Unity, Unreal) must NEGATE first.
2. **`power` multiplies `color`.**  Directional/ambient: radiance =
   `color * power` (no falloff).  `omni_light` / `spot_light`:
   `color * power / r^2` — distant point lights need large powers
   (hundreds+).  `power 3.14` (pi) on a directional key makes a fully
   lit white Lambertian surface return ~1.
3. **Colorspace defaults.**  `uniformcolor_painter` treats `color` as
   already-linear Rec.709 (`colorspace Rec709RGB_Linear`).  Add
   `colorspace sRGB` when the value came from a color picker / hex code
   so it is gamma-decoded.  Never use sRGB for normal maps.
4. **`standard_object` transform precedence: `matrix` > `quaternion` >
   `orientation`.**  A full `matrix` (16 doubles, column-major)
   supersedes everything; `quaternion` (xyzw) beats the Euler
   `orientation` (degrees) and still composes with `position`/`scale`.
5. **v7 format rules.**  Header `RISE ASCII SCENE 7`; braces on their
   own lines; one `param value...` per line; `#` starts a line comment.
   Older scenes: `python tools/migrate_scenes_v5_to_v7.py <path>`.

When a scene renders unexpectedly dark, FIRST swap the materials for a
Lambertian white and re-check the light directions before touching
anything else.

**Output note**: the agent surface renders in-memory, but a saved
scene run via the CLI writes NO image unless the scene carries a
`file_rasterizeroutput` chunk — see docs/SCENE_CONVENTIONS.md §10 for
the standard dual EXR (HDR archive) + PNG (display preview) idiom.
