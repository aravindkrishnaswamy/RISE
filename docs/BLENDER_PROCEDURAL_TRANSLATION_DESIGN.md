# Blender Procedural Shader Translation — Design (DRAFT 6)

**Status:** draft 6, 2026-11-14. Nothing here is implemented except
constant-reflectance GGX and energy-preserving Oren-Nayar (2026-11-19, §5.6),
the first four groups of procedural builtins -- gradient noise, fractal
noise, the Noise Texture node and White Noise (2026-11-20), the Voronoi
Texture node (2026-11-22), and the colour conversions and Mix Color node
(2026-11-22), §5.2(b) -- and the companion's step P
(2026-11-16, DL-633). Draft 4 was
reviewed by four independent reviewers
([round 1](BLENDER_TRANSLATION_DESIGN_REVIEW_ROUND1.md)) and draft 5 by a
second model and harness
([round 2](BLENDER_TRANSLATION_DESIGN_REVIEW_ROUND2.md)). Round 2's verdict:
the transport features (material mixing, shadow visibility, ambient
occlusion, the program cache) must not be started as written; the procedural
kernels, graph inspection and performance measurements may. This draft
answers round 2 and records the owner's decisions (§10). The largest
changes: a new thin diffuse-transmission lobe for Blender's Translucent; a
mix child may carry its own bump; declaration and refusal of path-dependent
inputs move into Phase 1, ahead of the first such input; the coverage rules
no longer count refused or unmapped shaders; cache keys cover all external
state; and Phase 1's fit in the existing evaluator is to be measured, not
assumed.
**After round 3 (2026-11-16).** A third review
([round 3](BLENDER_TRANSLATION_DESIGN_REVIEW_ROUND3.md)) found fourteen new
P1 defects in this draft and the companion's draft 3. The owner then decided
to stop iterating on prose for mixing and shadow visibility: they are to be
prototyped against failing closed-form tests, and the next review is of code
and tests. **This document is therefore not redrafted.** Read it with these
corrections:

- **Superseded, do not build from the text:** §5.7, §5.8 and every
  statement about `mix_material`, shadow visibility, ray context, `ao()` and
  the Phase 2 program cache. The round-3 record lists what is wrong with
  each.
- **§5.2(c):** the ramp lookup does not map infinities to 0. Follow
  `kernel/svm/ramp.h` exactly: with extrapolation on, an out-of-range input
  extrapolates first; otherwise the input is clamped to [0, 1], so +∞ reads
  the last entry.
- **§5.4:** `InlineShaderNodes.from_material` always targets "All" outputs.
  The exporter works on a temporary copy of the material whose chosen output
  is the only one, retargeted to "All". Mapping's unlinked Vector is the
  socket's own value, not zero. The inliner does unroll closure zones and
  constant-count repeat zones (open question 3 is answered).
- **§5.4, normals (owner decision):** Geometry and Texture Coordinate
  normals follow Cycles on a back face: flipped to face the arriving ray. On
  a material that transmits light they are view-direction inputs and
  path-tracer-only (87 corpus materials).
- **§5.5 (owner decision):** a Bump or Normal Map output wired into a value
  socket needs the perturbed normal as a value in the program. The survey
  now measures it: **243 materials (10.0%)**, so it is built, not left to a
  fallback. A linked Bump or Normal Map strength (84 materials) needs a
  modifier scale that is a painter; both are new Phase 1 and Phase 2 work
  items that draft 6 did not list.
- **§5.6:** in Blender 4.3 to 5.1 (and the 5.2 alpha `main` of 2026-03-17;
  the 5.2 release was not checked) the Diffuse multiscatter term scales as
  albedo cubed (`sqr(albedo)` in `bsdf_oren_nayar.h`, times the closure
  weight; introduced with the energy-preserving model in 4.3), 4.2% darker
  in white-sky albedo (5.8% at normal view) than the published model at
  albedo 0.5 and roughness 1, by the closed form. Blender 5.3 uses `albedo`
  and matches the published model. RISE implements the published model: a
  double-precision transcription of 5.3's formula agrees with it to 5e-15
  (relative) over 5 colours (0.2, 0.5, 0.8, 1 and 1.3; above 1 RISE follows
  Cycles' saturated form, which the published model does not define), 3
  roughnesses and 36 direction pairs (`tests/BlenderLobeAdditionsTest.cpp`).
  The lobe parity gate (§7 item 6, not yet built) is to record the
  difference in 4.3 to 5.1 and the 5.2 alpha. `diffuse_transmission_material` needs the contract lines of
  round 3 P1-13 before it is built. Camera visibility is not a step-0 item
  (round 3 P1-9).
- **Bridge ABI:** v19 for step 0, v20 for Phase 1, v21 for Phase 2.
- **§5.2(a) and §8, Phase 1's fit in the existing VM, now measured**
  (`tools/blender_graph_size.py`, 5,982 value programs in 1,832 corpus
  materials, against the VM's 496 free variable slots):
  - A compiler that names only node outputs used more than once, and
    inlines the rest, never exceeds the budget: the largest program needs
    173 slots (median 3, 99th percentile 121). One multi-output program per
    material (Phase 2) needs at most 191.
  - A compiler that names every node output would overflow on 410
    programs (6.9%) in 141 materials (7.7%); the largest would need 931.
    So inlining single-use outputs is a requirement of the compiler, not
    an optimisation.
  - Node chains run to 255 nodes deep (99th percentile 77), so the
    compiler must also break long inlined chains with a named value: the
    parser refuses an expression nested more than 200 deep
    (`kMaxParseDepth`), before the 512-entry value stack is reached.
  - Limits of the measurement: it counts node outputs, not the
    temporaries a node's own lowering adds; and the 65 materials with a
    repeat or closure zone are counted as authored, not unrolled.
- **§9 below is updated** to the corrected survey (`survey_json7`).

**Scope:** how the Blender add-on should carry shader node graphs (textures,
converters, coordinates, attributes, bump, and shader mixes) into RISE, and
which general RISE features that needs.
**Companion:** [MIX_MATERIAL_PATH_STATE_AO_DESIGN.md](MIX_MATERIAL_PATH_STATE_AO_DESIGN.md)
(material mixing, path dependence, ambient occlusion).
**Related:** [BLENDER_MATERIAL_TRANSLATION.md](BLENDER_MATERIAL_TRANSLATION.md)
(what the bridge does today), [RELIEF_MODIFIER_DESIGN.md](RELIEF_MODIFIER_DESIGN.md),
[ROADMAP.md](ROADMAP.md) §2.3 (the "translate Noise / Voronoi / Checker onto
existing painters" item, which this design replaces).
**Evidence:** `tools/blender_node_survey.py` (collects and reports; its
`CAPABILITY_RULES`, `PLAN`, `PATH_TRACER_ONLY`, `INEXACT_RULES` and
`GRAPH_INEXACT_RULES` tables are the machine-readable form of §9).

## 1. Summary

Today the bridge carries **image textures feeding a single Principled BSDF**.
Every other node is walked through and ignored (Color Ramp, Hue/Saturation,
RGB Curves), collapsed to one branch (Mix), or dropped to the socket default
(Noise, Voronoi, Math). Materials that are not a single Principled BSDF are
gated on a manual bake that replaces them with a metallic-0 PBR built from
three baked channels.

On a 2,418-material corpus that is faithful for at most **21%** of materials
and **2.5%** of distinct material structures, and three quarters of that 21%
are a bare Principled BSDF with nothing connected.

The proposal:

1. **Compile the node graph, do not bake it.** The exporter compiles each
   material's graph into RISE *expressions*, evaluated per shading point by
   the expression VM.
2. **Teach the VM Cycles' vocabulary** as ordinary builtins: Cycles' gradient
   noise and fractal types, Voronoi, white noise, colour-mix modes, lookup
   tables, safe math, and space transforms. These are ports of the Cycles
   kernel functions, so patterns agree with Cycles (§7 says how closely).
3. **Translate the shader level too.** Half the corpus is not a single
   Principled BSDF: it is Diffuse, Glossy, Transparent, Subsurface and
   Emission nodes joined by Mix and Add Shader. Single BSDFs map onto RISE's
   own physical materials, three small additions among them; mixes need a
   new general `mix_material`.
4. **Add the scene data the graphs read:** per-object attributes and named
   per-corner attributes.
5. **Keep baking only as a per-socket fallback**, from Phase 1, so an
   unsupported node costs one input its fidelity, not the whole material its
   lobes.

**Coverage by phase** (§9), share of materials:

| | today | step 0 | phase 1 | phase 2 | phase 3 | phase 4 |
|---|---|---|---|---|---|---|
| Graph translated, under the path tracer | 21% | 27% | 45% | 75% | 86% | 90% |
| … and every lobe also matches Cycles | 20% | 25% | 40% | 60% | 63% | 67% |
| Graph translated, under the path tracer, BDPT and VCM alike | 21% | 27% | 44% | 62% | 65% | 67% |

The gap between the first two rows is RISE's lobe models differing from
Cycles' (subsurface above all); the gap between the first and third is
material that renders under the path tracer only, mostly inputs defined only
along a path from the camera (view direction and Light Path). About 10% of
materials use something no phase covers. These are projections from the
survey's rules; none is yet confirmed by compiling a material (§2.4).

## 2. Problem and evidence

### 2.1 What happens to a graph today

| Graph | What RISE renders |
|---|---|
| Image Texture (UV, flat, repeat) into Principled | The image. Correct. |
| Image → Color Ramp / Hue-Sat / RGB Curves / Gamma / Invert | The raw image; the adjustment is dropped silently. |
| Mix of two branches | The A branch's image only; factor and B are dropped silently. |
| Noise / Voronoi / Checker (any path) | The socket default, with a warning. |
| Math, Map Range, Separate/Combine | The socket default, no warning. |
| Bump with a procedural height | No bump, with a warning. |
| Texture Coordinate other than UV | Image rejected, socket default, with a warning. |
| Mix Shader / Add Shader / any non-Principled surface | "Complex": the render is gated until the user bakes. Unbaked, the first Principled in node order is used, or a flat viewport colour if there is none. |

The manual bake has structural limits that no tuning removes:

- It writes Diffuse colour, Roughness and Normal only, and the consumer
  builds a metallic-0 PBR from them. Metallic, transmission, coat, sheen,
  emission, subsurface and alpha are lost.
- It bakes in UV space on one proxy object, so it cannot carry per-instance
  variation and needs an unwrap.
- It must be re-run after edits and per frame for animated graphs.
- It fails outright when the material lives only on a hidden object that
  geometry nodes instance into the scene ("No valid selected objects").

### 2.2 The corpus

68 files from Blender's public demo-file mirror (5.3 GB: the Cycles and EEVEE
demo scenes, twelve splash scenes from 2.90 to 5.3, a Sprite Fright
production shot, the geometry-nodes demos, classic benchmark scenes) plus six
files already on this machine. Unpacked: 177 .blend files, saved by Blender
2.66 through 5.3 (178 attempted; the 3.6 splash's main file did not load
within the time limit, its asset libraries did).

3,885 material instances; **2,418 distinct materials** (same name and same
feature set counted once); **1,020 distinct structures** (same feature set
counted once, whatever the name). Materials with no surface shader (29
instances, mostly volume-only) are excluded.

Three coverage measures are reported throughout, because a handful of
productions contribute hundreds of near-identical character materials:

- **materials** — share of the 2,418 distinct materials;
- **structures** — share of the 1,020 distinct structures;
- **per-file** — mean of each file's own percentage (94 files with five or
  more materials).

The corpus is Blender Foundation content. It leans toward stylised production
work built on large shared node groups; user scenes with hand-wired materials
probably resemble its smaller files more. Where the three measures disagree,
that is the reason.

### 2.3 What the corpus shows

**Share of materials needing each capability** (a material usually needs
several):

| Capability | Materials | Main contents |
|---|---|---|
| Converters | 65.1% | Mix 47%, Math 38%, Vector Math 32%, Map Range 31%, Color Ramp 29%, RGB Curves 25%, Separate Color 24%, Hue/Sat 17%, Vector Rotate 16%, Float Curve 16%; an image's Alpha output 6% |
| Non-Principled BSDFs | 47.7% | Diffuse 26%, Transparent 15%, Subsurface 13%, Emission 9%, Glossy 6%, Translucent 6% |
| Procedural textures | 42.2% | Noise 38%, Voronoi 21%, White Noise 20%, Gradient 3%; Magic, Wave, Checker under 1% each |
| Coordinates (other than Generated) | 41.3% | Geometry Normal 15% / Position 8%, Object 13%, Normal 12% |
| Shader mixing | 40.0% | Mix Shader 38%, Add Shader 7% |
| Node groups | 38.3% | today any group node makes a material "complex" |
| Per-vertex attributes | 26.8% | Attribute (Geometry) 24%, Color Attribute 2% |
| Bump beyond an image height | 25.7% | height from Math 6%, Noise 4%, Mix 2%; chained 6%; linked Strength or Distance 3.5% |
| **View direction** | 24.5% | Geometry Incoming 12.7%, Layer Weight 7.6%, Backfacing 5.4%, Fresnel 1.2% |
| Generated coordinates | 24.1% | half explicit, half an unlinked texture Vector |
| Image sampling beyond plain UV | 23.2% | computed coordinates 18%, cubic interpolation 8%, non-UV 4%, box projection 4% |
| Per-object attributes | 22.5% | Object Info Random 18%, view-layer attributes 14% |
| Camera-space vectors | 17.2% | Vector Transform 17%, Camera coordinates 12% |
| A UV set other than the render-active one | 15.3% | two sets 9.4%, a single non-active set 5.9% |
| Lobe options RISE lacked, in scope (added 2026-11-19, §5.6) | 7.0% | a tinted Glossy 4.9%, Diffuse roughness 2.2% |
| Ambient Occlusion | 6.4% | |
| Light Path | 6.2% | Is Shadow / Camera / Diffuse Ray |
| Image sources | 5.3% | UDIM 4.2%, generated (unsaved) images 1.1% |
| True displacement | 3.4% | |
| A refracting shader inside a mix | 2.2% | refused (companion §I.3) |
| A subsurface leaf under a mix weight that varies over the surface | 14.2% | path-tracer-only (companion §I.3); 278 of the 344 read the view direction too |
| Mix leaves with different Normal inputs | 4.0% | a modifier per mix child (companion §I.2) |
| Three or more UV sets | 1.9% | |
| Hair and particle inputs | 1.2% | |
| Brick, Gabor, Radial Tiling | 0.8% | |
| Not planned | 5.9% | Shader to RGB 2.4%, closure nodes 2.1%, repeat zones 0.6%, Raycast / Ray Portal 0.5%, Bevel 0.5%, volumes, Toon, Holdout |

**Lobes that translate but do not match Cycles** (29.8% of materials use at
least one):

| Lobe | Materials | Difference |
|---|---|---|
| Subsurface (the node, or Principled with Subsurface above 0) | 21.2% | the bridge's model is an approximation and takes an untextured colour |
| Rough or tinted Glass / Refraction | 4.4% | RISE's dielectric has no microfacet transmission and no interface tint |
| Beckmann or Ashikhmin-Shirley distribution | 4.2% | rendered as GGX |
| Specular BSDF | 1.2% | an EEVEE node; Cycles does not render it, so there is nothing to match |
| Refraction BSDF | 0.9% | Cycles' has no reflection lobe |
| Sheen BSDF | 0.2% | rendered with RISE's Charlie sheen, not Cycles' microfiber or Ashikhmin |
| Metallic BSDF with an edge tint | 1 material | the tint is not modelled |

**Graph-level differences that are disclosed, not refused** (23.2% of
materials use at least one): the pattern is translated but its values are
not Cycles' pixel for pixel.

| Node | Materials | Difference |
|---|---|---|
| White Noise | 19.6% | same distribution, different values (§5.2b) |
| Cubic image filtering | 8.3% | rendered bilinear (§5.2e) |
| Ambient Occlusion | 6.4% | RISE's estimate is fixed per point; Cycles redraws it per path (companion §III.3) |

**What the corpus changed about the plan:**

1. **Shader-level translation is not a late phase.** About 150 materials need
   nothing but a single non-Principled BSDF mapped directly, and 40% contain
   a Mix or Add Shader. Scenes from before the Principled BSDF (the classroom,
   BMW and Barcelona pavilion benchmarks) score 0% until this exists.
2. **Graphs are large, but not the ones Phase 1 covers.** Median 8 nodes,
   75th percentile 36, 90th 205, 99th 475, maximum 647; 18% of materials
   exceed 100 nodes. Restricting Phase 1 to graphs of at most 150 nodes
   loses 0.2 points (47.7% → 47.5%): the large graphs are the ones that also
   need shader mixing. So Phase 1 is planned on the existing expression VM,
   and the evaluator question belongs to Phase 2 (§5.2). A node count does
   not bound compiled size, though; whether Phase 1's graphs fit is measured
   first (§8).
3. **Per-vertex attributes need more than four channels.** Of the 647
   materials reading them, 112 read five or more distinct attributes.
4. **Capabilities are conjunctive.** No material needs only procedural
   textures, only coordinates or only object attributes. Phase 1 therefore
   has to be broad.
5. **Voronoi, White Noise and Vector Math matter**; Brick, Wave, Magic and
   Checker do not (under 1% each).
6. **A quarter of materials read the view direction**, which constrains the
   integrator (§5.8).

### 2.4 How far to trust these numbers

The survey has been audited twice. What is known to be soft:

- **"Today" is an upper bound by node type.** It assumes the existing image
  path handles every option of the nodes it accepts. 389 of the 501
  materials counted as covered today are a bare Principled BSDF with no
  image at all.
- **Phase 1 is a range.** Generated coordinates are exact only on undeformed
  meshes, and the survey does not record deformation. If no
  Generated-coordinate material qualified, Phase 1 would be 42.6%.
- **"Matches Cycles" is an upper bound.** It excludes the lobe options in
  the table above. It does not see: Cycles' single-scatter GGX against
  RISE's energy-compensated one at high roughness; constant Principled
  settings other than Subsurface and Transmission; premultiplied image
  alpha. A reviewer who hand-read 30 graphs found 12 fine, 5 needing a
  specification fix and 12 visibly wrong at their claimed phase under
  draft 4's rules, which is what prompted the second and third coverage
  rows.
- **Round 2 hand-read 13 materials of one file** (BMW27) and found two
  counted as translated that were not: a mix whose leaves have different
  normals, and a Sheen leaf with no stated mapping. Both now have survey
  keys and a stated treatment; the general lesson is that the rules have
  been checked against few graphs.
- **Refusals are subtracted only where the survey can see them.** A
  refracting shader as a direct mix input is counted as refused; one hidden
  inside a group that feeds the mix is not (125 materials have a mix leaf
  that arrives through a group boundary). A subsurface leaf in an
  energy-gaining Add is refused but not subtracted (at most 8 materials).
- **Coverage is per material.** Object-level restrictions, such as ray
  visibility masks that make an object path-tracer-only, are not counted.
- **Phase 1's fit in the existing evaluator is unmeasured** (§8).
- **Not recorded:** whether a mesh is deformed, how many Fresnel-weighted
  mixes are the classic diffuse-plus-glossy blend, whether a Fresnel or
  Layer Weight node takes a bumped normal.
- **Node counts are upper bounds:** inside a group every branch is counted,
  used by the caller or not.
- **Nothing has been compiled.** Every figure is a projection from feature
  keys. The corpus compile test (§7) is what turns it into a measurement.
- **The corpus is one publisher's content.** 79 of the 176 loadable files
  are EEVEE scenes, and 12 files hold 53% of the distinct materials. World
  shaders are not surveyed.

## 3. Goals and non-goals

**Goals**

- **Translate the graph.** A material built from the supported node set
  renders in RISE with the pattern its graph describes, at every object,
  instance and frame, with no bake step and no UV requirement.
- **Lobes are RISE's own physical models.** Each Blender shader maps to the
  RISE material that represents the same physical surface. Where RISE's
  model differs from Cycles' the difference is documented, and reported as
  the gap between the first two coverage rows; it is not hidden by imitating
  Cycles' model.
- Every lobe RISE already bridges keeps working when its inputs are
  procedural. (Some bridged inputs are numeric-only today, such as coat IOR
  and the subsurface coefficients; §5.6 lists them.)
- An unsupported node degrades one input, by a per-socket bake, with a
  warning naming the node. Nothing is dropped silently.
- Everything added to RISE is a general scene-language feature, usable from
  a hand-written `.RISEscene`.
- Coverage is measured, per phase, on the corpus, by the survey tool.

**Non-goals**

- Matching Cycles lobe for lobe. Three small additions are in scope because
  they are sound models RISE lacked (a Fresnel-free tinted glossy mode, an
  energy-preserving diffuse roughness and a thin diffuse transmitter, §5.6).
  Microfacet transmission, interface tint, Beckmann and a textured-colour
  subsurface model are separate projects.
- Path-dependent inputs (Light Path, view direction) under BDPT or VCM, and
  any of the new mixing materials under MLT, which is frozen.
- Native translation on Blender older than 5.0 (§5.4); those versions keep
  today's bake path.
- Arbitrary OSL script nodes; EEVEE-only shading (Shader to RGB).
- Volume shader graphs and world shader graphs beyond what exists.
- Bit-exact equality with Cycles (§7 defines the tolerance).

## 4. Options considered

| Option | Verdict |
|---|---|
| **A. Compile to RISE expressions; extend the VM with Cycles-compatible builtins** | **Chosen for Phase 1.** The expression VM already has scalar and vec3 types, named sub-expressions, scene-file round-trip and GUI editing; its noise builtins accept an arbitrary vec3, so coordinate warping composes; and Phase 1's graphs fit it as it is. |
| C. A separate node-graph executor behind the same language | **Open, to be decided by measurement before Phase 2** (§8). The large graphs of Phase 2 need multi-output programs, storage beyond 512 slots, a real per-hit cache and multi-result builtins. Review found that fitting those into the existing evaluator is not "two structural changes": its bodies are byte-pinned for floating-point reproducibility, its scalar function-id band is full, its builtins return one value, its context mask is 32 bits and its programs are copied by value. A flat register executor that the same program text lowers to would leave the existing path byte-identical. |
| B. One RISE painter class per Blender node, wired by name | Rejected. No RISE painter can take another painter's output as its coordinate; 18% of materials feed images computed coordinates and 6% distort noise. Graphs of 200+ nodes would become 200+ named painters each. |
| D. Embed OSL and run Cycles' own `.osl` node shaders | Rejected. Exact by construction, but adds OSL and LLVM to five build systems (including Android), and the node shaders still lean on OSL built-in noise and host attribute callbacks. |
| E. Export MaterialX and import that | Rejected for this purpose. RISE has no MaterialX runtime yet, Blender's export covers a subset, and MaterialX noise is not Cycles noise. The builtins here would later serve a MaterialX importer. |
| F. Improve the bake | Rejected as the main path: it cannot carry view-dependent inputs, per-instance variation or animation. Kept, reshaped, as the fallback (§5.9). |

## 5. Design

### 5.1 Architecture

```
Blender node tree
   │  Blender's own inliner (groups, muted nodes, reroutes)
   │  graph compiler (new; bpy-free core: node_compile.py)
   ▼
expressions: one per material input in Phase 1;
one multi-output program per material from Phase 2
   │  bridge ABI v19
   ▼
RISE expression painters (existing), later program views
   │
   ▼
existing material slots (painter names); relief_modifier; mix_material
```

The exporter already passes painter *names* into every material slot, so the
transport is expression text. Routing follows RISE's existing rule:
reflectance and tint inputs use the colour pipe (spectrally uplifted);
roughness, metallic, IOR, weights and bump height use the scalar pipe.

### 5.2 Expression VM changes

**(a) Evaluation, in two stages.**

*Phase 1: the existing VM, unchanged in structure.* Each material input is
one expression painter. A sub-graph shared by two inputs is evaluated once
per painter; at Phase 1's graph sizes (median 8 nodes; limiting Phase 1 to
graphs of at most 150 nodes loses 0.2 points of coverage) that is expected
to be acceptable, and is measured (§8). Nothing in the evaluator's pinned
bodies changes; new builtins are added through a separate dispatch.

- **Whether the graphs fit is a measurement, not an assumption.** The VM has
  512 scalar slots; a vec3 takes three, context variables take more, and one
  node lowers to several operations and conversions. A lowering pass over
  the corpus's Phase 1 graphs, counting slots and instructions, runs before
  Phase 1's coverage is promised (§8). A graph that does not fit falls back
  per socket (§5.9).
- **Time.** The scalar pipe evaluates expressions at time 0
  (`ExpressionPainter.cpp:356`). Compiled programs never read `time`: node
  values are exported per frame as constants, and motion reaches a program
  only through the hit's own position. So Phase 1 is unaffected.

*Phase 2: programs with several outputs.* A 400-node character shader
feeding six inputs and four mix weights needs each node evaluated once per
hit. Draft 4 said the "existing thread-local memo" would provide that. It
does not: every `def` is evaluated eagerly today, and the memo is a 4-entry
table holding one final value per program, fixed in size. What Phase 2 needs
is specified here and built either inside the VM or as option C's executor,
whichever §8's measurement favours:

- A program is a scene object (`expression_program`): `param`, `def` and
  `out <name> <kind> <expr>` lines (kinds in (f)). Painters and scalar
  painters gain a `program <name> output <name>` form.
- A per-thread cache holds, per (program, hit), the evaluated `def`s and a
  bitmask of which are valid. An output pulls only the `def`s it depends on.
- **The cache key is everything outside the program that its result depends
  on**, computed at compile time and followed through sampled painters and
  scene queries:
  - the hit: position, UVs, normals, footprint, attributes;
  - the ray: direction, and its context flags and depth counters
    (companion §II.3a), so a camera ray and a later diffuse ray at the same
    point do not share a `ray_camera` result;
  - the object instance (object attributes, object-space transforms, Only
    Local occlusion);
  - the scene and its generation, the camera transform's generation, and
    the time (scene queries such as `ao()`, and `to_camera_point`).

  A field the program does not read is left out of its key, so a program
  that does not read the ray is not invalidated when BDPT re-aims it.
  Invalidation follows the existing memo's contract: the scene is immutable
  within a render pass and every seam that can move state bumps a
  generation (`Utilities/ExpressionMemo.h`).
- Values are evaluated into local storage and committed afterwards, so a
  `def` that calls `sample()` on another program cannot evict itself.
- Storage is sized per program. Programs are shared by reference.
- Time belongs to the program, not to the pipe, so a `def` shared by a
  colour and a scalar output is the same value under animation.
- The existing `expression_memo` switch stays, with a gate that cached and
  uncached evaluation agree bit for bit.

*Lifecycle of a program.* An `expression_program` is a named,
reference-counted scene object like a painter, immutable once built.

- A `program <name>` parameter is a typed reference, so the scene document's
  rename and remove analysis sees it; `sample()` and `sample_at()` calls
  inside a program are scanned and rewritten on rename as `sample()` is
  today.
- An incremental edit to a program chunk is refused and falls back to a full
  re-derive, as edits to painter chunks are today.
- Snapshots and time clones share the program by reference.
- The bridge ABI moves to v19 for Phase 1 (expression text, object
  attributes) and to v20 for Phase 2 (programs, `mix_material` and the
  materials beside it). New `IJob` entry points are appended, never
  inserted.

**(b) Builtins.** Ports of the Cycles kernel functions, which are Apache-2.0
except the Perlin core (BSD-3-Clause, adapted from OSL) and the smooth
Voronoi / distance-to-edge code (MIT, Inigo Quilez). RISE's own licence is
BSD-style; the copyright notices and licence identifiers are reproduced in
each ported file's header, the full licence texts are in `licenses/`, and the
macOS release copies them into the app's `Third-Party Licenses` folder.

| Group | Builtins (names: `osl_` prefix, decided 2026-11-20; rows not yet built still show working names) | Source of truth |
|---|---|---|
| Gradient noise | `osl_snoise(p, w, dims)` signed, 1D–4D. **Implemented 2026-11-20** | `kernel/svm/noise.h`: Jenkins lookup3 lattice hash, quintic fade, per-dimension scale constants, the `fmod(p, 100000)` wrap |
| Fractal noise | `osl_fractal(p, w, dims, type, detail, roughness, lacunarity, offset, gain, normalize)`, the five types by Cycles' numbering. **Implemented 2026-11-20** | `kernel/svm/fractal_noise.h`, including the fractional-detail octave |
| Noise Texture node | `osl_noise_texture(...)` (Fac) and `osl_noise_texture_color(...)` (Color): the node after its Scale, with its detail and roughness clamps, Distortion and the Color output's offsets. **Implemented 2026-11-20** | `noisetex.h` |
| Voronoi | `osl_voronoi_distance`, `osl_voronoi_color`, `osl_voronoi_position`, `osl_voronoi_w`, `osl_voronoi_radius`, each `(p, w, dims, feature, metric, scale, detail, roughness, lacunarity, smoothness, exponent, randomness, normalize)`: F1, F2, Smooth F1, Distance to Edge, N-Sphere Radius; Euclidean, Manhattan, Chebychev, Minkowski; 1D–4D; the fractal parameters. The coordinate is passed before the Scale, which the kernel applies (Position and W are divided back by it). **Implemented 2026-11-22** | `kernel/svm/voronoi.h`: PCG integer cell hashes for 2D–4D (as of Blender 5.0; the hash changed in that release), the float hash in 1D |
| White noise | `osl_white_noise(p, w, dims)` (Value) and `osl_white_noise_color(p, w, dims)` (Color), 1D–4D. **Implemented 2026-11-20** | `kernel/svm/white_noise.h`, `util/hash.h`; see below |
| Colour | `osl_rgb2hsv(c)`, `osl_hsv2rgb(c)`, `osl_rgb2hsl(c)`, `osl_hsl2rgb(c)`, `osl_mix_color(mode, t, a, b, clamp_factor, clamp_result)` for the 19 Mix modes (`NodeMix` 0..18), with the node's factor and result clamps. **Implemented 2026-11-22** | `util/color.h`, `kernel/svm/color_util.h`, `kernel/svm/mix.h` |
| Safe math | the 41 Math and the Vector Math operations with Cycles' zero-on-undefined results | `kernel/svm/math_util.h` |
| Componentwise vec3 | `min/max/abs/floor/fract/pow/clamp`, `rotate(v, axis, angle)`, Euler rotation | Vector Math, Vector Rotate |
| Space transforms | `to_object_point/vector/normal`, `to_world_*`, `to_camera_*` | Vector Transform, Camera coordinates; the camera matrix is read from the live scene, not baked as constants |

- **Several results.** Voronoi returns distance, colour and position from
  one cell search, and Noise returns a factor and a colour. Builtins in the
  VM return one value today; these are either split with the search repeated
  (Phase 1, measured) or given multi-result kernels (Phase 2). The Noise
  Texture node is split this way, into a Fac builtin and a Color builtin,
  and the Voronoi node into one builtin per output, each repeating the
  search (As built, group 3). The Noise node's Color has Fac as its first
  channel bit for bit, so a Noise node with both outputs used lowers to one
  Color call in a `def` and reads Fac as its `.x`: three fractal
  evaluations rather than four.
- **As built (group 1, 2026-11-20).** `src/Library/Utilities/OslNoise.*`
  holds the kernels; `src/Library/Painters/ExpressionOslBuiltins.*` holds the
  `osl_` signature table and an out-of-line dispatch the VM reaches through
  two appended opcodes, so no existing builtin moves. A node's dimension
  count, fractal type and normalize flag are integer-literal arguments, checked
  at compile time.
- **As built (group 3, Voronoi, 2026-11-22).**
  `src/Library/Utilities/OslVoronoi.*`, a port of `svm_node_tex_voronoi` with
  its clamps, under the same strict-IEEE contract; its feature and metric are
  integer literals too. Each output is its own builtin, so a node with several
  outputs used costs one node evaluation per output (measured costs in
  `OslVoronoi.h`). It equals a strict build of Cycles' own node exactly on all
  1092 node rows of `tests/data/osl_voronoi_golden.inc`
  (`tools/osl_golden/dump_voronoi_golden.sh`, which runs the real node entry
  point; the file also holds 72 PCG hash rows, matched exactly), including 11 rows whose high octaves leave the int32 range, where
  Cycles' conversion is undefined and the port saturates as Cycles' arm64
  scalar path does. Against the Blender-flags builds it parts in three ways:
  rounding (every output within 2.01e-7 relative to max(1, |v|) where the same
  cells are chosen, but a different nearest point on 1 row per build, so cell
  colours match only in distribution on cell boundaries); a 1D coordinate of
  -0 in an octave that runs, which Blender's `-fno-signed-zeros` build hashes
  as -0 where strict IEEE hashes +0 (8 of 12 such rows, F1, F2 and N-Sphere
  Radius; reachable, e.g. W = 0 with a negative Scale); and, for the SIMD
  build in 3D and 4D, cell ids above int32, which sse2neon maps to INT32_MIN
  where the port saturates to INT32_MAX (4 of 8 such rows). The Minkowski
  metric uses the platform's `powf`, which C libraries need not round alike,
  so the test gates the rows that call it bit for bit only on Apple arm64
  (to 1e-6 elsewhere). The port's own departures (a non-finite coordinate
  gives 0; an octave whose coordinate is not finite ends the fractal) are
  where Cycles' result is undefined (2D-4D) or meaningless (1D); an unknown
  feature or metric, which the builtins' literal checks make unreachable, also
  gives 0. `tests/OslVoronoiTest.cpp` also checks closed forms that do not use
  Cycles: at randomness 0 every feature point is an integer corner.
- **As built (group 4, colour and Mix Color, 2026-11-22).**
  `src/Library/Utilities/OslColor.*` ports `util/color.h` (`rgb_to_hsv`,
  `hsv_to_rgb`, `rgb_to_hsl`, `hsl_to_rgb`) and `kernel/svm/color_util.h` +
  `mix.h` (`svm_node_mix_color` with Cycles' 19 `NodeMix` blend modes 0..18
  and both clamp flags; legacy `svm_node_mix` plus its optional
  `NODE_MIX_CLAMP` follow-up matches `clamp_factor = 1, clamp_result = use_clamp`
  bit for bit) under the same strict-IEEE contract. Exposed as
  `osl_rgb2hsv(c)`, `osl_hsv2rgb(c)`, `osl_rgb2hsl(c)`, `osl_hsl2rgb(c)` and
  `osl_mix_color(mode, t, a, b, clamp_factor, clamp_result)` (`mode`,
  `clamp_factor`, `clamp_result` integer literals; not memo-worthy: 2.7–12.2 ns
  per call on M1 Max, measured by `tools/osl_golden/bench_color.cpp`).
  Every operation is a correctly rounded IEEE 754 operation (no
  transcendentals, no float-to-int conversions), so the port equals Cycles'
  strict-IEEE build bit for bit
  (sign of zero included) on all 1,098 rows of
  `tests/data/osl_color_golden.inc` (300 conversion rows and 798 Mix Color
  rows, dumped by `tools/osl_golden/dump_color_golden.sh` from Cycles' real
  `svm_node_separate_color`, `svm_node_combine_color` and
  `svm_node_mix_color` entry points). Against the Blender-flags builds
  (whose SIMD and scalar columns agree bit-identically on all 1,098 rows),
  only float rounding appears (FMA and `-freciprocal-math`): 251/300
  bit-exact and max relative error 1.24e-7 on conversions, 761/798 bit-exact
  and max relative error 5.07e-7 on Mix Color, with 0 sign-of-zero-only rows,
  0 non-finite disagreements, and 0 departures above 1e-5. The only departure
  from Cycles is that `MixColor` maps `mode` outside 0..18 (including Cycles'
  internal sentinel `NODE_MIX_CLAMP = 19`, unreachable from the builtin) to
  `(0, 0, 0)`, and uses IEEE `std::fmax` everywhere (Cycles' `_WIN32` build
  overrides `fmaxf` with `(a > b) ? a : b`). `tests/OslColorTest.cpp` also
  verifies independent closed forms (round-trips, primaries, all 19 blend
  formulas at t = 0 and t = 1, clamps, aliasing).
- **Precision, as built.** The kernels evaluate in strict IEEE single
  precision in the order of Cycles' scalar path, compiled that way on every
  platform (each kernel file refuses to build under fast-math; built and
  run so far on macOS arm64 only), so the pattern is the same bits
  everywhere by construction (except Voronoi's Minkowski metric, which
  calls the platform's `powf`; see the Voronoi bullet above) and equals a
  strict-IEEE build of Cycles' own sources exactly on the tested inputs,
  within the documented guards (detail at most 15, a NaN detail read as 0,
  a non-finite coordinate giving 0 -- except in White Noise, which hashes
  it as Cycles does; a Voronoi octave whose coordinate is not finite ends
  the fractal, and Voronoi cell ids saturate at int32).
  Double precision was tried first and rejected: where Cycles rounds is
  part of its pattern. Against Cycles' sources rebuilt with Blender's
  flags, on identical inputs, the fractals and the Noise Texture node's
  Fac without distortion agree to float rounding at every roughness
  tested. Elsewhere the high octaves amplify float rounding by roughly
  (roughness × lacunarity) per octave: on outputs Cycles evaluates at
  shifted coordinates (Color G and B, distortion), where it is small at
  low roughness and reaches order one at roughness 1 with high detail, and
  more above it
  -- as large as the gap between a strict build and a Blender-flags build
  of the same Cycles source, so Blender's own builds are expected to
  differ by as much (inferred). In a render every output is also
  sensitive to the float coordinate each renderer starts from, which can
  differ by one or more ulps; at roughness 1 and detail 15 one ulp moves
  undistorted Fac by up to about 1e-2 and the shifted outputs by order one at
  small coordinates, more at large ones.
  `src/Library/Utilities/OslNoise.h` tabulates the measurements by
  roughness at detail 15, with their conditions. Beyond |p| ≈ 5.4e8
  Cycles' SIMD path wraps 3D/4D coordinates differently from its scalar
  path; neither result is a meaningful pattern there.
- **Golden values** come from Cycles' own kernels compiled three ways (SIMD
  and scalar as Blender builds them, and the scalar path as strict IEEE) by
  `tools/osl_golden/dump_noise_golden.sh`, `dump_voronoi_golden.sh` and
  `dump_color_golden.sh` (Blender commit afc6d7a4, the 5.2 alpha); the
  licences are in `licenses/`.
- **White Noise matches in distribution, not per pixel.** Cycles hashes the
  raw 32-bit float pattern of its input, with no `floor`. RISE computes in
  double precision, so a last-bit difference upstream changes the whole
  output; Cycles' own CPU and GPU kernels disagree here for the same reason.
  The hash is ported and applied to the input rounded to float, which is
  exact when the input is itself exact in float (object random, integer
  cell ids) and statistically equivalent otherwise. 20% of materials use it.
  As built, it is integer arithmetic on the float bit patterns, so it equals
  Cycles exactly on equal float inputs: all 200 golden rows, which include
  ±0 (different patterns, different values, as in Cycles), the infinities,
  a subnormal and the largest float.
- **No footprint fade.** Unlike RISE's existing `fbm()`, the new noise does
  not fade octaves by pixel footprint: Cycles does not, and the pattern is
  the point. Hand authors should prefer `fbm()`; the documentation says so.
- Gradient, Checker, Wave and Magic need no builtins; they are arithmetic.

**(c) Lookup tables.** Cycles evaluates Color Ramp, RGB Curves, Vector
Curves and Float Curve as 257-entry tables filled by Blender. The exporter
fills the same tables with Blender's own `evaluate()` and the program reads
them with `lut(name, t)`. Each table declares what Cycles' lookup does with
it:

- **interpolate or not.** A Constant-mode ramp is looked up without
  interpolation (`kernel/svm/ramp.h:42–52`); with interpolation it would
  turn each step into a 1/256-wide slope.
- **extrapolate or clamp**, and the **input range**: curves extrapolate
  beyond their range by default (605 of 606 corpus curve materials).
- NaN and infinite inputs are mapped to 0 before the index is formed.

**(d) Inputs.** New read-only inputs, implemented as zero-argument builtins
that read the hit (the mechanism `sample()` uses), not as context slots: the
context-variable mask is 32 bits wide and the proposals would overflow it,
silently misclassifying programs.

| Input | Notes |
|---|---|
| `Ng`, `u1`, `v1` | geometric normal; the second UV set |
| `N0` | the shading normal **before** modifiers. Blender's Geometry and Texture Coordinate normals are unperturbed, where RISE's `N` in a program is the bumped normal |
| `I`, `backfacing` | **view direction: path dependent** (§5.8). Valid under the path tracer only. They ship only together with the declaration-and-refusal step (companion step D1), never before it |
| `ray_*` | Light Path (companion part II) |

Every new input is specified for hand-built records too: sampled-light
records, BDPT/VCM rebuilt vertices and relief taps each state what they
carry, in the manner of the existing contract in `PathVertexEval.h`.

**(e) `sample_at(name, uv)`.** `sample()` reads a painter at the caller's
own hit. An image whose coordinates are computed, projected or box-projected
needs the image evaluated at a coordinate the program supplies.

- The hit's texture footprint is invalid at a computed coordinate (a ×10
  Mapping would select a mip ten times off), so `sample_at` samples
  bilinearly with no footprint, as Cycles does.
- `sample_alpha_at` returns the image's alpha (6% of materials use an
  image's Alpha output). Cycles returns colour premultiplied unless Alpha is
  linked; the compiler reproduces that.
- Extension modes (repeat, clip, extend, mirror) are applied to the
  coordinate in the program. Cubic interpolation (8% of materials) is
  rendered bilinear and counted as a known difference.
- The reference scanner that keeps `sample(name)` visible to rename and
  remove is extended to `sample_at`, and the rule that refuses sampling a
  spectrally defined painter covers it.

**(f) Output kinds.** What a compiled colour means must be declared, because
RISE's RGB and spectral paths treat out-of-range colours differently: RGB
keeps channels above 1, the spectral uplift clamps to [0, 1] and returns
exact white near 1. A computed (2, 1.5, 1) would be an energy-gaining tint
in RGB and white in spectral. Each output has a kind:

| Kind | Meaning | Treatment |
|---|---|---|
| `reflectance` | an albedo or tint | clamped to [0, 1] per channel, as Cycles clamps the socket; uplifted as a reflectance |
| `radiance` | emission | non-negative, unbounded; uplifted as an illuminant |
| `scalar` | roughness, weight, IOR, height | never uplifted; clamped to the slot's range |
| `vector` | a normal or coordinate | never uplifted |

**(g) Working colour space.** HSV, luminance, ramps and curves depend on the
primaries. The compiler requires the Blender scene's working space to be
linear Rec.709 (every corpus file is) and otherwise falls back to the bake
for colour sub-graphs, with a warning.

### 5.3 Scene data

**(a) Object attributes.** RISE has no per-object user data. Add a named
attribute table on objects (scalar and vec3), settable from the scene
language and read in programs as `objattr(name)` / `objattr3(name)` with a
declared default.

| Attribute | Value |
|---|---|
| `random` | Cycles' Object Info Random (18% of materials): `random_id / 0xFFFFFFFF`; `random_id` from the depsgraph instance, or for a non-instance `hash_uint2(hash_string(name), 0)` (a short port from `util/hash.h`) |
| `location`, `color`, `alpha`, `index` | the other Object Info outputs |
| `texspace_loc`, `texspace_size` | Generated coordinates |
| object and instancer custom properties | Attribute node, Object and Instancer types |

View-layer attributes (14% of materials) are constants for a render and are
folded by the compiler.

**(b) Named mesh attributes.** Meshes carry one unnamed vertex-colour layer.
The corpus needs named layers without a small fixed count (112 materials
read five or more).

- The exporter reads attributes from the **evaluated** mesh, after modifiers
  and geometry nodes: 397 corpus materials read attributes that exist only
  there. It converts every domain (point, face, face corner; float, integer,
  colour) to per-corner vec3, which is what RISE's mesh already interpolates.
- Layers are stored on the mesh and interpolated on demand: the hit record
  already carries the primitive index and barycentrics in its signal block
  (`ISurfaceSignalProvider.h`), on rebuilt BDPT/VCM vertices too. `attr3(name)`
  asks the geometry for that layer at that point; a second accessor returns
  the attribute's alpha.
- Relief taps hold the signal block fixed, so an attribute has no gradient
  across them. Cycles shifts attributes in its bump evaluations. Bump driven
  by a mesh attribute therefore needs the taps to re-interpolate; specified
  with the feature, in Phase 2.
- Extra UV sets beyond the second, Random Per Island and Pointiness ride the
  same mechanism.

**(c) `relief_modifier` strength.** Cycles' Bump output is
`normalize(strength·N′ + (1−strength)·N)` with `N′ = normalize(N − distance·∇h)`.
`relief_modifier` computes `N′` with `scale = ±distance`; it needs an
optional `strength` blend. Today's exporter folds Strength into the scale,
which is right only when Strength is 1.

### 5.4 The graph compiler

A new module, `node_compile.py`, with a bpy-free core so it is unit-tested
like `light_math.py`.

- **Flattening is Blender's.** Cycles 5.x calls Blender's own inliner before
  compiling a shader, and Python can call it too
  (`bpy.types.InlineShaderNodes`, in 5.0 and later). It resolves groups,
  nested groups with unlinked inputs, muted nodes and reroutes the way
  Cycles sees them. The compiler runs it on a temporary copy of the
  material (the call returns an empty tree for materials with
  engine-specific outputs unless the copy's output is retargeted). Whether
  it also unrolls closure nodes and repeat zones, as the source suggests, is
  to be validated; if so, 2.7% of materials move out of "not planned".
- **Native translation needs Blender 5.0.** The inliner and the current
  Voronoi hash both date from 5.0. On 4.x the add-on keeps today's
  behaviour.
- **Values come from the evaluated depsgraph**, so animated and driven node
  values (95 corpus node trees) export at the current frame.
- **Method:** depth-first walk from the active Material Output. Each node
  output becomes one `def`; each material input becomes one output. Implicit
  conversions follow Cycles (colour → float is luminance; float → vector
  broadcasts; vector → float averages). Nodes are identified by their RNA
  pointer, never by Python `id()`.
- **Constant folding:** a sub-graph with no varying input is evaluated in
  Python and emitted as a literal. A Mix whose factor is constant 0 or 1
  is replaced by the branch it selects, which removes dead branches and
  their requirements.
- **Images:** an Image Texture node is a registered image painter read with
  `sample()` or `sample_at()`.
- **Sharing:** identical expressions are registered once.

**Coordinates.**

| Blender | Program |
|---|---|
| UV (render-active map) | `vec3(u, v, 0)` |
| a second named map | `vec3(u1, v1, 0)`; further maps via `attr3` |
| Object | `Po`; with an Object picked on the node, world `P` through that object's inverse matrix |
| Generated | `(Po − texspace_loc + texspace_size) / (2·texspace_size)` from object attributes. Exact for undeformed meshes; a deformed mesh reads original coordinates from a mesh attribute |
| **An unlinked texture Vector** | per node, as Cycles defines it: Generated for Noise, Voronoi, Gradient and the other procedural textures; **UV for Image Texture; a constant zero for White Noise and for Mapping** |
| Normal | object-space `N0` |
| Camera | `to_camera_point(P)` |
| Window | the camera projection of `P` to normalised device coordinates |
| Geometry: Position, Normal, True Normal | `P`, `N0`, `Ng` |
| Geometry: Incoming, Backfacing; Reflection; Layer Weight; Fresnel | from `I`: path dependent (§5.8) |
| Mapping | affine expression; linked Location / Rotation / Scale are more `def`s |

### 5.5 Bump and displacement

Cycles evaluates the height sub-graph three times: at the hit and offset by
the ray differentials times the Bump node's **Filter Width**, a per-node
input whose default is 0.1 pixel and which files saved before Blender 4.4
are versioned to 1.0 (445 of 446 corpus bump materials read 1.0).
`relief_modifier` does the equivalent with a four-tap central difference in
the surface domain and already moves world position, object position and UV
together.

- **The step rule is RISE's, by decision.** On primary hits
  `relief_modifier` raises an authored step to half the pixel footprint, so
  it band-limits. At Filter Width 1.0 that is close to what Cycles does; a
  fine-grain bump authored at 0.1 pixel renders smoother in RISE. Parity
  gates on bump use Filter Width 1.0 and report, not gate, the 0.1 case.
- **Bump is per object in RISE, per shader socket in Blender.** A mix whose
  leaves have different bump or normal inputs cannot be expressed by one
  object-bound modifier (97 materials, 4.0%, in 26 files). By decision a
  `mix_material` child may name its own modifier, applied when that leaf is
  chosen (companion §I.2). When every leaf shares one normal input the
  exporter binds one modifier to the object, as today.
- Chained bumps and bump-over-normal-map (6% of materials) map to
  `stack_modifier`.
- A Bump with no height input is a no-op and is folded away. A Bump driven
  through an Ambient Occlusion node is **not** folded: draft 5 said it has
  no gradient in Cycles, and round 2 showed by render that only the
  occlusion fraction has none, while the node's Color output keeps the
  gradient of its Color input. `ao()` in a bump evaluation takes its
  occlusion at the centre point and its arguments at the tap (companion
  §III.3).
- A linked Displacement output in Bump mode becomes a relief modifier with
  height `(height − midlevel)·scale`, distance 1, as Cycles does. True
  displacement (3.4%) maps onto `displaced_geometry` in Phase 4.

### 5.6 Single shaders (step 0)

A surface that is one shader node maps onto a RISE material:

| Blender | RISE | Matches Cycles? |
|---|---|---|
| Diffuse BSDF | Lambertian; with Roughness above 0, Oren-Nayar in a **new energy-preserving mode** | yes with that mode, against Blender 5.3 (in 4.3 to 5.1 and the 5.2 alpha the multiple-scattering term carries one more factor of the colour, above). Cycles' diffuse stays at albedo 1 in a white furnace at any roughness; RISE's classic Oren-Nayar loses energy |
| Glossy BSDF | `ggx_material` in a **new constant-reflectance mode** | yes for Multiscatter GGX (the node default) in directional albedo, with that mode: Cycles' Glossy is colour × microfacet with no Fresnel, while RISE's other modes vary with angle (`schlick_f0` rises toward 1 at grazing). Cycles' plain GGX option has no energy compensation and is darker than RISE at high roughness. The multiple-scattering lobe's shape differs (Cycles scales the single-scatter lobe by a view-dependent factor; RISE adds a reciprocal Kulla-Conty lobe). Beckmann and Ashikhmin-Shirley render as GGX |
| Glass, Refraction | dielectric | only when smooth and untinted |
| Emission; a colour wired into a shader socket | luminaire (strength 1 for the bare colour, as Cycles converts it) | yes |
| Subsurface Scattering | random-walk SSS (the bridge's existing model) | no: an approximation |
| Translucent | **new** `diffuse_transmission_material` | yes: both are a Lambertian lobe on the far side of the surface |
| Transparent | fully uncovered alpha; tinted, `passthrough_material` | yes |
| Metallic | `ggx_material` in its conductor mode | yes for the physical-conductor mode with GGX; an F82 edge tint is not modelled |
| Sheen | `fabric_material` over a black base | no: RISE's sheen is the Charlie model; the mapping is to be verified in step 0 |
| Specular BSDF (EEVEE) | PBR specular workflow | no: Cycles does not render this node |
| Holdout, Toon | none: a black absorber with an error-level warning (§5.9) | not planned |

**Three RISE additions**, all general and all physically sound:

- `ggx_material` `fresnel_mode constant`: reflectance is the authored colour
  at every angle. The model is energy-bounded for colours at or below 1.
  **Implemented 2026-11-19**: at colour 1 it is the same function as
  `schlick_f0` with F0 = 1, and a white lobe's furnace reads 0.994 to 1.002
  at GGX alpha 0.1, 0.3, 0.6 and 1 and incidence 0, 45 and 75 degrees
  (200 000 samples each, standard error up to 1.7e-3; Monte Carlo plus the
  multiscatter table's residual).
- An energy-preserving Oren-Nayar option on the Oren-Nayar material, after
  the formulation Cycles adopted. RISE's classic model stays the default so
  existing scenes do not move. **Implemented 2026-11-19** as
  `orennayar_material model energy_preserving` (EON: Fujii's model plus the
  OpenPBR multiple-scattering term). Its `roughness` is a [0, 1] value, as
  in Blender, not the classic model's angle in radians. A white surface has
  albedo exactly 1 in closed form at every roughness and angle; a
  quadrature of the BRDF reads 1 within 6e-6 at 6 roughnesses and 5 angles
  up to 88 degrees.
- `diffuse_transmission_material { reflectance, transmittance }`: a thin
  sheet that reflects diffusely on the side light arrives and transmits
  diffusely to the other. Blender's Translucent is `reflectance 0`.
  - Reciprocal; energy-bounded when reflectance plus transmittance is at
    most 1 per channel, and a larger sum is scaled back to 1 with a warning.
  - It has no interior: no index of refraction, no entry on the refraction
    stack, no medium. That is what distinguishes it from the existing
    `translucent_material` and lets it be a `mix_material` leaf (146 corpus
    materials use Translucent, 142 of them inside a mix).
  - It is opaque to shadow rays, as Cycles' is, and scatters into the full
    sphere, so it joins the materials whose light sampling is not clipped
    to one hemisphere.
  - It ships with the full material contract (sampler, density and
    evaluator as one function; companion-wavelength evaluation) and the
    furnace and sampler-against-density tests the other lobes have.
  - Where a Mix of a Diffuse and a Translucent share one normal, the
    exporter may emit this one material with the two weights folded into
    its colours; that removes the per-hit choice and its noise.

**Also step 0, exporter only:**

- Object ray visibility for **camera** and **shadow** (shadow visibility is
  off on non-light objects in 45 of 174 corpus files). Camera visibility
  needs a small RISE object flag. Shadow maps to the existing
  `casts_shadows`, and is exported **only once the companion's step S is
  in**: until then `casts_shadows FALSE` under an area light has the
  two-strategy inconsistency the companion describes, and exporting it would
  spread that.
- The bake fixes of §5.9.

**Inputs that are numeric-only today** and so cannot yet be procedural:
coat IOR, and the subsurface coefficients (computed in Python from an
untextured colour). They stay constants; a procedural input to them goes to
the per-socket fallback's warning, not silently to a default.

Until Phase 1 a single shader's inputs must be constants or image textures,
as for Principled today.

### 5.7 Shader mixing (Phase 2)

966 materials (40%) contain a Mix or Add Shader, and mixes nest.

Any tree of Mix and Add Shader nodes is a weighted sum of its leaf shaders,
so the exporter flattens it into one N-ary `mix_material`. At each hit one
leaf is chosen with probability given by its weight, from a hash of the
object instance and the hit position, and the hit carries that leaf as its
material. The result is a fixed patchwork of the leaves below pixel scale.
For weights that depend on position alone it renders the area mixture for
every path that does not visit the same point twice (companion §I.1).

- **Weights are coefficients**, never normalised: the material is the
  weighted sum itself, and a shortfall below 1 absorbs.
- A white Transparent is the mix's alpha coverage; a tinted one is a
  pass-through leaf; an unlinked shader input is a black absorber.
- Adding emission to a surface uses the luminaire wrapper. Adding two
  surfaces can gain energy; it is allowed behind an explicit flag with a
  warning (151 materials; 99 are Principled + Translucent). In such a mix a
  Transparent is a scaled pass-through leaf, because coverage alone cannot
  carry a factor above its own probability.
- A leaf may carry its own bump (§5.5).
- A refracting shader as a mix leaf is refused, with no exception (2.2% of
  materials), because the refraction stack would enter through one leaf and
  leave through another. A Translucent leaf is not refracting (§5.6).
- A subsurface leaf under a weight that varies over the surface renders
  under the path tracer only (14.2% of materials, most of them
  path-tracer-only already).
- A weight that reads the view direction or the ray type makes the material
  path dependent (§5.8).

The design, the audit of where RISE asks an object for its material, the
integrator prerequisites and the tests are in the companion, parts 0 and I.

### 5.8 Path dependence, Ambient Occlusion and friends

Designed as general RISE features in the companion, parts II and III.

- **Path-dependent inputs** are those defined only along a path from the
  camera: the Light Path node (6.2% of materials) and anything that reads
  the arriving ray's direction, which is Geometry Incoming, Backfacing,
  Layer Weight, Fresnel and Reflection coordinates (24.5%). A light-subpath
  vertex does not know them, and BDPT, VCM and MLT rebuild hit records with
  the ray re-aimed per query. So a scene using any of them renders under
  the path tracer: Auto selects it, and an explicitly chosen BDPT, VCM or
  MLT refuses. By Phase 4, 23.7 points of the 92.1% translated are
  path-tracer-only. The machinery that declares and refuses ships before
  the first such input (§9).
- **Shadow visibility** is defined as "direct light is gathered straight
  through the object, and gets no second route through it", under both
  sampling strategies. It matches Cycles for point and small lights and is
  lighter than Cycles under large ones, where Cycles has no single answer.
  It needs a shadow-context walk of its own in the path tracer.
- **Emission that depends on the ray type, depth or length** is found by
  bounces only; an emitter that depends on the camera flag alone stays
  light-sampled.
- **Ambient Occlusion** (6.4%). A ray-traced `ao(distance, samples, normal,
  flags)` builtin: the node's own sample count, cosine-distributed rays,
  shadow-casting occluders with no shader evaluated, Only Local and Inside.
  - Its estimate is fixed per surface point where Cycles redraws per path,
    a disclosed difference that shows through steep ramps.
  - Its default hemisphere is Cycles': the unmodified shading normal on the
    side the ray arrived from. That works in every integrator on a material
    that does not transmit light, and is path-tracer-only on one that does
    (5 corpus materials).
  - The existing baked `occlusion()` signal stays for hand-authored masks.
- **Hair Info, Particle Info** (1.2%): per-strand and per-particle values
  ride object and mesh attributes.
- **Bevel and Wireframe** (0.5%) stay on the fallback.
- **Image sources** (5.3%): UDIM tiles are an image-loading feature
  independent of this design; generated images only need writing to disk.

### 5.9 Fallback: per-socket baking (Phase 1)

When a sub-graph contains an unsupported node, bake **only that sub-graph's
output**: wire the socket to a temporary Emission shader, bake `EMIT`, and
substitute an image for that one `def`. Everything else, including every
lobe, stays native. This replaces today's whole-material bake, and it ships
with Phase 1 so that no phase has undefined behaviour for an unsupported
node.

- A bake is keyed on a hash of the sub-graph it replaces, so a stale bake is
  detected rather than used.
- It keeps the limits inherent to baking (UV space, one proxy object, no
  per-instance or view-dependent variation, values clamped by the bake
  format) and says so in a warning naming the node.
- **The render gate.** A material that compiles natively is never gated. A
  material that needs a bake which is missing or stale is gated, as today.
- **An unsupported shader node has no bake fallback.** It becomes a black
  absorber leaf with an error-level warning; the render is gated until the
  user acknowledges it. It never renders silently wrong.
- Two operational fixes found while investigating the slow bake land in
  step 0: cap Cycles samples for the bake (these are noise-free shader
  evaluations; 128 samples cost 43 s per channel where 1 costs 0.6 s), and
  skip objects hidden from render when choosing the proxy.

## 6. Spectral behaviour

A Blender graph does arbitrary arithmetic on RGB; RISE's spectral
integrators ask a painter for a value at one wavelength. The rule is that
the program computes in RGB and the result is converted once, according to
its declared kind (§5.2f):

- `reflectance` outputs are clamped to [0, 1] and uplifted like an image
  texel; `radiance` outputs take the illuminant uplift; `scalar` and
  `vector` outputs never pass through an uplift.
- What RGB and spectral must agree on is stated as three tests (§7), not as
  one "band":
  - *at the painter:* a `reflectance` output's spectrum, integrated against
    the colour-matching functions under D65, returns the RGB output within
    the uplift table's own residual;
  - *in a white furnace:* an output of exactly 1 stays energy-neutral in
    both modes;
  - *in a lit scene:* the RGB and spectral renders are compared and the
    difference **reported**, not gated. Products of uplifted spectra over
    several bounces are not products of RGB triples, by design.
- Hero and companion wavelengths see the same RGB, since the program is a
  function of the hit record alone.
- A program that samples a spectrally defined painter is refused at attach
  time; the compiler never generates that case, since Blender graphs are
  RGB.
- A procedural IOR is a scalar and has no dispersion; combined with a
  dispersive glass the dispersion is dropped, with a warning.

## 7. Fidelity and validation

**What is being compared.** Patterns against Cycles; RISE against itself for
everything structural. The second kind needs no Blender and cannot be fooled
by colour management.

**RISE-internal, reference-free tests:**

1. Each builtin against golden values dumped once from Cycles (C++).
2. A multi-output program equals the same expressions as separate painters;
   cached evaluation equals uncached, bit for bit.
3. A program-driven material renders the same under PT, BDPT and VCM (for
   programs with no path-dependent input); and RGB against spectral by the
   three tests of §6.
4. Compiler unit tests (Python, no Blender): fake graphs → expected
   expression text, for conversions, folding, every Math and Mix mode,
   table modes, and the per-node defaults for unlinked vectors.

**Against Cycles (Blender required):**

5. **Node parity.** `tools/blender_node_parity.py`, built like
   `blender_light_parity.py`: one emissive plane per node configuration,
   rendered by both. The protocol is fixed: one sample at the pixel centre,
   no reconstruction filter, linear EXR, RISE's pixel convention.
   - Continuous outputs: maximum absolute difference ≤ 1e-4 where both
     renderers' inputs are exact in single precision, and a stated looser
     bound at large coordinates, where Cycles' single-precision fractional
     coordinate carries about 6e-5 error at a coordinate near 1000.
     For the noise builtins (§5.2(b), "Precision, as built") the bound
     must be RELATIVE to max(1, |value|) -- hetero terrain reaches 1e5 and
     more, and the multifractals grow past 1 -- and it depends on roughness
     AND detail together (and, through the float ulp, on coordinate size), since the high octaves amplify a one-ulp
     difference by roughly (roughness × lacunarity) per octave. The node
     parity test therefore bounds each configuration by the figures
     OslNoise.h tabulates (a pixel bound where they are small, a
     statistical comparison where they approach order one, as at high
     roughness with high detail). (The builtins equal a strict build of
     Cycles' sources exactly on identical float inputs;
     `tests/OslNoiseTest.cpp`, `tests/OslVoronoiTest.cpp` and
     `tests/OslColorTest.cpp` gate that,
     Voronoi's Minkowski rows bit for bit only on Apple arm64.)
   - Discontinuous outputs (Voronoi cell colour, constant ramps, Checker):
     at most 0.1% of pixels differing, excluding a 1D Voronoi (F1, F2 or
     N-Sphere Radius) whose W is a constant 0 and which reaches an octave
     at -0: with a negative Scale (every octave, while Lacunarity is
     positive) or a linked negative Lacunarity (alternate octaves, which
     run only at Detail and Roughness above 0). There Blender's
     `-fno-signed-zeros` build hashes the centre cell's feature point
     differently, so the constant image can differ at every pixel
     (`OslVoronoi.h`, PRECISION).
   - White Noise on a non-quantised input: a distribution test, not a pixel
     test (§5.2b).
6. **Lobe parity.** One lit sphere per shader and option under a point
   light and under a uniform environment, compared in linear EXR with
   Cycles' indirect clamp, Filter Glossy and caustic switches neutralised
   (153 corpus files clamp indirect light, 151 set Filter Glossy to 1.0).
   Lobes §5.6 lists as matching are gated; the others are reported with
   their measured difference, which is what the documentation then states.
7. **Corpus compile test.** Every corpus material is run through the
   compiler; the share that compiles with no fallback must equal what the
   survey tool predicts for the phase. This proves the survey's rules, not
   the rendering.

## 8. Performance

No number in this section has been measured yet; the VM's own header says it
is "not a hot per-sample path replacement for a hand-written C++ field, by
design". The work starts with a baseline, as the repository's performance
procedure requires.

**Budget.** A translated material costs at most 3× an image-textured GGX
material per shading point at the 90th-percentile graph size of its phase,
measured in PT and in BDPT. If a phase misses the budget, the evaluator is
the first suspect.

**Measured before Phase 1 code:**

- nanoseconds per VM instruction today;
- an image-textured GGX material, PT and BDPT, as the reference point;
- **the lowered size of the corpus's Phase 1 graphs:** slots and
  instructions per material input, from a throwaway lowering pass run over
  the survey's files. Phase 1's coverage figure is an upper bound until this
  says how many fit in 512 slots.

**Measured in Phase 1:**

- typical Phase 1 materials (8 and 36 nodes) as one expression per input;
- how often a shared sub-graph is re-evaluated per shading point, across
  evaluate / sample / density and in spectral mode.

**The spike that decides option A against option C, before Phase 2:**

- synthetic programs of 100, 400 and 650 `def`s with six outputs, a bump
  height and four mix weights;
- in the existing VM extended as §5.2a describes, against a flat register
  executor with static per-output slices and multi-result kernels;
- cache hit rates under PT and under BDPT with a program that samples
  images;
- per-hit storage cost against today's fixed 512-slot stack arrays.

The spike compares representative lowered programs. It does not build both
executors in full: a prototype register executor covering the builtins those
programs use is enough to decide.

Also measured when built: on-demand mesh attribute lookups against today's
eager interpolation; mix resolution with and without a mix in the scene;
`ao()` at 16 rays.

## 9. Phasing and coverage

From `tools/blender_node_survey.py report`, on 2,418 distinct materials,
1,020 distinct structures and 94 files with five or more materials.

**Graph translated, under the path tracer** (every feature supported):

| Step | materials | structures | per-file |
|---|---|---|---|
| today | 20.7% | 2.5% | 15.2% |
| step 0 | 27.4% | 4.2% | 21.9% |
| phase 1 | 44.5% | 30.5% | 38.2% |
| phase 2 | 75.1% | 71.0% | 77.0% |
| phase 3 | 86.1% | 79.8% | 83.1% |
| phase 4 | 89.9% | 85.3% | 88.5% |

**… and every lobe also matches Cycles** (no approximate lobe mapping; an
upper bound, §2.4):

| Step | materials | structures | per-file |
|---|---|---|---|
| today | 20.1% | 2.4% | 14.6% |
| step 0 | 25.4% | 3.2% | 20.5% |
| phase 1 | 39.6% | 24.1% | 31.5% |
| phase 2 | 59.7% | 52.7% | 56.3% |
| phase 3 | 63.1% | 58.4% | 58.7% |
| phase 4 | 66.7% | 63.4% | 63.2% |

**Graph translated, under the path tracer, BDPT and VCM alike** (nothing
path-tracer-only: no view-direction or Light Path input, no subsurface leaf
under a varying mix weight, no facing normal read on a transmitting
material). MLT is outside this row: it refuses the new mixing materials.

| Step | materials | structures | per-file |
|---|---|---|---|
| today | 20.7% | 2.5% | 15.2% |
| step 0 | 27.4% | 4.2% | 21.9% |
| phase 1 | 43.7% | 29.0% | 37.5% |
| phase 2 | 62.0% | 53.7% | 56.4% |
| phase 3 | 65.1% | 58.6% | 58.3% |
| phase 4 | 66.8% | 61.6% | 60.2% |

10.1% of materials (245) use something no phase covers:

- a subsurface leaf under an Add of two surfaces (65 materials; draft 6
  said "at most 8");
- a refracting shader inside a mix, or a linked Transmission Weight (56);
- a procedural input to a constant-only parameter of a lobe in use, almost
  all of them a linked Subsurface Radius or Scale (98, overlapping the
  others);
- Shader to RGB (58), closure nodes (51) and repeat zones (14), Raycast,
  Ray Portal, Bevel, volumes and Toon. The closure and repeat nodes should
  move into Phase 1 once the compiler runs on the inliner, which unrolls
  them; the survey still walks the authored tree and counts them here.

**How the figures moved from draft 6**, all from round 3's audit of the
survey (the tool now resolves mix leaves, perturbed-normal uses and value
sinks through node groups):

- The first row fell 2.2 points at phase 4 and 3.2 at phase 1. The largest
  causes: procedural inputs to constant-only sockets and linked Subsurface
  or Transmission weights were counted as supported; EEVEE Specular (30)
  was counted as a single lobe; a linked Bump strength was counted as plain
  bump.
- The third row fell 1.7 points at phase 4: facing normals on transmitting
  materials (87) are path-tracer-only by decision.

**What three pieces of new work are worth at phase 4** (first row):

| Without | phase 4 |
|---|---|
| the perturbed normal as a value in programs (243 materials use it) | 81.8% |
| a modifier scale that is a painter (84) | 87.1% |
| coated and sheen leaves in a gain mix (9) | 89.7% |

**What each step delivers:**

| Step | RISE | Exporter |
|---|---|---|
| **0** | constant-reflectance GGX mode and energy-preserving Oren-Nayar (implemented 2026-11-19); `diffuse_transmission_material`; object camera-visibility flag. The companion's steps **P** (emitters outside the light list) and **S** (shadow visibility in the path tracer) | single shaders mapped directly; colour-into-shader as emission; camera visibility; **shadow visibility, only after S**; bake sample cap and hidden-proxy fix |
| **1** | the companion's step **D1** (declaration, census, pre-flight refusal, Auto routing, guiding and SMS switches) **first**; then builtins (noise, Voronoi, hash, colour, safe math, transforms), `lut`, the new hit inputs, object attributes, `relief_modifier` strength; the existing evaluator otherwise unchanged | the lowered-size measurement of §8 first; compiler on Blender's inliner; converters; textures; coordinates; view-direction inputs (path tracer only, behind D1); Bump; per-socket bake fallback and the new render gate; ABI v19; tests 1, 4, 5 and 7 |
| **1→2** | the performance spike of §8, and the evaluator decision | |
| **2** | multi-output programs; the companion's steps B and D2 (`mix_material` with per-child modifiers, `scaled_material`, `passthrough_material`, ray context, emitter rules, visibility masks); named mesh attributes; `sample_at` | shader mixes; Light Path; mesh attributes; non-active and additional UV sets; ABI v20 |
| **3** | the companion's step A (`ao()`, which needs only D1 and may land earlier) and step C (mix-aware luminaries) | Ambient Occlusion; hair and particle inputs; UDIM and generated images |
| **4** | | true displacement; Brick, Gabor, Radial Tiling |

**Order constraints**, both from round 2:

- No path-dependent input ships before the machinery that declares and
  refuses it. Phase 1 exports `I` and `backfacing`, so D1 is Phase 1's first
  item.
- The exporter does not map shadow visibility to `casts_shadows` before the
  path tracer treats it consistently (S).

**What may start now, and what may not.** Round 3's verdict is the gate.

- May start: the procedural builtins and their golden tests (gradient noise,
  fractal noise, the Noise Texture node and White Noise **implemented
  2026-11-20**, `tests/OslNoiseTest.cpp`; Voronoi **implemented
  2026-11-22**, `tests/OslVoronoiTest.cpp`; colour conversions and Mix Color
  **implemented 2026-11-22**, `tests/OslColorTest.cpp`; safe math,
  componentwise vec3 and transforms next); the compiler's
  unit tests; the lowered-size measurement; the performance baseline;
  constant-reflectance GGX and energy-preserving Oren-Nayar
  (**implemented 2026-11-19**, `tests/BlenderLobeAdditionsTest.cpp`); the
  companion's step P (**implemented 2026-11-16**: DL-633,
  `tests/UnlistedEmitterWeightTest.cpp`; round 3's extended-SMS site
  concerns mix luminaries and moves to the mixing prototype).
- Needs a specification fix first (round 3 names each): the new hit inputs,
  `diffuse_transmission_material`, the companion's step D1, the step-0
  exporter and camera visibility.
- Not built from prose at all: the companion's steps S, B, D2 and A, and the
  Phase 2 program cache. They are prototyped against failing closed-form
  tests taken from the three reviews' counterexamples.

Each phase runs the full implementation-review loop and ends with the corpus
compile test, the parity tools and the survey report in agreement.

## 10. Decisions and open questions

**Decided by the owner, 2026-11-14.** The first four are detailed in the
companion's Decisions section.

1. Material mixing chooses one leaf per hit. Adding two surfaces may gain
   energy, behind an explicit flag and a warning.
2. Light Path, view direction and ray visibility are RISE features. They
   are path-tracer-only: Auto routes there and an explicit BDPT, VCM or MLT
   refuses.
3. Shadow visibility means "direct light ignores the object, under both
   sampling strategies". Ray-type-gated emission is found by bounces only.
4. `ao()` is evaluated wherever the program is, including shadow rays and
   sampled lights.
5. **The goal is to translate the graph, with lobes being RISE's own
   physical models.** Two lobe additions are in scope (constant-reflectance
   glossy, energy-preserving diffuse roughness); Beckmann renders as GGX;
   microfacet glass and textured subsurface are separate projects.
6. **Native translation requires Blender 5.0**; older versions keep the
   bake path.

After review round 2:

7. **Blender's Translucent BSDF maps to a new RISE lobe**, a thin diffuse
   transmitter with no interior, which may be a mix leaf (§5.6).
8. **No refracting shader as a mix leaf, with no exception.** The
   shadow-ray-only glass case draft 5 admitted is refused too.
9. **A mix child may name its own modifier**, so leaves with different bump
   or normal inputs translate (§5.5).
10. **`ao()`'s default hemisphere follows Cycles**, which makes it
    path-tracer-only on a material that transmits light (§5.8).

**Open.**

1. **The evaluator for Phase 2:** the existing VM extended, or a separate
   executor behind the same language. Decided by §8's spike.
2. **Builtin naming.** **Decided 2026-11-20:** an `osl_` prefix
   (`osl_snoise`, `osl_fractal`, `osl_noise_texture`, ...). RISE's own
   `perlin`, `fbm` and `worley_*` keep their different definitions.
3. **Does Blender's inliner unroll closure nodes and repeat zones** as its
   source suggests (2.7% of materials)?
4. **Phase 1's fit.** How many Phase 1 graphs lower into the existing
   evaluator's 512 slots (§8). Decides whether Phase 1 needs the evaluator
   work Phase 2 was meant to carry.
5. **The classic Fresnel blend.** Whether an exporter option should map
   `Mix(Diffuse, Glossy, Fresnel)` onto a physical layered material, which
   would work in every integrator at the cost of not following the graph
   literally (companion §II.5). Needs a count first.
6. **Stylised content.** Shader to RGB (2.4%) and the EEVEE-first splash
   scenes are out of scope here. Confirm that is the intent.
7. **The patchwork selector.** Round 2 asked for an independent leaf draw
   per transport event; this design keeps one fixed leaf per point of an
   object instance (companion §I.1, round 2 record §3). A third review
   should test that choice first.
8. **A third review.** Round 2 found errors that the answers to round 1 had
   introduced. The companion's revised contracts for mixing, shadow
   visibility and `ao()` have not been reviewed.

## Appendix A — Facts this design relies on

Verified in the RISE tree on 2026-11-14, the starred ones corrected by
review round 1:

- The expression VM is a postfix stack machine with scalar and vec3 types,
  `param` / `def` / `expr` lines and context `u, v, P, Po, N, fw, fwo, time,
  curv, curvR` (`src/Library/Painters/ExpressionEval.h`). Its limits are
  `kMaxSlots = 512` (a vec3 takes three) and `kStackCap = 512`.
- ★ Every `def` is evaluated eagerly. The thread-local memo
  (`Utilities/ExpressionMemo.h`) is a 4-entry table of one final value per
  program; it is not a per-`def` cache.
- ★ The evaluator's bodies are a byte-pinned floating-point contract under
  fast-math; its scalar function-id band is full; its context-variable mask
  is 32 bits; programs are copied by value into each painter.
- `sample(name)` evaluates the named painter at the caller's own hit.
- `relief_modifier` takes an `IScalarPainter` height and differentiates by
  four taps that move `P`, `Po` and UV together, holding the signal block
  fixed. ★ On primary hits an authored step below half the pixel footprint
  is raised to it. Modifiers bind to objects; `stack_modifier` chains them.
- `RayIntersectionGeometric` carries `ptCoord1`, `vColor`, `txFootprint`,
  the ray, and a signal block with `pSelf`, the primitive index and
  barycentrics. ★ BDPT, VCM and MLT rebuild it with the ray re-aimed per
  query (`Utilities/PathVertexEval.h`), and sampled-light records carry no
  ray.
- There is no `mix_material`; `composite_material` is a layer stack.
- The bridge carries painters by name; painter kinds today are uniform,
  PNG / EXR / HDR / TIFF / JPEG, blend and UV transform.

Checked after review round 2:

- The sampled closest-hit loop tests alpha with
  `ri.pMaterial->AlphaCoverage(ri.geometric)` after its inner intersection
  (`Managers/ObjectManager.cpp:2962`), and stamps the result as the hit's
  accepted coverage, which VCM and the photon tracers divide deposits by.
- A delta pass-through attenuates a shadow ray only for a delta light
  (`Rendering/RayCaster.cpp`, `bPassThrough = bDeltaLight && …`).
- `ISPF::EvaluateKrayNM` returns −1 by default; an SPF that does not
  override it contributes nothing on HWSS companion wavelengths.
- The scalar expression pipe evaluates at time 0
  (`Painters/ExpressionPainter.cpp:356`).
- A hit record carries its modifier (`ri.pModifier`), which each integrator
  applies after the hit (42 call sites in 15 files).
- `UnflippedGeomNormal()` returns the geometric normal, not a shading
  normal.
- The existing expression memo already keys cross-object queries on scene,
  object, world point and time, and invalidates by generation.

From the Blender source (`intern/cycles`, a 5.2-alpha checkout):

- Noise: `kernel/svm/noise.h` (BSD-3-Clause), `fractal_noise.h`,
  `noisetex.h`. Voronoi: `kernel/svm/voronoi.h` (Apache-2.0 with an MIT
  block). Hashes: `util/hash.h`.
- Color Ramp and curves are evaluated as 257-entry tables. ★ Constant ramps
  are looked up without interpolation; curves extrapolate.
- ★ An unlinked texture Vector defaults per node: Generated for the
  procedural textures, UV for images, nothing for White Noise and Mapping.
- Bump evaluates the height sub-graph three times with ray differentials.
  ★ The offset is the node's Filter Width (default 0.1, 1.0 on files saved
  before 4.4).
- ★ White Noise hashes the raw float bits of its input.
- ★ Glossy BSDF has no Fresnel term; Diffuse roughness is an
  energy-preserving Oren-Nayar; a colour wired into a shader socket becomes
  an Emission of strength 1; an unlinked shader input is a black absorber.
- Object Info Random is `random_id / 0xFFFFFFFF`.
- The instance `random_id` hash lives in `BLI_hash.h`, which is GPL; the
  exporter must read `random_id` from the depsgraph and not port that file.
- The Ambient Occlusion node's Color output is the occlusion fraction times
  its Color input, and its default normal is the shading normal `sd->N`
  (`kernel/svm/ao.h`). Ray Depth on an emission or shadow evaluation counts
  one bounce further (`kernel/svm/light_path.h:55`).
- Translucent BSDF is a diffuse lobe on the far side of the shading normal
  and is opaque to shadow rays.

## Appendix B — Reproducing the survey

```
python3 tools/blender_node_survey.py run --blender <Blender 5.3 binary> \
    --out <json dir> <files…>
python3 tools/blender_node_survey.py report <json dir>
```

Files are opened with `--factory-startup --disable-autoexec`. The corpus used
for this draft is the list in §2.2 from `mirrors.dotsrc.org/blender/blender-demo`
(directories `cycles`, `eevee`, `splash`, `geometry-nodes`, `rendering`,
`test`, and the root); it is not checked in.
