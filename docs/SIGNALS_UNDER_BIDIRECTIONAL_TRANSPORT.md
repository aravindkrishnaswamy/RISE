# Geometry Shading Signals Under Bidirectional Transport — closing item 11

**Status:** DESIGN ACCEPTED 2026-09-11; implementation in slices S1–S4 on
branch `signals-bidir` (see §9 for the per-slice ledger, filled as each
slice converges to zero P1).
**Closes:** [GEOMETRY_SHADING_SIGNALS_DESIGN.md](GEOMETRY_SHADING_SIGNALS_DESIGN.md)
§14 item 11 and the matching residual in
[CROSS_OBJECT_PROXIMITY_DESIGN.md](CROSS_OBJECT_PROXIMITY_DESIGN.md) §5.1 / §10.
**Inputs:** a source-grounded site audit (§2) of every transport site that
evaluates a painter against a `RayIntersectionGeometric` the object manager
did not stamp — `PathVertexEval.h`, `BDPTIntegrator.cpp`,
`VCMIntegrator.cpp`, `PathValueOps.h`, `LightSampler.cpp`,
`MediumTransport.cpp`, the photon tracers, the emitters — plus the memo
([ExpressionMemo.h](../src/Library/Utilities/ExpressionMemo.h)) and the
context builder
([ExpressionPainter.cpp](../src/Library/Painters/ExpressionPainter.cpp)
`BuildContext`).

---

## 1. The gap, stated precisely

The six geometry signals — `curv` / `curvR` (from `ri.derivatives`),
`occlusion(r)` / `thickness(r)` / `convexity(r)` (from `ri.signals`'
own-surface half) and `proximity(r)` / `interior(r)` (from `ri.signals`'
cross-object half) — are read by `ExpressionPainter::BuildContext` off the
hit record. The path tracer evaluates every painter against the record the
object manager stamped, so it sees them live. BDPT, VCM and MLT store a
`BDPTVertex` per subpath vertex and later **rebuild** a record from it through
`PathVertexEval::PopulateRIGFromVertex`; that rebuild carried neither
`derivatives` nor `signals` (nor `txFootprint`), so every painter evaluated
downstream of it read the documented neutral values. Two independent
`LightSampler.cpp` sites build emitter records by hand the same way, which
reaches plain PT for emissive materials.

Why this is a bias and not "an honest flat mask": the forward walk **samples**
the continuation direction with the live record (the SPF's `Scatter` sees
`ri.geometric` straight from the intersection) and then **prices** that same
sample through the rebuilt record (`EvalBSDFAtVertex( vertices.back(), … )`
at `BDPTIntegrator.cpp` ~:2663 / ~:6175). One material, one vertex, one
path: sampled with the true roughness, weighted with the neutral one. The
MIS reverse pdfs (`EvalPdfAtVertex`) go through the same rebuild, so the
strategy weights are computed for a material that is not the one the samples
were drawn from. `GGXSPF::Pdf` reads its roughness painter at `ri`
(`pAlphaX->GetValuesAt(ri)`, GGXSPF.cpp:721), so **no pdf query is
legitimately neutral** when a signal drives roughness — class (b) in the
charter's taxonomy is empty for this tree.

Since `auto_rasterizer` routes glossy-indirect scenes to BDPT and caustic
scenes to VCM, an agent-authored scene whose wear/grime/contact masks depend
on signals silently loses them — and loses them *inconsistently* within a
path — when the router picks a bidirectional integrator.

## 2. Site audit

Every site where a painter, BSDF, SPF pdf or emitter is evaluated against a
record the object manager did not stamp. Classification: **(a)** must be
live (a real surface vertex, forward or reverse); **(b)** legitimately
neutral (none found — see §1); **(c)** structurally cannot carry a signal
(no surface hit exists).

| # | Site | What is evaluated | Class | Disposition |
|---|------|-------------------|-------|-------------|
| 1 | `PathVertexEval::PopulateRIGFromVertex` — 48 call sites in `BDPTIntegrator.cpp`, 17 in `VCMIntegrator.cpp`, 11 in `PathValueOps.h` (MLT drives BDPT's machinery and inherits all of them) | BSDF value (`EvalBSDFAtVertex{,NM}`), SPF pdf (`EvalPdfAtVertex{,NM}`) for: forward-walk throughput re-pricing (eye ~:2663, light ~:6175), OpenPGL guiding RIS candidate scoring (~:2462–2547, ~:6000–6085), HWSS companion evals (~:2681, ~:6201, ~:6564), every connection strategy (s,t), NEE at eye vertices, MIS reverse pdfs (~:2758, ~:6270, ~:3915–4447), VCM connections (~:1910–1938) and VCM merges at the EYE vertex (~:2260–2267), emitter radiance at eye-end vertices (`LuminaryRadiance` ~:2939, `EvalEmitterRadiance` ~:2983/3016), BSSRDF profile Fresnel/IOR at entry vertices | (a) forward AND reverse | **S1**: widen `BDPTVertex` (§3) |
| 2 | `BDPTIntegrator.cpp` eye-subpath vertex population (~:2109) and light-subpath population (~:5625) | the copy from `ri.geometric` into the vertex | (a) | **S1**: copy `derivatives`, `signals`, `txFootprint` |
| 3 | BSSRDF entry vertices (~:2280, ~:2390, ~:5807, ~:5919) built from `BSSRDFSampling::SampleResult` (entry point / normals / ONB only) | `FresnelTransmission(cos, rig)` / `GetIOR(rig)` on the diffusion profile | (a)-narrow, **integrator-consistent**: PT builds its BSSRDF entry record by hand too (`BSSRDFSampling.h` comment "PT 1508/etc.") | residual, disclosed in §10 — an IOR painter keyed on a signal at a BSSRDF entry reads neutral under every integrator alike; not a PT-vs-BDPT disagreement |
| 4 | VCM `LightVertex` store (`VCMLightVertex.h`) | nothing — merges evaluate the BSDF at the **eye** `BDPTVertex` only (`EvalBSDFAtVertex<Tag>( v, wiAtEye, … )`, ~:2260); the stored light vertex contributes `wi`, throughput and MIS quantities | n/a | no widening of the store; stated in the store's header by S1 |
| 5 | `LightSampler.cpp` NEE emitter records (~:1949 RGB, ~:2451 NM): `vNormal/vGeomNormal/ptCoord/onb` only, from `UniformRandomPoint` | `IEmitter::emittedRadiance{,NM}` — the emission painter | (a) for an emissive material whose radiance keys on a signal; reaches **PT** | **S3** (§5) |
| 6 | `LightSampler::SampleLight` emission record (~:1124) → consumed as `ls.Le` by BDPT's light-subpath root (~:5232; the NM twin at ~:5239 rebuilds the same minimal record) and VCM's (~:1299) | light-subpath root `Le` | (a), same material class as #5 | **S3** |
| 7 | `MediumTransport.cpp` scatter records (~:128, ~:165): a medium point, `vNormal = wo` | phase-function NEE through `EvaluateDirectLighting` | (c) no surface | none |
| 8 | Photon-map emission (`LuminaryManager.cpp` samples `UniformRandomPoint`; photon power comes from `averageRadiantExitance`, a 10×10 UV-grid average computed at emitter construction against an empty `Ray()` record — `LambertianEmitter::RefreshAverages`, same in Phong/Composite) | photon power budget / light-importance weights, **not** a per-point radiance | (c) a design-level average; no hit exists | none; the average is deliberately record-free (its comment says so). Photon **hits** during tracing are evaluated on live intersection records |
| 9 | `ManifoldSolver.cpp` / `SMSPhotonMap.cpp` `SampleLight` consumers | seed positions | (c) | none |
| 10 | `PainterPreview.cpp`, `HairGenerator.cpp`, `GeometryUtilities.cpp`, `TriangleMeshGeometryIndexed.cpp` hand-built records | GUI preview / realize-time evaluation | out of scope (not transport); already disclosed in CROSS_OBJECT_PROXIMITY_DESIGN.md §10 | unchanged |

Findings that changed the plan:

- **One helper feeds forward and reverse.** Every BDPT/VCM/MLT BSDF and
  pdf evaluation at a stored vertex funnels through `PopulateRIGFromVertex`.
  Widening the vertex and the helper therefore fixes the forward re-pricing,
  the reverse pdfs, guiding, HWSS, connections, merges and emitter evals in
  one change — the charter's "never leave forward-live / reverse-neutral on
  master" requirement is met by construction, not by ordering.
- **The auto-router has no "signals → force PT" rule.** `AutoRasterizer.cpp`
  keys on two scene signals (transmissive material presence, positional
  lights); it never inspects painters. There is nothing to remove there; the
  containment is the one-time `GlobalLog` warning plus descriptor/README text.
- **The VCM light-vertex store does not need widening** (row 4).
- **BDPT/VCM/MLT eye rays carry no ray differentials** (no `hasDifferentials`
  anywhere in the BDPT/VCM/MLT rasterizers), so `fw`/`fwo` read 0 at every
  bidirectional vertex today, live or rebuilt. That is a PT-vs-BDPT
  difference on `fbm` fade bands that is **not** attributable to signals.
  §6's control variant is built so it cancels (§6.1); if the showcase
  invariant surfaces it as a residual anyway, it is recorded in
  RENDERING_INTEGRATORS.md per charter item 6, not absorbed.

## 3. The fix for rows 1–2: widen `BDPTVertex`

Per `PathVertexEval.h`'s own four-step contract, in one slice for **all** of
the fields `BuildContext` reads and the vertex does not carry:

1. **`BDPTVertex` gains** `SurfaceDerivativesInfo derivatives;`
   `SurfaceSignalInfo signals;` `TextureFootprint txFootprint;` in the
   surface-state block. Size: +168 B (derivatives) +136 B (signals) +104 B
   (footprint) ≈ +408 B per vertex; subpaths are short per-thread vectors,
   so this is noise. The VCM `LightVertex` store is NOT widened (row 4).
   `txFootprint` is carried although it is all-zero under today's
   bidirectional rasterizers: the contract is "every field a painter
   consumer reads", and a future BDPT ray-differential landing must not
   silently reopen the gap. The sentinel test pins all three.
2. **Population** at both subpath generators (rows 2): straight copies from
   `ri.geometric`, after `ri.pModifier->Modify( ri.geometric )` exactly as
   the existing fields are. Medium, camera, light and env vertices keep the
   defaults (an honest "no surface"). BSSRDF entry vertices keep the
   defaults too (row 3 — disclosed).
3. **`PopulateRIGFromVertex`** copies the three. The "DECLINED,
   DELIBERATELY" block is replaced by a paragraph stating the fields are
   carried and why the copy is safe under the hygiene invariant (§3.1).
4. **`tests/BDPTVertexRIGRebuildTest.cpp`** gains sentinel assertions for
   every scalar/pointer/flag of the three structs (a swapped or dropped
   field must fail by name).

### 3.1 The `signals` write-site invariant

`SourceHygieneTest` pins the set of files that assign `signals` (seven
today). The invariant it protects is "nothing assigns a **default-constructed**
channel over `ObjectManager::IntersectRay`'s stamp". The two new writers copy
the stamp **forward** — `v.signals = ri.geometric.signals` in
`BDPTIntegrator.cpp` (source: the stamped record) and
`ri.signals = vertex.signals` in `PathVertexEval.h` (source: that copy) — so
the value that reaches the painter is the object manager's stamp, one hop
later. The allowed set becomes nine, each new entry with that rationale in
the test's comment table, and CROSS_OBJECT_PROXIMITY_DESIGN.md §5.1's
enumeration is updated in the same commit.

### 3.2 Memo discipline

`ExpressionMemo::ProgramKey` is `{progId, pipe, u, v, P, Po, N, fw, fwo,
time, curv, curvR, SignalHitKey}` and `SignalKey` is `{SignalHitKey, radius,
fn, bRadiusIsConstant}`. A rebuilt record reproduces every one of those bits
from the vertex (`position`, `normal`, `ptCoord`, `ptObjIntersec`, the copied
`derivatives` → the same `curv`/`curvR` through `PopulateCurvature`, the
copied `signals`, `txFootprint` → the same `fw`/`fwo`), and the painter's
`m_time` is the painter's own. So the many evaluations BDPT/VCM make at one
vertex — forward re-pricing, up to `max_light_depth` connections, the
reverse pdfs, HWSS companions — are **memo hits** after the first, which is
the win the charter anticipated. Nothing in the memo changes in S1.

The one open cost question is associativity: `kL2Ways == 4` per thread, and
the (s,t) connection loops interleave evaluations at several vertices (eye
vertex t against each light vertex s), each material typically holding two
memo-worthy programs (colour + roughness). A round-robin 4-way table can
thrash under that interleave. **This is measured, not guessed** (§7); if the
cost gate shows an L2 miss rate materially above PT's, the remedy is a
`kL2Ways` bump measured the same way `kL1Ways` 4→8 was
(ExpressionMemoTest pins the byte budget), and it lands as its own commit.

## 4. MIS correctness argument

After S1, `EvalBSDFAtVertex` and `EvalPdfAtVertex` see the same
`derivatives`/`signals` the SPF's `Scatter` saw at sample time, so:

- forward throughput = f(sampled dir) / pdf(sampled dir) is evaluated on
  one material state;
- every reverse pdf in `MISWeight`'s ratio chain and every VCM
  `dVCM/dVC/dVM` update is computed for the material the samples were
  drawn from;
- BDPT's power-2 and VCM's balance heuristics stay untouched
  ([MIS_HEURISTICS.md](MIS_HEURISTICS.md)); nothing here touches a weight
  formula.

The reviewer axis for S1 (Opus tier) audits exactly this: every
`EvalPdfAtVertex` consumer is traced to confirm the record it prices is the
widened one, and no site rebuilds a record by any other route.

## 5. The fix for rows 5–6: `LightSampler` emitter records (S3)

The NEE light sample and the light-subpath root are **sampled points**, not
hits: `IObject::UniformRandomPoint` returns position, normal and UV only, and
no geometry stamps derivatives or signals for a point it did not intersect.
Two honest routes exist; S3 picks between them by cost, with these fixed
constraints:

- **Zero cost when no signal consumer is live.** Gate on
  `SurfaceCurvatureDemand::Any() || SurfaceSignalDemand::Any()` (both
  relaxed atomic loads, already process-wide). Scenes without signal-reading
  painters take today's path byte-for-byte.
- **The record must come from a real intersection.** Never fabricate
  derivatives or signals for a sampled point. Under the gate, obtain the
  emitter record by intersecting the **luminary object itself**
  (`IObject::IntersectRay` on `lumEntry.pLum`, whose transform layer stamps
  `derivatives` and whose geometry stamps the own-surface signal half) along
  the ray the record is built for — for NEE, from the shading point toward
  `ptOnLum`; for the light-subpath root, a short probe onto the sampled
  point along `-normal` — then stamp the cross-object triple
  (`pScene`/`pSelf`/`ptWorld`) exactly as `ObjectManager::IntersectRay`
  does, with the same values. `LightSampler.cpp` joins the hygiene set with
  that rationale (§3.1). Accept the hit only if it lands on the luminary
  within a distance tolerance derived from the object's diagonal (the
  `SelfHitRootFloor` convention), else fall back to today's record.
- **The shadow test is not replaced** (transparent-shadow transmittance
  through `ShadowOccluded*` stays as is); the probe is one extra
  object-level closest-hit under the gate.
- **The NM twins** (~:2451 and BDPT ~:5239) get the same record; the RGB
  and NM sites share one helper so they cannot drift.

Test (in S3): an emissive SDF sphere whose exitance expression keys on
`curv` (constant on a sphere, so the "baked" control is a plain constant),
lighting a Lambertian plane; under PT the plane's mean with the expression
must equal the mean with the constant within band; the same under BDPT and
VCM. Red-proof: with the probe disabled the PT row goes red (NEE reads
neutral `curv = 0`), which pins that this slice reaches PT.

## 6. The money test — `SignalIntegratorConsistencyTest` (S2)

Two layers, both reference-free with respect to PT (PT has been the broken
reference twice this month — debts 25/26 and the env double-count).

### 6.1 Constant-signal unit scenes (the sharp gate)

For each signal, a scene where the signal is a **known constant** over
every visible surface, so the control is the same expression with the
signal replaced by that constant — not by its neutral:

| signal | scene | constant | drives |
|--------|-------|----------|--------|
| `curv` | one `sphere_geometry`, omni light, black backdrop | `H · scaleHint` is constant on a sphere (`SurfaceCurvatureTest` already pins the value) | colour ramp AND `ggx` roughness (0.08 ↔ 0.6) |
| `occlusion(r)` / `convexity(r)` | an SDF with a deep uniform crease (a subtracted slot whose floor the camera looks at) | constant along the floor away from the ends (`SurfaceSignalsTest` pattern) | colour + roughness |
| `thickness(r)` | a thin SDF slab, camera on the broad face | constant away from edges | colour + roughness |
| `proximity(r)` / `interior(r)` | receiver plane under a wide flat box at height `h`, camera between them with a narrow FOV | `1 − h/r` exactly (CROSS_OBJECT_PROXIMITY_DESIGN.md closed form) | colour + roughness |

For each scene and integrator I ∈ {PT, BDPT, VCM} (MLT covered by
inheritance; a single MLT row on the `curv` scene as a smoke check), with
`oidn_denoise FALSE`, `pixel_filter box`, no adaptive sampling:

```
| mean(I, expr) / mean(I, control) − 1 | < band
```

Band: derived by the worker from the observed run-to-run spread at the
chosen spp (target ≤ 1 % noise on the mean, band 3 %), stated in the test
header with the derivation. Sensitivity requirement: the expression must
move the mean by ≥ 25 % between live and neutral, so a neutral read cannot
hide inside the band — the test asserts this on the PT row
(`mean(PT, expr) / mean(PT, neutral-baked) − 1` outside the band) so the
check can never pass by insensitivity.

**Red-proof protocol** (isolated worktree, never the shared checkout): drop
the `signals` copy from `PopulateRIGFromVertex` → BDPT and VCM rows go red,
PT rows stay green; drop the `derivatives` copy → only the `curv` rows go
red. Both directions recorded in the test header with the observed values.

### 6.2 Showcase ratio-of-ratios (the money numbers)

For each of `plank_closeup`, `tidal_stones`, `shelf_bunny`
(Textures/) and `pavilion_colonnade` (Combined/), loaded through Cst at
reduced resolution (≈ 160×120, low spp, denoise off, box filter — the
`ShelfBunnyShowcaseTest` `RenderDocument` pattern), with the rasterizer
chunk swapped per integrator:

- **E** = the scene as shipped (signals live);
- **B** = the same document with every signal call rewritten to its
  **neutral** constant by whole-word text substitution
  (`occlusion(…)`→`1`, `thickness(…)`→`1`, `convexity(…)`→`0`,
  `proximity(…)`→`0`, `interior(…)`→`0`, `curv`→`0`, `curvR`→`0`), so
  everything else — `fbm`, `fw` fades, relief — stays identical between E
  and B and cancels in the ratio.

Invariant, per showcase and per I ∈ {BDPT, VCM}:

```
R_E = mean(I, E) / mean(PT, E)
R_B = mean(I, B) / mean(PT, B)
| R_E / R_B − 1 | < band          (band 5 %, stated with its derivation)
```

Before S1, a neutral-reading integrator makes `mean(I, E) ≈ mean(I, B)`
while `mean(PT, E) ≠ mean(PT, B)`, so `R_E / R_B` sits away from 1 by the
signal's own effect; after S1 it returns to 1 within noise. The four
`R_E / R_B` values per integrator are the "invariant numbers per integrator
per showcase" the charter asks for, reported in §9 before and after.
Sensitivity is asserted the same way as §6.1 (`mean(PT,E)/mean(PT,B)` must
be outside the band on every showcase, else that showcase cannot witness
the gap and is dropped with a note). `pavilion_colonnade` ships under the
legacy `pixelpel_rasterizer`; the test swaps in the three modern chunks like
the others and says so.

Any residual `R_E / R_B` that survives S1 and is not noise is a
non-signal integrator disagreement — recorded in RENDERING_INTEGRATORS.md
beside debt 27 (charter item 6), never absorbed into the band.

## 7. Cost gate (§ performance-work-with-baselines)

Scene: `plank_closeup` (signals drive colour, roughness and relief; the
memo's own benchmark). Baseline measured on master `185b0d5f` **before any
code change** with `render_thread_reserve_count 0`, `oidn_denoise FALSE`,
sequential renders:

- **Wall time** — 3 runs each of PT / BDPT / VCM (`Total Rasterization
  Time`), mean ± stddev.
- **σ²·T** — K = 8 EXR trials per integrator (`Rec709RGB_Linear`,
  `HDRVarianceTest` mode 1 mean/median per-pixel σ²) × wall time.

After S1 (and again after S3, since NEE probes cost a ray), the same
protocol on the same machine state. Report per integrator: T before/after,
σ² before/after, σ²·T before/after, and the memo L1/L2 hit rates under
BDPT (a temporary counter, removed before commit, as the memo arc did).
Expectation to be confirmed, not asserted: BDPT/VCM T rises by the cost of
the previously-skipped estimator work (occlusion/proximity queries that
neutral reads never paid for) minus memo hits; σ² moves because the image
changes (the neutral render was wrong), so σ²·T is reported for what it is
and the comparison that matters is σ²·T of the *correct* image after vs
PT's σ²·T on the same image class.

## 8. Containment removal and the doc/descriptor family (S4)

Once S1–S3 have landed and §6 is green:

- Delete `WarnIfNonPTRenderHasLiveSignalConsumer` and its four call sites
  (`BDPTPelRasterizer.h`, `BDPTSpectralRasterizer.h`,
  `VCMRasterizerBase.cpp`, `MLTRasterizer.cpp`, `MLTSpectralRasterizer.cpp`).
- Delete `SurfaceSignalDemand` (its only consumer was the warning) and the
  `m_signalDemand` registrations in both expression painters;
  `SurfaceCurvatureDemand` and `ProximityDemand` stay (they gate real work).
- Rewrite the limitation family in one pass: the curv/occlusion/... builtin
  descriptor text (`ChunkParserRegistry.cpp` ~:1631 and any twin),
  `scenes/FeatureBased/README.md` ~:56, OCCLUSION_CONVEXITY_AND_EDGE_SIGNAL.md
  ~:813, PROXIMITY_SHOWCASES.md, `ExpressionPainter.h` / `ExpressionEval.h`
  comments, `MeshSignalBake.cpp`, `ISurfaceSignalProvider.h` (the
  `SurfaceSignalInfo` "ZERO IS THE HONEST ABSENCE" paragraph names
  `PopulateRIGFromVertex` as a neutral source), `RayIntersectionGeometric.h`,
  `BDPTVertex.h`, `PathVertexEval.h`, and the `tests/ProximitySignalTest.cpp`
  comment at ~:1282.
- GEOMETRY_SHADING_SIGNALS_DESIGN.md: header status line + §13 "Known
  residual" + §14 item 11 → **CLOSED 2026-09-xx** with the §6.2 numbers and
  §7 cost deltas. CROSS_OBJECT_PROXIMITY_DESIGN.md §5.1, §10 and the
  "PT-only more sharply" bullet → closed likewise.
- AUTO_RASTERIZER_DESIGN.md: one sentence recording that no signal rule
  ever existed and none is needed.
- `docs/README.md` index entry for this document; `tests/README.md` row for
  the new test.
- Agent skills: `skills/agent/*.md` carry no integrator warning about
  signals (audited 2026-09-11: only neutral-value teaching, which stays
  true); nothing to change there.

## 9. Slices, gates, reviewers

All slices on branch `signals-bidir` in a dedicated worktree; one writer at
a time; each slice converges through the implementation-review-loop before
the next starts. Gate suites are the ones that exercise the touched
classes (grep-derived), linked per test — never `make tests`.

| slice | worker tier | content | gate suites |
|-------|-------------|---------|-------------|
| S1 | Opus | §3 widening + sentinel + hygiene set + comment family in the touched headers | BDPTVertexRIGRebuildTest, PathValueOpsTest, BDPTStrategyBalanceTest, VCMStrategyBalanceTest, BDPTPhantomStrategyWeightTest, VCMEyePostPassTest, VCMLightPostPassTest, VCMSpectralRecurrenceTest, VCMRecurrenceTest, VCMLightVertexStoreTest, SourceHygieneTest, ExpressionMemoTest, SurfaceSignalsTest, ProximitySignalTest, MeshSignalBakeTest; library 0 warnings |
| S2 | Sonnet | §6 test (both layers), red-proofs in an isolated worktree, tests/README row | the new test; SourceHygieneTest |
| S3 | Opus | §5 LightSampler records + test + hygiene set | S3 test, EnvLightBalanceTest, LightSoloTest, LightBVHTest, AreaLightShaderOpScalarNTest, TransparentShadowTest, BDPTStrategyBalanceTest, VCMStrategyBalanceTest, SourceHygieneTest |
| S4 | Sonnet | §8 removal + doc family | SourceHygieneTest, ProximitySignalTest, ExpressionMemoTest, CstDeriveGoldenTest (descriptor text), AgentSkillsTest if any skill text moves |

Reviewer axes per round: (1) transport/MIS correctness — Opus — every
`EvalPdfAtVertex` consumer priced on the widened record, no second rebuild
route, the hygiene argument; (2) test strength / red-proof honesty —
Sonnet; (3) cost + comment honesty — Sonnet — numbers recomputed from the
logs, every "neutral under BDPT" sentence in the tree found and fixed.

Ledger (filled per slice):

- S1 — _pending_
- S2 — _pending_
- S3 — _pending_
- S4 — _pending_
- Cost gate — baseline on master `185b0d5f`: _pending_

## 10. Residuals and non-goals (stated up front)

- **BSSRDF entry vertices** (row 3) read default `derivatives`/`signals`
  under PT and BDPT alike; an IOR or Fresnel painter keyed on a signal at a
  subsurface entry point is integrator-consistent but neutral. Closing it
  means a probe record in `BSSRDFSampling::SampleResult`; out of scope,
  disclosed in PathVertexEval.h's contract.
- **GUI painter preview, realize-time displacement, `HairGenerator`** —
  not transport; unchanged and already disclosed.
- **Ray differentials under BDPT/VCM/MLT** — none today, so `fw`/`fwo` are 0
  there; a PT-vs-bidirectional difference on filtered-noise bands that §6.2's
  control cancels; recorded in RENDERING_INTEGRATORS.md only if it shows.
- **Debt 27** (PT under-reads gapped two-layer weaves) — untouched; any new
  integrator-disagreement data from §6.2 is appended there.
- **No weight formula changes** — MIS heuristics per integrator stay as
  documented in MIS_HEURISTICS.md.
