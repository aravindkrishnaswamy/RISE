---
name: sms-firefly-diagnosis
description: |
  Structured procedure for diagnosing and fixing bright outlier pixels
  ("fireflies") in Specular Manifold Sampling renders.  Use when:
  rendering an SMS-enabled scene and seeing bright speckles that VCM
  reference doesn't show, seeing per-trial trialLum values above the
  legitimate caustic intensity, or user reports "fireflies" in an SMS
  test scene.  Walks through the firefly-triggered diagnostic, decodes
  attenChain fingerprints that map directly to specific bugs, and lists
  the audit sites that historically produce spurious contributions.
---

# SMS Firefly Diagnosis

## When To Use

- User reports fireflies / bright speckles in an SMS test render.
- A new SMS scene (glass caster that's non-trivial: multi-caster, thin
  shell, bezier-patch, displaced mesh) shows bright outliers that the
  VCM reference render doesn't.
- You've just changed SMS-adjacent code (`ManifoldSolver`,
  `SMSPhotonMap`, `PathTracingIntegrator` suppression paths) and want
  to verify no firefly regression was introduced.
- Top per-trial `trialLum` exceeds the legitimate caustic peak by ≥2×
  on any scene.

## When NOT To Use

- Noise that uniformly scales down with more samples IS NOT a firefly
  — that's normal MC variance.  Fireflies are BRIGHT OUTLIERS that
  persist or have mean values inconsistent with neighbouring pixels.
- Fireflies outside the specular manifold pathway (e.g. bright
  emitters showing up in BSDF-sampled paths on non-SMS scenes) —
  those are a PT integrator / MIS issue, not an SMS issue.  That said,
  the SMS integrator does have emission-suppression bookkeeping and
  several firefly mechanisms live in PT code; treat both as in-scope.
- Minor colour casts on colored-glass scenes that only differ from
  VCM in tint, not in magnitude — likely a material attenuation
  issue, not a firefly.

## Procedure

### 1. Confirm the fireflies are real and localised

Before instrumenting anything, render the scene twice with different
random seeds (or two wall-clock-seeded passes).  Identical firefly
positions across renders = deterministic path-space outlier (a real
bug).  Random firefly positions = MC variance that needs more samples
or a denoiser.  Both get diagnosed, but the fix differs.

If you have Python available, use a tiny PNG-parsing script (pure
stdlib + `zlib`) to print the top-N brightest pixels and their
3×3-neighbour excess:

- Stable pixel, excess > 5× neighbours → suspect deterministic bug.
- Different pixel each run, excess > 5× neighbours → suspect
  stochastic path finding (SMS root discovery variance or PT+NEE
  lucky connection).

### 2. Enable `SMS_TRACE_DIAGNOSTIC` and render at LOW samples

`src/Library/Utilities/ManifoldSolver.cpp` has a file-scope
`#define SMS_TRACE_DIAGNOSTIC 0` gate.  Flip to 1, rebuild `Library`
+ `RISE-CLI`, and render the scene at **4 spp** — you want FEW events
per pixel so per-event logs are readable.

The gated instrumentation produces:

- **`SMS_SOLVE_STATS`** (every 256k solves): total / empty / seedTooFar /
  derivFail / newtonFail / physicsFail / shortSeg / ok.  This is the
  first stop for perf issues — a Newton failure rate above ~10% means
  every failed solve is eating 30 iterations of patch evaluation.
- **`SMS_SHORTSEG_HIST`**: distance-bucket histogram of chains
  rejected by `minReliableSegment`.  Use it to tune the threshold to
  the scene's geometric scale.
- **`SMS_FIREFLY[n]`** (capped at 2000 events): fires on every
  trial whose luminance `trialLum > 5.0`, regardless of position.
  Logs the full factor breakdown, each chain vertex's pos / normal /
  eta / `isExit` / `isRefl` / `atten`.
- **`SMS_FINAL_FF[n]`** (capped at 400): fires post-sum-clamp when the
  final per-call `totalLum > 5.0`.  Lets you see which fireflies
  survive the clamp.

After rendering, grep / `awk` the log by `trialLum=` and `totalLum=` in
descending order.  Top 5 typically covers ≥90% of the problem.

### 3. Decode the `attenChain` fingerprint

`attenChain` in the log is `mResult.contribution` — the chain's
Fresnel × radiance-rescale product.  For a dielectric with IOR n,
common values map to specific chain topologies:

| attenChain ≈ | Formula | Physical meaning |
|---|---|---|
| **0.86** | `T` at near-normal | Single reflection on dielectric surface |
| **0.74** (for n=2.2) | `T²` | **k=2 paired refraction (correct)** |
| **4.16** (for n=2.2) | `n²·T` | **Unpaired "exit" refraction** — missing entry |
| **0.18** (for n=2.2) | `(1/n²)·T` | Unpaired "entry" refraction — missing exit |
| **17.3** (for n=2.2) | `(n²·T)²` | **BOTH k=2 vertices flagged as exits** — normal-flip bug |
| **0.14** (for n=2.2) | `(1/n²·T)²` | Both k=2 vertices flagged as entries |

If a chainLen=2 log has `attenChain > 1`, physics is broken.  The
value exactly identifies WHICH flag in the chain is wrong.  Use the
per-vertex `isExit` / `isRefl` fields to confirm.

### 4. Check the chain topology fits the scene

For each top-log firefly, manually verify — do not trust the Newton
solver:

- **Walk the chain vertex-by-vertex.**  Given shading `pos` and
  `lightP`, does the first chain vertex lie on the expected caster's
  first face from the receiver direction?  Does each subsequent
  vertex lie on the expected next face?
- **Parameterise the ray `pos → v_0` and test for unrelated
  specular intersections.**  For a multi-caster scene (two toruses,
  teapot + other glass), work out whether the external segment
  passes INSIDE a specular object that's not in the chain's
  `pObject` set.  If yes, the chain is physically a k=n+2 path,
  not k=n — SMS is applying the wrong transport.
- **Do the same for every "air" inter-specular segment** (vertex `i`
  with `isExit=1`, vertex `i+1` with `isExit=0`) — these inter-
  caster segments are the P1 #2 class bug that bit the torus_cross
  and bezier-teapot scenes.

### 5. Compare RGB to Spectral (NM) formulas

Whenever a bug is found in RGB path's `EvaluateAtShadingPoint`, check
the spectral twin `EvaluateAtShadingPointNM` in the same file.  The
two paths have drifted in at least three historical fixes (geometry
factor, photon chain attenuation, emission suppression).  Run the
same scene with a spectral rasterizer; compare to RGB.

### 6. Check the PT integrator's SMS emission suppression

`PathTracingIntegrator.cpp` has a `bPassedThroughSpecular` /
`bHadNonSpecularShading` pair used to skip emission at BSDF-sampled
light hits that SMS already covers.  Audit:

- The state must be updated after EVERY scatter — branching path
  (multi-lobe), single-scatter iterative path, BSSRDF emergence, and
  the spectral NM twin of all of the above.  Historically ONE of
  these gets missed per rework.
- When `considerEmission` is forced false at a delta scatter, the
  subsequent iteration's `bsdfPdf=0` makes the MIS branch fall
  through to `w=1` — so if suppression didn't set it to false, full
  emission is added.  This is the firefly mechanism behind the
  classic "random bright pixels on the floor OUTSIDE the caustic."
- On recursive `CastRay` handoff (branching path, BSSRDF), the
  child's `rs2.smsPassedThroughSpecular` and
  `rs2.smsHadNonSpecularShading` must be populated from the parent's
  state.  A fresh `RAY_STATE` starts with `false/false` and breaks
  the suppression chain.

### 7. Audit the photon tracer's pre-scatter state

`SMSPhotonMap.cpp::TraceSMSPhoton` determines each recorded vertex's
`isEntering` via `!ior_stack.containsCurrent()`.  The stack is mutated
by `pSPF->Scatter()`, so this call MUST happen BEFORE the scatter, not
after.  Reading it after always returns the inverted state and every
entering refraction ends up flagged as exiting — the telltale
pattern is that EVERY `chainLen=1` firefly has `v0.isExit=1` but the
scene has no glass-embedded diffuse surfaces.

### 8. Audit `BuildSeedChain` for TIR mis-labeling

When a refractive material produces TIR (`sin²T > 1`) during seed
construction, the seed direction is REFLECTED but the pushed
`ManifoldVertex` keeps `isReflection=false` (set earlier from
`!specInfo.canRefract`).  Promote it to `isReflection=true` inside
the TIR branch — Newton otherwise solves the refractive half-vector
constraint at a geometrically-reflective bounce.

### 9. Fix one class of mechanism at a time

Each bug family has a characteristic diagnostic fingerprint.  Resist
the temptation to stack fixes before re-measuring:

1. Patch.
2. Rebuild (remember to clean the relevant `.obj` — stale parser
   builds have caused ≥1 hour of confusion; see "Gotchas" below).
3. Re-render at 4 spp with diagnostic on.
4. Re-grep top trialLum / totalLum.
5. Only advance to the next fix after the fingerprint that drove
   this patch is GONE.  Chained fixes with mixed evidence obscure
   which change did what.

### 10. Disable the diagnostic and regression-check

Flip `SMS_TRACE_DIAGNOSTIC` back to 0, rebuild, render at scene-
declared samples, and render all the SMS test scenes (`sms_k1_*`,
`sms_k2_*`, `sms_slab_close_*`, any new teapot / custom scenes).
Visually compare each against its VCM reference (if present) or
against the pre-change version.

## Anti-patterns

### Blanket rejection of chainLen=1 refraction photons

Tempting after seeing the teapot / slab firefly pattern.  Breaks
`sms_k1_refract` and any other scene whose only light transport IS a
single-refraction pass-through (thin refractive plane, funnel with
an opening).  The correct long-term fix is topology-aware handling
at the receiver side, not early rejection.

### Trusting `cosI < 0` for entering/exiting on double-sided / bezier meshes

Bezier patches (and `double_sided TRUE` triangle meshes) flip the
normal to face the ray so `cosI ≤ 0` always.  The legacy cosI test
returned "entering" on every hit, producing chains where every
vertex is flagged as entry — signature
`attenChain ≈ 0.14` (two `(1/n²)·T` factors) or `≈ 17.3` (after a
receiver-side flip, two `n²·T` factors).  The fix is already in:
use `IORStack::containsCurrent()` as the source of truth.

### Per-trial clamp on `smsGeometric` instead of a sum-level clamp

The first instinct is `smsGeoPost = min(smsGeo, cap)`.  For
multi-preimage fold caustics this saturates EACH preimage at the
cap and sums them — `N × cap` per pixel.  The sum-level clamp (cap
the sum of per-trial smsGeometric values, scale back proportionally)
is what's already in the code; don't revert to per-trial clamping.

### Uniqueness threshold of `1e-4`

That's 0.1 mm — tighter than Newton jitter on displaced surfaces.
Near-fold-caustic preimage clusters stay distinct and their
contributions get summed.  `1e-2` (1 cm) merges Newton-jitter
duplicates without losing genuinely distinct preimages.

### Fixing only the RGB path

Spectral (NM) and HWSS paths have DRIFTED from the RGB path at least
three times.  Always search for the twin in the same file; if the
file has a `*NM` function parallel to the RGB one, the fix usually
needs to go into both.

### Editing `src/Library/Parsers/AsciiSceneParser.cpp` and not doing a clean Library build

Stale `.obj` for the parser silently keeps the old chunk handler
behaviour.  Symptom: scenes that should parse give a mysterious
"Failed to parse parameter name `X`" for a parameter the CURRENT
source clearly accepts.  Resolution: delete the `Library.obj`
artefacts and full-rebuild.  This swallowed an hour on the teapot
scene added in this session.

## Concrete Examples (from this repo's history)

### Example A — chainLen=1 refraction producing attenChain=4.16

**Symptom**: 33× overbright pixel at caustic peak of
`sms_k2_torus_cross`; top `SMS_FIREFLY` had `chainLen=1`,
`v0.isExit=1`, `attenChain=(3.94,3.94,3.94)`.

**Diagnosis**: `3.94 ≈ n²·T` for n=2.2 — unpaired "exit"
refraction.  The photon had actually CHOSEN the Fresnel reflection
branch in `PerfectRefractorSPF::Scatter`, but `SMSPhoton` only
recorded the vertex and the receiver-side chain treated the lone
vertex as a refraction EXIT, applying `n²·T` where there had been
no refraction at all.

**Fix**: add `bit 1: isReflection` to `SMSPhotonChainVertex::flags`,
record it from `pScat->type == ScatteredRay::eRayReflection` in
`TraceSMSPhoton`, decode it back into `mv.isReflection` on the
receiver side.  Drops every chainLen=1 mis-tagged refraction.

### Example B — chainLen=2 with attenChain ≈ 17

**Symptom**: displaced-slab firefly pixels with `attenChain=17.23`,
both chain vertices on the SAME caster with SAME-direction normals
(`n0.n1 > 0.99`).

**Diagnosis**: `17.3 ≈ (n²·T)²` — both vertices flagged as exits.
Photon tracer was using `cosI < 0` to decide entering/exiting.  For
a displaced mesh whose per-vertex normals all point the same way,
`cosI < 0` on BOTH the entry hit and the exit hit — receiver side
gets two "exits" after the flag flip.

**Fix**: `SMSPhotonMap.cpp` — use `ior_stack.containsCurrent()`
captured BEFORE the scatter.  The stack is authoritative about
medium state; it's what `PerfectRefractorSPF::Scatter` itself uses.

### Example C — deterministic bright pixel at floor far from caustic

**Symptom**: pixel saturates to 255 at `(1.47, 0, 0.67)` across
multiple renders, while VCM reference shows ordinary diffuse floor.

**Diagnosis**: added per-pixel `PT_TRACE` at the suspect `(x, y)` in
`PathTracingIntegrator.cpp` emission block.  Log showed BSDF-sampled
path: floor → BSDF → glass → glass → glass → light, with
`considerEmission=1`, `bsdfPdf=0` (last delta), `w_bsdf=1.0`.  Full
emission (~1900 × BSDF × throughput ≈ 277 per channel) added to the
pixel.

**Root cause**: the single-scatter iterative continuation path was
updating `throughput`, `bsdfPdf`, `considerEmission` after each
scatter, but NOT updating `bPassedThroughSpecular` or
`bHadNonSpecularShading`.  The branching path already did; this
path was missed.  So `bHadNonSpecularShading` stayed `false`, the
SMS suppression check failed, and the emission landed.

**Fix**: copy the 4-line state update from the branching path into
the single-scatter continuation path.  Sometimes the scary-looking
fireflies are three lines of missing code.

### Example D — spectral renderer off by an eta-dependent factor

**Symptom** (caught by code review before an image-level test): the
RGB path uses `G(x, v_1) · |det(δv_1/δy)|` (the correct SMS
measure conversion), but `EvaluateAtShadingPointNM` still uses
`cosAtLight · EvaluateChainGeometry / jacobianDet` — the obsolete
formula that's wrong by `(eta_i / eta_t)²`.  Harmless only when
`eta → 1`.

**Fix**: lift the RGB formula verbatim into the NM function.  Drop
`cosAtLight` from the product — it's implicit in the Jacobian's
light-tangent projection.

## Stop Rule

The skill's work is done when:

- No `SMS_FIREFLY` event has `trialLum > 5` on the test scenes that
  motivated the session; OR
- Every remaining event has been manually walked through step 4 and
  confirmed to be legitimate caustic focal convergence (not a path-
  space artefact), AND matches the VCM reference pixel-for-pixel
  within MC variance bounds.
- All regression scenes (`sms_k1_*`, `sms_k2_*`, `sms_slab_close_*`,
  any user-added test scenes) render without new visual firefly
  regressions vs their committed baselines.

Do NOT stop earlier just because the top trialLum dropped — review
who's still above 5 and confirm each is a legitimate focal point.
The slab and teapot scenes have bitten this pattern twice: an initial
fix moved the top firefly from 800 to 25, then to 10, each time
unmasking a smaller but real second-order bug.
