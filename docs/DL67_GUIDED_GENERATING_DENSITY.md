# DL-67: the guided continuation at a multi-lobe SPF, priced on ONE partition

Slice `debt-dl67`, 2026-09-27/28, branched from `master` `6b91fd19`.  Three
rounds: round 1 (`1a2c7c67`); round 2 (`d56ace70`), which answers an
external review (FAIL, two P1s -- both about the round-1 rule's PREMISES,
not its algebra); and round 3, which answers a second review (FAIL, one
P1: round 2's vertex-level rule let the one-sample firing probability
reach 1, which drops every delta lobe -- a third premise).  Ledger row: [DEBT_LEDGER.md](DEBT_LEDGER.md) DL-67; residual
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
alpha `alpha_nom`, BDPT's `guidingAlpha`, both clamped to
`[0, kGuidingMaxOneSampleProbability = 0.9]` by
`PathTransportUtilities::GuidingOneSampleProbability` -- premise 3, §2)
or always (RIS), and the two
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
  bounce limits (premise 4, §2 -- a documented limitation) and resolves
  its medium through `GuidedContinuationIORStack` from the material.
* A delta lobe keeps `W_b = 1` (the guide cannot produce a delta
  direction) but is divided by `1 - a`, the probability it was kept --
  which is why `a` must stay strictly below 1 (premise 3).
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
  own contribution magnitude, and a guide candidate whose target-based
  weight is unusable (`target1 <= 0` or a vanishing proposal density) by
  ITS contribution magnitude (round 3; measure-zero before).

**BDPT's `pdfFwd` has one rule, guided or not**: the aggregate
`ISPF::Pdf()` at the direction actually traced -- the MIS-partner
function `pdfRev` and every connection strategy evaluate.  A substituted
direction marks the vertex non-delta.  The zero-aggregate fallback is the
density the continuation's own weight corresponds to (`equivScatterPdf`).

## 2. Why it is unbiased -- and its four premises

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
`W` under the premises below.  Rounds 1 and 2 stated only the first
two; the reviews found the third and fourth:

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
   polished render's NEE, outside this row.  **DL-285** carries it (fixed
   on `debt-dl285`, 2026-09-28 -- see §8).

3. **`0 < a < 1` wherever the BSDF technique owns mass the guide cannot
   reach.**  A delta lobe is such mass: `W_b = 1` there, priced
   `kray/q/(1 - a)` on the `1 - a` of samples that keep it.  At `a = 1`
   those samples never occur and the delta transport vanishes -- the
   DL-74 side condition ("a weight must be zero wherever its own
   strategy's density is") failing in the other direction.  Round 2 hit
   this: PT's `alpha_nom = pathguiding_alpha * 2 * sigma(cell)` was
   clamped to EXACTLY 1, and the learned sigmoid tops out at ~0.9997, so
   any `pathguiding_alpha` above ~0.5002 gave `a = 1` in a saturated cell
   (this slice's own topology-L rows use 0.7); with learned alpha off,
   `pathguiding_alpha 1.0` did it directly, in both integrators.  Affected:
   any guided material with a delta lobe -- `subsurfacescattering_material`
   / `randomwalk_sss_material` at roughness 0 (the shipped
   `composite_wacky_creature.RISEscene` uses roughness 0.0), weave/fabric
   gaps, polished at very high scattering.  Base and round 1 were immune
   (a delta lobe was never guide-eligible).  Round 3 clamps `a` to
   `[0, 0.9]` in both integrators (`GuidingOneSampleProbability`; PT
   clamps `alpha_nom` once at `Configure`, so NEE's partner and the
   continuation's partition stay one value).  0.9 rather than `1 - 1e-3`
   because the delta lobe's weight is amplified by `1/(1 - a)`: the cap
   bounds that variance factor at 10, while the guide's own share barely
   changes between 0.9 and 1 (`p_agg` is small wherever the guide
   matters).  The same clamp stops a configured alpha above 1 (learned
   alpha off was never clamped) from making `p_mix` negative -- round 2
   had also dropped the old `combinedPdf > NEARZERO` gate (row (t), §5.1:
   alpha 1.5 read -2.25%).
4. **The continuation state after the choice must not depend on WHICH
   technique chose the direction.**  NOT met, and documented rather than
   fixed: a guide draw continues as `eRayDiffuse` (for the per-type
   bounce limits -- `PropagateBounceLimits` in PT,
   `ExceedsBounceLimitForType` in BDPT -- and for `filter_glossy`) while a
   kept lobe keeps its own type, so under a per-type cap the two
   techniques integrate differently truncated paths.  The reviewer
   measured topology L, PT, `max_diffuse_bounce 1`, 1024 spp, n = 3:
   guided RIS 0.0569377 (sd 0.025%) vs un-guided 0.0572795 (sd 0.009%) =
   **-0.60%**; at `max_diffuse_bounce 5` -0.06% (+0.02% at 4096 spp).
   The per-type caps default to unlimited, so default renders are not
   affected.  A fix would book the guide draw with the type the aggregate
   would attribute its direction to, which needs per-lobe densities at an
   arbitrary direction that the SPF interface does not expose.

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
  `rs2.type` (it used to inherit the randomly selected lobe's type) --
  premise 4's documented limitation.
* Round 3: the one-sample firing probability is capped at 0.9 in PT
  (including the NEE partner's `alpha_nom`) and BDPT.  Variance-only
  wherever no delta lobe is present; a configured alpha above 0.9 now
  behaves as 0.9.
* Round 3: PT's Accurate-AOV capture keys on the SELECTED lobe being
  non-delta (as un-guided), not on the traced direction -- round 2 let a
  guide draw add a capture at a vertex whose kept lobe was delta, so the
  AOV depended on which technique fired.
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
every env row within 0.17% at 10x the samples (1.6M per row).  Round 3
added (s) a SMOOTH `subsurfacescattering_material` (all lobes delta; the
reference is the un-guided control at 2x the samples, which is
deterministic here) and (t) a white Lambertian at alpha 1.5: round 2
(`d56ace70`) reads **182/3** on round 3's file -- (s) one-sample fixed
alpha 1.0 **exactly 0**, (s) learned alpha 0.7 at a cell driven to
sigma 0.9984 **exactly 0**, (t) **-2.25%**; round 3: **185/0**, (s)
-0.82% / +0.44% (fixed 1.0 / learned), (t) -0.01%.  Rows:

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
| (s) smooth sss, one-sample fixed alpha 1.0 (round 3) | +0.00% (review) | -- | round 2 **0 exactly**, round 3 -0.82% |
| (t) lambertian, one-sample alpha 1.5 (round 3) | -- | -- | round 2 **-2.25%**, round 3 -0.01% |

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

Round 3 adds, on per-FIXTURE seed blocks (round 2's ranges restarted at
the same base in every fixture, so rows of different fixtures shared
streams -- review P3):

* one eye furnace per remaining multi-lobe SPF family --
  `ward_isotropic_material` (alpha 0.3), `ward_anisotropic_material`
  (0.2 / 0.4), `isotropic_phong_material` (N 20),
  `ashikminshirley_anisotropicphong_material` (nu 40, nv 10) -- 8 guided
  rows each.  On BASE `6b91fd19` all 32 are red: **+18.9% .. +111%**
  (Ward iso), **+20.5% .. +111%** (Ward aniso), **+30.4% .. +119%**
  (Phong), **+24.8% .. +75.6%** (Ashikhmin); base reads 74/90 on round 3's
  file.  Round 2 and round 3 are green on them.
* a SMOOTH `subsurfacescattering_material` eye furnace against a 4x
  un-guided reference (combined-standard-error z, |z| <= 4): round 2 reads
  **exactly 0** at one-sample alpha 1.0 (z = -120); round 3 reads
  0.0201969 +/- 0.00109 against 0.0206389 +/- 0.00017 (z = -0.40).

Round 2 on round 3's file: **163/1**.  Round 3: **164/0**, largest |z|
over the printed rows 2.03.

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

### 5.4 Which SPFs are verified, and how

| SPF family | generator-level (`BDPTGuidedContinuationTest`) | scene-level render |
|---|---|---|
| Lambertian | eye + light rows (incl. tilted normal) | PT furnaces (`PTGuidingMISPartitionTest` DL-74 rows, (r)) |
| `SchlickSPF` (incl. black diffuse, tilted normal) | eye + light + NM rows | topology L guided rows (`BDPTStrategyBalanceTest`) |
| `TranslucentSPF` | eye + light rows | PT furnace row (n) |
| Ward isotropic / anisotropic | eye rows (round 3) | reviewer's topology L, 1024 spp: +0.025% / +0.024% (base BDPT +54.2%), -0.051% / -0.006% (base +55.6%) |
| `IsotropicPhongSPF` | eye rows (round 3) | reviewer: +0.011% / -0.046% (base +52.9%) |
| `AshikminShirleyAnisotropicPhongSPF` | eye rows (round 3) | reviewer: -0.048% / -0.011% (base +62.1%) |
| smooth SSS (delta lobes only) | eye row (round 3) | PT furnace row (s) |
| `PolishedSPF` | -- | premise 2 restored by DL-285 (2026-09-28): `PolishedBRDFConsistencyTest` + `BDPTStrategyBalanceTest` / `VCMStrategyBalanceTest` topology AB (§8) |
| `CompositeSPF` | -- | not verified (DL-24/DL-221) |
| single-emit SPFs (GGX, Cook-Torrance, Coated) | -- | argued: selectProb is identically 1, so the partition reduces to the pre-DL-67 single-lobe case |

(PT scene-level numbers in the reviewer column are PT / BDPT guided RIS vs
un-guided of the same integrator.)

### 5.5 The residual: `polished_material` (DL-285)

Topology L with `polished_material` (reflectance 0.4, tau 0.9, ior 1.5,
scattering 20), guided RIS vs un-guided of the SAME integrator:

| | base | round 1 (review) | round 2, 1024 spp |
|---|---|---|---|
| PT | -10.4% | -1.60% | **-1.63%** |
| BDPT | +1.85% | -1.52% | **-1.18%** |

Un-guided PT and BDPT agree (+0.02%).  Premise 2 of §2, not a DL-67
defect.  Closed by DL-285 (§8).

## 6. Gate (current counts, from this round's run logs)

Listed in the ledger row and the final report.  `TranslucentIORStackTest`'s
BDPT probe needed two round-1 changes (look the guided density up by
direction; `>=` on the query count) and one round-2 change: the probe
forces an ENTRY transmission, which round 2 now guides (round 1 never
guided a translucent lobe), so it forces that vertex's technique choice
onto the kept lobe as well -- it observes the EXIT.

## 7. Cost

**Round 2's table below understated BDPT's cost.**  The external review
re-measured with its own interleaved, alternating-order A/B on topology L
(all `schlick_material`, 256x256, 96 spp, 3 training iterations, LIGHT
guiding on: `pathguiding_light_max_depth 4`):  BDPT RIS **+10.63%**
(n = 4, t = 14.9), **+8.94%** (t = 8.4) and **+8.79%** (repeat); BDPT
one-sample **+3.56%** (t = 3.2); PT RIS +2.62% (n.s.); PT one-sample
**-10.04%** (t = -5.7).  Round 3 re-measured the same scenes with the
reviewer's own generator (`gen.py`) and two freshly built binaries
(`6b91fd19` vs round 3), n = 4, order alternated, on a heavily loaded
machine (load average 25-54, another worker active):

| scene (reviewer's) | base (s) | round 3 (s) | paired delta | reviewer |
|---|---|---|---|---|
| BDPT RIS | 91.63 +/- 0.99 | 95.80 +/- 1.16 | **+4.55% (t = 8.0)** | +10.63% / +8.94% / +8.79% |
| BDPT one-sample | 78.06 +/- 1.78 | 78.67 +/- 0.58 | +0.83% (t = 0.7) | +3.56% (t = 3.2) |
| PT RIS | 54.70 +/- 1.15 | 55.67 +/- 0.44 | +1.81% (t = 1.7) | +2.62% (n.s.) |
| PT one-sample | 42.22 +/- 1.46 | 40.03 +/- 0.73 | **-5.10% (t = -2.5)** | -10.04% (t = -5.7) |

The honest reading: **BDPT RIS costs +4.5% to +10.6%** (both measurements
significant; they disagree on size, most plausibly through machine load),
BDPT one-sample 0 to +3.6%, PT RIS ~+2% (not significant in either), and
PT one-sample is 5-10% FASTER.  Round 2's measurement (below) ran without
light guiding.

Round 2's own measurement -- two separately built binaries (`6b91fd19` vs
round 2), interleaved, order alternated per repetition, n = 4, user CPU,
256x256 at 96 spp, 3 training iterations, no light guiding:

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
  §5.5).  **Fixed on `debt-dl285` (2026-09-28).**  `PolishedBRDF` is both
  `PolishedMaterial::GetBSDF()` and the function `PolishedSPF` samples
  lobe by lobe (each ray's kray is its own lobe's `f_I co / p_I`).  The
  pre-fix SPF's implied BRDF was not reciprocal, so the SPF side moved
  (DL-127's ruling) to a reciprocal, provably energy-bounded model: coat
  `tau min(F(ci),F(co)) P(cos alpha) 2/(ci+co)`, substrate
  `Rd (1-F(ci))(1-F(co)) / (pi T_avg)` (directional albedo `Rd (1-F(ci))`
  kept exactly).  `Pdf` is the realized `RandomlySelect` density: both
  lobes' selection weights depend on their own draws, so one replay
  quadrature per lobe over the other lobe's draw, memoized per shading
  point.  Topology L polished (the rows above), 1024 spp, n = 6 salted
  renders from two interleaved binaries, guided RIS vs un-guided of the
  same integrator: **PT -1.551 +- 0.051 % -> -0.002 +- 0.015 %, BDPT
  -1.666 +- 0.192 % -> +0.012 +- 0.008 %**; VCM / PT -1.746 % -> -0.041 %
  (VCM prices `value` too, so it was off un-guided; the post-fix residuals
  are the QMC point-set floor -- a disjoint salt set reads VCM/PT +0.002 %,
  BDPT/PT +0.018 %).  Shipped renders move (kaleidoscope_atrium -5.0 %)
  because NEE and connections now include the coat's (1-F) transmission
  loss and its highlights: the old bare-Lambertian BSDF over-counted, and
  ANY consistent model corrects that (the old non-reciprocal weights made
  consistent: -6.2 %), not the reciprocal coat (the coat choice moves
  scenes <= 0.4 pt).  Gated in
  `BDPTStrategyBalanceTest` / `VCMStrategyBalanceTest` topology AB (0.5 %
  band).  Premise 2's audit found one more material that breaks it,
  in the opposite direction: `datadriven_material` has a BSDF and NO SPF
  (**DL-325**, open).  Full account: the DL-285 ledger row.
* `CompositeSPF` (DL-24 / DL-221): its 50/50 placeholder `Pdf()` is a
  variance matter under round 2 (the realization-independence premise no
  longer depends on the SPF), except where it reads 0 at a generated
  direction (guard double count if `value()` is nonzero there) and
  wherever its composite `value` and random-walk `kray` disagree
  (premise 2).  Unmeasured; DL-24's rework.
* Glossy "half-trust" is gone: the guide now takes probability `a` at a
  glossy vertex too.  Unbiased by §2; a variance question for strongly
  glossy scenes, not measured beyond the cost table.
* **Premise 4** (§2): under a per-type bounce cap a guide draw and a kept
  lobe continue under different types -- -0.60% at PT
  `max_diffuse_bounce 1` on topology L (reviewer), -0.06% at 5, nothing at
  the default (unlimited).  Documented, not fixed.
* **Variance at a delta-only vertex** (review P2, same root as premise 3):
  a vertex whose lobes are all delta (smooth SSS) is still guided, and
  there the guide fires with probability `a` and always contributes 0,
  so the delta transport's variance grows by ~`1/(1 - a)` --
  `BDPTGuidedContinuationTest`'s smooth-SSS row reads standard error
  0.00063 at alpha 0.7 against base's 0.00034 (3.4x variance), 0.00109
  at the 0.9 cap.  Not guiding such a vertex needs a REALIZATION-
  INDEPENDENT "every lobe is delta" query (premise 1), and none exists:
  `SpecularInfo::isSpecular` means "HAS a delta interaction" (it is true
  for `polished_material`, whose diffuse substrate the guide should
  cover).  Left as a variance cost.

### 8.1 DL-307 (closed on `debt-dl307`, 2026-09-28): the un-guided twin of the empty-container rule

This slice's own round-3 review (P2-1) found that the rule "an empty or
unselectable container is a zero sample of the BSDF technique, not a
reason to skip the other technique" had been applied to the GUIDE
technique only.  Both BDPT generators still `break` on an empty container
at an UN-guided vertex BEFORE the BSSRDF / random-walk entry branch --
another technique whose Fresnel coin is independent of the Scatter
realization.  A rough `subsurfacescattering_material` /
`randomwalk_sss_material` front reflection drawn below the horizon is
dropped by `SubSurfaceScatteringSPF::Scatter`, the container comes back
empty, and the subsurface branch (taken with probability `Ft`) was lost.
PT takes that branch before it ever scatters; guided BDPT took it
because a guided vertex survives an empty container.  So un-guided and
guided BDPT disagreed, and un-guided BDPT, VCM and MLT (shared
generators) were biased dark.

Fix (`BDPTIntegrator.cpp`, both generators): a vertex with no selectable
lobe whose material has a subsurface-entry branch
(`HasSubsurfaceEntryBranch<Tag>`: a diffusion profile, random-walk
params, or the NM per-wavelength params -- queried lazily, only at a
vertex with no lobe) rides the existing placeholder into the subsurface
branch with the SAME coin, and terminates right after it if the branch
does not continue.  The spawned entry vertex's bookkeeping (`isDelta`,
`isConnectible`, `isBSSRDFEntry`, `pdfFwd`, the exit vertex marked delta)
is the non-empty case's own code; the guided path and the DL-126
null-BSDF route are untouched.

Measured (32x32, 1024 spp, salted Sobol', two separately built binaries
run interleaved; BDPT/PT - 1):

| topology | pre-fix | post-fix |
|---|---:|---:|
| U rough (0.3) diffusion sheets, un-guided BDPT (n = 16) | **-0.830 %** (z -29.7) | -0.033 % (z -1.1) |
| U, guided RIS BDPT (n = 16) | -0.048 % | -0.033 % |
| U, VCM (n = 16) | **-6.000 %** | -5.242 % (DL-317) |
| V rough (0.8) random-walk sphere, depth 16, un-guided BDPT (n = 8) | **-10.385 %** (z -339) | -0.015 % (z -0.4) |
| V, guided RIS BDPT (n = 8) | -0.076 % | +0.018 % |
| random walk on U's zero-thickness sheets (n = 8) | -0.06 % +/- 0.16 % | -- |

Post-fix guided and un-guided BDPT agree within noise: on U the n = 16
salted means differ by 3e-7 against a standard error of ~1.1e-5 (z
-0.01 -- the near-identity is a coincidence) and the external review's
independent n = 8 reads -0.016 % (z -1.7); on V within 1.3 sd (review:
-0.058 %).  The random-walk variant the reviewer
read as "-0.24 %, inconclusive" on U's sheets is resolved: a walk into a
zero-thickness sheet transports almost nothing, so the sheet cannot see
the defect; a closed sphere reads -0.45 % at roughness 0.3 and -10.4 %
at 0.8.  Under a directional light (the zero-exitance sweep reaches only
entry vertices the eye generator spawned) BDPT read **-23.9 %**
(random walk) / **-24.4 %** (diffusion) on roughness-0.8 spheres.  At
depth 5 a closed sphere keeps a -0.10 .. -0.20 % BDPT residual that
vanishes at depth 16: PT's per-type bounce caps and BDPT's per-surface-
vertex cap truncate multi-event subsurface paths differently -- depth
semantics, not bias.  VCM carries a separate, pre-existing defect in its
MIS running quantities at a BSSRDF / random-walk entry, with or without
merging (DL-317): -5 % on U's wall-dominated frame, -15.1 % on V, and
-78 % .. -97 % on frames the SSS object fills.

The external review of `2212f537` also verified the LIGHT-subpath half
(closed sphere, area light behind it, 40x40, 1024 spp, depth 16,
BDPT/PT - 1): diffusion r0.8 -23.17 % -> -0.52 %, r0.3 -1.55 % ->
+0.02 %; random walk r0.8 -22.74 % -> +1.46 %, r0.3 -1.58 % -> +0.80 %;
spectral BDPT on V -10.4 % -> -0.22 %.

Shipped scenes (quarter resolution, 256 spp, n = 3..6 per build,
interleaved; these are PER-PIXEL-CLAMPED means, which keep fireflies out
of the comparison and so read differently from a plain mean):
`bdpt_sss_dragon` +1.24 % (t 4.9), `vcm_sss_dragon` +2.29 % (t 4.7),
`bdpt_sss_different_bsdf` (VCM, roughness 0.5) +1.86 % (t 20),
`rwsss_bdpt` (roughness 0.05) +0.03 % (n.s.).  The review's PLAIN means:
`bdpt_sss_dragon` +1.56 % (t 2.7), `bdpt_sss_different_bsdf` +0.96 %
(t 3.1).  Tests: `BDPTStrategyBalanceTest` topologies U/V
(`--sss-only`; U gates the mean of 3 salted 2048-spp replicates per
integrator -- one unsalted draw read -0.34 % against its 0.35 % band),
`VCMStrategyBalanceTest` topology U (a DL-317 pin),
`BDPTZeroExitanceBSSRDFTest` Part E.
