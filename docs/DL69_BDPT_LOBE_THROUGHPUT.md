# DL-69 — BDPT/VCM/MLT paired an aggregate BSDF value with a per-lobe selection pdf

Status: **CLOSED 2026-09-14** (debt-dl69 slice, branch `debt-dl69`, fix
commit `19d97505`).  Red-proofs `tests/SchlickLobePairingTest.cpp` and
topology L of `tests/BDPTStrategyBalanceTest.cpp` /
`tests/VCMStrategyBalanceTest.cpp` (commit `721e8f12`).

Related: DL-67 (the same conflation one layer up, inside PT's and
BDPT's guided RIS / one-sample candidate densities — still OPEN, and
this slice deliberately does not touch those branches), DL-74 (the
sampling-density vs MIS-partner-density split PT already carries),
DL-42 (PT's kept-BSDF one-sample `selectProb` divide, whose fixed shape
this row reuses for BDPT's blended-density branch).

---

## 1. The two defects

RISE's SPFs emit **all** their lobes into one `ScatteredRayContainer`
per `Scatter()` call, each lobe carrying its own throughput weight
(`ScatteredRay::kray` / `krayNM`, the SPF's own `f_I cos / p_I`) and its
own conditional density (`ScatteredRay::pdf`).  Exactly one lobe is then
drawn, by `ScatteredRayContainer::RandomlySelect`, with realized
probability

```
q_I = max(kray_I) / sum_J max(kray_J)          (`selectProb` in the code)
```

### 1.1 Throughput: an N-times over-count

`GenerateEyeSubpathImpl` and `GenerateLightSubpathImpl` in
`src/Library/Shaders/BDPTIntegrator.cpp` priced a non-delta draw as

```
scatterPdf           = selectProb * pScat->pdf          // per-lobe density
f                    = PathValueOps::EvalBSDFAtVertex(...)   // AGGREGATE value
localScatteringWeight = f * cos / scatterPdf
```

`f` is `IBSDF::value()`, the sum over **every** lobe; `scatterPdf` is
the density of the **one** lobe that was drawn.  Conditioned on a single
joint `Scatter()` draw `(w_1 … w_N)`, the expectation over the lobe
choice is

```
sum_I q_I * [ f_agg(w_I) cos / (q_I p_I(w_I)) ] = sum_I f_agg(w_I) cos / p_I(w_I)
```

whose expectation over the draw is `N * integral f_agg cos dw` for `N`
accepted non-delta lobes whose supports overlap.  The identity is
**exact**, and holds despite `q_I` being realization-dependent (a
function of the already-drawn `kray`s), because `q_I` cancels pointwise.

The same file's **delta** branch, two dozen lines above each site, and
PT's own ordinary initialization
(`PathTracingIntegrator.cpp`, `PTScatterKray<Tag>(*pS) * (1/selectProb)`)
already used the correct per-lobe pairing.

### 1.2 `pdfFwd` vs `pdfRev`: one density, two formulas

`pdfFwdPrev` stored `selectProb * effectivePdf` — again the realization-
dependent sampling density of the drawn lobe.  But `pdfFwd` is read only
by the MIS machinery:

* `BDPTIntegrator::MISWeight`'s two ratio walks, `ri *= pdfRev/pdfFwd`;
* VCM's dVCM/dVC recurrence, which inverts it back to solid angle
  (`bsdfDirPdfW = next.pdfFwd * distSq / cosAtGen`).

and the **reverse** side of that same chain — this generator's own
`prev.pdfRev`, and every `pdfRev` override inside `ConnectAndEvaluate` —
evaluates `PathValueOps::EvalPdfAtVertex` → the material's **aggregate,
direction-only** `ISPF::Pdf()`.  Two different functions for what
Veach eq. 10.9 treats as one density.

Sign: at an overlapping-support vertex `selectProb * p_I(w)` is
generically **smaller** than the aggregate density at the same `w` (the
other lobes' mass there is missing), so every `pdfRev/pdfFwd` ratio
through that vertex reads high, `sumWeights` inflates, and the weight of
the strategy that **generated** the vertex is **deflated**.

### 1.3 What reads `pdfFwd` on a SURFACE vertex — verified

Every `.pdfFwd` read was enumerated before changing it.  In
`BDPTIntegrator.cpp` the contribution-side divides
(`pdfLight = lightStart.pdfFwd` at the s=1 connection, `pdfLight =
lightEnd.pdfFwd` at the s=1 splat, `lightVerts[0].pdfFwd`) are all
inside `BDPTVertex::LIGHT` branches, and in `VCMIntegrator.cpp` the
`pdfLightArea = v.pdfFwd` divide sits in a `v.type == LIGHT` arm of
`SplatLightSubpathToCameraImpl` that the loop's own `i == 0 || v.type ==
LIGHT` `continue` already makes unreachable.  Light-source vertices get
their `pdfFwd` from `ls.pdfSelect * ls.pdfPosition`, not from this
generator.  So on a **surface scatter** vertex `pdfFwd` is a pure MIS
quantity and redefining it changes no contribution.

`guidingPdfDirectionIn`, set a few lines above `pdfFwdPrev` from the
same `scatterPdf`, is **not** an MIS quantity: its only consumer writes
it to OpenPGL's `PathSegment::pdfDirectionIn`, which wants the true
sampling density.  It is deliberately unchanged.

---

## 2. The fix

Both generators, RGB and NM, eye and light:

```
localScatteringWeight = KrayValue<Tag>( *pScat ) * (bssrdfReflectCompensation / selectProb);
```

— the delta branch's own line — and

```
pdfFwdPrev = selectProb * effectivePdf;
if( !pScat->isDelta ) {
    const Scalar misFwdPdf = PathValueOps::EvalPdfAtVertex<Tag>(
        vertices.back(), -currentRay.Dir(), scatDir, tag );
    if( misFwdPdf > NEARZERO ) { pdfFwdPrev = misFwdPdf; }
}
```

Three deliberate carve-outs:

* **Guiding SUBSTITUTED a direction** (`usedGuidedDirection`): `scatDir`
  is not the lobe's direction, so `kray_I` does not price it.  That
  branch keeps the aggregate `f * cos / scatterPdf`.  This is DL-67's
  subject.  **Its THROUGHPUT is untouched here; its `pdfFwd` is NOT**
  (review correction, 2026-09-14 — an earlier version of this bullet,
  and DL-67's ledger row, said the whole branch was left alone).  The
  `pdfFwdPrev` block below carries **no** `usedGuidedDirection` guard,
  so under substitution `pdfFwd` becomes the material's aggregate
  `ISPF::Pdf()` evaluated at the GUIDED direction (`scatDir`, which the
  substitution overwrote), with the old `selectProb * effectivePdf`
  surviving only as the `misFwdPdf <= NEARZERO` fallback.  That is a
  third density — neither the guided sampling density nor a mixture
  including the guide — and DL-67's reconciliation has to account for
  it.
* **Guiding only BLENDED the density** for the lobe's own direction
  (`bsdfCombinedPdf > NEARZERO`): the per-lobe form still applies, and
  takes PT's DL-42-fixed shape,
  `kray_I * pScat->pdf / (selectProb * combinedPdf)`.
* **`misFwdPdf <= NEARZERO`** keeps the old per-lobe `pdfFwd`: the
  aggregate density carries no information there, its `pdfRev` is zero
  at the same vertex, and `MISWeight`'s remap0 already governs that
  case, so substituting a zero would be a behaviour change for no
  consistency gain.  **Which materials actually reach it** (review,
  2026-09-14 -- this bullet used to name `BioSpecSkinSPF` /
  `GenericHumanTissueSPF`, whose `ISPF::Pdf` IS the base-class 0 but
  which BDPT never reaches: their materials' `GetBSDF()` returns null,
  so the walk already `break`s at the `PositiveMagnitude(f) <= 0` gate
  one block earlier -- that is **DL-126**, not this fallback): the
  reachable case is a material whose `Pdf` is real but does not cover
  the lobe that was drawn.  `TranslucentSPF` is the shipped instance --
  its `Pdf`/`PdfNM` deliberately do not cover either Phong `cos^N` lobe
  (the entering transmission and the interior backscatter), which is
  **DL-41** -- so a draw from one of those lobes lands here.

The `PositiveMagnitude<Tag>( f ) <= 0 → break` guard is **retained** and
re-commented as a path-termination gate only.  DL-69 removed `f` from
the throughput, but killing the walk where the aggregate BSDF is zero is
pre-existing behaviour this row does not change.

**HWSS companions** follow the same rule per wavelength, through
`ISPF::EvaluateKrayNM( ri, pScat->ray.Dir(), pScat->type, lambda_w,
iorStack )` — the same call PT's HWSS body makes — with the same
aggregate `fw * cos / scatterPdf` fallback when the SPF declines
(returns -1, the base-class default).  That fallback still carries this
row's pattern; it is tracked as **DL-125**, and BDPT deliberately
mirrors PT here rather than diverging from it.

### 2.1 A second, wider consequence — read before reviewing a render diff

`kray_I` is the SPF's own transport weight and is **not** in general
equal to `f_I cos / p_I` as the paired BSDF evaluates it:

* **`TranslucentSPF`** — `kray` carries Beer extinction and the layer
  transmittance, which `TranslucentBSDF::value` does not model at all.
* **`SchlickSPF`** — the Schlick-1994 sampling weight differs from
  `f_I cos / p_I` by `hl / (nv * t * A)` on the specular lobe; see §5.

So the fix changes BDPT/VCM/MLT renders of **every** material at every
non-delta vertex, not only multi-lobe ones — always in the direction of
agreeing with PT, which has used `kray` since it shipped, and with this
file's own delta branch.

**STRUCK (review, 2026-09-14) — a third bullet that was WRONG.**  This
list used to claim a normal-mapped material diverges because the old
expression's `cos` "came from `ri.geometric.vNormal` (the GEOMETRIC
normal) while `p_I` is a density around `ri.onb.w()` (the SHADING
normal)".  `RayIntersectionGeometric::vNormal` **is** the shading
normal (its own doc comment says so: "SHADING normal — Phong-
interpolated on triangle meshes, perturbed by the normal-perturbing
modifiers"; the geometric one is `vGeomNormal`), and `Object::
IntersectRay` builds `ri.onb` **from** `vNormal`, so the two were the
same frame and there was no mismatch to name.  The other two bullets
stand, and DL-127 is the measured instance of the third.

---

## 3. Red-proof

### 3.1 Closed form — `tests/SchlickLobePairingTest.cpp`

A two-lobe overlapping-support `SchlickSPF` (grey `rd = rs = 0.4`,
roughness 0.5, isotropy 1), 200 000 draws per incidence, measured
against a 200x400 quadrature of `SchlickBRDF::value()`:

| incidence | Q (quadrature) | (a) `kray_I/q_I` | (b) `f_agg cos/P_mix` | (c) `f_agg cos/(q_I p_I)` | (c)/Q |
|---|---|---|---|---|---|
| 0 deg  | 0.66676 | 0.66677 (1.000 Q) | 0.67377 (1.011 Q) | 1.33283 | **1.999** |
| 30 deg | 0.68613 | 0.65966 (0.961 Q) | 0.68423 (0.997 Q) | 1.37222 | **2.000** |
| 60 deg | 0.80531 | 0.64605 (0.802 Q) | 0.80462 (0.999 Q) | 1.61393 | **2.004** |

(c) is exactly what BDPT computed.  It lands on **2.00 x** the true
integral at every incidence — the N-times over-count for N = 2.  Both
principled pairings land on Q.

**RE-MEASURED 2026-09-14 (review P1), against the merge target.**  The
numbers above are the post-merge ones.  Estimator (b) had to be fixed
first: it drew its lobe with a FIXED 50/50 coin over the realized
container, which is unbiased only while `ISPF::Pdf` IS that fixed
mixture.  DL-67 Slice 0 (merged from master) replaced `SchlickSPF::Pdf`
with the TRUE generating density of `Scatter` + `RandomlySelect` --
kray-weighted, direction-dependent, and inclusive of the specular
sampler's rejection rate -- so the fixed coin stopped matching it and
(b) read `0.842 / 0.821 / 0.793 Q` (`Passed: 21  Failed: 3`).  (b) now
draws with the REAL selection rule, which IS the procedure `P_mix` is
*intended* to be the density of; **(b) is not exact against the real
`SchlickSPF::Pdf`, only against an idealized one** -- for any `g`,
`E[g(w)/P_mix(w)] = integral over supp(P_mix) of g` holds exactly only
if `P_mix` IS the true marginal density of `Scatter`+`RandomlySelect`,
and `SchlickSPF::Pdf`'s `C_D` coefficient is itself a 16x16 stratified
quadrature (`kSpecQuadN`, `SchlickSPF.cpp`) with its own small,
documented residual (docs/DL67_SLICE0_SCHLICK_PDF_WEIGHTS.md SS4a/SS4c).
**Review P2-1 (2026-09-17) re-measured this with 24 independent
replicates of 50000 draws each** (`tests/SchlickLobePairingTest.cpp`'s
`ReplicateAggregateRatio`) to separate that systematic residual from a
single call's own MC noise: 0 deg mean 1.010486 +/- 0.000027 (sem),
z = (mean-1)/sem = +389; 30 deg mean 0.997092 +/- 0.000114, z = -25;
60 deg mean 0.999922 +/- 0.000186, z = -0.4 (consistent with zero).  The
0 deg and 30 deg residuals are overwhelmingly systematic, not noise --
and the largest of them (~1.1%) is the same order as
`SchlickSPFPdfConsistencyTest`'s own residual for the same `Pdf()`
implementation.  (That file's mass gate is `kMassTol=0.01`, widened to
0.015 on two low-roughness rows; its largest LIVE `|int Pdf - emitted|`
today is 0.01216, on its DL-101 KNOWN-FAILURE row.  The `0.0062` figure
this paragraph used to cite as that bound is a TOTAL VARIATION reading
on that file's th=30 CONTROL row from an earlier, smaller version of the
file -- a different metric, not the mass-gate `|int Pdf - emitted|` this
paragraph was using it to bound.)  `SchlickLobePairingTest`'s (b)
tolerance is set from the measured ~1.1% residual with headroom, not
loosened to a blanket "MC noise" allowance.

The same test measures the `pdfFwd` half: `(q_I p_I) / ISPF::Pdf()` on
real draws spans `[0.018, 1.104]` at 0 deg, `[0.006, 1.205]` at 30 deg,
`[0.004, 1.281]` at 60 deg — between two and almost three orders of
magnitude between the two formulas MIS treats as one density.  (Those
spreads also moved with the merge: against the pre-Slice-0 `Pdf` they
read `[0.018, 1.333]` / `[0.009, 1.537]` / `[0.003, 1.724]`.)

This test is a closed-form **characterisation** of the material's
arithmetic, not a red-to-green regression; it passes on both sides of
the fix (24 checks, 0 failures).  The rendered topology below is the
red-to-green one.

### 3.2 Rendered — topology L

A `schlick_material` receiver wall plus floor under a large area
emitter, depth-matched PT reference, 32x32 at 256 spp.  This is the
first non-`lambertian_material` topology in either balance test; the
existing eleven (BDPT) / nine (VCM) topologies all have N = 1 and were
structurally blind to this row.

| | PT mean | BDPT mean | rel | VCM mean | rel |
|---|---|---|---|---|---|
| pre-fix  | 0.0647231 | 0.077259 | **+19.37 %** | 0.0858983 | **+32.74 %** |
| post-fix | 0.0647129 | 0.0634207 | -2.00 % | 0.0633491 | -2.12 % |

Pre-fix p99 was +63.0 % (BDPT) / +76.8 % (VCM) and pixel max +981 % /
+469 %; post-fix all three metrics pass the suite's 8 % / 25 % / 100 %
bands.

### 3.2a Re-measured on the merge target, and the control (review P2-1)

After merging master `e290fc64` — which brings DL-67 Slice 0's
`SchlickSPF::Pdf` rewrite — topology L reads:

| build | PT mean | BDPT mean | rel |
|---|---|---|---|
| merge target                                | 0.0601796 | 0.0632837 | **+5.16 %** |
| same, `SchlickSPF.cpp` reverted to pre-Slice-0 | 0.0647011 | 0.0634197 | -1.98 % |

The final gate run reads 0.0601765 / 0.0632883 = +5.17 %, and
`VCMStrategyBalanceTest`'s twin of the same topology reads PT 0.0601639
/ VCM 0.0632532 = **+5.14 %** — VCM shares BDPT's two generators, so it
tracks it, as it did pre-fix.

**The swing is PT's.**  Across that one-file A/B, PT moved **-6.99 %**
and BDPT **-0.21 %**.  `SchlickSPF::Pdf` enters PT only through its MIS
partner arms, and Slice 0 did not touch `kray` (DL-127), so the mover is
**DL-103**: PT's un-guided escape-side MIS partner was still the
SELECTED lobe's own density while its NEE side now evaluated the true
aggregate, so `w_bsdf + w_nee != 1` at a multi-lobe SPF.  (Confirmed and
CLOSED 2026-09-17 — see the closure block below.)  BDPT is insensitive
because DL-69 made its `pdfFwd` and `pdfRev` the same function, so a
change to that function largely cancels in the ratio chain.

**Topology M** (`BDPTStrategyBalanceTest`) is the discriminator —
identical geometry, emitter, camera and both rasterizer strings, with a
`ggx_material` wall and a `lambertian_material` floor, both immune to
DL-103 and DL-127:

| build | PT mean | BDPT mean | rel |
|---|---|---|---|
| merge target                                | 0.0471226 | 0.0472101 | +0.19 % |
| same, `SchlickSPF.cpp` reverted to pre-Slice-0 | 0.0471251 | 0.0472100 | +0.18 % |

~0 %, and unmoved by the revert.  So topology L's residual is
Schlick-specific, not a property of this geometry, and the suite's
standard 8 % band is kept (2.8 pp of margin).

**Re-quoted 2026-09-17 (round-2 review P2-3/P2-4) against the TRUE
merge target.**  Both rows above predate this branch's merge of master
`e5a3750b`, which brings **DL-123** (`GGXBRDF::hemisphericalAlbedo`) —
and topology M's wall is `ggx_material`, the exact consumer DL-123
changed.  Re-measured on this branch's actual HEAD: PT 0.0471280 /
BDPT 0.0472026 (channel-averaged over the render's 3 output channels)
= **+0.16 %** — unchanged within MC noise of the pre-DL-123 +0.19 %, so
DL-123 does not move this control's conclusion.

**Topology L's band was PROVISIONAL pending DL-103; that row CLOSED
2026-09-17 and the gap closed with it**
([DL103_PT_ESCAPE_MIS_PARTNER.md](DL103_PT_ESCAPE_MIS_PARTNER.md)).
The prediction this section made — that the +5.16 % residual was PT's,
and that fixing PT's un-guided escape-side MIS partner would move PT and
not BDPT — is discharged.  Isolated A/B on
`src/Library/Shaders/PathTracingIntegrator.cpp` alone (n = 4 per side,
this same topology, same rasterizer strings):

| build | PT mean | BDPT mean | rel |
|---|---|---|---|
| pre-DL-103 | 0.0601656 ± 0.0000054 | 0.0632933 ± 0.0000024 | **+5.198 % ± 0.011 pp** |
| post-DL-103 | 0.0632676 ± 0.0000110 | 0.0632930 ± 0.0000044 | **+0.040 % ± 0.012 pp** |

and the VCM twin +5.119 % ± 0.020 pp → **−0.026 % ± 0.011 pp**.  PT
moved **+5.16 %**; BDPT moved −0.0005 % and VCM −0.002 %.  Topology M
(the immune control) is unmoved at +0.18 %.

Topology L's **mean** band is therefore tightened from the shared 8 % to
**2 %** in both suites and the PROVISIONAL marker is removed.  What a
pass there claims is "PT and BDPT/VCM agree to 2 % on a multi-lobe
material" — still **not** that either is correct in absolute terms,
because **DL-127** is open and both integrators consume `kray`, so a
residual from that row sits inside the agreement rather than showing up
as a gap.  There is still no closed form for topology L's full
multi-bounce scene; the closure evidence is the one-file A/B above plus
the two closed-form furnaces DL-103's own red-proof added.

### 3.2b Cost (review P2-5)

The `pdfFwd` addition makes both generators evaluate `ISPF::Pdf` one
extra time per non-delta vertex per subpath.  All-`schlick_material`
topology-L geometry at 256x256 / 256 spp, user CPU seconds, interleaved
A/B blocks:

| build | n | user CPU | vs pre-DL-69 |
|---|---|---|---|
| pre-DL-69 (`BDPTIntegrator.cpp` at `2a64b000`) | 3  | 136.53 +/- 3.47 | — |
| DL-69, one rebuild per query                   | 16 | 163.22 +/- 2.40 | +19.55 % |
| DL-69, shared `VertexPdfContext`               | 8  | 156.61 +/- 2.19 | **+14.71 %** |

Sharing is worth -4.05 % (Welch t = -6.39).  The residual is the second
`ISPF::Pdf` call itself, not the record rebuild: on `SchlickSPF` that
call costs ~690 ns since Slice 0 replaced the 17 ns closed form with the
true generating density of `Scatter` + `RandomlySelect`.  On a material
with an ordinary closed-form `Pdf` the residual is correspondingly
smaller.

### 3.2c Spectral / HWSS companions (review P2-6)

`SchlickSPF` does not override `ISPF::EvaluateKrayNM`, so §2's companion
fallback is taken on **every** companion wavelength at every non-delta
vertex of topology L — the `compScale < 0` branch is reachable 100 % of
the time there, which is what DL-125 records.
`BDPTStrategyBalanceTest::TestSpectralHWSSCompanionLadder` renders the
topology under `bdpt_spectral_rasterizer` twice, `hwss FALSE` (hero
only, DL-69-fixed on every bounce) against `hwss TRUE`:

| | channel means | achromatic mean |
|---|---|---|
| `hwss FALSE`, 1024 spp | 0.0655964 / 0.0626007 / 0.0640997 | 0.064099 |
| `hwss TRUE`, 256 spp   | 0.0634371 / 0.0632993 / 0.0631986 | 0.0633116 |
| ratio | — | **0.987717 (-1.23 %)** |

Three independent runs of this probe read **-1.24 %** (an earlier
configuration with BOTH sides at 256 spp), **-1.23 %** and **-1.09 %**
(the 1024/256 pair above and the final gate run).  All three are
negative and of the same magnitude across a 4x change in the hero-only
sample count, so the ~-1.2 % is systematic; the ~0.15 pp spread between
runs is the probe's own run-to-run noise, and is why the gate band is
5 % rather than something tight.  Two read-outs.  **No
over-count of DL-69's magnitude reaches the image through the
companions** — 2.00x on the hero path vs 1.2 % here — and the sign is
the opposite of a naive over-count, which points at RISE's separately
documented, pre-existing HWSS spectral-bundle bias as the larger term at
this scale.  And the **achromatic** mean is the statistic: a hero-only
render draws one wavelength per path and leaves several percent of
purely chromatic MC noise on each individual channel of what is a grey
scene under a white emitter, so a per-channel ratio would be reading
noise (they are printed, not gated).

### 3.3 Per-(s,t) strategy balance

Measured with temporary in-tree instrumentation (accumulating each
`ConnectionResult`'s `misWeight` and `misWeight * max-channel
contribution` per `(s,t)`), since removed.  Same scene and settings:

| s | t | mean MIS weight pre -> post | weighted contribution pre -> post |
|---|---|---|---|
| 0 | 3 | 0.3161 -> 0.4144 (**+31.1 %**) | 6852.13 -> 6159.47 (-10.1 %) |
| 0 | 4 | 0.2262 -> 0.3325 (**+47.0 %**) |  853.96 ->  407.07 (-52.3 %) |
| 0 | 5 | 0.2636 -> 0.3477 (**+31.9 %**) |  428.75 ->   84.67 (-80.3 %) |
| 0 | 6 | 0.1762 -> 0.3165 (**+79.7 %**) |   88.42 ->    8.67 (-90.2 %) |
| 1 | 2 | 0.7655 -> 0.7655 (-0.0 %)       | 8812.82 -> 8814.89 (+0.0 %) |
| 1 | 3 | 0.8187 -> 0.8294 (+1.3 %)       | 1643.54 ->  848.79 (-48.4 %) |
| 1 | 4 | 0.7126 -> 0.7499 (+5.2 %)       |  721.81 ->  182.84 (-74.7 %) |
| 1 | 5 | 0.7648 -> 0.8140 (+6.4 %)       |  243.71 ->   25.31 (-89.6 %) |

Totals: s=0 0.7912x, s=1 0.8470x, s>=2 0.5799x, all strategies 0.8218x.

Two things to read out of that table.

* **The s=0 rows are the deflation.**  s=0 is the BSDF-sampled
  emitter-hit strategy — the one whose vertex chain is generated by
  exactly the scattering whose `pdfFwd` was reading low.  Its mean MIS
  weight rises 31-80 %.
* **Row (1,2) is the internal control.**  Its eye endpoint is the FIRST
  surface vertex, so no Schlick scatter throughput and no Schlick
  `pdfFwd` enters it.  Weight and contribution are unchanged to four
  decimals, which is what says the movement elsewhere is the intended
  one and not a global rescale.

---

## 4. Sibling audit (docs/skills/audit-by-bug-pattern.md)

**Bug pattern in one sentence:** an estimator pairs a quantity summed
over all of a sampler's lobes with a density describing only one of
them.

The fix is at the **integrator**, so it corrects every affected material
at once; the table records which materials were actually carrying the
defect, because that is what a future reviewer needs.

| SPF | non-delta lobes per call | `.pdf` per lobe? | overlapping support? | verdict |
|---|---|---|---|---|
| `SchlickSPF` | 2, or **4** on the per-channel branch (1 diffuse + 3 per-channel specular) | yes | yes | **AFFECTED**, measured 2.00x; matches the ledger's "up to 4x" |
| `PolishedSPF` | glossy coat (1, or 3 on the per-channel dispersive branch) + diffuse substrate | yes | **yes** whenever the coat is non-delta (`scattering < 1e6` Phong / `g < 1` HG) and `MinValue(Rs) < 1` | **AFFECTED** — **refutes the ledger row's blanket "PolishedSPF's delta coat + non-delta substrate, where only one of the pair is ever non-delta"**; that holds only for the perfect-mirror-coat sub-case |
| `WardIsotropicGaussianSPF`, `WardAnisotropicEllipticalGaussianSPF` | diffuse + specular | yes | yes | **AFFECTED** (not previously enumerated by the row) |
| `IsotropicPhongSPF` | diffuse + specular (1 or 3 per-channel) | yes | yes | **AFFECTED** (not previously enumerated) |
| `AshikminShirleyAnisotropicPhongSPF` | diffuse + specular | yes | yes | **AFFECTED** (not previously enumerated) |
| `CompositeSPF` | forwards its sub-SPFs' lobes from the top and/or bottom layer | yes (the sub-SPF's) | possible | **AFFECTED** in principle; fixed by the same integrator change |
| `CookTorranceSPF` | **1** — `Scatter`/`ScatterNM` select the lobe INTERNALLY (`if( uLobe < pDiffuseSelect ) … else if … else`) and emit exactly one `ScatteredRay` | n/a — the one ray's `.pdf` is `mixPdf`, the aggregate density | n/a | **IMMUNE**, for GGX's reason |
| `GGXSPF` | **1** — same internal selection (`GGXSPF.cpp` `uLobe` branch; one of `:271`/`:370`/`:467` Pel, `:577`/`:672`/`:760` NM fires per call) | n/a — `.pdf = mixPdf` | n/a | **IMMUNE**, matches the ledger |
| `CoatedSPF` | one `AddScatteredRay` per call | n/a | n/a | **IMMUNE**, matches the ledger |
| `LambertianSPF`, `OrenNayarSPF`, `SheenSPF` | 1 | n/a | n/a | **IMMUNE** (N = 1) |
| `TranslucentSPF` | 2 (entry: front-reflect + transmit; exit: exit + interior backscatter) | yes | **no** — `TranslucentBSDF::value` is a `switch` on `GetReflectedSide`, returning exactly ONE lobe's term per direction | **not over-counting**, matches the ledger.  The fix still changes it, for §2.1's reason (its `kray` carries Beer extinction the BSDF value omits) — gated by `TranslucentIORStackTest` |
| `FabricSPF`, `WeaveSPF` | 1 non-delta (+ an optional delta) | — | delta and non-delta never overlap | **IMMUNE** to the over-count |
| `BioSpecSkinSPF`, `GenericHumanTissueSPF` | several | — | — | **not reached**: their materials' `GetBSDF()` returns 0, so `EvalBSDFAtVertex` is 0 and BDPT's eye/light walk already `break`s at such a vertex.  Pre-existing energy loss, filed **DL-126** |

**Why those two are immune, precisely** (this table used to say only
"all lobes carry `mixPdf`", which mis-describes the mechanism -- it
implies several rays reach the container).  BOTH halves are
load-bearing and neither alone suffices:

1. **Internal single-lobe selection.**  `Scatter` draws `uLobe` itself
   and emits exactly ONE `ScatteredRay`, so `RandomlySelect` has one
   candidate and the integrator's `selectProb` is identically 1.  The
   `sum_I` that produces DL-69's factor N has one term.
2. **That ray's `.pdf` is the AGGREGATE mixture density**
   (`mixPdf = (wd*diffPdf + wms*msPdf + ws*specPdf)/total`), not the
   selected sub-lobe's own conditional.  Single-emit with a per-lobe
   `.pdf` would still be mispaired -- not by a factor N, but by the
   mixture ratio.

Together they make `f_agg cos / (selectProb * pScat->pdf)` the correct
aggregate-over-aggregate pairing by construction, which is why the
pre-fix integrator was already right on these two.  `CoatedSPF` has
property 1 only; it is immune because its single lobe's `.pdf` is that
lobe's true density and its `kray` is the matching weight.

### 4.1 `SchlickSPF`'s per-channel branch (3 specular rays, one `ptrand`)

`SchlickSPF::Scatter`'s anisotropic/per-channel branch generates three
specular rays from a **shared** `Point2 ptrand`, each with a
channel-isolated `kray` and its own roughness/isotropy, plus the diffuse
ray — four overlapping non-delta lobes in one container.

The per-lobe pairing handles it, for two separate reasons.

* `selectProb` is computed over the **realized** container
  (`for i in [0, scattered.Count())`), so `q_I` is a proper PMF over
  exactly the lobes that were emitted, however many that is.  Nothing in
  the estimator assumes a fixed N.
* The shared `ptrand` correlates the three specular directions but does
  not bias anything: `E[sum_I kray_I(w_I)] = sum_I integral p_I kray_I`
  needs each `w_I` to follow `p_I` **marginally**, which it does; the
  joint law between lobes never enters.

By contrast the old pairing priced `f_agg` — which on that branch sums
all three channels' specular terms plus the diffuse — once per lobe, up
to 4x.

---

## 5. Two residuals this row did NOT close

* **DL-125** — PT's HWSS companion fallback
  (`PathTracingIntegrator.cpp`, `compWeight = pBRDFCur->valueNM(...) *
  cos / pS->pdf` when `ISPF::EvaluateKrayNM` returns -1) carries this
  exact pattern per companion wavelength.  PT's hero is correct
  (`pS->krayNM * invSelectProb`); BDPT's HWSS companion path now mirrors
  PT's, fallback included, by deliberate choice.  Scope is narrower than
  "every SPF that declines": exactly two classes override
  `EvaluateKrayNM` in the tree (`PolishedSPF`, `HairBSDF`), and
  `CoatedSPF` / `FabricSPF` / `WeaveSPF` each decline **on purpose** —
  `CoatedSPF.cpp`'s own block explains that the fallback divides by
  `pS->pdf`, "the true hero mixture pdf that actually drew it", and is
  therefore EXACT for an SPF that stores a MIXTURE density on every
  ray.  The defect is confined to the SPFs that store a PER-LOBE
  conditional density and do not override: `SchlickSPF`, both Wards,
  `IsotropicPhongSPF`, `AshikminShirleyAnisotropicPhongSPF`,
  `CompositeSPF` — §4's affected set minus `PolishedSPF`.
* **`SchlickSPF`'s `kray` is not `f_I cos / p_I`.**  §3.1's (a)/Q column
  drops to 0.802 at 60 deg incidence at roughness 0.5.
  `SPFBSDFConsistencyTest` already bands this material at 15 % and
  attributes it to the Schlick approximation; the measurement here shows
  the gap grows with roughness beyond that band's roughness-0.3 basis.
  Filed **DL-127**.  It is also one of the two rows §3.2a had to rule
  out before attributing topology L's merge-target residual to DL-103.
* **§4.1's "up to 4x" rests on a premise `DL-101` disputes.**  That
  section counts the per-channel branch's three specular rays as three
  distinct lobes in the container.  **DL-101** records that the
  per-channel loop REUSES one `ScatteredRay` across its three lanes, so
  a lane whose sampler declines to write a direction leaves the previous
  lane's — i.e. what actually reaches the container on that branch may
  be fewer than three distinct directions.  The per-lobe estimator does
  not care (it is a proper PMF over whatever was emitted, §4.1's first
  bullet), but the "4x" upper bound does: read it as a bound on the
  *intended* branch, and re-derive it when DL-101 closes.
