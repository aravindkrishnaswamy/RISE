# SMS Photon Regime — When `sms_photon_count` Helps

Empirical 3-scene measurement of the `sms_photon_count`-aided extension
(Weisstein, Jhang, Chang 2024 PMS as integrated into RISE) under
controlled `sms_target_bounces` and matching seeding modes.  All
measurements are post commit 9bd7c5b (audit-cleaned photon-chain
length filter; both seeding modes correctly enforce target chain
length).

## TL;DR

**Photons help only when the seed-builder MISSES caustic basins.**

| Scene class | snell ratio Δ w/ photons | uniform ratio Δ w/ photons |
|---|---|---|
| Displaced Veach egg (interior light, k=2 single-basin) | +1.9 % | −5.5 % |
| Diacaustic (k=1, curved metal mirror) | **+24 %** | **+96 %** |
| sms_slab_close_sms (k=2 glass slab, occluded snell-trace) | **+170×** | **+40×** |

Per-scene rule of thumb: **enable `sms_photon_count > 0` when the
no-photon ok rate is < 10 %.**  At that point the seed-builder is
demonstrably missing basins and photons are the principled (Weisstein
2024) recovery mechanism.  Above ~50 % ok rate, photons are typically
amortised cost without coverage gain.

## Measurements

All renders use PT, 4 spp, biased SMS, multi_trials=16,
sms_target_bounces set per-scene to the natural caustic K.  All numbers
are from a single render at the noted SMS_DIAG instrumentation; what
matters here are the order-of-magnitude differences, which dominate
single-render MC noise.

### Scene 1: Displaced Veach egg (interior light, dielectric egg shell)

Natural caustic is k=2: wall → outer-egg → inner-cavity → light.
Snell-trace from shading toward light naturally lands in the right
basin — there's only one basin per pixel.  v8 sweep across 66 painter
× displacement cells:

| Config | mean ok % | mean phys % | mean ratio |
|---|---|---|---|
| snell        | 94.4 | 1.85 | 0.893 |
| snell+ph     | 7.7  | 12.3 | **0.910** (+1.9 %) |
| uniform      | 43.4 | 9.47 | 0.351 |
| uniform+ph   | 2.9  | 13.0 | **0.332** (−5.5 %) |

`sms_valid` count is essentially unchanged with photons (snell+t2:
66,402 → 66,356) — photons converge to basins the snell-trace already
covered, basin-dedupe drops them, and net unique caustic coverage is
the same.  Wall time ~6× slower with photons.  **Don't enable photons
on this scene class.**

### Scene 2: Diacaustic (k=1, curved metal-tube mirror)

`scenes/Tests/Caustics/diacaustic_pt_sms.RISEscene`.  A curved metal
tube reflects a directional light onto a wooden box; the visible
caustic is the diacaustic curve where reflected rays self-intersect.
Natural caustic is k=1, but:

- Snell mode traces from the receiver shading point toward the LIGHT.
  The straight-line direction misses most reflection roots on a curved
  mirror — only the points where the line hits the tube AND reflects
  through the light contribute, which is a narrow set.
- Uniform mode samples uniformly on the tube surface, then traces
  from shading-point toward sp.  Better coverage on the tube but
  still only k=1 chains.
- Photons traverse the actual reflection physics: light → tube →
  diffuse landing.  The photon record captures EVERY reflection root
  the tube can produce.

Result (single render, 4 spp, target=1):

| Config | ok % | ratio | sms_valid |
|---|---|---|---|
| snell, no ph | 23.3 | 0.596 | 441,492 |
| snell + ph   | 29.9 | **0.738** (+24 %) | 447,589 |
| uniform, no ph | 33.2 | 0.483 | 437,689 |
| uniform + ph | 43.0 | **0.948** (+96 %) | 446,491 |

**Photons clearly help both modes; uniform sees the bigger gain
(+96 %) because uniform sampling already covers more of the caster
and photons add coverage on top of that.**  Snell trace finds part
of the diacaustic; photons fill the rest.  `uniform+ph` is best
overall (ratio 0.948, near-perfect parity with the supplementary
baseline).

### Scene 3: sms_slab_close_sms (k=2 dielectric, snell-trace blocked)

`scenes/Tests/SMS/sms_slab_close_sms.RISEscene`.  A glass slab sits
near the receiver with the light on the opposite side.  The snell-
trace from shading toward the light is blocked or refracted away from
the actual caustic basin — single render, 4 spp, target=2:

| Config | ok % | ratio | sms_valid | wall time |
|---|---|---|---|---|
| snell, no ph | 0.1 | 0.021 | 302 | 1.96 s |
| snell + ph   | 16.2 | **3.624** (×170) | 86,007 | 15.4 s |
| uniform, no ph | 0.1 | 0.100 | 4,839 | 18.2 s |
| uniform + ph | 7.3 | **4.027** (×40) | 98,394 | 33.0 s |

**Without photons, both modes are catastrophically broken on this
scene** (302 and 4,839 valid contributions out of ~400 k Solve calls
— ratio 0.02 and 0.10 respectively).  Photons recover 86–98 k valid
contributions and lift the ratio above 3.6, meaning SMS is now
contributing ~3.6× the supplementary baseline (it's a SDS caustic
that ordinary PT can't reach at all).  This is the canonical case
the photon-aided extension was designed for.

## Rule of thumb

Decision tree for `sms_photon_count`:

1. **Render once with `sms_photon_count 0`** in the user's chosen
   seeding mode (snell or uniform per the regime guide).
2. Look at the ok rate from `[SMS-SOLVE-DIAG]` (compile with
   `SMS_SOLVE_DIAG=1` once for instrumentation).
   - **ok rate > 50 %**: photons unlikely to help.  The seed-builder
     is finding the basin; coverage is good.
   - **ok rate 10–50 %**: photons may help marginally.  Test on a
     representative shading point.
   - **ok rate < 10 %**: photons probably essential.  The seed-
     builder is missing basins systematically; photons are the
     principled recovery mechanism.
3. Always set `sms_target_bounces` to the natural caustic K when
   enabling photons — the post-9bd7c5b filter keeps photon chain
   length consistent with snell/uniform main seeds.

## Cost vs. benefit

Photon trace pass is amortised across all pixels in the render but
adds:

- ~5–8× more Solve calls per shading point (each photon-aided seed
  runs Newton independently).
- Photon kd-tree construction at scene-prep (one-time per render).
- Photon storage memory (`sms_photon_count` × ~200 bytes per photon,
  bounded by `kSMSMaxPhotonChain`).

For displaced-egg-class scenes (single basin), 6× wall time for ~2 %
ratio gain is bad ROI.  For diacaustic-class scenes (multi-basin
mirror), modest wall time growth for 24–96 % ratio gain is excellent
ROI.  For slab-close-class scenes (snell-blocked), photons go from
"broken render" (ratio 0.02) to "production-quality SMS" (ratio 3.6+)
— enabling photons is the difference between SMS being usable and
being silently zero.

## Mode/photon asymmetry in unbiased mode

There is an intentional, mathematically-motivated asymmetry between
how snell mode and uniform mode handle photons in unbiased mode
(`sms_biased FALSE`):

| | Snell mode | Uniform mode |
|---|---|---|
| **Biased + photons** | ✓ used | ✓ used (separate post-loop pass) |
| **Unbiased + photons** | ✓ used (heuristic 1/p) | ✗ **skipped** |

The skip in uniform unbiased is not a bug.  It reflects the different
estimators each mode uses:

### Uniform unbiased = strict Mitsuba Bernoulli (proposal-matched)

Uniform mode's unbiased estimator (Zeltner 2020 §4.3 Algorithm 2,
Mitsuba `manifold_ss.cpp`) is a textbook Bernoulli:

1. Main trial: uniform-area sample on caster shape → Snell-trace →
   seed → Newton.
2. K-loop: keep doing **the same uniform-area-sample proposal** until
   one fresh sample lands in the same basin.
3. K = first-success-index → E[K] = 1/p_uniform → contribution × K
   is unbiased.

For the K count to correspond to 1/p_uniform, **every retry must use
the same proposal as the main trial**.  Photon seeds come from a
*different* proposal (light-emit-trace + kd-tree query), and mixing
them into the K-loop would bias the estimator.  Skipping photons in
uniform unbiased is mathematically required.

### Snell unbiased = RISE basin-width heuristic (proposal-agnostic)

Snell mode does not have a Bernoulli K-loop.  Instead, every Solve
call internally invokes `EstimatePDF` (`ManifoldSolver.cpp` ~5061),
which:

1. Perturbs the seed by ±0.1 in tangent plane.
2. Counts how many perturbations re-converge to the same root.
3. Returns the count as a basin-width estimate.

This heuristic is **proposal-agnostic** — it perturbs around whatever
seed it receives, regardless of how that seed was generated.  Snell-
traced seeds, photon-derived seeds, and surface-sample seeds all get
the same basin-width treatment.  Strictly speaking this is not the
classical Bernoulli unbiased estimator (`EstimatePDF` doesn't
reproduce the snell-trace's deterministic proposal), but it's a
widely-used SMS-literature heuristic for non-uniform proposals.

Because the heuristic doesn't depend on the source proposal, photon
seeds slot in cleanly: each photon's basin width is estimated
independently.  The estimator is no more biased with photons than
without them.

### Practical guidance

- **Want photons + unbiased + Mitsuba-strict?**  Not directly
  supported.  Switch to biased (the photon-aided extension at
  `EvaluateAtShadingPointUniform` line ~7421 is biased-only).
- **Want photons + unbiased + RISE flavoured?**  Use snell mode.
- **Want photons + biased?**  Both modes work (see
  decision tree above).

The asymmetry is documented at the seedingMode dispatch in
`ManifoldSolver.cpp` (the inline comment on the `else` branch of
`if( config.biased )`).

## Open question: when does uniform+ph beat snell+ph?

On diacaustic, `uniform+ph` was best (ratio 0.948 vs `snell+ph` 0.738).
On slab_close, `uniform+ph` was also best (4.027 vs 3.624).  This
suggests: **on multi-basin scenes where photons are useful, uniform
mode benefits more from photons than snell does**, because uniform
sampling + photons combine two complementary coverage strategies.
Single-basin scenes (the displaced-egg class) don't see this synergy
because there's only one basin to cover.

Recommend: when enabling photons, use `sms_seeding "uniform"`
unless the scene has previously-validated snell-mode preference
(refraction-dominated displaced caustics like the displaced
Veach egg).
