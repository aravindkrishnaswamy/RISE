# Simulation showcase scenes

2026-11-27. Branch `simulation-liquid` (scenes authored at `cc071c604`, proof bakes with the binaries of `cc071c604`
and of `1b4f924db`, the liquid performance pass, whose non-fire output bytes are identical). Solver versions:
`liquid.mac` m7m.1, `cloth.shell` m3a.8, `rigid.bodies` m6.2, `fire.methane` version 3 (being re-baselined to 4).

Six scenes in `scenes/Showcase/Simulation/`, each with one idea, that together exercise every solver and every
coupling the engine has. This round is **proof quality**: coarse grids, small cloth meshes, 640 px renders with
denoising. The final bakes and renders are a later round (parameters in each section and in the table at the end).

**Where they live, and why not `FeatureBased/`.** `scenes/README.md` puts showcase scenes in `FeatureBased/`, but
`run_scenes.sh` renders every scene under `FeatureBased/` with a plain `render`, and these scenes need a bake first
(unbaked, their simulated meshes are empty and the fire medium is empty) and a fire render needs its EXR output
first. So they get their own folder, `scenes/Showcase/Simulation/`, driven by `scripts/simulation_showcase.sh`.

| Scene | Idea | Solvers | Coupling | Status |
|---|---|---|---|---|
| `unveiling` | silk drawn off a crystal orb | cloth | cloth with analytic colliders, a keyframed pin, self-collision | proof done |
| `conjurer` | two props tossed up into a silk canopy | cloth + rigid | implicit cloth-rigid (mesh colliders, contact reaction) | proof done |
| `tide` | a wave takes apart a tower of toy blocks | liquid + rigid | owned liquid-rigid, body-body stacking and collisions | proof done |
| `honey` | a pillar of honey slumps onto stretched linen | viscous liquid + cloth | owned implicit sheet with viscosity | proof done |
| `monsoon` | rain fills an awning that spills into a basin with floats | liquid + cloth + rigid | one group: implicit sheet and owned bodies | proof done |
| `hearth` | a blue methane flame, a pennant in its updraft | fire + cloth | one-way fire to cloth (aerodynamic drag) | authored, not baked (fire reserved) |

Every solver: cloth (`unveiling`, `conjurer`, `honey`, `monsoon`, `hearth`), rigid (`conjurer`, `tide`, `monsoon`),
liquid (`tide`, `honey`, `monsoon`), fire (`hearth`). Every coupling: cloth-rigid (`conjurer`), liquid-rigid (`tide`),
liquid-cloth with viscosity (`honey`), liquid-cloth-rigid in one group (`monsoon`), fire-to-cloth one way (`hearth`).

## How to bake and render

```sh
export RISE_SIMULATION_CACHE_DIR=/path/to/private/cache    # optional; default: the per-user cache folder
export RISE_BIN=/path/to/copy/of/rise RISE_SIM_BIN=/path/to/copy/of/rise_sim   # optional; default bin/
scripts/simulation_showcase.sh proof tide               # bake, then render the hero frame and the others
scripts/simulation_showcase.sh final tide bake          # bake only
scripts/simulation_showcase.sh final tide render        # the hero frame at final quality
scripts/simulation_showcase.sh final tide render animation   # every frame
scripts/simulation_showcase.sh proof all
```

- **Proof and final values in one file.** A scene line written `<key> <final value>   # proof <proof value>` holds the
  final setting; `proof` mode writes a copy with the proof value substituted. Grid resolution, cloth vertex counts,
  the fire tier, film size, samples and the output folder are switched this way.
- **One copy per mode, baked and rendered from the same path:**
  `rendered/showcase/<scene>/<scene>.<mode>.RISEscene` (git-ignored). The default cache folder of a simulation is keyed
  by the scene file's path, so the bake and the render must read the same file.
- **What is baked:** every `simulation_group` and every producer that is no group's member (`awk` over the copy).
  Logs: `rendered/showcase/<scene>/<mode>/bake_<target>.log` (with `/usr/bin/time -l`), `status.txt`, and one
  `render_<frame>.log` per frame. Frames: `rendered/showcase/<scene>/<mode>/<scene>[_denoised]NNNN.png`.
- **Hero and other frames** come from each scene's header line `# showcase: hero <n> frames <a b c ...>`.
- Fire: never run two fire bakes at once (`rise_sim --gpu-lock` exists for that); the fire scene's primary output is
  a linear 32-bit EXR, with a PNG beside it.

## unveiling (cloth)

**Idea.** An unveiling. Crimson silk hangs from a brass rod just behind a black marble plinth and falls over the
crystal orb on it; after 1.8 s the rod rises and draws back, the silk's hem slides over the orb and lifts away, and
the orb, lit from the side, throws its dispersed light on the plinth.

**Demonstrates.** cloth.shell alone: silk-lightweight (0.07 kg/m²), barrier contact with three analytic colliders
(the floor plane, the plinth box, the orb sphere), self-collision, friction, a pinned edge that follows a keyframed
scene object (`pin v0 follow rod`, the rod's `timeline`). Renderer: `weave_material` silk with thin transmission
(the backlit silk glows), a dielectric orb with per-channel IOR (1.50 / 1.53 / 1.57: exaggerated dispersion),
coated black marble, a brass torus collar.

**Proof.** 31 × 31 vertices, 151 frames: **24 s** wall, 0.18 GB RSS; no Newton failures. Frames:
`rendered/showcase/unveiling/proof/unveiling_denoised0060.png` (veiled), `..._0084.png` (hero: the hem crossing the
orb), `..._0096.png`, `..._0150.png` (revealed).

**Final.** 61 × 61 vertices (four times the vertices; the direct solver's cost grows faster than linearly: expect
3-6 min, under 1 GB), 1920 × 1080, 512 spp, frames 0-150 (5 s). Improve: the orb's caustic on the plinth is faint at
proof samples; a stronger, smaller key or a spectral render would show the dispersion.

## conjurer (cloth + rigid)

**Idea.** A conjurer's canopy. A square of midnight-blue silk hangs by its four corners over an empty stage; a
gilded cube and an ivory ball are tossed up into it from below, punch two peaks into the silk, are thrown back down by
it and tumble across the boards while the canopy billows and settles. A hard spotlight, footlights, a cool rim.

**Demonstrates.** cloth.shell and rigid.bodies in one implicitly coupled group (sub-iterated to a tolerance, with
step halving on non-convergence): each prop is a mesh collider of the cloth moved by its body's state
(`rigid_surface`), and the cloth's contact reaction is the bodies' load (`contact_reaction targets=ball,cube`), so
the silk is lifted by the props and throws them back. Bodies made from scene meshes (`body_object`) with velocity and
spin; silk with self-collision. Renderer: translucent silk weave, a gold conductor-like GGX (Schlick), coated ivory.

**Limits designed around (design history).** (1) cloth-rigid coupling needs each body's surface to have the collider
mesh's topology, so the props are mesh bodies, and mesh bodies have no body-body contact (`contact off`): the props
are tossed apart and never meet (Engine requests, 1). (2) The first design dropped a *free* silk square through which
the props were tossed, so it fell over them and they landed on it: five attempts failed in the cloth's Newton solve
when the silk was pinched between a landing prop and the stage (the first: "the prescribed motion is infeasible ...
pinched between mesh collider 1 and analytic collider 0"; then, with the stage's cloth plane 6 and 15 mm below the
props' plane, more substeps and Newton iterations, a heavier silk, a looser tolerance: "cloth Newton did not reach its
tolerance" at t = 0.93-1.27 s, residuals 0.0025-0.57 m/s²). The airborne part rendered well
(`rendered/showcase/conjurer/` frames of that design were replaced). The canopy keeps the silk above the props, so
nothing is ever pinched against the stage. Worth a cloth-solver look: a light silk pinched between a resting mesh
collider and a plane.

**Proof.** Silk 31 × 31 vertices, 91 frames: **188 s** wall, 0.2 GB RSS; Newton converged at every step (no
halvings, none unconverged). Frames: `rendered/showcase/conjurer/proof/conjurer_denoised0006.png`,
**`..._0011.png` (hero: the two props in the silk's peaks)**, `..._0016.png`, `..._0024.png` (thrown back down),
`..._0090.png`.

**Final.** Silk 49 × 49 (2.5 times the vertices; the implicit coupling's sub-iterations dominate): **10-20 min**,
under 1 GB. 1920 × 1080, 512 spp, frames 0-90 (3 s). Improve: the stage reads dark outside the spot; the silk could
be larger and the toss timed so both props peak together.

## tide (liquid + rigid)

**Idea.** A child's tower of painted wooden blocks stands on the dry sand floor of a glass aquarium; a dam of green
water is released behind it with a toy boat riding on top. The wave lifts the blocks off one another, the tower comes
apart, and blocks and boat float, roll and knock together while the water sloshes. Backlit through the glass.

**Demonstrates.** rigid.bodies (six stacked bodies of three shapes plus the boat, body-body contact and stacking, the
tank as five planes) and liquid.mac in one group: the liquid owns the exchange and solves the bodies' six degrees of
freedom in its pressure solve (buoyancy, the wave's push, wakes); rigid.bodies owns every contact and gives the
liquid's copies the same contact inside the macro step (`body_contact`). Renderer: tinted water (green-teal `tau`),
6 mm glass panes, a luminous backdrop, a softbox for caustics on the sand.

**Design history.** The first version was a stone cairn (2,500-2,700 kg/m³); a 30 cm dam break does not topple
dense stones of that size (it only slid the second stone), and its splash reached the domain's ceiling. Wooden blocks
(650 kg/m³) are lifted by the water, which reads at once; the dam was lowered to 0.245 m and the domain raised to
0.6 m so the run-up stays inside.

**Proof.** 64 cells along x (1.25 cm), 121 frames: **177 s** wall (binaries of `cc071c604`), 1.19 GB RSS. Progress
detail: no clamped faces, no warning. Frames: `rendered/showcase/tide/proof/tide_denoised0000.png` (the tower),
`..._0012.png` (the wave arrives), **`..._0027.png` (hero: the tower breaking as the wave runs up the far wall)**,
`..._0045.png`, `..._0090.png`.

**Final.** 128 cells (6.25 mm): about 16 times the proof's work (8 times the cells, twice the steps), with the new
binaries' 1.4-2.5 times: **25-35 min**, about 4-6 GB RSS (extrapolated, not measured). 1920 × 1080, 512 spp, frames
0-120 (4 s). Improve: the far-wall splash is the only fine spray and it is coarse at 64 cells; the glass panes'
edges are visible at the frame's right.

## honey (viscous liquid + cloth)

**Idea.** A pillar of honey stands on a linen napkin stretched in a square walnut frame and is let go: it slumps
under its own weight like a melting candle into a bell, then spreads into a slow amber pool that weighs the linen
down. Low warm light from behind and above, so the honey glows.

**Demonstrates.** liquid.mac with viscosity (10 Pa s, 1420 kg/m³; implicit, the strain-rate form) and an implicitly
coupled cloth sheet (m7m: the linen is a no-slip wall of the viscous solve and its vertex velocities are unknowns of
the viscous step; the drag reaches the cloth through `surface_load_steps`). Renderer: honey as a strongly absorbing
dielectric (1 cm passes 0.93 / 0.62 / 0.22, IOR 1.49), so thick parts are deep amber and thin ones golden; woven
linen; coated walnut; a glass jar.

**Limits designed around.** The linen is pinned on all four edges (no free edge at a waterline) and weighs
0.5 kg/m² (the M7m honey scene's value; cotton's 0.15 raised warnings); the pool stays several cells deep.

**Proof.** 64 cells (6.25 mm), linen 17 × 17, 151 frames: **525 s** wall (binaries of `1b4f924db`), 0.91 GB RSS.
Progress detail: rotation stretch up to 0.4%, linearisation correction up to 0.44% (no warning), barrier holes 0,
cells wet across a sheet 0. Frames: `rendered/showcase/honey/proof/honey_denoised0002.png`, `..._0004.png`,
**`..._0006.png` (hero: the slumping bell, backlit)**, `..._0012.png`, `..._0060.png`, `..._0150.png`.

**Final.** 128 cells (3.1 mm), linen 33 × 33: the viscous solve grows faster than the pressure solve with the grid
(more CG iterations, and the viscous CFL `dt <= 4 h²/ν` = 5.6 ms at 128 cells now binds against the 8.3 ms macro
step): **1.5-3 h**, about 3-5 GB (extrapolated). 1920 × 1080, 512 spp, frames 0-150. Improve: the pool's rim shows
the 64-cell staircase as terraces; a true pour (a rope of honey coiling) needs an inflow emitter (Engine requests).

## monsoon (liquid + cloth + rigid, one group)

**Idea.** A cloudburst over a courtyard basin. A sheet of rain lands on a striped awning held in a frame tilted over
a raised stone basin; the awning bellies under the weight into a bag, fills, and the water brims over its low edge in
a clear sheet into the basin, where two wooden floats, a painted ball and a toy boat rock in the waves and knock
together.

**Demonstrates.** All three non-fire solvers in **one** `simulation_group` (`courtyard`): liquid.mac owns both
exchanges, the awning is an implicitly coupled sheet (its linearised response solved in the pressure solve) and the
floats are rigid.bodies bodies solved in the same pressure system; rigid.bodies owns their body-body and basin-wall
contact. The awning and the floats meet only through the water (there is no cloth-body contact in a liquid group).
Renderer: a striped `weave_material` driven by an `expression_painter` on the cloth's material UVs (metres), tinted
water, coated tiles.

**Limits designed around (and how the design got there).** The awning is pinned on all four edges (no free edge at
a waterline). The progress detail flagged the rotation stretch above 1% in four of the six attempts; what changed it:

| Attempt | Awning | Rain | Rotation stretch / linearisation | Outcome |
|---|---|---|---|---|
| 1 | cotton 0.5 kg/m², taut, tilt 0.22 | slab 7 cm above | 1.6% / 1.5% (warnings) | no belly: the water ran off as a film thinner than 2.5 cells |
| 2 | cotton 0.8, cut 13% full (`stretch 0.87`) | larger slab | **60% / 1,800%** | the slack sheet buckles: never start a pinned sheet compressed |
| 3 | polyester-knit 0.8 (100-150 N/m), tilt 0.15, macro step 1/240 | | 2.6% / 1.0% | a deep bag that held all the water: no spill |
| 4 | knit stiffened to 600/500 N/m, 1.2 kg/m², tilt 0.25 | | 1.2% / 0.53% | bag, spill and floats as designed |
| 5 | as 4, macro step 1/480 | | 1.3% / 0.57% | the liquid's own CFL steps already ruled: no gain |
| 6 | as 4, 2.0 kg/m² | | 1.4% / 0.78% | weight alone does not help |
| **7** | as 6 | **three boxes stepped 2-6 cm above the tilted awning** (the impact was the kick) | **0.79% / 0.39%** | committed |

**Proof.** 64 cells along x (1.4 cm), awning 21 × 18, 121 frames, macro step 1/240 s: **233 s** wall (binaries of
`1b4f924db`), 0.91 GB RSS. Progress detail: no clamped faces, rotation stretch up to 0.79%, linearisation correction
up to 0.39% (no warning); **cells wet across a sheet 6,272 (8.8 L·step summed, held by the guard), barrier holes 862**
(not understood here: no leak through the awning's interior is visible in any rendered frame; the water leaves only
over the edges; for the liquid owner to read). Frames: `rendered/showcase/monsoon/proof/monsoon_denoised0006.png`
(the rain lands), `..._0018.png`, **`..._0030.png` (hero: the spill over the low edge, the boat and floats rocking)**,
`..._0048.png`, `..._0100.png`.

**Final.** 128 cells (7 mm), awning 41 × 35: about 16 times the proof's liquid work over 1.4-2.5 times faster
binaries plus four times the cloth vertices: **45-90 min**, about 4-6 GB (extrapolated). 1920 × 1080, 512 spp, frames
0-120. Improve: the background (a flat beige ground and a uniform sky) needs an environment map or a courtyard set;
a finer grid will thin the spill into a proper sheet.

## hearth (fire + cloth)

**Idea.** A fire-lit still life. A methane pool fire burns in a ring of firebrick on a slate floor, a column of blue
chemiluminescent light that is the room's key light; a silk pennant hangs from an iron rod beside the plume and
flutters in its updraft; a heavy glass tumbler of water on a low oak stool catches the flame; a faint cool moonlight is
the only other light.

**Demonstrates.** fire.methane (the certified 0.30 m / 33 kW burner, tier 6 proof / tier 8 final, fast mode, window
1.0-4.5 s of fire time) and a **real one-way fire-to-cloth coupling**: the pennant's `surface_load` is fed from the
fire's cache through the `aerodynamic_drag` adapter (gas velocity and a density from the temperature, quadratic drag
on the velocity relative to the cloth). The fire does not heat, scorch or burn the cloth, and the water is static
geometry: nothing else is coupled. Renderer: the spectral path tracer (required, below), the emitting medium as the
key light on silk, glass and water.

**Found while authoring (look development on a partial version-3 cache, render only).** A methane flame from this
solver has no soot (the fuel record's soot yield is zero), so its light is the CH*/C₂*/CO₂* chemiluminescence, and
only `pathtracing_spectral_rasterizer` evaluates that emission; `pathtracing_pel_rasterizer` draws this fire
**black** (peak about 1e-24). The committed `scenes/Tests/Simulation/fire_campfire_scene.RISEscene` uses the Pel
rasterizer, so it would render black too (not changed here; reported). The flame is blue and dim (linear peak about
0.01): the PNG has `exposure 7.5`. The scene is in the fire's own z-up frame (there is no transform on a fire medium:
burner at x = y = 1.06 m, z = 0), and scene t maps to fire time 1.0 + t, frames every 1/16 s.

**Status: authored, not baked.** A tier-6 proof bake of the fire was stopped cleanly at 39 of 57 frames when the GPU
fire solver was reserved for the fire re-baseline (version 3 to 4: that cache is stale anyway). Its partial frames
were used only to light the scene: `rendered/showcase/hearth/lookdev/h_denoised0024.png` (t = 1.5 s, no pennant).
Measured before the stop: 1,066 accepted steps in 575 s (0.54 s per step at tier 6, about 2,800 steps for the
window, **about 25 min**), 1.6 GB RSS. The pennant is not baked (it reads the fire's cache).

**Final.** Tier 8 (69 × 69 × 106, 2.3 times the cells and a smaller step: about 3 times tier 6): **about 75-90 min**
of Metal for the window (extrapolated); pennant 19 × 37, seconds. 1920 × 1080, spectral, 256 spp, 16 wavelengths,
frames 0-56 at 16 fps (the fire's cadence). Risk to check first: how hard the plume's drag throws the silk
(lightweight silk at the plume's edge, 0.25 m off its axis); if it is thrown into the flame, move the rod outward.

## Engine requests

1. **Cloth-rigid coupling for analytic bodies (blocks body-body contact under cloth).** `rigid_surface` requires the
   cloth's `collider_port` mesh to have the rigid body's own surface topology (`RigidBodySurface`: an analytic
   sphere is an icosahedron subdivided once, 42 vertices). No scene geometry tessellates to it: an
   `indexedmesh_geometry` with exactly those vertices and triangles is renumbered by `TessellateToMeshEx`
   (first-seen order) and is refused ("the accepted port is not a mesh on this body's surface (topology)"); adapter
   parameters cannot carry `vertices=`/`triangles=` from a scene (the `key=value` parser makes strings). So a body on
   cloth must be a `body_object` (mesh body), and mesh bodies take no body-body contact. Minimal repro:
   `rigid_simulation { body ball sphere 0.04 ... }` + `cloth_simulation { collider_port ball }` with a
   `sphere_geometry` object `ball` + `connect drop.body_state -> sheet.collider_surface via rigid_surface body=ball`:
   status reports the topology refusal. Request: let `collider_port` take the body's own surface (e.g.
   `collider_port body:<name>`), or body-body contact for mesh bodies (convex hull).
2. **Liquid inflow (an emitter).** A pour, a tap or rain needs liquid added over time (`source box|sphere ...
   velocity v [start s] [end s]`); only initial regions exist, so the honey "rope" is a pillar released at t = 0 and
   the rain a single slab.
3. **A transform for `fire_medium` (or a placed `fire_simulation`).** The fire sits in its own frame (z up, burner at
   1.06, 1.06, 0 m); a scene must be authored around it in that frame. A translation/rotation would let a y-up scene
   place the burner.
4. **Fire light under the Pel path tracer** (or a loud warning): with zero soot the Pel integrator draws the fire black
   while the log only says it uses "the approximate Pel preview".
5. **Cloth-body contact inside a liquid group.** In `monsoon` the floats and the awning never touch; a body falling on
   a sheet inside liquid passes through it (there is no contact between them in the liquid group).

## Final-round parameters (summary)

Measured proof bakes, and the final round's settings with extrapolated costs (16 cores, at most about 10 busy;
liquid work grows about 16 times from 64 to 128 cells, 8 times the cells and twice the steps; the binaries of
`1b4f924db` are 1.4-2.5 times faster on whole liquid bakes than those of `cc071c604`). None of the final numbers
was measured.

| Scene | Proof bake (measured) | Final simulation settings | Final bake (extrapolated) | Final peak RSS | Final render | Animation |
|---|---|---|---|---|---|---|
| `unveiling` | 24 s, 0.18 GB (31 × 31) | silk 61 × 61 | 3-6 min | < 1 GB | 1920 × 1080, PT 512 spp, OIDN | frames 0-150, 5 s at 30 fps; hero 84 |
| `conjurer` | 188 s, 0.2 GB (31 × 31) | silk 49 × 49 | 10-20 min | < 1 GB | 1920 × 1080, PT 512 spp | frames 0-90, 3 s; hero 11 |
| `tide` | 177 s, 1.19 GB (64 cells) | 128 cells | 25-35 min | 4-6 GB | 1920 × 1080, PT 512 spp | frames 0-120, 4 s; hero 27 |
| `honey` | 525 s, 0.91 GB (64 cells, 17 × 17) | 128 cells, linen 33 × 33 | 1.5-3 h | 3-5 GB | 1920 × 1080, PT 512 spp | frames 0-150, 5 s; hero 6 |
| `monsoon` | 233 s, 0.91 GB (64 cells, 21 × 18) | 128 cells, awning 41 × 35 | 45-90 min | 4-6 GB | 1920 × 1080, PT 512 spp | frames 0-120, 4 s; hero 30 |
| `hearth` | not baked (fire reserved); fire measured 0.54 s/step at tier 6, about 25 min for the window, 1.6 GB | fire tier 8, pennant 19 × 37 | 75-90 min Metal | 3-4 GB | 1920 × 1080, spectral PT 256 spp, 16 wavelengths | frames 0-56, 3.5 s at 16 fps; hero 40 |

All renders: denoised copies (`_denoised`) beside the raw ones; the fire scene writes a linear 32-bit EXR first.
Recommendation: bake the final round one scene at a time (each liquid bake uses 8 threads, `threads 8` in the
scenes), and check every bake's last lines for `warning:` before rendering: a finer grid changes the impact steps.

## Tests and the corpus

- `tests/data/cst_derive_golden.txt`: one line per new scene (regenerated with `CstDeriveGoldenTest --generate`, no
  cache directory exported, so the scenes derive unbaked; only the six new lines changed). A scene edit changes its
  line: regenerate after editing a showcase scene.
- Scene sweeps: `CstDeriveGoldenTest` enumerates tracked `scenes/*.RISEscene` (derive only, no bake, no render);
  `run_scenes.sh` covers `scenes/FeatureBased/` only, so these scenes are not rendered by it.

## Verified, inferred, not done

- **Verified (run).** Every proof bake and render listed above (logs beside the frames, `/usr/bin/time -l` in each
  `bake_*.log`); the progress-detail lines quoted per scene (no rotation or linearisation warning in the committed
  versions of `tide`, `honey`, `monsoon`; `unveiling` and `conjurer` have no liquid); the golden lines; the fire
  look-development render (render only, partial version-3 cache) and the Pel-black finding (by a stripped-scene
  experiment and the integrator code).
- **Inferred, not measured.** Every final bake time and memory figure (scaling from the proof and
  [SIMULATION_PERFORMANCE.md](SIMULATION_PERFORMANCE.md)); that the final grids stay warning-free (a finer grid helps
  per M7m's Known limits, but the impact steps change); the hearth pennant's behaviour in the plume (never baked).
- **Not done.** The fire bake and the pennant (the GPU fire solver is reserved for the version-4 re-baseline; any
  version-3 cache would be stale); final bakes and renders (next round); `monsoon`'s barrier-hole count (862) is
  reported, not explained.
- **Proof caches** were baked with two builds (`tide`'s with the binaries of `cc071c604`, the rest with those of
  `1b4f924db`); for non-fire solvers their bytes are the same, but a partial cache resumes only with its own build.
