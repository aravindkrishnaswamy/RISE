# Pre-Phase-1 Workstream — Option (c) Design Pass

**Session 3 date**: 2026-05-27 (design-only pass; NO code changes)
**Revised**: 2026-05-28 (Session 6 — design-only redesign after Session 5 Piece 2.B failure)
**Predecessor**: [docs/PRE_PHASE1_STATUS.md](PRE_PHASE1_STATUS.md) §"Session 2 outcome" + §"Session 5 outcome"
**Branch state**: working-tree only; Phase 1.A (commit `a4a24b85`) is the production state on `master`.  Session 5 left a Piece 2.B working-tree diff in `BDPTIntegrator.cpp` (D4 RGB+NM migration) that the user must revert or land jointly per this revision.
**This document SUPERSEDES** the per-file change list in [docs/PRE_PHASE1_STATUS.md](PRE_PHASE1_STATUS.md) §"Phase 1.B–1.G".  That list was hand-derived from algebra that missed cross-strategy MIS-walk effects on s≥2 chains touching the env vertex.  Execution proved it incorrect (§"Session 2 outcome" residuals: BDPT collapse to 6–9 % of PT on env+omni / env+mesh).  Do NOT re-attempt against the superseded spec.

**Session 6 redesign** (skim before reading the rest of this doc): the Session 5 Piece 2.B execution attempt surfaced THREE design defects in the v1 spec below.  Read [§0 Session 6 revision summary](#0-session-6-revision-summary) FIRST — that section is the load-bearing entry point.  §0 supersedes the §3 diff map (adds row D19), the §4 piece decomposition (regroups by transport side), and the §7.1 dismissal of architectural change in `MISWeight` (recommends a delta-aware `remap0` precondition).  The remainder of the document — §1, §2 (PBRT-v4 reference), and §5/§6/§7/§8 — remains valid but should be read THROUGH the §0 revisions.

> **Session 11 update (2026-05-30) — READ THIS BEFORE Session 6 revision summary below.**
>
> Two material changes to the v2 attack plan in this doc, driven by Session 9 (continuous-PMF fix) and Session 11 (exhaustive A/B disable bisect on the residual env+mesh 22 % over):
>
> 1. **Catastrophe modes mostly defused.** The v1 spec's Session 2 collapse (BDPT to 6–9 % of PT on env+omni / env+mesh) and the Session 5 collapse (env-only Lambertian 109 % → 128 %) both came from the **interaction between RISE's binary `envSelectProbability` (returns 0 in mixed scenes) and partial SA-MIS landings**.  Session 9 made `envSelectProbability` continuous (`cachedEnvSelectProb ∈ [0, 1]` per scene, with the env-vs-alias roll in `LightSampler::SampleLight`).  Without the binary interaction, partial SA landings should now be testable incrementally rather than under the v2 design's strict monolithic-or-skip discipline.  **The monolithic-or-skip rule from §0.2 is RELAXED but not eliminated** — partition-of-unity at the env-S0 ↔ env-NEE pair is still the load-bearing invariant.  Validate every intermediate landing against `EnvLightBalanceTest` at the documented lax tolerances; any topology that fails lax = stop and re-think.
>
> 2. **VCM-side scope reduced to 2 functions.** Session 11's exhaustive A/B disable bisect (`docs/VCM_ENV_MIS_PARTITION_INVESTIGATION.md` §2.Δ11) proved the entire VCM env+mesh 22 % over (and the env-only 9 % over) is sourced in the env-S0 ↔ env-NEE partition specifically.  Mesh-side strategies (s0_mesh, nee_mesh), splats (all origins), and interior connections are all empirically innocent (≤ 0.005 linR each in a 32×32×256-spp env+mesh test).  So the "Group 3 (VCM caller flags)" piece in §0.2 of this doc reduces to just two VCM-integrator functions — the env-branch of `EvaluateS0Impl` and the env-branch of `EvaluateNEEImpl`.  Mesh-side branches don't need SA-MIS migration.
>
> Additionally, Session 11 proved the following hypotheses **innocent** of the over-count and they should be SKIPPED in any future bisect (don't re-test them):
>   - Session 10's BDPTVertex `pdfSelect` field, InitLight `dVC × pdfSelect`, and 3 wCamera consumer-site divides — all empirically no-op or < 0.001 linR in env-dominant scenes.
>   - The env-NEE `invLightSelect = 1/pdfSelect` rescale (no-op when envSelProb ≈ 1).
>   - The dVCM recurrence under continuous-PMF (verified mathematically correct to < 0.001 % rel-diff vs independent geometric computation).
>
> Decision on Session 10's micro-changes: they're kept in-tree as a more careful SmallVCM-convention foundation but not load-bearing for the partition fix.  The supervisor staging the SA-MIS migration should consider them when planning the diff cohesion (keep / refactor / revert as part of the migration, not independently).
>
> Estimated budget post-Session-11: **~1 week of focused work** (was 3 weeks in v1, ~2 weeks in v2).
>
> Full Session 11 evidence + refined attack plan: **[docs/VCM_ENV_MIS_PARTITION_INVESTIGATION.md](VCM_ENV_MIS_PARTITION_INVESTIGATION.md)** §6.

---

## §0. Session 6 revision summary (2026-05-28)

The Session 5 execution attempt landed only diff-map row **D4** (Path B s=0 env-vertex `pdfRev`, RGB+NM) and gates 4+5 fired catastrophically (env+omni `BDPT/PT 85% → 7%`, env+mesh `85% → 5%`, env-only Lambertian `109% → 128%`).  Root-cause analysis ([Session 5 outcome](PRE_PHASE1_STATUS.md#session-5-outcome-piece-2b--d4-migration-attempted-halts-on-documented-joint-landing-failure)) exposed three design defects in this doc:

1. **A diff-map row was missing** — the eye-side env-vertex `pdfFwd` assignment in `GenerateEyeSubpath` Path B push (BDPTIntegrator.cpp:2821 RGB / :6545 NM) was not in the §3 table.  PBRT-v4 handles it implicitly via `Vertex::PDF`'s `ConvertDensity` short-circuit at env destinations (§2.3, §2.4); RISE's eye walk doesn't go through that dispatch.  Without migrating it, the SA/area measure boundary at the env vertex is half-cooked: `pdfRev = 0` post-D4 in mixed scenes (env not in NEE alias table), but `pdfFwd ≠ 0` (still the area-converted BSDF SA-pdf from the predecessor).  RISE's `MISWeight` then `remap0`s `pdfRev` to 1, computes `ri *= 1 / pdfFwd`, and the ratio blows up by ~13× at the env vertex — `ri² ≈ 170` dominates `sumWeights`, collapsing the s=0 MIS weight to ~4e-4.  **This is added as row D19 in §0.1 below.**
2. **The piece decomposition was wrong** — pieces 2.B–2.E in §4 each migrate ONE diff-map row in isolation.  The Session 5 evidence is that rows D4 + D5 + D19 are mutually load-bearing (the **s=0 group**) and must land jointly; similarly rows D2 + D3 + D6 are the **light-subpath group** that must land jointly.  Landing any subset breaks the cross-strategy MIS-walk balance.  **Revised piece decomposition in §0.2 below.**
3. **The §7.1 dismissal of `MISWeight` changes was too confident** — the `remap0` line at [BDPTIntegrator.cpp:5049-5050](../src/Library/Shaders/BDPTIntegrator.cpp:5049) is UNCONDITIONAL on `isDelta`, applied to *every* vertex on the running `ri` before the skip-rule fires.  This is what caused Session 5's catastrophe: a non-delta env vertex with `pdfRev = 0` (a genuinely-impossible alternative strategy) gets `remap0`'d to 1 instead of propagating as a real zero.  A **delta-aware `remap0`** (3-line change in two MISWeight variants) makes the existing sentinels redundant and lets each transport-side group land independently without joint coupling to the others.  **Recommended in §0.3 below.**

The three defects compound: D19 is what the s=0 group needs internally, the regrouping is what makes pieces landable without all-or-nothing risk, and delta-aware `remap0` is the architectural enabler that decouples the s=0 group from the light-subpath group.

### §0.1 New diff-map row D19 — eye-side env-vertex `pdfFwd`

| # | Concern | PBRT-v4 (file:line) | RISE current (file:line) | Adaptation needed |
|---|---|---|---|---|
| **D19** | `pdfFwd` measure on the eye-side env vertex (synthetic env vertex pushed by Path B eye-subpath escape on miss) | Set via `Vertex::PDF`'s `ConvertDensity(bsdf_pdf, env_next)` short-circuit during `RandomWalk` — returns SA unchanged (`integrators.cpp:1731-1743` short-circuit + `1745-1776` dispatch).  **SA-measure at the env vertex itself.** | Area-measure `SolidAngleToArea(pdfFwdPrev, 1, distSqToExit)` set inline at the synthetic env-vertex push: [BDPTIntegrator.cpp:2821-2822](../src/Library/Shaders/BDPTIntegrator.cpp:2821) (RGB) and [BDPTIntegrator.cpp:6545-6546](../src/Library/Shaders/BDPTIntegrator.cpp:6545) (NM).  Both sites use `tExit² = distSqToExit` to convert the predecessor's SA pdf into area at the env vertex. | **Migrate**: replace `BDPTUtilities::SolidAngleToArea(pdfFwdPrev, 1, distSqToExit)` with `BDPTUtilities::ConvertDensity(pdfFwdPrev, ..., vEnv)` — the Phase 1.A helper short-circuits at env destinations and returns `pdfFwdPrev` unchanged in SA.  Equivalently, just assign `vEnv.pdfFwd = pdfFwdPrev` with an inline comment citing PBRT-v4 `ConvertDensity` line 1733.  This places the env vertex's `pdfFwd` in SA-measure, **matching what D4 places in `pdfRev`** — so the ratio at the env vertex inside `MISWeight` is `SA / SA = unitless`.  In the env-only case both are SA-positive; in the mixed-scene case both are zero (D4 sets `pdfRev = envSelectProb_NEE × pdf_env_sa = 0`; D19 sets `pdfFwd = pdfFwdPrev` which is the eye-walk's BSDF SA-pdf — see §0.4 for why this also needs to evaluate to zero in mixed-scene for the ratio to collapse cleanly).  |

**Subtlety in mixed scenes**: D4 makes `pdfRev_env = 0` when `envSelectProb_NEE = 0`.  D19 makes `pdfFwd_env = pdfFwdPrev` (the predecessor's BSDF SA-pdf), which is **not** zero in mixed-scene — it's the predecessor's BSDF Pdf evaluated in the escape direction, which is typically O(0.1)-O(1) for diffuse surfaces.  So D19 alone does NOT make `pdfFwd_env = 0` in the mixed-scene case.

This is the crux of why D19 alone (or D4+D19 together) is NOT enough: post-migration `pdfRev_env = 0`, `pdfFwd_env = bsdf_pdf_SA ≈ 0.3`, `remap0(0) / 0.3 = 1 / 0.3 = 3.3`, `ri² ≈ 11`, `misWeight ≈ 1/12 ≈ 0.08` — far below the correct value of ~1.0 (the s=1 NEE alternative literally cannot produce this path in mixed-scene).

This is what motivates the delta-aware `remap0` recommendation in §0.3: with delta-aware `remap0`, `pdfRev_env = 0` propagates as a real zero, `ri *= 0`, `ri² = 0`, `sumWeights` unaffected, `misWeight = 1`.  Correct.

PBRT-v4 doesn't have this problem because their `LightSampler.PMF(envLight)` is nonzero in mixed scenes (they NEE-sample env in mixed scenes via uniform light selection); RISE's `EnvSelectProbability()` is binary 0-or-1 (returns 1 only when `!aliasTable.IsValid()`, i.e. env-only-no-alias-table) per [LightSampler.h:362-367](../src/Library/Lights/LightSampler.h:362).  The RISE-specific asymmetry is the load-bearing fact: a non-delta vertex with `pdfRev = 0` IS a meaningful state ("this strategy genuinely cannot sample this vertex"), and the `MISWeight` walk must honour the zero rather than `remap0`-ing it away.

### §0.2 Revised piece decomposition — group by transport side

The Session 5 root-cause analysis showed that diff-map rows for the same env-vertex-instance must land jointly.  Pieces are regrouped accordingly:

| Group | Rows | Pieces in v1 spec | Joint landing required? |
|---|---|---|---|
| **s=0 group** (eye terminates on env) | D4 + D5 + **D19** | 2.B + 2.C + [missing] | YES.  All three modify the eye-side env vertex or its eye-predecessor.  Landing any subset leaves the MIS-walk ratio at the env vertex broken on at least one topology (Session 5 evidence). |
| **light-subpath group** (light-subpath emission of env, used by s=1 NEE + s≥2) | D2 + D3 + D6 | 2.D + 2.E | YES (within the group).  D2 sets `lightVerts[0].pdfFwd` SA, D3 sets `lightVerts[1].pdfFwd` projected disc-area, D6 sets `lightStart.pdfRev` SA at the Path A s=1 site.  Without D6, the s=1 site's MIS ratio at lightVerts[0] mixes SA `pdfFwd` (D2) with area `pdfRev` (current `SolidAngleToArea(...)`) — unit mismatch. |
| **VCM group** | D14 + D15 | 2.F | YES (within the group).  Caller flags interlock with the Georgiev 2012 recurrence. |
| **Tests + docs** | row D17 (no migration, just documented) | 2.G | Independent.  Land after the three groups close. |

**Are the s=0 group and the light-subpath group cross-strategy-coupled?**  See §0.4 for the algebraic trace.  **Short answer**: with delta-aware `remap0` (§0.3) the two groups are *structurally independent* (they touch disjoint vertex instances on a given BDPT path), so they can land in *either* order without coupling.  Without delta-aware `remap0`, the s=0 group's mixed-scene catastrophe (Session 5) cannot be cured by landing the light-subpath group alone, so the two groups effectively must land together (monolithic).

**Recommended sequencing** (assuming delta-aware `remap0` lands first per §0.3):
1. **Piece 2.A** — audit mode (unchanged from v1).
2. **Piece 2.B'** — delta-aware `remap0` (NEW; precondition, §0.3).  Behaviour-identical on master because no non-delta vertex has `pdfRev = 0` post-Phase-1.A.
3. **Piece 2.C'** — **s=0 group**: D4 + D5 + D19 (RGB + NM), landed atomically.  Replaces v1 pieces 2.B and 2.C.  Drops the `kEnvZeroSentinel` workaround at both Path B sites.
4. **Piece 2.D'** — **light-subpath group**: D2 + D3 + D6 (RGB + NM), landed atomically.  Replaces v1 pieces 2.D and 2.E.
5. **Piece 2.E'** — **VCM group**: D14 + D15 (templatized → single edit).  Same as v1 piece 2.F.
6. **Piece 2.F'** — tighten tests + docs.  Same as v1 piece 2.G.

Total estimate after revision: **~5 sessions** (audit 0.5 + delta-aware remap0 0.5 + s=0 group 1.0 + light-subpath group 1.0 + VCM 0.5 + tests/docs 1.0 + adversarial review headroom 0.5).  Up from v1's 4.0-4.5, in line with the option (c) upper bound projection.

### §0.3 Recommended precondition — delta-aware `remap0`

[BDPTIntegrator.cpp:5049-5050](../src/Library/Shaders/BDPTIntegrator.cpp:5049) currently has:

```cpp
const Scalar pdfR = (vi.pdfRev != 0) ? vi.pdfRev : Scalar(1);
const Scalar pdfF = (vi.pdfFwd != 0) ? vi.pdfFwd : Scalar(1);
ri *= pdfR / pdfF;
// ...later in the loop...
if( vi.isDelta ) { continue; }  // skip non-connectible
```

(Mirrored at lines 5102-5103 for the eye-side walk.)  The skip-rule fires AFTER `ri` has been multiplied — so even non-delta vertices with `pdfRev = 0` get their `pdfRev` remapped to 1, mutating the running `ri` for downstream iterations.  Per Session 5's root-cause analysis, this is what caused the env+omni / env+mesh 7-9% collapse: a non-delta env vertex with `pdfRev = 0` (the design's intended-correct value in mixed-scene) gets `remap0`'d, then `ri *= 1 / pdfFwd_env` blows up the ratio.

**Proposed change** — gate `remap0` on `isDelta`:

```cpp
Scalar pdfR;
if (vi.pdfRev != Scalar(0)) {
    pdfR = vi.pdfRev;
} else if (vi.isDelta) {
    pdfR = Scalar(1);  // Veach/PBRT convention: delta-area-pdf measure artifact
} else {
    pdfR = Scalar(0);  // Genuine zero: strategy cannot generate this vertex; propagate
}
// And similarly for pdfF.
ri *= pdfR / pdfF;  // when pdfF = 0 non-delta, this is 0/0 → set ri = 0 explicitly
```

(See §0.3.5 for the 0/0 handling.)

**Q1: Does PBRT-v4 gate on `isDelta`?**  No.  PBRT-v4 uses the same unconditional `remap0 = [](float f) -> Float { return f != 0 ? f : 1; }` at `integrators.cpp:2138`.  PBRT-v4 produces correct mixed-scene results because their `LightSampler::PMF` is nonzero for all lights (uniform selection always includes env with positive probability), so `PDFLightOrigin` returns nonzero — the `pdfRev = 0` corner case doesn't arise.  RISE's `EnvSelectProbability()` is binary 0-or-1 ([LightSampler.h:362-367](../src/Library/Lights/LightSampler.h:362)) by design (env-NEE only when no other lights are alias-table-eligible).  This is a RISE-specific architectural quirk that PBRT-v4's design assumption doesn't cover.

So delta-aware `remap0` is **a RISE-specific divergence from PBRT-v4 to handle a RISE-specific quirk**, not a strict port.  This is a deliberate, documented departure.

**Q2: Does it break existing delta-vertex handling?**  No — by inspection of the code:
- For delta vertices: `isDelta = true`, behaviour matches old `remap0` (zero → 1).
- For non-delta vertices with `pdfRev > 0`: behaviour unchanged (passthrough).
- For non-delta vertices with `pdfRev = 0`: NEW behaviour — propagates as zero.  In production master, this state does not occur because the sentinels (`kEnvZeroSentinel = 1e-30`) at the Path B s=0 site keep `pdfRev > 0` even when `envSelectProb = 0`.  So delta-aware `remap0` is a no-op on master.

**Q3: Does `BDPTStrategyBalanceTest` / `VCMStrategyBalanceTest` pass under delta-aware `remap0` without other changes?**  By the above, yes — these tests exercise non-env scenes where every non-delta vertex has `pdfRev > 0` (BSDF Pdf > 0 in well-defined directions).  The only change in semantics is for `pdfRev = 0` non-delta vertices, which don't occur in those test scenes.

**Q4: Does it enable D4 + D5 + D19 to land without coupling to D2 + D3 + D6?**  Yes.  With delta-aware `remap0`:
- s=0 group lands: env vertex `pdfRev = 0` (mixed-scene) propagates as zero, `ri = 0` at the env vertex, `sumWeights` only gets the (s=0) strategy itself = 1, `misWeight_s=0 = 1`.  Correct — no alternative can sample env in mixed-scene.
- Light-subpath group is untouched: s=1+ contributions for env still use the old disc-area `lightVerts[0].pdfFwd`.  Internally inconsistent with the new SA convention at the env vertex on the s=0 side, but DOESN'T cross-pollute because the two vertex instances are disjoint per path.

**Q5: Does it interact with Path A's PT-formula contribution override at s=1?**  See [BDPTIntegrator.cpp:4053-4093](../src/Library/Shaders/BDPTIntegrator.cpp:4053) where the env case uses `eyeEnd.throughput × fEye × Le × Tr × (cosEyeWi / pdfSA)` instead of the disc-G + pdfLight formula.  The contribution itself bypasses pdfLight, but MIS still consumes the staged `lightStart.pdfRev` and `eyeEnd.pdfRev` set at lines 4111-4173.  Under delta-aware `remap0`, if any of these are 0 the ratio propagates as 0 — but the values are normally positive (set from `EvalPdfAtVertex` BSDF eval and env emission pdf eval, both > 0 for typical paths).  No interaction expected.

**Q6: Does PBRT-v4's MISWeight have an `IsDeltaLight()` aware skip-rule that would be the moral analog?**  Yes — PBRT-v4 line 2199 has `bool condition_i = (i == 0 ? lightVertices[0].IsDeltaLight() : lightVertices[i-1].delta);` and `if (!lightVertices[i].delta && !condition_i)` adds to `sumRi`.  This is functionally a SKIP rule (don't accumulate the strategy if delta) and is mirrored in RISE at line 5069 (`if (vi.isDelta) continue;`).  It does NOT prevent `remap0` from mutating `ri` for downstream iterations.  So even PBRT-v4 does the unconditional `remap0` mutation; the difference is upstream (PBRT-v4 never has non-delta `pdfRev = 0` in well-formed scenes).

**Recommendation**: include delta-aware `remap0` as a precondition piece **2.B'** for option (c) execution.  Three reasons:
1. It's a 6-line code change (3 lines on the light-side walk + 3 on the eye-side walk + analogous tweak for `pdfF` 0/0 handling) — much smaller than D19's site-by-site migration.
2. It makes the existing `kEnvZeroSentinel` workaround redundant (Piece 2.C' can simply drop the sentinel — no replacement needed).
3. It decouples the s=0 group from the light-subpath group, so the pieces can land in either order or in parallel, removing the monolithic-landing risk.

### §0.3.5 The 0/0 handling

With delta-aware `remap0`, both `pdfR` and `pdfF` could simultaneously be 0 at a non-delta vertex (e.g. env vertex in mixed-scene if D19 also lands such that `pdfFwd = 0`).  `0/0 = NaN` would corrupt `ri`.  Mitigation: explicit check:

```cpp
if (pdfF == Scalar(0)) {
    ri = Scalar(0);  // propagate the zero forward
} else {
    ri *= pdfR / pdfF;
}
```

The semantics: if both `pdfR = 0` and `pdfF = 0` at a non-delta vertex, the strategy "this vertex was generated by the alternative side" has no defined ratio — best interpretation is that the alternative side can't generate this path, so `ri = 0`.  If only `pdfF = 0`, same outcome: the current strategy claims this vertex was generated by the current side with zero probability, which is mathematically degenerate but downstream `ri = 0` is the safe answer.

Implementation cost: 2 added lines per walk (4 lines total in MISWeight + 4 in the NM twin if separate).

### §0.4 Cross-strategy MIS-walk magnitude trace

To verify the group decomposition is safe, this section traces the per-strategy `ri` magnitude at the env vertex on the env-only Lambertian RGB topology (envSelectProb_NEE = 1) and the env+omni RGB topology (envSelectProb_NEE = 0), for four states:

**State M (master, post Phase 1.A)**: all sentinels in place; all rows in §3 disc-area.

**State A (master + delta-aware `remap0`)**: §0.3 precondition only.  No other migration.

**State B (M + s=0 group)**: D4 + D5 + D19 migrated.  Sentinels dropped at the s=0 site.  Light-subpath group untouched.  Plus the §0.3 precondition.

**State C (M + light-subpath group)**: D2 + D3 + D6 migrated.  s=0 group untouched.  Plus the §0.3 precondition.

**State D (M + both groups)**: full migration.  Plus the §0.3 precondition.

Numerical values from Session 4 Piece 2.A audit data (env-only Lambertian, scene radius ≈ 1.4):

| Quantity | env-only-Lambertian value |
|---|---|
| `pdf_env_sa(wi)` | 0.0796 sr⁻¹ |
| `1 / (π·r²)` (disc-area pdfPos) | 0.159 m⁻² |
| `bsdf_pdf_SA` at predecessor (Lambertian, normal incidence) | ~ 0.32 sr⁻¹ |
| `cos(rayDir, env_normal)` at env push | 1.0 by construction (geomNormal = -rayDir) |
| `tExit² = distSqToExit` (env vertex distance) | ~ 4 m² (r ≈ 1.4 → tExit ≈ 2) |

#### Trace at the env vertex on s=0 strategy, env-only Lambertian (envSelectProb_NEE = 1)

The eye-side walk in `MISWeight` iterates j from t-1 (env vertex) down to 1.  At j = t-1 (env vertex), the ratio determines the relative weight of the s=1 NEE alternative.

| State | `pdfFwd_env` formula | `pdfFwd_env` value | `pdfRev_env` formula | `pdfRev_env` value | `pdfR / pdfF` | `ri²` contribution |
|---|---|---|---|---|---|---|
| M | `SolidAngleToArea(0.32, 1, 4) = 0.32 / 4` | 0.080 | `envSelectProb × 1/(π·r²) + sentinel` | 0.159 | 0.159 / 0.080 = 1.99 | ~3.96 |
| A | same as M | 0.080 | same as M | 0.159 | 1.99 | ~3.96 |
| B | `pdfFwdPrev = bsdf_pdf_SA` (SA) | 0.32 | `envSelectProb_NEE × pdf_env_sa` | 1.0 × 0.0796 = 0.0796 | 0.0796 / 0.32 = 0.249 | ~0.062 |
| C | same as M (eye-side untouched) | 0.080 | same as M (eye-side untouched) | 0.159 | 1.99 | ~3.96 |
| D | same as B (eye-side migrated) | 0.32 | same as B | 0.0796 | 0.249 | ~0.062 |

**Interpretation**:
- States M and C have `ri² ≈ 3.96` at the env vertex, so `misWeight_s=0 ≈ 1 / (1 + 3.96 + ...) ≈ 0.2` (further suppressed by s≥2 terms).  s=0 contribution at full env intensity × 0.2 misWeight = 20% of full → BDPT visibly darker than PT.  This is the documented 15-22% residual.
- States B and D have `ri² ≈ 0.062`, so `misWeight_s=0 ≈ 1 / (1 + 0.062 + ...) ≈ 0.95`.  Much closer to the correct s=0 weight.

**Light-subpath group (state C) standalone effect on env-only Lambertian s=0 misWeight**: ZERO, because s=0 doesn't walk the light-side.  The light-subpath group's effect is only on s≥1 strategies' misWeight — different code paths inside `MISWeight` and `ConnectAndEvaluate`.

#### Trace at the env vertex on s=0 strategy, env+omni Lambertian (envSelectProb_NEE = 0)

| State | `pdfFwd_env` value | `pdfRev_env` value | `pdfR / pdfF` (with old `remap0`) | `pdfR / pdfF` (with delta-aware `remap0`) | `ri²` contribution |
|---|---|---|---|---|---|
| M | 0.080 | 1e-30 (sentinel) | 1e-30 / 0.080 = 1.25e-29 | same (`pdfRev > 0`, no remap) | ~0 (s=0 misWeight ≈ 1) |
| A | 0.080 | 1e-30 (sentinel still in place) | same as M | same as M | ~0 |
| B | 0.32 (D19) | 0 (D4: envSelectProb_NEE × pdf_env_sa = 0) | **remap0 fires: 1 / 0.32 = 3.13** | **0 / 0.32 = 0 (real zero)** | OLD: 9.77, NEW: 0 |
| C | 0.080 | 1e-30 (s=0 site untouched) | same as M | same as M | ~0 |
| D | 0.32 (D19) | 0 (D4) | **OLD: 9.77** | **NEW: 0** | OLD: 9.77, NEW: 0 |

**Key observation**: state B (s=0 group landed without light-subpath group) WITH delta-aware `remap0` gives the correct `ri² = 0`, `misWeight_s=0 ≈ 1`, matching the sentinel-approach behaviour of master.  Without delta-aware `remap0`, state B gives `ri² = 9.77`, `misWeight_s=0 ≈ 1/11 ≈ 0.09` — the Session 5 catastrophe.

**This is the analytic justification for §0.3's recommendation**: delta-aware `remap0` is what lets the s=0 group land independently of the light-subpath group on mixed scenes.

#### Trace on s=1 strategy through env-vertex (light-side walk)

For s=1 paths, `MISWeight` walks the light side at i = s-1 = 0 (the light-vertex 0 = env vertex on s=1).  The ratio at i=0 is `lightVerts[0].pdfRev / lightVerts[0].pdfFwd`.

| State | `lightVerts[0].pdfFwd` | `lightVerts[0].pdfRev` (staged by D6) | `pdfR / pdfF` | Cross-strategy meaning |
|---|---|---|---|---|
| M (env-only) | disc `1/(π·r²) = 0.159` | `SolidAngleToArea(bsdf_pdf_SA, 1, distSq) ≈ 0.32 / dist²` | depends on dist; off by `πr²` factor | mismatched measure (current 15-22% residual) |
| D, light-subpath group landed (env-only) | SA `envSelectProb_NEE × pdf_env_sa = 0.0796` | `ConvertDensity(bsdf_pdf_SA, eyeEnd, lightStart) = 0.32` (SA, short-circuit) | 0.32 / 0.0796 = 4.02 | both SA, unitless |
| M (env+omni) | disc `0.159` (unchanged in mixed-scene because pdfPos = 1/πr² regardless) | same as M env-only | same as M env-only | residual on top of the 15-22% bias |
| D, light-subpath group landed (env+omni) | SA `envSelectProb_NEE × pdf_env_sa = 0 × 0.0796 = 0` | `ConvertDensity(bsdf_pdf_SA, eyeEnd, lightStart) = 0.32` (SA) | **with old `remap0`**: 0.32 / 1 = 0.32; **with delta-aware**: 0.32 / 0 = `ri = 0` | s=1 alternative contributes 0 — correct (NEE can't sample env in mixed-scene) |

**Observation**: light-subpath group landing alone (state C) does NOT cause a catastrophe on s=1 in env+omni because `lightVerts[0].pdfRev` becomes nonzero (eyeEnd BSDF SA-pdf is positive), so the `remap0` doesn't fire on that vertex.  But the ratio `0.32 / 0` would NaN under naive division — with delta-aware `remap0` (and its §0.3.5 0/0 guard), `ri = 0` propagates correctly.

**Without delta-aware `remap0`, the light-subpath group standalone would HAVE its own catastrophe** in env+omni s=1 paths: `pdfFwd = 0` → `remap0` → 1 → `ri *= 0.32 / 1 = 0.32` → `ri² ≈ 0.1` per env vertex.  s=1 strategy in mixed-scene env+omni would get spuriously down-weighted while light-subpath emission is happening with proper probability.  Magnitude shift unknown without measurement.

#### Summary of the trace

- **State M → state A** (delta-aware `remap0` alone): no behaviour change on env-only because sentinels were already > 0; no behaviour change on env+omni because sentinels still > 0.  Master baseline preserved.
- **State M → state B** (s=0 group + delta-aware `remap0`): env-only s=0 `ri² 3.96 → 0.062`, misWeight moves from ~0.2 to ~0.95.  env+omni s=0 `ri² 1.25e-29 → 0` (no catastrophe with delta-aware `remap0`).  s=1 paths unchanged (light-subpath group untouched).  Net: env+omni mean should approximately match master (~85% of PT, since s=0 misWeight was already ~1 via the sentinel); env-only mean should improve to ~95% of PT.
- **State M → state C** (light-subpath group + delta-aware `remap0`): env-only s=0 unchanged.  env-only s=1 with corrected SA bookkeeping should reduce the ~15-22% residual on s=1 strategies.  env+omni s=1 properly down-weighted to 0 (matches the sentinel-approach intent).  Net: env-only improves, env+omni unchanged (mixed-scene s=1 already had zero contribution by construction).
- **State M → state D** (both groups + delta-aware `remap0`): both improvements compose; expect convergence to within strict tolerances `{0.10, 0.30, 1.00}` on every topology.

**This trace is sketch-level**: real magnitudes also depend on cross-strategy terms (`(p_2_1 / p_0_3)²` and higher-order) that propagate `ri` through multiple vertices.  The trace argues that the SIGN of the shifts is correct and that the catastrophes are bounded under delta-aware `remap0`.  Adversarial review can refute this analytically; if it does, fall back to monolithic landing.

### §0.5 Revised gate semantics

The original chip's gate-4 fired wrong-way on Piece 2.B because it required env-only Lambertian RGB to improve (`109% → closer to 100%`), but Piece 2.B alone (without D5, D19, delta-aware `remap0`) actually moves it `109% → 128%`.  This is an intermediate-state magnitude swing, not a correctness failure — the per-piece gate was wrong.

**Revised gate semantics per piece**:

1. **Per-piece correctness floor (NON-NEGOTIABLE)**:
   - Clean warning-free build (`make + Xcode RISE-GUI`).
   - 116/116 tests pass.
   - EnvLightBalanceTest passes at LAX tolerances `{ 0.35, 0.35, 2.00 }` on all topologies — i.e. no topology falls below 65% of PT or above 135% of PT (1.65/0.65 = ~2.5× swing window).
   - `BDPTStrategyBalanceTest`, `VCMStrategyBalanceTest`, `VCMRecurrenceTest` must NOT regress (these gate against non-env scenes).

2. **Per-piece magnitude-direction gates (RELAXED for intermediates)**:
   - The target topology for this piece must move in the predicted direction OR remain within ±20% of the pre-piece value.  Direction predictions per piece:
     - Piece 2.B' (delta-aware `remap0`): NO behaviour change — every topology within ±2% of master.
     - Piece 2.C' (s=0 group): env-only Lambertian RGB moves toward 100% (≥ 90%, ≤ 110%).  env+omni / env+mesh: remain within 70%-100% (catastrophe-free; was 85% under sentinel; expected stay near 85% under delta-aware remap0 + D4 cleanly zero).  Spectral HWSS=true: monitor; may swing.
     - Piece 2.D' (light-subpath group): env-only s=1 contribution improves (visible via PT vs BDPT visual diff); env+omni / env+mesh: stable (s=1 already at-zero contribution via envSelectProb = 0).  Spectral HWSS=true: target topology — must improve.
     - Piece 2.E' (VCM): all topologies match BDPT post-Piece-2.D' within ±10%.
   - If a topology moves OUTSIDE the predicted direction AND outside ±20% tolerance, STOP and audit (potential design defect or implementation bug).

3. **Per-piece strict tolerance monitoring (NON-BLOCKING)**:
   - Run `EnvLightBalanceTest` at strict tolerances `{ 0.10, 0.30, 1.00 }` after each piece.  Record the failure count.  It should monotonically decrease across pieces — non-monotonic implies something interacts in a way the design didn't predict.

4. **Final-state gates (NON-NEGOTIABLE at Piece 2.F')**:
   - EnvLightBalanceTest passes at STRICT tolerances `{ 0.10, 0.30, 1.00 }` on every topology × RGB/spectral × HWSS combination.
   - HDRVarianceTest K-trial: BDPT-vs-PT and VCM-vs-PT RMSE drops by ≥ 15% on env-IBL scenes.
   - Visual parity on `ripple_dreams_fields.RISEscene` — PT, BDPT, VCM render visually indistinguishable at matched samples.
   - Adversarial review post-2.C' (after s=0 group) and post-2.D' (after light-subpath group) — 2-3 reviewers minimum.

5. **Stop rule per piece**:
   - If the per-piece correctness floor or magnitude-direction gate fails AFTER honest debugging: STOP, REVERT, audit the design.  Don't push through hoping the next piece fixes it (that was Session 2's failure).

The chip-spec's lax tolerance pass per piece (116/116 tests + EnvLightBalanceTest at lax) is preserved; what's relaxed is the magnitude-direction assumption.

### §0.6 What's deprecated by this revision

The following sections of v1 are superseded by §0 and should be read as historical context:

- **§3 diff map** — supplement with row D19 from §0.1.  All other rows valid.
- **§4 piece decomposition** — replaced by §0.2's transport-side grouping.  Original pieces 2.B-2.E are sub-elements of the s=0 / light-subpath groups; they're not retired but the LANDING UNIT changes from per-row to per-group.
- **§5.4 Hypothesis 4 resolution** — **incorrect**.  The claim "MISWeight's `remap0` line does NOT fire because eyeEnd is the env vertex and env.isDelta = false" was based on the false belief that `remap0` is gated on `isDelta`.  It is NOT (verified by reading [BDPTIntegrator.cpp:5049-5050](../src/Library/Shaders/BDPTIntegrator.cpp:5049)).  The correct claim is: `remap0` fires unconditionally, so the sentinel was load-bearing not as a numerical guard but as a *behaviour gate*; dropping it requires either the delta-aware `remap0` (§0.3) OR an equivalent mechanism.
- **§5.5 cross-strategy MIS-walk discussion** — direction-correct on the env-only case but missed the mixed-scene `remap0` interaction.  Augmented by §0.4 trace.
- **§7.1 dismissal** — "Porting [a MISWeight architecture change] would require introducing a BDPTVertex::PDF / PDFLight / PDFLightOrigin API surface, which is structurally a bigger change than option (c) targets" — true for the full PBRT-v4 in-walk `ScopedAssignment` port, but a **minimal 6-line delta-aware `remap0`** is much smaller in scope and does not require introducing new API surface.  §0.3 supersedes the §7.1 dismissal for this specific change.

### §0.7 Residual uncertainty

This redesign rests on the following claims that I have NOT been able to verify exhaustively in this design pass:

1. **Quantitative magnitude predictions in §0.4** are sketch-level.  Real `ri²` cross-strategy effects depend on the full ratio chain through multiple vertices.  Adversarial review or a numerical experiment is the load-bearing check.
2. **Non-delta vertices with `pdfRev = 0` outside the env Path B s=0 site** — I have NOT exhaustively audited the entire codebase to confirm this state never occurs in master production code.  If it does occur somewhere (e.g. a grazing-angle BSDF Pdf that returns 0, a corner case in `EvalPdfAtVertex`), delta-aware `remap0` would alter that path's behaviour.  Recommended pre-flight audit: `grep -n "pdfRev = " src/Library/Shaders/BDPTIntegrator.cpp` and verify every site stores a positive value or has a sentinel.
3. **HWSS companion-wavelength path** at [BDPTIntegrator.cpp:5400-5470](../src/Library/Shaders/BDPTIntegrator.cpp) reads `lightVerts[0].pdfFwd` via the LightSample fields, not the BDPTVertex fields.  Per §5.3, this should be unaffected — but the analysis didn't explicitly trace HWSS companion paths through `MISWeight` itself.  Adversarial review axis 3 in §6.2 should explicitly cover this.
4. **The s=2 light-subpath emission of env + camera-rasterization splat (`SplatLightSubpathToCamera`)** uses its own MISWeight call at the t=1 site.  Did NOT trace this in §0.4.  The light-subpath group's D2 + D3 might shift this strategy's misWeight via the `lightVerts[0].pdfFwd` change.  Adversarial review axis should cover.

If any of these turn out to invalidate the redesign, the fallback is option (a) — accept the 15-22% residual.  Option (b) (audit-driven retry) remains an alternative if the user wants to spend 3-5 more sessions on rigorous algebra before code changes.

---



The env-IBL SA-MIS refactor (IMPROVEMENTS.md §12) closes a 15–22 % residual that BDPT/VCM exhibit vs PT on environment-light scenes.  The 2026-05-25 first-pass Path A + Path B disc-area workaround took BDPT/VCM from "~0–5 % of PT" up to "78–95 % of PT" but leaves a systematic disc-area-vs-true-SA gap.  Three closure options were enumerated in [PRE_PHASE1_STATUS.md](PRE_PHASE1_STATUS.md) §"Recommended next session approach":

| Option | Estimate | Residual after | Why selected / rejected |
|---|---|---|---|
| (a) Conservative stop | 0 sessions | 15–22 % | Accepts the residual.  Already the current production state.  Rejected because the gap is large enough to be visible (BDPT/VCM darker than PT on `ripple_dreams_fields.RISEscene` at matched samples). |
| (b) Audit-driven retry | 3–5 sessions | 0 % (target) | Derive the FULL MISWeight algebra for env-touching strategies before touching code.  Rejected because the previous session ALREADY hand-derived algebra and missed second-order effects — a more rigorous algebra pass risks the same failure mode at higher cost. |
| **(c) PBRT-v4-style port** | **4–6 sessions** | **0 % (target)** | **Selected.**  Mirrors a reference implementation that has been validated on env-IBL scenes for years.  Lets us cite line-for-line correctness against a known-good design instead of re-deriving algebra. |

**Selection rationale**: option (c) trades algebra-derivation risk for porting risk.  Algebra risk is what burned the previous session; porting risk is bounded by faithful reproduction.  The PBRT-v4 architecture is small enough to fit into the existing RISE BDPT (no rewrite of `MISWeight` required — see §7 architectural divergence — only stage-restore-callers re-route through a new measure-aware accessor).

**Numbers backing the selection**:
- Session 2 residual after Phase 1.B–1.E attempt: BDPT 6–9 % of PT on env+omni / env+mesh; VCM ~36 % of PT on env-only spectral HWSS=true.  Reverted to Phase 1.A only.
- Pre-Phase-1 residual: 15–22 % under PT, broadly across topologies.
- Phase 1.A landed: foundation helper present, behaviour-identical.

---

## §2. PBRT-v4's architectural model (annotated)

All citations are against `master` branch of the canonical PBRT-v4 repository:
- BDPT integrator + Vertex struct: [`src/pbrt/cpu/integrators.cpp`](https://github.com/mmp/pbrt-v4/blob/master/src/pbrt/cpu/integrators.cpp)
- Infinite-light implementations: [`src/pbrt/lights.cpp`](https://github.com/mmp/pbrt-v4/blob/master/src/pbrt/lights.cpp)

### §2.1 The `Vertex` struct (integrators.cpp:1552–1565)

```cpp
struct Vertex {
    VertexType type;             // {Camera, Light, Surface, Medium}
    SampledSpectrum beta;
    union { EndpointInteraction ei; MediumInteraction mi; SurfaceInteraction si; };
    BSDF bsdf;
    bool delta = false;          // true iff the SAMPLED scatter was Dirac
    Float pdfFwd = 0, pdfRev = 0;
    // ...
};
```

The struct carries pdfFwd and pdfRev in **the same measure for any given vertex**, but the measure differs by vertex type:
- finite vertex (Surface / Medium / Camera / area-light): area measure
- infinite-area-light vertex (Light + `IsInfiniteLight()`): **solid-angle measure**

The cross-vertex consistency is enforced not by a measure tag on the vertex but by the `ConvertDensity` short-circuit at the SA/area boundary (§2.3).

### §2.2 `IsInfiniteLight()` predicate (integrators.cpp:1666–1670)

```cpp
bool IsInfiniteLight() const {
    return type == VertexType::Light &&
           (!ei.light || ei.light.Type() == LightType::Infinite ||
            ei.light.Type() == LightType::DeltaDirection);
}
```

Three relevant cases pass:
1. The camera-subpath escape vertex (created at integrators.cpp:2062 via `EndpointInteraction(ray)` when a camera-side ray misses geometry) has `ei.light == nullptr`.
2. An importance-sampled env light has `ei.light.Type() == Infinite`.
3. A distant / directional light has `ei.light.Type() == DeltaDirection`.

The latter is bundled in because directional lights share the disc-parameterization convention with env IBL.  RISE's current `BDPTVertex::IsInfiniteLight()` predicate (added in Phase 1.A) is `pEnvLight != 0`, which covers cases (1) and (2) but **NOT** directional lights — see §3 for the gap and §7 for treatment.

Companion predicates:
- `IsLight()` (integrators.cpp:1660): `type == Light || (type == Surface && si.areaLight)`.  Matches an area-light surface hit on the eye subpath.
- `IsDeltaLight()` (integrators.cpp:1662–1664): true for `PointLight`, `SpotLight`, `DeltaDirection`.  **`Infinite` env lights are NOT delta** — they fully participate in MIS as a connectible strategy.
- `IsOnSurface()` (integrators.cpp:1627): `ng() != Normal3f()`.  False for the light-subpath env vertex (whose `Interaction` has zero normal); true for the camera-escape env vertex (whose `EndpointInteraction(Ray)` sets `n = Normal3f(-ray.d)`).

### §2.3 `ConvertDensity` — the key function (integrators.cpp:1731–1743)

```cpp
Float ConvertDensity(Float pdf, const Vertex &next) const {
    // Return solid angle density if _next_ is an infinite area light
    if (next.IsInfiniteLight())
        return pdf;

    Vector3f w = next.p() - p();
    if (LengthSquared(w) == 0)
        return 0;
    Float invDist2 = 1 / LengthSquared(w);
    if (next.IsOnSurface())
        pdf *= AbsDot(next.ng(), w * std::sqrt(invDist2));
    return pdf * invDist2;
}
```

This is the linchpin.  Takes an **SA-measure pdf at `this`** (typically a BSDF / phase / camera-We directional pdf for direction `this → next`) and returns the **canonical measure at `next`**:
- env destination → SA, identity passthrough (line 1733).
- camera/medium/surface destination → area-measure (lines 1737–1742).  The `IsOnSurface` gate kills the cos when `next.ng()` is the sentinel zero normal.

RISE Phase 1.A (commit `a4a24b85`) added the matching `BDPTUtilities::ConvertDensity(pdfSA, from, to)` free function at [BDPTUtilities.h:49–79](../src/Library/Utilities/BDPTUtilities.h).  **No call site migrates yet** — this design specifies which sites to migrate and in what order.

### §2.4 `Vertex::PDF` — unified dispatch (integrators.cpp:1745–1776)

```cpp
Float PDF(const Integrator &integrator, const Vertex *prev,
          const Vertex &next) const {
    if (type == VertexType::Light)
        return PDFLight(integrator, next);
    // ...compute wn = (next - this).normalized
    // ...compute wp = (prev - this).normalized
    Float pdf = 0, unused;
    if (type == VertexType::Camera)
        ei.camera.PDF_We(ei.SpawnRay(wn), &unused, &pdf);
    else if (type == VertexType::Surface)
        pdf = bsdf.PDF(wp, wn);
    else if (type == VertexType::Medium)
        pdf = mi.phase.p(wp, wn);
    // Return probability per unit area at vertex _next_
    return ConvertDensity(pdf, next);
}
```

The unified `Vertex::PDF` is what `MISWeight` calls to recompute pdfRev for hypothetical reverse-direction strategies (see §2.7).  Two env-aware behaviours fall out automatically:
- **`this` is a light vertex (env or finite)**: dispatch to `PDFLight` (§2.5).
- **`next` is an env vertex**: the `ConvertDensity(pdf, next)` tail returns SA unchanged.

### §2.5 `Vertex::PDFLight` — env vertex as source (integrators.cpp:1778–1814)

```cpp
Float PDFLight(const Integrator &integrator, const Vertex &v) const {
    Vector3f w = v.p() - p();
    Float invDist2 = 1 / LengthSquared(w);
    w *= std::sqrt(invDist2);
    Float pdf;
    if (IsInfiniteLight()) {
        // Compute planar sampling density for infinite light sources
        Bounds3f sceneBounds = integrator.aggregate.Bounds();
        Point3f sceneCenter; Float sceneRadius;
        sceneBounds.BoundingSphere(&sceneCenter, &sceneRadius);
        pdf = 1 / (Pi * Sqr(sceneRadius));
    } else if (IsOnSurface()) {
        // ...DiffuseAreaLight area pdf...
        Float pdfPos, pdfDir;
        light.PDF_Le(ei, w, &pdfPos, &pdfDir);
        pdf = pdfDir * invDist2;
    } else {
        // ...point/spot light directional pdf...
        Float pdfPos, pdfDir;
        ei.light.PDF_Le(Ray(p(), w, time()), &pdfPos, &pdfDir);
        pdf = pdfDir * invDist2;
    }
    if (v.IsOnSurface())
        pdf *= AbsDot(v.ng(), w);
    return pdf;
}
```

For an env source vertex, the returned pdf is the **disc-area density** `1/(πr²)` projected onto `v`'s area neighbourhood via the `cos(v.ng, w)` factor.  Note: PBRT-v4 ASKS for the area pdf at `v`, not the SA pdf at the env vertex.  This is how the area-measure framework absorbs the env-emission geometry: the SampleLe `pdfPos` lives at the **first scene vertex**, not the env vertex itself.

### §2.6 `Vertex::PDFLightOrigin` — env-as-NEE origin (integrators.cpp:1816–1836)

```cpp
Float PDFLightOrigin(const std::vector<Light> &infiniteLights, const Vertex &v,
                     LightSampler lightSampler) {
    Vector3f w = v.p() - p();
    w = Normalize(w);
    if (IsInfiniteLight()) {
        return InfiniteLightDensity(infiniteLights, lightSampler, w);
    } else {
        // pdfChoice * pdfPos for finite lights
    }
}
```

With (integrators.cpp:2209–2215):
```cpp
Float InfiniteLightDensity(const std::vector<Light> &infiniteLights,
                           LightSampler lightSampler, Vector3f w) {
    Float pdf = 0;
    for (const auto &light : infiniteLights)
        pdf += light.PDF_Li(Interaction(), -w) * lightSampler.PMF(light);
    return pdf;
}
```

This is the **NEE-side SA density** for the env light along direction `-w`: the sampler's `PMF(envLight) × envLight.PDF_Li(-w)`.  The value used by MISWeight when treating s=1 (NEE) as the alternative to a strategy that hit the env directly.

**Critical insight that the Session 2 attempt missed**: PDFLightOrigin returns `InfiniteLightDensity` (the NEE-side density), NOT `pdfDir` (the SampleLe-side density).  In RISE these can differ:
- `pdfSelect_emit × pdfDir` is what `SampleEnvLightEmission` produces (currently `1 × pdfDir`).
- `envSelectProb_NEE × pdfDir` is what direct env-NEE would have produced (currently 0 in mixed-light scenes per `EnvSelectProbability()` returning 0 when alias table is populated).

In the env-only-scene case (single env light, no alias-table mixed lights), the two coincide.  In mixed scenes (env + omni / env + mesh), `envSelectProb_NEE = 0` while `pdfSelect_emit = 1` — they differ.  The Session 2 attempt wrote `pdfSelect_emit × pdfDir` into `path[0].pdfFwd`; PBRT-v4 would write `envSelectProb_NEE × pdfDir`.  See §5 Hypothesis 4.

### §2.7 `MISWeight` walk (integrators.cpp:2129–2207)

PBRT-v4's `MISWeight` is structurally different from RISE's:

```cpp
Float MISWeight(const Integrator &integrator, Camera camera, Vertex *lightVertices,
                Vertex *cameraVertices, Vertex &sampled, int s, int t,
                LightSampler lightSampler) {
    if (s + t == 2) return 1;
    Float sumRi = 0;
    auto remap0 = [](float f) -> Float { return f != 0 ? f : 1; };

    // ... pointer setup: qs, pt, qsMinus, ptMinus ...

    // Temporarily inject `sampled` into the connection slot
    ScopedAssignment<Vertex> a1;
    if (s == 1) a1 = {qs, sampled};
    else if (t == 1) a1 = {pt, sampled};

    // Force connection vertices non-delta for the duration of the walk
    ScopedAssignment<bool> a2, a3;
    if (pt) a2 = {&pt->delta, false};
    if (qs) a3 = {&qs->delta, false};

    // ScopedAssignment overrides of pdfRev:
    //   pt->pdfRev      = qs ? qs->PDF(...) : pt->PDFLightOrigin(...)
    //   ptMinus->pdfRev = qs ? pt->PDF(...) : pt->PDFLight(...)
    //   qs->pdfRev      = pt->PDF(...)
    //   qsMinus->pdfRev = qs->PDF(...)

    // Walk the camera subpath; accumulate ri *= pdfRev/pdfFwd; sumRi += ri.
    // Walk the light subpath; same accumulation pattern.
    // Skip rule: !v[i].delta && !v[i-1].delta (with the i==0 special case
    // testing lightVertices[0].IsDeltaLight() — Infinite env vertex is NOT
    // delta and IS counted).

    return 1 / (1 + sumRi);
}
```

There is **no explicit "next is infinite light" branch in MISWeight itself**.  Env handling is entirely delegated to `ConvertDensity` (§2.3 — short-circuits at env destinations) and the `Vertex::PDF` / `PDFLight` / `PDFLightOrigin` dispatch (§2.4–§2.6).  MISWeight just walks the precomputed `pdfFwd` / `pdfRev` arrays *plus* the per-walk overrides.

This means the env-vertex's `pdfFwd` and `pdfRev` are both **SA-measure** during the walk, and the adjacent vertex's `pdfFwd` / `pdfRev` are both **area-measure** as usual.  The ratio `pdfRev / pdfFwd` at the env vertex is dimensionally `SA / SA = unitless`; the ratio at the adjacent vertex is `area / area = unitless`.  No mixing, no measure-conversion required inside the walk.

### §2.8 `G` — geometric term (integrators.cpp:2117–2127)

```cpp
SampledSpectrum G(const Integrator &integrator, Sampler sampler, const Vertex &v0,
                  const Vertex &v1, const SampledWavelengths &lambda) {
    Vector3f d = v0.p() - v1.p();
    Float g = 1 / LengthSquared(d);
    d *= std::sqrt(g);
    if (v0.IsOnSurface()) g *= AbsDot(v0.ns(), d);
    if (v1.IsOnSurface()) g *= AbsDot(v1.ns(), d);
    return g * integrator.Tr(v0.GetInteraction(), v1.GetInteraction(), lambda);
}
```

Each cos factor is gated on `IsOnSurface()`.  For the light-subpath env vertex (zero normal sentinel), the env-side cos is dropped.  PBRT-v4's `ConnectBDPT` never actually invokes G at the env vertex (s=0 uses `pt.Le() * pt.beta` directly without G; s=1 uses `f * AbsDot(wi, ns)` without G), so this gating is defensive but never load-bearing for env paths.

### §2.9 `GenerateLightSubpath` env-vertex post-walk fixup (integrators.cpp:1932–1959)

This is **the most important PBRT-v4 piece** and the one the Session 2 attempt missed.

```cpp
// Initial vertex placement
Float p_l = lightSamplePDF * les->pdfPos;            // disc-area pdfPos
path[0] = les->intr ? Vertex::CreateLight(light, *les->intr, les->L, p_l)
                    : Vertex::CreateLight(light, ray, les->L, p_l);
SampledSpectrum beta = les->L * les->AbsCosTheta(ray.d) / (p_l * les->pdfDir);

int nVertices = RandomWalk(integrator, lambda, ray, sampler, camera, scratchBuffer,
                           beta, les->pdfDir, maxDepth - 1, TransportMode::Importance,
                           path + 1, regularize);

// Correct subpath sampling densities for infinite area lights
if (path[0].IsInfiniteLight()) {
    // Set spatial density of _path[1]_ for infinite area light
    if (nVertices > 0) {
        path[1].pdfFwd = les->pdfPos;
        if (path[1].IsOnSurface())
            path[1].pdfFwd *= AbsDot(ray.d, path[1].ng());
    }
    // Set spatial density of _path[0]_ for infinite area light
    path[0].pdfFwd =
        InfiniteLightDensity(integrator.infiniteLights, lightSampler, ray.d);
}
```

Two post-walk overrides:
1. **`path[1].pdfFwd = pdfPos × cos(d, n1)`**.  Overrides what `RandomWalk` set via the normal `ConvertDensity(pdfDir, path[1])` chain.  The override moves the SampleLe `pdfPos` from the env vertex onto `path[1]` as a **projected area density**.
2. **`path[0].pdfFwd = InfiniteLightDensity(...)`**.  Overrides the initial `p_l = pdfSelect × pdfPos` value with the **NEE-side SA density**.  This is what closes the s=0 ↔ s=1 MIS comparison: when the eye subpath hits an env vertex (s=0 strategy), the alternative is s=1 NEE, whose pdf is exactly `InfiniteLightDensity`.

This redistribution is the design's keystone.  Without (1), `path[1].pdfFwd` is in SA but `path[2]` is in area — measure mismatch inside the walk.  Without (2), the env vertex's `pdfFwd` is the emission-side product `pdfSelect_emit × pdfPos`, which doesn't match what s=1 NEE would compute (`InfiniteLightDensity` ≠ `pdfSelect_emit × pdfPos` in general).

### §2.10 `ConnectBDPT` env handling (integrators.cpp:2324–2445)

Three relevant branches:

**Dedupe guard** (line 2330):
```cpp
if (t > 1 && s != 0 && cameraVertices[t - 1].type == VertexType::Light)
    return SampledSpectrum(0.f);
```
The eye subpath already ended on a light (area or env); only s=0 is legal.  RISE has the analogous guard implicitly via `ConnectAndEvaluate`'s branching on `eyeEnd.type`.

**s=0 (eye terminated on a light)** (lines 2336–2340):
```cpp
const Vertex &pt = cameraVertices[t - 1];
if (pt.IsLight())
    L = pt.Le(integrator.infiniteLights, cameraVertices[t - 2], lambda) * pt.beta;
```
For env, `pt.Le` dispatches to `Vertex::Le`'s env branch (integrators.cpp:1683–1687):
```cpp
SampledSpectrum Le(0.f);
for (const auto &light : infiniteLights)
    Le += light.Le(Ray(p(), -w), lambda);
```
No G factor; throughput absorbs everything.

**s=1 (NEE)** (lines 2385–2407): uses `SampleLi`; sets `sampled.pdfFwd = PDFLightOrigin(...)` so the env's NEE-SA pdf is stored on the sampled vertex.

---

## §3. RISE-vs-PBRT-v4 architectural diff map

**(revised 2026-05-28)** — see [§0.1](#01-new-diff-map-row-d19--eye-side-env-vertex-pdffwd) for row **D19** (eye-side env-vertex `pdfFwd`, missing in the v1 table below).  Row D19 is mandatory for any landing of the s=0 group (D4 + D5 + D19).

The table below lists every PBRT-v4 mechanism from §2 and the RISE-side equivalent.  This is the source of truth for §4's per-piece plan: every code change in §4 must be justified by a row in this table.

| # | Concern | PBRT-v4 (file:line) | RISE current (file:line) | Adaptation needed |
|---|---|---|---|---|
| **D1** | Env vertex type | `Vertex::type==Light` + `ei.light` null/Infinite/DeltaDirection (`integrators.cpp:1666-1670`) | `BDPTVertex::IsInfiniteLight() == pEnvLight != 0` ([BDPTVertex.h:211](../src/Library/Shaders/BDPTVertex.h:211)) | **None for env**.  DeltaDirection (sun/distant) is out of scope — RISE has no directional-light type that goes through this path (omni/spot/area only).  If RISE adds directional later, extend `IsInfiniteLight()` then. |
| **D2** | `pdfFwd` measure on env vertex | **SA-measure NEE-side density** `InfiniteLightDensity(...)`, set post-walk (`integrators.cpp:1957`) | Disc-area `pdfSelect × 1/(πr²)` set at light-subpath init ([BDPTIntegrator.cpp:1420](../src/Library/Shaders/BDPTIntegrator.cpp:1420)) | **Migrate**: post-`GenerateLightSubpath` fixup that overwrites `vertices[0].pdfFwd` with `envSelectProb_NEE × pdf_env_sa(wi)`.  Mirror PBRT-v4's pattern exactly. |
| **D3** | `pdfFwd` measure on `path[1]` (first scene vertex after env emit) | **Projected disc-area** `pdfPos × cos(ray.d, n1)` set post-walk (`integrators.cpp:1951-1954`) | Standard `SolidAngleToArea(pdfFwdPrev=pdfDir, absCosIn, distSq)` ([BDPTIntegrator.cpp:1716](../src/Library/Shaders/BDPTIntegrator.cpp:1716)) where `pdfFwdPrev = pdfDirArea = pdf_env_sa(wi)` | **Migrate**: post-walk overwrite `vertices[1].pdfFwd = (1/(πr²)) × cos(rayDir, n1)`.  Note the JACOBIAN: PBRT-v4 uses the disc `pdfPos` here, NOT the env directional pdf — that's intentional, the SA→area conversion via `pdf_env_sa × cos / dist²` would NOT equal `pdf_env_sa × cos × invR²` (they only agree if `dist == r`, which only holds asymptotically). |
| **D4** | `pdfRev` on env vertex (when eye lands on env, Path B s=0 site) | `pt->pdfRev = pt->PDFLightOrigin(...)` set inside MISWeight via `ScopedAssignment` (`integrators.cpp:2163-2165`) | `eyeEnd.pdfRev = envSelectProb × 1/(πr²)` (disc-area) with `kEnvZeroSentinel` workaround, set in caller before MISWeight ([BDPTIntegrator.cpp:3434-3454](../src/Library/Shaders/BDPTIntegrator.cpp:3434)) | **Migrate**: caller writes SA-measure `envSelectProb × pdf_env_sa(wiSky)`; drop `kEnvZeroSentinel` (the sentinel is only needed because the old disc-area value could be small but nonzero and the new SA value is naturally zero when `envSelectProb == 0`). |
| **D5** | `pdfRev` on env-vertex's predecessor (eyePred at Path B s=0 site) | `ptMinus->pdfRev = pt->PDFLight(...)` set inside MISWeight (`integrators.cpp:2171-2173`) | `eyePred.pdfRev = SolidAngleToArea(envSelectProb × pdf_env_sa, absCosAtPred, distPredSq)` with sentinel ([BDPTIntegrator.cpp:3469-3486](../src/Library/Shaders/BDPTIntegrator.cpp:3469)) | **Keep area-conversion** (PBRT-v4 also does it: `PDFLight` env branch returns `pdf × invDist² × cos(v.ng, w)`).  Drop the `envSelectProb` factor — PBRT-v4's `PDFLight` does NOT multiply by sampler PMF; that lives only in `PDFLightOrigin`.  This is the **measure-asymmetry** that Session 2 missed.  Drop sentinel. |
| **D6** | `pdfRev` on light-start env vertex (Path A s=1 site) | `qs->pdfRev = pt->PDF(integrator, ptMinus, *qs)` via MISWeight `ScopedAssignment` (`integrators.cpp:2180-2181`).  `pt->PDF` dispatches to surface `bsdf.PDF` then `ConvertDensity(pdf, qs)` — short-circuits at env. | `lightStart.pdfRev = SolidAngleToArea(pdfRevSA_bsdf, absCosAtLight, distSq_conn)` ([BDPTIntegrator.cpp:4002-4006](../src/Library/Shaders/BDPTIntegrator.cpp:4002)) | **Migrate**: use `ConvertDensity(pdfRevSA_bsdf, eyeEnd, lightStart)` which short-circuits at env → returns SA unchanged. |
| **D7** | Light-subpath next-vertex `pdfFwd` propagation off env | Goes through normal `ConvertDensity` in `RandomWalk` (would convert SA→area), then POST-WALK overridden to projected disc-area via (D3). | Goes through `SolidAngleToArea(pdfFwdPrev=pdf_env_sa, absCosIn, distSq)` at [BDPTIntegrator.cpp:1716](../src/Library/Shaders/BDPTIntegrator.cpp:1716). | **Same final state via post-walk override** (D3).  No change to the intermediate `pdfFwdPrev` machinery — only the post-walk overwrite. |
| **D8** | s=1 NEE: BDPT light-vertex insertion at connection | `sampled.pdfFwd = PDFLightOrigin(...)` (`integrators.cpp:2398`) → InfiniteLightDensity for env (SA NEE-density) | s=1 NEE writes the env-vertex pdfFwd via `lightStart.pdfFwd` already filled by `GenerateLightSubpath` — but only when light-subpath sampling chose env-emit, NOT during a per-eye-vertex NEE pass.  The current Path A site at [BDPTIntegrator.cpp:3944-3984](../src/Library/Shaders/BDPTIntegrator.cpp:3944) bypasses pdfLight via PT-formula override for contribution; MIS still consumes `lightStart.pdfFwd` which holds the disc-area value. | **Migrate**: once (D2) lands, `lightStart.pdfFwd` is already the SA NEE-density.  The Path A site's PT-formula contribution override still works (it bypasses pdfLight entirely).  The MIS chain then reads SA `pdfFwd` and SA `pdfRev` (D6) at the env vertex consistently. |
| **D9** | MISWeight walk itself | In-walk `ScopedAssignment` to recompute pdfRev per (s,t) strategy (`integrators.cpp:2153-2187`) | Caller stages pdfRev before MISWeight, restores after (`ConnectAndEvaluate` pattern at all 4 sites) | **NO CHANGE TO MISWeight ITSELF.**  RISE's stage-restore pattern is architecturally different but functionally equivalent IF every stage-restore site uses the correct measure-aware computation (rows D4–D6).  See §7 for why we don't port the in-walk pattern. |
| **D10** | `G` cosine gating at env | Gated on `IsOnSurface()` — env light-subpath vertex (zero normal) skips its cos; never actually invoked at env via ConnectBDPT's s=0 branch | RISE's G at the env site is in Path B s=0 contribution: NOT invoked (uses `eyeEnd.throughput * Le`); at Path A s=1: bypassed via PT-formula override.  G appears only in s≥2 inner connections, which never have env as a connection endpoint. | **No change**.  The env vertex is never an s≥2 connection endpoint in either PBRT-v4 or RISE. |
| **D11** | Skip rule at env-light vertex | Light-side skip tests `IsDeltaLight()` at `i==0` (`integrators.cpp:2199`).  `Infinite` env vertex returns false → NOT skipped → fully participates as a connectable strategy. | RISE skip rule at [BDPTIntegrator.cpp:4933-4940](../src/Library/Shaders/BDPTIntegrator.cpp:4933) tests `vi.isDelta` (per-vertex sampled-lobe flag) and a special-case for delta-position lights at `i==1`.  Env vertex has `isDelta = false` (set at LightSampler.cpp:1152) → NOT skipped.  **Already correct.** | **No change**.  Verify: an explicit unit-test assertion that env vertex's `isDelta == false` survives the migration. |
| **D12** | `pdfFwd / pdfRev` ratio at env vertex inside walk | Both SA-measure → `unitless ratio` (`integrators.cpp:2196`) | After migration: both SA-measure → `unitless ratio`.  Before migration: both disc-area → also unitless but at wrong magnitude. | **Implicit from D2 + D4 + D5 + D6**.  No standalone code change. |
| **D13** | Throughput at env vertex | `beta = Le * cos / (p_l * pdfDir) = Le / pdfDir` (disc-area factors cancel) at light-subpath init.  No change post-walk. | `v.throughput = ls.Le / v.pdfFwd = ls.Le × πr²` at init ([BDPTIntegrator.cpp:1425](../src/Library/Shaders/BDPTIntegrator.cpp:1425)); subsequent `beta /= pdfEmit` recovers final beta after init = `Le × πr² / (πr² × pdf_env_sa) = Le / pdf_env_sa`. | **Subtle**: the initial `v.throughput` will change magnitude after (D2) (`Le / pdf_env_sa` instead of `Le × πr²`), but the BDPT path-contribution formula at connection sites uses `throughput` *after* the subpath has been walked (i.e. beta).  Beta itself is unchanged because the cancellation works out.  **Verify with a unit-test print at the s=0 contribution site**: env-only Lambertian renders should be magnitude-identical to PT after (D2)+(D4)+(D5). |
| **D14** | VCM dVCM init at light endpoint | `dVCM = directPdfA / emissionPdfW = (pdfSelect × pdfPos) / (pdfSelect × pdfPos × pdfDir) = 1/pdfDir`.  Cancellation works out the same for env. | `mis = InitLight(v.pdfFwd, v.emissionPdfW, v.cosAtGen, isFiniteLight=true, isDelta, norm)` ([VCMIntegrator.cpp:469-477](../src/Library/Shaders/VCMIntegrator.cpp:469)) with `isFiniteLight = true` (hardcoded). | **Migrate**: `isFinite = (v.pEnvLight == 0)`.  When (D2) ships, `v.pdfFwd = envSelectProb × pdf_env_sa`; `v.emissionPdfW = pdfSelect × pdfPos × pdfDir = 1 × (1/πr²) × pdf_env_sa` (unchanged from current, because we DON'T migrate SampleEnvLightEmission's pdfPos field — that's the Session 2 trap).  `dVCM_env = (envSelectProb × pdf_env_sa) / ((1/πr²) × pdf_env_sa) = envSelectProb × πr²` — which is mathematically what Georgiev 2012 Appendix A's env special-case expects. |
| **D15** | VCM first-bounce `applyDistSqToDVCM` | Implicit via `(pathLength > 1 \|\| isFiniteLight)` gate in SmallVCM, mirrored by RISE's `applyDistSqToDVCM` param. | Hardcoded `applyDistSqToDVCM = true` ([VCMIntegrator.cpp:549](../src/Library/Shaders/VCMIntegrator.cpp:549)). | **Migrate**: `applyDistSqToDVCM = !(i == 1 && verts[0].pEnvLight != 0)`.  Skips the `dVCM *= distSq` step on the first bounce off an env light.  This is the Georgiev 2012 Appendix A env special case. |
| **D16** | NM/spectral twin sites | Single Vertex<float> in PBRT-v4; SampledWavelengths handles HWSS uniformly across the SA/area boundary. | RGB has dedicated sites, NM/spectral has parallel dedicated sites (BDPTIntegrator.cpp:5183, 6332, 7001, 7287, 7548).  RISE has not yet completed Phase 2c BDPT templatization. | **Mirror every RGB change in the NM twin** in the same commit.  Audit HWSS bookkeeping at NM:5223 (Le pdfEmit division uses ls.pdfPosition × ls.pdfDirection per wavelength) — if (D2) does NOT change the SampleEnvLightEmission output (it doesn't — see §4 piece B), the HWSS path is unaffected.  This is the safety margin that protects from the Session 2 spectral-HWSS=true catastrophic regression. |
| **D17** | Path A s=1 contribution-formula PT-override | PBRT-v4 has no analog — `ConnectBDPT` s=1 uses the natural `f × Le × cos × G / (pdfLi × pdfChoice)` formula directly, with SA-measure pdfs throughout. | RISE Path A overrides the contribution at [BDPTIntegrator.cpp:3944-3976](../src/Library/Shaders/BDPTIntegrator.cpp:3944) via `eyeEnd.throughput * fEye * Le * Tr * (cosEyeWi / pdfSA)` to dodge the disc-area pdfLight mismatch. | **Keep the PT-formula override.**  Removing it would require switching `lightStart.pdfFwd` semantics in a way that breaks the s=1 NEE-path lookup at BDPTIntegrator.cpp:3921 (`pdfLight = lightStart.pdfFwd`).  The PT-formula override is functionally correct and produces the same numerical contribution as the SA-measure natural formula — only the MIS bookkeeping needs to be made consistent with the new SA-measure storage convention. |
| **D18** | OpenPGL guiding NEE-RIS env handling | PBRT-v4 has no path-guiding integration; OpenPGL is a RISE-specific extension. | `GuidingRISCandidate` records the s=1 env contribution as a guiding sample with its disc-area pdf in the candidate record (BDPTIntegrator.cpp ~3974). | **Audit**: the guiding training pdf field needs to stay in SA after the migration.  If guiding consumes `cosEyeWi / pdfSA` (which is the PT-formula override's directional-pdf signature), nothing changes.  If it consumes `lightStart.pdfFwd` directly, that field's measure now changes — adapt. |

### §3.1 What's NOT in the diff map (out of scope for option (c))

- **SampleEnvLightEmission's pdfPosition/pdfDirection split**: Session 2 changed `pdfPosition` to `pdf_env_sa` (collapsing pdfDir to 1).  This **violates the PBRT-v4 design** — PBRT-v4 keeps the disc-`pdfPos = 1/(πr²)` and uses it (a) for `pdfFwd` redistribution at `path[1]`, and (b) for the throughput's `1/(p_l × pdfDir) = πr² / pdf_env_sa` factor.  Collapsing pdfPos into pdfDir loses the bookkeeping needed by both.  **DO NOT change `SampleEnvLightEmission`** in this design.  This is the single most important deviation from the Session 2 spec.
- **MISWeight rewrite to in-walk ScopedAssignment**: PBRT-v4's `ScopedAssignment` pattern is architecturally different but functionally equivalent to RISE's caller-stages-pdfRev pattern.  Switching architectures is a much larger refactor and risks introducing bugs unrelated to env handling.  Out of scope.  See §7.
- **DeltaDirection / sun-light support**: RISE has no directional-light type that goes through the env-light path.  If RISE adds one later, the `IsInfiniteLight()` predicate widens to cover it.
- **`G` cos-gating at env vertex**: PBRT-v4's `IsOnSurface()` gate matters for correctness in some hypothetical configurations, but RISE doesn't invoke G at the env vertex in any current call path.  Defensive change not needed.

---

## §4. Per-piece execution plan

**(revised 2026-05-28)** — [§0.2](#02-revised-piece-decomposition--group-by-transport-side) supersedes this section's decomposition.  The v1 per-row pieces (2.B = D4, 2.C = D5, 2.D = D2+D3, 2.E = D6) cannot land independently because their MIS-walk effects on adjacent vertices are coupled within each transport-side group (Session 5 catastrophic regression evidence).  Pieces 2.B and 2.C are *NOT* safely landable individually; they must land jointly as the **s=0 group** (with D19 included).  Pieces 2.D and 2.E must land jointly as the **light-subpath group**.  Read §0.2 for the revised piece list (2.A → 2.B' → 2.C' → 2.D' → 2.E' → 2.F') and the recommended sequencing.

The plan is decomposed into **seven pieces** [DEPRECATED — replaced by §0.2's group-based decomposition], each producing a *test-green checkpoint*.  This decomposition is the direct cure for the Session 2 failure mode (attempted B+C+D+E as a unit, all-or-nothing landing with no incremental gate).

Each piece lists:
- **Goal**: what changes.
- **Sites**: file:line refs from §3 diff map.
- **Test gate**: which EnvLightBalanceTest topology should pass (incremental coverage).
- **Non-scope**: explicit deliberate exclusion to prevent over-reach.
- **Estimated sessions**: incremental cost.

### Piece 2.A — Audit-mode parallel computation (no behaviour change)

**Goal**: add a `static const bool kSAMisAudit = false` compile-time guard at the top of `BDPTIntegrator.cpp`.  When defined `true`, sites that will be migrated in pieces 2.B–2.F additionally compute the new SA-measure value, log it alongside the current disc-area value, and assert they agree on the env-only-no-alias-table topology (where the values are mathematically identical).  When `false` (production default), the new computation is dead code.

**Sites**: BDPTIntegrator.cpp Path B s=0 (3434-3486), Path A s=1 (4002-4006, 4035-4036, 4067-4069), light-subpath init (1420), and the NM twins.  Roughly 10 print-and-assert insertion points.

**Test gate**: 116/116 tests pass; EnvLightBalanceTest passes at lax tolerances.  Manually flip `kSAMisAudit = true`, rebuild, render the env-only Lambertian topology, confirm assertions hold.

**Non-scope**: NO actual behaviour change.  This piece exists only to prove the SA-measure computation produces the expected value before any site is migrated.

**Estimated**: 0.5 session.

### Piece 2.B — Migrate Path B s=0 env-vertex pdfRev (RGB + NM)

**Goal**: Replace the disc-area `envSelectProb × 1/(πr²) + kEnvZeroSentinel` formula with the SA-measure `envSelectProb × pdf_env_sa(wiSky)` per row D4.  Drop the sentinel.

**Sites**: [BDPTIntegrator.cpp:3434-3454](../src/Library/Shaders/BDPTIntegrator.cpp:3434) (RGB) and [BDPTIntegrator.cpp:7027-7043](../src/Library/Shaders/BDPTIntegrator.cpp:7027) (NM/spectral).  ONLY the env-vertex's own `pdfRev` field; the predecessor's pdfRev (eyePred) stays as it is in piece 2.B and is migrated in piece 2.C.

**Test gate**:
- 116/116 tests pass.
- EnvLightBalanceTest at lax tolerances: **must improve** on the env-only Lambertian RGB topology (BDPT closer to PT than the 107 % current value).
- EnvLightBalanceTest at lax tolerances: **must NOT regress** on env+omni / env+mesh topologies.  If they regress, STOP — the env-vertex pdfRev migration interacts with the predecessor pdfRev in a way that needs joint migration (would force piece 2.B and 2.C into a single landing).

**Non-scope**: do NOT touch the predecessor pdfRev (eyePred — that's piece 2.C).  Do NOT touch the light-subpath init pdfFwd (that's piece 2.D).  Do NOT touch any VCM site.

**Estimated**: 0.5 session.

### Piece 2.C — Migrate Path B s=0 eyePred pdfRev (RGB + NM)

**Goal**: Replace the existing eyePred pdfRev computation per row D5: change from `SolidAngleToArea(envSelectProb × pdf_env_sa, absCosAtPred, distPredSq)` to **`SolidAngleToArea(pdf_env_sa, absCosAtPred, distPredSq)`** — drop the `envSelectProb` factor.  PBRT-v4's `PDFLight` env branch does NOT multiply by sampler PMF; that lives only in `PDFLightOrigin` (which is the env vertex's own pdfRev = piece 2.B).  This is **the measure-asymmetry that Session 2 missed**.

**Sites**: [BDPTIntegrator.cpp:3469-3486](../src/Library/Shaders/BDPTIntegrator.cpp:3469) (RGB) and [BDPTIntegrator.cpp:7046-7068](../src/Library/Shaders/BDPTIntegrator.cpp:7046) (NM).

**Test gate**:
- 116/116 tests pass.
- EnvLightBalanceTest at lax tolerances: env+omni / env+mesh topologies must improve (closer to PT).  These topologies are the load-bearing oracle for this piece — they exercise the alias-table-populated `envSelectProb = 0` path.  If they REGRESS, the `envSelectProb`-drop hypothesis is wrong and we need to audit more carefully.

**Non-scope**: do NOT touch s=1 NEE sites (Path A — piece 2.E).  Do NOT touch the light-subpath init pdfFwd (piece 2.D).

**Estimated**: 0.5 session.

### Piece 2.D — Light-subpath env-vertex pdfFwd post-walk override (RGB + NM)

**Goal**: After `GenerateLightSubpath` returns with `vertices[0].pEnvLight != 0`, overwrite `vertices[0].pdfFwd = envSelectProb × pdf_env_sa(wi)` per row D2.  Also overwrite `vertices[1].pdfFwd = (1/(πr²)) × cos(rayDir, n1)` per row D3 (when `vertices.size() > 1`).  This is the **PBRT-v4 fixup pattern** from §2.9.

**Sites**: append fixup block at the end of [`GenerateLightSubpath`](../src/Library/Shaders/BDPTIntegrator.cpp:1394) RGB (after line ~2050 where the function returns) AND at the end of the NM twin (`GenerateLightSubpathNM` around line 5180).

```cpp
// PBRT-v4 §15.5.2 post-walk fixup for infinite-area light subpaths.
// Mirrors integrators.cpp:1948-1959.  Replaces the disc-area pdfFwd
// on the env vertex with the NEE-side SA density, and the SA-derived
// pdfFwd on path[1] with the projected disc-area density.
if (!vertices.empty() && vertices[0].pEnvLight != 0 && pLightSampler) {
    const Scalar envSelectProb_NEE = pLightSampler->EnvSelectProbability();
    const EnvironmentSampler* pEnvSamp = pLightSampler->GetEnvironmentSampler();
    if (pEnvSamp) {
        // Recover wi from stored geomNormal (= -wi, by SampleEnvLightEmission)
        const Vector3 wi(-vertices[0].geomNormal.x,
                         -vertices[0].geomNormal.y,
                         -vertices[0].geomNormal.z);
        const Scalar pdf_env_sa = pEnvSamp->Pdf(wi);

        // (1) Env vertex: NEE-side SA density.
        vertices[0].pdfFwd = envSelectProb_NEE * pdf_env_sa;

        // (2) path[1] override (projected disc area).
        if (vertices.size() > 1 && vertices[1].type == BDPTVertex::SURFACE) {
            const Scalar sceneRadius = pLightSampler->GetCachedSceneRadius();
            const Scalar discArea = PI * sceneRadius * sceneRadius;
            const Scalar pdfPos_disc = (discArea > 0) ? (Scalar(1) / discArea) : 0;
            const Scalar absCosAtV1 = fabs(Vector3Ops::Dot(
                vertices[1].geomNormal, vertices[0].normal));  // normal = -wi
            vertices[1].pdfFwd = pdfPos_disc * absCosAtV1;
        }
    }
}
```

**Test gate**:
- 116/116 tests pass.
- EnvLightBalanceTest at lax tolerances: env-only Lambertian (RGB + spectral, HWSS=false) must improve toward 100 % of PT.  HWSS=true and mixed-scene topologies should not regress; they may also improve (Hypothesis 3 prediction).
- BDPTStrategyBalanceTest: must NOT regress.  This test exercises non-env scenes and is the canary for collateral damage to the s≥2 light-subpath strategies.

**Non-scope**: do NOT migrate Path A s=1 sites (piece 2.E).  Do NOT touch VCM (piece 2.F).  Do NOT change `SampleEnvLightEmission` (out of scope §3.1).

**Estimated**: 1 session.  Largest single piece because of the dual RGB+NM fixup with HWSS interaction.  HWSS audit is a 2.D sub-step: verify the per-wavelength companion path at BDPTIntegrator.cpp:5261 doesn't read `vertices[0].pdfFwd` differently than the hero wavelength.

### Piece 2.E — Migrate Path A s=1 sites (RGB + NM)

**Goal**: Apply row D6 — replace the s=1 NEE site's `lightStart.pdfRev` setter with `ConvertDensity(pdfRevSA_bsdf, eyeEnd, lightStart)` which short-circuits to SA at env (the Phase 1.A helper).  Also apply the analogous treatment to `eyeEnd.pdfRev` (the emission-side directional pdf evaluated at light → eye direction).

**Sites**: [BDPTIntegrator.cpp:4002-4006](../src/Library/Shaders/BDPTIntegrator.cpp:4002) (lightStart.pdfRev, RGB), [BDPTIntegrator.cpp:4035-4036](../src/Library/Shaders/BDPTIntegrator.cpp:4035) (eyeEnd.pdfRev), [BDPTIntegrator.cpp:4067-4069](../src/Library/Shaders/BDPTIntegrator.cpp:4067) (eyePred.pdfRev), and NM twins around BDPTIntegrator.cpp:7287-7483.

**Test gate**:
- 116/116 tests pass.
- EnvLightBalanceTest at lax tolerances: BDPT must continue improving on every env topology.  s=1 NEE is the alternative strategy for env-touching paths; its MIS-bookkeeping must be consistent with (D2)+(D4)+(D5) for the MIS sum to balance.
- **Adversarial review checkpoint**: at this point all four BDPT env-MIS sites use SA-measure consistently.  Run the adversarial-review skill before going further.

**Non-scope**: VCM is piece 2.F.  Strict-tolerance test family is piece 2.G.

**Estimated**: 0.5 session.

### Piece 2.F — VCM caller flag migration

**Goal**: Apply rows D14 + D15.  At [VCMIntegrator.cpp:469](../src/Library/Shaders/VCMIntegrator.cpp:469), `isFinite = (v.pEnvLight == 0)`.  At [VCMIntegrator.cpp:549](../src/Library/Shaders/VCMIntegrator.cpp:549), `applyDistSqToDVCM = !(i == 1 && verts[0].pEnvLight != 0)`.  Spectral / NM twins: verify symmetry at VCMIntegrator.cpp:1687-1772.

**Test gate**:
- 116/116 tests pass — especially VCMRecurrenceTest and VCMSpectralRecurrenceTest, which test the running-quantities invariants directly.
- EnvLightBalanceTest at lax tolerances: VCM must improve on all env topologies.  The env-only-spectral-HWSS=true scenario from Session 2 (catastrophic regression to ~16 % of PT) is the load-bearing oracle here.

**Non-scope**: this piece does NOT touch BDPT.  If VCM regresses on a non-env scene, that's a sign the recurrence's measure assumption is broken — STOP and re-audit `InitLight`'s `dVCM = directPdfA / emissionPdfW` derivation against the new `v.pdfFwd` semantics.

**Estimated**: 0.5 session.

### Piece 2.G — Tighten tests + documentation

**Goal**: Switch `kEnvTolerances` in EnvLightBalanceTest.cpp:608 from `{ 0.35, 0.35, 2.00 }` to `{ 0.10, 0.30, 1.00 }`.  Confirm all topologies × RGB/spectral × HWSS pass at strict tolerances.  Run the validation gates 4-10 from PRE_PHASE1_STATUS.md.  Update IMPROVEMENTS.md §12 status, CLAUDE.md High-Value Fact, and PRE_PHASE1_STATUS.md.

**Test gate**: full validation per [PRE_PHASE1_STATUS.md](PRE_PHASE1_STATUS.md) §"Post-refactor".

**Non-scope**: no behaviour change beyond doc updates.

**Estimated**: 0.5-1 session, including the K-trial variance run and visual-parity render.

### Cumulative estimate

| Piece | Est. sessions |
|---|---|
| 2.A audit-mode | 0.5 |
| 2.B Path B env-vertex pdfRev | 0.5 |
| 2.C Path B eyePred pdfRev | 0.5 |
| 2.D Light-subpath post-walk fixup | 1.0 |
| 2.E Path A s=1 sites | 0.5 |
| 2.F VCM caller flags | 0.5 |
| 2.G tighten tests + docs | 0.5-1.0 |
| **Total** | **4.0-4.5 sessions** |

Matches the 4-6 session estimate from [PRE_PHASE1_STATUS.md](PRE_PHASE1_STATUS.md) option (c) (within tolerance of the upper bound, with headroom for an adversarial review round and potential revert-and-retry of one piece).

---

## §5. Risk register

For each of the four diagnosis hypotheses in [PRE_PHASE1_STATUS.md](PRE_PHASE1_STATUS.md) §615-682, this section states explicitly how the design addresses or mitigates it.

### §5.1 Hypothesis 1: lightStart.pdfFwd magnitude shift

**Original concern**: Pre-refactor the env vertex's pdfFwd was `pdfSelect / (πr²)` (disc-area, ~1e-5 for r=100).  Post-refactor Session 2's attempt made it `pdfSelect × pdf_env_sa ≈ 0.04` — three orders of magnitude larger.  Cross-strategy MIS balance with s≥2 paths gets inverted.

**Resolution in this design**: row D2 stores `envSelectProb_NEE × pdf_env_sa` at the env vertex.  In the env-only-no-alias-table case (which is the regression-prone topology), `envSelectProb_NEE = 1` — same magnitude as Session 2.  In the mixed-scene case (env+omni / env+mesh), `envSelectProb_NEE = 0`, so the env vertex's `pdfFwd = 0`.  MISWeight's `remap0` handles the zero correctly (env vertex becomes "this strategy can't generate this vertex" — MIS gives s=0 strategy weight 1).  This is structurally **different** from Session 2, which stored `pdfSelect_emit × pdf_env_sa = 1 × pdf_env_sa` even when `envSelectProb_NEE = 0`.

**Confidence**: HIGH.  Row D2 is a direct port of PBRT-v4 line 1957.  The asymmetry between `pdfSelect_emit` and `envSelectProb_NEE` was the conceptual gap in Session 2.

**Residual risk**: the s≥2 MIS chain back-walks through `vertices[1].pdfFwd`, which row D3 overrides to projected disc-area.  If row D3's `absCosAtV1` factor is wrong (e.g. wrong normal sign convention), the s≥2 ratio collapses by `1/cos`.  **Mitigation**: piece 2.D's gate explicitly checks BDPTStrategyBalanceTest non-env scenes for collateral damage — those exercise s≥2 paths through finite lights, sharing the same machinery.

### §5.2 Hypothesis 2: MISWeight walk doesn't understand the SA boundary

**Original concern**: The walk assumes both pdfFwd and pdfRev at every vertex are in the same measure.  Storing the env vertex's pdfFwd/pdfRev in SA while neighbouring vertices stay in area creates a unit mismatch.  The PBRT-v4 fix is `ConvertDensity` at every site.

**Resolution in this design**: the env vertex's pdfFwd (D2) AND pdfRev (D4, D6) are both SA-measure.  The neighbouring vertex's pdfFwd is area-measure (set by `SolidAngleToArea` at light-subpath next-vertex propagation, unchanged) AND its pdfRev is area-measure (set via `SolidAngleToArea` in stage-restore callers, also unchanged for non-env neighbours).  The boundary is at the env vertex itself: ratio `SA/SA` inside the walk → unitless.  Adjacent vertex: ratio `area/area` → also unitless.  No mixing.

The eyePred pdfRev (row D5) is computed via `SolidAngleToArea(pdf_env_sa, absCos, distSq)` — area-measure at eyePred.  This is what `Vertex::PDFLight` returns in PBRT-v4 (§2.5, third return statement: `pdf *= AbsDot(v.ng(), w)` after `pdf = pdfDir * invDist²`).

**Confidence**: HIGH.  This is the core insight of the PBRT-v4 architecture, faithfully reproduced.

**Residual risk**: in the cross-strategy comparison `pdfFwd / pdfRev` at the env vertex, both are SA.  At eyePred, both are area.  But at vertices further down the eye subpath, BOTH are area — which is fine.  The risk is the **light-subpath** s≥2 paths: vertex `i=2, 3, ...` have area-measure pdfFwd that was derived via `ConvertDensity(SA-bsdf-pdf, vertex_i)` — i.e. the pdfFwd at vertex_i is area-at-vertex_i, NOT projected through the env vertex's SA pdf.  The s≥k strategies back-walk these pdfFwd/pdfRev ratios; as long as both are area-measure at each vertex, the walk is consistent.  This is **identical to the non-env-touching s≥k case**, which is known correct.

### §5.3 Hypothesis 3: HWSS companion-wavelength path

**Original concern**: Session 2 changed SampleEnvLightEmission's pdfPos/pdfDir split.  Companion wavelengths use per-wavelength pdfs that might independently break.  Spectral BDPT HWSS=true collapsed to 18 % of PT.

**Resolution in this design**: this design **does NOT change SampleEnvLightEmission** (out of scope §3.1).  `ls.pdfPosition` and `ls.pdfDirection` retain their current disc-area / SA semantics.  The HWSS per-wavelength bookkeeping at BDPTIntegrator.cpp:5261 reads `ls.pdfPosition × ls.pdfDirection` per wavelength — these are unchanged.  The HWSS hero-wavelength throughput uses the same product — unchanged.  Only the env vertex's `pdfFwd` is migrated via post-walk override (piece 2.D), which runs ONCE after all wavelengths are bookkept.

**Confidence**: HIGH.  The HWSS code reads the LightSample fields, not the post-walk-overridden BDPTVertex fields.  The Session 2 regression was caused by changing the LightSample fields (which the HWSS code consumes); this design avoids that change.

**Residual risk**: if the BDPT HWSS companion-wavelength path uses `lightStart.pdfFwd` anywhere (we haven't audited this exhaustively), then the post-walk override does affect HWSS.  **Mitigation**: piece 2.D's test gate explicitly includes the env-only spectral HWSS=true topology.  If it regresses, audit `lightStart.pdfFwd` reads in the spectral integrator.

### §5.4 Hypothesis 4: Mixed-scene env-NEE-in-alias-table

**REVISED 2026-05-28 — the v1 resolution below is INCORRECT.**  See [§0.3](#03-recommended-precondition--delta-aware-remap0).  The factual error: the v1 resolution claimed "the SA pdfRev = 0 ... `remap0` correctly fires for the delta-vertex case ... which is what we want: env vertex can't be generated by s=1 NEE in this case."  This is wrong on two counts.  First, the env vertex has `isDelta = false`, so the "delta-vertex case" descriptor doesn't apply to it.  Second, RISE's `remap0` ([BDPTIntegrator.cpp:5049-5050](../src/Library/Shaders/BDPTIntegrator.cpp:5049)) is UNCONDITIONAL — it remaps any `pdfRev = 0` to 1 regardless of `isDelta`.  When `pdfRev_env = 0` post-Piece-2.B, `remap0` fires, `ri *= 1 / pdfFwd_env`, and the ratio blows up — this is the Session 5 catastrophe at env+omni (BDPT/PT 85% → 7%).

The corrected resolution lives in §0.3: delta-aware `remap0` is the architectural fix that makes the v1 resolution's intent (`remap0` should NOT fire when a non-delta vertex's `pdfRev = 0` represents a genuinely impossible strategy) actually true.

**Original (incorrect) text retained below for audit**:

> **Original concern**: When `envSelectProb < 1`, dropping `kEnvZeroSentinel` lets `MISWeight`'s `remap0` line incorrectly fire when `envSelectProb`-derived pdfRev is positive-but-small.  PBRT-v4 handles via `IsInfiniteLight()` flag in MISWeight itself.
>
> **Resolution in this design**: `envSelectProb` in RISE is 0 or 1 (no fractional values — see [LightSampler.h:362-367](../src/Library/Lights/LightSampler.h:362)).  So the "positive-but-small" case the sentinel was protecting against doesn't exist in the current `EnvSelectProbability()` implementation.  When `envSelectProb = 0`, the SA pdfRev = `0 × pdf_env_sa = 0` — cleanly zero, `remap0` correctly fires for the delta-vertex case (which is what we want: env vertex can't be generated by s=1 NEE in this case).  When `envSelectProb = 1`, the SA pdfRev = `pdf_env_sa` — strictly positive, `remap0` doesn't fire.  Sentinel is unnecessary.
>
> **Confidence**: HIGH for the current `EnvSelectProbability()` 0-or-1 semantics.  **If RISE ever changes `EnvSelectProbability()` to return a fractional value** (e.g. a future fix for the mixed-scene companion limitation tracked in IMPROVEMENTS.md §12), the sentinel question must be revisited.  Recommend a code comment at the migrated D4 site noting this.
>
> **Residual risk**: PBRT-v4's `MISWeight` has an `IsInfiniteLight()` aware skip-rule via the `IsDeltaLight()` check at i==0 (§2.7).  RISE's MISWeight has an analogous skip-rule via `isDelta` (D11) — env vertex has `isDelta=false`, so it's treated as a connectable strategy.  Already correct.  The skip-rule doesn't need migration.

### §5.5 Cross-strategy MIS-walk effects on s≥2 chains touching the env vertex (Session 2's most damaging miss)

**Original concern**: Session 2's hand-derived algebra missed that the s=1 NEE alternative's pdf at the env vertex propagates through the MIS-walk's ratio chain to ALL s≥2 strategies' weight computation.  Net effect: BDPT s=0 contribution + sum of s≥2 contributions ≠ PT's expected value.

**Resolution in this design**:
- For s=0 strategy (eye→env miss), the MIS chain walks back through `eyeVerts[t-1..1]`'s `pdfRev` ratios.  All neighbour vertices use the pre-existing area-measure machinery, unchanged.  The env vertex's `pdfRev` is SA (D4); but the MIS chain ends AT the env vertex (no walk past it on the eye-side).  Self-consistent.
- For s=1 NEE strategy, the MIS chain walks back through `eyeVerts[t-1..1]`'s pdfRev (area, unchanged), then through `lightVerts[0]`'s pdfRev which equals the bsdf-pdf at eyeEnd toward env, SA-converted via `ConvertDensity(pdfRevSA, eyeEnd, lightStart)` per D6.  Since `lightStart.IsInfiniteLight()`, `ConvertDensity` returns SA unchanged.  Matches PBRT-v4's `qs->pdfRev = pt->PDF(integrator, ptMinus, *qs)` line 2180 (whose `ConvertDensity(pdf, qs)` short-circuits at env).
- For s≥2 strategies, the MIS chain walks back through the light subpath.  The env vertex is `lightVerts[0]`; its pdfFwd is SA (D2) and pdfRev is SA (D6) — ratio is unitless.  The NEXT vertex `lightVerts[1]` has area-measure pdfFwd (D3 post-walk override) and area-measure pdfRev (set by RandomWalk's stage-restore at adjacent vertices — unchanged).  Ratio at vertex 1: area/area, unitless.  Each subsequent vertex: same.  **The boundary at vertex 0/1 is the SA/area transition; both ratios are dimensionally consistent INDEPENDENTLY.**

This is **exactly** the structural property PBRT-v4's design exploits.  The reason Session 2's algebra missed it: Session 2 changed `vertices[0].pdfFwd` to `pdfSelect_emit × pdf_env_sa` but didn't override `vertices[1].pdfFwd` from the SA-derived value (`pdf_env_sa × cos / dist²`).  So at vertex 1, Session 2 had area-measure pdfFwd (`pdf_env_sa × cos / dist²`) but with the wrong `pdf_env_sa` factor baked in — when the s≥2 MIS chain ratioed pdfRev/pdfFwd at vertex 1, the `pdf_env_sa` factor didn't cancel correctly because the corresponding pdfRev was set by a non-env-aware stage-restore site that didn't include it.  Net: scale error of `1/pdf_env_sa` on every s≥k MIS ratio.

**Confidence**: HIGH.  The D3 override is what fixes this.  The s≥2 collapse on env+omni / env+mesh from Session 2 will not recur because D3 makes vertex 1's pdfFwd be `pdfPos_disc × cos`, which is what PBRT-v4 uses, which is the value that makes the MIS chain self-consistent.

**Mitigation**: piece 2.D's gate includes the env+omni / env+mesh topologies because they're the load-bearing oracle for s≥2 correctness.  If they don't improve, piece 2.D's implementation is wrong and we revert and re-audit.

---

## §6. Validation gates

Mirrors [PRE_PHASE1_STATUS.md](PRE_PHASE1_STATUS.md) §"Validation gates" but with **per-piece incremental coverage** instead of all-or-nothing landing.

### §6.1 Per-piece gates (mandatory checkpoint between pieces)

After each of pieces 2.A through 2.G:
1. Clean `make -C build/make/rise -j8 all` build — warning-free per CLAUDE.md "Compiler Warnings Are Bugs".
2. `./run_all_tests.sh` reports **116/116 pass**.
3. EnvLightBalanceTest at LAX tolerances `{ 0.35, 0.35, 2.00 }` passes — confirms no regression vs Session 2-pre baseline.
4. Per-piece test gate from §4 — the specific topology that piece is targeting must improve.

If any per-piece gate fails: **STOP, REVERT THE PIECE, AUDIT** — don't proceed to the next piece on a broken baseline.  Session 2's failure mode was attempting B+C+D+E as a unit; this discipline prevents recurrence.

### §6.2 End-to-end gates (after piece 2.G lands)

5. EnvLightBalanceTest at **STRICT tolerances `{ 0.10, 0.30, 1.00 }`** passes on every topology × RGB/spectral × HWSS combination.  Strict-tolerance pass is the closure criterion for IMPROVEMENTS.md §12.
6. BDPTStrategyBalanceTest, VCMStrategyBalanceTest, VCMRecurrenceTest, VCMSpectralRecurrenceTest must NOT regress — these test MIS-walk invariants on non-env scenes; collateral damage detection.
7. Render-baseline diff against `pre_piece1_envsamis/` (captured at Session 2 start, stored at `tests/baselines_refactor/pre_piece1_envsamis/`): within ~0.27 % mean luminance MC noise floor on non-env scenes.  On env scenes the diff is expected to be ~15-22 % (the residual being closed).
8. HDRVarianceTest K-trial measurement per [skills/variance-measurement.md](skills/variance-measurement.md): BDPT-vs-PT and VCM-vs-PT RMSE drops by **≥15 %** post-refactor on env-IBL scenes.  K=16 EXR trials, scene = `ripple_dreams_fields.RISEscene` + EnvLightBalanceTest's stencils.
9. Visual parity on `scenes/FeatureBased/ripple_dreams_fields.RISEscene`: PT, BDPT, VCM at matched samples render visually indistinguishable — confirms the perceived 15-22 % darkness gap is closed.
10. **Adversarial code review** per [skills/adversarial-code-review.md](skills/adversarial-code-review.md): 2-3 reviewers in parallel with orthogonal concerns.  Required after piece 2.E (when all four BDPT env-MIS sites are migrated and the design's MIS-balance is testable end-to-end) and again after piece 2.F (after VCM lands).  Suggested reviewer axes:
    - **MIS-walk self-consistency**: do the per-(s,t) ratio chains sum to 1 across all strategies on env-IBL scenes?  Reviewer should construct a synthetic small-path test (e.g. 3-vertex camera + 2-vertex env-light) and trace MISWeight by hand.
    - **PBRT-v4 line-for-line port audit**: every change in pieces 2.B-2.F must cite a PBRT-v4 line.  Reviewer cross-checks each citation against the actual PBRT-v4 source.
    - **VCM recurrence symmetry**: does `isFinite = (v.pEnvLight == 0)` + `applyDistSqToDVCM = !(i == 1 && pEnvLight)` match Georgiev 2012 Appendix A?  Reviewer should reproduce the SmallVCM `EmitPhoton`'s env case and confirm RISE's dVCM_env = `envSelectProb × πr²` is dimensionally and numerically correct.
    - **Numerical stability**: dark-env regions where `pdf_env_sa → 0`; `envSelectProb = 0` mixed-scene paths; the `remap0`-vs-zero distinction.

### §6.3 Stop rule for option (c)

Per [skills/adversarial-code-review.md](skills/adversarial-code-review.md): every material finding either fixed or rejected with a recorded reason, AND at least one post-fix review round returns no new P1/P2 findings.

Additional stop rule specific to this work: if at any piece the EnvLightBalanceTest catastrophic-regression pattern recurs (any topology drops below 50 % of PT at lax tolerances), STOP and write a Session 4 status section.  Do NOT push through hoping a later piece fixes it — that was Session 2's failure mode.

---

## §7. RISE-vs-PBRT-v4 architectural divergence

This section enumerates structural differences between RISE BDPT and PBRT-v4 BDPT that affect the option (c) port.  For each, we state whether the PBRT-v4 pattern applies cleanly or requires adaptation.

### §7.1 MISWeight architecture: in-walk `ScopedAssignment` vs caller-staged pdfRev

**(revised 2026-05-28)** — the v1 dismissal of MISWeight changes was overconfident.  See [§0.3](#03-recommended-precondition--delta-aware-remap0): a minimal 6-line **delta-aware `remap0`** is recommended as the precondition piece 2.B'.  It does NOT require introducing the `Vertex::PDF` / `PDFLight` / `PDFLightOrigin` API surface — only gates the existing `remap0` line on `vi.isDelta`.  The cost is two 3-line clauses (RGB MISWeight + NM twin) plus the 0/0 guard described in §0.3.5.  It addresses the RISE-specific binary 0-or-1 `EnvSelectProbability()` that PBRT-v4's design doesn't anticipate.  The full PBRT-v4 in-walk port (`ScopedAssignment` + `Vertex::PDF` dispatch) remains out of scope per the original §7.1 reasoning.

**Original (still valid for the full port; superseded for the minimal delta-aware `remap0` change)**:

**PBRT-v4** (`MISWeight` at integrators.cpp:2129-2207): the function itself, via `ScopedAssignment<Vertex>` and `ScopedAssignment<Float>`, temporarily overrides `pt->pdfRev`, `ptMinus->pdfRev`, `qs->pdfRev`, `qsMinus->pdfRev` based on the current (s,t) strategy.  The overrides invoke `Vertex::PDF` and `Vertex::PDFLight` / `PDFLightOrigin` to recompute pdfRev from scratch.  Caller passes raw vertex arrays.

**RISE** (`MISWeight` at BDPTIntegrator.cpp:4836-5001): the function reads pre-staged `pdfRev` fields and walks ratios; it does NOT recompute anything.  Each `ConnectAndEvaluate` site explicitly stages pdfRev on connection vertices before calling MISWeight, then restores afterward.

**Adaptation**: keep RISE's caller-staged pattern.  Do NOT port the in-walk ScopedAssignment design.  Reasons:
- Porting would require introducing a `BDPTVertex::PDF` / `PDFLight` / `PDFLightOrigin` API surface, which is structurally a bigger change than option (c) targets.
- RISE's caller-staged pattern is known-correct on non-env scenes; switching architecture risks bugs unrelated to env handling.
- Functional equivalence holds **IF** every stage-restore caller correctly uses the measure-aware accessor.  That's exactly what rows D4, D5, D6 enforce.

**Cost of NOT porting**: each migrated stage-restore site is a separate edit (RGB + NM × Path A + Path B = 4 sites per measure-aware update).  The in-walk port would centralize this.  Marginal cost ~5-10 lines per site.  Acceptable.

### §7.2 RISE has HWSS, OpenPGL guiding, BDPTVertex bookkeeping fields PBRT-v4 doesn't

**HWSS** (Hero Wavelength Spectral Sampling, IMPROVEMENTS.md §5): the spectral BDPT integrator carries `SampledWavelengths` per path and propagates per-wavelength bookkeeping for companion wavelengths.  PBRT-v4 also has SampledWavelengths but only one wavelength is "alive" at a time per path; RISE keeps 4 companion wavelengths active.

**Treatment**: out of scope for this design (§3.1).  The HWSS companion-wavelength path reads `ls.pdfPosition × ls.pdfDirection` directly (BDPTIntegrator.cpp:5261); since we don't change SampleEnvLightEmission, HWSS is unaffected.  Piece 2.D's test gate explicitly includes the HWSS=true env-only topology as a safety check.

**OpenPGL guiding** (IMPROVEMENTS.md §2): records s=1 NEE samples for path-guiding training.  The `GuidingRISCandidate` records the directional pdf in SA at the candidate vertex.

**Treatment**: out of scope.  The PT-formula contribution at the Path A site (D17) emits `cosEyeWi / pdfSA` as the directional signature, which is what guiding records.  Migration of `lightStart.pdfFwd` semantics doesn't change what guiding consumes.  Adversarial review round 2 should explicitly verify this with a guided-BDPT render of a partially-guided env scene.

**BDPTVertex extra fields** (`emissionPdfW`, `cosAtGen`, `guiding*`, `vColor`, etc.): VCM-post-pass uses `emissionPdfW`; guiding uses `guiding*`.  None of these are read by MISWeight itself.  The post-walk override (piece 2.D) overrides only `pdfFwd` on `vertices[0]` and `vertices[1]`; `emissionPdfW` retains its prior value (`pdfSelect × pdfPosition_disc × pdfDirection`), which VCM's `InitLight` consumes correctly via row D14's analysis.

### §7.3 Phase 2a templatization (VCM, in progress)

VCM has been templatized for RGB/NM uniformity per [INTEGRATOR_REFACTOR_STATUS.md](INTEGRATOR_REFACTOR_STATUS.md) Phase 2a.  Both BDPT (Phase 2c, not yet started) and PathTracing (Phase 2b) still have parallel RGB+NM code paths.

**Treatment**: piece 2.F (VCM) lands on the templatized VCM, so the migration is a single set of edits to `ConvertLightSubpath` instead of two parallel edits.  Pieces 2.B-2.E (BDPT) land on the un-templatized BDPT, requiring parallel RGB+NM edits as listed in §4 and §3 (rows D16).  This is more verbose but doesn't change the design's correctness — it just doubles the edit count.

### §7.4 RISE has no DeltaDirection light type

PBRT-v4 lumps DeltaDirection (sun / distant) into `IsInfiniteLight()` because they share the disc-parameterization.  RISE has no DeltaDirection at the BDPT level — directional lighting is via env IBL maps with concentrated peaks.

**Treatment**: `BDPTVertex::IsInfiniteLight() == (pEnvLight != 0)` is correct for RISE's current light set.  If RISE adds a true directional-light type, extend the predicate.  Out of scope for option (c).

### §7.5 RISE's MIS heuristic: BDPT uses power-2, VCM uses balance

PBRT-v4 uses balance heuristic in MISWeight (just `ri`, not `ri × ri`).  RISE BDPT uses power-2 (line 4946: `sumWeights += ri * ri`), VCM uses balance per Georgiev 2012 recurrence requirements.  See [docs/MIS_HEURISTICS.md](MIS_HEURISTICS.md) and the CLAUDE.md High-Value Fact "MIS heuristic per integrator".

**Treatment**: out of scope.  The SA-vs-area measure consistency at the env vertex is **orthogonal** to the heuristic choice — `ri` and `ri × ri` both depend on `pdfFwd / pdfRev` ratios being self-consistent across vertices, which is what this design enforces.  Power-2 vs balance only changes the weighting of the sumWeights, not the per-strategy ratio computation.

---

## §8. Open questions / decisions for the user

### §8.1 Is the `static const bool kSAMisAudit` mode (piece 2.A) worth the effort?

The audit mode adds parallel computation + assertion at every site that will be migrated in 2.B-2.F.  Cost: 0.5 session.  Value: catches a Session-2-style "the new computation doesn't equal the old at the env-only-no-alias-table topology" mismatch before it ships.

**Recommendation**: yes — it's cheap insurance against the exact failure mode that burned Session 2.  But the user may prefer to skip it and rely entirely on per-piece test gates.

### §8.2 Should piece 2.D include OpenPGL guiding audit, or defer to a follow-up?

Piece 2.D is the largest piece (1 session) and changes `lightStart.pdfFwd` semantics.  OpenPGL training data records `lightStart.pdfFwd` indirectly via the GuidingRISCandidate.  Audit whether guiding's record changes meaningfully.

**Recommendation**: defer to a follow-up piece 2.H if needed.  Reason: the PT-formula contribution override at Path A means guiding records `cosEyeWi / pdfSA` (directional signature), which is independent of `lightStart.pdfFwd`.  Confidence MEDIUM — explicit verification needed.  If guiding-trained env scenes start showing variance regressions, that's the signal to follow up.

### §8.3 What about the companion limitation: env not in alias table for mixed-light scenes?

[IMPROVEMENTS.md](IMPROVEMENTS.md) §12 documents a related limitation: env-NEE is currently restricted to env-only scenes; in mixed scenes, env contributes solely via Path B s=0.  A 2026-05-26 attempt to fix this caused a spectral-BDPT regression unrelated to the SA-MIS work.

**Recommendation**: defer.  This limitation is the next item to tackle AFTER option (c) lands and the 15-22 % residual closes.  Both fixes interact (mixing env into alias table changes `EnvSelectProbability()` from 0-or-1 to fractional), and tackling them sequentially keeps test bisection clean.

### §8.4 Adversarial review threshold

Adversarial review (gate 10) is mandatory after piece 2.E and piece 2.F.  Suggested 2-3 reviewers, but the user may want more depending on scene-specific risk (e.g. the renderer is going into a production milestone shortly).

**Recommendation**: default 2-3 reviewers per [skills/adversarial-code-review.md](skills/adversarial-code-review.md).  Escalate to 4-5 if scene-specific regressions are observed in pieces 2.B-2.E.

### §8.5 What if a piece's test gate passes at lax but fails at strict tolerances?

Piece 2.G is the strict-tolerance switch.  But intermediate pieces 2.B-2.F are gated at lax tolerances (so they're forced to MONOTONICALLY improve without yet requiring full closure).

**Recommendation**: at each intermediate piece, run EnvLightBalanceTest at strict tolerances as a non-blocking observation.  The number of strict-tolerance failures should monotonically decrease across pieces 2.B-2.F.  If it ever increases, that piece is suspect.

---

## §9. Stop conditions reached during this design pass

None.  The PBRT-v4 reference reading clarified the design and the 4-6 session estimate matches the [PRE_PHASE1_STATUS.md](PRE_PHASE1_STATUS.md) option (c) projection.  No fundamental RISE-vs-PBRT-v4 architectural clash was found that prevents the port; the architectural divergences in §7 are all manageable adaptations.

The most important finding from this design pass:

> **The Session 2 attempt's catastrophic regression was caused by collapsing `SampleEnvLightEmission`'s `pdfPosition` from disc-area `1/(πr²)` into the env directional pdf `pdf_env_sa`.  PBRT-v4 keeps these two separate — `pdfPos` stays at `1/(πr²)` and is consumed by a post-walk override that places it on `path[1]` as a projected area density (not on the env vertex itself).  The env vertex's own `pdfFwd` is set by a SECOND post-walk override to `InfiniteLightDensity(...)`, which is the NEE-side SA density and is GENERALLY NOT EQUAL to `pdfSelect_emit × pdf_env_sa` because the emission-side selection probability and the NEE-side selection probability are different quantities in RISE's mixed-scene case (envSelectProb_NEE = 0 when alias table is populated, but pdfSelect_emit = 1).  The previous spec's "store pdfPosition = pdf_env_sa" instruction violates the PBRT-v4 design; this design fixes that by leaving SampleEnvLightEmission alone and adding the two post-walk overrides per row D2 and D3.**

This insight is the design's keystone.  Every piece in §4 follows from it.

---

## §10. Cross-references

- Predecessor session report: [docs/PRE_PHASE1_STATUS.md](PRE_PHASE1_STATUS.md) (will be superseded by execution status reports of pieces 2.A–2.G).
- Canonical task spec: [docs/IMPROVEMENTS.md](IMPROVEMENTS.md) §12.
- PBRT-v4 reference: [`src/pbrt/cpu/integrators.cpp`](https://github.com/mmp/pbrt-v4/blob/master/src/pbrt/cpu/integrators.cpp), [`src/pbrt/lights.cpp`](https://github.com/mmp/pbrt-v4/blob/master/src/pbrt/lights.cpp).
- SmallVCM reference: [`src/vertexcm.hxx`](https://github.com/SmallVCM/SmallVCM/blob/master/src/vertexcm.hxx) `GetLightRadiance()`.
- Mitsuba reference: [`src/libbidir/path.cpp`](https://github.com/mitsuba-renderer/mitsuba/blob/master/src/libbidir/path.cpp) `Path::miWeight()` (more general framework with explicit EMeasure tagging; informative for understanding the design space, but option (c) ports PBRT-v4's simpler convention instead).
- Phase 1.A landed code: [BDPTVertex.h](../src/Library/Shaders/BDPTVertex.h) `IsInfiniteLight()` + [BDPTUtilities.h](../src/Library/Utilities/BDPTUtilities.h) `ConvertDensity`.
- Diagnostic skills: [skills/bdpt-vcm-mis-balance.md](skills/bdpt-vcm-mis-balance.md), [skills/adversarial-code-review.md](skills/adversarial-code-review.md), [skills/variance-measurement.md](skills/variance-measurement.md).
- Validation oracle: [tests/EnvLightBalanceTest.cpp](../tests/EnvLightBalanceTest.cpp).
- Companion VCM recurrence: [docs/VCM.md](VCM.md) Appendix A, [VCMRecurrence.h:140](../src/Library/Shaders/VCMRecurrence.h:140).
- MIS heuristic context: [docs/MIS_HEURISTICS.md](MIS_HEURISTICS.md).
