# GuillocheWatch — Agent Implementation Notes

Hard-won context for making changes to this scene effectively. Read this before
editing `watch_dial.RISEscene` or the generators. User-facing "how to render" is
in [README.md](README.md); the feature writeup is
[docs/THIN_FILM_INTERFERENCE.md](../../../docs/THIN_FILM_INTERFERENCE.md).

## Mental model (one paragraph)

A guilloché-dial titanium watch, dial-up on a polished dark tabletop, blue
leather strap. The dial's gold→magenta→blue iridescence is **real thin-film
interference** (TiO₂-on-Ti Fresnel via `ggx_material fresnel_mode thinfilm`),
**not** a texture — a per-pixel oxide-thickness map sets the film thickness in
nm. Spectral render (`pathtracing_spectral_rasterizer`, 16 hero wavelengths),
OIDN-denoised. Everything is in this folder; asset `file` paths in the scene are
repo-root-relative (resolved via `RISE_MEDIA_PATH`, which must = repo root).

## World layout & units (memorize this — it drives camera + edits)

- **`scene_unit 0.00079167`** metres/unit ⇒ **1 unit = 0.79167 mm**, i.e. mm→units = `mm / 0.79167`. (38 mm case ÷ 48 units.) `thinlens_camera` reconciles its mm focal/sensor against this.
- **Dial faces +Z (up).** Watch sits on the surface at **z = −10.30**.
- **Crown at +X (3 o'clock). Straps at ±Y: 12 o'clock = +Y, 6 o'clock = −Y.** 9 o'clock = −X.
- Real-watch dims baked in: case ⌀38 mm (≈48 u), thickness 10.9 mm, lug-to-lug 44.5 mm (≈56 u), lug width 20 mm.
- `crystalgeom` is an **ellipsoid, radii `21.5 21.5 4.5`** = a domed sapphire. **Ellipsoid `radii` are TRUE radii (semi-axes), NOT diameters** — fixed in EllipsoidGeometry 2026-06. So 21.5 = 21.5-u semi-axis = 43-u diameter ≈ the 48-u case. If you see "the crystal needs 43 to fit," you're on the pre-fix mental model — don't double it.

## Camera posing — the big trap of this scene

**RISE's screen basis (verified empirically, trust this over intuition):**
```
forward = normalize(lookat − location);  worldUp = (0,0,1)
screen_right = cross(forward, worldUp)
screen_up    = cross(screen_right, forward)
# project a world direction d onto (screen_right, screen_up):
#   x = d·screen_right  (+ = frame-right),  y = d·screen_up  (+ = frame-up)
```
**Verified at the hero azimuth ψ=315°** (camera `location 130 −130 182.85`, lookat `0 0 −1`, up `0 0 1`): world **+Y (12 o'clock) → NE (upper-right)**, world **+X (crown) → SE (lower-right)**. The watch reads **naturally** (12→3 clockwise, crown on the right). It is **NOT mirrored** — if a calculation tells you it's mirrored, your basis sign is flipped.

**To find a new azimuth:** sweep ψ, build the basis above, project +Y and +X. For "12 toward top, crown on the right," **ψ ∈ [285°, 330°]** works; **315° is the sweet spot**. A *literal* 180° orbit from a front view does NOT do what you'd guess — it puts the crown at NE and 12 at the left. Compute, don't eyeball.

**Up-vector rule:** use **`up 0 0 1`** for every non-top-down camera so the +Z-normal floor reads as ground and the watch reflects in it. Use `up 0 1 0` ONLY for the straight-down top view (`cam_topdown`) — there `0 0 1` is degenerate (parallel to view), and elsewhere `0 1 0` renders the floor as a vertical wall (a bug that bit cam_photo and every diagnostic camera originally).

**⚠ CALIBRATION LESSON (cost ~an hour this session):** to locate a small feature (the crown) in a render, **crop the polished feature and look** — do **NOT** make it emissive as a marker. An emissive crown *spills* light onto neighboring surfaces, so the bright blob you see is the spill, on the *opposite* side from the crown. Use `PIL` to crop `rendered/..._denoised.png` around the case; thumbnails are too small to place the crown.

The 7 cameras are a product set (see README table). **`cam_high34` is the ACTIVE camera** — it must stay **last** in the file (RISE activates the last-defined camera, and `renderanimation` drives the active camera). `cam_photo` is the hero STILL (render via `render_watch_views.py --cam cam_photo`).

## Animation (native timeline — NOT a Python script)

The subtle iridescence-reveal animation lives in the scene as native `timeline` keyframes on `cam_high34` + an `animation_options` chunk. Render with `renderanimation <t0> <t1> <frames>` (e.g. `renderanimation 0 1 48`).

- **Why cam_high34 is last/active:** `renderanimation` renders the ACTIVE (last-defined) camera and applies timelines to whatever element they name — so the animated camera MUST be active. That's why cam_high34 was moved last (displacing cam_photo as the bare-`render` default).
- **Orbit = keyframed `azimuth` (5 eased angle keyframes); dolly = keyframed `lookat` (3 pts); hermite.** The camera exposes keyframeable scalar **orbit-angle** params (added to `CameraCommon::KeyframeFromParameters`, in DEGREES): `azimuth` / `phi` → spin about world-up (`target_orientation.y`); `theta` / `elevation` → pitch about screen-right (`target_orientation.x`). `azimuth` keyframes rotate the base camera (cam_high34) about the watch's +Z — a 2–5-keyframe turntable, far more readable than world positions. (`location` + `lookat` are ALSO keyframeable for an explicit path.) `AdjustCameraForThetaPhi` applies it: `Rotation(up, phi)` ⇒ phi/.y = azimuth, `Rotation(cross(fwd,up), -theta)` ⇒ theta/.x = elevation.
- **Frame numbering needs `multiple TRUE`** on `file_rasterizeroutput` — without it every frame overwrites one file. With it, output is `<pattern>NNNN.png` (+ `<pattern>_denoisedNNNN.png`, 4-digit); a single `render` also gets a `0000` suffix, so `render_watch_views.py` overrides `multiple FALSE` to keep stills clean.
- **⚠ Camera-stripping tools must ALSO strip `timeline` + `animation_options`.** `render_watch_views.py` removes all camera chunks to make exactly one active; if it leaves a `timeline` whose target (`cam_high34`) was stripped, that chunk fails to load and the whole parse aborts (→ "Scene contains no camera"). Its `DROP_CHUNKS` set handles this — replicate it in any new scene-rewriting tool.
- **Tune the move:** edit the `timeline` keyframe values (the `azimuth` timeline = orbit angle about +Z; the `lookat` timeline = dolly truck). Full-360 turntable = `azimuth` 0→360 (or ±180). RISE writes PNG frames, not a movie — assemble with ffmpeg (see README).

## Lighting — the specular-dial vs diffuse-strap dynamic-range tension

Two softbox area lights (`clippedplane_geometry` + `lambertian_luminaire_material`)
+ the Uffizi HDRI (`radiance_scale 0.18`, `radiance_background FALSE` = lighting/reflection only, invisible).

- **key** `soft_top_lum` scale **45**, panel ≈ 95×72 u.   **fill** `soft_bot_lum` scale **8**, panel ≈ 60×46 u.
- **THE KEY INSIGHT:** the iridescent **specular** dial only needs the panel's *radiance* (it reflects the lobe), but the **diffuse matte strap** integrates the panel's whole *solid angle*. A **large** bright panel over-lights the strap **no matter how much you dim the HDRI**. The fix is to **shrink the panels** (less solid angle ⇒ less diffuse load) while keeping radiance high (dial highlight preserved) — not to dim everything.
- **Softboxes orbit WITH the camera.** Their centres are rotated by the same azimuth offset as the camera so they stay out of frame at any pose. If you re-pose the camera by Δψ, rotate both softbox centres by Δψ about Z (the helper in `render_watch_*` / the session scripts rebuilds the 4 corners target-facing). A plain camera orbit *without* moving the lights puts a softbox in the lens.

## Materials (exact current values)

| Material | Type | Key params | Notes |
|---|---|---|---|
| `tf_dial` | `ggx_material` | `fresnel_mode thinfilm`, `ior Ti_n`/`extinction Ti_k` (substrate), `film_ior TiO2_n`/`film_extinction TiO2_k`, `film_thickness oxide_thk`, `rs pnt_white`, `alpha 0.08` | The iridescent dial. n,k from shared `colors/thinfilm/`. |
| `oxide_thk` | `scalar_painter` | `function2d oxide_fn` (native `guilloche_oxide_painter`), `scale 13 bias 24.5` | **Film thickness in nm** = `bias + scale·dose`. Centre=24.5 nm (gold heart), rim=37.5 nm (violet/blue). **MUST be `scalar_painter` (IScalarPainter), never a colour painter** — film thickness is a physical scalar; a colour painter would JH-uplift it and mangle the nm. |
| `strap_leather` | `ggx_material` | `fresnel_mode schlick_f0`, `rd pnt_strap_blue (0.04 0.08 0.22)`, `rs pnt_black` (F0=0 ⇒ pure matte), `alpha 0.5` | Deep royal-blue suede. |
| `surface_dark` | `ggx_material` | `schlick_f0`, `rd pnt_black`, `rs 0.05`, `alpha 0.10` | Polished tabletop; `alpha 0.10` gives a soft watch reflection. |
| sapphire crystal | `dielectric` | Sellmeier + `ar_film_ior 1.38 ar_film_thickness 99.6` | Data-based AR coating (low reflection). |

**Heat-tint tuning (all in-scene):** change `oxide_thk` `bias` (torch START nm) and
`scale` (SPAN nm) — presets are in comments above the chunk (e.g. `bias 22 scale 16`
= gold→blue vivid; active is `bias 24.5 scale 13`). The **radial falloff** (how fast
it heats outward) is the `falloff` parameter on the `guilloche_oxide_painter` chunks
(`quadratic|smooth|linear`).

**⚠ STRAP PALE-BLUE TRAP:** if the strap looks washed-out/pale, it's almost always a
**too-shallow, desaturated albedo**, NOT over-exposure. Diffuse colour = albedo × light;
a blue with max channel ~0.4 + non-trivial green reads as periwinkle. **Test:** set the
albedo to pure red `(1,0,0)` — if the strap turns vivid red, albedo drives it (deepen +
saturate the blue), not the lights. Deep royal blue = `(0.04, 0.08, 0.22)`.

## Mesh / texture pipeline (NATIVE since 2026-06)

- **Nothing is pre-baked.** The dial meshes are `guilloche_dial_geometry` chunks, the oxide doses `guilloche_oxide_painter` functions, the strap + stitches `swept_band_geometry` chunks — all evaluated at scene-parse time in C++ (`src/Library/Painters/GuillocheField.h`, the factories in `src/Library/RISE_API.cpp`).
- The Python bakers (`dial_mesh_gen.py`, `dial_variants_gen.py`, `thermal_oxide_sim.py`, `strap_mesh_gen.py`) **stay in this folder as the golden reference implementations** — `tests/GuillocheFieldTest.cpp` + `tests/ProceduralMeshTest.cpp` pin the C++ to them (heights, oxide doses, torch masks, mesh vertices/normals/uv/triangles). Change the C++ and the Python together, regenerate goldens from the Python.
- **Do NOT go back to a polar mesh for the dial** — the polar parameterization had a center-wash singularity (the whole reason for the Cartesian layout). `guilloche_gen.py` is the earlier *polar* generator, kept for reference only.
- The dial's UV is the linear Cartesian map u=(x+R)/2R — every oxide painter / palette / metal applies to every dial pattern.

## Paths & scripts

- RISE resolves `file` via `GlobalMediaPathLocator` whose search paths are **`RISE_MEDIA_PATH` (repo root)** + the exe dir — **NOT** the scene's dir. So all scene asset paths are **repo-root-relative** (`scenes/FeatureBased/GuillocheWatch/…`). Always `export RISE_MEDIA_PATH="$(pwd)/"` from the repo root.
- Each script derives: `HERE = dirname(__file__)` (this folder; default output) and `ROOT = HERE/../../..` (repo root; for `bin/rise` + `rendered/`). If you move the folder, update those `..` counts.
- Shared assets that **stay outside** this folder: the HDRI `lightprobes/uffizi_probe.hdr` and the thin-film n,k curves `colors/thinfilm/{substrates,oxides}/*.{n,k}`. Don't relocate those into the folder.

## Editing gotchas

- **`.RISEscene` uses HARD TABS.** Multi-line `Edit` `old_string`s copied from `Read` output fail (the tab→space mismatch). Use single-line edits, or a Python script with `count`-asserts (the pattern used throughout this scene's history).
- **Substring-overlap trap:** `position 0 0 -3.5` matches *inside* `26.0 0 -3.5`; `-7.0 25.1` inside `7.0 25.1`. Anchor replacements (regex with a unique prefix) and assert the match count.
- **Chunk parsers are descriptor-driven:** braces on their own lines, unknown params hard-fail. Adding a param means the chunk's `Describe()` must declare it.
- **Render discipline:** RISE pegs all cores — **render sequentially, never two at once**. Committed defaults are `samples 32` / `800×800` (fast; OIDN-clean); the hero used `160` / `1100²` via a temp override or the render scripts' `--samples/--res`. Output → `rendered/watch_dial.png` (+ `_denoised`).

## "I want to change X" quick recipes

- **Dial colour/palette** → `oxide_thk` `bias`/`scale`; radial falloff = the `falloff` param on the `guilloche_oxide_painter` chunks. All in-scene.
- **Switch the base metal (Ti / Nb / Ta / steel)** → set the dial object's `material` to `tf_dial` / `tf_dial_nb` / `tf_dial_ta` / `tf_dial_steel` (GUI object-material dropdown; `ObjectIntrospection` exposes `material` as a rebindable reference). Each material = the metal's substrate n,k + its oxide n,k (TiO₂/Nb₂O₅/Ta₂O₅/Fe₃O₄) + a per-metal oxide-thickness window (`oxide_thk_<metal>` scale/bias). Windows are computed by `ThinFilmSwatchOracle(substrate, oxide)` (parameterized) — run `thermal_oxide_sim.py --metal-ladders`. PHYSICS: the same temper sweep lands at a DIFFERENT nm per metal (oxide indices differ), so Ti's 22–38 nm is NOT reused. The dose SHAPE is ALSO per-metal: each metal's parabolic-oxidation activation energy (`tox.METAL_KINETICS`, representative literature Q) bends the radial curvature (higher Q => rim-loaded; the `metal ti|nb|ta|steel` preset on each `oxide_fn_<metal>` `guilloche_oxide_painter` chunk, or explicit `activation_ea`).  So substrate n,k + oxide n,k + nm window + radial shape ALL differ per metal. n,k live in `colors/thinfilm/{substrates,oxides}/`.
- **Make the lightning stand out in colour (non-uniform torch)** → bind `tf_dial.film_thickness` to `oxide_thk_lightning_hot` / `_cool` (GUI scalar-painter slot dropdown). Those wrap `oxide_fn_lightning_hot/_cool` — `guilloche_oxide_painter` chunks with `torch_amount ±0.40` (the painter's TorchMask is the dial's petal zigzag, aligned with the relief; the model is thermal_oxide_sim.apply_torch_pattern). New torch looks = new painter chunk with a different `torch_amount` / pattern.
- **Switch the colour palette (temper window)** → set `tf_dial.film_thickness` to `oxide_thk` (vivid 22–38), `oxide_thk_warm` (16–30 gold→violet), `oxide_thk_cool` (28–44 violet→cyan), or `oxide_thk_wide` (14–46 full multicolour). Same dose shape (`oxide_fn`), different scale/bias = a different slice of the Ti temper sequence. Tune via scale/bias; the colour at each nm follows the oracle / `--metal-ladders` Ti row.
- **Wider palettes per metal** → every base metal has the same set with PER-METAL windows: `oxide_thk_<metal>_warm` / `_cool` / `_wide` (+ default `oxide_thk_<metal>` = vivid), wrapping that metal's dose function `oxide_fn_<metal>`. Switch via that metal's material film_thickness slot (4 metals × 4 palettes = 16 looks). Windows differ per metal because the colour sequence does (Ta has no blue, etc.) -- pick from each metal's `--metal-ladders` row.
- **Re-pose camera** → compute ψ via the basis above (crown-right ⇒ ψ∈[285,330]); set `cam_photo` location `(R cosψ, R sinψ, Z)` with R≈184, Z≈183; **rotate both softboxes by the same Δψ**; verify by cropping the polished crown.
- **Strap shape** → the repeatable `point <y> <z>` lines on the two `swept_band_geometry` chunks (band + stitches share the path; keep them identical).
- **Strap colour** → `pnt_strap_blue` (keep it deep + saturated + blue-dominant; matte F0=0).
- **Table reflection sharpness** → `surface_dark` `alphax/alphay` (0.10 = soft; lower = mirror-like).
- **New camera angle** → add a `pinhole_camera`/`thinlens_camera` chunk BEFORE `cam_high34` (which must stay last = active), `up 0 0 1` unless top-down; `render_watch_views.py` auto-discovers it.
- **Re-time / re-shape the animation** → edit the `timeline` keyframes (see the Animation section); `renderanimation 0 1 <frames>`.

## Dial-geometry experiments (dial_variants_gen.py + gen_dials.sh)

Flexible alternate-geometry generator (stock `dial_mesh_gen.py` unchanged). Reuses
its primitives + `build_mesh(p, height_fn=...)` + writer, composable height FIELDS.
Shipped library (`--field` / `dialmesh_<name>`):
- `lightning` — **the hero pattern.** 11 zigzag bolts of a tight CUBE (the raised
  broad zigzag arms) on a uniform RUNG-block ground (the channels between).
  angle-only `cos(N*theta)` ray mask + mask-only triangle-wave zigzag (weave stays
  radial → reads as bolts, not a pinwheel); `select` mode = fine `_cube` inside the
  arms, fixed-size `rung` grid between. Knobs: `--num-arms`, `--lightning-lo/-hi`
  (arm width), `--zigzag-amp/-freq`, `--field-cell` (arm cube), `--bolt-style
  {rung,cube,woven}` + `--rung-len/-width` (channel blocks), `--field-frame`.
- `radial` — earlier swirled-petal lightning, coarser bolt cell (the former
  `lightning_cell`). `--lightning-cell-scale`, `--lightning-lo/-hi`, `--cell-mode`.
- `iris` — 007 camera aperture: N blade edges (lines tangent to a central circle,
  rotated) pinwheeling around a hole; cube-filled blades. `--num-arms` (blades),
  `--iris-aperture`, `--iris-swirl`, `--iris-edge`.
- `swirl` — log-spiral guilloché. `--num-arms`, `--swirl-turns`.
- `varwidth` — alternating fine/coarse sunburst sectors. `--num-arms`, `--lightning-cell-scale`.
- `uniform` — stock single-cell baseline.

- **Regenerate every mesh:** `./gen_dials.sh` — the `dial*.raw2` are gitignored;
  that script is the source of truth for each pattern's params (never commit a mesh).
  Add an experiment = `field_<name>` + register in `FIELDS` + a `gen_dials.sh` line +
  a `dialmesh_<name>` scene chunk.
- Same Cartesian UV as the stock dial → all oxide maps / palettes / metals apply.
- **Swap live (scene/GUI):** set the `dial` object's `geometry` to any
  `dialmesh_<name>`. `geometry` is a LIVE rebind (ObjectIntrospection editable
  Reference → `SceneEdit::SetObjectGeometry`): mesh swaps, top-level BVH rebuilds
  next render, no reload.
- **NAMING (2026-06):** today's `lightning` is the former `lightning_radial`; the
  former `lightning_cell` is now `radial`.

## Contact sheet (render_contact_sheet.py)

Renders the metal × palette grid → `rendered/grid_<metal>_<range>.png` + assembles
a ~4K labelled PNG (`rendered/metal_palette_contactsheet.png`). 960 px/cell × 4
cols = 3840 (4K width), native; big PNGs gitignored (the script is the recipe).
Per cell: dial `material` → `tf_dial_<metal>`, that material's `film_thickness` →
`oxide_thk[_<metal>][_<range>]`, `multiple FALSE`, timelines stripped (static
cam_high34). `--ranges warm vivid cool wide` for 4×4; `--res/--samples` for speed.
