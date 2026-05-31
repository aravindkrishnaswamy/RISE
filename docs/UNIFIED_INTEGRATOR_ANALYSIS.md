# Unified Integrator Analysis Plan

**Status:** Plan of analysis — no implementation decisions yet.
**First written:** 2026-05-07.
**Revised:** 2026-05-27 — folds in the spectral parity audit outcomes
(2026-05-07 / 2026-05-24), the env-IBL Path A+B fix (2026-05-25), the
Rec.709/D65 colour-space migration (2026-05-24), and the BDPT HWSS
companion-wavelength zeroing fix.  Several "gaps" the prior revision
listed were either matrix errors or have shipped; the candidate
inventory and end-state evaluation are updated accordingly.
**Goal:** Determine whether RISE can converge on a single integrator (or
near-single integrator) that produces low-variance images quickly across
the widest practical range of scenes, while staying physically based.
**Read first:** [RENDERING_INTEGRATORS.md](RENDERING_INTEGRATORS.md)
(matrix corrected 2026-05-07),
[SPECTRAL_PARITY_AUDIT.md](SPECTRAL_PARITY_AUDIT.md),
[MIS_HEURISTICS.md](MIS_HEURISTICS.md),
[MLT_POSTMORTEM.md](MLT_POSTMORTEM.md),
[INTEGRATOR_REFACTOR_PLAN.md](INTEGRATOR_REFACTOR_PLAN.md),
[SMS_FUTURE_DIRECTIONS.md](SMS_FUTURE_DIRECTIONS.md),
[VCM.md](VCM.md), and [IMPROVEMENTS.md](IMPROVEMENTS.md) §12 (the
env-IBL SA-MIS refactor).

This document defines **what we will measure and analyze** before
deciding what to build. It is deliberately not an implementation plan;
the implementation plan is the *output* of the analysis.

---

## 1. Goal & explicit constraints

### Primary goal

A single integrator (or a smoothly composable family of strategies under
one integrator) that delivers **low variance per unit wall-clock time**
across the full RISE scene corpus — both the regression scenes under
[scenes/Tests/](../scenes/Tests/) and the showcase scenes under
[scenes/FeatureBased/](../scenes/FeatureBased/).

"Low variance" is measured against a high-quality reference per scene
(typically PT or BDPT at very high SPP, with VCM as a reference *only*
where PT/BDPT cannot reach the transport).

### Hard constraints

| Constraint | Implication |
|---|---|
| **Physical basis preferred** | Strictly unbiased > consistent (asymptotically unbiased) > bias-bounded. VCM's finite-radius merging is consistent but biased; this is a first-class consideration. |
| **Spectral parity required** | The spectral parity audit landed most quick wins in May; the remaining gaps (VCM-spectral per-wavelength photon store, PT-spectral inline AOV) are now refactor-blocked or research-blocked rather than feature-blocked.  See §3.4 and §7.1. |
| **Refactor latitude granted** | Phases 2b (PT) and 2c (BDPT) of the integrator refactor remain deferred. The spectral parity audit explicitly flags Phase 2b as a precondition for closing PT-spectral inline AOV without doubling-down on NM/HWSS duplication. |
| **Research-territory work permitted** | Optimal MIS extensions to BDPT/VCM, ReSTIR-PT for offline, Specular Polynomials. |
| **CPU-tile-based** | No GPU/wavefront pivot in scope. |

### Non-goals (unchanged)

- ERPT, MMLT, PathMLT, RJMCMC, delayed-rejection MLT.
- Neural BSDFs / NIS / NeRF.
- Polarization.
- GPU pivot.

---

## 2. Bias-class taxonomy

| Class | Definition | Examples in RISE today |
|---|---|---|
| **Strictly unbiased** | E[estimator] = exact integral at any finite N | PT, BDPT, MIS-weighted estimators on either, SMS in unbiased mode |
| **Consistent** | E[estimator] → exact integral as N → ∞ on the prescribed schedule; biased at any finite N | VCM (radius shrinks per SPPM), MLT (transient chain bias), SMS in biased mode, ReSTIR (resampling bias bounded ∝ 1/M, often negligible) |
| **Bias-bounded** | Bias is a quantifiable output-stage transform | OIDN denoising, the env-IBL Path A+B workaround (documented 15-22% mean underestimate vs PT) |

**Decision rule:** any candidate that places the *default* render in the
consistent class needs an explicit unbiased-mode toggle and must be
defensible against strictly-unbiased alternatives at equal wall clock.
VCM-as-default does not pass that test under the user's preference;
whether VCM-with-guiding-and-optimal-MIS passes is the question
Phase 1 measurement will answer.

**Note added 2026-05-27:** the env-IBL Path A+B workaround
([IMPROVEMENTS.md](IMPROVEMENTS.md) §12) puts BDPT and VCM in the
**bias-bounded** class for env-IBL scenes specifically until the SA-MIS
refactor lands. The bound is well-characterized (~15-22% mean
underestimate vs PT on env-only scenes; smaller on mixed env+explicit).
For env-IBL-dominant scenes this is a *current* bias-class downgrade
even for BDPT — which sharpens the case for prioritizing the SA-MIS
refactor before broader integrator unification.

---

## 3. Current-state recap (revised 2026-05-27)

### 3.1 Integrators shipped

| Integrator | Bias class | Algorithmic strengths | Algorithmic weaknesses (post-May-2026 updates) |
|---|---|---|---|
| PT | Unbiased | Diffuse interreflection, well-importance-sampled scenes, full optional-feature stack on the Pel side | SDS, hard caustics, small/occluded lights |
| PT+SMS | Unbiased (snell + unbiased mode); biased (biased mode) | PT regime + smooth dielectric refraction caustics | Heavy-displaced caustics, k≥3 chains, reflection-only specular chains |
| BDPT | Unbiased *except* env-IBL bias-bounded ~15-22% until SA-MIS refactor lands | Area-source indirect, glossy interreflection, partial portals. **Spectral now has full path guiding + adaptive sampling** (was partial / ✗ in prior analysis) | Caustics; pure-diffuse overhead vs PT; env-IBL residual gap; spectral optimal-MIS hard-rejected at parser (open research) |
| VCM | Consistent + env-IBL bias-bounded + spectral merge correctness gap | Strictly subsumes BDPT connections; SDS via merging; caustics | No path guiding (architectural); no optimal MIS (architectural); no SMS; spectral merge uses luminance proxy on Pel-only photon store; no media-aware connection transmittance (`VCMIsVisible` is binary); biased at finite N |
| MLT (PSSMLT) | Asymptotically unbiased (transient chain bias) | Sparse paths in narrow regimes | Per-pixel convergence; opts out of feature matrix |
| `pixelpel` (legacy) | Unbiased (PT-shader-op) | Custom shader-op chains | Throughput |

### 3.2 Optional-feature matrix — as it stands today

From [RENDERING_INTEGRATORS.md](RENDERING_INTEGRATORS.md) §4, corrected
2026-05-07 against parser source.  The diff vs the May-7 analysis is
significant — three rows flipped:

| Feature | Pel side wired | Spectral side wired | Notes |
|---|---|---|---|
| Path guiding (OpenPGL) | PT ✓, BDPT ✓ | PT ✗, BDPT ✓ (full, 2026-05-07) | PT-spectral was a matrix error; never wired. BDPT-spectral was "partial" (missing 4 params); now full. |
| Adaptive sampling | PT ✓, BDPT ✓, VCM ✓ | PT ✓, BDPT ✓ (2026-05-24), VCM ✓ | BDPT-spectral closed 2026-05-24 + latent bias bug fix. |
| Optimal MIS | PT only (`PixelBasedPelRasterizer`, `PathTracingIntegrator`) | None | BDPT-pel was a matrix error (parsed-never-consumed); parser now hard-rejects on BDPT/VCM/PT-spectral. Open research per [MIS_HEURISTICS.md](MIS_HEURISTICS.md). |
| SMS | PT only | PT only | BDPT-SMS excised 2026-05-07; VCM never wired (merging handles caustics). |
| OIDN (full inline AOV) | PT ✓, BDPT ✓, VCM ✓ | PT ✗ (Phase 2b), BDPT ✓ (post-2026-05-07 fix), VCM ✓ (post-2026-05-07 fix) | BDPT/VCM-spectral now use `BSDF->albedo(rig)` against helper-populated RIG (matches Pel). PT-spectral still uses retrace fallback. |

The **net** shift since the May-7 revision: BDPT-spectral became
substantially feature-complete, several "gaps" reduced to matrix
errors, and the remaining gaps cleanly partition into:

1. **Refactor-blocked**: PT-spectral inline AOV (needs Phase 2b).
2. **Architecturally blocked / research-territory**: VCM path guiding, VCM optimal MIS, BDPT optimal MIS, pixelintegratingspectral modern features.
3. **Architectural with a clear plan**: VCM spectral per-wavelength photon store (2-4 weeks, design in [SPECTRAL_PARITY_AUDIT.md](SPECTRAL_PARITY_AUDIT.md) §3).
4. **Correctness, in-flight workaround**: env-IBL on BDPT/VCM (Path A+B shipped; principled fix is [IMPROVEMENTS.md](IMPROVEMENTS.md) §12).

### 3.3 Env-IBL convergence — new bias surface

The 2026-05-25 Path A+B fix closes most of the previously-broken
env-IBL handling on BDPT/VCM. Empirical (from
[tests/EnvLightBalanceTest.cpp](../tests/EnvLightBalanceTest.cpp), 32×32 /
64 spp, uniform env Le=1.0, Lambertian quad albedo 0.5):

| Topology | PT mean | BDPT (pre) | BDPT (post) | VCM (pre) | VCM (post) |
|---|---|---|---|---|---|
| env-only Lambertian | 0.589 | ~0 | 0.457 | 0.026 | 0.562 |
| env + omni light | 0.601 | 0.012 | 0.512 | 0.012 | 0.512 |
| env + mesh emitter | 0.608 | 0.029 | 0.520 | 0.029 | 0.520 |

Delivery went from 0-5% to 78-95% of PT.  The residual 15-22% is the
disc-area-vs-true-SA mismatch that [IMPROVEMENTS.md](IMPROVEMENTS.md)
§12 closes via the PBRT-v4 measure-aware infinite-light vertex
refactor.  Side notes from the same effort:

- VCM env-NEE still has Tr=1 in any participating medium (`VCMIsVisible` is a binary occlusion test, not the media-aware shadow walk BDPT uses).  Documented as a general VCM gap.
- Env-NEE is currently restricted to env-only scenes.  A 2026-05-26 attempt to generalize via synthetic alias-table entry caused a severe spectral-BDPT regression (env-only delivery 76% → 20%) and was reverted.  Suspected cause: extra `Get1D()` consumed by alias-table selection misaligns Sobol dimensions in the per-wavelength sampling.

These three together — SA-MIS refactor, VCM connection-transmittance,
mixed-scene env-NEE — are the prerequisites for any "single
integrator" answer that includes env-IBL scenes.  They are not
prerequisites for a PT-only answer (PT already handles env-IBL
correctly), so they preferentially favor BDPT-centric or VCM-centric
end states (§6).

### 3.4 Spectral parity status — substantially closed

Updated from [SPECTRAL_PARITY_AUDIT.md](SPECTRAL_PARITY_AUDIT.md):

| Item | Status |
|---|---|
| Matrix correction (3 rows flipped, several footnotes added) | DONE 2026-05-07 |
| BDPT-spectral path-guiding subset → full | DONE 2026-05-07 |
| BDPT-spectral adaptive sampling + latent bias bug | DONE 2026-05-24 |
| BDPT/VCM-spectral OIDN albedo proxy retired | DONE 2026-05-07 |
| AOV `RayIntersectionGeometric` helper-populated (5 sites) | DONE 2026-05-07 |
| Parser hard-rejects silently-ignored optimal-MIS params on BDPT/VCM/PT-spectral | DONE 2026-05-07 |
| `pixelintegratingspectral_rasterizer` formally soft-deprecated | DONE 2026-05-07 |
| VCM-spectral per-wavelength photon store (correctness) | OPEN — design in audit §3, 2-4 weeks |
| PT-spectral inline AOV | OPEN — refactor-blocked on Phase 2b |
| Cross-cutting question 6: Accurate-prefilter mode in BDPT/VCM AOV walk | OPEN — design decision |

The Rec.709/D65 colour-space migration ([COLOR_SPACE_MIGRATION.md](COLOR_SPACE_MIGRATION.md))
**incidentally tidied** the `RISEPelToNMProxy` luminance projection
correctness (now computes Rec.709 Y by construction; was applying Rec.709
weights to ROMM-space values).  The *dispersion-loss* correctness gap on
VCM-spectral merges is unchanged — that's about per-wavelength throughput
projection at merge time, not about which luminance basis the scalar is in.

### 3.5 Recently-completed transport infrastructure (May-7 baseline still applies)

- Light BVH (Apr 2026, 4.78× variance reduction on 100-light corridor).
- Null-scattering volumes (Apr 2026).
- Random-walk SSS (Apr 2026).
- Optimal MIS in PT (Kondapaneni 2019).
- Light-subpath guiding in BDPT (Apr 2026).
- HWSS, Owen-scrambled Sobol, ZSobol.
- VCM with progressive radius shrinkage.

---

## 4. Phase 1 — Empirical scene→integrator mapping

The plan here is unchanged from the May-7 revision **with three
additions** reflecting the new env-IBL state:

### 4.1 Test corpus — additions

Add to the curated subset (~20-30 scenes total):

- **Env-IBL only** (Lambertian + glossy + dielectric receivers): measure the documented 15-22% BDPT/VCM gap on real scenes to confirm `EnvLightBalanceTest`'s 32×32 / 64 spp synthetic result holds at production resolution.
- **Env-IBL + mesh emitter mixed** (e.g. `scenes/FeatureBased/ripple_dreams_fields.RISEscene`): measure the env-NEE mixed-scene restriction's practical impact.
- **VCM connection-transmittance stress** (env-IBL through fog): isolate the VCM `VCMIsVisible` binary-occlusion gap.

These are not optional — env-IBL is widespread in production scenes
and any "uniformly great integrator" claim has to be defensible on it.
The May-7 revision underweighted env-IBL because the Path A+B fix
hadn't surfaced the residual yet.

### 4.2 Metrics — unchanged

Per-scene per-integrator, at multiple wall-clock budgets:

- RMSE vs reference
- Pure variance (K-trial stddev)
- Mean luminance bias (essential for VCM; now also tracks env-IBL bias-bounded)
- 99th-percentile pixel error
- Wall-clock per equivalent SPP
- Memory peak

### 4.3 Bias measurement protocol (revised)

**For VCM:** unchanged — vary radius (0.5×, 1×, 2× auto) and sample count (1×, 4×, 16×) to track shrinkage + convergence.

**For env-IBL on BDPT/VCM:** new — measure mean-luminance bias vs PT reference at convergence on the env-IBL test set. Cross-reference with `EnvLightBalanceTest` synthetic numbers.

**For VCM-spectral merging:** dispersive caustic scenes (`triplecaustic`, water/glass) measured against per-wavelength PT reference to quantify the luminance-proxy dispersion-loss bias.

### 4.4 Phase 1 budget

Estimate unchanged: **2–3 weeks elapsed**. Most time is render time.
Need ~1 week of instrumentation work: env-IBL bias-vs-reference
harness, variance scaffolding script, per-strategy contribution
splatting on BDPT/VCM.

### 4.5 Phase 1 output

`docs/UNIFIED_INTEGRATOR_BASELINES.md` (to be produced). Contains:

- Scene-by-integrator quality matrix (RMSE, variance, bias, wall-clock).
- Bias-premium per scene class for VCM-vs-PT-or-BDPT.
- Env-IBL bias quantification at production resolution.
- VCM-spectral dispersion bias quantification.
- Identification of scenes where *every* integrator has high variance (these are the highest-leverage scenes for the chosen direction).

---

## 5. Phase 2 — Candidate inventory (revised 2026-05-27)

### 5.1 Already-closed since May-7

The following candidates from the May-7 revision are no longer "open
work" — they shipped:

- ~~BDPT-spectral path guiding (partial → full)~~  DONE 2026-05-07
- ~~BDPT-spectral adaptive sampling~~  DONE 2026-05-24
- ~~BDPT/VCM-spectral OIDN AOV albedo proxy~~  DONE 2026-05-07

The following candidates have been **reclassified** from "in scope"
to "hard-blocked" by the audit:

- Optimal MIS on BDPT (Pel + spectral): parser hard-rejects; consumer code never existed. Real fix is open research per [MIS_HEURISTICS.md](MIS_HEURISTICS.md). Bumped to research-territory.
- Optimal MIS on VCM: architecturally blocked by Georgiev recurrence.
- Path guiding on VCM: architecturally blocked by merging-vs-training cooperation. Open research.
- Modern features on `pixelintegratingspectral_rasterizer`: out of scope (soft-deprecated).

### 5.2 New since May-7

#### 5.2.1 Env-IBL SA-MIS refactor ([IMPROVEMENTS.md](IMPROVEMENTS.md) §12)

- **Bias class:** moves BDPT/VCM env-IBL from bias-bounded back to strictly unbiased.
- **What it solves:** the documented 15-22% mean-luminance gap on env-IBL BDPT/VCM. Closes the disc-area parameterization correctness debt by going PBRT-v4-style (env vertex is direction-on-sphere, pdf in solid-angle measure throughout `MISWeight`).
- **Dependencies:** touches `MISWeight` core, `BDPTUtilities::SolidAngleToArea` / `GeometricTerm`, `SampleEnvLightEmission`, `GenerateLightSubpath`, both RGB and NM. VCM's `dVCM/dVC/dVM` recurrence has a parallel env-vertex special case per Georgiev 2012 Appendix A.
- **Effort:** ~10 files, ~300 lines if cleanly refactored.
- **Risk:** **medium-high.** MIS regressions are subtle (RISE has hit MIS-walk bugs before; see [skills/bdpt-vcm-mis-balance.md](skills/bdpt-vcm-mis-balance.md)). Disc-area convention is currently *working* for non-env-terminating paths; touching `MISWeight` risks breaking those.
- **Validation:** strict `EnvLightBalanceTest` tolerances (`0.10 / 0.30 / 1.00`), no regression on the 116 existing tests, visual parity PT/BDPT/VCM on `ripple_dreams_fields.RISEscene`, ≥15% RMSE drop on env-IBL scenes.
- **Why this is now top-of-list:** every candidate end state that includes BDPT or VCM gets a free 15-22% mean-luminance correctness uplift from this. It's a Phase-1 prerequisite for any honest empirical comparison.

#### 5.2.2 VCM media-aware connection transmittance

- **Bias class:** preserves VCM's consistent class.
- **What it solves:** VCM NEE / interior connection / s=0 strategies currently use `VCMIsVisible` (binary occlusion), not a transmittance-aware shadow walk like BDPT's `EvalConnectionTransmittance`. Result: VCM is wrong on env-IBL-through-fog and any participating-medium scene.
- **Effort:** small-to-medium — extend BDPT's `EvalConnectionTransmittance` to be called from the VCM connection sites; preserve the VCM running-quantity recurrence.
- **Risk:** low. Existing reference (BDPT).
- **Coupling:** if 5.2.1 lands, this folds in naturally.

#### 5.2.3 Mixed-scene env-NEE for BDPT-spectral (Sobol-stream issue)

- **Bias class:** would make BDPT/VCM strictly unbiased on env+explicit mixed scenes (currently env contributes via eye-subpath escape only — capping delivery at ~15% deficit).
- **What it solves:** the 2026-05-26 attempt regressed spectral-BDPT badly; suspected cause is an extra `Get1D()` consumed by alias-table selection misaligning Sobol dimensions per-wavelength.
- **Effort:** medium — either (a) dedicated low-discrepancy dimension for env's alias-table draw, or (b) the SA-MIS refactor (5.2.1) eliminates the disc parameterization entirely and removes the need.
- **Recommendation:** sequence after 5.2.1.

### 5.3 Carried over from May-7 revision (status updated)

The literature-track items below are unchanged in scope. Marked
status updates where they exist.

#### Path-guiding extensions

- **Path guiding in VCM** — confirmed architecturally blocked per audit §2.13. Research-territory.
- **Manifold Path Guiding (Fan 2023)** — unchanged. Unbiased. 4–8 weeks. Reference code available.
- **Online Photon Guiding 3DGS (Xu 2024)** — defer.

#### MIS extensions

- **Variance-aware MIS for BDPT (Grittmann 2019)** — unchanged. Unbiased. 8–12 weeks. Reference in Mitsuba 0.6 fork. **Now the only viable path** to optimal-MIS-in-BDPT after the audit hard-rejected the parsed-never-consumed params.
- **Correlation-aware MIS for BDPT (Grittmann 2021)** — unchanged. Unbiased. +4–6 weeks on top of variance-aware.
- **Optimal MIS for VCM** — confirmed architecturally blocked. Bumped to research-only.

#### Specular / manifold methods

- **Specular Polynomials (Mo 2024)** — unchanged. Unbiased. 6–10 weeks. Reference code available. Still the next SMS target per [SMS_FUTURE_DIRECTIONS.md](SMS_FUTURE_DIRECTIONS.md).
- **Bernstein Bounds (Schaufler 2025)** — unchanged. Unbiased. 6–8 weeks. For heavy-displaced caustics if Specular Polynomials leaves a gap.
- **SMBS (Jhang & Chang 2022)** — unchanged. Cheap (~1–2 weeks). Worth doing regardless of larger direction.

#### Path-space resampling

- **ReSTIR-PT for offline (Lin et al. 2022)** — unchanged. The big "uniformly great" lever. 16–24 weeks. High research risk on the CPU-tile execution model.
- **Generalized RIS for BDPT (Bitterli 2022)** — unchanged. +6–8 weeks after ReSTIR-PT.
- **PSMS-ReSTIR (Hong 2025)** — unchanged. +4–6 weeks after ReSTIR-PT.

#### BDPT–SMS reintegration

- Unchanged. Re-evaluate *after* Phase 1 + Specular Polynomials, *if* the "single integrator" direction needs caustics on BDPT.

### 5.4 Out-of-scope (confirmed)

ERPT, MMLT, PathMLT, neural BSDFs, polarization, GPU pivot.

---

## 6. Phase 3 — Synthesis decision frameworks (revised 2026-05-27)

The candidate end states are unchanged in shape, but their relative
attractiveness has shifted with the spectral parity outcomes and the
env-IBL state.

### 6.1 Candidate end-state architectures

#### Candidate A — "Strictly unbiased, BDPT-centric"

```
BDPT
  + env-IBL SA-MIS refactor (5.2.1, makes BDPT strictly unbiased on env-IBL)
  + VCM-borrowed media-aware connection transmittance (5.2.2)
  + path guiding (already shipped, pel + spectral)
  + variance-aware MIS (Grittmann 2019)
  + correlation-aware MIS (Grittmann 2021)
  + SMS reintegrated as connection strategy via Specular Polynomials
  + path-space resampling (G-RIS for BDPT, optional Phase 4)
```

**Bias class:** strictly unbiased everywhere (post-SA-MIS).
**Scope:** SA-MIS (~3 weeks) + Specular Polynomials (~2 months) + variance-aware MIS (~3 months) + correlation-aware MIS (~1.5 months). Full sequence: ~9-10 months. G-RIS add: +6 months.
**Strength:** physically defensible at every step; **spectral path is now substantially in place** (BDPT-spectral has feature parity post-audit). Subsumes what PT+SMS and BDPT do today.
**Weakness:** still doesn't natively cover SDS-through-merging caustic regime; relies on Specular Polynomials + path-space resampling to close that gap (both feasible, neither trivial).
**What's changed since May-7:** the spectral parity audit landed most of the spectral plumbing; the env-IBL gap is now bias-bounded with a known closure. **This is the candidate that gained most.**

#### Candidate B — "VCM-as-default, consistent"

```
VCM
  + env-IBL SA-MIS refactor (5.2.1)
  + media-aware connection transmittance (5.2.2)
  + per-wavelength photon store (spectral parity audit §3)
  + path guiding (novel implementation; architecturally hard)
  + optimal MIS for VCM (open research)
```

**Bias class:** consistent (VCM's finite-radius bias) + spectral merge correctness gap until per-wavelength photons land.
**Scope:** SA-MIS (~3 weeks) + connection transmittance (~2 weeks) + per-wavelength photons (~3 weeks) + path guiding (3-6 months if it works) + optimal MIS for VCM (3-6 months if tractable, possibly unbounded).
**Strength:** strictly subsumes BDPT in transport coverage; SDS / merging caustics handled natively.
**Weakness:** **fails the user's hard constraint on physical basis as default.** Three independent gaps to close (env-IBL, transmittance, per-wavelength photons) before VCM is even *correctness-equivalent* to BDPT on non-caustic scenes. Two more (path guiding, optimal MIS) for performance parity. Spectral merge correctness gap is a real correctness debt, not just a feature gap.
**What's changed since May-7:** **less attractive.** The May-7 revision underweighted the VCM correctness gaps. The audit surfaced (a) the per-wavelength photon store as a real correctness issue documented in [VCM.md](VCM.md):242 not just a v1 limitation, and the env-IBL work surfaced (b) and (c). Three open correctness gaps, plus the path-guiding and optimal-MIS-for-VCM both being confirmed-architecturally-blocked, weakens the case meaningfully.

#### Candidate C — "Hybrid: BDPT default + VCM caustic fallback"

```
BDPT default (with everything in Candidate A above)
  + VCM kept as opt-in for SDS / heavy-displaced caustic regimes only
  + bias clearly disclosed when VCM mode is chosen
  + per-wavelength photon store if spectral VCM is used
```

**Bias class:** strictly unbiased default; consistent only when user opts in to VCM.
**Scope:** Candidate A's scope; VCM stays roughly as-is (plus optional per-wavelength photons for spectral correctness).
**Strength:** matches the hard constraint; reduces total integrator count if PT can be retired (BDPT-with-Specular-Polynomials covers PT+SMS); MLT can be deprecated per the postmortem.
**Weakness:** still two integrators; not a "single integrator" answer.
**What's changed since May-7:** **most attractive overall under the user's constraints.** It's the only candidate that delivers the user's preference set without research-territory bets blocking the critical path. The Candidate-A workstream is the critical path; VCM stays as-is until/unless Phase 1 says merging is wall-clock-necessary on regimes A can't reach.

#### Candidate D — "Path-space resampling-first"

```
BDPT or PT base
  + path guiding (already shipped on PT/BDPT)
  + ReSTIR-PT spatial resampling pass
  + Specular Polynomials
  + variance-aware MIS
  + env-IBL SA-MIS refactor (still needed)
```

**Bias class:** strictly unbiased (RIS is unbiased; bias bound from M is tunable and typically negligible).
**Scope:** dominated by ReSTIR-PT for CPU-offline (~6 months exploratory). Total: ~14-18 months.
**Strength:** **the only candidate that directly attacks per-scene uniformity at the algorithm level rather than per-feature completion.** Highest research-payoff direction.
**Weakness:** highest research risk; CPU-tile execution model unproven for ReSTIR; may produce a great paper and a meh integrator if the cost model doesn't work on RISE's hardware.
**What's changed since May-7:** unchanged. The user's "research-territory permitted" answer keeps this on the table.

### 6.2 Criteria matrix (refreshed)

| Criterion | A (BDPT-centric) | B (VCM-default) | C (Hybrid) | D (ReSTIR-PT) |
|---|---|---|---|---|
| Strictly unbiased default | ✓ | ✗ | ✓ | ✓ |
| Single-integrator goal | partially | ✓ | ✗ | ✓ |
| Spectral parity tractable (post-audit) | **✓ substantially in place** | requires per-λ photons + 2 more gaps | ✓ | ✓ |
| Closes env-IBL gap | via 5.2.1 | via 5.2.1 | via 5.2.1 | via 5.2.1 |
| Closes SDS gap | via Specular Polynomials + path resampling | ✓ natively | via SMS only (gaps remain on heavy-displaced if Specular Polynomials saturates) | via Specular Polynomials |
| Per-scene uniformity | medium-high (with G-RIS add) | medium | medium-high | **high** |
| Engineering scope | 9-10 months critical path | 9-13 months | 9-10 months (= A's critical path) | 14-18 months |
| Research risk | medium | high | low-medium | very high |
| Open correctness debts as of 2026-05-27 | 1 (env-IBL, scoped) | 3 (env-IBL, transmittance, per-λ photons) | 1 | 1 |

### 6.3 Decision gate

After Phase 1 + Phase 2 are complete, hold a focused session that:

1. Reviews the empirical scene→integrator map from Phase 1.
2. Reviews bias-premium-per-scene-class — concretely answers "does VCM's finite-N bias meaningfully exceed the env-IBL Path A+B bound on RISE's scenes?"
3. Evaluates each candidate end-state against the matrix.
4. Picks either a single direction or a sequenced approach (e.g., "C now, evaluate D in 12 months when A's MIS work has landed on BDPT").
5. Produces an *implementation* plan document per the chosen direction.

**Pre-commitment note for the decision gate**: Candidate A's first
two items (5.2.1 SA-MIS, 5.2.2 VCM transmittance) are valuable
regardless of which end state we pick. They could reasonably be
landed *during* Phase 1 measurement, which would let Phase 1 measure
post-fix BDPT/VCM convergence rather than pre-fix (a much more honest
baseline for the Phase 3 decision).

---

## 7. Parallel tracks (revised 2026-05-27)

### 7.1 Spectral parity audit — substantially complete

[SPECTRAL_PARITY_AUDIT.md](SPECTRAL_PARITY_AUDIT.md) is published with
~7 items DONE.  Two items remain:

- **VCM-spectral per-wavelength photon store** (§3, correctness): design in audit §3, cross-cutting question 1 awaiting design input (hero-only vs full-chain). 2-4 weeks.
- **PT-spectral inline AOV**: refactor-blocked on Phase 2b.

The audit's other "open" items (PT-spectral path guiding, VCM path guiding, BDPT optimal MIS) are architecturally / research-blocked and re-classified as Phase 2 candidate inventory in §5 above.

### 7.2 Integrator refactor Phase 2b/2c — still deferred

Per [INTEGRATOR_REFACTOR_STATUS.md](INTEGRATOR_REFACTOR_STATUS.md),
Phases 2b (PathTracingIntegrator) and 2c (BDPTIntegrator) have not
landed.  The spectral parity audit explicitly recommends waiting for
Phase 2b before closing PT-spectral inline AOV — adding more NM/HWSS
duplication contradicts the refactor plan.

**Recommendation strengthened from May-7**: land Phase 2b/2c
*before* Candidate A's feature work (variance-aware MIS, Specular
Polynomials), since both of those would otherwise be implemented
across three NM/HWSS-duplicated code paths in BDPT. The duplication
tax for each new BDPT feature post-refactor is ~zero; pre-refactor
it's a 3× implementation burden.

Sequencing recommendation:

1. (Optional but valuable) Land 5.2.1 (env-IBL SA-MIS) and 5.2.2 (VCM transmittance) — these are correctness fixes that improve any Phase 1 baseline.
2. Phase 2b (PathTracing templatization) — ~3-4 weeks.
3. Phase 2c (BDPT templatization) — ~6-8 weeks.
4. Phase 1 baseline measurement (parallel with Phase 2c).
5. Phase 3 decision.
6. Implementation per chosen direction.

### 7.3 Variance-measurement infrastructure

Unchanged from May-7. Small (1-2 weeks), can be done before Phase 1
starts.

---

## 8. Open questions for the user before Phase 1 starts

Same four questions as May-7 (§8.1-§8.4); none have been resolved.
Brief restatement:

### 8.1 "Wide variety of scenes" — what's the concrete corpus?

(a) existing corpus as-is; (b) curated production-weighted subset;
(c) expanded with scenes you've avoided because today's integrators
fail.

### 8.2 Bias tolerance threshold

How much finite-N bias acceptable in opt-in mode? 1% / 0.1% / 0%?
The env-IBL Path A+B bound (15-22%) is **currently bias-bounded
default** on BDPT/VCM — if 5.2.1 is sequenced before Phase 1, this
issue is moot for Phase 1's measurements. Otherwise we should
explicitly flag env-IBL renders as bias-bounded in any reporting.

### 8.3 Variance metric: SPP-normalized, wall-clock, or time-to-converge?

Default assumption: wall-clock-normalized.

### 8.4 Sequence depth vs breadth

Long single-direction commitment, or shorter "win-something-in-3-months"
cycles? The refreshed Candidate-A scope (~9-10 months critical path)
is shorter than May-7's estimate because spectral parity work is done;
this may shift the answer.

### 8.5 New question — sequence 5.2.1 / 5.2.2 before Phase 1?

The 2026-05-27 audit-and-roadmap update surfaces a sequencing
question that wasn't visible in May-7:

The env-IBL SA-MIS refactor (5.2.1, ~3 weeks) and VCM media-aware
connection transmittance (5.2.2, ~2 weeks) are correctness fixes
that would significantly clean up any Phase 1 baseline. Should they
be sequenced **before** Phase 1 measurement (cost: ~5 weeks slip on
Phase 1; benefit: empirical comparisons are unbiased) or **after**
(cost: Phase 1 baselines need re-running once 5.2.1/5.2.2 land;
benefit: starts measurement immediately)?

Recommendation: **sequence before**. The 5-week slip is much smaller
than the Phase 1 → Phase 3 → implementation total cycle, and the
re-run-after-fix risk is non-trivial (results could meaningfully
re-shape the Phase 3 decision).

### 8.6 New question — Phase 2b/2c sequencing relative to Candidate A

Strengthened from §7.2: should Phase 2b/2c land before Candidate A's
feature work (variance-aware MIS, Specular Polynomials)?

Recommendation: **yes** — each of those features would otherwise be
written 3× (Pel/NM/HWSS), and Phase 2c specifically exists to retire
that duplication.

---

## 9. Anticipated phase outputs

| Phase | Document | Status |
|---|---|---|
| (Pre-Phase 1) | 5.2.1 env-IBL SA-MIS refactor implementation plan | **Design + survey landed 2026-05-27** in [PRE_PHASE1_STATUS.md](PRE_PHASE1_STATUS.md); code execution deferred to follow-up session |
| (Pre-Phase 1) | Phase 2b/2c integrator refactor execution | Plan exists, status doc exists; deferred until Pieces 1+2 land per [PRE_PHASE1_STATUS.md](PRE_PHASE1_STATUS.md) |
| Phase 1 | `docs/UNIFIED_INTEGRATOR_BASELINES.md` (new) | Not yet started; would post-date the pre-Phase-1 fixes if those are sequenced first |
| Phase 2 | This document, §5 (further revised) | Maintained here |
| Phase 3 | `docs/UNIFIED_INTEGRATOR_DECISION.md` (new) | Awaits Phase 1 output |
| Implementation | Per-direction implementation plans | Awaits Phase 3 |

Total elapsed time before implementation plan is finalized, with
recommended pre-Phase-1 sequencing: ~3-4 months (5.2.1 + 5.2.2 +
Phase 2b/2c + Phase 1 + Phase 3). Without pre-Phase-1 fixes: ~2
months, but Phase 1 baselines may need re-running.

---

## 10. What we want from the user before kickoff (revised)

1. Answers to questions §8.1-§8.4 (carried from May-7).
2. New: answer to §8.5 (sequence 5.2.1 / 5.2.2 before Phase 1?).
3. New: answer to §8.6 (sequence Phase 2b/2c before Candidate A feature work?).
4. Pointer to "must render well" scenes from the corpus — env-IBL scenes specifically called out.
5. Any candidate in §5 to rule out up-front.

---

## 11. Cross-references

- [RENDERING_INTEGRATORS.md](RENDERING_INTEGRATORS.md) — selection guide and feature matrix (corrected 2026-05-07)
- [SPECTRAL_PARITY_AUDIT.md](SPECTRAL_PARITY_AUDIT.md) — spectral parity remediation plan and shipped status
- [IMPROVEMENTS.md](IMPROVEMENTS.md) — current baseline, ranked roadmap, non-goals, and item #12 (env-IBL SA-MIS)
- [MIS_HEURISTICS.md](MIS_HEURISTICS.md) — power vs balance per integrator; why optimal-MIS-on-BDPT/VCM is open research
- [MLT_POSTMORTEM.md](MLT_POSTMORTEM.md) — confirmed-not-worth-doing branch
- [INTEGRATOR_REFACTOR_PLAN.md](INTEGRATOR_REFACTOR_PLAN.md) — Pel/NM duplication and templatization
- [INTEGRATOR_REFACTOR_STATUS.md](INTEGRATOR_REFACTOR_STATUS.md) — Phase 0/1/2a shipped, 2b/2c/3/4 deferred
- [SMS_FUTURE_DIRECTIONS.md](SMS_FUTURE_DIRECTIONS.md) — manifold method candidates
- [VCM.md](VCM.md) — VCM design, spectral merge limitation, optimal-MIS-extension research scope
- [COLOR_SPACE_MIGRATION.md](COLOR_SPACE_MIGRATION.md) — Rec.709/D65 migration that incidentally tidied `RISEPelToNMProxy`
- [skills/variance-measurement.md](skills/variance-measurement.md) — measurement protocol for Phase 1
- [skills/bdpt-vcm-mis-balance.md](skills/bdpt-vcm-mis-balance.md) — diagnostic procedure for cross-integrator disagreement
