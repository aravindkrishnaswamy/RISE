# Submerged-Camera IOR Seeding — the "VCM misbehavior" that was PT all along

**Date:** 2026-08-13.  **Trigger:** the banked torture scene
[scenes/FeatureBased/VCM/vcm_sdf_luminaire_jellyfish.RISEscene](../scenes/FeatureBased/VCM/vcm_sdf_luminaire_jellyfish.RISEscene)
(commit 07f04ba2) measured VCM at 2.2× PT's frame mean with dense
non-converging speckle and an upward mean drift, on a topology squarely
inside VCM's documented regime.  The investigation was commissioned to
"bisect the VCM excess."  The excess was PT's deficit.

## TL;DR

1. **PT never seeded its eye-ray IOR stack from the camera position.**
   The jellyfish camera sits inside the scene's water box (a closed
   `dielectric_material` boundary).  With an empty IOR stack, the first
   outward boundary crossing runs `DielectricSPF::GenerateScatteredRay`
   with `bFromInside == false`, and the wrong-side direction test drops
   the transmission lobe — **entirely** for delta transmission
   (`scattering 1000000`), partially and angle-dependently for the
   scene's diffuse boundary (`scattering 0.0`).  PT lost ~8× of the
   environment energy and ~4.4× of total env transport on this scene
   while *looking* like the healthy, converging estimator.  BDPT and
   VCM have seeded their eye/light subpaths since the original
   `IORStackSeeding` work — that is why they disagreed with PT.
2. The same gap existed at **seven audited sibling sites** (eight
   `SeedFromPoint` calls): the legacy `PixelBasedPelRasterizer` /
   `PixelBasedSpectralIntegratingRasterizer` camera entries (incl.
   HWSS) and the photon tracers' per-photon emission origins
   (`PhotonTracer.h`, `SpectralPhotonTracer.h` — a luminaire sealed
   inside a dielectric emitted photons with an empty stack).  The
   adversarial review round found a ninth: `AOVBuffers.cpp`'s
   Accurate-prefilter guide retrace cast an unseeded camera ray.  All
   fixed.
3. **`Object::GetArea()` ignored the object transform** while
   `Object::UniformRandomPoint()` returned world-space samples, so every
   consumer claiming `pdfPosition = 1/GetArea()` (NEE, BDPT/VCM
   `InitLight`, photon-power normalization) was wrong by the transform's
   area scaling.  Measured: a `scale 2` emissive sphere lit its
   surroundings at 0.39× of the identical unscaled sphere.  Fixed with
   the `|det(L)|^(2/3)` Jacobian (exact for rotations/uniform scales;
   geometric-mean approximation for shear/non-uniform — see the comment
   in [Object.cpp](../src/Library/Objects/Object.cpp) for the residual
   non-uniform-scale caveat, which lives in the sampler, not the scalar).

## How it was root-caused (protocol notes for the next investigation)

- The A/B channel means, not luma, gave the first real signal: the VCM
  "excess" had the exact g:b ratio of the scene's `pnt_sea_bg` radiance
  map → environment transport, not S-D-S.
- Removing the env made PT and VCM agree within 6% → every other
  suspect (SDF area lights, CanBeAreaLight partition, VM radii, medium)
  was eliminated in one render pair.
- A **closed-form micro-scene** arbitrated who was wrong: camera inside
  a box of `dielectric ior 1.0`, uniform env, absorption-only medium →
  every pixel is `L·exp(−σa·d)` analytically.  PT rendered ~8× low
  (exactly 0 with a delta boundary); VCM/BDPT were within 16% (their
  residual traced to the diffuse-transmission lobe drop below).  The
  delta variant of this scene is now a permanent regression:
  **EnvLightBalanceTest, topology J "submerged camera"** — every
  integrator must render exactly 1.0 per pixel.
- The pre-existing `RISE_DISABLE_IOR_STACK_SEEDING=1` kill switch
  proved the mechanism: with seeding disabled, VCM collapsed to PT's
  broken zero.
- Full protocol additions are folded into
  [docs/skills/bdpt-vcm-mis-balance.md](skills/bdpt-vcm-mis-balance.md)
  step 0 (now three known non-MIS causes).

## Post-fix jellyfish A/B (400×300, raw EXR, linear Rec.709 luma — new protocol; the pre-fix header numbers were PNG 0-255 luma and are NOT comparable)

| integrator | spp | wall | luma mean | neighbour-diff |
|---|---|---|---|---|
| pathtracing_pel | 256 | 21.8s | 0.05330 | 0.0067 |
| pathtracing_pel | 1024 | 87.1s | 0.05328 | 0.0048 |
| vcm_pel | 256 | 15.7s | 0.04151 | 0.0223 |
| vcm_pel | 1024 | 60.4s | 0.04147 | 0.0162 |

Both integrators are now flat across a 4× sample increase — the
pre-fix VCM "mean drift" is gone (it was never established to be real
drift; the linear-EXR reruns showed VCM settling at every spp, with the
tone-mapping of the original PNG protocol amplifying small shifts).

## Residual split (256 spp, luma mean) — measured, not assumed

| illumination | PT | VCM | VCM/PT | attribution |
|---|---|---|---|---|
| env only | 0.03383 | 0.02889 | 0.854 | VCM/BDPT do no NEE and no merges at MEDIUM vertices (docs/VCM.md "surface-only merging"); env in-scatter through the water medium is reachable only via phase-sampling continuation.  BDPT measures 0.02891 — identical, same structural gap. |
| sun+omni+emissives, no env | 0.01940 | 0.01260 | 0.649 | (a) VCM never samples **directional lights** — `DirectionalLight::radiantExitance()==0` keeps it out of the alias table, and VCM lacks the deterministic zero-exitance NEE loop PT (`LightSampler::EvaluateDirectLighting` Step 1) and BDPT (`EvaluateAllStrategies`) both run; the sun contributes nothing to VCM.  (b) The omni's glow through the medium hits the same medium-vertex gap as env. |
| full scene | 0.05330 | 0.04151 | 0.779 | components sum linearly (PT 0.0532, VCM 0.0415). |

VCM's remaining 3.4× neighbour-diff vs PT at 1024 spp **does** decrease
with samples (0.0223 → 0.0162) — it is splat/connection variance from
env light subpaths crossing the scene's diffuse-transmissive water-box
boundary, not bias.

## Known gaps documented (not fixed here)

1. **VCM cannot sample directional lights at all** (no NEE, no photon
   emission).  PT/BDPT compensate via their deterministic
   zero-exitance-light loops; VCM's hand-written strategy functions
   never got one.  A real fix needs an emission model (bbox-disc) plus
   MIS bookkeeping — feature-sized work.
2. **VCM/BDPT medium-vertex strategy gap** — no NEE/merge at MEDIUM
   vertices (already documented in docs/VCM.md; quantified here at
   ~15% of frame energy on this scene class).
3. **`dielectric_material` finite `scattering` drops perturbed rays**
   that cross the tangent plane without renormalizing
   ([DielectricSPF.cpp](../src/Library/Materials/DielectricSPF.cpp)
   `GenerateScatteredRay`, the `bDielectric = false` gate after
   `Perturb`) — a real energy loss (~16% at `scattering 0.0` on a flat
   boundary, all integrators equally, now that they classify
   consistently).  Renormalizing changes the look of every rough
   dielectric in existing scenes; deliberately left alone.
4. **Scene-authoring trap:** `scattering 0.0` on a dielectric is
   *maximally diffuse* transmission (Phong exponent 0), not "no
   scattering".  The GUI agent that authored the jellyfish scene fell
   into this; the banked file keeps it because the diffuse boundary is
   part of what the scene stresses.
5. **Non-uniform-scale area lights**: `Object::GetArea()`'s Jacobian is
   exact only up to uniform scale; non-uniform scale additionally makes
   object-space-uniform sampling non-uniform in world area (sampler-
   level, same class as the TorusGeometry density bug).  For flattened
   emitters (`scale 4 0.05 4` panel idiom) the `|det|^(2/3)` value can
   sit *further* from truth than no correction — `LuminaryManager` now
   warns once when a non-uniformly-scaled luminaire is admitted.  A
   corpus scan found zero non-uniformly-scaled emitters in tree; 29
   uniformly-scaled emitter objects (25 SMS scenes at `scale 0.4`,
   4 benchmark objects) get correctly *dimmer* NEE — their banked
   `ΣL_sms/ΣL_supp` ratios are scale-invariant (both terms scale
   together).
6. **`IORStackSeeding::SeedFromPoint` limitations (pre-existing, now on
   the default path)**: the exits-vs-entries parity is only valid for
   CLOSED dielectric manifolds — a single-sided open surface (a lone
   glass pane / open water quad) crossed by the probe can be
   misclassified as "containing"; and the probe direction is fixed +Z,
   so an enclosure whose boundary is never crossed along +Z (camera
   under an infinite horizontal water plane in a Y-up scene) is
   silently not detected.  The fixed `kSeedEps = 1e-4` along-ray
   advance can also accumulate parity on grazing tangent geometry.
   These were latent on the BDPT/VCM paths since the helper's
   introduction; closed-boundary scenes (like the jellyfish water box)
   are handled exactly.
7. **Photon-map flux is quadratic in luminaire area** (`power =
   E·area·scale` AND photon-count allocation ∝ `E·area`), so a scaled
   emitter's photon-map contribution moves by s⁴ where NEE moves by s².
   Pre-existing normalization defect, amplified now that area is
   world-correct; not addressed here.
8. **BDPT seeds its eye subpath from `pCamera->GetLocation()`** while
   PT seeds from `cameraRay.origin`; these differ for a thin-lens
   camera straddling a boundary (PT's is the more correct).  Left
   unaligned to keep this change focused.

## Perf note

`IORStackSeeding::SeedFromPoint` per camera sample costs ~7% wall on a
free-space-camera scene (pt_jewel_vault: 189s vs 177s with
`RISE_DISABLE_IOR_STACK_SEEDING=1`) — the same cost class BDPT/VCM
already paid per eye subpath.  On enclosed-camera scenes the recovered
transport itself dominates the wall-time change (jellyfish PT 256spp:
10.4s → 21.8s, almost all of it real added path length).

## Regression coverage

- `EnvLightBalanceTest` topology J — submerged camera, closed-form 1.0.
- `GeometryUVRoundtripTest::TestObjectWorldArea` — GetArea Jacobian
  (identity / uniform-scale / rotation+translation).
- Full suite green post-fix (221 run; VCM/BDPT strategy-balance,
  photon-map normalization, IORStackBehavior all pass unchanged).
