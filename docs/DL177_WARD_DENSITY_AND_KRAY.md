# DL-177 — Ward's stored density, aggregate `Pdf`, and `kray`

**Status: CLOSED 2026-09-18** (slice `debt-dl176`).
Red-proof and regression gate: `tests/WardDensityKrayTest.cpp`.

This document also carries a short **DL-176** section (§8): the
`SPFPdfConsistencyTest` chi-squared harness, which the same slice fixed.

---

## 1. Three defects, not one

`ward_isotropic_material` and `ward_anisotropic_material` are each one
object with three faces, exactly as DL-127 describes for Schlick:

| face | what it is | who reads it |
| --- | --- | --- |
| `Ward*SPF::Scatter`/`ScatterNM` | emits a diffuse and a specular lobe, each with a `kray` and a `pdf` | every integrator's BSDF-sampled continuation |
| `Ward*SPF::Pdf`/`PdfNM` | the aggregate density of the direction `Scatter` + `RandomlySelect` produce | MIS partners, BDPT/VCM `pdfFwd`/`pdfRev` |
| `Ward*BRDF::value`/`valueNM` | the BRDF | NEE, every BDPT/VCM connection strategy |

All three were wrong, in ways that compound:

1. **The stored per-lobe density was not a density.**
   `GenerateSpecularRay` stored the TRUE solid-angle density of its own
   half-vector **times `cos^4(theta_h)`**.
2. **`Pdf`/`PdfNM` were a raw-albedo-weighted mixture** of the two lobe
   densities — exact only while `kray_S` was direction-independent, and
   defect (3) removes that premise.
3. **`kray_S` was the reflectance painter's own colour**, not that
   lobe's `f_S cos / p_S` — the DL-127 pattern.

They must be fixed in that order: (3)'s correct expression is derived
against (1)'s corrected density, and (2)'s construction is only
*necessary* once (3) has landed.

---

## 2. Derivations

### 2.1 The true half-vector density (defect 1)

**Isotropic.**  `GenerateSpecularRay` draws `phi = 2 PI xi1` and
`theta = atan(alpha sqrt(-ln xi2))`.  Write `t = tan(theta)`; then
`xi2 = exp(-t^2/alpha^2)`, so

```
    P(T <= t) = 1 - exp(-t^2/alpha^2)
    p_T(t)    = (2t/alpha^2) exp(-t^2/alpha^2)
```

With `dt/dtheta = 1/cos^2(theta)`,
`p_Theta(theta) = (2 tan(theta)/alpha^2) exp(...) / cos^2(theta)`, and a
solid-angle density is `p_Theta * p_Phi / sin(theta)` with
`p_Phi = 1/(2 PI)`:

```
    p_h(theta_h) = exp(-tan^2(theta_h)/alpha^2)
                 / ( PI alpha^2 cos^3(theta_h) )                    (1)
```

**Anisotropic.**  `theta = atan(sqrt(-ln xi2 / D))` with
`D(phi) = cos^2(phi)/ax^2 + sin^2(phi)/ay^2`, so
`p_Theta(theta|phi) = 2 tan(theta) D exp(-tan^2(theta) D)/cos^2(theta)`.
The azimuthal warp on the first quadrant is
`tan(phi) = (ay/ax) tan(PI val/2)`, `xi1 = val/4`, whose derivative is

```
    p_Phi(phi) = dxi1/dphi
               = (1/(2 PI)) (ax/ay) / ( cos^2 phi + (ax/ay)^2 sin^2 phi )
               = 1 / ( 2 PI ax ay D(phi) )
```

so the `D` cancels between the two factors and

```
    p_h = exp(-tan^2(theta_h) D(phi)) / ( PI ax ay cos^3(theta_h) )   (2)
```

**What was stored.**  `cos(theta_h) exp(...) / (PI alpha^2)` and
`cos(theta_h) exp(...) / (PI ax ay)` — i.e. (1) and (2) times
`cos^4(theta_h)`.  `tests/WardDensityKrayTest.cpp` section C measures
that per draw and reports `|stored/true - cos^4(theta_h)|`, which reads
**0.000000** at every alpha, incidence and model on the pre-fix build.

The reflection Jacobian `1/(4 (h.wo))` was already applied on top and is
unchanged.

### 2.2 The correct `kray` (defect 3)

`WardIsotropicGaussianBRDF::ComputeFactors` returns, for the specular
term,

```
    f_S = Rs exp(-tan^2(theta_h)/alpha^2) / ( 4 PI alpha^2 sqrt(nr nv) )
```

with `nr = (n . wi)` (the view) and `nv = (n . wo)` (the queried
direction).  With `p_S(wo) = p_h/(4 (h.wo))` from (1), the exponential
**and the entire alpha dependence cancel**:

```
    f_S cos_o / p_S
        = [ Rs E / (4 PI a^2 sqrt(cos_i cos_o)) ] * cos_o
          * [ 4 PI a^2 cos^3(theta_h) (h.wo) / E ]

        = Rs (h.wo) cos^3(theta_h) sqrt( cos_o / cos_i )             (3)
```

The anisotropic BRDF's `1/(4 PI ax ay sqrt(nr nl))` cancels against (2)'s
`1/(PI ax ay cos^3)` identically, so **(3) is the same expression in both
models** — which is why `WardKrayRatio` is spelled the same way in both
files.

### 2.3 The aggregate `Pdf` (defect 2)

`ScatteredRayContainer::RandomlySelect` weights by `MaxValue(kray)`.
Before (3) that was `MaxValue(Rd)` and `MaxValue(Rs)` — two constants —
so a raw-albedo mixture really was the realized density.  After (3) the
specular weight moves with the sampled direction, and the density of the
selected direction is

```
    Pdf(w) = C_D p_D(w) [w passes both accept gates]
           + sum_i q_i(w) p_i(w)

    C_D    = P(no specular lane accepted)
           + E_{xi} [ wD / ( wD + sum_j w_j R(w_j(xi)) ) ]

    q_i(w) = aD * w_i / (wD + wS)  +  (1 - aD) * [lone-ray term]
```

with `aD` the probability `Scatter`'s diffuse ray survived its own
geometric-horizon gate (Malley's exact clipped-cosine fraction
`(1 + cos phi)/2`, the closed form DL-45 and `SchlickSPF` already use)
and `R` the scalar from (3).

`C_D` is estimated by a deterministic `kWardQuadN x kWardQuadN` = 16x16
stratified replay of the specular sampler's own `(xi1, xi2)` square —
the DL-67 / DL-98 / DL-99 construction, for the DL-67 reason: the
dominant term is usually the specular sampler's **rejection** rate
(`hdotk <= 0`, or a reflected direction below a horizon), which leaves
`RandomlySelect` holding one ray that it returns with probability 1
whatever its weight.  No closed form over reflectances can see that.

**Two things Ward does NOT need, and why:**

* **No second quadrature.**  DL-99's trap — when BOTH lobes have a
  direction-dependent realized weight, the specular lobe's own selection
  coefficient needs an independent quadrature over the diffuse ray's
  draw — does not apply.  Ward's diffuse `kray` is the constant `Rd`, so
  `q_i` depends on the query direction alone, modulated only by the
  scalar `aD`.
* **No reduction-order care.**  DL-98/DL-99 lesson (3) is that
  `MaxValue` does not commute past a product of two independently
  varying colours.  Here the second factor is a nonnegative SCALAR, so
  `MaxValue(Rs * R) == MaxValue(Rs) * R` exactly.

The **per-channel-alpha branch** (`HasPerChannelVariation`, three lanes
sharing one `Point2`) is handled by inverting the query direction back
to that shared random pair and replaying the siblings.  Both inversions
are closed form:

* isotropic — `xi1` fixes the azimuth (shared by every lane, since this
  sampler's `phi` has no alpha dependence) and
  `tan(theta_j) = (alpha_j/alpha_i) tan(theta_i)`, so no `exp`/`log`
  round trip is needed at all;
* anisotropic — `WardAnisoXiFromPhi` is the exact inverse of the warp
  and `-ln(xi2) = tan^2(theta_h) D_i(phi)`.  `WardAnisoPhiFromXi` was
  extracted VERBATIM from the sampler and the sampler now calls it, so
  the forward map and its inverse cannot drift.

---

## 3. Red-proof

`tests/WardDensityKrayTest.cpp`, committed before the fix.  Post-fix
**352 / 0**; against an isolated pre-fix library **138 / 214**.

> Both numbers are of the CURRENT file.  An earlier revision of this
> document quoted "108 / 158 -> 332 / 0", which compared two DIFFERENT
> files: the red run was of the file as first committed, and the fix
> commit then grew it by 80 lines (the NM leak rescale and the §G
> quadrature column), after which the review round added the tilted
> rows and §H.  A red/green pair has to be one file measured against
> two libraries — the DL-116 lesson, applied to this row's own
> counters.

The file measures its own instrument first:

* **§A** — the reference densities (1) and (2), derived here from the
  sampler's inverse CDF and deliberately not copied from the production
  expression, integrate to `1.0000005 .. 1.0000051` on a 2000x800 grid.
* **§B** — they are the density the sampler really draws from: TVD
  between a 400 000-draw histogram of real half-vectors and the
  reference is **0.0029 / 0.0040 / 0.0062** at alpha .1/.3/.6.  The
  reference carries the sampler's OWN accept test (`h.wi > 0`, the
  reflected direction above both horizons), which is not cosmetic: at
  alpha 0.6, 14% of draws are rejected and a reference that integrated
  the raw density reads a TVD of **0.1256** against correct code.

### §C — defect (1), per draw

| model | alpha | theta | mean | min | max | worst `\|ratio - cos^4\|` |
| --- | --- | --- | --- | --- | --- | --- |
| iso | 0.10 | 30 | 0.980677 | 0.831636 | 1.000000 | 0.000000 |
| iso | 0.30 | 30 | 0.858707 | 0.243254 | 0.999997 | 0.000000 |
| iso | 0.60 | 30 | 0.671990 | 0.069759 | 0.999991 | 0.000000 |
| aniso | 0.30 | 30 | 0.912769 | 0.302013 | 0.999996 | 0.000000 |
| aniso | 0.60 | 80 | 0.804081 | 0.050505 | 0.999999 | 0.000000 |

The last column is the diagnosis: the stored value is the true density
times `cos^4(theta_h)` **exactly**, to every printed digit, everywhere.
Post-fix every row reads mean = min = max = 1.000000.

### §D — defect (2), full-sphere mass

`int Pdf` over the whole sphere against the measured probability that
`Scatter` + `RandomlySelect` produce anything (1.0 on every row here).

| row | pre | post |
| --- | --- | --- |
| iso a.3 th30 | 0.927040 | 0.999158 |
| iso a.6 th30 | 0.789760 | 0.999866 |
| iso a.3 th80 | 0.784986 | 1.000042 |
| iso a.6 th30 BLACK diffuse | 0.579510 | 0.995771 |
| iso a.3 th45 BACK FACE | 0.918735 | 0.999424 |
| iso perchannel(.1,.3,.6) th60 | 0.866039 | 0.999585 |
| aniso a.6/.12 th30 | 0.869195 | 0.999499 |
| aniso a.3/.12 th45 BACK FACE | 0.947500 | 0.999526 |
| aniso perchannel(.1,.3,.6)/.12 th30 | 0.946890 | 0.999414 |

Worst `|diff|` over the 16 rows: **0.420490 -> 0.004749**.

### §E — defect (2), shape

TVD against a 600 000-draw real `Scatter` + `RandomlySelect` histogram
(the normalisation gate above is blind to mass moved BETWEEN lobes —
DL-98/DL-99's own lesson).  Worst over 8 rows: **0.050515 -> 0.008660**,
band 0.02.

### §F — defect (3), per draw

`kray_S p_S / (f_S cos)` with the diffuse painter black, so `value()` IS
the specular term.

| row | pre-fix mean | pre-fix [min, max] | post |
| --- | --- | --- | --- |
| iso RGB a.10 th80 | 8.675749 | [1.129543, 631.083439] | 1.000000 [1, 1] |
| iso RGB a.30 th80 | 6.046161 | [0.423448, 576.905155] | 1.000000 [1, 1] |
| iso RGB a.60 th30 | 1.888457 | [0.866026, 538.504670] | 1.000000 [1, 1] |
| aniso RGB a.30 th60 | (see the suite) | | 1.000000 [1, 1] |

Exact `1.000000` on all 64 rows post-fix — RGB, NM (`ScatterNM` /
`valueNM`) and the per-channel branch, both models.  A second,
**BRDF-free** column reads `kray / (Rs * R)` against this file's own
closed form (3), so an error shared between an SPF and its BRDF could
not hide in §F.

> **NM measurement note.**  The diffuse painter is the literal
> `(0,0,0)`, but `GetColorNM` puts it through the Jakob-Hanika uplift,
> which leaks ~2.5e-5 — enough to dominate `valueNM` wherever the
> specular lobe is faint.  A companion BRDF with BOTH painters black
> reads `D*leak + S*leak`; the live one reads `D*leak + S*Rs`, so the
> difference is `S*(Rs - leak)` and the specular term is that times
> `Rs/(Rs - leak)`.  Subtracting the companion outright over-removes
> `S*leak` and biases the ratio by exactly `leak/Rs = 5.0e-5`, which is
> what the row read before the rescale.  (DL-127 §4 solves the same
> problem by subtracting `GuardedGetColorNM(diffuse)*INV_PI` directly;
> the rescale here does not assume the diffuse factor is `INV_PI`.)

### §G — energy

`E[sum_I max(kray_I)]`, what the sampled continuation carries away,
against an independent 400x800 quadrature `Q = int max(value()) cos dw`.
Over 42 configurations (2 models x 3 alphas x 7 incidences), `E/Q` lies
in **[0.9925, 1.0066]**.  That is the point of the whole row: the
sampled estimator now integrates the BRDF that NEE and every BDPT/VCM
connection evaluate.

---

## 4. The tail — decision and data

DL-177 was deliberately held open because "the corrected weight carries
Ward's unbounded `1/sqrt(nl nv)` tail, which needs an answer of its own
before it goes in front of a path tracer".  The answer is **(a) leave
it**, and the reasoning is that the premise is half wrong.

**It is bounded.**  In (3), `(h.wo) <= 1`, `cos^3(theta_h) <= 1` and
`cos_o <= 1`, so

```
    kray_S  <=  Rs / sqrt(cos_i)
```

— a per-shading-point CONSTANT, not a per-draw divergence.  The `nl`
half of the BRDF's `1/sqrt(nl nv)` cancels against the cosine and the
sampler's own density; only `1/sqrt(nv)` survives, and `nv` is fixed by
the query.  Measured per-draw maxima at `Rs = 0.5` (§G, 200 000 draws
per cell, worst over both models and all three alphas):

| incidence | measured max `kray` | p99.9 | bound `Rs/sqrt(cos_i)` |
| --- | --- | --- | --- |
| 80.0 deg | 0.486 | 0.486 | 1.200 |
| 85.0 deg | 0.602 | 0.601 | 1.694 |
| 89.0 deg | 1.198 | 1.197 | 3.785 |
| 89.9 deg | 3.687 | 3.685 | 11.968 |

The ledger row's "per-draw maxima reach 685x and 1085x at 80 degrees"
was measured THROUGH the broken pdf; through the fixed one the same
probe reads `1.000000` exactly.

**It does not drive variance.**  The furnace estimator's relative
standard error over 200 000 draws is `<= 0.0040` at every one of the 42
configurations, 89.9 degrees included, and `p99.9` sits at or just below
the max rather than far above it — the distribution has no heavy tail to
speak of.

**And it is not heavy BY THE RENDERER'S OWN STANDARDS**, which is the
question an absolute bound does not answer.  Section H runs the same
per-draw statistic at the same 89.9 degrees on two controls: `GGXSPF`
(single-emit, energy-compensated — the tightest weight in the tree) and
the post-DL-127 `SchlickSPF`, whose model omits Schlick's geometric term
(DL-178) and therefore carries a genuinely heavy tail that ALREADY
SHIPS.  200 000 draws, `Rs = 0.5`:

| SPF | mean | p99.9 | max | max/mean |
| --- | --- | --- | --- | --- |
| `WardIsotropicGaussianSPF` α 0.3 | 0.5187 | 3.6183 | 3.6865 | **7.11** |
| `WardAnisotropicEllipticalGaussianSPF` α 0.3/0.12 | 0.5569 | 3.6529 | 3.6866 | **6.62** |
| `GGXSPF` conductor α 0.3 (control) | 0.2848 | 0.4510 | 0.4522 | 1.59 |
| `SchlickSPF` r 0.3 iso 1 (control, DL-178) | 84.87 | 2395.0 | 26522.4 | **312.5** |

So the corrected Ward weight sits an order of magnitude tighter than a
tail the renderer already lives with, and within a factor of ~4.5 of the
tightest one. That is the comparison decision (a) rests on; the
absolute bound above only establishes that the question is bounded at
all.

**Why not (b) or (c).**  The Ward-Dür / Geisler-Moroder-Dür bounded
variant replaces `1/sqrt(nl nv)` with a half-vector expression.  Adopting
it for the `kray` alone re-creates DL-127's defect verbatim — a `kray`
that is not `f cos/p` of the evaluated BRDF — so it would have to change
`Ward*BRDF::value` in lockstep, i.e. it is a **model substitution that
changes the shipped look of `ward_*_material`**, not a bug fix, and the
measurements above do not call for one.  A clamp (c) would lose energy
for no measured benefit and would have to be quantified against a
distribution whose maximum is already 3.2x below its own analytic bound.

**Residual, opened as DL-212 and not fixed.**  The MODEL's own grazing
energy gain now reaches indirect transport — the Ward analogue of
DL-178.  The specular directional albedo at `Rs = 0.5` (i.e. `int f_S cos
dw`, §G's `Q` column):

| model | alpha | 0 deg | 60 deg | 89.0 deg | 89.9 deg |
| --- | --- | --- | --- | --- | --- |
| iso | 0.10 | 0.4855 | 0.2445 | 0.0548 | 0.1339 |
| iso | 0.60 | 0.2474 | 0.1586 | 0.2626 | 0.7877 |
| aniso 0.6/0.12 | — | 0.3442 | 0.2131 | 0.3326 | 0.9957 |

i.e. up to **1.99x `Rs`** at 89.9 degrees for the rough anisotropic
case, rising monotonically past 60 degrees.  NEE has always evaluated
that; the sampled continuation now agrees with it.

---

## 5. Tolerances re-derived

Three suites carried relaxations attributed to "the Ward model is not
energy-conserving".  An isolated pre-fix rebuild (`git checkout
a9b1d3d8 --` the two Ward SPF `.cpp` only, library rebuilt, same test
binaries) shows every one of them was the implementation.

### 5.1 `SPFBSDFConsistencyTest` — 0.25 -> 0.01

| row | MC pre | MC post | Quad (unchanged) | relErr pre -> post |
| --- | --- | --- | --- | --- |
| WardIso @ 30 | 0.799994 | 0.733174 | 0.733211 | 8.3479% -> 0.0050% |
| WardIso @ 60 | 0.790517 | 0.637516 | 0.637679 | 19.3339% -> 0.0255% |
| WardAniso @ 30 | 0.799973 | 0.719404 | 0.719604 | 10.0464% -> 0.0278% |
| WardAniso @ 60 | 0.789566 | 0.629526 | 0.629638 | 20.2552% -> 0.0177% |

The old note claimed the MC arm "saturates near the albedo ... while
quadrature under-integrates the sharp specular peak on a finite grid".
Both halves are false, and the isolated A/B says so rather than an
argument: the quadrature column is UNCHANGED by the fix and the MC arm
moved onto it; and "MC pre" pinned at 0.79-0.80 on every row is the
signature of `kray` literally BEING the constant `Rd + Rs = 0.8`
reflectance rather than a transport weight.

Tolerance 0.01 on both, matching the Schlick entry's precedent.

**A 25x tightening deserves more than one seed** (review round 1,
P3-c), and until that review this file had only one — every
`RandomNumberGenerator` in it was default-constructed, i.e. seeded from
an un-`srand`ed libc `rand()`, which C defines as `srand(1)`: exactly
the fragility DL-176 was filed for next door.  It now takes an explicit
`g_seedBase`, overridable by `RISE_SPFBSDF_SEED`.  Swept over 8 seeds:
**worst Ward relErr 0.0757 %** (isotropic @ 60 deg) and worst over EVERY
row in the file 0.1288 % (Schlick @ 60 deg), with 0 failures at every
seed.  So the 1 % band carries ~13x headroom over the worst Ward
reading actually observed, not the ~36x the single default seed
suggests — deliberately not tightened further, because the MC arm now
carries the model's real grazing tail (DL-212) and these are Monte
Carlo estimators.

### 5.2 `SPFPdfConsistencyTest` — chi2 un-skipped, 10% band retired

|  | `int Pdf` pre | post | chi2 seed-mean z pre | post |
| --- | --- | --- | --- | --- |
| WardIso @ 30 | 0.973145 | 0.999611 | +25.36 | +0.837 |
| WardIso @ 60 | 0.963752 | 0.999912 | +35.87 | +0.302 |
| WardAniso @ 30 | 0.959258 | 0.999500 | +59.01 | −0.084 |
| WardAniso @ 60 | 0.949217 | 0.999978 | +107.81 | +0.059 |

The anisotropic 60-degree integral row FAILED even the 5% band pre-fix,
so the "relaxed to 10%" note was covering this and not anisotropy.  Both
rows return to `INTEGRAL_TOL` and both chi2 gates are live.

**On the ledger row's `cos^4 ~ 0.94` claim** — it is right, and this is
what it buys: both rows run at alpha 0.2/0.3, where a typical sampled
`cos^4(theta_h)` is ~0.94, so the mass stayed inside a 5%/10% band while
the *shape* was off by 25 to 108 standard deviations.  The histogram was
the only sub-test that could see the defect, and it was the one switched
off.

### 5.4 `SPFPdfConsistencyTest` Part 2b — the gate the rescale removed

DL-176's Part 3 rescale (§8) makes the chi-squared a pure SHAPE test.
Review round 1 (P2-1) pointed out that this removed the harness's only
implicit statement about MASS, and that Part 2 does not replace it:
Part 2 asks `|int Pdf over the HEMISPHERE - 1| <= 5 %`, which is the
wrong target on two counts at once — `Pdf` is legitimately a SUB-density
(it must integrate to the probability that `Scatter` +
`RandomlySelect` emit anything), and the domain is the SPHERE wherever
the sampler can emit below the shading horizon.

Part 2b adds the two checks that name the contract, because the
mismatch splits into two independent failure modes:

* **(i) MASS** — `int Pdf` over the whole sphere against the measured
  `P(emit a non-delta ray)`.  Every row passes at 0.01; the four
  largest residuals are `Schlick` 0.00221,
  `AshikminShirleyAnisotropicPhong` 0.00206 / 0.00204 and
  `CookTorrance_BlackSpecular` 0.00201 (the last being DL-211).  Those
  are the three SPFs whose `Pdf` is built on a deterministic
  selection quadrature, so the residual is that quadrature's, not MC
  noise — the DL-98/DL-99 lesson (4).
* **(ii) DOMAIN SPLIT** — the fraction of `Pdf`'s mass in the upper
  hemisphere against the fraction of emitted rays that land there.
  This is what (i) is blind to by construction, and it is the check the
  review's own example needs: **`Translucent` reads full-sphere mass
  agreeing to 4.1e-5 while putting ~100 % of its density in the upper
  hemisphere and only 55.5 % of its rays** — a 1.80x violation (DL-41:
  `TranslucentSPF::Pdf` covers neither Phong `cos^N` transmission
  lobe).  Every other row reads a domain-split difference of exactly 0.

  (The review read that row as a MASS mismatch — "emission rate
  0.556282 vs int Pdf 1.00004" — by comparing a hemisphere-restricted
  acceptance count against a hemisphere integral.  The defect is real;
  it is a domain split, and (ii) is what sees it.)

`Translucent` is pinned as a two-sided KNOWN-DEFECT control at its
measured `0.44469`, band `[0.40, 0.48]`, rather than having the band
loosened — so DL-41's closure is as visible as a regression.

**DL-211 is pinned the same way** (P2-4).  Its chi2 seed-mean `z` sits
at `+3.58` against the `|z| <= 4` band — 90 % of the band, the same
fragility shape DL-176 was filed for — so `CookTorrance_BlackSpecular`
now carries its own two-sided pin, `[3.0, 4.0]` at 60 deg and
`[1.5, 2.7]` at 30 deg (measured `+3.585` / `+2.096`), naming DL-211 in
the message.

### 5.3 `SchlickKrayBRDFConsistencyTest` §6 — promoted to a gate

Section 6 was reporting-only, gated merely on "the probe ran".  Every
MULTI-EMIT row (the Schlick control, both Wards, both Phongs) is now
gated on the per-lobe identity holding to 1e-9 **per draw**.  The two
single-emit rows (`CookTorranceSPF`, `GGXSPF`) stay reporting-only:
their `kray` is an internal-selection estimator whose per-draw spread of
`[0.006, 37]` is correct.

`537/0 -> 557/0`; **`549/8` against the pre-fix library**, the 8 being
exactly the Ward rows' new gates.

---

## 6. Renders

**Reach.**  Ten shipped scenes bind `ward_isotropic_material` or
`ward_anisotropic_material` (`grep -rl ward_..._material scenes/`).
Four of them name `pixelpel_rasterizer` at the top level, but **the
rasterizer is not what decides whether a Ward surface is shaded
directly — the per-object SHADER is**, and §6.1 below is where the
round-1 review caught this document getting it wrong.

**Protocol.**  Scratch copies at 256 px wide (aspect preserved), shipped
sample counts capped at 64, `oidn_denoise FALSE`, `pixel_filter box`,
EXR `color_space Rec709RGB_Linear`, each scene under ITS OWN shipped
rasterizer.  n = 3 renders per build (n = 15 for `showroom`, see below);
renders are not deterministic run to run (`BlockRasterizeSequence`
shuffles from `std::random_device`).  The "pre" build is an isolated
rebuild with ONLY the two Ward SPF `.cpp` at `a9b1d3d8`; the "post"
build is this branch's shipped state, i.e. INCLUDING §7's optimisation.
Values are the mean of the three RGB channels over the whole image;
`sigma` is the sample standard deviation over the runs and `t` uses the
standard errors.  The `ward region` rows are the 45x45-pixel box
(152,152)-(196,196), the projected extent of `sphere_ward_aniso`
(centre `416 135 280`, radius 65, pinhole at `278 273 -800`, fov 39).

| scene | rasterizer | pre | post | delta | t |
| --- | --- | --- | --- | --- | --- |
| `cornellbox_bdpt_materials_pt` | PT | 0.967649 ± 0.000155 | 0.950310 ± 0.000778 | **−1.792 %** | −37.9 |
| — ward region | | 0.518909 ± 0.000744 | 0.499280 ± 0.000654 | **−3.783 %** | −34.3 |
| `cornellbox_bdpt_materials` | BDPT | 0.956063 ± 0.000449 | 0.940088 ± 0.000496 | **−1.671 %** | −41.4 |
| — ward region | | 0.514654 ± 0.001655 | 0.493582 ± 0.001945 | **−4.095 %** | −14.3 |
| `kaleidoscope_atrium` | pixelpel | 0.429410 ± 0.000056 | 0.425963 ± 0.000032 | **−0.803 %** | −92.6 |
| `materials` | pixelpel | 0.169327 ± 0.000447 | 0.168835 ± 0.000292 | −0.291 % | −1.60 (NS) |
| `pt_alchemists_sanctum` | PT | 4.543042 ± 0.054928 | 4.542103 ± 0.026395 | −0.021 % | −0.03 (NS) |
| `pt_jewel_vault` | PT | 10.666218 ± 0.008208 | 10.667713 ± 0.012162 | +0.014 % | 0.18 (NS) |
| `bdpt_alchemists_sanctum` | BDPT | 4.533381 ± 0.001201 | 4.540183 ± 0.006298 | +0.150 % | 1.84 (NS) |
| `bdpt_jewel_vault` | BDPT | 10.657990 ± 0.015965 | 10.656748 ± 0.003755 | −0.012 % | −0.13 (NS) |
| `sss_different_bsdf` | pixelpel | 0.089886 ± 0.000010 | 0.089878 ± 0.000048 | −0.009 % | −0.27 (NS) |
| `showroom` | pixelpel | 0.612517 ± 0.094265 | 0.643686 ± 0.124605 | +5.089 % | 0.77 (NS) |

**Read this honestly.**

### 6.1 Which mechanism moved which row

Two mechanisms can move a render here, and they reach different scenes:

* **the `kray` correction (defect 3)** — reaches any BSDF-SAMPLED
  continuation, i.e. anything routed through `pathtracing_shaderop`,
  BDPT, VCM or MLT;
* **the `Pdf` correction (defect 2)** — reaches NEE's MIS weight,
  because `DirectLightingShaderOp` -> `LightSampler::EvaluateDirectLighting`
  computes `p_bsdf = pMaterial->Pdf( vToLight, ri, misIOR )`
  (`LightSampler.cpp`, the mesh-luminary arm).

An earlier revision of this section attributed `kaleidoscope_atrium`'s
−0.803 % to the second mechanism and called it proof that a
`pixelpel_rasterizer` scene is not immune.  **That attribution was
wrong.**  The scene has ONE Ward chunk (`pillar_mat`, a
`ward_anisotropic_material`), all six objects that bind it declare
`shader glossy_shader`, and `glossy_shader` is

```
standard_shader { name glossy_shader   shaderop pt }
```

— `pathtracing_shaderop` alone, deliberately WITHOUT
`DefaultDirectLighting` (the scene's own comment says why: stacking
them double-counts).  So `DirectLightingShaderOp` never runs on a Ward
surface in that scene at all, and its −0.803 % is the **kray**
correction, the same mechanism as the PT cornellbox row.

### 6.2 The NEE-through-`Pdf` mechanism, measured

The mechanism is real by code reading, but it had to be measured on a
configuration that actually exercises it, and the shipped set turns out
to contain **one** genuinely direct-only Ward scene:
`scenes/Tests/Materials/materials.RISEscene`, whose single
`standard_shader global` carries `shaderop DefaultDirectLighting` and
nothing else.  Re-rendered at 256 spp (its shipped 4 spp is far too
noisy to resolve anything), n = 5 per build, with per-teapot regions —
`wardIso` is `teapot4`/`wi` at rows 40-84, cols 160-224; `wardAniso` is
`teapot6`/`wa` at rows 184-228, cols 160-224:

| region | pre | post | delta | t |
| --- | --- | --- | --- | --- |
| ward isotropic teapot | 0.667067 ± 0.000005 | 0.667068 ± 0.000005 | +0.0002 % | +0.3 |
| ward anisotropic teapot | 0.655986 ± 0.000008 | 0.655983 ± 0.000008 | −0.0004 % | −0.5 |
| whole image | 0.168977 ± 0.000025 | 0.168969 ± 0.000032 | −0.0046 % | −0.4 |

**A measured null, and it has a reason rather than being a failure to
resolve** (σ is 5e-6 on those regions; a 0.1 % effect would read t ≈ 130).
That scene's only light is a `directional_light` — a DELTA light, which
has no MIS partner, so `EvaluateDirectLighting`'s delta arm takes
weight 1 and never evaluates `pMaterial->Pdf()` at all.

So the mechanism needs a NON-DELTA light, and the sharpest test is the
same scene with only the light swapped.  A purpose-built copy
(`materials.RISEscene` at 512 spp / `lum_samples 16`, identical shader
chain, identical materials, the `directional_light` replaced by a
`clippedplane_geometry` + `lambertian_luminaire_material` area emitter
off to the +x side at z = 12 so it front-lights without occluding),
n = 5 per build:

| region | pre | post | delta | t |
| --- | --- | --- | --- | --- |
| ward isotropic teapot | 1.535493 ± 0.000336 | 1.466030 ± 0.000151 | **−4.524 %** | −422 |
| ward anisotropic teapot | 1.869537 ± 0.000118 | 1.766052 ± 0.000206 | **−5.535 %** | −975 |
| whole image | 0.338291 ± 0.000049 | 0.329367 ± 0.000035 | −2.638 % | −334 |
| Lambertian teapot (control) | 0.405674 ± 0.000027 | 0.405677 ± 0.000015 | +0.0009 % | +0.3 |
| Schlick teapot (control) | 1.547793 ± 0.000548 | 1.547156 ± 0.000355 | −0.041 % | −2.2 |

That is the claim, demonstrated rather than asserted: **a direct-only
Ward surface is immune under a delta light and moves by 4.5-5.5 % under
an area light**, with everything else in the scene held fixed and two
non-Ward controls that do not move (the Schlick row's −0.041 % at n = 5
is a 2σ fluctuation, two orders of magnitude below the Ward rows, and
no Schlick code changed in this slice).

The scene is a scratch fixture, not committed — it is `materials.RISEscene`
with the light chunk replaced, and §6.2's description is enough to
rebuild it.

### 6.3 The shipped-scene table, re-read

* The two `cornellbox_bdpt_materials*` scenes are the high-signal rows —
  one sphere of nine is `ward_anisotropic_material`, and they are the
  only scenes here whose Ward surface fills a resolvable, identifiable
  region.  Both move down ~1.7 % whole-image and ~3.8-4.1 % on the
  sphere, at t = 14 to 41.
* **The direction is DOWN, and that is what the model says it should
  be.**  Pre-fix the specular lobe carried `kray_S = Rs` — a constant.
  The true `int f_S cos dw` is BELOW `Rs` everywhere except at extreme
  grazing — §G measures it at `Rs = 0.5` as 0.4855 (0 deg) / 0.4208
  (30 deg) / 0.2445 (60 deg) for isotropic alpha 0.1 and 0.3969 /
  0.3455 / 0.2107 for alpha 0.3 — so a Ward surface seen at ordinary
  angles was over-delivering, and every statistically resolvable row
  here darkens.  §5.1's furnace rows say the same thing from the other
  side: the MC arm was pinned at 0.79-0.80 (the constant `Rd + Rs`)
  against a quadrature of 0.63-0.73.
* `kaleidoscope_atrium` moves −0.803 % at t = −92.6 **through the kray
  correction**: its Ward pillars are shaded by `pathtracing_shaderop`
  (see §6.1), not by `DirectLightingShaderOp`, despite the scene naming
  `pixelpel_rasterizer` at the top level.
* `materials.RISEscene` is the one genuinely direct-only Ward scene in
  the shipped set, and at its own 4 spp it reads −0.291 % at t = −1.60,
  i.e. nothing; §6.2 re-measures it at 256 spp and shows the null is
  real and structural (delta light) rather than a resolution failure.
* `showroom` is **not resolvable** and no claim is made from it: at its
  shipped 8 spp it carries a firefly tail that puts sigma at 15-20 % of
  the mean, so even n = 15 per build leaves t = 0.77.  The +5.089 % is
  noise, not a measurement.
* The remaining scenes put Ward on small or grazing-lit objects and do
  not move outside their own noise.

---

## 7. Cost

`Pdf()`/`PdfNM()` now run a `kWardQuadN x kWardQuadN` = 16x16 replay of
the specular sampler, so they cost more than the raw-albedo mixture
they replace.  Measured under the DL-98/DL-99 **alternating-binary
protocol** — two separately built binaries run PRE/POST/PRE/POST in one
session, with an untouched in-process `LambertianSPF::Pdf` workload as
the machine-drift reference — over 512 fixed query directions, 6
alternations, median of the 5 settled ones:

| ns per `Pdf()` call | pre | first landing | shipped |
| --- | --- | --- | --- |
| ref (`LambertianSPF`) | 2.4 | 2.4 | 2.4 |
| Ward isotropic | 12.0 | 1060 | **639** |
| Ward anisotropic | 13.6 | 8130 | **1140** |
| Ward isotropic, per-channel alpha | 12.1 | 2950 | **1780** |
| Ward anisotropic, per-channel alpha | 13.5 | 15570 | **3320** |

The reference column is flat, so the movement is the function and not
the machine.

The first landing was not acceptable: **8130 ns for a SINGLE-lane
anisotropic query is worse than
`AshikminShirleyAnisotropicPhongSPF`'s per-channel 7650**, which is the
most expensive `Pdf` in the tree.  Two causes, both removed without
touching the model:

1. **The azimuth was recomputed at every node.**  Ward's warp depends on
   `xi1` and the lane's alpha ratio only — never on `xi2` — so
   `WardAnisoReplayLane` was paying an `atan`, a `tan`, a `cos` and a
   `sin` 256 times for 16 distinct values.  Hoisting them into a
   per-(lane, azimuth) row, exactly as the isotropic twin already did,
   is most of the 8130 -> 1140.
2. **Both quadratures normalised the reflected direction per node.**
   They do not need to.  Normalising the INCOMING direction ONCE makes
   every reflection unit by construction, and then
   `(h.wo) == (h.wi)` and `(n.wo) == (n.d) + 2(h.wi)(n.h)` are EXACT;
   the two accept gates need only signs. 256 sqrt-and-divides removed
   from each function.

The shipped numbers sit inside the band this construction already
carries: `SchlickSPF::Pdf` ~690 ns (DL-67 Slice 0 + DL-127),
`IsotropicPhongSPF` ~490 ns (DL-98),
`AshikminShirleyAnisotropicPhongSPF` ~2580 ns single / ~7650 ns
per-channel (DL-99).

**Where it is paid.**  PT evaluates the aggregate `Pdf` once per
non-delta vertex (DL-103); BDPT/VCM/MLT evaluate it twice (DL-69's
`pdfFwd` addition).  So a Ward-dominated BDPT render pays ~2.3 us per
vertex where it used to pay ~27 ns.  The grid stays at 16 for the reason
DL-67 §4f already recorded — 32 costs ~3.9x for a residual that is
already well inside these suites' bands (§3's worst full-sphere mass
residual is 0.0047).

The optimisation is **value-preserving**, checked rather than asserted:
§D's 16 full-sphere `int Pdf` readings are identical to six decimal
places before and after it, and the two high-signal renders agree within
MC noise (`cornellbox_bdpt_materials_pt` whole-image −0.011 %, BDPT
−0.024 %).

---

## 8. DL-176 — the chi-squared harness

`SPFPdfConsistencyTest`'s `CookTorrance_BlackDiffuse @ 60deg` row read
927.52 against a critical 928.261 (dof 799, +3.22 sd) — 99.92% of
critical, deterministic only because `RandomNumberGenerator rng;` seeds
from an un-`srand`ed libc `rand()` (which, per the C standard, behaves
as if `srand(1)` had been called: the whole suite was silently running
ONE seed).

**The 10-seed sweep on the unfixed harness** (`std::srand` per run;
dof 799, `sd(chi2) = 39.97`, `sd(mean of 10) = 12.64`):

| row | mean | sd | min | max | z of the mean |
| --- | --- | --- | --- | --- | --- |
| CookTorrance_BlackDiffuse @ 60deg | 938.26 | 24.25 | 896.19 | 976.41 | **+11.02** |
| CookTorrance_BlackSpecular @ 60deg | 859.94 | 34.67 | 790.85 | 921.80 | +4.82 |
| CookTorrance_BlackDiffuse @ 30deg | 879.34 | 38.11 | 817.42 | 945.48 | +6.36 |
| CookTorrance @ 60deg | 826.12 | 50.17 | 748.78 | 915.06 | +2.15 |
| Lambertian @ 30deg | 794.51 | 42.98 | 726.13 | 848.08 | −0.36 |

So the filed row was never one unlucky draw: its true mean sits ABOVE
the critical value.

**Root cause — in the harness.**  Part 3 built its expected counts as
`(4x4 sub-integral of Pdf over bin i) * totalAccepted` and never
normalised them to the observed total.  That null model is only valid if
`sum_i expected[i] == totalAccepted`, i.e. if `Pdf` integrates to
EXACTLY 1 over the binned hemisphere.  It legitimately does not:
`CookTorranceSPF::Pdf` is a correct SUB-density — the SPF genuinely
emits nothing on 1.5% of calls at 60 degrees, when the internally
selected lobe's `kray` is the authored literal black and `Scatter` only
adds a ray for `kray > 0`.  A deviation `eps = totalAccepted/sum(expected)
- 1` then appears in every group at once and injects
`totalAccepted * eps^2/(1+eps)` of pure normalisation artifact — about
100 chi2 on that row.

**Fix, three parts:**

1. Rescale so `sum expected == totalAccepted`, making Part 3 a pure
   SHAPE test (Pearson's standard construction).  The one degree of
   freedom this costs was ALREADY being subtracted — `dof--; // Lose 1
   DOF because total count is fixed` — the old code paid for a
   constraint it never imposed.  Total MASS stays gated by Part 2, which
   is the right split (mass -> Part 2, shape -> Part 3).
2. Fold the trailing partial merge group into the last completed group
   instead of discarding it (it held the grazing horizon bins).
3. Give every RNG an EXPLICIT seed and evaluate the statistic at
   `NUM_CHI2_SEEDS = 8` of them.  The gate is on the seed-MEAN:
   `z = (mean - dof)/sqrt(2 dof/N)` is ~N(0,1) under H0, band `|z| <= 4`
   — a 6.334e-5 two-sided test per row, against a single-seed
   alpha=0.001 threshold's 0.1 % PER ROW PER RUN, and one that is not
   nearly blind to a small persistent offset.  Family-wise, COUNTED
   rather than estimated (review round 1, P3-a): the run prints **30
   gated chi2 evaluations carrying 23 DISTINCT statistics** — several
   rows are bit-identical, since their samplers are all cosine
   hemispheres consuming the same draws from the same seeds — giving
   **0.146 %** over the 23 independent statistics and **0.190 %** over
   all 30, against **2.96 %** for a single-seed threshold over the same
   30.

**Post-fix** every gated row is within `|z| <= 4`; the filed row reads
`z = +0.54` with **nothing in `CookTorranceSPF` changed**.  Runtime
3.7s -> 18.1s (the per-bin `Pdf` quadrature is computed once and shared
across seeds).

Each row now also prints its measured emission rate against `int Pdf` —
the DL-98/DL-99 sub-density contract, made visible.  That column
identifies the one genuine residual, **opened as DL-211**:
`CookTorrance_BlackSpecular` reads `int Pdf = 1.00001` against an
emission rate of `0.998014`, i.e. `Pdf` prices ~0.2% of mass on a
specular lobe `Scatter` can never emit (`kSelFloor` keeps the lobe
selectable while its literal-black `kray` keeps it from producing a
ray).  `z = +3.58` at 60 degrees / `+2.10` at 30, inside the `|z| <= 4`
band.  With fixed seeds that value is DETERMINISTIC, so the remaining
0.4 sd of margin is not a coin flip the way the pre-DL-176 927.52 was:
it moves only if `CookTorranceSPF` or the harness moves.

---

## 9. New debts opened

**DL-211** — `CookTorranceSPF::Pdf` prices the `kSelFloor`-floored
specular lobe at an authored literally-black specular painter, which
`Scatter` can never emit.  §8 above.

**DL-212** — the Ward analogue of DL-178: `Ward*BRDF`'s own specular
term is not energy-conserving at grazing, and since DL-177 the sampled
continuation delivers that gain to indirect transport as well as NEE.
§4 above.
