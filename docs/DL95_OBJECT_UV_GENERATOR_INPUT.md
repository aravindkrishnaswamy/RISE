# DL-95: `Object::IntersectRay`'s override UV generator read `ptIntersection` before it was written

Status: **CLOSED** 2026-09-14 (slice `debt-dl95`, branched from `master` `12027967`).

## The bug, in one sentence

`Object::IntersectRay` called `pUVGenerator->GenerateUV(ri.geometric.ptIntersection, ...)`
roughly 380 lines before `ri.geometric.ptIntersection` is written for the
current hit -- the write is the general-path stamp near the end of the
function (`ri.geometric.ptIntersection = Point3Ops::Transform(m_mxFinalTrans, ri.geometric.ptObjIntersec)`),
which runs well after the UV generator has already returned.

## Why it was invisible for years

An overriding `IUVGenerator` (`box_uv` / `cylindrical_uv` / `spherical_uv`
in the construction API; there is no scene-language chunk that reaches
`Object::SetUVGenerator` -- see "Scene-level impact" below) is a rare
feature to begin with, and most of the geometries that get it in
practice are analytic primitives that stamp an OBJECT-space
`ri.ptIntersection` themselves, inside their own `IntersectRay`, before
returning to `Object::IntersectRay`:

| Geometry | Stamps `ptIntersection` itself? | Stamps `ptObjIntersec` itself? |
|---|---|---|
| `SphereGeometry` | yes (`ri.ptIntersection = ray.PointAtLength(range)`) | no |
| `BoxGeometry` | yes | no |
| `TorusGeometry` | yes | no |
| `CircularDiskGeometry` | yes | no |
| `ClippedPlaneGeometry` | yes | no |
| `BilinearPatchGeometry` | yes | no |
| `EllipsoidGeometry` | yes | no |
| `CylinderGeometry` | yes | no |
| `BezierPatchGeometry` | yes | no |
| `SDFGeometry` | yes | no |
| `HairGeometry` | yes (comment: "object space; Object::IntersectRay recomputes + transforms") | no |
| `HairGenerator` (bake helper) | yes | yes |
| `DisplacedGeometry` | yes | yes |
| `GeometryUtilities`'s bake-time synthetic hit (mesh displacement bake, not a live ray) | yes | yes |
| `TriangleMeshGeometry` | **no** | **no** |
| `TriangleMeshGeometryIndexed` | **no** | **no** |
| `InfinitePlaneGeometry` | **no** | **no** (review addendum: its hit point is a local used only for its own planar `ptCoord`; the DL-95 fix is geometry-agnostic so it is covered, but it was NOT in the original table) |

For every geometry in the first block, the pre-fix code accidentally
worked: at the moment the UV-generator call ran, `ri.geometric.ptIntersection`
already held that geometry's own just-computed object-space hit point
(nothing between the geometry's `IntersectRay` call and the UV-generator
call touches `ptIntersection` unless `ray.hasDifferentials` is set, and
even then the footprint block only touches `txFootprint`, not
`ptIntersection`). `TriangleMeshGeometry{,Indexed}` and (review
addendum, 2026-09-14) `InfinitePlaneGeometry` -- which reconstruct their
hit position generically, later, from `ray.PointAtLength(range)` in
`Object::IntersectRay`'s own general path -- exposed the bug: the
UV-generator call read whatever stale point already happened to sit in
the shared `RayIntersection` record.

## Frame decision

The UV-generator block runs in **OBJECT space**. Two independent facts
pin this down, and both point the same way:

1. Every analytic geometry above stamps `ptIntersection` in its own
   `IntersectRay`, which executes entirely in object space (the ray
   handed to it has already been transformed into local coordinates by
   `Object::IntersectRay`'s own entry code, and is only transformed back
   after the UV-generator call runs).
2. `BoxUVGenerator`'s constructor arguments (`width`, `height`, `depth`)
   are the object's own local dimensions -- a `box_uv 2 2 2` on an object
   that is placed with `scale 10` in the scene graph must still chart
   the *unscaled* 2x2x2 box, not a 20x20x20 world-space one.

The fix therefore computes the canonical object-space intersection
point, `ptObjIntersec`, **before** the UV-generator call, using exactly
the expression the pre-existing general-path stamp already used a few
hundred lines further down:

```cpp
ri.geometric.ptObjIntersec = ri.geometric.ray.PointAtLength( ri.geometric.range - SURFACE_INTERSEC_ERROR );
```

At this point in the function `ri.geometric.ray` is still the
object-space (transformed) ray, and `ri.geometric.range` is still the
object-space hit distance -- neither is touched by anything between the
geometry's `IntersectRay` returning and this new statement (verified by
grepping the whole span for `range\s*=` and `geometric\.ray\s*=`; the
only other writes to either are the world-space recomputes several
hundred lines later, after the UV-generator call). The general-path
stamp further down recomputes the identical expression a second time --
harmless, since the ray and range are unchanged in between -- so no
other consumer of `ptObjIntersec` or `ptIntersection` sees any
difference.

### Byte-identical or not, for the analytic primitives?

Not quite byte-identical, but agrees far below any UV generator's
output resolution. The self-stamp uses the raw range
(`ray.PointAtLength(range)`); the new expression backs off by
`SURFACE_INTERSEC_ERROR` (1e-12 by default) along the ray, exactly as
the pre-existing general-path stamp always has. That is an object-space
position difference on the order of 1e-12 units -- `tests/UVGeneratorObjectSpaceInputTest.cpp`
pins the two agreeing to 1e-9, which comfortably contains that back-off
with eight orders of magnitude to spare.

## CSG operands

`CSGObject::IntersectRay` **never calls `pUVGenerator->GenerateUV` at
all** -- `grep -n GenerateUV src/Library/Objects/CSGObject.cpp` returns
nothing. `CSGObject` inherits the `pUVGenerator` member from `Object`
(so `SetUVGenerator` can be called on a `csg_object` without erroring),
but the composite-level hit path never consults it. An *operand*
object's own UV generator, if it has one, does still fire -- inside
that operand's own `Object::IntersectRay`, in that operand's own object
space -- and the resulting `ptCoord` survives upward through
`AdoptCsgSurfacePayload`, which copies the winning child's UV verbatim.
So a UV generator bound to a CSG *operand* is unaffected by DL-95 and
unaffected by this fix (it was already being fed the operand's own
`ptIntersection`/`ptObjIntersec` correctly, or incorrectly per the
DL-95 pattern if the operand is itself a mesh -- and this fix reaches
that case too, since the operand's `Object::IntersectRay` is the exact
function this row fixes). A UV generator bound to the *composite*
`csg_object` itself is simply dead code, at every commit before and
after this fix -- a distinct bug pattern (a missing call, not a
stale-read ordering defect), out of scope here, opened as **DL-107**.

## Sibling audit (audit-by-bug-pattern)

**Bug pattern, one sentence:** a field is read by code that assumes it
already holds the value for the CURRENT operation, but it is actually
still holding whatever value was left over from a PREVIOUS operation
sharing the same mutable record.

Sites checked:

1. **Every other read of `ri.geometric.ptIntersection` inside
   `Object::IntersectRay`.** `grep -n 'ptIntersection\b' src/Library/Objects/Object.cpp`
   (post-fix) shows exactly one early read (the one fixed here, now
   reading `ptObjIntersec` instead) and the write/immediate-consumer
   pair at the general-path stamp, both of which run *after* the write.
   No other early read exists in this function. **Refuted** (no sibling
   here).
2. **`Object::IntersectRay_IntersectionOnly`.** Boolean shadow-ray test
   only; touches neither `ptIntersection`/`ptObjIntersec` nor
   `pUVGenerator`. **Not applicable.**
3. **`Object::UniformRandomPoint`.** Calls `pGeometry->UniformRandomPoint(point, normal, coord, prand)`
   directly and transforms the returned point/normal to world space --
   it never calls `pUVGenerator` at all, so an override UV generator has
   no effect on area-light / uniform-sampling UV. This is the SAME
   "missing invocation" pattern as the CSG finding above (not the
   DL-95 stale-read pattern), so it is not fixed here either. Opened as
   **DL-108**.
4. **The modifier stack / relief pipeline.** `Object::IntersectRay` only
   *stores* `ri.pModifier = pModifier` for a caller to invoke later; it
   does not itself read or apply the modifier, so there is no
   write-order hazard inside this function for that path.
5. **`vNormal` / `vGeomNormal`.** Both are written by the geometry's own
   `IntersectRay` call (line ~987 in the pre-fix numbering) *before* the
   footprint block, the UV-generator block, and the normal-transform
   block all run -- unlike `ptIntersection`/`ptObjIntersec`, no geometry
   leaves these fields unwritten, so there is no analogous ordering
   hazard for them in this function. (The DL-70 row already covers a
   completely different bug about these fields -- reading the reported,
   possibly ray-flipped, orientation without recovering the true one --
   which is a value-correctness issue, not a write-ordering one.)

## DL-107 and DL-108 (2026-09-17, slice `debt-uvgen2`): the sibling "missing call" pattern, CLOSED

Status: **CLOSED**, branched from `master` `a4495f94`.

Bug pattern, one sentence: `CSGObject::IntersectRay` and
`Object::UniformRandomPoint` each build a `ptCoord` for their hit
without ever calling `pUVGenerator->GenerateUV` at all -- a MISSING
call site, the DIFFERENT pattern from DL-95's stale-read ordering
defect that this same sibling audit (above) first named when it opened
both rows.

### DL-107 fix (`CSGObject::IntersectRay`)

The composite-level generator now fires at the very top of the
post-processing `if( ri.geometric.bHit )` block (line ~1403 pre-fix
numbering) -- the exact mirror of `Object::IntersectRay`'s own DL-95
fix site. This is the LAST point in the function where the frame is
still right for it: `ri.geometric.ray` / `range` are still the CSG's
own LOCAL (composite object-space) values (the world-space promotion
is the very next block), and `vGeomNormal` has not yet been through
THIS level's own `m_mxInvTranspose` transform either, so
`UnflippedGeomNormal()` still answers in that same local frame. The
composite's own object-space point is computed with the identical
expression the general-path stamp uses a few hundred lines further
down for `ptIntersection`'s world value:
`ray.PointAtLength(range - SURFACE_INTERSEC_ERROR)` -- but this is a
**DIFFERENT quantity from `ri.geometric.ptObjIntersec`**, which (per
the CSG-operand section above) holds the CHILD OPERAND's own
object-space point, copied verbatim by `AdoptCsgSurfacePayload`. A
composite-bound generator must chart the COMPOSITE's own local frame
(the frame its own construction-time dimensions, e.g. `BoxUVGenerator`'s
`width`/`height`/`depth`, are authored in), so the fix computes this
value into a local, does not touch `ptObjIntersec`, and hands the local
to `GenerateUV`.

**Precedence** (operand generator wins; the composite's is the
fallback for an operand with none of its own): a new
`RayIntersectionGeometric::bUVGeneratorApplied` bool records whether
ANY `GenerateUV` call has already supplied `ptCoord` for the reported
surface. `Object::IntersectRay` sets it in BOTH branches of its own
`if( pUVGenerator )` block (true when it fires, explicitly false when
it doesn't -- never left at a stale prior value, the same discipline
DL-95 established for every other field that block writes).
`CSGObject`'s composite check reads it (`if( pUVGenerator &&
!ri.geometric.bUVGeneratorApplied )`) and sets it after firing. The
flag composes through arbitrary CSG nesting for free, the same way
`ptObjIntersec` and `ptCoord` themselves already do: a whole-record
`ri = riObjA` / `= riObjB` copy carries it via the added constructor /
copy-constructor / `operator=` members, and a boundary-reattribution
branch carries it via the added line in `AdoptCsgSurfacePayload`
(same "per-surface identity" category as `ptCoord` and
`bGeomNormalOrientedToRay` there).

### DL-108 fix (`Object::UniformRandomPoint`)

Now calls the override generator (when bound) on the OBJECT-space
point/normal `pGeometry->UniformRandomPoint` just produced, BEFORE
`Object::UniformRandomPoint`'s own pre-existing code transforms either
to world space -- the exact frame contract DL-95 established. Since a
caller may pass a null `point`/`normal` (only wanting `coord`), the fix
obtains local `Point3`/`Vector3` temporaries in that case so the
generator always has an object-space point/normal to read, matching
what a ray hit on the same surface point would feed it via
`IntersectRay`.

`CSGObject` has no `UniformRandomPoint` override of its own -- it
inherits `Object`'s null-geometry-guard fallback (a CSGObject's
`pGeometry` is always null; see `Object::GetArea()`'s doc comment) --
so there is nothing to fix at the CSG level for this row; a `CSGObject`
was never, and still is not, uniformly area-samplable at all.

### Sibling audit (audit-by-bug-pattern)

- **Every `IObjectPriv` implementation**: `grep -rln 'public virtual
  Object\b\|public virtual IObjectPriv' src/Library/Objects/` finds
  exactly `Object.h` and `CSGObject.h` -- no `InstancedObject` or other
  third implementation exists in this codebase. Both are now covered.
- **`Object::CopySnapshotStateInto`** (shared by `Object::CloneSnapshot`
  and `CSGObject::CloneSnapshot`) already propagates `pUVGenerator` via
  `SetUVGenerator` on clone -- pre-existing, correct, unaffected by
  either fix.
- **Agent `query_object_at`** (`AgentSession::QueryObjectAt`) reuses the
  render's `objectmap` identity path (per-pixel OBJECT ID, decoded from
  a legend), never `ptCoord`/UV at all -- not a sibling of this pattern.
- **Scene-editing reachability**: unlike DL-95's own scene-level-impact
  finding below (no ASCII CHUNK reaches `SetUVGenerator`), the ASCII
  **command** surface does: `AsciiCommandParser.cpp`'s
  `ParseModifyObject_UV_Box` / `_Spherical` / `_Cylindrical` (the
  `modify <object> uv ...` command) call `IJob::SetObjectUVToBox` et
  al., which resolve ANY object by name via
  `ObjectManager::GetItem` -- including a `csg_object` -- and call
  `SetUVGenerator` on whatever comes back. Both fixes are therefore
  reachable from the scene-EDITING surface today, not just the raw
  construction API this doc's own "Scene-level impact" section (below)
  found for DL-95.
- Sweep/skeleton/hair/displaced geometries carry no bug of their own
  here: they are all `IGeometry` implementations plugged into a plain
  `Object`, so they go through the SAME `Object::IntersectRay` /
  `Object::UniformRandomPoint` pipeline this fix already covers -- there
  is no separate per-geometry-type UV-generator call site to audit.

### Red-proof and gate

`tests/UVGeneratorObjectSpaceInputTest.cpp` sub-tests 4-6 (DL-108, DL-107,
DL-107 precedence) and `tests/EmitterUVSampleTest.cpp`'s new
`TestUVGeneratorLightSamplingTopology` (DL-108, render-level): see
tests/README.md for the full numbers. Summary: 47/7 -> 54/0
(UVGeneratorObjectSpaceInputTest); 13/2 -> 15/0 (EmitterUVSampleTest,
ratio 0.4983/0.4984 -> 1.0000/1.0000 for PT/BDPT). Full gate (all green,
per-test builds, no `make tests`): `GeometryUVRoundtripTest`,
`CsgSurfacePayloadTest` 348/0, `ProceduralMeshTest` 440/0,
`SceneGraphParentTest` 291/0, `ViewportRenderModeTest` 530/0,
`AgentViewModeRenderTest` 687/0, `BDPTStrategyBalanceTest` 66/0,
`VCMStrategyBalanceTest` 55/0, `CstDeriveGoldenTest` 452 MATCH/0 DRIFT,
`SourceHygieneTest` 165/0. Clean rebuild: zero warnings.

## Scene-level impact

`grep -rln 'SetUVGenerator' src/Library/Parsers/ src/Library/Cst/` returns
nothing: no `.RISEscene` chunk currently exposes an overriding UV
generator at all. It is construction-API-only
(`RISE_API`/`IJob`/`Object::SetUVGenerator`) -- **and, per the DL-107/108
section above, the ASCII `modify` COMMAND surface** (a separate thing
from a scene-file CHUNK). Consistent with the chunk finding,
`grep -rl 'UVGenerator\|box_uv\|cylindrical_uv\|spherical_uv' scenes/`
finds nothing, and `CstDeriveGoldenTest` (452 golden scenes) shows
`452 MATCH, 0 DRIFT` before and after this fix -- there is no shipped
scene this change can visibly affect.

## Red-proof

`tests/UVGeneratorObjectSpaceInputTest.cpp`, committed red in `fc971a55`
against parent `12027967` (Object.cpp unmodified):

```
Sub-test 1: two different points on the same mesh face chart to different UVs
  FAIL: MONEY: two different hit points chart to different UVs
  FAIL: point A charts to u=0.65  (got 0.5, want 0.65 +/- 1e-09)
  FAIL: point A charts to v=0.45  (got 0.5, want 0.45 +/- 1e-09)
  FAIL: point B charts to u=0.35  (got 0.5, want 0.35 +/- 1e-09)
  FAIL: point B charts to v=0.30  (got 0.5, want 0.3 +/- 1e-09)
Sub-test 2: a mesh's UV chart does not follow a PREVIOUS object's leftover hit point
  FAIL: MONEY: mesh charts to its OWN hit point's u, not the previous object's  (got 1.5, want 0.65 +/- 1e-09)
  FAIL: MONEY: mesh charts to its OWN hit point's v, not the previous object's  (got -1, want 0.45 +/- 1e-09)
Sub-test 3: analytic primitives' UV-generator input is unaffected by the fix (consistency pin)
  (all green -- these geometries self-stamp before the UV-generator call runs)

Passed: 28  Failed: 7
```

Green after the fix (`146329c2`): **35 passed / 0 failed.**

Sub-test 1's got-0.5-for-both matches the ledger row's own measured
pre-fix evidence exactly (`BoxUVGenerator`'s output for
`ptIntersection == (0, 0, 0)`); sub-test 2's `(1.5, -1.0)` is the
independently-derivable value of the SAME generator fed the *previous*
object's leftover `(2, 3, 50)` world-space hit point, which is clearly
outside `[0, 1]` and clearly not the mesh's own `(0.65, 0.45)`.

## Gate

All green post-fix, per-test builds (no `make tests`):

| Test | Result |
|---|---|
| `UVGeneratorObjectSpaceInputTest` (new) | 35 / 0 |
| `GeomNormalOrientationSitesTest` | 72 / 0 |
| `GeometryUVRoundtripTest` | all passed (coverage + roundtrip, incl. bilinear-patch) |
| `ProceduralMeshTest` | 440 / 0 |
| `CsgSurfacePayloadTest` | 348 / 0 |
| `SceneGraphParentTest` | 291 / 0 |
| `DisplacedGeometryTest` | all passed |
| `ViewportRenderModeTest` | 530 / 0 |
| `SourceHygieneTest` | 165 / 0 (341 files scanned) |
| `CstDeriveGoldenTest` | 452 MATCH / 0 DRIFT (459 corpus scenes, 0 uncovered, 0 stale) |

Clean rebuild (`make -C build/make/rise -j8 all`): zero warnings, exit 0.

## New debts opened (at DL-95 time) -- both since CLOSED

- ~~**DL-107**: `CSGObject::IntersectRay` never calls
  `pUVGenerator->GenerateUV` at all, so an overriding UV generator bound
  directly to a `csg_object` composite is silently never invoked.~~
  **CLOSED 2026-09-17** -- see "DL-107 and DL-108" section above.
- ~~**DL-108**: `Object::UniformRandomPoint` samples `pGeometry` directly
  and never consults an override UV generator either, so an
  area-light/uniform-sampling UV on an object carrying a UV generator
  ignores it.~~ **CLOSED 2026-09-17** -- see "DL-107 and DL-108" section
  above.

Both shared a bug pattern with each other (a missing call site, not a
stale read) but a DIFFERENT pattern from DL-95 itself (an ordering
defect), so neither was fixed in the original DL-95 slice -- both are
now fixed, in slice `debt-uvgen2`.
