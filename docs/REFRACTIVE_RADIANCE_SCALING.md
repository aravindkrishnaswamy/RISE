# The η² basic-radiance factor at dielectric interfaces (debt 30, 2026-09-12)

**Status: RESOLVED.** Until 2026-09-12 RISE applied no η² radiance
scaling at any dielectric interface, in any integrator or shader op.
Every RADIANCE-mode walk now multiplies its throughput by
`(η_before / η_after)²` whenever a scattered ray's medium changes;
IMPORTANCE-mode walks (light subpaths, photon tracers, SMS photon seeds,
detector-sphere rigs) deliberately do not.

Guard: [`tests/RefractiveRadianceScalingTest.cpp`](../tests/RefractiveRadianceScalingTest.cpp)
(30 checks, ~27 s), plus `VCMStrategyBalanceTest` topology H (the red row)
and `BDPTStrategyBalanceTest` topology J (the cancellation pin).

---

## 1. The physics

Radiance is **not** invariant along a ray that crosses a smooth interface
between media of different refractive index. The invariant is the *basic
radiance* `L / n²` (Preisendorfer 1965; Veach 1997 §5.2; PBRT-v4 §9.5.2).
Crossing from a medium of index `η_before` into one of index `η_after`,

```
L_after = T(θ) · (η_before / η_after)² · L_before
```

with `T` the Fresnel transmittance. Entering water from air
(`1 → 1.33`) the factor is `1/1.7689`; leaving water for air it is
`1.7689`.

A path tracer that starts at the camera transports radiance *backwards*
along the light's direction of travel, and the convention that falls out
(PBRT's `TransportMode::Radiance`, Mitsuba's `TransportMode`) is: at each
scatter, `η_before` is the index on the side the walk arrived from and
`η_after` the side it leaves for. A light subpath or a photon transports
**importance / flux**, which is conserved across the interface up to
Fresnel — it gets no factor. That asymmetry is not a convention; it *is*
the non-symmetry of refractive scattering, and it is what makes a merge
(a flux-carrying photon paired with a radiance-carrying eye vertex) come
out right.

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
| **BDPT light-tracing splat** | one crossing inward | light subpath (flux) | **×n² too bright** |
| **transparent-shadow NEE** | one crossing inward | straight shadow ray (flux) | **×n² too bright** |
| **emitter INSIDE the refractor, viewed from outside** | one crossing inward | — | **×n² too bright** |
| **camera INSIDE the refractor** | one crossing outward | — | **×1/n² too dim** |

The last two rows are the reference-free ones, and they are what
`RefractiveRadianceScalingTest` rows A and B assert.

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
| tiny sphere emitter, **submerged** | after | 0.004690 | – | 0.004857 | **0.004908** | **1.047** |

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
  **1.047×**. PT and BDPT barely moved (0.004666 → 0.004690,
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
  count will land at a different point inside that band, not on 1.047
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
| `DetectorSpheres/*` | 5 | FLUX | no | measurement rigs; comment only |
| `Materials/SubSurfaceScatteringSPF.cpp`, `Utilities/RandomWalkSSS.{h,cpp}`, `Utilities/BSSRDFSampling.h` (`Sw`) | 0 | — | no — **untouched — telescoping argument; PBRT convention differs; NAMED RESIDUAL** | the whole subsurface-scattering family never pushes/pops the IOR stack (BSSRDF entry/exit is priced by the diffusion-profile importance sampling in the integrator, not by a stack transition), so the debt-30 factor is exactly 1 for it, unconditionally. See §10 for the two competing readings of whether that is correct. |

### 6.2 One hop deeper (audit-by-bug-pattern)

- **Nothing re-prices eye-side throughput from a stored vertex.**
  `PathVertexEval` rebuilds a `RayIntersectionGeometric` from a
  `BDPTVertex` to evaluate BSDFs and pdfs; it never recomputes
  throughput. `StoreThroughput` writes the already-scaled `beta` onto the
  vertex, and connections, splats, VCM merges (`v.throughput` /
  `v.throughputNM` in `VCMIntegrator.cpp`) and MLT re-evaluation all read
  that field. `BDPTVertexRIGRebuildTest` (68 checks) is green.
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

`SubSurfaceScatteringSPF`, `RandomWalkSSS`, and `BSSRDFSampling::Sw`
never touch the IOR stack (§6.1), so the debt-30 factor is exactly 1 on
every subsurface-scattering path, unconditionally. Two views on whether
that is correct are open, not reconciled by this work:

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
- **(b) PBRT-v3's own implementation disagrees with reading (a)
  literally.** `SeparableBSSRDFAdapter::f` (PBRT-v3 §11.4.3) multiplies
  the profile evaluation by `(1 - Fr(cosThetaI)) / (etaI * etaI)` when
  the integrator's transport mode is Radiance — an explicit η² divide
  that mirrors exactly the kind of factor this debt-30 fix adds at every
  other RADIANCE-mode dielectric interface. If PBRT's own radiance-mode
  BSSRDF needs that divide, the pure telescoping-to-1 argument in (a) is
  incomplete for at least PBRT's formulation, and RISE's BSSRDF may be
  undercounting light exiting a subsurface material whose *surrounding*
  medium is not air (e.g. skin submerged in water) by the same n² this
  whole fix is about.

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
telescope to net 1 the same way — no relative shift). If (b) is right,
the `subsurfacescattering_material` render will be dimmer than the
explicit-interior render by the same n² factor the air-camera dielectric
case already exhibits, and by an amount that tracks the surrounding
medium's index. Neither render has been produced; this residual is
open.

## 11. Cross-references

- [`tests/RefractiveRadianceScalingTest.cpp`](../tests/RefractiveRadianceScalingTest.cpp) — the closed forms
- [`docs/CAUSTIC_PHOTONMAP_NORMALIZATION.md`](CAUSTIC_PHOTONMAP_NORMALIZATION.md) §11, §13
- [`docs/RENDERING_INTEGRATORS.md`](RENDERING_INTEGRATORS.md) debt 30
- [`docs/SUBMERGED_CAMERA_IOR_SEEDING.md`](SUBMERGED_CAMERA_IOR_SEEDING.md) — the ×n² exit factor
- [`docs/skills/bdpt-vcm-mis-balance.md`](skills/bdpt-vcm-mis-balance.md) step 0
- [`src/Library/Utilities/IORStack.h`](../src/Library/Utilities/IORStack.h) — `RadianceEtaScale`
- [`src/Library/Interfaces/ISPF.h`](../src/Library/Interfaces/ISPF.h) — the `kray` contract
