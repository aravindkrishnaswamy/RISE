# Woven Cloth and Fabric — Weave-Structured Appearance as an Authorable, Summonable Material System

**Status:** **Phases 1–3 in the tree.** Phase 1 SHIPPED 2026-09-02. Phase 2
SHIPPED 2026-09-03 (slice P2-A, the structured `weave_material` triad, then
slice P2-B, thin-cloth transmission — see §10.1a; P2-B left two known
integrator-side debts open, **debt 20** — BDPT/VCM 100–350x over-count a
full-sphere-transmissive weave, **RESOLVED 2026-09-04**: the measurement did
not reproduce once an unrelated same-day fix (debt 21, below) corrected the
PT reference it was compared against — see §15 debt 20 — and **debt 21** —
PT's diffuse-transmission lobe scaled near-`transmit²`/`transmit^1.7` instead
of linearly, **RESOLVED 2026-09-03**: not an integrator MIS defect at all,
but a shadow-ray self-intersection epsilon bug in the shared
`RayBilinearPatchIntersection` geometry routine — see §15 debt 21). P2-B also
left one MATERIAL-side defect that no gate then in the tree could see, found
in review round 8 and fixed the same day: **debt 22** — a
`fabric_material` wrapping a `transmission thin` `weave_material` silently
extinguished the weave's transmission, so a sheen layer over a sheer curtain
rendered it 100 % opaque, **RESOLVED 2026-09-04** by forwarding the two
full-sphere flags and modulating the substrate's transmission with the same
Kulla-Conty product law the reflect side uses — see §15 debt 22. A follow-up
round on 2026-09-04 closed the last two integrator debts P2-B left and opened
two new, unrelated ones it uncovered: **debt 23** — BDPT/VCM dropped NEE on the
`gap` fraction of hits at a mixed delta+continuum vertex, because vertex
connectibility was read off the ONE continuation lobe that was drawn instead of
the material, costing exactly `(1−gap)` (0.900 at linen's default gap),
**RESOLVED 2026-09-04**; **debt 24** — VCM's NEE zeroed its light-side MIS
alternative for every DELTA light, so the t=1 light-tracing splat double-counted
the same path, a fabric-independent **+3.0 %** on any omni/spot-lit scene (a bare
Lambertian quad shows the identical figure), **RESOLVED 2026-09-04**; and two new
findings, **debt 25** — a CLOSED solid carrying a thin-transmissive weave
read BDPT/VCM ≈ 0.17× PT while free-standing weave planes read 1.000,
**CLOSED 2026-09-05**: root cause was `box_geometry`'s own self-hit root at
the ray origin's published face (a fourth "PT may be the broken one"
instance), not a closed-solid effect in general — see §15 debt 25 — and
**debt 26** — the legacy `pixelpel_rasterizer` reads 2.46× under the modern PT
on a gapped weave in front of an area emitter (0.0416 vs 0.1024),
**RESOLVED 2026-09-05**: not a rasterizer defect, but the scene's own
`DefaultDirectLighting`-only shader chain, which casts no continuation ray at
all (a plain dielectric pane renders 0.000000 under it, and adding
`DefaultRefraction` brings the legacy number to 0.991 of the modern PT);
`BDPTStrategyBalanceTest`'s PT reference is now
`pathtracing_pel_rasterizer` and the area-lit topology is its topology F —
see §15 debt 26.
Debt 25's fix exposed **debt 27** — a two-layer gapped weave reads PT UNDER
BDPT/VCM by 1.28–1.55× because PT's binary NEE cannot reach a path through
the far layer's delta gap and the near layer's continuum lobe, **OPEN**, not
a closed-solid effect. Phase 3's
weave-resolving-geometry scope (yarn-density loop/crossing geometry) stays
**declined**, on the same precedent and for the same reasons as before; what
shipped instead is the bounded `add_fuzz` verb — a sparse fuzz-shell groom
reusing the existing `hair_geometry`/`hair_material` machinery — SHIPPED
2026-09-03 (§11.3). What is locked is the design. Four review rounds (one
citation audit, one adversarial design pass, two fresh-eyes rounds) have been
applied in full — see the Amended block below for what moved and why. Phase
1's scope, the `fabric_material` contract (§9.2), the chunk surface (§9.3),
the sampling decision (§9.4), the anisotropy split (§9.5) and the exit gates
(§9.9) are the implementation brief; changing any of them is an amendment to
this document, not an implementation choice. Phase 3's weave-resolving-geometry
scope remains gated and is **not** locked; its bounded `add_fuzz` slice is
locked as shipped (§11.3).

**Phase 2 status, 2026-09-03 — slice P2-A is in the tree.** All three of
§10.2's entry conditions were satisfied (the gate-9b deficit, the demand
census, and §10.1's primary-source read), the read was done against the
primary PDFs, and `weave_material` — a structured two-thread-family cloth
BSDF — shipped as its own triad rather than as a mode on `fabric_material`.
§10.1 and §10.3 below are rewritten with what the read actually found and
what was built. **Slice P2-B, the transmission lobe, has NOT shipped**:
`ScattersFullSphere` and `CouldLightPassThrough` stay false, so the backlit
glow-through cue is still unserved, and the §10 goal's third clause is the
part still outstanding.

**Fix round 2, 2026-09-03 (same day, third pass).** Two more fresh reviews
(REVIEW_P2R4.md, REVIEW_P2R5.md) found one further P1: `ProjectDir`'s
per-family azimuth has a coordinate pole at view latitude `90 - tilt_deg`
(where a direction approaches the fibre axis and "front of thread" vs "back
of thread" is undefined), and the masking term's hard `max(cosPhi,0)` hinge
turned that pole into a real, in-spec discontinuity: at the old
`kMaxTilt = 0.6` rad the pole sat at an ordinary ~55.6 deg view angle and
produced a 48.9% brightness drop in a 0.5 deg step. Fixed three ways, all in
`WeaveBRDF.cpp`/`.h`: (a) `ProjectDir` blends the azimuth ratio toward the
pole's own neutral value (0) as the in-plane projection vanishes, rather
than trusting `atan2`'s sign arbitrarily close to the pole; (b) the masking
gate itself is `SmoothedRamp`, a C1 quadratic mollification of the SAME
`max(x,0)` hinge (matching value, not just clamping) so it stays the graded
cosine-weighted occlusion factor Sadeghi's model is, rather than becoming a
saturating front/back gate (a saturating smoothstep was tried first and
rejected -- it inflated denim's hemispherical albedo 25-30%); (c)
`kMaxTilt` is bounded to 0.17 rad (~10 deg) so the pole cannot sit inside
80 deg for any in-range tilt, on top of (a)+(b) rather than instead of
them. `WeaveMaterialChunkTest`'s new `MaskingPoleSeam` sweeps view latitude
in 0.25 deg steps at `tilt = kMaxTilt` for both families and asserts no
adjacent-sample ratio exceeds 1.10 below 87.75 deg (measured post-fix
worst case: 1.0019 at the new `kMaxTilt`, and 1.0091 even at the OLD 0.6 rad
tilt that produced the original 48.9% drop) -- the mutation-catching guard
neither review round before it had. One `LayeredWhiteFurnaceTest` row moved
outside its locked eps as a real consequence of fixing the masking gate
(satin, rotation 0, theta=80: 0.2272 -> 0.2164, -4.8%) and was re-locked
with the delta stated in its own comment; every other row moved by
<= 0.0002. Reciprocity and the pdf hemisphere integrals were re-verified
unchanged (the smoothing is applied identically to `wi` and `wo`).

**Fix round, 2026-09-03 (same day, second pass).** Three independent reviews
of the just-landed slice found the volume lobe's normaliser `C_v` was a loose
analytic BOUND shipped as if it were the exact one — 2.0–2.3× too large,
halving every fabric's dominant lobe — plus five smaller correctness/honesty
findings (the surface lobe trimming its azimuth over a different interval
than the sampler's own density; the "Eq. 15 reweighting degenerates to a
constant" claim being false for a two-family weave; two fitted "realised
fraction" constants retuning the reported albedo to agree with the dark BRDF
rather than fixing it; `make_fabric`'s pure-wrap path silently unreachable
for an already-correct `weave_material` base; and showcase `weave_scale`
values 4–8× coarser than the shipped presets, rendering as printed stripes
rather than cloth). All are fixed in this pass: `C_v` is now the exact,
reciprocity-symmetrised hemispherical integral; the fitted constants are
gone and `hemisphericalAlbedo` is an honest closed form; the azimuthal trim
mismatch is closed with a symmetrised boost; the Q claim is corrected (§10.1)
with the resulting grazing-only loss recorded as debt 7; `make_fabric`'s
weave-reuse path is fixed (`ScanFabricCandidates_`'s colour-refusal check was
unconditionally declining every weave base); and the two showcase scenes
were re-tuned and re-rendered against the corrected energy. A NEW guard,
`LayeredWhiteFurnaceTest`'s independent white-weave energy floor (row 49),
exists specifically because none of the pre-existing gates would have caught
the `C_v` defect — see §10.3.

**Gate status, 2026-09-03 (Phase 2 slice P2-A).** Gate **9b has been
RE-MEASURED** on the shipped `weave_material`, and its Phase-1 numbers reproduce
to every printed digit while the new pattern-scale proxy records structure where
Phase 1 had none (silk 0.035, satin 0.105 quadrature-subtracted structure rms,
against a Phase-1 shape whose entire high-frequency content is its noise floor).
Gate **11**'s scene count is now **six**: `weave_presets.RISEscene` joined the
regressions, and the two showcases moved to fabric-over-weave.

**Gate status, 2026-09-03 (Phase 1).** §9.9's gates carry measured numbers rather
than intentions: **gate 7** (HWSS invariant) is GREEN and asserted in
[tests/FabricRenderTest.cpp](../tests/FabricRenderTest.cpp), which also carries
a PT-vs-BDPT parity check reframed as a ratio-of-ratios so the documented +25 %
env-MIS BDPT bias cancels instead of swamping it; **gates 9, 9b and 10** are
RECORDING gates and are now recorded, measured through
[tools/fabric_sheen_measure.py](../tools/fabric_sheen_measure.py); **gate 11**
(scenes) is met by five scenes, two regressions and three showcases. Read each
gate's own MEASURED block below for the numbers and for what they say about the
Phase-2 entry conditions — in particular gate 9b, whose result §10.2's first
condition consumes directly.
**Date:** 2026-09-02.
**Nature:** decision document + phased plan, in the mold of
[UNIFIED_INTEGRATOR_DECISION.md](UNIFIED_INTEGRATOR_DECISION.md) (survey →
scored candidates against evidence → recommendation),
[HAIR_FUR_DESIGN.md](HAIR_FUR_DESIGN.md) (model survey → geometry survey →
phased plan with exit gates), and
[WETNESS_COAT_DESIGN.md](WETNESS_COAT_DESIGN.md) (verb-first adoption ordering,
observed-need-gated later phases).

**Inputs.** A five-report survey of the tree and the literature, conducted this
session. The RISE side: the sheen and layering stack
([CharlieSheen.h](../src/Library/Materials/CharlieSheen.h),
[SheenSPF.cpp](../src/Library/Materials/SheenSPF.cpp),
[SheenBRDF.cpp](../src/Library/Materials/SheenBRDF.cpp),
[CompositeMaterial.h](../src/Library/Materials/CompositeMaterial.h),
[CompositeSPF.cpp](../src/Library/Materials/CompositeSPF.cpp),
[CoatedMaterial.h](../src/Library/Materials/CoatedMaterial.h)); the tangent
frame and geometry derivatives
([Object.cpp](../src/Library/Objects/Object.cpp),
[RayIntersectionGeometric.h](../src/Library/Intersection/RayIntersectionGeometric.h),
[NormalMap.cpp](../src/Library/Modifiers/NormalMap.cpp),
[TriangleMeshGeometryIndexedSpecializations.h](../src/Library/Geometry/TriangleMeshGeometryIndexedSpecializations.h));
the painter stack and expression VM
([ExpressionEval.h](../src/Library/Painters/ExpressionEval.h),
[ExpressionPainter.h](../src/Library/Painters/ExpressionPainter.h),
[ChunkDescriptor.h](../src/Library/Parsers/ChunkDescriptor.h),
[ChunkParserRegistry.cpp](../src/Library/Parsers/ChunkParserRegistry.cpp));
the hair machinery available for reuse
([HairBSDF.cpp](../src/Library/Materials/HairBSDF.cpp),
[HairGeometry.h](../src/Library/Geometry/HairGeometry.h),
[HairMedullaProfile.h](../src/Library/Materials/HairMedullaProfile.h));
the verb, adoption and studio-rig surfaces
([AgentMcpAdapter.cpp](../src/Library/Agent/AgentMcpAdapter.cpp),
[AgentSession.cpp](../src/Library/Agent/AgentSession.cpp),
`docs/agentic-redesign/88-procedural-texture-expressiveness-candidates.md`);
the import status
([GLTFSceneImporter.cpp](../src/Library/Importers/GLTFSceneImporter.cpp),
[BLENDER_MATERIAL_TRANSLATION.md](BLENDER_MATERIAL_TRANSLATION.md)); and the
energy/consistency harnesses
([tests/LayeredWhiteFurnaceTest.cpp](../tests/LayeredWhiteFurnaceTest.cpp),
[tests/SPFBSDFConsistencyTest.cpp](../tests/SPFBSDFConsistencyTest.cpp)).
Contract conformance target: [MATERIALS.md](MATERIALS.md) §9. Spectral
conformance: [SPECTRAL_ILLUMINANT_CONVENTION.md](SPECTRAL_ILLUMINANT_CONVENTION.md)
§7.1, [ISCALARPAINTER_REFACTOR.md](ISCALARPAINTER_REFACTOR.md). External prior
art enumerated in §17, each carrying the verification tag its survey assigned
it.

**On citation hygiene.** Literature claims below carry **[verified]** (confirmed
this session against a primary or clearly authoritative secondary source) or
**[from memory]** (stated from training knowledge, not re-checked). The survey
corrected three author lists and one title that the original research brief had
wrong; the corrected forms are used throughout and the corrections are recorded
in §17. Where the reports disagreed with each other or with source, the source
wins and the disagreement is stated in the text rather than resolved silently.

**Amended 2026-09-02 (independent review).** A review round against the drafted
document found two Phase-1 mechanisms that do not survive contact with the
source, plus five narrower defects. Every finding was verified in the tree and
accepted; the recommendation itself — a `fabric_material` triad built as
`coated_material`'s sibling — is unchanged. What moved:

- **The sheen lobe is now strictly isotropic.** The proposed *elliptical
  Charlie* lobe is **withdrawn**: Charlie's `D` carries an isotropic-only
  normaliser, its Λ masking term is a polynomial fit with no azimuthal
  dependence, and a 2D `E(α, cosθ)` table cannot compensate a 4D anisotropic
  albedo. Weave anisotropy is instead delivered by the **substrate**, with
  `weave_rotation` rotating the basis `fabric_material` hands to the substrate's
  `value`/`Scatter`/`Pdf`. `weave_anisotropy` and the `weave` enum are removed
  from the Phase-1 chunk (§9.5; §9.3's preset table restructured; debt 5
  rewritten as "anisotropic sheen deferred").
- **Phase 1 keeps cosine-hemisphere sampling.** D-importance sampling of Charlie
  has no VNDF, leaks below the horizon at grazing, and its rejection-corrected
  density fails `SPFPdfConsistencyTest` Parts 2 and 3. Deferred behind a
  sampler/pdf pair proven against those tests first (§9.4, §9.2, §12).
- **The tangent write is conditional, prefers an authored `TANGENT`, and lands
  at eight sites, not seven.** It is gated on `useUVJacobian` (both mesh sites
  fall back to a per-triangle edge frame without it), prefers `ri.vTangent`
  where `bHasTangent` is set (otherwise the mirrored-seam remedy this document
  prescribes would have had no effect), and adds `clipped_plane_geometry`;
  `bilinear_patch_geometry` is examined and deliberately excluded (§9.1).
- **Presets cannot configure the substrate, so `make_fabric` mints one.** The
  `fabric` enum seeds only `fabric_material`'s own slots; `Finalize` warns on a
  substrate-class mismatch; the verb creates a matching base rather than
  performing `add_wetness`'s pure wrap (§9.3, §9.7).
- **The construction-time-quadrature route for `hemisphericalAlbedo` is
  discarded** — no hit context exists at construction, so it could only evaluate
  constant parameters (§9.2, debt 16).
- Smaller: an omitted `sheen_color` resolves to white on the `coat_tint`
  precedent rather than binding the built-in black painter (§9.3); the debt-2
  rename must include the registry descriptor, which feeds auto-generated
  schemas; debt 4 gains a Phase-1 checklist item to land GGX's Scalar-pipe
  rotation alias in the same slice.

A fourth round closed three remaining gaps in the amended text, after which the
design was locked:

- **A minted `ggx_material` substrate needs three fields the earlier text left
  out, or it renders with no highlight at all.** `fresnel_mode` defaults to
  `conductor` and `rs` (F0) is a required Color-painter reference, so a base
  carrying only `rd`/`alphax`/`alphay` gets a black `rs` under conductor
  Fresnel — no dielectric specular, i.e. no satin, silk or denim. And `rs`
  **cannot be written inline**: `rd`/`rs` resolve by painter name only
  ([Job.cpp:4243-4244](../src/Library/Job.cpp)), unlike `alphax`/`alphay`. So
  `make_fabric` mints a fourth chunk, `<name>_fabric_f0`, and sets
  `fresnel_mode schlick_f0` (§9.7 step 1; §9.3's † note; §14).
- **The substrate-mismatch warning belongs in `Job::AddFabricMaterial`, not in
  `Finalize`.** `IAsciiChunkParser::Finalize` sees only the substrate's *name*;
  the runtime class is knowable only after the material manager resolves it,
  which is exactly where `coated_material` runs its own check
  ([Job.cpp:3265](../src/Library/Job.cpp)). `Finalize` forwards the preset name
  and nothing more (§9.3).
- **The reflectance slot is named three different things**, so `make_fabric`
  looks up `reflectance`, then `base_color`, then `rd` in that order to find the
  painter to re-home, and refuses under Refusal 4 if none is present (§9.7
  step 0).

**Round 5 (implementation-time, 2026-09-02).** Phase 1 was built and the
build measured the locked formula against its own exit gates. One
measurement changed the model; three changed what the gates say. The
recommendation, the triad, the allowlist, the preset mechanism and the
weave-rotation mechanism are all unchanged.

- **The `min`-form base scaling is replaced by the Kulla-Conty product
  form, because the `min` provably cannot pass gate 3.** §9.2's
  `min(1 − m·E(v), 1 − m·E(l))` is reciprocal but **cannot** conserve
  energy: near normal incidence `E(v) → 0` while the `min` still picks
  `1 − m·E(l)` for every `l`, so the base loses `Ē`'s worth of energy the
  sheen lobe never returns. Measured on a white Lambertian base at
  m = 1: ρ = 0.863 at normal incidence for α = 0.5, against gate 3's
  required 1.000 ± 5 %. glTF's own **one-arm** form conserves energy
  exactly and is **not** reciprocal — the two properties are in direct
  tension and no combination of the two arms by `min` can hold both. The
  form that holds both is the product
  `(1 − m·E(v))·(1 − m·E(l)) / (1 − m·Ē)`, the closed form of the
  adding-doubling inter-reflection series between a lossless fuzz layer
  and the base: energy the fuzz intercepts is **re-scattered onto the
  substrate**, which is the physics the `min` was discarding. It is
  symmetric in `l`/`v` by inspection, and for a white Lambertian base at
  **any** m and **any** α gives ρ = 1 identically (§9.2, §9.4, §9.9
  gate 3).
- **The `S(α, m)` kernel table is retired.** It existed only because a
  `min` does not factor into (a function of `l`) × (a function of `v`),
  so `hemisphericalAlbedo`'s double integral had to be baked. A product
  factors, and route 1 collapses to the closed form
  `substrate.hemisphericalAlbedo() · (1 − m·Ē) + sheenColor · Ē` — with
  no interpolated table between it and the answer, and its
  "exact for Lambertian" claim now exact in closed form rather than up
  to a bake error. Routes 1b and 2 are retired with it: 1b was the
  remedy for a table that no longer exists, and 2's bound is no longer
  needed. `kSTable`, `SheenDirectionalAlbedo::S()` and the generator's
  `ComputeS`/`BakeS`/`ClampedOneMinusME` are removed; `E` and `EMean`
  are unchanged, bit-for-bit (§9.2, §9.4, §14).
- **Gate 4's re-diagnosis came back positive, and it re-writes §4.2.**
  The downward-ray probe measured `SheenSPF` emitting a downward ray in
  **0 %** of draws at both θ = 0 and θ = 80 — identical to the GGX top
  layer that furnace config 7 already indicts. `SheenSPF` is
  reflection-only by construction, so `composite_material`'s random walk
  **never reaches the substrate**, and config 6's ρ curve
  `{0.0875, 0.1293, 0.2812, 0.5461}` reproduces bare sheen's
  `{0.0875, 0.1283, 0.2810, 0.5453}` to MC noise. Config 6's old note
  ("the composite walk amplifies sheen's intrinsic dissipation") was
  wrong: configs 6 and 7 are **one** defect, not two (§4.2, debt 3).
- **Gate 5b's residual is the substrate's, not fabric's, and it is
  recorded as a new debt.** With the exact factorisation, fabric's own
  uncorrelated-response error is ≤ 0.64 % (0 by construction for
  Lambertian). The end-to-end error against a brute-force quadrature
  reaches 15.7 %, and **all** of it is the substrate's own
  `hemisphericalAlbedo`: `OrenNayarBRDF::hemisphericalAlbedo` returns
  `Rd` verbatim, documented at
  [OrenNayarBRDF.cpp:148-190](../src/Library/Materials/OrenNayarBRDF.cpp)
  as up to 25.6 % high, and GGX's estimate runs high at grazing. No
  change to `fabric_material` can fix that, and `coated_material`'s
  recycling denominator already inherits the identical debt (§9.9
  gate 5b, debt 16, new debt 17).

**Round 6 (implementation review, 2026-09-02).** Three adversarial
reviews of the built slice found one P1, in the *table* rather than in
the model. Round 5's Kulla-Conty form survives unchanged; what changed is
the axis it is evaluated on, and a bound it turned out to need.

- **The `E` table's cosθ axis could not resolve the lobe near grazing,
  and the energy identity broke by up to +1.05 ABSOLUTE.** The axis was
  32 uniform nodes on [0, 1], so the first interior node sat at
  μ = 0.0323 with an exact 0 at μ = 0 — the interpolant ramped linearly
  from zero across the entire band the Charlie lobe occupies, when the
  lobe is already near its peak by μ ≈ 0.005. Table 0.146 against a true
  1.19 at α = 0.04, μ = 0.005. Because `value()` **emits** the true lobe
  while suppressing the base by the **tabled** `E`,
  ρ(v) ≈ 1 + (E_true − E_tab): a white Lambertian fabric returned up to
  **2.05** under grazing illumination, at every roughness. Both arms
  were affected, so it was a rim-light bug over whole surfaces, not just
  a silhouette artefact. **Fixed by warping the axis** —
  μ_j = (j/(N−1))², ~6 of the 32 nodes below μ = 0.03 — with the runtime
  inverting it by `sqrt`. Re-measured: ρ within 0.06 % of 1 for
  μ ≥ 0.0349 and within +0.30 % at μ = 0.005. *(Round 8: both figures
  were measured at α = 0.04 and are not the worst case — see §9.2's
  exactness table for the α-swept numbers.)*
- **The gates could not see it, and now can.** `THETA_DEG` stops at 80°
  (μ = 0.1736, five times the old first node), and the non-Lambertian
  rows' prediction used the same `E` on both sides of the comparison.
  §9.9 gate 3 gains a **grazing check** at θ ∈ {80, 85, 88, 89}, and the
  non-Lambertian prediction is now re-derived in the test from
  `SheenDirectionalAlbedo` alone instead of calling `FabricBRDF`'s own
  helpers.
- **A symmetric normaliser now bounds the sliver the floor cannot.**
  With the band resolved, `E` exceeds 1 (reaching 1.152) even above the
  roughness floor, inside μ < 0.03. The sheen lobe is divided by
  `max(1, E(v), E(l))` and the base scaled by `Ê = min(E, 1)`; both are
  symmetric in l/v, so reciprocity is untouched, and outside the sliver
  both collapse to exactly 1 / exactly `E` — **bit-identical** to round
  5 there. **The exactness class is now explicit: energy-CONSERVING for
  n·v ≥ 0.03, energy-BOUNDED below it** (debt 18).
- **The roughness floor's criterion is stated and re-scanned.** It is
  *"the smallest α with max over μ ≥ 0.03 of E(α, μ) ≤ 1"* — not "over
  all μ", which no α satisfies once the band is resolved. The generator
  prints the scan on every bake; on the warped table the smallest
  qualifying α is 0.028289, so **0.04 stands unchanged**, with both
  bilinear-bracketing rows already under 1.
- **`EMean` became `EHatMean`**: the mean of `Ê`, not of raw `E`, since
  `Ê` is what the code multiplies by. Smaller items: `WeaveRotatedRI` is
  non-copyable (its implicit copy left `pRef` dangling into the source);
  the parameters and the rotated record are resolved **once** per
  `Scatter` / `Pdf` rather than 3–4×; and `alpha` and `weave_rotation`
  are read achromatically on the spectral pipe, for the same reason `m`
  already was.

**Round 7 (implementation review, 2026-09-03).** Two further adversarial
reviews. One P1 in the table again, one P1 in this document's own
description of a gate.

- **The warped axis' FIRST CELL still ramped E from zero, and the
  normaliser did not bound it.** Round 6 fixed the grazing band by
  warping the cosθ axis, but node 0 still holds an analytically exact
  `E = 0` at μ = 0 and node 1 sits at μ = 1/961 — so inside that one
  cell the interpolant ramped up from zero across a region where the
  true lobe is already at its **peak** (E_true = 0.53 at μ = 5e-6,
  1.15 by node 1; the interpolant read 0.08). The same defect, one cell
  narrower. **And §9.2's normaliser argument does not cover it**: `N =
  max(1, E(v), E(l))` is built from the *tabled* E while `value()`
  emits the *true* lobe, so where `E_tab < 1 < E_true` the normaliser
  collapses to 1, the lobe is emitted undivided **and** the base is
  barely suppressed. Measured white-Lambertian ρ reached **1.714**. The
  header stated the bound as a proof; it was not one.
  **Fixed** by flooring `SheenDirectionalAlbedo::E`'s cosθ argument at
  node 1 — constant extrapolation below, now the table's documented
  domain. Re-measured, ρ falls from 1.714 to **≤ 1.0067** across the
  cell, decaying to 0.18 at μ = 1e-6; at and above μ₁ the floor is a
  bit-identical no-op. *(Round 8 correction: this entry originally said
  "ρ ≤ 1 across the cell", justified by E_true's monotonicity in μ. That
  argument is incomplete — `E_tab(μ₁)` is a log-α interpolation across a
  cell where E is concave in α, and under-reads the true lobe by up to
  0.0067 near α = 0.9, which is where the residual +0.67 % comes from.)* `EHatMean` integrates the same floored function
  (an analytic term for [0, μ₁] instead of trapezoiding the zero node).
- **§9.9 gate 3 described a check that was never implemented.** This
  document claimed the Lambertian rows were cross-checked per angle
  against a closed form at "≤ 0.002"; the shipped test asserted only the
  5 % posture band, and the non-Lambertian eps was 0.01, not the 0.02
  the same passage stated. The closed-form check is **now implemented**
  (deterministic quadrature, independent of `FabricBRDF`) at a derived
  tolerance of 0.012, measuring 0.0024 — and gate 3's text now states
  exactly what is asserted. Both misquoted numbers are corrected here
  and in the test's own comment.
- **Exactness class refined, and the residual restated honestly.** The
  header quoted "+0.30 % at n·v = 0.005", which is the *best* case in
  that band. Three regimes now, not two. *(Round 8 re-measured all three
  over the full α range and every figure moved: +0.64 % / +1.67 % /
  +0.67 %, all worst at α ≈ 0.9 rather than at the roughness floor
  where rounds 6–7 sampled. See §9.2's table.)*
- **Gate coverage extended to the band that hid both P1s**: θ = 89.9°
  and 89.99° added to the grazing check, and off-node brute-force probes
  plus floored-domain assertions added to `SheenDirectionalAlbedoTest`
  (its slope-relative continuity check is blind to a *uniform* stencil
  shift, and near-zero-margin at the flat high-μ end).
- Smaller: `hemisphericalAlbedo{,NM}` clamped to [0,1] (an R > 1 drives
  `coated_material`'s recycling denominator `1/(1 − r_i·R)` toward zero);
  a NaN `sheen_roughness` is now gated rather than propagated; the
  sheen-branch selection weight's under-selection near grazing is
  documented as variance-not-bias; the substrate's non-horizon drop
  paths are documented as a GGX defect fabric inherits in proportion;
  and a Cook-Torrance spectral defect was found and (in a follow-up pass)
  fixed, recorded as debt 19.

**Round 8 (external review of the shipped range f2ef553a..73d0c983,
2026-09-04).** One P1 and five P2s from an independent reviewer of the
whole arc; three fresh adversarial reviewers on the fix returned one P1 of
their own, fixed the same day.

- **P1.1 — `fabric_material` over a `transmission thin` weave was 100 %
  opaque, silently.** The allowlist admitted `weave_material` (§7.3) but
  the wrapper never forwarded `ScattersFullSphere` /
  `CouldLightPassThrough`, `FabricBRDF` rejected every opposite-hemisphere
  pair and `FabricSPF` zeroed every below-horizon draw, delta gap ray
  included. This document's own header text said Phase 2 "flips the
  latter two"; it never did. **Fixed** by forward-and-modulate — the
  transmitted lobe carries the same Kulla-Conty product law as the
  reflect side, the delta gap lobe carries the two sheen arms without the
  recycling denominator (a measure-zero direction receives none of the
  diffusely redistributed series; charging it measured 1.13× brightening
  of an aperture), and a non-transmissive substrate is bit-identical (a
  45-row value lock). §15 debt 22 has the derivation, the furnace table
  (wrapped totals 0.871 → 0.810 over 0°–80°, T within 0.001–0.01 of the
  closed form) and the backlit render parity (BDPT/PT 0.913, VCM/PT
  0.946, inside the existing 0.20 band; runs re-seed from the wall
  clock, so the third digit wanders by ±0.001).
- **P2.4 / P2.2 / P2.1 / P2.3** — `add_fuzz`'s refusal on CSG,
  instanced and container hosts now names the host shape and a concrete
  route; `make_fabric`'s weave mint discloses that only `warp_color` took
  the author's colour and prints the preset's real weft dye (the fresh
  reviewers' one P1: a first cut said "undyed (white)" for every preset,
  which is true of denim and false of silk's champagne and satin's rose —
  the message now reads the triple from `WeavePresets.h` and the test pins
  satin's); clearcoat-over-fabric is a tracked IMPROVEMENTS.md backlog
  entry; the 0.04 sheen-roughness floor has a migration note in
  GLTF_IMPORT.md and MATERIALS.md. **P2.5** (the 5–10 % BDPT/VCM-under-PT
  residual on delta-lit thin weaves) was already tracked and stays open.

**Round 9 (implementation, 2026-09-04, reviewer P2.3).** Closes part of
debt 18's α-band over-unity by re-baking the sheen `E` / `EHatMean`
tables denser: `kNumAlphaBins` and `kNumCosThetaBins` both 32 → 64, a
uniform refinement of the existing log-α and grazing-warped-μ schemes,
not a new node or a new axis law.

- **Two of three exactness bands cleared an informal +0.2 % target;
  the third did not, and the reason is a relocated driver, not a
  resolution shortfall.** The outer band (n·v ≥ 0.0349) and inner band
  (n·v < μ₁) both fell roughly in half — +0.64 % → +0.19 % and
  +0.67 % → +0.18 % — because both are driven by the log-α chord's
  under-read of the concave E-at-node-1 peak near α ≈ 0.9, and doubling
  the α axis halves that cell's width. The **middle band**
  (μ₁ ≤ n·v < 0.0349) fell from +1.67 % to +0.75 % — a real 55 %
  reduction, but well short of the other two — because an exhaustive
  (α, μ) search (not the per-band spot checks rounds 6–8 used) found
  its new worst at **α ≈ 0.065, near the roughness floor**, not at
  α ≈ 0.9. Shrinking the α ≈ 0.9 mechanism below the floor-region one
  exposed a second, previously-smaller residual as the new bottleneck.
  Per this round's own stop rule, that residual is reported rather than
  chased with a still-finer table.
- **`kMinSheenAlpha`'s scan moved, but the floor still clears it.** The
  smallest baked α satisfying "max over μ ≥ 0.03 of E ≤ 1" is now
  0.037276 (was 0.028289 at 32 nodes) — `FabricBRDF::kMinSheenAlpha`'s
  fixed 0.04 still clears it, with less margin than before, and both
  of 0.04's bilinear-bracketing rows are still under 1.
- **Cost, measured, not estimated.** Data file payload 16.25 KB (was
  4.125 KB, ~4× — the expected cost of a uniform 2×/2× refinement).
  Bake time ~108 s (was ~24 s, ~4.5×) on the same hardware and
  quadrature settings; a larger fraction of the finer grid's cells
  needed the adaptive quadrature's higher rungs to converge, so the
  wall-clock cost slightly outpaced the ~4× cell-count growth.
- **Every consumer this table has was re-derived, not just re-run.**
  `tests/SheenDirectionalAlbedoTest.cpp`'s node-index spot checks and
  domination-slack constant were re-derived against the new bake (and
  two of its `mu` probe lists were changed from hard-coded absolute
  values to `mu1`-relative ones — the old absolute `5e-4` silently
  stopped being "below μ₁" once μ₁ shrank past it, which is exactly the
  kind of table-resolution-coupled literal this round's own header
  warning exists to prevent). `tests/LayeredWhiteFurnaceTest.cpp`'s
  grazing-check tolerance tightened 0.03 → 0.015 against the newly
  measured (and still deterministic, re-run-stable) worst case.
  `tests/FabricRenderTest.cpp` (42 checks) is unaffected — its
  integrator-ratio bands do not depend on the table's absolute values.
  `tests/FabricMaterialChunkTest.cpp`'s two 45-row value locks DO (they
  pin E-dependent reflect-side values) and were re-captured from the
  post-bake binary, their comment rewritten to say what they now lock,
  and the re-capture proven live by a 2e-8 mutation of one row that
  fails the suite; the reflect-side code is unchanged since c81fedf4,
  which is why re-capturing preserves the lock's purpose. `FabricBRDF.h`'s
  own copy of the exactness-class comment (lines ~100–151),
  `SheenDirectionalAlbedo.h`'s narrative and MATERIALS.md were rewritten
  to the round-9 figures in the same commit (the port worker's note that
  the header "could not be updated" was drafting text from the worktree
  stage, where it was out of scope; it was in scope for the port).

---

## 1. The question, and the answer

A cushion, a curtain, a shirt, a sofa arm — these are the objects that make an
interior render read as a photograph or as a product visualisation, and in RISE
today they all read as rubber or as plastic. The reason is not that RISE lacks a
sheen lobe. It has one, it is the right lobe, and it is correctly implemented.
The reason is that there is **no way to put that lobe over a base material
without losing the base**, no way to make a highlight follow a weave, and no way
for an author — human or agent — to say the word "velvet" and get velvet.

**The answer: RISE has the physics and none of the assembly. The assembly is one
new material class of a shape RISE has already built once, plus a three-line
geometry write repeated at eight call sites (~30 lines) that unlocks every
anisotropic material in the tree, plus a preset
surface that does not exist anywhere in RISE yet and therefore has to be
invented rather than copied.**

Five gaps, each verified in source:

1. **The documented sheen-over-base idiom does not layer.** Three separate
   places tell an author to write `composite_material(top = sheen_material,
   bottom = <base>)`: the BRDF header
   ([SheenBRDF.h:10-12](../src/Library/Materials/SheenBRDF.h)), the chunk
   descriptor ("Designed as the top layer in a CompositeMaterial(top=sheen,
   bottom=base) pairing",
   [ChunkParserRegistry.cpp:4359](../src/Library/Parsers/ChunkParserRegistry.cpp)),
   and [MATERIALS.md:262-263](MATERIALS.md). It does not work, for two
   independent structural reasons. `CompositeMaterial::GetBSDF()` takes the
   **top's BSDF whole** and never combines it with the bottom's
   ([CompositeMaterial.h:56-63](../src/Library/Materials/CompositeMaterial.h)),
   so NEE and every BDPT/VCM connection through the composite sees only the
   sheen lobe. And `SheenSPF::Scatter` samples
   `GeometricUtilities::CreateDiffuseVector` around the **outward** normal
   ([SheenSPF.cpp:73-74](../src/Library/Materials/SheenSPF.cpp)) and emits
   exactly one upward ray — so `CompositeSPF`'s walk, which hands the bottom
   layer a ray only when the top emits a **downward** one
   ([CompositeSPF.cpp:365-373](../src/Library/Materials/CompositeSPF.cpp)),
   never reaches the substrate at all. This is the failure mode the furnace
   suite already diagnosed and named for the structurally identical
   GGX-over-PBR case: *"GGXSPF only ever emits UPWARD lobes … so a GGX top
   layer never hands CompositeSPF's walk a downward ray and the SUBSTRATE IS
   NEVER REACHED"*
   ([LayeredWhiteFurnaceTest.cpp:756-761](../tests/LayeredWhiteFurnaceTest.cpp),
   config 7, `kPostureKnownFailure`). **There is no working energy-correct
   sheen-over-base recipe in RISE today.** This is the load-bearing gap.
2. **`coated_material` cannot host sheen either.** Its substrate allowlist is a
   hardcoded three-way `dynamic_cast` over `LambertianMaterial |
   OrenNayarMaterial | GGXMaterial`
   ([CoatedMaterial.h:130-132](../src/Library/Materials/CoatedMaterial.h)), and
   its coat is a dielectric film with its own IOR and a Fresnel-based energy
   split — not a Charlie lobe with an albedo-based one. The glTF importer says
   so in its own words while skipping `KHR_materials_sheen`: *"a
   retro-reflective grazing lobe, not a dielectric coat, so `coated_material`
   genuinely cannot express it"*
   ([GLTFSceneImporter.cpp:1449-1456](../src/Library/Importers/GLTFSceneImporter.cpp)).
3. **A weave's warp direction does not get a UV-aligned tangent for free.** The
   shading ONB's tangent on an ordinary mesh is whatever
   `OrthonormalBasis3D::CreateFromW` picks from a canonical axis — the code says
   so: *"the tangent (u-axis) is whatever CreateFromW picks from a canonical
   axis — fine for isotropic materials, but an arbitrary base for anisotropic
   GGX"* ([Object.cpp:688-690](../src/Library/Objects/Object.cpp), with the
   `CreateFromW` fallback at [Object.cpp:772](../src/Library/Objects/Object.cpp)).
   Triangle meshes **do** compute correct UV-derived `dpdu`/`dpdv` at
   intersection
   ([TriangleMeshGeometryIndexedSpecializations.h:458-462](../src/Library/Geometry/TriangleMeshGeometryIndexedSpecializations.h)),
   and `NormalMap::Modify` consumes them to build a local TBN
   ([NormalMap.cpp:109-120](../src/Library/Modifiers/NormalMap.cpp)) — and then
   throws that alignment away when it rebuilds the ONB, unless the hit carries
   a geometry-supplied shading tangent
   ([NormalMap.cpp:186-197](../src/Library/Modifiers/NormalMap.cpp)). GGX's
   `tangent_rotation` painter therefore rotates from an arbitrary, per-triangle,
   discontinuous base
   ([ChunkParserRegistry.cpp:4193](../src/Library/Parsers/ChunkParserRegistry.cpp)).
   The general per-hit override that would fix this
   (`bShadingTangentFromGeometry` + `vShadingTangent` + `bHasShadingTangent`,
   [RayIntersectionGeometric.h:301-345](../src/Library/Intersection/RayIntersectionGeometric.h))
   **already exists and is already consumed**
   ([Object.cpp:699-772](../src/Library/Objects/Object.cpp),
   `CSGObject.cpp:1234-1290`) — it simply has no mesh writer. Today only
   `HairGeometry` (a real fibre tangent) and `SDFGeometry` heightfield mode (a
   coherent world-X tangent) write it.
4. **`sheen_material` is Charlie, is correct, and is unfinished.** The audit
   result: `CharlieSheen::D` is the Estevez & Kulla exponentiated-sine NDF
   ([CharlieSheen.h:39-50](../src/Library/Materials/CharlieSheen.h)) and
   `CharlieSheen::V` is the full Λ-polynomial Charlie visibility, not the cheap
   Neubelt closed form
   ([CharlieSheen.h:71-112](../src/Library/Materials/CharlieSheen.h)). **The
   "Charlie / Neubelt" naming in both the header comment
   ([SheenBRDF.h:9,18](../src/Library/Materials/SheenBRDF.h)) and the chunk
   description
   ([ChunkParserRegistry.cpp:4358](../src/Library/Parsers/ChunkParserRegistry.cpp))
   is stale** — the Neubelt form was replaced precisely because it blew up to
   ρ ≈ 8.7 at grazing
   ([LayeredWhiteFurnaceTest.cpp:661-670](../tests/LayeredWhiteFurnaceTest.cpp)).
   What is missing: no directional-albedo compensation table (the furnace marks
   sheen `kPostureBounded`, "ρ may legitimately fall below 1 … but must NOT
   exceed 1", [LayeredWhiteFurnaceTest.cpp:667-669](../tests/LayeredWhiteFurnaceTest.cpp));
   no D-importance sampling (cosine-hemisphere only,
   [SheenSPF.cpp:73-74,116](../src/Library/Materials/SheenSPF.cpp)); no
   reciprocity or SPF↔BSDF consistency coverage at all — the reciprocity sweep
   lists only Lambertian, isotropic GGX and three Coated configurations
   ([SPFBSDFConsistencyTest.cpp:1140-1146](../tests/SPFBSDFConsistencyTest.cpp)).
5. **There is no fabric in the tree and no way to ask for one.** The only scene
   using `sheen_material` is a three-sphere synthetic demo
   (`scenes/Tests/Materials/sheen.RISEscene`); there is no cushion, curtain or
   garment. glTF `KHR_materials_sheen` is warn-and-skip
   ([GLTFSceneImporter.cpp:1456-1463](../src/Library/Importers/GLTFSceneImporter.cpp));
   `KHR_materials_anisotropy` imports strength but drops per-texel rotation
   ([GLTFSceneImporter.cpp:1300-1307](../src/Library/Importers/GLTFSceneImporter.cpp));
   and the Blender bridge documents no sheen, anisotropic or velvet mapping at
   all. **No material preset exists anywhere in RISE, and no `ValueKind::Enum`
   anywhere seeds a multi-slot bundle** — every enum in the descriptor set is an
   algorithm or mode selector (`oidn_quality`, `wrap_s`, Worley `metric`,
   `blend_painter` `mode`). The closest prior art is
   `ParameterDescriptor::presets`, a per-parameter list of named values for the
   scene editor's quick-pick combo box
   ([ChunkDescriptor.h:449](../src/Library/Parsers/ChunkDescriptor.h)), used on
   `scene_options.scene_unit` ("Centimetres" → `0.01`) and on the two
   `sensor_size` slots ("Full-frame 35mm" → `36`)
   ([ChunkParserRegistry.cpp:4552-4563, 4612-4623, 4986-5003](../src/Library/Parsers/ChunkParserRegistry.cpp)).
   That is a **single-scalar editor affordance on one parameter**, not a name
   that seeds several slots at once — which is the genuinely new part here.

**Decision (recommended): a three-phase plan — foundations plus one material
now, structured weave second and gated, yarn geometry third and probably
declined.**

- **Phase 1 — `fabric_material`, the UV-aligned tangent, and `make_fabric`.**
  One new material triad in `coated_material`'s exact architectural shape (a
  base-material reference with an allowlist, a closed-form `IBSDF::value` that
  sums both lobes so NEE and BDPT see them, a real mixture `Pdf`), whose top
  lobe is Charlie sheen with a **baked directional-albedo table** supplying both
  the lobe's own energy compensation and the glTF-standard albedo-scaling of the
  base. Plus the three-line geometry-side write of `vShadingTangent = dpdu`,
  repeated at eight call sites (§9.1), which fixes anisotropy alignment for GGX,
  Ward and Ashikhmin-Shirley at the same time. Plus a `weave_rotation` angle
  field through `IScalarPainter` that rotates the frame handed to the
  **substrate**, so an anisotropic base follows the weave — the sheen lobe
  itself stays strictly isotropic (§9.5). Plus a **`fabric` enum** on the
  chunk seeding named presets (cotton, denim, silk, satin, velvet, wool, linen)
  and a **zero-required-argument `make_fabric` verb** that converts a flat
  material into one.
- **Phase 2 — structured weave, gated on Phase-1 evidence.** A weave-aware
  two-yarn-family BSDF in the Zhu 2023 / Sadeghi 2013 surface lineage, with
  transmission. Gated on a *named* Phase-1 deficit (satin floats and twill lines
  that Phase 1 provably cannot produce) plus a census showing anyone asks.
- **Phase 3 — yarn-level geometry for knits and fuzz halos. Observed-need gated
  and likely to be declined**, on the [HAIR_FUR_DESIGN.md](HAIR_FUR_DESIGN.md)
  Phase-4 precedent. §11 carries the memory arithmetic that makes the case.

**The order is not arbitrary.** Three separate causalities force it.

*Adoption.* The measured laws
(`docs/agentic-redesign/88-procedural-texture-expressiveness-candidates.md` §2)
say advice converts approximately zero times — **C-ADV**: design notes naming
`scalar_painter` fired up to 30×/session, were demonstrably read, and drove
**0/24 lifetime adoptions**; voluntary tools **0/64**. **C-VERB**: when advice
fails, ship a verb. Shipping a structured weave BSDF first and documenting how
to reach it inverts the only causality RISE has actually measured. The verb and
the preset enum have to land with the material, not after it.

*Correctness.* The tangent fix is not cosmetic and not deferrable past the
anisotropy work: a rotation field applied to a discontinuous per-triangle base
produces a BSDF that is discontinuous across every triangle edge, which BDPT
evaluates at both endpoints of every connection. Building weave anisotropy on
today's base would be building on sand.

*Evidence.* Phase 2 is the expensive phase and the literature is not settled
enough to commit to it blind — the survey could confirm Zhu 2023's headline
claims and its comparison against Irawan-Marschner, but **could not obtain its
complete equation set or itemised parameter table** [R5's own flag]. Phase 2
starts with a primary-source read, and that read is its own first gate.

---

## 2. The physics, distilled

Cloth is not a surface. It is a woven or looped assembly of yarns, each a
twisted bundle of plies, each ply a bundle of dyed filaments a few tens of
microns across — a structure whose optical behaviour is genuinely volumetric,
as the micro-CT literature established directly (Zhao et al. 2011 [verified];
§3.7). Every model in §3 is an approximation to that, and the useful question is
which *cues* each approximation delivers.

Four cues make cloth read as cloth.

**(a) Grazing brightening.** A fibre is a cylinder; light striking it near
grazing forward-scatters strongly, and the aggregate over a field of near-normal
fibres is a rim of brightness at the silhouette that no Lambertian or GGX
surface produces. This is the single most diagnostic cue and it is the one
`sheen_material` already delivers — the demo scene's own comment says the sphere
"should look mostly dark in the centre with a bright halo around the silhouette"
(`scenes/Tests/Materials/sheen.RISEscene:6-8`). Supplied by: any grazing-peaked
microfacet distribution (Ashikhmin-Premoze-Shirley 2000, Charlie 2017) or any
forward-peaked volumetric phase function (SGGX, SpongeCake).

**(b) Weave-structured anisotropic highlights.** Satin's diagonal S-shaped
sheen; denim's twill lines; taffeta's crossed warp/weft glint. These exist
because the highlight geometry follows *which yarn is uppermost at each point*
and *which way that yarn runs*. This cue needs three inputs no isotropic model
has: a yarn direction, a weave-cell phase, and different behaviour for the two
yarn families. Supplied by: Irawan-Marschner 2012 (an explicit pattern raster),
Sadeghi 2013 (two statistical thread families, no raster), Montazeri 2020
(plies), Zhu 2023 and Jin 2022 (surface-BSDF-cost anisotropy). **The entire
sheen family — Charlie, LTC sheen, Ashikhmin velvet — is isotropic and
structurally cannot produce it.**

**(c) Soft terminator from multiple scattering.** Real cloth's light-to-shadow
transition is wide and slightly desaturating, because light that enters a fibre
bundle bounces many times before leaving. Base Charlie is single-scattering and
does not model this; its albedo-scaling table is an energy patch, not a
scattering term [verified via the Khronos glTF sheen spec text and Estevez &
Kulla's own framing]. Explicitly targeted by Zeltner-Burley-Chiang 2022 (whose
stated motivation is exactly that significant backward scattering was still
missing after the albedo patch) and by SpongeCake's fitted multi-scatter lobe.
Approximated non-physically by Filament's fixed `w = 0.5` wrap-diffuse
[verified — Filament's own material docs].

**(d) Silhouette fuzz.** The soft halo of stray fibres standing off a napped
surface. **This is not a BSDF problem at all.** Every surface model in §3 —
Irawan, Sadeghi, Charlie, LTC, Zhu — renders a hard geometric silhouette
underneath its shading. Production closes this with geometry (fuzz cards, fin
and shell passes) or with a volumetric shell. RISE's honest posture is to say so
and route it to Phase 3, not to pretend a BSDF term supplies it.

**No single model supplies all four.** The volumetric family gets (a), (c) and
(d) but not (b) without an added orientation-map workflow; the Zhu/Jin
surface-anisotropic lineage gets (a), (b) and partially (c) but not (d). That is
the central tension this design resolves by phasing: Phase 1 buys (a) properly
and (c) partially; Phase 2 buys (b); Phase 3 is the only route to (d) and is
gated.

**Spectral: dyed fibre vs RGB albedo.** RISE is a hero-wavelength spectral
renderer with a per-wavelength NM twin on every material, so this deserves a
straight answer. The literature's default posture is to fit RGB photographs and
ship an RGB or loosely-spectral tint — *not* to derive from dye chemistry. Three
tiers exist:

| tier | what it is | who has it | RISE fit |
|---|---|---|---|
| **1. Per-wavelength absorption over a path length** | a real σ_a(λ) integrated by Beer-Lambert through the fibre | volumetric family only (SGGX-as-medium, SpongeCake, Zhao's CT volumes) — architecturally, though none ships dye spectra by default | This is what RISE's own hair BSDF already does (`HairBSDF.h`'s three colour tiers, incl. artist colour inverted through Chiang's σ_a fit at [HairBSDF.cpp:731](../src/Library/Materials/HairBSDF.cpp)) |
| **2. Native per-λ reflectance spectra, no transport integral** | `kd`/`ks` authored as spectra | Irawan (natively spectral per yarn type), Sadeghi's per-thread tints | Reachable today: `spectral_painter` on a colour slot, read via `GetColorNM` |
| **3. RGB tint on a physically agnostic lobe** | `sheenColor` multiplied onto the lobe | Charlie, LTC sheen, Ashikhmin velvet | What `sheen_material` does today |

**Recommendation: Phase 1 stays at tier 2 and says so.** A `fabric_material`
whose dye slot is an ordinary colour painter routed through `GuardedGetColorNM`
gets tier-2 spectral fidelity for free and is exactly conformant with
[SPECTRAL_ILLUMINANT_CONVENTION.md](SPECTRAL_ILLUMINANT_CONVENTION.md) §7.1's
two-line rule — reflectance goes through `GetColorNM`, never `GetRadianceNM`;
the white guard keeps an authored-white dye a bit-exact no-op on the NM path.
Tier 1 requires a fibre-radius/path-length concept the sheen family does not
have; it is a genuine RISE-specific opportunity (RISE is one of very few
renderers with both a spectral hero path *and* a shipped σ_a fibre model to
borrow from) but it is Phase-2-or-later work and depends on the yarn model, not
the sheen lobe. §16 lists it as a non-goal for Phase 1 rather than a debt.

---

## 3. Cloth appearance models — survey and scoring

### 3.1 Irawan & Marschner 2012 — *Specular Reflection from Woven Cloth*

[verified: Piti Irawan and Steve Marschner, *ACM TOG* 31(1), Article 11, Feb
2012, DOI 10.1145/2077341.2077352.]

A procedural closed-form BRDF over a 2D tiling of a **weave pattern matrix**: at
each cell the pattern says which yarn family is on top, and the yarn segment is
shaded as a bent, twisted cylinder of parallel filaments — a fibre-cross-section
specular term plus a diffuse stand-in for internal scattering. Neighbouring
points share a segment, which is what produces cloth's *correlated* glints. This
is the model that invented cue (b) in renderable form.

**Parameters**, counted from the Mitsuba 0.5/0.6 `irawan` plugin, the de facto
reference implementation [verified via source]: **~8 pattern-level fields + ~10
per-yarn-type fields + the pattern raster**. Pattern: `alpha`, `beta`, `ss`,
`hWidth`, `warpArea`, `weftArea`, `tileWidth`, `tileHeight`, four
`d*UmaxOverD*` noise derivatives (whose whole job is to break the periodic
look), `fineness`, `period`, `pattern`, `yarns`. Per yarn type: `type`, `psi`
(fibre twist), `umax` (normal inclination across the yarn's width), `kappa`
(spine curvature), `width`, `length`, `centerU`, `centerV`, `kd`, `ks` — the
last two full spectra, which makes the model natively spectral. Irawan fit these
to photographs for a library of canonical presets; essentially every production
use is "pick a preset, tweak `kd`/`ks`."

**Fatal for RISE as an implementation target: no published importance sampler.**
Mitsuba's plugin samples it as a cosine-weighted diffuse lobe with the specular
contribution evaluated on top, leaning on NEE [from memory, consistent with the
plugin's documented behaviour]. Survivable in a NEE-dominant unidirectional
renderer; not survivable in RISE, where every lobe needs a real `Pdf` for BDPT
connections and VCM merges ([MATERIALS.md](MATERIALS.md) §9 item 2, enforced by
[SPFPdfConsistencyTest.cpp](../tests/SPFPdfConsistencyTest.cpp)). Energy
conservation is design intent, not furnace-exact [from memory — the paper's own
claims]. No adoption in any mainstream commercial shading system.

**Verdict: the fidelity baseline the whole field compares against, and Phase 2's
validation target. Not the implementation target.**

### 3.2 Sadeghi, Bisker, De Deken & Jensen 2013 — microcylinder

[verified: *ACM TOG* 32(2), Article 14, 2013, DOI 10.1145/2451236.2451240.
**The survey corrected an author list the research brief had wrong** —
"Bisceglio, Joshi, Bloom" appear on no such paper.]

Each *thread* is a microcylinder whose cross-sectional normal distribution is fit
to measured BRDF data. The response factors hair-BSDF-style into a
**longitudinal** term (highlight width along the thread axis, a Gaussian in the
difference of longitudinal angles — Marschner's M) and an **azimuthal** term
(highlight shape around the circumference, and the core-vs-glancing colour shift
that gives shot silk its travel). Two thread families blend per shading point
with a shadowing/masking term darkening the valleys between them.

**~5 physically-motivated scalars per thread appearance class** — `eta`, `kd`,
`gamma_s` (surface-lobe variance), `gamma_v` (volume-lobe variance, the
multiple-scattering stand-in), and a longitudinal shift analogous to hair's
cuticle tilt — plus two tangent directions and their weave fractions. Most
fabrics need one or two classes.

**Architecturally the closest model in the survey to machinery RISE already
has.** Its longitudinal/azimuthal factorisation is the one `HairBSDF.cpp`
implements, and the pieces are already stateless free functions in an anonymous
namespace: `Mp` at [HairBSDF.cpp:174](../src/Library/Materials/HairBSDF.cpp),
`TrimmedLogistic` at [:208](../src/Library/Materials/HairBSDF.cpp),
`FrDielectric` at [:239](../src/Library/Materials/HairBSDF.cpp), `MakeGeom` at
[:268](../src/Library/Materials/HairBSDF.cpp), `ComputeAp` at
[:472](../src/Library/Materials/HairBSDF.cpp) — none touching
`HairScatteringBase`'s painter state, so a header promotion is mechanical. Its
two-tangent requirement is exactly what §1 gap 3's fix supplies.

Weaknesses: no importance sampler in the base paper (a SIGGRAPH Asia 2013 talk,
*Importance Sampling for a Microcylinder Based Cloth BSDF*, exists to fill the
gap [verified as existing, not read]); no weave-pattern raster, so satin floats
and twill lines need an external texture; published presets are RGB-photograph
fits needing re-fit for genuine spectral work. Its validated fabric roster is
**medium confidence** — not itemisable from the abstract alone.

**Verdict: the reference architecture for Phase 2's per-thread lobe math.**

### 3.3 The sheen family: Neubelt 2013 → Estevez & Kulla 2017 → Zeltner 2022

**Neubelt & Pettineo 2013** [verified: *Crafting a Next-Gen Material Pipeline
for The Order: 1886*, SIGGRAPH 2013 PBS course notes]. Contributed the cheap
visibility approximation `V = 1/(4·(N·L + N·V − N·L·N·V))` everything downstream
still cites [from memory of the widely-reproduced form, cross-checked against
the Khronos glTF spec text this session].

**Estevez & Kulla 2017 — "Charlie"** [verified: Sony Pictures Imageworks,
SIGGRAPH 2017 PBS course notes]. The exponentiated-sine NDF, confirmed verbatim
against the Khronos glTF spec [verified]:

```
alpha_g = sheenRoughness^2
inv_r   = 1 / alpha_g
D_Charlie = (2 + inv_r) * (1 - (N·H)^2)^(inv_r/2) / (2*pi)
```

paired with either Neubelt's V or a fuller Λ-based Charlie masking function.
The paper also proposes a **directional-albedo table `E(θ)`** and the
albedo-scaling composition [verified — glTF spec text]:

```
sheen_albedo_scaling = min( 1 - max3(sheenColor)*E(V·N),
                            1 - max3(sheenColor)*E(L·N) )
result = sheenColor * sheen_BRDF + base_material * sheen_albedo_scaling
```

**Two authorable parameters** — the reason for universal adoption: Filament
(Charlie D + Neubelt V [verified — Google's own material docs]), Blender's Sheen
BSDF, glTF 2.0 `KHR_materials_sheen` [verified], Enterprise PBR [verified], and
OpenPBR's "fuzz" slab, moved to the top of its stack so it can sit over both
base and coat [verified — OpenPBR spec text; the "descendant" framing is
editorial, not independently sourced]. **RISE already has this, with the better
visibility term** — §4.1.

**Zeltner, Burley & Chiang 2022 — LTC sheen** [verified: *Practical
Multiple-Scattering Sheen Using Linearly Transformed Cosines*, ACM SIGGRAPH 2022
Talks, DOI 10.1145/3532836.3536240; reference implementation at
github.com/tizian/ltc-sheen]. Reframes sheen as a thin volumetric layer of
near-normally-oriented fibres, path-traces that layer for reference, and fits a
Linearly Transformed Cosine to the result over roughness × incidence.
Consequences: **energy-conserving by construction** (the fit's target already
integrates correctly), **exact closed-form sampling** (an LTC is a linear
transform of a cosine lobe), and **multiple scattering captured directly**
rather than patched — closing cue (c), which is the paper's stated motivation.
Same two art-facing parameters; billed as a drop-in for Charlie. Blender 4.0's
Principled sheen switched to it [from memory — widely reported around the 4.0
release; medium confidence, not re-checked against the changelog].

**Whole-family weakness: all three are isotropic fuzz layers.** No weave
direction, no pattern structure, no thread colour-shift. The wrong tool, alone,
for satin and denim.

### 3.4 Ashikhmin, Premoze & Shirley 2000 — velvet

[verified: SIGGRAPH 2000, pp. 65-74.] A generator turning an arbitrary 2D
microfacet-normal distribution into a reciprocal, energy-respecting BRDF through
a generic shadowing term — a stronger formal guarantee than Charlie's later ad
hoc albedo patch. Its velvet result feeds the generator an **inverted-Gaussian
distribution peaked near grazing**, motivated by the authors' own microscopy:
velvet is rows of filament bundles slanted roughly 40° off the normal
[verified]. Three to four parameters. The commonly reproduced closed form is

```
D_velvet(θh; σ) ∝ exp(−tan²θh / σ²) / (σ² cos⁴θh)
```

[**from memory** — the exact normalisation constant, and whether the paper uses
this closed form versus a numerically generated arbitrary distribution, was not
confirmed against the primary PDF. **Verify before hard-coding any constant.**]

**A naming trap worth recording**, because RISE has an unrelated
Ashikhmin-coauthored BRDF in tree (`AshikminShirleyAnisotropicPhongBRDF.h`): the
"Ashikhmin" in Blender's historical Velvet BSDF is *this* 2000 velvet
distribution, not the Ashikhmin-Shirley anisotropic Phong model. Two different
papers, unrelated formulas — ordinary bibliographic fact, not separately
re-checked this session. Blender's Cycles later retired the Velvet
node for a unified Sheen BSDF with a toggle between "Ashikhmin" and a newer
microfiber multi-scatter method (the Zeltner class) [verified — PR title found].

**Verdict: strictly superseded by Charlie for RISE.** Same grazing-peak physics,
same isotropic limitation, older parameterisation, unverified normalisation. No
reason to implement it when Charlie is already shipped.

### 3.5 Wang, Jin, Hašan & Yan 2022 — SpongeCake

[verified: *SpongeCake: A Layered Microflake **Surface** Appearance Model*, ACM
TOG 41(6), 2022 (SIGGRAPH Asia 2022), arXiv 2110.07145. **The survey corrected
both the title — "Surface", not "Volume" — and the author list** the research
brief had assumed.]

Each layer is a **volumetric slab of microflakes** with no explicit interfaces
between layers, which is precisely what makes a **closed-form analytic
single-scattering solution for an arbitrary layer count** derivable. Multiple
scattering is a fitted extra single-scattering-shaped lobe plus a Lambertian
term, with a small neural network regressing physical to fit parameters
[verified from search summaries].

Per layer: albedo (spectrum), thickness/density, an SGGX roughness `alpha`, and
the SGGX shape matrix `S` — which continuously interpolates **surface-like**
flakes (normals clustered about a normal) through **fibre-like** flakes (normals
about a tangent). That continuum is the signature: one model spans satin-like
sheen through velvet pile. It handles **silhouette fuzz — cue (d) — genuinely
well**, because a phase function has no hard interface.

**Cost to RISE: an entire missing infrastructure tier.** `grep -rni
"microflake\|SGGX\|LTC\b\|linearly transformed"` over `src/Library` returns
**zero hits**. RISE's phase functions are Henyey-Greenstein
([HenyeyGreensteinPhaseFunction.h:53](../src/Library/Materials/HenyeyGreensteinPhaseFunction.h))
and isotropic
([IsotropicPhaseFunction.h:38](../src/Library/Materials/IsotropicPhaseFunction.h))
only. And SpongeCake's layers are *media*, so it lands in participating-media
transport, not the material slot. **No production adoption was corroborated; the
research brief's suggestion of Adobe Substance 3D or Unity was explicitly not
confirmed and must not be claimed.**

### 3.6 Zhu, Jarabo, Aliaga, Yan & Chiang 2023 — surface-based cloth

[verified: *A Realistic Surface-based Cloth Rendering Model*, SIGGRAPH 2023
Conference Proceedings.] The most direct "Irawan-class fidelity at surface-BSDF
cost" answer published. It reproduces the four signatures its authors identify
as necessary — an **anisotropic S-shaped reflection highlight**, a
**cross-shaped transmission highlight**, **delta transmission**, and
**inter-ply/inter-yarn shadowing-masking** — with no explicit yarn geometry. Two
capabilities distinguish it from Irawan: it models **transmission** (cloth's
backlit glow-through, which a pure-reflection BRDF cannot express at all), and
it generalises to **knitted and thin woven cloth**. Explicitly benchmarked
against Irawan-Marschner and reported to exceed it on grazing appearance and
pattern visibility thanks to the shadow-masking term [verified summary].
Followed by *A Realistic Multi-scale Surface-based Cloth Appearance Model*
(SIGGRAPH 2024) and Khattar et al.'s *A Texture-Free Practical Model for
Realistic Surface-Based Rendering of Woven Fabrics* (CGF 2025) [both
title/venue verified, neither read].

**The survey could not obtain the complete equation set, the itemised parameter
table, or a confirmed importance-sampling strategy.** That is a real gap, and it
is why Phase 2's first gate is a primary-source read.

### 3.7 The rest, briefly

- **Zhao, Jakob, Marschner & Bala 2011**, *Building Volumetric Appearance Models
  of Fabric Using Micro CT Imaging* [verified]. The fidelity reference: scan the
  fabric, render it as a heterogeneous anisotropic medium with parameters
  *extracted*, not authored. Prohibitively expensive; the ground truth
  everything else validates against; the clearest evidence that cloth's true
  structure is a volumetric fibre field.
- **Khungurn, Schroeder, Zhao, Bala & Marschner 2015**, *Matching Real Fabrics
  with Micro-Appearance Models* [verified, ACM TOG 35(1)]. Not a model — a
  **fitting framework** optimising an existing model's parameters against
  multi-light photographs through the renderer. Relevant methodologically: it is
  direct evidence that these models' parameters, though individually physical,
  are **not independently art-directable without measurement or a differentiable
  fit**. That is the strongest argument in the survey *for* shipping named
  presets rather than raw physical knobs.
- **Heitz, Dupuy, Crassin & Dachsbacher 2015**, *The SGGX Microflake
  Distribution* [verified, ACM TOG 34(4) Art. 48. **The survey corrected a
  spurious "Iwasaki" co-author** the brief had included]. The phase-function
  foundation: an anisotropic microflake distribution is characterised by its
  projected area, parameterised as a 3×3 SPD matrix `S` with
  `σ(ω) = sqrt(ωᵀ S ω)` [from memory — the central widely-reproduced result;
  normalisation not re-read]. `S` composes under linear transforms, which is how
  orientation mapping works in this family.
- **Montazeri, Gammelmark, Zhao & Jensen 2020**, *A Practical Ply-Based
  Appearance Model of Woven Fabrics* [verified, ACM TOG 39(6) Art. 251], with a
  2021 knit follow-up [verified as existing]. Between Irawan (yarn-level BRDF)
  and Zhao (fibre-level volume): plies with a lightweight closed-form BCSDF.
  Needs ply-level geometry.
- **Jin, Wang & Yan 2022**, *Woven Fabric Capture from a Single Photo*
  [verified, SIGGRAPH Asia 2022; author list **medium confidence**]. A compact
  SGGX-based woven material with an azimuthally-invariant microflake
  multi-scatter term, parameterised specifically to be *identifiable from one
  photograph*. Independently reaches nearly the same answer as Zhu 2023 from the
  inverse-rendering direction — strong evidence that "few per-point knobs plus a
  weave orientation field" is the right shape.

### 3.8 Production calibration

Every production and interchange system surveyed converges on **1-3 art-facing
sheen parameters** on top of an otherwise standard base BRDF. Unreal's Cloth
Shading Model exposes a `Cloth` mask plus a `Fuzz Color` [verified — Epic's own
docs]. Filament exposes Charlie D + Neubelt V, an energy-conservative
Lambertian, an explicitly non-physical subsurface term, a `sheenColor`, and a
fixed non-tunable `w = 0.5` wrap diffuse; its docs credit Burley and Neubelt for
the observation that this parameter set **blends robustly**, i.e. you can
linearly interpolate two cloth materials and get a plausible in-between
[verified]. **None of them attempts weave-structure fidelity.** That trade is
made deliberately, in every single production system surveyed, in favour of
parameter count and evaluation cost.

That is a fact worth sitting with before proposing Phase 2.

### 3.9 Scoring

Weights reflect what RISE actually needs: a real `Pdf` is non-negotiable (BDPT
and VCM are first-class); energy conservation is measured by a shipped harness;
infrastructure cost is the dominant schedule risk; and adoption is the measured
failure mode.

| Criterion | Irawan 12 | Sadeghi 13 | **Charlie 17 (have it)** | LTC sheen 22 | Ashikhmin 00 | SpongeCake 22 | **Zhu 23** |
|---|---|---|---|---|---|---|---|
| Cue (a) grazing brightening | partial | yes | **yes** | yes | yes | yes | yes |
| Cue (b) weave structure | **yes (raster)** | partial (statistical) | **no** | no | no | only via an `S` field | **yes** |
| Cue (c) multiple scattering | implicit diffuse | `gamma_v` lobe | **no (patched)** | **yes** | no | yes (fitted) | yes |
| Cue (d) silhouette fuzz | no | no | **no** | no | no | **yes** | no |
| Authorable params | ~18 + raster | ~5/class + tangents | **2** | 2 | 3-4 | ~4/layer × N | small-med (unconfirmed) |
| Art-directable without measurement | poor | poor | **excellent** | excellent | good | medium | unconfirmed |
| Energy conservation | approx. | not claimed exact | **bounded (patchable)** | **exact by construction** | yes (generator) | single-scatter exact | approx./empirical |
| Reciprocity | yes | yes | **yes by construction** | yes | yes | yes | yes |
| **Real importance sampler (RISE-blocking)** | **none published** | none in base paper | **closed-form\*** | **exact (LTC)** | closed-form-ish | claimed | unconfirmed |
| Eval cost | med-high | low-med | **very low** | very low | very low | low-med | BSDF-level |
| New RISE infrastructure | pattern raster resource | tangent field (Phase 1 buys it) | **none** | **LTC table + eval/sample primitive + a bake tool** | none | **microflake phase fn + media path + a fitted network** | tangent field + weave phase |
| Spectral suitability | excellent (native spectra) | good (tint; refit needed) | **good (tint)** | good (tint) | good (tint) | excellent | likely good |
| Production adoption | academic only | academic only | **de facto standard** | Blender 4.0 [medium conf.] | historical (Blender, retired) | **none corroborated** | too recent |
| **Fit as RISE's Phase-1 top lobe** | ✗ | ✗ | **✓** | ✓ but expensive | ✗ | ✗ | ✗ |
| **Fit as RISE's Phase-2 target** | ✗ (no sampler) | ✓ (lobe math) | — | ✓ (cue c) | ✗ | ✗ (infra) | **✓ (reference arch.)** |

\* Charlie's closed-form invertibility is **[from memory], not verified** — the
survey never tagged that specific sentence and it is absent from its own
verification summary. Check the paper before implementing an analytic inverse;
§9.4 and §17 carry the same hedge, and §9.4 gives the tabulated-inverse-CDF
fallback that makes the question non-blocking either way.

---

## 4. What RISE has today

### 4.1 `sheen_material` — the audit

**The model.** [CharlieSheen.h](../src/Library/Materials/CharlieSheen.h) is the
single source of truth shared by `SheenBRDF.cpp` and `SheenSPF.cpp` — a
deliberate design decision recorded in the header's own preamble. It implements
Estevez & Kulla 2017's Charlie NDF
`D(α, n·h) = (2 + 1/α)/(2π) · sin(θh)^(1/α)`
([CharlieSheen.h:39-50](../src/Library/Materials/CharlieSheen.h)) and the **full
Λ-polynomial Charlie visibility** — coefficients interpolated between the α = 0
and α = 1 Table-1 endpoints with weight `w = (1 − α)²`, matching the Khronos
sample renderer's `lambdaSheenNumericHelper` convention, and made C¹-continuous
at `x = 0.5` by the reflection `Λ(x ≥ 0.5) = exp(2·L(0.5) − L(1 − x))`
([CharlieSheen.h:71-102](../src/Library/Materials/CharlieSheen.h)), composed
into `V = 1/((1 + Λ(l) + Λ(v))·4·n·l·n·v)`
([CharlieSheen.h:104-112](../src/Library/Materials/CharlieSheen.h)).

**The name is wrong in two places.** `SheenBRDF.h:9,18` and the chunk
description ("Charlie / Neubelt sheen BRDF for fabric / cloth surfaces",
[ChunkParserRegistry.cpp:4358](../src/Library/Parsers/ChunkParserRegistry.cpp))
both say Neubelt. The Neubelt closed form was *replaced* precisely because it
blew up to ρ ≈ 8.7 at θ = 80°, and the furnace suite records the switch
([LayeredWhiteFurnaceTest.cpp:661-670](../tests/LayeredWhiteFurnaceTest.cpp)).
This matters beyond tidiness: the chunk description is what the agent-facing
schema generator and both GUI scene editors surface, so the stale name is on the
authoring surface, not just in a comment.

**Energy.** No directional-albedo table exists anywhere in the sheen code. The
`albedo()` override is an OIDN AOV estimate that clamps sheen colour to itself
([SheenBRDF.cpp:133-141](../src/Library/Materials/SheenBRDF.cpp)), explicitly
not a directional-albedo model. The furnace configuration is therefore
`kPostureBounded` — ρ must not exceed 1, but may legitimately fall below it,
because Charlie is single-scattering and dissipates at grazing
([LayeredWhiteFurnaceTest.cpp:668-671](../tests/LayeredWhiteFurnaceTest.cpp)).

**Sampling.** Plain cosine-weighted hemisphere
(`GeometricUtilities::CreateDiffuseVector`,
[SheenSPF.cpp:73-74](../src/Library/Materials/SheenSPF.cpp)), stated in the
header as a deliberate choice because "Charlie's distribution doesn't have a
clean closed-form importance sample" (`SheenSPF.h:3-9`) — a claim §9.4 revisits.
`pdf = nDotL · 1/π` ([SheenSPF.cpp:116](../src/Library/Materials/SheenSPF.cpp)),
`kray = colour · D · V · π`
([SheenSPF.cpp:110](../src/Library/Materials/SheenSPF.cpp)). **The scattered ray
is tagged `eRayDiffuse`**
([SheenSPF.cpp:113](../src/Library/Materials/SheenSPF.cpp)), so `max_diffuse_bounce`
governs sheen depth today, not `max_glossy_bounce` — relevant to §12.
There is also a geometric-horizon gate against `GlintModifier`'s shading-normal
tilt ([SheenSPF.cpp:86-99](../src/Library/Materials/SheenSPF.cpp)), a detail any
new fabric SPF must replicate rather than rediscover.

**Spectral.** A full RGB/NM twin, not a stub: `SheenBRDF::valueNM`
([SheenBRDF.cpp:100-131](../src/Library/Materials/SheenBRDF.cpp)) and
`SheenSPF::ScatterNM`/`PdfNM`
([SheenSPF.cpp:121-196](../src/Library/Materials/SheenSPF.cpp)), reading
`pRoughness->GetValueAtNM` and `GuardedGetColorNM(*pColor, ri, nm)` — already
conformant with the white-guard convention.

**Reciprocity.** True by construction — `D` depends only on `n·h`, and `V` is
symmetric in `nDotL`/`nDotV`. **Untested.** Sheen appears in no
reciprocity or SPF↔BSDF-consistency sweep
([SPFBSDFConsistencyTest.cpp:1140-1146](../tests/SPFBSDFConsistencyTest.cpp)
lists Lambertian, isotropic GGX, and three Coated configurations only), and
`grep -l Sheen tests/*.cpp` finds only the furnace test, the JH white-guard
test, a rewire test, and the expression-VM test.

**Chunk surface.** `sheen_material { name, sheen_color, sheen_roughness }` —
`sheen_color` is a required Color-pipe painter reference, `sheen_roughness` is a
Scalar-pipe reference with `requireSingle = true` and a `0.5` default hint,
clamped to ≥ 1e-3 internally
([ChunkParserRegistry.cpp:4344-4370](../src/Library/Parsers/ChunkParserRegistry.cpp)).
Two parameters. That is the whole authoring surface for fabric in RISE today.

### 4.2 The layering gap, stated precisely

§1 gap 1 gives the two mechanisms. Two refinements matter for the design.

First, **the sheen case IS config 7's defect — measured 2026-09-02 (round 5),
where the earlier text declined to assert it.** Config 6, "Sheen / GGX-PBR",
used to be `kPostureBounded` with the note "sheen-over-PBR inherits sheen's
bounded dissipation", which did *not* carry config 7's "substrate never
reached" language; this document declined to assert the stronger reading
without running config 7's SPF-level downward-ray probe against it. §9.9
gate 4 made that a Phase-1 item, and the probe has now run:

> **`SheenSPF` emits a downward ray in 0 % of draws at θ = 0 and 0 % at
> θ = 80** — identical to config 7's GGX top layer.

`SheenSPF` is reflection-only by construction: it draws a cosine-hemisphere
direction about the *ray-facing* normal and gates anything below the
geometric horizon
([SheenSPF.cpp:73-99](../src/Library/Materials/SheenSPF.cpp)). So it never
hands `CompositeSPF`'s random walk a downward ray, and **the substrate is
never reached**. The furnace table corroborates it independently: config 6
reads `{0.0875, 0.1293, 0.2812, 0.5461}` while config 2 — *bare sheen, no
substrate at all* — reads `{0.0875, 0.1283, 0.2810, 0.5453}`. A composite
that reached its GGX-PBR base could not land on the bare lobe's own curve.

**Configs 6 and 7 are therefore one defect, not two**, and closing it needs a
transmission path for reflection-only top layers rather than a walk fix.
Config 6's note now carries the measured percentages, computed at test time
rather than remembered. §15 debt 3 is closed on this evidence.

Second, **`composite_material`'s `extinction`/`thickness` are not an energy
split.** They are Beer-Lambert gap absorption between the layers
([CompositeSPF.cpp:121-136](../src/Library/Materials/CompositeSPF.cpp)), not an
"attenuate the base by (1 − sheen albedo)" term. No such term exists anywhere in
the sheen or composite code. The glTF albedo-scaling law of §3.3 has no
implementation in RISE.

Third, and structurally: `ScatteredRayContainer::kCapacity = 12`
([ISPF.h:116](../src/Library/Interfaces/ISPF.h)) is a hard, **silently dropping**
cap on lobes per `Scatter()` call. A fabric material assembled as a deep
composite (specular yarn highlight + sheen + diffuse base, nested under something
else) risks losing energy invisibly. This is an argument for a single triad that
owns its lobes, not for stacking.

### 4.3 Anisotropic BRDFs and the tangent frame

**Which materials have anisotropy.** GGX (`alphaX`/`alphaY`,
[GGXBRDF.h:47-48](../src/Library/Materials/GGXBRDF.h)); Ward (`alphaU`/`alphaV`);
Ashikhmin-Shirley (`Nu`/`Nv` Phong exponents). **Cook-Torrance is isotropic
only** — no anisotropy parameters exist on it.

**Which have a rotation input.** GGX alone, via an optional
`const IPainter* pTangentRotation` in radians applied by
`MicrofacetUtils::RotateTangent`
([GGXBRDF.cpp:120-127](../src/Library/Materials/GGXBRDF.cpp)). Ward and
Ashikhmin-Shirley have none. The rotation binds through the **Color** pipe, and
the descriptor says so and calls it what it is: *"an angle in radians by
MEANING, plumbed through the Color pipe (IPainter) so an expression_function2d
painter can drive a spatially-varying groove direction. A scalar_painter does
NOT bind here"* — `p.semantics.note`, [ChunkParserRegistry.cpp:4193](../src/Library/Parsers/ChunkParserRegistry.cpp).

**Where the tangent comes from — the finding.** §1 gap 3. What matters for the
design is the *shape* of the fix. The general per-hit override already exists,
is already consumed by `Object::IntersectRay` and `CSGObject::IntersectRay`, and
already handles world-space promotion with the forward matrix, singular-transform
clearing, degenerate-projection fallback, and CSG-nested write-back
([Object.cpp:699-772](../src/Library/Objects/Object.cpp),
[RayIntersectionGeometric.h:314-345](../src/Library/Intersection/RayIntersectionGeometric.h)).
`HairGeometry` writes it in three lines:

```cpp
ri.bShadingTangentFromGeometry = true;
ri.vShadingTangent  = T;
ri.bHasShadingTangent = true;
```

**Nothing in the field declaration or the consuming branch restricts the writer
to curve geometry.** A triangle mesh that already computed `dpdu` can write the
same three lines and inherit the entire mechanism.

Two implementation details found in source that shape the fix:

- **Ordering.** `Object::IntersectRay` builds the ONB at lines 699-772 but
  promotes `derivatives.dpdu` to world space at lines 832-836 — *after*. So the
  write must happen at the **geometry** level in object space
  (`vShadingTangent = dpdu` alongside the existing `derivatives.dpdu` write),
  and the existing tangent-promotion branch does the rest. No reordering, no
  new `Object`/`CSGObject` code path.
- **Normal mapping survives it.** `NormalMap::Modify` rebuilds the ONB with
  `CreateFromWU` from the projected current `u` **when and only when
  `bHasShadingTangent` is set**, and with an arbitrary `CreateFromW` otherwise
  ([NormalMap.cpp:186-197](../src/Library/Modifiers/NormalMap.cpp)). So a
  geometry-level fix gives normal-mapped fabric a coherent tangent for free,
  where a material-side `ResolveTangentONB` preference would not. **This is the
  decisive argument for doing it at the geometry level.**

The analytic primitives populate `dpdu` too — sphere, ellipsoid, torus, cylinder
all write `ri.derivatives.dpdu` and `valid = true`
(e.g. [SphereGeometry.cpp:159-163](../src/Library/Geometry/SphereGeometry.cpp)),
so the same fix gives a cloth-on-sphere test scene a meaningful warp direction
without a mesh.

### 4.4 The painter stack for weave authoring

**The pipes.** `IPainter` (colour/reflectance/emission, JH-uplifted on the
spectral path) and `IScalarPainter` (physical scalars, `ScalarTriple`, **no
colorspace anywhere**, no `GetColorNM` path at all — that absence is the
structural guard, [ISCALARPAINTER_REFACTOR.md](ISCALARPAINTER_REFACTOR.md)).
`ParameterPipe` enumerates `Unspecified | Color | Scalar | Material | Function1D
| Function2D | Geometry | Other`
([ChunkDescriptor.h:184-194](../src/Library/Parsers/ChunkDescriptor.h)). **There
is no vector-valued pipe and no `IVectorPainter`.** Live usage in the registry:
Color 76, Scalar 60, Function 5, Material 5, Other 1.

**The expression VM.** `ExpressionEval.h` is the source of truth. Context: `u`,
`v` always; `P`, `Po`, `N` (vec3); `curv`, `curvR` (signed mean curvature from
the **geometric** normal field — bump and normal maps do not move it); `fw`
(world-space filter width, 0 where no footprint exists); `time`; plus any named
`param` constants and ordered `def` let-bindings. Builtins: the unary/binary/
ternary math set including `floor`, `mod`, `atan2`, `step`, `smoothstep`,
`select`, `mix`, `ramp`; vec3 `dot`/`cross`/`length`/`normalize` and `.x/.y/.z`;
the noise family `perlin`/`fbm`/`turbulence`/`ridged`/`worley_f1`/`f2`/`f2f1`/
`id`/`cellhash`; and the two **arg-taking geometry-signal builtins**
`occlusion(radius)` and `thickness(radius)`. Operators `+ - * / % ^` and the six
comparisons (yielding 1.0/0.0).

**Both a colour and a scalar expression painter exist**: `expression_painter`
(Color pipe, full 3D context) and `scalar_painter { expression … }` (Scalar
pipe, **also full 3D context** including `curv`, `fw` and the signal builtins —
the descriptor is explicit; `expression_function2d` by contrast is UV-only). This
matters for §5's pipe decision: the Scalar pipe loses nothing in expressiveness.

**Painters flagged as most useful for weave.** `gabor3d_painter`
([ChunkParserRegistry.cpp:2504](../src/Library/Parsers/ChunkParserRegistry.cpp)) —
oriented band-limited noise, the closest existing "fibre flow" field.
`voronoi2d_painter` ([:2851](../src/Library/Parsers/ChunkParserRegistry.cpp)) —
explicit art-directed cells, each with its own painter; one cell per weave
repeat is expressible. `mapping_painter` ([:6825](../src/Library/Parsers/ChunkParserRegistry.cpp)) —
scale/rotate/translate/triplanar reprojection of the domain, the tiling tool.
`stochastic_tile_painter` ([:6896](../src/Library/Parsers/ChunkParserRegistry.cpp)) —
histogram-preserving hex tiling that kills visible repetition.
`blend_painter` ([:3071](../src/Library/Parsers/ChunkParserRegistry.cpp)) — the
composition verb. Notably, **`lines_painter`'s own descriptor redirects fabric
authors away from itself**: "for real fabric weave … use gabor3d_painter /
turbulence3d_painter instead"
([:2025](../src/Library/Parsers/ChunkParserRegistry.cpp)).

**Filtering — the caveat that will bite.** `ri.txFootprint` exists
(`{dudx, dudy, dvdx, dvdy, worldWidth, valid}`, the `TextureFootprint`
struct in [RayIntersectionGeometric.h](../src/Library/Intersection/RayIntersectionGeometric.h))
and is consumed by `TexturePainter`'s mip path, `MappingPainter`'s domain
transform, and the expression VM's `fw` variable. But `fw` fades octaves toward
Nyquist **only inside `fbm`, `turbulence` and `ridged`**. The plain procedural
painter chunks — `perlin3d_painter`, `worley3d_painter`, `perlin2d_painter`,
`checker_painter`, `lines_painter` — carry **zero** references to `txFootprint`
or `fw`. A high-frequency weave authored through them aliases under minification
with no built-in mitigation. §5.5 gives the authoring answer.

**Geometry signals and the `add_wear` precedent.** `curv`/`curvR` plus
`occlusion(r)`/`thickness(r)` are the whole signal set; there is no "signal
painter" chunk, the VM is the only consumer surface. `add_wear` is the shipped
template for curvature-driven wear, and its mask prelude is worth quoting
verbatim because §9.8's worked example is built on it
([AgentSession.cpp:36610-36628](../src/Library/Agent/AgentSession.cpp)):

```
def   jitter        vec3(seed, seed*1.7, seed*2.3)
def   wear_mask     clamp(curv*edge_wear + breakup_amp*fbm(P*breakup_scale + jitter,4,0.5,2.0), 0, 1)
def   crevice_raw   clamp(-curv*crevice_grime + breakup_amp*fbm(P*grime_scale + jitter,4,0.5,2.0), 0, 1)
def   cavity_boost  1.0 + cavity_gain*(1.0 - occlusion(0.08))
def   crevice_mask  clamp(crevice_raw*cavity_boost, 0, 1)
```

Positive curvature (edges) drives wear; negative curvature (crevices) drives
grime, deepened by occlusion. **A seam is a locus of positive curvature.** This
is already the right mechanism for fabric seam wear, already shipped, already
callable as a verb.

### 4.5 Hair machinery available for reuse

Three things transfer, one does not.

**Transfers: the lobe math.** `Mp`, `TrimmedLogistic`/`SampleTrimmedLogistic`,
`FrDielectric`, `MakeGeom`, `ComputeAp` and friends are stateless free functions
in an anonymous namespace in `HairBSDF.cpp` (opening at
[HairBSDF.cpp:35](../src/Library/Materials/HairBSDF.cpp), member definitions
starting only at :872). None takes `this`; all operate on small caller-supplied
value structs. A header promotion — delete the anonymous namespace, promote the
structs — is mechanical and low-risk, and is the prerequisite for a Sadeghi-class
microcylinder lobe in Phase 2.

**Transfers: the tangent mechanism.** §4.3.

**Transfers: the scattering-profile LUT.** `HairMedullaProfile` is a generic
azimuthal scattering profile of *one crossing of a unit-radius, infinitely long,
non-absorbing scattering cylinder*
([HairMedullaProfile.h:20-23](../src/Library/Materials/HairMedullaProfile.h)),
indexed by impact parameter, optical depth and HG anisotropy — 8 × 10 × 5 cells
× (32 azimuthal bins + 1 longitudinal variance) = 13,200 floats = **52.8 KB**.
`Eval` and `Sample` are exact inverses by construction, which is exactly the
property an importance-sampled lobe needs. Nothing about its axes is
hair-specific; a yarn's forward-scattering term could re-bake it at
yarn-appropriate (τ, g) ranges using RISE's own generator
(`tools/HairMedullaProfileGen.cpp`). Measured overhead when engaged: ~1.36× on a
furnace groom (952/955/957 ms at κ = 0 vs 1298/1293/1305 ms at κ = 0.7,
[HairBSDF.h:267-275](../src/Library/Materials/HairBSDF.h)).

**Does not transfer: the orchestration.** `HairScatteringBase` owns eleven
painter pointers and a `Resolve` that reads them per hit; a fabric material needs
its own painter set and its own resolve. And the *sampling* PMF is deliberately
achromatic (a component-wise-minimum RGB proxy) so `EvaluateKrayNM` can
reconstruct the hero pdf exactly — a convention a fabric BSDF should copy but
cannot inherit.

### 4.6 Scenes, tests, import

**Scenes.** One: `scenes/Tests/Materials/sheen.RISEscene`, three spheres
(velvet α = 0.1 white, satin α = 0.5 gold, blurred α = 1.0 pink) applied
directly with **no composite base at all**, under a single directional key
`0.4 0.4 0.7` and an ambient fill. No cushion, curtain, garment or drape exists
anywhere in `scenes/`.

**Tests.** [LayeredWhiteFurnaceTest.cpp](../tests/LayeredWhiteFurnaceTest.cpp)
drives 19 configurations through a Monte-Carlo furnace at θ ∈ {0°, 30°, 60°,
80°}, 100k samples per (config, angle), under four postures — `kPosturePass`,
`kPostureBounded`, `kPostureMatchesPrediction`, `kPostureKnownFailure`. The
harness is generic over any `ISPF&`, so **a new fabric SPF plugs in by adding
one `add()`/`addPredicted()` line**. The reciprocity harness
([SPFBSDFConsistencyTest.cpp:361-491](../tests/SPFBSDFConsistencyTest.cpp)) tests
an exact property at `RECIPROCITY_TOL = 1e-6` relative over arbitrary direction
pairs, max-channel comparison — and its sweep list has no sheen and no
anisotropic entry.

**Import.** glTF `KHR_materials_sheen`: detected, warned, skipped; the composite
layering code sits `#if 0`'d and preserved for future work
([GLTFSceneImporter.cpp:1449-1466](../src/Library/Importers/GLTFSceneImporter.cpp)).
`KHR_materials_anisotropy`: strength imports, including per-pixel strength from
the texture's B channel; **per-pixel rotation is explicitly dropped**, with the
importer stating that an `atan2` painter primitive or a contract change would be
needed
([GLTFSceneImporter.cpp:1300-1307](../src/Library/Importers/GLTFSceneImporter.cpp)).
Blender bridge: `grep -i "sheen|anisotrop|velvet"` over
[BLENDER_MATERIAL_TRANSLATION.md](BLENDER_MATERIAL_TRANSLATION.md) returns **zero
matches** — Principled BSDF's Sheen, Sheen-Tint, Anisotropic and
Anisotropic-Rotation sockets are not in the supported-node table and not in the
force-bake list. A silent gap, not an explicit rule.

---

## 5. Weave structure as a painter problem

### 5.1 What a weave cell actually is

A woven fabric is a periodic 2D lattice. In each cell, one of the two yarn
families is uppermost. Three canonical bindings cover most cloth:

- **Plain weave** (1/1): warp over, weft over, alternating in both directions.
  Parity `(i + j) mod 2`. Cotton, linen, taffeta, poplin.
- **Twill 2/1 or 3/1**: the crossing point advances by one cell per row, which
  produces the diagonal wale. Parity `(i + k·j) mod N` for a shift `k`. Denim,
  gabardine, chino.
- **Satin, 5-harness**: a long *float* — one yarn passes over four before going
  under one — arranged so crossings never touch. Parity `(i + s·j) mod N == 0`
  for a satin step `s` coprime to `N`. Silk satin, sateen, duchesse.

The visual difference between these three at a distance is **almost entirely a
difference in highlight structure**, not in colour. That is cue (b), and it is
why a weave-aware BSDF is a genuinely different thing from a sheen lobe.

### 5.2 What a weave-aware BSDF needs per shading point

Four inputs, and it is worth being precise about their types because the type is
the design decision:

| input | type | meaning |
|---|---|---|
| **yarn direction** | scalar angle (radians) in the tangent plane | which way the uppermost yarn runs; warp = 0, weft = π/2 |
| **weave-cell phase** | scalar in [0,1] or a binary | which family is on top here; drives the family blend and the shadow-masking term |
| **yarn normal variation** | a normal perturbation | the yarn's own cylindrical cross-section, which is what makes the highlight a band rather than a point |
| **coverage** | scalar in [0,1] | how much yarn vs how much gap; the thin-cloth/openness parameter |

**All four are scalar or normal-shaped fields. None of them is a 3-vector that
must be authored as such.** That observation is what settles §5.4.

### 5.3 Authoring each with painters that exist today

**Weave-cell phase.** Directly expressible in the VM. The plain-weave parity —
verified against the operator and function table (`floor` unary, `mod` binary,
`+`/`*` in the grammar):

```
expression_function2d
{
	name			weave_plain
	param			N 48.0
	expr			mod( floor(u*N) + floor(v*N), 2.0 )
}
```

Twill and satin are the same shape with a shift, using `%` or `mod`
interchangeably:

```
expression_function2d
{
	name			weave_twill_3_1
	param			N 48.0
	param			shift 1.0
	def			i floor(u*N)
	def			j floor(v*N)
	expr			step( 0.5, mod( i + shift*j, 4.0 ) ) * step( mod( i + shift*j, 4.0 ), 2.5 )
}
```

```
expression_function2d
{
	name			weave_satin5
	param			N 40.0
	param			step5 2.0
	def			i floor(u*N)
	def			j floor(v*N)
	expr			1.0 - step( 0.5, mod( i + step5*j, 5.0 ) )
}
```

The satin form yields 1.0 only on the one cell in five where the weft crosses,
and 0.0 on the four float cells — which is the correct topology: a satin's face
is almost entirely warp float, and that is *why* it is shiny.

**Yarn direction.** A scalar angle field over the cell phase. Warp runs along
+U, weft along +V, so the angle is π/2 on weft-top cells and 0 on warp-top ones,
with the float direction following the dominant family:

```
scalar_painter
{
	name			weave_angle_satin
	param			N 40.0
	param			step5 2.0
	param			jitter_amp 0.06
	seed			7
	def			i floor(u*N)
	def			j floor(v*N)
	def			weft_on_top 1.0 - step( 0.5, mod( i + step5*j, 5.0 ) )
	def			jitter jitter_amp * (fbm(P*90.0 + vec3(seed, seed*1.7, seed*2.3), 3, 0.5, 2.0) - 0.5)
	expression		weft_on_top * 1.5707963 + jitter
}
```

The `jitter` term is not decoration: it is the cheap analogue of Irawan's four
`d*UmaxOverD*` noise derivatives, whose entire purpose is to break the perfectly
periodic look that makes procedural cloth read as printed rather than woven.

**Yarn normal variation.** `normal_map_modifier` already decodes tangent-space
normals in the standard glTF convention, from either an imported `TANGENT`
accessor or `dpdu`/`dpdv`
([NormalMap.cpp:71-81, 109-120](../src/Library/Modifiers/NormalMap.cpp)). It is
production code, not a proposal. The **authoring trap is documented and
load-bearing**: the painter feeding it must be loaded with `color_space
Rec709RGB_Linear` — no gamma decode, no colour-matrix conversion — or the
decoded vector is wrong (`NormalMap.h:9-24`). Mipmapping is on by default for
raster textures and must be explicitly disabled for vector-quantity textures
(`Job.cpp:1951`).

**Coverage.** Any scalar field; for a sheer curtain, `1 − weave_phase` scaled by
an openness parameter.

### 5.4 The pipe decision — angle field, not a vector pipe

**Recommendation: author yarn direction as a scalar angle through
`IScalarPainter`. Do not add a vector pipe.**

The argument has three legs.

*It is sufficient.* §5.2 established that all four weave inputs are scalar- or
normal-shaped. A yarn direction lying in the tangent plane has exactly one
degree of freedom, and the angle is it. The only thing a vector pipe would add
is a genuinely non-planar yarn lay direction, which no surveyed model needs at
BSDF level.

*It is cheap.* The `weave_rotation` slot is one descriptor line
(`p.semantics.pipe = ParameterPipe::Scalar; p.semantics.requireSingle = true;`)
plus one `ResolveOrDiagnoseScalar` call in `Job::AddFabricMaterial`, which
already emits the correct three-way diagnostic — bound-to-wrong-pipe,
bound-to-unknown, bound-to-per-channel — with wording that `ConnectionLegality`
quotes verbatim rather than re-deriving
([ChunkDescriptor.h:64-72](../src/Library/Parsers/ChunkDescriptor.h),
`Job.cpp:3463-3522`).

*A vector pipe is not.* The survey enumerated fourteen touchpoints for a
hypothetical `IVectorPainter`:

| # | touchpoint |
|---|---|
| 1 | `IVectorPainter.h` + `IVectorPainterManager.h` |
| 2 | Concrete implementations (uniform, per-UV, composition) |
| 3 | `ParameterPipe::Vector` — **appended, never reordered** (the sibling `ChunkCategory` enum crosses the GUI ABI as a bare int, [ChunkDescriptor.h:281-286](../src/Library/Parsers/ChunkDescriptor.h)) |
| 4 | `RISE_API_Create*VectorPainter` per implementation (10 such exist for `IScalarPainter`) |
| 5 | `IJob`/`IJobPriv`/`Job` manager member + `ResolveVectorPainterArg`/`ResolveOrDiagnoseVector` |
| 6 | A `vector_painter` `ChunkDescriptor` with its own form set |
| 7 | 2-3 new `kVectorBoundTo…Fmt` diagnostic constants |
| 8 | `ConnectionLegality::CheckConnection` pipe branch |
| 9 | `PainterIntrospection` — the properties-panel pipe bitmask gains a third bit |
| 10 | `Agent/SchemaGen.cpp` + `Agent/AgentDiagnostic.h` |
| 11 | Every consuming material slot's descriptor line |
| 12 | 5 build-project manifests per new `.cpp` |
| 13 | `IVectorPainterTest.cpp` + `VectorPainterParserTest.cpp`, plus the `SceneEditorSuggestionsTest` keyword-count bump |
| 14 | `gui/NODE_GRAPH_CANVAS.md` §6, `gui/MATERIAL_EDITOR.md` §6.4, `GUI_ROADMAP.md:361` — all describe the two-pipe model by name |

And the precedent prices it honestly: the `IScalarPainter` refactor is laid out
in ten phase headings, Phase 0 through Phase 9 — nine of them implementation
phases after the Phase-0 design-and-baseline step — and is still not fully
closed out (every checklist item under Phase 5's `Job.cpp` cleanup remains
unticked). **A vector pipe is deferred and observed-need gated**; the observed
need would be a genuine non-planar direction requirement that a Phase-2 model
turns out to have, or glTF's per-texel `anisotropy_rotation` (whose R/G channels
encode cos/sin) proving unrepresentable through an `atan2` expression — and note
that the VM *has* `atan2`, so that particular blocker is already soluble without
a new pipe.

**The one honest cost of the Scalar choice: a pipe split with GGX.**
`ggx_material.tangent_rotation` binds Color; `fabric_material.weave_rotation`
would bind Scalar. An author who writes `expression_function2d` for one and tries
it on the other gets a hard diagnostic rather than silence — but it is still a
seam. §15 debt 4 records it with the recommended resolution: add a Scalar-pipe
alias to GGX in a later slice and deprecate the Color binding, since the
descriptor already calls the Color binding an oddball rather than a pattern.
Note also that `scalar_painter { function2d <name> }` (form 7) lets an
`expression_function2d` reach the Scalar pipe today, so nothing an author can
express is lost.

### 5.5 Aliasing — the caveat that will bite, and its answer

A weave cell at N = 48 over a cushion is roughly a millimetre; at typical render
resolutions it is sub-pixel over most of the frame. Every painter in §5.3 that
produces a **hard step** — `floor`, `mod`, `step` — aliases, and §4.4 established
that the plain procedural painters have no footprint-aware filtering at all.

The VM can express the mitigation, because `fw` is a context variable and
the correct limit of any weave cell under minification is its **mean**:

```
scalar_painter
{
	name			weave_angle_satin_af
	param			N 40.0
	param			step5 2.0
	param			cell_world 0.0008
	def			i floor(u*N)
	def			j floor(v*N)
	def			weft_on_top 1.0 - step( 0.5, mod( i + step5*j, 5.0 ) )
	def			fade smoothstep( cell_world*0.5, cell_world*2.0, fw )
	expression		mix( weft_on_top * 1.5707963, 0.7853981, fade )
}
```

As the footprint `fw` grows past the cell size the angle field fades to π/4 —
the mean of a two-family weave — and the anisotropy fades with it, which is
exactly right: a woven surface seen from far enough away *is* isotropic. Note
`fw` is **0.0 where no footprint is available** (secondary bounces, non-mesh
geometry), and `smoothstep(a, b, 0)` is 0, so the fade correctly disengages on
those hits rather than snapping to the mean. This idiom should be in the skill
text, because no author will derive it.

The complementary tool is `stochastic_tile_painter`, which addresses *tiling
repetition* — a different problem from minification aliasing, and worth naming as
such so nobody reaches for it expecting anti-aliasing.

---

## 6. Geometry-level versus BSDF-level

### 6.1 Where the line falls

| fabric class | dominant cue | correct level | why |
|---|---|---|---|
| Cotton, linen, poplin, canvas | (a) + weak (b) | **BSDF** | The weave is at or below the pixel; structure reads statistically |
| Denim, gabardine, twill | (b) | **BSDF with a weave field** | Twill wales are visible but the yarn silhouette is not |
| Silk, satin, sateen, taffeta | (b) strongly | **BSDF with a weave field** | Float direction is the whole look; the yarns are smooth and fine |
| Velvet, suede, moleskin | (a) + (c) | **BSDF** | The pile is far below the pixel; it is the *distribution* that matters |
| Wool, tweed, fleece | (a) + (c) + (d) | **BSDF, with (d) unserved** | Honest partial answer |
| Chunky knits, macramé, rope, hero close-ups | (d) + real geometry | **geometry** | The loop silhouette is the subject |

The line is drawn by **whether the yarn's silhouette is visible**. Below that,
every model in §3 agrees a BSDF is the right abstraction and the debate is only
which one.

### 6.2 What a geometry route would cost

RISE has a ready-made engine for many thin curved primitives over a shared
surface: `HairGeometry`'s flat float control-point arrays, per-strand offsets and
cumulative arc lengths, a `BVH<HairSegmentRef>` with `maxLeafSize = 4` and
8-byte segment references, build-time per-span subdivision into up to 2³
sub-segments chosen from the span's own curvature, and a `Realize()`-hook
generator that area-samples a base mesh and rejection-filters by a
`density_painter`
([HairGeometry.h:107-178, 427-451](../src/Library/Geometry/HairGeometry.h),
[HairGenerator.h:9-11,40-44,118-124](../src/Library/Geometry/HairGenerator.h)).

Measured, from the hair arc:

| quantity | measured value | source |
|---|---|---|
| Build, 100K strands | **647 ms** | commit `d579bcfa` message, carried from `HairGeometryTest` |
| Memory, 100K strands | **73.7 MiB** | ibid. |
| Traversal, 2K closest-hit rays | **12.7 ms** | ibid. |
| Traversal, 50K closest-hit rays, 100K × 8 CPs | **287.5 ms ± 2.5 ms** | HAIR_FUR_DESIGN §7 Phase 4 |
| Profile split | `RecursivePieceIntersect` 54 %, `BVH::IntersectRay` 20 %, `IntersectSegment` self 14 %, `EvalSpanCoefficients` 8 % | ibid. |
| Best per-leaf optimisation found | **−3.64 %** (ray-frame hoist), not banked | ibid. |

Now the arithmetic for a woven cushion face. Take 40 cm × 40 cm at 30 threads/cm:
**1200 warp + 1200 weft = 2400 threads**. Resolving a plain weave's over/under
crossing needs roughly four control points per crossing, and each thread crosses
1200 others: **≈ 4800 CPs per thread**, hence **≈ 11.5 M control points**. The
measured point is 100K strands × 8 CPs = 800K CPs at 73.7 MiB — that is
77,279,232 bytes over 800K CPs, i.e. **~96.6 bytes per CP** all-in (binary units
throughout; the source figure is `MiB` verbatim from the commit message).
Extrapolating linearly:

> **≈ 1.03 GiB and ≈ 9.3 s of build time for one cushion face.**

**This is a linear extrapolation from a single measured point, not a
measurement.** It is nonetheless decisive: it is off by more than an order of
magnitude from anything usable, and it does not improve with a better BVH,
because the cost is the control points themselves.

**The saving grace of woven fabric is that it is periodic** — and that is exactly
the primitive RISE does not have. No periodic or instanced placement primitive
for strand-scale geometry exists. `path_instances_geometry` *duplicates* a
template mesh per instance into one flat mesh with hard caps of 20 M vertices and
100 K instances and is called "structurally unusable" at strand scale in
[HAIR_FUR_DESIGN.md](HAIR_FUR_DESIGN.md) itself; per-strand `standard_object`
instancing is rejected in the same document (1.4 GB of overhead plus a TLAS of
interpenetrating thin AABBs at 1 M strands). A knit or woven yarn system would
need either its own `HairGeometry`-style flat-array primitive with one shared BVH
over all loop segments, or genuinely new instancing infrastructure.

### 6.3 Recommendation

**Everything in §6.1's first five rows is BSDF work. Only the sixth row is
geometry work, and it is gated behind a missing primitive.** Silhouette fuzz —
cue (d) — is therefore honestly unserved by Phases 1 and 2, and §16 records that
as a non-goal rather than pretending a BSDF term supplies it. If it is ever
wanted, the cheapest partial answer is not yarn geometry but a **fuzz shell**: a
thin offset surface carrying a very rough, very low-albedo fabric material, which
softens the silhouette read without any new primitive. That is a Phase-3 sketch,
not a recommendation.

---

## 7. Architectural options considered

### (A) Fix layering, upgrade sheen in place, add UV tangents, ship presets via a verb — **REJECTED as insufficient**

Keep `sheen_material`; make `composite_material` work by giving `SheenSPF` a
downward transmission lobe; add the E table; add the tangent fix; deliver presets
through a verb that emits a composite.

Attractive because it adds no material class. It fails on two counts. First,
`CompositeMaterial::GetBSDF()` still returns only the top's BSDF
([CompositeMaterial.h:56-63](../src/Library/Materials/CompositeMaterial.h)), so
NEE and every BDPT connection would still miss the base — fixing the SPF walk
does not fix the BSDF side, and fixing the BSDF side means writing a combined
`value()` that sums two lobes with the Kulla-Conty energy split, which *is* the new
material. Second, `CompositeSPF`'s `Pdf` is an admitted flat 50/50 average of the
two sub-PDFs — `return 0.5 * (pdf_top + pdf_bottom);` under the comment "Equal
weighting is the best approximation for this material"
([CompositeSPF.cpp:586-598](../src/Library/Materials/CompositeSPF.cpp), with the
identical `PdfNM` twin at :600-611) — which is not the mixture PDF the albedo
split implies, so MIS weights would be wrong even with both lobes present.
The composite is a stochastic compositor, not a layered BSDF, and no amount of
patching makes it one — the same conclusion
[WETNESS_COAT_DESIGN.md](WETNESS_COAT_DESIGN.md) reached for the coat.

### (B) A new `fabric_material` triad — **RECOMMENDED**

One `IMaterial` + `IBSDF` + `ISPF` triad in `coated_material`'s architectural
shape: a **base-material reference** with an allowlist, a closed-form `value` /
`valueNM` that sums the Charlie lobe and the albedo-scaled base so NEE and BDPT
see the whole response, a real mixture `Pdf`, a `weave_rotation` angle painter
that rotates the frame handed to the substrate, a dye colour, and a `fabric`
preset enum.

The decisive argument is that **RISE has already built this exact shape once and
reviewed it to zero P1**: `coated_material` is a Material + BRDF + SPF + Layer
set with a `dynamic_cast` substrate allowlist, a view-independent
`hemisphericalAlbedo` contract added specifically because a view-dependent
`albedo()` in a shared term broke reciprocity by ~28 % at grazing
([IBSDF.h:87-97](../src/Library/Interfaces/IBSDF.h)), and furnace + reciprocity
+ pdf-consistency coverage. `fabric_material` is its sibling: **same
architecture, different top lobe, different energy split** (albedo-based per the
glTF sheen law, not Fresnel-based per the coat law).

It also unblocks glTF: `KHR_materials_sheen` is a sheen layer over a PBR base,
which is precisely what a base-material-reference `fabric_material` accepts. That
is the second paying customer, in the mould of clearcoat's for
`coated_material`.

### (C) Full Irawan-Marschner — **REJECTED**

~18 parameters plus a pattern raster, a preset library that must be fit to
photographs, **no published importance sampler** (§3.1), and no adoption in any
mainstream shading system. The missing sampler alone disqualifies it: RISE's
BDPT and VCM require a real `Pdf` per lobe, enforced by
[SPFPdfConsistencyTest.cpp](../tests/SPFPdfConsistencyTest.cpp). Retained as
Phase 2's fidelity comparison baseline, which is the role the rest of the
literature also gives it.

### (D) SpongeCake layered microflake — **REJECTED for now**

The strongest model for cues (a), (c) and (d) simultaneously, and the only
surveyed model that gets silhouette fuzz without geometry. Rejected on
infrastructure: RISE has **zero** microflake/SGGX/LTC code, its phase functions
are HG and isotropic only, its layers would be *media* rather than surfaces
(landing in participating-media transport, not the material slot), and its
multi-scatter term is a fitted neural network. No production adoption was
corroborated. **Not rejected on merit** — if RISE ever wants a volumetric fabric
tier, SGGX-as-a-phase-function is the right first brick, and it is a
self-contained addition to machinery that already exists (heterogeneous media
transport in PT and VCM). §16 lists it as a non-goal, not a dead end.

### (E) Yarn-level geometry — **DEFERRED to Phase 3, likely declined**

§6.2's arithmetic. ~1 GiB per cushion face without a periodic instancing
primitive that does not exist.

### (F) Extend `coated_material`'s substrate allowlist to accept sheen — **REJECTED**

Adding `SheenMaterial` to the `dynamic_cast` list at
[CoatedMaterial.h:130-132](../src/Library/Materials/CoatedMaterial.h) is a
two-line change and it solves the wrong problem: it would let an author put a
**dielectric coat over sheen**, when what fabric needs is sheen over a base. It
also inverts the physical stack — sheen is the top layer in every production
formulation surveyed, and OpenPBR moved its fuzz slab to the *top* of the stack
specifically so it can sit over both base and coat [verified — OpenPBR spec
text]. Recorded because it is the obvious cheap idea and it is wrong.

### Scoring against [MATERIALS.md](MATERIALS.md) §9

| §9 requirement | (A) patched composite | **(B) `fabric_material`** | (C) Irawan | (D) SpongeCake |
|---|---|---|---|---|
| 1. Right base | ✗ — composite is not a layered BSDF | **✓ fresh triad, coated_material's shape** | ✓ | ✗ — it is a medium |
| 2. `value`/`valueNM`, `Scatter`/`ScatterNM`, `Pdf`/`PdfNM` | ✗ — no combined `value`, 50/50 `Pdf` | **✓ all six, closed form** | ✗ no sampler | ✓ claimed, unverified |
| 3. `GetSpecularInfo` if delta | n/a — no delta lobe | **n/a** | n/a | n/a |
| 4. `albedo` override for OIDN | inherits sheen's clamp | **✓ base albedo × scaling + sheen tint** | ✓ | ✓ |
| 5. `IsVolumetric` | no | **no** | no | **yes** — different integrator contract |
| 6. Parser + API + Job + 5 build projects | minimal | **full tax (§14)** | full | full + media plumbing |
| 7. Furnace + reciprocity test + regression scene | possible | **✓ one `add()` line + sweep entries** | possible | needs a new harness |

---

## 8. The recommendation

**Build `fabric_material` — option (B) — as the sibling of `coated_material`,
land it together with the UV-aligned tangent fix and the `make_fabric` verb, and
gate everything structured behind measured evidence.**

Concretely, Phase 1 is five things that must ship together because each is
useless alone:

1. **The tangent.** Meshes and analytic primitives write `vShadingTangent =
   dpdu` alongside their existing `derivatives.dpdu` write — three lines at each
   of eight call sites (§9.1). Without it, weave
   anisotropy rotates from an arbitrary base and the whole feature is
   incoherent — and GGX, Ward and Ashikhmin-Shirley get a correct anisotropy
   base as a side effect.
2. **The material.** `fabric_material` with a base-material reference, a Charlie
   top lobe, and a **Kulla-Conty** energy split between them (round 5; the
   earlier glTF-derived `min` form is withdrawn — §9.2) — a real closed-form
   `value()` so NEE and BDPT see both lobes, and a real mixture `Pdf`.
3. **The energy table.** A baked directional-albedo `E(α, cosθ)` and its
   hemispherical mean `Ē(α)`, supplying both the sheen lobe's own
   compensation and the base's scaling. Without it there is no principled
   split and the furnace posture cannot rise above `kPostureBounded`; with
   it, and with the product form, the Lambertian rows reach `kPosturePass`.
4. **The direction.** `weave_rotation` as an `IScalarPainter` angle field that
   rotates the basis `fabric_material` hands to its substrate's
   `value`/`Scatter`/`Pdf` — so anisotropic GGX, Ward and Ashikhmin-Shirley
   bases all follow the weave, and Ward and A-S gain a rotation input they do
   not have today. **The sheen lobe is isotropic; there is no `weave` enum and
   no anisotropic Charlie in Phase 1** (§9.5).
5. **The summons.** A `fabric` preset enum on the chunk and a
   zero-required-argument `make_fabric` verb, because **C-ADV** says the
   material alone converts approximately zero times.

Phase 2 buys cue (b) properly — structured, per-thread, with transmission — and
is gated on a named Phase-1 deficit plus a census. Phase 3 buys cue (d) and is
gated on a primitive that does not exist. §16 says so plainly rather than
promising it.

---

## 9. Phase 1 — the cheapest honest win with the widest coverage

### 9.1 The UV-aligned mesh tangent

**Change — but conditionally, and not with a bare `dpdu`.** The naive version of
this fix (write `vShadingTangent = dpdu` unconditionally wherever `dpdu` is
written) is wrong twice over. Both defects are visible in the same source the
fix touches.

**Defect 1 — `dpdu` is not always a UV tangent.** Both mesh sites compute a
`useUVJacobian` flag and fall back to the raw triangle edge `e1` when the mesh
has no texture coordinates or the UV triangle is degenerate:
`const bool useUVJacobian = ( fabs( uvDet ) > NEARZERO );` for the non-indexed
mesh ([TriangleMeshGeometrySpecializations.h:224-228](../src/Library/Geometry/TriangleMeshGeometrySpecializations.h),
fallback `dpdu = e1; dpdv = e2;` at [:238-243](../src/Library/Geometry/TriangleMeshGeometrySpecializations.h)),
and the indexed mesh additionally requires all three `pCoords` to exist
([TriangleMeshGeometryIndexedSpecializations.h:345-357](../src/Library/Geometry/TriangleMeshGeometryIndexedSpecializations.h),
fallback at [:372-376](../src/Library/Geometry/TriangleMeshGeometryIndexedSpecializations.h)).
`ri.derivatives.dpdu` is then written **unconditionally** either way
([:458](../src/Library/Geometry/TriangleMeshGeometryIndexedSpecializations.h),
[:306](../src/Library/Geometry/TriangleMeshGeometrySpecializations.h)) — correct
for its own consumers, which want *a* parameterisation, but fatal here: the
edge-frame fallback is **per-triangle discontinuous**, so an unconditional write
would hand every untextured mesh in the tree exactly the arbitrary,
discontinuous tangent this fix exists to remove — and it would do it via a path
that *claims* geometric coherence.

**Defect 2 — an authored glTF `TANGENT` must win.** The indexed mesh already
interpolates an authored tangent into `ri.vTangent` and sets `bHasTangent`
([TriangleMeshGeometryIndexedSpecializations.h:283-300](../src/Library/Geometry/TriangleMeshGeometryIndexedSpecializations.h)),
but `Object::IntersectRay` builds the shading ONB **only** from
`vShadingTangent` ([Object.cpp:699-772](../src/Library/Objects/Object.cpp)) — it
never consults `vTangent` for the basis. So a fix that wrote only `dpdu` would
leave the authored tangent unused for anisotropy, and §9.1's own mirrored-seam
remedy ("re-export with a `TANGENT` accessor") **would have no effect at all**.

**The write, therefore:**

```cpp
// Indexed mesh: authored TANGENT wins, UV Jacobian second, else nothing.
if( ri.bHasTangent ) {
    ri.bShadingTangentFromGeometry = true;
    ri.vShadingTangent             = ri.vTangent;   // object space
    ri.bHasShadingTangent          = true;
} else if( useUVJacobian ) {
    ri.bShadingTangentFromGeometry = true;
    ri.vShadingTangent             = dpdu;          // object space
    ri.bHasShadingTangent          = true;
}
// else: leave the flags alone -- Object::IntersectRay falls through to
// CreateFromW exactly as today, byte-identical.
```

The non-indexed mesh has no `TANGENT` path, so it carries the `useUVJacobian`
branch only.

**Both sources are object-space at this point, and both promote identically.**
`ri.vTangent` is interpolated from the per-vertex array before any transform, in
the same coordinate frame as `ri.vNormal`; `Object::IntersectRay` promotes
`vTangent` with the **forward** matrix at
[Object.cpp:792-794](../src/Library/Objects/Object.cpp) and `vShadingTangent`
with the same forward matrix in the ONB branch at
[Object.cpp:740-741](../src/Library/Objects/Object.cpp) — "a tangent transforms
like a position, NOT inverse-transpose". So feeding either into
`vShadingTangent` is frame-correct with no conversion.

**The "otherwise" case is the important guarantee.** When neither branch fires —
an untextured mesh, a degenerate UV triangle, a mesh with no `TANGENT` — nothing
is written, `bShadingTangentFromGeometry` stays false, and `Object::IntersectRay`
takes its existing `CreateFromW` path
([Object.cpp:772](../src/Library/Objects/Object.cpp)). Those hits are
**byte-identical to today**, which is what keeps §9.9 gate 1's bucket B finite.

**The analytic primitives** write `dpdu` from a genuine surface
parameterisation with no fallback branch, so they take the write unconditionally:
[SphereGeometry.cpp:159](../src/Library/Geometry/SphereGeometry.cpp),
`EllipsoidGeometry.cpp:181`, `TorusGeometry.cpp:164`, and
`CylinderGeometry.cpp` at **two separate sites**, `:382` and `:510`.

**One further site the earlier count missed: `clipped_plane_geometry`.**
`ClippedPlaneGeometry::IntersectRay` computes an analytic UV tangent —
`GeometricUtilities::BilinearTangentU(...)` at
[ClippedPlaneGeometry.cpp:151-154](../src/Library/Geometry/ClippedPlaneGeometry.cpp)
— and sets `ri.ptCoord = Point2(h.u, h.v)` at
[:190](../src/Library/Geometry/ClippedPlaneGeometry.cpp), but **never writes
`ri.derivatives` at all** (`grep -n derivatives` over that file returns only
comments). It is therefore a *new* write site rather than an augmented one, and
it matters disproportionately for this feature: curtains, banners, flags and
material swatches are exactly what a clipped plane is used for.

**Examined and deliberately NOT written: `bilinear_patch_geometry`.**
`BilinearPatchGeometry::ComputeSurfaceDerivatives` sets `sd.dpdu = onb.u()` from
`onb.CreateFromW( objSpaceNormal )`
([BilinearPatchGeometry.cpp:399-402](../src/Library/Geometry/BilinearPatchGeometry.cpp))
— that is **the arbitrary canonical-axis tangent itself**, not a UV tangent.
Writing it into `vShadingTangent` would buy exactly nothing (it reproduces what
`CreateFromW` already picks) while falsely advertising a coherent
geometry-supplied tangent to every consumer that checks the flag. It is listed
here so the omission reads as a decision rather than an oversight; it becomes a
site the day the patch gains a real UV parameterisation.

**Eight write sites, not one.** Two mesh specialisation headers (each carrying
the two-branch form above), five analytic-primitive sites (sphere, ellipsoid,
torus, cylinder ×2), and the new clipped-plane site: **≈ 30 lines across seven
files**, counting the mesh sites' extra branch. It is still a mechanical change
— the same few statements at sites that already have the vector in hand — but
"a four-line fix" would be wrong, and the aggregate is what §14 costs.

**Why the geometry level and not the material.** §4.3: `NormalMap::Modify`
preserves a geometry-supplied tangent through `CreateFromWU` and discards
anything else ([NormalMap.cpp:186-197](../src/Library/Modifiers/NormalMap.cpp)),
so only the geometry-level write survives normal mapping. And `Object`'s
derivative promotion happens *after* the ONB build (lines 832 vs 699-772), so an
`Object`-side fix would need reordering; the object-space write needs none.

**Blast radius — wider than the anisotropic materials, and this matters for how
the gate is written.** There are two distinct effects, and conflating them will
make the regression sweep unreadable.

*Effect 1 — a real appearance change, on anisotropic materials only.* The
tangent basis changes for **every anisotropic material on every UV-mapped mesh
and on the analytic primitives** — GGX with `alphaX ≠ alphaY`, Ward,
Ashikhmin-Shirley, and any authored `tangent_rotation`. Highlights rotate. This
is a *correctness improvement* that is nonetheless a visible change on existing
scenes, and each such change needs a justification.

*Effect 2 — a benign noise-pattern shift, on essentially everything else.* The
ONB's `u`/`v` axes are not read only by anisotropic lobes. **Isotropic sampling
reads them too**, because a sampled direction is built by transforming a
canonical-frame vector through the whole basis:
`GeometricUtilities::CreateDiffuseVector` ends in `uvw.Transform(...)`
([GeometricUtilities.cpp:85-97](../src/Library/Utilities/GeometricUtilities.cpp)),
and that function is what `LambertianSPF`, `OrenNayarSPF` and `SheenSPF`
([SheenSPF.cpp:73-74](../src/Library/Materials/SheenSPF.cpp)) all sample with.
The microfacet SPFs read `onb.u()`/`onb.v()` directly for their sampling frame
**whether or not `alphaX == alphaY`** (`GGXSPF.cpp`, `MicrofacetUtils.h`,
`WardAnisotropicEllipticalGaussianSPF.cpp`,
`AshikminShirleyAnisotropicPhongSPF.cpp`, `SchlickSPF.cpp`). So on every
UV-mapped mesh, **the RNG-draw → direction mapping changes for every diffuse and
glossy material**, and a pixel-level diff of a plain Lambertian scene will show
differences.

**The expectation is unchanged.** Cosine and microfacet sampling are
rotationally invariant about the normal, so rotating `u`/`v` in the tangent
plane re-labels which random draw produces which direction without altering the
distribution. Effect 2 is a *different noise realisation of the same estimator*,
not a bias. **But it means byte-identity is the wrong gate**, and a sweep that
demands "no unexplained pixel differences" would flag every Lambertian scene in
the tree. §9.9 gate 1 is written accordingly.

Two further consequences for the plan: the sweep must be scoped and typed as
§9.9 gate 1 specifies, and its
interaction with `SDFGeometry` heightfield mode's deliberate world-X coherence
(chosen so a shared `tangent_rotation` rotates from the same base on the SDF and
on the `cartesian_disk` mesh, [Object.cpp:691-694](../src/Library/Objects/Object.cpp))
must be checked — the mesh half of that pairing is exactly what this change
alters.

**Mirrored UV seams — the one real limitation, inherited knowingly.** A
`dpdu`-derived frame is coherent within a UV island and can flip chirality
across a *mirrored* seam. `NormalMap.cpp` names this failure mode in its own
comment at the very site this fix borrows from: using `dpdu`/`dpdv` is
"qualitatively correct on any connected UV chart — the only failure mode is
across mirrored UV seams, where authored TANGENT.w is the only signal that
recovers the chirality flip (and that signal is what's missing here)"
([NormalMap.cpp:109-120](../src/Library/Modifiers/NormalMap.cpp)). That matters
for fabric specifically, because mirrored islands are the normal way to UV a
symmetric asset — a cushion's two faces, a garment's left and right panels — so
a weave direction will be internally consistent per island and can mirror across
the seam between them.

`vShadingTangent` carries **no sign companion** to fix this: `vTangent` has
`bitangentSign` and gets it folded through the transform's determinant, but the
shading-tangent pair is just `Vector3 vShadingTangent; bool bHasShadingTangent;`
([RayIntersectionGeometric.h:296-299](../src/Library/Intersection/RayIntersectionGeometric.h)
vs [:344-345](../src/Library/Intersection/RayIntersectionGeometric.h)).

**Recommendation: accept it as a known limitation in Phase 1, exactly as
`NormalMap` already accepts it, and say so in the descriptor** — an asset whose
weave must cross a mirrored seam should be re-exported with a `TANGENT`
accessor, which is the same remedy `NormalMap`'s comment prescribes. It is a
*better* limitation than today's state, where the direction is discontinuous at
every triangle rather than at every mirrored island boundary. §15 debt 15
records it, and §9.9 gate 1 puts a mirrored-UV asset in the sweep so the
severity is measured rather than assumed.

**Scope discipline.** Do *not* also change `NormalMap`'s TBN, `bHasTangent`
handling, or the glTF `TANGENT` priority. The imported `TANGENT` accessor
remains the higher-authority signal where present, and `dpdu` is the fallback —
the same priority `NormalMap` already implements.

### 9.2 `fabric_material` — the material contract

The triad, checked against [MATERIALS.md](MATERIALS.md) §9 item by item.

**Base.** A **material reference** with a `dynamic_cast` allowlist mirroring
`coated_material`'s, which is **three C++ types**: `LambertianMaterial |
OrenNayarMaterial | GGXMaterial`
([CoatedMaterial.h:130-132](../src/Library/Materials/CoatedMaterial.h)).

**Do not add a fourth cast target for PBR — there is no such class.**
`coated_material`'s *user-facing* allowlist string names four scene keywords
("lambertian_material, orennayar_material, ggx_material,
pbr_metallic_roughness_material",
[CoatedMaterial.h:112-113](../src/Library/Materials/CoatedMaterial.h)), and it
is easy to mistake that for the cast list. It is not:
`pbr_metallic_roughness_material` is **not its own material class** — it is
resolved at scene-build time in `Job::AddPBRMetallicRoughnessMaterial` into a
painter graph plus a single `ggx_material` in `eFresnelSchlickF0` mode
([MATERIALS.md:247-250](MATERIALS.md)), so by the time
`IsSupportedSubstrate` runs it *is* a `GGXMaterial` and passes transitively.
`fabric_material` should reuse the same split: three `dynamic_cast`s, and a
four-keyword diagnostic string kept as a single source of truth the way
`SubstrateAllowlistText()` is.

Oren-Nayar is the natural fabric base — it is rough-diffuse, it already has a
closed-form `hemisphericalAlbedo` override
([OrenNayarBRDF.cpp:149-199](../src/Library/Materials/OrenNayarBRDF.cpp)), and
it is already on `coated_material`'s list. The transitive PBR route is what lets
glTF `KHR_materials_sheen` land on a PBR base.

**`value` / `valueNM`.** Closed form, summing both lobes with a
**Kulla-Conty multiple-bounce coupling** between the fuzz layer and the
substrate (`m = max3(sheenColor)`, `Ē = EMean(α)`):

```
f(l, v) = sheenColor · D_Charlie(α, n·h) · V_Charlie(α, n·l, n·v)
        + f_base(l, v) · scale(l, v)

               (1 − m·E(α, n·v)) · (1 − m·E(α, n·l))
scale(l, v) =  ─────────────────────────────────────
                          (1 − m·Ē(α))
```

**Round 5 replaced a `min` here with this product, on a measurement — read
the Amended block before changing it back.** The earlier text combined the
two arms with `min(…)`, following glTF's albedo-scaling law of §3.3 but
symmetrising it for reciprocity. That form is reciprocal and **cannot
conserve energy**: near normal incidence `E(v) → 0` while the `min` still
picks `1 − m·E(l)` for every `l`, so the base loses `Ē`'s worth of energy
that the sheen lobe never returns — measured at ρ = 0.863 at normal
incidence on a white Lambertian base at α = 0.5, against gate 3's required
1.000. glTF's own **one-arm** form `1 − m·E(α, n·v)` conserves energy
exactly and is **not reciprocal**. The two properties are in genuine
tension, and the product form is the one that holds both.

**What the denominator is.** `1/(1 − m·Ē)` is the sum of the
adding-doubling inter-reflection series between a lossless fuzz layer and
the base. Energy the fuzz intercepts on the way in is not deleted — it is
re-scattered onto the substrate, bounces, and some of it comes back out.
That is the physics the `min` was throwing away, and it is why the product
form is not merely a fitted patch. Two consequences to expect and not
mistake for bugs: `scale` **exceeds 1** away from grazing (its supremum is
`1/(1 − m·Ē)`, measured 1.073 at α = 0.08 rising to 1.427 at α = 1, with
the practically visible near-normal figure peaking around 1.10 at
α ≈ 0.2), and that brightening is exactly balanced by the darkening at
grazing — §9.9 gate 3's Lambertian rows landing on ρ = 1.000 is what
proves the balance rather than assuming it.

For a white Lambertian base at **any** `m` and **any** `α`:

```
ρ(v) = m·E(v) + (1 − m·E(v)) · (1 − m·Ē)/(1 − m·Ē) = 1        exactly
```

The base term calls straight through to the substrate's own `IBSDF::value`.
The scaling factor is symmetric in `l`/`v` by inspection — the numerator's
arms exchange under the swap and the denominator is direction-independent —
which is what preserves reciprocity, the same discipline the
`hemisphericalAlbedo` contract enforces for the coat
([IBSDF.h:82-135](../src/Library/Interfaces/IBSDF.h)). `valueNM` is the
identical algebra with `GuardedGetColorNM` on the tint and the substrate's
`valueNM` for the base; `E` is achromatic (roughness and geometry only) and
therefore does not disturb the spectral path — the same argument hair's
achromatic pdf proxy makes. **`m` is taken from the authored RGB `max3` in
both pipes**, deliberately: the scaling and the lobe-selection weight are
achromatic quantities, and deriving `m` per-wavelength would make the
mixture density wavelength-dependent and put `Scatter`'s stored hero pdf
out of step with a companion-wavelength `Pdf()` call.

**The energy bound needs TWO things, and round 6 found that out the hard
way.** Estevez & Kulla's Charlie+Λ fit is production-friendly, not
tightly energy-conserving: `E` exceeds 1 near grazing, where `1 − m·E`
would go negative and the base term would *subtract* energy. Round 5
claimed the roughness floor alone handled it. That was true of the
table's **cells** and false of the **lobe** — the cosθ axis was uniform,
so the whole grazing band was interpolated as a ramp from zero and the
real peak was invisible (Round-6 note above). Both halves are now in
place:

1. **The `E` table's cosθ axis is warped toward grazing**
   (μ_j = (j/(N−1))², ~6 nodes below μ = 0.03), so `E` is resolved where
   the lobe actually lives.
2. **A symmetric normaliser bounds what the floor cannot.** The resolved
   lobe still reaches `E` = 1.152 above the floor, inside μ < 0.03, so
   the sheen term is divided by `N(l,v) = max(1, E(v), E(l))` and the
   base is scaled by `Ê = min(E, 1)`. Symmetric in l/v — a `max` of two
   arms that exchange — so reciprocity is untouched, and both collapse
   to exactly 1 / exactly `E` outside the sliver, leaving everything
   there bit-identical.

**The roughness floor's criterion, explicitly:** `kMinSheenAlpha` is the
smallest α for which **max over μ ≥ 0.03 of `E(α, μ)` ≤ 1**. Not "over
all μ", which no α satisfies. `tools/SheenDirectionalAlbedoGen.cpp`
prints the scan on every bake; on the round-9 (2026-09-04) 64-node
table the smallest qualifying α is **0.037276** (was 0.028289 at the
retired 32-node table — the finer log-α grid puts a node closer to the
boundary, so the scan's answer moved even though the underlying `E(α,
μ)` surface did not), so **0.04 still stands**, with less margin than
before but with both bilinear-bracketing rows still under 1. Re-run
that scan after any change to `CharlieSheen`, to the bake extents, or
to the bake resolution rather than carrying the number forward on
trust.

**Exactness class, because it is not uniform over the hemisphere.**
Three regimes, measured on a white Lambertian base at m = 1 through the
shipped tables. **Re-measured for round 9 (2026-09-04) after the
32 → 64 uniform rebake** — the table below is post-rebake; the retired
32-node figures follow it:

| n·v | worst ρ (64-node) | where | worst ρ (retired 32-node) |
|---|---|---|---|
| ≥ 0.0349 | **1.0019** (+0.19 %) | α ≈ 0.95, at the boundary n·v = 0.0349 | 1.0064 (+0.64 %) at α ≈ 0.91 |
| μ₁ … 0.0349 (μ₁ = 1/3969) | **1.0075** (+0.75 %) | α ≈ 0.065, n·v ≈ 5.5e-4 | 1.0167 (+1.67 %) at α ≈ 0.91, n·v ≈ 0.0023 |
| < μ₁ | **1.0018** (+0.18 %) | α ≈ 0.95, n·v → μ₁ | 1.0067 (+0.67 %) at α ≈ 0.91, n·v → μ₁ (μ₁ was 1/961) |

Global maximum anywhere: **ρ = 1.0075**; nothing reaches 1.01.

**Doubling the table resolution roughly halved the outer and inner
bands, and RELOCATED the middle band's driver instead of halving it.**
Rounds 6–8 traced all three residuals to one mechanism: `E` at cosθ
node 1 is *concave* in α with a peak near α ≈ 0.9, so the log-α chord
across the last, widest cell (0.800 → 1.000 at 32 nodes) **under-reads**
the true lobe by up to 0.0067. Doubling the α axis halves that cell's
width, and the outer/inner bands — both still driven by that same
mechanism — dropped to +0.19 % / +0.18 % accordingly, comfortably under
a bare +0.2 %. **The middle band did not track that improvement**:
exhaustive search over the full (α, μ) domain (not the coarse per-band
spot checks rounds 6–8 used) finds its new worst at **α ≈ 0.065, close
to the roughness floor** — a location neither round 6, 7, nor 8 sampled
— not at α ≈ 0.9. Once the α ≈ 0.9 mechanism shrank below it, a second,
previously-smaller residual near the floor became the dominant one in
this band; it fell from +1.67 % to +0.75 % (a real 55 % reduction), but
by a different mechanism than the one debt 18 originally named, and it
does not clear the same +0.2 % bar the outer/inner bands now clear.
That residual is reported here rather than chased with a still-finer
table: closing it would need its own root-cause hunt (an α node near
the floor, or a different table warp there), not a blind resolution
escalation.

The outer/inner-band residual is the α-chord shortfall directly;
closing the middle band's *new* residual is an open question for a
future round — table resolution alone did not close it, only relocate
its cause.  Debt 18 tracks all of this.

**The bottom band is the FLOORED DOMAIN, and it is a third mechanism —
not the normaliser.** `SheenDirectionalAlbedo::E` floors its cosθ
argument at node 1 (μ₁ = 1/3969 since round 9, 2026-09-04; was 1/961 at
the retired 32-node table) and constant-extrapolates below.
Without that, the first cell interpolates E up from the analytically
exact 0 at μ = 0 across a region where the true lobe is at its peak, and
**the normaliser does not save it**: `N` is built from the tabled E, so
where `E_tab < 1 < E_true` it collapses to 1, the lobe is emitted
undivided *and* the base is barely suppressed — measured ρ = 1.714 (at
the 32-node table; unchanged mechanism, the round-7 fix). Constant
extrapolation is **sound but not a proof**, and the earlier text wrote
it as one. E_true is monotone increasing in μ on (0, μ₁), so
`E_true(μ) ≤ E_true(μ₁)` — what does *not* follow is
`E_tab(μ₁) ≥ E_true(μ)`, because `E_tab(μ₁)` is a linear interpolation
in log-α across a cell where E is concave in α. **Re-measured for round
9's 64-node table**: worst shortfall `E_true − E_tab(μ₁) = +0.0018` at
α ≈ 0.95 (was +0.0067 at α ≈ 0.90 at the retired 32-node table — the
doubled α axis roughly halves the last cell's chord error, as the outer
band above shows too). So ρ creeps to **1.0018** below μ₁ rather than
staying ≤ 1, decaying monotonically toward the true E → 0 limit at
μ = 1e-6. A bound, not a blow-up — but "ρ ≤ 1 below μ₁" was false as
written. At and above μ₁ the floor is a bit-identical no-op.

One more documented edge: for α ≲ 0.05 the fabric goes **exactly black**
below μ ≈ 1e-6, because `CharlieSheen::V` hard-returns 0 once
`n·l · n·v < 1e-6` while the floored `E(μ₁) ≥ 1` fully suppresses the
base. Energy loss, not gain, and a sub-pixel sliver — but a step to
zero, not a graceful tail.
`sheen_material`'s own 1e-3 floor is deliberately left alone (it
compensates nothing, and raising it would move every existing sheen
render); that asymmetry is recorded as debt, not an oversight.

**`Pdf` / `PdfNM`.** The real mixture: `w · pdf_sheen(wo) + (1 − w) ·
pdf_base(wo)`, evaluated for an arbitrary `wo` with no memory of how anything
was sampled. This is the line that `CompositeSPF` cannot deliver and that BDPT
and VCM require. `CoatedSPF::PdfImpl` is the exact shape to copy — it returns
`pCoat * qCoat + (1 - pCoat) * qBase`
([CoatedSPF.cpp:58-96](../src/Library/Materials/CoatedSPF.cpp)), including the
detail that it repeats the sampler's own geometric-horizon rejection so a
direction `Scatter` can no longer emit carries zero density.

**`Scatter` / `ScatterNM` — sample one lobe, then price the sample against the
FULL mixture.** Selection uses a **discrete weight equal to the sheen's
directional albedo**, `w_sheen = max3(sheenColor) · E(α, n·v)`, so the selection
probability tracks the energy split rather than being an arbitrary constant.
Selected sheen → **cosine-hemisphere sample**, exactly as `SheenSPF` does today
([SheenSPF.cpp:73-74](../src/Library/Materials/SheenSPF.cpp)); §9.4 explains why
D-importance sampling is deferred rather than adopted. Selected base → delegate
to the substrate's `Scatter`, **handing it the weave-rotated basis** of §9.5.

**Then, whichever branch produced `wo`, recompute the full mixture density at
that direction and use it for both the weight and the reported pdf:**

```
q       = Pdf( ri, wo, ior_stack )        // the two-term mixture, above
s.kray  = value( wo, ri ) * cos(wo) / q   // the closed-form SUM of both lobes
s.pdf   = q
```

**Do not divide `kray` by the selection probability alone.** That convention is
correct only for **delta** lobes, where `Pdf()` returns 0 by contract
([ISPF.h:194](../src/Library/Interfaces/ISPF.h): "For delta distributions
(perfect reflection/refraction), returns 0") and MIS routes around the density
entirely through `GetSpecularInfo` — which is why the 2026-05 path-tree
branching excision, whose subject was multi-lobe **delta** vertices at Fresnel
splits, could use it. **Neither fabric lobe is delta.** Charlie sheen and every
allowlisted substrate have full-hemisphere support, so a given `wo` is
generically reachable from *both* branches, and the density that `Scatter` must
report is the one it effectively sampled from — the whole mixture, not the
branch-local term. Reporting `w · pdf_sheen(wo)` for a sheen-branch sample would
disagree with an independent `Pdf(ri, wo)` call for that same `wo` whenever
`pdf_base(wo) > 0`, which is exactly the Scatter↔Pdf mismatch
[SPFPdfConsistencyTest.cpp](../tests/SPFPdfConsistencyTest.cpp) exists to catch
and that §9.9 gate 6 commits Phase 1 to passing.

`CoatedSPF` already does precisely this for its own continuum/continuum pair:
it computes `q = PdfImpl(ri, wo, nm, ior_stack)` for the sampled direction
([CoatedSPF.cpp:209](../src/Library/Materials/CoatedSPF.cpp)), then sets
`s.kray = pBRDF->value(sw, ri) * (scos / sq)` and `s.pdf = sq`
([CoatedSPF.cpp:253-257](../src/Library/Materials/CoatedSPF.cpp) for the
substrate branch, rewritten in place; [:268-272](../src/Library/Materials/CoatedSPF.cpp)
for the coat branch) — so `Pdf()` agrees with the carried pdf by construction.
Copy that, including its `q <= 1e-12` guard, which zeroes the **throughput** of
a degenerate sample but deliberately leaves the pdf alone, because a non-delta
ray with `pdf == 0` is a live 0/0 hazard in a downstream MIS denominator
([CoatedSPF.cpp:211-231](../src/Library/Materials/CoatedSPF.cpp)).

**Also replicate `SheenSPF`'s geometric-horizon gate**
([SheenSPF.cpp:86-99](../src/Library/Materials/SheenSPF.cpp)) — a `GlintModifier`
tilt can otherwise send a continuation ray into the solid — and mirror it in
`Pdf`, as `CoatedSPF::PdfImpl` does, so the sampler and the density agree about
which directions are reachable.

**`GetSpecularInfo`.** Not overridden. No lobe is delta; SMS correctly ignores
fabric entirely, on hair's precedent.

**`albedo` (OIDN AOV).** `base.albedo(ri) · scaling + sheenColor · E(α, n·v)` —
a genuinely noise-free estimate rather than sheen's current clamp-to-itself.

**`hemisphericalAlbedo` — a closed form after all, once the kernel factors.**
The contract wants a single direction-independent number: the
hemispherically-averaged directional-hemispherical reflectance, with
implementations forbidden from reading `ri.ray`
([IBSDF.h:82-106](../src/Library/Interfaces/IBSDF.h)).

**Round 5 made this simple, and retired a whole baked table.** Under the
earlier `min` kernel the base's contribution was
`∫∫ f_base(l,v) · min(1 − w(v), 1 − w(l)) dl dv`, and a `min` **does not
factor** into (a function of `l`) × (a function of `v`) — so that double
integral could only come out of a bake, which is what the `S(α, m)` table
was. The product kernel factors, and the integral closes in two lines:

```
∫∫ f_base(l,v) · (1 − m·E(v))(1 − m·E(l))/(1 − m·Ē) · (n·l)(n·v) dl dv
    = ρ_base · (1 − m·Ē)² / (1 − m·Ē)
    = ρ_base · (1 − m·Ē)
```

using `(1/π)∫ E(μ) μ dμ ≡ Ē` once per arm. Adding the sheen lobe's own
bihemispherical share gives **route 1**, which is now the only route:

```
out = substrate.hemisphericalAlbedo() · (1 − m·Ē(α))  +  sheenColor · Ē(α)
```

**`S(α, m)` is therefore retired** — `kSTable`,
`SheenDirectionalAlbedo::S()`, and the generator's `ComputeS` / `BakeS` /
`ClampedOneMinusME` are all removed. `E` and `EMean` are unchanged
bit-for-bit. **Route 1b** (the per-substrate-class 3-D table `S(α, m,
σ_base)`) was the remedy for a table that no longer exists, and **route 2**
(the `min(a,b) ≤ (a+b)/2` bound) was a fallback for a quantity now available
in closed form; both are retired with it. The
construction-time-quadrature route stays discarded for its own separate
reason — there is no `RayIntersectionGeometric` at material-construction
time, so it could only evaluate a substrate's *constant* parameters and
would be silently wrong wherever a painter varies, while presenting itself
as exact.

**Exactness class, stated precisely.** Pulling the substrate's own albedo
out of the joint integral treats `f_base` and the kernel as
**uncorrelated**, which requires `f_base` to be **constant** in the
integration variables. So route 1 is:

- **EXACT for a Lambertian substrate** — and now exact *in closed form*
  rather than up to an interpolated table's bake error.
- An **uncorrelated-response approximation** for Oren-Nayar (which couples
  `l` and `v` through `max/min(θ_l, θ_v)` and the azimuthal difference) and
  for GGX (which couples them through the half-vector). Azimuthal symmetry
  is *not* sufficient and is the wrong test.

**Measured (§9.9 gate 5b): ≤ 0.64 %,** against a pre-committed 5 % — and 0
by construction on the Lambertian control row. The doc's earlier toy
estimate of ~3 % was pessimistic.

**A second, larger error lives one layer down and is not ours.** What this
method returns also inherits whatever the *substrate's* own
`hemisphericalAlbedo` reports, and that is an approximation with a measured
size: `OrenNayarBRDF::hemisphericalAlbedo` returns `Rd` verbatim
([OrenNayarBRDF.cpp:148-190](../src/Library/Materials/OrenNayarBRDF.cpp)
documents it as 12.6 % high at roughness 0.5 and up to 25.6 % high at
roughness 1), and GGX's runs high at grazing. End-to-end against a
brute-force quadrature that reaches **15.7 %**, essentially all of it
attributable there. No change to `fabric_material` can fix it, and
`coated_material`'s recycling denominator already inherits the identical
debt — §15 debt 17 tracks it as its own item.

Getting route 1 right is what would let a future `coated_material` sit
*over* a `fabric_material` — a waxed canvas — since this is the quantity the
coat's recycling denominator consumes. §15 debt 16 tracks that composition.

**`IsVolumetric`.** False. No Beer-Lambert in `kray`.

**`ScattersFullSphere`.** **FORWARDED FROM THE SUBSTRATE since R8 P1.1**
(2026-09-04, §15 debt 22). The Charlie lobe itself is reflection-only, so this
material's transmission *is* its substrate's — modulated by the fuzz layer's
two-crossing attenuation, never created by it. It therefore returns
`base.ScattersFullSphere()`: false for every Phase-1 substrate and for a
`transmission none` weave (the committed Phase-1 answer, unchanged), true over a
`transmission thin` `weave_material`, where it inherits the full-sphere-NEE
machinery ([IMaterial.h:237](../src/Library/Interfaces/IMaterial.h)) that
recovered 6-8× on backlit hair.

The doc used to say "false in Phase 1 — Phase 2's transmission lobe flips it".
**That never happened**: P2-B flipped it on `weave_material` and nobody flipped
it here, so a sheen layer over a sheer curtain rendered the curtain 100 % opaque,
silently, for the whole of P2-B and P3. §15 debt 22 has the mechanism and the
numbers.

**`CouldLightPassThrough`.** Forwarded from the substrate for the same reason,
with the same before/after. It reaches `AutoRasterizer`'s Tier-1
transmissive-material signal and the GUI's x-ray view, both of which should see a
sheen-wrapped sheer curtain exactly as they see the bare one.

### 9.3 The chunk

```
fabric_material
{
	name			<string>
	fabric			<enum: cotton|denim|silk|satin|velvet|wool|linen|custom>   # preset seed, default custom
	base			<material ref>          # lambertian | orennayar | ggx (pbr_metallic_roughness resolves to ggx)
	sheen_color		<Color painter ref>     # the dye/fuzz tint; omitted = preset colour, else white
	sheen_roughness		<Scalar painter ref, requireSingle>
	weave_rotation		<Scalar painter ref, requireSingle>   # radians; rotates the frame handed to the SUBSTRATE (9.5)
}
```

Five slots, of which the no-argument case needs **one** (`base`), because
`fabric` seeds the rest. There is no `weave` enum and no `weave_anisotropy` —
§9.5 explains why the sheen lobe is strictly isotropic and anisotropy is the
substrate's job.

**An omitted `sheen_color` must be handled explicitly, not resolved.** RISE's
built-in `"none"` painter is **black**, so a slot that passes an unset name
through the painter manager binds black and switches the lobe *off* — the exact
trap `coated_material` already hit and documented for `coat_tint`: *"Resolving
it through the painter manager would bind the built-in 'none' painter, which is
BLACK — an opaque coat, the opposite of the intended default — so an owned white
painter is synthesised instead"*
([Job.cpp:3275-3289](../src/Library/Job.cpp)). `fabric_material` follows that
precedent exactly: an omitted or `none` `sheen_color` resolves to **the preset's
colour if the preset sets one, otherwise an owned uniform white painter**;
a named painter still resolves normally, so an author who genuinely wants a
black (disabled) sheen binds one explicitly.

**Preset seeding rule, and why it needs care.** `bag.GetString(name, default)`
cannot distinguish "the author omitted this" from "the author wrote the default
value" — but `ParseStateBag::Has()` exists
([ChunkDescriptor.h:316](../src/Library/Parsers/ChunkDescriptor.h)). The rule is
therefore expressible and must be stated in the descriptor: **`fabric <name>`
supplies the default for every slot the author did not write; any explicitly
written slot wins.** In `Finalize`:

```cpp
const FabricPreset& P = LookupFabricPreset( bag.GetString( "fabric", "custom" ) );
std::string rough = bag.Has( "sheen_roughness" )
                  ? bag.GetString( "sheen_roughness", P.sheenRoughness )
                  : P.sheenRoughness;
```

**A preset cannot configure the substrate, and pretending otherwise is the
trap.** `fabric_material` holds a *reference* to an already-constructed base
material; `Finalize` can neither retype it nor re-parameterise it. So
`fabric satin` bound over a Lambertian base yields **chalk with a faint sheen** —
the preset's whole point (a tight, directional, anisotropic silk highlight)
lives in a substrate the chunk cannot reach. Two consequences, both required:

- **The `fabric` enum seeds only `fabric_material`'s own slots.** The preset
  table below is therefore split into what the preset *sets* and what it can
  only *recommend*.
- **A warn-level mismatch diagnostic — logged in `Job::AddFabricMaterial`, not
  in `Finalize`.** The location is forced by the parser contract:
  `IAsciiChunkParser::Finalize( const ParseStateBag&, IJob& )` sees only the
  substrate's **name**, a string, and has no way to learn its runtime class. The
  type check has to happen during assembly, where the material manager has
  already resolved the name to a pointer — exactly where `coated_material` does
  it: `CoatedMaterial::IsSupportedSubstrate( *pBase, &why )` is called from
  [Job.cpp:3265](../src/Library/Job.cpp), inside `Job::AddCoatedMaterial`
  ([:3245](../src/Library/Job.cpp)), *after* `pMatManager->GetItem` has resolved
  `pBase`. **`Finalize` therefore only forwards the preset name through to
  `Job::AddFabricMaterial`**, which resolves the base, `dynamic_cast`s it, and
  logs once when the class does not match what the preset recommends:

  ```
  fabric_material `%s`: fabric `%s` expects a %s substrate for its
  characteristic highlight, but `base` is bound to %s `%s`; the sheen lobe
  will apply but the weave/anisotropy the preset implies will not.  See
  docs/CLOTH_FABRIC_DESIGN.md 9.3, or call make_fabric which mints a
  matching substrate.
  ```

  Warn, not error: the composition is legal and may be deliberate. Note the
  contrast with `coated_material`, which *errors* and refuses on an unsupported
  substrate ([Job.cpp:3266-3273](../src/Library/Job.cpp)) — there the substrate
  is outside the BSDF's evaluable set, whereas here it is merely not the one the
  preset was calibrated for.
- **`make_fabric` closes the gap by minting the substrate** — §9.7.

**What is actually new here, precisely.** RISE does have named values already:
`ParameterDescriptor::presets` offers per-parameter quick-picks in the editor
([ChunkDescriptor.h:449](../src/Library/Parsers/ChunkDescriptor.h); e.g.
`scene_unit`, `sensor_size`). What does not exist is **a name that seeds several
slots at once, resolved in `Finalize` rather than in the editor UI** — and no
material carries a preset of any kind. That multi-slot seeding is the untested
mechanism, and it is what §13's census measures. Every existing
`ValueKind::Enum` is a mode or algorithm selector (§1 gap 5). That is not a
reason not to do it — but it *is* a reason to say so, to price it, and to design
the census that tests whether it works (§13).

Indicative preset table (starting points to be tuned against reference
photography, not measurements). **The left group is what the enum actually sets;
the right group is what `make_fabric` mints and a hand-authored chunk must
supply itself:**

| `fabric` | **`fabric_material` slots (seeded)** | | **recommended substrate (verb-supplied)** | note |
|---|---|---|---|---|
| | `sheen_roughness` | sheen colour | class + parameters | |
| `cotton` | 0.55 | dye, untinted white default | `orennayar_material`, σ ≈ 0.4 | matte; isotropic base is correct |
| `linen` | 0.65 | dye | `orennayar_material`, σ ≈ 0.5 | coarser slub; pair with a `gabor3d_painter` breakup |
| `denim` | 0.45 | dye | `ggx_material`†, `alphax` ≈ 0.34 / `alphay` ≈ 0.22, steered by `weave_rotation` | the twill wale is the look, and it is **substrate** anisotropy |
| `wool` | 0.75 | dye | `orennayar_material`, σ ≈ 0.6 | broad, soft sheen; isotropic |
| `silk` | 0.20 | dye | `ggx_material`†, `alphax` ≈ 0.30 / `alphay` ≈ 0.10 | strongly directional; the anisotropy ratio *is* the fabric |
| `satin` | 0.12 | dye | `ggx_material`†, `alphax` ≈ 0.34 / `alphay` ≈ 0.06 | float direction dominates; the tightest ratio in the set |
| `velvet` | 0.08 | dye, dark | `lambertian_material`, dark | pure grazing halo; **isotropic on purpose** — velvet is a pile, not a weave |

**† A minted `ggx_material` substrate is not just `alphax`/`alphay`.**
`fresnel_mode` defaults to `conductor`
([ChunkParserRegistry.cpp:4189](../src/Library/Parsers/ChunkParserRegistry.cpp))
and `rs` ("Specular reflectance / F0",
[:4182](../src/Library/Parsers/ChunkParserRegistry.cpp)) is a required
Color-painter *reference* that cannot be written inline, so a base carrying only
roughness and `rd` renders with **no dielectric specular at all** — no highlight,
which is the whole point of choosing GGX for these three. The three rows marked
† therefore also require `fresnel_mode schlick_f0` and `rs` bound to a
dielectric F0 ≈ 0.04 painter. §9.7 step 1 specifies exactly what `make_fabric`
mints; **a hand-authored chunk must supply both itself**, and this is the single
most likely way to hand-author a silent black satin.

Two readings worth making explicit. **`velvet` wants no anisotropy at all** —
its look is a pile, and the isotropic Charlie lobe over a dark Lambertian is
exactly right; that the table can say so is itself an argument for presets over
raw knobs. And **`silk`/`satin`/`denim` differ from the others only in the
substrate**, which is precisely why the split-column form above is the honest
one: an author who binds those three presets over a Lambertian base gets the
diagnostic, not the fabric.

### 9.4 Sheen upgrade — Charlie + `E` table now, LTC gated

**Two candidates.**

| | **Charlie + directional-albedo `E`** | **LTC sheen (Zeltner 2022)** |
|---|---|---|
| Energy conservation | patched — bounded, compensated | **exact by construction** |
| Multiple scattering (cue c) | **no** | **yes** |
| Importance sampling | **cosine-hemisphere, retained** (D-sampling deferred — no VNDF, below-horizon leakage; see below) | **exact, LTC-native** |
| Art-facing parameters | 2 | 2 (drop-in) |
| New RISE infrastructure | one 2D bake + a lookup | **LTC eval/sample primitive + a fitted 3×3 matrix table + a reference volumetric fibre-slab path tracer to fit against** |
| Reuses shipped, furnace-tested code | **yes — `CharlieSheen.h` unchanged** | no — replaces it |
| Table provenance | RISE bakes it (medulla precedent) | vendor from `tizian/ltc-sheen` (license review) **or** build the reference tracer and fit |

**Recommendation: Charlie + `E` in Phase 1. LTC is the better model and is
gated.**

The reason is provenance, not merit. RISE's standing precedent is to **re-derive
rather than vendor**: the medulla table committed at
`HairMedullaProfile_LUTData.cpp` is produced by RISE's own Monte-Carlo bake in
`tools/HairMedullaProfileGen.cpp` specifically so no third-party research code or
data is vendored ([HAIR_FUR_DESIGN.md](HAIR_FUR_DESIGN.md) §11). Applying that
precedent to LTC means building a reference volumetric fibre-slab path tracer
*and* a per-cell nonlinear LTC fit — a real tool, not a weekend. Applying it to
`E(α, cosθ)` means a straightforward directional-albedo integral over the lobe
RISE already has, of exactly the shape `HairMedullaProfileGen` already performs,
validated by the furnace harness RISE already runs.

**The `E` table — and `Ê̄`, and nothing else.** A 2D bake over
(α, cos θ) on a **grazing-warped** cosθ axis (μ_j = (j/(N−1))², round 6 —
a uniform axis cannot resolve the lobe below μ = 0.0323 and broke the
energy identity by up to +1.05 absolute), plus the hemispherical mean of
the **clamped** lobe, `Ê̄(α) = 2∫min(E,1)·μ dμ` — the mean of the
quantity the code actually multiplies by, so `hemisphericalAlbedo`'s
closed form cannot disagree with `value()`'s own denominator. Shipped as
64 × 64 + 64 **floats** (16.25 KB payload; round 9, 2026-09-04, raised
both axes 32 → 64 to close part of debt 18 — was 4.125 KB, not the 8 KB
this paragraph originally estimated for doubles), still an order of
magnitude smaller than the 52.8 KB medulla table. `Ē` is computed from
the **stored** 64-point `E` row by the trapezoidal rule, not from an
independent higher-resolution integral, so it is exactly reconstructable
from the shipped table — one number, not two that could drift.

**Round 5 removed a third table.** The earlier text had this same generator
pass also bake `S(α, max3(sheenColor))`, the bihemispherical average of the
`min`-form scaling factor, because a `min` does not factor and §9.2's
`hemisphericalAlbedo` could not get that double integral from `E` alone.
The product form factors, `hemisphericalAlbedo` closes in `Ē`, and `S` is
retired — table, runtime lookup and generator code alike. `E` and `Ē` are
byte-identical across the change.

The generator writes its own `.cpp` with the bake extents and the
regeneration command stamped into the banner, exactly as the medulla
generator does. Its test is the one the medulla table already has in a
different form: assert the baked float count against the baked extents, and
assert the runtime struct's capacity against the baked bin count. **Determinism caveat (fix round, E1 P3):** the committed table is
byte-identical only re-run on the SAME toolchain/platform — the adaptive bake
calls `pow`/`exp`/`sqrt` through platform libm, which can differ in the last
ULP across compilers/platforms, so "byte-identical across re-runs" is a
same-toolchain claim, not a cross-platform one.

**Sampling — Phase 1 KEEPS cosine-hemisphere sampling.** An earlier draft
proposed replacing `SheenSPF`'s cosine sampling with D-importance sampling; that
proposal is **withdrawn**, because the obvious implementation is not merely
suboptimal, it fails the shipped pdf tests.

The standard microfacet recipe — sample `h ∝ D(h)·(n·h)`, then reflect `v` about
`h` — has no **visible-normal (VNDF)** variant for Charlie: there is no published
VNDF derivation for the exponentiated-sine distribution, and Charlie's mass sits
near *grazing* half-vectors by construction (that is the entire point of the
lobe). Reflecting `v` about such an `h` puts `wo` **below the horizon** for a
substantial fraction of draws at grazing incidence. Rejecting those draws is the
only available remedy, and rejection changes the density: the true pdf of an
accepted sample becomes `D(h)·(n·h) / (4·(v·h)) · 1/C(v)` with an acceptance
factor `C(v) < 1` that varies with the view direction and has no closed form.

A `Pdf()` that returns the un-normalised density then **fails two of the three
parts of the shipped consistency harness**:

- **Part 2** integrates `Pdf` over the hemisphere on a 100 × 200 grid and
  requires the result within `INTEGRAL_TOL = 0.05` of 1
  ([SPFPdfConsistencyTest.cpp:84](../tests/SPFPdfConsistencyTest.cpp) for the
  tolerance, [:297-326](../tests/SPFPdfConsistencyTest.cpp) for the integral and
  its `fabs(pdfIntegral - 1.0) > integralTol` check). An un-normalised density
  integrates to `C(v) < 1` and fails directly.
- **Part 3**'s chi-squared histogram
  ([SPFPdfConsistencyTest.cpp:329](../tests/SPFPdfConsistencyTest.cpp) onward,
  `CHI2_ALPHA = 0.001`) compares sampled direction frequencies against the
  reported density and would reject the mismatch independently.

Normalising by `C(v)` requires computing it, which is a second tabulated
quantity over `(α, cosθ)` — real work, and work whose correctness is exactly
what the tests above would have to prove.

**So Phase 1 retains cosine-hemisphere sampling, exactly as `SheenSPF` does
today** (`GeometricUtilities::CreateDiffuseVector`,
[SheenSPF.cpp:73-74](../src/Library/Materials/SheenSPF.cpp), with
`pdf = nDotL · 1/π` at [:116](../src/Library/Materials/SheenSPF.cpp)). It is
always below the horizon-safe, it integrates to 1 by construction, and its
density is exact — so §9.2's mixture `Pdf` is exact too. **MIS against NEE
carries the variance**: a sheen lobe is lit overwhelmingly by direct light at
grazing, which is the regime light sampling handles well, and the mixture pdf of
§9.2 gives MIS a correct weight to work with.

**D-sampling becomes a gated follow-up, not a Phase-1 item.** Its entry
condition is a *sampler/pdf pair proven against Part 2 and Part 3 first* —
whether by an analytic inverse or a tabulated inverse CDF plus a tabulated
`C(v)` normaliser. Note also that R5 reports Charlie's CDF in `sin θh` as
closed-form invertible but **its own verification summary does not list that
claim among the items confirmed against a primary source**, so treat the
analytic route as [from memory] and check the paper before attempting it. §9.9
gate 10 measures what the variance actually costs, which is what would justify
the follow-up.

**What Charlie + `E` does not buy: cue (c).** The albedo patch is a
single-scatter energy correction, not a multiple-scattering term. Fabric will
still have a slightly harder terminator than reference. **That is the named,
measurable deficit that gates LTC**, and §9.9 makes measuring it a Phase-1 exit
item rather than an assertion.

### 9.5 Weave direction — an angle field that steers the SUBSTRATE, not the sheen

**The sheen lobe is strictly isotropic in Phase 1. Weave anisotropy is delivered
by the substrate.** An earlier draft of this document proposed an *elliptical
Charlie* lobe — `α_along` stretched against `α_across` — and that proposal is
**withdrawn**. Four independent reasons, each checkable in source:

1. **Charlie's `D` has an isotropic-only normaliser.** It is an empirical
   exponentiated sine, `D = (2 + 1/α)·sin(θ_h)^(1/α) / 2π`
   ([CharlieSheen.h:39-50](../src/Library/Materials/CharlieSheen.h)); the
   `(2 + 1/α)` factor is what makes it integrate correctly over the sphere for
   a *single* α. Substituting a direction-dependent α does not carry that
   normalisation with it, and there is no published anisotropic form to borrow.
2. **The Λ visibility is a polynomial fit to the isotropic distribution.** It
   takes exactly `(α, cosθ)` and has **no azimuthal dependence at all**
   ([CharlieSheen.h:71-102](../src/Library/Materials/CharlieSheen.h)) — the
   Estevez & Kulla Table-1 coefficients were fit against the isotropic NDF.
   Feeding it an azimuth-dependent roughness silently uses the wrong masking
   function.
3. **A 2D `E(α, cosθ)` table cannot compensate a 4D anisotropic albedo.** The
   directional albedo of an elliptical lobe depends on the azimuth of `v`
   relative to the yarn as well as on `α` and `cosθ`, so the base scaling of
   §9.2 would mis-compensate **by azimuth** — brightening the base along the
   warp and darkening it across, or vice versa, with no way to tell from the
   table.
4. **The proposed fallback destroyed the feature.** The earlier text said that
   if the furnace showed `ρ > 1`, the anisotropic path would fall back to the
   isotropic lobe at the geometric-mean α — i.e. exactly whenever the feature
   was energetically wrong, it would silently stop being a feature.

**Every production system surveyed makes the same call**: glTF's
`KHR_materials_sheen`, Filament's cloth model and OpenPBR's fuzz slab all keep
sheen isotropic and put anisotropy in the base layer (§3.3, §3.8). RISE follows
them.

**The mechanism instead: `fabric_material` rotates the frame it hands the
substrate.** `weave_rotation` is a `ParameterPipe::Scalar` reference with
`requireSingle = true`, an angle in radians. `fabric_material` builds a rotated
copy of the hit's basis with the helper GGX already uses —
`MicrofacetUtils::RotateTangent(src, angle)`, which rotates `u`/`v` about `w`
and returns a new `OrthonormalBasis3D`
([MicrofacetUtils.h:50-61](../src/Library/Utilities/MicrofacetUtils.h)) — and
passes a `RayIntersectionGeometric` carrying that rotated `onb` into the
substrate's `value`, `Scatter` and `Pdf`. The sheen lobe itself ignores the
rotation entirely, because it is isotropic and the rotation is a no-op on it.

Three things fall out of that, and they are the whole argument for this design:

- **Any anisotropic base now follows the weave.** A `ggx_material` substrate
  with `alphax ≠ alphay` ([GGXBRDF.h:47-48](../src/Library/Materials/GGXBRDF.h))
  gets its elliptical lobe steered per-point by the painter — real, published,
  furnace-tested anisotropy instead of an invented one.
- **Ward and Ashikhmin-Shirley gain a rotation input they do not have today.**
  §4.3 established that GGX is the *only* anisotropic material in the tree with
  a rotation slot. Under a `fabric_material` wrapper, all three get one, because
  the rotation happens in the frame rather than in the lobe.
- **It composes with GGX's own `tangent_rotation`.** Both are rotations about
  the same `w`, so they simply add: the fabric's weave angle orients the yarn,
  and the substrate's own rotation stays available for a finer per-material
  offset. Composition order must be fixed and documented (weave first, then the
  substrate's own), and it is exactly reproducible because both go through the
  one shared helper.

**Implementation notes.** The rotated basis must be handed to **all three** of
the substrate's entry points identically — a `Scatter` that samples in a rotated
frame while `Pdf` evaluates in the unrotated one is precisely the Scatter↔Pdf
mismatch §9.2 exists to avoid. And the rotated `ri` must be a local copy;
`RayIntersectionGeometric` is the shared hit record and the scene is immutable
during the parallel pass.

**There is no `weave` enum in Phase 1.** The earlier draft proposed
`none|plain|twill|satin`, where the value picked "a fixed anisotropy ratio and a
default cell-phase interpretation" — both of which were properties of the
elliptical sheen lobe that no longer exists. With anisotropy delegated to the
substrate, the enum has nothing left to select: the *rate* of anisotropy is the
substrate's `alphax`/`alphay`, and the *pattern* is whatever the author's
`weave_rotation` field says (§5.3 gives plain, twill and satin cell formulas
that parse today). **The name `weave` is reserved, unused, for Phase 2's
structured model**, where a genuine two-yarn-family BSDF will have real
per-binding behaviour to select — and the descriptor should say so, so nobody
reads its absence as an oversight.

Phase 1 therefore does *not* introduce a pattern raster and does *not* claim
pattern-scale structure. That limit is measured, not assumed, by §9.9 gate 9b.
### 9.6 Presets — the `fabric` enum

Argued in §9.3 for the mechanism. The adoption argument:

**C-TYPE** says the failure mode is a slot-typing prior, not ignorance — models
copy examples but do not derive a parameter set. Asked for "a velvet cushion,"
no model is going to derive `sheen_roughness 0.08` + a dark Lambertian base +
`weave none`. It *will* write `fabric velvet`, because enum values surface
automatically in the right-click context menu and inline autocomplete in both GUI
scene editors and in the agent-facing schema (`src/Library/Parsers/README.md`:
"Populate `p.enumValues` for `ValueKind::Enum`"; a new chunk "automatically
appears in… the right-click context menu and inline autocomplete").

**The honest caveat: this is an untested hypothesis.** No document in the tree
states a named-preset adoption law, and there is no evidence anywhere that
presets-by-name have ever been *measured* against raw physical parameters for a
RISE material. Note the precise scope (§9.3): named per-parameter quick-picks do
exist in `ParameterDescriptor::presets`, but they are an editor affordance on
one scalar, they have never been censused, and nothing anywhere seeds several
slots from one name. The census in §13 is designed specifically to test the
multi-slot form, and its stop rule allows for the outcome that the enum is what
works and the verb is redundant — or the reverse.

### 9.7 The `make_fabric` verb

Template: `add_wear` / `add_wetness`
([AgentMcpAdapter.cpp:1865-1991](../src/Library/Agent/AgentMcpAdapter.cpp)),
which are hand-authored in two places — `AgentMcpAdapter.cpp` and
`AgentChatCodecs.cpp`'s `kToolDefs` — with the rule stated in the code itself:
*"two texts, one verb — a semantic change to either must land in both."*

**Name.** `make_fabric`, not `add_fabric`: this converts a material into a
fabric rather than adding a layer of weather to it, and the `add_*` prefix is
already load-bearing for the two weathering verbs that mutually lock each other
out.

**Two deliberate deviations from the template, both flagged.** The first is the
argument shape, below; the second is that `make_fabric` **mints a substrate**
rather than performing `add_wetness`'s pure wrap — see "What it emits". Both
precedent verbs
take exactly two properties — an optional `material` string and
`baseHeadVersion` ([AgentMcpAdapter.cpp:1871-1877](../src/Library/Agent/AgentMcpAdapter.cpp)
and [:1929-1936](../src/Library/Agent/AgentMcpAdapter.cpp)); **no enum-typed
argument exists on either.** `make_fabric`'s `fabric` argument is therefore a
new shape, not a copy, and everything else in this section (refusal style,
refusal style, return shape, commit-only registration, one-undo-step atomicity)
*is* a copy. The enum is chosen over a free string for the same C-TYPE reason the chunk's own
`fabric` slot is an enum: a closed, enumerated value list is what surfaces in
the tool schema and constrains the model toward a value that exists, where a
free string invites `"crushed burgundy velour"` and a refusal. The two must
carry the **same** value list, since the verb's argument is seeding the chunk's
slot.

**Arg schema.**

```
material  : OPTIONAL string. The material to convert. Omit it to take the most
            prominent flat-colour material bound to a non-planar object -- the
            no-argument call is the intended one.
fabric    : OPTIONAL enum { cotton denim silk satin velvet wool linen }.
            Selects BOTH the fabric_material slot values AND the substrate
            class/parameters the verb mints (9.3's split table) -- this is the
            one place a preset gets to configure the substrate, because the
            verb, unlike Finalize, can create chunks.
            Omit it to infer from the object's own name where that is
            unambiguous ("cushion" -> velvet is NOT inferable; "denim_jacket"
            -> denim is), else default to cotton and say so in the message.
weft_color : OPTIONAL string (round 9, reviewer P2.4). `match` binds the
            warp's own re-homed colour painter to the minted
            weave_material's `weft_color` too -- a uniform dye; or the NAME
            of an existing painter chunk. Meaningful only when the verb
            MINTS a weave substrate (denim / silk / satin); given on a preset
            that mints something else, or when the base already IS a weave
            and is reused, it is a refusal (make_fabric never edits an
            existing chunk -- the message names `weft_color` on that chunk
            as the route). Omitted: the weft keeps the preset's own dye and
            the message says so (round 8, P2.2).
baseHeadVersion : the standard optimistic-concurrency token.
required  : {}   -- NOTHING is required.
```

**What it emits — and here is the second, larger deviation from the template.**
`add_wetness` performs a *pure* non-destructive wrap: the original chunk stays
byte-for-byte untouched and a `coated_material` is minted around it. A pure wrap
is not sufficient here, for the reason §9.3 gives: **a preset cannot configure
the substrate.** Wrapping a Lambertian base in `fabric_material { fabric satin }`
produces chalk with a faint sheen — the tight, directional, anisotropic highlight
that *is* satin lives in a substrate the wrapper cannot reach. A verb that
shipped that would be worse than no verb, because it would look like it worked.

So `make_fabric` **mints the substrate when the bound base does not match the
preset's recommended class**:

0. **Find the original's colour painter, by trying three slot names in order.**
   The reflectance slot is named differently by predecessor class, and there is
   no common accessor: `reflectance` on `lambertian_material`
   ([ChunkParserRegistry.cpp:3276](../src/Library/Parsers/ChunkParserRegistry.cpp))
   and on `orennayar_material` ([:4335](../src/Library/Parsers/ChunkParserRegistry.cpp));
   `base_color` on `pbr_metallic_roughness_material`
   ([:4249](../src/Library/Parsers/ChunkParserRegistry.cpp), and it is
   `p.required = true` there); `rd` on `ggx_material`
   ([:4181](../src/Library/Parsers/ChunkParserRegistry.cpp)) and on the other
   `rd`/`rs` materials. The verb's CST inspection therefore looks up
   **`reflectance`, then `base_color`, then `rd`**, in that order, and takes the
   first present — the order is class-frequency, not preference, and any one of
   them yields the painter *name* to re-home. **If none is present, the verb
   refuses under Refusal 4** rather than minting a base with no colour: a fabric
   whose dye was silently dropped is worse than no fabric.
1. Mint `<name>_fabric_base` — a chunk of the preset's recommended class
   (`orennayar_material` or `ggx_material` per §9.3's table), carrying the
   preset's calibrated roughness / `alphax` / `alphay`, and **re-using the
   painter found in step 0** so the author's colour, texture or expression graph
   survives the conversion untouched.

   **A minted `ggx_material` needs three more fields, or it renders black.**
   This is the specific trap, and it is worth spelling out because the failure
   is silent. `ggx_material.fresnel_mode` is a `ValueKind::String` **defaulting
   to `conductor`** ([ChunkParserRegistry.cpp:4189](../src/Library/Parsers/ChunkParserRegistry.cpp)),
   with `ior`/`extinction` defaulting to conductor constants; and `rs` —
   "Specular reflectance / F0" — is a **required Color-painter reference**
   ([:4182](../src/Library/Parsers/ChunkParserRegistry.cpp)). A base minted with
   only `rd`/`alphax`/`alphay` would therefore get `rs` unset, which resolves to
   the built-in `none` painter — **black** — under *conductor* Fresnel: no
   dielectric specular at all, i.e. **no satin, silk or denim highlight**, which
   is the entire reason the substrate was minted. So a minted GGX base must
   carry:

   - **`fresnel_mode schlick_f0`** — cloth fibres are dielectrics, not metals.
   - **`rs` bound to a dielectric F0 of ≈ 0.04.** It cannot be written inline:
     unlike `alphax`/`alphay`, which go through `ResolveOrDiagnoseScalar` and
     accept an inline literal, `rd` and `rs` are resolved **by name only** —
     `IPainter* pRs = pPntManager->GetItem(specular);`
     ([Job.cpp:4243-4244](../src/Library/Job.cpp), inside
     `Job::AddGGXMaterial` at [:4224](../src/Library/Job.cpp)) — so
     `rs 0.04 0.04 0.04` would look up a painter with that name, find none, and
     fail the material outright. **The verb therefore mints a fourth chunk,
     `<name>_fabric_f0`, a `uniformcolor_painter` at `0.04 0.04 0.04` with
     `colorspace Rec709RGB_Linear`,** and binds `rs` to it.
   - **`alphax` / `alphay`** from the preset (§9.3's table), which *can* be
     inline scalars.

   An `orennayar_material` base needs no such treatment — `reflectance` plus
   `roughness` is its whole surface.
2. Mint the weave painters (`weave_rotation`, and the wear fields if the recipe
   calls for them) ahead of it.
3. Mint `<name>_fabric` — the `fabric_material` — with `<name>_fabric_base` as
   its `base`.
4. Move every bound object's `material` reference to the wrapper.

So a `fabric satin` conversion of a Lambertian mints **four** chunks in one
document swap — `<name>_fabric_f0`, `<name>_fabric_base`, the weave painter, and
`<name>_fabric` — plus the rebinding. A `fabric wool` conversion mints two (an
Oren-Nayar base needs no F0 painter, and a wool preset needs no weave rotation).

**The original chunk is still never edited** — that invariant holds — but it is
**no longer referenced** by anything after the swap, which `add_wetness`'s wrap
cannot say. Both facts belong in the verb's result message, because "your
material is still there, unedited, and nothing points at it any more" is a
different and more surprising outcome than "your material is still there and is
now the substrate." When the bound base *already* matches the preset's class,
the verb does the pure wrap instead and says so. Either way: **one `headVersion`
bump, one undo step**, one composite document swap.

**Refusals (each a no-op, never a partial edit).**

1. Nothing qualifies — no flat-readable material on a bound object.
2. The material is already a `fabric_material`.
3. All bound objects are planar and the requested preset needs curvature to
   read (the `curv ≡ 0` refusal `add_wear` already implements).
4. **The base cannot be converted into an allowlisted substrate** — refuse and
   *name the allowlist*. Minting a substrate does not make this refusal
   obsolete, it sharpens it: the verb can re-home a flat reflectance painter
   from a Lambertian, Oren-Nayar, GGX or PBR material onto a new base of the
   preset's class, but it must not silently discard the physics of a material
   that is something else. A **dielectric** (its IOR, dispersion and IOR-stack
   behaviour have no counterpart in a fabric substrate), an **emissive**
   material or luminaire, a **hair** material, a **BSSRDF/subsurface** material,
   an existing **coated** or **composite** stack — all refuse, naming what could
   not be carried across. **Also refuse when step 0 finds no colour painter** —
   a material carrying none of `reflectance` / `base_color` / `rd` has no dye to
   re-home, and minting a base without one would invent an appearance the author
   never authored.
5. Collision with `add_wetness`'s coat wrap on the same material. Unlike the
   `add_wear`/`add_wetness` mutual lockout, **wet fabric is a legitimate and
   commonly wanted composition** — a rain-soaked coat. Phase 1 refuses (it is
   the safe default, and the composition needs `coated_material` to accept
   `fabric_material` as a substrate, which is a separate slice), but §13's
   census counts how often a run wants both, which is the number that decides
   whether the composition is worth building.

**Return shape**, on the two shipped verbs' pattern: `{ok, applied, rawCode,
status, retriable, headVersion, message, material, materialKind, fabricPreset,
baseMaterial, mintedSubstrate, mintedSubstrateKind, substrateWasReused,
originalNowUnreferenced, weavePainter, rotationPainter, weftColorPainter,
rebindObjectCount, geometry, qualifying, objects}` — the three new fields
relative to the precedent verbs report the substrate decision, which is the
part of this verb's behaviour an author most needs to see; `weftColorPainter`
(round 9) is empty unless `weft_color` was given.

**Registration.** Mutating, commit-only (one composite whole-document swap is no
`AgentProposalKind` an Owner could approve card-by-card), across the eight
plumbing surfaces §14 enumerates.

**Read-set placement (C-READ).** Exactly one execution-validated parsing example
goes into `materials-and-media-basics.md` — one of the two proven pulls. The
full reference goes to the materials skill with a hook-line rewrite; a pointer,
not the content, goes anywhere else. The measured basis: `procedural-textures.md`
— the skill every advisory pointed at — was read **0/6** in one probe batch.

### 9.8 Worked example — a velvet cushion with seam wear

Modelled on `add_wear`'s recipe (§4.4) and on
[WETNESS_COAT_DESIGN.md](WETNESS_COAT_DESIGN.md) §6.5's presentation. The
camera, film, rasterizer and light preamble is elided; the material block is
what matters.

**Every painter chunk below parses today.** The `fabric_material` chunk is
**proposed syntax and does not parse today** — it is marked as such rather than
presented as a recipe.

```
RISE ASCII SCENE 7
# --- velvet cushion: worn nap on the seam ridges, dust in the piping groove ---
# Painters: PARSE TODAY.   fabric_material: PROPOSED (Phase 1).

uniformcolor_painter
{
	name			velvet_dye
	color			0.22 0.05 0.09
	colorspace		Rec709RGB_Linear
}

# The sheen tint: the dye lifted toward a desaturated bloom on worn ridges,
# pushed toward a dusty grey in the piping groove.  add_wear's prelude verbatim,
# with the material's own colour carried as base_r/g/b so it stays retunable.
expression_painter
{
	name			velvet_sheen_tint
	param			edge_wear     3.20  min 0 max 12 step 0.1  label "Seam nap wear"
	param			crevice_grime 2.80  min 0 max 12 step 0.1  label "Groove dust"
	param			breakup_amp   0.30  min 0 max 1  step 0.01 label "Noise breakup"
	param			breakup_scale 22.0  min 0.1 max 40 step 0.1 label "Wear noise scale"
	param			grime_scale   9.0   min 0.1 max 40 step 0.1 label "Dust noise scale"
	param			cavity_gain   1.40  min 0 max 4  step 0.05 label "Cavity deepening"
	param			base_r 0.62
	param			base_g 0.30
	param			base_b 0.36
	seed			11
	def			jitter       vec3(seed, seed*1.7, seed*2.3)
	def			wear_mask    clamp(curv*edge_wear + breakup_amp*fbm(P*breakup_scale + jitter, 4, 0.5, 2.0), 0, 1)
	def			crevice_raw  clamp(-curv*crevice_grime + breakup_amp*fbm(P*grime_scale + jitter, 4, 0.5, 2.0), 0, 1)
	def			cavity_boost 1.0 + cavity_gain*(1.0 - occlusion(0.08))
	def			crevice_mask clamp(crevice_raw*cavity_boost, 0, 1)
	def			base         vec3(base_r, base_g, base_b)
	def			bloom        vec3(0.86, 0.72, 0.74)
	def			dust         vec3(0.30, 0.27, 0.26)
	expr			mix( mix(base, bloom, wear_mask), dust, crevice_mask )
}

# Sheen roughness on the SAME masks: crushed and shinier where the nap is worn
# flat on a seam ridge, broader and duller where dust has collected.
scalar_painter
{
	name			velvet_sheen_rough
	param			edge_wear     3.20  min 0 max 12 step 0.1  label "Seam nap wear"
	param			crevice_grime 2.80  min 0 max 12 step 0.1  label "Groove dust"
	param			breakup_amp   0.30  min 0 max 1  step 0.01 label "Noise breakup"
	param			breakup_scale 22.0  min 0.1 max 40 step 0.1 label "Wear noise scale"
	param			grime_scale   9.0   min 0.1 max 40 step 0.1 label "Dust noise scale"
	param			cavity_gain   1.40  min 0 max 4  step 0.05 label "Cavity deepening"
	param			rough_pile    0.08  min 0.01 max 1 step 0.01 label "Undisturbed nap"
	param			rough_crushed 0.04  min 0.01 max 1 step 0.01 label "Crushed nap"
	param			rough_dusty   0.42  min 0.01 max 1 step 0.01 label "Dust-clogged nap"
	seed			11
	def			jitter       vec3(seed, seed*1.7, seed*2.3)
	def			wear_mask    clamp(curv*edge_wear + breakup_amp*fbm(P*breakup_scale + jitter, 4, 0.5, 2.0), 0, 1)
	def			crevice_raw  clamp(-curv*crevice_grime + breakup_amp*fbm(P*grime_scale + jitter, 4, 0.5, 2.0), 0, 1)
	def			cavity_boost 1.0 + cavity_gain*(1.0 - occlusion(0.08))
	def			crevice_mask clamp(crevice_raw*cavity_boost, 0, 1)
	expression		mix( mix(rough_pile, rough_crushed, wear_mask), rough_dusty, crevice_mask )
}

orennayar_material
{
	name			velvet_base
	reflectance		velvet_dye
	roughness		0.55
}

# PROPOSED CHUNK -- does not parse today.  `fabric velvet` seeds every slot the
# author did not write; the two painters below override what they name.
fabric_material
{
	name			m_velvet_cushion
	fabric			velvet
	base			velvet_base
	sheen_color		velvet_sheen_tint
	sheen_roughness		velvet_sheen_rough
}

standard_object
{
	name			o_cushion
	geometry		cushion_mesh
	material		m_velvet_cushion
	position		0 0 0
}
```

Three things this example demonstrates that are worth stating explicitly.

- **The prelude is byte-identical across the two painter chunks** — same names,
  same values, same order, same `seed`. That is a hard requirement, not a
  convention: the two masks must agree pointwise or the tint and the roughness
  will disagree about where the seam is. `add_wetness` §6.1 makes the same rule.
- **A seam is `curv > 0`.** The whole mechanism reduces to that one observation,
  which is why `add_wear`'s prelude transfers unchanged.
- **The `occlusion(0.08)` cavity boost is what distinguishes a piping groove
  from a merely concave curve** — a fold two bumps wide reads as a cavity to
  `occlusion` and as nothing much to `curv`. That is the split the signals design
  exists to provide.

For a **denim** variant, the same skeleton takes `fabric denim`, a
`weave_rotation` bound to §5.3's `weave_twill_3_1` through
`scalar_painter { function2d weave_twill_3_1 }`, and — the part that actually
makes it denim — swaps the isotropic `orennayar_material` base for a
`ggx_material` with `alphax ≈ 0.34`, `alphay ≈ 0.22`. The wale is **substrate**
anisotropy steered by the weave field (§9.5); the sheen lobe is the same
isotropic Charlie in both cases. That substrate swap is exactly what a
hand-authored chunk must do for itself and what `make_fabric` mints on the
author's behalf (§9.7) — and it is why §9.3's preset table has two columns.

### 9.9 Phase-1 exit gates

Gated in the [SMS_UNIFORM_SEEDING_PLAN.md](SMS_UNIFORM_SEEDING_PLAN.md) /
[HAIR_FUR_DESIGN.md](HAIR_FUR_DESIGN.md) style — build gate, baseline gate,
validation gate — with the
[implementation-review-loop](skills/implementation-review-loop.md) run to zero
P1 before the phase closes.

**Build gate.** All five build projects updated; clean-rebuild warning-free on
both `make` and the Xcode `RISE-GUI` target ([AGENTS.md](../AGENTS.md)).

**Baseline gate.**
1. **The tangent-change regression sweep — two buckets, two different tests.**
   §9.1's blast radius has two effects and the sweep must not conflate them.
   - **Bucket A — anisotropic scenes: a real appearance change, justified
     case by case.** Every scene using GGX with `alphaX ≠ alphaY`, Ward,
     Ashikhmin-Shirley, or an authored `tangent_rotation`, re-rendered and
     compared. Differences are *expected*; each must be justified as a
     correctness improvement, and an unexplained one blocks.
   - **Bucket B — every other scene containing a UV-mapped mesh or an analytic
     primitive: a noise-realisation change, tested statistically.** Because
     isotropic sampling also reads `onb.u()`/`onb.v()` (§9.1 effect 2), the
     per-pixel diff on a plain Lambertian, Oren-Nayar, sheen or isotropic-GGX
     scene will be non-zero and that is correct. **The gate here is statistical
     equivalence, not byte-identity: converge both builds well past the scene's
     usual sample count and require the per-channel means to agree within the
     MC standard error, with no systematic spatial structure in the residual**
     (a difference image that is zero-mean noise passes; one with a visible
     shape does not). Byte-identity must **not** be required in bucket B, and
     the sweep's tooling must say which bucket a scene is in before it reports.
   - **A mirrored-UV asset is in bucket A explicitly**, to measure the §9.1
     seam limitation rather than assume its severity.
   - The `SDFGeometry`-heightfield / `cartesian_disk` pairing
     ([Object.cpp:691-694](../src/Library/Objects/Object.cpp)) is re-checked
     explicitly; it is a bucket-A case.
2. No regression in the standing suites. **Note that any test asserting
   byte-identical or golden pixel values on a mesh scene is in bucket B and will
   need its oracle restated statistically** — identify these before starting,
   not after they go red.

**Validation gate.**
3. **Furnace.** New `LayeredWhiteFurnaceTest` configurations: `fabric_material`
   over Lambertian, over Oren-Nayar, over isotropic GGX-PBR, and over an
   **anisotropic** GGX base (`alphax ≠ alphay`) with a non-zero
   `weave_rotation`, at four roughnesses, **plus the bare-substrate reference
   rows the non-Lambertian predictions are built from** (the Oren-Nayar and
   anisotropic-GGX bases; the Lambertian and isotropic-GGX-PBR references
   already exist as configs 0 and 17).

   **Postures, per substrate — round 5.** The earlier text asked for
   `kPosturePass` at 5 % on every row. That is achievable for a Lambertian
   base and *only* for a Lambertian base, because a substrate that is not
   itself energy-neutral cannot become so by being wrapped:

   - **Lambertian rows: `kPosturePass` at 5 %.** The product form makes
     ρ = 1 an **identity** here at any α and any m, so these must land on
     1.000 and a deviation is a real regression. This row set is also what
     proves the `1/(1 − m·Ē)` denominator's near-normal brightening
     (§9.2) is **energy-neutral rather than a gain** — it is exactly
     balanced by the darkening at grazing, and ρ = 1 is the statement
     that the balance is exact.
   - **Oren-Nayar and GGX rows: `kPostureMatchesPrediction`** against a
     prediction derived *at run time* from the bare substrate's own
     measured curve, using the closed form the product model gives:
     `ρ_fabric(v) = E(α, cos v) + ρ_substrate(v)·(1 − E(α, cos v))`.
     That is neither a locked-in measurement nor circular — the substrate
     row is measured independently by the same driver, and the fabric row
     must then equal an analytic function of it. It is the precise
     statement of *the fabric layer neither adds nor removes energy over
     the substrate's own posture*: Oren-Nayar dissipates by its own
     design, and bare white GGX-PBR is already over unity at grazing
     (config 17 records 1.1555 at θ = 80). `eps` is 0.02 rather than the
     Lambertian rows' 0.002, covering route 1's uncorrelated-response
     residual, which gate 5b measures directly and at full precision.

   **The Lambertian rows carry a SECOND assertion: a per-angle
   closed-form cross-check.** An earlier revision of this text described
   one at "≤ 0.002" that had never been implemented — the number came
   from a hand cross-check during development and the shipped test
   asserted only the 5 % posture band (round 7, M5 review). It is
   implemented now:

   ```
   ρ(v) = ∫_H D(α,n·h)·V(α,n·l,n·v) / N(v,l) · (n·l) dl
        + (1 − m·Ê(v)) / (1 − m·Ê̄) · 2∫₀¹ (1 − m·Ê(μ_l))·μ_l dμ_l
   ```

   evaluated by deterministic 1024 × 512 quadrature and compared per
   angle against the furnace's Monte-Carlo measurement. It re-derives
   the product-form algebra from `CharlieSheen` and
   `SheenDirectionalAlbedo` **without calling `FabricBRDF`**, so a
   self-consistent error inside `ComputeTerms` / `BaseScaling` /
   `SheenNormaliser` cannot appear on both sides.

   **Tolerance 0.012, derived not fitted:** the furnace's 100k-sample MC
   error puts 1σ near ρ = 1 at ~3e-3, the quadrature's own error is two
   orders below that, and the table's interpolation residual *cancels*
   (both sides read the same `E`). 0.012 is 4σ. **Measured worst
   |measured − closed form| = 0.0024.**

   **What gate 3 asserts, in full**, since the tolerances above were
   previously misquoted in both this document and the test's own comment:

   - Lambertian rows: `kPosturePass` at **5 %** around ρ = 1, **and** the
     closed-form cross-check at **0.012**.
   - Oren-Nayar / GGX rows: `kPostureMatchesPrediction` at eps **0.01**
     (not the 0.02 an earlier draft stated).
   - Grazing check: **5 %**, conserving above μ = 0.03, bounded below.

   **THE PREDICTION MUST NOT CALL `FabricBRDF` (round 6).** The
   non-Lambertian rows originally built `pred[]` from
   `FabricBRDF::SheenTransmit` / `SheenTransmitMean` — the very statics
   `value()` uses — which made the oracle "Scatter agrees with `value()`'s
   own formula" rather than an independently rederived target: a
   self-consistent error inside those helpers, one that still preserved
   the Lambertian ρ = 1 identity, would have passed. The test now
   re-derives the factors from `SheenDirectionalAlbedo` alone, so the
   only surface shared with the model is the baked table — which has its
   own independent brute-force test against `CharlieSheen.h`.

   **A GRAZING CHECK, because the angle columns cannot reach the band
   where this class of defect lives (round 6).** `THETA_DEG` stops at
   80° (μ = 0.1736); the P1 that round 6 found lived below μ = 0.0323 and
   every row stayed green throughout it. Adding columns would mean
   re-measuring all nineteen pre-existing locked curves, so gate 3 gains
   a separate sweep over the **Lambertian** fabric rows — the ones whose
   expected answer is an identity rather than a locked number, and
   therefore the only ones checkable at a new angle without
   re-measurement — at θ ∈ {80, 85, 88, 89, **89.9, 89.99**}, with two
   postures matching the model's own exactness class:

   - μ ≥ 0.03 (θ ≤ 88.28°): **conserving**, |ρ − 1| ≤ 5 %.
   - μ < 0.03: **bounded**, 0 ≤ ρ ≤ 1 + 5 %.

   **89.9° and 89.99° were added in round 7** (μ = 1.7e-3 and 1.7e-4),
   because 89° sits at μ = 0.0175 — seventeen times the E table's first
   node — and the round-7 P1 lived below it. Measured after the fix:
   ρ = 1.007–1.009 at 89.9° and **0.901–0.918 at 89.99°**, the
   floored-domain regime. Before the fix that band reached **1.714**.

   The anisotropic row is a *substrate* configuration rather than an
   anisotropic sheen lobe (§9.5), so it should behave on the same terms as
   the isotropic ones; if it does not, the fault is in the frame-rotation
   plumbing, not in the lobe.
4. **Re-diagnose furnace config 6.** Run config 7's SPF-level downward-ray probe
   against the existing sheen-over-PBR composite and update the note with the
   measured answer, whichever way it comes out (§4.2, §15 debt 3).
5. **Reciprocity, plus the `hemisphericalAlbedo` error measurement — two
   distinct checks, both required.**
   (a) `fabric_material` **and, separately, bare `sheen_material`** added to
   `SPFBSDFConsistencyTest`'s reciprocity sweep at the existing
   `RECIPROCITY_TOL = 1e-6`. Sheen's absence is a pre-existing hole this phase
   closes as a matter of course.
   (b) **`hemisphericalAlbedo`'s substrate-coupling error, measured** — §9.2
   route 1's closed form `substrate.hemisphericalAlbedo() · (1 − m·Ê̄)`
   compared against a brute-force `∫∫ f_base(l,v)·scale(l,v) dl dv`
   quadrature, for an Oren-Nayar and a GGX substrate at several roughnesses
   and several `m`, with a Lambertian control row (where route 1 is exact by
   construction, so a non-zero reading there indicts the quadrature rather
   than the model). Pre-committed tolerance: **5 %**.

   **The measurement must be DECOMPOSED, because two independent errors land
   on the same number and only one of them is ours.** Round 5's result:

   - **fabric's own uncorrelated-response error: ≤ 0.64 %** (0 on the
     Lambertian control), well inside the pre-committed band and well under
     this doc's earlier ~3 % toy estimate. This is the asserted quantity.
   - **the substrate's own `hemisphericalAlbedo` error: up to 17.8 %**,
     carrying the end-to-end figure to 15.7 %. This is *inherited*, not
     introduced: `OrenNayarBRDF::hemisphericalAlbedo` returns `Rd` verbatim
     and GGX's estimate runs high. It is reported unasserted, and tracked as
     debt 17.

   Round 4's remedy for an exceedance — switching to a per-substrate-class 3D
   table `S(α, m, σ_base)` — is **obsolete**: the kernel table is gone, and a
   3D table could not have touched the substrate's own error anyway. Note
   this gate is *not* covered by (a): `hemisphericalAlbedo` is a
   diffuse-recycling term that the `f(a→b) == f(b→a)` sweep never evaluates.
6. **SPF↔BSDF and Pdf consistency.** `fabric_material` entries in both
   consistency suites, RGB and NM. **This gate is what proves §9.2's
   sample-then-reprice recipe was actually implemented**: it fails precisely
   when `Scatter` reports a branch-local density instead of the full mixture,
   which is the one mistake the delta-lobe convention would invite. Include
   sampled directions reachable from *both* lobes (an ordinary mid-hemisphere
   `wo` on an Oren-Nayar or GGX base), since a sheen-only direction would not
   discriminate.
7. **HWSS invariant — MEASURED 2026-09-03, GREEN.** `hwss=true ≡ hwss=false`
   within MC noise on a fabric scene, on the hair Phase-1 pattern.
   [tests/FabricRenderTest.cpp](../tests/FabricRenderTest.cpp), built on
   [tests/HairRenderTest.cpp](../tests/HairRenderTest.cpp)'s harness: real
   `RISE ASCII SCENE 7` text through the CST parser (so the CHUNK and its
   preset seeding are exercised, not just the C++ class), a real rasterizer,
   the output image inspected, `oidn_denoise FALSE` throughout.

   Two rows, because they run different halves of the material — an
   **anisotropic `ggx_material` with `weave_rotation 0.6`** (the frame-rotation
   path of §9.5) and an **`orennayar_material`** (no rotation to steer, so the
   sheen lobe's own spectral behaviour is isolated). Measured at 256 spp,
   32×32, `num_wavelengths 8`, `spectral_samples 1`, n = 5 runs at **seed bases
   1000 / 2000 / 3000 / 4000 / 5000**
   (`for b in 1000 2000 3000 4000 5000; do ./bin/tests/FabricRenderTest $b; done`):

   | row | `hwss=false` ≡ `hwss=true`, relative difference across the 5 bases |
   |---|---|
   | satin over anisotropic GGX | 0.38 / 0.38 / 0.48 / 0.72 / **0.74 %** |
   | cotton over Oren-Nayar | 0.58 / 0.60 / 0.74 / 0.80 / **0.90 %** |

   **On seeding — corrected 2026-09-03, and the correction matters.** An
   earlier revision justified those repeats as independent because "renders
   seed from the wall clock". That is **false for a test binary**:
   `srand( GetMilliseconds() )` is called only in
   `src/RISE/commandconsole.cpp`'s `main()`, and a test that calls
   `RISE_CreateJobPriv` directly never runs it. The only run-to-run variation
   was that every render worker constructs `RandomNumberGenerator random;`
   (default `seed = rand()`) concurrently from `ThreadPool::ParallelFor` — an
   **unsynchronised race on libc `rand()`'s global state**, which is undefined
   behaviour and can just as easily collapse to correlated draws on a
   single-worker machine or a different libc. That is not a basis for calling
   five runs independent MC samples. The test now takes an optional **seed base
   as `argv[1]`** and calls `std::srand( base + n )` before render `n`, so a
   default invocation is reproducible in intent and a sweep over bases is
   independent **by construction**. Bit-exactness within one base is *not*
   claimed — the worker-side race still perturbs which worker draws which seed,
   and closing that means changing how `RasterizeDispatchers` seeds its
   workers, a renderer change outside this gate. The table above is the
   re-derived post-correction spread.

   Asserted at **3 %**, ≈3.3× the worst observed run. That is an order of
   magnitude tighter than hair's own HWSS row (12 %), and the gap is
   informative rather than incidental: hair's residual is the pre-existing
   spectral-bundle bias acting on a strongly wavelength-dependent absorption
   model, whereas a fabric's dye is an ordinary reflectance and its sheen an
   achromatic lobe, so the bundle has almost nothing to disagree about. The
   residual that remains is MC noise — its sign is not even consistent between
   the two rows within a run.

   **The same file carries a PT-vs-BDPT check, and it had to be reframed to
   mean anything.** The raw numbers on this scene are PT 0.443, BDPT 0.557 —
   **+25.8 %** — which no defensible parity tolerance would accept. That is
   not a fabric defect: RISE's BDPT is a documented +28.5 % over closed-form
   truth on env-only scenes ([PT_ENV_MIS_DOUBLECOUNT.md](PT_ENV_MIS_DOUBLECOUNT.md)
   §4a, where `EnvLightBalanceTest` *bands* that bias rather than asserting
   parity), and every scene in the file is env-lit. The test therefore renders
   the scene **four** times — PT and BDPT over the bare `ggx_material`, and PT
   and BDPT over the `fabric_material` wrapping it — and asserts that the two
   **ratios** agree:

   | | BDPT/PT across the same 5 seed bases |
   |---|---|
   | bare `ggx_material` | 1.25115 / 1.25196 / 1.25220 / 1.25232 / 1.25278 |
   | satin over the same | 1.25694 / 1.25707 / 1.25741 / 1.25757 / 1.25854 |
   | \|difference\| | 0.00463 / 0.00487 / 0.00561 / 0.00579 / **0.00622** |

   So the wrapper moves the PT/BDPT relationship by at most **0.62 %** while
   the inherited env bias it sits on is 25 % — nearly two orders of magnitude
   between signal and confound. Asserted at 3 % (≈4.8× headroom); the raw
   ratios are printed but not asserted, so a future change in the env-MIS
   partition shows up in the log as a moving pair rather than as a mysterious
   failure. Unlike hair's twin this row needed no `indirect_clamp` to be
   assertable — there is no heavy-tailed transport in the scene — and unlike
   hair it can afford a tight tolerance, because `fabric_material` is asserted
   reciprocal to 1e-6 by gate 5a while the Chiang BCSDF is not reciprocal at
   all.
8. **Spectral parity.** An authored-white dye is bit-exact between the RGB and NM
   paths (the `GuardedGetColorNM` guard, exercised).
9. **The cue-(c) measurement — MEASURED 2026-09-03, RECORDED.** Render a
   backlit velvet swatch under the studio rig's rim light, with
   `oidn_denoise FALSE`, and record the terminator profile against a reference.
   **This number is the LTC gate.** Recording it, not passing it, is the gate —
   Phase 1 is allowed to be worse than LTC; it is not allowed to be worse than
   LTC by an unknown amount.

   Harness: [tools/fabric_sheen_measure.py](../tools/fabric_sheen_measure.py)
   `terminator`. Subject a **unit sphere** rather than the showcase drape,
   because the numbers are indexed by incidence angle and a sphere makes that
   mapping analytic: viewed near-orthographically down +Z, the visible point at
   normalized screen abscissa `s` has `N = (s, 0, sqrt(1-s²))`, so one scanline
   sweeps a whole angular range with no unprojection. Dye 0.20/0.05/0.09, sheen
   colour 0.90 white, **sheen α 0.08 (the `velvet` preset's own value)**;
   reference is the SAME sphere with the bare `lambertian_material` substrate,
   unwrapped. 1024 spp, 768×768, **32-bit EXR** (an LDR write would have put an
   ACES tone curve on the exact quantity being measured — `display_transform`
   defaults to `aces` for LDR formats), `oidn_denoise FALSE`.

   **Two configurations, and the first one's null result is the finding.**

   **(A) 90° rim — a true terminator.** Light at `direction 1 0 0`, exactly
   perpendicular to the view, so the terminator lands on the disc's vertical
   centre line and the scanline sweeps `θ_i` 0 → 90° while `θ_o` sweeps 90 → 0°.

   | θ_i | θ_o | fabric L | bare L | fabric / bare |
   |---:|---:|---:|---:|---:|
   | 20° | 70° | 2.2607e-02 | 2.5090e-02 | **0.901** |
   | 40° | 50° | 2.0638e-02 | 2.0297e-02 | **1.017** |
   | 60° | 30° | 1.2896e-02 | 1.3164e-02 | **0.980** |
   | 70° | 20° | 8.0003e-03 | 8.9774e-03 | **0.891** |
   | 80° | 10° | 3.2025e-03 | 4.5359e-03 | **0.706** |
   | 85° | 5° | 1.2461e-03 | 2.2588e-03 | **0.552** |
   | 88° | 2° | 3.6390e-04 | 8.8366e-04 | **0.412** |

   Falloff width, each profile normalized by its own value at θ_i = 20°
   (one pixel spans 0.151° of incidence at the terminator):

   | | 90 % at | 50 % at | 10 % at | 90→10 width |
   |---|---:|---:|---:|---:|
   | fabric | 41.00° | 63.33° | 82.26° | **41.25°** (273.7 px) |
   | bare Lambertian | 31.82° | 61.53° | 84.45° | **52.63°** (349.2 px) |

   **The sheen never rises above the bare substrate in this configuration, and
   at grazing incidence it is 2.4× DARKER.** The terminator is *sharper* than
   Lambert's, not softer — 41.25° of falloff against 52.63°. That is the exact
   opposite of cue (c): a real velvet's multiple scattering *fills* the
   terminator with a soft glow, and Phase 1 supplies none of it. The mechanism
   is not a bug and is worth stating, because it will be re-derived otherwise:
   on this scanline `θ_i + θ_o = 90°` identically, so the half vector never
   leaves the neighbourhood of 45° and Charlie's `sin(θ_h)^(1/α)` — with
   1/α = 12.5 at velvet's roughness — is nowhere near its peak. What the fabric
   *does* do is the base-energy subtraction, `1 − max3(sheen_color)·E(α, cosθ_o)`,
   which at near-normal VIEW is at its largest. Net: a small, uniform darkening.

   **(B) On-axis light — the grazing halo.** Light at `direction 0 0 1`, on the
   camera axis, where `θ_h = θ_i = θ_o = asin(r)` and the lobe's whole grazing
   ramp lays itself along the disc radius. The image is rotationally symmetric,
   so the profile is radially binned (0.5° bins) rather than read off a
   scanline — which buys back the resolution `ds/dθ = cos θ` destroys near the
   silhouette.

   | θ (= θ_i = θ_o = θ_h) | fabric L | bare L | fabric / bare |
   |---:|---:|---:|---:|
   | 20° | 2.6979e-02 | 2.5435e-02 | 1.06 |
   | 40° | 2.2727e-02 | 2.0950e-02 | 1.08 |
   | 60° | 4.5484e-02 | 1.4005e-02 | **3.25** |
   | 70° | 9.5600e-02 | 9.8411e-03 | **9.71** |
   | 80° | 1.6104e-01 | 5.3777e-03 | **29.9** |
   | 85° | 1.8478e-01 | 2.9296e-03 | **63.1** |
   | 88° | 1.2598e-01 | 1.3333e-03 | **94.5** |

   Halo onset — the smallest θ at which the fabric exceeds the bare substrate by
   each factor: **1.10× at 41.75°, 1.50× at 51.25°, 2.00× at 55.25°, 3.00× at
   59.25°**; peak **95.8×** at 88.25°.

   **What this says for the LTC decision.** The lobe is emphatically alive and
   its grazing ramp is enormous — 95× the substrate at 88° — so nothing is
   missing in *magnitude*. What is missing is *where it lives*: the halo needs
   BOTH the light and the eye near grazing (`cos θ_i + cos θ_o → 0` is the only
   way to reach `θ_h → 90°` with both directions in the upper hemisphere), so a
   single-scatter Charlie lobe contributes essentially nothing anywhere else,
   including across a terminator. **An LTC / multiple-scattering sheen would be
   expected to move configuration A, not configuration B** — it would fill the
   41–53° falloff band and lift the 0.41 grazing-incidence ratio toward and past
   1.0, while leaving B's already-large ramp roughly where it is. Phase 1's
   distance from LTC is therefore bounded and now numbered: **on the terminator
   it is a 2.4× deficit at θ_i = 88° and an 11.4° too-narrow falloff; on the
   grazing halo it is not behind at all.** A scene-authoring consequence falls
   straight out and is recorded in each showcase's header: light a fabric near
   the VIEW PLANE, not from three-quarters, or the sheen does nothing.
9b. **The cue-(b) measurement — MEASURED 2026-09-03, RECORDED. The verdict is
    BRUSHED METAL, and the reason is not the one this gate was written to
    suspect.** Render `fabric silk` and `fabric satin` on a draped subject under
    the studio rig, `oidn_denoise FALSE`, and record whether an isotropic
    Charlie sheen over an anisotropic GGX substrate reads as *satin* or as
    *brushed metal* — i.e. whether the visible deficit is the missing
    pattern-scale structure (cue b) or the missing multiple scattering (cue c,
    gate 9). §10.2's first Phase-2 gate condition consumes this directly.

    **Subject.** `scenes/FeatureBased/Materials/denim_and_satin_drape.RISEscene`
    with `oidn_denoise FALSE` (the showcase default is on; a denoised frame is
    not evidence about a highlight's structure, because the denoiser is free to
    smear exactly what is being judged), plus a variant with the left half's
    substrate swapped to silk's `alphax 0.30 / alphay 0.10`. Both at 1600×1000,
    256 spp, PT, hard grazing key + back rim + dim dome.

    **Re-measured 2026-09-03 after the weave-field correction, verdict
    UNCHANGED and if anything sharper.** The evidence frames were first rendered
    with cell-quantised weave fields, whose checkerboard was itself a visible
    artefact competing with the judgement being made. Both frames were
    regenerated from the corrected continuous-field scenes; the proxy numbers
    below are **bit-identical** across the two rounds, because the proxy builds
    its own sphere scenes with `weave_rotation 0.0` and never reads the showcase
    painters at all — so it was never contaminated, and the visual half is now
    made on a frame with no painted grid in it.

    **Visual judgement: brushed metal.** Both halves show a broad, soft,
    low-contrast highlight band running along the folds with a diffuse edge and
    a sheen wash filling everything else. Silk (α_y 0.10) and satin (α_y 0.06)
    are distinguishable but only just — the satin band is slightly tighter, not
    qualitatively different. What real satin does and this does not: a *tight,
    high-contrast, elongated float band that snaps on and off across a fold*,
    with near-specular contrast against an almost black surround. What is on
    screen instead is what an anisotropic microfacet lobe on a smooth curved
    surface always looks like, which is a brushed or lacquered one. **There is
    no pattern-scale structure of any kind**, and after the weave-field
    correction there is not even a painted one to mistake for it: what is left
    is a smooth anisotropic sheen on a smooth curved surface, which is what a
    brushed or lacquered object looks like.

    **The quantitative proxy, and the surprise in it.** Same harness,
    `aniso` subcommand: a sphere under a light 25° off the view axis, the
    substrate highlight's half-max extent measured ALONG the weave axis and
    ACROSS it over the inner disc (r < 0.65), plus the outer annulus
    (r ∈ [0.90, 0.995]) where the isotropic halo lives. 1024 spp, 768×768, EXR,
    `oidn_denoise FALSE`.

    | row | along (px) | across (px) | anisotropy ratio | rim annulus L |
    |---|---:|---:|---:|---:|
    | silk, bare `ggx_material` (0.30/0.10) | 158 | 51 | **3.098** | 1.663e-02 |
    | silk, `fabric_material` over the same | 153 | 51 | **3.000** | 7.044e-02 |
    | satin, bare `ggx_material` (0.34/0.06) | 182 | 31 | **5.871** | 1.673e-02 |
    | satin, `fabric_material` over the same | 180 | 31 | **5.806** | 6.990e-02 |

    **95 % (silk) and 99 % (satin) of the substrate's anisotropy survives the
    sheen.** That refutes the hypothesis this gate was framed around. The
    isotropic Charlie lobe is *not* washing the directional structure out — the
    §9.5 delegation works, and works almost losslessly. What the sheen does add
    is a **4.2× isotropic lift in the grazing annulus** on both presets, which
    is the only thing competing with the directional structure, and it competes
    only near the silhouette.

    **Therefore the Phase-2 entry condition is met on cue (b), and gate 9 says
    it is NOT met on cue (c).** The deficit is not amplitude, not the sheen
    swamping the substrate, and not multiple scattering: it is that a single
    elliptical GGX lobe with a painted rotation field has **no pattern scale**.
    Real satin's look comes from discrete floats, each a short cylindrical
    highlight with its own orientation and its own shadowing against its
    neighbours; a per-point rotation of one continuous lobe cannot produce
    them.

    **And a discrete cell field cannot be used to fake them — measured, not
    assumed.** The first pass at both showcase scenes built each weave from its
    real §5.3 cell formula (`floor(u*N)`, `mod(i + k*j, 5)`). It renders as a
    **blocky checkerboard**, because a cell field is piecewise constant and a
    lobe as narrow as satin's (`alphay 0.06`) is either lit or dark on each side
    of a cell boundary with nothing in between. Retuning the per-cell excursion
    from 0.42 rad down to 0.11 rad produced a *fainter checkerboard of the same
    size* — the discontinuity, not the amplitude, is the defect. Both scenes now
    use a constant base angle plus a low-amplitude continuous fbm drift
    (≤ 0.05 rad, `fw`-faded), which has no cell boundary at any zoom. The
    general rule is written up as
    [SCENE_CONVENTIONS.md](SCENE_CONVENTIONS.md) §8.7.

    So Phase 1 cannot reach cue (b) by authoring, only by modelling. **That is
    precisely the structured two-yarn-family model §10 proposes, and this is the
    evidence for it.**

    **RE-MEASURED 2026-09-03 AFTER PHASE 2 SLICE P2-A, on the same subject and
    the same harness.** The Phase-1 rows reproduce to every printed digit
    (silk 3.098 → 3.000, satin 5.871 → 5.806, rim annulus ×4.2 on both), which
    is the control that says the harness and the Phase-1 material are unchanged.
    The new half measures the deficit this gate actually named — pattern scale —
    because *anisotropy was never the deficit* and no anisotropy proxy can see
    the thing that was.

    **Proxy 3, and the confound stated before the numbers.** Relative RMS of the
    high-passed inner disc (9-px box high-pass, r < 0.65). High-frequency
    content is pattern OR Monte-Carlo noise, and the weave rows are 3–8× darker
    in the rim annulus than the Phase-1 rows, so at a fixed 2048 spp their
    *relative* noise is correspondingly higher and a raw cross-shape comparison
    would be measuring brightness. Each weave row is therefore compared against
    **its own minified control** — the same material at `weave_scale 4000`,
    where the draft falls below one pixel and the material's own footprint fade
    leaves nothing but the noise. Pattern and noise are independent, so they add
    in quadrature and the structure alone is `sqrt(rms_weave² − rms_control²)`.

    | preset | configuration | pattern rms |
    |---|---|---:|
    | silk | fabric / ggx (Phase 1) | 0.00488 |
    | silk | fabric / weave, scale 60 | 0.05544 |
    | silk | weave, scale 4000 (control) | 0.04313 |
    | satin | fabric / ggx (Phase 1) | 0.01277 |
    | satin | fabric / weave, scale 60 | 0.14523 |
    | satin | weave, scale 4000 (control) | 0.10030 |

    **Structure alone: silk 0.03483, satin 0.10503 — against a Phase-1 shape
    that has none.** The Phase-1 rows need no subtraction: they carry no
    periodic content at all, so their rms *is* their noise floor.

    **The visual half, which is this gate's primary instrument, changed
    verdict.** `denim_and_satin_drape.RISEscene` re-rendered with both halves on
    a `weave_material`: the left half now shows a diagonal twill wale running
    across the whole drape, and the right a tight, high-contrast float band that
    snaps on and off along each fold ridge over a fine grain — which is
    word-for-word what this gate's Phase-1 block described real satin as doing
    and Phase 1 as not doing. It does not alias into a checkerboard, because the
    coverage field is continuous and fades to the draft's own mean under
    minification.

    **One honest caveat about the table's other column.** The `aniso ratio`
    proxy is meaningless on a patterned disc — it takes the bounding box of
    everything above half-max, which on a weave is the yarn field rather than
    one highlight — so its weave-row values (0.05–0.36) say nothing about a
    lobe and must not be read as "the anisotropy vanished". The caveat is now
    printed by the harness itself; it was added after this run, so the recorded
    output above does not contain it.
10. **Variance — MEASURED 2026-09-03. §9.4's argument survives contact with a
    measurement; the normalised D-sampler is NOT justified.** PT sample counts
    to a fixed noise floor at α ∈ {0.08, 0.5, 1.0}, per
    [variance-measurement](skills/variance-measurement.md). §9.4 accepts
    cosine-hemisphere sampling on the argument that MIS against NEE carries the
    variance; **that is an argument, not a measurement**, and this gate turns it
    into one. A large residual at α = 0.08 is what would justify building the
    normalised D-sampler.

    **The requested NEE on/off A/B could not be run as written, and the
    substitute is stated rather than quietly swapped in.**
    `pathtracing_pel_rasterizer` **exposes no NEE toggle** — its descriptor is
    the base, pixel-filter, radiance-map, SMS, path-guiding, adaptive-sampling,
    stability, transparent-shadow, optimal-MIS and progressive parameter blocks,
    and none of them switches next-event estimation off (`choose_one_light` is a
    legacy no-op). The A/B is therefore run as two LIGHTING configurations,
    which is arguably the sharper experiment for what §9.4 actually claims:

    - **DELTA** — one directional light. NEE is the *only* route to it (BSDF
      sampling can never hit a delta light), so this row isolates the cosine
      sampler's cost in the **indirect** bounces alone.
    - **ENV** — a uniform radiance map, no directional light. Both routes live
      and MIS combines them, so the mismatch between a cosine-hemisphere
      proposal and a peaked Charlie lobe **is** exercised against the light.
      This is the row §9.4's sentence is about.

    K-trial protocol, K = 8 independent renders per cell at 64 spp — independent
    because each is a separate `bin/rise` **process**, and `bin/rise`'s own
    `main()` (`src/RISE/commandconsole.cpp:624`) calls
    `srand( GetMilliseconds() )` at startup, so K sequential launches start
    from K different clock states. (That is a property of the CLI entry point,
    not of the renderer — which is exactly why gate 7's in-process test binary
    cannot rely on it and sets its seed base explicitly instead.) Per-pixel
    standard deviation across trials, averaged over the lit disc and normalized
    by the per-pixel mean. Velvet sphere, 256×256, 32-bit EXR,
    `oidn_denoise FALSE`. Harness:
    [tools/fabric_sheen_measure.py](../tools/fabric_sheen_measure.py)
    `variance`.

    | config | sheen α | mean L | relative σ | spp for a 1 % relative-σ floor |
    |---|---:|---:|---:|---:|
    | DELTA | 0.08 | 1.5457e-02 | 0.00207 | **3** |
    | DELTA | 0.50 | 3.2021e-02 | 0.00223 | **3** |
    | DELTA | 1.00 | 4.1998e-02 | 0.00217 | **3** |
    | ENV | 0.08 | 1.3800e-01 | 0.03378 | **730** |
    | ENV | 0.50 | 2.6506e-01 | 0.03014 | **581** |
    | ENV | 1.00 | 3.2593e-01 | 0.02482 | **394** |

    (Sample count scales as 1/N, so the last column is
    `64 · (relative σ / 0.01)²`.)

    **Reading.** The DELTA rows are **flat in α to within their own noise** —
    3 spp at every roughness — which is the direct statement that a tight sheen
    lobe costs nothing when NEE is the route to the light, exactly as §9.4
    predicted. The ENV rows do show a residual, and its shape is the right one:
    **α = 0.08 costs 1.85× the samples of α = 1.00** (730 vs 394) to reach the
    same floor. That is the cosine-hemisphere proposal failing to track the
    lobe, and it is real.

    **1.85× is not "large".** The pre-committed trigger for building the
    normalised D-sampler was *a large residual at α = 0.08*; a factor under two,
    on the one lighting configuration where BSDF sampling has to do the work,
    against the several-fold speedups a bespoke sampler would have to earn back
    against its own implementation, tabulation and Scatter↔Pdf-consistency
    surface, does not clear it. **Recorded as: D-sampling follow-up NOT
    triggered.** The number to beat, if it is ever revisited, is 730 spp at
    α = 0.08 under a uniform env at a 1 % relative per-pixel noise floor.

    Two caveats on the numbers themselves, so they are not over-read. The
    absolute spp figures are specific to this subject, this film size and this
    1 % floor — a 256×256 sphere is a small, smooth, entirely non-adversarial
    scene, and the DELTA rows' 3 spp should be read as "below the measurement's
    own resolution", not as a recommendation. What transfers is the **ratio**
    across α within a configuration, and the qualitative gap between DELTA and
    ENV.
11. **Scenes — MET 2026-09-03, and exceeded.** `scenes/Tests/Materials/fabric_presets.RISEscene`
    (all seven presets on one row, the `sheen.RISEscene` shape), plus
    `scenes/FeatureBased/Materials/velvet_cushion.RISEscene` — §9.8, and the
    scene the skill text points at. Shipped alongside them:
    `scenes/Tests/Materials/anisotropic_uv_tangent.RISEscene` (§9.1's own
    bucket-A regression) and two more showcases,
    `scenes/FeatureBased/Materials/fabric_swatches.RISEscene` (the seven presets
    on draped swatches, matte to lustrous left to right) and
    `denim_and_satin_drape.RISEscene` (the hero, and gate 9b's subject). Both
    scene catalogues carry entries. Three authoring findings came out of
    building them and are recorded where an author will hit them:

    - **`curv`'s magnitude is a property of the SUBJECT, so §9.8's gains do not
      transfer.** `curv` is `curvR` × the hit geometry's world bounding-box
      diagonal, so on the cushion the flat panels already read 5–15 and §9.8's
      `edge_wear 3.20` saturates `clamp(curv·edge_wear, 0, 1)` over the entire
      convex body — the wear field then has nothing to pick out. 0.060 is the
      value that reproduces the picture §9.8 describes on the shipped geometry
      (0.035 on an earlier, flatter revision of it — which is the point: it is
      fitted to the subject, not to the recipe). The recipe is unchanged;
      the two gains are subject-fitted and stay `param`s. Recorded in the
      scene's own header.
    - **A cushion has to be flat-PANELLED for the wear field to work at all.**
      `curv` on a plain dome is large and positive everywhere. The shipped
      geometry is two flat-panelled superellipsoid halves welded around a thin
      welt disc — which is also what a real piped cushion is.
    - **Micro-relief belongs in a BUMP map, not a displacement.** `curv` comes
      from the geometric normal field and is invariant under bump/normal maps,
      so the nap creases add cloth relief without polluting the wear field a
      displacement would have speckled. A second, separate finding on the same
      subject: at showcase resolution a bump large enough to shape the gathers
      also reads as *waxy smearing* where the SDF's cylindrical UV converges at
      the pole. The fine pile texture belongs on `sheen_roughness` instead — a
      high-frequency fbm there breaks the lobe's response without touching the
      surface, and a smooth sheen over a smooth surface is exactly what makes a
      velvet look lacquered.
    - **A DISCRETE weave-cell field cannot drive an anisotropic lobe.** Both
      weave scenes were first authored from §5.3's real cell formulas and both
      rendered as checkerboards; the fix is a continuous field, and the rule is
      written up as [SCENE_CONVENTIONS.md](SCENE_CONVENTIONS.md) §8.7. See gate
      9b for the measurement.
    - **A velvet needs a DOME, and a LOW rim.** Under directional lights alone
      the cushion renders as dark glossy plastic — the look this material exists
      to replace. Gate 9's own numbers say why: the halo needs the light *and*
      the eye both near grazing, so only a dome supplies it everywhere on a
      curved silhouette at once, and a rim at 30–55° elevation lands `N·L ≈ 0.6`
      on a top-facing shoulder and produces no halo there at all. The shipped
      rim is nearly horizontal (`0.55 0.08 -0.83`), which puts `N·L` within 0.03
      of zero on exactly that shoulder.
12. **Verb.** `tests/AgentMakeFabricTest.cpp` on `AgentAddWearTest.cpp`'s
    pattern, covering all five refusals and the non-destructive-wrap invariant
    (the original chunk byte-identical, exactly one `headVersion` bump).
13. **Census.** §13's pre/post pair. The pre-verb baseline **must be run before
    the first Phase-1 commit** — a baseline measured after shipping is not a
    baseline.

---

## 10. Phase 2 — structured weave

**Goal.** Cue (b) properly: satin's diagonal float sheen, denim's wale, and — the
capability no reflection-only model has — thin cloth's backlit glow-through.

### 10.1 The read, and what was built — SLICE P2-A SHIPPED 2026-09-03

**The gate was a primary-source read, and it was done.** §10.2's third condition
required the equation set, the parameter table and a confirmed sampling strategy
in hand before committing, because the survey could confirm Zhu 2023's citation
and its comparison claims but not its maths. Zhu 2023 (main paper + its
supplementary document), Zhu 2024 and Sadeghi 2013 were fetched and read in
full. Three findings changed the plan.

**Finding 1 — Zhu 2023 alone has no directional importance sampler, confirmed
exhaustively.** Its only sampling discussion (§4.4.1) is *spatial*: one
stochastic point per shading evaluation inside the pixel footprint, with ray
differentials defining the footprint. That answers "which texel do I shade", not
"which outgoing direction do I sample", and nothing in the main paper or the
supplement addresses the latter. This is what §10.1's original text suspected
and it is now confirmed rather than suspected.

**Finding 2 — Zhu 2024 supplies the sampler, so no fallback was needed.** Its
§5.1 is a complete, shipped, closed-form lobe-selection sampler: compute an
attenuation `A(p)` per lobe, select with pmf `p_a = A(p)/Σ A(p)`,
importance-sample the selected lobe for `p_l`, return `w = f_p/(p_a·p_l)`. That
is exactly RISE's own multi-lobe convention. It is **adopted**, with one
addition RISE's harness forces: the sample is repriced against the FULL mixture
density rather than against the lobe that drew it, because these lobes' supports
overlap everywhere and a branch-local density would disagree with an independent
`Pdf()` call — the mismatch `SPFPdfConsistencyTest` exists to catch.

**Finding 3 — the shape came from Sadeghi, not from Zhu, and that was a
decision.** Zhu 2023's headline contribution is a per-texel **Anisotropic
Spherical Gaussian fit of the visibility function** (5 floats/texel, built by a
radial horizon search with online covariance fitting, §4.4.2). It is a bake
pass, a storage format and a fitting procedure — real infrastructure, not a
closed form — and its reflection lobe additionally needs an **SGGX microflake
distribution with a Smith Λ** and its VNDF sampler, of which RISE has none
(`grep -rni SGGX src/Library` is still empty). Sadeghi's cardinal-direction
masking (his Eq. 7–9) is a closed form that needs neither, and RISE already owns
better versions of both of his lobe factors. So the shipped model is
**Sadeghi-lineage lobe math on RISE's hair primitives, with Zhu 2024's sampler
on top**, and the ASG bake is deliberately NOT adopted.

**What shipped.** `weave_material` — a new triad
([WeaveMaterial.h](../src/Library/Materials/WeaveMaterial.h),
[WeaveBRDF](../src/Library/Materials/WeaveBRDF.h),
[WeaveSPF](../src/Library/Materials/WeaveSPF.h),
[WeavePresets.h](../src/Library/Materials/WeavePresets.h)) — rather than a
`weave` mode on `fabric_material`. §10.3's original text left that choice open
"after the read"; it was decided on the read's own evidence. Nothing in this
model is a sheen lobe, the two parameter sets are disjoint, and the fuzz layer
belongs ON TOP of the weave rather than beside it — so `fabric_material`'s
substrate allowlist was extended to accept `weave_material` instead, which is
the physical stack.

Two thread families, warp and weft, each with its own tangent, dye and pair of
lobes, mixed by a weave-draft coverage field:

```
f(i,o) = (1 − gap) · Σ_k a_k · M_k(i,o) · [ f_surf,k(i,o) + A_k · f_vol,k(i,o) ]
```

- **Surface lobe**, Sadeghi Eq. 2 with both factors upgraded:
  `F(η_k, cos θ_d · cos(φ_d/2)) · Mp(θ_i,θ_o; β_k²) · TrimmedLogistic(φ_d; γ_k)`.
  `Mp` is d'Eon's energy-conserving longitudinal lobe, **promoted out of
  `HairBSDF.cpp` into a shared [FibreLobeMath.h](../src/Library/Materials/FibreLobeMath.h)**;
  it replaces Sadeghi's plain Gaussian and is normalised at every width rather
  than only in the narrow-angle limit. The trimmed logistic replaces his bare
  `cos(φ_d/2)`, which has **no independently normalised closed-form sampler** —
  his own conclusion names importance sampling as open work — where RISE's has an
  exact CDF and an exact inverse. The lobe is **untinted**: a dielectric's
  specular reflection preserves the incident spectrum, which is one of the four
  places his §7 shows Irawan-Marschner failing.
- **Volume lobe**, his Eq. 3: `(1−F) · [(1−k_d)·Mp(θ_i,θ_o; (2β_k)²) + k_d] /
  ((cos θ_i + cos θ_o) · C_v)`. The second `Mp`'s width is **twice** the
  surface lobe's — Table II's `γ_v ≈ 2 γ_s` recurs in all six of its rows, so it
  is derived rather than authored, cutting a parameter.
- **Masking**, his Eq. 7–9 verbatim, with Ashikhmin's correlation blend at a
  fixed 20°. His Eq. 15 normaliser `Q` is **not** implemented and its absence is
  a decision: `Q` reads `ω_r` and nothing else, so a model carrying it cannot be
  reciprocal, and §9.9's reciprocity gate is non-negotiable. The gap it carries
  is expressed instead as a direction-independent energy factor.
  **CORRECTION (P2R1 review finding P2-3):** an earlier revision of this
  paragraph claimed his *reweighting* (Eq. 11–15) "degenerates to a constant"
  under P2-A's one-tangent-per-family simplification and was therefore absent
  rather than forgotten — that claim is **wrong** for a two-family weave. `Q`
  sums the one-direction quantity `P(t,ω_r)` over *both* families while the
  reweighted numerator carries the two-direction `P(t,ω_i,ω_r)` for the *one*
  family being evaluated; those cancel only for a single-family weave, which
  P2-A never is. Dropping `Q` is therefore a real energy loss concentrated at
  grazing views along a family's own axis (measured on the shipped satin
  preset: `Q ≈ 0.336` at 80° off the warp axis, i.e. a missing factor of
  ~3×). A reciprocal substitute, `Q_sym = √(Q(ω_i)·Q(ω_o))`, was tried and
  rejected: at the same grazing configuration `Q_sym` runs as low as 0.30–0.34
  (the geometric mean does not rescue two individually-small arms the way an
  arithmetic mean would), so `1/Q_sym` reaches ~3× and pushes white presets
  past the furnace's 1.05 ceiling. `Q` therefore stays out on the same
  reciprocity grounds as before, and the resulting grazing-only deficit is
  debt 7 below rather than a silently-wrong "cancels" claim.

**The draft is a closed form, not a baked map.** Zhu 2023 takes its
warp/weft ID map as an authored texture (§4.1). P2-A ships four analytic drafts
— `plain`, `twill_2_1`, `twill_3_1`, `satin_5`, with exact mean warp coverages
1/2, 2/3, 3/4, 4/5 — plus `custom`, which binds a `coverage` scalar painter and
is the escape hatch that recovers Zhu's authored-map route. The field is
**continuous**: yarn edges are smoothed ramps and the whole pattern fades to its
own mean as the pixel footprint outgrows the cell. That fade is not decoration.
Gate 9b's second finding is that a piecewise-constant cell field renders as a
blocky checkerboard and that lowering its amplitude only lowers the contrast of
the checks — the discontinuity is the defect — so the anti-aliasing had to be
intrinsic to the material rather than left to an author's expression.

**Energy is BOUNDED, not conserving, and the model says so.** Both lobes inherit
normalisations that bound the directional albedo by construction (`Mp`'s d'Eon
normalisation and the trimmed logistic's for the surface lobe; the volume
lobe's `C_v` is divided by the *exact* hemispherical integral of its bracket
at zero tilt, reciprocity-symmetrised across the two view latitudes), so no
baked table was needed — in contrast to Phase 1, which needed one because
Charlie's albedo has no closed form. It is not conserving for the reason the
source model states plainly: it ignores inter-thread multiple scattering, and
the masking term removes energy that nothing puts back.

**CORRECTION, and the one substantive bug this slice shipped with.** A first
cut used the *loose bound* `C_v = 2(1+k_d)` **as if it were the exact
normaliser** — it is 2.00–2.33× too large across the authorable `k_d` range,
and because the volume lobe carries ~90–96 % of a white weave's energy, that
one constant *was* the "every fabric renders 3–8× dark" defect a fresh review
found and this session fixed (`WeaveBRDF.cpp`'s `ComputeThreadTerms`; full
derivation in `WeaveBRDF.h` §2). It shipped past this document's own review
because the tests that could have caught it either measured `value()` against
itself at a fixed point in time (`LayeredWhiteFurnaceTest`'s locked curve,
which pinned the buggy numbers) or against a closed form
(`hemisphericalAlbedo`) that is *derived assuming `C_v` is exact* and so
agreed with the bug. The fix added a genuinely independent check —
`LayeredWhiteFurnaceTest`'s white-weave energy floor, a from-scratch
quadrature reimplementation compared against Monte-Carlo `value()` — see the
guard table below.

Measured on the four shipped presets with both dyes forced to white
(`WeaveTest::PresetWeave`'s `whiteDyes` flag), coverage at the draft's own
mean: directional albedo runs **0.65–0.76** at normal incidence and stays
**≥ 0.55** through 30° — comfortably inside Sadeghi's own measured 0.5–0.8
band for white fabrics, and the actual number `LayeredWhiteFurnaceTest`'s
independent floor check now guards rather than the stale "0.785 max" this
paragraph used to assert (that figure conflated the masking term's *own*
cosine-weighted hemispherical mean, π/4, with a directional-albedo bound it
does not derive).

**Transmission did NOT ship in P2-A.** `ScattersFullSphere` and
`CouldLightPassThrough` stayed false. Zhu 2023's delta-transmission term
`δ(i+o)/(i·n_s)` is the model for it and it was slice **P2-B**; claiming the
flags without the lobe would have sent the full-sphere NEE machinery hunting
for transmission that did not exist.

### 10.1a P2-B — thin-cloth transmission, SHIPPED 2026-09-03

**What shipped.** A `transmission` enum (`none`|`thin`, default `none`) and a
per-family `transmit` scalar on `weave_material`. `transmission none` is
bit-identical to the committed P2-A code by construction: every new
expression is reached only through a `p.thin` branch (`WeaveBRDF.cpp`,
`WeaveSPF.cpp`), never mixed into the original ones, and `1 - transmit`
multiplying the volume term is a no-op multiply by exactly `1.0` when
`!p.thin` regardless of what a `transmit` painter happens to hold. The
existing P2-A suite (`LayeredWhiteFurnaceTest`, `SPFBSDFConsistencyTest`,
`SPFPdfConsistencyTest`) passed unmodified against its own pre-existing
locked numbers after the change, which is the "value table" proof rather
than a separately-captured one.

Two new lobes, gated on `transmission thin`:

1. **Delta transmission through the gaps** (Zhu 2023 Eq. 2):
   `f_delta = gap(x) · δ(i+o) / (i·n_s)`. `gap` is the SAME field P2-A
   shipped — it keeps its P2-A energy role (`available = 1 - gap` darkens
   the reflect budget) and, only under `thin`, additionally supplies this
   lobe's aperture. The chunk accepts `sheer` as an alias for the identical
   slot — the more honest name once the field can transmit — with `sheer`
   winning if both are authored on the same chunk. The delta ray is chosen
   with probability `gap(x)` in `WeaveSPF`'s outer selection, reported
   `isDelta = true`, `pdf = 1` (RISE's delta marker, matching
   `DielectricSPF`), `kray = 1` (the lobe's coefficient and its selection
   probability are the same number and cancel exactly). `Pdf()` reports 0
   for it; the continuum density is scaled by `(1 - gap)` — the continuum's
   own share — matching `SPFPdfConsistencyTest`'s convention for a mixed
   delta+continuum SPF.
2. **Diffuse transmission through the yarn**, Lambertian:
   `f_t,d,k = (1 - gap) · transmit_k · T_k / π`, active only when `i` and
   `o` are on opposite sides of the shading normal. `T_k` reuses the
   family's own dye (`warp_color`/`weft_color` — Zhu's `T_k` and Sadeghi's
   `A_k` are the same "this family's colour" slot). `transmit_k`
   (`warp_transmit`/`weft_transmit`, preset defaults linen 0.25, silk 0.35,
   satin 0.15, denim/custom 0.0) is the family's share of its OWN volume
   budget redirected to transmission — the reflect-side volume lobe is
   scaled by `(1 - transmit_k)`, so the two together never exceed the
   pre-P2-B budget (**one budget, split**, not two budgets added). A
   specular (SGGX-like) transmission lobe (Zhu's `f_t,s`) is NOT in scope —
   deferred, matching P2-A's own SGGX deferral.

`WeaveMaterial::ScattersFullSphere()` and `CouldLightPassThrough()` both
return `true` iff `transmission thin`, reading the same
`WeaveBRDF::GetTransmission()` the BSDF/SPF gate their own lobes on, so the
flag and the lobes it advertises can never disagree. The generic full-sphere
NEE machinery `LightSampler.cpp` already had for `HairMaterial` needed no
changes.

**Presets.** linen/silk/satin default to `transmission thin` (linen keeps
its existing `gap 0.10`; silk/satin keep `gap 0.0`, so their transmission is
entirely the diffuse lobe, no delta glow-through). denim and `custom` stay
`transmission none`.

**Verified with three isolated probes** (window filling the frame, camera
facing it directly, EXR linear readback), the numbers behind both the
furnace prediction below and the showcase scene's light-level tuning:
(a) window alone, mean linear radiance **1.273240**; (b) the same window
behind a linen curtain at `transmission none`, mean **EXACTLY 0.0** (opaque,
correct); (c) the same again at `transmission thin`, `sheer 0.2`,
`warp_transmit`/`weft_transmit` 0.25, mean **0.259** — (c)/(a) = 20.3%,
matching the delta lobe's own closed-form prediction (`sheer × L_window`
= 0.2546) almost exactly, with a small further contribution from the
diffuse-transmission lobe.

**Guards, new this slice.**

| Guard | What it pins |
|---|---|
| `WeaveMaterialChunkTest::TestP2ABitIdentical` (R8 P2-1) | a LITERAL captured `(value, Pdf)` table — 24 (θ,φ) direction pairs × 2 presets (denim, satin) — verified against commit 8378266b by a full-file diff showing every P2-B change to the `!p.thin` path is structural (a `× 1.0` no-op), not numeric; asserted at ≤ 1e-12 relative. Replaces the earlier claim that the unmodified `LayeredWhiteFurnaceTest` locked curve was "the value table proof" — that table is Monte-Carlo `Scatter()` sampling at `kWeaveLockEps = 0.006` absolute (~4σ MC noise) and would not have caught a low-single-digit-percent regression (R8 P2-1 finding) |
| `SPFBSDFConsistencyTest` Part E2 | cross-hemisphere reciprocity for the transmission lobes: 225 direction pairs on a tilted, thin satin, 0 failures, exactly `0.000e+00` relative error. **Labelled STRUCTURAL, not empirical** (R8 P3-4): the diffuse-transmission lobe has no directional dependence beyond the hemisphere-crossing gate, and the harness's single fixed shading point means `f(a→b)`/`f(b→a)` are the same flat expression evaluated on identical inputs — a real regression tripwire for a future directional term, not evidence that today's flat lobe is "reciprocal" in any deeper sense |
| `LayeredWhiteFurnaceTest` rows 50-51 (R8 P2-2) | a sheer white linen's TOTAL (reflect+transmit) energy compared against an INDEPENDENT prediction — `WeaveIndependentCheck::PredictSheerTotal`, a from-scratch quadrature (reflect side, `(1-transmit_k)`-scaled) plus a closed-form Lambertian hemispherical-integral identity (transmit side, exact) plus the delta lobe's own exact `gap` contribution — at `≤ 0.01` absolute (measured agreement `≤ 0.0053` at every angle, N=100000), replacing the old self-referential "reflect-only twin minus transmit" bound that a transmission formula wrong by up to ~2x low would still have passed (R8 P2-2 finding); the delta lobe measured IN ISOLATION (summing `kray` only over `isDelta` rays) converges to `gap` exactly (measured 0.2028 vs 0.2, N=100000) |
| `SPFPdfConsistencyTest` (new block) | the continuum `Pdf()` integral over the FULL SPHERE for a thin material equals `(1 - gap)`, not 1 (measured 0.800025 vs expected 0.8) |
| `WeaveMaterialChunkTest::TestThinTransmission` | `transmission`/`sheer`/`warp_transmit`/`weft_transmit` parsing, the `sheer`-wins-over-`gap` alias rule (now diagnosed with a warning when BOTH are authored, R8 P3-3), preset defaults, `[0,1]` CLAMPING (descriptor wording fixed, R8 P3-6), and that the transmission lobe is exactly zero under `transmission none` / `transmit 0` and strictly positive otherwise, on both `value()` and `Pdf()` |
| `tests/FabricRenderTest.cpp::TestBacklitSheerCurtain` | a backlit curtain scene: `transmission none` renders EXACTLY black (opaque, no other light path); `transmission thin` glows (money assertion) |
| `tests/AutoRasterizerTest.cpp` (new case, R7 P2) | "thin weave + point light -> PT (not VCM)" — the exact `hasTransmissive && hasPositional` shape the shipped sheer-curtain scene has no longer routes to VCM |
| `scenes/FeatureBased/Materials/sheer_curtain.RISEscene` | the showcase render — reworked, verified against the three probes above, and rendered/inspected this session (see the scene's own header and `scenes/FeatureBased/README.md`) |

**Debt 20: BDPT/VCM over-count this material by 100-350x — RESOLVED
2026-09-04 (chip 4 / task_93ff4a8a).** Full reproduction, root cause and
disposition: §15 debt 20 (and
[RENDERING_INTEGRATORS.md](RENDERING_INTEGRATORS.md)'s known-limitations
section for the integrator-side reader). Summary: the "100-350x" measurement
does not reproduce once an unrelated, same-day fix (debt 21's
`RayBilinearPatchIntersection` scale-relative self-hit floor) corrects the
PT reference it was compared against — BDPT/PT and VCM/PT now settle at
0.90-0.93 on the original scene and 1.01 on a harsher touching-distance
stress scene, both stable and firefly-free. The mitigations shipped in the
P2-B round (`AutoRasterizer`'s Tier-1 routing exclusion and the one-time
BDPT/VCM warning) have been LIFTED/removed accordingly.

### 10.2 What Phase-1 evidence gates it

*Coverage addendum (2026-09-04, review follow-up):* the theoretical
exposure — `GeometricTerm` floors only below 1e-10 — was stressed with a
DOUBLED thin weave (two coincident `transmission thin` quads at 1e-4, 1e-3
and 1e-2 units, mesh area light behind, 256 spp): BDPT/PT 1.006–1.012 and
VCM/PT 0.944–0.950 at every separation, max/mean identical across the three
integrators, so the coincident front/back-vertex singularity does not
manifest in practice. The ~5–10 % BDPT/VCM-under-PT residual on delta-light
scenes stays an open, numbered follow-up guarded by the 20 %/15 % regression
bands in `FabricRenderTest`, not hidden by them.

Three conditions, all of which must hold:

1. **A named, measured deficit — produced by §9.9 exit-gate 9b, not assumed.
   SATISFIED 2026-09-03.** That gate commits Phase 1 to the side-by-side of
   `fabric silk` and `fabric satin`; this condition is satisfied when that
   comparison shows the isotropic-sheen-over-anisotropic-GGX composition reads
   as *brushed metal* rather than as *satin* — i.e. that the missing
   pattern-scale structure is the visible failure, not the missing multiple
   scattering. If gate 9b instead points at the terminator (gate 9's cue-(c)
   number), the correct Phase 2 is **LTC sheen**, which is a much smaller phase.

   **It read as brushed metal, and gate 9b's quantitative half rules out the
   obvious alternative explanation**: 95 % (silk) / 99 % (satin) of the
   substrate's highlight anisotropy survives being wrapped, so the isotropic
   sheen is *not* washing the directional structure out and §9.5's delegation is
   working almost losslessly. The deficit is the absence of a **pattern scale** —
   a single elliptical lobe with a painted rotation field cannot make discrete
   floats with their own orientation and mutual shadowing, and painting the
   rotation harder produces a checkerboard rather than cloth (measured, §9.9
   gate 9b). Note this does **not** discharge the LTC question: gate 9 measured a
   genuine cue-(c) deficit on the terminator (a 2.4× darkening at θ_i = 88° and a
   falloff 11.4° narrower than Lambert's). Both deficits are real; gate 9b's
   condition is about which one is the *visible* failure on a structured fabric,
   and on satin and silk it is this one.
2. **Demand.** §13's census showing fabric authoring actually happens at a rate
   that justifies the cost, and that authors reach for structured fabrics
   (denim, satin) rather than only for velvet and cotton.
3. **The read.** §10.1's primary-source summary in hand, with a tractable
   sampler identified.

### 10.3 What it cost, and what it is guarded by

**Cost, actual.** The §10.3 estimate was "of `coated_material`'s order or
larger — the honest precedent is commit `1f929fef`, 36 files changed, 4620
insertions — and at the upper end of that, because a transmission lobe touches
the NEE full-sphere path." Slice P2-A came in **below** that estimate, and the
reason is exactly the clause that did not apply: transmission was split out into
P2-B. What landed is the triad, the shared `FibreLobeMath.h` promotion, the
chunk, the API/IJob/Job plumbing, the editor introspection, the
`make_fabric` change, six test files (a seventh, `FibreLobeMathTest`,
added in the same-day fix round below) and three scenes.

**The promotion is worth naming separately.** `Mp`, `TrimmedLogistic`,
`SampleTrimmedLogistic` and `FrDielectric` (with their closed subgraph) moved
out of `HairBSDF.cpp`'s anonymous namespace into `FibreLobeMath.h`, and
`HairBSDF.cpp` now consumes that header through using-declarations so every call
site is textually unchanged. §3.2 predicted this was available; it was, and the
proof that it cost nothing is that `HairBSDFTest`'s output is **byte-identical**
across the move. `MakeGeom` and `ComputeAp` were deliberately NOT promoted:
they carry the fibre radius, the medulla geometry and the R/TT/TRT
multi-order split, none of which a woven thread has.

**What guards it.**

| Guard | What it pins |
|---|---|
| `LayeredWhiteFurnaceTest` 39–49 | energy: `kPostureBounded` per preset at two view sets, a **locked measured curve** (eps 0.006, re-measured this session after the `C_v` fix), the fabric-over-weave row, and an **independent white-weave energy floor** (row 49) -- a from-scratch quadrature reimplementation, sharing only `FibreLobeMath.h`, checked against Monte-Carlo `value()` and against a literal ≥0.55 physical floor at θ≤30° |
| `SPFBSDFConsistencyTest` | reciprocity at **~2e-15** (denim, non-zero weave rotation) and **~5e-15** (satin, non-zero opposite float tilts); `kray·pdf == value·cos` pointwise; MC-vs-quadrature furnace to 0.11–2.2 % |
| `SPFPdfConsistencyTest` | the four-lobe mixture density, RGB and NM, cross-validated exactly (~1e-14) with the hemisphere integral at **0.9986–1.00003**, plus an explicit check that the configuration can *discriminate* a branch-local density |
| `WeaveMaterialChunkTest` | the drafts enumerated exhaustively against their literal rationals; the footprint fade's exactness and the yarn edge's continuity; the preset table slot by slot; both `coverage` diagnostics; fabric-over-weave; the `hemisphericalAlbedo` error, measured; the masking-pole seam (a 0.25 deg sweep at `tilt = kMaxTilt`, ratio bound 1.10) |
| `HairBSDFTest` / `HairRenderTest` | the promotion changed nothing |
| `scenes/Tests/Materials/weave_presets.RISEscene` | the four drafts, legible |

**Debts, recorded rather than hidden.**

1. **`hemisphericalAlbedo` was a two-constant estimate; it no longer is, and
   the reason it changed is worth recording.** The original design fitted two
   "realised fraction" constants (surface 0.18, volume 0.30) to reconcile a
   loose analytic bound against `value()` — but `value()` was, at the time,
   2.0–2.3× dark from the `C_v` defect (debt-turned-fix, see the "Energy is
   BOUNDED" correction above), so the constants were silently retuning the
   *reported* albedo down until it agreed with the *wrong* render, and the
   test guarding them compared the fit against the very `value()` it was fit
   to — a tautology a fresh review caught (REVIEW_P2R3.md P1-2). With `C_v`
   corrected, `hemisphericalAlbedo` is now an EXACT closed form at normal
   incidence (`rho_vol(0) = A_k(1-F_k(1))·π/4`, `rho_surf(0) = F_k(1)·G(s_k)`,
   derivation at `WeaveBRDF.cpp`'s definition site) with no fitted constant at
   all. Its remaining inexactness is real but different in kind: `IBSDF`'s
   contract asks for a *bihemispherical* average and this form is exact only
   at normal incidence, pre-committed at **±40 % relative** for the
   view-dependence and tilt cases it does not track (`WeaveBRDF.cpp`'s
   `hemisphericalAlbedo` banner states the class before it was measured).
   `WeaveMaterialChunkTest`'s `TestHemisphericalAlbedoError` checks a **25 %**
   band against brute-force quadrature of the real `value()` on all four
   presets, all three channels — tighter than the pre-committed ±40 % class
   because it is now a legitimate independent check (the closed form is no
   longer fit to `value()`, so agreement between them is evidence rather
   than circularity) and because 25 % was set from the actual measured worst
   case (denim channel 0, 21.2 %) with a margin. The deviation is
   systematic and explained, not noise: the closed form is exact only at
   theta_o = 0, where `C_v` is at its minimum, and `C_v` GROWS toward
   grazing (the Chandrasekhar denominator relaxes), so the true
   bihemispherical integral runs below the theta=0 closed form on every
   preset -- including the untilted ones, which rules out tilt as the sole
   cause.
   Closing the residual view/tilt dependence means baking a directional-
   albedo table over (width, azimuth, k_d, η, tilt) — several axes against
   `fabric_material`'s two — and no shipping consumer needs it to better than
   this.
2. **The sampler wastes effort at a grazing view along a tilted yarn.** When the
   specular cone lands in the latitude band a float tilt has buried, the surface
   branch emits nothing: 51 % of draws on satin at θ_o = 80°, 19 % on silk, 0 %
   on the untilted presets and ≤ 0.1 % below 60°. It is variance, not bias — the
   density reports zero there too and the furnace confirms MC against quadrature
   — and recovering it would make the branch probability depend on the drawn
   latitude, which destroys the closed-form mixture density.
3. **One tangent per family, not Sadeghi's tangent CURVE.** His Table II spends
   up to 8 segments with individual lengths per family, and that is how he
   reproduces satin's three-highlight asymmetry. P2-A ships one scalar float
   tilt per family. The full curve is Phase-3 material.
4. **Cross-family masking is not modelled** — warp does not shadow weft. That is
   the source model's own scope ("we do not compute masking between different
   threads").
5. **`weave_scale` is the one slot no preset can get right**, because it depends
   on the geometry's UV scale and the framing. Every shipped scene authors it.
6. **The volume lobe is sampled by a cosine hemisphere, and it is not purely
   diffuse.** Its `(1 − k_d)` share carries a second `Mp` at twice the surface
   width — 5° for a flat satin float, a genuinely narrow forward cone by design
   — which a cosine proposal under-samples. Measured on a 2048-spp direct-lit
   sphere at the shipped satin preset, the peakiness ratio max/p99 over the lit
   disc is **1.5, identical to the Phase-1 shape's 1.5** on the same subject, so
   at the shipped parameters it produces no fireflies; a narrow lobe's energy
   arrives overwhelmingly through NEE, which evaluates `value` directly. A scene
   lit only by indirect bounces off that lobe is the case to re-measure. Fixing
   it means a three-term-per-family mixture density, and the estimator's
   tractability rests on that density staying closed-form and cheap.
7. **Sadeghi's Eq. 15 reweighting normaliser `Q` is dropped, and the loss is
   real and grazing-concentrated.** §10.1's masking paragraph above has the
   corrected argument: `Q` is not reciprocal (it reads `ω_r` alone), a
   reciprocal symmetrised substitute `Q_sym = √(Q(ω_i)Q(ω_o))` was tried and
   pushes white presets past the furnace's 1.05 ceiling at grazing (measured
   `Q_sym` as low as 0.30–0.34 there, i.e. `1/Q_sym ≈ 3×`), so `Q` stays out
   entirely. Concretely: at a view 80° off the warp axis on the shipped satin
   preset, `Q ≈ 0.336`, so the undivided form under-represents that
   configuration by a factor of ~3× relative to what Sadeghi's full model
   would return there. This is a genuine, measured, unrecovered energy loss,
   confined to grazing views closely aligned with a family's own tangent —
   not a general darkening, which is why it does not show up as a furnace
   failure (the harness's fixed incident set does not sit in that narrow
   configuration) but would show up as a slightly-too-dark silhouette on a
   fabric lit edge-on along its warp or weft. No fix is proposed; a reciprocal
   `Q` that does not blow through the energy ceiling would need a different
   derivation than the geometric-mean symmetrisation this file uses
   elsewhere, and the grazing-edge-lit case has not been reported as visually
   material.
8. **The research report's Eq. 3 transcription has a bracketing error; the
   code is not affected.** `P2_ZHU_READ.md` §"Volume lobe" transcribes
   Sadeghi's Eq. 3 with `k_d` alone over the Chandrasekhar denominator
   (`[(1-k_d)·g(γ_v,θ_h) + k_d/(cosθ_i+cosθ_r)]`); the paper's own text puts
   the *whole bracket* over the denominator, and that is what
   `WeaveBRDF.cpp`'s `ComputeThreadTerms` implements (matches §10.1's `f_vol`
   formula above). Recorded so a future reader who compares the code against
   the research report rather than against the paper does not "fix" a
   defect that exists only in the intermediate summary.

## 11. Phase 3 — bounded: fuzz shell verb; weave-resolving geometry declined

**Split verdict, evaluated 2026-09-02/03.** Phase 3 as originally scoped —
resolving the weave itself at yarn density — remains **declined**, on the
[HAIR_FUR_DESIGN.md](HAIR_FUR_DESIGN.md) Phase-4 precedent, for the exact
reasons below. But the cheapest partial answer this section used to only
*sketch* ("a fuzz shell... a thin offset surface... this is a sketch, not a
recommendation") was evaluated properly rather than left as a sketch, and
the evaluation reversed the sketch's own pessimism: it is not a new,
unphysical shading trick — it is the already-shipped `hair_geometry` /
`hair_material` machinery, grown sparse, and it produces a real, measured,
*geometric* silhouette effect no BSDF term can. That bounded item shipped
as the `add_fuzz` verb (docs below, this section's own history stays
intact above the fold).

### 11.1 What was declined, and why (unchanged from the original Phase 3 scope)

**What weave-resolving geometry would buy.** Cue (d) — genuine silhouette
fuzz — and the chunky-knit class where the loop silhouette *is* the
subject. Nothing else in this document supplies either.

**What blocks it.** §6.2's arithmetic: ≈ 1.03 GiB and ≈ 9.3 s of build for
one 40 cm cushion face at 30 threads/cm, extrapolated linearly from the
measured 100K-strand point. Woven fabric is periodic and should be
instanced, and **no periodic or instanced placement primitive at strand
density exists** — `path_instances_geometry` is capped at 20 M vertices /
100 K instances and is called structurally unusable at this scale in the
hair design itself; per-strand `standard_object` instancing is rejected
there too.

**What would make it tractable.** A `weave_geometry` primitive on
`HairGeometry`'s exact pattern — flat float control-point arrays, one
shared `BVH<>` over all yarn sub-segments, generated in a `Realize()` hook
from the base mesh's UVs — but storing **one weave repeat** and
referencing it periodically through the BVH rather than materialising
every crossing. That is new infrastructure with no precedent in the tree,
and it remains the thing to cost before anything else in this scope.

**Decline criteria, pre-committed and unchanged.** If the Phase-1 and
Phase-2 censuses show no requests for knitwear, rope, or hero fabric
close-ups, weave-resolving geometry stays declined — exactly as
HAIR_FUR's Phase 4 does. No such request has been recorded as of this
evaluation.

### 11.2 The fuzz-shell evaluation (2026-09-02/03)

The experiment (full writeup: the P3 evaluation this section summarizes)
used RISE's *shipped* `hair_geometry`/`hair_material` machinery, grown
sparse over a `fabric_material` wool sphere (0.15 scene-unit radius, 24000
strands, 3.5mm length, 0.06/0.02mm root/tip width — no comb/clump/gravity/
curl, the cheapest possible groom recipe), rim-lit near the camera's own
view axis so a single directional light's terminator coincides with the
camera-visible limb.

**Finding: no shipped-chunk obstacle.** `hair_geometry` bound to a plain
`sphere_geometry` base and a `fabric_material`-wrapped `standard_object`
sharing that base worked on the first try. The same is expected (not yet
tested end-to-end) on the actual Phase-1/2 drape swatch geometry
(`clippedplane_geometry` + `displaced_geometry`), since `hair_geometry`'s
own descriptor names a `displaced_geometry` base as accepted.

**Finding: the effect is real and geometric, not a shading illusion.**
Measured on 512-spp 32-bit EXR renders, comparing the bare material
against the fuzzed one over 21 columns crossing the rim-lit limb:

- The fuzzed render's visible material extends a mean of **3.7 px past**
  the bare render's analytic silhouette edge — categorically impossible
  for any BSDF term, since the ray-traced sphere boundary is exact
  regardless of shading model.
- The fuzzed edge's luminance falloff is **14.6× more non-monotonic**
  (more sign changes in the first difference) than the bare render's
  smooth falloff — the signature of discrete strand hits rather than a
  continuous shading gradient.

**Finding: the body is not degraded** at the tested density (24,000
strands over 0.283 m², ≈ 85,000 strands/m²) — no visible fur texture away
from the silhouette beyond ordinary Monte-Carlo noise.

**Finding: cost at fuzz-shell density is three orders of magnitude
cheaper than resolving the weave.** §6.2's reference case (2400 threads ×
~4800 CPs/thread ≈ 11.5M control points, ≈ 1.0-1.6 GiB, ≈ 9.3 s build) is
about *resolving the weave itself* — a completely different regime from a
*sparse fuzz shell*, which only needs silhouette-scale fibre density. The
same 85,000 strands/m² density scaled to a 0.16 m² cushion face is only
≈ 13,600 strands (≈ 54,400 control points, ≈ 7.6 MiB by this experiment's
own measured 140.5 bytes/CP) — no new infrastructure, no periodic-
instancing primitive needed, because a fuzz shell never has to resolve a
single yarn crossing.

**Caveat, stated rather than hidden, and resolved in the showcase.** The
tested rig's near-camera-axis rim light does not, on its own, produce a
bright "glowing" halo — it proves the geometric silhouette effect exists
and is measurable, but a genuinely backlit, glowing fringe needs a true
rim/back light and a background the fringe can read against (the
evaluation's own `D_brightbg` probe: a fuzz shell reads AGAINST a bright
background and nearly vanishes against black). The `add_fuzz` showcase
scene (`scenes/FeatureBased/Materials/wool_throw_fuzz.RISEscene`) applies
both corrections together — a genuinely CURVED subject (a rolled wool
bolster, `sdf_geometry` roundbox, not a flat panel: a flat panel's
silhouette is a straight rectangular boundary a normal-grown strand does
not cross on its own, unlike a curving limb) under a light studio dome
(`radiance_background TRUE`, ~0.36) with a strong rim behind-and-above —
and produces an unmistakable, clearly visible fibrous halo along the
entire silhouette at showcase quality (256 spp, 18 s). Before/after PNGs
in the final report.

### 11.3 What shipped: `add_fuzz`

A zero-required-argument verb (`add_fuzz(material?, amount? in {light,
medium, heavy}, baseHeadVersion)`, per the C-VERB adoption law — see
`docs/skills/adversarial-code-review.md`'s sibling census and doc 88 §13)
that grows a fuzz shell over every object bound to a `fabric_material` or
`weave_material` (or, when named explicitly, a plain diffuse
`orennayar_material`/`lambertian_material`). For every bound object it
mints, and ONLY mints — it never edits or rebinds the target material or
its bound objects:

- `<obj>_fuzz` — a `hair_geometry` grown on that object's own geometry.
  `count` (density) reads the object's own REAL world-space surface area
  (`IObject::GetArea()`), falling back to an equivalent-sphere estimate
  off its bounding box only when that area is unavailable/zero/unbounded
  — so `light`/`medium`/`heavy` mean the SAME strands-per-unit-area on a
  flat object and a round one alike (a flat panel and a sphere of equal
  real area realize the same count, proven by `TestFlatVsRoundAreaParity`).
  `length`/`width_root`/`width_tip` scale off the bbox's SMALLEST extent
  (so a thin flat object gets short fibres, not absurdly long ones), off
  this evaluation's own tuned recipe, so `amount:"medium"` reproduces the
  evaluation's numbers unscaled for an object matching its calibration
  sphere. `segments`/`base_detail`/`frizz` stay at the evaluation's own
  tuned constants (4/48/0.35) regardless of `amount`; no comb/clump/
  gravity/curl is written. A raw count exceeding hair_geometry's own
  2,000,000 hard cap is clamped, and the report SAYS SO.
- `<obj>_fuzz_material` — a `hair_material` whose `color` reads the
  fabric's own dye: `fabric_material.sheen_color` when bound, else the
  chunk's OWN documented default (the preset's colour where the preset
  sets one — velvet only — and otherwise WHITE, never the wrapped base's
  reflectance); `weave_material.warp_color`; or the named plain-diffuse
  material's own colour slot. The value is never re-typed, but
  `hair_material.color`, like every colour-painter slot, resolves STRICTLY
  by name (only scalar slots accept an inline literal) — an inline-literal
  value is minted into its own `uniformcolor_painter` first and bound by
  name; a name-shaped value is bound directly.
- `<obj>_fuzz_object` — a `standard_object` binding the two, `parent`-ed
  to the target object with no transform fields of its own, so it tracks
  the target's placement exactly, including any later edit to it.

Four refusals, each a no-op leaving the document byte-identical: nothing
qualifies; a `<obj>_fuzz`/`<obj>_fuzz_material`/`<obj>_fuzz_object` name
already exists (an existing fuzz shell); a bound object's own geometry
cannot host a groom (an `infiniteplane_geometry`, another `hair_geometry`,
or no resolvable geometry); the picked material resolves to the `silk` or
`satin` fabric preset (their tight, glossy structural sheen reads wrong
with a fibrous fringe. A scene with fewer than two NON-AMBIENT light
chunks is NOT refused — the message WARNS instead, since the shell mints
correctly either way and only its look needs a rim/back light to glow
(`ambient_light` is excluded from the count: it contributes no
directional information, so it can never itself produce a rim highlight).

**Known limitation, RESOLVED in the fix round.** An earlier revision used
an equivalent-SPHERE model for density (half the bbox's largest extent as
a radius, that radius's sphere area) which over-minted a large flat
object by well over an order of magnitude, because a flat object's real
surface area is far smaller than the equivalent sphere its own largest
extent implies (measured directly: a showcase panel scene minted ~1e6
strands and read as a uniformly furry blanket, not cloth with a fringe).
Density now reads the object's own REAL surface area
(`IObject::GetArea()`) first, falling back to the equivalent-sphere
estimate only when that area is genuinely unavailable (a null-geometry
container object) or unbounded (an `infiniteplane_geometry`). A residual,
smaller limitation remains and is worth naming: `IObject::GetArea()`'s
own accuracy is whatever the underlying geometry's `GetArea()` provides
— exact for analytic primitives (sphere, box) and a tessellated mesh's
real post-displacement area for `displaced_geometry`, but a documented
PARALLELOGRAM APPROXIMATION for `clippedplane_geometry` (exact for an
axis-aligned rectangle, which is the common case) — not a defect this
verb introduces, just a downstream precision bound worth knowing about.

**Tests:** `tests/AgentAddFuzzTest.cpp` (mint fields, both multi-object and
single-object mints, all four refusals, the light-count WARN, amount-band
scaling, colour-derivation across all four source shapes, undo/redo,
bare-call selection + determinism, full wire-surface coverage on MCP/RPC/
chat-codec/autonomy postures, and a real render proving strands exist via
a per-pixel "newly lit" comparison against the bare material's exactly-
zero background). Regression scene pair:
`scenes/Tests/Materials/wool_fuzz_halo_{A_bare,B_fuzz}.RISEscene` (the
evaluation's own sphere scenes; `B_fuzz` is the ACTUAL `add_fuzz()` output
on `A_bare`, hand-copied from a real run rather than approximated by hand
— see the FIX ROUND section's P2-1 note). Showcase:
`scenes/FeatureBased/Materials/wool_throw_fuzz.RISEscene` (a rolled wool
bolster, genuinely curved on every side, backlit under a light studio
dome — an unmistakable fibrous halo at showcase quality).

---

## 12. Integrator and renderer implications

**PT is the default and the target.** Fabric is the rough-glossy-plus-diffuse
regime where PT already wins the σ²·T matrix on 10 of 13 converged classes
([UNIFIED_INTEGRATOR_DECISION.md](UNIFIED_INTEGRATOR_DECISION.md)), and it adds
no caustic-class transport that would re-route it. The `auto_rasterizer` Tier-1
fall-through `else → PT` routes fabric scenes correctly **with no new clause**.

**Do not add a per-material routing tag.** None exists — the routing map in
[RENDERING_INTEGRATORS.md](RENDERING_INTEGRATORS.md) §2 is a *scene-level*
heuristic (indirect dominates direct; glossy bounces; enclosed geometry), not a
per-chunk hint, and hair's own routing hint was declined for exactly this reason:
"the hint would encode a decision the dispatcher already makes"
([HAIR_FUR_DESIGN.md](HAIR_FUR_DESIGN.md) §7). §16 records it as a non-goal.

**Ray-type bucket.** `SheenSPF` tags its scattered ray `eRayDiffuse`
([SheenSPF.cpp:113](../src/Library/Materials/SheenSPF.cpp)), so `max_diffuse_bounce`
governs sheen depth today. `fabric_material` should tag the **sheen lobe
`eRayReflection`** — it is a glossy grazing lobe, `max_glossy_bounce` is the
right budget for it, and this is the same mapping decision hair made for all its
lobes. The base lobe keeps whatever the substrate tags it. **This is a behaviour
change relative to bare `sheen_material` and should be stated in the descriptor**,
since a scene that tuned `max_diffuse_bounce` for a sheen composite will behave
differently.

**Anisotropy is the substrate's, and that keeps the integrator story boring.**
Because Phase 1's sheen lobe is isotropic and the weave is delivered by rotating
the basis handed to an ordinary anisotropic substrate (§9.5), fabric introduces
**no new lobe shape to any integrator** — BDPT and VCM see a GGX/Ward/A-S lobe
they already evaluate, in a rotated frame, plus an isotropic Charlie lobe they
already evaluate. The one requirement this places on the implementation is that
the rotated basis reach the substrate's `value`, `Scatter` **and** `Pdf`
identically; a rotation applied in two of the three would be an MIS-visible
inconsistency, not merely a shading one.

**Sampling cost and grazing variance — a known, accepted Phase-1 cost.** Cosine
sampling a lobe as sharp as Charlie at α = 0.08 is a genuine mismatch: the pdf is
broad where the lobe is a thin ring near the horizon, so the estimator's weight
spikes exactly at grazing, where the lobe is the whole point of the material.
Phase 1 accepts that anyway, because the obvious remedy is worse than the disease
— D-importance sampling of Charlie has no VNDF, leaks below the horizon, and its
rejection-corrected density fails the shipped pdf harness (§9.4). **MIS against
NEE is what carries the variance in the interim**, which is the right tool here:
a sheen halo is lit overwhelmingly by direct light at grazing, the regime light
sampling handles well, and §9.2's mixture `Pdf` gives MIS a correct weight. §9.9
gate 10 measures the residual cost, and that number is the entry condition for
the D-sampling follow-up.

**BDPT and VCM.** A real evaluable-anywhere `IBSDF` with a real `Pdf` works
through the shared `PathVertexEval.h` machinery with **no integrator changes** —
hair's precedent. Two fabric-specific notes:

- **Reciprocity is genuinely exact here**, which is better than hair. `D` depends
  only on `n·h`, `V` is symmetric in `nDotL`/`nDotV`
  ([CharlieSheen.h:104-112](../src/Library/Materials/CharlieSheen.h)), and the
  albedo-scaling factor is a `min` of two symmetric-under-swap arms. So
  `fabric_material` carries none of the model-level bias hair's near-field
  h-conditioning and cuticle-tilt convention introduce. It should be *proven*
  (§9.9 gate 5) rather than asserted.
- **The tangent fix is a BDPT correctness prerequisite, not only a look fix.** A
  rotation field over a discontinuous per-triangle base gives a BSDF that jumps
  across every triangle edge; BDPT evaluates the BSDF at both endpoints of every
  connection and MIS-weights against reverse PDFs computed there. Building weave
  anisotropy on today's arbitrary base would inject a discontinuity into the MIS
  denominators.

**Full-sphere scattering.** The Charlie lobe is reflection-only, so
`fabric_material` originates no transmission of its own; what it does is
**forward its substrate's** (`ScattersFullSphere()` / `CouldLightPassThrough()`
both return the base's answer, R8 P1.1 / §15 debt 22), attenuating it by the same
Kulla-Conty product law the reflect side uses. Over a `transmission thin`
`weave_material` that inherits the shipped full-sphere-NEE machinery
([IMaterial.h:237](../src/Library/Interfaces/IMaterial.h)) — the same hook that
recovered 6-8× on point-lit backlit hair and dropped the PT/BDPT ratio from
15.5-16.3× to 1.72-1.77×. Over every other allowlisted substrate it is false, and
the material behaves exactly as the Phase-1 code did.

**Multiple scattering in the fabric layer.** Not modelled in Phase 1 (§9.4). The
consequence is a slightly harder terminator than reference. Named, gated,
measured — not hidden.

**HWSS and the spectral path.** The `E` table is achromatic (roughness and
geometry only), so it multiplies identically at every wavelength and cannot
introduce a spectral-bundle bias — the same structural argument hair's achromatic
sampling proxy makes. The dye tint goes through `GuardedGetColorNM`, so authored
white is a bit-exact no-op. §9.9 gate 7 measures `hwss=true ≡ hwss=false`
regardless, because the env-IBL arc is the standing evidence that structural
arguments about spectral bundles need measurements behind them.

**SMS.** No lobe is delta; SMS correctly ignores fabric entirely, as it ignores
hair.

**OIDN — the transferable warning.** The hair arc measured this and the result is
directly applicable: on high-frequency fibre structure, **`oidn_prefilter
accurate` is strictly worse than `fast` at every sample count** (15-18 % vs
25-30 % gradient-magnitude shortfall against reference), the deficit **does not
close with sample count**, and the root cause is the aux *prefilter* smoothing
one fibre's normal into its neighbours — not the aux capture. A resolved weave at
texel-scale frequency is the same regime. **Rules for fabric: never
`oidn_prefilter accurate` on a resolved weave; use `oidn_denoise FALSE` above
~100 spp; and validate every fabric appearance measurement with `oidn_denoise
FALSE`.** That last one is not a style preference — the `EnvLightBalanceTest`
recalibration is the standing evidence that a suite can silently measure the
denoiser's output for months.

**Studio rig — no change needed, and here is why.** The rig
([AgentSession.cpp:18274-18290](../src/Library/Agent/AgentSession.cpp)) already
carries a **RIM light at 0.68 intensity, "behind and above"**, which is exactly
the light a sheen halo needs, plus a neutral checker dome chosen because "the
sharp-versus-smeared edge of a reflected check is the roughness read." Two
observations:

- For **isotropic sheen**, the halo reads by *self-grazing* on any curved
  subject — the existing sheen scene's own comment makes this point, and it is
  why that scene works with a single key and no rim. The rig is already better
  than that scene.
- For **anisotropic weave**, the read is the checker dome, not the directionals:
  a reflected check smeared **along one axis and not the other** is the
  anisotropy read, in the same way its sharpness is the roughness read. That is
  already what the dome does.

So the recommendation is **do not touch the rig**. The one thing worth adding is
a *framing* note in the preset scene: a fabric preset previews best on a subject
with a silhouette that turns through grazing — a cushion or a draped cloth, not a
flat card — because the diagnostic cue is at the silhouette. That belongs in the
scene's header comment, not in code.

---

## 13. Adoption and measurement

**The laws this plan is built on** (`docs/agentic-redesign/88-…md` §2):

- **C-ADV.** Advice ≈ 0. Design notes naming `scalar_painter` fired up to
  30×/session, were demonstrably read, and drove **0/24** lifetime adoptions;
  voluntary/opt-in tools **0/64**. *Consequence:* documenting `fabric_material`
  and hoping is not a plan.
- **C-TYPE.** The failure is a slot-typing prior, not ignorance. Worked examples
  lifted painter *diversity* (floor 1→3) because models copy examples; the same
  lever moved spatially-varying roughness **0/24**. *Consequence:* the preset
  enum and the verb are the structural levers; the worked example is necessary
  and not sufficient.
- **C-VERB.** When advice fails, ship a verb. *Consequence:* `make_fabric`.
- **C-READ.** The read-set is the knowledge boundary; teaching content goes in
  the proven pulls with exactly one execution-validated parsing example.
- **C-MEAS.** Census, not vibes. N=3 minimum; cross-provider before believing a
  null; pre-committed stop rules.

**The master triad applied**: *summoned category, price the inferior path,
exactly one worked example that parses, and let the census say whether it moved.*
Summoned category: asked for a cushion, a model reaches for the **material**
category, and `fabric_material` is a material. Priced inferior path: the
descriptor for `sheen_material` should name `fabric_material` as the layered
route and say plainly that a bare sheen material has no base — pricing the thing
that produces the dark, baseless sphere. One example: §9.8, shipped as a scene
and pointed at from `materials-and-media-basics.md`.

### 13.1 The measurement plan

**Instrument.** A new `evals/scenarios/fabric_closeup.json` on
`rich_material_closeup`'s shape, subject changed to an **upholstered chair arm in
raking light** — chosen because it demands a fabric read, a curved silhouette,
and a seam, i.e. all three Phase-1 mechanisms, without naming any of them.
Gating checkpoints: a chunk-kind checkpoint that is a **disjunction** —
`fabric_material` OR (`sheen_material` AND `composite_material`) OR
`sheen_material` alone — so a run that reaches for today's (broken) idiom scores
as an *attempt*, not a failure; a `any_param_references_kind:scalar_painter`
checkpoint; and a render band on `meanLuma` **calibrated on the actual scene**,
since a dark velvet is legitimately darker than the brass the existing band was
set for.

**Pre-verb run against today's tree, before the first Phase-1 commit.** Post-verb
run with an identical config after Phase 1.

**Metrics, pre-committed.**

| metric | what it decides |
|---|---|
| `fabric_material` occurrence rate | whether the material converts at all |
| **`fabric <preset>` vs. raw-parameter authoring** | **whether the preset-enum mechanism works — the novel hypothesis of this design** |
| `make_fabric` call rate | whether the verb converts, and whether it is redundant with the enum |
| Any `weave_rotation` binding vs. none | whether weave direction is reachable in practice or is dead surface |
| Baseless-sheen occurrences (`sheen_material` with no base) | the specific failure the material exists to prevent |
| "Flat plastic" failures (Lambertian or bare PBR on a named-fabric subject) | the pre-verb baseline's dominant mode; the number that should fall |
| Runs wanting both `make_fabric` and `add_wetness` on one material | whether §9.7 refusal 5's composition is worth building |
| Per-provider compliance; report-level pass@1 | standard |

**Stop rules, pre-committed.**

- If the post-verb `fabric_material` rate is **0/6 across both providers**, the
  escalation is **not** more documentation (C-ADV). It is to check whether the
  paired design note fires at all, on the `add_wear` causality model (note at
  trajectory line 3 → call at line 97).
- If `fabric_material` converts but **`fabric <preset>` is never used** while raw
  parameters are, the preset-enum hypothesis is **falsified** — record it as such
  in this document, because it is the first test of **multi-slot preset seeding**
  anywhere in RISE (§9.3 — per-parameter quick-picks exist, but have never been
  censused either) and the null result is as valuable as the positive one.
- If **`make_fabric` is never called but `fabric_material` is authored directly**
  at a healthy rate, the verb is redundant for this capability and that is a
  finding about C-VERB's scope, not a failure.

**Cross-provider before believing a null.** A single-provider zero is not a
finding.

**Altar-stress element.** One fabric element in an `altar_stress`-shaped
instrument: *a draped velvet cloth over a plinth corner*, which quietly demands
the sheen lobe, the curvature signal, and a curved silhouette. Graded against the
per-element scoreboard narrative, re-run after each hardening commit.

---

## 14. Cost

Structural counts, not measurements. No implementation exists.

### 14.1 Phase 1 — library source files and the build tax

The per-file tax is measured, not estimated
(`grep -c` against a recently-added pair, `CoatedSPF.cpp`/`.h`):

| build file | count per `.cpp`+`.h` pair | count per header-only |
|---|---|---|
| `build/make/rise/Filelist` | 1 | 0 |
| `build/cmake/rise-android/rise_sources.cmake` | 1 | 0 |
| `build/VS2022/Library/Library.vcxproj` | 2 | 1 |
| `build/VS2022/Library/Library.vcxproj.filters` | 2 | 1 |
| `build/XCode/rise/rise.xcodeproj/project.pbxproj` | 10 | 4 |
| **total** | **16** | **6** |

Applied to Phase 1's new files, following `coated_material`'s file split:

| new file | kind | build touchpoints |
|---|---|---|
| `FabricMaterial.h` | header-only | 6 |
| `FabricPresets.h` | header-only (the `fabric` enum's table) | 6 |
| `FabricBRDF.{h,cpp}` | pair | 16 |
| `FabricSPF.{h,cpp}` | pair | 16 |
| `SheenDirectionalAlbedo.{h,cpp}` | pair (the `E` / `Ē` lookups) | 16 |
| `SheenDirectionalAlbedo_LUTData.cpp` | generated data | ~6 |
| **subtotal** | **6-7 files** | **≈ 66** |

The preset table is its own header rather than living inside
`FabricMaterial.h` because **two** consumers read it and must never disagree:
the chunk parser's `Finalize` seeds the chunk's own slots from it, and
`Job::AddFabricMaterial` (and later `make_fabric`) reads the
recommended-substrate half. §9.3's split table is a single struct.

**The baked data file holds two tables, not three** (round 5): `E(α, cosθ)`
and `Ē(α)`, 4.125 KB of floats at the 32×32 bake (16.25 KB since the
round-9 64×64 re-bake). The `S(α, m)` kernel table that §9.2's
earlier `min` form required is retired — the product form factors, so
`hemisphericalAlbedo` closes in `Ē` alone. The generator's `ComputeS`,
`BakeS` and `ClampedOneMinusME` are gone with it; `E` and `Ē` are
byte-identical across the change.

Plus edits to existing library files, which cost **no** build-project work:
`Object.cpp` / `CSGObject.cpp` (verify the tangent branch; likely unchanged),
the two triangle-mesh specialisation headers (two-branch form), the four
analytic-primitive `.cpp`s carrying five write sites (cylinder has two), and
`ClippedPlaneGeometry.cpp` (a new write site, not an augmented one) — **§9.1's
write at eight call sites across seven files, ≈ 30 lines**; `RISE_API.{h,cpp}`,
`IJob.h`, `Job.{h,cpp}`,
`ChunkParserRegistry.cpp`, `MaterialIntrospection.cpp`, `SheenBRDF.h` and the
sheen chunk description (the stale-name fix), `GLTFSceneImporter.cpp` (re-enable
the sheen path), `src/Library/Parsers/README.md`.

### 14.2 Phase 1 — everything else

| item | cost | basis |
|---|---|---|
| Bake tool | 1 file, **0 build-project edits** | `tools/SheenDirectionalAlbedoGen.cpp`; the make `tools` target loops over `tools/` sources (`build/make/rise/Makefile:190-212`), on `HairMedullaProfileGen`'s precedent. **Verify the Windows/Xcode tool build separately** |
| Verb plumbing surfaces | **8** | `AgentSession.h/.cpp`; `AgentMcpAdapter.cpp`; `AgentChatCodecs.cpp` (the hand-duplicated `kToolDefs`); `AgentChatLoop.cpp`; `AgentRpc.cpp`; `AgentLoopbackHttpServer.cpp`; `AgentDiagnostic.h`; `AgentEvalRunner.cpp` |
| Verb emission complexity | **up to 4 minted chunks + N rebinds, one swap** | §9.7: `<name>_fabric_f0` (`uniformcolor_painter`, only for a GGX substrate — `rs` cannot be inline, [Job.cpp:4243-4244](../src/Library/Job.cpp)), `<name>_fabric_base`, the weave painter, `<name>_fabric`. `add_wetness`'s emitter mints at most 3 and never a *material*, so `AgentSession`'s share of the work is larger than the 8-surface count alone suggests — budget the substrate-minting and the three-name reflectance lookup (§9.7 step 0) as the two genuinely new pieces |
| New test files | **3** (was 2) | `tests/FabricMaterialChunkTest.cpp` (on `CoatedMaterialChunkTest.cpp`'s 873-line pattern), `tests/AgentMakeFabricTest.cpp` (on `AgentAddWearTest.cpp`'s ~248-`Check(` pattern), and — added during implementation — `tests/FabricRenderTest.cpp`, gate 7's own render-level suite on `HairRenderTest.cpp`'s harness. **Tests are glob-discovered — no build-file edits** |
| Existing tests edited | **3** | `LayeredWhiteFurnaceTest.cpp` (new configs + the config-6 re-diagnosis), `SPFBSDFConsistencyTest.cpp` (fabric **and** sheen reciprocity entries), `SPFPdfConsistencyTest.cpp` |
| Scenes | **6** (Phase 2 added one; was 5, was 2) | `scenes/Tests/Materials/weave_presets.RISEscene` (Phase 2's own regression: the four weave presets, drafts legible), plus `scenes/Tests/Materials/fabric_presets.RISEscene`, `scenes/Tests/Materials/anisotropic_uv_tangent.RISEscene` (§9.1's own bucket-A regression), and three showcases: `scenes/FeatureBased/Materials/velvet_cushion.RISEscene` (§9.8), `fabric_swatches.RISEscene` (the seven presets on draped swatches), `denim_and_satin_drape.RISEscene` (the hero, and gate 9b's subject).  The last two moved to fabric-over-weave in Phase 2 |
| Tools | **1** (new row) | `tools/fabric_sheen_measure.py` — the gate 9 / 9b / 10 measurement harness (EXR-reading, sphere-subject, K-trial), added during implementation because all three gates need the same renderer-driving + EXR-reading scaffolding and none of the existing `tools/` scripts read radiance |
| Docs | **6** (was 4) | this file; `MATERIALS.md` §6/§8 catalogue; `SCENE_CONVENTIONS.md` §8.7 (the weave-aliasing idiom of §5.5); `GLTF_IMPORT.md` §15 (sheen now imports); and the two scene catalogues, `scenes/FeatureBased/README.md` and `scenes/Tests/README.md` |
| Read-set edits | **2** | `materials-and-media-basics.md` (one parsing example), the materials skill hook-line rewrite |
| Eval configs + committed results | **2 + 2** | pre/post runconfig + scenario, both runs' outputs committed under `evals/runs/` |

**Reference precedent for the whole.** `coated_material` — commit `1f929fef`,
**36 files changed, 4620 insertions, 21 deletions** — is the honest comparable:
a triad plus layering plus tests plus the five build projects plus interface
changes to `IBSDF.h` and `IJob.h`. Phase 1 is that **plus** the bake tool, the
verb's eight surfaces, and the tangent fix's cross-cutting regression sweep, so
**40-50 files is the honest expectation**, not 36.

**Per-hit render cost.** The `E` lookup is two clamped table reads and a bilinear
blend per evaluation — negligible against the Charlie `pow` it accompanies. The
mixture `Pdf` costs one extra sub-`Pdf` call. The tangent write costs one vector
copy, two bool stores and one predictable branch per hit on geometry that already
computes `dpdu`. The weave rotation costs one `RotateTangent` (two trig calls,
skipped outright when the angle is under 1e-9 —
[MicrofacetUtils.h:50-52](../src/Library/Utilities/MicrofacetUtils.h)) plus a
local `RayIntersectionGeometric` copy per substrate call. **The one real new cost
is variance at low roughness**, because Phase 1 keeps cosine-hemisphere sampling
under a sharp lobe (§9.4) and leans on MIS with NEE to carry it — a claim with a
measurement attached (§9.9 gate 10), not an assertion, and the number that gates
the D-sampling follow-up.

**Phase 2 cost.** `coated_material`'s order or larger; a transmission lobe pulls
in the full-sphere NEE path. Not costed further here — the parameter set is not
yet known (§10.1).

---

## 15. Correctness debts and open items

1. **Sheen has no reciprocity or consistency coverage.** Pre-existing.
   [SPFBSDFConsistencyTest.cpp:1140-1146](../tests/SPFBSDFConsistencyTest.cpp)
   lists Lambertian, isotropic GGX and three Coated configurations. Phase 1
   closes it (§9.9 gate 5). **No anisotropic material of any kind is in that
   sweep either** — Ward and Ashikhmin-Shirley are equally uncovered, and the
   tangent change of §9.1 alters their basis. Adding at least anisotropic GGX
   alongside is strongly advised.
2. **The "Charlie / Neubelt" name is stale in two places, and the rename must
   include the descriptor.** [SheenBRDF.h:9,18](../src/Library/Materials/SheenBRDF.h)
   is a comment; **[ChunkParserRegistry.cpp:4358](../src/Library/Parsers/ChunkParserRegistry.cpp)
   is not** — `cd.description` is the authoring surface, read verbatim by the
   agent-facing schema generator, the right-click context menu and inline
   autocomplete in both GUI scene editors, and the properties panel. Leaving it
   would keep publishing a wrong model attribution into auto-generated schemas
   long after the header comment was fixed, so **the descriptor text is part of
   the rename, not a follow-up to it** — and the same applies to the new
   `fabric_material` descriptor, which must not repeat the error.
3. **CLOSED 2026-09-02 (round 5) — furnace config 6 IS config 7's defect, now
   measured.** The debt was that config 6 carried a note about "inherited
   dissipation" rather than config 7's measured "substrate never reached", and
   this document declined to assert the stronger reading without a
   measurement. §9.9 gate 4 ran the probe and it came back unambiguous:
   **`SheenSPF` emits a downward ray in 0 % of draws at both θ = 0 and
   θ = 80**, identical to config 7's GGX top layer. `SheenSPF` is
   reflection-only by construction — a cosine-hemisphere draw about the
   ray-facing normal, gated below the geometric horizon
   ([SheenSPF.cpp:73-99](../src/Library/Materials/SheenSPF.cpp)) — so
   `CompositeSPF`'s random walk is never handed a downward ray and the
   substrate is never reached. The corroborating number is in the furnace
   table itself: config 6 reads `{0.0875, 0.1293, 0.2812, 0.5461}` and
   config 2 (bare sheen, no substrate at all) reads
   `{0.0875, 0.1283, 0.2810, 0.5453}` — the same curve to MC noise, which a
   composite that reached its GGX-PBR base could not produce. **Configs 6 and
   7 are one defect, not two**, and closing it needs a transmission path for
   reflection-only top layers rather than a walk fix. `fabric_material` is
   the shipped answer for the sheen case: it evaluates the combined closed
   form instead of walking. Config 6's note now carries the measured
   percentages, written at test time rather than remembered (§4.2).
4. **Pipe split on the rotation angle — and it now bites harder, so schedule the
   alias in Phase 1.** GGX's `tangent_rotation` is Color-pipe by construction
   (an admitted oddball); `fabric_material`'s `weave_rotation` is Scalar-pipe.
   Under §9.5 the two rotations **compose on the same surface** — the fabric's
   weave angle orients the yarn and the substrate's own rotation offsets within
   it — so an author now has a concrete reason to drive both from *one* painter,
   and cannot: the same field would have to be authored twice, once per pipe.
   **Phase-1 checklist item:** add a Scalar-pipe alias for
   `ggx_material.tangent_rotation` concurrently with `fabric_material` (accept
   either pipe on that slot, prefer Scalar, keep the Color binding working and
   mark it deprecated in the descriptor). It is a descriptor line plus a
   resolve-order branch, and doing it in the same slice avoids shipping a
   composition the scene language cannot express cleanly. `scalar_painter
   { function2d … }` bridges in the interim.
5. **Anisotropic sheen is deferred, not solved.** *(Rewritten 2026-09-02: this
   debt previously described an elliptical Charlie lobe as a Phase-1 feature.
   That feature is withdrawn — §9.5.)* Phase 1's sheen lobe is strictly
   isotropic, and weave directionality reaches the render through the substrate.
   That is the right call and it matches every production system surveyed, but
   it is not free: a genuinely **anisotropic fuzz** — a nap that is
   directionally combed, so the *grazing halo itself* is elongated — is
   inexpressible in Phase 1. Nothing in the surveyed literature supplies one
   cheaply: an anisotropic Charlie would need a re-derived normaliser, a
   masking term with azimuthal dependence, and a 4D (not 2D) directional-albedo
   table. If the need is ever observed, the LTC route (§9.4) is the more
   promising base for it, since an LTC's linear transform is naturally
   anisotropic.
6. **The Ashikhmin-Premoze-Shirley velvet normalisation is unverified.** §3.4's
   closed form is [from memory]. Not implemented here, so not blocking — but
   recorded so nobody hard-codes the constant from this document.
7. **Zhu 2023 and Jin 2022 parameter sets are unobtained.** Phase 2's first gate.
8. **BDPT/VCM/MLT read the geometry signals as neutral** in parts of their
   transport, because those integrators hand-build `RayIntersectionGeometric`
   records omitting `derivatives` and `signals`. Inherited, not created, by this
   design. Consequence: **a `curv`-driven seam-wear fabric will not match between
   a PT render and a BDPT or VCM render of the same scene**, and
   `auto_rasterizer` can route there without the author choosing it. The same
   debt [WETNESS_COAT_DESIGN.md](WETNESS_COAT_DESIGN.md) §12 item 1 carries.
9. **`ScatteredRayContainer::kCapacity = 12` drops silently**
   ([ISPF.h:116](../src/Library/Interfaces/ISPF.h)). A `fabric_material` nested
   under a deep composite could lose energy invisibly. An argument for the
   single-triad design, and a reason not to document composite nesting as a
   fabric idiom.
10. **Weave aliasing has no automatic mitigation.** §5.5's `fw` fade is an
    authoring idiom, not a mechanism; the plain procedural painters remain
    unfiltered. If weave fields become common, a footprint-aware `checker` or a
    filtered step builtin in the VM becomes worth costing.
11. **The multi-slot preset hypothesis is unmeasured.** Named per-parameter
    quick-picks already exist
    ([`ParameterDescriptor::presets`, ChunkDescriptor.h:449](../src/Library/Parsers/ChunkDescriptor.h)),
    but they are an editor affordance on one scalar and have themselves never
    been censused; what is new is **one name seeding several slots in
    `Finalize`**, and no material carries a preset of any kind. §13's stop rules
    include its falsification.
12. **glTF per-texel `anisotropy_rotation` is still dropped**
    ([GLTFSceneImporter.cpp:1300-1307](../src/Library/Importers/GLTFSceneImporter.cpp)).
    The importer says an `atan2` painter primitive would be needed — but the
    expression VM **has** `atan2` (confirmed against its function table), so
    this looks solvable today with no new primitive: extract the texture's R and
    G through two `channel_painter`s remapped from [0,1] to [−1,1], then

    ```
    scalar_painter
    {
    	name			aniso_rotation
    	param			dummy 0.0
    	expression		atan2( 2.0*rotG - 1.0, 2.0*rotR - 1.0 )
    }
    ```

    with `rotR`/`rotG` supplied as `scalar_painter { painter <chan> }` inputs.
    **Sketch only — not execution-validated**, and it would bind to
    `fabric_material`'s Scalar-pipe `weave_rotation`, not to GGX's Color-pipe
    `tangent_rotation` (debt 4). Worth a look; not in Phase 1's scope.
13. **The Blender bridge has no sheen, anisotropic or velvet mapping at all**,
    documented or otherwise. A silent gap. Phase 1's `fabric_material` is the
    natural target for Principled's Sheen sockets, but the bridge work is a
    separate slice.
14. **Tier-1 spectral dye (per-wavelength absorption through a fibre path) is
    not attempted.** §2's table. It is a genuine RISE-specific opportunity given
    the hair σ_a machinery already in tree, and it depends on a yarn model rather
    than a sheen lobe.
15. **Mirrored UV seams flip the weave direction.** §9.1. A `dpdu`-derived
    tangent is coherent within a UV island but can mirror across a seam, and
    `vShadingTangent` has no `bitangentSign` companion to recover the chirality
    the way `vTangent` does
    ([RayIntersectionGeometric.h:296-299](../src/Library/Intersection/RayIntersectionGeometric.h)
    vs [:344-345](../src/Library/Intersection/RayIntersectionGeometric.h)).
    Accepted as a known limitation on `NormalMap`'s own precedent
    ([NormalMap.cpp:109-120](../src/Library/Modifiers/NormalMap.cpp)); the
    remedy is the same one that comment prescribes, re-export with a `TANGENT`
    accessor. **That remedy is only real because of §9.1's tangent-precedence
    branch** — an earlier draft prescribed it while writing `dpdu`
    unconditionally, and since `Object::IntersectRay` builds the ONB solely from
    `vShadingTangent` ([Object.cpp:699-772](../src/Library/Objects/Object.cpp))
    and never consults `vTangent`, re-exporting with a `TANGENT` accessor would
    have changed nothing at all. The indexed-mesh site now prefers
    `ri.bHasTangent ? ri.vTangent : dpdu`, which is what makes the advice
    actionable. §9.9 gate 1 puts a mirrored-UV asset in bucket A so the residual
    severity is measured rather than assumed. A `bitangentSign` companion on
    `vShadingTangent` — for meshes with mirrored UVs and *no* authored tangent —
    is the complete fix and is not in Phase 1's scope.
16. **REWRITTEN 2026-09-02 (round 5), and now a small, measured debt.**
    `fabric_material`'s `hemisphericalAlbedo` **is** a closed form —
    `substrate.hemisphericalAlbedo() · (1 − m·Ē) + sheenColor · Ē` — because
    the product kernel factors where the `min` did not. The baked `S(α, m)`
    table, the per-substrate-class 3D table (route 1b) and the `min(a,b) ≤
    (a+b)/2` bound (route 2) are all **retired**; the
    construction-time-quadrature route stays withdrawn for its own reason
    (no hit context at construction, so it could only evaluate a substrate's
    *constant* parameters and would look exact while being wrong wherever a
    painter varies). §9.2.

    What remains is only the **uncorrelated-response approximation**: pulling
    the substrate's albedo out of the joint `(l, v)` integral requires
    `f_base` to be constant in the integration variables, so route 1 is
    **exact for Lambertian** and approximate for Oren-Nayar and GGX, both of
    which are genuinely joint in `l` and `v`. §9.9 gate 5(b) measured it at
    **≤ 0.64 %** against a pre-committed 5 % — smaller than this doc's
    earlier ~3 % toy estimate, and 0 by construction on the Lambertian
    control row. **The debt is therefore quantified and small**, which is a
    materially different statement from the "unquantified bias" the earlier
    text had to leave open.

    The open item that survives is the **composition**: getting this quantity
    right is what would let a `coated_material` sit *over* a
    `fabric_material` — a waxed canvas — since this is exactly what the
    coat's recycling denominator consumes. That composition is not built, and
    `coated_material`'s substrate allowlist does not admit `fabric_material`
    today.

17. **NEW 2026-09-02 (round 5) — the substrate's own `hemisphericalAlbedo` is
    the larger error, and it is not `fabric_material`'s to fix.** Gate 5(b)'s
    end-to-end figure against a brute-force quadrature reaches **15.7 %**,
    of which debt 16's factorisation accounts for at most 0.64 %. **All the
    rest is inherited**: `OrenNayarBRDF::hemisphericalAlbedo` returns `Rd`
    verbatim — its own header
    ([OrenNayarBRDF.cpp:148-190](../src/Library/Materials/OrenNayarBRDF.cpp))
    documents this as measured 12.6 % high at roughness 0.5 and up to 25.6 %
    high at roughness 1, and mildly view-dependent besides — and GGX's
    estimate runs high at grazing. Measured through the fabric wrapper:
    Oren-Nayar σ = 0.6 reports **17.8 %** high, GGX α = 0.5 **6.8 %** high.

    **Nothing in `fabric_material` can correct this**, and the round-4 remedy
    it would have been mistaken for — a 3D `S(α, m, σ_base)` table — could
    not have touched it either, because the error is the substrate
    misreporting *its own* reflectance one layer down. **`coated_material`
    already inherits the identical debt** through its Saunderson recycling
    denominator `1/(1 − r_i·R)`, where an over-estimated `R` over-amplifies
    the recycled term; OrenNayarBRDF's own note bounds that at roughly 20 %
    of the recycled portion at extreme roughness.

    The fix, if it is ever wanted, is a bake in `OrenNayarBRDF` (and a review
    of GGX's estimator), validated on its own — *not* work in either layered
    material. That file's note is explicit that no clean closed form exists
    to correct it with, since the C3 and L2 terms add energy back in a way
    that does not factor out of `Rd`. Recorded here so the next reader of
    gate 5(b)'s output does not mistake a substrate debt for a fabric one.

18. **NEW 2026-09-02 (round 6), FIGURES CORRECTED 2026-09-03 (round 8),
    RESOLUTION RAISED 2026-09-04 (round 9, reviewer P2.3) — fabric is
    energy-BOUNDED, not energy-CONSERVING; residual REDUCED, not
    closed.** Round 9 raised the `E` table's uniform refinement from
    32×32 to 64×64 nodes on both axes (kept the log-α scheme and the
    grazing-warped μ = (j/(N−1))² axis with the node-1 floor — no
    ad-hoc node). Worst ρ over the whole reachable domain
    (α ∈ [0.04, 1], μ down to 1e-6) is now **1.0075**, at α ≈ 0.065 and
    n·v ≈ 5.5e-4 — down from the round-8 figure of 1.0167, but **not
    proportionally**: two of the three bands (n·v ≥ 0.0349 and
    n·v < μ₁) fell to **+0.19 %** and **+0.18 %** respectively (both now
    under an informal +0.2 % bar) because they are driven by the same
    mechanism round 8 named — the log-α chord under-reading a concave
    peak near α ≈ 0.9 — and doubling the α axis halves that cell's
    error. The **middle band** (μ₁ ≤ n·v < 0.0349) fell only to
    **+0.75 %** (from +1.67 %, a 55 % reduction, not the ~4× the other
    two bands got) because its dominant driver **relocated**: once the
    α ≈ 0.9 mechanism shrank, a second, previously-smaller residual near
    the roughness floor (α ≈ 0.065, not α ≈ 0.9) became the largest
    contributor there. `kMinSheenAlpha`'s own scan also moved — the
    smallest baked α clearing "max over μ ≥ 0.03 of E ≤ 1" is now
    0.037276 (was 0.028289) — `kMinSheenAlpha = 0.04` still clears it,
    with less margin than before but with both bilinear-bracketing rows
    still under 1. Data file: 16.25 KB payload (was 4.125 KB, ~4×, as
    expected for a uniform 2×/2× refinement); bake time ~108 s (was
    ~24 s, ~4.5×, tracking the ~4× cell count plus a larger fraction of
    cells needing the adaptive quadrature's higher rungs). §9.2 has the
    full per-band table (both resolutions) and the re-derived shortfall
    figures; `tests/SheenDirectionalAlbedoTest.cpp`'s domination-slack
    check and `tests/LayeredWhiteFurnaceTest.cpp`'s grazing-check
    tolerance were both re-derived and tightened for the new table.
    **Closing the middle band's new residual is unstarted work** — table
    resolution alone did not close it, it only moved the bottleneck; a
    principled fix needs a fresh root-cause hunt near the roughness
    floor rather than another blind resolution doubling.

    The round-6/7/8 history follows, unedited, for the record. §9.2.
    Once the `E` table's cosθ
    axis was warped toward grazing, the resolved Charlie lobe turned out
    to exceed 1 (reaching 1.152) even above the roughness floor, inside a
    narrow sliver at n·v < 0.03. A symmetric normaliser
    `max(1, E(v), E(l))` on the sheen term and `Ê = min(E, 1)` on the
    base keep ρ ≤ 1 there, but **exact conservation is knowingly given
    up**: measured ρ within 0.06 % of 1 for n·v ≥ 0.0349 and within
    +0.30 % at n·v = 0.005 (that last figure is the table's own
    interpolation error between nodes, not the normaliser).

    The residual is small and one-signed, and the sliver carries almost
    no cosine-weighted energy — but it is a real departure from the
    "ρ = 1 exactly, at ANY m and ANY α" claim round 5 made, and the
    honest statement of the model is the two-regime one. Closing it
    would mean either a finer warp / more cosθ nodes (the residual is
    interpolation error, so it shrinks with resolution) or an
    energy-compensated sheen lobe such as the LTC form §9.4 gates.
    Neither is Phase-1 work. `tests/LayeredWhiteFurnaceTest.cpp`'s
    grazing check is the guard, and it asserts the bounded posture
    inside the sliver rather than pretending to the conserving one.

19. **CLOSED 2026-09-03 (round 8) — `CookTorranceSPF`'s `PdfNM` reported a
    different mixture from `ScatterNM`; fixed by adopting `fabric_material`'s
    achromatic-selection convention.** Not a fabric defect; surfaced by
    round 7's broadening of `tests/SPFPdfConsistencyTest.cpp`'s spectral
    companion beyond the two `fabric_material` rows it originally covered,
    and closed in a follow-up pass because the broadening that found it is
    this phase's work.

    `CookTorranceSPF::ScatterNM` built its 3-lobe selection weights
    per-wavelength (`GetValueAtNM` on masking, `GuardedGetColorNM` on
    diffuse and specular) while `PdfNM` was a bare `return Pdf(...)`,
    which rebuilds them from the RGB `max3`. So the density stored on a
    spectral sample was a different mixture from the one `PdfNM` reported
    for that direction — the Scatter↔Pdf agreement MIS depends on.
    **Measured before the fix: `maxRelErr` 1.44e-4 at 30° and 1.55e-4 at
    60°**, against 4e-16…1.6e-13 for every other row in that sweep
    (Lambertian, Oren-Nayar, GGX iso/aniso, SSS, both coated rows, both
    fabric rows) — eleven orders of magnitude out of family. The RGB pipe
    passed exactly, which is why nothing caught it before the twins were
    compared.

    **The fix**: `ScatterNM`'s lobe-selection weights (and `alpha`) are now
    read ACHROMATICALLY — the same RGB `max3` / `GetValuesAt(ri).v[0]`
    source `Pdf()` already used — exactly the convention
    `fabric_material` chose on purpose (§9.2's `ResolveFabric` note).
    `PdfNM`'s bare forward to `Pdf()` is therefore correct by construction
    now, for every wavelength; no code change was needed there beyond a
    comment. The per-wavelength VALUE (`kray`/`krayNM`) is unchanged.
    **Measured after the fix: `maxRelErr` 1.18e-13 at 30° and 1.27e-13 at
    60°** — squarely inside the family every other row occupies; the
    row-specific `crossValTol = 1e-3` relaxation was removed and the row
    now uses the shared `CROSS_VAL_TOL` like every other exact row.
    `tests/CookTorranceMultiscatterTest.cpp`'s Test E (an independent
    white-box reconstruction of the same selection weights) needed the
    identical achromatic correction to stay a valid oracle — a sibling of
    the same pattern found one hop outside the material itself.
    `tests/CookTorranceHWSSTest.cpp` (new) proves the render-level
    consequence: a tinted Cook-Torrance sphere's `hwss=true` PT-spectral
    render agrees with `hwss=false` within ~1% (tolerance 5%), reusing
    `FabricRenderTest.cpp`'s reference-free HWSS-invariant pattern.
    Sibling audit across every `PdfNM` in `src/Library/Materials`
    confirmed CookTorranceSPF was the only site carrying the pattern; full
    table in [SPECTRAL_PARITY_AUDIT.md](SPECTRAL_PARITY_AUDIT.md) §2.x.

    **Follow-up 2026-09-04 (REVIEW_CHIP3): the achromatic fix above traded
    the pdf-mismatch bug for a smaller dropped-lobe bias, now also
    closed.** Making the selection weights an *exact* RGB `max3` meant an
    authored PURE-BLACK diffuse or specular slot gave an exact-0 selection
    weight — permanently unreachable from `ScatterNM` — while
    `GuardedGetColorNM` on that same slot leaks `~2.5e-5` (the JH LUT's
    black cell cannot represent exact black), a value `CookTorranceBRDF::
    valueNM` (NEE) still reported every time. Fixed by flooring the
    selection weights at `kSelFloor = 1e-3` (`ComputeLobeWeights`, a single
    helper shared by `Scatter`/`ScatterNM`/`Pdf`) before normalising —
    unbiased for any floor in `(0,1]` since the sampled `kray`/`krayNM`
    still divides by the probability it was drawn with (the floor spends a
    fixed extra 0.1% of samples on a possibly-dead lobe; that is variance,
    not bias), and algebraically a no-op for any material whose RGB `max3`
    exceeds the floor (ordinary tinted/white rows are bit-identical to
    before). Proof: the existing `CookTorrance` row unchanged
    (`1.18e-13`/`1.27e-13`); two new rows with one authored-black slot
    each pass at the same exact tolerance; a direct reachability count
    (200k trials) confirms the floored lobe now fires at the predicted
    floor rate in the NM pipe (previously exactly 0); `CookTorranceMulti
    scatterTest`'s Test E — which already used a black diffuse painter —
    needed its own replicated formula updated to match, confirming the
    old formula and the new floored one disagree by exactly the floor's
    order of magnitude; a furnace/energy check on the base tinted fixture
    confirms the floor does not move a non-near-black material's
    directional albedo. Sibling audit: `WeaveBRDF::SurfaceSelectWeight`
    already floors (`kMinLobeWeight = 0.15`); `FabricBRDF::
    ValueNMWithParams` already caps the value at the selection weight
    (`tintNMCapped = min(tintNM, m)`) instead of flooring the weight at
    the value — a different but equally valid guard. Neither needed a
    change. Full detail: [SPECTRAL_PARITY_AUDIT.md](SPECTRAL_PARITY_AUDIT.md)
    §2.x follow-up.

20. **RESOLVED 2026-09-04 (chip 4 / task_93ff4a8a). Originally filed
    2026-09-03 (P2-B fix round, REVIEW_P2R7.md) as "BDPT/VCM over-count
    a `transmission thin` weave by 100-350x on the shipped
    backlit-curtain scene; diagnosed as an INTEGRATOR limitation, not a
    material defect." The measurement does not reproduce — the real
    cause was the OTHER side of an unrelated, same-day bug (debt 21,
    below), not a BDPT/VCM defect.**

    **Original reproduction.** `tests/FabricRenderTest.cpp::TestBacklitSheerCurtain`:
    a flat `weave_material` curtain (`fabric linen`, `transmission thin`)
    in front of an `omni_light`, 256 spp, 32x32, `oidn_denoise FALSE`.
    PT mean luminance ≈ 7.3-7.9e-5 across independent seeds; BDPT mean
    ≈ 0.0253, a stable, REPRODUCIBLE ≈ 325-350x (not a heavy tail's usual
    seed-to-seed spread). Reducing `indirect_clamp`/`direct_clamp` from
    off → 0.01 → 0.001 tracked BDPT's mean down roughly in proportion to
    the clamp ceiling itself (0.0253 → 0.008 → 0.0008) — the signature
    of a persistently-hit near-singular contribution, not rare fireflies
    a clamp ordinarily tames.

    **Original diagnosis (REVIEW_P2R7.md, independently re-derived from
    first principles after ruling out every material-side candidate).**
    The unguarded vertex-connection geometric term
    `G = cosA·cosB/dist²` in [`BDPTUtilities::GeometricTerm`](../src/Library/Utilities/BDPTUtilities.h)
    (floored only at `dist² < 1e-20`) is unbounded as `dist -> 0` while
    both `|cos|` terms stay ≈ 1. A `weave_material` sheer curtain is
    RISE's FIRST flat, zero-thickness `ScattersFullSphere()` surface —
    `HairMaterial`, the only prior full-sphere material, is a curve with
    real cross-sectional separation between its front and back, so an
    eye-subpath vertex and a light-subpath vertex sampled from opposite
    faces can never coincide there. On an infinitesimally-thin quad they
    can land arbitrarily close together, driving the geometric term
    toward infinity — a theoretically sound argument that this session
    confirmed is architecturally still true, but which this scene's
    measurement was NOT actually demonstrating (see below).

    **Re-diagnosis (chip 4, 2026-09-04).** Debt 21 (below) fixed a
    scale-relative self-hit floor in `RayBilinearPatchIntersection`
    (commit `d01a320a`) on the SAME DAY debt 20 was filed, a couple of
    hours later — an otherwise-unrelated PT-side bug. Re-measuring
    `TestBacklitSheerCurtain` on top of that fix (still `oidn_denoise
    FALSE`): BDPT/PT settles at 0.899-0.900 and VCM/PT at 0.932-0.933,
    stable across five independent seed bases at both 256 and 1024 spp,
    on both the MEAN and the MAX-pixel luminance (no residual firefly:
    `tools/ExrFireflyInspect.cpp`-style 4x-neighbourhood-median check
    finds 0). A harsher touching-distance stress scene was constructed
    to push as hard as possible on the theoretical singularity — a MESH
    area light 0.01 units directly behind the curtain (not the
    well-separated 1.5-unit gap the P2-B era's other regression uses),
    `gap 0` so every scatter uses the CONTINUUM diffuse-transmission
    lobe, `max_light_depth 12` / `max_eye_depth 12` — and it gives
    BDPT/PT = VCM/PT = 1.01, 0 fireflies. `tests/FabricRenderTest.cpp::TestTouchingAreaLitCurtainAllIntegrators`
    is the permanent regression for this scene.

    The mechanism: PT's own NEE shadow ray toward the point light behind
    the curtain has no epsilon bump of its own and relied entirely on
    `RayBilinearPatchIntersection`'s self-hit rejection, which pre-fix
    accepted ANY positive `dRange` (literally `dRange > 0` at all three
    hit-acceptance sites, not even an absolute `NEARZERO` compare) —
    spuriously self-occluding the large majority of those shadow rays
    and deflating PT's OWN reference value by roughly 370x on this exact
    scene (this debt's original `7.3-7.9e-5` measurement IS that
    deflated number; today's healthy PT reads `0.0281`). BDPT's and
    VCM's OWN connection-visibility shadow rays were never meaningfully
    exposed to that bug: both apply a much larger epsilon bump of their
    own (`BDPT_RAY_EPSILON` / `VCM_RAY_EPSILON = 1e-6`, six orders of
    magnitude above the ~1e-12 FP-noise floor debt 21 measured) via
    `Ray::Advance()` before casting
    (`src/Library/Shaders/BDPTIntegrator.cpp` / `VCMIntegrator.cpp`), so
    their absolute output barely moved across the debt-21 fix — BDPT
    read ~0.0253 both before and after. The "100-350x" figure was
    comparing a STABLE BDPT/VCM number against a PT reference that was
    itself broken — the "PT may be the broken one" trap
    [bdpt-vcm-mis-balance.md](skills/bdpt-vcm-mis-balance.md)'s step 0
    pre-flight names, generalised: it wasn't only PT's NEE weight-shift
    symptom (the debt-21 write-up's original framing) that this bug
    produced, it was also a flat multiplicative deflation on a delta-light
    scene with no competing strategy to shift weight toward, which is
    exactly the shape this debt's own measurement had.

    **What this means for the theoretical risk.** `GeometricTerm` still
    has no PRINCIPLED floor beyond the generic `dist < BDPT_RAY_EPSILON`
    / `distSq < 1e-20` checks already in place, so a future full-sphere
    material that reaches a BDPT/VCM connection through a code path
    LACKING an adequate epsilon bump of its own could, in principle,
    still trigger this. That risk is not eliminated architecturally —
    it is simply not what this measurement showed, now that the actual
    culprit (debt 21) is fixed and BDPT/VCM's own epsilon bumps are
    confirmed adequate at this scene's scale.

    **What changed this round.** (a) `AutoRasterizer.cpp`'s Tier-1
    static heuristic (`SceneHasTransmissiveMaterial`) no longer excludes
    `ScattersFullSphere()` materials — it is `CouldLightPassThrough()`
    alone again, so this material class routes like any other
    transmissive material. `tests/AutoRasterizerTest.cpp`'s case is
    flipped to assert VCM routing ("thin weave + point light -> VCM
    (debt 20 resolved)"). (b) The one-time
    `WarnIfBidirectionalRenderHasFullSphereTransmissive` diagnostic and
    its header (`WeaveBidirectionalWarning.h`) are REMOVED — its premise
    no longer holds, and keeping it would mislead authors away from a
    now-correct feature. (c) `tests/FabricRenderTest.cpp::TestBacklitSheerCurtain`'s
    BDPT/VCM comparisons are now ASSERTED (20% tolerance around the
    measured 0.90/0.93, plus a max/mean firefly bound), and a new
    `TestTouchingAreaLitCurtainAllIntegrators` regression locks the
    harsher stress scene at 15% around the measured 1.01/1.01.

    **What did NOT get chased down (RESOLVED 2026-09-04 — see debt 23 below).**
    A smaller, separate ~7-10% BDPT/VCM-under-PT residual remained on the
    delta-point-light backlit scene specifically (it was <2% on the
    mesh-arealight stress scene, stable at both 256 and 1024 spp).
    Root-caused and resolved in debt 23 below (stochastically sampled
    delta gap lobe marking subpath vertex non-connectible in BDPT/VCM).

    `docs/RENDERING_INTEGRATORS.md`'s known-limitations section carries
    the same resolution for readers who start from the integrator side
    rather than the material side.

21. **RESOLVED 2026-09-03 (three diagnosis rounds, then a fourth that found
    the real cause). Originally filed as "under PATH TRACING, the
    diffuse-transmission lobe's contribution scales close to `transmit²`
    instead of linearly in `transmit`." The actual mechanism is a
    GEOMETRY-LAYER shadow-ray self-intersection epsilon bug, not an
    integrator MIS defect — full writeup below; fix landed in
    `RayBilinearPatchIntersection.cpp`.**

    **Original measurement** (isolated probe: a bright window filling the
    frame, curtain directly in front, gap forced to 0 so only the diffuse
    lobe is live): holding `gap = 0` and varying `transmit` alone,
    `transmit = 0.5` gave `0.21x` the `transmit = 1.0` response (not
    `0.5x`), and `transmit = 0.25` gave `0.053x` (not `0.25x`) — under the
    `pathtracing_pel_rasterizer`. The SAME configuration rendered with
    `bdpt_pel_rasterizer` instead scaled EXACTLY linearly: `0.500x` and
    `0.250x`, to three figures. **This specific measurement's numbers
    (`0.21x`) turned out to be dominated by a SEPARATE, unrelated
    artifact** — `file_rasterizeroutput`'s `color_space` parameter
    defaults to `sRGB` even for 32-bit EXR, so a probe that reads
    "linear radiance" from a default-configured EXR is actually reading
    an sRGB-encoded value; the fixed exponent that mistake introduces
    (~0.45) looks exactly like a sub-linear curve and was never load-
    bearing evidence for the real, smaller bug found below. Every
    measurement scene in this section must set `color_space
    Rec709RGB_Linear` explicitly on `file_rasterizeroutput`.

    **Ruled OUT, round 3 (REVIEW_P2R9.md).** A from-scratch
    Scatter()-only Monte Carlo probe (no NEE, no MIS — just `Scatter()` +
    `kray`) on `linen` thin with `gap` forced to 0 measured
    ratio(transmit=0.5)/ratio(transmit=0.25) `= 1.998` and
    ratio(1.0)/ratio(0.25) `= 3.999` — linear — and
    `(value(wi)·|cos|) / Pdf(...)` is CONSTANT (`0.858824`) across
    `transmit` in `{0.25, 0.5, 1.0}`. `SPFPdfConsistencyTest`'s
    full-sphere `Pdf()` integral, `LayeredWhiteFurnaceTest` rows 50-51's
    quadrature, and `SPFBSDFConsistencyTest`'s pointwise
    `kray*pdf == BRDF*|cos|` table all agreed: `WeaveBRDF::value()`/
    `WeaveSPF::Pdf()`/`Scatter()` are exactly linear in `transmit_k`.
    This conclusion **stands**; it correctly pointed at the integrator,
    just not at the layer within it that was actually broken.

    **Round 4 (this fix): re-measured with the EXR colour-space artifact
    removed, on a MESH area light (a `lambertian_luminaire_material`
    behind the curtain, not the point light the earlier rounds used).**
    With `color_space Rec709RGB_Linear` correctly set, PT's response was
    STILL super-linear — `t = 0.1 → 1.0` gave a `46.7x` ratio (not the
    ideal `10x`; exponent ≈ 1.7), while BDPT gave `9.98x` (linear, matching
    the material's own proven linearity). This ruled the EXR artifact out
    as the sole explanation and left a real, smaller, previously-masked
    bug — this round's scratch diagnosis notes have the full reproduction
    trail (EXR colour-space isolation, per-strategy render comparison,
    the mesh-light NEE shadow-ray instrumentation) if a future session
    needs to re-derive it.

    **Mechanism, found by strategy isolation, not by inspection.**
    Forcing PT to use EACH strategy alone and unweighted (NEE-only:
    `w_light = 1`, BSDF-sampled emission suppressed; BSDF-only: the
    reverse) showed NEE-only was linear in `transmit` but read only
    ~10% of BSDF-only's mean — a ~10x gap between two estimators of the
    SAME direct-lighting integral, which is impossible if both are
    unbiased. Instrumenting the mesh-light NEE shadow ray
    (`LightSampler::EvaluateDirectLighting`'s mesh-luminary branch) found
    the cause directly: **93.6% of NEE shadow rays from the curtain
    toward the light behind it were spuriously self-shadowed by the
    curtain's own geometry.** The shadow ray's origin sits exactly on the
    curtain's own `clippedplane_geometry` bilinear patch (it just
    scattered from there); `RayBilinearPatchIntersection`'s self-hit
    rejection compared the computed intersection distance `dRange`
    against the fixed absolute `NEARZERO` (`1e-12`), but `dRange`'s
    numerator (`computet`'s `srfpos - ray.origin`, fed by a `(u, v)` from
    a quadratic solve over the patch's own corner coordinates) carries FP
    round-off that scales with the MAGNITUDE of those coordinates, not
    with machine epsilon in absolute terms. Measured directly on this
    scene (world-scale coordinates of a few units): self-intersection
    `dRange` values of `1e-12` to `3e-12` — straddling `NEARZERO`, so
    roughly half of all self-intersections registered as genuine,
    tiny-positive-distance hits and occluded the light.

    **Why this masqueraded as an MIS bug, and why the delta-light
    measurements in this same debt never caught it.** The (`p_light`,
    `p_bsdf`) and (`bsdfPdf`, `p_nee`) pairs the power heuristic combines
    were independently verified to be numerically IDENTICAL for the same
    direction (same alias-table selection pdf, same `WeaveSPF::Pdf`
    call) — the partition-of-unity `w_light + w_bsdf = 1` held exactly,
    as it algebraically must. What was NOT constant was NEE's REALIZED
    value: spuriously self-shadowed ~94% of the time (a purely geometric
    effect, independent of `transmit`), so NEE under-delivered by a
    roughly fixed multiplicative factor at every `transmit`. As
    `transmit` grows, `bsdfPdf` grows, and MIS correctly shifts weight
    share from NEE toward the (healthy) BSDF-sampling strategy — so the
    combined estimate climbed from "mostly the broken, deflated NEE
    estimator" toward "mostly the healthy BSDF estimator", an artificial
    super-linear curve manufactured by a geometry bug hiding behind a
    real, correctly-computed MIS weight shift. The earlier delta-light
    (`omni_light`) measurements in this file never saw the SUPER-LINEAR
    CURVE symptom: a delta light's NEE row has no competing BSDF-sampling
    strategy (`w = 1` unconditionally — see the MIS_HEURISTICS.md "mental
    model for delta lights" table), so there is no weight-shift to
    distort the curve SHAPE — the same self-shadowing still occurred
    there (documented as a "side finding" in the diagnosis, ~65-70% on
    that scene's specific angles).

    **CORRECTION 2026-09-04 (chip 4 / task_93ff4a8a): this paragraph
    used to end "...but it only added variance/noise to an otherwise-
    correct linear mean, never bias." That is WRONG about the MEAN,
    though right about the CURVE SHAPE.** A fixed self-shadow rate
    applied at every `transmit` value IS a bias on the mean (a roughly
    constant multiplicative deflation) — it just doesn't distort the
    LINEARITY of the transmit-scaling curve, because a constant factor
    cancels in a ratio. Debt 20's own re-diagnosis (§15 debt 20, chip 4)
    measured the actual size of that bias directly:
    `TestBacklitSheerCurtain`'s delta-light PT mean moved from
    `7.3-7.9e-5` (this fix not yet in tree) to `0.0281` (after) — a
    ~370x correction, and the reason debt 20's "BDPT reads 100-350x over
    PT" measurement on that SAME scene was actually PT being the broken
    reference, not a BDPT/VCM defect. The "never bias" claim would have
    hidden exactly this connection had it gone unchecked.

    **Fix**, in `src/Library/Intersection/RayBilinearPatchIntersection.cpp`
    (the shared ray-bilinear-patch solver — used by every
    `ClippedPlaneGeometry` caller AND by `BilinearPatchGeometry`, the
    `bilinearpatch_geometry` chunk area lights use;
    the dead `RayTriangleIntersectionWithDisplacement` stub that also called
    it was deleted 2026-09-06): the self-intersection floor is now
    scale-relative — `tMin = NEARZERO * (1 + coordScale)`, where
    `coordScale` is the largest L1 magnitude among the ray origin and the
    patch's four corners — instead of the bare `NEARZERO`. Fixed ONCE in
    the shared producer (`RayBilinearPatchIntersection` itself) rather
    than in each of `IntersectRay` / `IntersectRay_IntersectionOnly`
    separately, per `docs/skills/precision-fix-the-formulation.md`'s
    "fix it at one of the N callers" anti-pattern (that skill file
    already carries the identically-shaped `RayTorusIntersection` shadow-
    ray-speckle example) — the fix benefits primary/continuation rays
    too, not just shadow rays, even though only the shadow-ray path's
    binary occlusion made the deflation catastrophic enough to be
    visible here.

    **Verified fixed**: with the fix, the same mesh-light scene's PT
    self-shadow rate drops to 0/420 (from 397/420), PT's `t=0.1 → 1.0`
    ratio becomes `9.99x` (matching BDPT's `9.98x` and the material's
    proven-linear `10x` ideal), and PT/BDPT means agree within ~4% at
    every measured `transmit` (`0.1, 0.25, 0.5, 1.0`). Zero regressions
    across `EnvLightBalanceTest` (116/116, unchanged), `HairRenderTest`,
    `HairDirectionalBacklitTest`, `SPFPdfConsistencyTest`,
    `SPFBSDFConsistencyTest`, `LayeredWhiteFurnaceTest`,
    `WeaveMaterialChunkTest`, `ClippedPlaneGeometryTest`,
    `DisplacedGeometryTest`, `GeometryUVRoundtripTest`,
    `GeometrySurfaceDerivativesTest`, `TessellatedShapeDerivativesTest`,
    `GeometryShadingTangentTest`, `RectLightChunkTest`,
    `ShapeLightChunkTest`. Regression guard:
    `tests/FabricRenderTest.cpp::TestAreaLitSheerWeave` (a mesh-lit sheer
    curtain, asserting both PT's linearity in `transmit` and PT/BDPT
    agreement). `scenes/FeatureBased/README.md`'s probe-numbers paragraph
    is corrected to match.

    **Measurement-methodology lesson, for the next diagnosis that sees
    "PT disagrees with BDPT/VCM on an otherwise-clean scene":** the
    power-heuristic partition-of-unity identity (`w1 + w2 = 1` for the
    same `(p, q)` pair evaluated on both sides) is algebraic and cannot
    silently break unless the two sides genuinely evaluate different
    numbers for `p` or `q` — pointwise pdf tracing is the right first
    check, but if it comes back clean, the bias can still be
    manufactured entirely upstream of MIS, by one strategy's REALIZED
    samples being biased (not its weight formula). Isolating each
    strategy alone and unweighted, then comparing their means directly,
    catches that class of bug in one render each; it does not require
    walking a single pdf pair by hand. See also
    `docs/skills/bdpt-vcm-mis-balance.md`'s step 0 (three known non-MIS
    causes when this was written; eight as of 2026-09-05) — this is a fourth.

22. **RESOLVED 2026-09-04 (R8 P1.1) — a `fabric_material` over a
    `transmission thin` `weave_material` extinguished the weave's
    transmission, silently.**

    **Mechanism.** `FabricMaterial::IsSupportedSubstrate` has admitted
    `weave_material` since Phase 2, and P2-B gave a `transmission thin`
    weave two below-horizon lobes (a delta gap pass-through and a
    Lambertian back-face lobe) plus `ScattersFullSphere()` /
    `CouldLightPassThrough()` overrides. Nothing propagated any of that
    through the wrapper. Three independent sites each dropped it:

    - `FabricMaterial` neither overrode nor forwarded the two flags, so
      `LightSampler` never ran full-sphere NEE at a wrapped shading point
      and `AutoRasterizer`'s Tier-1 transmissive signal never saw the
      material;
    - `FabricBRDF::ComputeTerms` returned an invalid (all-zero) record for
      every `n·l <= 0` pair, so `value()` / `valueNM()` were 0 across the
      surface — which is what NEE and every BDPT/VCM connection evaluate;
    - `FabricSPF::ScatterImpl` treated `cos(wo) <= 0` as invalid and zeroed
      the sample's `kray`, killing both the continuum back-face ray and the
      delta gap ray, and `PdfWithParams` returned 0 below the horizon.

    Net effect: a sheen layer over a sheer curtain rendered it **100 %
    opaque**, with no diagnostic. The design intent was the opposite —
    §9.2's own text said "Phase 2's transmission lobe flips [the two
    flags]" — and it simply never happened; that sentence has been
    corrected in §9.2 and §12 rather than left to mislead the next reader.

    **Fix: FORWARD AND MODULATE.** The Charlie lobe is reflection-only, so
    the material originates no transmission of its own; what it must do is
    pass the substrate's through, attenuated by the fuzz layer:

    - the two flags return `base.ScattersFullSphere()` /
      `base.CouldLightPassThrough()`. `IsVolumetric` is deliberately NOT
      forwarded — it means "BDPT must use `kray` instead of BSDF·cos/pdf",
      and `FabricSPF` overwrites every `kray` with exactly BSDF·cos/pdf, so
      claiming it would be a false statement about the estimator;
    - `FabricBRDF` evaluates an opposite-hemisphere pair as
      `f_base(l,v) · scale(l,v)` with the SAME Kulla-Conty product law the
      reflect branch uses, `|n·l|` in place of `n·l` and no sheen term.
      **Both** arms, because `FabricBRDF` is two-sided (it flips to the
      ray-facing normal), so a transmitted path crosses a fuzz layer on
      entry *and* on exit; the same `1/(1 − m·Ē)` normaliser, because the
      adding-doubling series is a property of the layer pair, not of which
      exit the light eventually takes;
    - `FabricSPF` prices a transmit-side continuum sample against the full
      mixture exactly as a reflect sample, with `|cos|`; and reprices the
      substrate's DELTA sample by the two single-crossing arms over this
      branch's selection probability `(1 − w)`, keeping `isDelta` and the
      `pdf = 1` marker untouched.

    **The delta lobe deliberately does NOT carry the `1/(1 − m·Ē)`
    recycling factor**, and that asymmetry is the one judgement call in the
    fix. That denominator is the sum of the multiple-bounce series between
    the fuzz and the substrate — energy the fuzz intercepts, re-scattered
    *diffusely* back down. A delta lobe is a measure-zero direction, so
    none of a diffusely redistributed series lands on it; charging it the
    denominator brightens an aperture above the light that arrived at it
    (measured 1.13× at `alpha` 0.3, normal view, and rising with
    roughness). With the bare two-arm product the wrapper can only
    attenuate a pass-through, which is both the physical statement and a
    closed form: `gap · (1 − m·Ê(n·v))²`.

    **The energy claims, as closed forms rather than locked curves.** The
    weave's diffuse transmission lobe is Lambertian-shaped, so the
    recycling denominator cancels against the *l*-integral of the other arm
    (using `(1/π)∫Ê(μ)μ dω ≡ Ē`, the same identity
    `hemisphericalAlbedo`'s derivation uses twice) and the wrapper's
    **integrated** transmitted share is exactly `bare_T · (1 − m·Ê(n·v))` —
    an attenuation at every view angle. The **per-direction** value is not,
    and must not be: it carries the uncancelled denominator and exceeds 1
    away from grazing, exactly as the reflect side has since round 5. A
    first cut of the render regression asserted the pointwise ratio ≤ 1 and
    failed at 1.028; the assertion was wrong, not the model. Both
    statements are now asserted separately, against the right quantity.

    **Measured** (`LayeredWhiteFurnaceTest` rows 52/53, sheer white linen,
    `gap 0.2`, `transmit 0.25`, white sheen `alpha 0.3`, reflect/transmit/delta
    per exit):

    | θ | bare R/T/δ | wrapped R/T/δ | predicted T | predicted δ (gap-only) |
    |---|---|---|---|---|
    | 0°  | 0.454 / 0.199 / 0.199 | 0.494 / 0.193 / 0.184 | 0.192 | 0.1845 (meas. 0.1855) |
    | 30° | 0.439 / 0.201 / 0.196 | 0.496 / 0.182 / 0.173 | 0.186 | 0.1717 (meas. 0.1717) |
    | 60° | 0.387 / 0.197 / 0.200 | 0.515 / 0.158 / 0.125 | 0.155 | 0.1246 (meas. 0.1243) |
    | 80° | 0.349 / 0.202 / 0.200 | 0.648 / 0.106 / 0.056 | 0.106 | 0.0555 (meas. 0.0553) |

    Wrapped totals stay under the furnace's 1.05 ceiling at every angle
    (0.871 at 0°, 0.810 at 80°). `SPFPdfConsistencyTest`: the wrapped
    continuum density integrates over the FULL SPHERE to
    `w + (1−w)(1−gap)` — 0.814569 vs 0.814694 at 30°, 0.842191 vs 0.842171
    at 60° — and Scatter-vs-Pdf cross-validation is exact to 3e-15 across
    RGB and NM with both the transmit side (≈6 800 draws) and the delta
    branch (≈3 600 draws) live. `SPFBSDFConsistencyTest`: `kray·pdf ==
    value·|cos|` with 0 failures on both wrapped sheer linens at 30°/60°,
    and cross-hemisphere reciprocity exact. `FabricRenderTest` test 7
    (backlit sheer curtain, 32×32, 256 spp, `oidn_denoise FALSE`): wrapped
    `transmission none` is exactly 0, wrapped `thin` reads 0.02887 against
    the bare curtain's 0.02809 (ratio 1.028, inside the closed-form
    supremum `1/(1−Ē(0.65))` ≈ 1.34), BDPT/PT 0.913 and VCM/PT 0.946 —
    inside the SAME bands the unwrapped thin curtain uses, not loosened
    (measured at the 32×32 sheen table; an integrator ratio does not
    depend on the table's absolute values, and round 9's re-run of the
    suite after the 64×64 re-bake kept all 42 checks green).

    **Guards.** `FabricMaterialChunkTest::TestReflectionOnlyUnchanged` (a
    45-row absolute value/valueNM/Pdf table over a Lambertian and a
    `transmission none` satin, captured off the pre-fix binary — the
    "nothing that did not transmit before, moved" lock; the four other
    fabric suites' full outputs are additionally byte-identical
    before/after), `::TestTransmissiveSubstrateForwarding` (the flags, plus
    the transmitted value against an independently re-derived closed form,
    plus opaque negative controls), furnace rows 52/53,
    `SPFBSDFConsistencyTest` Parts D2/E2 wrapped rows,
    `SPFPdfConsistencyTest`'s wrapped full-sphere block, `FabricRenderTest`
    test 7, and `SourceHygieneTest`'s full-sphere claimer census (now 3,
    and taught to recognise a *delegating* claimer — a wrapper that
    forwards the flag would otherwise have slipped it entirely, which is
    the one outcome that census exists to prevent).

    **Audit-by-bug-pattern, one hop out.** The sibling wrapper is
    `coated_material`, whose substrate allowlist does **not** include
    `weave_material`, so the same extinction is unreachable there today; if
    that allowlist is ever widened, `CoatedBRDF`/`CoatedSPF` carry the
    identical opposite-hemisphere early-outs and would need the identical
    treatment. `composite_material` is unaffected (it forwards one
    sub-material's BSDF wholesale rather than gating on hemisphere).

23. **RESOLVED 2026-09-04 — BDPT/VCM ~7–10% under-count residual on delta-light
    backlit thin weave curtain resolved; root-caused as subpath vertex
    connectibility misclassification on mixed delta+continuum materials.**

    **Symptom.** On `TestBacklitSheerCurtain` (flat `weave_material` curtain with
    `fabric linen`, `transmission thin`, `gap 0.1` backlit by an `omni_light` at
    z = -3.0, camera at z = 3.2 looking at origin), BDPT and VCM yielded a stable,
    reproducible ~10% deficit vs Path Tracing (BDPT/PT ≈ 0.900, VCM/PT ≈ 0.930 —
    VCM's own number is the sum of THIS deficit and debt 24's independent
    +3.0 % over-count, which is why it never sat on 0.900).
    The deficit scaled directly with the weave gap:
    - At `gap 0.1` (linen default): BDPT/PT = 0.900 (-10%)
    - At `gap 0.2`: BDPT/PT = 0.800 (-20%)
    - At `gap 0.0`: BDPT/PT = 1.000 (0% error)

    **Mechanism.** In `src/Library/Shaders/BDPTIntegrator.cpp` (both in
    `GenerateEyeSubpathImpl` and `GenerateLightSubpathImpl`), subpath vertex
    connectibility was determined solely by checking whether the `scattered`
    container contained a non-delta ray:
    ```cpp
    bool hasNonDelta = false;
    for( unsigned int i = 0; i < scattered.Count(); i++ ) {
        if( !scattered[i].isDelta ) { hasNonDelta = true; break; }
    }
    vertices.back().isConnectible = hasNonDelta;
    ```
    For mixed delta + continuum materials such as `WeaveMaterial` (`transmission thin`,
    `gap > 0`), `WeaveSPF::ScatterImpl` has a single-ray budget and stochastically
    samples either the delta gap lobe (with probability `gap`) or the continuum yarn
    lobe (with probability `1 - gap`).

    Whenever the delta gap ray was selected, `scattered` only contained a single
    delta ray (`isDelta = true`), causing `hasNonDelta` to evaluate to `false`.
    This marked the surface vertex `isConnectible = false`, completely dropping
    Next Event Estimation (NEE, $s=1$) at that vertex on a `gap` fraction of
    camera rays.

    On the remaining $1 - \text{gap}$ fraction of camera rays where NEE did fire,
    `brdf.value()` additionally multiplied by the yarn fraction `p.available = 1 - gap`.
    Hence, BDPT received $(1 - \text{gap}) \times (1 - \text{gap}) = (1 - \text{gap})^2$,
    creating an exact systematic deficit of $1 - \text{gap}$ vs PT's $(1 - \text{gap})$.
    For `gap = 0.1`, $(1 - 0.1)^2 / (1 - 0.1) = 0.900$, exactly matching the
    measured 10% deficit.

    **Fix.** `isDelta` is a property of a sampled continuation *ray/lobe*, but
    `isConnectible` is a property of whether the *surface* has continuous scattering
    capability (`pMaterial->GetBSDF() != nullptr`). In `BDPTIntegrator.cpp`,
    surface vertices with a valid BSDF are now marked connectible:
    ```cpp
    bool hasNonDelta = false;
    for( unsigned int i = 0; i < scattered.Count(); i++ ) {
        if( !scattered[i].isDelta ) { hasNonDelta = true; break; }
    }
    if( ri.pMaterial->GetBSDF() ) {
        hasNonDelta = true;
    }
    vertices.back().isConnectible = hasNonDelta;
    ```

    **Results** (immediately after this fix, i.e. before debt 24 — see there for
    the final numbers).
    - Original scene (`gap 0.1`) across 5 seed bases: BDPT/PT mean = 0.999998
      ($\sigma = 0.000053$), max pixel ratio = $1.0000 \pm 0.001$. VCM/PT = 1.0296
      on this host / 1.0369 on the reviewer's — debt 24, not a residual of this one.
    - Variant `gap 0.2`: PT = 0.0249685, BDPT = 0.0249691 (BDPT/PT = 1.00003;
      was 0.800 pre-fix).
    - Variant light at $z = -0.5$: BDPT/PT = 1.00002.
    - Variant light at $z = -30.0$: BDPT/PT = 0.999985, VCM/PT = 1.00033.
    - Variant `gap 0.5`: BDPT/PT = 0.99997.
    - Variant 0.01-radius emissive sphere read BDPT/PT = 0.990963 and was filed as
      a −0.9 % residual "at a 5e-5 noise floor, not noise". **That reading was a
      noise-floor mistake**: 5e-5 is the floor for the DELTA-lit curtain, whose
      image is near-deterministic NEE; with an AREA light seen through a 10 % gap
      the floor is ~1 %. Re-measured on the area-light topology over 5 runs:
      BDPT/PT 1.00133 ± 0.0104 at 256 spp, tightening to 0.99957 ± 0.0023 at
      2048 spp — the disagreement **shrinks with sample count**, so it is
      variance, not bias. Guarded now by
      `FabricRenderTest::TestGappedWeaveWithAreaLight` (§ debt 26 for why it is
      not in `BDPTStrategyBalanceTest`).
    - Regression topology E (`TestBacklitThinCurtain`) added to
      `tests/BDPTStrategyBalanceTest.cpp`; the area-lit companion added as
      `tests/FabricRenderTest.cpp::TestGappedWeaveWithAreaLight`.

    **Sibling sites audited (`docs/skills/audit-by-bug-pattern.md`).** The bug
    pattern is *"a per-DRAW flag (`isDelta`, the lobe this walk sampled) used to
    answer a per-SURFACE question (can this vertex be connected to?)"*. Three
    siblings were enumerated and resolved with it:
    - `GenerateEyeSubpathImpl` and `GenerateLightSubpathImpl` surface vertices —
      the reported site, fixed above (both twins, RGB and NM share the template).
    - **MEDIUM vertex connectibility**, one hop downstream, in the same two
      functions: a medium vertex inside an object was marked non-connectible when
      `prev.type == SURFACE && prev.isDelta`. "Enclosed by a specular boundary" is
      a property of the boundary *material*, so a mixed boundary (weave gap,
      polished coat, Fresnel composite) turned the same medium vertex connectible
      or not depending on the draw. Now tests `!prev.isConnectible`, the same
      per-surface predicate. Image-level guard landed once debt 25 closed:
      `tests/FabricRenderTest.cpp::TestMediumVertexBehindGappedWeave`
      (keyword `mediumvertex`) — a parity guard only; its red-proof could
      not make the predicate flip move the ratio beyond noise (see §15
      debt 25).
    - `MISWeight`'s `vi.isDelta` / `vj.isDelta` skip rules — **confirmed correct
      as-is**, not changed: those walk the *realised* pdf ratios of the sampled
      lobes, and the connection vertices themselves are already overridden by the
      per-surface `isConnectible` in the "temporarily clear isDelta on the two
      connection vertices" block above them.

    **`CompositeMaterial` side effect (measured).** `CompositeMaterial::GetBSDF()`
    returns `top.GetBSDF()` if non-null else `bottom.GetBSDF()`, so a
    Fresnel-blend composite of a *delta* top (dielectric / perfect reflector,
    whose `GetBSDF()` is null) over a continuum bottom reports the bottom's BSDF —
    non-null. Under the old predicate such a composite was marked non-connectible
    on every draw that took the top's delta lobe, i.e. it was under-counted by
    exactly the same mechanism; the same one-line fix covers it. Same for
    `polished_material` (specular coat over a substrate BRDF) and
    `subsurfacescattering_material` (delta reflect/exit rays over a BSDF).

24. **RESOLVED 2026-09-04 — VCM read +3.0 % over PT on EVERY delta-light scene
    (the residual debt 23's fix exposed, and a partition-of-unity bug in its own
    right, not fabric-specific).**

    **Symptom.** With debt 23 fixed, BDPT/PT on the backlit gapped curtain landed
    on 1.0000 but VCM/PT sat at **1.0296** with VM off and **1.0370** with VM on
    (machine-stable; a reviewer on another host read 1.0369, this host 1.0296 —
    the ~0.7 pp is the `RasterizeDispatchers.h` worker-seed race, not seed noise).
    The debt-23 round widened `kThinCurtainVcmPtTol` to 0.05 to cover it.

    **It has nothing to do with fabric, the gap, or debt 23.** Measured on the
    same harness, 32×32, 256 spp, `oidn_denoise FALSE`, VCM/PT:

    | scene | VCM/PT before | VCM/PT after |
    |---|---|---|
    | weave `transmission thin`, gap 0.0, omni behind | 1.02964 | 0.99981 |
    | weave `transmission thin`, gap 0.1, omni behind | 1.03704 | 0.99990 |
    | weave `transmission thin`, gap 0.2, omni behind | 1.03693 | 0.99993 |
    | weave `transmission thin`, gap 0.5, omni behind | 1.03673 | 1.00004 |
    | **plain Lambertian quad, omni in front** | **1.02988** | **1.00001** |
    | plain Lambertian quad, mesh area light | 1.00000 | 1.00000 |

    A bare Lambertian quad under an omni light carries the identical +2.99 %; an
    area light carries none. The gap only ever changed which *sub-*strategies were
    active, never the sign or the mechanism.

    **Mechanism.** `VCMIntegrator::EvaluateNEEImpl` zeroed **both** MIS
    alternatives when `ls.isDelta`:
    ```cpp
    const Scalar wLight = ls.isDelta ? 0 : bsdfDirPdfW / ( lightPickProb * directPdfW );
    Scalar wCamera = 0;
    if( !ls.isDelta && directPdfW > 0 && cosAtLight > 0 ) { /* ... */ }
    ```
    Zeroing `wLight` is right: BSDF sampling at the receiver cannot land on a
    Dirac *position* by chance. Zeroing `wCamera` is **wrong**: `wCamera` is the
    relative density of the strategies that reach the same path from the *light*
    side — the t=1 light-tracing splat above all — and those do not have to "land
    on" the light vertex, they **start** there and sample an emission
    **direction**, which has an ordinary solid-angle density (`PointLight` returns
    1/4π, `SpotLight` the cone density) even when the position is a delta. So NEE
    took weight 1 while `SplatLightSubpathToCameraImpl` independently contributed
    its own `1/(1+wLight)` share of the very same path.

    Instrumented partition on the gap-0.1 curtain (mean MIS weight per strategy,
    32×32 × 256 spp; the sums are over slightly different path populations, hence
    the residual 9e-4 after):

    | strategy | mean weight before | mean weight after |
    |---|---|---|
    | s=1 NEE | 1.00000 | 0.96312 |
    | VM (merge) | 0.00815 | 0.00816 |
    | t=1 light-tracing splat | 0.02963 | 0.02963 |
    | **Σ** | **1.03778** | **1.00092** |

    Σ = 1.0378 is the +3.7 % exactly. At gap 0 (VM disabled by the pre-pass) it is
    1 + 0.02985 = 1.0299 — the +2.99 % exactly.

    **Fix.** Compute `wCamera` for delta lights too, from a `camFactor` written as
    the emission-direction **area** density rather than in the SmallVCM
    `emissionPdfW_geom / (directPdfW · cosAtLight)` spelling:
    ```cpp
    if( emissionDirPdfSA > 0 && distSq > 0 ) {
        const Scalar camFactor = ( emissionDirPdfSA * cosAtEye ) / distSq;
        wCamera = camFactor * ( norm.mMisVmWeightFactor
                              + eyeMis[i].dVCM
                              + eyeMis[i].dVC * bsdfRevPdfW );
    }
    ```
    The two forms are the *same number* whenever `directPdfW` is the honest
    area→solid-angle conversion `pdfPosition · dist² / cosAtLight` (the
    `pdfPosition` and `cosAtLight` factors cancel), so non-delta lights — env
    included — are algebraically unchanged. But the old spelling divides by two
    quantities that are **placeholders** for a delta light (`directPdfW := 1`,
    `cosAtLight := 1`), which is why it could not simply be un-gated. The new form
    touches neither and needs no `isDelta` branch.

    That it closes the partition is not just measured, it is exact: with
    `dVCM_eye = N / p_cam→A(x)` the product `camFactor · dVCM_eye` is the
    reciprocal of the splat's own `wLight = (p_cam→A / N)·(vmFactor + dVCM_light)`,
    and the merge's `wLight`/`wCamera` complete the same three-way ratio set, so
    NEE + splat + merge sum to 1 by construction.

    Lights with no emission-direction sampling at all — `DirectionalLight` and
    `AmbientLight`, both of which return `pdfDirection() == 0` and from which VCM
    never emits photons — fall out at `camFactor = 0`, i.e. NEE keeps weight 1,
    which is correct because no competing light-side strategy exists.

    **Results.** Every delta-lit row above lands on 1.000 ± 3e-4, with VM on or
    off. `FabricRenderTest` curtain, 5 seed bases: BDPT/PT mean 1.000007
    (σ 4.2e-5), VCM/PT mean 0.999804 (σ 4.5e-5); `kThinCurtainVcmPtTol` goes
    0.05 → **0.01** and `kThinCurtainBdptPtTol` 0.02 → 0.01.
    `VCMStrategyBalanceTest` topology A (delta omni) improves from VCM/PT 1.0082
    to 0.9975. `EnvLightBalanceTest` 116/116 with **no band moved** — its
    topology-E VCM mean ratio reads 1.2362 against the recorded 1.2365 centre,
    because that scene's omni term is only 2.4 % of an image dominated by the
    +24 % env bias. `scenes/Tests/VCM/pool_caustics_vcm` (two spot lights) drops
    9.3 % (0.1493/0.1503 → 0.1371/0.1357) — the removed over-count — with its
    auto-radius decision byte-identical (`segments=39429 median_segment=2.86251
    effective_radius=0.0286251`).

    **Why the auto-radius pre-pass was NOT changed.** The reviewer who found this
    residual attributed it to `VCMRasterizerBase::PreRenderSetup`'s `foundSpecular`
    predicate (`curr.isDelta && curr.type == SURFACE`) turning VM on because of the
    gap lobe. That is a true observation about the predicate but not the cause of
    the bias: VM accounts for only 0.7 pp of the 3.7 pp, VM-off still read
    +2.96 %, and gap-0 (VM disabled) read +2.96 % as well. With `wCamera` fixed,
    VCM is unbiased with VM **on** (0.99990 at gap 0.1), so whether the weave
    enables VM is a cost question, not a correctness one. Two candidate
    replacements were considered:
    - *"the material is PURELY delta" (`GetBSDF() == 0`)* — **rejected**: it would
      disable VM for `polished_material`, `subsurfacescattering_material` and
      dielectric-over-diffuse `composite_material`, all of which have a non-null
      BSDF **and** make real caustics.
    - *"the material has ANY delta lobe"* — the right property, and what the
      existing loop already estimates: it is a Monte-Carlo test over
      width × height × `lightSubpathsPerPixel` light subpaths, so a lobe of
      probability p is detected with probability 1 − (1−p)^N, exact for any p a
      scene uses. Making it exactly per-surface needs a new virtual on every
      `ISPF`, changes no scene's VM decision, and has no measured benefit —
      declined, with the reasoning recorded at the site.

    *Still open after debt 24 (2026-09-05 review).*
    `tests/VCMStrategyBalanceTest.cpp` topology C (a MIXED delta + mesh
    light scene) reads VCM/PT ≈ 0.9635, unchanged by this fix (0.9645 →
    0.9643) and inside its 8 % band. Debt 24's closure is exact for a
    pure delta light and a pure non-delta light; a mixed-light scene is
    where a residual partition wrinkle would show — most likely in the
    light-SELECTION pmf's interaction with the two light classes'
    different `dVC`/`dVCM` seeding. Not diagnosed; recorded so it is not
    rediscovered as a regression.

25. **CLOSED 2026-09-05 — a CLOSED solid whose material is a
    thin-transmissive weave read BDPT/VCM ≈ 0.17 × PT while free-standing
    weave planes did not. Root cause: `box_geometry`'s own self-hit root;
    PT was the broken reference, not BDPT/VCM.**

    *Review handoff (2026-09-05), kept as the historical record — each
    hypothesis marked against what the fix found.* Two facts narrowed it.
    (a) The gap-0 data point RULES OUT a debt-23-style per-draw mechanism:
    at gap 0 no delta lobe is ever drawn, so connectibility was
    unconditional even before the fix, and the number was taken after
    debts 23 and 24 landed. This is a different bug. **Confirmed**: the
    cause was a geometry-primitive self-hit, not an integrator MIS defect.
    (b) Two and three free-standing parallel planes deviate only 1–5 %, so
    "a shadow ray crosses a second transmissive surface" is not it either —
    those cross the same surfaces. **Confirmed** as far as it goes: a
    multi-surface transmissive chain was never the trigger of the 83 %
    deficit. (The "1–5 %" itself is superseded: two free-standing planes
    at gap 0.1 read BDPT/PT 1.277 once re-measured — debt 27 below, the
    separate PT gap-lobe blind spot the box leak was masking.) What
    differs is that
    `box_geometry` is an ANALYTIC SOLID: `BoxGeometry::IntersectRay` takes
    explicit front/back-face flags and tracks `RayBeginsInBox`, unlike an
    assembly of independent double-sided planes.
    - *Hypothesis: BDPT/VCM's subpath continuation after a transmissive
      bounce on a solid does not track "which solid I am now inside" the
      way PT's own walk does, so the box's inside-aware exit-face
      selection is mishandled for a bidirectional walk.* **Refuted.**
      BDPT/VCM were never the problem: they `Advance()` every shadow and
      continuation ray by 1e-6 and so never saw the self-root PT's
      unadvanced NEE shadow ray did.
    - *Hypothesis: a verified contributing asymmetry of the same class —
      BDPT/VCM visibility uses the binary `RayCaster::CastShadowRay` while
      PT alone uses `CastShadowRayTransmittance` (RayCaster.cpp ~1980,
      gated to perfect-specular dielectrics, so probably not engaged
      here).* **Refuted** by its own hedge: not engaged, since
      `weave_material` is not a perfect-specular dielectric. The asymmetry
      that mattered was NEE's unadvanced shadow ray, not which shadow-ray
      function was called.
    - *Discriminating render proposed at the time: the same sealed cube
      hand-built from six `clippedplane_geometry` quads (`doublesided
      TRUE`) with the same weave — if BDPT/PT is ≈ 1 there, the bug is
      `box_geometry`'s solid representation, not closed topology in
      general.* **This is exactly the render that settled it** — see
      Results below.

    Found while trying to build the medium-vertex regression debt 23's sibling
    audit calls for (that requires an `interior_medium`, which requires a closed
    solid). Measured, 24×24, 512 spp, `oidn_denoise FALSE`, omni light behind,
    `weave_material { transmission thin, gap g }` (kept as the pre-fix baseline):

    | scene | PT | BDPT/PT | VCM/PT |
    |---|---|---|---|
    | 1 free-standing weave plane | 0.0371877 | 1.00002 | 0.99991 |
    | 2 parallel weave planes | 0.00648917 | 0.98855 | 0.98817 |
    | 3 parallel weave planes | 0.00116805 | 1.05328 | 1.04553 |
    | `box_geometry`, weave material, gap 0.0 | 0.0333635 | **0.17349** | **0.17341** |
    | `box_geometry`, weave material, gap 0.3 | 0.0223869 | **0.81405** | **0.81845** |
    | same box + scattering `interior_medium`, gap 0.0 | 0.0333826 | 0.17293 | 0.17330 |
    | same box + scattering `interior_medium`, gap 0.3 | 0.0223894 | 0.81219 | 0.81430 |

    **Root cause.** A shadow / continuation ray whose origin is a hit point
    `Object::IntersectRay` published from THIS box sits ~1e-12
    (`SURFACE_INTERSEC_ERROR`, object-local) off the face it just hit, so
    `RayBoxIntersection` reports a tiny, direction-dependent self-root
    `t ≈ 1e-12 · |cos θ_in| / |cos θ_out|` as the PRIMARY hit. That root
    straddles `NEARZERO`, and `IntersectRay_IntersectionOnly`'s "reject the
    primitive when `dRange < NEARZERO`" then produced both failure modes on
    the same closed box: under `NEARZERO` the WHOLE box vanished for the
    shadow ray (light behind the box lit the front face as if the back face
    were not there, PT 3.5× too bright with the light outside); over it the
    box occluded its own origin face (a light inside the box self-shadowed
    every NEE, PT 0.44× too dim with the light inside). Path tracing casts
    NEE shadow rays unadvanced from the published hit point
    (`LightSampler.cpp` ~1839, `Ray rayToLight( ri.ptIntersection, vToLight
    )`) and saw both failure modes; BDPT/VCM `Advance()` theirs by 1e-6
    (`BDPT_RAY_EPSILON` / `VCM_RAY_EPSILON`) and saw neither — which is why
    they agreed with each other, with PT on the same six faces built from
    `clippedplane_geometry` quads, and not with PT on `box_geometry`. PT's
    own continuation rays Advance 1e-8 and were fine; only its NEE shadow
    ray was hit. This is the FOURTH instance of
    `docs/skills/bdpt-vcm-mis-balance.md` step 0's "PT may be the broken
    one" family (after IOR seeding, debts 20/21's bilinear self-hit, and debt
    26's legacy rasterizer) — see that skill's new cause 7.

    **Fix.** `BoxGeometry::DropSelfHitRoot`
    (`src/Library/Geometry/BoxGeometry.cpp`, commit `40e78b69`), called
    from both `IntersectRay` and `IntersectRay_IntersectionOnly`: a root
    belonging to a face the origin lies on (per axis, `|origin.axis − bound|
    ≤ 4·NEARZERO + 64·DBL_EPSILON·|origin.axis|` since review round 2 —
    NOT the coordinate-summed floor `RayBilinearPatchIntersection`'s
    debt-21 fix uses, which the fix commit first mirrored; the "Review
    rounds 1–2 follow-ups" below and
    `docs/skills/precision-fix-the-formulation.md` say why that pattern
    does not transfer to a box) is dropped in favour of
    the other root, mirroring `RaySphereIntersection` (which already skips
    roots at the origin and returns the next one) but as a plane-distance
    test rather than a range threshold. The survivor is then treated as an
    EXIT hit for the front/back-face flag rule, which also replaces the
    strict-inside early-out that rejected the far face for an on-face
    origin under back-faces-only.

    **Results** (24×24, 256 spp, `oidn_denoise FALSE`, omni light power 6,
    camera `0 0 3.2` → origin fov 34, `weave_material { fabric custom,
    transmission thin, gap g, warp_transmit 0.25, weft_transmit 0.25 }`,
    `box_geometry 2.8 × 2.8 × 1.0` at the origin; "six planes" is the same
    six faces as `clippedplane_geometry doublesided TRUE` quads; light
    outside is `0 0 -3`, light inside is `0 0 -0.2`):

    | scene | PT | BDPT/PT | VCM/PT |
    |---|---|---|---|
    | box gap 0, light outside, before | 0.04920 | 0.284 | 0.283 |
    | box gap 0, light outside, after | 0.01435 | 0.967 | 0.967 |
    | six planes gap 0, light outside | 0.01417 | 0.988 | 0.985 |
    | single plane gap 0 | 0.04801 | 1.000 | 1.000 |
    | box gap 0, light inside, before | 0.33986 | 2.245 | 2.248 |
    | box gap 0, light inside, after | 0.77381 | 0.987 | 0.988 |
    | six planes gap 0, light inside | 0.77374 | 0.987 | 0.989 |
    | box gap 0, camera inside (z=0.2, looking −z), after | 0.09107 | 1.003 | 1.004 |
    | six planes gap 0, camera inside | 0.09113 | 1.004 | 1.004 |

    **The discriminating render the review handoff proposed is what settled
    it.** The sealed cube built from six free-standing
    `clippedplane_geometry` quads reads within 1–2 % of the fixed box at
    every topology above (light outside, light inside, camera inside) — the
    bug was `box_geometry`'s solid representation, exactly as hypothesised,
    and not closed topology in general.

    **Note absolute values differ from the table above.** This table's PT
    values use a different box size and weave preset than the original
    review handoff's; the RATIOS are what carry the finding.

    **Review rounds 1–3 follow-ups (2026-09-05).** The self-hit band is
    a test on the face PLANE, not on provenance, so a deliberate standoff
    that wants to re-hit the face it stands off from reads as that face's
    own published point: `CSGObject::AdoptCsgExitFacePayloadViaProbe`'s
    1e-12 probe margin fell inside the band for every `box_geometry`
    operand and silently took its graceful entry-payload fallback on
    every box exit face (round 1, P2; `tests/CsgSurfacePayloadTest.cpp`
    Tests 4/11/15 were red at the fix commit — a suite the fix's gate
    list had not included). Round 1's first repair — a CSG floor of
    `8 × NEARZERO × (1 + |ptExit|)` — was itself a P1 in round 2: the
    L1-coordinate term is exactly the transverse coupling that suite's
    Test 14 guards (an assembly at world X = 1e12 turned the margin into
    eight world units and adopted a decoy face), and the box's own band
    (which then summed the half-extents) made the floor insufficient for
    operands wider than ~20 units or scaled up. Final formulation of THIS
    round, both sides coordinate-free (superseded for non-box operands by
    the 4b141ad3 follow-up further down, which adds the operand's own
    root floor as a second term): the box band is per axis,
    `eps = 4 × NEARZERO + 64·DBL_EPSILON × |origin.axis|` (the plane
    distance is an exact subtraction, so only that coordinate's rounding
    matters); the CSG probe's floor is that band doubled and divided by
    the exact operand-local plane-distance rate of a world-unit standoff,
    `2·eps / max(|M⁻¹dir · n̂_local|, |M⁻¹dir|/20)` (round 3 replaced the
    factorised `|M⁻¹dir| · |dir·n|`, which under-estimates the rate by
    the stretched axis's factor on an anisotropic operand — a slab
    stretched 4× along its exit normal and hit obliquely already fell
    back; no rotation needed, and a cube cannot show it since its exit
    face is then the max-|M⁻¹dir| axis) — 8e-12 at unit
    scale and normal incidence, acceptance window ~1.7e-11, ~100× under
    the ~2e-9 decoy radius that suite's Test 15 pins (the window scales
    with 1/rate: ~3e-10 at the grazing clamp, and with the operand's own
    scale). `CsgSurfacePayloadTest` is now in the gate (at this round the binary
    reported 299 checks passed with 298 static `Check(` sites; the
    4b141ad3 follow-up below adds Test 27, 299 → 332, and later rounds
    add more — the binary's own count is authoritative —
    including the round-3 scaled-operand and oblique-exit cases and the
    round-4 stretched-slab + oblique case, Test 26, which is the only one
    that goes red under the old `|M⁻¹dir| · |dir·n|` factorisation) and
    pins the CSG side in both directions; `tests/BoxGeometryTest.cpp::RunStandoffReentryContract`
    pins the box side (a 2·eps/|cos| standoff re-hits its face, a
    0.5·eps one reads as the origin's own face) at unit and 1000× scale.
    The promoted root publishes `range2 = 0`, the `RaySphereIntersection`
    / CSG inside-sentinel convention, instead of carrying the ~1e-12
    self-root as an exit BEHIND the entry — and so, since round 3, does a
    STRICTLY interior origin, whose negative entry root `Object` used to
    turn into a positive "exit behind the entry" that `CSGObject`'s
    interval ordering read as a phantom interior wall (a `CSG_UNION` of
    two overlapping boxes seen from inside one showed a wall where two
    spheres in the same construction do not; pre-existing, the direct
    sibling of the promoted-root case; `vNormal2` keeps naming the entry
    face behind the origin, pinned in the same test). Not pinned by any
    test: the probe's new non-finite `range2` guard (a triangle-mesh
    operand publishes IEEE inf, which the `RISE_INFINITY` compare misses)
    — mesh-in-CSG is separately broken (`range2` NaN), so a regression
    there would only trade one graceful fallback for another. Two
    measure-zero limits of the
    plane test are recorded in the helper's comment (a ray within ~1e-9
    rad of parallel to its own face; an unrelated object's exactly
    coplanar face — e.g. a glass box resting on a box reports the lower
    box's far face for a ray leaving the contact face, a first-hit
    shading error, never a shadow leak), as is the pre-existing
    every-primitive limit that a box translated ≳ 1e4 from the world
    origin round-trips its published point outside any local-frame band.
    **Sibling audit, closed 2026-09-05.** The prior "unverified suspects"
    state above is superseded: a re-audit through the REAL `Object` /
    `LightSampler` path (not the direct-construction probe) confirms the
    self-hit-floor bug class reproduces on every quadric-family sibling at
    large world coordinates, and on every flat- and quadric-family sibling
    whose shadow ray can CROSS the surface, and closes both (a8bef210) by
    applying the same scale-relative floor `RayBilinearPatchIntersection`'s debt-21 fix
    uses (`tMin = NEARZERO × (1 + coordScale)`) at each PRODUCER rather than
    a caller: `RaySphereIntersection` (both overloads; `coordScale = |origin|₁
    + radius`), `RayQuadricIntersection` (`|origin|₁`), `RayPlaneIntersection`
    (`|origin|₁`; serves `infiniteplane_geometry` and `circulardisk_geometry`),
    `RayTriangleIntersection` (`|origin|₁ + |vertex|₁`; serves both
    triangle-mesh classes and the displaced-mesh path), and both cylinder
    paths (`CylinderGeometry::IntersectCappedSolid`, `|origin|₁ + radius`;
    `RayX/Y/ZCylinderIntersection`, same). `box_geometry` keeps its own
    per-axis plane-distance band (above) — a plane-distance fact is
    available there and not for a solved root; the two mechanisms are
    siblings, not duplicates.

    *Two independent triggers, not one.* (i) A shadow ray that CROSSES the
    surface — any full-sphere-transmissive material (`weave_material
    { transmission thin }` and its kin) lit from behind, at ANY coordinate
    scale, on ANY primitive whose intersection routine can return a root at
    the origin's own published point. (ii) Coordinates ≳ 1e3 on a
    QUADRIC-family primitive (sphere, ellipsoid, cylinder), even for an
    opaque Lambertian material lit from the camera side, where the eye
    hit's own range carries enough absolute round-off (~1e-12 at this
    scale) to publish a point on the wrong side of the surface outright —
    a double-precision emulation of `RaySphereIntersection` on the
    published point (a scratch Python script, not in the tree: 20,000
    camera rays from (0, 0, 4000) at a radius-1000 sphere, light at
    (4000, 0, 1200), the point published exactly as `Object::IntersectRay`
    does, the quadratic solved exactly as the routine does) found 16 % of
    points landing INSIDE the sphere and 8.8 % of NEE rays self-hitting,
    with self roots up to 6.6e-9 at the most grazing exits. Flat-family
    primitives are far less exposed to trigger (ii) — a flat surface has
    no curvature for round-off to punch through, so plane, disk, box,
    torus and the patches read 1.000 at 1000× — but a triangle MESH is
    not immune: the PLY quad read 1.005 and a tessellated sphere 1.029
    (1.076 with a small displacement) — a 0.5–3 % effect at zero
    displacement, 7.6 % with a small one — which the same floor also
    closes. Trigger (i) engages every primitive, at whatever
    coordinate scale the crossing geometry demands (for a
    full-sphere-transmissive material, every scale, including unit).

    **Trigger (ii) sweep** — Lambertian, omni light, camera oblique,
    48×48/256 spp, BDPT/PT, unit scale vs 1000× (light power ×1e6):

    | primitive | unit scale | 1000× |
    |---|---|---|
    | sphere | 1.000 | 1.609 (grazing) / 1.490 (overhead) |
    | ellipsoid | 1.000 | 1.447 / 1.328 |
    | capped / open cylinder | 1.000 | 1.015–1.023 |
    | torus | 1.000 | 1.000 |
    | circular disk | 1.000 | 1.000 |
    | infinite plane | 1.000 | 1.000 |
    | clipped plane | 1.000 | 1.000 |
    | box | 1.000 | 1.000 |
    | PLY quad mesh | 1.000 | 1.005 |
    | bilinear patch | 1.000 | 1.000 |
    | Bezier patch | 1.000 | 1.000 |
    | SDF sphere | 1.000 | 0.995–1.004 |
    | displaced tessellated sphere (disp 0.02·r) | 1.000 | 1.076 (disp 0 control: 1.029) |

    Mechanism confirmed two ways: advancing PT's NEE shadow ray 1e-6 in a
    scratch build took the sphere cell from 1.61 to 1.0006 (the same
    `Advance()` convention BDPT/VCM already use); and the double-precision
    emulation above independently explains the same figure from the
    producer side. Angle-independent — grazing and overhead read within
    10 % of each other — so this is scale/rounding, not trigger (i)'s
    grazing straddle.

    **Trigger (i) sweep** — the debt-25 scene family (`weave_material
    { fabric custom transmission thin gap 0.0 warp_transmit 0.25
    weft_transmit 0.25 }`, omni light behind at `0 0 -3` power 6, camera
    `0 0 3.2` fov 34, 24×24/256 spp), BDPT/PT before → after the floor:

    | primitive | before | after |
    |---|---|---|
    | box (control, debt 25) | 0.966 | 0.967 |
    | clippedplane (control, debt 21) | 1.000 | 1.000 |
    | sphere | 3.12 | 0.98 |
    | ellipsoid | 2.80 | 1.01 |
    | capped cylinder (axis x) | 2.19 | 0.96 |
    | open tube (side-on) | 2.82 | 0.96 |
    | open tube (end-on) | 0.995 | 0.995 (clean both) |
    | circular disk | 393 | 1.000 |
    | infinite plane | 354 | 1.000 |
    | PLY two-triangle quad | 157 | 1.000 |
    | displaced sphere | 3.21 | 0.98 |
    | bilinear patch | 1.000 | 1.000 (shares the debt-21 solver) |
    | flat Bezier patch | 1.000 | 1.000 (flat case only; a curved patch was not tested) |
    | SDF sphere | — | 0.93 (clean; ray-marched, scale-relative epsilon) |
    | torus | 0.894 | 0.855 (NOT fixed — see below) |

    *Light INSIDE the closed shells* (unit scale): sphere 0.990, ellipsoid
    0.990, cylinder 1.095, displaced 0.982, torus 1.007 — clean. *At 1000×,
    light behind, before → after*: sphere 1.061→0.932, ellipsoid
    1.225→0.944, cylinder 1.174→0.943, displaced →0.943, disk/plane/mesh
    →1.000 — a residual ~6 % PT-OVER on the closed round shells at 1000×
    (vs the box's own ~3 % residual at unit scale, above), left OPEN.
    Lambertian 1000× after the floor: sphere 1.0003, ellipsoid 1.0014,
    unit sphere 0.9998.

    **Torus: reverted, left open.** The torus's quartic already deflates
    an exactly-on-surface root (a relative 1e-10 test on the quartic's
    leading coefficient `C[4]`); adding the scale-relative floor to its
    remaining root gate did nothing at unit scale and made the 1000× cell
    slightly WORSE (0.846 → 0.792 in the scratch build that carried it,
    beyond the ~0.005 noise floor), so it was reverted. The torus's mild
    PT-OVER residual (0.89 unit / 0.85 at 1000×, both light-behind; clean
    with the light inside) is recorded as open, mechanism unknown —
    probably the deflated cubic's conditioning, not the same self-hit-floor
    bug class as its siblings.

    **Consequence for the fabric workstream.** Before this fix, any
    `weave_material { transmission thin }` (or other full-sphere-
    transmissive material) on a triangle MESH, a `circulardisk_geometry`,
    or an `infiniteplane_geometry`, lit from behind, was almost completely
    self-shadowed under PT (1/157 to 1/393 of the right answer) while a
    `clippedplane_geometry` twin was fine — every mesh curtain or cloth
    panel lit from behind was affected, not just the closed-solid case
    above.

    **Review follow-up (2026-09-05, 4b141ad3) — the CSG exit-face probe must ask
    the operand for its floor.** The producer floors above are RANGE
    floors of size ~1e-12·(1 + |o|₁ + size), while
    `CSGObject::AdoptCsgExitFacePayloadViaProbe`'s standoff was derived from
    the box's plane-distance band (~8e-12 at unit scale): once a sphere
    operand is larger than ~3.5 units (ellipsoid ~8, cylinder ~8) the probe's
    re-entry root fell under the operand's own floor, the probe missed, and
    a carved cavity's far wall silently took the near wall's UV, derivatives
    and tangent (measured: box half-extent 20 minus a sphere of radius 4, the
    far wall reported the ANTIPODAL UV; radius 1–3 fine; pre-floor fine at
    every radius). Fix: a `SelfHitRootFloor(localOrigin, localDir,
    localNormal)` virtual on `IGeometry` and `IObject` — "the smallest ray
    parameter this geometry accepts as a genuine hit from this point off
    this face" — defaulted to the generic floor, overridden per class
    (sphere and cylinder add the radius; the box answers its per-axis band
    divided by the incidence cosine, which is algebraically the probe's
    existing band term; meshes and patches use their bounding-box corner
    L1; Bezier its 1e-6; SDF twice its march epsilon and the torus the
    generic default — both corrected by the 7923bf2f follow-up further
    down; displaced delegates
    to its baked mesh; `Object` forwards, `CSGObject` takes the max over
    its operands mapped into each child's frame and seeds at ZERO — seeding
    at the generic default re-imported the transverse coupling and turned
    Test 14 red). The probe now takes the max of that answer (a range,
    divided by the stretch) and the box band (a plane distance, divided by
    the rate). `CsgSurfacePayloadTest` Test 27 pins it (box minus sphere at
    radius 4 and 20, red without the term), and `BoxGeometryTest` gains a
    contract case that bisects each routine's real gate and compares it to
    the class's answer — a fixed 2×/0.5× bracket was too loose (dropping
    the sphere's radius term under-states by at most 2×). Two measured
    rulings from the same round: the triangle floor now uses the largest of
    the three vertices' L1 (its comment had promised that); and the
    suggested perpendicular-only plane floor `|n·o|` was implemented and
    REJECTED — the self root a crossing shadow ray produces is
    `1e-12·|cos in|/|cos out|`, an ANGLE effect that the coordinate term
    happens to bound, so tightening the plane floor to its round-off put
    the disk and infinite-plane rows straight back to ~367× dark.

    **Review follow-up (2026-09-05, 7923bf2f) — three overrides were wrong and
    the probe could go non-finite.** A fresh review of the interface
    change probed every override against its routine: (a) `TorusGeometry`
    had kept the generic floor, but its real gate is the quartic's
    on-surface DEFLATION test (`|C₄| ≤ 1e-10·quartScale` drops the near
    root outright, so a point inside that relative band reports the far
    wall) — 133× the default at R = 4, r = 1 in the fix's own bisection
    (1.20e-9 against 9.0e-12; the review's probe, at a different point on
    the tube, read a minimum accepted standoff of 3.33e-9 at unit scale
    and 3.33e-6 at 1000×; both are recorded in the code); the override
    now returns the deflation band's own distance
    along the ray, `2·1e-10·quartScale / max(|∇F·dir|, 0.05·|∇F|)`, and
    bisects to exactly 2× headroom at unit and 1000× scale, normal and
    oblique (obliquity via a lateral offset of R/2 — a ray through the
    axis meets every wall normally, and an offset of exactly R − r is
    tangent to the inner wall). (b) `SDFGeometry` returned `2·eps`, but a
    part's scale shrinks the field by `minScale/maxScale` (a part scaled
    (0.15, 1, 1) read exactly 0.15 of the claim), so the floor is now
    divided by the minimum of that ratio over the parts (exact to 1e-5).
    (c) An unbuilt mesh or empty patch inside a nested CSG operand has the
    default ±DBL_MAX bounding box, so the bbox-corner floor was +inf, the
    probe margin inf and the probe origin NaN with no gate left to reject
    it; `Geometry::BoundingBoxRootFloor` now falls back to the generic
    floor when the box is unbuilt, and the probe returns false (graceful
    entry-payload fallback) on any non-finite floor or margin. Also from
    that review: `CSGObject::SelfHitRootFloor` no longer lets an unrelated
    sibling inflate the answer (a triangle-mesh lobe 1e4 units from the
    probed face, whose bounding-box-corner floor is a collection floor,
    had widened the window 7494×) — only operands whose surface CONTAINS
    the point contribute, tested at this round with a 2δ ray along the
    normal (δ = 1e-6·(1 + |o|₁), fired in the composite's frame since
    each operand applies its own inverse transform) — the window and the
    SDF shrink rule were both refined by the 10f3e6c2 follow-up further
    down — falling back to the max
    over both if none owns it; and the SDF window (`2·epsFrac / lipschitz`
    of the shape itself: 0.01 % at the 5e-5 scene default with uniform
    parts, ~0.07 % at the 0.15-shrink test case, scale-invariant) is
    accepted and documented — a second SDF lobe within it would be
    adopted as the same face. Pinned by torus and SDF rows in
    `BoxGeometryTest`'s floor contract and by the new
    `tests/CsgProbeFloorTest.cpp` (35 checks at this round — 49 after the
    torus decoy cases below: the empty-mesh guard, the sibling filter,
    and the combined case whose fields must stay finite).
    Housekeeping from the same review: the interface comments no longer
    claim the virtual is declared last, and adding `override` to the
    six declarations that lacked it obliged (via clang's
    inconsistent-missing-override warning) a sweep of all 105 overriding
    virtuals across those seven geometry headers.

    **Review follow-up (2026-09-05, 10f3e6c2, the three test commits before
    it, 384e3752, and 269a5ad2) — closing the composite floor's P2s, then
    384e3752's own two P1s (the shared VERTEX the transverse jitter still
    grazed, and the unbounded floor a degenerate part scale produced).** (a) `CSGObject::SelfHitRootFloor`'s
    2δ ownership window could never be met by a child whose own floor
    exceeds it (an SDF sphere of radius 4 needs 2.77e-4 against δ = 5e-6;
    a thin torus R = 1000, r = 0.05 needs 4e-3), so on a face coincident
    with a box the SDF's floor was dropped 68-million-fold — the window is
    now `max(δ, 2·floorChild)` per child (a standoff of exactly the floor
    is a float knife-edge, and the SDF step-off is inclusive); a "floor ≥ δ
    means owner" shortcut was rejected because it would readmit a sibling
    a million units away. On a shared EDGE the ownership ray runs along
    one child's face plane and misses it (reversing or lengthening the ray
    does not help, because the coplanarity is in a TRANSVERSE axis), so a
    child the ray misses is retried from eight origins displaced
    transversely by the same window — ±t₁, ±t₂ and the four diagonals
    (±t₁ ± t₂)/√2, the diagonals fired only after the four axial ones have
    missed. The axial four alone settle an EDGE but not a VERTEX, where
    two boundary planes meet and every axial displacement lands exactly on
    one of them or leaves the operand: with the same long box and the
    small one lifted to share only the corner (1,1,1), the long box's
    2.84e-8 was dropped for the small box's 4.01e-12, **7081× under**,
    while the edge control at (1,1,0) stayed charged. A diagonal lands
    strictly inside a transverse quadrant, and a vertex of a convex
    operand always has one strictly interior. 10f3e6c2 first used a
    bounding-box SHELL test there instead; **that was withdrawn in the
    follow-up commit 384e3752** — a bounding box is not a surface, so a sibling
    whose padded AABB PLANE happens to pass through the point was charged
    with its floor while its surface was 1.59 world units away (composite
    floor 6.8e7× the owning slab's, and end to end a decoy face 1e-4 past
    the real one was adopted). Tests 5 and 6 of `CsgFloorOwnershipTest`.
    That commit also gave `SDFGeometry::SelfHitRootFloor` the incidence
    divide it lacked (its band is a perpendicular distance, the interface
    answers in range: claim/gate was 0.707 at 45°, 0.500 at 60°, 0.26 at
    75°, an unbounded under-statement; 1.000 at every incidence from 0 to
    75° after, 1.003 at 84°) and made its Lipschitz ratios read
    the same 1e-9-floored scale magnitudes `RecomputePartDerived` uses.
    The header's obliquity paragraph now also says where that divide stops
    following the geometry: the 1/20 grazing clamp **bites past 87.1°**
    (cos < 0.05), so claim/gate falls back to 0.72 at 88° and 0.39 at 89° —
    a bounded under-statement at near-tangency, which is the graceful
    direction (the probe misses and takes its entry-payload fallback).
    (b) `SDFGeometry`'s shrink ratio was a
    global minimum over parts, so a remote part squashed to 0.02 over-stated
    a uniform lobe's floor 50× (a ~2.9e-2 acceptance window); it now takes
    the minimum only over parts whose own `partEval` distance at the point
    is within twice the widest floor the function can return (a lower
    bound on true distance, so exclusion is sound), falling back to the
    global minimum at a blend seam: 50.0× → 1.0001×. **A degenerate part
    scale made that same divisor unbounded** (269a5ad2): the shrink is
    `min|scale| / max|scale|` over magnitudes FLOORED at 1e-9, so a part
    authored `scale (1,1,0)` — a slip the `part` descriptor already calls
    recurring for the neighbouring `<a b c>` slot — reads 1e-9 and a unit
    sphere part claimed **5.66e4 against a healthy twin's 6.9e-5**, twenty
    thousand times the field's own size; one layer up that number is an
    ownership-ray REACH, so it charges a healthy sibling's face tens of
    units away. Three layers, each red-proved on its own test: the
    authoring surfaces (`ParsePartLines` and the `part<N>.scale` keyframe
    setter) clamp a sub-1e-6 magnitude to 1e-6 with the sign preserved and
    a WARNING naming the part, and reject a non-finite component outright
    (`SDFGeometryTest` Test 29b, 21 checks); `SDFGeometry::SelfHitRootFloor`
    caps its answer at **half its own bbox diagonal** — a floor larger than
    the object sanctions a standoff outside the field, and the cap is four
    orders above a healthy field (`CsgFloorOwnershipTest` Test 8, which
    also carries the healthy twin as its control); and
    `CSGObject::SelfHitRootFloor` caps every child's ownership window at
    **the composite's own local-frame bbox diagonal**, which bounds ANY
    geometry's floor pathology rather than just an SDF's (Test 9:
    `SUBTRACTION(box, UNION(box 8 units away, a degenerate SDF of radius
    4000))` — a subtraction is bounded by operand A, so the inner union's
    5657 is 3266× half the outer composite's whole 3.46 diagonal; uncapped
    its ownership ray reaches its own box and charges 5657 on A's face,
    **1.4e15×** A's real 4.01e-12). An unbuilt or unbounded box disables
    either cap rather than poisoning it, the same screen
    `Geometry::BoundingBoxRootFloor` uses. Nothing is clamped at
    `SDFGeometry`'s constructor — the caps are the load-bearing guard, and
    the tests need a way to build the degenerate field that proves it.
    (c) The floor
    contract's upper bracket is 4× (measured maxima: sphere / cylinder /
    plane / disk / SDF ≈ 1.00, box oblique 1.33, indexed triangle 1.91,
    torus oblique 2.02), and a torus operand's decoy window is pinned from
    BOTH sides in `tests/CsgProbeFloorTest.cpp`: a decoy 2e-8 past the
    exit face is rejected, one at 1e-9 is adopted — the documented cost of
    the deflation band's 2× stacked with the probe's 2× (torus claim
    ~1.0e-9 at that R = 1, r = 0.4 exit point, probe margin ~2.0e-9,
    acceptance window ~4.2e-9; scale-invariant), asserted rather than
    hidden. (d)
    `CsgSurfacePayloadTest` Test 28 finally exercises the composite's OWN
    child-frame recursion (a 60°-rotated, 30×-stretched inner CSG): dropping
    the child-frame scale or the normal remap fails its four money checks,
    while seeding at the generic default trips Test 14, not 28. Suites
    at this branch's HEAD (the gate lines of 10f3e6c2 and 14d9f84d say
    332 for the first because their worktrees predate Test 28; 99c0f80a,
    which adds it, says 332 → 348):
    `CsgSurfacePayloadTest` 348, `CsgProbeFloorTest` 49, the new
    `tests/CsgFloorOwnershipTest.cpp` 72 (27 at 10f3e6c2: coincident SDF +
    box face, shared box edge, two-lobe SDF at the same 4× bracket, and
    the 1e6-distant mesh lobe that must NOT become an owner; 384e3752's
    Tests 5 and 6 above add the AABB-plane sibling and the end-to-end
    decoy at both gaps, taking it to 50; 269a5ad2's Tests 7–9 — the
    shared vertex, the SDF's own cap, the composite's window cap — take it
    to 72), `SDFGeometryTest` 655 → 676 (685 after 1332df39's keyframe nan/inf and parser inf rows). Red-proofs, each layer alone:
    axial-only jitters fail Test 7's two money checks (edge control stays
    green); the SDF cap removed reports 5.66e4 and fails Test 8's two
    (Test 9 stays green, its window cap holding even at a floor of
    2.26e8); the composite window cap removed reports 5657 on operand A's
    face and fails Test 9's two (Test 8 stays green); the parser clamp
    removed fails 7 of Test 29b's checks, the keyframe clamp removed
    exactly 1.

    **Accepted residuals from the closing review of 269a5ad2** (all
    graceful entry-payload fallbacks, i.e. under-statements that can only
    make the probe miss, never adopt a wrong face; measured with probes,
    not reachable by any shipped scene — the 437-golden `CstDeriveGoldenTest`
    corpus shows 0 drift): (i) the eight jitter samples sit 45° apart, so a
    co-owner whose transverse cross-section spans LESS than 45° of arc at
    the point (an acute `INTERSECTION` of two slabs; a triangle mesh's
    acute corner is NOT affected, its exact-apex ray is accepted) is
    missed — a 43° arc read 4.01e-12 against its 2.84e-8 floor, a 49° arc
    was found; (ii) a scale component the parser clamps to 1e-6 still
    yields a floor at the SDF's 0.5·diagonal cap (1.416 on a unit field
    against a healthy 6.9e-5, 20,410×) — a warning now names the part, and
    the reach cap keeps it from charging siblings; (iii) an authored
    `epsilon` of 0.02 or more on a flattened part can push a GENUINE floor
    above the 0.5·diagonal cap at grazing exits (claim/gate 0.67 at
    epsilon 0.02, 0.13 at 0.10), which the default 5e-5 never approaches;
    (iv) a SUBTRACTION caps its subtrahend's ownership window at the
    minuend's diagonal, so a subtrahend whose own step-off band exceeds the
    minuend (an SDF blade at `epsilon` 0.02 carving a box under 0.4 units:
    8e-11 against its own 6.66) is dropped — widening the cap to B's box
    was tried and re-admits exactly the degenerate-subtrahend reach Test 9
    pins (its box is 8000 units), so the cap stays A's and the coarse-
    epsilon subtrahend, the under-stating direction, is accepted.

    **Regression test.** `tests/PrimitiveSelfHitTest.cpp` renders the
    trigger-(i) family (sphere, ellipsoid, capped cylinder, open tube
    side-on, circular disk, infinite plane, two-triangle PLY quad; box and
    clippedplane as controls) and the radius-1000 Lambertian sphere with
    its unit twin through the real path, PT vs BDPT, 45 checks in ~5 s:
    closed shells banded ±8 % (they share the box's ~3 % residual), flat
    surfaces and the Lambertian rows ±3 %, bands derived over n = 5 seed
    bases in its own comment. Red-proved against the pre-fix sources
    (`8d97645f`; the table is in the test's header): 8 checks fail — the
    seven non-control rows by an order of magnitude or worse (the test's
    own run read sphere 3.16, disk 430, plane 344, mesh 163, against the
    sweep's 3.12 / 393 / 354 / 157 above — Monte Carlo noise on ratios
    whose PT denominators are near zero) and the Lambertian 1000× row at
    1.61 — while the two controls and the unit twin stay green.

    **What this does NOT close.** The gap > 0 rows do not converge with
    this fix — filed as **debt 27** below. And a small residual remains at
    gap 0: over n = 5 seed bases the fixed box reads BDPT/PT 0.969 ± 0.003
    and VCM/PT 0.968 ± 0.002, where the same six faces as free-standing
    planes read 0.988 / 0.985 and a single plane reads 1.000. BDPT itself
    reads the box and the six planes within 0.9 % of each other, so this
    is a ~2 % PT box-vs-planes swing of unknown origin. It is NOT BDPT's
    depth cap (`max_eye_depth` / `max_light_depth` 16 and 32 read 0.969
    and 0.975 against 0.972 at 8). Left open at this level; the closed-box
    guard's 8 % band is set to hold it while still tripping by an order of
    magnitude on the pre-fix 0.284.

    **Consequence for debt 23's medium sibling.** The per-surface
    medium-vertex predicate was fixed on principle and by symmetry with its
    surface twin, but until this fix it had no image-level regression test
    — every scene that exercises it needs an `interior_medium`, hence a
    closed solid, hence the 5.8× baseline error this debt recorded. With
    debt 25 closed, the scene is now buildable and lands as
    `tests/FabricRenderTest.cpp::TestMediumVertexBehindGappedWeave`
    (keyword `mediumvertex`; light INSIDE the box so that debt 27's
    far-layer gap path cannot enter, gap 0.1, homogeneous
    `interior_medium`; BDPT/PT 0.997 ± 0.002 and VCM/PT 0.998 ± 0.002 over
    n = 5). **It is a parity guard, not a discriminating regression:** the
    red-proof flipped both medium-vertex sites in `BDPTIntegrator.cpp` back
    to the pre-fix `prev.isDelta` predicate and the ratio did not move
    beyond noise (0.994–1.000 either way, also with scattering raised to
    15 and depth lowered to 2–5), because this scene's medium
    illumination is carried by connections that are connectible under
    both predicates. The only measured effect of the medium-vertex fix
    remains the one taken when it landed: +0.28 % on BDPT at gap 0.3 on
    the pre-fix (leaking) box scene (0.0181892 → 0.0182397), in the
    predicted direction and above the ±0.1 % run-to-run spread, but far
    too small a lever to assert on. Alongside it: the closed-box
    parity guard `TestClosedBoxThinWeave` (keyword `closedbox`; PT box vs
    six planes < 1.15, BDPT/PT and VCM/PT within 8 %) and the geometry unit
    test `tests/BoxGeometryTest.cpp` (on-face origins in and out, both
    flag rules, 1000× coordinate scale, exit info; red-proved against the
    pre-fix `BoxGeometry.cpp`) — derivations in the tests' own comments.

26. **RESOLVED 2026-09-05 — not a rasterizer defect: the scene declared a
    DIRECT-LIGHTING-ONLY shader chain, so no continuation ray was ever
    cast (or even sampled). `BDPTStrategyBalanceTest`'s PT reference has
    been switched to `pathtracing_pel_rasterizer` and the area-lit
    topology added there as topology F.**

    *The measurement that opened it (2026-09-04, re-reproduced 2026-09-05
    on this tree).* 32×32, 256 spp, gapped weave (`gap 0.1`,
    `transmission thin`) at z = 0 in front of a full-width
    `lambertian_luminaire` quad (`exitance` white, `scale 2.0`) at
    z = −1.5, pinhole camera at z = 3.2, `oidn_denoise FALSE`:
    `pixelpel_rasterizer` (`max_recursion 8`, `lum_samples 1`) reads
    **0.041586**, `pathtracing_pel_rasterizer` **0.102404** — ratio
    **2.46×**. BDPT 0.102585 and VCM 0.102416 agree with the modern PT to
    1.8e-3. Raising `max_recursion` from 2 to 8 changes nothing.

    **Mechanism.** The legacy rasterizer executes the scene's
    `standard_shader` chain literally, and the scene declared
    `shaderop DefaultDirectLighting` alone. `Job::AddStandardShader`
    ([Job.cpp:8549-8555](../src/Library/Job.cpp)) auto-prepends
    `DefaultEmission` (and only that op — the comment there says why), so
    the chain that actually runs is
    `[EmissionShaderOp, DirectLightingShaderOp]`. Neither op casts a
    continuation ray, and — the load-bearing detail — neither declares
    `RequireSPF()`
    ([DirectLightingShaderOp.h:68](../src/Library/Shaders/DirectLightingShaderOp.h),
    [EmissionShaderOp.h:62](../src/Library/Shaders/EmissionShaderOp.h)),
    so `StandardShader`'s `bComputeSPF`
    ([StandardShader.cpp:27-34](../src/Library/Shaders/StandardShader.cpp))
    is false and `StandardShader::Shade`
    ([StandardShader.cpp:56-62](../src/Library/Shaders/StandardShader.cpp))
    never calls `ISPF::Scatter` at all. The weave's delta gap lobe
    ([WeaveSPF.cpp:243-266](../src/Library/Materials/WeaveSPF.cpp) —
    `ScatteredRay::eRayRefraction`, `isDelta`, `pdf 1`, `kray 1`,
    emitted with probability `gap`) is therefore never generated, let
    alone followed, and the term it carries — the eye ray continuing
    undeviated through the gap onto the emitter, where `EmissionShaderOp`
    would credit `emittedRadiance` at full weight because
    `rs.bsdfPdf == 0` — is absent from the estimator. `max_recursion` is
    inert for exactly the same reason: with no recursive op the depth
    budget is never exercised.

    **Numeric proof (add the term back).** Adding ONE op,
    `DefaultRefraction`
    ([RefractionShaderOp.cpp:47-69](../src/Library/Shaders/RefractionShaderOp.cpp);
    `RequireSPF()` true, follows every `eRayRefraction` scattered ray),
    to the same legacy rasterizer, same scene, same spp:

    | gap | legacy `[Em,DL]` | legacy `[Em,DL,Rf]` | modern PT | legacy/PT | (legacy+Rf)/PT | predicted gap sighting `g·L·cov` | measured `Rf − DL` |
    |---|---|---|---|---|---|---|---|
    | 0   | 0.046022 | 0.046018 | 0.046942 | 0.9804 | 0.9803 | 0        | −0.000004 |
    | 0.1 | 0.041586 | 0.101484 | 0.102404 | 0.4061 | 0.9910 | 0.060429 | 0.059898 |
    | 0.3 | 0.032550 | 0.212469 | 0.213237 | 0.1526 | 0.9964 | 0.181287 | 0.179919 |

    (32×32, 256 spp, mean of 3 wall-clock seeds; per-cell σ ≤ 1.7e-4.
    `L = exitance·scale/π = 2/π = 0.636620`;
    `cov = 0.949254` is the fraction of the film whose straight-through
    ray lands on the emitter quad, `((1.4·3.2/4.7)/(3.2·tan 17°))²`.)

    The law holds to better than 1 %. The **shape** of the missing term
    pins it exactly: the difference image `[Em,DL,Rf] − [Em,DL]` is a
    TOP-HAT over the emitter's silhouette whose interior height is
    `gap × L` to **0.22 %** (gap 0.1: 0.063524 vs 0.063662; gap 0.3:
    0.190483 vs 0.190986, 1024 spp) and which is flat zero elsewhere; on
    a deliberately shrunk emitter (half-extent 0.5 instead of 1.4) it
    collapses to a crisp central square of the predicted size and the
    frame mean of the difference drops from 0.05984 to 0.007662 against a
    prediction of 0.0077087. The residual ~0.8 % on the FULL-frame
    numbers, versus ~0.2 % on the interior, is the pixel filter softening
    the emitter's silhouette — which falls in the outermost pixel ring at
    this framing.

    **It is not weave-specific, and it is not a defect.** The
    discriminating control is a plain `dielectric_material` pane
    (`ior 1.5`, `scattering 1000000`) substituted for the weave in the
    identical scene: legacy `[Em,DL]` renders **0.000000** — the pane is
    black and the emitter behind it invisible — while legacy
    `[Em,DL,Rf]` reads 0.611189 against the modern PT's 0.611155
    (**1.00006**). If this were a bug, every dielectric in RISE would
    render black under a direct-lighting-only chain. It is the legacy
    shader-op contract, and the shipped legacy corpus already honours it:
    `scenes/FeatureBased/Caustics/pool_caustics.RISEscene` pairs
    `DefaultDirectLighting` with `DefaultRefraction`, `scenes/pr.RISEscene`
    with `DefaultReflection`. A `DefaultDirectLighting`-only chain is a
    *direct lighting* render by construction — no reflections, no
    refractions, no indirect.

    **What was changed, and why the reference was switched rather than
    patched.** `tests/BDPTStrategyBalanceTest.cpp`'s `kRasterizerPT` is
    now `pathtracing_pel_rasterizer` (`samples 32`, `rr_min_depth 8`,
    `pixel_filter box`, `oidn_denoise FALSE`), with the same
    `DefaultPathTracing` shader as the BDPT string so the two scenes still
    differ only in the rasterizer chunk. Adding `DefaultRefraction` to the
    legacy string would have been the smaller edit, but it does not make
    the legacy chain a transport reference: at gap 0, where no delta lobe
    exists at all, the legacy chain is still **0.980** of the modern PT
    purely from the indirect bounces it does not follow (row 1 above), and
    a strategy-balance test's reference must not have coverage that
    depends on which ops a scene string happens to list.

    Every pre-existing topology agrees with BDPT MORE tightly after the
    switch, and no band was loosened (32×32, 32 spp, mean of 3 runs):

    | topology | OLD PT mean RGB | OLD BDPT/PT | NEW PT mean RGB | NEW BDPT/PT | NEW p99 | NEW max |
    |---|---|---|---|---|---|---|
    | A delta omni | (0.02463,0.02456,0.02458) | (0.9985,1.0029,1.0003) | (0.02460,0.02460,0.02460) | (1.0000,1.0000,1.0000) | 1.000 | 1.000 |
    | B mesh area | (0.05790,0.05800,0.05792) | (1.0003,1.0004,1.0004) | (0.05795,0.05795,0.05795) | (1.0000,1.0000,1.0000) | 1.000 | 1.000 |
    | C mixed | (0.05789,0.05775,0.05788) | (1.0013,1.0050,1.0015) | (0.05785,0.05785,0.05785) | (1.0009,1.0009,1.0009) | 1.031 | 1.035 |
    | D orthographic | (0.03674,0.03670,0.03672) | (1.0003,1.0030,1.0015) | (0.03675,0.03675,0.03675) | (1.0002,1.0002,1.0002) | 0.999 | 1.000 |
    | E backlit weave (omni) | (0.03158,0.02888,0.02378) | (0.9977,1.0037,0.9990) | (0.03155,0.02895,0.02377) | (1.0000,1.0000,1.0000) | 0.999 | 1.000 |
    | **F gapped weave + area light (NEW)** | — | — | (0.10802,0.10411,0.09630) | (0.9948,0.9946,0.9941) | 1.007 | 1.009 |

    Tolerances are unchanged (`kStrictTolerances` 8 % mean / 25 % p99 /
    2× max) and the suite goes 30 → **36 checks, 0 failures**.

    **Left standing, deliberately:** `tests/VCMStrategyBalanceTest.cpp`
    still references `pixelpel_rasterizer` with a
    `DefaultDirectLighting`-only chain. Its three topologies (delta omni,
    mesh area, mixed) are single-bounce direct lighting on ONE flat
    Lambertian quad — no transmissive material, no second SCATTERING surface (B and C add a purely emissive quad with `material none`, which cannot redistribute energy), so no
    scattered ray carries energy and the legacy chain is complete for
    them (18/18 checks green, unchanged). It is the same latent hazard,
    though: any topology added there that involves a scattered ray
    reaching a light needs the reference switched first. Recorded so it
    is not rediscovered from scratch.

    One confounder was removed in the same edit, symmetrically on BOTH
    rasterizer strings: `oidn_denoise FALSE`. `CapturingRasterizerOutput`
    only overrides `OutputImage`, and the default `OutputDenoisedImage`
    forwards POST-denoise pixels there, so the suite had been comparing
    DENOISED images — and OIDN's `auto` quality selection is timing-based,
    so it flipped between BALANCED and HIGH between otherwise identical
    invocations and made topology A's PT mean bimodal run to run. This is
    the same class of finding as the 2026-08-27 `EnvLightBalanceTest`
    de-OIDN'ing.

    **Relation to debt 27: a different mechanism; one fix does NOT close
    both.** Debt 27 is a *shadow-ray* blind spot (PT's binary NEE cannot
    see through a FAR delta layer because `CastShadowRayTransmittance` is
    gated to perfect-specular dielectrics), and a pass-through-visibility
    walk keyed on a material predicate would live in `RayCaster` /
    `LightSampler` — code the legacy chain *already* executes, since
    `DirectLightingShaderOp::PerformOperation` and
    `PathTracingIntegrator::PTEvaluateDirectLighting` both call
    `LightSampler::EvaluateDirectLighting`. Such a walk would change the
    NEE term identically in both integrators and would restore none of
    debt 26's missing 0.060: that term is a CAMERA-side sighting, not a
    shadow-ray term, and the proof is that adding `DefaultRefraction` —
    which touches no visibility code — closes debt 26 outright.
    Structurally, debt 26's scene has ONE weave layer, so debt 27's
    mechanism cannot even be present in it; consistently, PT, BDPT and VCM
    agree here to ≤ 0.2 % at every gap. Debt 27 therefore stays OPEN,
    untouched by this round.

    **Sibling sweep, same day:** the OIDN-denoised-capture trap this round
    fixed in `BDPTStrategyBalanceTest` was not unique to it — six more
    suites (`VCMStrategyBalanceTest`, `IORStackSeedingRegressionTest`,
    `RayCasterVolumeAbsorptionTest`, `VolumeAbsorptionAttenuationTest`,
    `DeferredRealizeTest`, `PhotonMapDeferralTest`) plus one stray
    rasterizer string in `CSGNullGeometryLuminaireCrashTest` gained
    `oidn_denoise FALSE`; every suite stayed green across 3 reruns with no
    band loosened — see [bdpt-vcm-mis-balance.md's "Sibling sweep"
    addendum](skills/bdpt-vcm-mis-balance.md) for the full accounting.

27. **OPEN 2026-09-05 — PT cannot sample the far layer's delta gap: a
    two-layer gapped weave reads PT UNDER BDPT/VCM by 1.28–1.30× at gap
    0.1 (two planes / box) and 1.55× at gap 0.3 with the light outside;
    exact with the light inside or a single layer.**

    Exposed by debt 25's fix — masked before it by the closed-box leak that
    debt 25 filed. Measured (same setup as debt 25's after-table):

    | scene | PT | BDPT/PT | VCM/PT |
    |---|---|---|---|
    | box gap 0.3, light outside, before | 0.03188 | 0.991 | 0.994 |
    | box gap 0.3, light outside, after | 0.02043 | 1.548 | 1.554 |
    | six planes gap 0.3, light outside | 0.02046 | 1.553 | 1.554 |
    | box gap 0.1, light outside, after | 0.01725 | 1.296 | 1.291 |
    | six planes gap 0.1, light outside | 0.01734 | 1.288 | 1.283 |
    | two planes gap 0.1, light outside | 0.01544 | 1.277 | 1.273 |
    | single plane gap 0.3 | 0.03361 | 1.000 | 1.000 |
    | box gap 0.3, light INSIDE, after | 0.69122 | 1.000 | 0.998 |

    **This table's "two planes" row supersedes debt 25's "2 parallel weave
    planes" row of the same name** (0.98855 there vs 1.277 here, at a
    different gap and a different `PT` denominator). The two free-standing
    z = ±0.5 planes measured here at gap 0.1 do not reproduce debt 25's
    `0.98855`, and its exact geometry cannot be recovered, so treat that
    earlier row as superseded by this measurement rather than as having
    been wrong.

    **Mechanism.** The path light → straight through the far layer's delta
    gap lobe → near layer's continuum lobe → camera is reachable by light
    tracing (t=1 splat / s=2 connection) but not by PT's binary NEE: the
    shadow ray cast from the near layer toward the light is blocked by the
    far layer, and `CastShadowRayTransmittance`
    (`RayCaster.cpp` ~1980) — the one path that lets a shadow ray see
    through a delta-transmissive occluder — is opt-in and gated to
    perfect-specular dielectrics, not weave gaps. A single layer, or a light
    already inside the box, has no "far layer" left to block the shadow
    ray, so PT is exact in both of those cases; any two-layer topology with
    the light outside always has one.

    **Direction of a fix** (not a plan). Extend the transparent-shadow walk
    to any material with a non-bending delta pass-through lobe (a weave
    gap: transmittance = `gap`, no Fresnel), which for DELTA lights needs no
    MIS (no competing strategy) but for AREA lights would double-count
    against PT's BSDF-sampled delta continuation that hits the emitter
    (the "delta-gap-to-emitter sighting" debt 26 is named after) unless one
    side is suppressed — the same caveat the existing dielectric shortcut
    carries.

    **Not the same mechanism as debt 26, and one fix does not close both**
    (assessed 2026-09-05 when debt 26 was resolved; nothing here changed).
    Debt 26 was a shader-op-composition issue in the legacy chain: the
    delta lobe was never even SAMPLED, because no op in the chain declared
    `RequireSPF()`. The pass-through-visibility walk sketched above lives
    in `RayCaster` / `LightSampler` — code the legacy chain already runs,
    since `DirectLightingShaderOp::PerformOperation` and
    `PathTracingIntegrator::PTEvaluateDirectLighting` both call
    `LightSampler::EvaluateDirectLighting` — so it would move the NEE term
    identically in both integrators and restore none of debt 26's missing
    energy, which is a CAMERA-side sighting rather than a shadow-ray term.
    Debt 26's scene has a SINGLE weave layer, so this debt's mechanism is
    structurally absent from it (PT, BDPT and VCM agree there to ≤ 0.2 % at
    every gap). This debt therefore remains independently open and still
    needs its own material-predicate design (a `HasNonBendingDeltaLobe()`
    query on the material, not a per-sample flag — the debt-23 lesson) plus
    the AREA-light double-count suppression above.

    **Auto-router.** `docs/RENDERING_INTEGRATORS.md` §2 /
    `docs/AUTO_RASTERIZER_DESIGN.md` currently have no rule for this
    two-layer-gap regime, and PT is the default.
---

## 16. Non-goals

- **No cloth simulation or dynamics.** Drape is a modelling problem; this
  document is about appearance.
- **No micro-CT or measured-BRDF capture pipeline.** Khungurn 2015's fitting
  framework is cited as the reason presets exist, not as something to build.
- **No new painter pipe.** §5.4. A vector/tangent pipe is deferred and
  observed-need gated at fourteen enumerated touchpoints.
- **No LTC infrastructure in Phase 1.** §9.4. Gated on a measured cue-(c)
  deficit, not declined.
- **No microflake / SGGX / volumetric fabric tier.** §7(D). Rejected on
  infrastructure, not on merit; SGGX-as-a-phase-function remains the right first
  brick if RISE ever wants one.
- **No pattern raster in Phase 1.** §9.5. Phase 1 has no `weave` enum at all;
  the name is reserved for Phase 2's structured model, and the descriptor must
  say so rather than leave its absence looking like an oversight.
- **No anisotropic sheen lobe.** §9.5, debt 5. Charlie's normaliser, its Λ
  masking fit and the `E` table are all isotropic-only; weave directionality is
  delivered by rotating the frame handed to an anisotropic *substrate*, exactly
  as glTF, Filament and OpenPBR all do.
- **No D-importance sampling of the sheen lobe in Phase 1.** §9.4. There is no
  VNDF for Charlie, the naive sampler leaks below the horizon, and the
  rejection-corrected density fails `SPFPdfConsistencyTest` Parts 2 and 3.
  Deferred behind a sampler/pdf pair proven against those tests.
- **No SMS involvement.** No fabric lobe is delta.
- **No BDPT/VCM fabric-specific strategies.** Fabric rides the existing
  vertex-eval machinery.
- **No `auto_rasterizer` per-material routing tag.** §12. It would encode a
  decision the dispatcher already makes.
- **No silhouette fuzz (cue d) in Phases 1 or 2.** It is a geometry problem, it
  is gated behind a missing primitive, and this document says so rather than
  implying a BSDF term supplies it.
- **No real-time or preview fabric path.**
- **No emissive fabric.** `CanBeAreaLight() = false`.

---

## 17. References

### Papers

- **Ashikhmin, Premože & Shirley**, *A Microfacet-Based BRDF Generator*,
  SIGGRAPH 2000, pp. 65-74. [verified — citation; velvet's ~40° filament-bundle
  tilt verified; **the closed-form normalisation constant of the
  inverted-Gaussian distribution is [from memory] and must be checked against the
  primary PDF before any implementation**.] *Naming trap:* the "Ashikhmin" in
  Blender's historical Velvet BSDF is this paper, **not** the Ashikhmin-Shirley
  anisotropic Phong BRDF RISE already implements.
- **Zhao, Jakob, Marschner & Bala**, *Building Volumetric Appearance Models of
  Fabric Using Micro CT Imaging*, SIGGRAPH 2011. [verified]
- **Irawan & Marschner**, *Specular Reflection from Woven Cloth*, ACM TOG 31(1),
  Article 11, February 2012, DOI 10.1145/2077341.2077352. [verified; parameter
  list counted from the Mitsuba 0.5/0.6 `irawan` plugin source, verified. The
  underlying Cornell PhD thesis, *The Appearance of Woven Cloth*, is [from
  memory] as to exact title and year.]
- **Neubelt & Pettineo**, *Crafting a Next-Gen Material Pipeline for The Order:
  1886*, SIGGRAPH 2013 Physically Based Shading course notes. [verified —
  citation; the visibility formula is [from memory] of the widely-reproduced
  form, cross-checked against the Khronos glTF sheen spec text.]
- **Sadeghi, Bisker, De Deken & Jensen**, *A Practical Microcylinder Appearance
  Model for Cloth Rendering*, ACM TOG 32(2), Article 14, 2013,
  DOI 10.1145/2451236.2451240. [verified — **this corrects the author list the
  research brief carried** ("Bisceglio, Joshi, Bloom" appear on no such paper).
  The follow-up SIGGRAPH Asia 2013 talk *Importance Sampling for a Microcylinder
  Based Cloth BSDF* is verified as existing, not read. The validated-fabric
  roster is **medium confidence**.]
- **Heitz, Dupuy, Crassin & Dachsbacher**, *The SGGX Microflake Distribution*,
  ACM TOG 34(4), Article 48, 2015. [verified — **this corrects a spurious
  "Iwasaki" co-author** in the research brief. The projected-area quadratic form
  `σ(ω) = sqrt(ωᵀ S ω)` is [from memory] of the widely-reproduced result.]
- **Khungurn, Schroeder, Zhao, Bala & Marschner**, *Matching Real Fabrics with
  Micro-Appearance Models*, ACM TOG 35(1), Article 1, 2015. [verified]
- **Estevez & Kulla**, *Production Friendly Microfacet Sheen BRDF*, SIGGRAPH 2017
  Physically Based Shading course notes. [verified — citation; the Charlie NDF
  and the `sheen_albedo_scaling` composition verified **verbatim** against the
  Khronos glTF `KHR_materials_sheen` spec text. **The claim that the Charlie CDF
  is closed-form invertible is [from memory]** and should be checked before
  implementing an analytic inverse.]
- **Montazeri, Gammelmark, Zhao & Jensen**, *A Practical Ply-Based Appearance
  Model of Woven Fabrics*, ACM TOG 39(6), Article 251, SIGGRAPH Asia 2020.
  [verified; the 2021 knit follow-up, arXiv 2105.02475, verified as existing.]
- **Wang, Jin, Hašan & Yan**, *SpongeCake: A Layered Microflake **Surface**
  Appearance Model*, ACM TOG 41(6), 2022 (SIGGRAPH Asia 2022), arXiv 2110.07145.
  [verified — **this corrects both the title** (the research brief said "Volume")
  **and the author list**. **No production adoption is corroborated**; the
  brief's suggestion of Adobe Substance 3D or Unity was explicitly not confirmed
  and is not claimed here.]
- **Zeltner, Burley & Chiang**, *Practical Multiple-Scattering Sheen Using
  Linearly Transformed Cosines*, ACM SIGGRAPH 2022 Talks,
  DOI 10.1145/3532836.3536240; reference implementation at
  github.com/tizian/ltc-sheen. [verified. Blender 4.0's adoption is [from
  memory], medium confidence.]
- **Jin, Wang & Yan**, *Woven Fabric Capture from a Single Photo*, SIGGRAPH Asia
  2022. [verified — citation and the two-term single/multiple-scatter
  description; **author list medium confidence**.] Follow-ons *Woven Fabric
  Capture with a Reflection-Transmission Photo Pair* (SIGGRAPH 2024) and
  *Fiber-level Woven Fabric Capture from a Single Photo* (arXiv 2409.06368)
  verified as to title/venue, not read.
- **Zhu, Jarabo, Aliaga, Yan & Chiang**, *A Realistic Surface-based Cloth
  Rendering Model*, SIGGRAPH 2023 Conference Proceedings. [verified — citation,
  the four target signatures, and the comparison-to-Irawan claims. **The
  equation set, the itemised parameter table, and the sampling strategy were not
  obtained** — this is Phase 2's first gate.] Follow-on *A Realistic Multi-scale
  Surface-based Cloth Appearance Model* (SIGGRAPH 2024) and, in the same lineage,
  Khattar et al., *A Texture-Free Practical Model for Realistic Surface-Based
  Rendering of Woven Fabrics* (CGF 2025), verified as to title/venue, not read.

**Explicitly omitted rather than fabricated.** No "Tang et al. 2024/2025"
real-time yarn-level woven-fabric paper was surfaced by any search this session;
per the cite-only-if-verified rule it is omitted.

### Specifications and production implementations

- **Khronos glTF 2.0 `KHR_materials_sheen`** — Charlie D, a choice of Neubelt or
  full-Λ visibility, and the `sheen_albedo_scaling` composition. [verified,
  fetched verbatim.]
- **Filament** (Google) — Charlie D + Neubelt V, an energy-conservative
  Lambertian, a documented non-physical subsurface term, `sheenColor`, and a
  fixed non-tunable `w = 0.5` wrap diffuse; credits Burley and Neubelt for the
  robust-blending property. [verified — Filament's own material docs.]
- **Unreal Engine Cloth Shading Model** — a `Cloth` mask plus a `Fuzz Color` on
  top of the standard model. [verified — Epic's own docs.]
- **Enterprise PBR** (Dassault Systèmes) — a Charlie-class sheen term and the
  `E(θ)` energy-compensation LUT the glTF spec cites. [verified via the glTF
  spec's own citation.]
- **OpenPBR** — the "fuzz" slab, moved to the top of the stack specifically so it
  can sit over both base and coat. [verified — OpenPBR spec text.]
- **Mitsuba 0.5/0.6 `irawan` plugin** — the canonical open Irawan-Marschner
  implementation and the source of §3.1's parameter list. [verified via source.]

### In-tree

- [MATERIALS.md](MATERIALS.md) §9 — the new-BSDF checklist this plan instantiates.
- [WETNESS_COAT_DESIGN.md](WETNESS_COAT_DESIGN.md) — `coated_material`'s
  architecture, the verb-first ordering, and the layered-material contract this
  design's Phase 1 mirrors.
- [HAIR_FUR_DESIGN.md](HAIR_FUR_DESIGN.md) — the model-survey/geometry-survey/
  phased-plan shape, the fibre-tangent plumbing, the OIDN measurement, the
  re-derive-don't-vendor precedent, and the observed-need decline rule.
- [GEOMETRY_SHADING_SIGNALS_DESIGN.md](GEOMETRY_SHADING_SIGNALS_DESIGN.md) —
  `curv` / `occlusion` / `thickness` and the `add_wear` verb.
- [ISCALARPAINTER_REFACTOR.md](ISCALARPAINTER_REFACTOR.md) — the two-pipe model
  and the cost precedent for adding a third.
- [SPECTRAL_ILLUMINANT_CONVENTION.md](SPECTRAL_ILLUMINANT_CONVENTION.md) §7.1 —
  `GetColorNM` vs `GetRadianceNM` and the `GuardedGetColorNM` rationale.
- [UNIFIED_INTEGRATOR_DECISION.md](UNIFIED_INTEGRATOR_DECISION.md) +
  [RENDERING_INTEGRATORS.md](RENDERING_INTEGRATORS.md) §2 — PT-default routing.
- [OIDN.md](OIDN.md) — the AOV contract behind §12's denoiser rules.
- [GEOMETRY_DERIVATIVES.md](GEOMETRY_DERIVATIVES.md) — the `dpdu`/`dpdv` contract
  §9.1's fix builds on.
- [GLTF_IMPORT.md](GLTF_IMPORT.md) §15 — the sheen skip this design unblocks.
- [BLENDER_MATERIAL_TRANSLATION.md](BLENDER_MATERIAL_TRANSLATION.md) — debt 13.
- `docs/agentic-redesign/88-procedural-texture-expressiveness-candidates.md` §2,
  §4, §7 — C-ADV / C-TYPE / C-VERB / C-READ / C-TEXT / C-MEAS and the master
  adoption triad.
- [skills/implementation-review-loop.md](skills/implementation-review-loop.md),
  [skills/variance-measurement.md](skills/variance-measurement.md),
  [skills/write-highly-effective-tests.md](skills/write-highly-effective-tests.md)
  — the process gates §9.9 invokes.
