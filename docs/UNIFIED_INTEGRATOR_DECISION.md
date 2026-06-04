# Unified Integrator Decision (Phase 3)

**Status:** Phase-3 decision — the synthesis the measurement arc was built for.
**Date:** 2026-06-04.
**Inputs:** the empirical [UNIFIED_INTEGRATOR_BASELINES.md](UNIFIED_INTEGRATOR_BASELINES.md)
matrix (Phase 1, now clean — see "What changed" below), the candidate
end-states + criteria in [UNIFIED_INTEGRATOR_ANALYSIS.md](UNIFIED_INTEGRATOR_ANALYSIS.md)
§6, the per-scene decision tree in [RENDERING_INTEGRATORS.md](RENDERING_INTEGRATORS.md),
and the [INTEGRATOR_BUGFIX_FINDINGS.md](INTEGRATOR_BUGFIX_FINDINGS.md) bug-fix arc.
**Nature:** a data-grounded *recommendation* for the user to ratify; it picks a
near-term direction and a sequenced research overlay per §6.3.

---

## 1. The question, and the answer

The arc began with: *can RISE converge on a single integrator that delivers low
variance fast across the widest array of scenes, while staying physically based?*

**The empirical answer is no — and that is itself the finding.** No single
integrator dominates the wall-clock-normalized variance (σ²·T) across the scene
space. But the per-class winners are clean and stable, so a **small principled
hybrid** does dominate. The decision is therefore **not** "pick the one
integrator" but "pick the smallest correct set and route by transport class."

**Decision (recommended): Candidate C — a PT-default, auto-routed hybrid (PT
baseline for the converged bulk, BDPT routed in for the strong-indirect/glossy
regime, VCM for caustics) — as the near-term direction, with Candidate D
(ReSTIR-PT) sequenced as a research overlay. NOTE: "BDPT-centric" on C means BDPT
is the *enhancement-investment* target (it has the most headroom), NOT the runtime
default — the σ²·T data makes PT the efficient default on the bulk (§2, §4).**

---

## 2. What the measurement established (the clean matrix)

From the Phase-1 matrix (18 scenes × {PT,BDPT,VCM}, K=16, σ²·T + RMSE-vs-truth),
re-validated after the EXR-FP16 reader/writer fixes and the Bug-3 HWSS fix:

- **PT wins σ²·T on the bulk of converged production classes** (diffuse-indirect,
  glossy-metal, mixed showcase, many-light, prism) — its per-sample cheapness
  (3–7× faster than BDPT/VCM) beats their lower raw variance.
- **BDPT wins the strong-indirect / glossy-interreflection regime decisively**
  — `gi_spheres` **56× σ²·T** over PT, `alchemists` too. Where the indirect is
  hard enough, BDPT's connections pay for themselves.
- **VCM wins only the caustic / refractive / dispersive regime** (`pool_caustics`,
  `diamond_teapot`, `torus_chain`, `spectral_caustic`) — and there because it is
  the **only** integrator that reaches the transport (PT/BDPT miss 44–78% of the
  energy), *not* because it is efficient.
- **VCM-as-default is contraindicated twice over:** it loses σ²·T **3–40×** on
  every converged class, and carries **−63%…+76% env/volume luminance bias** at
  production resolution (BDPT stays ≤6.4%). Its strength is coverage, not speed
  or correctness-by-default.

This is the per-class map the decision routes on (refining
RENDERING_INTEGRATORS.md §2 with data): **PT for the cheap-and-converged bulk,
BDPT for strong-indirect/glossy, VCM only for caustics.**

---

## 3. The candidates against the data

| | A — BDPT-centric | B — VCM-default | **C — Hybrid (recommended)** | D — ReSTIR-PT |
|---|---|---|---|---|
| Strictly-unbiased default | ✓ | ✗ | ✓ | ✓ |
| Matches the data's per-class map | partial (no native caustics) | ✗ (loses σ²·T everywhere but caustics) | **✓** | partial (still needs caustics) |
| Physical-basis hard constraint | ✓ | **✗** | ✓ | ✓ |
| Open correctness debts | 1 (env-IBL) | **3** (env-IBL, transmittance, per-λ photons) | 1 | 1 |
| Engineering scope | 9–10 mo | 9–13 mo | **= A's critical path** | 14–18 mo |
| Research risk | medium | high | **low–medium** | very high |
| Per-scene uniformity ceiling | medium-high | medium | medium-high | **high** |

- **B (VCM-default) is rejected by the data** — it is the worst σ²·T on the bulk,
  carries three open correctness debts, fails the user's physical-basis-as-default
  constraint, and its two performance-parity prerequisites (path guiding, optimal
  MIS for VCM) are architecturally blocked. The matrix's −63%…+76% env/volume bias
  is the empirical nail.
- **A and C share the same critical path** — both build out BDPT (env-IBL SA-MIS,
  variance/correlation-aware MIS, Specular Polynomials). They differ only in how
  caustics are covered: A bets on folding caustics into BDPT via Specular
  Polynomials + path-space resampling; **C keeps the already-working VCM as an
  opt-in caustic fallback** while that bet matures. C is strictly less risky for
  the same near-term work, and the matrix confirms VCM is genuinely needed *only*
  on the caustic class A can't yet reach.
- **D (ReSTIR-PT) is the only candidate that attacks per-scene uniformity at the
  algorithm level** — it targets exactly the §7 "every-integrator-noisy" scenes
  (caustic/SDS/spectral-caustic). Highest payoff, highest risk (CPU-tile ReSTIR
  is unproven on RISE's execution model). The user explicitly permitted
  research-territory work, so it stays on the table — sequenced, not on the
  critical path.

---

## 4. The decision

**Near-term direction: Candidate C (PT-default, auto-routed hybrid).**
- **PT is the runtime default/baseline.** The σ²·T data is decisive: PT wins
  wall-clock-normalized variance on **10 of the converged classes** because its
  per-sample cost is **3–7× lower** than BDPT's (37× on homogeneous_fog) — that
  cheapness outweighs BDPT's lower *raw* variance everywhere except the
  strong-indirect regime. A blanket BDPT default would pay that 3–7× penalty on
  the majority of scenes for no σ²·T win (BDPT's σ²·T runs 2.4–338× worse than
  PT's on those classes).
- **BDPT is routed in only for the strong-indirect / glossy-interreflection
  regime** — the 3 classes where its connections overcome the cost (gi_spheres
  56×, alchemists, env_mesh).
- **"BDPT-centric" = the enhancement-investment target, not the default.** Dev
  effort (env-IBL SA-MIS, Specular Polynomials, variance/correlation-aware MIS)
  goes into BDPT because it has the headroom to subsume PT+SMS and the most
  variance upside — which over time *widens* the regime where routing picks BDPT.
  The runtime default stays PT until BDPT's σ²·T actually wins more classes.
- **VCM is kept as the opt-in caustic/refractive fallback** — the matrix shows it
  is necessary there and nowhere else. Its env/volume bias is acceptable *because
  it is no longer a default*; the bias only applies when the user opts into the
  caustic regime where VCM is the only option.
- **MLT is deprecated** (inherits BDPT, adds maintenance surface for no measured
  win on the corpus).

**Research overlay (sequenced, not critical-path): Candidate D (ReSTIR-PT).**
- Re-evaluate in ~12 months, once the BDPT MIS workstream (below) has landed, per
  §6.3's "C now, evaluate D later." Target: the §7 scenes where even the best
  integrator is noisy. Scope it as exploratory (a spike), not a commitment.

This is the **smallest correct set** (PT + BDPT default, VCM caustic fallback)
that matches the data, honors the physical-basis constraint, and keeps the
highest-payoff research direction open without blocking shipping work.

### 4.1 What Candidate C is — and is not — architecturally

**C adds no new integrator algorithm.** PT, BDPT, and VCM stay as the existing
rasterizer types. C is a **default + routing policy** over them, plus an
**enhancement workstream on BDPT** — not a monolithic "hybrid renderer":

- **Default change:** BDPT becomes the recommended/default integrator (new scenes,
  GUI default, docs); PT remains the fast-path where its per-sample cheapness wins
  σ²·T; VCM is the opt-in for caustic/refractive scenes. A scene selects its
  integrator via the rasterizer chunk exactly as today — C changes the *default*
  and the *guidance* for which to pick, not the integrator set.
- **The actual new code is the BDPT enhancements** (env-IBL SA-MIS, Specular
  Polynomials, variance/correlation-aware MIS) — these improve the existing BDPT.
  VCM stays as-is (opt-in); MLT is deprecated.

**Routing mechanism — a spectrum (deferrable):**
1. *Defaults + guidance (minimal, no new code type):* BDPT is the documented/GUI
   default; authors pick VCM for caustic scenes guided by the data-backed
   RENDERING_INTEGRATORS.md §2 map.
2. *Auto-selecting dispatcher (optional convenience):* a thin meta-rasterizer that
   inspects the scene (caustic/SDS/dielectric regime detection) and dispatches to
   PT/BDPT/VCM. A new but thin type — a convenience layer, not core to C; regime
   detection is itself non-trivial and can come later.

**What C is NOT:** a single integrator that internally does BDPT + photon merging
*is* VCM (already exists — BDPT connections + merges + MIS), which is rejected as
the default for its per-sample cost and env/volume bias. An adaptive
"BDPT + merge-only-where-caustics" single integrator is a new algorithm
(research-territory, the D bucket), not C.

---

## 5. Implementation sequencing

The C critical path is the BDPT-centric (Candidate A) workstream — valuable
whether or not D is later pursued:

1. **env-IBL SA-MIS refactor (5.2.1)** *and* **VCM media-aware connection
   transmittance (5.2.2)** — valuable regardless of direction; the matrix
   quantified the VCM env/volume bias these close (−63%…+76%). Land these first;
   they sharpen BDPT's env correctness and VCM's caustic-fallback correctness.
   *(See the IMPROVEMENTS.md §12 / VCM_ENV_MIS_PARTITION history for the SA-MIS
   scope — note the prior over-suppression attempts; the disc-area baseline is
   the current floor.)*
2. **Integrator routing + guidance** — encode the §2 per-class map into
   RENDERING_INTEGRATORS.md §2 (and any auto-selection heuristic) so authors land
   on the right integrator by default. Low cost, immediate value.
3. **Specular Polynomials** — reintegrate SMS-class caustics into BDPT as a
   connection strategy; this is what lets BDPT begin to subsume PT+SMS and,
   eventually, narrow VCM's exclusive caustic territory.
4. **Variance-aware MIS (Grittmann 2019)** then **correlation-aware MIS
   (Grittmann 2021)** — the per-second variance wins on BDPT.
5. **Research spike: ReSTIR-PT (D)** — after 1–4, evaluate on the §7 scenes.

**Retire as covered:** MLT now; PT once BDPT+Specular-Polynomials demonstrably
covers PT+SMS at competitive σ²·T (re-measure to confirm before retiring).

---

## 6. Correctness debts + open items, prioritized for this direction

Surfaced by the measurement arc; ordered by relevance to Candidate C:

1. **PT's own `IntegrateFromHitHWSS` env bias (~20% under on uniform env)** — the
   hybrid uses PT-spectral, so this matters. A separate spectral-bundle workstream
   (sibling of the now-fixed Bug 3). [INTEGRATOR_BUGFIX_FINDINGS.md](INTEGRATOR_BUGFIX_FINDINGS.md) §3.
2. **env-IBL SA-MIS (5.2.1) + VCM transmittance (5.2.2)** — step 1 of §5; the
   matrix-quantified VCM env/volume bias.
3. **~4% pre-existing BDPT-spectral-vs-PT gap** (present at hwss=false; not Bug 3)
   — small, BDPT-spectral; worth a look since BDPT is the default.
4. **BDPT α=1 coverage matte** — BDPT bakes coverage into RGB and reports α=1;
   irrelevant to over-black beauty (correct), matters only for compositing over a
   non-black background. Touches IntegratePixelRGB/splat/denoiser-AOV/VCM.
5. **`-ffast-math` dead-guards debt** — decided leave-as-is (latent, no confirmed
   harm); revisit only if a real integrator Inf/NaN surfaces.

---

## 7. What changed since the pre-arc analysis (§6, 2026-05-27)

- **The matrix is now clean.** Three measurement-infrastructure artifacts — EXR
  write-side FP16 clip, EXR read-side FP16 clip, and a test alpha-convention
  mismatch — were corrupting the noisiest cells and produced two phantom
  "integrator bugs" (the glass_pavilion "VCM Inf" and the ortho "−9.6% residual").
  All three are fixed; the matrix's headline (VCM-default contraindicated, hybrid
  favored) was robust throughout because it rests on converged-class σ²·T whose
  pixels never tripped FP16.
- **Bug 3 (HWSS spectral-BDPT/VCM −36%) is fixed** — three concrete companion-path
  bugs, not the feared multi-week path-split. **This strengthens the BDPT-centric
  direction**: BDPT-spectral is now HWSS-correct, and the HWSS env-IBL deficit
  closed as a bonus. The spectral path is substantially in place *and* correct.
- **Methodological caution for the implementation phases:** the arc's recurring
  lesson is *don't trust a measurement tool's verdict when it reads through the
  component under suspicion.* Two sessions chased a non-existent VCM Inf because
  both trusted a variance tool that read through the buggy EXR reader; an
  independent reader (pyOpenEXR) broke the deadlock. Every variance/parity claim
  in the MIS workstream above should carry an independent cross-check (and a
  reference-free invariant where one exists, e.g. the hwss=true≡hwss=false bundle
  invariant that proved the Bug-3 fix).

---

## 8. Ratification

This recommends **C now + D sequenced**, with the §5 plan. It is the user's call
to ratify or adjust (e.g. weight D sooner if per-scene uniformity is the priority
over shipping; or commit to A's caustic-in-BDPT bet and plan VCM's eventual
retirement). On ratification, the next artifact is a per-workstream implementation
plan for step 1 (env-IBL SA-MIS + VCM transmittance), the shared critical-path
starting point.
