# DL-127 — `SchlickSPF`'s `kray` vs `SchlickBRDF`'s own integral

> **Model update, 2026-09-19 (DL-178 / DL-212):** The formulas and measurements below describe the historical model. [Current model derivation and validation](DL178_DL212_BOUNDED_SCHLICK_WARD.md) supersede the Schlick weight with its Eq.31 geometric product and both Ward weights with `2 Rs cos_o/(cos_i+cos_o)`. The sampler and per-lobe densities remain unchanged; aggregate selection weights follow the new krays. Schlick retains a finite energy-conservation residual tracked as DL-225.

**Status: CLOSED 2026-09-17** (slice `debt-dl127`).
Red-proof and regression gate: `tests/SchlickKrayBRDFConsistencyTest.cpp`.

---

## 1. The defect

`schlick_material` is one object with three faces:

| face | what it is | who reads it |
| --- | --- | --- |
| `SchlickSPF::Scatter`/`ScatterNM` | emits lobes, each with a `kray` (transport weight) and a `pdf` | every integrator's BSDF-sampled continuation |
| `SchlickSPF::Pdf`/`PdfNM` | the aggregate density of the direction `Scatter` + `RandomlySelect` produce (DL-67 Slice 0) | MIS partners, BDPT/VCM `pdfFwd`/`pdfRev` |
| `SchlickBRDF::value`/`valueNM` | the BRDF | NEE, every BDPT/VCM connection strategy |

They are one triple, and the contract that ties them is

```
    kray_I(w) * p_I(w)  ==  f_I(w) * cos(w)                       (*)
```

for each lobe `I`.  With (*) the sampled continuation `kray_I / q_I`
integrates the BRDF (`sum_I int p_I kray_I = int f_agg cos`), and the
BSDF-sampled and NEE arms of an MIS pair price the same surface.

Pre-fix the specular lobe violated (*).  `Scatter` stamped Schlick's own
*sampling weight*

```
    kray_S = S(w) = rho + (1-rho) * (1 - (h.v))^5
```

while `SchlickBRDF::value` returns `S(w) * Z(t) * A(phi) / (4 pi nl nv)`
and the lobe's `pdf` is `ComputeSchlickSpecularPdf`.  The diffuse lobe
was and remains exact: `kray_D = Rd`, `p_D = cos/pi`, `f_D = Rd/pi`, so
`f_D cos/p_D = Rd` identically.

---

## 2. Derivation of the exact discrepancy

Write `t = n.h`, `den = sin^2(theta_h) + r cos^2(theta_h)`,
`Z = r/den^2`, `A(phi) = sqrt(p/(p^2 - p^2 w^2 + w^2))` with
`w = dot(onb.v(), normalize(h - t n))`, and `p_phi(phi)` for the
azimuthal density `GenerateSpecularRay` actually draws from.

**BRDF term, times the cosine:**

```
    f_S cos = S * Z * A / (4 pi nl nv) * nl = S * Z * A / (4 pi nv)
```

**Sampling density.**  The sampler draws `cos^2(theta_h) = xi/(r - xi r + xi)`,
so `p(cos^2 theta_h) = r/den^2 = Z` and the half-vector density in solid
angle is `p_h = 2 t Z p_phi`; the reflection Jacobian gives

```
    p_S(w) = p_h / (4 (h.v)) = t * Z * p_phi / (2 (h.v))
```

**Ratio.**  `Z` — and with it the entire roughness dependence — cancels:

```
    f_S cos / p_S = S * A(phi) * (h.v) / ( 2 pi * nv * t * p_phi(phi) )   (**)
```

So the missing factor is

```
    R(w) = A(phi) * (h.v) / ( 2 pi * (n.v) * (n.h) * p_phi(phi) )
```

and `R == 1` exactly in two places, which is why the defect hid for so
long:

* `h == n` (the mirror direction at any incidence, isotropic or not
  — there `t = 1`, `(h.v) = nv`, `A/(2 pi p_phi) = 1` at `p = 1`);
* **normal incidence with `p = 1`** — there `(h.v) = t` for every `h`,
  so `R = A/(2 pi p_phi) = 1` for the whole lobe.  That is exactly the
  configuration `SchlickLobePairingTest`'s `theta=0deg` row measures,
  and it is why DL-69's table read `1.00001` there and `0.80224` at
  60 degrees.

> **Correction to the DL-127 ledger row's own formula.**  The row states
> the missing factor as `hl / (nv * t * A)`.  That expression is derived
> against the *pre-DL-67* azimuthal density `p_phi = A^2/(2 pi)`, which
> `ComputeSchlickSpecularPdf` no longer uses — DL-67 section 4b replaced
> it with the density the sampler really draws,
> `p_phi = p^2 / (2 pi (p^2 + t_q^2 (1-p^2))^{3/2})`.  Substituting the
> old form into (**) does reproduce `hl/(nv t A)`, so the row was correct
> when written and is stale now; `(**)` is the current expression.  At
> `p = 1` (where the two densities agree) both read `hl/(nv t)`.

---

## 3. The ruling: which side is the truth?

**`SchlickBRDF::value` is the truth side.**  Two independent reasons.

### 3.1 The `kray` convention does not define a BRDF at all

If the sampling weight were the intended energy, the BRDF it implies is
`f_impl = kray p_S / cos`, i.e.

```
    f_impl(v -> l) = S * t * Z * p_phi / ( 2 * nl * (h.v) )
```

Swap the two directions.  `h = normalize(l+v)` is symmetric, so `t`, `Z`,
`p_phi` and `(h.v) = (h.l)` are unchanged, and so is `S` (it is a
function of `(h.v)`).  The only thing that moves is the cosine in the
denominator:

```
    f_impl(l -> v) / f_impl(v -> l) = nl / nv
```

**`f_impl` is not reciprocal**, and RISE runs `schlick_material` through
BDPT/VCM/MLT light subpaths, where a non-reciprocal transport weight is
not merely inelegant — it makes the eye-side and light-side estimators
disagree by construction.  `SchlickBRDF::value` *is* reciprocal (`S`,
`Z`, `A` all depend on `h` only, and `1/(nl nv)` is symmetric).

Measured, `tests/SchlickKrayBRDFConsistencyTest.cpp` section 5
(the closed form is first pinned against the live SPF's own emitted
`pdf`, max relative `1.4e-14` over 7047 real draws, so this is the
shipped quantity and not a re-derivation of it):

| `theta_v` | `theta_l` | `value()` asymmetry | `f_impl(l->v)/f_impl(v->l)` | predicted `nl/nv` |
| --- | --- | --- | --- | --- |
| 20 | 50 | 3.0e-16 | 0.684040 | 0.684040 |
| 30 | 70 | 1.6e-16 | 0.394931 | 0.394931 |
| 10 | 80 | 0.0e+00 | 0.176327 | 0.176327 |
| 45 | 60 | 1.6e-16 | 0.707107 | 0.707107 |
| 5  | 75 | 0.0e+00 | 0.259808 | 0.259808 |

### 3.2 `value()` is the published model; the `kray` weight is a shortcut

`S * Z * A / (4 pi nl nv)` is Schlick 1994's directional factor written
out.  Setting `kray = S` amounts to asserting
`G' = (n.h)(n.v)/(h.v) == 1` — which is a *view-dependent* quantity, not
a constant, and not symmetric in `l` and `v`; it is the same shape as
half of Cook-Torrance's masking term, with the wrong direction in it.

**Not claimed here:** that `value()` is energy-conserving.  It is not
(RISE's implementation omits Schlick's geometric self-shadowing factor
`G(v) = v/(r - r v + v)` entirely), and the fix therefore makes the
model's own grazing energy gain reach the sampled continuation as well
as NEE.  That is **DL-178**, opened by this slice and deliberately not
fixed here — see section 7.

---

## 4. What changed

`src/Library/Materials/SchlickSPF.cpp` only.  `SchlickBRDF` is untouched
apart from a comment correction on `albedo()`.

**New shared helpers** (all `static inline`, same translation unit):

* `SchlickAzimuthalDensityFromPhi(phi, p)` — extracted verbatim from
  `ComputeSchlickSpecularPdf`, which now calls it, so the ratio and the
  density it is a ratio *against* cannot drift.
* `SchlickAzimuthFromH(h, onb)` — the half-vector azimuth in the
  sampling frame.
* `SchlickAzimuthFactorA(h, t, onb, p)` — spelled exactly as
  `SchlickBRDF`'s `ComputeFactor` spells `A`.
* `SchlickKrayRatioFromH(h, wi, onb, nv, p)` and the queried-direction
  wrapper `SchlickKrayRatio(ri, onb, woNorm, p)` — `R(w)` from (**).

**Sampler sites** (`kray = S * R`): `Scatter`'s single-lane specular
branch, each of `Scatter`'s three per-channel lanes (each with its own
`(r_i, p_i)`, hence its own `R_i`), and `ScatterNM`.

**Density sites.**  `RandomlySelect` weights by `MaxValue(kray)`, and
`R` is a nonnegative scalar, so the realized selection weight is
`R * (rho_max + (1 - rho_max) * F)` — the DL-67 Slice-0 aggregate
construction has to carry the same factor or `Pdf()` stops describing
the sampler:

* `SchlickDiffuseSelectCoefficient`'s `C_D` quadrature — via a new
  node-exact `SchlickQuadRows::azim[j][b] = A(phi_b)/(2 pi p_phi(phi_b))`
  row, so the inner loop gains two multiplies and a divide and **no**
  transcendental (the node's own half-vector has `w = sin(phi_b)`
  exactly, no normalize, no dot products);
* `SchlickSpecularDensity`'s `w_i`;
* `SchlickReplaySpecular`, which now reports the replayed lane's `R` for
  the per-channel branch's `wOther`.

---

## 5. Measurements

### 5.1 `tests/SchlickKrayBRDFConsistencyTest.cpp` (the red-proof)

Isolated build with **only** `src/Library/Materials/SchlickSPF.cpp`
reverted to `38e7939e`, running the same (final) test:

| | pre-fix | post-fix |
| --- | --- | --- |
| total | `Passed: 324  Failed: 213` | `Passed: 537  Failed: 0` |
| §1 worst \|per-draw − 1\| | 1.798527627 | 0.000000000 |
| §1 worst \|mean − 1\| | 0.713139892 | 0.000000000 |
| §3 worst \|E/Q − 1\| | 0.763426 | 0.005859 |
| §4 (NM) worst \|per-draw − 1\| | 1.370385987 | 0.000000000 |

`§1` is the identity (*) measured per draw over a
(rho, roughness, isotropy, theta) grid with the diffuse painter black,
so `value()` **is** the specular term; `§3` is
`E[sum_I max(kray_I)]` (what the sampled continuation carries away)
against `Q = int max(f_agg) cos dw` from a 400x800 quadrature of
`value()`.  Post-fix `§1`/`§4` are exact to below the printed 1e-9 —
(**) is exact arithmetic, and both sides recover `h` the same way.

Selected pre-fix `§1` rows (mean of `kray_S p_S / (f_S cos)`):

| rho | rough | iso | theta | mean | min | max |
| --- | --- | --- | --- | --- | --- | --- |
| 0.1 | 0.1 | 1.0 | 0 | 1.00000000 | 1.00000000 | 1.00000000 |
| 0.1 | 0.1 | 0.5 | 0 | 1.15226351 | 0.35361409 | 1.41421356 |
| 0.4 | 0.5 | 1.0 | 60 | 0.75238117 | 0.13816081 | 1.86061980 |
| 0.9 | 0.8 | 1.0 | 80 | 0.37356486 | 0.01540690 | 1.98091130 |

and the matching `§3` energy ratios `E[sum kray] / Q`: 0.999576 at
(0.1, 0.1, 1.0, 0 deg) down to **0.237970** at (0.9, 0.8, 0.5, 80 deg).

### 5.2 `SchlickLobePairingTest` (DL-69's closed-form table), re-quoted

Deterministic (fixed seeds; three consecutive runs printed identical
digits).

| incidence | (a) `E[kray_I/q_I]/Q` pre-fix | (a) post-fix | (b) `E[f_agg cos/P_mix]/Q` post-fix | (c) mismatched `/Q` post-fix |
| --- | --- | --- | --- | --- |
| 0 deg | 1.00001 | **1.00001** | 1.01051 | 1.99896 |
| 30 deg | 0.96142 | **1.00014** | 0.99783 | 1.99992 |
| 60 deg | 0.80224 | **1.00085** | 0.99816 | 2.00622 |

Estimator **(a)** — RISE's shipped per-lobe pairing — now lands on the
BRDF's own hemispherical integral at every incidence, which is the whole
point of DL-127.  Estimator **(b)**'s residual is DL-67's `C_D`
quadrature residual and is **not** this row's defect; re-measured with
the file's own 24 x 50000-draw replicate:

| incidence | (b)/Q mean | sem | z = (mean−1)/sem |
| --- | --- | --- | --- |
| 0 deg | 1.010486 | 0.000027 | +389.0 |
| 30 deg | 0.997799 | 0.000070 | −31.2 |
| 60 deg | 0.998725 | 0.000135 | −9.5 |

The **+1.05 % at normal incidence is bit-identical to the pre-fix
reading** (1.010486, same digits) — as section 2 predicts, `R == 1`
identically over the whole lobe at normal incidence with `p = 1`, so
DL-127 cannot have moved that row.  The 30/60-degree readings did move
slightly (from 0.997092 / 0.999922), which is the `C_D` quadrature
responding to a differently-shaped integrand — see 5.4.

`SchlickLobePairingTest`'s own `(a)`-vs-`Q` band was **tightened from
0.25 to 0.01** and its "reference-value caveat" rewritten: the
Schlick-1994 model gap it documented is exactly what DL-127 removed.

### 5.3 `SPFBSDFConsistencyTest`'s Schlick furnace tolerance — tightened 15x

That suite's Part C integrates `int BRDF cos dw` two ways: an MC sum over
`Scatter`'s `kray` values, and a quadrature of `value()`.  DL-127 is
exactly the reason those two disagreed, and the entry's own comment
attributed the gap to "a known limitation of the Schlick approximation
at grazing angles, not a sampling bug."  It was a sampling bug.

| | MC | quadrature | rel. err |
| --- | --- | --- | --- |
| 30 deg, pre-fix | — | — | **2.1 %** (the entry's recorded figure) |
| 60 deg, pre-fix | — | — | **12.7 %** (ditto) |
| 30 deg, post-fix | 0.740380 | 0.739452 | **0.1253 %** |
| 60 deg, post-fix | 0.810077 | 0.808799 | **0.1578 %** |

Tolerance `0.15 -> 0.01` (about 6x headroom over the measured worst
case; deliberately not tighter, because the MC arm now carries the
grazing tail of DL-178's un-shadowed model).

### 5.4 `SchlickSPFPdfConsistencyTest`'s mass gate — a real, small cost

`Pdf()`'s `C_D` is a deterministic `kSpecQuadN x kSpecQuadN` = 16x16
stratified quadrature of `wD/(wD + wS)` over the specular sampler's own
`(xi, b)` square.  DL-127 puts a `1/((n.v)(n.h))` factor into `wS`, so
the integrand is more sharply varying and **the 16-grid's own residual
grows** — while the underlying model gets strictly better.  Gate 1
(`|int Pdf - measured emission probability|`) on the four low-roughness
rows, all three states measured on this machine:

| row | pre-DL-127, grid 16 | DL-127, grid 16 | DL-127, grid 32 |
| --- | --- | --- | --- |
| `r.05 rd.6 rs1 i.3 th15` | 0.00836 | 0.00086 | 0.00019 |
| `r.05 rd.6 rs1 i.3 th45` | 0.00219 | 0.00624 | 0.00022 |
| `r.05 rd.6 rs1 i.3 th70` | 0.00566 | **0.01106** | 0.00230 |
| `r.02 rd.6 rs1 i.3 th85` | 0.00834 | 0.01192 | 0.00414 |
| `DL-101 CLOSED pc wide grazing` | 0.01216 | 0.01027 | 0.00490 |
| suite | 44/0 | 43/1 | **44/0** |

At grid 32 **every row in the file reads better than the pre-DL-127
build did at 16** — i.e. this is a quadrature-resolution residual, not a
modelling error introduced by the fix.  The grid stays at 16 for the
cost reason DL-67 section 4f already recorded (~3.3-4x per `Pdf()` call,
and BDPT evaluates it twice per non-delta vertex since DL-69), and the
one row that crossed is moved to `kMassTolLowRough` (0.015), the same
honestly-widened tolerance its two low-roughness siblings already carry,
with the table above quoted in the test file.  No ledger row: the lever
is known and recorded in two places.

### 5.5 Renders

**Reach.**  Four shipped scenes bind `schlick_material`
(`grep -rl schlick_material scenes/`).  TWO of them —
`scenes/FeatureBased/Combined/showroom.RISEscene` and
`scenes/Tests/Materials/materials.RISEscene` — render under
`pixelpel_rasterizer`, which is direct-only BY DESIGN (DL-26), so their
Schlick surfaces are shaded entirely through NEE, which evaluates
`value()` and is **unchanged by this fix**.  The two that can move are
`scenes/Tests/BDPT/cornellbox_bdpt_materials_pt.RISEscene` (PT) and
`cornellbox_bdpt_materials.RISEscene` (BDPT), where the material is on
ONE sphere of nine.

**Protocol.**  Copies at 256x256 (from 512), `oidn_denoise FALSE`,
`pixel_filter box`, EXR `Rec709RGB_Linear`, shipped sample counts (PT
128, BDPT 64), n = 3 renders per build.  Pre-fix build = an isolated
rebuild with ONLY `src/Library/Materials/SchlickSPF.cpp` at `38e7939e`.
"schlick region" is the 37x37 pixel box (156,110)-(192,146), the
projected extent of the `mat_schlick` sphere (center `416 275 280`,
radius 65, pinhole at `278 273 -800`, fov 39).  Values are the mean of
the three RGB channels.

| | image mean | image max | schlick-region mean | schlick-region max |
| --- | --- | --- | --- | --- |
| PT pre | 0.968556 +/- 0.000313 | 20.7969 +/- 0.0000 | 0.606880 +/- 0.003306 | 11.5920 +/- 0.4926 |
| PT post | 0.969135 +/- 0.000916 | 26.8214 +/- 10.4347 | 0.603374 +/- 0.001958 | 11.6875 +/- 0.2909 |
| | **+0.060 % (t = 1.0, NS)** | | −0.58 % (t = −1.6, NS) | |
| BDPT pre | 0.957620 +/- 0.000268 | 26.4805 +/- 5.3001 | 0.574250 +/- 0.003092 | 10.8672 +/- 0.4714 |
| BDPT post | 0.956116 +/- 0.000265 | 21.8978 +/- 1.9068 | 0.577474 +/- 0.001275 | 10.6181 +/- 0.5243 |
| | **−0.157 % (t = −6.9)** | | +0.56 % (t = 1.7, NS) | |

**Read this honestly**: on these two shipped scenes the change is at or
below MC noise except for BDPT's whole-image mean.  That is expected and
not evidence against the fix — the material covers one sphere of nine,
and the direct lighting on that sphere (NEE through `value()`, the
larger term at these settings) does not move at all.  The high-signal
render-level measurement is `BDPTStrategyBalanceTest`'s **topology L**,
whose walls and floor are ALL `schlick_material` (32x32, 256 spp, box
filter, no OIDN, PT and BDPT rasterizer strings from the suite itself),
n = 4 whole-suite runs per build:

| | PT mean | BDPT mean | BDPT/PT |
| --- | --- | --- | --- |
| topology L pre | 0.0601669 +/- 0.0000120 | 0.0632870 +/- 0.0000023 | 1.05186 +/- 0.00021 (+5.186 %) |
| topology L post | 0.0608636 +/- 0.0000156 | 0.0642661 +/- 0.0000082 | 1.05590 +/- 0.00035 (+5.590 %) |
| delta | **+1.158 % (t = 71)** | **+1.547 % (t = 230)** | +0.404 pp (t = 20) |
| topology M pre (control) | 0.0471248 +/- 0.0000039 | 0.0472097 +/- 0.0000036 | 1.00180 +/- 0.00013 (+0.180 %) |
| topology M post (control) | 0.0471305 +/- 0.0000041 | 0.0472156 +/- 0.0000041 | 1.00181 +/- 0.00006 (+0.181 %) |
| delta | +0.012 % (t = 2.0) | +0.013 % (t = 2.2) | +0.001 pp (t = 0.1, NS) |

(sigma is the sample standard deviation over the 4 runs; t uses the
standard errors.)  So a Schlick-dominated room gets **~1.2 % brighter
under PT and ~1.6 % under BDPT**, and the `ggx_material` control
topology M does not move — the change is Schlick-specific, as it must
be.  The direction is up because the missing factor `R` exceeds 1 on
average over the sampled lobe away from normal incidence (section 5.1's
pre-fix `E[sum kray]/Q` readings are *below* 1 everywhere away from
normal incidence, down to 0.238 at 80 degrees -- the sampled path was
under-delivering the BRDF).

**On the band.**  The suite's topology-L check is PT-vs-BDPT at an 8 %
band, and it stays green: the residual grows from +5.19 % to +5.59 %,
leaving 2.4 pp of margin.  That residual is **DL-103's**, not this row's
— the ledger's own DL-69 note attributes it to PT's un-guided escape-side
MIS partner still being the SELECTED lobe's density while its NEE side
uses the true aggregate, and topology M (immune to both DL-103 and
DL-127) reads +0.18 % before and after.  The band is PROVISIONAL pending
DL-103; **what DL-127 does to it is widen the L residual by 0.40 pp**,
which is the expected sign: DL-127 raises the energy the BSDF-sampled
arm carries at a multi-lobe Schlick vertex, and that arm is exactly the
one whose MIS partner DL-103 still mis-weights.  A concurrent slice is
changing that partner; when it lands, topology L should be re-measured
rather than assumed.

---

## 6. Sibling audit

**Bug pattern in one sentence:** an SPF stamps a lobe's `kray` with the
model's own *sampling weight* instead of that lobe's `f_I cos / p_I`, so
the sampled continuation and the paired BSDF integrate different
functions.

Probed with the same instrument (`tests/SchlickKrayBRDFConsistencyTest.cpp`
section 6, non-gated, reporting only), 20000 `Scatter()` calls per row at
0/30/60/80 degrees, specular reflectance 0.5, roughness/alpha 0.3:

| SPF | contract probed | `kray * pdf / (f cos)` mean, 0/30/60/80 deg | verdict |
| --- | --- | --- | --- |
| `SchlickSPF` (control, fixed) | per-lobe | 1.000000 / 1.000000 / 1.000000 / 1.000000 | fixed here |
| `IsotropicPhongSPF` | per-lobe | 1.000000 / 1.000000 / 1.000000 / 1.000000 | **IMMUNE** |
| `AshikminShirleyAnisotropicPhongSPF` | per-lobe | 1.000000 / 1.000000 / 1.000000 / 1.000000 | **IMMUNE** |
| `CookTorranceSPF` | single-emit, aggregate | 1.003697 / 0.999185 / 1.003956 / 1.005274 | **IMMUNE** |
| `GGXSPF` | single-emit, aggregate | 1.001784 / 0.989684 / 0.996688 / 0.999864 | **IMMUNE** |
| `WardIsotropicGaussianSPF` | per-lobe | 1.100053 / 1.385029 / 2.950590 / **5.878197** | **SAME DEFECT** |
| `WardAnisotropicEllipticalGaussianSPF` | per-lobe | 1.056958 / 1.324292 / 2.968491 / **6.071721** | **SAME DEFECT** |

Notes on the verdicts:

* The two **single-emit** rows are not pointwise 1 and are not meant to
  be — their `kray` is the internal-selection estimator
  `f_I cos / (p_agg * pSelect_I)`, whose expectation over that internal
  choice is `f_agg cos / p_agg`, so individual draws span `[0.006, 37]`
  while the mean lands on 1.  (This is exactly what DL-69's topology-M
  note asserts about `GGXSPF`, now measured.)
* **`IsotropicPhongSPF` and `AshikminShirleyAnisotropicPhongSPF` read
  1.000000 to every printed digit** — their krays already ARE
  `f_I cos / p_I`.  Worth recording, because DL-98/DL-99 rebuilt those
  two classes' `Pdf()` days earlier and it would have been easy to
  assume the same family shared this defect too.
* **Both Ward SPFs carry it**, and more severely than Schlick did
  (per-draw maxima 685x and 1085x at 80 degrees, against Schlick's
  worst 58x).  Filed as **DL-177**, not fixed here — see section 7.

Not probed, with reasons: `PolishedSPF` and `CompositeSPF` have no
paired `IBSDF` of their own to measure against; `TranslucentSPF`'s
`kray` deliberately carries Beer extinction its BSDF omits (a documented
convention, DL-01/DL-69, not this pattern); `CoatedSPF`, `FabricSPF`,
`WeaveSPF` are single-emit wrappers already covered by
`SPFBSDFConsistencyTest`'s own paired furnace at the default 5 %
tolerance.

---

## 7. New debts opened

**DL-177** — `WardIsotropicGaussianSPF` and
`WardAnisotropicEllipticalGaussianSPF` carry the DL-127 pattern: their
lobes' `kray` is the reflectance painter's own colour (a
direction-INDEPENDENT `GetColor(ri)`), not `f_I cos / p_I`, so the
sampled continuation and `Ward*BRDF::value` integrate different
functions — measured above at up to 6.07x on the mean and 1085x on a
single draw.  Deliberately not fixed in this slice: closing it needs the
same two-sided change DL-127 needed, and Ward's `Pdf()` is a
raw-albedo-weighted mixture *precisely because* its krays are currently
direction-independent (that is the reason DL-98/DL-99 recorded Ward as
immune to the selection-weight pattern), so both classes would need the
DL-67/DL-98/DL-99 aggregate-`Pdf` quadrature construction built for
them; and the corrected weight carries Ward's unbounded
`1/sqrt(nl nv)` tail, which needs an answer of its own before it can be
put in front of a path tracer.

**DL-178** — `SchlickBRDF` implements Schlick 1994's directional factor
WITHOUT the model's geometric self-shadowing term
`G(v) = v/(r - r v + v)`, so its directional albedo is unbounded at
grazing.  An independent 800x1600 quadrature of `int f_S cos dw` written
straight from the model's formulas (a throwaway Python script, NOT
reading RISE code — the point was to confirm the model itself, not the
implementation; `SchlickKrayBRDFConsistencyTest` section 3's `Q` column
is the implementation's own answer and agrees) reads, with the diffuse
reflectance ZERO so the number is the specular lobe alone and isotropy 1:

| roughness | rho | 0 deg | 30 deg | 60 deg | 80 deg |
| --- | --- | --- | --- | --- | --- |
| 0.1 | 0.9 | 0.8182 | 0.8231 | 0.8744 | **1.3145** |
| 0.5 | 0.9 | 0.6000 | 0.6423 | 0.8924 | **2.0785** |
| 0.8 | 0.9 | 0.5000 | 0.5614 | 0.9000 | **2.4187** |

(the same integral `SchlickKrayBRDFConsistencyTest` section 3 prints as
`Q`, where the lit-diffuse version reaches 2.776981).  That gain has
always been in NEE, which evaluates `value()` directly; DL-127 makes the
BSDF-sampled continuation agree with it, so it now also reaches indirect
transport — a **brighter grazing rim on every `schlick_material`**, and
a heavier firefly tail.  Adding `G` is a change to the shipped look of
the material and a separate piece of work.  `SchlickBRDF::albedo()`'s
claim that "integrated reflectance simplifies to Rd + Rs" belongs to the
same row: it is an approximation that can exceed the `IBSDF::albedo`
contract's `[0,1]` (`Rd = 0.4, Rs = 0.9` gives 1.3), which this slice
corrected in the comment but not in the value.

Deliberately NOT given a row: the `kSpecQuadN=16` quadrature residual of
section 5.4 (known lever, recorded in two places, and DL-67 section 4f
already owns the grid-size decision).
