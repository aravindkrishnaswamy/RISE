# Geometry Shading Signals Under Bidirectional Transport — closing item 11

**Status:** IMPLEMENTED 2026-09-11 — slices S1–S4 landed on branch
`signals-bidir` and each converged to zero P1 through independent review
rounds (§9 is the per-slice ledger with every round's verdict and the
measured numbers); the final whole-branch round is recorded at the end of
§9.
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

**(Written before slice S1; kept as the problem statement. As of S1
(`925c2720`, 2026-09-11) the rebuild described below carries all three
field groups, so the surface-vertex half of this gap is CLOSED — see §9;
what remains open is §2 rows 5–6, slice S3.)**

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
in both subpath generators). One material, one vertex, one
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
| 1 | `PathVertexEval::PopulateRIGFromVertex` — called by name 6× inside `PathVertexEval.h` (the four `Eval*AtVertex{,NM}` bodies plus the two BSSRDF-entry rebuilds), 5× in `BDPTIntegrator.cpp` (three emitter-radiance evals — `LuminaryRadiance`, `EvalEmitterRadiance` Pel/NM — plus the zero-exitance-light NEE sweep and `GetSpecularInfoNM`'s delta-vertex IOR lookup) and 2× in `VCMIntegrator.cpp`; reached indirectly by every `EvalBSDFAtVertex` / `EvalPdfAtVertex` consumer — 41 occurrences in `BDPTIntegrator.cpp`, 14 in `VCMIntegrator.cpp` — through the tag-dispatch wrapper `PathValueOps.h`, which rebuilds nothing itself (MLT drives BDPT's machinery and inherits all of them) | BSDF value (`EvalBSDFAtVertex{,NM}`), SPF pdf (`EvalPdfAtVertex{,NM}`) for: forward-walk throughput re-pricing (`EvalBSDFAtVertex( vertices.back(), … )` in `GenerateEyeSubpathImpl` / `GenerateLightSubpathImpl`), OpenPGL guiding RIS candidate scoring and HWSS companion evals in both generators, every connection strategy (s,t), NEE at eye vertices, MIS reverse pdfs (`pdfRev` in both generators and every `EvalPdfAtVertex` in the connection strategies), VCM connections and VCM merges at the EYE vertex (`EvaluateMergesImpl`), emitter radiance at eye-end vertices (`LuminaryRadiance`, `EvalEmitterRadiance`), BSSRDF profile Fresnel/IOR at entry vertices | (a) forward AND reverse | **S1**: widen `BDPTVertex` (§3) |
| 2 | `BDPTIntegrator.cpp` eye-subpath vertex population (`GenerateEyeSubpathImpl`, the `BDPTVertex v; v.type = SURFACE` block) and light-subpath population (`GenerateLightSubpathImpl`, same block) | the copy from `ri.geometric` into the vertex | (a) | **S1**: copy `derivatives`, `signals`, `txFootprint` |
| 3 | BSSRDF entry vertices (the four `BDPTVertex entryV` sites, diffusion-profile and random-walk, eye and light) built from `BSSRDFSampling::SampleResult` (entry point / normals / ONB only) | `FresnelTransmission(cos, rig)` / `GetIOR(rig)` on the diffusion profile | (a)-narrow, **integrator-consistent**: PT builds its BSSRDF entry record by hand too (`BSSRDFSampling.h` comment "PT 1508/etc.") | residual, disclosed in §10 — an IOR painter keyed on a signal at a BSSRDF entry reads neutral under every integrator alike; not a PT-vs-BDPT disagreement |
| 4 | VCM `LightVertex` store (`VCMLightVertex.h`) | nothing — merges evaluate the BSDF at the **eye** `BDPTVertex` only (`EvalBSDFAtVertex<Tag>( v, wiAtEye, … )`, ~:2260); the stored light vertex contributes `wi`, throughput and MIS quantities | n/a | no widening of the store; stated in the store's header by S1 |
| 5 | `LightSampler.cpp` NEE emitter records (~:1949 RGB, ~:2451 NM): `vNormal/vGeomNormal/ptCoord/onb` only, from `UniformRandomPoint` | `IEmitter::emittedRadiance{,NM}` — the emission painter | (a) for an emissive material whose radiance keys on a signal; reaches **PT** | **S3** (§5) |
| 6 | `LightSampler::SampleLight` emission record (~:1124) → consumed as `ls.Le` by BDPT's light-subpath root (`GenerateLightSubpathImpl`; the NM hero twin AND the HWSS companion-wavelength twin `rigW` rebuild the same minimal record beside it) and VCM's (`VCMIntegrator.cpp`, the `SampleLight` consumer). The LIGHT-type root vertex built there also carries default `derivatives`/`signals`, and two emitter evals price THROUGH it via the helper (`LuminaryRadiance` for the t=1 splat; VCM's light→camera splat) | light-subpath root `Le`, and the light-vertex emitter evals | (a), same material class as #5 | **S3**: the record AND the copy onto the root vertex |
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

**(As built — rewritten after S3's three review rounds; the original draft
prescribed a second, along-`vToLight` probe for NEE, which S3 round 2 found
to have a different refusal predicate from the root probe and deleted.)**

The NEE light sample and the light-subpath root are **sampled points**, not
hits: `IObject::UniformRandomPoint` returns position, normal and UV only, and
no geometry stamps derivatives or signals for a point it did not intersect.
S3 obtains them like this:

- **One normal-aligned probe** — `LightSampler::ProbeEmitterSurface` —
  shared by the two NEE sites (RGB and NM) and by `SampleLight` for the
  light-subpath root, called with the same arguments (the sampled point,
  the sampled normal, the luminary, the object manager) at every site, so
  PT, BDPT and VCM **refuse in exactly the same places**. It stands off the
  sampled point along its normal by
  `max( 0.001·D, 1.01·SelfHitRootFloor_world + 0.001·D )` (D = the luminary's
  world bbox diagonal; the floor asked in the luminary's local frame and
  converted by the ray's stretch; the 0.001·D cushion is the smaller of the
  two values tried — 0 fails on a high-epsilon SDF because a sample the
  Newton projection left INSIDE the surface trips the marcher's step-off),
  fires back along the normal with a travel limit of `2·standoff + 0.01·D`,
  intersects the **luminary object itself** (`IObject::IntersectRay`, whose
  transform layer stamps `derivatives` and whose geometry stamps the
  own-surface signal half), and accepts iff the hit is on that luminary
  within `0.01·D` of the sampled point; then it stamps the cross-object
  triple (`pScene`/`pSelf`/`ptWorld`) with the values
  `ObjectManager::IntersectRay` would, so `LightSampler.cpp` is the tenth
  sanctioned `signals` writer (§3.1). On refusal the record is left exactly
  as it was.
- **Gated** on `SurfaceCurvatureDemand::Any() || SurfaceSignalDemand::Any()`
  (two relaxed atomic loads, checked before any bounding-box read or
  payload construction). With no signal-reading painter alive: no ray is
  cast, no bounding box is read, no signal field moves, no sampler
  dimension is consumed — the render is unchanged. Not "byte-for-byte":
  see the next bullet.
- **`Po` is UNGATED and ray-free.** The emitter record's `ptObjIntersec`
  (the expression VM's `Po`, also read by `voronoi3d_painter` and
  `mapping_painter`) is computed by `LightSampler::EmitterObjectPoint` —
  the sampled world point through the luminary's final inverse transform
  (two virtual calls, a 128 B matrix by value, one 4×4 transform) — at the
  three sampling sites (`SampleLight`, RGB NEE, NM NEE) and COPIED from
  `LightSample::ptObjIntersec` at the other four record sites (BDPT's NM
  hero, HWSS companion and LIGHT root; VCM's NEE record), so the transform
  is paid once per sampled point. This closes a PRE-EXISTING inconsistency for
  every `Object` luminary — a direct hit carried the live object-space
  point while every NEE record carried `(0,0,0)` — and it must not ride
  the gate, or a `curv` painter on an unrelated material would change a
  `Po`-keyed emitter's brightness (S3 round 1 reproduced exactly that at
  2.94×). CSG composites keep `(0,0,0)` (their direct hit reports the
  winning operand's frame); delta and env samples have no surface.
- **What the probe applies** (`ApplyEmitterSurface`): `derivatives`,
  `signals`, `txFootprint` — nothing else. The sampled point, normal, ONB,
  UV and every pdf/cosine are untouched, so no MIS quantity moves.
- **Every twin gets the same record**: the NM NEE site, and in
  `GenerateLightSubpathImpl` the NM hero `Le` rebuild, the HWSS
  companion-wavelength `rigW` rebuild and the `type == LIGHT` root vertex
  (`derivatives`/`signals`/`txFootprint` copied onto it, so
  `LuminaryRadiance` and VCM's light→camera splat price the root live
  through `PopulateRIGFromVertex`), and VCM's own NEE record — five
  consumers of `LightSample::surface`, seven record sites.
- **The shadow test is not replaced** and still runs before the NEE probe;
  that ordering is a cost ordering (an occluded sample never pays a probe),
  not a correctness bound — the normal-aligned probe never looks down the
  shadow ray.
- **Residuals** (§10): the probe refuses — neutral, identically under every
  integrator — on a non-finite floor or a standoff larger than D (reachable
  only through a CSG luminary's uncapped floor, for a uniformly transformed
  luminary); and a second surface of the same luminary inside the standoff
  band along the normal and within the tolerance is ACCEPTED with that
  neighbour's channel (a bounded neighbour read, not a neutral one).

Test (S3): `tests/SignalEmitterRecordTest.cpp` — six families (an emissive
SDF sphere and an analytic sphere keyed on `curv`, whose closed-form control
is `1.0`; a `proximity(1.0)`-keyed quad beside a box; a `Po`-keyed emitter
with no signal painter plus a gate-invariance block; a high-epsilon SDF
whose floor exceeds the flat standoff; a two-lobe non-convex emitter with
`casts_shadows FALSE` whose far lobe the along-ray probe missed), PT/BDPT/VCM
plus spectral and HWSS rows, seeded per render. Red-proofs (eleven, in the
header): forcing `EmitterProbeWanted()` false reddens every money row
including PT (the slice reaches PT); dropping the cross-object stamp
reddens only the proximity rows; a flat standoff reddens the high-epsilon
family; the along-ray probe reddens the two-lobe family at PT 23 %.

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
(`| mean(PT, expr) / mean(PT, neutral-baked) − 1 | ≥ 0.25`) so the check
can never pass by insensitivity (observed margins 0.54–3.9).

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

**Masked form (added by the S2 follow-up, `75994824`, after every showcase
proved insensitive at whole-image level — 0.05 %–1.9 % under PT).** From the
two PT renders, the MASK is the set of pixels where
`|PT(E) − PT(B)| / max(PT(B), ε) > 0.2` — the pixels the signals actually
move; a showcase whose mask covers < 1 % of the frame is dropped with a
printed note. The masked means `mean_mask(I, E)` / `mean_mask(I, B)` give
`R_E,mask` and `R_B,mask`, and the assertion is
`| R_E,mask / R_B,mask − 1 | < 20 %`, the band derived from the masked-mean
run-to-run spread at 16 spp (≈ 2–3 % on a 2 %-coverage mask). The
whole-image ratios are still printed. Any (showcase, integrator) whose
whole-image means for BOTH E and B sit outside `[0.5×, 2×]` of PT AND agree
with each other (`| mean(I,E)/mean(I,B) − 1 | < 10 %`) is a non-signal
integrator disagreement: the test prints a labelled `INTEGRATOR
DISAGREEMENT` line with both ratios, SKIPS its ratio assertion for that
row, and counts the skip in its summary — recorded in
RENDERING_INTEGRATORS.md as debt 28 (charter item 6), never absorbed into
the band. An ASYMMETRIC blow-up (only E or only B outside the window, or E
and B disagreeing by more than 10 %) is exactly the signature of a
signal-attributable regression and FAILS instead of skipping (S2 review
round 1, finding F2). The convexity control constant is a 21×21
camera-frustum quadrature of the per-hit value, cross-checked against 33×33
to within 1 % (the per-hit "spin" bias converges from 21 on).

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

- Delete `WarnIfNonPTRenderHasLiveSignalConsumer` and its five call sites
  (`BDPTPelRasterizer.h`, `BDPTSpectralRasterizer.h`,
  `VCMRasterizerBase.cpp`, `MLTRasterizer.cpp`, `MLTSpectralRasterizer.cpp`).
  (Between S1 and S4 its text is NARROWED, not deleted — S1's review round
  found the old wording false the moment S1 landed.)
- **KEEP `SurfaceSignalDemand`** and the `m_signalDemand` registrations —
  this bullet used to delete them as warning-only, but slice S3 made the
  counter gate REAL work (the emitter-record probe, §5), exactly as
  `SurfaceCurvatureDemand` and `ProximityDemand` do; S4 rewrites its
  "diagnostic-only" doc comment instead (S3 already did the first pass).
- The limitation family, per file, with its status at the S1 branch head
  (S4 finishes every row marked "S4"; rows marked "S1" are already true and
  need only their "(until S3)" clauses dropped once S3 lands):

  | surface | status |
  |---------|--------|
  | `ChunkParserRegistry.cpp` builtin descriptors (~:1631 scalar pipe, ~:6958 colour pipe) — "BDPT / VCM / MLT currently evaluate them as their neutral fallback in PARTS of their transport … one-time warning" | S4 |
  | `scenes/FeatureBased/README.md` ~:56 and the two showcase headers `GeometrySignals/weathered_reliquary.RISEscene`, `Materials/velvet_cushion.RISEscene` | S4 |
  | OCCLUSION_CONVEXITY_AND_EDGE_SIGNAL.md §7 "BDPT / VCM / MLT residual"; PROXIMITY_SHOWCASES.md (the containment-warning sentence) | S4 |
  | `ExpressionPainter.h` (`m_signalDemand` comments) / `ExpressionEval.h`; `MeshSignalBake.cpp` | S4 rewrites the comments (the member stays — see §8's first bullets) |
  | `ISurfaceSignalProvider.h` "ZERO IS THE HONEST ABSENCE" paragraph and `NeutralProximity()`'s doc block; `SurfaceSignalProximity.h` precondition table; `RayIntersectionGeometric.h` `signals` comment; `BDPTVertex.h`; `PathVertexEval.h` contract block | S1 — rewritten: they state that `PopulateRIGFromVertex` FORWARDS the stamp and name the S3 residual |
  | `ISurfaceSignalProvider.h` `WarnIfNonPTRenderHasLiveSignalConsumer` doc block + `PrintEx` string | S1 narrowed it to the S3 residual; S3 narrowed it again (no bidirectional-specific neutral read remains; still `eLog_Warning` by name); S4 deletes the helper and its five call sites — `SurfaceSignalDemand` and the `m_signalDemand` members STAY (they gate the S3 probe) |
  | `tests/ProximitySignalTest.cpp` — the comment near its "(f) teeth" checks names `SurfaceSignalDemand` as what a signal call registers (no integrator claim of its own) | S4 — follows the namespace out |
  | `skills/agent/materials-and-media-basics.md` wetness section ("A wet PT render will not match…") | S4 |
  | WETNESS_COAT_DESIGN.md (§3.7 item 3, §6.9 item 10, §9, §12 item 1); CLOTH_FABRIC_DESIGN.md (§15 item 8) | S1 — interim dated notes; S4 final wording |
  | CROSS_OBJECT_PROXIMITY_DESIGN.md §2 definition, §5.1, §6 "PT-first" bullet, §10 both bullets | S1 — closed with dated notes; S4 drops the "(until S3)" clauses |

- GEOMETRY_SHADING_SIGNALS_DESIGN.md: header status line + §13 "Known
  residual" + §14 item 11 → **CLOSED 2026-09-xx** with the §6.2 numbers and
  §7 cost deltas (S4).
- AUTO_RASTERIZER_DESIGN.md: one sentence recording that no signal rule
  ever existed and none is needed.
- `docs/README.md` index entry for this document; `tests/README.md` row for
  the new test.
- Agent skills: `skills/agent/materials-and-media-basics.md` (the wetness
  section, "A wet PT render will not match a wet BDPT or VCM render")
  carries an integrator warning about `occlusion()` / `curv` and must be
  rewritten to the post-S3 truth (an earlier audit line here claimed no
  skill carried one — reviewer D of S1 round 2 found this one).
  `procedural-textures.md` and `object-modeling-recipes.md` carry only
  neutral-value teaching, which stays true. (The showcase headers and the
  two material design docs are rows of the table above.)

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

- S1 — landed `925c2720` (sizeof(BDPTVertex) 608 → 1016 B, +408 exactly, recomputed by two reviewers; 15 gate suites green, 0 warnings over 228 recompiled TUs). Review round 1 (A transport/MIS on Opus, B test strength, C comment fidelity; its fixes landed in two commits, `cb210926` and `5b28c33e` — the latter's title says "round 2" but it is round 1's P2 follow-up): transport core CLEAN; one P1 shared by A and C — the containment warning's doc block and runtime string still claimed the whole bidirectional transport reads neutral — fixed `cb210926` (narrowed to the S3 residual); P2s fixed `cb210926` (doc counts, symbol cites, S3 scope: HWSS companion record + LIGHT-root vertex copy, five call sites) and `5b28c33e` (sentinel test 55 → 68 checks: mixed-boolean passes close the same-struct boolean-swap blind spot, `mediumIOR`→`ambientIOR` sentinels incl. the ≤0 guard; `VCMLightVertex.h` stale vColor replay comments rewritten). B's red-proofs: dropping each of the three copies goes red by member name (13 / 16 / 11 FAILs); a population-site omission is caught by NO S1 suite — that is §6's job, as designed. Round 2 (D doc family, E correctness — fresh): D found ONE P1 — the proximity doc's §10 residual bullet still said the contract was "explicitly declined", contradicting its own §5.1 — fixed in the next commit, together with D's P2 (the §8 skill-audit line was wrong: `materials-and-media-basics.md` carries an integrator warning; now on S4's list). E: _pending_. Round 3 (doc fidelity, fresh): TWO P1s, both the same stale sentence in CROSS_OBJECT_PROXIMITY_DESIGN.md that round 2 had fixed in one place (§2 the signal's definition; §10 the "PT-only more sharply" bullet) — fixed in the next commit together with the §6 "Engine principles, checked" bullet ("PT-first; BDPT/VCM/MLT residual"), after a whole-file sweep for the family (`PathVertexEval`, `rebuilt record`, `non-PT`, `read 0`, `PT-only`, `BDPT/VCM/MLT`). E (round 2, correctness, fresh): CLEAN, zero P1 — both population sites copy before every exit, defaults value-initialised, thread-locals cleared per sample, no layout consumer of sizeof(BDPTVertex); red-proof recount 18/13/13 on the 68-check suite reconciles with 16/13/11 on the 55-check suite; one P2 (the warning's doc block attributed the light-subpath root vertex to LightSampler.cpp — it is built by GenerateLightSubpathImpl) fixed in the round-4 commit. Round 4 (doc fidelity, fresh): TWO P1s — `NeutralProximity()`'s doc block in ISurfaceSignalProvider.h still listed "a hit rebuilt by BDPT/VCM/MLT" (a SOURCE sibling the doc sweeps never covered), and this document's §1 problem statement carried no closure note — plus the `§8.x` mis-cite; all fixed in the next commit, together with the same sentence's last sibling in SurfaceSignalProximity.h (found by a supervisor grep of all of src/ for `BDPT/VCM/MLT`). Round 5 (doc fidelity, fresh): FIVE P1s, all the same restated debt in two material design docs the branch had never touched — WETNESS_COAT_DESIGN.md ×4 and CLOTH_FABRIC_DESIGN.md ×1 — annotated with the interim S1 truth in the next commit after a docs-wide grep for `item 11` and the "neutral in parts" phrasing (the only other hits are the S4-scheduled files). Round 6 (doc fidelity, fresh, pinned to `8687bfb1`): ONE P1 — a section mis-cite in §8 (WETNESS_COAT_DESIGN.md's item-10 list is §6.9, not §6.10); every annotation's content verified true; every hit outside the S4-scheduled files classified true/historical/unrelated. Fixed in the next commit. Round 7 (confirming pass): the one-line fix confirmed; ONE P1 in the original text — §8 cited `tests/ProximitySignalTest.cpp ~:1282` as carrying the integrator sentence; it carries a `SurfaceSignalDemand` mention (S4 deletes that namespace), nothing about integrators — reworded. Round 8 (confirming): ONE P1 — §8's parenthetical on `ISurfaceSignalProvider.h` still described that paragraph's PRE-S1 wording ("names PopulateRIGFromVertex as a neutral source"); §8's family list is now a per-file STATUS table (S1-done / S4) so it cannot describe a snapshot again. Round 9 (confirming, fresh): CLEAN — every §8 table row's status verified against the tree, ledger reconciled with the 12-commit log, 68 / 165 reproduced. **S1 CONVERGED at `78dc866b`** (9 rounds: code clean from round 1 on three independent correctness lenses; rounds 3–8 were the doc-drift family — the lesson, again: a list that describes a snapshot of other files rots one row per round; state status, not wording). Cost gate on the plank showcase (K=8 σ² + 3-run wall, `render_thread_reserve_count 0`, OIDN off): PT 18.0→19.0 s (noise), BDPT 22.7→21.3 s (−6 %), VCM 23.8→22.6 s (−5 %); σ² unchanged within K=8 noise (PT 2.19e-4, BDPT 9.49e-3→9.58e-3, VCM 2.49e-5); σ²·T: BDPT 0.216→0.204, VCM 5.9e-4→5.6e-4. The speed-up is the §3.2 memo win: with neutral reads the live-hit context and the rebuilt-record context had different memo keys, so every vertex evaluated its programs at least twice. Image means moved +0.4 % (BDPT) / +0.5 % (VCM) — the plank's signal-driven pixels are a small fraction of the frame (§6.2's masked layer exists for that reason).
- S2 — landed as `0bc468a4` + `75994824` (authored against the UNFIXED tree first: 13 red rows at −0.82…+1.18 with PT green, then re-run on S1: every row within 3e-5 except convexity 1.4 %; red-proofs on the fixed tree: dropping the `signals` copy reddens exactly the 10 occlusion/convexity/thickness/proximity/interior BDPT+VCM rows, dropping `derivatives` exactly the 3 curv rows). Review round 1 (F test strength, G header/doc truth): F — two P1s in the showcase harness's safety valves (the PT swap had no positive check; the blow-up skip never checked that E and B agree) + three P2s; G — header numbers that did not recompute from their cited logs ("four scenes" for six; a 9.3 % failed formulation described as 9.3e-3 passing; a table value copied from the wrong row), the design's own debt-28 prose ("< 0.1 %" E-vs-B, a 1/N scaling that is really sub-linear) and a missing §6.2 masked-layer spec (both fixed `4b0c88d5`). Fix `74c4511b`: positive chunk-keyword + param read-back for every integrator; symmetric-blow-up rule (both E and B outside [0.5, 2] AND |E/B−1| < 10 %, else FAIL as possibly signal-attributable); convexity control grid 9² → 21² cross-checked against 33² (0.16 %, PT ratio now ≤ 0.002); masked band re-derived from n=5 (max |ratio−1| 0.128 on plank BDPT, band 20 %); sensitivity gate 0.25; header numbers re-derived from two fresh runs (332/0/4 skips each). Round 2 (K harness, L header/doc truth — fresh): test file and README CLEAN on re-derivation; K — one P1-grade flake risk (plank BDPT masked ratio: worst of 9 runs −0.128 against the 20 % band, 1.66σ from the edge) + two P2s (rasterizer-chunk uniqueness not asserted; `ClassifyBlowup`'s comment says ratios agree, the code compares the integrator's own E and B means); L — two P1s in the SUPERVISOR's debt-28 entry (skip rule described pre-F2; E-vs-B floors 2× too high and contradicting the test header) fixed in the next commit, plus an unsourced convexity-sweep paragraph (P2). Fix round 2 `0ca944d9`: raising spp did NOT shrink the masked-ratio spread (96 spp: σ 0.055), so BDPT/VCM masked ratios are now the mean of 6 independent full renders — plank BDPT mean −0.055, σ 0.0185, worst −0.070 vs the 0.20 band (7.8σ from the mean, 2.9× the worst); VCM σ 0.0066; rasterizer-chunk uniqueness hard-checked at both lookups (Job::RegisterAndActivateRasterizer activates the LAST parsed chunk, cited); `ClassifyBlowup` compares the literal ratio agreement; the convexity sweep re-run and saved (one transcription error in the header corrected); default suite 900 checks / 4 skips in ~70 s. Round 3 (confirming, fresh): every round-2 number reproduced; ONE P1 — the test claimed "wall-clock-seeded" independence, but RISE renders seed from an unsynchronized libc `rand()` race (no `srand` anywhere in the path; FabricRenderTest had already diagnosed this) so the derived σ rested on a race. Fix `d20571e6`: explicit `std::srand( seedBase + renderIndex )` before every render (`argv[1]` seed base, default 1000, the PrimitiveSelfHitTest convention); under REAL independence K=6 FAILED the 2×-worst bar (worst −0.120) and K=9 too, K=12 passes (plank BDPT mean −0.070, σ 0.012, worst −0.089; VCM −0.058 / 0.014 / −0.077; margins 1.70× / 1.13× and 1.74× / 1.29×); hard check on the sub-render count; README refreshed (1528 checks, ~105 s). Round 4 (confirming, fresh): zero P1 — srand precedes all three `Rasterize` sites, index monotonic, comments claim exactly what the fix guarantees; K=12 / K=9 / K=6 statistics recomputed from the logs; two fresh seed bases inside the range; 1528/0/4 in 106 s. Two prose nits (README's pre-fix runtime said ~35–38 s where the K=6 state was ~70 s; the header mixed two margin formulas) fixed by the supervisor in `d8f5b633`. **S2 CONVERGED.**
- S3 — landed `87d1a72e` (cherry-picked onto this branch): `LightSampler::ProbeEmitterSurface` / `ApplyEmitterSurface` — under `SurfaceCurvatureDemand::Any() || SurfaceSignalDemand::Any()`, a closest-hit probe on the luminary object itself gives NEE records (RGB + NM) and the light-subpath root (`SampleLight`'s record, carried on `LightSample::surface`, then BDPT's NM hero `Le`, the HWSS companion `rigW`, the LIGHT root vertex incl. `txFootprint` + `ptObjIntersec`, and VCM's NEE record) a REAL intersection's `derivatives` + `signals`, with the cross-object triple stamped as `ObjectManager` does; accept iff the hit is on the luminary within 0.01·D of the sampled point (D = world bbox diagonal), standoff 0.001·D; `LightSampler.cpp` joins the hygiene set (10 writers). `SignalEmitterRecordTest` 21 checks: SDF-sphere `curv` emitter PT/BDPT/VCM 0.007 % / 0.32 % / 0.17 % from control, analytic sphere 0.02 %, `proximity(1.0)`-keyed emitter 1.56 % (band 5 %, sensitivity 3.2–4.0). Red-proofs: probe forced off → PT rows red at 45–75 % (the slice reaches PT); cross-object triple suppressed → only the proximity row red. Gated cost, worst case (every visible point NEE-samples one small emitter, 160², 256 spp, 5 interleaved runs): +17.8 % (985 vs 837 ms); gate closed 821 ms = probe-off within 1 σ. `LightSample` 152 → 592 B (stack local); `BDPTVertex` unchanged. 13 gate suites green incl. EnvLightBalanceTest 116/116, 0 warnings over 372 TUs. Review round 1 (H transport/NEE on Opus, I test strength, J cost/comment): J CLEAN (all 13 suite counts and both sizeofs reproduced; two phrasing P2s). H: FOUR P1s — (1) REPRODUCED at 2.94×: the gated payload carried `ptObjIntersec` (`Po`, not signal state) so a `curv` painter on an unrelated material changed a `Po`-keyed emitter's brightness; ruling: compute `Po` for emitter records UNGATED from the luminary's transform (no ray), which also closes the pre-existing PT direct-hit-vs-NEE `Po` inconsistency; (2) two new comments contradicted each other on `SurfaceSignalDemand`'s fate; (3) the 0.001·D standoff's "9 orders above every SelfHitRootFloor" claim is false for SDFGeometry's override (1e-4·diag normally, up to 0.1·diag) — a non-uniformly scaled SDF luminary makes the root probe refuse while NEE accepts: PT live / BDPT neutral, the very disagreement the narrowed warning denies; ruling: standoff = max(1e-3·D, geometry's `SelfHitRootFloor`); (4) the 0.01·D acceptance rationale was false (SDF samples are Newton-projected to ~5e-5·diag; the real need is grazing-incidence headroom) and the load-bearing shadow-test-before-probe ordering was undocumented. H P2s: unconditional 440 B payload reset per `SampleLight`, row C headroom (worst 2.89 % of 9 runs vs 1.56 % claimed), no bidirectional row exercises the cross-object triple through the root probe, four pre-existing "geometric face normal" comments false for vertex-normal meshes. I: TWO P1s — the header's red-proof recipe names `ProbeEmitterSurface` (NEE only) where the choke point is `EmitterProbeWanted()`; NM NEE / NM hero / HWSS companion sites have no coverage while the header calls the suite the end-to-end witness — ruling: spectral + HWSS rows, not a disclosure. PDF/MIS invariance, thread safety and hygiene were confirmed CLEAN by H. Fix round 1 (`0ad89a9e` + `6339ae15`, cherry-picked here as `96224b97` + `3f38b684`): `Po` left the gated payload and is computed UNGATED and ray-free (`LightSampler::EmitterObjectPoint`, one multiply through the luminary's final inverse transform) at all seven emitter-record sites — closing the pre-existing PT direct-hit-vs-NEE `Po` inconsistency for every emitter; H's 2.94× reproduced at 4.05× with `Po` put back on the gate and gone after (family D + a 4-render gate-invariance block: EXPR 0.000–0.075 %); standoff = max(0.001·D, 1.01·SelfHitRootFloor_world + 0.01·D) — the `+0.01·D` term is load-bearing (1.01·floor alone still hit the far side intermittently); family E (SDF luminary, `epsilon 0.002`) red under the flat standoff at BDPT 74.9 % / VCM 69.0 % with PT green — the exact PT-live/BDPT-neutral split — and green now; acceptance rationale rewritten (projection residual + δ/cos θ, two orders of margin), shadow-test-before-probe ordering documented; `valid = false` instead of the 440 B reset; row C 384 spp (worst-of-5 2.89 → 0.85 %) and now BDPT + VCM (red 75.7 / 68.5 % when the cross-object stamp is dropped at `SampleLight`); five 'geometric normal' comments corrected; red-proof recipe corrected to `EmitterProbeWanted()` (17 FAILs / 47 passes); spectral rows (PT/BDPT/VCM-spectral at 2048 spp + BDPT-spectral HWSS) — the NM NEE site red-proves cleanly (A/PT-spectral 75.6 %), but skipping the NM-hero rebuild moves only A/VCM-spectral by 3.4–5.7 % and skipping the HWSS companion moves NOTHING on this scene, because BDPT prices the emitter through the LIGHT root vertex and VCM through its own NEE record while `LeNM` only seeds light-subpath throughput (MIS-weighted to a few percent here) — DISCLOSED in the header as the two sites the suite does not guard; a light-tracing-dominated scene is the honest guard (ledger residual, §10). 13 suites green, 0 warnings; suite ~57 s. Round 2 (H2 transport on Opus, I2 tests/comments — fresh): all four round-1 P1s verified fixed; PDF/MIS invariance and hygiene re-confirmed; I2 zero P1 (family D's gate-invariance proven non-vacuous by code — Job teardown releases the painters' RAII registrations — and family E's red-proof reproduced by mutation at 75.0 / 69.1 %). H2: TWO P1s and a structural point — (1) the NEE probe fires along `vToLight` and so REFUSES at grazing incidence (δ/cos θ > tol at cos θ ≲ 0.01, ≲ 0.2 on family E's epsilon) while the root probe fires at cos I = 1 and never does — a PT-side neutral read the residual list and warning string deny; RULING: one normal-aligned probe for NEE and root alike, so the three integrators share ONE refusal predicate (family F red-proves it); (2) the `standoff > diag` comment named an SDF path that is arithmetically unreachable (needs floor > 0.98·D; the SDF cap is 0.5·diag) — the reachable path is a CSG luminary's uncapped floor. P2s: the geometry-derived arm silently raised the standoff 10× for every non-SDF luminary (RULING: measure the smallest additive term that keeps E green and name both constants' roles); 440 → 416 B; two residual numbers; the gate-off cost understated (the ungated `Po` transform returns a 128 B matrix by value; the NEE sites still built a 416 B payload with the gate closed); the `Po` closure claim unqualified for CSG luminaries (NEE still 0 there); `Po` agrees with a direct hit to SURFACE_INTERSEC_ERROR, not 'the same bits'; 'worst observed 0.61 %' is a five-run max (reviewers saw 0.74 % / 1.10 %); family D lacked an assert that the gate is actually closed. Fix round 2 (`5ad1fe1a` + `174e1a0a`, cherry-picked here): ONE probe — the along-`vToLight` entry point deleted, `ProbeEmitterSurface` IS the normal-aligned probe, both NEE sites and the root share it, so PT/BDPT/VCM refuse identically; H2's grazing model did NOT reproduce (epsilon up to 0.05 stayed green under the old probe: sampled point and marched hit agree far more tightly than δ/cos θ) — what the along-ray probe genuinely failed was the WRONG-SURFACE window, and family F (two-lobe non-convex SDF emitter, `casts_shadows FALSE`, curv-keyed, closed-form control 1.0, far lobe ~⅓ of the energy) red-proves it: F/PT 23.2 % red under the along-ray probe, 0.01 % unified; the shadow-test-first ordering is documented as a COST ordering, not a correctness bound (the normal-aligned probe never looks down the shadow ray); standoff cushion measured down to 0.001·D (family E worst 0.49 % over five runs; cushion 0 was red at 37 %), both constants named with their roles, interception band stated; `standoff > diag` comment corrected (SDF cannot reach it; a CSG luminary's uncapped floor can); 440 → 416 B; one projection-residual number (~3e-5·D, measured); NEE sites build the payload and fetch the manager only inside the gate, remaining ungated cost stated (two virtual calls + a 128 B matrix by value + one transform per record); `Po` closure qualified for CSG at every claim site; SURFACE_INTERSEC_ERROR wording; 'worst of five' labelled (1.14 % A/BDPT-spectral); gate-closed asserts before each CLOSED render. Red-proof (3) changed materially: E/PT is now red at 75.3 % too — the PT/BDPT disagreement is gone because both refuse. Suite 77/0; 13 gates green; clean rebuild 0 warnings. Process note from the worker: an uncommitted fix was destroyed by a red-proof's `git checkout --` — red-proof briefs must say commit first. Round 3 (H3 transport on Opus, I3 comments — fresh): NO behavioural P1 — one probe, identical arguments at the three sites, no pdf touched, family F's geometry and physics recomputed and correct (the far lobe genuinely lights the receiver with `casts_shadows FALSE`), 77/0 and 116/0; H3's grazing model from round 2 did not survive its own re-derivation (the along-ray probe's real failure was the wrong-surface window). Comment P1s: the louvre case (a second surface of the same luminary inside the standoff band and within the 0.01·D tolerance) is ACCEPTED with that neighbour's channel, not refused — three sites incl. the warning string said the opposite; the cushion's mechanism was stated with the sign inverted (the INSIDE-projected half trips SDFGeometry::March's step-off); I3: one leftover 'normal is geometric' comment at the NM NEE site (fixed `724baddc`). P2s: a 1e-2·D example that is really 1e-3·D; the CSG-vs-SDF `standoff > diag` claim needs 'uniformly transformed'; consumer counts (five consumers of `ls.surface`, seven record sites); two values recorded for red-proof (11); the noise tail exceeded every 'worst of five' again (C/PT 1.53 %, HWSS 1.27 %); the HWSS companion record lacked `vGeomNormal`. All folded into S4b.
- S4 — S4a `fe7fa4a1` (Sonnet): the doc/descriptor/skill family closed — both builtin descriptors, scenes/FeatureBased/README + three scene headers (one found by the sweep: `scenes/Tests/Materials/wetness_prelude_validation.RISEscene`), OCCLUSION_CONVEXITY §7, PROXIMITY_SHOWCASES, `ExpressionPainter.h` / ProximitySignalTest comments, the wetness skill section, WETNESS/CLOTH final wording, CROSS_OBJECT §2/§5.1 (ten writers)/§6/§10, GEOMETRY_SHADING_SIGNALS_DESIGN header + §13 + §14 item 11 CLOSED (history kept in `<details>`), AUTO_RASTERIZER_DESIGN dated note, docs/README index; 9 suites green incl. CstDeriveGoldenTest 450 MATCH and AgentSkillsTest 628. S4b `48ae411a` + `fce4cbed` (Sonnet): `WarnIfNonPTRenderHasLiveSignalConsumer` and its five call sites DELETED (zero hits in src/ tests/); `SurfaceSignalDemand` kept and re-documented as the probe gate; H3's two comment P1s and P2s applied (louvre = accepted neighbour read at three sites incl. both descriptors; cushion sign; 1e-3·D example; 'uniformly transformed'; five consumers / seven sites; 'smaller of the two tried'; `rigW.vGeomNormal`); SignalEmitterRecordTest now seeds explicitly (`argv[1]`, default 1000), five seeded runs 77/0, worst row 0.96 % (A/BDPT-spectral) = 5.2× inside the band; 12 suites + EnvLightBalanceTest 116 green; BDPT and MLT smoke renders clean with no warning line. Final round (M transport+regressions on Opus, N doc family, O source comments/tests — fresh, whole branch): M — zero red across 41 suites + 3 FabricRenderTest filters (every touched class's suites, EnvLightBalanceTest 116 last and alone), every transport invariant re-derived on the final code (20 record constructions classified; the probe writes only the three fields; gate before any work; no sampler dimension consumed; zero diff hunks in MIS code), plank wall within 6 % of the ledger, no scope creep; one P1 — §5 still prescribed the two-probe design and 'byte-for-byte', and never named the ungated `Po` change — rewritten as built in `b7fe5f51`; F2 — the post-S3 cost protocol had not been run (row above, now filled). N — four doc P1s (a ledger hash from an amended commit; '21 checks' for 77; the louvre case listed as a refusal in the two design docs) + the missing README row — fixed `ffd8f74c`. O — every number, red-proof and seeding claim in both test headers recomputed; one comment P1 ('never a wrong record' vs the louvre residual) — fixed `b7fe5f51`. Confirming pass P (fresh): one P1 in my own fix — the two design docs' bullets lacked 'for a uniformly transformed luminary' — and the §5 cost sentence implied the transform is paid at seven sites (three compute, four copy) — fixed `63cfa34a`. **BRANCH CONVERGED**; merged to master as a fast-forward (see the commit after this one).
- Cost gate — `plank_closeup` 640×480 / 48 spp, `oidn_denoise FALSE`, `render_thread_reserve_count 0`, sequential renders, wall = 3 runs (`Total Rasterization Time`), σ² = K=8 EXR trials through `HDRVarianceTest` (mode 1, mean per-pixel σ²):

  | state | PT wall | BDPT wall | VCM wall | PT σ² | BDPT σ² | VCM σ² | BDPT mean R | VCM mean R |
  |-------|---------|-----------|----------|-------|---------|--------|-------------|------------|
  | master `185b0d5f` | 18.0 ± 0.4 | 22.7 ± 0.3 | 23.8 ± 0.1 | 2.19e-4 | 9.49e-3 | 2.49e-5 | 0.2807 (1.034× PT) | 0.1496 (0.551× PT) |
  | after S1 `089dfa76` | 19.0 ± 0.4 | 21.3 ± 0.2 | 22.6 ± 0.1 | 2.18e-4 | 9.58e-3 | 2.49e-5 | 0.2819 (1.039×) | 0.1503 (0.554×) |
  | after S3+S4 `b7fe5f51` | 19.6 ± 0.4 | 22.5 ± 1.0 | 23.6 ± 0.1 | 2.18e-4 | 9.58e-3 | 2.49e-5 | 0.2818 | 0.1503 |

  σ²·T (BDPT): 0.216 → 0.204 → 0.215; (VCM): 5.9e-4 → 5.6e-4 → 5.9e-4. Reading: S1 bought the memo win (−6 % BDPT, −5 % VCM); S3's probe gives it back on this scene (+5–6 % BDPT, 21.3 → 22.5 s; +4.6 % VCM, 22.6 → 23.6 s: the plank's two `rect_light` luminaries are NEE-sampled with the gate open, so every NEE sample pays one object-level closest-hit) — net ≈ master within noise (BDPT 22.5 vs 22.7, VCM 23.6 vs 23.8), σ² unchanged throughout (the image is the same image after S1; the plank's emitters read no signal, so S3 changes nothing here). The final-round reviewer's independent single runs (PT 20.1 s, BDPT 22.6 s) agree.

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
- **One residual that is not neutral**: a second surface of the same
  luminary inside the emitter probe's standoff band along the normal and
  within the 0.01·D acceptance tolerance is accepted with that neighbour's
  live channel (a louvred single-object fixture with blade pitch under
  ~0.5 % of its diagonal); bounded to another point of the same luminary
  at most `standoff` away, second-order except across a discontinuity.
  Refusal (neutral) only beyond the tolerance, unreachable for any
  luminary whose self-hit floor is ≤ 0.0089·D — every analytic primitive
  and every SDF at its default epsilon.
- **Two S3 sites without a red row**: BDPT's NM-hero `Le` rebuild and the
  HWSS companion `rigW` rebuild are converted and share the helper with the
  sites that do red-prove, but on `SignalEmitterRecordTest`'s scenes their
  contribution is MIS-weighted to a few percent (BDPT prices the emitter
  through the LIGHT root vertex, VCM through its own NEE record), so
  skipping them moves the suite by ≤ 5.7 % / 0 %. Guarding them needs a
  light-tracing-dominated spectral scene (a caustic through glass, or an
  emitter reachable only by t=1 splats); disclosed in the test header.
