# DL-08: PSSMLT stream aliasing (debt 29)

CLOSED 2026-09-17 — fix `d7ebd453` (red-proof test `60994864`). The DL-08
regression run reported `tests/PSSMLTStreamAliasingTest` Test F green
(previously red on master `e290fc64`).

**Review follow-up P1, same day**: `d7ebd453`'s collision fix (raising
`kNumStreams` 49 -> 4096 and moving the reserved stream to 2048, both still
addressed through ONE flat vector) introduced a severe memory/CPU
regression of its own — see "Storage-cost follow-up" below, closed by a
second fix that replaces the flat vector with two-tier storage. That
section also retracts this doc's original Sobol-table analogy and
corrects the `assert`/NDEBUG claim from the original fix's code comment.

## Contract and root cause

`PSSMLTSampler` (`src/Library/Utilities/PSSMLTSampler.{h,cpp}`) stores the
Markov chain's primary sample vector `X` and multiplexes independent
sampling "streams" into it as

```
idx = streamIndex + kNumStreams * sampleIndex
```

This mapping is collision-free **only** while `0 <= streamIndex <
kNumStreams` for every call — it is then a base-`kNumStreams` positional
encoding of `(streamIndex, sampleIndex)`, and the map is injective. A
caller that passes `streamIndex >= kNumStreams` silently breaks that
invariant: it lands on lane `streamIndex % kNumStreams` at sample depth
`streamIndex / kNumStreams`, aliasing whatever legitimately owns that
lane.

`kNumStreams` defaulted to 49 (`kDefaultNumStreams`). `MLTRasterizer.cpp`
/ `MLTSpectralRasterizer.cpp` hardcoded the literal stream `48` for the
film position, lens position, and (debt 28) aperture point —
contiguous `Get2D`/`Get1D` draws on that one stream.

`BDPTIntegrator`'s eye-subpath walk (`GenerateEyeSubpath`, called by
`MLTRasterizer`/`MLTSpectralRasterizer` via
`pIntegrator->GenerateEyeSubpath(...)`, the same generator PT and BDPT's
own rasterizers use) calls `sampler.StartStream( 16u + depth )` once per
loop iteration (`BDPTIntegrator.cpp:1749`), where `depth` is the walk's
plain loop counter — it advances once per bounce, surface or volume,
regardless of what kind of vertex resulted. `StabilityConfig`'s
`maxVolumeBounce` defaults to 64
(`src/Library/Utilities/StabilityConfig.h`), so an ordinary
scattering-medium scene reaches eye depth 32 with **no unusual
settings at all**.

At eye depth 32, `16 + 32 == 48` — the exact literal MLT's rasterizers
reserved. This is **not** the modular wraparound aliasing that a small
`kNumStreams` normally produces (that starts one bounce later, at eye
depth 33: stream 49, lane `49 % 49 == 0`, i.e. stream 0's second sample).
It is a literal same-integer collision between two semantically
unrelated sampling dimensions: a chain whose accepted path reaches eye
depth 32 has its 32nd-bounce scattering direction and its film position
living in the **exact same** primary-sample-vector slot. A small PSSMLT
film-position mutation silently perturbs the 32nd bounce's BSDF sample;
accepting a path with a different 32nd-bounce outcome silently moves the
film position. Deeper bounces (depth 33, 34, ...) additionally alias
onto stream 0, 1, ... at later sample indices, coupling the film/lens/
aperture draws to the light-source and early-bounce dimensions too.

Only `mlt_rasterizer` / `mlt_spectral_rasterizer` are affected: PT,
BDPT (run directly, not via MLT), and VCM never construct a
`PSSMLTSampler` (confirmed by `grep -rl PSSMLTSampler src/Library` —
only `MLTRasterizer.{h,cpp}` / `MLTSpectralRasterizer.{h,cpp}`
reference the class). BDPTIntegrator's own `StartStream(16u+depth)` /
`StartStream(1u+depth)` calls are shared code, driven by a `SobolSampler`
or `IndependentSampler` everywhere except under MLT — and both of those
samplers have effectively unbounded stream capacity (see the sibling
audit below), so this defect is specific to the PSSMLT lane table.

## The fix

Two new constants in `BDPTCameraUtilities` (`src/Library/Cameras/CameraUtilities.h`):

- `kMaxBdptWalkStreamUnderPSSMLT = 16 + 1024`. `BDPTIntegrator`'s eye
  and light walk loops both saturate their iteration count at 1024
  (`maxEyeTotalDepth`/`maxLightTotalDepth`'s ternary, guarding against
  `maxVolumeBounce == UINT_MAX` wrapping the sum), so the eye walk's own
  `StartStream(16u+depth)` can reach at most stream `16+1023 = 1039`; the
  light walk reaches at most `1+1023 = 1024`; the BDPT `(s,t)` strategy
  select is a fixed stream 47. `16 + 1024 = 1040` is the same one-off
  margin `tests/SobolDimensionBudgetTest.cpp`'s
  `TestApertureDrawConsumption` already uses when it derives
  `kMaxEyeWalkStream` for its own (unrelated, Sobol-only) purposes — reused
  here for a documented, provable ceiling rather than a fresh guess.
  Note this is **not** the same bound `kApertureSamplerStream` (3322)
  clears — that constant also has to clear VCM's `48+eye-vertex-index`
  NEE stream (up to 3121 in the worst theoretical case), because it is
  usable from Sobol/Independent, which VCM does drive. VCM never drives
  a `PSSMLTSampler`, so `kMaxBdptWalkStreamUnderPSSMLT` only needs to
  clear BDPT's own two walks.
- `kPSSMLTFilmLensApertureStream = 2048`. Comfortably above
  `kMaxBdptWalkStreamUnderPSSMLT`, with headroom for future per-stream
  lanes (e.g. a wider spectral wavelength count) to be added without
  re-deriving this constant.

`PSSMLTSampler::kDefaultNumStreams` raised from 49 to 4096 (comfortably
above the new reserved stream). `MLTRasterizer.cpp` /
`MLTSpectralRasterizer.cpp`'s `sampler.StartStream( 48 )` calls became
`sampler.StartStream( BDPTCameraUtilities::kPSSMLTFilmLensApertureStream )`.
`PSSMLTSampler::Get1D()` now asserts `streamIndex` is in
`[0, kNumStreams)` at the one place `idx` is actually computed
(`assert`, described at the time as "debug builds only ... no runtime
cost in a release build" — **this characterization was wrong; see
"Storage-cost follow-up" below, which replaced the assert with an
always-on check for exactly this reason**), so a future violation of
this invariant fails loudly during development instead of silently
aliasing a lane.

This is a pure **re-numbering**, not an algorithmic change to PSSMLT's
mutation strategy: a given `(streamIndex, sampleIndex)` pair still maps
to exactly one, permanently-owned primary-sample-vector slot for the
life of a `PSSMLTSampler` instance, which is all Kelemen's small-step
mutation needs (see `PSSMLTSampler.h`'s class-level `INVARIANTS` comment
— unchanged). Every MLT render's specific noise pattern for a fixed seed
changes (the film/lens/aperture draws now land at different `idx` values
than before), but this is expected and statistically benign for shallow
paths that never reached the old collision — see Verification below.

## Reproduction and verification

The isolated branch started at master `e290fc64`. Test commit `60994864`
(`tests/PSSMLTStreamAliasingTest.cpp` Test F) was built and run against
that unfixed library before any source edit. The executable exited 1:

```text
Test F: Deep eye-walk stream aliasing (debt 29 / DL-08)
  FAIL: eye-walk stream 48 (16 + eye depth 32) and MLT's reserved stream 48
  produced 6/6 identical values -- they are the SAME primary-sample-vector
  lane.  A PSSMLT film-position mutation is aliased with the eye walk's
  32th-bounce scattering direction (debt 29 / DL-08): MLTRasterizer's
  reserved stream must sit strictly above every stream BDPTIntegrator's
  own StartStream calls can reach under PSSMLTSampler.
```

Test F draws two identically-seeded `PSSMLTSampler` instances directly
(no scene or render): one replays `StartStream(16+32)` (what the eye
walk computes at depth 32), the other `StartStream(48)` (MLT's pre-fix
literal). `PSSMLTSampler::Get1D`'s per-lane value is a pure function of
`(seed, streamIndex, sampleIndex)`, so two samplers seeded identically
and driven on the same stream number must — and, pre-fix, do — produce
bit-identical sequences.

After the fix (fix commit `d7ebd453`), the same test:

```text
Test F: Deep eye-walk stream aliasing (debt 29 / DL-08)
  Eye-walk stream 48 (eye depth 32) vs MLT reserved stream 2048: independent (0/6 matches)
  Eye-walk streams 32..130 (as 16+depth) vs streams {0,1,16,47,2048}: no collisions
  Passed!
```

`tests/PSSMLTStreamAliasingTest`'s Tests A-E (pre-existing) were updated
to read the reserved stream via `BDPTCameraUtilities::kPSSMLTFilmLensApertureStream`
instead of the retired literal 48, and Test C gained the DL-08 safety
property itself (`kPSSMLTFilmLensApertureStream > kMaxBdptWalkStreamUnderPSSMLT`
and `< kDefaultNumStreams`); full suite green, clean rebuild, zero
warnings.

Gate suites unaffected (none drive a `PSSMLTSampler`, confirming the
fix's blast radius is BDPT-under-MLT only):
`SobolDimensionBudgetTest` (unchanged — `kApertureSamplerStream`'s own
derivation does not reference any PSSMLT constant), `SourceHygieneTest`
165/0, `CstDeriveGoldenTest` 452 MATCH / 0 DRIFT, `BDPTStrategyBalanceTest`
66/0, `VCMStrategyBalanceTest` 55/0, `EnvLightBalanceTest` 116/0,
`RefractiveRadianceScalingTest` 38/0.

No MLT scene render was needed to demonstrate the aliasing (the bug is
proven at the sampler-arithmetic level, independent of what BDPT or
MLTRasterizer do with the returned values), and reproducing the
render-level symptom would require a deep-volume MLT render (`spp` in
the thousands for chain convergence) whose wall-clock cost was not
justified once the closed-form sampler-level proof existed; the
render-level claim ("spatially-varying bias / shifted-shadow-class
symptom on any MLT chain deep enough to reach eye depth 32") follows
directly from the same PSSMLT invariant `PSSMLTStreamAliasingTest`'s
header comment already documents for the ORIGINAL (kNumStreams=3)
shifted-shadow bug this file's test suite exists to guard against.

## Render-level proof (2026-09-27)

> **Measurement conventions (added at merge, 2026-09-27 review P3s).** Every "mean" in this section is the Rec.709 luma `0.2126 R + 0.7152 G + 0.0722 B` of the linear EXR, the renderer's own luminance convention; a plain `(R+G+B)/3` reproduction reads ~2% off and is not a discrepancy. The PT reference figures (2.858 RGB / 3.013 spectral) are single unseeded runs; an independent re-render at the same budget read 2.515 / 3.133, i.e. ~10-15% run-to-run spread on this heavy-tailed scene, two orders of magnitude below the 60-900x MLT/PT gap discussed here. That gap is PSSMLT's image mean being pegged to its bootstrap mean-luminance estimate `b_mean` (verified: the log's `b_mean` equals the resolved EXR luma to 6 significant figures), a small-N bootstrap of a heavy-tailed distribution reading low on BOTH builds -- quote `b_mean` beside any MLT-vs-PT ratio.

The paragraph above declined a render-level demonstration as "not
justified" given the closed-form sampler-arithmetic proof already
existed.  This section supplies that demonstration anyway, on a
purpose-built deep-volume MLT scene, and reports the result honestly
whichever way it came out.

**Scene**: `scenes/Tests/MLT/mlt_deep_fog.RISEscene` (RGB,
`mlt_rasterizer`) and its spectral twin `mlt_deep_fog_spectral.RISEscene`
(`mlt_spectral_rasterizer`).  A small (80-unit) closed Lambertian box,
entirely filled with a dense isotropic-scattering `homogeneous_medium`
(`absorption 0.02`, `scattering 0.98` -- mean free path 1 unit, two
orders of magnitude shorter than the box), lit by one small ceiling
emitter (`lambertian_luminaire_material`, `scale 1000000`).
`max_eye_depth`/`max_light_depth` are kept at 3 so almost all loop-depth
accumulation comes from medium scatters (which do NOT count toward the
surface-bounce cap -- see `BDPTIntegrator.cpp`'s
`eyeSurfaceBounces`/`depth` distinction above), driving the eye
subpath's `StartStream(16+depth)` counter well past the historical
collision depth of 32 on the great majority of subpaths.
`bootstrap_samples 3000`, `chains 24`, `mutations_per_pixel 24`; film
64x64; `oidn_denoise FALSE`, `pixel_filter box`; output `EXR`
`Rec709RGB_Linear`.  No translucent/SSS material.

**Depth >= 32 fraction (measured, not argued)**: a temporary atomic
counter pair was added to `BDPTIntegrator::GenerateEyeSubpathImpl`
(scratch commit `2c7c3e03`, reverted in a following commit before this
slice's final state) counting, per eye subpath generated, whether the
per-bounce loop counter `depth` ever reaches 32.  Rendering
`mlt_deep_fog.RISEscene` at its shipped settings on the post-fix
library: **101328 total eye subpaths generated (bootstrap + chain
mutations combined), 99451 reaching depth >= 32 -- 98.1%.**  The
fraction stays in the 98.0-98.2% band across every `bootstrap_samples`
value tried (3000-6000) and under both the RGB and spectral
rasterizers.

**Render sweep**: both scenes were rendered on BOTH builds -- pre-fix
(`e290fc64`, in a second worktree) and post-fix (this slice's HEAD) --
INTERLEAVED (pre, post, pre, post, ...), n=4 repeats each, under both
`mlt_rasterizer` and `mlt_spectral_rasterizer`.  `MLTRasterizer`'s
bootstrap/chain seeds are index-deterministic
(`bootstrapSamples[i].seed = i`; `RandomNumberGenerator
selRNG(c*31337)` in `InitChain`), so identical settings reproduce a
bit-identical render every time; repeats were varied by changing
`bootstrap_samples`, in two separate sweeps:

- **"narrow" sweep**: `bootstrap_samples` = 3000, 3001, 3002, 3003 (a
  minimal +1 variation -- these four repeats explore a
  nearly-identical bootstrap population and are highly correlated with
  each other; useful for isolating the code-caused difference from
  noise at a FIXED sampling population, but not representative of
  independent-trial variance).
- **"wide" sweep**: `bootstrap_samples` = 3000, 4000, 5000, 6000 (a
  materially different, largely-independent population each time).

EXRs were read with `OpenEXR`/`numpy`; "centre" = the middle 50%x50%
of the 64x64 frame, "edge" = the surrounding ring; Welch's t is
between the n=4 POST values and the n=4 PRE values.

**RGB (`mlt_rasterizer`), narrow sweep:**

| region | PRE mean | PRE sd | POST mean | POST sd | rel diff | Welch t |
|---|---|---|---|---|---|---|
| whole-image | 0.043627 | 0.000019 | 0.045424 | 0.000020 | +4.12% | +133.0 |
| centre (50%) | 0.032944 | 0.000014 | 0.050855 | 0.000021 | +54.37% | +1400.3 |
| edge (ring) | 0.047188 | 0.000020 | 0.043614 | 0.000019 | -7.57% | -258.2 |

**RGB, wide sweep:**

| region | PRE mean | PRE sd | POST mean | POST sd | rel diff | Welch t |
|---|---|---|---|---|---|---|
| whole-image | 0.040270 | 0.002779 | 0.038304 | 0.005838 | -4.88% | -0.61 (n.s.) |
| centre (50%) | 0.033709 | 0.002673 | 0.032927 | 0.012167 | -2.32% | -0.13 (n.s.) |
| edge (ring) | 0.042457 | 0.003825 | 0.040097 | 0.005607 | -5.56% | -0.70 (n.s.) |

**Spectral (`mlt_spectral_rasterizer`), narrow sweep:**

| region | PRE mean | PRE sd | POST mean | POST sd | rel diff | Welch t |
|---|---|---|---|---|---|---|
| whole-image | 0.005262 | 0.000002 | 0.014057 | 0.000006 | +167.15% | +2721.9 |
| centre (50%) | 0.004442 | 0.000002 | 0.011766 | 0.000005 | +164.88% | +2676.9 |
| edge (ring) | 0.005535 | 0.000002 | 0.014821 | 0.000006 | +167.76% | +2732.6 |

**Spectral, wide sweep:**

| region | PRE mean | PRE sd | POST mean | POST sd | rel diff | Welch t |
|---|---|---|---|---|---|---|
| whole-image | 0.003959 | 0.000962 | 0.011713 | 0.001899 | +195.88% | +7.29 |
| centre (50%) | 0.003645 | 0.000609 | 0.010353 | 0.001164 | +184.03% | +10.22 |
| edge (ring) | 0.004063 | 0.001089 | 0.012166 | 0.002321 | +199.43% | +6.32 |

Per-repeat spectral whole-image ratios (POST/PRE), wide sweep: 2.67x,
3.01x, 3.22x, 3.10x -- POST reads at least 2.67x PRE on every one of
the four widely-spread, largely-independent trials; the sign never
flips.  Absolute-difference image (narrow sweep, repeat 0, whole
64x64 luma): RGB mean |diff| = 0.084215, max = 7.461167; spectral mean
|diff| = 0.018618, max = 1.507747 -- both far above this scene's own
per-repeat sd, i.e. a structured difference, not read noise.

**PT reference** (reference-free -- PT never constructs a
`PSSMLTSampler`): `pathtracing_pel_rasterizer` (4096 spp) and
`pathtracing_spectral_rasterizer` (2048 spp), both `oidn_denoise
FALSE`/`pixel_filter box`, same geometry/medium (only the integrator
chunk swapped), built with the post-fix library (PT is unaffected
either way):

| | whole | centre | edge |
|---|---|---|---|
| PT RGB (4096spp) | 2.858056 | 2.729586 | 2.900879 |
| PT spectral (2048spp) | 3.013022 | 2.269985 | 3.260701 |

Both MLT builds are FAR below this truth at the deliberately cheap
settings chosen for fast iteration here (RGB: PRE/POST both ~60-75x
dim; spectral: PRE ~570-960x dim, POST ~210-310x dim) -- an MLT
convergence-BUDGET limitation, ORTHOGONAL to DL-08 (an albedo-0.98,
optically-very-thick scene needs vastly more bootstrap/chain/mutation
budget than these speed-oriented settings for the Markov chain to
discover the dominant high-throughput deep-scattering path
population; neither PRE nor POST is converged, and the fix neither
closes nor should close that gap).  Within that shared limitation,
POST moves consistently CLOSER to the PT truth on spectral (mean gap
shrinks from ~570-960x to ~210-310x -- the fix's effect points toward
correctness, not away from it); on RGB the centre/edge ratio (PT:
0.941, near-flat) is 0.698 for PRE-narrow and 1.166 for POST-narrow --
POST's ratio sits modestly closer to PT's (0.225 away vs 0.243 away),
though neither reading is genuinely close.

**Verdict**: the aliasing has a substantial, render-visible effect at
an attainable production setting (a deep-volume MLT scene) --
confirmed by measurement, not merely argued from the sampler-level
proof.  Under `mlt_spectral_rasterizer` the effect is large AND robust
to independent-ish resampling: POST reads at least 2.67x PRE's
whole-image mean on every one of 4 widely-spread `bootstrap_samples`
trials (wide-sweep Welch t=+7.29; narrow-sweep t=+2721.9).  Under
`mlt_rasterizer` (RGB), a FIXED bootstrap population shows a large,
exactly-reproducible SPATIAL energy redistribution (centre +54.37%,
edge -7.57%, |t|>250 both) -- a real, deterministic consequence of
which specific pixels the depth>=32 chains land on -- but the net
WHOLE-IMAGE mean shift this produces (+4.12% on the narrow sweep) is
NOT distinguishable from this scene's own large inherent MLT
run-to-run variance once the bootstrap population is varied
independently (wide-sweep whole-image Welch t=-0.61, not significant
at n=4).  Both readings are reported as measured: RGB's whole-image
mean washes into this scene's own MC noise under independent
resampling at this small a sample budget, while its per-fixed-seed
spatial redistribution and spectral's whole-image brightness shift are
both large and statistically overwhelming.

## Sibling audit

Bug pattern: **a fixed-size, dense (non-hashed) lane table whose
address is a caller-supplied integer with no enforced upper bound will
silently alias once that integer reaches the table's width.**

| Sampler | Lane/stream table | Reachable stream ceiling vs table width | Status |
|---|---|---|---|
| `PSSMLTSampler` | Dense, `idx = stream + kNumStreams*sample`, `kNumStreams` was 49 | BDPT eye walk reaches stream 1039 under PSSMLT (see above) — **1039 > 48** | **FIXED** this row: `kNumStreams` -> 4096, MLT's reserved stream -> 2048 |
| `SobolSampler` / `ZSobolSampler` | `dimension = stream*32`, finite table `kNumDimensions = 8192` (256 streams) since DL-81 | Shipped-scene PER-VERTEX ceiling (stream 241, `SobolDimensionBudgetTest` Test G1 -- Test G before DL-283 split it; the PT volume-walk and BDPT/VCM medium-distance streams live past the table by design, Test G2) is inside the table; a stream past it does not hard-alias — DL-81 made `Get1D` **Owen-permute by wrap count** past the table (decorrelates) and `Get2D` a padded, raw-dimension-keyed pair (never wraps at all) | Not vulnerable to *this* pattern — DL-81 already replaced hard aliasing with graceful degradation. `ZSobolSampler` inherits `SobolSampler::StartStream`/`Get1D`/`Get2D` unchanged (only `sampleIndex`'s Morton remap differs), so the same conclusion applies without a separate audit. |
| `IndependentSampler` | None — `StartStream` is a documented no-op, every draw is a fresh `RandomNumberGenerator::CanonicalRandom()` | N/A | Immune by construction: there is no lane concept to alias. |
| Legacy `PixelBased{Pel,SpectralIntegrating}Rasterizer`, `PathTracingPelRasterizer` | Use `SobolSampler`/`ZSobolSampler` exclusively (`grep -n "SobolSampler\|ZSobolSampler" src/Library/Rendering/PixelBased*.cpp`) — never `PSSMLTSampler` | Inherits the Sobol row's conclusion | Not applicable; these rasterizers are direct-only (no MLT chain) per the existing "legacy pixelpel is direct-only BY DESIGN" note. |
| Photon tracers (`PhotonTracer.h` and its `{Global,Caustic,Translucent,Shadow}{Pel,Spectral}PhotonTracer` subclasses) | None — hold raw `RandomNumberGenerator geomsampler`/`random` members, no `ISampler`/`StartStream` abstraction at all | N/A | Immune by construction: no stream/lane concept exists on this path. |
| `MMLTSampler` | Referenced only in `PSSMLTSampler.h`'s doc comments ("subclasses reserve more") | N/A | **Does not exist in the tree** (`grep -rn "class MMLTSampler\|: PSSMLTSampler" src/ tests/` finds nothing but the doc comment itself) — aspirational text, not a real sibling to audit. The doc comment was rewritten as part of this fix to stop implying a concrete reservation scheme (see `PSSMLTSampler.h`). |

`PSSMLTSampler` was the only sampler in the tree exhibiting this pattern.
`SobolSampler`'s finite-table risk is the same *shape* of bug but was
independently found and closed by DL-81 (2026-09-14, before this row);
its fix (graceful Owen-permuted decorrelation past the table, rather
than PSSMLT's harder collision) is a different resolution appropriate to
a different consumer (Sobol dimensions are read-only per draw; PSSMLT's
primary-sample vector is read-write and must remain a stable bijection
for the life of a chain, so "decorrelate past the boundary" is not an
available option — the boundary itself has to move).

## Notes / corrections to the ledger row

- The ledger row's evidence cites `BDPTIntegrator.cpp:1732`; on this
  worktree's checkout of `e290fc64` the `StartStream( 16u + depth )`
  call is at line 1749 (a handful of unrelated lines were added earlier
  in the file since the row was written; the cited line number is
  stale, the symbol is correct).
- `docs/RENDERING_INTEGRATORS.md`'s prior "Debt 29" write-up asserted
  BDPTIntegrator's eye walk is bounded by `48 + (3*1024+1) = 3121` in
  the worst case (a bound that conflates BDPT's own walk streams with
  VCM's separate `48+i` NEE stream, which never reaches a
  `PSSMLTSampler`). Re-deriving BDPT's OWN two walks in isolation (the
  only ones that matter for this fix) gives the tighter `16 + 1024 =
  1040` used above; the wider 3121 figure remains correct as the bound
  for `kApertureSamplerStream` (which does need to clear VCM's stream
  too, since Sobol/Independent are shared with VCM), and is left
  unchanged there.

## Storage-cost follow-up (2026-09-17, debt 29 review P1)

### The bug

`d7ebd453`'s collision fix kept the ORIGINAL single flat vector `X`,
addressed as `idx = streamIndex + kNumStreams * sampleIndex`, and just
raised `kNumStreams` (49 -> 4096) and moved the reserved stream (48 ->
2048) to clear BDPT's own reachable ceiling. That mapping's lazy-grow
loop (`while (idx >= X.size()) X.push_back(...)`) materialises EVERY
slot up to `idx`, so its cost is proportional to `streamIndex *
kNumStreams`, not to how many samples were actually drawn. Once the
reserved stream and `kNumStreams` both grew by ~80x, an unused/rarely-
used stream stopped being cheap: the film/lens/(debt 28) aperture
block's first 6 draws (stream 2048, sample 0..5) require `idx = 2048 +
4096*5 = 22528`, i.e. materialising **22529** `PrimarySample` slots
(sizeof(PrimarySample) == 24 on this platform -> 540696 bytes, ~528
KB) on EVERY fresh `PSSMLTSampler` -- independently confirmed by
building a throwaway probe directly against the unfixed library at
commit `8080cd2a`:

```text
kPSSMLTFilmLensApertureStream = 2048
After 6 draws on reserved stream: Slots=22529 Bytes=540696
```

vs 294 slots (~6.9 KB) before `d7ebd453` (kNumStreams=49, reserved
stream=48, `idx = 48 + 49*5 = 293`). `MLTRasterizer.cpp`'s bootstrap
phase constructs and destroys ONE `PSSMLTSampler` PER bootstrap sample
(100,000 by default on `scenes/Tests/MLT/cornellbox_mlt_fast.RISEscene`),
so this cost was paid 100,000 times over on every bootstrap pass of
every shipped MLT scene. Measured bootstrap wall time on that scene (3
runs each, this worktree, `RISE_MEDIA_PATH` set, `printf "render\nquit\n"
| ./bin/rise ...`, `MLTRasterizer:: Bootstrap took N ms` from the log):

| Build | Run 1 | Run 2 | Run 3 | Mean |
|---|---|---|---|---|
| base (`e290fc64`, pre-DL-08 entirely) | 2537 ms | 2613 ms | 2819 ms | ~2656 ms |
| `8080cd2a` (post-collision-fix, pre-storage-fix -- the P1) | 10411 ms | 10436 ms | 9889 ms | ~10245 ms |
| this fix | 1777 ms | 1809 ms | 1816 ms | ~1801 ms |

`8080cd2a` is a ~3.9x bootstrap regression vs base on this scene (worse
on `mlt_torus_chain_atrium.RISEscene`'s 1M-sample bootstrap, per the
review). The bootstrap-time-only comparison is deliberate: it isolates
the `PSSMLTSampler` construction/materialisation cost the review
flagged from mutation-phase and render-phase costs the fix does not
touch.

**Retracted claim**: the original fix's code comment justified raising
`kNumStreams` by analogy to `SobolSampler`'s `kNumDimensions` table
("just a bigger constant, like Sobol's"). This analogy is WRONG.
`SobolSampler`'s direction-number table (`SobolDirectionNumbers.cpp`,
DL-81) is ONE GLOBAL ~1 MiB array, shared by every sampler instance and
built ONCE per process. `PSSMLTSampler::X` is PER-INSTANCE, and under
the flat-vector design its materialised size scales with
`reservedStreamIndex * kNumStreams * samplesDrawn` — raising
`kNumStreams` does not enlarge a shared table, it multiplies the
per-draw cost of EVERY stream, including the ordinary ones at 0-47.

### The fix: two-tier storage, not one flat vector

- `X`: the ORIGINAL flat vector, but its row width is pinned to
  `kLegacyNumStreams = 49` **forever** (not tied to the sanity-bound
  `kNumStreams`) — used only while `streamIndex < kLegacyNumStreams`.
  A chain that only ever touches streams 0-48 runs the IDENTICAL
  formula on the IDENTICAL row width the base commit used, so it
  reproduces the base commit's draws bit-for-bit — see the
  determinism red-proof below.
- `XExtra`: a small `(stream, vector<PrimarySample>)` association list,
  searched linearly and grown by `FindOrCreateExtraStream()`, used for
  `streamIndex >= kLegacyNumStreams` (the MLT reserved stream at 2048,
  and any BDPT eye-walk depth deep enough to pass 48). Touching stream
  2048 for N samples now costs exactly N `PrimarySample` slots,
  independent of the stream's numeric value — the specific value 2048
  no longer has to be load-bearing for collision safety (any two
  distinct extra-tier streams get separate storage regardless of
  magnitude), though it stays where DL-08 placed it for clarity.

  A `std::unordered_map<int, std::vector<PrimarySample>>` was tried
  FIRST and rejected on measurement: `MLTRasterizer.cpp`'s bootstrap
  loop constructs and destroys 100,000 `PSSMLTSampler`s, each paying a
  hash table's bucket-array allocation on its first insert, and this
  measured **~2.2x SLOWER than even the pre-DL-08 base commit**
  (3910 ms bootstrap vs base's ~2656 ms mean) despite fixing the O(N)
  memory blow-up. Since production has exactly ONE extra-tier stream
  per instance for nearly every render (BDPT-under-MLT rarely reaches
  eye/light depth 33+), a linear-scan association list is both simpler
  and measurably faster than a hash map here — see the `XExtra` field
  comment in `PSSMLTSampler.h` for the full reasoning and a note on
  when to revisit it (many distinct extra streams per instance, not a
  concern for the two known consumers today).

  `Accept()`/`Reject()`'s `modifiedIndices` bookkeeping now records a
  small `ModifiedLane` tag (which tier, plus that tier's own index) per
  touched sample instead of a single flat index, so rollback finds the
  right storage without re-deriving stream/sampleIndex arithmetic; the
  mutation semantics (backup/restore, `lastModIteration`/
  `backupIteration`, the lazy large-step catch-up) are unchanged --
  only WHERE a `PrimarySample` lives changed, not how it is mutated.

### P2: the `assert`'s "no runtime cost in a release build" claim was false

`d7ebd453`'s comment characterized the bounds check as "debug builds
only ... no runtime cost in a release build". This conflates `assert`
with `NDEBUG`, and `NDEBUG`'s definition is platform/config-specific:
`Config.OSX` and `Config.Linux` (`build/make/rise/`) **never** define
it (confirmed: `grep -n NDEBUG build/make/rise/Config.OSX
build/make/rise/Config.Linux` returns nothing), so an `assert` here was
ALREADY active in every mac/Linux configuration including
Deployment/Opto — the "no cost in release" framing was backwards for
those platforms (the checked comparison is a cheap always-there cost,
which is fine, just not what the comment said). Only VS2022's
`Release|x64` config defines `NDEBUG` (confirmed:
`build/VS2022/Library/Library.vcxproj`'s `Release|x64`
`ItemDefinitionGroup` sets `NDEBUG;WIN32;...`; `Debug|x64` sets
`_DEBUG` instead), so an `assert` guard would vanish specifically --
and only -- on Windows Release, the one configuration where a runaway
`streamIndex` would then go uncaught.

Fixed by replacing the `assert` with an always-on `if` + `fprintf`
+ `abort()` in `PSSMLTSampler::Get1D()` (one branch per call, negligible
next to the `RandomNumberGenerator` draw and `PrimarySample` bookkeeping
the function already does) — see its comment for the corrected
rationale. With the two-tier storage above, the bound is no longer a
collision-avoidance requirement (no stream number can alias another,
at any magnitude); it now exists purely to catch a programming error
(a negative or overflowed `streamIndex`) loudly.

### Reproducing the reviewer's "no shipped scene reaches depth 32" finding

Confirmed independently. `grep -rl "mlt_rasterizer\|mlt_spectral_rasterizer"
scenes/` finds 10 shipped MLT scenes (`scenes/FeatureBased/MLT/*.RISEscene`
x4, `scenes/Tests/MLT/*.RISEscene` x3, `scenes/Tests/Spectral/
hwss_mlt_spectral_cornellbox.RISEscene`, `scenes/Tests/Samplers/
cornell_mlt.RISEscene`, plus two incidental substring matches that are not
actually MLT scenes). Every one sets `max_eye_depth` <= 12
(`mlt_veach_egg.RISEscene`: 4; `cornellbox_mlt_fast.RISEscene` /
`cornellbox_mlt.RISEscene`: 4; `hwss_mlt_spectral_cornellbox.RISEscene` /
`cornell_mlt.RISEscene`: 5; `mlt_keyhole.RISEscene`: 8;
`mlt_reflected_caustic.RISEscene` / `mlt_torus_chain_atrium.RISEscene`: 10;
`mlt_luminous_orb.RISEscene` / `mlt_caustic_chain.RISEscene`: 12), and NONE
of the 10 contains a volumetric medium (`grep -ciE
"medium|volume_material|scattering_coefficient|homogeneous|heterogeneous"`
returns 0 for every file). `BDPTIntegrator.cpp`'s eye-walk loop breaks at
`eyeSurfaceBounces >= maxEyeDepth` for every bounce that is NOT a medium
scatter event (`if (eyeSurfaceBounces >= maxEyeDepth) { break; }`), and the
medium-scatter branch is gated on `MediumTracking::GetCurrentMediumWithObject`
returning a non-null medium — which it never does when the scene has none.
So for every shipped MLT scene, the eye-walk's `depth` loop counter
(`StartStream(16u+depth)`) is bounded by `max_eye_depth` alone (<= 12),
giving a maximum reachable stream of `16+12=28` — far short of stream 33
(where even the historical `kNumStreams=49` modular aliasing would first
recur) and stream 48 (the literal pre-`d7ebd453` collision). **The DL-08
collision bug, and by extension this storage-cost P1, were both latent for
every scene the repository ships** — real for user-authored deep-volume
MLT scenes (`StabilityConfig::maxVolumeBounce` defaults to 64, so a scene
that adds a medium reaches eye depth 32 with no unusual settings), but not
reproducible from the shipped corpus alone. This matches, and gives a
concrete mechanism for, the original fix's own doc note: "No MLT scene
render was needed to demonstrate the aliasing... the render-level claim
follows directly from the same PSSMLT invariant."

### Red-proof

`tests/PSSMLTStreamAliasingTest.cpp` gained two new tests plus a fix to
Test F's second probe (see below):

- **Test G** (`TestStorageCostRedProof`): a `PSSMLTStorageProbe` (a
  `PSSMLTSampler` subclass exposing `MaterializedSlotCount()` /
  `ExtraStreamCount()` — the storage is protected on purpose, callers
  have no business inspecting the layout, but this red-proof needs to
  observe it the same way the reviewer's in-process harness did) drives
  6 draws on the reserved stream and asserts exactly 6 materialised
  slots across exactly 1 extra-tier entry:
  ```text
  After 6 draws on reserved stream 2048: 6 PrimarySample slots (144 bytes)
  across 1 extra-tier stream(s) (pre-storage-fix: 22529 slots / 540696 bytes)
  ```
- **Test H** (`TestLegacyLayoutDeterminism`), two parts:
  - Part 1 drives a real `PSSMLTSampler` and an independent, from-scratch
    reimplementation of the pre-storage-fix algorithm
    (`ReferenceLegacyPSSMLT` — NOT a `PSSMLTSampler` subclass, so it does
    not share the two-tier storage this test exists to validate) through
    an identical script (streams {0,1,16,47}, 25 iterations, varying
    per-stream draw counts, mixed accept/reject) and asserts every draw
    matches bit-for-bit: `328/328` matched. This is the "shallow scenes'
    chains are unchanged" property in its exact, provable form: any BDPT
    walk that never reaches eye depth 33 uses ONLY streams < 49, and this
    test proves those draws are unaffected by the storage change.
  - Part 2 drives two identically-seeded real `PSSMLTSampler`s through a
    script spanning BOTH tiers (streams {0,16,47,2048,200}) and asserts
    the two runs match bit-for-bit (`200/200`) — the two-tier layout
    itself is deterministic.
- Test F's second probe (the "eye-walk vs low streams" sweep, depths
  32..130) previously compared two SEPARATELY-CONSTRUCTED, identically-
  seeded, virgin `PSSMLTSampler`s — a valid way to probe the single flat
  vector's `idx` arithmetic (a virgin sampler's first Get1D() call always
  consumes exactly `idx+1` RNG draws under that scheme, so two different
  idx values land on different RNG positions regardless of instance).
  It is NOT a valid probe for two-tier storage: an extra-tier stream's
  first-ever draw on a virgin instance is simply "this instance's first
  RNG call", identical by construction to any OTHER virgin same-seed
  instance's own first draw regardless of which stream either one asked
  for — comparing across instances flagged that expected coincidence as
  a false "collision" (`Get1D()` on the fixed sampler reported one, real,
  observed false-positive: "eye-walk stream 49 (depth 33) collides with
  stream 0 sample 0" during this fix's own development). Rewritten to
  touch every stream through ONE shared instance via `StartStream()`
  switching, exactly as BDPT+MLT's real usage does — the property that
  actually matters (no two distinct lanes of the SAME instance ever
  share a `PrimarySample`) rather than an artifact of comparing isolated
  instances.

### What is, and is not, bit-identical at the render level

`cornellbox_mlt_fast.RISEscene`'s bootstrap "Mean luminance" (a
deterministic quantity — `MLTRasterizer.cpp`'s bootstrap loop seeds each
sample's `PSSMLTSampler` with `i` directly, not the unsynchronized
libc `rand()` the rest of a render uses, so this number reproduces
exactly run to run on a fixed build):

| Build | Mean luminance |
|---|---|
| base (`e290fc64`) | 0.536457 |
| `8080cd2a` (P1) | 0.552240 (+2.94% vs base) |
| this fix | 0.535700 (-0.14% vs base) |

This settles the reviewer's "unverified +1.6%" question, but not in the
literal "bit-for-bit" form originally hoped for: the fix's mean is much
CLOSER to base than the P1-buggy state was (-0.14% vs +2.94%), but it is
NOT bit-identical, and it cannot be while ALSO fixing the O(streamIndex)
blow-up. The reason is mechanical, not a bug: in the base commit's single
flat vector, touching the reserved stream (which lived INSIDE the same
49-wide legacy range, at literal index 48) for its 6 draws had a
cascading side effect — its lazy-grow loop materialised depths 0-5 for
ALL 49 streams at once (49 slots per depth), so by the time BDPT's OWN
calls asked for e.g. `(stream=0, sample=0)`, that slot's value had
ALREADY been consumed from the shared `RandomNumberGenerator`'s sequence
as a byproduct of the reserved stream's draws, and BDPT's read was
"free" (no new RNG draw). This fix deliberately eliminates exactly that
cross-stream coupling (an extra-tier draw touches ONLY its own storage,
consuming ONLY the RNG calls its own draws need), so BDPT's subsequent
legacy-stream calls now draw FRESH values the base commit's incidental
pre-fill made unnecessary — the RNG states diverge from the very first
legacy-tier call in a full `EvaluateSample()` walk that also touches the
reserved stream. Every draw is still a valid, correctly-mutated uniform
sample (Test H proves the algorithm itself is untouched), and the -0.14%
residual is ordinary MC noise from a different-but-equally-valid draw
sequence, not a bias — exactly the "expected and statistically benign"
characterization the original DL-08 fix's own doc gave for the render-level
noise-pattern change, now with the mechanism identified precisely. The
provable, EXACT invariant is the narrower one Test H Part 1 establishes:
a chain that never touches the reserved/extra tier at all (no shipped
scene does, at depth <= 28 per the finding above) reproduces the base
commit's draws bit-for-bit.

### Gate

Clean rebuild, zero warnings (`make -C build/make/rise clean && make -C
build/make/rise -j8 all`). `tests/PSSMLTStreamAliasingTest`: all 8 tests
(A-H) pass. Also green: `SobolDimensionBudgetTest`, `BDPTStrategyBalanceTest`
66/0, `VCMStrategyBalanceTest` 55/0, `EnvLightBalanceTest` 116/0,
`RefractiveRadianceScalingTest` 38/0, `CstDeriveGoldenTest` 452 MATCH/0
DRIFT, `SourceHygieneTest` 165/0. Every test file matching
`grep -liE "PSSMLT|\bmlt\b|mlt_" tests/*.cpp` (22 files) built and ran;
21 are green (most match only on an incidental comment/string, not actual
`PSSMLTSampler` usage — confirmed by content inspection). The one
exception, `AgentEvalCheckTest` (36 failures), is a pre-existing,
environment-dependent failure unrelated to this fix: every failing check
reports "no live session (run did not complete)", a missing
`replay.fixture`, or a `compareToImage` RMSE band against a PT/BDPT
render (never MLT/`PSSMLTSampler`) — this worktree has no live
agent/LLM session or the eval harness's replay infrastructure available,
which is an environment limitation, not a code regression.
