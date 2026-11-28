# Material Mixing, Path State and Ray-Traced Ambient Occlusion — Design

> **Shipped status (2026-11-27):** Core parts of this design are **implemented and shipped** on `master`:
> - **Part 0 (`RISE::PathDependence` containment):** `GetPathDependence()` propagation across `IPainter`, `IScalarPainter`, `IRayIntersectionModifier`, and `IMaterial`; `AutoRasterizer` routes flagged scenes to PT; BDPT, VCM, and MLT refuse flagged scenes; SMS excludes flagged casters; OpenPGL skips flagged hits.
> - **Part I (Material Mixing & Blending):** `mix_material` (`MixMaterial`), `diffuse_transmission_material` (`DiffuseTransmissionMaterial`), and `blended_modifier` (`BlendedModifier`).
> - **Part II (Path-State Builtins):** `osl_layer_weight`, `osl_fresnel`, `osl_is_camera_ray`, and `osl_backfacing` (all propagating `PathDependence`).
> - **Part III (Ray-Traced AO):** `osl_ao` (physically based, `kNone`, backed by the folded 1-way `AOCacheKey` in `ExpressionMemo::Tables`).
> - **Still unshipped:** `scaled_material`, `passthrough_material`, and `allow_energy_gain`.
>
> The original draft 3 + Round 3 review record below is retained as historical design rationale.

**Original draft record (draft 3, 2026-11-14):** Draft 1, draft 2, and draft 3 underwent three independent review rounds. Following round 3 (2026-11-16), mixing and shadow visibility were prototyped and verified against failing closed-form tests before shipping the implemented subset above. Read what follows as the historical record of intent and audit decisions:

- **The leaf is drawn from the sampler once per hit**, stored on the record
  and copied to bidirectional vertices. The fixed patchwork of §I.1 (a hash
  of object instance and position) is withdrawn: round 3 showed its claimed
  agreement between queries does not hold, and a per-hit sampler draw is
  what alpha coverage already does.
- **§I.2's "25 `AlphaCoverage(` call sites" is wrong.** There are 4. The
  gates that decide whether alpha is tested at all are
  `IMaterial::AnyAlphaMaterials`, `LightSampler`'s scene-alpha census,
  `AcceptAlpha` and the alpha-mode pre-checks; a mix with a pass-through
  must register with all of them.
- **§I.5's prerequisite (step P) is implemented** (DL-633,
  `tests/UnlistedEmitterWeightTest.cpp`): an emitter the light table leaves
  out gets full weight in the path tracer and BDPT, and SMS no longer
  suppresses it. §I.5's own text below describes the defect as it was. Its
  review also found and fixed the same weighting error for an emitter
  lighting itself, and for the emission op when the path tracer's
  subsurface continuation reaches it. The legacy shader-op chain is frozen
  and keeps the defect on its own rays (not after a volume scatter, where
  the shared RayCaster continuation applies the corrected weight). Round 3's third site, the extended-SMS check that
  compares a hit's material with the luminary's, concerns mix luminaries
  and belongs to the mixing prototype.
- **§II.3(d) is not to be built as written.** Round 3 P1-7 lists four
  defects; the corrected rules are in that record. Its claim of matching
  Cycles for point lights is false for a diffuse transmitter with shadows
  off.
- **§II.3(e):** gated emission stays bounce-only by decision. Measured, it
  is 432 times brighter than Cycles' default in the test scene, not "about
  13". An emitter that reads only the view direction or `backfacing` may
  stay light-sampled.
- **SMS is switched off in any scene with path-dependent transparency or
  visibility masks**, not only where a shadow-dependent surface is specular.
- **Refusal** is a capability of the rasterizer, enforced at its own entry
  point; the `Job` pre-flight only supplies the message.

**Parent:** [BLENDER_PROCEDURAL_TRANSLATION_DESIGN.md](BLENDER_PROCEDURAL_TRANSLATION_DESIGN.md).
This note designs the RISE features that document's shader-level translation
depends on. All are general scene-language features; the Blender exporter is
their first user, not their definition.

| Part | Feature | Corpus need (2,418 materials) |
|---|---|---|
| 0 | What is physically based here, and how the rest is contained | |
| I | `mix_material`: a weighted set of materials, one chosen per hit | 40% contain a Mix or Add Shader |
| II | Path dependence: ray type, view direction and ray visibility | Light Path 6.2%; view-direction inputs 24.5% |
| III | `ao()`: ray-traced ambient occlusion in the expression VM | 6.4% use the Ambient Occlusion node |

The parts interlock: most Light Path use, and some Ambient Occlusion use, is
as a Mix Shader factor. So a mix must be resolved **after** the ray traversal
has finished, because its weight may need the ray's context, scene signals,
or rays of its own (§I.2).

No estimator in this note has been run. Every "unbiased" or "sums to one"
below is an argument to be confirmed by the test named beside it.

---

## Part 0 — Physical basis and containment

RISE froze its non-physically-based materials rather than extend them
(SCENE_CONVENTIONS §11.4–11.5). The features below are sorted against that
ruling.

| Feature | Standing | Reason |
|---|---|---|
| Mix with weights that depend on surface position only, summing to at most 1 | Physically based | An area mixture: a fixed patchwork of the leaf surfaces at a scale below the pixel. Choosing the leaf from the hit position renders that patchwork (§I.1). |
| Emission added to a surface | Physically based | A surface may reflect and emit. Mapped to the existing luminaire wrapper, not to a gain (§I.4). |
| `ao()` as a texture input | A scene-geometry signal | Like `occlusion()` and `proximity()`. It is an N-sample estimate fixed per surface point: a deterministic texture, labelled as an estimate (§III). |
| Add of two reflecting shaders | **Not physically based** | The sum can reflect more light than arrives: the unbounded-energy combiner that got `composite_material` frozen. |
| Inputs that read the arriving ray's direction (Fresnel, Layer Weight, Incoming, Backfacing, Reflection) as a colour or weight | **Treated as not physically based** | In general the response is not reciprocal (rejected on that ground in DL-37 and DL-127). Some special cases are reciprocal; they are not told apart. This is a support policy, not a proof about each case. |
| Inputs that read the ray's type or depth (Light Path) | **Not physically based** | By construction. |
| Shadow-only visibility | **Not physically based** | An object that light passes but sight does not. |

**Containment.** The non-physical features exist because Blender scenes use
them. They are admitted on five conditions, each a testable property:

1. **Declared.** Each is detectable by a capability: `PathDependence()`
   returns a mask of {view direction, camera, shadow, scatter class, depth,
   integrator-restricted}; a mix reports whether it may gain energy. The
   capability is computed for painters, programs, materials, modifiers and
   object visibility masks, through every wrapper, sampled painter and scene
   query. A hygiene census (in the style of `SourceHygieneTest`) fails if a
   composite painter or wrapping material does not forward it. A false
   negative here would produce a plausible, wrong image, so the census is
   part of the feature, not a follow-up.
2. **Announced.** A scene using any of them logs one warning per kind per
   load, naming the materials, and the chunk schema marks the parameters
   `nonphysical`, as deprecated chunks are marked today.
3. **Confined to one estimator.** Anything path dependent is defined along a
   path from the camera and rendered by the path tracer only (§II.4).
4. **Estimator independent.** Within the path tracer the image must not
   depend on how the path tracer samples: light BVH on or off, RIS light
   sampling on or off, optimal MIS on or off, HWSS on or off all agree. Path
   guiding and SMS are switched off where they would interact (§II.4). These
   are gates in §I.6 and §II.6.
5. **Ordered.** Declaration and refusal (conditions 1–3) ship before the
   first path-dependent input does (Order of work, step D1).

---

## Part I — `mix_material`

### I.1 Model

Any tree of Blender Mix Shader and Add Shader nodes is a weighted sum of its
leaf shaders, `f(x) = Σ wᵢ(x) · fᵢ`, where each `wᵢ` is a product of mix
factors along the path to leaf *i*. Mix gives `(1−t)` and `t`, with `t`
clamped to [0, 1] as Cycles does; Add gives `1` and `1`. The general object
is N-ary:

```
mix_material
{
	name               skin
	child              diffuse_layer   weight w_diffuse
	child              sss_layer       weight w_sss      modifier pores
	child              none            weight w_dark
	passthrough                        weight w_clear
	allow_energy_gain  FALSE
}
```

**Weights are response coefficients.** The material *is* `Σ wᵢ fᵢ`; nothing
is normalised. Each weight names a scalar painter (from the exporter, an
expression or a program output; constants make a mix usable by hand).

- A negative or non-finite weight is read as 0. The material counts those
  and logs once per render.
- Write `S` for the sum of all weights at the hit, pass-through included.
- **A bounded mix** (the default) has `S ≤ 1`.
  - The shortfall `1 − S` absorbs, exactly as a `none` child of that weight
    would. So a lone child of weight 0.25 reflects a quarter of what the
    child does, and `S = 0` is black.
  - If `S > 1` at a hit, the weights are divided by `S` there and the
    material logs once. The exporter never emits this: a tree of Mix
    Shaders sums to exactly 1.
  - **Pass-through is the mix's alpha coverage**, `1 − w_clear`: the hit is
    skipped with probability `w_clear`, by the existing alpha test (§I.2).
  - An accepted hit takes leaf *i* with probability `wᵢ / (1 − w_clear)`,
    and the absorber with what remains.
- **A gain mix** (`allow_energy_gain TRUE`) may have `S > 1`.
  - Leaf *i*, with pass-through counted as a leaf, is taken with probability
    `wᵢ / S`, and the chosen leaf's response is multiplied by `S`. The mix
    wraps each child in a `scaled_material` at construction to do that
    (§I.4).
  - The pass-through leaf is a delta pass-through carrying the factor `S`,
    because an alpha skip cannot carry a factor. `Add(Diffuse, white
    Transparent)` therefore transmits 1 and reflects the full diffuse.
  - The mix's own coverage is 1.

**Selection is a fixed patchwork.** The leaf is chosen by one uniform number
hashed from three things: a stable id of the object instance (derived from
the object's name at scene build, never from a pointer, so the pattern is the
same on every run), the raw bits of the object-space hit position, and a
salt belonging to the mix. (`ao()` and a program's `hash` builtins use other
salts, so a white-noise weight is not correlated with its own selector.) The
pass-through of a bounded mix is not part of the patchwork: it is the
sampler-driven alpha test, as for any alpha-covered surface.

- The same point of the same instance always resolves to the same leaf.
  Every query that reaches it agrees with no shared state: the hit, a shadow
  ray that ends there, the subsurface entry probe, a relief tap, the
  denoiser's retrace, both ends of a bidirectional connection.
- Different instances, and different points however close, are independent.
  Two congruent sheets sharing a mix do not select together.
- A BDPT or VCM vertex copies the leaf, the coverage stamp and the
  modifier's result from the hit, so a rebuilt record recovers them with no
  new decision.

**What is estimated.** A path that visits each surface point at most once
sees independent leaf choices with the right probabilities, so its expected
contribution is that of the weighted mixture, in any integrator, because
what each integrator renders is an ordinary scene whose material varies from
point to point. Three cases fall outside that argument:

- **The same point reached twice**, or coincident geometry inside one
  object, resolves identically both times. That is the patchwork's meaning,
  not an error, but it is not the mixture of independent draws.
- **VCM merging** joins an eye vertex and a photon at two different points,
  each with its own leaf (below).
- **Weights that are not functions of position alone** are path dependent
  (Part II).

**VCM merging.** Light vertices are stored only when connectible
(`VCMIntegrator.cpp:1523`), and deposits are already divided by the
acceptance probability stamped on the hit (`:1578`). A mix thins deposits a
second way: the photon is stored only if the leaf chosen at its point is
connectible.

- Let `a` be the hit's stamped coverage (§I.2) and `q` the probability,
  given the leaf draw, that the chosen leaf is connectible: for a bounded
  mix `q = Σ_connectible wᵢ / (1 − w_clear)`, for a gain mix
  `Σ_connectible wᵢ / S`.
- The deposit is divided by `a · q`, once. For a half diffuse, half
  pass-through mix that is 0.5 · 1, not 0.25.
- Continuing photon throughput is unchanged, as for alpha today.
- **Combined VC and VM weights need a derivation before step B ships.** Its
  outline: connections see one realised scene, so their weights partition as
  in any scene; the quantities a stored light vertex carries must not depend
  on the leaf at the deposit point (to be checked against
  `ConvertLightSubpath`); and the thinned deposit is an unbiased stand-in
  for an unthinned one, as the alpha argument in ALPHA_COVERAGE.md has it.
  As there, the claim is exact for constant weights and keeps the ordinary
  finite-radius boundary bias for varying ones.

**The other leaf kinds.**

- **`none`** is the existing null material: a black absorber. That is what
  an unlinked Mix Shader input is in Blender.
- **A tinted Transparent** is a real leaf: `passthrough_material { tint }`,
  a new small material whose only lobe is a delta ray continuing undeviated
  with `kray = tint`.
  - It reports `HasDeltaPassThrough` and `DeltaPassThroughTransmittance`
    (RGB and per wavelength).
  - It implements the companion-wavelength evaluation
    (`ISPF::EvaluateKrayNM` and the lobe-ratio form). The base default
    returns −1, which would zero three of four HWSS lanes.
  - As a path-independent surface it uses the delta pass-through machinery
    as built: a delta light's shadow ray is attenuated by the tint; area and
    environment light reach through it by the continuation. A surface whose
    transparency depends on the shadow flag is handled differently (§II.3d).
- **A thin diffuse transmitter** (Blender's Translucent) is the parent's new
  `diffuse_transmission_material` (parent §5.6). It tracks no interior and
  is an ordinary leaf.

**Why choose rather than blend.** A blended material would have to
re-derive, for arbitrary pairs of children, the aggregate sampling density,
the per-lobe selection weights, the HWSS companion evaluation and the delta
flags that each of RISE's multi-lobe materials needed several correction
rounds to get right. Choosing leaves every one of those contracts untouched.

**What choosing costs, relative to Cycles.** Cycles keeps every closure and
evaluates them all in light sampling, so it has lower variance on soft
mixes; RISE's blend emerges as samples accumulate and a 50/50 mix at one
sample per pixel is a dither. Cycles also turns paths and shadows opaque
after 8 transparent bounces by default and caps a shader at 64 closures;
RISE does neither.

**Denoiser guides.** A chosen leaf would make the albedo and normal guides
dither while they are declared noise-free. At a first hit on a mix the
guides are expected values over the leaves, given acceptance: each leaf
contributes what it would report standing alone, its normal after its own
modifier. The pass-through share follows whatever the guide pass already
does at an alpha-covered surface.

### I.2 Where the choice is made

`Object::IntersectRay` stamps `ri.pMaterial` at one line
(`Objects/Object.cpp:1575`), and `CSGObject::IntersectRay` overrides it at
one more (`Objects/CSGObject.cpp:3725`). Resolving there is wrong: it would
run for every BVH candidate, the scene signals are stamped later, and it is
inside the traversal, where a weight that casts rays (Part III) would
re-enter the BVH.

**Rule:** `Object::IntersectRay` stamps the unresolved mix, and resolution
happens once, after traversal, on the winning hit. In
`ObjectManager::IntersectRay` the order is:

1. stamp the scene signals (`CompleteShadingSignals`);
2. resolve the mix (below);
3. `BSSRDFSampling::ResolveCoincidentEntry`, which reads the winner's
   subsurface parameters and may replace the hit (today it runs before the
   signal stamp, `ObjectManager.cpp:1645–1647`);
4. if the hit was replaced, stamp and resolve again.

**Resolution** evaluates the weights on the record as traversal left it,
before any modifier, and writes four things:

| Written | Value |
|---|---|
| `ri.pMaterial` | the chosen leaf (or the absorber) |
| `ri.mixCoverage` | the mix's own coverage: `1 − w_clear` for a bounded mix, 1 for a gain mix. The field defaults to 1 on every record. |
| `ri.mixConnectible` | `q` of §I.1, default 1 |
| `ri.pModifier` | the leaf's modifier, if the child names one (below) |

**The mix's coverage survives resolution.** Draft 2 resolved the leaf and
then let the alpha test call `ri.pMaterial->AlphaCoverage`, which by then is
the leaf's: a half-transparent mix became opaque
(`ObjectManager.cpp:2962`). Now every alpha site reads

```
HitCoverage(ri) = ri.mixCoverage × ri.pMaterial->AlphaCoverage(ri.geometric)
```

through one helper, so the mix's coverage and a leaf's own alpha compose as
a product. There are 25 `AlphaCoverage(` call sites today: the sampled
closest-hit loop, the sampled shadow walk, the subsurface and random-walk
endpoints, emitter alpha and SMS. A hygiene test forbids a direct call
outside the helper. The value stamped as the hit's accepted coverage, which
deposits divide by, is `HitCoverage`.

**A child's modifier.** A child may name a modifier (`modifier pores`
above). When that leaf is chosen its modifier replaces the object's on the
record; integrators apply `ri.pModifier` after the hit exactly as they do
today (42 call sites in 15 files, unchanged). Replace, not stack: an author who wants
both names a `stack_modifier`. This is what lets a Blender mix whose leaves
have different bump or normal inputs translate (97 corpus materials in 26
files). The weights themselves never see a leaf's modifier.

**Entry points that resolve:** `IntersectRay` (and through it the sampled
closest-hit loop, which re-casts after each rejected hit and so resolves
each candidate it meets), the sampled shadow walk, and
`IntersectSingleObject` (`ObjectManager.cpp:1903–1915`), whose records are
shaded directly through the subsurface contact cast from PT, BDPT and
`RayCaster`. The intersection-only queries do not resolve.

Probes that call an object directly and then read the material (45 sites
outside `ObjectManager` and CSG: 20 in `IORStackSeeding.h`, 20 in
`ManifoldSolver.cpp`, and the SSS, random-walk and light-sampler probes)
resolve through one helper, `ResolveMaterialAtHit(ri)`, or are shown not to
need the material.

**Cost when there is no mix.** A process-wide demand counter, like the alpha
one, makes resolution a single predictable branch in a scene with no
`mix_material`. With a mix, the weights are evaluated once per candidate hit,
including hits the alpha test then rejects.

**Making a missed site loud.** Null BSDF, SPF and emitter is what the
existing `none` material returns, and scene-preparation code asks object
materials for those on every load, so "return nothing and log" would say
nothing. Instead:

- `IMaterial::IsUnresolvedMix()` is checked at the shading entry points of
  every integrator and the legacy shader dispatch; reaching one with an
  unresolved mix is an error-level log and, in debug builds, an assert.
- A source-hygiene test enumerates every direct `->IntersectRay(` and
  `.IntersectRay(` call on an object outside `ObjectManager` and fails on
  any site not on an audited list.

### I.3 What the audit found, and what a mix is not allowed to be

`GetMaterial()` appears on 109 lines in 24 files. The core shading paths
never call it: the path tracer makes no such call, and BDPT/VCM vertices,
SMS chain vertices and SMS photons copy the material from the hit record.
Round 1's recount confirmed those figures and added the routes to a material
that a `GetMaterial()` grep does not see:

| Route | Handling |
|---|---|
| 45 direct object intersections whose `ri.pMaterial` is then read | `ResolveMaterialAtHit` (§I.2) |
| `IntersectSingleObject`, `ResolveCoincidentEntry` | §I.2 order |
| Closed IOR-stack entries, which are **re-derived through the object**, not copied from the hit (`IORStackSeeding.h`, the re-indexing near line 4327) | A mix never puts an entry on the stack: see the containment restriction below |
| Alpha state, which is non-virtual on the `IMaterial` base and gated by a process-wide counter | The coverage stamp and `HitCoverage` (§I.2) |
| Type-keyed code: `dynamic_cast` censuses, the snapshot clone list, the motion-blur painter audit, the closed `ScattersFullSphere` list | Each gets a mix entry, or the child is refused (below) |
| Light-root vertices, where the emitter comes from the luminary object | §I.5 |
| `TimeClone()`, CSG's internal material copies, material-preview lookups | Each needs a mix case; the preview shows a mix as its expected-value blend or as unsupported |

Two existing sites are bugs under a mix as written and are fixed with it:
`ManifoldSolver.cpp:8398` compares a hit's material with the emitter
object's, and `:11828` re-fetches from the object with the hit material in
scope.

**Restrictions, refused at scene build with a diagnostic, keyed on
capability rather than on type:**

- **A mix is outermost.** It may not sit under `coated_material`,
  `fabric_material`, `composite_material`, a luminaire wrapper or an
  authored `scaled_material`; those bind their child's BSDF at construction.
- **No child that tracks containment, whatever the weight and with no
  exception:** a refracting dielectric or perfect refractor,
  `translucent_material`, a composite, or any child on an object with
  `interior_medium`. The refraction stack would enter through one leaf and
  leave through another. This holds for a constant fractional weight (BMW27's
  glass is a 0.8 mix of Glass and grey Transparent) and for a weight that
  depends on ray type. Draft 2 admitted one case, glass that is transparent
  to shadow rays only; by decision it is refused like the rest (about six
  corpus materials). `diffuse_transmission_material` tracks nothing and is
  allowed.
- **Subsurface children are allowed, under a constant weight in every
  integrator.** They report `canRefract` but shipped subsurface materials do
  not move the refraction stack. The entry probe inherits the exit hit's
  material, so a walk that starts in a subsurface leaf treats the whole
  object as that leaf's body: the intended meaning of the commonest corpus
  mix (Diffuse + Subsurface).
- **A subsurface child under a weight that varies over the surface is
  path-tracer-only.** A light-side subsurface jump kept by BDPT or VCM would
  take the weight at its entry point where the eye side takes it at the
  exit point, and draft 2 had no algorithm that makes the two agree. Such a
  mix sets the integrator-restricted bit of `PathDependence()`. 344 corpus
  materials have this shape; 278 of them read the view direction as well
  and are path-tracer-only regardless.
- **A mix is never a specular caster.** `GetSpecularInfo` answers no, and
  the hit record carries `bMaterialFromMix`. It must reach every consumer
  that classifies a resolved hit as a caster, not only the path tracer's
  guards: the SMS seed walks and the SMS photon tracer read resolved records
  too (36 `GetSpecularInfo` consumers in 10 files). A mirror or smooth
  subsurface leaf of a mix is then an ordinary delta bounce to every
  integrator and a caster to none. Gate: PT with SMS on equals PT with SMS
  off on a scene whose only specular surfaces are mix leaves.
- **A mix is not a random-walk neighbour** for the DL-370 crossing query.

**Per-object questions a mix answers conservatively** (an OR over its
children): `HasDeltaPassThrough`, and "may need the sampled shadow path".
`CouldLightPassThrough` is an OR over children but does **not** count the
pass-through, so a Transparent leaf plus a point light does not send Auto to
VCM.

### I.4 Add Shader, emission and `scaled_material`

An Add makes the weights sum to more than 1. What that means depends on what
is being added:

| Added | Mapping | Standing |
|---|---|---|
| A surface and an Emission | One leaf: the surface under the existing luminaire wrapper. No gain. | Physically based; the emitter can be light-sampled (§I.5). |
| Two Emissions | One emitter whose exitance is the sum, computed in the program. | Physically based. |
| A colour wired straight into a shader socket | An Emission of strength 1, as Cycles converts it. | Physically based. |
| Two reflecting or transmitting shaders | A gain mix (§I.1). | **Not physically based.** Allowed by decision, flagged (Part 0). |

151 corpus materials have an Add of two surfaces; 99 of them are Principled
+ Translucent (leaves and petals).

`scaled_material` multiplies its child's response by a scalar painter: BSDF
value, every scattered ray's `kray`, and emitted radiance. Densities are
unchanged, and a uniform scale leaves lobe selection probabilities unchanged
too, since they are ratios of `kray`.

- As an authored chunk its scale is clamped to [0, 1]: an energy trim.
- A scale above 1 exists only inside a gain mix, which builds the wrappers
  itself from its own weight sum. The exporter sets `allow_energy_gain` when
  the shader tree adds two surfaces, and that is what triggers the warning.
- It must forward the whole material surface: 18 `IMaterial` virtuals, 18
  on `ISPF` (the companion-wavelength evaluations among them, scaled), 9 on
  `IBSDF` and 7 on `IEmitter`, each forwarded or scaled.
- **Children of a gain mix are an allow-list**, because several things do
  not pass through a wrapper: subsurface throughput is read straight off the
  material by the integrators (a scaled subsurface leaf would scale only its
  surface reflection), and type-keyed code does not see through it. The
  first list is the plain surface materials: Lambertian, Oren-Nayar, GGX,
  the PBR material, `diffuse_transmission_material`, `passthrough_material`
  and the luminaire wrappers. A subsurface leaf in a gain mix is refused
  until subsurface throughput is scaled too; at most 8 otherwise-covered
  corpus materials are affected (an upper bound from material-level keys).

### I.5 Emissive leaves

Light sampling is per object: the luminary list, the selection weights and
the emitter evaluated at a sampled light point all come from the object's
material.

**Prerequisite, independent of mixing: an emitter outside the light list
must get full weight when a path finds it.** The path tracer does not do
this: `PathTracingIntegrator.cpp:2962–2971` substitutes a selection
probability of 1 for an unlisted emitter and then weights the hit against a
light-sampling strategy that does not exist (a 10 cm emitter 2 m from a wall
is weighted about 6e-7, a calculated figure). With RIS light sampling active
the hit is zeroed outright (`:2948`). To fix, each with a red-proof:

- "Can be light-sampled" means membership of the light table
  (`FindLuminaryIndex ≥ 0`), not geometry capability, at the path tracer's
  RGB/NM and HWSS sites, in `EmissionShaderOp`, and in the RIS branch.
- **BDPT is expected to have the same defect.** Round 2 read the source: the
  "strategy absent" flag is set from geometry capability
  (`BDPTIntegrator.cpp:4452`) and the MIS walk keeps its zero-to-one
  remapping at the emitter endpoint unless that flag is set (`:6958`). A
  furnace test of an unlisted emitter under BDPT is the proof, and the fix
  follows it.
- VCM is already correct (`VCMIntegrator.cpp:1990`).
- The path tracer's SMS emission suppression (`:2790`) must ask whether the
  emitter is one SMS can sample; otherwise an unlisted emitter behind a
  glass caster is suppressed and never supplied.

This is a latent defect in RISE today for any emitter that is not in the
table, and is worth fixing whether or not mixing is built.

**Step 1: bounce-only.** An unresolved mix reports no emitter, so the object
is not in the light list, and an emissive leaf is found when a path hits it.
Correct once the prerequisite is in, and noisy for small bright emitters.

**Step 2: mix-aware luminaries.** The emitter at a sampled point on a mix
luminary is resolved like a hit: the record is filled, the mix resolved, and
the chosen leaf's emitter evaluated (zero if that leaf does not emit). The
selection weight is the **expected** exitance, `Σ pᵢ · Mᵢ`, averaged over
the surface; averaging a hashed 0-or-1 choice on a 10×10 grid would be noisy
and could read zero at small weights. Not every emitter record goes through
`LightSampler::FillEmitterRecord` (the SMS light-directed seeding and BDPT
light roots rebuild their own), so the single point of resolution is a new
helper, `EmitterAt(luminary, record)`, and a census test over emitter
fetches.

A mix whose emissive weights are path dependent follows §II.3e.

### I.6 Tests

Closed forms, each under PT, BDPT and VCM, in RGB, spectral and HWSS. MLT
refuses a scene with these materials (§II.4) and is tested only for that.

- `mix(Lambertian ρ₁, Lambertian ρ₂, w)` against a Lambertian of albedo
  `(1−w)ρ₁ + wρ₂`, for constant and textured `w`.
- A nested three-leaf mix against its flattened weights.
- **Two congruent sheets**, as two objects and as two instances, sharing
  `mix(passthrough tint t, none)` at equal weights: transmission `0.25·t²`.
  (Round 2's counterexample; a position-only selector gives `0.5·t²`.)
- **The mix's coverage survives:** a half white-transparent, half opaque mix
  transmits 0.5; the same with a leaf that has its own alpha transmits the
  product; both equal the same surface under `alpha_coverage`.
- **Weights are coefficients:** a lone child of weight 0.25 has a quarter of
  its albedo; zero-total and NaN weights are black and counted.
- **Add with a white Transparent** transmits 1 and keeps the full diffuse.
- Add with gain: a gain mix of `A` and `B` against `A + B` where the sum is
  itself a known material with albedo at most 1.
- A tinted `passthrough_material` under a point light and an area light
  against `tint × unoccluded`; under HWSS all four lanes agree with
  `hwss FALSE` at matched quadrature.
- **A child's modifier:** a mix of two leaves with different normal maps
  equals the weighted sum of the two single-leaf renders.
- **VCM:** merge-only, connection-only and combined renders of
  mix(Lambertian, pass-through), mix(Lambertian, mirror) and
  mix(glossy, Lambertian, mirror), each converging to the path tracer's
  answer as the merge radius shrinks.
- An unlisted emitter in a furnace, per integrator, with light BVH and RIS
  on and off (§I.5 prerequisite), and behind a glass caster with SMS on.
- An emissive leaf with and without step 2, against the analytic exitance.
- Constant-weight Diffuse + Subsurface, PT ≡ BDPT ≡ VCM; a textured weight
  is refused outside the path tracer.
- SMS on equals SMS off; light BVH, RIS and optimal MIS on equal off.
- The hygiene tests of §I.2 and Part 0.

Cost to measure: resolution on a scene with no mixes (must be within noise
of zero), and on an all-mix scene, with and without alpha rejections.

---

## Part II — Path dependence

### II.1 What Cycles does

From the Cycles source and from eleven small test renders made for round 1
(the "E" numbers):

- The ray-type flags describe the ray that *arrived*: camera, or the class
  of the scatter that produced it (diffuse; glossy, which includes singular;
  reflection or transmission; volume scatter). A subsurface bounce clears
  the camera flag.
- A transparent (straight-through) bounce keeps every flag, including
  "camera", and counts only toward Transparent Depth.
- **A surface struck by a shadow ray is evaluated with the shadow flag
  alone** (`kernel/integrator/shade_shadow.h:48–49`), while the
  BSDF-sampled ray to the same light sees the scatter flags, and the two are
  combined by multiple importance sampling. So a surface that is
  transparent to shadow rays has **no single shadow** under a non-delta
  light. E5, floor under such a blocker, relative to no blocker: point light
  1.0000; 0.2 m area light 1.0000; 2 m area light at 4 m 0.9947; 10 m
  emissive panel at 2.5 m 0.672; constant white world 0.448, identical to an
  opaque blocker.
- **A light chosen by light sampling is evaluated with no flags**
  (`kernel/light/sample.h:88–91`; its source comment calls this "weak"),
  where the same light found by a scattered ray sees the real flags. Ray
  Depth on an emission or shadow evaluation counts one bounce further
  (`kernel/svm/light_path.h:55`). Whether a mesh emitter is light-sampled at
  all is decided from a static estimate of its emission exceeding 0.5
  (`scene/shader.cpp:254–273`). E3: a panel that emits only to diffuse rays
  lights a floor at 0.074 of the scatter-only value under default settings.
- Object ray visibility (camera, diffuse, glossy, transmission, shadow,
  volume scatter) is a mask tested against the same flags in traversal.
  Diffuse and glossy bits are stripped on transmission rays, and a *light*
  with restricted visibility is handled by excluding closure classes in
  light sampling, which a per-hit mask cannot reproduce.
- `I`, the Incoming vector, is the direction back along the camera path, so
  Fresnel, Layer Weight and Backfacing depend on which side of the path the
  camera is.

So Cycles is well defined for path-dependent *surfaces* only under delta
lights or when the shadow variant equals every scatter variant, and is
knowingly inconsistent for path-dependent *lights*.

### II.2 What the corpus does with it

| Use | Meaning |
|---|---|
| Is Shadow Ray → mix with Transparent (the commonest Light Path idiom) | casts no shadow, or a lighter one |
| Is Camera Ray → mix with Transparent | visible to the camera or not |
| Is Diffuse Ray → switch between two surface shaders | a cheaper or different look for indirect light |
| Is Glossy / Transmission Ray → mix with Transparent | visible in reflections or refractions or not |
| Ray Length, Ray Depth as a factor or colour | distance fades, depth cut-offs |
| Layer Weight or Fresnel as a mix factor or tint | the pre-Principled "Fresnel blend" of diffuse and glossy; rim tints |
| Backfacing as a mix factor | a different material on the inside |
| Object ray visibility | shadow off on non-light objects in 45 of 174 files, camera off in 14, diffuse off in 22: more files than use Light Path |

Emission that is really gated by ray type is about 15 materials or fewer
(round 1's estimate; the survey tool does not separate it). The frequent
"Is Diffuse Ray → Add" idiom is a surface switch, not gated emission.

### II.3 Design

**(a) A ray context on the hit record.** A small value (flag bits and depth
counters, about 8 bytes) on `RayIntersectionGeometric`.

| Context | Flags |
|---|---|
| camera ray | camera |
| ray from a scatter | the scatter's class, from the table of (b) |
| after an alpha, pass-through or `passthrough_material` skip | unchanged; transparent depth + 1 |
| shadow or visibility ray | shadow only |
| hand-built record (light sample, probe) | none |

Where it is set, since the caster cannot always reach the record:

- It is a **parameter of the queries**: the sampled closest-hit loop and
  the shadow walks build their records inside `ObjectManager` and
  `RayCaster`, and increment transparent depth inside the alpha loop.
- It is added to `PropagateCastInputs` and to both hand-written record
  copies, or the BVH leaf copy-back wipes it.
- `RAY_STATE` gains the exact class of the scatter that produced the ray
  (it has only a collapsed type today), because the path tracer is also
  entered through `RayCaster::CastRay` with a precomputed hit.
- The denoiser's guide retrace casts camera rays and stamps camera context.

**(b) A published scatter-class table.** For every SPF lobe in the tree:
diffuse or glossy, reflection or transmission, singular or not. It is not
`ScatRayType`: GGX's multiscatter compensation lobe is typed diffuse
(`GGXSPF.cpp:473`) and must read as glossy here. Subsurface exits read as
diffuse reflection with the camera flag cleared; a phase-function scatter
reads as volume. The table is a test fixture: a surface whose colour is the
flag, after each lobe of each material.

**(c) Exposed three ways.**

- Program inputs: `ray_camera`, `ray_shadow`, `ray_diffuse`, `ray_glossy`,
  `ray_singular`, `ray_reflection`, `ray_transmission`, `ray_volume`,
  `ray_length`, `ray_depth` and the per-type depths; and the view-direction
  inputs `I` and `backfacing` (parent §5.2d).
- The `PathDependence()` mask of Part 0, computed for a program at compile
  time from the inputs it reads, following `sample()` references and scene
  queries.
- Object ray visibility: a per-object mask beside `visible` and
  `casts_shadows`.

**(d) Shadow visibility: one definition.** *Direct light is gathered
straight through a shadow-dependent surface, as its shadow context says;
light does not get a second route through the same surface. Everything else
sees the surface in the context of the ray that reached it.*

A **shadow-dependent surface** is a material whose `PathDependence()`
includes the shadow flag, or an object with `casts_shadows FALSE` (shadow
visibility off).

The **shadow-context transmittance** of a segment is what a walk along it
finds when each shadow-dependent surface is evaluated in shadow context:

- uncovered there (alpha, or a mix's pass-through): the walk skips it with
  the usual probability;
- a tinted pass-through leaf there: the walk multiplies by the tint, in RGB
  or at the wavelength, and continues, **for every kind of light**;
- an object with shadows off: skipped;
- anything else: blocked.

Surfaces that are *not* shadow dependent behave on this walk as on today's:
sampled alpha, the delta pass-through rule (tint for delta lights only), and
medium boundaries and attenuation as recorded today.

In the path tracer the definition is three rules. An **anchor** is a vertex
where direct light is gathered (a non-delta vertex); the camera is not one.

1. **Light sampling.** The shadow ray is the walk above.
2. **The emission partner.** A BSDF-sampled ray leaves an anchor. If, up to
   and including the surface where its crossing prefix ends (rule 3), its
   scatter-context walk met any shadow-dependent surface, then the emitter
   or environment credited to this ray *at the anchor* is looked up by a
   shadow-context walk along the same straight ray: the first emitter it
   reaches, or the environment, times the walk's transmittance. The
   densities are the ones used today for a BSDF-sampled emitter hit (the
   BSDF density at the anchor, the light's density for the point found); the
   emitter endpoint's own alpha is accepted once, as today; media attenuate
   along the walk. Both strategies then sample the same function, so their
   weights sum to one.
3. **The crossing prefix.** From the anchor, the path's consecutive
   crossings of shadow-dependent surfaces (an alpha or pass-through skip, or
   a transmission through the surface, delta or not) form its crossing
   prefix. Light sources met at the end of the prefix, and direct light
   sampled at a prefix vertex toward its far side, are dropped: rule 2
   stands for them. The prefix ends at the first event that is not such a
   crossing, and from there nothing is dropped. A path from the camera has
   no anchor: camera → shadow-off glass → light is seen, refracted.

What the rules give:

- **A surface opaque to sight and transparent to shadow rays** (the
  commonest idiom): the sampled ray hits it; rule 2 looks straight through
  it for the emitter; the path continues from the hit.
- **A partly shadow-transparent or shadow-tinted surface:** the walk applies
  its coverage and tint under both strategies. Draft 2 said this needed
  nothing extra; it needs this walk, because the existing pass-through rule
  tints shadow rays for delta lights only.
- **A shadow proxy** (opaque to shadow rays, absent otherwise): the sampled
  ray passes it and may reach an emitter; rule 3 drops that emission and
  rule 2's walk, which the proxy blocks, supplies none.
- **Glass on an object with shadows off:** light sampling passes straight
  through; the refracted continuation's emission is dropped by rule 3
  instead of being added to it (the DL-344 double count; Cycles has it
  unless caustics are off). Light the glass *reflects* onto a neighbour is
  untouched, as is a caustic from an ordinary mirror seen through that
  glass: the mirror ends the prefix.
- **SMS** would supply the dropped caustic again. It is switched off, with a
  warning, in a scene where a shadow-dependent surface has a specular lobe.
  Draft 2 proposed reusing the SMS emission guard for rule 3; that guard
  needs SMS on, an anchor and a covered caster chain, and cannot serve.
- RISE's existing `casts_shadows FALSE` is honoured on shadow queries only
  (`ObjectManager.cpp:306, 759, 1743`), so it has Cycles' split today under
  area and environment light. It adopts this definition in the path tracer;
  a red-proof renders a `casts_shadows FALSE` object under a large emitter
  against the same scene with the object's direct-light blocking removed
  analytically.

The cost is one extra walk per sampled ray that meets a shadow-dependent
surface, and nothing otherwise.

Against Cycles this matches for point and small lights and gives a lighter
shadow under large lights and sky, where Cycles has no single answer.

**(e) Emitters.** What an emitter's response depends on decides how it is
sampled:

| Emission or emissive mix weight reads | Sampling |
|---|---|
| nothing path dependent | light-sampled as usual |
| the camera flag only | stays light-sampled: light sampling is never the camera segment, so the sampled value (camera = 0) and the bounce-found value agree. This keeps the common camera-hidden light panel efficient. |
| depth, transparent depth, ray length, scatter class (Is Diffuse / Glossy / … Ray), the shadow flag, or view direction | **bounce-only**: left out of the light list and found by scattering at full weight (§I.5 prerequisite), evaluated with the arriving ray's real counters |
| an object visibility mask that excludes diffuse or glossy rays | bounce-only, for the same reason |

Draft 2 put depth beside the camera flag. That was wrong: a sampled-light
record has no depth, and an emitter that lights only at depth 2 or more
would be evaluated as dark by light sampling and then down-weighted where a
bounce finds it lit.

Bounce-only is the one well-defined answer. It will not match Cycles, which
light-samples such an emitter with every flag at zero and then down-weights
the bounces: in round 1's test RISE would be about 13 times brighter than
Cycles' default. The exporter reads the material's emission-sampling setting,
replicates Cycles' 0.5 rule, and warns when the two will differ.

### II.4 Integrators

Path dependence is a statement about the path *from the camera*. A light
subpath does not know it: the context at one of its vertices is set by the
scatter at the next vertex toward the camera, sampled later, or by a
connection. View direction has the same problem: BDPT, VCM and MLT rebuild a
vertex's record with the ray re-aimed per query
(`PathVertexEval.h:369, 415, 443, 702`), so `I` at a light-subpath vertex
points at the light, and a Fresnel-weighted mix would resolve differently at
the two ends of a connection.

| Integrator | Support |
|---|---|
| Path tracer (RGB, spectral, HWSS) | The definitions above. |
| Path guiding | Switched off, with a warning, in a scene with any path dependence: a guided draw carries one scatter class per material (`PathTransportUtilities.h:66`). |
| SMS | A path-dependent material is never an SMS caster: the solver rebuilds chain vertices without the arriving ray's class or depth, so it could price a chain differently from the path that realised it. Switched off where a shadow-dependent surface is specular (§II.3d). Otherwise it interacts only through §I.5's prerequisite. |
| Auto | The path-dependence check runs first, before the author pin and before the probe, and selects the path tracer. A pin to BDPT or VCM on such a scene is refused like an explicit choice. |
| BDPT, VCM chosen explicitly | Refuse to render a path-dependent scene, naming the materials. No silent fall-back. |
| MLT | Refuses any scene containing `mix_material`, `scaled_material`, `passthrough_material` or a path-dependent input. It is frozen, and nothing here is tested under it. |
| Legacy shader-op rasterizer, photon maps, irradiance cache | Refused: they have no path context or cache radiance across contexts. |
| Object **camera** visibility | Every integrator. The object is absent from the camera segment in both directions: camera rays skip it; a light-tracing splat from a vertex on it is not deposited; and the visibility test of any connection to the camera ignores it. No path then has its first vertex from the camera on the object under any strategy, so nothing needs re-weighting. |
| Object **shadow** visibility and `casts_shadows` | The §II.3d definition under the path tracer. BDPT and VCM keep today's behaviour for `casts_shadows`, which is recorded as a debt-ledger row; material-level shadow dependence is refused there. |
| Other object visibility masks (diffuse, glossy, transmission) | Path tracer only. |

Refusal needs a return channel that does not exist: `RasterizeScene` returns
nothing. The check is a pre-flight in `Job`, returning a message the CLI,
the Blender bridge and the GUIs each surface.

**Not planned: an eye-only rule for BDPT and VCM**, in which a
path-dependent surface may be an eye-subpath vertex and the MIS walk drops
every strategy that would place one on the light side. It changes
`MISWeight`, and is worth reopening only if path-dependent scenes turn out
to need caustics.

**A possible narrowing, not assumed:** `backfacing` on a surface with no
transmitting lobe may be valid in every integrator, since both ends of any
contributing path are then on the same side. It is counted as
path-tracer-only until a test shows otherwise. (`ao()`'s default normal uses
exactly this argument, §III.3, and is the test case.)

### II.5 What this costs in reach

By the end of the plan 23.7 points of the 92.1% of materials translated are
path-tracer-only (parent §9), so a scene containing one cannot use VCM for
caustics. By cause, for the materials where it is the only one: view
direction 213, Light Path 24, a varying subsurface mix 14, facing-normal AO
on a transmitting surface 3; the rest have several. Where the artist's
intent is a physical layered surface (the Fresnel blend of a diffuse and a
glossy shader), a later exporter option could map it to `coated_material`
or `ggx_material`, which is physically based and works everywhere, at the
cost of not matching the graph literally. Not scheduled; the survey must
first count that idiom.

### II.6 Tests

- The scatter-class table of §II.3b.
- Flags survive an alpha skip; depths count as specified.
- Shadow visibility: a floor under a shadow-transparent object, under a
  point light, a small and a large area light and a uniform environment,
  each against the same floor with the object removed; and the same with
  light BVH, RIS and optimal MIS toggled. The half-transparent case against
  half the shadow.
- **Shadow-only tint under an area light:** `tint × unoccluded`, from each
  strategy alone and combined.
- `casts_shadows FALSE` under a large emitter (§II.3d red-proof).
- **Shadow-off glass:** the floor behind it equals the floor with the glass
  removed (straight light, no caustic added); the camera still sees the
  light refracted through it; a rough-glass variant; a mirror caustic seen
  through it is kept; SMS is reported off.
- A shadow proxy darkens exactly as an opaque object's shadow.
- Camera-only visibility under every integrator, including a splat.
- A camera-gated emitter: light-sampled result equals bounce-only result.
- **A depth-gated emitter** (`ray_depth ≥ 2`) and a diffuse-gated emitter,
  each against the closed form for bounce-only, with light BVH and RIS on
  and off.
- Auto routes to the path tracer; a pinned or explicit BDPT/VCM refuses
  with the material named; MLT refuses; guiding is reported off.
- A path-dependent mirror with SMS on equals SMS off.
- The `PathDependence()` census of Part 0.
- Cycles parity for the idioms of §II.2 under a point light, gated; under
  area lights and for gated emission, reported with the measured difference.

---

## Part III — `ao()`

### III.1 What Cycles does

From `kernel/svm/ao.h`: N rays (the node's Samples, default 16),
cosine-distributed about the normal, drawn from the path's random numbers;
the normal is the node's Normal input if linked and otherwise the shading
normal `sd->N`, which faces the arriving ray; flipped for Inside; origin at
the shading point, length = Distance in world units (an unlinked 0 means the
world's AO distance); a ray is occluded by any object that casts shadows,
**with no shader evaluated**, so alpha-mapped surfaces occlude fully; Only
Local restricts occluders to the same object. The AO output is the
unoccluded fraction; the Color output is that fraction times the Color
input. It returns 1 on shadow rays, on sampled lights and in displacement.

In a Bump node's offset evaluations the rays start at the same point with
the same random numbers, so the *fraction* has no gradient. The Color
output still carries the gradient of its Color input. Round 2 rendered it:
a sine through AO's Color into Bump gives 0.4961 (mean red), the sine
straight into Bump 0.4959, no bump 0.6143. Round 1's test E2, which drove
Bump from the AO output with constant inputs, was the flat special case.

### III.2 Why the existing `occlusion()` is not this

`occlusion(r)` is a property of the surface alone: sphere-traced in an SDF's
own field, or baked once per vertex on a mesh. It cannot see other objects,
cannot take a world distance, a computed distance or a custom normal. It is
the right tool for hand-authored wear masks and stays.

### III.3 Design

A program builtin, `ao(distance, samples, normal, flags)`, implemented
beside `proximity()` as a scene-side query. It returns the fraction; the
Color output is program arithmetic on it.

- **It is a deterministic texture built from an estimate.** The value is the
  unoccluded fraction of `samples` rays, fixed for a given point, instance
  and time. Cycles redraws its rays on every path sample. Averaged over a
  pixel the two see the same kind of N-sample fraction, but not the same
  sampling law, so through a steep ramp their means can differ. That
  difference is measured and reported (§III.4); it is not promised away.
- **Rays.** A fixed low-discrepancy cosine set, shifted by a
  two-dimensional Cranley-Patterson offset hashed from the object instance,
  the hit position and `ao()`'s own salt. (A rotation in azimuth alone is a
  biased quadrature in the polar angle.)
- **Occluders.** The plain any-hit shadow query, `IntersectShadowRay`,
  which honours `visible` and `casts_shadows` and reads no material. Not
  `IntersectShadowRayOpaque`: that one skips every object whose material
  reports a delta pass-through (thin weave, and any mix with a tinted
  Transparent leaf), which would let AO leak through them.
- **Only Local** needs a query that does not exist: a single-object any-hit
  test that honours `casts_shadows`. `IntersectSingleObject` is closest-hit,
  forces both faces, stamps signals and resolves mixes.
- **Which hemisphere.** About the supplied normal, or by default Cycles'
  rule (by decision): the shading normal *before modifiers* (`N0`, parent
  §5.2d), on the side the ray arrived from; flipped for Inside.
  - "Side" is decided by the geometric normal and the arriving direction,
    and `N0` is flipped to agree, so a direction between the shading and
    geometric horizons cannot flip it.
  - Draft 2 used `UnflippedGeomNormal()` here, which is the geometric
    normal and would facet a smooth-shaded mesh, and fixed the side to the
    object's outward one, which differs from Cycles on back faces.
  - **Consequence.** On a material with no transmitting lobe both ends of
    any contributing path are on the same side, so the default is the same
    in every integrator. On a material that transmits, the side depends on
    which way the path runs: default-normal `ao()` there is a
    view-direction input and is path-tracer-only. The census decides this
    from the program (does it call `ao()` with no normal?) and the material
    (does it transmit?) together. 75 corpus materials use the default
    normal; 5 of them transmit.
  - A sampled-light record has no arriving ray: the side is the one the
    emission leaves from. Hair has no true side; the ray-derived normal is
    used.
- **In a bump or relief evaluation** the occlusion is taken at the centre
  point: the tap record carries the centre hit's position and instance, and
  `ao()` traces from there with the centre's ray set. Its arguments
  (distance, normal) are evaluated at the tap. So the fraction has no
  gradient unless its own inputs do, and anything multiplied by it keeps its
  gradient, as in Cycles. Nothing is folded away.
- **Evaluated once per hit.** `ao()` has its own per-thread memo entry, so
  it does not wait for the program executor work (parent §5.2a) and can be
  built first. Its key covers everything the value depends on: the scene and
  its generation, the time, the object instance (Only Local, and the ray
  offset), the program and call site, the centre position, the facing side
  and the arguments. It follows the existing memo's contract, which already
  keys cross-object queries on scene, object, world point and time and
  invalidates by generation (`Utilities/ExpressionMemo.h`).
- **On sampled lights.** A light-sample record has no scene pointer unless
  the emitter probe is requested, and that request tests curvature and
  occlusion demand only. It gains an `ao()` demand, so `ao()` reads the same
  value on a sampled light as on a hit. Without that it would silently read
  1 there, which is the Cycles behaviour the owner decided against.
- **Re-entrancy.** Painters are not evaluated inside an object-BVH
  traversal (alpha is tested between recasts, modifiers after the hit, and
  §I.2 keeps mix resolution outside). The one exception is `sdf_geometry`,
  which evaluates a function during its own march, on a record with no scene
  pointer: `ao()` returns 1 there, before its guard. The guard is a
  per-thread "in traversal" flag asserted clear on entry; it must cover the
  thread-local traversal stacks as well as the mesh mailbox.

**Where RISE differs from Cycles.**

- By decision, `ao()` is evaluated wherever the program is: on shadow-ray
  surfaces and sampled lights too, where Cycles returns 1. AO-driven
  emission and AO-driven shadow transparency show it.
- The estimate is fixed per point where Cycles' is redrawn per path. A
  threshold-like ramp on the AO value shows it.

### III.4 Tests

- A point on a plane at height *h* under an infinite parallel plane, and a
  point in a right-angle corner: the mean over many hits converges to the
  closed-form cosine-weighted visibility; the distribution of the 16-sample
  fraction is compared with Cycles' on the same scene.
- Only Local ignores a neighbouring object, including a second instance of
  the same geometry; Inside reads the back hemisphere; `casts_shadows FALSE`
  objects do not occlude; an alpha-mapped card, a thin weave and a
  tinted-transparent mix all occlude fully.
- **Bump:** a sine through `ao()`'s colour product into a relief modifier
  equals the sine alone into it; `ao()` alone with constant inputs is flat.
- **Normal:** a smooth-shaded coarse sphere beside an occluder shows no
  facets; the back face of an open sheet reads the hemisphere on the side
  the ray came from.
- **Cache:** an occluder moving during the shutter; two scenes that share a
  program and coordinates; repeated evaluation at one hit is identical.
- Identical results under PT, BDPT and VCM on non-transmitting surfaces; a
  transmitting material with default-normal `ao()` is refused outside the
  path tracer; the same value on a sampled light as on a hit of that light.
- Against Cycles: a contact-shadow scene through a linear ramp (gated) and
  through a steep ramp (the difference reported).

---

## Order of work

| Step | Delivers | Depends on |
|---|---|---|
| P | Full weight for emitters outside the light list: PT, HWSS, `EmissionShaderOp`, RIS, the SMS gate; BDPT and MLT fixed where the furnace test fails. A fix to RISE as it stands. | nothing |
| D1 | **Declaration and refusal:** `PathDependence()` on painters, programs, materials, modifiers and object masks, with its census; the pre-flight in `Job`; Auto routing; the guiding and SMS switches; MLT's refusal. | nothing. **Must precede the first path-dependent input** (the parent's Phase 1 ships `I` and `backfacing`). |
| S | The shadow-context walk, the emission partner and the crossing-prefix rule in the path tracer; `casts_shadows` adopts the definition. | P. **Must precede the exporter's shadow-visibility mapping** (parent step 0). |
| A | `ao()`, with its own memo, the single-object any-hit query, the tap-centre field and the emitter-probe demand. | D1, for the facing-normal classification |
| B | `passthrough_material`; `scaled_material`; `mix_material`: resolution after traversal, the coverage stamp and `HitCoverage`, the instance-keyed selector, a child's modifier, the VCM divisor with its derivation, expected-value guides, the hygiene guards. | P for emissive leaves; D1 |
| C | Mix-aware luminaries (§I.5 step 2). | B |
| D2 | Ray context and class table; the `ray_*` inputs; material-level shadow dependence through S; the emitter rules; object ray visibility masks. | B, D1, S |

P, D1 and S are fixes or guards on RISE as it stands and go first. B is the
long pole. Object **camera** visibility needs none of this: a small object
flag and exporter work, in the parent's step 0.

Round 2's verdict stands as the gate on this table: B, S, D2 and A do not
start until their contracts above have been reviewed again. The procedural
kernels, the survey and the performance measurements of the parent are not
blocked.

## Decisions

Decided by the owner on 2026-11-14: 1–9 before and after review round 1,
10–12 after round 2. Decision 7 lost its exception in round 2.

1. **Mixing** is one leaf chosen per hit.
2. **Ray-type-dependent emission** is found by bounces only, at full
   weight, knowing it is about 13 times brighter than Cycles' default in the
   one case measured. Emitters that depend on the camera flag alone stay
   light-sampled.
3. **Explicit BDPT, VCM or MLT** on a path-dependent scene refuses; Auto
   routes to the path tracer.
4. **View-direction inputs** (Fresnel, Layer Weight, Incoming, Backfacing,
   Reflection) follow the same rule as Light Path: path tracer only.
5. **Add of two surfaces** is allowed with energy gain, behind an explicit
   flag and a per-load warning.
6. **Shadow visibility** means "direct light ignores the object, under both
   sampling strategies", and `casts_shadows` adopts it.
7. **A refracting leaf in a mix** is refused, with no exception.
8. **`ao()`** is evaluated on shadow rays and sampled lights too.
9. **Names:** `mix_material`, `scaled_material`, `passthrough_material`,
   `ao()`, `ray_*`. The pass-through is not called `none`, which already
   names the black absorber.
10. **Blender's Translucent BSDF** maps to a new thin diffuse-transmission
    lobe (parent §5.6), which may be a mix leaf.
11. **A mix child may name its own modifier**, applied when it is chosen.
12. **`ao()`'s default hemisphere** is Cycles': the unmodified shading
    normal on the side the ray arrived from. On a transmitting material that
    makes it path-tracer-only.

**Not an owner decision, but stated to the owner with round 2's:** the
selector stays a fixed patchwork keyed on the object instance and the hit
position. Round 2 asked for an independent draw per transport event; the
reasons for not taking that are in
[round 2](MIX_MATERIAL_PATH_STATE_AO_DESIGN.md) §3. It is the first
thing a third review should attack.

## Open items

Not decisions; things this draft asserts that only an implementation or a
measurement can settle.

- The combined VC/VM weight derivation for a mix, and whether the
  quantities a stored light vertex carries are independent of the leaf at
  the deposit point (§I.1).
- The BDPT furnace test for an unlisted emitter, expected to fail (§I.5).
- The measured difference from Cycles for shadow visibility under large
  lights, for gated emission (§II.6) and for `ao()` through a steep ramp
  (§III.4).
- Whether `backfacing` on a non-transmitting surface is integrator
  independent (§II.4).
- The cost of mix resolution (per candidate hit when alpha rejects), of a
  child's modifier in the guide pass, and of the shadow-context emission
  lookup.
- How many Fresnel or Layer Weight nodes take a linked (bumped) Normal: the
  survey does not record it, and a weight that reads a leaf's own bumped
  normal is not expressible as specified.
- Whether switching SMS off for scenes with shadow-off glass is too blunt;
  the alternative is a chain rule inside SMS's own visibility tests.
