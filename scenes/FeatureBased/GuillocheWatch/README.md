# Guilloché Thin-Film Watch — RISE showcase scene

A physically-based hero render of an original guilloché-dial titanium watch
design, showcasing RISE's **thin-film
interference** material. The anodized-titanium dial's gold → magenta → blue
iridescence is *real* thin-film (TiO₂-on-Ti) Fresnel — not a painted texture; an
oxide-thickness map drives the film thickness per-pixel across the woven
guilloché relief.

Everything for this scene lives in this folder: the scene file, the asset
generators, and the generated meshes/textures. Asset `file` paths in the scene
are repo-root-relative (resolved via `RISE_MEDIA_PATH`), e.g.
`scenes/FeatureBased/GuillocheWatch/dial.raw2`.

## At a glance — what's switchable

A thin-film playground: every look below is **physically derived** (real n,k +
the in-renderer Airy/TMM evaluator), and most axes switch live in the GUI.

- **Base metal** — Ti / Nb / Ta / steel, via the dial object's `material`. Each
  differs in substrate n,k, oxide (TiO₂/Nb₂O₅/Ta₂O₅/Fe₃O₄), the oxide-thickness
  nm window, **and** the radial dose shape (per-metal oxidation kinetics).
- **Torch pattern** — uniform vs lightning-zigzag emphasis, via the dial
  material's `film_thickness` painter.
- **Colour palette** — different temper windows = different iridescent sweeps
  (warm gold→violet, vivid gold→blue, cool violet→blue, full multicolour), via
  the `film_thickness` painter.
- **Heat-tint** — fine torch start/end nm, via `oxide_thk` scale/bias.
- **Animation** — a native-timeline 45° turntable + subtle dolly on `cam_high34`.
- **Cameras** — 7 product angles (hero, macro punch-in, profile, flat-lay, …).

Per-topic sections follow; agent implementation notes are in [AGENTS.md](AGENTS.md).

## Render

```sh
export RISE_MEDIA_PATH="$(pwd)/"          # repo root
printf "render\nquit\n" | ./bin/rise scenes/FeatureBased/GuillocheWatch/watch_dial.RISEscene
# active camera = cam_high34 (the animation base) -> rendered/watch_dial0000.png
```

The scene defines **7 cameras**. **`cam_high34` is the ACTIVE camera** (last
defined) — it is the animation base, so a bare `render` and `renderanimation`
use it. `cam_photo` is the hero STILL; render it (or any angle) with the views
tool, which writes clean un-numbered PNGs:

```sh
python3 scenes/FeatureBased/GuillocheWatch/render_watch_views.py                  # all 7 stills
python3 scenes/FeatureBased/GuillocheWatch/render_watch_views.py --cam cam_photo  # the hero still
# one PNG per angle -> rendered/watch_<name>.png
```

| Camera | Angle | Purpose |
|---|---|---|
| `cam_high34` | high 3/4 | **ACTIVE** + animation base; rakes the relief from above |
| `cam_photo` | 3/4 (ψ=315°) | hero STILL (selectable) — 100 mm f/16; 12 to NE, crown right |
| `cam_macro` | low, crown-side | 100 mm f/8 punch-in, shallow DOF (bokeh strap) |
| `cam_topdown` | straight down | flat-lay, whole dial + strap |
| `cam_graze` | low ~12° | dramatic grazing rake |
| `cam_profile` | side | case slab, domed crystal, crown, lugs, strap |
| `cam_low34` | low 3/4 | sharp (no-DOF) alt beauty |

## Animation (native timeline)

A subtle **iridescence reveal** is authored directly in the scene as native
`timeline` keyframes on `cam_high34` (no external script):

- a **45° turntable** — the camera orbits ±22.5° about the watch's +Z axis
  (keyframed `azimuth` orbit angle, smoothstep-eased), look-at re-centred so
  the dial stays framed;
- a **subtle left→right dolly** — camera + look-at truck ~4 units laterally.

Lights stay fixed, so the specular highlight sweeps the guilloché and the
thin-film colours shimmer. Render the frame sequence (`multiple TRUE` in the
output chunk numbers the frames):

```sh
printf "renderanimation 0 1 48\nquit\n" | ./bin/rise scenes/FeatureBased/GuillocheWatch/watch_dial.RISEscene
# -> rendered/watch_dial0000.png .. watch_dial0047.png (+ _denoised)
```

`renderanimation <t0> <t1> <frames>` overrides `animation_options`. Assemble a
movie from the denoised frames with ffmpeg:

```sh
ffmpeg -framerate 24 -i rendered/watch_dial_denoised%04d.png -vf format=yuv420p rendered/watch_anim.mp4
```

Tune the move by editing the `timeline` keyframe values (`azimuth` = orbit angle
about +Z, `lookat` = dolly truck). The camera accepts keyframeable orbit angles
in **degrees**: `azimuth`/`phi` (spin about world-up) and `theta`/`elevation`
(pitch about screen-right). `cam_high34` must stay the **active (last)** camera
for `renderanimation` to drive it.

## Alternate dial geometry — a pattern library

The stock dial has a uniform woven-cell size everywhere.  The watch ships a small
**library of dial patterns** — all on the stock Cartesian UV, so every oxide
palette / metal applies to each (a pattern only changes the RELIEF).  Each is a
`dialfn_<name>` **`expression_function2d`** — the relief authored as a math
expression over (u,v) directly in the scene file — displaced onto the shared flat
`cartesian_disk_geometry` base (`dialdisk`) via `displaced_geometry … uv_seam_fold
FALSE`.  The six patterns are proven == the original C++ relief field to 1e-6 in
`tests/ExpressionFunction2DTest` (`dial_variants_gen.py` remains the historical
Python reference):

| `--field` / `dialmesh_<name>` | what it is |
|---|---|
| `lightning` | **the hero pattern** — 11 zigzag lightning bolts of a tight cube on a uniform rung-block ground |
| `radial`    | earlier swirled-petal lightning at a coarser bolt cell |
| `iris`      | 007 camera aperture — 8 blade leading-edges spiralling around a central hole, cube-filled blades |
| `swirl`     | log-spiral guilloché |
| `varwidth`  | alternating fine/coarse sunburst sectors |
| `uniform`   | the stock single-cell dial (A/B baseline) |

**The blessed parameters live in the scene chunks themselves.**  Author a new
pattern ENTIRELY IN THE SCENE FILE: write a new `expression_function2d` (any math
over u,v) + a `displaced_geometry` on the shared `dialdisk` base — no C++, no
rebuild.  (`param R` must equal the disk `radius`; see the AUTHORING RULE in
[docs/skills/effective-rise-scene-authoring.md](../../../docs/skills/effective-rise-scene-authoring.md).)
The six shipped patterns are also captured as templated builders in
`tests/GuillocheDialExpr.h`, the single source the test and a chunk-text emitter
share.

**Switch dials live in the GUI:** set the `dial` object\'s `geometry` to any
`dialmesh_<name>` — `geometry` is a **live rebindable reference** (like `material`:
the mesh swaps and the top-level BVH rebuilds on the next render, no reload —
`SceneEdit::SetObjectGeometry`).

## Asset pipeline — fully procedural (2026-06)

**Nothing is pre-baked.**  The dial relief (a `dialfn_*` `expression_function2d`
displaced onto a `cartesian_disk_geometry`), the oxide
heat-tint doses (`guilloche_oxide_painter` + `scalar_painter function2d`), and the
strap (`sweep_geometry`: a general closed-profile sweep fed the authored FKM cross-section) + stitching (`path_instances_geometry`: a general along-path instancer stamping an SDF thread capsule) are native scene chunks evaluated at
parse time — clone and render, no generator step.

The Python bakers stay in this folder as the **golden reference implementations**:

```sh
dial_mesh_gen.py        # stock dial field + oxide-dose model      (reference)
dial_variants_gen.py    # the composable pattern library            (reference)
thermal_oxide_sim.py    # Airy/CIE oracle, temper ladders, kinetics (reference + --metal-ladders)
strap_mesh_gen.py       # swept-band + saddle-stitch generator      (reference)
guilloche_gen.py        # earlier POLAR rose-engine generator       (historical)
render_contact_sheet.py # metal x palette ~4K contact sheet         (still a live tool)
```

`tests/GuillocheFieldTest.cpp` + `tests/ProceduralMeshTest.cpp` pin the C++ port
to these sources (pattern heights, oxide doses, torch masks, mesh vertices /
normals / UV / triangles).  Change the C++ and the Python together and regenerate
the golden values from the Python.

## Heat-tint tuning

The iridescence palette is dialable from the scene without re-baking: the
`oxide_thk` `scalar_painter` `bias`/`scale` set the torch START / SPAN thickness
in nm (centre = bias, rim = bias + scale). Presets are in comments at the top of
`watch_dial.RISEscene`. The *radial falloff* (how fast it heats outward) is the
`falloff` parameter on the `guilloche_oxide_painter` chunks
(`quadratic|smooth|linear`).

## Torch pattern variants (non-uniform torch)

Beyond the uniform radial sweep, the dial ships oxide painters that emulate the
artist holding the torch **non-uniformly** — dwelling along the lightning zigzag
so it stands out in a different interference colour (a deliberate, hand-torched
look). All three share the dial's Cartesian UV and the same scale/bias → nm
semantics, so they are drop-in:

| dose function (`guilloche_oxide_painter`) | scalar_painter | look |
|---|---|---|
| `oxide_fn` (torch_amount 0) | `oxide_thk` | uniform radial gold→blue |
| `oxide_fn_lightning_hot` (torch_amount +0.40) | `oxide_thk_lightning_hot` | zigzag torched LONGER → bluer lightning |
| `oxide_fn_lightning_cool` (torch_amount −0.40) | `oxide_thk_lightning_cool` | zigzag torched LESS → golder lightning |

**Switch in the GUI:** select the `tf_dial` material and set its `film_thickness`
slot (a `scalar_painter` reference dropdown) to one of the three. The torch model
is `thermal_oxide_sim.apply_torch_pattern(base, pattern, amount)` (reference); the
pattern is the painter's TorchMask — the dial's petal zigzag, so the colour zigzag
lines up with the relief. Add new looks with a new `guilloche_oxide_painter`
chunk (different `torch_amount`, or a different `pattern`).

## Colour palettes (temper windows)

The same Ti dial reads as a *different iridescent palette* depending on which
slice of the temper sequence the oxide spans — emulating the dial variety of
flame-anodized guilloché watches. These share the dose shape (`oxide_png`) and
differ only in the nm window; switch via `tf_dial`'s `film_thickness` painter:

| painter | window (nm) | look |
|---|---|---|
| `oxide_thk` | 22–38 | **vivid** gold → purple → blue (default) |
| `oxide_thk_warm` | 16–30 | gold/straw centre → magenta/violet |
| `oxide_thk_cool` | 28–44 | violet → blue → cyan (deep) |
| `oxide_thk_wide` | 14–46 | full straw → gold → purple → blue → cyan |

**Every base metal has the same palette set with its OWN windows** (the colour
sequence differs per metal, so the nm windows do too): `oxide_thk_<metal>_warm`
/ `_cool` / `_wide`, with the metal's default `oxide_thk_<metal>` as its *vivid*
— e.g. `oxide_thk_nb_warm`, `oxide_thk_ta_cool`, `oxide_thk_steel_wide`. Switch via
that metal's material `film_thickness` slot. That's **4 metals × 4 palettes = 16
example looks** (Ti's are the unprefixed `oxide_thk` / `oxide_thk_warm|cool|wide`).
Fine-tune any window via the painter's `scale`/`bias`, or add another.

## Base-metal variants (Ti / Nb / Ta / steel)

The dial's **base metal is switchable**, and it is *not* a simple recolour: each
metal has its own substrate n,k, its own oxide (TiO₂ / Nb₂O₅ / Ta₂O₅ / Fe₃O₄),
**and its own oxide-thickness window** — the same temper sweep lands at a
different nm on each metal because the oxide indices differ. The windows are
computed by the Airy/CIE oracle from the curated n,k (not reused from Ti):

| material | substrate · oxide | Q (kJ/mol) | nm window | temper sweep |
|---|---|---|---|---|
| `tf_dial` | Ti · TiO₂ | 160 | 22–38 | gold → blue (default) |
| `tf_dial_nb` | Nb · Nb₂O₅ | 135 | 30–55 | gold/orange → blue |
| `tf_dial_ta` | Ta · Ta₂O₅ | 80 | 26–52 | bronze → gold → purple (no clean blue) |
| `tf_dial_steel` | steel · Fe₃O₄ | 165 | 28–56 | gold → purple → blue (classic temper) |

**Switch in the GUI:** select the dial object (`dialmesh`) and set its
`material` (a rebindable reference — `ObjectIntrospection`) to one of the four.
The interference colour is then computed rigorously in-renderer from that metal's
substrate + oxide n,k at its thickness.

The dial **geometry** is a live rebindable reference too now — swap the dial mesh
(`dialmesh` ↔ `dialmesh_radial` ↔ `dialmesh_lightning`) from the same panel; the
top-level BVH rebuilds on the next render (`SceneEdit::SetObjectGeometry`).

**Regenerate / inspect the data:**
```sh
python3 scenes/FeatureBased/GuillocheWatch/thermal_oxide_sim.py --metal-ladders
```
prints each metal's thickness→colour ladder + its temper window. **Even the dose
*shape* is per-metal:** each metal's parabolic-oxidation activation energy `Q`
(`thermal_oxide_sim.METAL_KINETICS`, representative literature values) bends the
radial curvature — higher `Q` concentrates growth at the hot rim (bigger
thin/gold centre, sharper rim), lowest for Ta. The per-metal dose shapes
are the `metal ti|nb|ta|steel` presets on the `oxide_fn_<metal>` chunks (or an
explicit `activation_ea`). So **substrate n,k + oxide n,k + nm window + radial
shape ALL differ per metal** — nothing is reused from Ti. (Q is regime-dependent; values are documented + editable in
`METAL_KINETICS`.)

See [docs/THIN_FILM_INTERFERENCE.md](../../../docs/THIN_FILM_INTERFERENCE.md) for
the full feature writeup (TMM/Airy reference, n,k data, GGX `fresnel_mode
thinfilm`, the spectral path).
