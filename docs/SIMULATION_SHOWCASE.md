# Simulation showcase scenes

2026-11-27/28. Branch `simulation-liquid`. Solver versions: `liquid.mac` m7m.1 (with the inflow sources and drains),
`cloth.shell` m3a.8, `rigid.bodies` m6.2 (with coupling surfaces on analytic bodies), `fire.methane` version 4.

Six scenes in `scenes/Showcase/Simulation/`, each with one idea, that together exercise every solver and every
coupling the engine has. Each section gives the idea, what it demonstrates, the design history (what was tried and
why it changed), the proof and the **measured final** bake and renders; the summary table is at the end.

**Where they live, and why not `FeatureBased/`.** `scenes/README.md` puts showcase scenes in `FeatureBased/`, but
`run_scenes.sh` renders every scene under `FeatureBased/` with a plain `render`, and these scenes need a bake first
(unbaked, their simulated meshes are empty and the fire medium is empty) and a fire render needs its EXR output
first. So they get their own folder, `scenes/Showcase/Simulation/`, driven by `scripts/simulation_showcase.sh`.

| Scene | Idea | Solvers | Coupling | Hero (final) |
|---|---|---|---|---|
| `unveiling` | silk drawn off a crystal orb | cloth | cloth with analytic colliders, a keyframed pin, self-collision | `rendered/showcase/unveiling/final/unveiling_denoised0085.png` |
| `conjurer` | two props tossed up into a silk canopy | cloth + rigid | implicit cloth-rigid (analytic bodies with coupling surfaces), body-body contact | `rendered/showcase/conjurer/final/conjurer_denoised0022.png` |
| `tide` | a wave meets a tower of toy blocks | liquid + rigid | owned liquid-rigid, body-body stacking and collisions, a static solid, a drain | `rendered/showcase/tide/final/tide_denoised0010.png` |
| `honey` | honey drizzled from a dipper onto stretched linen | viscous liquid + cloth | an inflow source; owned implicit sheet with viscosity | `rendered/showcase/honey/final/honey_denoised0070.png` |
| `monsoon` | rain fills an awning that spills into a basin with floats | liquid + cloth + rigid | one group: implicit sheet and owned bodies; drop source and drain | `rendered/showcase/monsoon/final/monsoon_denoised0075.png` |
| `hearth` | a sooty pool fire in slow motion, a pennant riding its updraft | fire + cloth | one-way fire to cloth (aerodynamic drag) | `rendered/showcase/hearth/final/hearth_denoised0036.png` |

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
scripts/simulation_showcase.sh final tide render animation   # every frame at 960 x 540, 64 spp (SHOWCASE_ANIM_WIDTH/HEIGHT/SAMPLES)
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

## unveiling (cloth) — final

**Idea.** An unveiling. Crimson silk hangs from a brass rod just behind a black marble plinth and falls over the
crystal orb on it; after 1.8 s the rod rises and draws back, the silk's hem slides over the orb and lifts away.

**Demonstrates.** cloth.shell alone: a heavy silk satin (0.14 kg/m², overrides on silk-lightweight) in still air
(`drag_coefficient 1.0`), gathered onto the rod (1.11 m on 1.0 m, so it hangs in folds), barrier contact with three analytic colliders
(the floor plane, the plinth box, the orb sphere), self-collision, friction, a pinned edge that follows a keyframed
scene object (`pin v0 follow rod`, the rod's `timeline`). Renderer: `weave_material` silk with thin transmission, a
dielectric orb with per-channel IOR (1.50 / 1.53 / 1.57: exaggerated dispersion), coated black marble, a brass collar.

**Art direction (round 2).** A small, bright key high on the left (0.12 m area light, so the path tracer finds the
orb's caustic and the orb takes a crisp highlight); a warm rim from behind and above through the silk (the folds'
edges and the translucency read); both kept out of frame; the camera aimed a little right so the plinth sits centred.
The hero is the moment the hem is halfway across the orb, chosen on the final bake (the finer mesh runs a few frames
later than the proof: 86 at proof, **85** at final).

**Proof.** 31 × 31 vertices (cloth-weight round): **30 s**, 0.18 GB, 1 step halving, none unconverged; the proof frames
listed are the round-2 ones (silk-lightweight, no air), not re-rendered. Frames `rendered/showcase/unveiling/proof/unveiling_denoised00{20,60,86,110,150}.png`.

**Final (measured).** 61 × 61 vertices, 151 frames: **370 s** wall, **0.55 GB** RSS, 13 step halvings (the gathered
start), none unconverged, no warning. Hero 1920 × 1080, 512 spp: 5 min 49 s (349 s) →
**`rendered/showcase/unveiling/final/unveiling_denoised0085.png`**. Animation 960 × 540, 64 spp, 151 frames: 30 min (1,823 s)
→ `rendered/showcase/unveiling/final/anim/`, `rendered/showcase/movies/unveiling.mp4`. The first final (silk-lightweight,
no air, flat on the rod: 119 s) is kept in `rendered/showcase/unveiling/final_previous/` and
`rendered/showcase/movies/previous/unveiling.mp4`; why it changed: "Cloth weight and shimmer" below. Improve: the
orb's dispersion shows as colour in its body but the caustic on the plinth stays faint.

## conjurer (cloth + rigid) — final

**Idea.** A conjurer's canopy. A square of champagne silk hangs by its four corners over a stage; a gilded cube and an
emerald ball are tossed up into it from either side, meet in the silk, punch it into peaks, are thrown back down by
it and bounce across the boards while the canopy billows. A warm spotlight pool, footlights, a cool rim, a dark red
curtain.

**Demonstrates.** cloth.shell and rigid.bodies in one implicitly coupled group (sub-iterated to a tolerance, step
halving on non-convergence): each prop is an **analytic** body (sphere, box: its own mass, plane contact and
**body-body contact**) whose coupling surface is its scene object's mesh (M9: `body ... surface <object>` with the
cloth's `collider_port <object>`), moved as a mesh collider of the cloth (`rigid_surface`); the cloth's contact
reaction is the bodies' load (`contact_reaction targets=ball,cube`). The silk is a heavy satin (0.14 kg/m²) in
still air (`drag_coefficient 1.0`). Renderer: translucent silk weave, a metallic
gold GGX (Schlick, dark diffuse), coated emerald.

**Art direction (round 2).** The first final was too dark and read as a dark-blue shape on a dark-red backdrop
(`rendered/showcase/conjurer/final_v1/`, mesh bodies, no body-body contact). Now: champagne silk that separates from
the curtain, a visible warm spotlight pool from the front, stronger
footlights and a cool rim on the silk, larger props (ball 10.5 cm, cube 17 cm), the camera in so the canopy spans
about half the frame; the props tossed toward each other so they meet under the silk; a livelier stage (restitution
0.45). Hero: just after the props rebound, both in the air under the sagging silk (frame 22 on the final bake).
Round 3 ("Cloth weight and shimmer" below): the spot came from above the front through the canopy and skimmed the
silk, so its hard shadows drew stepped patches that flickered; it now comes from the front below the canopy (its pool
on the boards and the curtain base) and a soft box keys the silk.

**Design history.** (1) Before M9 a body on cloth had to be a mesh body without body-body contact (Engine requests,
1, now built). (2) A *free* silk dropped through which the props were tossed failed the cloth's Newton solve in five
attempts when it was pinched between a landing prop and the stage ("prescribed motion is infeasible ... pinched", then
"cloth Newton did not reach its tolerance" at t = 0.93-1.27 s); the canopy keeps the silk above the props.

**Proof.** Silk 31 × 31, 91 frames (cloth-weight round): **153 s**, 0.21 GB; no halving, none unconverged (the proof frames listed are
round 2's, not re-rendered). Frames
`rendered/showcase/conjurer/proof/conjurer_denoised00{08,11,18,24}.png`.

**Final (measured).** Silk 61 × 61 (was 49 × 49): **528 s** wall, **0.64 GB** RSS; no halving, none unconverged.
Hero 1920 × 1080, 512 spp, 6 min 31 s (391 s) → **`rendered/showcase/conjurer/final/conjurer_denoised0022.png`**;
animation 960 × 540, 64 spp, 91 frames, 21 min (1,256 s) → `rendered/showcase/conjurer/final/anim/`,
`rendered/showcase/movies/conjurer.mp4`. The round-2 final (silk-lightweight, no air, 49 × 49, the spot through the
canopy: 402 s) is kept in `rendered/showcase/conjurer/final_previous/` and `rendered/showcase/movies/previous/`.

## tide (liquid + rigid) — final

**Idea.** A child's tower of painted wooden blocks stands on the dry sand floor of a glass aquarium; a dam of green
water is released behind it with a toy boat riding on top. The wave lifts the blocks off one another, the tower comes
apart, and blocks and boat float, roll and knock together while the water runs up a sand beach and sloshes back.

**Demonstrates.** rigid.bodies (six stacked bodies of three shapes plus the boat: body-body contact and stacking, the
tank and the beach as planes) and liquid.mac in one group: the liquid owns the exchange and solves the bodies' six
degrees of freedom in its pressure solve (buoyancy, the wave's push, wakes); rigid.bodies owns every contact and gives
the liquid's copies the same contact (`body_contact`). A static `solid plane` (the beach) and an M9 `drain` (the open
rim). Renderer: tinted water, 6 mm glass panes (both dielectrics now specular: `scattering 1e6`, which removed the
soft, defocused look of the proof), a lightbox backdrop, a softbox for caustics.

**Art direction (round 2).** The camera pulled back to show the whole tank, the palette cut to natural wood, red and
one blue, a darker room around a lightbox; hero: the instant the wave face meets the still-standing tower.

**Design history (the ceiling).**

| Version | Tank / water | Result |
|---|---|---|
| round 1 | 0.6 m tall, dam 0.245 m, vertical far wall | proof: run-up to about 0.53 m |
| final 1 | the same at 128 cells | **3,575 s, 3.0 GB, no warning, but a thin sheet climbed the far corner to the ceiling and spread along it**: rejected |
| `top open` (proof) | 0.6 m, dam 0.22 m, top an open boundary | **29.6 million clamped faces** in 2,805 steps and ten times the bake time: rejected |
| final 2 | 0.7 m tall, dam 0.21 m | 3,583 s, 4.8 GB, no warning; the corner film still reached the 0.7 m ceiling at 128 cells: rejected |
| final 3 | 0.7 m, dam 0.21 m, a sand beach (31° slope to 0.12 m at the far wall) | 4,618 s, 5.0 GB, 2 faces clamped; the film still reached the ceiling: rejected |
| **final 4** | 0.7 m, dam 0.18 m (wider), the beach, and a **drain along the rim** (top 2 cm: an open tank's spill) | below |

The corner film is thin (a few cells) and appears only at 128 cells; it reads as a numerical wall jet rather than a
physical run-up (a 0.2 m bore runs up a vertical wall by a few times its height, not 0.7 m). Worth a look by the
liquid owner: free-slip corners at high resolution.

**Proof (final 4's design).** 64 cells: **419 s**, 0.68 GB, no clamped faces; the rim drain removed 0.059 L. Frames
`rendered/showcase/tide/proof/tide_denoised00{08,14,22,30,90}.png`.

**Final (measured, final 4).** 128 cells (6.25 mm), 121 frames: **3,106 s** (52 min) wall, **4.97 GB** RSS; no clamped
faces, no warning; the rim drain removed 0.21 L of corner film over the run (1% of the 20.45 L), so nothing collects
on the ceiling; a few drops of that film fall back from the rim later. With the lower dam the wave lifts the tower's
lower blocks and topples the ball, and the stack drifts off afloat rather than bursting apart. Hero (the wave face
meeting the standing tower) 1920 × 1080, 512 spp → **`rendered/showcase/tide/final/tide_denoised0010.png`**;
animation 960 × 540, 64 spp → `rendered/showcase/tide/final/anim/`. Render times: hero 12 min (732 s), animation 47 min (2,815 s).

## honey (viscous liquid + cloth) — a real pour (M9)

**Idea.** Honey drizzled from a wooden dipper onto a linen napkin stretched in a square walnut frame: the rope thins
as it falls 26 cm, piles into a glowing amber mound that slumps and spreads, and the linen sags under it. Low warm
light from behind and above, so the honey glows.

**Demonstrates.** An M9 inflow source (`source pour cylinder ... speed 0.25 schedule 0 0 0.15 1 2.6 1 3.2 0`: a nozzle
1.4 cm in radius, ramped in over 0.15 s, held, tapered off by 3.2 s) on liquid.mac with viscosity (10 Pa s,
1420 kg/m³) and an implicitly coupled linen sheet (no-slip wall of the viscous solve; the drag reaches the cloth).
Renderer: honey as a strongly absorbing dielectric (1 cm passes 0.93 / 0.62 / 0.22, IOR 1.49), woven linen, walnut.

**Limits designed around.** The nozzle is 4 cells across its radius at the final 128 cells (2 at the proof's 64,
where the rope's tip erodes: M9's stated limit), emits into air, and sits more than two cells from the sheet; no
coiling is expected at these resolutions (M9 saw none). Linen pinned all round, 0.5 kg/m².

**Proof.** 64 cells, linen 17 × 17, 151 frames: **623 s**, 0.89 GB; rotation stretch up to 0.044%, linearisation
0.035% (no warning), barrier holes 0, source added 0.449 L. Frames
`rendered/showcase/honey/proof/honey_denoised00{15,30,45,75}.png`, `..._0150.png`; hero frame 45 (the rope landing on
its growing mound). The first version (a pillar released at t = 0) is described in the previous round's history: it
read as a bell, not a pour.

**Final (measured).** The first final, 128 cells with the 0.5 kg/m² linen, baked in 14,732 s (4.1 h, 2.0 GB) and its
progress detail **warned**: linearisation correction up to 1.8% (rotation stretch 0.73%), where the rope lands on
the linen; not shipped. The committed final: **96 cells** (4.7 mm; the nozzle 3 cells in radius, at M9's stated
floor) and the linen at **1.0 kg/m²**: **2,982 s** (50 min) wall, **1.16 GB** RSS; rotation stretch up to 0.18%,
linearisation correction up to 0.18% (no warning), barrier holes 0, no clamped faces; the source added 0.4497 L, the
liquid within 0.0001 L of its target. Hero (the rope landing on its spreading pool) 1920 × 1080, 512 spp →
**`rendered/showcase/honey/final/honey_denoised0070.png`**; animation 960 × 540, 64 spp, 151 frames →
`rendered/showcase/honey/final/anim/`. Render times: hero 6 min 5 s (365 s), animation 36 min (2,177 s). Improve: no coiling (needs a rope several cells across
where it buckles: a finer grid than a warning-free bake allowed here); the pool's rim is smooth at 96 cells.

## monsoon (liquid + cloth + rigid, one group) — real rain (M9)

**Idea.** A cloudburst over a courtyard basin. Heavy rain falls on a striped awning held in a frame tilted over a
raised stone basin; the awning bellies under the weight into a bag, fills, and the water runs over its low edge in a
clear sheet into the basin, where two wooden floats, a painted ball and a toy boat rock in the waves and knock
together.

**Demonstrates.** All three non-fire solvers in **one** `simulation_group` (`courtyard`): liquid.mac owns both
exchanges, the awning is an implicitly coupled sheet and the floats are rigid.bodies bodies in the same pressure
system; rigid.bodies owns their contact. Rain is an M9 drop source (`source rain box ... drops 5 4 0.028 0.22`, 2.8 cm
drops: 2 cells at the proof's 64, 4 at the final 128, released 3.5 cm above the awning in a box tilted with it, at
0.5 m/s, for 2.6 s: 21.6 L) and an overflow `drain` in a corner keeps the basin below its kerb. Renderer: a striped
`weave_material` driven by an `expression_painter` on the cloth's UVs, tinted water, coated tiles.

**Rain (round 2).** The first rain (the same drops released 5 cm above the awning's high edge, so up to 17 cm above
its low end, at 1.2 m/s) kicked the awning to a rotation stretch of 2.2% (warning). Tilting the release box with the
awning and slowing the drops to 0.5 m/s: 0.45% / 0.14%, no warning. The M9 barrier-hole fix reads 0 holes.

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
| 7 | as 6 | three boxes stepped 2-6 cm above the tilted awning (the impact was the kick) | 0.79% / 0.39% | proof round 1 |
| 8 | as 6 | M9 drops, 5 cm above the high edge, 1.2 m/s | 2.2% / 0.67% | warning |
| **9** | as 6 | **M9 drops in a box tilted with the awning, 3.5 cm above it, 0.5 m/s** | **0.45% / 0.14%** | committed |

**Proof (rain).** 64 cells along x (1.4 cm), awning 21 × 18, 121 frames, macro step 1/240 s: **179 s**, 1.09 GB;
rotation stretch 0.45%, linearisation 0.14%, barrier holes 0 (20 faces not judged), rain added 21.6 L, drain removed
2.7 L. Frames `rendered/showcase/monsoon/proof/monsoon_denoised00{15,30,45,75}.png`, `..._0110.png`; hero 75 (the
spill over the low edge with the rain still falling and a crater where it lands).

**Final (measured).** 128 cells (7 mm), awning 41 × 35, 121 frames: **3,247 s** (54 min) wall, **2.75 GB** RSS;
rotation stretch up to 0.44%, linearisation correction up to 0.18% (no warning), barrier holes 0 (5 faces not
judged), no clamped faces; rain added 21.61 L, the drain removed 2.92 L, liquid volume within 0.01 L of its target.
Hero 1920 × 1080, 512 spp, 9 min 29 s → **`rendered/showcase/monsoon/final/monsoon_denoised0075.png`**; animation
960 × 540, 64 spp, 121 frames, 42 min → `rendered/showcase/monsoon/final/anim/`. Improve: the ground and sky are
still plain (an environment map or a courtyard set would finish it).

## hearth (fire + cloth) — final

**Idea.** A fire-lit still life in slow motion. A pool fire burns in a ring of firebrick on a slate floor and is the
room's only key light: a yellow-orange flame on a blue base, puffing, under a dark smoke column. A saffron silk
pennant, pinned along one edge to an iron rod behind the fire and laid out over the plume, is lifted and tossed by the
updraft; a heavy glass tumbler of water on a low oak stool catches the flame; a faint cool moonlight is the only other
light.

**Demonstrates.** fire.methane version 4 (the certified 0.30 m / 33 kW burner, tier 6 proof / tier 8 final, fast mode)
with the owner's chosen soot look (`methane_soot_yield 0.1`, `soot_radiative_fraction_cap 0.45`,
`soot_self_absorption 1`; docs/SIMULATION_FIRE_LOOK.md variant C), and a **one-way fire-to-cloth coupling**: the
pennant's `surface_load` comes from the fire's cache through the `aerodynamic_drag` adapter (gas velocity, density
from the temperature, quadratic drag on the velocity relative to the cloth). The fire does not heat, scorch or burn
the cloth, and the water is static geometry. Renderer: the spectral path tracer (the blue base is the CH*/C2*/CO2*
chemiluminescence, which only the spectral integrator evaluates; soot glows in either), the emitting medium as the
key light on silk, glass, stool and floor.

**Honesty.** Yield 0.1 is a sooty-fuel look on methane chemistry: about three times methane's fitted soot, and real
methane makes far less (the smoke column is not methane's). The tall, bright, puffing flame is the certified burner's
**ignition-pilot phase**, fire time 1.0-2.06 s; at about 2.1 s the pilot ends and the flame settles to 0.4-0.5 m, 8-16
times dimmer (SIMULATION_FIRE_LOOK.md, second section).

**Timing: a slow-motion shot inside the pilot phase.** The fire's frames are a fixed 1/16 s apart and play back at
scene time = fire time (the cache's time map has scale 1; no option changes it, and the frame cadence is fixed), so a
"higher cache frame rate" is not available. The shot therefore renders scene t = 0-1.05 s (fire time 1.0-2.05 s) as
64 frames, 1/60 s apart, meant to be played at 16 fps: a quarter-speed slow motion, 4 s on screen. Between fire frames
the renderer holds the earlier frame advanced by its velocity, so the in-between frames move rather than repeat (they
are not new solver states). The pennant is baked at 60 Hz, so every rendered frame has its own cloth state. Exposure:
the PNG at +8 EV through ACES from the linear EXR (the flame's 99.9th-percentile pixel is 0.1-0.36 linear; +8 keeps the
set readable, ACES rolls the flame core off); the EXR is the primary output.

**Design history.**

| Step | Change | Why |
|---|---|---|
| round 1 | soot-free version-3 fire, window 1.0-4.5 s, hero at 3.5 s | the fire was reserved; look development on a partial cache showed a tall uniform blue column (the version-3 placeholder chemistry term) |
| round 2 | soot look C, window 1.0-2.07 s, slow motion, exposure +8 EV | the owner's choice; only the pilot phase is tall and lively |
| pennant 1 | hanging from a rod beside the plume, then over the plume's axis | **limp**: a pennant hanging edge-on in an updraft feels only the tangential drag (coefficient 0.02); no flutter |
| pennant 2 | laid out level over the plume from a rod behind the fire, 1.45 m up, silk-lightweight (70 g/m²) | the updraft meets its face, but at 1.45 m the hot, thin gas only about balanced its weight: it drooped |
| pennant 3 | silk chiffon (25 g/m²), 0.46 × 0.58 m, the rod 1.22 m up, saffron colours | proof: lifted and tossed by the puffs, lit warm by the flame below |
| **pennant 4** | the rod raised to 1.42 m | the tier-8 pilot flame is taller than tier 6's (to about 1.4 m in the biggest puffs): at 1.22 m it licked the silk, which nothing in the model burns |

**Proof.** Tier 6 fire, window to 2.07 s, 18 frames: **954 s** (16 min), 1.45 GB RSS; pennant 13 × 25 at 60 Hz: 3 s.
Frames `rendered/showcase/hearth/proof/hearth_denoised00{20,28,44,63}.png` (EXR beside each; pennant 3, rod at 1.22 m).

**Final (measured).** Fire tier 8 (69 × 69 × 106), fast mode, window to 2.07 s, 18 frames: **1,698 s** (28 min) of Metal,
**2.65 GB** RSS, no retries; pennant 19 × 37 at 60 Hz, 64 frames: 8 s, 0.55 GB, Newton converged every step (no
halving, none unconverged, no projected steps). Hero (a puff with the pennant lifted over the flame) 1920 × 1080,
spectral, 256 spp, 16 wavelengths → **`rendered/showcase/hearth/final/hearth_denoised0036.png`** (and `.exr`);
animation 960 × 540, spectral, 32 spp, 64 frames (play at 16 fps for the quarter-speed shot) →
`rendered/showcase/hearth/final/anim/`. Render times: hero 13 min (793 s), animation 30 min. Improve: the smoke column is dark on a dark wall and
reads only faintly; the pennant is small in frame; a longer pilot window (or a continuous pilot, solver work) would
give a longer shot without slow motion.

## Cloth weight and shimmer (2026-11-28)

**The complaint.** Watching the six movies, the owner: the cloth "seems to shimmer a lot like it has instability",
then "actually not instability, it's like it has no weight". In `unveiling` the silk, once off the orb, swung as a
flat panel up toward horizontal; in `conjurer` the canopy showed blocky light and dark patches that flickered.

**Measured (scratch tests and the shipped caches; tools: a cache reader over `manifest.cbor` and the `RISEMESH`
frames).**

| Candidate | Measurement | Verdict |
|---|---|---|
| Gravity, mass, the drag law | a 1.0 x 0.9 m silk-lightweight sheet dropped flat: free fall 9.569 m/s at 1 s against 9.81 (the 0.05/s mass damping exactly: g/alpha (1 - e^-alpha t)); with `drag_coefficient 1.2` terminal 0.980 m/s against sqrt(2 m'' g / (rho C_d)) = 0.977; dropped edge-on with drag it falls at the vacuum rate (the drag is along the normal only); hung from an edge it comes to rest vertical (centre of mass 0.450 m below the pins) | correct; not the cause |
| Rayleigh damping | alpha 0.05/s: time constant 20 s against a 1.5 s pendulum; with damping off the same swing peaks at 80, 74, 67 degrees instead of 77, 65, 61 | negligible (it does damp rigid motion, 2.5% of a free fall at 1 s, harmless at this size) |
| **No air** | `drag_coefficient` defaults to 0 and none of the showcase cloth set it (only `hearth`'s pennant has drag, from the fire through `aerodynamic_drag`). Hung from an edge and released level, silk-lightweight swings to 77 degrees past vertical, then 65, 61, 61, 58, 53 (period 1.7 s: the rigid-plate pendulum, 2 pi sqrt(2L/3g) = 1.55 s, lengthened by the amplitude), a flat panel the whole time. In the `unveiling` cache the hem slid off the orb at 3.1 s and the sheet then swung between +46 and -36 degrees for the rest of the shot; the `conjurer` canopy's centre bounced between 0.77 and 1.0 m at 2 Hz and was still bouncing 0.16 m peak to peak at 3 s | **the cause of "no weight"**: fabric is dominated by air broadside (at 1.5 m/s the drag pressure on a sheet, 1.6 Pa, is 2.4 times the weight of 70 g/m^2), so a vacuum swing reads as a stiff, massless board |
| Choreography (`unveiling`) | the rod moves at a constant 0.46 m/s (1.8-3.4 s) then 0.27 m/s: the big swing was not the rod but the hem's release from the orb, undamped | the rod is fine; the release needs air |
| Shimmer in the simulation | pinned silk draped over the orb, nominally at rest (frames 41-49): median normal change 2-3 degrees per frame, median second difference of position 0.7-1.1 mm (about 0.9 m/s^2), no frame-to-frame sign flips; `conjurer` frames 61-65 (2 s after the toss): 6-7.5 degrees, 7-16 mm (the trampoline bounce) | a slow crawl and the undamped bounce, not buckling that pops between two states |
| Shimmer in rendering (`conjurer`) | frame 22 at 960 x 540, 256 spp, no denoiser: the patches are the same with `face_normals TRUE`, with a Lambertian in place of the weave and at 128 and 256 spp, and vanish when the spot light is removed. The spot (0.3 2.2 2.6, aimed through the canopy) met 19% of the canopy's triangles at under 10 degrees; 0.9% had a facet and smooth normal on opposite sides of it (the shadow-terminator case) | **grazing hard light**: the spot's shadows of the mesh-scale undulations (median neighbour-normal angle 2.4 degrees, 2 cm triangles) have triangle-stepped edges, and they move with every undulation. Not normals, not the weave's tangent frame, not the denoiser |
| Playback | caches are 30 Hz for 30 fps (`hearth` 60 Hz for its 1/60 s frames); the mesh plays the nearest frame | not a source |

**Changes (scene level only; the solver was not touched).**

- `unveiling`: a heavy silk satin, `material_density 0.14` (about 32 momme charmeuse) with `material_bend_warp 3e-6`,
  `material_bend_weft 2.5e-6`; `drag_coefficient 1.0`; the silk gathered onto the rod (`size_u 1.111`, `stretch_u 0.9`:
  1.11 m of silk on 1.0 m, so it hangs in vertical folds rather than as a flat sheet); `substeps 16`,
  `newton_iterations 100` (a gathered start is a compressed sheet: at 0.8 the Newton solve failed at once, at 0.9 with
  the default 8 substeps it failed at 61 x 61). In a scratch test the same weight and drag (0.15 kg/m^2, C_d 1.0)
  released from level reaches vertical in 0.7 s, overshoots by 16 degrees, then 9 and 5: falls, swings and settles.
- `conjurer`: the same satin and drag; the silk mesh 49 x 49 to 61 x 61; relit: the spot comes from the front low
  enough that its cone passes under the canopy (its pool now lies on the boards and the curtain base behind the
  props), and a soft box above the front (`canopy_key`, 1.2 x 0.8 m) keys the silk. Footlights and rim unchanged.
- `hearth`, `honey`, `monsoon`: unchanged. `hearth`'s chiffon pennant already has air (the fire's gas through
  `aerodynamic_drag`, normal coefficient 1.2) and is meant to be tossed; `honey`'s linen and `monsoon`'s awning are
  pinned all round and loaded by liquid, and are still (median normal change 0.01 and 0.1 degrees per frame).

**Result (final caches).** `unveiling`: after the hem leaves the orb (3.0 s) the silk reaches vertical in 0.4 s, overshoots by
6.5 degrees and swings back by 6 (was +46 / -36 degrees); draped at rest the median normal change per frame is
0.9-1.1 degrees (was 2.2-2.8), the second difference 0.5-0.66 mm (was 0.72-1.15). `conjurer`: the canopy centre's bounce,
peak to peak over each half second from 1 s, is 0.13, 0.08, 0.05, 0.04 m (was 0.26, 0.23, 0.19, 0.16 m); frames 61-65: 1.3-2.3 degrees and 2-3.4 mm
per frame (was 6-7.5 degrees and 7-16 mm); at the hero frame the median neighbour-normal angle is 1.4 degrees (was 2.4)
and the old spot would meet 8.8% of the triangles under 10 degrees (was 19%); the new key leaves no stepped patches.
In the movies (960 x 540, 8-bit luma), the mean frame-to-frame change over the canopy in frames 50-88 is 2.95 levels
(was 11.1); `unveiling` draped at rest (frames 30-50), whole frame, 0.27 (was 0.36). Contact sheets before and after:
`rendered/showcase/movies/sheets/{before,after}_{unveiling,conjurer}.png` (every 6th frame).

**Recommended solver changes (not made: each changes `cloth.shell` bytes, so it needs a version bump and re-recorded
pins).** (1) Default `drag_coefficient` to about 1.0 (air at 1.2 kg/m^3) rather than 0, so a cloth in open air is
in air unless the author says otherwise; the drag law already matches the terminal-velocity check above, so it could
also lose its "explicit and unvalidated: preview only" note (the predictive conditions would then need drag on their
list). (2) A heavier silk record beside silk-lightweight (e.g. `silk-charmeuse`: 0.14 kg/m^2, bending 3e-6 / 2.5e-6
N m, stretch and shear as silk-lightweight), since a heavy drape is the common use. (3) Optionally drop the mass
term of the default Rayleigh damping (alpha 0.05/s slows a free fall); it is too small to matter here.

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
   **Done (M9 engine agent, 2026-11-27; [Parsers README](../src/Library/Parsers/README.md) "Coupling (M6)"):** an analytic body takes its
   coupling surface from a scene object with the body option `surface <object>` (`rigid_simulation { body ball sphere
   0.04 ... at x y z surface ball }`, the object placed at the body's initial pose); `collider_port ball` then has
   that surface's topology and the body keeps sphere/box/cylinder contact with other bodies and planes.
2. **Liquid inflow (an emitter).** A pour, a tap or rain needs liquid added over time (`source box|sphere ...
   velocity v [start s] [end s]`); only initial regions exist, so the honey "rope" is a pillar released at t = 0 and
   the rain a single slab.
   **Done (M9 engine agent, 2026-11-27; [Parsers README](../src/Library/Parsers/README.md) "Liquid inflow sources and drains"):** `liquid_simulation` fields
   `source <name> box|cylinder|sphere ... speed v [direction ..] [start s] [end s] [schedule t f ...] [drops nx nz r
   period]` (rain from a box) and `drain <name> <shape>`; works with viscosity, bodies and sheets.
3. **A transform for `fire_medium` (or a placed `fire_simulation`).** The fire sits in its own frame (z up, burner at
   1.06, 1.06, 0 m); a scene must be authored around it in that frame. A translation/rotation would let a y-up scene
   place the burner.
4. **Fire light under the Pel path tracer** (or a loud warning): with zero soot the Pel integrator draws the fire black
   while the log only says it uses "the approximate Pel preview".
5. **Cloth-body contact inside a liquid group.** In `monsoon` the floats and the awning never touch; a body falling on
   a sheet inside liquid passes through it (there is no contact between them in the liquid group).
   **Open (assessed by the M9 engine agent, 2026-11-27; [proposal §5](SIMULATION_IN_RISE_PROPOSAL.md)):** not contained;
   needs a second load input on `rigid.bodies` (the liquid already feeds `body_load`), the cloth's collider contact
   inside the liquid's linearised copy of the cloth, and implicit cloth-body iteration beside an owned exchange (the
   group refuses sub-iterations there). `monsoon`'s 862 barrier holes were the check misjudging faces a sagging sheet
   crosses twice, not holes (the check was fixed with the inflow sources; it now reports 0).

## Final round: measured (summary)

All on the 16-core machine, binaries of `cc746ac42` (M9) and, for `unveiling` and `conjurer` after the cloth-weight
round, `0e911fac8`; liquid bakes with `threads 8`; RSS is `/usr/bin/time -l`'s peak. Every animation is numbered PNG
frames (`<scene>_denoisedNNNN.png`) in `rendered/showcase/<scene>/final/anim/` and an MP4 in
`rendered/showcase/movies/` (`ffmpeg -framerate 30 ... -c:v libx264 -pix_fmt yuv420p -crf 16 -preset slow -movflags
+faststart`; `hearth` at 16 fps); the movies before the cloth-weight round are in `rendered/showcase/movies/previous/`.

| Scene | Final simulation | Bake (wall) | Peak RSS | Progress detail | Hero (1920 × 1080, 512 spp) | Animation (960 × 540, 64 spp) |
|---|---|---|---|---|---|---|
| `unveiling` | satin 0.14 kg/m², drag 1.0, gathered, 61 × 61, 151 frames | 370 s | 0.55 GB | 13 halvings, none unconverged, no warning | frame 85, 5 min 49 s | 151 frames, 30 min |
| `conjurer` | satin 0.14 kg/m², drag 1.0, 61 × 61, 91 frames | 528 s | 0.64 GB | no halving, none unconverged | frame 22, 6 min 31 s | 91 frames, 21 min |
| `tide` | 128 cells, 121 frames | 3,106 s | 4.97 GB | no warning, no clamped faces; rim drain 0.21 L | frame 10, 12 min | 121 frames, 47 min |
| `monsoon` | 128 cells, awning 41 × 35, 121 frames | 3,247 s | 2.75 GB | rotation 0.44%, linearisation 0.18%, holes 0 | frame 75, 9 min 29 s | 121 frames, 42 min |
| `honey` | 96 cells, linen 33 × 33 at 1.0 kg/m², 151 frames | 2,982 s | 1.16 GB | rotation 0.18%, linearisation 0.18% (the 128-cell bake warned at 1.8%: not shipped) | frame 70, 6 min 5 s | 151 frames, 36 min |
| `hearth` | fire tier 8, window 1.0-2.07 s, 18 frames; pennant 19 × 37 at 60 Hz | 1,698 s (fire, Metal) + 8 s | 2.65 GB | fire: no retries; pennant: no halving, none unconverged | frame 36, 13 min | 64 frames (32 spp), 30 min |

## Tests and the corpus

- `tests/data/cst_derive_golden.txt`: one line per new scene (regenerated with `CstDeriveGoldenTest --generate`, no
  cache directory exported, so the scenes derive unbaked; only the six Showcase lines change). A scene edit changes its
  line: regenerate after editing a showcase scene.
- Scene sweeps: `CstDeriveGoldenTest` enumerates tracked `scenes/*.RISEscene` (derive only, no bake, no render);
  `run_scenes.sh` covers `scenes/FeatureBased/` only, so these scenes are not rendered by it.

## Verified, inferred, not done

- **Verified (run).** Every proof and final bake and render listed above (logs beside the frames:
  `rendered/showcase/<scene>/<mode>/driver.log`, `bake_*.log` with `/usr/bin/time -l`, `status.txt`, `render_*.log`);
  the progress-detail lines quoted per scene; every hero frame looked at at full size; the candidate frames for each
  hero re-picked on the final caches (the finer meshes and grids shift the timing by a few frames); the golden lines
  (only the six Showcase lines move) and `run_all_tests.sh --tier fast --filter 'CstDerive*,Scene*'` (17 passed).
  Cloth-weight round (2026-11-28): the scratch physics tests and cache measurements quoted in "Cloth weight and
  shimmer"; the `unveiling` and `conjurer` final bakes, heroes and animations, their MP4s rebuilt with the encoding
  above and watched as contact sheets; the lighting diagnosis at 960 x 540 without the denoiser; the two golden lines
  regenerated (only they moved) and the same 17 tests passed.
- **Inferred.** That a heavy satin with C_d 1.0 is the right look for the owner's "no weight" (judged from the
  contact sheets, not by the owner); that the `tide` corner film is a numerical wall jet rather than physics (from its thinness and its
  appearance only at 128 cells); that the `honey` warning at 128 cells comes from the rope's landing (the only impact).
- **Not done.** A `honey` rope that coils (needs a finer grid
  than a warning-free bake allowed); a sustained tall flame for `hearth` (solver work: a continuous pilot or a larger
  burner); the proof frames of `hearth` show the pennant at its earlier 1.22 m rod height.
- **Builds.** Proof and final caches were baked with four binary copies (`cc071c604`, `1b4f924db`, `cc746ac42` for
  the M9 scenes, and the fire-version-4 thin-LTO build of `65e4b5838` for `hearth`); non-fire bytes are the same across
  them, but a partial cache resumes only with its own build.
