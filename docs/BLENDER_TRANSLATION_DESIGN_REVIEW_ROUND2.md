# Blender Translation Designs — Review Round 2 (2026-11-14)

**Later note (2026-11-16):** the unlisted-emitter weighting defect this record describes in the present tense was fixed as DL-633 (`tests/UnlistedEmitterWeightTest.cpp`). Source line numbers here predate that change.

Findings of a second, independent review (a different model and harness from
round 1) on:

- [BLENDER_PROCEDURAL_TRANSLATION_DESIGN.md](BLENDER_PROCEDURAL_TRANSLATION_DESIGN.md) (draft 5), "PARENT" below
- [MIX_MATERIAL_PATH_STATE_AO_DESIGN.md](MIX_MATERIAL_PATH_STATE_AO_DESIGN.md) (draft 2), "MIX" below

This file records what the reviewer found in **those drafts**. Both have
since been revised (parent draft 6, companion draft 3) following the owner's
decisions of the same day (§4). Section numbers and coverage figures below
are the reviewed drafts'.

**Verdict:** "Implementation should not start on the transport features as
written. Pure procedural kernels, graph inspection, and performance
measurements can start." The design author agrees.

**What the reviewer did.** Read both designs before reading round 1;
reproduced the survey (2,418 materials, 1,020 structures, all three coverage
tables); opened BMW27 in Blender (`-b --factory-startup --disable-autoexec`,
never saved) and hand-checked 13 materials; ran three new Cycles renders
(Blender 5.3, CPU, 48×48, 16 samples, no denoising). Did not modify the
repository, and did not build or render RISE, so every transport magnitude
below is an analytic counterexample or a source-derived prediction.

"Checked" means the design author re-read the cited source lines and they say
what the reviewer reported. The reviewer's renders and counterexamples were
not re-run. Paths are under `src/Library/` unless they start with `intern/`
(Blender source). The reviewer's scripts and results were left in
`/tmp/rise-design-review/` on the review machine and are not checked in.

---

## 1. Findings that make the design wrong as written (P1)

| # | Finding | Evidence | Disposition in draft 3 / 6 |
|---|---|---|---|
| 1 | **A position-only selector correlates different objects.** Two congruent sheets sharing a mix select the same leaf at the same object-space point. `mix(passthrough(t), none)` at equal weights transmits about `0.5·t²` through both, where the mixture gives `0.25·t²`. | Analytic. | Accepted. The selector hash includes the object instance. The model is stated as a fixed microscopic patchwork, and the claim of equality with the weighted mixture is limited to paths that do not revisit a point (MIX §I.1). The reviewer's alternative, an independent draw per transport event, was not taken: see §3. |
| 2 | **Resolving the leaf before the alpha test loses the mix's alpha.** The sampled closest-hit loop calls `ri.pMaterial->AlphaCoverage(ri.geometric)` after its inner `IntersectRay`, which by then holds the leaf. A 50% transparent / opaque mix becomes opaque. | Checked: `Managers/ObjectManager.cpp:2962`. | Accepted. Resolution stamps the mix's own coverage on the record; every alpha site reads the product of that stamp and the leaf's coverage through one helper (MIX §I.2). |
| 3 | **Alpha alone cannot carry an Add.** `Add(Diffuse, white Transparent)` must transmit 1; coverage `w_clear/Σw` transmits 0.5. A lone child of weight 0.25 was also ambiguous (selected with probability 1). | Analytic. | Accepted. Weights are response coefficients, never normalised: a shortfall below 1 absorbs, and in a mix that declares energy gain the pass-through is a scaled delta leaf (MIX §I.1, §I.4). |
| 4 | **Depth-gated emitters were grouped with camera-gated ones.** "Camera = 0 on every light sample" says nothing about depth; `ray_depth ≥ 2 ? L : 0` evaluates to zero on a default-context light sample and MIS then down-weights the only non-zero strategy. | Checked: `Lights/LightSampler.cpp:1065`; `intern/cycles/kernel/svm/light_path.h:55`. | Accepted. Only the camera flag keeps an emitter light-sampled. Depth, ray length, transparent depth and shadow dependence are bounce-only (MIX §II.3e). |
| 5 | **Shadow-transparent glass cannot reuse the SMS guard.** The guard needs SMS on, an anchor and a covered caster chain; the same design makes mix leaves never casters; and dropping every chain through the glass would remove camera → glass → light. | Checked: `Shaders/PathTracingIntegrator.cpp:2777–2793`. | Accepted. The refracting-leaf exception is dropped by decision (§4). Object-level shadow-off glass gets a replacement rule of its own, independent of SMS (MIX §II.3d). |
| 6 | **"Tinted shadow transparency needs nothing extra" contradicts the mechanism chosen.** The delta pass-through machinery applies a tint on shadow rays only for delta lights. | Checked: `Rendering/RayCaster.cpp` (`bPassThrough = bDeltaLight && …`). | Accepted. Shadow-dependent surfaces get a dedicated shadow-context transmittance walk used by both strategies; path-independent pass-through keeps the existing rule (MIX §II.3d). |
| 7 | **The blanket "Bump from AO is a no-op" is wrong.** AO's Color output is the visibility fraction times its Color input, which keeps the input's gradient. Sine → AO.Color → Bump renders 0.496058 (mean red), the same sine straight into Bump 0.495903, no bump 0.614338: removing the bump is 23.8% brighter. | The reviewer's renders; `intern/cycles/kernel/svm/ao.h:145` checked. | Accepted. No fold. In a bump evaluation `ao()` takes its occlusion at the centre point and its arguments at the tap, as Cycles does (MIX §III.3). Round 1's E2 was one case, not a rule. |
| 8 | **The caches omit inputs that change the result:** ray flags and depth, object identity, scene identity and revision, the camera transform, and for AO the scene time and the Only Local object. | Checked: `Utilities/ExpressionMemo.h` (the existing key already carries scene, self, world point and time, with generation invalidation). | Accepted. Keys cover all external state, transitively through sampled painters and scene queries (PARENT §5.2a, MIX §III.3). |
| 9 | **Phase 1 ships path dependence before the machinery that contains it,** and step 0 exports shadow visibility before the estimator fix it relies on. | PARENT §9. | Accepted. Declaration, census, pre-flight refusal, Auto routing and the guiding switch move into Phase 1 ahead of the first view-direction input; the shadow fix precedes the shadow-visibility export (PARENT §9, MIX "Order of work"). |
| 10 | **Coverage counts refused or unmapped materials.** 80 materials at phase 4 (3.31 points) contain a Translucent mix leaf the companion refuses. Sheen, Metallic and Holdout are classed as plain shaders with no stated mapping. A mix whose leaves have different Normal inputs (BMW27 CarShellNew) is counted as translated. | Reproduced: 142 materials have Translucent as a mix or Add input; 80 of those were counted covered at phase 4. | Accepted. Translucent maps to a new lobe that may be a mix leaf, and per-leaf normals become a mix feature, both by decision (§4). Sheen and Metallic have stated mappings, Holdout is not planned, and the survey has keys for each (PARENT §5.6, §9). |

## 2. Findings that leave a contract incomplete (P2)

| Finding | Disposition |
|---|---|
| **VCM's connectible fraction has no stated probability.** Dividing by an unconditional `q` and by the accepted alpha over-corrects by 2 on a 50% diffuse / pass-through mix. Merge-only tests do not prove combined VC/VM weights when leaves have different densities. | Accepted. `q` is conditional on acceptance and the deposit divisor is their product, applied once. The combined-weight derivation and VM-only, VC-only and combined tests with a shrinking radius are a gate for step B (MIX §I.1, §I.6). |
| **AO's hemisphere.** `UnflippedGeomNormal()` is the geometric normal, not the unperturbed shading normal; Cycles defaults to `sd->N`. On a smooth-shaded coarse mesh the draft would facet. | Checked (`Intersection/RayIntersectionGeometric.h:~520`; `ao.h:129`). By decision (§4) the default is Cycles' rule: the unperturbed shading normal on the side the ray arrived from. |
| **AO's estimator is not Cycles' because its rays are cosine distributed.** Cycles draws from the path's random numbers; a position-hashed set gives a fixed estimate per point, and through a threshold ramp the two differ in mean. | Accepted as a disclosed difference. `ao()` is a deterministic scene texture; parity through ramps is measured and reported, not promised (MIX §III.3). |
| **HWSS and SMS.** `ISPF::EvaluateKrayNM` defaults to −1, so a new delta pass-through that implements only the methods named would lose three of four wavelength lanes. A path-dependent standalone specular material can evaluate differently along the path tracer's chain and SMS's reconstructed one. | Checked (`Interfaces/ISPF.h:551`). The companion evaluation is a stated requirement of `passthrough_material` and `scaled_material`; a path-dependent material is never an SMS caster (MIX §I.1, §II.4). |
| **Shadow-only glass still needs containment and spectral re-indexing.** | Moot for mixes: the exception is dropped (§4). The citation `IORStack.h:185` was wrong; the re-derivation is in `IORStackSeeding.h` (near line 4327). |
| **Phase 1 capacity and spectral parity.** 150 nodes is not a bound on compiled slots (512 scalar slots; a vec3 takes three). "Within the uplift's own band" has no definition; products of uplifted reflectances over several bounces are not RGB products. The scalar pipe evaluates at time 0. | Accepted. Lowered program size is measured on the corpus before Phase 1's coverage is promised; the spectral gate is split into a painter-level test, a white furnace and a reported render difference; compiled programs do not read `time` (PARENT §5.2, §6, §8). |
| **Missing contracts:** zero-total and non-finite weights; composition with a leaf's own alpha; program lifecycle, rename and ABI; the shadow partner's endpoint, density and medium handling; an algorithm for a subsurface mix whose weight varies over the surface; denoiser guides under transparency; camera visibility at a splat; MLT. | Each now has a stated contract (MIX §I.1, §I.2, §II.3d, §II.4; PARENT §5.2a). The varying subsurface mix is path-tracer-only. MLT refuses the new materials. |

## 3. Where the design author differs

One point. The reviewer's first correction asks for randomness per transport
event, so that repeated encounters with a mix are independent. The design
keeps a **fixed patchwork**: a point of an object instance always resolves to
the same leaf. Reasons:

- It is what a mixed surface is, at a scale below the pixel: a fixed
  arrangement of patches.
- Every query that reaches the same point agrees with no shared state: the
  hit, the shadow ray that ends there, the subsurface entry probe, a relief
  tap, the denoiser's retrace, and both ends of a bidirectional connection.
  A per-event draw would have to be threaded through 45 probe sites and
  stored wherever a point is revisited.
- With the instance in the hash and nearby points hashing independently, two
  events agree only at the same point of the same instance, which a path
  reaches twice with probability zero outside deliberate retracing.

The price is stated in the design: the estimate equals the weighted mixture
for paths that do not revisit a point, and coincident geometry of one
instance is correlated by definition.

## 4. Owner decisions taken from this review (2026-11-14)

1. **Translucent BSDF maps to a new RISE lobe**: a thin sheet that reflects
   and transmits diffusely and tracks no interior, so it may be a mix leaf.
   (146 corpus materials use Translucent, 142 of them inside a mix.)
2. **No refracting leaf in a mix, with no exception.** The shadow-ray-only
   glass case (about six materials) is refused like the rest.
3. **A mix child may name its own modifier**, applied when that leaf is
   chosen, so leaves with different bump or normal inputs translate.
4. **`ao()`'s default hemisphere follows Cycles**: the unperturbed shading
   normal on the side the ray arrived from. On a material that transmits
   light that makes default-normal AO a view-direction input, and so
   path-tracer-only.

## 5. The reviewer's disposition of round 1

Status of each round-1 finding in the *reviewed* drafts (parent 5, companion
2), as the reviewer judged it. "→" notes what draft 6 / 3 then changed.

| Round-1 finding | Reviewer's status | Note |
|---|---|---|
| View direction treated as ordinary surface data | Partly resolved | Enforcement arrived too late → moved into Phase 1 |
| Unlisted emitters get near-zero PT weight | Resolved as a diagnosis | |
| Shadow-only transparency has inconsistent MIS targets | Partly resolved | → tint, glass and context rules rewritten |
| Coverage conflates translation with lobe fidelity | Partly resolved | → refusals and unmapped shaders reclassified |
| Mix resolution misses entry points and order | Partly resolved | → parent coverage stamped at resolution |
| AO queries have the wrong semantics | Resolved in specification | |
| Unresolved mix resembles the null material | Resolved in specification | |
| VCM merge thinning | Partly resolved | → probability stated; derivation is a gate |
| Guiding and scatter-class mismatch | Partly resolved | → guiding switch moved into Phase 1 |
| The memo is not lazy per-def storage | Partly resolved | → keys completed |
| "53 emissive materials"; unlinked coordinate defaults; constant ramps and curve extrapolation; saturated Mix, implicit emission, black unlinked input; working colour space; evaluated mesh domains; Blender version | Resolved | |
| Gated-emission difference from Cycles; bump filter width; white-noise parity | Resolved as disclosed differences | The 13× figure is round 1's, not reproduced |
| **AO bump** | **Made worse** | → the fold is removed |
| Refracting mix children | Partly resolved | → exception dropped |
| **Parent alpha** | **Not resolved** | → finding 2 |
| Scaled-material forwarding and subsurface | Partly resolved | → finding 3 |
| Type-keyed censuses, snapshots, full-sphere list; capability census | Partly resolved | Named, not established: they stay gates |
| Mix exclusion from all SMS consumers | Partly resolved | → no longer leans on the SMS guard |
| Tinted Transparent material | Partly resolved | → companion-wavelength contract added |
| Denoiser guides | Partly resolved | → first-hit semantics stated |
| **Independent salts** | **Not resolved** | → finding 1 |
| Expected luminary power; emitter-fetch census; ray-context copies; pre-flight; performance plan; fallback timing and render gate | Resolved in specification or as a plan | |
| **Spatially varying subsurface mix** | **Not resolved** | → path-tracer-only |
| Camera visibility including splats | Partly resolved | → endpoint treatment stated |
| Object visibility compatibility | Partly resolved | Coverage is per material and still ignores object-level restrictions (PARENT §2.4) |
| Camera-only emitter efficiency | Partly resolved | → finding 4 |
| AO true side, sampled-light demand, SDF guard | Partly resolved | → normal rule and cache identity rewritten |
| Output kinds | Partly resolved | → spectral gate redefined |
| VM structural constraints | Partly resolved | → lowered size to be measured |
| Hand-built inputs; attribute bump derivatives; `sample_at` | Partly resolved | Requirements named; contracts come with the feature |
| **Per-leaf normals** | **Not resolved** | → per-child modifier, by decision |
| Validation protocol | Partly resolved | → tests added for each finding above |
| **Program lifecycle, ABI, rename** | **Not resolved** | → PARENT §5.2a |

## 6. Claims the reviewer checked and found correct

- The path tracer's unlisted-emitter defect is real; the 6e-7 weight in the
  draft's example follows from the power heuristic (calculated, not rendered).
- VCM handles an unlisted emitter correctly (`Shaders/VCMIntegrator.cpp:1990`).
- **BDPT is narrowed by source inspection:** its "strategy absent" flag is set
  from geometry capability, and its MIS walk keeps the zero-to-one remapping
  at the emitter endpoint unless that flag is set. An area-sampleable unlisted
  emitter therefore still has the phantom strategy
  (`Shaders/BDPTIntegrator.cpp:4452`, `:6958`). The furnace test remains the
  proof; the expectation is now that it fails.
- The VM evaluates every `def` eagerly, and the memo is not the proposed cache.
- Guided draws carry one scatter class per material.
- Cycles' AO uses cosine-distributed rays and shadow or local intersections
  with no shader evaluated; an unlinked zero Distance compiles to the
  global-radius flag.
- Camera Data depends on the shading point and the camera transform, not on
  the arriving ray, so it is correctly outside the path-dependent class.
- The survey reproduces exactly. Reproducibility does not validate its rules.

**Corpus spot check (BMW27, 13 materials):** nine step-0 materials map as
claimed, with the inexact label right where a Beckmann distribution is used;
CarShellNew's Fresnel mix is classified correctly but its leaves have
different normals; Floor contains an Ashikhmin Sheen leaf with no mapping and
no inexact flag; Glass is correctly refused.

## 7. The reviewer's direct answers

- **Physical basis.** A convex mixture of reciprocal, energy-bounded BSDFs is
  admissible; the draft's *selector* did not estimate it correctly. Adding
  emission is valid. AO defines a geometry-dependent texture, but a finite
  stochastic estimate of it is not automatically independent of the sampling
  strategy. Arbitrary weights in the arriving direction generally break
  reciprocity; special cases need not (a different front and back on a
  non-transmitting sheet). Treating them all as path-tracer-only is a support
  policy, not a proof that each is non-physical.
- **Containment.** Achievable only once the dependency, transport-context and
  pre-flight contracts exist. As reviewed the drafts failed their own
  "sampling mode must not change the image" condition.
- **Refusing BDPT, VCM and MLT** is sufficient only if the census covers every
  material input, wrapper, object mask, modifier and scene query, and it does
  not fix stale caches, SMS reconstruction, HWSS companions or denoiser
  guides.
- **Plan.** Baseline first and executor choice after a spike is right; the
  spike should compare representative lowered programs and should not require
  building both executors in full.

## 8. What would close the review

Isolated RISE tests, none of which exists yet because nothing is implemented:
the two-sheet selector correlation; parent-alpha preservation; transparent
gain under Add; a depth-gated emitter; shadow-only tinted transmission under
an area light; shadow-off glass with SMS on and off; combined VC/VM
convergence on a mixed glossy, diffuse and delta mix. Each is listed in the
companion's test sections. Until they pass, no claim that these estimators
are unbiased, or that their MIS weights sum to one, is verified.
