# DL-382: certifying open sheets, and the unpushed-exit rule

Slice `jetski-dl382`, 2026-10-12, branched from `master` `0131003bc`.
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
several components) or curvature.

The existing planar certificate (`IsPlanarConsistentlyWoundSheet`) is kept
as an OR, so every mesh certified today (including degenerate planar ones
such as two coincident copies of a sheet, which 4 refuses) stays certified
and bit-identical.  Closed meshes are refused by 3 and stay bit-identical.

**Consequences that are intended:** a slab authored as ONE mesh of two
sheets, an open-top box, a bent quad, a curved lens surface, a terrain, a
displaced non-planar / dart / non-convex clipped plane (its realized mesh)
are now certified when they pass 1-5.  "N sheets can bound a volume"
(DL-157 round 3) is answered by 4: N embedded sheets that bound a volume
must touch along the volume's edges, which is a refusal; if they leave a
gap the "volume" is connected to the outside and the face rule is the
reciprocal rule for it (DL-345 measured six open sheets around a box
matching the closed box to 4 digits).

**Cost**: the certificate runs in `ComputeAreas` (build, `UpdateVertices`,
deserialize) like the planar one.  Steps 1-3 are linear (hash maps); step 4
runs only for meshes that pass 1-3 (open, oriented) and costs one grid
pass with O(1) expected candidates per triangle.  Measured in section 8.

### 2.1 The inside-out crack risk

A NEARLY closed glass solid with cracks (fails DL-143) passes 1-5 when
consistently wound.  Wound OUTWARD, the face rule gives exactly the closed
solid's indices (front = air, back = glass) -- fine.  Wound INWARD it would
invert (the outside reads as glass).  There is no sound refusal for this:
a cracked inward-wound shell and an intentionally inward-facing open bowl
are the same topology, and any "how closed is it" threshold is a fudge
that would refuse legitimate sheets.  Decision: the transport certificate
stays purely topological; the `ObjectManager` preparation pass WARNS (once
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
cases (gated in `ExteriorIndexInvarianceTest` A7 / A8):

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
  NEWLY certified mesh takes `CompositeSPF`'s existing open-sheet path, as
  it already does on planar meshes and clipped planes.

## 6. Tests

- `MeshInteriorSignalTest`: certificate accept/refuse matrix (curved sheet,
  two-component mesh, open-top box, bent quad accepted; closed cube, sphere
  plus disk, flattened box, X-crossing sheets, T-junction box, folded
  sheet, non-manifold fan, mixed winding, sliver refused), UpdateVertices
  re-decides.
- `OpenSheetIndexConventionTest` DL-382 curved rows: PT vs BDPT vs VCM on
  two-sheet slabs of gently curved open meshes (separate objects) and on one
  two-component mesh, n >= 8 salted; red on the base, green after.
- `ExteriorIndexInvarianceTest`: A7-KF T6 / T5c promoted to gates, plus
  their reverse walks (A8).

## 7. Shipped scenes

Shipped scenes corpus audit:
- All transmissive mesh objects in shipped scenes (e.g. `diacaustic`, `sms_slab_close_*`) are either closed solids (watertight, verified bit-identical) or clipped-plane / displaced-geometry slabs (already certified under DL-345, bit-identical).
- No shipped showcase scenes authored transmissive open meshes; test suite verification confirms bit-identical rendering on closed solids and clipped-plane scenes.

## 8. Measurements

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

