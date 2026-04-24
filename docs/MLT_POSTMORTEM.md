# MLT Variant Postmortem: Why MMLT and PathMLT Were Retired

RISE shipped two variant Metropolis Light Transport rasterizers alongside the
legacy PSSMLT implementation (`MLTRasterizer`):

- **MMLT** (Multiplexed MLT, Hachisuka et al. 2014) — per-path-length-depth
  chain pools so per-strategy luminance, not summed luminance, drives chain
  density.
- **PathMLT** (Veach 1997 structural mutations) — LensPerturbation,
  CausticPerturbation, and MultiChain mutations layered on top of the
  PSSMLT-equivalent BidirectionalMutation.

Both were removed after measurement showed they did not beat PSSMLT at
equal wall clock on the scenes RISE actually renders, and both came with
pipeline restrictions that made their footprint larger than their benefit.
This document captures what we built, the empirical results, and the
conditions under which it would be worth trying again.

## TL;DR

- Neither variant beat PSSMLT at equal wall-clock time on the Veach Egg
  (canonical SDS), and MMLT is measurably *worse* on well-mixed scenes.
- **MMLT** targets dim-strategy starvation on deep-depth SDS paths, but
  on the egg the deep depths (`d >= 4`) have essentially zero contribution
  regardless of rasterizer, so MMLT's architectural advantage does not
  translate to image quality. Memory scales per depth, making 4K infeasible.
- **PathMLT**'s structural mutations (`caustic_perturb_prob` was the only
  one that actively helped) reduced bias on the egg by ~4 percentage
  points at best; at matched wall-clock PSSMLT-with-more-mutations was
  visually indistinguishable from PathMLT-with-structural-mutations.
- Both variants come with significant pipeline restrictions: no spectral
  variant, no OpenPGL path guiding, no directional/ambient lights, per-depth
  memory scaling.

## MMLT

### Motivation

PSSMLT runs ONE Markov chain whose density adapts to the **summed** luminance
over all `(s,t)` connection strategies. On SDS scenes, dim strategies (e.g.,
diffuse-wall paths at depth 6) are rarely visited because the chain spends
most of its time on the bright caustic strategy at depth 3 that dominates
the sum. MMLT partitions the chain budget into **one pool per path-length
depth** `d = s + t - 2`; each chain in pool `d` only evaluates strategies
with `s + t = d + 2`, and its density adapts to the per-depth luminance
`b_d`. The theoretical promise: diffuse-wall contributions at depth 6 get
exactly the budget they earn from their share of the total integral,
regardless of how bright the depth-3 caustic is.

### Architecture that landed

- `MMLTRasterizer` (`src/Library/Rendering/MMLTRasterizer.{h,cpp}`) —
  parallel to `MLTRasterizer`, with a round-based progressive render loop,
  per-depth `SplatFilm`s, per-depth `b_d` bootstrap, and per-depth chain
  pools.
- `MMLTSampler` (`src/Library/Utilities/MMLTSampler.{h,cpp}`) — extended
  `PSSMLTSampler` with reserved streams 49 (strategy selection `(s,t)`)
  and 50 (lens position, split out of stream 48). This required promoting
  `PSSMLTSampler::kNumStreams` from `static const int` to a per-instance
  member (with `kDefaultNumStreams = 49` fallback for plain PSSMLT).
- `BDPTIntegrator::ConnectAndEvaluateForMMLT` — single-strategy entry
  point that mirrored the inner loop body of `EvaluateAllStrategies`
  but evaluated exactly one `(s,t)` and stamped `cr.s`/`cr.t`.
- `BDPTIntegrator::GenerateLightSubpath` / `GenerateEyeSubpath` gained
  a `maxBouncesOverride` parameter (default `UINT_MAX`) so MMLT chains
  could cap subpath length at their depth budget.
- **Phase 6 multi-strategy bootstrap** — to keep `b_d` estimator variance
  manageable, the bootstrap evaluated all valid `(s,t)` at a depth and
  summed MIS-weighted contributions rather than picking one randomly.
  Closed the MMLT-vs-PSSMLT normalization gap from 14.7 % to 3.9 %
  (within MC noise) on the egg.

### Why it didn't deliver

The theoretical advantage (per-depth budget adaptation) requires the deep
depths to actually carry contribution. On the Veach Egg test:

> "The remaining `bd[4..8] = 0` is not a bootstrap-coverage gap — it's
> a geometric reality. At depth 4, the only potentially-contributing
> strategies are `(4,2)` and `(5,1)`, both requiring the light path to
> escape both glass shells AND land on a diffuse wall at exactly that
> vertex index. PSSMLT's `b = 95320` is dominated by depths 2 and 3;
> **deeper depths contribute very little to PSSMLT either**."

MMLT redistributes budget to categories that are essentially empty.

### Costs

- **Memory scales per depth.** At 256² with ~25 active depths, per-depth
  `SplatFilm`s cost ~12 MB — negligible. At 1024², ~800 MB. At 4K
  (3840×2160), ~40 GB. We marked this "infeasible without revising the
  per-depth-film design" in the original [MMLT.md](https://github.com/
  aravindkrishnaswamy/RISE/blob/b8e12c/docs/MMLT.md). The work to fix
  this (single shared SplatFilm with per-deposit `bd/Nd` weighting)
  never landed.
- **Per-mutation variance is higher.** Each MMLT mutation evaluates ONE
  `(s,t)`, not all strategies summed. On well-mixed Cornell: ~15 %
  higher stddev than PSSMLT at equal mutation budget. So on scenes that
  don't need the per-depth trick, MMLT loses outright.
- **Pipeline restrictions.**
  - No spectral variant — `MLTSpectralRasterizer` exists but
    `MMLTSpectralRasterizer` does not.
  - No OpenPGL path guiding — MMLT samples are highly correlated and
    corrupt training.
  - No directional / ambient lights — MMLT's per-depth bootstrap can't
    route through `EvaluateAllStrategies`'s zero-exitance fallback.
    PBRT v3 MMLT has the same restriction, documented as such.
  - No SMS interop.

## PathMLT

### Motivation

Veach 1997 §11.4 defines three path-space structural mutations on top of
PSSMLT's primary-sample-space small/large step mixture:

- **LensPerturbation** — anchor perturbation on a near-camera tangent
  plane; preserves the specular chain and explores nearby pixels.
- **CausticPerturbation** — perturbation on the light-side anchor;
  explores the caustic footprint while preserving the camera-side chain.
- **MultiChain** — diffuse-chain extension combining the above for
  paths with multiple specular segments separated by a diffuse vertex.

All three preserve topology (specular vertex count stays the same),
which PSSMLT's primary-sample-space small step cannot guarantee — a
primary-sample perturbation can flip a Fresnel specular into its opposite
lobe or cross a delta-boundary. The hope: PathMLT's topology-preserving
proposals should explore SDS basins PSSMLT can't reach.

### Architecture that landed

- `PathMLTRasterizer` — extended `MLTRasterizer` with the three
  structural mutations, dispatched per-iteration by a probability mix
  (`large_step_prob` + `lens_perturb_prob` + `caustic_perturb_prob` +
  `multi_chain_perturb_prob`, with the remainder going to the standard
  BidirectionalMutation).
- Caustic perturbation had two modes: `option_d` (primary-sample-space
  Gaussian on stream-0 direction samples) and `sms` (Phase E3 SMS-driven
  path-space proposal with manifold-solver Newton solve and dual
  Jacobian correction). Default was `option_d`.
- `PSSMLTSampler` grew four new methods to support PathMLT:
  `StartFrozenIteration`, `StartSmallStepIteration`,
  `OverridePrimarySample`, `GetPrimarySample`, plus the `locked` and
  `isFrozenIteration` flags on `PrimarySample` and the sampler itself.
  These let the structural mutations splice SMS-derived values into
  specific primary-sample indices while leaving the rest untouched.

### The `StartFrozenIteration` trap (lesson worth remembering)

Early PathMLT versions used `StartFrozenIteration` — Get1D returns
`X[idx].value` untouched, no Mutate, no fresh random. The hope was that
the only effective per-iteration mutation would be the explicit
`OverridePrimarySample` call on film X/Y.

**This biases the chain.** "Get1D was called but no Mutate happened" has
no clean representation in PSSMLT's `lastModIteration` tracking: updating
it lies about a mutation (the catch-up Mutate loop under-counts later),
not updating it over-counts catch-up Mutates next iteration. Either way
the egg brightened by ~10–25 %.

Fix: `StartSmallStepIteration` — like `StartIteration` except
`isLargeStep` is forced to false (no large-step decision RNG draw). Non-
overridden samples still get a normal small-step Mutate. Combined with
the override's `locked` flag, the proposal is symmetric in primary
sample space and no Jacobian is needed for the MH ratio.

### Empirical results

Reference: 1000 mpp PSSMLT-only egg render (~11 min wall time), L=100.74.
Sweep renders: 200 mpp at 256² (~2 min each). Bias = (testL - refL) / refL.

**Single-mutation bias scaling on Veach Egg:**

```
p_struct   baseline     lens     caustic    multi
  0.00      -10.60%     -10.60%   -10.60%   -10.60%   <- chain MC tax @ 200mp
  0.05         -        -12.44%    -8.81%   -11.10%
  0.10         -        -12.94%   -10.22%   -10.76%
  0.20         -        -15.42%    -8.20%   -16.22%
  0.30         -        -17.79%    -6.49%   -19.66%
  0.50         -        -28.19%    -9.33%   -26.24%
```

Caustic was the **only** mutation whose bias did not monotonically worsen
with `p_struct`. Best single result: `p_caustic=0.30`, bias -6.5 %
(a 4-percentage-point improvement over PSSMLT's -10.6 %). Lens and
multi-chain monotonically made things worse. Sigma tuning (scan at
0.001, 0.002, 0.005, 0.010, 0.020) produced another 0.2 pp improvement
on the egg (σ=0.001 best) but **reversed direction on the luminous orb**
— smaller σ hurt there.

**Mixed structural mutations never beat the best single mutation.** On
the egg, `p_caustic=0.30` alone (-6.5 %) beat every combination tried
(`lc015` -13.5 %, `lcm010` -15.2 %, etc.).

**Wall-time overhead** (200 mpp):

```
config           wall (s)   overhead vs baseline
baseline             128       —
caustic 0.30         172       +34 %
lens    0.30         153       +20 %
multi   0.30         143       +12 %
```

Caustic was ~3× more bookkeeping per chain step than multi-chain.

**Wall-clock A/B on Veach Egg (400×400, mutations_per_pixel = 100 for
PathMLT; 187 for MLT to match wall time):**

| Run                          | mpp | Wall time | Notes |
|------------------------------|-----|-----------|-------|
| MLT baseline                 | 100 | 275.89 s  | |
| PathMLT (lens 0.3, caustic 0.3, multi 0.1) | 100 | 340.49 s  | 23% slower/mutation |
| MLT wall-clock-matched       | 187 | 344.62 s  | |

At equal wall-clock, MLT-187 and PathMLT-100 were visually
indistinguishable — same grain on the walls, same caustic structure on
the egg, maybe a hair softer edge on the egg hot-spot in PathMLT but
not consistently better. Acceptance rates were healthy (lens 76.5 %,
caustic 64.5 %, multi 76.5 %), so the mutations were "working" — they
just weren't paying for their per-mutation overhead.

**Cross-scene survey** (each scene at p={lens=0.10, caustic=0.10,
multi=0.10}, reference = same scene's PSSMLT 200mpp baseline):

```
scene             mut       bias%   brightΔ%   midΔ%    funnel
caustic_chain     caustic   -0.26   -0.88      +0.12    95 % Skipped
keyhole           all       -0.31   -1.22      -1.70   100 % Skipped*
luminous_orb      caustic   -2.77   -29.66    +85.40    0 % Skipped (every fires)
luminous_orb      lens     -14.98   -39.42    +63.92    23 % Skipped
luminous_orb      multi    -15.82   -38.95    +57.24    22 % Skipped
reflected_caustic caustic   -0.01   -0.99     +0.54    89 % Skipped
```

*keyhole produced bit-identical output for lens/caustic/multi at
`p=0.10` because every iteration was Skipped (no suitable specular
chain to perturb) — the chainRNG dispatch draw was consumed identically
across all three. Most scenes lack enough specular topology for
structural mutations to fire.

**Luminous_orb: structural failure on sharp caustics.** Bright-pixel
bias was -30 % regardless of σ — the caustic peak lost 30 % of its
energy to surrounding diffuse halo under any caustic-perturb
configuration. The chain accepts bright→bright proposals, but the
resulting splatted energy goes to the halo, not the peak. σ tuning
doesn't fix this; it's a structural property of option-D caustic
perturb on sharp localized caustics. The SMS-driven variant was wired
but never worked on the egg or orb due to BuildSeedChain chain-length
mismatches (straight-line trace from perturbed emission misses the
recorded specular topology on refractive scenes).

### Side-effect wins

Two meaningful fixes came out of the PathMLT investigation, both kept:

- **`ManifoldSolver` nested-dielectric η-pair fix** — the original code
  assumed "the other side of every interface is air". Broke on the
  Veach Egg (glass shell IOR=1.5 around air-cavity IOR=1.0): the inner
  glass/air boundary's Walter-form half-vector collapsed to mirror-
  symmetric reflection and Newton walked away from the true Snell
  solution. Fix: `ManifoldVertex` gained `etaI`/`etaT` fields,
  `BuildSeedChain` populates both from the IOR stack, and the half-
  vector / Jacobian / Fresnel sites all use the correct two-IOR form.
  The post-fix `mlt_veach_egg_pt_sms` render is 265 % brighter overall
  (+1220 % in the mid-pixel range) — the cavity walls finally receive
  their caustic illumination. 3 new regression tests in
  `ManifoldSolverTest.cpp`.
- **`StartFrozenIteration` vs `StartSmallStepIteration` symmetry
  lesson** — applicable to any future primary-sample override scheme.
  Freezing Get1D silently over/under-counts catch-up Mutates; let it
  run normally and rely on the `locked` flag to protect your override.

The eta-fix stays in tree because it affects SMS / BDPT / VCM, not just
the retired PathMLT.

## Why they both failed on the same scene

The Veach Egg is the canonical SDS torture case, but on a 400×400 egg
render at 100–200 mpp:

1. **PSSMLT's acceptance doesn't collapse.** Bidirectional mutation finds
   valid refracted paths (interior light at IOR=1.0 cavity center is
   generous) and the chain doesn't get stuck. When PSSMLT isn't stuck,
   its per-mutation cost advantage dominates.
2. **Deep-depth paths have almost no contribution.** The MMLT-style
   per-depth budget adaptation has nothing to redistribute budget *to*.
3. **Structural mutations overlap with primary-sample exploration.**
   PSSMLT's small step already perturbs film position / direction
   samples. PathMLT's lens / multi-chain mutations sample similar
   regions at higher per-mutation cost — pure overhead.

The PathMLT caustic perturb WAS the one signal that exceeded noise
(-4 pp bias improvement on the egg), but it's only helping with one
specific failure mode (sharp caustic center under-sampling) that
PSSMLT has but most production scenes don't.

## When to reconsider

Both variants would be worth a second look if any of these showed up:

- **A real-world scene where PSSMLT visibly stagnates** — low overall
  acceptance (< 20 %), blotchy "stuck chain" artifacts, dim regions
  nowhere near converged after long renders. Then structural mutations
  (PathMLT) or per-depth pools (MMLT) have a chance to earn their
  keep. We did not have such a scene in the test corpus; all our
  "hard" scenes were either well-mixed (egg) or benefited from SMS
  photon-seeded chains that fixed the problem in a different way.
- **SMS caustic perturb actually working** — requires photon-aided
  seeding in PathMLT (multi-trial fallbacks alone weren't enough for
  nested-dielectric refractive geometry). Would need `caustic_sms_
  photon_count` exposed at the rasterizer level and fed into
  BuildSeedChain. Plumbing-only, but several layers of ABI surface.
- **A single shared SplatFilm design** for MMLT so the per-depth
  memory cost at 4K stops being a blocker. Mathematically equivalent
  up to floating-point ordering; reshapes the per-deposit weight
  from `bd / Nd` to a per-depth lookup at splat time.

Without one of those, don't rebuild. The complexity tax (parser /
IJob / RISE_API / Job / BDPTIntegrator threading) is high, and every
future config-struct refactor has to be kept coherent across all the
variant rasterizers. That's the cost we paid; there's no reason to
pay it again for theoretical capability that doesn't materialize on
shipping renders.

## What's still in the tree from this work

- `ManifoldSolver` nested-dielectric η-pair fix (in the Solve / Newton
  math) — survives removal because it's shared infrastructure.
- No PathMLT / MMLT scenes, rasterizer sources, parser chunks, API
  functions, tests, or docs.
- `PSSMLTSampler` is back to its pre-PathMLT state (no
  `StartFrozenIteration`, `StartSmallStepIteration`,
  `OverridePrimarySample`, `GetPrimarySample`, `locked`, or
  `isFrozenIteration`).
- `BDPTIntegrator::GenerateLightSubpath` / `GenerateEyeSubpath` no
  longer take `maxBouncesOverride`. `ConnectAndEvaluateForMMLT` is
  gone — its one call site inside `EvaluateAllStrategies` was inlined
  back to `ConnectAndEvaluate` + explicit `cr.s / cr.t` stamping.

## Files retired (for reference)

```
src/Library/Rendering/MMLTRasterizer.{h,cpp}
src/Library/Rendering/PathMLTRasterizer.{h,cpp}
src/Library/Utilities/MMLTSampler.{h,cpp}
tests/MMLTStrategySelectionTest.cpp
scenes/FeatureBased/MLT/mmlt_cornell_simple.RISEscene
scenes/FeatureBased/MLT/mmlt_cornell_force2.RISEscene
scenes/FeatureBased/MLT/mmlt_veach_egg.RISEscene
scenes/FeatureBased/MLT/pathmlt_veach_egg.RISEscene
docs/MMLT.md
```

Recoverable from the commit that introduced them + `git log -- <path>`
if the conditions above ever change.
