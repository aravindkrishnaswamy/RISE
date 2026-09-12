# The η² basic-radiance factor at dielectric interfaces (debt 30, 2026-09-12)

**Status: RESOLVED.** Until 2026-09-12 RISE applied no η² radiance
scaling at any dielectric interface, in any integrator or shader op.
Every RADIANCE-mode walk now multiplies its throughput by
`(η_before / η_after)²` whenever a scattered ray's medium changes;
IMPORTANCE-mode walks (light subpaths, photon tracers, SMS photon seeds,
detector-sphere rigs) deliberately do not.

Guard: [`tests/RefractiveRadianceScalingTest.cpp`](../tests/RefractiveRadianceScalingTest.cpp)
(38 checks, ~27 s), plus `VCMStrategyBalanceTest` topology H (the red row)
and `BDPTStrategyBalanceTest` topology J (the cancellation pin). Review
round 2 (2026-09-12) added `VCMStrategyBalanceTest` topology I and
`BDPTStrategyBalanceTest` topology K, a consistency pin on the same
scene family where the eye-side and light-tracing-splat strategies are
MIS-combined for the same path rather than isolated (§2's corrected
splat row). Review round 3 (2026-09-12) fixed two independent
pre-existing `TranslucentSPF` bugs found auditing that fix (an
unconditional-on-success-path `IORStack` leak plus a narrower
overflow-carryover variant of it, and a channel-0-only lobe gate that
silently dropped a reflectance/transmittance painter with zero red —
`tests/TranslucentIORStackTest.cpp`, 44 checks), added a
`perfectrefractor_material` variant of row A pinning the eta^2 factor's
second producer (`RefractiveRadianceScalingTest`, 30 → 38 checks), and
measured — for the first time — the counterfactual `VCMStrategyBalanceTest`
topology I / `BDPTStrategyBalanceTest` topology K were designed to catch:
applying the eta^2 factor to the light-tracing-splat side too. VCM's
topology I catches it dramatically (+45.6-45.8%, ~5.7x its own 8% band);
BDPT's topology K does NOT catch it at all (indistinguishable from the
correct-code baseline on this scene) — see both files' topology headers
for the measured ratios and the honest gap this leaves in topology K's
coverage.

---

## 1. The physics

Radiance is **not** invariant along a ray that crosses a smooth interface
between media of different refractive index. The invariant is the *basic
radiance* `L / n²` (Preisendorfer 1965; Veach 1997 §5.2; PBRT-v4 §9.5.2).
Stated in plain medium terms, following the light's own physical direction
of travel from a medium of index `n1` into one of index `n2`,

```
L2 = T(θ) · (n2 / n1)² · L1
```

with `T` the Fresnel transmittance. Light physically entering water from
air (`n1 = 1 → n2 = 1.33`) gets **brighter** by `1.7689`; light physically
leaving water for air (`n1 = 1.33 → n2 = 1`) gets **dimmer** by `1/1.7689`.
This is the textbook statement — a denser medium in equilibrium with an
external field carries higher radiance.

A path tracer's eye subpath does **not** follow that forward direction: it
starts at the camera and walks *backwards* along the light's direction of
travel. Naming the two sides of a scatter event by the WALK's own
direction — `η_before` the medium the walk was in when it hit the surface
(the side it arrived from), `η_after` the medium the scattered ray now
travels through (the side it leaves for) — the throughput multiplier a
RADIANCE-mode walk must apply at that scatter is

```
throughput *= (η_before / η_after)²
```

**(corrected, review round 3: this is the SAME number as the
medium-forward formula above for the light actually being transported,
NOT its reciprocal** — an earlier draft of this section claimed the
reciprocal relationship and was wrong physics; see the worked example
below for why.) The walk's `before → after` is the reverse of the
direction the PHYSICAL light ray travels along that same segment, and
reversing a ratio while also swapping which medium plays `n1` and which
plays `n2` cancels out: the light ray that actually delivers radiance
back to the camera along an eye-walk segment travels FROM `η_after`
(where the walk is heading, deeper into the scene) TO `η_before` (where
the walk came from, back toward the camera) — i.e. the walk's `before` is
the light's DESTINATION medium `n2`, and the walk's `after` is the
light's SOURCE medium `n1`. Substituting into the medium-forward formula,
`(n2/n1)² = (η_before/η_after)²` — textually identical to the walk-order
factor, not its inverse.

Worked example, camera in air: an eye ray that refracts from air into a
submerged object has `η_before = 1` (air, where the walk arrived from)
and `η_after = 1.33` (water, where the scattered ray goes), so its
throughput is multiplied by `(1/1.33)² = 1/1.7689` — **dimmer**. The
physical light ray this eye-walk segment represents travels water → air
to reach the camera (`n1 = 1.33`, `n2 = 1`), so the medium-forward formula
gives `(n2/n1)² = (1/1.33)² = 1/1.7689` — the SAME `1/1.7689`, not
`1.7689`. (The forward-direction ray through that same interface point,
air → water, is a genuinely DIFFERENT physical ray — one that would carry
radiance the other way, deeper into the water, and is not the ray this
eye walk represents at all; comparing against it, as an earlier draft of
this section did, is a category error dressed up as a reciprocal
relationship.) §7's "camera INSIDE a refractor looking out: air-side
content ×n²" is the identical identity applied the other way: the walk's
`before` is water (where the camera sits) and `after` is air, so
`(η_before/η_after)² = 1.33² = n²`, matching the medium-forward formula
for the physical light ray travelling air → water to reach that camera
(`n1 = 1, n2 = 1.33`, `(n2/n1)² = n²`). The two formulas agree in BOTH
directions — there is no case in this document where the eye-walk
convention and the medium-forward convention disagree; row A (`1/n²`)
and row B (`n²`) of `RefractiveRadianceScalingTest` are both consistent
with "identical," neither with "reciprocal."

This is exactly the convention `tests/RefractiveRadianceScalingTest.cpp`'s
own header states operationally ("Camera in air entering water: x
1/1.33^2. Camera in water exiting to air: x 1.33^2.") — before/after
there is the walk's medium at each step, not the light's forward-medium
order.

An IMPORTANCE-mode walk (a light subpath, a photon) transports flux
forward, in the light's own true direction, so it never applies this
walk-order factor at all — it gets no factor beyond Fresnel, conserved up
to Fresnel. That asymmetry is not a convention; it *is* the non-symmetry
of refractive scattering, and it is what makes a merge (a flux-carrying
photon paired with a radiance-carrying eye vertex) come out right.

## 2. What was wrong, and why it hid for years

`DielectricSPF::GenerateScatteredRay` sets the transmission lobe's
`kray = (1 − Fresnel) · tau^distance`. No η term, and no consumer added
one — grepping `eta` / `TransportMode` across `src/Library/Shaders`
returned nothing.

The reason nobody noticed is a **double cancellation** that covers the
single most common scene in the whole corpus: camera in air, light in
air, something refractive in between.

1. An eye path that **enters** water (owes ×1/n²) and later **exits** it
   to reach the emitter (owes ×n²) nets ×1. Both factors being absent is
   indistinguishable from both being present.
2. A BSDF-sampled emitter seen *through* an interface has its hit
   probability compressed by 1/n² (refraction compresses solid angle)
   while its radiance is not boosted. The irradiance comes out low by n²
   and the viewing high by n². Two errors cancelling.

Strategies whose **light side is flux-based** get neither cancellation,
because they never traverse the interface a second time in the radiance
direction:

| strategy | eye side | light side | pre-fix error |
|---|---|---|---|
| PT BSDF-sampled emitter through glass | in-and-out | — | none (cancels) |
| BDPT s=0 through glass | in-and-out | — | none (cancels) |
| **VCM / photon-map merge** | one crossing inward | photon (flux) | **×n² too bright** |
| **BDPT light-tracing splat** | none | light subpath (flux) | **none — consistent by construction** |
| **transparent-shadow NEE** | one crossing inward | straight shadow ray (flux) | **×n² too bright** |
| **emitter INSIDE the refractor, viewed from outside** | one crossing inward | — | **×n² too bright** |
| **camera INSIDE the refractor** | one crossing outward | — | **×1/n² too dim** |

**Correction (review round 2, 2026-09-12):** an earlier draft of this row
read "one crossing inward" for the BDPT light-tracing splat and claimed
the same ×n² pre-fix error as the VCM merge row above it. That is wrong
on two counts. First, a `t=1` splat has no eye-side subpath at all — the
"eye side" is the bare camera vertex, so there is no crossing to price
there. Second, a splat is a straight-line connection test, and a delta
interface is opaque to one: a light-subpath vertex sitting on the far
side of the water from the camera cannot validly splat to it at all (the
connection is occluded, exactly like an NEE shadow ray through the same
interface). The only way a light-subpath vertex reaches the SAME side as
the camera is by crossing the interface earlier, *while the light
subpath is being built* — an ordinary IMPORTANCE-mode scatter, which
correctly gets no factor whether or not this fix exists. So the splat
connection itself never had a crossing to misprice; the "bug" this row
tried to describe doesn't occur on RISE's actual splat mechanics. See
`BDPTStrategyBalanceTest` topology K (and its VCM twin, topology I in
`VCMStrategyBalanceTest`) §8-adjacent below for the scene that pins this
consistent-by-construction behaviour under MIS combination with the
eye-side strategies that DO cross the interface.

The last two rows of the table (emitter INSIDE / camera INSIDE) are the
reference-free ones, and they are what `RefractiveRadianceScalingTest`
rows A and B assert.

## 3. The exact measurement

A Lambertian luminaire (`exitance 1`, `scale 1` ⇒ `L = 1/π = 0.318310`)
on a quad, viewed from directly above by a pinhole camera in air, (a) in
air and (b) 25 cm inside a `dielectric_material` water box
(`tau 1.0`, `scattering 1000000` = pure delta transmission).
64×64, `oidn_denoise FALSE`, `pixel_filter box`, EXR
`Rec709RGB_Linear`. Scenes and logs under the session scratchpad; the
same geometry is row A of the test.

| ior | | PT | BDPT | VCM | pixelpel | physics |
|---|---|---|---|---|---|---|
| — | dry (no box) | 0.318359 | 0.318359 | 0.318359 | 0.318359 | 0.318310 |
| 1.33 | submerged, **before** | 0.311816 | 0.311816 | 0.311816 | 0.312012 | **0.176338** |
| 1.33 | submerged, **after** | **0.176310** | **0.176310** | **0.176310** | **0.176392** | 0.176338 |

Before: exactly `T·L` — the Fresnel transmittance was applied and the
1/n² was not (`0.311816 / 0.176338 = 1.7683`, close to but not the same
number as `n² = 1.33² = 1.7689` — the ~0.03 % gap is measurement
quantization in the "before" render, not a different physical constant).
After: within 0.02 % of `T·L/n² = 0.97994 · 0.318310 / 1.7689`.

The dry row is 0.318359 rather than 0.318310 because EXR stores half
floats; 0.318359 is the nearest half to 1/π.

## 4. Slab probe — where the integrators disagreed

Lambertian floor ρ=0.5 at y=0.05 inside a water box y∈[0, 0.3]
(ior 1.33), camera in AIR at (2, 2.5, 0), light at (0, 2.5, 0); 96×96,
`pixel_filter box`, `oidn_denoise FALSE`.

| light | | PT | PT ts | BDPT | VCM | VCM/PT |
|---|---|---|---|---|---|---|
| delta omni, dry | before | 0.083784 | 0.083783 | 0.083783 | 0.083777 | 1.000 |
| delta omni, dry | after | 0.083784 | 0.083783 | 0.083784 | 0.083777 | 1.000 |
| tiny sphere emitter, dry | before | 0.005655 | – | 0.005655 | 0.005652 | 0.999 |
| tiny sphere emitter, dry | after | 0.005656 | – | 0.005655 | 0.005652 | 0.999 |
| delta omni, **submerged** | before | 0.000000 | 0.095650 | 0.000000 | 0.102916 | – |
| delta omni, **submerged** | after | 0.000000 | **0.054075** | 0.000000 | **0.056977** | – |
| tiny sphere emitter, **submerged** | before | 0.004666 | – | 0.004786 | 0.007252 | **1.554** |
| tiny sphere emitter, **submerged** | after | 0.004690 | – | 0.004857 | **0.004908** | **1.046** |

Read the rows in pairs:

- **Dry rows do not move at all.** No interface, no factor. This is the
  cheapest possible check that the change is scoped.
- **Delta / submerged**: PT and BDPT are exactly 0 before and after —
  structural, and *correct*. An opaque shadow ray through a delta water
  surface kills every NEE connection, and no other PT strategy can reach
  a delta light through a delta interface. The two estimators that CAN
  see it, transparent-shadow PT and VCM, both dropped by
  `0.095650/0.054075 = 1.769` and `0.102916/0.056977 = 1.806` — n² and
  n² within VCM's noise on a delta caustic — and still agree with each
  other (0.949 after, 0.929 before).
- **Area / submerged** is the headline: VCM was **1.554× PT** and is now
  **1.046×** (`0.004908/0.004690 = 1.0465`, rounds to 1.046). PT and BDPT barely moved (0.004666 → 0.004690,
  0.004786 → 0.004857 — both inside their own run-to-run spread), which
  is the cancellation of §2 doing exactly what it is supposed to. This
  probe's emitter radius is 0.03 — a different scene from the two
  regression guards below, so the three numbers are independent
  measurements of the same physics, not three re-derivations of one
  run:
  - `tests/VCMStrategyBalanceTest.cpp` topology H (radius 0.08, 64×64,
    2048 spp): VCM/PT = 0.00467741/0.00463638 = **1.0088** (re-measured
    for this round; the file's own header records 1.0066 from its
    red-proof run and the assertion bands at 8%/60%/4x on mean/p99/max,
    so both readings pass comfortably).
  - `tests/RefractiveRadianceScalingTest.cpp` row C (radius 0.08, same
    scene family, 128/64/2048 spp for PT/BDPT/VCM): VCM/PT =
    0.00468599/0.00462829 = **1.0125** (re-measured for this round;
    BDPT/PT = 1.0067 in the same run).
  All three sit inside VCMStrategyBalanceTest's 8% mean band, which is
  sized for auto-radius merge drift (the `VCMRasterizerBase::
  PreRenderSetup` log line each run prints its own `effective_radius`
  from the scene's photon density that run), not for a fixed physical
  constant — a rerun at a different seed base or a different sample
  count will land at a different point inside that band, not on 1.046
  exactly.

## 5. `tidal_stones` — the debt-30 "BDPT vs VCM ≈ 20×" is TWO separate things

`scenes/FeatureBased/Textures/tidal_stones.RISEscene`, pinhole variant,
160×120, 64 spp, `pixel_filter box`, `oidn_denoise FALSE`.

| integrator | mean, before | mean, after | submerged-quadrant ratio vs PT-ts, before | after |
|---|---|---|---|---|
| PT, `transparent_shadows TRUE` | 0.109177 | 0.090168 | 1 | 1 |
| PT, `transparent_shadows FALSE` | 0.063864 | 0.063867 | 0.003–0.10 | 0.005–0.16 |
| BDPT | 0.064695 | 0.064459 | 0.011–0.10 | 0.012–0.16 |
| VCM | 0.113430 | 0.092001 | 0.87–1.24 (mean 1.04) | 0.96–1.15 (mean 1.02) |

**Finding 1 — the "20×" is BDPT's structural S-D-S gap, not a bug in
either integrator, and it is NOT what this fix addressed.** Every
caustic-lit stone pixel is `E → water top (S, delta) → stone (D) →
water top (S, delta) → L (delta)`. BDPT has no strategy for it: s=1 NEE
from the stone is blocked by the water surface, t=1 (stone → camera) is
blocked the same way, and every other split lands on a delta vertex.
VCM reaches it by merging at the stone. On tidal, BDPT is numerically
*identical* to "PT without transparent shadows" (0.0647 vs 0.0639
before, 0.0645 vs 0.0639 after) and VCM tracks transparent-shadow PT.
The ledger's "1 : 2.8 : 58" is BDPT/PT ≈ 0 on the masked pixels, not a
weighting error. This part needed documentation, not code.

**Finding 2 — the η² factor, which this fix landed.** Both of the
estimators that can actually see under the water dropped together:
whole-image PT-ts 0.109177 → 0.090168 and VCM 0.113430 → 0.092001, and
per-quadrant the SUBMERGED blocks dropped to 0.57–0.61 of their
pre-fix values (= 1/n² = 0.565) while the above-water blocks moved by
under 0.1 %. The two remain within 2 % of each other. PT-without-
transparent-shadows is unchanged to 5 digits, as it must be: it
contributes nothing under the water either way.

## 6. The rule, and where it is implemented

> **RADIANCE-mode walk** — anything rooted at a camera. When a scattered
> ray's IOR-stack top changes from `η_before` to `η_after`, multiply the
> throughput by `(η_before / η_after)²`. Reflection (stack unchanged):
> ×1. Applies to delta AND rough (`scattering < 1e6`) transmission
> alike. In the spectral / HWSS twins the factor is **not** re-derived
> per wavelength: it is computed ONCE from the HERO wavelength's
> IOR-stack transition (`pS->ior_stack` in PT, `pScat->ior_stack` in the
> BDPT/VCM/MLT eye subpath) and broadcast as a single scalar to the
> hero's throughput and every still-live companion wavelength alike
> (PathTracingIntegrator.cpp's dispersive-delta-termination site, and
> BDPTIntegrator.cpp's `GenerateEyeSubpathImpl`). This is exact when
> every wavelength crosses the same medium boundary — the common case,
> and the same hero-only convention the pre-existing delta-lobe
> `krayNM` broadcast already used — but is not independently verified
> per companion wavelength the way non-delta `EvalBSDFAtVertexNM`
> is; see the comments at both sites for the scope of the guarantee.
>
> **Bounded error when the broadcast is wrong (review round 2,
> 2026-09-12).** PT's termination block and BDPT's
> `HasDispersiveDeltaVertex` (~line 6866) both rely on
> `IMaterial::GetSpecularInfoNM` to detect a dispersive delta crossing
> and stop broadcasting the hero's factor to companions that shouldn't
> share it. Neither `CoatedSPF` nor `CompositeSPF` overrides
> `GetSpecularInfoNM`, so wrapping a DISPERSIVE dielectric in a
> `coated_material` or `composite_material` defeats that detection —
> the hero's etaScale reaches every companion regardless of its own
> IOR. The per-crossing error this produces is exactly
> `(η_hero/η_companion)² − 1`: for an illustrative crown-glass-class
> Δn ≈ 0.02 across 400–700 nm (e.g. 1.50 vs 1.52) that is ≈2.6–2.7%; for
> an illustrative high-dispersion flint-class Δn ≈ 0.07 (e.g. 1.78 vs
> 1.85) it climbs to ≈7.4–8.0%. These are illustrative index pairs
> chosen to match ordinary optical-glass dispersion magnitudes, not a
> measured catalog curve — the point is that the error is bounded by
> real Δn, not unbounded, and it compounds once per crossing on a
> multi-bounce path through such a wrapper. See the matching comment at
> `PathTracingIntegrator.cpp`'s dispersive-delta-termination site.
>
> **IMPORTANCE-mode walk** — light subpaths, photon tracers, SMS photon
> seeds, detector-sphere rigs. No factor.
>
> **Shadow rays / `CastShadowRayTransmittance` / transparent shadows.**
> No factor: a straight-line transmittance is a flux estimate.

No `TransportMode` parameter was added to `ISPF::Scatter` (~60
implementations). The factor is applied at the **consumer**, from the
two stacks the consumer already holds, by one inline helper in an
existing header:

```cpp
// src/Library/Utilities/IORStack.h
inline Scalar RadianceEtaScale( const IORStack& before, const IORStack* after );
```

`after` is `ScatteredRay::ior_stack`, which is null whenever the SPF left
the stack alone. The helper returns exactly 1 when the medium did not
change, so every reflection and every non-transmissive lobe is
bit-identical to the pre-fix behaviour. The contract that `kray` /
`krayNM` EXCLUDE the factor is written on the fields themselves in
`Interfaces/ISPF.h` and restated at `DielectricSPF`'s kray assignment.

**The factor telescopes**, which is why reading it off the two endpoints
is exact even across a layered SPF's internal chain:
`(n_out/n_gap)² · (n_gap/n_below)² = (n_out/n_below)²`.

### 6.1 Site table

| file | sites | mode | changed | why |
|---|---|---|---|---|
| `Shaders/PathTracingIntegrator.cpp` | 3 | RADIANCE | **yes** | SPF/specular continuation (throughput + its RR importance), BSDF continuation, HWSS bundle |
| `Shaders/BDPTIntegrator.cpp` `GenerateEyeSubpathImpl` | 1 | RADIANCE | **yes** | the eye side of BDPT, VCM and MLT alike |
| `Shaders/BDPTIntegrator.cpp` `GenerateLightSubpathImpl` | 1 | IMPORTANCE | no | comment only |
| `Shaders/RefractionShaderOp.cpp` | 2 (Pel + NM) | RADIANCE | **yes** | the legacy `pixelpel` chain's transmission consumer |
| `Shaders/ReflectionShaderOp.cpp` | 2 | RADIANCE | no | consumes only `eRayReflection`; a reflection never changes medium, so the helper would return 1 |
| `Shaders/DistributionTracingShaderOp.cpp` | 4 (Pel/NM × multi/single lobe) | RADIANCE | **yes** | |
| `Shaders/FinalGatherShaderOp.cpp` | 2 | RADIANCE | **yes** | one specular continuation (real), one diffuse gather (identity, written for uniformity) |
| `Shaders/StandardShader.cpp`, `Shaders/AdvancedShader.cpp` | 3 each | — | no | **they never read `kray`** — they only call `Scatter` and hand the container to shader ops. The consumers are the ops above. |
| `PhotonMapping/{Global,Caustic}{Pel,Spectral}PhotonTracer.cpp`, `TranslucentPelPhotonTracer.cpp` | 5 | IMPORTANCE | no | comment only |
| `Utilities/SMSPhotonMap.cpp` | 1 | IMPORTANCE | no | photon seeds; comment only |
| `Utilities/ManifoldSolver.cpp` | 1 | RADIANCE | no — **already correct** | SMS already applied `(η_i/η_t)²` per refraction, in the same convention. Its comment claiming PT omits the factor was corrected. |
| `Materials/CoatedSPF.cpp` | 1 | — | no | CLOSED layer: enters and leaves the same medium, so the consumer-side factor is 1, and the coat's own exit-side 1/η² is already inside `CoatedLayer.h`'s closed form |
| `Materials/CompositeSPF.cpp` | 4 | — | no | telescoping (§6); an up-exit gets 1, a down-exit gets the single net factor |
| `Materials/FabricSPF.cpp` | 2 | — | no | pure re-dispatch to the weave BSDF in a fibre frame; no medium change |
| `Materials/PerfectRefractorSPF.cpp` | 1 push + 1 pop (mirrors `DielectricSPF`'s entry/exit shape) | RADIANCE (consumer-applied, same as `DielectricSPF`) | **yes, via the consumer** | a delta dielectric with no Fresnel-modulated tau/scattering knobs — otherwise the same push-on-entry / pop-on-exit shape as `DielectricSPF`, so it gets the debt-30 factor the same way, from the same consumer sites (§6.1's integrator/shader-op rows), not from any change inside this file itself. No test in the tree exercises `perfectrefractor_material` for the eta² factor specifically: `SPFBSDFConsistencyTest`, `IORStackSeedingRegressionTest`, and `ConnectionLegalityTest` all use it, but for BSDF/pdf/connection-legality correctness, not radiance scaling — the many caustic/SMS scenes under `scenes/Tests/` that use `perfectrefractor_material` are visual, not asserted. |
| `Materials/PolishedSPF.cpp` | 0 (19 `ior_stack` occurrences total, re-verified review round 3: 7 are `.top()` reads for Fresnel pricing at lines 94, 95, 351, 482, 483, 522, 523; the other 12 are `ISPF::Scatter`/`ScatterNM`/`Pdf`/`PdfNM` signature parameters and pass-through call arguments, not reads) | — | no | transmits into the SAME object's substrate (a coat-over-substrate material), never pushing a new medium onto the stack — net factor 1 by construction, the same shape as `CoatedSPF`'s closed layer. |
| `Materials/BioSpecSkinSPF.cpp` | 0 | — | no | carries `ior_stack` only as an unused interface parameter (`Scatter`/`ScatterNM` never read it) — **not** `containsCurrent()`-gated the way `GenericHumanTissueSPF` is (see the next row); grep confirms zero non-signature references. |
| `Materials/GenericHumanTissueSPF.cpp` | 0 (2 `containsCurrent()` reads) | — | no | reads `ior_stack.containsCurrent()` to branch its own scattering behaviour but never pushes or pops — no medium-change site here for the debt-30 factor to apply to. |
| `Materials/WeaveSPF.cpp` | 0 | — | no | pure pass-through to its own `ScatterImpl`, same "thin transmission, no stack change" shape as `FabricSPF` above. |
| `DetectorSpheres/*` | 5 | FLUX | no | measurement rigs; comment only |
| `Materials/SubSurfaceScatteringSPF.cpp` (front-face BSSRDF entry), `Utilities/RandomWalkSSS.{h,cpp}`, `Utilities/BSSRDFSampling.h` (`Sw`) | 0 | — | no — **untouched — telescoping argument; PBRT convention differs; NAMED RESIDUAL** | the front-face BSSRDF entry path (`BSSRDFSampling::SampleEntryPoint`, `RandomWalkSSS`) never touches the IOR stack (grep confirms zero `IORStack` references in either) — BSSRDF entry/exit through the SAME interface is priced by the diffusion-profile importance sampling in the integrator, not by a stack transition, so the debt-30 factor is exactly 1 there. This is telescoping applying to an entry-and-exit-through-the-same-interface path, not "SSS never touches the stack" — see the next row. |
| `Materials/SubSurfaceScatteringSPF.cpp` back-face exit branch (RGB ~line 300-313, NM ~line 503-513) | 2 | RADIANCE / IMPORTANCE (mode-agnostic) | no — **arguably already correct; untested — NAMED RESIDUAL** | this branch (`// Also emit exit refraction if possible (for light subpaths that need to escape the medium)`) DOES touch the stack: `exitRay.ior_stack = new IORStack(ior_stack); exitRay.ior_stack->pop();`. It only fires when the walk is already inside the object at a back-face hit — reachable because `SubSurfaceScatteringMaterial::GetSpecularInfo` reports `canRefract = true` (`SubSurfaceScatteringMaterial.h` ~119, ~133), so `IORStackSeeding` (~194-209) tracks it and will seed a camera or light source starting inside an SSS object. When the walk was never actually seeded inside the object (the ordinary case: camera outside, BSSRDF entered and exited through the SAME point), this pop is a logged no-op — `IORStack::pop`'s find-and-destroy silently fails because the object was never pushed. When the walk WAS seeded inside (a submerged SSS object, or a camera embedded in one), this pop is a REAL exit transition and the debt-30 consumer-side factor at `RadianceEtaScale` fires exactly as it would for any other material's push/pop. No existing test exercises this branch with a seeded-interior walk. See §10.1(a)/(b). |

**Is this table exhaustive over `src/Library/Materials`?** (review round
2, 2026-09-12; recount verified review round 3, 2026-09-12 — the earlier
"24"/"20" were an arithmetic slip against this section's OWN enumeration,
which already summed to 25/21) `grep -ln 'ior_stack\|IORStack'
src/Library/Materials/*.cpp` returns **25** files. Of those, only four
actually push or pop the stack
(`grep -c 'ior_stack->push\|ior_stack\.push\|->pop()\|new IORStack'` > 0):
`DielectricSPF.cpp` and `TranslucentSPF.cpp` (both covered at length in
§2/§6/§6.1's `TranslucentSPF` discussion and C1 of this round's fix, not
repeated as a table row here), `PerfectRefractorSPF.cpp` and
`SubSurfaceScatteringSPF.cpp` (both rows above). Of the remaining **21**,
seven are covered by name above (`CoatedSPF`, `CompositeSPF`, `FabricSPF`,
`PolishedSPF`, `BioSpecSkinSPF`, `GenericHumanTissueSPF`, `WeaveSPF`)
because they read the stack (`.top()` or
`containsCurrent()`) without ever changing it. The remaining files
(`AshikminShirleyAnisotropicPhongSPF`, `CoatedBRDF`, `CookTorranceSPF`,
`GGXSPF`, `HairBSDF`, `IMaterial.cpp`, `IsotropicPhongSPF`,
`LambertianSPF`, `OrenNayarSPF`, `PerfectReflectorSPF`, `SchlickSPF`,
`SheenSPF`, `WardAnisotropicEllipticalGaussianSPF`,
`WardIsotropicGaussianSPF`) carry `ior_stack` only as an `ISPF::Scatter`/
`ScatterNM` interface parameter, pass it through unexamined, and never
push, pop, or query it — the same shape as `BioSpecSkinSPF` above, not
individually called out because none of them is a refractive or
subsurface material where a reader might otherwise expect a site. This
table is exhaustive over every file that TOUCHES `ior_stack` in
`src/Library/Materials`.

### 6.2 One hop deeper (audit-by-bug-pattern)

- **Nothing re-prices eye-side throughput from a stored vertex.**
  `PathVertexEval` rebuilds a `RayIntersectionGeometric` from a
  `BDPTVertex` to evaluate BSDFs and pdfs; it never recomputes
  throughput. `StoreThroughput` writes the already-scaled `beta` onto the
  vertex, and connections, splats, VCM merges (`v.throughput` /
  `v.throughputNM` in `VCMIntegrator.cpp`) and MLT re-evaluation all read
  that field. `BDPTVertexRIGRebuildTest` (68 checks) is green. The one
  place that DOES touch a stored vertex's throughput after the fact is
  `BDPTIntegrator::RecomputeSubpathThroughputNM` (~line 6770; callers
  `BDPTSpectralRasterizer.cpp` ~line 407, `MLTSpectralRasterizer.cpp`
  ~line 476, `VCMSpectralRasterizer.cpp` ~line 498) — the HWSS companion-
  wavelength re-evaluation that adjusts a hero-wavelength subpath's
  vertices to a companion wavelength. It rewrites `v.throughputNM` IN
  PLACE, but only MULTIPLICATIVELY, by a per-vertex ratio
  `companionValue / heroValue` (emission ratio at the light endpoint,
  BSDF ratio at a non-delta surface vertex, exactly 1.0 at a delta
  vertex). It never recomputes the debt-30 etaScale factor from scratch
  — that factor was already folded into `v.throughputNM` when the hero
  wavelength's subpath was originally generated, and a multiplicative
  ratio update cannot remove a factor already baked into the value it
  multiplies. So the hero's etaScale survives into every companion
  wavelength's adjusted throughput unchanged, which is exactly why §6's
  hero-only broadcast convention is safe here: this function is not a
  second, independent per-wavelength pricing of the interface crossing.
- **No pdf machinery reads `kray`.** `grep -l kray` over
  `src/Library/Shaders` returns the two integrators, the four shader ops
  and `BDPTIntegrator.h`'s `KrayValue` accessor — `MISWeight`,
  `VCMRecurrence.h` and `ConvertDensity` are not among them. The factor
  is a throughput term, not a density term, and putting it in a pdf would
  be wrong.
- **Eye paths that START inside a dielectric** exit with ×n². That is
  row B of the new test and the Snell's-window paragraph in
  [SUBMERGED_CAMERA_IOR_SEEDING.md](SUBMERGED_CAMERA_IOR_SEEDING.md).
  `EnvLightBalanceTest` topology J uses `ior 1.0`, so its factor is
  exactly 1 and it stays bit-exact at `mean = p99 = max = 1.0` (116/116
  green after the fix).
- **SMS was already right.** `ManifoldSolver.cpp`'s chain evaluation
  multiplies `(η_i/η_t)²` at every refraction, with `η_i` on the
  receiver side and `η_t` on the source side — for a chain walked
  shading-point → light that is exactly `η_before / η_after`. It is not
  a double count with the PT walk that delivered the eye ray *to* the
  shading point: the two price disjoint segments. Verified empirically
  in §8.

## 7. Scene classes whose look CHANGES

Everything below gets **dimmer or brighter by exactly n²**; nothing
changes shape, hue or noise character.

- **A surface or an emitter inside a refractor, seen from outside:
  ×1/n².** Fish under water, anything cast in resin or glass, a filament
  inside a glass envelope, a luminaire submerged in a pool. At water's
  1.33 that is 0.565×; at glass's 1.5, 0.444×.
- **Caustics reached by a merge / photon / light-tracing splat with the
  camera outside the medium: ×1/n².** This is the VCM and photon-map
  case, and it is the one that used to disagree with PT.
- **Transparent-shadow NEE onto a submerged surface: ×1/n².**
- **A camera inside a refractor looking out: air-side content ×n².**
  Snell's window gets brighter by 1.77× (water) / 2.25× (glass) — which
  is physically right: the radiance inside a medium of index n in
  equilibrium with an external field of radiance L is n²L.
- **Unchanged:** anything with no refractive interface between camera and
  subject; reflections; a path that enters and leaves the same medium
  (glass seen from air with the lit subject also in air — the whole
  ordinary "glass object on a table" case).

Scenes in-tree whose rendered brightness moves: `tidal_stones`
(submerged stones −n²), the VCM pool / caustic scenes with the camera
above the water, and any scene with a submerged camera. `TidalStonesShowcaseTest`
measures wet/dry RATIOS, which the factor does not move, and stayed at
122/122.

## 8. SMS consistency evidence

`sms_veach_egg` and `sms_luminous_orb` (both interior-light: a luminaire
sealed inside a dielectric shell, so the eye path crosses the shell once
and does NOT cancel), PT+SMS vs VCM, 256 spp, `oidn_denoise FALSE`,
`pixel_filter box`, EXR linear. Whole-image RGB means:

| scene | | PT+SMS | VCM | PT+SMS / VCM |
|---|---|---|---|---|
| `sms_veach_egg` | before | 0.982173 | 0.989697 | 0.9924 |
| `sms_veach_egg` | after | 0.981682 | 0.989654 | 0.9919 |
| `sms_luminous_orb` | before | 0.263954 | 0.255063 | 1.0349 |
| `sms_luminous_orb` | after | 0.264353 | 0.254932 | 1.0370 |

(The "before" rows were rendered on the same worktree with
`src/Library` checked out at the pre-fix commit and the library
rebuilt, then restored.)

**PT+SMS and VCM agree to 0.8 % and 3.7 % respectively, and BOTH
integrators moved by under 0.2 % across the fix.** That is the expected
result, and the reason is worth stating so the next reader does not read
"unchanged" as "untested": in both scenes the SMS caustic lands on a
diffuse floor OUTSIDE the shell, and the eye path from the camera to
that floor point crosses no interface at all, so the debt-30 factor is 1
on it. The chain from the floor point back through the shell to the
interior light is priced by `ManifoldSolver`, which already applied
`(η_i/η_t)²` per refraction.

That is precisely the double-count question: had the new consumer-side
factor also been applied to the SMS chain, these scenes would have moved
by n⁴ at the two shell crossings. They did not, because the two price
**disjoint segments** — PT's walk prices camera → floor, SMS's chain
prices floor → light — and only one of them crosses the shell.


## 9. Cost

One multiply per refraction event, and an equality compare on every
other scatter. Three runs each, same machine, same session, library
rebuilt between the two states.

| scene | before (ms) | after (ms) |
|---|---|---|
| `tidal_stones` pinhole 160×120, 64 spp, PT ts | 456 / 438 / 439 | 461 / 445 / 447 |
| `plank_closeup` 640×480, 48 spp, PT | 19547 / 20153 / 20135 | 19837 / 20010 / 19962 |

Both differences are inside the run-to-run spread of either state.

**Variance note.** PT's stochastic lobe selection at a dielectric delta
vertex (`PTScatterSelectWeight`, `PathTracingIntegrator.cpp`) weighs
reflection vs. transmission by `kray` alone -- `F` vs. `1-F` -- because
`kray` deliberately excludes this factor (§6). The actual contribution
of the two lobes is `F` and `(1-F) · etaScale`, which no longer matches
the selection weights once `etaScale != 1`: the transmission lobe is
now over- or under-selected relative to its true contribution by up to
a factor of `n²`. This costs variance only -- the `RadianceEtaScale(...)
/ selectProb` division at the consumer keeps the estimator unbiased
regardless of how the lobe was picked -- but it is no longer the
contribution-matched (near-optimal) selection PT's single-lobe scheme
was designed to approximate. Folding `etaScale` into
`PTScatterSelectWeight` itself (selecting by `F` vs. `(1-F) · etaScale`)
would restore the match; not done here, since it touches the selection
weighting for every PT delta vertex in the tree, not just the ones this
fix addresses.

## 10. Relationship to `CAUSTIC_PHOTONMAP_NORMALIZATION.md` §9–§11

§9.3 correctly observed that "RISE applies no η² radiance scaling in
transport". §10 then proposed a **merge-side measure rescale** to fix an
apparent VCM over-count, and §11 refuted the whole diagnosis: the
over-count was a cascade of measurement artifacts, and §11.6's standing
prohibition — *"do not implement the §9–§10 η² fix, a constant divide, or
any merge rescale"* — remains correct and is untouched by this work.

**This fix is a different thing.** It is the EYE-SIDE radiance factor
that every integrator lacked, not a rescale of the merge. §11's clean
measurements put the camera **under** the water precisely so no interface
sat on the eye side — which made them blind to this factor, and is why
they were internally consistent and remain valid. See §13 of that
document.

### 10.1 Named residual: the subsurface-scattering family (§6.1)

**Corrected (review round 2, 2026-09-12): "never touches the IOR stack" is
only true of the front-face BSSRDF entry path.** `RandomWalkSSS` and
`BSSRDFSampling::Sw` never touch it (grep confirms zero references), and
neither does `SubSurfaceScatteringSPF`'s front-face entry branch — that
is the ordinary case this section discusses: a BSSRDF entered and exited
through the SAME interface, for which the debt-30 factor is exactly 1 by
the telescoping argument below. But `SubSurfaceScatteringSPF`'s BACK-FACE
exit branch (RGB `SubSurfaceScatteringSPF.cpp` ~line 312-313, NM
~line 512-513) DOES push and pop: `exitRay.ior_stack = new
IORStack(ior_stack); exitRay.ior_stack->pop();`. That branch only fires
for a walk already inside the object at a back-face hit, which is
reachable — `SubSurfaceScatteringMaterial` reports `canRefract = true`
(§6.1's table), so `IORStackSeeding` will seed a camera or light source
that starts inside an SSS object exactly as it would for a dielectric.
For the ordinary case (walk never seeded inside; entry and exit through
the same point) the object was never pushed, so this pop is a logged
no-op and the factor stays 1 — the code is arguably right there, by the
same telescoping argument. It is a REAL `(n_sss/n_out)²` exit factor only
for a walk seeded inside the object, which no existing test exercises.
The residual below is scoped to that telescoping argument (entry and
exit through the same interface); it does not claim anything about the
untested seeded-interior case. Two views on whether the telescoping
scope itself is correct are open, not reconciled by this work:

- **(a) Telescoping argument (the position this fix takes no side
  against).** A BSSRDF is a closed-form solution to the diffusion
  approximation for light entering a semi-infinite slab at one point and
  exiting at another, already integrated over the internal random walk.
  If that closed form is derived (as PBRT-v3's classic dipole is) as an
  air-to-air quantity — the entry Fresnel transmittance and the internal
  radiance-to-flux conversion at entry cancelling algebraically against
  the exit's flux-to-radiance conversion and exit Fresnel transmittance
  — then the net factor over the whole entry-walk-exit trip is exactly
  1 by construction, the same telescoping identity §6's `(n_out/n_gap)²
  · (n_gap/n_below)² = (n_out/n_below)²` states for an explicit
  multi-interface SPF chain. Under this view, RISE's "never touch the
  stack" is not a gap; it is the same telescoping collapsed into a
  single opaque closed form instead of two explicit consumer-side
  multiplies.
- **(b) PBRT's own implementations don't obviously agree with the pure
  telescoping-to-1 reading in (a) either — corrected, review round 2,
  2026-09-12; the citation below is from memory of the sources,
  UNVERIFIED here (no PBRT source tree was checked out this round) and
  should be re-verified against the actual PBRT source before anyone
  leans on it.** An earlier draft of this row cited a single
  `SeparableBSSRDFAdapter::f` that DIVIDES by `eta²` in
  `TransportMode::Radiance`. As best recollected: PBRT-v3's
  `SeparableBSSRDFAdapter::f` (§11.4.3) instead MULTIPLIES —
  `f *= bssrdf->eta * bssrdf->eta` — under `TransportMode::Radiance`; a
  DIVIDE form, `f /= Sqr(eta)`, appears in PBRT-v4 but on a different
  class, `NormalizedFresnelBxDF::f`, with `eta` there being the
  interior/exterior *relative* index rather than a plain absolute IOR.
  These are not restatements of the same fact — a v3-vs-v4 multiply/
  divide flip, on top of different callers with different `eta`
  conventions, is exactly the kind of detail that inverts a sign if
  transcribed carelessly. **Whichever of the two is the accurate
  citation, both are non-trivial η-dependent factors in a radiance-mode
  BSSRDF/Fresnel evaluation that the pure telescoping-to-1 argument in
  (a) does not obviously account for**, so (a) is not settled as
  correct by default. Given the citation uncertainty, this residual's
  conclusion is direction-neutral: **RISE's SSS convention may differ
  from PBRT's by η² in EITHER direction (too bright or too dim) once the
  surrounding medium is not air — settle it with the observable below,
  not with the literature citation.**

Also note a scope gap in the telescoping argument itself: it is stated
for a path that enters AND exits a BSSRDF through the same interface (a
sensor and every light source outside the medium). It says nothing about
a sensor or a light source seeded INSIDE the medium (one crossing, no
telescoping partner) — which is exactly the untested back-face-exit case
in §6.1's second SSS row. That gap is moot for `RandomWalkSSS` /
`BSSRDFSampling::Sw` specifically, because the *entry* path
(`SampleEntryPoint`) is front-face-only by construction — a BSSRDF walk
cannot be entered from inside the medium — so the "sensor/source inside
the medium" case can only ever reach `SubSurfaceScatteringSPF`'s
separate back-face exit branch (§6.1), not the BSSRDF diffusion-profile
path this telescoping argument is about.

**The observable that would settle it**, not yet run: author the same
semi-infinite-slab geometry two ways — once as a `dielectric_material`
shell with a scattering *interior* medium (an explicit multi-interface
SPF chain that DOES take the debt-30 factor at both crossings), once as
`subsurfacescattering_material` on the same shape with matched albedo
and mean free path — and render both under a submerged camera (inside
the shell) AND an air camera (outside it, water or another medium
between camera and shell) with everything else held fixed. If (a) is
right, the two materials' brightness ratio between the submerged-camera
and air-camera renders should be IDENTICAL for both materials (both
telescope to net 1 the same way — no relative shift). If RISE's BSSRDF
diverges from PBRT's convention as described in (b), the
`subsurfacescattering_material` render will differ from the
explicit-interior render by some power of n that tracks the surrounding
medium's index, in a direction that isn't pinned down without checking
the actual PBRT source. Neither render has been produced; this residual
is open.

### 10.2 Named residual: spatially-varying `ior` mismatch (review round 2)

`RadianceEtaScale` (`Utilities/IORStack.h`) reads only `before.top()` and
`after->top()` — the values recorded on the IOR stack at push/pop time.
`DielectricSPF` and `PerfectRefractorSPF` instead price their own
Fresnel/Snell calculation with the `ior` painter's value FRESHLY
RE-FETCHED at the current hit (`DielectricSPF.cpp` ~line 384
`pRIndex->GetValuesAt(ri)`; the equivalent `newIOR` parameter shape in
`PerfectRefractorSPF.cpp`). For a spatially UNIFORM `ior` painter (every
scene in this document's measurements, and every canonical SMS/dielectric
test scene in the tree) those two reads are the same number and the
distinction is invisible. For an `ior` bound to a spatially-varying
`IScalarPainter` — a graded-index object, or any procedural `ior` texture
— the ENTRY hit's pushed value and the EXIT (or an interior bounce's)
freshly-fetched value can differ, and this helper's throughput factor
would then be computed from a different index than the one the SPF's own
Fresnel transmittance was priced against. A full entry-to-exit trip
still telescopes to the physically correct net factor (the same
mismatched entry value cancels against itself at the matching exit), so
this is NOT a bug for the ordinary "object seen from outside, light
outside too" case documented as CORRECT above — it is a residual only for
contributions gathered at a vertex INSIDE such an object (an NEE
connection or a bounce before the walk exits). No scene or test in the
tree currently uses a spatially-varying `ior` painter, so this is
unexercised, not measured to be wrong. Full detail in the
`RadianceEtaScale` doc comment (`IORStack.h`).

### 10.3 Named residual: `TranslucentSPF` guided-direction IOR-stack leak (review round 2, NOT fixed here)

Found while auditing the exit-loop fix (C1 of review round 2) for that
round; independent of the debt-30 eta² factor and recorded here, and as
debt 31(a) in [RENDERING_INTEGRATORS.md](RENDERING_INTEGRATORS.md),
rather than fixed in this pass.

**(review round 3 correction)** An earlier draft of this item claimed
`PathTracingIntegrator.cpp ~line 553` AND `BDPTIntegrator.cpp ~line 161`
"both accept any non-delta `eRayDiffuse`/`eRayReflection` scatter." Only
PT's `GuidingSupportsSurfaceSampling` does that
(`return scat.type == ScatteredRay::eRayDiffuse || scat.type ==
ScatteredRay::eRayReflection;`, gated on `!scat.isDelta`); BDPT's own
`GuidingSupportsSurfaceSampling` (`BDPTIntegrator.cpp` ~line 161) admits
only `eRayDiffuse` (`return !scat.isDelta && scat.type ==
ScatteredRay::eRayDiffuse;`) — `eRayReflection` is excluded there. The
translucent exit lobe (`front`, type `eRayDiffuse`, non-delta) is
admitted by BOTH functions regardless of this difference, since it is
always `eRayDiffuse`, so the leak description below is unaffected by
the correction — only the parenthetical about what else each function
admits was wrong.

- **Guided-direction IOR-stack leak.** The translucent exit lobe
  (`front`, type `eRayDiffuse`, non-delta) is admitted by
  `GuidingSupportsSurfaceSampling` in both integrators (see the
  correction above) even though it carries a POPPED `ior_stack`
  (`TranslucentSPF.cpp` ~line 248). When the path guiding field
  intercepts that vertex and substitutes a guided direction for the
  SPF's own sampled one, PT sets `traceIorStack = &iorStack`
  (`PathTracingIntegrator.cpp` ~line 3205, ~line 3260) — the
  PRE-scatter stack, not the SPF's `pS->ior_stack` — so the exit lobe's
  pop never reaches the continuation ray, and the translucent object
  silently stays on the stack. Every subsequent hit on that object then
  reads `containsCurrent() == true` and misclassifies as "exiting"
  (`bEntering == false`) when it should be entering fresh. Failing
  input: any scene with path guiding enabled and a `translucent_material`
  object, once training has populated enough of the guiding field to
  intercept a sample at that vertex.

**What used to be item (b) here is CLOSED, not a residual.** An earlier
draft described a `ScatteredRayContainer` overflow-only leak in the
entry per-channel loop. Review round 3's audit of that same loop for C1
(this document's §2/§6.1 `TranslucentSPF` discussion) found the leak is
actually far WORSE than overflow-only: it is UNCONDITIONAL on the
SUCCESS path — every anisotropic-N entry `Scatter` call leaked two
`IORStack`s regardless of container occupancy, because the loop never
re-armed `delete_stack` before each new allocation (fixed; see the
`TranslucentSPF.cpp` commit history for this round). Auditing that fix
against the ORIGINAL overflow-only framing then surfaced a narrower
residual the success-path fix alone did not close: after re-arming,
an iteration whose `AddScatteredRay` call FAILS (overflow) still
retains ownership of its own stack, but a SUBSEQUENT (non-last)
iteration's `new IORStack` would silently overwrite that pointer
without freeing it — orphaning it. That was also closed in the same
round (guard-and-free before each reassignment). The exit-side loop
never had either shape: it never assigns `ior_stack` inside its
per-channel loop at all (its only stack-bearing allocation, `front`'s
pop, is a single assignment made ONCE, outside and after both branches
— see `TranslucentSPF.cpp` ~line 257). Debt 31 in
[RENDERING_INTEGRATORS.md](RENDERING_INTEGRATORS.md) keeps only item
(a) (the guided-direction leak above); the former item (b) is removed,
not merely marked fixed, since both of its sub-shapes are now closed by
code, not by documentation.

## 11. Cross-references

- [`tests/RefractiveRadianceScalingTest.cpp`](../tests/RefractiveRadianceScalingTest.cpp) — the closed forms
- [`docs/CAUSTIC_PHOTONMAP_NORMALIZATION.md`](CAUSTIC_PHOTONMAP_NORMALIZATION.md) §11, §13
- [`docs/RENDERING_INTEGRATORS.md`](RENDERING_INTEGRATORS.md) debt 30
- [`docs/SUBMERGED_CAMERA_IOR_SEEDING.md`](SUBMERGED_CAMERA_IOR_SEEDING.md) — the ×n² exit factor
- [`docs/skills/bdpt-vcm-mis-balance.md`](skills/bdpt-vcm-mis-balance.md) step 0
- [`src/Library/Utilities/IORStack.h`](../src/Library/Utilities/IORStack.h) — `RadianceEtaScale`
- [`src/Library/Interfaces/ISPF.h`](../src/Library/Interfaces/ISPF.h) — the `kray` contract
