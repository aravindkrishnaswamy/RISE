# Blender Translation Designs — Review Round 3 (2026-11-16)

Findings of a third review (a model that had seen neither earlier round, run
by the owner) on:

- [BLENDER_PROCEDURAL_TRANSLATION_DESIGN.md](BLENDER_PROCEDURAL_TRANSLATION_DESIGN.md) (draft 6), "PARENT" below
- [MIX_MATERIAL_PATH_STATE_AO_DESIGN.md](MIX_MATERIAL_PATH_STATE_AO_DESIGN.md) (draft 3), "MIX" below

**The two designs have not been redrafted since this review.** Each now
carries an "After round 3" block at its top, and the parent's section 9 has
the corrected survey figures; the rest of their text is as reviewed.
Section and line numbers below are those of the reviewed drafts, and source
line numbers are those of commit `7ae2be23c`, before DL-633 shifted them. Earlier rounds:
[round 1](BLENDER_TRANSLATION_DESIGN_REVIEW_ROUND1.md),
[round 2](BLENDER_TRANSLATION_DESIGN_REVIEW_ROUND2.md).

**Verdict:** the drafts fix several of round 2's structural errors (weights
are no longer normalised, varying subsurface mixes and ray-facing `ao()` are
path-tracer-only, programs are typed references), but the rewritten
contracts contain fourteen new P1 defects. Steps S, B, D2, A and the Phase 2
program cache must not start as written. Of the eleven items the drafts say
may start, six may, and five need a specification fix first. The design
author agrees with the verdict.

**What the reviewer did.** Read both designs before either review record;
read the RISE sources the designs cite (VCM, BDPT, path tracer, object
manager, subsurface samplers, expression VM and memo, Auto rasterizer, SMS)
and the Cycles sources; ran Cycles test renders in Blender 5.3 (CPU) for
shadow-off blockers under six light types, gated emission, AO through a
constant ramp, a Translucent screen with shadows off, and
`InlineShaderNodes` behaviour. Did not modify the repository and did not
build or render RISE, so RISE magnitudes are analytic counterexamples.

**What the reviewer could not do.** The survey corpus under `~/Downloads`
was unreadable from its process (macOS folder privacy), so the coverage
tables were not reproduced. The survey tool was audited by reading, and 18
materials were checked by hand (the 13 BMW27 graphs round 2 left behind, and
five archetypes). The lowered size of corpus programs is still unmeasured.

"Checked" below means the design author re-read the cited source and it says
what the reviewer reported. The reviewer's renders were not re-run. Paths
are under `src/Library/` unless they start with `intern/` or `source/`
(Blender source).

---

## 1. P1 findings

| # | Finding | Checked | Author's disposition |
|---|---|---|---|
| 1 | **VCM merges are over-bright by `1/q` at an absorber leaf.** A vertex on a material with no SPF is pushed with `isConnectible` still at its default of true, so its photon is stored, and MIX §I.1 then divides by `q`, which excludes the absorber. `0.25 Diffuse + 0.75 none` merges four times too bright. The denoiser albedo for a material with no BSDF is white, so an absorber leaf reads as white. | Yes: `BDPTVertex.h:469`; the two generators break at `BDPTIntegrator.cpp:2754–2764` and `:7733–7742`, before the flag is set at `:2858` and `:7818`; `VCMIntegrator.cpp:1523`; `PathTracingIntegrator.cpp:5093–5095`. | Accepted. Set the flag from the material before both early exits; treat "no BSDF and no SPF" as black in the guides. |
| 2 | **`HitCoverage` misses the gates that decide whether alpha is tested at all.** There are 4 `AlphaCoverage` call sites, not 25. A mix whose only transparency is its pass-through is not counted as an alpha material, so the sampled loops are skipped and it renders opaque. A fully transparent mix decal coincident with a subsurface body makes the loop step past the body. | Yes for the count and the gates: the 25 was a count of lines containing the substring, 15 of them `SceneHasAlphaCoverage(`. Real gates: `IMaterial::AnyAlphaMaterials` (`ObjectManager.cpp:2921, 2986`); `HasEffectiveAlphaCoverage`, which reads the object material's alpha mode (`Lights/LightSampler.cpp:46–50`); `AcceptAlpha` (`LightSampler.cpp:1100`, `ManifoldSolver.cpp:12098`); alpha-mode pre-checks (`BSSRDFSampling.cpp:195`, `RandomWalkSSS.cpp:213`, `ManifoldSolver.cpp:12083, 12092`, both photon tracers). The coincident-decal case was not checked. | Accepted. A mix with a pass-through child registers as an alpha material; every gate above goes through one helper. |
| 3 | **The fixed-patchwork leaf selector.** See §3. | Partly. | Conceded: the leaf is drawn from the sampler once per hit. |
| 4 | **Subsurface probes: MIX §I.2 and §I.3 contradict each other.** One says the probes resolve the mix, the other that the entry inherits the exit's leaf. Resolving sends part of the entries to the wrong leaf; not resolving drops the mix's coverage and the leaf's own modifier at the entry. `FindSurfaceContact` asks the object's material for a diffusion profile and gets none from a mix. | The contradiction is in the text (MIX lines 288–292 and 352–354). The source claims were not checked. | Accepted. Probes take the exit's leaf and modifier and apply the mix's coverage at the entry point. |
| 5 | **`scaled_material`, `passthrough_material` and the luminaire wrappers.** (a) The gain factor `S` is recomputed at shading after the modifier ran, so it can differ from the `S` the leaf was chosen with. (b) Scaling a companion evaluation that returns the −1 sentinel breaks it. (c) The gain-mix allow-list leaves out `coated_material` and `fabric_material`, which the bridge emits for any Principled with Coat or Sheen, so Principled + Translucent foliage is refused. (d) `passthrough_material` must report its ray as a refraction for the HWSS recompute, and a narrowband tint with a zero hero value zeroes the companions. (e) The luminaire wrappers do not forward `ScattersFullSphere`, so an emissive Translucent leaf loses back-side light sampling. | (c) is in the text (MIX line 410). (e) yes: neither luminaire header mentions `ScattersFullSphere`. (a), (b), (d) not checked. | Accepted. `S` is stamped on the record at resolution. Whether coated and fabric children can be scaled is decided by reading how they bind their substrate, not assumed. |
| 6 | **Expected-value denoiser guides cannot be computed after resolution.** Every guide site runs after the hit record holds one leaf. | Not checked; follows from MIX §I.2 as written. | Accepted. The record keeps the unresolved mix for the guide pass. |
| 7 | **Shadow visibility (MIX §II.3d) has four defects.** (a) An emitter with `casts_shadows FALSE` is itself skipped by rule 2's walk, so its BSDF-sampled share is lost. (b) An ordinary alpha surface in front of a shadow-dependent one is tested twice on the BSDF side and once on the light-sampling side. (c) A shadow proxy that is absent to scatter rays leaves no trace on the record, so rules 2 and 3 never fire and its light leaks. (d) Rule 3 drops light sampled through a *diffuse* transmitter, which Cycles keeps even under a point light. | (a), (b) and (d) follow from the rule text (MIX lines 619–655). (c) yes: the sampled loop rebuilds the record after a rejected hit (`ObjectManager.cpp:2928–2935`). Cycles figures in §4. | Accepted, all four. Rule 2's walk credits each emitter it meets before skipping it and starts at the first shadow-dependent surface; the sampled loop reports rejected shadow-dependent candidates; the crossing prefix is straight crossings only. The claim "matches Cycles for point lights" was false for case (d). |
| 8 | **SMS prices a path-dependent blocker with no ray context.** The solver's visibility test has no ray class, so a card that is transparent only to glossy rays blocks the SMS chain while the path tracer's own emission is suppressed: a black caustic. | Yes for the mechanism: `ManifoldSolver.cpp:12021–12039` calls the sampled query with no context and treats any non-specular hit as a blocker. | Accepted. SMS is switched off, with a warning, in any scene with path-dependent transparency or visibility masks, as path guiding is. |
| 9 | **Camera visibility is not "a small object flag".** A light root vertex has no object pointer, only a luminary, so a camera-hidden emitter still splats; connections to the camera test `casts_shadows`, not camera visibility; a `passthrough_material` keeps the camera flag in the path tracer but BDPT tests depth 0 only. | Yes for the light root (`BDPTIntegrator.cpp:7170–7173`). The rest not checked. | Accepted. Camera visibility needs its own camera-segment query and moves out of step 0. |
| 10 | **Refusal can be bypassed.** Auto returns on an author pin before any scene check and caches its delegate once; several render entry points never pass through `Job::Rasterize`. | Yes: `AutoRasterizer.cpp:412–422` and `:1007`; direct `RasterizeScene` calls at `SceneEditController.cpp:3564, 20837`, five in `AgentSession.cpp`, and the animation paths in `Job.cpp`. | Accepted. Refusal is a capability on `IRasterizer`, enforced at the rasterizer's own entry, with the `Job` pre-flight kept for the message. |
| 11 | **Memo keys.** The new hit inputs (`I`, `Ng`, `N0`, second UV, object attributes) are not in the expression memo's key, and part of the key is compared only for programs that call `sample()`. No shutter time reaches a hit record, so `ao()` and the camera transforms return stale values under motion blur; MIX §III.4's own moving-occluder test cannot pass. | Yes: `ExpressionMemo.h:459–473` documents the motion-blur hole; `ExpressionPainter.cpp:368` fixes the scalar pipe's time at 0; ALPHA_COVERAGE.md states there is no per-ray time field. The "compared only with `sample()`" detail was not checked. | Accepted. The first half is a specification gap the hygiene test would catch; the second is a real hole. Per-thread memos are invalidated whenever a thread re-poses the scene for a sample. |
| 12 | **Normals.** A program's `N` is the bumped normal wherever a painter runs after the modifier. Bump and Normal Map outputs wired into a value socket (Fresnel or Layer Weight normal, AO normal, vector maths) have no lowering at all, and the survey counts them as covered. | Yes for `N` (`ExpressionPainter.cpp:105`). PARENT has no text on Bump or Normal Map feeding a value socket. PARENT already maps Geometry and Texture Coordinate normals to `N0`. | Accepted for the second half. Measure how many corpus materials do this before choosing between a perturbed-normal builtin and a fallback (§6). |
| 13 | **`diffuse_transmission_material`.** BDPT and VCM re-aim the record's ray without flipping the normal, so the lobe must decide reflection or transmission from both directions; it needs the per-scatter-type evaluation that per-type bounce caps use; RGB reflectance plus transmittance at most 1 does not bound the uplifted spectra at each wavelength; and ray-facing normals on transmitting materials are gated for `ao()` only in the survey's third table. | Yes for the re-aim (`PathVertexEval.h:520–525`). The rest not checked. | Accepted. The first three are contract lines for PARENT §5.6. The fourth depends on the normal rule in §6. |
| 14 | **Four Cycles facts are wrong or missing.** (a) `InlineShaderNodes.from_material` always targets "All" outputs, so a material with both an All and a Cycles output compiles the wrong one. (b) Mapping's unlinked Vector is a user-editable value, not zero. (c) The ramp lookup does not map infinities to 0: +∞ clamps to the last entry, and with extrapolation on it extrapolates first. (d) Cycles' Diffuse multiscatter term scales as albedo cubed, not squared, about 4% darker than the published model at albedo 0.5. | (a) yes: `NOD_shader_nodes_inline.hh:28`, `shader_nodes_inline.cc:307`. (b) yes: `node_shader_mapping.cc:20–24`. (c) yes: `intern/cycles/kernel/svm/ramp.h:26–43`. (d) partly: the squared albedo in the term is at `bsdf_oren_nayar.h:79`; the extra factor from the closure weight was not traced. | Accepted. For (d) RISE keeps the published model and the parity gate records the difference. |

## 2. P2 findings

| # | Finding | Author's disposition |
|---|---|---|
| 1 | **The combined VCM connection and merge weight does partition** once P1-1 is fixed: nothing a stored light vertex carries depends on the leaf at the deposit point. One caveat: next to a region with `q = 0` the finite merge radius darkens the border, vanishing with the radius. | Recorded as the reviewer's derivation; the design's open item closes when it is written into MIX §I.1 and a test confirms it. |
| 2 | **Step P misses three sites:** `EmissionShaderOp`, the legacy SMS suppression, and the extended-SMS emitter check, which compares a hit's material with the luminary's and would reject every mix luminary. The reviewer confirms the path tracer and BDPT defects by reading. | Accepted. |
| 3 | **Ray context.** `PropagateCastInputs` has four call sites, the subsurface contact ray builds a fresh record, and the delta-light see-through walk in `RayCaster` bypasses the sampled shadow query. Separately: an emitter that reads only `backfacing` or the incoming direction need not be bounce-only, because light sampling knows the direction. | Accepted, including the relaxation. |
| 4 | **`ao()` against Cycles.** Only Local in Cycles ignores shadow visibility, so the existing single-object query already matches and no new query is needed. Where Cycles returns AO = 1 its Color output is the input colour, not white. | Checked (`intern/cycles/kernel/svm/ao.h:82–84, 142`). Accepted. |
| 5 | **Step 0 adds bridge fields but the ABI bump is scheduled for Phase 1.** | Accepted: v19 for step 0, v20 and v21 after. |
| 6 | **Seven survey over-counts:** linked Bump strength and object- or world-space normal maps counted as Phase 1 (both modifiers take a constant scale, and the normal map is tangent-space only); Blackbody and Wavelength counted though PARENT never mentions them; linked Subsurface Weight and other Principled sockets counted as covered today; EEVEE Specular counted as a step-0 leaf though its transparency needs a mix; leaves inside groups invisible to the mix rules; Add Shader not checked against the gain-mix allow-list; White Noise flagged as reading Generated. | Checked for the first three (tool lines 531 and 552; `ReliefModifier.h:116`, `NormalMap.h:54`; PARENT has no mention of Blackbody or Wavelength). Accepted. The tool is fixed and the corpus re-collected before any coverage figure is quoted again. |
| 7 | **The three unmeasured Cycles differences, measured.** §4. | Recorded. |

P3: Perlin's large-coordinate offset, Voronoi's Radius output, the Object
Info random hash, constant-node kinds that the inliner synthesises, and line
shifts in several citations. The reviewer also answers PARENT open question
3: the inliner unrolls closure zones and constant-count repeat zones.

## 3. The leaf selector

Round 2 asked for an independent draw per scattering event. Draft 3 kept a
fixed patchwork (a hash of object instance, object-space position and a
salt) and said why (round 2 §3). Round 3 was asked to rule, and ruled for
the per-event draw:

| Reviewer's point | Author's reading |
|---|---|
| The reasons given for the patchwork do not hold. Hashing raw position bits means two computations of "the same point" that differ in the last bit get unrelated leaves, and a hit, a sampled light point and a relief tap do differ. Where agreement matters the leaf is copied onto the vertex anyway. | Correct. MIX lines 141–144 claim an agreement the mechanism does not provide. |
| None of the 45 direct probe sites needs a sampler it lacks: 40 never see a mix under MIX §I.3's own restrictions, three must not draw a leaf, two already have a sampler. | Consistent with the design's own restrictions; the 40 were not re-read. This removes the main practical objection. |
| A pixel-centre, no-filter render never converges on a mix. | True, but narrower than stated: that protocol is PARENT §7's node-parity test of emissive planes, not a mix test. |
| Exactly coincident faces in one object select together: `0.5 t²` where independent draws give `0.25 t²`. | True; the design already listed it as the patchwork's meaning. |
| The pass-through of a bounded mix is already a sampler draw, so the design mixes two mechanisms. | Correct, and decisive: a per-hit sampler draw is the mechanism RISE's alpha coverage already uses and has been validated under every integrator. |

**Outcome.** The author withdraws the patchwork. The leaf is drawn from the
sampler once per hit, stored on the record and copied to bidirectional
vertices; a hash is used only where no sampler exists. The instance id and
the per-mix salt are no longer needed.

## 4. Cycles measurements (Blender 5.3, CPU)

**A blocker with shadows off over a diffuse floor**, as a fraction of the
unblocked floor. The design's rule gives 1.000 in every row.

| Light | Cycles |
|---|---|
| Point | 1.000 |
| Area, 0.2 m | 1.000 |
| Area, 2 m | 0.995 |
| Area, 8 m | 0.791 |
| Mesh emitter, 10 m | 0.717 |
| Uniform white world | 0.541, the same as an opaque blocker |

**Emission gated by Is Diffuse Ray** (1 m emitter at 2.5 m): Cycles' default
light sampling renders 0.0011; with emission sampling off it renders 0.478,
which is what bounce-only gives. The design's "about 13 times brighter than
Cycles" was wrong for this scene by a factor of about 30: the ratio is 432, and it
grows as the emitter shrinks.

**AO through a constant ramp at 0.5**, across a penumbra: 16 samples per
evaluation, as Cycles redraws them, differ from a 128-sample value by up to
0.195 absolute near the threshold.

**A Translucent screen with shadows off, point light directly behind it:**
Cycles renders 1.063 on the floor (0.560 direct, 0.503 through the screen);
draft 3's rule 3 gives 0.560. With a baffle blocking the floor's own view of
the light, Cycles 0.027, rule 3 zero.

## 5. Disposition of round 2

The reviewer's table: of round 2's ten P1s, one is resolved (gated
emission), eight are partly resolved, and one is **made worse** (shadow
visibility, which gained the four defects of P1-7 and the SMS defect of
P1-8). Of its seven P2s, three are resolved and four partly. Round 1's
ray-type item is still not resolved, through P1-7, P1-8 and P1-10.

## 6. Owner decisions taken from this review (2026-11-16)

1. **Gated emission stays bounce-only**, now that the difference from
   Cycles' default is known to be hundreds of times and not thirteen. Cycles
   itself renders the bounce-only value when emission sampling is off; its
   default is a side effect of how it samples. The exporter warns loudly.
2. **Geometry and Texture Coordinate normals follow Cycles on a back face**:
   the normal is flipped to face the arriving ray, as already decided for
   `ao()`. On a material that transmits light those inputs are therefore
   view-direction inputs and path-tracer-only.
3. **A Bump or Normal Map wired into a value socket** is measured first (the
   survey now records where each perturbed normal goes), and gets a
   perturbed-normal builtin unless the count is a handful.
4. **Prose iteration on mixing and shadow visibility stops.** The cleared
   items start and the survey tool is fixed; the three reviews'
   counterexamples become failing closed-form tests; mixing and shadow
   visibility are prototyped on a branch against those tests, and the next
   review is of code and tests.

## 7. Gate

Nothing in steps S, B, D2 or A, or the Phase 2 program cache, starts.

May start: the pure builtins and their Cycles golden tests (with the ramp
rule corrected); the Python graph compiler's unit tests (with the output
selection and Mapping default corrected); the lowered-program size
measurement; the performance baseline; constant-reflectance GGX and
energy-preserving Oren-Nayar; step P with the three extra sites.

Needs a specification fix first: the new hit inputs (memo key);
`diffuse_transmission_material` (P1-13); step D1 (P1-8, P1-10); the step-0
exporter and camera visibility (P1-9, P2-5); and the survey tool (P2-6).
