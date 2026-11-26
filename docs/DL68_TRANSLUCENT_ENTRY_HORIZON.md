# DL-68 — TranslucentSPF's stack-transitioning Phong lobes and the geometric horizon

**Status: CLOSED 2026-09-14** (`903b8878`, red-proof `c9d751e4`).
Regression: `tests/TranslucentEntryHorizonTest.cpp` — 212 checks, 0 failures
at original closure; **251 checks, 0 failures after review round 2**
(2026-09-14, same day: P2-b sub-tests 7/8 + P3-b sub-test 9, see §6 and
the Counts note in `docs/DEBT_LEDGER.md`'s DL-68 row).

Companion reading: [DL01_TRANSLUCENT_EXIT_WEIGHT.md](DL01_TRANSLUCENT_EXIT_WEIGHT.md),
[DL02_TRANSLUCENT_EXIT_DENSITY.md](DL02_TRANSLUCENT_EXIT_DENSITY.md),
[DL03_GUIDED_IOR_CONTINUATION.md](DL03_GUIDED_IOR_CONTINUATION.md) (DL-45, the
exit-pop twin), [DL70_GEOM_NORMAL_ORIENTATION_SITES.md](DL70_GEOM_NORMAL_ORIENTATION_SITES.md)
(`UnflippedGeomNormal()` / `HasTrueGeomSide()`).

---

## 1. The defect

`TranslucentSPF::Scatter` / `ScatterNM` emit two Phong `cos^N` lobes whose
entire purpose is a **side-membership claim about the object**, and neither
checked that the direction it sampled actually satisfies that claim:

| lobe | branch | state claim | stack action |
|---|---|---|---|
| `trans` (entering transmission) | `bEntering` | "the continuation crossed to the far side" | **push** |
| `trans` (interior backscatter) | exit, `scattering > 0` | "the continuation stayed inside" | **deliberately none** |

Both were sampled about the shading normal (`myonb.FlipW()` / the
conditionally-flipped exit frame) with no geometric gate at all. The shading
normal is not the surface: a bump map, a normal map, `GlintModifier` (up to
60° of tilt) or simply a smooth-shaded mesh's interpolated normal all move it
off the geometry. Under tilt a real fraction of each lobe therefore travels
the wrong way while the stack records the other:

* an entry ray that leaves the way it came in **with the object pushed** — a
  later hit on that object is then read as an *exit*, pops a level that was
  never legitimately entered, and DL-45's exit machinery runs on a lie;
* a "backscatter" ray that **leaves the solid without the matching pop** — the
  same misclassification with the signs swapped.

This is the entry-push mirror of the exit-pop defect DL-45 closed
(`14b06223`), at a **distinct site**: DL-45 fixed the diffuse exit
re-emission's gate/renormalization and left both Phong lobes explicitly
exempt (the pre-DL-68 comment in `Scatter()` said so in as many words).

**Render-level effect (P3-c, review round 2): honestly, below MC noise on
every fixture tested.** This is a STATE-MISCLASSIFICATION fix, not
primarily a radiance-magnitude one, and its render-visible consequence is
correspondingly indirect: a mesh-shape convex object's wrong-side entry ray
re-emits diffusely from the point where it entered (the near side of the
surface, since the mislabeled "entry" ray never actually crossed anything),
while the corrected ray re-emits from wherever the corrected transmission
direction actually lands (typically the far side). On a mesh whose local
curvature is mild relative to the mean free path, both points see nearly
the same incident illumination, so the two give nearly the same expected
outgoing radiance -- the bug is a bookkeeping error whose downstream
symptom (a LATER hit on the same object misclassified as an exit that
never legitimately entered) requires a specific multi-bounce configuration
to become visible at all, not a per-pixel radiance error at the site of
the mistake itself. Review-round-2 measurement: four production PT
fixtures (tilted translucent spheres/meshes, `oidn_denoise FALSE`,
`pixel_filter box`) where the clip fires on 100% of entry lobes (heavy
tilt) showed mean deltas <= 0.1% pre- vs post-fix, per-pixel
indistinguishable from Monte Carlo noise at the sample counts tested.
**Visibility, stated honestly: a state-misclassification fix whose
render-level effect was below MC noise on every fixture reviewed this
round** -- not the blanket "user-visible" the row's own original filing
implied; see the `docs/DEBT_LEDGER.md` row's Visibility column, updated
to match.

### Measured (pre-fix)

`tests/TranslucentEntryHorizonTest.cpp` against the unfixed library at
`12027967` — **212 checks / 83 failures**. Fraction of **pushed** entry rays
that are *not* geometrically into the solid, closed analytic fixture,
`RandomNumberGenerator(777)`, 8192 trials per row:

| pipe | tilt 0 | 15 | 30 | 45 | 60 | 75 | 89 |
|---|---|---|---|---|---|---|---|
| RGB `N=1` | 0 | 142 | 548 | 1208 | **2039** | 3035 | 4049 |
| RGB `N=20` | 0 | 0 | 0 | 1 | 60 | 943 | 3856 |
| NM `N=1` | 0 | 142 | 548 | 1208 | 2039 | 3035 | 4049 |
| NM `N=20` | 0 | 0 | 0 | 1 | 60 | 943 | 3856 |
| RGB `N=1/7/30` (per-channel, /24576) | 0 | 142 | 553 | 1297 | 2626 | 5550 | 11805 |

The `N=1` row is exactly `(1 - cos(tilt))/2` — 0.0669 at 30°, 0.2489 at 60°,
0.4943 at 89° — which independently reproduces the ledger row's own ad-hoc
2032/8192 at 60°. Larger `N` narrows the lobe and pushes the onset to higher
tilt without removing it.

Other fixtures, pre-fix:

* **flat double-sided sheet**, back-face entry: 626/4096 at 45° tilt, 1051/4096
  at 60° (RGB and NM alike); 0/4096 at tilt 0 — a **control**, see §3.
* **smooth-shaded mesh**, oblique entry, interpolated-vs-face normal deviation
  2° / 8° / 20°: 1 / 33 / 216 per 8192. Small, but this needs **no** bump map,
  normal map or glint modifier — it is every smooth-shaded translucent mesh in
  the corpus.
* **interior backscatter** (the sibling), closed fixture, tilt sweep:
  73 / 271 / 605 / 1069 / 1564 / 2049 rays per 4096 escaping without a pop at
  15 / 30 / 45 / 60 / 75 / 89° (0 at tilt 0).
* **chi-squared** of 400k sampled directions against the density the fix
  reports: `chi2/dof` = 46.8 (30°, N=1), 272.2 (60°, N=1), 75.1 (60°, N=7) --
  and, for completeness (P3-d, review round 2 -- the original write-up
  elided this cell), 0.993 (30°, N=7): NOT a red signal, because a narrow
  lobe at a mild tilt puts almost none of its (unclipped, pre-fix) mass
  near the horizon bins this diagnostic has to exclude; see §3 for why
  that is a diagnostic blind spot at this one cell, not evidence of
  correctness there.

---

## 2. The geometric reference — and why it is *not* DL-45's `geomNRaw`

A transmission lobe's defining property is that it continues **through** the
surface: it must leave on the opposite side from the one the incoming ray
arrived on. That is the **ray-anchored** `geomN` this file already computes
for the entry front (reflection) lobe —

```
geomN = Dot(geomNRaw, ri.ray.Dir()) < 0 ? geomNRaw : -geomNRaw   // opposes the ray
```

— and the transmission half-space is its **exact complement**, `-geomN`. The
interior backscatter's is `+geomN` (the interior ray arrived from the inside,
and that is where the backscatter must stay). The two lobes are therefore the
same construction with opposite signs, and the entry reflection lobe's
existing gate and the entry transmission's new one partition the sphere.

DL-45's exit re-emission deliberately uses the **unflipped** `geomNRaw`
instead. That is not an inconsistency; the two questions differ:

* On a **closed** object the two references agree for a genuine entry — the
  ray travels inward, so `Dot(geomNRaw, dir) < 0` and `geomN == geomNRaw`.
  Every fixture in §1's first table has `-geomN == -geomNRaw`, which is why
  the ledger row's prescribed test `dot(dir, -trueOutward) > 0` is the right
  assertion there.
* On an **open double-sided sheet** struck from its back face they disagree,
  and only the ray-anchored one is defensible. Translucent is the material
  authors put on open sheets (DL-46 review round 3(c); DL-76 records the
  residual open-surface containment case). The sheet has no "inside", so
  `-geomNRaw` is not "into the solid" — it is *back at whatever the ray came
  from*. Sub-test 2 pins this with an explicit assertion that the half-space
  is `+Z` ("through the sheet") on a fixture whose true outward is `+Z` and
  whose ray travels `+Z`.
* DL-45 needs the opposite treatment for the symmetric reason: its ray is
  **already travelling outward**, so the ray-anchored flip lands on the inward
  direction (that is exactly the trap its own comment warns about).

The lobe **axis** is oriented into the half-space first
(`OrientedLobeAxis(n, halfSpace)`), so `cos(phi) = |Dot(n, halfSpace)| >= 0`
and the valid arc is never empty. This is the same lesson as DL-45 review
round 3(a): on a double-sided mesh `ri.onb.w()` is built from the **flipped**
`vNormal`, so an unoriented axis can sit in the wrong hemisphere and collapse
the lobe to nothing (silent total energy loss) instead of clipping it
slightly.

`ri.vGeomNormal` itself is recovered through `HasTrueGeomSide()` /
`UnflippedGeomNormal()` (DL-70) before any of this, exactly as the
pre-existing entry/exit gates in this file already do; a `HairGeometry` hit
(`bGeomNormalRayDerived`) falls back to the shading normal, making both gates
no-ops there.

---

## 3. The sampler — exact clipped Phong in two draws, for every `N`

Two constructions were unavailable:

* **Rejection.** `ISampler` may have a fixed per-bounce dimension budget
  (`ISampler::HasFixedDimensionBudget()`, `ISampler.h`); a variable draw count
  shifts every later `Get1D()` in the bounce's phase in a tilt-correlated way.
  `TranslucentSamplerDimensionCountTest` exists precisely because DL-45's
  first attempt did this — but at this row's original closure its own rows
  all built `MakeInsideStack` fixtures with `scattering=0`, which isolates
  only the exit branch's diffuse re-emission. It never actually counted a
  call through the entering `trans` lobe, its per-channel-`N` branch, or the
  exit branch's own interior backscatter `trans` lobe — precisely the lobes
  this row changed — so the suite's checks were not, at closure time,
  evidence that *this* fix respects the fixed-dimension-budget invariant
  (review P2-a). The invariant did hold regardless (both new and old code
  draw the lobe's two canonical numbers unconditionally, so the draw count
  cannot depend on geometry or which channel branch fires), and the suite
  now has dedicated rows proving it directly: an ENTERING-stack fixture
  (isotropic, per-channel-`N`, NM) and a `scattering > 0` exit fixture (same
  three pipes), each swept over tilts `{0,45,80}` and asserting `min==max`
  draw count (4, since both the entry front/reflection lobe and the entering
  `trans` lobe — or the backscatter `trans` lobe and the diffuse exit lobe —
  are active on these fixtures). 65589 checks post-P2-a (was 65544).
* **DL-45's Malley disk remap.** Its disk-projection equivalence is specific
  to the **plain cosine** (`N = 1`) case. These lobes carry an
  author-controlled `N` (`translucent_material`'s `N` painter, per-channel in
  the RGB split branch). The ledger row records this as the open part of the
  derivation, and suggests either a closed-form clipped-`cos^N` valid fraction
  or rejection. Neither is needed:

**The clipped Phong density is constant in azimuth at fixed `theta`** (the
lobe is azimuthally symmetric; the clip is a plane through the origin). So
draw `theta` from the *unclipped* marginal exactly as before,
`cos(theta) = u1^(1/(N+1))`, and draw the azimuth **uniformly on the valid
arc at that theta**. In the frame `(u, v, axis)` whose `+u` carries the clip
normal's tangential part — so `clipN = cos(phi)·axis + sin(phi)·u` — the
constraint `Dot(w, clipN) > 0` reads

```
sin(theta)cos(psi)sin(phi) + cos(theta)cos(phi) > 0
  <=>  cos(psi) > -cot(theta)cot(phi)
```

an arc centred on `+u` of half-width

```
halfArc(theta) = PI                           if cot(theta)cot(phi) >= 1
               = acos(-cot(theta)cot(phi))    otherwise
```

(implemented as the ratio comparison `cos(theta)cos(phi) < sin(theta)sin(phi)`
so `theta -> 0` needs no special case). Sampling is
`psi = (2·u2 - 1)·halfArc`, and the resulting solid-angle density is closed
form:

```
q(w) = (N + 1) · cos^N(theta) / (2 · halfArc(theta))
```

Properties:

* **Exact**, and exactly **2 canonical draws** every call regardless of
  geometry or `N`.
* Collapses to the pre-existing `(N+1)cos^N(theta)/(2·PI)` whenever the clip
  is inactive. The implementation detects that case (`clipN` parallel to the
  axis) and reproduces the pre-DL-68 `GeometricUtilities::Perturb` draw
  **bit-for-bit**, so every untilted surface — analytic primitives,
  flat-shaded faces, the overwhelming majority of production hits — is
  unchanged. `TranslucentSpectralParityTest`'s deterministic inverse-CDF
  identities (which pin the untilted case) are untouched.
* Because `cos(phi) >= 0` after orientation, `halfArc >= PI/2` always: the
  valid region never vanishes, and there is no analogue of DL-45's
  `kExitVanishThreshold` to reach.
* **The clipped-away energy is renormalized into the valid region** (DL-45's
  choice), not dropped: every trial still emits exactly one lobe, and the
  reported density is the density of that renormalized procedure.
* **P3-f (review round 2):** the parameterization has one measure-zero
  special case worth naming rather than leaving implicit: `u2 -> 0` and
  `u2 -> 1` both land `psi` exactly on the arc boundary
  `Dot(w,clipN) = 0` (the tangent plane to the clip). That is an ordinary,
  valid sample of the closed interval `[-halfArc,halfArc]` carrying the
  same density `q` above, not a degenerate or excluded case -- unlike the
  horizon-straddling BINS the regression's chi-squared excludes (those are
  a quadrature artifact of finite bin width, not a property of the sampler
  itself). Separately, `theta -> 0` (`u1 -> 1`) makes `uAxis`'s specific
  direction immaterial since `sin(theta) -> 0` multiplies it out of
  `outDir` -- the ordinary pole of any spherical parameterization.

### Difference from DL-45's shape, stated plainly

`q` renormalizes **per theta ring**, not globally — it is **not** DL-45's
`cos(theta)/pi / P(valid)`. The theta marginal is deliberately left at the
unclipped one, and that is exactly what makes the azimuth conditional
uniform and therefore exactly invertible with the second canonical number.
Both are exact samplers of their own reported density; this one is the one
that extends to `N != 1`. Verified two ways in the regression:

* a 400×800 quadrature of `q` over the sphere, implemented independently in
  the test, recovers `1.00000`–`1.00013` at every tilt 0…89° × `N ∈ {1,7,30}`;
* a chi-squared of 400k **sampled** directions against `q` (horizon-straddling
  bins excluded) reads `chi2/dof` = 1.013 / 0.800 / 0.867 / 0.875 post-fix
  (46.8 / 0.993 / 272.2 / 75.1 pre-fix, for (30°,N=1) / (30°,N=7) / (60°,N=1)
  / (60°,N=7) respectively -- re-measured against `12027967`, review round
  2, P3-d: the original write-up elided the (30°,N=7) figure as `—`).
  **The (30°,N=7) cell is NOT a red signal pre-fix** -- unlike the other
  three, which fail this suite's own `< 1.6` gate outright. At `N=7` the
  lobe is narrow enough, and 30° tilt mild enough, that almost none of the
  UNCLIPPED pre-fix lobe's mass reaches the horizon at all, so the
  horizon-straddling-bin exclusion (needed because a bin the horizon cuts
  through has a discontinuous integrand, see sub-test 6's own comment)
  removes essentially the only bins where pre-fix and post-fix differ,
  leaving a chi-squared over bins that were never wrong. This is a real
  limitation of THIS diagnostic at this particular (tilt, N) cell, not
  evidence the pre-fix sampler was correct there: the defect is a
  geometric fact about which HALF-SPACE a sampled direction lands in, and
  a narrow, mildly-tilted lobe simply puts little of its mass near that
  boundary in the first place, whether or not it is being clipped
  correctly. Sub-test 1's own side-of-horizon census (which has no
  bin-exclusion blind spot) is the diagnostic that does not go blind this
  way -- it is what actually establishes the defect at every `N` and
  tilt, including through the per-channel `N=1/7/30` row.

---

## 4. What was *not* changed

* **`Pdf` / `PdfNM` are untouched -- but the density this row's own emitted
  ray CARRIES did move, and that density is not a purely local quantity.**
  `Scatter`/`ScatterNM` are the only source of `rs2.bsdfPdf` / `c.bsdfPdf`
  for these two lobes, and PT/BDPT/VCM feed that value into their MIS
  weight as the BSDF-side density against an NEE partner --
  `TranslucentSPF::Pdf`/`PdfNM`, which has never covered either Phong lobe
  (`SPFPdfConsistencyTest`'s row comment: "Pdf() only covers diffuse lobe,
  not translucent") and returns 0 for these directions regardless. That
  MIS partition was therefore ALREADY broken before this row (DL-41, an
  open row with its own consequences) -- this fix does not create the
  break, and does not need to fix it to be correct on its own terms, but
  it does move the number that partition sees: `q`'s theta marginal is
  unchanged, so the movement is bounded by the SAME `pi/halfArc` factor
  that scales the whole clipped density relative to the unclipped one --
  at most 2x (`halfArc >= PI/2` always, post-orientation), and only under
  tilt (identity at tilt 0). Review-round-2 measurement on a tilted
  translucent fixture found the render-level movement from this bounded
  BSDF-side density change to be < 0.03% -- consistent with the bound: an
  already-degenerate partition (weight effectively 1 on the
  BSDF side, since the NEE partner is uniformly 0 here) does not become
  MORE broken merely because the BSDF-side density it already fully owns
  changed shape. Bringing the Phong lobes and their selection
  probabilities into `Pdf` in a principled way remains **DL-41**'s scope,
  not this row's; adding a bare per-lobe density without the selection
  weight would be a new half-correct thing. The density contract this row
  OWES -- the one the emitted `ScatteredRay` carries, consumed as an
  opaque throughput denominator -- is now exact; the density contract
  DL-41 owes -- a `Pdf()` that actually answers for these lobes -- remains
  open.
* **The entry front (reflection) lobe.** It already *has* the geometric gate;
  what it lacks is renormalization (it drops a below-horizon sample) and axis
  orientation. That is a different defect — energy loss, not a false state
  transition — and `Pdf`'s front branch documents the unnormalized
  restriction as the reviewed design. Filed as **DL-112**, and **CLOSED
  2026-09-17** with exactly DL-45's tool (measured: the lobe emitted
  `0.3·(1+cos φ)/2` of its reflectance, 0.15265 instead of 0.30000 at 89°) —
  [DL111_DL112_TRANSMISSION_PUSH_GATES.md](DL111_DL112_TRANSMISSION_PUSH_GATES.md) §6.
  One consequence to know before reading §4's first bullet: the entry and
  exit branches of `Pdf()` are now the SAME normalized clipped-cosine
  function wherever `geomN == geomNRaw`, which is what made
  `PTGuidingMISPartitionTest`'s DL-74 premise probe for this SPF need
  retargeting.
* **`DielectricSPF` / `PerfectRefractorSPF` / `SubSurfaceScatteringSPF`.**
  Same pattern, other files; filed as **DL-111** (§5), and **CLOSED
  2026-09-17**. Two findings there bear on this document. (i) For their
  DELTA lobes the defect is much narrower than "the same pattern" suggests:
  refraction into a *denser* medium can never land wrong-side, so the delta
  case needs a grazing, normal-perturbed silhouette refracting into a
  *rarer* medium. (ii) `DielectricSPF`'s `scattering` warp is a genuine
  lobe and took **this row's construction verbatim**, as a separate
  implementation in `GeometricUtilities::PerturbClipped`. `SampleClippedPhong`
  below is deliberately NOT refactored onto it: this one draws `cos(theta)`
  from its own `cos^N` inverse CDF and uses that value directly, while
  `PerturbClipped` takes a polar ANGLE (its callers' marginals — Henyey-
  Greenstein, Phong-by-`alpha` — produce one), so delegating would insert an
  `acos`/`cos` round trip into a path whose untilted branch is pinned
  bit-for-bit. Any change to the arc math belongs in BOTH.

---

## 5. Sibling audit (`docs/skills/audit-by-bug-pattern.md`)

**Pattern, in one sentence:** *a scattered lobe that transitions (or
deliberately preserves) IOR-stack membership is sampled about the shading
normal without confirming the direction is on the side the membership change
claims.*

| site | same pattern? | evidence | action |
|---|---|---|---|
| `TranslucentSPF::Scatter` entering `trans`, isotropic branch | **YES** | `ior_stack->push` with no geometric gate; 2039/8192 wrong-side at 60° | fixed, this slice |
| `TranslucentSPF::Scatter` entering `trans`, per-channel `N` branch | **YES** | same, ×3 channels sharing one canonical pair; 2626/24576 at 60° | fixed |
| `TranslucentSPF::ScatterNM` entering `trans` | **YES** | NM twin; identical counts | fixed |
| `TranslucentSPF::Scatter`/`ScatterNM` exit-branch interior backscatter (both `N` branches + NM) | **YES** (row: "shares the pattern… audit it alongside") | "stays inside this object, no stack change" violated — 605/4096 leave the solid at 45° tilt **without** the pop; the row called this "no stack-transition consequence", which understates it: the consequence is the *missing* transition | fixed |
| `TranslucentSPF` entry front (reflection) lobe | **NO** — gate present | `if( Dot(front.ray.Dir(), geomN) > 0 )` is already there; no stack action; the residual is an unnormalized *drop* plus an unoriented axis | refuted for this pattern; filed **DL-112**, closed 2026-09-17 |
| `TranslucentSPF` diffuse exit re-emission | already closed | DL-45 `SampleValidDiffuseExit` + matching `Pdf` | n/a |
| `DielectricSPF::GenerateScatteredRays*` transmission (`dielectric` lobe) | **YES**, other file — **fixed 2026-09-17** | `dielectric.ior_stack->push(rIndex)` (`DielectricSPF.cpp:213`); the only gate on the transmitted direction is `Dot(dielectric.ray.Dir(), ri.onb.w())` — the **shading** normal — while the companion Fresnel reflection lobe is correctly gated against `geomN`. The HG/Phong `scattering` warp (`Perturb` by `alpha`) widens the lobe off the Snell direction, so this is not a delta-only concern. | filed **DL-111**, closed 2026-09-17 |
| `PerfectRefractorSPF` transmission (`specular` lobe), RGB `:115` + NM `:289` | **YES**, other file — **fixed 2026-09-17** | `specular.ior_stack->push(newIOR)` with **no** geometric gate on the refracted direction at all; only the Fresnel lobe is gated against `geomN` | filed **DL-111**, closed 2026-09-17 |
| `SubSurfaceScatteringSPF` exit refraction (`exitRay`), RGB `:310` + NM | **YES**, other file — **fixed 2026-09-17** | carries the **popped** `exitStack` with no geometric gate, while the companion back-reflection *is* gated against `geomNBack`. Lower visibility: shipped materials set `bAbsorbBackFace=true` and do not reach this branch (cf. DL-51). | filed **DL-111**, closed 2026-09-17 |
| `PolishedSPF` | **NO** | never pushes or pops an IOR stack (its refracted ray is used only to evaluate Fresnel); no membership claim to violate | refuted |
| `CompositeSPF` | **NO** | a composition layer — it forwards whatever a sub-SPF produced (`scat.ior_stack ? *scat.ior_stack : gap_stack`) and makes no geometric claim of its own; it inherits its sub-SPFs' correctness | refuted (inherits) |
| `RandomWalkSSS` | **NO** | does its own outward-normal orientation from `vGeomNormal` (`RandomWalkSSS.cpp:84-85`, `:311-318`) and never mutates the caller's IOR stack | refuted |
| `RayCaster.cpp:2425` (`ior_stack.push(mediumIOR)`) | **NO** | a medium-boundary crossing driven by a real ray/geometry intersection, not by a sampled lobe direction; DL-70 already audited the geometric-normal side of the medium walks | refuted |

One hop further (downstream consumers of the fixed lobes): the emitted
`ScatteredRay::pdf` is the throughput denominator in PT/BDPT/VCM and the
`TranslucentPelPhotonTracer` deposit weight; all read it as an opaque number,
so no consumer needed a change. `TranslucentPhotonEnergyTest` (14 checks) and
`TranslucentIORStackTest` (real trained OpenPGL + production PT/BDPT RGB/NM)
both stay green.

---

## 6. Gate

Clean library rebuild, zero warnings. Per-test builds, all green:

| suite | result |
|---|---|
| `TranslucentEntryHorizonTest` (new) | 251 checks / 0 failures (red: 83 at original closure; sub-tests 7/8 independently red-proved at ~49% wrong-side/escaped @89deg against `12027967` in review round 2) |
| `TranslucentTiltedExitTest` | ALL TESTS PASSED |
| `TranslucentDoubleSidedTest` | Passed 44 / Failed 0 |
| `TranslucentInitialContainmentTest` | Passed 43 / Failed 0 |
| `TranslucentIORStackTest` | ALL TESTS PASSED (real trained OpenPGL + production PT/BDPT, RGB and NM) |
| `TranslucentSpectralParityTest` | 1846 checks / 0 failures |
| `TranslucentPhotonEnergyTest` | 14 checks / 0 failures |
| `TranslucentSamplerDimensionCountTest` | 65589 checks / 0 failures (was 65544 at original closure; +45 P2-a rows covering the entering/backscatter `trans` lobes directly) |
| `SPFPdfConsistencyTest` | all passed |
| `SPFBSDFConsistencyTest` | all passed |
| `PTGuidingMISPartitionTest` | 63 checks |
| `LayeredWhiteFurnaceTest` | 0 failing configurations |
| `IORStackSeedingRegressionTest` | 15 passed / 0 failed |
| `CompositeExtinctionTest` | all checks passed |
| `CstDeriveGoldenTest` | 0 drift |
| `SourceHygieneTest` | 165 passed / 0 failed (341 test files scanned) |

## 7. DL-130 closure — clipped density does not scale lobe energy

**CLOSED 2026-09-21 as documentation-only; production behavior was
already correct.** The current post-DL-157 one-function contract makes
the answer algebraic. For a lobe with normalized clipped directional
density `q(w)`, the evaluator is

`f(w) = kray * q(w) / |cos(w,n)|`.

Therefore `integral f(w)|cos(w,n)| dw = kray * integral q(w) dw = kray`.
The clipped-away directions cannot exist; their probability is
renormalized over the valid arc, while the lobe's painter-authored total
energy remains `kray`. Multiplying `kray` by `halfArc/PI` would apply the
clip twice, delete energy as tilt grows, and make `BuildLobeSet`'s
selection weights cease to be proportional to the lobe energies they
already represent. The same reasoning covers both the entry
transmission lobe and the interior backscatter lobe. No source behavior
changed.

`TranslucentEntryHorizonTest` sub-test 10 now pins that contract at
tilts 0°, 30°, 60°, and 85°, for RGB split-N and NM on both lobes. Each
row uses 4,096 real samples and prints count, mean, and sample SD. Every
sample has the same direction-independent `kray`, so sample SD is
exactly zero: entry means are `(0.4,0.4,0.4)` RGB and `0.399936` NM;
backscatter means are `(0.491238,0.491238,0.491238)` RGB and `0.491238`
NM at every tilt. The suite moves from **251/0 to 283/0**. There is no
red behavior count to claim: this row was an unresolved contract
question, and the present shared lobe-set/evaluator contract proves the
existing implementation rather than requiring a repair.
