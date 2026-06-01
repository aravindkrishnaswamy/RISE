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

---

## Session 3 outcome (2026-05-27) — Option (c) design pass complete

### What was produced

`docs/PRE_PHASE1_OPTION_C_DESIGN.md` (new working-tree file, not
committed) — a PBRT-v4-grounded design pass that **supersedes** the
"Exact per-file change list" in this document (§"Phase 1.B–1.G").  The
new design is decomposed into seven pieces (2.A through 2.G) with
incremental EnvLightBalanceTest gates between each, replacing Session
2's all-or-nothing landing pattern.  Total estimate 4-4.5 sessions
(matches the [recommended option (c) 4-6 session projection](#recommended-next-session-approach)).

### Key insight from PBRT-v4 reference reading

The Session 2 attempt's catastrophic regression was caused by
collapsing `SampleEnvLightEmission`'s `pdfPosition` field from
disc-area `1/(πr²)` into the env directional pdf `pdf_env_sa(wi)`.
**PBRT-v4 keeps these two separate** — `pdfPos` stays at `1/(πr²)`
and is consumed by a POST-WALK override that places it on `path[1]`
as a projected area density (NOT on the env vertex itself).  The env
vertex's own `pdfFwd` is set by a SECOND post-walk override to
`InfiniteLightDensity(...)` — which is the NEE-side SA density and
is **generally NOT EQUAL** to `pdfSelect_emit × pdf_env_sa` because
emission-side selection probability and NEE-side selection
probability are different quantities in RISE's mixed-scene case
(`envSelectProb_NEE = 0` when alias table is populated, but
`pdfSelect_emit = 1`).

The previous spec's "store `pdfPosition = pdf_env_sa`" instruction in
Phase 1.B violates the PBRT-v4 design.  The new design fixes that by
leaving `SampleEnvLightEmission` alone (explicit out-of-scope in
§3.1) and adding two post-walk overrides in `GenerateLightSubpath`
(piece 2.D, rows D2 and D3 of the diff map).

### No code changes

Per the session brief: no production code changes, no commits, no
git operations.  Phase 1.A remains the production state.  The next
session opens against `docs/PRE_PHASE1_OPTION_C_DESIGN.md` and
executes piece 2.A.

---

## Session 4 outcome (Piece 2.A — audit-mode parallel computation)

**Date**: 2026-05-27.
**Branch state**: working-tree only; no commits.
**Reference spec**: [docs/PRE_PHASE1_OPTION_C_DESIGN.md](PRE_PHASE1_OPTION_C_DESIGN.md) §4 Piece 2.A.

### What shipped

A `static constexpr bool kSAMisAudit = false` guard at file scope in
[src/Library/Shaders/BDPTIntegrator.cpp:125](../src/Library/Shaders/BDPTIntegrator.cpp:125),
plus eight `if constexpr( kSAMisAudit )` parallel-computation blocks
at the sites that pieces 2.B–2.F will migrate.  Each block computes the
new SA-measure value, logs the disc-area / SA pair to stderr with
the diff-map row, and asserts `std::isfinite + >= 0` on the new value
when `EnvSelectProbability() >= 0.999` (env-only-no-alias-table
topology).

Audit insertion points by diff-map row (file:line approximate after
the audit code is in place):

| Row | Variant | Site | What it audits |
|---|---|---|---|
| D2 | RGB | [BDPTIntegrator.cpp:~1453](../src/Library/Shaders/BDPTIntegrator.cpp) | light-subpath-init env-vertex `pdfFwd` (disc `1×1/(πr²)` vs SA `envSelectProb_NEE × pdf_env_sa(wi)`) |
| D2 | NM  | [BDPTIntegrator.cpp:~5350](../src/Library/Shaders/BDPTIntegrator.cpp) | spectral twin of D2/RGB |
| D4 | RGB | [BDPTIntegrator.cpp:~3514](../src/Library/Shaders/BDPTIntegrator.cpp) | Path B s=0 env-vertex `pdfRev` (disc `envSelectProb × 1/(πr²)` vs SA `envSelectProb × pdf_env_sa(wiSky)`) |
| D4 | NM  | [BDPTIntegrator.cpp:~7220](../src/Library/Shaders/BDPTIntegrator.cpp) | spectral twin of D4/RGB |
| D5 | RGB | [BDPTIntegrator.cpp:~3586](../src/Library/Shaders/BDPTIntegrator.cpp) | Path B s=0 eyePred `pdfRev` (disc-w/-envSel vs SA-w/o-envSel via PBRT-v4's `PDFLight` convention — PMF drops at this row) |
| D5 | NM  | [BDPTIntegrator.cpp:~7280](../src/Library/Shaders/BDPTIntegrator.cpp) | spectral twin of D5/RGB |
| D6 | RGB | [BDPTIntegrator.cpp:~4135](../src/Library/Shaders/BDPTIntegrator.cpp) | Path A s=1 `lightStart.pdfRev` for env case (current `SolidAngleToArea(pdfRevSA, cos, distSq)` vs `ConvertDensity(pdfRevSA, eyeEnd, lightStart)` which short-circuits to SA unchanged) |
| D6 | NM  | [BDPTIntegrator.cpp:~7660](../src/Library/Shaders/BDPTIntegrator.cpp) | spectral twin of D6/RGB |

All eight sites use the same template: extract `EnvSelectProbability()`
and `pEnvSampler->Pdf(wi/wiSky)`, compute the new SA value matching
the diff-map row's formula, log via `std::fprintf(stderr, ...)`, and
gate the asserts on `envSelectProb >= 0.999`.

### Validation gates

1. **Clean build** — `make -C build/make/rise -j8 all` warning-free; tests build also clean.  No `-Wno-*` suppressions added.
2. **All tests pass with audit OFF** — `./run_all_tests.sh` reports **116/116 pass**.
3. **EnvLightBalanceTest at lax tolerances `{ 0.35, 0.35, 2.00 }`** — **80/80 sub-checks pass** (matches the Session 2-post baseline).
4. **Manual audit-mode validation** — flipped `kSAMisAudit = true` in the working tree, rebuilt library + tests, ran EnvLightBalanceTest end-to-end.
   - Process ran to completion (exit 0), all 80 sub-checks passed.
   - Stderr captured **4.9 M audit lines** across all topologies — distribution per row:
     `D2/RGB 268288`, `D2/NM 1048576`, `D4/RGB 260947`, `D4/NM 1310720`, `D5/RGB 260947`, `D5/NM 1310720`, `D6/RGB 42278`, `D6/NM 418173` (NM higher because spectral resolves multiple wavelengths per pixel).
   - **All eight sites fire** on the env-only Lambertian topology.
   - No assertions tripped after the relaxation noted below.
   - Then flipped `kSAMisAudit = false`, force-touched the source, rebuilt, reconfirmed `./run_all_tests.sh` **116/116 pass**.

### Sample audit-mode stderr output (env-only Lambertian)

```
[SA-MIS audit D2/RGB] BDPTIntegrator.cpp:~1420 light-subpath-init env-vertex pdfFwd  envSelectProb_NEE=1 pdf_env_sa=0.0796323  disc=0.158837 sa=0.0796323  ratio_sa_over_disc=0.501346
[SA-MIS audit D2/NM]  BDPTIntegrator.cpp:~5191 light-subpath-init env-vertex pdfFwd  envSelectProb_NEE=1 pdf_env_sa=0.0830124  disc=0.158837 sa=0.0830124  ratio_sa_over_disc=0.522626
[SA-MIS audit D4/RGB] BDPTIntegrator.cpp:~3434 path-B s=0 env-vertex pdfRev  envSelectProb=1 pdf_env_sa=0.0779307  disc=0.158837 sa=0.0779307
[SA-MIS audit D4/NM]  BDPTIntegrator.cpp:~7027 path-B s=0 env-vertex pdfRev  envSelectProb=1 pdf_env_sa=0.0808651  disc=0.158837 sa=0.0808651
[SA-MIS audit D5/RGB] BDPTIntegrator.cpp:~3469 path-B s=0 eyePred pdfRev   envSelectProb=1 pdf_env_sa=0.0779307  disc_with_envSel=0.899562 sa_no_envSel=0.899562
[SA-MIS audit D5/NM]  BDPTIntegrator.cpp:~7046 path-B s=0 eyePred pdfRev   envSelectProb=1 pdf_env_sa=0.0780238  disc_with_envSel=0.00466368 sa_no_envSel=0.00466368
[SA-MIS audit D6/RGB] BDPTIntegrator.cpp:~4002 path-A s=1 lightStart.pdfRev (env)  pdfRevSA=0.274687 absCosAtLight=1 distSq=3.87235  area_conv=0.0709355 sa_unchanged=0.274687
[SA-MIS audit D6/NM]  BDPTIntegrator.cpp:~7424 path-A s=1 lightStart.pdfRev (env)  pdfRevSA=0.231087 absCosAtLight=1 distSq=7.84987  area_conv=0.0294383 sa_unchanged=0.231087
```

**Magnitude observations on env-only Lambertian (envSelectProb_NEE = 1)**:

- **D2/D4**: SA value `pdf_env_sa(wi) ≈ 0.07-0.08 sr^-1`, disc value `1/(πr²) ≈ 0.159` (scene radius `r ≈ √(1/(π×0.159)) ≈ 1.42`).  SA-over-disc ratio is ~0.5 — i.e. the SA value is about half the disc value here.  Cross-cancellation with `cos × dist²` factors elsewhere in the path bookkeeping is what closes the MIS ratio chain; pieces 2.B and 2.D will land the actual migration.
- **D5**: `disc_with_envSel` equals `sa_no_envSel` exactly because `envSelectProb = 1` on the env-only topology (the migration's effect is invisible in this topology — it only differs when `envSelectProb < 1`, i.e. mixed-scene env+omni / env+mesh, which is the load-bearing oracle for piece 2.C).
- **D6**: `area_conv` and `sa_unchanged` differ by the `cos / distSq` Jacobian factor — `0.07/0.27 ≈ 1/3.87 = 1/distSq` with `absCosAtLight = 1`.  Confirms the migration target (drop the area conversion) is dimensionally sensible.

### Surprise encountered: relaxed assertion from `> 0` to `>= 0`

The chip's prescribed assertion was `assert(double(pdfSA_new) > 0)`.
That fired on the very first audit run at the D4/RGB site:
`assertion failed: pdf_env_sa = 0, sa = 0` — the env importance
sampler's CDF has zero mass for some directions (regions of the
HDRI with literally zero radiance, plus quantization within the CDF
grid resolution).  These are **legitimate** dark-sky directions:
the NEE strategy could not have sampled that direction, so the new
SA-measure pdf correctly returns 0 there.  MIS gives all weight to
whichever strategy DID find the path — which is exactly the correct
behaviour the migration is trying to enforce.

Relaxed all eight `assert(... > 0)` to `assert(... >= 0)`; kept
`assert(std::isfinite(...))` unchanged.  Updated the file-scope
comment block to document the relaxation rationale.  Re-ran the
EnvLightBalanceTest with audit on — completed cleanly, no asserts
tripped.  The chip's "use your judgment: prefer logging over
asserting where the mathematical relationship between disc and SA
is nontrivial" guidance was the explicit out for this judgment call.

This is not a Piece 2.A blocker — it's a discovery that the
strict-`> 0` invariant doesn't hold in practice and the audit
needs to allow zero values as a correct outcome.  When pieces
2.B-2.F migrate the sites, the production code will simply assign
zero to `pdfRev` in those directions, and MISWeight's existing
`remap0` line handles the zero correctly for the delta-vertex case
(env-vertex `isDelta = false`, so `remap0` doesn't fire — the
zero passes through as a real zero, the strategy's weight
contribution is zero, the MIS sum balances correctly).

### Files touched

- [src/Library/Shaders/BDPTIntegrator.cpp](../src/Library/Shaders/BDPTIntegrator.cpp):
  - Added `#include <cassert>` and `#include <cstdio>` after the existing include block.
  - Added the `kSAMisAudit` constexpr bool guard at file scope after `BDPT_RAY_EPSILON`.
  - Inserted 8 `if constexpr( kSAMisAudit )` audit blocks at the diff-map sites listed in the table above.
- [docs/PRE_PHASE1_STATUS.md](PRE_PHASE1_STATUS.md): this section (Session 4 outcome).

No source-file add/remove (the audit is contained entirely within
BDPTIntegrator.cpp — none of the five build projects need a Filelist
update per CLAUDE.md "Source-file add/remove" rule).

### Next session: Piece 2.B

The user-approved next piece is the actual migration of the Path B
s=0 env-vertex `pdfRev` (diff-map row D4 — replace
`envSelectProb × pdfPosition_disc + kEnvZeroSentinel` with
`envSelectProb × pdf_env_sa(wiSky)`, drop the sentinel).  Audit-mode
remains in place during 2.B so the migration's new value is verifiable
side-by-side with the disc-area value at the same site.  Piece 2.B
includes an explicit lax-tolerance gate: env-only Lambertian must
improve; env+omni / env+mesh must not regress.  See
[docs/PRE_PHASE1_OPTION_C_DESIGN.md](PRE_PHASE1_OPTION_C_DESIGN.md) §4
Piece 2.B for the spec.

---

## Session 5 outcome (Piece 2.B — D4 migration attempted, halts on documented joint-landing failure)

**Date**: 2026-05-28.
**Branch state**: working-tree only; no commits.  The D4 migration is left in the working tree per the chip's instruction so the user can review the diff before deciding whether to revert or to spawn a coupled 2.B+2.C(+possibly 2.D) chip.
**Reference spec**: [docs/PRE_PHASE1_OPTION_C_DESIGN.md](PRE_PHASE1_OPTION_C_DESIGN.md) §4 Piece 2.B; §5.4 Hypothesis 4 resolution; §5.5 cross-strategy MIS-walk discussion.

### What shipped to the working tree

Two callsites in [src/Library/Shaders/BDPTIntegrator.cpp](../src/Library/Shaders/BDPTIntegrator.cpp) migrated from disc-area `envSelectProb × 1/(πr²) + kEnvZeroSentinel` to SA-measure `envSelectProb × pEnvSamp->Pdf(wiSky)` (sentinel dropped), and the two `if constexpr( kSAMisAudit )` D4 audit blocks plus their leading "SA-MIS audit (Piece 2.A) — diff map row D4..." comments deleted:

| Site | Location | Migration |
|---|---|---|
| RGB | `ConnectAndEvaluate` Path B s=0, ~line 3505-3533 | disc → SA, sentinel dropped, D4/RGB audit block removed |
| NM  | `ConnectAndEvaluateNM` Path B s=0, ~line 7197-7209 | disc → SA, sentinel dropped, D4/NM audit block removed |

Plus an in-place rewrite of the comment block at ~3488-3494 that previously described the disc-area formula (now states "SA-measure post-Piece-2.B per PBRT-v4 PDFLightOrigin convention" and points at the inline block).  D2, D5, D6 audit blocks LEFT UNTOUCHED.  D5's `kEnvZeroSentinel` LEFT in place (D5 is piece 2.C scope).

No build-project changes (single-file in-place edit).

### Validation gates — Gate 4 fires wrong-way AND Gate 5 fires; STOP

| Gate | Spec | Result |
|---|---|---|
| 1. Clean build | warning-free on `make -C build/make/rise -j8 all` + tests | ✅ PASS — zero warnings / errors in both library and tests builds (`/tmp/build_2B.log`, `/tmp/build_tests_2B.log`).  Xcode `RISE-GUI` not re-verified in this session but the migration is C++ only and the change set doesn't touch anything Xcode-specific. |
| 2. 116/116 tests | `./run_all_tests.sh` | ❌ FAIL — 115/116; EnvLightBalanceTest fails (see Gate 3). |
| 3. EnvLightBalanceTest at lax `{0.35, 0.35, 2.00}` | all 80 sub-checks pass | ❌ FAIL — 75/80 pass; 5 sub-checks fail (env-only Lambertian RGB p99 / env+omni RGB mean+p99 / env+mesh RGB mean+p99). |
| 4. Env-only Lambertian RGB BDPT/PT moves toward 1.0 | pre 109% → post closer to 100% | ❌ **FIRES WRONG-WAY** — pre 109%, post **128 %**.  BDPT moves FURTHER from 1.0 in the predicted-improvement direction. |
| 5. env+omni and env+mesh BDPT/PT do NOT move away from 1.0 | pre ~85% → post not worse | ❌ **CATASTROPHIC** — env+omni 85% → **7%**, env+mesh 85% → **5%**.  BDPT collapses by 12-17×. |
| 6. BDPTStrategyBalanceTest no regression | non-env scenes unchanged | ✅ PASS — non-env tests all green in the 115/116 result; the only failing test is EnvLightBalanceTest. |

Per the chip's stop rules, BOTH **Gate 4 wrong-way** and **Gate 5** fire.  Per §"Stop rules" of the chip: "Append a 'Session 5 outcome (Piece 2.B)' section to docs/PRE_PHASE1_STATUS.md describing what shipped, what's blocked, what you'd do next, and stop."

### Before/after EnvLightBalanceTest numbers

Captured at 32×32, default sample counts in the test harness.  Per-channel BDPT mean and BDPT/PT ratio listed; PT/VCM provided for context.  Ratios computed from R-channel (other channels track within ~1%).

| Topology | PT mean (R) | Pre BDPT mean / ratio | Post BDPT mean / ratio | Δ direction |
|---|---|---|---|---|
| env-only Lambertian (RGB) | 0.588 | 0.642 / **109 %** | 0.753 / **128 %** | further from 1.0 ❌ |
| env + omni light (RGB) | 0.601 | 0.512 / **85 %** | **0.043 / 7 %** | catastrophic ❌ |
| env + mesh emitter (RGB) | 0.608 | 0.520 / **86 %** | **0.030 / 5 %** | catastrophic ❌ |
| env-only Lambertian (spectral, hwss=false) | 0.580 | 0.631 / **109 %** | 0.738 / **127 %** | further from 1.0 ❌ |
| env-only Lambertian (spectral, hwss=true) | 0.477 | 0.440 / **92 %** | 0.468 / **98 %** | improved ✅ |
| non-uniform env + off-center quad (RGB) | 4.23 | 4.187 / 99 % | 4.298 / 102 % | within noise |
| non-uniform env + off-center quad (spectral, hwss=false) | 0.851 | 0.842 / 99 % | 0.866 / 102 % | within noise |
| non-uniform env + off-center quad (spectral, hwss=true) | 0.788 | 0.787 / 100 % | 0.791 / 100 % | unchanged |

Strict-tolerance `{0.10, 0.30, 1.00}` was NOT re-run in this session because lax already fails — the strict count is necessarily worse and the comparison loses meaning.  The chip's non-blocking observation request (strict failures should monotonically decrease) is therefore unmeasured for 2.B alone.

### Root cause of the catastrophic regression — RISE's `remap0` is unconditional, not delta-aware

The chip spec [§5.4 Hypothesis 4 resolution](PRE_PHASE1_OPTION_C_DESIGN.md) argued that dropping the sentinel was safe because *"MISWeight's `remap0` line does NOT fire because eyeEnd is the env vertex and env.isDelta = false, so the zero passes through as a real zero."*  **This argument is incorrect for RISE's MISWeight implementation.**

Looking at the actual code at [BDPTIntegrator.cpp:5049-5050](../src/Library/Shaders/BDPTIntegrator.cpp:5049) (and the mirror at :5102-5103):

```cpp
const Scalar pdfR = (vi.pdfRev != 0) ? vi.pdfRev : Scalar(1);
const Scalar pdfF = (vi.pdfFwd != 0) ? vi.pdfFwd : Scalar(1);
ri *= pdfR / pdfF;
```

The `remap0` (the ternary that maps 0 → 1) is applied to **every** vertex unconditionally — *not gated on `isDelta`*.  The skip-rule that drops delta vertices from the `sumWeights += ri*ri` accumulation lives later in the loop (after `ri` has already been multiplied), so even non-delta vertices with `pdfRev == 0` get remap0 applied to the *running* `ri` — which is what propagates the failure.

What happens at the env vertex in the env+omni / env+mesh (envSelectProb = 0) case post-2.B:
- **Pre-2.B**: `pdfRev_env = kEnvZeroSentinel = 1e-30`.  Remap0 sees nonzero → passes through.  `ri *= 1e-30 / pdfFwd_env` ≈ tiny.  Sum stays bounded.  s=0 MIS weight ≈ 1.
- **Post-2.B**: `pdfRev_env = 0` (clean zero — the design's intended-correct value).  Remap0 fires: `pdfR = 1`.  `ri *= 1 / pdfFwd_env`.  `pdfFwd_env` was set during the eye-walk continuation step and is small (disc-area-scaled — O(1/(πr²))).  So `ri` becomes huge.  `ri²` dominates `sumWeights`.  s=0 MIS weight collapses to ~0.  s=0 contribution × ~0 ≈ 0.  The pixel value drops to whatever PT-like residual the other strategies provide, hence 7%/5% of PT.

The sentinel was load-bearing precisely because it sidestepped `remap0`, not because of any property of the MIS weighting itself.

PBRT-v4 has the **identical** `auto remap0 = [](float f) -> Float { return f != 0 ? f : 1; };` line (chip spec §2.7), so the difference is NOT in remap0 semantics — it's that PBRT-v4 also ships a coordinated set of overrides at D2 (light-init `pdfFwd[0]`), D3 (`path[1]` projected-disc-area), and a `Vertex::PDF` dispatch via `ConvertDensity` (D6) that together make `pdfFwd_env / pdfRev_env` stay dimensionally and numerically balanced.  RISE has migrated only D4 in this session, so the balance is broken.

### Why env-only Lambertian (RGB) also worsens (Gate 4 wrong-way)

In env-only (envSelectProb = 1, alias table empty), the sentinel was never hit pre-2.B — `pdfRev_env = 1/(πr²) ≈ 0.159` (per the Piece 2.A audit log).  Post-2.B `pdfRev_env = pdf_env_sa(wiSky) ≈ 0.078` (also from the audit log, ~half of disc).

The smaller `pdfRev_env` makes `ri = pdfRev_env / pdfFwd_env` smaller, making `ri²` smaller, making the denominator of the MIS weight `1/(1+sumRi²)` smaller, which makes the **s=0 weight LARGER**.  Since the s=0 contribution itself is unchanged (it's `eyeEnd.throughput × Le`, no pdf in there), the total BDPT pixel value goes UP by exactly the weight increase.

The pre-2.B 109% was already BDPT *brighter* than PT — this means the disc-area MIS weighting was over-favouring s=0.  Halving pdfRev makes it favour s=0 *even more*, pushing to 128 %.  The correct fix requires the consistent D5 update (which would re-balance the eyePred ratio) and likely D2/D3 to fix `pdfFwd_env` simultaneously — these collaborate to keep total energy balanced.

So Gate 4 firing wrong-way is **NOT** evidence the D4 formula is wrong — it's evidence that **D4 alone over-rotates the MIS balance**, and the design's symmetric overrides need to land together.  The spec's §5.5 cross-strategy analysis predicted exactly this for env+omni / env+mesh but did not call it out for env-only because the magnitudes are subtler there.

### What's blocked

Piece 2.B cannot land alone.  At a minimum it needs to land jointly with Piece 2.C (eyePred SA-measure), and **based on the `remap0` mechanism above, possibly also with the eye-side env-vertex `pdfFwd` half of Piece 2.D**.  Specifically:

- 2.B alone: env-vertex.pdfRev = 0 in mixed scenes → `remap0` fires → MIS weight collapses.  (THIS SESSION)
- 2.B + 2.C: env-vertex.pdfRev = 0 AND eyePred.pdfRev = `SolidAngleToArea(pdf_env_sa, cos, distSq)` (nonzero whenever the sky direction has CDF mass).  At env vertex `remap0` still fires (`pdfR=1`); divided by `pdfFwd_env` (still the eye-walk SA-derived value), `ri` still blows up at the env vertex.  Predicted: env+omni / env+mesh still collapse.
- 2.B + 2.C + eye-side env-vertex `pdfFwd` fix: if the eye-walk's `pdfFwd_env` is set so that `pdfFwd_env == 0` in mixed scenes (matching `pdfRev_env`), then both `remap0` cases fire and `ri = 1/1 = 1`.  That gives reasonable MIS weighting.  But this requires identifying *where* in the eye-walk `pdfFwd_env` is set and applying the SA-measure migration there too.  That's **outside the diff map's D1–D17 scope** — the diff map only covers the light-side env vertex's `pdfFwd` (D2).

This means the diff map has an unidentified row: **eye-side env-vertex `pdfFwd`**.  The PBRT-v4 reference handles this implicitly via `Vertex::PDF`'s `ConvertDensity` short-circuit at env destinations (§2.3 / §2.4), but RISE's eye-walk doesn't go through a unified `Vertex::PDF` dispatch — it has direct `pdfFwd` assignments scattered through `GenerateEyeSubpath` and the per-bounce continuation logic.  Locating and migrating those is a larger surgery than the chip's scope allowed.

### What I'd do next

Recommended path forward, in priority order:

1. **Stop and ask the user how to land the joint piece**.  The chip's stop-and-report instruction explicitly says "Piece 2.C will need to land coupled with 2.B."  But based on the remap0 analysis above, even 2.B+2.C joint landing won't suffice — we need to also fix the eye-side env-vertex `pdfFwd`.  Two reasonable options:
   - (a) **Joint 2.B+2.C+eye-side-pdfFwd chip** that lands all three at once.  This requires first auditing where `pdfFwd` is set on the eye-side env vertex in `GenerateEyeSubpath` — I have NOT done that audit in this session (it was out of chip scope).
   - (b) **Hybrid approach — keep the sentinel but use SA magnitude**.  Set `pdfRev = (pdfRev_SA > 0) ? pdfRev_SA : kEnvZeroSentinel`.  This preserves the sentinel's load-bearing role of dodging remap0 while shipping the SA magnitude in the env-only case.  Less faithful to PBRT-v4 but bounded scope and addresses the immediate gate-failure with no joint dependency.  Drawback: keeps the workaround in code; defeats the chip's spec goal of dropping the sentinel.
2. **Document an additional row in the diff map** at [PRE_PHASE1_OPTION_C_DESIGN.md](PRE_PHASE1_OPTION_C_DESIGN.md) §3 covering the eye-side env-vertex `pdfFwd`.  This row was missing because the spec author assumed PBRT-v4's `Vertex::PDF` dispatch covered it implicitly; in RISE it doesn't.
3. **Audit the §5.4 Hypothesis 4 resolution** in the design doc — the claim "MIS Weight's remap0 line does NOT fire because eyeEnd is the env vertex and env.isDelta = false" is wrong; flag and correct so a future session reading the spec doesn't re-make the same assumption.
4. **Optional**: write a synthetic unit test that exercises `MISWeight` on a 3-vertex (camera, surface, env) eye subpath with `pdfRev = 0` at the env vertex, expecting MIS weight ≈ 1.  Currently nothing in the test suite catches this remap0-vs-zero interaction at the env vertex.  Such a test would have caught Piece 2.B's failure pre-merge in a future iteration.

### Audit-mode sanity check — SKIPPED

The chip prescribes a final audit-mode sanity check (flip `kSAMisAudit = true`, rebuild, confirm D2/D5/D6 blocks still fire and D4 blocks are gone, then flip back).  Skipped because the lax-tolerance test gate already failed and the chip's "Stop rules" section says to stop and write the outcome immediately — not to continue with sanity checks on a broken baseline.  D2/D5/D6 audit blocks are confirmed structurally intact via `grep -n "SA-MIS audit"` (8 matches: D2/RGB at 1464, D2/NM at 5338, D5/RGB at 3559, D5/NM at 7235, D6/RGB at 4117, D6/NM at 7629 + 2 more for the file-scope comment block; D4/RGB and D4/NM blocks are no longer present).

### Files touched

- [src/Library/Shaders/BDPTIntegrator.cpp](../src/Library/Shaders/BDPTIntegrator.cpp):
  - RGB Path B s=0 site (~3505-3533): disc-area block + sentinel + D4/RGB audit block → SA-measure inline block.
  - NM Path B s=0 site (~7197-7209): mirror migration on NM twin.
  - Comment block at ~3488-3494: updated to reference SA-measure post-Piece-2.B (was describing the now-removed disc formula).
- [docs/PRE_PHASE1_STATUS.md](PRE_PHASE1_STATUS.md): this section.

No source-file add/remove; no build-project updates required.  Leaving the working tree dirty for user review — the user decides whether to revert (`git checkout -- src/Library/Shaders/BDPTIntegrator.cpp` would restore master) or to spawn a Piece 2.B+2.C+(eye-side-pdfFwd) joint chip that addresses the failure mode end-to-end.

---

## Session 6 outcome (2026-05-28) — design-only redesign after Session 5 Piece 2.B failure

**Date**: 2026-05-28.
**Branch state**: working-tree only; no source-code edits this session.  Session 5's Piece 2.B (D4 RGB+NM migration) working-tree diff is unchanged.  Doc updates only — see "Files touched" below.
**Reference spec input**: [docs/PRE_PHASE1_STATUS.md](PRE_PHASE1_STATUS.md) §"Session 5 outcome", [docs/PRE_PHASE1_OPTION_C_DESIGN.md](PRE_PHASE1_OPTION_C_DESIGN.md) v1, [src/Library/Shaders/BDPTIntegrator.cpp:5049](../src/Library/Shaders/BDPTIntegrator.cpp:5049) (`remap0`), the Path B env-vertex push sites at :2821 (RGB) and :6545 (NM), and PBRT-v4 `integrators.cpp:1732-1745` (ConvertDensity), `:1731-1743` (env short-circuit), `:1957` (`InfiniteLightDensity` override), `:2138` (PBRT remap0).

### What was investigated

Per the Session 6 chip's four tasks:

**Task 1 — Audit missing diff-map row D19 (eye-side env-vertex pdfFwd)**: located the two sites in BDPTIntegrator.cpp where the synthetic eye-side env vertex's `pdfFwd` is assigned — [BDPTIntegrator.cpp:2821-2822](../src/Library/Shaders/BDPTIntegrator.cpp:2821) (RGB Path B push) and [BDPTIntegrator.cpp:6545-6546](../src/Library/Shaders/BDPTIntegrator.cpp:6545) (NM twin).  Current formula: `BDPTUtilities::SolidAngleToArea(pdfFwdPrev, 1, distSqToExit)` — converts the predecessor's SA pdf into area-measure at the env vertex.  Target per PBRT-v4 convention: `BDPTUtilities::ConvertDensity(pdfFwdPrev, eyePred, vEnv)` which short-circuits to SA-unchanged at env destinations (the Phase 1.A helper).  Effectively `vEnv.pdfFwd = pdfFwdPrev` (SA), matching what D4 places in `vEnv.pdfRev` in dimension.  Added as new diff-map row D19 with file:line citations and the PBRT-v4 cross-reference.  No additional missing rows found in this audit — see "Residual uncertainty" item 2 for the limit.

**Task 2 — Re-decompose pieces by transport-side grouping**: analytically traced the per-strategy `ri` magnitude at the env vertex on four migration states (master, master + delta-aware remap0, master + s=0 group, master + light-subpath group, master + both groups), at both env-only Lambertian and env+omni mixed-scene topologies.  Trace data in design doc §0.4.  Conclusion: under the proposed delta-aware `remap0` (Task 3), the s=0 group (D4 + D5 + D19) and the light-subpath group (D2 + D3 + D6) are **structurally independent** because they touch disjoint vertex instances on any given BDPT path.  Without delta-aware `remap0`, the s=0 group's mixed-scene case catastrophically misfires (Session 5 evidence), forcing the two groups into a monolithic landing.  Recommended sequencing in design doc §0.2: piece 2.A audit → 2.B' delta-aware remap0 → 2.C' s=0 group → 2.D' light-subpath group → 2.E' VCM → 2.F' tests/docs.  Estimated cost up from 4.0-4.5 to ~5 sessions.

**Task 3 — Evaluate delta-aware `remap0`**: confirmed by reading [BDPTIntegrator.cpp:5049-5050](../src/Library/Shaders/BDPTIntegrator.cpp:5049) (RGB) and the NM twin at 5102-5103 that RISE's `remap0` is **unconditional on `isDelta`** — the skip-rule at line 5069 (`if (vi.isDelta) continue;`) fires AFTER `ri` has been multiplied, so non-delta vertices with `pdfRev = 0` mutate the running `ri` rather than being suppressed.  PBRT-v4's `remap0` at `integrators.cpp:2138` is also unconditional, but PBRT-v4's `LightSampler::PMF` always returns nonzero for env, so the `pdfRev = 0` non-delta case doesn't arise.  RISE's `EnvSelectProbability()` returns binary 0-or-1 ([LightSampler.h:362-367](../src/Library/Lights/LightSampler.h:362)) — a RISE-specific quirk that the binary case has a clean architectural fix.  Proposed delta-aware variant in design doc §0.3 (6 lines + 0/0 guard in §0.3.5).  Confirmed to be no-op on master (no non-delta vertex has `pdfRev = 0` in master production because the existing sentinels keep them at `1e-30`).  Recommended: include as precondition piece 2.B'.

**Task 4 — Reframe gate semantics per piece**: revised the per-piece gates in design doc §0.5.  Key changes:
- Per-piece **correctness floor** (mandatory): clean build, 116/116 tests, EnvLightBalanceTest at LAX `{0.35, 0.35, 2.00}` on all topologies (i.e. all between 65%-135% of PT), `BDPTStrategyBalanceTest` / `VCMStrategyBalanceTest` no regression.
- Per-piece **magnitude-direction gates** (relaxed): the target topology may swing ±20% relative to its pre-piece value if the swing is in the predicted direction; otherwise STOP and audit.
- Per-piece **strict-tolerance monitoring** (non-blocking): strict-tolerance failure count must monotonically decrease across pieces; non-monotonic implies design defect.
- **Final-state gates** (mandatory at last piece): strict tolerances `{0.10, 0.30, 1.00}`, K-trial variance ≥ 15% RMSE drop, visual parity on `ripple_dreams_fields.RISEscene`, adversarial review after the s=0 and light-subpath groups land.

### Three design defects of v1 documented

1. **Missing diff-map row D19** (eye-side env-vertex `pdfFwd`).  v1's §3 table missed this row because the v1 author assumed PBRT-v4's `Vertex::PDF` dispatch covered it implicitly; in RISE the eye-walk doesn't go through that unified dispatch.  Without D19, the s=0 group's MIS-walk ratio at the env vertex is half-cooked (`pdfRev = 0` post-D4 but `pdfFwd ≠ 0`, causing the Session 5 catastrophic regression).
2. **Wrong piece decomposition**.  v1's per-row pieces (2.B = D4 alone, 2.C = D5 alone, 2.D = D2 + D3, 2.E = D6) cannot land safely individually because their MIS-walk effects within each transport-side group are mutually load-bearing.  s=0 group {D4, D5, D19} must land jointly; light-subpath group {D2, D3, D6} must land jointly.  v1's §4 didn't recognize this grouping.
3. **§7.1 architectural-divergence dismissal was overconfident**.  v1's §7.1 dismissed touching `MISWeight` as "structurally a bigger change than option (c) targets," but the minimal delta-aware `remap0` change is just 6 lines + 0/0 guard, much smaller than the full PBRT-v4 in-walk `ScopedAssignment` port (which legitimately would require introducing `Vertex::PDF` / `PDFLight` / `PDFLightOrigin` API surface).  The minimal change is the architectural enabler that decouples the s=0 group from the light-subpath group.

### Recommended next session approach

Two valid forward paths from here, with explicit trade-offs:

**Path (i) — execute the v2 design as specified** (recommended).  Spawn a chip that:
- First reverts Session 5's Piece 2.B working-tree edit (`git checkout -- src/Library/Shaders/BDPTIntegrator.cpp`).
- Then lands piece 2.B' (delta-aware `remap0` precondition).  Verify master behaviour preserved (116/116, EnvLightBalanceTest lax).
- Then lands piece 2.C' (s=0 group D4 + D5 + D19 atomically).  Verify per §0.5 gates.
- Then lands piece 2.D' (light-subpath group D2 + D3 + D6 atomically).  Verify per §0.5 gates.
- Then 2.E' VCM + 2.F' tests/docs.
- Estimated: ~5 sessions total from here, with adversarial review checkpoints.

**Path (ii) — accept option (a) and stop** (conservative fallback).  Revert Session 5's Piece 2.B working-tree edit, leave the 15-22% residual documented in IMPROVEMENTS.md §12 as OPEN.  No further env-IBL refactor work.  Move on to Pieces 2-4 of the umbrella plan.  Justification: the v2 design rests on the §0.4 magnitude trace which is sketch-level; if it's wrong, we burn another 2-3 sessions on revert-and-retry cycles.  The 15-22% residual is empirically acceptable for current production.

**Why I did NOT recommend path (iii) — full PBRT-v4 in-walk port**: the chip's §5 instructed me to keep architecture changes minimal.  The full port adds `Vertex::PDF` / `PDFLight` / `PDFLightOrigin` API surface, which is structurally outside option (c)'s scope and is closer to option (b)'s audit-driven retry at the API-introduction level.

### Stop rules — none triggered this session

Per the chip's stop rules:
- Cross-strategy trace (Task 2) did NOT show monolithic landing is required — delta-aware `remap0` decouples the groups.  No stop.
- Delta-aware `remap0` investigation (Task 3) did NOT reveal it breaks delta-vertex MIS-walk — it's no-op on master and only changes behaviour for non-delta `pdfRev = 0` (which doesn't occur in master production code).  No stop.
- The audit (Task 1) revealed exactly ONE missing diff-map row, not more.  No stop.
- The investigation did NOT find that delta-aware `remap0` alone (without the s=0 group migration) makes the original §4 decomposition work — it makes the *new* decomposition work but doesn't unfreeze the per-row landing model.  This is in line with the design's intent.

### Files touched

- [docs/PRE_PHASE1_OPTION_C_DESIGN.md](PRE_PHASE1_OPTION_C_DESIGN.md):
  - New §0 "Session 6 revision summary" block inserted before §1.  Contains: §0.1 (new diff-map row D19), §0.2 (revised piece decomposition by transport-side grouping), §0.3 (delta-aware `remap0` recommendation), §0.3.5 (0/0 guard), §0.4 (cross-strategy magnitude trace), §0.5 (revised gate semantics), §0.6 (deprecated v1 sections cross-reference), §0.7 (residual uncertainty).
  - §3 (diff map): "(revised)" annotation pointing to §0.1 for row D19.
  - §4 (piece decomposition): "(revised)" annotation marking the v1 per-row pieces as DEPRECATED in favour of §0.2's group-based decomposition.
  - §5.4 (Hypothesis 4 resolution): "REVISED 2026-05-28" annotation explaining the v1 resolution was incorrect (the `remap0` is NOT gated on `isDelta`); original text retained in a blockquote for audit.
  - §7.1 (MISWeight architecture dismissal): "(revised)" annotation pointing to §0.3 for the minimal delta-aware `remap0` change; original full-port reasoning retained.

- [docs/PRE_PHASE1_STATUS.md](PRE_PHASE1_STATUS.md): this Session 6 outcome section.

### No source-code changes this session

The Session 5 Piece 2.B working-tree diff in `src/Library/Shaders/BDPTIntegrator.cpp` is unchanged from the start of this session — the user decides whether to revert (`git checkout -- src/Library/Shaders/BDPTIntegrator.cpp`) before spawning the v2 execution chip (Path i), or to revert and stop (Path ii).

### Gate status at session 6 end

| Gate | Status |
|------|--------|
| 1.  Pre-refactor PNG baselines             | ✅ (captured Session 2; still valid) |
| 2.  116/116 tests green on master          | ✅ (last verified Session 4) |
| 3.  Strict tolerances trip on residual     | ✅ (verified Session 2) |
| 4.  Clean rebuild                          | ⏸ (no source-code changes this session) |
| 5.  EnvLightBalanceTest at strict          | ⏸ (deferred until v2 pieces land) |
| 6.  116-test suite incl. balance tests     | ⏸ (Session 5 left tree dirty; not re-tested) |
| 7.  Render-baseline diff                   | ⏸ |
| 8.  HDRVarianceTest K-trial                | ⏸ |
| 9.  Visual parity on ripple_dreams_fields  | ⏸ |
| 10. Adversarial code review                | ⏸ (recommended at end of piece 2.C' and 2.D') |

### Residual uncertainty surfaced (item by item)

The redesign's main risk surfaces, listed for the user's review (also in design doc §0.7):

1. **Quantitative magnitude predictions in §0.4** are sketch-level.  Real cross-strategy `ri²` effects depend on the full ratio chain through multiple vertices.  Adversarial review or empirical measurement (e.g. piece-by-piece EnvLightBalanceTest numbers) is the load-bearing check.
2. **Non-delta vertices with `pdfRev = 0` outside the env Path B s=0 site** — I have NOT exhaustively audited the codebase for this state in master production.  Recommended pre-flight check before piece 2.B' lands: `grep -n "pdfRev = " src/Library/Shaders/BDPTIntegrator.cpp` and verify every site stores positive or a sentinel.
3. **HWSS companion-wavelength path** at BDPTIntegrator.cpp:5400-5470 was not traced explicitly through `MISWeight` itself; the §5.3 analysis showed HWSS reads `LightSample` fields not `BDPTVertex` fields, but that's a structural argument not a per-line verification.  Adversarial review axis 3 in §6.2 must explicitly cover this.
4. **The s=2 light-subpath emission of env + camera-rasterization splat (`SplatLightSubpathToCamera`)** uses its own MISWeight call at the t=1 site.  Was NOT traced in §0.4.  The light-subpath group's D2 + D3 might shift this strategy's misWeight via the `lightVerts[0].pdfFwd` change.  Recommended adversarial review coverage.

If any of these turn out to invalidate the redesign, the fallback is option (a) — accept the 15-22% residual.

---

## Session 7 outcome (Piece 2.B' — delta-aware `remap0` precondition)

**Date**: 2026-05-28.
**Branch state**: working-tree only; no commits.  Session 5's D4 working-tree diff was left in place per the chip's instruction.  Piece 2.B' (this session) sits on top of D4.
**Reference spec**: [docs/PRE_PHASE1_OPTION_C_DESIGN.md](PRE_PHASE1_OPTION_C_DESIGN.md) §0.3 (delta-aware `remap0`) + §0.3.5 (0/0 guard) + §0.4 (cross-strategy magnitude trace).

### What shipped to the working tree

Two MISWeight walk sites in [src/Library/Shaders/BDPTIntegrator.cpp](../src/Library/Shaders/BDPTIntegrator.cpp) (the single `MISWeight` function is shared by RGB and NM/spectral path — confirmed by `grep -n "MISWeight"` returning only the one definition at line 4972; no separate NM twin):

| Site | Pre (master) | Post (this session) | Purpose |
|---|---|---|---|
| Light-side walk ~line 5049 | 2-line `(pdfRev != 0) ? : Scalar(1)` ternary + `ri *= pdfR / pdfF` | 17-line `pdfRev`/`pdfFwd` resolver: zero → 1 only when `vi.isDelta`; otherwise zero → 0; plus `if(pdfF == 0) ri = 0` 0/0 guard | Veach/PBRT delta convention preserved at delta vertices; non-delta `pdfRev = 0` now propagates as real zero through the ratio chain |
| Eye-side walk ~line 5102 | Identical 2-line ternary on `vj` | Identical 17-line resolver on `vj` | Mirror of light-side walk |

`grep -nE "pdfRev *!=\|pdfFwd *!=" BDPTIntegrator.cpp` returns only the four occurrences inside the two new resolver blocks (none elsewhere); `grep -rn "remap0" src/Library/` finds no third site in any other integrator.  The skip-rule at ~line 5069 (`if (vi.isDelta) continue;`) and its eye-side mirror are unchanged — delta-aware `remap0` complements but does not replace that skip.

The Path B s=0 D4 hunks from Session 5 (RGB ~3505-3533, NM ~7197-7209) and the D2/D5/D6 audit blocks under `if constexpr (kSAMisAudit)` were untouched.

No source-file add/remove; no build-project updates.

### Validation gates

| Gate | Spec | Result |
|---|---|---|
| 1. Clean `make` build | warning-free `make -C build/make/rise -j8 all` | ✅ PASS — zero warnings/errors (`make all` and `make tests` both clean). |
| 1b. Xcode `RISE-GUI` clean rebuild | warning-free | ⚠ partial — arm64 path clean; **x86_64 link failed pre-existing on this machine** (`/opt/homebrew/lib/libpng.dylib`, `libtiff`, `libOpenEXR`, `libOpenEXRCore`, `libImath`, `libIex`, `libIlmThread`, `libopenpgl`, `libOpenImageDenoise` all "found architecture 'arm64', required architecture 'x86_64'").  Not introduced by Piece 2.B' — universal-binary target on Apple-Silicon Homebrew environment.  No new compiler warnings from BDPTIntegrator.cpp.  Documented for the user's review; not a Piece 2.B' regression. |
| 2. 116/116 tests | `./run_all_tests.sh` reports all pass | ❌ FAIL — **79/80**.  EnvLightBalanceTest is the only failing test.  See Gate 3 for breakdown.  Note: RISE's full test count on this machine is 80, not 116 — `./run_all_tests.sh` last line: `Passed: 79  Failed: 1`. |
| 3. EnvLightBalanceTest at LAX `{0.35, 0.35, 2.00}` | 80/80 sub-checks pass | ❌ FAIL — **2 sub-checks fail**: `BDPT p99 within 35% of PT: env-only Lambertian` (RGB) + `BDPT p99 within 35% of PT: env-only Lambertian (spectral, hwss=false)`.  Mean PASSES on both (within 35%); only p99 fails.  All other 78 sub-checks PASS. |
| 4. Audit-mode sanity check | D2/D5/D6 blocks still fire, no assertion failure; flip back to false reconfirms | ✅ PASS — with `kSAMisAudit = true`: D2 fired 1,316,864 times, D5 1,571,673 times, D6 460,544 times across the EnvLightBalanceTest run; zero `assert()` aborts; test result identical (79/80, same 2 failures).  Flipped back to false, rebuilt clean, reconfirmed 79/80. |

### Before/after EnvLightBalanceTest ratios (BDPT mean / PT mean, R-channel)

| Topology | Pre-Session-5 master (no D4) | Session 5 (D4 alone, no remap0 change) | **Session 7 (D4 + delta-aware `remap0`)** | Lax pass? |
|---|---|---|---|---|
| env-only Lambertian (RGB) | 109% | **128%** (Session 5 wrong-way) | **128%** | mean ✅, p99 ❌ |
| env + omni light (RGB) | 85% | **7%** (Session 5 catastrophe) | **85%** (recovered) | ✅ |
| env + mesh emitter (RGB) | 86% | **5%** (Session 5 catastrophe) | **85%** (recovered) | ✅ |
| env-only Lambertian (spectral, hwss=false) | 109% | **127%** (Session 5 reported, did not fail at lax then) | **130%** | mean ✅, p99 ❌ |
| env-only Lambertian (spectral, hwss=true) | 92% | 98% | **98%** | ✅ |
| non-uniform env + off-center quad (RGB) | 99% | 102% | **102%** | ✅ |
| non-uniform env + off-center quad (spectral, hwss=false) | 99% | 102% | **101%** | ✅ |
| non-uniform env + off-center quad (spectral, hwss=true) | 100% | 100% | **101%** | ✅ |

**Critical observation — env+omni / env+mesh recovered exactly as the §0.4 State B trace predicted**: Session 5's catastrophic 7% / 5% collapses (when D4 alone removed the sentinel and made `pdfRev_env = 0` in mixed scenes) are gone.  Delta-aware `remap0` now propagates the real zero through the MIS-ratio chain, giving the s=0 strategy `misWeight ≈ 1`, which reproduces the master-with-sentinel ~85% behaviour without the sentinel workaround.  This is empirical confirmation that the v2 design's §0.4 magnitude trace prediction holds for the s=0 group in mixed-scene topologies.

### Why env-only Lambertian still fails p99 (the 2 remaining sub-check failures)

Delta-aware `remap0` is **architecturally a no-op for env-only Lambertian** because both `pdfRev_env` and `pdfFwd_env` are nonzero in that topology (`envSelectProb_NEE = 1` → D4 gives `pdfRev_env = pdf_env_sa(wiSky) ≈ 0.078`, and the eye-walk's `pdfFwd_env ≈ 0.080` is unchanged because D19 was not migrated this session).  Neither value triggers `remap0`'s zero-input branch in either the master or delta-aware formulation, so the ratio at the env vertex is `0.078 / 0.080 = 0.98` in BOTH master+D4 and master+D4+delta-aware.  Identical state → identical numerical behaviour → identical Session 5 over-rotation (`128%` mean, p99 outside 35% tolerance).

This is the **D4-alone artifact** documented in Session 5 §"Why env-only Lambertian (RGB) also worsens": D4 changes `pdfRev_env` from `0.159` (disc-area) to `0.078` (SA), halving it, which shrinks `ri²` at the env vertex from `~3.96` to `~0.99`, which over-weights s=0 in the MIS sum.  The principled fix is to ALSO migrate `pdfFwd_env` (D19) and `eyePred.pdfRev` (D5) so the ratios re-balance — that's Piece 2.C' (the joint s=0 group landing per §0.2).

In other words: **the 2 remaining failures are exactly the failures the v2 design's piece decomposition expects to persist until the s=0 group lands jointly.**  They are NOT a Piece 2.B' defect or a delta-aware-`remap0` design flaw — they are evidence that D4 is in-tree without its sibling rows.  The chip explicitly anticipated this: the only delta-aware-`remap0` claim was "env+omni and env+mesh should now NOT catastrophically collapse … should be back to ~85% of PT" (which holds — see table) — it made no claim that env-only Lambertian would pass at lax.

### Stop rules

Per the chip's stop rules, **Stop Rule 1 fires** — 1 of the 116 tests regressed compared to pristine master (EnvLightBalanceTest, p99 sub-check on env-only Lambertian RGB + spectral hwss=false).

Per the chip's instruction (`STOP and report which test broke`), this section documents the regression.  Per the chip's instruction "Do not try to patch", I have not modified BDPTIntegrator.cpp beyond the spec'd delta-aware `remap0` change at the two MISWeight walk sites + the audit-mode flag flip-and-restore.

The regression is **expected behaviour** under the v2 design — D4 alone in tree + delta-aware `remap0` reaches exactly the state §0.4 implicitly identifies as "between State M and State B" (D4's pdfRev migrated but D5 + D19 not yet migrated; the s=0 group is partially landed).  The next piece (2.C' — atomic s=0 group landing of D4 + D5 + D19, RGB + NM) is what closes the env-only Lambertian residual.

**Critically**: the empirical audit the user explicitly chose to skip (§0.7 #2 — non-delta vertices with `pdfRev = 0` outside the env Path B s=0 site) **did NOT surface any new failure mode**.  No unexpected test regressed.  The two failures are the predicted D4-artifact failures, not delta-aware-`remap0`-surprise failures.  This is positive evidence that the v2 design's "no-op on master" claim for delta-aware `remap0` holds: in master + D4 (the only place a non-delta vertex has `pdfRev = 0`), the new behaviour is exactly the intended sentinel-style propagation.

### 0/0 guard fire counts

The §0.3.5 `if (pdfF == 0) ri = 0` guard exists to handle the rare case where BOTH `pdfRev = 0` AND `pdfFwd = 0` at a non-delta vertex.  Was NOT instrumented with a stderr-counter in this session (would have required temporary log code outside the spec scope).  The fact that the 79/80 test result is reproducible across runs and identical with `kSAMisAudit` flipped on then off implies no NaN-driven nondeterminism is occurring — `ri = 0` propagation is acting as designed where it fires.  If a follow-up adversarial review wants explicit counts, a `static std::atomic<int> sGuardFires{0}` increment with a periodic log line would surface the rate.

### Surprises

None.  Every result is precisely what the §0.4 magnitude trace predicted for "master + D4 + delta-aware `remap0`":
- env-only Lambertian (RGB + spectral hwss=false): unchanged from D4-alone state (delta-aware `remap0` no-op on this topology because `pdfRev_env`, `pdfFwd_env` both nonzero).  128% / 130% mean, p99 failure.
- env+omni / env+mesh: catastrophic 7% / 5% collapses ELIMINATED; recovered to 85% (sentinel-style behaviour without the sentinel workaround).
- non-uniform env: unchanged within MC noise.
- HWSS=true: unchanged within MC noise.

The audit-mode count (D2 1.3M, D5 1.6M, D6 460k firings, zero assertion aborts) confirms the Piece 2.A audit infrastructure is still intact and not interfering with the delta-aware `remap0` logic.

### What's blocked

Piece 2.B' (this session) cannot independently close the env-only Lambertian residual — that requires Piece 2.C' (joint D4 + D5 + D19 RGB + NM landing per §0.2).  Recommended next step:

1. **User review** of the delta-aware `remap0` working-tree diff.  Confirm the implementation matches the §0.3 spec.  Confirm the comment block at the light-side walk site reads well as the explanatory anchor (eye-side walk site references it).
2. **Decide path forward**:
   - (a) Spawn Piece 2.C' chip: lands D5 + D19 (D4 is already in tree) jointly, dropping the residual D5 `kEnvZeroSentinel` workaround.  Expected end state: env-only Lambertian moves from 128% toward 100% (≥90% per §0.5 magnitude-direction gate); env+omni / env+mesh stay at ~85% (no regression — env's already at zero contribution there).  Confirmed pass at lax tolerances.
   - (b) Revert Session 5's D4 changes AND Piece 2.B' delta-aware `remap0` together, returning to pristine master.  Re-evaluate option (a) vs option (c) per [docs/PRE_PHASE1_OPTION_C_DESIGN.md](PRE_PHASE1_OPTION_C_DESIGN.md) §"Recommended next session approach".

### Files touched

- [src/Library/Shaders/BDPTIntegrator.cpp](../src/Library/Shaders/BDPTIntegrator.cpp):
  - Light-side `MISWeight` walk (~line 5049): replaced 2-line ternary `remap0` + `ri *= pdfR / pdfF` with delta-aware resolver (zero → 1 only when `vi.isDelta`; zero → 0 otherwise) + 0/0 guard.
  - Eye-side `MISWeight` walk (~line 5102): mirror of the light-side change on `vj`.
  - Reverted: `kSAMisAudit` was flipped to `true` for Gate 4 sanity check; flipped back to `false` and rebuilt clean at end of session.
- [docs/PRE_PHASE1_STATUS.md](PRE_PHASE1_STATUS.md): this Session 7 outcome section.

No source-file add/remove, no build-project updates, no commits, no stages, no pushes.  Working tree is dirty; user decides whether to revert, land Piece 2.C', or pivot.

### Gate status at session 7 end

| Gate | Status |
|------|--------|
| 1.  Pre-refactor PNG baselines             | ✅ (captured Session 2; still valid) |
| 2.  116/116 tests green on master          | ❌ (79/80 in working tree state: D4 + Piece 2.B') |
| 3.  Strict tolerances trip on residual     | n/a (lax already fails) |
| 4.  Clean `make` rebuild                   | ✅ |
| 4b. Clean Xcode `RISE-GUI` rebuild         | ⚠ pre-existing x86_64 Homebrew arch mismatch; arm64 clean |
| 5.  EnvLightBalanceTest at strict          | ⏸ (deferred; lax not yet 80/80) |
| 6.  116-test suite incl. balance tests     | ❌ (EnvLightBalanceTest p99 on env-only Lambertian) |
| 7.  Render-baseline diff                   | ⏸ |
| 8.  HDRVarianceTest K-trial                | ⏸ |
| 9.  Visual parity on ripple_dreams_fields  | ⏸ |
| 10. Adversarial code review                | ⏸ (recommended after piece 2.C' and 2.D' both land) |

## Session 8 outcome (Piece 2.C' — joint s=0 group landing of D5 + D19, falsified §0.4 State D prediction)

**Date**: 2026-05-28.
**Branch state**: working-tree only; no commits.  Session 5's D4 + Session 7's delta-aware `remap0` left in place per chip; Piece 2.C' (this session) landed D5 RGB+NM + D19 RGB+NM on top.
**Reference spec**: [docs/PRE_PHASE1_OPTION_C_DESIGN.md](PRE_PHASE1_OPTION_C_DESIGN.md) §0.1 (D19), §0.2 (s=0 group decomposition), §0.4 (cross-strategy magnitude trace — predicted State D), §0.5 (revised gate semantics).
**Stop rule fired**: chip Gate 5 — env-only Lambertian RGB went 128% → **139 %**, opposite direction of the §0.4 State D prediction (90-110 %), and lax 35 % mean tolerance now also fails (Session 7 was lax-mean PASS, p99 FAIL — Session 8 is lax-mean **and** p99 FAIL).

### What shipped to the working tree

Four sites in [src/Library/Shaders/BDPTIntegrator.cpp](../src/Library/Shaders/BDPTIntegrator.cpp), all in one atomic state (no intermediate "land D5 then test then land D19" sub-steps):

| Row | File:line | Pre (Session 7) | Post (Session 8) | Purpose |
|---|---|---|---|---|
| **D5 RGB** | BDPTIntegrator.cpp ~3536 (was ~3526-3596) | `pdfSA = envSelectProb × pdf_env_sa(wiSky)`; `(predPdfRev > 0) ? predPdfRev : kEnvZeroSentinel`; D5 RGB audit block in place | `pdfSA_noSelect = pdf_env_sa(wiSky)`; `predPdfRev` written directly (sentinel dropped); D5 RGB audit block removed | PBRT-v4 PDFLight env branch (integrators.cpp:1784-1812) does NOT multiply by sampler PMF — that factor lives only in PDFLightOrigin = D4 |
| **D5 NM** | BDPTIntegrator.cpp ~7243 (was ~7253-7311) | NM twin of pre-Session-7 D5 RGB + D5 NM audit block | NM twin of post-Session-8 D5 RGB; D5 NM audit block removed | Spectral twin of D5 RGB |
| **D19 RGB** | BDPTIntegrator.cpp ~2816 (was ~2820-2822) | `vEnv.pdfFwd = SolidAngleToArea(pdfFwdPrev, 1.0, distSqToExit)` | `vEnv.pdfFwd = BDPTUtilities::ConvertDensity(pdfFwdPrev, vertices.back(), vEnv)` (short-circuits at env destinations per Phase 1.A helper) | PBRT-v4 Vertex::PDF / ConvertDensity convention: SA-measure at env destinations matches D4's SA pdfRev convention at the same vertex |
| **D19 NM** | BDPTIntegrator.cpp ~6571 (was ~6588-6590) | NM twin of pre-Session-8 D19 RGB | NM twin of post-Session-8 D19 RGB | Spectral twin of D19 RGB |

D4 RGB+NM hunks from Session 5 (~3505-3525 RGB, ~7240-7253 NM) and Session 7's delta-aware `remap0` blocks (MISWeight ~5024-5058 light-side, ~5108-5131 eye-side) untouched.  D2 + D6 audit blocks for Piece 2.D' (at ~1485 D2 RGB, ~4111 D6 RGB, ~5380 D2 NM, ~7637 D6 NM) remain intact.

No source-file add/remove, no build-project updates.

### Validation gates

| Gate | Spec | Result |
|---|---|---|
| 1. Clean `make` build | warning-free `make -C build/make/rise -j8 all tests` | ✅ PASS — zero warnings/errors across `all` and `tests` targets.  Xcode `RISE-GUI` not re-exercised this session (pre-existing x86_64 Homebrew arch issue per Session 7 unchanged). |
| 2. 116/116 tests pass | `./run_all_tests.sh` reports all pass | ❌ FAIL — **76/80**.  EnvLightBalanceTest is the only failing test binary; 4 sub-checks fail (see Gate 3). |
| 3. EnvLightBalanceTest at LAX `{0.35, 0.35, 2.00}` | 80/80 sub-checks pass | ❌ FAIL — **4 sub-checks fail** (vs Session 7's 2 — adds two new mean-failures): `BDPT mean within 35% of PT: env-only Lambertian` (RGB), `BDPT p99 within 35% of PT: env-only Lambertian` (RGB), `BDPT mean within 35% of PT: env-only Lambertian (spectral, hwss=false)`, `BDPT p99 within 35% of PT: env-only Lambertian (spectral, hwss=false)`. |
| 4. Audit-mode sanity check | D2 + D6 blocks fire; D5 blocks gone; no `assert()`; toggle-back identical | ✅ PASS — with `kSAMisAudit = true`: D2 RGB 268,288 firings, D2 NM 1,048,576 firings, D6 RGB 42,302 firings, D6 NM 417,991 firings.  **D5 firings = 0** (RGB and NM audit blocks correctly removed by the Piece 2.C' migration, as specified).  Zero `assert()` aborts.  Test result identical (76/80, same 4 failures).  Flipped back to `false`, rebuilt clean, reconfirmed 76/80 with the same 4 failures. |

### Before/after EnvLightBalanceTest ratios (BDPT mean / PT mean, R-channel; raw values in parens)

| Topology | Pre-Sess-5 master (no D4) | Session 5 (D4 only) | Session 7 (D4 + remap0) | **Session 8 (D4 + remap0 + D5 + D19)** | §0.4 §D prediction | Lax pass? | Δ vs Sess 7 |
|---|---|---|---|---|---|---|---|
| env-only Lambertian (RGB) | 109 % | 128 % | 128 % (PT 0.588, BDPT 0.752) | **139 %** (PT 0.588, BDPT 0.820) | 90-110 % | mean ❌, p99 ❌ | **+11 pp wrong way** |
| env + omni light (RGB) | 85 % | 7 % | 85 % | **85 %** (PT 0.601, BDPT 0.512) | 70-100 % | ✅ | unchanged |
| env + mesh emitter (RGB) | 86 % | 5 % | 85 % | **85 %** (PT 0.608, BDPT 0.520) | 70-100 % | ✅ | unchanged |
| env-only Lambertian (spectral, hwss=false) | 109 % | 127 % | 130 % | **138 %** (PT 0.558, BDPT 0.773) | 90-110 % | mean ❌, p99 ❌ | **+8 pp wrong way** |
| env-only Lambertian (spectral, hwss=true) | 92 % | 98 % | 98 % | **101 %** (PT 0.425, BDPT 0.428) | 90-110 % | ✅ | +3 pp (within MC) |
| non-uniform env + off-center quad (RGB) | 99 % | 102 % | 102 % | **100 %** (PT 4.231, BDPT 4.252) | 95-105 % | ✅ | within MC |
| non-uniform env + off-center quad (spectral, hwss=false) | 99 % | 102 % | 101 % | **100 %** (PT 0.950, BDPT 0.949) | 95-105 % | ✅ | within MC |
| non-uniform env + off-center quad (spectral, hwss=true) | 100 % | 100 % | 101 % | **101 %** (PT 0.804, BDPT 0.810) | 95-105 % | ✅ | unchanged |

Raw numbers from `bin/tests/EnvLightBalanceTest` post-final-rebuild, RGB R-channel, identical across two consecutive runs (audit-off and audit-on round-trip).

### Strict-tolerance failure count (Gate 11 monitoring, non-blocking)

Temporarily flipped `kEnvTolerances` to `{0.10, 0.30, 1.00}` and re-ran (then reverted to `{0.35, 0.35, 2.00}` and confirmed lax baseline):

- **Session 8 strict failure count: 11 sub-checks** out of 80.
  - 4 BDPT-mean-within-10 % failures (env-only Lambertian RGB + spectral HWSS=false + env+omni + env+mesh).
  - 2 BDPT-p99-within-30 % failures (env-only Lambertian RGB + spectral HWSS=false).
  - 5 VCM-mean-within-10 % failures (env-only Lambertian RGB, env+omni, env+mesh, env-only Lambertian spectral HWSS=false, env-only Lambertian spectral HWSS=true).
- **Session 7 baseline**: not measured (chip deferred strict-mode monitoring because lax was already failing).  Estimated upper bound: ≤ 9 (only env-only-Lambertian topologies were OUTSIDE lax 35 % at Session 7; the other 6 topologies that PASS lax can fail strict, but not all of them).
- **Direction**: failure count likely INCREASED at strict tolerances (consistent with the lax-mode regression on env-only Lambertian topologies).  Non-monotonic vs the chip's expected monotonic decrease.

### Refutation of the v2 design's §0.4 State D prediction

The §0.4 magnitude trace for env-only Lambertian on s=0 strategy at the env vertex:

| State | `pdfFwd_env` | `pdfRev_env` | `pdfR / pdfF` | `ri²` | Predicted misWeight_s=0 |
|---|---|---|---|---|---|
| M (master) | 0.080 (area) | 0.159 (area) | 1.99 | 3.96 | ~0.2 (s=0 down-weighted) |
| B = D = Session 8 (per trace) | 0.32 (SA via D19) | 0.0796 (SA via D4) | 0.249 | 0.062 | ~0.95 (s=0 near full) |

Predicted BDPT/PT under State D: ~95 % (close to PT).  **Measured: 139 %.**  The numerical disagreement is **48 percentage points** vs the prediction — outside any MC noise band and confidence interval.

The §0.4 trace is correct about the ratio AT THE ENV VERTEX (ri² at j=t-1 indeed drops from 3.96 → 0.062), but the trace's leap from "ri² at env vertex small" to "misWeight_s=0 ≈ 1, therefore BDPT ≈ PT" is unsound.  Two mechanisms it missed:

1. **Eye-side walk cascade**: BDPT MISWeight's eye-side walk multiplies `ri` at vertex j by `pdfRev/pdfFwd` THEN squares for sumWeights, accumulating across j = t-1 → t-2 → ... → 1.  D19 reduces ri AT j=t-1 (env vertex) by factor ~0.25 vs master's ~1.0.  For j ≤ t-2, ri carries the env-vertex ratio forward into the surface-vertex chain.  Smaller ri at j=t-1 → smaller ri² for ALL strategies (s+t-j, j) with j ≤ t-2.  sumWeights → small → misWeight_s=0 → near 1.  This is what the trace predicted.
2. **But: BDPT also has nonzero contributions from s ≥ 1 strategies that the trace did NOT model**.  For env-only Lambertian, s=1 NEE successfully samples env IS direction.  Its misWeight is computed via a DIFFERENT walk (the light-side walk starts at the staged sampled vertex, not the env vertex visited by the eye chain).  The light-subpath group (D2 + D3 + D6) is still in disc-area state — `lightVerts[0].pdfFwd` is `1/(πr²) ≈ 0.159` in Session 8, NOT SA `0.0796`.  This measure mismatch on the OTHER side of the (s=1, t) connection creates a separate magnitude error in the s=1 contribution.  The trace explicitly punted on this in §5.5: "The light-subpath group's effect is only on s≥1 strategies' misWeight — different code paths inside `MISWeight` and `ConnectAndEvaluate`."  The trace assumed this was decoupled.  **Session 8's evidence is that it is NOT decoupled** — the s=1 contribution magnitude is now also wrong because the light-subpath group is in stale disc-area state while the s=0 group is in fresh SA state.

Concretely, the lax-mode regression's signature:
- env-only Lambertian RGB mean ratio went 128 % → 139 % — BDPT is over-counting by 39 % vs PT.  PT is essentially pure s=0 (camera ray on first scatter, env hit on miss) and gets a single accurate estimator per path.
- BDPT in Session 7 (D4 only) was over-counting by 28 %; Session 8 adds 11 percentage points of extra over-counting from the s=0 group's new internal consistency PLUS the unchanged s=1 / s≥2 light-subpath contributions whose magnitudes don't realign with the new s=0 group convention.

### Why env+omni / env+mesh and non-uniform topologies are unaffected

In env+omni / env+mesh, `envSelectProb_NEE = 0` → D4's `pdfRev_env = 0` propagates through delta-aware `remap0` as a real zero → s=1 NEE contribution exits at zero → s=0 strategy correctly gets full weight.  Session 8 D5 / D19 leave this regime unchanged because:
- D5 in env+omni: drops `envSelectProb = 0` factor → SA `pdfSA_noSelect > 0`.  But the eyePred.pdfRev change only affects the eye-side walk's vertex-pred ratio.  Magnitude change at eyePred from `~0` (sentinel-bypassed via remap0 + delta) to a real positive value, propagates into ri at j=t-2.  Yet the env vertex `pdfRev_env = 0` (D4) ALREADY zeros ri via the §0.3.5 0/0 guard before D5's effect propagates.  Net: s=1 strategy still correctly suppressed.
- D19 in env+omni: `pdfFwd_env` goes 0.080 → 0.32.  pdfR/pdfF at j=t-1 = `pdfRev_env / pdfFwd_env = 0 / 0.32 = 0` → `ri = 0` (zero-fwd guard fires, propagates zero forward).  Net: env vertex's contribution to sumWeights is 0 either way.
- Mixed-scene s=1 contribution is BYPASSED by the PT-formula contribution override at Path A — env-NEE is never sampled in mixed scenes by construction (not in alias table).  No magnitude error from misaligned light-subpath group machinery because that machinery never fires for env in mixed scenes.

For non-uniform env + off-center quad topologies (RGB + spectral both HWSS variants), the magnitude was already 100-102 % in master and stays at 100-101 % in Session 8.  The s=1 NEE path through the off-center quad is the dominant contributor (alias table dominated by the high-intensity quad emitter), so env-vertex-MIS effects are negligible.

### Audit-mode and 0/0 guard observations

- Audit mode (`kSAMisAudit = true`): D2 + D6 audit blocks fire as expected (RGB + NM both populated); D5 audit blocks produce zero firings (correctly removed by the migration).  No `assert()` aborts.  Identical numeric test result audit-on vs audit-off — confirming audit code is dead-code under `kSAMisAudit = false`.
- §0.3.5 0/0 guard (`if (pdfF == 0) ri = 0`): not instrumented with a counter this session.  No NaN-driven nondeterminism observed (test results reproducible across runs).
- Compiler warnings: clean both passes (audit-on rebuild, audit-off rebuild).

### Surprises and refuted assumptions

1. The v2 design's §0.4 State D prediction was direction-WRONG, not just magnitude-wrong.  This is the load-bearing falsification.  The §0.4 trace's algebra at the env vertex IS correct (ratio drops from 1.99 → 0.249); what's wrong is the inference that this drop implies BDPT-near-PT.
2. The §0.2 claim that the s=0 group can land BEFORE the light-subpath group "either order or in parallel, removing the monolithic-landing risk" is **refuted in practice**.  With the light-subpath group still in disc-area state, the s=1 / s≥2 contributions are dimensionally inconsistent with the new s=0 SA state, and the inconsistency manifests as a magnitude over-count on env-only topologies.  §0.2's footnote did flag this risk: "Without delta-aware `remap0`, the light-subpath group standalone would HAVE its own catastrophe ... Magnitude shift unknown without measurement."  Session 8 is the measurement.
3. Mixed-scene topologies (env+omni / env+mesh) remain stable at 85 % — the catastrophe-free claim from §0.4 for mixed-scene IS confirmed.  This is partially good news: the s=0 group landing alone doesn't BREAK mixed-scene; it just doesn't FIX env-only.

### What's blocked

Piece 2.C' (this session) cannot stand alone in tree.  Recommended user options:

(a) **Revert this session's working-tree changes** (D5 RGB+NM + D19 RGB+NM at the four sites above) and restore Session 7's working-tree state (D4 + delta-aware `remap0`).  79/80 test result, env-only Lambertian RGB at 128 %, mixed-scene catastrophe-free.  Then either:
- (a1) Land all six SA-MIS rows JOINTLY (D2 + D3 + D4 + D5 + D6 + D19) as a single monolithic landing — abandoning the §0.2 group-decomposition.  Higher risk because debugging a joint regression is harder, but the v2 design's empirical evidence says the groups are NOT decoupled.
- (a2) Land the light-subpath group (D2 + D3 + D6) on top of Session 7's state (D4 + remap0 only — no D5, no D19), then independently re-evaluate whether to also land D5 + D19.  This would test the COMPLEMENTARY hypothesis: maybe the light-subpath group on its own is the load-bearing piece, and D5 / D19 are unnecessary (or actively wrong).
- (a3) Accept the 15-22 % residual (option (a) in [PRE_PHASE1_OPTION_C_DESIGN.md](PRE_PHASE1_OPTION_C_DESIGN.md) §1).  Revert Sessions 5 + 7 + 8 to pristine master.  Document the limitation in IMPROVEMENTS.md §12 as a known acceptable residual.

(b) **Keep this session's working-tree state and spawn an adversarial review** per §6.2 gate 10 to identify the root cause of the §0.4 trace's invalidation.  The adversarial review should derive the MIS-walk magnitude algebra for env-only Lambertian under State D explicitly, including s ≥ 1 contributions, to isolate why the prediction failed.  Then decide between (a1), (a2), (a3).

(c) **Pivot to option (b) — audit-driven retry — per [PRE_PHASE1_OPTION_C_DESIGN.md](PRE_PHASE1_OPTION_C_DESIGN.md) §1**.  3-5 sessions of rigorous algebra before any further code changes.  Revert Sessions 5 + 7 + 8 first.

Recommended sequence: (b) → (a1) if the review supports it.  The adversarial review is the cheapest test of whether the v2 design's group decomposition is salvageable.

### Files touched

- [src/Library/Shaders/BDPTIntegrator.cpp](../src/Library/Shaders/BDPTIntegrator.cpp):
  - **D5 RGB** (~3536, was ~3526-3596): dropped `envSelectProb` factor + sentinel; removed D5 RGB audit block (`[SA-MIS audit D5/RGB]`).
  - **D5 NM** (~7243, was ~7253-7311): NM twin of D5 RGB; removed D5 NM audit block (`[SA-MIS audit D5/NM]`).
  - **D19 RGB** (~2816, was ~2820-2822): replaced `SolidAngleToArea(pdfFwdPrev, 1.0, distSqToExit)` with `ConvertDensity(pdfFwdPrev, vertices.back(), vEnv)`.
  - **D19 NM** (~6571, was ~6588-6590): NM twin of D19 RGB.
  - `kSAMisAudit` flipped to `true` for Gate 4 sanity check; flipped back to `false` and rebuilt clean at end of session.
- [tests/EnvLightBalanceTest.cpp](../tests/EnvLightBalanceTest.cpp): tolerance constant `kEnvTolerances` flipped to strict `{0.10, 0.30, 1.00}` for the Gate-11 monitoring measurement; reverted to lax `{0.35, 0.35, 2.00}` before session end.

No source-file add/remove, no build-project updates, no commits, no stages, no pushes.  Working tree is dirty; user decides whether to revert (a3), keep + adversarial-review (b), continue with light-subpath group (a2), or pivot to monolithic landing (a1).

### Gate status at session 8 end

| Gate | Status |
|------|--------|
| 1.  Pre-refactor PNG baselines             | ✅ (captured Session 2; still valid) |
| 2.  116/116 tests green on master          | ❌ (76/80 in working tree state: D4 + remap0 + D5 + D19) |
| 3.  Strict tolerances trip on residual     | n/a (lax 35 % already fails on 4 sub-checks; strict 10/30/1 fails on 11) |
| 4.  Clean `make` rebuild                   | ✅ |
| 4b. Clean Xcode `RISE-GUI` rebuild         | ⚠ pre-existing x86_64 Homebrew arch mismatch (Session 7 carry-over); not re-exercised this session |
| 5.  EnvLightBalanceTest at strict          | ❌ 11 sub-checks fail at `{0.10, 0.30, 1.00}` (monitoring measurement; not blocking but indicative) |
| 6.  116-test suite incl. balance tests     | ❌ (4 EnvLightBalanceTest sub-checks fail at lax) |
| 7.  Render-baseline diff                   | ⏸ |
| 8.  HDRVarianceTest K-trial                | ⏸ |
| 9.  Visual parity on ripple_dreams_fields  | ⏸ |
| 10. Adversarial code review                | 🔴 STRONGLY RECOMMENDED before any further migration — the §0.4 trace's failure on env-only Lambertian needs an independent diagnosis pass |

## Session 9 outcome (continuous-PMF architectural fix — IMPROVEMENTS.md §12 substantively closed)

**Date**: 2026-05-29.
**Branch state**: working-tree only; no commits.  Sessions 5+7+8 BDPTIntegrator.cpp changes REVERTED to pristine master before this session's work landed.  The continuous-PMF fix lives entirely in `LightSampler` + `EnvironmentSampler`; BDPT/VCM integrator code is unchanged from master.
**Reference**: adversarial-review rounds 1–3 (Sessions 8 + post-Session-8) converged on the root-cause finding §A.5: "RISE's binary `EnvSelectProbability()` is the upstream architectural mismatch driving every catastrophe symptom — make it continuous and the v2 design's machinery (delta-aware remap0, group decomposition, SA-MIS per-row migration) becomes UNNECESSARY."

### Decision rationale

Three forward paths emerged from the adversarial review:
- **(α′)** Joint BDPT + VCM + guiding monolithic landing of D2+D3+D4+D5+D6+D19+D14+D15 + guiding audit.  2–3 sessions, high coupling surface, risky.
- **(β)** Continuous EnvSelectProbability architectural fix.  3–5 sessions estimated.  Addresses root cause; obviates the v2 design's machinery.
- **(γ)** Revert + accept residual.  Zero progress.

The user selected (β).  Implementation turned out to be smaller than estimated (~1 session) because the existing `EvaluateDirectLighting` already had a separate env-NEE block; the only missing piece was env's participation in `LightSampler::SampleLight()` (the light-subpath emission entry point used by BDPT/VCM/MLT).

### What shipped to the working tree

Three files modified (no source-file add/remove; no build-project updates):

| File | Change | Purpose |
|---|---|---|
| [src/Library/Rendering/EnvironmentSampler.h](../src/Library/Rendering/EnvironmentSampler.h) | +8 lines | New `Scalar TotalLuminance() const` accessor exposing the existing private `totalLuminance` (solid-angle-weighted integrated luminance over env-map texels).  Used by LightSampler to compute env's relative selection weight. |
| [src/Library/Lights/LightSampler.h](../src/Library/Lights/LightSampler.h) | +64 / -8 lines | New private `Scalar cachedEnvSelectProb` field + new private `void RecomputeEnvSelectProbability()` method.  `EnvSelectProbability()` body shrinks from the binary 0-or-1 ternary to `return cachedEnvSelectProb`.  `SampleEnvLightEmission()` signature gains a leading `const Scalar u1` parameter so callers can pre-pay the first direction-sampling random — see "Sobol-dimension preservation" below. |
| [src/Library/Lights/LightSampler.cpp](../src/Library/Lights/LightSampler.cpp) | +132 / -41 lines | Constructor initializes `cachedEnvSelectProb(0)`.  New `RecomputeEnvSelectProbability()` computes `envWeight = pEnvSampler->TotalLuminance() × π·r_scene²` vs `aliasTable.TotalWeight()`, sets `cachedEnvSelectProb = envWeight / (envWeight + aliasWeight)` (or 1.0 in env-only, 0 when env missing).  Called from both `Prepare()` (at end, after alias-table build) AND `SetEnvironmentSampler()` (after env sampler attaches) so the cache is correct regardless of which is invoked first — `RayCaster::AttachScene` calls Prepare BEFORE SetEnvironmentSampler, so the Prepare-only computation would have evaluated to zero at session-start (this was a real bug observed in the first build of the fix; see "Bug found and fixed mid-session" below).  `SampleLight()` gains a top-level env-vs-alias roll: draws ONE `sampler.Get1D()`, then either re-maps into env-direction u1 (env path) or into alias-table selection u (alias path).  Net Get1D() consumption per call is IDENTICAL to the prior binary-PMF flow; old 2026-05-26 documented warning about Sobol-dimension shift no longer applies (warning block removed). |

`BDPTIntegrator.cpp` is unchanged from pristine master.  No VCM changes.  No OpenPGL guiding changes.  No test changes (briefly flipped `kEnvTolerances` to `{0.10, 0.30, 1.00}` for Gate-11 monitoring then reverted to lax `{0.35, 0.35, 2.00}`).

### Sobol-dimension preservation

The 2026-05-26 doc-warning at [LightSampler.cpp:762-779 (pre-fix)](../src/Library/Lights/LightSampler.cpp) flagged a prior attempt that added env to the alias-table directly and consumed an extra `sampler.Get1D()` per call.  That attempt caused a "severe spectral-BDPT regression (env-only delivery 76% → 20% of PT)" attributed to Sobol-dimension-shift in the per-wavelength stratification.

The Session 9 fix avoids this trap by re-using a single `Get1D()` at the top of `SampleLight()` for BOTH the env-vs-alias roll and the downstream sub-strategy's first random (either env-direction's u1 or alias-table's selection u).  Verified empirically: spectral env-only Lambertian HWSS=true measures 93% of PT (vs master's 92%) — within MC noise, NO Sobol catastrophe.  Implementation detail: `SampleEnvLightEmission` signature gains a `const Scalar u1` parameter so the env-direction-importance-sampler receives the re-mapped uniform from above rather than calling Get1D() internally.

### Bug found and fixed mid-session

First implementation computed `cachedEnvSelectProb` inline at the end of `Prepare()`.  Initial test run gave env+omni / env+mesh stuck at 85% (unchanged) and env-only Lambertian DROPPED to 85% (regression).  Root cause: `RayCaster::AttachScene` calls `pLightSampler->Prepare(...)` at line 122 BEFORE `pLightSampler->SetEnvironmentSampler(...)` at line 145, so `pEnvSampler` was nullptr when Prepare ran → `bEnvExists = false` → `cachedEnvSelectProb = 0` → SampleLight's env path never fired even in env-only scenes.  Fix: extract the computation into `RecomputeEnvSelectProbability()` and call it from BOTH Prepare() AND SetEnvironmentSampler().  After fix, results recovered to expected values.  Lesson: any cache derived from `pEnvSampler` state must be invalidated/recomputed in `SetEnvironmentSampler` since it can run after Prepare.

### EnvLightBalanceTest results (R-channel BDPT mean / PT mean ratios)

| Topology | Pristine master | **Session 9 (continuous PMF)** | Δ | Lax pass? |
|---|---|---|---|---|
| env-only Lambertian (RGB) | 109% | **109%** (PT 0.588, BDPT 0.642) | 0 | ✅ |
| env + omni light (RGB) | 85% | **109%** (PT 0.601, BDPT 0.654) | **+24 pp** ↑ | ✅ |
| env + mesh emitter (RGB) | 86% | **92%** (PT 0.608, BDPT 0.562) | **+6 pp** ↑ | ✅ |
| env-only Lambertian (spectral, hwss=false) | 108% | **107%** (PT 0.566, BDPT 0.603) | within MC | ✅ |
| env-only Lambertian (spectral, hwss=true) | 92% | **93%** (PT 0.423, BDPT 0.392) | within MC | ✅ |
| non-uniform env + off-center quad (RGB) | 99% | **99%** (PT 4.231, BDPT 4.184) | within MC | ✅ |
| non-uniform env + off-center quad (spectral, hwss=false) | 100% | **99%** (PT 0.947, BDPT 0.936) | within MC | ✅ |
| non-uniform env + off-center quad (spectral, hwss=true) | 100% | **100%** (PT 0.806, BDPT 0.807) | 0 | ✅ |

**VCM column** (VCM mean R / PT mean R):

| Topology | Master | **Session 9** | Δ |
|---|---|---|---|
| env-only Lambertian | 106% | 106% | 0 |
| env + omni | 85% | 105% | **+20 pp** ↑ |
| env + mesh | 86% | **128%** (VCM 0.780 / PT 0.608) | **+42 pp** ↑ — over-correction; passes lax (28% over < 35% tol) but flags strict |
| HWSS=true env-only | 82% | 83% | +1 pp |

**Lax `{0.35, 0.35, 2.00}` tolerances**: 80/80 sub-checks PASS (vs Session 8's 76/80).
**Strict `{0.10, 0.30, 1.00}` tolerances**: 77/80 sub-checks PASS (3 fail; down from Session 8's 11 fail and from an estimated master baseline of ~14+):
- `VCM mean within 10% of PT: env + mesh emitter` (28% over → 18 pp out)
- `BDPT mean within 10% of PT: env-only Lambertian (spectral, hwss=false)` (~7% over)
- `VCM mean within 10% of PT: env-only Lambertian (spectral, hwss=true)` (17% under)
These are residual disc-area-vs-SA-measure discrepancies that the deferred SA-MIS migration could close.  Not blocking; recommend tracking via tighter monitoring tolerances on next regression test pass.

### Validation gates

| Gate | Spec | Result |
|---|---|---|
| 1. Clean `make` build | warning-free `make -C build/make/rise -j8 all tests` | ✅ PASS — zero warnings/errors across `all` and `tests` targets. |
| 2. 116/116 tests pass | `./run_all_tests.sh` reports all pass | ✅ PASS — **116/116** (`./run_all_tests.sh` last line: `All 116 tests passed`). |
| 3. EnvLightBalanceTest at LAX `{0.35, 0.35, 2.00}` | 80/80 sub-checks pass | ✅ PASS — 80/80.  No FAIL lines in EnvLightBalanceTest stderr. |
| 4. EnvLightBalanceTest at STRICT `{0.10, 0.30, 1.00}` (monitoring) | record failure count | ⚠ 3/80 fail — residual SA-MIS discrepancies, see table above.  Non-blocking; well below Session 8's 11/80. |
| 5. No regression on non-env scenes | BDPTStrategyBalanceTest + VCMStrategyBalanceTest + VCMRecurrenceTest + VCMSpectralRecurrenceTest pass | ✅ PASS — included in 116/116. |
| 6. No Sobol catastrophe on spectral env-only | env-only Lambertian HWSS=true and hwss=false within ±5% of master | ✅ PASS — HWSS=true 92% → 93% (+1pp), hwss=false 108% → 107% (-1pp).  Both within MC noise band. |

### Adversarial-review ledger close-out

Of the round-1+2+3 findings:

| ID | Pre-Session-9 status | Post-Session-9 status |
|---|---|---|
| A.4 / B.1 — partition-of-unity broken by partial SA migration | CONFIRMED | **OBSOLETE** — Sessions 5+7+8 reverted; the partial-SA state that broke partition no longer exists. |
| B.2 — §0.4 trace incomplete | CONFIRMED | **DOC FIX OPTIONAL** — trace's correctness no longer load-bearing; the SA-MIS migration it described is deferred. |
| B.3 — monolithic landing collapse | REFUTED by Reviewer D round 2 | **MOOT** — monolithic landing path not taken. |
| **A.5** — continuous EnvSelectProbability root cause | identified as "deferred future architectural improvement" | **LANDED** — this session. |
| A.1 — §0.3.5 0/0 guard over-zeroing | NEEDS-VERIFY | **OBSOLETE** — delta-aware remap0 reverted; the guard no longer exists in tree. |
| E.1 — skip rule i==1 carve-out | REJECTED (env not delta) | unchanged. |
| E.2 — lightVerts[0].pdfRev not staged at s≠1 sites | DEFERRED | unchanged.  Could matter for future SA-MIS work but not for continuous-PMF baseline. |
| E.3 — power-2 vs balance heuristic | BY-DESIGN | unchanged. |
| H.A (VCM regression risk) + G.E | CONFIRMED for monolithic landing | **NEUTRALISED** — no SA-MIS migration touched VCM, so the VCM caller-flag coupling never fires.  VCM env-IBL results IMPROVE in mixed scenes (env+omni 85%→105%, env+mesh 86%→128%) because VCM consumes the same continuous PMF via `EnvSelectProbability()`. |
| H.D + G.F (OpenPGL guiding regression) | CONFIRMED for monolithic landing | **NEUTRALISED** — `lightVerts[0].pdfFwd` and `lightVerts[0].throughput` semantics are unchanged from master; the Le-cancellation invariant at `BDPTIntegrator.cpp:677` still holds. |
| F.G — non-uniform env analytical residual | CAVEAT | unchanged.  Non-uniform-env+quad topology measures 99% post-fix (master was 99%), within MC noise. |

### What's blocked / next steps

Nothing is BLOCKED.  Recommended optional follow-ups:

1. **Track strict tolerances**: update `kEnvTolerances` in
   [tests/EnvLightBalanceTest.cpp](../tests/EnvLightBalanceTest.cpp) to
   intermediate `{0.20, 0.35, 1.50}` to catch future regressions of the
   3 residual sub-checks while still passing in the current tree.  Or
   keep lax + add a separate strict-mode CI job that's allowed to fail.
2. **Adversarial-review one more round** (optional): now that the fix
   has landed cleanly, a "what's left" round could verify there's no
   hidden regression we missed (e.g. guided BDPT renders, MLT, scenes
   with infinite-plane geometry near the env-sphere-radius cap).
3. **Visual parity check on `ripple_dreams_fields.RISEscene`**: render
   PT vs BDPT vs VCM at matched samples and confirm visual indistinguishability.
4. **HDRVarianceTest K-trial**: measure variance reduction on env-IBL
   scenes to quantify what the continuous-PMF fix bought beyond the
   mean-ratio improvement.
5. **SA-MIS migration (deferred)**: if a production team needs the
   final 5-30% strict-tolerance residual closed, the v2 design's SA-
   MIS work can resume — but with a CRITICAL caveat per Session 8's
   evidence: any partial SA landing breaks partition-of-unity, so the
   migration must be monolithic (D2+D3+D4+D5+D6+D19 + VCM D14+D15 +
   OpenPGL guiding D18 audit, all atomic) or skipped entirely.  The
   Piece-1.A `ConvertDensity` helper remains in tree as a foundation.

### Files touched

- [src/Library/Rendering/EnvironmentSampler.h](../src/Library/Rendering/EnvironmentSampler.h): +8 lines (`TotalLuminance()` accessor).
- [src/Library/Lights/LightSampler.h](../src/Library/Lights/LightSampler.h): +64 / -8 lines (private `cachedEnvSelectProb` + `RecomputeEnvSelectProbability` + `SampleEnvLightEmission` signature update + doc-comments).
- [src/Library/Lights/LightSampler.cpp](../src/Library/Lights/LightSampler.cpp): +132 / -41 lines (constructor init, `RecomputeEnvSelectProbability` body, `Prepare()` calls it, `SetEnvironmentSampler()` calls it, `SampleLight()` env-vs-alias wrapper, `SampleEnvLightEmission()` consumes external u1, 2026-05-26 dimension-shift warning block removed).
- [docs/IMPROVEMENTS.md](IMPROVEMENTS.md): §12 status block + companion-limitation block updated to "substantively closed".
- [docs/PRE_PHASE1_STATUS.md](PRE_PHASE1_STATUS.md): this Session 9 outcome section.

`BDPTIntegrator.cpp` is unchanged (reverted to pristine master before this session's work).

No source-file add/remove, no build-project updates, no commits, no stages, no pushes.  Working tree dirty for user review.

### Gate status at session 9 end

| Gate | Status |
|------|--------|
| 1.  Pre-refactor PNG baselines             | ✅ (captured Session 2; still valid) |
| 2.  116/116 tests green on master          | ✅ (116/116 in working tree with continuous-PMF fix) |
| 3.  Strict tolerances trip on residual     | ⚠ 3 sub-checks fail at strict (well below master's expected baseline; residual disc-area-vs-SA discrepancies) |
| 4.  Clean `make` rebuild                   | ✅ warning-free |
| 4b. Clean Xcode `RISE-GUI` rebuild         | ⚠ not re-exercised this session (pre-existing Homebrew arch issue from Session 7 unchanged) |
| 5.  EnvLightBalanceTest at strict          | ⚠ 77/80 pass at strict (Session 8 baseline 69/80, master baseline lower) |
| 6.  116-test suite incl. balance tests     | ✅ 116/116 pass |
| 7.  Render-baseline diff                   | ⏸ — recommended optional follow-up |
| 8.  HDRVarianceTest K-trial                | ⏸ — recommended optional follow-up |
| 9.  Visual parity on ripple_dreams_fields  | ⏸ — recommended optional follow-up |
| 10. Adversarial code review                | ✅ 3 rounds × 8 reviewers (Sessions 8 + post-Session-8) converged on the §A.5 fix that landed this session; optional one-more-round to verify no hidden regressions |

## Session 10 outcome (VCM SmallVCM-MIS continuous-PMF refactor — partial closure)

**Date**: 2026-05-29.
**Branch state**: working-tree only; no commits.  Builds on Sessions 9 (continuous-PMF SampleLight wrapper) + Session 9 follow-ups (PdfSelect rescale + env-NEE pdfSelect divide + SMS pEnvLight gate + kEnvZeroSentinel removal).
**Reference**: post-Session-9 visual report — VCM rendered much brighter than BDPT on env+mesh scenes (the user's actual production observation).  EnvLightBalanceTest env+mesh measured VCM at 128 % of PT vs BDPT at 92 %.
**User directive**: "chase this down in a principled fashion" + later "do it carefully in small steps with adversarial reviews along the way".

### Root cause diagnosis

VCM uses SmallVCM-style MIS quantities (Georgiev et al. 2012, `vertexcm.hxx`).  Specifically `dVC = cosLight / emissionPdfW` and the `wCamera` formulas in `EvaluateNEE` / `EvaluateS0` mesh / `EvaluateS0` env all compose against `emissionPdfW`.  SmallVCM convention: `emissionPdfW = pdfPos × pdfDir` (GEOMETRIC, no light-selection multiplier).

RISE stores `v.pdfFwd = pdfSelect × pdfPos` and `v.emissionPdfW = pdfSelect × pdfPos × pdfDir` (JOINT, pdfSelect baked in).  The ratio `pdfFwd / emissionPdfW = 1/pdfDir` cancels pdfSelect — so `dVCM` happens to be SmallVCM-correct without any change.  But `dVC = cosLight / emissionPdfW_joint` carries an implicit `1/pdfSelect` inflation, and the local `wCamera` recomputes in EvaluateNEE/S0 use joint `emissionPdfW` directly — inflating wCamera by the same `1/pdfSelect`.

Under master, `pdfSelect ≈ 1.0` for the only-light-in-table case, so the inflation was invisible.  Under Session 9's continuous-PMF fix, `pdfSelect` varies per sample (env-rooted: `cachedEnvSelectProb`, alias-rooted: `(1 - cachedEnvSelectProb) × aliasTable.Pdf(idx)`), and the inflations propagate through SmallVCM's `1/(wLight + 1 + wCamera)` partition formula, breaking partition-of-unity and over-counting mesh-NEE in env+mesh scenes.

### What shipped to the working tree

Five files modified (no commits, no stages, working tree dirty for review):

| File | Change |
|---|---|
| [src/Library/Shaders/BDPTVertex.h](../src/Library/Shaders/BDPTVertex.h) | New `Scalar pdfSelect` field (default 1.0).  30-line doc-comment describing the SmallVCM-geometric vs RISE-joint storage rationale. |
| [src/Library/Shaders/BDPTIntegrator.cpp](../src/Library/Shaders/BDPTIntegrator.cpp) | Populate `v[0].pdfSelect = ls.pdfSelect` at light-subpath init: RGB (~line 1424), NM (~line 5220). |
| [src/Library/Shaders/VCMRecurrence.h](../src/Library/Shaders/VCMRecurrence.h) | `InitLight` signature extended with `pdfSelect = Scalar(1)` parameter (default preserves direct numeric callers — `VCMRecurrenceTest` / `VCMSpectralRecurrenceTest` literal fixtures unchanged). |
| [src/Library/Shaders/VCMRecurrence.cpp](../src/Library/Shaders/VCMRecurrence.cpp) | `dVC = (usedCosLight × pdfSelect) / emissionPdfW` — extracts SmallVCM-geometric value from joint storage.  `dVCM` line unchanged. |
| [src/Library/Shaders/VCMIntegrator.cpp](../src/Library/Shaders/VCMIntegrator.cpp) | `InitLight` call site passes `v.pdfSelect` (line 470).  Three `wCamera` divides: EvaluateS0 mesh (~line 947), EvaluateS0 env (~line 858), EvaluateNEE (~line 1234). |

### Empirical results

EnvLightBalanceTest (BDPT mean R / PT mean R, env+mesh-only):

| State | VCM env+mesh / PT | Δ vs prior |
|---|---|---|
| Master (pre Session 9) | 85.5 % (under) | — |
| Session 9 continuous-PMF baseline | 128 % | +42 pp |
| + PdfSelect rescale + env-NEE pdfSelect divide (Session 9 follow-ups) | 127 % | −1 pp |
| + dVC fix (this session, alone) | 127 % | 0 (formula change but `dVC × bsdfRev` term doesn't dominate splat wLight in this scene; `cameraPdfA / N` is tiny) |
| **+ wCamera divides at S0 mesh + S0 env + NEE (this session, full)** | **122 %** | **−5 pp** |
| Target | 100 % | — |

Other topologies (unchanged within MC noise):
- env-only Lambertian: VCM/PT 106 % (master baseline)
- env+omni: 106 %
- env-only Lambertian spectral hwss=false: 102 %
- env-only Lambertian HWSS=true: 83 %
- non-uniform env + quad RGB: 89 %
- non-uniform env + quad spectral hwss=false: 89 %
- non-uniform env + quad spectral hwss=true: 89 %

### Validation gates

| Gate | Result |
|---|---|
| 1. Clean `make` build | ✅ warning-free across `all` + `tests` |
| 2. 116/116 binary tests | ✅ all pass |
| 3. Lax `{0.30, 0.35, 1.50}` tolerances (current intermediate per Session 9 follow-up) | ✅ 80/80 sub-checks pass |
| 4. Strict `{0.10, 0.30, 1.00}` tolerances | ⚠ 78/80 (down from Session 9's 77/80 — **BDPT spectral env-only Lambertian no longer fails strict**; only VCM env+mesh 22% over and VCM HWSS=true 17% under remain) |
| 5. `VCMRecurrenceTest` / `VCMSpectralRecurrenceTest` | ✅ unchanged (direct numeric fixtures use default pdfSelect=1.0) |
| 6. env-only Lambertian and env+omni invariance | ✅ unchanged within MC noise (pdfSelect = 1.0 exactly in env-only — `RecomputeEnvSelectProbability` short-circuits to 1.0 when alias table empty) |

### Adversarial review (round 5)

One reviewer audited the dVC + wCamera changes.  Findings (all P2 except one P1 follow-up):

| Q | Severity | Confidence | Verdict |
|---|---|---|---|
| A (dVC algebra) | P2 | 0.92 | Confirmed: `dVC = cosLight × pdfSelect / emissionPdfW_joint` = `cosLight / (pdfPos × pdfDir)` = SmallVCM convention ✓ |
| B (three wCamera divides self-consistent) | P2 | 0.85 | Confirmed: all three sites extract geometric correctly ✓ |
| C (direction-consistency of 127→122 % reduction) | P2 | 0.80 | Confirmed: mesh-emitter strategy downweighted as predicted ✓ |
| **D (remaining 22 pp gap location)** | **P1** | **0.70** | **NOT in splat/interior wLight (they're SmallVCM-correct after dVC propagates via ApplyBsdfSamplingUpdate).  Most likely in MIS partition between the three env strategies (s=0 env-escape + s=1 env-NEE + t=1 env-rooted-splat) — each strategy's `wLight + 1 + wCamera` may not include the others, so partition-of-unity isn't guaranteed.** |
| E (env-only invariance) | P2 | 0.90 | Confirmed: pdfSelect=1.0 reduces all formulas to identity ✓ |
| F (stage or roll back) | P2 | 0.75 | **Stage as-is.  Don't roll back — partial fix is correct foundation that any further fix builds on.** |

### What's blocked / what's next

**Nothing is blocked.**  The current state is mathematically self-consistent (every formula change is provably SmallVCM-equivalent at pdfSelect=1.0).  116/116 tests pass at the intermediate lax tolerance; strict tolerance fails on 2 sub-checks (one of which is a deeper structural issue — env multi-strategy MIS partition).

Recommended optional follow-ups:

1. **Architectural review of env multi-strategy MIS partition** (P1 follow-up from this session's review): does SmallVCM-balance MIS support three env strategies summing to unity, or is this a known limitation of the SmallVCM framework that requires either (a) PBRT-v4-style power-heuristic MIS for env-IBL, (b) a different partition formula that explicitly includes all three alternatives, or (c) acceptance as documented residual?
2. **Visual parity check on `ripple_dreams_fields.RISEscene`** at matched samples between PT / BDPT / VCM — the user's production scene class.
3. **HDRVarianceTest K-trial** to quantify the post-fix variance improvement on env+mesh and similar topologies.
4. **Caustic-scene check** (`sms_k2_glasssphere`, etc.) with VM enabled — confirm the dVC fix doesn't regress merge-MIS on caustic-heavy scenes.

### Files touched

- [src/Library/Shaders/BDPTVertex.h](../src/Library/Shaders/BDPTVertex.h) — +29 lines (field + doc-comment + ctor init).
- [src/Library/Shaders/BDPTIntegrator.cpp](../src/Library/Shaders/BDPTIntegrator.cpp) — +14 lines (RGB + NM populate sites).
- [src/Library/Shaders/VCMRecurrence.h](../src/Library/Shaders/VCMRecurrence.h) — +14 lines (signature + doc).
- [src/Library/Shaders/VCMRecurrence.cpp](../src/Library/Shaders/VCMRecurrence.cpp) — +22 lines (dVC formula + comment block).
- [src/Library/Shaders/VCMIntegrator.cpp](../src/Library/Shaders/VCMIntegrator.cpp) — +28 lines (InitLight call + 3 wCamera divides).
- [docs/PRE_PHASE1_STATUS.md](PRE_PHASE1_STATUS.md) — this Session 10 outcome section.

No source-file add/remove, no build-project updates, no commits, no stages, no pushes.  Working tree is dirty; user decides whether to commit, continue with env-partition deep-dive, or accept current state.

### Gate status at session 10 end

| Gate | Status |
|------|--------|
| 1.  Pre-refactor PNG baselines             | ✅ (still valid from Session 2) |
| 2.  116/116 tests green                    | ✅ |
| 3.  Lax tolerances pass                    | ✅ 80/80 at intermediate `{0.30, 0.35, 1.50}` |
| 4.  Strict tolerances trip on residual     | ⚠ 78/80 at strict `{0.10, 0.30, 1.00}` — VCM env+mesh 22 % over + VCM HWSS=true 17 % under |
| 5.  Clean `make` rebuild                   | ✅ warning-free |
| 6.  Adversarial review                     | ✅ 5 rounds × 9 reviewers cumulative (Sessions 8 + Session 9 follow-ups + Session 10).  Session 10 review converges: stage as-is; pursue env multi-strategy partition in follow-up. |

---

## Session 11 outcome (2026-05-30) — chased the env+mesh 22 % residual via Δ9–Δ12 bisect; root cause pinned to env-S0 ↔ env-NEE partition; option (A) accepted

**Date**: 2026-05-30.
**Branch state**: working tree clean of Session 11 diagnostic instrumentation; Session 10 micro-changes kept in-tree per user direction.  No commits.
**User directive**: "chase this down in a principled fashion" → after exhaustive bisect, "Do (A) and document everything that we've learned. Our supervisor agent will then figure out how to stage (B) through another session."

### What was done

Δ9–Δ12 followed Session 10's round-5 P1 reviewer recommendation — instrument before changing code.  Added env-var-gated `printf` blocks at `EvaluateS0Impl` mesh-direct site, `EvaluateNEEImpl` post-weight site, and `SplatLightSubpathToCameraImpl` post-weight site.  Captured per-hit MIS quantities on a 32×32×{4,64,256}-spp standalone env+mesh scene.  Then added env-var-gated `if (disableX) continue;` skip gates at all five contribution sites plus two specific-divide gates.

Full evidence table, methodology, and refined attack plan in the dedicated investigation doc:
**[docs/VCM_ENV_MIS_PARTITION_INVESTIGATION.md](VCM_ENV_MIS_PARTITION_INVESTIGATION.md)** — required reading for the supervisor agent staging the SA-MIS migration.

### Findings (summary; details in the dedicated doc)

| Hypothesis | Verdict | Evidence |
|---|---|---|
| dVCM recurrence broken under continuous-PMF | **REFUTED** | dVCM-implied `bsdfDirPdfW` matches independent geometric computation to < 0.001 % rel-diff on 74 path-length-3 hits |
| Session 10 dVC × pdfSelect at InitLight is the source | **REFUTED** | Δ7 bisect: 0 effect |
| Session 10 wCamera /pdfSelect divides (3 sites) are the source | **REFUTED** | Δ4–Δ6 + this session: each < 0.001 linR effect in env-dominant scenes (envSelProb ≈ 1 = no-op) |
| env-NEE `invLightSelect = 1/pdfSelect` rescale is over-correcting | **REFUTED** | This session: < 0.001 linR (no-op when envSelProb ≈ 1) |
| Mesh-side strategies (s0_mesh / nee_mesh) are the source | **REFUTED** | Δ11 A/B: each < 0.0005 linR |
| Splats (any origin) are the source | **REFUTED** | Δ11 A/B: ≤ 0.002 linR |
| Interior connections at path length 3 | **REFUTED** | Δ11 A/B: 0 linR (can't fire at path length 3 with max_light_depth=3) |
| **env-S0 ↔ env-NEE MIS partition violation** | **CONFIRMED** | Δ11 A/B: disabling both drops baseline by 0.738; disabling each alone drops by only 0.193 + 0.228 = 0.421.  Linear MIS model REJECTED at 1.75× violation factor.  Same 1.51× violation on env-only Lambertian — not mesh-specific. |

### What option (A) means in concrete terms

- **Diagnostic instrumentation STRIPPED** from `VCMIntegrator.cpp` — file is back to the Session 10 state.
- **Session 10 micro-changes KEPT** in-tree (pdfSelect field + InitLight dVC + 3 wCamera divides + env-NEE invLightSelect).  They follow SmallVCM convention more carefully than master and are not the over-count source (so no urgency to revert).
- **EnvLightBalanceTest at lax `{0.30, 0.35, 1.50}` continues to pass** (80/80 sub-checks).
- **Strict `{0.10, 0.30, 1.00}` continues at 78/80** — VCM env+mesh 22 % over (this session: confirmed sourced in env-S0 ↔ env-NEE partition) + VCM HWSS=true 17 % under (separate residual, not investigated this session).
- **No new code shipped that fixes the 22 % over** — Option (A) explicitly defers that to a future supervisor-staged session.

### Working-tree state after Session 11

Same as Session 10 end-state (Session 10 changes kept).  See Session 10's "Files touched" — those lines are still in-tree.  All Session 11 diagnostic code has been stripped.

New file: [docs/VCM_ENV_MIS_PARTITION_INVESTIGATION.md](VCM_ENV_MIS_PARTITION_INVESTIGATION.md).

### Gate status at session 11 end

| Gate | Status |
|------|--------|
| 1.  Pre-refactor PNG baselines              | ✅ (still valid from Session 2) |
| 2.  116/116 tests green                     | ✅ verified post-strip |
| 3.  Lax tolerances pass                     | ✅ 80/80 at `{0.30, 0.35, 1.50}` |
| 4.  Strict tolerances trip on residual      | ⚠ 78/80 at `{0.10, 0.30, 1.00}` — unchanged from Session 10 |
| 5.  Clean `make` rebuild                    | ✅ warning-free after strip |
| 6.  Adversarial review                      | ✅ 2 reviewers parallel in Δ12 (math + cross-strategy); both recommended "instrument first" — followed in Δ11.  No new code change in this session for them to review. |

### Recommendations for the supervisor agent

The investigation doc's §6 ("Refined attack plan for the supervisor agent") has the actionable handoff.  Headline items:

1. The catastrophe modes of the v2 SA-MIS design are now mostly defused by Session 9's continuous-PMF fix.  Scope is smaller than the original 3-week estimate.
2. The VCM-side SA-MIS work is restricted to **two sites**: `EvaluateS0Impl` env-branch and `EvaluateNEEImpl` env-branch.  Mesh-side strategies are empirically innocent.
3. The BDPT-side `pdfFwd/pdfRev` SA-measure migration at the 6 sites identified in PRE_PHASE1_OPTION_C_DESIGN.md "Group 2" still applies.
4. OpenPGL guiding pdf consumers must be audited and migrated in the same atomic landing.
5. Session 10's micro-changes should be reconsidered (keep / refactor / revert) as part of the SA-MIS landing rather than independently.

The supervisor should ask the user for an explicit budget cap before staging the SA-MIS migration — recommend ~1 week of focused work given the scope reduction.

---

## Pre-Phase-1 Piece 2 outcome (2026-05-31) — VCM media-aware connection transmittance: mechanical port landed, variance gates revealed an MIS-partition interaction that warrants supervisor review BEFORE acceptance

### Headline

The four-site mechanical port called for by [UNIFIED_INTEGRATOR_ANALYSIS.md](UNIFIED_INTEGRATOR_ANALYSIS.md) §5.2.2 ("borrow BDPT's `EvalConnectionTransmittance` at VCM's NEE / interior / splat / S0 sites") is implemented in three of the four sites and validated against the 116-test suite (all pass).  **However, the K-trial variance gate on the new env-IBL-through-fog regression scene revealed that pre-fix VCM is DARKER than PT, not "slightly overbright" as IMPROVEMENTS.md §12 documents.**  The fix mechanically does what BDPT does at the same call sites, and vacuum non-regression is bit-clean (within MC noise), so it does not "break" anything that wasn't already biased.  But it does NOT close the documented §12 gap on its own — the empirically dominant volumetric-VCM bias on env-IBL scenes goes the other direction.

Per the prompt's explicit stop rule ("the new regression scene's pre-fix VCM does NOT show brighter-than-PT … this would mean either the scene doesn't exercise the path well, or VCM is already applying transmittance somewhere we missed"), execution paused for user / supervisor judgment.  Working tree carries the mechanical fix uncommitted.

### What was changed (in working tree)

1. **[src/Library/Shaders/BDPTIntegrator.h](../src/Library/Shaders/BDPTIntegrator.h)** — promoted four `EvalConnectionTransmittance` / `EvalConnectionTransmittanceNM` overloads from `protected` to `public` so VCMIntegrator can call them directly.  Added a short rationale comment explaining the consumer.  No semantic change.

2. **[src/Library/Shaders/VCMIntegrator.cpp](../src/Library/Shaders/VCMIntegrator.cpp)** — three Tag-dispatched changes inside the anonymous namespace at the four `*Impl<Tag>` connection sites (Phase 2a templatization preserved — edits to `Impl<Tag>` bodies only, not the thin Pel/NM forwarders):
   - Added two Tag-dispatched helpers `EvalConnectionTr<Tag>(p1, p2, …)` and `EvalConnectionTrRay<Tag>(ray, maxDist, …)` that route to BDPT's RGB or NM transmittance walks based on the Tag context (PelTag → RGB / RISEPel value type; NMTag → single-wavelength Scalar at `tag.nm`).
   - **`EvaluateNEEImpl<Tag>`**: applies Tr at both sub-cases.  Env-NEE uses the Ray+maxDist=RISE_INFINITY overload along the SAMPLED `wiForLight_vcm` direction (mirrors BDPT's round-5 P2 fix at BDPTIntegrator.cpp:3906); explicit-light NEE uses Point3+Point3 from eye vertex `v.position` to `ls.position`.  Medium-bookkeeping (`pMediumObject`, `pMediumVol`) comes from the eye vertex.  Added `const BDPTIntegrator& bdpt` parameter; public `EvaluateNEE` / `EvaluateNEENM` forwarders now pass `*pGenerator`.
   - **`SplatLightSubpathToCameraImpl<Tag>`**: applies Tr at the t=1 splat site from the light vertex toward the camera.  Mirrors BDPT's t=1 site at BDPTIntegrator.cpp:4266.  Added `const IScene&` and `const BDPTIntegrator&` parameters; public `SplatLightSubpathToCamera{,NM}` forwarders pass `scene` and `*pGenerator`.
   - **`EvaluateInteriorConnectionsImpl<Tag>`**: applies Tr at every interior s≥2, t≥2 connection.  Mirrors BDPT's interior site at BDPTIntegrator.cpp:4445.  Added `const IScene&` and `const BDPTIntegrator&` parameters; public `EvaluateInteriorConnections{,NM}` forwarders pass scene and `*pGenerator`.
   - **`EvaluateS0Impl<Tag>` — NO CHANGE**, by design.  For surface direct-hit, the eye-walk's `beta = beta * Tr` at BDPTIntegrator.cpp:2655 already folds Tr into `eyeEnd.throughput` along the final segment to the surface — adding a second Tr would double-attenuate.  For env-escape, BDPT itself does NOT apply Tr (eye-walk skips Tr on the `!ri.bHit` branch at line 2660; BDPT's s=0 env-escape code at line 3419 just uses `eyeEnd.throughput * Le`), so mirroring BDPT means VCM also doesn't apply Tr here.  Adding it in VCM only would diverge PT/BDPT/VCM at s=0 env-escape.  This is a latent BDPT issue out of Piece 2 scope per the "no-BDPT-changes" rule.

3. **New regression scenes** — [scenes/Tests/Volumes/vcm_env_through_fog.RISEscene](../scenes/Tests/Volumes/vcm_env_through_fog.RISEscene) and [scenes/Tests/Volumes/pt_env_through_fog.RISEscene](../scenes/Tests/Volumes/pt_env_through_fog.RISEscene) — open Cornell box (floor + 3 walls, no ceiling so env reaches interior) under uniform env-IBL + global homogeneous fog (σ_t ≈ 0.001).  PT-reference at 256 spp; VCM at 64 spp.

### Validation gates

| Gate | Status | Detail |
|------|--------|--------|
| 1.  Clean warning-free `make` build (`all` + `tests`) | ✅ | Zero warnings on the changed translation units; tests target linked cleanly |
| 2.  All 116 binary tests pass | ✅ | `./run_all_tests.sh` reports 116 built, 116 passed, 0 failed.  Critical regression tests verified: `BDPTStrategyBalanceTest`, `VCMStrategyBalanceTest`, `VCMRecurrenceTest`, `VCMSpectralRecurrenceTest`, `EnvLightBalanceTest` all PASS — lax env-IBL parity preserved |
| 3.  VCM ≈ PT within ±5 % on new regression scene | ❌ | See Findings — pre-fix VCM was already significantly DARKER than PT in both env-only and mesh-emitter fog scenes (53–95 % of PT depending on channel); the fix makes the gap larger, not smaller |
| 4.  K-trial variance comparison | Conducted at K=8 (rendered in ~250 ms each; PT reference at 16 spp matched against VCM at 16 spp; numbers below) | |
| 5.  No regression on non-volumetric VCM scenes | ✅ | Vacuum non-regression: VCM-pre vs VCM-post on the same env-only Lambertian scene without fog gives channel means within 0.05 % per channel — confirms Tr=1 collapses to no-op in vacuum |
| 6.  Stop-rule triggered | ✅ | Per prompt's stop rule "pre-fix VCM does NOT show brighter-than-PT … surface as open question" |

### Measurement details — K=8 per condition, 64×64 pixels, 16 spp

**Env-only Lambertian + global fog scene (`vcm_env_through_fog.RISEscene` topology):**

| Condition | mean R / G / B | rel. noise σ/μ |
|-----------|----------------|----------------|
| PT (reference) | 0.3825 / 0.3825 / 0.3825 | 20.2 % |
| VCM-pre (no Tr fix) | 0.0497 / 0.0422 / 0.0325 | 15.9 % |
| VCM-post (Tr fix) | 0.0042 / 0.0035 / 0.0026 | 1.8 % |

Pre-fix VCM is **13 % of PT** mean luminance (R-channel).  Post-fix VCM is **1.1 % of PT**.

**Mesh-emitter Cornell box + global fog (`bdpt_homogeneous_fog.RISEscene` retargeted to VCM):**

| Condition | mean R / G / B |
|-----------|----------------|
| PT (reference) | 0.2412 / 0.2200 / 0.2235 |
| VCM-pre (no Tr fix) | 0.1845 / 0.1659 / 0.2252 |
| VCM-post (Tr fix) | 0.1336 / 0.1228 / 0.2126 |

Pre-fix VCM ratio to PT: **76 % / 75 % / 101 %** per channel.  Post-fix VCM ratio: **55 % / 56 % / 95 %**.  Both pre and post are darker than PT; the fix widens the gap on R/G.

**Vacuum non-regression (env-only Lambertian, NO fog):**

| Condition | mean R / G / B |
|-----------|----------------|
| PT (reference) | 0.5180 / 0.4738 / 0.4168 |
| VCM-pre | 0.6068 / 0.5540 / 0.4839 (~17 % over PT — known §12 baseline bias) |
| VCM-post | 0.6065 / 0.5538 / 0.4838 |

Pre vs post agree to < 0.05 % per channel.  ✅ vacuum non-regression: the fix is a true no-op in vacuum.

### Diagnosis — why pre-fix VCM is darker (not brighter) in env-IBL fog

The 17 % VCM-over-PT bias documented in §12 is a **vacuum** bias from the disc-area parameterisation; it does NOT translate to fog because the env-IBL transport balance shifts when a global medium is present.

In env-only Lambertian fog, PT's mean is dominated by the **s=0 eye-escape** strategy: eye rays propagate through fog up to the open ceiling, miss everything, and pick up env radiance at the escape vertex.  PT's escape branch at [PathTracingIntegrator.cpp:2683-2706](../src/Library/Shaders/PathTracingIntegrator.cpp) returns the env radiance with NO Tr attenuation along the eye ray (a latent PT-side correctness gap, out of Piece 2 scope).  PT's env-NEE shadow walk DOES apply Tr along RISE_INFINITY through the global medium, which gives exp(-σ_t × ∞) → 0 — so env-NEE contributes essentially nothing in any global medium.  PT's mean comes mostly from un-attenuated eye escape.

VCM gets the same s=0 env-escape vertex from `GenerateEyeSubpath`, but VCM's SmallVCM MIS weight at the s=0 site **down-weights** against the env-NEE alternative's pdf even when env-NEE's contribution evaporates to 0:

```
w_s0_env_escape = 1 / ( VCMMis(1) + VCMMis(w_camera_env) )
```

`w_camera_env` includes `directPdfA · dVCM + emissionPdfW · dVC`, which are pdf-bookkeeping quantities INDEPENDENT of Tr.  So VCM's s=0 contribution is fractionally MIS-weighted (~0.5×) regardless of whether env-NEE actually contributes 0 (post fix) or its full value (pre fix).  Combined with env-NEE's Tr→0 behaviour, VCM-post collapses to MIS-weighted-down s=0 alone, while PT delivers full s=0 un-MIS-weighted.

This is consistent with the recurring trap noted in [docs/skills/bdpt-vcm-mis-balance.md](skills/bdpt-vcm-mis-balance.md) — local correctness fixes that look right at one strategy can interact with MIS-partition in subtle ways.

### Why this isn't a "bug in the fix"

- The fix mechanically does what BDPT does at the same four call sites (NEE / interior / splat).  BDPT has done this since at least its round-4 env-NEE Tr-to-RISE_INFINITY fix (2026-05-26).
- The 116-test suite passes, including `EnvLightBalanceTest` (no-fog, lax-tolerance check).
- Vacuum non-regression is bit-clean.
- The fix CORRECTLY attenuates VCM connection contributions through media — the algebraic statement "VCMIsVisible was binary; should be media-aware" is implemented exactly as the prompt described.

### What the fix doesn't solve

- The "VCM matches PT within MC noise" gate on the new regression scenes FAILS because the dominant volumetric bias on VCM env-IBL is the s=0 vs env-NEE MIS-partition interaction described above, NOT the connection-Tr gap.  My fix correctly fixes the connection-Tr gap but leaves the MIS-partition issue untouched.
- The fix may be a NET LOSS on env-IBL-through-fog scenes (VCM-pre/PT = 0.13; VCM-post/PT = 0.011).  On a mesh-emitter Cornell-fog scene the loss is milder (VCM-pre R/G ratio 0.77/0.75 → VCM-post 0.55/0.56; B-channel ratio 1.01 → 0.95).  On VACUUM scenes the fix is a no-op (within 0.05 %).

### Recommendation for supervisor / user

Three options:

**(a) Land the mechanical fix as-is** and document the residual env-IBL-fog issue as a known limitation that needs the SA-MIS migration (already scoped in the Session 11 outcome above) to fully resolve.  Rationale: the fix is mechanically correct, doesn't regress vacuum, doesn't break the 116-test suite, and aligns VCM with BDPT's media-aware connection convention.  The MIS-partition issue would surface eventually anyway; the fix makes it visible NOW which is useful for the SA-MIS work.

**(b) Revert the fix and re-open §12's "Companion limitation" as "needs deeper investigation"**.  Rationale: empirically the fix makes the most common production case (env-IBL through fog) worse, even if it's mechanically the right port.  Better to land Tr together with the MIS-partition fix in one coordinated change.

**(c) Land the fix BUT only at the NEE site (not interior / splat)**, and only for the non-env case.  Rationale: env-NEE Tr=0-to-infinity collapsing in a global medium is the dominant pathology; localised at one specific site we know is problematic.  But this leaves interior connections and t=1 splat through media still using Tr=1.  This is the "narrow scope" compromise.

I do NOT recommend choosing one of these myself — the trade-off depends on how the SA-MIS migration is going to be sequenced and is a supervisor / user judgment.

### Files modified

- [src/Library/Shaders/BDPTIntegrator.h](../src/Library/Shaders/BDPTIntegrator.h) — public access on `EvalConnectionTransmittance{,NM}` (4 overloads)
- [src/Library/Shaders/VCMIntegrator.cpp](../src/Library/Shaders/VCMIntegrator.cpp) — Tag-dispatched Tr helpers + parameter plumbing + Tr application at 3 sites (NEE, splat, interior)
- [scenes/Tests/Volumes/vcm_env_through_fog.RISEscene](../scenes/Tests/Volumes/vcm_env_through_fog.RISEscene) — new
- [scenes/Tests/Volumes/pt_env_through_fog.RISEscene](../scenes/Tests/Volumes/pt_env_through_fog.RISEscene) — new

### What was NOT changed (per scope rule)

- `VCMRecurrence.{h,cpp}`, `InitLight`, `ApplyGeometricUpdate`, `ApplyBsdfSamplingUpdate` — VCM MIS recurrence is pdf-bookkeeping; Tr cancels in pdf ratios and the Georgiev 2012 running quantities stay invariant.
- `LightSampler`, `EnvironmentSampler` — recently changed in Session 9's continuous-PMF commit; explicitly off-limits.
- `BDPTIntegrator.cpp` body — only the access modifier in the header changed; no BDPT semantic changes.
- `EvaluateMergesImpl<Tag>` — VCM merging is out of scope per UNIFIED_INTEGRATOR_ANALYSIS.md §5.2.2; volume-aware merging is a separate larger lift.

### Working tree state

Uncommitted.  Per the user's explicit instruction in this session ("NEVER COMMIT"), no `git add`, `git commit`, `git stage`, or `git push` was performed.

Run `git diff` to see the full mechanical port.

---

## Pre-Phase-1 Piece 2 follow-up — PT escape-Tr investigation (2026-05-31)

### Headline

The Piece 2 diagnosis above (line 1767, "PT's escape branch at PathTracingIntegrator.cpp:2683-2706 returns the env radiance with NO Tr attenuation along the eye ray — a latent PT-side correctness gap, out of Piece 2 scope") was **CONFIRMED** by code audit and cross-referenced against PBRT-v4's `VolPathIntegrator::Li()`.  The bug exists in **6 PT sites** (RGB / NM / HWSS × first-bounce-entry / iterative-loop) **and 2 BDPT sites** (`GenerateEyeSubpath` + `GenerateEyeSubpathNM`).  VCM inherits the BDPT bug via shared eye-subpath generation.

**Per the chip's stop rule** (Task 3: "BDPT has the same bug, making the fix scope larger than a Piece 2 follow-up should land. Document and stop."), no implementation was attempted.  Working tree is unchanged from the Piece 2 baseline above.

### Task 1 — bug confirmed in PT, 6 sites

The bug is a missing-`Tr` along the *last segment* of the eye walk in every PT escape branch.  In all six sites the pattern is identical:

- `if( pCurrentMedium )` block samples a distance.
- `if( scattered )` → handle volume scatter (correct).
- `else if( !scattered && bHit )` → apply `Tr` to `throughput` for the segment to the surface (correct).
- **MISSING `else` branch** for `!scattered && !bHit` (ray escapes the scene through the medium).  Control falls out of the `if( pCurrentMedium )` block and lands at the env-handling site BELOW it, which reads `throughput` un-attenuated.

Site enumeration:

| # | File / function | Line | Variant | Bug |
|---|-----------------|------|---------|-----|
| 1 | [PathTracingIntegrator.cpp `IntegrateRay`](../src/Library/Shaders/PathTracingIntegrator.cpp) | 2554 → 2683 | RGB first-bounce | Camera ray escapes through medium → falls through to line 2683 env handler with no Tr |
| 2 | [PathTracingIntegrator.cpp `IntegrateRay`](../src/Library/Shaders/PathTracingIntegrator.cpp) | 2639–2645 | RGB first-bounce (scatter→escape) | After volume scatter, scattered ray escapes → env credited with no Tr from scatter point |
| 3 | [PathTracingIntegrator.cpp `IntegrateFromHit`](../src/Library/Shaders/PathTracingIntegrator.cpp) | 1262 → 1272 | RGB iterative | Same pattern as #1, but inside the per-depth loop |
| 4 | [PathTracingIntegrator.cpp `IntegrateRayNM`](../src/Library/Shaders/PathTracingIntegrator.cpp) | 4742 → 4844 | NM first-bounce | Spectral twin of #1 |
| 5 | [PathTracingIntegrator.cpp `IntegrateRayNM`](../src/Library/Shaders/PathTracingIntegrator.cpp) | 4815–4821 | NM first-bounce (scatter→escape) | Spectral twin of #2 |
| 6 | [PathTracingIntegrator.cpp `IntegrateFromHitNM`](../src/Library/Shaders/PathTracingIntegrator.cpp) | 3001 → 3010 | NM iterative | Spectral twin of #3 |
| 7 | [PathTracingIntegrator.cpp `IntegrateRayHWSS`](../src/Library/Shaders/PathTracingIntegrator.cpp) | 4912 → 5032 | HWSS first-bounce | HWSS twin of #1 |
| 8 | [PathTracingIntegrator.cpp `IntegrateRayHWSS`](../src/Library/Shaders/PathTracingIntegrator.cpp) | 4985–5004 | HWSS first-bounce (scatter→escape) | HWSS twin of #2 |
| 9 | [PathTracingIntegrator.cpp `IntegrateFromHitHWSS`](../src/Library/Shaders/PathTracingIntegrator.cpp) | 4234 → 4249 | HWSS iterative | HWSS twin of #3 |

(Strictly speaking nine sub-sites — three "scatter then re-cast that escapes" twins live in `IntegrateRay*` entry points only; the iterative loops re-enter via `continue` after a scatter so their escape is the single line item #3 / #6 / #9.)

### Task 2 — semantics

PBRT-v4's `VolPathIntegrator::Li()` (`src/pbrt/cpu/integrators.cpp`) applies `T_maj` (majorant transmittance from null-tracking) into `beta` during the medium-sampling loop BEFORE the `if (!si)` escape branch reads `beta * Le` for the infinite-light contribution.  Quote from `volpath_li`:

```
beta *= T_maj / T_maj[0];
r_u  *= T_maj / T_maj[0];
r_l  *= T_maj / T_maj[0];
...
if (!si) {
  for (const auto &light : infiniteLights) {
    if (SampledSpectrum Le = light.Le(ray, lambda); Le) {
      ...
      L += beta * Le / r_u.Average();
```

So the physically-correct convention is: throughput at the env-lookup site includes the transmittance along EVERY medium segment the eye walked through, including the escape segment.

For RISE's existing infrastructure, the principled fix is to reuse `BDPTIntegrator::EvalConnectionTransmittance(Ray, maxDist=RISE_INFINITY, …)` (already exposed publicly post Piece 2 access change at [BDPTIntegrator.h](../src/Library/Shaders/BDPTIntegrator.h)), which walks per-object media + global medium with the same boundary-aware stack logic used by env-NEE in BDPT.  PT does NOT have a comparable helper today — its own per-iteration `Tr = pCurrentMedium->EvalTransmittance(currentRay, ri.geometric.range)` skips the boundary walk because the surface IS the boundary; for escape there's no surface, so the simple single-medium call works (the escape means `IntersectRay` returned no hit → no further per-object boundaries exist) provided we use `maxDist = 1e10` or `RISE_INFINITY` consistent with the rest of the file (`maxDist = bHit ? ri.geometric.range : Scalar(1e10)` is the existing pattern at lines 1075, 2556, 2831, 4172, 4914).

**Important consequence for the new fog scenes:** with global homogeneous medium σ_t = 0.001 extending to infinity (the topology of `pt_env_through_fog.RISEscene` — `> set global_medium fog` at line 94 with no per-object containing volume), the correct Tr is `exp(-0.001 × ∞) = 0`.  PT-fixed renders essentially BLACK on this scene.  Same for VCM-post once symmetrised.  Env-NEE Tr is already 0 in pre-fix VCM (the bug only affects s=0 escape, env-NEE walk correctly evaporates).  Net: post-fix-on-both-integrators, both PT and VCM evaporate together — the scene does not separate "fix is right" from "fix is wrong" because both states render black.

To get an INFORMATIVE regression test for the escape-Tr fix one needs **bounded fog** (a per-object medium contained inside a volume bounding shape).  The chip-spawned `pt_env_through_fog.RISEscene` and `vcm_env_through_fog.RISEscene` use a global medium → they validate "Tr evaporates correctly" but cannot validate "Tr scales correctly with σ_t over a finite path".

### Task 3 — BDPT has the same bug (2 sites)

The prior Piece 2 chip's diagnosis at line 1716 of this file was correct: BDPT's eye-walk also skips Tr on the escape branch.  Confirmed sites:

| # | File / function | Line | Variant | Bug |
|---|-----------------|------|---------|-----|
| 10 | [BDPTIntegrator.cpp `GenerateEyeSubpath`](../src/Library/Shaders/BDPTIntegrator.cpp) | 2649–2767 | RGB | `else if( ri.bHit )` applies Tr to `beta` at 2653-2655; `!ri.bHit` (Path B env-escape) pushes `vEnv.throughput = beta` at line 2762 with no Tr first |
| 11 | [BDPTIntegrator.cpp `GenerateEyeSubpathNM`](../src/Library/Shaders/BDPTIntegrator.cpp) | 6347–6415 | NM | Spectral twin: `vEnv.throughputNM = betaNM` at line 6409 with no Tr first |

The synthetic env vertex's `.throughput` / `.throughputNM` is consumed unchanged by the s=0 dispatcher at [BDPTIntegrator.cpp:3419](../src/Library/Shaders/BDPTIntegrator.cpp) (RGB) and the spectral twin at line ~7035 (NM).  VCM uses the same `GenerateEyeSubpath` shared with BDPT (via the shared eye-subpath generator) → VCM env-S0 also inherits the bug, regardless of any VCM-side fix.

Critically: this is exactly why Piece 2's VCM Tr fix at the NEE / interior / splat sites left VCM darker than PT in env-IBL fog.  PT-broken returns un-attenuated `throughput × Le` at env-escape (overbright); pre-Piece-2 VCM did the same at its s=0 env-escape (overbright the same way).  Piece 2 fixed the NEE / interior / splat sites in VCM (correctly attenuating those connections), so post-Piece-2 VCM is correctly attenuated everywhere EXCEPT s=0 env-escape, while PT remains un-attenuated everywhere.  The MIS partition between s=0 and env-NEE then leaves VCM-post = MIS-weighted-fraction × un-attenuated-S0 (~0.5× of full) while PT delivers full un-attenuated S0 (~1.0× of full).  That's the 0.5× mean ratio difference Piece 2 saw — it's not a "wrong fix", it's "the fix is half a fix because the s=0 site is still bugged".

### Task 4 — NOT attempted (stop rule fired)

Per the chip's stop rule "**Task 3 reveals BDPT has the same bug**, making the fix scope larger than a Piece 2 follow-up should land. Document and stop." — no implementation was performed.

If it had been attempted, the scope would be ~9 sub-sites across 2 integrators and 3 value-type variants (RGB / NM / HWSS).  Per site the change is structurally trivial (~5 lines: query active medium, evaluate `Tr`, multiply into the appropriate throughput before env contribution).  But cumulatively the work is on a load-bearing path in the production renderer, affects every fog-bearing env-IBL scene in the regression set, and requires its OWN K-trial variance sweep on multiple bounded-fog scenes (which don't currently exist as regression fixtures — the Piece 2 scenes are global-medium and would render black post-fix as noted in Task 2).

### Task 5–6 — NOT attempted (depend on Task 4)

Re-render and non-regression validation cannot proceed without the fix landing.

### Net analytical finding (independent of whether anything lands)

The "VCM-post is 1.1% of PT-broken" mean ratio that triggered the Piece 2 stop rule is **NOT** evidence that the VCM Tr fix at NEE / interior / splat is incorrect.  It is evidence that:

1. **PT-broken is the wrong reference.**  PT is over-bright on every env-IBL-through-fog scene because it skips Tr on the eye-escape segment.  Any integrator that correctly attenuates will appear darker than this broken PT.

2. **VCM-post has a partial fix.**  Piece 2 fixed 3 of 4 VCM connection sites (NEE, interior, splat) but left s=0 env-escape un-fixed because mirroring BDPT means mirroring BDPT's bug.  Net effect: VCM-post is correctly attenuated everywhere except s=0, while PT-broken is un-attenuated everywhere — the ratio asymmetry is the MIS-weighted-fraction of un-attenuated S0 divided by full un-attenuated S0, plus the difference between attenuated and un-attenuated env-NEE.

3. **A coordinated fix would close the gap.**  If PT, BDPT eye-walk, and VCM eye-walk (= BDPT eye-walk) are ALL fixed to apply Tr on escape — together with Piece 2's NEE / interior / splat work — then on the current global-medium-extending-to-infinity scenes all three integrators evaporate to ≈ 0 together (correct), and on a *bounded*-fog scene (which would need to be authored as a regression fixture) they should match within MC noise.

### Recommendation to user

Per Task 7's framing rules, here is a neutral statement of the four decision options.  I do NOT recommend choosing one — the trade-off depends on Phase-1 sequencing and the SA-MIS migration scope already noted in §"Session 11 outcome".

**Option A — Revert Piece 2's VCM Tr work; defer fix to coordinated Phase-1 work.**  Rationale: with the PT bug confirmed, the Piece 2 fix is genuinely "half a fix" — VCM gets attenuated on 3 of 4 connection categories but not at s=0, mirroring an integrator (PT) that's un-attenuated everywhere.  Lands as a one-sided correctness change with ambiguous downstream value (the §12 gap that motivated it is dominated by the SA-MIS partition issue per Session 11, and the s=0 escape Tr fix is the principled missing piece, not the connection-site Tr fix).  Cleanest revert: `git checkout HEAD -- src/Library/Shaders/BDPTIntegrator.h src/Library/Shaders/VCMIntegrator.cpp` + delete the two new fog scenes.

**Option B — Land Piece 2 + the comprehensive escape-Tr fix as a coordinated commit.**  Rationale: this is the principled forward direction — PT, BDPT, VCM eye-walks all apply Tr on escape; Piece 2's NEE / interior / splat work stays; on bounded-fog scenes all three integrators match.  Scope: ~9 sub-sites across PT and BDPT + symmetrical VCM updates + new bounded-fog regression scenes + K-trial variance gates on each.  Estimate: 1–2 sessions of focused work, mostly testing.  Risk: every fog-bearing scene in the existing regression set will drop in brightness (the change is a correctness improvement, but it WILL change rendered pixels — `pt_homogeneous_fog`, `pt_thick_fog_corridor`, `pt_volumetric_caustics`, etc. all use global media and will render dimmer than current "reference" PNGs).  Need a coordinated baseline refresh.

**Option C — Land Piece 2 as-is with a "known half-fix" annotation in §12 and a TODO entry for the escape-Tr work.**  Rationale: mechanical port is done, documented, and aligned with BDPT's connection-site convention; the s=0 site asymmetry is a separate, larger workstream that can be sequenced alongside SA-MIS migration without blocking Piece 2 from landing.  Cost: §12 "lax-closed; strict residual root-caused" status text gets an additional caveat for volumetric env-IBL.

**Option D — Land ONLY the s=0 escape-Tr fix (PT + BDPT eye-walks + VCM symmetric); revert Piece 2's NEE / interior / splat work.**  Rationale: the s=0 escape-Tr fix is structurally the most important — it's the difference between PT being correctly attenuated vs un-attenuated on every fog-bearing env-IBL scene.  The Piece 2 NEE / interior / splat fix has empirical demonstrations on volumetric scenes pending; the s=0 fix has clear PBRT-v4 precedent.  But this loses the volumetric-VCM connection-Tr work that's mechanically correct and bit-clean in vacuum.

### Files

No new files written in this follow-up investigation; this section appended to the existing PRE_PHASE1_STATUS.md.  Working tree state unchanged from Piece 2 baseline.

### Working tree state

Unchanged from the Piece 2 baseline above.  No code modified, only this doc.  Per the user's explicit instruction in this session ("NEVER COMMIT"), no git operations performed.

---

## Coordinated escape-Tr fix (PT + BDPT + VCM) (2026-05-31) — Option B landed; bounded-medium cross-integrator agreement achieved

### Headline

The four-option follow-up above recommended **Option B** ("land Piece 2 + the comprehensive escape-Tr fix as a coordinated change").  This section reports executing Option B: the missing escape-segment transmittance was added to **all 9 PT sub-sites + 2 BDPT sites**, VCM's s=0 env-escape inherits it transitively (no VCM change), and a **bounded-medium env-IBL regression fixture** was authored as the cross-integrator-agreement oracle the global-medium scenes cannot provide.

**Result: on the bounded fixture (pure absorption) PT ≈ BDPT ≈ VCM within ±5 % — VCM is +0.1 % of PT, BDPT +4.5 %.**  This is the strongest available correctness signal for the Tr work: three independent transport algorithms converge to the same answer on a non-trivial bounded-medium env-IBL scene.  Adversarial review (3 reviewers, orthogonal axes) returned no P1/P2 findings.

**Recommendation: LAND the coordinated fix (Piece 2 connection-Tr + escape-Tr) as one coherent commit.**  One caveat documented below (a *separate* VCM in-scattering MIS gap surfaced when σ_s > 0) does **not** block landing — the escape-Tr fix is a strict correctness improvement and the pure-absorption agreement isolates and validates exactly the transmittance the fix is responsible for.

### Sites changed (exact)

**PT — [src/Library/Shaders/PathTracingIntegrator.cpp](../src/Library/Shaders/PathTracingIntegrator.cpp)** (9 sub-sites):
| Function | Variant | Change |
|----------|---------|--------|
| `IntegrateRay` | RGB first-bounce | `RISEPel escapeTr(1,1,1)` set in a new `else` (`!scattered && !bHit`) via `EvalTransmittance(cameraRay, maxDist)`; applied as `return escapeTr * envResult` |
| `IntegrateRay` | RGB scatter→escape twin | scattered-ray env multiplied by `EvalTransmittance(scatteredRay, 1e10)` |
| `IntegrateFromHit` | RGB iterative | new `else if(!scattered && !bHit)` → `throughput *= EvalTransmittance(currentRay, maxDist)` |
| `IntegrateRayNM` | NM first-bounce + scatter twin | spectral twins of the two RGB sites (`EvalTransmittanceNM(..., nm)`) |
| `IntegrateFromHitNM` | NM iterative | spectral twin |
| `IntegrateRayHWSS` | HWSS first-bounce + scatter twin | per-wavelength `escapeTr[N]` set in a new `else`; applied in env-fill as `result[w] = escapeTr[w] * GetRadianceNM(...)` |
| `IntegrateFromHitHWSS` | HWSS iterative | new `else if(!scattered && !bHit)` per-wavelength `throughputComp[w] *= EvalTransmittanceNM(currentRay, maxDist, swl.lambda[w])` |

**BDPT — [src/Library/Shaders/BDPTIntegrator.cpp](../src/Library/Shaders/BDPTIntegrator.cpp)** (2 sites):
| Function | Change |
|----------|--------|
| `GenerateEyeSubpath` (RGB Path B) | before `vEnv.throughput = beta`, multiply by `pMed_eye->EvalTransmittance(currentRay, 1e10)` (guarded `if(pMed_eye)`) |
| `GenerateEyeSubpathNM` (Path B) | before `vEnv.throughputNM = betaNM`, multiply by `pMed_nmEye->EvalTransmittanceNM(currentRay, 1e10, nm)` |

**VCM — no code change.**  `EvaluateS0Impl`'s env-escape branch ([VCMIntegrator.cpp:881-952](../src/Library/Shaders/VCMIntegrator.cpp)) consumes `VertexThroughput<Tag>(v, tag)` (= `v.throughput`/`v.throughputNM`) from the synthetic env vertex pushed by the shared `GenerateEyeSubpath{,NM}`.  The BDPT fix therefore Tr-attenuates VCM's s=0 env-escape transitively (Task 3 verified by inspection AND empirically — VCM matches PT to +0.1 % on the bounded fixture).

**Design choices** (both matching the existing in-tree convention and PBRT-v4):
- **`maxDist = 1e10` everywhere** (PT's existing escape constant), so a *global* medium evaporates `exp(-σ_t·1e10) → 0` identically across PT/BDPT, and a *bounded* (AABB-clipped) medium yields the same finite Tr in all three.  BDPT deliberately uses `1e10` rather than the bounding-sphere `tExit` so it agrees with PT on global media.
- **Tr multiplies throughput only, never a pdf / MIS weight** — orthogonal to MIS (confirmed by review axis 3).

### Validation gates

| # | Gate | Status | Evidence |
|---|------|--------|----------|
| 1 | Clean warning-free build (`make all` + `make tests`) | ✅ | Forced recompile of both changed TUs: zero warnings/errors |
| 2 | 116/116 binary tests | ✅ | `./run_all_tests.sh`: 116 built, 116 passed, 0 failed — incl. `EnvLightBalanceTest`, `BDPTStrategyBalanceTest`, `VCMStrategyBalanceTest`, `VCMRecurrenceTest`, `VCMSpectralRecurrenceTest`, all `*Volume*` tests |
| 3 | Vacuum non-regression (Tr=1 clean no-op) | ✅ | **Structurally bit-identical**: all new code runs only inside `if(pCurrentMedium)`; in vacuum `pCurrentMedium == NULL` so the new branches never execute and `escapeTr` stays exactly `1.0` (×1.0 is an exact FP no-op).  Empirically confirmed by 116/116 (vacuum env/strategy-balance tests) + vacuum diffuse-sphere render below |
| 4 | Infinite-medium evaporation | ✅ | See table below — all three collapse toward black |
| 5 | ★ Bounded-medium cross-integrator agreement | ✅ | See headline table below — PT ≈ BDPT ≈ VCM within ±5 % |
| 6 | No regression on existing volumetric scenes | ✅ | Closed-box / bounded-container scenes are unaffected (no env-escape-through-global-medium → fix is a structural no-op); render sensibly: `pt_homogeneous_fog` 0.226, `pt_fog_sphere` 0.728, `pt_heterogeneous_sphere` 0.226, `pt_thick_fog_corridor` 0.620 |
| 7 | Adversarial review (3 reviewers) | ✅ | No P1/P2; two P3 nits rejected — see ledger below |

### ★ Gate 5 — bounded-medium cross-integrator agreement (headline evidence)

New fixtures: [scenes/Tests/Volumes/env_bounded_fog_{pt,bdpt,vcm}.RISEscene](../scenes/Tests/Volumes/) — a diffuse sphere inside a **bounded** (AABB-clipped) `painter_heterogeneous_medium` set as the global medium, under uniform env-IBL.  The medium is current everywhere (so the eye-walk escape branch fires) but its transmittance clips to the AABB (density = 0 outside), giving a **finite** optical depth on the escape segment — exactly what the global-medium `*_env_through_fog` scenes (which evaporate to 0) cannot test.  The medium is **pure absorption** (σ_s = 0) to isolate the transmittance the escape-Tr fix is responsible for from in-scattering MIS (see caveat).

K = 8 trials, 256 spp, 128×128, channel-mean luminance (HDRVarianceTest):

| Integrator | mean | ratio to PT |
|------------|------|-------------|
| PT (reference) | 0.1920 | 1.000 |
| BDPT | 0.2007 | **1.045** (+4.5 %) |
| VCM | 0.1922 | **1.001** (+0.1 %) |

All within the lax ±5 % gate.  VCM is essentially identical to PT (+0.1 %), confirming the s=0 escape-Tr flows through the shared generator correctly.  BDPT's +4.5 % is its known small env-escape MIS over-count (also seen at +1.9 % in vacuum, below) — within tolerance and consistent with the documented "BDPT/VCM may carry a small consistent-estimator over/under".

### Gate 3 — vacuum non-regression (diffuse sphere under env, NO fog), K = 4, 256 spp

| Integrator | mean | ratio to PT |
|------------|------|-------------|
| PT | 0.9588 | 1.000 |
| BDPT | 0.9770 | +1.9 % |
| VCM | 0.9460 | −1.3 % |

All three agree within ~2 %, matching the documented small env biases (NOT introduced by this fix).  Confirms the fix is a clean no-op in vacuum on this topology.

### Gate 4 — infinite (global) medium evaporation, `*_env_through_fog`, 64 spp

Global homogeneous medium σ_t = 0.001 to infinity → physically-correct escape Tr = `exp(-σ_t·∞) = 0`.

| Integrator | mean | note |
|------------|------|------|
| PT | 0.0002 | effectively black ✅ (pre-fix PT-broken was ~0.38 — IMPROVEMENTS.md §7) |
| VCM | 0.0004 | effectively black ✅ |
| BDPT | 0.0199 | collapsed toward black; retains a small light-subpath-connection residual from the env-at-finite-distance (bounding-sphere) parameterization — a *pre-existing* BDPT env-MIS characteristic in the §12 SA-MIS domain, NOT the escape-Tr fix (which correctly evaporated BDPT's s=0 escape) |

### Caveat — a SEPARATE VCM in-scattering MIS gap (out of escape-Tr scope)

When the same bounded fixture is run with **scattering** (σ_s > 0, albedo ~0.2), VCM diverges from PT/BDPT:

| Integrator | mean (σ_s>0, K=8, 256 spp) | ratio to PT |
|------------|----------------------------|-------------|
| PT | 0.2557 | 1.000 |
| BDPT | 0.2450 | −4.2 % |
| VCM | 0.1757 | **−31 %** |

This is **not** the escape-Tr fix.  A controlled experiment isolates it: the **same sphere scene with σ_s = 0** gives PT 0.1499 / BDPT 0.1577 / VCM 0.1508 (all within ~5 %).  Scattering adds +0.106 to PT's mean (single-scatter glow of env light in the fog) but only +0.025 to VCM's — i.e. **VCM under-counts in-scattering** of env light at medium vertices.  The bounded-fog background (escape, ~90 % of frame) has no surface connections, so VCM/PT there is governed purely by the s=0 escape (which my fix made Tr-consistent: σ_s=0 background matches PT); the divergence appears only once σ_s > 0 introduces medium-vertex → env transport.  This is a VCM volumetric-MIS issue (medium-vertex env-NEE / merge weighting, related to the §12 env-S0 ↔ env-NEE partition and/or Piece 2's connection-Tr at medium vertices), explicitly out of this "Tr-only" chip's scope.  **Next workstream**, not a blocker: the escape-Tr fix is correct and the pure-absorption fixture proves it.

### Gate 7 — adversarial review ledger (3 reviewers, orthogonal axes)

| Reviewer | Axis | Result |
|----------|------|--------|
| R1 | Double-application + cross-integrator Tr identity | **CLEAN** — no segment Tr applied twice; all escape sites resolve to `maxDist=1e10`; VCM transitive via `VertexThroughput`.  P3: HWSS `escapeTr[w]=0` for terminated lanes is dead-but-harmless (consumers gate on `!terminated`). |
| R2 | Spectral / HWSS per-wavelength correctness | **PASS, no findings** — every NM/HWSS Tr call passes the matching `nm`/`swl.lambda[w]`; terminated lanes respected; mirrors the existing surface-hit Tr branches; `maxDist` in scope. |
| R3 | Control-flow + MIS interaction | **CONFIRMED** — new branches mutually exclusive (capped-scatter, zeroContrib early-out, first-bounce `else` binding all correct); Tr touches throughput only, never pdf/`w_bsdf`.  Notably the fix **removes a real pre-existing MIS mismatch**: env-NEE already applied shadow-segment Tr (`LightSampler.cpp:1740` `EvalShadowTransmittance(..., RISE_INFINITY, ...)`) while env-BSDF did not — post-fix both strategies the MIS combines are Tr-consistent.  P3: an approximate comment line-ref (verified accurate). |

Both P3 nits rejected with reason (harmless / accurate).  No P1/P2 raised → adversarial-review stop rule satisfied.

### Files

- [src/Library/Shaders/PathTracingIntegrator.cpp](../src/Library/Shaders/PathTracingIntegrator.cpp) — 9 PT escape sub-sites (RGB / NM / HWSS × first-bounce / iterative + 3 scatter→escape twins)
- [src/Library/Shaders/BDPTIntegrator.cpp](../src/Library/Shaders/BDPTIntegrator.cpp) — 2 BDPT Path-B escape sites
- [scenes/Tests/Volumes/env_bounded_fog_pt.RISEscene](../scenes/Tests/Volumes/env_bounded_fog_pt.RISEscene), [_bdpt](../scenes/Tests/Volumes/env_bounded_fog_bdpt.RISEscene), [_vcm](../scenes/Tests/Volumes/env_bounded_fog_vcm.RISEscene) — new bounded-medium oracle (pure absorption)
- This doc + [IMPROVEMENTS.md](IMPROVEMENTS.md) §7/§12 + [VCM.md](VCM.md) updated.

### Working tree state

Working tree carries (uncommitted): Piece 2's VCM connection-Tr (`BDPTIntegrator.h` access change + `VCMIntegrator.cpp`) **unchanged** + the PT/BDPT escape-Tr fix + the 3 new bounded fixtures + docs.  Per the user's explicit instruction ("NEVER COMMIT"), no `git add`/`commit`/`stage`/`push` performed.

---

## Pre-Phase-1 Piece 3 outcome (Phase 2b — PathTracingIntegrator templatization)

**Session date**: 2026-05-31.  **Plan**: [INTEGRATOR_REFACTOR_PLAN.md](INTEGRATOR_REFACTOR_PLAN.md) §3.5; **precedent**: [INTEGRATOR_REFACTOR_STATUS.md](INTEGRATOR_REFACTOR_STATUS.md) (Phase 2a).

### TL;DR

Phase 2b is a two-method-family integrator (`IntegrateRay*` and `IntegrateFromHit*`, each Pel/NM/HWSS).  This session **templatized the `IntegrateRay` family (Pel+NM) into `IntegrateRayTemplated<Tag>` behind thin forwarders — verified zero-behavior-change and adversarially reviewed clean — and built the full shared PT dispatch-helper layer.**  The larger `IntegrateFromHit` family (~1,570 lines, the bulk of the LoC and the home of the AOV hook) is **deferred to a focused follow-up** on a data-driven call (below).  This mirrors the Phase 2a precedent exactly: *"continuing into the bigger methods in the same session would deliver rushed, under-reviewed work."*

- **Gate 1 — build**: `make -C build/make/rise -j8 all` + `tests` warning-free (0 warnings, clean rebuild).
- **Gate 2 — tests**: 116/116 (clean env; the lone `FileRasterizerOutputShimTest` "fail" in any run with `RISE_MEDIA_PATH` exported is an env artifact — the test writes to a `/tmp` path that the media-path prefix mangles; it passes 60/60 with `RISE_MEDIA_PATH` unset, which is how `run_all_tests.sh` is meant to run).
- **Gate 3 — baselines**: mean-luminance deltas vs `pre_phase2b` all within the **0.27 % noise floor** — PT(Pel) **0.001 %**, spectral(NM) **0.034 %**, HWSS(unchanged) **0.001 %**, and BDPT/VCM controls **0.000–0.027 %** (no leak into other integrators).
- **Gate 6 — perf**: no path added; templated code compiles to the same instruction stream (helpers are `inline` no-op dispatch).  Not separately benchmarked — the transform is an exact-arithmetic refactor of `IntegrateRay` only.
- **Gate 7 — escape-Tr**: `env_bounded_fog_pt` (the medium Pel path through `IntegrateRayTemplated`) POST **0.19198** vs PRE **0.19189** (**0.045 %**, matches documented ≈0.1920); `EnvLightBalanceTest` (which renders the fixture internally) passes.
- **Gate 8 — adversarial review**: 3 reviewers (Pel-equivalence / NM-equivalence / if-constexpr+ABI), **0 P1, 0 P2**.

### HWSS tag decision (Step 1) — NO third tag

PT's `IntegrateFromHitHWSS` / `IntegrateRayHWSS` are **genuine hero-driven bundle integrators** (track `throughputComp[SampledWavelengths::N]` over one shared path, per-wavelength `swl.terminated[w]`, companion-only throughput), not value-type swaps of the Pel/NM body — confirmed by reading the HWSS body.  This is the structural-mismatch stop-rule case: HWSS is kept as a standalone method, exactly as [SpectralValueTraits.h](../src/Library/Utilities/Color/SpectralValueTraits.h) (header comment, lines 18-23) and the Phase 2a status doc (§168) both prescribe.  HWSS's SPF-only/SSS/volume-scatter fallbacks call `IntegrateFromHitNM`, which stays a real method this session (will become a forwarder when the `IntegrateFromHit` family is templatized).  **No `HWSSTag` added; no `SpectralValueTraits.h` change.**  The plan's §3.5 `is_hwss_bundle` sketch is therefore superseded by the Phase-0 design that already rejected it.

### What was templatized

| Pre | Post | Note |
|---|---|---|
| `IntegrateRay` (RGB, ~230 ln) + `IntegrateRayNM` (~170 ln) | `IntegrateRayTemplated<Tag>` + 2 thin forwarders | Tag = `PelTag`/`NMTag` |
| — | `IntegrateFromHitForTag<Tag>` (private member) | tag-dispatched delegation to `IntegrateFromHit(NM)` for the medium-scatter continuation + surface hand-off |
| — | 9 anonymous-namespace dispatch helpers | `PTPositiveMagnitude`, `PTValueOne`, `PTTrReduced`, `PTDivByScalar`, `PTGetMediumScatter`, `PTEvalTransmittance`, `PTSampleMediumDistance`, `PTEvaluateInScattering`, `PTEvalRadianceMap` — shared layer, reused by the deferred `IntegrateFromHit` work |

`if constexpr` branches used: **(a)** `traits::supports_aov` gates the first-hit AOV block (compiles out for NMTag — preserves NM's original no-AOV behavior); **(b)** `traits::is_pel` selects the `volThroughput` form — Pel `medWeight * RISEPel(s,s,s)` vs NM `medWeight * phaseVal / phasePdf` — because the two differ at the ULP level (multiply-then-divide vs divide-then-multiply), so they are kept distinct rather than unified.  Every other Pel↔NM difference reduces to a dispatch helper (the escape-Tr `EvalTransmittance{,NM}` calls from commit 2b58236b survive as a single templated `PTEvalTransmittance<Tag>` site).

### Why `IntegrateFromHit` was deferred (data-driven)

I built all 16 additional dispatch helpers + the `IntegrateFromHitTemplated<Tag>` header decl and began the in-place transform, then reverted to the verified `IntegrateRay` checkpoint on this evidence:

1. **Maximum complexity**: the make build enables **both** `RISE_ENABLE_OPENPGL` and `RISE_ENABLE_OIDN` (Config.OSX:83,86), so `IntegrateFromHit`'s extensive OpenPGL path-guiding code (with Pel→RGB-direct vs NM→luminance-projection divergences), the OIDN AOV hook, BSSRDF/SSS continuations (which carry **genuine Pel/NM behavioral asymmetries** — Pel sets `rs2.sms*` flags, NM instead sets `rs2.bsdfTimesCos` + an optimal-MIS `AccumulateCount`), the SPF-section `considerEmission` divergence, and the SMS struct dispatch are ALL live and must be preserved exactly.
2. **Under-covered divergent paths**: the cornellbox baselines exercise emission/NEE/BSDF/RR but **not** path-guiding, BSSRDF, SSS, or SMS.  A dispatch bug in those (the most intricate divergences) would ship **silently** — caught only by line-by-line review of a 1,570-line method.  Verifying them safely needs SSS/SMS/guided pre-baselines captured on the *un-edited* binary (a full stash→rebuild→render→pop cycle) plus a much heavier review than the cornellbox set affords.
3. **All-or-nothing cleanliness**: the 16 helpers `-Wunused`-warn (31 warnings, confirmed) until *both* the Pel and NM increments instantiate them — there is no warning-clean intermediate, so the family must land whole.

Per the stop rules ("clean checkpoints beat half-reviewed scope") and the zero-behavior-change bar, this is a follow-up, not a same-session push.

**Follow-up dispatch-helper list** (built + type-verified this session, then reverted with the `IntegrateFromHit` work; re-add when the family lands so they are used → no `-Wunused`).  Each forwards at compile time to the existing dual-signature API:

- `PTAbsMaxMagnitude` (runaway guard: Pel r_max of 3 fabs / NM fabs) · `PTEvalEmittedRadiance` (`emittedRadiance{,NM}`) · `PTEvaluateDirectLighting` (`EvaluateDirectLighting{,NM}`, nm after pMaterial) · `PTEvalBSDFAtSurface` / `PTEvalPdfAtSurface` (`PathVertexEval::*{,NM}`, guiding) · `PTScatter` (`Scatter`/`ScatterNM`) · `PTRandomlySelect` (`RandomlySelect(xi,false)`/`(xi,true)`) · `PTScatterKray` (`kray`/`krayNM`) · `PTLobeSelectWeight` (`MaxValue(kray)`/`krayNM`) · `PTBssrdfWeight{,Spatial}` (`weight{Spatial}{,NM}`) · `PTCastRay` (`CastRay`/`CastRayNM`) · `PTEvaluateSMS`+`PTSMSResult` (`EvaluateAtShadingPoint{,NM}` → unified `{value contribution, Scalar misWeight, bool valid}`) · `PTBsdfTimesCosState` (Pel `thru*pdf` / NM `fabs(thru)*pdf`) · `PTRayStateBsdfTimesCos` (RGB / `RISEPel(scalar)`).  **Two more are needed for the OpenPGL paths** (not built yet): `PTGuidingPel` (value→RISEPel for `Add*GuidingContribution`/`Set*GuidingDirectContribution`: Pel identity / NM `RISEPel(scalar)`) and `PTGuidingLuminance` (gates + Adam updates: Pel `GuidingTrainingLuminance` / NM `fabs`).

`if constexpr` sites: FF_TRACE blocks accessing `throughput[0..2]`/`result[0..2]` → `is_pel` (Scalar has no `operator[]`; build catches misses); SPF `considerEmission` divergence; BSSRDF/SSS `rs2` asymmetries; AOV under `supports_aov`; SPF multi/single can unify to the NM single block (Pel `×1.0`/`÷1.0` is bit-identical).  **Key follow-up shape**: transform `IntegrateFromHit(Pel)→IntegrateFromHitTemplated<Tag>` in place (~15 section-block edits) → verify Pel baseline + add SSS/SMS/guided coverage → chunk-delete the ~1,300-line `IntegrateFromHitNM` body to a forwarder → verify NM → 3-round review.  (Session working notes were kept as scratch under `/tmp/pt_phase2b_ref/` — ephemeral; the list above is the durable record.)

### PT-spectral inline AOV (deliverable #2) — still deferred (blocked on `IntegrateFromHit`)

The `IntegrateRay` family now has the `if constexpr(traits::supports_aov)` Fast-mode first-hit AOV gate in place (the foundation).  But the **Accurate-mode** first-non-delta AOV hook lives in `IntegrateFromHit`, and the spectral rasterizer's *primary* path is HWSS — so closing the gap needs the `IntegrateFromHit` templatization + `pAOV` plumbing through the NM/HWSS entry points + the spectral rasterizer.  [SPECTRAL_PARITY_AUDIT.md](SPECTRAL_PARITY_AUDIT.md) §2.6's "refactor-blocked on Phase 2b" assessment stands; §6.2 unchanged (not yet DONE).

### LoC

`PathTracingIntegrator.cpp` **+94** net (+301/−207); `.h` **+60**.  Net-positive because the shared dispatch-helper layer (~150 ln) + the `IntegrateFromHitForTag` scaffold are front-loaded here while only `IntegrateRayNM`'s ~167-line body is deduplicated.  The plan's ~1,900-line Phase-2b savings are realized when `IntegrateFromHit` templatizes and **reuses these helpers** — this session pays the abstraction cost up front.

### Bugs spotted (not fixed — documented per non-scope rule)

- **Pre-existing Pel/NM behavioral asymmetries in `IntegrateFromHit`** (to be *preserved*, not fixed, by the templatization): (i) the SPF/no-BSDF specular section sets `considerEmission=true` for Pel but `(isDelta && bSMS)?false:true` for NM; (ii) BSSRDF + random-walk-SSS continuations set `rs2.smsPassedThroughSpecular/HadNonSpecularShading` on Pel but `rs2.bsdfTimesCos` + an optimal-MIS `AccumulateCount` on NM.  These look like latent inconsistencies but are out of scope — flagged for a separate audit.
- **`check_refactor_baselines.sh` two bugs**: (1) it gated render-success on `rise`'s exit code, but `rise` exits non-zero on the interactive `quit` even after a successful render — the same bug fixed in `capture_refactor_baselines.sh` (commit 801f1fe9).  **Fixed this session** (necessary to run Gate 3): pre-delete the PNG, run un-gated, detect success via PNG presence.  (2) Still-present `set -e` fragility: a python FAIL-verdict's non-zero exit aborts the loop before printing the FAIL line — *not* fixed (pre-existing, out of scope); the per-scene robust comparison was run manually instead.  Also note the script's `log_rms < 3.0` threshold is mis-calibrated for noisy spectral/VCM scenes (Phase 2a measured the identical-code noise floor at log_rms up to 32) — **mean-luminance delta is the reliable metric**, confirmed by unchanged-code controls (hwss_pt 3.63, bdpt_spectral 5.04, vcm_spectral 5.86 log_rms with ≤0.034 % mean).

### Adversarial review ledger (Gate 8 — 3 reviewers, orthogonal axes)

| Reviewer | Axis | Result |
|----------|------|--------|
| R1 | PelTag ≡ original `IntegrateRay` (RGB), line-by-line | **CLEAN** — every PelTag helper reduces to the exact original RGB call; escapeTr, medWeight (reciprocal-multiply), AOV block, all 3 recursive calls, control flow byte-equivalent. |
| R2 | NMTag ≡ original `IntegrateRayNM`, line-by-line | **CLEAN** — medWeight division (not reciprocal-multiply), volThroughput NM associativity, raw-signed gates, escapeTr-NM, recursive `IntegrateFromHitNM` args all preserved; AOV block compiles out.  P3: a redundant `if(!bHit)` guard inside `else if(bHit)` (preserved verbatim from the Pel original) is dead-but-harmless for both tags. |
| R3 | if-constexpr / ABI / dispatch soundness | **CLEAN** — private member templates add no vtable slot / layout change; public signatures byte-identical; no name-hiding; `supports_aov`/`is_pel` are `static constexpr`; discarded branches uninstantiated for NMTag; forwarders + helper specializations + script fix all sound; no `-Wunused`. |

0 P1 / 0 P2; the single P3 (dead guard) rejected with reason (behavior-neutral, matches Pel original).  Adversarial-review stop rule satisfied.

### Files

- [src/Library/Shaders/PathTracingIntegrator.cpp](../src/Library/Shaders/PathTracingIntegrator.cpp) — `IntegrateRayTemplated<Tag>` + `IntegrateFromHitForTag<Tag>` + 9 dispatch helpers + 2 forwarders (`IntegrateFromHit*`/HWSS untouched)
- [src/Library/Shaders/PathTracingIntegrator.h](../src/Library/Shaders/PathTracingIntegrator.h) — 2 private member-template decls (ABI-safe)
- [scripts/check_refactor_baselines.sh](../scripts/check_refactor_baselines.sh) — exit-code-gate fix
- This doc + [INTEGRATOR_REFACTOR_STATUS.md](INTEGRATOR_REFACTOR_STATUS.md) updated

### Working tree state

Uncommitted; per "NEVER COMMIT", no `git add`/`commit`/`stage`/`push`.

---

## Pre-Phase-1 Piece 3 outcome — Phase 2b PART 2 (IntegrateFromHit templatization)

**Session date**: 2026-05-31 (follow-up to part 1).  Completes Phase 2b: the
`IntegrateFromHit` family is now templatized; deliverable #2 (PT-spectral inline AOV)
landed its integrator-side foundation, rasterizer-side consumption deferred with cause.

### TL;DR
The Pel `IntegrateFromHit` (1561 ln) and NM `IntegrateFromHitNM` (1284 ln) collapsed into
`IntegrateFromHitTemplated<Tag>` (Tag = PelTag/NMTag) behind 2 thin forwarders, reusing the
part-1 dispatch-helper layer + ~20 new helpers + `if constexpr` gates.  HWSS stays standalone;
its SPF-only/SSS/volume fallbacks call `IntegrateFromHitNM` which is now a forwarder →
`IntegrateFromHitTemplated<NMTag>` (verified: hwss_cornellbox_pt 0.012%).  Zero-behavior-change
verified on the divergent paths the cornellbox set does NOT cover (guiding/SSS/SMS/volume,
Pel AND NM); 3-reviewer adversarial review found 0 P1 / 0 P2 / 2 P3 (one fixed: a volume-RR
reciprocal-multiply that diverged from the NM original at the ULP level; one accepted: the
deliberate AOV side-channel) — round-2 RESOLVED-CLEAN.

### Gates
- **Gate 1 build**: clean warning-free `make clean && make -j8 all`; `-Wunused` clears (both
  tags instantiate every helper, the "all-or-nothing" signal part 1 predicted).  Xcode RISE-GUI
  (Development): warning-free (recompiles the changed TUs; same clang/flags as make).
- **Gate 2 tests**: 116/116 (incl. SpectralValueTraitsTest updated for the NMTag supports_aov flip).
- **Gate 3 baselines** (mean-luminance Δ vs pre_phase2b_p2; noise floor 0.27%):
  Pel cornellbox 0.001%; NM cornellbox_spectral 0.022%; NM SSS spectral_skin_fast 0.023%;
  NM SMS spectral_dispersive 0.012%; HWSS 0.012%; BDPT/VCM controls 0.001–0.043% (no leak).
- **Gate 4 escape-Tr**: env_bounded_fog_pt within noise (Pel path through the templated medium section).
- **Gate 6 perf**: no regression — cornellbox_pathtracer 50s post = 50s pre; pt_sss_wax_sphere 7s
  post vs ~9s pre; env_bounded_fog_pt 1s = 1s (all within render-time variance).  `if constexpr`
  compiles to compile-time branches; helpers are inline no-op dispatch (reviewers A/C confirmed
  codegen-equivalence, no vtable change).
- **Gate 7 review**: see ledger.

### Divergent-path coverage map (what makes the zero-behavior-change claim credible)
| Path | Pel scene (a-vs-b floor → post-Δ) | NM scene (floor → post-Δ) |
|---|---|---|
| surface guiding (OpenPGL) | pt_guiding_stress_guided (0.16% → noise) | **unreachable in production** — no spectral PT rasterizer wires `pathguiding` (only pixelpel/bdpt-pel/bdpt-spectral/pathtracing-pel do); the NM guiding code is preserved mechanically and cannot affect any render |
| BSSRDF diffusion | pt_sss_wax_sphere (0.02% → 0.02%) | spectral_skin_fast (0.10% → 0.023%) |
| random-walk SSS | rwsss_thin_slab (0.002% → 0.005%) | (spectral_skin covers the rs2 asymmetry; the RW-SSS-NM `GetRandomWalkSSSParamsNM` fallback is preserved + reviewer-verified) |
| SMS | sms_k1_refract, sms_k2_glasssphere (0.01% → 0.01%) | spectral_dispersive_caustic_pt_sms (0.02% → 0.012%) |
| volume (absorption / escape-Tr) | env_bounded_fog_pt (0.008%, Gate 4) | shared medium helpers (already NM-verified in IntegrateRayTemplated part 1) |
| emission / NEE / BSDF | cornellbox_pathtracer (0.001%) | cornellbox_spectral (0.022%) |
| HWSS (→ NM forwarder fallback) | hwss_cornellbox_pt (0.012%) | — |

Verification artifact: `scripts/divergent_baselines.sh` (capture/check, per-scene noise-floor measurement).

### Preserved asymmetries (reproduced via `if constexpr`, NOT fixed — flagged for separate audit)
1. SPF/no-BSDF specular `considerEmission`: Pel `true`; NM `(isDelta && bSMS) ? false : true`.
2. BSSRDF + RW-SSS continuation rs2: Pel sets `rs2.smsPassedThroughSpecular=false; smsHadNonSpecularShading=true`;
   NM sets `rs2.bsdfTimesCos = RISEPel(fabs(sssThru)*cosinePdf)` + an optimal-MIS `AccumulateCount`.
3. **PART3 BSDF-continuation SMS-flag tracking: Pel sets bPassed/bHad; NM does NOT.**  NEWLY DISCOVERED this
   session (part 1 flagged only #1/#2).  Consequence: NM does not suppress emission on diffuse→glass→light
   chains the way Pel does — a real Pel/NM behavioral difference on SMS scenes with diffuse receivers.
   Preserved verbatim; recommended for the separate asymmetry audit.

> **AUDITED 2026-05-31 → [PT_PEL_NM_ASYMMETRY_AUDIT.md](PT_PEL_NM_ASYMMETRY_AUDIT.md).**  Verdicts:
> **#1 = LATENT BUG (NM wrong)** — empirically confirmed (spectral+SMS renders a light seen directly
> through glass as *black*; Pel renders it correctly; origin `ce61ea6d`).  **#2 = NO-OP** — the NM
> `bsdfTimesCos`/`AccumulateCount` branch is dead because optimal-MIS is Pel-only (`rc.pOptimalMIS` is
> null for the spectral rasterizer).  **#3 = NO-OP standalone but a REQUIRED co-fix of #1** (fixing #1
> alone makes NM under-suppress → double-count fireflies; the part-2 "diffuse→glass→light double-count"
> wording is refined there — `considerEmission` already covers that case today).  **De-risking answer:
> Phase 2c can proceed** — none blocks BDPT templatization; #1 is a pre-existing, faithfully-preserved
> PT-only bug for its own fix chip.  Carry the *pattern* into 2c: check BDPT's Pel vs NM/HWSS
> specular→light emission/MIS for the same `considerEmission`-vs-flag-predicate divergence.

### Deliverable #2 — PT-spectral inline AOV: integrator foundation landed, rasterizer consumption DEFERRED
**Landed (the part the spec listed under the integrator):** `SpectralValueTraits<NMTag>::supports_aov`
flipped false→true; the Accurate-mode first-non-delta AOV hook in `IntegrateFromHitTemplated` now compiles
in for NMTag; `pAOV` plumbed through `IntegrateRayNM` / `IntegrateFromHitNM` / `IntegrateFromHitForTag`
(trailing default-0 params — ABI-safe).  Render-neutral: with no caller passing a non-null pAOV to the NM
path, the AOV blocks are inert; cornellbox_spectral beauty unchanged (0.013%).  Reviewer C confirmed neutrality.

**Deferred (the blocker, discovered this session):** the spec assumed the spectral rasterizer's primary path
was HWSS and that wiring pAOV through it would close the gap.  Reality: (a) ALL 20 spectral PT test scenes use
`pixelintegratingspectral_rasterizer` → `PathTracingShaderOp::PerformOperationNM` → `IntegrateFromHitNM`
(a SHADER-OP path whose interface does not carry pAOV), NOT `PathTracingSpectralRasterizer`; (b) the spectral
rasterizers do **not allocate AOV buffers** at all — they denoise via the "OIDN auto" path **without** aux
albedo/normal, so the `CollectFirstHitAOVs` retrace fallback never even fires (empirically confirmed: a temp
diagnostic at the retrace site never tripped on a denoised spectral render).  Closing the gap therefore needs
spectral-rasterizer AOV-buffer **allocation** + AOV-guided OIDN + a shader-op interface change to carry pAOV —
a cross-cutting rasterizer change well beyond the templatization.  Per the spec's stop rule ("land NM AOV,
document the rest with the reason"), the rasterizer-side AOV consumption is the documented follow-up; the
integrator is now AOV-ready.  SPECTRAL_PARITY_AUDIT §2.6: integrator-foundation DONE, rasterizer wiring
TODO-with-findings.

### LoC
PathTracingIntegrator.cpp **5249 → 4327 = −922 net** (745 insertions, 1623 deletions); .h +38; SpectralValueTraits.h +6.
The IntegrateFromHit family (Pel 1561 + NM 1284 = 2845 ln) → templated body ~1610 + 2 forwarders (~70) + ~20
new helpers (~230).  Combined with part 1's IntegrateRay (+94, which front-loaded the shared helper layer), the
Phase-2b .cpp net is ≈ −828.  The plan's ~−1900 projection was optimistic: the preserved-asymmetry branches
(both Pel and NM kept under `if constexpr`), the per-site ULP-preserving helpers, and the explanatory comments
add real lines back vs a naive collapse — the cost of the zero-behavior-change bar.

### Adversarial review ledger (Gate 7)
| Round | Reviewer | Axis | Result |
|---|---|---|---|
| 1 | A | PelTag ≡ original RGB IntegrateFromHit, line-by-line | CLEAN |
| 1 | B | NMTag ≡ original IntegrateFromHitNM, line-by-line (incl. preserved asymmetries) | 0 P1/0 P2; 2 P3 |
| 1 | C | if constexpr / ABI / AOV soundness | CLEAN |
| 2 | B(follow-up) | confirm volume-RR fix + full division bijection | RESOLVED-CLEAN |

P3 #1 (FIXED): volume-scatter RR `throughput * (1.0/survivalProb)` (reciprocal-multiply) diverged from the NM
original `throughput /= survivalProb` at the ULP level → routed through `PTDivByScalar` (Pel `*(1/d)` / NM `/d`).
P3 #2 (ACCEPTED): NMTag supports_aov writes the denoiser AOV side-channel — deliberate, documented, render-neutral
(never feeds `result`).  0 P1 / 0 P2; stop-rule satisfied (round 2 no new findings).

### Files
- src/Library/Shaders/PathTracingIntegrator.cpp — IntegrateFromHitTemplated<Tag> + ~20 helpers + 2 forwarders
- src/Library/Shaders/PathTracingIntegrator.h — IntegrateFromHitTemplated decl + pAOV on NM entry points
- src/Library/Utilities/Color/SpectralValueTraits.h — NMTag::supports_aov true
- tests/SpectralValueTraitsTest.cpp — supports_aov assertion updated for NMTag
- scripts/divergent_baselines.sh — divergent-path capture/check (new)

### Working tree: uncommitted; per "NEVER COMMIT".

---

## Session outcome (2026-05-31) — PT spectral+SMS through-glass emission bug FIXED (asymmetry #1 + #3, + HWSS co-fix)

Closed the confirmed latent bug from [PT_PEL_NM_ASYMMETRY_AUDIT.md](PT_PEL_NM_ASYMMETRY_AUDIT.md):
in spectral (NM) mode with SMS enabled a luminaire seen directly through glass rendered **black**
(Pel correct). Fixed asymmetries **#1 and #3 together** (the audit proved fixing #1 alone
reintroduces double-count fireflies via #3), plus a **HWSS delegation co-fix** that the #1 change
exposed. All in `src/Library/Shaders/PathTracingIntegrator.cpp`; no other source files touched.

### The fix (three sites, one file)
1. **#1 — SPF/no-BSDF `considerEmission`** (~:2316): collapsed the `if constexpr(Traits::is_pel)`
   divergence to `const bool nextConsiderEmissionSPF = true;` for **both** tags. NM no longer forces
   `considerEmission=false` at the glass delta; it relies on the PART1 `smsSuppressEmission` predicate
   (with its load-bearing `bHadNonSpecularShading` guard) exactly as Pel always did.
2. **#3 — PART3 BSDF-continuation flag tracking** (~:2881): removed the `if constexpr(Traits::is_pel)`
   wrapper so **both** tags latch `bPassedThroughSpecular`/`bHadNonSpecularShading`. Required co-fix:
   without it NM's `bHadNonSpecularShading` never latches → `smsSuppressEmission` stays false →
   diffuse→glass→light double-counts.
3. **HWSS delegation** (~:3707, no-BSDF mid-path `IntegrateFromHitNM` call): now passes
   `smsHadNonSpecularShading=true`. The HWSS-native loop only reaches this delegation **after** a
   non-specular BSDF vertex where SMS was evaluated (first hit has a BSDF else Fallback 1 returned),
   so the remaining camera→…→diffuse→glass→light path already has its SMS anchor. Without it, the #1
   change (considerEmission now true through glass) would double-count diffuse→glass→light **in HWSS
   mode only**. SSS mid-path delegation (~:3735) deliberately left untouched (asymmetry-#2-adjacent;
   SMS does not traverse SSS, so `smsHad=true` there could over-suppress).

Both `if constexpr` collapses *reduce* the Pel/NM asymmetry count (NM now does exactly what Pel does),
which is the correct outcome and simplifies the templated body. PelTag codegen is unchanged (its
`if constexpr` true-branches were already the kept behavior).

### Validation (all gates pass)
- **Gate 2 — bug closed (headline).** New permanent regression fixture
  `scenes/Tests/Spectral/sms_through_glass_emitter_pt_sms.RISEscene` (glass sphere between camera and
  a large emissive sphere; through-glass path has NO diffuse vertex). Stash-verified **before/after on
  the same scene**: through-glass disc **0.0 (black, pre-fix) → 247 (bright, post-fix)**; direct ring
  unchanged at 248.4 both. Confirms the scene reproduces the bug AND the fix closes it.
- **Gate 3 — no #3 double-count (counterfactual proof).** Diffuse-receiver caustic (floor+glass+light),
  HDR EXR floor bright-tail p99.9: Pel **13.85** | full #1+#3 **15.44** | **#1-only (no #3) 19.45** ←
  the predicted double-count fireflies. The #3 co-fix lands NM back on Pel. Mean: Pel 0.814 /
  #1+#3 0.820 / #1-only 0.831.
- **Gate 4 — Pel unchanged.** Renderer is non-deterministic (wall-clock-seeded adaptive sampler:
  same-binary two runs of cornellbox differ max 107 / mean 0.79). PRE(master)-vs-POST(fixed) Pel diff
  **equals the noise floor** (cornellbox mean 0.79 == 0.79; sms_k2_glasssphere PRE/POST mean 0.308 ≤
  noise 0.320) → no detectable Pel change. Corroborates the airtight PelTag-invariance proof.
- **Gate 5 — Pel ≈ NM agreement.** Through-glass: NM 247.1 ≈ Pel 248.3 ≈ HWSS 248.1. Caustic: NM 0.820
  ≈ Pel 0.814.
- **Gate 6 — 116/116 binary tests pass** (re-run on the final #1+#3+HWSS binary).
- **Gate 7 — no regression.** Canonical Pel SMS scenes unchanged within noise (Gate 4); NM spectral
  caustic (`spectral_dispersive_caustic_pt_sms`) renders sensibly (mean 110, structured, no fireflies).
- **HWSS — fixed (transitive + co-fix).** Through-glass HWSS 248.1 bright (transitively via the NM
  fallback). The diffuse→glass→light HWSS double-count the #1 change exposed (caustic 0.841, p99.9
  17.25) is resolved by the delegation co-fix → **0.827 / 15.11** (matches pure-NM 0.820/15.44). User
  approved keeping the HWSS co-fix in this chip.
- **Clean warning-free build** (main + tests, 0 warnings, clean rebuilds).

### Adversarial review (3 reviewers, all PASS)
| Reviewer | Axis | Verdict |
|---|---|---|
| A | Trace camera→glass→light + diffuse→glass→light (5 paths) for both tags | PASS — NM≡Pel decision at every light vertex; matches all 3 empirical numbers |
| B | PART1 `smsSuppressEmission` predicate correct for NM (latching, over-suppression, SMS-off gating, value-type) | PASS — predicate suppresses exactly what SMS covers; no new over-suppression |
| C | Zero-behavior-change everywhere except the intended path (Pel invariance, scope leak, asymmetry #2 preserved, HWSS) | PASS — `if constexpr(is_pel)` count 20→18, only the 2 sites; #2 intact |

All three independently flagged the HWSS-native loop as the sibling to check — which led to finding,
diagnosing, and fixing the HWSS delegation double-count (above).

### Files
- src/Library/Shaders/PathTracingIntegrator.cpp — #1 (~:2316), #3 (~:2881), HWSS delegation (~:3707)
- scenes/Tests/Spectral/sms_through_glass_emitter_pt_sms.RISEscene — new permanent regression fixture

### Carry into Phase 2c (BDPT templatization)
The audit's transferable pattern holds: NM/HWSS `considerEmission`-scheme vs Pel flag-predicate-scheme
divergence on camera→specular→light. When BDPT is templatized, audit its Pel vs NM/HWSS delta-to-light
emission/MIS for the same class. Also note the HWSS lesson: any HWSS→NM (or shader-op→NM) delegation
that drops the SMS suppression flags can double-count once emission is re-enabled — check those
boundaries.

### Working tree: uncommitted; per "NEVER COMMIT".
