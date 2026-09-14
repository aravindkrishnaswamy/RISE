# DL-74: the guided pdf had two jobs and one home

Status: **CLOSED 2026-09-14** (debt-guiding2 slice, review round 3).
**DL-83 (the RIS-mode residual) closes with it** — the repair's nominal
density does not depend on the guiding mode, so there was nothing
mode-specific left to leave open.

Red-proof: `tests/PTGuidingMISPartitionTest.cpp`, 37 checks / 0 failures;
red on `8fcce0bf` with 4 of its 6 measurement rows failing by −66 % to
+63 %, and red again on the round-2 HEAD `2ebcaff9` with the two rows
round 3 added failing by +44 % and −14 % (§8).

> **Supersedes the first repair.** An earlier version of this document
> described a narrower fix (an `IGuidedNEEPdfBlend` hook applied to the
> ENV-NEE arm only, in ONE-SAMPLE mode only, using the BASE
> `rc.guidingAlpha`) and declared DL-74 closed on the strength of a
> manual derivation plus "existing suites unchanged". Review found that
> repair did not close the partition — it left four independent
> disagreements standing (below) and had no test that could have seen
> them. The account below replaces it.

## 1. Mechanism

Path guiding draws the continuation from a mixture of the material and a
learned guide. `PathTracingIntegrator.cpp` recorded the density that
mixture was drawn from —

```
rs2.bsdfPdf = effectiveBsdfPdf;   // combinedPdf, or risEffectivePdf
```

— in a single `RAY_STATE` field, and that one field was then used for
**two different jobs**:

1. as the **throughput / training denominator**: `bsdfTimesCos` is
   `scatterThroughput * bsdfPdf`, and `OptimalMISAccumulator::Accumulate`
   forms the second moment `f² / pdf²`, so this role REQUIRES the true
   sampling density;
2. as the **MIS partner** against light sampling, at the env escape, at
   the emitter hit, and (through `RayCasterEnvEscapeMISWeight`) at every
   `RayCaster` continuation escape.

Role 2 has a hard requirement role 1 cannot satisfy: the light-sampling
side must evaluate the SAME function of direction for the same ω, or the
two weights stop summing to one. `LightSampler`'s NEE arms cannot
reproduce `effectiveBsdfPdf`: NEE runs at PART 2, before PART 3 has
stochastically selected a lobe, so the per-lobe alpha, the per-lobe
cosine convention and the selected lobe's own pdf are all unavailable to
it by construction.

Four concrete disagreements followed, each measured by its own row of the
red-proof:

| # | Escape side | NEE side | Row |
|---|---|---|---|
| 1 | `min(1, alpha·2·GetCellAlpha(cell))` when `guidingLearnedAlpha` (the DEFAULT, `RuntimeContext.h`) | base `rc.guidingAlpha` | (b) |
| 2 | `GuidingEffectiveAlpha` HALVES alpha for an `eRayReflection` lobe | no per-lobe damping | (c) |
| 3 | `ApplyCosineProduct` only for `eRayDiffuse` | applied unconditionally | (c) |
| 4 | area-light hits weighted with the guided pdf; RIS mode weighted with `risEffectivePdf` | area-light NEE unguided; RIS excluded outright | (d), (e) |

Worked example for #1 (α 0.5, cell 0.9, material pdf b = 1, guide pdf
g = 100, envPdf 50.5): the NEE side weights with
`p = 0.5·100 + 0.5·1 = 50.5`, giving `w_nee = 50.5²/(50.5²+50.5²) = 0.500`;
the escape side weights with `α_eff = min(1, 0.5·2·0.9) = 0.9`, so
`p = 0.9·100 + 0.1·1 = 90.1` and `w_bsdf = 90.1²/(90.1²+50.5²) = 0.761`.
Sum 1.261 — 26 % of the energy at that direction created out of nothing.

## 2. The repair: separate the two roles

### 2.1 Throughput denominator — untouched

Every division that produces radiance still uses the density the
direction was really drawn from: the per-lobe `GuidingEffectiveAlpha`,
`combinedPdf`, `risEffectivePdf`, `PTScatterKray`/`selectProb`. Changing
any of those would bias the estimator. `RAY_STATE::bsdfPdf` keeps that
value, and so do `bsdfTimesCos` and every optimal-MIS `Accumulate`.

### 2.2 MIS partner — one lobe-independent NOMINAL density

A new field, `RAY_STATE::bsdfMisPdf`, carries

```
p_mis(w) = alpha_nom * guide(w) + (1 - alpha_nom) * p_aggregate(w)
```

with three fixed conventions, each chosen because it is the only variant
BOTH sides can evaluate:

- **`alpha_nom`** is the base `rc.guidingAlpha`, scaled by the learned
  per-cell sigmoid exactly as PART 3 scales it
  (`min(1, alpha·2·GetCellAlpha)`) when `rc.guidingLearnedAlpha` is on,
  and **not** halved for a glossy lobe. Per-lobe damping is unavailable
  to NEE by construction, so it cannot be part of a shared function.
- **the cosine product is applied unconditionally.** `guide(w)` must be
  one function of direction; guide×cosine is the physically motivated
  factorisation for surface reflection in both lobe regimes (it is what
  PART 3 already used for the dominant diffuse case and what the NEE side
  already used for every case), and applying it changes nothing about
  unbiasedness on the sampling side because the guided throughput divides
  by the same post-product density it sampled from.
- **`p_aggregate`** is the material's all-lobes `IMaterial::Pdf()` — the
  quantity every NEE arm already computes, and the one quantity at the
  vertex that does not depend on the stochastic lobe choice.

The guiding **mode** does not appear anywhere in that formula. That is
why DL-83 closes here rather than staying open: there was never a
RIS-specific quantity to reproduce, only a RIS-specific *sampling*
density, and the sampling density is no longer what the weights are built
from.

### 2.3 Why weighting with a non-sampling density is legal

One line. For strategies `s` with true sampling densities `p_s`, drawing
`w_s ~ p_s` and forming `F = Σ_s W_s(w_s) f(w_s) / p_s(w_s)`,

```
E[F] = Σ_s ∫ W_s(w) f(w) dw = ∫ f(w) · ( Σ_s W_s(w) ) dw = ∫ f
```

whenever `Σ_s W_s(w) = 1`. The true densities appear in the estimator
only as the divisors `p_s`, which are untouched; the weights `W_s` are
free, and are unbiased for **any** partition of unity. So building both
weights from a shared nominal `p_mis` costs nothing in correctness. Only
variance is affected, and only to the extent `p_mis` is a worse
description of the sampling than the true density — second-order, and in
the common (diffuse-lobe) case `p_mis` IS the true density.

The derivation has one side condition: `W_s(w)` must be zero wherever
`p_s(w)` is zero, or the strategy that is supposed to carry that share of
`f(w)` can never generate `w` and the share is simply lost. Section 2.4
is that condition.

### 2.4 The `pdf > 0` gate, and delta lobes

> **Rewritten in round 4 (§9.2).** Rounds 2 and 3 gated the blend on
> `p_aggregate > 0` and the paragraphs below argued for that gate. The
> argument is sound only where the material is the ONLY sampler, which
> is precisely what guiding stops being true. What follows is the
> corrected rule; §9.2 has the measurement that overturned the old one.

Both sides agree on a single rule: **`p_mis(w) = 0` means no BSDF-side
partner exists at `w`, and the light-sampling sample is taken whole.**
What changed is *when that is true*.

- **Delta lobes** — still exactly this case. `bsdfMisPdf` stays 0 for a
  delta lobe on the escape side, NEE cannot sample through a Dirac, and
  `w_bsdf = 1` for the delta direction is correct.
- **Guiding inactive** — still exactly this case. `PTGuidingMisPdf::Eval`
  passes `aggregatePdf` through untouched (zero included) when it is not
  configured, so a vertex with no guiding keeps its pre-DL-74 weights
  byte for byte.
- **`p_aggregate(w) = 0` WITH guiding active** — **not** this case. The
  continuation is drawn from `alpha_nom*guide + (1-alpha_nom)*p_agg`, and
  `PathTracingIntegrator`'s one-sample branch traces the guided direction
  whenever `combinedPdf > NEARZERO`, which `alpha*guide` alone satisfies.
  The BSDF technique therefore DOES generate `w`, with density
  `alpha_nom*guide(w)`, and that is the partner both sides must use.

So `Eval` returns the full mixture unconditionally while guiding is
active, and the four NEE arms call the hook OUTSIDE their `pdf > 0`
gates, gating the weight on the BLENDED value. The hook — not the
caller — decides what a zero aggregate pdf means; `IGuidedNEEPdfBlend`'s
contract says so.

The `polished_material` worry the old gate was built around does not
survive contact with the code: with guiding active, a zero aggregate pdf
now yields `alpha_nom*guide(w) > 0` on the NEE side AND on the escape
side (`misBsdfPdf = guidingMis.Eval(traceRay.Dir(), PTEvalPdfAtSurface(...))`
takes the same zero argument through the same function), so the two move
together and the pair still sums to one. The case where the escape
"kept full weight" — a SELECTED DELTA LOBE — is unaffected, because a
delta lobe is excluded from the blend on the escape side by `!pS->isDelta`
and its own direction is one NEE can never sample.

### 2.5 One distribution per shading point

The guiding distribution is now initialised once, above PART 2, and
shared by NEE and the continuation. Three things depended on that:

1. **the partition itself** — the two sides must query the same
   distribution object, cosine-multiplied the same way;
2. **the stochastic region lookup** — `InitDistribution` takes a 1D
   sample and uses it to pick a spatial region. Two calls could resolve
   the two sides of one MIS pair to two different regions of the field;
3. **the sampler dimensions** — the old NEE-side setup drew an extra
   `Get1D()` off the NEE sampler, shifting every downstream QMC dimension
   whenever it fired. Now nothing extra is drawn, and when guiding is
   inactive the gate fails on its first clause and NOTHING is drawn at
   all, so a guiding-off render's dimension budget is byte-identical
   (`SobolDimensionBudgetTest` passes).

What this changed about WHERE the draw happens, stated plainly because it
is a behaviour change and not only a refactor: with guiding ON, the
`InitDistribution` 1D draw now happens at EVERY eligible vertex, before
PART 2, instead of only at those vertices whose PART 3 reached the guiding
block.  That is a variance-only difference — it consumes one sampler
dimension per eligible vertex, shifting the QMC stream of a guided render,
and buys the partition (§2.5 reasons 1-3).  With guiding OFF or untrained
the gate fails on its first clause and NOTHING is drawn, so a guiding-off
render's dimension budget is byte-identical; `SobolDimensionBudgetTest` is
the guard for that half of the statement.

The gate above PART 2 carries only the VERTEX-level parts of
`GuidingEffectiveAlpha`'s eligibility: a trained field, `depth <=
maxGuidingDepth`, a nonzero base alpha, and the same `eRaySpecular`
incoming-state rejection. The per-LOBE parts (`isDelta`, glossy
half-damping, `GuidingSupportsSurfaceSampling`) stay in PART 3, where the
lobe is known. They steer SAMPLING and deliberately do not steer the
shared nominal density — that separation is the whole point.

Note what this removes: the first repair's `ScattersFullSphere()` gate.
It existed to keep the guide term away from transmission lobes that PART
3 would never guide. Under the nominal-density design it is unnecessary
and in fact harmful: the escape side now also weights with `p_mis`
whether or not guiding actually fired for the selected lobe, so a
full-sphere material's two sides agree like every other material's.
(`TranslucentIORStackTest`, which exercises real trained OpenPGL through
production PT and BDPT on a full-sphere translucent material, passes.)

### 2.6 The volume vertex (DL-72 P2-5)

`RayCaster`'s volume phase-scatter continuations were the same
two-roles collision with the signs reversed. DL-73 established that the
volume vertex's MIS partner must be the RAW `phasePdf`, because env-NEE
at a volume vertex weights with `MediumScatterMaterial::Pdf`, which is
that same raw value. But the direction is drawn from `effectivePdf` (the
guided mixture when guiding fires), and the round-3 code recorded the
training moment against `phasePdf` — dividing the moment by a density the
sample did not come from.

With the fields separated, both statements are expressible at once:
`bsdfMisPdf = phasePdf` (DL-73's requirement) and `bsdfPdf = effectivePdf`
(the true density). The trained ratio `bsdfTimesCos / bsdfPdf` is then
`phaseValue / effectivePdf`, which is exactly this continuation's own
per-sample weight — 1 with guiding off, `guidingMISWeight` with it on.

### 2.7 Default and accessor

`bsdfMisPdf` defaults to **-1**, meaning "not set — use `bsdfPdf`", and is
read through `RAY_STATE::MisPartnerPdf()`. A producer that has no guiding
to describe (and any future one that forgets the field) keeps its exact
pre-DL-74 weight rather than silently losing it. Zero remains meaningful
and distinct: "no partner exists, weight 1".

## 3. Red-proof

`tests/PTGuidingMISPartitionTest.cpp` builds two closed-form furnaces on
the live `Job`/`RayCaster`/`LightSampler` (one throwaway render first,
because `RayCaster::AttachScene` calls `Prepare()` **before**
`SetEnvironmentSampler()`, so the env sampler does not exist straight
after a load):

- a reflectance-1 Lambertian under a constant-radiance environment
  re-radiates exactly `L_env`;
- the same surface under a uniform-radiance sphere of radius `R` centred
  `d` along its normal reads `L_e·R²/d²`.

One NEE sample and one BSDF/guided continuation are the only two
strategies in play, so either reading is the closed form only if their
weights partition to one.

Rows and measurements on `8fcce0bf` (pre-fix):

| Row | Configuration | Pre-fix | Target | Error |
|---|---|---|---|---|
| reference | guiding off | 0.599541 | 0.6 | +0.08 % |
| (a) | fixed alpha, diffuse lobe | 0.599027 | 0.6 | +0.16 % |
| (b) | `guidingLearnedAlpha=true` (default), cell sigmoid driven to its floor | 0.979993 | 0.6 | **+63.3 %** |
| (c) | lobe tagged `eRayReflection` | 0.884727 | 0.6 | **+47.5 %** |
| (d) | area light: area-NEE vs the guided emitter hit | 0.116749 | 0.203718 | **−42.7 %** |
| (e) | RIS-mode guiding | 0.204389 | 0.6 | **−65.9 %** |

18 passed, 4 failed. Post-fix: **22 passed, 0 failed**, every row within
0.55 % of its closed form — the same order as the two unguided controls,
i.e. Monte Carlo noise.

Two design notes about the fixture:

- **Rows "reference" and (a) are controls, and were green before the
  fix.** Fixed alpha + a diffuse lobe + one-sample MIS + an environment
  is the single configuration in which the two old constructions
  coincided. Their being green on the unfixed library is what proves the
  other four rows measure the asymmetry and not the harness.
- **Row (c) re-tags a real `LambertianSPF`'s lobe** through an `ISPF`
  decorator rather than introducing a real glossy SPF. The tag is the
  sole input to both asymmetries that row exists to measure
  (`GuidingEffectiveAlpha`'s glossy half-damping and PART 3's
  `eRayDiffuse`-only `ApplyCosineProduct`), so changing only the tag
  keeps the sampling density, the BSDF value, the horizon gate and above
  all the albedo-1 closed form bit-identical to rows (a)/(b). A
  production glossy lobe (GGX's specular lobe, `IsotropicPhongSPF`,
  `CoatedSPF`'s coat lobe) reaches the same integrator code with the same
  tag.

## 4. Sibling audit

| Site | Verdict |
|---|---|
| **Env NEE, RGB + NM** (`LightSampler.cpp`) | Fixed. **Round 4**: the blend moved OUTSIDE the `pBsdf > 0` gate and the weight now gates on the blended value (§9.2). |
| **Area-light NEE, RGB + NM** (`LightSampler.cpp`) | Fixed — the same defect, unfiled before this round. Its BSDF-side partner (PT's emitter-hit weight) was already guided. **Round 4**: same gate move as the env arms, red-proofed at +100 % by row (i). |
| **Escape weight** (PT's own env-miss arm, and `RayCasterEnvEscapeMISWeight` for every `RayCaster` continuation) | Fixed: weight from the nominal density, training from the true one. |
| **Emitter-hit weight** (PT PART 1) | Fixed, same split. |
| **RIS mode** (DL-83) | Closed here. The nominal density is mode-independent; there is no RIS-specific quantity to reproduce. |
| **Volume vertices** | Fixed as the two-role split (§2.6). DL-73's "the MIS partner stays the raw phase pdf" ruling is preserved exactly and is now explicit in its own field. |
| **The shader-op boundary** (`PathTracingShaderOp`'s three entry points) | Round 3: was forwarding only `rs.bsdfPdf` into `IntegrateFromHit{,NM,HWSS}`, which then re-derived the partner as "the same value". Fixed -- all three forward `rs.MisPartnerPdf()`, and the three entry points take it as a trailing parameter defaulting to -1 = "same as `bsdfPdf`". |
| **`EmissionShaderOp`** (RGB + NM emitter-hit weight -- the legacy shader chain's half of the same pair) | Round 3: read `rs.bsdfPdf` in the partner role, the same defect one file over. Fixed; both now read `MisPartnerPdf()` and gate on either density being positive. |
| **PT's OWN in-loop volume vertex** (`IntegrateFromHitTemplated`'s medium-scatter branch) | Round 3: a THIRD producer, missed by the round-2 split. Fixed -- `bsdfMisPdf = phasePdf` beside `bsdfPdf = effectivePdf`; its stale `bsdfTimesCos` cleared (DL-84). See §8.3; measured +9.6 % on a floor-in-fog white furnace. |
| **PT's OWN camera-ray volume walks** (`IntegrateRayTemplated` and `IntegrateRayHWSS`) | NOT siblings -- confirmed by reading. Both sample `pPhase->Sample(wo, sampler)` with no guiding block anywhere in those loops, so their local `walkPdf` is the true density and the MIS partner at once. |
| **HWSS** (`IntegrateFromHitHWSS`) | NOT a sibling — confirmed by reading, not assumed. `effectiveBsdfPdf` there is assigned once from `pS->isDelta ? 0 : pS->pdf` and never reassigned; the function has no guiding block. Both its sides already used the raw material pdf. The stale comment claiming otherwise is corrected in place. **Round 3 qualifier**: "produces no guided density" is not the same as "never sees one" -- HWSS is reachable through the shader-op boundary from `RayCaster`'s guided volume continuation, so it now CARRIES an incoming partner (used by its env-escape and emitter-hit weights and by its per-wavelength NM fallbacks, and reset to the sampling density from its first own continuation onwards). |
| **BSSRDF / RW-SSS entry NEE** | Not wired. The entry material's `Pdf()` is the BSSRDF's own cosine density and its continuation's `rs2.bsdfPdf = bssrdf.cosinePdf` is never guiding-adjusted anywhere, so both sides already agree. Both fields are set to the same value there, explicitly. |
| **BDPT / VCM / MLT** | Do not call `LightSampler::EvaluateDirectLighting{,NM}` at all. BDPT's own guiding/NEE consistency is a structurally separate code path, out of scope. BDPT's one `RAY_STATE` producer (a training probe cast) sets both fields to the same value. |

## 5. Known residuals (not filed as new rows)

- **Coverage invariant the nominal partner relies on (review round 5).**
  `p_mis` is lobe-independent (configured from vertex gates) while the
  true continuation density is lobe-dependent (only non-delta
  diffuse/reflection lobes are guiding-eligible).  At a vertex where NO
  scattered lobe is eligible, `p_mis = alpha_nom * guide > 0` in the
  zero-aggregate region while the true density there is 0, so the
  BSDF-side weight claims mass the technique cannot deliver -- the mirror
  image of the round-4 double count.  Unreachable in-tree: the only SPFs
  emitting exclusively ineligible lobes (`GenericHumanTissueSPF`,
  `PerfectRefractorSPF`, `DielectricSPF`) belong to materials whose
  `GetBSDF()` is null, so NEE evaluates nothing.  Invariant to keep: a
  material with a non-null BSDF must expose at least one selectable
  non-delta diffuse/reflection lobe.
- **Optimal-MIS training sites disagree on Russian roulette (round 5).**
  The BSSRDF exit/entry pair trains PRE-RR quantities (`bssrdfWeight *
  cosinePdf`, `neeTrainingScale`), while the main surface continuation
  trains from `scatterThroughput` AFTER the `rr.survivalProb` division.
  Two estimators' second moments feed one tile's alpha.  Variance-only,
  never bias; the BSSRDF pair is internally consistent.  Pick one
  convention when DL-84 (the still-untrained in-loop volume site) is
  wired.

- ~~**The IOR stack passed to the aggregate pdf differs by side.**~~
  **WRONG, and fixed in round 3 (§8.2).** This entry claimed the
  divergence was "pre-existing, orthogonal to guiding, and not made worse
  here". Two of those three are false. The escape side never evaluated the
  aggregate pdf AT ALL before this row — that is exactly what §2.2
  introduced — so "pre-existing" describes a comparison that did not
  previously exist; and the measured error on an albedo-1 furnace inside a
  dielectric is −14 %, which is not orthogonal to anything. (The one true
  part: it is equally present with guiding off, which is why round 3's fix
  is NOT gated on the blend being active.)
- **Aggregate vs selected-lobe pdf with guiding OFF.** With no guiding,
  the escape side still stores the SELECTED LOBE's `pS->pdf` while NEE
  uses the aggregate. At a multi-lobe material those differ. This is
  DL-67 territory (the same measure inconsistency, from the throughput
  side) and is deliberately left exactly as it was. Round 3 narrows what
  "byte-identical with guiding off" can still be claimed for: the escape
  side's DENSITY is untouched with guiding off, but the NEE side's
  `p_aggregate` is now evaluated under the live IOR stack (§8.2), so a
  guiding-off render of a material whose `Pdf()` reads the stack DOES
  change — for the better, and only there.

## 6. File status

| File | Status |
|---|---|
| `src/Library/Interfaces/IRayCaster.h` | `RAY_STATE` gains `bsdfMisPdf` (default -1) and `MisPartnerPdf()`; `bsdfPdf`'s doc now states its single remaining role. |
| `src/Library/Lights/LightSampler.h` | `IGuidedNEEPdfBlend`'s contract rewritten around the nominal-density design. **Round 3**: `EvaluateDirectLighting{,NM}` gain a trailing `const IORStack* pMisIorStack` (null = the historical sentinel). **Round 4**: the contract's `rawPdf > 0` PRECONDITION is gone -- `rawPdf` may be zero and the IMPLEMENTATION decides what that means (§9.2); and `EvaluateDirectLighting{,NM}` gain a trailing `neeTrainingScale` (default 1) that scales the optimal-MIS TRAINING integrand only, never the returned radiance (DL-72 P2-3). |
| `src/Library/Lights/LightSampler.cpp` | All FOUR NEE arms (env and area-light, RGB and NM) blend through the hook, gated `!isVolumeScatter`. **Round 3**: the same four arms evaluate the aggregate pdf under the caller-supplied live IOR stack instead of the `IORStack(1.0)` sentinel. **Round 4**: the blend moved OUTSIDE each arm's `pdf > 0` gate, which now tests the BLENDED value (§9.2); and all four `Accumulate` sites apply `neeTrainingScale`. |
| `src/Library/Shaders/PathTracingIntegrator.cpp` | **Round 4**: `PTGuidingMisPdf::Eval` returns the full mixture whenever guiding is active, zero aggregate pdf included (§9.2); the HWSS SSS mid-path fallback forwards `bsdfMisPdf` like its three siblings (§9.1); `PTBssrdfSwTimesCos` -> `PTBssrdfTrainedBsdfTimesCos` and the two BSSRDF entry-NEE calls pass `neeTrainingScale` (DL-72 §5). **Round 3 also**: the in-loop volume-scatter continuation now sets `bsdfMisPdf = phasePdf` and clears its stale `bsdfTimesCos` (§8.3, DL-84). `PTGuidingMisPdf` (replaces `PTGuidedNEEPdfBlend`); the distribution is initialised once above PART 2 and shared; PART 3 no longer re-initialises, re-draws or re-applies the cosine product; `misBsdfPdf` computed at the continuation and carried to the emitter-hit and env-escape weights; BSSRDF, SPF-only and HWSS sites set both fields explicitly; three stale comments corrected. **Round 3**: the three `IntegrateFromHit*` entry points and the two templates behind them take a trailing `bsdfMisPdf_` (-1 = "same as `bsdfPdf`"); the env-escape and emitter-hit gates admit either density; the HWSS body carries an incoming partner through its weights and its NM fallbacks; PART 2's NEE and the HWSS NEE site pass the live `iorStack` as the MIS-partner evaluation context. |
| `src/Library/Shaders/PathTracingShaderOp.cpp` | **Round 3**: all three entry points forward `rs.MisPartnerPdf()` alongside `rs.bsdfPdf`. This file is the P1 defect's whole surface. |
| `src/Library/Shaders/EmissionShaderOp.cpp` | **Round 3**: the RGB and NM emitter-hit weights read `MisPartnerPdf()` instead of `bsdfPdf` and gate on either density. |
| `src/Library/Shaders/DirectLightingShaderOp.cpp` | **Round 3**: passes its own `ior_stack` as the MIS-partner evaluation context, so its NEE arm evaluates `p_aggregate` under the stack the vertex is standing in rather than the `IORStack(1.0)` sentinel. **Round 4 correction**: the earlier wording here claimed this made "the legacy chain's NEE arm and `EmissionShaderOp`'s weight evaluate one function of direction". It does not, and that chain does not partition at all: `ReflectionShaderOp`, `RefractionShaderOp`, `DistributionTracingShaderOp` and `FinalGatherShaderOp` all continue with a DEFAULT-CONSTRUCTED `IRayCaster::RAY_STATE rs2;`, so `bsdfPdf = 0` and `bsdfMisPdf = -1`, `MisPartnerPdf()` is 0, and `EmissionShaderOp` weights every emitter hit reached through them at 1. What round 3 actually bought is that the legacy NEE arm is now self-consistent with the BSDF-sampling density its own vertex uses; making the legacy chain partition would mean teaching those four ops to populate a `RAY_STATE`, which is not in this slice's scope and is not claimed. |
| `src/Library/Utilities/MediumTransport.cpp` | **Round 3**: `MediumScatterMaterial::Pdf`'s comment corrected -- it is `rs2.bsdfMisPdf`, not `rs2.bsdfPdf`, that carries the raw phase pdf since the round-2 split. |
| `src/Library/Utilities/OptimalMISAccumulator.{h,cpp}` | **Round 3**: const `GetTileTraining()` accessor for the raw per-tile sums and counts, so a test can assert the trained QUANTITY and not only `Solve()`'s ratio (DL-72 P3-4). |
| `src/Library/Rendering/RayCaster.cpp` | `RayCasterEnvEscapeMISWeight` weights from `MisPartnerPdf()` and trains from `bsdfPdf`; both volume continuations split the two roles (DL-72 P2-5). |
| `src/Library/Shaders/BDPTIntegrator.cpp` | Its one `RAY_STATE` producer sets both fields. |
| `tests/PTGuidingMISPartitionTest.cpp` | Added — the red-proof. **Round 3**: two measurement rows added (§8) plus two deterministic premise checks on the production `PolishedSPF` / `TranslucentSPF` pdfs. **Round 4**: rows (h) and (i), the zero-aggregate-pdf wedge on each of the two MIS pairs, each with a guiding-OFF control and a deterministic wedge premise (§9.2). 63 checks. |
| `tests/OptimalMISTrainingSitesTest.cpp` | **Round 3**: both sites now assert the trained moment itself (a whole multiple of `L_env^2` at the volume site; exact `L_env^2` scaling at both). **Round 4**: the spatial-weight scaling law on both halves of the BSSRDF pair (DL-72 §5). 23 checks. |
| `tests/VolumeAbsorptionAttenuationTest.cpp` | **Round 4**: row [R] repeat-averaged over 4 independently seeded renders (`RenderCentralBlockAveraged`) — it was a 2.6-sigma gate on its own 3.13 % spread and failed ~1 % of runs. Not a transport change; §9.6 has the measurement. |
| `docs/DEBT_LEDGER.md` | DL-74 and DL-83 struck CLOSED; Counts updated. |

## 7. Gate

**Round 2** (kept for the record): `PTGuidingMISPartitionTest` 22/0 ·
`PTGuidedSelectProbTest` ALL PASSED · `TranslucentIORStackTest` ALL PASSED ·
`RayCasterEnvEscapeMISTest` 91/0 · `OptimalMISAccumulatorTest` 34/0 ·
`MISWeightsTest` 59/0 · `SobolDimensionBudgetTest` ALL PASSED ·
`SSSRadianceScalingTest` 574017/0 · `OptimalMISTrainingSitesTest` 7/0 ·
`EnvLightBalanceTest` 116/0 · `BDPTStrategyBalanceTest` 66/0 ·
`AgentLiveCommitTest` 884/0 · `VCMStrategyBalanceTest` 54/1 then 55/0 on an
immediate re-run.  **That gate did not include `VolumeEnvFurnaceTest`,
which is what let §8.3's +9.6 % through.**

**Round 3**, after `make -C build/make/rise clean && make -C build/make/rise
-j8 all` (**0 warnings**):

`PTGuidingMISPartitionTest` 37/0 · `OptimalMISTrainingSitesTest` 19/0 ·
`PTGuidedSelectProbTest` ALL PASSED · `TranslucentIORStackTest` ALL PASSED ·
`RayCasterEnvEscapeMISTest` 91/0 · `OptimalMISAccumulatorTest` 34/0 ·
`MISWeightsTest` 59/0 · `SobolDimensionBudgetTest` ALL PASSED ·
`SSSRadianceScalingTest` 574017/0 · `BDPTStrategyBalanceTest` 66/0 ·
`VCMStrategyBalanceTest` 55/0 **twice** (run twice on purpose -- round 2's
54/1 was a seed-sensitive p99 tail; both runs clean here) ·
`AgentLiveCommitTest` 884/0 · `DirectionalFogTest` 11/0 ·
`VolumeEnvFurnaceTest` 29/0 · `EnvLightBalanceTest` 116/0.

Plus every suite that mentions `IntegrateFromHit` or `RAY_STATE`
(`grep -l 'IntegrateFromHit\|RAY_STATE' tests/*.cpp tests/*.h`), which is
how the touched-class rule resolves for this change:
`AgentViewModeRenderTest` 687/0 · `AreaLightShaderOpScalarNTest` 11/0 ·
`AmbientOcclusionCastsShadowsTest` 10/0 · `HairRenderTest` 26/0 ·
`VolumeAbsorptionAttenuationTest` 89/0 — **that last figure was quoted as
if it were stable and it was not**: the suite fails ~1 % of runs on row
[R], independently of this slice.  §9.6 has the measurement and the
fixture fix.

**Round 4**, after `make -C build/make/rise clean && make -C build/make/rise
-j8 all` (373 files, **0 warnings**), on `7c6d8f10`:

`PTGuidingMISPartitionTest` 63/0 · `VolumeEnvFurnaceTest` 29/0 ·
`OptimalMISTrainingSitesTest` 23/0 · `OptimalMISAccumulatorTest` 34/0 ·
`RayCasterEnvEscapeMISTest` 91/0 · `MISWeightsTest` 59/0 ·
`DirectionalFogTest` 11/0 · `RayCasterVolumeAbsorptionTest` 9/0 ·
`VolumeAbsorptionAttenuationTest` 89/0 **three times** ·
`EnvLightBalanceTest` 116/0 · `BDPTStrategyBalanceTest` 66/0 ·
`VCMStrategyBalanceTest` 55/0 · `TranslucentIORStackTest` ALL TESTS
PASSED · `TranslucentSpectralParityTest` 1918 checks / 0 failures ·
`SSSRadianceScalingTest` 574017/0 · `RandomWalkSSSTest` ALL PASSED ·
`PTGuidedSelectProbTest` ALL TESTS PASSED · `SobolDimensionBudgetTest`
ALL PASSED · `GGXWhiteFurnaceTest` ALL TESTS PASSED ·
`LayeredWhiteFurnaceTest` 0 of 57 configurations failed ·
`RefractiveRadianceScalingTest` 38/0 · `HairRenderTest` 26/0 ·
`FabricRenderTest` 57/0 · `AgentLiveCommitTest` 884/0.

## 8. Round 3 (2026-09-14, review round 3 of the same slice)

Round 2 split one field into two.  Round 3 is the work that split implies:
enumerating every CONSUMER of the old field, and auditing the evaluation
CONTEXT the two sides share.

### 8.1 P1 — the partner did not survive the shader-op boundary

`RAY_STATE::bsdfMisPdf` was consumed in exactly one place:
`RayCasterEnvEscapeMISWeight`.  Every other consumer still read
`bsdfPdf`:

* `PathTracingShaderOp`'s three entry points forwarded `rs.bsdfPdf` into
  `IntegrateFromHit{,NM,HWSS}`, and the integrator opened with
  `bsdfMisPdf = bsdfPdf` under a comment asserting that every caller
  enters from a non-guided context.  That assertion was false for exactly
  one producer — `RayCaster`'s volume phase-scatter continuation, the one
  §2.6 had just taught to set the two fields to different values.
* `EmissionShaderOp`'s RGB and NM emitter-hit weights read `rs.bsdfPdf`
  directly in the partner role.

So: a global medium + trained VOLUME guiding + an area emitter weighted
the emitter hit with the guided combined pdf, while the volume vertex's
own area-NEE arm (`MediumScatterMaterial::Pdf` = the raw phase pdf)
weighted against the raw one.  With the guide skewed toward the emitter,
`w_bsdf -> 1` where `w_bsdf` should have been ~0.03, and `w_nee` stayed
~0.97.

Measured on `2ebcaff9` (row (g), a global fog + an emissive sphere,
driven through the real `RayCaster::CastRay` with the real shader
dispatch; the invariant is that guiding may not change the EXPECTATION):

| | unguided | guided | error |
|---|---|---|---|
| round-2 HEAD | 0.0077211 | 0.0111283 | **+44.1 %** |
| round 3 | 0.0077211 | 0.00772427 | +0.041 % |

Fix: `IntegrateFromHit`, `IntegrateFromHitNM`, `IntegrateFromHitHWSS`
(and `IntegrateFromHitTemplated` / `IntegrateFromHitForTag` behind them)
take a trailing `bsdfMisPdf_` with the same three-valued convention
`MisPartnerPdf()` uses, -1 meaning "same as `bsdfPdf`"; the shader op
forwards `rs.MisPartnerPdf()`; `EmissionShaderOp` reads the partner.
Every MIS block whose two arms use two different densities (train from
`bsdfPdf`, weight from `bsdfMisPdf`) now gates on EITHER being positive,
the rule `RayCasterEnvEscapeMISWeight` already followed.

Consumer classification, the deliverable of this audit (`grep bsdfPdf`
over `src/Library/Shaders/*ShaderOp.cpp` and `*Integrator*.cpp`):

| Site | Role | Verdict |
|---|---|---|
| `RayCaster.cpp` env-escape weight | partner | already `MisPartnerPdf()` |
| `RayCaster.cpp` env-escape `Accumulate` | true density | `bsdfPdf`, correct |
| `PathTracingShaderOp.cpp` x3 | forwards into BOTH roles | **fixed** |
| `EmissionShaderOp.cpp` RGB + NM | partner | **fixed** |
| PT env-escape / emitter-hit `Accumulate` | true density | `bsdfPdf`, correct |
| PT env-escape / emitter-hit weight | partner | `bsdfMisPdf`, correct |
| PT env-escape / emitter-hit GATE | both | **widened to either** |
| HWSS env-escape + emitter-hit weight | partner | **fixed** (carries the incoming partner) |
| HWSS per-wavelength NM fallbacks | forwards | **fixed** |
| PT's own camera-ray volume walk (`walkPdf`) | both | no guiding there; equal by construction |
| `BDPTIntegrator.cpp` training probe | producer | sets both alike |

### 8.2 P2 — the two sides evaluated `p_aggregate` under different IOR stacks

The NEE arms evaluated `pMaterial->Pdf(w, ri, defaultIOR)` against a
`static const IORStack(1.0)` sentinel; the escape side evaluates
`PTEvalPdfAtSurface(pSPF, ..., iorStack)` against the LIVE stack.  Two
production materials really do vary with it, measured directly in the
test's premise checks:

```
PolishedSPF::Pdf     stack-top 1.0 -> 0.266774 ,  stack-top 1.5 -> 0.296169
TranslucentSPF::Pdf  not-in-stack  -> 0        ,  in-stack      -> 0.185406
```

Row (f) puts an albedo-1 furnace material whose sampling density depends
on the stack at a vertex inside a dielectric (entry stack top 1.5):

| | guiding OFF | guiding ON |
|---|---|---|
| round-2 HEAD | 0.516268 vs L_env 0.6 (**−14.0 %**) | 0.511380 (**−14.8 %**) |
| round 3 | 0.599805 (−0.03 %) | 0.597713 (−0.38 %) |

The guiding-OFF control failing is the part §5 had got wrong: this is not
a guiding-specific divergence, because the escape side's density is
produced under the live stack whether or not guiding is on.  The fix is
therefore NOT gated on the blend being active —
`EvaluateDirectLighting{,NM}` take a trailing `const IORStack*
pMisIorStack` and PT (both the Pel/NM PART 2 site and the HWSS NEE site)
and `DirectLightingShaderOp` pass their live stack.  `MediumTransport`'s
volume NEE and PT's two BSSRDF entry NEE sites keep the null default with
a stated reason: `MediumScatterMaterial::Pdf` forwards to the phase
function and the BSSRDF continuation's density is a plain cosine pdf, so
neither reads a stack at all.

**Consequence, stated rather than buried**: a guiding-OFF render of a
material whose aggregate `Pdf()` reads the IOR stack changes — it was
mis-weighted before.  Every other render is byte-identical.

### 8.3 P1, second instance — PT's OWN in-loop volume vertex

The gate found what the consumer sweep alone would not have: a third
PRODUCER.  `IntegrateFromHitTemplated`'s own medium-scatter branch (the
`bsdfPdf = effectivePdf; continue;` site) sets the sampling density and
falls straight back into the same loop's env-escape block -- and round 2
did not give it a `bsdfMisPdf` assignment, so the escape after a volume
scatter weighted against whatever the PREVIOUS vertex had left there.

It is reached only AFTER a surface bounce (a camera ray's first medium
interaction is handled by `IntegrateRayTemplated`'s separate walk, which
carries its own `walkPdf` and has no guiding block), so the stale value
is the surface's BSDF pdf -- or, when the surface bounce was the camera
ray's first hit, **0**, which under §2.4's rule means "no partner
exists" and hands the escape FULL weight on top of an already-weighted
volume-NEE sample.

`VolumeEnvFurnaceTest` is the guard, and it is a white furnace, so the
error is read directly:

| `VolumeEnvFurnaceTest` | §7 floor RGB PT | suite |
|---|---|---|
| `ddf05c6c` (before this slice) | 0.992665 (−0.73 %) | 29/0 |
| `2ebcaff9` (after round 2) | 1.09604 (**+9.60 %**) | 26/3 |
| round 3 | 0.993353 (−0.66 %) | 29/0 |

Bisected to the file by rebuilding `ddf05c6c` with only
`PathTracingIntegrator.cpp` (plus its compile dependencies) taken from
`2ebcaff9`: 1.09574 (+9.57 %), 26/3.  §6 (fog box, no floor) passes
throughout, which is the signature that pointed at the surface-bounce
entry.

Fix: `bsdfMisPdf = phasePdf` at that site (DL-73's ruling -- the NEE call
a few lines above weights through `MediumScatterMaterial::Pdf`, which
forwards to `IPhaseFunction::Pdf`), with `bsdfPdf = effectivePdf`
unchanged.  Its `bsdfTimesCos` was ALSO stale -- the previous vertex's,
i.e. a moment whose numerator and denominator come from different
vertices -- and is now cleared to zero, the conservative state DL-72
round 2 established for sites whose correct quantity is not wired; wiring
it needs a paired `AccumulateCount` here too and is filed as **DL-84**.

Refuted siblings: the other two `volumeBounces++` sites
(`IntegrateRayTemplated` and `IntegrateRayHWSS`) are camera-ray walks
that carry a local `walkPdf` used for both roles and have no guiding
block, so neither can diverge.

### 8.4 Why the row-(f) furnace uses a decorator SPF

Rows (f) target a CLOSED FORM (`L_out == L_env` for an albedo-1 surface
under a constant environment), which needs a material whose directional
albedo is exactly 1.  Neither `PolishedSPF` nor `TranslucentSPF` is
exactly energy-conserving, so neither has a closed-form furnace value,
and their unguided readings cannot serve as a reference either (DL-67:
a multi-lobe material's unguided escape side stores the SELECTED lobe's
pdf while NEE uses the aggregate).  The decorator keeps the albedo-1
Lambertian BRDF and varies ONLY the stack-dependence of the sampling
density — a uniform hemisphere about a TILTED axis when the stack top is
1.5, the real cosine `LambertianSPF` otherwise.  Both are legitimate
sampling densities, so the closed form is exact; and the wedge where the
tilted hemisphere does not cover the upper hemisphere is the
`p_aggregate == 0` region — which round 3 believed was handled
symmetrically by §2.4's old gate and round 4 measured as a double count
(rows (h) and (i), §9.2).  The production materials' stack-dependence is asserted
separately and deterministically (the premise block quoted above), so
the decorator stands in for measured behaviour rather than for a guess.

## 9. Round 4 (2026-09-14, review round 4 of the same slice)

Round 4 found **no P1**.  What it found was one latent asymmetry, one
live partition defect at the edge of the design (`p_aggregate == 0`), two
documentation overclaims, and one flaky suite quoted as green.

### 9.1 P2-1 — the fourth HWSS delegation did not forward the partner

`IntegrateFromHitHWSS` hands off to `IntegrateFromHitNM` at four places:
the two HWSS-ENTRY fallbacks (no BSDF at the first hit; SSS at the first
hit), the mid-path "entered a dielectric" delegation, and the mid-path
SSS delegation.  Round 3 gave the first three the incoming `bsdfMisPdf`;
the fourth kept the default -1, i.e. "the partner equals `bsdfPdf`".

Unreachable today WITH A DIFFERING VALUE: the only producer that splits
the two densities is `RayCaster`'s volume phase-scatter continuation, and
reaching this site from it needs a guided medium vertex whose
continuation lands on a diffusion-profile / random-walk SSS surface in
the MIDDLE of an HWSS walk.  Nothing in `scenes/` builds that, so there
is no behaviour change and none is claimed.  Forwarded anyway
(`0860f78f`) so the next producer that splits the fields inherits the
right answer at all four sites instead of three.

### 9.2 P2-2 — a zero AGGREGATE pdf is not a zero MIXTURE pdf

The defect is §2.4's old gate, quoted in full there.  Rounds 2 and 3
treated `p_aggregate(w) <= 0` as "the BSDF-sampling technique never
generates `w`" and handed the whole sample to NEE.  Under guiding that is
false: the continuation is drawn from a mixture whose guide term reaches
directions the material's own sampler cannot, and
`PathTracingIntegrator`'s one-sample branch evaluates `bsdfPdfGuided`
purely as a mixture component and traces the guided direction whenever
`combinedPdf > NEARZERO` — which `alpha*guide` alone satisfies.  Both
halves of the partition then returned "no partner" and both took weight
1, so that whole region's energy was counted **twice**.

Reachable set in production: any direction with a nonzero BSDF value but
zero aggregate SAMPLING density.  That includes the four full-sphere
BSDFs (hair, fabric, weave, translucent), whose NEE hemisphere rejection
is deliberately disabled (`bFullSphere` in `LightSampler.cpp`), and any
partial-support lobe.

Fix (`145c9259`): `PTGuidingMisPdf::Eval` returns the true mixture
`alpha_nom*guide(w) + (1-alpha_nom)*max(p_agg,0)` whenever guiding is
ACTIVE, and passes `aggregatePdf` through untouched when it is not — so
"no partner exists" survives exactly where it is correct (delta lobes; no
guiding).  The four NEE arms call the hook OUTSIDE their `pdf > 0` gates
and gate the weight on the BLENDED value.  `IGuidedNEEPdfBlend`'s
contract now states that `rawPdf` MAY be zero and that the
implementation, not the caller, decides what that means.  With guiding
off `pGuidedBlend` is null at every call site, so nothing changes.

**Red-proof** — two new rows in `tests/PTGuidingMISPartitionTest.cpp`
(`0beb7885`), one per MIS pair, built on the tilted-lobe `StackAwareSPF`
row (f) already uses.  Inside the dielectric that SPF samples uniformly
over a hemisphere tilted 45 degrees off `N` and its `Pdf` is EXACTLY zero
outside it, while its BRDF stays the albedo-1 Lambertian over the whole
upper hemisphere; the lune between the two is the wedge `W`.

| row | pair | expected | pre-fix | post-fix |
|---|---|---|---|---|
| (h) | env-NEE vs the env ESCAPE weight, guide aimed into `W` | `L_env` = 0.6 | 0.66341 (**+10.57 %**) | 0.597905 (−0.35 %) |
| (h) CONTROL | same, guiding OFF | 0.6 | 0.600058 | 0.600058 |
| (i) | area-NEE vs the EMITTER-HIT weight, emitter entirely inside `W` | `L_e*(R/d)^2*cos(beta)` = 0.0254648 | 0.0510197 (**+100.35 %**) | 0.0254577 (−0.03 %) |
| (i) CONTROL | same, guiding OFF | 0.0254648 | 0.0255986 | 0.0255986 |

The predicted pre-fix excess is `W`'s cosine-weighted share of the upper
hemisphere, `(1 - cos 45)/2 = 14.6 %` for (h) and 100 % for (i) (the
emitter lies entirely inside `W`, so its whole contribution is the
doubled part).  Row (i) lands on that to 0.35 %.  Row (h) reads +10.57 %
at its 160k samples and +12.23 % at 640k — an under-converged estimate of
the ≈13.7 % that the guided branch's `combinedPdf > NEARZERO` gate leaves
reachable (14.6 % is the closed form BEFORE that truncation; review round 5
measured the reachable share at 0.1369 by quadrature), because the guide
is a narrow `cos^64` lobe inside a
45-degree-wide lune, so the pre-fix escape side's own estimate of the
wedge integral is heavy-tailed.  Both CONTROL rows — same material, same
geometry, guiding OFF, where the wedge really is unreachable by the BSDF
technique and weight 1 is correct — pass before and after, which is what
shows the two failing rows measure the blend and not the harness.

Row (i)'s geometry is chosen so the closed form survives: angular radius
`asin(1/5) = 11.5 deg` at `beta = 60 deg` from the normal, which clears
the surface horizon by 18.5 degrees and the wedge's own boundary by 15,
so the projected solid angle is exactly `PI*sin^2(alpha)*cos(beta)`.

### 9.3 P3(a) — `ApplyCosineProduct` moved the SAMPLING distribution too

§2.5 hoisted `InitDistribution` + `ApplyCosineProduct` above PART 2 and
applies the cosine product UNCONDITIONALLY, where PART 3 had applied it
only for an `eRayDiffuse` lobe.  The stated motive was the shared nominal
MIS density, but `guideDist` is ALSO the object PART 3 samples from —
`rc.pGuidingField->Sample(guideDist, xi2d, guidePdf)` — so for a GLOSSY
lobe under guiding this changes the proposal distribution, not only the
weight.

That is unbiased: the guided throughput divides by `combinedPdf`, which
is built from `guidePdf` read back from the same product-applied
distribution, so the estimator's denominator matches its proposal at
every direction.  It is a VARIANCE change, in a direction that depends on
the scene — guide-times-cosine is the physically motivated factorisation
for surface reflection, but on a narrow glossy lobe the cosine factor
does not describe where the BSDF's own mass is.  No measurement of that
variance effect was taken in this slice, and none is claimed; the rows
above measure only the expectation (row (c), whose lobe is tagged
`eRayReflection`, is closed-form-green before and after).

### 9.4 P3(b) — clay_lights: reviewed and REFUTED

Round 4 raised: "PART 2 uses `pClayMaterial->Pdf` while PART 3's
`misBsdfPdf` uses the real SPF, so DL-74's invariant does not hold in
clay mode."  It does hold.  Read end to end:

* PART 3's aggregate pdf is `PTEvalPdfAtSurface<Tag>(pSPF, ...)`, and
  `pSPF` is acquired as
  `EffectivePathTracingClayOverride(rc, mClayOverride) ? pClaySPF : ri.pMaterial->GetSPF()`
  — under clay_lights it is **`pClaySPF`**, not the authored SPF.
* `ClayNEEMaterial` deliberately does NOT override `Pdf`/`PdfNM`, and
  `IMaterial::Pdf` (Materials/IMaterial.cpp) is
  `GetSPF()->Pdf(ri, wo, ior_stack)`; `ClayNEEMaterial::GetSPF()` returns
  the same `pClaySPF` instance.

So both sides evaluate `pClaySPF::Pdf` on the same direction and the same
IOR stack.  No change made; recorded here so the next reviewer does not
re-raise it.

### 9.5 P3(c) — the legacy shader chain still does not partition

Corrected in §6's `DirectLightingShaderOp.cpp` row.  Round 3's wording
there claimed its `ior_stack` fix made "the legacy chain's NEE arm and
`EmissionShaderOp`'s weight evaluate one function of direction".
`ReflectionShaderOp`, `RefractionShaderOp`, `DistributionTracingShaderOp`
and `FinalGatherShaderOp` all continue with a DEFAULT-CONSTRUCTED
`IRayCaster::RAY_STATE rs2;` — `bsdfPdf = 0`, `bsdfMisPdf = -1`,
`MisPartnerPdf() == 0` — so `EmissionShaderOp` weights every emitter hit
reached through them at 1.  What round 3 bought is that the legacy NEE
arm is self-consistent with its own vertex's sampling density.  Making
that chain partition means teaching those four ops to populate a
`RAY_STATE`; not in this slice, and not claimed.

### 9.6 P2-4 — `VolumeAbsorptionAttenuationTest` was quoted as 89/0 and is flaky

§7's round-3 gate quotes `VolumeAbsorptionAttenuationTest` 89/0.  A
reviewer saw 86/3 once in twelve runs: row **[R]** (RGB coloured absorber
with an omni light, equiangular-MIS regime, 1024 spp) over by ~9.4 % in
every channel, observed optical depth 0.310 against the authored 0.400.

By derivation this slice cannot touch that row — guiding off, a delta
refractor, `scattering 0`, `pathtracing_pel_rasterizer` — and the
measurement below confirms the row is unbiased.  What it is, is
**under-averaged for the tolerance it asserts**.

Reproduction on this HEAD: 48 serial runs, all 89/0.  Under CPU
contention (four concurrent instances), **2 failures in 41 runs**, both
with the reviewer's exact signature:

    R.r  measured=0.729078  expected=0.67032  rel=0.0876572  (observed optical depth=0.315974 vs authored=0.4)
    R.g  measured=0.327553  expected=0.301194  rel=0.087515   (observed optical depth=1.1161  vs authored=1.2)
    R.b  measured=0.0986316 expected=0.090718  rel=0.0872332  (observed optical depth=2.31636 vs authored=2.4)

    R.r  measured=0.735396  expected=0.67032  rel=0.0970822  (observed optical depth=0.307346 vs authored=0.4)

Then 40 renders of that row alone, four-at-a-time:

| | mean | vs closed form | sd (relative) | range |
|---|---|---|---|---|
| r | 0.674237 | +0.58 % | **3.13 %** | −5.91 % .. +7.96 % |
| g | 0.302915 | +0.57 % | **3.13 %** | −5.92 % .. +7.94 % |
| b | 0.091213 | +0.55 % | **3.13 %** | −5.94 % .. +7.92 % |

The row's gate is `kRelTol = 0.08`, i.e. **2.6 sigma** on its own
distribution — a ~1 % per-run failure rate, which is what 2-in-89 is.
The mean is within 0.6 % of the closed form, so nothing is biased; and
the per-channel sd agrees to four significant figures, i.e. the
run-to-run variation is a pure MULTIPLICATIVE scale on the whole
measurement rather than a coloured firefly.  That rules out the
heavy-tail-estimator reading as much as it rules out a transport defect:
**no ledger row is filed.**

Why a test that calls `std::srand` varies between runs at all:
`std::srand` fixes the SEQUENCE, not the ASSIGNMENT.  RISE's render
workers draw their per-item seeds from the unsynchronized libc `rand()`,
so which worker gets which seed is a function of thread scheduling —
which is why contention is what surfaced it, and why the observed values
cluster into a handful of repeated outcomes rather than spreading
smoothly.

Fix (fixture-level): row [R] is repeat-averaged over 4 renders through a
new `RenderCentralBlockAveraged` helper; `RenderCentralBlock` already
does `std::srand(g_renderSeed++)` per call, so the repeats are
independently seeded — the `rise-render-seeding` convention.  Re-measured
the same way (36 runs, four-at-a-time):

| | mean | vs closed form | sd (relative) | range |
|---|---|---|---|---|
| r | 0.677147 | +1.02 % | **2.09 %** | −4.33 % .. +4.83 % |
| g | 0.304223 | +1.01 % | **2.09 %** | −4.34 % .. +4.81 % |
| b | 0.091607 | +0.98 % | **2.09 %** | −4.36 % .. +4.79 % |

sd falls by 1.50x (not the ideal 2.0x — the four repeats share one
process's `rand()` lineage and thread pool, so they are not fully
independent), the gate becomes ~3.8 sigma, and the worst of 36 loaded
runs sits at 60 % of the tolerance.  Runtime cost: 10.1 s -> 10.8 s for
the whole suite.  Tolerance unchanged at 8 %.

Not done, and deliberately: the neighbouring omni rows ([Q], and the
heterogeneous-medium rows) are in the same regime and presumably carry a
similar spread, but none has been observed to fail and none is quoted in
a gate — observed-need gated, like any other speculative hardening.
