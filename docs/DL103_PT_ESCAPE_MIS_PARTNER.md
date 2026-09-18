# DL-103 — un-guided PT's escape-side MIS partner at a multi-lobe SPF

**Status: CLOSED 2026-09-17** (slice `debt-dl103`, branched from `master`
`aa64c45e`).  Fix `86f4c623`, red-proof `0535bfb6`, band tightening
`b5ddb691`.

Related rows: **DL-74** (the two-fields design this uses), **DL-69** (the
BDPT/VCM half of the same "aggregate vs per-lobe density" family, and the
row whose own topology-L residual this closes), **DL-67 Slice 0** (which
made `SchlickSPF`'s aggregate `Pdf()` correct and, by doing so, made this
row measurable at render scale), **DL-41** (the SPFs whose `Pdf()` does
not cover their own lobes), **DL-127** (still open, and not what this row
is about).

---

## 1. The defect in one paragraph

`LightSampler`'s four next-event arms weight their samples against
`pMaterial->Pdf(wo, ri, iorStack)` / `PdfNM(...)` — the material's
**aggregate**, all-lobes density, which is what `IMaterial::Pdf` forwards
to `ISPF::Pdf`.  `PathTracingIntegrator.cpp` PART 3 stored, as the MIS
partner the *other* side of that pair reads back
(`RAY_STATE::bsdfMisPdf`, consumed by the env-escape and emitter-hit
weights), `effectiveBsdfPdf` — which, with guiding inactive, is
`pS->isDelta ? 0 : pS->pdf`, the **selected lobe's own** density.  At a
single-lobe SPF those two are the same number, which is why every
pre-existing furnace in the tree (all Lambertian) was blind to it.  At a
multi-lobe SPF they are different functions of direction, so
`w_bsdf(ω) + w_nee(ω) != 1` and the estimator reads off its own closed
form — in ordinary, un-guided, default-configuration path tracing.

The guided branch was already correct: `PTGuidingMisPdf::Eval` blends the
**aggregate** pdf with the guide density, which is what DL-74 designed
it to do.  Only the `else` — the one every shipped render takes — was
wrong.

## 2. Why it went unnoticed until now

Three things had to line up.

1. **DL-74 (2026-09-14) split the two roles into two fields.**  Before
   that there was only `bsdfPdf`, and the question "which density is the
   MIS partner" could not even be asked separately from "which density
   did we divide the throughput by".
2. **DL-67 Slice 0 (2026-09-14) rewrote `SchlickSPF::Pdf`** so the
   aggregate was finally *correct*.  That made the two sides diverge
   further, not less, and it moved PT's own mean on a real scene by 7 %
   — which is how the row was found (the DL-69 round-2 review's isolated
   `SchlickSPF.cpp`-only A/B).
3. **DL-69 (2026-09-14) fixed the same family on the BDPT/VCM side**, so
   the remaining topology-L gap could no longer be blamed on the
   bidirectional integrators.

## 3. The fix

`PathTracingIntegrator.cpp`, both PART 3 sites (the RGB/NM templated
body and the HWSS body):

```
misBsdfPdf = 0                                    if pS->isDelta
           = guidingMis.Eval(dir, p_aggregate)    if guiding is active
           = p_aggregate                          otherwise  (DL-103)
           = pS->pdf                              otherwise, if p_aggregate == 0
```

where `p_aggregate = ISPF::Pdf(ri.geometric, traceRay.Dir(), iorStack)`
(`PdfNM(..., heroNM, ...)` in the HWSS body) — the same function, the
same direction, the same **live** IOR stack (DL-74 P2), the same
`ri.geometric` and therefore the same `glossyFilterWidth`, that
`LightSampler`'s arms evaluate.

`bsdfPdf` is **not** changed.  It stays the true density the direction
was drawn from: the throughput denominator, and the divisor in
`OptimalMISAccumulator`'s `f²/pdf²` moment.  Two roles, two fields — that
is DL-74's design and this row is an application of it, not an extension.

### 3.1 The one guard, and why it is not cosmetic

The last line above is a DL-41 guard, and it applies **only** in the
guiding-inactive branch.

A few SPFs emit a non-delta lobe their own `Pdf()` does not cover —
`TranslucentSPF`'s two Phong `cos^N` lobes are the documented case
(DL-41).  At such a vertex the aggregate reads 0 at a direction the BSDF
technique really did generate.  Handing 0 to both sides means "no
BSDF-side partner exists" on **both**: `w_bsdf = 1` (the escape arm's
`if (bsdfMisPdf > 0)` never fires, so the weight stays 1) and
`w_nee = p_nee²/(p_nee² + 0) = 1`.  That is a full double count —
strictly worse than the pre-DL-103 asymmetry, which at least had
`w_bsdf < 1` there.  Falling back to the lobe's own density reproduces
the pre-DL-103 behaviour exactly at those SPFs and nowhere else.

BDPT's own DL-69 fix applies the same rule at the same place
(`if (misFwdPdf > NEARZERO) pdfFwdPrev = misFwdPdf;`), for the same
reason, so the two integrators do not drift apart on it.  The thresholds
are not literally identical — PT tests `aggregatePdf > 0` and BDPT
`> NEARZERO` (1e-12).  That can only matter for an aggregate density in
`(0, 1e-12]`, where PT keeps a vanishing partner (weight ≈ 0 — the
sample is effectively handed to NEE) and BDPT falls back to the lobe's
own.  Neither is wrong; PT's `> 0` matches the `> 0` gates its own two
consumer sites already apply to `bsdfMisPdf`.

Under **active** guiding the blend runs unconditionally instead — the
mixture's guide term reaches directions the material's own pdf does not,
which is DL-74 round 4's own ruling, and is unchanged here.

## 4. Red-proof

`tests/PTGuidingMISPartitionTest.cpp` gained two closed-form furnace rows
and their controls, plus a premise on production code.  The file's
`#ifdef RISE_ENABLE_OPENPGL` was moved down past the shared harness so
these rows run in **every** build configuration — they need no guiding
field.

### 4.1 The material

Two overlapping, non-delta, cosine-power lobes about the shading normal:

    p_I(ω) = (n_I + 1) cosⁿᴵ(θ) / (2π)        (integrates to 1)
    f_I(ω) = c_I · p_I(ω) / cos(θ)            (so kray_I = c_I, constant)

with `n_A = 1` (a plain cosine lobe), `n_B = 63` (a narrow one),
`c_A = 0.3`, `c_B = 0.7`.  Three properties make it a closed form rather
than a comparison against another integrator:

1. `kray_I = c_I` is **constant**, so `RandomlySelect`'s kray-weighted
   choice is exactly `w_I = c_I`, and the aggregate is the fixed mixture
   `Pdf(ω) = Σ c_I p_I(ω)`.  A direction-dependent kray would make the
   aggregate ill-defined.
2. `f(ω)·cos(θ) = Σ c_I p_I(ω) = Pdf(ω)` identically, so the BRDF is
   `Pdf(ω)/cos(θ)` and its bihemispherical albedo is `Σ c_I = 1` exactly.
   The env-furnace target is therefore `L_env`.
3. Over a cone of half-angle `θ_max` the same algebra integrates to
   `Σ c_I (1 − cos^(n_I+1)(θ_max))`, which reduces to the Lambertian
   `R²/d²` when the only lobe is the cosine one.

### 4.2 The control

`MultiLobeMaterial(true)` emits **one** ray drawn from that same mixture,
with `pdf = Pdf(ω)` and `kray = 1` (exact, by property 2).  Identical
BRDF, identical marginal sampling density, identical closed form — the
only variable is whether the lobes reach the integrator as one
`ScatteredRay` or two.  It is green before and after, which is what
proves the two-lobe rows measure the partner mismatch and not the
harness.

### 4.3 The premise on production code

Deterministic (fixed seed), no render: draw 4096 `Scatter` calls from a
real `SchlickSPF` and a real `IsotropicPhongSPF` at an oblique incidence
and compare each emitted non-delta lobe's own `.pdf` against the SPF's
aggregate `Pdf()` at that same direction.

    SchlickSPF:        max lobes/Scatter 2, aggregate/selected-lobe pdf ratio
                       over 7288 non-delta draws in [0.505783, 10.8666]
    IsotropicPhongSPF: max lobes/Scatter 2, aggregate/selected-lobe pdf ratio
                       over 8192 non-delta draws in [0.5, 38.686]

So the synthetic material is a controlled stand-in for measured
behaviour, not a straw man.

### 4.4 Counters

Unfixed library (`aa64c45e`, this slice's base):

    (j) two-lobe 0.666203 , expected 0.6
      FAIL: (j) MULTI-lobe SPF, guiding off: env-NEE vs the escape weight partition to 1  relErr=11.0338% tol=1.5%
    (k) two-lobe 0.280529 , expected 0.248936
      FAIL: (k) MULTI-lobe SPF, guiding off: area-NEE vs the emitter-hit weight partition to 1  relErr=12.6912% tol=2%
    81 passed, 2 failed

Fixed:

    (j) two-lobe 0.599399 , expected 0.6   relErr=0.100175%
    (k) two-lobe 0.248949 , expected 0.248936   relErr=0.00517102%
    83 passed, 0 failed

Both pre-fix errors match an independent quadrature of the two weightings
over the respective domain (+11.2 % predicted for (j) against the env
sampler's `1/4π` density; +12.70 % for (k)).

### 4.5 Why row (k) needed its own, smaller emitter

The DL-74 area row's emitter is `R = 2` at `d = 5`.  Its area-sampled
solid-angle density `d²/(A·cos_light)` is ≈ 0.18 sr⁻¹ there — three
orders below the narrow lobe's ≈ 10 sr⁻¹ — so `w_bsdf` saturates at 1 for
that lobe and the wrong partner changes nothing.  Measured on that
geometry, row (k) reads within 0.33 % of its closed form both before and
after the fix: a green row, not a red-proof.  The same quadrature that
predicts +12.70 % for `R = 0.5` predicts **−0.45 %** for `R = 2`, against
that measured −0.33 %, which is what validates the model rather than the
tuning.  **A partition-of-unity defect is only visible where the two
techniques' densities are comparable.**

## 5. Render-level evidence, and what it closes

`tests/BDPTStrategyBalanceTest.cpp` / `tests/VCMStrategyBalanceTest.cpp`
topology L (a `schlick_material` wall + floor with a large area emitter,
32×32 at 256 spp, depth-matched PT reference).  Isolated A/B: revert
`src/Library/Shaders/PathTracingIntegrator.cpp` alone to this slice's
pre-fix commit, rebuild, re-render the same scene; n = 4 per side.

| | PT | BDPT / VCM | ratio |
|---|---|---|---|
| BDPT pre-fix | 0.0601656 ± 0.0000054 | 0.0632933 ± 0.0000024 | **+5.198 % ± 0.011 pp** |
| BDPT post-fix | 0.0632676 ± 0.0000110 | 0.0632930 ± 0.0000044 | **+0.040 % ± 0.012 pp** |
| VCM pre-fix | 0.0601696 ± 0.0000120 | 0.0632495 ± 0.0000012 | **+5.119 % ± 0.020 pp** |
| VCM post-fix | 0.0632650 ± 0.0000084 | 0.0632485 ± 0.0000039 | **−0.026 % ± 0.011 pp** |

PT moved **+5.16 %** / **+5.14 %**; BDPT and VCM moved −0.0005 % and
−0.002 %.  The asymmetry is the whole point: the only file that changed
lives in PT, BDPT's `pdfFwd` has been the same aggregate function as its
`pdfRev` since DL-69, and VCM derives its densities from
`BDPTVertex::pdfFwd`.  Two estimators that share no MIS code and weight
completely different strategy mixes now agree to 0.04 % — four times
their own run-to-run sigma.

Topology M (the `ggx_material` + `lambertian_material` control, immune to
DL-103 and DL-127) is unmoved: PT 0.0471266 / BDPT 0.0472119 = +0.18 %,
against the +0.16 % recorded before this slice.  The residual really was
Schlick-specific.

### 5.1 It amplifies an open row on the HWSS path (review P2-1)

The fix is correct and its sign is right, but it is worth stating
plainly that it makes **DL-125** cost more, not less.

DL-125 is that `PathTracingIntegrator.cpp`'s HWSS companion loop falls
back to the material's AGGREGATE `pBRDFCur->valueNM(...)` paired with the
hero's PER-LOBE `pS->pdf` and `invSelectProb` whenever the SPF declines
`ISPF::EvaluateKrayNM` — DL-69's own pattern, on the companion
wavelengths, compounding once per bounce.  `SchlickSPF` declines, so on
topology L it fires on every companion.  Raising the weight the
BSDF-sampling strategy carries at a multi-lobe vertex therefore moves an
already-inflated estimate further from the truth.

`tests/BDPTStrategyBalanceTest.cpp`'s `TestPTSpectralHWSSKnownDefect`
measures it PT-vs-PT on one scene — `pathtracing_pel_rasterizer` vs
`pathtracing_spectral_rasterizer` at `hwss TRUE`, plus the hero-only
`hwss FALSE` spectral render as the discriminator between "the spectral
pipeline" and "the HWSS bundle".  One integrator, one material, one
geometry, so a ratio far from 1 cannot be an integrator-balance
question.  Isolated `PathTracingIntegrator.cpp`-only A/B, n = 3 per side:

| | pre-fix | post-fix | move |
|---|---|---|---|
| L, PT pel | 0.060166 ± 0.000004 | 0.063255 ± 0.000008 | **+5.134 %** |
| L, PT spectral hero-only | 0.060886 ± 0.000072 | 0.064053 ± 0.000053 | **+5.202 %** |
| L, PT spectral `hwss TRUE` | 0.091532 ± 0.000508 | 0.103005 ± 0.000446 | **+12.534 %** |
| M, PT pel | 0.047133 ± 0.000004 | 0.047131 ± 0.000005 | −0.004 % |
| M, PT spectral hero-only | 0.047677 ± 0.000060 | 0.047676 ± 0.000071 | −0.003 % |
| M, PT spectral `hwss TRUE` | 0.047127 ± 0.000008 | 0.047120 ± 0.000009 | −0.014 % |

Ratios: L `hwss TRUE`/pel **1.5213 ± 0.0084 → 1.6284 ± 0.0069**;
L hero/pel 1.0120 → 1.0126; M `hwss TRUE`/pel 0.9999 → 0.9998.

Three things that measurement settles.  The hero-only spectral render
moves **with** pel (+5.20 % vs +5.13 %), so DL-103's correction lands
identically on the spectral path — the extra +7.4 pp is the HWSS bundle
alone.  Topology M — `ggx_material` + `lambertian_material`, whose
selected lobe already carries the aggregate `mixPdf` and a matching
`kray`, so the companion fallback computes the right thing — is
unchanged to 0.014 % on every row, so the amplification is specific to
the material the fallback gets wrong.  And the ~1.2 % hero/pel offset is
material-independent (present identically on M), i.e. the
spectral-vs-RGB pipeline, not this row.

DL-103 did not create the 1.63; it made an open row's price visible.
The probe's `hwss TRUE` band is a KNOWN-DEFECT pin, not a gate — DL-125's
closure must drive it toward 1.0.  The DL-125 ledger row is upgraded from
"OPEN-confirmed (static)" to a measured row with this fixture as its
recipe.

**Consequence for the suites**: topology L's *mean* band is tightened
from the shared 8 % to **2 %** in both files and the PROVISIONAL marker
is removed.  What a pass there now claims is "PT and BDPT/VCM agree to
2 % on a multi-lobe material", which is what a strategy-balance test is
for.  It does **not** claim either is correct in absolute terms: DL-127
(`SchlickSPF`'s per-lobe `kray` is the Schlick-1994 sampling weight, not
that lobe's `f_I cos / p_I`) is still open and **both** integrators
consume `kray`, so a residual from that row can sit inside this
agreement rather than show up as a gap.

## 6. Cost

One extra aggregate `ISPF::Pdf()` per non-delta vertex, at every material
— not only multi-lobe ones.

Measured on `scenes/Tests/BDPT/cornellbox_bdpt_materials_pt.RISEscene`
(512×512, PT, 128 spp, a `schlick_material` among the receivers),
**interleaved** pre/post A/B with two separately built binaries, n = 6
pairs, user CPU (wall clock on this machine was contaminated by
concurrent builds and is not usable):

    pre   228.16 ± 3.85 s
    post  232.99 ± 2.23 s
    paired delta +4.82 ± 3.49 s  =  +2.11 %,  paired t = 3.39

A first, non-interleaved batch read +3.09 % (Welch t = 2.90); the
interleaved pairing is the number to quote, and the difference between
the two is exactly the machine-contention drift interleaving removes.

### 6.1 The worst case, and it is not that scene (review P2-2)

`cornellbox_bdpt_materials_pt` has ONE `schlick_material` among twelve
receivers, so +2.11 % is a realistic-mixed-scene number, not a bound.
Re-measured on an **all-`schlick_material`** scene — topology L's own
wall + floor at 320×320 / 256 spp, depth 5, the same interleaved
two-binary protocol, n = 6 pairs, user CPU — and against an
all-`lambertian_material` control that differs from it by exactly one
chunk:

| scene | pre | post | paired delta |
|---|---|---|---|
| all `schlick_material` | 80.04 ± 1.22 s | 114.66 ± 0.85 s | **+34.61 ± 1.19 s = +43.25 %**, t = 71.1 |
| all `lambertian_material` | 52.84 ± 1.41 s | 54.16 ± 1.02 s | +1.32 ± 1.84 s = +2.50 %, t = 1.75 (not significant) |

So the cost is essentially all `SchlickSPF::Pdf` (~690–780 ns per call
since DL-67 Slice 0), once per non-delta bounce; a single-lobe material
pays only the call.  **Quote +43 % as the worst case and +2.11 % as the
realistic one.**

### 6.2 Deferred evaluation: measured, and declined

The partner is *consumed* only when the continuation escapes to the
environment or hits an NEE-sampleable emitter, so most evaluations are
thrown away.  Measured directly with temporary producer/consumer counters
(reverted; the working tree is clean of them):

| scene | aggregate `Pdf()` evaluations | consumed | wasted |
|---|---|---|---|
| all-`schlick_material` topology L, 16 spp | 2 078 282 | 1 035 858 | **50.2 %** |
| `cornellbox_bdpt_materials_pt`, 128 spp | 125 044 482 | 1 950 141 | **98.4 %** |

The capture a deferred design would have to pay was then measured
directly (variant X: copy exactly what a lazy consumer needs — the
`RayIntersectionGeometric`, the live `IORStack`, the traced direction and
the `ISPF*` — and do **not** call `Pdf`), on the all-Schlick scene,
n = 6 interleaved:

    pre-fix                    84.16 ± 3.29 s
    capture only (variant X)   85.69 ± 1.42 s     (+1.8 %)
    eager (shipped)           120.09 ± 2.61 s     (+42.7 %)

    projected lazy = capture + 0.498 x (eager - pre) = 103.60 s  (+23.1 %)

i.e. a lazy design would recover about **46 %** of the added cost on the
worst case (+42.7 % → +23.1 %), and nearly all of it on the realistic one
(+2.11 % → ~+0.06 %).  The capture itself is cheap.

**It is still declined, and the reasons are structural rather than
arithmetic.**

1. **`RAY_STATE` cannot carry it.**  `sizeof(RAY_STATE)` is **96 bytes**
   and it is copied per bounce *and* per shadow ray;
   `sizeof(RayIntersectionGeometric)` is **1120** and `IORStack` 32, so
   carrying the capture inside it is a ~13× growth of the hottest
   copied struct in the transport loop.  The capture therefore has to
   live in PT's own loop frame instead.
2. **Two consumers live outside that frame.**
   `RayCasterEnvEscapeMISWeight` and `EmissionShaderOp` read
   `MisPartnerPdf()` *through* `RAY_STATE`, with no access to the
   producing vertex, and the entry vertex's caller-supplied
   `bsdfMisPdf_` has no producing vertex at all.  A deferred design is
   therefore a THREE-case partner computation (deferred inside the loop,
   eager for anything that leaves it, pass-through at entry) — and
   DL-74's own postmortem is that this bug family comes from exactly
   that shape: *"when a fix SPLITS one field into two, the follow-up work
   is enumerating every consumer of the old one."*
3. **It re-introduces the argument-reconstruction hazard by
   construction.**  The whole point of evaluating at the producer is
   that `ri`, the live `IORStack` and `glossyFilterWidth` are in hand and
   provably the same ones `LightSampler` used at that vertex.  DL-74 P2
   measured **−14 %** on a furnace when the two sides disagreed about the
   IOR stack alone, and DL-69 P2-5 landed on the same class when
   reconstructing a vertex record.  `RayIntersectionGeometric` is also
   not trivially copyable — it contains a polymorphic
   `OrthonormalBasis3D` (the compiler warns that a `memcpy` copies a
   vtable pointer), so the real capture is a non-trivial 1120-byte copy,
   making variant X's +1.8 % a lower bound.

**The better direction, recorded for a future slice**: the SPF already
knows its mixture weights at `Scatter` time, so it could stamp the
aggregate density on each emitted `ScatteredRay` for near-zero marginal
cost — same function, same arguments, computed by the object that owns
them, no capture and no second code path.  That is exactly what
`GGXSPF` and `CookTorranceSPF` already do (`.pdf = mixPdf` on the one
ray they emit), which is also why they are immune to DL-69 and DL-103 in
the first place.  Making the multi-emit SPFs do the same would remove
this cost at its source rather than routing around it.

So the evaluation stays unconditional.  That also keeps the two sides of
the pair structurally identical: an `IsMultiLobe()`-style gate would
reintroduce a case where the escape side and the NEE side evaluate
different functions, which is the defect this row exists to remove.

## 7. Sibling audit

Bug pattern, one sentence: *a strategy's MIS partner density is the
SELECTED lobe's own density while its partner strategy evaluates the
material's AGGREGATE density, so the two weights do not partition to
one.*

| Site | Verdict | Evidence |
|---|---|---|
| `PathTracingIntegrator.cpp` PART 3, RGB/NM templated body | **FIXED** | the row's own site; rows (j)/(k) |
| `PathTracingIntegrator.cpp` PART 3, HWSS body | **FIXED** | same pattern, hero wavelength; its own comment used to claim "no mismatch exists here" on the (false) premise that the NEE arms use the raw material pdf |
| `PathTracingIntegrator.cpp` guided / RIS branches | REFUTED (already correct) | `PTGuidingMisPdf::Eval` has blended the **aggregate** since DL-74; byte-identical after this fix |
| `BDPTIntegrator.cpp` eye/light generators (`pdfFwd`, feeding the s=0 emitter-hit strategy) | REFUTED (fixed by DL-69) | `pdfFwdPrev = PathValueOps::EvalPdfAtVertex(...)`, the aggregate, with the same DL-41 fallback |
| `VCMIntegrator.cpp` `EvaluateS0Impl` / `EvaluateNEEImpl` / the dVCM-dVC recurrence | REFUTED | every density is either `BDPTVertex::pdfFwd` (aggregate since DL-69) or a direct `PathValueOps::EvalPdfAtVertex` call (`VCMIntegrator.cpp` `bsdfDirPdfW`/`bsdfRevPdfW`) |
| MLT | REFUTED | shares BDPT's subpath generators; no MIS partner of its own |
| `RayCaster.cpp`'s env-escape helper `RayCasterEnvEscapeMISWeight` | REFUTED (consumer, not producer) | reads `rs.MisPartnerPdf()`; whatever the producer stored.  Its producers are PT (now aggregate), the two volume continuations, and the BDPT training probe — rows below |
| `RayCaster.cpp` volume phase-scatter continuations (`rs2.bsdfMisPdf = phasePdf`) | REFUTED | a phase function is single-lobe, and `MediumScatterMaterial::Pdf` — the density the volume NEE arm uses — returns that same raw `phasePdf`.  DL-73's ruling, unchanged |
| PT's BSSRDF exit continuations (`rs2.bsdfMisPdf = bssrdf.cosinePdf`) | REFUTED | the paired NEE arm is handed `BSSRDFEntryMaterial`, whose `Pdf` is literally `cosTheta * INV_PI` — the same single-lobe function |
| PT's no-BRDF (SPF-only) continuation (`rs2.bsdfMisPdf = rs2.bsdfPdf`) | REFUTED | `rs2.bsdfPdf` is ALWAYS 0 there, so there is no per-lobe density for an aggregate to disagree with. Exactly two families reach the branch (`GetBSDF()` null): Dielectric / PerfectReflector / PerfectRefractor, whose every emitted lobe sets `isDelta = true` (`DielectricSPF`'s `scattering`/HG-warped transmission included); and BioSpecSkin / GenericHumanTissue, whose lobes are non-delta but never assign `.pdf` at all, so it keeps `ScatteredRay()`'s 0 — and their `ISPF::Pdf` is the base-class 0, so the aggregate would be 0 too. (An earlier revision of this row said "NEE never fires there"; that is a claim about the NEE side, not a checkable property of this line.) |
| `BDPTIntegrator.cpp:264` `RecordGuidingTrainingSampleNM` (`rs.bsdfMisPdf = samplePdf`, a per-lobe `selectProb * pdf`) | REFUTED for this row | a standalone OpenPGL **training** probe with no NEE partner; it affects the recorded training radiance (guiding quality) and cannot bias a render.  Whether a training probe should apply an MIS weight at all is a separate question and was not opened as a row |
| Legacy shader-op chain (`DistributionTracingShaderOp`, `ReflectionShaderOp`, `RefractionShaderOp`, `FinalGatherShaderOp`) | **DISTINCT DEFECT — opened as DL-171** | these never populate `RAY_STATE::bsdfPdf`/`bsdfMisPdf` at all, so `EmissionShaderOp` takes weight 1 (or the emission is suppressed outright) while `DirectLightingShaderOp`'s NEE arm still weights against a positive aggregate pdf.  Absent partner, not wrong partner — a different pattern |
| HWSS's single scalar partner vs `SampledWavelengths::N` companion NEE arms | **DISTINCT DEFECT — opened as DL-170** | pre-existing and equally true of `pS->pdf`; see that row |

One hop deeper, by the CONSUMED field: `grep -rn "bsdfMisPdf\|MisPartnerPdf" src/ tests/`
returns exactly the producers and consumers tabulated above, plus
`IRayCaster.h`'s declaration, `PathTracingShaderOp.cpp`'s three
`rs.MisPartnerPdf()` forwards (already correct since DL-74) and
`EmissionShaderOp.cpp`'s two reads (likewise).

## 8. Gate

Clean rebuild, zero warnings.

| Suite | Counter |
|---|---|
| `PTGuidingMISPartitionTest` | 83 passed, 0 failed (was 81/2 red) |
| `EnvLightBalanceTest` | Passed 123, Failed 0 |
| `BDPTStrategyBalanceTest` | Passed 93, Failed 0 (with the tightened 2 % band) |
| `VCMStrategyBalanceTest` | Passed 68, Failed 0 (with the tightened 2 % band) |
| `MISWeightsTest` | Passed 59, Failed 0 |
| `OptimalMISTrainingSitesTest` | 78 passed, 0 failed |
| `RayCasterEnvEscapeMISTest` | Passed 91, Failed 0 |
| `VolumeEnvFurnaceTest` | Passed 32, Failed 0 |
| `SchlickLobePairingTest` | Passed 27, Failed 0 |
| `LayeredWhiteFurnaceTest` | 0 of 57 configurations failed |
| `CstDeriveGoldenTest` | 452 MATCH, 0 DRIFT |
| `SourceHygieneTest` | 165 passed, 0 failed |

### 8.1 One note for whoever merges this

This slice branched from `master` `aa64c45e`, and every "DL-127 is still
open" caveat above (in §5, in both suites' topology-L comments, and in
the ledger row) is true **at that base**.  `master` has since moved to
`19944c34`, which merges `debt-dl127` — `SchlickSPF`'s per-lobe `kray`
is now `f cos / p`.  That does not invalidate anything measured here:
every A/B in this document is an *isolated*
`src/Library/Shaders/PathTracingIntegrator.cpp`-only revert, with
`SchlickSPF` identical on both sides, so the attributions stand.  But
two things follow for the merge.

* The DL-127 caveat becomes **weaker, not stronger** — it says a residual
  could hide inside the PT/BDPT agreement because both consume `kray`;
  if that row is closed, there is less to hide.  Leaving the caveat in
  is conservative, but it should be re-worded (or struck) once the
  merged tree is the reference.
* Topology L's **absolute** means will move on the merged tree, because
  `kray` changed.  The tightened band is on the **ratio**, which is what
  should survive; re-run `BDPTStrategyBalanceTest` and
  `VCMStrategyBalanceTest` after the merge and re-quote the topology-L
  numbers rather than carrying this document's `0.0632…` figures
  forward.  The same applies to the DL-125 probe's `1.637`: its three
  renders all use `SchlickSPF`, so the ratio is the robust part, not the
  means.

`EnvLightBalanceTest`'s closed-form PT rows did not move, and they should
not have: every receiver in that suite is `lambertian_material`, a
single-lobe SPF whose `.pdf` **is** its aggregate.  The fix is a no-op
there by construction.
