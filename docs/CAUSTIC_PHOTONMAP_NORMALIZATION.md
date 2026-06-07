# Caustic photon-map normalization fix — and why VCM was *not* over-counting (2026-06-06)

> ⚠️ **SUPERSEDED / WITHDRAWN — read [§11](#11-resolution-2026-06-06-gated-fix-session--there-is-no-vcm-over-count-the-entire-diagnosis-was-a-chain-of-measurement-artifacts-vcm-matches-unbiased-references-to-3-) FIRST.**
> §1–§10 below conclude VCM's merge over-counts refractive caustics. **That
> conclusion is wrong.** A gated fix attempt (same day) showed every reference
> used in §1–§10 is biased *low* — the controlled "flat dielectric" scene
> actually measures the **Fresnel surface reflection of the light** (not the
> caustic), the caustic **photon map under-counts ~2×**, and the pool
> BDPT/PT references **cannot reach the delta-light caustic through the water
> surface**. Measured against unbiased references (PT for area lights; BDPT's
> light-tracing splat for delta lights, camera placed *under* the water), the
> shipped **VCM is correct to ≤3 % (0.1 % on the pool-regime caustic)**. **No
> over-count exists; no fix was or should be implemented.** §1–§10 are retained
> only as the record of the misdiagnosis.

**Verdict: the pool-caustic scene's VCM-vs-BDPT brightness gap is a VCM
*vertex-merging (VM) over-count bug*, NOT "VCM captures real L-S-D-S-E
transport the others miss."**  VCM's merge over-counts the caustic by
**~1.6× vs an unbiased path-traced ground truth** (area-light variant) and
by **~3.3× vs the caustic photon map** on the pure delta-spot primary
caustic.  This *overturns* the first-order "working-as-intended / transport
coverage" reading of `pool_caustics`.

No integrator code was changed (this is a diagnosis + reproduction + root-cause
hand-off, per [docs/skills/bdpt-vcm-mis-balance.md](skills/bdpt-vcm-mis-balance.md)).

Scene: [scenes/Tests/VCM/pool_caustics_vcm.RISEscene](../scenes/Tests/VCM/pool_caustics_vcm.RISEscene).
Independent reference: [scenes/FeatureBased/Caustics/pool_caustics.RISEscene](../scenes/FeatureBased/Caustics/pool_caustics.RISEscene)
(caustic photon map, `power_scale 5.0`).

---

## 1. TL;DR / evidence ledger

Two delta spot lights over a Gerstner-displaced dielectric (ior 1.33) water
surface, white-tile floor.  The underwater illumination is the L-S(water)-D(floor)-S(water)-E
caustic.  All renders: matched single still, 400×300, `pixel_filter box`,
OIDN off, float-EXR, read with pyOpenEXR.  Luminance = 0.2126R+0.7152G+0.0722B.

| Test | Result | Implication |
|---|---|---|
| **Area-light PT ground truth** (caustic directly samplable; PT converged s64≡s1024 @0.1%) | **VCM 1.60× PT(GT)**; BDPT 1.04× PT(GT) | VCM over-counts vs an estimator that uses **no density estimation at all** |
| Caustic photon map (delta scene), courtyard-calibrated | VCM underwater **4.0×** photon-primary; photon ≈ BDPT (0.088 ≈ 0.091) | VCM is the outlier; two independent refs agree |
| Strategy isolation: VC-only (vm off) | underwater 0.0925 ≈ **BDPT 0.0907** | VCM's *connection* path is correct |
| Strategy isolation: VM-only (vc off) | underwater 0.364 ≈ **full VCM 0.366** | the **merge** produces essentially the whole result |
| VM-only, light depth = 2 (primary L-S-D only) vs photon caustic map | **0.290 vs 0.088 = 3.3×** | merge over-counts the *primary caustic itself*, apples-to-apples |
| Merge-radius sweep r = 0.05 / 0.15 / 0.40 / auto | 0.3638 / 0.3642 / 0.3640 / 0.3638 | **radius-invariant** → NOT a kernel-area (`1/πr²`) bug; a constant factor |
| BDPT convergence s32 / s128 / s512 | underwater 0.0890 / 0.0907 / 0.0917 (flat; max 17.6→13.1) | **bias, not variance** — BDPT does not climb toward VCM |
| Courtyard (direct-lit, non-caustic) | VCM **+9.2%** vs PT=BDPT=photon consensus | VM leaks energy even outside caustics |
| `scattering 10000` confound | perfect-refraction (1e6) VCM uw ratio 0.998; VCM/BDPT 2.295→2.280 | inert — it's a refraction-cone exponent, not a medium |

---

## 2. The question and the prior it overturns

A first-order analysis read the ladder PT(0.035) < BDPT(0.066) < VCM(0.151)
as "each integrator adds techniques; VCM reaches the L-S-D-S-E caustic that
BDPT/PT structurally miss → working as intended."  The premise that **BDPT
cannot form the primary caustic with a delta light is correct** (the merge
needs the light-floor and eye-floor vertices to coincide, P=Q, which BDPT's
connections — distinct points — cannot do; only VM/photon-merging can).  But
the conclusion that VCM is therefore *correct* does not follow: the right
question is whether VCM's merge reaches that caustic with the **right
magnitude**.  It does not.

`pool_caustics` is already flagged in
[docs/AUTO_RASTERIZER_DESIGN.md](AUTO_RASTERIZER_DESIGN.md) (§6.x) as a
"separate, pre-existing residual."  This document characterizes that residual:
it is a VM merge over-count.

## 3. Why the references are trustworthy (the load-bearing point)

The whole question is *which estimator is right*.  Three independent estimators
agree the caustic is ~3–4× dimmer than VCM's merge says:

1. **Area-light PT ground truth.**  Replacing the two delta spots with one
   overhead emissive quad makes the caustic E-S-D-S-L directly samplable by
   PT's BSDF sampling — so high-spp PT is **unbiased and complete and uses no
   density estimation**.  It converges trivially (s64 underwater 1.0721 ≡
   s1024 1.0707, 0.1%).  Courtyard-calibrated underwater: PT(GT) 1.659,
   **VCM 2.659 (1.60×)**, BDPT 1.719 (1.04× — with an area light BDPT can
   sample the primary via s=0 eye-hits-light, so it joins PT as a complete
   unbiased witness).  This is the decisive arbiter: it does not depend on the
   photon map *or* VCM's merge.

2. **Caustic photon map** (the scene's own designated reference,
   `power_scale 1.0`).  A standard consistent density estimator; its region
   *mean* is energy-conserving and **radius-invariant** (verified — see §5).
   Its absolute scale is anchored to the same lights whose *direct* courtyard
   illumination matches PT/BDPT exactly (0.0586).  `power_scale` linearity
   confirmed: courtyard ps5/ps1 = 1.00 (caustic-only, as the code guarantees —
   caustic photons require an L-S…-D specular bounce), floor ps5/ps1 = 5.03–5.05.

3. **BDPT** captures all underwater transport *except* the primary caustic
   (GI: L-S-D-D-S-E and up).  Converged flat at 0.092.  photon-primary (0.088)
   + BDPT-GI (0.092) = ~0.18 ≈ the true total; VCM's 0.366 is ~2× that.

Energy conservation independently bounds it: a primary caustic of 0.088 off an
albedo-1 floor cannot amplify (via GI, even with the ~43% Lambertian TIR
return at the ior-1.33 surface) to VCM's 0.366 — that needs a return fraction
f≈0.76, far above what an open-top pool with absorbing turquoise walls allows.

## 4. Localization: the merge, not the connections

`vc_enabled`/`vm_enabled` strategy isolation pins it precisely:

- **VC-only ≈ BDPT** (0.0925 vs 0.0907 underwater; 0.0740 vs 0.0747 floor) —
  VCM's connection machinery is correct in expectation (the balance vs power-2
  heuristic difference is variance-only, as it must be).
- **VM-only ≈ full VCM** (0.364 vs 0.366) — the merge dominates and is the
  source of the excess.
- The **+9.2% courtyard** excess decomposes as VC +3.1% / VM +2.2% on a
  *directly-lit* region where the merge should contribute ~0 — the merge leaks.

## 5. The over-count is a constant factor, not a kernel-area / radius bug

VM-only region mean vs `merge_radius`: 0.3638 (r=0.05), 0.3642 (0.15), 0.3640
(0.40), 0.3638 (auto).  **Perfectly radius-invariant** — so the
`1/(π·r²·N)` kernel normalization
([VCMRecurrence.cpp:64-67](../src/Library/Shaders/VCMRecurrence.cpp)) is
correctly energy-conserving.  The over-count is a **radius-independent constant
≈ 3.3×** on the pure primary caustic (VM-only `max_light_depth 2` = 0.290 vs
photon caustic map 0.088).

Because the over-count hits the **primary** caustic — where the merge is the
*only* valid strategy and its MIS weight is legitimately ≈ 1 — and the balance
weight `1/(wLight+1+wCamera) ≤ 1` can only *reduce*, the excess **cannot** be
an MIS-weight error and **cannot** be a VC/VM GI double-count (the depth-2
isolation removes GI).  It is a **constant factor in the merge
contribution / normalization**.

The measured ≈3.3 is suggestively near **π** (π = 3.14159; the 0.29/0.088
ratio is 3.29, slightly above π because VM-Ldepth-2 still merges a few non-
primary vertices).  This points at a **π normalization-convention discrepancy**
between VCM's merge and the caustic photon map / SmallVCM convention — i.e. an
extra or missing `π` (or an analogous constant in the light-subpath-count
accounting) somewhere in the merge contribution path.  **This is a hypothesis,
not a confirmed line.**

### Candidate sites (ranked) — for the next engineer

1. **Merge contribution constant** —
   [VCMIntegrator.cpp:2144](../src/Library/Shaders/VCMIntegrator.cpp)
   (`total += VertexThroughput(v) * pixelMerge * norm.mVmNormalization`) and the
   per-merge accumulation at lines 2111-2141.  Check the `π` convention of
   `mVmNormalization = 1/(π·r²·N)` ([VCMRecurrence.cpp:64-67](../src/Library/Shaders/VCMRecurrence.cpp))
   against how `LightVertexThroughput`/`VertexThroughput` already fold (or
   don't) the `1/π` Lambertian BRDF normalization.  A double-counted or
   missing `1/π` here is the leading suspect.
2. **`mLightSubPathCount` accounting** — `= width*height`
   ([VCMRecurrence.cpp:58](../src/Library/Shaders/VCMRecurrence.cpp)).  Confirm
   the number of light subpaths actually traced/stored per render iteration
   equals the `N` used in the normalization (a per-sample vs per-iteration
   mismatch would also be radius-invariant — though that tends to give integer
   factors, not ≈π).
3. **`dVM` recurrence seed / auto-radius** —
   [VCMRecurrence.cpp:179](../src/Library/Shaders/VCMRecurrence.cpp)
   (`q.dVM = q.dVC * norm.mMisVcWeightFactor`) and
   `VCMRasterizerBase::PreRenderSetup`.  Lower likelihood (these drive MIS
   weights, which are bounded ≤1 and so cannot amplify the primary), but worth
   confirming for the delta-spot `foundSpecular` path the skill documents.

**Do not "fix" by flipping the MIS heuristic or loosening a test** — see the
skill's anti-patterns.  Pin the exact factor first with the per-strategy
instrumentation in [docs/skills/bdpt-vcm-mis-balance.md](skills/bdpt-vcm-mis-balance.md)
§3 (log `weight`, `contribution`, `mVmNormalization`, and a hand-computed
single-merge value), then add a `VCMStrategyBalanceTest` topology that locks
VM region-mean == caustic-photon-map / area-light-PT.

## 6. The `scattering 10000` confound is inert (ruled out)

`scattering` on `dielectric_material` is **not** a participating medium — it is
a Phong refraction-cone sharpness exponent
([DielectricSPF.cpp:148](../src/Library/Materials/DielectricSPF.cpp),
`alpha = acos(rand^(1/(scat+1)))`; default 10000 at
[AsciiSceneParser.cpp:3086](../src/Library/Parsers/AsciiSceneParser.cpp)).  At
10000 the cone half-angle is ~0.67° (near-deterministic); attenuation is via
`tau` (Beer-Lambert) only; no volume/ray-march/phase-function path exists.
Empirically, re-rendering at `scattering 1000000` (perfect refraction): VCM
underwater ratio 1e6/1e4 = 0.998, VCM/BDPT 2.295 → 2.280 — unchanged.  The
brightness gap is not volumetric.

## 7. Implications

- The auto-rasterizer routes `pool_caustics` → VCM
  ([AUTO_RASTERIZER_DESIGN.md](AUTO_RASTERIZER_DESIGN.md) §"hit"), i.e. to the
  over-counting integrator.  The *routing* (caustic → VCM) is still the right
  call (VCM is the only integrator that reaches the primary caustic at all);
  the *magnitude* VCM then produces is too high.  Fixing the merge will change
  the rendered brightness of every VCM caustic, including the auto-routed ones.
- The CLAUDE.md/integrator-selection claim "VCM reaches caustic energy PT/BDPT
  miss" remains true; the needed caveat is "…but VCM's merge over-counts that
  energy by ~1.6–3×."  The asymmetric MIS heuristic (BDPT power-2 / VCM
  balance) is *not* implicated — VC-only matches BDPT; the bug is in the merge
  contribution magnitude.

## 8. Reproduction

All matched-still variants are derived mechanically from the two scenes above
(strip `animation_options`/`timeline`/`DEFINE FRAMES`; film 400×300; EXR
`bpp 32` `multiple FALSE`; `pixel_filter box`; `oidn_denoise false`).  Render
from the repo root so `> run scenes/colors.RISEscript` resolves:

```sh
printf "render\nquit\n" | RISE_MEDIA_PATH="$(pwd)/" ./bin/rise <scene>
# verify via RISE_Log.txt: "FileEncoderObserver:: Written to" + "Total Rasterization Time"
# (the CLI exits 1 after quit even on success — check the log/EXR, not $?)
```

Rasterizer chunks used:

```
vcm_pel_rasterizer  { max_eye_depth 10  max_light_depth 10  samples N  merge_radius 0.0  vc_enabled true   vm_enabled true   pixel_filter box  oidn_denoise false }   # full
vcm_pel_rasterizer  { ... vc_enabled true   vm_enabled false  ... }   # VC-only  (≈ BDPT)
vcm_pel_rasterizer  { ... vc_enabled false  vm_enabled true   ... }   # VM-only  (≈ full)  ; add max_light_depth 2 to isolate the primary caustic
bdpt_pel_rasterizer { max_eye_depth 10  max_light_depth 10  samples N  pixel_filter box  oidn_denoise false }   # NO sms_* lines (SMS excised from BDPT 2026-05-07)
pathtracing_pel_rasterizer { samples N  pixel_filter box  oidn_denoise false }
```

Photon reference: the FeatureBased scene with `power_scale 1.0` (physical) and
`5.0` (linearity check), `fill` set to `shootphotons TRUE` so both lights drive
the caustic (matching VCM's transport).

Area-light arbiter: replace the two `spot_light` chunks with one
`lambertian_luminaire_material` (exitance `1.0 0.96 0.88`, scale 40) on a
downward-facing 6×6 `clippedplane_geometry` quad at `0 6 -1`; render PT at high
spp (ground truth), VCM and BDPT for comparison.

Analysis regions (rows×cols on the 300×400 image): courtyard anchor
`[260:294, 70:330]` (direct-lit dry checker, VCM≈BDPT≈PT), underwater
`[120:232, 85:315]`, white-floor caustic `[198:232, 110:290]`.  Calibrate
exposure on the courtyard, compare underwater.

Generator + analysis scripts and the working-tree `_pool_*.RISEscene` variants
used for this report are left in place for review.

---

## 9. Pin-it follow-up (2026-06-06) — the over-count is NOT a constant; §5's "≈π" is refuted

A focused pin-it pass (controlled scenes + temporary per-merge instrumentation,
since reverted; `src/**` byte-identical to HEAD) **overturns** the §5 hypothesis
that the excess is a constant ≈π in the merge normalization.

1. **The merge is EXACTLY correct for non-refractive transport.** Clean diffuse
   scene (flat floor + overhead area light, no dielectric), VM-only vs PT:
   **VM/PT = 1.009** (MC noise). Per-merge instrumentation showed every component
   exact: `camBsdf = ρ/π = 0.159155` (the Lambertian 1/π is present),
   `mVmNormalization = 1/(πr²N) = 0.0031831`, MIS `weight ≤ 1`, count `N = W·H`.
   → the §5 candidate #1 ("a double/missing 1/π or a constant ≈π in the merge")
   is **refuted**; the merge's kernel / BSDF / MIS / count machinery is sound.

2. **The over-count is refraction-specific AND ior-dependent — not a constant.**
   Controlled full-coverage flat dielectric over the floor, VM-only / PT, with
   **PT ≡ BDPT to 1.00** at every ior (two unbiased references agree → reference
   trustworthy; PT's brightening with ior is the physical refractive
   concentration of the overhead light, not a bug):

   | η | VM/PT (flat) | note |
   |---|---|---|
   | 1.00 (no dielectric) | 1.0× | diffuse merge correct |
   | 1.33 | **18×** | pool (wavy): 3.3× — magnitude is geometry-dependent |
   | 2.00 | **4.3×** | |

   The factor varies with ior **and** geometry (flat 18× vs pool 3.3× at the same
   η=1.33), so it is **not a single constant** — a blind `÷π` / `÷3.3` would be WRONG.

3. **RISE applies no η² radiance scaling in transport** (`DielectricSPF` kray =
   (1−Fresnel)·tau^dist, no η² — [DielectricSPF.cpp:201-224](../src/Library/Materials/DielectricSPF.cpp);
   BDPT throughput `beta *= kray` — [BDPTIntegrator.cpp:1269-1273](../src/Library/Shaders/BDPTIntegrator.cpp)).
   → **not** a missing/double-η² (Veach non-symmetry) bug.

4. **The merge fails to track the refraction's effect on photon density.** PT
   correctly shows the floor brightening with ior (0.064→0.354, η 1.33→2.0 — the
   refractive concentration); VM-only stays ~flat (1.16→1.52). So the over-count
   is large where the true caustic is dim (weak concentration) and small where it
   is bright — the merge's empirical photon-density / measure estimate is **not
   reflecting how a (delta) refraction maps the light onto the floor.**

**Refined site:** not the merge-formula constants (proven exact) and not an η²
scaling (absent). The discrepancy is in the **measure/density interaction for
refraction-crossing photons** — leading candidates: (a) the delta-refraction
pdf/measure as stored on the light vertex vs as consumed by the merge's area
density `1/(πr²N)`, and (b) the eye-subpath-through-refraction vertex's
contribution to the merge vs the (correct) connection. Pinning the exact line
needs a **single-refracted-path A/B trace** (one L-S-D photon: merge contribution
vs the analytically-known floor radiance for that path), which is the next
focused step. **The fix must be a principled refraction-measure correction, not a
constant divide.**

---

## 10. B2 single-path trace (2026-06-06) — root cause = refraction non-symmetry (η² measure) NOT captured by the merge

Re-instrumented the merge to decompose, per eye-vertex, the photon **density**
(`cand` = photons within radius r) vs the per-photon **throughput** (`lThru`),
and rendered the controlled full-coverage flat dielectric at η=1.33 vs η=2.00
(temporary; reverted, `src/**` byte-identical to HEAD):

| component | η=1.33 | η=2.00 | ratio |
|---|---|---|---|
| `cand` (photon DENSITY) | 1.678 | 1.709 | **1.02 — flat** |
| `lThru` (per-photon) | 857 | 951 | 1.11 |
| `camBsdf` | 0.159155 | 0.159155 | 1.00 |
| `weight` (MIS) | 0.570 | 0.514 | 0.90 |
| **`contrib` (per eye-vertex)** | 0.796 | 0.791 | **0.99 — flat** |
| PT≡BDPT (truth) | 0.064 | 0.354 | **5.53** |

**Root cause.** The merge's per-vertex contribution is **ior-independent (flat,
0.99×)** while the true floor radiance scales **5.53×** with ior. The flatness
traces to the **photon density being flat (1.02×)**: the merge's empirical
photon-density estimate does **not** reflect how the refraction concentrates /
compresses the light onto the floor. PT and BDPT (which agree to 1.00 — see §9)
*do* capture it, through the refracted-ray directions/pdfs (the implicit η²
solid-angle Jacobian of refraction). So the divergence is the **radiance-vs-
importance non-symmetry of refraction** (Veach 1997, §5; "the η² solid-angle
compression"): the eye-subpath transport carries it via geometry, but the VCM
merge's photon area-density estimate is **missing** the corresponding
measure/Jacobian. The merge therefore returns a roughly fixed value regardless
of refraction, **over-counting most where the true caustic is dimmest** (low ior
/ weak concentration → 18×; high ior → 4.3×; pool → 3.3×).

**The site is a MISSING correction, not a buggy line.** RISE applies no η² in
transport (§9.3), and the merge's constants are all exact (§9.1). The fix must
**add** the refraction non-symmetry / measure factor where the merge consumes a
refraction-crossing photon — i.e. convert the photon's (solid-angle-measure)
throughput to the floor's area-measure radiance with the refraction Jacobian the
eye-subpath already gets implicitly. Candidate insertion points: the light-vertex
record at the refraction in `GenerateLightSubpath` (carry an accumulated
refraction-Jacobian on `LightVertex`), or the merge contribution at
[VCMIntegrator.cpp:2141-2144](../src/Library/Shaders/VCMIntegrator.cpp) (apply the
per-photon non-symmetry factor before `mVmNormalization`).

**This is a rendering-theory fix, not a typo fix** — design it against the
VCM/UPBP-with-refraction references (SmallUPBP's handling of specular-chain
merges; Veach §5 non-symmetric scattering) before implementing, then verify VM
region-mean == PT≡BDPT across ior on the flat control AND restore the pool to the
photon-map value. Estimated effort: multi-day (design + implement + regression
test + re-validate every VCM caustic scene), well beyond a constant divide.

**Status:** mechanism pinned; exact correction is a design task. No integrator
code changed (`src/**` byte-identical to HEAD; all instrumentation reverted).

---

## 11. RESOLUTION (2026-06-06, gated-fix session) — there is NO VCM over-count; the entire diagnosis was a chain of measurement artifacts. VCM matches unbiased references to ≤3 %.

A careful gated attempt to *implement* the §10 η² fix began at STEP 0
(reproduce the controls) and **never proceeded to a code change**, because the
reproduction immediately falsified the premise. Every claim in §1–§10 that VCM
"over-counts" rests on a reference that is itself biased *low*. Measured against
**unbiased** references (path tracing for area lights; BDPT's light-tracing
*splat* for delta lights), **the shipped VCM is correct** — including in the
displaced-water (pool) regime. **No fix was implemented; none is warranted.**
`src/**` is byte-identical to HEAD.

### 11.1 The controlled "flat dielectric" (Control B, §9–§10) measured the FRESNEL SURFACE REFLECTION of the light, not the caustic

`gen_ctrl_flatdielectric.py` puts a top-down camera *above* the flat water.
The centre-pixel reflection ray bounces off the flat surface straight up into
the overhead light. The "PT≡BDPT truth" in §9–§10 is that specular reflection,
which grows with ior exactly as the Fresnel R₀, and has nothing to do with the
floor caustic:

| η | Control-B PT (measured) | R₀(η)·(L_light=10/π) (predicted reflection) | ratio |
|---|---|---|---|
| 1.33 | 0.063847 | R₀=0.020059 → 0.063851 | **0.9999** |
| 2.00 | 0.353697 | R₀=0.111111 → 0.353678 | **1.0001** |

Independent confirmations that Control-B PT is the surface reflection, not the
caustic: it is **floor-albedo-INDEPENDENT** (identical at ρ=0.5 and ρ=0.05,
0.0638 / 0.3537 both), and its "5.53× brightening with ior" is exactly
R₀(2.0)/R₀(1.33)=5.54. So §10's headline — "merge contrib flat 0.99× while
truth scales 5.53×" — compared the (correctly ≈flat) caustic merge against the
(ior-rising) **surface reflection**. The η²/measure root cause derived from
this is an artifact. This is the exact "never trust a measurement that reads
through the suspect component" trap flagged repeatedly in CLAUDE.md.

### 11.2 Clean caustic measurement (camera UNDER the water): full VCM ≈ unbiased ground truth

Put the camera *under* the water sheet so it sees the floor caustic directly,
with no air↔glass interface (hence no surface reflection) in the line of sight.
Then PT and BDPT can both reach the caustic and **PT≡BDPT** is a trustworthy,
density-estimation-free reference. Region [40:100,40:100], floor ρ=0.5.

**Area light** (PT can s0-sample the emitter → all three integrators reach it):

| η | PT | BDPT | full VCM (auto r≈0.026) | full VCM / PT |
|---|---|---|---|---|
| 1.33 | 0.9338 | 0.9338 | 0.9108 | **0.975** |
| 2.00 | 1.1247 | 1.1248 | 1.0767 | **0.957** |

(PT albedo-dependent ×10 here — it is the real caustic. The true caustic scales
only ~1.2× with ior, not 5.5×.) The shipped integrator is *slightly under*, not
over.

**Delta (spot) light** — the pool's actual regime; s0/connections cannot reach
the caustic, so the unbiased witness is BDPT's **s=3,t=1 light-tracing splat**
(light→water→floor→camera, both endpoints under the water so the connecting ray
never crosses the surface):

| η | BDPT (unbiased splat) | full VCM (auto) | caustic photon map | VCM/BDPT | photonmap/BDPT |
|---|---|---|---|---|---|
| 1.33 | 0.8087 ± 0.108 | 0.8110 | 0.4086 | **1.003** | **0.505** |
| 2.00 | 0.8613 ± 0.110 | 0.8373 | 0.4424 | **0.972** | **0.514** |

**Wavy water** (Gerstner displacement, pool params, concentrated caustic),
delta light, camera under water:

| region | full VCM | BDPT (unbiased) | VCM/BDPT |
|---|---|---|---|
| [15:125,15:125] | 0.3254 | 0.3254 | **0.9999** |
| [40:100,40:100] | 0.3285 | 0.3281 | **1.0012** |

VCM ≈ BDPT to ≤3 % everywhere, **0.1 % on the displaced-water (pool) caustic**.
Control A (no refraction) was VM/PT=1.01 throughout. The merge is unbiased.

**The ACTUAL `pool_caustics_vcm` scene** (real displaced-Gerstner water + the two
real delta spot lights + tile floor + the five submerged diffuse spheres), with
*only* the camera moved under the water surface (`location 0 -0.4 0.01`,
`lookat 0 -2 0`) so BDPT's splat reaches the floor caustic — nothing else changed:

| region | full VCM (auto r) | BDPT (unbiased, 1024 spp) | VCM/BDPT |
|---|---|---|---|
| full frame | 0.5096 | 0.5153 | **0.989** |
| central floor caustic [30:120,40:160] | 0.5370 | 0.5473 | **0.981** |
| tight centre [50:100,70:130] | 0.4590 | 0.4657 | **0.986** |

A 5×5 grid over the whole frame is 0.92–1.02 in every cell — VCM tracks the
unbiased reference spatially, not just in the mean, on the real scene. The
"~1.6–3.3× over-count" was entirely the original camera-above-water references
being unable to reach (or contaminating) the caustic. Scene generator:
`_pool_overcount_repro/make_pool_uw.py`.

### 11.3 The "1.5× over-count" of §4 is an artifact of the VM-only DIAGNOSTIC mode

`vc_enabled false` (VM-only) is an **incomplete estimator**: it adds the merge
contributions with the MIS weights computed for the *full* strategy set, but
omits the s0/connection contributions. For area lights it reads ~1.5× PT, but
that is not "the merge's contribution to the answer" — the *full* integrator
(vc+vm) is 0.96–0.98× PT (§11.2). The §4 inference "VM-only ≈ full ⇒ the merge
is the whole result" only held on the pool because there the light is *delta*
(s0/connections genuinely can't reach the caustic), so full VCM = merge alone —
and that merge-alone result is **correct** (§11.2 delta table), it just had no
valid reference in the original setup (see §11.4). The §4/§5 "radius-invariant
constant ≈π" was also not reproduced cleanly: in the clean scene VM-only is
radius-DEPENDENT (1.74×@r.025 → 0.99×@r.2) and mildly ior-*decreasing* — i.e. it
is finite-radius / incomplete-estimator behaviour, not a constant and not η².

### 11.4 Why all three original references were biased LOW (each made correct VCM look like an over-count)

1. **Control B / flat dielectric** → measured the Fresnel surface reflection
   (§11.1), not the caustic.
2. **Caustic photon map** → **under-counts the caustic by ~2×** (§11.2 delta
   table: photonmap/BDPT ≈ 0.51). The direct-lighting calibration only
   validated the pixelpel NEE path (VCM/PM_direct=1.046); the photon map's
   *caustic-gather* scale is independently ~2× low (a power-scale/gather
   calibration issue, separate from VCM — see §11.5). So "VCM 3.3–4.0×
   photon-primary" (§1, §3) is the photon map being low, not VCM being high.
3. **Pool BDPT / area-light "PT ground truth"** (§3) → the pool camera is
   *above* the water, so (a) BDPT's caustic splat is **blocked by the specular
   surface**, (b) PT's s0 **can't hit a delta spot**, and (c) the area-light
   variant is viewed *through* the surface, re-introducing the §11.1 Fresnel
   contamination. So pool BDPT (0.092) **under-reports the caustic it cannot
   reach**, and "VCM 1.6× area-light PT" is contamination + unreachable
   transport, not over-count. The §2 premise "BDPT cannot form the primary
   delta caustic" is true *only when the camera is above the water* — move it
   under, and BDPT's splat forms it and **agrees with VCM to 0.1 %** (§11.2).

### 11.5 Separate, real, minor finding: the caustic photon map under-counts ~2× — **RESOLVED 2026-06-06, see [§12](#12-caustic-photon-map-2-under-count--root-caused-and-fixed-2026-06-06)**

Independent of VCM: RISE's `caustic_pel_photonmap` + `caustic_pel_gather` at
`power_scale 1.0` renders the clean delta caustic at ~0.5× the unbiased BDPT
value, while its *direct* (NEE) channel matches to 4.6 %. This is a
photon-map caustic-scale/gather calibration discrepancy, not a VCM issue, and is
out of scope here — noted so the photon map is not used as a caustic ground
truth without re-calibration.  **§12 root-causes it (Jensen Gaussian gather
filter normalized to ~0.531 instead of 1) and fixes it.**

### 11.6 Conclusion and recommendation

- **Do not implement the §9–§10 η² fix, a constant divide, or any merge
  rescale.** They would make every VCM caustic ~2× too dim, breaking correct
  behaviour. The merge is unbiased (matches PT and BDPT to ≤3 %, 0.1 % on the
  pool-regime caustic).
- The §1–§10 verdict ("VM merge over-count") is **withdrawn**; it was a
  measurement-artifact cascade. The MIS asymmetry was never implicated and
  remains correct.
- Auto-rasterizer routing (`pool_caustics` → VCM) stays correct, and the
  brightness it produces is correct. No probe-gate re-tuning is needed.
- Recommended follow-ups (optional): (a) add a `VCMStrategyBalanceTest`
  topology locking **full VCM == BDPT** on the clean camera-under-water
  refractive caustic (flat + wavy), so this misdiagnosis cannot recur; (b)
  separately investigate the §11.5 photon-map caustic-scale ~2× under-count
  (**done — [§12](#12-caustic-photon-map-2-under-count--root-caused-and-fixed-2026-06-06)**).

**Process note:** the clean test requires an unbiased witness that can actually
*reach* the caustic. For a delta-light refractive caustic, put the camera under
the refractor so BDPT's light-tracing splat (and a direct-view PT) are valid —
then compare full VCM to BDPT, never VM-only to a photon map or a through-surface
PT. Repro harness: `scenes/Tests/VCM/_pool_overcount_repro/` (`gen_clean2.py`,
`gen_clean3.py`, `gen_delta_pm.py`, `gen_wavy2.py`, `region.py`).

**Status:** RESOLVED — no *VCM* bug; VCM/MIS code unchanged. (The caustic
*photon map* under-count noted in §11.5 is a separate, real bug, now fixed —
see §12.)

---

## 12. Caustic photon-map ~2× under-count — root-caused and fixed (2026-06-06)

The §11.5 follow-up. **Root cause: Jensen's Gaussian gather filter in the RGB
("Pel") photon-map radiance estimate is normalized to a disk-average of ~0.531
instead of 1, so the estimate was biased LOW by ~1.88×.** Fixed; the caustic
photon map now matches the unbiased BDPT reference.

### 12.1 Root cause

[`PhotonMapDirectionalPelHelper::RadianceEstimateFromSearch`](../src/Library/PhotonMapping/PhotonMap.h)
weights each gathered photon by Jensen's Gaussian filter
(*Realistic Image Synthesis Using Photon Mapping* eq. 7.7)

```
w(d) = α [ 1 − (1 − e^{−β d²/(2r²)}) / (1 − e^{−β}) ],   α = 0.918, β = 1.953
```

then divides the sum by the disk area `π r²`. For that estimate to stay
unbiased the filter's **disk average** must be 1:

```
(1/(π r²)) ∫₀ʳ w(d) · 2π d · dd = 1.
```

With α = 0.918, β = 1.953 this integral evaluates to **0.5311**, not 1
(substitute t = d²/(2r²): N = 2α[(1−1/D)/2 + (1−e^{−β/2})/(βD)], D = 1−e^{−β}).
Jensen normalizes his **cone** filter by the analogous `1 − 2/(3k)` factor; the
Gaussian filter here had no such correction, so every caustic-gather radiance
was multiplied by 0.531. The `1/π` Lambertian BRDF term and the `1/(π r²)`
density term are both correct — the *only* defect is the un-normalized filter.

This is the canonical "never trust a measurement that reads through the suspect
component" pattern: the photon map was used as a caustic reference in §1–§3,
and its 0.531× deficit (read as "VCM is 1/0.531 ≈ 1.9× too high") was one of the
three biased-low references that manufactured the false VCM over-count.

### 12.2 Evidence (clean, cross-checked — camera under the water, ior 1.33 & 2.0)

`scenes/Tests/VCM/_pool_overcount_repro/gen_pmcal.py` (140×140, region
`[40:100,40:100]`, pyOpenEXR). BDPT (light-tracing splat, no density estimation)
is the unbiased arbiter; the no-water DIRECT scene calibrates the light units.

| measurement | before fix (pm/BDPT) | after fix (pm/BDPT) |
|---|---|---|
| caustic ior 1.33 | **0.531** | **1.007** |
| caustic ior 2.00 | 0.504 | 0.925 |
| direct, no water (NEE channel) | 1.000 | 1.000 (untouched) |

The empirical 0.531 deficit matches the analytic filter disk-average (0.5311) to
0.1 %. ior 1.33 lands at ≈1.00 across *every* region (full-frame 1.002, tight
centre 1.009) and the direct/NEE channel is unchanged — so the fix is a pure,
correct rescale of the gather, nothing else.

**ior-2.00 residual (~6 %) is a separate, pre-existing density-estimator bias,
not the scale fix.** It is the photon map's inherent finite-radius boundary /
min-photon-threshold darkening on the *sharper* ior-2 caustic: (a) it shrinks
with photon density (0.925 @2 M photons → 0.943 @8 M); (b) the ior2/ior1.33
*relative* deficit is the SAME before (0.504/0.531 = 0.95) and after
(0.925/1.007 = 0.92) the fix — i.e. it is orthogonal to the global scale my fix
removes; (c) BDPT's mean ≈ median at ior 2 (0.9053 ≈ 0.9055) rules out firefly
inflation of the reference. A consistent (biased) density estimator only
reaches the unbiased value as r → 0 / N → ∞; this residual is expected and is
*not* a calibration error.

### 12.3 The fix

In `RadianceEstimateFromSearch`, divide the estimate by the filter's analytic
disk-integral (computed from α, β so it stays self-consistent if the constants
are ever retuned):

```cpp
const Scalar gaussD    = 1.0 - exp(-beta);
const Scalar gaussNorm = 2.0*alpha*( 0.5*(1.0 - 1.0/gaussD)
                                   + (1.0 - exp(-beta*0.5))/(beta*gaussD) ); // ≈ 0.5311
const Scalar invArea   = 1.0 / (PI * farthest_away * gaussNorm);
```

This is the principled fix (normalize the filter), not a magic `÷0.53` — the
filter now only *reshapes* the kernel and never rescales its energy, matching
the unfiltered `÷π r²` convention every other RISE photon map already uses.

### 12.4 Blast radius (audited)

`RadianceEstimateFromSearch` (the only filtered estimator in the tree) is shared
by two maps:

- **Caustic Pel photon map** → always uses it → **corrected** (~1.88× brighter,
  now unbiased). The intended fix. Every `caustic_pel_photonmap` scene now
  renders caustics at the correct brightness.
- **Global Pel photon map** → its render path is the *precomputed-irradiance*
  nearest-photon lookup (`SetGatherParams` always calls `PrecomputeIrradiance`),
  which uses the **unfiltered** `IrradianceEstimateFromSearch` — unaffected. Only
  the rare "no precomputed photon within radius" fallback hit `RadianceEstimate-
  FromSearch`; that edge case is now also correct. So non-caustic GI scenes are
  unchanged.

Already-correct (unfiltered `÷π r²`, **no change**): caustic/global **spectral**
maps and the **translucent** Pel map. They confirm the convention the fix
restores for the Pel path.

No unit test asserted the old 0.531 scale. Shipped caustic showcase scenes use
artistic `power_scale` (5 – 3000) and are not pass/fail-tested; they simply
render the caustic correctly brighter now (authors may wish to revisit those
exposure multipliers, an authoring choice, not a correctness issue).

### 12.5 Regression

`tests/CausticPhotonMapNormalizationTest.cpp` renders the camera-under-water
ior-1.33 caustic with the caustic Pel photon map and with BDPT, and asserts the
region-mean ratio is within 22 % of 1.0. Verified to have teeth: it fails at
ratio 0.533 with the normalization disabled and passes at 1.004 with the fix.

**Status:** FIXED. Code change: [`PhotonMap.h`](../src/Library/PhotonMapping/PhotonMap.h)
`RadianceEstimateFromSearch` only. VCM/MIS untouched (§11 still stands). Left
uncommitted for review.
