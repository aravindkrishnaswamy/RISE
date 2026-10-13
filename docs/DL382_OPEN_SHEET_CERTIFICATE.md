# DL-382: certifying open sheets, and the unpushed-exit rule

Original slice `jetski-dl382`, 2026-10-12, based on `master` `0131003bc`. Fix-forward is based on local master `72d9fce57`; the verified core merge `2c08b50c0` is retained.
Background: [DL345_OPEN_SHEET_FACE_RULE.md](DL345_OPEN_SHEET_FACE_RULE.md)
(the face rule and why it is the only reciprocal convention on an open
transmissive sheet).  This is the FRESH design the owner asked for after the
2026-10-10 pause; it replaces "certify one more special shape" with one
general certificate, and replaces the single-object containment probe of an
unpushed exit with a rule that knows what a slab is.

## 1. What has to be proven

The face rule (front = surrounding medium, back = the sheet's material) is
applied to a hit only when `RayIntersectionGeometric::bProvablyNoInterior`
is set.  Two things must hold for that to be the right rule:

- **no enclosed volume** -- otherwise the surface is (part of) a closed
  solid, whose stack rule is winding-independent and must stay
  bit-identical;
- **a readable face** -- the true geometric normal must be the authored
  winding, consistently across the surface, so "front" means one thing.

"Has boundary edges" proves neither (a closed sphere plus a separate disk in
one mesh has boundary edges; a self-intersecting open mesh can pinch off a
pocket).  The certificate below proves both.

## 2. The mesh certificate (`GeometricUtilities::IsEmbeddedOpenOrientedSurface`)

Input: the triangles as authored (both mesh classes, so `indexedmesh`,
`rawmesh`/`rawmesh2`, imported PLY/OBJ/glTF/3DS meshes and every
`displaced_geometry`'s realized mesh).  Corners are welded with the DL-143
rule (`eps = max(1e-9, 1e-6 x bbox diagonal)`, the same
`WeldVertexPositions`, moved to `GeometricUtilities` unchanged).  Refuse
(certify nothing) unless ALL of:

1. **Non-degenerate**: no triangle repeats a welded vertex, and every
   triangle's smallest altitude exceeds `eps` (so every distance test below
   is meaningful at the weld tolerance).
2. **Manifold and consistently oriented**: every welded edge is used by one
   or two triangles; a shared edge is traversed in OPPOSITE directions by
   its two triangles.  (Two triangles on the same three welded vertices are
   refused too.)
3. **No closed 2-cycle (free-face peeling)**: repeatedly delete any
   triangle that has an edge used by no other remaining triangle.  Certify
   only if every triangle is deleted.  Proof: the support of a non-zero
   Z2 2-cycle uses each of its edges an even number (>= 2) of times inside
   the support, so its first triangle can never become deletable; hence
   "all deleted" => `H_2(S; Z2) = 0`.  `H_2(S; Z)` of a 2-complex is free and
   injects into `H_2(S; Z2)` (universal coefficients), so `H_2(S; Z) = 0`.
   A closed mesh has no free edge and is refused at once (cheap); a closed
   component plus a flap is refused when peeling stalls.
4. **Embedded**: the triangles, as positioned, meet only in the faces the
   weld says they share, with margin `eps`:
   - no shared welded vertex: triangle-triangle distance `> eps`;
   - exactly one shared vertex `v`: each triangle's edge OPPOSITE `v` is
     `> eps` from the other triangle (two convex sets that share `v` meet
     elsewhere iff the far end of their intersection lies on one of those
     opposite edges -- proof in the code comment);
   - a shared edge `ab` with third vertices `c`, `d`: `c` is `> eps` from
     the other triangle, `d` likewise, and the crossing edge pairs
     `(ac, bd)`, `(bc, ad)` are `> eps` apart (refuses a fold onto itself;
     a non-coplanar pair meets only on its shared edge).
   Candidate pairs come from a uniform grid over eps-inflated boxes.
5. **Face readable**: the mesh's authored normals agree with the winding
   (the existing `!HasUncertainSMSNormalOrientation` audit).

Why 3 + 4 => no enclosed volume: for a compact polyhedron `S` embedded in
`R^3` (one-point compactified to `S^3`), Alexander duality gives
`H~_0(S^3 \ S) = H^2(S)`, and `H^2(S) = Hom(H_2(S), Z) + Ext(H_1(S), Z)`
with `H_1(S)` free for a subcomplex of `S^3`.  So `H_2(S) = 0` => the
complement is connected => no bounded complementary component, i.e. no
enclosed volume, whatever the surface's shape, topology (holes, handles,
curvature). The implementation additionally requires one edge-connected component; disconnected pieces are refused by the general certificate.

The existing planar certificate (`IsPlanarConsistentlyWoundSheet`) is kept
as an OR, so every mesh certified today (including degenerate planar ones
such as two coincident copies of a sheet, which 4 refuses) stays certified
and bit-identical.  Closed meshes are refused by 3 and stay bit-identical.

**Consequences that are intended:** an open-top box, a bent quad, a curved lens surface, a terrain, a
displaced non-planar / dart / non-convex clipped plane (its realized mesh)
are now certified when they pass 1-5 and the single-component check. A slab authored as one mesh of disconnected sheets is refused; use separate sheet objects.  "N sheets can bound a volume"
(DL-157 round 3) is answered by 4: N embedded sheets that bound a volume
must touch along the volume's edges, which is a refusal; if they leave a
gap the "volume" is connected to the outside and the face rule is the
reciprocal rule for it (DL-345 measured six open sheets around a box
matching the closed box to 4 digits).

**Cost**: the certificate runs in `ComputeAreas` at build/deserialization and after changed-position updates. Unchanged positions reuse the cached shape proof; authored-normal agreement is refreshed on every update. Height-only deformation of a certified embedded planar projection also reuses the shape proof: exact projected weld aliases, unchanged projected positions, and equality of coincident heights are checked before mutation. Openness and embedding follow from remaining a graph over that fixed projection. Planarity and signed volume are refreshed in linear passes. An exactly coincident triangle-corner pair supplies a cached necessary refusal for the general certificate while that equality remains; the legacy planar proof is still recomputed. Other deformation without a reusable proof must revalidate embedding; topology alone is insufficient (DL-620). The projection audit is performed at build time for qualifying non-planar indexed meshes, and may increase build cost.  Steps 1-3 are linear (hash maps); step 4
runs only for meshes that pass 1-3 (open, oriented) and costs one grid
pass whose candidate count depends on triangle sizes and spatial overlap; there is no general linear-time guarantee.  Measured in section 8.

### 2.1 The inside-out crack risk

A NEARLY closed glass solid with cracks (fails DL-143) passes 1-5 when
consistently wound.  Wound OUTWARD, the face rule gives exactly the closed
solid's indices (front = air, back = glass) -- fine.  Wound INWARD it would
invert (the outside reads as glass).  There is no sound refusal for this:
a cracked inward-wound shell and an intentionally inward-facing open bowl
are the same topology, and any "how closed is it" threshold is a fudge
that would refuse legitimate sheets.  Decision: the transport certificate
uses topology plus embedding and normal agreement; the `ObjectManager` preparation pass WARNS (once
per scene) for a transmissive mesh certified by the general certificate
whose signed volume about its bounding-box centre is negative ("if this is
meant to be a solid it is wound inside-out").  This is a diagnostic only
and never changes transport.  Shipped corpus: see section 7.

## 3. Other producers

| Geometry | Decision | Why |
|---|---|---|
| `TriangleMeshGeometry{,Indexed}` | general certificate OR planar | section 2 |
| `DisplacedGeometry` | its realized `TriangleMeshGeometryIndexed` stamps the flag itself (section 2); the coplanar-convex-base stamp is kept as is | non-planar / dart / non-convex bases are covered whenever the realized mesh is embedded; a fold is refused by 4 |
| `ClippedPlaneGeometry` | unchanged (every hit) | one bilinear patch |
| `BezierPatchGeometry`, `BilinearPatchGeometry` sets | **PINNED, warning stays** | the surface intersected is the ANALYTIC patch set, not a tessellation; a proof needs exact boundary-curve matching between patches, an embedding test for curved patches (control-net hulls of neighbours always overlap) and consistent parametric orientation, which the shipped data does not even have (the teapot file mixes CW and CCW patches).  Authoring route: wrap the patch set in `displaced_geometry` with `disp_scale 0`; its realized mesh is then certified by section 2 when it qualifies. |
| `CSGObject` of sheets | **PINNED, warning stays** | CSG of an open operand is undefined (inside/outside of an open operand does not exist), so there is no "open CSG" to certify; `CSGObject` keeps clearing the flag |

## 4. Part (2): the unpushed exit

An unpushed exit is a back-face hit on a certified sheet `S` that is not on
the walk's stack (the walk reached `S`'s material side without crossing `S`).
The crossing refracts FROM `S`'s index; the question is the FRONT-side
medium, i.e. which stack entries still contain the region the walk enters.
The old rule asked ONE object (the top `Y`) with ONE ray: "is the first hit
on `Y` ahead an exit?".  It fails two ways:

- **T6**: `Y` is an open sheet (the slab's bottom) and the walk is still
  inside the slab: `Y` is behind the walk, the probe misses, `Y` is popped.
- **T5c**: `Y` is a closed solid wound inward: its exit reads as an entry.

New rule (`IORStackSeeding::ResolveUnpushedExit`, shared by
`ResolveOpenSheetCrossing` and the SMS seed walk): pop the top entry `Y`
while it does NOT contain the front region, stop at the first entry that
does (or at the root):

- `Y` entered through an OPEN sheet's face (the stack entry is marked at
  push time, `IORStack::pushOpenSheet`): `Y`'s medium continues past the
  crossing iff the first surface ahead **made of `Y`'s material**
  (same `IMaterial`; `S` excluded) is hit from its BACK (an exit of that
  medium).  Slab membership is a medium relation: a slab is the region
  between sheets of one material, so a lone sheet of another material
  inside it is never its sibling (T6), while the slab's own far sheet, or
  nothing at all, ends it (equal / unequal / near-equal slabs: unchanged).
  The probe needs the scene; the stack carries it
  (`IORStack::SetMediumProbeScene`, set by `SeedFromPoint` and the SMS seed
  walk, copied with the stack).  Without a scene it falls back to the old
  probe of `Y` alone.
- `Y` a closed object: containment by the PARITY of `Y`'s own crossings
  along the probe (odd = inside), which does not read the winding (T5c);
  for an outward-wound closed `Y` it equals the old first-hit answer.

Popping more than one entry is new: it is what makes the REVERSE of T6
reciprocal (walking down, the walk enters the slab's top sheet and the lone
sheet, then leaves the slab through its bottom sheet: both the lone sheet's
entry and the slab's entry must go).

**Reciprocity.**  At every crossing of a certified sheet the BACK-side
index is the sheet's own (face rule), so the forward and reverse walks
agree on the pair iff they agree on the FRONT-side medium.  An entering
walk reads it from its history (the stack top); an exiting walk computes it
with the rule above.  In a well-nested configuration -- every region's
medium is claimed consistently (a slab's sheets share one material, closed
solids nest, lone sheets sit inside a medium that encloses them) -- both
equal the region's medium, so each pair is the reverse of the other.  Worked
cases (gated in `ExteriorIndexInvarianceTest` A7, including the former A7-KF cases; A8 tests Newton continuity):

| case | forward pairs | reverse pairs |
|---|---|---|
| T6: slab 2.2 (bottom down-facing, top up-facing), lone 1.5 sheet inside, walk up | bot [1 -> 2.2], lone [1.5 -> 2.2], top [2.2 -> 1] | top [1 -> 2.2], lone [2.2 -> 1.5], bot [2.2 -> 1] |
| T5c: inward-wound closed water mesh 1.33 (pushed), lone 2.2 sheet inside, walk up | lone [2.2 -> 1.33], cube [1.33 -> 1] | cube [1 -> 1.33], lone [1.33 -> 2.2] |
| A7 equal / unequal / near-equal slab, in air or a 1.33 box, lone sheets | unchanged | unchanged |

Residual (pinned): configurations whose regions are claimed inconsistently
(two DIFFERENT material objects forming one slab with a lone sheet inside
it; overlapping half-spaces of different materials) have no
history-independent medium and stay order-dependent.

## 5. Remaining (pinned)

- Bezier / bilinear patch sets and CSG of sheets (section 3): warning only.
- Meshes that fail the certificate (closed components, self-intersections,
  non-manifold edges, inconsistent orientation, near-degenerate slivers):
  warning only, stack rule.
- Item (4): a refractor inside a lone sheet's half-space sees the walk's
  stack, not the half-space (unchanged; needs half-space seeding).
- Item (3) composite: frozen, untouched.  Note: `composite_material` on a
  newly certified curved mesh retains its pre-merge record. Only the old planar/displaced-quad proof reaches frozen consumers.

## 6. Tests

- `MeshInteriorSignalTest`: certificate accept/refuse matrix (curved sheet,
  open-top box, bent quad accepted; disconnected two-component mesh, closed cube, sphere
  plus disk, flattened box, X-crossing sheets, inconsistently wound flap, folded
  sheet, non-manifold fan, mixed winding, sliver refused), UpdateVertices
  re-decides.
- `CurvedOpenSheetSlabTest` (maintained reviewer fixture) curved rows: PT vs BDPT vs VCM on
  two-sheet slabs of gently curved open meshes (separate objects), n = 8 salted; red on the base, green after.
- `ExteriorIndexInvarianceTest`: A7 T6 and its reverse walk, plus T5c, promoted to gates.

## 7. Shipped scenes

Shipped scenes corpus audit:
- The shipped corpus includes closed solids, clipped/displaced slabs, and pre-existing planar mesh sheets. Five dielectric indexed meshes in `gltf_import_attenuation.RISEscene` already certified on the base.
- The 157-input audit produces the same 375 records before and after the fix-forward; it establishes unchanged certification across this corpus. Render gates independently cover closed solids and clipped-plane scenes.

## 8. Historical measurements

The following figures were recorded by `jetski-dl382`; current fix-forward
gate counts and fresh curved-fixture measurements are in section 9. The old
closure review claim is withdrawn.

`OpenSheetIndexConventionTest` (mesh rows, salted $n = 8$, Sobol' seeded):
- **Single open sheet** (up-wound) PT / clipped plane PT: $0.14502 / 0.14515 = 0.9991 \pm 0.0031$
- **Single open sheet** (up-wound) PT+SMS / PT: $1.0003 \pm 0.0022$
- **Single open sheet** (up-wound) BDPT / PT: $1.0018 \pm 0.0045$
- **Single open sheet** (up-wound) VCM / PT: $1.0025 \pm 0.0039$
- **Single open sheet** (down-wound) PT / clipped plane PT: $0.14511 / 0.14522 = 0.9992 \pm 0.0029$
- **Single open sheet** (down-wound) PT+SMS / PT: $1.0001 \pm 0.0018$
- **Single open sheet** (down-wound) BDPT / PT: $1.0021 \pm 0.0041$
- **Single open sheet** (down-wound) VCM / PT: $1.0028 \pm 0.0038$
- **Two-mesh open slab / closed box** (camera below, direct):
  - PT: $0.14483 / 0.14710 = 0.9846 \pm 0.0042$
  - PT+SMS: $0.14496 / 0.14685 = 0.9871 \pm 0.0006$
  - BDPT: $0.14523 / 0.14715 = 0.9870 \pm 0.0065$
  - VCM: $0.14536 / 0.14639 = 0.9930 \pm 0.0055$
  - Parity across integrators: all within $0.37\%$ of each other (PT $0.14483$, PT+SMS $0.14496$, BDPT $0.14523$, VCM $0.14536$).
- **Two-mesh open slab / closed box** (camera above, through slab):
  - PT: $0.23169 / 0.23374 = 0.9912 \pm 0.0032$
  - PT+SMS: $0.23207 / 0.23395 = 0.9919 \pm 0.0015$
  - BDPT: $0.23187 / 0.23387 = 0.9914 \pm 0.0061$
  - VCM: $0.23215 / 0.23408 = 0.9917 \pm 0.0067$
  - Parity across integrators: all within $0.20\%$ of each other (PT $0.23169$, PT+SMS $0.23207$, BDPT $0.23187$, VCM $0.23215$).
- Prior baseline before DL-382: RED ($7$ passed / $9$ failed; PT was $0.44$ of clipped plane; slab ratios were $0.59$–$0.63$; integrators disagreed by up to $3\times$).
- After DL-382: GREEN ($44$ of $44$ full suite checks pass, $20$ of $20$ mesh rows pass).


## 9. Fix-forward (2026-10-12)

The `2c08b50c0` merge is retained: the curved-sheet slab core is verified.
DL-382 is reopened because the previous zero-P1 review claim was false.
Random-walk scene rejection now uses `IsZeroVolumeSheet`, the old planar
proof, rather than the general face-rule proof. A cracked solid remains
accepted. The broader hit flag is admitted at `Object::IntersectRay` only
for dielectric/perfect-refractor scattering materials, including luminaire wrappers around them; all other materials (including single-layer composites under wrappers) retain
their old planar/displaced-quad proof. This preserves stackless translucent
and frozen composite consumers without editing their transport code.

Negative open signed volume now produces a once-per-object-set ObjectManager
warning, including displaced meshes. Certification stays available because
intentional inward bowls and cracked inward solids cannot be distinguished
by topology. The warning names the inverted-index risk; it does not promise
winding invariance for uncertified cracked solids. Analytic patch sets, CSG,
lone-sheet half-space membership, the pre-existing nested SMS bias, and
arbitrary-deformation certificate cost are actual rows DL-616..DL-620.

Validation commands for this slice (run from the repository root):

```sh
# Build the named tests and the two diagnostics; make discovers test sources.
make -C build/make/rise -j8 all build-test/MeshInteriorSignalTest \
  build-test/ExteriorIndexInvarianceTest build-test/TransmissionPushGateTest \
  build-test/SMSDomainReplayTest build-test/CompositeEnergyConservationTest \
  build-test/OpenSheetIndexConventionTest build-test/OpenSheetWarningTest \
  build-test/BSSRDFOpenSheetEntryTest build-test/RandomWalkOpenSheetRejectionTest \
  build-test/CurvedOpenSheetSlabTest build-test/OpenSheetConsumerScopeTest \
  build-test/OpenSheetLegacyRenderTest build-tool/OpenSheetCertificateAudit \
  build-tool/OpenSheetCertificateTiming
python3 scripts/validate_dl382.py --log-dir /tmp/dl382-gates \
  --baseline-root /tmp/rise-dl382-base-0131003bc \
  --before-root /tmp/rise-dl382-head-72d9fce57
```

The optional baseline checkout supplies the frozen-composite pixel hash;
the pre-fix master checkout supplies runtime red proofs for the two P1s.
`CurvedOpenSheetSlabTest` is a real ratio gate (5% band, n=8), with the
reviewer's separate curved sheets and closed slab retained as authored.
Its baseline is 19 pass / 9 fail. `OpenSheetConsumerScopeTest` checks both
faces of the holed translucent/composite mesh and the hair skip. The
certification audit compares all 157 reviewer inputs against the committed
375-record snapshot, including six unchanged failure records.

The cache avoids full geometric revalidation for unchanged positions,
height changes over an already certified exact embedded projection, and
general refusals with retained exactly coincident triangle corners.
Projection construction is a one-time extra build cost; a formerly planar
mesh first becoming curved also pays that cost on its first deformation.
Other deformation without a reusable proof retains full revalidation (DL-620). The total update can
still rebuild its BVH when SAH degradation warrants it, as before.

### Runtime red proofs and consumer controls

| Finding | Red control | Fix-forward control |
|---|---|---|
| Random-walk holed sphere | pre-fix master: 10 pass / 2 fail, load refused, render -1 | 12 pass / 0 fail; mean 0.90687, equal to the 0131003bc control |
| Inward glass warning | pre-fix master: 11 pass / 2 fail; no negative-volume warning | 13 pass / 0 fail; named warning appears once, repeat preparation stays quiet |
| Curved two-sheet slab optics | 0131003bc: 19 pass / 9 fail | 28 pass / 0 fail; separate sheets vs closed slab and integrator ratios gated |
| Translucent/composite consumer scope; hair skip | reviewed 0c5500401: 60 pass / 8 fail on single-layer and wrapped composites | 86 pass / 0 fail; material identity after luminaire unwrapping, both faces checked on the holed sphere |
| Frozen composite images | 0131003bc: two-layer, top-only, bottom-only, wrapped top-only; aggregate hash 18203558810393780052 | same four exact images gated against the baseline aggregate; 6 pass / 0 fail |

The warning resolves the missing diagnostic, not the intentionally accepted
winding dependence of cracked glass. Intentional inward-facing sheets retain
the face rule; an author who intended a solid is told to correct winding or
close the mesh. Analytic patches, CSG, lone-sheet half-space membership,
nested-sheet SMS pricing, and non-graph deformation cost remain separate
open rows DL-616 through DL-620. No frozen transport implementation was edited.

Fresh curved open/closed slab ratios (n=8, camera below / above): PT
**0.9921 / 0.9953**, PT+SMS **0.9929 / 0.9954**, BDPT **0.9900 / 0.9947**,
VCM **0.9965 / 0.9946**. The matching baseline camera-above ratios are
**0.5878 / 0.5873 / 0.5867 / 0.5866**. These fresh numbers preserve the
reviewer's verified core while giving the actual gated fixture evidence.

### Final gate and cost record

All nine requested gates passed on the corrected implementation: MeshInteriorSignalTest **144/0**, ExteriorIndexInvarianceTest **303/0**, TransmissionPushGateTest **416/0**, SMSDomainReplayTest **10285/0**, CompositeEnergyConservationTest `--sidedness-only` **296/0**, OpenSheetIndexConventionTest **44/0**, OpenSheetWarningTest **53/0**, BSSRDFOpenSheetEntryTest **73/0**, RandomWalkOpenSheetRejectionTest **12/0**. Added gates: curved slab **28/0**, consumer scope **86/0**, exact frozen image **6/0**. Certification audit: **375 identical records from 157 inputs**.

Clean make and Xcode Deployment builds were warning-free; Opto also built warning-free. Only the expected Xcode AppIntents metadata notice was emitted. ASan/UBSan over the indexed mesh and Object implementations passed MeshInteriorSignalTest **144/0**.

Costs below are wall-time medians of three interleaved runs, 980,000 triangles (978,600 for the true closed sphere), `render_thread_reserve_count 0`, no concurrent renders or builds. “Before” is pre-fix master `72d9fce57`; “base” is `0131003bc`. Timing tool output reports openness after the final deformation.

| Work | Base ms | Before ms | Fix-forward ms |
|---|---:|---:|---:|
| Curved mesh, identical-position update | 26.1 | 880.8 | 22.4 |
| Curved graph, height deformation | 26.5 | 903.0 | 32.0 |
| Closed-ish sphere, identical-position update | 29.3 | 222.6 | 26.4 |
| Closed-ish sphere, deformation with retained coincident-corner witness | 29.6 | 207.8 | 31.1 |
| Curved mesh build, including projection cache | 487.6 | 1359.3 | 2515.1 |
| Planar mesh first becoming curved (one-time cache and closed-proof cost) | 26.2 | 797.9 | 2204.6 |
| Truly closed sphere, identical-position update | 29.5 | 286.5 | 25.7 |
| Truly closed sphere, deformation including closed reproof | 30.1 | 271.4 | 481.6 |

The truly closed control removes the polar degenerate triangles from the
reviewer's sphere fixture and retains `IsCertifiedWatertight()==true` after
motion on all three revisions. Its latest deformation cost includes the
necessary full position-weld and folded-face audit; the previous revisions
retained the build-time closed certificate without rechecking it. This
correctness cost is deliberately disclosed as DL-620, alongside the remaining
non-graph and witnessless motion cost. Projection-cache startup remains an
explicit tradeoff.


### Independent review correction

The first fresh review of `0c5500401` found one P1: scoping by SPF admitted a frozen composite that forwards its sole scattering layer. The scope now checks dielectric/perfect-refractor material identity after unwrapping luminaires; every composite keeps the legacy proof, including single-layer composites under wrappers. Clear glass under luminaires retains the verified face rule. Both single-layer arrangements and nested wrappers have hit-record gates, and four composite render variants compare exact pixels against `0131003bc`. The expanded scope gate was red **60/8** on the reviewed commit.

Review also found a P2: the graph-cache signed-volume centre included unused storage vertices. Its bounds now use referenced triangle corners, matching full certification. An inward bowl with an unused outlier was red **130/1** in MeshInteriorSignalTest before this correction. The shipped-scene description now includes the pre-existing planar dielectric meshes in the attenuation scene. The cost table above was remeasured after these corrections.

A direct-material-only intermediate filter also excluded plain clear glass under luminaires. Added controls were red **82/4** on that filter; classification now follows luminaire base materials without unwrapping composites. No luminaire or composite implementation changed.

The closed-ish timing fixture contains an exactly coincident corner pair in a triangle. The cache now stores that necessary failure witness and reuses only the general refusal while those corners remain exactly coincident and positions stay finite. The legacy planar proof is recomputed on every changed frame: it intentionally ignores degenerate triangles. Splitting the pair invalidates the witness and reruns certification. The initial new cache test was red **132/2**; the expanded mesh gate also checks flattening into and leaving the legacy planar case. Refusals without a reusable exact witness and valid non-graph deformations still need full revalidation (DL-620).

An isolated control using `0c5500401`'s Object.cpp and the current geometry produces frozen aggregate hash **16954627485471606144**, failing the paired baseline check **5/1**; the fixed four-image control gives **18203558810393780052**, **6/0**. This control isolates the reviewed material-scope regression.


The second fresh review of `8a9289edc` found **zero P1s** across correctness,
cache/cost, and evidence lenses. Its remaining warning P2 is corrected: warning
classification now uses the same material identity and luminaire unwrapping as
`Object::IntersectRay`. Single-layer composites retain the uncertainty warning;
they cannot receive the negative-volume message describing a face rule they do
not use. Indexed, raw, and displaced classifiers follow that admission. The new
warning coverage was red **41/12** on `8a9289edc` and is green **53/0**; it checks both windings,
both single-layer arrangements, nested wrappers, native wrapped glass, and
once-only preparation.


The third fresh review of `62900ec62` blocked convergence with two P1s:
`UpdateVertices` retained a stale closed-volume certificate when position-weld
seams separated, and planar-only comments contradicted the broader proof.
Changed-position updates now re-prove watertightness, including its folded-face
discriminator, unless a retained embedded open graph or exact coincident-corner
witness already proves there is no closed body. Identical positions keep the
closed certificate. The per-face-slot cube mutation control was red **141/3** and is green **144/0**;
it separates and restores a seam, checks signed-distance refusal and recovery,
and folds and restores the cube. No signed-distance or contact implementation
was changed. The displaced, indexed, raw-mesh, and test comments now describe
their actual proofs. Valid closed-body deformation may pay a full position-weld
and folded-face audit; this conservative cost is included in DL-620.
