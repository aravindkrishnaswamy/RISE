# DL-225 — a bounded, reciprocal Schlick specular family

Slice `debt-dl225`, 2026-09-28.  Closes the residual DL-178 left open
([DL178_DL212_BOUNDED_SCHLICK_WARD.md](DL178_DL212_BOUNDED_SCHLICK_WARD.md),
"Schlick residual"): after Schlick 1994 Eq.31's geometric factor
`G(c) = c/(r + (1-r)c)` removed the grazing divergence, the specular
family still had finite directional reflectance above one — the ledger's
witness cell (`rho .9, r .1, isotropy 1, 89.9°`) integrated to 1.2445.

**Ruling.** The masking is now

```
m(v) = min( G_Eq31(n.v),  n.v / I_bound(v) )
```

where `I_bound(v)` is a closed-form UPPER bound of the projected area of
the microfacets `v` can see under Schlick's OWN distribution `Z(t)A(phi)/pi`.
The model is Eq.31 wherever Eq.31 already respects that projected-area
limit, and the Smith-bounded masking of Schlick's distribution where it
does not.  It is reciprocal (the BRDF is `S Z A m(v) m(l) / (4 nv nl)` and
`m` depends on one direction), bounded (proof below; max over the grid
0.9943), and applied in lockstep to `value`/`valueNM`/`albedo`, the SPF's
`kray`, the DL-67 `C_D`/`q_i` replay that reads the realized `kray`, and
`EvaluateKrayNM`/`EvaluateLobeFNM` — all through ONE helper,
`src/Library/Materials/SchlickMasking.h`.  It is a model change confined to
a grazing, low-roughness band; the ruling on that is in §5.

## 1. Measurement of the shipped model

Formula-only half-vector quadrature, independent of the SPF's sampler
(`xi = u^2` regularized, 512 × 1024; the witness cell reproduces the
ledger's convergence study: 1.24461 at 256 radial nodes, 1.24456 at
512, 1.24455 at 1024 through 4096).  The grid is `r ∈ {.005,.01,.02,.05,.1,.2,.3,.5,.8,1}`,
isotropy `p ∈ {.01,.05,.1,.3,.5,.7,1}`, incidence
`θ ∈ {0,30,60,70,75,80,85,88,89,89.9}°`, view azimuth from `onb.u()`
`φv ∈ {0,45,90}°` (1900 cells).  `ρ_d` is linear in the reflectance `rho`,
and `rho = 1` (`S ≡ 1`) is its maximum; the `rho .9` column is also kept.

| θ | cells | cells with ρ_d > 1 (rho 1) | worst pre-fix (r, p, φv) | worst post-fix | worst change where pre ≤ 1 (rho 1) | same, rho .9 |
|---:|---:|---:|---|---:|---:|---:|
| 0 | 190 | 0 | 0.9946 (.005, 1, 0) | 0.9943 | −0.24% | −0.24% |
| 30 | 190 | 0 | 0.9930 (.005, 1, 0) | 0.9926 | −0.33% | −0.33% |
| 60 | 190 | 0 | 0.9842 (.005, 1, 0) | 0.9826 | −0.97% | −0.98% |
| 70 | 190 | 0 | 0.9754 (.005, 1, 0) | 0.9703 | −1.52% | −1.55% |
| 75 | 190 | 0 | 0.9683 (.005, 1, 0) | 0.9543 | −2.90% | −2.95% |
| 80 | 190 | 0 | 0.9619 (.005, 1, 0) | 0.9307 | −8.20% | −8.25% |
| 85 | 190 | 26 | 1.1301 (.05, .5, 0) | 0.9146 | −15.36% | −20.78% |
| 88 | 190 | 61 | 1.6316 (.02, .5, 0) | 0.9488 | −16.30% | −22.48% |
| 89 | 190 | 71 | 2.1990 (.01, .5, 0) | 0.9669 | −14.53% | −20.96% |
| 89.9 | 190 | 81 | **5.6194** (.005, .3, 0) | 0.9776 | −16.63% | −17.24% |

239 of 1900 cells exceed one at `rho 1` (all at `θ ≥ 85°`, all at
`r ≤ 0.1`); the worst is **5.6194** (5.3085 at `rho .9`), not the
ledger's 1.2445, which is one cell of this grid (the post-fix value of that
cell is 0.7899).  The post-fix maximum over all 1900 cells is **0.9943**, at
normal incidence (`r .005, p 1`) — the model's own single-scattering loss.

**Where the excess lives: both.**  Isotropically `Z` is GGX with
`α² = r`, and Eq.31 scales as `nv/r` at grazing where exact Smith gives
`2 nv/√r`; the two cross at `cos = (1-4r)/(3-4r)` for `r < 1/4` and Eq.31
under-masks below it.  That is the dominant term (5.2167 at `r .005`
isotropically).  Anisotropy then ADDS: per (r, θ), the worst `p < 1` cell
exceeds the isotropic one in every violating row, up to ~10%, and at
`r .1, θ 85°` the isotropic cell is conservative (0.9693) while `p .5`
violates (1.0365) — so the `A` family needs its own treatment, as the
ledger row said.

| r | θ | isotropic pre | post | worst anisotropic pre (p, φv) | post |
|---:|---:|---:|---:|---|---:|
| .005 | 85 | 1.0070 | .8802 | 1.0103 (.7, 0) | .8827 |
| .005 | 89.9 | 5.2167 | .9302 | 5.6194 (.3, 0) | .9524 |
| .02 | 88 | 1.5404 | .8965 | 1.6316 (.5, 0) | .9009 |
| .05 | 85 | 1.0741 | .8582 | 1.1301 (.5, 0) | .8679 |
| .1 | 85 | 0.9693 | .8293 | 1.0365 (.5, 0) | .8388 |
| .1 | 89.9 | 1.3625 | .8649 | 1.4997 (.3, 0) | .8873 |

(20 rows in the working table; these are representative.)

## 2. Choosing the formulation

* **(a) Smith height-correlated masking with Schlick's own `D`.**  Exact
  for the microfacet model and reciprocal, and it is what this slice
  implements *where Eq.31 overshoots*.  Adopting it *everywhere* was
  rejected on the appearance gate: Schlick's `k = r` masks much harder
  than Smith at high roughness, so the substitution brightens every rough
  Schlick material — isotropic `ρ_d` ratios Smith/Eq.31 of 1.05 / 1.19 /
  1.32 at `r .3` (0/60/80°), 1.18 / 1.61 / 2.26 at `r .8` — in cells that
  were already conservative.  For the anisotropic `A` family there is no
  closed-form Smith Λ at all (`Z(θ)A(φ)` is separable, not a stretched
  GGX).
* **(b) Geisler-Moroder–Dür-style bounded normalization (DL-212's Ward
  precedent).**  The GMD rewrite is specific to Ward's Gaussian in the
  unnormalized half vector; its Schlick analogue (the Kelemen
  `G/(nl nv) → 1/(h.v)²` denominator) changes the look at every angle and
  only bounds `ρ_d` by 2 without further argument.  Rejected.
* **(c) Kelemen / Kulla–Conty energy normalization.**  Kulla–Conty adds
  energy (it compensates LOSS) and does not remove a gain.  The only
  normalization that reduces exactly to Eq.31 where Eq.31 is conservative
  is `f/max(1, ρ_old(v), ρ_old(l))`, which needs the old model's
  directional albedo per direction — a 2-D integral per call, or a 4-D
  `(θ, φ, r, p)` table — at 256 replay nodes per `Pdf`, and is a rescale,
  not a masking.  Rejected.

**Chosen: Eq.31 clipped to the Smith projected-area limit of Schlick's
own distribution** — the physical constraint Eq.31 is an approximation
to.  For any microfacet distribution `D`, the projected area of the
facets `v` sees is `I(v) = ∫ max(0, v.m) D(m) dm`, and a masking can
never exceed `nv / I(v)` (the visible projected microfacet area cannot
exceed the macro-surface's own; this equality defines Smith's G1).

### Proof that the model is bounded and reciprocal

With `m ≤ 1`, `m(v) ≤ nv / I_bound(v)`, `I_bound ≥ I`, and `S ≤ 1`:

```
ρ_d(v) = ∫ f nl dω_l = ∫ S D m(v) m(l) (h.v)/nv dω_h
      ≤ (m(v)/nv) ∫ max(0, h.v) D dω_h = m(v) I(v)/nv ≤ I(v)/I_bound(v) ≤ 1.
```

The restriction to accepted directions (`nl > 0`, the geometric-horizon
gate) only removes mass.  Reciprocity: `S` and `D` depend on `h` only, and
`m(v) m(l)` is a symmetric product of one-direction factors, so
`f(l→v) = f(v→l)` identically — measured 2.2e-15 at five pairs chosen
INSIDE the bounded regime (§4).  The argument holds for `D` of any
normalization, which matters: Schlick's `A` integrates to
`c(p) = (1/2π)∫A dφ = √p / AGM(1,p) ≤ 1`, so the anisotropic `D` is
sub-normalized and `1 + Λ` can dip below one — harmless, since `m` is
also capped by Eq.31 ≤ 1.

### The closed-form projected area

The `θ` integral against `Z` is elementary; the signed part integrates to
`c(p) cos θv` because `A(φ+π) = A(φ)` kills the tangential term, and the
back-facing facets leave

```
I(v)/nv = c(p) + (k/2π) E(φv),   k = √r tan θv,
E(φv)   = ∫_{-π/2}^{π/2} A(φv + x) cos x atan(k cos x) dx.
```

(At isotropy 1, `E = π(√(1+k²) − 1)/k` and this is exactly GGX's
`Λ = (√(1+α² tan²θ) − 1)/2`.)  `E` has no closed form for `p < 1` (its
`k`-derivative is a complete elliptic integral of the third kind), so the
production helper uses two closed-form UPPER bounds and takes the smaller:

* **Jensen** (`atan` concave): `E ≤ W atan(k Q / W)` with
  `W = ∫_window A cos x = (2√p/q)[cos φv asinh(q cos φv/p) + sin φv asin(q sin φv)]`
  (antiderivatives `(√p/q) asinh(q sin φ/p)` of `A cos φ` and
  `−(√p/q) asin(q cos φ)` of `A sin φ`, `q = √(1−p²)`) and `Q = ∫_window A cos² x = (π c(p) + cos 2φv C2(p))/2`,
  `C2(p) = ∫_0^π A cos 2φ dφ = π c(p) S(p)` from the same AGM
  (`S = Σ 2^(m−1) e_m`, written without the `K − E` cancellation).  Within a
  few percent of `E` at every azimuth (worst ~10% across the grain at
  `p .01`, 1–4% typical).
* **Rearrangement + Cauchy–Schwarz**: `A` restricted to any half-plane has
  the same symmetric-decreasing rearrangement (it is π-periodic), so
  `E(φv) ≤ E(0) ≤ √(M2(k,p) E_iso(k))` with
  `M2 = (π/q)[atan(k/q) − atan(k p/(q√(1+k²)))]`, evaluated through the
  atan-difference identity so it is exact at `q → 0`.  Exact at isotropy 1;
  used only for `p ≥ 1/2` (below it Jensen is within 0.50% of the smaller
  of the two in all 194 560 swept states).

Isotropy 1 short-circuits to the closed-form GGX Smith G1
`2c/(c + √(c² + r s²))`.  Isotropy `p > 1` is `A_{1/p}` rotated a quarter
turn (`A_p(φ) = A_{1/p}(φ + π/2)`), folded in `Prepare`.

### Cost structure

`W(c²)` is concave and increasing in `c² = cos²φv` (verified down to
isotropy 1e-4; it is the convolution of two symmetric-decreasing
functions), so the production code reads it through the minimum of 12
tangent lines at `c²_j = (j/11)^4` — an upper bound, ≤ 1.31% loose at
isotropy 1e-4 and ≤ 0.53% for isotropy ≥ .01 — built once per distinct
isotropy per thread.  A lane's `cFast` (the root of the `atan x ≤ x`
sufficient test, exactly `(1−4r)/(3−4r)` at isotropy 1 and 0 when
`r ≥ 1/4`) skips every direction above it with one comparison; two further
`atan`-free tests precede the single remaining `atan`.  The API is the
masking's DENOMINATOR `c/m(c)` (`r + (1−r)c` wherever Eq.31 stands), so
every call site keeps the one division the Eq.31 code had.

## 3. Lockstep application

| Site | File | Change |
|---|---|---|
| `value`, `valueNM` | `SchlickBRDF.cpp` | per-lane `Z A / (4π dv dl)`; channels with equal (r, p) share a lane |
| `albedo` (AOV) | `SchlickBRDF.cpp` | same masking in the 16×32 quadrature |
| `kray` (Scatter, ScatterNM, per-channel) | `SchlickSPF.cpp` `SchlickKrayRatioFromH` | `R = A (h.wi) nl / (2π t p_φ dv dl)` |
| `Pdf` specular half (`q_i`) | `SchlickSpecularDensity` | reads `SchlickKrayRatioFromH` |
| `Pdf` diffuse coefficient `C_D` | `SchlickDiffuseSelectCoefficient` | per-node `dl` from the replayed direction (`2(h.v)h − v`), `dv` once per lane |
| `EvaluateKrayNM`, `EvaluateLobeFNM` | `SchlickSPF.cpp` | read `SchlickKrayRatioFromH` |

**`SchlickSPF::Pdf` changed** (the realized selection weights inside the
DL-67 replay moved with `kray`), which matters for merge order with any
slice that pins Schlick `Pdf` values (DL-67's guided branches read the
aggregate `Pdf` as their MIS partner; no file overlaps).

## 4. Tests

| Suite | Pre-fix (isolated A/B against `6b91fd19`) | Post-fix |
|---|---|---|
| `SchlickKrayBRDFConsistencyTest` (sections 7–10 new) | **1299 / 150** — 138 of 455 grid cells and 6 of 18 per-channel lanes above 1 + 1e-3 (worst 5.6390, r .005, isotropy 3, 89.9°, view along `onb.v`), the ledger's witness cell (1.24458), and the 5 "pair exercises the bound" checks | **1449 / 0** — worst ρ_d 0.99417; witness cell 0.78993; reciprocity 2.2e-15; Eq.31 reduction 3.0e-15; bounded-regime `kray p = f cos` 1.6e-9 |
| `SchlickMaskingBoundTest` (new) | n/a (tests the new helper) | **21655 / 0** — AGM constants 5.6e-15 vs quadrature; semi-analytic projected area vs brute-force 2-D 1.2e-7; `m ≤ exact Smith G1` over 3969 states (worst 4.3e-12 above, i.e. equality at isotropy 1); tightness ≥ 0.9641 of exact Smith where it binds; isotropy-1 = closed-form GGX Smith 2.1e-16; `p > 1` symmetry; continuity at `p → 1`; `cFast` never skips a bound state (9600); W closed form 4.3e-13, concave, envelope ≥ W; 1470 extreme-parameter states (isotropy 1e-300..1e300, roughness and cos to 1e-300) finite and ≤ Eq.31 |
| `BoundedSchlickWardTest` | 917 / 0 with three grazing Schlick rows printed as an OPEN witness (1.244529893) | **920 / 0** — the three rows gated (r .1: 0.7899) |

Section 6's Schlick control row (isotropy 100) moved from a 1e-9 to a
1e-8 per-draw tolerance: `A` has a relative condition number ~p² = 1e4 in
the half-vector azimuth near the pole, and a 1-ulp difference between the
SPF's and `value()`'s reconstruction of `h` already moved that ratio by
1.7e-9 on the base commit (same draw, isolated A/B) — sections 1, 2, 4 and
10 gate the same identity at 1e-6.

Unchanged and green (post-fix counts): `SchlickSPFPdfConsistencyTest`
44 / 0 (int Pdf / TVD per row moved by ≤ .0024 / .0013; worst mass
residual 0.01014 against the 1.5% low-roughness band, worst TVD 0.00795
against 0.012 — no band could be tightened), `SchlickLobePairingTest`
27 / 0, `SPFBSDFConsistencyTest` pass (byte-identical output),
`LayeredWhiteFurnaceTest` 0 / 58 (no `schlick_material` row; byte-identical),
`PTGuidingMISPartitionTest` 101 / 0, `HWSSCompanionKrayTest` 189 / 0,
`BDPTStrategyBalanceTest` 170 / 0, `VCMStrategyBalanceTest` 74 / 0,
`CstDeriveGoldenTest` 454 MATCH / 0 DRIFT, `SourceHygieneTest` 167 / 0,
`SPFPdfConsistencyTest`, `CookTorranceSchlickGlossyFilterConsistencyTest`,
`PathValueOpsTest`, `SchlickWardBackfaceEnergyTest`,
`SchlickWardPerChannelReuseTest`, `WardDensityKrayTest`,
`ConnectionLegalityTest` 319 / 0.

`BDPTStrategyBalanceTest` topology L (all `schlick_material`, r .5,
isotropy 1 — never in the bounded band, `r ≥ 1/4`), n = 3 interleaved
separately built binaries, `--materials-only`:

| | base | fix |
|---|---:|---:|
| L / PT | 0.05824500 ± 0.00000470 | 0.05824830 ± 0.00000785 |
| L / BDPT | 0.05821276 ± 0.00000510 | 0.05821191 ± 0.00000389 |
| PT / BDPT | 1.000554 | 1.000625 |
| M / PT (GGX control) | 0.04713432 ± 0.00000142 | 0.04712932 ± 0.00000715 |

## 5. Appearance, and the ruling on it

Over the 1661 grid cells whose pre-fix `ρ_d ≤ 1`: 1240 change by < 0.1%,
1506 by < 1%, 58 by > 5%; the worst is **−16.63%** (`r .2, p .5, 89.9°`,
0.9855 → 0.8216).  No change at all for `r ≥ 0.3` in this grid; ≤ 0.97%
for `θ ≤ 60°`, ≤ 2.9% to 75°, ≤ 8.2% at 80°.  **Ruling: this is a model
substitution confined to the band where Schlick's Eq.31 masking exceeds
the Smith projected-area limit of Schlick's own distribution (grazing
directions at `r < 1/4` isotropically, up to `r ≈ 0.29` at isotropy .3–.5, and only `r < 0.07` at isotropy .01);
outside that band the shipped Eq.31 model is reproduced to rounding
(3.0e-15), and inside it the old values were not a physically attainable
reflectance.  Grazing highlights of low-roughness Schlick materials
darken; that is the intended correction, not a regression.**

Renders (linear EXR, `oidn_denoise FALSE`, separately built base/fix
binaries, whole-image and Schlick-object region means, mean ± sample SD):

| Scene | Rasterizer | n | Region | Base | Fix | Change | t |
|---|---|---:|---|---:|---:|---:|---:|
| `cornellbox_bdpt_materials` | BDPT 64 spp | 3 | whole | 0.945264 ± 0.000225 | 0.944980 ± 0.000082 | −0.030% | −2.1 |
| | | | Schlick sphere | 0.661268 ± 0.001874 | 0.661366 ± 0.000731 | +0.015% | +0.1 |
| `cornellbox_bdpt_materials_pt` | PT 128 spp | 3 | whole | 0.956433 ± 0.000374 | 0.956209 ± 0.000227 | −0.023% | −0.9 |
| | | | Schlick sphere | 0.675786 ± 0.003925 | 0.673573 ± 0.000987 | −0.327% | −0.9 |
| `materials` | pixelpel (direct) | 8 | whole | 0.176317 ± 0.000053 | 0.176345 ± 0.000097 | +0.016% | +0.7 |
| | | | Schlick teapot | 0.311530 ± 0.000442 | 0.311741 ± 0.000836 | +0.068% | +0.6 |
| `showroom` (shipped) | pixelpel + irradiance cache | 16 | whole | 0.373022 ± 0.069655 | 0.378099 ± 0.037275 | +1.36% | +0.3 |
| | | | ruby sphere | 1.144633 ± 0.110224 | 1.141673 ± 0.056512 | −0.26% | −0.1 |
| `showroom`, irradiance cache off | pixelpel | 8 | whole | 0.373777 ± 0.000158 | 0.373599 ± 0.000302 | −0.048% | −1.5 |
| | | | ruby sphere | 1.143547 ± 0.002716 | 1.140989 ± 0.003988 | −0.224% | −1.5 |
| all-Schlick cornell (r .05, isotropy .3), 256², | PT 32 spp | 3 | whole | 1010.61 ± 4.92 | 933.78 ± 32.34 | −7.60% | −4.1 |
| every receiver `mat_schlick`, white specular | BDPT 16 spp | 3 | whole | 25.8153 ± 0.1510 | 24.6704 ± 0.0560 | −4.43% | −12.3 |

No shipped scene moves resolvably: the two cornell boxes bind one sphere
among nine, the `materials` teapot is lit by a directional light well away
from grazing, and the shipped `showroom` is dominated by its irradiance
cache's run-to-run variation (whole-image SD 19%, a single base run at
0.612) — with the cache off the same scene is resolvable and reads
−0.05% / −0.22%.  The two constructed all-Schlick boxes show what the fix
does where it applies: a closed box of near-mirror white-specular walls
seen at grazing lost 4–8% of its (grazing-amplified) energy.

## 6. Cost

User CPU, n = 3, interleaved base/fix, separately built binaries, 256²:

| Scene | Base (s) | Fix (s) | Change |
|---|---:|---:|---:|
| mixed cornell (`cornellbox_bdpt_materials_pt` materials), PT 32 spp | 23.86 ± 0.34 | 24.34 ± 0.36 | +2.0% |
| all-Schlick, r .5 isotropy 1 (never bound), PT 32 spp | 31.55 ± 0.34 | 33.01 ± 0.28 | +4.6% |
| all-Schlick, r .05 isotropy .3 (bound at grazing), PT 32 spp | 41.24 ± 1.00 | 53.66 ± 1.62 | +30.1% |
| same, BDPT 16 spp | 107.97 ± 1.00 | 153.56 ± 1.87 | +42.2% |

The worst case is the bounded band itself: every `Pdf` at a grazing view
replays 256 specular draws whose outgoing directions are mostly grazing
too, and each such node now evaluates the bound (one `atan`, one `sqrt`,
the 12-tangent `W` envelope).  Profiling put `SchlickDiffuseSelectCoefficient`
(the pre-existing DL-67 replay) first in both builds and the new
`atan`/bound evaluation next.  Micro-benchmarks (ns/call, noisy shared
machine, n = 3): `Pdf` at r .05 isotropy 1 ≈ 0.9–1.3 → 1.3–1.4 µs,
at isotropy .3 grazing ≈ 1.2 → 1.9–2.2 µs; `value()` 15–25 → 21–53 ns;
`Scatter` unchanged within noise.  For comparison, DL-212's accepted Ward
coefficient is 20–51× its predecessor per changing-hit `Pdf`.

## 7. Sibling audit

* **Ward (DL-212).**  Does not share the defect or the derivation: its
  Geisler-Moroder–Dür normalization bounds its own weight by `2 Rs`, and
  DL-212's measured specular albedo is ≤ `Rs` in all 24 rows (`BoundedSchlickWardTest`
  Ward rows unchanged, 920 / 0).  GMD has no masking term to clip.
* **Other `G`-carrying BSDFs.**  No other file uses Eq.31 (`git grep`);
  GGX/CookTorrance carry their own Smith/V-cavity terms; the Phong siblings
  are bounded by their own normalizations (`BoundedSchlickWardTest`'s
  sibling section).
* **DL-67 guided branches.**  Untouched by this slice (no edits to
  `PathTracingIntegrator.cpp` or `BDPTIntegrator.cpp`); they consume the
  aggregate `Pdf`, which changed.

## 8. Residuals

* The closed-form bound is conservative: where it binds it sits at
  ≥ 0.9641 of the exact Smith G1 over `SchlickMaskingBoundTest`'s grid
  (exact at isotropy 1).  Slightly more masking than exact Smith in that
  band; not a debt (a tighter exact `E` needs an elliptic-integral
  quadrature per direction, which the replay cannot afford).
* Schlick's Eq.30 re-emission term is still not modelled (energy LOSS, the
  same as before DL-225).  Not a debt.
* The bounded-band cost in §6.
