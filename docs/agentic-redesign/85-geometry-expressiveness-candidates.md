# 85 — Geometry Expressiveness: Candidate Survey

**Status: CANDIDATE SURVEY (2026-08-14).  C2 SHIPPED 2026-08-14** — see the
C2 section; shipped as `profile_circle`/`profile_rect`/`point_scale`/
`path_closed` on `sweep_geometry` (point_scale was added mid-slice when
review proved the grammar could not express a round non-linear taper —
the headline tentacle case), plus builder-prompt adoption gated on the
plan's declared `construction == "sweep"` (schema + worked example ride
the declaration; context-volume law respected).  Two review rounds to
zero P1; adoption is wired but NOT yet measured live — the census on the
next live runs is the C2 measurement.  Remaining candidates unstarted.
This is the opening document of the workstream seeded by the
creative-richness closing verdict ([CREATIVITY_JOURNAL.md](CREATIVITY_JOURNAL.md),
Closing): *"what limits realism now is what a `part` line can EXPRESS"* —
the harness work is done; the next lever is richer geometry inside RISE
itself, exposed so scene-building agents actually reach for it.

Three inputs were synthesized: (a) the agentic surface as shipped through
arc 83, (b) a full inventory of RISE's geometry system, (c) an external
survey of modeling products and text-to-3D / LLM-writes-CAD literature.

---

## 1. The governing finding from the literature

Every serious 2024–2026 text-to-3D system converges on the same shape:
**LLMs reliably author small, declarative, named-parameter grammars with
relative/low-dimensional numbers, and reliably fail at raw mesh topology
(vertex/index lists, winding) and absolute-coordinate spatial math.**

- OpenSCAD's tiny declarative grammar out-scores Python CadQuery for LLM
  correctness despite Python being in-distribution
  (getleo.ai open-source text-to-CAD survey).
- Text2CAD-Bench: sharp accuracy falloff at complex-topology tiers
  (arXiv:2605.18430). Fine-tuned Text-to-CadQuery still only ~59–69 %
  top-1 (arXiv:2505.06507).
- AIDL (arXiv:2502.09819): offload numeric/spatial resolution to the
  system; don't make the model compute absolute coordinates.
- ShapeCraft / ShapeAssembly / ShapeLib: decompose into a graph of small
  named parametric sub-parts ("leg", "seat", radius, taper) — the
  best-scoring representation family.
- 3D-GPT / SceneCraft: the LLM fills parameters into curated procedural
  generators; SceneCraft additionally *promotes* recurring generated
  patterns into a named reusable library over time.

RISE's chunk language (named chunks, descriptor-validated parameters,
canonical-frame `build_element` clean rooms) is already exactly this
paradigm. **The gap is primitive coverage, not authoring paradigm.**

And from our own measured history (journal laws 8, 9, 12): capability
without adoption mechanics measures zero. `sweep_geometry` has existed
for months and **no builder has ever emitted one** — the same story as
area lights before arc 81. Every candidate below therefore ships in two
halves: the geometry, and its adoption (summoned category, one worked
example that parses, census visibility, priced inferior path where one
exists).

## 2. What RISE already has (relevant ceiling)

- Analytic primitives: sphere, ellipsoid, cylinder, torus, box,
  clipped plane, disk, infinite plane, Bézier/bilinear patches.
- `sdf_geometry`: 7 prims (sphere, box, roundbox, cylinder, torus,
  capsule, **roundcone**), hard/smooth union/subtract/intersect,
  per-part TRS, marching-tet tessellation, heightfield mode,
  keyframeable. No domain ops (repeat/twist/mirror).
- `csg_object` (object-level booleans), `displaced_geometry` (any
  tessellatable base + IFunction2D field), rich noise/expression
  painter families for displacement.
- **`sweep_geometry`** — already a generalized cylinder: arbitrary
  closed 2D profile, Catmull-Rom path, rotation-minimizing frames,
  linear taper (`end_scale_x/y`), per-station width (`point_width`),
  caps, `frame_hint`. Bakes to an indexed mesh. **Unused by builders.**
- `path_instances_geometry` — template instancing along a path.
- Mesh import (.3ds/.raw/.rawmesh2/.risemesh/.ply/.gltf) and a clean
  programmatic emission path: `TriangleMeshGeometryIndexed` +
  `Job::AddPrebuiltTriangleMeshGeometry` + `GeometryUtilities`
  (grid generation, welding, normal recompute). BVH is automatic.
- Missing entirely: lathe/revolve, skeleton→flesh, loft, L-systems,
  superquadrics, subdivision surfaces, bevel/inset operators.

Two proven, cheap implementation patterns exist:

1. **Parse-time expansion chunk** (`rect_light`/`shape_light`
   precedent): one new chunk keyword expands in the parser into a graph
   of existing chunks. Zero new rendering code.
2. **Mesh-baking builder** (`sweep_geometry`/`path_instances_geometry`
   precedent): a `RISE_API_Create*` factory computes vertices and hands
   a finished `ITriangleMeshGeometryIndexed` to the job. New code is
   pure computational geometry; intersection/BVH/area-light machinery
   is inherited.

## 3. Ranked candidates

### C1. `skeleton_geometry` — sphere-mesh / bone-chain flesh (creatures) — **highest value**

The one object class nothing current reaches: articulated creatures
(the dragon that "reads as mass," drooping tentacles, limbs). Author a
joint graph — `joint <name> <parent|none> <x y z> <radius>` — and the
system generates flesh.

Implementation is nearly free via **pattern 1**: each bone expands to an
SDF `roundcone` (frustum between parent radius and child radius, posed
along the bone) joined with `smin` at shared joints — i.e. the chunk
expands into the existing `sdf_geometry` part grammar. Sphere-tracing,
tessellation, displacement-base eligibility, area-light guards all
inherited. A `blend` knob per joint controls smin radius; optional
per-joint scale gives flattened limbs. This is literally the sphere-mesh
representation from the literature (Thiery et al.), and it matches how a
model already describes a creature in prose (a rig).

Note `file_build_plan`'s construction-method vocabulary already lists
`chain` — the summoned category slot is pre-reserved; today nothing
backs it. Adoption: back the `chain` method with this chunk + one worked
example (a quadruped or tentacle) in the build_element clean room.

Effort: S/M (parser expansion + pose math + tests). Risk: low.

### C2. Sweep adoption + tapered-sweep affordances (limbs, rails, stems, pipes) — **highest leverage-per-line**

The external survey's #1 candidate by value/cost already exists in-tree.
The work is (a) **adoption**: make `sweep` a first-class construction
method with a drop-in worked example (tapered tentacle: circular
profile, 4 path points, `point_width` droop) inside the clean-room
prompt, and census whether it moves; (b) **affordance gaps** that keep
an LLM from using it one-shot:
- `profile_circle <r> [n]` / `profile_rect <w> <h> [bevel]` convenience
  (today the model must hand-author a closed CCW polygon — exactly the
  coordinate-math failure mode the literature warns about);
- optional closed-loop path mode (handles, wreaths, chair armrest
  loops);
- possibly per-station roll.

Effort: S. Risk: minimal. Do this first — it is also the measurement
vehicle that validates the adoption playbook for everything below.

### C3. `lathe_geometry` — surface of revolution (furniture, vessels)

Profile polyline (2D points, r ≥ 0) revolved about an axis, partial
`sweep_degrees` supported. Unlocks chair/table legs, lamp bases, vases,
bottles, pedestals, knobs — the furniture vocabulary. Pattern 2: radial
grid emission + `GeometryUtilities` welding/normals; textbook code. A 2D
profile polyline is among the most reliable things an LLM writes (it's
a silhouette — models think in silhouettes; `file_build_plan` already
asks for an `outline`).

Effort: S. Risk: minimal.

### C4. Parametric macro-builders — Archimesh-style furniture/architecture

`stairs { steps 12 rise 0.18 run 0.28 width 1.2 rail TRUE }`,
`shelf_unit`, `table_frame`, `chair_frame`, `fence`, `door_frame`…
Each expands (pattern 1) into existing chunks (boxes, sweeps, lathes,
path_instances). Highest per-call authorability of anything surveyed —
named args, zero spatial reasoning, the builder resolves layout.
Value doesn't generalize (a growing library, not a primitive), so seed
with the 4–6 highest-frequency classes and grow. SceneCraft's
library-learning suggests a later mechanism: promote recurring
successful agent-built elements into named builders.

Effort: S per builder after C2/C3 exist. Risk: scope creep — cap the
initial set.

### C5. `branch_geometry` — L-system / recursive branching (plants)

Axiom + a few production parameters (branch angle, ratio, twist,
levels, stochastic seed), each segment realized through the existing
sweep machinery (tapered tube), leaves optionally via
`path_instances_geometry` or instanced profiles. Closes the
plants/trees/coral gap; FloraForge (arXiv:2512.11925) validates
LLM-authored plant grammars specifically, and plants tolerate parameter
imprecision gracefully. Keep the grammar small (parametric preset-style
knobs, not raw rewrite rules) — raw L-system strings are write-only for
models and humans alike.

Effort: M (turtle + recursion + reuse of sweep emission). Risk: medium
(tessellation budget on deep recursion; needs poly-count caps).

### C6. Superellipsoid — cheap primitive-family widening

Add exponent pair (e1, e2) to the ellipsoid family (analytic chunk
and/or an SDF prim with a conservative Lipschitz bound). One line, a
continuum from sphere→rounded box→cylinder-ish→pinched forms; torsos,
heads, cushions, stylized furniture shells. Not a new object class —
a force-multiplier inside `part` lines, where the census already
measures usage.

Effort: XS–S. Risk: SDF distance-bound care under non-uniform scale.

### Deliberately excluded

- **Subdivision surfaces over an authored cage** — requires the model
  to emit indexed topology with correct winding: the literature's
  canonical LLM failure mode. Revisit only as named operators
  (extrude-face/inset/bevel) over generated cages, never raw index
  lists.
- **Node-graph systems** (Geometry Nodes/Houdini-style) — mine the node
  *concepts* (curve-to-mesh = C2, skin = C1, scatter =
  path_instances); the graph representation itself is a poor LLM target
  and a huge evaluator to build.
- **Loft between cross-sections** — real value (boat hulls, car bodies)
  but the multi-profile correspondence problem is fiddly; defer until
  C2/C3 usage data shows demand.
- **Terrain/heightfield builders** — substantially covered by the SDF
  heightfield mode + `displaced_geometry` over `cartesian_disk_geometry`
  with the existing noise painters; environment dressing, not the
  structured-object gap.

## 4. Suggested sequencing

1. **C2** (sweep adoption + profile conveniences) — smallest slice,
   validates the adoption playbook with the census, and its emission
   machinery is a dependency of C5.
2. **C1** (`skeleton_geometry`) — the creature unlock; backs the
   already-reserved `chain` construction method.
3. **C3** (`lathe_geometry`) → **C4** (first 4–6 macro-builders).
4. **C5** (branching) and **C6** (superellipsoid) as satellites.

Each slice = geometry + parser descriptor + one worked example wired
into the relevant clean-room prompt + a parse/render test scene +
census visibility, then a measured live run before the next slice
(repeat-run discipline per journal §10 — N=1 spreads ±20 %).

Mechanical reminder: any new `.cpp/.h` touches all five build projects
(Filelist, rise_sources.cmake, both VS2022 files, pbxproj) — see
CLAUDE.md.

## 5. Source pointers

- Agentic surface + adoption mechanics: [CREATIVITY_JOURNAL.md](CREATIVITY_JOURNAL.md)
  (laws 8/9/12, closing handoff), `skills/agent/modeling-workflow-and-geometry.md`,
  `src/Library/Agent/AgentRpc.h`.
- Geometry inventory ground truth: `src/Library/Parsers/ChunkParserRegistry.cpp`,
  `src/Library/Geometry/`, `src/Library/RISE_API.cpp` (sweep machinery ~line 905+),
  `src/Library/Interfaces/ITriangleMeshGeometry.h`.
- External: OpenSCAD-vs-CadQuery LLM survey (getleo.ai); Text2CAD-Bench
  arXiv:2605.18430; Text-to-CadQuery arXiv:2505.06507; AIDL
  arXiv:2502.09819; ShapeCraft arXiv:2510.17603; ShapeLib
  arXiv:2502.08884; ShapeAssembly; CAD-Llama arXiv:2505.04481; 3D-GPT
  arXiv:2310.12945; SceneCraft arXiv:2403.01248; B-Mesh (Ji et al. 2010);
  Sphere-Meshes (Thiery et al.); FloraForge arXiv:2512.11925;
  Houdini Sweep SOP; Archimesh.
