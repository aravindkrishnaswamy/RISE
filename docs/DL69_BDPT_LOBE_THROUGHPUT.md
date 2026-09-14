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
  subject and is untouched here.
* **Guiding only BLENDED the density** for the lobe's own direction
  (`bsdfCombinedPdf > NEARZERO`): the per-lobe form still applies, and
  takes PT's DL-42-fixed shape,
  `kray_I * pScat->pdf / (selectProb * combinedPdf)`.
* **A material whose `ISPF::Pdf` is the base-class 0** (`BioSpecSkinSPF`,
  `GenericHumanTissueSPF` do not override it) keeps the old per-lobe
  `pdfFwd`.  Its `pdfRev` is zero there too and `MISWeight`'s remap0
  already governs that case, so substituting a zero would be a
  behaviour change for no consistency gain.

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
* **Any material with a bump/normal map** — the old expression's `cos`
  came from `ri.geometric.vNormal` (the GEOMETRIC normal) while `p_I`
  is a density around `ri.onb.w()` (the SHADING normal).  `kray` is
  internally consistent; the old pairing was not.
* **`SchlickSPF`** — the Schlick-1994 sampling weight differs from
  `f_I cos / p_I` by `hl / (nv * t * A)` on the specular lobe; see §5.

So the fix changes BDPT/VCM/MLT renders of **every** material at every
non-delta vertex, not only multi-lobe ones — always in the direction of
agreeing with PT, which has used `kray` since it shipped, and with this
file's own delta branch.

---

## 3. Red-proof

### 3.1 Closed form — `tests/SchlickLobePairingTest.cpp`

A two-lobe overlapping-support `SchlickSPF` (grey `rd = rs = 0.4`,
roughness 0.5, isotropy 1), 200 000 draws per incidence, measured
against a 200x400 quadrature of `SchlickBRDF::value()`:

| incidence | Q (quadrature) | (a) `kray_I/q_I` | (b) `f_agg cos/P_mix` | (c) `f_agg cos/(q_I p_I)` | (c)/Q |
|---|---|---|---|---|---|
| 0 deg  | 0.66676 | 0.66628 (0.999 Q) | 0.66640 (0.999 Q) | 1.33228 | **1.998** |
| 30 deg | 0.68613 | 0.65966 (0.961 Q) | 0.68600 (1.000 Q) | 1.37162 | **1.999** |
| 60 deg | 0.80531 | 0.64585 (0.802 Q) | 0.80608 (1.001 Q) | 1.61569 | **2.006** |

(c) is exactly what BDPT computed.  It lands on **2.00 x** the true
integral at every incidence — the N-times over-count for N = 2.  Both
principled pairings land on Q.

The same test measures the `pdfFwd` half: `(q_I p_I) / ISPF::Pdf()` on
real draws spans `[0.018, 1.333]` at 0 deg, `[0.009, 1.537]` at 30 deg,
`[0.003, 1.724]` at 60 deg — over two orders of magnitude between the
two formulas MIS treats as one density.

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
| `CookTorranceSPF` | diffuse + specular + multiscatter | **no** — all three set `.pdf = mixPdf` | n/a | **IMMUNE**, for GGX's reason (`CookTorranceSPF.cpp:205/256/316` Pel, `:419/463/515` NM) |
| `GGXSPF` | diffuse + specular + multiscatter | **no** — `mixPdf` | n/a | **IMMUNE**, matches the ledger |
| `CoatedSPF` | one `AddScatteredRay` per call | n/a | n/a | **IMMUNE**, matches the ledger |
| `LambertianSPF`, `OrenNayarSPF`, `SheenSPF` | 1 | n/a | n/a | **IMMUNE** (N = 1) |
| `TranslucentSPF` | 2 (entry: front-reflect + transmit; exit: exit + interior backscatter) | yes | **no** — `TranslucentBSDF::value` is a `switch` on `GetReflectedSide`, returning exactly ONE lobe's term per direction | **not over-counting**, matches the ledger.  The fix still changes it, for §2.1's reason (its `kray` carries Beer extinction the BSDF value omits) — gated by `TranslucentIORStackTest` |
| `FabricSPF`, `WeaveSPF` | 1 non-delta (+ an optional delta) | — | delta and non-delta never overlap | **IMMUNE** to the over-count |
| `BioSpecSkinSPF`, `GenericHumanTissueSPF` | several | — | — | **not reached**: their materials' `GetBSDF()` returns 0, so `EvalBSDFAtVertex` is 0 and BDPT's eye/light walk already `break`s at such a vertex.  Pre-existing energy loss, filed **DL-126** |

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
  exact pattern per companion wavelength for every multi-lobe SPF that
  does not implement `EvaluateKrayNM` — Schlick, Ward, Phong, Ashikmin,
  Composite.  PT's hero is correct (`pS->krayNM * invSelectProb`).
  BDPT's HWSS companion path now mirrors PT's, fallback included, by
  deliberate choice.
* **`SchlickSPF`'s `kray` is not `f_I cos / p_I`.**  §3.1's (a)/Q column
  drops to 0.802 at 60 deg incidence at roughness 0.5.
  `SPFBSDFConsistencyTest` already bands this material at 15 % and
  attributes it to the Schlick approximation; the measurement here shows
  the gap grows with roughness beyond that band's roughness-0.3 basis.
  Filed **DL-127**.
