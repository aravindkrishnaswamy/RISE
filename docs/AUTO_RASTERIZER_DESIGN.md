# Auto-Rasterizer (Integrator Dispatcher) — Design & Plan

**Status:** design — the routing mechanism for the Phase-3 decision (Candidate C,
PT-default auto-routed hybrid). See [UNIFIED_INTEGRATOR_DECISION.md](UNIFIED_INTEGRATOR_DECISION.md)
§4.1; data backing in [UNIFIED_INTEGRATOR_BASELINES.md](UNIFIED_INTEGRATOR_BASELINES.md).
**Date:** 2026-06-04.
**Goal:** a thin-shell `auto_rasterizer` that picks the right integrator
(PT / BDPT / VCM) per scene, so authors land on the efficient choice without
hand-selecting — *without* adding a new integrator algorithm.

---

## 1. The routing policy (settled — from the matrix)

| transport regime | integrator | why (matrix) |
|---|---|---|
| **default / converged bulk** (diffuse, glossy-metal, mixed, many-light, most env) | **PT** | wins σ²·T on 10/13 converged classes; 3–7× cheaper per sample |
| **strong-indirect / glossy-interreflection** (gi_spheres-class) | **BDPT** | the 3 classes where its connections overcome the 3–7× cost (gi_spheres 56×, alchemists, env_mesh) |
| **caustic / refractive / dispersive** (dielectric caustics, SDS) | **VCM** | the only integrator reaching the transport (PT/BDPT miss 44–78%); accept its bias+cost *here only* |

**Default to PT when uncertain** — never pay BDPT/VCM's penalty unless detection
is confident it pays off. (VCM-as-default and BDPT-as-default are both
contraindicated: §2 of the decision doc.)

---

## 2. Detection — three tiers, cheapest first

1. **Tier 0 — author hints / documented guidance (Phase 0, this commit).** The
   routing rules live in the project docs (RENDERING_INTEGRATORS.md §2 + a
   CLAUDE.md High-Value Fact) so future agents and authors know what to pick, and
   so the static analyzer (Tier 1) has an explicit spec to implement. An author
   can also *pin* an integrator (an explicit override that skips Tiers 1–2).
2. **Tier 1 — static scene-analysis best-guess (cheap, zero render).** Heuristics
   over the assembled scene: dielectric shells + small/point lights → caustic risk
   (VCM); high glossy-bounce material coverage + enclosed geometry → strong-
   indirect (BDPT); else → PT. Fast but approximate (the regime depends on
   *transport*, which static rules only proxy).
3. **Tier 2 — probe render (accurate, gated on cost — see Phase 3 experiment).** A
   cheap low-spp pass on a sparse tile sample that *operationalizes the matrix
   per-scene*: estimate each candidate's σ²·T (and detect caustic energy PT/BDPT
   miss), pick the winner. Backs up / overrides the Tier-1 guess. **Only adopted
   if the cost experiment (Phase 3) shows the probe is a small fraction of the
   render budget AND reliably matches the matrix verdicts.**

The auto-rasterizer runs Tier 0 (pin?) → else Tier 1 (guess) → else Tier 2
(probe, if enabled) and delegates to the chosen concrete rasterizer.

---

## 3. Architecture — a thin-shell `auto_rasterizer`

A new **`auto_rasterizer`** chunk → a thin `IRasterizer` wrapper that, at
construction/first render, runs detection and **delegates to a concrete
PT/BDPT/VCM rasterizer instance**. No integrator algorithm code; it *composes*
the existing ones. Fits RISE's pluggable-rasterizer + descriptor-driven-parser
architecture (a wrapper IRasterizer is idiomatic).

- **Round-trip:** the chunk persists as `auto_rasterizer`; the *runtime choice* is
  logged, not stored (read-only round-trip preference). An author override saves
  as the concrete chunk (or an explicit pin field).
- **Spectral:** an `auto_spectral_rasterizer` sibling delegating to the
  `*_spectral_` variants (same shell, spectral concrete targets).
- **Granularity:** whole-scene selection first (matches the matrix's per-scene
  verdicts). Per-region/per-tile routing (caustic tiles→VCM, rest→PT in one frame)
  is powerful for mixed scenes but needs a unified framebuffer + cross-integrator
  combination — that's a separate research effort (Candidate D bucket), explicitly
  out of scope for v1.

---

## 4. OPEN QUESTION — how to populate the auto-rasterizer's parameters

The shell wraps integrators with different parameter sets (PT: max_recursion,
samples; BDPT: max_eye_depth/max_light_depth, samples; VCM: samples, merge radius,
photon count; + common: spectral params, film, output). How does the
`auto_rasterizer` chunk express them? Options:

- **(i) Common params + per-integrator defaults** *(lean for v1)* — the chunk takes
  only universal params (samples, spectral, output); per-integrator-specific params
  use sensible built-in defaults. Simplest; "just works"; loses fine tuning.
- **(ii) Nested per-integrator override blocks** — `auto_rasterizer { samples 64;
  bdpt { max_eye_depth 5 }; vcm { merge_radius ... } }`. Common block + optional
  per-integrator overrides. Clean, explicit, flexible; more parser surface.
- **(iii) Superset flat descriptor** — accept the union of all params, forward the
  relevant subset. Big descriptor; ambiguous which params apply at author time.
- **(iv) Reference named configs** — the chunk names pre-defined PT/BDPT/VCM
  rasterizer chunks; picks among them. Reuses the chunk system; verbose.

**Lean: ship (i) for the MVP** (the dispatcher should need minimal config), **add
(ii) nested overrides** as the power-user path. **This is the design item to settle
before building the shell (Phase 1).**

---

## 5. Phased plan (build order)

- **Phase 0 — Author hints in project .md** *(this commit)*: routing rules into
  RENDERING_INTEGRATORS.md §2 + a CLAUDE.md High-Value Fact. Tier 0 + the spec for
  Tier 1.
- **Phase 1 — Thin-shell `auto_rasterizer` skeleton + parameter population.** Build
  the wrapper IRasterizer + chunk parser; resolve §4 (param population); selection
  is trivial at first (PT, or author pin) — just delegation. Establishes the
  architecture + round-trip + UI hook.
- **Phase 2 — Tier-1 static best-guess.** Implement the heuristic selection in the
  shell; validate its picks against the matrix's known per-scene verdicts.
- **Phase 3 — Probe-cost EXPERIMENT (the GATE).** Before building the probe:
  measure a low-spp sparse-tile probe's cost across a *variety* of scenes
  (diffuse/glossy/caustic/volume/spectral) as a fraction of the full render
  budget, AND its selection accuracy vs the matrix. **Proceed to Phase 4 only if
  the probe is cheap enough (target: a small single-digit % of budget) and
  reliable.** If not, stop at Tier 1 (static) + author pins.
- **Phase 4 — Tier-2 probe-render detection** *(gated on Phase 3)*: add the probe
  to the shell as the backup/override to the static guess.

---

## 6. Probe-cost experiment design (Phase 3 — the gate)

- **Measure:** probe wall-time (low-spp, sparse-tile, candidate integrators) vs
  the scene's full-render wall-time, across the production-weighted corpus (reuse
  the `var_test/` harness + `bin/tools/HDRVarianceTest`).
- **Gate criteria (both must hold):**
  1. **Cost:** probe ≪ full budget (target a small single-digit %; the exact
     threshold is a fork — discuss).
  2. **Accuracy:** the probe's integrator pick matches the matrix's per-scene
     verdict on ≥ most scenes (especially: detects caustic regimes PT/BDPT miss).
- **Knobs to sweep:** probe spp, tile-sample fraction, which integrators to probe
  (static pre-filter can skip the VCM probe when there are no dielectrics).
- **Outcome:** a go/no-go on Tier 2 + the chosen probe parameters.

---

## 7. UI integration (to design with the GUI bridges)

- An **"Auto" mode** as the default in the integrator selector; manual override to
  PT/BDPT/VCM/MLT.
- **Surface the choice + reason** ("Auto → VCM: dielectric caustic regime") for
  transparency; let the user pin an override (persists per scene).
- Mac + Windows bridges have switch-on-int enum getters that fall through to None
  on a missing case — **audit both bridges when adding the Auto enum value** (per
  the CLAUDE.md bridge-enum note).

---

## 8. Open questions for discussion

1. **Parameter population (§4)** — confirm (i) common+defaults for v1, with (ii)
   nested overrides later? Or prefer (ii)/(iv) from the start?
2. **Probe-cost gate threshold (§6)** — what fraction of budget is "acceptable"
   for the probe?
3. **Granularity** — whole-scene v1 confirmed; per-region deferred to D-research?
4. **Selection caching** — re-run detection every render, or cache the choice
   (and invalidate on scene edit)?
