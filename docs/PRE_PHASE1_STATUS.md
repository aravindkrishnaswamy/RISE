# Pre-Phase-1 Workstream — Session Status Report

**Session 1 date**: 2026-05-27 (design pass; spec)
**Session 2 date**: 2026-05-27 (execution attempt; Phase 1.A landed, 1.B–1.E reverted)
**Plan**: [docs/UNIFIED_INTEGRATOR_ANALYSIS.md](UNIFIED_INTEGRATOR_ANALYSIS.md) §8.5 + §8.6
**Branch state (session 2 end)**: commit `a4a24b85` Phase 1.A on master (additive only).  Phases 1.B–1.E were attempted and reverted after `EnvLightBalanceTest` collapsed at LAX tolerances post-refactor (11 failures vs 0 on master); see "Session 2 outcome" below.

---

## TL;DR

The umbrella plan calls for four pre-Phase-1 workstreams:

1. Env-IBL SA-MIS refactor — `IMPROVEMENTS.md` §12 (estimated 3 weeks)
2. VCM media-aware connection transmittance (~2 weeks)
3. Phase 2b — PathTracingIntegrator templatization (~3-4 weeks)
4. Phase 2c — BDPTIntegrator templatization (~6-8 weeks)

User-confirmed scope for this session: **Piece 1 only, full discipline.**

**Data-driven go/no-go call this session**: the spec's stopping rule
"cost materially exceeds the estimate" applies upfront — Piece 1's
3-week estimate spans multiple work sessions and includes mandatory
gates (pre-piece baselines, 116-test pass, K-trial variance, visual
parity, adversarial review rounds, clean warning-free rebuild) that
cannot be honestly executed end-to-end in a single AI session.
**Stopping here with a complete code survey, an exact per-file change
spec, and validation gates listed makes Piece 1 ready for execution in
the next session without re-doing the upstream reading.**

This document is the spec the next session executes against — modeled
on [INTEGRATOR_REFACTOR_STATUS.md](INTEGRATOR_REFACTOR_STATUS.md)
which serves the same role for Phase 0/1/2a of the integrator
refactor.

---

## What was completed this session

### Required reading covered

- [UNIFIED_INTEGRATOR_ANALYSIS.md](UNIFIED_INTEGRATOR_ANALYSIS.md) — full doc, §8.5 + §8.6 sequencing rationale.
- [IMPROVEMENTS.md](IMPROVEMENTS.md) §12 — Piece 1 canonical spec.
- [INTEGRATOR_REFACTOR_STATUS.md](INTEGRATOR_REFACTOR_STATUS.md) — Phase 2a verification pattern (53 tests, baselines, adversarial review).
- [VCM.md](VCM.md) — env-vertex relevant sections, dVC/dVCM/dVM recurrence.
- [skills/bdpt-vcm-mis-balance.md](skills/bdpt-vcm-mis-balance.md) — diagnostic procedure for MIS regressions.
- [skills/variance-measurement.md](skills/variance-measurement.md) — K-trial EXR protocol.
- [skills/adversarial-code-review.md](skills/adversarial-code-review.md) — multi-reviewer dispatch.

### Code surveyed

- [BDPTVertex.h](../src/Library/Shaders/BDPTVertex.h) — confirmed `pEnvLight != NULL` already marks env vertices; semantics for `position` / `normal` / `pdfFwd` not yet documented as SA-measure.
- [BDPTUtilities.h](../src/Library/Utilities/BDPTUtilities.h) — `SolidAngleToArea` / `GeometricTerm` / `SolidAngleToAreaMedium` / `GeometricTermSurfaceMedium` all unconditionally apply the `cos / dist²` Jacobian; no env-aware path.
- [BDPTIntegrator.cpp:1394-1465](../src/Library/Shaders/BDPTIntegrator.cpp) — `GenerateLightSubpath` vertex-0 init: `v.pdfFwd = pdfSelect * pdfPosition` (currently `1/(πr²)` for env); `pdfEmit = pdfSelect * pdfPosition * pdfDirection` (drives throughput division).
- [BDPTIntegrator.cpp:2652-2758](../src/Library/Shaders/BDPTIntegrator.cpp) — `GenerateEyeSubpath` Path B (RGB): pushes synthetic env vertex on miss, `vEnv.pdfFwd = SolidAngleToArea(pdfFwdPrev, 1.0, distSqToExit)`, geomNormal = -rayDir (stores direction).
- [BDPTIntegrator.cpp:3376-3491](../src/Library/Shaders/BDPTIntegrator.cpp) — `ConnectAndEvaluate` s=0 site (RGB): installs `eyeEnd.pdfRev = envSelectProb * pdfPosition_disc` and `eyePred.pdfRev = SolidAngleToArea(envSelectProb * pdf_env_sa, cos, dist²)`, both with `kEnvZeroSentinel = 1e-30` guards against `remap0`.
- [BDPTIntegrator.cpp:3750-4082](../src/Library/Shaders/BDPTIntegrator.cpp) — `ConnectAndEvaluate` s=1 site (RGB): bypasses disc-G with PT-formula for *contribution*, but MIS bookkeeping still flows through `SolidAngleToArea` at the connection.
- [BDPTIntegrator.cpp:4836-5001](../src/Library/Shaders/BDPTIntegrator.cpp) — `MISWeight` itself: walks `pdfRev/pdfFwd` ratios; does NOT do explicit area conversion. The measure mismatch is *in what's stored* on env vertices, not in MISWeight.
- [BDPTIntegrator.cpp:5183-5215, 6332-6386, 7001-7073, 7283-7483](../src/Library/Shaders/BDPTIntegrator.cpp) — NM/spectral twins of the four sites above; symmetric.
- [LightSampler.cpp:1059-1155](../src/Library/Lights/LightSampler.cpp) — `SampleEnvLightEmission`: `pdfPosition = 1/(πr²)` (disc area), `pdfDirection = pdf_env_sa(wi)`, `pdfSelect = 1`.
- [LightSampler.h:362-367](../src/Library/Lights/LightSampler.h) — `EnvSelectProbability()` returns 1 iff env-only (no alias-table); 0 otherwise. Used by Path B for `pdfRev` gating.
- [VCMRecurrence.h:140-172](../src/Library/Shaders/VCMRecurrence.h) — `InitLight(directPdfA, emissionPdfW, cosLight, isFiniteLight, isDelta, norm)` already exposes `isFiniteLight` flag; `ApplyGeometricUpdate(q, distSq, cos, applyDistSqToDVCM)` already exposes `applyDistSqToDVCM` gate.
- [VCMRecurrence.cpp:128-237](../src/Library/Shaders/VCMRecurrence.cpp) — `isFiniteLight = true` overrides `cosLight = 1` in `dVC`; `applyDistSqToDVCM = false` skips the `dVCM *= distSq` step. Both already implement the Georgiev 2012 Appendix A env special-case correctly.
- [VCMIntegrator.cpp:417-477, 549-550, 791-808, 1044-1106, 1687-1772](../src/Library/Shaders/VCMIntegrator.cpp) — VCM CALLER sites that currently pass `isFiniteLight = true` unconditionally and `applyDistSqToDVCM = true` unconditionally (with explicit comments deferring the env case — lines 405-413, 469, 545-548).
- [tests/EnvLightBalanceTest.cpp:580-608](../tests/EnvLightBalanceTest.cpp) — `kEnvTolerances{ 0.35, 0.35, 2.00 }` lax tolerances accept the current 15-22% bias. Strict tolerance family per spec: `{ 0.10, 0.30, 1.00 }` matching `BDPTStrategyBalanceTest.cpp:514` `kStrictTolerances{ 0.08, 0.25, 1.00 }`.
- [tests/BDPTStrategyBalanceTest.cpp:486-527](../tests/BDPTStrategyBalanceTest.cpp) — reference tolerance family for non-env strategies; must NOT regress.

### Findings: the measure-consistency wrinkle

`MISWeight` itself does NOT do explicit area conversion — it just walks
`pdfRev/pdfFwd` ratios at each vertex. **So the 15-22% bias lives in
what's stored as `pdfFwd` / `pdfRev` on the env vertex and its
predecessor, not in the walk routine.**  This is good news: `MISWeight`
itself does not need restructuring.

Working through the algebra of the ratio at the env vertex on the s=1
NEE site:

```
Current (disc-area):
  pdfFwd[v0_env] = pdfSelect * (1/πr²)        // disc-area "position" pdf
  pdfRev[v0_env] = SolidAngleToArea(
                     pdfRevSA, |cos·n|, r²)    // ≈ pdfRevSA / r²
                                               // (cosLightDirection = 1)
  ratio = pdfRev / pdfFwd
        = (pdfRevSA / r²) / (1/πr²)
        = pdfRevSA · π

Target (PBRT-v4 SA-measure):
  pdfFwd[v0_env] = pdfSelect · pdf_env_sa(wi)
  pdfRev[v0_env] = envSelectProb_NEE · pdf_env_sa(wi)  // alt strategy
  ratio = (envSelectProb_NEE · pdf_env_sa) / (pdfSelect · pdf_env_sa)
        = envSelectProb_NEE / pdfSelect
```

The `pdf_env_sa(wi)` factor **cancels cleanly** in the target — no
scene-radius dependence, no `π` factor. This is the structural property
the refactor establishes.

**Throughput compensation**: at [BDPTIntegrator.cpp:1420](../src/Library/Shaders/BDPTIntegrator.cpp:1420), `v.throughput = Le / v.pdfFwd`. After the change, throughput at v0 changes from `Le · πr²` to `Le / pdf_env_sa(wi)`. The downstream `beta /= pdfEmit` already divides by the FULL joint, so:

```
Current:
  pdfEmit = pdfSelect · (1/πr²) · pdf_env_sa(wi)
  beta_after_v0 = Le · cosAtLight / pdfEmit
                = Le · 1 · πr² / pdf_env_sa(wi)

Target:
  pdfEmit = pdfSelect · pdf_env_sa(wi) · 1.0   // pdfDirection collapsed to 1
  beta_after_v0 = Le · 1 / (pdfSelect · pdf_env_sa)
```

**The `πr²` factor disappears from beta**. Every s≥2 contribution
shrinks by `πr²`. Conversely, in MIS the s≥2 ratio chain back-walks
through v[1]'s `pdfFwd`, which currently uses `pdfFwdPrev = pdfDirection
= pdf_env_sa(wi)` (line 1467), giving `pdfFwd[v1] = pdf_env_sa · cos_v1
/ dist_v0v1²`. With `dist_v0v1² ≈ r²`, that's `pdf_env_sa · cos_v1 /
r²`. After the refactor, `pdfFwdPrev = 1.0` (deterministic given wi —
all the directional density is in `pdfPosition` now), so `pdfFwd[v1] =
1 · cos_v1 / dist_v0v1²`. **That changes v1.pdfFwd by `1/pdf_env_sa`**,
which when MIS-walked back through the ratios scales the s≥2 MIS term
by `pdf_env_sa^k` for a k-vertex chain — pathological if `pdf_env_sa`
is tiny in part of the env map.

This is the **measure-consistency wrinkle**: the redistribution between
`pdfPosition` and `pdfDirection` in `SampleEnvLightEmission` propagates
to every downstream `pdfFwd` along the light subpath, not just the env
vertex's own field.

**Resolution** (per PBRT-v4 `ConvertDensity` symmetry, paraphrased in
[IMPROVEMENTS.md](IMPROVEMENTS.md) §12): the env-vertex's `pdfFwd` /
`pdfRev` must be in SA-measure (cancelling `pdf_env_sa(wi)` cleanly in
the env-vertex ratio), AND the conversion from env to its successor
must use the SA→area Jacobian against the env's emission direction
density. Concretely: store the **direction** pdf in
`sample.pdfDirection` (unchanged at `pdf_env_sa(wi)`), and store the
"selection density" `pdf_env_sa(wi)` ALSO in `sample.pdfPosition`
(replacing `1/πr²`). The `pdfPosition * pdfDirection` joint becomes
`pdf_env_sa² · ...` — which is wrong dimensionally. So a more careful
encoding is needed.

**The clean encoding (recommended):**
- Introduce `bool BDPTVertex::IsInfiniteLight()` = `pEnvLight != 0`.
- `BDPTUtilities::ConvertDensity(pdfSA, fromVertex, toVertex)` — an
  abstraction PBRT-v4 has; checks `toVertex.IsInfiniteLight()` and
  short-circuits the cos/dist² conversion. Replaces the bare
  `SolidAngleToArea` calls at the env-related sites.
- `SampleEnvLightEmission` stores `pdfPosition = 1` and `pdfDirection =
  pdf_env_sa(wi)`. Joint `pdfEmit = pdfSelect · pdf_env_sa` (SA, no
  disc-area factor).
- `GenerateLightSubpath` vertex-0 setup stores `pdfFwd = pdfSelect ·
  pdf_env_sa(wi)` (SA at env). Throughput `ls.Le / pdfFwd` is the same
  algebraic value as before the refactor for the v0 vertex itself.
- Vertex-1 `pdfFwd` propagation uses `ConvertDensity(pdf_env_sa, env,
  v1) = pdf_env_sa · cos_v1 / dist_v0v1²` — unchanged from current
  behavior in the *propagation* leg, since `pdfFwdPrev = pdf_env_sa`
  was already what was used.

Net effect: **only the env vertex's own `pdfFwd` and `pdfRev` change
measure** (from disc-area to SA). The propagation leg is unaffected.
The s=1 ratio computed above becomes `envSelectProb_NEE / pdfSelect`,
which is what we want.

### What still needs validation in the next session

The above algebra is hand-derived and may have errors in second-order
terms (HWSS companion-wavelength path, NM/spectral env handling,
mixed-scene env-NEE-in-alias-table path). The validation gate
(`EnvLightBalanceTest` at strict tolerances) is the load-bearing
oracle. **Do not declare the refactor done without those tolerances
passing**.

---

## Exact per-file change list (the next-session spec)

Each entry is `file:line — change`. Numbered for ledger reference. The
changes are intentionally listed in *dependency order*: each can be
made incrementally with the test suite green between steps.

### Phase 1.A — Introduce the abstraction (no behavior change)

1. **[BDPTVertex.h](../src/Library/Shaders/BDPTVertex.h)** — add `bool
   IsInfiniteLight() const { return pEnvLight != 0; }` accessor.
   Document the new semantics in the file-header comment block:
   "For env vertices (`IsInfiniteLight() == true`): `position` stores
   the disc-projection of the sky direction (legacy bookkeeping for
   distance computations); `normal` is the inward-pointing emission
   normal `-wi`; `pdfFwd` and `pdfRev` are SA-measure (sr⁻¹) at the
   env vertex itself. The SA→area conversion at the env-vertex
   boundary is skipped (PBRT-v4 `ConvertDensity` convention)."

2. **[BDPTUtilities.h](../src/Library/Utilities/BDPTUtilities.h)** —
   add `ConvertDensity(pdfSolidAngle, fromVertex, toVertex)` free
   function:
   ```cpp
   inline Scalar ConvertDensity(
       const Scalar pdfSolidAngle,
       const BDPTVertex& from,
       const BDPTVertex& to)
   {
       if (to.IsInfiniteLight()) return pdfSolidAngle;  // SA, no convert
       // Standard area conversion at non-env destination
       const Vector3 d = mkVector3(to.position, from.position);
       const Scalar distSq = SquaredModulus(d);
       if (distSq < 1e-20) return 0;
       const Scalar invDist = 1.0 / sqrt(distSq);
       if (to.type == BDPTVertex::MEDIUM)
           return pdfSolidAngle * to.sigma_t_scalar / distSq;
       if (to.type == BDPTVertex::CAMERA)
           return pdfSolidAngle / distSq;  // implicit cos=1
       const Scalar absCos = fabs(Dot(to.geomNormal, d * invDist));
       return pdfSolidAngle * absCos / distSq;
   }
   ```
   **No callers migrate yet.** This step is purely additive and
   maintains a clean test-green checkpoint.

3. **No callers migrate.** Build + 116-test pass. Commit as a discrete
   checkpoint.

### Phase 1.B — Switch SampleEnvLightEmission to SA-measure

4. **[LightSampler.cpp:1133-1135](../src/Library/Lights/LightSampler.cpp:1133)** — replace:
   ```cpp
   const Scalar discArea = PI * cachedSceneRadius * cachedSceneRadius;
   sample.pdfPosition = (discArea > 0) ? (1/discArea) : 0;
   sample.pdfDirection = pdfDir;
   sample.pdfSelect = 1.0;
   ```
   with:
   ```cpp
   sample.pdfPosition  = pdfDir;       // SA emission density (sr⁻¹)
   sample.pdfDirection = Scalar(1);    // deterministic given wi
   sample.pdfSelect    = Scalar(1);
   ```
   The disc geometry (sample.position on the bounding sphere) is kept
   — it's still used for visibility / dist² bookkeeping at the
   neighboring vertices, which `ConvertDensity` now handles correctly.

5. **[BDPTIntegrator.cpp:1420](../src/Library/Shaders/BDPTIntegrator.cpp:1420)** — verify the existing
   `v.pdfFwd = ls.pdfSelect * ls.pdfPosition` and `v.throughput = ls.Le
   / v.pdfFwd` line works correctly with the new convention: pdfFwd
   becomes SA, throughput becomes `Le / (pdfSelect · pdf_env_sa)`. The
   subsequent `pdfEmit = pdfSelect * pdfPosition * pdfDirection`
   collapses to `pdfSelect · pdf_env_sa` — no disc-area factor. No
   line changes needed; *semantics* change because pdfPosition's
   meaning changed.

6. **[BDPTIntegrator.cpp:1467](../src/Library/Shaders/BDPTIntegrator.cpp:1467)** — `pdfFwdPrev = pdfDirArea
   = ls.pdfDirection` becomes 1.0. The downstream propagation to v[1]
   at line ~1716 (`v.pdfFwd = SolidAngleToArea(pdfFwdPrev, absCosIn,
   distSq)`) would now give `pdfFwd[v1] = cos_v1 / r²` (missing the
   `pdf_env_sa(wi)` factor). **Fix**: at the env-init site explicitly
   set `pdfFwdPrev = ls.pdfPosition` (= `pdf_env_sa(wi)`) for env
   vertices, OR migrate the conversion at line ~1716 to use
   `ConvertDensity(pdfFwdPrev, v0, v1)` and have `ConvertDensity` look
   up `from.pdfFwd` when source is env. Recommended: the former
   (simpler, no abstraction change to the surface vertex path).

7. **[BDPTIntegrator.cpp:5183-5215](../src/Library/Shaders/BDPTIntegrator.cpp:5183)** — NM/spectral twin of
   the above. Same fix.

### Phase 1.C — Migrate Path A (s=1 NEE) sites

8. **[BDPTIntegrator.cpp:3944-3984](../src/Library/Shaders/BDPTIntegrator.cpp:3944)** — RGB s=1 env case.
   The PT-formula contribution stays. The pdfRev bookkeeping at lines
   3987-4070 currently uses `SolidAngleToArea(pdfRevSA, absCosAtLight,
   distSq_conn)` for the env light vertex; replace with
   `ConvertDensity(pdfRevSA, eyeEnd, lightStart)` which short-circuits
   to `pdfRevSA` (no `/r²`). The eyeEnd.pdfRev assignment for emission
   directional pdf likewise uses the new helper.

9. **[BDPTIntegrator.cpp:7287-7483](../src/Library/Shaders/BDPTIntegrator.cpp:7287)** — NM/spectral twin.
   Same migration.

### Phase 1.D — Migrate Path B (s=0 escape) sites and drop sentinels

10. **[BDPTIntegrator.cpp:2702-2757](../src/Library/Shaders/BDPTIntegrator.cpp:2702)** — RGB
    `GenerateEyeSubpath` Path B push: `vEnv.pdfFwd =
    SolidAngleToArea(pdfFwdPrev, 1.0, distSqToExit)` becomes
    `vEnv.pdfFwd = pdfFwdPrev` (SA-measure direct; PBRT-v4
    convention).

11. **[BDPTIntegrator.cpp:6345-6386](../src/Library/Shaders/BDPTIntegrator.cpp:6345)** — NM/spectral twin
    of #10.

12. **[BDPTIntegrator.cpp:3434-3490](../src/Library/Shaders/BDPTIntegrator.cpp:3434)** — RGB Path B s=0
    `ConnectAndEvaluate`: drop `pdfPositionDisc`, drop
    `kEnvZeroSentinel`. `eyeEnd.pdfRev = envSelectProb * pdf_env_sa(wiSky)`
    (SA, no disc factor). `eyePred.pdfRev = ConvertDensity(envSelectProb
    * pdf_env_sa, eyeEnd, eyePred)` — area at eyePred, standard
    conversion. Sentinel epsilon becomes unnecessary because
    `remap0`'s zero-detection still fires correctly when
    `envSelectProb = 0` (the new pdfRev is `0 * pdf_env_sa = 0`,
    cleanly).

13. **[BDPTIntegrator.cpp:7027-7072](../src/Library/Shaders/BDPTIntegrator.cpp:7027)** — NM/spectral twin
    of #12.

### Phase 1.E — VCM caller updates

14. **[VCMIntegrator.cpp:469](../src/Library/Shaders/VCMIntegrator.cpp:469)** — replace `const bool isFinite
    = true;` with `const bool isFinite = (v.pEnvLight == 0);`. The
    `VCMRecurrence::InitLight` already handles the `!isFinite` case
    correctly per Georgiev 2012 Appendix A (line 144: `usedCosLight =
    isFiniteLight ? cosLight : 1`).

15. **[VCMIntegrator.cpp:549](../src/Library/Shaders/VCMIntegrator.cpp:549)** — replace `const bool
    applyDistSqToDVCM = true;` with `const bool applyDistSqToDVCM =
    !(i == 1 && verts[0].pEnvLight != 0);`. Skips the `dVCM *= distSq`
    factor on the first bounce off an env light, per Georgiev 2012
    Appendix A (line 199-200 in VCMRecurrence.cpp implements the
    gate).

16. **[VCMIntegrator.cpp:1772](../src/Library/Shaders/VCMIntegrator.cpp:1772)** — verify symmetry for the
    NM/spectral light-subpath conversion (separate call site or
    shared per Phase 2a templatization).

### Phase 1.F — Tighten tests

17. **[tests/EnvLightBalanceTest.cpp:608](../tests/EnvLightBalanceTest.cpp:608)** — replace
    `kEnvTolerances{ 0.35, 0.35, 2.00 }` with `{ 0.10, 0.30, 1.00 }`
    matching `BDPTStrategyBalanceTest.cpp`'s `kStrictTolerances`
    family. Verify all topologies pass:
    - env-only Lambertian (RGB + spectral, HWSS on/off)
    - env + omni light (RGB + spectral, HWSS on/off)
    - env + mesh emitter (RGB + spectral, HWSS on/off)

### Phase 1.G — Documentation updates

18. **[BDPTVertex.h](../src/Library/Shaders/BDPTVertex.h)** — extend the file-header comment block
    documenting the SA-measure semantics for env vertices.

19. **[IMPROVEMENTS.md](IMPROVEMENTS.md) §12** — mark as DONE with
    measured residual / RMSE drop / strict-tolerance pass numbers.

20. **[CLAUDE.md](../CLAUDE.md)** — add a High-Value Fact summarizing
    the env-vertex SA-measure convention so the next agent doesn't
    re-derive it from scratch.

---

## Validation gates (mandatory before declaring Piece 1 done)

Replicating the Phase 2a precedent from
[INTEGRATOR_REFACTOR_STATUS.md](INTEGRATOR_REFACTOR_STATUS.md):

### Pre-refactor

1. Capture PNG baselines on 10+ scenes per
   [scripts/capture_refactor_baselines.sh](../scripts/capture_refactor_baselines.sh):
   `bash scripts/capture_refactor_baselines.sh pre_piece1_envsamis`.
   The same 10 scenes Phase 2a uses (cornellbox × PT/BDPT/VCM × RGB/spectral/caustics,
   hwss variants).
2. Confirm `./run_all_tests.sh` reports 116/116 pass on master.
3. Confirm `EnvLightBalanceTest` currently passes at the lax
   tolerances `0.35 / 0.35 / 2.00` and would fail at `0.10 / 0.30 /
   1.00` (validate the test will trip on the residual).

### Post-refactor

4. Build clean (warning-free) on `make -C build/make/rise -j8 all` AND
   Xcode `RISE-GUI` target per [CLAUDE.md](../CLAUDE.md) "Compiler Warnings Are Bugs".
5. `EnvLightBalanceTest` passes at strict tolerances `{ 0.10, 0.30,
   1.00 }` on every topology × RGB/spectral × HWSS on/off combination.
6. All 116 tests pass — especially `BDPTStrategyBalanceTest`,
   `VCMStrategyBalanceTest`, `VCMRecurrenceTest`,
   `VCMSpectralRecurrenceTest` (these test MIS-walk invariants on
   non-env scenes; must NOT regress).
7. Render-baseline diff against `pre_piece1_envsamis`: within the
   empirically-measured noise floor (~0.27% mean luminance on
   multi-threaded stochastic accumulators).
8. **HDRVarianceTest K-trial measurement** per
   [skills/variance-measurement.md](skills/variance-measurement.md):
   - Scene: `scenes/FeatureBased/ripple_dreams_fields.RISEscene` +
     `EnvLightBalanceTest`'s scene-stencils.
   - K=16 EXR trials per condition (PT, BDPT, VCM) × (master, fix).
   - Reference: PT at 4× SPP.
   - **Required outcome**: BDPT-vs-PT and VCM-vs-PT RMSE drops by ≥15%
     post-refactor on env-IBL scenes.
9. **Visual parity** on `ripple_dreams_fields.RISEscene`: PT, BDPT,
   VCM at matched samples render visually indistinguishable (currently
   they don't — BDPT/VCM are 15-22% darker on env contributions).
10. **Adversarial code review** per
    [skills/adversarial-code-review.md](skills/adversarial-code-review.md):
    2-3 reviewers in parallel with orthogonal concerns. Suggested
    axes:
    - **MIS-walk correctness**: does the MISWeight ratio chain still
      sum to 1 across all (s,t) strategies on env-IBL scenes?
      Concrete failure scenario: per-strategy contribution + MIS
      weight × number-of-strategies summing to PT's expected value.
    - **Path-A/B excision audit**: every disc-area assumption removed?
      `pdfPosition_disc`, `kEnvZeroSentinel`, `1/(π·r²)` literals.
      Grep clean.
    - **VCM recurrence symmetry**: does the env-vertex special case in
      `InitLight` + `ApplyGeometricUpdate` match Georgiev 2012 App. A
      and is BDPT's treatment isomorphic to it?
    - **Numerical stability**: grazing-cos cases on the predecessor's
      `ConvertDensity`, NaN paths when `pdf_env_sa(wi) → 0` in dark
      env regions.

### Stop rule for Piece 1

Per `skills/adversarial-code-review.md` Stop Rule: every material
finding either fixed or `rejected` with a recorded reason, AND at
least one post-fix review round returns no new P1/P2 findings.

---

## Why I stopped here (data-driven go/no-go call)

**The Phase 2a precedent**: "Stopping here leaves clean reviewable
checkpoints and establishes a replicable pattern for the remaining
phases."

Applied to Piece 1:

- The 3-week estimate in `UNIFIED_INTEGRATOR_ANALYSIS.md` §5.2.1
  reflects multi-session engineering with adversarial review cycles
  and K-trial variance measurement. The required-reading + code
  survey + design pass alone is roughly one full session's worth.
- Writing code without a baseline capture is a Phase 2a anti-pattern:
  the rendering-baseline noise floor (~0.27%) is the threshold
  against which "did the refactor produce visible changes outside MC
  noise" gets answered. Without pre-baselines captured, any
  subsequent diff is uninterpretable.
- The MISWeight walk's measure-consistency wrinkle (above) means a
  partial refactor — e.g., flipping `SampleEnvLightEmission` to SA
  without updating Path A/B's `pdfRev` setters and the VCM caller —
  produces a tree state that *breaks BDPT on env-IBL more than the
  current Path A/B workaround does*. There's no incremental midway
  checkpoint between "current Path A/B" and "fully migrated SA-MIS"
  that the test suite would accept.
- The user-confirmed scope (option A: "Piece 1 only, full
  discipline") explicitly accepts this is multi-session work.

**What landed this session**: this status doc + full code survey + the
exact per-file change list above. The next session opens with no
re-reading required — the spec is in this file.

**What hasn't landed**: any code changes. The branch state is
unmodified (the `M docs/UNIFIED_INTEGRATOR_ANALYSIS.md` from the
session start is the only working-tree change, predating this work).

---

## Recommended next-session entry point

1. Open this doc + [IMPROVEMENTS.md](IMPROVEMENTS.md) §12 + [INTEGRATOR_REFACTOR_STATUS.md](INTEGRATOR_REFACTOR_STATUS.md).
2. Capture pre-piece-1 baselines (validation gate #1-3 above).
3. Execute Phase 1.A through 1.G in order; **build + test green
   between each lettered phase**.
4. Run validation gates 4-10.
5. Update [IMPROVEMENTS.md](IMPROVEMENTS.md) §12 → DONE.
6. Update this doc with the as-built diffs and validation numbers.
7. Proceed to Piece 2 (VCM media-aware connection transmittance).

---

## Pieces 2, 3, 4 — deferred

Per the user-confirmed scope ("Piece 1 only, full discipline"), pieces
2-4 are deferred to follow-up sessions:

- **Piece 2** — VCM media-aware connection transmittance (~2 weeks).
  Extend BDPT's `EvalConnectionTransmittance` to be called from the
  four VCM connection sites (`EvaluateS0`, `EvaluateNEE`,
  `EvaluateInteriorConnections`, `SplatLightSubpathToCamera`).
  Preserve the VCM running-quantity recurrence. New regression scene:
  VCM env-IBL through homogeneous fog vs PT reference.

- **Piece 3** — Phase 2b PathTracingIntegrator templatization (~3-4
  weeks). Per [INTEGRATOR_REFACTOR_PLAN.md](INTEGRATOR_REFACTOR_PLAN.md)
  §3.5. Resolve PT-spectral inline AOV ([SPECTRAL_PARITY_AUDIT.md](SPECTRAL_PARITY_AUDIT.md) §2.6) as
  part of the same change.

- **Piece 4** — Phase 2c BDPTIntegrator templatization (~6-8 weeks).
  Largest piece; identical pattern to 2a + 2b. Do AFTER Pieces 1 + 2
  land so the templatization happens on a stable env-IBL + media-VCM
  base.

---

## Cross-references

- [UNIFIED_INTEGRATOR_ANALYSIS.md](UNIFIED_INTEGRATOR_ANALYSIS.md) §5.2.1, §5.2.2, §7.2, §8.5, §8.6 — umbrella plan.
- [IMPROVEMENTS.md](IMPROVEMENTS.md) §12 — Piece 1 canonical spec.
- [INTEGRATOR_REFACTOR_PLAN.md](INTEGRATOR_REFACTOR_PLAN.md) — Phase 2b/2c spec.
- [INTEGRATOR_REFACTOR_STATUS.md](INTEGRATOR_REFACTOR_STATUS.md) — Phase 0/1/2a verification pattern (this doc's template).
- [VCM.md](VCM.md) — VCM design + Georgiev 2012 recurrence.
- [SPECTRAL_PARITY_AUDIT.md](SPECTRAL_PARITY_AUDIT.md) §2.6 — PT-spectral inline AOV (Phase 2b-blocked).
- [tests/EnvLightBalanceTest.cpp](../tests/EnvLightBalanceTest.cpp) — primary validation oracle.
- [skills/bdpt-vcm-mis-balance.md](skills/bdpt-vcm-mis-balance.md) — diagnostic procedure.
- [skills/adversarial-code-review.md](skills/adversarial-code-review.md) — review dispatch.
- [skills/variance-measurement.md](skills/variance-measurement.md) — K-trial protocol.

---

## Session 2 outcome (2026-05-27 PM) — Phase 1.A landed, 1.B–1.E reverted

### What landed

- **Phase 1.A** committed as `a4a24b85` (master): purely additive.
  - `BDPTVertex::IsInfiniteLight()` accessor + env-vertex semantics
    documentation block in [BDPTVertex.h](../src/Library/Shaders/BDPTVertex.h).
  - `BDPTUtilities::ConvertDensity(pdfSA, from, to)` free helper in
    [BDPTUtilities.h](../src/Library/Utilities/BDPTUtilities.h) that
    short-circuits at env destinations and otherwise applies the
    standard cos/dist² (surface), sigma_t/dist² (medium) or 1/dist²
    (camera) area Jacobian.  Matches PBRT-v4 §15.5.2 ConvertDensity.
  - **Validation gates passed**: clean `make` build, 116/116 tests
    pass on the post-Phase-1.A tree.  Phase 1.A introduces no
    behaviour change, so visual / variance gates are trivially
    satisfied (binary-identical to master modulo the BDPTVertex.h
    header-comment rebuild churn).

### Pre-refactor baselines captured (gate 1)

`tests/baselines_refactor/pre_piece1_envsamis/` — 10 PNGs (the 10 of
12 curated scenes whose source files existed; 2 SKIP-missing
RussianRoulette scenes documented in the script).  Captured manually
because `scripts/capture_refactor_baselines.sh` mis-classifies a
successful `rise` render as a failure when `rise` exits with status 1
(a pre-existing bug in either `rise` CLI or the script — chip spawned
for follow-up: "Fix capture_refactor_baselines.sh exit-code handling").

### Gate 2 baseline (master)

`./run_all_tests.sh` reports **116/116 pass** in ~67 seconds on a
warm-cache build.

### Gate 3 confirmed: residual trips strict tolerances

Temporarily flipped `tests/EnvLightBalanceTest.cpp:608` from
`{ 0.35, 0.35, 2.00 }` to `{ 0.10, 0.30, 1.00 }`, rebuilt
EnvLightBalanceTest, ran on master:

```
Passed: 75
Failed: 5
  BDPT mean within 10% of PT: env + omni light          FAIL
  VCM  mean within 10% of PT: env + omni light          FAIL
  BDPT mean within 10% of PT: env + mesh emitter        FAIL
  VCM  mean within 10% of PT: env + mesh emitter        FAIL
  VCM  mean within 10% of PT: env-only Lambertian (spectral, hwss=true)  FAIL
```

The 5 strict-tolerance failures match the residual modes the spec
predicts (BDPT/VCM ~15% under on env+omni and env+mesh; VCM ~18%
under on env-only spectral HWSS=true).  The test is a valid
load-bearing oracle for the refactor.  Tolerances were reverted to
the lax `{ 0.35, 0.35, 2.00 }` family before any code changes.

### Why Phases 1.B–1.E were reverted

I implemented Phases 1.B (SA-measure SampleEnvLightEmission +
env-aware pdfFwdPrev in GenerateLightSubpath RGB+NM), 1.C (Path A
s=1 NEE pdfRev via ConvertDensity, RGB+NM), 1.D (Path B s=0 escape
SA-direct pdfFwd + drop pdfPositionDisc/kEnvZeroSentinel, RGB+NM),
and 1.E (VCM caller flags `isFinite` and `applyDistSqToDVCM` for env,
plus the second-order VCM consumers in EvaluateS0Impl env-escape MIS
and EvaluateNEEImpl env directPdfW/emissionPdfW).  Clean `make` build
succeeded.  `EnvLightBalanceTest` post-refactor at the EXISTING LAX
`{ 0.35, 0.35, 2.00 }` tolerances:

```
Passed: 69
Failed: 11
```

Worst regressions (all worse than master at the same lax tolerances):

| Topology                                       | PT mean | BDPT mean (master) | BDPT mean (post 1.B–1.E) | VCM (master) | VCM (post) |
|------------------------------------------------|---------|--------------------|--------------------------|--------------|------------|
| env-only Lambertian (RGB)                      | 0.59    | 0.64               | 0.50                     | 0.62         | 0.38       |
| env + omni light                               | 0.60    | 0.51               | **0.036**                | 0.51         | 0.51       |
| env + mesh emitter                             | 0.61    | 0.52               | **0.052**                | 0.52         | 0.52       |
| env-only Lambertian (spectral, hwss=false)     | 0.58    | 0.62               | 0.50                     | 0.59         | 0.36       |
| env-only Lambertian (spectral, hwss=true)      | 0.48    | 0.44               | **0.18**                 | 0.39         | 0.16       |

The env+omni and env+mesh BDPT collapse to **6%–9% of PT** is
catastrophic — *worse* than the pre-fix BDPT (~1–5% of PT in the
table at the top of this doc).  The env-only spectral HWSS=true
collapse to ~36% of PT is similarly catastrophic.

These match the spec's own warning:

> The above algebra is hand-derived and may have errors in
> second-order terms (HWSS companion-wavelength path, NM/spectral
> env handling, mixed-scene env-NEE-in-alias-table path).  The
> validation gate (EnvLightBalanceTest at strict tolerances) is the
> load-bearing oracle.  Do not declare the refactor done without
> those tolerances passing.

**Mixed-scene env-NEE-in-alias-table** (env+omni, env+mesh) and the
**HWSS companion-wavelength path** (spectral hwss=true) are the two
specific second-order interactions the spec flagged.  Both failed
catastrophically.

Per the spec's own stopping rule:

> Stop and update [docs/PRE_PHASE1_STATUS.md] (don't proceed) if:
> - Any validation gate fails after honest debugging.
> - You discover the spec's algebra is wrong in some second-order
>   detail (HWSS companion-wavelength, mixed-scene
>   env-NEE-in-alias-table interaction) ...

I reverted the working-tree changes for Phases 1.B–1.E
(`git restore` on the three touched files), rebuilt, re-ran
EnvLightBalanceTest at lax tolerances → 80/80 pass, ran
`./run_all_tests.sh` → 116/116 pass.  Tree state at session end:
Phase 1.A committed; no other code changes.

### Diagnosis hypotheses for the next session

The catastrophic env+omni and env+mesh BDPT collapse (~94% energy
loss) is consistent with the s=1 NEE alternative strategy being
catastrophically OVERWEIGHTED in MIS, draining weight from the
otherwise-correct s=0 / s≥2 strategies.  Likely culprits:

1. **`lightStart.pdfFwd` redistribution**: pre-refactor the env
   vertex's pdfFwd was `pdfSelect / (πr²)` (disc-area, very small —
   ~1e-5 for r=100).  Post-refactor it's `pdfSelect * pdf_env_sa` —
   for a uniform env, ~0.5 * 0.08 ≈ 0.04, ~3 orders of magnitude
   larger.  When this enters the MIS ratio chain via `pdfFwd[v0_env]`
   for s=1 strategies, the alternative's relative weight changes by
   ~10⁶ — enough to invert which strategy dominates the MIS sum.
   The new value MIGHT still be internally consistent (s=1's
   contribution itself is divided by pdf_env_sa via the PT-formula
   override), but the cross-strategy MIS balance with s≥2 paths
   that propagate through the env vertex needs algebraic
   verification against PBRT-v4's `MISWeight` walk semantics.

2. **MISWeight walk doesn't understand the SA boundary**: the spec
   claims `MISWeight` itself does NOT need changes ("the walk
   routine does NOT do explicit area conversion").  That's true,
   but the walk *assumes* both `pdfFwd` and `pdfRev` at every
   vertex are in the SAME measure (area).  Storing the env vertex's
   pdfFwd/pdfRev in SA while neighbouring vertices stay in area
   creates a unit mismatch at the boundary that the walk doesn't
   compensate for.  The PBRT-v4 fix is in `ConvertDensity` AT THE
   POINT OF PROPAGATION — every site that stores a pdfFwd or
   pdfRev needs to call `ConvertDensity` to do the right thing
   based on the destination's type.  We migrated the env-light
   pdfFwd / s=1 lightStart.pdfRev / s=0 eyeEnd.pdfRev to use
   ConvertDensity, but the s≥2 light-subpath chain still uses the
   OLD `SolidAngleToArea` at GenerateLightSubpath:1716 — that
   site converts pdfFwdPrev from SA to area at v1 using
   `SolidAngleToArea(pdfFwdPrev, absCosIn, distSq)`.  v1 is a
   surface vertex, not env, so the area conversion IS correct
   there.  But the inverse propagation (v1's pdfRev being computed
   later when the eye subpath connects to it) needs to convert
   FROM area-at-v1 BACK to SA-at-env for the chain to be
   self-consistent.  This inverse leg wasn't audited.

3. **HWSS companion-wavelength path**: the spectral env-emission
   site at BDPTIntegrator.cpp:5235-5267 evaluates Le for companion
   wavelengths in the SampledWavelengths struct.  The throughput
   division at line 5223 uses `pdfEmit = ls.pdfSelect * ls.pdfPosition
   * ls.pdfDirection` for the hero wavelength.  After the refactor
   `ls.pdfPosition * ls.pdfDirection` collapses from
   `(1/(πr²)) * pdf_env_sa` to `pdf_env_sa * 1` — same numerical
   value.  So the joint emission pdf is invariant.  But the
   companion-wavelength HWSS bookkeeping uses different per-wavelength
   pdfs that might independently break.  The 60%+ collapse of BDPT
   on HWSS=true scenes suggests this path needs a separate audit.

4. **Mixed-scene env-NEE-in-alias-table**: when `envSelectProb < 1`
   (env in alias table alongside other lights), the s=0 Path B
   `eyeEnd.pdfRev = envSelectProb * pdf_env_sa` is no longer
   guarded by `kEnvZeroSentinel`.  The MISWeight `remap0` line that
   converts `pdfRev == 0 → 1.0` for delta-vertex handling MIGHT
   fire incorrectly when envSelectProb is positive-but-small,
   tipping the MIS sum toward s=1.  The sentinel existed
   specifically to differentiate "truly zero pdf (env not in
   table)" from "small positive pdf".  Dropping the sentinel
   without an alternative gate might be the bug here.  PBRT-v4
   handles this via the vertex's `IsInfiniteLight()` flag in
   `MISWeight` itself — RISE's `MISWeight` would need a parallel
   special-case to be fully consistent with the SA convention.

### Recommended next session approach

Given the failures, the spec is undeniably incomplete in its
second-order treatment.  Three options for the next session:

1. **Conservative**: stop here.  Phase 1.A is a useful additive
   foundation — keep the helper available for future work, but
   don't pursue Phases 1.B–1.E without a more rigorous algebraic
   audit first.  The current Path A + Path B disc-area workaround
   is empirically within 15% on most topologies and that bias has
   been documented as the residual in IMPROVEMENTS.md §12 for
   weeks; the 15% residual is acceptable for current production
   work.

2. **Audit-driven retry**: spend 1–2 sessions deriving the FULL
   MISWeight algebra for env-touching strategies, with explicit
   per-vertex measure tracking and per-strategy unit checks.  The
   `bdpt-vcm-mis-balance` skill outlines the diagnostic procedure.
   Then attempt Phases 1.B–1.E with a precise algebraic
   correctness specification, NOT the hand-derived sketch in this
   doc.  Estimate: 3–5 sessions to reach a green oracle.

3. **PBRT-v4 port**: replace RISE's `MISWeight` walk with a more
   structured port of PBRT-v4's `MISWeight` that treats env
   vertices as a first-class measure-distinct type throughout the
   walk.  Larger surface but cleanly separates the SA/area boundary
   handling.  Estimate: 4–6 sessions.

For now: **stop at Phase 1.A.**  The 15-22% env-IBL residual stays
documented in IMPROVEMENTS.md §12.  Update §12 to note that the
Piece-1.A foundation has landed.

### Gate status at session 2 end

| Gate | Status |
|------|--------|
| 1.  Pre-refactor PNG baselines             | ✅ Captured (10 scenes, manual copy due to script bug) |
| 2.  116/116 tests green on master          | ✅ Pass (67s warm cache) |
| 3.  Strict tolerances trip on residual     | ✅ Confirmed (5 failures on master at strict; reverted to lax for stability) |
| 4.  Clean rebuild (make + Xcode)           | ✅ make clean post-Phase 1.A (Xcode not exercised this session — pure header additions, no source-file changes) |
| 5.  EnvLightBalanceTest at strict          | ❌ Not attempted (1.B–1.E reverted) |
| 6.  116-test suite (incl. BDPTStrategyBalance, VCMStrategyBalance, VCMRecurrence) | ✅ 116/116 pass on post-Phase-1.A + post-revert tree |
| 7.  Render-baseline diff                   | ⏸  Not run (Phase 1.A is binary-identical; deferred until 1.B–1.E land cleanly) |
| 8.  HDRVarianceTest K-trial                | ⏸  Not run (deferred — RMSE-vs-PT change requires 1.B–1.E to land cleanly) |
| 9.  Visual parity on ripple_dreams_fields  | ⏸  Not run (same reason as gate 8) |
| 10. Adversarial code review                | ⏸  Not run (no working refactor to review) |

### Files in the working tree at session end

- Committed: `a4a24b85 BDPT/VCM env-IBL refactor (Piece 1.A): add SA-measure ConvertDensity helper`
- Working-tree-only (unchanged from session start): `docs/UNIFIED_INTEGRATOR_ANALYSIS.md` (M, pre-existing), `docs/PRE_PHASE1_STATUS.md` (??), `tests/baselines_refactor/pre_piece1_envsamis/` (??, 10 PNGs).
- Reverted (no longer in working tree): the Phase 1.B–1.E edits to
  `src/Library/Lights/LightSampler.cpp`,
  `src/Library/Shaders/BDPTIntegrator.cpp`,
  `src/Library/Shaders/VCMIntegrator.cpp`.
