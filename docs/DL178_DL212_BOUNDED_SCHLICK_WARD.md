# DL-178 / DL-212 — geometric correction for Schlick and bounded Ward

Derivations and initial experiments: 2026-09-19. Candidate validation: 2026-09-20.
Cost and isolated proposal experiments continued on 2026-09-21. Bounded Schlick
geometric attenuation (DL-178) and Geisler-Moroder & Dür bounded Ward transport
(DL-212) are formally accepted with exact domain-split quadrature and a 4-entry
thread-local evaluation cache. Finite grazing residual for Schlick is tracked
under DL-225 (CLOSED 2026-09-28 by the `debt-dl225` slice; the uncoupled diffuse term is DL-310). Appearance changes are documented below for both models. This document
separates a specular model's energy bound, the sum of independently authored
`Rd` and `Rs`, and the bounded auxiliary albedo AOV. These are different
contracts: an AOV saturation does not repair transport energy.

## Published definitions and independent derivation

Schlick's [1994 paper and author copy](https://dept-info.labri.fr/~schlick/DOC/eur2.html)
define Eq.31 as `G(c)=c/[r+(1-r)c]`, using the paper's roughness `r` directly.
RISE retains its existing directional distribution `Z=r/[1-(1-r)t²]²`,
azimuth factor `A=sqrt(p/[p²+(1-p²)w²])`, and Fresnel
`S=rho+(1-rho)(1-h.v)^5`. Here `t=n.h`, `h=normalize(l+v)`,
`nv=n.v`, `nl=n.l`. Multiplication by `G(nv)G(nl)` cancels the former
`nl nv` denominator analytically:

```
f_S = S Z A / [4 pi (r+(1-r)nv)(r+(1-r)nl)]
p_S = t Z p_phi / [2(h.v)]
kray_S = S A (h.v) nl /
         [2 pi t p_phi (r+(1-r)nv)(r+(1-r)nl)]
```

`Z` cancels in the weight; **roughness does not disappear** because `G`
contains `r`. Pel, NM, per-channel, companion `EvaluateKrayNM`, and the
aggregate Pdf's realized selection weights all carry this same factor.
Eq.30's additional re-emission term is not introduced: this change implements
the explicitly prescribed Eq.31 product on RISE's single-scattering model.

Both Ward materials adopt Geisler-Moroder and Dür,
[*A New Ward BRDF Model with Bounded Albedo* (2010)](https://doi.org/10.1111/j.1467-8659.2010.01735.x).
The [authors' presentation](https://www.radiance-online.org/community/workshops/2010-freiburg/PDF/geisler-moroder_duer_thetmeyer_RW2010.pdf)
shows the BRDF, unnormalised-half-vector rewrite, and sampling weight
(slides 12–13, 21–22). Set `ax=ay` for isotropy:

```
E = exp(-[(h.u/ax)^2+(h.v_tangent/ay)^2]/t²)
f_S = Rs E / [4 pi ax ay (h.v)^2 t^4]
p_S = E / [4 pi ax ay t^3 (h.v)]
kray_S = Rs nl / [(h.v)t] = Rs 2 nl/(nv+nl)
```

The new Ward weight is strictly less than `2 Rs` on accepted directions;
its roughness cancels exactly. The Gaussian sampler and stored density stay
unchanged. An independent test reference uses `H=l+v` and
`f_S=Rs E |H|²/[pi ax ay (H.n)^4]`, avoiding the production normalization.
The diffuse lobe remains additive `Rd/pi`; over-authored `Rd+Rs` can still
exceed one even though the Ward specular family is bounded by `Rs`.
**(Superseded 2026-09-28 by DL-310, section "DL-310" below: Ward's diffuse
is now `min(Rd, 1 - Rs)/pi` -- `Rd` bit for bit whenever `Rd <= 1 - Rs`.)**

## Schlick residual: DL-225 (closed by the `debt-dl225` slice, 2026-09-28)

**Superseded by [DL225_BOUNDED_SCHLICK.md](DL225_BOUNDED_SCHLICK.md):** the
masking is now `min(Eq.31, the Smith projected-area bound of Schlick's own
Z*A)`, shared by the BRDF, the SPF `kray`, the aggregate `Pdf` replay and
the auxiliary albedo; the worst directional reflectance over a 1900-cell
grid falls from 5.6194 to 0.9943 and the witness cell below from 1.2445
to 0.7899.  The rest of this section is the historical DL-178-time
measurement.


Eq.31 removes the missing-G divergence, but does **not** make this complete
Schlick specular family energy-conserving. Formula-only half-vector
quadrature at `rho=.9,r=.1,p=1,theta=89.9°` converges as follows:

| Radial × azimuth nodes | Specular directional reflectance |
|---|---:|
| 64 × 128 | 1.2455530234 |
| 128 × 256 | 1.2448134128 |
| 256 × 512 | 1.2446129561 |
| 512 × 1024 | 1.2445614434 |
| 1024 × 2048 | 1.2445492385 |

Independent outgoing-angle quadrature of the live corrected BRDF gives
1.244529893. This is a finite model residual, not sampler/BRDF disagreement.
For isotropy, `Z` is a GGX distribution with `alpha²=r`; at grazing, exact
Smith `G1` scales as `2 nv/sqrt(r)` whereas Eq.31 scales as `nv/r`. The
latter is too large when `r<.25`. Replacing just that isotropic factor does
not establish a bound for RISE's anisotropic `A` family. DL-225 therefore
requires a reciprocal normalization/masking treatment for both families,
with weights and aggregate Pdf updated together. No arbitrary transport
clamp or unreviewed lookup table is part of this slice. The residual witness
is printed as open, not mislabeled as a passing energy bound.

The original DL-178 family is corrected:

| r | Angle | Before Q | With Eq.31 Q |
|---:|---:|---:|---:|
| .1 | 0 | .818189401 | .771496478 |
| .1 | 30 | .823127970 | .752213958 |
| .1 | 60 | .874427388 | .700605683 |
| .1 | 80 | 1.314463504 | .761652130 |
| .5 | 0 | .600015575 | .405628244 |
| .5 | 30 | .642314289 | .399614428 |
| .5 | 60 | .892408981 | .392111758 |
| .5 | 80 | 2.078542030 | .403208766 |
| .8 | 0 | .500016140 | .278303630 |
| .8 | 30 | .561369883 | .277693191 |
| .8 | 60 | .900005384 | .277182101 |
| .8 | 80 | 2.418726332 | .278490429 |

These are formula-only outgoing-angle 800×1600 integrations at `rho=.9,p=1`.

## DL-310 — the coupled diffuse term (`debt-dl310`, 2026-09-28)

DL-225 bounds the Schlick SPECULAR lobe; `SchlickBRDF::value` still added
`Rd/pi` with no coupling, and Schlick's `S = rho + (1-rho)(1-h.v)^5` drives
the lobe's directional albedo `A(v)` toward 1 at grazing for EVERY `rho`, so
the full material exceeded 1 even for authored `Rd + rho <= 1`.  Ward's
Geisler-Moroder–Dür family integrates to at most `Rs`, so Ward exceeded 1
only for over-authored `Rd + Rs > 1`.

### Measurement (independent quadrature of the live `value()`)

Two deterministic midpoint grids -- cosine-warped over the outgoing
hemisphere, and the half-vector warped by the lobe's own shape (Schlick:
GGX `alpha^2 = r` with `xi = u^2`; Ward: the elliptical Gaussian slope) --
combined with the balance heuristic so every outgoing direction counts once
(256 x 512 per grid; 128 vs 256 agree to 7e-4 over the whole grid; a
Lambertian-plus-DL-178-table cell reproduces the DL-178 figure to 3e-5).
Grid: `Rd, rho ∈ {.1,.5,.9}`, `r ∈ {.05,.2,.5,.8}`, isotropy `∈ {1,.5,.1}`
(Ward aniso: `ax = r`, `ay = r p`), `θ ∈ {0,30,60,80,89}`, view azimuth
`{0,45,90}` for isotropy < 1.

| Model | Authoring | Cells | Pre-fix cells > 1 | Pre-fix worst (r, p, Rd, rho, θ, azimuth) | Post-fix cells > 1 | Post-fix worst |
|---|---|---:|---:|---|---:|---:|
| Schlick | all | 1260 | 369 | **1.7448** (.05, .1, .9, .9, 89, 0) | 0 | 0.9996 |
| Schlick | `Rd + rho <= 1` | 840 | 34 | **1.1538** (.05, .1, .9, .1, 89, 90) | 0 | 0.9996 |
| Ward iso | all | 180 | 58 | 1.7977 (.05, 1, .9, .9, 0) | 0 | 0.9997 |
| Ward iso | `Rd + Rs <= 1` | 120 | 0 | 0.9997 | 0 | 0.9997 (identical) |
| Ward aniso | all | 1080 | 363 | 1.7989 (.05, .1, .9, .9, 0) | 3 | 1.000003 |
| Ward aniso | `Rd + Rs <= 1` | 720 | 3 | 1.000003 | 3 | 1.000003 (identical) |

The three Ward aniso "> 1" cells are the conserving `Rd .9, Rs .1, ax .05,
ay .005` normal-incidence cells, bit-identical before and after (quadrature
noise on an exactly-`Rd + Rs` material).  The ledger's quoted `Rd .1 +
rho .9 -> 1.0202` came from the DL-225 review's finer grid; that family's
worst on this suite's own re-measurement is 1.0151 (`r .005`, isotropy
.05, 89.9°), 0.9907 post-fix.  Schlick's violation is angle-independent
for over-authored materials (67-87 cells > 1 at every θ from 0 to 89) and
grazing-only for conserving ones.

### The coupling, and why not the DL-37 product

With `A(v)` an upper bound of the specular lobe's directional albedo,

```
f_D(i, o) = min( Rd, 1 - A(i), 1 - A(o) ) / pi        (per channel / lane)
```

* **Reciprocal:** symmetric in `i` and `o` (measured asymmetry 4.2e-15 at
  five pairs where the clip binds, `SchlickKrayBRDFConsistencyTest`
  section 13).
* **Bounded:** `rho_d(i) = A_true(i) + ∫ f_D cos <= A_true(i) + (1 - A(i))
  <= 1` whenever `A >= A_true` (any `Rd`, any `rho`, over-authored included).
  `A >= A_true` is enforced at the table's probes and MEASURED, not proven,
  between them: the external review's 15,925-cell extended grid (roughness
  down to .001, isotropy to .01, 89.9°) reads pre-fix 5143 cells > 1 (worst
  1.8486) and post-fix 0 (worst 0.99991), but one of its 5000 table probes
  under-reads the truth by 3e-3 and the full material there reads 1.00298
  (`Rd .5`, `rho 1`, roughness .024, isotropy .004, 89.62°: table 0.9531 vs
  true 0.9561) -- so the bound holds to within 3e-3 at isotropy below .01
  at grazing.
* **Minimal:** it equals `Rd/pi` BIT FOR BIT for every direction pair both
  of whose directions already conserve energy under the additive model
  (`Rd <= 1 - A(v)` at `v = i` and `v = o`, judged against the table's
  conservative `A`; see Residuals) -- DL-225's ruling applied to
  the diffuse term (section 13: 2.2e-16 relative, the three materials /
  direction pairs that do not clip).

Four candidates were compared on the grid (exact per-azimuth `A` from the
same quadrature; hemispherical-mean albedo change, worst..best over
`(r, p)`):

| Schlick `(Rd, rho)` | DL-37 `(1-A(i))(1-A(o))` | Kulla-Conty `/(1-Ā)` | product clip `c(i)c(o)` | **min clip (chosen)** |
|---|---|---|---|---|
| (.1, .1) | −9.7..−3.8% | −5.1..−1.9% | 0 | **0** |
| (.1, .9) | −12.8..−10.7% | −8.9..−6.7% | 0 | **0** |
| (.5, .5) | −36.8..−17.7% | −23.8..−9.4% | 0 | **0** |
| (.9, .1) | −18.0..−4.6% | −9.5..−2.3% | −2.2..0 | **−2.0..0** |
| (.9, .5) | −46.6..−19.2% | −30.1..−10.2% | −42.0..−3.4% | **−26.5..−2.6%** |
| (.9, .9) | −51.0..−30.1% | −42.3..−16.8% | −50.4..−18.1% | **−42.1..−11.4%** |

The DL-37 product form (what the ledger recipe named) is bounded and
reciprocal but moves EVERY material at EVERY angle, including normal
incidence where the additive model was already physical -- the
Rd-dominated, low-rho `(.9, .1)` material loses up to 18% -- and it moves
energy-conserving Ward materials by 7-17%, which have no defect at all.
By DL-225's own precedent (full Smith masking was rejected for
brightening already-conservative cells) it fails the appearance gate.
Kulla-Conty normalization is energy-exact for white diffuse but still
moves every material -- its directional albedo is `A(i) + Rd (1 - A(i))`,
not the additive `A(i) + Rd`, so the premise that normalizing by
`1 - Ā` leaves the material "unchanged on average" is false (external
review: conserving sub-grid mean change at normal incidence −7.5%, against
−13.5% for the DL-37 product and −0.05% for the min clip) (and has zero slack for an under-estimated `A` at
`Rd = 1`).  The product clip `Rd c(i) c(o)`, `c = min(1, (1-A)/Rd)`, is
bounded and reciprocal but darkens unclipped incoming directions through
`c̄ < 1`; the min form is pointwise the largest symmetric function with
`f_D <= (1 - A(v))/pi` at both ends and `f_D <= Rd/pi`, so it is never
darker.  With it the Rd-dominated low-rho material moves least: `(.9, .1)`
at most −1.27% for `θ <= 60°` (mean −0.24%), −4.34% at grazing, on the
implementation's own grid (below).

### Schlick's `A`: a baked, conservative table

`A(v) = rho M0(v) + (1 - rho) M5(v)`, with `M0` the lobe's unit-reflectance
albedo and `M5` its `(1-h.v)^5` moment, both functions of `(μ = n.v, φ_v, r,
p)`.  `tools/SchlickDirectionalAlbedoGen.cpp` bakes
`src/Library/Materials/SchlickDirectionalAlbedo_LUTData.cpp` (13299 nodes x 2
floats; deterministic, byte-reproducible; ~26 s on 12 cores) over `u =
sqrt(μ)` (33 nodes), `log r ∈ [-5, 0]` (31), `log p ∈ [-3, 0]` (13), storing
the **maximum over the view azimuth** (17 samples of the quarter period
`A(φ)`'s symmetry covers).  An azimuth maximum is both an upper bound and a
function of `μ` alone, which keeps the Pdf's diffuse-draw quadrature
one-dimensional; its price is over-darkening in the clip band at azimuths
where `A` is below its maximum (section 12: up to 0.48 absolute at `p .07`,
`φ_v` across the grain).  The generator calls `SchlickMasking.h` verbatim.
Quadrature: per half-vector azimuth the reflection is above the horizon
exactly for `θ_h < (atan2(V, n.v) + π/2)/2`, so the `θ_h` integral runs over
that interval (sampler warp, `ξ = u^2`, 64-point Gauss–Legendre), and `φ_h`
is Gauss–Legendre on sub-intervals graded geometrically toward `A`'s peaks;
16/64 vs 32/128 nodes agree to 1.3e-4.  **Conservative interpolation:** the
runtime interpolates trilinearly in `(u, log r, log p)`; every node is raised
by the largest deficit (true − interpolated) found at the probe points of
the cells it bounds (49562 probes: every cell centre and edge midpoint), so
the interpolant is >= the truth at every probe.  Off-probe validation (4000
Weyl points over the whole box): worst remaining deficit 3.2e-3 (`r 2.7e-5`,
`p .006`, 89.93°), 2 points > 1e-3; section 12's independent check on 408
off-node points of the practical range (`r .013-.33`, isotropy `.07-1.9`)
reads worst 6.96e-4 below the truth (quadrature noise of the check itself
at `p .07`; it was 2.6e-3 at the check's coarser 128 grid, 7e-4 at 256).
Outside the box: `r < 1e-5` uses the proven `M0 <= 1` and `M5 = max(table,
(1-μ)^5)` (measured within 3e-6 of an upper bound at `r <= 1e-6`); isotropy
below 1e-3 clamps (measured <= 7.4e-3 low, only beyond 89.99° at `r <=
1e-3`); isotropy > 1 folds exactly.  A two-entry thread-local memo keys the
`(r, p)` bracket (`SchlickMasking::Prepare`'s pattern).

### Ward's `A`: GMD's own bound `Rs`

Using `A = Rs` -- the Geisler-Moroder–Dür family's analytic albedo bound
(DL-212) -- makes the coupled term the per-channel constant
`min(Rd, 1 - Rs)`: `Rd` bit for bit whenever `Rd <= 1 - Rs` (every
energy-conserving authoring; `WardDensityKrayTest` section I (2): 2.2e-16),
`1 - Rs` otherwise.  Being direction-independent it keeps DL-177's ruling
intact -- Ward's selection quadrature needs NO second (diffuse-draw)
integral; only its diffuse weight changes, and `WardSelection::Evaluate`'s
cache key already contains it.  The cost of the supremum over the exact
directional `Rs M(v)`: over-authored ROUGH Ward loses up to ~24 percentage
points more of its hemispherical albedo than a directional clip would
(`(.5, .9)`: −39.5..−28.7% vs −28.5..−4.9%).  A directional Ward `A` would
make the diffuse weight direction-dependent, and Ward's boundary-aware `C_D`
quadrature (~20 µs cold, DL-212) would have to be re-run at every query
direction instead of hitting its per-view cache -- declined on cost for a
change confined to over-authored materials.

### Lockstep

| Site | Change |
|---|---|
| `SchlickBRDF::value/valueNM` | diffuse `min(Rd, 1-A(v), 1-A(l))/π` per channel, via `SchlickDirectionalAlbedo::CoupledDiffuseAt` |
| `SchlickBRDF::albedo` (AOV) | gated hemispherical integral of the coupled term (same band quadrature as `Pdf`) |
| `SchlickSPF::Scatter/ScatterNM` | diffuse `kray` = the same function at the sampled direction (its own `f_D cos/p_D`, section 14: 1.2e-13) |
| `SchlickSPF::Pdf/PdfNM` `C_D` | the replay runs at the query direction's coupled weight `w_D(wo)` |
| `SchlickSPF::Pdf/PdfNM` `q_i` | **second quadrature** over the diffuse draw (the DL-99 construction): `q_i = aD g(W0) + ∫ ds P(s)[g(w_D(s)) − g(W0)]`, `g(x) = w_i/(x + w_S)`, nonzero only in the band where a channel clips; in `u = sqrt(μ)` every channel's `A` is linear between table nodes, so each band interval is split at the exact clip crossings and at the geometric gate's kink, 3-point Gauss–Legendre per piece; `P(s)` is the closed-form fraction of azimuths passing the tilted geometric gate (integrates to `aD = (1+cos φ)/2`) |
| `SchlickSPF::EvaluateKrayNM/EvaluateLobeFNM` | diffuse branch returns the coupled term (`/π`) |
| Ward (both) value/valueNM/albedo, Scatter/ScatterNM, Pdf/PdfNM `wDiff`, EvaluateKrayNM/EvaluateLobeFNM | `WardSelection::CoupledDiffuse(Rd, Rs)` |

Every Schlick consumer goes through ONE function, `CoupledDiffuseAt`, which
returns `rd` itself unless the lane can clip anywhere (`Rd > 1 − A_top`,
`A_top` the row maximum), so a non-clipping Schlick material renders
identically (all-Schlick `r .5` box: 0.799022 vs 0.799149 PT, n = 5).

**Is the second quadrature necessary?**  At the shipped `kSpecQuadN = 16`
the `C_D` replay's own low-roughness mass bias (+0.5..+1%, DL-67 §4a,
DL-127 §5.4) partly masks it; at
`kSpecQuadN = 64` (bias ~1e-4) the four clip rows read `|∫Pdf − P(emit)|`
0.00085 / 0.00099 / 0.00019 / 0.00003 WITH it and 0.00022 / 0.00532 /
0.00607 / 0.00141 WITHOUT it (the `rd .9 rs .5 r .05 i .3 θ60` and `rd .8
rs .9 r .2 θ45` rows move 0.5-0.6% of the mass).  **Stale-replay controls**
(`SchlickSPFPdfConsistencyTest`, mass gate blind as documented, TVD gate
0.012): the new sampler with the pre-DL-310 `Pdf` fails 11 of 28 checks,
TVD 0.013-0.142 (the two rows it cannot see: `rd .9 rs .1 r .05 i .3 θ30`,
0.0059, whose clip band is a sliver near grazing, and the per-channel
`θ70` row, 0.0111); the new
sampler with the new `C_D` but no second quadrature reads 1 failure
(`r .05 rd .6 rs 1 θ70`, TVD 0.0124).  Ward with the new `kray` and the old
`wDiff`: TVD 0.088-0.107 on section I's over-authored rows (0.020 gate).
Post-fix every DL-310 row sits at 1.08-1.71x its own measured half-split
noise floor (0.0039-0.0050).

### Tests

| Suite | Pre-fix (this slice's final test files against `b89a8aa9`'s material sources) | Post-fix |
|---|---|---|
| `SchlickKrayBRDFConsistencyTest` (sections 11-15 new) | **422905 / 383** -- 369 grid cells > 1, the ledger family (8), the five clip-binding reciprocity pairs, section 14's "exercises the clip" | **423288 / 0** |
| `WardDensityKrayTest` (section I new) | **81398 / 170418** -- 418 grid cells over the 1e-3 band, every over-authored diffuse draw's kray | **251816 / 0** |
| `SchlickSPFPdfConsistencyTest` (six DL-310 rows + NM twins, noise floors) | 64 / 0 (the pre-fix sampler and density agree; the stale-replay control is the red) | 64 / 0 |
| `BDPTStrategyBalanceTest` (topology Y) | -- | 227 / 0 |

Unchanged and green: `SchlickMaskingBoundTest` 21655/0, `BoundedSchlickWardTest`
920/0, `SchlickLobePairingTest` 27/0, `SPFBSDFConsistencyTest`,
`SPFPdfConsistencyTest`, `LayeredWhiteFurnaceTest` 0/58,
`HWSSCompanionKrayTest` 189/0, `PTGuidingMISPartitionTest` 185/0,
`VCMStrategyBalanceTest` 74/0, `BDPTGuidedContinuationTest` 164/0,
`CstDeriveGoldenTest` 456 MATCH / 0 DRIFT, `SourceHygieneTest` 167/0,
`WardSelectionQuadratureTest` 969/0, `SchlickWardBackfaceEnergyTest` 19/0,
`SchlickWardPerChannelReuseTest` 4/0,
`CookTorranceSchlickGlossyFilterConsistencyTest` 22/0, `PathValueOpsTest`,
`AshikminShirleySPFPdfConsistencyTest` 99/0,
`IsotropicPhongSPFPdfConsistencyTest` 46/0, `ConnectionLegalityTest` 319/0,
`AgentChunkCrudTest` 3816/0, `SSSRadianceScalingTest` 576220/0;
`RefractiveRadianceScalingTest` 40/1 (row C, DL-308, pre-existing on
`master`).  Clean `make` rebuild (library + 27 test targets) and clean
Xcode `RISE-GUI` build: zero compiler warnings.

### Appearance

On the implementation's own grid (cells whose pre-fix `rho_d <= 1`,
relative change worst / mean): Schlick `(.1, *)` and `(.5, .1)` 0 / 0;
`(.5, .5)` −7.63% / −0.07% (−0.08% worst at `θ <= 60`); `(.9, .1)` −4.34% /
−0.23% (−1.27% at `θ <= 60`); `(.5, .9)` −34.1% / −0.85% (grazing);
conserving Ward 0 everywhere.  Over-authored cells drop by up to 55%
(Schlick, `(.9, .9)`) and 59% (Ward) directionally.

Shipped scenes, linear capture, `oidn_denoise FALSE`, `std::srand(seedBase
+ n)` per render, interleaved base/fix binaries built from `b89a8aa9`'s and
this slice's material sources (n = 4; whole-image mean luminance ± sample
SD):

| Scene (settings) | Base | Fix | Change | t |
|---|---:|---:|---:|---:|
| `materials` (pixelpel, 800², shipped) | 0.178868 ± 0.000033 | 0.128677 ± 0.000024 | −28.06% | −2477 |
| `sss_different_bsdf` (pixelpel, shipped) | 0.098935 ± 0.000006 | 0.093882 ± 0.000005 | −5.11% | −1209 |
| `kaleidoscope_atrium` (pixelpel, 512x384) | 0.453484 ± 0.000041 | 0.443877 ± 0.000066 | −2.12% | −248 |
| `showroom` (shipped, irradiance cache) | 0.385843 ± 0.040340 | 0.367435 ± 0.035237 | −4.77% | −0.7 |
| `showroom`, irradiance cache off (n = 3 base) | 0.360716 ± 0.000270 | 0.343584 ± 0.000451 | −4.75% | −62.5 |
| `cornellbox_bdpt_materials_pt` (PT, 256²) | 0.983532 ± 0.000151 | 0.945228 ± 0.000439 | −3.89% | −165 |
| `cornellbox_bdpt_materials` (BDPT, 256²) | 0.973399 ± 0.000504 | 0.941137 ± 0.000109 | −3.31% | −125 |
| `pt_alchemists_sanctum` (PT, 192x128) | 4.804440 ± 0.016516 | 4.789953 ± 0.017608 | −0.30% | −1.2 |
| `bdpt_alchemists_sanctum` (BDPT, 192x128, 64 spp) | 4.802400 ± 0.003052 | 4.793248 ± 0.017101 | −0.19% | −1.1 |
| `pt_jewel_vault` (PT, 128x96, 128 spp) | 9.093668 ± 0.022245 | 9.077630 ± 0.025107 | −0.18% | −1.0 |
| `bdpt_jewel_vault` (BDPT, 128x96) | 10.879074 ± 0.020105 | 10.890844 ± 0.003384 | +0.11% | +1.2 |

**Every move above 2% is an over-authored material** (white or near-white
specular over a coloured diffuse): the `materials` teapots are 50% grey
under a white `rs` (their pixel region −44%; a white-`Rs` Ward keeps no
diffuse at all, `1 - Rs = 0`), `sss_different_bsdf`'s Ward is orange
`(1, .5, 0)` under a `.7` cream (clipped to `.3`), the kaleidoscope pillars
are `(.42,.48,.58)` under `(.92,.95,.99)`, the showroom brushed metal is
`(.65,.63,.6)` under `(.9,.88,.85)` (its image blocks −33..−37%) and the ruby
`(.6,.1,.15)` under `(.95,.6,.65)` (−12%), and the Cornell boxes' Schlick
and Ward spheres are grey / copper under white (their blocks −19..−38%).
The only shipped energy-conserving Schlick/Ward materials -- the
alchemists' and jewel vault's Ward floors, copper and bronze -- are
unchanged within noise (|t| <= 1.2).  (A 2x2 NaN block appeared in 1 of 13
base `showroom` renders and 0 of 10 fix renders -- DL-324.)  The
constructed all-Schlick box (every receiver `rd .6` under white `rs`, `r
.05`, isotropy .3, DL-225's cost fixture) shows the defect directly: base PT
1120.98 ± 35.55 and BDPT 25.12 ± 0.11 (a closed box of surfaces with
`rho_d > 1` -- the Neumann series diverges and each integrator's depth caps
truncate it differently), fix PT 2.272 ± 0.059 and BDPT 2.081 ± 0.002.

### Cost

Micro-benchmarks, ns/call, 4 alternating runs of separately built base/fix
binaries (mean ± SD), view 30° / 80°:

| Config | `value` base → fix | `Pdf` base → fix | `Scatter` base → fix |
|---|---|---|---|
| Schlick `rd .4 rs .4 r .5` (never clips) | 21.3 → 29.9 / 21.2 → 30.6 | 1059 → 1059 / 979 → 1020 | 191 → 197 / 175 → 183 |
| Schlick `rd .9 rs .5 r .05 i .3` (clips) | 20.4 → 48.8 / 27.1 → 58.9 | 1320 → 1725 / 1483 → 1789 | 212 → 234 / 190 → 214 |
| Schlick `rd .6 rs 1 r .05 i .3` (over-authored) | 20.3 → 50.6 / 28.2 → 55.8 | 1338 → 1663 / 1482 → 1795 | 212 → 234 / 188 → 214 |
| Ward iso / aniso over-authored | unchanged (±1 ns) | unchanged (warm cache) | unchanged |

Renders (user CPU, n = 4-5 interleaved): all-Schlick box that never clips
(`rd .4 rs .4 r .5`): PT 28.79 ± 0.26 → 29.24 ± 0.25 s (+1.6%), BDPT 81.39 ±
1.07 → 82.66 ± 0.65 s (+1.6%), images identical within noise.  The clipping
all-Schlick box reads −5.3% PT / −5.2% BDPT, but that comparison is
confounded: the base render's paths carry runaway throughput and survive
Russian roulette longer.  **The unconfounded clipping cost (external
review):** an energy-conserving furnace sphere, Rd .9 / rho .1 / roughness
.05 -- which clips -- reads PT user CPU 7.56 ± 0.03 s → 10.72 ± 0.12 s,
**+41.7%** (n = 3 interleaved); the mixed Cornell box +0.8%.  Every Schlick
lane with `Rd > 1 − A_top` (`A_top` the table's maximum over incidence)
pays the band quadrature on every `Pdf` call.  At isotropy 1 that
threshold is:

| roughness | rho .04 | rho .1 | rho .5 | rho .9 |
|---:|---:|---:|---:|---:|
| .01 | Rd > .63 | .59 | .33 | .07 |
| .05 | .80 | .75 | .45 | .14 |
| .2 | .90 | .85 | .55 | .25 |
| .5 | .96 | .93 | .75 | .57 |
| .8 | .98 | .96 | .84 | .71 |

Mitigation recipe (not done in this slice): the band nodes
(`SchlickDiffuseDraw`) depend only on the lanes, `rho`, `Rd`, `mu_i` and the
geometric-normal frame -- not on the query direction -- so a small
thread-local memo keyed on those can share them across the several `Pdf`
calls one vertex makes (the DL-24 shared-walk precedent); verify it
bit-identical on a deterministic single-thread render.

### Residuals

* **DL-323** (new, reframed by the external review): `CookTorranceBRDF`
  adds the same uncoupled `Rd/pi` under a specular whose Fresnel RISES
  toward 1 at grazing -- the mechanism DL-310 closed for Schlick -- so a
  STANDARD plastic (Rd .9 under a white specular tint, ior 1.5) reads
  0.940 at 0°, 1.452 at 85° and 1.715 at 89° (spec .3: 1.144, spec .5:
  1.307 at 89°); `Rd + spec <= 1` is not its conservation criterion (the
  sibling audit's own `rd .9 spec .1` 0.982 / `rd .5 spec .5` 0.909 rows
  simply never reached that corner).  Normalized isotropic Phong reads
  exactly `Rd + Rs` (over-authoring only, 1.900 at `Rd .9 Rs 1`).  The
  shipped `materials` scene shows the resulting POLICY SPLIT: under
  identical grey-under-white authoring its Ward teapots lose 47-50% while
  the Cook-Torrance teapot moves −0.02%.  Ruling needed: extend the
  min clip to Cook-Torrance and Phong (the consistent choice) or revert
  Ward's half.  `AshikminShirleyAnisotropicPhongBRDF`'s coupled diffuse is
  bounded by design (max 0.996).
* **DL-324** (new): an intermittent NaN in the shipped `showroom` on the
  base build (about 1 in 33 renders); lead from the external review: Ward
  anisotropic `ComputeFactors`' unclamped `acos` of a possibly zero-length
  tangent (NaN on 46,912 of 200,000 constructed pairs), pre-existing.
* The azimuth-maximum `A` over-darkens anisotropic Schlick in the clip band
  (above); Ward's supremum `A` over-darkens over-authored rough Ward.
  Both are bounded, reciprocal choices made on cost.  "Untouched where the
  pair already conserves" holds against the TABLE's `A`, not the true one:
  on the external review's extended grid 40 of 449 never-over cells move by
  more than 1%, worst −6.6% (`Rd .3`, `rho .7`, roughness .05, isotropy .01,
  89.9°), all at `θ >= 89°` with isotropy <= .1 or roughness <= .001, where
  the table's conservative bump or the azimuth maximum over-reads `A`.
* The table's residual under-read: the generator's off-probe validation
  reads 3.2e-3 worst (roughness 2.7e-5, isotropy .006, 89.93°), and the
  external review found the same order at an ordinary roughness (.024,
  isotropy .004, 89.62°: full material 1.00298).  The bound holds to
  within 3e-3 at isotropy below .01 at grazing.

## Auxiliary albedo and consumers

Schlick's auxiliary albedo integrates the corrected directional reflectance,
then saturates to `[0,1]` as required by `IBSDF::albedo`. Sampling `p_h=tZ/pi`
analytically cancels the sharp distribution peak. The substitution `xi=u²`
with Jacobian `2u` removes its grazing endpoint singularity; a deterministic
16×32 rule integrates two Fresnel moments, reusing matching roughness/isotropy
between RGB channels. The actual geometric-horizon gate is included; diffuse
reflectance uses the exact clipped-cosine fraction `(1+n.g)/2`.

An independent 300-state sweep over `r=.02/.1/.3/.5/.8`,
`p=.1/.3/.7/1`, incidence `0/30/60/80/89°`, and azimuth `0/45/90°`
compares against 512×1024 nodes. Maximum raw error is .0187550
(`r=.02,p=.1,89°,azimuth0`, raw estimate1.7384127/reference1.7571677).
Maximum **bounded-AOV** error is .0162536
(`r=.1,p=.1,80°,azimuth0`, .6628490/.6791027). A chromatic test additionally
uses `rho=(.2,.9,.5)`, `r=(.02,.1,.8)`, `p=(.3,.1,1)` and a separate
half-angle solid-angle reference with the reflection Jacobian.

This is not an every-bounce transport operation, but it is also not necessarily
once per final pixel. PT/BDPT capture albedo for requested fast/accurate AOV
samples, `AOVBuffers` can supersample a separate guide pass, the interactive
material preview evaluates it per visible hit, and Fabric's auxiliary albedo
can delegate to its base BRDF (the scene parser currently excludes these
Schlick/Ward models from Fabric's allowed base-material set). Accuracy and cost must be judged on those
consumers. Ward retains a conservative saturated `Rd+Rs` auxiliary estimate.

## Aggregate Pdf: integrate the selection domain

Changing `kray` changes `RandomlySelect`'s realized weights. The aggregate
remains `C_D p_D + sum q_j p_j`, with the same exact query-direction
specular coefficients and clipped-cosine diffuse acceptance. An initial
32×32 sampler replay was insufficient: an independently converged state
(`Rd=.4,Rs=.5,alpha=.1`, view60° in the shading frame, view azimuth0,
geometric tilt30°) has `C_D=.720097153229`, whereas replay32 returns
`.713337220462`. Its diffuse mass discrepancy alone is
`.006759933*(1+cos30°)/2=.0063066`. The public regression now isolates that
coefficient through a direction where the specular Gaussian is below
`1e-100`; the old approximation fails its `1e-5` accuracy requirement.

`WardSelectionQuadrature.h` instead transforms the shared sampler draw to
an elliptical slope and a Gaussian radial coordinate:

```
x_j = alphaX_j cos(psi), y_j = alphaY_j sin(psi)
h_j = (s x_j, s y_j, 1) / sqrt(1+s²(x_j²+y_j²))
p(s) ds = 2s exp(-s²) ds
```

This is a measure-preserving reordering of Ward's quadrant-folded azimuth,
shared by all lanes, so their random-number correlation is preserved. At a
fixed `psi`, write `a²=x²+y²`, `sv=x vx+y vy`, `sg=x gx+y gy`, and `vg=v.g`.
The shading and geometric acceptance boundaries are quadratics in `s`:

```
cos_o numerator = nv (1-a²s²) + 2 sv s
geom_o numerator = 2(nv+sv s)(ng+sg s) - vg(1+a²s²)
```

The union of every lane's positive roots splits the radial integral into
regions with constant acceptance sets. Rejected regions integrate
analytically. Accepted regions use eight-point Gauss–Legendre quadrature;
additional fixed radial cuts at 2 and 4.5 resolve the Gaussian. A further
quadratic split where `cos_o=4 nv` resolves the grazing transition in
`2 cos_o/(nv+cos_o)` without altering that weight. The omitted
tail has total probability `exp(-4.5²)<1.61e-9`; including it with coefficient
one bounds that tail error by the same amount. A scaled, cancellation-safe
quadratic solver handles linear and repeated-root cases.

Angular cuts include each lane's geometric discriminant zeros, the two
shading/geometric horizon intersections, and view-aligned quadrants. A
`sin²` change of variable regularizes interval endpoints; each interval uses
32 Gauss–Legendre nodes. This has fixed work, no recursive convergence loop
and no fallback to the biased midpoint grid. At most 35 angular cuts and
24 radial cuts give a conservative cap of 200,192 numeric radial nodes;
all-rejected pieces cost no numeric nodes. The final 472-state suite combines the original 112-state roughness,
anisotropy, chromatic, incidence and tilt grid with 360 extreme states
through 89.99° and axes .01 through 1. It observes at most 29,016 numeric
nodes and maximum absolute error `6.9047e-6` (axes 1/.01, view 89.9°,
azimuth 0°, no tilt). The worst reference converges from `.643360592079`
at 128 angular nodes to `.643360591407` at both 256 and 512, with 32 radial
nodes. The earlier 24×8 rule missed the preserved `1e-5` target on some
untilted grazing states; the final 32×8 rule and radial transition split
resolve them. The cold
coefficient accuracy target is `1e-5`, substantially below the sampling
error in the public mass, TVD, and estimator tests. The aggregate mass error
from this coefficient is at most its absolute error, because `int p_D<=1`.

The coefficient depends on the incoming view and material state, not the
outgoing query direction. PT uses it for NEE/guiding and continuation; BDPT
also queries forward/reverse and connection endpoints. Four thread-local
memo entries reuse identical values. The complete key is the view and
geometric normal in the sampling frame, diffuse selection weight, lane
count, and all live lane axes and weights. It contains no object, material,
or intersection pointers; an edit or different wavelength changes its
numeric key. Pure cold evaluation remains available to tests. Storage is
616 bytes per thread and disappears with the thread. The cache follows
[const-correctness guidance](skills/const-correctness-over-escape-hatches.md):
it changes evaluation cost only, with no shared material mutation.

A quiet standalone benchmark of the preliminary 24-angular-node prototype
(before the additional grazing transition split) retained all three alternating-order
replicates, including first-replicate warmup variation:

| Algorithm | ns/call mean ± sample SD (n=3) |
|---|---:|
| Midpoint16 | 1027.9 ± 579.2 |
| Midpoint32 | 3286.8 ± 1088.5 |
| Boundary method, cold | 21563.9 ± 1027.3 |
| Boundary method, changing 112-state sequence | 19601.4 ± 168.9 |
| Boundary method, identical-key warm | 3.187 ± .012 |

Cold state means ranged 7993–134018 ns. The changing-state sequence contains
some identical keys (normal-view azimuth variants); it is not advertised as
100% cache misses. These are algorithm-selection microbenchmarks, not whole
renderer overhead. Cold/changing-hit production Pdf and scene timings must
remain visible alongside cache-hit results.

## Production cost (quiet, 2026-09-20)

Three independent process runs per binary, alternating base/fixed order,
retain every measurement including the first run. Each row below gives
mean ± sample SD in ns/call. The incoming angle is measured from the normal;
view azimuth is fixed with tangent components in the ratio .8/.6. Uniform
material states use `Rd=(.4,.1,.2)`, `Rs=(.2,.5,.8)`; Schlick uses
`r=.3,p=.4`, Ward isotropic `.3`, anisotropic `.3/.12`, and chromatic
axes `(.1,.3,.6)/(.3,.12,.6)`. The query direction is `normalize(.2,.3,1)`.

Warm Pdf repeats one hit 100,000 times. Changing-view Pdf cycles 97 distinct
views (angle increments of .001°) for 5,000 calls, exceeding the four-entry
cache; every timed query misses. Both binaries include the same ray update
and loop overhead, and each row has 100 untimed setup calls. These are
production material entry points linked to separate source states, not
the preliminary integration-only benchmark above.

| Model | View | Base Pdf | Fixed warm Pdf | Base changing Pdf | Fixed changing Pdf |
|---|---:|---:|---:|---:|---:|
| Schlick | 30° | 790.6 ± 27.8 | 864.1 ± 24.2 | 780.4 ± 8.1 | 849.8 ± 8.5 |
| Schlick | 80° | 734.5 ± 3.1 | 793.8 ± 2.2 | 733.4 ± 8.2 | 793.1 ± 11.1 |
| Schlick | 89° | 672.4 ± 2.1 | 732.0 ± 2.6 | 672.3 ± 3.1 | 730.5 ± 1.7 |
| WardIso | 30° | 652.3 ± 3.7 | 30.3 ± 0.1 | 653.2 ± 7.8 | 26128.0 ± 205.5 |
| WardIso | 80° | 560.4 ± 1.0 | 31.5 ± 0.2 | 563.9 ± 6.3 | 25874.5 ± 80.8 |
| WardIso | 89° | 523.5 ± 4.5 | 32.5 ± 0.2 | 528.7 ± 6.7 | 26777.2 ± 27.8 |
| WardAniso | 30° | 1124.1 ± 0.5 | 60.1 ± 0.9 | 1131.5 ± 22.3 | 22728.6 ± 137.0 |
| WardAniso | 80° | 1127.3 ± 2.2 | 54.4 ± 0.1 | 1082.3 ± 12.4 | 24986.6 ± 60.5 |
| WardAniso | 89° | 1060.9 ± 2.9 | 56.3 ± 0.4 | 1054.0 ± 17.8 | 26157.8 ± 180.1 |
| WardChromatic | 30° | 3234.9 ± 2.8 | 205.8 ± 1.0 | 3237.1 ± 11.5 | 101362.8 ± 430.9 |
| WardChromatic | 80° | 3069.6 ± 46.3 | 216.5 ± 0.9 | 3038.7 ± 51.9 | 123365.5 ± 1227.8 |
| WardChromatic | 89° | 2878.8 ± 7.6 | 213.6 ± 2.5 | 2873.9 ± 12.9 | 128643.7 ± 330.6 |

The Ward changing-hit cost is material: 20–51× the historical midpoint
approximation. Warm-cache speedups cannot stand in for this cost. Complete
render measurements below therefore include isotropic, anisotropic and
chromatic Ward workloads. BRDF `value()` costs 21–45 ns in these states,
with fixed/base ratios 1.000–1.028 (one million calls per row). Ward's
auxiliary albedo remains 5–6 ns. Schlick's albedo rises from 4.7–5.6 ns
to 645–672 ns (10,000 calls per row); its final per-angle values are
653.7 ± 89.6, 672.1 ± 71.1, and 645.0 ± 73.7 ns at 30°, 80°, and 89°.
This auxiliary cost is measured through the actual preview pipeline below.

## Ward energy and tails

The following live `value()` quadratures use the independent half-vector
solid-angle reference (400×800), `Rd=0`, `Rs=.5`; the anisotropic rows
use fixed `ay=.12`. Both stored-density identities remain exact. The earlier
outgoing-angle reference undersampled the narrow grazing peak, which is why
the replacement integrates half-vector measure instead.

| Model | ax | View | Before Q | After Q |
|---|---:|---:|---:|---:|
| iso | 0.10 | 0.0° | 0.485590 | 0.495064 |
| iso | 0.10 | 60.0° | 0.244526 | 0.487043 |
| iso | 0.10 | 80.0° | 0.091798 | 0.433772 |
| iso | 0.10 | 89.9° | 0.135082 | 0.482773 |
| iso | 0.30 | 0.0° | 0.396954 | 0.455008 |
| iso | 0.30 | 60.0° | 0.211168 | 0.409305 |
| iso | 0.30 | 80.0° | 0.122546 | 0.390239 |
| iso | 0.30 | 89.9° | 0.518433 | 0.491952 |
| iso | 0.60 | 0.0° | 0.247390 | 0.331194 |
| iso | 0.60 | 60.0° | 0.158644 | 0.333649 |
| iso | 0.60 | 80.0° | 0.131546 | 0.379488 |
| iso | 0.60 | 89.9° | 0.787672 | 0.494664 |
| aniso | 0.10 | 0.0° | 0.482540 | 0.493954 |
| aniso | 0.10 | 60.0° | 0.242989 | 0.485915 |
| aniso | 0.10 | 80.0° | 0.091230 | 0.432771 |
| aniso | 0.10 | 89.9° | 0.134381 | 0.482708 |
| aniso | 0.30 | 0.0° | 0.436039 | 0.473918 |
| aniso | 0.30 | 60.0° | 0.231630 | 0.425877 |
| aniso | 0.30 | 80.0° | 0.133107 | 0.399443 |
| aniso | 0.30 | 89.9° | 0.558155 | 0.492387 |
| aniso | 0.60 | 0.0° | 0.344232 | 0.409449 |
| aniso | 0.60 | 60.0° | 0.213134 | 0.381942 |
| aniso | 0.60 | 80.0° | 0.170162 | 0.404863 |
| aniso | 0.60 | 89.9° | 0.995792 | 0.495514 |

The same fixed-seed sampling experiment (200,000 draws per row) at 89.9° gives:

| Model | Before mean / p99.9 / max / max:mean | After mean / p99.9 / max / max:mean |
|---|---|---|
| Ward iso .3 | .5187 / 3.6183 / 3.6865 / 7.1071 | .4935 / .9981 / .9982 / 2.0226 |
| Ward aniso .3/.12 | .5569 / 3.6529 / 3.6866 / 6.6195 | .4919 / .9981 / .9983 / 2.0294 |
| GGX conductor .3 control | .2848 / .4510 / .4522 / 1.5875 | .2848 / .4510 / .4522 / 1.5875 |
| Schlick .3, p1 | 84.8723 / 2395.0084 / 26522.4006 / 312.4977 | .3646 / 6.1102 / 9.7696 / 26.7946 |

These are per-draw sum-of-lobe weights, not image quantiles. DL-177's
tail question is resolved by changing Ward's published normalization: its
accepted per-lobe weight is analytically bounded by `2 Rs`. The Schlick
change strongly reduces this tail without asserting a global conservation
result; DL-225 was CLOSED 2026-09-28 by the bounded reciprocal masking (see the DL-225 section above); the uncoupled diffuse term is DL-310.

## BDPT controls and measurement dispatch

At 32×32, 256 spp, three independent base/fixed process pairs alternate
execution order. The tabulated scalar is the average of RGB image means,
with sample SD across the three renders. All twelve L/M checks pass in
every process; the original two-percent L parity threshold is unchanged.

| Topology / integrator | Base mean ± sample SD | Fixed mean ± sample SD | Change |
|---|---:|---:|---:|
| L / PT | 0.06427794 ± 0.00000589 | 0.05825891 ± 0.00000374 | -9.364% |
| L / BDPT | 0.06426296 ± 0.00000528 | 0.05822671 ± 0.00000467 | -9.393% |
| M / PT | 0.04712938 ± 0.00000585 | 0.04713510 ± 0.00000724 | +0.012% |
| M / BDPT | 0.04720443 ± 0.00000406 | 0.04720782 ± 0.00000156 | +0.007% |

`--materials-only` calls the unchanged `TestSchlickMultiLobe()` and
`TestGGXLambertianControl()` functions and returns their accumulated status.
Its only other source change adds the option to usage text. The default
full suite, `--spectral-only 1|2`, and `--spectral-aggregate-unit` dispatches
remain intact. The final full candidate run passed 170/0.

## Rejected external fixed-selection alternative (2026-09-21)

An ABI-isolated experiment preserved Ward's emitted directions, conditional
stored densities, physical tags and `kray=f_I*cos/p_I`, but gave the external
selector direction-independent painter weights. Its public density used an
analytic inner Gaussian CDF and bounded outer quadrature. The production
source was not replaced by this experiment. The default selector's 74,016-case
digest stayed identical, 72 emission traces stayed byte-identical, and live
Ward density (404/0), HWSS (172/0), and physical-model checks (917/0) passed.

The quiet whole-render comparison retained all 66 runs: three replicates for
each scalar/chromatic PT/BDPT comparison and the added tilted-chromatic
fixtures. Relative to the original baseline, experimental frame times were
2.044–4.048× for scalar Ward, 7.674–11.900× for chromatic Ward, and
12.544–18.868× for tilted chromatic Ward. This alternative was also rejected
for cost. The isolated preselection experiment remains a faster alternative,
but deliberately changes the stored-density/null-event contract and requires
a coherent DL-67 guiding and response-provenance prerequisite; it is not
a shipping replacement by itself.

A separate bounded profile counted every cache event and sampled per-thread
CPU time on eight fixtures, once each. BDPT cache hit rates were only
4.64–5.19%. Coefficient evaluation accounted for approximately 98.94% of
chromatic Ward Pdf CPU and 99.31% in the tilted chromatic case. Traversal was
a small fraction. Four renderer threads participated; accumulated CPU
estimates are distinct from elapsed frame time. One profile run cannot
isolate causal instrumentation overhead from the previous three timing runs.

Composite's existing DL-24 density-shape defect remains explicit. Bare Ward
proofs do not establish universal wrapper/MIS/guiding compatibility, even
when an aggregate density integrates to approximately one. The candidate
Schlick correction also retains the separately documented finite DL-225
energy residual; no global energy-conservation claim is made.
