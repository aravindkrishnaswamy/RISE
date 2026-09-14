# DL-74: the guided pdf had two jobs and one home

Status: **CLOSED 2026-09-14** (debt-guiding2 slice, review round 2).
**DL-83 (the RIS-mode residual) closes with it** — the repair's nominal
density does not depend on the guiding mode, so there was nothing
mode-specific left to leave open.

Red-proof: `tests/PTGuidingMISPartitionTest.cpp`, 22 checks / 0 failures;
red on `8fcce0bf` with 4 of its 6 measurement rows failing by −66 % to
+63 %.

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

`p_mis` is evaluated **inside** each side's existing "the material pdf is
positive" gate:

- **NEE arms** (`LightSampler.cpp`, both env arms and both area-light
  arms, RGB and NM): the raw aggregate pdf is computed first; if it is
  zero the blend is skipped entirely and the historical unweighted
  behaviour stands.
- **Escape / emitter hit** (`PathTracingIntegrator.cpp`): a delta lobe
  keeps `bsdfMisPdf = 0`, and a non-delta lobe whose aggregate pdf
  evaluates to zero at the traced direction also yields 0. `w_bsdf` is
  then 1.

Both sides therefore agree on a single rule: **`p_mis(w) = 0` means no
BSDF-side partner exists at `w`, and the light-sampling sample is taken
whole.** That is correct for a delta lobe (NEE cannot sample through a
Dirac, and the delta contribution is unreachable by NEE), and it is the
only safe answer where `p_aggregate(w) = 0` — there, the BSDF strategy
genuinely cannot generate `w`, so any weight below 1 on the NEE side
would discard energy nothing restores.

Without this gate the repair would have introduced a new defect of its
own: a `polished_material` with a delta coat reaches PART 2, its
aggregate pdf can be zero at the NEE direction, and a nonzero
`alpha·guide(w)` would have scaled that NEE sample down while the escape,
having selected the delta lobe, kept full weight.

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
| **Env NEE, RGB + NM** (`LightSampler.cpp`) | Fixed. Blend inside the `pBsdf > 0` gate. |
| **Area-light NEE, RGB + NM** (`LightSampler.cpp`) | Fixed — the same defect, unfiled before this round. Its BSDF-side partner (PT's emitter-hit weight) was already guided. |
| **Escape weight** (PT's own env-miss arm, and `RayCasterEnvEscapeMISWeight` for every `RayCaster` continuation) | Fixed: weight from the nominal density, training from the true one. |
| **Emitter-hit weight** (PT PART 1) | Fixed, same split. |
| **RIS mode** (DL-83) | Closed here. The nominal density is mode-independent; there is no RIS-specific quantity to reproduce. |
| **Volume vertices** | Fixed as the two-role split (§2.6). DL-73's "the MIS partner stays the raw phase pdf" ruling is preserved exactly and is now explicit in its own field. |
| **HWSS** (`IntegrateFromHitHWSS`) | NOT a sibling — confirmed by reading, not assumed. `effectiveBsdfPdf` there is assigned once from `pS->isDelta ? 0 : pS->pdf` and never reassigned; the function has no guiding block. Both its sides already used the raw material pdf. The stale comment claiming otherwise is corrected in place. |
| **BSSRDF / RW-SSS entry NEE** | Not wired. The entry material's `Pdf()` is the BSSRDF's own cosine density and its continuation's `rs2.bsdfPdf = bssrdf.cosinePdf` is never guiding-adjusted anywhere, so both sides already agree. Both fields are set to the same value there, explicitly. |
| **BDPT / VCM / MLT** | Do not call `LightSampler::EvaluateDirectLighting{,NM}` at all. BDPT's own guiding/NEE consistency is a structurally separate code path, out of scope. BDPT's one `RAY_STATE` producer (a training probe cast) sets both fields to the same value. |

## 5. Known residuals (not filed as new rows)

- **The IOR stack passed to the aggregate pdf differs by side.** The NEE
  arms evaluate `pMaterial->Pdf(w, ri, IORStack(1.0))` — a pre-existing
  convention, unchanged by this row — while the escape side evaluates
  `PTEvalPdfAtSurface` against the live `iorStack`. For any material
  whose aggregate pdf depends on the stack (nested dielectrics), the two
  `p_aggregate` values can differ. This predates DL-74, is orthogonal to
  guiding (it is equally present with guiding off), and is not made worse
  by anything here.
- **Aggregate vs selected-lobe pdf with guiding OFF.** With no guiding,
  the escape side still stores the SELECTED LOBE's `pS->pdf` while NEE
  uses the aggregate. At a multi-lobe material those differ. This is
  DL-67 territory (the same measure inconsistency, from the throughput
  side) and is deliberately left exactly as it was: the repair changes
  the escape side's density only where guiding is active, so a
  guiding-off render is byte-identical.

## 6. File status

| File | Status |
|---|---|
| `src/Library/Interfaces/IRayCaster.h` | `RAY_STATE` gains `bsdfMisPdf` (default -1) and `MisPartnerPdf()`; `bsdfPdf`'s doc now states its single remaining role. |
| `src/Library/Lights/LightSampler.h` | `IGuidedNEEPdfBlend`'s contract rewritten around the nominal-density design; the `rawPdf > 0` precondition is part of it. |
| `src/Library/Lights/LightSampler.cpp` | All FOUR NEE arms (env and area-light, RGB and NM) blend through the hook, inside their `pdf > 0` gates, gated `!isVolumeScatter`. |
| `src/Library/Shaders/PathTracingIntegrator.cpp` | `PTGuidingMisPdf` (replaces `PTGuidedNEEPdfBlend`); the distribution is initialised once above PART 2 and shared; PART 3 no longer re-initialises, re-draws or re-applies the cosine product; `misBsdfPdf` computed at the continuation and carried to the emitter-hit and env-escape weights; BSSRDF, SPF-only and HWSS sites set both fields explicitly; three stale comments corrected. |
| `src/Library/Rendering/RayCaster.cpp` | `RayCasterEnvEscapeMISWeight` weights from `MisPartnerPdf()` and trains from `bsdfPdf`; both volume continuations split the two roles (DL-72 P2-5). |
| `src/Library/Shaders/BDPTIntegrator.cpp` | Its one `RAY_STATE` producer sets both fields. |
| `tests/PTGuidingMISPartitionTest.cpp` | Added — the red-proof. |
| `docs/DEBT_LEDGER.md` | DL-74 and DL-83 struck CLOSED; Counts updated. |

## 7. Gate

`PTGuidingMISPartitionTest` 22/0 · `PTGuidedSelectProbTest` ALL PASSED ·
`TranslucentIORStackTest` ALL PASSED · `RayCasterEnvEscapeMISTest` 91/0 ·
`OptimalMISAccumulatorTest` 34/0 · `MISWeightsTest` 59/0 ·
`SobolDimensionBudgetTest` ALL PASSED · `SSSRadianceScalingTest` 574017/0 ·
`OptimalMISTrainingSitesTest` 7/0 · `EnvLightBalanceTest` 116/0 ·
`BDPTStrategyBalanceTest` 66/0 · `AgentLiveCommitTest` 884/0 ·
`VCMStrategyBalanceTest` 54/1 then 55/0 on an immediate re-run — the one
failure is a p99 TAIL statistic on a thin-lens VCM scene and nothing in
this row touches VCM, thin-lens sampling or any tail; renders seed from
an unsynchronized libc `rand()` (see CLAUDE.md), so this suite's tail
checks are seed-sensitive. Clean `make -C build/make/rise clean && make
-C build/make/rise -j8 all`, zero warnings.
