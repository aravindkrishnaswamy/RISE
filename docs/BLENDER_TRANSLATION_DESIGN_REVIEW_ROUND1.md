# Blender Translation Designs — Review Round 1 (2026-11-14)

**Later note (2026-11-16):** the unlisted-emitter weighting defect this record describes in the present tense was fixed as DL-633 (`tests/UnlistedEmitterWeightTest.cpp`). Source line numbers here predate that change.

Consolidated findings from four independent adversarial reviewers on:

- [BLENDER_PROCEDURAL_TRANSLATION_DESIGN.md](BLENDER_PROCEDURAL_TRANSLATION_DESIGN.md) (draft 4), "PARENT" below
- [MIX_MATERIAL_PATH_STATE_AO_DESIGN.md](MIX_MATERIAL_PATH_STATE_AO_DESIGN.md), "MIX" below

This file records what the reviewers found in **draft 4 of the parent and
draft 1 of the companion**. Both have since been revised in response (parent
draft 5, companion draft 2), following the owner's decisions of the same
day, and again after a second review
([round 2](BLENDER_TRANSLATION_DESIGN_REVIEW_ROUND2.md): parent draft 6,
companion draft 3); section numbers below refer to the reviewed drafts. Figures quoted here
(2,416 materials, the 94% headline) are the reviewed drafts' and were
superseded by a re-run of the survey with lobe options recorded.

Reviewers and their lanes: **T** physical basis and light transport; **C**
RISE code feasibility; **Y** Cycles fidelity and the survey; **V** compiler,
expression VM and spectral correctness. All were read-only on the repo; none
built or rendered RISE. Y ran eleven small headless Cycles 5.3 renders
(E1–E9) and a second pass over the corpus.

"Checked" below means the design author re-read the cited source lines after
the review and they say what the reviewer reported. Everything else is the
reviewer's finding, not independently confirmed. Paths are under
`src/Library/` unless they start with `intern/` (Blender source,
`~/Development/blender-git/blender`, 5.2-alpha, older than the 5.3 binary).

---

## 1. Does the design violate RISE's physically based principles?

Reviewer T's verdict: **partly**.

| Piece | Verdict | Why |
|---|---|---|
| `mix_material`, convex, weights a function of position only | Sound | An area mixture, like alpha coverage. Choosing one leaf per hit is unbiased in PT, in BDPT and VCM connections, and under HWSS (T derived this). |
| `ao()` as a texture input | Sound, with caveats | A scene-geometry signal like `occlusion()` and `proximity()`. Must be labelled an N-sample estimate, not "a geometric quantity" (§5). |
| Add Shader as total weight above 1 (`scaled_material` gain) | Violates | The unbounded-energy combiner that got `composite_material` frozen (SCENE_CONVENTIONS §11.5). `Add(Diffuse 0.8, Diffuse 0.8)` has albedo 1.6; in a closed room the bounce series diverges. |
| Weights or painters that read the view direction | Violates | Fresnel, Layer Weight, Incoming, Backfacing give a one-direction (non-reciprocal) surface, rejected on those grounds in DL-37 and DL-127. Found independently by T, V and Y. |
| Path state | Non-physical by construction, and not contained | Several "exact" and "unbiased" claims are false against the source (§3). |

T's condition for shipping Add and path state at all: as flagged
non-physical features (per-load warning, schema flag, as deprecated chunks
have), and only once the image is shown not to depend on MIS weights,
guiding, or light-sampling mode.

---

## 2. Findings that make the design wrong as written

### 2.1 View-direction inputs are path state in disguise (T P1-3, V P1-1, Y P2-2)

- PARENT §5.2d, §5.4, §9 treat `I`, `backfacing`, Reflection coordinates,
  Layer Weight and Fresnel as ordinary inputs, valid in every integrator,
  counted in Phase 1 (view vectors: 24.9% of materials).
- BDPT/VCM/MLT rebuild a vertex's record with the ray re-aimed per query:
  `Utilities/PathVertexEval.h:369, 415, 443, 702`
  (`isLightSubpathVertex ? -wi : -wo`). **Checked.** On a light-subpath
  vertex `I` points toward the light. Sampled-light records carry no ray at
  all (`Lights/LightSampler.cpp:1065-1085`). **Checked.**
- Consequence: a Fresnel-mixed material is a different function at the two
  ends of a connection and in forward versus reverse density. The mix's
  position hash makes the two ends agree on the random number, not on the
  weights. Example (T): `Mix(Diffuse, Glossy, fac = Fresnel)` at normal light
  incidence and grazing view; eye strategies pick glossy with probability
  near 1, light strategies pick diffuse with 0.96.
- The hazard already exists in the tree: `Painters/IridescentPainter.cpp:48`
  reads `ri.ray.Dir()`. **Checked.**
- Reviewers' fix: a `ReadsViewDirection()` capability treated like
  `ReadsPathState()` (path tracer only), with coverage restated. T's
  physically based alternative: the exporter pattern-matches Fresnel-weighted
  mixes onto `coated_material` or `ggx_material`.

### 2.2 "Found by bounces only" is not what the path tracer does today (T P1-1, C P1-1)

- MIX §I.5 and §II.3 say an emitter left out of the light list is found by
  scattering at full weight, citing `PathTracingIntegrator.cpp:2968`.
- That line does the opposite: `if( pdfSelect <= 0 ) pdfSelect = 1.0;`, then
  the power heuristic runs against a light-sampling density that does not
  exist. **Checked** (`Shaders/PathTracingIntegrator.cpp:2962-2971`; the only
  weight-1 gate is geometric, `CanBeAreaLight`, `:2929-2934`). HWSS twin
  `:6949-6956`; `Shaders/EmissionShaderOp.cpp:155-176` likewise. With RIS
  light sampling active the hit emission is zeroed outright (`:2948`).
  **Checked.**
- BDPT sets its "strategy absent" flag from geometry capability, not from
  membership in the light table (T: `BDPTIntegrator.cpp:4452`; C reads the
  nearby block as handling it; the two reviewers disagree on BDPT, to be
  resolved). VCM is correct (`VCMIntegrator.cpp:1990`).
- SMS: PT's emission suppression never asks whether SMS can sample the
  emitter (`PathTracingIntegrator.cpp:2790`), so an unlisted emitter behind a
  glass caster is suppressed and never supplied.
- Magnitude (T): a 10 cm emissive patch 2 m from a diffuse wall is weighted
  about 6e-7, essentially black.
- Fix: define "can be light-sampled" as table membership at all PT sites,
  the BDPT flag and the SMS gate; gate with a furnace test.

### 2.3 Shadow-only transparency is not one well-defined function (T P1-2, Y P1-2)

- The commonest Light Path idiom (Is Shadow Ray → Transparent, 67
  materials). The shadow ray sees through the object; the bounce ray toward
  the same light is blocked; the two are combined by MIS as if they agreed.
- This is true **in Cycles too**. Y's E5, direct light, floor under such a
  blocker, relative to no blocker: point light 1.0000; 0.2 m area light
  1.0000; 2 m area light at 4 m 0.9947; 10 m emissive panel at 2.5 m 0.672;
  constant white world 0.448, identical to an opaque blocker. So MIX §II.1's
  "Cycles is exact for surfaces that are not light sources" is false.
  (`intern/cycles/kernel/integrator/shade_shadow.h:48-49` **checked**.)
- RISE as designed would give a third number, and is not "exact under the
  path tracer" for this idiom except under delta lights.
- With a glass child it reproduces DL-344's double count.
- Under BDPT/VCM a connection edge is "shadow kind" while the same path
  under a neighbouring strategy has it as a walk ray, so "camera and shadow
  visibility work in all integrators" (MIX §II.4) holds for the camera flag
  only.
- T's fix: define shadow visibility as a property of the light-adjacent
  segment for both PT strategies (the emission partner of a bounce ray is
  found under shadow-context visibility); refuse shadow-kind dependence
  under BDPT/VCM.

### 2.4 Coverage numbers count "a mapping exists" as "exact" (Y P1-1)

Y's own estimate of exact coverage at the end of the plan is 62–75%, against
the design's 94%. The node-graph translation is not the problem; the BSDF
mappings are:

- **Subsurface:** 507 materials (21.0%); 453 counted covered. The bridge's
  own doc calls its model an approximation; it takes untextured colour only
  (243 of 257 SSS nodes have a linked Color); the survey cannot see constant
  Principled Subsurface (152 materials).
- **Glossy BSDF:** Cycles' Glossy is colour × microfacet with **no Fresnel**
  (`intern/cycles/kernel/svm/closure.h:686-697`, `bsdf->ior = 1`,
  **checked**). RISE's GGX has only conductor, Schlick-F0 and thin-film. 120
  of 152 glossy materials are tinted; 63 of 152 use Beckmann or
  Ashikhmin-Shirley.
- **Diffuse roughness:** Cycles uses an energy-preserving Oren-Nayar variant
  (E7: white stays 0.9998 in a furnace at any roughness). 40 materials.
- **Glass/Refraction:** tinted at the interface, GGX or Beckmann rough
  transmission. Of 38 glass, 31 Beckmann, 19 rough, 13 tinted.
- Spot check of 30 hand-read graphs: 12 fine, 5 need a spec fix, 12 visibly
  wrong at their claimed phase, 1 unreadable.

Y's table (percent of 2,416 materials):

| | today | step 0 | phase 1 | phase 2 | phase 3 | phase 4 | + path |
|---|---|---|---|---|---|---|---|
| As reported | 20.7 | 28.5 | 47.7 | 75.7 | 86.8 | 90.6 | 94.0 |
| SSS not counted as exact | 20.5 | 28.3 | 44.2 | 65.4 | 68.8 | 72.2 | 75.2 |
| Also excluding known-inexact BSDF options | 20.2 | 26.3 | 41.0 | 58.6 | 61.9 | 65.3 | 66.1 |
| Also excluding tinted Glossy, Translucent, tinted Transparent | 20.2 | 26.1 | 40.6 | 54.5 | 57.4 | 60.8 | 61.5 |
| Scenes fully covered (94 files with ≥ 5 materials) | 0 | 1 | 5 | 31 | 43 | 61 | 81 |

Rows two to four are upper bounds (option data missing for 485 materials).
Corpus caveats: 79 of 176 files are EEVEE scenes; 12 files hold 53% of
distinct materials; world shaders are unsurveyed (44 of 169 use Light Path).

### 2.5 Mix resolution: missed entry point and circular ordering (C P1-2)

- A fourth entry point, `ObjectManager::IntersectSingleObject`
  (`Managers/ObjectManager.cpp:1903-1915`, **checked**), produces records
  that are shaded directly, via `BSSRDFSampling::IntersectContactRay`, from
  PT, BDPT and RayCaster.
- Inside `ObjectManager::IntersectRay`,
  `BSSRDFSampling::ResolveCoincidentEntry` runs **before** the signal stamp
  (`ObjectManager.cpp:1645` then `:1647`, **checked**), reads the winner's
  subsurface parameters, and can replace the winning hit. Resolution needs
  the signals; the coincident-entry logic needs the resolved child.
- Fix: stamp signals → resolve → `ResolveCoincidentEntry` → if the hit was
  replaced, stamp and resolve again.

### 2.6 Both `ao()` ray queries have the wrong semantics (C P1-3, T P2)

- `IntersectShadowRayOpaque` excludes every object whose material reports
  `HasDeltaPassThrough()` (`Interfaces/IObjectManager.h:470-490`,
  **checked**): thin weave, fabric, and by MIX §I.3's own rule any mix with
  a tinted Transparent leaf would not occlude.
- `IntersectSingleObject` is closest-hit, forces both faces, ignores
  `casts_shadows`, stamps signals, and would have to resolve mixes, so
  "Only Local" inside a mix weight would recurse.
- Fix: plain any-hit `IntersectShadowRay` for the global case; a new
  single-object any-hit query for Only Local.

### 2.7 The "unresolved mix fails loudly" mechanism cannot work (C P1-4)

Null BSDF, SPF and emitter is exactly `NullMaterial`, registered as `"none"`
(`Materials/Material.h:28-35`, `Job.cpp:670-672`, **checked**), which PT
shades as silent black. Scene-prep code calls those getters legitimately on
every object, so the log would fire for every mix and identify nothing. The
leaf name `none` also collides with a name that already means a black
absorber; and in Blender an unlinked shader input **is** a black absorber,
not a pass-through (Y, E9). Fix: an explicit `IsUnresolvedMix()` checked at
shading entry points; name the leaf `passthrough`.

### 2.8 VCM merging is biased by any non-connectible leaf (T P1-4)

Light vertices are stored only when connectible
(`Shaders/VCMIntegrator.cpp:1523`, **checked**). For mix(diffuse w, mirror
or pass-through), a photon is stored with probability w and the eye vertex
is diffuse with probability w, so merging estimates w² where connections
estimate w: about 25% dark at w = 0.5 with half the weight on merging. The
design's Lambertian + Lambertian test cannot see it. Fix: stamp the
connectible fraction on the hit (generalising the alpha-coverage stamp) and
divide deposits by it.

### 2.9 "Exact under the path tracer" fails with path guiding; scatter classes unspecified (T P1-6)

- Every guided draw carries one label per material
  (`Utilities/PathTransportUtilities.h:66`, **checked**), so a
  diffuse-plus-glossy surface under guiding sends all guided energy on as
  "diffuse".
- GGX's multiscatter specular lobe is typed diffuse
  (`Materials/GGXSPF.cpp:473`, **checked**), so `ray_diffuse` is true after a
  rough metal's energy-compensation bounce.
- The class of subsurface exits and volume scatters is unspecified.
- Fix: refuse guiding with path-state scenes or classify guide draws by
  lobe; publish a class table per SPF lobe.

### 2.10 The lazy per-hit cache is not "the existing memo" (V P1-2, C P2-9)

- Today every `def` is evaluated eagerly; the existing memo is a 4-entry
  thread-local table of one final vec3 per program
  (`Utilities/ExpressionMemo.h`, `kL2Ways = 4`, **checked**), fixed size with
  a 4096-byte ceiling asserted by a test. Per-def storage for a 647-node
  program is about 15.5 KB per entry.
- The key includes ray direction for any program that calls `sample()`,
  which every image-textured material will; with rays re-aimed per query
  there is no reuse under BDPT/VCM.
- The colour and scalar pipes are keyed apart and the scalar pipe forces
  time 0, so a `def` shared by base colour and roughness is neither shared
  nor consistent under animation.
- So "`ao()` once per hit" does not exist yet, and `ao()` is not independent
  of the VM work unless it gets its own cache entry.

### 2.11 Stated Cycles facts that are wrong (Y P1-3, P1-4, P1-5)

- **"Is Diffuse Ray → emissive Add branch, 53 materials"** (MIX §II.2). The
  source of that idiom is a BSDF switch with emission strength 0. Genuine
  ray-type-gated emission is about 15 materials or fewer.
- Where emission really is gated, Y's E3 has Cycles at 0.074 of the
  scatter-only value by default: RISE's rule would be about **13× brighter**.
  Cycles decides whether to light-sample from a static estimate above 0.5
  (`intern/cycles/scene/shader.cpp:254-273`, **checked**).
- **"Every unlinked texture Vector uses Generated."** Not White Noise or
  Mapping (constant), and images default to UV. The survey repeats the error.
- **"Linear tables match every ramp mode."** Constant ramps are looked up
  without interpolation (`intern/cycles/kernel/svm/ramp.h:42-52`,
  **checked**), 81 materials; curves extrapolate, 605 of 606.
- **Bump from AO needs a gradient.** In Cycles it is exactly a no-op (E2).
- **Bump filter width 0.1 pixel.** It is a per-node input; 445 of 446 corpus
  bump materials read 1.0.
- **White Noise "matches the viewport".** It hashes the raw float32 bits of
  its input (`intern/cycles/kernel/svm/white_noise.h:24-41`, **checked**), so
  a double-precision upstream chain changes the whole output. 475 materials
  (19.7%). Either emulate float32 for chains feeding a hash, or promise a
  match in distribution only.

---

## 3. Gaps to resolve before implementation

**Material mixing**

- Refracting child under a ray-type weight (decision 3 as recorded): with
  `ray_camera ? glass : transparent` the camera ray pushes the IOR stack and
  the exit resolves to pass-through and never pops. Only predicates invariant
  between entering and transmitted kinds are safe, in practice `ray_shadow`
  alone (T). Same stack problem for a refracting child under a *constant*
  fractional weight, e.g. BMW27 "Glass" at 0.8 (Y); 40 covered materials have
  Glass or Refraction as a mix child.
- Closed IOR-stack entries do not copy the material from the hit; they are
  re-derived through the object (`Utilities/IORStack.h:185`,
  `IORStackSeeding.h` several sites). The restriction should be keyed on
  capability: translucent, subsurface, composite and `interior_medium`
  objects also track containment (C).
- Alpha state is non-virtual on the `IMaterial` base and gated by a
  process-wide counter, so "capability virtuals on the mix" cannot answer
  the alpha sites. Suggested: implement the pass-through leaf as the mix's
  own alpha coverage (C).
- `scaled_material`: also `IEmitter`'s 7 virtuals; subsurface throughput is
  read off the material, bypassing `ISPF`; type-keyed code
  (`dynamic_cast`, snapshot clone, the closed `ScattersFullSphere` list)
  does not see through a wrapper. Restrict children by allow-list (C).
- Type-based censuses are not an OR over children: a composite child hides
  from the extended-SMS census; a Transparent leaf plus a point light would
  route Auto to VCM (C).
- `bMaterialFromMix` reaches only PT's guards; 36 `GetSpecularInfo`
  consumers in 10 files read resolved records (C).
- A tinted Transparent leaf needs a material that does not exist yet (C).
  T suggests an alpha-style skip with a throughput tint.
- Mix semantics missing: factor is saturated; colour wired into a shader
  socket becomes Emission of strength 1 (77 mixes); Cycles evaluates all
  closures in NEE, attenuates shadows by transparent weight
  deterministically, goes opaque after 8 transparent bounces (Y).
- Denoiser guides: a chosen leaf makes albedo and normal dither while
  declared noise-free. Give a mix an expected-value guide (T, Y).
- Hash independence: the mix selector, `ao()`'s rotation and `hash_v(P)`
  must use distinct salts (T, V).
- Mix-aware luminaries: average the expectation Σ wᵢ·Mᵢ, not a hashed 0/1
  choice (T, C). Not every emitter record goes through `FillEmitterRecord`.
- Subsurface child with a spatially varying weight: light-side and eye-side
  jumps estimate the weight at different points (T).

**Path state**

- `ReadsPathState()` has no real precedent: `ReadsWorldPosition()` exists
  only on `IScalarPainter`, five implementers, one consumer (**checked**).
  A false negative gives exactly the plausible-and-wrong render the design
  forbids; budget an audit plus a hygiene census (C).
- Plumbing: the field must be added to `PropagateCastInputs` and both
  hand-written record copies or the BVH leaf copy-back wipes it; shadow
  records are built inside the object manager; PT is also entered through
  `CastRay` with only the collapsed ray type; refusal has no return channel
  (`RasterizeScene` is `void`); the Auto "forced integrator" check runs after
  the author pin and forces VCM (C).
- Camera flag under BDPT/VCM: the light-tracing splat must re-resolve the
  light vertex under camera context (T).
- Object ray visibility: Cycles strips diffuse/glossy bits on transmission
  rays and handles restricted-visibility lights by excluding closure classes
  in light sampling; a per-hit mask cannot do that. It is used by more files
  than Light Path (shadow off in 45 of 174 files). Camera and shadow masks
  could move to step 0 (Y).
- Camera-only emitters can stay light-sampled; blanket exclusion makes every
  camera-hidden light panel bounce-only (T, Y).

**`ao()`**

- On double-sided transmissive sheets the normal is ray-flipped, so eye and
  light walks get different hemispheres: not identical across integrators
  (T).
- On sampled lights it would read 1 unless the emitter probe gains an AO
  demand, contradicting decision 4 (C).
- SDF geometry evaluates a function during traversal; `ao()` must return 1
  there before its assert (C).

**Compiler and VM**

- What a compiled colour output is, is unspecified: RGB keeps channels
  above 1, the spectral uplift clamps to [0,1] and returns exact white near
  1 (`Interfaces/IPainter.h:272-278, 306-313`). A computed (2, 1.5, 1) is
  tinted and energy-gaining in RGB and exact white in spectral. Needs a
  declared output kind (V).
- Working colour space is never mentioned (V). All corpus files are
  Rec.709 (Y).
- The VM imposes more than "two structural changes": byte-pinned evaluator
  bodies under fast-math, a full scalar function-id band, single-return
  builtins (Voronoi and Noise are multi-output), a 32-bit context mask that
  the new inputs overflow, value-copied programs, no control flow (V).
- New hit inputs must reach every hand-built record; relief taps hold
  signals fixed so per-vertex attributes have zero gradient; `N` in a
  program is the post-modifier normal where Blender's is unperturbed (V).
- Bump: an explicit step below half the footprint is raised by design
  (`Modifiers/ReliefModifier.cpp:193-213`), so fine-grain bump renders
  smoother than Cycles; modifiers bind to objects while Blender wires Bump
  per BSDF socket (V).
- `sample_at`: mip selection uses the hit's UV Jacobian (wrong at a computed
  coordinate); drops alpha; invisible to the rename/remove reference scanner
  (V, C). Image colour is premultiplied unless Alpha is linked; cubic
  interpolation in 171 materials (Y).
- Performance has neither a baseline nor a budget. V's unmeasured estimate:
  10–20 µs per evaluation of a 400-node program against 1–3 µs for today's
  ray-plus-shade (V).
- Validation is all Cycles comparison; the 1e-4 tolerance is unreachable at
  ordinary coordinate scales unless builtins compute in float32; no
  reference-free tests for the VM change (V). Parity must neutralise clamping
  and Filter Glossy: 153 files clamp indirect, 151 set Filter Glossy 1.0 (Y).
- Fallback and render gate contradict each other, and the per-socket bake
  arrives in phase 3, leaving phases 1–2 undefined (V, Y).
- A program as its own scene object needs a new chunk category (crosses the
  GUI ABI), incremental-derive refusal, reference-graph and rename handling,
  appended `IJob` methods, all five build projects (C).
- Per-vertex attributes do not fit the data: face and face-corner domains
  dominate; 397 materials read attributes that exist only after modifiers or
  geometry nodes (Y).
- Version dependence: the Voronoi cell hash changed in Blender 5.0; the
  add-on declares a 4.0 minimum (Y).

---

## 4. Suggestions that change the plan

- **Use Blender's own node-tree inliner** (`bpy.types.InlineShaderNodes`,
  5.0+): resolves groups, muted nodes, reroutes, and unrolls closures and
  repeat zones the design lists as "not planned". Y: final coverage
  94.0% → 96.2% for no RISE work. Needs a temporary copy for materials with
  engine-specific outputs.
- **Split Phase 1** (V): 1a on the existing VM (median graph is 8 nodes, P75
  36) with converter builtins, `lut`, and loud warnings for every dropped
  node; 1b the executor change, justified by measurement.
- **Measure before choosing the executor** (V): ns per instruction; synthetic
  100-, 400- and 650-def programs with six outputs and bump; cache hit rates
  under PT and BDPT; against a stated budget. V's view: keep the language and
  chunks, but lower to a separate flat register executor, leaving the
  existing evaluator byte-identical; decide with the measurement.
- **Move the per-socket bake fallback to Phase 1** and key bakes on a hash of
  the sub-graph (V, Y).
- **Fold Emission leaves into the luminaire wrapper** instead of a gain
  above 1: no energy gain and light-sampleable, removing most bounce-only
  cases (T).
- **Shader mixing is the most valuable single addition to Phase 1**
  (47.7% → 57.0%) (Y).
- **Constant-fold Mix factors 0 and 1** (Y).
- **Add a constant-tint (no Fresnel) glossy mode and decide on Beckmann**
  before claiming step 0 (Y).
- Read node values from the evaluated depsgraph: 95 node trees are animated
  or driven (Y).

---

## 5. Decisions of 2026-11-14 that the findings touch

| Decision | Status after review |
|---|---|
| 1. Ray-type-dependent lights found by bounces only | Needs integrator changes first (§2.2). The "53 materials" premise was wrong: about 15 or fewer. Where it applies RISE would be about 13× brighter than Cycles' default. Camera-only emitters need not be excluded. |
| 2. Explicit BDPT/VCM/MLT refuse path-state scenes | Stands, but the same rule is needed for view-direction inputs (§2.1), about a quarter of the corpus, and for shadow-kind dependence (§2.3). |
| 3. Refracting child allowed when the weight depends on ray type alone | Unsafe as recorded; only `ray_shadow` is safe, and a constant fractional weight has the same problem. |
| 4. `ao()` evaluated on shadow rays and sampled lights | Stands; needs emitter-probe plumbing or it silently reads 1 on sampled lights. |
| 5. Names | `none` for the pass-through leaf collides with an existing black material. |

New questions the review raises: how to treat view-direction inputs under
bidirectional integrators; whether Add with energy gain is allowed at all;
what RISE's definition of shadow-only transparency is; whether "coverage"
means mapped or exact; and whether the executor is the existing VM or a new
one.

## 6. What the reviewers found sound

- Choose-one-per-hit for a convex, position-weighted mixture, in every
  path-integral strategy (T).
- The material audit's headline: stamps at `Object.cpp:1575` and
  `CSGObject.cpp:3725`; 109 lookup sites; PT never asks an object for its
  material; BDPT/VCM/SMS vertices copy it from the hit, so a rebuilt record
  recovers the live hit's child with no new vertex field (C).
- Cycles facts confirmed: flag semantics for arriving rays; transparent
  bounces keep flags; the AO node's estimator; the bump formula; Mix/Add as
  a weighted sum; white Transparent as pass-through; 257-entry tables;
  Generated coordinates; noise wrap; implicit conversions (Y).
- Options B, D, E and F were rejected for sound reasons (V).
- Emission routes through the radiance uplift, so strength above 1 is
  handled; hero and companion wavelengths see the same RGB; negative channels
  agree across pipes (V).
- The survey report reproduces exactly from its data, and errs on the
  conservative side for reachability (Y).

Y's scripts, per-file audit and render outputs are in the session scratchpad
under `cyrev/` and are not part of the repo.
