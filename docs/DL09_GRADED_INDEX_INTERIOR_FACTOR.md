# DL-09: the interior-segment basic-radiance factor in a graded-index medium

Status: **FIXED 2026-09-28 (slice `debt-dl09`); row left unstruck for the
supervisor to strike after a fresh review.**  §1–§6 are the derivation,
committed FIRST (`349cfec8`) before any code changed; §7–§11 (implementation,
verification, cost, residuals, gate) were appended by later commits of the
same slice.  The one edit to §2–§3 after `349cfec8`: the interior camera
position S moved from z = 0.5 to z = 1/3 (so the nested-box references of §8
converge -- see there) and the (iii) numbers were restated for it.

Background: [REFRACTIVE_RADIANCE_SCALING.md](REFRACTIVE_RADIANCE_SCALING.md)
§1 (the η² factor) and §10.2 (this row's history), and the DL-09 row of
[DEBT_LEDGER.md](DEBT_LEDGER.md).

## 1. The three earlier rounds, restated

1. **Round 1 (`ae475295`, reverted).** Substituted the SPF's fresh exit-hit
   index `n_B` for `RadianceEtaScale`'s stale `before.top()` (= `n_A`, the
   value pushed at entry) and added nothing else. A through-trip then nets
   `(1/n_A)²·(n_B/1)² = (n_B/n_A)²` — 2.25× at a 1.2 → 1.8 slab: an emitter
   behind a passive, lossless slab rendered brighter than the emitter.
   The stale read was never the bug; it was silently standing in for the
   missing interior factor.
2. **Round 2, read literally** ("apply the factor on the walk, not at the
   SPF exit read" = add `(n_A/n_B)²` and leave the exit read stale). A
   through-trip then nets `(1/n_A)²·(n_A/n_B)²·(n_A/1)² = (n_A/n_B)²` —
   0.444× at the same slab, the reciprocal (mirror image) of round 1.
3. **Round 3 (the row as it stands).** The interior factor and the switch
   of the exit read to the fresh value must land together; only an
   interior gather (a contribution that never reaches the exit) can tell
   "fixed" from "unfixed"; `n_C` is undefined for UV-driven painters; and
   the DielectricSPF exit-hit Snell trace uses a different index than the
   walk tracks.

## 2. Model and notation

RISE traces **straight** segments. Inside a medium whose index varies with
position the true rays curve, and two quantities are conserved along them:
the basic radiance `L/n²` and the étendue `n² dA dΩ`. A straight-ray
tracer can honour the first exactly and cannot honour the second. The
model this slice implements is therefore:

> **Basic radiance is continuous along every straight segment and across
> every interface.** Equivalently, a graded medium is treated as a
> continuum of infinitesimal index steps along the segment, each paying
> the ordinary interface factor, with no bending.

For an eye (RADIANCE) walk this is the familiar per-event factor
`(η_before/η_after)²` telescoped over the segment:
**`(n_start/n_end)²` per segment**, walk order. `n(x)` is the graded
painter's value at world point `x`; air is `n = 1`.

Fixture used for every number in this document (the red-proof scenes of
§8 below use it): a box `G` spanning `z ∈ [0, 2]`, index
`n(z) = 1.8 − 0.6·|z − 1|` (1.2 at both faces, 1.8 at mid-plane). Top face
`A` (`z = 2`, `n_A = 1.2`), interior diffuse floor `C` (`z = 0.02`,
`n_C = 1.212`), emitter `E` a one-sided plane at `z = 1` facing down
(`n_E = 1.8`), interior camera position `S` (`z = 1/3`, `n_S = 1.4`),
exit face `B` (for through-trips) with `n_B`.

## 3. The factor tables

"Today" = master `259495db`. "After" = this slice: every time a walk
reaches a new vertex `X` while the IOR stack's top medium is a graded
(world-position) field, it multiplies `(top/n(X))²` (RADIANCE) and sets
`top ← n(X)`; `RadianceEtaScale` is unchanged and so reads the UPDATED top
at an exit — that is the exit-read switch, and it cannot be separated from
the interior factor in this design (§4).

### (i) Full through-trip: camera(air) → A → B → air → emitter in air

| event | physics | today | after |
|---|---|---|---|
| entry at A | `(1/n_A)²` | `(1/n_A)²` (after→top = `n_A` pushed fresh) | `(1/n_A)²` |
| segment A → B | `(n_A/n_B)²` | 1 (none) | `(n_A/n_B)²`, top ← `n_B` |
| exit at B | `(n_B/1)²` | `(n_A/1)²` (stale top) | `(n_B/1)²` (updated top) |
| **net** | **1** | **1** | **1** |
| round 1 (reverted) | | `(n_B/n_A)²` = 2.25 | |
| round 2 literal | | `(n_A/n_B)²` = 0.444 | |

Today and after agree on every completed through-trip. This is the
invariant `RadianceEtaScaleGradedIndexTest` pins (PT/BDPT 0.290109 vs
`L·T1·T2` = 0.289909) and it must stay green throughout.

With an interior vertex `C` on the way (camera → A → C → B → air), after
the change: `(1/n_A)²·(n_A/n_C)²·(n_C/n_B)²·(n_B/1)² = 1` — the per-segment
factors telescope, so a through-trip is 1 however many interior vertices
it has.

### (ii) Interior NEE gather at C of an emitter E inside G (camera outside)

| quantity | physics | today | after |
|---|---|---|---|
| eye throughput arriving at C | `(1/n_A)²·(n_A/n_C)² = (1/n_C)²` | `(1/n_A)²` | `(1/n_C)²` |
| NEE connection segment C → E | `(n_C/n_E)²` | 1 | `(n_C/n_E)²` |
| **NEE contribution, total** | **`(1/n_E)²`** | **`(1/n_A)²`** | **`(1/n_E)²`** |
| BSDF-sampled hit of E from C | `(1/n_C)²·(n_C/n_E)² = (1/n_E)²` | `(1/n_A)²` | `(1/n_E)²` |
| camera walk hitting E directly (A → E) | `(1/n_E)²` | `(1/n_A)²` | `(1/n_E)²` |

The eye throughput at C moves from `(1/n_A)²` to `(1/n_C)²` exactly as the
row states. The row does not name the connection segment, and it is
required: without `(n_C/n_E)²` on the NEE arm, NEE would carry
`(1/n_C)²` while the BSDF-sampled hit of the same emitter carries
`(1/n_E)²`, and `w_nee·f_nee + w_bsdf·f_bsdf` would be a mixture of two
different integrands. With it, both strategies carry the same number and
MIS is untouched (the factor is a throughput term, never a density).

Fixture (camera outside, ortho along −z, floor ρ, emitter `L_e`):
physics pixel `= T_A·ρ·L_e/n_E²`; today `= T_A·ρ·L_e/n_A²`. Today/physics
`= (n_E/n_A)² = (1.8/1.2)² = 2.25`.

### (iii) Walk seeded INSIDE G (camera at S)

`IORStackSeeding::SeedFromPoint` fires axis-aligned probes from S and
pushes, for each containing object, the index its probe's FIRST SURFACE
HIT reported (`GetSpecularInfo` at a boundary point, `n_P`), not `n(S)`.
In the fixture the `+z` probe's first hit on G is the top face: `n_P = 1.2`.

| quantity | physics | today | after (seed at S + interior factor) |
|---|---|---|---|
| stack top at the camera | `n_S` | `n_P` = 1.2 | `n_S` = 1.4 |
| eye throughput at C | `(n_S/n_C)²` | 1 | `(n_S/n_C)²` |
| NEE at C of E, total | `(n_S/n_E)²` | 1 | `(n_S/n_E)²` |
| through-trip S → B → air | `(n_S/n_B)²·(n_B/1)² = n_S²` | `n_P²` | `n_S²` |

Fixture: physics pixel `= ρ·L_e·(1.4/1.8)² = 0.605·ρ·L_e`; today
`= ρ·L_e`, ratio 1.653. The two changes are again both needed: the
interior factor with the stale seed gives `(n_P/n_E)² = 0.444` (0.735×
physics); the seed fix alone changes nothing (no factor is ever applied
inside, so the seeded top is never read by an interior gather).

### (iv) Light (IMPORTANCE) subpaths and connections — an extension the row does not state

At a real interface an importance walk needs no explicit factor: the
refraction's direction mapping has a Jacobian that spreads or compresses
the sampled rays, and the light-tracing estimator's density picks up
`(n_i/n_t)²` from that mapping implicitly (§1 of
REFRACTIVE_RADIANCE_SCALING.md). A **straight** graded segment has no
direction mapping and therefore no implicit factor. The path
contribution in the model of §2 carries, for every graded segment, the
factor `(n_cameraside/n_lightside)²` regardless of which walk built the
segment, so:

| segment built by | factor, in that walk's own order |
|---|---|
| eye walk, `x_i → x_{i+1}` | `(n_start/n_end)²` |
| light walk, `y_j → y_{j+1}` | `(n_end/n_start)²` |
| connection eye vertex `e` ↔ light vertex `l` (incl. NEE, BDPT s=1, t=1 splat) | `(n_e/n_l)²` |

Check, camera inside G at S, BDPT light tracing: light walk E → C′
`(n_C′/n_E)²`, splat C′ → S `(n_S/n_C′)²`, product `(n_S/n_E)²` = PT's
(iii). Today both are 1 — consistent with each other and both wrong — so
a PT-vs-BDPT comparison cannot red-prove this; it pins a HALF fix (eye
factor without the light/connection factor), which would make them
disagree by `(n_S/n_E)²`.

A VCM merge joins an eye vertex and a light vertex at the same point: no
segment, no factor; the two throughputs already telescope to
`(n_cam/n_E)²`.

## 4. Why the exit-read switch comes for free here, and why that matters

The factor is implemented as an UPDATE of the IOR stack's top value
(`top ← n(X)`) at every vertex reached inside a graded medium, paid for by
`(top/n(X))²`. The exit crossing's `RadianceEtaScale(before, after)` then
reads `before.top() = n_B` because the walk updated it on arrival at B.
There is no code path that applies the interior factor without also
switching the exit read, or vice versa: both rounds' failure modes are
unreachable by construction. And every factor is `(top/n_new)²` with
`top` whatever the stack holds, so a vertex the update misses (a site out
of scope, §8) does NOT break any later factor — the next update telescopes
over it; only a gather AT the missed vertex keeps today's error.

Constant-index media: the painter is not a world-position field, the
material reports no graded field, the helper returns before evaluating
anything, and the factor is exactly 1 with no floating-point operation —
bit-identical renders.

## 5. What this does NOT fix: ray bending (straight-ray geometry)

The model gets the RADIANCE along each straight segment right. It does not
bend rays, so where bending changes WHICH points a set of directions
reaches, the answer is still approximate — before and after this slice.
Configurations where the straight model is exact:

- radiance along a single ray parallel to `∇n` (the camera ray of §3(ii)
  and (iii) with an orthographic camera looking along the gradient);
- irradiance at C from a strata-parallel emitter plane that covers the
  hemisphere, when `n` is non-decreasing from C toward the emitter (curved
  rays then also reach the plane from every upward direction, so the
  solid-angle set is the same; the fixture is built this way);
- anything in a constant-index region.

Where it is not: a SMALL emitter on the gradient axis at distance `d`
from C. Curved rays collimate the pencil; with `D = ∫dz/n`, physics
`E_C ∝ L_e/(n_E²·D²)` against the model's `L_e·(n_C/n_E)²/d²`, so the
model/physics ratio is `(n_C·⟨1/n⟩)²` (0.658 for `n` 1.2 → 1.8 linear),
while today's ratio is `(n_E·⟨1/n⟩)²·(n_C/n_A)²` (1.48 at `n_A = n_C`).
Both are first-order in `Δn/n`; neither is a radiometric statement. This
is the same approximation as the exit-Snell item below and is filed with
it.

**The DielectricSPF exit-hit Snell trace (the row's "second
inconsistency").** After this slice the walk's tracked index at an exit
hit IS the fresh `n_B` the SPF Snell-tests against, so the inconsistency
AS THE ROW DEFINES IT (SPF index ≠ tracked index) is gone for the walks
this slice covers — and the exit event is radiometrically self-consistent
(its explicit factor `(n_B/1)²` matches the Jacobian of the Snell map it
actually traces). It is still GEOMETRICALLY wrong for a stratified slab:
the true invariant is `n·sinθ` along the curved ray, so a ray entering
from air at `θ_0` must leave a parallel-faced slab at `θ_0`; the straight
segment carries `sinθ_A = sinθ_0/n_A` to the exit and Snell at `n_B` gives
`sinθ_out = (n_B/n_A)·sinθ_0`. At 1.2 → 1.8: 30° exits at 48.6°, and every
entry angle above 41.8° is TOTALLY INTERNALLY REFLECTED at the exit face
(55.6 % of cosine-weighted incident light) where physics transmits all of
it. Snell-testing at the carried `n_A` would be exact for a stratified
slab and wrong for any other gradient; the general answer needs curved
rays. That is a ray-geometry change, not a radiance-accounting one, it is
unchanged by this slice (directions are untouched), and it is deferred
with these numbers as **DL-293**.

## 6. Verdict on the round-3 analysis

Correct in substance, and not re-derived here as wrong: the missing factor
is real, the pairing is mandatory, the interior-gather error is
`(n_C/n_A)²` on the eye throughput, the seeded-inside error is real,
`n_C` is undefined for UV-driven painters. Two places where round 3 is
INCOMPLETE (both anticipated by the slice brief, neither contradicted by
the row): the NEE / connection segment needs its own factor (§3 ii), and
importance walks need an explicit factor on graded segments because a
straight segment has no Jacobian to supply it implicitly (§3 iv). One
place where the recipe's wording is satisfied vacuously and should not be
mistaken for physics: the exit Snell trace becomes "consistent with the
tracked medium" automatically, and remains geometrically approximate
(§5).

## 7. Implementation (appended after the derivation)

| piece | where | what |
|---|---|---|
| where `n` is defined | `IScalarPainter::IsWorldPositionField()` (default false); `ExpressionScalarPainter` overrides via `IsWorldPositionProgram` | a SCALAR-typed program that reads `P` and none of `u`,`v`,`Po`,`N`,`fw`,`fwo`,`curv`,`curvR`, no surface signal, no `sample()` — resolved from the compiled context-variable mask |
| which media are graded | `IMaterial::GetGradedIORField()` (default null); `DielectricMaterial`, `PerfectRefractorMaterial` | the `ior` painter when it is a world-position field, queried live |
| the factor | `Utilities/GradedIndexMedium.h` (new, header-only) | `Advance(stack, x, mode, scale)`: `(top/n(x))²` radiance / `(n(x)/top)²` importance, then `top ← n(x)` (`IORStack::SetTopIOR`, new); `RecordAt` (seed point, no factor); `RecordVertex`; `ConnectionScale`, `ConnectionScaleToPoint`, `ConnectionScaleFromPoint` |
| cost gate | `GradedIndexDemand` in the same header, registered by `ExpressionScalarPainter` | while no world-position painter exists in the process every query returns after one relaxed atomic load |
| PT | `PathTracingIntegrator.cpp` surface-hit processing (Pel/NM template, HWSS twin: every lane) | `Advance` before emission/NEE/scatter; `throughput` and RR `importance` scaled |
| PT NEE | `LightSampler::EvaluateDirectLighting{,NM}` new trailing `pGradedIndexStack` | delta-position light and mesh-luminary arms (RGB+NM) multiply `(top/n(light point))²`, AFTER the optimal-MIS training accumulate (the BSDF side's trained `bsdfTimesCos` excludes the segment factor too); env / directional / ambient arms: 1 |
| PT volume NEE | `MediumTransport::EvaluateInScattering{,NM}` new trailing `pGradedIndexStack`; 4 PT call sites | medium vertices do not Advance; the NEE factor from the stack top telescopes |
| BDPT eye / light | `GenerateEyeSubpathImpl` / `GenerateLightSubpathImpl` | `Advance` (radiance / importance) at every surface vertex, `beta` and HWSS `hwssBetaNM` scaled, `ambientIOR` re-stamped; every vertex (camera, light, surface, medium) records `pGradedMedium`/`gradedIOR` (new `BDPTVertex` fields) |
| BDPT / MLT connections | `ConnectAndEvaluateImpl` → wrapper around the renamed `ConnectAndEvaluateImplCore` | `(n_eye/n_light)²` on every valid s ≥ 1, t ≥ 1 result — one choke point for BDPT, MLT, the complete-path selector and both tags |
| VCM | `EvaluateNEEImpl`, `SplatLightSubpathToCameraImpl`, `EvaluateInteriorConnectionsImpl` | the same connection factor; merges build no segment |
| seeding | `IORStackSeeding::SeedFromPoint` | `RecordAt(stack, pos)` after the pushes |
| seeding (found building the reference) | same file | the per-probe entry table was 8 entries (`kMaxNestingDepth`) and the probe records every DISTINCT trackable object it MEETS, containing or not: 12 non-containing boxes in front of the seed filled it and the 5-6 containing ones behind them were dropped, so a camera inside 6 of 17 nested CONSTANT-index boxes seeded as bare air and rendered 0.38× its closed form.  Table now sized to the step cap (`kMaxProbeEntries = kMaxProbeSteps = 32`; one step adds at most one entry).  This is a constant-index bug, independent of DL-09. |

`DielectricSPF` / `PerfectRefractorSPF` are untouched: no direction changes
anywhere, only throughput factors and the value held in the stack top.

## 8. Verification

**Scenes.** `scenes/Tests/Materials/graded_index_interior_gather.RISEscene`
(camera outside, ortho down the gradient) and
`graded_index_seeded_inside.RISEscene` (camera at `z = 1/3`, `n_S = 1.4`), both
registered in the CST golden.  The nested-box REFERENCE replaces the graded box
by `K+1` nested constant-index boxes (box `k` spans `z ∈ [kd, 2−kd]`,
`d = 1/(K+½)`, index = the tent at its band centre); `K = 3k+1` makes the
seeded camera a band centre for every `K`.  The references use only
debt-30's per-interface factors, so they are physics by construction and
converge as `K` grows (their residual is the `O(1/K)` Fresnel loss of the
interfaces).

**Protocol.** Two separately built `GradedIndexInteriorFactorTest`
binaries (library at `2e4ab987`, i.e. master, vs this slice), run
interleaved 4 times each with different seed bases.  Every render is
32×32 split into 16 tiles; on a uniformly lit floor the tiles are
independent replicas (disjoint per-pixel Sobol seeds) and `sd` below is the
across-tile spread of one render; the run-to-run sd of the mean was ≤ 2e-6
for every PT/BDPT row (renders are nearly deterministic) and ≤ 2.2e-4 for
the spectral rows.  "ratio" = mean / closed form.

| row | quantity | pre-DL-09 (master) | post-DL-09 |
|---|---|---|---|
| A | PT graded (camera outside, big emitter) | 0.109498 ± 0.000161, ratio **2.2504** | 0.048666 ± 0.000072, ratio **1.0002** |
| A | PT nested boxes K = 4 / 7 / 10 / 16 | 0.9785 / 0.9841 / 0.9875 / 0.9926 | identical (bit-identical, see below) |
| A | PT graded / K=16 reference | **2.267** | **1.0077** |
| A | BDPT / VCM graded | 2.2506 / 2.2505 | 1.0002 / 1.0002 |
| B | PT graded (camera inside, seeded) | 0.159025 ± 0.000302, ratio **1.6537** | 0.096200 ± 0.000183, ratio **1.0004** |
| B | PT nested boxes K = 4 / 7 / 10 / 16 | 0.9854 / 0.9898 / **0.7549 / 0.3801** (seeding table overflow) | 0.9854 / 0.9898 / 0.9924 / 0.9944 |
| B | PT graded / K=16 reference | 4.350 | **1.0060** |
| B | BDPT / VCM graded | 1.6539 / 1.6532 | 1.0005 / 1.0001 |
| C | small emitter (NEE / s=1 carry the weight): PT / BDPT / VCM | 2.2506 / 2.2506 / 2.1608 | 1.0003 / 1.0003 / 0.9617 |
| D | pinhole inside, small emitter: BDPT/PT, VCM/PT | 1.0001, 0.9958 (consistently wrong) | 1.0000, 0.9961 |
| G | spectral scene A: PT hero / PT HWSS / BDPT HWSS (`num_wavelengths 160`) | 2.2505 / 2.2515 / 2.2511 | 0.9960 / 1.0006 / 1.0005 |
| G | spectral pinhole: BDPT HWSS / PT HWSS | 1.0003 | 1.0003 |
| H | scattering medium in the graded box: BDPT/PT, VCM/PT | 0.9961, 0.9786 | 0.9955, 0.9781 |
| E | `SeedFromPoint` at z = 0.3 / 0.5 / 1.4 records | 1.2 / 1.2 / 1.2 | 1.38 / 1.5 / 1.56 = n(z) |
| E | seed inside 6 of 17 nested boxes | 1.0 (air) | 1.4 (innermost box) |
| F | uniform-ior 1.5 control | 1.0000 | 1.0000 |
| — | `GradedIndexInteriorFactorTest` | **34 passed / 21 failed** (×4 runs) | **55 / 0** (×4 runs) |
| — | `RadianceEtaScaleGradedIndexTest` through-slab | 0.290109 (1.00069), 13/0 | 0.290126 (1.00075), 13/0 |

VCM's row-C offset (0.960 → 0.962 of the model) is the SAME before and after
— it is present with no graded factor anywhere — so it is not DL-09's; it is
recorded, not attributed.  Row D and H are not red on master because every
integrator there was consistently wrong; they exist to catch an INCOMPLETE
fix, which the mutation table shows they do.

**Mutations of the shipped fix** (edit, rebuild, run, `git checkout HEAD --
src/Library`):

| id | mutation | caught by |
|---|---|---|
| M1 | round-2 literal: interior factor paid, stack top NOT refreshed (stale exit read) | through-slab test **0.444645** × physics; this suite 7 failures (rows B, C, D) |
| M2 | round-1: stack top refreshed (fresh exit read), NO interior factor | through-slab test **2.25168** ×; this suite 12 failures |
| M3 | importance-walk factor dropped | row C VCM, row D VCM/PT **1.1932** |
| M4 | BDPT connection factor dropped | rows A/B/C BDPT, row D BDPT/PT **2.0756** |
| M5 | PT NEE segment factor dropped | rows A/B/C PT, row D BDPT/PT **0.4807** |
| M6 | VCM connection/splat factors dropped | rows A/B/C VCM, row D VCM/PT **1.8184** |
| M7 | PT volume NEE without the stack | row H BDPT/PT **0.7269** |
| M8 | BDPT/VCM medium vertices do not record the tracked index | row H BDPT/PT **1.3876** |

M3 moves BDPT/PT only to 1.0057 on row D (BDPT's light-tracing strategies
carry little weight there); VCM, which shares BDPT's light generator, catches
it.

**Constant-index media are bit-identical.**  RISE renders are not
deterministic across threads (block order is shuffled from
`std::random_device`), so the bit comparison was run single-threaded
(`force_number_of_threads 1`), where each binary reproduces its own hashes.
Every constant-index render in the suite hashes IDENTICALLY pre and post:
nested boxes K = 4, 7, 10, 16 (camera outside), K = 4, 7 (camera inside),
and the uniform-1.5 control (e.g. `8a615be83043a7a0` both).  The two that
differ — camera inside K = 10 and 16 — are exactly the seeding-table fix.
Multi-threaded, the constant rows show the same small set of run-to-run
hashes in both builds.  On a real scene
(`cornellbox_bdpt_materials_pt`, `dielectrics_changing_ior`) the means agree
within run-to-run noise (below).

## 9. Cost

`rise` CLI, two separately built binaries, interleaved, user CPU:

| scene | pre | post | Δ |
|---|---|---|---|
| `dielectrics_changing_ior` PT 640×240, 256 spp, n = 5 | 95.99 ± 2.50 s | 96.31 ± 3.23 s | **+0.34 %** (paired t = 0.51) |
| `cornellbox_bdpt_materials_pt` PT 512², 32 spp, n = 4 | 92.30 ± 0.26 s | 92.52 ± 0.70 s | **+0.24 %** (t = 0.96) |

Without the `GradedIndexDemand` gate the dielectric scene read **+1.95 %**
(t = 5.53, n = 4): three virtual calls per vertex inside any object.  With it
the factor is free where `ior` is constant.

## 10. Residuals

- **DL-292 — coverage.**  Walks that do not Advance keep the pre-DL-09
  accounting, which nets correctly on every completed through-trip and is
  internally consistent, but is still wrong by `(n_C/n_A)²` at an interior
  gather: the legacy shader-op chain (`pixelpel` and friends — its
  `DirectLightingShaderOp` NEE passes no graded stack, on purpose, so its own
  NEE/BSDF partition stays consistent), the photon tracers and photon-map
  gathers, SMS manifold chains, `RayCaster`'s own volume walk (its NEE passes
  no stack; a PT continuation re-entering from it DOES Advance, so a gather at
  such a medium vertex is priced `(1/n_top)²` on its NEE arm vs `(1/n_E)²` on
  its phase-sampled arm), PT's BSSRDF-entry NEE (no stack), and a
  transparent-shadow NEE ray that crosses the graded boundary (factor skipped;
  the graded segment up to the boundary is unpriced).  Forms not recognised as
  world-position fields: per-channel (`vec3`) expression programs, the
  `Scaled`/`Multiply`/`Add` composites (they do not forward
  `IsWorldPositionField`), and every UV/normal-driven painter (`n` undefined
  there).  Stack pushers other than the two refractors (`TranslucentSPF`, SSS)
  report no graded field.  A connection that leaves a graded object through
  its OWN non-delta surface (a rough graded dielectric) prices only when both
  endpoints recorded the same medium.  BDPT's t==1 factor uses the camera
  vertex's recorded index, not the separately sampled lens point (thin-lens,
  camera inside, graded: lens-scale difference).
- **DL-293 — ray bending (§5).**  Straight segments: the exit Snell trace uses
  the local index, which is now CONSISTENT with the tracked medium (so the
  row's "second inconsistency" as written is gone for PT/BDPT/VCM/MLT) but is
  geometrically wrong for a stratified slab (1.2 → 1.8: a 30° entry exits at
  48.6°; entries above 41.8° TIR at the exit face, 55.6 % of cosine-weighted
  incident light, where physics transmits it all), and solid angles do not
  collimate (a small on-axis emitter reads `(n_C⟨1/n⟩)²` of physics with the
  fix, 0.658 at 1.2 → 1.8).  Needs curved rays; deferred with these numbers.
- **Found, not DL-09's:** `RefractiveRadianceScalingTest` reads **40 passed / 1
  failed on master `259495db`** as well as on this branch (row C "BDPT within
  8 % of PT": BDPT/PT 0.907–0.917 in four runs of each build, constant `ior`
  1.33 — the graded code path is never entered there).  The brief's "38/0" is
  stale: the suite has 41 checks and this row has regressed on master since
  it last read 41/0.

## 11. Gate (clean rebuild, 0 warnings: library and all 47 test targets built)

`GradedIndexInteriorFactorTest` 55/0 · `RadianceEtaScaleGradedIndexTest` 13/0
· `RefractiveRadianceScalingTest` 40/1 (pre-existing, identical on master) ·
`TranslucentIORStackTest` ALL PASSED · `TransmissionPushGateTest` 416/0 ·
`DielectricGrazingFresnelTest` 338/0 · `EnvLightBalanceTest` 123/0 (topology J
included) · `BDPTStrategyBalanceTest` 170/0 · `VCMStrategyBalanceTest` 74/0 ·
`MediumInsideOutsideInvariantTest` 18/0 · `CstDeriveGoldenTest` 456 MATCH / 0
DRIFT · `SourceHygieneTest` 167/0 · `SSSRadianceScalingTest` 576220/0 ·
`IORStackSeedingRegressionTest` 15/0 · `TranslucentInitialContainmentTest`
43/0 · `TranslucentDoubleSidedTest` 53/0 · `GeomNormalOrientationSitesTest`
72/0 · `VolumeEnvFurnaceTest` 32/0 · `PTGuidingMISPartitionTest` 101/0 ·
`OptimalMISTrainingSitesTest` 111/0 · `RayCasterEnvEscapeMISTest` 91/0 ·
`IScalarPainterTest` 134/0 · `TextureExpressionVMTest` 965/0 · `LightBVHTest`
20/0.
