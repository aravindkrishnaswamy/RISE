# Scene-Language Scripting: What It Takes to Retire the Python Generators

**Status: B1 + B2 SHIPPED (2026-06-11).**  The texture bakers are now native
painters (`guilloche_oxide_painter` + the `scalar_painter function2d scale/bias`
form) and the mesh bakers native chunks (`guilloche_disk_geometry`, and the strap decomposed into the fully
general `sweep_geometry` + `path_instances_geometry`) — see `src/Library/Painters/GuillocheField.h`, the
factories in `src/Library/RISE_API.cpp`, and the golden tests
`tests/GuillocheFieldTest.cpp` / `tests/ProceduralMeshTest.cpp`.  Chunk names
differ slightly from the proposals below (kept as authored).  Orchestration
(C) stays external by decision; `sdf_gen.py` replacement (A-class, needs
in-chunk `FOR`) is deferred.

The GuillocheWatch scene leans on nine standalone Python/shell files.  This
document evaluates what it would take to fold that generation into the
`.RISEscene` language itself, what should instead become *native* RISE
features, and what should stay external.

## 1. What the scene language can already do

Three scripting layers exist today (all in
[AsciiSceneParser.cpp](../src/Library/Parsers/AsciiSceneParser.cpp) /
[AsciiCommandParser.cpp](../src/Library/Parsers/AsciiCommandParser.cpp) /
[AsciiScriptParser.cpp](../src/Library/Parsers/AsciiScriptParser.cpp)):

1. **Macros + expressions** — `DEFINE NAME value` (names `[A-Z_]+`, no digits),
   `UNDEF`, and `$(expr)` evaluation with `+ - * /`, parens, `sin cos tan sqrt
   hal`, constants `@PI @E`, macro refs `@NAME`.  Expressions must contain **no
   whitespace** (the tokenizer splits first).  `hal(d)` is a stateful Halton
   stream per dimension d (d < 256; ≥256 currently segfaults — known issue).
2. **Loops** — `FOR var start end inc` … `ENDFOR`, implemented by stream-seek
   replay, nestable.  **Top-level only**: a `FOR` can wrap *whole chunks*
   (this is how `mlt_torus_chain_atrium` / `bdpt_cloister` emit object grids)
   but **cannot sit inside a chunk body** — the body-accumulation loop only
   understands `#`, `>`, and `}` lines.
3. **Commands** — `>`-prefixed: `set / remove / modify / clearall / predict /
   render / renderanimation / load / photonmap / run / quit / mediapath /
   echo`.  `> load x.RISEscene` is a scene include; `> run x.RISEscript` runs
   a command-only script (no chunks, no loops).

Crucially, **macro substitution and `$()` evaluation DO apply inside chunk
bodies** (body lines pass through `substitute_macros_in_tokens` +
`evaluate_expressions_in_tokens` before dispatch).  So a chunk parameter can
already be computed; what cannot be done is *emitting a variable number of
lines inside one chunk*.

## 2. What the Python files actually do

| File | Lines | Class | Needs |
|---|---|---|---|
| `sdf_gen.py` | 149 | **chunk-line emitter** | loops + trig + number formatting, emitting repeated `part` lines *inside* `sdf_geometry` chunks |
| `strap_mesh_gen.py` | 216 | **mesh baker** | Catmull-Rom spline, swept profile with superellipse/crown cross-section, stitch capsule placement, RAW2 output |
| `dial_mesh_gen.py` | 429 | **mesh baker** | guilloché height field sampled on a Cartesian grid, analytic normals, 27 MB RAW2 (gitignored, must be regenerated before rendering) |
| `dial_variants_gen.py` | 359 | **mesh baker** | alternate pattern fields (lightning/iris/swirl/varwidth) — same sink |
| `guilloche_gen.py` | 975 | **field library + texture baker** | the pattern math (radial petals, woven grid, bolt fields) + relief/cut-angle map PNGs |
| `thermal_oxide_sim.py` | 1080 | **texture baker** | torch heat-diffusion simulation → oxide-thickness PNGs |
| `render_watch_views.py` | 137 | **orchestration** | per-camera renders (exists ONLY because the CLI cannot select a named camera) |
| `render_contact_sheet.py` | 133 | **orchestration** | render matrix + PNG montage |
| `gen_dials.sh` | — | **orchestration** | calls the bakers with blessed parameters |

## 3. Gap analysis

To absorb each class **as language features** you would need:

- Class "chunk-line emitter": in-chunk `FOR`/`ENDFOR` (the keystone gap), a
  richer function set (`atan2 pow abs min max floor mod` are all missing — the
  marker-ring generator needs none beyond `sin/cos`, but the crown flutes need
  none either; in practice trig + arithmetic covers `sdf_gen.py` entirely),
  and quality-of-life fixes (whitespace inside `$()`, digits in macro names).
- Class "mesh baker": arrays/vertex buffers, user-defined functions, file
  emission — i.e. a real programming language.  **This is the wrong direction**
  for a declarative, descriptor-driven scene format: it would make scenes
  Turing-complete, unintrospectable by the GUI, and slow to parse.
- Class "texture baker": same, plus image I/O — same verdict.
- Class "orchestration": already 90% covered by `.RISEscript` commands; the
  missing 10% is a camera-selection command.

## 4. The recommended shape: small language upgrade + native procedural chunks

The insight from porting this watch: every mesh/texture the Python files bake
is a *procedural definition* that RISE could evaluate natively — and once it
is native, it becomes **live-editable in the GUI and free of bake steps**,
which no scripting language (in-scene or external) can offer.

### Prong A — targeted language upgrades (unlocks the emitter class)

| Item | What | Effort | Notes |
|---|---|---|---|
| A1 | **In-chunk `FOR`/`ENDFOR`** | 3–5 days | Move the loop stack + seek-replay into a shared line-provider used by both the top-level loop and the chunk-body accumulator.  **Risk to manage**: the Phase-6 save engine records body-line stream positions (`RawTokenCapture::RecordLine`); replayed iterations must record the *loop source text once*, not N expanded copies, or override-block round-tripping breaks.  Add a parser regression test + a SceneEditorSuggestions count guard. |
| A2 | More math functions: `pow abs min max floor mod atan2` | 0.5–1 day | `MathExpressionEvaluator` + the `s/c/t/h` function dispatcher; update the parser-quirks doc. |
| A3 | Whitespace inside `$()`, digits in macro names | 1 day | Tokenizer-aware expression capture; relax `add_macro`'s `[A-Z_]+`. |
| A4 | `IF cond` / `ENDIF` (optional) | 1–2 days | Same seek machinery, simpler than FOR.  Not needed by any current generator; defer. |

With A1+A2, `sdf_gen.py` becomes ~30 lines of scene text — e.g. the marker
ring collapses to:

```
sdf_geometry
{
	name			markerringsdf
	part			cylinder union 0  0 0 0.72  90 0 0  1 1 1  18.475 0.11 0  0
	part			cylinder subtract 0  0 0 0.72  90 0 0  1 1 1  17.525 0.44 0  0
	FOR K 0 11 1
	DEFINE TH $(90.0-@K*30.0)
	part			box subtract 0  $(18.0*cos(@TH*@PI/180.0)) $(18.0*sin(@TH*@PI/180.0)) 0.72  0 0 @TH  1 1 1  1.425 1.05 0.44  0
	ENDFOR
}
```

(The 12-o'clock wide gap needs either `IF` or one hand-written line — fine.)

### Prong B — native procedural chunks (kills the bakers, the bake step, and the 27 MB meshes)

| Item | What | Replaces | Effort |
|---|---|---|---|
| B1 | **`guilloche_function2d_painter`** (pattern enum: uniform / lightning / radial / iris / swirl / varwidth + cell/amplitude/bolt params) feeding the existing `displaced_geometry` over a disk, plus a companion oxide-dose painter (radial falloff × pattern-correlated term) consumed by `scalar_painter` (extend it to accept a function2d source — it already learned `texture` this way) | `dial_mesh_gen.py`, `dial_variants_gen.py`, most of `guilloche_gen.py`, `thermal_oxide_sim.py`'s runtime role, `gen_dials.sh`, the gitignored 27–70 MB `dial*.raw2`, and the "regenerate before rendering" gotcha | 3–5 days |
| B2 | ~~`swept_band_geometry`~~ — shipped instead as the fully GENERAL `sweep_geometry` (arbitrary closed profile × 3D path, RMF, taper, caps) + `path_instances_geometry` (any template geometry stamped along a path); the strap-specific profile/stitch knowledge became authored scene data | `strap_mesh_gen.py`, `strap.raw2`, `strap_stitch.raw2` | 2–3 days |
| B3 | **`> set active_camera <name>`** command | `render_watch_views.py` (entirely), simplifies the turntable script | hours |

Both B1 and B2 follow existing, well-trodden extension patterns
(function2d-painter family; descriptor-driven geometry chunk + the same
5-build-project checklist the SDF feature just exercised).  Live GUI editing
of dial pattern/cell size/torch window — today a re-bake + reload — becomes a
slider.

### Prong C — embed a general-purpose language (REJECTED)

Lua/JS/embedded-Python would cover everything in one stroke, but:
scenes stop being declarative data (the GUI/save-engine/suggestion machinery
all assume introspectable chunks); parse becomes execution of arbitrary code
(reproducibility + security posture changes); it adds a dependency and a
second way to do everything the parser already does.  The repo's whole parser
philosophy (descriptors as the single source of truth) argues against it.

## 5. What stays external (honestly)

- `render_contact_sheet.py` — image montage assembly is not a renderer job.
- `tools/migrate_*.py` — one-shot migrations.
- `thermal_oxide_sim.py` — keep as the *reference* simulation that validated
  the oxide painter's analytic approximation (same pattern as the thin-film
  TMM oracle vs the in-renderer evaluator).

## 6. Suggested sequencing

1. **B1** (guilloché + oxide painters) — highest leverage: deletes three
   bakers + the bake step + the big meshes, and unlocks live dial editing.
2. **A1 + A2** (in-chunk FOR + math fns) — retires `sdf_gen.py`; scene text
   becomes the single source of truth for all SDF parts.
3. **B2** (swept band) — retires `strap_mesh_gen.py`.
4. **B3** (active_camera command) — retires `render_watch_views.py`.

Total: roughly 2–2.5 focused weeks, each stage independently shippable with
its own tests, no scene-format version bump required (all changes additive;
existing scenes parse unchanged).

---

## 2026-06-12 — in-scene field scripting SHIPPED (`expression_function2d`)

The "patterns can't be authored in the scene, only composed from fixed
C++ primitives" gap is closed.  `expression_function2d` is a small
math-expression evaluator (recursive-descent compile to a postfix stack
machine; `param`/`def`/`expr`; sin/cos/atan2/floor/mod/clamp/smoothstep/
select/...) over (u, v) — the in-scene analogue of a C++ field painter.
With general displacement (`displaced_geometry` + the new `uv_seam_fold
FALSE` opt-out for open Cartesian fields) and `cartesian_disk_geometry`
(a flat Cartesian-UV base), a guilloché dial is authored ENTIRELY in the
scene: `cartesian_disk_geometry` + a guilloché `expression_function2d` +
`displaced_geometry`.  Proven: the uniform guilloché expression
reproduces `GuillocheField::Height` to 1e-6 (tests/ExpressionFunction2DTest)
and renders identically to the retired `guilloche_disk_geometry`.  The
engine is hardened against untrusted scene text (bounded parse recursion,
compile-time value-stack bound, no per-eval heap, ffast-math-safe finite
guard).  **2026-06-12 follow-up — migration COMPLETE:** all six watch dial
patterns (uniform/lightning/radial/iris/swirl/varwidth) are now authored
as in-scene `expression_function2d` chunks (proven == the original C++
relief to 1e-6 across a 52k-point sweep), the watch + temper scenes are
migrated, and `guilloche_disk_geometry` (chunk + factory + IJob/Job + the
C++ relief math in `GuillocheField`) is EXCISED — `GuillocheField` keeps
only its oxide/thermal physics, which the heat-tint painter still uses.
The six patterns live as templated builders in `tests/GuillocheDialExpr.h`,
the single source the equivalence test and a chunk-text emitter share.
Remaining future work: an in-chunk `FOR` for repeated geometry (the
sdf_gen.py case).
