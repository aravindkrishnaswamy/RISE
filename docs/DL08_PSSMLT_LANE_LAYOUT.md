# DL-08: PSSMLT stream aliasing (debt 29)

CLOSED 2026-09-17 — fix `d7ebd453` (red-proof test `60994864`). The DL-08
regression run reported `tests/PSSMLTStreamAliasingTest` Test F green
(previously red on master `e290fc64`).

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
`[0, kNumStreams)` at the one place `idx` is actually computed (`assert`,
debug builds only, per the codebase's existing `assert` idiom — no
runtime cost in a release build), so a future violation of this
invariant fails loudly during development instead of silently aliasing
a lane.

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

## Sibling audit

Bug pattern: **a fixed-size, dense (non-hashed) lane table whose
address is a caller-supplied integer with no enforced upper bound will
silently alias once that integer reaches the table's width.**

| Sampler | Lane/stream table | Reachable stream ceiling vs table width | Status |
|---|---|---|---|
| `PSSMLTSampler` | Dense, `idx = stream + kNumStreams*sample`, `kNumStreams` was 49 | BDPT eye walk reaches stream 1039 under PSSMLT (see above) — **1039 > 48** | **FIXED** this row: `kNumStreams` -> 4096, MLT's reserved stream -> 2048 |
| `SobolSampler` / `ZSobolSampler` | `dimension = stream*32`, finite table `kNumDimensions = 8192` (256 streams) since DL-81 | Shipped-scene ceiling (stream 241, `SobolDimensionBudgetTest` Test G) is inside the table; a stream past it does not hard-alias — DL-81 made `Get1D` **Owen-permute by wrap count** past the table (decorrelates) and `Get2D` a padded, raw-dimension-keyed pair (never wraps at all) | Not vulnerable to *this* pattern — DL-81 already replaced hard aliasing with graceful degradation. `ZSobolSampler` inherits `SobolSampler::StartStream`/`Get1D`/`Get2D` unchanged (only `sampleIndex`'s Morton remap differs), so the same conclusion applies without a separate audit. |
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
