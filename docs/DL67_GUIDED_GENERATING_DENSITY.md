# DL-67: the guided continuation at a multi-lobe SPF, priced on ONE partition

Slice `debt-dl67`, 2026-09-27/28, branched from `master` `6b91fd19`.  Two
rounds: round 1 (`1a2c7c67`) and round 2, which answers an external review
(FAIL, two P1s -- both about the round-1 rule's PREMISES, not its
algebra).  Ledger row: [DEBT_LEDGER.md](DEBT_LEDGER.md) DL-67; residual
DL-285.  Predecessors:
[DL67_SLICE0_SCHLICK_PDF_WEIGHTS.md](DL67_SLICE0_SCHLICK_PDF_WEIGHTS.md)
(the aggregate `ISPF::Pdf()` made the true generating density),
[DL74_ENV_NEE_GUIDING_PARTITION.md](DL74_ENV_NEE_GUIDING_PARTITION.md)
(sampling density vs MIS-partner density),
[DL69_BDPT_LOBE_THROUGHPUT.md](DL69_BDPT_LOBE_THROUGHPUT.md) and
[DL103_PT_ESCAPE_MIS_PARTNER.md](DL103_PT_ESCAPE_MIS_PARTNER.md) (the
un-guided halves).

## 1. The one rule (round 2)

A guided surface vertex runs two techniques on the continuation integral
`I = integral f(w) cos(w) L(w) dw`:

* the **BSDF technique** -- `Scatter` emits every lobe, `RandomlySelect`
  keeps lobe `I` with realized probability `q_I`, unweighted estimator
  `kray_I / q_I` (the SPF contract `kray_I = f_I cos / p_I`);
* the **guide technique** -- `w ~ g` from the trained field, unweighted
  estimator `f(w) cos / g(w)` with `f` the AGGREGATE BSDF (`IBSDF::value`).

**Which vertices are guided, and with what probability the guide fires,
is decided from the VERTEX, never from the Scatter realization.**  A
vertex is guided when the field is active there (trained, depth, alpha,
non-specular arrival -- the same gate `PTGuidingMisPdf` uses for NEE) and
the material has a BSDF for the guide technique to price.  At a guided
vertex the guide fires with probability `a` (one-sample: PT's nominal
alpha `alpha_nom`, BDPT's `guidingAlpha`) or always (RIS), and the two
techniques are combined through ONE deterministic partition:

```
W_g(w) = a g(w) / (a g(w) + (1 - a) p_agg(w)),        W_b(w) = 1 - W_g(w)

lobe I kept, non-delta   X_b = kray_I / q_I * W_b(w_I) / (1 - a)
lobe I kept, delta       X_b = kray_I / q_I            / (1 - a)
guide draw               X_g = f(w) cos / g(w) * W_g(w) / a  =  f(w) cos / p_mix(w)
                         p_mix = a g + (1 - a) p_agg
```

`p_agg` is the aggregate `ISPF::Pdf()` (a = 1/2 in RIS).  In one-sample
mode the guide draw's density is literally `p_mix`, the same nominal
function NEE is weighted against (DL-74).  Details:

* An EMPTY or unselectable container at a guided vertex is a zero sample
  of the BSDF technique -- the guide technique still fires with
  probability `a`.  The walk continues on a placeholder `ScatteredRay`
  (non-delta, zero weight); a guide draw ALWAYS continues on that
  placeholder (a guide draw is a non-delta event of its own, not the
  selected lobe's), so it counts as a diffuse bounce for the per-type
  bounce limits and resolves its medium through
  `GuidedContinuationIORStack` from the material.
* A delta lobe keeps `W_b = 1` (the guide cannot produce a delta
  direction) but is divided by `1 - a`, the probability it was kept.
* Where `p_agg(w_I) <= 0` at a direction a lobe really generated,
  `W_b := 1` (DL-103's guard, no known production inhabitant since DL-41
  closed); in RIS the lobe's own `selectProb * pdf` also stands in for
  candidate 0's proposal density in the (free) resampling weight.
* A guide draw that yields nothing is a ZERO sample -- never a fall-back
  to the lobe.
* RIS: both candidates at every guided vertex; resampled output
  `c_y * sum(w) / w_y` with `c_0 = X_b`-without-`1/(1-a)` and
  `c_1 = f cos / (g + p_agg)`.  A weight only has to be positive where its
  contribution is: a delta or target-less candidate 0 is weighted by its
  own contribution magnitude.

**BDPT's `pdfFwd` has one rule, guided or not**: the aggregate
`ISPF::Pdf()` at the direction actually traced -- the MIS-partner
function `pdfRev` and every connection strategy evaluate.  A substituted
direction marks the vertex non-delta.  The zero-aggregate fallback is the
density the continuation's own weight corresponds to (`equivScatterPdf`).

## 2. Why it is unbiased -- and its two premises

Condition on the Scatter realization `R` and let the technique coin be
independent of `R` with probability `a`:

```
E[F | R] = a * integral g X_g dw  +  (1 - a) * sum_I q_I X_b(I)
         = integral f cos W_g dw  +  sum_I kray_I V_I(w_I)
```

with `V_I = W_b` for a non-delta lobe and 1 for a delta lobe.  Taking
`E_R` with the SPF contract `E_R[sum_I kray_I h(w_I)] = integral f_SPF cos h`
gives `integral f_BSDF cos W_g + integral f_SPF cos W_b` (the delta part of
`f_SPF` rides entirely on `V = 1`).  That equals `I` for ANY deterministic
`W` under exactly two premises:

1. **The technique choice is independent of the realization.**  Round 1
   violated this.  It let the guide fire only when the SELECTED lobe was
   guide-eligible, with a per-lobe alpha, and divided the guide draw by
   the realized `alphaBar = sum_J q_J alpha_J`.  That is exact only when
   "some lobe can fire the guide" has probability 0 or 1 at the vertex.
   Round 1's doc claimed only `CompositeSPF` could break that -- false.
   A realization can hold only a ZERO-weight eligible ray
   (`schlick_material` with a black diffuse: the diffuse ray is still
   emitted and the specular draw is rejected on a sizeable fraction of
   calls), or NO eligible ray (a diffuse draw dropped below the GEOMETRIC
   horizon under a tilted shading normal, in `LambertianSPF` and
   `SchlickSPF`; PT also broke out of the walk on an empty container
   before the guide could fire).  The bias is `P(no eligible lobe) *
   integral f cos W_g`, measured below.  Round 2 makes the choice
   vertex-level, which removes the premise instead of enumerating its
   exceptions.
2. **`IBSDF::value` and the SPF's `kray` describe ONE function**
   (`f_BSDF == f_SPF`).  The guide prices `value`, the kept lobes price
   `kray`.  `polished_material` violates it: its BSDF is a bare
   `LambertianBRDF(Rd)` (`PolishedMaterial.h`, the defect DL-R21 records
   and routed the wetness verb around) while its SPF samples a dielectric
   coat over an attenuated substrate, so guiding moves the expectation
   (-1.6% PT, -1.2% BDPT, §5).  DL-67 cannot close that without building
   a `PolishedBRDF` that matches `PolishedSPF` -- a model change to every
   polished render's NEE, outside this row.  **DL-285** carries it.

`p_agg`'s exactness is a variance matter (the balance-heuristic choice of
`W`), except through the zero-aggregate guard.

### 2.1 The RIS identity

With `W_g = g / (g + p_agg)`, `c_0 = kray_I/q_I * W_b(x_0)`,
`c_1 = f cos(x_1) / (g + p_agg)(x_1)`, and weights
`w_i = p_hat(x_i) / (0.5 (p_agg + g)(x_i))`, the output
`c_y (sum w) / w_y` has conditional mean `c_0 + c_1`; at a single-lobe
material it reproduces the standard two-sample balance RIS estimator.  The
identity `sum_i p_i(x) / sum_j p_j(x) = 1` holds because both candidates
evaluate the same `p_agg` (pre-fix candidate 0 used `p_I`).

### 2.2 The synthetic two-lobe SPF

`PTGuidingMISPartitionTest`'s `MultiLobeSPF`: constant krays
`c_A = 0.3`, `c_B = 0.7`, `q_I = c_I`, `p_agg = sum c_I p_I`,
`f cos = p_agg`, albedo 1.  One-sample:

```
guide:  a * integral g * (f cos / g) * (a g / p_mix) / a   = integral a g p_agg / p_mix
kept:   sum_I c_I (1 - a) * integral p_I * (c_I / c_I) * (1 - a) p_agg / ((1 - a) p_mix)
      = integral (1 - a) p_agg^2 / p_mix
sum:    integral p_agg (a g + (1 - a) p_agg) / p_mix  =  integral p_agg  =  1
```

Pre-fix the kept term was `(1 - alpha) sum_I c_I integral p_I^2 /
(alpha g + (1 - alpha) p_I)`, visible only where `g` is comparable to
`p_I` -- which is why every DL-67 row runs under a sharp (cos^64) AND a
broad (cos^2) trained field.

## 3. Sites

1. **PT PART 3** (`PathTracingIntegrator.cpp`, RGB/NM templated body).
   PT HWSS has no guided block.  The per-lobe helpers
   `GuidingSupportsSurfaceSampling` / `GuidingEffectiveAlpha` were removed
   from PT (the glossy "half-trust" and the transmission exclusion no
   longer steer sampling; the partition weights handle a narrow lobe).
2. **BDPT eye** and 3. **BDPT light** generators (`BDPTIntegrator.cpp`),
   one helper `BDPTGuidedContinuation`; a kept DELTA lobe carries the
   technique factor too (`deltaGuideScale`, companions included).
4. **`pdfFwd`** in both generators (§1).

VCM shares the generator but never installs a field; MLT constructs its
own `BDPTIntegrator` and never installs one.  No guided path to test.

### 3.1 HWSS

Kept lobe: companions use `EvaluateKrayNM(lambda_c) * krayScale` (delta:
`krayNM * deltaGuideScale`), `krayScale` carrying the same partition
factor as the hero, and `scatterType` = the lobe's type, so
`RecomputeSubpathThroughputNM`'s `f_I` ratio cancels the factor.
Substituted: `eRayUnknown`, aggregate ratio (DL-125).  Weights are
computed at the hero wavelength and shared by every lane -- a partition
only has to sum to one per lane.  `pathtracing_spectral_rasterizer` does
not accept `pathguiding`, so PT spectral guided renders cannot be built.

### 3.2 Audited, not changed

`RayCaster::CastRay`'s two volume guided sites and PT's in-loop volume
guided vertex: one-sample over a single-lobe phase function with one
alpha, so `GuidingCombinedPdf(alpha, g, phasePdf)` is the true density;
their fall-back fires only on `guidePdf <= 0` at a SAMPLED direction.

## 4. Behaviour changes

* Guided renders of multi-emit materials move; un-guided renders are
  unaffected (every change sits behind an active guiding field).
* The guide now fires at every guided vertex with probability `a`,
  including when the selected lobe is delta, glossy or a transmission --
  those lobes pay `1/(1 - a)` or `W_b/(1 - a)`.  A material with NO BSDF
  (dielectric, perfect reflector/refractor, `biospec_skin_material`,
  `generic_human_tissue_material`) is not guided at all; round 1's RIS
  killed the walk at the null-BSDF materials.
* A guide draw is a diffuse-type event for per-type bounce limits and
  `rs2.type` (it used to inherit the randomly selected lobe's type).
* BDPT RIS at `alpha = 0` (the warm-up iteration) no longer resamples;
  BDPT spends the guide distribution's `Get1D` at every guided vertex.
* Training-only: PT RIS's candidate-0 pick now reports
  `effectiveBsdfPdf = (g + p_agg) w_0 / sum(w)` (pre-fix
  `risEffectivePdf`), so `bsdfTimesCos` is `kray p_agg/(q (g+p_agg))` times
  that -- the optimal-MIS training moment and OpenPGL's `pdfDirectionIn`,
  never a weight.

## 5. Red-proofs

Isolated `git checkout <commit> -- <files>` reverts of COMMITTED state,
rebuilt (library and `build-test/<Name>`), run, restored.

### 5.1 `PTGuidingMISPartitionTest`

Base `6b91fd19`: 141/13 on round 1's rows.  Round 1 (`1a2c7c67`) on
round 2's file: **175/3** -- the new tilted-shading-normal Lambertian
rows read **-3.04% / -3.45% / -2.47%** (one-sample fixed / learned / RIS,
broad field; sharp field ~ -1%), tilted Schlick -0.55..-0.87% (inside
band).  Round 2: **178/0**, every row within 0.83% of its reference, and
every env row within 0.17% at 10x the samples (1.6M per row).  Rows:

| row | base | round 1 | round 2 |
|---|---|---|---|
| (l) two-lobe env, one-sample broad | +4.59% | -0.20% | +0.29% |
| (l) two-lobe env, RIS broad | -9.57% | -0.18% | +0.08% |
| (m) schlick env, RIS sharp | +5.45% | -0.03% | -0.09% |
| (n) translucent env, one-sample broad | +8.81% | -0.22% | +0.08% |
| (q) black-diffuse schlick env (new) | -- | within 0.37% | within 0.54% |
| (r) tilted lambertian env, one-sample broad (new) | -- | **-3.04%** | +0.30% |
| (r) tilted lambertian env, RIS broad (new) | -- | **-2.47%** | +0.05% |
| (r) tilted schlick env (new, worst) | -- | -0.87% | -0.56% |

PT read fine on the black-diffuse case even in round 1: PT's per-lobe
rule admitted the glossy lobe too, which kept `alphaBar > 0`.

### 5.2 `BDPTGuidedContinuationTest` (new this slice)

Base: 29/33 on round 1's rows (schlick eye one-sample +137.9%, LAMBERTIAN
+165.7% / +233.3% from the fall-back-on-zero-draw defect).  Round 1 on
round 2's file: **95/15** -- black-diffuse schlick eye one-sample
**+14.1%**, RIS **+10.4%** (light side the same), tilted Lambertian eye
**-3.43%**, light **-4.01%**.  Round 2: **110/0**.  Rows now draw from
DISJOINT seed ranges (round 1's `seed += 1000` with 200000 samples made
consecutive rows share ~99.5% of their seeds, so its "every eye row within
2.5 standard errors" was not a set of independent checks).

### 5.3 `BDPTStrategyBalanceTest` guided topology L (2% band)

| row (256 spp) | base | round 1 | round 2 |
|---|---|---|---|
| BDPT guided RIS vs un-guided PT | +19.07% | -0.02% | -0.07% |
| PT guided RIS vs un-guided BDPT | PT +7.46% | PT -0.17% | PT -0.13% |
| BLACK-diffuse: BDPT guided RIS vs un-guided PT (new) | -- | **+4.73%** | +0.21% |
| BLACK-diffuse: PT guided RIS vs un-guided BDPT (new) | -- | +0.02% | +0.01% |
| BDPT guided one-sample vs un-guided PT | +0.08% | -0.09% | -0.08% |
| PT guided one-sample vs un-guided BDPT | -0.03% | +0.05% | +0.03% |

**Error bars.**  Repeat renders re-use nearly the same QMC pattern, so a
repeat spread is not Monte Carlo error: six runs with six different
`std::srand` seeds give per-row sd **0.007-0.019%** while the row means sit
at -0.13% to +0.21% at 256 spp.  What IS informative is convergence with
sample count: at 1024 spp the same rows read **-0.08%, -0.04%, +0.05%,
+0.005%, +0.02% (black BDPT RIS), +0.03% (black PT RIS)** -- the
residuals are the pattern error of a 256-spp render (and ~-0.07%
un-guided BDPT-vs-PT), not a bias.  Round 1's black-diffuse +4.73% did
not move between 1024 and 4096 spp (the reviewer's measurement), which is
what a bias looks like.  The one-sample rows fire rarely (adaptive alpha
~0.016 on this scene) and are consistency pins; the generator-level test
is their red-proof.

### 5.4 The residual: `polished_material` (DL-285)

Topology L with `polished_material` (reflectance 0.4, tau 0.9, ior 1.5,
scattering 20), guided RIS vs un-guided of the SAME integrator:

| | base | round 1 (review) | round 2, 1024 spp |
|---|---|---|---|
| PT | -10.4% | -1.60% | **-1.63%** |
| BDPT | +1.85% | -1.52% | **-1.18%** |

Un-guided PT and BDPT agree (+0.02%).  Premise 2 of §2, not a DL-67
defect.

## 6. Gate (current counts, from this round's run logs)

Listed in the ledger row and the final report.  `TranslucentIORStackTest`'s
BDPT probe needed two round-1 changes (look the guided density up by
direction; `>=` on the query count) and one round-2 change: the probe
forces an ENTRY transmission, which round 2 now guides (round 1 never
guided a translucent lobe), so it forces that vertex's technique choice
onto the kept lobe as well -- it observes the EXIT.

## 7. Cost

Two separately built `bin/rise` binaries (`6b91fd19` vs round 2),
interleaved, order alternated per repetition, n = 4, user CPU, 256x256 at
96 spp, 3 training iterations:

| scene | base (s) | round 2 (s) | paired delta |
|---|---|---|---|
| PT RIS, all `schlick_material` | 55.58 +/- 2.53 | 56.10 +/- 2.35 | +0.97% (t = 0.90) |
| PT one-sample, all schlick | 44.27 +/- 0.91 | 40.93 +/- 0.48 | **-7.50% (t = -5.72)** |
| PT RIS, schlick wall + lambertian floor | 48.79 +/- 1.36 | 48.81 +/- 1.14 | +0.09% |
| BDPT RIS, all schlick | 96.66 +/- 2.91 | 99.48 +/- 4.17 | **+2.90% (t = 3.55)** |
| BDPT one-sample, all schlick | 80.64 +/- 2.65 | 81.48 +/- 2.81 | +1.06% (t = 0.75) |

BDPT RIS pays for resampling at vertices whose selected lobe it used to
skip (the specular lobe); PT one-sample got faster (the continuation
throughput distribution changed, which moves Russian roulette and path
length -- not attributed further).  Round 1's own measurement read no
significant change anywhere.

## 8. Residuals

* **DL-285** -- `polished_material`'s BSDF/SPF mismatch (§2 premise 2,
  §5.4).
* `CompositeSPF` (DL-24 / DL-221): its 50/50 placeholder `Pdf()` is a
  variance matter under round 2 (the realization-independence premise no
  longer depends on the SPF), except where it reads 0 at a generated
  direction (guard double count if `value()` is nonzero there) and
  wherever its composite `value` and random-walk `kray` disagree
  (premise 2).  Unmeasured; DL-24's rework.
* Glossy "half-trust" is gone: the guide now takes probability `a` at a
  glossy vertex too.  Unbiased by §2; a variance question for strongly
  glossy scenes, not measured beyond the cost table.
