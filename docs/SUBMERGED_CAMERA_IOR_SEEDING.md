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
| env only | 0.03383 | 0.02889 | 0.854 | (2026-08-13 figures; superseded -- see the re-measurement below.) |
| sun+omni+emissives, no env | 0.01940 | 0.01260 | 0.649 | (a) VCM never samples **directional lights** — `DirectionalLight::radiantExitance()==0` keeps it out of the alias table, and VCM lacks the deterministic zero-exitance NEE loop PT (`LightSampler::EvaluateDirectLighting` Step 1) and BDPT (`EvaluateAllStrategies`) both run; the sun contributes nothing to VCM.  (b) The omni's glow through the medium hits the same medium-vertex gap as env. |
| full scene | 0.05330 | 0.04151 | 0.779 | components sum linearly (PT 0.0532, VCM 0.0415). |

**Env-only row re-measured 2026-09-27 (debt-dl247b build; 400×300, 256 spp,
`oidn_denoise FALSE`, linear Rec.709 luma, sun/omni removed and every
`emissive_scale` zeroed):** PT **0.037878 ± 0.000004** (n = 4, sd),
VCM **0.037177 ± 0.000001** (n = 4), BDPT **0.036966 ± 0.0000002** (n = 2);
VCM/PT 0.981, BDPT/PT 0.976.  PT moved +12% from the 0.03383 above: this
camera sits inside `water_volume`, so its eye rays start with the
camera-ray volumetric walk, whose surface hand-off used to skip the medium
in front of the surface (DL-247) -- plus the intervening env-MIS and
DL-218 fixes, which this re-measurement does not separate.  VCM's old
0.854 was the DL-218 medium-vertex gap, since closed.  The residual
~2% BDPT/VCM-below-PT is not attributed here (the DL-247 inside/outside
box shows the same BDPT ~2.6% deficit, pre-existing).  The sun/omni row
and the full-scene row were NOT re-measured.

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
2. **VCM medium-vertex photon merge gap** — VCM now connects and performs
   NEE at MEDIUM vertices (DL-218 closed), but photon merges remain
   surface-only (documented in docs/VCM.md). The "BDPT/VCM ~2x PT in a
   global medium" gap (DL-247) was PT's camera-walk surface hand-off
   skipping the medium segment -- fixed for pel/NM (`db71fdfd`) and
   `hwss TRUE` (debt-dl247b) -- plus mismatched `max_volume_bounce`
   truncation, now one rule for all integrators (see the ledger row).
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
   **UPDATE (2026-09-14, DL-76 closure, debt-misc slice)**: the
   single-open-surface false positive described above was closed by a
   later slice's P2-4 fix (require positive parity along the probe AND
   its reverse — see `IORStackSeeding.h`'s own doc comment and
   `docs/DEBT_LEDGER.md` DL-46).  A narrower residual DL-46 opened
   (DL-76) — a SINGLE `Object` built from two disjoint open pieces
   straddling the seed on opposite sides of one axis, both pieces
   facing away from the seed — was closed this pass by extending the
   vote to the two other principal axes; see the DL-76 row for the
   accepted false-POSITIVE residual (an adversarial object built from
   open pieces straddling the seed along all three principal axes at
   once would still fool the vote — this is a bounded improvement, not
   a general winding-number/solid-angle containment test).  The
   fixed-`+Z`-only probe direction and grazing-tangent-accumulation
   limitations in this item are UNCHANGED by that work.

   **UPDATE (2026-09-14, DL-76 perf + residual follow-up, same slice)**:
   two corrections to the above. (1) **Probe count.** The X/Y vote is
   traced ONCE per `SeedFromPoint` call, not per candidate object and
   not via a per-call `IsConfirmedAlongAxis` helper (that helper was
   removed) — the two X probes and two Y probes are hoisted next to the
   existing Z-reverse probe, gated on at least one Z-confirmed candidate
   existing, and each candidate is then checked by an array lookup
   (`HasPositiveParity`) against the shared, already-traced arrays. The
   real per-call budget is therefore **at most 6 `TallyProbe` traces**
   (Z forward, Z reverse, X forward, X reverse, Y forward, Y reverse),
   independent of how many candidates the Z round finds — not "three
   probes" and not the up-to-32-calls-per-candidate cost the original
   per-candidate implementation paid (measured red on the unfixed
   per-candidate code with a 2-candidate nested-box fixture: 10 traces
   [2 for Z + 4 per candidate x 2 candidates]; bounded at 6 after
   hoisting). See `tests/TranslucentInitialContainmentTest.cpp`
   sub-test 6 for the instrumented regression. (2) **No false-negative
   from tunnel alignment (correction to an earlier draft of this
   update).** This update originally claimed the three-axis vote could
   REJECT a legitimately closed object whose through-tunnels happen to
   align with the probe's three fixed axes. That claim is wrong and is
   retracted: a point strictly inside the solid bounded by a closed
   orientable manifold has ODD (net positive) crossing parity along
   EVERY generic probe direction, independent of genus, tunnels, or
   convexity — the ray starts inside a bounded solid and ends outside
   it, so it must cross the boundary an odd number of times regardless
   of which axis it follows, including one running through a hole.
   Verified directly: a `TorusGeometry(major 3, minor 1, hole axis Y)`
   probed from a seed point on the tube itself reads positive parity on
   all six +-X/+-Y/+-Z probes and is correctly seeded. The one real
   residual, now shared by three axes instead of just Z, is a probe
   that grazes the surface exactly tangentially or threads a face/edge
   boundary at the sampled precision — a pre-existing degenerate-
   alignment hazard, not a new failure mode.
7. **Photon-map flux is quadratic in luminaire area** (`power =
   E·area·scale` AND photon-count allocation ∝ `E·area`), so a scaled
   emitter's photon-map contribution moves by s⁴ where NEE moves by s².
   Pre-existing normalization defect, amplified now that area is
   world-correct; not addressed here.
8. **BDPT seeds its eye subpath from `pCamera->GetLocation()`** while
   PT seeds from `cameraRay.origin`; these differ for a thin-lens
   camera straddling a boundary (PT's is the more correct).  Left
   unaligned to keep this change focused.

## Non-refracting stateful media (DL-46, 2026-09-13)

The probe's original contract only tracked materials whose
`GetSpecularInfo` reported `canRefract=true` — i.e. an actual dielectric
with its own numeric IOR.  `TranslucentSPF` (the diffuse "lampshade"
model) is a second, narrower case the probe missed entirely: its
`Scatter`/`ScatterNM` classify entry vs. exit purely from
`ior_stack.containsCurrent()`, exactly like a refractor, but the
material is not specular (its lobes are diffuse/Phong, sampled
stochastically) and it carries no distinct IOR of its own — interior
segments re-push the ENCLOSING medium's IOR unchanged (see
`TranslucentSPF::Scatter`'s entry/backscatter comments).  Because
`TranslucentMaterial` inherited `IMaterial`'s invalid/non-refracting
default `GetSpecularInfo`, a camera or light origin already inside a
closed translucent object was never seeded with its membership, and the
first physical crossing was misclassified as an entry instead of an
exit (the same failure mode this whole document is about, on a
non-refracting material).

The fix adds a `hasInterior` flag to `SpecularInfo`
([SpecularInfo.h](../src/Library/Interfaces/SpecularInfo.h)) that a
material sets to report "I track my own containment like a refractor,
but I am not one" — `TranslucentMaterial::GetSpecularInfo` now returns
`{valid=true, isSpecular=false, canRefract=false, hasInterior=true}`.
`SeedFromPoint`'s probe accepts an object when EITHER `canRefract`
(with `ior > 0`) OR `hasInterior` holds.  The two cases push differently
in the final ordered push loop: a `canRefract` entry pushes its own
captured `ior` (unchanged); a `hasInterior`-only entry instead re-pushes
whatever IOR is already on the stack at that point (`stack.top()`),
because such a material never introduces a new numeric medium — only
membership changes.  Setting `canRefract` (or `isSpecular`) on
`TranslucentMaterial` merely to be picked up by the probe was
deliberately rejected: every OTHER `GetSpecularInfo` consumer (SMS
chain building in `ManifoldSolver`/`SMSPhotonMap`, the dielectric clear-
shadow-ray gate in `RayCaster`, the specular-companion checks in
`BDPTIntegrator`/`PathTracingIntegrator`) gates on `isSpecular`/
`canRefract` together, so a material that is genuinely non-specular
must keep reporting exactly that; `hasInterior` is additive and those
consumers are unaffected (confirmed by inspection of every
`GetSpecularInfo`/`GetSpecularInfoNM` call site — see the DL-46 slice
report for the full list).

## Perf note

`IORStackSeeding::SeedFromPoint` per camera sample costs ~7% wall on a
free-space-camera scene (pt_jewel_vault: 189s vs 177s with
`RISE_DISABLE_IOR_STACK_SEEDING=1`) — the same cost class BDPT/VCM
already paid per eye subpath.  On enclosed-camera scenes the recovered
transport itself dominates the wall-time change (jellyfish PT 256spp:
10.4s → 21.8s, almost all of it real added path length).

## The exit factor a submerged camera now picks up (debt 30, 2026-09-12)

Seeding the stack only decides whether the first crossing is classified
as an exit at all.  What that exit is WORTH changed on 2026-09-12: a
radiance-mode walk now multiplies its throughput by
`(eta_before / eta_after)^2` at every medium change, so an eye ray that
starts inside water (n = 1.33) and leaves for air is scaled by
**n^2 = 1.7689**, and one that starts inside glass (n = 1.5) by
**2.25**.  Everything the submerged camera sees through Snell's window
is that much brighter — which is the correct physics, not a gain: the
radiance inside a medium of index n in equilibrium with an external
field of radiance L is n^2 L.  Outside the window, total internal
reflection is unaffected (a reflection does not change medium, so the
factor is exactly 1).

The jellyfish scene and every other submerged-camera scene therefore
render brighter than the figures in this document, which predate the
fix.  `EnvLightBalanceTest` topology J is the exception and is
deliberately so: its shell is `ior 1.0`, so its factor is identically 1
and its closed form stays bit-exactly 1.0.  The refracting sibling of
that topology — the same geometry at `ior 1.5`, closed form n^2 = 2.25
— is row B of
[`tests/RefractiveRadianceScalingTest.cpp`](../tests/RefractiveRadianceScalingTest.cpp).
Mechanism: [REFRACTIVE_RADIANCE_SCALING.md](REFRACTIVE_RADIANCE_SCALING.md).

## Regression coverage

- `EnvLightBalanceTest` topology J — submerged camera, closed-form 1.0
  (`ior 1.0`, so the debt-30 eta^2 factor is 1 and this stays bit-exact).
- `RefractiveRadianceScalingTest` row B — the same topology at `ior 1.5`,
  closed form n^2 = 2.25: the EXIT direction of the eta^2 factor.
- `GeometryUVRoundtripTest::TestObjectWorldArea` — GetArea Jacobian
  (identity / uniform-scale / rotation+translation).
- Full suite green post-fix (221 run; VCM/BDPT strategy-balance,
  photon-map normalization, IORStackBehavior all pass unchanged).
