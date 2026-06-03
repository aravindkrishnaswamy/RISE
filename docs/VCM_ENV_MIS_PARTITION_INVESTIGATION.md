# VCM env-light MIS partition investigation — Session 11 (2026-05-30)

**Audience**: a future supervisor / planning agent staging the SA-MIS
migration ([IMPROVEMENTS.md §12](IMPROVEMENTS.md), [PRE_PHASE1_OPTION_C_DESIGN.md](PRE_PHASE1_OPTION_C_DESIGN.md)).
**Status**: investigation complete; **no code change landed** beyond
keeping Session 10's diagnostic state in-tree (see "Working-tree state"
below).
**Purpose**: tell the next session exactly which hypotheses were
exhaustively tested (saving the iteration cycles), which sites are
proven INNOCENT (saving the wrong-tree time), and where the remaining
22 %-over residual on VCM env+mesh actually lives (env-S0 ↔ env-NEE
MIS partition violation, characterised quantitatively below).

---

## TL;DR

1. **The 22 % over VCM env+mesh and 9 % over VCM env-only have a single
   architectural source**: the MIS partition between the env-S0
   strategy (eye ray escapes to env, Path-B synthetic vertex) and the
   env-NEE strategy (`EvaluateNEE` env branch) sums to **more than 1**
   in a path-dependent way. Both strategies evaluate the SAME physical
   env light via different sampling techniques; the partition is supposed
   to give them MIS-balanced shares but instead grants each ~70 % of the
   converged radiance, so combined they overshoot by ~40 %.
2. **None of the Session 10 micro-changes** (`BDPTVertex::pdfSelect`
   field, `InitLight` `dVC × pdfSelect`, the three `/ pdfSelect` /
   `/ envSelProb` consumer-site divides, env-NEE `invLightSelect` rescale)
   **are the over-count source.** Δ4–Δ7 bisect + this session's A/B
   gate sweep each test as no-op for VCM env+mesh mean within 0.0005
   (linR units on a 32×32×256-spp standalone render).
3. **Splats, mesh-NEE, mesh-S0, interior-connect contributions are all
   tiny** (< 0.005 linR each in env+mesh). Not over-count sources either.
4. The fix is the **monolithic SA-MIS migration** specced in
   [PRE_PHASE1_OPTION_C_DESIGN.md](PRE_PHASE1_OPTION_C_DESIGN.md) — but
   the design's v2 catastrophe modes are now substantially defused by
   the Session 9 continuous-PMF fix. What's left is genuinely just the
   env-vertex pdfFwd/pdfRev SA-vs-disc-area normalization. Detailed
   refined attack plan in **§6 — Refined attack plan for the supervisor
   agent**.

---

## 1. Context — what was already known going in

From [PRE_PHASE1_STATUS.md](PRE_PHASE1_STATUS.md) Session 10:

- VCM env+mesh = 122 % of PT (over).
- BDPT env+mesh = 92 % of PT (closer, passing).
- env-only Lambertian VCM = ~106 % of PT (slight over).
- All other topologies passing at lax `{0.30, 0.35, 1.50}` tolerances.

Session 10 added the JOINT-vs-geometric `pdfSelect` plumbing to
VCMRecurrence and three consumer-site divides at `EvaluateS0` mesh,
`EvaluateS0` env, and `EvaluateNEE` `camFactor`. Result: 127 % → 122 %
(modest improvement, residual unexplained).

Session 10's adversarial reviewer (round 5) flagged the P1 follow-up
diagnostic question:

> "Most likely [the remaining 22 pp gap is] in MIS partition between
> the three env strategies (s=0 env-escape + s=1 env-NEE + t=1 env-
> rooted-splat) — each strategy's `wLight + 1 + wCamera` may not
> include the others, so partition-of-unity isn't guaranteed."

That hypothesis was the Session 11 starting point. The investigation
confirmed it (modulo splats — they turned out empirically negligible)
and pinpointed the partition violation to the env-S0 ↔ env-NEE pair
specifically.

---

## 2. Δ9–Δ12 investigation steps

### Δ9 — s=0 mesh-hit instrumentation

Added env-var-gated `printf` at the s=0 mesh-direct-hit site
([VCMIntegrator.cpp EvaluateS0Impl](../src/Library/Shaders/VCMIntegrator.cpp)
`i != 1` branch). Dumped per-hit: `pdfSelect`, `pdfPosition`, `directPdfA`,
`emissionPdfW`, `dVCM`, `dVC`, `distSq`, `cosE`, `s1Term =
directPdfA × dVCM`, `s2Term = emissionPdfW × dVC`, `wCameraJoint`,
`wCamera` (with the Session 10 `/pdfSelect` divide applied), and the
final weight. Ran on a uniform-env L=1 + scale-10 mesh-emitter quad
scene at 32×32×4 spp.

74 path-length-3 hits at `i=2` were captured. Sample line:

```
VCM_S0  i=2  pdfSel=0.000406  pdfPos=1  dirPdfA=0.000406  emitPdfW=0.000127
        dVCM=54.28  dVC=11.27  distSq=16.6  cosE=0.981
        s1Term=0.0220  s2Term=0.001429  wCamJoint=0.02348  wCam=57.80  w=0.0170
```

### Cross-check: dVCM under continuous PMF

From the log, `bsdfDirPdfW` implied by the dVCM recurrence:
`distSq / (dVCM × cosE) = 16.6 / (54.28 × 0.981) = 0.3122`.
Independent geometric calc (Lambertian, bounce normal +z, direction to
emitter mostly +z): `pdf_BSDF_SA = cosOut/π = (4/√distSq)/π ≈ 0.3122`.
**Agreement < 0.001 % rel-diff on every hit.** The dVCM propagation
under continuous-PMF light selection is mathematically correct. The
"Hypothesis B" prior reviewer concern about a dVCM recurrence bug is
**refuted**.

### Insight from log: Session 10 divide algebra

- `directPdfA × dVCM = 0.0220` matches the JOINT NEE-pdf-SA / BSDF-pdf-SA
  ratio analytically (`directPdfA × distSq / cosE / bsdfDirPdfW`).
- Then current code divides by `pdfSelect = 0.000406` → `wCamera = 57.8`
  → `weight = 0.017` (severe under).
- Removing the divide: `wCamera = 0.0234` → `weight = 0.977` (matches
  analytical balance over the two competing strategies).

Δ4 bisect (Session 10) had already confirmed reverting this divide
moves the standalone VCM mean by only ~+0.027 linR. **So the divide
IS algebraically wrong, but it's covering only ~1.8 % of paths
(74 of 4096 in 32×32×4 spp) so the absolute effect is small.** It's
acting as a small symptom of, not the cause of, the env+mesh 22 % over.

### Δ10 — partition-of-unity sibling instrumentation

Added matching env-var-gated `printf` to `EvaluateNEEImpl` (after
`weight` is set, distinguishing env vs mesh NEE) and
`SplatLightSubpathToCameraImpl`. Same env-var gate `RISE_VCM_S0_DEBUG=1`
enables all three sites. Cap of 5000 lines per site.

Results on env+mesh at 32×32×4 spp:

```
VCM_S0:    74 hits (all i=2 mesh, weights 0.015-0.018)
VCM_NEE:   200 hits (cap; all "light=env", weights 0.7-0.98)
VCM_SPLAT: 191 hits (all i=1, weights 0.003-0.011)
```

200/200 NEE hits being env-NEE confirms `envSelectProb ≈ 0.9996` for
this scene (uniform L=1 over the full sphere dominates the alias
table's total weight). Mesh-NEE hits are theoretically possible but
~0.04 % of NEE samples, so virtually invisible in 4 spp.

Splat weights of 0.003–0.011 are tiny. NEE-env weights of 0.7–0.98
are high.

### Δ11 — exhaustive A/B disable bisect

Added env-var-gated `if (disableX) continue;` skip gates at all
contribution sites in `VCMIntegrator.cpp`:

- `RISE_VCM_DISABLE_S0_ENV` — skip env-direct s=0 contribution
- `RISE_VCM_DISABLE_S0_MESH` — skip mesh-direct s=0 contribution
- `RISE_VCM_DISABLE_NEE_ENV` — skip env-NEE contribution
- `RISE_VCM_DISABLE_NEE_MESH` — skip mesh-NEE contribution
- `RISE_VCM_DISABLE_SPLAT={all,mesh,env}` — skip splats (all, mesh-
  rooted only, env-rooted only)
- `RISE_VCM_DISABLE_INTERIOR` — return zero from
  `EvaluateInteriorConnections`
- `RISE_VCM_NEE_NO_INVSEL` — force `invLightSelect = 1.0` in env-NEE
  contribution (skip the continuous-PMF rescale)
- `RISE_VCM_S0_ENV_NO_DIVIDE` — keep `wCameraEnv` JOINT (skip the
  Session 10 `/ envSelProb` divide)

All gates default OFF; behaviour identical to baseline. Ran the
standalone env+mesh scene at 32×32×256 spp, computing per-channel
mean in linear space (sRGB-decoded from the 8-bit PNG output).

**Results (env+mesh, linR mean per pixel):**

| Disabled | linR | Δ from baseline | Verdict |
|---|---|---|---|
| (baseline, all on) | 0.7419 | — | baseline |
| `S0_MESH` | 0.7416 | −0.0003 | INNOCENT |
| `S0_ENV` | 0.5486 | −0.1933 | **major contributor** |
| `NEE_MESH` | 0.7416 | −0.0003 | INNOCENT |
| `NEE_ENV` | 0.5137 | −0.2282 | **major contributor** |
| `SPLAT=all` | 0.7400 | −0.0019 | INNOCENT |
| `SPLAT=mesh` | 0.7418 | −0.0001 | INNOCENT |
| `SPLAT=env` | 0.7418 | −0.0001 | INNOCENT |
| `INTERIOR` | 0.7419 | 0.0000 | INNOCENT |
| `NEE_NO_INVSEL` | 0.7419 | 0.0000 | divide is no-op (envSelProb ≈ 1) |
| `S0_ENV_NO_DIVIDE` | 0.7419 | 0.0000 | divide is no-op (envSelProb ≈ 1) |
| `S0_ENV` + `NEE_ENV` | 0.0044 | **−0.7375** | **the pair accounts for ~99 % of baseline** |
| ALL OFF | 0.0000 | −0.7419 | partition closure verified |

**Cross-check on env-only Lambertian (no mesh):**

| Disabled | linR | Δ |
|---|---|---|
| baseline | 0.6801 | — |
| `S0_ENV` | 0.5072 | −0.1729 |
| `NEE_ENV` | 0.4119 | −0.2682 |
| `S0_ENV + NEE_ENV` | 0.0166 | −0.6635 |

**Same pattern.** Single-disable drops sum to 0.4411 but both-disable
drop is 0.6635 — ratio 1.50× (linearity violation).

### Linearity violation = partition-of-unity violation

For balance-MIS with partition Σ w_i = α(P) (possibly non-1):

- `baseline = ∫ f × α`
- `disable_i = baseline − ∫ f × w_i`
- `disable_i + disable_j drop = ∫ f × (w_i + w_j)`
- Linear: drops should sum to the both-drop, **independent of α**.

env+mesh: single drops sum = 0.193 + 0.228 = 0.421. Both drop = 0.738.
Ratio: 1.75×. Linear-MIS model REJECTED.

env-only: single drops sum = 0.173 + 0.268 = 0.441. Both drop = 0.664.
Ratio: 1.51×. Linear-MIS model REJECTED.

The Δ9–Δ12 reviewers' explanation (and the only one consistent with
the data): **the env-S0 weight and env-NEE weight on the SAME concrete
path don't independently sum cleanly with the implicit `1` in
`1/(wLight + 1 + wCamera)`**. The two strategies are evaluating the
same env emission via different sampling techniques, but each "self"
position in the partition denominator double-counts the other strategy
when both run together. Disabling one removes that strategy's
contribution AND restores the other's "alone" estimate — a non-linear
relationship.

This is the **disc-area-vs-solid-angle pdf measure mismatch** at the
env vertex that [PRE_PHASE1_OPTION_C_DESIGN.md](PRE_PHASE1_OPTION_C_DESIGN.md)
predicted requires monolithic SA-MIS migration. The Session 9
continuous-PMF fix closed most of the symptom (env+mesh was 24 % UNDER
PT before Session 9; now 22 % OVER), but the partition mismatch at the
env-S0 ↔ env-NEE pair remains.

### Δ12 — adversarial review of the divide-removal hypothesis

Two reviewers in parallel attacked the "remove the Session 10 divide"
hypothesis. Verdicts:

- **Reviewer 1 (math)**: UNBROKEN at conf 0.75. Confirmed dVCM is
  correct under continuous PMF. Confirmed SmallVCM's `GetLightRadiance`
  uses JOINT pdfs (no divide). Confirmed "removing the divide ALONE"
  is the right local fix but its absolute magnitude (~3.5 % per Δ4
  bisect) doesn't explain the 22 % env+mesh over.
- **Reviewer 2 (cross-strategy)**: UNCERTAIN at conf 0.55. Top risk:
  blind divide-removal could unmask deeper InitLight/splat/interior
  bugs. **Recommended: instrument FIRST** — exactly what Δ11 did.

Both reviewers explicitly recommended *instrument before changing
code* — followed.

---

## 3. What the Session 10 divides are actually doing (post-investigation)

Re-reading the Session 10 changes in light of the Δ11 evidence:

| Site | What the divide does | Empirical impact on env+mesh |
|---|---|---|
| `EvaluateS0Impl` mesh `wCameraJoint / pdfSelect` (~line 957) | Converts JOINT `wCamera` to SmallVCM-geometric. **Algebraically wrong** for balance-MIS over JOINT-pdf strategies, but only fires on 1.8 % of paths in env-dominant scenes. | ~+0.027 linR (Δ4 bisect) — small over-correction in the wrong direction. |
| `EvaluateS0Impl` env `wCameraEnvJoint / envSelProb` (~line 858) | Same conversion at env-S0. envSelProb ≈ 1 in env-dominant → no-op. | < 0.001 linR (Δ11). |
| `EvaluateNEE` `emissionPdfW / lightPickProb` for `camFactor` (~line 1235) | Same conversion at NEE. lightPickProb ≈ 1 in env-dominant → no-op. | 0 (Δ6 bisect). |
| `InitLight` `dVC = cosLight × pdfSelect / emissionPdfW` (`VCMRecurrence.cpp:163`) | Multiplies pdfSelect into dVC numerator to extract SmallVCM-geometric from JOINT `emissionPdfW`. Tested no-op in current scene class (Δ7). | 0 (Δ7 bisect). |
| `EvaluateNEE` env-branch `× invLightSelect = 1/ls.pdfSelect` (~line 1335) | Continuous-PMF rescale: compensates the env-NEE contribution for env's selection probability < 1. envSelProb ≈ 0.9996 → no-op. | < 0.001 linR (this session). |

**None of these is the over-count source on the env-S0 ↔ env-NEE pair.**
They're all algebraically self-consistent at `pdfSelect = 1` (preserve
the master baseline of env-only) and have negligible magnitude in
env-dominant scenes.

**Decision (this session)**: leave them in. They follow SmallVCM
convention more carefully than the master baseline; they don't help
on env-IBL specifically; reverting risks regressing on scene classes
the test suite doesn't cover (e.g. mesh-dominant scenes with multiple
mesh emitters where `pdfSelect << 1`). The supervisor agent staging
SA-MIS should consider whether to keep / refactor / revert them as
part of the broader migration.

---

## 4. The actual residual — env-S0 ↔ env-NEE partition mismatch

What the empirical data says:

```
       w_s0_env(P) + w_nee_env(P) + (others tiny) = α(P)

baseline    ≡ ∫ f × α     = 0.742
disable_S0  ≡ baseline − ∫ f × w_s0_env = 0.549  → ∫ f × w_s0_env = 0.193
disable_NEE ≡ baseline − ∫ f × w_nee_env = 0.514 → ∫ f × w_nee_env = 0.228
disable_BOTH ≡ ∫ f × (α − w_s0_env − w_nee_env) = 0.004 ≈ 0
```

A consistent linear model would require single-drops to sum to the
both-drop:

```
single_sum = 0.193 + 0.228 = 0.421
both_drop  = 0.742 − 0.004 = 0.738
ratio      = 1.75
```

The 0.317 excess (= 0.738 − 0.421) is what each strategy's MIS weight
ADDS when the other is removed. In partition-of-unity terms: when both
strategies run, each "thinks" the other has w_alt > 0 and downweights
itself accordingly; when the alt is REMOVED, the surviving strategy's
weight doesn't recover — instead it stays at its "shared" value while
the alt's contribution is gone. The combined estimator over-counts
because the `(wLight + 1 + wCamera)` denominator was sized for full
partition.

**The pdf measure mismatch**: env-S0 uses `directPdfA = envSelProb /
discArea` (area on the projection disc, m⁻²). env-NEE's MIS denominator
sees `directPdfW = ls.pdfPosition × distSq / cosAtLight` (solid-angle).
These two measures are linked by the disc-Jacobian but aren't computed
in a way that closes partition-of-unity. The correct fix is to express
the env vertex's pdfFwd/pdfRev in solid-angle measure and propagate
the SA convention through MIS — exactly what PBRT-v4 does and what
[PRE_PHASE1_OPTION_C_DESIGN.md](PRE_PHASE1_OPTION_C_DESIGN.md) §"Group 3:
VCM caller flags" specs.

---

## 5. What's RULED OUT (don't re-test these in a future session)

These hypotheses were tested via bisects + instrumented A/B in
Sessions 10 + 11. The supervisor agent and any future session can
skip them:

- ❌ "The dVCM recurrence is wrong under continuous PMF" — Δ12
  reviewer Q-A: dVCM-implied `bsdfDirPdfW` matches the analytical
  geometric value to < 0.001 % rel-diff.
- ❌ "The Session 10 dVC × pdfSelect at InitLight is the over-count
  source" — Δ7 bisect: 0 effect.
- ❌ "The S0_MESH `/pdfSelect` divide is the source" — Δ4 bisect:
  0.027 linR effect (small symptom, not cause).
- ❌ "The S0_ENV `/envSelProb` divide is the source" — Δ5 bisect:
  no-op when envSelProb ≈ 1.
- ❌ "The NEE camFactor `/lightPickProb` divide is the source" — Δ6
  bisect: no-op.
- ❌ "The env-NEE `invLightSelect = 1/pdfSelect` rescale is over-
  correcting" — this session A/B test: no-op when envSelProb ≈ 1.
- ❌ "Mesh-side strategies (s0_mesh, nee_mesh) are the source" —
  Δ11 A/B: each < 0.0005 linR.
- ❌ "Splats are the source (env-rooted, mesh-rooted, or any)" —
  Δ11 A/B: ≤ 0.002 linR.
- ❌ "Interior connections are the source" — Δ11 A/B: zero at path
  length 3 (path-length-4 interior connections only fire in deeper
  scenes; verify with a 4-bounce scene if the supervisor wants
  belt-and-suspenders).
- ❌ "Path-tree branching at delta vertices" — excised 2026-05 per
  CLAUDE.md HVF; not a factor.
- ❌ "Cycles-style excludeEnv from BDPT light-subpath" — β experiment
  in Session 10 reverted because BDPT cratered (109 % → 41 % on
  env+omni); BDPT relies on env participating in light-subpath
  generation for Path A s=1 site at BDPTIntegrator.cpp:3897.
- ❌ "Static-cache thread-safety bug" — verified determinism across
  three baseline runs (jitter ≤ ±0.0001 vs 0.32 discrepancy).
- ❌ "PNG quantization / sRGB nonlinearity at 8-bit" — confirmed in
  linear-decoded space; 64 spp and 256 spp give identical pattern.

---

## 6. Refined attack plan for the supervisor agent

The remaining residual is the env-vertex pdf-measure mismatch. Below
is the **delta-from-PRE_PHASE1_OPTION_C_DESIGN.md** the supervisor
should consume.

### 6.1 What's still valid in the v2 design

- The **monolithic-or-skip** rule. Any partial landing breaks
  partition-of-unity in a different place. Skip-or-fully-land.
- The **delta-aware `remap0` precondition** (Piece 2.B'). Already
  landed in Session 7. Don't re-do.
- The **6 BDPT rows** that need SA-pdf normalisation (Path A s=1,
  Path B s=0, Path A interior, etc.). Plan still applies.
- The **OpenPGL guiding audit** dependency — guiding consumes the
  same pdfFwd; any change there must update guiding pdf computation.

### 6.2 What's NEW / changed from v2 design

- **Defused catastrophe**: the Session 9 continuous-PMF fix
  (env-vs-alias roll in `SampleLight`) means env's selection
  probability is no longer 0-or-1 binary. The v2 design's "joint
  landing requires all 6 BDPT rows + VCM caller flags simultaneously
  or BDPT collapses to 36 %" is no longer accurate — the catastrophe
  mode came from binary envSelectProb interacting with partial SA
  landings. With continuous-PMF, partial SA landings should now be
  testable in isolation (one BDPT row at a time, measure on
  EnvLightBalanceTest). This is a significant scope reduction.
- **VCM caller flags scope reduction**: only the env-S0 + env-NEE
  pair needs SA. The mesh-side strategies (S0_MESH, NEE_MESH) are
  empirically innocent; their JOINT-pdf MIS is already partition-
  consistent. Splats and interior connections are also innocent. So
  the VCM SA-MIS migration scope is `EvaluateS0Impl` env branch +
  `EvaluateNEEImpl` env branch — two functions, not the entire
  integrator.
- **New empirical baseline for measurement**: every change must
  measure
  - env-only Lambertian VCM/PT (currently 1.06, target 1.00)
  - env+mesh VCM/PT (currently 1.22, target 1.00)
  - env+omni VCM/PT (currently 1.06, target 1.00)
  - non-uniform env (env_offset topology) VCM/PT (currently 0.89,
    target 1.00 — note this is UNDER, not over)
  - all of the spectral variants
  - All these are reported in `bin/tests/EnvLightBalanceTest` `PrintStats`
    lines (search the test output for "Testing PT-vs-BDPT-vs-VCM:"
    then read the next 3 lines of PT / BDPT / VCM mean values).

### 6.3 Concrete file-level work list

The supervisor agent should sequence these as one atomic landing:

1. **env-S0 weight construction** at
   [VCMIntegrator.cpp EvaluateS0Impl env-branch](../src/Library/Shaders/VCMIntegrator.cpp)
   ~lines 770–870. Replace the disc-area `directPdfA = envSelProb / discArea`
   construction with SA-measure `directPdfA_SA = envSelProb × pdf_env_sa(wi)`,
   and `emissionPdfW_SA = directPdfA_SA × 1` (the disc-area Jacobian
   factor `1/discArea` cancels in the SA convention). Then `wCameraEnv`
   uses SA-measure pdfs throughout. The Session 10 `/envSelProb` divide
   becomes unnecessary at this site.
2. **env-NEE weight construction** at
   [VCMIntegrator.cpp EvaluateNEEImpl env-branch](../src/Library/Shaders/VCMIntegrator.cpp)
   ~lines 1228–1295. The env case currently has `directPdfW` computed
   in SA already (good) but `emissionPdfW` is the JOINT
   `pdfSelect × pdfPosition × emissionDirPdfSA`. Normalize to SA
   convention matching the env-S0 site. The Session 10 `emissionPdfW /
   lightPickProb` divide becomes unnecessary.
3. **BDPT env-vertex pdfFwd / pdfRev** at the corresponding sites in
   [BDPTIntegrator.cpp](../src/Library/Shaders/BDPTIntegrator.cpp).
   The 6 sites enumerated in PRE_PHASE1_OPTION_C_DESIGN.md "Group 2"
   need SA-measure pdfs on env vertices. Use
   `BDPTVertex::IsInfiniteLight()` (already landed Phase 1.A) and
   `BDPTUtilities::ConvertDensity` (also landed) to migrate each
   call-site.
4. **OpenPGL guiding pdf** — any place that reads `pdfFwd` on a
   light-subpath env vertex must use the SA value too. Audit
   [src/Library/Guiding/](../src/Library/Guiding/) for env pdf consumers.
5. **Re-evaluate Session 10 divides**: with SA convention, are the
   `/pdfSelect` etc. divides still needed for the JOINT-vs-geometric
   conversion at the consumer sites, or can they be removed cleanly?
   Probably the latter — Session 10's divides exist because the
   storage convention forced a runtime conversion; if storage is SA,
   the conversion is built into the construction.

### 6.4 Validation gates (this session's empirical baseline)

Before / after every step, run:

```sh
make -C build/make/rise -j8 all && ./run_all_tests.sh
./bin/tests/EnvLightBalanceTest 2>&1 | grep -E "^  (PT|BDPT|VCM) "
```

Expected: 116/116 binary tests pass; EnvLightBalanceTest at lax
`{0.30, 0.35, 1.50}` 80/80; strict `{0.10, 0.30, 1.00}` improves from
current 78/80 toward 80/80 as the env-S0/NEE partition closes.

A failing intermediate state at lax is a STOP gate — revert and
re-think. The "partial landing breaks partition" rule still applies
(albeit more leniently now that continuous-PMF defused the worst
catastrophes).

### 6.5 K-trial variance gate

Per [skills/variance-measurement.md](skills/variance-measurement.md):
run `bin/tools/HDRVarianceTest.exe` master-vs-fix on the env+mesh
scene at matched samples. SA-MIS migration should REDUCE variance (more
balanced strategy weights → better MIS gain), not increase it. If
variance goes UP, partition is still over-counting somewhere — back to
instrumentation.

---

## 7. Working-tree state (what's currently in-tree as of this doc's
write-time)

All Session 10 changes (BDPTVertex pdfSelect field + InitLight dVC ×
pdfSelect + the three consumer-site divides) remain in-tree, unmodified.

All Session 11 diagnostic instrumentation (env-var-gated printfs and
A/B disable gates) was **stripped from VCMIntegrator.cpp** before this
doc was written. The file is back to the Session 10 state.

The standalone debug scenes at `/tmp/vcm_s0_envmesh_debug.RISEscene`
and `/tmp/vcm_s0_envonly_debug.RISEscene` are scratch and can be
deleted by the user.

`EnvLightBalanceTest` should pass at lax `{0.30, 0.35, 1.50}` with all
116 binary tests; verify with `./run_all_tests.sh` and the explicit
PrintStats grep above.

---

## 8. Open questions for the supervisor agent

The investigation answered the user's "where is the over-count
sourced" question conclusively. Questions the supervisor needs to
make a judgement call on before staging SA-MIS work:

1. **Scope decision**: do a minimal SA-MIS migration (just env-S0 +
   env-NEE in VCM, plus the 6 BDPT rows) or a broader pdf-measure
   harmonisation (also normalize mesh-side conventions)? The
   empirical data says minimal-scope is sufficient for the
   EnvLightBalanceTest delta, but a broader audit might prevent
   similar regressions on future scene classes.
2. **Keep / revert Session 10 micro-changes during the migration**?
   The investigation shows they're not actively helping but also not
   actively hurting on the test class. The cleanest path may be to
   revert them as part of the SA migration so the diff is cohesive.
3. **K-trial reference scene**: which production scene should be the
   K-trial reference for SA-MIS variance validation? The user
   mentioned `ripple_dreams_fields.RISEscene` as their production
   scene class. A representative env-IBL + non-trivial geometry
   scene is needed for the perf / variance baseline.
4. **HDRVarianceTest sequencing**: per `skills/variance-measurement.md`,
   measure variance BEFORE landing any code change, then AFTER, on
   the same scene at matched seeds. The Session 11 investigation
   used standalone PNG render diffing which is fine for finding
   where contributions land but not for quantifying variance. The
   supervisor should plan for K-trial gates per-step.
5. **Timeline budget**: PRE_PHASE1_OPTION_C_DESIGN.md scoped 3 weeks
   for the full Piece 1 SA-MIS migration. With the catastrophe modes
   defused by Session 9 and the scope reduction from this session's
   evidence (only 2 VCM sites + 6 BDPT sites + OpenPGL audit), the
   budget might be ~1 week of focused work. Recommended: ask the user
   for an explicit go-ahead with a budget cap before staging.

---

## Authoritative reference

For the underlying theory and the v2 attack plan that this
investigation refines:

- [PRE_PHASE1_OPTION_C_DESIGN.md](PRE_PHASE1_OPTION_C_DESIGN.md) — original v2 spec
- [PRE_PHASE1_STATUS.md](PRE_PHASE1_STATUS.md) — Sessions 1–10 historical record (Session 11 to be appended)
- [IMPROVEMENTS.md §12](IMPROVEMENTS.md) — Phase 1 canonical spec; status field references this doc
- [VCM.md](VCM.md) — Georgiev 2012 recurrence reference + env-vertex relevant sections
- [skills/bdpt-vcm-mis-balance.md](skills/bdpt-vcm-mis-balance.md) — MIS regression diagnostic procedure
- [skills/variance-measurement.md](skills/variance-measurement.md) — K-trial EXR protocol for any variance claim
- [skills/adversarial-code-review.md](skills/adversarial-code-review.md) — multi-reviewer pattern (used twice this session)
- [src/Library/Shaders/VCMRecurrence.h](../src/Library/Shaders/VCMRecurrence.h) — InitLight contract, SmallVCM recurrence formulas
- [src/Library/Lights/LightSampler.cpp](../src/Library/Lights/LightSampler.cpp) — continuous-PMF `SampleLight` wrapper (Session 9 fix)


---

## Session 12 outcome (2026-06-03) — minimal SA-measure fix FALSIFIED; phantom s=2 light-tracing MIS term + ~5% contribution residual identified

**Status**: investigation complete; **NO code change landed** (working tree
reverted to pristine baseline; `EnvLightBalanceTest` 80/80 lax, 116/116 binary
tests). The §6 minimal-scope prescription (env-S0 + env-NEE SA-measure
migration in VCM) was implemented exactly, **empirically falsified by the
oracle**, and reverted per the stop rules. This section corrects the §4/§6
root-cause model on two specific points.

### TL;DR
1. The §6.3 minimal VCM fix (SA-measure env density at the env-S0 and env-NEE
   sites, matching SmallVCM line-for-line) **regresses** `EnvLightBalanceTest`
   from 80/80 to **78/80** lax — it over-corrects uniform-env VCM from ~+6%
   over to ~-18% under and craters hwss=true to 0.45. This is the v1/v2
   regression signature.
2. Direct-render isolation shows **the MIS partition is NOT the dominant
   problem**. The over-correction is a **phantom s=2 light-tracing term in the
   MIS denominator**: RISE's env light-subpath connection contributes ~0
   (Session-11 Δ11: splat ≤ 0.002), yet the SmallVCM denominator
   (`emissionPdfW·dVC` in env-S0, the `wCamera` term in env-NEE) assumes it
   contributes — suppressing `w_s0 + w_s1` below 1. The disc-area measure was
   *accidentally compensating* for it.
3. A **separate ~5% contribution-level under-estimate** remains (independent of
   MIS), with a likely test-setup confounder (PT vs VCM use different pixel
   filters in `EnvLightBalanceTest`).
4. **Neither variant is landable.** SA-alone regresses; SA + s2-term-exclusion
   is a non-principled hack that would break VM/caustic env scenes.

### What was implemented (then reverted)
SA-measure construction in the env branch of `EvaluateS0Impl` and
`EvaluateNEEImpl`, matching SmallVCM `BackgroundLight::GetRadiance` /
`Illuminate` + `VertexCM::GetLightRadiance` / `DirectIllumination`:
- env-S0: `directPdfA = envSelProb · pdf_env_sa` (solid-angle), `emissionPdfW =
  directPdfA / discArea`; dropped the Session-10 `/envSelProb` divide (subsumed).
  (was disc-area `directPdfA = envSelProb / discArea`.)
- env-NEE: `directPdfW = pdf_env_sa` (solid-angle, directly). (was disc-derived
  `pdfPosition · distSq / cosAtLight`.)
Verified the construction matches SmallVCM line-for-line; the env-NEE
*contribution* (PT-style `cosEyeWi/pdfSA · invLightSelect`) was already
SA-correct and unchanged.

### Result — REGRESSION (oracle: EnvLightBalanceTest, freshly-relinked binary)
| Topology (VCM/PT mean R) | baseline | SA-measure | SA + s2-excl |
|---|---|---|---|
| env-only Lambertian | 1.057 | 0.819 | 0.894 |
| env + omni | 1.056 | 0.824 | 0.898 |
| env + mesh | 1.282 | 0.792 | 0.817 |
| env-only spec hwss=F | 1.038 | 0.795 | 0.877 |
| env-only spec hwss=T | 0.822 | **0.453** | **0.492 (LAX FAIL)** |
| non-uniform x3 | ~0.888 | ~0.93 | ~0.947 |

- **SA-measure alone**: lax 80/80 → **78/80** (hwss=true VCM mean + p99 fail).
- **SA + s2-term exclusion** (also oracle-measured): improves the uniform
  topologies (0.819 → 0.894 env-only), confirming the phantom-s2 finding — **but
  is STILL 78/80** (hwss=true 0.492 lax-fails) and still ~10-18% under on uniform.
  **No SA-measure variant even passes lax.**

Decisive takeaway: the disc-area baseline (80/80) is empirically CLOSER to PT
than any SA-measure variant, *despite* being "theoretically wrong" — its measure
errors compensate for deeper under-counting (phantom-s2 term + ~5-10%
contribution residual + a severe HWSS-specific breakage where the SA migration
roughly HALVES hwss=true, 0.82 → 0.45-0.49). All variants reverted.

### Root cause — direct-render isolation (env-only Lambertian, `bin/rise`)
- my-PT = 0.6643; my-VCM (SA + s2-term excluded) = 0.6293 → **0.947 (5% under)**.
- Isolation (MIS weight forced to 1 per strategy):
  - env-S0 only = 0.6292
  - env-NEE only = 0.6296
  - both with MIS = 0.6293  — **all equal.**
- Interpretation: each env strategy ALONE is a *complete* unbiased estimator
  (= 0.629); MIS combines two equal estimators correctly. The 18%-under of the
  SA fix is the phantom s=2 denominator term suppressing `w_s0 + w_s1` below 1.
  Excluding it recovers env-only from 0.819 → 0.947.

### Two corrections to the §4 / §6 root-cause model
1. **"Splats / interior connections are innocent" (§5) is true for the
   CONTRIBUTION but FALSE for the MIS DENOMINATOR TERM.** Δ11's disable-bisect
   removed *contributions*, never *denominator terms*. The phantom denominator
   term (`emissionPdfW·dVC`, `wCamera`) IS the dominant over-suppressor once the
   disc-area measure (which masked it) is corrected to SA.
2. **"Pure SA-measure migration closes the partition" (§4, §6.2) is FALSE.**
   SA-measure alone over-corrects (lax fail) by exposing the phantom term.

### Residual ~5% (separate from MIS — needs its own investigation)
Even with the s2 term excluded and weights forced to 1, each VCM env strategy
is ~5% under PT — a contribution-level under-estimate, common to both
strategies, independent of MIS. **Confounder (likely minor)**:
`EnvLightBalanceTest` renders PT with the default (Mitchell) pixel filter but
VCM with `pixel_filter box` — not apples-to-apples at the filter level. However,
**BDPT also uses `box`** and tracks PT within ~3% (env-only BDPT/PT = 1.09,
env+mesh 0.97), so the filter mismatch accounts for at most ~3%; most of the VCM
5-10% residual is likely VCM-specific. Matched-filter re-measurement should be
the first step to isolate the genuinely-VCM portion before treating it as a bug.

### Why neither variant is a landable fix
- SA-measure alone: lax regression. NO.
- SA + s2-term exclusion: non-principled hack. The crude implementation zeroes
  the whole `wCamera` for env-NEE, which also removes the **VM merge**
  alternative (`mMisVmWeightFactor`) — would double-count merges in VM-enabled
  env caustic scenes (over-bright). A principled version must drop ONLY the env
  light-tracing CONNECTION terms while keeping the merge term, and be validated
  on VM/caustic scenes — beyond a gated single-step session.

### Recommended next steps (for a properly-scoped follow-up)
1. **Resolve the filter confounder**: make `EnvLightBalanceTest` use the SAME
   pixel filter for PT and VCM, then re-measure the residual. Part of the
   "VCM under/over PT" signal may be a test artifact.
2. **Decide the principled treatment of RISE's near-zero env light-tracing
   connection**: either (a) make it contribute its proper share (so the MIS
   denominator term is balanced — a larger change to env light-subpath
   connection handling), or (b) surgically exclude ONLY the env light-tracing
   CONNECTION MIS terms (keep the VM merge term), validated on VM/caustic scenes.
3. Resolve any remaining contribution-level residual after (1).
4. Re-run the SA-measure migration on that corrected foundation (it is correct
   for the s0↔s1 ratio; it just needs the phantom-term + residual fixed first).

### Process note (important for the next session)
`make all` rebuilds the library + `bin/rise` but does **NOT** relink test
binaries — `make tests` is required. Mid-investigation measurements that ran
`./bin/tests/EnvLightBalanceTest` after only `make all` used a STALE binary and
gave misleading results; this temporarily masked the phantom-s2 finding until
caught. Always `make all && make tests` before running the test; `bin/rise` is
fresh after `make all` alone (used for the direct-render isolation above).

### Working-tree state
Pristine baseline (Session-10 micro-changes remain in-tree, unchanged, as they
were committed). No instrumentation, no SA changes, no commits.

---

## Session 13 outcome (2026-06-03) — gated prerequisite chain executed; SA-MIS re-attempt NOT undertaken; well-evidenced recommendation to STOP

**Status**: gated diagnosis complete; **NO SA-measure code written** (integrator /
library byte-identical to HEAD); ONE optional test-harness improvement landed in the
working tree (`pixel_filter box` added to the two PT rasterizer strings in
`tests/EnvLightBalanceTest.cpp` — removes the documented filter confounder, still
80/80 lax, 116/116 suite). `EnvLightBalanceTest` 80/80 lax, 75/80 strict; 116/116
binary tests pass. This session executed the 4-step gated prerequisite chain the
Session-12 "Recommended next steps" laid out. **Two of the gates (Step 2 scope, Step 3
HWSS) trigger the spec's STOP rules. The SA re-attempt (Step 4) was therefore not
undertaken — by design of the gated structure, which exists to avoid a 5th failed
attempt.** Both the phantom-term and HWSS findings are corrected/sharpened below.

### TL;DR
1. **Filter confounder (Step 1) accounts for ~0 % of the VCM residual** — stronger
   than Session-12's "≤ 3 %" prediction. Matching `pixel_filter box` across PT / BDPT /
   VCM moves every VCM/PT mean ratio by < 0.6 % (within run-to-run jitter); all 5
   strict failures are on the MEAN, which a normalized reconstruction filter preserves
   exactly. The whole 5–28 % VCM residual is genuinely VCM-specific.
2. **The phantom-term root cause (Step 2) is neither (a) deficient generation nor
   (b) a non-existent strategy — it is (c): the env light-tracing strategy EXISTS,
   is generated by the (shared, healthy) `GenerateLightSubpath`, FIRES, and is
   correctly MIS-weighted near-zero (matching SmallVCM for a pinhole camera + diffuse
   surface).** The Session-12 "phantom s=2 term" is a **measure-inconsistency artifact
   of a PARTIAL (eye-side-only) migration**, not a spurious term. The principled fix is
   **MONOLITHIC** (both subpath sides + the shared generator's env-vertex pdf convention
   + the 6 BDPT rows + OpenPGL), NOT the 2-function surgical edit the Session-11 §6.2
   scope reduction claimed. → large/architectural → **STOP-and-report-scope** (Step-2
   gate's (a)-branch outcome).
3. **The HWSS breakage (Step 3) is a PRE-EXISTING spectral-bundle env-IBL bias**
   (hwss=true env-only is **18 % under PT at the disc-area baseline already** — 0.820 vs
   hwss=false 1.016 — a 20 % HWSS-specific gap on the same achromatic uniform-env scene,
   128 spp, reproduced 2×, not noise), **independent of any SA work**. SA's down-shift
   compounds with it multiplicatively, cratering hwss=true to 0.45–0.49 — a **lax
   FAILURE** that persists even with the s2 term excluded. Fixing HWSS env-IBL is a
   **separate spectral-bundle workstream** (same class as the PT_PEL_NM_ASYMMETRY
   spectral bugs). Without it fixed FIRST, **no SA migration can pass the non-negotiable
   lax gate**. → **STOP** (Step-3 gate).
4. **Recommendation: accept the disc-area baseline for Phase 1 measurement.** It is
   80/80 lax, production-working, empirically closest to PT (Session 12), and its
   residual is bounded (strict-only, ~11–28 %). The real fix is a multi-week monolithic
   migration gated behind a separate HWSS prerequisite — not justified for a bounded
   strict-only residual given the 4-attempt failure history.

### Step 0 — baseline (reproduced; matches Session-12 baseline column)
`make all && make tests`, then `EnvLightBalanceTest`. Lax **80/80**; strict
`{0.10,0.30,1.00}` **75/80** (5 fails, ALL `VCM.mean`). Per-topology VCM/PT mean:

| Topology | VCM/PT | BDPT/PT | direction |
|---|---|---|---|
| env-only Lambertian (RGB) | 1.057 | 1.091 | over |
| env + omni | 1.055 | 1.090 | over |
| env + mesh | **1.282** | 0.972 | **over (strict fail)** |
| env-only spec hwss=F | 1.016–1.020 | 1.074 | ~ok |
| env-only spec hwss=T | **0.820** | 0.924 | **under (strict fail)** |
| non-uniform RGB | **0.888** | 0.989 | **under (strict fail)** |
| non-uniform spec hwss=F | **0.880** | 0.998 | **under (strict fail)** |
| non-uniform spec hwss=T | **0.891** | 0.997 | **under (strict fail)** |

(The doc's earlier "78/80 strict" applied strict tol only to the uniform topologies;
applying strict uniformly gives 75/80. Same signal either way.)

**Key new observation — the bias is BIDIRECTIONAL and structured:**
- The disc-area VCM bias is **over** on uniform-env over-cases (env+mesh +28 %,
  env-only +5.7 %) and **under** on non-uniform (−11 to −12 %) and hwss=true (−18 %).
- Cross-referenced with Session-12's SA-measure column, SA is **directionally correct
  for the non-uniform case** (0.888 → 0.93 → 0.947, improving toward 1.0 — it fixes the
  real disc-position-vs-directional-pdf mismatch), **overshoots the uniform over-cases**
  to under (1.057 → 0.819; 1.282 → 0.792 — the partial-migration phantom effect), and
  **craters hwss=true** (0.820 → 0.453, lax fail — the separate HWSS breakage).
- So "SA shifts everything down" is too crude. SA is the *right direction* for the
  non-uniform under-count but exposes (i) the partial-migration phantom suppression on
  uniform scenes and (ii) the pre-existing HWSS bias.

### Step 1 — filter confounder resolved (~0 % of the residual)
`EnvLightBalanceTest` rendered PT with the default `gaussian` (2-pixel) filter while
BDPT/VCM used `box`. Added `pixel_filter box` to `kRasterizerPT` and
`kRasterizerPTSpectral` (2 lines). Re-measured: VCM/PT ratios change by < 0.6 % on every
topology (env-only 1.057→1.056, env+mesh 1.282→1.282, hwss=T 0.821→0.820, non-uniform
0.888→0.888); the 5 strict failures are byte-for-byte the same topologies. **A
normalized reconstruction filter preserves the per-image mean; all strict failures are
on the mean; therefore the filter cannot be the cause.** Still 80/80 lax with the
change. The change is a legitimate apples-to-apples test improvement worth keeping
independent of the rest (kept in the working tree, easily reverted).

### Step 2 — phantom-term root cause: it is (c), and the fix is monolithic
**The gate asks (a) deficient generation vs (b) non-existent strategy. The evidence
says NEITHER — it is (c).**

- **Rules OUT (a) — generation is healthy and SHARED with BDPT.** The env light
  subpath is generated by `GenerateLightSubpathImpl` (`BDPTIntegrator.cpp:4763`): env
  light vertex 0 is created with `v.pEnvLight` (line 4882) and the emission ray is
  traced into the scene (line 4924). This generator is **shared with BDPT**. BDPT tracks
  PT within ~9 % on every env topology (table above: env-only 1.091, env+mesh 0.972,
  non-uniform 0.989–0.998). If the env generation were deficient, BDPT would suffer too
  — it does not. The bias is **specific to VCM's balance-MIS env-vertex weight
  construction**, not generation.
- **Rules OUT clean (b) — the strategy exists and fires.** Env-rooted light-subpath
  downstream vertices ARE splatted (`SplatLightSubpathToCameraImpl` line 1495 skips only
  vertex 0; downstream surface bounces splat) and interior-connected
  (`EvaluateInteriorConnectionsImpl` line 1728 iterates light vertices from `i=1`). The
  code paths are reached for env-rooted subpaths.
- **It is (c) — correctly near-zero.** The splat weight is
  `1 / (1 + (cameraPdfA / mLightSubPathCount)·(mMisVmWeightFactor + dVCM + dVC·bsdfRevPdfW))`.
  For a **pinhole** camera `cameraPdfA` is enormous → splat weight ≈ 0. This is the
  *same* mechanism by which SmallVCM downweights t=1 light→camera connections for a
  pinhole + diffuse surface. Δ11 (Session 11, ruled-out to re-test) measured these
  contributions at ≤ 0.002 linR. The near-zero is **correct**, not a bug.

**Why the SA migration then under-counts (the corrected mechanism).** The env-S0 and
env-NEE *eye-side* denominators carry terms for the light-tracing alternatives
(`emissionPdfW·dVC` in env-S0, the `wCamera = camFactor·(… + dVCM + dVC·bsdfRevPdfW)`
in env-NEE). Under disc-area these terms are tiny (the env-NEE `directPdfW ≈ 1/π`
disc-derived keeps `camFactor` small; the env-S0 `emissionPdfW` carries a `1/discArea`
factor), so the eye-side weights sum to ≈ 1 and the light-tracing contributes ≈ 0 —
consistent. Session-12 migrated ONLY the eye side to SA: `directPdfW = pdf_env_sa`
(≈ 4× smaller for uniform env) inflates `camFactor`, so the eye-side denominators now
**reserve real MIS mass for the light-tracing strategies**. But those strategies' OWN
weights are still computed in disc-area (their `lightMis` is seeded by
`InitLight` at the env vertex using the disc-area `v.pdfFwd` / `v.emissionPdfW` stored
by the shared generator — `VCMRecurrence.cpp:148/163`, `ConvertLightSubpath`
`VCMIntegrator.cpp:559`, with the env vertex hardcoded `isFinite=true` /
`applyDistSqToDVCM=true`, diverging from SmallVCM's infinite-light gate). So they STILL
contribute ≈ 0, leaving the reserved mass uncounted → the 18 % under-count. **It is a
partial-migration measure inconsistency, not a spurious denominator term.**

**Consequence for the fix (the Step-2 gate result).** The principled fix is NOT the
"surgical term-exclusion keeping the merge term" (option-b's fix) — excluding the term
would re-break partition the other way and, per Session 12, double-count merges in
VM-enabled env caustic scenes. Nor is it "generation correction" (option-a's fix) —
generation is healthy. It is the **MONOLITHIC** migration the original
`PRE_PHASE1_OPTION_C_DESIGN.md` mandated: migrate the env-vertex pdf convention
**consistently across both subpath sides** —
(i) eye-side `EvaluateS0Impl` / `EvaluateNEEImpl` env branches,
(ii) light-side `InitLight` env-vertex seeding (+ the finite/infinite gate) which feeds
splat + interior,
(iii) the SHARED `GenerateLightSubpath` env-vertex pdf storage (`pdfFwd`,
`emissionPdfW`) — which also feeds BDPT, so the **6 BDPT rows** must migrate in lockstep
(prior attempts' catastrophe mode was exactly partial SA landings interacting with
BDPT), and
(iv) the OpenPGL env-pdf consumers.
The **Session-11 §6.2 "scope reduction to 2 VCM functions" was WRONG** — it inferred
the light side was innocent from disc-area Δ11 data (where light-tracing legitimately
contributes ≈ 0), but under SA the light side must migrate too or the partition does not
close. This is a multi-week, cross-integrator change → **large/architectural →
STOP-and-report-scope** per the Step-2 gate.

### Step 3 — HWSS root cause: a pre-existing spectral-bundle env-IBL bias (separate workstream)
- **The HWSS env under-count exists at the disc-area BASELINE, before any SA change.**
  hwss=true env-only VCM/PT = **0.820** vs hwss=false **1.016** — a 20 % HWSS-specific
  gap on the *same* achromatic uniform-env scene at 128 spp, reproduced across two runs.
  For a uniform white env the bundle's hero and companion wavelengths should give the
  same value (achromatic, non-dispersive), so this 18 % deficit is a real spectral-bundle
  bias, not bundle variance.
- **It is concentrated on the uniform env-only case.** hwss=true non-uniform (0.891)
  MATCHES RGB non-uniform (0.888); the HWSS-specific deficit only appears on uniform
  env-only. It also affects **BDPT** hwss=true env-only (0.924, 8 % under) less than VCM
  (0.820, 18 % under) → a **shared spectral-bundle env issue** (in the
  `GenerateLightSubpath{,NM}` / HWSS companion re-weighting,
  `VCMSpectralRasterizer.cpp:483-565` companion loop) plus a VCM-specific extra.
- **SA compounds it to a lax failure.** Session-12 measured SA → hwss=true 0.453, and
  SA + s2-exclusion → 0.492 — both lax FAILURES (> 30 % band) and both **independent of
  the phantom term** (the s2 exclusion does not rescue it). The mechanism is
  multiplicative: SA's env-weight down-shift applied on top of the already-18 %-under
  bundle.
- **Conclusion (Step-3 gate):** HWSS env-IBL needs its **own** spectral-bundle
  workstream, BEFORE any SA migration — otherwise SA cannot pass lax via hwss=true.
  → **STOP**.

### Decision — STOP (well-evidenced), accept the disc-area baseline for Phase 1
Multiple spec STOP rules are triggered:
- *"phantom-term root cause … large/architectural … report the scope, don't undertake
  it blind"* — the fix is monolithic (both subpath sides + shared generator + 6 BDPT
  rows + OpenPGL), not the 2-function surgical edit.
- *"The HWSS mechanism needs its own workstream"* — pre-existing spectral-bundle bias;
  SA fails lax via hwss until it is fixed first.
- *"cumulative effort/risk exceeds what's justified for a bounded … residual"* — the
  residual is bounded (~11–28 %, strict-only), the disc-area baseline is 80/80 lax,
  production-working, and (Session 12) empirically closest to PT.

**This is a SUCCESS outcome for the chip per its charter** ("a well-evidenced 'don't do
this' is a SUCCESS"). The disc-area baseline is an acceptable strictly-bounded
foundation; Phase 1 can measure on it. The user decides whether to invest in the
HWSS-then-monolithic path for strict closure.

### The real-fix scope, if strict closure is later prioritized (for a future team)
Ordered prerequisite chain (do NOT attempt out of order — the lax gate fails otherwise):
1. **Fix the pre-existing HWSS env-IBL spectral-bundle bias** (separate workstream;
   target hwss=true env-only 0.820 → ~1.0 at disc-area, and confirm it tracks RGB).
   This is the same diagnostic family as PT_PEL_NM_ASYMMETRY_AUDIT.
2. **THEN the full MONOLITHIC SA migration** — env-vertex pdf in SA across: VCM
   `EvaluateS0Impl`/`EvaluateNEEImpl` env branches; VCM light-side `InitLight` env vertex
   + finite/infinite gate (→ splat + interior); the shared `GenerateLightSubpath`
   env-vertex `pdfFwd`/`emissionPdfW`; the 6 BDPT "Group 2" rows
   (`IsInfiniteLight()` + `ConvertDensity` helpers, already in tree); OpenPGL env-pdf
   consumers. Land with a lax gate after each sub-step; STOP-and-revert on any lax
   regression. Expect SA to fix env+mesh / env-only over-counts and *help* non-uniform,
   landing near 1.0 — but only after step 1, or hwss craters.
3. K-trial variance gate + VM/caustic env scene validation per §6.5 / §6 of this doc.

### What this session did NOT do (and why)
- Did **not** write any SA-measure code (the Step-2/Step-3 gates both said STOP; the
  gated structure exists to avoid a 5th catastrophic attempt). The Session-12 SA
  regression was not re-reproduced because the Step-0 baseline reproduces Session-12's
  baseline column exactly (so the tree is at the same state Session-12 measured from),
  and the linchpin — SA fails lax via hwss — is grounded in the **pre-existing**
  HWSS bias measured independently this session (SA can only worsen an already-18 %-under
  case).
- Did **not** revert the test `pixel_filter box` change (the spec blesses keeping it; it
  removes a confounder and aids Phase-1 apples-to-apples measurement). Integrator /
  library remain byte-identical to HEAD.

### Working-tree state (Session 13)
- `src/**` (integrator, recurrence, generator, lights, guiding): **byte-identical to
  HEAD** (`git diff --name-only -- 'src/**'` is empty).
- `tests/EnvLightBalanceTest.cpp`: +2 lines (`pixel_filter box` in the two PT rasterizer
  strings) — optional, blessed, easily reverted (`git checkout -- tests/EnvLightBalanceTest.cpp`).
- `EnvLightBalanceTest` 80/80 lax; 116/116 binary tests pass. No commits, no stages.
