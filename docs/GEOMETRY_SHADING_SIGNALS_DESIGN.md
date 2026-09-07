# Geometry-Derived Shading Signals — Curvature, Occlusion/Cavity, Thickness

**Status:** **PHASES 1–3 SHIPPED** (2026-08-29, same day: proposed → accepted →
implemented; every phase converged zero-P1 through independent review rounds —
see the per-phase status blocks in §13). The §5.5 correctness item landed first,
separately (commits `3f495c25..da92af3e`, zero-P1 — including a sign-pairing bug
family in CSG/back-face `dndu/dndv` negation the review loop surfaced beyond
this doc's scope). Where the implementation differs from this document's
sketch, the amendments are marked **AMENDED (2026-08-29)** in the relevant
section. **Open debts:** the Phase-1 cross-provider census (requires hosted
provider keys; harness + scenario configs in place, not yet run); the
BDPT/VCM/MLT neutral-signal transport gap (§14 item 11 — contained by a
startup diagnostic + honest descriptor text; the real fix is the
`PathVertexEval.h:94-106` widening contract, plus the `LightSampler`
NEE/photon-emission records). **Phase 4 remains observed-need gated and
untouched.**
**Date:** 2026-08-29.
**Inputs:** a six-pass source-grounded survey of the RISE tree — the expression
VM ([ExpressionEval.h](../src/Library/Painters/ExpressionEval.h),
[ExpressionPainter.cpp](../src/Library/Painters/ExpressionPainter.cpp)), the
intersection record
([RayIntersectionGeometric.h](../src/Library/Intersection/RayIntersectionGeometric.h)),
the painter contracts ([IPainter.h](../src/Library/Interfaces/IPainter.h),
[IScalarPainter.h](../src/Library/Interfaces/IScalarPainter.h)), the geometry
zoo ([SDFGeometry.cpp](../src/Library/Geometry/SDFGeometry.cpp), the triangle-mesh
specializations, the analytic primitives, the patch stubs,
[DisplacedGeometry.cpp](../src/Library/Geometry/DisplacedGeometry.cpp)), the
parser/registry surface
([ChunkParserRegistry.cpp](../src/Library/Parsers/ChunkParserRegistry.cpp)), the
derivation/threading model ([ARCHITECTURE.md](ARCHITECTURE.md),
`docs/agentic-redesign/20-derivation-engine.md`), and the measured adoption
record (doc 88, doc 75, `CREATIVITY_JOURNAL.md`). Contract conformance target:
[GEOMETRY_DERIVATIVES.md](GEOMETRY_DERIVATIVES.md). External prior art: the
curvature / AO / thickness surfaces of Arnold, Blender, Substance 3D, RenderMan,
V-Ray and Corona, plus the SDF curvature/AO literature — enumerated in §16.
**Nature:** research + design recommendation, in the mold of
[UNIFIED_INTEGRATOR_DECISION.md](UNIFIED_INTEGRATOR_DECISION.md) (survey →
candidates against evidence → recommendation) and
[HAIR_FUR_DESIGN.md](HAIR_FUR_DESIGN.md) (gated phases). **The user drives
implementation; this document proposes.**

---

## 1. The question, and the answer

Every believable wear/aging mask in production lookdev keys on three geometry
signals: **curvature** (paint worn off convex edges), **occlusion / cavity**
(dirt and patina settling in crevices), and **thickness** (translucency at thin
extremities, paint worn through). Substance's Smart Materials, Arnold's
`aiCurvature`, Blender's AO + Pointiness, and V-Ray's `VRayDirt` are four
independent implementations of the same three primitives, and Substance's mask
generators consume baked Curvature + AO + Thickness *directly* — this is the
canonical artist pattern, not a niche one.

RISE's expression VM exposes exactly seven context variables — `u`, `v`, `P`,
`Po`, `N`, `fw`, `time` ([ExpressionEval.h](../src/Library/Painters/ExpressionEval.h),
`ExprEvalContext` at :105-120; slot layout pinned at :179-193). None of them
describes the *shape* of the surface. A grep of `src/Library/Painters/`,
`skills/agent/`, and `src/Library/Agent/*.cpp` for curvature / cavity / ambient
occlusion / thickness returns **zero** hits for a geometry-derived shading
signal. This is greenfield.

The observed consequence is that agents authoring lookdev fake the signal with
axis-aligned position proxies — a `smoothstep` over `P.z` standing in for
"front edge wear". That mask does not follow the geometry: rotate the object,
displace it, swap the mesh for an SDF, and the wear stays where the world axis
was. It is the wrong signal wearing the right name.

**The answer: yes, and the cheapest honest version is nearly free.** RISE
already carries first- and second-fundamental-form data on the intersection
record for triangle-mesh hits (`SurfaceDerivativesInfo` at
`RayIntersectionGeometric.h:30-44`, populated at
`TriangleMeshGeometryIndexedSpecializations.h:458-462`), and the SDF family
already computes mean curvature for an unrelated purpose
(`SDFGeometry.cpp:1580-1611`). Differential curvature is a few dot products
away from being an expression context variable.

**Decision (recommended): signals enter shading through the intersection record
and the expression surface, delivered in three phased mechanisms —**

- **M1 — `curv` as an expression context variable**, computed in
  `ExpressionPainter::BuildContext` from `ri.derivatives` via the shape
  operator. Follows the exact `fw` precedent. Phase 1.
- **M2 — lazy per-hit `occlusion(radius)` / `thickness(radius)` builtins for
  the SDF family**, evaluated on demand from the distance field. Pure `Map()`
  evaluations: no rays, no scene access, thread-safe by construction. Phase 2.
- **M3 — baked per-vertex AO and thickness fields for meshes**, geometry-owned,
  built lazily at first production shading access, interpolated into the
  intersection record, and delivered through **M2's provider interface and the
  same builtin names** so an expression is portable across geometry families.
  Phase 3.

**Scene-wide (cross-object) AO is DECLINED for v1** (§8). Self-occlusion covers
the crevice-grime use case that motivates the work, and cross-object occlusion
is the one variant that breaks the free-invalidation story.

---

## 2. What RISE has today

### 2.1 The expression VM, and the wall around it

`ExpressionProgram` compiles to a flat postfix instruction list evaluated by
`RunAny` (`ExpressionEval.h:1224-1329`) over a stack-local `Scalar stack[512]`
and a caller-supplied flat `Scalar env[512]`. No heap, no mutable state,
`const` — one compiled program is safe for concurrent multi-thread evaluation,
a stated design goal (:12-15).

Two consequences matter here:

1. **Context variables are cheap and well-trodden.** Adding one is a bounded
   five-site edit — struct field, slot constant, `LookupContextVar` case
   (:471-481), the sole `BindEnv` definition (:1086-1104) and its call sites
   (:210, :221, :234, :274), and `BuildContext` population. The comment at
   :179-193 enumerates every site that must stay in sync. The `fw` variable is
   the exact precedent, including its honesty convention:
   `ctx.fw = ri.txFootprint.valid ?
   ri.txFootprint.worldWidth : 0` (`ExpressionPainter::BuildContext` /
   `ExpressionScalarPainter::BuildContext` in `ExpressionPainter.cpp`) — a
   valid-flag gate with a documented zero fallback, not a silent fabrication.

2. **Builtins can reach nothing.** `CallFunc` (:1127-1192) is **static**. It
   receives the popped stack arguments and one hardcoded scalar (`fw`, read
   from `env[kContextSlotFw]`). No `ExprEvalContext*`, no `ri*`, no scene
   pointer reaches it. A builtin that needs per-hit geometry — anything taking
   a `radius` argument — requires threading a `const` context pointer into
   `CallFunc`. Report 1 of the survey assessed this as a **moderate**, and
   `const`-safe, VM change.

A context variable is one fixed value per hit and cannot take a parameter.
That asymmetry — curvature is parameterless and differential; occlusion and
thickness need a radius — is why M1 is a context variable and M2/M3 are
builtins.

### 2.2 The painter contract

`IPainter::GetColor/GetColorNM/GetSpectrum/GetAlpha`
([IPainter.h](../src/Library/Interfaces/IPainter.h):56-92) and
`IScalarPainter::GetValuesAt/GetValueAtNM`
([IScalarPainter.h](../src/Library/Interfaces/IScalarPainter.h):124-140) take
**only** `const RayIntersectionGeometric&`. Every material call site passes
only that geometric slice (verified at `GGXSPF.cpp:161-162, 174-175, 287-288,
466-467), even though the full `RayIntersection` — which carries `pObject`
(`RayIntersection.h:37`) — exists one frame up.

`RayIntersectionGeometric` carries no caster and no scene pointer. It does
carry an opaque `IReference* pCustom` (:184) whose own comment (:176-183)
admits the gap: *"this is a huge hack... the correct thing to do here is to
refactor the entire idea of RayIntersection and RayIntersectionGeometric into a
ShaderContext, BSDFContext, PainterContext, so that they can ask for whatever
information they want."* That is a pre-existing architectural admission that
painters lack a clean extensibility channel — and it is the hook M2 formalizes
in the narrow, typed, `const` direction.

Ray casting from shading is available exactly one layer up, at
`IShaderOp` ([IShaderOp.h](../src/Library/Interfaces/IShaderOp.h), `PerformOperation`
at :42), which receives `const IRayCaster&`. RISE's existing AO is a shader op
for precisely this reason (`AmbientOcclusionShaderOp.h:46-54`, casting via
`caster.GetAttachedScene()->GetObjects()->IntersectRay(...)` at
`AmbientOcclusionShaderOp.cpp:135`). Being a shader op, it sits **outside** the
painter graph: not composable with masks, ramps, or blends, which is exactly
what a wear mask needs.

### 2.3 Curvature data already on the record

`SurfaceDerivativesInfo` (`RayIntersectionGeometric.h:30-44`) carries
`dpdu, dpdv, dndu, dndv, valid` — the first and second fundamental form data.
Triangle meshes populate it at intersection time
(`TriangleMeshGeometrySpecializations.h:291-310`,
`TriangleMeshGeometryIndexedSpecializations.h:458-462`) from the UV Jacobian
and the linear variation of the three vertex normals; `Object::IntersectRay`
transforms it to world space (`Object.cpp:764-772`). The comment at :201-205
states the honesty convention: other geometries leave `valid = false` and
consumers fall back to `IGeometry::ComputeSurfaceDerivatives`. The shape
operator `S = I⁻¹ · II` follows from those four vectors in a handful of dot
products — **no extra rays, no extra geometry queries**.

---

## 3. The architectural decision: where signals enter

Three plausible entry points were considered and **rejected**:

**(a) Widen the `IPainter` / `IScalarPainter` ABI to carry a caster —
REJECTED.** The blast radius is ~40 painter classes plus every BRDF/SPF call
site. It also runs against an otherwise uniform codebase convention: every
per-hit quantity a painter needs — `derivatives`, `txFootprint`, `vColor`,
`vTangent`, `ambientIOR` — is computed at intersection time and stashed on the
record, and the painter is a field read (`VertexColorPainter` is the two-line
archetype). And casting rays from an arbitrary painter call site nested inside
another cast has no precedent in the tree and no thread-safety analysis behind
it.

**(b) Give the expression VM scene access via a scene-query builtin —
REJECTED.** `CallFunc` is static and stack-args-only *by design* — that is what
makes one compiled program safe for concurrent evaluation across every render
thread (:12-15). Handing it a scene pointer inverts that design, and the
resulting reentrancy (a painter evaluated during shading casts a ray, which
shades, which evaluates a painter) is a new and unbounded control-flow shape.
M2 threads a **`const` context pointer to already-computed per-hit state** — a
strictly weaker, analyzable change that does not open the scene.

**(c) Screen-space AO / curvature — REJECTED on principle.** RISE is a path
tracer, so these signals must be **view-independent**: identical for a primary
hit and for the same point seen at depth 4 through a mirror, consistent across
bounces, defined for path vertices that were never on screen. A screen-space AO
value simply does not exist for a vertex reached through a reflection. Every
surveyed production tool agrees — Arnold, RenderMan, V-Ray and Corona fire
independent object-space rays; Substance bakes into texture space.

**The accepted route: the intersection record plus the expression surface.**
Signals are computed where the geometry is known (intersection time, or a
per-geometry bake — eagerly at the `Realize` seam or lazily at first production
shading access, per §7.3), ride the record as typed fields or a typed `const` hook,
and are read by expressions. Zero painter-ABI change, zero scene access from
the VM, view-independent by construction.

---

## 4. Per-family feasibility

Condensed from the geometry-zoo survey. "Free" means no extra ray casts and no
extra field evaluations beyond what the hit already paid.

| Geometry family | Curvature | Occlusion / cavity | Thickness | Notes |
|---|---|---|---|---|
| **SDF** (`sdf_geometry`) | **Excellent** — `div n̂ = k₁+k₂` already implemented as `jacobianAt`, `SDFGeometry.cpp:1580-1611`; smooth, resolution-free | **Good** — Evans-style ~5 taps along `n̂`; no rays | **Good** — inward field sample / short march; the field *is* the distance to the other side | `EvaluateParts` is a public static (`SDFGeometry.cpp:971-987`) — evaluable on a detached parts list, `O(#parts)` per eval |
| **`skeleton_geometry`** | as SDF | as SDF | as SDF | It **is** an `SDFGeometry` (expands to a Part list via smooth-min; `ChunkParserRegistry.cpp:5902`) |
| **Triangle mesh** (indexed + non-indexed) | **Free but faceted** — `dndu/dndv` derive from a per-triangle-constant vertex-normal-difference term (discontinuous across shared edges; see §5.3) | **Bake only** — needs real BVH rays | **Bake, or live via `bComputeExitInfo`** → `range2`/`ptExit` | Quality tracks vertex-normal quality; no adjacency/one-ring query exists (`ITriangleMeshGeometry.h:52-122`) |
| **Sweep / lathe / skeleton bakes** | inherits mesh | inherits mesh | inherits mesh | `RISE_API_CreateSweepGeometry` (`RISE_API.cpp:1519+`) returns an `ITriangleMeshGeometryIndexed` — the Catmull-Rom/Frenet sweep math it shares lives at `RISE_API.cpp:969-1140`. Baked at construction; no recipe retained |
| **Analytic curved** (Sphere, Ellipsoid, Torus, Cylinder) | **Exact, free — but not wired** | Bake or analytic | Analytic (closed-form ray-through) | Genuine Weingarten maps exist but are not written into `ri.derivatives` — see §5.4 |
| **Planar** (Box, InfinitePlane, ClippedPlane, CircularDisk) | **Correctly zero** | Bake | Analytic | Nothing to do |
| **Patches** (Bezier, bilinear) | **STUB — reports flat while genuinely curved** | Bake | — | `dndu/dndv` hard-zeroed (`BezierPatchGeometry.cpp:402-414`, `BilinearPatchGeometry.cpp:397-409`); closed-form second partials exist, unwired |
| **`displaced_geometry`** | mesh-family via the baked mesh; **plus** an analytic escape hatch | inherits mesh | inherits mesh | `Realize()` bakes to an internal indexed mesh; `ComputeAnalyticalDerivatives` (`DisplacedGeometry.cpp:407+`) chain-rules base derivatives against the displacement painter with a `smoothing` knob — tessellation-independent, but only for analytic bases |
| **`hair_geometry`** | Out of scope for v1 | Out of scope | Out of scope | Strand shading has its own tangent-frame contract; curvature of a fibre is not the wear signal |

The table drives the phasing directly: **SDF is strongest on all three signals
and needs no rays**, which is why M2 targets it first; **mesh curvature is free
today** and drives M1; **mesh AO/thickness need a bake**, which is M3.

---

## 5. M1 — differential curvature as an expression context variable

### 5.1 The computation

From `ri.derivatives`, with the orientation reference `n = ri.vGeomNormal`
(**not** `ri.vNormal` — see the sign discussion below):

```
E = dpdu·dpdu    F = dpdu·dpdv    G = dpdv·dpdv        (first fundamental form)
e = dndu·dpdu    f = ½(dndu·dpdv + dndv·dpdu)    g = dndv·dpdv
H = (e·G − 2f·F + g·E) / (2·(E·G − F²))               (mean curvature)
K = (e·g − f²)     / (E·G − F²)                        (Gaussian curvature)
```

**Sign convention.** The classical second fundamental form is `II = −dn·dp`;
this design uses the **negated** convention (`e = +dndu·dpdu`) so that an
outward-normal sphere of radius `r` reports `H = +1/r`. That is: **positive =
convex, negative = concave, zero = flat** — matching Arnold's convex/concave
split and composing algebraically: `clamp(curv,0,1)` is a wear mask,
`clamp(-curv,0,1)` is a crevice mask. The sign must be pinned by a unit test
against a known sphere, not left to derivation in a comment.

**The orientation reference must be `ri.vGeomNormal`, not `ri.vNormal`.**
Modifiers mutate the shading normal **in place, before material shading**:
`BumpMap.cpp:65` and `NormalMap.cpp:169` both assign straight into
`ri.vNormal`, so by the time `BuildContext` runs, `ri.vNormal` is
bump-perturbed. Orienting the shape operator off it would make `curv` flip sign
wherever a normal map tilts past the geometric tangent plane — a texture
artifact masquerading as geometry. `GlintModifier.cpp:224-225` is the in-tree
precedent for reaching past the perturbation to `ri.vGeomNormal` (including its
degenerate-`vGeomNormal` fallback to `vNormal`, which this should copy). This
is a **requirement**, not a preference — see §14 item 6.

*Honest caveat.* On a smooth-shaded mesh `vGeomNormal` is the flat face normal,
which is not exactly consistent with the Phong-interpolated `dndu/dndv` field
the magnitude comes from. That inconsistency is bounded and does not matter
here: **only the orientation enters the sign**, and the face normal and the
interpolated normal agree in orientation everywhere except at pathological
geometry (inverted or near-degenerate triangles, where `valid = false` is the
right answer anyway). The curvature **magnitude** comes entirely from the
derivative field, which no modifier touches.

Degenerate `E·G − F²` (a pinched parameterization, a pole) must return the
`valid = false` path, not a NaN. This is the fourth invariant of
[GEOMETRY_DERIVATIVES.md](GEOMETRY_DERIVATIVES.md) applied one level up.

Home: a small new header, e.g. `src/Library/Utilities/SurfaceCurvature.h`,
holding the shape-operator helper. **Cost note: any new file under
`src/Library/` must be added to all five build projects** — a header-only
helper still pays it. The list and the per-project rules are in
[CLAUDE.md](../CLAUDE.md) and [AGENTS.md](../AGENTS.md) → "Change Checklist".

### 5.2 Units, and why two variables

`H` has units of **1/length**. That is a real hazard for the adoption target:
an agent writing `clamp(curv,0,1)` on a creature whose features have 0.05-unit
radius gets `curv = 20` and a mask that is saturated everywhere. Every surveyed
tool avoids this — the baked tools emit a 0.5-centered `[0,1]` scalar, the
shade-time tools normalize by a user radius.

**Recommendation: expose two variables from one computation.**

- **`curv`** — dimensionless: `H` multiplied by the hit geometry's bounding-box
  diagonal. `O(1)` for a feature at object scale, so `clamp(curv,0,1)` and
  `clamp(-curv,0,1)` behave the way an author expects on any scene scale. This
  is the adoption-facing primary.
- **`curvR`** — the raw signed mean curvature in 1/world-length, for work that
  is genuinely physically scaled.

The normalization factor is one extra `Scalar` on `SurfaceDerivativesInfo`
(a `scaleHint`, defaulting to 1), stamped by the geometry at intersection time.
`SDFGeometry` already keeps `m_diagonal`; meshes have their bounding box.
**This does not touch the `ComputeSurfaceDerivatives` contract** — `SurfaceDerivatives`
(the return struct documented in [GEOMETRY_DERIVATIVES.md](GEOMETRY_DERIVATIVES.md))
and `SurfaceDerivativesInfo` (the record-riding twin) are separate types.

**`scaleHint` must be object-space bbox × the object's world scale.** A
geometry knows only its own object-space bounding box, and RISE shares one
`IGeometry` across instances at different world transforms — so an
object-space-only hint makes `curv` scale-*variant* across instances of the
same geometry, exactly the failure the normalization exists to prevent (and it
breaks radius-as-a-bbox-fraction in §9 the same way). **Fold the object's world
scale in at the `Object::IntersectRay` transform layer — the same site as the
§5.5 fix — via `|det M|^(1/3)`.** That is exact for rotations and uniform
scales, and it is the linear-measure sibling of the area Jacobian
`|det L|^(2/3)` that `Object::GetArea()` adopted in the 2026-08-13 fix
([SUBMERGED_CAMERA_IOR_SEEDING.md](SUBMERGED_CAMERA_IOR_SEEDING.md):39).
That doc also records the honest limit, which applies verbatim here: under
**non-uniform** scale no single scalar can be right — a `scale 4 0.05 4` panel
has no one characteristic length (:117-121). The hint is approximate there by
construction; say so in the descriptor rather than implying otherwise.

**Instancing.** With the world-scale fold — and assuming the §5.5 `dndu/dndv`
renormalization fix has also landed (both are Phase-1 deliverables, §13) —
**differential `curv` is per-instance-correct**: two instances of one geometry
at different scales report the curvature each actually has, because the shape
operator is built from world-space derivatives and normalized by that
instance's world scale.
**Baked per-vertex fields (M3) are not** — they are geometry-owned and
therefore *shared* across every instance, which is the correct and intended
sharing (the bake is a property of the mesh, not of a placement). The
consequence is that a **bake radius is object-space**: a `thickness(0.05)`
baked once means "5% of the object-space bbox diagonal" for every instance, so
the sampled world distance scales with the instance. Document that; do not try
to make a shared bake instance-relative.

Gaussian curvature (`curvK`) is derivable from the same four vectors at
negligible extra cost, but it has no established artist idiom and no surveyed
tool exposes it. **Defer it to Phase 4, gated on observed need.**

### 5.3 What this signal is, and is not

**Be honest about three limits, in the descriptor text as well as here:**

1. **It is fine-scale and radius-free.** This is *differential* curvature — the
   local Weingarten map — not Arnold's radius-sampled curvature, which
   integrates shape over a sampling sphere and is therefore tunable to "wear
   the 2cm edges, ignore the 2mm ones." A differential signal has no such knob.
   Sampled curvature is a Phase-4 item.
2. **On triangle meshes it is faceted**, and only as good as the vertex
   normals. The stored `dndu/dndv` derive from a per-triangle-constant
   vertex-normal-difference term (`TriangleMeshGeometryIndexed.cpp:1117-1177`;
   the stored field itself is additionally projected and renormalized per
   point, so it varies smoothly *within* a face), and that raw term jumps at
   every shared edge — so the signal is discontinuous across edges; a mesh
   whose normals were synthesized (PLY's `ComputeVertexNormals()`
   fallback) yields worse curvature than a glTF import carrying authored ones.
   This mirrors Blender's documented Pointiness limitation — quality coupled to
   vertex density, patchy on low-poly.
3. **On patches it is a lie.** `ComputeSurfaceDerivatives` is a stub for both
   patch types; they report perfectly flat while being genuinely curved. Until
   Phase 4 fixes that, keep patches on the `valid = false` path so `curv` reads
   0 as an *absence*, not a claim.

**Why it is still the right Phase-1 signal.** The authoring that motivated this
work is SDF- and creature-heavy, and **SDF curvature is smooth, high-quality
and resolution-free** — a genuine differential quantity on a genuine implicit
surface, with none of the faceting caveat. For that corpus it is not a
compromise but the correct signal; the mesh caveat is a bounded cost of
covering the rest of the zoo with the same expression.

### 5.4 Companion work: populating `ri.derivatives` more widely

Three mechanical extensions, no interface change:

- **Analytic primitives.** Sphere, Ellipsoid, Torus and Cylinder already
  compute genuine Weingarten maps in `ComputeSurfaceDerivatives`; they simply
  do not write them into `ri` at intersection time. Wiring that is per-geometry
  bookkeeping and makes exact closed-form curvature available essentially free.

  **AMENDED (2026-08-29) — "essentially free" was optimistic, and the wiring
  has two side effects worth naming.**  Cost: `ComputeSurfaceDerivatives` is
  closed-form but not arithmetic-free (a sphere hit pays ~6 transcendentals
  on top of the UV mapping it already does), and it is **ungated** —
  gating a *record field* on a global demand flag would make SMS's and
  NormalMap's behaviour depend on unrelated material authoring, which is
  worse than the cost.  Side effects, both improvements, both behaviour
  changes: **(a)** `NormalMap` on an analytic primitive now derives its
  tangent frame from `dpdu` instead of the arbitrary ONB fallback its own
  warning describes as "correct only when the normal map's UV axes happen to
  align with the arbitrary ONB frame — i.e. essentially never"; **(b)** SMS's
  `ComputeVertexDerivatives` reads the exact analytic frame instead of its
  geometry-agnostic finite-difference fallback (`sms_k2_glasssphere`
  re-rendered clean).  A third, purely internal: several `CSGObject` branches
  documented as UNREACHABLE *because* no analytic geometry populated
  derivatives are now live — the `dndu`/`dndv` negations they were already
  carrying are correct, the exit-face probe helper gained the sign re-pairing
  its own P1-1 audit note had flagged as needed-if-reachable, and
  `tests/CsgSurfacePayloadTest.cpp` Tests 18/19 were upgraded from
  documenting the unreachability to asserting the behaviour.
- **SDF hits.** Following the `jacobianAt` precedent, mean curvature from
  `div n̂` via one-sided FD of `GradientNormal` at three offset points —
  **18 extra `Map()` evaluations** beyond the hit's own 6, i.e. three extra
  normal computations. For SDFs it is likely better to compute **curvature
  directly** than to synthesize a full `dndu/dndv` pair, since the SDF has no
  natural `(u,v)` for which those partials are meaningful. That argues for a
  narrow `Scalar curvature` + `bool curvatureValid` on the record alongside
  `derivatives`, with `BuildContext` preferring the direct value when present
  and falling back to the shape operator otherwise.
- **Cost gating.** 18 extra `Map()` evals per hit, each `O(#parts)`, is not
  free on a heavy SDF. Gate the computation on consumption: compute it only
  when an expression that references `curv` exists (a compile-time-known
  property of `ExpressionProgram`, since the Builder already resolves every
  name).

  **AMENDED (2026-08-29) — the shipped mechanism.** The compile-time half is
  as sketched: `ExpressionProgram::Builder` records a bitmask of the context
  variables any body it compiles (defs included) actually resolved, and the
  program exposes `UsesContextVar(slot)` / `UsesSurfaceCurvature()`.  The
  *delivery* half is **not** a per-material lookup at the hit, and not a
  scene/job flag threaded to the geometry, because **a geometry cannot reach
  either**: `IGeometry::IntersectRay` receives only a
  `RayIntersectionGeometric`, which carries no scene, object or material
  back-pointer, and RISE deliberately shares one `IGeometry` across objects
  and scenes.  The two routes that *would* reach it were both rejected:
  a new per-cast INPUT field on the record (the `bWantsWireEdgeInfo`
  pattern) needs every caster to stamp it, and a missed caster silently
  disables the feature on that path; a painter→material→object→scene
  aggregation walk does not exist and would be a project of its own.

  What shipped instead is `SurfaceCurvatureDemand`
  ([SurfaceCurvature.h](../src/Library/Utilities/SurfaceCurvature.h)): a
  **process-wide `std::atomic<int>` demand counter**, RAII-incremented by
  `ExpressionPainter` / `ExpressionScalarPainter` at construction when their
  compiled program reads `curv`/`curvR`, decremented at destruction.
  Geometry asks `SurfaceCurvatureDemand::Any()` — one relaxed atomic load —
  at intersection time.  It gates the SDF finite-difference stencil **and**
  the `scaleHint` stamping in every family (nothing but `curv` reads
  `scaleHint`).

  Its one documented weakness is **conservatism, never incorrectness**: a
  curvature-reading painter alive anywhere in the process enables the
  computation for every scene in it, which matters only to the GUI/MCP
  surface that holds several scenes at once, and costs performance rather
  than correctness (the computed value is a pure function of the hit).  It
  is thread-safe by construction — mutated only at painter construction and
  destruction, i.e. at scene build and teardown, outside the render phase;
  render threads only load it, with no lock and no per-sample
  synchronization.  Pinned by `tests/SurfaceCurvatureTest.cpp` (g), which
  asserts the SDF publishes **no** curvature with the gate closed and the
  right value with it open, that a `def`-only reference still raises demand,
  and that a user `param` named `curv` (which shadows the context variable)
  does **not**.

### 5.5 A correctness item this work exposes

`Object::IntersectRay` transforms `dndu/dndv` to world space by the
inverse-transpose (`Object.cpp:764-772`) **with no renormalization term**. That
is correct for a rigid transform and wrong under scale: the true derivative of
the *unit* world normal is

```
n_w      = normalize(M⁻ᵀ n)
dn_w/du  = (I − n_w n_wᵀ) · (M⁻ᵀ dndu) / ‖M⁻ᵀ n‖
```

The code applied `M⁻ᵀ` and stopped. Under a uniform scale `s` this yielded
`dndu/s` where the correct answer is `dndu`, propagating to a factor-`s`
error in the second fundamental form and reported curvature. **FIXED, 2026-08-29**
(commits `3f495c25..da92af3e`, merged to master): the quotient-rule transform now
lands at `Object::IntersectRay`, `Object::ComputeAnalyticalDerivatives`, and
`CSGObject::IntersectRay`, with `NEARZERO` singular guards; the factor-`s` claim
was **confirmed empirically** by a scaled-sphere test (no longer derived-only).
The fix is a no-op for rigid transforms (`‖M⁻ᵀ n‖ = 1`, and the projection
removes only a component that is already zero). The review loop additionally
surfaced a **sign-pairing bug family** beyond this doc's scope — every site that
negates `vNormal` while carrying live derivatives must negate `dndu/dndv` too
(three CSG_SUBTRACTION cavity-wall branches + both double-sided-mesh back-face
flips were shipping wrong-signed curvature); all fixed in the same arc, and
[GEOMETRY_DERIVATIVES.md](GEOMETRY_DERIVATIVES.md) now carries the world-space
transform clause and the CSG-subtraction handedness exception. `ManifoldSolver`
(`ManifoldSolver.cpp:2720-2721, 2765-2766`) was regression-checked as part of
that arc.

---

## 6. M2 — lazy per-hit signal queries for the SDF family

### 6.1 The mechanism

Two arg-taking builtins: `occlusion(radius)` and `thickness(radius)` (a third, `convexity(radius)`, joined them 2026-09-06 through the same channel). All need
per-hit geometry access, which the static `CallFunc` cannot reach today. Two
pieces:

1. **A `const` signal-provider hook on `RayIntersectionGeometric`** — a typed
   `const ISurfaceSignalProvider*` populated at intersection time by geometries
   that can answer these queries. Cheap (one pointer store), typed (unlike
   `pCustom`), and `const` throughout. This is the narrow, principled version
   of the `PainterContext` refactor the `pCustom` comment (:176-183) has been
   asking for since the 3DS Max era — not the whole refactor, one typed
   channel.
2. **A `const` context pointer threaded into `CallFunc`** — assessed as a
   moderate and `const`-safe VM change. `CallFunc` gains a
   `const ExprEvalContext*` (or a narrower provider pointer) parameter; the
   existing static `fw` read is unchanged; every current builtin ignores it.

**The provider interface is the single, family-independent dispatch channel for
`occlusion` and `thickness`.** It is not an SDF mechanism that meshes later
duplicate: §7.1's mesh bakes are delivered through *this same interface*, via a
trivial bake-backed provider. The VM sees one call shape; which geometry family
answered it, and whether the answer came from a live field evaluation or a
baked field, is entirely behind the interface. That is what makes "the same
builtin names" a real portability claim rather than a naming coincidence.

**SHIPPED 2026-08-29** — [`src/Library/Interfaces/ISurfaceSignalProvider.h`](../src/Library/Interfaces/ISurfaceSignalProvider.h).
Two halves, as sketched:

```cpp
class ISurfaceSignalProvider {          // per-geometry, stateless, const
  virtual bool ComputeOcclusion( const Point3& ptObject, const Vector3& nObject,
                                 Scalar radiusFraction, Scalar& outValue ) const = 0;
  virtual bool ComputeThickness( ... ) const = 0;   // same signature
};
struct SurfaceSignalInfo {              // the per-hit field on the record
  const ISurfaceSignalProvider* pProvider;   // 0 == this surface publishes none
  Point3  ptObject;  Vector3 nObject;        // the PROVIDER's own object space
};
```

Three properties worth naming because they are what make the channel work
unchanged for Phase 3: the methods **return bool and may refuse** (which is
§7.1's mismatch contract, expressible without a second entry point);
`SurfaceSignalInfo` owns the fallback / clamp / finiteness policy, so no caller
can invent its own convention; and queries are posed in the **geometry's own
object space** with a **dimensionless** radius and dimensionless outputs, so
nothing crosses the transform boundary. `Object::IntersectRay` and
`CSGObject::IntersectRay` therefore leave the field untransformed — CSG only
re-adopts it in `AdoptCsgSurfacePayload`, alongside `derivatives`, when the
algebra credits the reported boundary to the other operand.

### 6.2 The SDF estimators

Both are **pure `const` evaluations of the distance field** — no rays, no scene
access, no lock. `SDFGeometry::EvaluateParts` is a public static
(`SDFGeometry.cpp:971-987`, declared `SDFGeometry.h:161`), so the query is a
pure function of the parts list and a point.

- **Cavity / occlusion** — ~~the Evans-style estimator: ~5 taps at
  geometrically increasing distances along the normal~~. **SUPERSEDED
  2026-09-06** by a ball-ACCESSIBILITY estimator, and the `convexity(radius)`
  builtin was added as its other half — see
  [OCCLUSION_CONVEXITY_AND_EDGE_SIGNAL.md](OCCLUSION_CONVEXITY_AND_EDGE_SIGNAL.md).
  The normal-line form read a shortfall in `|map|`, which is exact only where
  `map` is the exact Euclidean distance; RISE's composed field is a
  conservative lower bound, so at a convex CSG edge (hard `max`) it returned
  `cos γ` — 0.707 at a plain 90° arris, on surface with no cavity — and
  returned that *same* 0.707 in a concave 90° valley. `O(80 × #parts)`, no
  rays.
- **Thickness** — sample the field along the *inward* normal, or short-march to
  the far zero crossing. The SDF is the one family with first-class "distance
  to the other side" already in hand.

Both are **object-local self-signals** — they see the hit object's own parts,
nothing else. That is exactly the v1 scope (§8), and it is what makes them free
of scene access.

**Lazy is the whole point.** The query fires only when the expression actually
calls it, so an SDF creature whose material never mentions `occlusion` pays
nothing. This is the inverse of the M1 gating problem: curvature is a context
variable and must be computed *before* the painter runs, so it needs a
consumption predicate (§5.4); M2's queries are demand-driven by construction
and need none.

**SHIPPED 2026-08-29** — `SDFGeometry::ComputeOcclusion` /
`ComputeThickness`, ~6 and ~O(march) `Map()` evaluations respectively, both
pure `const`. Two details the sketch above left open, both settled by making
the arithmetic exact rather than tuned:

- **The cavity taps and weights are a matched pair.** `h_i = R·2^(i−N)`,
  `w_i = 2^(1−i)` makes every `w_i·h_i` equal, so each octave contributes the
  same share of the normalizer `Σ w_i·h_i` and the estimator has no preferred
  scale inside the query radius. That fixes IQ's tuned `3.0·occ` constant into
  a derived one, and both ends then come out exact: a plane or convex body has
  `map(p + h·n̂) = h` at every tap and reads **1**; a point whose every tap
  lands on the surface reads **0**. The sphere-trace band residual `map(p)` is
  subtracted off each tap — without it, a small radius on a large object reads
  that residual as occlusion and darkens a perfectly convex surface.
- **It sees creases, not spherical pits.** Still true after the 2026-09-06
  rewrite, though for a different reason. The normal-line estimator was blind to
  a dimple because the field reported no shortfall inside a sphere; the
  directional estimator is blind to a *shallow* one because a ray at angle φ
  from the inward normal crosses a chord of `2ρ cos φ`, so nothing is blocked
  until the query radius approaches the pit's own diameter (closed form:
  `occlusion = 1 − (R/2ρ)²`). Measured on the `materials-and-media-basics` head
  fixture, the scar floor still reads **1.0**; the knot seam — a genuine fold —
  moved from 0.68 to **0.50**, i.e. the old reading squared, which is exactly
  what §2.1 of the new document predicts. Creases, corners and folds are what
  darken, and they now darken more.

Thickness marches inward through the intersector's own `March()` (whose
on-surface step-off is what keeps the entry face from being reported as the
exit) and divides by the query radius. "No far side within the radius" returns
a saturated **1**, not a refusal — that is a measurement of thickness, not an
absence.

**Smooth-blended parts soften the Lipschitz-1 exactness locally (minor,
unlike the heightfield case below).**  *(Moot since 2026-09-06: the
accessibility estimator reads only the SIGN of the field, which is exact for
the surface actually rendered, so field-magnitude softening no longer reaches
occlusion at all.)* The exact-1-on-convex/flat property
above assumes `map(p + h·n̂) = h`, which holds for a hard union but is only
*approximately* true within a `smin`/`smooth-subtract` blend radius (`k > 0`):
the polynomial blend smooths the field's gradient magnitude near the seam, so
a tap landing inside another part's blend radius reads a slightly softened
distance rather than the exact one. This is bounded by the blend radius itself
(negligible once taps clear it) and is a world apart from heightfield mode's
issue below — a *local*, self-correcting softening near an authored blend,
not a *global* mis-scaling that misreads an entire flat, unrelated region of
the field.

---

## 7. M3 — baked per-vertex fields for meshes

### 7.1 Who owns the bake, and how it is dispatched

The bake is **geometry-owned**: signals are a property of realized geometry
(config-independent — the `DerivedScene` layer of the derivation engine), not
of the render configuration. **When** it is built is settled in §7.3 (lazily,
at first production shading access); this section covers ownership and
dispatch, which are independent of that choice. Either way it follows the
idempotent `Object::Realize` / `DisplacedGeometry::Realize` discipline: the
realize walk runs on **every** `AttachScene` (`RayCaster.cpp:208`), including
same-scene-pointer re-attaches, precisely because "an interactive editor can
swap a fresh (unrealized) geometry onto an object and re-render the SAME scene
pointer" (`RayCaster.cpp:213-221`).

**Dispatch: mesh geometries install a bake-backed `ISurfaceSignalProvider`**
(§6.1) — the same interface the SDF family implements, so there is exactly one
channel from the VM to a geometry signal and no second mechanism to keep in
sync. The mesh provider's `occlusion(radius)` / `thickness(radius)` is a
handful of lines: check the radius precondition below, find-or-build the
table, interpolate, return it.

**Interpolation timing — AMENDED 2026-08-29, at implementation.** This section
originally said **interpolate at intersection time**, so that the provider held
one ready scalar per signal instead of a back-pointer into mesh storage — the
`VertexColorPainter` pattern (per-vertex data → intersection interpolates into
the record → a two-line read). That sentence predates §7.3's decision to build
the bake **lazily**, and the two cannot both hold: intersection strictly
precedes shading, and a lazy bake does not exist until shading first asks for
it, so on the first-ever query there is by construction nothing to interpolate.

What ships instead keeps the half of that argument that was actually
load-bearing — *the barycentric arithmetic belongs to the intersector's frame*
— and moves only the lookup:

- the mesh intersector stamps **where** the hit is: triangle index plus the two
  barycentric weights it has already computed for normals, UVs and vertex
  colours (`SurfaceSignalInfo::primId` / `baryA` / `baryB`);
- the provider interpolates **on demand**, after its find-or-build.

This is strictly *cheaper* at intersection than the original sketch — one int
and two scalars, no table lookup, and nothing at all for a hit whose material
never calls the builtins — and the draft-mode guarantee survives intact. The
SDF family leaves `primId` at its `-1` default and answers positionally, which
is what makes one record field serve both dispatch styles.

**The radius argument must be a compile-time constant on the baked path.** M2's
SDF estimators evaluate any expression per hit, but a bake commits to **one**
radius — a mesh cannot answer `occlusion(fbm(P)*0.1)` from a baked field. Two
halves of the contract:

- **Precondition.** The `radius` argument must be a literal or a scene-constant
  (`param`/`def` resolving to a constant), detected at parse time. `ParseCall`
  proves the *pattern* — parse-time rejection of a bad literal argument with a
  per-argument diagnostic, as done for `fbm`'s octave count
  (`ExpressionEval.h:917-948`) — but that check only recognizes a bare numeric
  token in argument position. Resolving a `param`/`def` name back to a
  compile-time constant is **new work** (tracing the identifier through its
  declaration and confirming the bound expression is itself constant) that this
  doc scopes to Phase 3 but does not design.
- **Mismatch contract.** If the requested radius is non-constant, or differs
  from the baked radius beyond a documented tolerance, the mesh path returns
  the signal's **honest fallback** — the same `valid = false` → neutral-value
  convention used for `fw`, for unpopulated derivatives, and for unbaked
  geometry throughout this design. It **never** silently substitutes the baked
  radius. A wrong-scale mask that looks plausible is worse than a flat one that
  is visibly absent.

The resulting M2/M3 asymmetry is documented, not hidden: on the SDF family
`radius` is a live knob; on meshes it is a bake parameter the lookup validates
against.

**What shipped, 2026-08-29 (Phase 3), and the two places it is sharper than
the sketch above.**

*The proof travels with the call, because it cannot be recovered later.* By the
time a `Scalar` radius reaches a provider, "0.05 written in the scene text" and
"the current value of `fbm(P)*0.1`" are the same bit pattern — nothing at the
provider can tell them apart. So the compiler's per-call-site literal proof is
**emitted into the instruction**: a signal builtin whose radius was not proven
literal is emitted as an internal `...DynR` twin id (`ExpressionEval.h`'s
`kFnOcclusionDynR` / `kFnThicknessDynR`), and `CallFunc` forwards that as the
`bRadiusIsConstant` argument on the interface. Without it the baked path would
have to either answer a computed radius from whatever table it had (the silent
wrong-scale substitution this section forbids) or bake per distinct value —
unbounded work, and a render whose output depends on which radius arrived
first, i.e. a reproducibility break. The `param`/`def` constant-resolution work
the bullet above scopes to Phase 3 was **not** done: an unproven-but-genuinely-
constant radius such as `0.15*2` is treated as dynamic and refused, which is
the conservative direction.

*One bake per (geometry, signal, radius), with a bounded map rather than a
single first-wins table.* Distinct **literal** radii are few and come from the
scene text, so each gets its own table; the alternative — refusing every radius
after the first — makes which radius won depend on thread arrival order. The
lookup matches on a **1e-4 relative tolerance** (two spellings of one literal
match; 5 % and 5.1 % do not), and the map is capped at **8 tables per signal
per geometry**, past which further distinct radii are refused with a one-time
warning. That cap is the one non-determinism in the feature, reachable only by
a scene that spells more than eight distinct radii of one signal against one
mesh; unbounded growth was judged the worse failure.

*Scope: `TriangleMeshGeometryIndexed` only.* It is what every mesh path in the
engine produces — every loader but the legacy RAW one, `TessellateToMesh` for
analytic primitives and SDFs, and `DisplacedGeometry`'s internal mesh (so a
`displaced_geometry` wrapping any base answers baked signals for free). The
non-indexed `TriangleMeshGeometry`, reachable only via
`RISE_API_CreateTriangleMeshGeometry` / the RAW loader, publishes **no
provider** and reads the neutral fallback — an honest absence, not a silent
zero.

### 7.2 Invalidation is free — with one trap

Geometry edits in the incremental derivation path are **drop-and-recreate**:
`Cst::DeriveToJobIncremental` (`Cst.cpp:3359`) drops the chunk
(`DropChunkByCategory` :1474 → `pJob.RemoveGeometry(name)`) and re-finalizes
into a **brand-new `IGeometry*`** (:3785, comments :3465-3474). A new geometry
starts with an empty bake, and the idempotent realize walk notices. Transform-only
object edits leave geometry untouched — correct, because these are object-local
signals. **Nothing needs a dirty flag.**

**The trap, named explicitly:** object chunks are **re-pointed in place** (same
`IObject*` address — recreating them was the P1.1 use-after-free, :3463-3470).
So a cache keyed by `const IObject*` does **not** get free invalidation from a
geometry swap. The `SubSurfaceScatteringShaderOp` `PointSetMap` is keyed exactly
that way (`src/Library/Shaders/SSS/SubSurfaceScatteringShaderOp.h:72`) and is
the cautionary precedent. **Key the bake on the geometry, not the object.**

**A SECOND trap, found at implementation (2026-08-29), which the argument above
does not cover.** Free invalidation rests on vertex data being immutable for
the lifetime of an `IGeometry*` — §14 item 10 flagged that assumption and asked
for it to be *confirmed*. It is **false**:
`TriangleMeshGeometryIndexed::UpdateVertices` replaces the vertex and normal
arrays **in place** and refits the BVH (the keyframed-painter
`DisplacedGeometry::RefreshMeshVertices` path), so one geometry genuinely does
change shape over time while keeping its address. A bake computed for one frame
would be carried into every later one. That path — and every other vertex
mutation site (`BeginIndexedTriangles`, `DoneIndexedTriangles`,
`ComputeVertexNormals`) — now drops the tables **explicitly**, under the same
between-frames contract `UpdateVertices` already has. Free invalidation covers
the *derive* path, not the *animation* one; both are needed.

**And that between-frames contract is itself violated by one pre-existing
path** — motion blur calls `EvaluateAtTime` from worker threads
([ARCHITECTURE.md](ARCHITECTURE.md):68-74), which reaches `UpdateVertices`
through a keyframed displacement painter. The bake tables are reference-counted
so an invalidation there cannot free one under a reader; the vertex mutation and
BVH refit underneath still race traversal, and still do after this work. See
"Phase 3 fix round" below for exactly what was and was not fixed.

### 7.3 Gating, and when the bake actually fires

Gate the bake on consumption — bake only if some material in the scene actually
references the signal, the `ConsumesScenePhotonMaps` pattern. Unbaked geometry
answers `valid = false` and the expression reads the documented fallback.

**But consumption gating alone does not make draft mode free, and an earlier
draft of this section wrongly claimed it did.** The gate keys off the material
graph, which is config-independent; a `Realize`-seam bake therefore fires on
**whichever render comes first** once a signal-consuming material exists — and
in the agent edit loop that first render is very often a `quality:"draft"`
preview. Draft would then pay for a bake whose output it can never display,
because the draft pipeline ignores all authored materials and lighting
(`AgentMcpAdapter.cpp:1904`, `AgentSession.h:660`).

**Resolution: build mesh bakes lazily, at first *production* shading access,**
using the sanctioned SSS-style `mutable` cache + `RMutex` find-or-build
(`ARCHITECTURE.md` §Known Exceptions "SSS Pointset Lazy Initialization",
:60-66). This is not a convenience use of the exception — it is correct here
for the same structural reason it is correct for SSS: **the signal is only ever
read from material shading, and draft never executes material shading.** Draft
becomes bake-free *by construction* rather than by assertion, and the bake
happens exactly once, when something is actually going to look at it. It earns
the same discipline as the SSS precedent: a null-sentinel cache-on-failure so a
failed build is not retried per-sample, and a lock held only across the
find-or-build.

**Alternative, if the lazy path proves awkward:** eager baking at the `Realize`
seam, accepting that a draft preview may pay for it. Prefer lazy; fall back to
eager only if the mutex-guarded path complicates the geometry lifetime more
than it is worth. Either way, the immutability rule (`ARCHITECTURE.md:5-9`)
must be honoured — a lazy build under `lock_guard` is an exception to it, not
an exemption from thinking about it.

**Shipped 2026-08-29: lazy, as prescribed** — the fallback was not needed. The
cache lives on the mesh geometry (`MeshSignalBakeCache`), one `lock_guard`
spans the whole find-or-build, failures cache a null sentinel, and the
interpolation runs outside the lock on the now-immutable table. Two additions
the sketch did not anticipate, both forced by the lazy choice rather than
optional:

- **The input assembly is itself lazy**, behind an `IInputSource` callback the
  cache invokes *inside* its lock. A mesh's bake input includes derived state
  it also builds once (a per-**position** normal array, which its independently
  indexed `pNormals` cannot supply and which does not exist at all on a
  face-normal mesh) — so assembling the input is a `mutable` write that must be
  serialized, and doing it eagerly on every cache *hit* would put the very work
  the lazy bake exists to avoid back on the per-sample path.
- **The bake must be deterministic**, which the exception's SSS precedent
  learned the hard way (its build once captured the winning thread's RNG state
  and varied run to run). The bakes use a fixed Hammersley pattern with a
  per-vertex-index golden-ratio rotation — no RNG, no wall clock — so the table
  is byte-identical no matter which thread built it or when.

`ISurfaceSignalProvider`'s thread-safety contract was widened to say this out
loud: it previously required implementations to be pure `const` functions with
"no locks, no caches, no rays," which the mesh family cannot satisfy and which
would have made the shipped provider look like a violation rather than the
sanctioned second shape it is.

---

## 8. Scene-wide AO: DECLINED for v1

Cross-object occlusion — a wall darkening the floor beside it — is the one
variant that breaks everything above.

- **It destroys the free-invalidation story.** Moving a neighbouring object
  must invalidate the receiver's bake, and the traced-edge model in the
  derivation engine (`20-derivation-engine.md` §2.2) has **no
  geometry-proximity edges**. Building them means inventing a spatial
  dependency graph, which is a project of its own.
- **Every surveyed tool ships a local-only mode**, because artists want it:
  Blender's AO node has **Only Local**; V-Ray's `VRayDirt` has "Consider same
  object only"; Substance's AO baker has self-occlusion Always / By Mesh Name;
  Arnold's `aiCurvature` has `self_only`. Self-occlusion is not a degraded
  mode, it is a first-class one.
- **Self-occlusion covers the motivating use case.** Grime in crevices, patina
  in a casting's recesses, dirt in a seam — all are self-occlusion.

**Later-phase route, explicitly out of scope now:** either the SSS-style
lazy + mutex shader-op route, or bake-time `IObjectManager::IntersectShadowRay`
(`IObjectManager.h:46-51`) against the whole scene — the cheapest occlusion
primitive available, boolean-only, far cheaper than a full `IRayCaster::CastRay`.

---

## 9. Parameter surface and conventions

Grounded in the external survey. **Convention choices should match the tools
authors already know**, because that is what an LLM has read.

| Signal | Range / sign | Convention source | RISE proposal |
|---|---|---|---|
| Curvature | signed; **+ = convex, − = concave, 0 = flat** | Arnold `aiCurvature` splits convex→R / concave→G; Substance bakes 0.5-centered (black = concave, white = convex); Blender Pointiness centers at 0.5 | **Signed scalar** — algebraically composable: `clamp(curv,0,1)` = wear, `clamp(-curv,0,1)` = crevice. Signed beats 0.5-centered for expression math; an Arnold-style split mode is a Phase-4 convenience |
| Occlusion | `[0,1]`, **1 = unoccluded** | Universal — Blender ("white = unoccluded"), Substance AO baker ("white = unoccluded"), V-Ray unoccluded_color | `occlusion(radius) → [0,1]`, 1 = unoccluded. Keep it a pure exposure measure; do **not** replicate V-Ray's dual inside/outside mode — cavity is served by `clamp(-curv,0,1)` |
| Thickness | `[0,1]` normalized by radius, **1 = thick** | Substance Thickness-from-Mesh ("black = thin, white = thick") | `thickness(radius) → [0,1]`, 1 = thick |
| Convexity (**added 2026-09-06**) | `[0,1]`, **0 = flat or concave, 1 = knife edge** | The radius-sampled half of the same measurement occlusion reports; Substance/Blender expose only the AO half | `convexity(radius) → [0,1]`. With `A` the fraction of the query ball outside the solid, `occlusion = clamp(2A,0,1)` and `convexity = clamp(2A−1,0,1)` — one estimator, two clamps. Fixed geometric meanings (0.5 = a 90° arris, 0.75 = a three-face corner) at every scale, which `curv` cannot offer. See [OCCLUSION_CONVEXITY_AND_EDGE_SIGNAL.md](OCCLUSION_CONVEXITY_AND_EDGE_SIGNAL.md) |

**`radius` is the one mandatory artist knob**, and it is scene-scale-relative.
No universal fixed default exists across the surveyed tools. **Recommendation:
default the radius relative to the hit geometry's bounding-box diagonal** (the
same `scaleHint` M1 introduces), overridable by an explicit value. That is the
scene-scale-independent behaviour, and it means `occlusion(0.05)` reads as "5%
of the object" rather than "5 world units." On the baked mesh path it must
additionally satisfy the constant-radius precondition of §7.1.

**SHIPPED 2026-08-29, with one refinement: there is no *default* radius.** The
argument is mandatory (arity 1), because a scene that silently inherits a wrong
radius is worse than one that fails to compile. The fraction is taken of the
geometry's own bounding-box diagonal *inside the provider*, in object space, so
it never depends on `scaleHint` being populated and is instance-scale-invariant
by construction. A **literal** radius ≤ 0 is a compile error naming what the
number means; a computed one that lands ≤ 0 is refused at runtime and returns
the neutral value.

**The neutral values are `occlusion = 1` and `thickness = 1`** — for both, the
*do-nothing* end of the range, so an absent signal lights nothing up. Occlusion
is uncontroversial (absent and unoccluded agree). Thickness's choice is argued
from the use case, not from symmetry: a thickness mask exists to key *thin*
regions (`1 − thickness(r)` drives translucency, subsurface tint, edge
scatter), so a neutral 0 would set every unbaked object glowing.

**`samples` is an optional advanced argument only where sampling is
stochastic** — i.e. the mesh bake. The SDF estimators are **fixed-tap**, so no
`samples` argument is offered there; exposing a knob that does nothing on half
the geometry zoo is worse than not exposing it.

**Deferred secondary parameters**, all present in the surveyed tools and all
Phase-4-at-earliest: `spread` / `cosineSpread` (hemisphere bias toward the
normal), `threshold` (suppress low curvature), `bias` (falloff shaping),
`attenuation`, `trace_set` (per-signal inclusion sets).

**All three signals are auxiliary queries, fully decoupled from NEE and MIS.**
Every shade-time tool surveyed — Arnold, RenderMan, V-Ray, Corona — fires
independent auxiliary rays with no participation in the integrator's light
sampling, and Blender's AO node is explicitly decoupled from the path tracer's
light integral by design. This is matching industry practice, not a shortcut,
and it is the conservative reading of [MIS_HEURISTICS.md](MIS_HEURISTICS.md)'s
caution: nothing here perturbs a PDF, so nothing here can unbalance a weight.

---

## 10. Expression route vs. dedicated painter chunk

Both routes are real. They differ mainly in what they reach.

**A dedicated `curvature_painter` chunk** would need: one `IScalarPainter`
class, one descriptor + `Finalize` in
[ChunkParserRegistry.cpp](../src/Library/Parsers/ChunkParserRegistry.cpp), a
`Job::Add*` plus its `IJob` declaration, a `RISE_API_Create*` declaration, and
all five build projects. The `ramp_painter` end-to-end shape is the worked
precedent (`ChunkParserRegistry.cpp:6570-6675` parser, :12966 registration,
`Job.cpp:1365-1421` job layer, `RISE_API.h:685-691` factory).

**The expression route** — context variables plus builtins — reaches **every
existing expression body** with no new chunk keyword. Both routes share the
same underlying work (the shape operator, the `ri` population, the provider);
they differ only in the surfacing layer, so the delta between them is the
chunk registration above versus the five-site `ExpressionEval.h` edit. Neither
delta dominates, which is why the choice below turns on reach and adoption
rather than on cost.

**Recommendation: the expression route is PRIMARY; no new chunk in v1.**

- **C-TEXT, the governing adoption finding:** linear text beats
  graph-as-data for LLM authorship (76% vs 53%, arXiv 2409.00856; doc 85). New
  signals should be string tokens inside chunks that already exist, not new
  deep graph node types.
- **Delivery is free.** Builtin and context-variable names ride the
  `expression_painter` `cd.description`
  (`ChunkParserRegistry.cpp:~6552`), which already enumerates the noise
  builtins and the context variables `u, v, P, Po, N, fw, time`. That
  descriptor reaches every agent through `read_schema` at zero marginal cost —
  the highest-ranked delivery channel in the measured record.
- **Composition already works.** `scalar_painter { expression "curv" }` yields
  an `IScalarPainter` that plugs into the physical-scalar pipe
  ([ISCALARPAINTER_REFACTOR.md](ISCALARPAINTER_REFACTOR.md)); `expression_painter`
  composes on the colour side. Nothing is missing for the common wear-mask
  shapes.

**One real gap to note, not to fix now:** `BlendPainter`'s mask is
`const IPainter&` (`BlendPainter.h:45,124-136`) and `ramp_painter`'s input is
"ANY colour painter" (descriptor :6663) — both colour-side. Feeding a scalar
signal into them needs a `ScalarToPainterAdapter`, the reverse of the existing
`PainterToScalarAdapter`, which does not exist. In practice an author routes
around it by writing the mask directly as an `expression_painter` (where `curv`
is in scope and the result is a colour), so this is a convenience gap, not a
blocker. If a chunk is ever wanted, `ramp_painter` is the shape to copy.

---

## 11. Adoption wiring

New capability that no one uses is not shipped. The measured laws (doc 88 §2 /
§7-8, doc 75, `CREATIVITY_JOURNAL.md`) are specific about what works:

- **C-ADV — advice ≈ 0.** Design-note prose naming `scalar_painter` fired up to
  30× per session for **0/24 lifetime adoptions**; voluntary tools 0/64.
  **Do not plan on a design note.**
- **C-TYPE — the failure is a typing prior, not ignorance.** "Roughness is a
  number" survived worked examples (painter diversity rose 1→3; spatially-varying
  roughness stayed 0/24). **"Masks are made of `P.z`" is the same prior**, and
  it is the exact risk this feature runs.
- **C-READ — the read-set is the knowledge boundary.** gemini pulls 0–2 skills,
  gpt 6; `procedural-textures.md` read **0/6**. The one skill both providers
  read is `object-modeling-recipes.md`.
- **C-VERB — when advice fails, ship a verb.** `vary_material` was
  census-confirmed 3/3 once it existed as a callable.
- **Master triad:** summoned category, price the inferior path, exactly one
  worked example that parses, and let the census say whether it moved.

**The plan:**

1. **Descriptor text (channel 1, free, always delivered).** Add `curv`/`curvR`
   to the `expression_painter` `cd.description` context-variable list and
   `occlusion`/`thickness` to its builtin enumeration
   (`ChunkParserRegistry.cpp:~6552`), plus `kBuilderGrammarKeywords`. State
   the sign convention, the scale normalization, and the baked-path
   constant-radius rule *in the descriptor* — that text is the only
   documentation guaranteed to arrive.
2. **One execution-validated worked example** in
   `skills/agent/materials-and-media-basics.md`, modeled on the load-bearing
   "Roughness is not a number — bind it to an expression" section (:302-407):
   **patina in the crevices, done right** — a concave-curvature-driven ramp
   mixing darkened, roughened patina over a base material, on an **SDF
   creature** (the family where the signal is highest quality). Validated to
   the doc 88 S5 standard: parse + derive + render + luma band.
3. **A short second example** in `object-modeling-recipes.md`, the only skill
   both providers read — edge wear in four lines.
4. **Pointer-only in `procedural-textures.md`** (0/6 pull rate).
5. **Census-gate the result** with the existing checker ops
   (`any_param_references_kind`, `distinct_chunk_kinds`,
   `objects_reaching_kinds`), **N=3 minimum, cross-provider before believing a
   null**, stop rules pre-committed.

**Honest risk, and the escalation.** A signal usable only inside an expression
may hit the C-TYPE prior head-on: an agent that believes masks are built from
positions will not reach for `curv` merely because it exists. The worked
example and descriptor enumeration are the *measured* mitigations, and they are
not guaranteed. **If the census misses, the escalation is a verb** — an
`add_wear`-class call in the `vary_material` mold (one qualifying predicate
feeding both a design-note clause and a zero-required-arg verb with
refuse-on-no-op semantics). **OUT OF SCOPE here**, but the hook points are
named so the escalation is cheap: the predicate and scaffold sites in
`AgentSession.cpp` (~:5749-5795, ~:9532-9934) and the parity-tested descriptor
pair in `AgentMcpAdapter.cpp` + `AgentChatCodecs.cpp` (the two-name-lists
lesson, commit `1ed4e7c3`, doc 88 §8 S5).

**CENSUS RUN (2026-08-29)** — `evals/runconfigs/curv_census_gemini_gpt.json`:
`rich_material_closeup` (the doc-88 S6 instrument — its prompt is a *weathered*
brass doorknob, wear summoned without naming a mechanism) × 3 repeats ×
{gemini-3.5-flash, gpt-5.6-terra}, single run by user direction (token-lean).
Post-hoc over the committed scenes (`evals/runs/curv_census/`):

| | curv | occlusion | position proxy | expression chunks |
|---|---|---|---|---|
| gemini r1 | 0 | 0 | 0 | 1 |
| gemini r2 | **8 refs** | 0 | 1 | 4 |
| gemini r3 | 0 | 0 | 2 | 2 |
| gpt r1-r3 | 0 | 0 | 0 | 1/1/0 |

**Reading.** (1) **Delivery worked 6/6** — every trajectory carries the `curv`
descriptor text via `read_schema`. (2) **Compliance: gemini 1/3, gpt 0/3.**
The one adopting run is *textbook* — `clamp(curv·k + noise, 0, 1)` edge wear and
`clamp(-curv·k + noise, 0, 1)` crevice patina coherently driving albedo,
roughness AND metallic — and it is the only gemini run that pulled
`materials-and-media-basics` (the patina example's home). gemini r1 skipped
that skill and did not adopt; gemini r3 read it and still shipped position
proxies. **gpt read every teaching skill in all three runs — including the
worked patina example — and produced zero `curv` and near-zero expression
work**: the C-TYPE prior ("masks are made of positions / materials are
numbers") holding at full strength against delivered advice + examples,
exactly the doc-88 profile that preceded the `vary_material` verb. (3)
**`occlusion`/`thickness`: 0/6, a cross-provider null.** (4) The 0% strict
pass@1 is **not** a regression — the archived s6 census shows the same
instrument at 0% pass@1 (`final_text:3`) on the same gemini model; the T10
gate has never passed here. **Verdict: the descriptor+example levers produce
real but weak, provider-differentiated adoption for `curv` and none for
`occlusion`; by the pre-committed escalation rule this is a census miss, and
the evidence now backs the `add_wear`-class verb (Phase 4) as the next lever.
Building it stays a user decision.**

**ESCALATION TAKEN (2026-08-30).** `add_wear` is BUILT — see §13 Phase 4's
status block for the qualifying predicate, the emitted composition, the
refusal contract and the execution-validated evidence. It is the sixth verb
whose commit is one composite whole-document swap (so it is excluded from
`IsProposeSafeVerb` alongside the other five) and it is wired on every surface
`vary_material` is. The hook points named in the paragraph above were used
exactly as written; the one addition beyond them is the new design-note
condition L (`DESIGN_UNWORN_MATERIALS`), which is the "one predicate, two
consumers" half of the same pattern.

**VERB-EFFECT CENSUS (2026-08-30)** —
`evals/runconfigs/addwear_census_gemini_gpt.json`: the identical instrument and
providers as the pre-verb census, so the delta is attributable to the one
change (the verb + condition L). Results (`evals/runs/addwear_census/`):

| | add_wear called | curv in scene | occlusion in scene | position proxy |
|---|---|---|---|---|
| gemini r1 | 0 | 0 | 0 | 0 |
| gemini r2 | **1** | 4 | 2 | 0 |
| gemini r3 | **1** | 4 | 2 | 0 |
| gpt r1 | 0 (note seen) | 0 | 0 | 0 |
| gpt r2 | 0 (note seen) | 0 | 0 | 0 |
| gpt r3 | **1** (note seen) | 4 | 2 | 0 |

**Deltas vs the pre-verb baseline:** curv-driven wear **1/6 → 3/6** (every
adoption now routes through the verb; the minted painters are genuinely bound
into the material's own slots — `base_color <mat>_wear`, `roughness
<mat>_wearrough`); **occlusion 0/6 → 3/6** (the verb carries the cavity term
the advice never moved); **position proxies 3 occurrences → 0/6**; **gpt
0/3 → 1/3** — its first-ever compliance on this axis, and the causality is
clean: condition L's note lands at trajectory line 3, the `add_wear` call
follows at line 97. The provider that read every skill and complied with
nothing responds to the note+verb pairing — the C-VERB law replicating, though
at 1/3 rather than `vary_material`'s 3/3 (this scenario's note fires as one
clause among several; `constant_materials_polish`'s did too, but on a scene
whose whole task was the polish). Report-level: gpt pass@1 0% → **33.3%**
(first-ever strict-gate pass on this instrument by any provider),
spatially_varying_scalar 0.67 → 1.00; gemini meanCkpt 0.698 → 0.746.
**Verdict: the verb works — every call produced the full composition and
zero calls produced a refusal or a broken scene; adoption of the signals
tripled and the position-proxy anti-pattern went to zero. Remaining gap is
call-rate (3/6), not mechanism.**

---

## 12. Cost and invalidation

**Per-hit costs.** All figures below are **structural counts or
order-of-magnitude estimates, not measurements.**

| Signal / family | Cost | Basis |
|---|---|---|
| Mesh curvature | ~10 dot products + one 2×2 solve; effectively free | Data already on the record |
| Analytic-primitive curvature | Same, once `ri.derivatives` is populated | Closed-form derivatives already computed |
| SDF curvature | **~18 extra `Map()` evaluations** = 3 extra `GradientNormal` calls ≈ 3× the hit's own normal cost; each `Map()` is `O(#parts)` | `jacobianAt`, `SDFGeometry.cpp:1580-1611`; `GradientNormal` is 6 taps, :989-998 |
| SDF cavity | ~5 taps × `O(#parts)` | Evans-style estimator |
| SDF thickness | ~1 tap (field sample) to ~10 (short march) × `O(#parts)` | Depends on estimator choice |
| Mesh AO bake | `V` vertices × `N` rays through the mesh's own BVH. For 100k vertices at 64 rays: **6.4M ray casts** — `O(seconds)` single-threaded; **parallelization unmeasured**. The ray count is exact; the wall clock is an order-of-magnitude estimate, not a measurement | Substance's default is 64 secondary rays |
| Mesh thickness bake | Same order, rays cast inward | Structurally "AO cast inward" |

**Memory.** A per-vertex scalar field is 4 bytes/vertex/signal at `float`
precision, 8 at `double`. A 100k-vertex mesh with both AO and thickness costs
0.8–1.6 MB. Negligible against the mesh itself.

**Invalidation is free** (§7.2): geometry edits produce new geometry objects
whose bake starts empty; transform-only edits do not touch geometry.

**The agent edit loop is the real budget constraint.** Agent production renders
are clamped to `samples ∈ [1,16]` (`AgentChatCodecs.cpp:811`), so a bake that
outlasts the render it precedes turns a fast iteration loop into a slow one.
**A bake must be small relative to a 16-spp render, or be deferred lazily** —
the sharpest argument for consumption-gating M3 and for putting the SDF
(bake-free) family first.

**Which render pays.** Two mechanisms, both needed: consumption gating decides
*whether* a bake ever happens, and the §7.3 lazy build decides *which render*
pays. With both, a mesh bake fires **once per geometry, at first production
shading access** — never during a `quality:"draft"` preview, which executes no
material shading at all. The gate alone would not have sufficed: it would still
have let the first render after authoring a signal-consuming material (very
often a draft preview) pay for a bake it could not display. If the eager
`Realize`-seam alternative is taken instead, that draft cost is real and must
be accepted knowingly.

---

## 13. Phasing

Each phase ships something usable on its own and has an explicit exit gate.
**Warning-free clean rebuild on both `make` and the Xcode `RISE-GUI` target is
a precondition for every phase** — incremental builds hide warnings.

### Phase 1 — `curv` (the cheapest honest win)

1. Shape-operator helper (`src/Library/Utilities/SurfaceCurvature.h`) —
   **pays the five-build-project cost**; oriented off `ri.vGeomNormal`.
2. `curv` + `curvR` context variables: the five-site `ExpressionEval.h` edit,
   plus `BuildContext` population in both `ExpressionPainter` and
   `ExpressionScalarPainter` (`ExpressionPainter.cpp:26-42`, :143-159), with
   `valid`-flag gating and a 0 fallback (the `fw` convention).
3. `scaleHint` on `SurfaceDerivativesInfo`; stamped object-space by geometries
   at intersection time, then folded with the object's world scale
   (`|det M|^(1/3)`) at the `Object::IntersectRay` transform layer (§5.2).
4. `ri.derivatives` population extended to the analytic curved primitives.
5. SDF curvature population — direct `div n̂`, consumption-gated.
6. The `Object.cpp:764-772` renormalization correctness item (§5.5), with an
   SMS regression pass.
7. Descriptor text + `kBuilderGrammarKeywords`.
8. The two skill examples, execution-validated.

**Exit gate:** clean warning-free build on both toolchains; a new
`SurfaceCurvatureTest` pinning the sign convention (unit sphere → `+1/r`), the
flat-primitive zero, the scaled-sphere transform correction, **`curv`
invariance across two instances of one geometry at different world scales**,
**`curv` invariance under an applied bump/normal map** (the `vGeomNormal`
requirement, §5.1), and the degenerate-parameter no-NaN case;
`GeometrySurfaceDerivativesTest` and the SMS suite green; both worked examples
parse + derive + render + land in their luma band; **cross-provider census,
N=3.**

**AMENDED (2026-08-29) — status of the gate.** Items 1–7 are done and
`tests/SurfaceCurvatureTest.cpp` covers every listed case (94 checks, all
green; the sign convention and the world-measure fold were red-proved by
mutation).  `TextureExpressionVMTest` (685), `GeometrySurfaceDerivativesTest`,
`GeometryUVRoundtripTest`, `SDFGeometryTest` (600) and `CsgSurfacePayloadTest`
(251, two assertions upgraded — see §5.4's analytic-primitive amendment) are
green, and `sms_k2_glasssphere` renders clean.  One deviation from the letter
of §5.1: the orientation reference is **not** an explicit `ri.vGeomNormal`
check.  It does not need to be — post the 2026-08-29 sign-pairing fix the
record's `dndu`/`dndv` are already sign-paired with the geometric normal at
every site that negates one, and modifiers mutate only `ri.vNormal`, so H
computed from the stored derivatives is oriented off the geometry and
bump-immune *by construction*.  The requirement that matters (§14 item 6) is
that nothing orients off `ri.vNormal`, and nothing does; the bump-invariance
test pins it empirically rather than by inspection.  Item 8 (the two skill
examples) and the cross-provider census remain.

### Phase 2 — SDF occlusion and thickness

1. `ISurfaceSignalProvider` `const` hook on `RayIntersectionGeometric`,
   populated at intersection time — **the single dispatch channel Phase 3 also
   uses** (§6.1).
2. `const` context-pointer threading into `CallFunc` / `CallFuncVec3`.
3. `occlusion(radius)` and `thickness(radius)` builtins in the `FnSig` table,
   with arity/type checking at parse (`ParseCall`, :905-960), plus the
   constant-radius detection the baked path will need in Phase 3 (the `fbm`
   octave literal-validation precedent, `ExpressionEval.h:917-948`).
4. SDF estimators: Evans-style cavity, inward field sample/march for thickness.
5. Radius defaulting relative to `scaleHint`.
6. Descriptor text; extend the worked example to use `occlusion`.

**Exit gate:** clean build; VM concurrency unchanged (the compiled program stays
`const` and stateless — assert it in review, and keep the existing
multi-thread expression test green); analytic sanity tests (occlusion → 1 on an
isolated convex sphere, < 1 in a modeled crevice; thickness monotone in a slab's
width); no measurable cost on scenes whose expressions do not call them.

**AMENDED (2026-08-29) — status of the gate.** Items 1–4 and 6's descriptor
half are done; `tests/SurfaceSignalsTest.cpp` covers every listed case (116
checks, all green), and `SurfaceCurvatureTest` (94), `TextureExpressionVMTest`
(685) and `SDFGeometryTest` (600) stay green. Item 6's *worked-example*
extension is a later wave, alongside Phase 1's item 8. Five places where what
shipped differs from the sketch above, all deliberate:

1. **Item 5 (`scaleHint` radius defaulting) is satisfied by construction, not
   by reading the record.** `radius` is dimensionless — a fraction of the
   geometry's own bounding-box diagonal — and is applied *inside* the provider,
   in the geometry's own object space. That is the same characteristic-length
   convention `scaleHint` encodes, but it never has to cross the transform
   boundary, so both signals come out transform-invariant with no fold and with
   no dependence on `SurfaceCurvatureDemand` (which is what controls
   `scaleHint` population today). There is no *default* radius: the argument is
   mandatory, so no scene can silently inherit a wrong one.
2. **The provider install is NOT demand-gated.** §6.2 already argued the
   estimators are lazy, and that is where the "costs nothing when unused"
   guarantee comes from; the *stamp* is a pointer plus six scalars with no
   field evaluation at all, on a path that just finished a sphere trace. A gate
   there would be unmeasurable and would add a real failure mode — a closed
   gate silently degrades a live `occlusion()` call to its fallback. Contrast
   `curv`, whose ~18 extra `Map()` calls per hit genuinely need the counter.
3. **The neutral fallbacks are occlusion = 1 and thickness = 1**, both the
   do-nothing end of their range: an absent signal must light *nothing* up.
   Thickness's choice is the non-obvious one and is argued from the use case —
   a thickness mask exists to key *thin* regions (`1 - thickness(r)` drives
   translucency), so a neutral 0 ("thin") would set an unbaked object glowing
   and read as a feature rather than as an absence.
4. **`occlusion` sees creases, not spherical pits — and that is a property of
   the Evans estimator, not a bug.** Inside a sphere, the distance to the wall
   is *exactly* the tap distance, so the field reports no shortfall and a
   hemispherical dimple reads unoccluded. Wedges, corners and folds — where the
   nearest surface is off to the side — are what it darkens, which is what a
   cavity mask is for. The Phase-2 test uses the crease between two overlapping
   spheres for exactly this reason, and says so.
5. **Constant-radius detection is the literal half only** (§7.1's Phase-3
   scope, honoured): every call site is recorded as
   `{fn, radiusIsLiteral, radiusLiteral}` on the compiled program
   (`ExpressionProgram::SurfaceSignalCalls`), with the literal peek widened one
   token past the `fbm`-octave precedent so a leading sign counts. A
   `param`/`def` name bound to a constant reports **not proven**, never a
   guess — which is the safe direction, since Phase 3 must then treat it as
   dynamic and answer with the neutral fallback rather than substitute the
   baked radius.

**Known residual, disclosed not fixed — CORRECTED 2026-08-29 (Phase-2 fix
round; the paragraph below replaces an earlier, narrower draft that
undercounted both the surface and the severity):**

**(a) Surface.** BDPT/VCM/MLT rebuild hit records manually via
[`PathVertexEval.h`](../src/Library/Utilities/PathVertexEval.h)'s
`PopulateRIGFromVertex` (its own contract block, :94-106, names the fields
that must stay in sync), which carries neither `derivatives` nor `signals` —
so `curv`, `occlusion` and `thickness` read their neutral values at **every**
call site downstream of it, not only "connection-time evaluations." That
includes: the forward walk's own **per-bounce throughput re-evaluation**
(`BDPTIntegrator.cpp` ~:2645/2647 on the eye subpath, ~:6140/6142 on the
light subpath — `EvalBSDFAtVertex` called on the vertex the walk just
extended, to price the very sample it just drew); connection/NEE BSDF
evaluations; MIS reverse-pdf recomputation (`EvalPdfAtVertex`); OpenPGL
guiding RIS candidate scoring; the HWSS companion (spectral) evaluations
alongside every RGB one; VCM merge-radius contribution evaluations; and MLT,
which drives BDPT's own machinery and so inherits the gap unchanged. This is
the same gap Phase 1 shipped with (`BDPTVertex` carries no curvature fields
either); closing it means widening `BDPTVertex`'s surface-state block per the
contract's own four-step checklist.

**(b) Failure mode.** The earlier description ("an honest flat mask, not a
wrong one") understated this: because the **forward walk itself** re-evaluates
the BSDF it already sampled from — with the *true* signal at sample time,
then the *neutral* signal moments later when pricing that same sample — a
single material can be evaluated once with the real curvature/occlusion/
thickness and once without, **within one path**. This is a mixed true/neutral
evaluation, not a uniformly-flat one, and it can bias both the reported color
and the sampling-pdf consistency the path's throughput weight depends on —
not merely render as a flat mask where the signal should have varied.

**(c) A second, independent instance.** [`LightSampler.cpp`](../src/Library/Lights/LightSampler.cpp)
builds its own minimal `RayIntersectionGeometric` records by hand for NEE
light samples (~:1949-1956 RGB, ~:2445-2452 spectral) and for photon-emission
sampling (~:1124) — each stamps only `vNormal`/`vGeomNormal`/`ptCoord`/`onb`,
omitting `derivatives` and `signals` the same way `PopulateRIGFromVertex`
does. This reaches **plain PT**, not just BDPT/VCM/MLT: any EMISSIVE
material whose radiance expression keys on `curv`/`occlusion`/`thickness`
reads the true signal when the light is hit directly (a camera or BSDF-sampled
ray intersecting the emitter through the normal shading path) but the neutral
value when the same light is reached via NEE or found by photon emission —
a light-shape-dependent, path-technique-dependent inconsistency independent
of the BDPT/VCM surface above.

**(d) The real fix, deferred.** Both instances share one shape: a hand-built
`RayIntersectionGeometric` that skips the surface-state fields a *live*
intersection populates. The principled fix is therefore two-part — widen
`BDPTVertex`'s surface-state block per `PathVertexEval.h`'s own contract
(:94-106), **and** extend `LightSampler.cpp`'s NEE/photon-emission records
the same way — plus sentinel tests on both (mirroring
`tests/BDPTVertexRIGRebuildTest.cpp`'s existing role for the BDPT side).
This is tracked as a new item in §14 ("Correctness debts and open items"),
explicitly deferred: PT is the shipped default rasterizer family and is
unaffected by (a)/(b), and the LightSampler gap in (c) is a real but narrow
slice (emissive materials that both key radiance on these signals and are
reached via NEE/photon emission) — contained enough to defer as a diagnostic
rather than block on, but no longer safe to describe as "an honest flat
mask."

### Phase 3 — mesh bakes behind the same names

1. Per-vertex AO (self-occlusion only) and thickness, **keyed on geometry,
   never on `IObject*`**, built lazily at first production shading access
   under the sanctioned `mutable` + `RMutex` find-or-build (§7.3).
2. A bake-backed `ISurfaceSignalProvider` installed by mesh geometries —
   **no second dispatch path**; interpolation into the record at intersection
   time (`VertexColorPainter` pattern), read through the provider.
3. Consumption gating.
4. The constant-radius precondition and the mismatch fallback (§7.1);
   documented object-space radius semantics for instanced geometry (§5.2).

**Exit gate:** clean build; bake idempotence and single-build-under-contention
across repeated `AttachScene` and multi-threaded first access;
drop-and-recreate invalidation verified through the incremental derive path;
**a `quality:"draft"` render of a signal-consuming scene provably triggers no
bake**; a non-constant and an out-of-tolerance radius each return the neutral
fallback rather than the baked value; bake wall-time measured against a 16-spp
agent render on a representative mesh and reported as a number; the Phase-1
worked example ported to a mesh object and rendering equivalently.

#### Phase 3 status — SHIPPED 2026-08-29

Clean build (make + Xcode targets updated for the two new files), no warnings.
`MeshSignalBakeTest` 87/87, `SurfaceSignalsTest` 155/155, `SurfaceCurvatureTest`
94/94, `CsgSurfacePayloadTest` 251/251. (The first two counts are post-fix-round;
they shipped at 61 and 148 — see "Phase 3 fix round" below.)

Three departures from the plan above, each recorded where it belongs: the
**stamp + interpolate-on-demand** shape (§7.1, replacing item 2's
interpolate-at-intersection — the lazy decision makes that one impossible on a
first query); the **compiler-carried constant-radius proof** and the bounded
radius map (§7.1); and **explicit invalidation for vertex animation** (§7.2's
second trap — §14 item 10's assumption turned out to be false). Item 3,
"consumption gating," is **subsumed rather than implemented**: laziness *is* the
gate. A table that only exists once a provider is read costs nothing in a scene
whose materials never call the builtins, so a separate consumption predicate
would gate work that has already not happened.

**The two bake algorithms**, object space and self-occlusion only (§8), both at
a fixed **64 rays per vertex** — Substance's own AO-baker default, and a
compile-time constant rather than a scene parameter (§9 admits a sample count
only as an advanced argument, and v1 deliberately ships without one: an author
tuning ray counts is an author handed the renderer's problem):

- **Occlusion** — cosine-weighted directions over the outward hemisphere, max
  distance `radiusFraction × object-space bbox diagonal`, boolean any-hit
  against the mesh's own BVH. Cosine weighting makes the plain hit *fraction*
  the estimator; no per-sample weight is needed.
- **Thickness** — the same budget cast **inward**, nearest self-hit normalized
  by the query radius, a miss counting as "at least this thick" (the saturated
  1, matching the SDF estimator's own convention). The inward cone is **20°**,
  not the full hemisphere, and that number is load-bearing: a hemisphere's
  cosine-weighted mean of `1/cos θ` is exactly **2**, so a slab of width `w`
  would read `2w` against the interface's documented `min(w/R, 1)`. At 20° the
  same mean is 1.031 — a bounded ~3 % overestimate, in exchange for seeing the
  nearest wall in a neighbourhood rather than along one degenerate direction.
  `MeshSignalBakeTest` (f) pins the number (`w/R = 0.5` reads 0.515), which
  catches both the hemisphere error above and the opposite one: lifting the ray
  origin *outward* instead of into the solid, which makes every thickness ray
  re-enter through the face it started on and report ~0 everywhere.

Per-vertex orientations are accumulated from each incident corner's authored
normal (falling back to the face normal where none is authored), not read from
`pNormals` directly — that array is indexed independently of positions and is
empty on a face-normal mesh. A vertex that accumulates to zero is left
unoriented and written the neutral value rather than traced in an invented
direction.

**The draft-mode guarantee, proved rather than asserted.** Draft executes no
material shading, so it evaluates no expression, so it queries no provider, so
no bake exists to fire — an argument that is only worth as much as its weakest
link, which is "no bake fires." `MeshSignalBake::BuildCounter()` makes that
link checkable, and `MeshSignalBakeTest` (a) fires 1,600 intersections at a
signal-publishing mesh and asserts the counter has not moved, while (b) shows
the very next painter-driven `occlusion()` moves it by exactly one and (c) that
eight threads racing the first query still move it by exactly one.

**Bake wall-time, measured** (this machine, single-threaded bake, 256×256 at 16
spp, the ported patina head below):

| mesh | bake | 16-spp render of the same scene, no signal | ratio |
|---|---|---|---|
| 12,028 vertices (`detail 32`) | **78 ms** | 90 ms | **0.87×** |
| 193,886 vertices (`detail 128`) | **1441 ms** | 280 ms | **5.1×** |

So at the mesh densities an agent actually authors the bake costs about one
draft render, and at a quarter-million vertices it costs five. Two residuals
follow from that and are **not** fixed: the bake is single-threaded (the losing
threads block on the find-or-build mutex for its duration — the SSS precedent
has exactly the same shape), and it is per (geometry, signal, radius), so a
scene using both signals on one 200k-vertex mesh pays it twice. Both are
straightforward to improve if a real scene makes them hurt; neither is a
correctness issue. Every bake now announces itself with these numbers at
`eLog_Event`, so the trade is checkable in any render rather than inferred.

**Phase-1 worked example ported to a mesh.** The shipped patina example from
`skills/agent/materials-and-media-basics.md` (curv-driven crevice mask ×
`cavity_boost = 1 + occl_gain·(1-occlusion(0.08))` × fbm breakup) rendered with
its SDF head replaced by `displaced_geometry { base_geometry head_sdf,
disp_scale 0 }` — the same body, as a 194k-vertex triangle mesh, so every signal
is answered by the Phase-3 bake instead of the SDF's live field. Mask values
read out directly (emissive readout, 17×17 patch means, 64 spp):

| patch | `1-occlusion(0.08)` on the MESH | same on the SDF | patina mask, `occl_gain 1.5` | patina mask, `occl_gain 0` |
|---|---|---|---|---|
| knot/skull seam (a real fold) | **0.370** | 0.508 | **0.257** | 0.023 |
| scar dimple (smooth pit) | 0.062 | 0.000 | 0.431 | 0.393 |
| clean convex body | **0.000** | 0.000 | 0.000 | 0.000 |

The mask still keys on geometry: the occlusion term deepens the fold **11.2×**
and the smooth dimple only 1.10× (curv already saturates there), while the
convex body stays at exactly 0 in every variant. Two honest differences from
the SDF twin, neither a defect: the mesh reads the fold at **73 %** of the SDF's
magnitude (a ray-traced hemisphere and the Evans field estimator are different
estimators, and the tessellated seam is slightly rounded), and the mesh reads
**0.062** in the smooth scar dimple where the SDF reads exactly 0 — the bake
sees that pit's real self-occlusion within the query radius, which the Evans
identity `map(p + h·n) = h` cannot (Phase 2 documented that blind spot; here the
mesh is arguably the more correct of the two). Mesh `curv` was **not** the noisy
term this port was braced for: it matched the SDF at every patch (scar 0.998 vs
0.998, body 0.000 vs 0.000).

**One trap worth carrying forward** (it cost a debugging cycle and would cost
another): the bakes trace through the mesh class's own `IntersectRay` /
`IntersectRay_IntersectionOnly`, **never `pPtrBVH` directly**, because the
per-ray mailbox id (`RISE_ENABLE_MAILBOXING`, on by default everywhere) is
bumped in the former. Reaching past them makes every bake ray after the first
find all triangles already stamped and skip them — a total loss of hits that
presents as "this mesh is unoccluded and infinitely thick everywhere," i.e.
indistinguishable from a correctly-neutral answer.

**CSG.** `AdoptCsgSurfacePayload` copies `signals` as a whole struct, so the new
`primId` / barycentric fields ride along with the provider pointer and a
composite reports exactly what the bare mesh reports (pinned by
`MeshSignalBakeTest` (k)). The Phase-2 subtraction fix negates
`signals.nObject`, which the mesh provider does not read — its lookup is
positional in the triangle, not directional — so a subtraction-exposed interior
answers with the **outward** surface's bake. That is a real limit and a
different one from the SDF's: a bake describes the mesh's own surface, and a CSG
cut exposes an interior no bake ever measured.

#### Phase 3 fix round — 2026-08-29

Three independent reviews of the shipped work. What they found, and what was
done about it.

**The cache handed out raw `Table*` pointers, and one path could free one under
a reader.** `FindOrBuild` returned a bare pointer, `LookupBakedSignal`
interpolated from it *after* releasing the lock, and `Invalidate()` `delete`d
it. That is safe exactly as far as the "invalidation never runs concurrent with
rendering" contract is — and the contract has a documented hole. Motion-blur
temporal sampling calls `IAnimator::EvaluateAtTime` **from worker threads**
([ARCHITECTURE.md](ARCHITECTURE.md):68-74), so a keyframed displacement painter
drives `DisplacedGeometry::RefreshMeshVertices` → `UpdateVertices` →
`InvalidateSignalBakes` on a worker thread while other workers may be mid-read.
Tables are now `std::shared_ptr<const Table>`; `FindOrBuild` copies one out
under the lock and `Invalidate()` drops only the cache's own reference, so a
reader that loses the race finishes on a table that is **stale rather than
freed**. Steady-state cost is one refcount pair per provider query — the copy is
taken once and held across the whole interpolation, never per tap.

**Said plainly, because the fix invites the wrong conclusion:** this removes the
use-after-free *Phase 3 introduced*. It does **not** fix the race that made it
reachable. Mutating vertex positions and refitting the BVH from a worker thread,
under traversal, is **pre-existing**, is what ARCHITECTURE.md documents, and
**remains** — a reader on that path can still see a half-refit BVH. What is now
true is only that it will not read freed memory. `UpdateVertices` and
`InvalidateSignalBakes` carry the DEBUG freeze assert
`DisplacedGeometry::Realize()` uses, so the violation announces itself in a
debug build instead of being inferred from a crash. The invalidation one fires
only when something live was actually dropped, because every mesh *construction*
path invalidates an empty cache on a geometry no render can reach.

**`InvalidateSignalBakes` had an unlocked tail.** It called the self-locking
`MeshSignalBakeCache::Invalidate()` and then cleared a *geometry-owned*
per-position normal array with no lock at all — racing the `MakeBakeInput` that
reads it from inside the cache's lock. The normals now live **in the cache**, so
one acquisition drops the tables and the state they were derived from together,
and the mesh's `BuildVertexNormals()` is a pure function into the cache's array
with no second copy for an invalidation to chase.

**Three author-facing texts still promised mesh support in the future tense** —
both `expression`-parameter descriptors in `ChunkParserRegistry.cpp` and the
thickness passage in `skills/agent/materials-and-media-basics.md` all said the
builtins were SDF-only with "Mesh support arrives in Phase 3". An agent reading
any of them would decline to use the builtins on the mesh in front of it. All
three now describe the shipped split: SDF live-evaluated with a dynamic radius,
indexed meshes lazily baked and therefore literal-radius-only, everything else
(analytic primitives, non-indexed meshes, heightfield-mode SDFs) neutral.

**The exit gate above claimed more than the suite tested.** "Verified through
the incremental derive path" was not true: the invalidation coverage constructed
geometries by hand, which proves the cache is a member subobject and says nothing
about whether the derive path *recreates* rather than mutating in place — the
thing §7.2's free-invalidation argument rests on entirely. `MeshSignalBakeTest`
(n) now drives a real `lathe_geometry` edit through `ParseToCst` → `DeriveToJob`
→ `DocSetParamValue` → `DocEditClosure` → `DeriveToJobIncremental` and watches
the build counter move again on the next query. It does recreate; the gate's
claim is now earned rather than asserted.

Four smaller test gaps closed at the same time: mesh **instancing** ((l): two
objects at different scales over one geometry read identically, and the second
builds *nothing*); **concurrent distinct radii** ((m): eight threads on eight
radii build exactly eight tables, no duplicate and no lost insert — (c) only
ever raced them onto *one*); the **thickness convention** ((f) read the slab at
`w/R = 0.5`, where an inverted convention answers 0.485 against the correct
0.515, inside any tolerance loose enough to admit the cone's own 3 % — moved to
`w/R = 0.25`, where the candidates are 0.258 and 0.742); and, on the SDF side,
`SurfaceSignalsTest` (l), a **computed radius that lands positive** being
genuinely answered and agreeing with the same radius spelled as a literal. Phase
2 only ever covered computed-and-*non*-positive, so a regression making the SDF
adopt the mesh's refusal rule would have left the suite green while flattening
every expression-driven cavity mask to 1.

### Phase 4 — observed-need gated (may be declined)

Per the project's observed-need rule (the precedent is
[HAIR_FUR_DESIGN.md](HAIR_FUR_DESIGN.md) Phase 4, where four of six menu items
were **declined** for lack of a driving scene): take these up **only** when a
real scene or user need appears.

- **Patch derivative stubs fixed** (`BezierPatchGeometry.cpp:402-414`,
  `BilinearPatchGeometry.cpp:397-409`) — closed-form second partials exist and
  are unwired; this is the one item that removes an outright falsehood from the
  signal.
- **Sampled (radius-parameterized) curvature** — the Arnold shape, tunable to a
  feature size. The real answer to §5.3's first limit.
- **Gaussian curvature `curvK`** and an Arnold-style convex/concave split
  output mode.
- **Bent normals** — the natural companion output of an AO bake.
- **Scene-wide (cross-object) AO** — §8.
- **An `add_wear`-class verb** — the C-VERB escalation if the Phase-1/2 census
  misses. **BUILT 2026-08-30**, census-driven: the §11 CENSUS RUN block is the
  evidence that earned it (delivery 6/6, adoption 1/6, position proxies shipped
  in its place), and the pre-committed escalation rule for a census miss is a
  verb. Status block below.

#### `add_wear` — BUILT 2026-08-30

A zero-required-argument mutating verb in the exact `vary_material` mould: one
qualifying predicate feeding **both** a new design-note clause (**condition L**,
`DESIGN_UNWORN_MATERIALS`) and the callable verb, refuse-on-no-op with a
byte-identical document, one composite whole-document swap, one head bump, one
undo step, no staged-proposal form under External authority.

**Qualifies** (`WearMaterial_`, `AgentSession.cpp`) — a material whose

1. kind carries colour-pipe slots, spells at least one out, and where **every**
   spelled-out colour slot classifies `Constant` (condition H's flat test); and
2. **primary** colour slot (preference list `base_color` / `reflectance` / `rd` /
   `albedo` / `diffuse` / `color`, falling back to descriptor order) binds a
   `uniformcolor_painter` whose RGB is readable in the default Rec.709-linear
   space — a blackbody, a spectral painter, or a non-default `colorspace` is a
   base it must not invent; and
3. is bound by at least one `standard_object` (following `source` links, bounded
   at 8 hops) whose geometry chunk is **not** curv-barren — the barren set is
   stated as a negative (`CurvBarrenGeometryKind_`) so a future geometry kind
   defaults to "assume it curves": `bezierpatch` / `bilinearpatch` (the §13
   stubs, 0 as *absence*), `infiniteplane` / `clippedplane` / `circulardisk` /
   `box` / `cartesian_disk` (genuinely planar, 0 is the true answer); and
4. does not already bind an expression reading `curv` / `curvR` / `occlusion` /
   `thickness` — a second pass would stack two wear layers, and this clause is
   what makes the note **self-disarm** as work lands.

**What it writes.** One `expression_painter` (colour), plus — only when the
material also carries a readable constant microsurface — one
`scalar_painter` / `expression_painter` (roughness). Both carry the *identical*
mask prelude, so colour and roughness cannot disagree about where the wear is:

```
def wear_mask    clamp(curv*edge_wear + breakup_amp*fbm(P*breakup_scale + jitter, 4, 0.5, 2.0), 0, 1)
def crevice_raw  clamp(-curv*crevice_grime + breakup_amp*fbm(P*grime_scale + jitter, 4, 0.5, 2.0), 0, 1)
def cavity_boost 1.0 + cavity_gain*(1.0 - occlusion(0.08))
def crevice_mask clamp(crevice_raw*cavity_boost, 0, 1)
expr             mix(mix(base, edge_tint, wear_mask), patina_tint, crevice_mask)
```

`edge_tint` / `patina_tint` are **derived from `base` inside the expression**
(desaturate + lift toward white; desaturate + darken), never a hardcoded
verdigris — so the verb suits wood, stone and painted steel as much as bronze.
`base_r/g/b` are params carrying the material's own authored colour; the
roughness band is `VaryBandFor_`, the literal function `vary_material` uses.

**One deliberate exception to the doc-88 "every art-directable number is a
param" rule**: `occlusion(0.08)`'s radius stays a numeric **literal**.
`ExpressionEval.h`'s compiler emits the `kFnOcclusionDynR` twin for any radius
it cannot prove is exactly one numeric literal, and that twin reads the neutral
fallback on every indexed triangle mesh — a `param`-bound radius would silently
delete the cavity term across the whole mesh family while still *looking*
art-directable. The **gain** is a param; the radius is a mechanism constant.

**Refusals**, each byte-identical: nothing qualifies; a named material that is
not a candidate (message distinguishes "no such chunk" / "no colour slot this
can read" / the specific clause it tripped); planar-only geometry; already worn;
already varying; an unreadable base; a name collision; an underivable candidate.

**Verified** by `tests/AgentAddWearTest.cpp` (151 checks, execution-validated,
not a render-pixel check): three synthetic hits differing **only** in mean
curvature — same world point, so the `fbm` breakup contributes identically —
give colour luma convex `0.5098` > flat `0.3053` > concave `0.1049` and
roughness convex `0.1750` < flat `0.2480` < concave `0.3500`, through
`IPainter::GetColor` / `IScalarPainter::GetValuesAt` on the live managers; plus
parse + derive + render + zero-error `validate` on the mutated document.

**Future census plan** (not run): re-run `rich_material_closeup` (the doc-88 S6
instrument, a *weathered* brass doorknob) × 3 repeats × {gemini, gpt} and count
(a) `add_wear` calls per run and (b) the resulting `curv` reference count in the
committed scenes — the same post-hoc reading §11's CENSUS RUN block uses. The
`vary_material` precedent (census-confirmed 3/3 once it existed as a callable)
is the comparison to beat.

---

## 14. Correctness debts and open items

1. **Non-uniform / scaled object transforms corrupt world-space `dndu/dndv`**
   (§5.5). **RESOLVED 2026-08-29** — fixed and merged to master with the
   sign-pairing family (see §5.5); one disclosed residual remains (a CSG
   exit-designated subtraction branch's `dndu` pairing, reachable only via a
   nested-CSG construction no test could produce).
2. **Patch geometries report flat while genuinely curved.** Until Phase 4, keep
   them on the `valid = false` path so `curv` reads 0 as *absence*, not as a
   claim of flatness. An expression cannot distinguish the two without a
   companion validity variable — decide whether one is warranted or whether
   `fw`'s honest-zero convention suffices.
3. **Displaced geometry: which curvature?** The baked path reports **only** the
   displaced surface, at a fidelity bounded by tessellation. The analytic
   escape hatch (`ComputeAnalyticalDerivatives`, `DisplacedGeometry.cpp:407+`)
   can produce base (`smoothing = 1`) or displaced (`smoothing = 0`) curvature
   tessellation-independently — but **only for analytic bases**, and only
   `EllipsoidGeometry` and `DisplacedGeometry` override that method today.
   Base and displaced curvature genuinely differ, and lookdev often wants the
   *base* (wear follows the form, not the bumps). Unresolved; note it in the
   descriptor rather than pretending there is one answer.
4. **Vertex-normal quality is loader-dependent** (`GLTFSceneImporter.cpp:2663-2724`
   carries authored normals; PLY may synthesize them). Mesh curvature quality
   follows. No fix proposed; document it.
5. **CSG behaviour is unspecified.** Curvature at a CSG boundary edge is
   genuinely undefined, and the record's derivative fields are already
   exercised there (`tests/CsgSurfacePayloadTest.cpp:833-834`). Decide whether
   the composite forwards the contributing surface's curvature (likely) or
   invalidates at the seam.
6. **Modifier and bump-map interaction — an implementation requirement, not a
   preference.** Modifiers assign into `ri.vNormal` **in place, before material
   shading** (`BumpMap.cpp:65`, `NormalMap.cpp:169`), so by `BuildContext` time
   the shading normal is already perturbed. **The curvature computation must
   therefore orient off `ri.vGeomNormal`, not `ri.vNormal`** (§5.1), copying
   `GlintModifier.cpp:224-225`'s degenerate-`vGeomNormal` fallback. `curv` and
   `N` in the same expression will then legitimately disagree on a bump-mapped
   surface — that is correct and must be stated in the descriptor: a wear mask
   wants the form, not the texture, and a bump-perturbed curvature would
   double-count detail the bump map already shades. Pinned by a Phase-1 test
   (`curv` invariant under an applied normal map).
7. **`expression_function2d` stays frozen.** It is a UV-only contract by design
   (`ExpressionEval.h:403-414`, `ExpressionPainter.h:68-86`) and must **not**
   gain 3D context variables. Only `expression_painter` and
   `scalar_painter { expression }` opt into `EnableContextVars(true)`.
8. **`ScalarToPainterAdapter` does not exist** (§10). A convenience gap, not a
   blocker; note it, do not build it speculatively.
9. **Attach-order trap.** `RayCaster::AttachScene` calls `Prepare()` **before**
   `SetEnvironmentSampler()` — the env-IBL lesson. Any cache derived from
   late-set state must recompute in its setter. The signals proposed here are
   geometry-derived and should not depend on late-set state; verify that
   assumption rather than assume it.
10. **Deforming geometry would stale a bake — RESOLVED 2026-08-29, and the
   assumption below was WRONG.** `TriangleMeshGeometryIndexed::UpdateVertices`
   (the keyframed-painter `DisplacedGeometry::RefreshMeshVertices` path)
   replaces the vertex and normal arrays **in place** on a live `IGeometry*`
   and refits the BVH. So vertex-level animation does exist, transform-only
   timelines are *not* the whole story, and free invalidation does not cover
   it. Phase 3 drops the bakes explicitly there and at every other vertex
   mutation site (§7.2's second trap). The grep below missed it because it
   looked for `EvaluateAtTime`; the mutation arrives through an observer
   callback instead. Original text follows.
   **Deforming geometry would stale a bake — confirm whether it can happen.**
   M3's free-invalidation argument (§7.2) rests on vertex data being immutable
   for the lifetime of an `IGeometry*`. That fails under **vertex-level or
   skinned animation**, where one geometry changes shape over time and would
   carry a bake computed for one frame into every other. A grep of
   `src/Library/Geometry/` finds **no `EvaluateAtTime` implementation**,
   suggesting RISE animates transforms and objects rather than vertices —
   transform-only timelines are safe by construction. **Confirm before relying
   on it.** If vertex animation exists or is added, M3 needs a time key or
   explicit per-frame invalidation; M1 and M2 are unaffected (both evaluate per
   hit from live state).
11. **BDPT/VCM/MLT + LightSampler neutral-signal gap — deferred, not fixed**
   (§13 Phase-2 "Known residual," corrected 2026-08-29). Two independent hand-
   built-record sites read `curv`/`occlusion`/`thickness` as neutral instead
   of live: (a) every `PathVertexEval.h::PopulateRIGFromVertex` consumer
   (BDPT/VCM's forward-walk throughput re-evaluation, connection/NEE, MIS
   reverse-pdf, OpenPGL guiding RIS, the HWSS companion evals, VCM merges, and
   MLT which drives BDPT's machinery) and (b) `LightSampler.cpp`'s manually-
   built NEE light-sample records (~:1949-1956, :2445-2452) and photon-
   emission record (~:1124), which reaches plain PT for emissive materials.
   The failure mode is a **mixed true/neutral evaluation of one material
   within one walk** (the forward walk samples with the true signal, then
   re-prices that same sample with the neutral one moments later) — a bias
   on color and sampling-pdf consistency, not merely a flat mask. The
   principled fix is two-part: widen `BDPTVertex`'s surface-state block per
   `PathVertexEval.h`'s own contract (:94-106), and extend `LightSampler.cpp`'s
   NEE/photon-emission records the same way, each with a sentinel test
   (mirroring `tests/BDPTVertexRIGRebuildTest.cpp`). Deferred rather than
   fixed because PT is the shipped default and unaffected, and a cheap
   containment diagnostic (a one-time GlobalLog warning when a BDPT/VCM/MLT
   render begins with a signal-consuming expression live) covers the gap in
   the interim — see the descriptor text in `ChunkParserRegistry.cpp` for the
   curv/occlusion/thickness builtins, which now states this limitation
   directly.

---

## 15. Non-goals

- **Not a new integrator strategy.** These are auxiliary queries, decoupled
  from NEE/MIS (§9). Nothing here touches a PDF.
- **Not a physically-based occlusion model** — an artist masking control,
  decoupled from the light integral exactly as Blender's AO node is.
- **Not screen-space.** §3(c).
- **Not a `PainterContext` refactor.** M2 adds one typed `const` channel where
  the `pCustom` comment has asked for a full refactor for years; that full
  refactor remains desirable and out of scope.
- **Not hair/fur.** Strand geometry has its own tangent-frame contract
  ([HAIR_FUR_DESIGN.md](HAIR_FUR_DESIGN.md) §4.1); fibre curvature is not the
  wear signal.
- **Not a new material.** Every proposed signal composes into existing
  materials through existing painter slots.

---

## 16. References

**RISE.** [GEOMETRY_DERIVATIVES.md](GEOMETRY_DERIVATIVES.md) (the contract this
must conform to), [ARCHITECTURE.md](ARCHITECTURE.md) (threading phases,
`Realize` seam, sanctioned lazy exceptions),
[ISCALARPAINTER_REFACTOR.md](ISCALARPAINTER_REFACTOR.md) (scalar-vs-colour pipe
discipline), [MIS_HEURISTICS.md](MIS_HEURISTICS.md) (why auxiliary queries stay
out of the weighting), [HAIR_FUR_DESIGN.md](HAIR_FUR_DESIGN.md) (phased-plan and
observed-need-gating precedent),
[UNIFIED_INTEGRATOR_DECISION.md](UNIFIED_INTEGRATOR_DECISION.md) (decision-doc
form), `src/Library/Parsers/README.md` (descriptor-driven chunk parsers),
`docs/agentic-redesign/20-derivation-engine.md` (incremental derive,
drop-and-recreate, traced edges).

**External prior art.** Only the facts this design actually argues from are
listed; per-tool numeric defaults were surveyed but are not load-bearing here
and are deliberately omitted.

- **Arnold `aiCurvature`** (Autodesk help) — output Convex / Concave / Both;
  radius-sampled, `self_only` available. Source of the convex/concave split and
  of the radius-sampled shape deferred to Phase 4.
- **Blender** — AO node (**Only Local**, `Inside` toggle; *white = unoccluded*);
  Pointiness (Geometry node; convex > 0.5, concave < 0.5, flat 0.5, with the
  documented quality-coupled-to-vertex-density limitation, commit
  `227a94077f`); Bevel node. Source of the self-occlusion-is-first-class
  argument (§8) and of the mesh faceting caveat (§5.3).
- **Substance 3D bakers** — Curvature (*black = concave, 0.5 = flat, white =
  convex*), AO from Mesh (*white = unoccluded*), Thickness from Mesh (rays cast
  inward; *black = thin, white = thick*). Source of the §9 range conventions
  and of the "mask generators consume baked curvature + AO + thickness
  directly" pattern this work targets.
- **RenderMan `PxrCurvature`** — mirrors Arnold's parameter shape;
  **defaults UNVERIFIED** (secondary source, primary wiki unfetchable), so
  nothing here rests on them.
- **V-Ray `VRayDirt`** — radius guidance ~1–5% of object size; `invert_normal`
  makes cavity a sign flip of one ray-cast primitive; "Consider same object
  only". **Corona AO** — same shade-time shape.
- **SDF techniques** — exact mean curvature
  `Kₘ = (∇f·H(f)·∇fᵀ − ‖∇f‖²·tr H) / (2‖∇f‖³)`; cheap approximation `∇²f`
  (Laplacian stencil); equivalent `div(∇f/‖∇f‖)` — the form RISE already uses
  (rodolphe-vaillant.fr/entry/118). Evans-style SDF AO: ~5 taps at increasing
  distances along the normal. iq tetrahedral 4-tap normals (cited by name;
  coefficients unverified).
- **arXiv 2409.00856** — linear text vs graph-as-data for LLM authorship,
  76% vs 53%; the C-TEXT basis for §10.
