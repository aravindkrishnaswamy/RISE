# DL-67: the guided continuation at a multi-lobe SPF, priced on ONE partition

Slice `debt-dl67`, 2026-09-27, branched from `master` `6b91fd19`.
Ledger row: [DEBT_LEDGER.md](DEBT_LEDGER.md) DL-67.  Predecessors:
[DL67_SLICE0_SCHLICK_PDF_WEIGHTS.md](DL67_SLICE0_SCHLICK_PDF_WEIGHTS.md)
(the aggregate `ISPF::Pdf()` made the true generating density),
[DL74_ENV_NEE_GUIDING_PARTITION.md](DL74_ENV_NEE_GUIDING_PARTITION.md)
(sampling density vs MIS-partner density),
[DL69_BDPT_LOBE_THROUGHPUT.md](DL69_BDPT_LOBE_THROUGHPUT.md) and
[DL103_PT_ESCAPE_MIS_PARTNER.md](DL103_PT_ESCAPE_MIS_PARTNER.md) (the
un-guided halves).

## 1. The one rule

A guided surface vertex runs two techniques on the continuation integral
`I = integral f(w) cos(w) L(w) dw`:

* the **BSDF technique** -- `Scatter` emits every lobe, `RandomlySelect`
  keeps lobe `I` with realized probability `q_I`, and its unweighted
  estimator is `kray_I / q_I` (the SPF contract `kray_I = f_I cos / p_I`);
* the **guide technique** -- a direction `w ~ g` from the trained field,
  unweighted estimator `f(w) cos / g(w)` with `f` the AGGREGATE BSDF.

Every guided branch now prices its technique's unweighted estimator times
that technique's share of ONE deterministic, lobe-independent partition,
divided by the probability that the technique fired:

```
W_g(w) = a g(w) / (a g(w) + (1 - a) p_agg(w)),        W_b(w) = 1 - W_g(w)

lobe I kept     X_b = kray_I / q_I  *  W_b(w_I) / (1 - alpha_I)
guide draw      X_g = f(w) cos / g(w)  *  W_g(w) / alphaBar,   alphaBar = sum_J q_J alpha_J
```

* `p_agg` is the material's aggregate `ISPF::Pdf()` -- since DL-67 Slice 0,
  DL-98/99, DL-177 and DL-157 exactly the density of what `Scatter` +
  `RandomlySelect` generate for every multi-emit SPF in tree -- never the
  selected lobe's own `pS->pdf`.
* `a` is `PTGuidingMisPdf`'s nominal alpha in one-sample mode (so the
  continuation splits directions by the SAME function NEE is weighted
  against) and 1/2 in RIS mode (the RIS proposal mixture).
* `alpha_I` is the probability that the guide REPLACES lobe `I` once it is
  selected -- the per-lobe eligibility rule the code always had (PT:
  diffuse or glossy non-delta, glossy at half alpha, learned per-cell
  scale; BDPT: diffuse non-delta at the constant alpha; RIS: 1 for an
  eligible lobe).  `alphaBar` is the REALIZED probability the guide fires,
  computed from the container by `GuidingRealizedGuideProbability` with the
  exact `RandomlySelect` weights.
* A lobe the guide never replaces (`alpha_I = 0`: a transmission lobe, a
  BDPT glossy lobe) still takes `W_b` wherever it is non-delta -- the guide
  technique prices the AGGREGATE BSDF, so it covers that lobe too.  A delta
  lobe keeps weight 1 (the guide cannot generate a delta direction).
* Where `p_agg(w_I) <= 0` at a direction a lobe really generated, `W_b := 1`
  (DL-103's guard, no known production inhabitant since DL-41 closed).
* A guide draw that yields nothing is a ZERO sample of the guide technique
  -- never a fall-back to the lobe's own direction.

**BDPT's `pdfFwd` has one rule too, guided or not**: the aggregate
`ISPF::Pdf()` at the direction actually traced -- the function `pdfRev` and
every connection strategy evaluate.  It is the MIS-partner density, and MIS
needs the same function on every strategy (no other strategy can evaluate
this vertex's trained guide).  The zero-aggregate fallback is the density
the continuation's own weight corresponds to (`equivScatterPdf`).  This is
what the 2026-09-14 review called "a THIRD density": it is not a third
density, it is the DL-69 partner rule, now stated for the guided branches.

## 2. Why it is unbiased (and what the old code got wrong)

Condition on one `Scatter` realization `R`.  The walk selects `I` with
probability `q_I`, then the guide fires with probability `alpha_I`:

```
E[F | R] = sum_I q_I [ alpha_I integral g X_g dw  +  (1 - alpha_I) X_b(I) ]
         = integral f cos W_g dw  +  sum_I kray_I W_b(w_I)
```

The first term needs `X_g` independent of `I` and the division by the
realized `alphaBar` (so the `q_I alpha_I` sum cancels); the second is the
SPF's own kray contract, `E_R[sum_I kray_I h(w_I)] = integral f cos h` for
any `h`.  The total is `integral f cos (W_g + W_b) = I` for ANY `W` that is
a deterministic function of direction, so **unbiasedness does not depend on
`p_agg` being exact** -- only the variance (balance-heuristic optimality)
and the zero-aggregate guard do.  Condition: the event "some lobe in this
realization can fire the guide" has probability 0 or 1 at the vertex (a
realization with `alphaBar = 0` keeps `W_b = 1`); true for every SPF whose
eligible lobe is always emitted, and the only way to violate it is an SPF
whose emitted lobe SET is random, i.e. `CompositeSPF` (see §8).

The pre-fix branches, and what each one computed:

| site | pre-fix estimator | defect |
|---|---|---|
| PT one-sample, lobe kept | `kray_I p_I / (q_I (alpha_I g + (1-alpha_I) p_I))` | partition built from the SELECTED lobe's density; expectation `(1-alpha) sum_I integral f_I cos p_I / p_c,I` + guide term != `I` |
| PT one-sample, guide draw | `f cos / (alpha_I g + (1-alpha_I) p_agg)` | uses the SELECTED lobe's `alpha_I` in a technique the other lobes also fire -- inconsistent once alpha varies by lobe (Schlick: diffuse alpha, specular alpha/2) |
| PT RIS | candidate 0 proposal `p_I`, candidate 1 `p_agg` | RIS identity `sum_i p_i(x)/(p_0+p_1)(x) = 1` needs ONE function evaluated at both candidates |
| PT, any lobe with `alpha_I = 0` (e.g. `eRayTranslucent`) | `kray/q` | double count wherever `g > 0` on that lobe's support (the vMF cosine product has mass below the horizon) |
| BDPT substituted | `f_agg cos / (q_I (alpha g + (1-alpha) p_agg))` | a `1/selectProb` on a draw that is not conditioned on any lobe |
| BDPT kept | `kray_I p_I / (q_I p_c)` | as PT |
| BDPT RIS pick of candidate 0 | priced as SUBSTITUTED (`f_agg / (q_I risEff)`) | aggregate over a per-lobe density |
| BDPT guide draw that produced `f = 0` or `p_c <= NEARZERO` | fell back to the kept branch | raises the BSDF technique's firing probability above `1 - alpha` -- biased even at a single-lobe Lambertian |
| BDPT glossy / transmission lobes | never guided, `kray/q` | double count as PT's `alpha_I = 0` row |

### 2.1 The RIS identity

Two candidates, `x_0` from the BSDF technique and `x_1 ~ g`, contributions
`c_0 = kray_I/q_I * W_b(x_0)` and `c_1 = f cos(x_1) W_g(x_1) / (g(x_1) eps)`,
`eps` = realized probability an RIS vertex is reached (`sum_J q_J` over
eligible `J`).  Resampling with any positive weights `w_i` and outputting
`c_y * (sum w)/w_y` has conditional expectation `c_0 + c_1`, so the output
is unbiased for the same partition.  With `W_g = g/(g + p_agg)` (a = 1/2)
and the code's weights `w_i = p_hat(x_i) / (0.5 (p_agg + g)(x_i))`, a
single-lobe material reproduces the pre-fix estimator exactly:
`c_y (sum w)/w_y = f cos(y) (sum w) / (2 p_hat(y))`.  The identity
`sum_i p_i(x) / sum_j p_j(x) = 1` holds at every `x` because BOTH candidates
now evaluate `p_0 = p_agg` -- pre-fix candidate 0 used `p_I(x_0)`.

### 2.2 The synthetic two-lobe SPF

`PTGuidingMISPartitionTest`'s `MultiLobeSPF`: `p_A` cosine, `p_B` cos^63,
constant krays `c_A = 0.3`, `c_B = 0.7`, so `q_I = c_I`,
`p_agg = sum c_I p_I`, `f cos = p_agg`, albedo exactly 1, both lobes
`eRayDiffuse` (one `alpha`).  One-sample:

```
guide:  alpha * integral g * (f cos / g) * (a g / p_mis) / alpha   = integral a g p_agg / p_mis
kept:   sum_I c_I (1 - alpha) * integral p_I * (c_I / c_I) * (1 - a) p_agg / ((1 - alpha) p_mis)
      = integral (1 - a) p_agg^2 / p_mis
sum:    integral p_agg (a g + (1 - a) p_agg) / p_mis  =  integral p_agg  =  1
```
(`alphaBar = alpha` because both lobes carry the same alpha, `kray_I / q_I = 1`,
`p_mis = a g + (1 - a) p_agg`, `a = alpha`.)

Pre-fix the kept term was `(1-alpha) sum_I c_I integral p_I^2 / (alpha g +
(1-alpha) p_I)`, which differs wherever `g` is comparable to `p_I` -- the
BROAD field row reads +4.59%, the sharp one (guide dominates in ~0.1 sr and
is negligible elsewhere) only -0.05%.  **A partition error is only visible
where the two techniques' densities are comparable** (DL-103's lesson), so
every DL-67 row runs under both a sharp (cos^64) and a broad (cos^2) field.

## 3. The four sites

1. **PT PART 3** (`PathTracingIntegrator.cpp`, RGB/NM templated body):
   one-sample kept / guide draw, RIS candidates 0/1, and the new
   `alpha_I = 0` branch.  PT HWSS has no guiding block.
2. **BDPT eye generator** and 3. **BDPT light generator**
   (`BDPTIntegrator.cpp`): both call one helper, `BDPTGuidedContinuation`.
   The vertex-level gate is no longer the SELECTED lobe's eligibility: the
   guide distribution is initialised whenever a lobe in the container can
   fire it (`guideProb > 0`), because a lobe that cannot still needs `g` at
   its own direction.  The gate also requires `guidingAlpha > NEARZERO`, so
   a warm-up iteration (alpha 0) is truly un-guided in RIS mode too (it
   used to resample).
4. **`pdfFwd`** in both generators: the rule of §1; the aggregate the
   guided branch already evaluated at the traced direction is handed over
   instead of being queried twice.

VCM shares the generator but never installs a field (no VCM rasterizer
calls `BDPTIntegrator::SetGuidingField` and the VCM chunks do not accept
`pathguiding*`); MLT constructs its own `BDPTIntegrator` and never installs
one either.  Neither has a guided path to test.

### 3.1 HWSS

* PT: no guided block in `IntegrateFromHitHWSS` (DL-74).
* BDPT kept lobe: companions use `EvaluateKrayNM(lambda_c) * krayScale`,
  and `krayScale` now carries the same partition factor as the hero; the
  aggregate fallback's `invScale` carries it through `scatterPdf`.
  `scatterType` = the lobe's type, so `RecomputeSubpathThroughputNM`'s
  ratio `f_I(lambda_c)/f_I(lambda_h)` (DL-216) is the ratio of two
  estimators that differ only in `kray` -- the partition factor cancels.
* BDPT substituted (including a RIS pick of candidate 1): `eRayUnknown`,
  aggregate ratio, as DL-125 ruled.  A RIS pick of candidate 0 is now a
  KEPT lobe (`useKray`), not a substitution.
* The partition weights are computed once, at the hero wavelength, and
  shared by every lane -- legitimate because a partition only has to sum to
  one per lane, and both techniques use the same `W`.

### 3.2 Audited, not changed

`RayCaster::CastRay`'s two volume guided sites and PT's in-loop volume
guided vertex are one-sample MIS over a single-lobe phase function with one
alpha, so `GuidingCombinedPdf(alpha, g, phasePdf)` IS the true generating
density; their fall-back fires only on `guidePdf <= 0` at a SAMPLED
direction (OpenPGL degenerate), not on a zero BSDF value the way BDPT's did.

## 4. Red-proofs

All A/Bs are isolated `git checkout 6b91fd19 -- <file>` reverts of
COMMITTED state, rebuilt and re-run, then restored.

### 4.1 `PTGuidingMISPartitionTest` -- 141/13 -> 154/0

| row (tol) | pre-fix | post-fix |
|---|---|---|
| (l) two-lobe env, one-sample fixed, broad | +4.59% | -0.20% |
| (l) two-lobe env, one-sample learned, broad | +4.65% | -0.15% |
| (l) two-lobe env, RIS, sharp / broad | -1.59% / -9.57% | -0.36% / -0.18% |
| (m) schlick env 40 deg, one-sample fixed / learned, broad | -1.63% / -1.65% | +0.11% / +0.09% |
| (m) schlick env, RIS sharp / broad | +5.45% / +2.44% | -0.03% / -0.06% |
| (n) translucent env, one-sample sharp / broad | +4.06% / +8.81% | -0.20% / -0.22% |
| (o) two-lobe area, RIS sharp | -2.44% | -0.25% |
| un-guided controls; one-lobe mixture control; schlick area | green | green |

At 10x the samples every row reads within 0.11% of its reference, so the
residuals above are noise, not a remaining bias.  The pre-DL-67 Lambertian
rows (a)-(i) are the single-lobe control: unchanged.

### 4.2 `BDPTGuidedContinuationTest` (new) -- 29/33 -> 62/0

Worst pre-fix rows: eye schlick one-sample (axis below the horizon,
cos^64) **+137.9%**, light schlick one-sample **+102.5%**, eye translucent
one-sample **+41.0%**, and the LAMBERTIAN one-sample rows **+165.7% /
+233.3%** (eye) and **+163.5%** (light) -- the fall-back-on-zero-guide-draw
defect, which is why the Lambertian material is not a control for BDPT.
Post-fix every eye row is within 2.5 printed standard errors.

### 4.3 `BDPTStrategyBalanceTest` guided topology L (2% band)

| row | pre-fix | post-fix |
|---|---|---|
| BDPT guided RIS vs un-guided PT | +19.07% | -0.01% |
| PT guided RIS vs un-guided BDPT | PT +7.46% | PT -0.16% |
| BDPT guided one-sample vs un-guided PT | +0.08% | -0.09% |
| PT guided one-sample vs un-guided BDPT | -0.03% | +0.05% |

The one-sample rows are consistency pins, not red-proofs: both rasterizers
scale the configured alpha by their adaptive variance heuristic, which
settles at ~0.016 on this scene, so the guided branches fire on ~1% of
vertices.  RIS resamples every eligible vertex regardless of alpha.

## 5. Gate (current counts, from this slice's run logs)

`PTGuidingMISPartitionTest` 154/0, `BDPTGuidedContinuationTest` 62/0,
`BDPTStrategyBalanceTest` 188/0 (full run; 24/0 `--guided-only` after the
PT RIS row), `VCMStrategyBalanceTest` 74/0, `TranslucentIORStackTest` all
pass, `PTGuidedSelectProbTest` all pass, `RayCasterEnvEscapeMISTest` 91/0,
`EnvLightBalanceTest` 123/0, `VolumeEnvFurnaceTest` 32/0,
`OptimalMISTrainingSitesTest` 111/0, `SchlickLobePairingTest` 27/0,
`HWSSCompanionKrayTest` 189/0, `CstDeriveGoldenTest` 454 MATCH / 0 DRIFT,
`SourceHygieneTest` 167/0, `RasterizerDefaultsConsistencyTest` 164/0,
`AgentFirstSliceTest` 372/0.  `SSSRadianceScalingTest` reads 574014/3 --
IDENTICAL with this slice's three source files reverted to `6b91fd19`, so
it pre-dates DL-67 (opened as DL-284).  Zero compiler warnings, library
and every test target built.

`TranslucentIORStackTest`'s BDPT probe needed two test-side changes, both
consequences of the ruling rather than of a defect: its DL-43 density check
captured the FIRST exit-window `Pdf()` query, which in RIS mode is now
candidate 0's aggregate, so it now looks the query up by direction (and
counts a substituted candidate with no query at its own direction as bad);
and its liveness check "guided issues more exit queries than un-guided" is
`>=`, because the guided branch hands its aggregate to `pdfFwd` instead of
re-querying it.

## 6. Cost

Two separately built `bin/rise` binaries (`6b91fd19` vs this slice),
interleaved, order alternated per repetition, n = 4, user CPU, 256x256 at
96 spp with 3 training iterations:

| scene | base (s) | fix (s) | paired delta |
|---|---|---|---|
| PT RIS, all `schlick_material` | 55.79 +/- 0.84 | 56.06 +/- 1.32 | +0.48% (t = +0.57) |
| PT one-sample, all schlick | 45.57 +/- 0.85 | 44.65 +/- 1.48 | -1.97% (t = -0.91) |
| PT RIS, schlick wall + lambertian floor | 49.59 +/- 0.96 | 49.81 +/- 1.13 | +0.44% (t = +0.69) |
| BDPT RIS (eye + light guiding), all schlick | 97.61 +/- 1.99 | 97.93 +/- 2.20 | +0.33% (t = +0.90) |
| BDPT one-sample, all schlick | 82.50 +/- 2.17 | 82.95 +/- 3.39 | +0.57% (t = +0.31) |

No row is significant.  The `Pdf()` count per guided vertex did not grow:
PT RIS now evaluates candidate 0's aggregate but reuses the traced
direction's aggregate for the MIS partner; BDPT reuses it for `pdfFwd`.
The new work is `alphaBar` (a loop over at most a few container entries)
and, in BDPT, initialising the guide distribution at vertices whose
selected lobe it never replaces.

## 7. Behaviour changes worth knowing

* Guided renders of multi-emit materials move; un-guided renders are
  bit-identical (every change sits behind an active guiding field).
* BDPT RIS at `alpha = 0` (the warm-up iteration) no longer resamples.
* BDPT consumes the guide distribution's `Get1D` at any connectible
  non-delta vertex whose container has an eligible lobe, not only when the
  selected lobe is eligible -- a sampler-stream change, not a bias.

## 8. Residuals

* `CompositeSPF` (DL-24 / DL-221): its 50/50 `Pdf()` is a placeholder, not
  the generating density.  Under this rule that is a VARIANCE matter
  (unbiasedness holds for any deterministic `W`), except where its `Pdf`
  reads 0 at a direction a lobe really generated (the `W_b = 1` guard
  double-counts there if `value()` is nonzero), and its random-walk lobe
  SET can make "the guide can fire here" a random event (§2's condition).
  Neither is measured; both belong to DL-24's rework.
* The one-sample partition uses `a = alpha_nom` while a glossy lobe is
  replaced with probability `alpha/2`; unbiased by §2, and it is the same
  partition NEE uses, but it is not the balance heuristic for a
  mixed-alpha vertex.  A variance question, not a bias.
